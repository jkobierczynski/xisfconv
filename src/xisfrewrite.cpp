// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "xisfrewrite.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "codecs.hpp"
#include "xisf.hpp"

namespace xisfconv {

namespace {

constexpr uint64_t kAlignment = 4096;

uint64_t alignUp(uint64_t v) { return (v + kAlignment - 1) / kAlignment * kAlignment; }

bool isXmlSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Collects changes to the header text and applies them in one pass. Everything that is not
// edited stays byte for byte as it was.
class HeaderEditor {
public:
    explicit HeaderEditor(const std::string& xml) : xml_(xml) {}

    void set(const xml::Node& node, const std::string& name, const std::string& value) {
        const std::string text = name + "=\"" + value + "\"";  // (a value that needs escaping comes escaped)
        for (size_t i = 0; i < node.attributes.size(); ++i) {
            if (node.attributes[i].first == name) {
                const auto span = node.attributeSpans[i];
                edits_.push_back({span.first, span.second - span.first, text});
                return;
            }
        }
        // New attributes go behind the last one, before any space that precedes the '>'.
        size_t at = node.tagClose;
        while (at > node.start && isXmlSpace(xml_[at - 1])) --at;
        inserts_[at] += " " + text;
    }

    void remove(const xml::Node& node, const std::string& name) {
        for (size_t i = 0; i < node.attributes.size(); ++i) {
            if (node.attributes[i].first == name) {
                size_t begin = node.attributeSpans[i].first;
                while (begin > node.start && isXmlSpace(xml_[begin - 1])) --begin;
                edits_.push_back({begin, node.attributeSpans[i].second - begin, ""});
                return;
            }
        }
    }

    // Sets the attribute to `value`, or removes it if `value` is empty; no edit if nothing changes.
    void update(const xml::Node& node, const std::string& name, const std::string& value) {
        const std::string* current = node.attr(name);
        if (value.empty()) {
            if (current) remove(node, name);
        } else if (!current || *current != value) {
            set(node, name, value);
        }
    }

    void removeElement(const xml::Node& node) {
        size_t end = node.end;
        while (end < xml_.size() && (xml_[end] == ' ' || xml_[end] == '\t' || xml_[end] == '\r')) ++end;
        if (end < xml_.size() && xml_[end] == '\n') ++end;
        edits_.push_back({node.start, end - node.start, ""});
    }

    // Adds a child element at the end of `node` (which must not be an empty-element tag).
    void appendChild(const xml::Node& node, const std::string& elementText) {
        if (!node.selfClosing) inserts_[node.contentEnd] += elementText;
    }

    void replaceContent(const xml::Node& node, const std::string& text) {
        if (node.selfClosing) return;
        edits_.push_back({node.tagClose + 1, node.contentEnd - (node.tagClose + 1), text});
    }

