// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "xisf.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "codecs.hpp"

namespace xisfconv {

namespace {

uint32_t readLE32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

std::string attrOr(const xml::Node& n, const char* key, const std::string& dflt = std::string()) {
    const std::string* v = n.attr(key);
    return v ? *v : dflt;
}

using Compression = XisfCompression;

std::vector<uint8_t> decompressOne(const Compression& c, const uint8_t* src, size_t size, size_t expected) {
    if (c.codec == "zlib") return zlibDecompress(src, size, expected);
    if (c.codec == "lz4" || c.codec == "lz4hc") return lz4BlockDecompress(src, size, expected);
    return zstdDecompress(src, size, expected);
}

// Converts interleaved ("Normal") samples to planar layout.
void deinterleave(std::vector<uint8_t>& data, uint64_t pixels, uint64_t channels, size_t sb) {
    if (channels <= 1) return;
    std::vector<uint8_t> out(data.size());
    for (uint64_t ch = 0; ch < channels; ++ch) {
        uint8_t* dst = out.data() + ch * pixels * sb;
        const uint8_t* src = data.data() + ch * sb;
        const size_t stride = static_cast<size_t>(channels) * sb;
        for (uint64_t i = 0; i < pixels; ++i) {
            std::memcpy(dst + i * sb, src + i * stride, sb);
        }
    }
    data.swap(out);
}

}  // namespace

XisfCompression parseXisfCompression(const std::string& text) {
    const auto parts = split(text, ':');
    if (parts.size() < 2) throw Error("malformed compression attribute '" + text + "'");
    Compression c;
    std::string codec = trim(parts[0]);
    const size_t plus = codec.find('+');
    if (plus != std::string::npos) {
        if (codec.substr(plus) != "+sh") throw Error("unknown compression modifier in '" + text + "'");
        c.shuffled = true;
        codec = codec.substr(0, plus);
    }
    if (codec != "zlib" && codec != "lz4" && codec != "lz4hc" && codec != "zstd") {
        throw Error("unsupported compression codec '" + codec + "'");
    }
    c.codec = codec;
    if (!parseUInt64(parts[1], c.uncompressedSize)) throw Error("malformed compression size in '" + text + "'");
    if (c.shuffled) {
        if (parts.size() < 3 || !parseUInt64(parts[2], c.itemSize) || c.itemSize == 0) {
            throw Error("byte-shuffled compression without item size in '" + text + "'");
        }
    }
    return c;
}

void warnIfChecksumUnknownToPixInsight(const std::string& algorithm) {
    if (!startsWith(toLower(algorithm), "sha3")) return;
    warn(algorithm + " checksums are valid XISF 1.0, but PixInsight (1.9.3) does not open images that carry them; "
         "use sha1, sha256 or sha512 for files PixInsight has to read");
}

bool xisfDigest(const std::string& algorithm, const uint8_t* data, size_t size, std::string& hex) {
    const std::string algo = toLower(trim(algorithm));
    if (algo == "sha-1" || algo == "sha1") hex = sha1Hex(data, size);
    else if (algo == "sha-256" || algo == "sha256") hex = sha256Hex(data, size);
    else if (algo == "sha-512" || algo == "sha512") hex = sha512Hex(data, size);
    else if (algo == "sha3-256") hex = sha3Hex(data, size, 256);
    else if (algo == "sha3-512") hex = sha3Hex(data, size, 512);
    else return false;
    return true;
}

XisfChecksumState XisfFile::verifyBlockChecksum(const XisfStoredBlock& block, const std::string& what) {
    if (block.checksum.empty()) return XisfChecksumState::None;
    const size_t colon = block.checksum.find(':');
    if (colon == std::string::npos) {
        warn("malformed checksum attribute on " + what + "; not verified");
        return XisfChecksumState::Unsupported;
    }
    const std::string algo = toLower(trim(block.checksum.substr(0, colon)));
    const std::string expected = toLower(trim(block.checksum.substr(colon + 1)));
    std::string actual;
    if (!xisfDigest(algo, block.bytes.data(), block.bytes.size(), actual)) {
        warn("checksum algorithm '" + algo + "' on " + what + " is not supported; not verified");
        return XisfChecksumState::Unsupported;
    }
    if (actual != expected) {
        throw Error("checksum mismatch on " + what + " (" + algo + "): file is corrupt "
                    "(use --no-verify to convert anyway)", ErrorKind::Checksum);
    }
    return XisfChecksumState::Verified;
}

XisfFile::XisfFile(const std::string& path) : path_(path) {
    file_.open(toPath(path), std::ios::binary);
    if (!file_) failToOpen(path);
    std::error_code directoryError;
    if (std::filesystem::is_directory(toPath(path), directoryError)) throw Error("is a directory, not a file", ErrorKind::Io);
    file_.seekg(0, std::ios::end);
    fileSize_ = static_cast<uint64_t>(file_.tellg());
    file_.seekg(0);

    unsigned char preamble[16];
    if (fileSize_ < 16 || !file_.read(reinterpret_cast<char*>(preamble), 16)) {
        throw Error("file too short to be XISF");
    }
    if (std::memcmp(preamble, "XISF0100", 8) != 0) {
        if (std::memcmp(preamble, "XISB0100", 8) == 0) {
            throw Error("this is an XISF data blocks file (.xisb); distributed XISF units are not supported");
        }
        if (std::memcmp(preamble, "SIMPLE", 6) == 0) throw Error("this looks like a FITS file, not XISF");
        throw Error("not an XISF 1.0 file (bad signature)");
    }
    const uint32_t headerLength = readLE32(preamble + 8);
    if (headerLength == 0 || 16ull + headerLength > fileSize_) throw Error("invalid XISF header length");

    headerXml_.resize(headerLength);
    if (!file_.read(&headerXml_[0], headerLength)) throw Error("cannot read XISF header");
    while (!headerXml_.empty() && headerXml_.back() == '\0') headerXml_.pop_back();

    root_ = xml::parse(headerXml_);
    if (root_->name != "xisf") throw Error("XML header root element is <" + root_->name + ">, expected <xisf>");
    version_ = attrOr(*root_, "version");
    if (version_ != "1.0") warn("unexpected XISF version '" + version_ + "'; trying anyway");

    for (const auto& child : root_->children) {
        if (child->name == "Image") {
            parseImage(*child);
        } else if (child->name == "Metadata") {
            for (const xml::Node* p : child->childrenNamed("Property")) fileProperties_.push_back(parseProperty(*p));
        } else if (child->name == "Property") {
            fileProperties_.push_back(parseProperty(*child));
        }
    }
}

XisfProperty XisfFile::parseProperty(const xml::Node& node) {
    XisfProperty p;
    p.id = attrOr(node, "id");
    p.type = attrOr(node, "type");
    p.comment = attrOr(node, "comment");
    p.location = attrOr(node, "location");
    p.node = &node;
    if (const std::string* v = node.attr("value")) {
        p.value = *v;
    } else if (!p.location.empty()) {
        if (p.type == "String") {
            try {
                std::vector<uint8_t> bytes = readBlock(node, true, "property " + p.id);
                // XISF String data blocks are UTF-8; drop a trailing NUL if present.
                while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
                p.value.assign(bytes.begin(), bytes.end());
            } catch (const Error& e) {
                warn(std::string("cannot read property ") + p.id + ": " + e.what());
                p.hasBlockData = true;
            }
        } else {
            p.hasBlockData = true;
        }
    } else {
        p.value = node.text;  // String properties may store their value as character data
    }
    return p;
}

void XisfFile::parseImage(const xml::Node& node) {
    XisfImage img;
    img.node = &node;
    img.id = attrOr(node, "id");
    img.location = attrOr(node, "location");
    img.compression = attrOr(node, "compression");
    img.subblocks = attrOr(node, "subblocks");
    img.checksum = attrOr(node, "checksum");
    if (img.location == "embedded") {
        if (const xml::Node* d = node.child("Data")) {
            img.compression = attrOr(*d, "compression", img.compression);
            img.subblocks = attrOr(*d, "subblocks", img.subblocks);
            img.checksum = attrOr(*d, "checksum", img.checksum);
        }
    }
    img.imageType = attrOr(node, "imageType");
    img.orientation = attrOr(node, "orientation");
    img.colorSpace = attrOr(node, "colorSpace", "Gray");

    const size_t index = images_.size();
    auto unsupported = [&](const std::string& why) {
        if (img.unsupported.empty()) img.unsupported = why;
    };

    // geometry = width:height[:depth...]:channelCount
    const auto g = split(attrOr(node, "geometry"), ':');
    std::vector<uint64_t> dims;
    for (const auto& s : g) {
        uint64_t v;
        if (!parseUInt64(s, v) || v == 0) { dims.clear(); break; }
        dims.push_back(v);
    }
    if (dims.size() < 2) {
        unsupported("missing or malformed geometry attribute");
    } else {
        img.width = dims[0];
        img.height = dims[1];
        img.channels = dims.size() >= 3 ? dims.back() : 1;
        if (dims.size() > 3) unsupported("images with more than 2 dimensions are not supported");
    }

    img.sampleFormatText = attrOr(node, "sampleFormat");
    if (!parseXisfSampleFormat(img.sampleFormatText, img.format)) {
        unsupported("sample format '" + img.sampleFormatText + "' is not supported");
    }

    if (const std::string* b = node.attr("bounds")) {
        const auto parts = split(*b, ':');
        double lo, hi;
        if (parts.size() == 2 && parseDouble(parts[0], lo) && parseDouble(parts[1], hi) && hi > lo) {
            img.lowerBound = lo;
            img.upperBound = hi;
            img.boundsDeclared = true;
        } else {
            warn("image " + std::to_string(index) + ": ignoring malformed bounds '" + *b + "'");
        }
    }
    if (!img.boundsDeclared && !isFloat(img.format)) {
        img.lowerBound = 0;
        img.upperBound = std::ldexp(1.0, static_cast<int>(8 * sampleBytes(img.format))) - 1;
    }

    const std::string storage = attrOr(node, "pixelStorage", "Planar");
    if (storage == "Normal") img.planar = false;
    else if (storage != "Planar") unsupported("unknown pixelStorage '" + storage + "'");

    const std::string order = attrOr(node, "byteOrder", "little");
    if (order == "big") img.bigEndian = true;
    else if (order != "little") unsupported("unknown byteOrder '" + order + "'");

    if (img.location.empty()) unsupported("image has no location attribute");

    for (const auto& child : node.children) {
        const xml::Node& c = *child;
        if (c.name == "FITSKeyword") {
            img.keywords.push_back({attrOr(c, "name"), attrOr(c, "value"), attrOr(c, "comment")});
        } else if (c.name == "Property") {
            img.properties.push_back(parseProperty(c));
        } else if (c.name == "ColorFilterArray") {
            img.cfa.present = true;
            img.cfa.pattern = attrOr(c, "pattern");
            img.cfa.name = attrOr(c, "name");
            uint64_t w = 0, h = 0;
            parseUInt64(attrOr(c, "width"), w);
            parseUInt64(attrOr(c, "height"), h);
            img.cfa.width = static_cast<int>(std::min<uint64_t>(w, 1024));
            img.cfa.height = static_cast<int>(std::min<uint64_t>(h, 1024));
        } else if (c.name == "Resolution") {
            double h = 0, v = 0;
            if (parseDouble(attrOr(c, "horizontal"), h) && parseDouble(attrOr(c, "vertical"), v) && h > 0 && v > 0) {
                img.resolution.present = true;
                img.resolution.horizontal = h;
                img.resolution.vertical = v;
                img.resolution.unit = attrOr(c, "unit", "inch");
            }
        } else if (c.name == "DisplayFunction") {
            DisplayFunction& df = img.displayFunction;
            bool ok = true;
            auto load = [&](const char* key, double* dst) {
                const std::string* v = c.attr(key);
                if (!v) return;  // attribute absent: keep the identity default
                const auto parts = split(*v, ':');
                if (parts.size() != 4) { ok = false; return; }
                for (int k = 0; k < 4; ++k)
                    if (!parseDouble(parts[k], dst[k])) ok = false;
            };
            load("m", df.m);
            load("s", df.s);
            load("h", df.h);
            load("l", df.l);
            load("r", df.r);
            df.name = attrOr(c, "name");
            if (ok) df.present = true;
            else warn("image " + std::to_string(index) + ": ignoring malformed DisplayFunction");
        } else if (c.name == "ICCProfile") {
            img.hasIccProfile = true;
            img.iccNode = &c;
        }
    }
    images_.push_back(std::move(img));
}

std::vector<uint8_t> XisfFile::readAttachment(uint64_t position, uint64_t size) {
    if (position > fileSize_ || size > fileSize_ - position) {
        throw Error("data block at " + std::to_string(position) + "+" + std::to_string(size) +
                    " lies beyond the end of the file (truncated download?)");
    }
    if (size > std::numeric_limits<size_t>::max()) throw Error("data block too large for this platform");
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(position));
    if (size > 0 && !file_.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size))) {
        throw Error("read error in data block", ErrorKind::Io);
    }
    return buf;
}

