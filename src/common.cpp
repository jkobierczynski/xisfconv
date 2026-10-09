// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <sstream>
#include <utility>

namespace xisfconv {

namespace {
thread_local const MessageHandler* t_handler = nullptr;

void emit(MessageLevel level, const std::string& message) {
    const MessageHandler* handler = t_handler;
    if (!handler || !*handler) return;
    // A handler that converts another file must not receive that file's messages as its own.
    t_handler = nullptr;
    try {
        (*handler)(level, message);
    } catch (...) {
        t_handler = handler;
        throw;
    }
    t_handler = handler;
}
}  // namespace

MessageScope::MessageScope(MessageHandler handler) : handler_(std::move(handler)), previous_(t_handler) { t_handler = &handler_; }
MessageScope::~MessageScope() { t_handler = previous_; }

void warn(const std::string& message) { emit(MessageLevel::Warning, message); }
void info(const std::string& message) { emit(MessageLevel::Info, message); }

namespace {
thread_local const ProgressHandler* t_progress = nullptr;
// The last report of the call, which a sign of life repeats, and the bytes since then.
thread_local const char* t_stage = nullptr;
thread_local uint64_t t_done = 0, t_total = 0, t_since = 0;
constexpr uint64_t kTickBytes = uint64_t(8) << 20;
}  // namespace

// A handler is installed for a call: what an earlier call (or the call this one is made from)
// reported is not this one's, and comes back when it ends.
ProgressScope::ProgressScope(ProgressHandler handler)
    : handler_(std::move(handler)), previous_(t_progress), stage_(t_stage), done_(t_done), total_(t_total), since_(t_since) {
    t_progress = &handler_;
    t_stage = nullptr;
    t_done = t_total = t_since = 0;
}
ProgressScope::~ProgressScope() {
    t_progress = previous_;
    t_stage = stage_;
    t_done = done_;
    t_total = total_;
    t_since = since_;
}

void progress(const char* stage, uint64_t done, uint64_t total) {
    t_stage = stage;
    t_done = done;
    t_total = total;
    t_since = 0;
    const ProgressHandler* handler = t_progress;
    if (!handler || !*handler) return;
    if (!(*handler)(stage, done, total)) throw Error("cancelled", ErrorKind::Cancelled);
}

void progressTick(uint64_t bytes) {
    if (!t_stage) return;   // (a call that reports no steps has none to repeat: reading the pixels of an open file)
    t_since += bytes;
    if (t_since < kTickBytes) return;
    t_since = 0;
    const ProgressHandler* handler = t_progress;
    if (!handler || !*handler) return;
    if (!(*handler)(t_stage, t_done, t_total)) throw Error("cancelled", ErrorKind::Cancelled);
}

std::filesystem::path toPath(const std::string& utf8) {
#if defined(__cpp_lib_char8_t)
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::u8path(utf8);
#endif
}

void failToOpen(const std::string& utf8) {
    std::error_code ec;
    if (std::filesystem::is_directory(toPath(utf8), ec)) throw Error("is a directory, not a file", ErrorKind::Io);
    throw Error("cannot open file", ErrorKind::Io);
}

std::string fromPath(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
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
        const std::string text = cNumber(buf);
        std::snprintf(buf, sizeof buf, "%s", text.c_str());
        double back;
        if (parseDouble(buf, back) && back == v) break;
    }
    return buf;
}

namespace {
// The decimal point of the locale the host program has set ("." unless it set one).
std::string localeDecimalPoint() {
    const std::lconv* conventions = std::localeconv();
    return conventions && conventions->decimal_point && *conventions->decimal_point ? conventions->decimal_point : ".";
}
}  // namespace

std::string cNumber(const char* printed) {
    std::string text = printed;
    const std::string point = localeDecimalPoint();
    if (point != ".") {
        const size_t at = text.find(point);
        if (at != std::string::npos) text.replace(at, point.size(), ".");
    }
    return text;
}

double strtodC(const std::string& text, bool* complete) {
    std::string local = text;
    const std::string point = localeDecimalPoint();
    if (point != ".") {
        const size_t at = local.find('.');
        if (at != std::string::npos) local.replace(at, 1, point);
    }
    char* end = nullptr;
    const double value = std::strtod(local.c_str(), &end);
    if (complete) *complete = !local.empty() && end == local.c_str() + local.size();
    return value;
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
