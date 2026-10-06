// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "property.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "codecs.hpp"
#include "fitsread.hpp"

namespace xisfconv {

namespace {

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void appendLittleEndian(std::vector<uint8_t>& out, const std::vector<double>& values) {
    out.resize(values.size() * 8);
    if (!values.empty()) std::memcpy(out.data(), values.data(), out.size());
    if (!hostIsLittleEndian()) byteSwapInPlace(out.data(), values.size(), 8);
}

}  // namespace

bool isMatrixPropertyType(const std::string& type) { return endsWith(type, "Matrix"); }

bool propertyElement(const std::string& type, PropertyElement& element) {
    struct Known { const char* name; size_t size; char kind; };
    static const Known known[] = {{"I8", 1, 'i'},   {"UI8", 1, 'u'},  {"Byte", 1, 'u'}, {"I16", 2, 'i'},  {"UI16", 2, 'u'},
                                  {"I32", 4, 'i'},  {"UI32", 4, 'u'}, {"I64", 8, 'i'},  {"UI64", 8, 'u'},
                                  {"F32", 4, 'f'},  {"F64", 8, 'f'},  {"C32", 8, 'c'},  {"C64", 16, 'c'},
                                  // the short names of the specification: IVector is I32Vector, Vector is F64Vector ...
                                  {"I", 4, 'i'},    {"UI", 4, 'u'},   {"F", 4, 'f'},    {"", 8, 'f'}};
    if (type == "ByteArray") {
        element = {1, 'u', false};
        return true;
    }
    const bool matrix = endsWith(type, "Matrix"), vector = endsWith(type, "Vector");
    if (!matrix && !vector) return false;
    const std::string name = type.substr(0, type.size() - 6);
    for (const Known& k : known) {
        if (name == k.name) {
            element = {k.size, k.kind, matrix};
            return true;
        }
    }
    return false;
}

std::string propertyProblem(const Property& p) {
    if (!p.array) return {};
    PropertyElement e;
    if (!propertyElement(p.type, e)) return {};
    uint64_t count = p.rows;
    if (e.matrix) {
        // (A matrix of nothing may have rows or columns, 0 x 4. Its other dimension is then not
        // bounded by any data: 2^64 - 1 rows of no columns is no shape, and no program that is
        // handed it can make an array of it. PixInsight counts rows and columns in 32 bits.)
        if ((p.rows == 0 || p.columns == 0) && (p.rows > INT32_MAX || p.columns > INT32_MAX)) return "its shape is impossible";
        if (p.columns != 0 && p.rows > UINT64_MAX / p.columns) return "its shape is impossible";
        count = p.rows * p.columns;
    }
    if (count > UINT64_MAX / e.size || count * e.size != p.data.size()) {
        return "its data is " + std::to_string(p.data.size()) + " bytes, its shape asks for " +
               (count > UINT64_MAX / e.size ? std::string("more than there can be") : std::to_string(count * e.size));
    }
    return {};
}

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// A whole number of at most `bits` bits, as text: digits, with a sign if `isSigned`.
bool wholeNumberFits(const std::string& s, int bits, bool isSigned) {
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        if (!isSigned && s[i] == '-') return false;
        negative = s[i] == '-';
        ++i;
    }
    if (i >= s.size()) return false;
    uint64_t value = 0;
    for (; i < s.size(); ++i) {
        if (!isDigit(s[i])) return false;
        const uint64_t digit = static_cast<uint64_t>(s[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (!isSigned) return bits == 64 || value <= (uint64_t(1) << bits) - 1;
    const uint64_t most = uint64_t(1) << (bits - 1);   // the largest magnitude, of the most negative number
    return negative ? value <= most : value <= most - 1;
}

// A floating point number as text, and nothing around it: digits with a point or an exponent
// as C writes them, of a size the type holds (`single`: 32 bits), or not a number, or infinity.
// (The syntax is checked here and not left to the library that reads numbers: what that takes
// besides, hexadecimal for one, differs from one to the next.)
bool isRealText(const std::string& s, bool single) {
    const std::string word = toLower(s);
    if (word == "nan" || word == "+nan" || word == "-nan" || word == "inf" || word == "+inf" || word == "-inf" ||
        word == "infinity" || word == "+infinity" || word == "-infinity") {
        return true;
    }
    size_t i = 0, digits = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    for (; i < s.size() && isDigit(s[i]); ++i) ++digits;
    if (i < s.size() && s[i] == '.') {
        for (++i; i < s.size() && isDigit(s[i]); ++i) ++digits;
    }
    if (!digits) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        if (i >= s.size()) return false;
        for (; i < s.size(); ++i)
            if (!isDigit(s[i])) return false;
    }
    if (i != s.size()) return false;
    bool complete = false;
    const double value = strtodC(s, &complete);
    if (!complete || std::isinf(value) || std::isnan(value)) return false;   // (1e400 is no number a double holds)
    // The largest 32-bit number is (2^24 - 1) * 2^104; from half a step above it a number rounds to infinity.
    return !single || std::fabs(value) < std::ldexp(16777215.5, 104);
}

// YYYY-MM-DD, with Thh:mm, Thh:mm:ss or Thh:mm:ss.sss behind it, and then Z or an offset
// from UTC (+hh, +hh:mm). The year may have a sign and more than four digits.
bool isTimePointText(const std::string& s) {
    size_t i = 0;
    auto digitsAt = [&](size_t count) {
        for (size_t k = 0; k < count; ++k)
            if (i + k >= s.size() || !isDigit(s[i + k])) return false;
        i += count;
        return true;
    };
    auto take = [&](char c) {
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    };
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    // two digits that make a number from `lowest` to `highest`
    auto twoDigits = [&](int lowest, int highest) {
        if (!digitsAt(2)) return false;
        const int value = (s[i - 2] - '0') * 10 + (s[i - 1] - '0');
        return value >= lowest && value <= highest;
    };
    const size_t yearAt = i;
    if (!digitsAt(4)) return false;
    while (i < s.size() && isDigit(s[i])) ++i;
    int year = 0;   // modulo 400: what a leap year depends on
    for (size_t k = yearAt; k < i; ++k) year = (year * 10 + (s[k] - '0')) % 400;
    if (!take('-') || !twoDigits(1, 12)) return false;
    const int month = (s[i - 2] - '0') * 10 + (s[i - 1] - '0');
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year == 0);
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (!take('-') || !twoDigits(1, days[month - 1] + (month == 2 && leap ? 1 : 0))) return false;   // (a day the month has)
    if (i == s.size()) return true;
    if (!take('T')) return false;   // (a zone belongs to a time, not to a date)
    if (!twoDigits(0, 23) || !take(':') || !twoDigits(0, 59)) return false;
    if (take(':')) {
        if (!twoDigits(0, 60)) return false;   // (60: a leap second)
        if (take('.')) {
            if (!digitsAt(1)) return false;
            while (i < s.size() && isDigit(s[i])) ++i;
        }
    }
    if (i == s.size()) return true;
    if (take('Z')) return i == s.size();
    if (!take('+') && !take('-')) return false;
    if (!twoDigits(0, 23)) return false;
    if (take(':') && !twoDigits(0, 59)) return false;
    return i == s.size();
}

}  // namespace