XisfStoredBlock XisfFile::readStoredBlock(const xml::Node& element, const std::string& what) {
    const std::string location = attrOr(element, "location");
    const xml::Node* attrSource = &element;
    XisfStoredBlock block;

    auto decodeText = [&](const std::string& encoding, const std::string& text) {
        if (encoding == "base64") return base64Decode(text);
        if (encoding == "hex") return hexDecode(text);
        throw Error("unsupported data encoding '" + encoding + "' in " + what);
    };

    if (startsWith(location, "attachment:")) {
        const auto parts = split(location, ':');
        uint64_t pos = 0, size = 0;
        if (parts.size() != 3 || !parseUInt64(parts[1], pos) || !parseUInt64(parts[2], size)) {
            throw Error("malformed location '" + location + "' in " + what);
        }
        block.bytes = readAttachment(pos, size);
        block.attachment = true;
        block.position = pos;
    } else if (startsWith(location, "inline:")) {
        block.bytes = decodeText(location.substr(7), element.text);
    } else if (location == "embedded") {
        const xml::Node* data = element.child("Data");
        if (!data) throw Error("embedded " + what + " has no <Data> element");
        block.bytes = decodeText(attrOr(*data, "encoding"), data->text);
        attrSource = data;
    } else if (startsWith(location, "url(") || startsWith(location, "path(")) {
        throw Error(what + " is stored in an external file (distributed XISF), which is not supported");
    } else {
        throw Error("unsupported location '" + location + "' in " + what);
    }

    auto attr = [&](const char* key) {
        if (const std::string* v = attrSource->attr(key)) return *v;
        return attrOr(element, key);
    };
    block.checksum = attr("checksum");
    block.compression = attr("compression");
    block.subblocks = attr("subblocks");
    return block;
}

