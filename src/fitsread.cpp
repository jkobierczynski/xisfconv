// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "fitsread.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

#include "bytes.hpp"
#include "fits.hpp"
#include "fitstile.hpp"
#include "imagesource.hpp"

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

// How the stored samples of an image become those it is read as (BITPIX, BSCALE, BZERO).
struct FitsMapping {
    enum class Op { Same, FlipSign, ToFloat };
    Op op = Op::Same;
    SampleFormat raw = SampleFormat::UInt8;      // the stored samples in host order, as unsigned integers or floats
    SampleFormat format = SampleFormat::UInt8;   // what they become
    int bitpix = 0;
    double bscale = 1, bzero = 0;
    std::string note;
};

SampleFormat rawFormatOf(int bitpix) {
    switch (bitpix) {
        case 8: return SampleFormat::UInt8;
        case 16: return SampleFormat::UInt16;
        case 32: return SampleFormat::UInt32;
        case 64: return SampleFormat::UInt64;
        case -32: return SampleFormat::Float32;
        case -64: return SampleFormat::Float64;
        default: throw Error("unsupported BITPIX " + std::to_string(bitpix));
    }
}

// True if the mapping depends on whether a sample is negative: signed integers without
// scaling become unsigned ones if none is.
bool mappingAsksForSign(const FitsImage& img) {
    if (img.bscale != 1 || img.bzero != 0) return false;
    return img.bitpix == 16 || img.bitpix == 32 || img.bitpix == 64;
}

FitsMapping fitsMapping(const FitsImage& img, bool anyNegative) {
    FitsMapping m;
    m.bitpix = img.bitpix;
    m.bscale = img.bscale;
    m.bzero = img.bzero;
    m.raw = rawFormatOf(img.bitpix);
    const bool plain = img.bscale == 1 && img.bzero == 0;
    auto offsetIs = [&](double z) { return img.bscale == 1 && img.bzero == z; };
    using Op = FitsMapping::Op;
    switch (img.bitpix) {
        case 8:
            if (plain) {
                m.format = SampleFormat::UInt8;
                m.note = "8-bit unsigned";
            } else {
                m.op = Op::ToFloat;
                m.format = SampleFormat::Float32;
                m.note = "8-bit with BSCALE/BZERO -> Float32";
            }
            break;
        case 16:
            if (offsetIs(32768)) {
                m.op = Op::FlipSign;
                m.format = SampleFormat::UInt16;
                m.note = "16-bit unsigned (BZERO=32768)";
            } else if (plain && !anyNegative) {
                m.format = SampleFormat::UInt16;
                m.note = "16-bit signed, no negative values -> UInt16";
            } else {
                m.op = Op::ToFloat;
                m.format = SampleFormat::Float32;
                m.note = plain ? "16-bit signed with negative values -> Float32" : "16-bit with BSCALE/BZERO -> Float32";
            }
            break;
        case 32:
            if (offsetIs(2147483648.0)) {
                m.op = Op::FlipSign;
                m.format = SampleFormat::UInt32;
                m.note = "32-bit unsigned (BZERO=2^31)";
            } else if (plain && !anyNegative) {
                m.format = SampleFormat::UInt32;
                m.note = "32-bit signed, no negative values -> UInt32";
            } else {
                m.op = Op::ToFloat;
                m.format = SampleFormat::Float64;
                m.note = plain ? "32-bit signed with negative values -> Float64" : "32-bit with BSCALE/BZERO -> Float64";
            }
            break;
        case 64:
            if (offsetIs(9223372036854775808.0)) {
                m.op = Op::FlipSign;
                m.format = SampleFormat::UInt64;
                m.note = "64-bit unsigned (BZERO=2^63)";
            } else if (plain && !anyNegative) {
                m.format = SampleFormat::UInt64;
                m.note = "64-bit signed, no negative values -> UInt64";
            } else {
                m.op = Op::ToFloat;
                m.format = SampleFormat::Float64;
                m.note = "64-bit signed -> Float64 (values beyond 2^53 lose precision)";
            }
            break;
        case -32:
            if (!plain) m.op = Op::ToFloat;
            m.format = SampleFormat::Float32;
            m.note = "32-bit float";
            break;
        case -64:
            if (!plain) m.op = Op::ToFloat;
            m.format = SampleFormat::Float64;
            m.note = "64-bit float";
            break;
        default:
            throw Error("unsupported BITPIX " + std::to_string(img.bitpix));
    }
    return m;
}

