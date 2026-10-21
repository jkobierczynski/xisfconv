// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "png.hpp"
#include "parallel.hpp"

#include <zlib.h>

#include <algorithm>
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
    explicit ChunkWriter(const std::string& path) : path_(path), out_(toPath(path)) {
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
    OutputFile out_;
};

uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

// Filters one scanline, choosing the filter with the smallest sum of absolute values (the usual
// heuristic; of equal sums the first). Each filter in a loop of its own; `tmp` is room for the
// one tried and the best so far.
void filterRow(const uint8_t* cur, const uint8_t* prev, size_t len, size_t bpp, std::vector<uint8_t>& out,
               std::vector<uint8_t>& tmp) {
    tmp.resize(2 * len);
    uint8_t* tried = tmp.data();
    uint8_t* best = tmp.data() + len;
    size_t bestSum = SIZE_MAX;
    uint8_t bestType = 0;
    const size_t lead = std::min(bpp, len);   // the bytes with no left neighbour
    for (uint8_t type = 0; type < 5; ++type) {
        switch (type) {
            case 0:
                std::memcpy(tried, cur, len);
                break;
            case 1:
                for (size_t i = 0; i < lead; ++i) tried[i] = cur[i];
                for (size_t i = lead; i < len; ++i) tried[i] = static_cast<uint8_t>(cur[i] - cur[i - bpp]);
                break;
            case 2:
                if (prev) {
                    for (size_t i = 0; i < len; ++i) tried[i] = static_cast<uint8_t>(cur[i] - prev[i]);
                } else {
                    std::memcpy(tried, cur, len);
                }
                break;
            case 3:
                for (size_t i = 0; i < lead; ++i) tried[i] = static_cast<uint8_t>(cur[i] - (prev ? prev[i] : 0) / 2);
                for (size_t i = lead; i < len; ++i) {
                    tried[i] = static_cast<uint8_t>(cur[i] - (cur[i - bpp] + (prev ? prev[i] : 0)) / 2);
                }
                break;
            default:
                for (size_t i = 0; i < lead; ++i) tried[i] = static_cast<uint8_t>(cur[i] - paeth(0, prev ? prev[i] : 0, 0));
                if (prev) {
                    for (size_t i = lead; i < len; ++i) tried[i] = static_cast<uint8_t>(cur[i] - paeth(cur[i - bpp], prev[i], prev[i - bpp]));
                } else {
                    for (size_t i = lead; i < len; ++i) tried[i] = static_cast<uint8_t>(cur[i] - paeth(cur[i - bpp], 0, 0));
                }
                break;
        }
        size_t sum = 0;
        for (size_t i = 0; i < len; ++i) sum += tried[i] < 128 ? tried[i] : 256 - tried[i];
        if (sum < bestSum) {
            bestSum = sum;
            bestType = type;
            std::swap(tried, best);
        }
    }
    out.push_back(bestType);
    out.insert(out.end(), best, best + len);
}

}  // namespace

void writePng(const std::string& path, const PngImage& image, int level) {
    ImageSource& px = *image.pixels;
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
    std::vector<uint8_t> zbuf(1 << 20);
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
    // the rows of each channel, a band of them at a time: interleaved and filtered side by side,
    // then given to deflate one after the other, as one stream
    const size_t bandRows = static_cast<size_t>(std::min<uint64_t>(rowsPerPiece(width * sb * channels), height));
    std::vector<uint8_t> band(bandRows * width * sb * channels);
    std::vector<uint8_t> raw((bandRows + 1) * rowLen);   // the last row of the band before, then the band's
    std::vector<std::vector<uint8_t>> filteredRows(bandRows);
    try {
        for (size_t bandFirst = 0; bandFirst < height; bandFirst += bandRows) {
            const size_t bandCount = std::min(bandRows, height - bandFirst);
            for (size_t c = 0; c < channels; ++c) px.readRows(c, bandFirst, bandCount, band.data() + c * bandRows * width * sb);
            progressTick(bandCount * rowLen);
            if (bandFirst > 0) std::memcpy(raw.data(), raw.data() + bandRows * rowLen, rowLen);   // (the band before was whole)
            // A few rows at a time, while they are in the cache: some 256 KiB of them for each thread.
            const size_t step = std::max<size_t>(1, (size_t(threadLimit()) << 18) / std::max<size_t>(rowLen, 1));
            for (size_t first = 0; first < bandCount; first += step) {
                const size_t count = std::min(step, bandCount - first);
                // interleave planar channels; PNG samples are big-endian
                // (what the loops use as values of their own: written bytes cannot change them)
                const size_t w = width, nc = channels, ss = sb, pp = bpp, len = rowLen, stride = bandRows;
                uint8_t* const rows = raw.data();
                const uint8_t* const planes = band.data();
                parallelPieces(count, 8, [=](size_t from, size_t to) {
                    for (size_t r = first + from; r < first + to; ++r) {
                        uint8_t* const cur = rows + (r + 1) * len;
                        for (size_t c = 0; c < nc; ++c) {
                            const uint8_t* const src = planes + (c * stride + r) * w * ss;
                            if (ss == 1) {
                                for (size_t x = 0; x < w; ++x) cur[x * pp + c] = src[x];
                            } else {
                                for (size_t x = 0; x < w; ++x) {
                                    uint16_t v;
                                    std::memcpy(&v, src + 2 * x, 2);
                                    cur[x * pp + 2 * c] = static_cast<uint8_t>(v >> 8);
                                    cur[x * pp + 2 * c + 1] = static_cast<uint8_t>(v);
                                }
                            }
                        }
                    }
                });
                parallelPieces(count, 8, [&](size_t from, size_t to) {
                    static thread_local std::vector<uint8_t> tmp;   // (filterRow's room, one for each thread)
                    for (size_t r = first + from; r < first + to; ++r) {
                        const uint8_t* cur = raw.data() + (r + 1) * rowLen;
                        filteredRows[r].clear();
                        filterRow(cur, bandFirst + r ? cur - rowLen : nullptr, rowLen, bpp, filteredRows[r], tmp);
                    }
                });
                for (size_t r = first; r < first + count; ++r) {
                    zs.next_in = filteredRows[r].data();
                    zs.avail_in = static_cast<uInt>(filteredRows[r].size());
                    pump(Z_NO_FLUSH);
                }
            }
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
