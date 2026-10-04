# Development decisions

What was decided while building xisfconv, and why. The README says what the program does and
`TODO.md` what is planned; this file records the choices behind both, so that they are not
reopened by accident. State: version 0.9.2, 5 October 2026.

## Purpose and scope

- xisfconv converts between PixInsight XISF, FITS and ASDF in every direction and exports TIFF and
  PNG from all three. It also rewrites XISF files (compression, checksums) and verifies files.
- The centre is XISF and the conversions. FITS and ASDF are supported as far as images need them:
  no tables, no general header editing, no ASDF data models. For those, CFITSIO, astropy and the
  Python `asdf` package are the tools to use.
- TIFF and PNG are output only.
- A feature is only called done when its output has been compared with an independent
  implementation (see "Testing").

## Licence

- The command line tool is GPL-3.0-or-later (since commit `de1a3a3`).
- The library (libxisfconv, see below) will be LGPL-3.0-or-later, so that programs under other
  licences can link it. The tool stays GPL-3.0-or-later. "Or later" was chosen for both, to keep
  them consistent.
- The licence headers of the library sources change, and the LGPL text is added, with the patch
  that introduces the C API. Until then every file carries the GPL tag.

## Language, build and dependencies

- C++17, CMake 3.15 or later, warnings on (`-Wall -Wextra -Wpedantic`, `/W4`) and kept at zero.
- The only required dependency is zlib; libzstd is optional (`XISFCONV_WITH_ZSTD`). Everything
  else is in the source: XML and YAML readers, LZ4 decoder, MD5, SHA-1, SHA-2 and SHA-3, Rice and
  PLIO decoders, the FITS, ASDF, TIFF and PNG readers and writers.
- Release binaries are self-contained (`XISFCONV_PORTABLE`: static zstd and C++ runtime), built by
  CI for Linux x86_64, macOS arm64 and Windows x64 when a version tag is pushed. A tag that does
  not match the program version fails the build.

### Own code instead of existing libraries

Building on libXISF, CFITSIO, libtiff, libpng and libasdf was considered and rejected:

| Library | Would bring | Why not |
|---|---|---|
| libXISF | an existing XISF implementation | GPL-3.0-or-later, which a LGPL library cannot link; header patching and block verification need our own parser anyway |
| CFITSIO | tables, HCOMPRESS, writing `.fits.fz` | a large dependency to bundle on three platforms and in Python wheels; thread-safe only in its reentrant build; our tests use funpack and astropy as independent references, which they would no longer be |
| libtiff | BigTIFF, more compressions, TIFF reading | we only write TIFF, a few hundred lines; it pulls in further optional dependencies |
| libpng | the reference implementation | we only write PNG, a small layer over zlib |
| libasdf | follows the ASDF standard, gwcs extension | version 0.2.0 and read-only; four more dependencies |

The price is that the FITS, ASDF, TIFF and PNG code duplicates what those libraries do. It is
kept small by the scope above. CFITSIO is the one to reconsider, as an optional backend, if FITS
tables or HCOMPRESS are ever wanted.

## Format conventions

These are the choices a user could otherwise be surprised by. Each has an option to override it.

- **Row order.** XISF stores rows top-down, FITS viewers expect the first row at the bottom. XISF
  to FITS and ASDF flips the rows and writes `ROWORDER = 'BOTTOM-UP'`; `--top-down` keeps them.
  FITS and ASDF input is taken as bottom-up unless `ROWORDER` says otherwise. `BAYERPAT` and the
  WCS keywords follow the flip.
- **Pixel values are never rescaled** unless `--bits` or `--stretch` asks for it. Unsigned FITS
  conventions (BZERO 32768, 2^31, 2^63) map exactly to UInt16/32/64; signed data with negative
  values becomes floating point with the scaling applied.
- **Bounds of floating point data** from FITS and ASDF: `0:1` if the data fits, else `0:65535` if
  it fits, else minimum and maximum; `--bounds` overrides. The same range is black to white for
  TIFF and PNG.
