// xisfconv - convert PixInsight XISF images to FITS, ASDF, TIFF or PNG, and FITS or ASDF images to XISF.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include <algorithm>
#include <cstdio>
#include <filesystem>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "asdf.hpp"
#include "codecs.hpp"
#include "common.hpp"
#include "fitsread.hpp"
#include "pipeline.hpp"
#include "xisf.hpp"
#include "xisfrewrite.hpp"
#include "yaml.hpp"

namespace fs = std::filesystem;
using namespace xisfconv;

namespace {

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
    std::string codec;           // XISF and ASDF output: zlib or zstd
    bool codecNone = false;      // --codec none: store uncompressed (XISF -> XISF: decompress)
    std::string checksum;        // XISF output: sha1, sha256 or sha512
    bool checksumNone = false;   // --checksum none (XISF -> XISF: remove checksums)
    bool inPlace = false;        // XISF -> XISF: replace the input file
    bool verifyMode = false;     // --verify: check the files, convert nothing
    std::optional<std::pair<double, double>> bounds;  // FITS/ASDF input: range of floating point data
    uint64_t subblockSize = 1u << 30;
    bool propertyKeywords = true;
    bool verify = true;
    bool wcs = true;
    int sipOrder = 3;
    bool force = false;
    bool info = false;
    bool dumpHeader = false;
    bool treeJson = false;  // undocumented: print the parsed ASDF tree as JSON (for the tests)
    bool quiet = false;
    std::vector<std::string> inputs;
};