std::string scalarPropertyProblem(const std::string& type, const std::string& text) {
    struct Whole { const char* name; int bits; bool isSigned; };
    static const Whole wholes[] = {{"Int8", 8, true},    {"UInt8", 8, false},   {"Byte", 8, false},   {"Int16", 16, true},
                                   {"Short", 16, true},  {"UInt16", 16, false}, {"UShort", 16, false}, {"Int32", 32, true},
                                   {"Int", 32, true},    {"UInt32", 32, false}, {"UInt", 32, false},   {"Int64", 64, true},
                                   {"UInt64", 64, false}};
    if (type == "String") return isValidUtf8(text) ? std::string() : "its text is not UTF-8";
    if (type == "TimePoint") {
        return isTimePointText(text) ? std::string() : "'" + text + "' is not a date and time of ISO 8601 (2026-10-06T18:30:00Z)";
    }
    if (type == "Boolean") {   // (the two words of the specification; 1 and 0 are read, not written)
        return text == "true" || text == "false" ? std::string() : "a Boolean is true or false, not '" + text + "'";
    }
    for (const Whole& w : wholes) {
        if (type != w.name) continue;
        return wholeNumberFits(text, w.bits, w.isSigned) ? std::string() : "'" + text + "' is not a number that " + type + " holds";
    }
    if (type == "Float32" || type == "Float64" || type == "Float" || type == "Double") {
        return isRealText(text, type == "Float32" || type == "Float") ? std::string()
                                                                      : "'" + text + "' is not a number that " + type + " holds";
    }
    if (type == "Complex32" || type == "Complex64") {
        const bool single = type == "Complex32";
        const size_t comma = text.find(',');
        if (text.size() >= 5 && text.front() == '(' && text.back() == ')' && comma != std::string::npos &&
            isRealText(text.substr(1, comma - 1), single) && isRealText(text.substr(comma + 1, text.size() - comma - 2), single)) {
            return {};
        }
        return "'" + text + "' is not a complex number that " + type + " holds, written (re,im)";
    }
    PropertyElement element;
    if (propertyElement(type, element)) return "a " + type + " is given by its elements, not by a text";
    return "the type " + (isXmlText(type) && !type.empty() ? type : std::string("given")) + " is not one that is written from a value";
}

