// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "asdf.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>

#include "codecs.hpp"
#include "yaml.hpp"

namespace xisfconv {

namespace {

const char kBlockMagic[4] = {'\xd3', 'B', 'L', 'K'};
constexpr size_t kBlockHeaderSize = 48;  // the part after the magic and the header size field
constexpr uint32_t kFlagStreamed = 1;

// ------------------------------------------------------------------ writer

// Double-quoted YAML scalar. The text is printable ASCII (see fitsSanitize).
std::string yamlQuote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// Keywords are written unquoted when YAML cannot mistake them for anything but a string.
std::string yamlKeyword(const std::string& name) {
    bool plain = !name.empty() && name[0] >= 'A' && name[0] <= 'Z';
    for (char c : name) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) plain = false;
    }
    static const char* reserved[] = {"Y", "N", "YES", "NO", "TRUE", "FALSE", "ON", "OFF", "NULL"};
    for (const char* r : reserved)
        if (name == r) plain = false;
    return plain ? name : yamlQuote(name);
}

bool isFitsInteger(const std::string& v) {
    size_t i = (!v.empty() && (v[0] == '+' || v[0] == '-')) ? 1 : 0;
    if (i == v.size()) return false;
    for (; i < v.size(); ++i)
        if (v[i] < '0' || v[i] > '9') return false;
    return true;
}

// YAML 1.1 reads integers with a leading zero as octal, so the digits are normalized.
std::string yamlInteger(const std::string& v) {
    const bool neg = v[0] == '-';
    size_t i = (v[0] == '+' || v[0] == '-') ? 1 : 0;
    while (i + 1 < v.size() && v[i] == '0') ++i;
    const std::string digits = v.substr(i);
    return (neg && digits != "0" ? "-" : "") + digits;
}

// Rewrites a FITS real (1.5E3, 1D-5, .5, 3.) in the form YAML 1.1 parsers take for a float:
// digits on both sides of the point and a signed exponent.
bool yamlReal(const std::string& v, std::string& out) {
    size_t i = 0;
    std::string sign, whole, frac, exp;
    if (i < v.size() && (v[i] == '+' || v[i] == '-')) sign = v[i++] == '-' ? "-" : "";
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') whole += v[i++];
    if (i < v.size() && v[i] == '.') {
        ++i;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') frac += v[i++];
    }
    if (whole.empty() && frac.empty()) return false;
    if (i < v.size()) {
        const char e = v[i++];
        if (e != 'E' && e != 'e' && e != 'D' && e != 'd') return false;
        std::string esign = "+";
        if (i < v.size() && (v[i] == '+' || v[i] == '-')) esign = std::string(1, v[i++]);
        std::string digits;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') digits += v[i++];
        if (digits.empty() || i != v.size()) return false;
        exp = "e" + esign + digits;
    }
    out = sign + (whole.empty() ? "0" : whole) + "." + (frac.empty() ? "0" : frac) + exp;
    return true;
}

// ASDF limits integer literals in the tree to -(2^63 - 2) ... 2^63 - 1.
bool fitsAsdfInteger(const std::string& normalized) {
    const bool neg = normalized[0] == '-';
    const std::string digits = normalized.substr(neg ? 1 : 0);
    const std::string limit = neg ? "9223372036854775806" : "9223372036854775807";
    return digits.size() < limit.size() || (digits.size() == limit.size() && digits <= limit);
}

// Converts a FITS-formatted value ('text', T, 42, 1.5E3) to a YAML scalar of the same type.
std::string yamlValue(const std::string& fitsValue, const std::string& keyword) {
    const std::string v = trim(fitsSanitize(fitsValue));
    if (v.empty()) return "null";
    if (v[0] == '\'') return yamlQuote(fitsSanitize(fitsUnquote(fitsValue)));
    if (v == "T") return "true";
    if (v == "F") return "false";
    if (isFitsInteger(v)) {
        const std::string n = yamlInteger(v);
        if (fitsAsdfInteger(n)) return n;
        warn("keyword " + keyword + ": " + n + " is beyond the integer range of an ASDF tree; written as a string");
        return yamlQuote(n);
    }
    std::string real;
    if (yamlReal(v, real)) return real;
    if (v.size() > 2 && v.front() == '(' && v.back() == ')') {
        // Complex number: (real, imaginary)
        const auto parts = split(v.substr(1, v.size() - 2), ',');
        std::string re, im;
        if (parts.size() == 2 && yamlReal(trim(parts[0]), re) && yamlReal(trim(parts[1]), im)) {
            return "!core/complex-1.0.0 " + re + (im[0] == '-' ? "" : "+") + im + "j";
        }
    }
    return yamlQuote(v);
}

std::string headerEntry(const std::string& key, const std::string& value, const std::string& comment) {
    std::string e = "  - [" + yamlKeyword(key) + ", " + value;
    if (!comment.empty()) e += ", " + yamlQuote(comment);
    return e + "]\n";
}

void keywordEntry(const FitsKeyword& k, std::string& tree) {
    const std::string rawName = fitsSanitize(trim(k.name));
    const std::string name = toUpper(rawName);
    const std::string comment = trim(fitsSanitize(k.comment));
    if (name.empty() || name == "COMMENT" || name == "HISTORY") {
        std::string text = trim(fitsSanitize(k.value));
        if (!comment.empty()) text = text.empty() ? comment : text + " " + comment;
        if (name.empty() && text.empty()) return;
        tree += "  - [" + (name.empty() ? std::string("\"\"") : name) + ", " + yamlQuote(text) + "]\n";
        return;
    }
    // astropy takes a "HIERARCH " prefix as the request for a HIERARCH card.
    tree += headerEntry(isStandardFitsName(name) ? name : "HIERARCH " + rawName, yamlValue(k.value, rawName), comment);
}

