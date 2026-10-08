// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "xisfwrite.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <set>
#include <utility>

#include "codecs.hpp"
#include "xisf.hpp"

namespace xisfconv {

namespace {

constexpr uint64_t kAlignment = 4096;

// The text of a keyword. A FITS header is ASCII, but the keywords of an XISF file are XML, and
// what one holds beyond ASCII (a name with an umlaut, a degree sign in a comment) is kept: a
// character of UTF-8 as it is, any other byte as a question mark.
std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
            if (n != 0 && i + n <= s.size() && isXmlText(s.substr(i, n))) {
                out.append(s, i, n);
                i += n;
            } else {
                out += '?';
                ++i;
            }
            continue;
        }
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:
                if (c == '\t' || c == '\n' || c == '\r') out += "&#" + std::to_string(c) + ";";
                else if (c < 0x20 || c == 0x7F) out += ' ';   // not representable in XML 1.0
                else out += static_cast<char>(c);
        }
        ++i;
    }
    return out;
}

// The text of a property: UTF-8 as it is. (What XML cannot hold is not written this way, see
// isXmlText.) In an attribute, tabs and line breaks are written as character references,
// which is the only way they stay what they are there. As the content of an element they are
// written as they are: a text that PixInsight wrote with its line breaks as CR LF is then the
// same bytes again, and every reader makes of it what it made of the original.
std::string xmlText(const std::string& s, bool attribute = true) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\t': case '\n': case '\r':
                if (attribute) out += "&#" + std::to_string(c) + ";";
                else out += static_cast<char>(c);
                break;
            default: out += static_cast<char>(c);
        }
    }
    return out;
}

// Text that is not XML text, made into some: a control character becomes a blank, a byte that
// is not UTF-8 and a code point XML excludes a question mark. The rest stays.
std::string madeXmlText(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
        if (n == 0 || i + n > s.size() || !isXmlText(s.substr(i, n))) {
            out += c < 0x80 ? ' ' : '?';
            ++i;
            continue;
        }
        out.append(s, i, n);
        i += n;
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

// The bytes stored for one image plus the attributes that describe them.
struct Block {
    const uint8_t* data = nullptr;  // points into `owned` or into the pixel buffer
    uint64_t size = 0;
    std::vector<uint8_t> owned;
    std::string attributes;         // compression / subblocks / checksum, each with a leading space
    uint64_t uncompressed = 0;      // the size before compression, if the block is stored compressed
};

// `raw`: the bytes to store, `itemSize` the size of the numbers they consist of (1 if none).
void prepareBlock(const uint8_t* raw, size_t rawSize, size_t itemSize, const XisfWriteOptions& opt, Block& block) {
    block.data = raw;
    block.size = rawSize;

    if (!opt.codec.empty() && rawSize > 0) {
        const bool shuffle = opt.shuffle && itemSize > 1;
        std::vector<uint8_t> shuffledData;
        const uint8_t* src = raw;
        if (shuffle) {
            shuffledData = shuffled(raw, rawSize, itemSize);
            src = shuffledData.data();
        }
        const uint64_t chunk = xisfSubblockSize(opt.codec, opt.subblockSize);
        std::vector<uint8_t> stored;
        std::string subblocks;
        size_t chunks = 0;
        for (uint64_t off = 0; off < rawSize; off += chunk, ++chunks) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk, rawSize - off));
            const std::vector<uint8_t> c = xisfCompress(opt.codec, src + off, n, opt.level);
            stored.insert(stored.end(), c.begin(), c.end());
            if (!subblocks.empty()) subblocks += ':';
            subblocks += std::to_string(c.size()) + "," + std::to_string(n);
        }
        if (stored.size() < rawSize) {  // otherwise compression doesn't pay off: store raw
            block.owned.swap(stored);
            block.data = block.owned.data();
            block.size = block.owned.size();
            block.uncompressed = rawSize;
            block.attributes += " compression=\"" + opt.codec + (shuffle ? "+sh" : "") + ":" + std::to_string(rawSize) +
                                (shuffle ? ":" + std::to_string(itemSize) : "") + "\"";
            if (chunks > 1) block.attributes += " subblocks=\"" + subblocks + "\"";
        }
    }
    if (!opt.checksum.empty()) {
        // The checksum covers the block as stored (i.e. the compressed bytes).
        const size_t n = static_cast<size_t>(block.size);
        std::string digest;
        if (!xisfDigest(opt.checksum, block.data, n, digest)) throw Error("unsupported checksum algorithm '" + opt.checksum + "'", ErrorKind::Argument);
        block.attributes += " checksum=\"" + opt.checksum + ":" + digest + "\"";
    }
}

