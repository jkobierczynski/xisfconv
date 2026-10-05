// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "fitsread.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

#include "fits.hpp"
#include "fitstile.hpp"

namespace xisfconv {

namespace {

constexpr uint64_t kBlock = 2880;

std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

struct Card {
    std::string name;     // upper case; empty for blank-keyword commentary
    std::string value;    // FITS-formatted; strings keep their quotes
    std::string comment;
    bool isString = false;
};

// Parses the value field (everything after "= ").
void parseValue(const std::string& field, Card& card) {
    size_t i = 0;
    while (i < field.size() && field[i] == ' ') ++i;
    if (i < field.size() && field[i] == '\'') {
        // String: runs to the closing quote; '' is an escaped quote.
        size_t j = i + 1;
        std::string inner;
        bool closed = false;
        while (j < field.size()) {
            if (field[j] == '\'') {
                if (j + 1 < field.size() && field[j + 1] == '\'') {
                    inner += "''";
                    j += 2;
                    continue;
                }
                closed = true;
                ++j;
                break;
            }
            inner += field[j++];
        }
        if (!closed) j = field.size();
        card.isString = true;
        card.value = "'" + rtrim(inner) + "'";
        const size_t slash = field.find('/', j);
        if (slash != std::string::npos) card.comment = trim(field.substr(slash + 1));
    } else {
        const size_t slash = field.find('/', i);
        card.value = trim(field.substr(i, slash == std::string::npos ? std::string::npos : slash - i));
        if (slash != std::string::npos) card.comment = trim(field.substr(slash + 1));
    }
}

Card parseCard(const std::string& raw) {
    Card card;
    const std::string key = rtrim(raw.substr(0, 8));
    if (key == "HIERARCH") {
        const size_t eq = raw.find('=', 8);
        if (eq != std::string::npos) {
            card.name = trim(raw.substr(8, eq - 8));
            parseValue(raw.substr(eq + 1), card);
            return card;
        }
    }
    card.name = toUpper(key);
    if (raw.size() >= 10 && raw[8] == '=' && raw[9] == ' ' && card.name != "COMMENT" && card.name != "HISTORY" &&
        !card.name.empty()) {
        parseValue(raw.substr(10), card);
    } else {
        // Commentary (COMMENT, HISTORY, blank keyword) or a keyword without a value.
        card.comment = rtrim(raw.size() > 8 ? raw.substr(8) : std::string());
        if (card.name == "CONTINUE") card.comment = raw.size() > 8 ? raw.substr(8) : std::string();
    }
    return card;
}

struct Header {
    std::vector<Card> cards;

