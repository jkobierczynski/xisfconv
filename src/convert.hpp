// Sample format conversion and geometric helpers.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include "common.hpp"

namespace xisfconv {

// Converts pixel samples to `target`.
//  int  -> int   : rescaled over the full ranges (e.g. 16-bit 65535 -> 8-bit 255)
//  int  -> float : normalized to [0,1]
//  float-> int   : [lower,upper] mapped to the full integer range, clipped
//  float-> float : values copied unchanged
void convertSampleFormat(PixelBuffer& px, SampleFormat target, double lower, double upper);

// Histogram transformation in PixInsight's STF form, on values normalized to [0,1]:
//   x1 = clip((x - shadows) / (highlights - shadows))
//   x2 = MTF(midtones, x1)          (midtones transfer function)
//   y  = clip((x2 - low) / (high - low))
struct StretchParams {
    double shadows = 0, midtones = 0.5, highlights = 1, low = 0, high = 1;
};

// PixInsight-style auto-STF (shadows at median - 2.8 * normalized MAD, median mapped to 0.25).
// Computes one parameter set per channel for the first `colorChannels` channels; with `linked`
// all of them share the averaged statistics, which preserves the color balance.
std::vector<StretchParams> autoStretch(const PixelBuffer& px, double lower, double upper, size_t colorChannels,
                                       bool linked);

// Applies the stretch to the first params.size() channels; remaining (alpha) channels are only
// normalized. The result is Float32 in [0,1].
void applyStretch(PixelBuffer& px, const std::vector<StretchParams>& params, double lower, double upper);

// Maps floating point samples from [lower,upper] to [0,1] without clipping (no-op for integers).
void normalizeFloat(PixelBuffer& px, double lower, double upper);

// Reverses the row order of every channel plane.
void flipVertical(PixelBuffer& px);

// A smaller picture of an image, for TIFF and PNG export.
struct Downsample {
    uint64_t bin = 1;                       // n x n pixels become one; what is left over at the right and the bottom is dropped
    uint64_t fitWidth = 0, fitHeight = 0;   // the picture is to fit this many pixels (0: no limit), its proportions kept
    double scale = 0;                       // the picture is this fraction of the image in width and height (0: not asked for)
    // True if a smaller picture was asked for (also if the image turns out to be small enough as it is).
    bool any() const { return bin > 1 || fitWidth || fitHeight || scale > 0; }
};

// What a Downsample makes of an image of a given size: the part of the image that is used (all
// of it, but for what binning leaves over) and the size of the result. A picture is never
// larger than the image.
struct DownsampledSize {
    uint64_t useWidth = 0, useHeight = 0;
    uint64_t width = 0, height = 0;
    bool changes = false;   // false: the image stays as it is
};
DownsampledSize downsampledSize(const Downsample& how, uint64_t width, uint64_t height);

// Replaces the image by the picture of that size. Every pixel of the picture is the mean of
// the part of the image it covers, each pixel of the image counted by the share of it that is
// covered: no pixel is left out or counted twice, so the mean of the image stays what it is
// and nothing is sharpened or rings. Binning is the case where every share is a whole pixel.
// Integers are rounded to the nearest value. Floating point samples that are not finite are
// left out of the mean; a pixel that covers no finite sample is NaN. The sums are doubles: a
// mean of 64-bit samples, integers or floating point, is right to their last bit or two.
void downsample(PixelBuffer& px, const DownsampledSize& size);

}  // namespace xisfconv
