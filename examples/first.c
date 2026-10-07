/* The smallest program with libxisfconv: what is in a file.
 *
 *   cc first.c $(pkg-config --cflags --libs xisfconv) -o first
 *   ./first image.xisf                 (or a FITS or ASDF file)
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 Jurgen Kobierczynski
 */
#include <stdio.h>

#include "xisfconv.h"

int main(int argc, char **argv) {
    xisfconv_context *ctx;
    xisfconv_file *file = NULL;
    xisfconv_image_info info;
    size_t i;

    if (argc < 2) return 2;
    ctx = xisfconv_context_new(); /* holds the text of a failure */
    if (!ctx) return 1;
    if (xisfconv_open(ctx, argv[1], &file) != XISFCONV_OK) {
        fprintf(stderr, "%s: %s\n", argv[1], xisfconv_error_message(ctx));
        xisfconv_context_free(ctx);
        return 1;
    }
    for (i = 0; i < xisfconv_image_count(file); ++i) {
        xisfconv_image_info_init(&info, sizeof info);
        if (xisfconv_image_info_get(file, i, &info) != XISFCONV_OK) continue;
        printf("%s: %llu x %llu pixels, %llu channel(s)\n", xisfconv_image_name(file, i),
               (unsigned long long)info.width, (unsigned long long)info.height,
               (unsigned long long)info.channels);
    }
    xisfconv_close(file);
    xisfconv_context_free(ctx);
    return 0;
}