std::vector<uint8_t> XisfFile::decodeBlock(const XisfStoredBlock& block, const std::string& what, uint64_t expectedSize) {
    if (block.compression.empty()) return block.bytes;
    const std::vector<uint8_t>& stored = block.bytes;

    const Compression c = parseXisfCompression(block.compression);
    if (expectedSize != 0 && c.uncompressedSize != expectedSize) {
        throw Error(what + ": compressed block declares " + std::to_string(c.uncompressedSize) +
                    " bytes, geometry requires " + std::to_string(expectedSize));
    }
    // Guard against absurd declared sizes in damaged headers (no codec here expands beyond ~1:33000).
    if (c.uncompressedSize / (1u << 20) > stored.size() + 1) {
        throw Error(what + ": declared uncompressed size is implausible for the stored data");
    }
    if (c.uncompressedSize > std::numeric_limits<size_t>::max()) throw Error("data block too large for this platform");
    std::vector<uint8_t> out;
    if (block.subblocks.empty()) {
        out = decompressOne(c, stored.data(), stored.size(), static_cast<size_t>(c.uncompressedSize));
    } else {
        out.reserve(static_cast<size_t>(c.uncompressedSize));
        uint64_t offset = 0;
        for (const auto& pair : split(block.subblocks, ':')) {
            const auto cu = split(pair, ',');
            uint64_t cs = 0, us = 0;
            if (cu.size() != 2 || !parseUInt64(cu[0], cs) || !parseUInt64(cu[1], us)) {
                throw Error("malformed subblocks attribute in " + what);
            }
            if (cs > stored.size() - offset || us > c.uncompressedSize - out.size()) {
                throw Error("subblock sizes exceed the data block in " + what);
            }
            const auto part = decompressOne(c, stored.data() + offset, static_cast<size_t>(cs), static_cast<size_t>(us));
            out.insert(out.end(), part.begin(), part.end());
            offset += cs;
        }
        if (out.size() != c.uncompressedSize) throw Error("subblocks do not add up to the declared size in " + what);
    }
    if (c.shuffled) unshuffle(out, static_cast<size_t>(c.itemSize));
    return out;
}

