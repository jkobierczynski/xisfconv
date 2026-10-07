/* Tests of libxisfconv's C API, in plain C99: what a C program sees of the library.
 *
 * Usage: xisfconv_capi_test <empty directory for test files> [--quiet]
 *
 * The files it needs are written by the library itself and read back; error paths, buffer
 * sizes, handle lifetimes and the callbacks are exercised. With --quiet nothing is printed
 * unless a check fails, so that a caller can also assert that the library itself is silent.
 * The exit status is 0 if every check passed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Jurgen Kobierczynski
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xisfconv.h"

static int g_checks = 0, g_failed = 0;

#define CHECK(cond, what)                                              \
    do {                                                               \
        ++g_checks;                                                    \
        if (!(cond)) {                                                 \
            ++g_failed;                                                \
            fprintf(stderr, "FAIL (line %d): %s\n", __LINE__, (what)); \
        }                                                              \
    } while (0)

static char g_dir[1024];

static const char *path_of(const char *name) {
    static char buffers[8][1200];
    static int next = 0;
    char *b = buffers[next++ % 8];
    sprintf(b, "%s/%s", g_dir, name);
    return b;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    long n = -1;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    fclose(f);
    return n;
}

/* Copies a file and inverts one byte, `from_end` bytes before its end. */
static int copy_damaged(const char *from, const char *to, long from_end) {
    FILE *in = fopen(from, "rb"), *out;
    long n, i;
    int c;
    if (!in) return 0;
    out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    fseek(in, 0, SEEK_END);
    n = ftell(in);
    fseek(in, 0, SEEK_SET);
    for (i = 0; i < n && (c = fgetc(in)) != EOF; ++i) fputc(i == n - from_end ? c ^ 0xFF : c, out);
    fclose(in);
    fclose(out);
    return 1;
}

/* ---- callbacks ---- */

typedef struct {
    int warnings, infos, with_path;
    char last[512];
} messages;

static void on_message(void *user, xisfconv_message_level level, const char *path, const char *message) {
    messages *m = (messages *)user;
    if (level == XISFCONV_MESSAGE_WARNING) ++m->warnings;
    else ++m->infos;
    if (path && *path) ++m->with_path;
    strncpy(m->last, message, sizeof m->last - 1);
    m->last[sizeof m->last - 1] = 0;
}

typedef struct {
    int calls, cancel_at;
} progress;

static int32_t on_progress(void *user, const char *stage, uint64_t done, uint64_t total) {
    progress *p = (progress *)user;
    (void)done;
    (void)total;
    if (!stage || !*stage) return 1;
    ++p->calls;
    return p->cancel_at && p->calls >= p->cancel_at;
}

/* ---- test images ---- */

enum { W = 7, H = 5 };

static uint16_t g_gray[W * H];          /* top-down, every sample different */
static float g_rgb[3 * W * H];          /* planar, values in 0..1 */

static void make_images(void) {
    int x, y, c;
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x) g_gray[y * W + x] = (uint16_t)(1000 + 100 * y + x);
    for (c = 0; c < 3; ++c)
        for (y = 0; y < H; ++y)
            for (x = 0; x < W; ++x) g_rgb[(c * H + y) * W + x] = (float)(c * 0.25 + y * 0.03 + x * 0.004);
}

static xisfconv_status write_gray(xisfconv_context *ctx, const char *path, const xisfconv_write_options *options,
                                  const xisfconv_keywords *kw, const void *icc, size_t icc_size) {
    xisfconv_writer *w = NULL;
    xisfconv_image img;
    xisfconv_status st = xisfconv_writer_new(ctx, path, options, &w);
    if (st != XISFCONV_OK) return st;
    xisfconv_image_init(&img, sizeof img);
    img.pixels = g_gray;
    img.width = W;
    img.height = H;
    img.channels = 1;
    img.sample_format = XISFCONV_SAMPLE_UINT16;
    img.row_order = XISFCONV_ROWS_TOP_DOWN;
    img.name = "gray";
    img.keywords = kw;
    img.icc_profile = icc;
    img.icc_profile_size = icc_size;
    st = xisfconv_writer_add_image(w, &img);
    if (st != XISFCONV_OK) {
        xisfconv_writer_discard(w);
        return st;
    }
    return xisfconv_writer_finish(w);
}

static int rows_equal(const uint16_t *a, const uint16_t *b, int flipped) {
    int x, y;
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x)
            if (a[y * W + x] != b[(flipped ? H - 1 - y : y) * W + x]) return 0;
    return 1;
}

/* ---- the tests ---- */

static void test_basics(void) {
    xisfconv_status s;
    CHECK(strlen(xisfconv_version()) >= 5, "version text");
    CHECK(xisfconv_version_number() ==
              XISFCONV_VERSION_MAJOR * 10000 + XISFCONV_VERSION_MINOR * 100 + XISFCONV_VERSION_PATCH,
          "version number matches the header");
    {
        char expected[32];
        sprintf(expected, "%d.%d.%d", XISFCONV_VERSION_MAJOR, XISFCONV_VERSION_MINOR, XISFCONV_VERSION_PATCH);
        CHECK(strcmp(expected, xisfconv_version()) == 0, "version text matches the header");
    }
    for (s = 0; s <= 12; ++s) CHECK(xisfconv_status_text(s) && *xisfconv_status_text(s), "status text");
    CHECK(strcmp(xisfconv_status_text(XISFCONV_ERR_CHECKSUM), "checksum mismatch") == 0, "status text of a checksum error");
    CHECK(*xisfconv_status_text(12345), "status text of an unknown status");
    CHECK(xisfconv_sample_size(XISFCONV_SAMPLE_UINT8) == 1 && xisfconv_sample_size(XISFCONV_SAMPLE_UINT16) == 2 &&
              xisfconv_sample_size(XISFCONV_SAMPLE_FLOAT32) == 4 && xisfconv_sample_size(XISFCONV_SAMPLE_FLOAT64) == 8 &&
              xisfconv_sample_size(XISFCONV_SAMPLE_UINT64) == 8 && xisfconv_sample_size(XISFCONV_SAMPLE_AS_STORED) == 0 &&
              xisfconv_sample_size(77) == 0,
          "sample sizes");
    CHECK(xisfconv_codec_available(XISFCONV_CODEC_ZLIB, 1) == 1 && xisfconv_codec_available(XISFCONV_CODEC_LZ4, 0) == 1 &&
              xisfconv_codec_available(XISFCONV_CODEC_LZ4, 1) == 1 && xisfconv_codec_available(XISFCONV_CODEC_LZ4HC, 1) == 1 &&
              xisfconv_codec_available(99, 0) == 0,
          "codec availability");
}

static void test_null_arguments(xisfconv_context *ctx) {
    xisfconv_file *f = (xisfconv_file *)1;
    xisfconv_keywords *kw = NULL;
    xisfconv_report *report = NULL;
    xisfconv_format format = 0;
    uint64_t size = 0;
    CHECK(xisfconv_open(NULL, "x", &f) == XISFCONV_ERR_ARGUMENT, "open without a context");
    CHECK(xisfconv_open(ctx, NULL, &f) == XISFCONV_ERR_ARGUMENT && f == NULL, "open without a path");
    CHECK(*xisfconv_error_message(ctx), "an error leaves a message");
    CHECK(xisfconv_open(ctx, "x", NULL) == XISFCONV_ERR_ARGUMENT, "open without a place for the handle");
    CHECK(xisfconv_detect_format(ctx, NULL, &format) == XISFCONV_ERR_ARGUMENT, "detect_format without a path");
    CHECK(xisfconv_convert(ctx, NULL, "x.fits", NULL) == XISFCONV_ERR_ARGUMENT, "convert without an input");
    CHECK(xisfconv_rewrite(ctx, "a.xisf", NULL, NULL, NULL) == XISFCONV_ERR_ARGUMENT, "rewrite without an output");
    CHECK(xisfconv_verify(ctx, NULL, &report) == XISFCONV_ERR_ARGUMENT && report == NULL, "verify without a path");
    CHECK(xisfconv_keywords_new(NULL, &kw) == XISFCONV_ERR_ARGUMENT, "keywords_new without a context");
    CHECK(xisfconv_keywords_new(ctx, NULL) == XISFCONV_ERR_ARGUMENT, "keywords_new without a place for the list");
    CHECK(xisfconv_pixels_size(NULL, 0, NULL, &size) == XISFCONV_ERR_ARGUMENT, "pixels_size without a file");
    CHECK(xisfconv_image_count(NULL) == 0 && xisfconv_file_size(NULL) == 0 && xisfconv_skipped_count(NULL) == 0, "counts of no file");
    CHECK(*xisfconv_image_name(NULL, 0) == 0 && *xisfconv_image_detail(NULL, 0, "x") == 0 && *xisfconv_file_detail(NULL, "x") == 0 &&
              *xisfconv_skipped_text(NULL, 0) == 0 && *xisfconv_image_unsupported_reason(NULL, 0) == 0,
          "texts of no file are empty, not NULL");
    CHECK(xisfconv_keywords_count(NULL) == 0 && xisfconv_keywords_find(NULL, "A") == -1, "an absent keyword list");
    CHECK(xisfconv_report_verdict(NULL) == XISFCONV_VERDICT_FAILED && *xisfconv_report_summary(NULL) == 0 &&
              xisfconv_report_problem_count(NULL) == 0 && *xisfconv_report_problem(NULL, 3) == 0,
          "an absent report");
    CHECK(*xisfconv_error_message(NULL) == 0, "error message of no context");
    /* none of these may crash */
    xisfconv_close(NULL);
    xisfconv_keywords_free(NULL);
    xisfconv_report_free(NULL);
    xisfconv_writer_discard(NULL);
    xisfconv_context_free(NULL);
    xisfconv_context_set_message_handler(NULL, NULL, NULL);
    xisfconv_context_set_progress_handler(NULL, NULL, NULL);
    xisfconv_image_info_init(NULL, 64);
    xisfconv_read_options_init(NULL, 64);
    xisfconv_convert_options_init(NULL, 64);
    xisfconv_rewrite_options_init(NULL, 64);
    xisfconv_rewrite_result_init(NULL, 64);
    xisfconv_image_init(NULL, 64);
    xisfconv_write_options_init(NULL, 64);
    CHECK(xisfconv_writer_finish(NULL) == XISFCONV_ERR_ARGUMENT, "finish without a writer");
}

/* A program built against an older, shorter struct: init must not write behind what it was given. */
static void test_struct_sizes(void) {
    union {
        xisfconv_read_options options;
        unsigned char bytes[sizeof(xisfconv_read_options) + 16];
    } u;
    union {
        xisfconv_image_info info;
        unsigned char bytes[sizeof(xisfconv_image_info)];
    } v;
    const size_t shorter = offsetof(xisfconv_read_options, verify_checksums);
    size_t i;
    int untouched = 1;
    memset(&u, 0xAB, sizeof u);
    xisfconv_read_options_init(&u.options, shorter);
    for (i = shorter; i < sizeof u; ++i)
        if (u.bytes[i] != 0xAB) untouched = 0;
    CHECK(untouched && u.options.struct_size == shorter && u.options.sample_format == XISFCONV_SAMPLE_AS_STORED,
          "init fills a shorter struct and nothing behind it");
    memset(&u, 0xAB, sizeof u);
    xisfconv_read_options_init(&u.options, sizeof u);   /* a newer, longer struct than the library knows */
    CHECK(u.options.struct_size == sizeof(xisfconv_read_options) && u.options.verify_checksums == 1 &&
              u.bytes[sizeof(xisfconv_read_options)] == 0xAB,
          "init of a longer struct stops at what the library knows");
    memset(&u, 0xAB, sizeof u);
    xisfconv_read_options_init(&u.options, 2);
    CHECK(u.bytes[0] == 0xAB, "a size that cannot hold struct_size leaves the struct alone");
    memset(&v, 0xCD, sizeof v);
    xisfconv_image_info_init(&v.info, offsetof(xisfconv_image_info, sample_format));
    untouched = 1;
    for (i = offsetof(xisfconv_image_info, sample_format); i < sizeof v; ++i)
        if (v.bytes[i] != 0xCD) untouched = 0;
    CHECK(untouched && v.info.width == 0, "the same for xisfconv_image_info");
}

static xisfconv_keywords *test_keywords(xisfconv_context *ctx) {
    xisfconv_keywords *kw = NULL;
    const char *name = NULL, *value = NULL, *comment = NULL, *text = NULL;
    CHECK(xisfconv_keywords_new(ctx, &kw) == XISFCONV_OK && kw, "a new keyword list");
    if (!kw) return NULL;
    CHECK(xisfconv_keywords_count(kw) == 0, "which is empty");
    CHECK(xisfconv_keywords_append(kw, NULL, "1", NULL) == XISFCONV_ERR_ARGUMENT, "a keyword needs a name");
    CHECK(xisfconv_keywords_append(kw, "EXPTIME", "30.5", "exposure [s]") == XISFCONV_OK, "append");
    CHECK(xisfconv_keywords_append_string(kw, "OBJECT", "O'Neil 1", NULL) == XISFCONV_OK, "append a string");
    CHECK(xisfconv_keywords_append_number(kw, "GAIN", 2.0, "e-/ADU") == XISFCONV_OK, "append a number");
    CHECK(xisfconv_keywords_append_number(kw, "BAD", sqrt(-1.0), NULL) == XISFCONV_ERR_ARGUMENT, "NaN is not a keyword value");
    CHECK(xisfconv_keywords_append(kw, "HISTORY", NULL, "made by the test") == XISFCONV_OK, "append a HISTORY card");
    CHECK(xisfconv_keywords_count(kw) == 4, "four cards");
    CHECK(xisfconv_keywords_get(kw, 1, &name, &value, &comment) == XISFCONV_OK && strcmp(name, "OBJECT") == 0 &&
              strcmp(value, "'O''Neil 1'") == 0 && *comment == 0,
          "a string value is quoted, its quote doubled");
    CHECK(xisfconv_keywords_get_text(kw, 1, &text) == XISFCONV_OK && strcmp(text, "O'Neil 1") == 0, "and comes back unquoted");
    CHECK(xisfconv_keywords_get(kw, 2, NULL, &value, NULL) == XISFCONV_OK && strchr(value, '.') != NULL, "a number has a decimal point");
    CHECK(xisfconv_keywords_append_number(kw, "JD", 2460000.123456789, NULL) == XISFCONV_OK &&
              xisfconv_keywords_get(kw, 4, NULL, &value, NULL) == XISFCONV_OK && atof(value) == 2460000.123456789 &&
              xisfconv_keywords_append_number(kw, "TINY", 1.5e-300, NULL) == XISFCONV_OK &&
              xisfconv_keywords_get(kw, 5, NULL, &value, NULL) == XISFCONV_OK && strchr(value, 'E') != NULL && atof(value) == 1.5e-300 &&
              xisfconv_keywords_remove(kw, 5) == XISFCONV_OK && xisfconv_keywords_remove(kw, 4) == XISFCONV_OK,
          "a number keeps all its digits");
    CHECK(xisfconv_keywords_find(kw, "gain") == 2 && xisfconv_keywords_find(kw, "NOPE") == -1, "find, whatever the case");
    CHECK(xisfconv_keywords_get(kw, 9, &name, NULL, NULL) == XISFCONV_ERR_INDEX, "get beyond the end");
    CHECK(xisfconv_keywords_remove(kw, 9) == XISFCONV_ERR_INDEX, "remove beyond the end");
    CHECK(xisfconv_keywords_append(kw, "TEMP", "1", NULL) == XISFCONV_OK && xisfconv_keywords_remove(kw, 4) == XISFCONV_OK &&
              xisfconv_keywords_count(kw) == 4,
          "remove");
    return kw;
}

