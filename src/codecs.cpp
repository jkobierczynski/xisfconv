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
        if (litLen) std::memcpy(op, ip, litLen);   // (a block of nothing has no memory to copy to)
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

// ---------------------------------------------------------------- LZ4 compression
// A block is a row of sequences: a token (the length of the literals, the length of the match
// less 4), the literals, the distance back to the match (1 to 65535, two bytes) and what of the
// lengths did not fit the token. The last sequence is literals only. Three rules of the format
// keep a decoder from reading past the end: the last 5 bytes are literals, the last match
// starts at least 12 bytes before the end, and so a block of less than 13 bytes has no match.
//
// A block is compressed a piece at a time: what is looked at is a window of it that moves along
// (the block itself when it is in memory), and what is made goes to a sink. The bytes are the
// same however the block is held: the window only says where they are read from.

namespace {

constexpr size_t kLz4MinMatch = 4, kLz4LastLiterals = 5, kLz4MatchStartLimit = 12, kLz4Window = 65535;

// How many bytes are the same at `a` and at `b`, up to `limit`.
inline size_t lz4Same(const uint8_t* a, const uint8_t* b, size_t limit) {
    size_t n = 0;
    while (n + 8 <= limit) {
        uint64_t x, y;
        std::memcpy(&x, a + n, 8);
        std::memcpy(&y, b + n, 8);
        if (x != y) break;
        n += 8;
    }
    while (n < limit && a[n] == b[n]) ++n;
    return n;
}

// The block being compressed, by its positions: [position, position + size) of the input.
class Lz4Input {
public:
    Lz4Input(RandomBytes& in, uint64_t position, size_t size, size_t span) : in_(in), base_(position), n_(size), span_(span) {
        if (const uint8_t* all = in.contiguous()) {
            direct_ = all + position;
            hi_ = size;
        }
    }
    size_t size() const { return n_; }
    // The bytes [p, p + length), until the next call.
    const uint8_t* at(size_t p, size_t length) {
        if (direct_) return direct_ + p;
        if (p < lo_ || p + length > hi_) load(p, p + length);
        return window_.data() + (p - lo_);
    }
    uint8_t byte(size_t p) { return *at(p, 1); }
    uint32_t read32(size_t p) {
        uint32_t v;
        std::memcpy(&v, at(p, 4), 4);
        return v;   // (in the order of the host: it is compared and hashed, never written)
    }
    // How many bytes are the same from `a` and from `b` (a < b), up to `limit`.
    size_t same(size_t a, size_t b, size_t limit) {
        if (direct_) return lz4Same(direct_ + a, direct_ + b, limit);
        size_t n = 0;
        while (n < limit) {
            const size_t step = std::min<size_t>(limit - n, size_t(1) << 16);
            const uint8_t* pa = at(a + n, b - a + step);
            const size_t k = lz4Same(pa, pa + (b - a), step);
            n += k;
            if (k < step) break;
        }
        return n;
    }
    // Copies [p, p + length) to `out`.
    void copy(size_t p, size_t length, uint8_t* out) {
        if (direct_) std::memcpy(out, direct_ + p, length);
        else if (p >= lo_ && p + length <= hi_) std::memcpy(out, window_.data() + (p - lo_), length);
        else in_.read(base_ + p, length, out);
    }
    // A sign of life every MiB or so of the block.
    void reached(size_t p) {
        if (p - ticked_ >= (size_t(1) << 20)) {
            progressTick(p - ticked_);
            ticked_ = p;
        }
    }

private:
    static constexpr size_t kBack = kLz4Window + 1 + 64;   // what is kept before the first byte asked for
    RandomBytes& in_;
    uint64_t base_;
    size_t n_, span_;
    const uint8_t* direct_ = nullptr;
    std::vector<uint8_t> window_, spare_;
    size_t lo_ = 0, hi_ = 0;   // the window holds [lo_, hi_)
    size_t ticked_ = 0;

