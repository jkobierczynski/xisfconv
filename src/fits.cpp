// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "fits.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>

#include "fitstile.hpp"

namespace xisfconv {

namespace {

constexpr size_t kBlock = 2880;

std::string sanitize(const std::string& s) { return fitsSanitize(s); }

std::string padRight(std::string s, size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    return s;
}

std::string padLeft(const std::string& s, size_t width) {
    return s.size() >= width ? s : std::string(width - s.size(), ' ') + s;
}

std::string finishCard(std::string card) {
    if (card.size() > 80) card.resize(80);
    return padRight(card, 80);
}

std::string valueCard(const std::string& name, const std::string& valueField, const std::string& comment) {
    std::string card = padRight(name, 8) + "= " + valueField;
    if (!comment.empty() && card.size() + 3 < 80) card += " / " + comment;
    return finishCard(card);
}

std::string intCard(const std::string& name, long long v, const std::string& comment) {
    return valueCard(name, padLeft(std::to_string(v), 20), comment);
}

bool isValidFitsName(const std::string& name) { return isStandardFitsName(name); }

std::string unescapeFitsString(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        out += s[i];
        if (s[i] == '\'' && i + 1 < s.size() && s[i + 1] == '\'') ++i;
    }
    return out;
}

void commentaryCards(const std::string& name, const std::string& text, std::vector<std::string>& cards) {
    const std::string key = padRight(name, 8);
    if (text.empty()) {
        cards.push_back(finishCard(key));
        return;
    }
    for (size_t off = 0; off < text.size(); off += 72) cards.push_back(finishCard(key + text.substr(off, 72)));
}

// A real number as a FITS card may hold it, starting at `i`: 12, -1.5, .5, 1.5E+03, 1e-7, 2.D5.
// Returns the place behind it, or npos.
size_t realNumberEnd(const std::string& s, size_t i) {
    auto digits = [&] {
        const size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        return i - start;
    };
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    const size_t whole = digits();
    size_t fraction = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        fraction = digits();
    }
    if (whole + fraction == 0) return std::string::npos;
    if (i < s.size() && (s[i] == 'E' || s[i] == 'e' || s[i] == 'D' || s[i] == 'd')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        if (digits() == 0) return std::string::npos;
    }
    return i;
}

// True for a real number and for a complex one, "(1.0, -2.5E3)".
bool isFitsNumber(const std::string& s) {
    if (realNumberEnd(s, 0) == s.size()) return true;
    if (s.size() < 5 || s.front() != '(' || s.back() != ')') return false;
    auto blanks = [&](size_t i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return i;
    };
    size_t i = realNumberEnd(s, blanks(1));
    if (i == std::string::npos) return false;
    i = blanks(i);
    if (i >= s.size() || s[i] != ',') return false;
    i = realNumberEnd(s, blanks(i + 1));
    if (i == std::string::npos) return false;
    return blanks(i) == s.size() - 1;
}

