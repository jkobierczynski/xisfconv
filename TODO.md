# To do

Planned features, roughly in order. Done items move to the README; the decisions behind them are
in `DEVELOPMENT.md`.

- [x] TIFF and PNG export from FITS and ASDF input, with `--stretch` and `--bits` (0.7.0)
- [x] XISF -> XISF rewriting: recompress or decompress existing files, add or remove checksums,
      extract one image of a multi-image file, `--in-place`; a `--verify` mode that checks XISF, FITS
      and ASDF files (and directories of them) without converting (0.8.0)
- [x] Read tile-compressed FITS (`.fits.fz`: RICE_1, GZIP_1, GZIP_2, PLIO_1, NOCOMPRESS, quantized
      and lossless floating point) instead of asking for funpack; `-t fits` unpacks (0.9.0)
- [x] **libxisfconv**: the converter as a library with a plain C API (0.10.0); its Python package
      is next (see "Library" below)
- [ ] Lossless property round trip: carry all XISF properties through FITS (HIERARCH keywords) and
      ASDF (tree entries) and restore them on the way back
- [ ] Downsampling (`--resize` / `--bin`) for TIFF and PNG export, and a `.thumbnailer` entry so Linux
      file managers show previews of `.xisf` and `.fits` files
- [ ] More platforms and packaging: Linux arm64 and Intel macOS release builds; Homebrew formula,
      AUR package, winget manifest; man page

Smaller items, each closing a limitation listed in the README:

- [ ] BigTIFF for output over 4 GiB
- [ ] Distributed XISF units (`.xish` + `.xisb`)
- [ ] JPEG output
- [ ] TPV distortion alongside SIP
- [ ] Recursive directory conversion (directories are accepted by `--verify` only)
- [ ] Wildcard expansion on Windows (`*.xisf` is not expanded by cmd or PowerShell)
- [ ] Write CHECKSUM / DATASUM keywords in FITS output
- [ ] Write tile-compressed FITS (`-t fits -c`: RICE_1 for integers, GZIP_2 for floating point)
- [ ] Read HCOMPRESS_1 tile compression

## Library

The decisions are recorded in `DEVELOPMENT.md`; the API is `include/xisfconv.h`.

1. [x] Build split: a core library with everything but `main.cpp` (0.9.1).
2. [x] The conversion logic in `src/pipeline.cpp`, without printing and without globals (0.9.1).
3. [x] The C API (`include/xisfconv.h`, `src/capi.cpp`): files, images, keywords, properties, pixels,
   astrometry, conversion, rewriting, verification, writing arrays, stretch (0.10.0).
4. [x] The command line tool on `xisfconv.h` alone, its output unchanged (0.10.0).
5. [x] Shared library, exported symbols only, install rules, pkg-config and CMake package files
   (0.10.0).
6. [x] Tests of the library: a C test program built by a C compiler, error paths, ASan/UBSan,
   fuzzing through the API, a check that nothing is written to stdout or stderr (0.10.0).
7. [ ] Python package: NumPy arrays in and out, wheels, `CCDData.read("image.xisf")` through
   astropy's I/O registry; then the existing oracles run through it.

Later, each when it is needed:

- [ ] A CMake package for the static library (the dependencies have to be described to the consumer).
- [ ] Library messages without the tool's option names.
- [ ] Check in PixInsight that it accepts the inline ICC profile the writer stores in XISF.
- [ ] Reading from and writing to memory instead of files, and input that cannot be rewound
      (a pipe).
- [ ] Reading one image of a FITS or ASDF file without going through the headers before it again
      (a file with a thousand HDUs is read image by image in half a minute).
- [ ] Rust and Perl bindings.

Open check:

- [ ] Confirm the tag-release job on macOS and Windows publishes working binaries