static void test_writer_and_readers(xisfconv_context *ctx, const xisfconv_keywords *kw) {
    static const unsigned char icc[] = {0, 0, 0, 24, 't', 'e', 's', 't', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const char *names[] = {"gray.xisf", "gray.fits", "gray.asdf", "gray.tif", "gray.png"};
    xisfconv_write_options wo;
    xisfconv_writer *w = NULL;
    xisfconv_image img;
    size_t i;

    xisfconv_write_options_init(&wo, sizeof wo);
    wo.codec = XISFCONV_CODEC_ZLIB;
    wo.checksum = XISFCONV_CHECKSUM_SHA256;
    for (i = 0; i < 5; ++i) {
        const xisfconv_status st = write_gray(ctx, path_of(names[i]), &wo, kw, icc, sizeof icc);
        CHECK(st == XISFCONV_OK, names[i]);
        if (st != XISFCONV_OK) fprintf(stderr, "  %s\n", xisfconv_error_message(ctx));
        CHECK(file_size(path_of(names[i])) > 0, "the file is there");
    }
    CHECK(!file_exists(path_of("gray.xisf.part")), "no temporary file is left");
    CHECK(write_gray(ctx, path_of("gray.fits"), &wo, kw, NULL, 0) == XISFCONV_ERR_EXISTS, "an existing file is not overwritten");
    wo.overwrite = 1;
    CHECK(write_gray(ctx, path_of("gray.fits"), &wo, kw, NULL, 0) == XISFCONV_OK, "unless that is asked for");
    CHECK(write_gray(ctx, path_of("gray.abc"), &wo, kw, NULL, 0) == XISFCONV_ERR_ARGUMENT, "an unknown extension needs a format");
    wo.format = XISFCONV_FORMAT_FITS;
    CHECK(write_gray(ctx, path_of("gray.abc"), &wo, kw, NULL, 0) == XISFCONV_OK, "which can be given");
    wo.format = XISFCONV_FORMAT_AUTO;

    /* arguments of an image */
    CHECK(xisfconv_writer_new(ctx, path_of("bad.xisf"), &wo, &w) == XISFCONV_OK, "a writer");
    xisfconv_image_init(&img, sizeof img);
    img.pixels = g_gray;
    img.width = W;
    img.height = H;
    CHECK(xisfconv_writer_add_image(w, &img) == XISFCONV_ERR_ARGUMENT, "an image needs a sample format");
    img.sample_format = XISFCONV_SAMPLE_UINT16;
    img.width = 0;
    CHECK(xisfconv_writer_add_image(w, &img) == XISFCONV_ERR_ARGUMENT, "and a width");
    img.width = W;
    img.pixels = NULL;
    CHECK(xisfconv_writer_add_image(w, &img) == XISFCONV_ERR_ARGUMENT, "and pixels");
    CHECK(xisfconv_writer_add_image(w, NULL) == XISFCONV_ERR_ARGUMENT, "and has to be there");
    CHECK(xisfconv_writer_finish(w) == XISFCONV_ERR_ARGUMENT, "a writer without images writes nothing");
    CHECK(!file_exists(path_of("bad.xisf")), "really nothing");

    /* XISF */
    {
        xisfconv_file *f = NULL;
        xisfconv_image_info info;
        const xisfconv_keywords *cards = NULL;
        xisfconv_read_options ro;
        uint16_t back[W * H];
        float as_float[W * H];
        unsigned char profile[64];
        const char *text = NULL;
        size_t length = 0, got = 0;
        uint64_t size = 0;
        CHECK(xisfconv_open(ctx, path_of("gray.xisf"), &f) == XISFCONV_OK && f, "open the XISF file");
        if (f) {
            CHECK(xisfconv_file_format(f) == XISFCONV_FORMAT_XISF && xisfconv_image_count(f) == 1, "one XISF image");
            CHECK((long)xisfconv_file_size(f) == file_size(path_of("gray.xisf")), "file size");
            CHECK(strcmp(xisfconv_file_detail(f, "version"), "1.0") == 0, "XISF version");
            xisfconv_image_info_init(&info, sizeof info);
            CHECK(xisfconv_image_info_get(f, 0, &info) == XISFCONV_OK && info.width == W && info.height == H && info.channels == 1 &&
                      info.sample_format == XISFCONV_SAMPLE_UINT16 && info.data_known == 1 &&
                      info.color_space == XISFCONV_COLOR_GRAY && info.row_order == XISFCONV_ROWS_TOP_DOWN &&
                      info.row_order_declared == 1 && info.convertible == 1 && info.has_icc_profile == 1 && info.has_cfa == 0,
                  "XISF image info");
            CHECK(xisfconv_image_info_get(f, 1, &info) == XISFCONV_ERR_INDEX, "no second image");
            CHECK(strcmp(xisfconv_image_name(f, 0), "gray") == 0, "image name");
            CHECK(strncmp(xisfconv_image_detail(f, 0, "compression"), "zlib", 4) == 0, "compressed with zlib");
            CHECK(strncmp(xisfconv_image_detail(f, 0, "checksum"), "sha256:", 7) == 0, "with a SHA-256 checksum");
            CHECK(strcmp(xisfconv_image_detail(f, 0, "sampleFormat"), "UInt16") == 0 && *xisfconv_image_detail(f, 0, "nonsense") == 0,
                  "details by name");
            CHECK(xisfconv_image_keywords(f, 0, &cards) == XISFCONV_OK && xisfconv_keywords_find(cards, "OBJECT") >= 0 &&
                      xisfconv_keywords_find(cards, "GAIN") >= 0,
                  "the keywords are in the file");
            CHECK(xisfconv_keywords_append((xisfconv_keywords *)cards, "X", "1", NULL) == XISFCONV_ERR_ARGUMENT,
                  "a file's keyword list cannot be changed");
            xisfconv_keywords_free((xisfconv_keywords *)cards); /* must be harmless */
            CHECK(xisfconv_keywords_count(cards) >= 4, "nor freed by the caller");
            CHECK(xisfconv_header_text(f, &text, &length) == XISFCONV_OK && length > 100 && strncmp(text, "<?xml", 5) == 0,
                  "the XML header");
            CHECK(xisfconv_pixels_size(f, 0, NULL, &size) == XISFCONV_OK && size == sizeof back, "size of the pixels");
            CHECK(xisfconv_read_pixels(f, 0, NULL, back, sizeof back - 1) == XISFCONV_ERR_BUFFER, "a buffer that is too small");
            CHECK(xisfconv_read_pixels(f, 0, NULL, NULL, sizeof back) == XISFCONV_ERR_ARGUMENT, "no buffer at all");
            CHECK(xisfconv_read_pixels(f, 3, NULL, back, sizeof back) == XISFCONV_ERR_INDEX, "pixels of an image that is not there");
            memset(back, 0, sizeof back);
            CHECK(xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_OK && rows_equal(back, g_gray, 0),
                  "the pixels come back as they were written");
            xisfconv_read_options_init(&ro, sizeof ro);
            ro.row_order = XISFCONV_ROWS_BOTTOM_UP;
            CHECK(xisfconv_read_pixels(f, 0, &ro, back, sizeof back) == XISFCONV_OK && rows_equal(back, g_gray, 1),
                  "and bottom-up when asked");
            xisfconv_read_options_init(&ro, sizeof ro);
            ro.sample_format = XISFCONV_SAMPLE_FLOAT32;
            CHECK(xisfconv_pixels_size(f, 0, &ro, &size) == XISFCONV_OK && size == sizeof as_float, "size as Float32");
            CHECK(xisfconv_read_pixels(f, 0, &ro, as_float, sizeof as_float) == XISFCONV_OK &&
                      fabs(as_float[0] - 1000.0 / 65535.0) < 1e-6 && fabs(as_float[W * H - 1] - g_gray[W * H - 1] / 65535.0) < 1e-6,
                  "integers become [0,1] as Float32");
            ro.sample_format = 42;
            CHECK(xisfconv_read_pixels(f, 0, &ro, as_float, sizeof as_float) == XISFCONV_ERR_ARGUMENT, "an unknown sample format");
            ro.sample_format = XISFCONV_SAMPLE_AS_STORED;
            ro.struct_size = 0;
            CHECK(xisfconv_read_pixels(f, 0, &ro, back, sizeof back) == XISFCONV_ERR_ARGUMENT, "options that were not initialized");
            /* options of an older, shorter layout: only the fields up to row_order are read */
            xisfconv_read_options_init(&ro, sizeof ro);
            ro.row_order = XISFCONV_ROWS_BOTTOM_UP;
            ro.verify_checksums = 12345;
            ro.struct_size = offsetof(xisfconv_read_options, verify_checksums);
            CHECK(xisfconv_read_pixels(f, 0, &ro, back, sizeof back) == XISFCONV_OK && rows_equal(back, g_gray, 1),
                  "options of a shorter, older layout");
            CHECK(xisfconv_read_icc_profile(f, 0, NULL, 0, &got) == XISFCONV_OK && got == sizeof icc, "size of the ICC profile");
            CHECK(xisfconv_read_icc_profile(f, 0, profile, 4, &got) == XISFCONV_ERR_BUFFER, "ICC profile into a small buffer");
            CHECK(xisfconv_read_icc_profile(f, 0, profile, sizeof profile, &got) == XISFCONV_OK && memcmp(profile, icc, sizeof icc) == 0,
                  "the ICC profile comes back");
            CHECK(xisfconv_property_count(f, 0) == 0 && xisfconv_property_find(f, 0, "x") == -1, "no properties");
            CHECK(xisfconv_property_count(f, XISFCONV_FILE_PROPERTIES) >= 2, "file metadata");
            CHECK(xisfconv_property_read_f64(f, 0, "none", NULL, 0, NULL, NULL) == XISFCONV_ERR_NOT_FOUND, "a property that is not there");
            CHECK(xisfconv_stored_stretch(f, 0, NULL, 0, &got) == XISFCONV_ERR_NOT_FOUND, "no saved stretch");
            xisfconv_close(f);
        }
    }

    /* FITS and ASDF: stored bottom-up by default */
    for (i = 1; i <= 2; ++i) {
        xisfconv_file *f = NULL;
        xisfconv_image_info info;
        xisfconv_read_options ro;
        const xisfconv_keywords *cards = NULL;
        uint16_t back[W * H];
        uint64_t size = 0;
        const char *label = i == 1 ? "FITS" : "ASDF";
        CHECK(xisfconv_open(ctx, path_of(names[i]), &f) == XISFCONV_OK && f, label);
        if (!f) continue;
        CHECK(xisfconv_file_format(f) == (i == 1 ? XISFCONV_FORMAT_FITS : XISFCONV_FORMAT_ASDF) && xisfconv_image_count(f) == 1, label);
        xisfconv_image_info_init(&info, sizeof info);
        CHECK(xisfconv_image_info_get(f, 0, &info) == XISFCONV_OK && info.width == W && info.height == H && info.data_known == 0 &&
                  info.row_order == XISFCONV_ROWS_BOTTOM_UP && info.row_order_declared == 1 && info.bitpix == (i == 1 ? 16 : 0),
              "info before the pixels are loaded");
        CHECK(xisfconv_pixels_size(f, 0, NULL, &size) == XISFCONV_OK && size == sizeof back, "size of the pixels (loads them)");
        CHECK(xisfconv_image_info_get(f, 0, &info) == XISFCONV_OK && info.data_known == 1 && info.sample_format == XISFCONV_SAMPLE_UINT16,
              "info after loading");
        CHECK(*xisfconv_image_detail(f, 0, "mapping"), "how the samples were mapped");
        CHECK(xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_OK && rows_equal(back, g_gray, 1),
              "the rows are stored bottom-up");
        xisfconv_read_options_init(&ro, sizeof ro);
        ro.row_order = XISFCONV_ROWS_TOP_DOWN;
        CHECK(xisfconv_read_pixels(f, 0, &ro, back, sizeof back) == XISFCONV_OK && rows_equal(back, g_gray, 0),
              "and read top-down when asked");
        CHECK(xisfconv_image_keywords(f, 0, &cards) == XISFCONV_OK && xisfconv_keywords_find(cards, "EXPTIME") >= 0, "keywords");
        CHECK(strcmp(xisfconv_image_name(f, 0), "gray") == 0, "name");
        CHECK(xisfconv_property_count(f, 0) == 0, "no properties in a file that was not converted from XISF");
        CHECK(xisfconv_read_icc_profile(f, 0, NULL, 0, NULL) == XISFCONV_ERR_NOT_FOUND, "no ICC profile outside XISF");
        xisfconv_close(f);
    }

    /* row order as written: top-down FITS on request */
    {
        xisfconv_file *f = NULL;
        xisfconv_image_info info;
        uint16_t back[W * H];
        xisfconv_write_options_init(&wo, sizeof wo);
        wo.row_order = XISFCONV_ROWS_TOP_DOWN;
        CHECK(write_gray(ctx, path_of("topdown.fits"), &wo, NULL, NULL, 0) == XISFCONV_OK, "FITS with top-down rows");
        CHECK(xisfconv_open(ctx, path_of("topdown.fits"), &f) == XISFCONV_OK, "open it");
        if (f) {
            xisfconv_image_info_init(&info, sizeof info);
            CHECK(xisfconv_image_info_get(f, 0, &info) == XISFCONV_OK && info.row_order == XISFCONV_ROWS_TOP_DOWN, "ROWORDER says so");
            CHECK(xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_OK && rows_equal(back, g_gray, 0), "rows as given");
            xisfconv_close(f);
        }
    }

    /* three float channels: RGB, with its range */
    {
        xisfconv_file *f = NULL;
        xisfconv_image_info info;
        float back[3 * W * H];
        xisfconv_write_options_init(&wo, sizeof wo);
        CHECK(xisfconv_writer_new(ctx, path_of("rgb.xisf"), &wo, &w) == XISFCONV_OK, "a writer for RGB");
        xisfconv_image_init(&img, sizeof img);
        img.pixels = g_rgb;
        img.width = W;
        img.height = H;
        img.channels = 3;
        img.sample_format = XISFCONV_SAMPLE_FLOAT32;
        CHECK(xisfconv_writer_add_image(w, &img) == XISFCONV_OK, "add a float RGB image");
        img.use_bounds = 1;
        img.lower_bound = 2;
        img.upper_bound = 1;
        CHECK(xisfconv_writer_add_image(w, &img) == XISFCONV_ERR_ARGUMENT, "bounds the wrong way round");
        CHECK(xisfconv_writer_finish(w) == XISFCONV_OK, "write it");
        CHECK(xisfconv_open(ctx, path_of("rgb.xisf"), &f) == XISFCONV_OK, "open it");
        if (f) {
            xisfconv_image_info_init(&info, sizeof info);
            CHECK(xisfconv_image_info_get(f, 0, &info) == XISFCONV_OK && info.channels == 3 && info.color_space == XISFCONV_COLOR_RGB &&
                      info.sample_format == XISFCONV_SAMPLE_FLOAT32 && info.lower_bound == 0 && info.upper_bound == 1,
                  "RGB, Float32, bounds 0:1");
            CHECK(xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_OK && memcmp(back, g_rgb, sizeof back) == 0,
                  "float pixels are unchanged");
            xisfconv_close(f);
        }
    }
}

static void test_convert_rewrite_verify(xisfconv_context *ctx) {
    xisfconv_convert_options co;
    xisfconv_rewrite_options ro;
    xisfconv_rewrite_result rr;
    xisfconv_report *report = NULL;
    xisfconv_format format = 0;
    int32_t same = 0;

    CHECK(xisfconv_detect_format(ctx, path_of("gray.xisf"), &format) == XISFCONV_OK && format == XISFCONV_FORMAT_XISF, "detect XISF");
    CHECK(xisfconv_detect_format(ctx, path_of("gray.fits"), &format) == XISFCONV_OK && format == XISFCONV_FORMAT_FITS, "detect FITS");
    CHECK(xisfconv_detect_format(ctx, path_of("gray.asdf"), &format) == XISFCONV_OK && format == XISFCONV_FORMAT_ASDF, "detect ASDF");
    CHECK(xisfconv_detect_format(ctx, path_of("gray.png"), &format) == XISFCONV_ERR_FORMAT, "a PNG is none of them");
    CHECK(xisfconv_detect_format(ctx, path_of("missing.xisf"), &format) == XISFCONV_ERR_IO, "a missing file cannot be read");
    CHECK(xisfconv_detect_format(ctx, g_dir, &format) == XISFCONV_ERR_IO, "nor can a directory");

    /* convert */
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("conv.fits"), NULL) == XISFCONV_OK, "XISF to FITS");
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("conv.fits"), NULL) == XISFCONV_ERR_EXISTS, "not over an existing file");
    xisfconv_convert_options_init(&co, sizeof co);
    co.overwrite = 1;
    co.codec = XISFCONV_CODEC_DEFAULT;
    CHECK(xisfconv_convert(ctx, path_of("conv.fits"), path_of("conv.xisf"), &co) == XISFCONV_OK, "FITS to XISF, compressed");
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("conv.unknown"), &co) == XISFCONV_ERR_ARGUMENT, "no format for the output");
    co.output_format = XISFCONV_FORMAT_PNG;
    co.stretch = XISFCONV_STRETCH_LINKED;
    co.sample_format = XISFCONV_SAMPLE_UINT8;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("conv.unknown"), &co) == XISFCONV_OK, "a stretched 8-bit PNG");
    co.stretch = XISFCONV_STRETCH_STORED;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("stf.png"), &co) == XISFCONV_ERR_NOT_FOUND, "no saved stretch to apply");
    xisfconv_convert_options_init(&co, sizeof co);
    co.image = 5;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("five.fits"), &co) == XISFCONV_ERR_INDEX, "an image that is not there");
    CHECK(xisfconv_convert(ctx, path_of("missing.xisf"), path_of("m.fits"), NULL) == XISFCONV_ERR_IO, "an input that is not there");
    CHECK(xisfconv_convert(ctx, path_of("gray.png"), path_of("m.fits"), NULL) == XISFCONV_ERR_FORMAT, "an input that is no image file");
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("same.xisf"), NULL) == XISFCONV_ERR_ARGUMENT, "XISF to XISF is a rewrite");
    CHECK(xisfconv_convert(ctx, path_of("conv.fits"), path_of("same.fits"), NULL) == XISFCONV_ERR_ARGUMENT, "FITS to FITS needs a reason");
    CHECK(xisfconv_convert(ctx, path_of("gray.fits"), path_of("same.fits"), NULL) == XISFCONV_OK,
          "which a tile-compressed file is (the writer compressed gray.fits)");
    xisfconv_convert_options_init(&co, sizeof co);
    co.sip_order = 1;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("sip.fits"), &co) == XISFCONV_ERR_ARGUMENT, "a SIP order out of range");
    CHECK(!file_exists(path_of("five.fits")) && !file_exists(path_of("five.fits.part")) && !file_exists(path_of("m.fits")),
          "failed conversions leave no files");

    /* a smaller picture: for TIFF and PNG */
    xisfconv_convert_options_init(&co, sizeof co);
    CHECK(co.bin == 1 && co.fit_width == 0 && co.fit_height == 0 && co.scale == 0, "no smaller picture unless one is asked for");
    co.overwrite = 1;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("full.tif"), &co) == XISFCONV_OK, "the image as a TIFF file");
    co.bin = 2;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("small.tif"), &co) == XISFCONV_OK &&
              file_size(path_of("small.tif")) < file_size(path_of("full.tif")),
          "bin = 2 makes a smaller one");
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("small.fits"), &co) == XISFCONV_ERR_ARGUMENT &&
              strstr(xisfconv_error_message(ctx), "TIFF and PNG") && !file_exists(path_of("small.fits")),
          "which is for pictures, not for FITS");
    co.bin = 0;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("small.tif"), &co) == XISFCONV_ERR_ARGUMENT, "bin = 0");
    co.bin = -3;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("small.tif"), &co) == XISFCONV_ERR_ARGUMENT, "a negative bin");
    co.bin = 1;
    co.scale = 1.5;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("small.tif"), &co) == XISFCONV_ERR_ARGUMENT, "a scale above 1");
    co.scale = -0.5;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("small.tif"), &co) == XISFCONV_ERR_ARGUMENT, "a negative scale");
    co.scale = 0.5;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("half.png"), &co) == XISFCONV_OK &&
              file_size(path_of("half.png")) < file_size(path_of("conv.unknown")) + 4096,
          "scale = 0.5 as PNG");
    co.scale = 0;
    co.fit_width = 2;
    co.fit_height = 1000000;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("fit.tif"), &co) == XISFCONV_OK &&
              file_size(path_of("fit.tif")) < file_size(path_of("small.tif")),
          "a box to fit");
    co.fit_width = (uint64_t)-1;
    co.fit_height = (uint64_t)-1;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("same.tif"), &co) == XISFCONV_OK &&
              file_size(path_of("same.tif")) == file_size(path_of("full.tif")),
          "a box larger than the image leaves it as it is");
    /* a caller built against the header of 0.13: its options end before these fields, and what was
       padding at their end then (the field `reserved` now) holds whatever it holds */
    co.bin = 0;
    co.reserved = -559038737;
    co.struct_size = offsetof(xisfconv_convert_options, fit_width);
    CHECK(sizeof(void *) != 8 || co.struct_size == 104, "the options of 0.13 were 104 bytes");
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("older.tif"), &co) == XISFCONV_OK &&
              file_size(path_of("older.tif")) == file_size(path_of("full.tif")),
          "options of the shorter layout of 0.13 ask for no smaller picture");

    /* rewrite */
    xisfconv_rewrite_options_init(&ro, sizeof ro);
    xisfconv_rewrite_result_init(&rr, sizeof rr);
    ro.codec = XISFCONV_CODEC_NONE;
    CHECK(xisfconv_rewrite(ctx, path_of("gray.xisf"), path_of("plain.xisf"), &ro, &rr) == XISFCONV_OK && rr.decompressed == 1 &&
              rr.blocks >= 1 && rr.read_back == 1 && rr.changed == 1 && rr.output_size > 0 &&
              (long)rr.input_size == file_size(path_of("gray.xisf")),
          "rewrite without compression");
    CHECK(xisfconv_rewrite(ctx, path_of("gray.xisf"), path_of("plain.xisf"), &ro, NULL) == XISFCONV_ERR_EXISTS, "not over an existing file");
    CHECK(xisfconv_rewrite(ctx, path_of("gray.fits"), path_of("no.xisf"), &ro, NULL) != XISFCONV_OK, "a FITS file is not rewritten");
    xisfconv_rewrite_options_init(&ro, sizeof ro);
    ro.checksum = XISFCONV_CHECKSUM_SHA1;
    CHECK(xisfconv_stored_as_requested(ctx, path_of("plain.xisf"), &ro, &same) == XISFCONV_OK && same == 0, "not yet stored as requested");
    CHECK(xisfconv_rewrite_in_place(ctx, path_of("plain.xisf"), &ro, &rr) == XISFCONV_OK && rr.changed == 1 && rr.checksums == 1,
          "a checksum in place");
    CHECK(xisfconv_stored_as_requested(ctx, path_of("plain.xisf"), &ro, &same) == XISFCONV_OK && same == 1, "now it is");
    CHECK(xisfconv_rewrite_in_place(ctx, path_of("plain.xisf"), &ro, &rr) == XISFCONV_OK && rr.changed == 0, "and is left alone");
    ro.image = 3;
    CHECK(xisfconv_rewrite(ctx, path_of("gray.xisf"), path_of("three.xisf"), &ro, NULL) == XISFCONV_ERR_INDEX, "an image that is not there");
    ro.image = XISFCONV_ALL_IMAGES;
    ro.codec = 77;
    CHECK(xisfconv_rewrite(ctx, path_of("gray.xisf"), path_of("lz4.xisf"), &ro, NULL) == XISFCONV_ERR_ARGUMENT, "a codec that is none");

    /* verify */
    CHECK(xisfconv_verify(ctx, path_of("gray.xisf"), &report) == XISFCONV_OK && report, "verify");
    CHECK(xisfconv_report_verdict(report) == XISFCONV_VERDICT_OK && xisfconv_report_format(report) == XISFCONV_FORMAT_XISF &&
              xisfconv_report_verified(report) == 1 && xisfconv_report_problem_count(report) == 0 && *xisfconv_report_summary(report),
          "an intact file");
    xisfconv_report_free(report);
    report = NULL;
    CHECK(copy_damaged(path_of("gray.xisf"), path_of("damaged.xisf"), 3), "damage a copy");
    CHECK(xisfconv_verify(ctx, path_of("damaged.xisf"), &report) == XISFCONV_OK && xisfconv_report_verdict(report) == XISFCONV_VERDICT_FAILED &&
              xisfconv_report_problem_count(report) == 1 && strstr(xisfconv_report_problem(report, 0), "checksum mismatch") != NULL,
          "a damaged file is a finding, not an error");
    xisfconv_report_free(report);
    report = NULL;
    CHECK(xisfconv_verify(ctx, path_of("missing.xisf"), &report) == XISFCONV_OK && xisfconv_report_verdict(report) == XISFCONV_VERDICT_FAILED,
          "so is a file that cannot be opened");
    xisfconv_report_free(report);
    report = NULL;
    CHECK(xisfconv_verify(ctx, path_of("gray.fits"), &report) == XISFCONV_OK && xisfconv_report_format(report) == XISFCONV_FORMAT_FITS &&
              xisfconv_report_verdict(report) == XISFCONV_VERDICT_OK,
          "verify a FITS file");
    xisfconv_report_free(report);
    report = NULL;
    CHECK(xisfconv_verify(ctx, path_of("gray.asdf"), &report) == XISFCONV_OK && xisfconv_report_format(report) == XISFCONV_FORMAT_ASDF &&
              xisfconv_report_verdict(report) == XISFCONV_VERDICT_OK && xisfconv_report_verified(report) == 1,
          "verify an ASDF file");
    xisfconv_report_free(report);
    {
        xisfconv_file *f = NULL;
        uint16_t back[W * H];
        xisfconv_status st;
        CHECK(xisfconv_open(ctx, path_of("damaged.xisf"), &f) == XISFCONV_OK, "the damaged file still opens");
        st = xisfconv_read_pixels(f, 0, NULL, back, sizeof back);
        CHECK(st == XISFCONV_ERR_CHECKSUM, "but its pixels are refused");
        xisfconv_close(f);
    }
}

/* ---- tile-compressed FITS ---- */

typedef struct {
    int compressing, others, cancel_at;
    uint64_t last_done, total;
    int in_order;
} tile_progress;

static int32_t on_tile_progress(void *user, const char *stage, uint64_t done, uint64_t total) {
    tile_progress *p = (tile_progress *)user;
    if (strcmp(stage, "compressing") != 0) {
        ++p->others;
        return 0;
    }
    if (p->compressing && (done <= p->last_done || total != p->total)) p->in_order = 0;
    ++p->compressing;
    p->last_done = done;
    p->total = total;
    return p->cancel_at && p->compressing >= p->cancel_at;
}

static const char *tiles_of(xisfconv_context *ctx, const char *path, size_t image, uint16_t *pixels) {
    static char algorithm[32];
    xisfconv_file *f = NULL;
    algorithm[0] = 0;
    if (xisfconv_open(ctx, path, &f) != XISFCONV_OK) return "(cannot open)";
    if (image < xisfconv_image_count(f)) {
        strncpy(algorithm, xisfconv_image_detail(f, image, "tileCompression"), sizeof algorithm - 1);
        if (pixels) {
            xisfconv_read_options ro;
            xisfconv_read_options_init(&ro, sizeof ro);
            ro.row_order = XISFCONV_ROWS_TOP_DOWN;
            if (xisfconv_read_pixels(f, image, &ro, pixels, W * H * sizeof *pixels) != XISFCONV_OK) strcpy(algorithm, "(cannot read)");
        }
    }
    xisfconv_close(f);
    return algorithm;
}

