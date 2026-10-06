// The C API: every function of xisfconv.h, as a thin layer over the C++ modules.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "xisfconv.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "asdf.hpp"
#include "codecs.hpp"
#include "common.hpp"
#include "convert.hpp"
#include "fits.hpp"
#include "fitsread.hpp"
#include "pipeline.hpp"
#include "wcs.hpp"
#include "xisf.hpp"
#include "xisfrewrite.hpp"
#include "yaml.hpp"

using namespace xisfconv;

// ------------------------------------------------------------------------------------------
// Handles
// ------------------------------------------------------------------------------------------

namespace {

struct KeptMessage {
    xisfconv_message_level level;
    bool named;         // it is about a file
    std::string path;
    std::string text;
};

struct ContextState {
    std::string error;
    xisfconv_message_fn message = nullptr;
    void* messageUser = nullptr;
    xisfconv_progress_fn progress = nullptr;
    void* progressUser = nullptr;
    bool keepMessages = false;        // xisfconv_context_keep_messages
    std::vector<KeptMessage> kept;
    std::atomic<bool> cancel{false};  // xisfconv_context_cancel: the one thing another thread may set
    std::atomic<int> running{0};      // calls in progress in this context (nested ones included)
    xisfconv_host_progress_fn hostProgress = nullptr;  // xisfconv_context_set_host_progress
    void* hostProgressUser = nullptr;
    bool hostProgressFailed = false;
};

// Counts a call in a context for as long as it runs.
struct Running {
    explicit Running(ContextState* s) : state(s) { state->running.fetch_add(1); }
    ~Running() { state->running.fetch_sub(1); }
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;
    ContextState* state;
};

// Between two steps of a call: does the host's progress handler let it go on?
bool hostAllows(ContextState* s, const char* stage, uint64_t done, uint64_t total) {
    if (!s->hostProgress) return true;
    const xisfconv_progress_report report{s->hostProgressUser, stage, done, total};
    const int32_t answer = s->hostProgress(&report);
    if (answer == XISFCONV_HOST_GO_ON) return true;
    if (answer != XISFCONV_HOST_STOP) s->hostProgressFailed = true;
    return false;
}

using StatePtr = std::shared_ptr<ContextState>;

// What is known about a FITS or ASDF image once its pixels have been read.
struct Known {
    bool known = false;
    SampleFormat format = SampleFormat::UInt8;
    double lower = 0, upper = 1;
    std::string mapping;
};

}  // namespace

struct xisfconv_context {
    StatePtr state;
};

struct xisfconv_keywords {
    StatePtr state;
    std::vector<FitsKeyword> cards;
    bool readOnly = false;       // owned by a file
    mutable std::string text;    // xisfconv_keywords_get_text
    std::string summary;         // xisfconv_wcs_keywords: fit quality
};

struct xisfconv_file {
    StatePtr state;
    std::string path;                 // as the caller named it: for messages
    std::string readPath;             // absolute: FITS and ASDF pixels are read from the file again later,
                                      //   when the working directory may be another one
    xisfconv_format format = XISFCONV_FORMAT_XISF;
    std::unique_ptr<XisfFile> xisf;   // XISF
    FitsFile fits;                    // FITS and ASDF: the headers
    std::vector<Known> known;         // FITS and ASDF, one per image
    std::vector<std::unique_ptr<xisfconv_keywords>> keywords;  // one per image
    std::optional<size_t> loaded;     // FITS and ASDF: the image whose pixels are held
    bool loadedVerified = false;      // ... read with its checksum verified
    FitsImage loadedImage;
    bool headerRead = false;
    std::string headerText;
    std::optional<size_t> iccImage;   // ICC profile read last
    std::vector<uint8_t> icc;
};

struct xisfconv_report {
    xisfconv_format format = XISFCONV_FORMAT_XISF;
    VerifyReport report;
};

struct xisfconv_writer {
    StatePtr state;
    std::string path;
    Format format = Format::Xisf;
    ConvertOptions options;
    xisfconv_row_order rowOrder = XISFCONV_ROWS_DEFAULT;
    FitsFile images;
};

namespace {

// ------------------------------------------------------------------------------------------
// Errors and the calling convention
// ------------------------------------------------------------------------------------------

xisfconv_status statusOf(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::Format: return XISFCONV_ERR_FORMAT;
        case ErrorKind::Io: return XISFCONV_ERR_IO;
        case ErrorKind::Unsupported: return XISFCONV_ERR_UNSUPPORTED;
        case ErrorKind::Checksum: return XISFCONV_ERR_CHECKSUM;
        case ErrorKind::Argument: return XISFCONV_ERR_ARGUMENT;
        case ErrorKind::Index: return XISFCONV_ERR_INDEX;
        case ErrorKind::Exists: return XISFCONV_ERR_EXISTS;
        case ErrorKind::NotFound: return XISFCONV_ERR_NOT_FOUND;
        case ErrorKind::Cancelled: return XISFCONV_ERR_CANCELLED;
    }
    return XISFCONV_ERR_INTERNAL;
}

// An error the API layer raises itself, with the status it stands for.
struct ApiError {
    xisfconv_status status;
    std::string message;
};

[[noreturn]] void fail(xisfconv_status status, const std::string& message) { throw ApiError{status, message}; }

// Runs `body` with the context's handlers installed and turns every exception into a status.
template <class Body>
xisfconv_status guarded(const StatePtr& state, const char* path, Body&& body) noexcept {
    if (!state) return XISFCONV_ERR_ARGUMENT;
    auto remember = [&](const char* text) noexcept {
        try {
            state->error = text;
        } catch (...) {
            state->error.clear();
        }
    };
    try {
        const std::string file = path ? path : "";
        ContextState* s = state.get();
        const bool named = path != nullptr;
        const MessageScope messages([s, file, named](MessageLevel level, const std::string& text) {
            const xisfconv_message_level kind = level == MessageLevel::Warning ? XISFCONV_MESSAGE_WARNING : XISFCONV_MESSAGE_INFO;
            if (s->keepMessages) s->kept.push_back({kind, named, file, text});
            if (s->message) s->message(s->messageUser, kind, named ? file.c_str() : nullptr, text.c_str());
        });
        // A request to stop is for the call it is made during: one that came between calls is
        // dropped. (A call made from inside another, by a handler of the host, leaves the outer
        // one's request alone.)
        if (s->running.load() == 0) {
            s->cancel.store(false);
            s->hostProgressFailed = false;
        }
        const Running running(s);
        const ProgressScope reports([s](const char* stage, uint64_t done, uint64_t total) {
            if (s->cancel.exchange(false)) return false;
            if (!hostAllows(s, stage, done, total)) return false;
            return !s->progress || s->progress(s->progressUser, stage, done, total) == 0;
        });
        body();
        return XISFCONV_OK;
    } catch (const ApiError& e) {
        remember(e.message.c_str());
        return e.status;
    } catch (const Error& e) {
        remember(e.what());
        return statusOf(e.kind);
    } catch (const std::bad_alloc&) {
        remember("out of memory");
        return XISFCONV_ERR_MEMORY;
    } catch (const std::length_error&) {
        remember("out of memory");
        return XISFCONV_ERR_MEMORY;
    } catch (const std::filesystem::filesystem_error& e) {
        remember(e.what());
        return XISFCONV_ERR_IO;
    } catch (const std::exception& e) {
        remember(e.what());
        return XISFCONV_ERR_INTERNAL;
    } catch (...) {
        remember("unknown error");
        return XISFCONV_ERR_INTERNAL;
    }
}

// Fills a caller's struct with defaults, as far as the caller's version of it reaches: a program
// built against an older, shorter struct must not have memory behind it written.
template <class T>
void initStruct(T* target, size_t callerSize, T defaults) {
    if (!target || callerSize < sizeof(size_t)) return;
    defaults.struct_size = std::min(callerSize, sizeof(T));
    std::memcpy(static_cast<void*>(target), &defaults, defaults.struct_size);
}

// A struct handed in by the caller, read up to the size the caller's header knew.
template <class T>
T optionsFrom(const T* given, void (*init)(T*, size_t)) {
    T out;
    init(&out, sizeof out);
    if (!given) return out;
    if (given->struct_size < sizeof(size_t)) fail(XISFCONV_ERR_ARGUMENT, "options: struct_size is not set (call the _init function first)");
    std::memcpy(static_cast<void*>(&out), given, std::min(given->struct_size, sizeof(T)));
    out.struct_size = sizeof(T);
    return out;
}

// ------------------------------------------------------------------------------------------
// Enumerations
// ------------------------------------------------------------------------------------------

xisfconv_sample_format toApi(SampleFormat f) {
    switch (f) {
        case SampleFormat::UInt8: return XISFCONV_SAMPLE_UINT8;
        case SampleFormat::UInt16: return XISFCONV_SAMPLE_UINT16;
        case SampleFormat::UInt32: return XISFCONV_SAMPLE_UINT32;
        case SampleFormat::UInt64: return XISFCONV_SAMPLE_UINT64;
        case SampleFormat::Float32: return XISFCONV_SAMPLE_FLOAT32;
        case SampleFormat::Float64: return XISFCONV_SAMPLE_FLOAT64;
    }
    return XISFCONV_SAMPLE_UINT8;
}

bool fromApi(xisfconv_sample_format f, SampleFormat& out) {
    switch (f) {
        case XISFCONV_SAMPLE_UINT8: out = SampleFormat::UInt8; return true;
        case XISFCONV_SAMPLE_UINT16: out = SampleFormat::UInt16; return true;
        case XISFCONV_SAMPLE_UINT32: out = SampleFormat::UInt32; return true;
        case XISFCONV_SAMPLE_UINT64: out = SampleFormat::UInt64; return true;
        case XISFCONV_SAMPLE_FLOAT32: out = SampleFormat::Float32; return true;
        case XISFCONV_SAMPLE_FLOAT64: out = SampleFormat::Float64; return true;
        default: return false;
    }
}

std::optional<SampleFormat> wantedFormat(xisfconv_sample_format f) {
    if (f == XISFCONV_SAMPLE_AS_STORED) return std::nullopt;
    SampleFormat out;
    if (!fromApi(f, out)) fail(XISFCONV_ERR_ARGUMENT, "unknown sample format " + std::to_string(f));
    return out;
}