// Samples in host order (as integers of their size, or floats), with BSCALE/BZERO applied.
template <class S, class D>
void toFloat(const uint8_t* in, uint8_t* out, size_t n, double bscale, double bzero) {
    for (size_t i = 0; i < n; ++i) {
        S v;
        std::memcpy(&v, in + i * sizeof(S), sizeof(S));
        const D d = static_cast<D>(bzero + bscale * static_cast<double>(v));
        std::memcpy(out + i * sizeof(D), &d, sizeof(D));
    }
}

template <class T>
void flipSign(const uint8_t* in, uint8_t* out, size_t n) {
    constexpr T sign = static_cast<T>(T(1) << (8 * sizeof(T) - 1));
    for (size_t i = 0; i < n; ++i) {
        T v;
        std::memcpy(&v, in + i * sizeof(T), sizeof(T));
        v ^= sign;
        std::memcpy(out + i * sizeof(T), &v, sizeof(T));
    }
}

// `in` and `out` may be one buffer where the samples keep their size.
void mapFitsSamples(const FitsMapping& m, const uint8_t* in, uint8_t* out, size_t n) {
    using Op = FitsMapping::Op;
    switch (m.op) {
        case Op::Same:
            if (in != out) std::memcpy(out, in, n * sampleBytes(m.raw));
            return;
        case Op::FlipSign:
            if (m.bitpix == 16) flipSign<uint16_t>(in, out, n);
            else if (m.bitpix == 32) flipSign<uint32_t>(in, out, n);
            else flipSign<uint64_t>(in, out, n);
            return;
        case Op::ToFloat:
            switch (m.bitpix) {
                case 8: toFloat<uint8_t, float>(in, out, n, m.bscale, m.bzero); return;
                case 16: toFloat<int16_t, float>(in, out, n, m.bscale, m.bzero); return;
                case 32: toFloat<int32_t, double>(in, out, n, m.bscale, m.bzero); return;
                case 64: toFloat<int64_t, double>(in, out, n, m.bscale, m.bzero); return;
                case -32: toFloat<float, float>(in, out, n, m.bscale, m.bzero); return;
                default: toFloat<double, double>(in, out, n, m.bscale, m.bzero); return;
            }
    }
}

template <class T>
bool anyNegative(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        T v;
        std::memcpy(&v, data + i * sizeof(T), sizeof(T));
        if (v < 0) return true;
    }
    return false;
}

bool anyNegativeSample(int bitpix, const uint8_t* data, size_t n) {
    if (bitpix == 16) return anyNegative<int16_t>(data, n);
    if (bitpix == 32) return anyNegative<int32_t>(data, n);
    return anyNegative<int64_t>(data, n);
}

void decodeSamples(FitsImage& img, std::vector<uint8_t>& raw) {
    PixelBuffer& px = img.pixels;
    const size_t n = static_cast<size_t>(px.samples());
    const size_t sb = static_cast<size_t>(std::abs(img.bitpix)) / 8;
    if (raw.size() != n * sb) throw Error("the image data does not have the size the header gives it");
    if (hostIsLittleEndian()) byteSwapInPlace(raw.data(), n, sb);
    const FitsMapping m = fitsMapping(img, mappingAsksForSign(img) && anyNegativeSample(img.bitpix, raw.data(), n));
    if (sampleBytes(m.format) == sb) {
        mapFitsSamples(m, raw.data(), raw.data(), n);
        px.data.swap(raw);
    } else {
        std::vector<uint8_t> out(n * sampleBytes(m.format));
        mapFitsSamples(m, raw.data(), out.data(), n);
        px.data.swap(out);
    }
    px.format = m.format;
    img.note = m.note;
    updateFloatRange(img);
}