    void load(size_t from, size_t to) {
        const size_t lo = from > kBack ? from - kBack : 0;
        const size_t hi = std::min(n_, std::max(to, from + span_));
        spare_.resize(hi - lo);
        const size_t a = std::max(lo, lo_), b = std::min(hi, hi_);
        if (a < b) {
            // what the old window has of the new one is copied, the rest is read
            std::memcpy(spare_.data() + (a - lo), window_.data() + (a - lo_), b - a);
            if (lo < a) in_.read(base_ + lo, a - lo, spare_.data());
            if (b < hi) in_.read(base_ + b, hi - b, spare_.data() + (b - lo));
        } else {
            in_.read(base_ + lo, hi - lo, spare_.data());
        }
        window_.swap(spare_);
        lo_ = lo;
        hi_ = hi;
    }
};

class Lz4Output {
public:
    Lz4Output(Lz4Input& input, ByteSink& sink) : input_(input), sink_(sink) { buffer_.reserve(kFlush + 64); }
    // matchLength 0: the last sequence, literals only.
    void sequence(size_t literals, size_t literalCount, size_t distance, size_t matchLength) {
        const size_t extra = matchLength ? matchLength - kLz4MinMatch : 0;
        put(static_cast<uint8_t>((std::min<size_t>(literalCount, 15) << 4) | std::min<size_t>(extra, 15)));
        if (literalCount >= 15) length(literalCount - 15);
        while (literalCount > 0) {
            const size_t n = std::min(literalCount, kFlush);
            const size_t at = buffer_.size();
            buffer_.resize(at + n);
            input_.copy(literals, n, buffer_.data() + at);
            literals += n;
            literalCount -= n;
            if (buffer_.size() >= kFlush) flush();
        }
        if (!matchLength) return;
        put(static_cast<uint8_t>(distance));
        put(static_cast<uint8_t>(distance >> 8));
        if (extra >= 15) length(extra - 15);
    }
    void finish() { flush(); }

private:
    static constexpr size_t kFlush = size_t(1) << 20;
    Lz4Input& input_;
    ByteSink& sink_;
    std::vector<uint8_t> buffer_;

    void put(uint8_t b) {
        buffer_.push_back(b);
        if (buffer_.size() >= kFlush) flush();
    }
    void length(size_t rest) {
        while (rest >= 255) {
            put(255);
            rest -= 255;
        }
        put(static_cast<uint8_t>(rest));
    }
    void flush() {
        if (!buffer_.empty()) sink_.write(buffer_.data(), buffer_.size());
        buffer_.clear();
    }
};

// One table of the last place each hash was seen at, one look per position.
void lz4Fast(Lz4Input& src, Lz4Output& out) {
    const size_t n = src.size();
    size_t anchor = 0;   // the first byte that is not written yet
    if (n > kLz4MatchStartLimit) {
        std::vector<uint32_t> table(size_t(1) << 16, 0);   // a position + 1; 0: none
        const size_t matchEnd = n - kLz4LastLiterals, lastStart = n - kLz4MatchStartLimit;
        size_t ip = 0;
        size_t misses = 0;   // looks since the last match
        while (ip <= lastStart) {
            src.reached(ip);
            const uint32_t word = src.read32(ip);
            const uint32_t hash = (word * 2654435761u) >> 16;
            const size_t seen = table[hash];
            table[hash] = static_cast<uint32_t>(ip + 1);
            if (seen && ip - (seen - 1) <= kLz4Window && src.read32(seen - 1) == word) {
                size_t match = seen - 1, start = ip;
                size_t length = kLz4MinMatch + src.same(match + kLz4MinMatch, ip + kLz4MinMatch, matchEnd - ip - kLz4MinMatch);
                while (start > anchor && match > 0 && src.byte(start - 1) == src.byte(match - 1)) {
                    --start;
                    --match;
                    ++length;
                }
                out.sequence(anchor, start - anchor, start - match, length);
                ip = anchor = start + length;
                misses = 0;
                continue;
            }
            // Longer steps through what does not compress: one byte more for every 64 looks
            // that found nothing. (The step must not grow with the distance: it would be so
            // long after some megabytes of noise that what compresses behind them is missed.)
            ip += 1 + (misses++ >> 6);
        }
    }
    out.sequence(anchor, n - anchor, 0, 0);
}

// Every position is kept in a chain of the positions with the same hash, and the chain is
// followed for the longest match; a match is put off by a byte if the next position has a
// longer one.
void lz4Chains(Lz4Input& src, int effort, Lz4Output& out) {
    const size_t n = src.size();
    size_t anchor = 0;
    if (n > kLz4MatchStartLimit) {
        const int tries = effort >= 12 ? 4096 : effort >= 3 ? 1 << (effort - 1) : effort + 1;
        std::vector<uint32_t> head(size_t(1) << 16, 0);    // the last position with a hash, + 1
        std::vector<uint16_t> back(size_t(1) << 16, 0);    // from a position to the one before it in its chain; 0: none
        const size_t matchEnd = n - kLz4LastLiterals, lastStart = n - kLz4MatchStartLimit;
        size_t next = 0;   // the first position that is not in the chains yet
        auto hashAt = [&](size_t p) { return (src.read32(p) * 2654435761u) >> 16; };
        auto insertUpTo = [&](size_t end) {
            for (; next < end; ++next) {
                const uint32_t hash = hashAt(next);
                const size_t distance = head[hash] ? next + 1 - head[hash] : 0;
                back[next & 0xFFFF] = static_cast<uint16_t>(distance <= kLz4Window ? distance : 0);
                head[hash] = static_cast<uint32_t>(next + 1);
            }
        };
        // The longest match for position p (which is at most lastStart): its length, 0 if none.
        auto longest = [&](size_t p, size_t& match) {
            insertUpTo(p);
            const size_t limit = matchEnd - p;
            const uint32_t word = src.read32(p);
            size_t best = 0;
            size_t candidate = head[hashAt(p)];
            for (int left = tries; candidate && left > 0; --left) {
                const size_t at = candidate - 1;
                if (p - at > kLz4Window) break;
                if (src.read32(at) == word && (best < kLz4MinMatch || src.byte(at + best) == src.byte(p + best))) {
                    const size_t length = kLz4MinMatch + src.same(at + kLz4MinMatch, p + kLz4MinMatch, limit - kLz4MinMatch);
                    if (length > best) {
                        best = length;
                        match = at;
                        if (best >= limit) break;   // there is no longer one
                    }
                }
                const size_t distance = back[at & 0xFFFF];
                if (!distance) break;
                candidate -= distance;
            }
            return best;
        };
        size_t ip = 0;
        while (ip <= lastStart) {
            src.reached(ip);
            size_t match = 0;
            size_t length = longest(ip, match);
            if (!length) {
                ++ip;
                continue;
            }
            while (ip + 1 <= lastStart) {
                size_t later = 0;
                const size_t longer = longest(ip + 1, later);
                if (longer <= length) break;
                ++ip;
                length = longer;
                match = later;
            }
            size_t start = ip;
            while (start > anchor && match > 0 && src.byte(start - 1) == src.byte(match - 1)) {
                --start;
                --match;
                ++length;
            }
            out.sequence(anchor, start - anchor, start - match, length);
            ip = anchor = start + length;
        }
    }
    out.sequence(anchor, n - anchor, 0, 0);
}

// How much of a block that is not in memory is held at once: a piece, within bounds.
size_t lz4Span() {
    // (at least what is kept behind it, which every load copies)
    return static_cast<size_t>(std::min<uint64_t>(std::max<uint64_t>(pieceSettings().pieceBytes, 128 << 10), uint64_t(4) << 20));
}

void lz4Compress(RandomBytes& in, uint64_t position, uint64_t size, int effort, ByteSink& sink) {
    if (size > kLz4MaxInput) throw Error("lz4: a block of more than " + std::to_string(kLz4MaxInput) + " bytes");
    Lz4Input src(in, position, static_cast<size_t>(size), lz4Span());
    Lz4Output out(src, sink);
    if (effort <= 0) lz4Fast(src, out);
    else lz4Chains(src, std::min(effort, 12), out);
    out.finish();
}

}  // namespace

