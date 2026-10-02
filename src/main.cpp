// xisfconv - convert PixInsight XISF images to FITS, TIFF or PNG, and FITS images to XISF.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "codecs.hpp"
#include "common.hpp"
#include "convert.hpp"
#include "fits.hpp"
#include "fitsread.hpp"
#include "png.hpp"
#include "tiff.hpp"
#include "wcs.hpp"
#include "xisf.hpp"
#include "xisfwrite.hpp"

namespace fs = std::filesystem;
using namespace xisfconv;

namespace {

enum class Format { Fits, Tiff, Png, Xisf };

enum class Stretch { None, Auto, Linked, Unlinked, Stored };

struct Options {
    Stretch stretch = Stretch::None;
    std::optional<Format> format;
    std::string output;
    std::string outdir;
    std::optional<SampleFormat> bits;
    std::optional<size_t> imageIndex;
    bool compress = false;
    bool bottomUp = true;  // FITS convention: first stored row is the bottom of the image
    bool rowOrderGiven = false;  // --top-down / --bottom-up given explicitly (overrides ROWORDER on FITS input)
    std::string codec;           // XISF output: zlib or zstd
    std::string checksum;        // XISF output: sha1, sha256 or sha512
    std::optional<std::pair<double, double>> bounds;  // XISF output: range of floating point data
    uint64_t subblockSize = 1u << 30;
    bool propertyKeywords = true;
    bool verify = true;
    bool wcs = true;
    int sipOrder = 3;
    bool force = false;
    bool info = false;
    bool dumpHeader = false;
    bool quiet = false;
    std::vector<std::string> inputs;
};

void usage(std::ostream& os) {
    os << "xisfconv " << kVersion << " - convert PixInsight XISF images to FITS, TIFF or PNG, and FITS to XISF\n\n"
          "Usage: xisfconv [options] <file>...\n"
          "       XISF inputs are converted to FITS (default), TIFF or PNG; FITS inputs to XISF.\n\n"
          "Output:\n"
          "  -t, --to <fits|tiff|png|xisf>  output format (default: fits for XISF input, xisf for FITS input,\n"
          "                              or taken from -o's extension)\n"
          "  -o, --output <file>         output file name (single input only)\n"
          "  -d, --outdir <dir>          directory for output files (default: next to each input)\n"
          "  -f, --force                 overwrite existing output files\n\n"
          "Conversion:\n"
          "  -b, --bits <fmt>            output sample format: u8, u16, u32, f32, f64 (default: as stored)\n"
          "  -i, --image <n>             convert only image n (0-based); default: all images\n"
          "                              (FITS: extra images become IMAGE extensions; TIFF: extra pages)\n"
          "  -c, --compress              TIFF: Deflate compression with predictor\n"
          "                              XISF: compress the pixel data (zstd, or zlib without libzstd)\n"
          "  -s, --stretch[=mode]        apply a screen stretch for viewing linear data:\n"
          "                                auto     (default) the STF saved by PixInsight if any, else linked\n"
          "                                linked   auto-STF with shared statistics (keeps color balance)\n"
          "                                unlinked auto-STF per channel (neutralizes color casts)\n"
          "                                stf      only the STF saved in the file\n"
          "                              (TIFF: stretched float data becomes 16-bit unless --bits is given)\n"
          "      --top-down              XISF -> FITS: keep XISF's top-down row order (ROWORDER='TOP-DOWN')\n"
          "                              instead of the FITS convention, bottom-up (the default)\n"
          "                              FITS -> XISF: the FITS rows are stored top-down (don't flip them)\n"
          "      --bottom-up             FITS -> XISF: the FITS rows are stored bottom-up, whatever ROWORDER says\n"
          "      --no-property-keywords  FITS: don't add missing keywords (EXPTIME, DATE-OBS, BAYERPAT...)\n"
          "                              derived from XISF properties\n"
          "      --no-wcs                to FITS: don't write WCS keywords from a PixInsight astrometric solution\n"
          "                              to XISF: don't write PixInsight solution properties from WCS keywords\n"
          "      --sip-order <n>         FITS: SIP distortion order fitted to the solution (2-7, default 3; 0 = off)\n"
          "      --no-verify             don't verify data block checksums\n\n"
          "XISF output (FITS -> XISF):\n"
          "      --codec <zlib|zstd>     compression codec (implies --compress); byte shuffling is always used\n"
          "      --checksum <sha1|sha256|sha512>  store a checksum of the pixel data block\n"
          "      --bounds <lo:hi>        range of floating point data (default: 0:1 if the data fits,\n"
          "                              else 0:65535 if it fits, else the data's minimum and maximum)\n\n"
          "Inspection:\n"
          "  -I, --info                  print image geometry, keywords and properties; no conversion\n"
          "      --dump-header           print the raw XML header (XISF) or all keywords (FITS); no conversion\n\n"
          "  -q, --quiet                 suppress warnings\n"
          "  -h, --help                  show this help\n"
          "  -V, --version               show version and enabled codecs\n";
}

std::string lowerExt(const std::string& path) { return toLower(fs::path(path).extension().string()); }

std::optional<Format> formatFromExtension(const std::string& path) {
    const std::string e = lowerExt(path);
    if (e == ".fits" || e == ".fit" || e == ".fts") return Format::Fits;
    if (e == ".tif" || e == ".tiff") return Format::Tiff;
    if (e == ".png") return Format::Png;
    if (e == ".xisf") return Format::Xisf;
    return std::nullopt;
}

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

void printInfo(const XisfFile& f) {
    std::cout << f.path() << ": XISF " << f.version() << ", " << f.fileSize() << " bytes, header "
              << f.headerXml().size() << " bytes, " << f.images().size() << " image(s)\n";
    for (size_t i = 0; i < f.images().size(); ++i) {
        const XisfImage& img = f.images()[i];
        std::cout << "\nImage " << i;
        if (!img.id.empty()) std::cout << " \"" << img.id << "\"";
        std::cout << ": " << img.width << " x " << img.height << " x " << img.channels << ", "
                  << img.sampleFormatText << ", " << img.colorSpace << ", " << (img.planar ? "Planar" : "Normal")
                  << ", " << (img.bigEndian ? "big" : "little") << "-endian\n";
        if (isFloat(img.format)) std::cout << "  bounds:      " << img.lowerBound << " : " << img.upperBound << "\n";
        std::cout << "  location:    " << img.location << "\n";
        if (!img.compression.empty()) std::cout << "  compression: " << img.compression << "\n";
        if (!img.subblocks.empty()) std::cout << "  subblocks:   " << img.subblocks << "\n";
        if (!img.checksum.empty()) std::cout << "  checksum:    " << img.checksum << "\n";
        if (!img.imageType.empty()) std::cout << "  imageType:   " << img.imageType << "\n";
        if (!img.orientation.empty()) std::cout << "  orientation: " << img.orientation << "\n";
        if (img.cfa.present)
            std::cout << "  CFA:         " << img.cfa.pattern << " (" << img.cfa.width << "x" << img.cfa.height << ")"
                      << (img.cfa.name.empty() ? "" : " " + img.cfa.name) << "\n";
        if (img.resolution.present)
            std::cout << "  resolution:  " << img.resolution.horizontal << " x " << img.resolution.vertical << " per "
                      << img.resolution.unit << "\n";
        if (img.hasIccProfile) std::cout << "  ICC profile: yes\n";
        if (img.displayFunction.present) {
            const DisplayFunction& df = img.displayFunction;
            std::cout << "  STF:         " << (df.isIdentity() ? "identity (no stretch)" : "");
            if (!df.isIdentity()) {
                for (int k = 0; k < (img.colorSpace == "Gray" ? 1 : 3); ++k)
                    std::cout << (k ? "; " : "") << "s=" << df.s[k] << " m=" << df.m[k] << " h=" << df.h[k];
            }
            std::cout << "\n";
        }
        if (!img.unsupported.empty()) std::cout << "  NOT CONVERTIBLE: " << img.unsupported << "\n";
        std::cout << "  FITS keywords (" << img.keywords.size() << "):\n";
        for (const auto& k : img.keywords) {
            std::cout << "    " << k.name;
            if (k.name.size() < 8) std::cout << std::string(8 - k.name.size(), ' ');
            if (!k.value.empty()) std::cout << "= " << k.value;
            if (!k.comment.empty()) std::cout << (k.value.empty() ? " " : " / ") << k.comment;
            std::cout << "\n";
        }
        std::cout << "  Properties (" << img.properties.size() << "):\n";
        for (const auto& p : img.properties) {
            std::cout << "    " << p.id << " (" << p.type << ")";
            if (p.hasBlockData) std::cout << " [data block]";
            else {
                std::string v = p.value;
                if (v.size() > 100) v = v.substr(0, 100) + "...";
                for (auto& c : v)
                    if (c == '\n' || c == '\r') c = ' ';
                std::cout << " = " << v;
            }
            std::cout << "\n";
        }
    }
    if (!f.fileProperties().empty()) {
        std::cout << "\nFile metadata (" << f.fileProperties().size() << "):\n";
        for (const auto& p : f.fileProperties()) {
            std::cout << "  " << p.id << " (" << p.type << ")";
            if (p.hasBlockData) std::cout << " [data block]";
            else std::cout << " = " << p.value;
            std::cout << "\n";
        }
    }
}

std::string outputPathFor(const std::string& input, const Options& opt, Format format) {
    if (!opt.output.empty()) return opt.output;
    fs::path p(input);
    fs::path dir = opt.outdir.empty() ? p.parent_path() : fs::path(opt.outdir);
    fs::path name = p.stem();
    name += format == Format::Fits ? ".fits" : format == Format::Tiff ? ".tif" : format == Format::Png ? ".png" : ".xisf";
    return (dir / name).string();
}

void convertXisfFile(const std::string& input, const Options& opt) {
    XisfFile file(input);
    if (opt.dumpHeader) {
        std::cout << file.headerXml() << "\n";
        return;
    }
    if (opt.info) {
        printInfo(file);
        return;
    }

    Format format = Format::Fits;
    if (opt.format) format = *opt.format;
    else if (!opt.output.empty()) {
        if (auto f = formatFromExtension(opt.output)) format = *f;
    }

    std::vector<size_t> indices;
    if (opt.imageIndex) {
        if (*opt.imageIndex >= file.images().size()) {
            throw Error("image index " + std::to_string(*opt.imageIndex) + " out of range (file has " +
                        std::to_string(file.images().size()) + ")");
        }
        indices.push_back(*opt.imageIndex);
    } else {
        for (size_t i = 0; i < file.images().size(); ++i) {
            if (file.images()[i].unsupported.empty()) indices.push_back(i);
            else warn("skipping image " + std::to_string(i) + ": " + file.images()[i].unsupported);
        }
    }
    if (indices.empty()) throw Error("no convertible images in file");
    if (format == Format::Xisf) throw Error("the input is already an XISF file; choose fits, tiff or png as output");
    if (format == Format::Png) {
        if (indices.size() > 1) {
            warn("PNG holds one image; writing image " + std::to_string(indices[0]) + " only (use --image to choose)");
            indices.resize(1);
        }
        if (opt.bits && *opt.bits != SampleFormat::UInt8 && *opt.bits != SampleFormat::UInt16) {
            throw Error("PNG supports only --bits u8 or u16");
        }
    }

    const std::string outPath = outputPathFor(input, opt, format);
    if (fs::exists(outPath) && !opt.force) throw Error(outPath + " already exists (use --force to overwrite)");
    if (fs::exists(outPath) && fs::equivalent(outPath, input)) throw Error("output would overwrite the input file");

    std::vector<PixelBuffer> buffers;
    std::vector<std::string> stretchNotes;  // HISTORY text per converted image
    buffers.reserve(indices.size());
    for (size_t idx : indices) {
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
                                "use --stretch=linked or --stretch=unlinked");
                }
                const bool linked = opt.stretch != Stretch::Unlinked;
                params = autoStretch(px, img.lowerBound, img.upperBound, colorChannels, linked);
                how = linked ? "linked auto-STF" : "unlinked auto-STF";
            }
            applyStretch(px, params, img.lowerBound, img.upperBound);
            SampleFormat target = isFloat(img.format) ? (format == Format::Fits ? SampleFormat::Float32
                                                                                : SampleFormat::UInt16)
                                                      : img.format;
            if (opt.bits) target = *opt.bits;
            convertSampleFormat(px, target, 0, 1);
            std::string desc = how + ":";
            char buf[96];
            for (size_t c = 0; c < params.size(); ++c) {
                std::snprintf(buf, sizeof buf, " c%zu s=%.6f m=%.6f h=%.6f", c, params[c].shadows,
                              params[c].midtones, params[c].highlights);
                desc += buf;
            }
            stretchNotes.push_back("Stretched with " + desc);
            if (!opt.quiet) std::cerr << "info: image " << idx << ": " << desc << "\n";
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
            if (isFloat(img.format) && opt.stretch == Stretch::None && !opt.quiet) {
                std::cerr << "info: linear data may look dark in PNG; add --stretch for a viewable image\n";
            }
        }
        if (format == Format::Fits && opt.bottomUp) flipVertical(px);
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

    const std::string tmpPath = outPath + ".part";
    try {
        if (format == Format::Fits) {
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
                        if (!opt.quiet) std::cerr << "info: image " << idx << ": " << wcs.summary << "\n";
                    }
                }
                hdu.keywords.push_back({"HISTORY", "", std::string("Converted from XISF by xisfconv ") + kVersion});
                if (n < stretchNotes.size()) hdu.keywords.push_back({"HISTORY", "", stretchNotes[n]});
                hdus.push_back(std::move(hdu));
            }
            writeFits(tmpPath, hdus);
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
        std::error_code ec;
        fs::rename(tmpPath, outPath, ec);
        if (ec) {
            fs::remove(outPath, ec);
            fs::rename(tmpPath, outPath);
        }
    } catch (...) {
        std::error_code ec;
        fs::remove(tmpPath, ec);
        throw;
    }
    if (!opt.quiet) std::cout << input << " -> " << outPath << "\n";
}

