// FITS reader (primary HDU + IMAGE extensions).
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "property.hpp"

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
    std::string tileCompression;        // ZCMPTYPE if the image was stored tile-compressed (RICE_1, GZIP_1, ...)
    // ASDF input only:
    std::string source;                 // location of the array in the tree, e.g. fits[0].data
    std::string storage;                // datatype, byte order, block and compression
    bool generic = false;               // a plain array, not an HDU of a FITS-tagged node
    // Images handed over in memory (the C API's writer) only:
    std::optional<std::pair<double, double>> bounds;  // range of floating point samples, if the caller states it
    std::vector<uint8_t> iccProfile;    // written to TIFF and PNG
    // The XISF properties the image carries: from the table behind it in a FITS file, from the
    // tree of an ASDF file (both written by a conversion from XISF).
    std::vector<Property> properties;
    std::string wcsDigest;              // the WCS keywords an astrometric solution among them was written with
    // Images handed over in memory only: the properties are the caller's own. An astrometric
    // solution among them is the caller's word and is written as it is; without one, a solution
    // is made from the WCS keywords as for any image.
    bool propertiesGiven = false;
    // Images handed over in memory only: the row order the WCS keywords describe, where it is
    // not that of the pixels. (They are turned to the order of the file when it is written, once:
    // turned to the pixels first and from there to the file, a number would be computed twice
    // and not come back as it was.)
    std::optional<bool> wcsTopDown;
    // DNG input only: the colour filter array, a letter per cell (R, G, B, C, M, Y, W), row after
    // row of cfaWidth by cfaHeight cells, relative to the first pixel of the image.
    std::string cfaPattern;
    int cfaWidth = 0, cfaHeight = 0;
};

struct FitsFile {
    std::string path;
    uint64_t fileSize = 0;
    std::vector<FitsImage> images;
    std::vector<std::string> skipped;   // descriptions of HDUs that are not convertible images
    std::string formatNote;             // ASDF input: versions and block count
    std::vector<Property> properties;   // the XISF properties of the file the images were converted from, if carried
};

// Reads all image HDUs. With headersOnly the pixel data is skipped (for --info); with onlyImage
// the pixels of that one image are read and the others are left as headers. The XISF properties
// a file carries are read in both cases but the last.
// Throws xisfconv::Error on malformed files.
FitsFile readFits(const std::string& path, bool headersOnly = false, std::optional<size_t> onlyImage = std::nullopt);

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
