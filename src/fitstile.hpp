// Tile-compressed FITS images (the "tiled image compression convention", as written by fpack,
// CFITSIO and astropy): decompression, and the two lossless compressors the writer uses.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"

namespace xisfconv {

// A column of the binary table that holds the compressed image.
struct TileColumn {
    std::string name;      // TTYPEn: COMPRESSED_DATA, GZIP_COMPRESSED_DATA, UNCOMPRESSED_DATA, ZSCALE, ZZERO, ZBLANK
    char type = 'B';       // element type: B, I, J, K, E, D, ...
    bool variable = false; // variable-length array (P or Q descriptor in the row, data in the heap)
    bool wide = false;     // Q descriptor (64-bit) instead of P (32-bit)
    uint64_t repeat = 1;
    uint64_t offset = 0;   // byte offset of the field in a row
};

// What the header of the table says about the image and its compression.
struct TiledImage {
    std::string algorithm;          // ZCMPTYPE: RICE_1, GZIP_1, GZIP_2, PLIO_1, HCOMPRESS_1, NOCOMPRESS
    int bitpix = 0;                 // ZBITPIX
    std::vector<uint64_t> naxis;    // ZNAXISn
    std::vector<uint64_t> tile;     // ZTILEn
    int riceBlockSize = 32;
    int riceBytePix = 4;
    std::string quantize;           // ZQUANTIZ: NONE, NO_DITHER, SUBTRACTIVE_DITHER_1, SUBTRACTIVE_DITHER_2 (or empty)
    long long ditherSeed = 0;       // ZDITHER0
    bool hasBlank = false;          // ZBLANK keyword: the integer that stands for an undefined pixel
    long long blank = 0;
    bool hasScale = false;          // ZSCALE / ZZERO keywords (used when the columns are absent)
    double scale = 1, zero = 0;

    uint64_t rowBytes = 0, rows = 0;  // NAXIS1, NAXIS2 of the table
    uint64_t heapOffset = 0;          // THEAP: start of the heap, from the start of the data unit
    std::vector<TileColumn> columns;
};

// True for the keywords that describe the table and the compression, not the image (TFORMn,
// ZCMPTYPE, ZTILEn, ZSCALE, ...). `name` is in upper case.
bool isTileKeyword(const std::string& name);

// True for the algorithms decodeTiledImage implements.
bool tileAlgorithmSupported(const std::string& algorithm);

// Decompresses the image. `table` is the data unit of the binary table (rows and heap).
// Returns the image as an uncompressed FITS data unit would hold it: big-endian samples of the
// type ZBITPIX names, first axis fastest. Floating point images that were quantized come back
// as the values CFITSIO restores (including its subtractive dithering).
// Throws Unsupported for algorithms that are not implemented, Error for damaged data.
std::vector<uint8_t> decodeTiledImage(const TiledImage& image, const std::vector<uint8_t>& table);

// The block size the writer uses for RICE_1, which is CFITSIO's.
constexpr int kRiceBlockSize = 32;

// Rice compression of `count` integers of `bytepix` bytes each (1, 2 or 4), big-endian: the
// first value as it is, then the differences of neighbours in blocks of `blockSize`.
// The result is byte for byte what CFITSIO's ricecomp.c produces. It is never much larger than
// the input: riceBound says how large at most.
std::vector<uint8_t> riceEncode(const uint8_t* values, size_t count, int bytepix, int blockSize = kRiceBlockSize);
uint64_t riceBound(uint64_t count, int bytepix, int blockSize = kRiceBlockSize);

// gzip compression of the tiles of an image, one after the other (GZIP_1, and GZIP_2 when the
// bytes of the samples are regrouped first: all most significant bytes, then the next ones...).
// The header of the stream is the same on every system.
class TileGzip {
public:
    TileGzip();
    ~TileGzip();
    TileGzip(const TileGzip&) = delete;
    TileGzip& operator=(const TileGzip&) = delete;

    // `samples`: `count` values of `sampleSize` bytes each, big-endian.
    std::vector<uint8_t> compress(const uint8_t* samples, size_t count, size_t sampleSize, bool regroup);
    // Upper limit of the size of a compressed tile of `bytes` bytes.
    static uint64_t bound(uint64_t bytes);

private:
    struct State;
    State* state_;
    std::vector<uint8_t> regrouped_;
};

}  // namespace xisfconv
