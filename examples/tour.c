/* A tour of libxisfconv in C: every chapter of the manual (docs/manual.html) as one function.
 *
 *   cc tour.c $(pkg-config --cflags --libs xisfconv) -o tour
 *   ./tour integrated_light.xisf out/          (the directory must exist; files in it are replaced)
 *
 * The lines between a pair of marks like [inspect] and [/inspect] are what the manual shows.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 Jurgen Kobierczynski
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xisfconv.h"

/* [errors] */
/* Every call that can fail returns a status; the text of the failure is kept in the context. */
static int failed(xisfconv_context *ctx, const char *what) {
    fprintf(stderr, "%s: %s\n", what, xisfconv_error_message(ctx));
    return 1;
}

/* Warnings and notes of the library come here, if the program asks for them: the library itself
   prints nothing. */
static void on_message(void *user, xisfconv_message_level level, const char *path,
                       const char *message) {
    (void)user;
    (void)path;
    printf("  [%s] %s\n", level == XISFCONV_MESSAGE_WARNING ? "warning" : "note", message);
}

/* What the library does not know of: this program's own malloc gave nothing. */
static int no_memory(void) {
    fprintf(stderr, "out of memory\n");
    return 1;
}
/* [/errors] */

static char g_out[900];

/* A path in the output directory (good until the fourth call after it). */
static const char *out_path(const char *name) {
    static char paths[4][1024];
    static int next = 0;
    char *p = paths[next++ % 4];
    sprintf(p, "%s/%s", g_out, name);
    return p;
}

static const char *sample_name(xisfconv_sample_format format) {
    static const char *const names[] = {"as stored", "UInt8", "UInt16", "UInt32", "UInt64",
                                        "Float32", "Float64"};
    return format >= 0 && format <= 6 ? names[format] : "?";
}

/* A text for one line of output: at most 44 bytes of it, cut between two characters of UTF-8. */
static const char *shortened(const char *text) {
    static char line[52];
    size_t n = strlen(text);
    if (n <= 44) return text;
    for (n = 44; n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80; --n) {}
    memcpy(line, text, n);
    strcpy(line + n, "...");
    return line;
}

/* ------------------------------------------------------------------------------------------ */

/* [inspect] */
static int inspect(xisfconv_context *ctx, const char *path) {
    xisfconv_file *file = NULL;
    xisfconv_image_info info;
    const xisfconv_keywords *cards = NULL;
    const char *name, *value, *comment, *text, *type;
    int32_t in_block;
    int64_t at;
    size_t i, n, owner;

    /* Opening reads the header only. */
    if (xisfconv_open(ctx, path, &file) != XISFCONV_OK) return failed(ctx, "open");
    printf("%lu image(s) in %llu bytes, XISF %s, %s\n", (unsigned long)xisfconv_image_count(file),
           (unsigned long long)xisfconv_file_size(file), xisfconv_file_detail(file, "version"),
           xisfconv_file_detail(file, "unit"));

    /* A struct is initialised with the size the program was compiled with: that is how a
       newer library knows an older caller. */
    xisfconv_image_info_init(&info, sizeof info);
    if (xisfconv_image_info_get(file, 0, &info) != XISFCONV_OK) {
        xisfconv_close(file);
        return failed(ctx, "image");
    }
    printf("image 0 \"%s\": %llu x %llu pixels, %llu channel(s), %s, range %g to %g\n",
           xisfconv_image_name(file, 0), (unsigned long long)info.width,
           (unsigned long long)info.height, (unsigned long long)info.channels,
           sample_name(info.sample_format), info.lower_bound, info.upper_bound);
    printf("stored at %s, compression \"%s\"\n", xisfconv_image_detail(file, 0, "location"),
           xisfconv_image_detail(file, 0, "compression"));

    /* The FITS keywords of the image: cards of name, value and comment, in their order. The
       list belongs to the file: there is nothing to free. */
    if (xisfconv_image_keywords(file, 0, &cards) != XISFCONV_OK) {
        xisfconv_close(file);
        return failed(ctx, "keywords");
    }
    n = xisfconv_keywords_count(cards);
    printf("%lu keywords, the first of them:\n", (unsigned long)n);
    for (i = 0; i < n && i < 4; ++i) {
        xisfconv_keywords_get(cards, i, &name, &value, &comment);
        printf("  %-8s= %s / %s\n", name, value, comment);
    }
    /* One card by its name. The value of a card is as FITS writes it ('ic5146', with its
       quotes); xisfconv_keywords_get_text takes the quotes off. */
    at = xisfconv_keywords_find(cards, "OBJECT");
    if (at >= 0 && xisfconv_keywords_get_text(cards, (size_t)at, &text) == XISFCONV_OK) {
        printf("the object is %s\n", text);
    } else {
        printf("the object is not named\n");
    }

    /* XISF properties: typed values with an id, of the image and of the file. A vector or a
       matrix has no text: xisfconv_property_read gives its elements. */
    for (owner = 0; owner < 2; ++owner) {
        const size_t whose = owner == 0 ? 0 : XISFCONV_FILE_PROPERTIES;
        n = xisfconv_property_count(file, whose);
        printf("%lu properties of the %s:\n", (unsigned long)n, owner == 0 ? "image" : "file");
        for (i = 0; i < n; ++i) {
            if (xisfconv_property_get(file, whose, i, &name, &type, &value, NULL, &in_block) != XISFCONV_OK) {
                continue;
            }
            printf("  %s (%s) = %s\n", name, type, in_block ? "<a vector or matrix>" : shortened(value));
        }
    }

    xisfconv_close(file);
    return 0;
}
/* [/inspect] */

