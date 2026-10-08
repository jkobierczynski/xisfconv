# To do

Planned features, roughly in order. Done items move to `MANUAL.md`; the decisions behind them are
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
- [x] Python: the interface of the `xisf` package (`from xisfconv.xisf import XISF`), so that
      programs written for it run on the library; XISF properties written from Python values and
      from the C API, vectors and matrices read in the type of their elements; LZ4 and LZ4HC
      written to XISF, with a compression level (0.15.0)
- [x] Distributed XISF units: a header file (`.xish`) and its data blocks file (`.xisb`), and
      blocks in other files (`path(...)`), read and written by the tool, the library and the
      Python package; packing and unpacking; a setting for which files a header is followed to
      (0.16.0)
- [x] A manual of the library for C, C++ and Python (`docs/manual.html`): one file, with a tour of
      example programs in the three languages that the tests compile and run, and the reference
      of the C API and of the Python package made from the header and the docstrings (0.16.0)
- [x] The documents around the program: `CHANGELOG.md`, `SECURITY.md`, `CONTRIBUTING.md` with
      issue templates, `CITATION.cff`, a man page (`man/xisfconv.1`, installed with the tool), and
      the layout of the XISF properties in FITS and ASDF as a document for other programs
      (`docs/xisf-properties-in-fits-and-asdf.md`, with `examples/wcs_digest.py`) (0.16.0)
- [x] Whole folders: a directory as input, converted with what is below it, the files that
      already are what is asked for passed over, the tree kept below `-d`, `--skip-existing`;
      and patterns (`*.xisf`) expanded by the program, so that they work in cmd and PowerShell
      (0.17.0)
- [x] DNG input: the raw image of a DNG file (uncompressed, lossless JPEG, Deflate), with its
      colour filter pattern and the exposure as keywords, in the tool, the library and the Python
      package (0.18.0); and `--debayer` for colour pictures of a mosaic, in TIFF and PNG (0.18.1)
- [ ] More platforms and packaging: Linux arm64 and Intel macOS release builds; Homebrew formula,
      AUR package, winget manifest

Smaller items, each closing a limitation listed in `MANUAL.md`:

- [ ] BigTIFF for output over 4 GiB
- [ ] `--debayer`: an interpolation that fringes less (VNG, or AMaZE's kind), X-Trans patterns,
      and the white balance of a DNG file (`AsShotNeutral`) as an option
- [ ] DNG: compute the `NewRawImageDigest` in `--verify`; read JPEG XL compressed raw data (DNG
      1.7); the raw files of cameras directly (through LibRaw, as an optional dependency)
- [ ] Distributed units: several data blocks files for one unit when writing (one per image),
      and a preview of `.xish` files in GNOME Files (its sandbox holds the header file alone)
- [ ] JPEG output
- [ ] TPV distortion alongside SIP
- [ ] Directories: a run that shows what it would do and does nothing (`--dry-run`); converting
      again what changed since its output was written (by the dates); several files at a time
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

Found when the documents were checked against the program (October 2026), and not changed yet:

- [ ] The digest of the WCS is taken from the keywords of the XISF image before the FITS writer
      has them. A WCS keyword that cannot be written as it is (text outside ASCII or with a tab,
      a number too long for a card or too large for a double, a complex value with a lower-case
      exponent) is written changed or left out, and xisfconv then does not find its own digest
      on the way back: the solution is made from the keywords, though nothing changed. Through
      ASDF the same happens to a value that the tree writes another way (`(1,2)`). The digest
      should be taken from what is written.
- [ ] A FITS file that ends inside the header of an `XISF_PROPERTIES` table converts without the
      properties and without a warning (what is behind the last whole HDU counts as something
      that does not belong to the file). `--verify` reports it; a conversion should warn.
- [ ] `MANUAL.md` says a refusal does not tell whether there is something where a symbolic link
      leads. For a path that goes through a link and back (`link/../name`, the link leading to a
      directory outside) the message differs between a file that is there and one that is not.
- [ ] `--dump-header` of a FITS file prints what `--info` prints (the keywords without the
      structural ones, and the properties), where the help says "all keywords". Either the
      cards as they are, or other words.

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
- [ ] Register the project on PyPI and switch publishing on (see "Releasing" in `DEVELOPMENT.md`).

Later, each when it is needed:

- [ ] Library and Python: read and write an image in pieces instead of as a whole (which would
      also let Ctrl-C stop the reading or writing of one large image: it is one step now); wheels
      for musllinux and Intel macOS; type stubs.
- [ ] A compression level and byte shuffling off for conversions and rewrites, and for the tool
      (`--level`): the writer of images from memory has both since 0.15.0.
- [ ] `xisfconv.xisf`: the resolution, the ICC profile and the thumbnail of an image, which the
      `xisf` package leaves out as well; table properties.
- [ ] Write LZ4 blocks to ASDF (its own layout of chunks, which is read).
- [ ] Hand a data block of a property type without a name over the API as its bytes, so that
      `read_image` and `write` carry it as a conversion does; and tables.
- [ ] XML attribute values as XML normalizes them (a line break written as such inside an
      attribute is a blank), and a header text with both literal and referenced carriage returns
      written back as it was.

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
- [ ] Open in PixInsight an XISF file written with `--codec lz4` and one with `--codec lz4hc`,
      and a plate-solved frame that was read and written again through `xisfconv.xisf` (0.15.0)
- [ ] Check in PixInsight that a String property stored as `location="inline:base64"` is read: a
      new text with a carriage return or with white space at its ends is written that way
      (0.15.0). (PixInsight writes long Strings as attached blocks itself, when it compresses.)
- [ ] Distributed units (0.16.0) were checked against the specification and OpenXISF 0.5.0, and
      not with PixInsight, which opens monolithic files only: if a version of PixInsight reads
      `.xish`, open one written by xisfconv
- [ ] Confirm the tag-release job on macOS and Windows publishes working binaries