void printFitsInfo(const FitsFile& f) {
    std::cout << f.path << ": FITS, " << f.fileSize << " bytes, " << f.images.size() << " image HDU(s)\n";
    for (const auto& img : f.images) {
        std::cout << "\nHDU " << img.hduIndex;
        if (!img.name.empty()) std::cout << " \"" << img.name << "\"";
        std::cout << ": " << img.pixels.width << " x " << img.pixels.height << " x " << img.pixels.channels
                  << ", BITPIX " << img.bitpix;
        if (img.bscale != 1 || img.bzero != 0) std::cout << ", BZERO " << img.bzero << ", BSCALE " << img.bscale;
        std::cout << ", rows " << (img.hasRowOrder ? (img.topDown ? "top-down (ROWORDER)" : "bottom-up (ROWORDER)")
                                                    : "bottom-up (FITS default, no ROWORDER)")
                  << "\n";
        std::cout << "  Keywords (" << img.keywords.size() << "):\n";
        for (const auto& k : img.keywords) {
            std::cout << "    " << k.name;
            if (k.name.size() < 8) std::cout << std::string(8 - k.name.size(), ' ');
            if (!k.value.empty()) std::cout << "= " << k.value;
            if (!k.comment.empty()) std::cout << (k.value.empty() ? " " : " / ") << k.comment;
            std::cout << "\n";
        }
    }
    for (const auto& s : f.skipped) std::cout << "\nSkipped " << s << "\n";
}

