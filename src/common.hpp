// xisfconv - XISF <-> FITS <-> ASDF converter with TIFF/PNG export
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xisfconv.h"

namespace xisfconv {

// The version is that of the public header: XISFCONV_VERSION_MAJOR, _MINOR and _PATCH.
#define XISFCONV_STRINGIFY_2(x) #x
#define XISFCONV_STRINGIFY(x) XISFCONV_STRINGIFY_2(x)
constexpr const char* kVersion = XISFCONV_STRINGIFY(XISFCONV_VERSION_MAJOR) "." XISFCONV_STRINGIFY(XISFCONV_VERSION_MINOR) "."
                                 XISFCONV_STRINGIFY(XISFCONV_VERSION_PATCH);

// What went wrong, for callers that have to tell the cases apart (the C API's status codes).
enum class ErrorKind {
    Format,       // the file is malformed or truncated
    Io,           // cannot open, read, write or rename
    Unsupported,  // a feature this library, or this build of it, does not implement
    Checksum,     // a stored checksum does not match the data
    Argument,     // an option that is out of range or does not apply
    Index,        // no image with that index
    Exists,       // the output exists and may not be overwritten
    NotFound,     // what was asked for is not in the file
    Cancelled,    // the progress handler asked to stop
    NotAllowed    // the header of a distributed XISF unit names a file it is not to be followed to
};

struct Error : std::runtime_error {
    explicit Error(const std::string& message, ErrorKind kind = ErrorKind::Format) : std::runtime_error(message), kind(kind) {}
    ErrorKind kind;
};

// A feature of the file that this program (or this build of it) does not implement. The file
// itself may be perfectly fine.
struct Unsupported : Error {
    explicit Unsupported(const std::string& message) : Error(message, ErrorKind::Unsupported) {}
};

// File names are UTF-8 everywhere. On Windows a std::string handed to the standard library
// would be taken in the ANSI code page, so every file is opened through these.
std::filesystem::path toPath(const std::string& utf8);
// Throws the error for an input that could not be opened: it says so if the path is a directory
// (which opens on some systems and not on others), "cannot open file" otherwise.
[[noreturn]] void failToOpen(const std::string& utf8);
std::string fromPath(const std::filesystem::path& path);

// Warnings and notes about the file being processed. The library prints nothing: a message
// goes to the handler that is installed on the calling thread, or nowhere if there is none.
enum class MessageLevel { Warning, Info };
using MessageHandler = std::function<void(MessageLevel level, const std::string& text)>;

// Installs a handler for the current thread for as long as the object lives; the handler that
// was installed before is put back afterwards.
class MessageScope {
public:
    explicit MessageScope(MessageHandler handler);
    ~MessageScope();
    MessageScope(const MessageScope&) = delete;
    MessageScope& operator=(const MessageScope&) = delete;

private:
    MessageHandler handler_;
    const MessageHandler* previous_;
};

void warn(const std::string& message);   // something the user should know about the result
void info(const std::string& message);   // how the conversion was done (row order, value range, WCS fit)

// Progress of a long operation, and the way to stop one. The handler is told what is being
// done and how far it is (`total` is 0 when that is not known) and returns false to cancel.
using ProgressHandler = std::function<bool(const char* stage, uint64_t done, uint64_t total)>;

class ProgressScope {
public:
    explicit ProgressScope(ProgressHandler handler);
    ~ProgressScope();
    ProgressScope(const ProgressScope&) = delete;
    ProgressScope& operator=(const ProgressScope&) = delete;

private:
    ProgressHandler handler_;
    const ProgressHandler* previous_;
    const char* stage_;   // the last report of the call it is made from (progressTick)
    uint64_t done_, total_, since_;
};

// Reports to the handler of the calling thread, if there is one. Throws Error (Cancelled) when
// the handler asks to stop; files being written are removed on the way out.
void progress(const char* stage, uint64_t done, uint64_t total);
// A sign of life in the middle of a long step, and the chance to stop it: `bytes` more were read
// or written. Every 8 MiB or so the handler gets the last report of the call again (as if the
// step had begun once more), and may stop the work there as it may at any report. A call that has
// reported nothing is not reported on.
void progressTick(uint64_t bytes);

enum class SampleFormat { UInt8, UInt16, UInt32, UInt64, Float32, Float64 };

size_t sampleBytes(SampleFormat f);
bool isFloat(SampleFormat f);
const char* sampleFormatName(SampleFormat f);          // XISF spelling, e.g. "UInt16"
bool parseXisfSampleFormat(const std::string& s, SampleFormat& out);
bool parseShortSampleFormat(const std::string& s, SampleFormat& out);  // u8,u16,u32,u64,f32,f64

bool hostIsLittleEndian();
void byteSwapInPlace(uint8_t* data, size_t count, size_t itemSize);

// Multiplies with overflow detection.
uint64_t checkedMul(uint64_t a, uint64_t b, const char* what);

struct FitsKeyword {
    std::string name;
    std::string value;    // FITS-formatted value as stored in XISF (strings keep their quotes)
    std::string comment;
};

// Decoded pixel data in host byte order, planar layout:
// channel 0 rows (top to bottom, each left to right), then channel 1, ...
struct PixelBuffer {
    uint64_t width = 0;
    uint64_t height = 0;
    uint64_t channels = 0;
    SampleFormat format = SampleFormat::UInt8;
    std::vector<uint8_t> data;

    uint64_t samples() const { return width * height * channels; }
    uint64_t planeSamples() const { return width * height; }
};

// Outcome of checking one file's integrity (--verify).
struct VerifyReport {
    std::string summary;                 // what was examined, e.g. "3 data blocks"
    size_t verified = 0;                 // checksums that were present and matched
    size_t unchecked = 0;                // blocks (or HDUs) that carry no checksum this build can verify
    std::vector<std::string> problems;   // empty = the file is intact as far as can be told
    std::vector<std::string> notChecked; // parts that could not be checked (unsupported codec or checksum)
};

std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
std::string toLower(std::string s);
std::string toUpper(std::string s);
bool startsWith(const std::string& s, const std::string& prefix);
bool parseUInt64(const std::string& s, uint64_t& out);
bool parseDouble(const std::string& s, double& out);

// Shortest decimal text that reads back as the same double.
std::string formatDouble(double v);

// The library lives in programs that may have set a locale with a decimal comma, which printf
// and strtod then follow. Numbers in files always have a decimal point:
// cNumber turns text printf made into that form, strtodC reads such text.
std::string cNumber(const char* printed);
double strtodC(const std::string& text, bool* complete = nullptr);
// Current UTC time as an ISO 8601 time point, e.g. 2026-10-02T02:04:05Z.
std::string utcTimestamp();

}  // namespace xisfconv
