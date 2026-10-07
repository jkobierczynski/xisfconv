# Development decisions

What was decided while building xisfconv, and why. The README says what the program does and
`TODO.md` what is planned; this file records the choices behind both, so that they are not
reopened by accident. State: version 0.16.0, 7 October 2026.

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
  else is in the source: XML and YAML readers, LZ4 decoder and (since 0.15) compressor, MD5,
  SHA-1, SHA-2 and SHA-3, Rice and PLIO decoders, the FITS, ASDF, TIFF and PNG readers and writers.
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
- **A smaller picture is the mean of what it covers** (`--bin`, `--resize`, 0.14.0). One method
  for every ratio: each pixel of the picture is the mean of the part of the image it covers,
  the pixels at its edges counted by the share that is covered. For whole ratios that is binning,
  and the shares are computed so that they are exactly 1 there. Sharper filters (Lanczos,
  bicubic) were not taken: they ring around stars and make values the data never had, and for
  a picture that is a fraction of the image the mean is what looks right anyway. Nothing is
  made larger. The picture is made of the image as stored and stretched afterwards, because
  the mean of linear data is a measurement (what larger pixels would have recorded) and the
  mean of stretched data is not; it also means that the statistics of the auto-STF are taken
  of a small picture. The options are for TIFF and PNG: binned data in FITS or XISF would need
  its WCS, its solution properties, XPIXSZ, XBINNING and the colour filter pattern changed
  with it, which is a feature of its own (`TODO.md`).
- **The thumbnailer is the tool with its ordinary options**, spelled out in the entry
  (`--quiet --force --to png --stretch --bits u8 --resize %s --output %o %i`). There is no
  thumbnail mode: what a file manager shows can be reproduced on a command line, and there is
  nothing to test that the options are not tested for already. The entry names several
  spellings of the XISF type, since the format has no registered one; the package defines
  `image/x-xisf` and `application/x-asdf` by name and by the first bytes of a file, and adds
  `*.fits.fz` to the FITS type of the desktop. GNOME runs thumbnailers in a sandbox that sees
  `/usr` and nothing of the home directory: the entry may be installed for one user, the
  program may not.
  The shares are counted in whole numbers (of 1 / width of the picture) and divided once, and
  the sums are doubles: the same on every processor, exact for the sample types pictures are
  made of, and right to the last bit or two for 64-bit samples.
- **Verification outcomes** are OK, NOT FULLY CHECKED (a part this build cannot check, named) and
  FAILED. Only FAILED sets exit status 1.

## XISF properties in FITS and ASDF

Since 0.13.0 a conversion from XISF takes the properties along and a conversion to XISF restores
them. The choices:

- **A table, not HIERARCH keywords** (which the plan in `TODO.md` named). A card holds 80
  characters. The plate-solved test frame has 77 properties: identifiers of up to 113 characters,
  the processing history as one XML text, and the splines of the astrometric solution as matrices
  of 9 MB together. Keywords could carry a handful of scalars and would lose the rest, and their
  types with it.
- **FITS: a binary table behind each image** (`XISF_PROPERTIES`, `EXTVER` the number of the image)
  and one for the file (`XISF_METADATA`). PixInsight has no convention of its own for this that
  could be followed, so the layout is ours: a row per property, id and type as text columns,
  value, comment and format as variable-length byte arrays. Comment and format are bytes and not
  text columns because FITS text columns are ASCII and a comment is UTF-8. Vectors and matrices
  are stored as the bytes XISF stores, little-endian, with their shape in two columns: the way
  back is then a copy, and a reader needs one line (`np.frombuffer(value, "<f8")`). A column says
  whether the value is a data block in XISF, so that a block of a type nobody knows is told from
  text, and a String that PixInsight keeps in a block from one in the header.
- **ASDF: in the tree**, a mapping from id to `{type, value, comment, format}` under `xisf`. The id
  is the key because that is how a Python user wants to get at a property; the price is that two
  properties of one id (which XISF forbids) cannot both be kept, and that the asdf library sorts
  the keys when it writes a file again. Values are YAML values of their kind and arrays are
  `ndarray` blocks of their element type and shape, so that Python gets a float, a bool, a matrix.
  That makes the tree lossless in value and not in spelling: `1e-05` comes back as `1.0e-05`.
  A number is written with its own digits wherever YAML reads them as that number, and a text
  that is no value of its type goes along as a string. The byte order of an array is always
  stated: the schema of `ndarray` asks for it with `source`, also for single bytes.
- **The astrometric solution and the WCS keywords are two descriptions of one thing**, and a FITS
  file can be changed by a program that knows only the keywords. So a digest of the WCS keywords,
  the size of the image and the order of its rows is stored with the properties (SHA-1 over the
  sorted keywords; numbers as numbers, so that a program that writes `1.0E-5` as `1e-05` changes
  nothing; every keyword the WCS papers, SIP and the distortion conventions define counts, in
  its old spellings too). On the way back the carried solution is used if the digest still
  matches, and left out otherwise, with the solution then made from the keywords as for any
  FITS file. When it matches, nothing is made from the keywords at all: an XISF file that had
  WCS keywords and no solution comes back without one.
