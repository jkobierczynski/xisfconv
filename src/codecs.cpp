// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "codecs.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <climits>

#include "common.hpp"

#ifdef XISFCONV_HAVE_ZSTD
#include <zstd.h>
#endif

namespace xisfconv {

// ---------------------------------------------------------------- text encodings

std::vector<uint8_t> base64Decode(const std::string& text) {
    // filled once, also when several threads arrive here together
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> t;
        t.fill(-1);
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
        return t;
    }();
    std::vector<uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    bool padding = false;
    for (char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') { padding = true; continue; }
        if (padding || table[c] < 0) throw Error("invalid base64 data");
        acc = (acc << 6) | static_cast<uint32_t>(table[c]);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::string base64Encode(const uint8_t* data, size_t size) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = data[i], b1 = i + 1 < size ? data[i + 1] : 0, b2 = i + 2 < size ? data[i + 2] : 0;
        const uint32_t v = b0 << 16 | b1 << 8 | b2;
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        out += i + 1 < size ? alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < size ? alphabet[v & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> hexDecode(const std::string& text) {
    std::vector<uint8_t> out;
    out.reserve(text.size() / 2);
    int hi = -1;
    for (char c : text) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else throw Error("invalid hex data");
        if (hi < 0) hi = v;
        else { out.push_back(static_cast<uint8_t>(hi << 4 | v)); hi = -1; }
    }
    if (hi >= 0) throw Error("odd number of hex digits");
    return out;
}

// ---------------------------------------------------------------- zlib

std::vector<uint8_t> zlibDecompress(const uint8_t* src, size_t srcSize, size_t expectedSize) {
    std::vector<uint8_t> out(expectedSize);
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) throw Error("zlib: inflateInit failed");
    size_t inPos = 0, outPos = 0;
    uint8_t scratch[1];
    bool inScratch = false;  // output buffer full; probing for unexpected extra output
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        if (zs.avail_in == 0 && inPos < srcSize) {
            const size_t n = std::min<size_t>(srcSize - inPos, UINT_MAX);
            zs.next_in = const_cast<Bytef*>(src + inPos);
            zs.avail_in = static_cast<uInt>(n);
            inPos += n;
        }
        if (zs.avail_out == 0) {
            if (inScratch) {
                inflateEnd(&zs);
                throw Error("zlib: decompressed data larger than declared size");
            }
            if (outPos >= expectedSize) {
                zs.next_out = scratch;
                zs.avail_out = 1;
                inScratch = true;
            } else {
                const size_t n = std::min<size_t>(expectedSize - outPos, UINT_MAX);
                zs.next_out = out.data() + outPos;
                zs.avail_out = static_cast<uInt>(n);
                outPos += n;
            }
        }
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_BUF_ERROR && zs.avail_in == 0 && inPos >= srcSize) {
            inflateEnd(&zs);
            throw Error("zlib: truncated compressed data");
        }
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            const std::string msg = zs.msg ? zs.msg : "error " + std::to_string(ret);
            inflateEnd(&zs);
            throw Error("zlib: " + msg);
        }
    }
    if (inScratch && zs.avail_out == 0) {
        inflateEnd(&zs);
        throw Error("zlib: decompressed data larger than declared size");
    }
    const size_t produced = inScratch ? outPos : outPos - zs.avail_out;
    inflateEnd(&zs);
    if (produced != expectedSize) {
        throw Error("zlib: decompressed " + std::to_string(produced) + " bytes, expected " +
                    std::to_string(expectedSize));
    }
    return out;
}

// ---------------------------------------------------------------- LZ4 (block format)
// LZ4 and LZ4HC produce the same block format, so one decoder serves both codecs.