// The same for an image that is read a piece at a time: `bytes` holds its stored samples
// (big-endian). Whether a sample is negative and the range of floating point samples are
// found by reading them once each.
void decodeSamples(FitsImage& img, std::shared_ptr<RandomBytes> bytes) {
    PixelBuffer& px = img.pixels;
    const SampleFormat raw = rawFormatOf(img.bitpix);
    StoredLayout layout;
    layout.swap = hostIsLittleEndian() && sampleBytes(raw) > 1;
    if (bytes->size() != checkedMul(px.samples(), sampleBytes(raw), "image size")) {
        throw Error("the image data does not have the size the header gives it");
    }
    Source stored = storedSource(std::move(bytes), px.width, px.height, px.channels, raw, layout);
    bool negative = false;
    if (mappingAsksForSign(img)) {
        const uint64_t rows = rowsPerPiece(stored->rowBytes());
        std::vector<uint8_t> band(static_cast<size_t>(std::min(rows, px.height) * stored->rowBytes()));
        for (uint64_t c = 0; c < px.channels && !negative; ++c) {
            for (uint64_t y = 0; y < px.height && !negative; y += rows) {
                const uint64_t count = std::min(rows, px.height - y);
                stored->readRows(c, y, count, band.data());
                negative = anyNegativeSample(img.bitpix, band.data(), static_cast<size_t>(count * px.width));
                progressTick(count * stored->rowBytes());
            }
        }
    }
    const FitsMapping m = fitsMapping(img, negative);
    px.format = m.format;
    img.note = m.note;
    img.pieces = m.op == FitsMapping::Op::Same
                     ? stored
                     : mappedSource(stored, m.format, [m](uint64_t, const uint8_t* in, uint8_t* out, size_t n) { mapFitsSamples(m, in, out, n); });
    const FloatRange range = floatRange(*img.pieces);
    img.dataMin = range.min;
    img.dataMax = range.max;
    img.hasNaN = img.hasNaN || range.hasNaN;
}

uint64_t padded(uint64_t bytes) { return (bytes + kBlock - 1) / kBlock * kBlock; }

}  // namespace

void updateFloatRange(FitsImage& img) {
    if (!isFloat(img.pixels.format)) return;
    const FloatRange r = img.pieces ? floatRange(*img.pieces) : floatRange(*borrowedSource(img.pixels));
    img.dataMin = r.min;
    img.dataMax = r.max;
    if (r.hasNaN) img.hasNaN = true;
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

// The columns of a binary table, from TFIELDS, TTYPEn and TFORMn.
std::vector<TileColumn> tableColumns(const Header& hdr, const std::string& label, uint64_t rowBytes) {
    std::vector<TileColumn> columns;
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
        columns.push_back(std::move(c));
    }
    if (offset != rowBytes) throw Error(label + ": the columns do not add up to the row length of the table");
    return columns;
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
    t.columns = tableColumns(hdr, label, rowBytes);
    return t;
}

