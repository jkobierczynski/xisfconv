// xisfconv - convert PixInsight XISF images to FITS, ASDF, TIFF or PNG, and FITS or ASDF images to XISF.
// The command line tool. It uses the library through its C API (xisfconv.h) and nothing else.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <locale>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "xisfconv.h"

namespace fs = std::filesystem;

namespace {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------- small helpers

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

bool startsWith(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

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

bool parseShortSampleFormat(const std::string& s, xisfconv_sample_format& out) {
    const std::string l = toLower(s);
    if (l == "u8" || l == "8") out = XISFCONV_SAMPLE_UINT8;
    else if (l == "u16" || l == "16") out = XISFCONV_SAMPLE_UINT16;
    else if (l == "u32") out = XISFCONV_SAMPLE_UINT32;
    else if (l == "u64") out = XISFCONV_SAMPLE_UINT64;
    else if (l == "f32" || l == "32f" || l == "float") out = XISFCONV_SAMPLE_FLOAT32;
    else if (l == "f64" || l == "64f" || l == "double") out = XISFCONV_SAMPLE_FLOAT64;
    else return false;
    return true;
}

// File names are UTF-8 in this program, as they are in the library.
fs::path toPath(const std::string& utf8) {
#if defined(__cpp_lib_char8_t)
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return fs::u8path(utf8);
#endif
}

std::string fromPath(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool zstdAvailable() { return xisfconv_codec_available(XISFCONV_CODEC_ZSTD, 1) != 0; }

// ---------------------------------------------------------------- options

struct Options {
    xisfconv_stretch stretch = XISFCONV_STRETCH_NONE;
    std::optional<xisfconv_format> format;
    std::string output;
    std::string outdir;
    std::optional<xisfconv_sample_format> bits;
    std::optional<size_t> imageIndex;
    bool compress = false;
    bool bottomUp = true;  // FITS convention: first stored row is the bottom of the image
    bool rowOrderGiven = false;  // --top-down / --bottom-up given explicitly (overrides ROWORDER on FITS input)
    std::string codec;           // XISF output: zlib, zstd, lz4 or lz4hc; ASDF: zlib or zstd; FITS: zlib (GZIP tiles)
    bool codecNone = false;      // --codec none: store uncompressed (XISF -> XISF: decompress)
    std::string checksum;        // XISF output: sha1, sha256, sha512, sha3-256 or sha3-512
    bool checksumNone = false;   // --checksum none (XISF -> XISF: remove checksums)
    bool inPlace = false;        // XISF -> XISF: replace the input file
    bool distributed = false;    // -t xish: XISF output is a header file (.xish) and a data blocks file (.xisb)
    bool monolithic = false;     // -t xisf, said in so many words: one file
    xisfconv_external_files externalFiles = XISFCONV_EXTERNAL_HEADER_DIRECTORY;   // --external-files
    bool verifyMode = false;     // --verify: check the files, convert nothing
    std::optional<std::pair<double, double>> bounds;  // FITS/ASDF input: range of floating point data
    uint64_t subblockSize = 1u << 30;
    bool propertyKeywords = true;
    bool properties = true;      // take XISF properties along to FITS and ASDF, and use those such a file carries
    uint64_t bin = 1;            // TIFF and PNG output: n x n pixels become one
    uint64_t fitWidth = 0, fitHeight = 0;   // ... the picture fits that many pixels
    double scale = 0;            // ... the picture is that fraction of the image
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

const char* const kVersion = xisfconv_version();

void usage(std::ostream& os) {
    os << "xisfconv " << kVersion << " - convert between PixInsight XISF, FITS and ASDF images; export TIFF and PNG\n\n"
          "Usage: xisfconv [options] <file>...\n"
          "       XISF inputs are converted to FITS (default), ASDF, TIFF or PNG, or rewritten as XISF\n"
          "       with another compression or checksum (-t xisf). An XISF input is a monolithic file\n"
          "       (.xisf) or the header file of a distributed unit (.xish), whose data is in the\n"
          "       files that header names (.xisb); -t xish writes such a unit, -t xisf one file;\n"
          "       FITS inputs to XISF (default), ASDF, TIFF or PNG; tile-compressed FITS (.fits.fz)\n"
          "       is read like any FITS file, -t fits writes it as a plain FITS file, and -t fits -c\n"
          "       writes a FITS file tile-compressed;\n"
          "       ASDF inputs to XISF (default), FITS, TIFF or PNG.\n"
          "       xisfconv --verify <file or directory>... checks files without converting them.\n\n"
          "Output:\n"
          "  -t, --to <fits|asdf|tiff|png|xisf|xish>\n"
          "                              output format (default: fits for XISF input, xisf for FITS and ASDF\n"
          "                              input, or taken from -o's extension). xish: XISF as a distributed\n"
          "                              unit, the header in <name>.xish and the data blocks in <name>.xisb\n"
          "                              beside it (PixInsight itself opens monolithic .xisf files only)\n"
          "  -o, --output <file>         output file name (single input only)\n"
          "  -d, --outdir <dir>          directory for output files (default: next to each input)\n"
          "  -f, --force                 overwrite existing output files\n"
          "      --in-place              XISF -> XISF: replace the input file. The new file is written next\n"
          "                              to it, read back and compared, and only then takes its place\n\n"
          "Conversion:\n"
          "  -b, --bits <fmt>            output sample format: u8, u16, u32, f32, f64 (default: as stored)\n"
          "  -i, --image <n>             convert only image n (0-based); default: all images\n"
          "                              (FITS, ASDF: extra images become further HDUs; TIFF: extra pages)\n"
          "  -c, --compress              FITS: tile compression, lossless (image.fits.fz, the format of fpack):\n"
          "                              RICE_1 for integers, GZIP_2 for floating point. An output name\n"
          "                              that ends in .fz is written this way with or without -c\n"
          "                              TIFF: Deflate compression with predictor\n"
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
          "      --bin <n>               TIFF and PNG output: a smaller picture, n x n pixels averaged into one\n"
          "      --resize <size>         TIFF and PNG output: a smaller picture, the pixels it covers averaged:\n"
          "                                256        its longest side is 256 pixels\n"
          "                                1024x768   it fits a box of that size, its proportions kept\n"
          "                                50%        half the width and half the height\n"
          "                              (never larger than the image; made before a stretch is applied)\n"
          "      --top-down              XISF input: keep XISF's top-down row order in FITS and ASDF output\n"
          "                              (ROWORDER='TOP-DOWN') instead of the FITS convention, bottom-up\n"
          "                              FITS and ASDF input: the rows are stored top-down\n"
          "      --bottom-up             FITS and ASDF input: the rows are stored bottom-up, whatever ROWORDER says\n"
          "      --no-property-keywords  from XISF: don't add missing keywords (EXPTIME, DATE-OBS, BAYERPAT...)\n"
          "                              derived from XISF properties\n"
          "      --no-properties         from XISF: don't take the XISF properties along to FITS and ASDF\n"
          "                              from FITS and ASDF: leave the XISF properties a file carries where they are\n"
          "      --no-wcs                from XISF: don't write WCS keywords from a PixInsight astrometric solution\n"
          "                              to XISF: don't write PixInsight solution properties from WCS keywords\n"
          "      --sip-order <n>         from XISF: SIP distortion order fitted to the solution (2-7, default 3;\n"
          "                              0 = off)\n"
          "      --bounds <lo:hi>        FITS and ASDF input: the range of floating point data, written as the\n"
          "                              XISF bounds and taken as black:white for TIFF and PNG (default: 0:1\n"
          "                              if the data fits, else 0:65535 if it fits, else minimum:maximum)\n"
          "      --no-verify             don't verify data block checksums (XISF -> XISF: nor read the output\n"
          "                              back, except with --in-place)\n"
          "      --external-files <header-dir|anywhere|none>\n"
          "                              XISF input: which files the header may name for its data.\n"
          "                                header-dir (default) files in the directory of the header and below\n"
          "                                anywhere   also absolute paths, file: URLs and where links lead\n"
          "                                none       no file but the header itself\n"
          "                              (a header is data from somewhere: it could name any file of this\n"
          "                              machine as the pixels of an image. Nothing is fetched from a network)\n\n"
          "XISF, ASDF and FITS output:\n"
          "      --codec <zlib|zstd|lz4|lz4hc|none>\n"
          "                              compression codec (a codec implies --compress). XISF blocks are\n"
          "                              also byte shuffled; lz4 and lz4hc are for XISF only. zstd in ASDF\n"
          "                              needs the asdf-compression package in Python. none: no compression;\n"
          "                              XISF -> XISF: decompress the blocks\n"
          "                              FITS: zlib compresses the tiles of every sample type with gzip\n"
          "                              (GZIP_2; GZIP_1 for 8-bit data); there is no zstd or lz4 for FITS\n"
          "      --checksum <sha1|sha256|sha512|sha3-256|sha3-512|none>\n"
          "                              XISF: store a checksum of the pixel data block; XISF -> XISF: of every\n"
          "                              attached block (none removes them). ASDF blocks always carry MD5.\n"
          "                              PixInsight opens files with sha1, sha256 and sha512 checksums only\n\n"

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

std::string lowerExt(const std::string& path) { return toLower(fromPath(toPath(path).extension())); }

std::optional<xisfconv_format> formatFromExtension(const std::string& path) {
    const std::string e = lowerExt(path);
    // image.fits.fz: FITS, tile-compressed
    if (e == ".fz") {
        if (formatFromExtension(fromPath(toPath(path).stem())) == XISFCONV_FORMAT_FITS) return XISFCONV_FORMAT_FITS;
        return std::nullopt;
    }
    if (e == ".fits" || e == ".fit" || e == ".fts") return XISFCONV_FORMAT_FITS;
    if (e == ".tif" || e == ".tiff") return XISFCONV_FORMAT_TIFF;
    if (e == ".png") return XISFCONV_FORMAT_PNG;
    if (e == ".xisf" || e == ".xish") return XISFCONV_FORMAT_XISF;   // (.xish: the header file of a distributed unit)
    if (e == ".asdf") return XISFCONV_FORMAT_ASDF;
    return std::nullopt;
}

std::string outputPathFor(const std::string& input, const Options& opt, xisfconv_format format) {
    if (!opt.output.empty()) return opt.output;
    const fs::path p = toPath(input);
    const fs::path dir = opt.outdir.empty() ? p.parent_path() : toPath(opt.outdir);
    fs::path name = p.stem();
    // image.fits.fz is named after "image"
    if (lowerExt(input) == ".fz" && formatFromExtension(fromPath(name)) && lowerExt(fromPath(name)) != ".fz") name = name.stem();
    switch (format) {
        case XISFCONV_FORMAT_FITS: name += opt.compress ? ".fits.fz" : ".fits"; break;   // tile-compressed, as fpack names it
        case XISFCONV_FORMAT_TIFF: name += ".tif"; break;
        case XISFCONV_FORMAT_PNG: name += ".png"; break;
        case XISFCONV_FORMAT_ASDF: name += ".asdf"; break;
        default: name += opt.distributed ? ".xish" : ".xisf"; break;
    }
    return fromPath(dir / name);
}

std::string megabytes(uint64_t bytes) {
    char buf[48];
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mib < 10) std::snprintf(buf, sizeof buf, "%.2f MiB", mib);
    else std::snprintf(buf, sizeof buf, "%.1f MiB", mib);
    return buf;
}

// ---------------------------------------------------------------- the library

// One context for the whole run. Its message handler prints the library's warnings and notes
// the way this program always has.
struct Library {
    xisfconv_context* ctx = nullptr;
    bool quiet = false;

    Library() : ctx(xisfconv_context_new()) {
        if (!ctx) throw std::bad_alloc();
        xisfconv_context_set_message_handler(ctx, &Library::print, this);
    }
    ~Library() { xisfconv_context_free(ctx); }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    static void print(void* user, xisfconv_message_level level, const char* path, const char* message) {
        const Library* self = static_cast<const Library*>(user);
        if (self->quiet) return;
        if (level == XISFCONV_MESSAGE_WARNING) {
            std::cerr << "warning: ";
            if (path && *path) std::cerr << path << ": ";
            std::cerr << message << '\n';
        } else {
            std::cerr << "info: " << message << '\n';
        }
    }

    // Turns a failed call into the exception that main() reports.
    void check(xisfconv_status status) const {
        if (status == XISFCONV_OK) return;
        if (status == XISFCONV_ERR_MEMORY) throw std::bad_alloc();
        throw Error(xisfconv_error_message(ctx));
    }
};

// A file opened through the library, closed when it goes out of scope.
struct OpenFile {
    xisfconv_file* file = nullptr;
    OpenFile(const Library& lib, const std::string& path) { lib.check(xisfconv_open(lib.ctx, path.c_str(), &file)); }
    ~OpenFile() { xisfconv_close(file); }
    OpenFile(const OpenFile&) = delete;
    OpenFile& operator=(const OpenFile&) = delete;
};

xisfconv_checksum checksumOption(const std::string& name) {
    if (name == "sha1") return XISFCONV_CHECKSUM_SHA1;
    if (name == "sha256") return XISFCONV_CHECKSUM_SHA256;
    if (name == "sha512") return XISFCONV_CHECKSUM_SHA512;
    if (name == "sha3-256") return XISFCONV_CHECKSUM_SHA3_256;
    if (name == "sha3-512") return XISFCONV_CHECKSUM_SHA3_512;
    return XISFCONV_CHECKSUM_NONE;
}

// --codec as the library names it; `unnamed` for --compress without a codec.
xisfconv_codec codecOption(const std::string& name, xisfconv_codec unnamed) {
    return name == "zlib" ? XISFCONV_CODEC_ZLIB : name == "zstd" ? XISFCONV_CODEC_ZSTD : name == "lz4" ? XISFCONV_CODEC_LZ4
           : name == "lz4hc" ? XISFCONV_CODEC_LZ4HC : unnamed;
}

xisfconv_convert_options conversionOptions(const Options& opt, xisfconv_format format) {
    xisfconv_convert_options c;
    xisfconv_convert_options_init(&c, sizeof c);
    c.output_format = format;
    c.stretch = opt.stretch;
    c.sample_format = opt.bits ? *opt.bits : XISFCONV_SAMPLE_AS_STORED;
    c.image = opt.imageIndex ? *opt.imageIndex : XISFCONV_ALL_IMAGES;
    c.codec = !opt.compress ? XISFCONV_CODEC_NONE : codecOption(opt.codec, XISFCONV_CODEC_DEFAULT);
    c.checksum = checksumOption(opt.checksum);
    c.subblock_size = opt.subblockSize;
    c.row_order = !opt.rowOrderGiven ? XISFCONV_ROWS_DEFAULT : opt.bottomUp ? XISFCONV_ROWS_BOTTOM_UP : XISFCONV_ROWS_TOP_DOWN;
    if (opt.bounds) {
        c.use_bounds = 1;
        c.lower_bound = opt.bounds->first;
        c.upper_bound = opt.bounds->second;
    }
    c.property_keywords = opt.propertyKeywords;
    c.properties = opt.properties;
    c.bin = static_cast<int32_t>(opt.bin);
    c.fit_width = opt.fitWidth;
    c.fit_height = opt.fitHeight;
    c.scale = opt.scale;
    c.verify_checksums = opt.verify;
    c.wcs = opt.wcs;
    c.sip_order = opt.sipOrder;
    c.overwrite = opt.force;
    return c;
}

// ---------------------------------------------------------------- --info

void printCards(const xisfconv_keywords* kw) {
    for (size_t k = 0; k < xisfconv_keywords_count(kw); ++k) {
        const char *name = "", *value = "", *comment = "";
        xisfconv_keywords_get(kw, k, &name, &value, &comment);
        const size_t length = std::strlen(name);
        std::cout << "    " << name;
        if (length < 8) std::cout << std::string(8 - length, ' ');
        if (*value) std::cout << "= " << value;
        if (*comment) std::cout << (*value ? " / " : " ") << comment;
        std::cout << "\n";
    }
}

const xisfconv_keywords* cardsOf(const Library& lib, const xisfconv_file* f, size_t image) {
    const xisfconv_keywords* kw = nullptr;
    lib.check(xisfconv_image_keywords(f, image, &kw));
    return kw;
}

xisfconv_image_info infoOf(const Library& lib, const xisfconv_file* f, size_t image) {
    xisfconv_image_info info;
    xisfconv_image_info_init(&info, sizeof info);
    lib.check(xisfconv_image_info_get(f, image, &info));
    return info;
}

bool isFloat(xisfconv_sample_format f) { return f == XISFCONV_SAMPLE_FLOAT32 || f == XISFCONV_SAMPLE_FLOAT64; }

void printProperties(const Library& lib, xisfconv_file* f, size_t image, const char* indent) {
    const size_t properties = xisfconv_property_count(f, image);
    for (size_t p = 0; p < properties; ++p) {
        const char *pid = "", *type = "", *value = "";
        int32_t block = 0;
        lib.check(xisfconv_property_get(f, image, p, &pid, &type, &value, nullptr, &block));
        std::cout << indent << pid << " (" << type << ")";
        if (block) std::cout << " [data block]";
        else {
            std::string v = value;
            if (image != XISFCONV_FILE_PROPERTIES) {
                if (v.size() > 100) v = v.substr(0, 100) + "...";
                for (auto& c : v)
                    if (c == '\n' || c == '\r') c = ' ';
            }
            std::cout << " = " << v;
        }
        std::cout << "\n";
    }
}

void printXisfInfo(const Library& lib, const std::string& path, xisfconv_file* f) {
    const char* header = "";
    size_t headerSize = 0;
    lib.check(xisfconv_header_text(f, &header, &headerSize));
    const size_t images = xisfconv_image_count(f);
    const size_t others = xisfconv_external_count(f);
    size_t there = 0;   // of the files the header names: those that are there and are read
    for (size_t k = 0; k < others; ++k) there += xisfconv_external_status(f, k) == XISFCONV_OK;
    if (std::string(xisfconv_file_detail(f, "unit")) == "distributed") {
        // a header file: the unit is that file and those its header names
        std::cout << path << ": XISF " << xisfconv_file_detail(f, "version") << ", distributed unit, " << xisfconv_unit_size(f)
                  << " bytes in " << there + 1 << (there ? " files" : " file") << ", header " << headerSize << " bytes, "
                  << images << " image(s)\n";
    } else {
        std::cout << path << ": XISF " << xisfconv_file_detail(f, "version") << ", " << xisfconv_file_size(f) << " bytes, header "
                  << headerSize << " bytes, " << images << " image(s)\n";
    }
    for (size_t k = 0; k < others; ++k) {
        const xisfconv_status status = xisfconv_external_status(f, k);
        std::cout << "  data in:     " << xisfconv_external_file(f, k)
                  << (status == XISFCONV_OK ? ""
                      : status == XISFCONV_ERR_NOT_ALLOWED ? "  (not read: the header is not followed there, see --external-files)"
                      : status == XISFCONV_ERR_UNSUPPORTED ? "  (not read: nothing is fetched from a network)"
                      : "  (not there, or not a regular file)")
                  << "\n";
    }
    for (size_t i = 0; i < images; ++i) {
        const xisfconv_image_info img = infoOf(lib, f, i);
        auto detail = [&](const char* name) { return std::string(xisfconv_image_detail(f, i, name)); };
        const std::string id = xisfconv_image_name(f, i);
        std::cout << "\nImage " << i;
        if (!id.empty()) std::cout << " \"" << id << "\"";
        std::cout << ": " << img.width << " x " << img.height << " x " << img.channels << ", "
                  << detail("sampleFormat") << ", " << detail("colorSpace") << ", " << detail("pixelStorage")
                  << ", " << detail("byteOrder") << "-endian\n";
        if (isFloat(img.sample_format)) std::cout << "  bounds:      " << img.lower_bound << " : " << img.upper_bound << "\n";
        std::cout << "  location:    " << detail("location") << "\n";
        if (!detail("compression").empty()) std::cout << "  compression: " << detail("compression") << "\n";
        if (!detail("subblocks").empty()) std::cout << "  subblocks:   " << detail("subblocks") << "\n";
        if (!detail("checksum").empty()) std::cout << "  checksum:    " << detail("checksum") << "\n";
        if (!detail("imageType").empty()) std::cout << "  imageType:   " << detail("imageType") << "\n";
        if (!detail("orientation").empty()) std::cout << "  orientation: " << detail("orientation") << "\n";
        if (img.has_cfa)
            std::cout << "  CFA:         " << detail("cfaPattern") << " (" << img.cfa_width << "x" << img.cfa_height << ")"
                      << (detail("cfaName").empty() ? "" : " " + detail("cfaName")) << "\n";
        if (img.resolution_unit)
            std::cout << "  resolution:  " << img.resolution_x << " x " << img.resolution_y << " per "
                      << detail("resolutionUnit") << "\n";
        if (img.has_icc_profile) std::cout << "  ICC profile: yes\n";
        if (img.has_display_function) {
            std::cout << "  STF:         " << (img.has_stored_stretch ? "" : "identity (no stretch)");
            if (img.has_stored_stretch) {
                xisfconv_stretch_params stf[3];
                size_t count = 0;
                lib.check(xisfconv_stored_stretch(f, i, stf, 3, &count));
                for (size_t k = 0; k < count; ++k)
                    std::cout << (k ? "; " : "") << "s=" << stf[k].shadows << " m=" << stf[k].midtones << " h=" << stf[k].highlights;
            }
            std::cout << "\n";
        }
        if (!img.convertible) std::cout << "  NOT CONVERTIBLE: " << xisfconv_image_unsupported_reason(f, i) << "\n";
        const xisfconv_keywords* kw = cardsOf(lib, f, i);
        std::cout << "  FITS keywords (" << xisfconv_keywords_count(kw) << "):\n";
        printCards(kw);
        std::cout << "  Properties (" << xisfconv_property_count(f, i) << "):\n";
        printProperties(lib, f, i, "    ");
    }
    const size_t metadata = xisfconv_property_count(f, XISFCONV_FILE_PROPERTIES);
    if (metadata) {
        std::cout << "\nFile metadata (" << metadata << "):\n";
        printProperties(lib, f, XISFCONV_FILE_PROPERTIES, "  ");
    }
}

// FITS and ASDF: the XISF properties a file carries from the XISF file it was converted from.
void printCarriedProperties(const Library& lib, xisfconv_file* f, size_t image) {
    const size_t count = xisfconv_property_count(f, image);
    if (!count) return;
    if (image == XISFCONV_FILE_PROPERTIES) std::cout << "\nXISF file metadata (" << count << "):\n";
    else std::cout << "  XISF properties (" << count << "):\n";
    printProperties(lib, f, image, image == XISFCONV_FILE_PROPERTIES ? "  " : "    ");
}

void printKeywords(const xisfconv_keywords* kw) {
    std::cout << "  Keywords (" << xisfconv_keywords_count(kw) << "):\n";
    printCards(kw);
}

const char* rowsText(const xisfconv_image_info& img, const char* undeclared) {
    if (!img.row_order_declared) return undeclared;
    return img.row_order == XISFCONV_ROWS_TOP_DOWN ? "top-down (ROWORDER)" : "bottom-up (ROWORDER)";
}

void printFitsInfo(const Library& lib, const std::string& path, xisfconv_file* f) {
    const size_t images = xisfconv_image_count(f);
    std::cout << path << ": FITS, " << xisfconv_file_size(f) << " bytes, " << images << " image HDU(s)\n";
    for (size_t i = 0; i < images; ++i) {
        const xisfconv_image_info img = infoOf(lib, f, i);
        const std::string name = xisfconv_image_name(f, i);
        const std::string tiles = xisfconv_image_detail(f, i, "tileCompression");
        std::cout << "\nHDU " << img.source_index;
        if (!name.empty()) std::cout << " \"" << name << "\"";
        std::cout << ": " << img.width << " x " << img.height << " x " << img.channels << ", BITPIX " << img.bitpix;
        if (!tiles.empty()) std::cout << ", tile-compressed (" << tiles << ")";
        if (img.bscale != 1 || img.bzero != 0) std::cout << ", BZERO " << img.bzero << ", BSCALE " << img.bscale;
        std::cout << ", rows " << rowsText(img, "bottom-up (FITS default, no ROWORDER)") << "\n";
        printKeywords(cardsOf(lib, f, i));
        printCarriedProperties(lib, f, i);
    }
    printCarriedProperties(lib, f, XISFCONV_FILE_PROPERTIES);
    for (size_t s = 0; s < xisfconv_skipped_count(f); ++s) std::cout << "\nSkipped " << xisfconv_skipped_text(f, s) << "\n";
}

void printAsdfInfo(const Library& lib, const std::string& path, xisfconv_file* f) {
    const size_t images = xisfconv_image_count(f);
    std::cout << path << ": " << xisfconv_file_detail(f, "format") << ", " << xisfconv_file_size(f) << " bytes, " << images
              << " image(s)\n";
    for (size_t i = 0; i < images; ++i) {
        const xisfconv_image_info img = infoOf(lib, f, i);
        const std::string name = xisfconv_image_name(f, i);
        std::cout << "\nImage " << img.source_index << " at " << xisfconv_image_detail(f, i, "source");
        if (!img.plain_array && !name.empty()) std::cout << " \"" << name << "\"";
        std::cout << ": " << img.width << " x " << img.height << " x " << img.channels << ", "
                  << xisfconv_image_detail(f, i, "storage") << "\n";
        std::cout << "  rows:        "
                  << rowsText(img, img.plain_array ? "assumed bottom-up (plain array)" : "bottom-up (FITS default, no ROWORDER)")
                  << "\n";
        if (!img.plain_array) printKeywords(cardsOf(lib, f, i));
        printCarriedProperties(lib, f, i);
    }
    printCarriedProperties(lib, f, XISFCONV_FILE_PROPERTIES);
    for (size_t s = 0; s < xisfconv_skipped_count(f); ++s) std::cout << "\nSkipped " << xisfconv_skipped_text(f, s) << "\n";
}

// ---------------------------------------------------------------- converting

// XISF -> XISF: the same file with its data blocks stored another way.
void rewriteXisfInput(const Library& lib, const std::string& input, const Options& opt) {
    if (opt.bits || opt.stretch != XISFCONV_STRETCH_NONE) {
        throw Error("XISF -> XISF changes how the data blocks are stored and leaves the pixels as they are; "
                    "--bits and --stretch do not apply (convert to FITS, TIFF or PNG for those)");
    }
    if (opt.bin > 1 || opt.fitWidth || opt.fitHeight || opt.scale > 0) {
        throw Error("--bin and --resize make a smaller picture: they are for TIFF and PNG output");
    }
    if (opt.inPlace && opt.distributed && lowerExt(input) != ".xish") {
        throw Error("--in-place keeps the kind of unit, which goes with the name of the file: under this name it stays "
                    "one monolithic file (-t xish without --in-place writes a header file and its data blocks file)");
    }

    xisfconv_rewrite_options r;
    xisfconv_rewrite_options_init(&r, sizeof r);
    std::string codec;  // as it is named in the report
    if (opt.codecNone) {
        r.codec = XISFCONV_CODEC_NONE;
        codec = "none";
    } else if (opt.compress) {
        codec = !opt.codec.empty() ? opt.codec : (zstdAvailable() ? "zstd" : "zlib");
        r.codec = codecOption(codec, XISFCONV_CODEC_ZLIB);
    }
    r.checksum = opt.checksumNone ? XISFCONV_CHECKSUM_NONE : opt.checksum.empty() ? XISFCONV_CHECKSUM_KEEP : checksumOption(opt.checksum);
    r.image = opt.imageIndex ? *opt.imageIndex : XISFCONV_ALL_IMAGES;
    r.verify_input = opt.verify;
    r.read_back = opt.verify;
    r.subblock_size = opt.subblockSize;
    r.overwrite = opt.force;

    xisfconv_rewrite_result done;
    xisfconv_rewrite_result_init(&done, sizeof done);
    std::string output;
    if (opt.inPlace) {
        // The file itself is replaced, not a link that leads to it: that is the name reported.
        std::error_code pathError;
        const fs::path real = fs::canonical(toPath(input), pathError);
        output = pathError ? input : fromPath(real);
        lib.check(xisfconv_rewrite_in_place(lib.ctx, input.c_str(), &r, &done));
    } else {
        output = outputPathFor(input, opt, XISFCONV_FORMAT_XISF);
        lib.check(xisfconv_rewrite(lib.ctx, input.c_str(), output.c_str(), &r, &done));
    }
    if (opt.quiet) return;
    if (opt.inPlace && !done.changed) {
        std::cout << input << ": already stored as requested; left unchanged\n";
        return;
    }
    std::string what;
    auto add = [&](uint64_t n, const std::string& text) {
        if (!n) return;
        if (!what.empty()) what += ", ";
        what += std::to_string(n) + " " + text;
    };
    add(done.compressed, std::string(done.compressed == 1 ? "block" : "blocks") + " compressed with " + codec);
    add(done.decompressed, std::string(done.decompressed == 1 ? "block" : "blocks") + " decompressed");
    add(done.kept, std::string(done.kept == 1 ? "block" : "blocks") + " kept as stored");
    add(done.checksums, std::string(done.checksums == 1 ? "checksum" : "checksums") + " computed");
    add(done.checksums_removed, std::string(done.checksums_removed == 1 ? "checksum" : "checksums") + " removed");
    if (what.empty()) what = "no attached data blocks";
    char percent[32];
    std::snprintf(percent, sizeof percent, "%.1f%%",
                  done.input_size ? 100.0 * static_cast<double>(done.output_size) / static_cast<double>(done.input_size) : 100.0);
    std::cout << input << " -> " << output << ": " << megabytes(done.input_size) << " -> " << megabytes(done.output_size)
              << " (" << percent << "); " << what << (done.read_back ? "; read back and compared with the input" : "") << "\n";
}

void convertXisfInput(const Library& lib, const std::string& input, const Options& opt) {
    if (opt.treeJson) throw Error("--asdf-tree-json needs an ASDF file");
    xisfconv_format format = opt.inPlace ? XISFCONV_FORMAT_XISF : XISFCONV_FORMAT_FITS;
    if (opt.format) format = *opt.format;
    else if (!opt.output.empty()) {
        if (auto f = formatFromExtension(opt.output)) format = *f;
    }
    if (format == XISFCONV_FORMAT_XISF && !opt.dumpHeader && !opt.info) {
        rewriteXisfInput(lib, input, opt);
        return;
    }
    if (opt.dumpHeader || opt.info || opt.inPlace) {
        const OpenFile file(lib, input);
        if (opt.dumpHeader) {
            const char* header = "";
            size_t size = 0;
            lib.check(xisfconv_header_text(file.file, &header, &size));
            std::cout.write(header, static_cast<std::streamsize>(size));   // all of it, whatever bytes it holds
            std::cout << "\n";
        } else if (opt.info) {
            printXisfInfo(lib, input, file.file);
        } else {
            throw Error("--in-place is for rewriting XISF files as XISF");
        }
        return;
    }
    const std::string outPath = outputPathFor(input, opt, format);
    const xisfconv_convert_options c = conversionOptions(opt, format);
    lib.check(xisfconv_convert(lib.ctx, input.c_str(), outPath.c_str(), &c));
    if (!opt.quiet) std::cout << input << " -> " << outPath << "\n";
}

// Converts a FITS or ASDF file. Both readers deliver the images in the same form.
void convertFitsOrAsdfInput(const Library& lib, const std::string& input, bool asdfInput, const Options& opt) {
    if (asdfInput && opt.treeJson) {
        size_t size = 0;
        lib.check(xisfconv_asdf_tree_json(lib.ctx, input.c_str(), nullptr, 0, &size));
        std::string json(size, '\0');
        lib.check(xisfconv_asdf_tree_json(lib.ctx, input.c_str(), &json[0], json.size(), &size));
        std::cout << json << "\n";
        return;
    }
    if (opt.treeJson) throw Error("--asdf-tree-json needs an ASDF file");
    if (asdfInput && opt.dumpHeader) {
        size_t size = 0;
        lib.check(xisfconv_asdf_tree_text(lib.ctx, input.c_str(), nullptr, 0, &size));
        std::string tree(size, '\0');
        lib.check(xisfconv_asdf_tree_text(lib.ctx, input.c_str(), &tree[0], tree.size(), &size));
#ifdef _WIN32
        // The tree is printed byte for byte; text mode would turn its CR LF into CR CR LF.
        std::cout.flush();
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::cout << tree;
        return;
    }
    if (opt.info || opt.dumpHeader) {
        const OpenFile file(lib, input);
        if (asdfInput) printAsdfInfo(lib, input, file.file);
        else printFitsInfo(lib, input, file.file);
        return;
    }
    xisfconv_format format = XISFCONV_FORMAT_XISF;
    if (opt.format) format = *opt.format;
    else if (!opt.output.empty()) {
        if (auto f = formatFromExtension(opt.output)) format = *f;
    }
    if (opt.inPlace) throw Error("--in-place is for rewriting XISF files as XISF");
    const std::string outPath = outputPathFor(input, opt, format);
    const xisfconv_convert_options c = conversionOptions(opt, format);
    lib.check(xisfconv_convert(lib.ctx, input.c_str(), outPath.c_str(), &c));
    if (!opt.quiet) std::cout << input << " -> " << outPath << "\n";
}

// ---------------------------------------------------------------- arguments

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
            if (v == "fits" || v == "fit") opt.format = XISFCONV_FORMAT_FITS;
            else if (v == "tiff" || v == "tif") opt.format = XISFCONV_FORMAT_TIFF;
            else if (v == "png") opt.format = XISFCONV_FORMAT_PNG;
            else if (v == "xisf") opt.format = XISFCONV_FORMAT_XISF;
            else if (v == "xish") opt.format = XISFCONV_FORMAT_XISF;
            else if (v == "asdf") opt.format = XISFCONV_FORMAT_ASDF;
            else throw Error("unknown output format '" + v + "' (use fits, asdf, tiff, png, xisf or xish)");
            opt.distributed = v == "xish";
            opt.monolithic = v == "xisf";
        } else if (a == "--external-files") {
            const std::string v = toLower(need(i, a));
            if (v == "header-dir") opt.externalFiles = XISFCONV_EXTERNAL_HEADER_DIRECTORY;
            else if (v == "anywhere") opt.externalFiles = XISFCONV_EXTERNAL_ANYWHERE;
            else if (v == "none") opt.externalFiles = XISFCONV_EXTERNAL_NONE;
            else throw Error("--external-files expects header-dir, anywhere or none");
        } else if (a == "-o" || a == "--output") opt.output = need(i, a);
        else if (a == "-d" || a == "--outdir") opt.outdir = need(i, a);
        else if (a == "-b" || a == "--bits") {
            xisfconv_sample_format f;
            const std::string v = need(i, a);
            if (!parseShortSampleFormat(v, f)) throw Error("unknown sample format '" + v + "' (use u8, u16, u32, f32, f64)");
            opt.bits = f;
        } else if (a == "-i" || a == "--image") {
            uint64_t n;
            const std::string v = need(i, a);
            if (!parseUInt64(v, n) || n >= XISFCONV_ALL_IMAGES) throw Error("invalid image index '" + v + "'");
            opt.imageIndex = static_cast<size_t>(n);
        } else if (a == "-c" || a == "--compress") opt.compress = true;
        else if (a == "-s" || a == "--stretch") opt.stretch = XISFCONV_STRETCH_AUTO;
        else if (startsWith(a, "--stretch=")) {
            const std::string v = toLower(a.substr(10));
            if (v == "auto") opt.stretch = XISFCONV_STRETCH_AUTO;
            else if (v == "linked") opt.stretch = XISFCONV_STRETCH_LINKED;
            else if (v == "unlinked") opt.stretch = XISFCONV_STRETCH_UNLINKED;
            else if (v == "stf") opt.stretch = XISFCONV_STRETCH_STORED;
            else throw Error("unknown stretch mode '" + v + "' (use auto, linked, unlinked or stf)");
        }
        else if (a == "--bottom-up") { opt.bottomUp = true; opt.rowOrderGiven = true; }
        else if (a == "--top-down") { opt.bottomUp = false; opt.rowOrderGiven = true; }
        else if (a == "--codec") {
            const std::string v = toLower(need(i, a));
            if (v != "zlib" && v != "zstd" && v != "lz4" && v != "lz4hc" && v != "none") {
                throw Error("unknown codec '" + v + "' (use zlib, zstd, lz4, lz4hc or none)");
            }
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
        else if (a == "--bin") {
            uint64_t n;
            const std::string v = need(i, a);
            if (!parseUInt64(v, n) || n == 0 || n > 1000000) throw Error("--bin expects a number of pixels, 1 or more (2 makes one pixel of 2 x 2)");
            opt.bin = n;
        } else if (a == "--resize") {
            // 256: the longest side; 1024x768: a box; 50%: of the width and of the height
            const std::string v = toLower(trim(need(i, a)));
            const char* expects = "--resize expects the longest side in pixels (256), a box to fit (1024x768) or a percentage (50%)";
            uint64_t w = 0, h = 0;
            double percent = 0;
            const size_t x = v.find('x');
            if (!v.empty() && v.back() == '%') {
                if (!parseDouble(v.substr(0, v.size() - 1), percent) || !(percent > 0 && percent <= 100)) throw Error(expects);
                opt.scale = percent / 100;
                opt.fitWidth = opt.fitHeight = 0;
            } else if (x != std::string::npos) {
                if (!parseUInt64(v.substr(0, x), w) || !parseUInt64(v.substr(x + 1), h) || w == 0 || h == 0) throw Error(expects);
                opt.fitWidth = w;
                opt.fitHeight = h;
                opt.scale = 0;
            } else {
                if (!parseUInt64(v, w) || w == 0) throw Error(expects);
                opt.fitWidth = opt.fitHeight = w;
                opt.scale = 0;
            }
        }
        else if (a == "--no-property-keywords") opt.propertyKeywords = false;
        else if (a == "--no-properties") opt.properties = false;
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
        throw Error("cannot infer output format from '" + opt.output + "'; add --to fits|asdf|tiff|png|xisf|xish");
    }
    // The kind of an XISF unit goes with the name of its file: .xish is a header file, any
    // other name a monolithic file.
    if (!opt.output.empty() && opt.format && *opt.format == XISFCONV_FORMAT_XISF) {
        const bool header = lowerExt(opt.output) == ".xish";
        if (opt.distributed && !header) {
            throw Error("-t xish writes a header file, whose name ends in .xish (and its data blocks into the file of "
                        "that name that ends in .xisb): '" + opt.output + "' is no such name");
        }
        if (opt.monolithic && header) {
            throw Error("-t xisf writes one monolithic file, and '" + opt.output + "' is the name of a header file: use "
                        "-t xish, or leave -t out (the kind of unit goes with the name), or give another name");
        }
    }
    if (!opt.outdir.empty() && !fs::is_directory(toPath(opt.outdir))) throw Error("output directory does not exist: " + opt.outdir);
    return true;
}

// ---------------------------------------------------------------- --verify

// Collects the XISF, FITS and ASDF files in and below a directory. Directories that cannot be
// read are reported in `errors`.
void findImageFiles(const fs::path& directory, std::vector<std::string>& found, std::vector<std::string>& errors, int depth = 0) {
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    if (ec || depth > 64) {
        errors.push_back(fromPath(directory) + ": " + (ec ? ec.message() : std::string("directories nested too deeply")));
        return;
    }
    std::vector<fs::path> directories;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            errors.push_back(fromPath(directory) + ": " + ec.message());
            break;
        }
        try {
            std::error_code entryError;
            if (it->is_directory(entryError) && !it->is_symlink(entryError)) {
                directories.push_back(it->path());
            } else if (it->is_regular_file(entryError)) {
                const std::string name = fromPath(it->path());
                const auto format = formatFromExtension(name);   // image.fits.fz is FITS, too
                if (format && (*format == XISFCONV_FORMAT_XISF || *format == XISFCONV_FORMAT_FITS || *format == XISFCONV_FORMAT_ASDF)) {
                    found.push_back(name);
                }
            }
        } catch (const std::exception& e) {
            errors.push_back(fromPath(directory) + ": " + e.what());
        }
    }
    std::sort(directories.begin(), directories.end());
    for (const auto& sub : directories) findImageFiles(sub, found, errors, depth + 1);
}