std::vector<uint8_t> lz4BlockDecompress(const uint8_t* src, size_t srcSize, size_t expectedSize) {
    std::vector<uint8_t> out(expectedSize);
    uint8_t* const ostart = out.data();
    uint8_t* op = ostart;
    uint8_t* const oend = ostart + expectedSize;
    const uint8_t* ip = src;
    const uint8_t* const iend = src + srcSize;
    auto corrupt = [] { throw Error("lz4: corrupt compressed data"); };

    while (ip < iend) {
        const unsigned token = *ip++;
        size_t litLen = token >> 4;
        if (litLen == 15) {
            unsigned b;
            do {
                if (ip >= iend) corrupt();
                b = *ip++;
                litLen += b;
            } while (b == 255);
        }
        if (litLen > static_cast<size_t>(iend - ip) || litLen > static_cast<size_t>(oend - op)) corrupt();
        std::memcpy(op, ip, litLen);
        op += litLen;
        ip += litLen;
        if (ip >= iend) break;  // last sequence carries literals only

        if (iend - ip < 2) corrupt();
        const size_t offset = static_cast<size_t>(ip[0]) | (static_cast<size_t>(ip[1]) << 8);
        ip += 2;
        if (offset == 0 || offset > static_cast<size_t>(op - ostart)) corrupt();
        size_t matchLen = token & 15;
        if (matchLen == 15) {
            unsigned b;
            do {
                if (ip >= iend) corrupt();
                b = *ip++;
                matchLen += b;
            } while (b == 255);
        }
        matchLen += 4;
        if (matchLen > static_cast<size_t>(oend - op)) corrupt();
        const uint8_t* match = op - offset;
        if (offset >= matchLen) {
            std::memcpy(op, match, matchLen);
            op += matchLen;
        } else {
            for (size_t i = 0; i < matchLen; ++i) *op++ = *match++;  // overlapping copy
        }
    }
    if (op != oend) {
        throw Error("lz4: decompressed " + std::to_string(op - ostart) + " bytes, expected " +
                    std::to_string(expectedSize));
    }
    return out;
}

// ---------------------------------------------------------------- Zstandard

bool zstdAvailable() {
#ifdef XISFCONV_HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

std::vector<uint8_t> zstdDecompress(const uint8_t* src, size_t srcSize, size_t expectedSize) {
#ifdef XISFCONV_HAVE_ZSTD
    std::vector<uint8_t> out(expectedSize);
    const size_t r = ZSTD_decompress(out.data(), expectedSize, src, srcSize);
    if (ZSTD_isError(r)) throw Error(std::string("zstd: ") + ZSTD_getErrorName(r));
    if (r != expectedSize) {
        throw Error("zstd: decompressed " + std::to_string(r) + " bytes, expected " + std::to_string(expectedSize));
    }
    return out;
#else
    (void)src;
    (void)srcSize;
    (void)expectedSize;
    throw Unsupported("this build has no Zstandard support (rebuild with libzstd)");
#endif
}

bool zstdFrameContentSize(const uint8_t* src, size_t srcSize, uint64_t& size) {
#ifdef XISFCONV_HAVE_ZSTD
    const unsigned long long r = ZSTD_getFrameContentSize(src, srcSize);
    if (r == ZSTD_CONTENTSIZE_UNKNOWN || r == ZSTD_CONTENTSIZE_ERROR) return false;
    size = r;
    return true;
#else
    (void)src;
    (void)srcSize;
    (void)size;
    return false;
#endif
}

// ---------------------------------------------------------------- compression

std::vector<uint8_t> zlibCompress(const uint8_t* src, size_t srcSize, int level) {
    // Streaming deflate, so blocks larger than 4 GiB work on every platform.
    z_stream zs{};
    if (deflateInit(&zs, level) != Z_OK) throw Error("zlib: deflateInit failed");
    std::vector<uint8_t> out;
    std::vector<uint8_t> buf(1 << 20);
    size_t inPos = 0;
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        if (zs.avail_in == 0 && inPos < srcSize) {
            const size_t n = std::min<size_t>(srcSize - inPos, 1u << 30);
            zs.next_in = const_cast<Bytef*>(src + inPos);
            zs.avail_in = static_cast<uInt>(n);
            inPos += n;
        }
        zs.next_out = buf.data();
        zs.avail_out = static_cast<uInt>(buf.size());
        ret = deflate(&zs, inPos >= srcSize ? Z_FINISH : Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR) {
            deflateEnd(&zs);
            throw Error("zlib: compression failed");
        }
        out.insert(out.end(), buf.data(), buf.data() + (buf.size() - zs.avail_out));
    }
    deflateEnd(&zs);
    return out;
}

std::vector<uint8_t> zstdCompress(const uint8_t* src, size_t srcSize, int level) {
#ifdef XISFCONV_HAVE_ZSTD
    std::vector<uint8_t> out(ZSTD_compressBound(srcSize));
    const size_t r = ZSTD_compress(out.data(), out.size(), src, srcSize, level);
    if (ZSTD_isError(r)) throw Error(std::string("zstd: ") + ZSTD_getErrorName(r));
    out.resize(r);
    return out;
#else
    (void)src;
    (void)srcSize;
    (void)level;
    throw Unsupported("this build has no Zstandard support (use --codec zlib, or rebuild with libzstd)");
#endif
}

