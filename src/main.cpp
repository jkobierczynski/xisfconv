// xisfconv - convert PixInsight XISF images to FITS, ASDF, TIFF or PNG, and FITS, ASDF or DNG images to XISF.
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
#include <map>
#include <optional>
#include <set>
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
#else
#include <sys/stat.h>
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
    bool debayer = false;        // TIFF and PNG output: a colour picture of a mosaic
    int level = 0;               // XISF output: compression level of the codec; 0: its usual one
    bool shuffle = true;         // XISF output: byte shuffling before compression
    uint64_t fitWidth = 0, fitHeight = 0;   // ... the picture fits that many pixels
    double scale = 0;            // ... the picture is that fraction of the image
    bool verify = true;
    bool wcs = true;
    int sipOrder = 3;
    bool force = false;
    bool skipExisting = false;   // an input whose output is there already is passed over
    bool info = false;
    bool dumpHeader = false;
    bool treeJson = false;  // undocumented: print the parsed ASDF tree as JSON (for the tests)
    bool quiet = false;
    std::vector<std::string> inputs;
};

const char* const kVersion = xisfconv_version();

void usage(std::ostream& os) {
    os << "xisfconv " << kVersion << " - convert between PixInsight XISF, FITS and ASDF images; read DNG; export TIFF and PNG\n\n"
          "Usage: xisfconv [options] <file or directory>...\n"
          "       XISF inputs are converted to FITS (default), ASDF, TIFF or PNG, or rewritten as XISF\n"
          "       with another compression or checksum (-t xisf). An XISF input is a monolithic file\n"
          "       (.xisf) or the header file of a distributed unit (.xish), whose data is in the\n"
          "       files that header names (.xisb); -t xish writes such a unit, -t xisf one file;\n"
          "       FITS inputs to XISF (default), ASDF, TIFF or PNG; tile-compressed FITS (.fits.fz)\n"
          "       is read like any FITS file, -t fits writes it as a plain FITS file, and -t fits -c\n"
          "       writes a FITS file tile-compressed;\n"
          "       ASDF inputs to XISF (default), FITS, TIFF or PNG;\n"
          "       DNG inputs (camera raw) to XISF (default), FITS, ASDF, TIFF or PNG: the raw image as the\n"
          "       sensor recorded it, not demosaiced, with the colour filter pattern and the exposure.\n"
          "       A directory stands for the XISF, FITS, ASDF and DNG files in it and below it: those that are\n"
          "       not yet what is written are converted (-t says what; a directory of one format needs\n"
          "       no -t). An argument with * or ? that names no file is a pattern for the names it\n"
          "       matches, on every system (cmd and PowerShell leave patterns to the program).\n"
          "       xisfconv --verify <file or directory>... checks files without converting them.\n\n"
          "Output:\n"
          "  -t, --to <fits|asdf|tiff|png|xisf|xish>\n"
          "                              output format (default: fits for XISF input, xisf for FITS, ASDF and\n"
          "                              DNG input, or taken from -o's extension). xish: XISF as a distributed\n"
          "                              unit, the header in <name>.xish and the data blocks in <name>.xisb\n"
          "                              beside it (PixInsight itself opens monolithic .xisf files only)\n"
          "  -o, --output <file>         output file name (single input only)\n"
          "  -d, --outdir <dir>          directory for output files (default: next to each input). The files\n"
          "                              of a directory given as input keep their places below it\n"
          "  -f, --force                 overwrite existing output files\n"
          "      --skip-existing         leave an output that exists as it is and pass its input over, so\n"
          "                              that a run on a directory converts what was added since the last one\n"
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
          "      --debayer               TIFF and PNG output: a colour picture of the mosaic of a colour camera\n"
          "                              (bilinear, by the 2 x 2 pattern of BAYERPAT, or of the file), before\n"
          "                              --bin, --resize and --stretch; without white balance\n"
          "      --top-down              XISF and DNG input: keep the top-down row order in FITS and ASDF output\n"
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
          "                              also byte shuffled (but with --no-shuffle); lz4 and lz4hc are for\n"
          "                              XISF only. zstd in ASDF needs the asdf-compression package in\n"
          "                              Python. none: no compression;\n"
          "                              XISF -> XISF: decompress the blocks\n"
          "                              FITS: zlib compresses the tiles of every sample type with gzip\n"
          "                              (GZIP_2; GZIP_1 for 8-bit data); there is no zstd or lz4 for FITS\n"
          "      --level <n>             XISF: the compression level of the codec (implies --compress): zlib 1-9\n"
          "                              (default 6), lz4hc 1-12 (9), zstd 1-22 (3); lz4 has none. Higher is\n"
          "                              smaller and slower to write, as fast to read. XISF -> XISF: every\n"
          "                              block is compressed again (a file does not say its level)\n"
          "      --no-shuffle            XISF: no byte shuffling before compression (implies --compress)\n"
          "      --checksum <sha1|sha256|sha512|sha3-256|sha3-512|none>\n"
          "                              XISF: store a checksum of the pixel data block; XISF -> XISF: of every\n"
          "                              attached block (none removes them). ASDF blocks always carry MD5.\n"
          "                              PixInsight opens files with sha1, sha256 and sha512 checksums only\n\n"

          "Inspection:\n"
          "      --verify                check the integrity of the files (and of the XISF, FITS, ASDF and DNG\n"
          "                              files in the directories) given: checksums are verified, compressed\n"
          "                              data is decompressed, sizes are compared. Exit status 1 on a failure\n"
          "  -I, --info                  print image geometry, keywords and properties; no conversion\n"
          "      --dump-header           print the raw XML header (XISF), all keywords (FITS, DNG) or the YAML tree\n"
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
    if (e == ".dng") return XISFCONV_FORMAT_DNG;   // (read, never written)
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
    c.debayer = opt.debayer;
    c.compression_level = opt.level;
    c.shuffle = opt.shuffle;
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

// DNG: the raw image, and what the other images of the file are.
void printDngInfo(const Library& lib, const std::string& path, xisfconv_file* f) {
    std::cout << path << ": " << xisfconv_file_detail(f, "format") << ", " << xisfconv_file_size(f) << " bytes\n";
    const xisfconv_image_info img = infoOf(lib, f, 0);
    std::cout << "\nRaw image at " << xisfconv_image_detail(f, 0, "source") << ": " << img.width << " x " << img.height << " x "
              << img.channels << ", " << xisfconv_image_detail(f, 0, "storage") << "\n";
    std::cout << "  rows:        top-down\n";
    if (img.has_cfa) {
        std::cout << "  CFA:         " << xisfconv_image_detail(f, 0, "cfaPattern") << " (" << img.cfa_width << "x" << img.cfa_height
                  << ")\n";
    }
    printKeywords(cardsOf(lib, f, 0));
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
    if (opt.debayer) throw Error("--debayer makes a colour picture: it is for TIFF and PNG output");
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
    r.compression_level = opt.level;
    r.shuffle = opt.shuffle;
    if (opt.level && !codec.empty() && codec != "none") codec += " (level " + std::to_string(opt.level) + ")";
    if (!opt.shuffle && !codec.empty() && codec != "none") codec += ", not shuffled";

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

// Converts a FITS, ASDF or DNG file. The readers deliver the images in the same form.
void convertFitsOrAsdfInput(const Library& lib, const std::string& input, xisfconv_format inputFormat, const Options& opt) {
    const bool asdfInput = inputFormat == XISFCONV_FORMAT_ASDF;
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
        else if (inputFormat == XISFCONV_FORMAT_DNG) printDngInfo(lib, input, file.file);
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
#ifdef _WIN32
        // "/?" asks a Windows program what it does. (As a pattern it would be every name of one
        // letter in the root of the drive.)
        if (!endOfOptions && a == "/?") {
            usage(std::cout);
            exitCode = 0;
            return false;
        }
#endif
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
        } else if (a == "--level") {
            uint64_t n;
            const std::string v = need(i, a);
            if (!parseUInt64(v, n) || n == 0 || n > 100) throw Error("--level expects the compression level of the codec: zlib 1-9, lz4hc 1-12, zstd 1-22");
            opt.level = static_cast<int>(n);
            opt.compress = true;
        } else if (a == "--no-shuffle") {
            opt.shuffle = false;
            opt.compress = true;
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
        else if (a == "--debayer") opt.debayer = true;
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
        else if (a == "--skip-existing") opt.skipExisting = true;
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
    if (opt.skipExisting && opt.force) throw Error("--skip-existing leaves outputs that exist and --force writes over them: give one of the two");
    if (opt.skipExisting && opt.inPlace) throw Error("--skip-existing is for outputs under names of their own; --in-place replaces the input itself");
    if (!opt.output.empty() && opt.inputs.size() > 1) throw Error("-o/--output can only be used with a single input");
    if (!opt.output.empty() && !opt.format && !formatFromExtension(opt.output)) {
        throw Error("cannot infer output format from '" + opt.output + "'; add --to fits|asdf|tiff|png|xisf|xish");
    }
    if (!opt.output.empty() && !opt.format && formatFromExtension(opt.output) == XISFCONV_FORMAT_DNG) {
        throw Error(opt.output + ": DNG is read, not written; add --to fits|asdf|tiff|png|xisf|xish");
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

// ---------------------------------------------------------------- patterns and directories

// The characters of a name, for comparing. A byte that is not UTF-8 counts as a character of its own.
std::u32string codePoints(const std::string& text) {
    std::u32string out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        char32_t value = length == 1 ? c : length ? static_cast<char32_t>(c & (0xFF >> (length + 1))) : 0;
        bool whole = length && i + length <= text.size();
        for (size_t k = 1; whole && k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            whole = (next & 0xC0) == 0x80;
            value = (value << 6) | (next & 0x3F);
        }
        if (whole) {
            out += value;
            i += length;
        } else {
            out += static_cast<char32_t>(0x110000 + c);   // no character has this number
            ++i;
        }
    }
    return out;
}

// A character as this system compares names: Windows takes "A" and "a" for one.
char32_t folded(char32_t c) {
#ifdef _WIN32
    if (c < 0x10000 && !(c >= 0xD800 && c < 0xE000)) {
        const wchar_t in = static_cast<wchar_t>(c);
        wchar_t upper = in;
        if (LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, &in, 1, &upper, 1) == 1) return upper;
    }
#endif
    return c;
}

bool hasWildcard(const std::string& text) { return text.find_first_of("*?") != std::string::npos; }

// True if `name` is one of the names the pattern stands for: * is any characters, also none, and
// ? is exactly one; every other character is itself ("[" and "]" too, which Windows has in names
// and its shells do not take for anything). As in the shells of Unix, a name that begins with a
// dot is matched only by a pattern that begins with one.
bool matchesPattern(const std::string& pattern, const std::string& name) {
    if (!name.empty() && name[0] == '.' && (pattern.empty() || pattern[0] != '.')) return false;
    const std::u32string p = codePoints(pattern), n = codePoints(name);
    const size_t none = std::u32string::npos;
    size_t pi = 0, ni = 0, star = none, mark = 0;
    while (ni < n.size()) {
        if (pi < p.size() && p[pi] == U'*') {
            star = pi++;
            mark = ni;
        } else if (pi < p.size() && (p[pi] == U'?' || folded(p[pi]) == folded(n[ni]))) {
            ++pi;
            ++ni;
        } else if (star != none) {   // let the last * take one character more
            pi = star + 1;
            ni = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == U'*') ++pi;
    return pi == p.size();
}

bool isSeparator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

// An argument as a pattern is read: the root, which is taken as it stands, the parts behind it,
// and whether it ends in a separator. The root is cut off by hand: std::filesystem does not agree
// with itself across compilers on what the root of "\\?\C:\dir" is, and joins such paths wrongly.
struct PatternPath {
    std::string root;                 // "", "/", "C:", "C:\", "\\server\share\", "\\?\C:\", "\\?\UNC\server\share\"
    std::vector<std::string> parts;   // the names between the separators
    bool directory = false;           // it ends in a separator: directories are meant
};

PatternPath splitPattern(const std::string& text) {
    PatternPath out;
    size_t at = 0;
#ifdef _WIN32
    const auto partEnd = [&](size_t from) {
        from = std::min(from, text.size());
        while (from < text.size() && !isSeparator(text[from])) ++from;
        return from;
    };
    if (text.size() >= 2 && isSeparator(text[0]) && isSeparator(text[1])) {
        size_t end = partEnd(2);                         // the server, or "?" or "."
        const std::string first = text.substr(2, end - 2);
        size_t more = 1;                                 // one more part is of the root: the share, or the drive
        if (first == "?" || first == ".") {
            const size_t next = partEnd(end + 1);
            if (end < text.size() && toLower(text.substr(end + 1, next - end - 1)) == "unc") more = 3;   // UNC, server, share
        }
        for (size_t k = 0; k < more && end < text.size(); ++k) end = partEnd(end + 1);
        at = end < text.size() ? end + 1 : end;
    } else if (text.size() >= 2 && text[1] == ':' && std::isalpha(static_cast<unsigned char>(text[0]))) {
        at = text.size() > 2 && isSeparator(text[2]) ? 3 : 2;   // "C:\dir", or "C:dir" in the current directory of C:
    } else if (!text.empty() && isSeparator(text[0])) {
        at = 1;
    }
#else
    while (at < text.size() && text[at] == '/') ++at;
#endif
    out.root = text.substr(0, at);
    std::string part;
    for (; at < text.size(); ++at) {
        if (!isSeparator(text[at])) {
            part += text[at];
        } else if (!part.empty()) {
            out.parts.push_back(part);
            part.clear();
        }
    }
    if (!part.empty()) out.parts.push_back(part);
    out.directory = !out.parts.empty() && isSeparator(text.back());
    return out;
}

// A name in a directory, as one path.
std::string joined(const std::string& directory, const std::string& name) {
    if (directory.empty() || isSeparator(directory.back())) return directory + name;
#ifdef _WIN32
    if (directory.size() == 2 && directory[1] == ':') return directory + name;   // "C:" + "name": in the current directory of C:
    return directory + '\\' + name;
#else
    return directory + '/' + name;
#endif
}

// True for an argument that is taken as a pattern: one with * or ? behind its root that is not
// the name of something that is there. (On Unix a file may be called "what?.xisf", and then
// that file is meant; and the "\\?\" a long Windows path begins with is no wildcard.)
bool isPattern(const std::string& argument) {
    bool wildcard = false;
    for (const auto& part : splitPattern(argument).parts) wildcard = wildcard || hasWildcard(part);
    if (!wildcard) return false;
    std::error_code ec;
    return !fs::exists(fs::symlink_status(toPath(argument), ec));
}

// The names a pattern stands for, those of each directory in their order: files and
// directories, and only directories if the pattern ends in a separator. (Not a pipe or a device,
// which a directory is not searched for either.) The wildcards may be in any part of the path:
// "night*/lights/*.xisf". A directory that is there and cannot be read goes to `errors`.
std::vector<std::string> expandPattern(const std::string& argument, std::vector<std::string>& errors) {
    const PatternPath pattern = splitPattern(argument);
    std::vector<std::string> found{pattern.root};
    for (size_t k = 0; k < pattern.parts.size() && !found.empty(); ++k) {
        const std::string& part = pattern.parts[k];
        const bool last = k + 1 == pattern.parts.size();
        std::vector<std::string> next;
        for (const auto& base : found) {
            if (!hasWildcard(part)) {
                next.push_back(joined(base, part));
                continue;
            }
            const std::string shown = base.empty() ? std::string(".") : base;
            std::error_code ec;
            if (!fs::is_directory(toPath(shown), ec)) continue;
            std::vector<std::string> here;
            fs::directory_iterator it(toPath(shown), ec);
            for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
                try {
                    const std::string name = fromPath(it->path().filename());
                    if (!matchesPattern(part, name)) continue;
                    std::error_code entryError;
                    if (!last && !it->is_directory(entryError)) continue;   // more of the path follows: it leads on through directories
                    here.push_back(name);
                } catch (const std::exception&) {
                    // (a name that cannot be written as UTF-8: no pattern stands for it)
                }
            }
            if (ec) errors.push_back(shown + ": " + ec.message());
            std::sort(here.begin(), here.end());
            for (const auto& name : here) next.push_back(joined(base, name));
        }
        found.swap(next);
    }
    std::vector<std::string> names;
    for (const auto& name : found) {
        std::error_code ec;
        const fs::file_status status = fs::status(toPath(name), ec);
        if (fs::is_directory(status) || (!pattern.directory && fs::is_regular_file(status))) names.push_back(name);
    }
    return names;
}

// An argument as it is meant.
struct Name {
    std::string path;
    bool matched = false;   // it is one of the names a pattern stands for
};

// The arguments as they are meant: a pattern replaced by the names it matches. A pattern that
// matches nothing goes to `unmatched`.
std::vector<Name> expandPatterns(const std::vector<std::string>& arguments, std::vector<std::string>& unmatched,
                                 std::vector<std::string>& errors, bool& expanded) {
    std::vector<Name> names;
    for (const auto& argument : arguments) {
        if (!isPattern(argument)) {
            names.push_back({argument, false});
            continue;
        }
        expanded = true;
        const size_t errorsBefore = errors.size();
        const std::vector<std::string> matched = expandPattern(argument, errors);
        if (matched.empty() && errors.size() == errorsBefore) unmatched.push_back(argument);   // (else that error says it)
        for (const auto& name : matched) names.push_back({name, true});
    }
    return names;
}

// How a directory is searched.
struct Search {
    bool hidden = true;                 // take names that begin with a dot as well
    const fs::path* outputs = nullptr;  // a directory that is not searched: the one the outputs go to
};

// Collects the XISF, FITS, ASDF and DNG files in and below a directory, by their names. Directories
// that cannot be read are reported in `errors`. A link to a directory is not followed.
void findImageFiles(const fs::path& directory, const Search& search, std::vector<std::string>& found, std::vector<std::string>& errors,
                    int depth = 0) {
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    if (ec || depth > 64) {
        errors.push_back(fromPath(directory) + ": " + (ec ? ec.message() : std::string("directories nested too deeply")));
        return;
    }
    std::vector<fs::path> directories;
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        try {
            std::error_code entryError;
            const std::string leaf = fromPath(it->path().filename());
            if (!search.hidden && !leaf.empty() && leaf[0] == '.') continue;
            if (it->is_directory(entryError) && !it->is_symlink(entryError)) {
                if (search.outputs && fs::equivalent(it->path(), *search.outputs, entryError)) continue;
                directories.push_back(it->path());
            } else if (it->is_regular_file(entryError)) {
                const std::string name = fromPath(it->path());
                const auto format = formatFromExtension(name);   // image.fits.fz is FITS, too
                if (format && (*format == XISFCONV_FORMAT_XISF || *format == XISFCONV_FORMAT_FITS || *format == XISFCONV_FORMAT_ASDF ||
                               *format == XISFCONV_FORMAT_DNG)) {
                    found.push_back(name);
                }
            }
        } catch (const std::exception& e) {
            errors.push_back(fromPath(directory) + ": " + e.what());
        }
    }
    // (An iterator that fails stands at the end: the listing stopped short, and that is said.)
    if (ec) errors.push_back(fromPath(directory) + ": " + ec.message());
    std::sort(directories.begin(), directories.end());
    for (const auto& sub : directories) findImageFiles(sub, search, found, errors, depth + 1);
}

// What a file is, by its name: the kinds a conversion makes one of the other of.
enum class Kind { XisfFile, XisfUnit, Fits, PackedFits, Asdf, Dng };   // (a DNG file is never written)

Kind kindOfName(const std::string& path) {
    const std::string extension = lowerExt(path);
    if (extension == ".xish") return Kind::XisfUnit;
    if (extension == ".fz") return Kind::PackedFits;
    const auto format = formatFromExtension(path);
    return format == XISFCONV_FORMAT_FITS ? Kind::Fits : format == XISFCONV_FORMAT_ASDF ? Kind::Asdf
           : format == XISFCONV_FORMAT_DNG    ? Kind::Dng
                                              : Kind::XisfFile;
}

bool isXisf(Kind kind) { return kind == Kind::XisfFile || kind == Kind::XisfUnit; }

// The kind of file a conversion to `format` writes with these options; none for a picture.
std::optional<Kind> kindWritten(const Options& opt, xisfconv_format format) {
    switch (format) {
        case XISFCONV_FORMAT_FITS: return opt.compress ? Kind::PackedFits : Kind::Fits;
        case XISFCONV_FORMAT_ASDF: return Kind::Asdf;
        case XISFCONV_FORMAT_XISF: return opt.distributed ? Kind::XisfUnit : Kind::XisfFile;
        default: return std::nullopt;
    }
}

std::string countOfKind(size_t n, Kind kind) {
    const char* what = kind == Kind::XisfFile ? "monolithic XISF file" : kind == Kind::XisfUnit ? "distributed XISF unit"
                       : kind == Kind::Fits ? "FITS file" : kind == Kind::PackedFits ? "tile-compressed FITS file"
                       : kind == Kind::Asdf ? "ASDF file" : "DNG file";
    return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
}

// One file to work on.
struct Input {
    std::string path;
    std::string below;   // found in a directory that was given: where it is below that directory ("night1/darks")
    xisfconv_format format = XISFCONV_FORMAT_XISF;   // what its first bytes say it is
    bool image = false;  // ... and whether they say that it is one of the three formats at all
};

// What a run works on.
struct Plan {
    std::vector<Input> inputs;
    std::vector<std::string> errors;   // directories that could not be read
    size_t passedOver = 0;             // files of directories that are what is written already
    bool many = false;                 // a directory or a pattern was given: the run ends with its counts
};

// Where a file found below `directory` is, below it: "" for a file of the directory itself.
std::string placeBelow(const fs::path& directory, const fs::path& file) {
    auto d = directory.begin();
    auto f = file.begin();
    while (d != directory.end() && f != file.end() && *d == *f) {   // ("lights/" ends in an empty part)
        ++d;
        ++f;
    }
    fs::path rest;
    for (; f != file.end(); ++f) rest /= *f;
    return fromPath(rest.parent_path());
}

// The files of a directory that this run works on. A file that is named is converted whatever
// it is; of a directory, the files that are not yet what is written.
void planDirectory(const std::string& directory, const Options& opt, Plan& plan) {
    Search search;
    search.hidden = false;
    const fs::path outputs = toPath(opt.outdir);
    if (!opt.outdir.empty()) search.outputs = &outputs;
    std::vector<std::string> found;
    findImageFiles(toPath(directory), search, found, plan.errors);
    std::sort(found.begin(), found.end());
    if (found.empty()) {
        if (!opt.quiet) std::cerr << "warning: " << directory << ": no XISF, FITS, ASDF or DNG files found\n";
        return;
    }
    size_t xisf = 0;
    for (const auto& file : found) xisf += isXisf(kindOfName(file));
    const bool reads = opt.info || opt.dumpHeader || opt.treeJson;
    if (!reads && !opt.inPlace && !opt.format && xisf && xisf != found.size()) {
        throw Error(directory + " holds " + std::to_string(xisf) + " XISF and " + std::to_string(found.size() - xisf) +
                    " FITS, ASDF or DNG " + (found.size() - xisf == 1 ? "file" : "files") + ": say with -t what to make of them "
                    "(-t fits converts what is not FITS, -t xisf what is not XISF)");
    }
    std::map<Kind, size_t> passed;
    for (const auto& file : found) {
        const Kind kind = kindOfName(file);
        if (!reads) {
            if (opt.inPlace) {
                if (!isXisf(kind)) {
                    ++passed[kind];
                    continue;
                }
            } else {
                const xisfconv_format format = opt.format ? *opt.format : isXisf(kind) ? XISFCONV_FORMAT_FITS : XISFCONV_FORMAT_XISF;
                const std::optional<Kind> written = kindWritten(opt, format);
                if (written && *written == kind) {
                    ++passed[kind];
                    continue;
                }
            }
        }
        plan.inputs.push_back({file, placeBelow(toPath(directory), toPath(file))});
    }
    for (const auto& entry : passed) {
        plan.passedOver += entry.second;
        if (!opt.quiet) {
            std::cerr << "info: " << directory << ": " << countOfKind(entry.second, entry.first) << " passed over ("
                      << (opt.inPlace ? "--in-place rewrites XISF files" : "that is what is written") << ")\n";
        }
    }
}

// A name for telling whether two paths are one file: absolute, without "." and "..", links
// followed as far as the path exists, and in the letters this system compares names by.
std::u32string sameFileKey(const std::string& path) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(toPath(path), ec);
    fs::path whole = ec ? toPath(path) : fs::weakly_canonical(absolute, ec);
    if (ec) whole = absolute.lexically_normal();
    std::u32string key = codePoints(fromPath(whole));
    for (auto& c : key) c = folded(c);
    return key;
}

