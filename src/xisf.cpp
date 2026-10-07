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

XisfFile::XisfFile(const std::string& path, const XisfBlocksRedirect* redirect, bool verifying) : path_(path) {
    file_.open(toPath(path), std::ios::binary);
    if (!file_) failToOpen(path);
    std::error_code directoryError;
    if (std::filesystem::is_directory(toPath(path), directoryError)) throw Error("is a directory, not a file", ErrorKind::Io);
    file_.seekg(0, std::ios::end);
    fileSize_ = static_cast<uint64_t>(file_.tellg());
    file_.seekg(0);

    unsigned char preamble[16] = {0};
    const size_t have = static_cast<size_t>(std::min<uint64_t>(fileSize_, 16));
    if (have > 0 && !file_.read(reinterpret_cast<char*>(preamble), static_cast<std::streamsize>(have))) {
        throw Error("cannot read the file", ErrorKind::Io);
    }
    // An XISF header file is an XML document and nothing else (the specification has it begin
    // with the XML declaration; a byte order mark and blanks before it are let pass).
    size_t first = have >= 3 && preamble[0] == 0xEF && preamble[1] == 0xBB && preamble[2] == 0xBF ? 3 : 0;
    while (first < have && (preamble[first] == ' ' || preamble[first] == '\t' || preamble[first] == '\r' || preamble[first] == '\n')) ++first;
    if (have >= 8 && std::memcmp(preamble, "XISF0100", 8) == 0) {
        if (fileSize_ < 16) throw Error("file too short to be XISF");
        const uint32_t headerLength = readLE32(preamble + 8);
        if (headerLength == 0 || 16ull + headerLength > fileSize_) throw Error("invalid XISF header length");
        headerXml_.resize(headerLength);
        if (!file_.read(&headerXml_[0], headerLength)) throw Error("cannot read XISF header");
    } else if (first < have && preamble[first] == '<') {
        // An XML document. Before all of it is read, its beginning has to show the root element
        // of an XISF header: any SVG, HTML or XML file begins like this, of any size.
        std::string beginning(static_cast<size_t>(std::min<uint64_t>(fileSize_, kXmlBeginning)), '\0');
        file_.clear();
        file_.seekg(0);
        if (!file_.read(&beginning[0], static_cast<std::streamsize>(beginning.size()))) throw Error("cannot read the file", ErrorKind::Io);
        const std::string rootName = xmlRootNameAtStart(beginning);
        if (rootName != "xisf" && (!rootName.empty() || beginning.size() < fileSize_)) {
            throw Error(rootName.empty() ? "this XML file is not an XISF header: it does not begin with a root element <xisf> (within its first 64 KiB)"
                                         : "this XML file is not an XISF header: its root element is <" + rootName + ">, expected <xisf>");
        }
        headerFile_ = true;
        if (fileSize_ > 0xFFFFFFFFull) throw Error("the XML header is larger than an XISF header can be");
        headerXml_.resize(static_cast<size_t>(fileSize_));
        file_.clear();
        file_.seekg(0);
        if (!file_.read(&headerXml_[0], static_cast<std::streamsize>(fileSize_))) throw Error("cannot read XISF header", ErrorKind::Io);
    } else {
        if (have >= 8 && std::memcmp(preamble, "XISB0100", 8) == 0) {
            throw Error("this is an XISF data blocks file (.xisb): it holds data of a distributed XISF unit; name the "
                        "header file of that unit (.xish)");
        }
        if (fileSize_ < 16) throw Error("file too short to be XISF");
        if (std::memcmp(preamble, "SIMPLE", 6) == 0) throw Error("this looks like a FITS file, not XISF");
        throw Error("not an XISF 1.0 file (bad signature)");
    }
    while (!headerXml_.empty() && headerXml_.back() == '\0') headerXml_.pop_back();

    root_ = xml::parse(headerXml_);
    if (root_->name != "xisf") {
        throw Error(std::string(headerFile_ ? "this XML file is not an XISF header: its" : "XML header") +
                    " root element is <" + root_->name + ">, expected <xisf>");
    }
    version_ = attrOr(*root_, "version");
    if (version_ != "1.0") warn("unexpected XISF version '" + version_ + "'; trying anyway");

    // The files the header names beside itself. (Before the elements are looked at: the text of
    // a String may be in one of them.)
    // A monolithic file holds all of its data, and a header file is named as one: the
    // specification says both. A file that is neither is not followed to other files by its own
    // word (a file somebody was sent as "an image" would otherwise read the files beside it).
    external_ = std::make_unique<XisfExternalFiles>(path_, redirect, verifying);
    if (!headerFile_) external_->notAHeaderToFollow("this is a monolithic XISF file, which holds all of its data");
    else if (!isXisfHeaderName(path_)) external_->notAHeaderToFollow("this header file does not have the name of one");
    unitSize_ = fileSize_;
    findExternalFiles(*root_);
    for (const auto& ids : idsByWhere_) {
        const auto path = resolved_.find(ids.first);
        if (path != resolved_.end()) namedBlocks_[path->second].insert(ids.second.begin(), ids.second.end());
    }
    if (!headerFile_ && !externalFiles_.empty()) {
        warn("this monolithic XISF file names data in other files (" + externalFiles_.front() +
             (externalFiles_.size() > 1 ? ", ..." : "") + "), which the specification has for a header file (.xish) only");
    }

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

namespace {

// What went wrong with a property, for a message that names the property already: an error
// that begins with its name is told without it.
std::string reasonFor(const std::string& id, const std::string& message) {
    const std::string name = "property " + id;
    if (!startsWith(message, name)) return message;
    const std::string rest = message.substr(name.size());
    if (startsWith(rest, ": ")) return rest.substr(2);
    if (startsWith(rest, " is ")) return "it" + rest;
    return message;
}

}  // namespace

void XisfFile::findExternalFiles(const xml::Node& node) {
    const std::string* location = node.attr("location");
    if (location && node.name != "Data" && isExternalXisfLocation(*location)) {
        try {
            const XisfLocation loc = parseXisfLocation(*location, "<" + node.name + ">");
            const std::string where = external_->where(loc);
            size_t slot = static_cast<size_t>(std::find(externalFiles_.begin(), externalFiles_.end(), where) - externalFiles_.begin());
            const bool first = slot == externalFiles_.size();
            if (first) {
                externalFiles_.push_back(where);
                externalReasons_.emplace_back(ErrorKind::Io);
            }
            // A file is resolved once, however often the header names it; again only while it is
            // not read and is written another way than before. (One way may be followed where
            // another is not: an absolute path and @header_dir/ can lead to the same file.)
            const std::string written = std::string(loc.kind == XisfLocation::Kind::Url ? "u" : loc.headerDir ? "h" : "a") + loc.target;
            if (tried_.insert(written).second) {
                const std::string asked = external_->asked(loc);
                if (!asked.empty() && std::find(externalAsked_.begin(), externalAsked_.end(), asked) == externalAsked_.end()) {
                    externalAsked_.push_back(asked);
                }
                if (externalReasons_[slot]) {
                    try {
                        const std::string path = external_->resolve(loc, "<" + node.name + ">");
                        resolved_[where] = path;
                        externalReasons_[slot].reset();
                        if (std::find(externalPaths_.begin(), externalPaths_.end(), path) == externalPaths_.end()) {
                            std::error_code ec;
                            const uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(toPath(path), ec));
                            externalPaths_.push_back(path);
                            if (!ec && size <= std::numeric_limits<uint64_t>::max() - unitSize_) unitSize_ += size;
                        }
                    } catch (const Error& e) {
                        // (a file that is not there, or that the header is not followed to, is said when it is read)
                        if (first) externalReasons_[slot] = e.kind;
                    }
                }
            }
            // (the blocks the header names in data blocks files, whichever way it names the file:
            // they are put to the file once all of the header was seen, see the constructor)
            if (loc.hasId) {
                idsByWhere_[where].insert(loc.id);
                const std::string asked = external_->asked(loc);
                if (!asked.empty()) idsAsAsked_[asked].insert(loc.id);
            }
        } catch (const Error&) {
        }
    }
    for (const auto& child : node.children) findExternalFiles(*child);
}