static void test_tile_compression(xisfconv_context *ctx) {
    xisfconv_write_options wo;
    xisfconv_convert_options co;
    xisfconv_writer *w = NULL;
    xisfconv_image img;
    uint16_t back[W * H];
    messages seen;

    /* the writer */
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.codec = XISFCONV_CODEC_DEFAULT;
    CHECK(write_gray(ctx, path_of("tiles.fits"), &wo, NULL, NULL, 0) == XISFCONV_OK, "FITS with the default codec");
    memset(back, 0, sizeof back);
    CHECK(strcmp(tiles_of(ctx, path_of("tiles.fits"), 0, back), "RICE_1") == 0 && rows_equal(back, g_gray, 0),
          "is tile-compressed with RICE_1, and reads back");
    wo.codec = XISFCONV_CODEC_ZLIB;
    CHECK(write_gray(ctx, path_of("gzip.fits"), &wo, NULL, NULL, 0) == XISFCONV_OK &&
              strcmp(tiles_of(ctx, path_of("gzip.fits"), 0, back), "GZIP_2") == 0 && rows_equal(back, g_gray, 0),
          "zlib means GZIP_2");
    CHECK(file_size(path_of("tiles.fits")) % 2880 == 0 && file_size(path_of("gzip.fits")) % 2880 == 0, "whole FITS blocks");
    CHECK(write_gray(ctx, path_of("byname.fits.fz"), NULL, NULL, NULL, 0) == XISFCONV_OK &&
              strcmp(tiles_of(ctx, path_of("byname.fits.fz"), 0, back), "RICE_1") == 0 && rows_equal(back, g_gray, 0),
          "a name that ends in .fz asks for it");
    CHECK(write_gray(ctx, path_of("byname.tif.fz"), NULL, NULL, NULL, 0) == XISFCONV_ERR_ARGUMENT, "but only a FITS name");
    wo.codec = XISFCONV_CODEC_NONE;
    CHECK(write_gray(ctx, path_of("plain.fits"), &wo, NULL, NULL, 0) == XISFCONV_OK && *tiles_of(ctx, path_of("plain.fits"), 0, NULL) == 0,
          "without a codec the image is plain");
    wo.codec = XISFCONV_CODEC_ZSTD;
    {
        const xisfconv_status st = write_gray(ctx, path_of("zstd.fits"), &wo, NULL, NULL, 0);
        CHECK((st == XISFCONV_ERR_ARGUMENT && strstr(xisfconv_error_message(ctx), "Zstandard") != NULL) || st == XISFCONV_ERR_UNSUPPORTED,
              "FITS has no Zstandard");
        CHECK(!file_exists(path_of("zstd.fits")) && !file_exists(path_of("zstd.fits.part")), "and nothing is written");
    }

    /* floating point, and 64-bit integers, which stay as they are */
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.codec = XISFCONV_CODEC_DEFAULT;
    memset(&seen, 0, sizeof seen);
    xisfconv_context_set_message_handler(ctx, on_message, &seen);
    CHECK(xisfconv_writer_new(ctx, path_of("mixed.fits.fz"), &wo, &w) == XISFCONV_OK, "a writer for three images");
    if (w) {
        static uint64_t wide[W * H];
        int i;
        for (i = 0; i < W * H; ++i) wide[i] = (uint64_t)i << 40;
        xisfconv_image_init(&img, sizeof img);
        img.width = W;
        img.height = H;
        img.pixels = g_rgb;
        img.channels = 3;
        img.sample_format = XISFCONV_SAMPLE_FLOAT32;
        CHECK(xisfconv_writer_add_image(w, &img) == XISFCONV_OK, "floating point");
        img.pixels = wide;
        img.channels = 1;
        img.sample_format = XISFCONV_SAMPLE_UINT64;
        img.name = "wide";
        CHECK(xisfconv_writer_add_image(w, &img) == XISFCONV_OK, "64-bit integers");
        img.pixels = g_gray;
        img.sample_format = XISFCONV_SAMPLE_UINT16;
        img.name = "gray";
        CHECK(xisfconv_writer_add_image(w, &img) == XISFCONV_OK, "16-bit integers");
        CHECK(xisfconv_writer_finish(w) == XISFCONV_OK, "write them");
        CHECK(strcmp(tiles_of(ctx, path_of("mixed.fits.fz"), 0, NULL), "GZIP_2") == 0 && *tiles_of(ctx, path_of("mixed.fits.fz"), 1, NULL) == 0 &&
                  strcmp(tiles_of(ctx, path_of("mixed.fits.fz"), 2, back), "RICE_1") == 0 && rows_equal(back, g_gray, 0),
              "GZIP_2 for floating point, 64-bit integers plain, RICE_1 for the rest");
        CHECK(seen.warnings == 1 && strstr(seen.last, "64-bit integer") != NULL && strstr(seen.last, "'wide'") != NULL,
              "a warning names the image that is not compressed");
    }
    xisfconv_context_set_message_handler(ctx, NULL, NULL);

    /* conversions */
    xisfconv_convert_options_init(&co, sizeof co);
    co.codec = XISFCONV_CODEC_DEFAULT;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("conv-tiles.fits"), &co) == XISFCONV_OK &&
              strcmp(tiles_of(ctx, path_of("conv-tiles.fits"), 0, back), "RICE_1") == 0 && rows_equal(back, g_gray, 0),
          "XISF to tile-compressed FITS");
    CHECK(*tiles_of(ctx, path_of("plain.fits"), 0, NULL) == 0 &&
              xisfconv_convert(ctx, path_of("plain.fits"), path_of("packed.fits"), &co) == XISFCONV_OK &&
              strcmp(tiles_of(ctx, path_of("packed.fits"), 0, back), "RICE_1") == 0 && rows_equal(back, g_gray, 0),
          "a plain FITS file to a tile-compressed one");
    CHECK(xisfconv_convert(ctx, path_of("plain.fits"), path_of("packed.fits.fz"), NULL) == XISFCONV_OK &&
              strcmp(tiles_of(ctx, path_of("packed.fits.fz"), 0, NULL), "RICE_1") == 0,
          "and by the name of the output alone");
    CHECK(xisfconv_convert(ctx, path_of("gzip.fits"), path_of("repacked.fits"), &co) == XISFCONV_OK &&
              strcmp(tiles_of(ctx, path_of("repacked.fits"), 0, back), "RICE_1") == 0 && rows_equal(back, g_gray, 0),
          "a tile-compressed file written again with another algorithm");
    CHECK(xisfconv_convert(ctx, path_of("packed.fits.fz"), path_of("unpacked.fits"), NULL) == XISFCONV_OK &&
              *tiles_of(ctx, path_of("unpacked.fits"), 0, back) == 0 && rows_equal(back, g_gray, 0),
          "and back to a plain FITS file");
    co.codec = XISFCONV_CODEC_ZSTD;
    {
        const xisfconv_status st = xisfconv_convert(ctx, path_of("gray.xisf"), path_of("conv-zstd.fits"), &co);
        CHECK((st == XISFCONV_ERR_ARGUMENT || st == XISFCONV_ERR_UNSUPPORTED) && !file_exists(path_of("conv-zstd.fits")), "no Zstandard here either");
    }

    /* an image large enough for the work to be reported, and stopped */
    {
        enum { BW = 3000, BH = 2000 };
        uint16_t *big = (uint16_t *)malloc((size_t)BW * BH * sizeof *big);
        tile_progress tp;
        size_t i;
        CHECK(big != NULL, "memory for a larger image");
        if (big) {
            for (i = 0; i < (size_t)BW * BH; ++i) big[i] = (uint16_t)((i % BW) * 7 + (i / BW) * 3 + (i * 2654435761u >> 28));
            xisfconv_write_options_init(&wo, sizeof wo);
            wo.codec = XISFCONV_CODEC_DEFAULT;
            xisfconv_image_init(&img, sizeof img);
            img.pixels = big;
            img.width = BW;
            img.height = BH;
            img.sample_format = XISFCONV_SAMPLE_UINT16;
            memset(&tp, 0, sizeof tp);
            tp.in_order = 1;
            xisfconv_context_set_progress_handler(ctx, on_tile_progress, &tp);
            CHECK(xisfconv_writer_new(ctx, path_of("big.fits.fz"), &wo, &w) == XISFCONV_OK && xisfconv_writer_add_image(w, &img) == XISFCONV_OK &&
                      xisfconv_writer_finish(w) == XISFCONV_OK,
                  "a larger image");
            CHECK(tp.compressing >= 2 && tp.in_order && tp.total == BH && tp.last_done < BH,
                  "the compression reports how far it is, in rows");
            CHECK(file_size(path_of("big.fits.fz")) > 0 && file_size(path_of("big.fits.fz")) < (long)BW * BH * 2 * 3 / 4, "and compresses");
            memset(&tp, 0, sizeof tp);
            tp.in_order = 1;
            tp.cancel_at = 2;
            CHECK(xisfconv_writer_new(ctx, path_of("stopped.fits.fz"), &wo, &w) == XISFCONV_OK && xisfconv_writer_add_image(w, &img) == XISFCONV_OK &&
                      xisfconv_writer_finish(w) == XISFCONV_ERR_CANCELLED,
                  "it can be stopped on the way");
            CHECK(!file_exists(path_of("stopped.fits.fz")) && !file_exists(path_of("stopped.fits.fz.part")), "which leaves no file");
            xisfconv_context_set_progress_handler(ctx, NULL, NULL);
            free(big);
        }
    }
}

static void test_callbacks(xisfconv_context *ctx) {
    messages seen;
    progress steps;
    xisfconv_write_options wo;
    xisfconv_convert_options co;
    xisfconv_report *report = NULL;

    memset(&seen, 0, sizeof seen);
    xisfconv_context_set_message_handler(ctx, on_message, &seen);
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.checksum = XISFCONV_CHECKSUM_SHA3_256;
    CHECK(write_gray(ctx, path_of("sha3.xisf"), &wo, NULL, NULL, 0) == XISFCONV_OK, "write with a SHA-3 checksum");
    CHECK(seen.warnings == 1 && seen.with_path == 1 && strstr(seen.last, "PixInsight") != NULL, "which comes with a warning");
    xisfconv_convert_options_init(&co, sizeof co);
    co.overwrite = 1;
    CHECK(xisfconv_convert(ctx, path_of("gray.fits"), path_of("note.xisf"), &co) == XISFCONV_OK && seen.infos >= 1,
          "a conversion reports what it did");
    xisfconv_context_set_message_handler(ctx, NULL, NULL);
    memset(&seen, 0, sizeof seen);
    CHECK(write_gray(ctx, path_of("sha3b.xisf"), &wo, NULL, NULL, 0) == XISFCONV_OK && seen.warnings == 0, "no handler, no messages");

    memset(&steps, 0, sizeof steps);
    xisfconv_context_set_progress_handler(ctx, on_progress, &steps);
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("progress.fits"), NULL) == XISFCONV_OK && steps.calls >= 2,
          "a conversion reports progress");
    steps.calls = 0;
    CHECK(xisfconv_verify(ctx, path_of("gray.xisf"), &report) == XISFCONV_OK && steps.calls >= 1, "so does verifying");
    xisfconv_report_free(report);
    report = NULL;
    steps.calls = 0;
    steps.cancel_at = 1;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("cancelled.fits"), NULL) == XISFCONV_ERR_CANCELLED, "a conversion can be cancelled");
    CHECK(!file_exists(path_of("cancelled.fits")) && !file_exists(path_of("cancelled.fits.part")), "and leaves nothing behind");
    steps.calls = 0;
    CHECK(xisfconv_verify(ctx, path_of("gray.xisf"), &report) == XISFCONV_ERR_CANCELLED && report == NULL, "verifying can be cancelled");
    steps.calls = 0;
    CHECK(xisfconv_rewrite(ctx, path_of("gray.xisf"), path_of("cancelled.xisf"), NULL, NULL) == XISFCONV_ERR_CANCELLED, "so can a rewrite");
    CHECK(!file_exists(path_of("cancelled.xisf")) && !file_exists(path_of("cancelled.xisf.part")), "which leaves nothing behind either");
    xisfconv_context_set_progress_handler(ctx, NULL, NULL);
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("cancelled.fits"), NULL) == XISFCONV_OK, "without the handler it goes through");
}

/* A progress handler that asks through the context for the call to stop, as another thread or a
 * signal handler would, and itself says "go on". */
typedef struct {
    xisfconv_context *ctx;
    int calls, cancel_at, told;
} canceller;

static int32_t on_progress_cancel_told(void *user, const char *stage, uint64_t done, uint64_t total) {
    canceller *c = (canceller *)user;
    (void)stage, (void)done, (void)total;
    if (++c->calls == c->cancel_at) c->told = xisfconv_context_running(c->ctx) + 2 * xisfconv_context_cancel(c->ctx);
    return 0;
}

static int32_t on_progress_cancel(void *user, const char *stage, uint64_t done, uint64_t total) {
    canceller *c = (canceller *)user;
    (void)stage, (void)done, (void)total;
    if (++c->calls == c->cancel_at) xisfconv_context_cancel(c->ctx);
    return 0;
}

static void test_kept_messages_and_cancel(xisfconv_context *ctx) {
    messages seen;
    canceller stop;
    xisfconv_write_options wo;
    xisfconv_convert_options co;
    xisfconv_report *report = NULL;
    xisfconv_message_level level = 0;
    const char *path = "x", *text = "x";
    size_t n, i;
    int infos = 0, warnings = 0;

    /* messages kept in the context instead of handed to a handler */
    CHECK(xisfconv_context_message_count(ctx) == 0, "nothing is kept unless asked for");
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.checksum = XISFCONV_CHECKSUM_SHA3_256;
    CHECK(write_gray(ctx, path_of("kept0.xisf"), &wo, NULL, NULL, 0) == XISFCONV_OK && xisfconv_context_message_count(ctx) == 0,
          "a warning without a handler is dropped");
    xisfconv_context_keep_messages(ctx, 1);
    CHECK(write_gray(ctx, path_of("kept1.xisf"), &wo, NULL, NULL, 0) == XISFCONV_OK, "write with a SHA-3 checksum, keeping messages");
    CHECK(xisfconv_context_message_count(ctx) == 1, "its warning is kept (it came from the writer, a handle of the context)");
    CHECK(xisfconv_context_message(ctx, 0, &level, &path, &text) == XISFCONV_OK && level == XISFCONV_MESSAGE_WARNING && path &&
              strcmp(path, path_of("kept1.xisf")) == 0 && strstr(text, "PixInsight") != NULL,
          "with its level, its file and its text");
    CHECK(xisfconv_context_message(ctx, 0, NULL, NULL, NULL) == XISFCONV_OK, "which need not all be asked for");
    CHECK(xisfconv_context_message(ctx, 1, &level, &path, &text) == XISFCONV_ERR_INDEX && level == XISFCONV_MESSAGE_INFO && path == NULL &&
              text && *text == 0,
          "there is no second one");
    CHECK(xisfconv_context_message(NULL, 0, &level, &path, &text) == XISFCONV_ERR_ARGUMENT && path == NULL && text && *text == 0 &&
              xisfconv_context_message_count(NULL) == 0,
          "and none without a context");

    /* they add up over the calls until they are cleared; a handler is called as well */
    memset(&seen, 0, sizeof seen);
    xisfconv_context_set_message_handler(ctx, on_message, &seen);
    xisfconv_convert_options_init(&co, sizeof co);
    co.overwrite = 1;
    CHECK(xisfconv_convert(ctx, path_of("gray.fits"), path_of("kept2.xisf"), &co) == XISFCONV_OK, "a conversion, keeping messages");
    n = xisfconv_context_message_count(ctx);
    CHECK(n >= 2 && (int)n - 1 == seen.infos + seen.warnings, "its notes are kept after the warning, and the handler heard them too");
    for (i = 0; i < n; ++i) {
        CHECK(xisfconv_context_message(ctx, i, &level, &path, &text) == XISFCONV_OK && text && *text, "a kept message");
        if (level == XISFCONV_MESSAGE_WARNING) ++warnings;
        else if (level == XISFCONV_MESSAGE_INFO) ++infos;
        if (i > 0) CHECK(path && strcmp(path, path_of("gray.fits")) == 0, "names the file that was converted");
    }
    CHECK(warnings == 1 + seen.warnings && infos == seen.infos && infos >= 1, "warnings and notes, in order");
    xisfconv_context_set_message_handler(ctx, NULL, NULL);
    CHECK(xisfconv_convert(ctx, path_of("missing.fits"), path_of("kept3.xisf"), &co) != XISFCONV_OK &&
              xisfconv_context_message_count(ctx) == n,
          "a failing call leaves what was kept");
    xisfconv_context_clear_messages(ctx);
    CHECK(xisfconv_context_message_count(ctx) == 0 && xisfconv_context_message(ctx, 0, NULL, NULL, NULL) == XISFCONV_ERR_INDEX,
          "cleared");

    /* keeping ends: what was kept is dropped, and nothing more is kept */
    CHECK(write_gray(ctx, path_of("kept4.xisf"), &wo, NULL, NULL, 0) == XISFCONV_OK && xisfconv_context_message_count(ctx) == 1,
          "one more warning");
    xisfconv_context_keep_messages(ctx, 0);
    CHECK(xisfconv_context_message_count(ctx) == 0, "keeping ended: nothing is left");
    CHECK(write_gray(ctx, path_of("kept5.xisf"), &wo, NULL, NULL, 0) == XISFCONV_OK && xisfconv_context_message_count(ctx) == 0,
          "and nothing is kept any more");
    xisfconv_context_keep_messages(NULL, 1);
    xisfconv_context_clear_messages(NULL);
    xisfconv_context_cancel(NULL);

    /* a request to stop, made through the context */
    xisfconv_context_cancel(ctx);
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("stopped.fits"), NULL) == XISFCONV_OK,
          "a request made while no call runs is dropped");
    remove(path_of("stopped.fits"));
    memset(&stop, 0, sizeof stop);
    stop.ctx = ctx;
    stop.cancel_at = 1;
    xisfconv_context_set_progress_handler(ctx, on_progress_cancel, &stop);
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("stopped.fits"), NULL) == XISFCONV_ERR_CANCELLED && stop.calls == 1,
          "a request made during a call stops it at its next step");
    CHECK(!file_exists(path_of("stopped.fits")) && !file_exists(path_of("stopped.fits.part")), "and nothing is left behind");
    CHECK(strstr(xisfconv_error_message(ctx), "cancel") != NULL, "the error says so");
    stop.calls = 0;
    CHECK(xisfconv_rewrite(ctx, path_of("gray.xisf"), path_of("stopped.xisf"), NULL, NULL) == XISFCONV_ERR_CANCELLED &&
              !file_exists(path_of("stopped.xisf")) && !file_exists(path_of("stopped.xisf.part")),
          "a rewrite is stopped the same way");
    stop.calls = 0;
    CHECK(xisfconv_verify(ctx, path_of("gray.xisf"), &report) == XISFCONV_ERR_CANCELLED && report == NULL, "and verifying");
    stop.calls = 0;
    stop.cancel_at = 0;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("stopped.fits"), NULL) == XISFCONV_OK && stop.calls >= 2,
          "the request was for that call only: the next one goes through");
    xisfconv_context_set_progress_handler(ctx, NULL, NULL);
    stop.calls = 0;
    xisfconv_context_cancel(ctx);
    CHECK(xisfconv_verify(ctx, path_of("gray.xisf"), &report) == XISFCONV_OK && report, "also without a handler");
    xisfconv_report_free(report);
}

/* A host's progress handler, as an interpreter would give it. */
typedef struct {
    int reports, answer_at;
    int32_t answer; /* given at report `answer_at`; "go on" otherwise */
    int stage_ok;
} host;

static int32_t host_progress(const xisfconv_progress_report *report) {
    host *h = (host *)report->user;
    if (!report->stage || !*report->stage || (report->total && report->done > report->total)) h->stage_ok = 0;
    return ++h->reports == h->answer_at ? h->answer : XISFCONV_HOST_GO_ON;
}

static void test_host_progress(xisfconv_context *ctx) {
    host h;
    progress steps;
    xisfconv_report *report = NULL;

    memset(&h, 0, sizeof h);
    h.stage_ok = 1;
    memset(&steps, 0, sizeof steps);
    xisfconv_context_set_host_progress(ctx, host_progress, &h);
    xisfconv_context_set_progress_handler(ctx, on_progress, &steps);
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("host.fits"), NULL) == XISFCONV_OK && h.reports >= 2 && h.stage_ok,
          "a conversion reports to the host's progress handler");
    CHECK(steps.calls == h.reports && xisfconv_context_host_progress_failed(ctx) == 0, "and to the ordinary handler, as often");
    remove(path_of("host.fits"));

    /* "stop" stops the call, and the ordinary handler is not asked any more */
    h.reports = steps.calls = 0;
    h.answer_at = 2;
    h.answer = XISFCONV_HOST_STOP;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("host.fits"), NULL) == XISFCONV_ERR_CANCELLED && h.reports == 2 &&
              steps.calls == 1,
          "the answer \"stop\" stops the call");
    CHECK(xisfconv_context_host_progress_failed(ctx) == 0 && !file_exists(path_of("host.fits")) &&
              !file_exists(path_of("host.fits.part")),
          "which is not a failure of the handler, and leaves nothing behind");

    /* any other answer stops the call too and is remembered until the next call */
    {
        static const int32_t odd[] = {0, 1, 2, -1, 0x676F6F6F};
        size_t i;
        for (i = 0; i < sizeof odd / sizeof odd[0]; ++i) {
            h.reports = 0;
            h.answer_at = 1;
            h.answer = odd[i];
            CHECK(xisfconv_verify(ctx, path_of("gray.xisf"), &report) == XISFCONV_ERR_CANCELLED && report == NULL && h.reports == 1 &&
                      xisfconv_context_host_progress_failed(ctx) == 1,
                  "an answer that is neither stops the call and counts as a failure of the handler");
        }
    }
    h.reports = 0;
    h.answer_at = 0;
    CHECK(xisfconv_verify(ctx, path_of("gray.xisf"), &report) == XISFCONV_OK && xisfconv_context_host_progress_failed(ctx) == 0,
          "until the next call");
    xisfconv_report_free(report);
    report = NULL;

    /* without it; without a context */
    xisfconv_context_set_host_progress(ctx, NULL, NULL);
    h.reports = steps.calls = 0;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("host.fits"), NULL) == XISFCONV_OK && h.reports == 0 && steps.calls >= 2,
          "without it only the ordinary handler is called");
    xisfconv_context_set_progress_handler(ctx, NULL, NULL);
    xisfconv_context_set_host_progress(NULL, host_progress, &h);
    CHECK(xisfconv_context_host_progress_failed(NULL) == 0, "and nothing without a context");

    /* cancel tells whether a call was running */
    CHECK(xisfconv_context_cancel(ctx) == 0 && xisfconv_context_cancel(NULL) == 0 && xisfconv_context_running(ctx) == 0 &&
              xisfconv_context_running(NULL) == 0,
          "no call is running");
    {
        canceller stop;
        memset(&stop, 0, sizeof stop);
        stop.ctx = ctx;
        stop.cancel_at = 1;
        xisfconv_context_set_progress_handler(ctx, on_progress_cancel_told, &stop);
        CHECK(xisfconv_verify(ctx, path_of("gray.xisf"), &report) == XISFCONV_ERR_CANCELLED && stop.told == 3,
              "during a call the context says that one is running, and so does the request to stop it");
        CHECK(xisfconv_context_running(ctx) == 0, "and afterwards no more");
        xisfconv_context_set_progress_handler(ctx, NULL, NULL);
    }
}