std::vector<uint8_t> lz4BlockCompress(const uint8_t* src, size_t srcSize, int effort) {
    std::vector<uint8_t> out;
    out.reserve(srcSize / 2 + 64);
    MemoryBytes in(src, srcSize);
    VectorSink sink(out);
    lz4Compress(in, 0, srcSize, effort, sink);
    return out;
}

// ---------------------------------------------------------------- compression in pieces

namespace {

class ZlibCompressor : public StreamCompressor {
public:
    ZlibCompressor(int level, ByteSink& out) : out_(out), buffer_(size_t(1) << 20) {
        if (deflateInit(&zs_, level) != Z_OK) throw Error("zlib: deflateInit failed");
    }
    ~ZlibCompressor() override { deflateEnd(&zs_); }
    void write(const uint8_t* data, size_t n) override {
        while (n > 0) {
            const size_t piece = std::min<size_t>(n, size_t(1) << 30);
            zs_.next_in = const_cast<Bytef*>(data);
            zs_.avail_in = static_cast<uInt>(piece);
            pump(Z_NO_FLUSH);
            data += piece;
            n -= piece;
        }
    }
    void finish() override {
        zs_.next_in = nullptr;
        zs_.avail_in = 0;
        pump(Z_FINISH);
    }

private:
    z_stream zs_{};
    ByteSink& out_;
    std::vector<uint8_t> buffer_;
    void pump(int flush) {
        for (;;) {
            zs_.next_out = buffer_.data();
            zs_.avail_out = static_cast<uInt>(buffer_.size());
            const int ret = deflate(&zs_, flush);
            if (ret == Z_STREAM_ERROR) throw Error("zlib: compression failed");
            const size_t made = buffer_.size() - zs_.avail_out;
            if (made) out_.write(buffer_.data(), made);
            if (flush == Z_FINISH ? ret == Z_STREAM_END : (zs_.avail_in == 0 && zs_.avail_out != 0)) break;
        }
    }
};

#ifdef XISFCONV_HAVE_ZSTD
// Zstandard is given its input in pieces of one size, whatever the caller writes at once: so
// the bytes it makes do not depend on that.
class ZstdCompressor : public StreamCompressor {
public:
    ZstdCompressor(int level, uint64_t total, ByteSink& out) : out_(out), output_(ZSTD_CStreamOutSize()) {
        cctx_ = ZSTD_createCCtx();
        if (!cctx_) throw Error("zstd: out of memory");
        check(ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, level));
        check(ZSTD_CCtx_setPledgedSrcSize(cctx_, total));
        input_.reserve(kPiece);
    }
    ~ZstdCompressor() override { ZSTD_freeCCtx(cctx_); }
    void write(const uint8_t* data, size_t n) override {
        while (n > 0) {
            const size_t take = std::min(n, kPiece - input_.size());
            input_.insert(input_.end(), data, data + take);
            data += take;
            n -= take;
            if (input_.size() == kPiece) {
                run(ZSTD_e_continue);
                input_.clear();
            }
        }
    }
    void finish() override {
        run(ZSTD_e_end);
        input_.clear();
    }

private:
    static constexpr size_t kPiece = size_t(1) << 20;
    ZSTD_CCtx* cctx_ = nullptr;
    ByteSink& out_;
    std::vector<uint8_t> input_, output_;
    static void check(size_t r) {
        if (ZSTD_isError(r)) throw Error(std::string("zstd: ") + ZSTD_getErrorName(r));
    }
    void run(ZSTD_EndDirective how) {
        ZSTD_inBuffer in{input_.data(), input_.size(), 0};
        for (;;) {
            ZSTD_outBuffer out{output_.data(), output_.size(), 0};
            const size_t left = ZSTD_compressStream2(cctx_, &out, &in, how);
            check(left);
            if (out.pos) out_.write(output_.data(), out.pos);
            if (how == ZSTD_e_end ? left == 0 : in.pos == in.size) break;
        }
    }
};
#endif

}  // namespace

