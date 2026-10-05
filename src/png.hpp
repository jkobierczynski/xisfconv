// PNG writer (8/16-bit gray, gray+alpha, RGB, RGBA).
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace xisfconv {

struct PngImage {
    const PixelBuffer* pixels = nullptr;  // UInt8 or UInt16, 1-4 channels
    bool rgb = false;                     // color image (else grayscale); 4th/2nd channel = alpha
    std::vector<uint8_t> iccProfile;
    double pixelsPerMeter = 0;            // 0 = no pHYs chunk
};

void writePng(const std::string& path, const PngImage& image, int compressionLevel = 6);

}  // namespace xisfconv