const char* datatypeName(SampleFormat f) {
    switch (f) {
        case SampleFormat::UInt8: return "uint8";
        case SampleFormat::UInt16: return "uint16";
        case SampleFormat::UInt32: return "uint32";
        case SampleFormat::UInt64: return "uint64";
        case SampleFormat::Float32: return "float32";
        case SampleFormat::Float64: return "float64";
    }
    return "uint8";
}

void putBE(std::string& out, uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

// ------------------------------------------------------------------ reader

uint64_t getBE(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | p[i];
    return v;
}

struct Block {
    uint64_t dataPos = 0, allocated = 0, used = 0, dataSize = 0;
    std::string compression;
    uint8_t checksum[16] = {};
    bool hasChecksum = false;
    bool streamed = false;
};

enum class DKind { Unsigned, Signed, Float, Other };

struct DType {
    const char* name;
    size_t size;
    DKind kind;
};

const DType* findDatatype(const std::string& name) {
    static const DType types[] = {
        {"uint8", 1, DKind::Unsigned},   {"uint16", 2, DKind::Unsigned}, {"uint32", 4, DKind::Unsigned},
        {"uint64", 8, DKind::Unsigned},  {"int8", 1, DKind::Signed},     {"int16", 2, DKind::Signed},
        {"int32", 4, DKind::Signed},     {"int64", 8, DKind::Signed},    {"float16", 2, DKind::Float},
        {"float32", 4, DKind::Float},    {"float64", 8, DKind::Float},   {"bool8", 1, DKind::Other},
        {"complex64", 8, DKind::Other},  {"complex128", 16, DKind::Other},
    };
    for (const auto& t : types)
        if (name == t.name) return &t;
    return nullptr;
}

template <class S, class D>
void toFloat(std::vector<uint8_t>& data, size_t n) {
    std::vector<uint8_t> out(n * sizeof(D));
    for (size_t i = 0; i < n; ++i) {
        S v;
        std::memcpy(&v, data.data() + i * sizeof(S), sizeof(S));
        const D d = static_cast<D>(v);
        std::memcpy(out.data() + i * sizeof(D), &d, sizeof(D));
    }
    data.swap(out);
}

template <class T>
bool anyNegative(const std::vector<uint8_t>& data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        T v;
        std::memcpy(&v, data.data() + i * sizeof(T), sizeof(T));
        if (v < 0) return true;
    }
    return false;
}

void halfToFloat(std::vector<uint8_t>& data, size_t n) {
    std::vector<uint8_t> out(n * sizeof(float));
    for (size_t i = 0; i < n; ++i) {
        uint16_t h;
        std::memcpy(&h, data.data() + 2 * i, 2);
        const int exponent = (h >> 10) & 0x1F;
        const int mantissa = h & 0x3FF;
        float f;
        if (exponent == 0) f = std::ldexp(static_cast<float>(mantissa), -24);
        else if (exponent == 31) f = mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        else f = std::ldexp(static_cast<float>(mantissa + 1024), exponent - 25);
        if (h & 0x8000) f = -f;
        std::memcpy(out.data() + 4 * i, &f, 4);
    }
    data.swap(out);
}

// Signed integers are mapped as in the FITS reader: to the unsigned type of the same width
// when no sample is negative, to floating point otherwise.
template <class S, class F>
void mapSigned(FitsImage& img, size_t n, SampleFormat unsignedFormat, SampleFormat floatFormat, const char* name) {
    PixelBuffer& px = img.pixels;
    if (!anyNegative<S>(px.data, n)) {
        px.format = unsignedFormat;
        img.note = std::string(name) + ", no negative values -> " + sampleFormatName(unsignedFormat);
    } else {
        toFloat<S, F>(px.data, n);
        px.format = floatFormat;
        img.note = std::string(name) + " with negative values -> " + sampleFormatName(floatFormat);
    }
}

void decodeSamples(FitsImage& img, std::vector<uint8_t>& raw, const DType& dt, bool bigEndian, bool interleaved) {
    PixelBuffer& px = img.pixels;
    const size_t n = static_cast<size_t>(px.samples());
    if (dt.size > 1 && bigEndian == hostIsLittleEndian()) byteSwapInPlace(raw.data(), n, dt.size);
    px.data.swap(raw);
    const std::string name = dt.name;
    img.note = name;
    if (name == "uint8") px.format = SampleFormat::UInt8;
    else if (name == "uint16") px.format = SampleFormat::UInt16;
    else if (name == "uint32") px.format = SampleFormat::UInt32;
    else if (name == "uint64") px.format = SampleFormat::UInt64;
    else if (name == "float32") px.format = SampleFormat::Float32;
    else if (name == "float64") px.format = SampleFormat::Float64;
    else if (name == "float16") {
        halfToFloat(px.data, n);
        px.format = SampleFormat::Float32;
        img.note = "float16 -> Float32";
    } else if (name == "int8") mapSigned<int8_t, float>(img, n, SampleFormat::UInt8, SampleFormat::Float32, "int8");
    else if (name == "int16") mapSigned<int16_t, float>(img, n, SampleFormat::UInt16, SampleFormat::Float32, "int16");
    else if (name == "int32") mapSigned<int32_t, double>(img, n, SampleFormat::UInt32, SampleFormat::Float64, "int32");
    else if (name == "int64") mapSigned<int64_t, double>(img, n, SampleFormat::UInt64, SampleFormat::Float64, "int64");
    else throw Error("unsupported datatype " + name);

    if (interleaved && px.channels > 1) {
        // [row][column][channel] -> planar
        const size_t sb = sampleBytes(px.format);
        const size_t plane = static_cast<size_t>(px.planeSamples());
        const size_t channels = static_cast<size_t>(px.channels);
        std::vector<uint8_t> planar(px.data.size());
        for (size_t c = 0; c < channels; ++c) {
            uint8_t* dst = planar.data() + c * plane * sb;
            const uint8_t* src = px.data.data() + c * sb;
            for (size_t i = 0; i < plane; ++i) std::memcpy(dst + i * sb, src + i * channels * sb, sb);
        }
        px.data.swap(planar);
        img.note += ", [rows, columns, channels] layout";
    }
    updateFloatRange(img);
}