void usage(std::ostream& os) {
    os << "xisfconv " << kVersion << " - convert between PixInsight XISF, FITS and ASDF images; export TIFF and PNG\n\n"
          "Usage: xisfconv [options] <file>...\n"
          "       XISF inputs are converted to FITS (default), ASDF, TIFF or PNG, or rewritten as XISF\n"
          "       with another compression or checksum (-t xisf);\n"
          "       FITS inputs to XISF (default), ASDF, TIFF or PNG; tile-compressed FITS (.fits.fz)\n"
          "       is read like any FITS file, and -t fits writes it as a plain FITS file;\n"
          "       ASDF inputs to XISF (default), FITS, TIFF or PNG.\n"
          "       xisfconv --verify <file or directory>... checks files without converting them.\n\n"
          "Output:\n"
          "  -t, --to <fits|asdf|tiff|png|xisf>\n"
          "                              output format (default: fits for XISF input, xisf for FITS and ASDF\n"
          "                              input, or taken from -o's extension)\n"
          "  -o, --output <file>         output file name (single input only)\n"
          "  -d, --outdir <dir>          directory for output files (default: next to each input)\n"
          "  -f, --force                 overwrite existing output files\n"
          "      --in-place              XISF -> XISF: replace the input file. The new file is written next\n"
          "                              to it, read back and compared, and only then takes its place\n\n"
          "Conversion:\n"
          "  -b, --bits <fmt>            output sample format: u8, u16, u32, f32, f64 (default: as stored)\n"
          "  -i, --image <n>             convert only image n (0-based); default: all images\n"
          "                              (FITS, ASDF: extra images become further HDUs; TIFF: extra pages)\n"
          "  -c, --compress              TIFF: Deflate compression with predictor\n"
          "                              XISF: compress the pixel data (zstd, or zlib without libzstd);\n"
          "                              XISF -> XISF: all attached data blocks\n"
          "                              ASDF: compress the pixel data (zlib)\n"
          "  -s, --stretch[=mode]        apply a screen stretch for viewing linear data:\n"
          "                                auto     (default) the STF saved by PixInsight if any, else linked\n"
          "                                linked   auto-STF with shared statistics (keeps color balance)\n"
          "                                unlinked auto-STF per channel (neutralizes color casts)\n"
          "                                stf      only the STF saved in the file\n"
          "                              (TIFF: stretched float data becomes 16-bit unless --bits is given;\n"
          "                              FITS and ASDF input: for TIFF and PNG output, auto means linked)\n"
          "      --top-down              XISF input: keep XISF's top-down row order in FITS and ASDF output\n"
          "                              (ROWORDER='TOP-DOWN') instead of the FITS convention, bottom-up\n"
          "                              FITS and ASDF input: the rows are stored top-down\n"
          "      --bottom-up             FITS and ASDF input: the rows are stored bottom-up, whatever ROWORDER says\n"
          "      --no-property-keywords  from XISF: don't add missing keywords (EXPTIME, DATE-OBS, BAYERPAT...)\n"
          "                              derived from XISF properties\n"
          "      --no-wcs                from XISF: don't write WCS keywords from a PixInsight astrometric solution\n"
          "                              to XISF: don't write PixInsight solution properties from WCS keywords\n"
          "      --sip-order <n>         from XISF: SIP distortion order fitted to the solution (2-7, default 3;\n"
          "                              0 = off)\n"
          "      --bounds <lo:hi>        FITS and ASDF input: the range of floating point data, written as the\n"
          "                              XISF bounds and taken as black:white for TIFF and PNG (default: 0:1\n"
          "                              if the data fits, else 0:65535 if it fits, else minimum:maximum)\n"
          "      --no-verify             don't verify data block checksums (XISF -> XISF: nor read the output\n"
          "                              back, except with --in-place)\n\n"
          "XISF and ASDF output:\n"
          "      --codec <zlib|zstd|none>  compression codec (zlib and zstd imply --compress). XISF blocks are\n"
          "                              also byte shuffled. zstd in ASDF needs the asdf-compression package\n"
          "                              in Python. none: no compression; XISF -> XISF: decompress the blocks\n"
          "      --checksum <sha1|sha256|sha512|sha3-256|sha3-512|none>\n"
          "                              XISF: store a checksum of the pixel data block; XISF -> XISF: of every\n"
          "                              attached block (none removes them). ASDF blocks always carry MD5\n\n"

          "Inspection:\n"
          "      --verify                check the integrity of the files (and of the XISF, FITS and ASDF\n"
          "                              files in the directories) given: checksums are verified, compressed\n"
          "                              data is decompressed, sizes are compared. Exit status 1 on a failure\n"
          "  -I, --info                  print image geometry, keywords and properties; no conversion\n"
          "      --dump-header           print the raw XML header (XISF), all keywords (FITS) or the YAML tree\n"
          "                              (ASDF); no conversion\n\n"
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
    if (e == ".asdf") return Format::Asdf;
    return std::nullopt;
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
    // image.fits.fz is named after "image"
    const std::string outer = lowerExt(p.string());
    if (outer == ".fz" && formatFromExtension(name.string())) name = name.stem();
    switch (format) {
        case Format::Fits: name += ".fits"; break;
        case Format::Tiff: name += ".tif"; break;
        case Format::Png: name += ".png"; break;
        case Format::Xisf: name += ".xisf"; break;
        case Format::Asdf: name += ".asdf"; break;
    }
    return (dir / name).string();
}

std::string megabytes(uint64_t bytes) {
    char buf[48];
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mib < 10) std::snprintf(buf, sizeof buf, "%.2f MiB", mib);
    else std::snprintf(buf, sizeof buf, "%.1f MiB", mib);
    return buf;
}

// The library's warnings and notes, as this program has always printed them.
MessageHandler messagePrinter(const std::string& file, bool quiet) {
    return [file, quiet](MessageLevel level, const std::string& text) {
        if (quiet) return;
        if (level == MessageLevel::Warning) std::cerr << "warning: " << file << ": " << text << '\n';
        else std::cerr << "info: " << text << '\n';
    };
}

ConvertOptions conversionOptions(const Options& opt) {
    ConvertOptions c;
    c.stretch = opt.stretch;
    c.bits = opt.bits;
    c.imageIndex = opt.imageIndex;
    c.compress = opt.compress;
    c.codec = opt.codec;
    c.checksum = opt.checksum;
    c.subblockSize = opt.subblockSize;
    c.bottomUp = opt.bottomUp;
    c.rowOrderGiven = opt.rowOrderGiven;
    c.bounds = opt.bounds;
    c.propertyKeywords = opt.propertyKeywords;
    c.verify = opt.verify;
    c.wcs = opt.wcs;
    c.sipOrder = opt.sipOrder;
    c.force = opt.force;
    return c;
}

