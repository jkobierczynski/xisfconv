# Development decisions

What was decided while building xisfconv, and why. The README says what the program does and
`TODO.md` what is planned; this file records the choices behind both, so that they are not
reopened by accident. State: version 0.12.0, 6 October 2026.

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
- The library (libxisfconv, see below) is LGPL-3.0-or-later, so that programs under other licences
  can link it. The tool stays GPL-3.0-or-later. "Or later" was chosen for both, to keep them
  consistent.
- Since 0.10.0 the first lines of each source file say which applies: `include/xisfconv.h`,
  everything in `src/` but `main.cpp`, the example and the build files are the library (LGPL);
  `main.cpp` and the tests are GPL. `COPYING.LESSER` holds the LGPL text, `LICENSE` the GPL it
  builds on.
- The Python package (`python/xisfconv`) is part of the library: LGPL. Its tests are GPL like the
  others. The source distribution of the package holds LGPL files only (no `main.cpp`, no tests).
- Binaries that are handed out (the release archives, the wheels) contain Zstandard and zlib.
  Zstandard's BSD licence asks for its notice to go with binaries: `THIRD-PARTY-NOTICES.md` is
  packed with both since 0.11.0. (The release archives up to 0.10.0 lacked it.)

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
- **Tile-compressed FITS** is read in every form but `HCOMPRESS_1`. Quantized floating point is
  restored as CFITSIO restores it, including its dithering sequence; that it is lossy is stated
  in the README.
- **Tile-compressed FITS is written without loss** (0.12.0, `-c` on FITS output). What was decided:
  - *Lossless only*: `RICE_1` for integers, `GZIP_2` for floating point. What makes fpack's
    floating point files small is quantization, which discards bits; a converter does not do that
    unasked. It can become an option of its own.
  - *A tile is a row*, the default of fpack and astropy, and gzip runs at zlib's level 6. Measured
    on real 32-bit floating point frames (4656 x 3520): the whole image as one tile would be 3 %
    smaller, level 9 another 0.2 %, level 1 0.8 % larger. None of that is worth leaving the
    layout every reader has seen most.
  - *The Rice encoder is CFITSIO's, bit for bit* (`ricecomp.c`: the split position of a block
    from its mean, differences in the arithmetic of the sample width). The tests can then compare
    bytes with astropy and fpack, which says more than decoding the result with the same reader
    that the encoder was written against.
  - *Nothing in the file names the system it was written on*: the gzip header carries no time
    and no operating system (zlib would write the one it was built on). The Rice-coded tiles are
    the same bytes everywhere; the gzip ones are as far as the zlib that is linked compresses
    alike, which holds for zlib itself and not for zlib-ng.
  - *The file is laid out as fpack lays it out*: an empty primary HDU, the first image marked
    `ZSIMPLE`, the others `ZTENSION`, and the cards of the image with the comments of the plain
    writer, so that funpack restores exactly the file xisfconv writes without `-c`.
  - *Keywords of a compressed image belong to the writer.* An image handed over with `ZSCALE`,
    `THEAP` or `TFORM1` among its keywords (found in the review: astropy and funpack then read
    wrong pixels or none) is written without them, with a warning. In a plain file they are
    keywords like any other.
  - *64-bit integers stay plain images.* CFITSIO 4.3 refuses to compress them and to decompress
    them ("Bad image datatype"); astropy writes them with gzip. A FITS file that CFITSIO cannot
    read is not worth the saving, and the type is rare. If such an image comes first it is the
    primary HDU, as in a plain file.
  - *The table is written last.* Its rows (size and place of every tile) and two numbers in the
    header are known only when the tiles are compressed: room is left for both, the tiles are
    written one by one, then the writer goes back. The memory needed is one row. Whether the
    rows are 32-bit or 64-bit descriptors has to be known before, so it is decided from the
    largest size the tiles could have, not from the size they turn out to have. The 64-bit form
    cannot be part of the test suite (it starts near 2 GiB of pixels): it was checked once with
    a 34000 x 32000 image of noise, and in the review with the limit lowered in a patched copy.
  - *A name that ends in `.fz` asks for compression*, in the tool and in the library. `--codec
    zstd` is an error for FITS output; before 0.12.0 `-c` and `--codec` were ignored there, so
    `xisfconv -c image.xisf` now writes `image.fits.fz` where it wrote `image.fits`.
  - *FITS to FITS with `-c` is a conversion*, like every other path: the images go through the
    reader (which maps signed integers with negative values to floating point) and tables are
    left out. A copy that only repacks the data would be another program inside this one; fpack
    is that program.
  - *The C API has no new fields*: `codec` of the conversion and writer options means tile
    compression for FITS, as it means Deflate for TIFF.