XisfProperty XisfFile::parseProperty(const xml::Node& node) {
    XisfProperty p;
    p.id = attrOr(node, "id");
    p.type = attrOr(node, "type");
    p.comment = attrOr(node, "comment");
    p.format = attrOr(node, "format");
    p.location = attrOr(node, "location");
    p.node = &node;
    if (const std::string* v = node.attr("value")) {
        p.value = *v;
        p.exactText = p.type == "String" && !textFitsElement(*v);
    } else if (!p.location.empty()) {
        if (p.type == "String") {
            try {
                // (texts are read when the file is opened: no more of them than the file can hold, see propertyBudget)
                const uint64_t declared = declaredBlockSize(node), budget = propertyBudget(budgetSize());
                if (declared > budget || stringBytes_ > budget - declared) {
                    throw Error("the texts of this file declare more data than a file of its size can hold");
                }
                stringBytes_ += declared;
                std::vector<uint8_t> bytes = readBlock(node, true, "property " + p.id);
                // XISF String data blocks are UTF-8; drop a trailing NUL if present.
                while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
                p.value.assign(bytes.begin(), bytes.end());
            } catch (const Error& e) {
                warn(std::string("cannot read property ") + p.id + ": " + reasonFor(p.id, e.what()));
                p.hasBlockData = true;
            }
        } else {
            p.hasBlockData = true;
        }
    } else {
        p.value = node.text;  // String properties may store their value as character data
        p.exactText = p.type == "String" && node.crReference;
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

    const XisfLocation loc = parseXisfLocation(location, what);
    switch (loc.kind) {
        case XisfLocation::Kind::Attachment:
            if (headerFile_) {
                throw Error(what + " is said to be attached to the file (" + location + "), but this is an XISF header "
                            "file (.xish): nothing is attached to it");
            }
            block.bytes = readAttachment(loc.position, loc.size);
            block.attachment = true;
            block.position = loc.position;
            break;
        case XisfLocation::Kind::Inline:
            block.bytes = decodeText(loc.encoding, element.text);
            break;
        case XisfLocation::Kind::Embedded: {
            const xml::Node* data = element.child("Data");
            if (!data) throw Error("embedded " + what + " has no <Data> element");
            block.bytes = decodeText(attrOr(*data, "encoding"), data->text);
            attrSource = data;
            break;
        }
        case XisfLocation::Kind::Path:
        case XisfLocation::Kind::Url: {
            XisfExternalFiles::Block found = external_->read(loc, what);
            block.bytes = std::move(found.bytes);
            block.external = true;
            block.indexed = found.indexed;
            block.indexUncompressedLength = found.uncompressedLength;
            break;
        }
    }

    auto attr = [&](const char* key) {
        if (const std::string* v = attrSource->attr(key)) return *v;
        return attrOr(element, key);
    };
    block.checksum = attr("checksum");
    block.compression = attr("compression");
    block.subblocks = attr("subblocks");
    if (block.indexed && block.indexUncompressedLength != 0 && block.compression.empty()) {
        // (the header says how a block is read; the index of the data blocks file only repeats it)
        warn(what + ": the index of its data blocks file calls the block compressed, the header does not; it is read "
             "as the header says");
    }
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
    if (isExternalXisfLocation(attrOr(*img.node, "location")) && !img.node->attr("compression")) {
        // (pixels that are a file, or a block of one: its size is known before it is read, and a
        // file of another size is not read to find that out)
        const uint64_t stored = external_->storedSize(parseXisfLocation(attrOr(*img.node, "location"), "image " + std::to_string(index)),
                                                      "image " + std::to_string(index));
        if (stored != expected) {
            throw Error("image " + std::to_string(index) + ": pixel data is " + std::to_string(stored) +
                        " bytes, geometry requires " + std::to_string(expected));
        }
    }
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

bool isNumericPropertyType(const std::string& type) {
    PropertyElement e;
    return propertyElement(type, e) && e.kind != 'c';
}

Property XisfFile::loadProperty(const XisfProperty& x, bool verify) {
    Property p;
    p.id = x.id;
    p.type = x.type;
    p.comment = x.comment;
    p.format = x.format;
    PropertyElement element;
    const bool known = propertyElement(x.type, element);
    // A property is a value, or a data block. One that is built of other elements (the tables
    // and structures of the specification) is neither, and is not something this form can hold.
    if (x.node) {
        for (const auto& child : x.node->children) {
            if (child->name != "Data") throw Error("it is made of <" + child->name + "> elements, which are not read");
        }
    }
    const bool stringBlock = x.type == "String" && !x.location.empty() && x.node && !x.node->attr("value");
    if (stringBlock) {
        // the text as it is in its block, with whatever it ends in
        const std::vector<uint8_t> bytes = readBlock(*x.node, verify, "property " + x.id);
        p.text.assign(bytes.begin(), bytes.end());
        p.block = true;
        return p;
    }
    if (!x.hasBlockData) {
        if (known) throw Error("a " + x.type + " without a data block");
        p.text = x.value;
        // A carriage return the header writes as a character reference is one for every reader.
        // Written into a header again as it is, an XML reader would read CR LF as a line feed;
        // in a data block nothing is read into the text.
        p.block = x.exactText;
        return p;
    }
    if (!x.node) throw Error("its data block was not found");
    p.array = true;
    p.data = readBlock(*x.node, verify, "property " + x.id);
    auto number = [&](const char* name, uint64_t& out) {
        const std::string* text = x.node->attr(name);
        if (text && !parseUInt64(*text, out)) throw Error(std::string("its ") + name + " attribute is not a number");
        return text != nullptr;
    };
    if (isMatrixPropertyType(x.type)) {
        if (!number("rows", p.rows) || !number("columns", p.columns)) throw Error("a matrix without rows and columns");
    } else if (known) {
        // The block says how long a vector is; a length attribute that says otherwise is not believed.
        number("length", p.rows);
        if (p.data.size() % element.size != 0) throw Error("its data is not a whole number of elements");
        p.rows = p.data.size() / element.size;
    } else {
        number("length", p.rows);
    }
    if (known) {
        // XISF blocks are little-endian unless they say otherwise; a complex number is two numbers
        const size_t part = element.kind == 'c' ? element.size / 2 : element.size;
        if (attrOr(*x.node, "byteOrder", "little") == "big" && part > 1) byteSwapInPlace(p.data.data(), p.data.size() / part, part);
        const std::string problem = propertyProblem(p);
        if (!problem.empty()) throw Error(problem);
    }
    return p;
}

const std::vector<std::string>& XisfFile::indexProblems() const { return external_->indexProblems(); }

// What the properties may declare is held against the bytes that are there (propertyBudget): the
// file, and of its data blocks files the blocks the header names, each once and no more of a
// file than the file has. A file that is one block counts for nothing: a header could name any
// large file of its directory that way, and declare as much.
uint64_t XisfFile::budgetSize() const {
    if (!budgetSize_) {
        uint64_t size = fileSize_;
        for (const auto& file : namedBlocks_) {
            const uint64_t bytes = external_->indexedBytes(file.first, file.second);
            size = bytes > std::numeric_limits<uint64_t>::max() - size ? std::numeric_limits<uint64_t>::max() : size + bytes;
        }
        budgetSize_ = size;
    }
    return *budgetSize_;
}

uint64_t XisfFile::storedBlockSize(const xml::Node& element) const {
    const std::string location = attrOr(element, "location");
    try {
        const XisfLocation loc = parseXisfLocation(location, "a data block");
        if (loc.kind == XisfLocation::Kind::Attachment) return loc.size;
        if (loc.external()) return external_->storedSize(loc, "a data block");
    } catch (const Error&) {
    }
    return 0;
}

// What the data block of an element says it holds, without reading it: the size it is stored
// with, or the size it declares to have once it is decompressed. 0 if it has none.
uint64_t XisfFile::declaredBlockSize(const xml::Node& element) const {
    const std::string location = attrOr(element, "location");
    const xml::Node* source = &element;
    uint64_t stored = 0;
    if (startsWith(location, "attachment:")) {
        const auto parts = split(location, ':');
        if (parts.size() != 3 || !parseUInt64(parts[2], stored)) return 0;   // (reading it says what is wrong)
    } else if (isExternalXisfLocation(location)) {
        try {
            stored = external_->storedSize(parseXisfLocation(location, "a data block"), "a data block");
        } catch (const Error&) {
            return 0;   // (reading it says what is wrong)
        }
    } else if (location == "embedded") {
        if (const xml::Node* data = element.child("Data")) source = data;
        stored = source->text.size();
    } else {
        stored = element.text.size();
    }
    const std::string* compression = source->attr("compression");
    if (!compression) compression = element.attr("compression");
    if (!compression || compression->empty()) return stored;
    try {
        return std::max<uint64_t>(stored, parseXisfCompression(*compression).uncompressedSize);
    } catch (const Error&) {
        return stored;
    }
}

XisfFile::PropertyStorage XisfFile::propertyStorage(const XisfProperty& x) const {
    if (x.node) {
        for (const auto& child : x.node->children)
            if (child->name != "Data") return PropertyStorage::Unread;
    }
    PropertyElement element;
    const bool known = propertyElement(x.type, element);
    if (x.type == "String" && !x.location.empty() && x.node && !x.node->attr("value")) {
        return x.hasBlockData ? PropertyStorage::Unread : PropertyStorage::TextBlock;   // (unread: the block could not be read)
    }
    if (!x.hasBlockData) return known ? PropertyStorage::Unread : x.exactText ? PropertyStorage::TextBlock : PropertyStorage::Header;
    return known && x.node ? PropertyStorage::Array : PropertyStorage::Unread;
}

Property XisfFile::loadPropertyCounted(const XisfProperty& x, bool verify) {
    const bool counted = counted_.count(&x) != 0;
    const uint64_t declared = x.node && !x.location.empty() ? declaredBlockSize(*x.node) : x.value.size();
    if (!counted) {
        const uint64_t budget = propertyBudget(budgetSize());
        if (declared > budget || countedBytes_ > budget - declared) {
            throw Error("the properties of this file declare more data than a file of its size can hold (" +
                        std::to_string(declared) + " bytes on top of " + std::to_string(countedBytes_) + ")");
        }
    }
    Property p = loadProperty(x, verify);
    if (!counted) {
        counted_.insert(&x);
        countedBytes_ += std::max<uint64_t>(declared, p.array ? p.data.size() : p.text.size());
    }
    return p;
}

std::vector<Property> XisfFile::loadProperties(size_t imageIndex, bool verify) {
    const std::vector<XisfProperty>& list = imageIndex == kFileProperties ? fileProperties_ : images_.at(imageIndex).properties;
    const uint64_t budget = propertyBudget(budgetSize());
    std::vector<Property> out;
    out.reserve(list.size());
    for (const XisfProperty& x : list) {
        if (x.id.empty()) {
            warn("a property without an id is left out");
            continue;
        }
        try {
            const uint64_t declared = x.node && !x.location.empty() ? declaredBlockSize(*x.node) : x.value.size();
            if (declared > budget || propertyBytes_ > budget - declared) {
                throw Error("the properties of this file declare more data than a file of its size can hold (" +
                            std::to_string(declared) + " bytes on top of " + std::to_string(propertyBytes_) + ")");
            }
            out.push_back(loadProperty(x, verify));
            propertyBytes_ += std::max<uint64_t>(declared, out.back().array ? out.back().data.size() : out.back().text.size());
        } catch (const Error& e) {
            warn("property " + x.id + " is left out: " + reasonFor(x.id, e.what()));
        }
    }
    return out;
}

bool XisfFile::readNumericProperty(size_t imageIndex, const std::string& id, std::vector<double>& out,
                                   size_t* rows, size_t* columns) {
    const XisfProperty* x = findProperty(imageIndex, id);
    if (!x || !x->node || x->location.empty() || !isNumericPropertyType(x->type)) return false;
    Property p;
    try {
        p = loadProperty(*x, true);
    } catch (const Error& e) {
        warn(std::string("cannot read property ") + id + ": " + reasonFor(id, e.what()));
        return false;
    }
    if (!propertyNumbers(p, out)) return false;
    const bool matrix = isMatrixPropertyType(p.type);
    if (rows) *rows = static_cast<size_t>(matrix ? p.rows : 1);
    if (columns) *columns = static_cast<size_t>(matrix ? p.columns : p.rows);
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
