# xisfconv

A small, dependency-light command-line converter between PixInsight **XISF**, **FITS** and **ASDF**,
in every direction, with **TIFF** and **PNG** export from all three.

```
xisfconv M31_integration.xisf                 # -> M31_integration.fits
xisfconv -c light_0001.fits                   # -> light_0001.xisf (zstd-compressed)
xisfconv -t asdf M31_integration.xisf         # -> M31_integration.asdf
xisfconv observation.asdf                     # -> observation.xisf
xisfconv -t tiff -c -b u16 *.xisf -d export/  # batch to 16-bit Deflate TIFFs
xisfconv -t tiff -s -b u8 integration.xisf     # stretched 8-bit TIFF for GIMP
xisfconv -t png -s -b u8 integration.xisf      # stretched 8-bit PNG for the web
xisfconv -t png -s -b u8 light_0001.fits       # quick look at a raw FITS frame
xisfconv -c --in-place *.xisf                 # recompress XISF files with zstd, replacing them
xisfconv --verify ~/astro/2026                # check every XISF, FITS and ASDF file below a folder
xisfconv --info light_0001.xisf               # geometry, codecs, FITS keywords, properties
```

## Features

**Reading (monolithic XISF 1.0)**
- Sample formats UInt8/16/32/64, Float32/64; Gray, RGB (and extra/alpha channels)
- Planar and Normal (interleaved) pixel storage, little- and big-endian data
- Compression: zlib, LZ4, LZ4HC (built-in decoder), Zstandard (via libzstd), each with or without
  byte shuffling, including compressed **subblocks**
- Data blocks as attachments, `inline:base64`/`inline:hex`, or `embedded` `<Data>` elements
- Checksum verification: SHA-1, SHA-256, SHA-512, SHA3-256 and SHA3-512
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

**XISF → XISF: another compression, checksums, one image of several** (`-t xisf`, `-o name.xisf` or `--in-place`)
- Rewrites a file with its attached data blocks stored another way, for example to shrink an archive
  of uncompressed files: `-c` compresses every attached block with Zstandard and byte shuffling
  (`--codec zlib` for zlib, `--codec none` to store everything uncompressed). On the uncompressed
  71 MiB test frame from PixInsight that gives 52 MiB, the size PixInsight's own zstd files have.
- `--checksum sha1|sha256|sha512|sha3-256|sha3-512` adds a checksum to every attached block
  (replacing others); `--checksum none` removes them. Without the option, checksums the file has are
  kept, and computed again with the same algorithm for blocks whose stored bytes change.
- `--image n` writes a file that holds only that image, with its keywords, properties and other
  blocks, and the file metadata.
- Nothing else changes. The XML header is carried over as text: only the `location`, `compression`,
  `subblocks` and `checksum` attributes of the attached blocks are edited, plus the
  `XISF:CompressionCodecs` / `XISF:CompressionLevel` / `XISF:BlockAlignmentSize` file properties that
  describe the storage. Pixels, keywords, properties (astrometric solution, processing history), ICC
  profile, thumbnail, comments and elements xisfconv does not know all stay as they are, and so do
  the creation time and the creating application. Blocks stored inline or embedded in the header
  are left where they are. Blocks already stored as requested are copied, not compressed again, and
  a block the codec cannot shrink is stored uncompressed. Uncompressed blocks are aligned to 4096
  bytes; compressed blocks follow each other directly, as in PixInsight's files.
- It is careful with the data. The input's checksums are verified and every compressed block is
  decompressed, so a damaged file is refused rather than given a fresh checksum. A block with a
  checksum of a kind xisfconv does not know is copied with it, never stored differently. The output
  is then read back, every block compared with the input and the whole file verified (`--no-verify`
  skips these checks).
- `--in-place` replaces the input file. The new file is written next to it as `name.xisf.part`, read
  back and compared (always, even with `--no-verify`), given the permissions of the original,
  flushed to disk, and only then renamed over the original; if anything fails the original is
  untouched. A symbolic link is followed (the file is replaced, the link stays); a read-only file
  is refused. Files that are already stored as requested are left alone, so
  `xisfconv -c --in-place *.xisf` can be run again on a folder. Without `--in-place`, give `-o` or
  `-d`: the input is never overwritten by accident.
- All codecs PixInsight writes are read (zlib, LZ4, LZ4HC, Zstandard, with subblocks); the output
  uses zlib or Zstandard. Tested on PixInsight 1.9.3 files in each of those codecs, Float32, Float64
  and UInt32, with SHA-1/256/512 checksums: every block of every rewritten file decodes to the
  original bytes.

