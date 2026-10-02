# xisfconv

A small, dependency-light command-line converter between PixInsight **XISF** and **FITS**, in both
directions, with **TIFF** and **PNG** export from XISF.

```
xisfconv M31_integration.xisf                 # -> M31_integration.fits
xisfconv -c light_0001.fits                   # -> light_0001.xisf (zstd-compressed)
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
  regenerated, long names use HIERARCH, long strings are split over CONTINUE cards
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

**FITS → XISF** (a FITS input is converted to XISF automatically)
- Reads the primary HDU and IMAGE extensions: BITPIX 8/16/32/64/-32/-64, 2-D images and 3-D cubes
  (channels). Each image HDU becomes an XISF image; tables are skipped with a warning.
- Sample mapping: the standard unsigned conventions (BZERO = 32768, 2^31, 2^63) become UInt16/32/64
  exactly; signed integers without negative values become unsigned of the same width; signed data
  with negative values, or any other BSCALE/BZERO, becomes Float32 (8/16-bit) or Float64 (32/64-bit)
  with the scaling applied.
- Floating point data gets the mandatory XISF `bounds`: `0:1` when the data fits, else `0:65535` when
  it fits (ADU-scaled floats, as Siril and ASTAP write), else the data's minimum and maximum.
  `--bounds lo:hi` overrides. The pixel values themselves are never rescaled.
- Rows are flipped to XISF's top-down order unless the file says `ROWORDER = 'TOP-DOWN'`
  (`--top-down` / `--bottom-up` override), and BAYERPAT follows the flip. A 2x2 RGB BAYERPAT also
  becomes an XISF ColorFilterArray element.
- Every non-structural keyword is carried over as an XISF FITSKeyword, in order, including HISTORY and
  COMMENT, HIERARCH names and long strings split over CONTINUE cards.
- Astrometry travels as WCS keywords in the FITS bottom-up convention, which is what PixInsight
  expects in XISF files; keywords of top-down FITS files are converted (CRPIX2, CD/PC, SIP terms).
- The solution is also written as PixInsight's native `PCL:AstrometricSolution` properties, because
  PixInsight reads only the linear part of WCS keywords. A SIP distortion model becomes a spline
  world transformation: the SIP polynomials are sampled on a grid of control points covering the
  image, from which PixInsight rebuilds its thin plate splines. Supported for RA/Dec axes with a
  zenithal projection (TAN, STG, ZEA, SIN, ARC) given as a CD matrix, PC + CDELT or CDELT + CROTA2.
  `--no-wcs` leaves the properties out.
- Output is a monolithic XISF 1.0 file. `-c` compresses with Zstandard + byte shuffling (the same
  settings PixInsight uses; `--codec zlib` for zlib), blocks over 1 GiB are written as subblocks, and
  `--checksum sha1|sha256|sha512` adds an integrity checksum.

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
xisfconv [options] <file>...      # XISF -> FITS/TIFF/PNG, FITS -> XISF

  -t, --to <fits|tiff|png|xisf>  output format (default: fits for XISF input, xisf for FITS input)
  -o, --output <file>         output file name (single input only)
  -d, --outdir <dir>          directory for output files (default: next to each input)
  -f, --force                 overwrite existing output files
  -b, --bits <fmt>            output sample format: u8, u16, u32, f32, f64 (default: as stored)
  -i, --image <n>             convert only image n (0-based); default: all images
  -c, --compress              TIFF: Deflate with predictor; XISF: zstd + byte shuffling
  -s, --stretch[=mode]        screen stretch for viewing: auto (default), linked, unlinked, stf
      --top-down              to FITS: keep XISF's top-down row order (default: bottom-up)
                              from FITS: the rows are stored top-down (don't flip them)
      --bottom-up             from FITS: the rows are stored bottom-up, whatever ROWORDER says
      --no-property-keywords  FITS: don't derive missing keywords from XISF properties
      --no-wcs                to FITS: don't write WCS from a PixInsight astrometric solution
                              to XISF: don't write PixInsight solution properties from WCS
      --sip-order <n>         FITS: SIP distortion order (2-7, default 3; 0 = linear only)
      --no-verify             don't verify data block checksums
      --codec <zlib|zstd>     XISF output: compression codec (implies -c)
      --checksum <sha1|sha256|sha512>  XISF output: checksum of the pixel data
      --bounds <lo:hi>        XISF output: range of floating point data
  -I, --info                  print image geometry, keywords and properties; no conversion
      --dump-header           print the raw XML header (XISF) or all keywords (FITS)
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

For FITS → XISF, the inputs are written by astropy (every BITPIX, signed and unsigned, BSCALE/BZERO,
cubes, several HDUs, CONTINUE and HIERARCH cards) and astropy's own reading of each file is the
reference. The XISF output is read back by the `xisf` package, and by a separate decoder in the test
script for what that package lacks (subblocks, UInt64); checksums are verified there as well. Round
trips XISF → FITS → XISF and FITS → XISF → FITS must return identical pixels, keywords and WCS.

## Limitations / not yet done

- Distributed XISF units (`.xish` + `.xisb`) are not supported, only monolithic `.xisf` files.
- Complex sample formats and images with more than two dimensions are skipped.
- SHA3 checksums are not verified.
- CIELab images are written as raw 3-channel data without color conversion.
- TIFF output is classic TIFF (4 GiB limit); BigTIFF is not implemented.
- WCS keywords in an XISF header are taken to follow the FITS bottom-up convention (PixInsight's);
  they are converted when writing top-down FITS. Distortion models other than SIP (TPV, TNX) are
  copied without that conversion.
- FITS → XISF writes the solution properties in the layout PixInsight 1.9.3 uses. Other XISF
  properties (observation time, instrument) are not created; PixInsight derives those from the
  keywords. Distortion other than SIP (TPV, TNX) and non-zenithal projections stay keyword-only.
- A PixInsight spline solution that goes XISF → FITS → XISF comes back as a spline rebuilt from the
  SIP approximation, not as the original: on the test frame the two agree to 0.5 arcsec rms.
- FITS input: tile-compressed images (fpack) and tables are not read; BLANK pixels of integer images
  are kept as ordinary values; FITS can only be converted to XISF, not directly to TIFF or PNG.
- XISF output is not compressed with LZ4 (zlib and Zstandard only).
- The PixInsight spline distortion model is approximated by SIP polynomials, not carried over exactly.
- Please report any file that fails to convert, ideally with `xisfconv --info` output.

## Releasing

Bump the version in `src/common.hpp` and `CMakeLists.txt`, commit, then push a matching tag:

```
git tag v0.5.0 && git push origin v0.5.0
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

