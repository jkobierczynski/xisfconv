// FITS reader (primary HDU + IMAGE extensions).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace xisfconv {

struct FitsImage {
    size_t hduIndex = 0;
    PixelBuffer pixels;                 // host byte order, planar, rows in the order stored in the file
    std::vector<FitsKeyword> keywords;  // every card except the structural ones (strings keep their quotes)
    std::string name;                   // EXTNAME / HDUNAME, if any
    int bitpix = 0;
    double bscale = 1, bzero = 0;
    bool hasRowOrder = false;           // ROWORDER keyword present
    bool topDown = false;               // ROWORDER = 'TOP-DOWN'
    bool hasData = false;               // false when only the header was read
    double dataMin = 0, dataMax = 0;    // finite range of floating point data (after BSCALE/BZERO)
    bool hasNaN = false;
    std::string note;                   // how the samples were mapped (for the console)
};

struct FitsFile {
    std::string path;
    uint64_t fileSize = 0;
    std::vector<FitsImage> images;
    std::vector<std::string> skipped;   // descriptions of HDUs that are not convertible images
};

// Reads all image HDUs. With headersOnly the pixel data is skipped (for --info).
// Throws xisfconv::Error on malformed files.
FitsFile readFits(const std::string& path, bool headersOnly = false);

// True if the file starts with a FITS primary header.
bool looksLikeFits(const std::string& path);

// Returns the content of a FITS string value ('...' with doubled quotes), trimmed.
// Non-string values are returned trimmed and unchanged.
std::string fitsUnquote(const std::string& value);

}  // namespace xisfconv