std::vector<uint8_t> XisfFile::readBlock(const xml::Node& element, bool verify, const std::string& what,
                                         uint64_t expectedSize) {
    XisfStoredBlock block = readStoredBlock(element, what);
    if (verify) verifyBlockChecksum(block, what);
    if (block.compression.empty()) return std::move(block.bytes);
    return decodeBlock(block, what, expectedSize);
}

bool DisplayFunction::isIdentity() const {
    for (int k = 0; k < 4; ++k) {
        if (m[k] != 0.5 || s[k] != 0 || h[k] != 1 || l[k] != 0 || r[k] != 1) return false;
    }
    return true;
}

PixelBuffer XisfFile::readPixels(size_t index, bool verify) {
    const XisfImage& img = images_.at(index);
    if (!img.unsupported.empty()) throw Unsupported("image " + std::to_string(index) + ": " + img.unsupported);

    const size_t sb = sampleBytes(img.format);
    const uint64_t expected = checkedMul(checkedMul(checkedMul(img.width, img.height, "image size"), img.channels,
                                                    "image size"), sb, "image size");
    PixelBuffer px;
    px.width = img.width;
    px.height = img.height;
    px.channels = img.channels;
    px.format = img.format;
    px.data = readBlock(*img.node, verify, "image " + std::to_string(index), expected);
    if (px.data.size() != expected) {
        throw Error("image " + std::to_string(index) + ": pixel data is " + std::to_string(px.data.size()) +
                    " bytes, geometry requires " + std::to_string(expected));
    }
    if (sb > 1 && img.bigEndian == hostIsLittleEndian()) {
        byteSwapInPlace(px.data.data(), px.data.size() / sb, sb);
    }
    if (!img.planar) deinterleave(px.data, img.width * img.height, img.channels, sb);
    return px;
}