bool tagContains(const YamlNode& node, const char* what) { return node.tag.find(what) != std::string::npos; }

std::string scalarText(const YamlNode* node) {
    return node && node->isScalar() ? node->value : std::string();
}

bool scalarUInt(const YamlNode* node, uint64_t& out) {
    if (!node || !node->isScalar()) return false;
    const YamlValue v = yamlResolve(*node);
    return v.type == YamlValue::Type::Int && parseUInt64(v.text, out);
}

class Reader {
public:
    Reader(const std::string& path, bool headersOnly, bool verify, std::optional<size_t> onlyImage = std::nullopt)
        : in_(toPath(path), std::ios::binary), headersOnly_(headersOnly), verify_(verify), onlyImage_(onlyImage) {
        file_.path = path;
        if (!in_) throw Error("cannot open file", ErrorKind::Io);
        in_.seekg(0, std::ios::end);
        file_.fileSize = static_cast<uint64_t>(in_.tellg());
    }

    std::string treeText() { return readTree(); }

    // The tree as it is parsed: a NUL byte, which no valid tree holds, becomes a space, so that
    // names and values taken from it are whole for C callers.
    std::string treeWithoutNul() {
        std::string tree = readTree();
        std::replace(tree.begin(), tree.end(), '\0', ' ');
        return tree;
    }

    VerifyReport verifyAll() {
        VerifyReport report;
        const YamlPtr root = parseYaml(treeWithoutNul());
        scanBlocks();
        for (size_t i = 0; i < blocks_.size(); ++i) {
            const Block& b = blocks_[i];
            progress("verifying", i, blocks_.size());
            try {
                blockData(i, 0, b.compression.empty() ? b.used : b.dataSize, "block " + std::to_string(i));
                if (b.hasChecksum) ++report.verified;
                else ++report.unchecked;
            } catch (const Unsupported& e) {
                report.notChecked.push_back(e.what());
            } catch (const Error& e) {
                report.problems.push_back(e.what());
            }
        }
        // Every block the tree refers to must be there, and nothing but the block index may
        // follow the last block: a block whose header is cut off or damaged is not found above.
        uint64_t highest = 0;
        bool any = false;
        highestSource(root, highest, any);
        if (any && highest >= blocks_.size()) {
            report.problems.push_back("the tree refers to block " + std::to_string(highest) + ", but the file has " +
                                      std::to_string(blocks_.size()) + " block(s): truncated or damaged");
        }
        if (scanEnd_ < file_.fileSize) {
            const auto tail = readAt(scanEnd_, std::min<uint64_t>(file_.fileSize - scanEnd_, 4096));
            size_t i = 0;
            while (i < tail.size() && (tail[i] == 0 || tail[i] == '\n' || tail[i] == '\r' || tail[i] == ' ')) ++i;
            const std::string index = "#ASDF BLOCK INDEX";
            if (i < tail.size() && std::string(tail.begin() + static_cast<std::ptrdiff_t>(i), tail.end()).compare(0, index.size(), index) != 0) {
                report.problems.push_back(std::to_string(file_.fileSize - scanEnd_) + " bytes after block " +
                                          (blocks_.empty() ? std::string("area start") : std::to_string(blocks_.size() - 1)) +
                                          " are neither a block nor the block index: a damaged block header");
            }
        }
        report.summary = std::to_string(blocks_.size()) + (blocks_.size() == 1 ? " binary block" : " binary blocks");
        return report;
    }

    FitsFile run() {
        const YamlPtr root = parseYaml(treeWithoutNul());
        if (!root || !root->isMapping()) throw Error("the ASDF tree is not a mapping");
        scanBlocks();
        file_.formatNote += ", " + std::to_string(blocks_.size()) + " binary block(s)";
        for (const auto& pair : root->pairs) {
            const std::string key = scalarText(pair.first.get());
            if (key == "asdf_library" || key == "history") continue;
            walk(pair.second, key);
        }
        return std::move(file_);
    }

private:
    std::ifstream in_;
    FitsFile file_;
    bool headersOnly_, verify_;
    std::optional<size_t> onlyImage_;
    uint64_t treeEnd_ = 0;
    uint64_t scanEnd_ = 0;  // where the block scan stopped
    std::vector<Block> blocks_;
    std::set<const YamlNode*> visited_;

