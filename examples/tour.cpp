// A tour of libxisfconv in C++: every chapter of the manual (docs/manual.html) as one function.
//
//   c++ -std=c++17 tour.cpp $(pkg-config --cflags --libs xisfconv) -o tour
//   ./tour integrated_light.xisf out/          (the directory must exist; files in it are replaced)
//
// The library has one interface, the C API of xisfconv.h, and that header is C++ as it stands.
// What C++ adds is in the namespace xisf below: handles that free themselves, and failures as
// exceptions. It is forty lines, meant to be copied into a program and changed to its taste.
// The lines between a pair of marks like [inspect] and [/inspect] are what the manual shows.
//
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xisfconv.h"

// [raii]
namespace xisf {

// A failure of the library: its status, and as what() the text the context has for it.
struct Error : std::runtime_error {
    xisfconv_status status;
    Error(xisfconv_status s, const std::string& text) : std::runtime_error(text), status(s) {}
};

// Handles that free themselves, each with the function the header names for it.
struct Free {
    void operator()(xisfconv_context* p) const { xisfconv_context_free(p); }
    void operator()(xisfconv_file* p) const { xisfconv_close(p); }
    void operator()(xisfconv_keywords* p) const { xisfconv_keywords_free(p); }
    void operator()(xisfconv_properties* p) const { xisfconv_properties_free(p); }
    void operator()(xisfconv_report* p) const { xisfconv_report_free(p); }
    void operator()(xisfconv_writer* p) const { xisfconv_writer_discard(p); }
};
template <class T>
using Handle = std::unique_ptr<T, Free>;
using Context = Handle<xisfconv_context>;
using File = Handle<xisfconv_file>;

// Throws if a call did not succeed.
inline void check(const Context& ctx, xisfconv_status status) {
    if (status != XISFCONV_OK) throw Error(status, xisfconv_error_message(ctx.get()));
}

// An options struct, filled with its defaults: auto o = xisf::defaults(xisfconv_read_options_init);
template <class T>
T defaults(void (*init)(T*, size_t)) {
    T options;
    init(&options, sizeof options);
    return options;
}

inline File open(const Context& ctx, const std::string& path) {
    xisfconv_file* file = nullptr;
    check(ctx, xisfconv_open(ctx.get(), path.c_str(), &file));
    return File(file);
}

}  // namespace xisf
// [/raii]

