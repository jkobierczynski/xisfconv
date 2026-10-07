// Writer for XISF 1.0 units: monolithic files, and distributed units of a header file and one
// data blocks file.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"
#include "property.hpp"

namespace xisfconv {

struct XisfOutImage {
    const PixelBuffer* pixels = nullptr;  // host byte order, planar, rows top-down
    std::string id;                       // image identifier (made a valid, unique identifier on write)
    bool rgb = false;                     // colorSpace RGB (else Gray)
    double lowerBound = 0, upperBound = 1;  // representable range of floating point samples
    std::vector<FitsKeyword> keywords;    // written as FITSKeyword elements, in order
    std::string cfaPattern;               // e.g. "RGGB" (empty = none)
    int cfaWidth = 0, cfaHeight = 0;
    // Scalars are written as a value attribute, a String as the text of its element, vectors and
    // matrices as data blocks: in the header (base64) when small, attached to the file otherwise.
    std::vector<Property> properties;
    std::vector<uint8_t> iccProfile;      // written as an ICCProfile element with an inline block (empty = none)
};

struct XisfWriteOptions {
    std::string codec;              // "" (uncompressed), "zlib", "lz4", "lz4hc" or "zstd"
    int level = 0;                  // compression level; 0: the usual one of the codec (see xisfCompress)
    bool shuffle = true;            // byte shuffling before compression
    std::string checksum;           // "", "sha1", "sha256", "sha512" (PixInsight's spelling), "sha3-256" or "sha3-512"
    uint64_t subblockSize = 1u << 30;  // blocks larger than this are compressed in subblocks
    // File-level properties (the Metadata element), written after those the writer sets itself
    // (isFileStorageProperty), which are left out of this list.
    std::vector<Property> metadata;
    // XISF:CreatorApplication. Empty: this library names itself. With another name the library
    // is named in XISF:CreatorModule, as the specification has it for the code that did the writing.
    std::string creatorApplication;
    // A distributed unit: `path` gets the header alone (an XISF header file, .xish) and this file
    // the data blocks (an XISF data blocks file, .xisb), which the header names as blocksName in
    // its own directory. Empty: a monolithic file, with the blocks attached.
    std::string blocksPath;
    std::string blocksName;
};

// Data blocks up to this size are written into the header, larger ones are attached to the
// file: PixInsight's own limit (its XISF:MaxInlineBlockSize).
constexpr size_t kXisfMaxInlineBlock = 3072;

void writeXisf(const std::string& path, const std::vector<XisfOutImage>& images, const XisfWriteOptions& options);

}  // namespace xisfconv