In the other direction, PixInsight's FITS export converts back to an XISF whose pixels are identical
to PixInsight's own XISF of the same image. PixInsight 1.9.3 opens xisfconv's XISF files, including
Zstandard-compressed ones with byte shuffling, and loads the astrometric solution from the WCS
keywords with the orientation and reference pixel xisfconv intends.

PixInsight reads only the linear part of WCS keywords (it reports "WCS transformation: Linear") and
ignores the SIP distortion terms: on the 4656 x 3520 test frame that leaves the image center exact
and the corners off by about 15 arcseconds. For that reason xisfconv also writes the solution as
PixInsight's native properties. The control points it generates agree with PixInsight's own
distortion model of the same image to 0.5 arcseconds rms. PixInsight 1.9.3 accepts the properties
and rebuilds its splines from the control points, which are written for exact interpolation (no
smoothing, no surface simplification). The image bounds PixInsight then reports agree with the SIP
model to under 0.01 arcseconds, once one behaviour of PixInsight itself is taken into account: at
the very border of the image its 8-pixel point grid returns the value belonging to a point
1.33 pixels inside, so the printed corner coordinates sit about 1.4 arcseconds inside the true
corners, with `ex`/`ey` round-trip errors of 1 to 2 pixels there.

## License

Copyright (C) 2026 Jurgen Kobierczynski

xisfconv is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version. See [LICENSE](LICENSE).

XISF and PixInsight are products of Pleiades Astrophoto S.L.; xisfconv is an independent
implementation of the published XISF 1.0 specification and is not affiliated with them.