namespace {

using ull = unsigned long long;  // what printf takes for %llu

std::string g_out;

std::string out_path(const std::string& name) { return g_out + "/" + name; }

const char* sample_name(xisfconv_sample_format format) {
    static const char* const names[] = {"as stored", "UInt8",   "UInt16", "UInt32",
                                        "UInt64",    "Float32", "Float64"};
    return format >= 0 && format <= 6 ? names[format] : "?";
}

// A text for one line of output: at most 44 bytes of it, cut between two characters of UTF-8.
std::string shortened(const std::string& text) {
    if (text.size() <= 44) return text;
    size_t n = 44;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    return text.substr(0, n) + "...";
}

// ---------------------------------------------------------------------------------------------

// [inspect]
void inspect(const xisf::Context& ctx, const std::string& path) {
    // Opening reads the header only. The file closes itself at the end of the function.
    const xisf::File file = xisf::open(ctx, path);
    std::printf("%zu image(s) in %llu bytes, XISF %s, %s\n", xisfconv_image_count(file.get()),
                ull(xisfconv_file_size(file.get())), xisfconv_file_detail(file.get(), "version"),
                xisfconv_file_detail(file.get(), "unit"));

    auto info = xisf::defaults(xisfconv_image_info_init);
    xisf::check(ctx, xisfconv_image_info_get(file.get(), 0, &info));
    std::printf("image 0 \"%s\": %llu x %llu pixels, %llu channel(s), %s, range %g to %g\n",
                xisfconv_image_name(file.get(), 0), ull(info.width), ull(info.height), ull(info.channels),
                sample_name(info.sample_format), info.lower_bound, info.upper_bound);
    std::printf("stored at %s, compression \"%s\"\n", xisfconv_image_detail(file.get(), 0, "location"),
                xisfconv_image_detail(file.get(), 0, "compression"));

    // The FITS keywords of the image: cards of name, value and comment, in their order. The
    // list belongs to the file: there is nothing to free.
    const xisfconv_keywords* cards = nullptr;
    xisf::check(ctx, xisfconv_image_keywords(file.get(), 0, &cards));
    const size_t count = xisfconv_keywords_count(cards);
    std::printf("%zu keywords, the first of them:\n", count);
    for (size_t i = 0; i < std::min<size_t>(count, 4); ++i) {
        const char *name, *value, *comment;
        xisfconv_keywords_get(cards, i, &name, &value, &comment);
        std::printf("  %-8s= %s / %s\n", name, value, comment);
    }
    // One card by its name. The value of a card is as FITS writes it ('ic5146', with its
    // quotes); xisfconv_keywords_get_text takes the quotes off.
    const int64_t at = xisfconv_keywords_find(cards, "OBJECT");
    const char* text = "";
    if (at >= 0 && xisfconv_keywords_get_text(cards, static_cast<size_t>(at), &text) == XISFCONV_OK) {
        std::printf("the object is %s\n", text);
    } else {
        std::printf("the object is not named\n");
    }

    // XISF properties: typed values with an id, of the image and of the file. A vector or a
    // matrix has no text: xisfconv_property_read gives its elements.
    for (const size_t owner : {size_t(0), XISFCONV_FILE_PROPERTIES}) {
        const size_t n = xisfconv_property_count(file.get(), owner);
        std::printf("%zu properties of the %s:\n", n, owner == 0 ? "image" : "file");
        for (size_t i = 0; i < n; ++i) {
            const char *id, *type, *value;
            int32_t in_block = 0;
            xisf::check(ctx,
                        xisfconv_property_get(file.get(), owner, i, &id, &type, &value, nullptr, &in_block));
            const std::string shown = in_block ? "<a vector or matrix>" : shortened(value);
            std::printf("  %s (%s) = %s\n", id, type, shown.c_str());
        }
    }
}
// [/inspect]

// ---------------------------------------------------------------------------------------------

// [pixels]
struct Frame {
    std::vector<float> pixels;  // planar: channel 0 row after row, then channel 1, and so on
    uint64_t width = 0, height = 0, channels = 0;
    double low = 0, high = 1;   // the range the samples are meant to cover
};

// The first image of a file as 32-bit floating point, top row first.
Frame read_float32(const xisf::Context& ctx, const std::string& path) {
    const xisf::File file = xisf::open(ctx, path);
    auto options = xisf::defaults(xisfconv_read_options_init);
    options.sample_format = XISFCONV_SAMPLE_FLOAT32;  // whatever the file holds
    options.row_order = XISFCONV_ROWS_TOP_DOWN;

    // How large the buffer has to be, the pixels, and then what the image is like. (In that
    // order: of a FITS or ASDF image the sample format and the range are known once its pixels
    // have been read. An XISF file says them in its header.)
    uint64_t size = 0;
    xisf::check(ctx, xisfconv_pixels_size(file.get(), 0, &options, &size));
    Frame frame;
    frame.pixels.resize(static_cast<size_t>(size / sizeof(float)));
    xisf::check(ctx, xisfconv_read_pixels(file.get(), 0, &options, frame.pixels.data(), size));
    auto info = xisf::defaults(xisfconv_image_info_init);
    xisf::check(ctx, xisfconv_image_info_get(file.get(), 0, &info));

    frame.width = info.width;
    frame.height = info.height;
    frame.channels = info.channels;
    // Floating point samples come as they are stored, in the range the image states for them;
    // integers come as 0 to 1.
    if (info.sample_format == XISFCONV_SAMPLE_FLOAT32 || info.sample_format == XISFCONV_SAMPLE_FLOAT64) {
        frame.low = info.lower_bound;
        frame.high = info.upper_bound;
    }
    return frame;
}

void pixels(const xisf::Context& ctx, const std::string& path) {
    const Frame f = read_float32(ctx, path);
    const auto [low, high] = std::minmax_element(f.pixels.begin(), f.pixels.end());
    const double sum = std::accumulate(f.pixels.begin(), f.pixels.end(), 0.0);
    std::printf("%zu samples: minimum %.6f, maximum %.6f, mean %.6f\n", f.pixels.size(), *low, *high,
                sum / static_cast<double>(f.pixels.size()));
    // Channel c, row y, column x is pixels[(c * height + y) * width + x].
    std::printf("the pixel in the middle of channel 0: %.6f\n",
                f.pixels[static_cast<size_t>((f.height / 2) * f.width + f.width / 2)]);
}
// [/pixels]

// ---------------------------------------------------------------------------------------------

// [stretch]
void stretch(const xisf::Context& ctx, const std::string& path) {
    const Frame f = read_float32(ctx, path);
    const size_t colours = std::min<size_t>(static_cast<size_t>(f.channels), 3);  // a fourth is taken for alpha

    // PixInsight's automatic screen stretch: shadows, midtones and highlights for each colour
    // channel, found from the data; here with the statistics of the channels shared. The
    // functions are told the range of the samples.
    xisfconv_stretch_params params[3];
    std::vector<float> shown(f.pixels.size());
    xisf::check(ctx, xisfconv_auto_stretch(ctx.get(), f.pixels.data(), f.width, f.height, f.channels,
                                           XISFCONV_SAMPLE_FLOAT32, f.low, f.high, colours, 1, params));
    xisf::check(ctx, xisfconv_apply_stretch(ctx.get(), f.pixels.data(), f.width, f.height, f.channels,
                                            XISFCONV_SAMPLE_FLOAT32, f.low, f.high, params, colours,
                                            shown.data()));
    std::printf("shadows %.6f, midtones %.6f, highlights %.6f\n", params[0].shadows, params[0].midtones,
                params[0].highlights);

    // The stretched samples are 0 to 1: as bytes they are a picture any program shows.
    std::vector<uint8_t> bytes(shown.size());
    std::transform(shown.begin(), shown.end(), bytes.begin(),
                   [](float v) { return static_cast<uint8_t>(v * 255.0f + 0.5f); });

    auto options = xisf::defaults(xisfconv_write_options_init);
    options.overwrite = 1;
    auto picture = xisf::defaults(xisfconv_image_init);
    picture.pixels = bytes.data();
    picture.width = f.width;
    picture.height = f.height;
    picture.channels = f.channels;
    picture.sample_format = XISFCONV_SAMPLE_UINT8;
    picture.row_order = XISFCONV_ROWS_TOP_DOWN;

    // A writer collects images and writes them when it is finished; the format follows the
    // name. Left alone (an exception on the way), the handle discards it and nothing is written.
    xisfconv_writer* raw = nullptr;
    xisf::check(ctx, xisfconv_writer_new(ctx.get(), out_path("stretched.png").c_str(), &options, &raw));
    xisf::Handle<xisfconv_writer> writer(raw);
    xisf::check(ctx, xisfconv_writer_add_image(writer.get(), &picture));
    xisf::check(ctx, xisfconv_writer_finish(writer.release()));  // finish frees the writer itself
    std::printf("wrote stretched.png, %llu x %llu, 8 bits\n", ull(f.width), ull(f.height));
}
// [/stretch]

// ---------------------------------------------------------------------------------------------

// [write]
void write_crop(const xisf::Context& ctx, const std::string& path) {
    const Frame f = read_float32(ctx, path);
    // A square from the middle of the frame.
    const uint64_t side = std::min<uint64_t>({f.width, f.height, 512});
    const uint64_t left = (f.width - side) / 2, top = (f.height - side) / 2;
    std::vector<float> crop;
    crop.reserve(static_cast<size_t>(side * side * f.channels));
    for (uint64_t c = 0; c < f.channels; ++c) {
        for (uint64_t y = 0; y < side; ++y) {
            const uint64_t first = (c * f.height + top + y) * f.width + left;
            const auto row = f.pixels.begin() + static_cast<std::ptrdiff_t>(first);
            crop.insert(crop.end(), row, row + static_cast<std::ptrdiff_t>(side));
        }
    }
    const std::string number = std::to_string(side);

    // Keywords: a new list, ours to free. A part of a frame is another image. The cards that
    // tell of the instrument and of the observation hold for it as well; those that tell where
    // a pixel is (WCS keywords, BAYERPAT) would be wrong for it. So the cards that hold are
    // taken by their names, and two of our own are added.
    xisfconv_keywords* raw_cards = nullptr;
    xisf::check(ctx, xisfconv_keywords_new(ctx.get(), &raw_cards));
    const xisf::Handle<xisfconv_keywords> mine(raw_cards);
    {
        const xisf::File file = xisf::open(ctx, path);
        const xisfconv_keywords* cards = nullptr;
        xisf::check(ctx, xisfconv_image_keywords(file.get(), 0, &cards));
        for (const char* kept : {"INSTRUME", "TELESCOP", "OBJECT", "DATE-OBS", "EXPTIME"}) {
            const int64_t at = xisfconv_keywords_find(cards, kept);
            if (at < 0) continue;
            const char *name, *value, *comment;
            xisf::check(ctx, xisfconv_keywords_get(cards, static_cast<size_t>(at), &name, &value, &comment));
            xisf::check(ctx, xisfconv_keywords_append(mine.get(), name, value, comment));  // as FITS writes it
        }
    }  // the cards are copies: the file is closed here
    xisf::check(ctx, xisfconv_keywords_append_string(mine.get(), "CROPPED", "the middle of the frame",
                                                     "what this is"));
    xisf::check(ctx, xisfconv_keywords_append(mine.get(), "CROPSIZE", number.c_str(), "pixels"));

    // XISF properties: a number, a text, a date and a vector. A value that is not a vector or a
    // matrix is given as the text XISF writes for it.
    xisfconv_properties* raw_properties = nullptr;
    xisf::check(ctx, xisfconv_properties_new(ctx.get(), &raw_properties));
    const xisf::Handle<xisfconv_properties> properties(raw_properties);
    xisfconv_properties* p = properties.get();
    const double scale[2] = {0.85, 0.85};  // arcseconds per pixel, in x and in y
    const char* made = "2026-10-07T12:00:00Z";
    xisf::check(ctx, xisfconv_properties_set(p, "Tour:Side", "UInt32", number.c_str(), nullptr, nullptr));
    xisf::check(ctx,
                xisfconv_properties_set(p, "Tour:Note", "String", "cut out by tour.cpp", nullptr, nullptr));
    xisf::check(ctx, xisfconv_properties_set(p, "Tour:Made", "TimePoint", made, nullptr, nullptr));
    xisf::check(ctx, xisfconv_properties_set_array(p, "Tour:Scale", "F64Vector", scale, sizeof scale, 2, 0,
                                                   "arcseconds per pixel", nullptr));

    auto options = xisf::defaults(xisfconv_write_options_init);
    options.codec = XISFCONV_CODEC_ZLIB;  // with byte shuffling, as PixInsight compresses
    options.checksum = XISFCONV_CHECKSUM_SHA256;
    options.overwrite = 1;
    options.creator_application = "tour.cpp";

    auto image = xisf::defaults(xisfconv_image_init);
    image.pixels = crop.data();
    image.width = side;
    image.height = side;
    image.channels = f.channels;
    image.sample_format = XISFCONV_SAMPLE_FLOAT32;
    image.row_order = XISFCONV_ROWS_TOP_DOWN;
    image.use_bounds = 1;  // the range of the frame, not one guessed from this part of it
    image.lower_bound = f.low;
    image.upper_bound = f.high;
    image.name = "crop";
    image.keywords = mine.get();
    image.properties = properties.get();

    xisfconv_writer* raw = nullptr;
    xisf::check(ctx, xisfconv_writer_new(ctx.get(), out_path("crop.xisf").c_str(), &options, &raw));
    xisf::Handle<xisfconv_writer> writer(raw);
    xisf::check(ctx, xisfconv_writer_add_image(writer.get(), &image));  // copies what it is given
    xisf::check(ctx, xisfconv_writer_finish(writer.release()));        // writes the file, frees the writer
    std::printf("wrote crop.xisf: %llu x %llu pixels, compressed with zlib, SHA-256 checksum\n", ull(side),
                ull(side));
}
// [/write]

// ---------------------------------------------------------------------------------------------

// [convert]
void convert(const xisf::Context& ctx, const std::string& path) {
    // To FITS, with everything the tool does: rows turned bottom-up, keywords, properties.
    auto options = xisf::defaults(xisfconv_convert_options_init);
    options.overwrite = 1;
    xisf::check(ctx, xisfconv_convert(ctx.get(), path.c_str(), out_path("frame.fits").c_str(), &options));
    std::printf("wrote frame.fits\n");

    // A picture to look at: stretched, 8 bits, its longest side 800 pixels.
    options = xisf::defaults(xisfconv_convert_options_init);
    options.overwrite = 1;
    options.stretch = XISFCONV_STRETCH_AUTO;
    options.sample_format = XISFCONV_SAMPLE_UINT8;
    options.fit_width = options.fit_height = 800;
    xisf::check(ctx, xisfconv_convert(ctx.get(), path.c_str(), out_path("preview.png").c_str(), &options));
    std::printf("wrote preview.png\n");
}
// [/convert]

// ---------------------------------------------------------------------------------------------

// [rewrite]
void rewrite_and_verify(const xisf::Context& ctx, const std::string& path) {
    // The same file with its data blocks compressed and a checksum on each. Pixels, keywords
    // and properties are not touched: the blocks are stored another way, nothing else.
    auto options = xisf::defaults(xisfconv_rewrite_options_init);
    auto result = xisf::defaults(xisfconv_rewrite_result_init);
    options.codec = XISFCONV_CODEC_DEFAULT;  // Zstandard, or zlib in a build without it
    options.checksum = XISFCONV_CHECKSUM_SHA1;
    options.overwrite = 1;
    const std::string smaller = out_path("smaller.xisf");
    xisf::check(ctx, xisfconv_rewrite(ctx.get(), path.c_str(), smaller.c_str(), &options, &result));
    std::printf("%llu -> %llu bytes: %llu block(s) compressed, %llu kept, %llu checksum(s), read back: %s\n",
                ull(result.input_size), ull(result.output_size),
                ull(result.compressed), ull(result.kept),
                ull(result.checksums), result.read_back ? "yes" : "no");

    // Is a file stored the way these options ask? (The header tells; nothing else is read.)
    int32_t as_asked = 0;
    xisf::check(ctx, xisfconv_stored_as_requested(ctx.get(), smaller.c_str(), &options, &as_asked));
    std::printf("smaller.xisf is stored as asked: %s\n", as_asked ? "yes" : "no");

    // Verification reads everything and converts nothing. A damaged file is not an exception
    // here: it is a report that says "failed", and why.
    xisfconv_report* raw = nullptr;
    xisf::check(ctx, xisfconv_verify(ctx.get(), smaller.c_str(), &raw));
    const xisf::Handle<xisfconv_report> report(raw);
    std::printf("verdict %s: %s; %zu checksum(s) verified\n",
                xisfconv_report_verdict(report.get()) == XISFCONV_VERDICT_OK ? "OK" : "not OK",
                xisfconv_report_summary(report.get()), xisfconv_report_verified(report.get()));
    for (size_t i = 0; i < xisfconv_report_problem_count(report.get()); ++i) {
        std::printf("  problem: %s\n", xisfconv_report_problem(report.get(), i));
    }
}
// [/rewrite]

// ---------------------------------------------------------------------------------------------

// [units]
void units(const xisf::Context& ctx, const std::string& path) {
    // The kind of unit follows the name of the output: under a name that ends in .xish the
    // header goes there and every data block into the file of that name that ends in .xisb.
    auto options = xisf::defaults(xisfconv_rewrite_options_init);
    options.overwrite = 1;
    const std::string unit = out_path("unit.xish");
    xisf::check(ctx, xisfconv_rewrite(ctx.get(), path.c_str(), unit.c_str(), &options, nullptr));

    // Only the header file is ever named. It says where the data is.
    {
        const xisf::File file = xisf::open(ctx, unit);
        std::printf("unit.xish is a %s unit: header %llu bytes, %llu bytes with its data\n",
                    xisfconv_file_detail(file.get(), "unit"),
                    ull(xisfconv_file_size(file.get())),
                    ull(xisfconv_unit_size(file.get())));
        for (size_t i = 0; i < xisfconv_external_count(file.get()); ++i) {
            const std::string other = xisfconv_external_file(file.get(), i);  // an absolute path
            std::printf("  data in %s%s\n", other.substr(other.find_last_of("/\\") + 1).c_str(),
                        xisfconv_external_status(file.get(), i) == XISFCONV_OK ? "" : " (not read)");
        }
    }

    // A header is followed to files in its own directory. With the setting that follows it to
    // no other file, the pixels are refused, and the status says that this is the reason.
    xisfconv_context_set_external_files(ctx.get(), XISFCONV_EXTERNAL_NONE);
    try {
        read_float32(ctx, unit);
        std::printf("with XISFCONV_EXTERNAL_NONE: read\n");
    } catch (const xisf::Error& e) {
        std::printf("with XISFCONV_EXTERNAL_NONE: %s\n",
                    e.status == XISFCONV_ERR_NOT_ALLOWED ? "not allowed" : "failed");
    }
    xisfconv_context_set_external_files(ctx.get(), XISFCONV_EXTERNAL_HEADER_DIRECTORY);  // the default
    read_float32(ctx, unit);
    std::printf("with the default: read\n");

    // And back into one file, for PixInsight, which opens monolithic files only.
    const std::string packed = out_path("packed.xisf");
    xisf::check(ctx, xisfconv_rewrite(ctx.get(), unit.c_str(), packed.c_str(), &options, nullptr));
    std::printf("packed into packed.xisf\n");
}
// [/units]

// ---------------------------------------------------------------------------------------------

// [wcs]
void wcs(const xisf::Context& ctx, const std::string& path) {
    const xisf::File file = xisf::open(ctx, path);
    auto info = xisf::defaults(xisfconv_image_info_init);
    xisf::check(ctx, xisfconv_image_info_get(file.get(), 0, &info));
    std::printf("astrometric solution: %s\n", info.has_astrometric_solution ? "yes" : "none");

    // WCS keywords for rows counted from the bottom, as FITS has them: those of the file, or
    // made from PixInsight's solution, its distortion fitted with SIP polynomials of order 3.
    xisfconv_keywords* raw = nullptr;
    const char* summary = "";
    const xisfconv_status status =
        xisfconv_wcs_keywords(file.get(), 0, XISFCONV_ROWS_BOTTOM_UP, 3, &raw, &summary);
    if (status == XISFCONV_ERR_NOT_FOUND) {
        std::printf("this image has no solution to make WCS keywords from\n");
        return;
    }
    xisf::check(ctx, status);
    const xisf::Handle<xisfconv_keywords> cards(raw);  // this list is the caller's
    std::printf("%llu WCS keywords%s%s\n", ull(xisfconv_keywords_count(cards.get())), *summary ? "; " : "",
                summary);
    for (size_t i = 0; i < std::min<size_t>(xisfconv_keywords_count(cards.get()), 8); ++i) {
        const char *name, *value;
        xisfconv_keywords_get(cards.get(), i, &name, &value, nullptr);
        std::printf("  %-8s= %s\n", name, value);
    }
}
// [/wcs]

// ---------------------------------------------------------------------------------------------

// [progress]
struct Watcher {
    int calls = 0;
    int stop_after = 0;  // 0: never
};

void progress_and_errors(const xisf::Context& ctx, const std::string& path) {
    // What a failure looks like: an exception with the status and the text for this case.
    try {
        xisf::open(ctx, "no such file.xisf");
    } catch (const xisf::Error& e) {
        std::printf("status %d, \"%s\": %s\n", static_cast<int>(e.status), xisfconv_status_text(e.status),
                    e.what());
    }

    // Messages: the notes of a conversion to FITS and back. A handler is a plain function with
    // a pointer of the program's own; a lambda that captures nothing is one. (No exception may
    // leave a handler: it is called from C.)
    xisfconv_context_set_message_handler(
        ctx.get(),
        [](void*, xisfconv_message_level level, const char*, const char* message) {
            std::printf("  [%s] %s\n", level == XISFCONV_MESSAGE_WARNING ? "warning" : "note", message);
        },
        nullptr);
    auto there_and_back = xisf::defaults(xisfconv_convert_options_init);
    there_and_back.overwrite = 1;
    const std::string there = out_path("there.fits"), back = out_path("back.xisf");
    xisfconv_status converted = xisfconv_convert(ctx.get(), path.c_str(), there.c_str(), &there_and_back);
    if (converted == XISFCONV_OK) {
        converted = xisfconv_convert(ctx.get(), there.c_str(), back.c_str(), &there_and_back);
    }
    xisfconv_context_set_message_handler(ctx.get(), nullptr, nullptr);
    xisf::check(ctx, converted);

    // Progress, and a call that is stopped by its handler: an answer other than 0 stops the
    // call, which then returns XISFCONV_ERR_CANCELLED and leaves no partly written file.
    Watcher watcher;
    watcher.stop_after = 2;
    xisfconv_context_set_progress_handler(
        ctx.get(),
        [](void* user, const char* stage, uint64_t done, uint64_t total) -> int32_t {
            auto& w = *static_cast<Watcher*>(user);
            if (++w.calls <= 3) {
                std::printf("  %s %llu of %llu\n", stage, ull(done),
                            ull(total));
            }
            return w.stop_after && w.calls >= w.stop_after;
        },
        &watcher);
    auto options = xisf::defaults(xisfconv_rewrite_options_init);
    options.codec = XISFCONV_CODEC_ZLIB;
    options.overwrite = 1;
    const xisfconv_status status =
        xisfconv_rewrite(ctx.get(), path.c_str(), out_path("stopped.xisf").c_str(), &options, nullptr);
    xisfconv_context_set_progress_handler(ctx.get(), nullptr, nullptr);
    const bool left = std::ifstream(out_path("stopped.xisf")).good();
    std::printf("stopped: %s; a file is left: %s\n", xisfconv_status_text(status), left ? "yes" : "no");
    if (status != XISFCONV_ERR_CANCELLED) throw xisf::Error(status, "the rewrite was not stopped");
}
// [/progress]

}  // namespace

