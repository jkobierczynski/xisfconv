// DNG reader. See dng.hpp.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "dng.hpp"

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

#include "codecs.hpp"
#include "fits.hpp"

namespace xisfconv {

// ------------------------------------------------------------------------------------------
// Lossless JPEG
// ------------------------------------------------------------------------------------------

namespace {

// A Huffman table of the lossless process: the symbols are the categories of a difference, 0 to 16.
struct HuffmanTable {
    bool defined = false;
    uint8_t fastLength[512] = {};   // codes of up to 9 bits, by their first 9 bits (0: longer)
    uint8_t fastSymbol[512] = {};
    int32_t maxCode[17] = {};       // the largest code of each length, -1 for none
    int32_t firstCode[17] = {};
    int32_t firstIndex[17] = {};
    std::vector<uint8_t> symbols;
};

void buildTable(HuffmanTable& t, const uint8_t counts[16], const uint8_t* symbols, size_t total) {
    t = HuffmanTable();
    t.symbols.assign(symbols, symbols + total);
    for (uint8_t s : t.symbols)
        if (s > 16) throw Error("lossless JPEG: a Huffman table with a symbol for a difference of more than 16 bits");
    uint32_t code = 0;
    size_t index = 0;
    for (int length = 1; length <= 16; ++length) {
        const uint8_t n = counts[length - 1];
        t.firstCode[length] = static_cast<int32_t>(code);
        t.firstIndex[length] = static_cast<int32_t>(index);
        // (checked before the codes are entered: more codes of a length than there are would
        // reach beyond the table of 9-bit codes)
        if (code + n > (1u << length)) throw Error("lossless JPEG: a Huffman table that is no prefix code");
        for (uint8_t k = 0; k < n; ++k, ++code, ++index) {
            if (length <= 9) {
                const uint32_t start = code << (9 - length), stop = (code + 1) << (9 - length);
                for (uint32_t e = start; e < stop; ++e) {
                    t.fastLength[e] = static_cast<uint8_t>(length);
                    t.fastSymbol[e] = t.symbols[index];
                }
            }
        }
        t.maxCode[length] = n ? static_cast<int32_t>(code) - 1 : -1;
        code <<= 1;
    }
    t.defined = true;
}

// The entropy-coded data: bits, first the highest of each byte, with the 0x00 that follows a
// 0xFF taken out. At a marker the data ends; what is asked for beyond it is an error.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size, size_t at) : data_(data), size_(size), pos_(at) {}

    uint32_t peek(int n) {
        fill();
        return static_cast<uint32_t>(acc_ >> (64 - n));
    }
    void skip(int n) {
        acc_ <<= n;
        count_ -= n;
        if (count_ < padding_) throw Error("lossless JPEG: the compressed data ends before the image does");
    }
    uint32_t take(int n) {
        const uint32_t v = peek(n);
        skip(n);
        return v;
    }
    // After a restart interval or the scan: the bits up to the next byte are fill bits, and a
    // marker must follow. Returns its code, 0 at the end of the data.
    int marker() {
        const int real = count_ - padding_;
        if (real >= 8 || (!atMarker_ && pos_ < size_)) throw Error("lossless JPEG: data beyond the end of a restart interval or of the scan");
        acc_ = 0;
        count_ = padding_ = 0;
        atMarker_ = false;
        while (pos_ < size_ && data_[pos_] == 0xFF) ++pos_;   // (fill bytes 0xFF may stand before a marker)
        if (pos_ >= size_) return 0;
        return data_[pos_++];
    }
    size_t position() const { return pos_; }

private:
    // Fills the accumulator to more than 56 bits. A byte of the data is real; once a marker or the
    // end of the data is reached, zero bytes stand in, counted as padding.
    void fill() {
        while (count_ <= 56) {
            uint8_t b = 0;
            bool real = false;
            if (!atMarker_ && pos_ < size_) {
                if (data_[pos_] != 0xFF) {
                    b = data_[pos_++];
                    real = true;
                } else if (pos_ + 1 < size_ && data_[pos_ + 1] == 0x00) {
                    b = 0xFF;
                    pos_ += 2;
                    real = true;
                } else {
                    atMarker_ = true;
                }
            }
            if (!real) padding_ += 8;
            acc_ |= static_cast<uint64_t>(b) << (56 - count_);
            count_ += 8;
        }
    }

    const uint8_t* data_;
    size_t size_, pos_;
    uint64_t acc_ = 0;
    int count_ = 0;     // bits in acc_
    int padding_ = 0;   // of them, at the end: bits that are not data (after a marker, or after the end)
    bool atMarker_ = false;
};

}  // namespace