std::unique_ptr<StreamCompressor> StreamCompressor::create(const std::string& codec, int level, uint64_t totalSize, ByteSink& out) {
    if (codec == "zlib") return std::make_unique<ZlibCompressor>(level ? level : 6, out);
    if (codec == "zstd") {
#ifdef XISFCONV_HAVE_ZSTD
        return std::make_unique<ZstdCompressor>(level ? level : 3, totalSize, out);
#else
        (void)totalSize;
        throw Unsupported("this build has no Zstandard support (use --codec zlib, or rebuild with libzstd)");
#endif
    }
    throw Error("no stream compression with " + codec + " (internal error)");
}

namespace {

void checkLevel(const std::string& codec, int level) {
    if (level == 0) return;
    int lowest = 0, highest = 0;
    if (!xisfCodecLevels(codec, lowest, highest)) {
        throw Error("the codec " + codec + " has no compression levels", ErrorKind::Argument);
    }
    if (level < lowest || level > highest) {
        throw Error("compression level " + std::to_string(level) + ": " + codec + " has the levels " + std::to_string(lowest) +
                    " to " + std::to_string(highest), ErrorKind::Argument);
    }
}

}  // namespace

void compressBytes(const std::string& codec, int level, RandomBytes& in, uint64_t position, uint64_t size, ByteSink& out) {
    checkLevel(codec, level);
    if (!isXisfWriteCodec(codec)) {
        throw Error("unsupported XISF compression codec '" + codec + "' (use zlib, lz4, lz4hc or zstd)", ErrorKind::Argument);
    }
    if (codec == "lz4" || codec == "lz4hc") {
        lz4Compress(in, position, size, codec == "lz4" ? 0 : (level ? level : 9), out);
        return;
    }
    const std::unique_ptr<StreamCompressor> compressor = StreamCompressor::create(codec, level, size, out);
    const size_t piece = size_t(1) << 20;
    const uint8_t* all = in.contiguous();
    std::vector<uint8_t> buffer(all ? 0 : static_cast<size_t>(std::min<uint64_t>(size, piece)));
    for (uint64_t done = 0; done < size;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(piece, size - done));
        if (all) {
            compressor->write(all + position + done, n);
        } else {
            in.read(position + done, n, buffer.data());
            compressor->write(buffer.data(), n);
        }
        done += n;
        progressTick(n);
    }
    compressor->finish();
}