- **The properties that describe the XISF file itself** (creation time, creating application,
  block alignment, compression) are not taken along. They would be untrue of the next XISF file,
  which sets its own, and carrying them would give every FITS file a table of its own for them.
- **On by default.** The cost is one more HDU in a FITS file, which programs that read the
  primary image do not look at; the gain is that a conversion can be undone. `--no-properties`
  is there for the program that minds.
- **A String stays where it was.** XISF has two places for a text: the header, and a data block
  (where PixInsight puts its long spline serializations, compressed). They are not the same to a
  reader: XML turns CR LF in the header into LF, and may take blanks at the ends for layout; a
  block is what it is. The first version wrote every String into the header, and the review found
  what that did to PixInsight's own files: 703 carriage returns gone from a serialization, for
  every reader that follows XML. So the place is carried with the value, and the bytes of a block
  are never touched. A text from the header is written as text again, with its line breaks as
  they are (the same bytes are the one form every reader takes as it took the original); only
  what XML cannot hold (control characters, bytes that are not UTF-8) goes into a block, and,
  since 0.15, a text with a carriage return that the header wrote as a character reference.
  (Up to 0.14 a text with blanks at its ends or a carriage return on its own went into a block
  as well. That broke the rule of this paragraph for the readers it was meant to protect: see
  "Where a text is kept is part of what was read".)
- **Arrays** up to 3072 bytes are written into the header and larger ones attached, which is
  PixInsight's own limit. The solution properties this library makes from WCS keywords stay in
  the header whatever their size, as in every version before: that form is the one PixInsight
  was seen to accept, and files without carried properties are byte for byte what 0.12 wrote.
- **A file cannot ask for more memory than it could fill.** Properties are loaded when a file is
  converted or opened, and a property is a few bytes of header that may point at any block: 200
  of them at one compressed block of 64 MiB made a file of 89 KB ask for 12 GiB in the review.
  The properties of a file may hold its size plus 256 MiB together; what is beyond is left out
  with a warning. Compressed blocks of an ASDF file are decompressed once for all the properties
  that share them.
- **What a format cannot hold is said, not written.** An id or type that is not ASCII does not go
  into a FITS text column, one that is not UTF-8 not into a YAML stream, a control character not
  into an XML attribute. Such a property is left out, or the character replaced, with a warning;
  no file is written that its own format would refuse. A long id (over 500 characters) is
  written to the tree as an explicit key (`? id`), because the parser of Python reads a plain
  key only up to 1024 characters and would refuse the whole file.
- **One form in memory** (`Property` in `property.hpp`): id, type, comment, format, and the value
  as text or as little-endian bytes with a shape. The three writers and three readers meet there,
  and a type that is not known is carried without being understood.
- Not done: properties given by the caller of the library (`xisfconv_image`, Python's `write`),
  and the other things of an XISF image that FITS has no place for (see `TODO.md`).
- A file written by 0.13 and read by an older xisfconv: the tables of a FITS file are named as
  skipped HDUs, and the matrices in an ASDF tree are taken for images, since any array of two
  dimensions is one there. `--no-properties` writes files without them.

## Distributed XISF units (0.16.0)

- **Scope.** OpenXISF does more than xisfconv in several places; of those, this one was chosen:
  the header file (`.xish`), the data blocks file (`.xisb`) and `path(...)` locations, read and
  written, in the tool, the C API and the Python package. Features are added one at a time and
  for a reason; "everything OpenXISF has" is not a goal.
- **PixInsight reads and writes monolithic files only.** The baseline of the specification is the
  monolithic file, and PCL's reader and writer are that. So this feature cannot be checked with
  PixInsight and is not for it: it is for other software, and for packing what that software
  writes into a file PixInsight opens. The independent implementation to compare with is
  OpenXISF (C++20, Apache-2.0): its sample programs read what xisfconv writes and write what
  xisfconv reads, in the tool's tests when `OPENXISF_BIN` is set. None of its code is used.
- **Only the header file is named.** A unit is its header and what the header names; there is no
  second argument for the data, anywhere. A `.xisb` file given as input is an error that names
  the header file.
- **The kind of an output follows its name**: `.xish` (in any case of the letters) is a header
  file with `<stem>.xisb` beside it, any other name a monolithic file. The specification ties
  the suffixes to the kinds of file, so a name is enough, and no function needed a new
  argument or a new field in a struct: `write`, `convert` and `rewrite` already take a name.
  `-t xish` is the tool's way to ask for the names. A header file under another name is still
  read as what it is, since reading goes by content; whether it is followed to its data is
  another matter (see "Only a header file that is named as one is followed").
- **One data blocks file per unit, written anew every time.** Every block that is not in the
  header goes into it; the index is one node directly behind the signature; nothing is ever
  added to or changed in an existing `.xisb`. The specification's index (linked nodes, free
  elements) is made for files that are edited in place; xisfconv never edits a file in place
  (see "Care with files"), so it writes the simplest index and reads every index.