LosslessJpeg decodeLosslessJpeg(const uint8_t* data, size_t size, uint64_t maxSamples) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) throw Error("lossless JPEG: no start of image");
    HuffmanTable tables[4];
    struct Component { int id = 0; int table = 0; };
    std::vector<Component> components;
    LosslessJpeg out;
    unsigned restartInterval = 0;
    bool frame = false;
    size_t pos = 2;
    while (true) {
        if (pos + 4 > size) throw Error("lossless JPEG: the stream ends before its scan");
        if (data[pos] != 0xFF) throw Error("lossless JPEG: a marker was expected");
        while (pos < size && data[pos] == 0xFF) ++pos;
        if (pos + 3 > size) throw Error("lossless JPEG: the stream ends before its scan");
        const int code = data[pos++];
        const size_t length = static_cast<size_t>(data[pos]) << 8 | data[pos + 1];
        if (length < 2 || pos + length > size) throw Error("lossless JPEG: a segment runs beyond the end of the stream");
        const uint8_t* s = data + pos + 2;
        const size_t n = length - 2;
        pos += length;
        if (code == 0xC4) {   // DHT
            size_t at = 0;
            while (at < n) {
                if (at + 17 > n) throw Error("lossless JPEG: a Huffman table is cut short");
                const int cls = s[at] >> 4, id = s[at] & 15;
                if (cls != 0 || id > 3) throw Error("lossless JPEG: a Huffman table of another class or number");
                size_t total = 0;
                for (int k = 0; k < 16; ++k) total += s[at + 1 + k];
                if (at + 17 + total > n || total > 17) throw Error("lossless JPEG: a Huffman table is cut short or too long");
                buildTable(tables[id], s + at + 1, s + at + 17, total);
                at += 17 + total;
            }
        } else if (code == 0xC3) {   // SOF3: lossless, Huffman
            if (n < 6) throw Error("lossless JPEG: the frame header is cut short");
            out.precision = s[0];
            out.height = static_cast<unsigned>(s[1]) << 8 | s[2];
            out.width = static_cast<unsigned>(s[3]) << 8 | s[4];
            out.components = s[5];
            if (out.precision < 2 || out.precision > 16) throw Error("lossless JPEG: a precision of " + std::to_string(out.precision) + " bits");
            if (out.height == 0) throw Unsupported("lossless JPEG whose height follows the scan (DNL)");
            if (out.width == 0 || out.components == 0 || out.components > 4 || n < 6 + 3u * out.components) {
                throw Error("lossless JPEG: a frame without width or components");
            }
            components.resize(out.components);
            for (unsigned c = 0; c < out.components; ++c) {
                components[c].id = s[6 + 3 * c];
                if (s[7 + 3 * c] != 0x11) throw Unsupported("lossless JPEG with subsampled components");
            }
            frame = true;
        } else if (code == 0xDD) {   // DRI
            if (n < 2) throw Error("lossless JPEG: the restart interval is cut short");
            restartInterval = static_cast<unsigned>(s[0]) << 8 | s[1];
        } else if (code == 0xDA) {   // SOS: the scan
            if (!frame) throw Error("lossless JPEG: a scan before the frame header");
            if (n < 1 || n < 4 + 2u * s[0]) throw Error("lossless JPEG: the scan header is cut short");
            if (s[0] != out.components) throw Unsupported("lossless JPEG with more than one scan");
            for (unsigned k = 0; k < out.components; ++k) {
                const int id = s[1 + 2 * k], table = s[2 + 2 * k] >> 4;
                if (components[k].id != id) throw Unsupported("lossless JPEG whose scan has its components in another order");
                if (table > 3 || !tables[table].defined) throw Error("lossless JPEG: a component without a Huffman table");
                components[k].table = table;
            }
            const int predictor = s[1 + 2 * out.components];
            const int transform = s[3 + 2 * out.components] & 15;
            if (predictor < 1 || predictor > 7) throw Error("lossless JPEG: predictor " + std::to_string(predictor));
            if (transform >= static_cast<int>(out.precision)) throw Error("lossless JPEG: a point transform as large as the precision");
            const uint64_t total = static_cast<uint64_t>(out.width) * out.height * out.components;
            if (total > maxSamples) throw Error("lossless JPEG: " + std::to_string(total) + " samples where there is room for " + std::to_string(maxSamples));
            if (total > 8 * static_cast<uint64_t>(size)) throw Error("lossless JPEG: more samples than its bytes can hold");
            if (restartInterval && restartInterval % out.width) throw Unsupported("lossless JPEG with restart intervals that do not begin with a line");
            const unsigned linesPerInterval = restartInterval ? restartInterval / out.width : 0;
            out.samples.resize(static_cast<size_t>(total));

            const unsigned C = out.components, W = out.width;
            std::vector<int32_t> previous(static_cast<size_t>(W) * C), current(previous.size());
            const int32_t initial = 1 << (out.precision - transform - 1);
            BitReader bits(data, size, pos);
            int expectedRestart = 0;
            size_t o = 0;
            for (unsigned y = 0; y < out.height; ++y) {
                bool first = y == 0;
                if (linesPerInterval && y > 0 && y % linesPerInterval == 0) {
                    const int m = bits.marker();
                    if (m != 0xD0 + expectedRestart) throw Error("lossless JPEG: a restart marker is missing or out of order");
                    expectedRestart = (expectedRestart + 1) & 7;
                    first = true;
                }
                for (unsigned x = 0; x < W; ++x) {
                    for (unsigned c = 0; c < C; ++c) {
                        const HuffmanTable& t = tables[components[c].table];
                        // the category of the difference
                        int category;
                        const uint32_t nine = bits.peek(9);
                        if (t.fastLength[nine]) {
                            category = t.fastSymbol[nine];
                            bits.skip(t.fastLength[nine]);
                        } else {
                            int length = 10;
                            const uint32_t sixteen = bits.peek(16);
                            int32_t value = static_cast<int32_t>(sixteen >> 6);
                            while (length <= 16 && value > t.maxCode[length]) {
                                ++length;
                                if (length <= 16) value = static_cast<int32_t>(sixteen >> (16 - length));
                            }
                            if (length > 16) throw Error("lossless JPEG: a code that is not in its Huffman table");
                            category = t.symbols[static_cast<size_t>(t.firstIndex[length] + value - t.firstCode[length])];
                            bits.skip(length);
                        }
                        int32_t difference = 0;
                        if (category == 16) {
                            difference = 32768;
                        } else if (category > 0) {
                            const int32_t v = static_cast<int32_t>(bits.take(category));
                            difference = v < (1 << (category - 1)) ? v - (1 << category) + 1 : v;
                        }
                        int32_t prediction;
                        const size_t at = static_cast<size_t>(x) * C + c;
                        if (first) {
                            prediction = x == 0 ? initial : current[at - C];
                        } else if (x == 0) {
                            prediction = previous[at];
                        } else {
                            const int32_t a = current[at - C], b = previous[at], cc = previous[at - C];
                            switch (predictor) {
                                case 1: prediction = a; break;
                                case 2: prediction = b; break;
                                case 3: prediction = cc; break;
                                case 4: prediction = a + b - cc; break;
                                case 5: prediction = a + ((b - cc) >> 1); break;
                                case 6: prediction = b + ((a - cc) >> 1); break;
                                default: prediction = (a + b) >> 1; break;
                            }
                        }
                        const int32_t value = (prediction + difference) & 0xFFFF;
                        current[at] = value;
                        out.samples[o++] = static_cast<uint16_t>((value << transform) & 0xFFFF);
                    }
                }
                std::swap(previous, current);
            }
            const int end = bits.marker();
            if (end != 0 && end != 0xD9) throw Error("lossless JPEG: something other than the end of the image follows the scan");
            return out;
        } else if (code == 0xD9) {
            throw Error("lossless JPEG: the image ends before its scan");
        } else if (code >= 0xC9 && code <= 0xCF && code != 0xCC) {
            throw Unsupported("JPEG with arithmetic coding (a frame of the kind SOF" + std::to_string(code - 0xC0) + ")");
        } else if (code == 0xCC) {
            throw Unsupported("JPEG with arithmetic coding");
        } else if (code >= 0xC0 && code <= 0xC7) {   // (0xC3, 0xC4 are taken above)
            throw Unsupported("JPEG that is not lossless (a frame of the kind SOF" + std::to_string(code - 0xC0) + ")");
        } else if (code == 0xDC) {
            throw Unsupported("lossless JPEG whose height follows the scan (DNL)");
        }
        // every other segment (APPn, COM, DQT) says nothing about the samples
    }
}

// ------------------------------------------------------------------------------------------
// The TIFF structure
// ------------------------------------------------------------------------------------------