**Verifying files** (`--verify <file or directory>...`)
- Reads every file completely without converting anything and says whether it is intact. A
  directory stands for the `.xisf`, `.fits`/`.fit`/`.fts` and `.asdf` files in it and below it.
- XISF: every data block (pixels, properties, ICC profile, thumbnail; attached, inline or embedded)
  has its checksum verified where it has one, is decompressed, and for images compared with the
  size the geometry requires.
- FITS: the structure of every HDU is checked, and the `CHECKSUM` and `DATASUM` keywords where the
  file has them (most capture programs do not write them; astropy and CFITSIO can).
- ASDF: the tree is parsed, and every binary block has its MD5 checksum verified and is decompressed.
- One line per file, `OK` with what was checked or `FAILED` with the reasons; with several files a
  count at the end. The exit status is 1 if any file failed (or a directory could not be read), so
  it can be used in scripts. `-q` prints the failures only. A file with a part xisfconv cannot
  check (a bzip2-compressed ASDF block, a checksum of an unknown kind) is reported as
  `NOT FULLY CHECKED`, with that part named; it does not count as a failure. A file without checksums can still fail (truncated, compressed data that
  does not decompress), but a changed pixel in uncompressed data goes unnoticed: add checksums with
  `xisfconv --checksum sha1 --in-place` to be able to tell later.

