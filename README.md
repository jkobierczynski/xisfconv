<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo-dark.svg">
    <img src="docs/logo.svg" alt="xisfconv, XISF image converter" width="480">
  </picture>
</p>

# xisfconv

A small, dependency-light command-line converter between PixInsight **XISF**, **FITS** and **ASDF**,
in every direction, with **TIFF** and **PNG** export from all three, and a reader of the raw images
of **DNG** files (camera raw). The same code is available as a
library, **libxisfconv**, with a plain C API for C, C++ and other languages, and as a
**Python package** that reads and writes the images as NumPy arrays and works with astropy.

```
xisfconv M31_integration.xisf                 # -> M31_integration.fits
xisfconv -c M31_integration.xisf              # -> M31_integration.fits.fz (tile-compressed, lossless)
xisfconv -c light_0001.fits                   # -> light_0001.xisf (zstd-compressed)
xisfconv -t asdf M31_integration.xisf         # -> M31_integration.asdf
xisfconv observation.asdf                     # -> observation.xisf
xisfconv IMG_0001.dng                         # -> IMG_0001.xisf: the camera's raw image, not demosaiced
xisfconv -t tiff -c -b u16 *.xisf -d export/  # batch to 16-bit Deflate TIFFs
xisfconv -t fits lights/                      # every XISF file below lights/ -> a FITS file next to it
xisfconv -t tiff -s -b u8 integration.xisf     # stretched 8-bit TIFF for GIMP
xisfconv -t png -s -b u8 integration.xisf      # stretched 8-bit PNG for the web
xisfconv -t png -s -b u8 light_0001.fits       # quick look at a raw FITS frame
xisfconv -t png -s -b u8 --resize 1024 *.xisf  # previews, the longest side 1024 pixels
xisfconv -c --in-place *.xisf                 # recompress XISF files with zstd, replacing them
xisfconv -t xish light_0001.xisf              # -> light_0001.xish + light_0001.xisb (a distributed unit)
xisfconv light_0001.xish -t xisf              # ... and packed into one file again
xisfconv --verify ~/astro/2026                # check every XISF, FITS, ASDF and DNG file below a folder
xisfconv --info light_0001.xisf               # geometry, codecs, FITS keywords, properties
```

## What it does

- **XISF 1.0**, read and written: every sample format (UInt8 to UInt64, Float32, Float64), gray,
  RGB and extra channels, planar and interleaved storage, both byte orders, zlib, LZ4, LZ4HC and
  Zstandard with byte shuffling and subblocks, attached, inline and embedded data blocks,
  SHA-1, SHA-2 and SHA-3 checksums, several images in a file, monolithic files and distributed
  units (`.xish` + `.xisb`).
- **FITS** in both directions, with the keywords carried over, the rows turned to the convention
  of each format, and tile-compressed files (`.fits.fz`) read and written without loss.
- **ASDF** in both directions, in the layout astropy's `asdf` packages read as an HDU list.
- **DNG** read: the raw image as the sensor recorded it (cut to its active area, not demosaiced),
  with its colour filter pattern (Bayer or X-Trans) and the exposure (camera, time, exposure
  time, ISO) as keywords; uncompressed, lossless JPEG and Deflate. A DNG file of any camera is
  made by Adobe's free DNG Converter.
- **Astrometry**: a PixInsight plate solution becomes WCS keywords with SIP distortion, and WCS
  keywords become the solution properties PixInsight reads.
- **XISF properties** (processing history, instrument, observation, the astrometric solution) go
  along through FITS and ASDF and come back as they were.
- **XISF → XISF**: another compression, checksums added or removed, one image of several, in place
  if asked; a distributed unit packed into one file and the reverse.
- **Whole folders**: a directory is converted with what is below it, the files that already are
  what is asked for passed over, the tree kept below `-d`, and on a later run only what was added
  (`--skip-existing`). `*.xisf` works in cmd and PowerShell too: the program expands patterns
  itself.
- **`--verify`** checks XISF, FITS, ASDF and DNG files, and whole directories of them, without
  converting anything.