bool isFileStorageProperty(const std::string& id) {
    static const char* ids[] = {"XISF:CreationTime",      "XISF:CreatorApplication", "XISF:CreatorModule",     "XISF:CreatorOS",
                                "XISF:BlockAlignmentSize", "XISF:MaxInlineBlockSize", "XISF:CompressionCodecs", "XISF:CompressionLevel"};
    for (const char* known : ids)
        if (id == known) return true;
    return false;
}

namespace {
// UTF-8, and with `xml` nothing that XML 1.0 cannot hold: control characters other than tab and
// line breaks, and the code points U+FFFE and U+FFFF.
bool validText(const std::string& s, bool xml) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (xml && ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7F)) return false;
            ++i;
            continue;
        }
        const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
        if (n == 0 || c > 0xF4 || i + n > s.size()) return false;
        uint32_t code = c & (0xFF >> (n + 1));
        for (size_t k = 1; k < n; ++k) {
            const unsigned char t = static_cast<unsigned char>(s[i + k]);
            if ((t & 0xC0) != 0x80) return false;
            code = (code << 6) | (t & 0x3F);
        }
        if ((n == 3 && code < 0x800) || (n == 4 && code < 0x10000) || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) return false;
        if (xml && (code == 0xFFFE || code == 0xFFFF)) return false;
        i += n;
    }
    return true;
}
}  // namespace

bool isValidUtf8(const std::string& s) { return validText(s, false); }

bool isXmlText(const std::string& s) { return validText(s, true); }

bool textFitsElement(const std::string& s) {
    auto space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    if (!s.empty() && (space(static_cast<unsigned char>(s.front())) || space(static_cast<unsigned char>(s.back())))) return false;
    if (s.find('\r') != std::string::npos) return false;
    return isXmlText(s);
}

uint64_t propertyBudget(uint64_t fileSize) {
    const uint64_t slack = uint64_t(256) << 20;
    return fileSize > UINT64_MAX - slack ? UINT64_MAX : fileSize + slack;
}

bool isSolutionProperty(const std::string& id) { return startsWith(id, "PCL:AstrometricSolution:"); }

const Property* findProperty(const std::vector<Property>& properties, const std::string& id) {
    for (const auto& p : properties)
        if (p.id == id) return &p;
    return nullptr;
}

Property scalarProperty(const std::string& id, const char* type, const std::string& value) {
    Property p;
    p.id = id;
    p.type = type;
    p.text = value;
    return p;
}

Property vectorProperty(const std::string& id, const std::vector<double>& values) {
    Property p;
    p.id = id;
    p.type = "F64Vector";
    p.array = true;
    p.inHeader = true;
    p.rows = values.size();
    appendLittleEndian(p.data, values);
    return p;
}

Property matrixProperty(const std::string& id, size_t rows, size_t columns, const std::vector<double>& values) {
    Property p;
    p.id = id;
    p.type = "F64Matrix";
    p.array = true;
    p.inHeader = true;
    p.rows = rows;
    p.columns = columns;
    appendLittleEndian(p.data, values);
    return p;
}

bool propertyNumbers(const Property& p, std::vector<double>& out) {
    PropertyElement e;
    if (!p.array || !propertyElement(p.type, e) || e.kind == 'c' || !propertyProblem(p).empty()) return false;
    const size_t n = p.data.size() / e.size;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        uint8_t b[8];
        std::memcpy(b, p.data.data() + i * e.size, e.size);
        if (!hostIsLittleEndian()) std::reverse(b, b + e.size);
        if (e.kind == 'f') {
            if (e.size == 4) { float f; std::memcpy(&f, b, 4); out[i] = f; }
            else { double d; std::memcpy(&d, b, 8); out[i] = d; }
        } else if (e.kind == 'i') {
            int64_t v = 0;
            switch (e.size) {
                case 1: { int8_t x; std::memcpy(&x, b, 1); v = x; break; }
                case 2: { int16_t x; std::memcpy(&x, b, 2); v = x; break; }
                case 4: { int32_t x; std::memcpy(&x, b, 4); v = x; break; }
                default: std::memcpy(&v, b, 8);
            }
            out[i] = static_cast<double>(v);
        } else {
            uint64_t v = 0;
            switch (e.size) {
                case 1: v = b[0]; break;
                case 2: { uint16_t x; std::memcpy(&x, b, 2); v = x; break; }
                case 4: { uint32_t x; std::memcpy(&x, b, 4); v = x; break; }
                default: std::memcpy(&v, b, 8);
            }
            out[i] = static_cast<double>(v);
        }
    }
    return true;
}