/* ------------------------------------------------------------------------------------------ */

/* [pixels] */
struct frame {
    float *pixels;       /* planar: channel 0 row after row, then channel 1, and so on */
    uint64_t width, height, channels;
    double low, high;    /* the range the samples are meant to cover */
};

/* The first image of a file as 32-bit floating point, top row first. The caller frees
   frame->pixels, which is NULL after a failure. XISFCONV_ERR_MEMORY is also what it returns
   when this program's own malloc gives nothing. */
static xisfconv_status read_float32(xisfconv_context *ctx, const char *path, struct frame *frame) {
    xisfconv_file *file = NULL;
    xisfconv_image_info info;
    xisfconv_read_options options;
    xisfconv_status status;
    uint64_t size = 0;

    memset(frame, 0, sizeof *frame);
    status = xisfconv_open(ctx, path, &file);
    if (status != XISFCONV_OK) return status;
    xisfconv_image_info_init(&info, sizeof info);
    xisfconv_read_options_init(&options, sizeof options);
    options.sample_format = XISFCONV_SAMPLE_FLOAT32; /* whatever the file holds */
    options.row_order = XISFCONV_ROWS_TOP_DOWN;

    /* How large the buffer has to be, the pixels, and then what the image is like. (In that
       order: of a FITS or ASDF image the sample format and the range are known once its pixels
       have been read. An XISF file says them in its header.) */
    status = xisfconv_pixels_size(file, 0, &options, &size);
    if (status == XISFCONV_OK && size != (uint64_t)(size_t)size) status = XISFCONV_ERR_MEMORY;
    if (status == XISFCONV_OK) {
        frame->pixels = (float *)malloc((size_t)size);
        status = frame->pixels ? xisfconv_read_pixels(file, 0, &options, frame->pixels, size)
                               : XISFCONV_ERR_MEMORY;
    }
    if (status == XISFCONV_OK) status = xisfconv_image_info_get(file, 0, &info);
    if (status != XISFCONV_OK) {
        free(frame->pixels);
        frame->pixels = NULL;
    }
    frame->width = info.width;
    frame->height = info.height;
    frame->channels = info.channels;
    /* Floating point samples come as they are stored, in the range the image states for
       them; integers come as 0 to 1. */
    if (info.sample_format == XISFCONV_SAMPLE_FLOAT32 || info.sample_format == XISFCONV_SAMPLE_FLOAT64) {
        frame->low = info.lower_bound;
        frame->high = info.upper_bound;
    } else {
        frame->low = 0;
        frame->high = 1;
    }
    xisfconv_close(file); /* the pixels are the caller's own copy */
    return status;
}

