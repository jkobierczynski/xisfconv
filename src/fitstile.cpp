// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "fitstile.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <zlib.h>

namespace xisfconv {

namespace {

uint64_t getBE(const uint8_t* p, size_t bytes) {
    uint64_t v = 0;
    for (size_t i = 0; i < bytes; ++i) v = (v << 8) | p[i];
    return v;
}

void putBE(uint8_t* p, uint64_t v, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) p[i] = static_cast<uint8_t>(v >> (8 * (bytes - 1 - i)));
}

// Signed integer of `bytes` big-endian bytes; single bytes are unsigned (BITPIX = 8).
int64_t getSignedBE(const uint8_t* p, size_t bytes) {
    const uint64_t v = getBE(p, bytes);
    if (bytes == 1 || bytes == 8) return static_cast<int64_t>(v);
    const uint64_t sign = 1ull << (8 * bytes - 1);
    return static_cast<int64_t>((v ^ sign)) - static_cast<int64_t>(sign);
}

size_t elementBytes(char type) {
    switch (type) {
        case 'L': case 'B': case 'A': case 'X': return 1;
        case 'I': return 2;
        case 'J': case 'E': return 4;
        case 'K': case 'D': case 'C': return 8;
        case 'M': return 16;
        default: return 0;
    }
}

// gzip or zlib stream of unknown output size: `expected` bytes are likely, `maxSize` the limit.
std::vector<uint8_t> gunzip(const uint8_t* src, size_t size, size_t expected, size_t maxSize) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 32) != Z_OK) throw Error("zlib: inflateInit failed");  // 32: detect gzip or zlib
    const size_t limit = maxSize + 1;  // one byte more, to tell "full" from "too much"
    std::vector<uint8_t> out(std::min(expected, maxSize) + 1);
    const size_t chunk = std::numeric_limits<uInt>::max() / 2;
    size_t consumed = 0, produced = 0;
    int ret = Z_OK;
    for (;;) {
        const size_t in = std::min(size - consumed, chunk), room = std::min(out.size() - produced, chunk);
        zs.next_in = const_cast<Bytef*>(src + consumed);
        zs.avail_in = static_cast<uInt>(in);
        zs.next_out = out.data() + produced;
        zs.avail_out = static_cast<uInt>(room);
        ret = inflate(&zs, Z_NO_FLUSH);
        consumed += in - zs.avail_in;
        produced += room - zs.avail_out;
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) break;
        if (produced == out.size()) {
            // more than expected: the samples may be wider than the image's (up to maxSize)
            if (out.size() >= limit) {
                ret = Z_BUF_ERROR;
                break;
            }
            out.resize(std::min(limit, out.size() * 2));
        } else if (consumed == size) {
            ret = Z_DATA_ERROR;  // the stream ends too early
            break;
        }
    }
    inflateEnd(&zs);
    if (ret == Z_BUF_ERROR || (ret == Z_STREAM_END && produced > maxSize)) throw Error("gzip: more data than the tile holds");
    if (ret != Z_STREAM_END) throw Error("gzip: damaged data");
    out.resize(produced);
    return out;
}

