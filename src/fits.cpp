// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "fits.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>

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
        // FITS requires an upper-case exponent letter; PixInsight writes e.g. 1.7870e+04.
        static const std::regex lowerExp(R"(^[+-]?(\d+\.?\d*|\.\d+)e[+-]?\d+$)");
        if (std::regex_match(valueField, lowerExp)) valueField = toUpper(valueField);
        if (valueField.size() <= 20) valueField = padLeft(valueField, 20);
    }

    const bool hierarch = !isValidFitsName(name);
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
            for (size_t i = 0; i < pieces.size(); ++i) {
                const bool last = i + 1 == pieces.size();
                std::string card = (i == 0 ? prefix : std::string("CONTINUE  ")) + "'" + pieces[i] +
                                   (last ? "" : "&") + "'";
                if (last && !comment.empty() && card.size() + 3 < 80) card += " / " + comment;
                cards.push_back(finishCard(card));
            }
            return;
        }
        // Shorten the string, keeping doubled quotes intact.
        std::string s = stringContent;
        while (!s.empty() && fitsString(s).size() > room) s.pop_back();
        warn("value of keyword '" + rawName + "' truncated to fit an 80-column FITS card");
        valueField = fitsString(s);
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

void writeData(std::ofstream& out, const PixelBuffer& px) {
    const size_t sb = sampleBytes(px.format);
    const size_t n = static_cast<size_t>(px.samples());
    const size_t chunk = size_t(1) << 20;
    std::vector<uint8_t> buf(std::min(n, chunk) * sb);
    const uint8_t* src = px.data.data();
    for (size_t off = 0; off < n; off += chunk) {
        const size_t m = std::min(chunk, n - off);
        const uint8_t* s = src + off * sb;
        uint8_t* d = buf.data();
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
        out.write(reinterpret_cast<const char*>(d), static_cast<std::streamsize>(m * sb));
    }
    const size_t bytes = n * sb;
    const size_t pad = (kBlock - bytes % kBlock) % kBlock;
    if (pad) {
        const std::vector<char> zeros(pad, 0);
        out.write(zeros.data(), static_cast<std::streamsize>(pad));
    }
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

void writeFits(const std::string& path, const std::vector<FitsHdu>& hdus) {
    std::ofstream out(toPath(path), std::ios::binary | std::ios::trunc);
    if (!out) throw Error("cannot create " + path, ErrorKind::Io);

    for (size_t h = 0; h < hdus.size(); ++h) {
        const FitsHdu& hdu = hdus[h];
        const PixelBuffer& px = *hdu.pixels;
        std::vector<std::string> cards;
        if (h == 0) cards.push_back(valueCard("SIMPLE", padLeft("T", 20), "file conforms to FITS standard"));
        else cards.push_back(valueCard("XTENSION", fitsString("IMAGE"), "image extension"));
        cards.push_back(intCard("BITPIX", bitpixFor(px.format), "bits per data value"));
        const int naxis = px.channels > 1 ? 3 : 2;
        cards.push_back(intCard("NAXIS", naxis, "number of data axes"));
        cards.push_back(intCard("NAXIS1", static_cast<long long>(px.width), "image width"));
        cards.push_back(intCard("NAXIS2", static_cast<long long>(px.height), "image height"));
        if (naxis == 3) cards.push_back(intCard("NAXIS3", static_cast<long long>(px.channels), "number of channels"));
        if (h == 0) {
            cards.push_back(valueCard("EXTEND", padLeft("T", 20), "file may contain extensions"));
        } else {
            cards.push_back(intCard("PCOUNT", 0, "no group parameters"));
            cards.push_back(intCard("GCOUNT", 1, "one data group"));
        }
        switch (px.format) {
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
        if (h == 0) cards.push_back(valueCard("PROGRAM", fitsString(std::string("xisfconv ") + kVersion),
                                              "software that created this HDU"));
        if (!hdu.extname.empty()) cards.push_back(valueCard("EXTNAME", fitsString(sanitize(hdu.extname)), "image identifier"));
        cards.push_back(valueCard("ROWORDER", fitsString(hdu.bottomUp ? "BOTTOM-UP" : "TOP-DOWN"), "order of image rows"));

        std::vector<std::string> userCards;
        bool hasLongStrn = false;
        for (const auto& k : hdu.keywords) {
            if (isReservedFitsKeyword(k.name)) continue;
            if (!hdu.extname.empty() && toUpper(trim(k.name)) == "EXTNAME") continue;
            if (h == 0 && toUpper(trim(k.name)) == "PROGRAM") continue;
            if (toUpper(trim(k.name)) == "LONGSTRN") hasLongStrn = true;
            keywordCards(k, userCards);
        }
        if (!hasLongStrn) {
            // Announce the long-string convention when CONTINUE cards are present.
            for (const auto& c : userCards) {
                if (c.compare(0, 10, "CONTINUE  ") == 0) {
                    cards.push_back(valueCard("LONGSTRN", fitsString("OGIP 1.0"), "The OGIP long string convention may be used"));
                    break;
                }
            }
        }
        cards.insert(cards.end(), userCards.begin(), userCards.end());
        cards.push_back(finishCard("END"));

        std::string header;
        for (const auto& c : cards) header += c;
        header.append((kBlock - header.size() % kBlock) % kBlock, ' ');
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        writeData(out, px);
        if (!out) throw Error("write error on " + path, ErrorKind::Io);
    }
    out.close();
    if (!out) throw Error("write error on " + path, ErrorKind::Io);
}

}  // namespace xisfconv