- **Random identifiers.** A block is found by a 64-bit number. With numbers counted from 1, the
  header of one unit would find a block in the data blocks file of any other, and a mixed-up or
  half-replaced pair of files would be read as an image of noise or, worse, of another frame.
  With random numbers a header that is not this file's finds nothing, and the error asks "is
  it the file that was written with this header?". (The specification recommends random
  identifiers.) Written as 16 hexadecimal digits, as the specification suggests; read in
  decimal too.
- **Two files, one commit.** Both are written as `<name>.part`; the data blocks file is renamed
  first and the header last. Two renames are not one, so a data blocks file that is there
  already (a unit that is written over, or replaced in place) is first set aside as
  `<name>.xisb.replaced`; if one of the new files cannot get its name, the new data blocks file
  is taken away again and the old one put back, and the unit that was there is as it was. Only
  when both names are given is the old file removed. Should the machine stop in between, the old
  data is there under that name, and the old header finds none of its blocks in the new file
  (random identifiers), so nothing is read as an image that is not one. The first version
  removed the new data blocks file when the header could not follow, which with `--force` was
  the only copy of the old unit's data by then: found in review, with an immutable header file.
  The second review found the same one step further on: a rename that fails for another reason
  than a file that may not be touched (an I/O error) went into the old fallback of every output,
  "remove the file that is there and try again", and that removed the old header. No output is
  removed before its replacement has the name now, monolithic ones included: where renaming
  over a file does not work, the old file is set aside and put back. And where putting back
  fails too, the message says under which name the old data is, instead of "as it was".
  These paths are tested with a `rename()` that fails on request (`tests/rename_shim.c`, loaded
  with LD_PRELOAD on Linux): each step for a conversion, a rewrite and a rewrite in place.
  A third review went through the fixes of the second: the header's own put-back could fail
  under a message that said "as it was" (the error names every file that is under another
  name now); on Windows a file that may not be written to is renamed and then not deleted, so
  `--force` left `<name>.replaced` behind without a word (it is made deletable, and a warning
  names it if it stays, unless `-q` has turned warnings off); and the advice in the README, "rename it back", was wrong for a run
  that was stopped *after* both renames (the advice is now: only if the unit does not verify,
  and the message of the failure names the file that really has the blocks, by looking into its
  index).
- **In place**, a unit becomes its header and the data blocks file of the header's name. Blocks
  that were in other files (several `.xisb`, files that are one block) come into that one; the
  other files are left alone, because another header may read them. An output is refused if it
  is a file the input reads its data from, except in place on that very unit; and so is a
  temporary file that is one (a hard link named `out.xisb.part`), for rewrites and for
  conversions alike.
- **A data blocks file can be shared, and is not written that way.** The specification lets
  several headers name blocks in one file. xisfconv reads that, and writes every unit a file of
  its own. Replacing a shared file in place would take the other headers' blocks with it, so a
  file whose index holds blocks this header does not name is not replaced without
  `--force`/`overwrite`. A second header that names the *same* blocks (a copy of the header)
  cannot be seen from here, and loses them: documented, not solved. A file that holds none of
  the blocks its header names is not replaced at all, with or without `--force`: that is what a
  replacement leaves that was stopped half-way, and "use --force" would be advice to destroy
  the half that is good. (For a round, that case was let through to the reader, which "would
  say so": it does not when the header names the file as a whole, or names it in an image that
  `--image` leaves out. A check that is skipped because something else will catch the case
  wants that something tested. The fourth review found the same door open one frame further:
  a file the header names and is *not followed to*, or whose index cannot be read, was skipped
  by the check and replaced unread with `--image`. The rule is now stated the other way round:
  a data blocks file that is there is replaced in place only if it was read and holds this
  header's blocks and no others; `--force` waives "no others" and "its index can be read",
  and nothing waives "was read".) A fifth reader then ran the rule over 15 000 generated cases
  (33 ways for a header to name the file, 25 things the file can be, the three settings, with
  and without `--force` and `--image`) and found no foreign block lost; what it found were the
  edges of "names": a path through a link and back (`a/L/../u.xisb`) spells another file than
  it leads to, so whether a path is one of the input's files is asked of the system with the
  path as the header wrote it, and the blocks a header names in a file are collected from all
  the ways it writes that file, whichever comes first.
- **A header is not trusted with the file system.** It is data from somewhere, and it names the
  files to read. Followed blindly, `location="path(/etc/passwd)"` on an image would have a
  conversion copy that file into its output, and a service that converts uploads would hand it
  out. The default follows a header to its own directory and below, by `@header_dir/` paths
  only: a `..` that leaves the directory is refused from the words of the path, before the file
  system is asked anything, and where the path leads is then checked on the resolved path, so a
  symbolic link out of the directory is refused too. `anywhere` and `none` are the two other
  settings. It is a setting of the context in C (a thread-local scope inside, like the message
  handlers), an option of the tool, and in Python an argument of each call that is not
  remembered: a permission that stays set is one that is forgotten. A refused block is
  `NotAllowed` (its own status and exception, a `PermissionError` in Python): for a caller it
  is neither a damaged file nor an unsupported one. The check is not a sandbox: it does not
  defend against someone who changes the directory while the file is read (the path is
  resolved and checked, then opened by its name; a reviewer who swapped a file for a link in
  between got through one time in six).