std::vector<uint8_t> zlibCompress(const uint8_t* src, size_t srcSize, int level) {
    std::vector<uint8_t> out;
    VectorSink sink(out);
    ZlibCompressor compressor(level, sink);
    compressor.write(src, srcSize);
    compressor.finish();
    return out;
}

std::vector<uint8_t> zstdCompress(const uint8_t* src, size_t srcSize, int level) {
    std::vector<uint8_t> out;
    VectorSink sink(out);
    const std::unique_ptr<StreamCompressor> compressor = StreamCompressor::create("zstd", level, srcSize, sink);
    compressor->write(src, srcSize);
    compressor->finish();
    return out;
}

// ---------------------------------------------------------------- decompression in pieces

namespace {

// Reads the compressed bytes a piece at a time.
class PieceReader {
public:
    PieceReader(RandomBytes& in, uint64_t position, uint64_t size)
        : in_(in), position_(position), end_(position + size), all_(in.contiguous()) {}
    // The next piece (empty at the end).
    std::pair<const uint8_t*, size_t> next() {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(end_ - position_, size_t(1) << 20));
        if (n == 0) return {nullptr, 0};
        const uint8_t* p;
        if (all_) {
            p = all_ + position_;
        } else {
            buffer_.resize(n);
            in_.read(position_, n, buffer_.data());
            p = buffer_.data();
        }
        position_ += n;
        progressTick(n);
        return {p, n};
    }

private:
    RandomBytes& in_;
    uint64_t position_, end_;
    const uint8_t* all_;
    std::vector<uint8_t> buffer_;
};

void inflateBytes(RandomBytes& in, uint64_t position, uint64_t size, uint64_t expected, ByteSink& out) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) throw Error("zlib: inflateInit failed");
    struct End {
        z_stream& zs;
        ~End() { inflateEnd(&zs); }
    } end{zs};
    PieceReader reader(in, position, size);
    std::vector<uint8_t> buffer(size_t(1) << 20);
    uint64_t produced = 0;
    bool more = true;   // input that is not handed over yet
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        if (zs.avail_in == 0 && more) {
            const auto piece = reader.next();
            more = piece.second != 0;
            zs.next_in = const_cast<Bytef*>(piece.first);
            zs.avail_in = static_cast<uInt>(piece.second);
        }
        // (room for what is declared and one byte more, which tells data that goes beyond it)
        const size_t room = static_cast<size_t>(std::min<uint64_t>(buffer.size(), expected - produced + 1));
        zs.next_out = buffer.data();
        zs.avail_out = static_cast<uInt>(room);
        ret = inflate(&zs, Z_NO_FLUSH);
        const size_t made = room - zs.avail_out;
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) throw Error("zlib: " + std::string(zs.msg ? zs.msg : "error " + std::to_string(ret)));
        if (made > expected - produced) throw Error("zlib: decompressed data larger than declared size");
        if (made) out.write(buffer.data(), made);
        produced += made;
        if (ret == Z_BUF_ERROR && zs.avail_in == 0 && !more) throw Error("zlib: truncated compressed data");
    }
    if (produced != expected) {
        throw Error("zlib: decompressed " + std::to_string(produced) + " bytes, expected " + std::to_string(expected));
    }
}

void zstdBytes(RandomBytes& in, uint64_t position, uint64_t size, uint64_t expected, ByteSink& out) {
#ifdef XISFCONV_HAVE_ZSTD
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) throw Error("zstd: out of memory");
    struct End {
        ZSTD_DCtx* d;
        ~End() { ZSTD_freeDCtx(d); }
    } end{dctx};
    PieceReader reader(in, position, size);
    std::vector<uint8_t> buffer(ZSTD_DStreamOutSize());
    uint64_t produced = 0;
    size_t left = 1;   // ZSTD_decompressStream: 0 where a frame is complete
    bool any = false;
    for (auto piece = reader.next(); piece.second != 0; piece = reader.next()) {
        any = true;
        ZSTD_inBuffer zin{piece.first, piece.second, 0};
        while (zin.pos < zin.size || left > 0) {
            ZSTD_outBuffer zout{buffer.data(), buffer.size(), 0};
            left = ZSTD_decompressStream(dctx, &zout, &zin);
            if (ZSTD_isError(left)) throw Error(std::string("zstd: ") + ZSTD_getErrorName(left));
            if (zout.pos > expected - produced) throw Error("zstd: decompressed data larger than declared size");
            if (zout.pos) out.write(buffer.data(), zout.pos);
            produced += zout.pos;
            // (all input taken and the output not full: there is no more to be had from this piece)
            if (zin.pos == zin.size && zout.pos < zout.size) break;
        }
    }
    if (!any || left != 0) throw Error("zstd: truncated compressed data");
    if (produced != expected) {
        throw Error("zstd: decompressed " + std::to_string(produced) + " bytes, expected " + std::to_string(expected));
    }
