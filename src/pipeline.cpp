// Converting whole files: XISF to FITS, ASDF, TIFF or PNG; FITS and ASDF to XISF, to each other,
// or to TIFF or PNG; and rewriting an XISF file, also in place.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "asdf.hpp"
#include "codecs.hpp"
#include "convert.hpp"
#include "dng.hpp"
#include "fits.hpp"
#include "fitsread.hpp"
#include "png.hpp"
#include "tiff.hpp"
#include "wcs.hpp"
#include "xisf.hpp"
#include "xisfwrite.hpp"

namespace fs = std::filesystem;

namespace xisfconv {

namespace {

bool hasKeyword(const std::vector<FitsKeyword>& kw, const std::string& name) {
    for (const auto& k : kw)
        if (toUpper(trim(k.name)) == name) return true;
    return false;
}

FitsKeyword* findKeyword(std::vector<FitsKeyword>& kw, const std::string& name) {
    for (auto& k : kw)
        if (toUpper(trim(k.name)) == name) return &k;
    return nullptr;
}
const FitsKeyword* findKeyword(const std::vector<FitsKeyword>& kw, const std::string& name) {
    for (const auto& k : kw)
        if (toUpper(trim(k.name)) == name) return &k;
    return nullptr;
}

// Converts an ISO 8601 time point to a FITS DATE-OBS value (UTC, no zone designator).
bool isoToFitsDate(std::string t, std::string& out) {
    t = trim(t);
    if (t.size() < 10) return false;
    if (t.back() == 'Z') t.pop_back();
    const size_t tpos = t.find('T');
    if (tpos != std::string::npos) {
        const size_t zone = t.find_first_of("+-", tpos);
        if (zone != std::string::npos) {
            const std::string offset = t.substr(zone + 1);
            if (offset != "00:00" && offset != "0000" && offset != "00") return false;
            t = t.substr(0, zone);
        }
    }
    out = t;
    return true;
}

// Reorders the rows of a CFA pattern after a vertical flip of an image of the given height.
std::string flipPatternRows(const std::string& pattern, int pw, int ph, uint64_t imageHeight) {
    if (pw <= 0 || ph <= 0 || pattern.size() != static_cast<size_t>(pw * ph)) return pattern;
    std::string out(pattern.size(), ' ');
    for (int j = 0; j < ph; ++j) {
        const uint64_t h = static_cast<uint64_t>(ph);   // (row imageHeight - 1 - j of the pattern, also where j > imageHeight - 1)
        const int src = static_cast<int>(((imageHeight - 1) % h + h - static_cast<uint64_t>(j)) % h);
        out.replace(static_cast<size_t>(j * pw), static_cast<size_t>(pw), pattern, static_cast<size_t>(src * pw),
                    static_cast<size_t>(pw));
    }
    return out;
}

void addPropertyKeywords(const XisfFile& file, size_t index, std::vector<FitsKeyword>& kw) {
    const XisfImage& img = file.images()[index];
    const char* comment = " (from XISF property)";
    struct Map { const char* prop; const char* key; bool numeric; double scale; const char* what; };
    static const Map maps[] = {
        {"Observation:Object:Name", "OBJECT", false, 1, "name of observed object"},
        {"Instrument:ExposureTime", "EXPTIME", true, 1, "exposure time [s]"},
        {"Instrument:Telescope:Name", "TELESCOP", false, 1, "telescope"},
        {"Instrument:Camera:Name", "INSTRUME", false, 1, "camera"},
        {"Instrument:Filter:Name", "FILTER", false, 1, "filter"},
        {"Instrument:Sensor:Temperature", "CCD-TEMP", true, 1, "sensor temperature [C]"},
        {"Instrument:Sensor:XPixelSize", "XPIXSZ", true, 1, "pixel width [um]"},
        {"Instrument:Sensor:YPixelSize", "YPIXSZ", true, 1, "pixel height [um]"},
        {"Instrument:Telescope:FocalLength", "FOCALLEN", true, 1000, "focal length [mm]"},
        {"Instrument:Telescope:Aperture", "APTDIA", true, 1000, "aperture diameter [mm]"},
    };
    for (const auto& m : maps) {
        if (hasKeyword(kw, m.key)) continue;
        const XisfProperty* p = file.findProperty(index, m.prop);
        if (!p || p->hasBlockData || trim(p->value).empty()) continue;
        std::string value;
        if (m.numeric) {
            double v;
            if (!parseDouble(p->value, v)) continue;
            value = fitsReal(v * m.scale);
        } else {
            value = fitsString(trim(p->value));
        }
        kw.push_back({m.key, value, std::string(m.what) + comment});
    }
    if (!hasKeyword(kw, "DATE-OBS")) {
        const XisfProperty* p = file.findProperty(index, "Observation:Time:Start");
        std::string date;
        if (p && !p->hasBlockData && isoToFitsDate(p->value, date)) {
            kw.push_back({"DATE-OBS", fitsString(date), std::string("observation start time (UTC)") + comment});
        }
    }
    if (img.cfa.present && img.channels == 1 && !hasKeyword(kw, "BAYERPAT") && !img.cfa.pattern.empty() &&
        img.cfa.pattern.find_first_not_of("RGB") == std::string::npos) {
        kw.push_back({"BAYERPAT", fitsString(img.cfa.pattern), "CFA pattern (from XISF ColorFilterArray)"});
    }
    if (!img.imageType.empty() && !hasKeyword(kw, "IMAGETYP")) {
        kw.push_back({"IMAGETYP", fitsString(img.imageType), "type of image (from XISF imageType)"});
    }
}

// --bin and --resize make a picture to look at; an image that is data keeps its pixels.
void downsampleIsForPictures(const ConvertOptions& opt, Format format) {
    if (opt.downsample.any() && format != Format::Tiff && format != Format::Png) {
        throw Error("--bin and --resize make a smaller picture: they are for TIFF and PNG output", ErrorKind::Argument);
    }
}

// --level and --no-shuffle are of XISF's compression: for XISF output, and with a codec.
void xisfStorageIsForXisf(const ConvertOptions& opt, Format format) {
    if (opt.level == 0 && opt.shuffle) return;
    const std::string what = opt.level != 0 ? "a compression level (--level)" : "byte shuffling off (--no-shuffle)";
    if (format != Format::Xisf) {
        throw Error(what + " is for XISF output (and XISF -> XISF)", ErrorKind::Argument);
    }
    if (!opt.compress) throw Error(what + " needs a codec that compresses (--codec or --compress)", ErrorKind::Argument);
    // (a level the codec does not have is said before anything is read)
    if (opt.level != 0) xisfCompress(!opt.codec.empty() ? opt.codec : (zstdAvailable() ? "zstd" : "zlib"), nullptr, 0, opt.level);
}

// --debayer makes a colour picture of the mosaic of a one-shot colour camera; data keeps its mosaic.
void debayerIsForPictures(const ConvertOptions& opt, Format format) {
    if (opt.debayer && format != Format::Tiff && format != Format::Png) {
        throw Error("--debayer makes a colour picture: it is for TIFF and PNG output (XISF, FITS and ASDF keep the "
                    "mosaic, with its pattern, for calibration and stacking)", ErrorKind::Argument);
    }
}

// The 2 x 2 colour filter pattern of an image, relative to its first pixel as stored: from the
// pattern the file states for it (XISF's ColorFilterArray, DNG), else from BAYERPAT with
// XBAYROFF and YBAYROFF (the pattern begins that many pixels to the left and up). "" if there is
// none that --debayer can use, and `why` says why.
std::string bayerPatternOf(const std::vector<FitsKeyword>& keywords, const std::string& cfa, int cfaWidth, int cfaHeight,
                           std::string& why) {
    std::string pattern;
    int xOffset = 0, yOffset = 0;
    if (!cfa.empty()) {
        if (cfaWidth != 2 || cfaHeight != 2) {
            why = "its colour filter pattern is " + std::to_string(cfaWidth) + " x " + std::to_string(cfaHeight) + " (" + cfa +
                  "), and only 2 x 2 patterns are interpolated";
            return {};
        }
        pattern = cfa;
    } else if (const FitsKeyword* bp = findKeyword(keywords, "BAYERPAT")) {
        pattern = toUpper(trim(fitsUnquote(bp->value)));
        for (const char* key : {"XBAYROFF", "YBAYROFF"}) {
            if (const FitsKeyword* k = findKeyword(keywords, key)) {
                double v = 0;
                if (!parseDouble(k->value, v) || v != std::floor(v) || std::fabs(v) > 1e9) {
                    why = std::string(key) + " = " + trim(k->value) + " is not a whole number";
                    return {};
                }
                (key[0] == 'X' ? xOffset : yOffset) = static_cast<int>(std::fabs(std::fmod(v, 2.0)));
            }
        }
    } else {
        why = "it has no colour filter pattern (no BAYERPAT keyword)";
        return {};
    }
    if (pattern.size() != 4 || pattern.find_first_not_of("RGB") != std::string::npos || pattern.find('R') == std::string::npos ||
        pattern.find('G') == std::string::npos || pattern.find('B') == std::string::npos) {
        why = "its colour filter pattern \"" + pattern + "\" is not a 2 x 2 pattern of R, G and B";
        return {};
    }
    std::string shifted(4, ' ');
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) shifted[static_cast<size_t>(2 * y + x)] = pattern[static_cast<size_t>(2 * ((y + yOffset) % 2) + (x + xOffset) % 2)];
    return shifted;
}

// --debayer on one image: the mosaic becomes RGB, or a warning says why it stays as it is.
// `pattern` is that of the pixels as they are now. Returns true if the image is RGB now.
bool debayerForPicture(Source& px, const std::string& pattern, const std::string& why, const std::string& label) {
    std::string reason = why;
    if (px->channels != 1) reason = "it has " + std::to_string(px->channels) + " channels, a mosaic has one";
    else if (!pattern.empty() && (px->width < 2 || px->height < 2)) reason = "it is smaller than 2 x 2 pixels";
    if (px->channels != 1 || pattern.empty() || px->width < 2 || px->height < 2) {
        warn(label + ": not debayered: " + reason);
        return false;
    }
    px = debayeredSource(px, pattern);
    info(label + ": debayered (" + pattern + ", bilinear)");
    return true;
}

// Makes the image the picture that was asked for, if one was. Returns what happened, for the
// notes, and how many pixels of the picture there are for one of the image in width and in
// height (what a resolution in pixels per inch is to be multiplied by).
std::string makeSmaller(Source& px, const Downsample& how, double& perPixelX, double& perPixelY) {
    const uint64_t fullWidth = px->width, fullHeight = px->height;
    perPixelX = perPixelY = 1;
    if (!how.any()) return {};
    const DownsampledSize size = downsampledSize(how, px->width, px->height);
    if (!size.changes) return {};
    px = downsampledSource(px, size);
    perPixelX = static_cast<double>(size.width) / static_cast<double>(size.useWidth);
    perPixelY = static_cast<double>(size.height) / static_cast<double>(size.useHeight);
    std::string note = std::to_string(fullWidth) + " x " + std::to_string(fullHeight) + " pixels averaged to " +
                       std::to_string(px->width) + " x " + std::to_string(px->height);
    if (size.useWidth != fullWidth || size.useHeight != fullHeight) {
        note += " (" + std::to_string(fullWidth - size.useWidth) + " column(s) and " + std::to_string(fullHeight - size.useHeight) +
                " row(s) at the right and the bottom do not fill a block of " + std::to_string(how.bin) + " and are left out)";
    }
    return note;
}

std::string countOf(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

std::string stretchDescription(const std::string& how, const std::vector<StretchParams>& params) {
    std::string desc = how + ":";
    char buf[96];
    for (size_t c = 0; c < params.size(); ++c) {
        std::snprintf(buf, sizeof buf, " c%zu s=", c);
        desc += buf;
        std::snprintf(buf, sizeof buf, "%.6f", params[c].shadows);
        desc += cNumber(buf) + " m=";
        std::snprintf(buf, sizeof buf, "%.6f", params[c].midtones);
        desc += cNumber(buf) + " h=";
        std::snprintf(buf, sizeof buf, "%.6f", params[c].highlights);
        desc += cNumber(buf);
    }
    return desc;
}

// Name of the temporary file an output is written to before it gets its final name. A file of
// that name that this run did not create is never overwritten silently, and never if it is the input.
// `others`: the files the input reads its data from beside itself (an XISF unit), which are no
// more to be written over than the input is.
using InputFiles = std::vector<std::string>;

bool isOneOf(const std::string& path, const InputFiles* files) {
    if (!files) return false;
    std::error_code ec;
    if (!fs::exists(toPath(path), ec)) return false;
    for (const std::string& file : *files)
        if (fs::equivalent(toPath(path), toPath(file), ec)) return true;
    return false;
}

// What an XISF unit is read from beside its header: the files the header names, as they are
// looked for and as they are found (both: a file that is not read is no file to write over either).
InputFiles inputFilesOf(const XisfFile& file) {
    InputFiles files = file.externalPaths();
    for (const std::vector<std::string>* more : {&file.externalAsked(), &file.externalFiles()})
        for (const std::string& other : *more)
            if (std::find(files.begin(), files.end(), other) == files.end()) files.push_back(other);
    return files;
}

std::string partPathFor(const std::string& outPath, const std::string& input, bool force, const InputFiles* others = nullptr) {
    const std::string tmpPath = outPath + ".part";
    std::error_code ec;
    // Only a plain file of that name is ever written over: through a link the data would land
    // somewhere else, and a directory or a pipe is not ours to remove.
    const fs::file_status there = fs::symlink_status(toPath(tmpPath), ec);
    if (!ec && fs::exists(there) && !fs::is_regular_file(there)) {
        throw Error(tmpPath + ", the name of the temporary file for this output, is taken by something that is not a "
                    "regular file; remove it or choose another output name", ErrorKind::Io);
    }
    if (fs::exists(toPath(tmpPath), ec)) {
        if (!input.empty() && fs::equivalent(toPath(tmpPath), toPath(input), ec)) {
            throw Error("the temporary file for this output, " + tmpPath + ", is the input file; choose another output name",
                        ErrorKind::Argument);
        }
        if (isOneOf(tmpPath, others)) {
            throw Error("the temporary file for this output, " + tmpPath + ", is a file the input reads its data from; choose "
                        "another output name", ErrorKind::Argument);
        }
        if (!force) {
            throw Error(tmpPath + " exists (left by an interrupted run?); delete it, or use --force to overwrite it",
                        ErrorKind::Exists);
        }
    }
    return tmpPath;
}

// Asks the system to put a file's data (or a directory's entries) on the disk.
void syncToDisk(const std::string& path, bool directory) {
#ifdef _WIN32
    if (directory) return;
    const int fd = _wopen(toPath(path).c_str(), _O_RDWR | _O_BINARY);
    if (fd >= 0) {
        _commit(fd);
        _close(fd);
    }
#else
    const int fd = ::open(path.c_str(), directory ? O_RDONLY | O_DIRECTORY : O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

// A name beside a file that nothing has: where the file is set aside while another takes its
// place. "" if there is none to be had.
std::string asideNameFor(const std::string& path) {
    std::error_code ec;
    for (int n = 0; n < 100; ++n) {
        const std::string name = path + ".replaced" + (n ? std::to_string(n) : std::string());
        if (!fs::exists(fs::symlink_status(toPath(name), ec))) return name;
    }
    return {};
}

// Removes a file that was set aside while another took its place. One that cannot be removed
// (on Windows: a file that may not be written to is renamed, and not deleted) is given that
// right and tried again; if it stays, that is said: nobody else knows what it is.
void removeSetAside(const std::string& aside) {
    if (aside.empty()) return;
    std::error_code ec;
    fs::remove(toPath(aside), ec);
    if (ec) {
        std::error_code other;
        // (not through a link: what it leads to is not the file that was set aside)
        if (!fs::is_symlink(fs::symlink_status(toPath(aside), other))) {
            fs::permissions(toPath(aside), fs::perms::owner_write, fs::perm_options::add, other);
        }
        ec.clear();
        fs::remove(toPath(aside), ec);
    }
    if (ec) warn("the file that was replaced could not be removed and is left as " + aside + " (" + ec.message() + ")");
}

// Gives a file that was written its name. A file of that name that is there stays until the new
// one has its place: where a file cannot be renamed over an existing one, the old one is set
// aside, and put back if the new one cannot take its place then either. (It is never removed
// first: a rename that fails for another reason would leave neither.) Throws
// std::filesystem::filesystem_error, an I/O error; `keptAs`, if given, is then the name the file
// that was there is under, should it not have been put back.
void replaceFile(const std::string& tmpPath, const std::string& outPath, std::string* keptAs = nullptr) {
    std::error_code ec;
    fs::rename(toPath(tmpPath), toPath(outPath), ec);
    if (!ec) return;
    const std::error_code why = ec;
    std::error_code other;
    const std::string aside = fs::exists(fs::symlink_status(toPath(outPath), other)) ? asideNameFor(outPath) : std::string();
    if (!aside.empty()) {
        fs::rename(toPath(outPath), toPath(aside), other);
        if (!other) {
            fs::rename(toPath(tmpPath), toPath(outPath), other);
            if (!other) {
                removeSetAside(aside);
                return;
            }
            fs::rename(toPath(aside), toPath(outPath), other);
            if (other) {
                if (keptAs) *keptAs = aside;
                throw fs::filesystem_error("cannot rename (the file that was there is kept as " + aside + ")", toPath(tmpPath),
                                           toPath(outPath), why);
            }
        }
    }
    throw fs::filesystem_error("cannot rename", toPath(tmpPath), toPath(outPath), why);
}

void removeFile(const std::string& path) {
    std::error_code ec;
    fs::remove(toPath(path), ec);
}

// The files of an XISF unit that was written get their names. Of a distributed unit the data
// blocks file goes first, so that a header that is there has its data. Two renames are not one.
// A data blocks file that is there already (of a unit that is written over) is set aside until
// the header has its name too, and put back if it does not get it: the unit that was there is
// then there still. Data blocks without a header are nobody's, and are taken away again. What
// cannot be put back is named in the error, with the name it is under.
void replaceUnit(const std::string& tmpPath, const std::string& outPath, const std::string& blocksTmp, const std::string& blocksPath) {
    if (blocksTmp.empty()) {
        replaceFile(tmpPath, outPath);
        return;
    }
    std::error_code ec;
    std::string aside;
    if (fs::exists(toPath(blocksPath), ec)) {
        aside = asideNameFor(blocksPath);
        if (!aside.empty()) fs::rename(toPath(blocksPath), toPath(aside), ec);
        if (aside.empty() || ec) {
            throw Error("the data blocks file " + blocksPath + " could not be replaced (" +
                        (aside.empty() ? std::string("no name to set it aside under") : ec.message()) +
                        "); the unit that was there is as it was", ErrorKind::Io);
        }
    }
    std::string headerKeptAs;
    try {
        replaceFile(blocksTmp, blocksPath);
        replaceFile(tmpPath, outPath, &headerKeptAs);
    } catch (const std::exception& e) {
        removeFile(blocksPath);
        std::string kept;
        if (!aside.empty()) {
            ec.clear();
            fs::rename(toPath(aside), toPath(blocksPath), ec);
            if (ec) kept = "the data blocks file that was there is kept as " + aside + " (rename it to " + blocksPath + ")";
        }
        if (!headerKeptAs.empty()) {
            kept += std::string(kept.empty() ? "" : ", and ") + "the header file that was there is kept as " + headerKeptAs +
                    " (rename it to " + outPath + ")";
        }
        throw Error("the files of the unit could not be given their names (" + std::string(e.what()) + "); " +
                    (!kept.empty() ? kept : !aside.empty() ? std::string("the unit that was there is as it was")
                                                            : std::string("the unit is not written")),
                    ErrorKind::Io);
    }
    removeSetAside(aside);
}

// An output replaces a file of its name. A directory or a device of that name is left alone,
// whatever the caller allows: renaming a file over /dev/null would take the device away.
void notInPlaceOfSomethingElse(const std::string& outPath) {
    std::error_code ec;
    const fs::file_status status = fs::status(toPath(outPath), ec);
    if (!ec && fs::exists(status) && !fs::is_regular_file(status)) {
        throw Error(outPath + (fs::is_directory(status) ? " is a directory" : " is not a regular file") +
                    "; it is not replaced by the output", ErrorKind::Io);
    }
}

// Refuses an output that exists (unless it may be overwritten) or that is the input itself.
void checkOutput(const std::string& outPath, const std::string& input, bool force, const InputFiles* others = nullptr) {
    notInPlaceOfSomethingElse(outPath);
    if (isOneOf(outPath, others)) {
        throw Error("the output, " + outPath + ", is a file the input reads its data from; name another one", ErrorKind::Argument);
    }
    if (fs::exists(toPath(outPath)) && !force) throw Error(outPath + " already exists (use --force to overwrite)", ErrorKind::Exists);
    if (!input.empty() && fs::exists(toPath(outPath)) && fs::equivalent(toPath(outPath), toPath(input))) {
        throw Error("output would overwrite the input file", ErrorKind::Argument);
    }
}

// The data blocks file that is written beside an XISF header file: where it goes, the temporary
// file it is written to first, and the name the header has for it. Nothing for any other output.
struct BlocksOutput {
    std::string path, tmpPath, name;
    bool wanted() const { return !path.empty(); }
};

BlocksOutput blocksOutputFor(const std::string& outPath, const std::string& input, bool force, const InputFiles* others = nullptr) {
    BlocksOutput blocks;
    if (outPath.empty() || !isXisfHeaderName(outPath)) return blocks;
    blocks.path = xisfBlocksPathFor(outPath);
    blocks.name = fromPath(toPath(blocks.path).filename());
    checkOutput(blocks.path, input, force, others);
    blocks.tmpPath = partPathFor(blocks.path, input, force, others);
    return blocks;
}

// Chooses the range of floating point data: the XISF bounds attribute, or black and white when
// exporting to TIFF or PNG (`display`).
std::pair<double, double> floatBounds(const FitsImage& img, const ConvertOptions& opt, std::string& how, bool display = false) {
    if (img.bounds) return *img.bounds;
    if (opt.bounds) {
        how = display ? "range set with --bounds" : "bounds set with --bounds";
        return *opt.bounds;
    }
    if (img.dataMin >= 0 && img.dataMax <= 1) return {0.0, 1.0};
    char low[48], high[48];
    std::snprintf(low, sizeof low, "%g", img.dataMin);
    std::snprintf(high, sizeof high, "%g", img.dataMax);
    const std::string span = "float data spans " + cNumber(low) + ".." + cNumber(high) + ": ";
    const char* hint = " (override with --bounds)";
    if (img.dataMin >= 0 && img.dataMax <= 65535) {
        how = std::string(span) + (display ? "0:65535 taken as black:white" : "bounds set to 0:65535") + hint;
        return {0.0, 65535.0};
    }
    how = std::string(span) + (display ? "that range taken as black:white" : "bounds set to that range") + hint;
    return {img.dataMin, img.dataMax > img.dataMin ? img.dataMax : img.dataMin + 1};
}

// How FITS output is stored: tile-compressed when compression is asked for, and when the output
// has the name of a tile-compressed file (image.fits.fz).
FitsWriteOptions fitsStorage(const ConvertOptions& opt, const std::string& outPath) {
    FitsWriteOptions storage;
    if (opt.compress) {
        if (opt.codec == "zstd" || opt.codec == "lz4" || opt.codec == "lz4hc") {
            throw Error(std::string("FITS has no ") + (opt.codec == "zstd" ? "Zstandard" : "LZ4") +
                        " compression; use --compress for tile compression (RICE_1, and GZIP_2 for "
                        "floating point), or --codec zlib for GZIP_2 alone", ErrorKind::Argument);
        }
        storage.tiles = opt.codec == "zlib" ? FitsTiles::Gzip : FitsTiles::Default;
    } else if (toLower(fromPath(toPath(outPath).extension())) == ".fz") {
        storage.tiles = FitsTiles::Default;
    }
    return storage;
}

// The codec of ASDF output: zlib if none is named.
std::string asdfCodec(const ConvertOptions& opt) {
    if (!opt.compress) return {};
    if (opt.codec == "lz4" || opt.codec == "lz4hc") {
        throw Error("ASDF output is compressed with zlib or zstd; LZ4 is written to XISF only (--codec zlib, --codec zstd)",
                    ErrorKind::Argument);
    }
    return opt.codec.empty() ? "zlib" : opt.codec;
}

// The directory a file is written to: where the temporary files of its conversion go.
std::string directoryOf(const std::string& path) {
    const fs::path parent = toPath(path).parent_path();
    return parent.empty() ? std::string(".") : fromPath(parent);
}

// The pixels of an image, read a piece at a time: from where the reader left them, or from
// memory (an image handed over by a caller), which the image keeps.
Source sourceOf(const FitsImage& img) {
    if (img.pieces) return img.pieces;
    return borrowedSource(img.pixels);
}

}  // namespace

InputFormat detectInputFormat(const std::string& path) {
    if (looksLikeFits(path)) return InputFormat::Fits;
    if (looksLikeAsdf(path)) return InputFormat::Asdf;
    if (looksLikeDng(path)) return InputFormat::Dng;
    return InputFormat::Xisf;
}

XisfFileRewrite rewriteXisfFile(const std::string& input, const std::string& output, bool inPlace, bool force,
                                XisfRewriteOptions ropt) {
    XisfFileRewrite done;
    // In place, the file itself is replaced, not a link that leads to it.
    std::error_code pathError;
    const fs::path real = fs::canonical(toPath(input), pathError);
    const std::string outPath = !inPlace ? output : pathError ? input : fromPath(real);
    done.output = outPath;
    const bool same = fs::exists(toPath(outPath)) && fs::equivalent(toPath(outPath), toPath(input));
    if (same && !inPlace) {
        throw Error("the output would overwrite the input file; add --in-place to replace it, or name another file "
                    "or directory with -o or -d", ErrorKind::Argument);
    }
    if (!same) notInPlaceOfSomethingElse(outPath);
    const fs::perms permissions = fs::status(toPath(input)).permissions();
    const auto readOnly = [](fs::perms p) {
        return (p & (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write)) == fs::perms::none;
    };
    if (same && readOnly(permissions)) throw Error("the file is read-only; it is not replaced", ErrorKind::Io);
    if (same && isXisfHeaderName(outPath)) {
        // A header that is reached through a link from another directory has its data looked for
        // beside the link, and the unit that replaces it would be written beside the file.
        std::error_code ec;
        const fs::path named = fs::absolute(toPath(input), ec).parent_path(), real = toPath(outPath).parent_path();
        if (!ec && !fs::equivalent(named.empty() ? fs::path(".") : named, real.empty() ? fs::path(".") : real, ec) && !ec) {
            throw Error("this header file is a link from another directory: its data is looked for beside the link, and "
                        "in place the unit would be written beside " + outPath + "; name that file itself", ErrorKind::Argument);
        }
    }

    // A unit under the name of a header file (.xish) is written distributed: its data blocks go
    // into the file of that name that ends in .xisb. The files the input reads are not written
    // over, except, in place, the one that is that very file.
    InputFiles inputFiles, resolvedFiles;   // the files the header names, and those of them it is read from
    std::map<std::string, std::set<uint64_t>> namedBlocks;   // of each data blocks file: the blocks the header names
    bool inputIsHeader = false;
    {
        const MessageScope silent([](MessageLevel, const std::string&) {});   // (what there is to say, the rewrite says)
        const XisfFile in(input);
        inputFiles = inputFilesOf(in);
        resolvedFiles = in.externalPaths();
        for (const std::string& file : in.externalPaths()) namedBlocks[file] = in.namedBlockIds(file);
        // (and by every way the header writes a file, followed or not: which of them are one file, the system says)
        for (const auto& asked : in.namedIdsAsAsked()) namedBlocks[asked.first].insert(asked.second.begin(), asked.second.end());
        done.inputSize = in.unitSize();
        inputIsHeader = in.headerFile();
    }
    const auto readByInput = [&](const std::string& path) { return isOneOf(path, &inputFiles); };
    if (!same && readByInput(outPath)) {
        throw Error("the output, " + outPath + ", is a file the input reads its data from; name another one", ErrorKind::Argument);
    }
    if (!same && fs::exists(toPath(outPath)) && !force) {
        throw Error(outPath + " already exists (use --force to overwrite)", ErrorKind::Exists);
    }
    std::string blocksPath, blocksTmp, blocksName;
    bool blocksReplaced = false;   // the data blocks file of the output is one the input reads: it is replaced with it
    if (isXisfHeaderName(outPath)) {
        blocksPath = xisfBlocksPathFor(outPath);
        notInPlaceOfSomethingElse(blocksPath);
        std::error_code ec;
        if (fs::exists(toPath(blocksPath), ec) && fs::equivalent(toPath(blocksPath), toPath(input), ec)) {
            throw Error("the data blocks file of the output, " + blocksPath + ", is the input file", ErrorKind::Argument);
        }
        blocksReplaced = readByInput(blocksPath);
        // (a link the header is not followed through is not followed here either: where it leads is not ours to look at)
        if (blocksReplaced && same && isOneOf(blocksPath, &resolvedFiles) && fs::is_symlink(fs::symlink_status(toPath(blocksPath), ec))) {
            // In place, the file is replaced and not the link that leads to it, as for the header.
            // (The header goes on naming the link.)
            const fs::path target = fs::canonical(toPath(blocksPath), ec);
            if (!ec) {
                blocksName = fromPath(toPath(blocksPath).filename());
                blocksPath = fromPath(target);
            }
        }
        if (blocksReplaced && !same) {
            throw Error("the data blocks file of the output, " + blocksPath + ", is a file the input reads its data from; "
                        "name another output, or add --in-place to replace the input", ErrorKind::Argument);
        }
        if (!blocksReplaced && fs::exists(toPath(blocksPath), ec) && !force) {
            throw Error(blocksPath + " already exists (use --force to overwrite)", ErrorKind::Exists);
        }
        if (blocksReplaced && readOnly(fs::status(toPath(blocksPath)).permissions())) {
            throw Error("the data blocks file " + blocksPath + " is read-only; it is not replaced", ErrorKind::Io);
        }
    }
    ropt.readBack = ropt.readBack || same;  // a file that replaces its source is always read back first

    // Replacing a file by an identical one would only cost time: such files are left alone. (A
    // unit that is not of the kind its name says, a header file named .xisf, is not identical
    // to what is written under that name.)
    if (same && inputIsHeader == isXisfHeaderName(outPath) && xisfStoredAsRequested(input, ropt)) {
        done.unchanged = true;
        return done;
    }

    // A data blocks file may hold the blocks of several headers. The one that is replaced in
    // place is written anew with the blocks of this header alone: what it holds beside them
    // would be gone, and with it the images of whoever names them. So a file that is there is
    // replaced only when it was read and holds this header's blocks and no others. Nothing is
    // left to "reading will say so": with --image, what names the file may never be read.
    if (blocksReplaced) {
        if (!isOneOf(blocksPath, &resolvedFiles)) {
            throw NotAllowed("the data blocks file " + blocksPath + " is named by the header, which " +
                             (externalFilesPolicy() == ExternalFiles::Anywhere
                                  ? "is not read from it as it names it: a file that is not read is not replaced"
                                  : "is not followed to it: a file that is not read is not replaced (--external-files "
                                    "anywhere lets a header lead there)"));
        }
        size_t foreign = 0, own = 0;
        std::set<uint64_t> named;
        std::error_code ec;
        for (const auto& file : namedBlocks)
            if (fs::equivalent(toPath(file.first), toPath(blocksPath), ec)) named.insert(file.second.begin(), file.second.end());
        bool indexed = true;
        try {
            const MessageScope silent([](MessageLevel, const std::string&) {});
            for (const XisbElement& e : readXisbIndex(blocksPath, blocksPath).elements) {
                if (e.position == 0) continue;
                ++(named.count(e.id) ? own : foreign);
            }
        } catch (const Error&) {
            indexed = false;   // (no data blocks file: a file that is one block, or a damaged one)
        }
        bool signed_ = false;   // it begins as a data blocks file does
        if (!indexed) {
            char first[8] = {0};
            std::ifstream begin(toPath(blocksPath), std::ios::binary);
            signed_ = begin.read(first, 8) && std::memcmp(first, "XISB0100", 8) == 0;
        }
        if (!indexed && (!named.empty() || signed_) && !force) {
            throw Error("the data blocks file " + blocksPath + " cannot be read as one, so what it holds beside the blocks of "
                        "this header cannot be told: write the unit under another name, or use --force to replace the file "
                        "all the same", ErrorKind::Exists);
        }
        if (foreign && !own && !named.empty()) {
            // The file holds none of the blocks this header names: it is not this header's, as it
            // is. (What a replacement leaves that was stopped half-way; nothing is to be replaced
            // then, with or without leave: the files of that run are what puts the unit right.)
            std::string hint;
            size_t looked = 0;
            for (const uint64_t id : named) {
                if (!hint.empty() || ++looked > 8) break;
                hint = xisfSetAsideHint(blocksPath, &id);
            }
            throw Error("the data blocks file " + blocksPath + " holds none of the blocks this header names (" +
                        std::to_string(foreign) + " others): it is not the file that was written with this header, and "
                        "the unit cannot be read as it is" + hint);
        }
        if (foreign && !force) {
            throw Error("the data blocks file " + blocksPath + " also holds " + std::to_string(foreign) +
                        (foreign == 1 ? " block" : " blocks") + " that this header does not name (another header may read " +
                        (foreign == 1 ? "it" : "them") + "), and replacing the file would lose " + (foreign == 1 ? "it" : "them") +
                        ": write the unit under another name, or use --force to replace the file all the same", ErrorKind::Exists);
        }
    }

    const std::string tmpPath = partPathFor(outPath, input, force, &inputFiles);
    const TempDirectoryScope temporaries(directoryOf(outPath));   // (what a block is made into on the way, where it is large)
    if (!blocksPath.empty()) {
        // (in place, the temporary file of the data blocks file is no file the unit reads either)
        blocksTmp = partPathFor(blocksPath, input, force, &inputFiles);
        ropt.blocksPath = blocksTmp;
        ropt.blocksName = !blocksName.empty() ? blocksName : fromPath(toPath(blocksPath).filename());
    }
    const auto removeTemporaries = [&] {
        removeFile(tmpPath);
        if (!blocksTmp.empty()) removeFile(blocksTmp);
    };
    XisfRewriteResult& r = done.result;
    try {
        r = rewriteXisf(input, tmpPath, ropt);
        if (!same) replaceUnit(tmpPath, outPath, blocksTmp, blocksPath);
    } catch (...) {
        removeTemporaries();
        throw;
    }
    if (same && !r.changed) {
        removeTemporaries();
        done.unchanged = true;
        return done;
    }
    if (same) {
        // The original is only ever replaced by a rename, once the new file is on the disk with
        // the permissions of the old one; if the rename fails both files stay.
        std::error_code ec;
        const fs::path parent = toPath(outPath).parent_path();
        if (!blocksTmp.empty()) {
            if (blocksReplaced) fs::permissions(toPath(blocksTmp), fs::status(toPath(blocksPath)).permissions(), ec);
            syncToDisk(blocksTmp, false);
        }
        fs::permissions(toPath(tmpPath), permissions, ec);
        syncToDisk(tmpPath, false);
        if (blocksTmp.empty()) {
            fs::rename(toPath(tmpPath), toPath(outPath), ec);
            if (ec) throw Error("could not replace the file (" + ec.message() + "); the rewritten copy is kept as " + tmpPath, ErrorKind::Io);
        } else {
            // A unit of two files cannot be replaced in one step. The data blocks file that is
            // there is set aside, the new one takes its place, then the header takes its own; if
            // either cannot, what was set aside is put back and the unit is as it was. (Should the
            // machine stop between the steps, the old data blocks file is there under the name it
            // was set aside with, <name>.xisb.replaced, and the old header names its blocks by
            // identifiers the new file does not have: nothing is read that is not the image.)
            std::string aside;
            if (fs::exists(toPath(blocksPath), ec)) {
                aside = asideNameFor(blocksPath);
                if (!aside.empty()) fs::rename(toPath(blocksPath), toPath(aside), ec);
                if (aside.empty() || ec) {
                    const std::string why = aside.empty() ? std::string("no name to set it aside under") : ec.message();
                    removeTemporaries();
                    throw Error("could not replace the data blocks file " + blocksPath + " (" + why + "); the unit is as it was", ErrorKind::Io);
                }
            }
            ec.clear();
            fs::rename(toPath(blocksTmp), toPath(blocksPath), ec);
            if (!ec) fs::rename(toPath(tmpPath), toPath(outPath), ec);
            if (ec) {
                const std::string why = ec.message();
                std::error_code back;
                fs::remove(toPath(blocksPath), back);   // the new one, if it got there
                back.clear();
                if (!aside.empty()) fs::rename(toPath(aside), toPath(blocksPath), back);
                removeTemporaries();
                throw Error("could not replace the files of the unit (" + why + "); " +
                            (back ? "the data blocks file that was there is kept as " + aside + ": renamed to " + blocksPath + ", the unit is as it was"
                                  : std::string("it is as it was")),
                            ErrorKind::Io);
            }
            removeSetAside(aside);
        }
        syncToDisk(parent.empty() ? "." : fromPath(parent), true);
    }
    return done;
}

std::vector<FitsKeyword> xisfImageFitsKeywords(XisfFile& file, size_t idx, bool bottomUp, bool propertyKeywords,
                                               bool wcs, int sipOrder, std::string* wcsSummary) {
    const XisfImage& img = file.images()[idx];
    std::vector<FitsKeyword> keywords = img.keywords;
    if (propertyKeywords) addPropertyKeywords(file, idx, keywords);
    if (bottomUp) {
        if (FitsKeyword* bp = findKeyword(keywords, "BAYERPAT")) {
            const std::string v = trim(bp->value);
            std::string pattern;
            if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') pattern = trim(v.substr(1, v.size() - 2));
            int pw = 2, ph = 2;
            if (img.cfa.present && img.cfa.pattern == pattern) {
                pw = img.cfa.width;
                ph = img.cfa.height;
            }
            if (!pattern.empty() && pattern.size() == static_cast<size_t>(pw * ph)) {
                bp->value = fitsString(flipPatternRows(pattern, pw, ph, img.height));
            } else {
                warn("cannot adjust BAYERPAT " + v + " for the bottom-up row order; check it manually");
            }
        }
    } else {
        // WCS keywords stored in an XISF file follow the FITS bottom-up convention
        // (as PixInsight wrote them); adapt them to the top-down rows being written.
        flipWcsRowOrder(keywords, img.height);
    }
    if (wcs && !hasKeyword(keywords, "CTYPE1")) {
        WcsResult result;
        if (astrometricSolutionToWcs(file, idx, bottomUp, sipOrder, result)) {
            keywords.insert(keywords.end(), result.keywords.begin(), result.keywords.end());
            if (wcsSummary) *wcsSummary = result.summary;
        }
    }
    return keywords;
}

std::vector<Property> carriedProperties(XisfFile& file, size_t index, bool verify) {
    std::vector<Property> properties = file.loadProperties(index, verify);
    if (index == XisfFile::kFileProperties) {
        properties.erase(std::remove_if(properties.begin(), properties.end(),
                                        [](const Property& p) { return isFileStorageProperty(p.id); }),
                         properties.end());
    }
    return properties;
}

void flipBayerRows(std::vector<FitsKeyword>& keywords, uint64_t height) {
    if (FitsKeyword* bp = findKeyword(keywords, "BAYERPAT")) {
        const std::string pattern = fitsUnquote(bp->value);
        if (pattern.size() == 4) bp->value = fitsString(flipPatternRows(pattern, 2, 2, height));
        else warn("cannot adjust BAYERPAT " + bp->value + " for the changed row order; check it manually");
    }
}

void flipCfaRows(FitsImage& img) {
    if (!img.cfaPattern.empty() && img.cfaWidth > 0 && img.cfaHeight > 0 &&
        img.cfaPattern.size() == static_cast<size_t>(img.cfaWidth) * static_cast<size_t>(img.cfaHeight)) {
        img.cfaPattern = flipPatternRows(img.cfaPattern, img.cfaWidth, img.cfaHeight, img.pixels.height);
    }
}

void flipKeywordRows(std::vector<FitsKeyword>& keywords, uint64_t height) {
    flipBayerRows(keywords, height);
    flipWcsRowOrder(keywords, height);
}

void convertXisfFile(const std::string& input, const std::string& outPath, Format format, const ConvertOptions& opt) {
    if (format == Format::Xisf) throw Error("XISF to XISF is a rewrite, not a conversion", ErrorKind::Argument);
    downsampleIsForPictures(opt, format);
    debayerIsForPictures(opt, format);
    xisfStorageIsForXisf(opt, format);
    const FitsWriteOptions fitsOptions = format == Format::Fits ? fitsStorage(opt, outPath) : FitsWriteOptions();
    if (format == Format::Asdf) asdfCodec(opt);   // (said before anything is read)
    XisfFile file(input);

    std::vector<size_t> indices;
    if (opt.imageIndex) {
        if (*opt.imageIndex >= file.images().size()) {
            throw Error("image index " + std::to_string(*opt.imageIndex) + " out of range (file has " +
                        std::to_string(file.images().size()) + ")", ErrorKind::Index);
        }
        indices.push_back(*opt.imageIndex);
    } else {
        for (size_t i = 0; i < file.images().size(); ++i) {
            if (file.images()[i].unsupported.empty()) indices.push_back(i);
            else warn("skipping image " + std::to_string(i) + ": " + file.images()[i].unsupported);
        }
    }
    if (indices.empty()) throw Error("no convertible images in file");
    // ASDF output holds the images as FITS HDUs, so both formats are prepared the same way.
    const bool fitsLike = format == Format::Fits || format == Format::Asdf;
    if (format == Format::Png) {
        if (indices.size() > 1) {
            warn("PNG holds one image; writing image " + std::to_string(indices[0]) + " only (use --image to choose)");
            indices.resize(1);
        }
        if (opt.bits && *opt.bits != SampleFormat::UInt8 && *opt.bits != SampleFormat::UInt16) {
            throw Error("PNG supports only --bits u8 or u16", ErrorKind::Argument);
        }
    }

    // (the files a distributed unit reads its data from are no outputs, and no temporary files)
    const InputFiles others = inputFilesOf(file);
    checkOutput(outPath, input, opt.force, &others);

    // Temporary files go beside the output.
    const TempDirectoryScope temporaries(directoryOf(outPath));
    std::vector<Source> buffers;
    std::vector<std::string> stretchNotes;  // HISTORY text per converted image
    std::vector<std::pair<double, double>> shrunk;   // pixels of each picture per pixel of its image, in width and height
    std::vector<bool> colour;                        // an RGB picture (an RGB image, or a mosaic debayered)
    std::vector<bool> debayered;                     // ... a mosaic debayered
    buffers.reserve(indices.size());
    for (size_t idx : indices) {
        progress("reading", buffers.size(), indices.size());
        const XisfImage& img = file.images()[idx];
        Source px = file.pixelSource(idx, opt.verify);
        bool rgb = img.colorSpace != "Gray", madeRgb = false;
        if (opt.debayer) {
            // (XISF rows are top-down, as the picture's: the pattern is that of the pixels as they are)
            std::string why;
            const std::string pattern = bayerPatternOf(img.keywords, img.cfa.present ? img.cfa.pattern : std::string(),
                                                       img.cfa.width, img.cfa.height, why);
            if (debayerForPicture(px, pattern, why, "image " + std::to_string(idx))) rgb = madeRgb = true;
        }
        colour.push_back(rgb);
        debayered.push_back(madeRgb);
        // A smaller picture is made of the image as it is stored (after demosaicing): the mean of
        // linear data is what larger pixels would have recorded. A stretch comes after, on the picture.
        double perPixelX = 1, perPixelY = 1;
        const std::string smaller = makeSmaller(px, opt.downsample, perPixelX, perPixelY);
        if (!smaller.empty()) info("image " + std::to_string(idx) + ": " + smaller);
        shrunk.emplace_back(perPixelX, perPixelY);
        if (opt.stretch != Stretch::None) {
            const size_t colorChannels = rgb ? 3 : 1;
            std::vector<StretchParams> params;
            std::string how;
            const DisplayFunction& df = img.displayFunction;
            const bool useStored = df.present && !df.isIdentity() &&
                                   (opt.stretch == Stretch::Auto || opt.stretch == Stretch::Stored);
            if (useStored) {
                for (size_t c = 0; c < std::min<size_t>(colorChannels, px->channels); ++c) {
                    const size_t k = madeRgb ? 0 : c;   // (the STF of a mosaic is that of its one channel: for each colour)
                    params.push_back({df.s[k], df.m[k], df.h[k], df.l[k], df.r[k]});
                }
                how = "PixInsight STF";
            } else {
                if (opt.stretch == Stretch::Stored) {
                    throw Error("image " + std::to_string(idx) + " has no saved STF (DisplayFunction); "
                                "use --stretch=linked or --stretch=unlinked", ErrorKind::NotFound);
                }
                const bool linked = opt.stretch != Stretch::Unlinked;
                params = autoStretch(*px, img.lowerBound, img.upperBound, colorChannels, linked);
                how = linked ? "linked auto-STF" : "unlinked auto-STF";
            }
            px = stretchedSource(px, params, img.lowerBound, img.upperBound);
            SampleFormat target = isFloat(img.format) ? (fitsLike ? SampleFormat::Float32
                                                                                : SampleFormat::UInt16)
                                                      : img.format;
            if (opt.bits) target = *opt.bits;
            px = convertedSource(px, target, 0, 1);
            const std::string desc = stretchDescription(how, params);
            stretchNotes.push_back("Stretched with " + desc);
            info("image " + std::to_string(idx) + ": " + desc);
        } else if (opt.bits) {
            px = convertedSource(px, *opt.bits, img.lowerBound, img.upperBound);
        }
        if (format == Format::Png) {
            if (px->format != SampleFormat::UInt8 && px->format != SampleFormat::UInt16) {
                px = convertedSource(px, SampleFormat::UInt16, opt.stretch == Stretch::None ? img.lowerBound : 0,
                                     opt.stretch == Stretch::None ? img.upperBound : 1);
            }
            const uint64_t colorCh = (rgb && px->channels >= 3) ? 3 : 1;
            if (px->channels > colorCh + 1) {
                warn("PNG: keeping " + std::to_string(colorCh + 1) + " of " + std::to_string(px->channels) +
                     " channels (color + alpha)");
                px = channelSource(px, 0, colorCh + 1);
            }
            if (isFloat(img.format) && opt.stretch == Stretch::None) {
                info("linear data may look dark in PNG; add --stretch for a viewable image");
            }
        }
        if (fitsLike && opt.bottomUp) px = flippedSource(px);
        if (format == Format::Tiff && (px->format == SampleFormat::UInt32 || px->format == SampleFormat::UInt64 ||
                                       px->format == SampleFormat::Float64)) {
            warn(std::string("image ") + std::to_string(idx) + ": " + sampleFormatName(px->format) +
                 " TIFF is not supported by many programs; consider --bits u16 or --bits f32");
        }
        if (opt.stretch == Stretch::None && isFloat(img.format) && !isFloat(px->format) && !img.boundsDeclared) {
            warn("image " + std::to_string(idx) + " has no bounds attribute; assuming [0,1]");
        }
        buffers.push_back(std::move(px));
    }

    const std::string tmpPath = partPathFor(outPath, input, opt.force, &others);
    progress("writing", 0, 0);
    try {
        if (fitsLike) {
            std::vector<FitsHdu> hdus;
            for (size_t n = 0; n < indices.size(); ++n) {
                const size_t idx = indices[n];
                const XisfImage& img = file.images()[idx];
                FitsHdu hdu;
                hdu.pixels = buffers[n];
                hdu.extname = img.id;
                hdu.bottomUp = opt.bottomUp;
                std::string wcsSummary;
                hdu.keywords = xisfImageFitsKeywords(file, idx, opt.bottomUp, opt.propertyKeywords, opt.wcs, opt.sipOrder,
                                                     &wcsSummary);
                if (!wcsSummary.empty()) info("image " + std::to_string(idx) + ": " + wcsSummary);
                hdu.keywords.push_back({"HISTORY", "", std::string("Converted from XISF by xisfconv ") + kVersion});
                if (n < stretchNotes.size()) hdu.keywords.push_back({"HISTORY", "", stretchNotes[n]});
                if (opt.properties) {
                    hdu.properties = carriedProperties(file, idx, opt.verify);
                    if (!hdu.properties.empty()) {
                        hdu.wcsDigest = wcsDigest(hdu.keywords, buffers[n]->width, buffers[n]->height, opt.bottomUp);
                        info("image " + std::to_string(idx) + ": " + countOf(hdu.properties.size(), "XISF property", "XISF properties") +
                             " taken along");
                    }
                }
                hdus.push_back(std::move(hdu));
            }
            std::vector<Property> metadata;
            if (opt.properties) metadata = carriedProperties(file, XisfFile::kFileProperties, opt.verify);
            if (format == Format::Asdf) {
                AsdfWriteOptions aopt;
                aopt.codec = asdfCodec(opt);
                aopt.metadata = std::move(metadata);
                writeAsdf(tmpPath, hdus, aopt);
            } else {
                FitsWriteOptions fopt = fitsOptions;
                fopt.metadata = std::move(metadata);
                writeFits(tmpPath, hdus, fopt);
            }
        } else if (format == Format::Tiff) {
            std::vector<TiffPage> pages;
            for (size_t n = 0; n < indices.size(); ++n) {
                const size_t idx = indices[n];
                const XisfImage& img = file.images()[idx];
                TiffPage page;
                page.pixels = buffers[n];
                page.rgb = colour[n];
                if (img.colorSpace == "CIELab") warn("CIELab image written as RGB samples without color conversion");
                if (img.hasIccProfile && debayered[n]) {
                    warn("image " + std::to_string(idx) + ": its ICC profile, of the one-channel mosaic, is not given to the colour picture");
                } else if (img.hasIccProfile) {
                    try {
                        page.iccProfile = file.readIccProfile(idx, opt.verify);
                    } catch (const Error& e) {
                        warn(std::string("ICC profile not copied: ") + e.what());
                    }
                }
                if (img.resolution.present) {
                    // (fewer pixels per inch, so that the picture is as large on paper as the image)
                    page.xResolution = img.resolution.horizontal * shrunk[n].first;
                    page.yResolution = img.resolution.vertical * shrunk[n].second;
                    page.resolutionInCm = img.resolution.unit == "cm";
                }
                page.description = img.id;
                pages.push_back(std::move(page));
            }
            writeTiff(tmpPath, pages, opt.compress);
        } else {
            const XisfImage& img = file.images()[indices[0]];
            PngImage png;
            png.pixels = buffers[0];
            png.rgb = colour[0] && buffers[0]->channels >= 3;
            if (img.hasIccProfile && debayered[0]) {
                warn("image " + std::to_string(indices[0]) + ": its ICC profile, of the one-channel mosaic, is not given to the colour picture");
            } else if (img.hasIccProfile) {
                try {
                    png.iccProfile = file.readIccProfile(indices[0], opt.verify);
                } catch (const Error& e) {
                    warn(std::string("ICC profile not copied: ") + e.what());
                }
            }
            if (img.resolution.present) {
                png.pixelsPerMeter = img.resolution.horizontal * shrunk[0].first / (img.resolution.unit == "cm" ? 0.01 : 0.0254);
            }
            writePng(tmpPath, png);
        }
        replaceFile(tmpPath, outPath);
    } catch (...) {
        removeFile(tmpPath);
        throw;
    }
}

void convertFitsOrAsdfFile(const std::string& input, InputFormat kind, const std::string& outPath, Format format,
                           const ConvertOptions& opt) {
    if (kind == InputFormat::Xisf) throw Error("not a FITS, ASDF or DNG file", ErrorKind::Argument);
    if (format == Format::Fits) fitsStorage(opt, outPath);   // an option that does not apply is reported before the file is read
    if (format == Format::Asdf) asdfCodec(opt);
    const bool asdfInput = kind == InputFormat::Asdf, dngInput = kind == InputFormat::Dng;
    const char* inputName = asdfInput ? "ASDF" : dngInput ? "DNG" : "FITS";
    const bool exporting = format == Format::Tiff || format == Format::Png;
    if (asdfInput && format == Format::Asdf) {
        throw Error("the input is already an ASDF file; choose xisf, fits, tiff or png as output", ErrorKind::Argument);
    }
    if (opt.stretch != Stretch::None && !exporting) {
        throw Error(std::string("--stretch is for viewing: from ") + inputName + " input it is available for TIFF and PNG output",
                    ErrorKind::Argument);
    }
    downsampleIsForPictures(opt, format);
    debayerIsForPictures(opt, format);
    xisfStorageIsForXisf(opt, format);
    if (opt.stretch == Stretch::Stored) {
        throw Error(std::string(inputName) + " files carry no saved STF; use --stretch, --stretch=linked or --stretch=unlinked",
                    ErrorKind::NotFound);
    }
    if (format == Format::Png && opt.bits && *opt.bits != SampleFormat::UInt8 && *opt.bits != SampleFormat::UInt16) {
        throw Error("PNG supports only --bits u8 or u16", ErrorKind::Argument);
    }

    progress("reading", 0, 0);
    // (images are read a piece at a time; a compressed one is decompressed into a temporary file
    // beside the output where it does not fit the memory a conversion takes)
    const TempDirectoryScope temporaries(directoryOf(outPath));
    FitsFile fits = asdfInput ? readAsdf(input, false, opt.verify, std::nullopt, true)
                    : dngInput ? readDng(input, false, true)
                               : readFits(input, false, std::nullopt, true);
    // (the previews of a DNG file are no news: every DNG file has them)
    for (const auto& s : fits.skipped) (dngInput ? info : warn)("skipped " + s);
    if (fits.images.empty()) throw Error(std::string("no image data found in this ") + inputName + " file");
    if (kind == InputFormat::Fits && format == Format::Fits) {
        // FITS -> FITS has two uses: writing tile-compressed images as plain ones, and plain
        // images as tile-compressed ones (what funpack and fpack do).
        bool tiled = false;
        for (const auto& img : fits.images)
            if (!img.tileCompression.empty()) tiled = true;
        if (!tiled && fitsStorage(opt, outPath).tiles == FitsTiles::None) {
            throw Error("the input is already a FITS file; choose xisf, asdf, tiff or png as output, or add --compress "
                        "for a tile-compressed FITS file", ErrorKind::Argument);
        }
    }
    ImageSetOrigin origin;
    origin.format = inputName;
    origin.input = input;
    origin.defaultName = fromPath(toPath(input).stem());
    if (dngInput) {
        // The rows of a DNG file are top-down. As from XISF: FITS and ASDF output has them
        // bottom-up, the FITS convention, unless --top-down keeps them as they are.
        ConvertOptions dngOptions = opt;
        dngOptions.rowOrderGiven = false;
        if ((format == Format::Fits || format == Format::Asdf) && !(opt.rowOrderGiven && !opt.bottomUp)) {
            for (FitsImage& img : fits.images) flipImageRows(img);
        }
        writeImageSet(fits, origin, outPath, format, dngOptions);
        return;
    }
    writeImageSet(fits, origin, outPath, format, opt);
}

std::pair<double, double> automaticBounds(const FitsImage& image) {
    std::string how;
    return floatBounds(image, ConvertOptions(), how);
}

void flipImageRows(FitsImage& img) {
    if (img.pieces) img.pieces = flippedSource(img.pieces);
    else flipVertical(img.pixels);
    flipKeywordRows(img.keywords, img.pixels.height);
    flipCfaRows(img);
    img.topDown = !img.topDown;
    img.hasRowOrder = true;
}

void writeImageSet(FitsFile& fits, const ImageSetOrigin& source, const std::string& outPath, Format format,
                   const ConvertOptions& opt) {
    const bool asdfInput = source.format == "ASDF";
    const bool exporting = format == Format::Tiff || format == Format::Png;
    const std::string& input = source.input;
    if (fits.images.empty()) throw Error("no images to write", ErrorKind::Argument);
    const FitsWriteOptions fitsOptions = format == Format::Fits ? fitsStorage(opt, outPath) : FitsWriteOptions();
    if (format == Format::Asdf) asdfCodec(opt);   // (said before anything is read)

    std::vector<size_t> indices;
    if (opt.imageIndex) {
        if (*opt.imageIndex >= fits.images.size()) {
            throw Error("image index " + std::to_string(*opt.imageIndex) + " out of range (file has " +
                        std::to_string(fits.images.size()) + " image(s))", ErrorKind::Index);
        }
        indices.push_back(*opt.imageIndex);
    } else {
        for (size_t i = 0; i < fits.images.size(); ++i) indices.push_back(i);
    }
    if (format == Format::Png && indices.size() > 1) {
        warn("PNG holds one image; writing image " + std::to_string(indices[0]) + " only (use --image to choose)");
        indices.resize(1);
    }

    checkOutput(outPath, input, opt.force);

    auto origin = [&](const FitsImage& img) {
        return asdfInput || !img.source.empty() ? " (" + img.source + ")" : " (HDU " + std::to_string(img.hduIndex) + ")";
    };
    // Images that come from a file say so in their header; images handed over in memory do not.
    const std::string history = source.format.empty() ? std::string() : "Converted from " + source.format + " by xisfconv " + kVersion;
    const std::string tmpPath = partPathFor(outPath, input, opt.force);
    const TempDirectoryScope temporaries(directoryOf(outPath));
    // An XISF unit under the name of a header file (.xish) is written distributed: the header
    // there, the data blocks in a file of the same name that ends in .xisb.
    const BlocksOutput blocksFile = blocksOutputFor(format == Format::Xisf ? outPath : std::string(), input, opt.force);
    progress("writing", 0, 0);

    if (exporting) {
        // TIFF and PNG: rows top-down, floating point data scaled so that its range is 0..1.
        std::vector<Source> buffers;   // one per page; planes of a cube that is not RGB become pages
        std::vector<std::string> names;
        std::vector<std::vector<uint8_t>> profiles;   // ICC profiles, only for images handed over in memory
        for (size_t idx : indices) {
            FitsImage& img = fits.images[idx];
            Source px = sourceOf(img);
            const std::string label = "image " + std::to_string(idx);
            const bool topDown = opt.rowOrderGiven ? !opt.bottomUp : img.topDown;
            if (!topDown) px = flippedSource(px);
            if (opt.debayer) {
                // the pattern is that of the rows as stored: turned with them
                std::string why;
                std::string pattern = bayerPatternOf(img.keywords, img.cfaPattern, img.cfaWidth, img.cfaHeight, why);
                if (!topDown && !pattern.empty()) pattern = flipPatternRows(pattern, 2, 2, px->height);
                if (debayerForPicture(px, pattern, why, label) && !img.iccProfile.empty()) {
                    warn(label + ": its ICC profile, of the one-channel mosaic, is not given to the colour picture");
                    img.iccProfile.clear();
                }
            }
            double perPixelX = 1, perPixelY = 1;
            const std::string smaller = makeSmaller(px, opt.downsample, perPixelX, perPixelY);   // before a stretch, as from XISF
            if (!smaller.empty() && img.hasNaN) {
                // what was no number is left out of the means: is there still a pixel that is none?
                // (the range stays that of the image)
                img.hasNaN = floatRange(*px).hasNaN;
            }
            const SampleFormat stored = px->format;
            const bool wasFloat = isFloat(stored);
            std::string rangeNote;
            std::pair<double, double> range{0.0, 1.0};   // of floating point samples; integers use their full range
            if (wasFloat) {
                range = floatBounds(img, opt, rangeNote, true);
                if (img.hasNaN) warn(label + ": the data contains NaN/Inf values (black in integer output)");
            }
            std::string stretchNote;
            if (opt.stretch != Stretch::None) {
                // Every plane is image data here (FITS has no alpha channel): all are stretched.
                const bool linked = opt.stretch != Stretch::Unlinked;
                const auto params = autoStretch(*px, range.first, range.second, static_cast<size_t>(px->channels), linked);
                px = stretchedSource(px, params, range.first, range.second);   // Float32 in [0,1]
                range = {0.0, 1.0};
                // As for XISF input: 16-bit for floating point data, the stored type for integers.
                px = convertedSource(px, opt.bits ? *opt.bits : wasFloat ? SampleFormat::UInt16 : stored, 0, 1);
                stretchNote = stretchDescription(linked ? "linked auto-STF" : "unlinked auto-STF", params);
            } else if (opt.bits) {
                px = convertedSource(px, *opt.bits, range.first, range.second);
            }
            if (format == Format::Png && px->format != SampleFormat::UInt8 && px->format != SampleFormat::UInt16) {
                px = convertedSource(px, SampleFormat::UInt16, range.first, range.second);
            }
            bool scaled = false;
            if (isFloat(px->format) && wasFloat && (range.first != 0 || range.second != 1)) {
                px = normalizedSource(px, range.first, range.second);
                scaled = true;
            }
            if (format == Format::Tiff && (px->format == SampleFormat::UInt32 || px->format == SampleFormat::UInt64 ||
                                           px->format == SampleFormat::Float64)) {
                warn(label + ": " + sampleFormatName(px->format) +
                     " TIFF is not supported by many programs; consider --bits u16 or --bits f32");
            }
            if (source.notes) {
                std::string line = label + origin(img) + ": " + img.note + ", rows " +
                                   (topDown ? "top-down" : (img.generic && !img.hasRowOrder && !opt.rowOrderGiven)
                                                               ? "assumed bottom-up (flipped; add --top-down if the image comes out "
                                                                 "upside down)"
                                                               : "bottom-up (flipped)");
                if (!rangeNote.empty()) line += "; " + rangeNote;
                if (scaled) line += "; float samples scaled to 0..1";
                info(line);
                if (!smaller.empty()) info(label + ": " + smaller);
                if (!stretchNote.empty()) info(label + ": " + stretchNote);
                if (format == Format::Png && wasFloat && opt.stretch == Stretch::None) {
                    info("linear data may look dark in PNG; add --stretch for a viewable image");
                }
            }
            const std::string name = img.name.empty() ? source.defaultName : img.name;
            if (px->channels == 1 || px->channels == 3) {
                buffers.push_back(std::move(px));
                names.push_back(name);
                profiles.push_back(img.iccProfile);
                continue;
            }
            // A cube that is not an RGB image: one grayscale page per plane.
            const uint64_t planes = format == Format::Png ? 1 : px->channels;
            if (format == Format::Png) {
                warn(label + ": PNG holds one image; writing the first of " + std::to_string(px->channels) + " planes");
            }
            for (uint64_t c = 0; c < planes; ++c) {
                buffers.push_back(channelSource(px, c, 1));
                names.push_back(name + " plane " + std::to_string(c));
                profiles.push_back(img.iccProfile);
            }
        }
        try {
            if (format == Format::Tiff) {
                std::vector<TiffPage> pages;
                for (size_t n = 0; n < buffers.size(); ++n) {
                    TiffPage page;
                    page.pixels = buffers[n];
                    page.rgb = buffers[n]->channels == 3;
                    page.iccProfile = profiles[n];
                    page.description = names[n];
                    pages.push_back(std::move(page));
                }
                writeTiff(tmpPath, pages, opt.compress);
            } else {
                PngImage png;
                png.pixels = buffers[0];
                png.rgb = buffers[0]->channels == 3;
                png.iccProfile = profiles[0];
                writePng(tmpPath, png);
            }
            replaceFile(tmpPath, outPath);
        } catch (...) {
            removeFile(tmpPath);
            throw;
        }
        return;
    }

    if (format != Format::Xisf) {
        // FITS <-> ASDF: the same HDUs in another container. Rows stay in the order they are stored in.
        std::vector<FitsHdu> hdus;
        for (size_t idx : indices) {
            FitsImage& img = fits.images[idx];
            Source px = sourceOf(img);
            const std::string label = "image " + std::to_string(idx);
            const bool topDown = opt.rowOrderGiven ? !opt.bottomUp : img.topDown;
            std::string boundsNote;
            if (opt.bits && *opt.bits != px->format) {
                std::pair<double, double> bounds{0.0, 1.0};
                if (isFloat(px->format)) bounds = floatBounds(img, opt, boundsNote);
                px = convertedSource(px, *opt.bits, bounds.first, bounds.second);
            }
            if (img.hasNaN && isFloat(px->format)) warn(label + ": the data contains NaN/Inf values, which are copied as they are");
            FitsHdu hdu;
            hdu.pixels = px;
            hdu.keywords = img.keywords;
            hdu.extname = img.name;
            hdu.bottomUp = !topDown;
            if (!history.empty()) hdu.keywords.push_back({"HISTORY", "", history});
            if (opt.properties) {
                // What was converted from XISF stays what it was, in the other container too.
                hdu.properties = std::move(img.properties);
                hdu.wcsDigest = img.wcsDigest;
            }
            if (source.notes) {
                std::string line = label + origin(img) + ": " + img.note + ", rows " +
                                   (img.hasRowOrder || opt.rowOrderGiven || !img.generic ? "" : "assumed ") +
                                   (topDown ? "top-down" : "bottom-up") + " (kept)";
                if (!boundsNote.empty()) line += "; " + boundsNote;
                info(line);
            }
            hdus.push_back(std::move(hdu));
        }
        try {
            if (format == Format::Asdf) {
                AsdfWriteOptions aopt;
                aopt.codec = asdfCodec(opt);
                if (opt.properties) aopt.metadata = std::move(fits.properties);
                writeAsdf(tmpPath, hdus, aopt);
            } else {
                FitsWriteOptions fopt = fitsOptions;
                if (opt.properties) fopt.metadata = std::move(fits.properties);
                writeFits(tmpPath, hdus, fopt);
            }
            replaceFile(tmpPath, outPath);
        } catch (...) {
            removeFile(tmpPath);
            throw;
        }
        return;
    }

    std::vector<XisfOutImage> out;
    for (size_t idx : indices) {
        FitsImage& img = fits.images[idx];
        Source px = sourceOf(img);
        const std::string label = "image " + std::to_string(idx);

        // XISF stores rows top-down. FITS rows are bottom-up unless ROWORDER (or the user) says otherwise.
        const bool topDown = opt.rowOrderGiven ? !opt.bottomUp : img.topDown;
        // The XISF properties an image carries are its properties again. An astrometric solution
        // among them describes the image as it was: it is the solution still if the WCS keywords
        // (which were written from it, or with it) are the same and the rows are where they were.
        const bool carried = opt.properties && !img.properties.empty();
        bool sameWcs = carried && topDown == img.topDown &&
                       wcsDigest(img.keywords, px->width, px->height, !img.topDown) == img.wcsDigest;
        if (carried && img.propertiesGiven) {
            // The caller's own properties: a solution among them stands as it is given. Without
            // one, a solution is made from the WCS keywords as for an image without properties.
            bool solution = false;
            for (const auto& p : img.properties) solution = solution || isSolutionProperty(p.id);
            sameWcs = solution || !opt.wcs;
        }
        if (!topDown) {
            px = flippedSource(px);
            flipBayerRows(img.keywords, px->height);
            flipCfaRows(img);
        }
        // PixInsight interprets WCS keywords in the FITS bottom-up convention even though XISF
        // rows are top-down, so keywords describing top-down rows are converted. (Those of an
        // image handed over in memory may describe the other order than its pixels have.)
        if (img.wcsTopDown ? *img.wcsTopDown : topDown) flipWcsRowOrder(img.keywords, px->height);

        XisfOutImage o;
        o.id = img.name.empty() ? source.defaultName : img.name;
        o.rgb = px->channels == 3;
        std::string boundsNote;
        if (isFloat(px->format)) {
            const auto b = floatBounds(img, opt, boundsNote);
            o.lowerBound = b.first;
            o.upperBound = b.second;
            if (img.hasNaN) warn(label + ": the data contains NaN/Inf values, which are copied as they are");
        }
        if (opt.bits && *opt.bits != px->format) {
            const bool wasFloat = isFloat(px->format);
            px = convertedSource(px, *opt.bits, o.lowerBound, o.upperBound);
            if (isFloat(px->format) && !wasFloat) {  // integers are normalized to [0,1]
                o.lowerBound = 0;
                o.upperBound = 1;
            }
        }
        o.pixels = px;
        if (px->channels == 1 && !img.cfaPattern.empty()) {
            // the colour filter array of a DNG file, of any size (an X-Trans sensor's is 6 x 6)
            o.cfaPattern = img.cfaPattern;
            o.cfaWidth = img.cfaWidth;
            o.cfaHeight = img.cfaHeight;
        } else if (px->channels == 1) {
            const FitsKeyword* bp = findKeyword(img.keywords, "BAYERPAT");
            const std::string pattern = bp ? toUpper(fitsUnquote(bp->value)) : std::string();
            auto offsetIsZero = [&](const char* key) {
                const FitsKeyword* k = findKeyword(img.keywords, key);
                double v = 0;
                return !k || (parseDouble(k->value, v) && v == 0);
            };
            if (pattern.size() == 4 && pattern.find_first_not_of("RGB") == std::string::npos &&
                offsetIsZero("XBAYROFF") && offsetIsZero("YBAYROFF")) {
                o.cfaPattern = pattern;
                o.cfaWidth = o.cfaHeight = 2;
            }
        }
        o.keywords = img.keywords;
        o.iccProfile = img.iccProfile;
        std::string solutionNote, propertyNote;
        if (sameWcs) {
            o.properties = std::move(img.properties);
            bool solution = false;
            for (const auto& p : o.properties) solution = solution || isSolutionProperty(p.id);
            propertyNote = countOf(o.properties.size(), "XISF property", "XISF properties") + " restored" +
                           (solution ? ", the astrometric solution among them" : "");
        } else if (opt.wcs) {
            // The keywords are now in the bottom-up convention PixInsight uses. PixInsight reads only
            // their linear part, so the solution is also written as its native properties.
            if (!wcsToAstrometricSolution(o.keywords, px->width, px->height, o.properties, solutionNote) &&
                !solutionNote.empty()) {
                warn(label + ": no PixInsight solution properties written: " + solutionNote);
                solutionNote.clear();
            }
        }
        if (carried && !sameWcs) {
            // What the keywords say now comes first: the solution that was carried is not written,
            // and neither is a property that was made from the keywords just now.
            const size_t made = o.properties.size();
            size_t restored = 0;
            bool solution = false;
            for (auto& p : img.properties) {
                if (isSolutionProperty(p.id)) {
                    solution = true;
                    continue;
                }
                bool again = false;
                for (size_t k = 0; k < made && !again; ++k) {
                    again = o.properties[k].id == p.id;
                    // (the reference system and the equinox belong to the solution that is made)
                    if (again && img.propertiesGiven && !p.array && o.properties[k].text != p.text) {
                        double made_ = 0, given = 0;   // (2000 and 2000.0 are one number)
                        if (parseDouble(o.properties[k].text, made_) && parseDouble(p.text, given) && made_ == given) continue;
                        warn(label + ": property " + p.id + " is written as the WCS keywords have it (" + o.properties[k].text +
                             "), not as it was given (" + p.text + "): the astrometric solution is made from them");
                    }
                }
                if (again) continue;
                o.properties.push_back(std::move(p));
                ++restored;
            }
            propertyNote = countOf(restored, "XISF property", "XISF properties") + " restored";
            if (solution) {
                propertyNote += std::string("; not the astrometric solution among them: ") +
                                (topDown != img.topDown ? "the rows are taken in the other order than the file says"
                                                        : "the WCS keywords, the size of the image or the order of its rows "
                                                          "changed since it was written") +
                                (made ? ", so it is made from the keywords" : "");
            }
        }
        if (!history.empty()) o.keywords.push_back({"HISTORY", "", history});
        if (source.notes) {
            const bool assumed = img.generic && !img.hasRowOrder && !opt.rowOrderGiven;
            std::string line = label + origin(img) + ": " + img.note + ", rows " +
                               (topDown ? "top-down (kept)"
                                        : assumed ? "assumed bottom-up (flipped to XISF's top-down order; add --top-down "
                                                    "if the image comes out upside down)"
                                                  : "bottom-up (flipped to XISF's top-down order)");
            if (!boundsNote.empty()) line += "; " + boundsNote;
            info(line);
            if (!solutionNote.empty()) info(label + ": " + solutionNote);
            if (!propertyNote.empty()) info(label + ": " + propertyNote);
        }
        out.push_back(std::move(o));
    }

    XisfWriteOptions wopt;
    if (opt.compress) wopt.codec = !opt.codec.empty() ? opt.codec : (zstdAvailable() ? "zstd" : "zlib");
    wopt.checksum = opt.checksum;
    wopt.subblockSize = opt.subblockSize;
    wopt.level = opt.level;
    wopt.shuffle = opt.shuffle;
    wopt.creatorApplication = opt.creatorApplication;
    if (opt.properties) wopt.metadata = std::move(fits.properties);

    wopt.blocksPath = blocksFile.tmpPath;
    wopt.blocksName = blocksFile.name;
    try {
        writeXisf(tmpPath, out, wopt);
        replaceUnit(tmpPath, outPath, blocksFile.tmpPath, blocksFile.path);
    } catch (...) {
        removeFile(tmpPath);
        if (blocksFile.wanted()) removeFile(blocksFile.tmpPath);
        throw;
    }
}

}  // namespace xisfconv