// A name for telling whether two paths are one name: as sameFileKey, but a link at the end of
// the path is not followed.
std::u32string nameKey(const std::string& path) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(toPath(path), ec);
    if (ec) return sameFileKey(path);
    fs::path whole = fs::weakly_canonical(absolute.parent_path(), ec);
    whole = ec ? absolute.lexically_normal() : whole / absolute.filename();
    std::u32string key = codePoints(fromPath(whole));
    for (auto& c : key) c = folded(c);
    return key;
}

// The file a name leads to, where the system has a number for that. Two names may be one file
// without looking it: on a disk that takes "Frame.fits" and "frame.fits" for one name (macOS as
// it comes, a camera's card), and through a hard link.
using FileId = std::pair<uint64_t, uint64_t>;

std::optional<FileId> fileId(const std::string& path) {
#ifndef _WIN32
    struct stat status;
    if (::stat(path.c_str(), &status) == 0) return FileId(static_cast<uint64_t>(status.st_dev), static_cast<uint64_t>(status.st_ino));
#else
    (void)path;   // (sameFileKey compares names as Windows does)
#endif
    return std::nullopt;
}

Plan planInputs(const std::vector<Name>& names, const Options& opt) {
    Plan plan;
    for (const auto& name : names) {
        std::error_code ec;
        if (!fs::is_directory(toPath(name.path), ec)) {
            plan.inputs.push_back({name.path, std::string()});
            continue;
        }
        if (!opt.output.empty()) throw Error("-o/--output names one file, and " + name.path + " is a directory: give a directory with -d");
        plan.many = true;
        // A pattern that also matches the directory of the outputs ("*" with "-d out") does not mean it.
        if (name.matched && !opt.outdir.empty() && fs::equivalent(toPath(name.path), toPath(opt.outdir), ec)) {
            if (!opt.quiet) std::cerr << "info: " << name.path << ": the directory of the outputs (-d) is not searched\n";
            continue;
        }
        planDirectory(name.path, opt, plan);
    }
    // An input that is given twice (a directory and a file of it, "a.xisf ./a.xisf") is one input,
    // the first mention of it. (A link to a file is a name of its own, with an output of its own.)
    std::set<std::u32string> seen;
    std::vector<Input> once;
    for (auto& in : plan.inputs) {
        if (seen.insert(nameKey(in.path)).second) once.push_back(std::move(in));
    }
    plan.inputs.swap(once);
    if (!opt.output.empty() && plan.inputs.size() > 1) throw Error("-o/--output can only be used with a single input");
    return plan;
}

