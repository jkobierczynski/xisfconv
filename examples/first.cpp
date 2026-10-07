// The smallest program with libxisfconv in C++: what is in a file.
//
//   c++ -std=c++17 first.cpp $(pkg-config --cflags --libs xisfconv) -o first
//   ./first image.xisf                 (or a FITS or ASDF file)
//
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include <cstdio>
#include <memory>

#include "xisfconv.h"

int main(int argc, char** argv) {
    if (argc < 2) return 2;

    // The handles of the C API, freed when they go out of scope.
    std::unique_ptr<xisfconv_context, void (*)(xisfconv_context*)> ctx(xisfconv_context_new(),
                                                                       xisfconv_context_free);
    xisfconv_file* opened = nullptr;
    if (!ctx || xisfconv_open(ctx.get(), argv[1], &opened) != XISFCONV_OK) {
        std::fprintf(stderr, "%s: %s\n", argv[1], ctx ? xisfconv_error_message(ctx.get()) : "no memory");
        return 1;
    }
    std::unique_ptr<xisfconv_file, void (*)(xisfconv_file*)> file(opened, xisfconv_close);

    for (size_t i = 0; i < xisfconv_image_count(file.get()); ++i) {
        xisfconv_image_info info;
        xisfconv_image_info_init(&info, sizeof info);
        if (xisfconv_image_info_get(file.get(), i, &info) != XISFCONV_OK) continue;
        using ull = unsigned long long;  // what printf takes for %llu
        std::printf("%s: %llu x %llu pixels, %llu channel(s)\n", xisfconv_image_name(file.get(), i),
                    ull(info.width), ull(info.height), ull(info.channels));
    }
    return 0;
}