namespace {

enum Tag : uint16_t {
    kNewSubFileType = 254, kImageWidth = 256, kImageLength = 257, kBitsPerSample = 258, kCompression = 259,
    kPhotometric = 262, kFillOrder = 266, kMake = 271, kModel = 272, kStripOffsets = 273, kSamplesPerPixel = 277,
    kRowsPerStrip = 278, kStripByteCounts = 279, kPlanarConfiguration = 284, kSoftware = 305, kPredictor = 317,
    kTileWidth = 322, kTileLength = 323, kTileOffsets = 324, kTileByteCounts = 325, kSubIfds = 330, kSampleFormat = 339,
    kCfaRepeatPatternDim = 33421, kCfaPattern = 33422, kExposureTime = 33434, kExifIfd = 34665, kIsoSpeed = 34855,
    kDateTimeOriginal = 36867, kOffsetTimeOriginal = 36881, kFocalLength = 37386, kSubSecTimeOriginal = 37521,
    kDngVersion = 50706, kUniqueCameraModel = 50708, kCfaPlaneColor = 50710, kCfaLayout = 50711, kLinearizationTable = 50712,
    kBlackLevelRepeatDim = 50713, kBlackLevel = 50714, kWhiteLevel = 50717, kRawImageDigest = 50972, kActiveArea = 50829,
    kNewRawImageDigest = 51111
};

size_t typeSize(uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

struct Entry {
    uint16_t type = 0;
    uint64_t count = 0;
    uint64_t at = 0;   // where the value is in the file (in the entry itself, or where it points)
};

struct Ifd {
    uint64_t offset = 0;
    std::string where;   // "IFD 0", "IFD 0 / SubIFD 1"
    std::map<uint16_t, Entry> entries;
    bool has(uint16_t tag) const { return entries.count(tag) != 0; }
};

class Tiff {
public:
    explicit Tiff(const std::string& path) : path_(path), in_(toPath(path), std::ios::binary) {
        if (!in_) failToOpen(path);
        in_.seekg(0, std::ios::end);
        size_ = static_cast<uint64_t>(in_.tellg());
        in_.seekg(0);
        uint8_t h[8];
        if (size_ < 8) throw Error("not a DNG file: too short for a TIFF header");
        read(0, h, 8, "the TIFF header");
        if (h[0] == 'I' && h[1] == 'I') little_ = true;
        else if (h[0] == 'M' && h[1] == 'M') little_ = false;
        else throw Error("not a DNG file: no TIFF header");
        const uint16_t magic = u16(h + 2);
        if (magic == 43) throw Unsupported("BigTIFF (a DNG file is a classic TIFF file)");
        if (magic != 42) throw Error("not a DNG file: no TIFF header");
        first_ = u32(h + 4);
    }

    uint64_t size() const { return size_; }
    uint64_t first() const { return first_; }
    bool little() const { return little_; }

    void read(uint64_t at, void* out, uint64_t n, const std::string& what) {
        if (at > size_ || n > size_ - at) throw Error(what + " lies beyond the end of the file (is it cut short?)");
        if (!n) return;
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(at));
        in_.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
        if (static_cast<uint64_t>(in_.gcount()) != n) throw Error("cannot read " + what, ErrorKind::Io);
    }
    std::vector<uint8_t> bytes(uint64_t at, uint64_t n, const std::string& what) {
        if (at > size_ || n > size_ - at) throw Error(what + " lies beyond the end of the file (is it cut short?)");
        std::vector<uint8_t> v(static_cast<size_t>(n));
        read(at, v.data(), n, what);
        return v;
    }

    uint16_t u16(const uint8_t* p) const {
        return little_ ? static_cast<uint16_t>(p[0] | p[1] << 8) : static_cast<uint16_t>(p[0] << 8 | p[1]);
    }
    uint32_t u32(const uint8_t* p) const {
        return little_ ? static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
                             static_cast<uint32_t>(p[3]) << 24
                       : static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
                             static_cast<uint32_t>(p[3]);
    }

    Ifd readIfd(uint64_t offset, const std::string& where, uint64_t& next) {
        uint8_t c[2];
        read(offset, c, 2, where);
        const uint16_t n = u16(c);
        if (n == 0) throw Error(where + " is empty");
        entries_ += n;   // (directories may overlap in the file: what they hold together is limited)
        if (entries_ > 65536) throw Error(where + ": the directories of the file hold more than 65536 entries");
        const std::vector<uint8_t> raw = bytes(offset + 2, uint64_t(n) * 12 + 4, where);
        Ifd ifd;
        ifd.offset = offset;
        ifd.where = where;
        for (uint16_t k = 0; k < n; ++k) {
            const uint8_t* e = raw.data() + 12u * k;
            Entry entry;
            const uint16_t tag = u16(e);
            entry.type = u16(e + 2);
            entry.count = u32(e + 4);
            const size_t size = typeSize(entry.type);
            if (!size) continue;   // a type TIFF does not know: passed over, as TIFF readers do
            const uint64_t length = entry.count * size;
            entry.at = length <= 4 ? offset + 2 + 12u * k + 8 : u32(e + 8);
            if (length > size_ || entry.at > size_ - length) {
                throw Error(where + ": the value of tag " + std::to_string(tag) + " lies beyond the end of the file (is it cut short?)");
            }
            ifd.entries.emplace(tag, entry);   // (a tag given twice: the first one counts)
        }
        next = u32(raw.data() + 12u * n);
        return ifd;
    }

    // The values of a tag of an integer type; none if it is not there.
    std::vector<uint64_t> uints(const Ifd& ifd, uint16_t tag) {
        const auto it = ifd.entries.find(tag);
        if (it == ifd.entries.end()) return {};
        const Entry& e = it->second;
        const size_t size = typeSize(e.type);
        const std::vector<uint8_t> raw = bytes(e.at, e.count * size, ifd.where + ": tag " + std::to_string(tag));
        std::vector<uint64_t> out(static_cast<size_t>(e.count));
        for (size_t k = 0; k < out.size(); ++k) {
            const uint8_t* p = raw.data() + k * size;
            switch (e.type) {
                case 1: case 7: out[k] = p[0]; break;
                case 3: out[k] = u16(p); break;
                case 4: case 13: out[k] = u32(p); break;
                default: throw Error(ifd.where + ": tag " + std::to_string(tag) + " is not an unsigned integer");
            }
        }
        return out;
    }
    uint64_t one(const Ifd& ifd, uint16_t tag, uint64_t fallback) {
        const std::vector<uint64_t> v = uints(ifd, tag);
        return v.empty() ? fallback : v[0];
    }
    // The values of a tag of any numeric type, rationals divided out.
    std::vector<double> numbers(const Ifd& ifd, uint16_t tag) {
        const auto it = ifd.entries.find(tag);
        if (it == ifd.entries.end()) return {};
        const Entry& e = it->second;
        const size_t size = typeSize(e.type);
        const std::vector<uint8_t> raw = bytes(e.at, e.count * size, ifd.where + ": tag " + std::to_string(tag));
        std::vector<double> out(static_cast<size_t>(e.count));
        for (size_t k = 0; k < out.size(); ++k) {
            const uint8_t* p = raw.data() + k * size;
            switch (e.type) {
                case 1: case 7: out[k] = p[0]; break;
                case 6: out[k] = static_cast<int8_t>(p[0]); break;
                case 3: out[k] = u16(p); break;
                case 8: out[k] = static_cast<int16_t>(u16(p)); break;
                case 4: case 13: out[k] = u32(p); break;
                case 9: out[k] = static_cast<int32_t>(u32(p)); break;
                case 5: out[k] = u32(p + 4) ? static_cast<double>(u32(p)) / u32(p + 4) : std::nan(""); break;
                case 10: {
                    const int32_t d = static_cast<int32_t>(u32(p + 4));
                    out[k] = d ? static_cast<double>(static_cast<int32_t>(u32(p))) / d : std::nan("");
                    break;
                }
                case 11: {
                    const uint32_t bits = u32(p);
                    float f;
                    std::memcpy(&f, &bits, 4);
                    out[k] = f;
                    break;
                }
                case 12: {
                    uint8_t b[8];
                    for (int i = 0; i < 8; ++i) b[i] = little_ == hostIsLittleEndian() ? p[i] : p[7 - i];
                    double d;
                    std::memcpy(&d, b, 8);
                    out[k] = d;
                    break;
                }
                default: throw Error(ifd.where + ": tag " + std::to_string(tag) + " is not a number");
            }
        }
        return out;
    }
    // The text of a tag (ASCII), up to its first NUL, without blanks at its ends.
    std::string text(const Ifd& ifd, uint16_t tag) {
        const auto it = ifd.entries.find(tag);
        if (it == ifd.entries.end() || (it->second.type != 2 && it->second.type != 1 && it->second.type != 7)) return std::string();
        const std::vector<uint8_t> raw = bytes(it->second.at, it->second.count, ifd.where + ": tag " + std::to_string(tag));
        std::string s(raw.begin(), raw.end());
        const size_t nul = s.find('\0');
        if (nul != std::string::npos) s.resize(nul);
        return trim(s);
    }
    std::vector<uint8_t> raw(const Ifd& ifd, uint16_t tag) {
        const auto it = ifd.entries.find(tag);
        if (it == ifd.entries.end()) return {};
        return bytes(it->second.at, it->second.count * typeSize(it->second.type), ifd.where + ": tag " + std::to_string(tag));
    }

private:
    std::string path_;
    std::ifstream in_;
    uint64_t size_ = 0;
    uint64_t first_ = 0;
    uint64_t entries_ = 0;
    bool little_ = true;
};

// Every directory of the file that holds an image: IFD 0 and those chained to it, and their
// SubIFDs; and the EXIF directory.
struct Directories {
    std::vector<Ifd> images;
    Ifd exif;
    bool hasExif = false;
};

Directories readDirectories(Tiff& tiff) {
    Directories out;
    std::set<uint64_t> seen;
    size_t count = 0;
    std::vector<std::pair<uint64_t, std::string>> todo;   // (offset, name) of SubIFDs, read after their parent
    auto visit = [&](uint64_t offset, const std::string& where, uint64_t& next) -> Ifd& {
        if (!seen.insert(offset).second) throw Error(where + " is a directory that was read already (the directories run in a circle)");
        if (++count > 256) throw Error("more than 256 directories");
        out.images.push_back(tiff.readIfd(offset, where, next));
        return out.images.back();
    };
    uint64_t offset = tiff.first();
    for (size_t n = 0; offset != 0; ++n) {
        uint64_t next = 0;
        const std::string where = "IFD " + std::to_string(n);
        Ifd& ifd = visit(offset, where, next);
        const std::vector<uint64_t> subs = tiff.uints(ifd, kSubIfds);
        if (n == 0) {
            if (!ifd.has(kDngVersion)) throw Error("not a DNG file: a TIFF file whose first directory has no DNGVersion");
            if (ifd.has(kExifIfd)) {
                const uint64_t at = tiff.one(ifd, kExifIfd, 0);
                if (at && seen.insert(at).second) {
                    uint64_t ignored = 0;
                    out.exif = tiff.readIfd(at, "the EXIF directory", ignored);
                    out.hasExif = true;
                }
            }
        }
        if (subs.size() > 64) throw Error(where + " names more than 64 SubIFDs");
        for (size_t k = 0; k < subs.size(); ++k) todo.emplace_back(subs[k], where + " / SubIFD " + std::to_string(k));
        offset = next;
    }
    for (size_t k = 0; k < todo.size(); ++k) {
        uint64_t ignored = 0;
        Ifd& ifd = visit(todo[k].first, todo[k].second, ignored);
        if (todo[k].second.size() > 64) continue;   // (SubIFDs of SubIFDs, a level or two down; no deeper)
        const std::vector<uint64_t> subs = tiff.uints(ifd, kSubIfds);
        if (subs.size() > 64) throw Error(ifd.where + " names more than 64 SubIFDs");
        const std::string where = ifd.where;
        for (size_t j = 0; j < subs.size(); ++j) todo.emplace_back(subs[j], where + " / SubIFD " + std::to_string(j));
    }
    return out;
}

const char* compressionName(uint64_t c) {
    switch (c) {
        case 1: return "uncompressed";
        case 7: return "JPEG";
        case 8: case 32946: return "Deflate";
        case 34892: return "lossy JPEG";
        case 52546: return "JPEG XL";
        default: return nullptr;
    }
}

std::string compressionText(uint64_t c) {
    const char* name = compressionName(c);
    return name ? name : "compression " + std::to_string(c);
}

std::string number(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return cNumber(buf);
}

// Days since 1970-01-01 of a date of the proleptic Gregorian calendar, and back.
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

bool digitsAt(const std::string& s, size_t at, size_t n, int& value) {
    if (at + n > s.size()) return false;
    value = 0;
    for (size_t k = 0; k < n; ++k) {
        const char c = s[at + k];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    return true;
}

// EXIF's "YYYY:MM:DD HH:MM:SS" with its fraction of a second and its offset from UTC, as FITS
// writes a time: in UTC when the offset is known (`utc`), else as the camera clock had it.
bool exifTime(const std::string& when, const std::string& subSeconds, const std::string& offset, std::string& out, bool& utc) {
    int Y, M, D, h, m, s;
    if (!digitsAt(when, 0, 4, Y) || when[4] != ':' || !digitsAt(when, 5, 2, M) || when[7] != ':' || !digitsAt(when, 8, 2, D) ||
        (when[10] != ' ' && when[10] != 'T') || !digitsAt(when, 11, 2, h) || when[13] != ':' || !digitsAt(when, 14, 2, m) ||
        when[16] != ':' || !digitsAt(when, 17, 2, s)) {
        return false;
    }
    static const int monthDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0;
    if (M < 1 || M > 12 || D < 1 || D > monthDays[M - 1] || (M == 2 && D == 29 && !leap) || h > 23 || m > 59 || s > 59) return false;
    std::string fraction;
    for (char c : subSeconds) {
        if (c >= '0' && c <= '9') fraction += c;
        else break;
    }
    int64_t seconds = daysFromCivil(Y, static_cast<unsigned>(M), static_cast<unsigned>(D)) * 86400 + h * 3600 + m * 60 + s;
    utc = false;
    int oh, om;
    if (offset.size() >= 6 && (offset[0] == '+' || offset[0] == '-') && digitsAt(offset, 1, 2, oh) && offset[3] == ':' &&
        digitsAt(offset, 4, 2, om) && oh <= 14 && om <= 59) {
        const int64_t shift = (oh * 3600 + om * 60) * (offset[0] == '-' ? -1 : 1);
        seconds -= shift;
        utc = true;
    }
    int64_t day = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
    const int64_t rest = seconds - day * 86400;
    int64_t y;
    unsigned mo, d;
    civilFromDays(day, y, mo, d);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02d:%02d:%02d", static_cast<long long>(y), mo, d, static_cast<int>(rest / 3600),
                  static_cast<int>(rest / 60 % 60), static_cast<int>(rest % 60));
    out = buf;
    if (!fraction.empty()) out += "." + fraction;
    return true;
}

// What a TIFF file writes in a row: the samples, packed to as many bits as they have, the row
// filled to a whole byte. Samples of 8, 16 or 32 bits are in the byte order of the file; DNG packs
// every other size with the highest bit first, also in a little-endian file (24 bits included).
template <typename T>
void unpackRow(const uint8_t* p, uint64_t samples, unsigned bits, bool little, T* out) {
    if (bits == 8) {
        for (uint64_t k = 0; k < samples; ++k) out[k] = p[k];
    } else if (bits == 16) {
        for (uint64_t k = 0; k < samples; ++k, p += 2) out[k] = static_cast<T>(little ? p[0] | p[1] << 8 : p[0] << 8 | p[1]);
    } else if (bits == 32) {
        for (uint64_t k = 0; k < samples; ++k, p += 4) {
            uint32_t v = 0;
            for (unsigned b = 0; b < 4; ++b) v |= static_cast<uint32_t>(p[little ? b : 3 - b]) << (8 * b);
            out[k] = static_cast<T>(v);
        }
    } else {
        uint64_t acc = 0;
        unsigned count = 0;
        for (uint64_t k = 0; k < samples; ++k) {
            while (count < bits) {
                acc = acc << 8 | *p++;
                count += 8;
            }
            out[k] = static_cast<T>((acc >> (count - bits)) & ((uint64_t(1) << bits) - 1));
            count -= bits;
        }
    }
}

struct RawLayout {
    uint64_t width = 0, height = 0;
    unsigned spp = 1, bits = 0;
    uint64_t compression = 1, predictor = 1;
    bool tiled = false;
    uint64_t chunkWidth = 0, chunkHeight = 0;   // a tile, or a strip of the full width
    std::vector<uint64_t> offsets, counts;
    uint64_t top = 0, left = 0, bottom = 0, right = 0;   // the active area
    std::vector<uint16_t> linearization;
};

// The samples of one tile or strip, as the file has them (before linearization), chunk rows of
// chunkWidth samples times spp.
template <typename T>
std::vector<T> decodeChunk(Tiff& tiff, const RawLayout& r, size_t k, uint64_t rows, uint64_t validWidth, uint64_t validRows,
                           const std::string& name) {
    const uint64_t rowSamples = checkedMul(r.chunkWidth, r.spp, "the size of a tile");
    const uint64_t capacity = checkedMul(rowSamples, rows, "the size of a tile");
    std::vector<T> out(static_cast<size_t>(capacity));
    const uint64_t offset = r.offsets[k], count = r.counts[k];
    if (r.compression == 7) {
        const std::vector<uint8_t> data = tiff.bytes(offset, count, name);
        LosslessJpeg j = decodeLosslessJpeg(data.data(), data.size(), capacity);
        const uint64_t jpegRow = static_cast<uint64_t>(j.width) * j.components;
        // The samples go into the tile one after the other, a row of the tile at a time. A tile at
        // the edge of the image may have been encoded as wide as its part of the image.
        const uint64_t wrap = jpegRow != rowSamples && jpegRow == validWidth * r.spp ? jpegRow : rowSamples;
        const uint64_t needed = (validRows - 1) * wrap + validWidth * r.spp;
        if (j.samples.size() < needed) {
            throw Error(name + ": its JPEG data holds " + std::to_string(j.samples.size()) + " samples, fewer than its part of the image (" +
                        std::to_string(needed) + ")");
        }
        if (j.samples.size() > wrap * rows) {
            throw Error(name + ": its JPEG data holds more samples than the tile");
        }
        for (size_t i = 0; i < j.samples.size(); ++i) out[static_cast<size_t>((i / wrap) * rowSamples + i % wrap)] = j.samples[i];
        return out;
    }
    const uint64_t rowBytes = (checkedMul(rowSamples, r.bits, "the size of a tile") + 7) / 8;
    const uint64_t needed = checkedMul(rowBytes, rows, "the size of a tile");
    std::vector<uint8_t> bytes;
    if (r.compression == 1) {
        if (count < needed) {
            throw Error(name + " holds " + std::to_string(count) + " bytes where " + std::to_string(needed) + " are needed");
        }
        bytes = tiff.bytes(offset, needed, name);
    } else {
        // Deflate: a zlib stream. It cannot make more than 1032 bytes of one.
        if (needed / 1032 > count + 1) throw Error(name + ": " + std::to_string(count) + " bytes of Deflate data cannot make the " + std::to_string(needed) + " bytes of the tile");
        const std::vector<uint8_t> data = tiff.bytes(offset, count, name);
        bytes = zlibDecompress(data.data(), data.size(), static_cast<size_t>(needed));
    }
    for (uint64_t row = 0; row < rows; ++row) {
        T* line = out.data() + row * rowSamples;
        unpackRow(bytes.data() + row * rowBytes, rowSamples, r.bits, tiff.little(), line);
        progressTick(rowBytes);
        if (r.predictor != 1) {
            // horizontal differences: of the sample one pixel (two, four) to the left
            const uint64_t back = r.spp * (r.predictor == 2 ? 1 : r.predictor == 34892 ? 2 : 4);
            const uint64_t mask = r.bits >= 32 ? 0xFFFFFFFFull : (uint64_t(1) << r.bits) - 1;
            for (uint64_t i = back; i < rowSamples; ++i) line[i] = static_cast<T>((static_cast<uint64_t>(line[i]) + line[i - back]) & mask);
        }
    }
    return out;
}

// The samples of the file go to the image (T) as they are, or through the linearization table;
// they are decoded as C, which holds as many bits as the file's samples have.
// Each run of samples of a row of a plane goes to `put`, with its place in the image (in bytes,
// planar).
using SamplePut = std::function<void(uint64_t at, const uint8_t* data, size_t n)>;

template <typename T, typename C>
void readSamples(Tiff& tiff, const RawLayout& r, const SamplePut& put) {
    const uint64_t outWidth = r.right - r.left, outHeight = r.bottom - r.top;
    const uint64_t planeSize = outWidth * outHeight;
    std::vector<T> run;
    const uint64_t across = r.tiled ? (r.width + r.chunkWidth - 1) / r.chunkWidth : 1;
    for (size_t k = 0; k < r.offsets.size(); ++k) {
        const uint64_t x0 = r.tiled ? (k % across) * r.chunkWidth : 0;
        const uint64_t y0 = r.tiled ? (k / across) * r.chunkHeight : k * r.chunkHeight;
        const uint64_t rows = r.tiled ? r.chunkHeight : std::min(r.chunkHeight, r.height - y0);
        const uint64_t validWidth = std::min(r.chunkWidth, r.width - x0), validRows = std::min(rows, r.height - y0);
        const std::string name = (r.tiled ? "tile " : "strip ") + std::to_string(k);
        const std::vector<C> chunk = decodeChunk<C>(tiff, r, k, rows, validWidth, validRows, name);
        // the columns of the chunk that are in the active area
        const uint64_t from = std::max(x0, r.left), to = std::min(x0 + validWidth, r.right);
        for (uint64_t row = 0; row < validRows && from < to; ++row) {
            const uint64_t y = y0 + row;
            if (y < r.top || y >= r.bottom) continue;
            const C* line = chunk.data() + row * r.chunkWidth * r.spp;
            for (unsigned s = 0; s < r.spp; ++s) {
                run.resize(static_cast<size_t>(to - from));
                for (uint64_t x = from; x < to; ++x) {
                    uint64_t v = line[(x - x0) * r.spp + s];
                    if (!r.linearization.empty()) v = r.linearization[static_cast<size_t>(std::min<uint64_t>(v, r.linearization.size() - 1))];
                    run[static_cast<size_t>(x - from)] = static_cast<T>(v);
                }
                put((s * planeSize + (y - r.top) * outWidth + (from - r.left)) * sizeof(T), reinterpret_cast<const uint8_t*>(run.data()),
                    run.size() * sizeof(T));
            }
        }
        progress("reading", k + 1, r.offsets.size());
    }
}

}  // namespace

