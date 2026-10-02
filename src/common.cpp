// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <limits>
#include <sstream>

namespace xisfconv {

namespace {
std::string g_context;
bool g_quiet = false;
}  // namespace

void setWarningContext(const std::string& context) { g_context = context; }
void setQuiet(bool quiet) { g_quiet = quiet; }

void warn(const std::string& message) {
    if (g_quiet) return;
    std::cerr << "warning: ";
    if (!g_context.empty()) std::cerr << g_context << ": ";
    std::cerr << message << '\n';
}

size_t sampleBytes(SampleFormat f) {
    switch (f) {
        case SampleFormat::UInt8: return 1;
        case SampleFormat::UInt16: return 2;
        case SampleFormat::UInt32: return 4;
        case SampleFormat::UInt64: return 8;
        case SampleFormat::Float32: return 4;
        case SampleFormat::Float64: return 8;
    }
    return 1;
}

bool isFloat(SampleFormat f) { return f == SampleFormat::Float32 || f == SampleFormat::Float64; }

const char* sampleFormatName(SampleFormat f) {
    switch (f) {
        case SampleFormat::UInt8: return "UInt8";
        case SampleFormat::UInt16: return "UInt16";
        case SampleFormat::UInt32: return "UInt32";
        case SampleFormat::UInt64: return "UInt64";
        case SampleFormat::Float32: return "Float32";
        case SampleFormat::Float64: return "Float64";
    }
    return "?";
}

bool parseXisfSampleFormat(const std::string& s, SampleFormat& out) {
    static const struct { const char* name; SampleFormat f; } table[] = {
        {"UInt8", SampleFormat::UInt8},     {"UInt16", SampleFormat::UInt16},
        {"UInt32", SampleFormat::UInt32},   {"UInt64", SampleFormat::UInt64},
        {"Float32", SampleFormat::Float32}, {"Float64", SampleFormat::Float64},
    };
    for (const auto& e : table) {
        if (s == e.name) { out = e.f; return true; }
    }
    return false;
}

bool parseShortSampleFormat(const std::string& s, SampleFormat& out) {
    const std::string l = toLower(s);
    if (l == "u8" || l == "8") out = SampleFormat::UInt8;
    else if (l == "u16" || l == "16") out = SampleFormat::UInt16;
    else if (l == "u32") out = SampleFormat::UInt32;
    else if (l == "u64") out = SampleFormat::UInt64;
    else if (l == "f32" || l == "32f" || l == "float") out = SampleFormat::Float32;
    else if (l == "f64" || l == "64f" || l == "double") out = SampleFormat::Float64;
    else return false;
    return true;
}

bool hostIsLittleEndian() {
    const uint16_t x = 1;
    uint8_t b = 0;
    std::memcpy(&b, &x, 1);
    return b == 1;
}

void byteSwapInPlace(uint8_t* data, size_t count, size_t itemSize) {
    if (itemSize <= 1) return;
    for (size_t i = 0; i < count; ++i) {
        std::reverse(data + i * itemSize, data + (i + 1) * itemSize);
    }
}

uint64_t checkedMul(uint64_t a, uint64_t b, const char* what) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
        throw Error(std::string("size overflow computing ") + what);
    }
    return a * b;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string toUpper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool parseUInt64(const std::string& str, uint64_t& out) {
    const std::string s = trim(str);
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (std::numeric_limits<uint64_t>::max() - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

bool parseDouble(const std::string& str, double& out) {
    // Locale-independent parse ("." decimal separator regardless of LC_NUMERIC).
    const std::string s = trim(str);
    if (s.empty()) return false;
    std::istringstream is(s);
    is.imbue(std::locale::classic());
    double v = 0;
    is >> v;
    if (is.fail()) return false;
    is >> std::ws;
    if (!is.eof()) return false;
    out = v;
    return true;
}

std::string formatDouble(double v) {
    char buf[40] = "0";
    for (int precision : {15, 16, 17}) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, v);
        double back;
        if (parseDouble(buf, back) && back == v) break;
    }
    return buf;
}

std::string utcTimestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace xisfconv