// XISF -> XISF: the same file with its data blocks stored another way.
void rewriteXisfInput(const std::string& input, const Options& opt) {
    if (opt.bits || opt.stretch != Stretch::None) {
        throw Error("XISF -> XISF changes how the data blocks are stored and leaves the pixels as they are; "
                    "--bits and --stretch do not apply (convert to FITS, TIFF or PNG for those)");
    }
    XisfRewriteOptions ropt;
    if (opt.codecNone) ropt.codec = "none";
    else if (opt.compress) ropt.codec = !opt.codec.empty() ? opt.codec : (zstdAvailable() ? "zstd" : "zlib");
    ropt.checksum = opt.checksumNone ? "none" : opt.checksum;
    ropt.imageIndex = opt.imageIndex;
    ropt.verifyInput = opt.verify;
    ropt.readBack = opt.verify;
    ropt.subblockSize = opt.subblockSize;

    const std::string output = opt.inPlace ? std::string() : outputPathFor(input, opt, Format::Xisf);
    const XisfFileRewrite done = rewriteXisfFile(input, output, opt.inPlace, opt.force, ropt);
    if (opt.quiet) return;
    if (done.unchanged) {
        std::cout << input << ": already stored as requested; left unchanged\n";
        return;
    }
    const XisfRewriteResult& r = done.result;
    std::string what;
    auto add = [&](size_t n, const std::string& text) {
        if (!n) return;
        if (!what.empty()) what += ", ";
        what += std::to_string(n) + " " + text;
    };
    add(r.compressed, std::string(r.compressed == 1 ? "block" : "blocks") + " compressed with " + ropt.codec);
    add(r.decompressed, std::string(r.decompressed == 1 ? "block" : "blocks") + " decompressed");
    add(r.kept, std::string(r.kept == 1 ? "block" : "blocks") + " kept as stored");
    add(r.checksums, std::string(r.checksums == 1 ? "checksum" : "checksums") + " computed");
    add(r.checksumsRemoved, std::string(r.checksumsRemoved == 1 ? "checksum" : "checksums") + " removed");
    if (what.empty()) what = "no attached data blocks";
    char percent[32];
    std::snprintf(percent, sizeof percent, "%.1f%%",
                  done.inputSize ? 100.0 * static_cast<double>(r.outputSize) / static_cast<double>(done.inputSize) : 100.0);
    std::cout << input << " -> " << done.output << ": " << megabytes(done.inputSize) << " -> " << megabytes(r.outputSize)
              << " (" << percent << "); " << what << (r.readBack ? "; read back and compared with the input" : "") << "\n";
}

void convertXisfInput(const std::string& input, const Options& opt) {
    if (opt.treeJson) throw Error("--asdf-tree-json needs an ASDF file");
    Format format = opt.inPlace ? Format::Xisf : Format::Fits;
    if (opt.format) format = *opt.format;
    else if (!opt.output.empty()) {
        if (auto f = formatFromExtension(opt.output)) format = *f;
    }
    if (format == Format::Xisf && !opt.dumpHeader && !opt.info) {
        rewriteXisfInput(input, opt);
        return;
    }
    if (opt.dumpHeader || opt.info || opt.inPlace) {
        XisfFile file(input);
        if (opt.dumpHeader) std::cout << file.headerXml() << "\n";
        else if (opt.info) printInfo(file);
        else throw Error("--in-place is for rewriting XISF files as XISF");
        return;
    }
    const std::string outPath = outputPathFor(input, opt, format);
    convertXisfFile(input, outPath, format, conversionOptions(opt));
    if (!opt.quiet) std::cout << input << " -> " << outPath << "\n";
}

void printKeywords(const std::vector<FitsKeyword>& keywords) {
    std::cout << "  Keywords (" << keywords.size() << "):\n";
    for (const auto& k : keywords) {
        std::cout << "    " << k.name;
        if (k.name.size() < 8) std::cout << std::string(8 - k.name.size(), ' ');
        if (!k.value.empty()) std::cout << "= " << k.value;
        if (!k.comment.empty()) std::cout << (k.value.empty() ? " " : " / ") << k.comment;
        std::cout << "\n";
    }
}