    std::string result() const {
        std::vector<Edit> all = edits_;
        for (const auto& ins : inserts_) all.push_back({ins.first, 0, ins.second});
        std::stable_sort(all.begin(), all.end(), [](const Edit& a, const Edit& b) { return a.pos < b.pos; });
        std::string out;
        out.reserve(xml_.size() + 256 * all.size());
        size_t cursor = 0;
        for (const Edit& e : all) {
            if (e.pos < cursor || e.pos + e.len > xml_.size()) throw Error("internal error: overlapping header edits");
            out.append(xml_, cursor, e.pos - cursor);
            out += e.text;
            cursor = e.pos + e.len;
        }
        out.append(xml_, cursor, std::string::npos);
        return out;
    }

private:
    struct Edit {
        size_t pos, len;
        std::string text;
    };
    const std::string& xml_;
    std::vector<Edit> edits_;
    std::map<size_t, std::string> inserts_;  // new attributes, keyed by the end of the start tag
};

// True for a location attribute that refers to a data block. Other elements may use the
// attribute name for something else.
bool isBlockLocation(const xml::Node& node) {
    const std::string* location = node.attr("location");
    if (!location || node.name == "Data") return false;  // <Data> is the container of an embedded block
    return startsWith(*location, "attachment:") || startsWith(*location, "inline:") || *location == "embedded" ||
           startsWith(*location, "url(") || startsWith(*location, "path(");
}

// True for the location of a block that is not in the header itself: attached to a monolithic
// file, or in another file. These are the blocks a rewrite stores again.
bool isStoredLocation(const std::string& location) {
    return startsWith(location, "attachment:") || isExternalXisfLocation(location);
}

// A header element that refers to a data block.
struct BlockRef {
    const xml::Node* node = nullptr;
    std::string what;        // for messages: "image 0", "property X of image 0", ...
    const XisfImage* image = nullptr;  // set when the block holds the pixels of an image
};

void collectBlocks(const xml::Node& node, const std::string& context, const std::set<const xml::Node*>& skip,
                   std::vector<BlockRef>& out) {
    for (const auto& child : node.children) {
        const xml::Node& c = *child;
        if (skip.count(&c)) continue;
        std::string what;
        const std::string* id = c.attr("id");
        if (c.name == "Property") what = "property " + (id ? *id : std::string("(no id)"));
        else if (c.name == "ICCProfile") what = "ICC profile";
        else if (c.name == "Thumbnail") what = "thumbnail";
        else what = "<" + c.name + ">";
        if (!context.empty()) what += " of " + context;
        if (isBlockLocation(c)) out.push_back({&c, what, nullptr});
        collectBlocks(c, context, skip, out);
    }
}

// All elements with a location attribute, in document order.
std::vector<BlockRef> collectBlocks(const XisfFile& file, const std::set<const xml::Node*>& skip) {
    std::vector<BlockRef> out;
    for (const auto& child : file.root().children) {
        const xml::Node& c = *child;
        if (skip.count(&c)) continue;
        if (c.name == "Image") {
            std::string what = "image";
            const XisfImage* image = nullptr;
            for (size_t i = 0; i < file.images().size(); ++i) {
                if (file.images()[i].node == &c) {
                    what = "image " + std::to_string(i);
                    image = &file.images()[i];
                }
            }
            if (isBlockLocation(c)) out.push_back({&c, what, image});
            collectBlocks(c, what, skip, out);
        } else {
            const std::string* id = c.attr("id");
            const std::string what = c.name == "Property" ? "property " + (id ? *id : std::string("(no id)")) : "<" + c.name + ">";
            if (isBlockLocation(c)) out.push_back({&c, what, nullptr});
            collectBlocks(c, c.name == "Metadata" ? std::string() : what, skip, out);
        }
    }
    return out;
}

// Size of the items of a block, for byte shuffling. Any value gives a correct file; the right
// one compresses better.
size_t guessItemSize(const xml::Node& node) {
    std::string type;
    if (node.name == "Image") {
        if (const std::string* f = node.attr("sampleFormat")) type = *f;
    } else if (node.name == "Property") {
        if (const std::string* t = node.attr("type")) type = *t;
    }
    // UInt16, Float32, Complex64, F64Vector, UI16Matrix, I32, C32Vector ...: the bit count of a component.
    size_t bits = 0;
    for (size_t i = 0; i < type.size(); ++i) {
        if (type[i] >= '0' && type[i] <= '9') {
            while (i < type.size() && type[i] >= '0' && type[i] <= '9') bits = bits * 10 + static_cast<size_t>(type[i++] - '0');
            break;
        }
    }
    return (bits == 16 || bits == 32 || bits == 64) ? bits / 8 : 1;
}

// Size the pixel data of an image block must have; 0 for other blocks and unknown formats.
uint64_t imageBytes(const BlockRef& b) {
    if (!b.image || !b.image->unsupported.empty()) return 0;
    return checkedMul(checkedMul(checkedMul(b.image->width, b.image->height, "image size"), b.image->channels, "image size"),
                      sampleBytes(b.image->format), "image size");
}

// Algorithm of a checksum attribute in the spelling of the --checksum option ("sha-1" -> "sha1").
std::string checksumAlgorithm(const std::string& attribute) {
    std::string algo = toLower(trim(attribute.substr(0, attribute.find(':'))));
    algo.erase(std::remove(algo.begin(), algo.end(), '-'), algo.end());
    return algo;
}

// True if the block is already stored the way the options ask (so it is copied as it is).
bool storedAsRequested(const XisfRewriteOptions& opt, bool compressed, const XisfCompression& comp, size_t itemSize) {
    if (opt.codec.empty()) return true;
    if (opt.codec == "none") return !compressed;
    if (opt.level != 0) return false;   // (no file says the level a block was compressed with)
    return compressed && comp.codec == opt.codec && (itemSize <= 1 || comp.shuffled == opt.shuffle);
}

// A level and byte shuffling are of a codec that compresses: they need one, and the level must
// be one of its own.
void checkStorageOptions(const XisfRewriteOptions& opt) {
    const bool compresses = isXisfWriteCodec(opt.codec);
    if (opt.level != 0) {
        if (!compresses) {
            throw Error(opt.codec == "none" ? "a compression level, and no compression" : "a compression level without a codec",
                        ErrorKind::Argument);
        }
        xisfCompress(opt.codec, nullptr, 0, opt.level);   // (says what is wrong with the level)
    }
    if (!opt.shuffle && !compresses) {
        throw Error(opt.codec == "none" ? "byte shuffling off, and no compression" : "byte shuffling off without a codec",
                    ErrorKind::Argument);
    }
}

void checkImageSize(const BlockRef& b, uint64_t size) {
    const uint64_t expected = imageBytes(b);
    if (expected != 0 && size != expected) {
        throw Error(b.what + ": pixel data is " + std::to_string(size) + " bytes, geometry requires " + std::to_string(expected));
    }
}

struct Packed {
    std::vector<uint8_t> bytes;
    std::string compression, subblocks;
};

// Compresses `raw` the way the XISF writer does. Empty result if that is not smaller.
Packed compressBlock(const std::vector<uint8_t>& raw, size_t itemSize, const XisfRewriteOptions& opt) {
    Packed p;
    if (raw.empty()) return p;
    const bool shuffle = opt.shuffle && itemSize > 1;
    std::vector<uint8_t> shuffledData;
    const uint8_t* src = raw.data();
    if (shuffle) {
        shuffledData = shuffled(raw.data(), raw.size(), itemSize);
        src = shuffledData.data();
    }
    const uint64_t chunk = xisfSubblockSize(opt.codec, opt.subblockSize);
    std::string subblocks;
    size_t chunks = 0;
    for (uint64_t off = 0; off < raw.size(); off += chunk, ++chunks) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk, raw.size() - off));
        const std::vector<uint8_t> c = xisfCompress(opt.codec, src + off, n, opt.level);
        p.bytes.insert(p.bytes.end(), c.begin(), c.end());
        if (!subblocks.empty()) subblocks += ':';
        subblocks += std::to_string(c.size()) + "," + std::to_string(n);
        if (p.bytes.size() >= raw.size()) return Packed();  // no gain: stop early
    }
    p.compression = opt.codec + (shuffle ? "+sh" : "") + ":" + std::to_string(raw.size()) +
                    (shuffle ? ":" + std::to_string(itemSize) : "");
    if (chunks > 1) p.subblocks = subblocks;
    return p;
}