// Chooses the XISF bounds attribute for floating point data.
std::pair<double, double> floatBounds(const FitsImage& img, const Options& opt, std::string& how) {
    if (opt.bounds) {
        how = "bounds set with --bounds";
        return *opt.bounds;
    }
    if (img.dataMin >= 0 && img.dataMax <= 1) return {0.0, 1.0};
    char buf[160];
    if (img.dataMin >= 0 && img.dataMax <= 65535) {
        std::snprintf(buf, sizeof buf, "float data spans %g..%g: bounds set to 0:65535 (override with --bounds)",
                      img.dataMin, img.dataMax);
        how = buf;
        return {0.0, 65535.0};
    }
    std::snprintf(buf, sizeof buf, "float data spans %g..%g: bounds set to that range (override with --bounds)",
                  img.dataMin, img.dataMax);
    how = buf;
    return {img.dataMin, img.dataMax > img.dataMin ? img.dataMax : img.dataMin + 1};
}

void convertFitsFile(const std::string& input, const Options& opt) {
    if (opt.info || opt.dumpHeader) {
        printFitsInfo(readFits(input, true));
        return;
    }
    Format format = Format::Xisf;
    if (opt.format) format = *opt.format;
    else if (!opt.output.empty()) {
        if (auto f = formatFromExtension(opt.output)) format = *f;
    }
    if (format != Format::Xisf) {
        throw Error("FITS input can only be converted to XISF (omit --to, or use --to xisf)");
    }
    if (opt.stretch != Stretch::None) throw Error("--stretch is not available for XISF output");

    FitsFile fits = readFits(input);
    for (const auto& s : fits.skipped) warn("skipped " + s);
    if (fits.images.empty()) throw Error("no image data found in this FITS file");

    std::vector<size_t> indices;
    if (opt.imageIndex) {
        if (*opt.imageIndex >= fits.images.size()) {
            throw Error("image index " + std::to_string(*opt.imageIndex) + " out of range (file has " +
                        std::to_string(fits.images.size()) + " image(s))");
        }
        indices.push_back(*opt.imageIndex);
    } else {
        for (size_t i = 0; i < fits.images.size(); ++i) indices.push_back(i);
    }

    const std::string outPath = outputPathFor(input, opt, format);
    if (fs::exists(outPath) && !opt.force) throw Error(outPath + " already exists (use --force to overwrite)");
    if (fs::exists(outPath) && fs::equivalent(outPath, input)) throw Error("output would overwrite the input file");

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
        o.id = img.name.empty() ? fs::path(input).stem().string() : img.name;
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
        o.keywords.push_back({"HISTORY", "", std::string("Converted from FITS by xisfconv ") + kVersion});
        if (!opt.quiet) {
            std::cerr << "info: " << label << " (HDU " << img.hduIndex << "): " << img.note << ", rows "
                      << (topDown ? "top-down (kept)" : "bottom-up (flipped to XISF's top-down order)");
            if (!boundsNote.empty()) std::cerr << "; " << boundsNote;
            std::cerr << "\n";
            if (!solutionNote.empty()) std::cerr << "info: " << label << ": " << solutionNote << "\n";
        }
        out.push_back(std::move(o));
    }

    XisfWriteOptions wopt;
    if (opt.compress) wopt.codec = !opt.codec.empty() ? opt.codec : (zstdAvailable() ? "zstd" : "zlib");
    wopt.checksum = opt.checksum;
    wopt.subblockSize = opt.subblockSize;

    const std::string tmpPath = outPath + ".part";
    try {
        writeXisf(tmpPath, out, wopt);
        std::error_code ec;
        fs::rename(tmpPath, outPath, ec);
        if (ec) {
            fs::remove(outPath, ec);
            fs::rename(tmpPath, outPath);
        }
    } catch (...) {
        std::error_code ec;
        fs::remove(tmpPath, ec);
        throw;
    }
    if (!opt.quiet) std::cout << input << " -> " << outPath << "\n";
}

