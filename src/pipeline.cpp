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

// Refuses an output that exists (unless it may be overwritten) or that is the input itself.
void checkOutput(const std::string& outPath, const std::string& input, bool force) {
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

void convertXisfFile(const std::string& input, const std::string& outPath, Format format, const ConvertOptions& opt) {
    if (format == Format::Xisf) throw Error("XISF to XISF is a rewrite, not a conversion", ErrorKind::Argument);
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
    buffers.reserve(indices.size());
    for (size_t idx : indices) {
        progress("reading", buffers.size(), indices.size());
        const XisfImage& img = file.images()[idx];
        PixelBuffer px = file.readPixels(idx, opt.verify);
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
                hdu.keywords = img.keywords;
                if (opt.propertyKeywords) addPropertyKeywords(file, idx, hdu.keywords);
                if (opt.bottomUp) {
                    if (FitsKeyword* bp = findKeyword(hdu.keywords, "BAYERPAT")) {
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
                    flipWcsRowOrder(hdu.keywords, img.height);
                }
                if (opt.wcs && !hasKeyword(hdu.keywords, "CTYPE1")) {
                    WcsResult wcs;
                    if (astrometricSolutionToWcs(file, idx, opt.bottomUp, opt.sipOrder, wcs)) {
                        hdu.keywords.insert(hdu.keywords.end(), wcs.keywords.begin(), wcs.keywords.end());
                        info("image " + std::to_string(idx) + ": " + wcs.summary);
                    }
                }
                hdu.keywords.push_back({"HISTORY", "", std::string("Converted from XISF by xisfconv ") + kVersion});
                if (n < stretchNotes.size()) hdu.keywords.push_back({"HISTORY", "", stretchNotes[n]});
                hdus.push_back(std::move(hdu));
            }
            if (format == Format::Asdf) {
                AsdfWriteOptions aopt;
                if (opt.compress) aopt.codec = opt.codec.empty() ? "zlib" : opt.codec;
                writeAsdf(tmpPath, hdus, aopt);
            } else {
                writeFits(tmpPath, hdus);
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
                    page.xResolution = img.resolution.horizontal;
                    page.yResolution = img.resolution.vertical;
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
                png.pixelsPerMeter = img.resolution.horizontal / (img.resolution.unit == "cm" ? 0.01 : 0.0254);
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
        // FITS -> FITS has one use: writing tile-compressed images as plain ones.
        bool tiled = false;
        for (const auto& img : fits.images)
            if (!img.tileCompression.empty()) tiled = true;
        if (!tiled) {
            throw Error("the input is already a FITS file; choose xisf, asdf, tiff or png as output", ErrorKind::Argument);
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
    if (FitsKeyword* bp = findKeyword(img.keywords, "BAYERPAT")) {
        const std::string pattern = fitsUnquote(bp->value);
        if (pattern.size() == 4) bp->value = fitsString(flipPatternRows(pattern, 2, 2, img.pixels.height));
        else warn("cannot adjust BAYERPAT " + bp->value + " for the changed row order; check it manually");
    }
    flipWcsRowOrder(img.keywords, img.pixels.height);
    img.topDown = !img.topDown;
    img.hasRowOrder = true;
}

void writeImageSet(FitsFile& fits, const ImageSetOrigin& source, const std::string& outPath, Format format,
                   const ConvertOptions& opt) {
    const bool asdfInput = source.format == "ASDF";
    const bool exporting = format == Format::Tiff || format == Format::Png;
    const std::string& input = source.input;
    if (fits.images.empty()) throw Error("no images to write", ErrorKind::Argument);

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
                if (opt.compress) aopt.codec = opt.codec.empty() ? "zlib" : opt.codec;
                writeAsdf(tmpPath, hdus, aopt);
            } else {
                writeFits(tmpPath, hdus);
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
        if (!topDown) {
            flipVertical(px);
            if (FitsKeyword* bp = findKeyword(img.keywords, "BAYERPAT")) {
                const std::string pattern = fitsUnquote(bp->value);
                if (pattern.size() == 4) bp->value = fitsString(flipPatternRows(pattern, 2, 2, px.height));
                else warn("cannot adjust BAYERPAT " + bp->value + " for the changed row order; check it manually");
            }
        } else {
            // PixInsight interprets WCS keywords in the FITS bottom-up convention even though XISF
            // rows are top-down, so keywords describing top-down rows are converted.
            flipWcsRowOrder(img.keywords, px.height);
        }

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
        std::string solutionNote;
        if (opt.wcs) {
            // The keywords are now in the bottom-up convention PixInsight uses. PixInsight reads only
            // their linear part, so the solution is also written as its native properties.
            if (!wcsToAstrometricSolution(o.keywords, px.width, px.height, o.properties, solutionNote) &&
                !solutionNote.empty()) {
                warn(label + ": no PixInsight solution properties written: " + solutionNote);
                solutionNote.clear();
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
        }
        out.push_back(std::move(o));
    }

    XisfWriteOptions wopt;
    if (opt.compress) wopt.codec = !opt.codec.empty() ? opt.codec : (zstdAvailable() ? "zstd" : "zlib");
    wopt.checksum = opt.checksum;
    wopt.subblockSize = opt.subblockSize;

    try {
        writeXisf(tmpPath, out, wopt);
        replaceFile(tmpPath, outPath);
    } catch (...) {
        removeFile(tmpPath);
        throw;
    }
}

}  // namespace xisfconv