- **Only a header file that is named as one is followed.** The first version followed any file
  with external locations, a monolithic `.xisf` included, to its own directory, with a warning.
  A reviewer pointed at what that means: `.xisf` is what people are sent and what a thumbnailer
  opens unasked, and such a file, lying in a download folder, could have named
  `.ssh/id_ed25519` below it as its pixels. The specification already draws the line: a
  monolithic file holds all of its data, and a header file has the suffix `.xish`. So the
  default follows only a file that is an XML header *and* is named `.xish`; anything else
  (monolithic, or a header under another name, such as the temporary file astropy downloads a
  URL into) is followed only with `anywhere`. Whoever opens a `.xish` knows it is a unit of
  several files.
- **Windows is asked where a path leads.** The rule about the directory rests on the resolved
  path. MSVC's `std::filesystem::canonical` follows symbolic links and junctions; MinGW's
  takes them for what they lead to, and a link out of the directory passed for a file in it.
  So on Windows the system is asked (`GetFinalPathNameByHandleW`), whatever compiler built
  the library; only on a volume that has no drive letter to give is the C++ library's answer
  taken. (Following a link *in place*, for the file that is replaced, is still the C++
  library's: a MinGW build replaces the link there, not what it leads to. The released
  Windows binaries are built with MSVC.) Not testable here: Wine shows the links of the system it runs on as plain
  files. The tests of links run on CI's Windows if the runner may make symbolic links, and say
  that they were skipped if it may not.
- **A refusal does not answer questions.** For a symbolic link below the header's directory the
  message does not say where it leads, and reads the same whether or not there is a file there:
  a header that could ask "is there a file at X" of any path would be a small oracle for
  whoever sees the messages of a service.
- **The budget of the properties counts bytes the unit brings.** Properties may declare, in
  total, the size of the file plus 256 MiB (so that a small header cannot ask for gigabytes).
  For a unit, "the file" is the header plus the blocks its header names in data blocks files,
  each once, and never more of a file than the file has. A file that is one block counts for
  nothing: the first version counted the whole size of every file a header named, and a header
  could raise its own limit to 8 GiB by naming a large (or sparse) file that happened to lie
  beside it. The price: a property of more than 256 MiB stored as a file of its own is left
  out (in a data blocks file it is read). Nobody writes such units; it is in the README. For the same reason the size of an image's pixels in another file is held against
  the geometry before the file is read.
- **An XML file is not read to find out that it is not XISF.** A header file begins with `<`,
  and so does every SVG and HTML file; `--verify` of a directory opens what it finds. The root
  element has to be `<xisf>` within the first 64 KiB, or the file is refused unread. (The first
  version read 300 MB of SVG into 4 GiB of memory to say "root element is <svg>".)
- **Nothing is fetched from a network**, with any setting. `url(http://...)` is `Unsupported`.
  A converter that makes requests because a file says so is a tool for reaching into networks
  the file's author cannot reach. `file:` URLs are local paths and fall under the setting.
- **Only regular files** are read as data: a path to a device or a pipe would hang the reader or
  feed it without end.
- **A stream has no directory.** `xisfconv.astropy` copies a stream or a packed file into the
  directory for temporary files before the library reads it. `@header_dir` would then be that
  directory, where other people's files are. A header read that way is followed to no file,
  and the error says to read the unit by its name. What astropy itself downloads for a URL is
  an open file with a name in that directory, which this module cannot tell from a file of the
  caller's; the rule about names covers it, since that name is no header file's.
- **Names in the header.** The name of the data blocks file goes into an XML attribute inside
  `path(...)`: parentheses get a backslash (the specification), then `& < > "` become entities
  and a tab or line break a character reference (XML). The first version did the first and not
  the second, and `a&b.xish` was a header no XML parser took. A name that is not valid UTF-8
  cannot be written into a header at all, and is refused. (OpenXISF 0.5.0 writes parentheses
  without the backslash, which is read, and does not take the backslash off when it reads.)
- **Lenient reading, strict writing**, as everywhere: several index nodes, free elements,
  decimal identifiers, reserved fields that are not zero (a warning), and a monolithic file
  that names blocks in other files (a warning) are read. What cannot be right is an error: an
  index in a circle, a node or a block beyond the end of the file, an `attachment` in a header
  file. A monolithic file that names blocks in other files is read only with `anywhere` (see
  above), and then with a warning. Index nodes may not lie in each other: with that, an index
  names no more blocks than its file has room for, where 65 000 nodes pointing into one another
  made 137 MB of index from a file of one megabyte. Limits on the index (65 536 nodes,
  4 194 304 elements) are kept on top, and an index that could not be read is not read again
  for every block that names it.