**ASDF output** (`-t asdf` or `-o name.asdf`, from XISF or FITS)
- [ASDF](https://www.asdf-format.org) is the YAML-plus-binary-blocks format of the Python astronomy
  world (asdf, astropy, the Roman Space Telescope pipeline). xisfconv writes the images as a FITS HDU
  list under the tree's `fits` key, using the tag `tag:astropy.org:astropy/fits/fits-1.0.0`: every
  HDU has its header as `[keyword, value, comment]` entries, with numbers, logicals and strings as
  YAML values of that type, and the pixels as an `ndarray` of shape `[height, width]` or
  `[channels, height, width]` in a binary block. With `asdf` and `asdf-astropy` installed, Python
  gets an astropy `HDUList`:

  ```python
  import asdf
  with asdf.open("M31_integration.asdf") as af:
      hdul = af["fits"]                    # astropy.io.fits.HDUList
      pixels = hdul[0].data                # numpy array
      exposure = hdul[0].header["EXPTIME"]
  ```

  Without `asdf-astropy` the same data arrives as plain lists and arrays
  (`af["fits"][0]["data"]`, `af["fits"][0]["header"]`), with a warning about the unknown tag.
- The content is what the FITS output would hold: the same keywords (including those derived from
  XISF properties and the WCS of a PixInsight plate solution), the same row order (bottom-up by
  default, recorded in `ROWORDER`; `--top-down` keeps XISF's order), the same `--bits` and
  `--stretch` handling. Unsigned samples are stored as they are (no BZERO offset).
- `-c` compresses the blocks with zlib, which every ASDF reader has. `--codec zstd` uses Zstandard,
  which Python reads once the `asdf-compression` package is installed. Every block carries an MD5
  checksum, and a block index is written at the end of the file.
- The file declares ASDF Standard 1.5.0, which old and current releases of the Python library read:
  tested with asdf 5.4 / asdf-astropy 0.11 and with asdf 2.15 / asdf-astropy 0.4. One caveat for
  asdf 2.x: its optional `validate_checksums=True` rejects compressed blocks written to the
  standard (by xisfconv or by asdf 3 and later), because it expected the checksum of the uncompressed
  data. Opening without that option, the default, works.
- Keyword text is reduced to printable ASCII, as in FITS (astropy rejects anything else in a header).
  An integer keyword beyond 64 bits is written as a string, because ASDF does not allow such
  literals in the tree; xisfconv says so when it happens.

**ASDF input** (an ASDF file is converted to XISF by default, or to FITS with `-t fits`)
- FITS HDU lists, as xisfconv, asdf-astropy and older writers store them (`fits/fits-1.x` tags of
  astropy.org and stsci.edu), are read with their headers. The pixels follow the same path as a FITS
  input: flipped to XISF's top-down order unless `ROWORDER` says otherwise, WCS keywords turned into
  PixInsight solution properties, and so on.
- Any other numeric array in the tree with two or three dimensions is taken as an image as well, so
  a file made with `asdf.AsdfFile({"image": array}).write_to(...)` converts too, and so should data
  products that keep their pixels in arrays under custom tags (tested with files of that shape, not
  yet with real mission data). The image is named after its place in the tree (`roman.data`);
  `--info` lists what was found and `--image n` picks one. Three-dimensional arrays are read as
  `[channels, rows, columns]`, or as `[rows, columns, channels]` when the last axis has at most four
  entries. Such arrays carry no row order: bottom-up is assumed (the FITS and numpy/astropy habit),
  and `--top-down` says otherwise. Only the pixels of these arrays are converted; the rest of the
  tree (metadata, generalized WCS objects) is not carried over.
- Data types: 8/16/32/64-bit integers, signed and unsigned, and 16/32/64-bit floats, little- or
  big-endian. Signed integers are mapped as for FITS input; 16-bit floats become Float32.
- Blocks: uncompressed, zlib, LZ4 and Zstandard (the asdf library's `lz4` and `zstd` layouts), with
  padding, streamed blocks and arrays that share a block. MD5 checksums are verified
  (`--no-verify` skips that), in both conventions in use: the asdf library computed them over the
  uncompressed data before version 3 and over the stored bytes since.
- The YAML tree is read by a built-in parser (no libyaml needed) that follows PyYAML, the parser
  behind the Python library, in how plain values become numbers, logicals or strings.
  `--dump-header` prints the tree.
- FITS ↔ ASDF is a repackaging: same HDUs, same keywords, rows left in the order they are stored in.

**TIFF output**
- 8/16/32/64-bit unsigned or 32/64-bit IEEE float samples, chunky (interleaved) layout
- Optional Deflate compression (`-c`) with horizontal or floating-point predictor
- ICC profile and resolution copied; extra channels written as ExtraSamples (first one = alpha)
- Multiple images become multiple pages

**PNG output** (`-t png` or `-o name.png`)
- 8- or 16-bit grayscale, gray+alpha, RGB or RGBA; ICC profile (iCCP) and resolution (pHYs) copied.
- Float data is scaled through its bounds to 16-bit; add `--stretch` for linear data.
- PNG holds one image: multi-image files write the first one (or the one chosen with `--image`).

**TIFF and PNG from FITS and ASDF input**
- The same export as from XISF: `-t tiff` or `-t png`, with `--bits`, `--compress` and `--stretch`, so
  `xisfconv -t png -s frame.fits` gives a viewable picture of any FITS or ASDF image without a
  detour through XISF. Exporting a FITS or ASDF file gives the same pixels as exporting the XISF file
  it was converted from (or to).
- Rows are flipped to the top-down order of TIFF and PNG unless the file says `ROWORDER = 'TOP-DOWN'`
  (`--top-down` / `--bottom-up` override, as for conversion to XISF).
- Integer data keeps its values. Floating point data has no declared range in FITS, so one is chosen as
  for XISF output: `0:1` when the data fits, else `0:65535` when it fits (ADU-scaled floats), else the
  data's minimum and maximum; `--bounds lo:hi` overrides. That range is black to white: it is used
  for conversion to integers and for the stretch, and floating point TIFF output is scaled so that it
  becomes 0..1, which is what image programs expect. The range used is printed.
- `--stretch` computes an auto-STF (`linked`, the default, or `unlinked`); FITS and ASDF files hold no
  saved STF. All planes of the image take part.
- A FITS cube with three planes is written as RGB. Any other cube becomes one grayscale TIFF page per
  plane (PNG: the first plane). Several HDUs become several TIFF pages; PNG takes the first, or the
  one chosen with `--image`.
- Keywords and WCS are not carried into TIFF or PNG.

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

Requirements: a C++17 compiler, CMake ≥ 3.15, zlib. libzstd is optional but recommended
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
xisfconv [options] <file>...      # any of XISF, FITS, ASDF -> any other of them, or TIFF/PNG

  -t, --to <fits|asdf|tiff|png|xisf>
                              output format (default: fits for XISF input, xisf for FITS and ASDF input)
  -o, --output <file>         output file name (single input only)
  -d, --outdir <dir>          directory for output files (default: next to each input)
  -f, --force                 overwrite existing output files
      --in-place              XISF -> XISF: replace the input file (after reading the new one back)
  -b, --bits <fmt>            output sample format: u8, u16, u32, f32, f64 (default: as stored)
  -i, --image <n>             convert only image n (0-based); default: all images
  -c, --compress              TIFF: Deflate with predictor; XISF: zstd + byte shuffling; ASDF: zlib
                              XISF -> XISF: every attached data block
  -s, --stretch[=mode]        screen stretch for viewing: auto (default), linked, unlinked, stf
      --top-down              from XISF: keep XISF's top-down row order in FITS/ASDF (default: bottom-up)
                              from FITS/ASDF: the rows are stored top-down
      --bottom-up             from FITS/ASDF: the rows are stored bottom-up, whatever ROWORDER says
      --no-property-keywords  from XISF: don't derive missing keywords from XISF properties
      --no-wcs                from XISF: don't write WCS from a PixInsight astrometric solution
                              to XISF: don't write PixInsight solution properties from WCS
      --sip-order <n>         from XISF: SIP distortion order (2-7, default 3; 0 = linear only)
      --no-verify             don't verify data block checksums
      --codec <zlib|zstd|none>  XISF and ASDF output: compression codec (zlib, zstd imply -c);
                              none = uncompressed (XISF -> XISF: decompress)
      --checksum <sha1|sha256|sha512|sha3-256|sha3-512|none>
                              XISF output: checksum of the pixel data;
                              XISF -> XISF: of every attached block (none removes them)
      --bounds <lo:hi>        from FITS/ASDF: range of floating point data (XISF bounds; black:white
                              for TIFF and PNG)
      --verify                check the files, and the image files in the directories, given;
                              converts nothing; exit status 1 if a file is damaged
  -I, --info                  print image geometry, keywords and properties; no conversion
      --dump-header           print the raw XML header (XISF), all keywords (FITS) or the YAML tree (ASDF)
  -q, --quiet                 suppress warnings
```

Output is written to `<name>.part` and renamed when complete, so an interrupted run never leaves a
half-written file under the final name. A `<name>.part` that already exists (the leftover of an
interrupted run, or another file) is not overwritten unless `--force` is given, and never when it
is the input itself. With several inputs, a failing file is reported and the rest are still
converted (exit status 1).

## Testing

```
pip install numpy astropy tifffile imagecodecs xisf lz4 zstandard pillow asdf asdf-astropy asdf-compression
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

TIFF and PNG export from FITS and ASDF input is checked against the export of the XISF file the input
was made from: for every sample format, gray and RGB, both row orders and a set of `--bits`,
`--stretch` and `--compress` combinations the pixels must be identical, and separate cases cover
ADU-scaled floats, signed data, NaN pixels, cubes and several HDUs.

XISF → XISF is checked with a reader in the test script that knows nothing of xisfconv: for source
files in every codec, with and without checksums and subblocks, and for every option set, all data
blocks of the output must decode to the bytes of the input, the header must be the same text once
the storage attributes are taken out, and the blocks must be stored the way the options say. The
source holds what a rewrite could lose: attached and inline properties, an embedded image, an ICC
profile, a thumbnail, comments, CDATA, entities and an unknown element. Damaged inputs must be
refused with the original left byte for byte as it was, and so must an input that is named like
the temporary file. `--verify` is tested on intact files, on files with one byte flipped in each
kind of place and on files cut short at each kind of place; FITS checksums come from astropy
(image HDUs and random groups), SHA-3 digests are compared with Python's hashlib.

ASDF is checked against Python's `asdf` library with `asdf-astropy` (the tests are skipped if those
are not installed). Files written by xisfconv must open without a warning, pass schema validation
and checksum validation, and yield an astropy HDU list with the pixels and header cards of the
corresponding FITS output. In the other direction the inputs are written by the library: plain trees
with arrays of every data type, byte order and compression, views and shared arrays, and HDU lists
serialized by asdf-astropy. A third set of files is assembled byte by byte in the test script (old
tags, padded and streamed blocks, both checksum conventions, damaged files). The YAML reader is
compared with PyYAML on random documents in all of PyYAML's output styles.

## Limitations / not yet done

- Distributed XISF units (`.xish` + `.xisb`) are not supported, only monolithic `.xisf` files.
- Complex sample formats and images with more than two dimensions are skipped.
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
  are kept as ordinary values.
- ASDF input: arrays stored inline in the tree or in another file, non-contiguous views, Fortran-ordered
  arrays, tables and structured or complex data types are skipped with a message; bzip2- and
  Blosc-compressed blocks are not read. Line breaks written as U+0085, U+2028 or U+2029 inside the
  tree are not recognized as such.
- ASDF output always uses the FITS HDU list layout described above; it does not write generalized WCS
  (gwcs) objects or instrument-specific data models.
- XISF output is not compressed with LZ4 (zlib and Zstandard only).
- XISF → XISF does not move blocks between the header (inline, embedded) and attachments. Replacing a
  file in place gives it a new inode: other hard links to the old file keep the old content.
- FITS output carries no CHECKSUM/DATASUM keywords yet; `--verify` checks them where a file has them.
- On Windows the shell does not expand `*.xisf`; name the files, or use a directory with `--verify`.
- The PixInsight spline distortion model is approximated by SIP polynomials, not carried over exactly.
- Please report any file that fails to convert, ideally with `xisfconv --info` output.

## Releasing

Bump the version in `src/common.hpp` and `CMakeLists.txt`, commit, then push a matching tag:

```
git tag v0.8.0 && git push origin v0.8.0
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
