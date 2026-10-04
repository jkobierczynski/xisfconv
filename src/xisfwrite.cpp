// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "xisfwrite.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <set>

#include "codecs.hpp"
#include "xisf.hpp"

namespace xisfconv {

namespace {

constexpr uint64_t kAlignment = 4096;

std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:
                if (c == '\t' || c == '\n' || c == '\r') out += "&#" + std::to_string(c) + ";";
                else if (c < 0x20 || c == 0x7F) out += ' ';   // not representable in XML 1.0
                else if (c >= 0x80) out += '?';                // FITS headers are ASCII
                else out += static_cast<char>(c);
        }
    }
    return out;
}

// XISF image ids must be identifiers: [A-Za-z_][A-Za-z0-9_]*
std::string makeIdentifier(const std::string& text, size_t index, std::set<std::string>& used) {
    std::string id;
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') id += static_cast<char>(c);
        else if (!id.empty() && id.back() != '_') id += '_';
    }
    while (!id.empty() && id.back() == '_') id.pop_back();
    if (id.empty()) id = "image" + std::string(index ? std::to_string(index) : "");
    if (id[0] >= '0' && id[0] <= '9') id = "_" + id;
    std::string unique = id;
    for (int n = 2; used.count(unique); ++n) unique = id + "_" + std::to_string(n);
    used.insert(unique);
    return unique;
}

std::string propertyXml(const XisfOutProperty& p) {
    std::string x = "<Property id=\"" + xmlEscape(p.id) + "\" type=\"" + p.type + "\"";
    if (p.type == "String") return x + ">" + xmlEscape(p.value) + "</Property>\n";
    if (p.type == "F64Vector" || p.type == "F64Matrix") {
        std::vector<uint8_t> bytes(p.data.size() * 8);
        if (!p.data.empty()) std::memcpy(bytes.data(), p.data.data(), bytes.size());
        if (!hostIsLittleEndian()) byteSwapInPlace(bytes.data(), p.data.size(), 8);
        if (p.type == "F64Vector") x += " length=\"" + std::to_string(p.data.size()) + "\"";
        else x += " rows=\"" + std::to_string(p.rows) + "\" columns=\"" + std::to_string(p.columns) + "\"";
        return x + " location=\"inline:base64\">" + base64Encode(bytes.data(), bytes.size()) + "</Property>\n";
    }
    return x + " value=\"" + xmlEscape(p.value) + "\"/>\n";
}

// The bytes stored for one image plus the attributes that describe them.
struct Block {
    const uint8_t* data = nullptr;  // points into `owned` or into the pixel buffer
    uint64_t size = 0;
    std::vector<uint8_t> owned;
    std::string attributes;         // compression / subblocks / checksum, each with a leading space
};

std::vector<uint8_t> compressChunk(const std::string& codec, const uint8_t* src, size_t size) {
    return codec == "zstd" ? zstdCompress(src, size) : zlibCompress(src, size);
}

void prepareBlock(const PixelBuffer& px, const XisfWriteOptions& opt, Block& block) {
    const size_t rawSize = px.data.size();
    const size_t itemSize = sampleBytes(px.format);
    block.data = px.data.data();
    block.size = rawSize;

    if (!opt.codec.empty() && rawSize > 0) {
        const bool shuffle = opt.shuffle && itemSize > 1;
        std::vector<uint8_t> shuffledData;
        const uint8_t* src = px.data.data();
        if (shuffle) {
            shuffledData = shuffled(px.data.data(), rawSize, itemSize);
            src = shuffledData.data();
        }
        const uint64_t chunk = std::max<uint64_t>(1, opt.subblockSize);
        std::vector<uint8_t> stored;
        std::string subblocks;
        size_t chunks = 0;
        for (uint64_t off = 0; off < rawSize; off += chunk, ++chunks) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk, rawSize - off));
            const std::vector<uint8_t> c = compressChunk(opt.codec, src + off, n);
            stored.insert(stored.end(), c.begin(), c.end());
            if (!subblocks.empty()) subblocks += ':';
            subblocks += std::to_string(c.size()) + "," + std::to_string(n);
        }
        if (stored.size() < rawSize) {  // otherwise compression doesn't pay off: store raw
            block.owned.swap(stored);
            block.data = block.owned.data();
            block.size = block.owned.size();
            block.attributes += " compression=\"" + opt.codec + (shuffle ? "+sh" : "") + ":" + std::to_string(rawSize) +
                                (shuffle ? ":" + std::to_string(itemSize) : "") + "\"";
            if (chunks > 1) block.attributes += " subblocks=\"" + subblocks + "\"";
        }
    }
    if (!opt.checksum.empty()) {
        // The checksum covers the block as stored (i.e. the compressed bytes).
        const size_t n = static_cast<size_t>(block.size);
        std::string digest;
        if (!xisfDigest(opt.checksum, block.data, n, digest)) throw Error("unsupported checksum algorithm '" + opt.checksum + "'");
        block.attributes += " checksum=\"" + opt.checksum + ":" + digest + "\"";
    }
}

uint64_t alignUp(uint64_t v) { return (v + kAlignment - 1) / kAlignment * kAlignment; }

}  // namespace