    std::vector<uint8_t> readAt(uint64_t pos, uint64_t n) {
        if (pos > file_.fileSize || n > file_.fileSize - pos) throw Error("unexpected end of file");
        if (n > std::numeric_limits<size_t>::max() / 2) throw Error("data block too large for this platform");
        std::vector<uint8_t> buf(static_cast<size_t>(n));
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(pos));
        if (n && !in_.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n))) throw Error("read error", ErrorKind::Io);
        return buf;
    }

    // Returns the YAML tree, the text part of the file.
    std::string readTree() {
        constexpr uint64_t kChunk = 1u << 20;
        constexpr uint64_t kMaxTree = 1u << 30;
        std::string head;
        size_t searchFrom = 0, yamlStart = 0;
        bool found = false;
        while (!found && head.size() < file_.fileSize) {
            const size_t old = head.size();
            const auto more = readAt(old, std::min<uint64_t>(kChunk, file_.fileSize - old));
            head.append(reinterpret_cast<const char*>(more.data()), more.size());
            const bool complete = head.size() >= file_.fileSize;
            if (old == 0) {
                if (head.compare(0, 5, "#ASDF") != 0) throw Error("not an ASDF file");
                // Comment lines carry the format and standard versions.
                while (yamlStart < head.size() && head[yamlStart] == '#') {
                    size_t e = head.find('\n', yamlStart);
                    if (e == std::string::npos) e = head.size();
                    const std::string line = trim(head.substr(yamlStart, e - yamlStart));
                    if (startsWith(line, "#ASDF ")) file_.formatNote = "ASDF " + trim(line.substr(6));
                    else if (startsWith(line, "#ASDF_STANDARD ")) file_.formatNote += ", standard " + trim(line.substr(15));
                    yamlStart = e < head.size() ? e + 1 : e;
                }
                if (file_.formatNote.empty()) file_.formatNote = "ASDF";
                if (head.compare(yamlStart, 4, kBlockMagic, 4) == 0 || yamlStart >= head.size()) {
                    throw Error("this ASDF file has no tree (it holds binary blocks only)");
                }
                searchFrom = yamlStart ? yamlStart - 1 : 0;
            }
            // The tree ends with a "..." line.
            for (;;) {
                const size_t i = head.find("\n...", searchFrom);
                if (i == std::string::npos) {
                    searchFrom = head.size() > 3 ? head.size() - 3 : 0;
                    break;
                }
                const size_t e = i + 4;
                if (e >= head.size() || (head[e] == '\r' && e + 1 >= head.size())) {
                    if (complete) {
                        treeEnd_ = head.size();
                        found = true;
                    } else {
                        searchFrom = i;
                    }
                    break;
                }
                if (head[e] == '\n' || (head[e] == '\r' && head[e + 1] == '\n')) {
                    treeEnd_ = e + (head[e] == '\n' ? 1 : 2);
                    found = true;
                    break;
                }
                searchFrom = i + 1;
            }
            if (!found && head.size() > kMaxTree) throw Error("the YAML tree is larger than 1 GiB");
        }
        if (!found) throw Error("the end of the YAML tree was not found (truncated file?)");
        return head.substr(yamlStart, static_cast<size_t>(treeEnd_) - yamlStart);
    }

    // Reads the headers of the binary blocks that follow the tree.
    void scanBlocks() {
        uint64_t pos = treeEnd_;
        // Padding may separate the tree from the first block.
        while (pos < file_.fileSize) {
            const auto buf = readAt(pos, std::min<uint64_t>(65536, file_.fileSize - pos));
            size_t i = 0;
            while (i < buf.size() && (buf[i] == 0 || buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ')) ++i;
            pos += i;
            if (i < buf.size()) break;
        }
        scanEnd_ = pos;
        while (file_.fileSize - pos >= 6 + kBlockHeaderSize) {
            const auto start = readAt(pos, 6);
            if (std::memcmp(start.data(), kBlockMagic, 4) != 0) break;  // block index or end of file
            const std::string label = "block " + std::to_string(blocks_.size());
            const uint64_t headerSize = getBE(start.data() + 4, 2);
            if (headerSize < kBlockHeaderSize) throw Error(label + ": invalid header size");
            const auto h = readAt(pos + 6, kBlockHeaderSize);
            Block b;
            const uint32_t flags = static_cast<uint32_t>(getBE(h.data(), 4));
            for (int i = 4; i < 8 && h[i] != 0; ++i) b.compression += static_cast<char>(h[i]);
            b.compression = trim(b.compression);
            b.allocated = getBE(h.data() + 8, 8);
            b.used = getBE(h.data() + 16, 8);
            b.dataSize = getBE(h.data() + 24, 8);
            std::memcpy(b.checksum, h.data() + 32, 16);
            for (uint8_t c : b.checksum)
                if (c) b.hasChecksum = true;
            b.dataPos = pos + 6 + headerSize;
            if (b.dataPos > file_.fileSize) throw Error(label + ": header extends beyond the end of the file");
            if (flags & kFlagStreamed) {
                // A streamed block is the last one and runs to the end of the file.
                b.allocated = b.used = b.dataSize = file_.fileSize - b.dataPos;
                b.hasChecksum = false;
                b.streamed = true;
                blocks_.push_back(b);
                scanEnd_ = file_.fileSize;
                break;
            }
            if (b.used > b.allocated) throw Error(label + ": used size exceeds allocated size");
            if (b.allocated > file_.fileSize - b.dataPos) {
                throw Error(label + ": data extends beyond the end of the file (truncated?)");
            }
            blocks_.push_back(b);
            if (blocks_.size() > 1000000) throw Error("too many binary blocks");
            pos = b.dataPos + b.allocated;
            scanEnd_ = pos;
        }
    }

    // Highest block index that an array in the tree refers to.
    void highestSource(const YamlPtr& node, uint64_t& highest, bool& any) {
        if (!node || !visited_.insert(node.get()).second) return;
        if (node->isMapping()) {
            uint64_t index = 0;
            if (tagContains(*node, "/core/ndarray-") && scalarUInt(node->get("source"), index)) {
                highest = any ? std::max(highest, index) : index;
                any = true;
            }
            for (const auto& pair : node->pairs) highestSource(pair.second, highest, any);
        } else if (node->isSequence()) {
            for (const auto& item : node->items) highestSource(item, highest, any);
        }
    }

    bool checksumMatches(const Block& b, const std::vector<uint8_t>& bytes) const {
        uint8_t digest[16];
        md5(bytes.data(), bytes.size(), digest);
        return std::memcmp(digest, b.checksum, 16) == 0;
    }

    [[noreturn]] static void checksumError(size_t index) {
        throw Error("block " + std::to_string(index) + ": MD5 checksum mismatch (the file is damaged; "
                    "--no-verify skips this check)", ErrorKind::Checksum);
    }

    // Returns `count` bytes at `offset` of the (decompressed) data of a block.
    std::vector<uint8_t> blockData(size_t index, uint64_t offset, uint64_t count, const std::string& label) {
        const Block& b = blocks_[index];
        const std::string where = "block " + std::to_string(index);
        const uint64_t available = b.compression.empty() ? b.used : b.dataSize;
        if (offset > available || count > available - offset) {
            throw Error(label + ": the array needs " + std::to_string(count) + " bytes at offset " +
                        std::to_string(offset) + ", but " + where + " holds " + std::to_string(available));
        }
        std::vector<uint8_t> data;
        if (b.compression.empty()) {
            if (!verify_ || !b.hasChecksum) return readAt(b.dataPos + offset, count);
            data = readAt(b.dataPos, b.used);
            if (!checksumMatches(b, data)) checksumError(index);
        } else {
            if (b.compression == "bzp2") {
                throw Unsupported(where + ": bzip2 compression is not supported; rewrite the file with zlib or no compression");
            }
            if (b.compression == "zstd" && !zstdAvailable()) {
                throw Unsupported(where + ": zstd compression, but this build of xisfconv has no Zstandard support");
            }
            if (b.compression != "zlib" && b.compression != "zstd" && b.compression != "lz4") {
                throw Unsupported(where + ": compression '" + b.compression + "' is not supported");
            }
            const auto stored = readAt(b.dataPos, b.used);
            // The checksum of a compressed block covers the stored bytes (the standard, and the
            // asdf library since 3.0) or the uncompressed data (asdf 2.x); both are accepted.
            const bool check = verify_ && b.hasChecksum;
            const bool storedOk = check && checksumMatches(b, stored);
            if (b.dataSize > std::numeric_limits<size_t>::max() / 2) throw Error("data block too large for this platform");
            const size_t size = static_cast<size_t>(b.dataSize);
            // A header that declares more data than the stored bytes can expand to is damaged;
            // catching it here avoids allocating the declared size.
            uint64_t limit = std::numeric_limits<uint64_t>::max();
            if (b.compression == "zlib") limit = b.used * 1032 + 1024;
            else if (b.compression == "lz4") limit = b.used * 255 + 1024;
            else if (b.compression == "zstd" && !zstdFrameContentSize(stored.data(), stored.size(), limit)) {
                limit = b.used * 50000 + (1u << 20);  // RLE blocks reach about 43000:1
            }
            if (b.dataSize > limit) {
                throw Error(where + ": the header declares " + std::to_string(b.dataSize) + " bytes of data, more than its " +
                            std::to_string(b.used) + " compressed bytes can hold (damaged file?)");
            }
            try {
                if (b.compression == "zlib") data = zlibDecompress(stored.data(), stored.size(), size);
                else if (b.compression == "zstd") data = zstdDecompress(stored.data(), stored.size(), size);
                else data = lz4Chunks(stored, size);
            } catch (const Error& e) {
                if (check && !storedOk) checksumError(index);  // damaged data rarely decompresses
                throw Error(where + ": " + e.what(), e.kind);
            }
            if (check && !storedOk && !checksumMatches(b, data)) checksumError(index);
        }
        if (offset == 0 && count == data.size()) return data;
        return std::vector<uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                    data.begin() + static_cast<std::ptrdiff_t>(offset + count));
    }

    // The asdf library's LZ4 layout: chunks of [compressed size, 4 bytes big-endian]
    // [uncompressed size, 4 bytes little-endian][LZ4 block].
    static std::vector<uint8_t> lz4Chunks(const std::vector<uint8_t>& stored, size_t size) {
        std::vector<uint8_t> out;
        out.reserve(std::min<size_t>(size, stored.size() * 255 + 64));
        size_t p = 0;
        while (p < stored.size()) {
            if (stored.size() - p < 8) throw Error("lz4: truncated chunk header");
            const uint64_t clen = getBE(stored.data() + p, 4);
            const uint64_t ulen = stored[p + 4] | (stored[p + 5] << 8) | (stored[p + 6] << 16) |
                                  (static_cast<uint64_t>(stored[p + 7]) << 24);
            p += 4;
            if (clen < 4 || clen > stored.size() - p) throw Error("lz4: invalid chunk size");
            if (ulen > size - out.size()) throw Error("lz4: more data than the block header declares");
            const auto chunk = lz4BlockDecompress(stored.data() + p + 4, static_cast<size_t>(clen - 4), static_cast<size_t>(ulen));
            out.insert(out.end(), chunk.begin(), chunk.end());
            p += static_cast<size_t>(clen);
        }
        if (out.size() != size) {
            throw Error("lz4: decompressed " + std::to_string(out.size()) + " bytes, expected " + std::to_string(size));
        }
        return out;
    }

    // Converts "1.5-2.5j" (core/complex) to the FITS notation (1.5, -2.5).
    static bool fitsComplex(std::string text, std::string& out) {
        text = trim(text);
        if (text.size() > 1 && text.front() == '(' && text.back() == ')') text = trim(text.substr(1, text.size() - 2));
        if (text.empty()) return false;
        std::string re = "0", im = "0";
        const char last = text.back();
        if (last == 'j' || last == 'J' || last == 'i' || last == 'I') {
            text.pop_back();
            size_t cut = std::string::npos;
            for (size_t i = text.size(); i-- > 1;) {
                if ((text[i] == '+' || text[i] == '-') && text[i - 1] != 'e' && text[i - 1] != 'E') {
                    cut = i;
                    break;
                }
            }
            if (cut == std::string::npos) {
                im = text;
            } else {
                re = text.substr(0, cut);
                im = text.substr(cut);
            }
        } else {
            re = text;
        }
        double a = 0, b = 0;
        if (!parseDouble(re, a) || !parseDouble(im, b) || !std::isfinite(a) || !std::isfinite(b)) return false;
        auto real = [](double v) {
            std::string t = toUpper(formatDouble(v));
            if (t.find_first_of(".E") == std::string::npos) t += ".0";
            return t;
        };
        out = "(" + real(a) + ", " + real(b) + ")";
        return true;
    }

    // Converts a header value to the FITS notation used for keywords. Returns false for
    // values a FITS card cannot hold.
    static bool fitsValue(const YamlNode& node, std::string& out) {
        if (!node.isScalar()) return false;
        if (tagContains(node, "/core/complex-") && fitsComplex(node.value, out)) return true;
        const YamlValue v = yamlResolve(node);
        switch (v.type) {
            case YamlValue::Type::Null:
                out.clear();
                break;
            case YamlValue::Type::Bool:
                out = v.boolean ? "T" : "F";
                break;
            case YamlValue::Type::Int:
                out = v.text;
                break;
            case YamlValue::Type::Float:
                if (!std::isfinite(v.number)) {
                    out = fitsString(std::isnan(v.number) ? "NaN" : v.number > 0 ? "Inf" : "-Inf");
                } else {
                    out = toUpper(v.text);
                    const size_t first = out[0] == '-' ? 1 : 0;
                    if (out[first] == '.') out.insert(first, "0");
                    if (out.find_first_of(".E") == std::string::npos) out += ".0";
                }
                break;
            case YamlValue::Type::String:
                out = fitsString(v.text);
                break;
        }
        return true;
    }

    // Reads the [keyword, value, comment] entries of a FITS-tagged header.
    void readHeader(const YamlNode& header, FitsImage& img, const std::string& label) {
        if (!header.isSequence()) return;
        for (const auto& entry : header.items) {
            if (!entry || !entry->isSequence() || entry->items.empty()) continue;
            const auto& items = entry->items;
            const std::string rawName = trim(scalarText(items[0].get()));
            std::string name = toUpper(rawName);
            // Names that do not fit a standard card keep their case (HIERARCH convention).
            if (startsWith(name, "HIERARCH ")) name = trim(rawName.substr(9));
            else if (!name.empty() && !isStandardFitsName(name)) name = rawName;
            const std::string comment = items.size() > 2 ? trim(scalarText(items[2].get())) : std::string();
            if (name.empty() || name == "COMMENT" || name == "HISTORY") {
                std::string text = items.size() > 1 ? scalarText(items[1].get()) : std::string();
                if (!comment.empty()) text = text.empty() ? comment : text + " " + comment;
                if (name.empty() && trim(text).empty()) continue;
                img.keywords.push_back({name, "", text});
                continue;
            }
            std::string value;
            if (items.size() > 1 && !(items[1] && fitsValue(*items[1], value))) {
                warn(label + ": keyword " + name + " has a value that is not a scalar; skipped");
                continue;
            }
            if (name == "ROWORDER") {
                img.hasRowOrder = true;
                img.topDown = toUpper(fitsUnquote(value)) == "TOP-DOWN";
            }
            if (name == "EXTNAME" || name == "HDUNAME") {
                if (img.name.empty() || name == "EXTNAME") img.name = fitsUnquote(value);
                continue;
            }
            if (isReservedFitsKeyword(name) || name == "CONTINUE") continue;
            img.keywords.push_back({name, value, comment});
        }
    }

    // Adds the array to the images if it is a block-stored numeric array of 2 or 3 dimensions.
    void addArray(const YamlNode& node, const std::string& path, FitsImage img) {
        const bool hdu = !img.generic;
        auto skip = [&](const std::string& why) { file_.skipped.push_back(path + ": " + why); };

        // Shape first: generic arrays that are not images are passed over without comment.
        std::vector<uint64_t> dims;
        bool streamedAxis = false;
        const YamlNode* shape = node.get("shape");
        if (shape && shape->isSequence()) {
            for (size_t i = 0; i < shape->items.size(); ++i) {
                const YamlNode* item = shape->items[i].get();
                uint64_t d = 0;
                if (i == 0 && scalarText(item) == "*") streamedAxis = true;
                else if (!scalarUInt(item, d)) throw Error(path + ": invalid shape");
                dims.push_back(d);
            }
        }
        const YamlNode* source = node.get("source");
        if (!node.isMapping() || !source) {
            if (hdu || dims.size() >= 2) skip("the array is stored inline in the tree, not in a binary block");
            return;
        }
        if (!shape || !shape->isSequence()) throw Error(path + ": the array has no shape");
        size_t leading = 0;
        while (dims.size() - leading > 2 && dims[leading] == 1 && !(leading == 0 && streamedAxis)) ++leading;
        const size_t ndim = dims.size() - leading;
        if (ndim < 2) {
            if (hdu) skip(std::to_string(ndim) + "-dimensional data is not an image");
            return;
        }
        if (ndim > 3) {
            skip(std::to_string(ndim) + "-dimensional data is not supported");
            return;
        }

        uint64_t blockIndex = 0;
        if (source->isScalar() && yamlResolve(*source).type == YamlValue::Type::Int && yamlResolve(*source).text == "-1") {
            // The asdf library's reference to the streamed block, the last one of the file.
            if (blocks_.empty() || !blocks_.back().streamed) throw Error(path + ": streamed array, but the file has no streamed block");
            blockIndex = blocks_.size() - 1;
        } else if (!scalarUInt(source, blockIndex)) {
            skip("the array is stored in another file");
            return;
        }
        const YamlNode* datatype = node.get("datatype");
        const DType* dt = datatype && datatype->isScalar() ? findDatatype(datatype->value) : nullptr;
        if (!dt || dt->kind == DKind::Other) {
            skip("datatype " + (datatype && datatype->isScalar() ? datatype->value : std::string("(structured)")) +
                 " is not image data");
            return;
        }
        const bool bigEndian = scalarText(node.get("byteorder")) == "big";
        uint64_t offset = 0;
        if (const YamlNode* o = node.get("offset")) {
            if (!scalarUInt(o, offset)) throw Error(path + ": invalid offset");
        }
        if (blockIndex >= blocks_.size()) {
            throw Error(path + ": the array is in block " + std::to_string(blockIndex) + ", but the file has " +
                        std::to_string(blocks_.size()) + " block(s)");
        }
        const Block& block = blocks_[static_cast<size_t>(blockIndex)];
        if (streamedAxis) {
            uint64_t rest = dt->size;
            for (size_t i = 1; i < dims.size(); ++i) rest = checkedMul(rest, dims[i], "array size");
            const uint64_t available = block.compression.empty() ? block.used : block.dataSize;
            dims[0] = rest && available > offset ? (available - offset) / rest : 0;
        }
        uint64_t elements = 1;
        for (uint64_t d : dims) elements = checkedMul(elements, d, "array size");
        if (elements == 0) {
            skip("empty array");
            return;
        }
        if (const YamlNode* strides = node.get("strides")) {
            // Only the default layout (C order, contiguous) is read.
            bool contiguous = strides->isSequence() && strides->items.size() == dims.size();
            uint64_t expected = dt->size;
            for (size_t i = dims.size(); contiguous && i-- > 0;) {
                uint64_t s = 0;
                contiguous = scalarUInt(strides->items[i].get(), s) && s == expected;
                expected = checkedMul(expected, dims[i], "array size");
            }
            if (!contiguous) {
                skip("the array is a non-contiguous view or not in C order (strides); rewrite it as a plain array");
                return;
            }
        }

        PixelBuffer& px = img.pixels;
        bool interleaved = false;
        if (ndim == 2) {
            px.height = dims[leading];
            px.width = dims[leading + 1];
            px.channels = 1;
        } else if (!hdu && dims[leading + 2] <= 4 && dims[leading] > 4) {
            // [rows, columns, channels], as image libraries store RGB(A) data
            px.height = dims[leading];
            px.width = dims[leading + 1];
            px.channels = dims[leading + 2];
            interleaved = true;
        } else {
            px.channels = dims[leading];
            px.height = dims[leading + 1];
            px.width = dims[leading + 2];
        }
        img.hduIndex = file_.images.size();
        img.source = path;
        if (!hdu) img.name = path;
        img.storage = std::string(dt->name) + (dt->size > 1 ? (bigEndian ? ", big-endian" : ", little-endian") : "") +
                      ", block " + std::to_string(blockIndex) +
                      (block.compression.empty() ? "" : ", " + block.compression + " compressed");
        img.note = dt->name;
        if (!headersOnly_ && (!onlyImage_ || *onlyImage_ == file_.images.size())) {
            auto raw = blockData(static_cast<size_t>(blockIndex), offset, checkedMul(elements, dt->size, "array size"), path);
            decodeSamples(img, raw, *dt, bigEndian, interleaved);
            img.hasData = true;
        }
        file_.images.push_back(std::move(img));
    }

    void addHduList(const YamlNode& list, const std::string& path) {
        for (size_t i = 0; i < list.items.size(); ++i) {
            const YamlNode* hdu = list.items[i].get();
            const std::string label = path + "[" + std::to_string(i) + "]";
            if (!hdu || !hdu->isMapping()) continue;
            FitsImage img;
            if (const YamlNode* header = hdu->get("header")) readHeader(*header, img, label);
            const YamlNode* data = hdu->get("data");
            if (!data || (data->isScalar() && yamlResolve(*data).type == YamlValue::Type::Null)) {
                if (i > 0) file_.skipped.push_back(label + ": no image data");
                continue;
            }
            visited_.insert(data);
            if (tagContains(*data, "/table")) {
                file_.skipped.push_back(label + ": table (not an image)");
                continue;
            }
            addArray(*data, label + ".data", std::move(img));
        }
    }

    void walk(const YamlPtr& node, const std::string& path) {
        if (!node || !visited_.insert(node.get()).second) return;
        if (node->isSequence() && tagContains(*node, "/fits/fits-")) {
            addHduList(*node, path);
            return;
        }
        if (tagContains(*node, "/core/ndarray-")) {
            FitsImage img;
            img.generic = true;
            addArray(*node, path, std::move(img));
            return;
        }
        if (node->isMapping()) {
            for (const auto& pair : node->pairs) {
                const std::string key = scalarText(pair.first.get());
                walk(pair.second, path.empty() ? key : path + "." + key);
            }
        } else if (node->isSequence()) {
            for (size_t i = 0; i < node->items.size(); ++i) walk(node->items[i], path + "[" + std::to_string(i) + "]");
        }
    }
};

}  // namespace

