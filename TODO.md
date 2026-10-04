# To do

Planned features, roughly in order. Done items move to the README.

- [x] TIFF and PNG export from FITS and ASDF input, with `--stretch` and `--bits` (0.7.0)
- [x] XISF -> XISF rewriting: recompress or decompress existing files, add or remove checksums,
      extract one image of a multi-image file, `--in-place`; a `--verify` mode that checks XISF, FITS
      and ASDF files (and directories of them) without converting (0.8.0)
- [x] Read tile-compressed FITS (`.fits.fz`: RICE_1, GZIP_1, GZIP_2, PLIO_1, NOCOMPRESS, quantized
      and lossless floating point) instead of asking for funpack; `-t fits` unpacks (0.9.0)
- [ ] **libxisfconv**: the converter as a library with a plain C API, for C, C++, Python, Perl and
      Rust (see "Library" below)
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

Brief: `LIBRARY-HANDOFF.md` with `xisfconv.h` (draft 1 of the C API) and `example.c`, written
against 0.8.0.

Decided:

- The C++ code stays the engine; a plain C API in `xisfconv.h` is the only public interface, and
  every language binds to it (C++ through a header-only wrapper).
- The command line tool is rebuilt on the C API alone.
- Licence of the library: LGPL-3.0; the command line tool stays GPL-3.0-or-later.
- 0.x, no ABI promise until two bindings have used the API. Bindings: Python first, then Rust, Perl.

Open:

- Function prefix: `xc_` (draft) or `xisfconv_`. `xc_` is taken by libxc (`xc_version`,
  `xc_func_init`) and by Xen's libxenctrl.
- Whether writing images from memory (`xc_writer_*`) and the stretch on buffers are in the first
  library release.

Steps, each ending with the whole test suite green and the console output unchanged:

1. Build split: a static core library with everything but `main.cpp`; the executable links it.
2. Move the conversion logic (`convertXisfFile`, `convertFitsOrAsdfFile`, `rewriteXisfFile`, the
   `.part` handling) out of `main.cpp` into a module that takes an options struct and a message
   sink. Replace the globals `g_context` / `g_quiet` and `warn()`. Also: the lazily filled table
   in `base64Decode` (not thread-safe), an error kind in `xisfconv::Error` (I/O, format, checksum,
   exists, argument), UTF-8 paths on Windows for every file that is opened, and reading one FITS
   HDU or ASDF array at a time (`readFits` and `readAsdf` read all images of a file at once).
3. The C API over it, with one try/catch per entry point.
4. The command line tool on `xisfconv.h` alone.
5. Shared library, exported symbols only, install rules, pkg-config and CMake package files.
6. Tests of the library: a C test program built by a C compiler, error paths, ASan/UBSan, fuzzing
   through the API, a check that nothing is written to stdout or stderr.
7. Writer and stretch functions.
8. Python binding (NumPy arrays in and out), then the existing oracles run through it.

To change in the draft header (it predates 0.9.0):

- FITS to FITS is a conversion now (unpacking tile-compressed images), and a FITS image has a
  storage text (the tile compression).
- Enumerations inside structs as `int32_t`: the size of a C enum is up to the compiler, which
  matters to ctypes and FFI::Platypus.
- A file handle keeps its context alive (reference count), so the order of `xc_close` and
  `xc_context_free` cannot crash a binding's garbage collector.
- The image index is `size_t` in some places and `int64_t` in others; use one.
- A shared library version that changes with every 0.x release, so that a binding built for another
  release fails to load instead of misreading structs.
- `xc_asdf_tree_json` is a test hook: keep it out of the documented API.
- A progress and cancel callback in the context before the first binding is published.

Open check:

- [ ] Confirm the tag-release job on macOS and Windows publishes working binaries