std::vector<uint8_t> XisfFile::readIccProfile(size_t index, bool verify) {
    const XisfImage& img = images_.at(index);
    if (!img.iccNode) return {};
    return readBlock(*img.iccNode, verify, "ICC profile of image " + std::to_string(index));
}

namespace {
struct ElemType { const char* name; size_t size; bool isFloat; bool isSigned; };
// The element types of vector and matrix properties that are read as numbers.
const ElemType* numericElementType(const std::string& type) {
    static const ElemType types[] = {{"I8", 1, false, true},   {"UI8", 1, false, false}, {"Byte", 1, false, false},
                                     {"I16", 2, false, true},  {"UI16", 2, false, false}, {"I32", 4, false, true},
                                     {"UI32", 4, false, false}, {"I64", 8, false, true},  {"UI64", 8, false, false},
                                     {"F32", 4, true, true},   {"F64", 8, true, true}};
    const bool matrix = type.size() > 6 && type.compare(type.size() - 6, 6, "Matrix") == 0;
    const bool vector = type.size() > 6 && type.compare(type.size() - 6, 6, "Vector") == 0;
    if (!matrix && !vector) return nullptr;
    const std::string elem = type.substr(0, type.size() - 6);
    for (const auto& e : types)
        if (elem == e.name) return &e;
    return nullptr;
}
}  // namespace

