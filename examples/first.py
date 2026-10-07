#!/usr/bin/env python3
"""The smallest program with the xisfconv package: what is in a file.

    python first.py image.xisf                 (or a FITS or ASDF file)

SPDX-License-Identifier: LGPL-3.0-or-later
Copyright (C) 2026 Jurgen Kobierczynski
"""
import sys

import xisfconv

if len(sys.argv) < 2:
    sys.exit(2)
try:
    with xisfconv.open(sys.argv[1]) as f:
        for image in f:
            print("%s: %d x %d pixels, %d channel(s)" % (image.name, image.width, image.height, image.channels))
except xisfconv.Error as error:
    sys.exit(str(error))                       # the text names the file
