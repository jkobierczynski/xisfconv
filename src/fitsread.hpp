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
    // ASDF input only:
    std::string source;                 // location of the array in the tree, e.g. fits[0].data
    std::string storage;                // datatype, byte order, block and compression
    bool generic = false;               // a plain array, not an HDU of a FITS-tagged node
};

struct FitsFile {
    std::string path;
    uint64_t fileSize = 0;
    std::vector<FitsImage> images;
    std::vector<std::string> skipped;   // descriptions of HDUs that are not convertible images
    std::string formatNote;             // ASDF input: versions and block count
};

// Reads all image HDUs. With headersOnly the pixel data is skipped (for --info).
// Throws xisfconv::Error on malformed files.
FitsFile readFits(const std::string& path, bool headersOnly = false);

// Sets dataMin, dataMax and hasNaN from floating point pixels (no-op for integer data).
void updateFloatRange(FitsImage& img);

// Checks the structure of every HDU and its CHECKSUM / DATASUM keywords where present.
VerifyReport verifyFits(const std::string& path);

// True if the file starts with a FITS primary header.
bool looksLikeFits(const std::string& path);

// Returns the content of a FITS string value ('...' with doubled quotes), trimmed.
// Non-string values are returned trimmed and unchanged.
std::string fitsUnquote(const std::string& value);

}  // namespace xisfconv