void writeBytes(std::ofstream& out, const uint8_t* data, uint64_t size) {
    uint64_t done = 0;
    while (done < size) {  // some platforms limit a single write to 2 GiB
        const uint64_t n = std::min<uint64_t>(size - done, 1u << 30);
        out.write(reinterpret_cast<const char*>(data + done), static_cast<std::streamsize>(n));
        done += n;
    }
}

void writeZeros(std::ofstream& out, uint64_t count) {
    static const std::vector<char> zeros(65536, 0);
    while (count > 0) {
        const uint64_t n = std::min<uint64_t>(count, zeros.size());
        out.write(zeros.data(), static_cast<std::streamsize>(n));
        count -= n;
    }
}

// What the read-back compares: the stored bytes of a block that was copied, the decoded bytes
// of one that was stored differently.
struct Fingerprint {
    bool stored = true;
    std::string sha1;
    std::string what;
};

void readBack(const std::string& path, const std::vector<Fingerprint>& expected, size_t imageCount,
              const XisfBlocksRedirect* redirect) {
    XisfFile out(path, redirect);
    if (out.images().size() != imageCount) throw Error("read-back: the output has a different number of images");
    std::vector<BlockRef> blocks;
    for (const auto& b : collectBlocks(out, {})) {
        if (isStoredLocation(*b.node->attr("location"))) blocks.push_back(b);
    }
    if (blocks.size() != expected.size()) throw Error("read-back: the output has a different number of data blocks");
    for (size_t i = 0; i < blocks.size(); ++i) {
        progress("comparing", i, blocks.size());
        const std::string& what = expected[i].what;
        const XisfStoredBlock sb = out.readStoredBlock(*blocks[i].node, what);
        XisfFile::verifyBlockChecksum(sb, what);
        std::string sha1;
        if (expected[i].stored) {
            sha1 = sha1Hex(sb.bytes.data(), sb.bytes.size());
        } else {
            const auto raw = XisfFile::decodeBlock(sb, what);
            sha1 = sha1Hex(raw.data(), raw.size());
        }
        if (sha1 != expected[i].sha1) throw Error("read-back: " + what + " differs from the input");
    }
}


}  // namespace