#else
    (void)in;
    (void)position;
    (void)size;
    (void)expected;
    (void)out;
    throw Unsupported("this build has no Zstandard support (rebuild with libzstd)");
#endif
}

// The LZ4 block format, with the last 64 KiB of what was decoded at hand for the matches.
void lz4Bytes(RandomBytes& in, uint64_t position, uint64_t size, uint64_t expected, ByteSink& out) {
    auto corrupt = [] { throw Error("lz4: corrupt compressed data"); };
    PieceReader reader(in, position, size);
    std::pair<const uint8_t*, size_t> piece{nullptr, 0};
    size_t at = 0;              // in the piece
    uint64_t consumed = 0;      // of the input
    auto available = [&]() {
        if (at < piece.second) return true;
        piece = reader.next();
        at = 0;
        return piece.second != 0;
    };
    auto get = [&]() -> unsigned {
        if (!available()) corrupt();
        ++consumed;
        return piece.first[at++];
    };
    constexpr size_t kKeep = 65536, kFlush = size_t(1) << 20;
    std::vector<uint8_t> window;   // the end of what was decoded
    window.reserve(kKeep + kFlush + 1024);
    uint64_t produced = 0;
    auto flush = [&](bool all) {
        const size_t keep = all ? 0 : std::min(window.size(), kKeep);
        const size_t n = window.size() - keep;
        if (n == 0) return;
        out.write(window.data(), n);
        window.erase(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(n));
    };
    while (consumed < size) {
        const unsigned token = get();
        uint64_t litLen = token >> 4;
        if (litLen == 15) {
            unsigned b;
            do {
                b = get();
                litLen += b;
            } while (b == 255);
        }
        if (litLen > size - consumed || litLen > expected - produced) corrupt();
        for (uint64_t left = litLen; left > 0;) {
            if (!available()) corrupt();
            const size_t n = static_cast<size_t>(std::min<uint64_t>(left, piece.second - at));
            window.insert(window.end(), piece.first + at, piece.first + at + n);
            at += n;
            consumed += n;
            left -= n;
            if (window.size() >= kKeep + kFlush) flush(false);
        }
        produced += litLen;
        if (consumed >= size) break;  // the last sequence carries literals only

        if (size - consumed < 2) corrupt();
        const unsigned lo = get(), hi = get();
        const size_t offset = lo | (hi << 8);
        if (offset == 0 || offset > produced) corrupt();
        uint64_t matchLen = token & 15;
        if (matchLen == 15) {
            unsigned b;
            do {
                b = get();
                matchLen += b;
            } while (b == 255);
        }
        matchLen += 4;
        if (matchLen > expected - produced) corrupt();
        for (uint64_t left = matchLen; left > 0;) {
            // (offset <= 65535 < kKeep: the match is in the window)
            const size_t n = static_cast<size_t>(std::min<uint64_t>(left, kFlush));
            const size_t start = window.size() - offset;
            window.resize(window.size() + n);
            uint8_t* d = window.data() + window.size() - n;
            const uint8_t* s = window.data() + start;
            if (offset >= n) std::memcpy(d, s, n);
            else for (size_t i = 0; i < n; ++i) d[i] = s[i];   // overlapping copy
            left -= n;
            if (window.size() >= kKeep + kFlush) flush(false);
        }
        produced += matchLen;
    }
    flush(true);
    if (produced != expected) {
        throw Error("lz4: decompressed " + std::to_string(produced) + " bytes, expected " + std::to_string(expected));
    }
}

}  // namespace

void decompressBytes(const std::string& codec, RandomBytes& in, uint64_t position, uint64_t size, uint64_t expected,
                     ByteSink& out) {
    if (codec == "zlib") inflateBytes(in, position, size, expected, out);
    else if (codec == "lz4" || codec == "lz4hc") lz4Bytes(in, position, size, expected, out);
    else if (codec == "zstd") zstdBytes(in, position, size, expected, out);
    else throw Error("unsupported compression codec '" + codec + "'");
}

bool isXisfWriteCodec(const std::string& codec) {
    return codec == "zlib" || codec == "lz4" || codec == "lz4hc" || codec == "zstd";
}