- **Keywords.** Every non-structural FITS keyword is carried over in order, including COMMENT,
  HISTORY, HIERARCH and CONTINUE. Keywords that exist always win over values derived from XISF
  properties or from an astrometric solution.
- **Astrometry.** A PixInsight solution is written as standard WCS keywords, its spline distortion
  fitted with SIP polynomials (order 3 by default). In the other direction the solution is written
  both as WCS keywords and as PixInsight's native properties, because PixInsight reads only the
  linear part of WCS keywords. The control points are written for exact interpolation (no
  smoothing, no simplifiers).
- **ASDF** output is a FITS HDU list under the tree's `fits` key (astropy's tag), so that
  `asdf` with `asdf-astropy` returns an astropy `HDUList`; no gwcs objects or data models. The
  YAML reader types plain scalars the way PyYAML does, because that is what Python's asdf uses.
  MD5 checksums are accepted in both conventions (asdf 2.x and the standard).
- **XISF to XISF** edits the header as text: only the `location`, `compression`, `subblocks` and
  `checksum` attributes of attached blocks change, plus the metadata describing them. Everything
  else, including unknown elements, is copied byte for byte. A block whose checksum is of an
  unknown kind is copied as it is, never re-stored.
- **Checksums.** SHA3-256 and SHA3-512 are in the XISF 1.0 specification and are read and written,
  but PixInsight 1.9.3 implements only SHA-1, SHA-256 and SHA-512 and refuses an image that carries
  another one. Writing a SHA-3 checksum is therefore allowed, with a warning: the file is valid,
  and what the specification allows is not withheld because one reader lacks it.
- **Tile-compressed FITS** is read, not written. Quantized floating point is restored exactly as
  CFITSIO restores it, including its dithering sequence; that it is lossy is stated in the README.
- **Stretch** is for viewing: PixInsight's STF maths, the saved STF if there is one, else a linked
  auto-STF. It is available for TIFF, PNG and, from XISF, FITS and ASDF output, and is recorded in
  a HISTORY card.
- **Verification outcomes** are OK, NOT FULLY CHECKED (a part this build cannot check, named) and
  FAILED. Only FAILED sets exit status 1.

## Care with files

- Output is written to `<name>.part` and renamed when complete. An existing `.part` file is not
  overwritten without `--force`, and never when it is the input.
- An existing output is not overwritten without `--force`; an output that is the input is refused.
- `--in-place` writes the new file next to the original, reads it back and compares every block,
  copies the permissions, flushes to the disk and only then renames. Read-only files are refused,
  symbolic links are followed, files already stored as requested are left alone.
- Checksums of the input are verified by default (`--no-verify` skips it). A damaged input is
  refused, not given a fresh checksum.
- A size in a header that the stored data cannot account for is refused before memory is allocated
  for it, wherever the format allows that check.
- With several inputs, one failing file does not stop the others; the exit status is 1.

## Structure of the code

- One module per format or concern in `src/`: `xisf`, `xisfwrite`, `xisfrewrite`, `fits`,
  `fitsread`, `fitstile`, `asdf`, `yaml`, `xml`, `tiff`, `png`, `wcs`, `convert` (sample formats,
  stretch), `codecs` (compression, digests), `common`.
- `pipeline` holds the conversion of whole files. It takes an options struct and prints nothing.
- `main.cpp` is the command line only: arguments, output names, `--info`, `--verify`, printing.
- CMake builds a static core library from everything but `main.cpp`; the executable links it.
- The modules do not print. Warnings and notes go to a message handler installed per thread
  (`MessageScope`); the tool's handler prints them. There is no other global mutable state.
- Errors are exceptions: `xisfconv::Error`, and `Unsupported` for a feature this build lacks while
  the file may be fine. `Unsupported` becomes NOT FULLY CHECKED in `--verify`.
- FITS and ASDF input share one in-memory form (`FitsFile`), so every conversion from them is
  written once.

