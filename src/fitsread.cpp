// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "fitsread.hpp"

#include <cmath>
#include <fstream>
#include <limits>

#include "fits.hpp"

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
        if (!in.read(&block[0], static_cast<std::streamsize>(kBlock))) throw Error("read error in FITS header");
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
    std::ifstream in(path, std::ios::binary);
    char buf[9] = {};
    in.read(buf, 9);
    return in.gcount() == 9 && std::string(buf, 9) == "SIMPLE  =";
}

FitsFile readFits(const std::string& path, bool headersOnly) {
    FitsFile file;
    file.path = path;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot open file");
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
        if (!isImage) {
            const Card* z = hdr.find("ZIMAGE");
            if (xtension == "BINTABLE" && z && trim(z->value) == "T") {
                file.skipped.push_back(label + ": tile-compressed image (fpack); decompress it with funpack first");
            } else {
                file.skipped.push_back(label + ": " + (xtension.empty() ? "unknown" : xtension) + " extension (not an image)");
            }
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
        if (img.name.empty()) img.name = hdr.getString("HDUNAME");
        const std::string rowOrder = toUpper(hdr.getString("ROWORDER"));
        if (!rowOrder.empty()) {
            img.hasRowOrder = true;
            img.topDown = rowOrder == "TOP-DOWN";
        }
        if (hdr.find("BLANK") && bitpix > 0) {
            warn(label + ": BLANK (undefined) pixels are kept as ordinary sample values");
        }
        for (const auto& c : hdr.cards) {
            if (isReservedFitsKeyword(c.name) || c.name == "EXTNAME" || c.name == "HDUNAME" || c.name == "CONTINUE") {
                continue;
            }
            img.keywords.push_back({c.name, c.value, c.comment});
        }

        if (!headersOnly) {
            if (dataBytes > std::numeric_limits<size_t>::max()) throw Error("image too large for this platform");
            std::vector<uint8_t> raw(static_cast<size_t>(dataBytes));
            in.clear();
            in.seekg(static_cast<std::streamoff>(dataPos));
            if (!in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(dataBytes))) {
                throw Error(label + ": read error in image data");
            }
            decodeSamples(img, raw);
            img.hasData = true;
        }
        file.images.push_back(std::move(img));
    }
    return file;
}

}  // namespace xisfconv
