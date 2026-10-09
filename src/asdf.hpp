// ASDF (Advanced Scientific Data Format) reader and writer.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "fits.hpp"
#include "fitsread.hpp"

namespace xisfconv {

struct AsdfWriteOptions {
    std::string codec;  // block compression: empty (none), "zlib" or "zstd"
    std::vector<Property> metadata;   // the XISF properties of the file the images come from. Empty: none.
};

// XISF properties in an ASDF file: the key "xisf" of the tree, with
//   images:    a list with an entry per HDU of "fits", in their order: a mapping that is empty,
//              or has "properties" and "wcs_digest" (what FitsHdu::wcsDigest holds);
//   metadata:  the properties of the file.
// Properties are a mapping from id to {type, value, comment, format}: type is the XISF type
// name; value is a YAML scalar of that kind (true, 42, 1.5, "text"; a complex number as
// core/complex) and for vectors and matrices an ndarray of their shape in a binary block.
// A String that is not UTF-8 is an array of its bytes; one that XISF keeps in a data block has
// "block: true". A data block of a type this library does not know is an array of its bytes
// too, with the "length", or the "rows" and "columns", of its element.

// Writes the images as a FITS HDU list (tag:astropy.org:astropy/fits/fits-1.0.0) under the
// tree's "fits" key: each HDU has the header as [keyword, value, comment] entries and the
// pixels as an ndarray of shape [height, width] or [channels, height, width] in a binary block.
// Python's asdf library with asdf-astropy returns it as an astropy.io.fits.HDUList.
void writeAsdf(const std::string& path, const std::vector<FitsHdu>& hdus, const AsdfWriteOptions& options);

// Reads the images of an ASDF file: the HDUs of FITS-tagged nodes, and any other numeric array
// with 2 or 3 dimensions that is stored in a binary block. The result has the same form as a
// FITS file. With headersOnly the pixel data is not read (for --info).
// Throws xisfconv::Error on malformed files.
// With onlyImage the pixels of that one image are read and the others are left as headers.
// With inPieces they are not read into memory: each image gets a source of them (FitsImage::pieces).
FitsFile readAsdf(const std::string& path, bool headersOnly = false, bool verifyChecksums = true,
                  std::optional<size_t> onlyImage = std::nullopt, bool inPieces = false);

// Parses the tree and reads every binary block: MD5 checksums are verified, compressed blocks
// are decompressed.
VerifyReport verifyAsdf(const std::string& path);

// Returns the YAML tree of an ASDF file as text (from the %YAML directive to the closing "...").
std::string readAsdfTree(const std::string& path);

// True if the file starts with the ASDF signature.
bool looksLikeAsdf(const std::string& path);

}  // namespace xisfconv