std::string checksumName(xisfconv_checksum c, bool forRewrite) {
    switch (c) {
        case XISFCONV_CHECKSUM_NONE: return forRewrite ? "none" : "";
        case XISFCONV_CHECKSUM_SHA1: return "sha1";
        case XISFCONV_CHECKSUM_SHA256: return "sha256";
        case XISFCONV_CHECKSUM_SHA512: return "sha512";
        case XISFCONV_CHECKSUM_SHA3_256: return "sha3-256";
        case XISFCONV_CHECKSUM_SHA3_512: return "sha3-512";
        case XISFCONV_CHECKSUM_KEEP:
            if (forRewrite) return "";
            break;
        default: break;
    }
    fail(XISFCONV_ERR_ARGUMENT, "unknown checksum algorithm " + std::to_string(c));
}

Stretch stretchFrom(xisfconv_stretch s) {
    switch (s) {
        case XISFCONV_STRETCH_NONE: return Stretch::None;
        case XISFCONV_STRETCH_AUTO: return Stretch::Auto;
        case XISFCONV_STRETCH_LINKED: return Stretch::Linked;
        case XISFCONV_STRETCH_UNLINKED: return Stretch::Unlinked;
        case XISFCONV_STRETCH_STORED: return Stretch::Stored;
        default: fail(XISFCONV_ERR_ARGUMENT, "unknown stretch mode " + std::to_string(s));
    }
}

void checkRowOrder(xisfconv_row_order order) {
    if (order != XISFCONV_ROWS_DEFAULT && order != XISFCONV_ROWS_TOP_DOWN && order != XISFCONV_ROWS_BOTTOM_UP) {
        fail(XISFCONV_ERR_ARGUMENT, "unknown row order " + std::to_string(order));
    }
}

// Compression for a conversion or the writer: `compress` and the codec name.
void codecFrom(xisfconv_codec codec, ConvertOptions& out) {
    switch (codec) {
        case XISFCONV_CODEC_NONE: out.compress = false; out.codec.clear(); return;
        case XISFCONV_CODEC_DEFAULT: out.compress = true; out.codec.clear(); return;
        case XISFCONV_CODEC_ZLIB: out.compress = true; out.codec = "zlib"; return;
        case XISFCONV_CODEC_ZSTD:
            if (!zstdAvailable()) fail(XISFCONV_ERR_UNSUPPORTED, "this build has no Zstandard support; use --codec zlib");
            out.compress = true;
            out.codec = "zstd";
            return;
        case XISFCONV_CODEC_LZ4:
        case XISFCONV_CODEC_LZ4HC: fail(XISFCONV_ERR_UNSUPPORTED, "LZ4 compression is read, not written");
        default: fail(XISFCONV_ERR_ARGUMENT, "unknown codec " + std::to_string(codec));
    }
}

std::optional<Format> formatFromExtension(const std::string& path) {
    const std::string e = toLower(fromPath(toPath(path).extension()));
    // image.fits.fz: FITS, tile-compressed
    if (e == ".fz") return formatFromExtension(fromPath(toPath(path).stem())) == Format::Fits ? std::optional<Format>(Format::Fits) : std::nullopt;
    if (e == ".fits" || e == ".fit" || e == ".fts") return Format::Fits;
    if (e == ".tif" || e == ".tiff") return Format::Tiff;
    if (e == ".png") return Format::Png;
    if (e == ".xisf") return Format::Xisf;
    if (e == ".asdf") return Format::Asdf;
    return std::nullopt;
}

Format outputFormat(xisfconv_format format, const std::string& path) {
    switch (format) {
        case XISFCONV_FORMAT_XISF: return Format::Xisf;
        case XISFCONV_FORMAT_FITS: return Format::Fits;
        case XISFCONV_FORMAT_ASDF: return Format::Asdf;
        case XISFCONV_FORMAT_TIFF: return Format::Tiff;
        case XISFCONV_FORMAT_PNG: return Format::Png;
        case XISFCONV_FORMAT_AUTO:
            if (auto f = formatFromExtension(path)) return *f;
            fail(XISFCONV_ERR_ARGUMENT, "cannot infer output format from '" + path + "'; add --to fits|asdf|tiff|png|xisf");
        default: fail(XISFCONV_ERR_ARGUMENT, "unknown output format " + std::to_string(format));
    }
}

std::optional<size_t> imageChoice(size_t image) {
    if (image == XISFCONV_ALL_IMAGES) return std::nullopt;
    return image;
}

XisfRewriteOptions rewriteOptionsFrom(const xisfconv_rewrite_options& o) {
    XisfRewriteOptions r;
    switch (o.codec) {
        case XISFCONV_CODEC_KEEP: break;
        case XISFCONV_CODEC_NONE: r.codec = "none"; break;
        case XISFCONV_CODEC_ZLIB: r.codec = "zlib"; break;
        case XISFCONV_CODEC_ZSTD: r.codec = "zstd"; break;
        case XISFCONV_CODEC_DEFAULT: r.codec = zstdAvailable() ? "zstd" : "zlib"; break;
        case XISFCONV_CODEC_LZ4:
        case XISFCONV_CODEC_LZ4HC: fail(XISFCONV_ERR_UNSUPPORTED, "LZ4 compression is read, not written");
        default: fail(XISFCONV_ERR_ARGUMENT, "unknown codec " + std::to_string(o.codec));
    }
    r.checksum = checksumName(o.checksum, true);
    r.imageIndex = imageChoice(o.image);
    r.verifyInput = o.verify_input != 0;
    r.readBack = o.read_back != 0;
    if (o.subblock_size == 0) fail(XISFCONV_ERR_ARGUMENT, "invalid subblock size");
    r.subblockSize = o.subblock_size;
    return r;
}

void fillResult(const XisfFileRewrite& done, xisfconv_rewrite_result* result) {
    if (!result) return;
    xisfconv_rewrite_result r;
    xisfconv_rewrite_result_init(&r, sizeof r);
    r.input_size = done.inputSize;
    r.output_size = done.unchanged ? done.inputSize : done.result.outputSize;
    r.blocks = done.result.blocks;
    r.compressed = done.result.compressed;
    r.decompressed = done.result.decompressed;
    r.kept = done.result.kept;
    r.checksums = done.result.checksums;
    r.checksums_removed = done.result.checksumsRemoved;
    r.read_back = done.result.readBack ? 1 : 0;
    r.changed = done.unchanged ? 0 : (done.result.changed ? 1 : 0);
    const size_t size = result->struct_size >= sizeof(size_t) ? std::min(result->struct_size, sizeof r) : sizeof r;
    r.struct_size = size;
    std::memcpy(static_cast<void*>(result), &r, size);
}

// ------------------------------------------------------------------------------------------
// Files
// ------------------------------------------------------------------------------------------

bool isXisf(const xisfconv_file* f) { return f->format == XISFCONV_FORMAT_XISF; }

size_t imageCount(const xisfconv_file* f) { return isXisf(f) ? f->xisf->images().size() : f->fits.images.size(); }

void checkImage(const xisfconv_file* f, size_t image) {
    if (image >= imageCount(f)) {
        fail(XISFCONV_ERR_INDEX, "image index " + std::to_string(image) + " out of range (file has " + std::to_string(imageCount(f)) + ")");
    }
}

bool hasKeyword(const std::vector<FitsKeyword>& cards, const char* name) {
    for (const auto& k : cards)
        if (toUpper(trim(k.name)) == name) return true;
    return false;
}

const std::vector<FitsKeyword>& cardsOf(const xisfconv_file* f, size_t image) {
    return isXisf(f) ? f->xisf->images()[image].keywords : f->fits.images[image].keywords;
}

// Reads the pixels of a FITS or ASDF image into the handle, unless they are there already.
void loadPixels(xisfconv_file* f, size_t image, bool verify) {
    // Pixels that were loaded without verifying their checksum do not serve a caller who wants it verified.
    if (isXisf(f) || (f->loaded && *f->loaded == image && (f->loadedVerified || !verify))) return;
    f->loaded.reset();
    f->loadedImage = FitsImage();
    FitsFile all = f->format == XISFCONV_FORMAT_ASDF ? readAsdf(f->readPath, false, verify, image)
                                                     : readFits(f->readPath, false, image);
    if (image >= all.images.size() || !all.images[image].hasData) {
        fail(XISFCONV_ERR_FORMAT, "image " + std::to_string(image) + " cannot be read (has the file changed since it was opened?)");
    }
    // The caller sized its buffer by what the headers said when the file was opened.
    const PixelBuffer& then = f->fits.images[image].pixels;
    const PixelBuffer& now = all.images[image].pixels;
    if (now.width != then.width || now.height != then.height || now.channels != then.channels) {
        fail(XISFCONV_ERR_FORMAT, "image " + std::to_string(image) + " is not the image it was when the file was opened: "
                                  "the file has changed");
    }
    f->loadedImage = std::move(all.images[image]);
    f->loaded = image;
    f->loadedVerified = verify;
    Known& k = f->known[image];
    k.known = true;
    k.format = f->loadedImage.pixels.format;
    k.mapping = f->loadedImage.note;
    k.lower = 0;
    k.upper = 1;
    if (isFloat(k.format)) {
        const auto bounds = automaticBounds(f->loadedImage);
        k.lower = bounds.first;
        k.upper = bounds.second;
    }
}

// A path that cannot be opened and read is an I/O error, whatever the question about it was.
void mustBeReadable(const char* path) {
    std::error_code ec;
    if (std::filesystem::is_directory(toPath(path), ec)) throw Error("is a directory, not a file", ErrorKind::Io);
    std::ifstream in(toPath(path), std::ios::binary);
    char first = 0;
    if (!in || (!in.read(&first, 1) && !in.eof())) failToOpen(path);
}

void copyText(char* dest, size_t size, const std::string& text) {
    const size_t n = std::min(text.size(), size - 1);
    std::memcpy(dest, text.data(), n);
    dest[n] = 0;
}

