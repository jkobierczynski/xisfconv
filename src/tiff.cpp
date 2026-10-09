// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "tiff.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace xisfconv {

namespace {

enum : uint16_t { kShort = 3, kLong = 4, kRational = 5, kAscii = 2, kUndefined = 7, kLong8 = 16 };

struct Entry {
    uint16_t tag;
    uint16_t type;
    uint64_t count;
    std::vector<uint8_t> data;  // little-endian payload
};

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

Entry shorts(uint16_t tag, const std::vector<uint16_t>& vals) {
    Entry e{tag, kShort, static_cast<uint32_t>(vals.size()), {}};
    for (auto v : vals) put16(e.data, v);
    return e;
}
void put64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

Entry longs(uint16_t tag, const std::vector<uint32_t>& vals) {
    Entry e{tag, kLong, static_cast<uint32_t>(vals.size()), {}};
    for (auto v : vals) put32(e.data, v);
    return e;
}
// Offsets and sizes: LONG in a classic file, LONG8 in a BigTIFF file.
Entry offsets(uint16_t tag, const std::vector<uint64_t>& vals, bool big) {
    Entry e{tag, big ? kLong8 : kLong, vals.size(), {}};
    for (auto v : vals) {
        if (big) put64(e.data, v);
        else put32(e.data, static_cast<uint32_t>(v));
    }
    return e;
}
Entry rational(uint16_t tag, double value) {
    uint32_t num, den;
    if (value > 0 && value == std::floor(value) && value < 4e9) {
        num = static_cast<uint32_t>(value);
        den = 1;
    } else {
        den = 10000;
        const double n = std::round(std::max(0.0, value) * den);
        num = n > 4294967295.0 ? 4294967295u : static_cast<uint32_t>(n);
    }
    Entry e{tag, kRational, 1, {}};
    put32(e.data, num);
    put32(e.data, den);
    return e;
}
Entry ascii(uint16_t tag, const std::string& s) {
    Entry e{tag, kAscii, static_cast<uint32_t>(s.size() + 1), {}};
    e.data.assign(s.begin(), s.end());
    e.data.push_back(0);
    return e;
}
Entry undefinedBytes(uint16_t tag, const std::vector<uint8_t>& bytes) {
    return Entry{tag, kUndefined, static_cast<uint32_t>(bytes.size()), bytes};
}

class Output {
public:
    Output(const std::string& path, bool big) : path_(path), out_(toPath(path), std::ios::binary | std::ios::trunc), big_(big) {
        if (!out_) throw Error("cannot create " + path, ErrorKind::Io);
    }
    uint64_t pos() const { return pos_; }
    bool big() const { return big_; }
    void write(const void* data, size_t size) {
        if (!big_ && pos_ + size > 0xFFFFFFFFull) {
            // (the size of a classic file is judged from the data before it is written; data
            // that compression makes larger than it was can go beyond it)
            throw Error("TIFF output would exceed 4 GiB (classic TIFF limit); try a smaller --bits, or without --compress");
        }
        out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        pos_ += size;
        if (!out_) throw Error("write error on " + path_, ErrorKind::Io);
    }
    void align2() {
        if (pos_ & 1) {
            const uint8_t z = 0;
            write(&z, 1);
        }
    }
    // An offset: 4 bytes in a classic file, 8 in a BigTIFF file.
    void patchOffset(uint64_t at, uint64_t value) {
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(value >> (8 * i));
        out_.seekp(static_cast<std::streamoff>(at));
        out_.write(reinterpret_cast<const char*>(b), big_ ? 8 : 4);
        out_.seekp(0, std::ios::end);
        if (!out_) throw Error("write error on " + path_, ErrorKind::Io);
    }
    void close() {
        out_.close();
        if (!out_) throw Error("write error on " + path_, ErrorKind::Io);
    }

private:
    std::string path_;
    std::ofstream out_;
    bool big_;
    uint64_t pos_ = 0;
};

// Horizontal differencing (TIFF predictor 2) on one row of host-order integers.
template <class T>
void horizontalDiff(uint8_t* row, size_t samples, size_t spp) {
    T* v = reinterpret_cast<T*>(row);
    for (size_t i = samples; i-- > spp;) v[i] = static_cast<T>(v[i] - v[i - spp]);
}

// Floating point predictor (TIFF predictor 3, Adobe Tech Note 3) on one row of host-order floats.
// Output is a byte stream, independent of file byte order.
void floatingPointDiff(uint8_t* row, size_t samples, size_t spp, size_t sb, std::vector<uint8_t>& tmp) {
    tmp.assign(row, row + samples * sb);
    const bool little = hostIsLittleEndian();
    for (size_t i = 0; i < samples; ++i) {
        for (size_t b = 0; b < sb; ++b) {
            // byte b counts from the most significant byte
            row[b * samples + i] = tmp[i * sb + (little ? sb - 1 - b : b)];
        }
    }
    for (size_t i = samples * sb; i-- > spp;) row[i] = static_cast<uint8_t>(row[i] - row[i - spp]);
}

void writePage(Output& out, const TiffPage& page, bool deflate, uint64_t& prevNextPointer) {
    ImageSource& px = *page.pixels;
    if (px.width > 0xFFFFFFFFull || px.height > 0xFFFFFFFFull || px.channels > 0xFFFF) {
        throw Error("image dimensions exceed TIFF limits");
    }
    const size_t sb = sampleBytes(px.format);
    const size_t spp = static_cast<size_t>(px.channels);
    const size_t width = static_cast<size_t>(px.width);
    const size_t height = static_cast<size_t>(px.height);
    const size_t rowSamples = width * spp;
    const size_t rowBytes = rowSamples * sb;
    const size_t rowsPerStrip = std::max<size_t>(1, std::min<size_t>(height, (256 * 1024) / std::max<size_t>(rowBytes, 1)));
    const bool little = hostIsLittleEndian();
    const bool floatData = isFloat(px.format);
    // Predictor: floating point (3) for floats, horizontal differencing (2) for 8/16/32-bit
    // integers. libtiff only decodes predictor 2 on 64-bit samples since 4.4, so 64-bit integer
    // data is compressed without a predictor to stay readable by older installations.
    const uint16_t predictor = !deflate ? 1 : floatData ? 3 : (sb <= 4 ? 2 : 1);

    std::vector<uint64_t> offsets_, counts;
    std::vector<uint8_t> strip, compressed, tmp, channel;
    for (size_t y0 = 0; y0 < height; y0 += rowsPerStrip) {
        const size_t rows = std::min(rowsPerStrip, height - y0);
        strip.resize(rows * rowBytes);
        // Interleave planar channels into chunky rows (host byte order).
        if (spp == 1) {
            px.readRows(0, y0, rows, strip.data());
        } else {
            channel.resize(rows * width * sb);
            for (size_t c = 0; c < spp; ++c) {
                px.readRows(c, y0, rows, channel.data());
                for (size_t r = 0; r < rows; ++r) {
                    uint8_t* dst = strip.data() + r * rowBytes;
                    const uint8_t* src = channel.data() + r * width * sb;
                    for (size_t x = 0; x < width; ++x) std::memcpy(dst + (x * spp + c) * sb, src + x * sb, sb);
                }
            }
        }
        progressTick(rows * rowBytes);
        if (predictor != 1) {
            for (size_t r = 0; r < rows; ++r) {
                uint8_t* row = strip.data() + r * rowBytes;
                if (predictor == 3) floatingPointDiff(row, rowSamples, spp, sb, tmp);
                else if (sb == 1) horizontalDiff<uint8_t>(row, rowSamples, spp);
                else if (sb == 2) horizontalDiff<uint16_t>(row, rowSamples, spp);
                else horizontalDiff<uint32_t>(row, rowSamples, spp);
            }
        }
        // File is little-endian; the floating point predictor output is already a byte stream.
        if (!little && sb > 1 && predictor != 3) byteSwapInPlace(strip.data(), strip.size() / sb, sb);

        out.align2();
        offsets_.push_back(out.pos());
        if (deflate) {
            uLongf destLen = compressBound(static_cast<uLong>(strip.size()));
            compressed.resize(destLen);
            if (compress2(compressed.data(), &destLen, strip.data(), static_cast<uLong>(strip.size()), 6) != Z_OK) {
                throw Error("deflate compression failed");
            }
            out.write(compressed.data(), destLen);
            counts.push_back(destLen);
        } else {
            out.write(strip.data(), strip.size());
            counts.push_back(strip.size());
        }
    }

    const bool rgb = page.rgb && spp >= 3;
    const size_t colorSamples = rgb ? 3 : 1;
    std::vector<Entry> entries;
    entries.push_back(longs(256, {static_cast<uint32_t>(width)}));
    entries.push_back(longs(257, {static_cast<uint32_t>(height)}));
    entries.push_back(shorts(258, std::vector<uint16_t>(spp, static_cast<uint16_t>(sb * 8))));
    entries.push_back(shorts(259, {static_cast<uint16_t>(deflate ? 8 : 1)}));
    entries.push_back(shorts(262, {static_cast<uint16_t>(rgb ? 2 : 1)}));
    if (!page.description.empty()) entries.push_back(ascii(270, page.description));
    entries.push_back(offsets(273, offsets_, out.big()));
    entries.push_back(shorts(277, {static_cast<uint16_t>(spp)}));
    entries.push_back(longs(278, {static_cast<uint32_t>(rowsPerStrip)}));
    entries.push_back(offsets(279, counts, out.big()));
    entries.push_back(rational(282, page.xResolution));
    entries.push_back(rational(283, page.yResolution));
    entries.push_back(shorts(284, {1}));
    entries.push_back(shorts(296, {static_cast<uint16_t>(page.resolutionInCm ? 3 : 2)}));
    entries.push_back(ascii(305, std::string("xisfconv ") + kVersion));
    if (predictor != 1) entries.push_back(shorts(317, {predictor}));
    if (spp > colorSamples) {
        std::vector<uint16_t> extra(spp - colorSamples, 0);
        extra[0] = 2;  // first extra channel: unassociated alpha (PixInsight's convention)
        entries.push_back(shorts(338, extra));
    }
    entries.push_back(shorts(339, std::vector<uint16_t>(spp, static_cast<uint16_t>(floatData ? 3 : 1))));
    if (!page.iccProfile.empty()) entries.push_back(undefinedBytes(34675, page.iccProfile));
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

    out.align2();
    const uint64_t ifdOffset = out.pos();
    // A classic directory: a count of 2 bytes, entries of 12 (a value of up to 4 bytes in
    // place), the next one's offset in 4. BigTIFF: 8, 20 (8 bytes in place) and 8.
    const bool big = out.big();
    const size_t countBytes = big ? 8 : 2, entryBytes = big ? 20 : 12, valueBytes = big ? 8 : 4;
    const uint64_t ifdSize = countBytes + entryBytes * entries.size() + valueBytes;
    std::vector<uint8_t> ifd, extra;
    if (big) put64(ifd, entries.size());
    else put16(ifd, static_cast<uint16_t>(entries.size()));
    for (const auto& e : entries) {
        put16(ifd, e.tag);
        put16(ifd, e.type);
        if (big) put64(ifd, e.count);
        else put32(ifd, static_cast<uint32_t>(e.count));
        if (e.data.size() <= valueBytes) {
            std::vector<uint8_t> v = e.data;
            v.resize(valueBytes, 0);
            ifd.insert(ifd.end(), v.begin(), v.end());
        } else {
            if (extra.size() & 1) extra.push_back(0);
            const uint64_t at = ifdOffset + ifdSize + extra.size();
            if (big) {
                put64(ifd, at);
            } else {
                if (at > 0xFFFFFFFFull) throw Error("TIFF output would exceed 4 GiB");
                put32(ifd, static_cast<uint32_t>(at));
            }
            extra.insert(extra.end(), e.data.begin(), e.data.end());
        }
    }
    const uint64_t nextPointerAt = ifdOffset + countBytes + entryBytes * entries.size();
    if (big) put64(ifd, 0);
    else put32(ifd, 0);
    out.write(ifd.data(), ifd.size());
    if (!extra.empty()) out.write(extra.data(), extra.size());

    out.patchOffset(prevNextPointer, ifdOffset);
    prevNextPointer = nextPointerAt;
}

// What a page takes in the file at most, without compression: its strips and its directory.
uint64_t pageBytes(const TiffPage& page) {
    const ImageSource& px = *page.pixels;
    const uint64_t data = px.samples() * sampleBytes(px.format);
    // (with room for what Deflate makes of data that does not compress, and the tables of the strips)
    return data + data / 256 + 64 * px.height + page.iccProfile.size() + page.description.size() + 65536;
}

}  // namespace