bool xisfCodecLevels(const std::string& codec, int& lowest, int& highest) {
    lowest = 1;
    if (codec == "zlib") highest = 9;
    else if (codec == "lz4hc") highest = 12;
    else if (codec == "zstd") {
#ifdef XISFCONV_HAVE_ZSTD
        highest = ZSTD_maxCLevel();
#else
        highest = 22;
#endif
    } else return false;
    return true;
}

std::vector<uint8_t> xisfCompress(const std::string& codec, const uint8_t* src, size_t srcSize, int level) {
    std::vector<uint8_t> out;
    VectorSink sink(out);
    MemoryBytes in(src, srcSize);
    compressBytes(codec, level, in, 0, srcSize, sink);
    return out;
}

uint64_t xisfSubblockSize(const std::string& codec, uint64_t wanted) {
    wanted = std::max<uint64_t>(1, wanted);
    return codec == "lz4" || codec == "lz4hc" ? std::min(wanted, kLz4MaxInput) : wanted;
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

// ---------------------------------------------------------------- digests
// SHA-1, SHA-256 and SHA-512 (FIPS 180-4), SHA3-256 and SHA3-512 (FIPS 202), MD5 (RFC 1321), each
// fed a piece at a time.

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

void sha1Block(uint32_t h[5], const uint8_t* blk) {
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
}

void sha256Block(uint32_t h[8], const uint8_t* blk) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
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
}

void sha512Block(uint64_t h[8], const uint8_t* blk) {
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
}

void md5Block(uint32_t h[4], const uint8_t* blk) {
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
}

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

enum class HashKind { Sha1, Sha256, Sha512, Sha3_256, Sha3_512, Md5 };

bool hashKindOf(const std::string& algorithm, HashKind& kind) {
    const std::string a = toLower(trim(algorithm));
    if (a == "sha1" || a == "sha-1") kind = HashKind::Sha1;
    else if (a == "sha256" || a == "sha-256") kind = HashKind::Sha256;
    else if (a == "sha512" || a == "sha-512") kind = HashKind::Sha512;
    else if (a == "sha3-256") kind = HashKind::Sha3_256;
    else if (a == "sha3-512") kind = HashKind::Sha3_512;
    else if (a == "md5") kind = HashKind::Md5;
    else return false;
    return true;
}

}  // namespace

struct Hasher::State {
    HashKind kind;
    size_t block = 64;          // the bytes one step takes: 64, 128, or the rate of SHA-3
    uint32_t h32[8] = {};
    uint64_t h64[8] = {};
    uint64_t keccak[25] = {};
    uint8_t buffer[200] = {};   // what does not fill a block yet
    size_t used = 0;
    uint64_t total = 0;         // bytes so far

    void step(const uint8_t* blk) {
        switch (kind) {
            case HashKind::Sha1: sha1Block(h32, blk); break;
            case HashKind::Sha256: sha256Block(h32, blk); break;
            case HashKind::Sha512: sha512Block(h64, blk); break;
            case HashKind::Md5: md5Block(h32, blk); break;
            default:
                for (size_t i = 0; i < block / 8; ++i) {
                    uint64_t w = 0;
                    for (int k = 7; k >= 0; --k) w = (w << 8) | blk[i * 8 + static_cast<size_t>(k)];   // little-endian lanes
                    keccak[i] ^= w;
                }
                keccakF1600(keccak);
        }
    }
};

bool Hasher::known(const std::string& algorithm) {
    HashKind kind;
    return hashKindOf(algorithm, kind);
}