// Rice decompression (CFITSIO's ricecomp.c): differences of neighbouring pixels, mapped to
// non-negative numbers and Golomb-Rice coded in blocks, each with its own split position.
// Returns `count` values of `bytepix` bytes each, big-endian.
std::vector<uint8_t> riceDecode(const uint8_t* src, size_t size, size_t count, int bytepix, int blockSize) {
    int fsbits, fsmax;
    switch (bytepix) {
        case 1: fsbits = 3; fsmax = 6; break;
        case 2: fsbits = 4; fsmax = 14; break;
        case 4: fsbits = 5; fsmax = 25; break;
        default: throw Unsupported("Rice compression with " + std::to_string(bytepix) + " bytes per pixel is not supported");
    }
    if (blockSize <= 0) throw Error("Rice: invalid block size");
    const int bbits = 8 * bytepix;
    const uint32_t mask = bytepix == 4 ? 0xFFFFFFFFu : (1u << bbits) - 1;
    const size_t bytes = static_cast<size_t>(bytepix);
    // every block takes at least the fsbits bits that say how it is coded
    if (count / static_cast<size_t>(blockSize) > size * 8 / static_cast<size_t>(fsbits)) {
        throw Error("Rice: the compressed data ends too early");
    }
    std::vector<uint8_t> out(count * bytes);
    if (count == 0) return out;

    size_t pos = 0;
    bool overrun = false;
    auto next = [&]() -> uint32_t {
        if (pos < size) return src[pos++];
        overrun = true;
        return 0;
    };
    // The first pixel is stored as it is.
    uint32_t last = 0;
    for (int i = 0; i < bytepix; ++i) last = (last << 8) | next();

    uint32_t b = next();  // bit buffer
    int nbits = 8;        // number of bits in it
    for (size_t i = 0; i < count && !overrun;) {
        nbits -= fsbits;
        while (nbits < 0) {
            b = (b << 8) | next();
            nbits += 8;
        }
        const int fs = static_cast<int>(b >> nbits) - 1;
        b &= (1u << nbits) - 1;
        const size_t end = std::min(count, i + static_cast<size_t>(blockSize));
        if (fs < 0) {
            // all differences in the block are zero
            for (; i < end; ++i) putBE(out.data() + i * bytes, last, bytes);
        } else if (fs == fsmax) {
            // differences stored as plain numbers of bbits bits
            for (; i < end && !overrun; ++i) {
                int k = bbits - nbits;
                uint32_t diff = k < 32 ? b << k : 0;
                for (k -= 8; k >= 0; k -= 8) {
                    b = next();
                    diff |= b << k;
                }
                if (nbits > 0) {
                    b = next();
                    diff |= b >> (-k);
                    b &= (1u << nbits) - 1;
                } else {
                    b = 0;
                }
                diff &= mask;
                diff = (diff & 1) ? ~(diff >> 1) : diff >> 1;
                last = (diff + last) & mask;
                putBE(out.data() + i * bytes, last, bytes);
            }
        } else {
            for (; i < end && !overrun; ++i) {
                // unary part: count the zero bits before the next one bit
                while (b == 0 && !overrun) {
                    nbits += 8;
                    b = next();
                }
                if (overrun) break;
                int top = 0;  // number of significant bits in b
                for (uint32_t t = b; t; t >>= 1) ++top;
                const int nzero = nbits - top;
                nbits -= nzero + 1;
                b ^= 1u << nbits;  // drop the one bit
                // binary part: fs bits
                nbits -= fs;
                while (nbits < 0) {
                    b = (b << 8) | next();
                    nbits += 8;
                }
                uint32_t diff = (static_cast<uint32_t>(nzero) << fs) | (b >> nbits);
                b &= (1u << nbits) - 1;
                diff = (diff & 1) ? ~(diff >> 1) : diff >> 1;
                last = (diff + last) & mask;
                putBE(out.data() + i * bytes, last, bytes);
            }
        }
    }
    if (overrun) throw Error("Rice: the compressed data ends too early");
    return out;
}