void printFitsInfo(const FitsFile& f) {
    std::cout << f.path << ": FITS, " << f.fileSize << " bytes, " << f.images.size() << " image HDU(s)\n";
    for (const auto& img : f.images) {
        std::cout << "\nHDU " << img.hduIndex;
        if (!img.name.empty()) std::cout << " \"" << img.name << "\"";
        std::cout << ": " << img.pixels.width << " x " << img.pixels.height << " x " << img.pixels.channels
                  << ", BITPIX " << img.bitpix;
        if (!img.tileCompression.empty()) std::cout << ", tile-compressed (" << img.tileCompression << ")";
        if (img.bscale != 1 || img.bzero != 0) std::cout << ", BZERO " << img.bzero << ", BSCALE " << img.bscale;
        std::cout << ", rows " << (img.hasRowOrder ? (img.topDown ? "top-down (ROWORDER)" : "bottom-up (ROWORDER)")
                                                    : "bottom-up (FITS default, no ROWORDER)")
                  << "\n";
        printKeywords(img.keywords);
    }
    for (const auto& s : f.skipped) std::cout << "\nSkipped " << s << "\n";
}

void printAsdfInfo(const FitsFile& f) {
    std::cout << f.path << ": " << f.formatNote << ", " << f.fileSize << " bytes, " << f.images.size() << " image(s)\n";
    for (const auto& img : f.images) {
        std::cout << "\nImage " << img.hduIndex << " at " << img.source;
        if (!img.generic && !img.name.empty()) std::cout << " \"" << img.name << "\"";
        std::cout << ": " << img.pixels.width << " x " << img.pixels.height << " x " << img.pixels.channels << ", "
                  << img.storage << "\n";
        std::cout << "  rows:        "
                  << (img.hasRowOrder ? (img.topDown ? "top-down (ROWORDER)" : "bottom-up (ROWORDER)")
                                      : img.generic ? "assumed bottom-up (plain array)" : "bottom-up (FITS default, no ROWORDER)")
                  << "\n";
        if (!img.generic) printKeywords(img.keywords);
    }
    for (const auto& s : f.skipped) std::cout << "\nSkipped " << s << "\n";
}

std::string jsonString(const std::string& text) {
    std::string out = "\"";
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
    return out + "\"";
}

// Prints a parsed YAML tree as JSON (mappings as {"m": [[key, value], ...]}); used by the tests.
void printYamlJson(const YamlNode& node, int depth) {
    if (depth > 1000) throw Error("tree too deep");
    if (node.isSequence()) {
        std::cout << "[";
        for (size_t i = 0; i < node.items.size(); ++i) {
            if (i) std::cout << ",";
            printYamlJson(*node.items[i], depth + 1);
        }
        std::cout << "]";
    } else if (node.isMapping()) {
        std::cout << "{\"t\":" << jsonString(node.tag) << ",\"m\":[";
        for (size_t i = 0; i < node.pairs.size(); ++i) {
            std::cout << (i ? ",[" : "[");
            printYamlJson(*node.pairs[i].first, depth + 1);
            std::cout << ",";
            printYamlJson(*node.pairs[i].second, depth + 1);
            std::cout << "]";
        }
        std::cout << "]}";
    } else {
        const YamlValue v = yamlResolve(node);
        switch (v.type) {
            case YamlValue::Type::Null: std::cout << "null"; break;
            case YamlValue::Type::Bool: std::cout << (v.boolean ? "true" : "false"); break;
            case YamlValue::Type::Int: std::cout << v.text; break;
            case YamlValue::Type::Float:
                if (v.number != v.number || v.number - v.number != 0) std::cout << "{\"f\":" << jsonString(v.text) << "}";
                else std::cout << "{\"f\":" << jsonString(formatDouble(v.number)) << "}";
                break;
            case YamlValue::Type::String: std::cout << jsonString(v.text); break;
        }
    }
}