- **Arithmetic does not depend on the processor.** The code is compiled with
  `-ffp-contract=off`: every multiplication and addition is rounded by itself. GCC and Clang
  otherwise fuse `a*b + c` into one instruction on arm64, which changes the last bit of some
  results, and more than that where the product and `c` nearly cancel. That showed in 0.10.0 on Apple Silicon, in the values restored from quantized
  tile-compressed FITS: astropy restores undithered values with NumPy (never fused) and dithered
  ones with C code (fused there), so no build could agree with it on both. Since the reference
  itself is not of one mind about that rounding, the tests accept a difference of that size for
  quantized floating point (computed from the zero points in the file) and report how often it
  was needed (never on x86-64). With the option, the Linux builds for x86-64
  and arm64 convert the same file to the same bytes (FITS with a fitted WCS, a stretched PNG, XISF
  with solution properties, an unpacked `.fits.fz`); without it each of those differed. What the
  C library computes (sine, logarithm) may still differ between systems.
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
  Its second half, `writeImageSet`, writes images that are in memory to any format: FITS and ASDF
  input and the API's writer both end there, so an array handed to the library is treated exactly
  like an image read from a FITS file.
- `capi` is the C API: a thin layer that turns handles and structs into calls of the modules and
  every exception into a status code.
- `main.cpp` is the command line only: arguments, output names, `--info`, `--verify`, printing. It
  includes `xisfconv.h` and no internal header.
- CMake builds the library from everything but `main.cpp`, static by default and linked into the
  tool, shared with `-DBUILD_SHARED_LIBS=ON`.
- The modules do not print. Warnings and notes go to a message handler, progress reports to a
  progress handler; both are installed per thread for the duration of a call (`MessageScope`,
  `ProgressScope`). There is no other global mutable state.
- Errors are exceptions inside the library: `xisfconv::Error` with a kind (format, I/O, checksum,
  argument, index, exists, not found, cancelled), and `Unsupported` for a feature this build lacks
  while the file may be fine. `Unsupported` becomes NOT FULLY CHECKED in `--verify`. No exception
  crosses the C API.
- File names are UTF-8 inside the library and the tool; every file is opened through `toPath`, so
  that Windows gets wide-character names. The tool takes its arguments as UTF-16 there (`wmain`).
- FITS and ASDF input share one in-memory form (`FitsFile`), so every conversion from them is
  written once.
- The version is stated in one place, `include/xisfconv.h`; CMake reads it from there.
- Numbers are formatted and parsed independently of the locale of the program the library lives
  in (`cNumber`, `strtodC`): a host that has set a decimal comma must not change a file.
- A NUL byte in header text (which no valid file has) is read as a space, in all three readers:
  C callers' strings would end there.

## The library (libxisfconv)

Planned in a separate brief with a draft header (not part of the repository), decided as follows
and built in 0.10.0. What remains is in `TODO.md`.

- The C++ code stays the engine. A plain C API in `xisfconv.h` is the only public interface;
  every language binds to it, C++ included, so no C++ ABI is exposed.