- **TIFF and PNG** for looking at: PixInsight's screen stretch (`--stretch`), a sample format of
  choice (`--bits`), smaller pictures (`--bin`, `--resize`), colour pictures of a colour camera's
  frames (`--debayer`), and previews in Linux file managers.
- **Images of any size**: an image is read and written a piece at a time, so a mosaic of several
  gigabytes converts on a machine with far less memory: what has to wait between reading and
  writing is kept in memory up to 256 MB and in temporary files beside the output beyond. TIFF
  output beyond 4 GiB is BigTIFF, and Ctrl-C stops a conversion within one image without leaving
  a half-written file.
- **Careful with files**: output is written under another name and renamed when it is complete,
  nothing is overwritten unless asked, no file is written twice in a run or over a file the run
  reads, a rewrite is read back and compared before it replaces anything, and the header of a
  distributed unit is followed only to files in its own directory unless told otherwise.
- **Few dependencies**: zlib, and libzstd if it is there. The readers and writers of every format
  are in the source.

[`MANUAL.md`](MANUAL.md) says for each of these what exactly is done, what is not, and what was
checked against PixInsight.

## Getting it

**Binaries.** Ready-to-run binaries for Linux (x86_64), macOS (Apple Silicon) and Windows (x64) are
attached to each [GitHub release](https://github.com/jkobierczynski/xisfconv/releases), with SHA-256
checksums. They need no extra libraries: Zstandard (and on Windows the C runtime) is linked in.

**Building.** A C++17 compiler, CMake ≥ 3.15 and zlib; libzstd is optional but recommended
(PixInsight can write Zstandard-compressed files).

```
# Debian/Ubuntu: sudo apt install build-essential cmake zlib1g-dev libzstd-dev
# Fedora:        sudo dnf install gcc-c++ cmake zlib-devel libzstd-devel
# macOS:         brew install cmake zstd
cmake -S . -B build
cmake --build build -j
./build/xisfconv --version        # lists the enabled codecs
sudo cmake --install build        # optional: the tool and its man page, the library
```

Windows (vcpkg): `vcpkg install zlib zstd`, then configure with
`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`. With `-DBUILD_SHARED_LIBS=ON` the
library is built as a shared library and installed with its header, a pkg-config file and a CMake
package; [`DEVELOPMENT.md`](DEVELOPMENT.md#building-for-development) has the other options.

**Python.** `pip install .` in a checkout builds the library and installs the package
(`pip install ".[astropy]"` with astropy). It needs Python 3.10 or later and NumPy. The package is
not on PyPI yet.

## Using it

**The command line.** `xisfconv --help` lists the options; the examples above are most of what is
needed day to day. The output format follows `-t` or the name given with `-o`; without either, XISF
becomes FITS, and FITS, ASDF and DNG become XISF. [`MANUAL.md`](MANUAL.md) has every option and what
each conversion does.

**From C and C++.** Everything the tool does is done by the library, through
[`include/xisfconv.h`](include/xisfconv.h):

```c
#include "xisfconv.h"
/* cc app.c $(pkg-config --cflags --libs xisfconv) */

xisfconv_context *ctx = xisfconv_context_new();
xisfconv_file *file = NULL;
if (xisfconv_open(ctx, "M31.xisf", &file) != XISFCONV_OK) {
    fprintf(stderr, "%s\n", xisfconv_error_message(ctx));
} else {
    xisfconv_read_options ro;
    uint64_t size;
    xisfconv_read_options_init(&ro, sizeof ro);
    ro.sample_format = XISFCONV_SAMPLE_FLOAT32;      /* whatever the file holds */
    xisfconv_pixels_size(file, 0, &ro, &size);
    float *pixels = malloc(size);                    /* [channels][height][width], top row first */
    xisfconv_read_pixels(file, 0, &ro, pixels, size);
    /* ... */
    free(pixels);
    xisfconv_close(file);
}
xisfconv_convert(ctx, "M31.xisf", "M31.fits", NULL); /* what the tool does, in one call */
xisfconv_context_free(ctx);
```

**From Python.**

```python
import xisfconv

data = xisfconv.read("m31.xisf")                    # [height, width] or [height, width, channels]
image = xisfconv.read_image("m31.xisf")             # with keywords, name, bounds, XISF properties
xisfconv.write("out.xisf", data, keywords={"OBJECT": "M 31"}, codec="zstd", checksum="sha256")
xisfconv.convert("m31.xisf", "m31.fits")            # what the command line tool does
print(xisfconv.verify("m31.xisf").verdict)

import xisfconv.astropy                             # CCDData.read("m31.xisf") and HDU lists
from xisfconv.xisf import XISF                      # the interface of the xisf package
```

## Documentation

| Document | What is in it |
|---|---|
| [`MANUAL.md`](MANUAL.md) | The command line tool: every option, what each conversion does with keywords, rows, astrometry and properties, the limitations, and what was verified against PixInsight |
| [`man/xisfconv.1`](man/xisfconv.1) | The manual page of the tool: the options in short. `man xisfconv` once it is installed, `man ./xisfconv.1` in the directory of a release |
| [`docs/manual.html`](docs/manual.html), [`offsite online manual.html`](https://jurgenkobierczynski.com/xisfconv/docs/manual.html) | The library for C, C++ and Python: the rules that hold everywhere, a tour of example programs in the three languages with what they print, and the reference of every function. One file, to be opened in a browser (GitHub shows its source) |
| [`python/README.md`](python/README.md) | The Python package: arrays, keywords, properties, astropy, the interface of the `xisf` package |
| [`examples/`](examples) | The programs the manual of the library shows, in C, C++ and Python |
| [`docs/xisf-properties-in-fits-and-asdf.md`](docs/xisf-properties-in-fits-and-asdf.md) | For programs that read or write them without xisfconv: where the XISF properties are in a FITS and in an ASDF file, column by column and key by key, and the digest that ties an astrometric solution to its WCS keywords |
| [`CHANGELOG.md`](CHANGELOG.md) | What changed in each version |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | How to report a file that fails, and what a change to the code needs |
| [`SECURITY.md`](SECURITY.md) | How to report a security problem, what counts as one, and what the program does about files from people you do not know |
| [`CITATION.cff`](CITATION.cff) | How to cite xisfconv (GitHub: "Cite this repository") |
| [`DEVELOPMENT.md`](DEVELOPMENT.md) | For whoever changes the program: building and testing, the way a change is made and released, the steps taken so far, and the decisions behind it all |
| [`TODO.md`](TODO.md) | What is planned |

## Status

Version 0.17. The files it writes were checked against PixInsight 1.9.3, astropy, CFITSIO's tools,
the `xisf` package, Python's `asdf` and OpenXISF, each where it applies:
[what was verified](MANUAL.md#verified-against-pixinsight) and [what is known not to
work](MANUAL.md#limitations) are written down. The interfaces of the library may still change
between 0.x releases. Please report any file that fails to convert, ideally with the output of
`xisfconv --info`.

## License

Copyright (C) 2026 Jurgen Kobierczynski

The command line tool (`src/main.cpp`) is free software under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your
option) any later version: see [LICENSE](LICENSE).

The library libxisfconv (`include/xisfconv.h` and everything else in `src/`) is free software under
the terms of the GNU Lesser General Public License, either version 3 of the License, or (at your
option) any later version: see [COPYING.LESSER](COPYING.LESSER), which adds its permissions to the
terms in [LICENSE](LICENSE). A program may link the library without taking on the GPL, provided the
conditions of the LGPL are met. Each source file says in its first lines which of the two applies;
the build files, the examples, the manual and the Python package (`python/xisfconv`) belong to the
library, the tests to the tool.

The release binaries and the Python wheels contain Zstandard, and some of them zlib:
see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

XISF and PixInsight are products of Pleiades Astrophoto S.L.; xisfconv is an independent
implementation of the published XISF 1.0 specification and is not affiliated with them.