static void test_stretch_and_wcs(xisfconv_context *ctx) {
    xisfconv_stretch_params params[3];
    float out[3 * W * H];
    float dark[W * H];
    int i, ok = 1;

    for (i = 0; i < W * H; ++i) dark[i] = (float)(0.01 + 0.0002 * i);
    memset(params, 0, sizeof params);
    CHECK(xisfconv_auto_stretch(ctx, dark, W, H, 1, XISFCONV_SAMPLE_FLOAT32, 0, 1, 1, 1, params) == XISFCONV_OK &&
              params[0].midtones > 0 && params[0].midtones < 0.5 && params[0].highlights == 1,
          "auto-stretch of dark data");
    CHECK(xisfconv_apply_stretch(ctx, dark, W, H, 1, XISFCONV_SAMPLE_FLOAT32, 0, 1, params, 1, out) == XISFCONV_OK, "apply it");
    for (i = 0; i < W * H; ++i)
        if (!(out[i] >= 0 && out[i] <= 1)) ok = 0;
    CHECK(ok && out[W * H - 1] > dark[W * H - 1] * 5, "the result is brighter and within 0..1");
    CHECK(xisfconv_auto_stretch(ctx, g_rgb, W, H, 3, XISFCONV_SAMPLE_FLOAT32, 0, 1, 3, 0, params) == XISFCONV_OK, "three channels, unlinked");
    CHECK(xisfconv_apply_stretch(ctx, g_rgb, W, H, 3, XISFCONV_SAMPLE_FLOAT32, 0, 1, params, 3, out) == XISFCONV_OK, "apply to three channels");
    CHECK(xisfconv_auto_stretch(ctx, g_rgb, W, H, 3, XISFCONV_SAMPLE_FLOAT32, 0, 1, 4, 0, params) == XISFCONV_ERR_ARGUMENT, "more colour channels than channels");
    CHECK(xisfconv_auto_stretch(ctx, NULL, W, H, 1, XISFCONV_SAMPLE_FLOAT32, 0, 1, 1, 1, params) == XISFCONV_ERR_ARGUMENT, "no pixels");
    CHECK(xisfconv_apply_stretch(ctx, dark, W, H, 1, XISFCONV_SAMPLE_FLOAT32, 1, 1, params, 1, out) == XISFCONV_ERR_ARGUMENT, "an empty range");

    {
        xisfconv_keywords *kw = NULL, *wcs = NULL;
        xisfconv_file *f = NULL;
        xisfconv_write_options wo;
        const char *value = NULL, *summary = NULL;
        double before = 0, after = 0;
        int64_t at;
        xisfconv_keywords_new(ctx, &kw);
        xisfconv_keywords_append_string(kw, "CTYPE1", "RA---TAN", NULL);
        xisfconv_keywords_append_string(kw, "CTYPE2", "DEC--TAN", NULL);
        xisfconv_keywords_append_number(kw, "CRVAL1", 10.5, NULL);
        xisfconv_keywords_append_number(kw, "CRVAL2", 41.25, NULL);
        xisfconv_keywords_append_number(kw, "CRPIX1", 4.0, NULL);
        xisfconv_keywords_append_number(kw, "CRPIX2", 2.0, NULL);
        xisfconv_keywords_append_number(kw, "CD1_1", -0.0003, NULL);
        xisfconv_keywords_append_number(kw, "CD1_2", 0.0, NULL);
        xisfconv_keywords_append_number(kw, "CD2_1", 0.0, NULL);
        xisfconv_keywords_append_number(kw, "CD2_2", 0.0003, NULL);
        xisfconv_keywords_append_string(kw, "OBJECT", "M 31", NULL);
        xisfconv_write_options_init(&wo, sizeof wo);
        /* the buffer is top-down, and so are its keywords: CRPIX2 = 2 counts from the top */
        CHECK(write_gray(ctx, path_of("wcs.fits"), &wo, kw, NULL, 0) == XISFCONV_OK, "FITS with WCS keywords");
        CHECK(xisfconv_open(ctx, path_of("wcs.fits"), &f) == XISFCONV_OK, "open it");
        if (f) {
            xisfconv_image_info info;
            xisfconv_image_info_init(&info, sizeof info);
            CHECK(xisfconv_image_info_get(f, 0, &info) == XISFCONV_OK && info.has_astrometric_solution == 1, "it has a solution");
            CHECK(xisfconv_wcs_keywords(f, 0, XISFCONV_ROWS_DEFAULT, 3, &wcs, &summary) == XISFCONV_OK && wcs && summary, "its WCS keywords");
            CHECK(xisfconv_keywords_find(wcs, "OBJECT") == -1 && xisfconv_keywords_find(wcs, "CTYPE1") >= 0 &&
                      xisfconv_keywords_find(wcs, "CD2_2") >= 0,
                  "only the WCS keywords");
            at = xisfconv_keywords_find(wcs, "CRPIX2");
            CHECK(at >= 0 && xisfconv_keywords_get(wcs, (size_t)at, NULL, &value, NULL) == XISFCONV_OK, "CRPIX2");
            before = value ? atof(value) : 0;
            /* stored bottom-up: row 2 from the top of 5 rows is row 4 from the bottom */
            CHECK(fabs(before - 4.0) < 1e-9, "CRPIX2 follows the rows as they are stored");
            xisfconv_keywords_free(wcs);
            wcs = NULL;
            CHECK(xisfconv_wcs_keywords(f, 0, XISFCONV_ROWS_TOP_DOWN, 3, &wcs, NULL) == XISFCONV_OK, "the same for top-down rows");
            at = xisfconv_keywords_find(wcs, "CRPIX2");
            xisfconv_keywords_get(wcs, (size_t)at, NULL, &value, NULL);
            after = value ? atof(value) : 0;
            CHECK(fabs(after - 2.0) < 1e-9, "CRPIX2 for top-down rows is what was handed in");
            CHECK(xisfconv_wcs_flip_rows(wcs, H) == XISFCONV_OK && xisfconv_wcs_flip_rows(wcs, H) == XISFCONV_OK, "flip twice");
            xisfconv_keywords_get(wcs, (size_t)at, NULL, &value, NULL);
            CHECK(value && fabs(atof(value) - 2.0) < 1e-9, "restores the value");
            xisfconv_keywords_free(wcs);
            wcs = NULL;
            CHECK(xisfconv_wcs_keywords(f, 0, XISFCONV_ROWS_DEFAULT, 1, &wcs, NULL) == XISFCONV_ERR_ARGUMENT && wcs == NULL, "a SIP order out of range");
            xisfconv_close(f);
        }
        CHECK(xisfconv_open(ctx, path_of("gray.fits"), &f) == XISFCONV_OK, "a file without WCS");
        if (f) {
            CHECK(xisfconv_wcs_keywords(f, 0, XISFCONV_ROWS_DEFAULT, 3, &wcs, NULL) == XISFCONV_ERR_NOT_FOUND && wcs == NULL, "has none");
            xisfconv_close(f);
        }
        /* From XISF and back out: its WCS keywords are bottom-up whatever its rows are, and say so. */
        xisfconv_write_options_init(&wo, sizeof wo);
        CHECK(write_gray(ctx, path_of("wcs.xisf"), &wo, kw, NULL, 0) == XISFCONV_OK, "XISF with WCS keywords");
        CHECK(xisfconv_open(ctx, path_of("wcs.xisf"), &f) == XISFCONV_OK, "open it");
        if (f) {
            const xisfconv_keywords *cards = NULL;
            xisfconv_image_info info;
            xisfconv_writer *w = NULL;
            xisfconv_image img;
            uint16_t back[W * H];
            xisfconv_image_info_init(&info, sizeof info);
            CHECK(xisfconv_image_info_get(f, 0, &info) == XISFCONV_OK && info.row_order == XISFCONV_ROWS_TOP_DOWN &&
                      info.wcs_row_order == XISFCONV_ROWS_BOTTOM_UP,
                  "XISF: rows top-down, WCS keywords bottom-up");
            CHECK(xisfconv_image_keywords(f, 0, &cards) == XISFCONV_OK, "its keywords");
            at = xisfconv_keywords_find(cards, "CRPIX2");
            CHECK(at >= 0 && xisfconv_keywords_get(cards, (size_t)at, NULL, &value, NULL) == XISFCONV_OK && fabs(atof(value) - 4.0) < 1e-9,
                  "CRPIX2 in the file counts from the bottom");
            CHECK(xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_OK, "its pixels");
            /* hand pixels, row order, keywords and their row order to the writer as they came */
            wo.overwrite = 1;
            wo.row_order = XISFCONV_ROWS_TOP_DOWN;
            CHECK(xisfconv_writer_new(ctx, path_of("wcs-again.fits"), &wo, &w) == XISFCONV_OK, "a writer");
            xisfconv_image_init(&img, sizeof img);
            img.pixels = back;
            img.width = W;
            img.height = H;
            img.sample_format = XISFCONV_SAMPLE_UINT16;
            img.row_order = info.row_order;
            img.keywords = cards;
            img.wcs_row_order = info.wcs_row_order;
            CHECK(xisfconv_writer_add_image(w, &img) == XISFCONV_OK && xisfconv_writer_finish(w) == XISFCONV_OK, "write them as FITS, rows top-down");
            xisfconv_close(f);
            f = NULL;
            CHECK(xisfconv_open(ctx, path_of("wcs-again.fits"), &f) == XISFCONV_OK, "open the copy");
            if (f) {
                xisfconv_image_info_init(&info, sizeof info);
                CHECK(xisfconv_image_info_get(f, 0, &info) == XISFCONV_OK && info.row_order == XISFCONV_ROWS_TOP_DOWN &&
                          info.wcs_row_order == XISFCONV_ROWS_TOP_DOWN,
                      "FITS: WCS keywords follow the stored rows");
                CHECK(xisfconv_image_keywords(f, 0, &cards) == XISFCONV_OK, "keywords of the copy");
                at = xisfconv_keywords_find(cards, "CRPIX2");
                CHECK(at >= 0 && xisfconv_keywords_get(cards, (size_t)at, NULL, &value, NULL) == XISFCONV_OK && fabs(atof(value) - 2.0) < 1e-9,
                      "the WCS still points at the same pixels: CRPIX2 counts from the top again");
                xisfconv_close(f);
            }
        }
        CHECK(xisfconv_wcs_flip_rows(kw, 0) == XISFCONV_ERR_ARGUMENT, "flipping needs the image height");
        xisfconv_keywords_free(kw);
    }
}

/* Handles keep their context alive: the order of freeing does not matter. */
/* The header of an image for FITS, cards as text, and the cards the writer does not take. */
/* XISF properties in FITS and ASDF files that were converted from XISF */
static void test_carried_properties(xisfconv_context *ctx) {
    static const char *LINEAR = "PCL:AstrometricSolution:LinearTransformationMatrix";
    static const char *SYSTEM = "PCL:AstrometricSolution:ProjectionSystem";
    xisfconv_keywords *kw = NULL;
    xisfconv_write_options wo;
    xisfconv_convert_options co;
    xisfconv_file *f = NULL;
    double matrix[4] = {0, 0, 0, 0}, carried[4] = {0, 0, 0, 0};
    size_t count = 0, rows = 0, columns = 0, i;
    int k;

    /* an XISF file with properties: the solution that is written from WCS keywords */
    xisfconv_keywords_new(ctx, &kw);
    xisfconv_keywords_append_string(kw, "CTYPE1", "RA---TAN", NULL);
    xisfconv_keywords_append_string(kw, "CTYPE2", "DEC--TAN", NULL);
    xisfconv_keywords_append_number(kw, "CRVAL1", 10.5, NULL);
    xisfconv_keywords_append_number(kw, "CRVAL2", 41.25, NULL);
    xisfconv_keywords_append_number(kw, "CRPIX1", 4.0, NULL);
    xisfconv_keywords_append_number(kw, "CRPIX2", 2.0, NULL);
    xisfconv_keywords_append_number(kw, "CD1_1", -0.0003, NULL);
    xisfconv_keywords_append_number(kw, "CD1_2", 0.00001, NULL);
    xisfconv_keywords_append_number(kw, "CD2_1", 0.00002, NULL);
    xisfconv_keywords_append_number(kw, "CD2_2", 0.0003, NULL);
    xisfconv_write_options_init(&wo, sizeof wo);
    CHECK(write_gray(ctx, path_of("solved.xisf"), &wo, kw, NULL, 0) == XISFCONV_OK, "an XISF file with solution properties");
    xisfconv_keywords_free(kw);
    CHECK(xisfconv_open(ctx, path_of("solved.xisf"), &f) == XISFCONV_OK && f, "open it");
    if (!f) return;
    count = xisfconv_property_count(f, 0);
    CHECK(count >= 6 && xisfconv_property_read_f64(f, 0, LINEAR, matrix, 4, &rows, &columns) == XISFCONV_OK && rows == 2 && columns == 2,
          "its properties");
    xisfconv_close(f);

    xisfconv_convert_options_init(&co, sizeof co);
    CHECK(co.properties == 1, "properties are taken along unless told otherwise");
    for (k = 0; k < 2; ++k) {
        const char *carrier = path_of(k ? "carried.asdf" : "carried.fits");
        const char *label = k ? "ASDF" : "FITS";
        const char *id = NULL, *type = NULL, *value = NULL, *comment = NULL;
        int32_t block = -1;
        int64_t at;
        f = NULL;
        xisfconv_convert_options_init(&co, sizeof co);
        co.overwrite = 1;
        CHECK(xisfconv_convert(ctx, path_of("solved.xisf"), carrier, &co) == XISFCONV_OK, label);
        CHECK(xisfconv_open(ctx, carrier, &f) == XISFCONV_OK && f, label);
        if (!f) continue;
        CHECK(xisfconv_image_count(f) == 1 && xisfconv_skipped_count(f) == 0, "the properties are no image and nothing that is skipped");
        CHECK(xisfconv_property_count(f, 0) == count && xisfconv_property_count(f, XISFCONV_FILE_PROPERTIES) == 0 &&
                  xisfconv_property_count(f, 1) == 0,
              "the file carries the properties of the image");
        at = xisfconv_property_find(f, 0, SYSTEM);
        CHECK(at >= 0 && xisfconv_property_find(f, 0, "No:Such") == -1 && xisfconv_property_find(f, 0, NULL) == -1 &&
                  xisfconv_property_find(f, 7, SYSTEM) == -1,
              "one of them is found by its id");
        CHECK(at >= 0 && xisfconv_property_get(f, 0, (size_t)at, &id, &type, &value, &comment, &block) == XISFCONV_OK &&
                  strcmp(id, SYSTEM) == 0 && strcmp(type, "String") == 0 && strcmp(value, "Gnomonic") == 0 && *comment == 0 && block == 0,
              "a String with its text");
        at = xisfconv_property_find(f, 0, LINEAR);
        CHECK(at >= 0 && xisfconv_property_get(f, 0, (size_t)at, NULL, &type, &value, NULL, &block) == XISFCONV_OK &&
                  strcmp(type, "F64Matrix") == 0 && *value == 0 && block == 1,
              "a matrix is said to be numbers");
        CHECK(xisfconv_property_get(f, 0, count, &id, NULL, NULL, NULL, NULL) == XISFCONV_ERR_INDEX, "an index beyond the last property");
        CHECK(*xisfconv_property_format(f, 0, (size_t)at) == 0 && *xisfconv_property_format(f, 0, count) == 0 &&
                  *xisfconv_property_format(f, 9, 0) == 0 && *xisfconv_property_format(NULL, 0, 0) == 0,
              "a property without a format, and one that is not there, have the format \"\"");
        rows = columns = 0;
        CHECK(xisfconv_property_read_f64(f, 0, LINEAR, NULL, 0, &rows, &columns) == XISFCONV_OK && rows == 2 && columns == 2, "its shape");
        CHECK(xisfconv_property_read_f64(f, 0, LINEAR, carried, 3, NULL, NULL) == XISFCONV_ERR_BUFFER, "a buffer that is too small");
        CHECK(xisfconv_property_read_f64(f, 0, LINEAR, carried, 4, &rows, &columns) == XISFCONV_OK &&
                  memcmp(carried, matrix, sizeof matrix) == 0,
              "its numbers are those of the XISF file, bit for bit");
        CHECK(xisfconv_property_read_f64(f, 0, SYSTEM, carried, 4, NULL, NULL) == XISFCONV_ERR_NOT_FOUND &&
                  xisfconv_property_read_f64(f, 0, "No:Such", carried, 4, NULL, NULL) == XISFCONV_ERR_NOT_FOUND &&
                  xisfconv_property_read_f64(f, 3, LINEAR, carried, 4, NULL, NULL) == XISFCONV_ERR_INDEX,
              "what is no vector or matrix, what is not there, an image that is not there");
        xisfconv_close(f);

        /* back to XISF: the same properties, in the same order */
        f = NULL;
        CHECK(xisfconv_convert(ctx, carrier, path_of("restored.xisf"), &co) == XISFCONV_OK &&
                  xisfconv_open(ctx, path_of("restored.xisf"), &f) == XISFCONV_OK && f,
              "back to XISF");
        if (f) {
            xisfconv_file *first = NULL;
            int same = xisfconv_property_count(f, 0) == count;
            CHECK(xisfconv_open(ctx, path_of("solved.xisf"), &first) == XISFCONV_OK && first, "the first file again");
            for (i = 0; first && same && i < count; ++i) {
                const char *id2 = NULL, *type2 = NULL, *value2 = NULL;
                if (xisfconv_property_get(first, 0, i, &id, &type, &value, NULL, NULL) != XISFCONV_OK ||
                    xisfconv_property_get(f, 0, i, &id2, &type2, &value2, NULL, NULL) != XISFCONV_OK || strcmp(id, id2) != 0 ||
                    strcmp(type, type2) != 0 || strcmp(value, value2) != 0) {
                    same = 0;
                }
            }
            CHECK(same, "every property is there again, the time the solution was made at included");
            xisfconv_close(first);
            xisfconv_close(f);
        }

        /* without them */
        f = NULL;
        co.properties = 0;
        CHECK(xisfconv_convert(ctx, path_of("solved.xisf"), carrier, &co) == XISFCONV_OK &&
                  xisfconv_open(ctx, carrier, &f) == XISFCONV_OK && f && xisfconv_property_count(f, 0) == 0,
              "properties = 0 leaves them out");
        xisfconv_close(f);
        /* a caller built against the header of 0.12, whose options end before that field */
        f = NULL;
        co.struct_size = offsetof(xisfconv_convert_options, properties);
        CHECK(xisfconv_convert(ctx, path_of("solved.xisf"), carrier, &co) == XISFCONV_OK &&
                  xisfconv_open(ctx, carrier, &f) == XISFCONV_OK && f && xisfconv_property_count(f, 0) == count,
              "options of the shorter layout of 0.12 take them along");
        xisfconv_close(f);
    }
}

/* Properties from the caller's values, their values in the type of their elements, and what the
 * writer learned with them: the LZ4 codecs, a compression level, no byte shuffling, the name of
 * the program that writes (0.15). */
enum { BW = 64, BH = 48, MANY = 600 };

static uint16_t g_smooth[BW * BH];   /* compresses */

static xisfconv_status write_smooth(xisfconv_context *ctx, const char *path, const xisfconv_write_options *options,
                                    const xisfconv_keywords *kw, const xisfconv_properties *properties) {
    xisfconv_writer *w = NULL;
    xisfconv_image img;
    xisfconv_status st = xisfconv_writer_new(ctx, path, options, &w);
    if (st != XISFCONV_OK) return st;
    xisfconv_image_init(&img, sizeof img);
    img.pixels = g_smooth;
    img.width = BW;
    img.height = BH;
    img.sample_format = XISFCONV_SAMPLE_UINT16;
    img.name = "smooth";
    img.keywords = kw;
    img.properties = properties;
    st = xisfconv_writer_add_image(w, &img);
    if (st != XISFCONV_OK) {
        xisfconv_writer_discard(w);
        return st;
    }
    return xisfconv_writer_finish(w);
}

static int smooth_comes_back(xisfconv_context *ctx, const char *path) {
    static uint16_t back[BW * BH];
    xisfconv_file *f = NULL;
    xisfconv_read_options ro;
    int ok;
    if (xisfconv_open(ctx, path, &f) != XISFCONV_OK || !f) return 0;
    xisfconv_read_options_init(&ro, sizeof ro);
    ro.row_order = XISFCONV_ROWS_TOP_DOWN;
    memset(back, 0, sizeof back);
    ok = xisfconv_read_pixels(f, 0, &ro, back, sizeof back) == XISFCONV_OK && memcmp(back, g_smooth, sizeof back) == 0;
    xisfconv_close(f);
    return ok;
}

/* The XML header of an XISF file, as text; NULL if it cannot be read. Valid until the next call. */
static const char *header_of(const char *path) {
    static char text[200000];
    unsigned char head[16];
    unsigned long length;
    FILE *file = fopen(path, "rb");
    text[0] = 0;
    if (!file) return NULL;
    if (fread(head, 1, 16, file) != 16 || memcmp(head, "XISF0100", 8) != 0) {
        fclose(file);
        return NULL;
    }
    length = (unsigned long)head[8] | ((unsigned long)head[9] << 8) | ((unsigned long)head[10] << 16) | ((unsigned long)head[11] << 24);
    if (length >= sizeof text || fread(text, 1, length, file) != length) {
        fclose(file);
        return NULL;
    }
    fclose(file);
    text[length] = 0;
    return text;
}

/* Replaces the first `from` in a file by `to`, of the same length. 1 if it was found. */
static int change_in_file(const char *path, const char *from, const char *to) {
    static char data[4000000];
    const size_t n = strlen(from);
    size_t size, i;
    FILE *file = fopen(path, "rb");
    if (!file || strlen(to) != n) {
        if (file) fclose(file);
        return 0;
    }
    size = fread(data, 1, sizeof data, file);
    fclose(file);
    for (i = 0; i + n <= size; ++i)
        if (memcmp(data + i, from, n) == 0) break;
    if (i + n > size) return 0;
    memcpy(data + i, to, n);
    file = fopen(path, "wb");
    if (!file) return 0;
    i = fwrite(data, 1, size, file);
    return fclose(file) == 0 && i == size;
}

/* The value of a property of an image (or of the file) that is not an array; NULL if it has none of that id. */
static const char *property_text(xisfconv_file *f, size_t image, const char *wanted, const char *wanted_type) {
    const char *type = NULL, *value = NULL;
    int32_t block = -1;
    const int64_t at = xisfconv_property_find(f, image, wanted);
    if (at < 0 || xisfconv_property_get(f, image, (size_t)at, NULL, &type, &value, NULL, &block) != XISFCONV_OK) return NULL;
    if (block != 0 || (wanted_type && strcmp(type, wanted_type) != 0)) return NULL;
    return value;
}

