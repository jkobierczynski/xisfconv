// Sample format conversion and geometric helpers.
// SPDX-License-Identifier: GPL-3.0-or-later
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

}  // namespace xisfconv