/* What a caller of read_float32 says when it failed. */
static int read_failed(xisfconv_context *ctx, xisfconv_status status) {
    return status == XISFCONV_ERR_MEMORY ? no_memory() : failed(ctx, "read");
}

static int pixels(xisfconv_context *ctx, const char *path) {
    struct frame f;
    uint64_t i, count;
    double sum = 0;
    float low, high;
    const xisfconv_status status = read_float32(ctx, path, &f);

    if (status != XISFCONV_OK) return read_failed(ctx, status);
    count = f.width * f.height * f.channels;
    low = high = f.pixels[0];
    for (i = 0; i < count; ++i) {
        if (f.pixels[i] < low) low = f.pixels[i];
        if (f.pixels[i] > high) high = f.pixels[i];
        sum += f.pixels[i];
    }
    printf("%llu samples: minimum %.6f, maximum %.6f, mean %.6f\n", (unsigned long long)count,
           low, high, sum / (double)count);
    /* Channel c, row y, column x is pixels[(c * height + y) * width + x]. */
    printf("the pixel in the middle of channel 0: %.6f\n",
           f.pixels[(f.height / 2) * f.width + f.width / 2]);
    free(f.pixels);
    return 0;
}
/* [/pixels] */

/* ------------------------------------------------------------------------------------------ */

/* [stretch] */
static int stretch(xisfconv_context *ctx, const char *path) {
    struct frame f;
    uint64_t i, count;
    size_t colours;
    xisfconv_stretch_params params[3];
    xisfconv_write_options options;
    xisfconv_writer *writer = NULL;
    xisfconv_image picture;
    xisfconv_status status;
    float *shown;
    uint8_t *bytes;
    int ok = 0;

    status = read_float32(ctx, path, &f);
    if (status != XISFCONV_OK) return read_failed(ctx, status);
    count = f.width * f.height * f.channels;
    colours = f.channels < 3 ? (size_t)f.channels : 3; /* a fourth channel is taken for alpha */
    shown = (float *)malloc((size_t)count * sizeof(float));
    bytes = (uint8_t *)malloc((size_t)count);
    if (!shown || !bytes) {
        free(shown);
        free(bytes);
        free(f.pixels);
        return no_memory();
    }

    /* PixInsight's automatic screen stretch: shadows, midtones and highlights for each colour
       channel, found from the data; here with the statistics of the channels shared. The
       functions are told the range of the samples. */
    if (xisfconv_auto_stretch(ctx, f.pixels, f.width, f.height, f.channels, XISFCONV_SAMPLE_FLOAT32,
                              f.low, f.high, colours, 1, params) != XISFCONV_OK ||
        xisfconv_apply_stretch(ctx, f.pixels, f.width, f.height, f.channels, XISFCONV_SAMPLE_FLOAT32,
                               f.low, f.high, params, colours, shown) != XISFCONV_OK) {
        free(shown);
        free(bytes);
        free(f.pixels);
        return failed(ctx, "stretch");
    }
    printf("shadows %.6f, midtones %.6f, highlights %.6f\n", params[0].shadows,
           params[0].midtones, params[0].highlights);

    /* The stretched samples are 0 to 1: as bytes they are a picture any program shows. */
    for (i = 0; i < count; ++i) bytes[i] = (uint8_t)(shown[i] * 255.0f + 0.5f);

    /* A writer collects images and writes them when it is finished. The format follows the
       name of the file. */
    xisfconv_write_options_init(&options, sizeof options);
    options.overwrite = 1;
    xisfconv_image_init(&picture, sizeof picture);
    picture.pixels = bytes;
    picture.width = f.width;
    picture.height = f.height;
    picture.channels = f.channels;
    picture.sample_format = XISFCONV_SAMPLE_UINT8;
    picture.row_order = XISFCONV_ROWS_TOP_DOWN;
    if (xisfconv_writer_new(ctx, out_path("stretched.png"), &options, &writer) == XISFCONV_OK) {
        if (xisfconv_writer_add_image(writer, &picture) != XISFCONV_OK) {
            xisfconv_writer_discard(writer);
        } else {
            ok = xisfconv_writer_finish(writer) == XISFCONV_OK; /* frees the writer */
        }
    }
    free(bytes);
    free(shown);
    free(f.pixels);
    if (!ok) return failed(ctx, "write");
    printf("wrote stretched.png, %llu x %llu, 8 bits\n", (unsigned long long)f.width,
           (unsigned long long)f.height);
    return 0;
}
/* [/stretch] */