static void test_given_properties(xisfconv_context *ctx) {
    static const float vector[3] = {1.5f, -2.5f, 1e-20f};
    static const int16_t table[6] = {-1, 2, -3, 4, -5, 32767};
    static const double pairs[4] = {1.0, 2.0, -3.5, 0.25}; /* two complex numbers */
    static const unsigned char bytes[2] = {7, 250};
    static double many[MANY], many_back[MANY];
    static char long_text[4001], header_text[3500];
    xisfconv_properties *own = NULL, *whole = NULL, *solved = NULL, *odd = NULL;
    xisfconv_keywords *kw = NULL;
    xisfconv_write_options wo;
    xisfconv_convert_options co;
    xisfconv_rewrite_options ro;
    xisfconv_rewrite_result rr;
    xisfconv_writer *w = NULL;
    xisfconv_file *f = NULL;
    const char *id = NULL, *type = NULL, *value = NULL, *comment = NULL, *text;
    int32_t block = -1, matrix = -1;
    int64_t at;
    size_t size = 0, rows = 0, columns = 0, count, i;
    float vector_back[3];
    int16_t table_back[6];
    double pairs_back[4];
    unsigned char bytes_back[2];
    int k, x, y;
    union {
        xisfconv_image image;
        unsigned char raw[sizeof(xisfconv_image)];
    } older;
    union {
        xisfconv_write_options options;
        unsigned char raw[sizeof(xisfconv_write_options)];
    } older_options;

    for (y = 0; y < BH; ++y)
        for (x = 0; x < BW; ++x) g_smooth[y * BW + x] = (uint16_t)(2000 + 3 * y + x / 4);
    for (i = 0; i < MANY; ++i) many[i] = 0.5 * (double)i;

    /* the elements of the vector and matrix types */
    CHECK(xisfconv_property_element("F32Vector", &matrix) == XISFCONV_ELEMENT_FLOAT32 && matrix == 0, "the element of a vector type");
    CHECK(xisfconv_property_element("UI16Matrix", &matrix) == XISFCONV_ELEMENT_UINT16 && matrix == 1, "and of a matrix type");
    CHECK(xisfconv_property_element("ByteArray", NULL) == XISFCONV_ELEMENT_UINT8 &&
              xisfconv_property_element("C64Vector", NULL) == XISFCONV_ELEMENT_COMPLEX64 &&
              xisfconv_property_element("Vector", NULL) == XISFCONV_ELEMENT_FLOAT64 &&
              xisfconv_property_element("IMatrix", NULL) == XISFCONV_ELEMENT_INT32 &&
              xisfconv_property_element("I64Vector", NULL) == XISFCONV_ELEMENT_INT64,
          "the other names");
    matrix = 5;
    CHECK(xisfconv_property_element("String", &matrix) == XISFCONV_ELEMENT_NONE && matrix == 0 &&
              xisfconv_property_element("", NULL) == XISFCONV_ELEMENT_NONE && xisfconv_property_element(NULL, NULL) == XISFCONV_ELEMENT_NONE &&
              xisfconv_property_element("Float64", NULL) == XISFCONV_ELEMENT_NONE,
          "a type that is not a vector or a matrix has no element");
    CHECK(xisfconv_element_size(XISFCONV_ELEMENT_INT8) == 1 && xisfconv_element_size(XISFCONV_ELEMENT_UINT16) == 2 &&
              xisfconv_element_size(XISFCONV_ELEMENT_FLOAT32) == 4 && xisfconv_element_size(XISFCONV_ELEMENT_UINT64) == 8 &&
              xisfconv_element_size(XISFCONV_ELEMENT_COMPLEX32) == 8 && xisfconv_element_size(XISFCONV_ELEMENT_COMPLEX64) == 16 &&
              xisfconv_element_size(XISFCONV_ELEMENT_NONE) == 0 && xisfconv_element_size(99) == 0,
          "element sizes");

    /* a list of properties */
    CHECK(xisfconv_properties_new(ctx, &own) == XISFCONV_OK && own && xisfconv_properties_count(own) == 0, "a property list");
    CHECK(xisfconv_properties_new(NULL, &whole) == XISFCONV_ERR_ARGUMENT && xisfconv_properties_new(ctx, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_count(NULL) == 0,
          "what a list is not made of");
    xisfconv_properties_free(NULL);
    if (!own) return;
    CHECK(xisfconv_properties_set(own, "Lab:Flag", "Boolean", "true", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Count", "Int32", "-5", "a count", "%d") == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Byte", "UInt8", "255", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Low", "Int64", "-9223372036854775808", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:High", "UInt64", "18446744073709551615", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Gain", "Float32", "0.25", NULL, "%.2f") == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Nothing", "Float64", "nan", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Phase", "Complex64", "(1,-2.5)", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Name", "String", "caf\xC3\xA9 <&> \"au lait\"", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Lines", "String", "one\ntwo", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Empty", "String", "", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:When", "TimePoint", "2026-10-06T18:30:00.250Z", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Day", "TimePoint", "2026-10-06", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(own, "Lab:Short", "Short", "-32768", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_count(own) == 14,
          "scalars, strings and time points");
    count = xisfconv_properties_count(own);
    CHECK(xisfconv_properties_set(own, "X", "UInt8", "256", NULL, NULL) == XISFCONV_ERR_ARGUMENT && strstr(xisfconv_error_message(ctx), "UInt8") &&
              xisfconv_properties_set(own, "X", "Int8", "-129", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "UInt16", "-1", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Int64", "9223372036854775808", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "UInt64", "18446744073709551616", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Int32", "1.5", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Int32", "", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Int32", " 5", NULL, NULL) == XISFCONV_ERR_ARGUMENT,
          "a number the type does not hold");
    CHECK(xisfconv_properties_set(own, "X", "Boolean", "yes", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Float64", "much", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Complex32", "(1;2)", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Complex32", "1,2", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "yesterday", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "2026-10-06T18", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "String", "\xFF\xFE", NULL, NULL) == XISFCONV_ERR_ARGUMENT,
          "a value that is not one of its type");
    CHECK(xisfconv_properties_set(own, "X", "Boolean", "1", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Boolean", "True", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Float64", " 1.5 ", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Float64", "0x10", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Float64", "1e999", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Float32", "1e39", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Complex64", "(1, 2)", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Complex32", "(1e39,0)", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "2026-13-06", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "2026-10-32", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "2026-10-06T25:00:00", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "2026-10-06T18:61:00", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "2026-10-06Z", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "2026-02-30", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "2026-02-29T12:00:00Z", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "1900-02-29", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "TimePoint", "2026-04-31", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_count(own) == count,
          "nor is one that only looks like it");
    xisfconv_properties_new(ctx, &odd);
    CHECK(xisfconv_properties_set(odd, "X", "Float32", "3.4028235e38", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(odd, "X", "Float32", "-inf", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(odd, "X", "TimePoint", "2024-02-29", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(odd, "X", "TimePoint", "2000-02-29T23:59:60Z", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(odd, "X", "TimePoint", "2026-10-06T18:30:00+02:00", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_count(odd) == 1,
          "the largest number of a type, and a time with its zone");
    /* what a file said is carried as it said it */
    memset(long_text, 'a', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = 0;
    CHECK(xisfconv_properties_set_as_read(odd, "Odd:Wide", "Float128", "1.5", "sixteen bytes", NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_OK &&
              xisfconv_properties_set_as_read(odd, "Odd:Flag", "Boolean", "1", NULL, NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_OK &&
              xisfconv_properties_set_as_read(odd, "Odd:Whole", "Float64", " 2000 ", NULL, "%.1f", XISFCONV_PROPERTY_VALUE) == XISFCONV_OK &&
              xisfconv_properties_set_as_read(odd, "Odd:Kind", "Table", "what is this", NULL, NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_OK &&
              xisfconv_properties_set(odd, "Odd:Long", "String", long_text, NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_count(odd) == 6,
          "properties as a file had them");
    CHECK(xisfconv_properties_set_as_read(odd, "Y\x01", "Float64", "1", NULL, NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_as_read(odd, "Y", "Float64", "1", "a\x01", NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_as_read(odd, "Y", "F64Vector", "1 2", NULL, NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_as_read(odd, "Y", "UI8Matrix", "", NULL, NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_as_read(odd, "", "Float64", "1", NULL, NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_as_read(odd, "Y", "Float64", NULL, NULL, NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_as_read(NULL, "Y", "Float64", "1", NULL, NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_count(odd) == 6,
          "but not an id or a comment that XML cannot hold, nor a vector as text");
    /* where a text is kept: in the header as it is, or as data */
    for (i = 0; i + 1 < sizeof header_text; ++i) header_text[i] = i % 50 == 48 ? '\r' : i % 50 == 49 ? '\n' : 'b';
    header_text[sizeof header_text - 2] = 'b';
    header_text[sizeof header_text - 1] = 0;
    CHECK(xisfconv_properties_set_as_read(odd, "Odd:Header", "String", header_text, NULL, NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_OK &&
              xisfconv_properties_set_as_read(odd, "Odd:Kept", "String", "one\r\ntwo", NULL, NULL, XISFCONV_PROPERTY_TEXT_BLOCK) ==
                  XISFCONV_OK &&
              xisfconv_properties_set(odd, "Odd:Lines", "String", "three\r\nfour", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_count(odd) == 9,
          "a text of the header, a text that is kept as data, and a new one with a carriage return");
    CHECK(xisfconv_properties_set_as_read(odd, "Y", "Float64", "1", NULL, NULL, XISFCONV_PROPERTY_TEXT_BLOCK) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_as_read(odd, "Y", "String", "1", NULL, NULL, XISFCONV_PROPERTY_ARRAY) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_as_read(odd, "Y", "String", "1", NULL, NULL, XISFCONV_PROPERTY_NONE) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_as_read(odd, "Y", "String", "1", NULL, NULL, 99) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_count(odd) == 9,
          "a storage a value does not have");
    {
        /* a property without a type, bytes that are no text, and texts with something at their ends */
        xisfconv_properties *as_they_are = NULL;
        xisfconv_properties_new(ctx, &as_they_are);
        CHECK(xisfconv_properties_set_as_read(as_they_are, "Bare:NoType", "", "1", NULL, NULL, XISFCONV_PROPERTY_VALUE) == XISFCONV_OK &&
                  xisfconv_properties_set_as_read(as_they_are, "Bare:Bytes", "String", "a\xFF\xFEz", NULL, NULL, XISFCONV_PROPERTY_TEXT_BLOCK) ==
                      XISFCONV_OK &&
                  xisfconv_properties_set_as_read(as_they_are, "Bare:Ends", "String", " line one\r\nline two\r\n", NULL, NULL,
                                                  XISFCONV_PROPERTY_VALUE) == XISFCONV_OK &&
                  xisfconv_properties_set_as_read(as_they_are, "Bare:Mac", "String", "one\rtwo", NULL, NULL, XISFCONV_PROPERTY_VALUE) ==
                      XISFCONV_OK &&
                  xisfconv_properties_set(as_they_are, "Bare:New", "String", " new, with blanks ", NULL, NULL) == XISFCONV_OK &&
                  xisfconv_properties_set_as_read(as_they_are, "Bare:Bell", "Float64", "a\x01", NULL, NULL, XISFCONV_PROPERTY_VALUE) ==
                      XISFCONV_OK &&
                  xisfconv_properties_set_as_read(as_they_are, "Bare:Latin", "String", "caf\xE9", NULL, NULL, XISFCONV_PROPERTY_VALUE) ==
                      XISFCONV_OK,
              "what a file may say");
        xisfconv_write_options_init(&wo, sizeof wo);
        wo.overwrite = 1;
        CHECK(write_smooth(ctx, path_of("bare.xisf"), &wo, NULL, as_they_are) == XISFCONV_OK &&
                  (text = header_of(path_of("bare.xisf"))) != NULL && strstr(text, "<Property id=\"Bare:NoType\" type=\"\" value=\"1\"/>") &&
                  strstr(text, "<Property id=\"Bare:Bytes\" type=\"String\" location=\"inline:base64\">Yf/+eg==</Property>") &&
                  strstr(text, "<Property id=\"Bare:Ends\" type=\"String\"> line one\r\nline two\r\n</Property>") &&
                  strstr(text, "<Property id=\"Bare:Mac\" type=\"String\">one\rtwo</Property>") &&
                  strstr(text, "<Property id=\"Bare:New\" type=\"String\" location=\"inline:base64\">") &&
                  strstr(text, "<Property id=\"Bare:Bell\" type=\"Float64\" value=\"a \"/>") &&
                  strstr(text, "<Property id=\"Bare:Latin\" type=\"String\" location=\"inline:base64\">Y2Fm6Q==</Property>"),
              "is written as the file said it: a text of the header with the bytes it had, a new one with blanks as data, "
              "and what XML cannot hold as a conversion writes it");
        f = NULL;
        if (xisfconv_open(ctx, path_of("bare.xisf"), &f) == XISFCONV_OK && f) {
            CHECK((text = property_text(f, 0, "Bare:Ends", "String")) != NULL && strcmp(text, " line one\r\nline two\r\n") == 0 &&
                      (text = property_text(f, 0, "Bare:New", "String")) != NULL && strcmp(text, " new, with blanks ") == 0 &&
                      xisfconv_property_stored(f, 0, (size_t)xisfconv_property_find(f, 0, "Bare:Ends")) == XISFCONV_PROPERTY_VALUE &&
                      xisfconv_property_stored(f, 0, (size_t)xisfconv_property_find(f, 0, "Bare:New")) == XISFCONV_PROPERTY_TEXT_BLOCK,
                  "and read again as it was");
            xisfconv_close(f);
        }
        xisfconv_properties_free(as_they_are);
    }
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.overwrite = 1;
    wo.codec = XISFCONV_CODEC_ZLIB;
    CHECK(write_smooth(ctx, path_of("odd.xisf"), &wo, NULL, odd) == XISFCONV_OK, "are written");
    f = NULL;
    if (xisfconv_open(ctx, path_of("odd.xisf"), &f) == XISFCONV_OK && f) {
        CHECK((text = property_text(f, 0, "Odd:Wide", "Float128")) != NULL && strcmp(text, "1.5") == 0 &&
                  (text = property_text(f, 0, "Odd:Flag", "Boolean")) != NULL && strcmp(text, "1") == 0 &&
                  (text = property_text(f, 0, "Odd:Whole", "Float64")) != NULL && strcmp(text, " 2000 ") == 0 &&
                  (text = property_text(f, 0, "Odd:Kind", "Table")) != NULL && strcmp(text, "what is this") == 0 &&
                  (text = property_text(f, 0, "Odd:Header", "String")) != NULL && strcmp(text, header_text) == 0 &&
                  (text = property_text(f, 0, "Odd:Kept", "String")) != NULL && strcmp(text, "one\r\ntwo") == 0 &&
                  (text = property_text(f, 0, "Odd:Lines", "String")) != NULL && strcmp(text, "three\r\nfour") == 0 &&
                  xisfconv_property_count(f, 0) == 9,
              "and are there as they were given");
        at = xisfconv_property_find(f, 0, "Odd:Wide");
        CHECK(at >= 0 && xisfconv_property_get(f, 0, (size_t)at, NULL, NULL, NULL, &comment, NULL) == XISFCONV_OK &&
                  comment && strcmp(comment, "sixteen bytes") == 0,
              "with their comments");
        at = xisfconv_property_find(f, 0, "Odd:Long");
        block = -1;
        CHECK(at >= 0 && xisfconv_property_get(f, 0, (size_t)at, NULL, &type, &value, NULL, &block) == XISFCONV_OK &&
                  strcmp(type, "String") == 0 && strcmp(value, long_text) == 0 && block == 0,
              "a long text reads as any text");
        CHECK(xisfconv_property_stored(f, 0, (size_t)at) == XISFCONV_PROPERTY_TEXT_BLOCK &&
                  xisfconv_property_stored(f, 0, (size_t)xisfconv_property_find(f, 0, "Odd:Kept")) == XISFCONV_PROPERTY_TEXT_BLOCK &&
                  xisfconv_property_stored(f, 0, (size_t)xisfconv_property_find(f, 0, "Odd:Lines")) == XISFCONV_PROPERTY_TEXT_BLOCK &&
                  xisfconv_property_stored(f, 0, (size_t)xisfconv_property_find(f, 0, "Odd:Header")) == XISFCONV_PROPERTY_VALUE &&
                  xisfconv_property_stored(f, 0, (size_t)xisfconv_property_find(f, 0, "Odd:Wide")) == XISFCONV_PROPERTY_VALUE &&
                  xisfconv_property_stored(f, 0, 9) == XISFCONV_PROPERTY_NONE && xisfconv_property_stored(f, 7, 0) == XISFCONV_PROPERTY_NONE &&
                  xisfconv_property_stored(NULL, 0, 0) == XISFCONV_PROPERTY_NONE,
              "how each is stored");
        xisfconv_close(f);
        text = header_of(path_of("odd.xisf"));
        CHECK(text && strstr(text, "id=\"Odd:Long\" type=\"String\" location=\"attachment:") && strstr(text, "compression=\"zlib:") &&
                  !strstr(text, "aaaaaaaa"),
              "and is a data block in the file, compressed with its codec");
        CHECK(text && strstr(text, "<Property id=\"Odd:Header\" type=\"String\">bbbb") && strstr(text, "bbbb\r\nbbbb") &&
                  strstr(text, "<Property id=\"Odd:Kept\" type=\"String\" location=\"inline:base64\">b25lDQp0d28=</Property>") &&
                  strstr(text, "<Property id=\"Odd:Lines\" type=\"String\" location=\"inline:base64\">"),
              "a text of the header stays there with its line ends, whatever its length; the two others are data");
    } else {
        CHECK(0, "open it");
    }
    xisfconv_properties_free(odd);
    CHECK(xisfconv_properties_set_array(own, "X", "I16Matrix", NULL, 0, UINT64_MAX, 0, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_array(own, "X", "I16Matrix", NULL, 0, 0, (uint64_t)1 << 31, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_count(own) == count,
          "a matrix of more rows than can be counted, of no columns");
    CHECK(xisfconv_properties_set(own, "X", "Table", "1", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "F64Vector", "1 2 3", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "", "1", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "", "Int32", "1", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "a\x01" "b", "Int32", "1", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Int32", "1", "a\x02", NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, NULL, "Int32", "1", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", NULL, "1", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(own, "X", "Int32", NULL, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set(NULL, "X", "Int32", "1", NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_count(own) == count,
          "what is not a property, and nothing of it is kept");
    CHECK(xisfconv_properties_set(own, "Lab:Count", "Int32", "7", "counted again", NULL) == XISFCONV_OK &&
              xisfconv_properties_count(own) == count,
          "an id that is set again replaces the property");
    CHECK(xisfconv_properties_set_array(own, "Lab:Vector", "F32Vector", vector, sizeof vector, 3, 0, "three numbers", NULL) == XISFCONV_OK &&
              xisfconv_properties_set_array(own, "Lab:Table", "I16Matrix", table, sizeof table, 2, 3, NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set_array(own, "Lab:Pairs", "C64Vector", pairs, sizeof pairs, 2, 0, NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set_array(own, "Lab:Bytes", "ByteArray", bytes, sizeof bytes, 2, 0, NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set_array(own, "Lab:Many", "F64Vector", many, sizeof many, MANY, 0, NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set_array(own, "Lab:None", "UI32Vector", NULL, 0, 0, 0, NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_count(own) == count + 6,
          "vectors and matrices");
    count = xisfconv_properties_count(own);
    CHECK(xisfconv_properties_set_array(own, "X", "F32Vector", vector, sizeof vector, 2, 0, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_array(own, "X", "F32Vector", vector, sizeof vector - 1, 3, 0, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_array(own, "X", "F32Vector", vector, sizeof vector, 3, 1, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_array(own, "X", "I16Matrix", table, sizeof table, 2, 2, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_array(own, "X", "I16Matrix", table, sizeof table, (uint64_t)1 << 63, 4, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_array(own, "X", "Float64", many, 8, 1, 0, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_array(own, "X", "F64Vector", NULL, 8, 1, 0, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_array(own, NULL, "F64Vector", many, 8, 1, 0, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_set_array(NULL, "X", "F64Vector", many, 8, 1, 0, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              xisfconv_properties_count(own) == count,
          "what is not a vector or a matrix");

    /* written to XISF: LZ4HC at its highest level, without byte shuffling, by a program with a name */
    xisfconv_properties_new(ctx, &whole);
    CHECK(xisfconv_properties_set(whole, "Note:Author", "String", "somebody", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(whole, "XISF:CreatorOS", "String", "an abacus", NULL, NULL) == XISFCONV_OK &&
              xisfconv_properties_set(whole, "XISF:BlockAlignmentSize", "UInt16", "1", NULL, NULL) == XISFCONV_OK,
          "properties of the file");
    xisfconv_write_options_init(&wo, sizeof wo);
    CHECK(wo.shuffle == 1 && wo.compression_level == 0 && wo.properties == NULL && wo.creator_application == NULL,
          "the defaults of what is new in the write options");
    wo.overwrite = 1;
    wo.codec = XISFCONV_CODEC_LZ4HC;
    wo.compression_level = 12;
    wo.shuffle = 0;
    wo.properties = whole;
    wo.creator_application = "  capi test 1.0 ";
    CHECK(write_smooth(ctx, path_of("given.xisf"), &wo, NULL, own) == XISFCONV_OK, "an image with properties");
    CHECK(xisfconv_open(ctx, path_of("given.xisf"), &f) == XISFCONV_OK && f, "open it");
    if (f) {
        CHECK(xisfconv_property_count(f, 0) == count, "every property is there");
        CHECK(xisfconv_property_get(f, 0, 0, &id, &type, &value, &comment, &block) == XISFCONV_OK && strcmp(id, "Lab:Flag") == 0 &&
                  strcmp(type, "Boolean") == 0 && strcmp(value, "true") == 0 && block == 0,
              "in the order they were set in");
        at = xisfconv_property_find(f, 0, "Lab:Count");
        CHECK(at == 1 && xisfconv_property_get(f, 0, 1, NULL, &type, &value, &comment, NULL) == XISFCONV_OK && strcmp(type, "Int32") == 0 &&
                  strcmp(value, "7") == 0 && strcmp(comment, "counted again") == 0 && *xisfconv_property_format(f, 0, 1) == 0,
              "the one that was replaced, in its place");
        text = property_text(f, 0, "Lab:Gain", "Float32");
        at = xisfconv_property_find(f, 0, "Lab:Gain");
        CHECK(text && strcmp(text, "0.25") == 0 && at >= 0 && strcmp(xisfconv_property_format(f, 0, (size_t)at), "%.2f") == 0, "a format");
        CHECK((text = property_text(f, 0, "Lab:Low", "Int64")) != NULL && strcmp(text, "-9223372036854775808") == 0 &&
                  (text = property_text(f, 0, "Lab:High", "UInt64")) != NULL && strcmp(text, "18446744073709551615") == 0 &&
                  (text = property_text(f, 0, "Lab:Nothing", "Float64")) != NULL && strcmp(text, "nan") == 0 &&
                  (text = property_text(f, 0, "Lab:Phase", "Complex64")) != NULL && strcmp(text, "(1,-2.5)") == 0 &&
                  (text = property_text(f, 0, "Lab:Short", "Short")) != NULL && strcmp(text, "-32768") == 0,
              "numbers as they were given");
        CHECK((text = property_text(f, 0, "Lab:Name", "String")) != NULL && strcmp(text, "caf\xC3\xA9 <&> \"au lait\"") == 0 &&
                  (text = property_text(f, 0, "Lab:Lines", "String")) != NULL && strcmp(text, "one\ntwo") == 0 &&
                  (text = property_text(f, 0, "Lab:Empty", "String")) != NULL && *text == 0 &&
                  (text = property_text(f, 0, "Lab:When", "TimePoint")) != NULL && strcmp(text, "2026-10-06T18:30:00.250Z") == 0,
              "text and time");

        /* arrays in the type of their elements */
        at = xisfconv_property_find(f, 0, "Lab:Vector");
        CHECK(at >= 0 && xisfconv_property_get(f, 0, (size_t)at, NULL, &type, &value, &comment, &block) == XISFCONV_OK && block == 1 &&
                  strcmp(type, "F32Vector") == 0 && strcmp(comment, "three numbers") == 0,
              "a vector is a data block");
        CHECK(at >= 0 && xisfconv_property_stored(f, 0, (size_t)at) == XISFCONV_PROPERTY_ARRAY &&
                  xisfconv_property_stored(f, 0, (size_t)xisfconv_property_find(f, 0, "Lab:Lines")) == XISFCONV_PROPERTY_VALUE &&
                  xisfconv_property_stored(f, XISFCONV_FILE_PROPERTIES, 0) == XISFCONV_PROPERTY_VALUE,
              "and is stored as an array; a text and a property of the file as values");
        CHECK(at >= 0 && xisfconv_property_read(f, 0, (size_t)at, NULL, 0, &size, &rows, &columns) == XISFCONV_OK && size == sizeof vector &&
                  rows == 1 && columns == 3,
              "its size and shape");
        CHECK(at >= 0 && xisfconv_property_read(f, 0, (size_t)at, vector_back, sizeof vector_back - 1, NULL, NULL, NULL) == XISFCONV_ERR_BUFFER,
              "a buffer that is too small");
        memset(vector_back, 0, sizeof vector_back);
        CHECK(at >= 0 && xisfconv_property_read(f, 0, (size_t)at, vector_back, sizeof vector_back, &size, NULL, NULL) == XISFCONV_OK &&
                  memcmp(vector_back, vector, sizeof vector) == 0,
              "its elements as 32-bit numbers");
        at = xisfconv_property_find(f, 0, "Lab:Table");
        memset(table_back, 0, sizeof table_back);
        CHECK(at >= 0 && xisfconv_property_read(f, 0, (size_t)at, table_back, sizeof table_back, &size, &rows, &columns) == XISFCONV_OK &&
                  size == sizeof table && rows == 2 && columns == 3 && memcmp(table_back, table, sizeof table) == 0,
              "a matrix of 16-bit integers, read in one call");
        at = xisfconv_property_find(f, 0, "Lab:Pairs");
        CHECK(at >= 0 && xisfconv_property_read(f, 0, (size_t)at, pairs_back, sizeof pairs_back, &size, &rows, &columns) == XISFCONV_OK &&
                  size == sizeof pairs && rows == 1 && columns == 2 && memcmp(pairs_back, pairs, sizeof pairs) == 0,
              "complex numbers");
        CHECK(xisfconv_property_read_f64(f, 0, "Lab:Pairs", NULL, 0, &rows, &columns) == XISFCONV_ERR_UNSUPPORTED,
              "which are not read as doubles");
        at = xisfconv_property_find(f, 0, "Lab:Bytes");
        CHECK(at >= 0 && xisfconv_property_read(f, 0, (size_t)at, bytes_back, sizeof bytes_back, &size, NULL, &columns) == XISFCONV_OK &&
                  size == 2 && columns == 2 && memcmp(bytes_back, bytes, 2) == 0,
              "bytes");
        at = xisfconv_property_find(f, 0, "Lab:Many");
        CHECK(at >= 0 && xisfconv_property_read(f, 0, (size_t)at, many_back, sizeof many_back, &size, &rows, &columns) == XISFCONV_OK &&
                  size == sizeof many && columns == MANY && memcmp(many_back, many, sizeof many) == 0,
              "a vector that is attached to the file");
        at = xisfconv_property_find(f, 0, "Lab:None");
        size = 5;
        CHECK(at >= 0 && xisfconv_property_read(f, 0, (size_t)at, many_back, sizeof many_back, &size, &rows, &columns) == XISFCONV_OK &&
                  size == 0 && columns == 0,
              "a vector of nothing");
        CHECK(xisfconv_property_read(f, 0, 0, many_back, sizeof many_back, &size, NULL, NULL) == XISFCONV_ERR_NOT_FOUND &&
                  xisfconv_property_read(f, 0, 9999, NULL, 0, &size, NULL, NULL) == XISFCONV_ERR_INDEX &&
                  xisfconv_property_read(f, 5, 0, NULL, 0, &size, NULL, NULL) == XISFCONV_ERR_INDEX &&
                  xisfconv_property_read(NULL, 0, 0, NULL, 0, &size, NULL, NULL) == XISFCONV_ERR_ARGUMENT,
              "what is not a vector or a matrix, and what is not there");

        /* the file: its own properties, the writer's, and how it is stored */
        CHECK((text = property_text(f, XISFCONV_FILE_PROPERTIES, "Note:Author", "String")) != NULL && strcmp(text, "somebody") == 0,
              "a property of the file");
        CHECK(xisfconv_property_find(f, XISFCONV_FILE_PROPERTIES, "XISF:CreatorOS") == -1 &&
                  (text = property_text(f, XISFCONV_FILE_PROPERTIES, "XISF:BlockAlignmentSize", "UInt16")) != NULL && strcmp(text, "4096") == 0,
              "the properties that describe the storage are the writer's own");
        CHECK((text = property_text(f, XISFCONV_FILE_PROPERTIES, "XISF:CreatorApplication", "String")) != NULL &&
                  strcmp(text, "capi test 1.0") == 0 &&
                  (text = property_text(f, XISFCONV_FILE_PROPERTIES, "XISF:CreatorModule", "String")) != NULL &&
                  strncmp(text, "xisfconv ", 9) == 0,
              "the program that wrote the file, and the library");
        CHECK((text = property_text(f, XISFCONV_FILE_PROPERTIES, "XISF:CompressionCodecs", "String")) != NULL && strcmp(text, "lz4hc") == 0 &&
                  xisfconv_property_find(f, XISFCONV_FILE_PROPERTIES, "XISF:CompressionLevel") == -1,
              "the codec; its level is not named (XISF:CompressionLevel is no level of a codec)");
        CHECK(strncmp(xisfconv_image_detail(f, 0, "compression"), "lz4hc:", 6) == 0, "LZ4HC without byte shuffling");
        xisfconv_close(f);
        f = NULL;
    }
    CHECK(smooth_comes_back(ctx, path_of("given.xisf")), "the pixels come back from LZ4HC");

    /* the same to FITS and to ASDF, and from there to XISF */
    for (k = 0; k < 2; ++k) {
        const char *carrier = path_of(k ? "given.asdf" : "given.fits");
        xisfconv_write_options_init(&wo, sizeof wo);
        wo.overwrite = 1;
        wo.properties = whole;
        wo.compression_level = 99;   /* (of XISF: means nothing here) */
        CHECK(write_smooth(ctx, carrier, &wo, NULL, own) == XISFCONV_OK, k ? "to ASDF" : "to FITS");
        f = NULL;
        CHECK(xisfconv_open(ctx, carrier, &f) == XISFCONV_OK && f && xisfconv_property_count(f, 0) == count &&
                  xisfconv_property_count(f, XISFCONV_FILE_PROPERTIES) == 1,
              "the file carries them");
        if (f) {
            at = xisfconv_property_find(f, 0, "Lab:Table");
            memset(table_back, 0, sizeof table_back);
            CHECK(at >= 0 && xisfconv_property_read(f, 0, (size_t)at, table_back, sizeof table_back, &size, &rows, &columns) == XISFCONV_OK &&
                      rows == 2 && columns == 3 && memcmp(table_back, table, sizeof table) == 0,
                  "a matrix from there");
            CHECK(at >= 0 && xisfconv_property_stored(f, 0, (size_t)at) == XISFCONV_PROPERTY_ARRAY &&
                      xisfconv_property_stored(f, 0, (size_t)xisfconv_property_find(f, 0, "Lab:Count")) == XISFCONV_PROPERTY_VALUE &&
                      xisfconv_property_stored(f, 0, 9999) == XISFCONV_PROPERTY_NONE,
                  "stored as an array there too");
            CHECK(xisfconv_property_read(f, 0, 0, NULL, 0, &size, NULL, NULL) == XISFCONV_ERR_NOT_FOUND &&
                      xisfconv_property_read(f, 0, 9999, NULL, 0, &size, NULL, NULL) == XISFCONV_ERR_INDEX,
                  "and what is not one");
            xisfconv_close(f);
        }
        xisfconv_convert_options_init(&co, sizeof co);
        co.overwrite = 1;
        co.codec = XISFCONV_CODEC_LZ4;
        CHECK(xisfconv_convert(ctx, carrier, path_of("given-back.xisf"), &co) == XISFCONV_OK, "and back to XISF, with LZ4");
        f = NULL;
        CHECK(xisfconv_open(ctx, path_of("given-back.xisf"), &f) == XISFCONV_OK && f && xisfconv_property_count(f, 0) == count &&
                  (text = property_text(f, 0, "Lab:When", "TimePoint")) != NULL && strcmp(text, "2026-10-06T18:30:00.250Z") == 0 &&
                  (text = property_text(f, XISFCONV_FILE_PROPERTIES, "Note:Author", "String")) != NULL && strcmp(text, "somebody") == 0 &&
                  strncmp(xisfconv_image_detail(f, 0, "compression"), "lz4+sh:", 7) == 0,
              "they are the properties again");
        xisfconv_close(f);
        CHECK(smooth_comes_back(ctx, path_of("given-back.xisf")), "the pixels come back from LZ4");
    }

    /* an astrometric solution: made from WCS keywords unless the caller brings one */
    xisfconv_keywords_new(ctx, &kw);
    xisfconv_keywords_append_string(kw, "CTYPE1", "RA---TAN", NULL);
    xisfconv_keywords_append_string(kw, "CTYPE2", "DEC--TAN", NULL);
    xisfconv_keywords_append_number(kw, "CRVAL1", 10.5, NULL);
    xisfconv_keywords_append_number(kw, "CRVAL2", 41.25, NULL);
    xisfconv_keywords_append_number(kw, "CRPIX1", 4.0, NULL);
    xisfconv_keywords_append_number(kw, "CRPIX2", 2.0, NULL);
    xisfconv_keywords_append_number(kw, "CD1_1", -0.0003, NULL);
    xisfconv_keywords_append_number(kw, "CD1_2", 0.00001, NULL);
    xisfconv_keywords_append_number(kw, "CD2_1", 0.00002, NULL);
    xisfconv_keywords_append_number(kw, "CD2_2", 0.0003, NULL);
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.overwrite = 1;
    CHECK(write_smooth(ctx, path_of("made.xisf"), &wo, kw, own) == XISFCONV_OK, "properties and WCS keywords");
    f = NULL;
    if (xisfconv_open(ctx, path_of("made.xisf"), &f) == XISFCONV_OK && f) {
        CHECK((text = property_text(f, 0, "PCL:AstrometricSolution:ProjectionSystem", "String")) != NULL && strcmp(text, "Gnomonic") == 0 &&
                  xisfconv_property_count(f, 0) > count && property_text(f, 0, "Lab:Gain", "Float32") != NULL,
              "the solution is made from the keywords, and the properties are written with it");
        xisfconv_close(f);
    } else {
        CHECK(0, "open it");
    }
    wo.wcs = 0;
    CHECK(write_smooth(ctx, path_of("made.xisf"), &wo, kw, own) == XISFCONV_OK, "the same without a solution");
    f = NULL;
    if (xisfconv_open(ctx, path_of("made.xisf"), &f) == XISFCONV_OK && f) {
        CHECK(xisfconv_property_count(f, 0) == count, "the properties alone");
        xisfconv_close(f);
    }
    xisfconv_properties_new(ctx, &solved);
    xisfconv_properties_set(solved, "PCL:AstrometricSolution:ProjectionSystem", "String", "Mercator", NULL, NULL);
    xisfconv_properties_set(solved, "Lab:Gain", "Float32", "0.5", NULL, NULL);
    wo.wcs = 1;
    CHECK(write_smooth(ctx, path_of("brought.xisf"), &wo, kw, solved) == XISFCONV_OK, "a solution among the properties");
    f = NULL;
    if (xisfconv_open(ctx, path_of("brought.xisf"), &f) == XISFCONV_OK && f) {
        CHECK(xisfconv_property_count(f, 0) == 2 &&
                  (text = property_text(f, 0, "PCL:AstrometricSolution:ProjectionSystem", "String")) != NULL && strcmp(text, "Mercator") == 0,
              "is written as it is given, and none is made");
        xisfconv_close(f);
    }
    /* through FITS it stays the solution of these keywords */
    CHECK(write_smooth(ctx, path_of("brought.fits"), &wo, kw, solved) == XISFCONV_OK, "the same to FITS");
    xisfconv_convert_options_init(&co, sizeof co);
    co.overwrite = 1;
    CHECK(xisfconv_convert(ctx, path_of("brought.fits"), path_of("brought-back.xisf"), &co) == XISFCONV_OK, "and to XISF");
    f = NULL;
    if (xisfconv_open(ctx, path_of("brought-back.xisf"), &f) == XISFCONV_OK && f) {
        CHECK(xisfconv_property_count(f, 0) == 2 &&
                  (text = property_text(f, 0, "PCL:AstrometricSolution:ProjectionSystem", "String")) != NULL && strcmp(text, "Mercator") == 0,
              "the solution the caller brought comes back");
        xisfconv_close(f);
    }
    CHECK(smooth_comes_back(ctx, path_of("brought-back.xisf")), "with the pixels");
    f = NULL;
    if (xisfconv_open(ctx, path_of("brought.fits"), &f) == XISFCONV_OK && f) {
        CHECK(strcmp(xisfconv_image_detail(f, 0, "carriedSolution"), "current") == 0, "the FITS file says the solution is that of its keywords");
        xisfconv_close(f);
    }
    f = NULL;
    if (xisfconv_open(ctx, path_of("brought-back.xisf"), &f) == XISFCONV_OK && f) {
        CHECK(strcmp(xisfconv_image_detail(f, 0, "carriedSolution"), "") == 0, "an XISF file carries none: it has it");
        xisfconv_close(f);
    }
    /* the keywords are changed by another program: the solution is no longer theirs */
    CHECK(change_in_file(path_of("brought.fits"), "CRVAL1  =                 10.5", "CRVAL1  =                 11.5"),
          "another reference value");
    f = NULL;
    if (xisfconv_open(ctx, path_of("brought.fits"), &f) == XISFCONV_OK && f) {
        CHECK(strcmp(xisfconv_image_detail(f, 0, "carriedSolution"), "stale") == 0 &&
                  (text = property_text(f, 0, "PCL:AstrometricSolution:ProjectionSystem", "String")) != NULL && strcmp(text, "Mercator") == 0,
              "the solution is stale, and still there to be read");
        xisfconv_close(f);
    }
    CHECK(xisfconv_convert(ctx, path_of("brought.fits"), path_of("brought-back.xisf"), &co) == XISFCONV_OK, "to XISF again");
    f = NULL;
    if (xisfconv_open(ctx, path_of("brought-back.xisf"), &f) == XISFCONV_OK && f) {
        CHECK((text = property_text(f, 0, "PCL:AstrometricSolution:ProjectionSystem", "String")) != NULL && strcmp(text, "Gnomonic") == 0 &&
                  (text = property_text(f, 0, "Lab:Gain", "Float32")) != NULL && strcmp(text, "0.5") == 0,
              "the solution is made from the keywords as they are now, and the other properties stay");
        xisfconv_close(f);
    }
    /* without one, a conversion of the FITS file makes it from the keywords */
    CHECK(write_smooth(ctx, path_of("plainly.fits"), &wo, kw, own) == XISFCONV_OK &&
              xisfconv_convert(ctx, path_of("plainly.fits"), path_of("plainly-back.xisf"), &co) == XISFCONV_OK,
          "properties without a solution, through FITS");
    f = NULL;
    if (xisfconv_open(ctx, path_of("plainly-back.xisf"), &f) == XISFCONV_OK && f) {
        CHECK((text = property_text(f, 0, "PCL:AstrometricSolution:ProjectionSystem", "String")) != NULL && strcmp(text, "Gnomonic") == 0 &&
                  property_text(f, 0, "Lab:Gain", "Float32") != NULL,
              "get a solution from the keywords there");
        xisfconv_close(f);
    }
    f = NULL;
    if (xisfconv_open(ctx, path_of("plainly.fits"), &f) == XISFCONV_OK && f) {
        CHECK(strcmp(xisfconv_image_detail(f, 0, "carriedSolution"), "") == 0, "properties without a solution carry none");
        xisfconv_close(f);
    }
    {
        /* the digest a program compares for itself */
        char first[200];
        const char *digest = NULL;
        CHECK(xisfconv_wcs_digest(kw, BW, BH, XISFCONV_ROWS_DEFAULT, &digest) == XISFCONV_OK && digest && strlen(digest) > 8 &&
                  strlen(digest) < sizeof first,
              "the digest of WCS keywords");
        first[0] = 0;
        if (digest && strlen(digest) < sizeof first) strcpy(first, digest);
        CHECK(xisfconv_wcs_digest(kw, BW, BH, XISFCONV_ROWS_BOTTOM_UP, &digest) == XISFCONV_OK && strcmp(digest, first) == 0,
              "bottom-up is the usual order");
        CHECK(xisfconv_wcs_digest(kw, BW, BH, XISFCONV_ROWS_TOP_DOWN, &digest) == XISFCONV_OK && strcmp(digest, first) != 0 &&
                  xisfconv_wcs_digest(kw, BW + 1, BH, XISFCONV_ROWS_DEFAULT, &digest) == XISFCONV_OK && strcmp(digest, first) != 0 &&
                  xisfconv_wcs_digest(kw, BW, BH - 1, XISFCONV_ROWS_DEFAULT, &digest) == XISFCONV_OK && strcmp(digest, first) != 0,
              "another row order or size is another digest");
        xisfconv_keywords_append_string(kw, "OBJECT", "M 31", "not of the WCS");
        CHECK(xisfconv_wcs_digest(kw, BW, BH, XISFCONV_ROWS_DEFAULT, &digest) == XISFCONV_OK && strcmp(digest, first) == 0,
              "a keyword that says nothing of the WCS does not change it");
        xisfconv_keywords_append_number(kw, "CROTA2", 1.0, NULL);
        CHECK(xisfconv_wcs_digest(kw, BW, BH, XISFCONV_ROWS_DEFAULT, &digest) == XISFCONV_OK && strcmp(digest, first) != 0,
              "one that does, does");
        CHECK(xisfconv_wcs_digest(NULL, BW, BH, XISFCONV_ROWS_DEFAULT, &digest) == XISFCONV_ERR_ARGUMENT &&
                  xisfconv_wcs_digest(kw, BW, BH, XISFCONV_ROWS_DEFAULT, NULL) == XISFCONV_ERR_ARGUMENT,
              "what has no digest");
    }
    xisfconv_keywords_free(kw);
    xisfconv_properties_free(solved);

    /* text of keywords beyond ASCII: XISF keeps it, a FITS card has no place for it */
    kw = NULL;
    xisfconv_keywords_new(ctx, &kw);
    CHECK(xisfconv_keywords_append_string(kw, "OBSERVER", "J\xC3\xBCrgen", "at 20\xC2\xB0") == XISFCONV_OK, "a name with an umlaut");
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.overwrite = 1;
    CHECK(write_smooth(ctx, path_of("umlaut.xisf"), &wo, kw, NULL) == XISFCONV_OK &&
              (text = header_of(path_of("umlaut.xisf"))) != NULL && strstr(text, "J\xC3\xBCrgen") && strstr(text, "at 20\xC2\xB0\""),
          "is written to XISF as it is");
    CHECK(write_smooth(ctx, path_of("umlaut.fits"), &wo, kw, NULL) == XISFCONV_OK, "and to FITS");
    f = NULL;
    if (xisfconv_open(ctx, path_of("umlaut.fits"), &f) == XISFCONV_OK && f) {
        const xisfconv_keywords *have = NULL;
        int64_t where = -1;
        if (xisfconv_image_keywords(f, 0, &have) == XISFCONV_OK) where = xisfconv_keywords_find(have, "OBSERVER");
        text = NULL;
        CHECK(where >= 0 && xisfconv_keywords_get_text(have, (size_t)where, &text) == XISFCONV_OK && text && strcmp(text, "J?rgen") == 0,
              "where each character that is not ASCII is a question mark");
        xisfconv_close(f);
    } else {
        CHECK(0, "open it");
    }
    xisfconv_keywords_free(kw);

    /* the options of the writer */
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.overwrite = 1;
    wo.codec = XISFCONV_CODEC_ZLIB;
    wo.compression_level = 10;
    CHECK(xisfconv_writer_new(ctx, path_of("level.xisf"), &wo, &w) == XISFCONV_ERR_ARGUMENT && w == NULL &&
              strstr(xisfconv_error_message(ctx), "1 to 9"),
          "a level zlib does not have");
    wo.compression_level = -1;
    CHECK(xisfconv_writer_new(ctx, path_of("level.xisf"), &wo, &w) == XISFCONV_ERR_ARGUMENT, "a level below the first");
    wo.codec = XISFCONV_CODEC_LZ4;
    wo.compression_level = 3;
    CHECK(xisfconv_writer_new(ctx, path_of("level.xisf"), &wo, &w) == XISFCONV_ERR_ARGUMENT, "LZ4 has no levels");
    wo.codec = XISFCONV_CODEC_NONE;
    CHECK(xisfconv_writer_new(ctx, path_of("level.xisf"), &wo, &w) == XISFCONV_ERR_ARGUMENT, "a level without a codec");
    wo.codec = XISFCONV_CODEC_LZ4HC;
    wo.compression_level = 13;
    CHECK(xisfconv_writer_new(ctx, path_of("level.xisf"), &wo, &w) == XISFCONV_ERR_ARGUMENT, "a level LZ4HC does not have");
    wo.compression_level = 1;
    wo.creator_application = "two\nlines";
    CHECK(xisfconv_writer_new(ctx, path_of("level.xisf"), &wo, &w) == XISFCONV_ERR_ARGUMENT && w == NULL, "a name of two lines is not one");
    wo.creator_application = "caf\xFF";
    CHECK(xisfconv_writer_new(ctx, path_of("level.xisf"), &wo, &w) == XISFCONV_ERR_ARGUMENT && w == NULL, "nor is one that is not UTF-8");
    wo.creator_application = "   ";
    CHECK(write_smooth(ctx, path_of("level.xisf"), &wo, NULL, NULL) == XISFCONV_OK && smooth_comes_back(ctx, path_of("level.xisf")),
          "LZ4HC at its first level, and a name of blanks is none");
    f = NULL;
    if (xisfconv_open(ctx, path_of("level.xisf"), &f) == XISFCONV_OK && f) {
        CHECK((text = property_text(f, XISFCONV_FILE_PROPERTIES, "XISF:CreatorApplication", "String")) != NULL &&
                  strncmp(text, "xisfconv ", 9) == 0 && xisfconv_property_find(f, XISFCONV_FILE_PROPERTIES, "XISF:CreatorModule") == -1 &&
                  strncmp(xisfconv_image_detail(f, 0, "compression"), "lz4hc+sh:", 9) == 0,
              "the library names itself, and shuffles the bytes");
        xisfconv_close(f);
    }
    CHECK(file_exists(path_of("level.xisf")) && !file_exists(path_of("level.xisf.part")), "no temporary file is left");
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.overwrite = 1;
    wo.codec = XISFCONV_CODEC_LZ4;
    CHECK(write_smooth(ctx, path_of("lz4.fits"), &wo, NULL, NULL) == XISFCONV_ERR_ARGUMENT &&
              write_smooth(ctx, path_of("lz4.asdf"), &wo, NULL, NULL) == XISFCONV_ERR_ARGUMENT && !file_exists(path_of("lz4.asdf")),
          "LZ4 is for XISF");

    /* rewriting with LZ4 */
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.overwrite = 1;
    CHECK(write_smooth(ctx, path_of("plain-smooth.xisf"), &wo, NULL, own) == XISFCONV_OK, "an uncompressed file");
    xisfconv_rewrite_options_init(&ro, sizeof ro);
    xisfconv_rewrite_result_init(&rr, sizeof rr);
    ro.overwrite = 1;
    for (k = 0; k < 2; ++k) {
        int32_t same = -1;
        ro.codec = k ? XISFCONV_CODEC_LZ4HC : XISFCONV_CODEC_LZ4;
        CHECK(xisfconv_rewrite(ctx, path_of("plain-smooth.xisf"), path_of("rewritten.xisf"), &ro, &rr) == XISFCONV_OK && rr.changed == 1 &&
                  rr.compressed >= 1 && rr.read_back == 1 && rr.output_size < rr.input_size,
              k ? "rewritten with LZ4HC" : "rewritten with LZ4");
        CHECK(xisfconv_stored_as_requested(ctx, path_of("rewritten.xisf"), &ro, &same) == XISFCONV_OK && same == 1 &&
                  smooth_comes_back(ctx, path_of("rewritten.xisf")),
              "and stored that way");
    }

    /* a program built against 0.14: its structs end where they ended then */
    memset(&older, 0xEE, sizeof older);
    xisfconv_image_init(&older.image, offsetof(xisfconv_image, reserved));
    CHECK(older.image.struct_size == offsetof(xisfconv_image, reserved) && older.raw[offsetof(xisfconv_image, reserved)] == 0xEE,
          "an image of the size of 0.14");
    older.image.struct_size = offsetof(xisfconv_image, properties);   /* (with its padding, as sizeof gave it) */
    older.image.pixels = g_smooth;
    older.image.width = BW;
    older.image.height = BH;
    older.image.sample_format = XISFCONV_SAMPLE_UINT16;
    memset(&older_options, 0xEE, sizeof older_options);
    xisfconv_write_options_init(&older_options.options, offsetof(xisfconv_write_options, shuffle));
    older_options.options.overwrite = 1;
    older_options.options.codec = XISFCONV_CODEC_ZLIB;
    w = NULL;
    CHECK(xisfconv_writer_new(ctx, path_of("older.xisf"), &older_options.options, &w) == XISFCONV_OK && w &&
              xisfconv_writer_add_image(w, &older.image) == XISFCONV_OK && xisfconv_writer_finish(w) == XISFCONV_OK &&
              smooth_comes_back(ctx, path_of("older.xisf")),
          "is written, and what lies behind its structs is not looked at");
    f = NULL;
    if (xisfconv_open(ctx, path_of("older.xisf"), &f) == XISFCONV_OK && f) {
        CHECK(xisfconv_property_count(f, 0) == 0 && strncmp(xisfconv_image_detail(f, 0, "compression"), "zlib+sh:", 8) == 0,
              "without properties, with the bytes shuffled");
        xisfconv_close(f);
    }

    xisfconv_properties_free(own);
    xisfconv_properties_free(whole);
}

static void test_fits_header(xisfconv_context *ctx) {
    xisfconv_keywords *kw = NULL, *out = NULL;
    const xisfconv_keywords *stored = NULL;
    xisfconv_file *f = (xisfconv_file *)1;
    xisfconv_writer *w = NULL;
    xisfconv_image img;
    const char *text = NULL, *summary = NULL, *value = NULL;
    size_t length = 99;
    char long_text[151];
    char xisf[1024], fits[1024];
    strcpy(xisf, path_of("header.xisf"));
    strcpy(fits, path_of("header.fits"));
    memset(long_text, 'x', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = 0;

    CHECK(xisfconv_keywords_new(ctx, &kw) == XISFCONV_OK && kw, "a list for the header tests");
    if (!kw) return;
    CHECK(xisfconv_keywords_fits_text(kw, &text, &length) == XISFCONV_OK && text && *text == 0 && length == 0,
          "an empty list is an empty header");
    xisfconv_keywords_append(kw, "SIMPLE", "T", NULL);
    xisfconv_keywords_append(kw, "BITPIX", "16", NULL);
    xisfconv_keywords_append(kw, "NAXIS", "2", NULL);
    xisfconv_keywords_append(kw, "NAXIS1", "5", NULL);
    xisfconv_keywords_append(kw, "EXTEND", "T", NULL);
    xisfconv_keywords_append(kw, "BZERO", "32768", NULL);
    xisfconv_keywords_append(kw, "BSCALE", "1", NULL);
    xisfconv_keywords_append_string(kw, "ROWORDER", "TOP-DOWN", NULL);
    xisfconv_keywords_append_string(kw, "BAYERPAT", "RGGB", "the filter pattern");
    xisfconv_keywords_append_string(kw, "OBJECT", "M 1", NULL);
    xisfconv_keywords_append_string(kw, "LONGTEXT", long_text, NULL);
    xisfconv_keywords_append_number(kw, "Long Keyword Name", 1.5, NULL);
    xisfconv_keywords_append(kw, "HISTORY", NULL, "made by the test");
    CHECK(xisfconv_keywords_count(kw) == 13, "thirteen cards");

    CHECK(xisfconv_keywords_fits_text(kw, &text, &length) == XISFCONV_OK && length > 0 && length % 80 == 0 &&
              strlen(text) == length,
          "the cards as text: 80 characters each");
    CHECK(!strstr(text, "SIMPLE") && !strstr(text, "BITPIX") && !strstr(text, "NAXIS") && !strstr(text, "EXTEND") &&
              !strstr(text, "BZERO") && !strstr(text, "BSCALE") && !strstr(text, "ROWORDER"),
          "without the cards on how a FITS file stores its data");
    CHECK(strncmp(text, "LONGSTRN= 'OGIP 1.0'", 20) == 0 && strstr(text, "CONTINUE  '"), "a long string continues, and says so first");
    CHECK(strstr(text, "BAYERPAT= 'RGGB    ' / the filter pattern") && strstr(text, "HIERARCH Long Keyword Name = ") &&
              strstr(text, " 1.5 ") && strstr(text, "HISTORY made by the test"),
          "value, comment, HIERARCH and HISTORY cards");
    CHECK(strlen(text) == length && length == 80 * 8, "eight cards: LONGSTRN, BAYERPAT, OBJECT, LONGTEXT on three, the name, HISTORY");
    CHECK(xisfconv_keywords_fits_text(kw, NULL, &length) == XISFCONV_ERR_ARGUMENT && length == 0, "the text needs a place");
    CHECK(xisfconv_keywords_fits_text(NULL, &text, &length) == XISFCONV_ERR_ARGUMENT, "and a list");
    CHECK(xisfconv_keywords_fits_text(kw, &text, NULL) == XISFCONV_OK && strlen(text) == 80 * 8, "the length is optional");

    /* The writer leaves the same cards out, whatever the format. */
    /* (four rows: an even number, so that the filter pattern is another one seen from the bottom) */
    xisfconv_image_init(&img, sizeof img);
    img.pixels = g_gray;
    img.width = W;
    img.height = 4;
    img.channels = 1;
    img.sample_format = XISFCONV_SAMPLE_UINT16;
    img.row_order = XISFCONV_ROWS_TOP_DOWN;
    img.keywords = kw;
    CHECK(xisfconv_writer_new(ctx, xisf, NULL, &w) == XISFCONV_OK && xisfconv_writer_add_image(w, &img) == XISFCONV_OK &&
              xisfconv_writer_finish(w) == XISFCONV_OK,
          "an XISF file from a header that came from FITS");
    CHECK(xisfconv_open(ctx, xisf, &f) == XISFCONV_OK && f, "open it");
    if (f) {
        CHECK(xisfconv_image_keywords(f, 0, &stored) == XISFCONV_OK && xisfconv_keywords_count(stored) == 5 &&
                  xisfconv_keywords_find(stored, "SIMPLE") == -1 && xisfconv_keywords_find(stored, "BZERO") == -1 &&
                  xisfconv_keywords_find(stored, "NAXIS1") == -1 && xisfconv_keywords_find(stored, "ROWORDER") == -1 &&
                  xisfconv_keywords_find(stored, "OBJECT") == 1,
              "it holds the five cards that describe the image");

        /* the header for FITS: BAYERPAT follows the rows */
        summary = "x";
        CHECK(xisfconv_fits_keywords(f, 0, XISFCONV_ROWS_BOTTOM_UP, 1, 1, 3, &out, &summary) == XISFCONV_OK && out &&
                  summary && *summary == 0,
              "the header for FITS, rows bottom-up");
        CHECK(out && xisfconv_keywords_get_text(out, (size_t)xisfconv_keywords_find(out, "BAYERPAT"), &value) == XISFCONV_OK &&
                  strcmp(value, "GBRG") == 0 && xisfconv_keywords_count(out) == 5,
              "BAYERPAT is turned over with the rows");
        xisfconv_keywords_free(out);
        out = NULL;
        CHECK(xisfconv_fits_keywords(f, 0, XISFCONV_ROWS_DEFAULT, 0, 0, 0, &out, NULL) == XISFCONV_OK && out &&
                  xisfconv_keywords_get_text(out, (size_t)xisfconv_keywords_find(out, "BAYERPAT"), &value) == XISFCONV_OK &&
                  strcmp(value, "GBRG") == 0,
              "bottom-up is the default, and the summary is optional");
        CHECK(out && xisfconv_keywords_append(out, "MINE", "1", NULL) == XISFCONV_OK, "the list is the caller's");
        xisfconv_keywords_free(out);
        out = NULL;
        CHECK(xisfconv_fits_keywords(f, 0, XISFCONV_ROWS_TOP_DOWN, 1, 1, 3, &out, NULL) == XISFCONV_OK && out &&
                  xisfconv_keywords_get_text(out, (size_t)xisfconv_keywords_find(out, "BAYERPAT"), &value) == XISFCONV_OK &&
                  strcmp(value, "RGGB") == 0,
              "top-down, as XISF stores them, it stays");
        xisfconv_keywords_free(out);
        out = (xisfconv_keywords *)1;
        CHECK(xisfconv_fits_keywords(f, 5, XISFCONV_ROWS_DEFAULT, 1, 1, 3, &out, NULL) == XISFCONV_ERR_INDEX && out == NULL,
              "no such image");
        CHECK(xisfconv_fits_keywords(f, 0, XISFCONV_ROWS_DEFAULT, 1, 1, 3, NULL, NULL) == XISFCONV_ERR_ARGUMENT, "no place for the list");
        CHECK(xisfconv_fits_keywords(f, 0, XISFCONV_ROWS_DEFAULT, 1, 1, 9, &out, NULL) == XISFCONV_ERR_ARGUMENT, "a SIP order out of range");
        CHECK(xisfconv_fits_keywords(f, 0, 7, 1, 1, 3, &out, NULL) == XISFCONV_ERR_ARGUMENT, "a row order that does not exist");
        CHECK(xisfconv_fits_keywords(NULL, 0, XISFCONV_ROWS_DEFAULT, 1, 1, 3, &out, NULL) == XISFCONV_ERR_ARGUMENT, "no file");
        xisfconv_close(f);
        f = NULL;
    }

    /* A FITS file stores the rows bottom-up: there the pattern is turned, and comes back. */
    CHECK(xisfconv_convert(ctx, xisf, fits, NULL) == XISFCONV_OK, "the same as FITS");
    CHECK(xisfconv_open(ctx, fits, &f) == XISFCONV_OK && f, "open it");
    if (f) {
        out = NULL;
        CHECK(xisfconv_image_keywords(f, 0, &stored) == XISFCONV_OK &&
                  xisfconv_keywords_get_text(stored, (size_t)xisfconv_keywords_find(stored, "BAYERPAT"), &value) == XISFCONV_OK &&
                  strcmp(value, "GBRG") == 0,
              "the FITS file holds the pattern of its bottom-up rows");
        CHECK(xisfconv_fits_keywords(f, 0, XISFCONV_ROWS_TOP_DOWN, 1, 1, 3, &out, NULL) == XISFCONV_OK && out &&
                  xisfconv_keywords_get_text(out, (size_t)xisfconv_keywords_find(out, "BAYERPAT"), &value) == XISFCONV_OK &&
                  strcmp(value, "RGGB") == 0,
              "for top-down rows it is turned back");
        xisfconv_keywords_free(out);
        out = NULL;
        CHECK(xisfconv_fits_keywords(f, 0, XISFCONV_ROWS_BOTTOM_UP, 1, 1, 3, &out, NULL) == XISFCONV_OK && out &&
                  xisfconv_keywords_get_text(out, (size_t)xisfconv_keywords_find(out, "BAYERPAT"), &value) == XISFCONV_OK &&
                  strcmp(value, "GBRG") == 0,
              "for bottom-up rows it is as stored");
        xisfconv_keywords_free(out);
        xisfconv_close(f);
    }
    xisfconv_keywords_free(kw);

    f = (xisfconv_file *)1;
    CHECK(xisfconv_open(ctx, g_dir, &f) == XISFCONV_ERR_IO && f == NULL && strstr(xisfconv_error_message(ctx), "directory"),
          "a directory is not a file");
    CHECK(xisfconv_convert(ctx, g_dir, path_of("from-directory.fits"), NULL) == XISFCONV_ERR_IO &&
              strstr(xisfconv_error_message(ctx), "directory"),
          "nor an input of a conversion");
    /* an input that is not there is that, whatever the names say about the formats */
    strcpy(xisf, path_of("missing-input.fits"));
    CHECK(xisfconv_convert(ctx, xisf, path_of("never.xisf"), NULL) == XISFCONV_ERR_IO, "a missing input is an I/O error");
    /* a directory of the output's name is not replaced by the output */
    CHECK(write_gray(ctx, g_dir, NULL, NULL, NULL, 0) != XISFCONV_OK, "a directory is not an output (no format in its name)");

    /* a card without a keyword name: text only */
    kw = NULL;
    CHECK(xisfconv_keywords_new(ctx, &kw) == XISFCONV_OK && xisfconv_keywords_append(kw, "", NULL, "text only") == XISFCONV_OK &&
              xisfconv_keywords_append(kw, NULL, NULL, "x") == XISFCONV_ERR_ARGUMENT && xisfconv_keywords_count(kw) == 1 &&
              xisfconv_keywords_fits_text(kw, &text, &length) == XISFCONV_OK && length == 80 &&
              strncmp(text, "        text only", 17) == 0,
          "a card of text only has an empty name");
    xisfconv_keywords_free(kw);
}

/* Distributed XISF units (0.16): a header file (.xish) and the files it names. */

static int file_begins(const char *path, const char *with) {
    char first[64] = {0};
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(first, 1, sizeof first - 1, f);
    fclose(f);
    return n >= strlen(with) && memcmp(first, with, strlen(with)) == 0;
}

static int write_bytes(const char *path, const void *bytes, size_t size) {
    FILE *f = fopen(path, "wb");
    size_t n;
    if (!f) return 0;
    n = fwrite(bytes, 1, size, f);
    return fclose(f) == 0 && n == size;
}

/* A header file for the gray image, with its pixels at `location`. */
static int write_header(const char *path, const char *location) {
    char text[2000];
    sprintf(text,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xisf version=\"1.0\" xmlns=\"http://www.pixinsight.com/xisf\">"
            "<Image geometry=\"%d:%d:1\" sampleFormat=\"UInt16\" colorSpace=\"Gray\" location=\"%s\"/></xisf>\n",
            W, H, location);
    return write_bytes(path, text, strlen(text));
}

/* The pixels of the unit at `path`, held against the gray image. */
static int unit_is_gray(xisfconv_context *ctx, const char *path, const char *unit) {
    xisfconv_file *f = NULL;
    uint16_t back[W * H];
    int ok = xisfconv_open(ctx, path, &f) == XISFCONV_OK;
    ok = ok && xisfconv_file_format(f) == XISFCONV_FORMAT_XISF && strcmp(xisfconv_file_detail(f, "unit"), unit) == 0;
    ok = ok && xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_OK && rows_equal(back, g_gray, 0);
    xisfconv_close(f);
    return ok;
}

static void test_distributed_units(xisfconv_context *ctx) {
    xisfconv_write_options wo;
    xisfconv_convert_options co;
    xisfconv_rewrite_options ro;
    xisfconv_rewrite_result rr;
    xisfconv_report *report = NULL;
    xisfconv_format format = 0;
    xisfconv_file *f = NULL;
    xisfconv_status st;
    unsigned char little[2 * W * H];
    uint16_t back[W * H];
    char location[1400];
    const char *slash, *name;
    int32_t same = 0;
    long header_size, blocks_size;
    int i;

    /* the setting of a context */
    CHECK(xisfconv_context_external_files(ctx) == XISFCONV_EXTERNAL_HEADER_DIRECTORY && xisfconv_context_external_files(NULL) == 0,
          "a header is followed to its own directory unless something else is asked for");
    CHECK(xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_NONE) == XISFCONV_OK &&
              xisfconv_context_external_files(ctx) == XISFCONV_EXTERNAL_NONE,
          "which can be set");
    CHECK(xisfconv_context_set_external_files(ctx, 3) == XISFCONV_ERR_ARGUMENT && xisfconv_context_set_external_files(ctx, -1) == XISFCONV_ERR_ARGUMENT &&
              strstr(xisfconv_error_message(ctx), "XISFCONV_EXTERNAL_") && xisfconv_context_external_files(ctx) == XISFCONV_EXTERNAL_NONE,
          "to one of three values; another one changes nothing");
    CHECK(xisfconv_context_set_external_files(NULL, XISFCONV_EXTERNAL_ANYWHERE) == XISFCONV_ERR_ARGUMENT, "not without a context");
    CHECK(*xisfconv_status_text(XISFCONV_ERR_NOT_ALLOWED) != 0 && strcmp(xisfconv_status_text(XISFCONV_ERR_NOT_ALLOWED), xisfconv_status_text(12345)) != 0,
          "the status of a file a header is not followed to has a text");

    /* written: a header file and a data blocks file. (No file but the header is to be read: what is
       written is read back all the same, the setting is about what a header sends the reader to.) */
    xisfconv_write_options_init(&wo, sizeof wo);
    wo.codec = XISFCONV_CODEC_ZLIB;
    wo.checksum = XISFCONV_CHECKSUM_SHA256;
    st = write_gray(ctx, path_of("unit.xish"), &wo, NULL, NULL, 0);
    CHECK(st == XISFCONV_OK, "an XISF unit written under the name of a header file");
    if (st != XISFCONV_OK) fprintf(stderr, "  %s\n", xisfconv_error_message(ctx));
    CHECK(file_begins(path_of("unit.xish"), "<?xml version=\"1.0\" encoding=\"UTF-8\"?>") && file_begins(path_of("unit.xisb"), "XISB0100"),
          "is a header file, which is XML, and a data blocks file");
    CHECK(!file_exists(path_of("unit.xish.part")) && !file_exists(path_of("unit.xisb.part")), "no temporary files are left");
    CHECK(xisfconv_detect_format(ctx, path_of("unit.xish"), &format) == XISFCONV_OK && format == XISFCONV_FORMAT_XISF, "a header file is XISF");
    CHECK(xisfconv_detect_format(ctx, path_of("unit.xisb"), &format) == XISFCONV_ERR_FORMAT && strstr(xisfconv_error_message(ctx), ".xish"),
          "a data blocks file is no file to open: the error names the header file");
    CHECK(xisfconv_open(ctx, path_of("unit.xisb"), &f) == XISFCONV_ERR_FORMAT && f == NULL && strstr(xisfconv_error_message(ctx), ".xish"),
          "nor does it open");
    header_size = file_size(path_of("unit.xish"));
    blocks_size = file_size(path_of("unit.xisb"));

    /* read: with no file but the header, the header and what it holds */
    st = xisfconv_open(ctx, path_of("unit.xish"), &f);
    CHECK(st == XISFCONV_OK && f != NULL, "the header file opens, whatever it may be followed to");
    if (st == XISFCONV_OK) {
        xisfconv_image_info info;
        xisfconv_image_info_init(&info, sizeof info);
        CHECK(strcmp(xisfconv_file_detail(f, "unit"), "distributed") == 0 && strcmp(xisfconv_file_detail(f, "version"), "1.0") == 0,
              "it is a distributed unit");
        CHECK(xisfconv_image_info_get(f, 0, &info) == XISFCONV_OK && info.width == W && info.height == H, "the image is described by the header");
        CHECK(xisfconv_external_count(f) == 1 && (name = xisfconv_external_file(f, 0)) != NULL && strlen(name) > 9 &&
                  strcmp(name + strlen(name) - 9, "unit.xisb") == 0 && *xisfconv_external_file(f, 1) == 0,
              "and names one other file, the data blocks file");
        CHECK((long)xisfconv_file_size(f) == header_size && (long)xisfconv_unit_size(f) == header_size,
              "of which nothing counts while it may not be read");
        CHECK(xisfconv_external_status(f, 0) == XISFCONV_ERR_NOT_ALLOWED && xisfconv_external_status(f, 1) == XISFCONV_ERR_INDEX &&
                  xisfconv_external_status(NULL, 0) == XISFCONV_ERR_INDEX,
              "and its status says why");
        CHECK(xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_ERR_NOT_ALLOWED &&
                  strstr(xisfconv_error_message(ctx), "no file but the header"),
              "the pixels are in a file that is not opened");
    }
    xisfconv_close(f);
    f = NULL;
    CHECK(xisfconv_convert(ctx, path_of("unit.xish"), path_of("refused.fits"), NULL) == XISFCONV_ERR_NOT_ALLOWED && !file_exists(path_of("refused.fits")) &&
              !file_exists(path_of("refused.fits.part")),
          "nor is it converted");

    /* ... and with the default: the unit */
    CHECK(xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_HEADER_DIRECTORY) == XISFCONV_OK, "back to the default");
    st = xisfconv_open(ctx, path_of("unit.xish"), &f);
    CHECK(st == XISFCONV_OK, "the unit opens");
    if (st == XISFCONV_OK) {
        CHECK((long)xisfconv_file_size(f) == header_size && (long)xisfconv_unit_size(f) == header_size + blocks_size,
              "the size of the unit is that of its files");
        CHECK(xisfconv_external_count(f) == 1 && xisfconv_external_status(f, 0) == XISFCONV_OK, "one other file, which is read");
        /* a file that is open keeps the setting it was opened with */
        CHECK(xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_NONE) == XISFCONV_OK &&
                  xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_OK && rows_equal(back, g_gray, 0),
              "the pixels are read from the data blocks file, also after the setting has changed");
        CHECK(xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_HEADER_DIRECTORY) == XISFCONV_OK, "the default again");
    }
    xisfconv_close(f);
    f = NULL;
    /* what is no distributed unit */
    CHECK(xisfconv_open(ctx, path_of("gray.xisf"), &f) == XISFCONV_OK && strcmp(xisfconv_file_detail(f, "unit"), "monolithic") == 0 &&
              xisfconv_external_count(f) == 0 && *xisfconv_external_file(f, 0) == 0 && xisfconv_unit_size(f) == xisfconv_file_size(f),
          "a monolithic file names no other files");
    xisfconv_close(f);
    f = NULL;
    CHECK(xisfconv_open(ctx, path_of("gray.fits"), &f) == XISFCONV_OK && *xisfconv_file_detail(f, "unit") == 0 && xisfconv_external_count(f) == 0 &&
              xisfconv_unit_size(f) == xisfconv_file_size(f),
          "nor does a FITS file");
    xisfconv_close(f);
    f = NULL;
    CHECK(xisfconv_external_count(NULL) == 0 && *xisfconv_external_file(NULL, 0) == 0 && xisfconv_unit_size(NULL) == 0, "nor no file at all");

    /* an existing unit is not written over: neither of its two files */
    wo.codec = XISFCONV_CODEC_NONE;
    CHECK(write_gray(ctx, path_of("unit.xish"), &wo, NULL, NULL, 0) == XISFCONV_ERR_EXISTS, "an existing header file is not overwritten");
    remove(path_of("unit.xish"));
    CHECK(write_gray(ctx, path_of("unit.xish"), &wo, NULL, NULL, 0) == XISFCONV_ERR_EXISTS && !file_exists(path_of("unit.xish")) &&
              file_size(path_of("unit.xisb")) == blocks_size,
          "nor an existing data blocks file, and no header is written then");
    wo.overwrite = 1;
    CHECK(write_gray(ctx, path_of("unit.xish"), &wo, NULL, NULL, 0) == XISFCONV_OK && unit_is_gray(ctx, path_of("unit.xish"), "distributed") &&
              file_size(path_of("unit.xisb")) != blocks_size && !file_exists(path_of("unit.xisb.part")),
          "unless that is asked for");
    /* the format is XISF by that name; another format that is asked for is written, in one file */
    wo.format = XISFCONV_FORMAT_FITS;
    CHECK(write_gray(ctx, path_of("named.xish"), &wo, NULL, NULL, 0) == XISFCONV_OK && file_begins(path_of("named.xish"), "SIMPLE  =") &&
              !file_exists(path_of("named.xisb")),
          "FITS under the name of a header file is FITS");

    /* converted: from a unit, and to one */
    xisfconv_convert_options_init(&co, sizeof co);
    co.overwrite = 1;
    CHECK(xisfconv_convert(ctx, path_of("unit.xish"), path_of("unit.fits"), &co) == XISFCONV_OK, "a unit to FITS");
    co.codec = XISFCONV_CODEC_LZ4HC;
    CHECK(xisfconv_convert(ctx, path_of("unit.fits"), path_of("from.xish"), &co) == XISFCONV_OK && file_begins(path_of("from.xisb"), "XISB0100") &&
              unit_is_gray(ctx, path_of("from.xish"), "distributed"),
          "FITS to a unit");
    co.overwrite = 0;
    CHECK(xisfconv_convert(ctx, path_of("unit.fits"), path_of("from.xish"), &co) == XISFCONV_ERR_EXISTS, "not over an existing one");
    CHECK(xisfconv_convert(ctx, path_of("unit.xish"), path_of("other.xish"), NULL) == XISFCONV_ERR_ARGUMENT, "XISF to XISF is a rewrite, whatever the kind of unit");

    /* rewritten: packed into one file, unpacked from it, and in place */
    xisfconv_rewrite_options_init(&ro, sizeof ro);
    xisfconv_rewrite_result_init(&rr, sizeof rr);
    header_size = file_size(path_of("unit.xish"));
    blocks_size = file_size(path_of("unit.xisb"));
    CHECK(xisfconv_rewrite(ctx, path_of("unit.xish"), path_of("packed.xisf"), &ro, &rr) == XISFCONV_OK && rr.blocks == 1 && rr.kept == 1 &&
              rr.changed == 1 && rr.read_back == 1 && (long)rr.input_size == header_size + blocks_size &&
              (long)rr.output_size == file_size(path_of("packed.xisf")) && unit_is_gray(ctx, path_of("packed.xisf"), "monolithic"),
          "a unit packed into one file");
    CHECK(xisfconv_open(ctx, path_of("packed.xisf"), &f) == XISFCONV_OK && xisfconv_external_count(f) == 0, "which names no other file");
    xisfconv_close(f);
    f = NULL;
    ro.codec = XISFCONV_CODEC_ZLIB;
    ro.checksum = XISFCONV_CHECKSUM_SHA1;
    xisfconv_rewrite_result_init(&rr, sizeof rr);
    CHECK(xisfconv_rewrite(ctx, path_of("packed.xisf"), path_of("unpacked.xish"), &ro, &rr) == XISFCONV_OK && rr.compressed == 1 && rr.checksums == 1 &&
              rr.read_back == 1 && (long)rr.output_size == file_size(path_of("unpacked.xish")) + file_size(path_of("unpacked.xisb")) &&
              unit_is_gray(ctx, path_of("unpacked.xish"), "distributed") && !file_exists(path_of("unpacked.xisb.part")),
          "a file unpacked into a header and its data, compressed on the way");
    CHECK(xisfconv_rewrite(ctx, path_of("packed.xisf"), path_of("unpacked.xish"), &ro, NULL) == XISFCONV_ERR_EXISTS, "not over an existing unit");
    remove(path_of("unpacked.xish"));
    CHECK(xisfconv_rewrite(ctx, path_of("packed.xisf"), path_of("unpacked.xish"), &ro, NULL) == XISFCONV_ERR_EXISTS && !file_exists(path_of("unpacked.xish")),
          "nor over the data blocks file of one");
    ro.overwrite = 1;
    CHECK(xisfconv_rewrite(ctx, path_of("packed.xisf"), path_of("unpacked.xish"), &ro, NULL) == XISFCONV_OK, "unless that is asked for");
    CHECK(xisfconv_rewrite(ctx, path_of("unpacked.xish"), path_of("unpacked.xisb"), &ro, NULL) == XISFCONV_ERR_ARGUMENT &&
              file_begins(path_of("unpacked.xisb"), "XISB0100"),
          "the output is not a file the input reads");
    CHECK(xisfconv_stored_as_requested(ctx, path_of("unpacked.xish"), &ro, &same) == XISFCONV_OK && same == 1, "the unit is stored as requested");
    xisfconv_rewrite_result_init(&rr, sizeof rr);
    CHECK(xisfconv_rewrite_in_place(ctx, path_of("unpacked.xish"), &ro, &rr) == XISFCONV_OK && rr.changed == 0, "and left alone in place");
    ro.codec = XISFCONV_CODEC_NONE;
    CHECK(xisfconv_stored_as_requested(ctx, path_of("unpacked.xish"), &ro, &same) == XISFCONV_OK && same == 0, "not as requested with another codec");
    blocks_size = file_size(path_of("unpacked.xisb"));
    xisfconv_rewrite_result_init(&rr, sizeof rr);
    CHECK(xisfconv_rewrite_in_place(ctx, path_of("unpacked.xish"), &ro, &rr) == XISFCONV_OK && rr.changed == 1 && rr.decompressed == 1 &&
              rr.read_back == 1 && file_size(path_of("unpacked.xisb")) != blocks_size && unit_is_gray(ctx, path_of("unpacked.xish"), "distributed") &&
              !file_exists(path_of("unpacked.xish.part")) && !file_exists(path_of("unpacked.xisb.part")),
          "in place, the header file and its data blocks file are replaced");

    /* verified */
    CHECK(xisfconv_verify(ctx, path_of("unpacked.xish"), &report) == XISFCONV_OK && report != NULL &&
              xisfconv_report_verdict(report) == XISFCONV_VERDICT_OK && strstr(xisfconv_report_summary(report), "distributed unit") &&
              xisfconv_report_verified(report) == 1,
          "a unit is verified through its header");
    xisfconv_report_free(report);
    report = NULL;
    copy_damaged(path_of("unpacked.xisb"), path_of("damaged.xisb"), 5);
    CHECK(xisfconv_rewrite(ctx, path_of("unpacked.xish"), path_of("copy.xisf"), NULL, NULL) == XISFCONV_OK, "(a copy of the unit)");
    remove(path_of("unpacked.xisb"));
    CHECK(rename(path_of("damaged.xisb"), path_of("unpacked.xisb")) == 0, "(its data blocks file, with one byte changed)");
    CHECK(xisfconv_verify(ctx, path_of("unpacked.xish"), &report) == XISFCONV_OK && xisfconv_report_verdict(report) == XISFCONV_VERDICT_FAILED &&
              xisfconv_report_problem_count(report) == 1 && strstr(xisfconv_report_problem(report, 0), "checksum mismatch"),
          "a changed byte in the data blocks file is found");
    xisfconv_report_free(report);
    report = NULL;
    remove(path_of("unpacked.xisb"));
    CHECK(xisfconv_open(ctx, path_of("unpacked.xish"), &f) == XISFCONV_OK && xisfconv_external_count(f) == 1 &&
              xisfconv_external_status(f, 0) == XISFCONV_ERR_IO && (long)xisfconv_unit_size(f) == file_size(path_of("unpacked.xish")) &&
              xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_ERR_IO && strstr(xisfconv_error_message(ctx), "unpacked.xisb"),
          "a unit without its data blocks file opens; the error of reading names the file that is missing");
    xisfconv_close(f);
    f = NULL;

    /* what a header is followed to: a file that is one block, by the ways a header can name it */
    for (i = 0; i < W * H; ++i) {
        little[2 * i] = (unsigned char)(g_gray[i] & 0xFF);
        little[2 * i + 1] = (unsigned char)(g_gray[i] >> 8);
    }
    CHECK(write_bytes(path_of("whole.dat"), little, sizeof little), "(the pixels in a file of their own)");
    CHECK(write_header(path_of("beside.xish"), "path(@header_dir/whole.dat)") && unit_is_gray(ctx, path_of("beside.xish"), "distributed"),
          "a file beside the header that is one block");
    CHECK(xisfconv_open(ctx, path_of("beside.xish"), &f) == XISFCONV_OK && xisfconv_external_count(f) == 1 &&
              (long)xisfconv_unit_size(f) == file_size(path_of("beside.xish")) + (long)sizeof little,
          "counts as a file of the unit");
    xisfconv_close(f);
    f = NULL;
    /* the same file by a path that leaves the directory and comes back: <dir>/../<name of dir>/whole.dat */
    slash = strrchr(g_dir, '/');
    name = strrchr(g_dir, '\\');
    if (name && (!slash || name > slash)) slash = name;
    name = slash ? slash + 1 : g_dir;
    if (*name && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && !strchr(name, '(') && !strchr(name, ')') && !strchr(name, '"') &&
        !strchr(name, '&') && !strchr(name, '<')) {
        sprintf(location, "path(@header_dir/../%s/whole.dat)", name);
        CHECK(write_header(path_of("climbs.xish"), location), "(a header whose path climbs out of its directory)");
        st = xisfconv_open(ctx, path_of("climbs.xish"), &f);
        CHECK(st == XISFCONV_OK && xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_ERR_NOT_ALLOWED &&
                  strstr(xisfconv_error_message(ctx), "leads out of the directory of the header"),
              "a path that climbs out of the directory of the header is not followed");
        xisfconv_close(f);
        f = NULL;
        CHECK(xisfconv_convert(ctx, path_of("climbs.xish"), path_of("climbs.fits"), NULL) == XISFCONV_ERR_NOT_ALLOWED && !file_exists(path_of("climbs.fits")),
              "nor by a conversion");
        CHECK(xisfconv_rewrite(ctx, path_of("climbs.xish"), path_of("climbs.xisf"), NULL, NULL) == XISFCONV_ERR_NOT_ALLOWED &&
                  !file_exists(path_of("climbs.xisf")) && !file_exists(path_of("climbs.xisf.part")),
              "nor by a rewrite: the file is not copied into the output");
        CHECK(xisfconv_verify(ctx, path_of("climbs.xish"), &report) == XISFCONV_OK && xisfconv_report_verdict(report) == XISFCONV_VERDICT_NOT_FULLY_CHECKED &&
                  xisfconv_report_not_checked_count(report) == 1,
              "a verification says what it did not check");
        xisfconv_report_free(report);
        report = NULL;
        CHECK(xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_ANYWHERE) == XISFCONV_OK &&
                  unit_is_gray(ctx, path_of("climbs.xish"), "distributed"),
              "unless the context lets a header lead anywhere");
        CHECK(xisfconv_rewrite(ctx, path_of("climbs.xish"), path_of("climbs.xisf"), NULL, NULL) == XISFCONV_OK &&
                  unit_is_gray(ctx, path_of("climbs.xisf"), "monolithic"),
              "then it is packed, too");
        CHECK(xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_HEADER_DIRECTORY) == XISFCONV_OK, "the default again");
    }
    CHECK(write_header(path_of("network.xish"), "url(https://example.com/whole.dat)") &&
              xisfconv_convert(ctx, path_of("network.xish"), path_of("network.fits"), NULL) == XISFCONV_ERR_UNSUPPORTED &&
              strstr(xisfconv_error_message(ctx), "nothing is fetched from a network"),
          "nothing is fetched from a network");
    CHECK(xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_ANYWHERE) == XISFCONV_OK &&
              xisfconv_convert(ctx, path_of("network.xish"), path_of("network.fits"), NULL) == XISFCONV_ERR_UNSUPPORTED &&
              xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_HEADER_DIRECTORY) == XISFCONV_OK,
          "whatever is allowed");
    CHECK(xisfconv_open(ctx, path_of("network.xish"), &f) == XISFCONV_OK && xisfconv_external_count(f) == 1 &&
              strcmp(xisfconv_external_file(f, 0), "https://example.com/whole.dat") == 0 &&
              xisfconv_external_status(f, 0) == XISFCONV_ERR_UNSUPPORTED,
          "the URL is what the unit names");
    xisfconv_close(f);
    f = NULL;
    /* only a file that is named as a header file is followed by its own word */
    CHECK(write_header(path_of("header.xml"), "path(@header_dir/whole.dat)") && xisfconv_open(ctx, path_of("header.xml"), &f) == XISFCONV_OK &&
              strcmp(xisfconv_file_detail(f, "unit"), "distributed") == 0 && xisfconv_external_status(f, 0) == XISFCONV_ERR_NOT_ALLOWED &&
              xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_ERR_NOT_ALLOWED &&
              strstr(xisfconv_error_message(ctx), "does not have the name of one"),
          "a header file under another name is read, and not followed to its data");
    xisfconv_close(f);
    f = NULL;
    CHECK(xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_ANYWHERE) == XISFCONV_OK && unit_is_gray(ctx, path_of("header.xml"), "distributed") &&
              xisfconv_context_set_external_files(ctx, XISFCONV_EXTERNAL_HEADER_DIRECTORY) == XISFCONV_OK,
          "unless a header may lead anywhere");
    xisfconv_rewrite_options_init(&ro, sizeof ro);
    CHECK(xisfconv_rewrite(ctx, path_of("gray.xisf"), path_of("second.xish"), &ro, NULL) == XISFCONV_OK, "(a unit)");
    CHECK(xisfconv_rewrite(ctx, path_of("second.xish"), path_of("second.xisb"), &ro, NULL) == XISFCONV_ERR_ARGUMENT,
          "an output that is the data of the input is a wrong argument, whether or not it may be overwritten");
    /* a rewrite does not replace, in place, a data blocks file that holds what this header does not name:
       shared.xisb has the pixels twice, as block 11 and as block 22, and shared.xish names block 11 */
    {
        unsigned char both[16 + 16 + 2 * 40 + 2 * sizeof little];
        const size_t first_block = 16 + 16 + 2 * 40;
        int k;
        memset(both, 0, sizeof both);
        memcpy(both, "XISB0100", 8);
        both[16] = 2;                                             /* a node of two elements, the last one */
        for (k = 0; k < 2; ++k) {
            unsigned char *element = both + 32 + 40 * k;
            const size_t position = first_block + k * sizeof little;
            element[0] = (unsigned char)(k ? 22 : 11);            /* the identifier */
            element[8] = (unsigned char)(position & 0xFF);        /* where the block is */
            element[9] = (unsigned char)(position >> 8);
            element[16] = (unsigned char)sizeof little;           /* and how long */
            memcpy(both + position, little, sizeof little);
        }
        CHECK(write_bytes(path_of("shared.xisb"), both, sizeof both) && write_header(path_of("shared.xish"), "path(@header_dir/shared.xisb):11") &&
                  unit_is_gray(ctx, path_of("shared.xish"), "distributed"),
              "(a unit whose data blocks file holds a block of another header)");
        xisfconv_rewrite_options_init(&ro, sizeof ro);
        ro.checksum = XISFCONV_CHECKSUM_SHA1;
        CHECK(xisfconv_rewrite_in_place(ctx, path_of("shared.xish"), &ro, NULL) == XISFCONV_ERR_EXISTS &&
                  strstr(xisfconv_error_message(ctx), "does not name") && file_size(path_of("shared.xisb")) == (long)sizeof both &&
                  !file_exists(path_of("shared.xish.part")) && !file_exists(path_of("shared.xisb.part")),
              "in place, it is not replaced: the other block would be gone");
        ro.overwrite = 1;
        CHECK(xisfconv_rewrite_in_place(ctx, path_of("shared.xish"), &ro, NULL) == XISFCONV_OK && unit_is_gray(ctx, path_of("shared.xish"), "distributed") &&
                  file_size(path_of("shared.xisb")) != (long)sizeof both && !file_exists(path_of("shared.xisb.replaced")),
              "unless that is asked for");
    }
    CHECK(write_header(path_of("malformed.xish"), "path(whole.dat)") && xisfconv_open(ctx, path_of("malformed.xish"), &f) == XISFCONV_OK &&
              xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_ERR_FORMAT,
          "a path that is neither absolute nor below @header_dir is no location");
    xisfconv_close(f);
    f = NULL;
    CHECK(write_header(path_of("attached.xish"), "attachment:4096:70") && xisfconv_open(ctx, path_of("attached.xish"), &f) == XISFCONV_OK &&
              xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_ERR_FORMAT && strstr(xisfconv_error_message(ctx), "nothing is attached"),
          "nothing is attached to a header file");
    xisfconv_close(f);
}

static void test_lifetime(void) {
    xisfconv_context *ctx = xisfconv_context_new();
    xisfconv_file *f = NULL;
    xisfconv_keywords *kw = NULL;
    xisfconv_report *report = NULL;
    xisfconv_writer *w = NULL;
    uint16_t back[W * H];
    CHECK(ctx != NULL, "a second context");
    if (!ctx) return;
    CHECK(xisfconv_open(ctx, path_of("gray.xisf"), &f) == XISFCONV_OK, "open");
    CHECK(xisfconv_keywords_new(ctx, &kw) == XISFCONV_OK, "keywords");
    CHECK(xisfconv_verify(ctx, path_of("gray.xisf"), &report) == XISFCONV_OK, "report");
    CHECK(xisfconv_writer_new(ctx, path_of("late.xisf"), NULL, &w) == XISFCONV_OK, "writer");
    {
        static messages seen;
        static progress steps;
        xisfconv_write_options wo;
        xisfconv_writer *loud = NULL;
        xisfconv_context_set_message_handler(ctx, on_message, &seen);
        xisfconv_context_set_progress_handler(ctx, on_progress, &steps);
        xisfconv_write_options_init(&wo, sizeof wo);
        wo.checksum = XISFCONV_CHECKSUM_SHA3_256;   /* would warn */
        wo.overwrite = 1;
        CHECK(xisfconv_writer_new(ctx, path_of("late-sha3.xisf"), &wo, &loud) == XISFCONV_OK, "a writer that will have something to say");
        if (loud) {
            xisfconv_image img;
            xisfconv_image_init(&img, sizeof img);
            img.pixels = g_gray;
            img.width = W;
            img.height = H;
            img.sample_format = XISFCONV_SAMPLE_UINT16;
            CHECK(xisfconv_writer_add_image(loud, &img) == XISFCONV_OK, "with an image");
        }
        xisfconv_context_free(ctx);
        CHECK(xisfconv_writer_finish(loud) == XISFCONV_OK && seen.warnings == 0 && steps.calls == 0,
              "after the context is freed its handlers are not called any more");
    }
    CHECK(xisfconv_read_pixels(f, 0, NULL, back, sizeof back) == XISFCONV_OK && rows_equal(back, g_gray, 0), "the file outlives its context");
    CHECK(xisfconv_read_pixels(f, 9, NULL, back, sizeof back) == XISFCONV_ERR_INDEX, "errors included");
    CHECK(xisfconv_keywords_append(kw, "A", "1", NULL) == XISFCONV_OK, "so does a keyword list");
    CHECK(xisfconv_report_verdict(report) == XISFCONV_VERDICT_OK, "and a report");
    CHECK(xisfconv_writer_finish(w) == XISFCONV_ERR_ARGUMENT, "and a writer");
    xisfconv_report_free(report);
    xisfconv_keywords_free(kw);
    xisfconv_close(f);
}

int main(int argc, char **argv) {
    xisfconv_context *ctx;
    xisfconv_keywords *kw;
    const int quiet = argc > 2 && strcmp(argv[2], "--quiet") == 0;
    if (argc < 2 || strlen(argv[1]) > 900) {
        fprintf(stderr, "usage: %s <empty directory for test files> [--quiet]\n", argv[0]);
        return 2;
    }
    strcpy(g_dir, argv[1]);
    make_images();

    test_basics();
    ctx = xisfconv_context_new();
    CHECK(ctx != NULL, "a context");
    if (!ctx) return 1;
    CHECK(*xisfconv_error_message(ctx) == 0, "no error yet");
    test_null_arguments(ctx);
    test_struct_sizes();
    kw = test_keywords(ctx);
    test_writer_and_readers(ctx, kw);
    xisfconv_keywords_free(kw);
    test_convert_rewrite_verify(ctx);
    test_tile_compression(ctx);
    test_callbacks(ctx);
    test_kept_messages_and_cancel(ctx);
    test_host_progress(ctx);
    test_stretch_and_wcs(ctx);
    test_fits_header(ctx);
    test_carried_properties(ctx);
    test_given_properties(ctx);
    test_distributed_units(ctx);
    xisfconv_context_free(ctx);
    test_lifetime();

    if (!quiet || g_failed) printf("%d checks passed, %d failed\n", g_checks - g_failed, g_failed);
    return g_failed ? 1 : 0;
}
