// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "tiff.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace xisfconv {

namespace {

enum : uint16_t { kShort = 3, kLong = 4, kRational = 5, kAscii = 2, kUndefined = 7 };

struct Entry {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
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
Entry longs(uint16_t tag, const std::vector<uint32_t>& vals) {
    Entry e{tag, kLong, static_cast<uint32_t>(vals.size()), {}};
    for (auto v : vals) put32(e.data, v);
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
    explicit Output(const std::string& path) : path_(path), out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) throw Error("cannot create " + path);
    }
    uint64_t pos() const { return pos_; }
    void write(const void* data, size_t size) {
        if (pos_ + size > 0xFFFFFFFFull) {
            throw Error("TIFF output would exceed 4 GiB (classic TIFF limit); try --compress or a smaller --bits");
        }
        out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        pos_ += size;
        if (!out_) throw Error("write error on " + path_);
    }
    void align2() {
        if (pos_ & 1) {
            const uint8_t z = 0;
            write(&z, 1);
        }
    }
    void patch32(uint64_t at, uint32_t value) {
        uint8_t b[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
        out_.seekp(static_cast<std::streamoff>(at));
        out_.write(reinterpret_cast<const char*>(b), 4);
        out_.seekp(0, std::ios::end);
        if (!out_) throw Error("write error on " + path_);
    }
    void close() {
        out_.close();
        if (!out_) throw Error("write error on " + path_);
    }

private:
    std::string path_;
    std::ofstream out_;
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
    const PixelBuffer& px = *page.pixels;
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
    const size_t plane = width * height;
    const bool little = hostIsLittleEndian();
    const bool floatData = isFloat(px.format);
    // Predictor: floating point (3) for floats, horizontal differencing (2) for 8/16/32-bit
    // integers. libtiff only decodes predictor 2 on 64-bit samples since 4.4, so 64-bit integer
    // data is compressed without a predictor to stay readable by older installations.
    const uint16_t predictor = !deflate ? 1 : floatData ? 3 : (sb <= 4 ? 2 : 1);

    std::vector<uint32_t> offsets, counts;
    std::vector<uint8_t> strip, compressed, tmp;
    for (size_t y0 = 0; y0 < height; y0 += rowsPerStrip) {
        const size_t rows = std::min(rowsPerStrip, height - y0);
        strip.resize(rows * rowBytes);
        // Interleave planar channels into chunky rows (host byte order).
        for (size_t r = 0; r < rows; ++r) {
            uint8_t* dst = strip.data() + r * rowBytes;
            const size_t base = (y0 + r) * width;
            if (spp == 1) {
                std::memcpy(dst, px.data.data() + base * sb, rowBytes);
            } else {
                for (size_t c = 0; c < spp; ++c) {
                    const uint8_t* src = px.data.data() + (c * plane + base) * sb;
                    for (size_t x = 0; x < width; ++x) std::memcpy(dst + (x * spp + c) * sb, src + x * sb, sb);
                }
            }
        }
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
        offsets.push_back(static_cast<uint32_t>(out.pos()));
        if (deflate) {
            uLongf destLen = compressBound(static_cast<uLong>(strip.size()));
            compressed.resize(destLen);
            if (compress2(compressed.data(), &destLen, strip.data(), static_cast<uLong>(strip.size()), 6) != Z_OK) {
                throw Error("deflate compression failed");
            }
            out.write(compressed.data(), destLen);
            counts.push_back(static_cast<uint32_t>(destLen));
        } else {
            out.write(strip.data(), strip.size());
            counts.push_back(static_cast<uint32_t>(strip.size()));
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
    entries.push_back(longs(273, offsets));
    entries.push_back(shorts(277, {static_cast<uint16_t>(spp)}));
    entries.push_back(longs(278, {static_cast<uint32_t>(rowsPerStrip)}));
    entries.push_back(longs(279, counts));
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
    const uint64_t ifdSize = 2 + 12 * entries.size() + 4;
    std::vector<uint8_t> ifd, extra;
    put16(ifd, static_cast<uint16_t>(entries.size()));
    for (const auto& e : entries) {
        put16(ifd, e.tag);
        put16(ifd, e.type);
        put32(ifd, e.count);
        if (e.data.size() <= 4) {
            std::vector<uint8_t> v = e.data;
            v.resize(4, 0);
            ifd.insert(ifd.end(), v.begin(), v.end());
        } else {
            if (extra.size() & 1) extra.push_back(0);
            const uint64_t at = ifdOffset + ifdSize + extra.size();
            if (at > 0xFFFFFFFFull) throw Error("TIFF output would exceed 4 GiB");
            put32(ifd, static_cast<uint32_t>(at));
            extra.insert(extra.end(), e.data.begin(), e.data.end());
        }
    }
    const uint64_t nextPointerAt = ifdOffset + 2 + 12 * entries.size();
    put32(ifd, 0);
    out.write(ifd.data(), ifd.size());
    if (!extra.empty()) out.write(extra.data(), extra.size());

    out.patch32(prevNextPointer, static_cast<uint32_t>(ifdOffset));
    prevNextPointer = nextPointerAt;
}

}  // namespace

void writeTiff(const std::string& path, const std::vector<TiffPage>& pages, bool deflate) {
    Output out(path);
    const uint8_t header[8] = {'I', 'I', 42, 0, 0, 0, 0, 0};
    out.write(header, 8);
    uint64_t nextPointer = 4;  // first-IFD offset field in the file header
    for (const auto& page : pages) writePage(out, page, deflate, nextPointer);
    out.close();
}

}  // namespace xisfconv