// Converts a FITS or ASDF file. Both readers deliver the images in the same form.
void convertFitsOrAsdfInput(const std::string& input, InputFormat kind, const Options& opt) {
    const bool asdfInput = kind == InputFormat::Asdf;
    if (asdfInput && opt.treeJson) {
        printYamlJson(*parseYaml(readAsdfTree(input)), 0);
        std::cout << "\n";
        return;
    }
    if (opt.treeJson) throw Error("--asdf-tree-json needs an ASDF file");
    if (asdfInput && opt.dumpHeader) {
        const std::string tree = readAsdfTree(input);
#ifdef _WIN32
        // The tree is printed byte for byte; text mode would turn its CR LF into CR CR LF.
        std::cout.flush();
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::cout << tree;
        return;
    }
    if (opt.info || opt.dumpHeader) {
        if (asdfInput) printAsdfInfo(readAsdf(input, true, opt.verify));
        else printFitsInfo(readFits(input, true));
        return;
    }
    Format format = Format::Xisf;
    if (opt.format) format = *opt.format;
    else if (!opt.output.empty()) {
        if (auto f = formatFromExtension(opt.output)) format = *f;
    }
    if (opt.inPlace) throw Error("--in-place is for rewriting XISF files as XISF");
    const std::string outPath = outputPathFor(input, opt, format);
    convertFitsOrAsdfFile(input, kind, outPath, format, conversionOptions(opt));
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
            else if (v == "asdf") opt.format = Format::Asdf;
            else throw Error("unknown output format '" + v + "' (use fits, asdf, tiff, png or xisf)");
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
            if (v != "zlib" && v != "zstd" && v != "none") throw Error("unknown codec '" + v + "' (use zlib, zstd or none)");
            if (v == "zstd" && !zstdAvailable()) throw Error("this build has no Zstandard support; use --codec zlib");
            opt.codecNone = v == "none";
            opt.codec = opt.codecNone ? std::string() : v;
            if (!opt.codecNone) opt.compress = true;
        } else if (a == "--checksum") {
            std::string v = toLower(need(i, a));
            v.erase(std::remove(v.begin(), v.end(), '-'), v.end());
            if (v == "sha3256" || v == "sha3512") v.insert(4, "-");  // the XISF spelling: sha3-256
            if (v != "sha1" && v != "sha256" && v != "sha512" && v != "sha3-256" && v != "sha3-512" && v != "none") {
                throw Error("unknown checksum '" + v + "' (use sha1, sha256, sha512, sha3-256, sha3-512 or none)");
            }
            opt.checksumNone = v == "none";
            opt.checksum = opt.checksumNone ? std::string() : v;
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
        else if (a == "--in-place") opt.inPlace = true;
        else if (a == "--verify") opt.verifyMode = true;
        else if (a == "-I" || a == "--info") opt.info = true;
        else if (a == "--dump-header") opt.dumpHeader = true;
        else if (a == "--asdf-tree-json") opt.treeJson = true;
        else if (a == "-q" || a == "--quiet") opt.quiet = true;
        else throw Error("unknown option '" + a + "' (see --help)");
    }
    if (opt.inputs.empty()) {
        usage(std::cerr);
        exitCode = 2;
        return false;
    }
    if (opt.codecNone) opt.compress = false;
    if (opt.verifyMode) return true;
    if (opt.inPlace && (!opt.output.empty() || !opt.outdir.empty())) throw Error("--in-place cannot be combined with -o or -d");
    if (!opt.output.empty() && opt.inputs.size() > 1) throw Error("-o/--output can only be used with a single input");
    if (!opt.output.empty() && !opt.format && !formatFromExtension(opt.output)) {
        throw Error("cannot infer output format from '" + opt.output + "'; add --to fits|asdf|tiff|png|xisf");
    }
    if (!opt.outdir.empty() && !fs::is_directory(opt.outdir)) throw Error("output directory does not exist: " + opt.outdir);
    return true;
}