## The library (libxisfconv)

Planned in a separate brief (`LIBRARY-HANDOFF.md`, with a draft header `xisfconv.h`) and decided
as follows. The steps and their state are in `TODO.md`.

- The C++ code stays the engine. A plain C API in `xisfconv.h` is the only public interface;
  every language binds to it, C++ included (a header-only wrapper), so no C++ ABI is exposed.
- The command line tool is rebuilt on the C API alone. That is the proof that the API is complete.
- Function prefix `xisfconv_`, macros and enumerators `XISFCONV_`. The draft's `xc_` is taken by
  libxc (which has its own `xc_version`) and by Xen's libxenctrl.
- Presented as "XISF, and conversion between XISF, FITS and ASDF", not as a general FITS or ASDF
  library.
- Writing images from memory and the stretch on buffers are in the first release, for all five
  output formats: saving an array as XISF is what Python users cannot get elsewhere.
- Version 0.x with no ABI promise until two bindings have used the API. The shared library version
  changes with every 0.x release, so that a binding built for another release fails to load.
- Bindings: Python first (NumPy arrays; it can register `xisf` with astropy's I/O registry), then
  Rust and Perl when someone asks for them.
- Delivered in three patches: the internal refactor (0.9.1, done), the C API with the tool rebuilt
  on it, the Python binding.
- Changes to the draft header: enumerations inside structs as `int32_t`; a file handle keeps its
  context alive; one type for image indices; FITS to FITS is a conversion; the ASDF tree JSON test
  hook stays out of the documented API; a progress and cancel callback before the first binding
  is published.

## Testing

- `tests/run_tests.py` drives the built program (3447 checks at 0.9.1). The Python packages it
  needs are listed at its top; the `asdf` packages and the external tools (`tiffcp`, `fitsverify`,
  `pngcheck`, `fpack`/`funpack`) are used when installed and their checks skipped when not.
- Every format is checked against an implementation that shares no code with xisfconv: astropy
  (FITS, WCS, tile compression), the `xisf` package and a separate decoder in the test script
  (XISF), Python's `asdf` with `asdf-astropy` and PyYAML (ASDF), tifffile, libtiff's tools, Pillow
  and `pngcheck` (TIFF, PNG), `fitsverify`, `fpack`/`funpack`, `hashlib`.
- Round trips must return identical pixels, keywords and WCS.
- Damaged and truncated files are part of the suite: each must be refused with a message, and an
  original must be left byte for byte as it was.
- Before a release the suite also runs under AddressSanitizer and UBSan and with a clang build, and
  new readers are fuzzed (mutated headers, data and truncations).
- A refactor is checked by running the old and the new binary on the same invocations and comparing
  exit status, stdout, stderr and every file written.
- Real files matter: PixInsight 1.9.3 output in every codec and checksum, and PixInsight opening
  what xisfconv writes. What was verified that way is listed in the README. A feature that follows
  the specification is not proven until PixInsight has opened its output: SHA-3 checksums passed
  every test here and were refused by PixInsight.
- CI builds and runs the suite on Linux, macOS and Windows.

## How changes are made

- One commit per feature, with the version bumped in `src/common.hpp` and `CMakeLists.txt` and the
  README and `TODO.md` updated in the same commit.
- The work is done together with Claude (Anthropic), credited as co-author in the commit messages.
  Each change is delivered as a `git format-patch` file to apply with `git am`, plus a source
  archive, after it has been built and tested on a clean clone.
- Large features (so far ASDF and XISF rewriting) get an independent review pass before delivery;
  its findings are fixed first.
- Limitations are written into the README when a feature ships, not left to be discovered.
- Messages to the user say what happened and what to do about it; nothing is skipped silently.

## Not decided yet

- Whether error messages that name command line options (`--force`, `--bounds`) stay in the library
  or are added by the tool.
- Packaging of the Python binding (cffi or ctypes; wheel builds).
- Order of the remaining items in `TODO.md` after the library.