/* ------------------------------------------------------------------------------------------ */

/* [write] */
/* The keywords of the crop: a new list, which the caller frees. A part of a frame is another
   image. The cards that tell of the instrument and of the observation hold for it as well;
   those that tell where a pixel is (WCS keywords, BAYERPAT) would be wrong for it. So the cards
   that hold are taken by their names, and two of our own are added. */
static xisfconv_status crop_keywords(xisfconv_context *ctx, const char *path, const char *side,
                                     xisfconv_keywords **out) {
    static const char *const kept[] = {"INSTRUME", "TELESCOP", "OBJECT", "DATE-OBS", "EXPTIME"};
    xisfconv_file *file = NULL;
    const xisfconv_keywords *cards = NULL;
    const char *name, *value, *comment;
    xisfconv_status status;
    int64_t at;
    size_t i;

    status = xisfconv_keywords_new(ctx, out);
    if (status != XISFCONV_OK) return status;
    status = xisfconv_open(ctx, path, &file);
    if (status == XISFCONV_OK) status = xisfconv_image_keywords(file, 0, &cards);
    for (i = 0; status == XISFCONV_OK && i < sizeof kept / sizeof kept[0]; ++i) {
        at = xisfconv_keywords_find(cards, kept[i]);
        if (at < 0) continue;
        xisfconv_keywords_get(cards, (size_t)at, &name, &value, &comment);
        status = xisfconv_keywords_append(*out, name, value, comment); /* the value as FITS writes it */
    }
    xisfconv_close(file); /* the cards are copies: the file can go */
    if (status == XISFCONV_OK) {
        status = xisfconv_keywords_append_string(*out, "CROPPED", "the middle of the frame", "what this is");
    }
    if (status == XISFCONV_OK) status = xisfconv_keywords_append(*out, "CROPSIZE", side, "pixels");
    return status;
}

/* XISF properties: a number, a text, a date and a vector. A value that is not a vector or a
   matrix is given as the text XISF writes for it. */
static xisfconv_status crop_properties(xisfconv_context *ctx, const char *side, xisfconv_properties **out) {
    const double scale[2] = {0.85, 0.85}; /* arcseconds per pixel, in x and in y */
    xisfconv_status status = xisfconv_properties_new(ctx, out);

    if (status == XISFCONV_OK) status = xisfconv_properties_set(*out, "Tour:Side", "UInt32", side, NULL, NULL);
    if (status == XISFCONV_OK) {
        status = xisfconv_properties_set(*out, "Tour:Note", "String", "cut out by tour.c", NULL, NULL);
    }
    if (status == XISFCONV_OK) {
        status = xisfconv_properties_set(*out, "Tour:Made", "TimePoint", "2026-10-07T12:00:00Z", NULL, NULL);
    }
    if (status == XISFCONV_OK) {
        status = xisfconv_properties_set_array(*out, "Tour:Scale", "F64Vector", scale, sizeof scale, 2, 0,
                                               "arcseconds per pixel", NULL);
    }
    return status;
}