- The command line tool is rebuilt on the C API alone. That is the proof that the API is complete:
  for the 2060 invocations of the test suite, and some 8000 more of an independent review, exit
  status, output and files are what 0.9.2 produced. The differences are deliberate and few:
  an IMAGE extension with PCOUNT or GCOUNT other than 0 and 1 is refused (it crashed before); a
  tile-compressed image with an unsupported Rice setting is skipped in `--info` too; NUL bytes in
  headers print as spaces; `-i` with the largest 64-bit number is an invalid index; on Windows,
  file names are Unicode.
- Function prefix `xisfconv_`, macros and enumerators `XISFCONV_`. The draft's `xc_` is taken by
  libxc (which has its own `xc_version`) and by Xen's libxenctrl.
- Presented as "XISF, and conversion between XISF, FITS and ASDF", not as a general FITS or ASDF
  library.
- Writing images from memory and the stretch on buffers are in the first release, for all five
  output formats: saving an array as XISF is what Python users cannot get elsewhere.
- Version 0.x with no ABI promise until two bindings have used the API. The shared library version
  changes with every 0.x release (`libxisfconv.so.0.12`), so that a binding built for another
  release fails to load.
- Bindings: Python first (NumPy arrays; it can register `xisf` with astropy's I/O registry), then
  Rust and Perl when someone asks for them.
- Delivered in three patches: the internal refactor (0.9.1), the C API with the tool rebuilt on it
  (0.10.0), the Python package (0.11.0).

Choices made while building the API:

- **Enumerations are `int32_t`** with named constants, flags are `int32_t`, structs start with
  `struct_size` and only grow at the end. That is what makes the header safe for ctypes, cffi,
  bindgen and FFI::Platypus, and lets a program built against an older header run with a newer
  library. For that to hold the `_init` functions take the size the caller compiled with: an
  init that filled the library's idea of the struct would write behind an older program's.
- **One context, reference-counted.** Error text, message handler and progress handler live in a
  context. Files, reports, keyword lists and writers keep it alive, so a garbage collector may free
  them in any order.
- **One image model for three formats.** What all formats share is in `xisfconv_image_info`; what
  only one has is reached by name (`xisfconv_image_detail(file, image, "compression")`), so the
  struct does not grow with every format detail.
- **FITS and ASDF sample formats are known only after reading.** Whether signed integers become
  unsigned or floating point depends on the data. The info says so (`data_known`), and
  `xisfconv_load_pixels` reads an image into the handle; the next read takes it from there.
- **Keywords describe the buffer as it is handed over.** WCS keywords and BAYERPAT count rows from
  the first row of the array, whichever end of the image that is; the library converts them when
  it stores the rows the other way round. That is the rule a FITS file follows, too. XISF is the
  exception on the reading side: its WCS keywords are bottom-up although its rows are top-down,
  because PixInsight writes and reads them so. The image info names the row order the WCS
  keywords describe (`wcs_row_order`), and the writer accepts the same field, so that pixels and
  keywords read from any file can be handed back unchanged.
- **The writer has no colour space argument.** Three channels are RGB, one is grayscale, any other
  number is a stack of planes: the rule of the conversion from FITS.
- **A damaged file is a finding of `xisfconv_verify`, not an error of it.** The call fails only when
  no report can be made.
- **Cancelling** goes through the progress handler, or through `xisfconv_context_cancel` from
  another thread, and leaves no partly written file.
- **Freeing a context silences its handlers.** Handles may outlive the context; what the handlers
  point to (a Python object, say) need not.
- **Messages keep naming the tool's options** (`--force`, `--bounds`). Making them neutral would
  have meant rewriting them in two places to keep the tool's output unchanged; the option names
  double as the names of the settings.
- **The CMake package comes with the shared library only.** A static library needs its dependencies
  (zlib, zstd) described to the consumer, which depends on how they were found; pkg-config's
  `Libs.private` covers that case.
- **Diagnostics** (`xisfconv_asdf_tree_text`, `xisfconv_asdf_tree_json`) are in the header for the
  tool and the tests, marked as not stable.

## The Python package

Built in 0.11.0, in `python/`. What was decided:

- **ctypes, not cffi or a compiled extension.** The header was designed for it (sized structs,
  32-bit enumerations, no macros in the interface). Nothing is compiled against Python, so one
  wheel per platform serves every Python version, and there is no dependency besides NumPy.
  The price is that the declarations are written twice; a test compares them with the header
  (every function, constant and structure field) and with the layout a C compiler produces.
- **The wheel holds the shared library** next to the modules, without a version in its file name
  (wheels cannot hold symbolic links). The package looks for the library in this order: the
  `XISFCONV_LIBRARY` environment variable, its own directory, the system. It refuses a library of
  another 0.x release, since the layouts may differ.
- **Build**: scikit-build-core, from `pyproject.toml` in the root of the repository (so that the
  source distribution can hold the C++ sources). CMake knows this build by `SKBUILD`: shared
  library only, no tool, no header. The version is read from `xisfconv.h` here too.
- **Wheels** are built by cibuildwheel for Linux x86_64 and arm64 (manylinux_2_28), macOS arm64
  and Windows x64. Zstandard is linked statically, so that a wheel needs only the C and C++
  runtime and zlib of the system: on Linux and macOS it is built from its release archive, which
  is checked against a SHA-256 written in `python/tools/build-zstd.sh`; on Windows it comes from
  vcpkg, as for the release binary. On Linux the shared library exports the functions of
  `xisfconv.h` and nothing else (a linker version script), so another copy of Zstandard or of the
  C++ library in the same process is not disturbed. musllinux, 32-bit Windows and Intel macOS are
  left for when someone needs them.
- **Arrays have row 0 at the top and the channels last** by default: that is what Pillow,
  matplotlib, tifffile and the `xisf` package give, and what a Python user expects of an image.
  `row_order` and `channels` give the FITS conventions (bottom-up, planes first) on request, and
  `xisfconv.astropy` uses those throughout, because astropy does. A colour image with the
  channels last is a view of the planar buffer the library fills: no copy is made.
- **`sample_format`, not `dtype`.** The conversion rescales (`--bits`), and an argument called
  `dtype` would promise a cast. In `read()` and `read_image()` it is given by name, because the
  second positional argument is the image.
- **Only the six sample types of the library are written.** Signed integers and the like raise an
  error that says so, instead of being converted silently. (`xisfconv.astropy` converts signed
  integers the way the FITS reader of the library does, because FITS data is signed by nature.)
- **Keywords are a list of cards that can be asked by name**, with typed values, made from a list,
  a dict or an astropy `Header`. A value read from a file remembers its text and is written back
  unchanged unless it is replaced. COMMENT and HISTORY have their text as value, as in astropy.
- **`read_image()` gives the keywords as the file has them** and names the row order their WCS part
  describes (`wcs_row_order`), so that `write(read_image(...))` changes nothing. BAYERPAT is the
  exception: it is turned over when the rows are handed over in the other order than stored.
- **XISF properties are read, not written.** Writing them needs the lossless property round trip
  of `TODO.md` first.
- **Errors are exceptions** derived from `xisfconv.Error` and, where one fits, from the built-in
  one (`OSError`, `FileNotFoundError`, `FileExistsError`, `ValueError`, `IndexError`,
  `LookupError`). Their text names the file. **Warnings are Python warnings**, raised when the
  call is back and blamed on the caller's line; **notes go to the logger** `xisfconv`. For that
  the library keeps its messages (`xisfconv_context_keep_messages`) until the call has returned;
  a message handler written in Python would be a callback from C, with the trouble described
  next.
- **Progress is a function passed to the call**, not a setting. An exception it raises stops the
  work and is passed on.
- **Ctrl-C and other signals: the library calls a generator between its steps.** This took four
  designs, and the reasons belong here so that the first three are not tried again.

  A Python signal handler does not run when the signal arrives. It runs when the main thread next
  executes Python code, at one of a few kinds of instruction: the start of a function, the jump
  back to the top of a loop, the return from a call. While the main thread is inside the library
  there is no such moment, so Ctrl-C waits for the end of the call. If the library calls a Python
  function in between (a progress callback), the handler runs at that function's first
  instruction, which is before its `try` block; what the handler raises (`KeyboardInterrupt`)
  goes back to the C code that called the function, and ctypes can only print it. The interrupt
  is shown and forgotten.

  1. *Replacing the program's SIGINT handler during a call* by one that only takes note. Every
     other signal that raises (an alarm that sets a time limit, a SIGTERM handler that exits) was
     still lost.
  2. *Wrapping the handlers of the signals that commonly raise*, and putting them back after the
     call. Python code that swaps handlers can itself be interrupted between any two instructions;
     an independent review kept finding moments at which a signal was dropped or a handler stayed
     replaced.
  3. *Doing the work in a thread of its own* while the caller waits in Python, where a handler may
     raise. Now the caller's wait and its clean-up were the code that a handler could tear at any
     instruction: on Python 3.11 and 3.12 an exception raised at the jump back of a loop was
     attributed to an instruction outside the `try` and skipped the `finally` altogether, so the
     caller got its exception while the work went on, and a second call collided with the first.
     The thread also had to be a bare one (a `threading.Thread` is registered in a weak set whose
     clean-up is Python code called from C), had to be stopped before Python ends, and made a
     forked child wait for ever for a thread it did not have.
  4. *What is there now.* The call is an ordinary call in the caller's thread. Between its steps
     the library calls the `send` method of a **generator**. A generator comes back to life in
     the middle of its code, inside its `try` block, so the handlers that are due run there and
     what they raise is caught, kept, and answered with "stop"; the library stops its work,
     removes what it had begun and returns, and the exception is raised from the call. The loop
     of the generator is inside the `try`, not around it, because the jump back is such a moment
     too; and once it has caught something the generator has done its work (the next call gets
     a new one), because storing the exception and going round again would be two more.
     Measured on Python 3.10 to 3.14: entered with a signal already due, an ordinary function
     lost the exception every time and a generator never; with real signals 20 to 170
     microseconds after the start of a run of reports, a generator with the `try` inside the
     loop lost about a third of them on 3.11 and later, and this shape none of some ten thousand
     per version.

  The same generator calls the caller's progress function, so that is ordinary Python code that
  may raise, and may use the package. For this the library has a second kind of progress handler
  (`xisfconv_context_set_host_progress`): it takes one argument, because `send` takes one, and its
  answers are two unlikely numbers, because a ctypes callback that fails leaves whatever was in
  memory as its answer (on Python 3.14 that happened to be 1). Any other answer stops the call,
  which then says that the report did not come back.

  Only the calls that have steps get the generator: conversions, rewrites, verification and the
  writing of a file. Opening a file, reading an image and reading keywords are one piece of work
  each for the library, so nothing of Python runs inside them.
- **What that does not cover.** In a thread other than the main one no handler runs, so a call
  without a progress function is not asked anything there. A call is stopped only between its
  steps: a rewrite and a verification have one per data block; a conversion has one per image
  while it reads an XISF file, reads a FITS or ASDF file in one, and writes its output in one;
  reading one image is one step. And an exception that a handler raises in the Python code of
  the package, before the library is entered or after it has returned, is like one raised
  anywhere else in a Python program: it reaches the caller, and everything is written so that
  whatever instruction it strikes at, a handle is closed once and no more (the pointer is given
  to what will free it in one step, and taken from it in one step). The last review sent some
  240,000 raising signals into 29 kinds of calls on Python 3.10 to 3.14: every exception
  reached the caller except some of the class `Exception` that astropy's own code caught
  (`CCDData.read`; never `KeyboardInterrupt` or `SystemExit`), nothing crashed or hung, and no
  handle stayed open in some 10,000 interrupted `open` calls on each of 3.10, 3.11 and 3.13.
  Known limits that remain:
  - *Two handlers that raise at the same moment*: the caller gets the later exception with the
    earlier one as its context. On Python 3.10 the second is printed by Python and the call
    raises `Cancelled` instead (3.10 looks for signals between almost any two instructions).
  - *Python 3.14* (3.14.0rc2 is what was at hand): an exception from a signal handler can leave
    a `with lock:` statement without releasing the lock, in any Python program; it did so in
    half of 87,000 interrupted runs of a bare loop, and never on 3.10 to 3.13. Here the effect is
    that an open `File` shared between threads can stay locked by the thread that was
    interrupted. That thread itself can go on using and closing it.
  - A temporary file of `xisfconv.astropy` that an interrupt keeps from being removed where it
    was used is removed when Python ends.
- **A file and what runs inside a call.** A signal handler can run between two calls of the
  library, also inside a method that makes several (reading an image does); if it closes the
  file there, the method finds the file closed and says so (every call looks once more just
  before it enters the library). Inside a call on an open file nothing runs, as said. Should
  that change, the library says whether a call is running in a context
  (`xisfconv_context_running`), and using or closing the file then raises `RuntimeError`. A
  progress function or handler that uses the package from inside a conversion gets a context of
  its own; calls nested more than 24 deep raise `RecursionError` (a handler that uses the
  package and is itself interrupted by its signal again and again would otherwise use up the C
  stack).
- **When Python ends** while a daemon thread is inside a call, the call is asked to stop
  (`xisfconv_context_cancel`) and waited for by an `atexit` function, and from then on calls
  report no progress: a thread that came back from the library into an interpreter being taken
  down, to report, would crash it. (A thread that is about to enter the library is given two
  seconds to do so; one that was torn out of a call and left its entry behind is not waited for
  longer than that.) A daemon thread that starts another call after that is cut off where it is
  when the process ends, as daemon threads are. A forked child forgets the calls of its
  parent's threads.
- **Handles are freed without running Python code**: the weak reference to their owner has the
  library's free function as its callback, so nothing can be raised and lost there. An object
  that the collector has taken knows by that reference that its handle is gone, and says
  "closed" instead of using it. The references are kept in a set without a lock: a lock there was
  taken a second time by a finalizer that closed a file, and the process hung.
- **A call made while another is running** in the same thread (from a progress function, or from
  a signal handler) gets a context of its own; the thread's usual one is busy.
- Tests send real signals into loops of calls, from timers and from other threads, use the
  package from inside handlers and progress functions, fork, and end Python in the middle of
  calls.
- **A file object does not keep its image objects**: they are made when asked for. Otherwise
  file and images would form a reference cycle, and an unclosed file would stay open until the
  cycle collector runs.
- **The messages of the library name the tool's options**; the package rewords them to its
  arguments (`--force` to `overwrite=True`) from a short list. A test reads the sources of the
  library and fails when a message names an option the list does not know. File names in a
  message are left as they are, also one that looks like an option.
- **Threads.** The functions that take file names use one context per thread; an open file has
  its own, with a lock, so it may be shared between threads but is used by one at a time.
- **astropy is optional** and registered by `import xisfconv.astropy` (astropy has no entry point
  for this). `CCDData.read` hands the image to astropy's own FITS reader as a FITS file in
  memory: every detail of units, mask, uncertainty and WCS is then astropy's, at the price of
  about four times the image in memory. Mask, uncertainty and PSF are stored as further images
  named as astropy names its HDUs, and found by those names whatever the case, as astropy finds
  HDUs. The registered writer writes XISF whatever the file is called.
- **Additions to the C API** that came out of this: `xisfconv_fits_keywords` (the header an image
  gets in a conversion to FITS, which before only a conversion could produce);
  `xisfconv_keywords_fits_text` (a keyword list as FITS cards, exactly as the FITS writer formats
  them, so that the package does not format cards a second way); the writer leaving out the
  cards that describe how a FITS file stores its data, so that a header from astropy can be
  passed as it is; messages kept in the context (`xisfconv_context_keep_messages`,
  `_message_count`, `_message`, `_clear_messages`); the host progress handler described above
  (`xisfconv_context_set_host_progress`, `xisfconv_context_host_progress_failed`);
  `xisfconv_context_cancel`, the one function that another thread may call while a call runs; and
  `xisfconv_context_running`, which says whether one does.
- **What the review of the package changed in the library and the tool** (0.11.0): a keyword
  value that is text without quotes (`Ha`) is written to FITS in quotes, where it used to make
  an invalid card; a HIERARCH card whose string does not fit is shortened to a valid card or
  left out, and a keyword with `=` in its name is left out, each with a warning; an output name
  that is a directory or a device is refused instead of replaced (as root, `-f -o /dev/null`
  replaced the device); a directory given as input is called that; a FITS or ASDF file that
  changed between opening and reading is an error; a card without a keyword name can be put in
  a keyword list; a text too long for one card that ends in `&` keeps it (astropy took it for
  the mark that the text goes on, so an empty last piece is written); an output whose `.part`
  name is taken by a link or a directory is refused.
- **Publishing to PyPI is switched off** until the project is registered there: a version can be
  uploaded once only, so that step is the maintainer's. It uses trusted publishing, without a
  stored token.

## Testing

- `tests/run_tests.py` drives the built program (4867 checks at 0.12.0). The Python packages it
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
- The library has tests of its own: a C99 program for the mechanics of the API (arguments, buffers,
  lifetimes, callbacks), a Python script that calls the API through ctypes and compares what the
  library writes and reads with astropy, the `xisf` package, asdf, tifffile and Pillow, and a
  program that reads all there is of any file, which is what gets fuzzed.
- The Python package is tested with pytest (`python/tests`, 288 tests at 0.12.0): the same
  comparisons with other software, made through the package, run from the source tree and from the
  installed wheel on Python 3.10 to 3.14, with the oldest NumPy and astropy the package allows and
  with the newest, and under AddressSanitizer.
- An arm64 build of the tool is run under qemu against the suite before a release that touches
  arithmetic (see "Arithmetic does not depend on the processor").
- Windows code is compiled with MinGW and run under Wine before delivery, since no Windows machine
  is at hand; CI on Windows remains the real check. Since 0.11.1 that includes the Python package:
  its tests run with a Windows build of Python under Wine against the MinGW build of the library.
  (The first CI runs of 0.11.0 failed on what this would have shown: a directory given as input
  was not called one on Windows, where a directory cannot be opened at all, and a test replaced a
  file that astropy still had mapped into memory.) The wheels for macOS and Windows have only CI
  to prove them.
- CI builds and runs the suite on Linux, macOS and Windows.

## How changes are made

- One commit per feature, with the version bumped in `include/xisfconv.h` and the
  README and `TODO.md` updated in the same commit.
- The work is done together with Claude (Anthropic), credited as co-author in the commit messages.
  Each change is delivered as a `git format-patch` file to apply with `git am`, plus a source
  archive, after it has been built and tested on a clean clone.
- Large features (so far ASDF and XISF rewriting) get an independent review pass before delivery;
  its findings are fixed first.
- Limitations are written into the README when a feature ships, not left to be discovered.
- Messages to the user say what happened and what to do about it; nothing is skipped silently.

## Not decided yet

- A CMake package for the static library.
- Order of the remaining items in `TODO.md` after the library.