std::string cardLine(const FitsKeyword& k) {
    std::string line = k.name;
    if (k.name.size() < 8) line += std::string(8 - k.name.size(), ' ');
    if (!k.value.empty()) line += "= " + k.value;
    if (!k.comment.empty()) line += (k.value.empty() ? " " : " / ") + k.comment;
    return line;
}

// ------------------------------------------------------------------------------------------
// The parsed ASDF tree as JSON (mappings as {"t": tag, "m": [[key, value], ...]})
// ------------------------------------------------------------------------------------------

void jsonString(const std::string& text, std::string& out) {
    out += '"';
    char buf[8];
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

void yamlJson(const YamlNode& node, int depth, std::string& out) {
    if (depth > 1000) throw Error("tree too deep");
    // aliases are written out wherever they are used, which a few lines of YAML can turn into gigabytes
    if (out.size() > (256u << 20)) throw Error("the tree is too large to be written out with its aliases expanded");
    if (node.isSequence()) {
        out += "[";
        for (size_t i = 0; i < node.items.size(); ++i) {
            if (i) out += ",";
            yamlJson(*node.items[i], depth + 1, out);
        }
        out += "]";
    } else if (node.isMapping()) {
        out += "{\"t\":";
        jsonString(node.tag, out);
        out += ",\"m\":[";
        for (size_t i = 0; i < node.pairs.size(); ++i) {
            out += i ? ",[" : "[";
            yamlJson(*node.pairs[i].first, depth + 1, out);
            out += ",";
            yamlJson(*node.pairs[i].second, depth + 1, out);
            out += "]";
        }
        out += "]}";
    } else {
        const YamlValue v = yamlResolve(node);
        switch (v.type) {
            case YamlValue::Type::Null: out += "null"; break;
            case YamlValue::Type::Bool: out += v.boolean ? "true" : "false"; break;
            case YamlValue::Type::Int: out += v.text; break;
            case YamlValue::Type::Float:
                out += "{\"f\":";
                jsonString(v.number != v.number || v.number - v.number != 0 ? v.text : formatDouble(v.number), out);
                out += "}";
                break;
            case YamlValue::Type::String: jsonString(v.text, out); break;
        }
    }
}

// ------------------------------------------------------------------------------------------
// WCS
// ------------------------------------------------------------------------------------------

bool allDigits(const std::string& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// Keywords of the FITS world coordinate system, including the SIP distortion convention.
bool isWcsKeyword(const std::string& rawName) {
    const std::string n = toUpper(trim(rawName));
    static const char* exact[] = {"WCSAXES", "LONPOLE", "LATPOLE", "RADESYS", "RADECSYS", "EQUINOX", "A_ORDER", "B_ORDER",
                                  "AP_ORDER", "BP_ORDER", "A_DMAX", "B_DMAX"};
    for (const char* e : exact)
        if (n == e) return true;
    static const char* numbered[] = {"CTYPE", "CUNIT", "CRVAL", "CRPIX", "CDELT", "CROTA"};
    for (const char* p : numbered)
        if (startsWith(n, p) && allDigits(n, std::strlen(p))) return true;
    static const char* matrix[] = {"CD", "PC", "PV", "A_", "B_", "AP_", "BP_"};
    for (const char* p : matrix) {
        const size_t len = std::strlen(p);
        if (!startsWith(n, p)) continue;
        const size_t sep = n.find('_', len);
        if (sep != std::string::npos && sep > len && allDigits(n.substr(0, sep), len) && allDigits(n, sep + 1)) return true;
    }
    return n == "PC001001" || n == "PC001002" || n == "PC002001" || n == "PC002002";
}

}  // namespace

// ------------------------------------------------------------------------------------------
// Status, version, codecs
// ------------------------------------------------------------------------------------------

const char* xisfconv_status_text(xisfconv_status status) {
    switch (status) {
        case XISFCONV_OK: return "no error";
        case XISFCONV_ERR_ARGUMENT: return "invalid argument";
        case XISFCONV_ERR_IO: return "input/output error";
        case XISFCONV_ERR_FORMAT: return "malformed file";
        case XISFCONV_ERR_UNSUPPORTED: return "not supported";
        case XISFCONV_ERR_CHECKSUM: return "checksum mismatch";
        case XISFCONV_ERR_MEMORY: return "out of memory";
        case XISFCONV_ERR_INDEX: return "index out of range";
        case XISFCONV_ERR_EXISTS: return "output exists";
        case XISFCONV_ERR_BUFFER: return "buffer too small";
        case XISFCONV_ERR_NOT_FOUND: return "not found";
        case XISFCONV_ERR_CANCELLED: return "cancelled";
        case XISFCONV_ERR_INTERNAL: return "internal error";
        default: return "unknown status";
    }
}

const char* xisfconv_version(void) { return kVersion; }

int32_t xisfconv_version_number(void) {
    return XISFCONV_VERSION_MAJOR * 10000 + XISFCONV_VERSION_MINOR * 100 + XISFCONV_VERSION_PATCH;
}

int32_t xisfconv_codec_available(xisfconv_codec codec, int32_t for_writing) {
    switch (codec) {
        case XISFCONV_CODEC_NONE:
        case XISFCONV_CODEC_ZLIB:
        case XISFCONV_CODEC_DEFAULT: return 1;
        case XISFCONV_CODEC_LZ4:
        case XISFCONV_CODEC_LZ4HC: return for_writing ? 0 : 1;
        case XISFCONV_CODEC_ZSTD: return zstdAvailable() ? 1 : 0;
        default: return 0;
    }
}

size_t xisfconv_sample_size(xisfconv_sample_format format) {
    SampleFormat f;
    return fromApi(format, f) ? sampleBytes(f) : 0;
}

// ------------------------------------------------------------------------------------------
// Context
// ------------------------------------------------------------------------------------------

xisfconv_context* xisfconv_context_new(void) {
    try {
        auto ctx = std::make_unique<xisfconv_context>();
        ctx->state = std::make_shared<ContextState>();
        return ctx.release();
    } catch (...) {
        return nullptr;
    }
}

void xisfconv_context_free(xisfconv_context* ctx) {
    if (!ctx) return;
    // Handles may outlive the context, but what the handlers point to need not: they are not called any more.
    ctx->state->message = nullptr;
    ctx->state->progress = nullptr;
    ctx->state->hostProgress = nullptr;
    ctx->state->keepMessages = false;
    ctx->state->kept.clear();
    delete ctx;
}

void xisfconv_context_keep_messages(xisfconv_context* ctx, int32_t keep) {
    if (!ctx) return;
    ctx->state->keepMessages = keep != 0;
    if (!keep) ctx->state->kept.clear();
}

size_t xisfconv_context_message_count(const xisfconv_context* ctx) { return ctx ? ctx->state->kept.size() : 0; }

xisfconv_status xisfconv_context_message(const xisfconv_context* ctx, size_t index, xisfconv_message_level* level,
                                         const char** path, const char** message) {
    if (level) *level = XISFCONV_MESSAGE_INFO;
    if (path) *path = nullptr;
    if (message) *message = "";
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    if (index >= ctx->state->kept.size()) return XISFCONV_ERR_INDEX;
    const KeptMessage& m = ctx->state->kept[index];
    if (level) *level = m.level;
    if (path) *path = m.named ? m.path.c_str() : nullptr;
    if (message) *message = m.text.c_str();
    return XISFCONV_OK;
}

void xisfconv_context_clear_messages(xisfconv_context* ctx) {
    if (ctx) ctx->state->kept.clear();
}

int32_t xisfconv_context_cancel(xisfconv_context* ctx) {
    if (!ctx) return 0;
    ctx->state->cancel.store(true);
    return ctx->state->running.load() > 0 ? 1 : 0;
}

int32_t xisfconv_context_running(const xisfconv_context* ctx) { return ctx && ctx->state->running.load() > 0 ? 1 : 0; }

void xisfconv_context_set_host_progress(xisfconv_context* ctx, xisfconv_host_progress_fn handler, void* user) {
    if (!ctx) return;
    ctx->state->hostProgress = handler;
    ctx->state->hostProgressUser = user;
}

int32_t xisfconv_context_host_progress_failed(const xisfconv_context* ctx) {
    return ctx && ctx->state->hostProgressFailed ? 1 : 0;
}

void xisfconv_context_set_message_handler(xisfconv_context* ctx, xisfconv_message_fn handler, void* user) {
    if (!ctx) return;
    ctx->state->message = handler;
    ctx->state->messageUser = user;
}

void xisfconv_context_set_progress_handler(xisfconv_context* ctx, xisfconv_progress_fn handler, void* user) {
    if (!ctx) return;
    ctx->state->progress = handler;
    ctx->state->progressUser = user;
}

const char* xisfconv_error_message(const xisfconv_context* ctx) { return ctx ? ctx->state->error.c_str() : ""; }

// ------------------------------------------------------------------------------------------
// Keyword lists
// ------------------------------------------------------------------------------------------

xisfconv_status xisfconv_keywords_new(xisfconv_context* ctx, xisfconv_keywords** out) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    if (out) *out = nullptr;
    return guarded(ctx->state, nullptr, [&] {
        if (!out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_keywords_new: out is NULL");
        auto kw = std::make_unique<xisfconv_keywords>();
        kw->state = ctx->state;
        *out = kw.release();
    });
}

void xisfconv_keywords_free(xisfconv_keywords* kw) {
    if (kw && !kw->readOnly) delete kw;
}

size_t xisfconv_keywords_count(const xisfconv_keywords* kw) { return kw ? kw->cards.size() : 0; }

xisfconv_status xisfconv_keywords_get(const xisfconv_keywords* kw, size_t index, const char** name, const char** value,
                                      const char** comment) {
    if (!kw) return XISFCONV_ERR_ARGUMENT;
    return guarded(kw->state, nullptr, [&] {
        if (index >= kw->cards.size()) fail(XISFCONV_ERR_INDEX, "keyword index " + std::to_string(index) + " out of range");
        const FitsKeyword& k = kw->cards[index];
        if (name) *name = k.name.c_str();
        if (value) *value = k.value.c_str();
        if (comment) *comment = k.comment.c_str();
    });
}

int64_t xisfconv_keywords_find(const xisfconv_keywords* kw, const char* name) {
    if (!kw || !name) return -1;
    try {
        const std::string wanted = toUpper(trim(name));
        for (size_t i = 0; i < kw->cards.size(); ++i)
            if (toUpper(trim(kw->cards[i].name)) == wanted) return static_cast<int64_t>(i);
    } catch (...) {
    }
    return -1;
}

xisfconv_status xisfconv_keywords_append(xisfconv_keywords* kw, const char* name, const char* value, const char* comment) {
    if (!kw) return XISFCONV_ERR_ARGUMENT;
    return guarded(kw->state, nullptr, [&] {
        if (kw->readOnly) fail(XISFCONV_ERR_ARGUMENT, "the keyword list belongs to a file and cannot be changed");
        if (!name) fail(XISFCONV_ERR_ARGUMENT, "a keyword needs a name (an empty one makes a card of text only)");
        kw->cards.push_back({name, value ? value : "", comment ? comment : ""});
    });
}

xisfconv_status xisfconv_keywords_append_string(xisfconv_keywords* kw, const char* name, const char* text,
                                                const char* comment) {
    if (!kw) return XISFCONV_ERR_ARGUMENT;
    return guarded(kw->state, nullptr, [&] {
        if (kw->readOnly) fail(XISFCONV_ERR_ARGUMENT, "the keyword list belongs to a file and cannot be changed");
        if (!name || !*name) fail(XISFCONV_ERR_ARGUMENT, "a keyword needs a name");
        kw->cards.push_back({name, fitsString(text ? text : ""), comment ? comment : ""});
    });
}

xisfconv_status xisfconv_keywords_append_number(xisfconv_keywords* kw, const char* name, double number,
                                                const char* comment) {
    if (!kw) return XISFCONV_ERR_ARGUMENT;
    return guarded(kw->state, nullptr, [&] {
        if (kw->readOnly) fail(XISFCONV_ERR_ARGUMENT, "the keyword list belongs to a file and cannot be changed");
        if (!name || !*name) fail(XISFCONV_ERR_ARGUMENT, "a keyword needs a name");
        if (number != number || number - number != 0) fail(XISFCONV_ERR_ARGUMENT, "a FITS keyword cannot hold NaN or infinity");
        std::string value = toUpper(formatDouble(number));   // every digit that matters; E for the exponent
        if (value.find_first_of(".E") == std::string::npos) value += ".0";
        kw->cards.push_back({name, value, comment ? comment : ""});
    });
}

xisfconv_status xisfconv_keywords_remove(xisfconv_keywords* kw, size_t index) {
    if (!kw) return XISFCONV_ERR_ARGUMENT;
    return guarded(kw->state, nullptr, [&] {
        if (kw->readOnly) fail(XISFCONV_ERR_ARGUMENT, "the keyword list belongs to a file and cannot be changed");
        if (index >= kw->cards.size()) fail(XISFCONV_ERR_INDEX, "keyword index " + std::to_string(index) + " out of range");
        kw->cards.erase(kw->cards.begin() + static_cast<std::ptrdiff_t>(index));
    });
}

xisfconv_status xisfconv_keywords_get_text(const xisfconv_keywords* kw, size_t index, const char** out) {
    if (!kw) return XISFCONV_ERR_ARGUMENT;
    return guarded(kw->state, nullptr, [&] {
        if (!out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_keywords_get_text: out is NULL");
        if (index >= kw->cards.size()) fail(XISFCONV_ERR_INDEX, "keyword index " + std::to_string(index) + " out of range");
        kw->text = fitsUnquote(kw->cards[index].value);
        *out = kw->text.c_str();
    });
}

xisfconv_status xisfconv_keywords_fits_text(const xisfconv_keywords* kw, const char** text, size_t* length) {
    if (!kw) return XISFCONV_ERR_ARGUMENT;
    if (text) *text = "";
    if (length) *length = 0;
    return guarded(kw->state, nullptr, [&] {
        if (!text) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_keywords_fits_text: text is NULL");
        kw->text = fitsCards(kw->cards);
        *text = kw->text.c_str();
        if (length) *length = kw->text.size();
    });
}

// ------------------------------------------------------------------------------------------
// Opening files
// ------------------------------------------------------------------------------------------

xisfconv_status xisfconv_detect_format(xisfconv_context* ctx, const char* path, xisfconv_format* out) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    return guarded(ctx->state, path, [&] {
        if (!path || !out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_detect_format: path or out is NULL");
        mustBeReadable(path);
        if (looksLikeFits(path)) {
            *out = XISFCONV_FORMAT_FITS;
        } else if (looksLikeAsdf(path)) {
            *out = XISFCONV_FORMAT_ASDF;
        } else {
            std::ifstream again(toPath(path), std::ios::binary);
            char signature[8] = {};
            again.read(signature, 8);
            if (again.gcount() != 8 || std::memcmp(signature, "XISF0100", 8) != 0) {
                throw Error("not an XISF, FITS or ASDF file");
            }
            *out = XISFCONV_FORMAT_XISF;
        }
    });
}

xisfconv_status xisfconv_open(xisfconv_context* ctx, const char* path, xisfconv_file** out) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    if (out) *out = nullptr;
    return guarded(ctx->state, path, [&] {
        if (!path || !out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_open: path or out is NULL");
        auto f = std::make_unique<xisfconv_file>();
        f->state = ctx->state;
        f->path = path;
        std::error_code absoluteError;
        const std::filesystem::path absolute = std::filesystem::absolute(toPath(path), absoluteError);
        f->readPath = absoluteError ? std::string(path) : fromPath(absolute);
        const InputFormat kind = detectInputFormat(path);
        if (kind == InputFormat::Fits) {
            f->format = XISFCONV_FORMAT_FITS;
            f->fits = readFits(path, true);
        } else if (kind == InputFormat::Asdf) {
            f->format = XISFCONV_FORMAT_ASDF;
            f->fits = readAsdf(path, true, true);
        } else {
            f->format = XISFCONV_FORMAT_XISF;
            f->xisf = std::make_unique<XisfFile>(path);
        }
        const size_t n = imageCount(f.get());
        f->known.resize(n);
        for (size_t i = 0; i < n; ++i) {
            auto kw = std::make_unique<xisfconv_keywords>();
            kw->state = ctx->state;
            kw->readOnly = true;
            kw->cards = cardsOf(f.get(), i);
            f->keywords.push_back(std::move(kw));
        }
        *out = f.release();
    });
}

void xisfconv_close(xisfconv_file* file) { delete file; }

xisfconv_format xisfconv_file_format(const xisfconv_file* file) { return file ? file->format : XISFCONV_FORMAT_AUTO; }

uint64_t xisfconv_file_size(const xisfconv_file* file) {
    if (!file) return 0;
    return isXisf(file) ? file->xisf->fileSize() : file->fits.fileSize;
}

size_t xisfconv_image_count(const xisfconv_file* file) { return file ? imageCount(file) : 0; }

const char* xisfconv_file_detail(const xisfconv_file* file, const char* name) {
    if (!file || !name) return "";
    if (isXisf(file) && !std::strcmp(name, "version")) return file->xisf->version().c_str();
    if (file->format == XISFCONV_FORMAT_ASDF && !std::strcmp(name, "format")) return file->fits.formatNote.c_str();
    return "";
}

size_t xisfconv_skipped_count(const xisfconv_file* file) { return file && !isXisf(file) ? file->fits.skipped.size() : 0; }

const char* xisfconv_skipped_text(const xisfconv_file* file, size_t index) {
    if (!file || isXisf(file) || index >= file->fits.skipped.size()) return "";
    return file->fits.skipped[index].c_str();
}

xisfconv_status xisfconv_header_text(xisfconv_file* file, const char** text, size_t* length) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    return guarded(file->state, file->path.c_str(), [&] {
        if (!text) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_header_text: text is NULL");
        if (!file->headerRead) {
            if (isXisf(file)) {
                file->headerText = file->xisf->headerXml();
            } else if (file->format == XISFCONV_FORMAT_ASDF) {
                file->headerText = readAsdfTree(file->readPath);
            } else {
                std::string all;
                for (const auto& img : file->fits.images) {
                    all += "HDU " + std::to_string(img.hduIndex) + "\n";
                    for (const auto& k : img.keywords) all += cardLine(k) + "\n";
                }
                file->headerText = std::move(all);
            }
            file->headerRead = true;
        }
        *text = file->headerText.c_str();
        if (length) *length = file->headerText.size();
    });
}

xisfconv_status xisfconv_asdf_tree_text(xisfconv_context* ctx, const char* path, char* buffer, size_t buffer_size,
                                        size_t* size) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    return guarded(ctx->state, path, [&] {
        if (!path) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_asdf_tree_text: path is NULL");
        mustBeReadable(path);
        if (!looksLikeAsdf(path)) fail(XISFCONV_ERR_UNSUPPORTED, "not an ASDF file");
        const std::string tree = readAsdfTree(path);
        if (size) *size = tree.size();
        if (!buffer) return;
        if (buffer_size < tree.size()) fail(XISFCONV_ERR_BUFFER, "the buffer is too small for the tree");
        std::memcpy(buffer, tree.data(), tree.size());
    });
}