namespace {

bool digits(const std::string& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// i_j, with one or more digits each
bool indexPair(const std::string& s, size_t from) {
    const size_t bar = s.find('_', from);
    return bar != std::string::npos && bar > from && digits(s.substr(from, bar - from), 0) && digits(s, bar + 1);
}

// True for the keywords that describe a world coordinate system, its distortion included, in
// the primary description and in the alternate ones (CRVAL1A).
bool isWcsKeyword(const std::string& upper) {
    const auto letter = [](char c) { return c >= 'A' && c <= 'Z'; };
    // the SIP polynomials: A_ORDER, A_p_q, B_p_q, AP_p_q, BP_p_q ...
    for (const char* w : {"A_ORDER", "B_ORDER", "AP_ORDER", "BP_ORDER", "A_DMAX", "B_DMAX"})
        if (upper == w) return true;
    for (const char* prefix : {"A_", "B_", "AP_", "BP_"})
        if (startsWith(upper, prefix) && indexPair(upper, std::strlen(prefix))) return true;
    if (upper == "EPOCH") return true;   // the equinox, as it was written before EQUINOX
    // the distortion of IRAF (TNX, ZPX: WATn_nnn) and the lookup tables of the distortion paper
    if (startsWith(upper, "WAT") && upper.size() == 8 && upper[4] == '_' && digits(upper.substr(3, 1), 0) && digits(upper, 5)) return true;
    for (const char* prefix : {"CPDIS", "CQDIS", "CPERR", "CQERR", "D2IMDIS", "D2IMERR", "D2IM"})
        if (startsWith(upper, prefix) && upper.size() > std::strlen(prefix)) return true;
    for (const char* prefix : {"DP", "DQ"}) {   // DP1 = 'NAXES: 2', DQ2A ...
        std::string rest = upper.substr(std::min<size_t>(2, upper.size()));
        if (rest.size() > 1 && letter(rest.back())) rest.pop_back();
        if (startsWith(upper, prefix) && digits(rest, 0)) return true;
    }
    // the matrix as the first WCS papers wrote it: PC001002, CD002001
    for (const char* prefix : {"PC", "CD"})
        if (startsWith(upper, prefix) && upper.size() == 8 && digits(upper, 2)) return true;
    for (const char* w : {"WCSAXES", "LONPOLE", "LATPOLE", "RADESYS", "RADECSYS", "EQUINOX"}) {
        const size_t n = std::strlen(w);
        if (upper == w || (upper.size() == n + 1 && startsWith(upper, w) && letter(upper.back()))) return true;
    }
    std::string n = upper;
    if (n.size() > 1 && letter(n.back()) && n[n.size() - 2] >= '0' && n[n.size() - 2] <= '9') n.pop_back();
    for (const char* prefix : {"CTYPE", "CUNIT", "CRVAL", "CRPIX", "CDELT", "CROTA"})
        if (startsWith(n, prefix) && digits(n, 5)) return true;
    for (const char* prefix : {"CD", "PC", "PV", "PS"})
        if (startsWith(n, prefix) && indexPair(n, 2)) return true;
    return false;
}

}  // namespace

std::string wcsDigest(const std::vector<FitsKeyword>& keywords, uint64_t width, uint64_t height, bool bottomUp) {
    std::vector<std::string> entries;
    for (const auto& k : keywords) {
        const std::string name = toUpper(trim(k.name));
        if (!isWcsKeyword(name)) continue;
        // A number counts as its value, a text as its content: how a program formats them is not the WCS.
        const std::string value = trim(k.value);
        double number = 0;
        std::string entry = name + "=";
        std::string plain = value;   // (FITS also writes the exponent of a double as D)
        for (char& c : plain)
            if (c == 'D' || c == 'd') c = 'E';
        if (!value.empty() && value[0] != '\'' && parseDouble(plain, number)) entry += formatDouble(number == 0 ? 0.0 : number);   // (no -0)
        else entry += fitsUnquote(value);
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end());   // nor is the order of the cards
    std::string all = std::to_string(width) + "x" + std::to_string(height) + (bottomUp ? " bottom-up" : " top-down") + "\n";
    for (const auto& e : entries) all += e + "\n";
    return sha1Hex(reinterpret_cast<const uint8_t*>(all.data()), all.size());
}

}  // namespace xisfconv
