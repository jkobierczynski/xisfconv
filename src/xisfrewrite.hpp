// Rewrites an XISF file with another block storage (compression, checksums), and verifies files.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <optional>
#include <string>

#include "common.hpp"

namespace xisfconv {

struct XisfRewriteOptions {
    std::string codec;      // "" = keep every block as it is stored; "none", "zlib", "lz4", "lz4hc" or "zstd"
    std::string checksum;   // "" = keep (recomputed where the stored bytes change); "none" = remove;
                            // "sha1", "sha256", "sha512", "sha3-256" or "sha3-512"
    std::optional<size_t> imageIndex;  // keep only this image
    bool verifyInput = true;   // check the input's checksums and that every compressed block decodes
    bool readBack = true;      // read the written file back and compare every block with the input
    uint64_t subblockSize = 1u << 30;  // blocks larger than this are compressed in subblocks
};

struct XisfRewriteResult {
    uint64_t inputSize = 0, outputSize = 0;
    size_t blocks = 0;            // attached data blocks written
    size_t compressed = 0;        // blocks compressed with the requested codec
    size_t decompressed = 0;      // blocks now stored uncompressed
    size_t kept = 0;              // blocks copied as they were stored
    size_t checksums = 0;         // checksums computed for the output
    size_t checksumsRemoved = 0;
    bool readBack = false;        // the output was read back and matched
    bool changed = false;         // false if the output stores everything the way the input did
};

// Copies an XISF file, storing its attached data blocks as the options say. The XML header is
// carried over as it is: only the location, compression, subblocks and checksum attributes of the
// attached blocks change (and the XISF:Compression* metadata that describes them). Pixels,
// keywords, properties, ICC profiles, thumbnails and unknown elements are not touched.
// With imageIndex, the other Image elements and their blocks are left out.
XisfRewriteResult rewriteXisf(const std::string& input, const std::string& output, const XisfRewriteOptions& options);

// True if every attached block of the file is already stored the way the options ask, judged by
// the header alone (uncompressed blocks count as not compressed yet when a codec is requested).
bool xisfStoredAsRequested(const std::string& path, const XisfRewriteOptions& options);

// Reads every data block of an XISF file: verifies its checksum, decompresses it and, for
// images, compares its size with the geometry.
VerifyReport verifyXisf(const std::string& path);

}  // namespace xisfconv