// IRAF PLIO line list (pl_l2pi): run-length code for masks, a list of 16-bit instructions.
// Returns `count` values of 4 bytes each, big-endian.
std::vector<uint8_t> plioDecode(const uint8_t* src, size_t size, size_t count) {
    const size_t words = size / 2;
    auto word = [&](size_t index1) -> int {  // 1-based, as in the original
        if (index1 < 1 || index1 > words) throw Error("PLIO: the line list ends too early");
        return static_cast<int16_t>(getBE(src + 2 * (index1 - 1), 2));
    };
    std::vector<int32_t> px(count, 0);
    long lllen, llfirst;
    if (words >= 3 && word(3) > 0) {
        lllen = word(3);
        llfirst = 4;
    } else {
        lllen = static_cast<long>(word(5)) * 32768 + word(4);
        llfirst = word(2) + 1;
    }
    if (llfirst < 1) throw Error("PLIO: invalid line list");
    const long xe = static_cast<long>(count);
    long op = 1, x1 = 1;
    int64_t pv = 1;  // the current value (wide, so that damaged data cannot overflow it)
    bool skip = false;
    for (long ip = llfirst; ip <= lllen && x1 <= xe; ++ip) {
        if (skip) {
            skip = false;
            continue;
        }
        const int w = word(static_cast<size_t>(ip));
        const int opcode = (w >> 12) & 0xF;
        const int data = w & 4095;
        switch (opcode) {
            case 0: case 4: case 5: {  // run of zeros, run of the current value, zeros ending in the value
                const long x2 = x1 + data - 1;
                const long i2 = std::min(x2, xe);
                const long np = i2 - x1 + 1;
                if (np > 0) {
                    const long otop = op + np - 1;
                    if (opcode == 4) {
                        for (long i = op; i <= otop; ++i) px[static_cast<size_t>(i - 1)] = static_cast<int32_t>(pv);
                    } else if (opcode == 5 && i2 == x2) {
                        px[static_cast<size_t>(otop - 1)] = static_cast<int32_t>(pv);
                    }
                    op = otop + 1;
                }
                x1 = x2 + 1;
                break;
            }
            case 1:  // set the value: high bits in the next word
                pv = static_cast<int64_t>(word(static_cast<size_t>(ip + 1))) * 4096 + data;
                skip = true;
                break;
            case 2: pv += data; break;
            case 3: pv -= data; break;
            case 6: case 7:  // change the value and write one pixel
                pv += opcode == 6 ? data : -data;
                if (x1 <= xe) px[static_cast<size_t>(op++ - 1)] = static_cast<int32_t>(pv);
                ++x1;
                break;
            default:
                throw Error("PLIO: invalid instruction");
        }
    }
    std::vector<uint8_t> out(count * 4);
    for (size_t i = 0; i < count; ++i) putBE(out.data() + 4 * i, static_cast<uint32_t>(px[i]), 4);
    return out;
}

// The 10000 pseudo-random numbers CFITSIO uses for subtractive dithering (Park and Miller's
// "minimal standard" generator), identical on every platform by design.
const std::vector<float>& ditherNumbers() {
    static const std::vector<float> numbers = [] {
        std::vector<float> v(10000);
        const double a = 16807.0, m = 2147483647.0;
        double seed = 1;
        for (auto& x : v) {
            const double t = a * seed;
            seed = t - m * static_cast<double>(static_cast<int>(t / m));
            x = static_cast<float>(seed / m);
        }
        return v;
    }();
    return numbers;
}

constexpr int32_t kZeroValue = -2147483646;  // quantized stand-in for exactly 0.0 (SUBTRACTIVE_DITHER_2)

const TileColumn* findColumn(const TiledImage& image, const char* name) {
    for (const auto& c : image.columns)
        if (c.name == name) return &c;
    return nullptr;
}

}  // namespace

bool tileAlgorithmSupported(const std::string& algorithm) {
    return algorithm == "RICE_1" || algorithm == "RICE_ONE" || algorithm == "GZIP_1" || algorithm == "GZIP_2" ||
           algorithm == "PLIO_1" || algorithm == "NOCOMPRESS";
}

