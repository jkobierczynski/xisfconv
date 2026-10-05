/* Reads everything the C API offers from the files named on the command line, verifies them
 * and converts them into a directory. Whatever the files contain, the program has to end
 * normally: it is the target for fuzzing the library through its API (build with a sanitizer).
 *
 * Usage: xisfconv_capi_readall <output directory> <file>...
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Jurgen Kobierczynski
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xisfconv.h"

static unsigned long g_calls = 0, g_failed = 0;

static void note(xisfconv_status st) {
    ++g_calls;
    if (st != XISFCONV_OK) ++g_failed;
}

static void read_all(xisfconv_context *ctx, const char *path) {
    static const char *details[] = {"sampleFormat", "colorSpace", "pixelStorage", "byteOrder", "location", "compression",
                                    "subblocks", "checksum", "imageType", "orientation", "cfaPattern", "cfaName",
                                    "resolutionUnit", "tileCompression", "mapping", "source", "storage"};
    xisfconv_file *f = NULL;
    xisfconv_format format = 0;
    const char *text = NULL;
    size_t length = 0, i, k, d;
    unsigned long sum = 0;

    note(xisfconv_detect_format(ctx, path, &format));
    note(xisfconv_open(ctx, path, &f));
    if (!f) return;
    sum += strlen(xisfconv_file_detail(f, "version")) + strlen(xisfconv_file_detail(f, "format")) + (unsigned long)xisfconv_file_size(f);
    for (i = 0; i < xisfconv_skipped_count(f); ++i) sum += strlen(xisfconv_skipped_text(f, i));
    note(xisfconv_header_text(f, &text, &length));
    if (text) sum += (unsigned long)length + (unsigned char)text[length ? length - 1 : 0];

    for (i = 0; i < xisfconv_image_count(f); ++i) {
        xisfconv_image_info info;
        xisfconv_read_options ro;
        const xisfconv_keywords *kw = NULL;
        xisfconv_keywords *wcs = NULL;
        xisfconv_stretch_params stf[3];
        uint64_t size = 0;
        size_t count = 0, icc = 0;
        int pass;

        xisfconv_image_info_init(&info, sizeof info);
        note(xisfconv_image_info_get(f, i, &info));
        sum += strlen(xisfconv_image_name(f, i)) + strlen(xisfconv_image_unsupported_reason(f, i)) + strlen(info.cfa_pattern);
        for (d = 0; d < sizeof details / sizeof *details; ++d) sum += strlen(xisfconv_image_detail(f, i, details[d]));
        note(xisfconv_image_keywords(f, i, &kw));
        for (k = 0; k < xisfconv_keywords_count(kw); ++k) {
            const char *name = NULL, *value = NULL, *comment = NULL, *plain = NULL;
            note(xisfconv_keywords_get(kw, k, &name, &value, &comment));
            note(xisfconv_keywords_get_text(kw, k, &plain));
            if (name && value && comment && plain) sum += strlen(name) + strlen(value) + strlen(comment) + strlen(plain);
        }
        for (k = 0; k < xisfconv_property_count(f, i); ++k) {
            const char *id = NULL, *type = NULL, *value = NULL, *comment = NULL;
            int32_t block = 0;
            note(xisfconv_property_get(f, i, k, &id, &type, &value, &comment, &block));
            if (id && block) {
                size_t rows = 0, columns = 0;
                if (xisfconv_property_read_f64(f, i, id, NULL, 0, &rows, &columns) == XISFCONV_OK && rows && columns &&
                    rows < 4096 && columns < 4096) {
                    double *values = (double *)malloc(rows * columns * sizeof(double));
                    if (values) {
                        note(xisfconv_property_read_f64(f, i, id, values, rows * columns, &rows, &columns));
                        free(values);
                    }
                }
            }
        }
        if (xisfconv_read_icc_profile(f, i, NULL, 0, &icc) == XISFCONV_OK && icc && icc < (1u << 24)) {
            void *profile = malloc(icc);
            if (profile) {
                note(xisfconv_read_icc_profile(f, i, profile, icc, &icc));
                free(profile);
            }
        }
        note(xisfconv_stored_stretch(f, i, stf, 3, &count));
        if (xisfconv_wcs_keywords(f, i, XISFCONV_ROWS_TOP_DOWN, 3, &wcs, NULL) == XISFCONV_OK) {
            note(xisfconv_wcs_flip_rows(wcs, info.height ? info.height : 1));
            xisfconv_keywords_free(wcs);
        }
        /* the pixels: as stored, then as bottom-up 32-bit floats */
        for (pass = 0; pass < 2; ++pass) {
            xisfconv_read_options_init(&ro, sizeof ro);
            if (pass) {
                ro.sample_format = XISFCONV_SAMPLE_FLOAT32;
                ro.row_order = XISFCONV_ROWS_BOTTOM_UP;
            }
            note(xisfconv_pixels_size(f, i, &ro, &size));
            if (size && size < (1u << 28)) {
                unsigned char *pixels = (unsigned char *)malloc((size_t)size);
                if (pixels) {
                    if (xisfconv_read_pixels(f, i, &ro, pixels, size) == XISFCONV_OK) sum += pixels[0] + pixels[size - 1];
                    else ++g_failed;
                    ++g_calls;
                    free(pixels);
                }
            }
            xisfconv_image_info_init(&info, sizeof info);
            note(xisfconv_image_info_get(f, i, &info));
        }
    }
    for (k = 0; k < xisfconv_property_count(f, XISFCONV_FILE_PROPERTIES); ++k) {
        const char *id = NULL;
        note(xisfconv_property_get(f, XISFCONV_FILE_PROPERTIES, k, &id, NULL, NULL, NULL, NULL));
    }
    xisfconv_close(f);
    if (sum == 0xFFFFFFFFul) fputs("", stdout); /* the sum only forces every string and byte to be read */
}