xisfconv_status xisfconv_asdf_tree_json(xisfconv_context* ctx, const char* path, char* buffer, size_t buffer_size,
                                        size_t* size) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    return guarded(ctx->state, path, [&] {
        if (!path) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_asdf_tree_json: path is NULL");
        mustBeReadable(path);
        if (!looksLikeAsdf(path)) fail(XISFCONV_ERR_UNSUPPORTED, "--asdf-tree-json needs an ASDF file");
        std::string json;
        yamlJson(*parseYaml(readAsdfTree(path)), 0, json);
        if (size) *size = json.size();
        if (!buffer) return;
        if (buffer_size < json.size()) fail(XISFCONV_ERR_BUFFER, "the buffer is too small for the tree");
        std::memcpy(buffer, json.data(), json.size());
    });
}

// ------------------------------------------------------------------------------------------
// Images
// ------------------------------------------------------------------------------------------

void xisfconv_image_info_init(xisfconv_image_info* info, size_t struct_size) {
    xisfconv_image_info defaults;
    std::memset(static_cast<void*>(&defaults), 0, sizeof defaults);
    defaults.upper_bound = 1;
    defaults.bscale = 1;
    initStruct(info, struct_size, defaults);
}

xisfconv_status xisfconv_image_info_get(const xisfconv_file* file, size_t image, xisfconv_image_info* info) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    return guarded(file->state, file->path.c_str(), [&] {
        if (!info) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_image_info_get: info is NULL");
        if (info->struct_size < sizeof(size_t)) fail(XISFCONV_ERR_ARGUMENT, "info: struct_size is not set (call xisfconv_image_info_init first)");
        checkImage(file, image);
        xisfconv_image_info out;
        xisfconv_image_info_init(&out, sizeof out);
        const std::vector<FitsKeyword>& cards = cardsOf(file, image);
        if (isXisf(file)) {
            const XisfImage& img = file->xisf->images()[image];
            out.width = img.width;
            out.height = img.height;
            out.channels = img.channels;
            out.sample_format = toApi(img.format);
            out.data_known = 1;
            out.lower_bound = img.lowerBound;
            out.upper_bound = img.upperBound;
            out.color_space = img.colorSpace == "Gray" ? XISFCONV_COLOR_GRAY : img.colorSpace == "RGB" ? XISFCONV_COLOR_RGB : XISFCONV_COLOR_OTHER;
            out.row_order = XISFCONV_ROWS_TOP_DOWN;
            out.row_order_declared = 1;
            out.convertible = img.unsupported.empty() ? 1 : 0;
            out.has_icc_profile = img.hasIccProfile ? 1 : 0;
            out.has_display_function = img.displayFunction.present ? 1 : 0;
            out.has_stored_stretch = img.displayFunction.present && !img.displayFunction.isIdentity() ? 1 : 0;
            bool solution = hasKeyword(cards, "CTYPE1");
            for (const auto& p : img.properties)
                if (startsWith(p.id, "PCL:AstrometricSolution:")) solution = true;
            out.has_astrometric_solution = solution ? 1 : 0;
            if (img.cfa.present) {
                out.has_cfa = 1;
                out.cfa_width = img.cfa.width;
                out.cfa_height = img.cfa.height;
                copyText(out.cfa_pattern, sizeof out.cfa_pattern, img.cfa.pattern);
            }
            if (img.resolution.present) {
                out.resolution_unit = img.resolution.unit == "cm" ? 2 : 1;
                out.resolution_x = img.resolution.horizontal;
                out.resolution_y = img.resolution.vertical;
            }
        } else {
            const FitsImage& img = file->fits.images[image];
            const Known& k = file->known[image];
            out.width = img.pixels.width;
            out.height = img.pixels.height;
            out.channels = img.pixels.channels;
            out.sample_format = k.known ? toApi(k.format) : XISFCONV_SAMPLE_AS_STORED;
            out.data_known = k.known ? 1 : 0;
            out.lower_bound = k.lower;
            out.upper_bound = k.upper;
            out.color_space = img.pixels.channels == 3 ? XISFCONV_COLOR_RGB : XISFCONV_COLOR_GRAY;
            out.row_order = img.topDown ? XISFCONV_ROWS_TOP_DOWN : XISFCONV_ROWS_BOTTOM_UP;
            out.row_order_declared = img.hasRowOrder ? 1 : 0;
            out.convertible = 1;
            out.has_astrometric_solution = hasKeyword(cards, "CTYPE1") ? 1 : 0;
            out.bitpix = img.bitpix;
            out.plain_array = img.generic ? 1 : 0;
            out.bscale = img.bscale;
            out.bzero = img.bzero;
            out.source_index = img.hduIndex;
        }
        out.wcs_row_order = isXisf(file) ? XISFCONV_ROWS_BOTTOM_UP : out.row_order;
        const size_t size = std::min(info->struct_size, sizeof out);
        out.struct_size = size;
        std::memcpy(static_cast<void*>(info), &out, size);
    });
}