uint64_t alignUp(uint64_t v) { return (v + kAlignment - 1) / kAlignment * kAlignment; }

// The properties of one element (an image, or the file), as XML. A property whose data is
// attached gets a place in `blocks`, and its location is filled in from `positions` (which
// holds zeros until the layout is known).
struct PropertyWriter {
    const XisfWriteOptions& opt;
    std::vector<Block>& blocks;
    const std::function<std::string(size_t)>& location;   // of a block that is not in the header, by its number
    bool layout;       // the first pass: blocks are prepared
    size_t next = 0;   // the block the next attached property uses

    std::string dataBlock(const uint8_t* data, size_t size, size_t itemSize, bool inHeader = false) {
        if (size <= kXisfMaxInlineBlock || inHeader) return " location=\"inline:base64\">" + base64Encode(data, size) + "</Property>\n";
        if (layout) {
            blocks.emplace_back();
            prepareBlock(data, size, itemSize, opt, blocks.back());
        }
        const size_t at = next++;
        return " location=\"" + location(at) + "\"" + blocks[at].attributes + "/>\n";
    }

    // (the header is built more than once: what there is to say is said the first time)
    void note(const std::string& message) const {
        if (layout) warn(message);
    }

    std::string xml(const Property& p) {
        // The id and the type are attributes of the element: text that XML cannot hold has no
        // place there, and a property without them is none.
        if (p.id.empty() || !isXmlText(p.id) || !isXmlText(p.type)) {
            note("a property whose id or type is not text that XML can hold is not written" +
                 (isXmlText(p.id) && !p.id.empty() ? " (" + p.id + ")" : std::string()));
            return {};
        }
        std::string x = "<Property id=\"" + xmlText(p.id) + "\" type=\"" + xmlText(p.type) + "\"";
        // (an array, not a braced list of std::make_pair: GCC 14 warns that the pairs of such a
        // list may dangle, which they do not, -Wdangling-reference)
        const std::pair<const char*, const std::string*> extras[] = {{"comment", &p.comment}, {"format", &p.format}};
        for (const auto& extra : extras) {
            if (extra.second->empty()) continue;
            if (!isXmlText(*extra.second)) {
                note("property " + p.id + ": its " + extra.first + " is not text that XML can hold; characters are replaced");
                x += std::string(" ") + extra.first + "=\"" + xmlText(madeXmlText(*extra.second)) + "\"";
            } else {
                x += std::string(" ") + extra.first + "=\"" + xmlText(*extra.second) + "\"";
            }
        }
        if (p.array) {
            PropertyElement element;
            const bool known = propertyElement(p.type, element);
            if (isMatrixPropertyType(p.type)) x += " rows=\"" + std::to_string(p.rows) + "\" columns=\"" + std::to_string(p.columns) + "\"";
            else if (known || p.rows) x += " length=\"" + std::to_string(p.rows) + "\"";
            // (shuffled by the size of its numbers: a complex number is two of them)
            const size_t item = !known ? 1 : element.kind == 'c' ? element.size / 2 : element.size;
            return x + dataBlock(p.data.data(), p.data.size(), item, p.inHeader);
        }
        if (p.type == "String") {
            // A text of a header is written as that header had it, whatever is at its ends:
            // each reader then makes of it what it made of it before. A text that was a data
            // block is one again, and so is one that is to come back byte for byte (see
            // Property::block) and one that XML cannot hold. XISF allows that for a String.
            if (!p.block && isXmlText(p.text)) return x + ">" + xmlText(p.text, false) + "</Property>\n";
            return x + dataBlock(reinterpret_cast<const uint8_t*>(p.text.data()), p.text.size(), 1);
        }
        if (!isXmlText(p.text)) {
            note("property " + p.id + ": its value is not text that XML can hold; characters are replaced");
            return x + " value=\"" + xmlText(madeXmlText(p.text)) + "\"/>\n";
        }
        return x + " value=\"" + xmlText(p.text) + "\"/>\n";
    }
};

}  // namespace

