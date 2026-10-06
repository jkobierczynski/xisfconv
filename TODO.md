# To do

Planned features, roughly in order. Done items move to the README; the decisions behind them are
in `DEVELOPMENT.md`.

- [x] TIFF and PNG export from FITS and ASDF input, with `--stretch` and `--bits` (0.7.0)
- [x] XISF -> XISF rewriting: recompress or decompress existing files, add or remove checksums,
      extract one image of a multi-image file, `--in-place`; a `--verify` mode that checks XISF, FITS
      and ASDF files (and directories of them) without converting (0.8.0)
- [x] Read tile-compressed FITS (`.fits.fz`: RICE_1, GZIP_1, GZIP_2, PLIO_1, NOCOMPRESS, quantized
      and lossless floating point) instead of asking for funpack; `-t fits` unpacks (0.9.0)
- [x] **libxisfconv**: the converter as a library with a plain C API (0.10.0), and its Python
      package: NumPy arrays, astropy `CCDData` and HDU lists (0.11.0); see "Library" below
- [x] Write tile-compressed FITS (`-c` on FITS output, `image.fits.fz`): lossless, RICE_1 for
      integers and GZIP_2 for floating point, the tiles and the layout of fpack (0.12.0)
- [x] Lossless property round trip: every XISF property goes along to FITS (a table behind each
      image) and ASDF (the tree) with its type and exact value, and is restored on the way back,
      the astrometric solution as PixInsight wrote it (0.13.0)
- [x] Downsampling (`--bin`, `--resize`) for TIFF and PNG export, and a thumbnailer entry with
      the file types, so that Linux file managers show previews of XISF, FITS and ASDF files
      (0.14.0)
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
- [ ] Read HCOMPRESS_1 tile compression
- [ ] Tile-compressed FITS output: quantized floating point as an option (lossy, fpack's default
      for floats and much smaller), a choice of tile shape
- [ ] Previews: use the thumbnail an XISF file holds when it has one (no pixels to read); a
      plugin for KDE's Dolphin; a look at what Windows Explorer and macOS Quick Look need
- [ ] `--bin` for FITS, ASDF and XISF output: with the WCS, the astrometric solution, the pixel
      size keywords and the colour filter pattern changed to match
- [ ] Take the rest of an XISF image along to FITS and ASDF the way the properties go: the saved
      screen stretch, the resolution, the ICC profile, the thumbnail and the image attributes
      that no keyword says
- [ ] Mark the keywords a conversion from XISF adds (derived from properties, made from a
      solution) so that the way back can leave them out, and XISF -> FITS -> XISF returns the
      keywords as they were

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
7. [x] Python package: NumPy arrays in and out, wheels, `CCDData.read("image.xisf")` through
   astropy's I/O registry; the existing oracles run through it (0.11.0).

Open checks of the Python package:

- [x] The wheels for macOS and Windows: for v0.12.1 the `wheels` workflow built and tested all
      of them (Linux x86_64 and arm64, macOS arm64, Windows x64) and the source distribution.
- [ ] Register the project on PyPI and switch publishing on (see "Releasing" in the README).

Later, each when it is needed:

- [ ] Library and Python: write XISF properties from the caller's values (`xisfconv_image` and
      `write` take none yet; `convert` carries those of a file since 0.13.0); read vectors and
      matrices in their own element type instead of as float64, complex ones included; read and
      write an image in pieces instead of as a whole (which would also let Ctrl-C stop the reading
      or writing of one large image: it is one step now); wheels for musllinux and Intel macOS;
      type stubs.

- [ ] A CMake package for the static library (the dependencies have to be described to the consumer).
- [ ] Library messages without the tool's option names.
- [ ] Check in PixInsight that it accepts the inline ICC profile the writer stores in XISF.
- [ ] Reading from and writing to memory instead of files, and input that cannot be rewound
      (a pipe).
- [ ] Reading one image of a FITS or ASDF file without going through the headers before it again
      (a file with a thousand HDUs is read image by image in half a minute).
- [ ] Rust and Perl bindings.

Open check:

- [ ] Install the thumbnailer on a desktop and look at a folder of frames: in GNOME Files (which
      runs it in a sandbox), and in one of Nemo, Caja and Thunar
- [ ] Open in PixInsight an XISF file that came back from FITS with its properties: it should
      report the astrometric solution of the original and show the processing history
- [ ] Confirm the tag-release job on macOS and Windows publishes working binaries