const char* xisfconv_image_name(const xisfconv_file* file, size_t image) {
    if (!file || image >= imageCount(file)) return "";
    return isXisf(file) ? file->xisf->images()[image].id.c_str() : file->fits.images[image].name.c_str();
}

const char* xisfconv_image_unsupported_reason(const xisfconv_file* file, size_t image) {
    if (!file || !isXisf(file) || image >= imageCount(file)) return "";
    return file->xisf->images()[image].unsupported.c_str();
}

const char* xisfconv_image_detail(const xisfconv_file* file, size_t image, const char* name) {
    if (!file || !name || image >= imageCount(file)) return "";
    auto is = [&](const char* n) { return std::strcmp(name, n) == 0; };
    if (isXisf(file)) {
        const XisfImage& img = file->xisf->images()[image];
        if (is("sampleFormat")) return img.sampleFormatText.c_str();
        if (is("colorSpace")) return img.colorSpace.c_str();
        if (is("pixelStorage")) return img.planar ? "Planar" : "Normal";
        if (is("byteOrder")) return img.bigEndian ? "big" : "little";
        if (is("location")) return img.location.c_str();
        if (is("compression")) return img.compression.c_str();
        if (is("subblocks")) return img.subblocks.c_str();
        if (is("checksum")) return img.checksum.c_str();
        if (is("imageType")) return img.imageType.c_str();
        if (is("orientation")) return img.orientation.c_str();
        if (is("cfaPattern")) return img.cfa.pattern.c_str();
        if (is("cfaName")) return img.cfa.name.c_str();
        if (is("resolutionUnit")) return img.resolution.present ? img.resolution.unit.c_str() : "";
        return "";
    }
    const FitsImage& img = file->fits.images[image];
    if (is("mapping")) return file->known[image].mapping.c_str();
    if (file->format == XISFCONV_FORMAT_FITS) {
        if (is("tileCompression")) return img.tileCompression.c_str();
    } else {
        if (is("source")) return img.source.c_str();
        if (is("storage")) return img.storage.c_str();
    }
    return "";
}

xisfconv_status xisfconv_image_keywords(const xisfconv_file* file, size_t image, const xisfconv_keywords** out) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    if (out) *out = nullptr;
    return guarded(file->state, file->path.c_str(), [&] {
        if (!out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_image_keywords: out is NULL");
        checkImage(file, image);
        *out = file->keywords[image].get();
    });
}

// ------------------------------------------------------------------------------------------
// XISF properties
// ------------------------------------------------------------------------------------------

namespace {
const std::vector<XisfProperty>* propertiesOf(const xisfconv_file* file, size_t image) {
    if (!file || !isXisf(file)) return nullptr;
    if (image == XISFCONV_FILE_PROPERTIES) return &file->xisf->fileProperties();
    if (image >= file->xisf->images().size()) return nullptr;
    return &file->xisf->images()[image].properties;
}

// FITS and ASDF: the XISF properties a file carries from the XISF file it was converted from.
const std::vector<Property>* carriedOf(const xisfconv_file* file, size_t image) {
    if (!file || isXisf(file)) return nullptr;
    if (image == XISFCONV_FILE_PROPERTIES) return &file->fits.properties;
    if (image >= file->fits.images.size()) return nullptr;
    return &file->fits.images[image].properties;
}
}  // namespace

size_t xisfconv_property_count(const xisfconv_file* file, size_t image) {
    if (const auto* carried = carriedOf(file, image)) return carried->size();
    const auto* list = propertiesOf(file, image);
    return list ? list->size() : 0;
}

xisfconv_status xisfconv_property_get(const xisfconv_file* file, size_t image, size_t index, const char** id,
                                      const char** type, const char** value, const char** comment,
                                      int32_t* in_data_block) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    return guarded(file->state, file->path.c_str(), [&] {
        if (const auto* carried = carriedOf(file, image)) {
            if (index >= carried->size()) fail(XISFCONV_ERR_INDEX, "property index " + std::to_string(index) + " out of range");
            const Property& p = (*carried)[index];
            if (id) *id = p.id.c_str();
            if (type) *type = p.type.c_str();
            if (value) *value = p.array ? "" : p.text.c_str();
            if (comment) *comment = p.comment.c_str();
            if (in_data_block) *in_data_block = p.array ? 1 : 0;
            return;
        }
        const auto* list = propertiesOf(file, image);
        if (!list || index >= list->size()) fail(XISFCONV_ERR_INDEX, "property index " + std::to_string(index) + " out of range");
        const XisfProperty& p = (*list)[index];
        if (id) *id = p.id.c_str();
        if (type) *type = p.type.c_str();
        if (value) *value = p.value.c_str();
        if (comment) *comment = p.comment.c_str();
        if (in_data_block) *in_data_block = p.hasBlockData ? 1 : 0;
    });
}

int64_t xisfconv_property_find(const xisfconv_file* file, size_t image, const char* id) {
    if (const auto* carried = carriedOf(file, image)) {
        for (size_t i = 0; id && i < carried->size(); ++i)
            if ((*carried)[i].id == id) return static_cast<int64_t>(i);
        return -1;
    }
    const auto* list = propertiesOf(file, image);
    if (!list || !id) return -1;
    for (size_t i = 0; i < list->size(); ++i)
        if ((*list)[i].id == id) return static_cast<int64_t>(i);
    return -1;
}

const char* xisfconv_property_format(const xisfconv_file* file, size_t image, size_t index) {
    if (const auto* carried = carriedOf(file, image)) return index < carried->size() ? (*carried)[index].format.c_str() : "";
    const auto* list = propertiesOf(file, image);
    return list && index < list->size() ? (*list)[index].format.c_str() : "";
}