- **`--verify` and the index.** What is wrong with an index and does not stop reading (a block
  of nobody's that lies beyond the file, two blocks under one identifier) is a warning when a
  file is converted and a failure when it is verified: verification is about the files, a
  conversion about the image.
- **A rewrite reads its own output back before the files have their names**, so the header
  names `frame.xisb` while the data is in `frame.xisb.part`. The reader takes a redirection for
  exactly that one name; it is not subject to the setting above, which is about what a header
  sends the reader to, not about what the library wrote a moment ago.
- **Alignment.** Written from pixels, every block starts at a multiple of 4096 bytes, as in
  monolithic files. A rewrite aligns uncompressed blocks and lets compressed ones follow each
  other, as it does in monolithic files and as PixInsight does. A header file states no
  `XISF:BlockAlignmentSize`: that property describes attached blocks.
- **The thumbnailer** takes `.xish` files where it may read the file beside the one it is
  given. GNOME runs thumbnailers in a sandbox that holds the one file; there a header file gets
  no preview. Not solved; listed as a limitation.

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
  `fitsread`, `fitstile`, `asdf`, `yaml`, `xml`, `tiff`, `png`, `wcs`, `property` (XISF
  properties as they go from one format to another), `convert` (sample formats, stretch),
  `codecs` (compression, digests), `common`.
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
  changes with every 0.x release (`libxisfconv.so.0.14`), so that a binding built for another
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
  And a struct ends without padding: `xisfconv_convert_options` of 0.13 ended in four bytes of
  it, the first new field of 0.14 landed there, and an older program hands those bytes over
  with whatever they hold. They are a field named `reserved` now, which nothing reads; new
  fields begin behind it, and a `static_assert` keeps the next one from doing the same.
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
- **On Windows the wheel is built in a directory that stays** (`build-wheel/`, an override in
  `pyproject.toml`, 0.12.1). scikit-build-core builds in a temporary directory and deletes it
  when the wheel is made. On the Windows runners that deletion failed, after a build without
  an error: "the process cannot access the file because it is being used by another process",
  for the build directory itself, which some process the compiler tools had started still had
  open. Which one was not established (it cannot be reproduced without Visual Studio); a
  directory that nobody deletes does not depend on the answer.
- **A test must not depend on where a signal lands.** One test showed why the progress reporter
  is a generator by sending signals at a plain function until one slipped through, and failed
  when none did within 300 tries; that passed everywhere here and for one release in CI, then
  failed on a macOS runner. It is a test of its own now, skipped where no signal arrives at
  that moment within three seconds; what the package promises (nothing escapes the reporter)
  never depended on it.
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
- **XISF properties are written from Python values** (0.15.0; read only before). `properties=` of
  an image and `file_properties=` of `write` take `{id: value}`, and the XISF type follows from
  the value: bool is Boolean, int is Int32 (Int64, UInt64 if it does not fit), float is Float64,
  complex is Complex64, str is String, a `datetime` is a TimePoint, and an array is the vector or
  matrix of its element type. NumPy scalars keep their width. That guess is right for what a
  program makes up itself and wrong for what it read from a file (a UInt16 would come back as
  Int32, a TimePoint as a String), so what `read_image` returns remembers what the file states:
  `PropertyDict`, a dict with the type, comment and format of each key on the side. It is a
  subclass of dict because programs written for 0.11 to 0.14 compare `image.properties` with a
  dict and index it; a class of wrapped values would have broken `properties[id] == 120`.
  Assigning a value keeps the stated type; deleting the key forgets it.
- **Vectors and matrices are read in the type of their elements** (0.15.0; float64 before, complex
  ones not at all). The package is not on PyPI yet, which is the moment to change what a call
  returns.
- **A solution the caller brings is the caller's word.** Properties given to the writer that hold
  `PCL:AstrometricSolution:...` are written as they are, and no solution is made from WCS
  keywords. Without one among them, a solution is made from the keywords as for an image without
  properties: an unrelated property must not switch that off. Written to FITS or ASDF, a brought
  solution gets the digest of the keywords it is stored with, so that a later conversion to XISF
  restores it; properties without a solution get none, so that such a conversion makes it from
  the keywords. A solution that was read with an image is the file's word, not the caller's: it
  is written while the keywords and the size are those it was read with (see "Properties from
  the caller's values").
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

### The interface of the `xisf` package (`xisfconv.xisf`, 0.15.0)

The `xisf` package of Sergio Díaz is what Python programs read and write XISF with, and the
oracle of this project's tests. `xisfconv.xisf` has its class, so that such a program can use the
library with one line changed.

- **Its interface, none of its code.** That package is under the GPL (version 3), this library
  under the LGPL: its code cannot be part of it. Names, arguments and the shapes of what is
  returned are an interface. The module was written from what the package returns for files, and
  is tested by comparing the two on the same files, structure by structure: key order, tuples
  against lists, dtypes.
- **The same structures, the library's behaviour.** Where the package is wrong or stops, the
  module does what the library does, and its documentation lists each case: checksums are
  verified; subblocks, big-endian samples, Normal storage, UInt64, embedded data, ByteArray and
  complex vectors are read; a Float64 written as `3` is a float; a Boolean written as `1` is
  true; a property that cannot be read is left out with a warning and does not cost the file; an
  array with the channels first is written with its real geometry; keyword strings are written
  with their quotes; the dictionary given as `xisf_metadata` is not changed.
- **What the package's structures cannot say is carried on the side.** It strips the quotes off a
  keyword value, so `'7'` and `7` are the same to it, and an XML reader turns PixInsight's CR LF
  into LF. A value read here is a `str` that also has `raw`, the text of the file, and `write`
  uses it: a file that is read and written again keeps its keywords and its texts byte for
  byte. A value without `raw` (one the program made) is written as a number or `T`/`F` if it
  reads as one, else as a FITS string. A number among the properties cannot remember its
  spelling the way a string can (it is a Python number), so the dictionary of the property
  does: it keeps the text of the value and what that text was read as, and while the value in
  the dictionary is still that one, the text is written. The first version wrote every number
  anew, and a Float64 that PixInsight wrote as `2000` became `2000.0`: the same value, but also
  a `Float128` nobody here can parse and a Boolean written as `1` had to be written from a
  value, and could not be. A file that says something odd is no reason to lose the rest of it.
- **The header is parsed twice**, by the library and by Python's ElementTree: the dictionaries
  hold every attribute of an element as it is written, also those the library has no name for,
  and `get_metadata_xml()` returns the tree. The two have to agree on which elements are images
  and properties: the Image elements of the root in their order, the Property elements of each,
  and for the file those of every Metadata element and of the root. The module counts both and
  refuses a header on which they differ. (One such header was found by review: `<!-->` was a
  whole comment to the library, which looked for the end from the start of the opening, and the
  beginning of one to ElementTree. The library now reads it as XML does.) They do not agree on a
  document type declaration: the library skips it, ElementTree reads it, and with it default
  attributes and entities that are not in the file (an `ATTLIST` that gives every image a
  `compression`, an entity of a gigabyte). A header with a DOCTYPE is refused by the module (one
  that stands where a declaration stands, before the first element: the word in a comment or in a
  text is none). The header is decoded as UTF-8 and its XML declaration taken off before
  ElementTree sees it: the `xisf` package writes `encoding='utf8'`, a name expat does not know.
- **Errors keep both families.** The package raises `ValueError` and `NotImplementedError`; the
  library has its own classes. The module raises classes that are both (`NotXisfError` is a
  `FormatError` and a `ValueError`), so that `except ValueError` in a program written for the
  package still catches.
- **Byte shuffling is off unless asked for** there (`shuffle=False`), on in `xisfconv.write`.
  Each keeps its default.
- **The wording is this project's.** The names of the public methods and their arguments are the
  interface; everything else (private names, messages, comments, the order things are done in)
  was written here, and where a first draft had come to resemble the package, it was rewritten.

### Properties from the caller's values (0.15.0)

- **Two ways to give a property.** `xisfconv_properties_set` checks the value against the type
  and is strict about it (`true` or `false`, no blanks, a number the type holds, a date that
  exists): what a program makes should be right. `xisfconv_properties_set_as_read` checks
  nothing but that XML can hold it: what a file said is written again. The Python package picks
  between them by whether the value is still the one that was read (0.0 and -0.0 are two
  values there, and not-a-number is the one it was).
- **Where a text is kept is part of what was read.** `xisfconv_property_stored` says whether a
  String is a value of the header or a data block, and `xisfconv_properties_set_as_read` takes
  that back. The second review found why it must: the writer put every text of more than 3072
  bytes into a block, also the 31 KB spline serializations PixInsight keeps in the header
  with CR LF. In the header an XML reader makes a line feed of CR LF; from a block it gets
  both. The bytes were the same and the text other programs read was not. The third review
  found the same in what the library had done since 0.13 on purpose: a text of the header with
  a blank or a line break at an end was moved into a block, "where every reader takes it as it
  is", which is true of the block and not of what the reader had before. A text of the header
  is now written into the header again with the bytes it has, whatever its length and whatever
  is at its ends: nothing is decided about what a reader makes of them, so nothing changes for
  any reader. Only a text that is given (by a program, or by a `value` attribute) is looked at,
  and kept as data if an element might not give it back.
- **A carriage return that is meant is kept as data.** `&#13;` in a header is a carriage return
  for every reader; a literal CR LF is one for PixInsight's own reader and a line feed for an
  XML parser. In memory both are the same bytes, so the reader marks a String whose header
  text has the reference (`xml::Node::crReference`) and it is carried and written as a block
  (`Property::block`), as a String that was a block is. A new text with a carriage return,
  given through `xisfconv_properties_set`, goes the same way. Literal CR LF stays literal: that
  is what PixInsight writes on Windows, and those files must come back byte for byte. Two edges
  stay: a text with both a literal CR LF and a reference is kept as data whole, so an XML reader
  that read a line feed for the literal one reads CR LF afterwards; and a line break written as
  such inside a `value` attribute, which XML reads as a blank, is read here as the line break.
- **What is not read has no value, and says so.** A table, a property made of elements, a block
  of a type without a name: `xisfconv_property_stored` calls them unread, the Python package
  gives None, and `write` leaves them out with a warning. The first version wrote a Table back
  as an empty value. A conversion does better in one case: it carries a data block of a type it
  has no name for as the bytes it is, which the property functions of the API cannot hand over
  (see TODO). A text block is handed over as its bytes also where they are not UTF-8; a NUL
  character ends it, as it ends any C string.
- **A solution belongs to the keywords it was read with.** An astrometric solution among the
  properties a caller gives is written as it is, and none is made. But a program that reads an
  image, crops it or solves it again and writes it would then write the old solution next to
  new WCS keywords, and PixInsight prefers the properties. So `read_image` notes a digest of
  the WCS keywords, the size and the row order with the solution (`PropertyDict.solution_of`,
  `xisfconv_wcs_digest`), and `write` leaves the solution out when the digest of what is
  written differs; one is then made from the WCS keywords if the image has them. It is the rule
  a conversion through FITS and ASDF already had; a FITS or ASDF file written with a solution
  stores the digest, and one written without stores none. The digest is kept per property, with
  the value that was read, so that a solution the program puts in the place of the one that was
  read is the program's word and is written. But a solution is one thing: if some of its
  properties were set and the others are still those of another image, all of it goes, with a
  warning. (For one release candidate the set ones stayed, and a reference coordinate was
  written alone, which also kept a solution from being made from the keywords.) The same rule
  holds wherever properties come from a file: `read_image`, the `properties` of an
  open file given to `write` or to a `PropertyDict`, and the dictionaries of `xisfconv.xisf`
  (where the `xisf` package writes the old solution: a difference on purpose).
- **No solution is better than a wrong one, but not silently.** PixInsight often keeps the
  solution in the properties alone, without WCS keywords. A crop of such an image is written
  without any solution: there is nothing to make one from, and the old one is wrong by the
  crop. That is a warning, not a log line; `solution_of = None` is the way to say that the
  solution still holds. What is said depends on where the image goes: a FITS or ASDF file has
  the WCS keywords and needs no warning, and for a TIFF or PNG, which holds no properties, the
  question is not asked at all.
- **`XISF:CompressionLevel` is not written.** It looked like the place to name the level of the
  codec. PixInsight's is a number of its own scale, 0 to 100, whatever the codec: 12 there would
  not have meant LZ4HC 12.
- **The WCS keywords of an image in memory are converted once.** They were turned to the stored
  row order when the image was added and again when it was written, with pixel coordinates that
  are not symmetric about the middle row: CRPIX2 came back changed in its last digits. The image
  now says which order its keywords describe, and the writer converts them if that is not the
  order it stores.
- **What a file may cost is counted before it is decompressed.** `xisfconv_property_read` reads
  one property at a time, on demand, and each read was checked against the budget on its own:
  a thousand blocks that each inflate to the limit passed. The file now keeps the count across
  reads, and a property counts once however often it is read.
- A new String of more than 3072 bytes given through the API is stored as a data block and so
  compressed with the codec. Keyword text keeps its UTF-8 in XISF, where it is XML; in FITS
  and ASDF it is ASCII as before.
- A TimePoint is a date that exists (no 30 February), and Python writes one with the digits it
  has: a `datetime64` of nanoseconds keeps them, and an offset from UTC that is not whole
  minutes (local mean time before the time zones) is written as the UTC time it is. A
  `datetime64` has no zone and is written without one, as a `datetime` without `tzinfo` is.

### LZ4 written by the library's own compressor (0.15.0)

- The `xisf` package writes `lz4` and `lz4hc`, and its documentation recommends `lz4hc` with
  shuffling; an interface that refused them would not be its interface. PixInsight writes them
  too. The block format is small: a compressor with one hash table (the codec `lz4`) and one
  with hash chains and lazy matching for the levels 1 to 12 (`lz4hc`) are about 150 lines, where
  linking liblz4 would be a second required dependency on three platforms and in the wheels.
- They are not the reference compressors and do not make the same bytes. They make blocks the
  lz4 library decodes, which is what the format asks, and come within a percent of its sizes on
  image data. Tests decode the blocks with the lz4 library itself, for every size around the
  limits of the format (no match in the last 12 bytes, 5 literals at the end, the window of
  65535), and a fuzzer checked 84 000 blocks against those rules and its round trip.
- One mistake worth keeping: the first version lengthened its step through incompressible data
  with the distance since the last match. After 49 MB of noise (the low bytes of shuffled
  float32 samples) the step was 768 KB, and the 16 MB that compress, behind them, were never
  looked at. The reference adds a byte per 64 failed looks, which grows with the square root of
  the distance. Found by comparing sizes with the lz4 library on a real frame, not by any test
  of correctness.
- **A compression level and byte shuffling off** are options of the writer (`xisfconv_write`,
  `xisfconv.write`) because the interface of the `xisf` package has them. A conversion and a
  rewrite keep the usual level and always shuffle; the tool has no option for either.
- LZ4 is written to XISF only. ASDF has its own LZ4 layout, which is read; nothing asks for it
  to be written.

## Testing

- `tests/run_tests.py` drives the built program (6128 checks at 0.16.0, 6141 with OpenXISF beside it). The Python packages it
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
- The Python package is tested with pytest (`python/tests`, 387 tests at 0.16.0): the same
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
  file that astropy still had mapped into memory. The first Windows run of 0.13.0 failed on the
  same thing again, in a new test: Wine lets a mapped file be replaced, Windows does not. Tests
  that write a file again after astropy read it open it with `memmap=False`, and since 0.14.1 the
  two test scripts switch astropy's mapping off altogether.) The wheels for macOS and Windows have
  only CI to prove them.
- MinGW is not the compiler of the Windows build: CI uses Microsoft's, and that one is not at hand
  here either. 0.14.0 did not compile there: `if constexpr (isFloat)` inside a lambda, with
  `isFloat` a `constexpr` variable of the function template around it, is "not a constant" for
  MSVC (error C2131), though GCC, clang and MinGW take it. Since 0.14.1 the condition is written
  out where it is used (`if constexpr (std::is_floating_point<T>::value)`); a compile-time
  condition inside a lambda names the type, never a local of the enclosing function. The same
  release gives a starting value to the variables that MSVC called potentially uninitialized in
  `src/wcs.cpp` (warning C4701; they were always set before use).
- `xisfconv.xisf` is tested against the package it stands in for: for files that package wrote
  and files this library wrote, in every codec, both must return the same dictionaries, down to
  key order and dtypes, and the same arrays; the package must read what the module writes (but
  for what it reads from no file: a property that is not-a-number, a vector without elements);
  and each difference its documentation names has a test that shows it.
- A review by a reader who did not write the code, before delivery, found seventeen things in
  0.15.0 that every test passed over: the old solution written next to new keywords, values the
  module could read and not write again, the budget, the DOCTYPE. The tests had compared with
  the `xisf` package on the files that package writes; the findings were all in files it does
  not write. Hand-made headers (`handmade` in `python/tests/util.py`) are now part of the
  module's tests. A second review, of the fixes, found nine more, most of them next to a fix:
  the solution rule held for `read_image` and not for the two other ways properties come from
  a file, and the long texts that the first review asked to be compressed were PixInsight's
  own, moved out of the header. A third found twelve, smaller, again next to the fixes. A fix
  is new code and gets the review new code gets.
- A struct that grows is tested from the side of a program built before it grew: the C test hands
  over `xisfconv_image` and `xisfconv_write_options` in the size of 0.14, with garbage behind.
  (`xisfconv_image` ended in four bytes of padding, as `xisfconv_convert_options` did in 0.13: the
  new pointer starts behind them, and a `static_assert` says so.)
- Distributed units (0.16.0) are tested from both sides without xisfconv: the test scripts take
  the two files apart with `struct` and hold them against sections 9.3, 9.4 and 10.3 of the
  specification (signature, reserved fields, the index node, positions, lengths, unused space),
  and build units by hand in the forms a writer may choose (several index nodes, free elements,
  decimal identifiers, files that are one block, subdirectories, names with parentheses and
  blanks). Each way out of the header's directory has a test for each setting, in the tool, in
  C, through ctypes with six threads that hold different settings at once, and in Python.
- Two reviews of 0.16.0 by readers who did not write it, one of the reader and its rules for
  following a header, one of writing, replacing files and the interfaces, found what the tests
  had passed over, again: memory that a small or unrelated file could ask for (an SVG read
  whole, the budget raised by naming a large file, index nodes lying in each other), the
  monolithic file that read its neighbours, a name with `&` written into the header as it was,
  and three ways to lose data with `--force` or in place (the header that could not follow,
  the shared data blocks file, a temporary file that was the input's data). Each has a test
  now; the ones that need a file that cannot be renamed run where `chattr +i` works.
- CI builds and runs the suite on Linux, macOS and Windows.

## How changes are made

- One commit per feature, with the version bumped in `include/xisfconv.h` and the
  README and `TODO.md` updated in the same commit.
- The work is done together with Claude (Anthropic), credited as co-author in the commit messages.
  Each change is delivered as a `git format-patch` file to apply with `git am`, plus a source
  archive, after it has been built and tested on a clean clone.
- Large features (so far ASDF, XISF rewriting, tile compression and the properties) get an
  independent review pass before delivery; its findings are fixed first. The review of 0.13.0
  is the example of why: every test passed, and it found that PixInsight's own files lost their
  carriage returns, that a file of 89 KB could ask for 12 GiB, and a read beyond a buffer.
- Limitations are written into the README when a feature ships, not left to be discovered.
- Messages to the user say what happened and what to do about it; nothing is skipped silently.

## Not decided yet

- A CMake package for the static library.
- Order of the remaining items in `TODO.md` after the library.
