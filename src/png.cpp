// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "png.hpp"

#include <zlib.h>

#include <cmath>
#include <cstdlib>
#include <fstream>

namespace xisfconv {

namespace {

void put32be(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

class ChunkWriter {
public:
    explicit ChunkWriter(const std::string& path) : path_(path), out_(toPath(path), std::ios::binary | std::ios::trunc) {
        if (!out_) throw Error("cannot create " + path, ErrorKind::Io);
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        out_.write(reinterpret_cast<const char*>(sig), 8);
    }
    void chunk(const char type[4], const uint8_t* data, size_t size) {
        if (size > 0x7FFFFFFFu) throw Error("PNG chunk too large");
        std::vector<uint8_t> head;
        put32be(head, static_cast<uint32_t>(size));
        head.insert(head.end(), type, type + 4);
        uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(type), 4);
        if (size) crc = crc32(crc, data, static_cast<uInt>(size));
        std::vector<uint8_t> tail;
        put32be(tail, static_cast<uint32_t>(crc));
        out_.write(reinterpret_cast<const char*>(head.data()), 8);
        if (size) out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        out_.write(reinterpret_cast<const char*>(tail.data()), 4);
        if (!out_) throw Error("write error on " + path_, ErrorKind::Io);
    }
    void chunk(const char type[4], const std::vector<uint8_t>& d) { chunk(type, d.data(), d.size()); }
    void close() {
        out_.close();
        if (!out_) throw Error("write error on " + path_, ErrorKind::Io);
    }

private:
    std::string path_;
    std::ofstream out_;
};

uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

// Filters one scanline, choosing the filter with the smallest sum of absolute values (the usual heuristic).
void filterRow(const uint8_t* cur, const uint8_t* prev, size_t len, size_t bpp, std::vector<uint8_t>& out,
               std::vector<uint8_t>& tmp) {
    size_t bestSum = SIZE_MAX;
    uint8_t bestType = 0;
    std::vector<uint8_t> best;
    tmp.resize(len);
    for (uint8_t type = 0; type < 5; ++type) {
        size_t sum = 0;
        for (size_t i = 0; i < len; ++i) {
            const int a = i >= bpp ? cur[i - bpp] : 0;
            const int b = prev ? prev[i] : 0;
            const int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            uint8_t pred = 0;
            switch (type) {
                case 1: pred = static_cast<uint8_t>(a); break;
                case 2: pred = static_cast<uint8_t>(b); break;
                case 3: pred = static_cast<uint8_t>((a + b) / 2); break;
                case 4: pred = paeth(a, b, c); break;
                default: break;
            }
            tmp[i] = static_cast<uint8_t>(cur[i] - pred);
            sum += tmp[i] < 128 ? tmp[i] : 256 - tmp[i];
        }
        if (sum < bestSum) {
            bestSum = sum;
            bestType = type;
            best = tmp;
        }
    }
    out.push_back(bestType);
    out.insert(out.end(), best.begin(), best.end());
}

}  // namespace

void writePng(const std::string& path, const PngImage& image, int level) {
    const PixelBuffer& px = *image.pixels;
    if (px.format != SampleFormat::UInt8 && px.format != SampleFormat::UInt16) {
        throw Error("PNG supports only 8- and 16-bit samples");
    }
    if (px.width == 0 || px.height == 0 || px.width > 0x7FFFFFFF || px.height > 0x7FFFFFFF) {
        throw Error("image dimensions exceed PNG limits");
    }
    const size_t colorCh = image.rgb ? 3 : 1;
    const size_t channels = static_cast<size_t>(px.channels);
    if (channels != colorCh && channels != colorCh + 1) throw Error("unsupported channel count for PNG");
    const bool alpha = channels == colorCh + 1;
    const uint8_t colorType = image.rgb ? (alpha ? 6 : 2) : (alpha ? 4 : 0);
    const size_t sb = sampleBytes(px.format);
    const size_t width = static_cast<size_t>(px.width), height = static_cast<size_t>(px.height);
    const size_t bpp = channels * sb;
    const size_t rowLen = width * bpp;
    const size_t plane = width * height;

    ChunkWriter w(path);
    std::vector<uint8_t> ihdr;
    put32be(ihdr, static_cast<uint32_t>(width));
    put32be(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(static_cast<uint8_t>(sb * 8));
    ihdr.push_back(colorType);
    ihdr.push_back(0);  // deflate
    ihdr.push_back(0);  // adaptive filtering
    ihdr.push_back(0);  // no interlace
    w.chunk("IHDR", ihdr);

    if (!image.iccProfile.empty()) {
        std::vector<uint8_t> iccp = {'I', 'C', 'C', ' ', 'p', 'r', 'o', 'f', 'i', 'l', 'e', 0, 0};
        uLongf len = compressBound(static_cast<uLong>(image.iccProfile.size()));
        std::vector<uint8_t> z(len);
        if (compress2(z.data(), &len, image.iccProfile.data(), static_cast<uLong>(image.iccProfile.size()), 9) == Z_OK) {
            iccp.insert(iccp.end(), z.begin(), z.begin() + static_cast<std::ptrdiff_t>(len));
            w.chunk("iCCP", iccp);
        }
    }
    if (image.pixelsPerMeter > 0) {
        std::vector<uint8_t> phys;
        const uint32_t ppm = static_cast<uint32_t>(std::lround(image.pixelsPerMeter));
        put32be(phys, ppm);
        put32be(phys, ppm);
        phys.push_back(1);  // unit: meter
        w.chunk("pHYs", phys);
    }
    {
        const std::string key = "Software";
        const std::string val = std::string("xisfconv ") + kVersion;
        std::vector<uint8_t> t(key.begin(), key.end());
        t.push_back(0);
        t.insert(t.end(), val.begin(), val.end());
        w.chunk("tEXt", t);
    }

    // Stream rows through deflate, emitting IDAT chunks of up to 1 MiB.
    z_stream zs{};
    if (deflateInit(&zs, level) != Z_OK) throw Error("deflateInit failed");
    std::vector<uint8_t> cur(rowLen), prev(rowLen), filtered, tmp, zbuf(1 << 20);
    // Compressed output accumulates in zbuf and is emitted as an IDAT chunk whenever it fills up.
    zs.next_out = zbuf.data();
    zs.avail_out = static_cast<uInt>(zbuf.size());
    auto pump = [&](int flush) {
        for (;;) {
            const int r = deflate(&zs, flush);
            if (r == Z_STREAM_ERROR) throw Error("deflate failed");
            if (zs.avail_out == 0) {
                w.chunk("IDAT", zbuf.data(), zbuf.size());
                zs.next_out = zbuf.data();
                zs.avail_out = static_cast<uInt>(zbuf.size());
                continue;
            }
            if (flush == Z_FINISH && r != Z_STREAM_END) continue;
            break;
        }
        if (flush == Z_FINISH && zs.avail_out < zbuf.size()) {
            w.chunk("IDAT", zbuf.data(), zbuf.size() - zs.avail_out);
        }
    };
    try {
        for (size_t y = 0; y < height; ++y) {
            // interleave planar channels; PNG samples are big-endian
            for (size_t c = 0; c < channels; ++c) {
                const uint8_t* src = px.data.data() + (c * plane + y * width) * sb;
                for (size_t x = 0; x < width; ++x) {
                    uint8_t* d = cur.data() + x * bpp + c * sb;
                    if (sb == 1) {
                        d[0] = src[x];
                    } else {
                        uint16_t v;
                        std::memcpy(&v, src + 2 * x, 2);
                        d[0] = static_cast<uint8_t>(v >> 8);
                        d[1] = static_cast<uint8_t>(v);
                    }
                }
            }
            filtered.clear();
            filterRow(cur.data(), y ? prev.data() : nullptr, rowLen, bpp, filtered, tmp);
            zs.next_in = filtered.data();
            zs.avail_in = static_cast<uInt>(filtered.size());
            pump(Z_NO_FLUSH);
            cur.swap(prev);
        }
        zs.next_in = nullptr;
        zs.avail_in = 0;
        pump(Z_FINISH);
    } catch (...) {
        deflateEnd(&zs);
        throw;
    }
    deflateEnd(&zs);
    w.chunk("IEND", nullptr, 0);
    w.close();
}

}  // namespace xisfconv
