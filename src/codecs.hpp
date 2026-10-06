// Decoders for XISF data block encodings and compression codecs.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xisfconv {

std::vector<uint8_t> base64Decode(const std::string& text);  // ignores whitespace
std::string base64Encode(const uint8_t* data, size_t size);
std::vector<uint8_t> hexDecode(const std::string& text);     // ignores whitespace

// Each function decompresses exactly `expectedSize` bytes or throws.
std::vector<uint8_t> zlibDecompress(const uint8_t* src, size_t srcSize, size_t expectedSize);
std::vector<uint8_t> lz4BlockDecompress(const uint8_t* src, size_t srcSize, size_t expectedSize);
std::vector<uint8_t> zstdDecompress(const uint8_t* src, size_t srcSize, size_t expectedSize);
bool zstdAvailable();
// Size of the data a Zstandard frame holds, if the frame states it.
bool zstdFrameContentSize(const uint8_t* src, size_t srcSize, uint64_t& size);

// Compressors used when writing XISF. zstdCompress throws if the build has no Zstandard support.
std::vector<uint8_t> zlibCompress(const uint8_t* src, size_t srcSize, int level = 6);
std::vector<uint8_t> zstdCompress(const uint8_t* src, size_t srcSize, int level = 3);
// The LZ4 block format. effort 0: one look at one earlier place per position (the codec "lz4");
// 1 to 12: chains of earlier places are searched for the longest match, the longer the higher
// (the codec "lz4hc"). Both make blocks that one decoder reads. More than kLz4MaxInput bytes
// are not one block: the caller splits them.
constexpr uint64_t kLz4MaxInput = 0x7E000000;
std::vector<uint8_t> lz4BlockCompress(const uint8_t* src, size_t srcSize, int effort = 0);

// The codecs an XISF data block is written with, by their names in the compression attribute:
// zlib, lz4, lz4hc and zstd.
bool isXisfWriteCodec(const std::string& codec);
// The compression levels of a codec, lowest and highest; false if it has none (lz4).
bool xisfCodecLevels(const std::string& codec, int& lowest, int& highest);
// level 0: the usual one of the codec (zlib 6, lz4hc 9, zstd 3). Throws for a level the codec
// does not have, and for zstd in a build without it.
std::vector<uint8_t> xisfCompress(const std::string& codec, const uint8_t* src, size_t srcSize, int level = 0);
// The most bytes one compressed (sub)block of the codec may hold: `wanted`, or less.
uint64_t xisfSubblockSize(const std::string& codec, uint64_t wanted);

// XISF byte shuffling: all first bytes of each item, then all second bytes, ...
// Trailing bytes that do not form a full item are left in place.
std::vector<uint8_t> shuffled(const uint8_t* data, size_t size, size_t itemSize);

// Reverses XISF byte shuffling. Trailing bytes that do not form a full item are left in place.
void unshuffle(std::vector<uint8_t>& data, size_t itemSize);

// Message digests used by XISF checksums. Returns lowercase hex.
std::string sha1Hex(const uint8_t* data, size_t size);
std::string sha256Hex(const uint8_t* data, size_t size);
std::string sha512Hex(const uint8_t* data, size_t size);
std::string sha3Hex(const uint8_t* data, size_t size, int bits);  // SHA3-256 or SHA3-512

// MD5 digest (16 raw bytes), used for ASDF block checksums.
void md5(const uint8_t* data, size_t size, uint8_t digest[16]);

}  // namespace xisfconv
