// Baseline TIFF writer (little-endian, chunky, optional Deflate + predictor), and BigTIFF for
// files that may be larger than 4 GiB.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "imagesource.hpp"

namespace xisfconv {

struct TiffPage {
    Source pixels;                  // read a piece at a time as the page is written
    bool rgb = false;               // photometric RGB (else grayscale); extra channels become alpha/extra samples
    std::vector<uint8_t> iccProfile;
    double xResolution = 72, yResolution = 72;
    bool resolutionInCm = false;
    std::string description;
};

// Returns true if the file is a BigTIFF file: one whose pages could take more than 4 GiB.
bool writeTiff(const std::string& path, const std::vector<TiffPage>& pages, bool deflate);

}  // namespace xisfconv