std::vector<uint8_t> decodeTiledImage(const TiledImage& image, const std::vector<uint8_t>& table) {
    if (!tileAlgorithmSupported(image.algorithm)) {
        throw Unsupported("tile compression " + (image.algorithm.empty() ? std::string("(not stated)") : image.algorithm) +
                          " is not supported" + (image.algorithm == "HCOMPRESS_1" ? "; funpack can decompress the file" : ""));
    }
    const size_t ndim = image.naxis.size();
    if (ndim == 0 || image.tile.size() != ndim) throw Error("invalid tile-compressed image dimensions");
    const int bitpix = image.bitpix;
    if (bitpix != 8 && bitpix != 16 && bitpix != 32 && bitpix != 64 && bitpix != -32 && bitpix != -64) {
        throw Error("invalid ZBITPIX " + std::to_string(bitpix));
    }
    const size_t sampleSize = static_cast<size_t>(std::abs(bitpix)) / 8;
    const bool floating = bitpix < 0;

    uint64_t pixels = 1, tiles = 1;
    std::vector<uint64_t> tilesPerAxis(ndim);
    for (size_t d = 0; d < ndim; ++d) {
        if (image.naxis[d] == 0 || image.tile[d] == 0) throw Error("invalid tile-compressed image dimensions");
        pixels = checkedMul(pixels, image.naxis[d], "image size");
        tilesPerAxis[d] = (image.naxis[d] + image.tile[d] - 1) / image.tile[d];
        tiles = checkedMul(tiles, tilesPerAxis[d], "image size");
    }
    if (tiles != image.rows) {
        throw Error("the table has " + std::to_string(image.rows) + " rows, the image needs " + std::to_string(tiles) + " tiles");
    }
    const uint64_t rowsEnd = checkedMul(image.rowBytes, image.rows, "table size");
    if (rowsEnd > table.size() || image.heapOffset > table.size()) throw Error("the compressed image table is truncated");
    const uint64_t total = checkedMul(pixels, sampleSize, "image size");
    if (total > std::numeric_limits<size_t>::max() / 2) throw Error("image too large for this platform");
    // A header that promises far more pixels than the table can hold is not worth the memory:
    // gzip expands at most 1032 times, Rice needs a few bits for every block of pixels.
    // (PLIO has no such limit: an empty mask line is a list of 7 words, however long the line.)
    if (image.algorithm != "PLIO_1") {
        long double perByte = 2;  // NOCOMPRESS; quantized pixels may become twice as wide
        if (image.algorithm == "GZIP_1" || image.algorithm == "GZIP_2") perByte = 2 * 1032;
        else if (image.algorithm != "NOCOMPRESS") perByte = 8.0L / 3 * std::max(image.riceBlockSize, 1) * static_cast<long double>(sampleSize);
        if (static_cast<long double>(total) > perByte * static_cast<long double>(table.size()) + 2880) {
            throw Error("the image size in the header is implausible for the stored data");
        }
    }

    const TileColumn* compressed = findColumn(image, "COMPRESSED_DATA");
    const TileColumn* gzipped = findColumn(image, "GZIP_COMPRESSED_DATA");
    const TileColumn* plain = findColumn(image, "UNCOMPRESSED_DATA");
    const TileColumn* scaleColumn = findColumn(image, "ZSCALE");
    const TileColumn* zeroColumn = findColumn(image, "ZZERO");
    const TileColumn* blankColumn = findColumn(image, "ZBLANK");
    if (!compressed || !compressed->variable) throw Error("the compressed image table has no COMPRESSED_DATA column");
    for (const auto& c : image.columns) {
        if (c.offset > image.rowBytes) throw Error("invalid column layout in the compressed image table");
    }
    // Floating point pixels are either stored as they are (lossless, gzip only) or quantized to
    // 32-bit integers with a scale and zero point per tile.
    // CFITSIO marks the first case with ZQUANTIZ = 'NONE'; astropy leaves out the ZSCALE and ZZERO columns.
    const bool gzipLike = image.algorithm == "GZIP_1" || image.algorithm == "GZIP_2" || image.algorithm == "NOCOMPRESS";
    const bool lossless = image.quantize == "NONE" || (gzipLike && !scaleColumn && !zeroColumn && !image.hasScale);
    const bool quantized = floating && !lossless;
    const int dither = image.quantize == "SUBTRACTIVE_DITHER_1" ? 1 : image.quantize == "SUBTRACTIVE_DITHER_2" ? 2 : 0;
    const std::vector<float>& randoms = ditherNumbers();

    // Bytes of a variable-length array field of a row.
    auto heapField = [&](const TileColumn& column, uint64_t row, const uint8_t*& data, uint64_t& bytes) {
        const size_t width = column.wide ? 8 : 4;
        const uint64_t at = row * image.rowBytes + column.offset;
        if (column.offset + 2 * width > image.rowBytes) throw Error("invalid column layout in the compressed image table");
        const uint64_t count = getBE(table.data() + at, width);
        const uint64_t offset = getBE(table.data() + at + width, width);
        bytes = checkedMul(count, elementBytes(column.type), "tile size");
        if (offset > table.size() - image.heapOffset || bytes > table.size() - image.heapOffset - offset) {
            throw Error("tile " + std::to_string(row) + " lies beyond the end of the table (truncated file?)");
        }
        data = table.data() + image.heapOffset + offset;
    };
    auto fixedField = [&](const TileColumn& column, uint64_t row) -> const uint8_t* {
        if (column.variable || column.offset + elementBytes(column.type) > image.rowBytes) {
            throw Error("invalid column layout in the compressed image table");
        }
        return table.data() + row * image.rowBytes + column.offset;
    };
    auto fieldNumber = [&](const TileColumn& column, uint64_t row) -> double {
        const uint8_t* p = fixedField(column, row);
        switch (column.type) {
            case 'D': { uint64_t u = getBE(p, 8); double d; std::memcpy(&d, &u, 8); return d; }
            case 'E': { uint32_t u = static_cast<uint32_t>(getBE(p, 4)); float f; std::memcpy(&f, &u, 4); return f; }
            case 'B': return static_cast<double>(p[0]);
            case 'I': case 'J': case 'K': return static_cast<double>(getSignedBE(p, elementBytes(column.type)));
            default: throw Error("unsupported column type in the compressed image table");
        }
    };

    std::vector<uint8_t> out(static_cast<size_t>(total));
    std::vector<uint64_t> index(ndim, 0), start(ndim), size(ndim);
    for (uint64_t row = 0; row < image.rows; ++row) {
        // Position and size of this tile; tiles at the upper edges may be smaller.
        uint64_t r = row, tilePixels = 1;
        for (size_t d = 0; d < ndim; ++d) {
            index[d] = r % tilesPerAxis[d];
            r /= tilesPerAxis[d];
            start[d] = index[d] * image.tile[d];
            size[d] = std::min(image.tile[d], image.naxis[d] - start[d]);
            tilePixels *= size[d];
        }
        const size_t n = static_cast<size_t>(tilePixels);
        const std::string label = "tile " + std::to_string(row);

        // The tile as integers or raw samples: `values`, n elements of `width` big-endian bytes.
        std::vector<uint8_t> values;
        size_t width = 0;
        bool raw = false;  // already samples of the image's type (not quantized)
        const uint8_t* data = nullptr;
        uint64_t bytes = 0;
        heapField(*compressed, row, data, bytes);
        try {
            if (bytes > 0) {
                if (image.algorithm == "RICE_1" || image.algorithm == "RICE_ONE") {
                    values = riceDecode(data, static_cast<size_t>(bytes), n, image.riceBytePix, image.riceBlockSize);
                    width = static_cast<size_t>(image.riceBytePix);
                } else if (image.algorithm == "PLIO_1") {
                    values = plioDecode(data, static_cast<size_t>(bytes), n);
                    width = 4;
                } else {
                    if (image.algorithm == "NOCOMPRESS") values.assign(data, data + bytes);
                    else values = gunzip(data, static_cast<size_t>(bytes), n * (floating && quantized ? 4 : sampleSize), n * 8);
                    if (n == 0 || values.size() % n != 0) throw Error("unexpected amount of data");
                    width = values.size() / n;
                    if (width != 1 && width != 2 && width != 4 && width != 8) throw Error("unexpected amount of data");
                    if (image.algorithm == "GZIP_2" && width > 1) {
                        // the bytes were regrouped by significance: all first bytes, all second bytes, ...
                        std::vector<uint8_t> plainOrder(values.size());
                        for (size_t k = 0; k < width; ++k)
                            for (size_t i = 0; i < n; ++i) plainOrder[i * width + k] = values[k * n + i];
                        values.swap(plainOrder);
                    }
                    raw = floating && !quantized;
                }
            } else if (gzipped && gzipped->variable && (heapField(*gzipped, row, data, bytes), bytes > 0)) {
                // a tile that could not be quantized: its samples, gzip-compressed
                values = gunzip(data, static_cast<size_t>(bytes), n * sampleSize, n * sampleSize);
                width = sampleSize;
                raw = true;
            } else if (plain && plain->variable && (heapField(*plain, row, data, bytes), bytes > 0)) {
                values.assign(data, data + bytes);
                width = elementBytes(plain->type);
                raw = floating && ((plain->type == 'E' && bitpix == -32) || (plain->type == 'D' && bitpix == -64));
                if (floating && !raw) throw Error("unexpected type of uncompressed tile data");
            } else {
                throw Error("no data");
            }
            if (width == 0 || values.size() != n * width) throw Error("unexpected amount of data");
        } catch (const Unsupported&) {
            throw;
        } catch (const Error& e) {
            throw Error(label + ": " + e.what());
        }

        // Convert to samples of the image's type.
        std::vector<uint8_t> samples;
        if (raw || (!floating && width == sampleSize)) {
            if (width != sampleSize) throw Error(label + ": unexpected sample size");
            samples.swap(values);
        } else if (!floating) {
            samples.resize(n * sampleSize);
            for (size_t i = 0; i < n; ++i) {
                putBE(samples.data() + i * sampleSize, static_cast<uint64_t>(getSignedBE(values.data() + i * width, width)), sampleSize);
            }
        } else {
            if (width != 4) throw Error(label + ": quantized pixels must be 32-bit integers");
            const double scale = scaleColumn ? fieldNumber(*scaleColumn, row) : image.scale;
            const double zero = zeroColumn ? fieldNumber(*zeroColumn, row) : image.zero;
            // Undefined pixels: the integer named by ZBLANK (column or keyword), CFITSIO's default otherwise.
            double blankValue = image.hasBlank ? static_cast<double>(image.blank) : -2147483647.0;
            if (blankColumn) blankValue = fieldNumber(*blankColumn, row);
            const bool hasBlank = blankValue >= -2147483648.0 && blankValue <= 2147483647.0;  // false for NaN, too
            const int32_t blank = hasBlank ? static_cast<int32_t>(blankValue) : 0;
            // CFITSIO's sequence: the tile number and ZDITHER0 pick where in the table to start.
            size_t seed = static_cast<size_t>((row + static_cast<uint64_t>(image.ditherSeed > 0 ? image.ditherSeed - 1 : 0)) % 10000);
            size_t nextRandom = static_cast<size_t>(randoms[seed] * 500);
            samples.resize(n * sampleSize);
            for (size_t i = 0; i < n; ++i) {
                const int32_t q = static_cast<int32_t>(static_cast<uint32_t>(getBE(values.data() + 4 * i, 4)));
                double v;
                if (hasBlank && q == blank) v = std::numeric_limits<double>::quiet_NaN();
                else if (dither == 2 && q == kZeroValue) v = 0.0;
                else if (dither) v = (static_cast<double>(q) - static_cast<double>(randoms[nextRandom]) + 0.5) * scale + zero;
                else v = static_cast<double>(q) * scale + zero;
                if (dither && ++nextRandom == 10000) {
                    if (++seed == 10000) seed = 0;
                    nextRandom = static_cast<size_t>(randoms[seed] * 500);
                }
                if (bitpix == -32) {
                    const float f = static_cast<float>(v);
                    uint32_t u;
                    std::memcpy(&u, &f, 4);
                    putBE(samples.data() + 4 * i, u, 4);
                } else {
                    uint64_t u;
                    std::memcpy(&u, &v, 8);
                    putBE(samples.data() + 8 * i, u, 8);
                }
            }
        }

        // Copy the tile into the image, one line along the first axis at a time.
        const size_t lineBytes = static_cast<size_t>(size[0]) * sampleSize;
        const uint64_t lines = tilePixels / size[0];
        std::vector<uint64_t> at(ndim, 0);  // position within the tile, axes 1..n-1
        for (uint64_t line = 0; line < lines; ++line) {
            uint64_t dest = 0, stride = 1;
            for (size_t d = 0; d < ndim; ++d) {
                dest += (start[d] + (d == 0 ? 0 : at[d])) * stride;
                stride *= image.naxis[d];
            }
            std::memcpy(out.data() + static_cast<size_t>(dest) * sampleSize, samples.data() + static_cast<size_t>(line) * lineBytes, lineBytes);
            for (size_t d = 1; d < ndim; ++d) {
                if (++at[d] < size[d]) break;
                at[d] = 0;
            }
        }
    }
    return out;
}

}  // namespace xisfconv
