/* How libxisfconv reads in C: inspect a file, read an image, convert, verify, write an array.
 *
 *   cc example.c $(pkg-config --cflags --libs xisfconv) -o example
 *   ./example image.xisf preview.png
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 Jurgen Kobierczynski
 */
#include <stdio.h>
#include <stdlib.h>

#include "xisfconv.h"

static void on_message(void *user, xisfconv_message_level level, const char *path, const char *message) {
    (void)user;
    fprintf(stderr, "%s: %s: %s\n", level == XISFCONV_MESSAGE_WARNING ? "warning" : "info", path ? path : "-", message);
}

int main(int argc, char **argv) {
    xisfconv_context *ctx;
    xisfconv_file *file = NULL;
    xisfconv_status st;
    size_t i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <image.xisf|.fits|.asdf> <output.png>\n", argv[0]);
        return 2;
    }
    ctx = xisfconv_context_new();
    if (!ctx) return 1;
    xisfconv_context_set_message_handler(ctx, on_message, NULL);

    /* 1. Inspect a file and read its first image as 32-bit floats, top row first. */
    if (xisfconv_open(ctx, argv[1], &file) != XISFCONV_OK) {
        fprintf(stderr, "error: %s\n", xisfconv_error_message(ctx));
        xisfconv_context_free(ctx);
        return 1;
    }
    for (i = 0; i < xisfconv_image_count(file); ++i) {
        xisfconv_image_info info;
        const xisfconv_keywords *kw = NULL;
        size_t k;
        xisfconv_image_info_init(&info, sizeof info);
        if (xisfconv_image_info_get(file, i, &info) != XISFCONV_OK) continue;
        printf("image %lu \"%s\": %llu x %llu x %llu\n", (unsigned long)i, xisfconv_image_name(file, i),
               (unsigned long long)info.width, (unsigned long long)info.height, (unsigned long long)info.channels);
        if (xisfconv_image_keywords(file, i, &kw) != XISFCONV_OK) continue;
        for (k = 0; k < xisfconv_keywords_count(kw); ++k) {
            const char *name, *value, *comment;
            xisfconv_keywords_get(kw, k, &name, &value, &comment);
            printf("  %-8s = %s / %s\n", name, value, comment);
        }
    }
    if (xisfconv_image_count(file) > 0) {
        xisfconv_read_options ro;
        uint64_t size = 0;
        xisfconv_read_options_init(&ro, sizeof ro);
        ro.sample_format = XISFCONV_SAMPLE_FLOAT32;
        ro.row_order = XISFCONV_ROWS_TOP_DOWN;
        if (xisfconv_pixels_size(file, 0, &ro, &size) == XISFCONV_OK) {
            float *pixels = (float *)malloc((size_t)size);
            if (pixels && xisfconv_read_pixels(file, 0, &ro, pixels, size) == XISFCONV_OK) {
                printf("first sample: %g\n", pixels[0]);

                /* 2. Write an array: here the same pixels, as a compressed XISF file. */
                {
                    xisfconv_image_info info;
                    xisfconv_write_options wo;
                    xisfconv_writer *writer = NULL;
                    xisfconv_image image;
                    xisfconv_image_info_init(&info, sizeof info);
                    xisfconv_image_info_get(file, 0, &info);
                    xisfconv_write_options_init(&wo, sizeof wo);
                    wo.codec = XISFCONV_CODEC_DEFAULT;
                    wo.overwrite = 1;
                    if (xisfconv_writer_new(ctx, "example-copy.xisf", &wo, &writer) == XISFCONV_OK) {
                        xisfconv_image_init(&image, sizeof image);
                        image.pixels = pixels;
                        image.width = info.width;
                        image.height = info.height;
                        image.channels = info.channels;
                        image.sample_format = XISFCONV_SAMPLE_FLOAT32;
                        image.row_order = XISFCONV_ROWS_TOP_DOWN;
                        if (xisfconv_writer_add_image(writer, &image) != XISFCONV_OK) {
                            xisfconv_writer_discard(writer);
                        } else if (xisfconv_writer_finish(writer) != XISFCONV_OK) {
                            fprintf(stderr, "error: %s\n", xisfconv_error_message(ctx));
                        }
                    }
                }
            } else {
                fprintf(stderr, "error: %s\n", xisfconv_error_message(ctx));
            }
            free(pixels);
        }
    }
    xisfconv_close(file);

    /* 3. Convert: a stretched 8-bit PNG. */
    {
        xisfconv_convert_options co;
        xisfconv_convert_options_init(&co, sizeof co);
        co.stretch = XISFCONV_STRETCH_AUTO;
        co.sample_format = XISFCONV_SAMPLE_UINT8;
        co.overwrite = 1;
        st = xisfconv_convert(ctx, argv[1], argv[2], &co);
        if (st != XISFCONV_OK) fprintf(stderr, "error (%s): %s\n", xisfconv_status_text(st), xisfconv_error_message(ctx));
    }

    /* 4. Verify. */
    {
        xisfconv_report *report = NULL;
        if (xisfconv_verify(ctx, argv[1], &report) == XISFCONV_OK) {
            printf("verdict %d, %s\n", (int)xisfconv_report_verdict(report), xisfconv_report_summary(report));
            for (i = 0; i < xisfconv_report_problem_count(report); ++i) printf("  %s\n", xisfconv_report_problem(report, i));
            xisfconv_report_free(report);
        }
    }

    xisfconv_context_free(ctx);
    return st == XISFCONV_OK ? 0 : 1;
}