xisfconv_status xisfconv_property_read_f64(xisfconv_file* file, size_t image, const char* id, double* values,
                                           size_t capacity, size_t* rows, size_t* columns) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    return guarded(file->state, file->path.c_str(), [&] {
        if (!id) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_property_read_f64: id is NULL");
        if (image != XISFCONV_FILE_PROPERTIES) checkImage(file, image);
        std::vector<double> data;
        size_t r = 0, c = 0;
        // (Vector and Matrix are type names themselves: the short ones for 64-bit floating point)
        auto vectorOrMatrix = [](const std::string& type) {
            return type == "ByteArray" ||
                   (type.size() >= 6 && (type.compare(type.size() - 6, 6, "Vector") == 0 || type.compare(type.size() - 6, 6, "Matrix") == 0));
        };
        auto notNumbers = [&](const std::string& type) {
            fail(XISFCONV_ERR_UNSUPPORTED, std::string("property '") + id + "' is of type " + type + ", which is not read as numbers");
        };
        if (isXisf(file)) {
            const XisfProperty* property = file->xisf->findProperty(image, id);
            if (!property || !vectorOrMatrix(property->type)) {
                fail(XISFCONV_ERR_NOT_FOUND, std::string("no numeric vector or matrix property '") + id + "'");
            }
            if (!isNumericPropertyType(property->type)) notNumbers(property->type);
            if (!file->xisf->readNumericProperty(image, id, data, &r, &c)) {
                fail(XISFCONV_ERR_FORMAT, std::string("the data of property '") + id + "' cannot be read");
            }
        } else {
            // as in an XISF file: the properties of the image first, then those of the file
            const Property* property = nullptr;
            if (const auto* own = carriedOf(file, image)) property = findProperty(*own, id);
            if (!property) property = findProperty(file->fits.properties, id);
            if (!property || !property->array || !vectorOrMatrix(property->type)) {
                fail(XISFCONV_ERR_NOT_FOUND, std::string("no numeric vector or matrix property '") + id + "'");
            }
            if (!isNumericPropertyType(property->type)) notNumbers(property->type);
            if (!propertyNumbers(*property, data)) {
                fail(XISFCONV_ERR_FORMAT, std::string("the data of property '") + id + "' cannot be read");
            }
            const bool matrix = isMatrixPropertyType(property->type);
            r = static_cast<size_t>(matrix ? property->rows : 1);
            c = static_cast<size_t>(matrix ? property->columns : property->rows);
        }
        if (rows) *rows = r;
        if (columns) *columns = c;
        if (!values) return;
        if (capacity < data.size()) {
            fail(XISFCONV_ERR_BUFFER, "the property holds " + std::to_string(data.size()) + " numbers, the buffer " + std::to_string(capacity));
        }
        std::copy(data.begin(), data.end(), values);
    });
}

// ------------------------------------------------------------------------------------------
// Pixels
// ------------------------------------------------------------------------------------------

void xisfconv_read_options_init(xisfconv_read_options* options, size_t struct_size) {
    xisfconv_read_options defaults;
    std::memset(static_cast<void*>(&defaults), 0, sizeof defaults);
    defaults.sample_format = XISFCONV_SAMPLE_AS_STORED;
    defaults.row_order = XISFCONV_ROWS_DEFAULT;
    defaults.verify_checksums = 1;
    defaults.upper_bound = 1;
    initStruct(options, struct_size, defaults);
}

xisfconv_status xisfconv_load_pixels(xisfconv_file* file, size_t image, int32_t verify_checksums) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    return guarded(file->state, file->path.c_str(), [&] {
        checkImage(file, image);
        loadPixels(file, image, verify_checksums != 0);
    });
}

namespace {
// Number of samples of an image, checked for overflow.
uint64_t sampleCount(const xisfconv_file* file, size_t image) {
    uint64_t w, h, c;
    if (isXisf(file)) {
        const XisfImage& img = file->xisf->images()[image];
        w = img.width, h = img.height, c = img.channels;
    } else {
        const PixelBuffer& px = file->fits.images[image].pixels;
        w = px.width, h = px.height, c = px.channels;
    }
    return checkedMul(checkedMul(w, h, "image size"), c, "image size");
}
}  // namespace

xisfconv_status xisfconv_pixels_size(xisfconv_file* file, size_t image, const xisfconv_read_options* options,
                                     uint64_t* size) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    return guarded(file->state, file->path.c_str(), [&] {
        if (!size) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_pixels_size: size is NULL");
        checkImage(file, image);
        const xisfconv_read_options o = optionsFrom(options, xisfconv_read_options_init);
        SampleFormat format;
        if (const auto wanted = wantedFormat(o.sample_format)) {
            format = *wanted;
        } else if (isXisf(file)) {
            format = file->xisf->images()[image].format;
        } else {
            loadPixels(file, image, o.verify_checksums != 0);
            format = file->known[image].format;
        }
        *size = checkedMul(sampleCount(file, image), sampleBytes(format), "image size");
    });
}

xisfconv_status xisfconv_read_pixels(xisfconv_file* file, size_t image, const xisfconv_read_options* options,
                                     void* buffer, uint64_t buffer_size) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    return guarded(file->state, file->path.c_str(), [&] {
        if (!buffer) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_read_pixels: buffer is NULL");
        checkImage(file, image);
        const xisfconv_read_options o = optionsFrom(options, xisfconv_read_options_init);
        checkRowOrder(o.row_order);
        const std::optional<SampleFormat> wanted = wantedFormat(o.sample_format);
        if (o.use_bounds && !(o.upper_bound > o.lower_bound)) fail(XISFCONV_ERR_ARGUMENT, "the upper bound must be above the lower bound");
        const uint64_t samples = sampleCount(file, image);
        auto checkBuffer = [&](SampleFormat format) {
            const uint64_t needed = checkedMul(samples, sampleBytes(format), "image size");
            if (buffer_size < needed) {
                fail(XISFCONV_ERR_BUFFER, "the image needs " + std::to_string(needed) + " bytes, the buffer has " + std::to_string(buffer_size));
            }
        };

        PixelBuffer px;
        double lower = 0, upper = 1;
        bool storedTopDown = true;
        if (isXisf(file)) {
            const XisfImage& img = file->xisf->images()[image];
            if (img.unsupported.empty()) checkBuffer(wanted ? *wanted : img.format);   // before the work of reading
            px = file->xisf->readPixels(image, o.verify_checksums != 0);
            lower = img.lowerBound;
            upper = img.upperBound;
        } else {
            if (wanted) checkBuffer(*wanted);
            loadPixels(file, image, o.verify_checksums != 0);
            const Known& k = file->known[image];
            checkBuffer(wanted ? *wanted : k.format);
            px = std::move(file->loadedImage.pixels);
            storedTopDown = file->loadedImage.topDown;
            file->loaded.reset();
            file->loadedImage = FitsImage();
            lower = k.lower;
            upper = k.upper;
        }
        if (o.use_bounds) {
            lower = o.lower_bound;
            upper = o.upper_bound;
        }
        if (wanted && *wanted != px.format) convertSampleFormat(px, *wanted, lower, upper);
        const bool wantTopDown = o.row_order == XISFCONV_ROWS_DEFAULT ? storedTopDown : o.row_order == XISFCONV_ROWS_TOP_DOWN;
        if (wantTopDown != storedTopDown) flipVertical(px);
        if (px.data.size() > buffer_size) fail(XISFCONV_ERR_BUFFER, "the buffer is too small for the image");
        if (!px.data.empty()) std::memcpy(buffer, px.data.data(), px.data.size());
    });
}

xisfconv_status xisfconv_read_icc_profile(xisfconv_file* file, size_t image, void* buffer, size_t buffer_size,
                                          size_t* size) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    return guarded(file->state, file->path.c_str(), [&] {
        checkImage(file, image);
        if (!isXisf(file) || !file->xisf->images()[image].hasIccProfile) fail(XISFCONV_ERR_NOT_FOUND, "the image has no ICC profile");
        if (!file->iccImage || *file->iccImage != image) {
            file->iccImage.reset();
            file->icc = file->xisf->readIccProfile(image, true);
            file->iccImage = image;
        }
        if (size) *size = file->icc.size();
        if (!buffer) return;
        if (buffer_size < file->icc.size()) {
            fail(XISFCONV_ERR_BUFFER, "the ICC profile has " + std::to_string(file->icc.size()) + " bytes, the buffer " + std::to_string(buffer_size));
        }
        std::memcpy(buffer, file->icc.data(), file->icc.size());
    });
}

// ------------------------------------------------------------------------------------------
// Stretch
// ------------------------------------------------------------------------------------------

xisfconv_status xisfconv_stored_stretch(const xisfconv_file* file, size_t image, xisfconv_stretch_params* params,
                                        size_t capacity, size_t* count) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    return guarded(file->state, file->path.c_str(), [&] {
        checkImage(file, image);
        if (!isXisf(file) || !file->xisf->images()[image].displayFunction.present) {
            fail(XISFCONV_ERR_NOT_FOUND, "image " + std::to_string(image) + " has no saved STF (DisplayFunction)");
        }
        const XisfImage& img = file->xisf->images()[image];
        const DisplayFunction& df = img.displayFunction;
        const size_t n = img.colorSpace == "Gray" ? 1 : 3;
        if (count) *count = n;
        if (!params) return;
        if (capacity < n) fail(XISFCONV_ERR_BUFFER, "the image has " + std::to_string(n) + " stretch parameter sets, the buffer " + std::to_string(capacity));
        for (size_t c = 0; c < n; ++c) params[c] = {df.s[c], df.m[c], df.h[c], df.l[c], df.r[c]};
    });
}

namespace {
PixelBuffer bufferFrom(const void* pixels, uint64_t width, uint64_t height, uint64_t channels, xisfconv_sample_format format) {
    if (!pixels) fail(XISFCONV_ERR_ARGUMENT, "pixels is NULL");
    PixelBuffer px;
    if (!fromApi(format, px.format)) fail(XISFCONV_ERR_ARGUMENT, "unknown sample format " + std::to_string(format));
    if (width == 0 || height == 0 || channels == 0) fail(XISFCONV_ERR_ARGUMENT, "an image needs a width, a height and at least one channel");
    px.width = width;
    px.height = height;
    px.channels = channels;
    uint64_t bytes = 0;
    try {
        bytes = checkedMul(checkedMul(checkedMul(width, height, "image size"), channels, "image size"), sampleBytes(px.format),
                           "image size");
    } catch (const Error& e) {
        fail(XISFCONV_ERR_ARGUMENT, e.what());
    }
    if (bytes > static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
        fail(XISFCONV_ERR_ARGUMENT, "image too large for this platform");
    }
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    px.data.assign(src, src + static_cast<size_t>(bytes));
    return px;
}
}  // namespace

