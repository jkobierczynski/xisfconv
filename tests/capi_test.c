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
              xisfconv_codec_available(XISFCONV_CODEC_LZ4, 1) == 0 && xisfconv_codec_available(99, 0) == 0,
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
        CHECK(xisfconv_property_count(f, 0) == 0, "no properties outside XISF");
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
    CHECK(xisfconv_convert(ctx, path_of("gray.fits"), path_of("same.fits"), NULL) == XISFCONV_ERR_ARGUMENT, "FITS to FITS needs a reason");
    xisfconv_convert_options_init(&co, sizeof co);
    co.sip_order = 1;
    CHECK(xisfconv_convert(ctx, path_of("gray.xisf"), path_of("sip.fits"), &co) == XISFCONV_ERR_ARGUMENT, "a SIP order out of range");
    CHECK(!file_exists(path_of("five.fits")) && !file_exists(path_of("five.fits.part")) && !file_exists(path_of("m.fits")),
          "failed conversions leave no files");

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
    ro.codec = XISFCONV_CODEC_LZ4;
    CHECK(xisfconv_rewrite(ctx, path_of("gray.xisf"), path_of("lz4.xisf"), &ro, NULL) == XISFCONV_ERR_UNSUPPORTED, "LZ4 is not written");

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
    test_callbacks(ctx);
    test_stretch_and_wcs(ctx);
    xisfconv_context_free(ctx);
    test_lifetime();

    if (!quiet || g_failed) printf("%d checks passed, %d failed\n", g_checks - g_failed, g_failed);
    return g_failed ? 1 : 0;
}