void writeXisf(const std::string& path, const std::vector<XisfOutImage>& images, const XisfWriteOptions& opt) {
    if (images.empty()) throw Error("no images to write");
    warnIfChecksumUnknownToPixInsight(opt.checksum);
    if (!opt.codec.empty() && opt.codec != "zlib" && opt.codec != "zstd") {
        throw Error("unsupported XISF compression codec '" + opt.codec + "' (use zlib or zstd)");
    }

    std::vector<Block> blocks(images.size());
    std::vector<std::string> ids;
    std::set<std::string> usedIds;
    for (size_t i = 0; i < images.size(); ++i) {
        prepareBlock(*images[i].pixels, opt, blocks[i]);
        ids.push_back(makeIdentifier(images[i].id, i, usedIds));
    }

    const std::string created = utcTimestamp();
    auto buildHeader = [&](const std::vector<uint64_t>& positions) {
        std::string x;
        x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        x += "<!--\nExtensible Image Serialization Format - XISF version 1.0\nCreated with xisfconv " +
             std::string(kVersion) + "\n-->\n";
        x += "<xisf version=\"1.0\" xmlns=\"http://www.pixinsight.com/xisf\" "
             "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
             "xsi:schemaLocation=\"http://www.pixinsight.com/xisf http://pixinsight.com/xisf/xisf-1.0.xsd\">\n";
        for (size_t i = 0; i < images.size(); ++i) {
            const XisfOutImage& img = images[i];
            const PixelBuffer& px = *img.pixels;
            x += "<Image id=\"" + ids[i] + "\" geometry=\"" + std::to_string(px.width) + ":" +
                 std::to_string(px.height) + ":" + std::to_string(px.channels) + "\" sampleFormat=\"" +
                 sampleFormatName(px.format) + "\"";
            if (isFloat(px.format)) {
                x += " bounds=\"" + formatDouble(img.lowerBound) + ":" + formatDouble(img.upperBound) + "\"";
            }
            x += std::string(" colorSpace=\"") + (img.rgb ? "RGB" : "Gray") + "\"";
            if (!hostIsLittleEndian() && sampleBytes(px.format) > 1) x += " byteOrder=\"big\"";
            x += " location=\"attachment:" + std::to_string(positions[i]) + ":" + std::to_string(blocks[i].size) + "\"";
            x += blocks[i].attributes + ">\n";
            for (const auto& k : img.keywords) {
                x += "<FITSKeyword name=\"" + xmlEscape(k.name) + "\" value=\"" + xmlEscape(k.value) +
                     "\" comment=\"" + xmlEscape(k.comment) + "\"/>\n";
            }
            if (!img.cfaPattern.empty()) {
                x += "<ColorFilterArray pattern=\"" + xmlEscape(img.cfaPattern) + "\" width=\"" +
                     std::to_string(img.cfaWidth) + "\" height=\"" + std::to_string(img.cfaHeight) + "\"/>\n";
            }
            for (const auto& p : img.properties) x += propertyXml(p);
            x += "</Image>\n";
        }
        x += "<Metadata>\n";
        x += "<Property id=\"XISF:CreationTime\" type=\"TimePoint\" value=\"" + created + "\"/>\n";
        x += "<Property id=\"XISF:CreatorApplication\" type=\"String\">xisfconv " + std::string(kVersion) + "</Property>\n";
        x += "<Property id=\"XISF:BlockAlignmentSize\" type=\"UInt16\" value=\"" + std::to_string(kAlignment) + "\"/>\n";
        if (!opt.codec.empty()) {
            x += "<Property id=\"XISF:CompressionCodecs\" type=\"String\">" + opt.codec +
                 (opt.shuffle ? "+sh" : "") + "</Property>\n";
        }
        x += "</Metadata>\n</xisf>\n";
        return x;
    };

    // Attachment positions depend on the header length, which depends on the positions' digits:
    // iterate until stable (it settles in two or three rounds).
    std::vector<uint64_t> positions(images.size(), 0);
    std::string header;
    for (int round = 0; round < 8; ++round) {
        header = buildHeader(positions);
        std::vector<uint64_t> next(images.size());
        uint64_t p = alignUp(16 + header.size());
        for (size_t i = 0; i < images.size(); ++i) {
            next[i] = p;
            p = alignUp(p + blocks[i].size);
        }
        if (next == positions) break;
        positions = next;
        if (round == 7) throw Error("could not lay out the XISF header");
    }
    if (header.size() > 0xFFFFFFFFull) throw Error("XISF header too large");

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw Error("cannot create " + path);
    const uint32_t len = static_cast<uint32_t>(header.size());
    const unsigned char preamble[16] = {'X', 'I', 'S', 'F', '0', '1', '0', '0',
                                        static_cast<unsigned char>(len), static_cast<unsigned char>(len >> 8),
                                        static_cast<unsigned char>(len >> 16), static_cast<unsigned char>(len >> 24),
                                        0, 0, 0, 0};
    out.write(reinterpret_cast<const char*>(preamble), 16);
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    uint64_t pos = 16 + header.size();
    const std::vector<char> zeros(kAlignment, 0);
    for (size_t i = 0; i < images.size(); ++i) {
        if (positions[i] > pos) out.write(zeros.data(), static_cast<std::streamsize>(positions[i] - pos));
        // Write in pieces: some platforms limit a single write to 2 GiB.
        uint64_t done = 0;
        while (done < blocks[i].size) {
            const uint64_t n = std::min<uint64_t>(blocks[i].size - done, 1u << 30);
            out.write(reinterpret_cast<const char*>(blocks[i].data + done), static_cast<std::streamsize>(n));
            done += n;
        }
        pos = positions[i] + blocks[i].size;
        if (!out) throw Error("write error on " + path);
    }
    out.close();
    if (!out) throw Error("write error on " + path);
}

}  // namespace xisfconv