// The XISF properties of a table written by the FITS writer (kPropertyTable in fits.hpp). `table`
// is its data unit. A property that is damaged is left out with a warning; a table that cannot
// be read at all throws.
std::vector<Property> propertiesFromTable(const Header& hdr, const std::string& label, uint64_t rowBytes, uint64_t rows,
                                          const std::vector<uint8_t>& table, uint64_t& budget) {
    const std::vector<TileColumn> columns = tableColumns(hdr, label, rowBytes);
    auto find = [&](const char* name) -> const TileColumn* {
        for (const auto& c : columns)
            if (c.name == name) return &c;
        return nullptr;
    };
    const TileColumn *id = find("ID"), *type = find("TYPE"), *comment = find("COMMENT"), *format = find("FORMAT"),
                     *nrows = find("ROWS"), *ncolumns = find("COLUMNS"), *value = find("VALUE"), *binary = find("BLOCK");
    auto text = [](const TileColumn* c) { return c && c->type == 'A' && !c->variable; };
    auto number = [](const TileColumn* c) { return c && c->type == 'K' && !c->variable && c->repeat == 1; };
    auto bytes = [](const TileColumn* c) { return c && c->variable && c->type == 'B' && c->repeat == 1; };
    if (!text(id) || !text(type) || !bytes(value) || (comment && !bytes(comment)) || (format && !bytes(format)) ||
        (nrows && !number(nrows)) || (ncolumns && !number(ncolumns)) ||
        (binary && (binary->type != 'L' || binary->variable || binary->repeat != 1))) {
        throw Error("its columns are not those of a table of XISF properties");
    }
    uint64_t heapOffset = checkedMul(rowBytes, rows, "table size");
    long long theap = 0;
    if (hdr.getInt("THEAP", theap)) {
        if (theap < 0 || static_cast<uint64_t>(theap) < heapOffset) throw Error("invalid THEAP");
        heapOffset = static_cast<uint64_t>(theap);
    }
    if (heapOffset > table.size()) throw Error("the table is truncated");
    const uint64_t heapSize = table.size() - heapOffset;

    std::vector<Property> properties;
    for (uint64_t r = 0; r < rows; ++r) {
        const uint8_t* row = table.data() + r * rowBytes;
        auto field = [&](const TileColumn* c) {
            // text ends at a NUL, or is padded with blanks (if another program rewrote the table)
            std::string s(reinterpret_cast<const char*>(row + c->offset), static_cast<size_t>(c->repeat));
            const size_t nul = s.find('\0');
            if (nul != std::string::npos) s.resize(nul);
            else while (!s.empty() && s.back() == ' ') s.pop_back();
            return s;
        };
        auto be = [&](const uint8_t* p, size_t n) {
            uint64_t v = 0;
            for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
            return v;
        };
        // the bytes a variable-length column has in this row; false if they are not in the heap
        auto heap = [&](const TileColumn* c, const uint8_t*& at, size_t& size) {
            at = table.data();
            size = 0;
            if (!c) return true;
            const size_t width = c->wide ? 8 : 4;
            const uint64_t n = be(row + c->offset, width), offset = be(row + c->offset + width, width);
            if (offset > heapSize || n > heapSize - offset) return false;
            at = table.data() + heapOffset + offset;
            size = static_cast<size_t>(n);
            return true;
        };
        Property p;
        p.id = field(id);
        p.type = field(type);
        p.rows = nrows ? be(row + nrows->offset, 8) : 0;
        p.columns = ncolumns ? be(row + ncolumns->offset, 8) : 0;
        if (p.id.empty()) {
            warn(label + ": a property without an id is left out");
            continue;
        }
        const uint8_t *valueAt = nullptr, *commentAt = nullptr, *formatAt = nullptr;
        size_t valueSize = 0, commentSize = 0, formatSize = 0;
        if (!heap(value, valueAt, valueSize) || !heap(comment, commentAt, commentSize) || !heap(format, formatAt, formatSize)) {
            warn(label + ": property " + p.id + " lies beyond the end of the table; it is left out");
            continue;
        }
        // (rows that all point at the same bytes of the heap would hold more than the file does)
        const uint64_t bytes3 = static_cast<uint64_t>(valueSize) + commentSize + formatSize;
        if (bytes3 > budget) {
            warn(label + ": property " + p.id + " is left out: the properties of this file hold more data than a file of its size can");
            continue;
        }
        budget -= bytes3;
        p.comment.assign(reinterpret_cast<const char*>(commentAt), commentSize);
        p.format.assign(reinterpret_cast<const char*>(formatAt), formatSize);
        PropertyElement element;
        const bool known = propertyElement(p.type, element);
        const bool inBlock = binary ? row[binary->offset] == 'T' : known || p.rows != 0 || p.columns != 0;
        if (p.type == "String") {
            p.block = inBlock;
        } else {
            p.array = inBlock;
            if (known && !p.array) {
                warn(label + ": property " + p.id + " is left out: a " + p.type + " whose value is said to be text");
                continue;
            }
        }
        if (p.array) p.data.assign(valueAt, valueAt + valueSize);
        else p.text.assign(reinterpret_cast<const char*>(valueAt), valueSize);
        const std::string problem = propertyProblem(p);
        if (!problem.empty()) {
            warn(label + ": property " + p.id + " is left out: " + problem);
            continue;
        }
        properties.push_back(std::move(p));
    }
    return properties;
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
        progressTick(n);
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
    if (!in) failToOpen(path);
    std::shared_ptr<RawFile> rawFile;   // the file again, for tiles read one at a time
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
                if (!empty) {
                    // (decompressed a tile at a time, and forgotten)
                    if (!rawFile) rawFile = RawFile::openForReading(path);
                    FileBytes unit(rawFile, dataPos, dataBytes, label + ": read error in image data");
                    decodeTiledImage(tile, unit, [](uint64_t, const uint8_t*, size_t) {});
                }
            } catch (const Unsupported& e) {
                report.notChecked.push_back(label + ": " + e.what());
            } catch (const Error& e) {
                if (e.kind == ErrorKind::Cancelled) throw;   // (stopped within the image: no finding)
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

FitsFile readFits(const std::string& path, bool headersOnly, std::optional<size_t> onlyImage, bool inPieces) {
    FitsFile file;
    file.path = path;
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in) failToOpen(path);
    in.seekg(0, std::ios::end);
    file.fileSize = static_cast<uint64_t>(in.tellg());

    uint64_t pos = 0;
    std::shared_ptr<RawFile> rawFile;   // the file again, for the images that are read in pieces
    // The image the HDU before the current one became, if it became one: a table of XISF
    // properties belongs to it.
    std::optional<size_t> imageBefore;
    uint64_t budget = propertyBudget(file.fileSize);   // what the XISF properties of the file may hold together
    for (size_t hduIndex = 0;; ++hduIndex) {
        Header hdr;
        if (!readHeader(in, file.fileSize, pos, hdr, hduIndex == 0)) break;
        const std::optional<size_t> owner = imageBefore;
        imageBefore.reset();

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
            const std::string name = hdr.getString("EXTNAME");
            if (xtension == "BINTABLE" && (name == kPropertyTable || name == kMetadataTable)) {
                // XISF properties, written by a conversion from XISF: of the image before, or of the file
                // (not when the pixels of one image are asked for: who asks has read the headers before)
                const bool ofImage = name == kPropertyTable;
                if (onlyImage) continue;
                try {
                    if (dims.size() != 2) throw Error("invalid binary table");
                    if (ofImage && !owner) throw Error("there is no image before it that it could belong to");
                    std::vector<Property> properties = propertiesFromTable(hdr, label, dims[0], dims[1], readBytes(in, dataPos, dataBytes, label), budget);
                    if (ofImage) {
                        file.images[*owner].properties = std::move(properties);
                        file.images[*owner].wcsDigest = hdr.getString(kWcsDigestKeyword);
                    } else {
                        file.properties = std::move(properties);
                    }
                } catch (const Error& e) {
                    warn(label + ": the table of XISF properties is not used: " + e.what());
                }
                continue;
            }
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

        if (!headersOnly && (!onlyImage || *onlyImage == file.images.size()) && inPieces) {
            // The samples are read from the file where they are, as they are asked for; those of a
            // tile-compressed image are decompressed first (into memory, or a temporary file).
            if (!rawFile) rawFile = RawFile::openForReading(path);
            std::shared_ptr<RandomBytes> unit = std::make_shared<FileBytes>(rawFile, dataPos, dataBytes, label + ": read error in image data");
            if (tiled) {
                try {
                    uint64_t total = elements;
                    total = checkedMul(total, static_cast<uint64_t>(std::abs(tile.bitpix)) / 8, "image size");
                    std::shared_ptr<Store> decoded;
                    decodeTiledImage(tile, *unit, [&](uint64_t at, const uint8_t* data, size_t n) {
                        if (!decoded) decoded = std::make_shared<Store>(total);   // (once the header has passed the checks)
                        decoded->write(at, data, n);
                    });
                    if (!decoded) decoded = std::make_shared<Store>(total);
                    unit = decoded;
                } catch (const Unsupported& e) {
                    if (onlyImage) throw Unsupported(label + ": tile-compressed image: " + e.what());
                    file.skipped.push_back(label + ": tile-compressed image: " + e.what());
                    continue;
                } catch (const Error& e) {
                    // (reading the file, a temporary file, and a request to stop are not about the image)
                    if (e.kind == ErrorKind::Io || e.kind == ErrorKind::Cancelled) throw;
                    throw Error(label + ": tile-compressed image: " + e.what(), e.kind);
                }
            }
            decodeSamples(img, unit);
            if (tiled) img.note += ", " + tile.algorithm + " tile compression";
            img.hasData = true;
        } else if (!headersOnly && (!onlyImage || *onlyImage == file.images.size())) {
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
        imageBefore = file.images.size() - 1;
        if (onlyImage && file.images.size() > *onlyImage) break;   // the one image that was asked for is read
    }
    return file;
}

}  // namespace xisfconv