static int write_crop(xisfconv_context *ctx, const char *path) {
    struct frame f;
    uint64_t side, left, top, x, y, c;
    xisfconv_keywords *cards = NULL;
    xisfconv_properties *properties = NULL;
    xisfconv_write_options options;
    xisfconv_writer *writer = NULL;
    xisfconv_image image;
    xisfconv_status status;
    char number[32];
    float *crop;

    status = read_float32(ctx, path, &f);
    if (status != XISFCONV_OK) return read_failed(ctx, status);
    /* A square from the middle of the frame. */
    side = f.width < f.height ? f.width : f.height;
    if (side > 512) side = 512;
    left = (f.width - side) / 2;
    top = (f.height - side) / 2;
    crop = (float *)malloc((size_t)(side * side * f.channels) * sizeof(float));
    if (!crop) {
        free(f.pixels);
        return no_memory();
    }
    for (c = 0; c < f.channels; ++c)
        for (y = 0; y < side; ++y)
            for (x = 0; x < side; ++x)
                crop[(c * side + y) * side + x] = f.pixels[(c * f.height + top + y) * f.width + left + x];
    free(f.pixels);

    sprintf(number, "%lu", (unsigned long)side);
    status = crop_keywords(ctx, path, number, &cards);
    if (status == XISFCONV_OK) status = crop_properties(ctx, number, &properties);

    xisfconv_write_options_init(&options, sizeof options);
    options.codec = XISFCONV_CODEC_ZLIB; /* with byte shuffling, as PixInsight compresses */
    options.checksum = XISFCONV_CHECKSUM_SHA256;
    options.overwrite = 1;
    options.creator_application = "tour.c";

    xisfconv_image_init(&image, sizeof image);
    image.pixels = crop;
    image.width = side;
    image.height = side;
    image.channels = f.channels;
    image.sample_format = XISFCONV_SAMPLE_FLOAT32;
    image.row_order = XISFCONV_ROWS_TOP_DOWN;
    image.use_bounds = 1; /* the range of the frame, not one guessed from this part of it */
    image.lower_bound = f.low;
    image.upper_bound = f.high;
    image.name = "crop";
    image.keywords = cards;
    image.properties = properties;

    if (status == XISFCONV_OK) status = xisfconv_writer_new(ctx, out_path("crop.xisf"), &options, &writer);
    if (status == XISFCONV_OK) status = xisfconv_writer_add_image(writer, &image);
    if (status == XISFCONV_OK) {
        status = xisfconv_writer_finish(writer); /* writes the file and frees the writer */
    } else {
        xisfconv_writer_discard(writer);
    }
    /* The writer copied what it was given when the image was added: all of it can go. */
    xisfconv_properties_free(properties);
    xisfconv_keywords_free(cards);
    free(crop);
    if (status != XISFCONV_OK) return failed(ctx, "write");
    printf("wrote crop.xisf: %llu x %llu pixels, compressed with zlib, SHA-256 checksum\n",
           (unsigned long long)side, (unsigned long long)side);
    return 0;
}
/* [/write] */

/* ------------------------------------------------------------------------------------------ */

/* [convert] */
static int convert(xisfconv_context *ctx, const char *path) {
    xisfconv_convert_options options;

    /* To FITS, with everything the tool does: rows turned bottom-up, keywords, properties. */
    xisfconv_convert_options_init(&options, sizeof options);
    options.overwrite = 1;
    if (xisfconv_convert(ctx, path, out_path("frame.fits"), &options) != XISFCONV_OK) {
        return failed(ctx, "to FITS");
    }
    printf("wrote frame.fits\n");

    /* A picture to look at: stretched, 8 bits, its longest side 800 pixels. */
    xisfconv_convert_options_init(&options, sizeof options);
    options.overwrite = 1;
    options.stretch = XISFCONV_STRETCH_AUTO;
    options.sample_format = XISFCONV_SAMPLE_UINT8;
    options.fit_width = options.fit_height = 800;
    if (xisfconv_convert(ctx, path, out_path("preview.png"), &options) != XISFCONV_OK) {
        return failed(ctx, "to PNG");
    }
    printf("wrote preview.png\n");
    return 0;
}
/* [/convert] */

