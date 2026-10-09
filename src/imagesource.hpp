// An image that is read a piece at a time: rows of one channel, from wherever they are (a file,
// a temporary file, memory) and through whatever is done to them on the way (another sample
// format, a stretch, the rows turned over). A conversion reads its input this way and writes its
// output from it, so that no image needs to be held whole.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "bytes.hpp"
#include "common.hpp"

namespace xisfconv {

class ImageSource {
public:
    virtual ~ImageSource() = default;
    uint64_t width = 0, height = 0, channels = 0;
    SampleFormat format = SampleFormat::UInt8;

    // Rows [y, y + rows) of channel `channel`, top to bottom, each left to right, in host byte
    // order: rows * width samples. (Within the image; throws Error for what is wrong with the
    // data, as a reader does.)
    virtual void readRows(uint64_t channel, uint64_t y, uint64_t rows, uint8_t* out) = 0;

    uint64_t rowBytes() const { return width * sampleBytes(format); }
    uint64_t samples() const { return width * height * channels; }
};
using Source = std::shared_ptr<ImageSource>;

// How many rows of `rowBytes` bytes make a piece (at least one).
uint64_t rowsPerPiece(uint64_t rowBytes);

// Calls `fn` for each piece of the image: channel after channel, bands of rows top to bottom.
// `rows`: rows of a piece; 0: rowsPerPiece. A sign of life is given on the way (progressTick).
using BandFunction = std::function<void(uint64_t channel, uint64_t y, uint64_t rows, const uint8_t* data)>;
void forEachBand(ImageSource& source, const BandFunction& fn, uint64_t rows = 0);

// The whole image in memory, as the readers have it (planar, host byte order).
PixelBuffer readAll(ImageSource& source);

// An image in memory: the buffer is kept by the source (shared), or by the caller (borrowed: it
// must outlive the source and not change).
Source bufferSource(std::shared_ptr<const PixelBuffer> pixels);
Source borrowedSource(const PixelBuffer& pixels);

// The bytes a data block stores the pixels in, without compression: channel after channel
// (planar) or pixel after pixel (interleaved, "normal"), in either byte order.
struct StoredLayout {
    bool planar = true;
    bool swap = false;   // samples of more than one byte are in the other byte order than the host's
};
Source storedSource(std::shared_ptr<RandomBytes> bytes, uint64_t width, uint64_t height, uint64_t channels, SampleFormat format,
                    StoredLayout layout);

// The bytes of a block that was byte-shuffled with items of `itemSize` bytes, as they were
// before: [all first bytes][all second bytes]... is read as the items one after the other.
// (What does not make a whole item at the end stays where it is.)
std::shared_ptr<RandomBytes> unshuffledBytes(std::shared_ptr<RandomBytes> shuffled, uint64_t itemSize);

// The other way: the bytes as byte shuffling makes them of items of `itemSize` bytes, read from
// the items as they are. (A piece of the shuffled bytes is a byte of each of a run of items:
// reading all of them reads the items itemSize times.)
std::shared_ptr<RandomBytes> shuffledBytes(std::shared_ptr<RandomBytes> items, uint64_t itemSize);

// The rows in the other order.
Source flippedSource(Source source);

// Channels [first, first + count) of an image.
Source channelSource(Source source, uint64_t first, uint64_t count);

// Samples made from those of another image, a piece at a time: `map` gets `count` samples of
// channel `channel` in the format of `source` and writes as many in `format`.
using SampleMap = std::function<void(uint64_t channel, const uint8_t* in, uint8_t* out, size_t count)>;
Source mappedSource(Source source, SampleFormat format, SampleMap map);

// The finite range of floating point samples and whether any is not finite (NaN, Inf). For an
// image of integers: 0, 0, false.
struct FloatRange {
    double min = 0, max = 0;
    bool hasNaN = false;
};
FloatRange floatRange(ImageSource& source);

}  // namespace xisfconv