int main(int argc, char **argv) {
    xisfconv_context *ctx;
    int a;
    if (argc < 3) {
        fprintf(stderr, "usage: %s <output directory> <file>...\n", argv[0]);
        return 2;
    }
    ctx = xisfconv_context_new();
    if (!ctx) return 1;
    for (a = 2; a < argc; ++a) {
        static const char *extensions[] = {"xisf", "fits", "asdf", "tif", "png"};
        xisfconv_convert_options co;
        xisfconv_rewrite_options ro;
        xisfconv_report *report = NULL;
        char out[2048];
        size_t e, i;
        if (strlen(argv[1]) > 1900) return 2;
        read_all(ctx, argv[a]);
        note(xisfconv_verify(ctx, argv[a], &report));
        for (i = 0; i < xisfconv_report_problem_count(report); ++i) (void)strlen(xisfconv_report_problem(report, i));
        for (i = 0; i < xisfconv_report_not_checked_count(report); ++i) (void)strlen(xisfconv_report_not_checked(report, i));
        (void)strlen(xisfconv_report_summary(report));
        xisfconv_report_free(report);
        for (e = 0; e < 5; ++e) {
            sprintf(out, "%s/readall.%s", argv[1], extensions[e]);
            xisfconv_convert_options_init(&co, sizeof co);
            co.overwrite = 1;
            co.codec = e % 2 ? XISFCONV_CODEC_DEFAULT : XISFCONV_CODEC_NONE;
            if (e >= 3) co.stretch = XISFCONV_STRETCH_AUTO;
            note(xisfconv_convert(ctx, argv[a], out, &co));
        }
        sprintf(out, "%s/readall-rewritten.xisf", argv[1]);
        xisfconv_rewrite_options_init(&ro, sizeof ro);
        ro.overwrite = 1;
        ro.codec = XISFCONV_CODEC_ZLIB;
        ro.checksum = XISFCONV_CHECKSUM_SHA1;
        note(xisfconv_rewrite(ctx, argv[a], out, &ro, NULL));
    }
    xisfconv_context_free(ctx);
    printf("%lu calls, %lu returned an error\n", g_calls, g_failed);
    return 0;
}
