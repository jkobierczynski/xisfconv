# xisfconv

A small, dependency-light command-line converter from PixInsight **XISF** images to **FITS**, **TIFF** and **PNG**.

```
xisfconv M31_integration.xisf                 # -> M31_integration.fits
xisfconv -t tiff -c -b u16 *.xisf -d export/  # batch to 16-bit Deflate TIFFs
xisfconv -t tiff -s -b u8 integration.xisf     # stretched 8-bit TIFF for GIMP
xisfconv -t png -s -b u8 integration.xisf      # stretched 8-bit PNG for the web
xisfconv --info light_0001.xisf               # geometry, codecs, FITS keywords, properties
```

## Features

**Reading (monolithic XISF 1.0)**
- Sample formats UInt8/16/32/64, Float32/64; Gray, RGB (and extra/alpha channels)
- Planar and Normal (interleaved) pixel storage, little- and big-endian data
- Compression: zlib, LZ4, LZ4HC (built-in decoder), Zstandard (via libzstd), each with or without
  byte shuffling, including compressed **subblocks**
- Data blocks as attachments, `inline:base64`/`inline:hex`, or `embedded` `<Data>` elements
- SHA-1 / SHA-256 / SHA-512 checksum verification (SHA3 checksums are reported but not verified)
- FITS keywords, XISF properties, ColorFilterArray, Resolution, ICC profile, multiple images

**FITS output**
- BITPIX 8/16/32/64/-32/-64 with the standard BZERO offsets for unsigned data
- All original FITS keywords carried over; structural keywords (SIMPLE, BITPIX, NAXISn, BZERO, ...) are
  regenerated, long names use HIERARCH, over-long strings are truncated with a warning
- Missing keywords filled from XISF properties: OBJECT, EXPTIME, DATE-OBS, TELESCOP, INSTRUME, FILTER,
  CCD-TEMP, XPIXSZ/YPIXSZ, FOCALLEN, APTDIA, IMAGETYP, and BAYERPAT from the CFA element
  (disable with `--no-property-keywords`; existing keywords always win)
- Additional images become IMAGE extensions (EXTNAME = XISF image id)
- Row order: XISF stores rows top-down, FITS viewers expect the first row at the bottom. By default
  rows are flipped to the FITS convention (`ROWORDER = 'BOTTOM-UP'`) and BAYERPAT is adjusted to
  match, so the image shows the same way up as in PixInsight. `--top-down` keeps XISF's order and
  writes `ROWORDER = 'TOP-DOWN'` for readers that honor that keyword.
- Numeric keyword values with a lower-case exponent (PixInsight writes `1.7870e+04`) are written
  with the upper-case `E` the FITS standard requires.

**Astrometry (WCS) from PixInsight plate solutions**
- Images solved with PixInsight's ImageSolver carry the solution as `PCL:AstrometricSolution:*`
  properties rather than FITS keywords, and PixInsight's own FITS export (1.9.3) writes no WCS
  keywords at all, only the approximate center as `RA`/`DEC`. xisfconv turns it into standard WCS keywords (`CTYPE`,
  `CRVAL`, `CRPIX`, `CD`, `RADESYS`) that match the row order written (bottom-up or `--top-down`).
- PixInsight models field distortion with a thin-plate spline. xisfconv fits SIP distortion
  polynomials (order 3 by default, `--sip-order 2..7`, `0` = linear only) to the spline's point grid,
  including the inverse (AP/BP) terms, and prints the fit quality against the matched stars.
- Projections: Gnomonic (TAN), Stereographic, Plate carrée, Mercator, Hammer-Aitoff, zenithal
  equal-area/equidistant, orthographic. Existing WCS keywords always win; `--no-wcs` turns this off.

**TIFF output**
- 8/16/32/64-bit unsigned or 32/64-bit IEEE float samples, chunky (interleaved) layout
- Optional Deflate compression (`-c`) with horizontal or floating-point predictor
- ICC profile and resolution copied; extra channels written as ExtraSamples (first one = alpha)
- Multiple images become multiple pages

**PNG output** (`-t png` or `-o name.png`)
- 8- or 16-bit grayscale, gray+alpha, RGB or RGBA; ICC profile (iCCP) and resolution (pHYs) copied.
- Float data is scaled through its bounds to 16-bit; add `--stretch` for linear data.
- PNG holds one image: multi-image files write the first one (or the one chosen with `--image`).

**Stretch for viewing** (`-s`, `--stretch[=auto|linked|unlinked|stf]`)
- Linear data (integrations, calibrated frames) looks black in ordinary viewers. `--stretch` applies
  PixInsight's screen-transfer-function maths: shadows clip, midtones transfer function, highlights.
- `auto` (the default) uses the STF PixInsight saved in the file (`DisplayFunction`) when there is one,
  otherwise a linked auto-STF. `linked` and `unlinked` always compute an auto-STF like PixInsight's
  AutoStretch (shadows at median − 2.8 × MADN, background to 0.25): linked keeps the color balance,
  unlinked stretches each channel separately and neutralizes color casts. `stf` requires a saved STF.
- Stretched float data becomes 16-bit in TIFF (32-bit float in FITS) unless `--bits` says otherwise.
  The parameters used are printed, and in FITS also recorded as a HISTORY card.
- The stretch is for display and export only: don't feed stretched FITS back into calibration or
  photometry.