// ------------------------------------------------------------------------------------------
// Reading a DNG file
// ------------------------------------------------------------------------------------------

bool looksLikeDng(const std::string& path) {
    try {
        std::ifstream in(toPath(path), std::ios::binary);
        uint8_t h[8];
        if (!in.read(reinterpret_cast<char*>(h), 8)) return false;
        const bool little = h[0] == 'I' && h[1] == 'I';
        if (!little && !(h[0] == 'M' && h[1] == 'M')) return false;
        auto u16 = [&](const uint8_t* p) { return little ? p[0] | p[1] << 8 : p[0] << 8 | p[1]; };
        auto u32 = [&](const uint8_t* p) {
            return little ? static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
                                static_cast<uint32_t>(p[3]) << 24
                          : static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
                                static_cast<uint32_t>(p[3]);
        };
        if (u16(h + 2) != 42) return false;
        in.seekg(static_cast<std::streamoff>(u32(h + 4)));
        uint8_t c[2];
        if (!in.read(reinterpret_cast<char*>(c), 2)) return false;
        const int n = u16(c);
        std::vector<uint8_t> entries(static_cast<size_t>(n) * 12);
        if (!in.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entries.size()))) return false;
        for (int k = 0; k < n; ++k)
            if (u16(entries.data() + 12 * k) == kDngVersion) return true;
    } catch (...) {
    }
    return false;
}