// ---------------------------------------------------------------------------------------------

// [main]
int main(int argc, char** argv) {
    static const std::pair<const char*, void (*)(const xisf::Context&, const std::string&)> chapters[] = {
        {"inspect", inspect},  {"pixels", pixels},   {"stretch", stretch},
        {"write", write_crop}, {"convert", convert}, {"rewrite", rewrite_and_verify},
        {"units", units},      {"wcs", wcs},         {"progress", progress_and_errors}};
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <image.xisf> <output directory> [chapter]\n", argv[0]);
        return 2;
    }
    g_out = argv[2];

    // A context holds the error text and the handlers. It belongs to one thread at a time; a
    // program with several threads gives each its own.
    const xisf::Context ctx(xisfconv_context_new());
    if (!ctx) return 1;

    // The chapters ask about what an XISF file has. (first.cpp takes a FITS or ASDF file too.)
    xisfconv_format format = XISFCONV_FORMAT_AUTO;
    if (xisfconv_detect_format(ctx.get(), argv[1], &format) != XISFCONV_OK) {
        std::fprintf(stderr, "%s: %s\n", argv[1], xisfconv_error_message(ctx.get()));
        return 1;
    }
    if (format != XISFCONV_FORMAT_XISF) {
        std::fprintf(stderr, "%s is not an XISF file\n", argv[1]);
        return 2;
    }
    int bad = 0, ran = 0;
    for (const auto& chapter : chapters) {
        if (argc > 3 && std::strcmp(argv[3], chapter.first) != 0) continue;
        std::printf("== %s\n", chapter.first);
        ++ran;
        try {
            chapter.second(ctx, argv[1]);
        } catch (const std::exception& e) {  // xisf::Error, or no memory for the pixels
            std::fprintf(stderr, "%s: %s\n", chapter.first, e.what());
            ++bad;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "%s: there is no chapter \"%s\"\n", argv[0], argv[3]);
        return 2;
    }
    return bad ? 1 : 0;
}
// [/main]
