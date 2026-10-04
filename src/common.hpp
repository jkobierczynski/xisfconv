// xisfconv - XISF <-> FITS <-> ASDF converter with TIFF/PNG export
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace xisfconv {

constexpr const char* kVersion = "0.8.0";

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A feature of the file that this program (or this build of it) does not implement. The file
// itself may be perfectly fine.
struct Unsupported : Error {
    using Error::Error;
};

// Emits a warning on stderr, prefixed with the file currently being processed.
void warn(const std::string& message);
void setWarningContext(const std::string& context);
void setQuiet(bool quiet);

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
// Current UTC time as an ISO 8601 time point, e.g. 2026-10-02T02:04:05Z.
std::string utcTimestamp();

}  // namespace xisfconv
