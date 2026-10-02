// Decoders for XISF data block encodings and compression codecs.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xisfconv {

std::vector<uint8_t> base64Decode(const std::string& text);  // ignores whitespace
std::vector<uint8_t> hexDecode(const std::string& text);     // ignores whitespace

// Each function decompresses exactly `expectedSize` bytes or throws.
std::vector<uint8_t> zlibDecompress(const uint8_t* src, size_t srcSize, size_t expectedSize);
std::vector<uint8_t> lz4BlockDecompress(const uint8_t* src, size_t srcSize, size_t expectedSize);
std::vector<uint8_t> zstdDecompress(const uint8_t* src, size_t srcSize, size_t expectedSize);
bool zstdAvailable();

// Compressors used when writing XISF. zstdCompress throws if the build has no Zstandard support.
std::vector<uint8_t> zlibCompress(const uint8_t* src, size_t srcSize, int level = 6);
std::vector<uint8_t> zstdCompress(const uint8_t* src, size_t srcSize, int level = 3);

// XISF byte shuffling: all first bytes of each item, then all second bytes, ...
// Trailing bytes that do not form a full item are left in place.
std::vector<uint8_t> shuffled(const uint8_t* data, size_t size, size_t itemSize);

// Reverses XISF byte shuffling. Trailing bytes that do not form a full item are left in place.
void unshuffle(std::vector<uint8_t>& data, size_t itemSize);

// Message digests used by XISF checksums. Returns lowercase hex.
std::string sha1Hex(const uint8_t* data, size_t size);
std::string sha256Hex(const uint8_t* data, size_t size);
std::string sha512Hex(const uint8_t* data, size_t size);

}  // namespace xisfconv