XisfRewriteResult rewriteXisf(const std::string& input, const std::string& output, const XisfRewriteOptions& opt) {
    if (!opt.codec.empty() && opt.codec != "none" && !isXisfWriteCodec(opt.codec)) {
        throw Error("unsupported XISF compression codec '" + opt.codec + "' (use zlib, lz4, lz4hc, zstd or none)", ErrorKind::Argument);
    }
    if (opt.codec == "zstd" && !zstdAvailable()) throw Unsupported("this build has no Zstandard support; use --codec zlib");
    checkStorageOptions(opt);
    const bool recompress = isXisfWriteCodec(opt.codec);
    warnIfChecksumUnknownToPixInsight(opt.checksum);

    {
        std::error_code ec;
        if (std::filesystem::exists(toPath(output), ec) && std::filesystem::equivalent(toPath(input), toPath(output), ec)) {
            throw Error("internal error: the output of a rewrite is its input");
        }
    }
    XisfFile in(input);
    XisfRewriteResult result;
    result.inputSize = in.fileSize();
    const std::string& xml = in.headerXml();
    HeaderEditor editor(xml);

    std::set<const xml::Node*> removed;
    if (opt.imageIndex) {
        if (*opt.imageIndex >= in.images().size()) {
            throw Error("image index " + std::to_string(*opt.imageIndex) + " out of range (file has " +
                        std::to_string(in.images().size()) + ")", ErrorKind::Index);
        }
        for (size_t i = 0; i < in.images().size(); ++i) {
            if (i == *opt.imageIndex) continue;
            removed.insert(in.images()[i].node);
            editor.removeElement(*in.images()[i].node);
        }
    }
    const std::vector<BlockRef> blocks = collectBlocks(in, removed);

    // The blocks are written first, one at a time, behind room that is reserved for the header;
    // the header follows once their positions and sizes are known.
    uint64_t reserve = xml.size() + 1024;
    for (const xml::Node* node : removed) reserve -= node->end - node->start;
    size_t storedCount = 0;
    for (const BlockRef& b : blocks) {
        const std::string& location = *b.node->attr("location");
        if (!isStoredLocation(location)) continue;
        ++storedCount;
        reserve += 320;  // location, compression and checksum attributes at their longest
        uint64_t size = in.storedBlockSize(*b.node);
        // Sizes a damaged header declares must not decide how much is written: a block cannot be
        // larger than the file, nor expand beyond what any codec achieves. Reading the block
        // reports such a header further down.
        size = std::min(size, in.unitSize());
        if (const std::string* c = b.node->attr("compression")) {
            size = std::max(size, std::min(parseXisfCompression(*c).uncompressedSize, (size + 1) << 20));
        }
        reserve += 44 * (size / std::max<uint64_t>(1, opt.subblockSize) + 1);  // one subblocks entry per chunk
    }
    // The blocks are written into the file of the output itself, behind the room for its header,
    // or (a distributed unit) into the data blocks file, behind the room for its index.
    const bool distributed = !opt.blocksPath.empty();
    if (distributed && !isXisfBlocksFileName(opt.blocksName)) {
        throw Error("the output cannot be a distributed unit under this name: a header names its data blocks file, and holds "
                    "only names that are valid UTF-8 text; choose another output name", ErrorKind::Argument);
    }
    const std::string& blocksOutput = distributed ? opt.blocksPath : output;
    const uint64_t firstPosition = distributed ? xisbIndexSize(storedCount) : 16 + reserve;  // each block is aligned as needed when it is written
    const std::vector<uint64_t> blockIds = distributed ? newXisbIds(storedCount) : std::vector<uint64_t>();
    std::vector<XisbOutBlock> index;

    std::ofstream out(toPath(blocksOutput), std::ios::binary | std::ios::trunc);
    if (!out) throw Error("cannot create " + blocksOutput, ErrorKind::Io);
    writeZeros(out, firstPosition);
    uint64_t pos = firstPosition;

    std::vector<Fingerprint> fingerprints;
    bool unaligned = false;
    size_t written = 0;
    for (const BlockRef& b : blocks) {
        progress("rewriting", written++, blocks.size());
        const xml::Node& node = *b.node;
        if (!isStoredLocation(*node.attr("location"))) {
            // Inline and embedded blocks stay in the header as they are.
            if (opt.verifyInput) checkImageSize(b, in.readBlock(node, true, b.what).size());
            continue;
        }
        XisfStoredBlock sb = in.readStoredBlock(node, b.what);
        const bool unverifiable = opt.verifyInput && XisfFile::verifyBlockChecksum(sb, b.what) == XisfChecksumState::Unsupported;

        const bool wasCompressed = !sb.compression.empty();
        XisfCompression comp;
        if (wasCompressed) comp = parseXisfCompression(sb.compression);
        const size_t itemSize = wasCompressed && comp.shuffled ? static_cast<size_t>(comp.itemSize) : guessItemSize(node);

        // Blocks already in the requested form are copied as they are.
        bool verbatim = storedAsRequested(opt, wasCompressed, comp, itemSize);
        if (unverifiable && (!verbatim || !opt.checksum.empty())) {
            // Storing the block differently would drop the one thing that can tell whether it is intact.
            throw Unsupported(b.what + " has a checksum of a kind this program cannot verify (" +
                              sb.checksum.substr(0, sb.checksum.find(':')) + "); it is not stored differently or given "
                              "another checksum without that check (--no-verify overrides)");
        }
        std::vector<uint8_t> decoded;
        if (wasCompressed && (!verbatim || opt.verifyInput)) {
            decoded = XisfFile::decodeBlock(sb, b.what, imageBytes(b));
        }
        const std::vector<uint8_t>& raw = wasCompressed ? decoded : sb.bytes;
        if (opt.verifyInput && !wasCompressed) checkImageSize(b, raw.size());

        const std::vector<uint8_t>* stored = &sb.bytes;
        std::string compression = sb.compression, subblocks = sb.subblocks;
        Packed packed;
        if (!verbatim && opt.codec == "none") {
            stored = &raw;
            compression.clear();
            subblocks.clear();
            ++result.decompressed;
        } else if (!verbatim) {
            packed = compressBlock(raw, itemSize, opt);
            if (!packed.bytes.empty()) {
                stored = &packed.bytes;
                compression = packed.compression;
                subblocks = packed.subblocks;
                ++result.compressed;
            } else if (wasCompressed) {
                // The requested codec gains nothing here. The block is stored uncompressed rather
                // than left in another codec, so that the file uses the requested one only.
                stored = &raw;
                compression.clear();
                subblocks.clear();
                ++result.decompressed;
            } else {
                verbatim = true;  // compression does not pay off for this block
            }
        }
        if (verbatim) ++result.kept;

        std::string checksum = sb.checksum;
        const auto digest = [&](const std::string& algorithm) {
            std::string hex;
            if (!xisfDigest(algorithm, stored->data(), stored->size(), hex)) return false;
            checksum = algorithm + ":" + hex;
            ++result.checksums;
            return true;
        };
        if (opt.checksum == "none") {
            if (!checksum.empty()) ++result.checksumsRemoved;
            checksum.clear();
        } else if (!opt.checksum.empty()) {
            // A block that is copied and already carries this kind of checksum keeps it.
            if (!(verbatim && checksumAlgorithm(checksum) == checksumAlgorithm(opt.checksum)) && !digest(opt.checksum)) {
                throw Error("unsupported checksum algorithm '" + opt.checksum + "'", ErrorKind::Argument);
            }
        } else if (!verbatim && !checksum.empty()) {
            // The stored bytes changed: the checksum is computed again with the algorithm the file uses.
            const std::string algorithm = trim(checksum.substr(0, checksum.find(':')));
            if (!digest(algorithm)) {
                warn(b.what + ": its " + algorithm + " checksum cannot be recomputed for the new block; removed");
                checksum.clear();
                ++result.checksumsRemoved;
            }
        }

        // Uncompressed blocks are aligned (they can be memory-mapped); compressed ones follow directly,
        // as in the files PixInsight writes.
        const uint64_t size = stored->size();
        const uint64_t at = compression.empty() ? alignUp(pos) : pos;
        if (at % kAlignment != 0) unaligned = true;
        writeZeros(out, at - pos);
        writeBytes(out, stored->data(), size);
        if (!out) throw Error("write error on " + blocksOutput, ErrorKind::Io);
        pos = at + size;
        ++result.blocks;

        if (distributed) {
            const uint64_t id = blockIds[index.size()];
            index.push_back({id, at, size, compression.empty() ? 0 : parseXisfCompression(compression).uncompressedSize});
            editor.set(node, "location", xmlAttributeValue(xisfBlocksFileLocation(opt.blocksName, id)));
        } else {
            editor.set(node, "location", "attachment:" + std::to_string(at) + ":" + std::to_string(size));
        }
        editor.update(node, "compression", compression);
        editor.update(node, "subblocks", subblocks);
        editor.update(node, "checksum", checksum);

        if (opt.readBack) {
            Fingerprint f;
            f.stored = verbatim;
            f.what = b.what;
            const std::vector<uint8_t>& bytes = verbatim ? sb.bytes : raw;
            f.sha1 = sha1Hex(bytes.data(), bytes.size());
            fingerprints.push_back(std::move(f));
        }
    }

    // File metadata that describes the block storage.
    if (const xml::Node* metadata = in.root().child("Metadata")) {
        bool hasCodecs = false;
        for (const xml::Node* p : metadata->childrenNamed("Property")) {
            const std::string* pid = p->attr("id");
            if (pid && *pid == "XISF:CompressionCodecs") hasCodecs = true;
        }
        if (!hasCodecs && result.compressed > 0) {
            // PixInsight records the codec it used here; do the same for a file that had none.
            editor.appendChild(*metadata, "<Property id=\"XISF:CompressionCodecs\" type=\"String\">" + opt.codec +
                                              (opt.shuffle ? "+sh" : "") + "</Property>");
        }
        for (const xml::Node* p : metadata->childrenNamed("Property")) {
            const std::string* id = p->attr("id");
            if (!id) continue;
            if (*id == "XISF:BlockAlignmentSize") {
                // Compressed blocks are not aligned; like PixInsight, such a file states no alignment.
                // (Nor does a header file: nothing is attached to it.)
                if (unaligned || distributed) editor.removeElement(*p);
                else if (p->attr("value")) editor.update(*p, "value", std::to_string(kAlignment));
            } else if (result.compressed + result.decompressed > 0 && *id == "XISF:CompressionLevel") {
                editor.removeElement(*p);  // the level of the original compressor no longer applies
            } else if (result.compressed + result.decompressed > 0 && *id == "XISF:CompressionCodecs") {
                if (!recompress) editor.removeElement(*p);
                else if (p->attr("value")) editor.update(*p, "value", opt.codec + (opt.shuffle ? "+sh" : ""));
                else if (!p->selfClosing && !p->attr("location")) editor.replaceContent(*p, opt.codec + (opt.shuffle ? "+sh" : ""));
            }
        }
    }

    const std::string header = editor.result();
    if (distributed) {
        // The index of the data blocks file, now that it is known where the blocks are; and the
        // header, which is a file of its own.
        const std::vector<uint8_t> head = xisbIndexBytes(index);
        if (index.size() != storedCount || head.size() != firstPosition) throw Error("internal error: the block index does not fit");
        out.seekp(0);
        out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
        out.close();
        if (!out) throw Error("write error on " + blocksOutput, ErrorKind::Io);
        std::ofstream headerFile(toPath(output), std::ios::binary | std::ios::trunc);
        if (!headerFile) throw Error("cannot create " + output, ErrorKind::Io);
        headerFile.write(header.data(), static_cast<std::streamsize>(header.size()));
        headerFile.close();
        if (!headerFile) throw Error("write error on " + output, ErrorKind::Io);
    } else {
        if (16 + header.size() > firstPosition || header.size() > 0xFFFFFFFFull) throw Error("internal error: XISF header does not fit");
        const uint32_t len = static_cast<uint32_t>(header.size());
        const unsigned char preamble[16] = {'X', 'I', 'S', 'F', '0', '1', '0', '0',
                                            static_cast<unsigned char>(len), static_cast<unsigned char>(len >> 8),
                                            static_cast<unsigned char>(len >> 16), static_cast<unsigned char>(len >> 24),
                                            0, 0, 0, 0};
        out.seekp(0);
        out.write(reinterpret_cast<const char*>(preamble), 16);
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.close();
        if (!out) throw Error("write error on " + output, ErrorKind::Io);
    }

    // (the header names the data blocks file as it will be called; for now it is where it was written)
    const XisfBlocksRedirect redirect{opt.blocksName, opt.blocksPath};
    if (opt.readBack) {
        readBack(output, fingerprints, opt.imageIndex ? 1 : in.images().size(), distributed ? &redirect : nullptr);
        // And everything else in the file (inline and embedded blocks, image sizes) must check out.
        const VerifyReport report = verifyXisf(output, distributed ? &redirect : nullptr);
        if (!report.problems.empty()) throw Error("read-back: " + report.problems.front());
        result.readBack = true;
    }
    // Stored another way, or as the other kind of unit, or in other files than it was.
    result.changed = result.compressed + result.decompressed + result.checksums + result.checksumsRemoved > 0 ||
                     !removed.empty() || in.headerFile() != distributed;
    std::error_code ec;
    result.inputSize = in.unitSize();
    result.outputSize = static_cast<uint64_t>(std::filesystem::file_size(toPath(output), ec));
    if (distributed) result.outputSize += static_cast<uint64_t>(std::filesystem::file_size(toPath(opt.blocksPath), ec));
    return result;
}