// Appends the card(s) for one keyword carried over from the XISF header.
void keywordCards(const FitsKeyword& k, std::vector<std::string>& cards) {
    const std::string rawName = sanitize(trim(k.name));
    const std::string name = toUpper(rawName);
    const std::string value = trim(k.value);
    const std::string comment = sanitize(trim(k.comment));

    if (name.empty() || name == "COMMENT" || name == "HISTORY") {
        std::string text = sanitize(value);
        if (!comment.empty()) text = text.empty() ? comment : text + " " + comment;
        commentaryCards(name, text, cards);
        return;
    }

    std::string valueField;
    bool isString = false;
    std::string stringContent;
    if (!value.empty() && value[0] == '\'') {
        isString = true;
        const size_t last = value.rfind('\'');
        std::string inner = last > 0 ? value.substr(1, last - 1) : value.substr(1);
        stringContent = unescapeFitsString(sanitize(inner));
        while (!stringContent.empty() && stringContent.back() == ' ') stringContent.pop_back();
        valueField = fitsString(stringContent);
    } else if (!value.empty()) {
        valueField = sanitize(value);
        // What is a number, a logical value or a complex number goes into the card as it is, with
        // the exponent letter in upper case as FITS wants it (PixInsight writes 1.7870e+04).
        // Anything else is text, and a FITS card holds text in quotes: an XISF keyword may say
        // Ha  where 'Ha' is meant.
        if (valueField == "T" || valueField == "F") {
            valueField = padLeft(valueField, 20);
        } else if (isFitsNumber(valueField)) {
            valueField = toUpper(valueField);
            if (valueField.size() <= 20) valueField = padLeft(valueField, 20);
        } else {
            isString = true;
            stringContent = trim(valueField);
            valueField = fitsString(stringContent);
        }
    }

    const bool hierarch = !isValidFitsName(name);
    if (hierarch && rawName.find('=') != std::string::npos) {
        // the first '=' of a HIERARCH card ends the name
        warn("keyword '" + rawName + "' has a '=' in its name, which a FITS card cannot hold; skipped");
        return;
    }
    const std::string prefix = hierarch ? "HIERARCH " + rawName + " = " : padRight(name, 8) + "= ";
    if (prefix.size() >= 80) {
        warn("keyword '" + rawName + "' is too long for a FITS card; skipped");
        return;
    }
    const size_t room = 80 - prefix.size();
    if (valueField.size() > room) {
        if (!isString) {
            warn("value of keyword '" + rawName + "' does not fit in a FITS card; skipped");
            return;
        }
        if (!hierarch) {
            // Long-string convention: split the value over CONTINUE cards, each piece but the
            // last ending in '&'. A quote counts double because it is written as ''.
            std::vector<std::string> pieces(1);
            size_t used = 0;
            for (char c : stringContent) {
                const size_t w = c == '\'' ? 2 : 1;
                if (used + w > 66) {  // leave room for the '&'
                    pieces.emplace_back();
                    used = 0;
                }
                pieces.back() += c;
                if (c == '\'') pieces.back() += '\'';
                used += w;
            }
            // A text that itself ends in '&' would lose it to readers that take the '&' of the
            // last piece for the mark too (astropy does): an empty piece ends the value then.
            if (!pieces.back().empty() && pieces.back().back() == '&') pieces.emplace_back();
            for (size_t i = 0; i < pieces.size(); ++i) {
                const bool last = i + 1 == pieces.size();
                std::string card = (i == 0 ? prefix : std::string("CONTINUE  ")) + "'" + pieces[i] +
                                   (last ? "" : "&") + "'";
                if (last && !comment.empty() && card.size() + 3 < 80) card += " / " + comment;
                cards.push_back(finishCard(card));
            }
            return;
        }
        // A HIERARCH card has no long-string form: the string is shortened, keeping doubled
        // quotes intact. (It is in free format, so it need not be padded to eight characters.)
        auto quoted = [](const std::string& text) {
            std::string out = "'";
            for (char c : text) {
                out += c;
                if (c == '\'') out += '\'';
            }
            return out + "'";
        };
        std::string s = stringContent;
        while (!s.empty() && quoted(s).size() > room) s.pop_back();
        if (quoted(s).size() > room) {
            warn("value of keyword '" + rawName + "' does not fit in a FITS card; skipped");
            return;
        }
        if (s != stringContent) warn("value of keyword '" + rawName + "' truncated to fit an 80-column FITS card");
        valueField = quoted(s);
    }
    std::string card = prefix + valueField;
    if (!comment.empty() && card.size() + 3 < 80) card += " / " + comment;
    cards.push_back(finishCard(card));
}

int bitpixFor(SampleFormat f) {
    switch (f) {
        case SampleFormat::UInt8: return 8;
        case SampleFormat::UInt16: return 16;
        case SampleFormat::UInt32: return 32;
        case SampleFormat::UInt64: return 64;
        case SampleFormat::Float32: return -32;
        case SampleFormat::Float64: return -64;
    }
    return 8;
}

template <class T>
void storeBE(uint8_t* p, T v) {
    for (size_t i = 0; i < sizeof(T); ++i) p[i] = static_cast<uint8_t>(v >> (8 * (sizeof(T) - 1 - i)));
}

