/* A rename() that fails on request, for the tests of what xisfconv does when a file it has written
 * cannot be given its name (tests/run_tests.py builds it and loads it with LD_PRELOAD, on Linux).
 *
 *   cc -shared -fPIC -o rename_shim.so tests/rename_shim.c -ldl
 *   XISFCONV_TEST_FAIL_RENAME=".xish.part>.xish,.replaced>.xisb" LD_PRELOAD=./rename_shim.so xisfconv ...
 *
 * Every rename whose source ends in what stands before a '>' and whose target ends in what stands
 * behind it fails with EIO; every other rename is the system's. Nothing of this is in the program
 * or in the library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Jurgen Kobierczynski
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ends_with(const char *text, const char *suffix, size_t length) {
    const size_t n = strlen(text);
    return n >= length && memcmp(text + n - length, suffix, length) == 0;
}

static int is_to_fail(const char *from, const char *to) {
    const char *rule = getenv("XISFCONV_TEST_FAIL_RENAME");
    while (rule && *rule) {
        const char *arrow = strchr(rule, '>');
        const char *end;
        if (!arrow) return 0;
        end = strchr(arrow, ',');
        if (!end) end = arrow + strlen(arrow);
        if (ends_with(from, rule, (size_t)(arrow - rule)) && ends_with(to, arrow + 1, (size_t)(end - arrow - 1))) return 1;
        rule = *end ? end + 1 : end;
    }
    return 0;
}

int rename(const char *from, const char *to) {
    static int (*system_rename)(const char *, const char *);
    if (!system_rename) system_rename = (int (*)(const char *, const char *))dlsym(RTLD_NEXT, "rename");
    if (is_to_fail(from, to)) {
        errno = EIO;
        return -1;
    }
    return system_rename(from, to);
}
