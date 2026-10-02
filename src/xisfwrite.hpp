// Writer for monolithic XISF 1.0 files.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace xisfconv {

struct XisfOutImage {
    const PixelBuffer* pixels = nullptr;  // host byte order, planar, rows top-down
    std::string id;                       // image identifier (made a valid, unique identifier on write)
    bool rgb = false;                     // colorSpace RGB (else Gray)
    double lowerBound = 0, upperBound = 1;  // representable range of floating point samples
    std::vector<FitsKeyword> keywords;    // written as FITSKeyword elements, in order
    std::string cfaPattern;               // e.g. "RGGB" (empty = none)
    int cfaWidth = 0, cfaHeight = 0;
};

struct XisfWriteOptions {
    std::string codec;              // "" (uncompressed), "zlib" or "zstd"
    bool shuffle = true;            // byte shuffling before compression
    std::string checksum;           // "", "sha1", "sha256" or "sha512" (PixInsight's spelling)
    uint64_t subblockSize = 1u << 30;  // blocks larger than this are compressed in subblocks
};

void writeXisf(const std::string& path, const std::vector<XisfOutImage>& images, const XisfWriteOptions& options);

}  // namespace xisfconv