    const Card* find(const std::string& name) const {
        for (const auto& c : cards)
            if (c.name == name) return &c;
        return nullptr;
    }
    bool getInt(const std::string& name, long long& out) const {
        const Card* c = find(name);
        if (!c || c->isString) return false;
        double d;
        if (!parseDouble(c->value, d) || d != std::floor(d)) return false;
        out = static_cast<long long>(d);
        return true;
    }
    bool getDouble(const std::string& name, double& out) const {
        const Card* c = find(name);
        if (!c || c->isString) return false;
        std::string v = c->value;
        for (auto& ch : v)
            if (ch == 'D' || ch == 'd') ch = 'E';  // Fortran-style exponent
        return parseDouble(v, out);
    }
    std::string getString(const std::string& name) const {
        const Card* c = find(name);
        return c && c->isString ? fitsUnquote(c->value) : std::string();
    }
};

// Reads one header (a whole number of 2880-byte blocks). Returns false at a clean end of file.
bool readHeader(std::ifstream& in, uint64_t fileSize, uint64_t& pos, Header& hdr, bool primary) {
    if (pos >= fileSize) return false;
    std::string block(kBlock, ' ');
    bool end = false, first = true;
    while (!end) {
        if (pos + kBlock > fileSize) {
            if (first) {
                // Trailing junk shorter than a block after the last HDU is tolerated.
                return false;
            }
            throw Error("FITS header is truncated (no END card)");
        }
        in.seekg(static_cast<std::streamoff>(pos));
        if (!in.read(&block[0], static_cast<std::streamsize>(kBlock))) throw Error("read error in FITS header", ErrorKind::Io);
        std::replace(block.begin(), block.end(), '\0', ' ');   // not valid in a header; C callers' text would end there
        pos += kBlock;
        if (first) {
            const std::string start = block.substr(0, 8);
            if (primary && start != "SIMPLE  ") throw Error("not a FITS file (no SIMPLE card)");
            if (!primary && start != "XTENSION") return false;  // padding or special records after the last HDU
            first = false;
        }
        for (size_t off = 0; off < kBlock; off += 80) {
            const std::string raw = block.substr(off, 80);
            if (rtrim(raw.substr(0, 8)) == "END" && rtrim(raw.substr(8)).empty()) {
                end = true;
                break;
            }
            Card card = parseCard(raw);
            if (card.name == "CONTINUE" && !hdr.cards.empty() && hdr.cards.back().isString) {
                // Long-string convention: previous value ends with '&', this card continues it.
                Card& prev = hdr.cards.back();
                Card cont;
                parseValue(card.comment, cont);
                if (cont.isString && prev.value.size() >= 3 && prev.value[prev.value.size() - 2] == '&') {
                    prev.value = prev.value.substr(0, prev.value.size() - 2) + cont.value.substr(1);
                    if (!cont.comment.empty()) prev.comment += (prev.comment.empty() ? "" : " ") + cont.comment;
                    continue;
                }
            }
            if (card.name.empty() && card.comment.empty() && card.value.empty()) continue;  // blank card
            hdr.cards.push_back(std::move(card));
            if (hdr.cards.size() > 100000) throw Error("FITS header has too many cards");
        }
    }
    return true;
}

template <class T>
T* as(std::vector<uint8_t>& v) {
    return reinterpret_cast<T*>(v.data());
}

// Converts samples (already in host byte order) to a floating point buffer, applying BSCALE/BZERO.
template <class S, class D>
void toFloat(std::vector<uint8_t>& data, size_t n, double bscale, double bzero) {
    std::vector<uint8_t> out(n * sizeof(D));
    const S* src = reinterpret_cast<const S*>(data.data());
    D* dst = reinterpret_cast<D*>(out.data());
    for (size_t i = 0; i < n; ++i) dst[i] = static_cast<D>(bzero + bscale * static_cast<double>(src[i]));
    data.swap(out);
}

template <class T>
bool anyNegative(const std::vector<uint8_t>& data, size_t n) {
    const T* p = reinterpret_cast<const T*>(data.data());
    for (size_t i = 0; i < n; ++i)
        if (p[i] < 0) return true;
    return false;
}

template <class T>
void floatRange(FitsImage& img, size_t n) {
    const T* p = reinterpret_cast<const T*>(img.pixels.data.data());
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(p[i]);
        if (std::isfinite(v)) {
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        } else {
            img.hasNaN = true;
        }
    }
    if (lo > hi) lo = hi = 0;
    img.dataMin = lo;
    img.dataMax = hi;
}

void decodeSamples(FitsImage& img, std::vector<uint8_t>& raw) {
    PixelBuffer& px = img.pixels;
    const size_t n = static_cast<size_t>(px.samples());
    const size_t sb = static_cast<size_t>(std::abs(img.bitpix)) / 8;
    if (raw.size() != n * sb) throw Error("the image data does not have the size the header gives it");
    if (hostIsLittleEndian()) byteSwapInPlace(raw.data(), n, sb);
    px.data.swap(raw);

    const bool plain = img.bscale == 1 && img.bzero == 0;
    auto offsetIs = [&](double z) { return img.bscale == 1 && img.bzero == z; };
    switch (img.bitpix) {
        case 8:
            if (plain) {
                px.format = SampleFormat::UInt8;
                img.note = "8-bit unsigned";
            } else {
                toFloat<uint8_t, float>(px.data, n, img.bscale, img.bzero);
                px.format = SampleFormat::Float32;
                img.note = "8-bit with BSCALE/BZERO -> Float32";
            }
            break;
        case 16:
            if (offsetIs(32768)) {
                uint16_t* p = as<uint16_t>(px.data);
                for (size_t i = 0; i < n; ++i) p[i] ^= 0x8000u;
                px.format = SampleFormat::UInt16;
                img.note = "16-bit unsigned (BZERO=32768)";
            } else if (plain && !anyNegative<int16_t>(px.data, n)) {
                px.format = SampleFormat::UInt16;
                img.note = "16-bit signed, no negative values -> UInt16";
            } else {
                toFloat<int16_t, float>(px.data, n, img.bscale, img.bzero);
                px.format = SampleFormat::Float32;
                img.note = plain ? "16-bit signed with negative values -> Float32" : "16-bit with BSCALE/BZERO -> Float32";
            }
            break;
        case 32:
            if (offsetIs(2147483648.0)) {
                uint32_t* p = as<uint32_t>(px.data);
                for (size_t i = 0; i < n; ++i) p[i] ^= 0x80000000u;
                px.format = SampleFormat::UInt32;
                img.note = "32-bit unsigned (BZERO=2^31)";
            } else if (plain && !anyNegative<int32_t>(px.data, n)) {
                px.format = SampleFormat::UInt32;
                img.note = "32-bit signed, no negative values -> UInt32";
            } else {
                toFloat<int32_t, double>(px.data, n, img.bscale, img.bzero);
                px.format = SampleFormat::Float64;
                img.note = plain ? "32-bit signed with negative values -> Float64" : "32-bit with BSCALE/BZERO -> Float64";
            }
            break;
        case 64:
            if (offsetIs(9223372036854775808.0)) {
                uint64_t* p = as<uint64_t>(px.data);
                for (size_t i = 0; i < n; ++i) p[i] ^= 0x8000000000000000ull;
                px.format = SampleFormat::UInt64;
                img.note = "64-bit unsigned (BZERO=2^63)";
            } else if (plain && !anyNegative<int64_t>(px.data, n)) {
                px.format = SampleFormat::UInt64;
                img.note = "64-bit signed, no negative values -> UInt64";
            } else {
                toFloat<int64_t, double>(px.data, n, img.bscale, img.bzero);
                px.format = SampleFormat::Float64;
                img.note = "64-bit signed -> Float64 (values beyond 2^53 lose precision)";
            }
            break;
        case -32:
            if (!plain) toFloat<float, float>(px.data, n, img.bscale, img.bzero);
            px.format = SampleFormat::Float32;
            img.note = "32-bit float";
            break;
        case -64:
            if (!plain) toFloat<double, double>(px.data, n, img.bscale, img.bzero);
            px.format = SampleFormat::Float64;
            img.note = "64-bit float";
            break;
        default:
            throw Error("unsupported BITPIX " + std::to_string(img.bitpix));
    }
    updateFloatRange(img);
}

uint64_t padded(uint64_t bytes) { return (bytes + kBlock - 1) / kBlock * kBlock; }

}  // namespace