bool parseArgs(int argc, char** argv, Options& opt, int& exitCode) {
    auto need = [&](int& i, const std::string& flag) -> std::string {
        if (i + 1 >= argc) throw Error("option " + flag + " requires an argument");
        return argv[++i];
    };
    bool endOfOptions = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (endOfOptions || a.empty() || a[0] != '-' || a == "-") {
            opt.inputs.push_back(a);
            continue;
        }
        if (a == "--") endOfOptions = true;
        else if (a == "-h" || a == "--help") { usage(std::cout); exitCode = 0; return false; }
        else if (a == "-V" || a == "--version") {
            std::cout << "xisfconv " << kVersion << "\ncodecs: zlib lz4 lz4hc" << (zstdAvailable() ? " zstd" : "")
                      << " (+byte shuffling)\n";
            exitCode = 0;
            return false;
        } else if (a == "-t" || a == "--to") {
            const std::string v = toLower(need(i, a));
            if (v == "fits" || v == "fit") opt.format = Format::Fits;
            else if (v == "tiff" || v == "tif") opt.format = Format::Tiff;
            else if (v == "png") opt.format = Format::Png;
            else if (v == "xisf") opt.format = Format::Xisf;
            else throw Error("unknown output format '" + v + "' (use fits, tiff, png or xisf)");
        } else if (a == "-o" || a == "--output") opt.output = need(i, a);
        else if (a == "-d" || a == "--outdir") opt.outdir = need(i, a);
        else if (a == "-b" || a == "--bits") {
            SampleFormat f;
            const std::string v = need(i, a);
            if (!parseShortSampleFormat(v, f)) throw Error("unknown sample format '" + v + "' (use u8, u16, u32, f32, f64)");
            opt.bits = f;
        } else if (a == "-i" || a == "--image") {
            uint64_t n;
            const std::string v = need(i, a);
            if (!parseUInt64(v, n)) throw Error("invalid image index '" + v + "'");
            opt.imageIndex = static_cast<size_t>(n);
        } else if (a == "-c" || a == "--compress") opt.compress = true;
        else if (a == "-s" || a == "--stretch") opt.stretch = Stretch::Auto;
        else if (startsWith(a, "--stretch=")) {
            const std::string v = toLower(a.substr(10));
            if (v == "auto") opt.stretch = Stretch::Auto;
            else if (v == "linked") opt.stretch = Stretch::Linked;
            else if (v == "unlinked") opt.stretch = Stretch::Unlinked;
            else if (v == "stf") opt.stretch = Stretch::Stored;
            else throw Error("unknown stretch mode '" + v + "' (use auto, linked, unlinked or stf)");
        }
        else if (a == "--bottom-up") { opt.bottomUp = true; opt.rowOrderGiven = true; }
        else if (a == "--top-down") { opt.bottomUp = false; opt.rowOrderGiven = true; }
        else if (a == "--codec") {
            const std::string v = toLower(need(i, a));
            if (v != "zlib" && v != "zstd") throw Error("unknown codec '" + v + "' (use zlib or zstd)");
            if (v == "zstd" && !zstdAvailable()) throw Error("this build has no Zstandard support; use --codec zlib");
            opt.codec = v;
            opt.compress = true;
        } else if (a == "--checksum") {
            std::string v = toLower(need(i, a));
            v.erase(std::remove(v.begin(), v.end(), '-'), v.end());
            if (v != "sha1" && v != "sha256" && v != "sha512") {
                throw Error("unknown checksum '" + v + "' (use sha1, sha256 or sha512)");
            }
            opt.checksum = v;
        } else if (a == "--bounds") {
            const std::string v = need(i, a);
            const auto parts = split(v, ':');
            double lo, hi;
            if (parts.size() != 2 || !parseDouble(parts[0], lo) || !parseDouble(parts[1], hi) || !(hi > lo)) {
                throw Error("--bounds expects lo:hi with hi > lo, e.g. 0:65535");
            }
            opt.bounds = std::make_pair(lo, hi);
        } else if (a == "--xisf-subblock-size") {  // undocumented: for testing subblock output
            uint64_t n;
            const std::string v = need(i, a);
            if (!parseUInt64(v, n) || n == 0) throw Error("invalid subblock size");
            opt.subblockSize = n;
        }
        else if (a == "--no-property-keywords") opt.propertyKeywords = false;
        else if (a == "--no-verify") opt.verify = false;
        else if (a == "--no-wcs") opt.wcs = false;
        else if (a == "--sip-order") {
            uint64_t n;
            const std::string v = need(i, a);
            if (!parseUInt64(v, n) || n == 1 || n > 7) throw Error("--sip-order must be 0 (off) or 2..7");
            opt.sipOrder = static_cast<int>(n);
        }
        else if (a == "-f" || a == "--force") opt.force = true;
        else if (a == "-I" || a == "--info") opt.info = true;
        else if (a == "--dump-header") opt.dumpHeader = true;
        else if (a == "-q" || a == "--quiet") opt.quiet = true;
        else throw Error("unknown option '" + a + "' (see --help)");
    }
    if (opt.inputs.empty()) {
        usage(std::cerr);
        exitCode = 2;
        return false;
    }
    if (!opt.output.empty() && opt.inputs.size() > 1) throw Error("-o/--output can only be used with a single input");
    if (!opt.output.empty() && !opt.format && !formatFromExtension(opt.output)) {
        throw Error("cannot infer output format from '" + opt.output + "'; add --to fits|tiff|png|xisf");
    }
    if (!opt.outdir.empty() && !fs::is_directory(opt.outdir)) throw Error("output directory does not exist: " + opt.outdir);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    try {
        int exitCode = 0;
        if (!parseArgs(argc, argv, opt, exitCode)) return exitCode;
    } catch (const std::exception& e) {
        std::cerr << "xisfconv: " << e.what() << "\n";
        return 2;
    }
    setQuiet(opt.quiet);
    int failures = 0;
    for (const auto& input : opt.inputs) {
        setWarningContext(input);
        try {
            if (looksLikeFits(input)) convertFitsFile(input, opt);
            else convertXisfFile(input, opt);
        } catch (const std::bad_alloc&) {
            std::cerr << "error: " << input << ": out of memory\n";
            ++failures;
        } catch (const std::exception& e) {
            std::cerr << "error: " << input << ": " << e.what() << "\n";
            ++failures;
        }
    }
    return failures ? 1 : 0;
}
