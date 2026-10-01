// FITS writer (primary HDU + IMAGE extensions).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace xisfconv {

struct FitsHdu {
    const PixelBuffer* pixels = nullptr;
    std::vector<FitsKeyword> keywords;  // user keywords; structural ones are filtered out
    std::string extname;
    bool bottomUp = false;  // pixel rows already flipped to bottom-up order
};

void writeFits(const std::string& path, const std::vector<FitsHdu>& hdus);

// Formats a value as a FITS string literal ('...' with doubled quotes).
std::string fitsString(const std::string& s);
// Formats a floating point value for a FITS card (always contains '.' or 'E').
std::string fitsReal(double v);

// True for keywords the writer generates itself (SIMPLE, BITPIX, NAXISn, BZERO, ...).
bool isReservedFitsKeyword(const std::string& name);

}  // namespace xisfconv