void updateFloatRange(FitsImage& img) {
    const size_t n = static_cast<size_t>(img.pixels.samples());
    if (img.pixels.format == SampleFormat::Float32) floatRange<float>(img, n);
    else if (img.pixels.format == SampleFormat::Float64) floatRange<double>(img, n);
}

std::string fitsUnquote(const std::string& value) {
    const std::string v = trim(value);
    if (v.size() < 2 || v.front() != '\'') return v;
    const size_t last = v.rfind('\'');
    const std::string inner = last > 0 ? v.substr(1, last - 1) : v.substr(1);
    std::string out;
    for (size_t i = 0; i < inner.size(); ++i) {
        out += inner[i];
        if (inner[i] == '\'' && i + 1 < inner.size() && inner[i + 1] == '\'') ++i;
    }
    return rtrim(out);
}

bool looksLikeFits(const std::string& path) {
    std::ifstream in(toPath(path), std::ios::binary);
    char buf[9] = {};
    in.read(buf, 9);
    return in.gcount() == 9 && std::string(buf, 9) == "SIMPLE  =";
}

namespace {

// A binary table extension that holds a tile-compressed image (written by fpack, CFITSIO, astropy).
bool isTiledImage(const Header& hdr, const std::string& xtension) {
    const Card* z = hdr.find("ZIMAGE");
    return xtension == "BINTABLE" && z && trim(z->value) == "T";
}

// Gathers the compression parameters and the table layout from the header.
TiledImage tiledImageFromHeader(const Header& hdr, const std::string& label, uint64_t rowBytes, uint64_t rows) {
    TiledImage t;
    t.algorithm = toUpper(hdr.getString("ZCMPTYPE"));
    long long v = 0, n = 0;
    if (!hdr.getInt("ZBITPIX", v) || !hdr.getInt("ZNAXIS", n) || n < 0 || n > 9) throw Error(label + ": missing or invalid ZBITPIX/ZNAXIS");
    t.bitpix = static_cast<int>(v);
    for (long long k = 1; k <= n; ++k) {
        long long d = 0;
        if (!hdr.getInt("ZNAXIS" + std::to_string(k), d) || d < 0) throw Error(label + ": missing or invalid ZNAXIS" + std::to_string(k));
        t.naxis.push_back(static_cast<uint64_t>(d));
        long long tile = k == 1 ? d : 1;  // the default: one row of the image per tile
        if (hdr.getInt("ZTILE" + std::to_string(k), tile) && tile <= 0) throw Error(label + ": invalid ZTILE" + std::to_string(k));
        t.tile.push_back(static_cast<uint64_t>(tile));
    }
    for (int i = 1; i < 100; ++i) {
        const std::string name = toUpper(hdr.getString("ZNAME" + std::to_string(i)));
        if (name.empty()) break;
        if (!hdr.getInt("ZVAL" + std::to_string(i), v)) continue;
        if (v < 0 || v > 1000000) throw Error(label + ": invalid " + name);
        if (name == "BLOCKSIZE") t.riceBlockSize = static_cast<int>(v);
        else if (name == "BYTEPIX") t.riceBytePix = static_cast<int>(v);
    }
    t.quantize = toUpper(hdr.getString("ZQUANTIZ"));
    hdr.getInt("ZDITHER0", t.ditherSeed);
    t.hasBlank = hdr.getInt("ZBLANK", t.blank);
    const bool hasScale = hdr.getDouble("ZSCALE", t.scale), hasZero = hdr.getDouble("ZZERO", t.zero);
    t.hasScale = hasScale || hasZero;

    t.rowBytes = rowBytes;
    t.rows = rows;
    t.heapOffset = checkedMul(rowBytes, rows, "table size");
    if (hdr.getInt("THEAP", v)) {
        if (v < 0 || static_cast<uint64_t>(v) < t.heapOffset) throw Error(label + ": invalid THEAP");
        t.heapOffset = static_cast<uint64_t>(v);
    }
    long long fields = 0;
    if (!hdr.getInt("TFIELDS", fields) || fields < 1 || fields > 999) throw Error(label + ": missing or invalid TFIELDS");
    uint64_t offset = 0;
    for (long long i = 1; i <= fields; ++i) {
        TileColumn c;
        c.name = toUpper(hdr.getString("TTYPE" + std::to_string(i)));
        const std::string form = toUpper(hdr.getString("TFORM" + std::to_string(i)));
        size_t p = 0;
        uint64_t repeat = 0;
        bool hasRepeat = false;
        while (p < form.size() && form[p] >= '0' && form[p] <= '9' && repeat < 100000000) {
            repeat = repeat * 10 + static_cast<uint64_t>(form[p++] - '0');
            hasRepeat = true;
        }
        c.repeat = hasRepeat ? repeat : 1;
        if (p >= form.size()) throw Error(label + ": invalid TFORM" + std::to_string(i));
        uint64_t width = 0;
        if (form[p] == 'P' || form[p] == 'Q') {
            c.variable = true;
            c.wide = form[p] == 'Q';
            if (p + 1 >= form.size()) throw Error(label + ": invalid TFORM" + std::to_string(i));
            c.type = form[p + 1];
            width = c.repeat * (c.wide ? 16 : 8);
        } else {
            c.type = form[p];
            static const std::string known = "LXBIJKAEDCM";
            const size_t code = known.find(c.type);
            if (code == std::string::npos) throw Error(label + ": invalid TFORM" + std::to_string(i));
            static const uint64_t bytes[] = {1, 0, 1, 2, 4, 8, 1, 4, 8, 8, 16};
            width = c.type == 'X' ? (c.repeat + 7) / 8 : c.repeat * bytes[code];
        }
        c.offset = offset;
        offset += width;
        t.columns.push_back(std::move(c));
    }
    if (offset != rowBytes) throw Error(label + ": the columns do not add up to the row length of the table");
    return t;
}

// Keywords that describe the table and the compression, not the image.
bool isTileKeyword(const std::string& name) {
    static const char* exact[] = {"TFIELDS", "THEAP", "ZIMAGE", "ZCMPTYPE", "ZBITPIX", "ZNAXIS", "ZMASKCMP", "ZQUANTIZ", "ZDITHER0",
                                  "ZSIMPLE", "ZEXTEND", "ZBLOCKED", "ZTENSION", "ZPCOUNT", "ZGCOUNT", "ZHECKSUM", "ZDATASUM",
                                  "ZBLANK", "ZSCALE", "ZZERO"};
    for (const char* e : exact)
        if (name == e) return true;
    static const char* indexed[] = {"TTYPE", "TFORM", "TUNIT", "TDIM", "TNULL", "TSCAL", "TZERO", "TDISP", "ZNAXIS", "ZTILE", "ZNAME", "ZVAL"};
    for (const char* prefix : indexed) {
        uint64_t k;
        if (startsWith(name, prefix) && parseUInt64(name.substr(std::strlen(prefix)), k)) return true;
    }
    return false;
}

std::vector<uint8_t> readBytes(std::ifstream& in, uint64_t pos, uint64_t size, const std::string& label) {
    if (size > std::numeric_limits<size_t>::max() / 2) throw Error("image too large for this platform");
    std::vector<uint8_t> raw(static_cast<size_t>(size));
    in.clear();
    in.seekg(static_cast<std::streamoff>(pos));
    if (size && !in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(size))) {
        throw Error(label + ": read error in image data", ErrorKind::Io);
    }
    return raw;
}