xisfconv_status xisfconv_auto_stretch(xisfconv_context* ctx, const void* pixels, uint64_t width, uint64_t height,
                                      uint64_t channels, xisfconv_sample_format sample_format, double lower_bound,
                                      double upper_bound, size_t color_channels, int32_t linked,
                                      xisfconv_stretch_params* params) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    return guarded(ctx->state, nullptr, [&] {
        if (!params) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_auto_stretch: params is NULL");
        if (color_channels == 0 || color_channels > channels) fail(XISFCONV_ERR_ARGUMENT, "color_channels must be between 1 and the number of channels");
        if (!(upper_bound > lower_bound)) fail(XISFCONV_ERR_ARGUMENT, "the upper bound must be above the lower bound");
        const PixelBuffer px = bufferFrom(pixels, width, height, channels, sample_format);
        const std::vector<StretchParams> found = autoStretch(px, lower_bound, upper_bound, color_channels, linked != 0);
        for (size_t c = 0; c < found.size() && c < color_channels; ++c) {
            params[c] = {found[c].shadows, found[c].midtones, found[c].highlights, found[c].low, found[c].high};
        }
    });
}

xisfconv_status xisfconv_apply_stretch(xisfconv_context* ctx, const void* pixels, uint64_t width, uint64_t height,
                                       uint64_t channels, xisfconv_sample_format sample_format, double lower_bound,
                                       double upper_bound, const xisfconv_stretch_params* params, size_t param_count,
                                       float* out) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    return guarded(ctx->state, nullptr, [&] {
        if (!params || !out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_apply_stretch: params or out is NULL");
        if (param_count == 0 || param_count > channels) fail(XISFCONV_ERR_ARGUMENT, "param_count must be between 1 and the number of channels");
        if (!(upper_bound > lower_bound)) fail(XISFCONV_ERR_ARGUMENT, "the upper bound must be above the lower bound");
        PixelBuffer px = bufferFrom(pixels, width, height, channels, sample_format);
        std::vector<StretchParams> list;
        for (size_t c = 0; c < param_count; ++c) {
            list.push_back({params[c].shadows, params[c].midtones, params[c].highlights, params[c].low, params[c].high});
        }
        applyStretch(px, list, lower_bound, upper_bound);
        std::memcpy(out, px.data.data(), px.data.size());
    });
}

// ------------------------------------------------------------------------------------------
// Astrometry
// ------------------------------------------------------------------------------------------

xisfconv_status xisfconv_wcs_keywords(xisfconv_file* file, size_t image, xisfconv_row_order row_order, int32_t sip_order,
                                      xisfconv_keywords** out, const char** fit_summary) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    if (out) *out = nullptr;
    return guarded(file->state, file->path.c_str(), [&] {
        if (!out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_wcs_keywords: out is NULL");
        checkImage(file, image);
        checkRowOrder(row_order);
        if (sip_order != 0 && (sip_order < 2 || sip_order > 7)) fail(XISFCONV_ERR_ARGUMENT, "--sip-order must be 0 (off) or 2..7");
        const bool wantTopDown = row_order == XISFCONV_ROWS_TOP_DOWN;
        auto kw = std::make_unique<xisfconv_keywords>();
        kw->state = file->state;
        const std::vector<FitsKeyword>& cards = cardsOf(file, image);
        if (hasKeyword(cards, "CTYPE1")) {
            for (const auto& k : cards)
                if (isWcsKeyword(k.name)) kw->cards.push_back(k);
            // WCS keywords in an XISF file follow the FITS bottom-up convention (as PixInsight writes
            // them); in FITS and ASDF they describe the rows as they are stored.
            const bool describesTopDown = isXisf(file) ? false : file->fits.images[image].topDown;
            const uint64_t height = isXisf(file) ? file->xisf->images()[image].height : file->fits.images[image].pixels.height;
            if (describesTopDown != wantTopDown) flipWcsRowOrder(kw->cards, height);
        } else {
            WcsResult wcs;
            if (!isXisf(file) || !astrometricSolutionToWcs(*file->xisf, image, !wantTopDown, sip_order, wcs)) {
                fail(XISFCONV_ERR_NOT_FOUND, "image " + std::to_string(image) + " has no astrometric solution");
            }
            kw->cards = std::move(wcs.keywords);
            kw->summary = std::move(wcs.summary);
        }
        if (fit_summary) *fit_summary = kw->summary.c_str();
        *out = kw.release();
    });
}

xisfconv_status xisfconv_fits_keywords(xisfconv_file* file, size_t image, xisfconv_row_order row_order,
                                       int32_t property_keywords, int32_t wcs, int32_t sip_order, xisfconv_keywords** out,
                                       const char** fit_summary) {
    if (!file) return XISFCONV_ERR_ARGUMENT;
    if (out) *out = nullptr;
    if (fit_summary) *fit_summary = "";
    return guarded(file->state, file->path.c_str(), [&] {
        if (!out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_fits_keywords: out is NULL");
        checkImage(file, image);
        checkRowOrder(row_order);
        if (sip_order != 0 && (sip_order < 2 || sip_order > 7)) fail(XISFCONV_ERR_ARGUMENT, "--sip-order must be 0 (off) or 2..7");
        const bool wantTopDown = row_order == XISFCONV_ROWS_TOP_DOWN;
        auto kw = std::make_unique<xisfconv_keywords>();
        kw->state = file->state;
        if (isXisf(file)) {
            kw->cards = xisfImageFitsKeywords(*file->xisf, image, !wantTopDown, property_keywords != 0, wcs != 0, sip_order,
                                              &kw->summary);
        } else {
            const FitsImage& img = file->fits.images[image];
            kw->cards = img.keywords;
            if (img.topDown != wantTopDown) flipKeywordRows(kw->cards, img.pixels.height);
        }
        if (fit_summary) *fit_summary = kw->summary.c_str();
        *out = kw.release();
    });
}

xisfconv_status xisfconv_wcs_flip_rows(xisfconv_keywords* kw, uint64_t image_height) {
    if (!kw) return XISFCONV_ERR_ARGUMENT;
    return guarded(kw->state, nullptr, [&] {
        if (kw->readOnly) fail(XISFCONV_ERR_ARGUMENT, "the keyword list belongs to a file and cannot be changed");
        if (image_height == 0) fail(XISFCONV_ERR_ARGUMENT, "the image height is needed");
        if (!flipWcsRowOrder(kw->cards, image_height)) fail(XISFCONV_ERR_NOT_FOUND, "the list holds no WCS keywords");
    });
}

// ------------------------------------------------------------------------------------------
// Converting
// ------------------------------------------------------------------------------------------

void xisfconv_convert_options_init(xisfconv_convert_options* options, size_t struct_size) {
    xisfconv_convert_options defaults;
    std::memset(static_cast<void*>(&defaults), 0, sizeof defaults);
    defaults.output_format = XISFCONV_FORMAT_AUTO;
    defaults.sample_format = XISFCONV_SAMPLE_AS_STORED;
    defaults.image = XISFCONV_ALL_IMAGES;
    defaults.stretch = XISFCONV_STRETCH_NONE;
    defaults.codec = XISFCONV_CODEC_NONE;
    defaults.checksum = XISFCONV_CHECKSUM_NONE;
    defaults.subblock_size = 1u << 30;
    defaults.row_order = XISFCONV_ROWS_DEFAULT;
    defaults.property_keywords = 1;
    defaults.wcs = 1;
    defaults.sip_order = 3;
    defaults.verify_checksums = 1;
    defaults.upper_bound = 1;
    defaults.properties = 1;
    initStruct(options, struct_size, defaults);
}

xisfconv_status xisfconv_convert(xisfconv_context* ctx, const char* input, const char* output,
                                 const xisfconv_convert_options* options) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    return guarded(ctx->state, input, [&] {
        if (!input || !output) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_convert: input or output is NULL");
        const xisfconv_convert_options o = optionsFrom(options, xisfconv_convert_options_init);
        ConvertOptions c;
        c.stretch = stretchFrom(o.stretch);
        c.bits = wantedFormat(o.sample_format);
        c.imageIndex = imageChoice(o.image);
        codecFrom(o.codec, c);
        c.checksum = checksumName(o.checksum, false);
        if (o.subblock_size == 0) fail(XISFCONV_ERR_ARGUMENT, "invalid subblock size");
        c.subblockSize = o.subblock_size;
        checkRowOrder(o.row_order);
        c.bottomUp = o.row_order != XISFCONV_ROWS_TOP_DOWN;
        c.rowOrderGiven = o.row_order != XISFCONV_ROWS_DEFAULT;
        if (o.use_bounds) {
            if (!(o.upper_bound > o.lower_bound)) fail(XISFCONV_ERR_ARGUMENT, "--bounds expects lo:hi with hi > lo, e.g. 0:65535");
            c.bounds = std::make_pair(o.lower_bound, o.upper_bound);
        }
        c.propertyKeywords = o.property_keywords != 0;
        c.verify = o.verify_checksums != 0;
        c.wcs = o.wcs != 0;
        if (o.sip_order != 0 && (o.sip_order < 2 || o.sip_order > 7)) fail(XISFCONV_ERR_ARGUMENT, "--sip-order must be 0 (off) or 2..7");
        c.sipOrder = o.sip_order;
        c.force = o.overwrite != 0;
        c.properties = o.properties != 0;
        const Format format = outputFormat(o.output_format, output);
        mustBeReadable(input);
        const InputFormat kind = detectInputFormat(input);
        if (kind == InputFormat::Xisf) convertXisfFile(input, output, format, c);
        else convertFitsOrAsdfFile(input, kind, output, format, c);
    });
}

// ------------------------------------------------------------------------------------------
// Rewriting XISF files
// ------------------------------------------------------------------------------------------

void xisfconv_rewrite_options_init(xisfconv_rewrite_options* options, size_t struct_size) {
    xisfconv_rewrite_options defaults;
    std::memset(static_cast<void*>(&defaults), 0, sizeof defaults);
    defaults.codec = XISFCONV_CODEC_KEEP;
    defaults.checksum = XISFCONV_CHECKSUM_KEEP;
    defaults.image = XISFCONV_ALL_IMAGES;
    defaults.verify_input = 1;
    defaults.read_back = 1;
    defaults.subblock_size = 1u << 30;
    initStruct(options, struct_size, defaults);
}

