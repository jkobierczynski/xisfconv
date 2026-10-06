// Converting whole files: XISF to FITS, ASDF, TIFF or PNG; FITS and ASDF to XISF, to each other,
// or to TIFF or PNG; and rewriting an XISF file, also in place.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
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
        const int src = static_cast<int>((imageHeight - 1 - static_cast<uint64_t>(j)) % static_cast<uint64_t>(ph));
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

// Makes the image the picture that was asked for, if one was. Returns what happened, for the
// notes, and how many pixels of the picture there are for one of the image in width and in
// height (what a resolution in pixels per inch is to be multiplied by).
std::string makeSmaller(PixelBuffer& px, const Downsample& how, double& perPixelX, double& perPixelY) {
    const uint64_t fullWidth = px.width, fullHeight = px.height;
    perPixelX = perPixelY = 1;
    if (!how.any()) return {};
    const DownsampledSize size = downsampledSize(how, px.width, px.height);
    if (!size.changes) return {};
    downsample(px, size);
    perPixelX = static_cast<double>(size.width) / static_cast<double>(size.useWidth);
    perPixelY = static_cast<double>(size.height) / static_cast<double>(size.useHeight);
    std::string note = std::to_string(fullWidth) + " x " + std::to_string(fullHeight) + " pixels averaged to " +
                       std::to_string(px.width) + " x " + std::to_string(px.height);
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
std::string partPathFor(const std::string& outPath, const std::string& input, bool force) {
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

void replaceFile(const std::string& tmpPath, const std::string& outPath) {
    std::error_code ec;
    fs::rename(toPath(tmpPath), toPath(outPath), ec);
    if (ec) {
        // where a file cannot be renamed over an existing one
        fs::remove(toPath(outPath), ec);
        fs::rename(toPath(tmpPath), toPath(outPath));   // throws std::filesystem::filesystem_error: an I/O error
    }
}

void removeFile(const std::string& path) {
    std::error_code ec;
    fs::remove(toPath(path), ec);
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
void checkOutput(const std::string& outPath, const std::string& input, bool force) {
    notInPlaceOfSomethingElse(outPath);
    if (fs::exists(toPath(outPath)) && !force) throw Error(outPath + " already exists (use --force to overwrite)", ErrorKind::Exists);
    if (!input.empty() && fs::exists(toPath(outPath)) && fs::equivalent(toPath(outPath), toPath(input))) {
        throw Error("output would overwrite the input file", ErrorKind::Argument);
    }
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

}  // namespace

InputFormat detectInputFormat(const std::string& path) {
    if (looksLikeFits(path)) return InputFormat::Fits;
    if (looksLikeAsdf(path)) return InputFormat::Asdf;
    return InputFormat::Xisf;
}

XisfFileRewrite rewriteXisfFile(const std::string& input, const std::string& output, bool inPlace, bool force,
                                XisfRewriteOptions ropt) {
    XisfFileRewrite done;
    std::error_code sizeError;
    done.inputSize = static_cast<uint64_t>(fs::file_size(toPath(input), sizeError));
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
    if (!same && fs::exists(toPath(outPath)) && !force) {
        throw Error(outPath + " already exists (use --force to overwrite)", ErrorKind::Exists);
    }
    const fs::perms permissions = fs::status(toPath(input)).permissions();
    if (same && (permissions & (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write)) == fs::perms::none) {
        throw Error("the file is read-only; it is not replaced", ErrorKind::Io);
    }
    ropt.readBack = ropt.readBack || same;  // a file that replaces its source is always read back first

    // Replacing a file by an identical one would only cost time: such files are left alone.
    if (same && xisfStoredAsRequested(input, ropt)) {
        done.unchanged = true;
        return done;
    }

    const std::string tmpPath = partPathFor(outPath, input, force);
    XisfRewriteResult& r = done.result;
    try {
        r = rewriteXisf(input, tmpPath, ropt);
        if (!same) replaceFile(tmpPath, outPath);
    } catch (...) {
        removeFile(tmpPath);
        throw;
    }
    if (same && !r.changed) {
        removeFile(tmpPath);
        done.unchanged = true;
        return done;
    }
    if (same) {
        // The original is only ever replaced by a rename, once the new file is on the disk with
        // the permissions of the old one; if the rename fails both files stay.
        std::error_code ec;
        fs::permissions(toPath(tmpPath), permissions, ec);
        syncToDisk(tmpPath, false);
        fs::rename(toPath(tmpPath), toPath(outPath), ec);
        if (ec) {
            throw Error("could not replace the file (" + ec.message() + "); the rewritten copy is kept as " + tmpPath, ErrorKind::Io);
        }
        const fs::path parent = toPath(outPath).parent_path();
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

void flipKeywordRows(std::vector<FitsKeyword>& keywords, uint64_t height) {
    flipBayerRows(keywords, height);
    flipWcsRowOrder(keywords, height);
}

void convertXisfFile(const std::string& input, const std::string& outPath, Format format, const ConvertOptions& opt) {
    if (format == Format::Xisf) throw Error("XISF to XISF is a rewrite, not a conversion", ErrorKind::Argument);
    downsampleIsForPictures(opt, format);
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

    checkOutput(outPath, input, opt.force);

    std::vector<PixelBuffer> buffers;
    std::vector<std::string> stretchNotes;  // HISTORY text per converted image
    std::vector<std::pair<double, double>> shrunk;   // pixels of each picture per pixel of its image, in width and height
    buffers.reserve(indices.size());
    for (size_t idx : indices) {
        progress("reading", buffers.size(), indices.size());
        const XisfImage& img = file.images()[idx];
        PixelBuffer px = file.readPixels(idx, opt.verify);
        // A smaller picture is made of the image as it is stored: the mean of linear data is
        // what larger pixels would have recorded. A stretch comes after, on the picture.
        double perPixelX = 1, perPixelY = 1;
        const std::string smaller = makeSmaller(px, opt.downsample, perPixelX, perPixelY);
        if (!smaller.empty()) info("image " + std::to_string(idx) + ": " + smaller);
        shrunk.emplace_back(perPixelX, perPixelY);
        if (opt.stretch != Stretch::None) {
            const size_t colorChannels = img.colorSpace == "Gray" ? 1 : 3;
            std::vector<StretchParams> params;
            std::string how;
            const DisplayFunction& df = img.displayFunction;
            const bool useStored = df.present && !df.isIdentity() &&
                                   (opt.stretch == Stretch::Auto || opt.stretch == Stretch::Stored);
            if (useStored) {
                for (size_t c = 0; c < std::min<size_t>(colorChannels, px.channels); ++c) {
                    params.push_back({df.s[c], df.m[c], df.h[c], df.l[c], df.r[c]});
                }
                how = "PixInsight STF";
            } else {
                if (opt.stretch == Stretch::Stored) {
                    throw Error("image " + std::to_string(idx) + " has no saved STF (DisplayFunction); "
                                "use --stretch=linked or --stretch=unlinked", ErrorKind::NotFound);
                }
                const bool linked = opt.stretch != Stretch::Unlinked;
                params = autoStretch(px, img.lowerBound, img.upperBound, colorChannels, linked);
                how = linked ? "linked auto-STF" : "unlinked auto-STF";
            }
            applyStretch(px, params, img.lowerBound, img.upperBound);
            SampleFormat target = isFloat(img.format) ? (fitsLike ? SampleFormat::Float32
                                                                                : SampleFormat::UInt16)
                                                      : img.format;
            if (opt.bits) target = *opt.bits;
            convertSampleFormat(px, target, 0, 1);
            const std::string desc = stretchDescription(how, params);
            stretchNotes.push_back("Stretched with " + desc);
            info("image " + std::to_string(idx) + ": " + desc);
        } else if (opt.bits) {
            convertSampleFormat(px, *opt.bits, img.lowerBound, img.upperBound);
        }
        if (format == Format::Png) {
            if (px.format != SampleFormat::UInt8 && px.format != SampleFormat::UInt16) {
                convertSampleFormat(px, SampleFormat::UInt16, opt.stretch == Stretch::None ? img.lowerBound : 0,
                                    opt.stretch == Stretch::None ? img.upperBound : 1);
            }
            const uint64_t colorCh = (img.colorSpace != "Gray" && px.channels >= 3) ? 3 : 1;
            if (px.channels > colorCh + 1) {
                warn("PNG: keeping " + std::to_string(colorCh + 1) + " of " + std::to_string(px.channels) +
                     " channels (color + alpha)");
                px.channels = colorCh + 1;
                px.data.resize(static_cast<size_t>(px.samples()) * sampleBytes(px.format));
            }
            if (isFloat(img.format) && opt.stretch == Stretch::None) {
                info("linear data may look dark in PNG; add --stretch for a viewable image");
            }
        }
        if (fitsLike && opt.bottomUp) flipVertical(px);
        if (format == Format::Tiff && (px.format == SampleFormat::UInt32 || px.format == SampleFormat::UInt64 ||
                                       px.format == SampleFormat::Float64)) {
            warn(std::string("image ") + std::to_string(idx) + ": " + sampleFormatName(px.format) +
                 " TIFF is not supported by many programs; consider --bits u16 or --bits f32");
        }
        if (opt.stretch == Stretch::None && isFloat(img.format) && !isFloat(px.format) && !img.boundsDeclared) {
            warn("image " + std::to_string(idx) + " has no bounds attribute; assuming [0,1]");
        }
        buffers.push_back(std::move(px));
    }

    const std::string tmpPath = partPathFor(outPath, input, opt.force);
    progress("writing", 0, 0);
    try {
        if (fitsLike) {
            std::vector<FitsHdu> hdus;
            for (size_t n = 0; n < indices.size(); ++n) {
                const size_t idx = indices[n];
                const XisfImage& img = file.images()[idx];
                FitsHdu hdu;
                hdu.pixels = &buffers[n];
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
                        hdu.wcsDigest = wcsDigest(hdu.keywords, buffers[n].width, buffers[n].height, opt.bottomUp);
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
                page.pixels = &buffers[n];
                page.rgb = img.colorSpace != "Gray";
                if (img.colorSpace == "CIELab") warn("CIELab image written as RGB samples without color conversion");
                if (img.hasIccProfile) {
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
            png.pixels = &buffers[0];
            png.rgb = img.colorSpace != "Gray" && buffers[0].channels >= 3;
            if (img.hasIccProfile) {
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
    if (kind == InputFormat::Xisf) throw Error("not a FITS or ASDF file", ErrorKind::Argument);
    if (format == Format::Fits) fitsStorage(opt, outPath);   // an option that does not apply is reported before the file is read
    if (format == Format::Asdf) asdfCodec(opt);
    const bool asdfInput = kind == InputFormat::Asdf;
    const char* inputName = asdfInput ? "ASDF" : "FITS";
    const bool exporting = format == Format::Tiff || format == Format::Png;
    if (asdfInput && format == Format::Asdf) {
        throw Error("the input is already an ASDF file; choose xisf, fits, tiff or png as output", ErrorKind::Argument);
    }
    if (opt.stretch != Stretch::None && !exporting) {
        throw Error(std::string("--stretch is for viewing: from ") + inputName + " input it is available for TIFF and PNG output",
                    ErrorKind::Argument);
    }
    downsampleIsForPictures(opt, format);
    if (opt.stretch == Stretch::Stored) {
        throw Error(std::string(inputName) + " files carry no saved STF; use --stretch, --stretch=linked or --stretch=unlinked",
                    ErrorKind::NotFound);
    }
    if (format == Format::Png && opt.bits && *opt.bits != SampleFormat::UInt8 && *opt.bits != SampleFormat::UInt16) {
        throw Error("PNG supports only --bits u8 or u16", ErrorKind::Argument);
    }

    progress("reading", 0, 0);
    FitsFile fits = asdfInput ? readAsdf(input, false, opt.verify) : readFits(input);
    for (const auto& s : fits.skipped) warn("skipped " + s);
    if (fits.images.empty()) throw Error(std::string("no image data found in this ") + inputName + " file");
    if (!asdfInput && format == Format::Fits) {
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
    writeImageSet(fits, origin, outPath, format, opt);
}

std::pair<double, double> automaticBounds(const FitsImage& image) {
    std::string how;
    return floatBounds(image, ConvertOptions(), how);
}

void flipImageRows(FitsImage& img) {
    flipVertical(img.pixels);
    flipKeywordRows(img.keywords, img.pixels.height);
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
        return asdfInput ? " (" + img.source + ")" : " (HDU " + std::to_string(img.hduIndex) + ")";
    };
    // Images that come from a file say so in their header; images handed over in memory do not.
    const std::string history = source.format.empty() ? std::string() : "Converted from " + source.format + " by xisfconv " + kVersion;
    const std::string tmpPath = partPathFor(outPath, input, opt.force);
    progress("writing", 0, 0);

    if (exporting) {
        // TIFF and PNG: rows top-down, floating point data scaled so that its range is 0..1.
        std::vector<PixelBuffer> buffers;   // one per page; planes of a cube that is not RGB become pages
        std::vector<std::string> names;
        std::vector<std::vector<uint8_t>> profiles;   // ICC profiles, only for images handed over in memory
        for (size_t idx : indices) {
            FitsImage& img = fits.images[idx];
            PixelBuffer& px = img.pixels;
            const std::string label = "image " + std::to_string(idx);
            const bool topDown = opt.rowOrderGiven ? !opt.bottomUp : img.topDown;
            if (!topDown) flipVertical(px);
            double perPixelX = 1, perPixelY = 1;
            const std::string smaller = makeSmaller(px, opt.downsample, perPixelX, perPixelY);   // before a stretch, as from XISF
            if (!smaller.empty() && img.hasNaN) {
                // what was no number is left out of the means: is there still a pixel that is none?
                const double low = img.dataMin, high = img.dataMax;   // (the range stays that of the image)
                img.hasNaN = false;
                updateFloatRange(img);
                img.dataMin = low;
                img.dataMax = high;
            }
            const SampleFormat stored = px.format;
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
                const auto params = autoStretch(px, range.first, range.second, static_cast<size_t>(px.channels), linked);
                applyStretch(px, params, range.first, range.second);   // leaves Float32 in [0,1]
                range = {0.0, 1.0};
                // As for XISF input: 16-bit for floating point data, the stored type for integers.
                convertSampleFormat(px, opt.bits ? *opt.bits : wasFloat ? SampleFormat::UInt16 : stored, 0, 1);
                stretchNote = stretchDescription(linked ? "linked auto-STF" : "unlinked auto-STF", params);
            } else if (opt.bits) {
                convertSampleFormat(px, *opt.bits, range.first, range.second);
            }
            if (format == Format::Png && px.format != SampleFormat::UInt8 && px.format != SampleFormat::UInt16) {
                convertSampleFormat(px, SampleFormat::UInt16, range.first, range.second);
            }
            bool scaled = false;
            if (isFloat(px.format) && wasFloat && (range.first != 0 || range.second != 1)) {
                normalizeFloat(px, range.first, range.second);
                scaled = true;
            }
            if (format == Format::Tiff && (px.format == SampleFormat::UInt32 || px.format == SampleFormat::UInt64 ||
                                           px.format == SampleFormat::Float64)) {
                warn(label + ": " + sampleFormatName(px.format) +
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
            if (px.channels == 1 || px.channels == 3) {
                buffers.push_back(std::move(px));
                names.push_back(name);
                profiles.push_back(img.iccProfile);
                continue;
            }
            // A cube that is not an RGB image: one grayscale page per plane.
            const uint64_t planes = format == Format::Png ? 1 : px.channels;
            if (format == Format::Png) {
                warn(label + ": PNG holds one image; writing the first of " + std::to_string(px.channels) + " planes");
            }
            const size_t planeBytes = static_cast<size_t>(px.planeSamples()) * sampleBytes(px.format);
            for (uint64_t c = 0; c < planes; ++c) {
                PixelBuffer plane;
                plane.width = px.width;
                plane.height = px.height;
                plane.channels = 1;
                plane.format = px.format;
                plane.data.assign(px.data.begin() + static_cast<std::ptrdiff_t>(c * planeBytes),
                                  px.data.begin() + static_cast<std::ptrdiff_t>((c + 1) * planeBytes));
                buffers.push_back(std::move(plane));
                names.push_back(name + " plane " + std::to_string(c));
                profiles.push_back(img.iccProfile);
            }
            px.data.clear();
            px.data.shrink_to_fit();
        }
        try {
            if (format == Format::Tiff) {
                std::vector<TiffPage> pages;
                for (size_t n = 0; n < buffers.size(); ++n) {
                    TiffPage page;
                    page.pixels = &buffers[n];
                    page.rgb = buffers[n].channels == 3;
                    page.iccProfile = profiles[n];
                    page.description = names[n];
                    pages.push_back(std::move(page));
                }
                writeTiff(tmpPath, pages, opt.compress);
            } else {
                PngImage png;
                png.pixels = &buffers[0];
                png.rgb = buffers[0].channels == 3;
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
            PixelBuffer& px = img.pixels;
            const std::string label = "image " + std::to_string(idx);
            const bool topDown = opt.rowOrderGiven ? !opt.bottomUp : img.topDown;
            std::string boundsNote;
            if (opt.bits && *opt.bits != px.format) {
                std::pair<double, double> bounds{0.0, 1.0};
                if (isFloat(px.format)) bounds = floatBounds(img, opt, boundsNote);
                convertSampleFormat(px, *opt.bits, bounds.first, bounds.second);
            }
            if (img.hasNaN && isFloat(px.format)) warn(label + ": the data contains NaN/Inf values, which are copied as they are");
            FitsHdu hdu;
            hdu.pixels = &px;
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
        PixelBuffer& px = img.pixels;
        const std::string label = "image " + std::to_string(idx);

        // XISF stores rows top-down. FITS rows are bottom-up unless ROWORDER (or the user) says otherwise.
        const bool topDown = opt.rowOrderGiven ? !opt.bottomUp : img.topDown;
        // The XISF properties an image carries are its properties again. An astrometric solution
        // among them describes the image as it was: it is the solution still if the WCS keywords
        // (which were written from it, or with it) are the same and the rows are where they were.
        const bool carried = opt.properties && !img.properties.empty();
        bool sameWcs = carried && topDown == img.topDown &&
                       wcsDigest(img.keywords, px.width, px.height, !img.topDown) == img.wcsDigest;
        if (carried && img.propertiesGiven) {
            // The caller's own properties: a solution among them stands as it is given. Without
            // one, a solution is made from the WCS keywords as for an image without properties.
            bool solution = false;
            for (const auto& p : img.properties) solution = solution || isSolutionProperty(p.id);
            sameWcs = solution || !opt.wcs;
        }
        if (!topDown) {
            flipVertical(px);
            flipBayerRows(img.keywords, px.height);
        }
        // PixInsight interprets WCS keywords in the FITS bottom-up convention even though XISF
        // rows are top-down, so keywords describing top-down rows are converted. (Those of an
        // image handed over in memory may describe the other order than its pixels have.)
        if (img.wcsTopDown ? *img.wcsTopDown : topDown) flipWcsRowOrder(img.keywords, px.height);

        XisfOutImage o;
        o.pixels = &px;
        o.id = img.name.empty() ? source.defaultName : img.name;
        o.rgb = px.channels == 3;
        std::string boundsNote;
        if (isFloat(px.format)) {
            const auto b = floatBounds(img, opt, boundsNote);
            o.lowerBound = b.first;
            o.upperBound = b.second;
            if (img.hasNaN) warn(label + ": the data contains NaN/Inf values, which are copied as they are");
        }
        if (opt.bits && *opt.bits != px.format) {
            const bool wasFloat = isFloat(px.format);
            convertSampleFormat(px, *opt.bits, o.lowerBound, o.upperBound);
            if (isFloat(px.format) && !wasFloat) {  // integers are normalized to [0,1]
                o.lowerBound = 0;
                o.upperBound = 1;
            }
        }
        if (px.channels == 1) {
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
            if (!wcsToAstrometricSolution(o.keywords, px.width, px.height, o.properties, solutionNote) &&
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

    try {
        writeXisf(tmpPath, out, wopt);
        replaceFile(tmpPath, outPath);
    } catch (...) {
        removeFile(tmpPath);
        throw;
    }
}

}  // namespace xisfconv