void writeXisf(const std::string& path, const std::vector<XisfOutImage>& images, const XisfWriteOptions& opt) {
    if (images.empty()) throw Error("no images to write");
    warnIfChecksumUnknownToPixInsight(opt.checksum);
    if (!opt.codec.empty() && !isXisfWriteCodec(opt.codec)) {
        throw Error("unsupported XISF compression codec '" + opt.codec + "' (use zlib, lz4, lz4hc or zstd)", ErrorKind::Argument);
    }
    if (opt.level != 0) {
        if (opt.codec.empty()) throw Error("a compression level without a codec", ErrorKind::Argument);
        xisfCompress(opt.codec, nullptr, 0, opt.level);   // (says what is wrong with the level)
    }
    if (!opt.creatorApplication.empty() && !textFitsElement(opt.creatorApplication)) {
        throw Error("the name of the creator application is not text that XML holds as it is (control characters, "
                    "or blanks at its ends)", ErrorKind::Argument);
    }

    // The attached blocks: the pixels of each image, then the data of the properties that are
    // too large for the header, in the order of the header.
    std::vector<Block> blocks(images.size());
    std::vector<std::string> ids;
    std::set<std::string> usedIds;
    for (size_t i = 0; i < images.size(); ++i) {
        const PixelBuffer& px = *images[i].pixels;
        prepareBlock(px.data.data(), px.data.size(), sampleBytes(px.format), opt, blocks[i]);
        ids.push_back(makeIdentifier(images[i].id, i, usedIds));
    }

    const bool distributed = !opt.blocksPath.empty();
    if (distributed && !isXisfBlocksFileName(opt.blocksName)) {
        throw Error("the output cannot be a distributed unit under this name: a header names its data blocks file, and holds "
                    "only names that are valid UTF-8 text; choose another output name", ErrorKind::Argument);
    }
    const std::string created = utcTimestamp();
    bool layout = true;   // the first header that is built finds out which properties are attached
    std::vector<uint64_t> positions(images.size(), 0);
    std::vector<uint64_t> blockIds;   // distributed: the identifier of each block in the data blocks file
    // Where a block is that is not in the header: attached to this file, or in the data blocks file.
    const std::function<std::string(size_t)> location = [&](size_t at) {
        if (distributed) return xmlAttributeValue(xisfBlocksFileLocation(opt.blocksName, at < blockIds.size() ? blockIds[at] : 0));
        return "attachment:" + std::to_string(at < positions.size() ? positions[at] : 0) + ":" + std::to_string(blocks[at].size);
    };
    auto buildHeader = [&]() {
        PropertyWriter properties{opt, blocks, location, layout, images.size()};
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
            x += " location=\"" + location(i) + "\"";
            x += blocks[i].attributes + ">\n";
            for (const auto& k : img.keywords) {
                x += "<FITSKeyword name=\"" + xmlEscape(k.name) + "\" value=\"" + xmlEscape(k.value) +
                     "\" comment=\"" + xmlEscape(k.comment) + "\"/>\n";
            }
            if (!img.cfaPattern.empty()) {
                x += "<ColorFilterArray pattern=\"" + xmlEscape(img.cfaPattern) + "\" width=\"" +
                     std::to_string(img.cfaWidth) + "\" height=\"" + std::to_string(img.cfaHeight) + "\"/>\n";
            }
            for (const auto& p : img.properties) x += properties.xml(p);
            if (!img.iccProfile.empty()) {
                x += "<ICCProfile location=\"inline:base64\">" + base64Encode(img.iccProfile.data(), img.iccProfile.size()) +
                     "</ICCProfile>\n";
            }
            x += "</Image>\n";
        }
        x += "<Metadata>\n";
        x += "<Property id=\"XISF:CreationTime\" type=\"TimePoint\" value=\"" + created + "\"/>\n";
        if (opt.creatorApplication.empty()) {
            x += "<Property id=\"XISF:CreatorApplication\" type=\"String\">xisfconv " + std::string(kVersion) + "</Property>\n";
        } else {
            x += "<Property id=\"XISF:CreatorApplication\" type=\"String\">" + xmlText(opt.creatorApplication, false) + "</Property>\n";
            x += "<Property id=\"XISF:CreatorModule\" type=\"String\">xisfconv " + std::string(kVersion) + "</Property>\n";
        }
        // (the alignment of the blocks attached to a monolithic file; a header file has none)
        if (!distributed) {
            x += "<Property id=\"XISF:BlockAlignmentSize\" type=\"UInt16\" value=\"" + std::to_string(kAlignment) + "\"/>\n";
        }
        if (!opt.codec.empty()) {
            x += "<Property id=\"XISF:CompressionCodecs\" type=\"String\">" + opt.codec +
                 (opt.shuffle ? "+sh" : "") + "</Property>\n";
        }
        for (const auto& p : opt.metadata)
            if (!isFileStorageProperty(p.id)) x += properties.xml(p);
        x += "</Metadata>\n</xisf>\n";
        layout = false;
        return x;
    };

    const auto writeBlocks = [&](std::ofstream& out, const std::string& name, uint64_t pos) {
        const std::vector<char> zeros(kAlignment, 0);
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (positions[i] > pos) out.write(zeros.data(), static_cast<std::streamsize>(positions[i] - pos));
            // Write in pieces: some platforms limit a single write to 2 GiB.
            uint64_t done = 0;
            while (done < blocks[i].size) {
                const uint64_t n = std::min<uint64_t>(blocks[i].size - done, 1u << 30);
                out.write(reinterpret_cast<const char*>(blocks[i].data + done), static_cast<std::streamsize>(n));
                done += n;
            }
            pos = positions[i] + blocks[i].size;
            if (!out) throw Error("write error on " + name, ErrorKind::Io);
        }
        out.close();
        if (!out) throw Error("write error on " + name, ErrorKind::Io);
    };

    if (distributed) {
        // The header is the whole of its file. The blocks go into the data blocks file, behind an
        // index of one node that has them all, each under an identifier the header names it by.
        std::string header = buildHeader();   // (finds out which properties have a block there)
        blockIds = newXisbIds(blocks.size());
        header = buildHeader();
        positions.assign(blocks.size(), 0);
        std::vector<XisbOutBlock> index(blocks.size());
        uint64_t p = alignUp(xisbIndexSize(blocks.size()));
        for (size_t i = 0; i < blocks.size(); ++i) {
            positions[i] = p;
            index[i] = {blockIds[i], p, blocks[i].size, blocks[i].uncompressed};
            p = alignUp(p + blocks[i].size);
        }
        std::ofstream data(toPath(opt.blocksPath), std::ios::binary | std::ios::trunc);
        if (!data) throw Error("cannot create " + opt.blocksPath, ErrorKind::Io);
        const std::vector<uint8_t> head = xisbIndexBytes(index);
        data.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
        writeBlocks(data, opt.blocksPath, head.size());

        std::ofstream out(toPath(path), std::ios::binary | std::ios::trunc);
        if (!out) throw Error("cannot create " + path, ErrorKind::Io);
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.close();
        if (!out) throw Error("write error on " + path, ErrorKind::Io);
        return;
    }

    // Attachment positions depend on the header length, which depends on the positions' digits:
    // iterate until stable (it settles in two or three rounds).
    std::string header;
    for (int round = 0; round < 8; ++round) {
        header = buildHeader();
        std::vector<uint64_t> next(blocks.size());
        uint64_t p = alignUp(16 + header.size());
        for (size_t i = 0; i < blocks.size(); ++i) {
            next[i] = p;
            p = alignUp(p + blocks[i].size);
        }
        if (next == positions) break;
        positions = next;
        if (round == 7) throw Error("could not lay out the XISF header");
    }
    if (header.size() > 0xFFFFFFFFull) throw Error("XISF header too large");

    std::ofstream out(toPath(path), std::ios::binary | std::ios::trunc);
    if (!out) throw Error("cannot create " + path, ErrorKind::Io);
    const uint32_t len = static_cast<uint32_t>(header.size());
    const unsigned char preamble[16] = {'X', 'I', 'S', 'F', '0', '1', '0', '0',
                                        static_cast<unsigned char>(len), static_cast<unsigned char>(len >> 8),
                                        static_cast<unsigned char>(len >> 16), static_cast<unsigned char>(len >> 24),
                                        0, 0, 0, 0};
    out.write(reinterpret_cast<const char*>(preamble), 16);
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    writeBlocks(out, path, 16 + header.size());
}

}  // namespace xisfconv