// ---------------------------------------------------------------- byte shuffling

std::vector<uint8_t> shuffled(const uint8_t* data, size_t size, size_t itemSize) {
    std::vector<uint8_t> out(size);
    if (itemSize <= 1 || size < itemSize) {
        std::copy(data, data + size, out.begin());
        return out;
    }
    const size_t n = size / itemSize;
    for (size_t b = 0; b < itemSize; ++b) {
        uint8_t* d = out.data() + b * n;
        const uint8_t* s = data + b;
        for (size_t i = 0; i < n; ++i, s += itemSize) d[i] = *s;
    }
    const size_t tail = n * itemSize;
    std::copy(data + tail, data + size, out.begin() + static_cast<std::ptrdiff_t>(tail));
    return out;
}

void unshuffle(std::vector<uint8_t>& data, size_t itemSize) {
    if (itemSize <= 1 || data.size() < itemSize) return;
    const size_t n = data.size() / itemSize;
    std::vector<uint8_t> out(data.size());
    for (size_t b = 0; b < itemSize; ++b) {
        const uint8_t* s = data.data() + b * n;
        uint8_t* d = out.data() + b;
        for (size_t i = 0; i < n; ++i, d += itemSize) *d = s[i];
    }
    const size_t tail = n * itemSize;
    std::copy(data.begin() + static_cast<std::ptrdiff_t>(tail), data.end(),
              out.begin() + static_cast<std::ptrdiff_t>(tail));
    data.swap(out);
}

// ---------------------------------------------------------------- SHA family

namespace {

std::string toHex(const uint8_t* d, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += digits[d[i] >> 4];
        s += digits[d[i] & 15];
    }
    return s;
}

inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

// Generic Merkle-Damgard padding driver. BlockSize is 64 or 128; LenBytes is 8 or 16.
template <size_t BlockSize, size_t LenBytes, class Compress>
void mdHash(const uint8_t* data, size_t size, Compress compress) {
    size_t full = size / BlockSize;
    for (size_t i = 0; i < full; ++i) compress(data + i * BlockSize);
    uint8_t buf[BlockSize * 2] = {};
    const size_t rem = size - full * BlockSize;
    if (rem) std::memcpy(buf, data + full * BlockSize, rem);
    buf[rem] = 0x80;
    const size_t total = (rem + 1 + LenBytes <= BlockSize) ? BlockSize : 2 * BlockSize;
    const uint64_t bits = static_cast<uint64_t>(size) * 8;  // < 2^64 bits always for in-memory data
    for (int i = 0; i < 8; ++i) buf[total - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    compress(buf);
    if (total == 2 * BlockSize) compress(buf + BlockSize);
}

}  // namespace

std::string sha1Hex(const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    mdHash<64, 8>(data, size, [&h](const uint8_t* blk) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = static_cast<uint32_t>(blk[4 * i]) << 24 | static_cast<uint32_t>(blk[4 * i + 1]) << 16 |
                   static_cast<uint32_t>(blk[4 * i + 2]) << 8 | blk[4 * i + 3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rotl32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl32(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    });
    uint8_t out[20];
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
    return toHex(out, 20);
}

std::string sha256Hex(const uint8_t* data, size_t size) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    mdHash<64, 8>(data, size, [&h](const uint8_t* blk) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = static_cast<uint32_t>(blk[4 * i]) << 24 | static_cast<uint32_t>(blk[4 * i + 1]) << 16 |
                   static_cast<uint32_t>(blk[4 * i + 2]) << 8 | blk[4 * i + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    });
    uint8_t out[32];
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
    return toHex(out, 32);
}