// 32-bit ones' complement sum of big-endian words (the FITS checksum convention) over `size`
// bytes at `pos`. A last incomplete word counts as if it were padded with zeros, as do the
// missing bytes of a data unit that was left unpadded.
uint32_t onesComplementSum(std::ifstream& in, uint64_t pos, uint64_t size) {
    std::vector<uint8_t> buf(static_cast<size_t>(std::min<uint64_t>(size + 4, 1u << 20)));
    uint64_t sum = 0;
    in.clear();
    in.seekg(static_cast<std::streamoff>(pos));
    while (size > 0) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(size, 1u << 20));
        if (!in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n))) throw Error("read error", ErrorKind::Io);
        size_t words = n;
        while (words % 4 != 0) buf[words++] = 0;  // only at the very end (the chunk size is a multiple of 4)
        for (size_t i = 0; i < words; i += 4) {
            sum += (static_cast<uint32_t>(buf[i]) << 24) | (static_cast<uint32_t>(buf[i + 1]) << 16) |
                   (static_cast<uint32_t>(buf[i + 2]) << 8) | buf[i + 3];
        }
        sum = (sum & 0xFFFFFFFFull) + (sum >> 32);  // fold per chunk, so the accumulator cannot overflow
        size -= n;
    }
    while (sum >> 32) sum = (sum & 0xFFFFFFFFull) + (sum >> 32);
    return static_cast<uint32_t>(sum);
}