bool xisfStoredAsRequested(const std::string& path, const XisfRewriteOptions& opt) {
    checkStorageOptions(opt);
    XisfFile file(path);
    if (opt.imageIndex && (file.images().size() > 1 || *opt.imageIndex >= file.images().size())) return false;
    for (const BlockRef& b : collectBlocks(file, {})) {
        const xml::Node& node = *b.node;
        if (!isStoredLocation(*node.attr("location"))) continue;
        const std::string* c = node.attr("compression");
        const std::string* sum = node.attr("checksum");
        XisfCompression comp;
        if (c) comp = parseXisfCompression(*c);
        if (!storedAsRequested(opt, c != nullptr, comp, c && comp.shuffled ? static_cast<size_t>(comp.itemSize) : guessItemSize(node))) {
            return false;
        }
        if (opt.checksum == "none" ? sum != nullptr
                                   : !opt.checksum.empty() && (!sum || checksumAlgorithm(*sum) != checksumAlgorithm(opt.checksum))) {
            return false;
        }
    }
    return true;
}

VerifyReport verifyXisf(const std::string& path, const XisfBlocksRedirect* redirect) {
    VerifyReport report;
    XisfFile file(path, redirect, true);
    const std::vector<BlockRef> blocks = collectBlocks(file, {});
    size_t done = 0;
    for (const BlockRef& b : blocks) {
        progress("verifying", done++, blocks.size());
        try {
            const XisfStoredBlock sb = file.readStoredBlock(*b.node, b.what);
            const XisfChecksumState state = XisfFile::verifyBlockChecksum(sb, b.what);
            if (state == XisfChecksumState::Verified) ++report.verified;
            else if (state == XisfChecksumState::None) ++report.unchecked;
            else report.notChecked.push_back(b.what + ": checksum of an unknown kind (" + sb.checksum.substr(0, sb.checksum.find(':')) + ")");
            checkImageSize(b, XisfFile::decodeBlock(sb, b.what).size());
            if (sb.indexed) {
                // What the index of the data blocks file says of the block is what the header says.
                const uint64_t declared = sb.compression.empty() ? 0 : parseXisfCompression(sb.compression).uncompressedSize;
                if (sb.indexUncompressedLength != declared) {
                    report.problems.push_back(b.what + ": the index of its data blocks file has an uncompressed length of " +
                                              std::to_string(sb.indexUncompressedLength) + ", the header " +
                                              (declared ? "one of " + std::to_string(declared) : std::string("a block that is not compressed")));
                }
            }
        } catch (const Unsupported& e) {
            report.notChecked.push_back(b.what + ": " + e.what());
        } catch (const Error& e) {
            const std::string message = e.what();
            if (e.kind == ErrorKind::NotAllowed) {
                // (a block in a file the header is not followed to is not a damaged one)
                report.notChecked.push_back(message.find(b.what) == std::string::npos ? b.what + ": " + message : message);
                continue;
            }
            report.problems.push_back(message.find(b.what) == std::string::npos ? b.what + ": " + message : message);
        }
    }
    // What is wrong with the index of a data blocks file, whether or not a block of this header
    // is concerned: the file is not what it was written as.
    for (const std::string& problem : file.indexProblems()) report.problems.push_back(problem);
    report.summary = std::to_string(file.images().size()) + (file.images().size() == 1 ? " image, " : " images, ") +
                     std::to_string(blocks.size()) + (blocks.size() == 1 ? " data block" : " data blocks");
    if (!file.externalFiles().empty()) {
        report.summary += std::string(file.headerFile() ? ", a distributed unit with data in " : ", with data in ") +
                          std::to_string(file.externalFiles().size()) +
                          (file.externalFiles().size() == 1 ? " other file" : " other files");
    }
    return report;
}

}  // namespace xisfconv