// --verify: checks every file (and the image files in every directory) given.
int verifyFiles(const Library& lib, const Options& opt) {
    std::vector<std::string> files, errors;
    for (const auto& input : opt.inputs) {
        std::error_code ec;
        if (!fs::is_directory(toPath(input), ec)) {
            files.push_back(input);
            continue;
        }
        std::vector<std::string> found;
        findImageFiles(toPath(input), found, errors);
        std::sort(found.begin(), found.end());
        if (found.empty()) std::cerr << "warning: " << input << ": no XISF, FITS or ASDF files found\n";
        files.insert(files.end(), found.begin(), found.end());
    }

    auto plural = [](size_t n, const char* word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); };
    size_t ok = 0, partly = 0, failed = 0;
    for (const auto& f : files) {
        xisfconv_report* report = nullptr;
        std::vector<std::string> problems;
        const xisfconv_status status = xisfconv_verify(lib.ctx, f.c_str(), &report);
        if (status != XISFCONV_OK) {
            problems.push_back(status == XISFCONV_ERR_MEMORY ? "out of memory" : xisfconv_error_message(lib.ctx));
        } else {
            for (size_t i = 0; i < xisfconv_report_problem_count(report); ++i) problems.push_back(xisfconv_report_problem(report, i));
        }
        if (!problems.empty()) {
            ++failed;
            std::cout << f << ": FAILED\n";
            for (std::string p : problems) {
                // Hints meant for conversions do not apply here.
                for (const char* hint : {" (use --no-verify to convert anyway)", " (the file is damaged; --no-verify skips this check)"}) {
                    const size_t at = p.find(hint);
                    if (at != std::string::npos) p.erase(at, std::strlen(hint));
                }
                std::cout << "  " << p << "\n";
            }
            xisfconv_report_free(report);
            continue;
        }
        const xisfconv_format format = xisfconv_report_format(report);
        const char* kind = format == XISFCONV_FORMAT_FITS ? "FITS" : format == XISFCONV_FORMAT_ASDF ? "ASDF" : "XISF";
        const size_t notChecked = xisfconv_report_not_checked_count(report);
        const size_t verified = xisfconv_report_verified(report), unchecked = xisfconv_report_unchecked(report);
        const bool complete = notChecked == 0;
        ++(complete ? ok : partly);
        if (!(opt.quiet && complete)) {
            std::cout << f << ": " << (complete ? "OK" : "NOT FULLY CHECKED") << " (" << kind << ", " << xisfconv_report_summary(report)
                      << "; ";
            if (verified) std::cout << plural(verified, "checksum") << " verified";
            else std::cout << "no checksums " << (complete ? "in the file" : "verified");
            if (verified && unchecked) std::cout << ", " << unchecked << " without checksum";
            std::cout << ")\n";
            for (size_t i = 0; i < notChecked; ++i) std::cout << "  not checked: " << xisfconv_report_not_checked(report, i) << "\n";
        }
        xisfconv_report_free(report);
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

int run(int argc, char** argv) {
    Options opt;
    try {
        int exitCode = 0;
        if (!parseArgs(argc, argv, opt, exitCode)) return exitCode;
    } catch (const std::exception& e) {
        std::cerr << "xisfconv: " << e.what() << "\n";
        return 2;
    }
    Library lib;
    lib.quiet = opt.quiet;
    xisfconv_context_set_external_files(lib.ctx, opt.externalFiles);
    if (opt.verifyMode) return verifyFiles(lib, opt);
    int failures = 0;
    for (const auto& input : opt.inputs) {
        try {
            // A file that is neither FITS nor ASDF goes to the XISF reader, which says what is wrong with it.
            xisfconv_format kind = XISFCONV_FORMAT_XISF;
            if (xisfconv_detect_format(lib.ctx, input.c_str(), &kind) != XISFCONV_OK) kind = XISFCONV_FORMAT_XISF;
            if (kind == XISFCONV_FORMAT_XISF) convertXisfInput(lib, input, opt);
            else convertFitsOrAsdfInput(lib, input, kind == XISFCONV_FORMAT_ASDF, opt);
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

}  // namespace

#ifdef _WIN32
// The arguments arrive as UTF-16 and are handed on as UTF-8, which is what the library expects;
// the console is told that the program's output is UTF-8 as well.
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(size > 0 ? static_cast<size_t>(size - 1) : 0, '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &arg[0], size, nullptr, nullptr);
        args.push_back(std::move(arg));
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    const UINT codePage = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    const int status = run(argc, argv.data());
    std::cout.flush();
    std::cerr.flush();
    if (codePage) SetConsoleOutputCP(codePage);
    return status;
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
