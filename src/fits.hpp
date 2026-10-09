// FITS writer (primary HDU + IMAGE extensions, or tile-compressed images).
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "imagesource.hpp"
#include "property.hpp"

namespace xisfconv {

struct FitsHdu {
    Source pixels;                      // read a piece at a time as the image is written
    std::vector<FitsKeyword> keywords;  // user keywords; structural ones are filtered out
    std::string extname;
    bool bottomUp = false;  // pixel rows already flipped to bottom-up order
    // The XISF properties the image brings along: written as a binary table behind the image
    // (see kPropertyTable). Empty: no table.
    std::vector<Property> properties;
    // What the WCS keywords looked like when the properties were taken from an XISF file
    // (wcsDigest), if they hold an astrometric solution: it goes with them.
    std::string wcsDigest;
};

// XISF properties in a FITS file: a binary table extension with a row per property, named
// XISF_PROPERTIES behind the image it belongs to and XISF_METADATA for the properties of the
// file. The columns: ID and TYPE (the XISF type name) as text; BLOCK, true if the value is what
// XISF keeps in a data block (a vector, a matrix, a String that was stored as a block); ROWS
// and COLUMNS (the shape of a matrix; the length of a vector in ROWS); VALUE, COMMENT and
// FORMAT, variable-length arrays of bytes: UTF-8 text, and in VALUE the elements of a vector
// or matrix as little-endian numbers, row after row. This is not a
// convention anyone else reads; it is plain FITS, though, and any program that reads tables can
// show it.
constexpr const char* kPropertyTable = "XISF_PROPERTIES";
constexpr const char* kMetadataTable = "XISF_METADATA";
constexpr const char* kWcsDigestKeyword = "WCSDIGST";

// How the images are stored. With tile compression (the "tiled image compression convention" of
// the FITS standard, the format of fpack) each image is a binary table that holds its rows
// compressed one by one, behind a primary HDU without data. Both choices are lossless.
// Images of 64-bit integers stay plain images (CFITSIO has no tile compression for them).
enum class FitsTiles {
    None,     // plain images: the primary HDU and IMAGE extensions
    Default,  // RICE_1 for integers, GZIP_2 for floating point
    Gzip      // GZIP_2 for every sample type (GZIP_1 for single bytes, which is the same)
};

struct FitsWriteOptions {
    FitsTiles tiles = FitsTiles::None;
    std::vector<Property> metadata;   // the XISF properties of the file: a table at the end. Empty: none.
};

void writeFits(const std::string& path, const std::vector<FitsHdu>& hdus, const FitsWriteOptions& options = {});

// The cards the writer makes of the keywords of an image, 80 characters each, one after the
// other: long strings on CONTINUE cards (announced by a LONGSTRN card), HIERARCH for names
// that do not fit a standard card, text reduced to printable ASCII, reserved keywords left out.
std::string fitsCards(const std::vector<FitsKeyword>& keywords);

// Formats a value as a FITS string literal ('...' with doubled quotes).
std::string fitsString(const std::string& s);
// Formats a floating point value for a FITS card (always contains '.' or 'E').
std::string fitsReal(double v);

// Maps text to printable ASCII, as FITS headers require. Each UTF-8 multi-byte sequence
// becomes a single '?', control characters become spaces.
std::string fitsSanitize(const std::string& s);
// True for names that fit a standard card: 1-8 characters from A-Z, 0-9, '_' and '-'.
// Other names are written with the HIERARCH convention.
bool isStandardFitsName(const std::string& name);

// True for keywords the writer generates itself (SIMPLE, BITPIX, NAXISn, BZERO, ...).
bool isReservedFitsKeyword(const std::string& name);

}  // namespace xisfconv