uint64_t bigTiffThreshold() {
    // (a smaller one only for tests of BigTIFF files, which need no 4 GiB of disk then)
    if (const char* text = std::getenv("XISFCONV_BIGTIFF_ABOVE")) {
        uint64_t n = 0;
        if (parseUInt64(text, n)) return n;
    }
    return 0xFFFFFFFFull;
}

bool writeTiff(const std::string& path, const std::vector<TiffPage>& pages, bool deflate) {
    // A classic TIFF file reaches 4 GiB: one that could be larger is a BigTIFF file, which has
    // offsets of 8 bytes. (Its size is judged without compression, which seldom makes data larger.)
    uint64_t estimate = 16;
    for (const auto& page : pages) estimate += pageBytes(page);
    const bool big = estimate > bigTiffThreshold();
    Output out(path, big);
    uint64_t nextPointer = 4;  // first-IFD offset field in the file header
    if (big) {
        const uint8_t header[16] = {'I', 'I', 43, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(header, 16);
        nextPointer = 8;
    } else {
        const uint8_t header[8] = {'I', 'I', 42, 0, 0, 0, 0, 0};
        out.write(header, 8);
    }
    for (const auto& page : pages) writePage(out, page, deflate, nextPointer);
    out.close();
    return big;
}

}  // namespace xisfconv