void writeAsdf(const std::string& path, const std::vector<FitsHdu>& hdus, const AsdfWriteOptions& options) {
    std::ofstream out(toPath(path), std::ios::binary | std::ios::trunc);
    if (!out) throw Error("cannot create " + path, ErrorKind::Io);

    std::string tree = "#ASDF 1.0.0\n"
                       "#ASDF_STANDARD 1.5.0\n"
                       "%YAML 1.1\n"
                       "%TAG ! tag:stsci.edu:asdf/\n"
                       "--- !core/asdf-1.1.0\n"
                       "asdf_library: !core/software-1.0.0 {author: Jurgen Kobierczynski, "
                       "homepage: 'https://github.com/jkobierczynski/xisfconv',\n"
                       "  name: xisfconv, version: \"" + std::string(kVersion) + "\"}\n"
                       "fits: !<tag:astropy.org:astropy/fits/fits-1.0.0>\n";
    for (size_t h = 0; h < hdus.size(); ++h) {
        const FitsHdu& hdu = hdus[h];
        const PixelBuffer& px = *hdu.pixels;
        tree += "- header:\n";
        if (h == 0) {
            tree += headerEntry("PROGRAM", yamlQuote(std::string("xisfconv ") + kVersion), "software that created this HDU");
        }
        const std::string extname = trim(fitsSanitize(hdu.extname));
        if (!extname.empty()) tree += headerEntry("EXTNAME", yamlQuote(extname), "image identifier");
        tree += headerEntry("ROWORDER", yamlQuote(hdu.bottomUp ? "BOTTOM-UP" : "TOP-DOWN"), "order of image rows");
        for (const auto& k : hdu.keywords) {
            const std::string name = toUpper(trim(k.name));
            if (isReservedFitsKeyword(name) || name == "CONTINUE" || name == "LONGSTRN") continue;
            if (!extname.empty() && name == "EXTNAME") continue;
            if (h == 0 && name == "PROGRAM") continue;
            keywordEntry(k, tree);
        }
        tree += "  data: !core/ndarray-1.0.0\n"
                "    source: " + std::to_string(h) + "\n"
                "    datatype: " + datatypeName(px.format) + "\n"
                "    byteorder: " + (hostIsLittleEndian() ? "little" : "big") + "\n"
                "    shape: [";
        if (px.channels > 1) tree += std::to_string(px.channels) + ", ";
        tree += std::to_string(px.height) + ", " + std::to_string(px.width) + "]\n";
    }
    tree += "...\n";
    out.write(tree.data(), static_cast<std::streamsize>(tree.size()));

    uint64_t pos = tree.size();
    std::vector<uint64_t> offsets;
    for (const auto& hdu : hdus) {
        const PixelBuffer& px = *hdu.pixels;
        const size_t bytes = static_cast<size_t>(px.samples()) * sampleBytes(px.format);
        std::vector<uint8_t> packed;
        const uint8_t* stored = px.data.data();
        size_t storedSize = bytes;
        if (!options.codec.empty()) {
            if (options.codec == "zlib") packed = zlibCompress(px.data.data(), bytes);
            else if (options.codec == "zstd") packed = zstdCompress(px.data.data(), bytes);
            else throw Error("ASDF output: unknown codec " + options.codec);
            stored = packed.data();
            storedSize = packed.size();
        }
        uint8_t digest[16];
        md5(stored, storedSize, digest);

        std::string header(kBlockMagic, 4);
        putBE(header, kBlockHeaderSize, 2);
        putBE(header, 0, 4);  // flags
        std::string label = options.codec;
        label.resize(4, '\0');
        header += label;
        putBE(header, storedSize, 8);  // allocated
        putBE(header, storedSize, 8);  // used
        putBE(header, bytes, 8);       // size of the uncompressed data
        header.append(reinterpret_cast<const char*>(digest), 16);
        offsets.push_back(pos);
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.write(reinterpret_cast<const char*>(stored), static_cast<std::streamsize>(storedSize));
        if (!out) throw Error("write error on " + path, ErrorKind::Io);
        pos += header.size() + storedSize;
    }

    std::string index = "#ASDF BLOCK INDEX\n%YAML 1.1\n---\n";
    for (uint64_t o : offsets) index += "- " + std::to_string(o) + "\n";
    index += "...\n";
    out.write(index.data(), static_cast<std::streamsize>(index.size()));
    out.close();
    if (!out) throw Error("write error on " + path, ErrorKind::Io);
}

FitsFile readAsdf(const std::string& path, bool headersOnly, bool verifyChecksums, std::optional<size_t> onlyImage) {
    Reader reader(path, headersOnly, verifyChecksums, onlyImage);
    return reader.run();
}

VerifyReport verifyAsdf(const std::string& path) {
    Reader reader(path, false, true);
    return reader.verifyAll();
}

std::string readAsdfTree(const std::string& path) {
    Reader reader(path, true, false);
    return reader.treeText();
}

bool looksLikeAsdf(const std::string& path) {
    std::ifstream in(toPath(path), std::ios::binary);
    char buf[5] = {};
    in.read(buf, 5);
    return in.gcount() == 5 && std::string(buf, 5) == "#ASDF";
}

}  // namespace xisfconv
