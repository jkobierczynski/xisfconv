// ASDF (Advanced Scientific Data Format) reader and writer.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "fits.hpp"
#include "fitsread.hpp"

namespace xisfconv {

struct AsdfWriteOptions {
    std::string codec;  // block compression: empty (none), "zlib" or "zstd"
};

// Writes the images as a FITS HDU list (tag:astropy.org:astropy/fits/fits-1.0.0) under the
// tree's "fits" key: each HDU has the header as [keyword, value, comment] entries and the
// pixels as an ndarray of shape [height, width] or [channels, height, width] in a binary block.
// Python's asdf library with asdf-astropy returns it as an astropy.io.fits.HDUList.
void writeAsdf(const std::string& path, const std::vector<FitsHdu>& hdus, const AsdfWriteOptions& options);

// Reads the images of an ASDF file: the HDUs of FITS-tagged nodes, and any other numeric array
// with 2 or 3 dimensions that is stored in a binary block. The result has the same form as a
// FITS file. With headersOnly the pixel data is not read (for --info).
// Throws xisfconv::Error on malformed files.
FitsFile readAsdf(const std::string& path, bool headersOnly = false, bool verifyChecksums = true);

// Parses the tree and reads every binary block: MD5 checksums are verified, compressed blocks
// are decompressed.
VerifyReport verifyAsdf(const std::string& path);

// Returns the YAML tree of an ASDF file as text (from the %YAML directive to the closing "...").
std::string readAsdfTree(const std::string& path);

// True if the file starts with the ASDF signature.
bool looksLikeAsdf(const std::string& path);

}  // namespace xisfconv