// Collects the XISF, FITS and ASDF files in and below a directory. Directories that cannot be
// read are reported in `errors`.
void findImageFiles(const fs::path& directory, std::vector<std::string>& found, std::vector<std::string>& errors, int depth = 0) {
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    if (ec || depth > 64) {
        errors.push_back(directory.string() + ": " + (ec ? ec.message() : std::string("directories nested too deeply")));
        return;
    }
    std::vector<fs::path> directories;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            errors.push_back(directory.string() + ": " + ec.message());
            break;
        }
        try {
            std::error_code entryError;
            if (it->is_directory(entryError) && !it->is_symlink(entryError)) {
                directories.push_back(it->path());
            } else if (it->is_regular_file(entryError)) {
                std::string name = it->path().string();
                auto format = formatFromExtension(name);
                // image.fits.fz: a FITS file with tile-compressed images
                if (!format && lowerExt(name) == ".fz" && formatFromExtension(it->path().stem().string()) == Format::Fits) format = Format::Fits;
                if (format && (*format == Format::Xisf || *format == Format::Fits || *format == Format::Asdf)) found.push_back(name);
            }
        } catch (const std::exception& e) {  // e.g. a name that has no narrow-character form
            errors.push_back(directory.string() + ": " + e.what());
        }
    }
    std::sort(directories.begin(), directories.end());
    for (const auto& sub : directories) findImageFiles(sub, found, errors, depth + 1);
}

// --verify: checks every file (and the image files in every directory) given.
int verifyFiles(const Options& opt) {
    std::vector<std::string> files, errors;
    for (const auto& input : opt.inputs) {
        std::error_code ec;
        if (!fs::is_directory(input, ec)) {
            files.push_back(input);
            continue;
        }
        std::vector<std::string> found;
        findImageFiles(input, found, errors);
        std::sort(found.begin(), found.end());
        if (found.empty()) std::cerr << "warning: " << input << ": no XISF, FITS or ASDF files found\n";
        files.insert(files.end(), found.begin(), found.end());
    }

    auto plural = [](size_t n, const char* word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); };
    size_t ok = 0, partly = 0, failed = 0;
    for (const auto& f : files) {
        const MessageScope messages(messagePrinter(f, opt.quiet));
        VerifyReport r;
        const char* kind = "XISF";
        try {
            if (looksLikeFits(f)) {
                kind = "FITS";
                r = verifyFits(f);
            } else if (looksLikeAsdf(f)) {
                kind = "ASDF";
                r = verifyAsdf(f);
            } else {
                r = verifyXisf(f);
            }
        } catch (const std::bad_alloc&) {
            r.problems.push_back("out of memory");
        } catch (const std::exception& e) {
            r.problems.push_back(e.what());
        }
        if (!r.problems.empty()) {
            ++failed;
            std::cout << f << ": FAILED\n";
            for (std::string p : r.problems) {
                // Hints meant for conversions do not apply here.
                for (const char* hint : {" (use --no-verify to convert anyway)", " (the file is damaged; --no-verify skips this check)"}) {
                    const size_t at = p.find(hint);
                    if (at != std::string::npos) p.erase(at, std::strlen(hint));
                }
                std::cout << "  " << p << "\n";
            }
            continue;
        }
        const bool complete = r.notChecked.empty();
        ++(complete ? ok : partly);
        if (opt.quiet && complete) continue;
        std::cout << f << ": " << (complete ? "OK" : "NOT FULLY CHECKED") << " (" << kind << ", " << r.summary << "; ";
        if (r.verified) std::cout << plural(r.verified, "checksum") << " verified";
        else std::cout << "no checksums " << (complete ? "in the file" : "verified");
        if (r.verified && r.unchecked) std::cout << ", " << r.unchecked << " without checksum";
        std::cout << ")\n";
        for (const auto& n : r.notChecked) std::cout << "  not checked: " << n << "\n";
    }
    for (const auto& e : errors) std::cout << e << ": FAILED (cannot be read)\n";
    failed += errors.size();
    if ((files.size() + errors.size() > 1) && !opt.quiet) {
        std::cout << "\n" << plural(ok, "file") << " OK, ";
        if (partly) std::cout << partly << " not fully checked, ";
        std::cout << failed << " failed\n";
    }
    return failed ? 1 : 0;
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
    if (opt.verifyMode) return verifyFiles(opt);
    int failures = 0;
    for (const auto& input : opt.inputs) {
        const MessageScope messages(messagePrinter(input, opt.quiet));
        try {
            const InputFormat kind = detectInputFormat(input);
            if (kind == InputFormat::Xisf) convertXisfInput(input, opt);
            else convertFitsOrAsdfInput(input, kind, opt);
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