uint32_t onesComplementAdd(uint32_t a, uint32_t b) {
    uint64_t sum = static_cast<uint64_t>(a) + b;
    while (sum >> 32) sum = (sum & 0xFFFFFFFFull) + (sum >> 32);
    return static_cast<uint32_t>(sum);
}

}  // namespace

VerifyReport verifyFits(const std::string& path) {
    VerifyReport report;
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in) throw Error("cannot open file", ErrorKind::Io);
    in.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(in.tellg());

    uint64_t pos = 0;
    size_t hdus = 0;
    for (size_t hduIndex = 0;; ++hduIndex) {
        progress("verifying", pos, fileSize);
        Header hdr;
        const uint64_t headerPos = pos;
        if (!readHeader(in, fileSize, pos, hdr, hduIndex == 0)) {
            // Whatever follows the last HDU must be padding. Anything else is the remains of an
            // HDU: a cut-off header, or one whose first card is damaged.
            pos = headerPos;
            uint64_t stray = 0;
            in.clear();
            in.seekg(static_cast<std::streamoff>(pos));
            std::vector<char> buf(65536);
            for (uint64_t left = fileSize - pos; left > 0;) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
                if (!in.read(buf.data(), static_cast<std::streamsize>(n))) throw Error("read error", ErrorKind::Io);
                for (size_t i = 0; i < n; ++i)
                    if (buf[i] != 0 && buf[i] != ' ') ++stray;
                left -= n;
            }
            if (stray) {
                report.problems.push_back(std::to_string(fileSize - pos) + " bytes after HDU " + std::to_string(hduIndex - 1) +
                                          " are not padding: a truncated or damaged extension");
            }
            break;
        }
        const std::string label = "HDU " + std::to_string(hduIndex);
        long long bitpix = 0, naxis = 0, pcount = 0, gcount = 1;
        if (!hdr.getInt("BITPIX", bitpix) || !hdr.getInt("NAXIS", naxis) || naxis < 0 || naxis > 999 ||
            (bitpix != 8 && bitpix != 16 && bitpix != 32 && bitpix != 64 && bitpix != -32 && bitpix != -64)) {
            throw Error(label + ": missing or invalid BITPIX/NAXIS");
        }
        std::vector<uint64_t> dims;
        for (long long k = 1; k <= naxis; ++k) {
            long long d = 0;
            if (!hdr.getInt("NAXIS" + std::to_string(k), d) || d < 0) throw Error(label + ": missing or invalid NAXIS" + std::to_string(k));
            dims.push_back(static_cast<uint64_t>(d));
        }
        // Random groups (primary HDU, NAXIS1 = 0, GROUPS = T): the data are GCOUNT groups of
        // PCOUNT parameters plus an array of NAXIS2 x ... elements.
        const Card* groups = hdr.find("GROUPS");
        const bool randomGroups = hduIndex == 0 && dims.size() > 1 && dims[0] == 0 && groups && trim(groups->value) == "T";
        uint64_t elements = dims.empty() ? 0 : 1;
        for (size_t k = randomGroups ? 1 : 0; k < dims.size(); ++k) elements = checkedMul(elements, dims[k], "FITS data size");
        if (hduIndex > 0 || randomGroups) {
            hdr.getInt("PCOUNT", pcount);
            hdr.getInt("GCOUNT", gcount);
            if (pcount < 0 || gcount < 0) throw Error(label + ": invalid PCOUNT/GCOUNT");
        }
        const uint64_t sb = static_cast<uint64_t>(bitpix < 0 ? -bitpix : bitpix) / 8;
        const uint64_t dataBytes = checkedMul(checkedMul(sb, static_cast<uint64_t>(gcount), "FITS data size"),
                                              static_cast<uint64_t>(pcount) + elements, "FITS data size");
        const uint64_t dataPos = pos;
        if (dataBytes > fileSize || dataPos > fileSize - dataBytes) {
            throw Error(label + ": data extends beyond the end of the file (truncated?)");
        }
        // The last data unit of a file is sometimes left unpadded.
        const uint64_t stored = std::min<uint64_t>(padded(dataBytes), fileSize - dataPos);
        pos = dataPos + stored;
        ++hdus;

        // A tile-compressed image must decompress.
        if (isTiledImage(hdr, toUpper(hdr.getString("XTENSION")))) {
            try {
                if (dims.size() != 2) throw Error("invalid binary table");
                const TiledImage tile = tiledImageFromHeader(hdr, label, dims[0], dims[1]);
                bool empty = tile.naxis.empty();
                for (uint64_t d : tile.naxis)
                    if (d == 0) empty = true;
                if (!empty) decodeTiledImage(tile, readBytes(in, dataPos, dataBytes, label));
            } catch (const Unsupported& e) {
                report.notChecked.push_back(label + ": " + e.what());
            } catch (const Error& e) {
                const std::string message = e.what();
                report.problems.push_back(message.find(label) == std::string::npos ? label + ": " + message : message);
            }
        }

        const Card* checksum = hdr.find("CHECKSUM");
        const Card* datasum = hdr.find("DATASUM");
        if (!checksum && !datasum) {
            ++report.unchecked;
            continue;
        }
        const uint32_t dataSum = onesComplementSum(in, dataPos, stored);
        bool good = true;
        if (datasum) {
            uint64_t expected = 0;
            if (!parseUInt64(fitsUnquote(datasum->value), expected) || expected != dataSum) {
                report.problems.push_back(label + ": DATASUM mismatch: the data unit is damaged (keyword " +
                                          fitsUnquote(datasum->value) + ", data " + std::to_string(dataSum) + ")");
                good = false;
            }
        }
        if (checksum) {
            // With a correct CHECKSUM card the sum over the whole HDU is all ones (or zero).
            const uint32_t total = onesComplementAdd(onesComplementSum(in, headerPos, dataPos - headerPos), dataSum);
            if (total != 0xFFFFFFFFu && total != 0) {
                report.problems.push_back(label + ": CHECKSUM mismatch: " + (datasum && good ? "the header was changed" : "the HDU is damaged"));
                good = false;
            }
        }
        if (good) ++report.verified;
    }
    report.summary = std::to_string(hdus) + (hdus == 1 ? " HDU" : " HDUs");
    return report;
}

