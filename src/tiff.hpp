// Baseline TIFF writer (little-endian, chunky, optional Deflate + predictor).
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace xisfconv {

struct TiffPage {
    const PixelBuffer* pixels = nullptr;
    bool rgb = false;               // photometric RGB (else grayscale); extra channels become alpha/extra samples
    std::vector<uint8_t> iccProfile;
    double xResolution = 72, yResolution = 72;
    bool resolutionInCm = false;
    std::string description;
};

void writeTiff(const std::string& path, const std::vector<TiffPage>& pages, bool deflate);

}  // namespace xisfconv