void xisfconv_rewrite_result_init(xisfconv_rewrite_result* result, size_t struct_size) {
    xisfconv_rewrite_result defaults;
    std::memset(static_cast<void*>(&defaults), 0, sizeof defaults);
    initStruct(result, struct_size, defaults);
}

xisfconv_status xisfconv_rewrite(xisfconv_context* ctx, const char* input, const char* output,
                                 const xisfconv_rewrite_options* options, xisfconv_rewrite_result* result) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    return guarded(ctx->state, input, [&] {
        if (!input || !output) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_rewrite: input or output is NULL");
        const xisfconv_rewrite_options o = optionsFrom(options, xisfconv_rewrite_options_init);
        fillResult(rewriteXisfFile(input, output, false, o.overwrite != 0, rewriteOptionsFrom(o)), result);
    });
}

xisfconv_status xisfconv_rewrite_in_place(xisfconv_context* ctx, const char* path, const xisfconv_rewrite_options* options,
                                          xisfconv_rewrite_result* result) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    return guarded(ctx->state, path, [&] {
        if (!path) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_rewrite_in_place: path is NULL");
        const xisfconv_rewrite_options o = optionsFrom(options, xisfconv_rewrite_options_init);
        fillResult(rewriteXisfFile(path, std::string(), true, o.overwrite != 0, rewriteOptionsFrom(o)), result);
    });
}

xisfconv_status xisfconv_stored_as_requested(xisfconv_context* ctx, const char* path, const xisfconv_rewrite_options* options,
                                             int32_t* out) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    return guarded(ctx->state, path, [&] {
        if (!path || !out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_stored_as_requested: path or out is NULL");
        const xisfconv_rewrite_options o = optionsFrom(options, xisfconv_rewrite_options_init);
        *out = xisfStoredAsRequested(path, rewriteOptionsFrom(o)) ? 1 : 0;
    });
}

// ------------------------------------------------------------------------------------------
// Verifying
// ------------------------------------------------------------------------------------------

xisfconv_status xisfconv_verify(xisfconv_context* ctx, const char* path, xisfconv_report** out) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    if (out) *out = nullptr;
    return guarded(ctx->state, path, [&] {
        if (!path || !out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_verify: path or out is NULL");
        auto report = std::make_unique<xisfconv_report>();
        // Whatever goes wrong while reading the file is a finding about the file.
        try {
            if (looksLikeFits(path)) {
                report->format = XISFCONV_FORMAT_FITS;
                report->report = verifyFits(path);
            } else if (looksLikeAsdf(path)) {
                report->format = XISFCONV_FORMAT_ASDF;
                report->report = verifyAsdf(path);
            } else {
                report->report = verifyXisf(path);
            }
        } catch (const Error& e) {
            if (e.kind == ErrorKind::Cancelled) throw;
            report->report.problems.push_back(e.what());
        } catch (const std::bad_alloc&) {
            report->report.problems.push_back("out of memory");
        } catch (const std::exception& e) {
            report->report.problems.push_back(e.what());
        }
        *out = report.release();
    });
}

void xisfconv_report_free(xisfconv_report* report) { delete report; }

xisfconv_verdict xisfconv_report_verdict(const xisfconv_report* report) {
    if (!report || !report->report.problems.empty()) return XISFCONV_VERDICT_FAILED;
    return report->report.notChecked.empty() ? XISFCONV_VERDICT_OK : XISFCONV_VERDICT_NOT_FULLY_CHECKED;
}

xisfconv_format xisfconv_report_format(const xisfconv_report* report) { return report ? report->format : XISFCONV_FORMAT_AUTO; }
const char* xisfconv_report_summary(const xisfconv_report* report) { return report ? report->report.summary.c_str() : ""; }
size_t xisfconv_report_verified(const xisfconv_report* report) { return report ? report->report.verified : 0; }
size_t xisfconv_report_unchecked(const xisfconv_report* report) { return report ? report->report.unchecked : 0; }
size_t xisfconv_report_problem_count(const xisfconv_report* report) { return report ? report->report.problems.size() : 0; }

const char* xisfconv_report_problem(const xisfconv_report* report, size_t index) {
    return report && index < report->report.problems.size() ? report->report.problems[index].c_str() : "";
}

size_t xisfconv_report_not_checked_count(const xisfconv_report* report) { return report ? report->report.notChecked.size() : 0; }

const char* xisfconv_report_not_checked(const xisfconv_report* report, size_t index) {
    return report && index < report->report.notChecked.size() ? report->report.notChecked[index].c_str() : "";
}

// ------------------------------------------------------------------------------------------
// Writing images from memory
// ------------------------------------------------------------------------------------------

void xisfconv_image_init(xisfconv_image* image, size_t struct_size) {
    xisfconv_image defaults;
    std::memset(static_cast<void*>(&defaults), 0, sizeof defaults);
    defaults.sample_format = XISFCONV_SAMPLE_AS_STORED;
    defaults.channels = 1;
    defaults.row_order = XISFCONV_ROWS_DEFAULT;
    defaults.upper_bound = 1;
    initStruct(image, struct_size, defaults);
}

void xisfconv_write_options_init(xisfconv_write_options* options, size_t struct_size) {
    xisfconv_write_options defaults;
    std::memset(static_cast<void*>(&defaults), 0, sizeof defaults);
    defaults.format = XISFCONV_FORMAT_AUTO;
    defaults.codec = XISFCONV_CODEC_NONE;
    defaults.checksum = XISFCONV_CHECKSUM_NONE;
    defaults.row_order = XISFCONV_ROWS_DEFAULT;
    defaults.subblock_size = 1u << 30;
    defaults.wcs = 1;
    initStruct(options, struct_size, defaults);
}

xisfconv_status xisfconv_writer_new(xisfconv_context* ctx, const char* path, const xisfconv_write_options* options,
                                    xisfconv_writer** out) {
    if (!ctx) return XISFCONV_ERR_ARGUMENT;
    if (out) *out = nullptr;
    return guarded(ctx->state, path, [&] {
        if (!path || !*path || !out) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_writer_new: path or out is NULL");
        const xisfconv_write_options o = optionsFrom(options, xisfconv_write_options_init);
        auto w = std::make_unique<xisfconv_writer>();
        w->state = ctx->state;
        w->path = path;
        w->format = outputFormat(o.format, path);
        codecFrom(o.codec, w->options);
        w->options.checksum = checksumName(o.checksum, false);
        if (o.subblock_size == 0) fail(XISFCONV_ERR_ARGUMENT, "invalid subblock size");
        w->options.subblockSize = o.subblock_size;
        w->options.wcs = o.wcs != 0;
        w->options.force = o.overwrite != 0;
        checkRowOrder(o.row_order);
        w->rowOrder = o.row_order;
        *out = w.release();
    });
}

xisfconv_status xisfconv_writer_add_image(xisfconv_writer* writer, const xisfconv_image* image) {
    if (!writer) return XISFCONV_ERR_ARGUMENT;
    return guarded(writer->state, writer->path.c_str(), [&] {
        if (!image) fail(XISFCONV_ERR_ARGUMENT, "xisfconv_writer_add_image: image is NULL");
        const xisfconv_image in = optionsFrom(image, xisfconv_image_init);
        checkRowOrder(in.row_order);
        FitsImage img;
        img.pixels = bufferFrom(in.pixels, in.width, in.height, in.channels, in.sample_format);
        img.hduIndex = writer->images.images.size();
        img.hasData = true;
        img.hasRowOrder = true;
        img.topDown = in.row_order != XISFCONV_ROWS_BOTTOM_UP;
        if (in.name) img.name = in.name;
        if (in.keywords) {
            // The cards that describe how a FITS file stores its data say nothing about a buffer
            // (a header taken over from a FITS file brings them along): the writer sets its own.
            for (const auto& card : in.keywords->cards)
                if (!isReservedFitsKeyword(card.name)) img.keywords.push_back(card);
        }
        checkRowOrder(in.wcs_row_order);
        if (in.wcs_row_order != XISFCONV_ROWS_DEFAULT && (in.wcs_row_order == XISFCONV_ROWS_TOP_DOWN) != img.topDown) {
            // the WCS keywords count rows from the other end than the buffer does
            flipWcsRowOrder(img.keywords, img.pixels.height);
        }
        if (in.icc_profile && in.icc_profile_size) {
            const uint8_t* icc = static_cast<const uint8_t*>(in.icc_profile);
            img.iccProfile.assign(icc, icc + in.icc_profile_size);
        }
        if (in.use_bounds) {
            if (!(in.upper_bound > in.lower_bound)) fail(XISFCONV_ERR_ARGUMENT, "the upper bound must be above the lower bound");
            img.bounds = std::make_pair(in.lower_bound, in.upper_bound);
        }
        updateFloatRange(img);
        writer->images.images.push_back(std::move(img));
    });
}

xisfconv_status xisfconv_writer_finish(xisfconv_writer* writer) {
    if (!writer) return XISFCONV_ERR_ARGUMENT;
    const std::unique_ptr<xisfconv_writer> owned(writer);
    return guarded(writer->state, writer->path.c_str(), [&] {
        if (writer->images.images.empty()) fail(XISFCONV_ERR_ARGUMENT, "no images to write");
        if (writer->format == Format::Fits || writer->format == Format::Asdf) {
            // FITS and ASDF keep the rows as they are handed over: put them in the order asked for.
            const bool storeTopDown = writer->rowOrder == XISFCONV_ROWS_TOP_DOWN;
            for (FitsImage& img : writer->images.images)
                if (img.topDown != storeTopDown) flipImageRows(img);
        }
        ImageSetOrigin origin;
        origin.defaultName = fromPath(toPath(writer->path).stem());
        origin.notes = false;
        writeImageSet(writer->images, origin, writer->path, writer->format, writer->options);
    });
}

void xisfconv_writer_discard(xisfconv_writer* writer) { delete writer; }