FitsFile readFits(const std::string& path, bool headersOnly, std::optional<size_t> onlyImage) {
    FitsFile file;
    file.path = path;
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in) throw Error("cannot open file", ErrorKind::Io);
    in.seekg(0, std::ios::end);
    file.fileSize = static_cast<uint64_t>(in.tellg());

    uint64_t pos = 0;
    for (size_t hduIndex = 0;; ++hduIndex) {
        Header hdr;
        if (!readHeader(in, file.fileSize, pos, hdr, hduIndex == 0)) break;

        long long bitpix = 0, naxis = 0, pcount = 0, gcount = 1;
        if (!hdr.getInt("BITPIX", bitpix) || !hdr.getInt("NAXIS", naxis) || naxis < 0 || naxis > 999) {
            throw Error("HDU " + std::to_string(hduIndex) + ": missing or invalid BITPIX/NAXIS");
        }
        if (bitpix != 8 && bitpix != 16 && bitpix != 32 && bitpix != 64 && bitpix != -32 && bitpix != -64) {
            throw Error("HDU " + std::to_string(hduIndex) + ": invalid BITPIX " + std::to_string(bitpix));
        }
        std::vector<uint64_t> dims;
        for (long long k = 1; k <= naxis; ++k) {
            long long d = 0;
            if (!hdr.getInt("NAXIS" + std::to_string(k), d) || d < 0) {
                throw Error("HDU " + std::to_string(hduIndex) + ": missing or invalid NAXIS" + std::to_string(k));
            }
            dims.push_back(static_cast<uint64_t>(d));
        }
        const std::string xtension = toUpper(hdr.getString("XTENSION"));
        const bool isImage = hduIndex == 0 || xtension == "IMAGE";
        if (hduIndex > 0) {
            hdr.getInt("PCOUNT", pcount);
            hdr.getInt("GCOUNT", gcount);
            if (pcount < 0 || gcount < 0) throw Error("HDU " + std::to_string(hduIndex) + ": invalid PCOUNT/GCOUNT");
            // An image is its pixels and nothing else; other values would shift or drop them.
            if (xtension == "IMAGE" && (pcount != 0 || gcount != 1)) {
                throw Error("HDU " + std::to_string(hduIndex) + ": an IMAGE extension must have PCOUNT = 0 and GCOUNT = 1");
            }
        }
        uint64_t elements = dims.empty() ? 0 : 1;
        for (uint64_t d : dims) elements = checkedMul(elements, d, "FITS data size");
        const uint64_t sb = static_cast<uint64_t>(bitpix < 0 ? -bitpix : bitpix) / 8;
        const uint64_t dataBytes =
            checkedMul(checkedMul(sb, static_cast<uint64_t>(gcount), "FITS data size"),
                       static_cast<uint64_t>(pcount) + elements, "FITS data size");
        const uint64_t dataPos = pos;
        if (dataBytes > file.fileSize || dataPos > file.fileSize - dataBytes) {
            throw Error("HDU " + std::to_string(hduIndex) + ": data extends beyond the end of the file (truncated?)");
        }
        pos = dataPos + padded(dataBytes);

        const std::string label = "HDU " + std::to_string(hduIndex);
        const bool tiled = isTiledImage(hdr, xtension);
        TiledImage tile;
        if (tiled) {
            // The image is described by the Z keywords; the table only carries its compressed tiles.
            if (dims.size() != 2) throw Error(label + ": invalid binary table");
            tile = tiledImageFromHeader(hdr, label, dims[0], dims[1]);
            if (!tileAlgorithmSupported(tile.algorithm)) {
                file.skipped.push_back(label + ": tile-compressed image (" + (tile.algorithm.empty() ? "unknown method" : tile.algorithm) +
                                       "), which is not supported; decompress it with funpack first");
                continue;
            }
            // Known from the header alone, so that the image is left out of every kind of read
            // and the numbering of the others does not depend on whether pixels are read.
            if ((tile.algorithm == "RICE_1" || tile.algorithm == "RICE_ONE") && tile.riceBytePix != 1 && tile.riceBytePix != 2 &&
                tile.riceBytePix != 4) {
                file.skipped.push_back(label + ": tile-compressed image: Rice compression with " + std::to_string(tile.riceBytePix) +
                                       " bytes per pixel is not supported");
                continue;
            }
            bitpix = tile.bitpix;
            if (bitpix != 8 && bitpix != 16 && bitpix != 32 && bitpix != 64 && bitpix != -32 && bitpix != -64) {
                throw Error(label + ": invalid ZBITPIX " + std::to_string(bitpix));
            }
            dims = tile.naxis;
            naxis = static_cast<long long>(dims.size());
            elements = dims.empty() ? 0 : 1;
            for (uint64_t d : dims) elements = checkedMul(elements, d, "FITS data size");
        } else if (!isImage) {
            file.skipped.push_back(label + ": " + (xtension.empty() ? "unknown" : xtension) + " extension (not an image)");
            continue;
        }
        // Trailing axes of length 1 are ignored; leading axes are width, height[, channels].
        while (dims.size() > 2 && dims.back() == 1) dims.pop_back();
        if (elements == 0) {
            if (hduIndex > 0 || naxis != 0) file.skipped.push_back(label + ": no image data");
            continue;
        }
        if (dims.size() < 2 || dims.size() > 3) {
            file.skipped.push_back(label + ": " + std::to_string(dims.size()) + "-dimensional data is not supported");
            continue;
        }

        FitsImage img;
        img.hduIndex = hduIndex;
        img.bitpix = static_cast<int>(bitpix);
        hdr.getDouble("BSCALE", img.bscale);
        hdr.getDouble("BZERO", img.bzero);
        if (img.bscale == 0) img.bscale = 1;
        img.pixels.width = dims[0];
        img.pixels.height = dims[1];
        img.pixels.channels = dims.size() == 3 ? dims[2] : 1;
        img.name = hdr.getString("EXTNAME");
        if (tiled && img.name == "COMPRESSED_IMAGE") img.name.clear();  // the name fpack and astropy give the table
        if (img.name.empty()) img.name = hdr.getString("HDUNAME");
        if (tiled) img.tileCompression = tile.algorithm;
        const std::string rowOrder = toUpper(hdr.getString("ROWORDER"));
        if (!rowOrder.empty()) {
            img.hasRowOrder = true;
            img.topDown = rowOrder == "TOP-DOWN";
        }
        if (hdr.find("BLANK") && bitpix > 0 && (!onlyImage || *onlyImage == file.images.size())) {
            warn(label + ": BLANK (undefined) pixels are kept as ordinary sample values");
        }
        for (const auto& c : hdr.cards) {
            if (isReservedFitsKeyword(c.name) || c.name == "EXTNAME" || c.name == "HDUNAME" || c.name == "CONTINUE") {
                continue;
            }
            if (tiled && isTileKeyword(c.name)) continue;
            img.keywords.push_back({c.name, c.value, c.comment});
        }

        if (!headersOnly && (!onlyImage || *onlyImage == file.images.size())) {
            std::vector<uint8_t> raw = readBytes(in, dataPos, dataBytes, label);
            if (tiled) {
                try {
                    raw = decodeTiledImage(tile, raw);
                } catch (const Unsupported& e) {
                    if (onlyImage) throw Unsupported(label + ": tile-compressed image: " + e.what());
                    file.skipped.push_back(label + ": tile-compressed image: " + e.what());
                    continue;
                } catch (const Error& e) {
                    throw Error(label + ": tile-compressed image: " + e.what(), e.kind);
                }
            }
            decodeSamples(img, raw);
            if (tiled) img.note += ", " + tile.algorithm + " tile compression";
            img.hasData = true;
        }
        file.images.push_back(std::move(img));
        if (onlyImage && file.images.size() > *onlyImage) break;   // the one image that was asked for is read
    }
    return file;
}

}  // namespace xisfconv