// The format a file is converted to, as the options and its own format say.
xisfconv_format formatFor(xisfconv_format input, const Options& opt) {
    if (opt.format) return *opt.format;
    if (!opt.output.empty()) {
        if (const auto f = formatFromExtension(opt.output)) return *f;
    }
    return input == XISFCONV_FORMAT_XISF ? XISFCONV_FORMAT_FITS : XISFCONV_FORMAT_XISF;
}

// ---------------------------------------------------------------- --verify

// --verify: checks every file (and the image files in every directory) given.
// (`unmatched`: how many patterns among the arguments matched nothing; they are reported already.)
int verifyFiles(const Library& lib, const Options& opt, const std::vector<Name>& names, size_t unmatched) {
    std::vector<std::string> files, errors;
    for (const auto& name : names) {
        const std::string& input = name.path;
        std::error_code ec;
        if (!fs::is_directory(toPath(input), ec)) {
            files.push_back(input);
            continue;
        }
        std::vector<std::string> found;
        findImageFiles(toPath(input), Search(), found, errors);
        std::sort(found.begin(), found.end());
        if (found.empty() && !opt.quiet) std::cerr << "warning: " << input << ": no XISF, FITS, ASDF or DNG files found\n";
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
        const char* kind = format == XISFCONV_FORMAT_FITS ? "FITS" : format == XISFCONV_FORMAT_ASDF ? "ASDF"
                           : format == XISFCONV_FORMAT_DNG  ? "DNG"
                                                            : "XISF";
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
    failed += errors.size() + unmatched;
    if ((files.size() + errors.size() + unmatched > 1) && !opt.quiet) {
        std::cout << "\n" << plural(ok, "file") << " OK, ";
        if (partly) std::cout << partly << " not fully checked, ";
        std::cout << failed << " failed\n";
    }
    return failed ? 1 : 0;
}

// Directories that were made for an output and are not needed after all: they are taken away
// again, the deepest first, if nothing is in them.
void removeEmpty(const std::vector<fs::path>& made) {
    for (auto it = made.rbegin(); it != made.rend(); ++it) {
        std::error_code ec;
        fs::remove(*it, ec);
    }
}

int convertFiles(const Library& lib, Options& opt) {
    // What the arguments stand for: patterns first, then the files of the directories.
    std::vector<std::string> unmatched, unreadable;
    bool expanded = false;
    const std::vector<Name> names = expandPatterns(opt.inputs, unmatched, unreadable, expanded);
    opt.inputs.clear();
    for (const auto& e : unreadable) std::cerr << "error: " << e << "\n";
    for (const auto& pattern : unmatched) std::cerr << "error: " << pattern << ": no file matches this pattern\n";
    if (opt.verifyMode) return verifyFiles(lib, opt, names, unmatched.size() + unreadable.size());
    Plan plan;
    try {
        plan = planInputs(names, opt);
    } catch (const Error& e) {   // (nothing has been read or written yet)
        std::cerr << "xisfconv: " << e.what() << "\n";
        return 2;
    }
    for (const auto& e : plan.errors) std::cerr << "error: " << e << "\n";
    size_t failures = unmatched.size() + unreadable.size() + plan.errors.size(), done = 0, passedOver = plan.passedOver, inTheWay = 0;

    // A run that writes files under names of their own does not write a file twice, and not over
    // a file it reads: of two inputs with one output name the second is refused, with or without
    // --force, and so is an input whose output is another input. An input is a file that is one
    // of the three formats: a picture of an earlier run that "*" brought along is not read, and
    // does not stand in the way of the file it is made from.
    const bool converts = !(opt.info || opt.dumpHeader || opt.treeJson || opt.inPlace);
    std::set<std::u32string> inputKeys;
    std::set<FileId> inputIds;
    std::map<std::u32string, std::string> written;
    std::map<FileId, std::u32string> writtenIds;   // the same outputs, by the file they became
    for (auto& in : plan.inputs) {
        // A file that is neither FITS, ASDF nor DNG goes to the XISF reader, which says what is wrong with it.
        in.image = xisfconv_detect_format(lib.ctx, in.path.c_str(), &in.format) == XISFCONV_OK;
        if (!in.image) in.format = XISFCONV_FORMAT_XISF;
        if (converts && in.image) {
            inputKeys.insert(sameFileKey(in.path));
            if (const auto id = fileId(in.path)) inputIds.insert(*id);
        }
    }
    for (const auto& in : plan.inputs) {
        const std::string& input = in.path;
        std::vector<fs::path> made;   // directories made for this output
        try {
            Options own = opt;
            if (!in.below.empty() && !opt.outdir.empty()) own.outdir = fromPath(toPath(opt.outdir) / toPath(in.below));
            std::u32string key;
            std::string planned;
            // (A file that is no image goes straight to the reader, which says what it is not.)
            if (converts && in.image) {
                planned = outputPathFor(input, own, formatFor(in.format, own));
                key = sameFileKey(planned);
                const std::optional<FileId> id = fileId(planned), ownId = fileId(input);
                const bool nameItself = key == sameFileKey(input);           // (the library says what it thinks of that)
                const bool itself = nameItself || (id && id == ownId);       // ... and of a second name of the input
                // Two inputs with one output: the second is refused, whatever else is asked for.
                auto before = written.find(key);
                if (before == written.end() && id) {
                    const auto same = writtenIds.find(*id);
                    if (same != writtenIds.end()) before = written.find(same->second);
                }
                if (before != written.end()) {
                    throw Error("its output " + planned + " was written in this run already, from " + before->second +
                                ": two inputs cannot have one output (convert them in runs of their own, or into "
                                "directories of their own with -d)");
                }
                std::error_code ec;
                const bool there = !nameItself && fs::exists(fs::symlink_status(toPath(planned), ec));
                if (opt.skipExisting && there) {
                    if (!opt.quiet) std::cout << input << ": " << planned << " exists; passed over\n";
                    ++passedOver;
                    continue;
                }
                if (!itself && (inputKeys.count(key) || (id && inputIds.count(*id)))) {
                    throw Error("its output " + planned + " is an input of this run and is not written over: convert the "
                                "formats in runs of their own (-t), or give another directory for the outputs (-d)");
                }
                if (there && !opt.force && !fs::exists(toPath(planned), ec)) {   // (the library would take the name for free)
                    throw Error(planned + " is a link that leads nowhere; it is not replaced (use --force to overwrite)");
                }
                inTheWay += there && !itself && !opt.force;
                if (own.outdir != opt.outdir) {
                    // the directories below -d that are not there yet
                    std::vector<fs::path> missing;
                    for (fs::path d = toPath(own.outdir); !d.empty() && !fs::exists(d, ec); d = d.parent_path()) missing.push_back(d);
                    made.assign(missing.rbegin(), missing.rend());
                    fs::create_directories(toPath(own.outdir), ec);
                    if (ec) throw Error("the directory " + own.outdir + " could not be made (" + ec.message() + ")");
                }
            }
            if (in.format == XISFCONV_FORMAT_XISF) convertXisfInput(lib, input, own);
            else convertFitsOrAsdfInput(lib, input, in.format, own);
            if (converts && in.image) {
                written[key] = input;
                if (const auto id = fileId(planned)) writtenIds[*id] = key;
            }
            ++done;
        } catch (const std::bad_alloc&) {
            std::cerr << "error: " << input << ": out of memory\n";
            ++failures;
            removeEmpty(made);
        } catch (const std::exception& e) {
            std::cerr << "error: " << input << ": " << e.what() << "\n";
            ++failures;
            removeEmpty(made);
        }
    }
    if ((plan.many || expanded) && !opt.quiet && !(opt.info || opt.dumpHeader || opt.treeJson)) {
        std::cout << "\n" << done << (done == 1 ? " file " : " files ") << (opt.inPlace ? "done" : "converted") << ", " << passedOver
                  << " passed over, " << failures << " failed\n";
        if (inTheWay) {
            std::cout << "(" << inTheWay << (inTheWay == 1 ? " output was" : " outputs were") << " there already and not written over: "
                      << "--skip-existing passes such inputs over, --force replaces the outputs)\n";
        }
    }
    return failures ? 1 : 0;
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
    try {
        Library lib;
        lib.quiet = opt.quiet;
        xisfconv_context_set_external_files(lib.ctx, opt.externalFiles);
        return convertFiles(lib, opt);
    } catch (const std::bad_alloc&) {
        std::cerr << "xisfconv: out of memory\n";
        return 1;
    } catch (const std::exception& e) {   // (a name the system has and this program cannot write down, where none is expected)
        std::cerr << "xisfconv: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace

#if defined(_WIN32) && defined(__MINGW32__)
// The program expands patterns itself (expandPattern), the same way whatever compiler built it:
// the runtime of MinGW is told not to.
extern "C" {
int _dowildcard = 0;
}
#endif

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