std::string sha512Hex(const uint8_t* data, size_t size) {
    static const uint64_t K[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
        0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
        0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
        0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};
    uint64_t h[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                     0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                     0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    mdHash<128, 16>(data, size, [&h](const uint8_t* blk) {
        uint64_t w[80];
        for (int i = 0; i < 16; ++i) {
            uint64_t v = 0;
            for (int j = 0; j < 8; ++j) v = (v << 8) | blk[8 * i + j];
            w[i] = v;
        }
        for (int i = 16; i < 80; ++i) {
            const uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; ++i) {
            const uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
            const uint64_t ch = (e & f) ^ (~e & g);
            const uint64_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
            const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint64_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    });
    uint8_t out[64];
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) out[8 * i + j] = static_cast<uint8_t>(h[i] >> (56 - 8 * j));
    return toHex(out, 64);
}

// ---------------------------------------------------------------- SHA-3 (FIPS 202)

namespace {

void keccakF1600(uint64_t s[25]) {
    static const uint64_t roundConstants[24] = {
        0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808aull, 0x8000000080008000ull,
        0x000000000000808bull, 0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull,
        0x000000000000008aull, 0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000aull,
        0x000000008000808bull, 0x800000000000008bull, 0x8000000000008089ull, 0x8000000000008003ull,
        0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800aull, 0x800000008000000aull,
        0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull};
    static const int rotation[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
    static const int lane[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};
    auto rotl = [](uint64_t x, int n) { return (x << n) | (x >> (64 - n)); };
    for (int round = 0; round < 24; ++round) {
        uint64_t c[5];
        for (int i = 0; i < 5; ++i) c[i] = s[i] ^ s[i + 5] ^ s[i + 10] ^ s[i + 15] ^ s[i + 20];
        for (int i = 0; i < 5; ++i) {
            const uint64_t t = c[(i + 4) % 5] ^ rotl(c[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5) s[j + i] ^= t;
        }
        uint64_t t = s[1];
        for (int i = 0; i < 24; ++i) {
            const uint64_t next = s[lane[i]];
            s[lane[i]] = rotl(t, rotation[i]);
            t = next;
        }
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; ++i) c[i] = s[j + i];
            for (int i = 0; i < 5; ++i) s[j + i] ^= ~c[(i + 1) % 5] & c[(i + 2) % 5];
        }
        s[0] ^= roundConstants[round];
    }
}

}  // namespace

std::string sha3Hex(const uint8_t* data, size_t size, int bits) {
    if (bits != 256 && bits != 512) throw Error("unsupported SHA-3 digest size");
    const size_t digestBytes = static_cast<size_t>(bits) / 8;
    const size_t rate = 200 - 2 * digestBytes;  // bytes absorbed per permutation
    uint64_t s[25] = {};
    auto absorb = [&](const uint8_t* block) {
        for (size_t i = 0; i < rate / 8; ++i) {
            uint64_t w = 0;
            for (int k = 7; k >= 0; --k) w = (w << 8) | block[i * 8 + static_cast<size_t>(k)];  // little-endian lanes
            s[i] ^= w;
        }
        keccakF1600(s);
    };
    size_t offset = 0;
    for (; size - offset >= rate; offset += rate) absorb(data + offset);
    uint8_t last[144] = {};  // the largest rate (SHA3-256: 136)
    if (size > offset) std::memcpy(last, data + offset, size - offset);
    last[size - offset] ^= 0x06;
    last[rate - 1] ^= 0x80;
    absorb(last);
    uint8_t digest[64];
    for (size_t i = 0; i < digestBytes; ++i) digest[i] = static_cast<uint8_t>(s[i / 8] >> (8 * (i % 8)));
    return toHex(digest, digestBytes);
}

// ---------------------------------------------------------------- MD5

void md5(const uint8_t* data, size_t size, uint8_t digest[16]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    auto compress = [&h](const uint8_t* blk) {
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = static_cast<uint32_t>(blk[4 * i]) | static_cast<uint32_t>(blk[4 * i + 1]) << 8 |
                   static_cast<uint32_t>(blk[4 * i + 2]) << 16 | static_cast<uint32_t>(blk[4 * i + 3]) << 24;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t t = a + f + K[i] + m[g];
            a = d;
            d = c;
            c = b;
            b = b + rotl32(t, S[i]);
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    };
    const size_t full = size / 64;
    for (size_t i = 0; i < full; ++i) compress(data + i * 64);
    uint8_t buf[128] = {};
    const size_t rem = size - full * 64;
    if (rem) std::memcpy(buf, data + full * 64, rem);
    buf[rem] = 0x80;
    const size_t total = rem + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(size) * 8;
    for (int i = 0; i < 8; ++i) buf[total - 8 + i] = static_cast<uint8_t>(bits >> (8 * i));  // little-endian length
    compress(buf);
    if (total == 128) compress(buf + 64);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) digest[4 * i + j] = static_cast<uint8_t>(h[i] >> (8 * j));
}

}  // namespace xisfconv
