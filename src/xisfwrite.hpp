// Writer for monolithic XISF 1.0 files.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace xisfconv {

// An XISF image property. Scalars carry `value`; String properties carry it as element text;
// F64Vector / F64Matrix properties carry `data` (row-major), written as an inline base64 block.
struct XisfOutProperty {
    std::string id;
    std::string type;   // String, TimePoint, Boolean, Int32, Float32, Float64, F64Vector, F64Matrix
    std::string value;
    std::vector<double> data;
    size_t rows = 0, columns = 0;  // F64Matrix only
};

struct XisfOutImage {
    const PixelBuffer* pixels = nullptr;  // host byte order, planar, rows top-down
    std::string id;                       // image identifier (made a valid, unique identifier on write)
    bool rgb = false;                     // colorSpace RGB (else Gray)
    double lowerBound = 0, upperBound = 1;  // representable range of floating point samples
    std::vector<FitsKeyword> keywords;    // written as FITSKeyword elements, in order
    std::string cfaPattern;               // e.g. "RGGB" (empty = none)
    int cfaWidth = 0, cfaHeight = 0;
    std::vector<XisfOutProperty> properties;
    std::vector<uint8_t> iccProfile;      // written as an ICCProfile element with an inline block (empty = none)
};

struct XisfWriteOptions {
    std::string codec;              // "" (uncompressed), "zlib" or "zstd"
    bool shuffle = true;            // byte shuffling before compression
    std::string checksum;           // "", "sha1", "sha256", "sha512" (PixInsight's spelling), "sha3-256" or "sha3-512"
    uint64_t subblockSize = 1u << 30;  // blocks larger than this are compressed in subblocks
};

void writeXisf(const std::string& path, const std::vector<XisfOutImage>& images, const XisfWriteOptions& options);

}  // namespace xisfconv