**Sample conversion** (`-b u8|u16|u32|f32|f64`)
- integer → integer: rescaled over the full range (65535 → 255)
- integer → float: normalized to [0,1]
- float → integer: the XISF `bounds` range (normally [0,1]) maps to the full integer range, clipped
- float → float: values unchanged

## Download

Ready-to-run binaries for Linux (x86_64), macOS (Apple Silicon) and Windows (x64) are attached to each
[GitHub release](https://github.com/jkobierczynski/xisfconv/releases), with SHA-256 checksums. They
need no extra libraries: Zstandard (and on Windows the C runtime) is linked in.

## Building

Requirements: a C++17 compiler, CMake ≥ 3.14, zlib. libzstd is optional but recommended
(PixInsight can write Zstandard-compressed files).

```
# Debian/Ubuntu: sudo apt install build-essential cmake zlib1g-dev libzstd-dev
# Fedora:        sudo dnf install gcc-c++ cmake zlib-devel libzstd-devel
# macOS:         brew install cmake zstd
cmake -S . -B build
cmake --build build -j
./build/xisfconv --version        # lists the enabled codecs
sudo cmake --install build        # optional
```

Windows (vcpkg): `vcpkg install zlib zstd`, then configure with
`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`.

## Usage

```
xisfconv [options] <file.xisf>...

  -t, --to <fits|tiff|png>    output format (default: fits, or taken from -o's extension)
  -o, --output <file>         output file name (single input only)
  -d, --outdir <dir>          directory for output files (default: next to each input)
  -f, --force                 overwrite existing output files
  -b, --bits <fmt>            output sample format: u8, u16, u32, f32, f64 (default: as stored)
  -i, --image <n>             convert only image n (0-based); default: all images
  -c, --compress              TIFF: Deflate compression with predictor
  -s, --stretch[=mode]        screen stretch for viewing: auto (default), linked, unlinked, stf
      --top-down              FITS: keep XISF's top-down row order (default: bottom-up)
      --no-property-keywords  FITS: don't derive missing keywords from XISF properties
      --no-wcs                FITS: don't write WCS from a PixInsight astrometric solution
      --sip-order <n>         FITS: SIP distortion order (2-7, default 3; 0 = linear only)
      --no-verify             don't verify data block checksums
  -I, --info                  print image geometry, keywords and properties; no conversion
      --dump-header           print the raw XML header; no conversion
  -q, --quiet                 suppress warnings
```

Output is written to `<name>.part` and renamed when complete, so an interrupted run never leaves a
half-written file under the final name. With several inputs, a failing file is reported and the
rest are still converted (exit status 1).

## Testing

```
pip install numpy astropy tifffile imagecodecs xisf lz4 zstandard pillow
python3 tests/run_tests.py build/xisfconv
```

Test inputs come from two independent writers: the `xisf` PyPI package (all codecs ± shuffling,
5 sample formats, gray and RGB) and a small encoder in the test script for the features that package
doesn't produce (Normal storage, big-endian, inline/embedded blocks, subblocks, checksums, CFA, ICC,
multiple images, tricky keywords, corrupt and truncated files). FITS output is checked with astropy
and, if installed, NASA's `fitsverify`; TIFF output is decoded with libtiff's `tiffcp` and tifffile;
PNG output with an independent decoder in the test script, Pillow and `pngcheck`; WCS output is
checked against synthetic astrometric solutions (with and without distortion) through astropy.

## Limitations / not yet done

- Distributed XISF units (`.xish` + `.xisb`) are not supported, only monolithic `.xisf` files.
- Complex sample formats and images with more than two dimensions are skipped.
- SHA3 checksums are not verified.
- CIELab images are written as raw 3-channel data without color conversion.
- TIFF output is classic TIFF (4 GiB limit); BigTIFF is not implemented.
- WCS keywords already present in the XISF header are copied unchanged (they're expected to match
  the bottom-up order); WCS generated from a PixInsight solution follows the chosen row order.
- The PixInsight spline distortion model is approximated by SIP polynomials, not carried over exactly.
- Please report any file that fails to convert, ideally with `xisfconv --info` output.

## Releasing

Bump the version in `src/common.hpp` and `CMakeLists.txt`, commit, then push a matching tag:

```
git tag v0.3.2 && git push origin v0.3.2
```

CI builds and tests all three platforms and, only if every one passes, publishes a GitHub release with
the packaged binaries. A tag that doesn't match the program version fails the build.

## Verified against PixInsight

Tested with files saved by PixInsight 1.9.3 (XISF module 1.1.3): Float32, Float64 and UInt32 images
compressed with zlib, LZ4, LZ4HC and Zstandard (all with byte shuffling), and SHA-1/SHA-256/SHA-512
checksums. Every variant decodes bit-identical to the original data and to an independent decoder,
and a corrupted byte is caught by each checksum type. Pixel data matches PixInsight's own FITS export
exactly (row order aside), and the WCS generated from a plate solution was confirmed by Siril's
annotation of the converted image.

## License

Copyright (C) 2026 Jurgen Kobierczynski

xisfconv is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version. See [LICENSE](LICENSE).

XISF and PixInsight are products of Pleiades Astrophoto S.L.; xisfconv is an independent
implementation of the published XISF 1.0 specification and is not affiliated with them.