/* ------------------------------------------------------------------------------------------ */

/* [rewrite] */
static int rewrite_and_verify(xisfconv_context *ctx, const char *path) {
    xisfconv_rewrite_options options;
    xisfconv_rewrite_result result;
    xisfconv_report *report = NULL;
    int32_t as_asked = 0;
    size_t i;

    /* The same file with its data blocks compressed and a checksum on each. Pixels, keywords
       and properties are not touched: the blocks are stored another way, nothing else. */
    xisfconv_rewrite_options_init(&options, sizeof options);
    xisfconv_rewrite_result_init(&result, sizeof result);
    options.codec = XISFCONV_CODEC_DEFAULT; /* Zstandard, or zlib in a build without it */
    options.checksum = XISFCONV_CHECKSUM_SHA1;
    options.overwrite = 1;
    if (xisfconv_rewrite(ctx, path, out_path("smaller.xisf"), &options, &result) != XISFCONV_OK) {
        return failed(ctx, "rewrite");
    }
    printf("%llu -> %llu bytes: %llu block(s) compressed, %llu kept, %llu checksum(s), "
           "read back: %s\n",
           (unsigned long long)result.input_size, (unsigned long long)result.output_size,
           (unsigned long long)result.compressed, (unsigned long long)result.kept,
           (unsigned long long)result.checksums, result.read_back ? "yes" : "no");

    /* Is a file stored the way these options ask? (The header tells; nothing else is read.) */
    if (xisfconv_stored_as_requested(ctx, out_path("smaller.xisf"), &options, &as_asked) != XISFCONV_OK) {
        return failed(ctx, "stored as requested");
    }
    printf("smaller.xisf is stored as asked: %s\n", as_asked ? "yes" : "no");

    /* Verification reads everything and converts nothing. A damaged file is not an error of
       the call: it is a report that says "failed", and why. */
    if (xisfconv_verify(ctx, out_path("smaller.xisf"), &report) != XISFCONV_OK) {
        return failed(ctx, "verify");
    }
    printf("verdict %s: %s; %lu checksum(s) verified\n",
           xisfconv_report_verdict(report) == XISFCONV_VERDICT_OK ? "OK" : "not OK",
           xisfconv_report_summary(report), (unsigned long)xisfconv_report_verified(report));
    for (i = 0; i < xisfconv_report_problem_count(report); ++i) {
        printf("  problem: %s\n", xisfconv_report_problem(report, i));
    }
    xisfconv_report_free(report);
    return 0;
}
/* [/rewrite] */

/* ------------------------------------------------------------------------------------------ */