Hasher::Hasher(const std::string& algorithm) : s_(new State()) {
    if (!hashKindOf(algorithm, s_->kind)) throw Error("unknown digest algorithm '" + algorithm + "'", ErrorKind::Argument);
    static const uint32_t sha1Init[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    static const uint32_t sha256Init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    static const uint64_t sha512Init[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                                           0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                           0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    static const uint32_t md5Init[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    switch (s_->kind) {
        case HashKind::Sha1: std::memcpy(s_->h32, sha1Init, sizeof sha1Init); break;
        case HashKind::Sha256: std::memcpy(s_->h32, sha256Init, sizeof sha256Init); break;
        case HashKind::Sha512: std::memcpy(s_->h64, sha512Init, sizeof sha512Init); s_->block = 128; break;
        case HashKind::Md5: std::memcpy(s_->h32, md5Init, sizeof md5Init); break;
        case HashKind::Sha3_256: s_->block = 200 - 2 * 32; break;
        case HashKind::Sha3_512: s_->block = 200 - 2 * 64; break;
    }
}

Hasher::~Hasher() = default;
Hasher::Hasher(Hasher&&) noexcept = default;
Hasher& Hasher::operator=(Hasher&&) noexcept = default;

void Hasher::update(const uint8_t* data, size_t n) {
    State& s = *s_;
    s.total += n;
    if (s.used) {
        const size_t take = std::min(n, s.block - s.used);
        std::memcpy(s.buffer + s.used, data, take);
        s.used += take;
        data += take;
        n -= take;
        if (s.used < s.block) return;
        s.step(s.buffer);
        s.used = 0;
    }
    for (; n >= s.block; data += s.block, n -= s.block) s.step(data);
    if (n) std::memcpy(s.buffer, data, n);
    s.used = n;
}

std::vector<uint8_t> Hasher::finish() {
    State& s = *s_;
    std::vector<uint8_t> out;
    if (s.kind == HashKind::Sha3_256 || s.kind == HashKind::Sha3_512) {
        uint8_t last[200] = {};
        std::memcpy(last, s.buffer, s.used);
        last[s.used] ^= 0x06;
        last[s.block - 1] ^= 0x80;
        s.step(last);
        const size_t digestBytes = s.kind == HashKind::Sha3_256 ? 32 : 64;
        for (size_t i = 0; i < digestBytes; ++i) out.push_back(static_cast<uint8_t>(s.keccak[i / 8] >> (8 * (i % 8))));
        return out;
    }
    // Merkle-Damgard padding: 0x80, zeros, the length in bits (big-endian; little-endian for MD5)
    const size_t lengthBytes = s.kind == HashKind::Sha512 ? 16 : 8;
    uint8_t last[256] = {};
    std::memcpy(last, s.buffer, s.used);
    last[s.used] = 0x80;
    const size_t total = s.used + 1 + lengthBytes <= s.block ? s.block : 2 * s.block;
    const uint64_t bits = s.total * 8;
    for (int i = 0; i < 8; ++i) {
        if (s.kind == HashKind::Md5) last[total - 8 + static_cast<size_t>(i)] = static_cast<uint8_t>(bits >> (8 * i));
        else last[total - 1 - static_cast<size_t>(i)] = static_cast<uint8_t>(bits >> (8 * i));
    }
    s.step(last);
    if (total == 2 * s.block) s.step(last + s.block);
    switch (s.kind) {
        case HashKind::Sha1:
        case HashKind::Sha256:
            for (int i = 0; i < (s.kind == HashKind::Sha1 ? 5 : 8); ++i)
                for (int j = 0; j < 4; ++j) out.push_back(static_cast<uint8_t>(s.h32[i] >> (24 - 8 * j)));
            break;
        case HashKind::Sha512:
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j) out.push_back(static_cast<uint8_t>(s.h64[i] >> (56 - 8 * j)));
            break;
        default:   // MD5
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) out.push_back(static_cast<uint8_t>(s.h32[i] >> (8 * j)));
    }
    return out;
}

std::string Hasher::finishHex() {
    const std::vector<uint8_t> digest = finish();
    return toHex(digest.data(), digest.size());
}

namespace {
std::string digestHex(const char* algorithm, const uint8_t* data, size_t size) {
    Hasher h(algorithm);
    h.update(data, size);
    return h.finishHex();
}
}  // namespace

std::string digestHex(const std::string& algorithm, RandomBytes& bytes, uint64_t position, uint64_t size) {
    Hasher hasher(algorithm);
    const uint8_t* all = bytes.contiguous();
    std::vector<uint8_t> piece;
    const size_t step = static_cast<size_t>(std::min<uint64_t>(size, std::max<uint64_t>(pieceSettings().pieceBytes, 4096)));
    for (uint64_t done = 0; done < size;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(step, size - done));
        if (all) {
            hasher.update(all + position + done, n);
        } else {
            piece.resize(n);
            bytes.read(position + done, n, piece.data());
            hasher.update(piece.data(), n);
        }
        done += n;
        progressTick(n);
    }
    return hasher.finishHex();
}

std::string sha1Hex(const uint8_t* data, size_t size) { return digestHex("sha1", data, size); }
std::string sha256Hex(const uint8_t* data, size_t size) { return digestHex("sha256", data, size); }
std::string sha512Hex(const uint8_t* data, size_t size) { return digestHex("sha512", data, size); }

std::string sha3Hex(const uint8_t* data, size_t size, int bits) {
    if (bits != 256 && bits != 512) throw Error("unsupported SHA-3 digest size");
    return digestHex(bits == 256 ? "sha3-256" : "sha3-512", data, size);
}

void md5(const uint8_t* data, size_t size, uint8_t digest[16]) {
    Hasher h("md5");
    h.update(data, size);
    const std::vector<uint8_t> d = h.finish();
    std::memcpy(digest, d.data(), 16);
}

}  // namespace xisfconv