bool isNumericPropertyType(const std::string& type) { return numericElementType(type) != nullptr; }

bool XisfFile::readNumericProperty(size_t imageIndex, const std::string& id, std::vector<double>& out,
                                   size_t* rows, size_t* columns) {
    const XisfProperty* p = findProperty(imageIndex, id);
    if (!p || !p->node || p->location.empty()) return false;
    std::string t = p->type;
    const bool matrix = t.size() > 6 && t.compare(t.size() - 6, 6, "Matrix") == 0;
    const ElemType* et = numericElementType(t);
    if (!et) return false;
    std::vector<uint8_t> bytes;
    try {
        bytes = readBlock(*p->node, true, "property " + id);
    } catch (const Error& e) {
        warn(std::string("cannot read property ") + id + ": " + e.what());
        return false;
    }
    if (bytes.size() % et->size != 0) return false;
    const bool swap = attrOr(*p->node, "byteOrder", "little") == "big" ? hostIsLittleEndian() : !hostIsLittleEndian();
    if (swap) byteSwapInPlace(bytes.data(), bytes.size() / et->size, et->size);
    const size_t n = bytes.size() / et->size;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* b = bytes.data() + i * et->size;
        if (et->isFloat) {
            if (et->size == 4) { float f; std::memcpy(&f, b, 4); out[i] = f; }
            else { double d; std::memcpy(&d, b, 8); out[i] = d; }
        } else if (et->isSigned) {
            int64_t v = 0;
            switch (et->size) {
                case 1: { int8_t x; std::memcpy(&x, b, 1); v = x; break; }
                case 2: { int16_t x; std::memcpy(&x, b, 2); v = x; break; }
                case 4: { int32_t x; std::memcpy(&x, b, 4); v = x; break; }
                default: std::memcpy(&v, b, 8);
            }
            out[i] = static_cast<double>(v);
        } else {
            uint64_t v = 0;
            switch (et->size) {
                case 1: v = b[0]; break;
                case 2: { uint16_t x; std::memcpy(&x, b, 2); v = x; break; }
                case 4: { uint32_t x; std::memcpy(&x, b, 4); v = x; break; }
                default: std::memcpy(&v, b, 8);
            }
            out[i] = static_cast<double>(v);
        }
    }
    uint64_t r = 1, c = n;
    if (matrix) {
        if (!parseUInt64(attrOr(*p->node, "rows"), r) || !parseUInt64(attrOr(*p->node, "columns"), c) || r * c != n) {
            return false;
        }
    }
    if (rows) *rows = static_cast<size_t>(r);
    if (columns) *columns = static_cast<size_t>(c);
    return true;
}

const XisfProperty* XisfFile::findProperty(size_t imageIndex, const std::string& id) const {
    if (imageIndex < images_.size()) {
        for (const auto& p : images_[imageIndex].properties)
            if (p.id == id) return &p;
    }
    for (const auto& p : fileProperties_)
        if (p.id == id) return &p;
    return nullptr;
}

}  // namespace xisfconv