/* [units] */
static int units(xisfconv_context *ctx, const char *path) {
    xisfconv_rewrite_options options;
    xisfconv_file *file = NULL;
    xisfconv_status status;
    struct frame f;
    size_t i;

    /* The kind of unit follows the name of the output: under a name that ends in .xish the
       header goes there and every data block into the file of that name that ends in .xisb. */
    xisfconv_rewrite_options_init(&options, sizeof options);
    options.overwrite = 1;
    if (xisfconv_rewrite(ctx, path, out_path("unit.xish"), &options, NULL) != XISFCONV_OK) {
        return failed(ctx, "unpack");
    }

    /* Only the header file is ever named. It says where the data is. */
    if (xisfconv_open(ctx, out_path("unit.xish"), &file) != XISFCONV_OK) return failed(ctx, "open");
    printf("unit.xish is a %s unit: header %llu bytes, %llu bytes with its data\n",
           xisfconv_file_detail(file, "unit"), (unsigned long long)xisfconv_file_size(file),
           (unsigned long long)xisfconv_unit_size(file));
    for (i = 0; i < xisfconv_external_count(file); ++i) {
        const char *other = xisfconv_external_file(file, i); /* an absolute path */
        const char *slash = strrchr(other, '/');
        const char *back = strrchr(other, '\\');
        if (back && (!slash || back > slash)) slash = back;
        printf("  data in %s%s\n", slash ? slash + 1 : other,
               xisfconv_external_status(file, i) == XISFCONV_OK ? "" : " (not read)");
    }
    xisfconv_close(file);

    /* A header is followed to files in its own directory. With the setting that follows it to
       no other file, the pixels are refused, and the status says that this is the reason. */
    xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_NONE);
    status = read_float32(ctx, out_path("unit.xish"), &f);
    printf("with XISFCONV_EXTERNAL_NONE: %s\n",
           status == XISFCONV_ERR_NOT_ALLOWED ? "not allowed" : status == XISFCONV_OK ? "read" : "failed");
    free(f.pixels);
    xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_HEADER_DIRECTORY); /* the default */
    status = read_float32(ctx, out_path("unit.xish"), &f);
    free(f.pixels);
    if (status != XISFCONV_OK) return read_failed(ctx, status);
    printf("with the default: read\n");

    /* And back into one file, for PixInsight, which opens monolithic files only. */
    if (xisfconv_rewrite(ctx, out_path("unit.xish"), out_path("packed.xisf"), &options, NULL) !=
        XISFCONV_OK) {
        return failed(ctx, "pack");
    }
    printf("packed into packed.xisf\n");
    return 0;
}
/* [/units] */

/* ------------------------------------------------------------------------------------------ */

/* [wcs] */
static int wcs(xisfconv_context *ctx, const char *path) {
    xisfconv_file *file = NULL;
    xisfconv_image_info info;
    xisfconv_keywords *cards = NULL;
    const char *summary = "", *name, *value;
    xisfconv_status status;
    size_t i;

    if (xisfconv_open(ctx, path, &file) != XISFCONV_OK) return failed(ctx, "open");
    xisfconv_image_info_init(&info, sizeof info);
    xisfconv_image_info_get(file, 0, &info);
    printf("astrometric solution: %s\n", info.has_astrometric_solution ? "yes" : "none");

    /* WCS keywords for rows counted from the bottom, as FITS has them: those of the file, or
       made from PixInsight's solution, its distortion fitted with SIP polynomials of order 3. */
    status = xisfconv_wcs_keywords(file, 0, XISFCONV_ROWS_BOTTOM_UP, 3, &cards, &summary);
    if (status == XISFCONV_ERR_NOT_FOUND) {
        printf("this image has no solution to make WCS keywords from\n");
    } else if (status != XISFCONV_OK) {
        xisfconv_close(file);
        return failed(ctx, "wcs");
    } else {
        printf("%lu WCS keywords", (unsigned long)xisfconv_keywords_count(cards));
        printf("%s%s\n", *summary ? "; " : "", summary);
        for (i = 0; i < xisfconv_keywords_count(cards) && i < 8; ++i) {
            xisfconv_keywords_get(cards, i, &name, &value, NULL);
            printf("  %-8s= %s\n", name, value);
        }
        xisfconv_keywords_free(cards); /* this list is the caller's */
    }
    xisfconv_close(file);
    return 0;
}
/* [/wcs] */

/* ------------------------------------------------------------------------------------------ */

/* [progress] */
struct watcher {
    int calls;
    int stop_after; /* 0: never */
};

/* Called between the steps of long work. An answer other than 0 stops the call, which then
   returns XISFCONV_ERR_CANCELLED and leaves no partly written file. */
static int32_t on_progress(void *user, const char *stage, uint64_t done, uint64_t total) {
    struct watcher *w = (struct watcher *)user;
    ++w->calls;
    if (w->calls <= 3) {
        printf("  %s %llu of %llu\n", stage, (unsigned long long)done, (unsigned long long)total);
    }
    return w->stop_after && w->calls >= w->stop_after;
}