// readDng; `digest`, if given, names the digest of the raw data the file carries ("" if none).
static FitsFile readDngFile(const std::string& path, bool headersOnly, std::string* digest, bool inPieces = false) {
    Tiff tiff(path);
    Directories dirs = readDirectories(tiff);
    if (digest) {
        digest->clear();
        for (const Ifd& ifd : dirs.images) {
            if (ifd.has(kNewRawImageDigest) || ifd.has(kRawImageDigest)) {
                *digest = ifd.has(kNewRawImageDigest) ? "NewRawImageDigest" : "RawImageDigest";
                break;
            }
        }
    }
    FitsFile file;
    file.path = path;
    file.fileSize = tiff.size();
    const Ifd& ifd0 = dirs.images.front();

    // the raw image: the first full-resolution image of sensor data
    const Ifd* raw = nullptr;
    for (const Ifd& ifd : dirs.images) {
        const uint64_t kind = tiff.one(ifd, kNewSubFileType, 0), photometric = tiff.one(ifd, kPhotometric, 0);
        const bool sensor = photometric == 32803 || photometric == 34892;
        if (!raw && kind == 0 && sensor) {
            raw = &ifd;
            continue;
        }
        std::string what = kind == 0 && sensor ? "a second raw image"
                           : (kind & 1) ? "a preview"
                           : (kind & 4) ? "a transparency mask"
                           : kind >= 0x10000 ? "a depth map or a mask"
                           : "an image that is not sensor data (photometric " + std::to_string(photometric) + ")";
        file.skipped.push_back(ifd.where + ": " + what + " of " + std::to_string(tiff.one(ifd, kImageWidth, 0)) + " x " +
                               std::to_string(tiff.one(ifd, kImageLength, 0)) + " (" + compressionText(tiff.one(ifd, kCompression, 1)) + "), not read");
    }
    if (!raw) throw Error("no raw image in this DNG file: none of its images is sensor data at full resolution");

    RawLayout r;
    const std::string& where = raw->where;
    r.width = tiff.one(*raw, kImageWidth, 0);
    r.height = tiff.one(*raw, kImageLength, 0);
    if (r.width == 0 || r.height == 0 || r.width > (1u << 20) || r.height > (1u << 20)) {
        throw Error(where + ": an image of " + std::to_string(r.width) + " x " + std::to_string(r.height) + " pixels");
    }
    const uint64_t photometric = tiff.one(*raw, kPhotometric, 0);
    r.spp = static_cast<unsigned>(tiff.one(*raw, kSamplesPerPixel, 1));
    if (r.spp < 1 || r.spp > 4 || (photometric == 32803 && r.spp != 1)) {
        throw Error(where + ": " + std::to_string(r.spp) + " samples per pixel");
    }
    const std::vector<uint64_t> bits = tiff.uints(*raw, kBitsPerSample);
    r.bits = bits.empty() ? 1 : static_cast<unsigned>(bits[0]);
    for (uint64_t b : bits)
        if (b != r.bits) throw Unsupported(where + ": samples of different sizes");
    for (uint64_t f : tiff.uints(*raw, kSampleFormat)) {
        if (f == 3) throw Unsupported("floating-point DNG data (an HDR merge, not the samples of a sensor)");
        if (f != 1) throw Unsupported(where + ": samples of the sample format " + std::to_string(f));
    }
    r.compression = tiff.one(*raw, kCompression, 1);
    if (r.compression == 34892) throw Unsupported("lossy DNG (its raw data is compressed with lossy JPEG and is no longer what the sensor recorded)");
    if (r.compression == 52546) throw Unsupported("DNG with JPEG XL compression");
    if (r.compression == 32946) r.compression = 8;
    if (r.compression != 1 && r.compression != 7 && r.compression != 8) throw Unsupported(where + ": " + compressionText(r.compression));
    if ((r.compression != 7 && !(r.bits >= 1 && r.bits <= 16) && r.bits != 24 && r.bits != 32) ||
        (r.compression == 7 && (r.bits < 2 || r.bits > 16))) {
        throw Unsupported(where + ": " + std::to_string(r.bits) + "-bit samples with " + compressionText(r.compression) + " data");
    }
    if (r.spp > 1 && tiff.one(*raw, kPlanarConfiguration, 1) != 1) throw Unsupported(where + ": samples stored plane by plane");
    if (tiff.one(*raw, kFillOrder, 1) != 1) throw Unsupported(where + ": the bits of a byte in the reverse order (FillOrder 2)");
    r.predictor = tiff.one(*raw, kPredictor, 1);
    if (r.predictor != 1 && (r.compression != 8 || (r.predictor != 2 && r.predictor != 34892 && r.predictor != 34893))) {
        throw Unsupported(where + ": predictor " + std::to_string(r.predictor) + " with " + compressionText(r.compression) + " data");
    }
    if (r.predictor != 1 && r.bits != 8 && r.bits != 16 && r.bits != 32) {
        throw Unsupported(where + ": predictor " + std::to_string(r.predictor) + " with " + std::to_string(r.bits) + "-bit samples");
    }

    if (raw->has(kTileWidth) || raw->has(kTileOffsets)) {
        r.tiled = true;
        r.chunkWidth = tiff.one(*raw, kTileWidth, 0);
        r.chunkHeight = tiff.one(*raw, kTileLength, 0);
        if (r.chunkWidth == 0 || r.chunkHeight == 0 || r.chunkWidth > 65536 || r.chunkHeight > 65536) {
            throw Error(where + ": tiles of " + std::to_string(r.chunkWidth) + " x " + std::to_string(r.chunkHeight));
        }
        r.offsets = tiff.uints(*raw, kTileOffsets);
        r.counts = tiff.uints(*raw, kTileByteCounts);
    } else {
        r.chunkWidth = r.width;
        r.chunkHeight = std::min<uint64_t>(tiff.one(*raw, kRowsPerStrip, r.height), r.height);
        if (r.chunkHeight == 0) r.chunkHeight = r.height;
        r.offsets = tiff.uints(*raw, kStripOffsets);
        r.counts = tiff.uints(*raw, kStripByteCounts);
    }
    // Where the samples are is checked when they are read (the headers of a file whose data is
    // cut off can still be looked at).
    // And whether they can hold the image, before memory is taken for it: a header can claim any
    // size, the bytes of the file cannot (uncompressed data has all its bytes, lossless JPEG at
    // least a bit a sample, Deflate at most 1032 bytes of each).
    auto checkChunks = [&] {
        const uint64_t expected = r.tiled ? ((r.width + r.chunkWidth - 1) / r.chunkWidth) * ((r.height + r.chunkHeight - 1) / r.chunkHeight)
                                          : (r.height + r.chunkHeight - 1) / r.chunkHeight;
        const char* what = r.tiled ? " tiles" : " strips";
        if (r.tiled && (r.chunkWidth > (r.width + 15) / 16 * 16 || r.chunkHeight > (r.height + 15) / 16 * 16)) {
            throw Error(where + ": tiles of " + std::to_string(r.chunkWidth) + " x " + std::to_string(r.chunkHeight) +
                        ", larger than the image of " + std::to_string(r.width) + " x " + std::to_string(r.height));
        }
        if (r.offsets.size() != expected || r.counts.size() != expected) {
            throw Error(where + ": " + std::to_string(r.offsets.size()) + what + " where the image has " + std::to_string(expected) +
                        (r.counts.size() != r.offsets.size() ? " (and " + std::to_string(r.counts.size()) + " byte counts)" : ""));
        }
        const uint64_t across = r.tiled ? (r.width + r.chunkWidth - 1) / r.chunkWidth : 1;
        for (size_t k = 0; k < r.offsets.size(); ++k) {
            const std::string name = (r.tiled ? "tile " : "strip ") + std::to_string(k);
            if (r.offsets[k] > tiff.size() || r.counts[k] > tiff.size() - r.offsets[k]) {
                throw Error(where + ": " + name + " lies beyond the end of the file (is it cut short?)");
            }
            const uint64_t x0 = r.tiled ? (k % across) * r.chunkWidth : 0;
            const uint64_t y0 = (r.tiled ? k / across : k) * r.chunkHeight;
            const uint64_t rows = r.tiled ? r.chunkHeight : std::min(r.chunkHeight, r.height - y0);
            const uint64_t valid = std::min(r.chunkWidth, r.width - x0) * std::min(rows, r.height - y0) * r.spp;
            const uint64_t bytes = (r.chunkWidth * r.spp * r.bits + 7) / 8 * rows;
            const uint64_t count = r.counts[k];
            if ((r.compression == 1 && count < bytes) || (r.compression == 7 && valid > 8 * count) ||
                (r.compression == 8 && bytes / 1032 > count + 1)) {
                throw Error(where + ": " + name + " has " + std::to_string(count) + " bytes, too few for its " + std::to_string(valid) +
                            " samples" + (r.compression == 1 ? " (is the file cut short?)" : ""));
            }
        }
        // The same for all of them together: strips or tiles that point at the same bytes would
        // otherwise make a little data stand for a large image. Overlaps count once.
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        for (size_t k = 0; k < r.offsets.size(); ++k) ranges.emplace_back(r.offsets[k], r.offsets[k] + r.counts[k]);
        std::sort(ranges.begin(), ranges.end());
        uint64_t covered = 0, reached = 0;
        for (const auto& range : ranges) {
            const uint64_t from = std::max(range.first, reached);
            if (range.second > from) covered += range.second - from;
            reached = std::max(reached, range.second);
        }
        const uint64_t bitsOfImage = r.width * r.height * r.spp * (r.compression == 7 ? 1 : r.bits);
        if (bitsOfImage / 8 / (r.compression == 8 ? 1032 : 1) > covered) {
            throw Error(where + ": the " + std::to_string(r.width * r.height * r.spp) + " samples of the image cannot be in the " +
                        std::to_string(covered) + " bytes its " + (r.tiled ? "tiles" : "strips") + " take (they overlap)");
        }
    };

    r.top = 0;
    r.left = 0;
    r.bottom = r.height;
    r.right = r.width;
    const std::vector<uint64_t> area = tiff.uints(*raw, kActiveArea);
    if (!area.empty()) {
        if (area.size() != 4 || area[0] >= area[2] || area[1] >= area[3] || area[2] > r.height || area[3] > r.width) {
            throw Error(where + ": an active area that is not inside the image");
        }
        r.top = area[0];
        r.left = area[1];
        r.bottom = area[2];
        r.right = area[3];
    }
    for (uint64_t v : tiff.uints(*raw, kLinearizationTable)) r.linearization.push_back(static_cast<uint16_t>(v));
    if (raw->has(kLinearizationTable) && (r.linearization.empty() || r.linearization.size() > 65536)) {
        throw Unsupported(where + ": a linearization table for this image");
    }

    FitsImage img;
    img.hduIndex = 0;
    img.source = where;
    img.topDown = true;
    img.hasRowOrder = true;
    PixelBuffer& px = img.pixels;
    px.width = r.right - r.left;
    px.height = r.bottom - r.top;
    px.channels = r.spp;
    px.format = r.bits > 16 && r.linearization.empty() ? SampleFormat::UInt32 : SampleFormat::UInt16;
    img.bitpix = px.format == SampleFormat::UInt32 ? 32 : 16;
    std::vector<FitsKeyword>& kw = img.keywords;

    // the colour filter array, relative to the corner of the active area (as the DNG SDK takes it)
    std::string patternNote;
    if (photometric == 32803) {
        const std::vector<uint64_t> dim = tiff.uints(*raw, kCfaRepeatPatternDim);
        const std::vector<uint8_t> pattern = tiff.raw(*raw, kCfaPattern);
        if (dim.size() != 2 || dim[0] < 1 || dim[1] < 1 || dim[0] > 16 || dim[1] > 16 || pattern.size() != dim[0] * dim[1]) {
            throw Error(where + ": a colour filter array without a pattern of its size");
        }
        if (tiff.one(*raw, kCfaLayout, 1) != 1) throw Unsupported(where + ": a colour filter array that is not rectangular (CFALayout)");
        // The cells name colours (TIFF/EP: 0 red, 1 green, 2 blue, 3 cyan, 4 magenta, 5 yellow,
        // 6 white); CFAPlaneColor says which colours the planes of the file are, and a cell must be
        // one of them.
        std::vector<uint8_t> planes = tiff.raw(*raw, kCfaPlaneColor);
        if (planes.empty()) planes = {0, 1, 2};
        static const char letters[] = "RGBCMYW";
        std::string text;
        for (uint8_t colour : pattern) {
            if (colour > 6 || std::find(planes.begin(), planes.end(), colour) == planes.end()) {
                throw Error(where + ": a colour of the filter pattern that is not one of the colours of the file (CFAPlaneColor)");
            }
            text += letters[colour];
        }
        img.cfaPattern = text;
        img.cfaWidth = static_cast<int>(dim[1]);
        img.cfaHeight = static_cast<int>(dim[0]);
        if (dim[0] == 2 && dim[1] == 2 && text.find_first_not_of("RGB") == std::string::npos) {
            kw.push_back({"BAYERPAT", fitsString(text), "colour filter pattern of the sensor (from DNG)"});
        }
        patternNote = ", CFA " + text + (dim[0] == 2 && dim[1] == 2 ? "" : " (" + std::to_string(dim[1]) + " x " + std::to_string(dim[0]) + ")");
    }

    // what the camera says of the exposure
    auto lookup = [&](uint16_t tag) -> const Ifd* {
        if (dirs.hasExif && dirs.exif.has(tag)) return &dirs.exif;
        return ifd0.has(tag) ? &ifd0 : nullptr;
    };
    std::string camera = fitsSanitize(tiff.text(ifd0, kUniqueCameraModel));
    const std::string make = fitsSanitize(tiff.text(ifd0, kMake)), model = fitsSanitize(tiff.text(ifd0, kModel));
    if (!model.empty()) camera = startsWith(toLower(model), toLower(make)) || make.empty() ? model : make + " " + model;
    if (!camera.empty()) kw.push_back({"INSTRUME", fitsString(camera), "camera (from DNG)"});
    if (const Ifd* d = lookup(kDateTimeOriginal)) {
        const Ifd* sub = lookup(kSubSecTimeOriginal);
        const Ifd* off = lookup(kOffsetTimeOriginal);
        std::string when;
        bool utc = false;
        if (exifTime(tiff.text(*d, kDateTimeOriginal), sub ? tiff.text(*sub, kSubSecTimeOriginal) : std::string(),
                     off ? tiff.text(*off, kOffsetTimeOriginal) : std::string(), when, utc)) {
            if (utc) kw.push_back({"DATE-OBS", fitsString(when), "EXIF DateTimeOriginal in UTC"});
            else kw.push_back({"DATE-LOC", fitsString(when), "EXIF DateTimeOriginal: camera clock, time zone unknown"});
        }
    }
    if (const Ifd* d = lookup(kExposureTime)) {
        const std::vector<double> v = tiff.numbers(*d, kExposureTime);
        if (!v.empty() && std::isfinite(v[0]) && v[0] > 0) kw.push_back({"EXPTIME", fitsReal(v[0]), "[s] exposure time (EXIF)"});
    }
    if (const Ifd* d = lookup(kIsoSpeed)) {
        const std::vector<uint64_t> v = tiff.uints(*d, kIsoSpeed);
        if (!v.empty() && v[0] > 0) kw.push_back({"ISOSPEED", std::to_string(v[0]), "ISO speed (EXIF)"});
    }
    if (const Ifd* d = lookup(kFocalLength)) {
        const std::vector<double> v = tiff.numbers(*d, kFocalLength);
        if (!v.empty() && std::isfinite(v[0]) && v[0] > 0) kw.push_back({"FOCALLEN", fitsReal(v[0]), "[mm] focal length of the lens (EXIF)"});
    }
    const std::vector<double> black = tiff.numbers(*raw, kBlackLevel);
    if (!black.empty()) {
        double sum = 0;
        bool same = true;
        for (double b : black) {
            sum += b;
            same = same && b == black[0];
        }
        kw.push_back({"BLKLEVEL", number(same ? black[0] : sum / static_cast<double>(black.size())),
                      same ? "black level of the samples (from DNG)" : "mean of the black levels of the samples (from DNG)"});
    }
    const std::vector<double> white = tiff.numbers(*raw, kWhiteLevel);
    if (!white.empty()) kw.push_back({"WHTLEVEL", number(*std::max_element(white.begin(), white.end())), "white level: the largest value of a sample (from DNG)"});

    const std::vector<uint8_t> version = tiff.raw(ifd0, kDngVersion);
    std::string versionText;
    for (size_t k = 0; k < version.size() && k < 4; ++k) versionText += (k ? "." : "") + std::to_string(version[k]);
    file.formatNote = "DNG " + versionText + (camera.empty() ? "" : ", " + camera);
    const std::string software = fitsSanitize(tiff.text(ifd0, kSoftware));
    if (!software.empty()) file.formatNote += " (" + software + ")";
    img.storage = compressionText(r.compression) + (r.compression == 7 ? " (lossless)" : "") + ", " + std::to_string(r.offsets.size()) +
                  (r.tiled ? (r.offsets.size() == 1 ? " tile of " : " tiles of ") + std::to_string(r.chunkWidth) + " x " + std::to_string(r.chunkHeight)
                           : (r.offsets.size() == 1 ? " strip" : " strips"));
    img.note = std::string(px.format == SampleFormat::UInt32 ? "32" : "16") + "-bit unsigned (" + std::to_string(r.bits) +
               " bits per sample" + (r.linearization.empty() ? "" : ", linearized") + ")" + patternNote + ", " + img.storage;
    if (r.top || r.left || r.bottom != r.height || r.right != r.width) {
        img.note += "; the active area " + std::to_string(px.width) + " x " + std::to_string(px.height) + " of " +
                    std::to_string(r.width) + " x " + std::to_string(r.height);
    }

    if (!headersOnly) {
        checkChunks();
        const uint64_t bytes = checkedMul(px.samples(), sampleBytes(px.format), "the size of the image");
        SamplePut put;
        std::shared_ptr<Store> store;
        if (inPieces) {
            // (into memory, or a temporary file where the image is large)
            store = std::make_shared<Store>(bytes);
            put = [&](uint64_t at, const uint8_t* data, size_t n) { store->write(at, data, n); };
        } else {
            px.data.resize(static_cast<size_t>(bytes));
            put = [&](uint64_t at, const uint8_t* data, size_t n) { std::memcpy(px.data.data() + at, data, n); };
        }
        if (px.format == SampleFormat::UInt32) readSamples<uint32_t, uint32_t>(tiff, r, put);
        else if (r.bits > 16) readSamples<uint16_t, uint32_t>(tiff, r, put);
        else readSamples<uint16_t, uint16_t>(tiff, r, put);
        if (store) img.pieces = storedSource(store, px.width, px.height, px.channels, px.format, StoredLayout());
        img.hasData = true;
    }
    file.images.push_back(std::move(img));
    return file;
}

FitsFile readDng(const std::string& path, bool headersOnly, bool inPieces) { return readDngFile(path, headersOnly, nullptr, inPieces); }

VerifyReport verifyDng(const std::string& path) {
    VerifyReport report;
    FitsFile file;
    std::string digest;
    try {
        file = readDngFile(path, false, &digest, true);   // (the raw image decoded into a store, not into memory)
    } catch (const Unsupported& e) {
        // (a raw image this reader does not decode, in a file that may well be intact)
        report.summary = "the raw image is not read";
        report.notChecked.push_back(std::string("the raw image: ") + e.what());
        return report;
    }
    const FitsImage& img = file.images.front();
    report.summary = "raw image " + std::to_string(img.pixels.width) + " x " + std::to_string(img.pixels.height) + ", " + img.storage;
    if (!file.skipped.empty()) report.summary += "; " + std::to_string(file.skipped.size()) + (file.skipped.size() == 1 ? " other image" : " other images") + " not read";
    if (!digest.empty()) {
        report.notChecked.push_back("the MD5 digest of the raw data that the file carries (" + digest +
                                    "): it is not computed by this program");
    }
    return report;
}

}  // namespace xisfconv