// The samples [first, first + count) of the buffer as a FITS data unit holds them: big-endian,
// unsigned integers of 16 bits and more as signed ones (the header says BZERO).
void storedSamples(const PixelBuffer& px, size_t first, size_t count, uint8_t* d) {
    const size_t sb = sampleBytes(px.format);
    const uint8_t* s = px.data.data() + first * sb;
    const size_t m = count;
    switch (px.format) {
        case SampleFormat::UInt8:
            std::memcpy(d, s, m);
            break;
        case SampleFormat::UInt16:
            for (size_t i = 0; i < m; ++i) {
                uint16_t v; std::memcpy(&v, s + 2 * i, 2);
                storeBE<uint16_t>(d + 2 * i, static_cast<uint16_t>(v ^ 0x8000u));
            }
            break;
        case SampleFormat::UInt32:
            for (size_t i = 0; i < m; ++i) {
                uint32_t v; std::memcpy(&v, s + 4 * i, 4);
                storeBE<uint32_t>(d + 4 * i, v ^ 0x80000000u);
            }
            break;
        case SampleFormat::UInt64:
            for (size_t i = 0; i < m; ++i) {
                uint64_t v; std::memcpy(&v, s + 8 * i, 8);
                storeBE<uint64_t>(d + 8 * i, v ^ 0x8000000000000000ull);
            }
            break;
        case SampleFormat::Float32:
            for (size_t i = 0; i < m; ++i) {
                uint32_t v; std::memcpy(&v, s + 4 * i, 4);
                storeBE<uint32_t>(d + 4 * i, v);
            }
            break;
        case SampleFormat::Float64:
            for (size_t i = 0; i < m; ++i) {
                uint64_t v; std::memcpy(&v, s + 8 * i, 8);
                storeBE<uint64_t>(d + 8 * i, v);
            }
            break;
    }
}

void writeZeros(std::ofstream& out, uint64_t count) {
    const std::vector<char> zeros(static_cast<size_t>(std::min<uint64_t>(count, 1u << 16)), 0);
    for (uint64_t done = 0; done < count;) {
        const uint64_t n = std::min<uint64_t>(count - done, zeros.size());
        out.write(zeros.data(), static_cast<std::streamsize>(n));
        done += n;
    }
}

// Fills the last block of a data unit of `bytes` bytes.
void padBlock(std::ofstream& out, uint64_t bytes) { writeZeros(out, (kBlock - bytes % kBlock) % kBlock); }

void writeData(std::ofstream& out, const PixelBuffer& px) {
    const size_t sb = sampleBytes(px.format);
    const size_t n = static_cast<size_t>(px.samples());
    const size_t chunk = size_t(1) << 20;
    std::vector<uint8_t> buf(std::min(n, chunk) * sb);
    for (size_t off = 0; off < n; off += chunk) {
        const size_t m = std::min(chunk, n - off);
        storedSamples(px, off, m, buf.data());
        out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(m * sb));
    }
    padBlock(out, static_cast<uint64_t>(n) * sb);
}

}  // namespace

std::string fitsSanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 0x80 && c < 0xC0) continue;  // UTF-8 continuation byte
        if (c >= 0xC0) out += '?';
        else if (c < 32 || c == 127) out += ' ';
        else out += static_cast<char>(c);
    }
    return out;
}