static int progress_and_errors(xisfconv_context *ctx, const char *path) {
    struct watcher w = {0, 0};
    xisfconv_convert_options convert_options;
    xisfconv_rewrite_options options;
    xisfconv_file *file = NULL;
    xisfconv_status status;
    FILE *left;

    /* What a failure looks like: a status, its fixed name, and the text for this case. */
    status = xisfconv_open(ctx, "no such file.xisf", &file);
    printf("status %d, \"%s\": %s\n", (int)status, xisfconv_status_text(status),
           xisfconv_error_message(ctx));

    /* Messages: the notes of a conversion to FITS and back, through the handler of the context. */
    xisfconv_convert_options_init(&convert_options, sizeof convert_options);
    convert_options.overwrite = 1;
    xisfconv_context_set_message_handler(ctx, on_message, NULL);
    status = xisfconv_convert(ctx, path, out_path("there.fits"), &convert_options);
    if (status == XISFCONV_OK) {
        status = xisfconv_convert(ctx, out_path("there.fits"), out_path("back.xisf"), &convert_options);
    }
    xisfconv_context_set_message_handler(ctx, NULL, NULL);
    if (status != XISFCONV_OK) return failed(ctx, "there and back");

    /* Progress, and a call that is stopped by its handler. */
    xisfconv_rewrite_options_init(&options, sizeof options);
    options.codec = XISFCONV_CODEC_ZLIB;
    options.overwrite = 1;
    w.stop_after = 2;
    xisfconv_context_set_progress_handler(ctx, on_progress, &w);
    status = xisfconv_rewrite(ctx, path, out_path("stopped.xisf"), &options, NULL);
    xisfconv_context_set_progress_handler(ctx, NULL, NULL);
    left = fopen(out_path("stopped.xisf"), "rb");
    printf("stopped: %s; a file is left: %s\n", xisfconv_status_text(status), left ? "yes" : "no");
    if (left) fclose(left);
    return status == XISFCONV_ERR_CANCELLED ? 0 : 1;
}
/* [/progress] */

/* ------------------------------------------------------------------------------------------ */

/* [main] */
int main(int argc, char **argv) {
    static const struct {
        const char *name;
        int (*run)(xisfconv_context *, const char *);
    } chapters[] = {{"inspect", inspect},  {"pixels", pixels},   {"stretch", stretch},
                    {"write", write_crop}, {"convert", convert}, {"rewrite", rewrite_and_verify},
                    {"units", units},      {"wcs", wcs},         {"progress", progress_and_errors}};
    xisfconv_context *ctx;
    xisfconv_format format = XISFCONV_FORMAT_AUTO;
    size_t i;
    int bad = 0, ran = 0;

    if (argc < 3 || strlen(argv[2]) > 800) {
        fprintf(stderr, "usage: %s <image.xisf> <output directory> [chapter]\n", argv[0]);
        return 2;
    }
    strcpy(g_out, argv[2]);

    /* A context holds the error text and the handlers. It belongs to one thread at a time;
       a program with several threads gives each its own. */
    ctx = xisfconv_context_new();
    if (!ctx) return no_memory();

    /* The chapters ask about what an XISF file has. (first.c takes a FITS or ASDF file too.) */
    if (xisfconv_detect_format(ctx, argv[1], &format) != XISFCONV_OK) {
        failed(ctx, argv[1]);
        xisfconv_context_free(ctx);
        return 1;
    }
    if (format != XISFCONV_FORMAT_XISF) {
        fprintf(stderr, "%s is not an XISF file\n", argv[1]);
        xisfconv_context_free(ctx);
        return 2;
    }
    for (i = 0; i < sizeof chapters / sizeof chapters[0]; ++i) {
        if (argc > 3 && strcmp(argv[3], chapters[i].name) != 0) continue;
        printf("== %s\n", chapters[i].name);
        bad += chapters[i].run(ctx, argv[1]);
        ++ran;
    }
    xisfconv_context_free(ctx);
    if (!ran) {
        fprintf(stderr, "%s: there is no chapter \"%s\"\n", argv[0], argv[3]);
        return 2;
    }
    return bad ? 1 : 0;
}
/* [/main] */