bool isStandardFitsName(const std::string& name) {
    if (name.empty() || name.size() > 8) return false;
    for (char c : name) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

std::string fitsString(const std::string& s) {
    std::string escaped;
    for (char c : s) {
        escaped += c;
        if (c == '\'') escaped += '\'';
    }
    return "'" + padRight(escaped, 8) + "'";
}

std::string fitsReal(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.12G", v);
    const std::string printed = cNumber(buf);
    std::snprintf(buf, sizeof buf, "%s", printed.c_str());
    std::string s = buf;
    if (s.find_first_of(".EN") == std::string::npos) s += ".0";
    return s;
}

bool isReservedFitsKeyword(const std::string& rawName) {
    const std::string n = toUpper(trim(rawName));
    static const char* fixed[] = {"SIMPLE", "BITPIX", "NAXIS", "EXTEND", "BZERO", "BSCALE", "END", "XTENSION",
                                  "PCOUNT", "GCOUNT", "ROWORDER", "BLANK", "CHECKSUM", "DATASUM"};
    for (const char* f : fixed)
        if (n == f) return true;
    if (startsWith(n, "NAXIS")) {
        uint64_t k;
        return parseUInt64(n.substr(5), k);
    }
    return false;
}

namespace {

// The cards of the keywords an image brings along, after those the writer sets itself.
std::vector<std::string> userKeywordCards(const std::vector<FitsKeyword>& keywords, bool skipExtname, bool skipProgram) {
    std::vector<std::string> userCards;
    bool hasLongStrn = false;
    for (const auto& k : keywords) {
        if (isReservedFitsKeyword(k.name)) continue;
        if (skipExtname && toUpper(trim(k.name)) == "EXTNAME") continue;
        if (skipProgram && toUpper(trim(k.name)) == "PROGRAM") continue;
        if (toUpper(trim(k.name)) == "LONGSTRN") hasLongStrn = true;
        keywordCards(k, userCards);
    }
    if (!hasLongStrn) {
        // Announce the long-string convention when CONTINUE cards are present.
        for (const auto& c : userCards) {
            if (c.compare(0, 10, "CONTINUE  ") == 0) {
                userCards.insert(userCards.begin(),
                                 valueCard("LONGSTRN", fitsString("OGIP 1.0"), "The OGIP long string convention may be used"));
                break;
            }
        }
    }
    return userCards;
}

}  // namespace

std::string fitsCards(const std::vector<FitsKeyword>& keywords) {
    std::string text;
    for (const auto& card : userKeywordCards(keywords, false, false)) text += card;
    return text;
}

namespace {

std::string logicalCard(const std::string& name, bool v, const std::string& comment) {
    return valueCard(name, padLeft(v ? "T" : "F", 20), comment);
}

// The cards that say how the samples of the buffer relate to the integers that are stored.
void scalingCards(SampleFormat format, std::vector<std::string>& cards) {
    switch (format) {
        case SampleFormat::UInt16:
            cards.push_back(intCard("BZERO", 32768, "offset for unsigned 16-bit data"));
            cards.push_back(intCard("BSCALE", 1, "default scaling factor"));
            break;
        case SampleFormat::UInt32:
            cards.push_back(intCard("BZERO", 2147483648LL, "offset for unsigned 32-bit data"));
            cards.push_back(intCard("BSCALE", 1, "default scaling factor"));
            break;
        case SampleFormat::UInt64:
            cards.push_back(valueCard("BZERO", padLeft("9223372036854775808", 20), "offset for unsigned 64-bit data"));
            cards.push_back(intCard("BSCALE", 1, "default scaling factor"));
            break;
        default:
            break;
    }
}

// The cards every image gets after those that describe its storage: who wrote it, its name,
// the order of its rows, and the keywords it brought along. `tableName`: the EXTNAME of an
// image without a name, if it needs one.
void imageCards(const FitsHdu& hdu, const std::vector<FitsKeyword>& keywords, bool first, const char* tableName,
                std::vector<std::string>& cards) {
    if (first) cards.push_back(valueCard("PROGRAM", fitsString(std::string("xisfconv ") + kVersion), "software that created this HDU"));
    bool named = !hdu.extname.empty();
    if (named) {
        // The name has to fit one card with its quotes (a quote inside it counts double).
        std::string name = sanitize(hdu.extname);
        const std::string whole = name;
        while (fitsString(name).size() > 70) name.pop_back();
        if (name != whole) warn("image name '" + whole + "' shortened to fit a FITS card");
        cards.push_back(valueCard("EXTNAME", fitsString(name), "image identifier"));
    }
    for (const auto& k : keywords)
        if (toUpper(trim(k.name)) == "EXTNAME") named = true;
    if (!named && tableName) cards.push_back(valueCard("EXTNAME", fitsString(tableName), "name of this binary table extension"));
    cards.push_back(valueCard("ROWORDER", fitsString(hdu.bottomUp ? "BOTTOM-UP" : "TOP-DOWN"), "order of image rows"));
    const std::vector<std::string> userCards = userKeywordCards(keywords, !hdu.extname.empty(), first);
    cards.insert(cards.end(), userCards.begin(), userCards.end());
}

std::string headerBlocks(std::vector<std::string> cards) {
    cards.push_back(finishCard("END"));
    std::string header;
    for (const auto& c : cards) header += c;
    header.append((kBlock - header.size() % kBlock) % kBlock, ' ');
    return header;
}

void writePlainImage(std::ofstream& out, const FitsHdu& hdu, bool primary, bool first) {
    const PixelBuffer& px = *hdu.pixels;
    std::vector<std::string> cards;
    if (primary) cards.push_back(logicalCard("SIMPLE", true, "file conforms to FITS standard"));
    else cards.push_back(valueCard("XTENSION", fitsString("IMAGE"), "image extension"));
    cards.push_back(intCard("BITPIX", bitpixFor(px.format), "bits per data value"));
    const int naxis = px.channels > 1 ? 3 : 2;
    cards.push_back(intCard("NAXIS", naxis, "number of data axes"));
    cards.push_back(intCard("NAXIS1", static_cast<long long>(px.width), "image width"));
    cards.push_back(intCard("NAXIS2", static_cast<long long>(px.height), "image height"));
    if (naxis == 3) cards.push_back(intCard("NAXIS3", static_cast<long long>(px.channels), "number of channels"));
    if (primary) {
        cards.push_back(logicalCard("EXTEND", true, "file may contain extensions"));
    } else {
        cards.push_back(intCard("PCOUNT", 0, "no group parameters"));
        cards.push_back(intCard("GCOUNT", 1, "one data group"));
    }
    scalingCards(px.format, cards);
    imageCards(hdu, hdu.keywords, first, nullptr, cards);
    const std::string header = headerBlocks(std::move(cards));
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    writeData(out, px);
}

// An image as the tiled image compression convention stores it: a binary table with one row
// per tile, each a descriptor (size and position) of the compressed tile in the heap behind the
// table. A tile is one row of the image, as in the files of fpack and astropy. The table and the
// sizes in the header are known only when the tiles are compressed: both are written last, into
// the room left for them.
void writeTiledImage(std::ofstream& out, const std::string& path, const FitsHdu& hdu, bool first, FitsTiles tiles) {
    const PixelBuffer& px = *hdu.pixels;
    const size_t sb = sampleBytes(px.format);
    const bool rice = tiles == FitsTiles::Default && !isFloat(px.format);
    const char* algorithm = rice ? "RICE_1" : sb == 1 ? "GZIP_1" : "GZIP_2";
    const uint64_t ntiles = checkedMul(px.height, px.channels, "number of tiles");
    const uint64_t tileSamples = px.width;
    if (tileSamples > std::numeric_limits<size_t>::max() / 16) throw Error("image too large for this platform");

    // Descriptors of 32 bits where the heap is sure to stay below 2 GiB, of 64 bits otherwise.
    const uint64_t tileLimit = rice ? riceBound(tileSamples, static_cast<int>(sb)) : TileGzip::bound(tileSamples * sb);
    const uint64_t narrowLimit = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    const bool wide = tileLimit > narrowLimit || ntiles > narrowLimit / tileLimit;
    const size_t rowBytes = wide ? 16 : 8;
    const uint64_t tableBytes = checkedMul(ntiles, rowBytes, "table size");
    if (tableBytes > std::numeric_limits<size_t>::max() / 2) throw Error("image too large for this platform");

    // The cards of the image itself. Keywords that describe a compressed image and its table
    // (an image read from a tile-compressed file has none, but a caller may hand some over)
    // would contradict those written here, or be taken for them by a reader: they are left out.
    std::vector<FitsKeyword> keywords;
    for (const auto& k : hdu.keywords) {
        const std::string name = toUpper(trim(k.name));
        if (isTileKeyword(name)) warn("keyword " + name + " describes a tile-compressed image and is set by the writer; the one given is left out");
        else keywords.push_back(k);
    }
    std::vector<std::string> ownCards;
    imageCards(hdu, keywords, first, "COMPRESSED_IMAGE", ownCards);

    const int naxis = px.channels > 1 ? 3 : 2;
    auto header = [&](uint64_t heapBytes, uint64_t longestTile) {
        std::vector<std::string> cards;
        cards.push_back(valueCard("XTENSION", fitsString("BINTABLE"), "binary table extension"));
        cards.push_back(intCard("BITPIX", 8, "array data type"));
        cards.push_back(intCard("NAXIS", 2, "number of array dimensions"));
        cards.push_back(intCard("NAXIS1", static_cast<long long>(rowBytes), "width of table in bytes"));
        cards.push_back(intCard("NAXIS2", static_cast<long long>(ntiles), "number of rows in table: the tiles"));
        cards.push_back(intCard("PCOUNT", static_cast<long long>(heapBytes), "size of the heap: the compressed tiles"));
        cards.push_back(intCard("GCOUNT", 1, "one data group"));
        cards.push_back(intCard("TFIELDS", 1, "number of fields in each row"));
        cards.push_back(valueCard("TTYPE1", fitsString("COMPRESSED_DATA"), "label for field 1"));
        cards.push_back(valueCard("TFORM1", fitsString(std::string(wide ? "1QB(" : "1PB(") + std::to_string(longestTile) + ")"),
                                  "variable-length array of bytes"));
        cards.push_back(logicalCard("ZIMAGE", true, "extension contains a compressed image"));
        if (first) {
            cards.push_back(logicalCard("ZSIMPLE", true, "file conforms to FITS standard"));
        } else {
            cards.push_back(valueCard("ZTENSION", fitsString("IMAGE"), "image extension"));
        }
        // (with the comments of the plain image, which funpack takes over when it restores it)
        cards.push_back(intCard("ZBITPIX", bitpixFor(px.format), "bits per data value"));
        cards.push_back(intCard("ZNAXIS", naxis, "number of data axes"));
        cards.push_back(intCard("ZNAXIS1", static_cast<long long>(px.width), "image width"));
        cards.push_back(intCard("ZNAXIS2", static_cast<long long>(px.height), "image height"));
        if (naxis == 3) cards.push_back(intCard("ZNAXIS3", static_cast<long long>(px.channels), "number of channels"));
        if (first) {
            cards.push_back(logicalCard("ZEXTEND", true, "file may contain extensions"));
        } else {
            cards.push_back(intCard("ZPCOUNT", 0, "no group parameters"));
            cards.push_back(intCard("ZGCOUNT", 1, "one data group"));
        }
        cards.push_back(intCard("ZTILE1", static_cast<long long>(px.width), "width of a tile"));
        cards.push_back(intCard("ZTILE2", 1, "height of a tile: one row"));
        if (naxis == 3) cards.push_back(intCard("ZTILE3", 1, "a tile stays within a channel"));
        cards.push_back(valueCard("ZCMPTYPE", fitsString(algorithm), "compression algorithm"));
        if (rice) {
            cards.push_back(valueCard("ZNAME1", fitsString("BLOCKSIZE"), "compression block size"));
            cards.push_back(intCard("ZVAL1", kRiceBlockSize, "pixels per block"));
            cards.push_back(valueCard("ZNAME2", fitsString("BYTEPIX"), "bytes per pixel (1, 2, 4, or 8)"));
            cards.push_back(intCard("ZVAL2", static_cast<long long>(sb), "bytes per pixel (1, 2, 4, or 8)"));
        }
        if (isFloat(px.format)) cards.push_back(valueCard("ZQUANTIZ", fitsString("NONE"), "lossless: the pixels are not quantized"));
        scalingCards(px.format, cards);
        cards.insert(cards.end(), ownCards.begin(), ownCards.end());
        return headerBlocks(std::move(cards));
    };

    const std::ofstream::pos_type headerAt = out.tellp();
    const std::string placeholder = header(0, 0);
    out.write(placeholder.data(), static_cast<std::streamsize>(placeholder.size()));
    const std::ofstream::pos_type tableAt = out.tellp();
    writeZeros(out, tableBytes);

    std::vector<uint8_t> table(static_cast<size_t>(tableBytes));
    std::vector<uint8_t> tile(static_cast<size_t>(tileSamples) * sb);
    TileGzip gzip;
    uint64_t heap = 0, longest = 0;
    const size_t count = static_cast<size_t>(tileSamples);
    // a sign of life (and a chance to stop) every 8 MiB of pixels or so
    const uint64_t reportEvery = std::max<uint64_t>(1, (uint64_t(8) << 20) / (tileSamples * sb));
    // The rows of the buffer one after the other, channel by channel: the order of the tiles.
    for (uint64_t row = 0; row < ntiles; ++row) {
        if (row % reportEvery == 0) progress("compressing", row, ntiles);
        storedSamples(px, static_cast<size_t>(row) * count, count, tile.data());
        const std::vector<uint8_t> packed = rice ? riceEncode(tile.data(), count, static_cast<int>(sb))
                                                 : gzip.compress(tile.data(), count, sb, true);
        if (!wide && heap + packed.size() > narrowLimit) throw Error("the compressed tiles do not fit the table (internal error)");
        uint8_t* descriptor = table.data() + static_cast<size_t>(row) * rowBytes;
        if (wide) {
            storeBE<uint64_t>(descriptor, packed.size());
            storeBE<uint64_t>(descriptor + 8, heap);
        } else {
            storeBE<uint32_t>(descriptor, static_cast<uint32_t>(packed.size()));
            storeBE<uint32_t>(descriptor + 4, static_cast<uint32_t>(heap));
        }
        out.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
        if (!out) throw Error("write error on " + path, ErrorKind::Io);
        heap += packed.size();
        longest = std::max<uint64_t>(longest, packed.size());
    }
    padBlock(out, tableBytes + heap);
    const std::ofstream::pos_type endAt = out.tellp();

    const std::string finished = header(heap, longest);
    if (finished.size() != placeholder.size()) throw Error("the header of the compressed image changed its size (internal error)");
    out.seekp(headerAt);
    out.write(finished.data(), static_cast<std::streamsize>(finished.size()));
    out.seekp(tableAt);
    out.write(reinterpret_cast<const char*>(table.data()), static_cast<std::streamsize>(table.size()));
    out.seekp(endAt);
}

}  // namespace

void writeFits(const std::string& path, const std::vector<FitsHdu>& hdus, const FitsWriteOptions& options) {
    std::ofstream out(toPath(path), std::ios::binary | std::ios::trunc);
    if (!out) throw Error("cannot create " + path, ErrorKind::Io);

    if (options.tiles == FitsTiles::None) {
        for (size_t h = 0; h < hdus.size(); ++h) {
            writePlainImage(out, hdus[h], h == 0, h == 0);
            if (!out) throw Error("write error on " + path, ErrorKind::Io);
        }
    } else {
        // Two kinds of image stay as they are: 64-bit integers, which CFITSIO (and with it most
        // programs) neither writes nor reads tile-compressed, and an image without pixels.
        auto compressed = [](const FitsHdu& hdu) {
            return hdu.pixels->samples() != 0 && hdu.pixels->format != SampleFormat::UInt64;
        };
        for (const FitsHdu& hdu : hdus) {
            if (hdu.pixels->format == SampleFormat::UInt64) {
                warn("64-bit integer images are not tile-compressed (CFITSIO does not read them): " +
                     (hdu.extname.empty() ? std::string("the image") : "image '" + hdu.extname + "'") + " is stored as it is");
            }
        }
        // A compressed image is a table, and a table cannot be the primary HDU: that one is
        // empty then, and the first image says that it belongs there (ZSIMPLE).
        const bool emptyPrimary = compressed(hdus.front());
        if (emptyPrimary) {
            std::vector<std::string> cards;
            cards.push_back(logicalCard("SIMPLE", true, "file conforms to FITS standard"));
            cards.push_back(intCard("BITPIX", 8, "bits per data value"));
            cards.push_back(intCard("NAXIS", 0, "no data: the images are in the extensions"));
            cards.push_back(logicalCard("EXTEND", true, "file may contain extensions"));
            const std::string primary = headerBlocks(std::move(cards));
            out.write(primary.data(), static_cast<std::streamsize>(primary.size()));
        }
        for (size_t h = 0; h < hdus.size(); ++h) {
            if (compressed(hdus[h])) writeTiledImage(out, path, hdus[h], h == 0, options.tiles);
            else writePlainImage(out, hdus[h], h == 0, h == 0);
            if (!out) throw Error("write error on " + path, ErrorKind::Io);
        }
    }
    out.close();
    if (!out) throw Error("write error on " + path, ErrorKind::Io);
}

}  // namespace xisfconv
