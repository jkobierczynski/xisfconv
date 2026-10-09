# Changelog

What changed in each version of xisfconv, for whoever uses the tool, the library or the Python
package. The newest version comes first. A version marked *released* has binaries on the
[releases page](https://github.com/jkobierczynski/xisfconv/releases); the others are states of the
source. What a feature does in full is in [`MANUAL.md`](MANUAL.md); why it was made the way it is,
in [`DEVELOPMENT.md`](DEVELOPMENT.md).

Under "changed" stands what makes the same command, or the same call, do something else than
before.

## 0.20.0 (9 October 2026)

**Images read and written a piece at a time.**

Changed
- A conversion and a rewrite no longer hold an image whole: it is read, converted and written in
  bands of rows of about 4 MiB. What has to be kept between reading and writing (a compressed
  block of the input, decompressed; the pixels of compressed XISF output and what they compress
  to) is kept in memory up to 256 MiB together, beyond that in temporary files beside the
  output, which are gone when the conversion ends, however it ends. Converting a 192 MB image
  took 0.2 to 0.8 GB of memory and takes 10 to 260 MB; a 4.3 GB image becomes a TIFF file in
  10 MB. The output is the same, byte for byte, but for Zstandard (below). In the library and
  the Python package, `xisfconv_convert` and the rewrites work this way; reading an image into
  memory and writing one from it still take the whole array.
- Zstandard compression (XISF and ASDF output) is given its input in pieces of 1 MiB: other bytes
  than before for the same data, which decodes the same.
- Ctrl-C (or a request to terminate) stops the tool at the next piece of its work, also within
  one image: the partly written output and its temporary files are removed and the exit status
  is 130. Before, the program was ended where it stood and left the `.part` file. A second
  Ctrl-C ends the program at once.
- Progress: a step that goes on is reported again every 8 MiB or so of data (the stage and
  numbers of the last report), so that a progress handler, `xisfconv_context_cancel` and Ctrl-C in
  Python stop a call within one image. A handler is called more often than before.

Added
- BigTIFF: TIFF output whose pages could take more than 4 GiB is written as BigTIFF (before, an
  error). Smaller files are classic TIFF as before.
- The environment variables `XISFCONV_MEMORY_LIMIT` (what the kept data may take in memory; `0`:
  all of it in temporary files) and `XISFCONV_PIECE_BYTES` (the size of a piece).

## 0.19.0 (9 October 2026)

**The compression level and byte shuffling, for conversions and rewrites.**

Added
- `--level n`: the compression level of the codec for XISF output, from FITS, ASDF and DNG and
  XISF -> XISF (zlib 1 to 9, LZ4HC 1 to 12, Zstandard 1 to 22). It implies `-c`. In a rewrite
  every block is compressed again, since a file does not say with which level its blocks were.
- `--no-shuffle`: XISF blocks compressed without byte shuffling. It implies `-c`.
- Library: `compression_level` and `shuffle` in `xisfconv_convert_options` and
  `xisfconv_rewrite_options` (the latter after a `reserved` field where its old layout had
  padding). Python: `level` and `shuffle` for `convert`, `rewrite`, `rewrite_in_place` and
  `stored_as_requested`, as `write` has them.

Fixed
- Python: `write(level=...)` took a level above 2**31 - 1 cut to 32 bits, as another level; it
  is refused. `shuffle=None` turned shuffling off; it is the default (on).

## 0.18.1 (8 October 2026), released

**Colour pictures of a mosaic.**

Added
- `--debayer`, for TIFF and PNG output: a colour picture of the mosaic of a one-shot colour
  camera (DNG, or FITS and XISF frames with `BAYERPAT` or a colour filter array), by bilinear
  interpolation of its 2 x 2 pattern, before `--bin`, `--resize` and `--stretch`; no white
  balance. In the library, the field `debayer` of `xisfconv_convert_options` (it was reserved),
  and `debayer=True` for `xisfconv.convert` in Python.

## 0.18.0 (8 October 2026), released

**DNG input.** The raw image of a DNG file, the format some
cameras write themselves and Adobe DNG Converter makes of the raw files of every other camera.

Added
- DNG files are read, by the tool, the library and the Python package: the raw image as the
  sensor recorded it, cut to its active area, with the linearization table applied and nothing
  else done (not demosaiced). `xisfconv IMG_0001.dng` writes XISF; FITS, ASDF, TIFF and PNG as
  for any other input.
- The colour filter pattern goes along: to XISF as the `ColorFilterArray` element of any size
  (Bayer 2 x 2, X-Trans 6 x 6), and as `BAYERPAT` for a 2 x 2 RGB pattern. A `LinearRaw` file
  (Apple ProRAW) becomes an RGB image.
- Keywords from the file and its EXIF directory: `INSTRUME`, `DATE-OBS` (in UTC when the camera
  recorded its offset from UTC, else `DATE-LOC`), `EXPTIME`, `ISOSPEED`, `FOCALLEN`, `BLKLEVEL`,
  `WHTLEVEL`.
- Uncompressed, lossless JPEG and Deflate compressed raw data. Lossy DNG, JPEG XL and floating
  point data are not read.
- Directories and patterns take `.dng` files; `--info`, `--dump-header` and `--verify` read them.
- Library: `XISFCONV_FORMAT_DNG` (input only); the colour filter array of a DNG image in
  `xisfconv_image_info` and as the detail `cfaPattern`. Python: `format` and `detect_format`
  give `"dng"`.

Fixed
- GCC 14 warned of a possibly dangling reference (`-Wdangling-reference`) in the code that writes
  XISF properties to XISF and ASDF files; a false alarm, written another way so that the build is
  free of warnings again.

Changed
- A TIFF file given as input is said to be one ("a TIFF file that is not a DNG file") instead of
  "not an XISF 1.0 file (bad signature)".

## 0.17.0 (8 October 2026)

**Whole folders, and patterns on Windows.**

Added
- A directory as input: `xisfconv -t fits lights/` converts the image files in `lights` and below
  it. The files that already are what is asked for are passed over (with `-t fits`, the FITS
  frames of the folder), where each would be an error; a directory of one format needs no `-t`,
  and one that holds both XISF and FITS or ASDF files is refused without it. `--in-place`,
  `--info` and `--dump-header` take directories as well.
- With `-d`, the files of a directory keep their places below it, and the folders are made as
  needed.
- `--skip-existing`: an output that exists is left as it is and its input passed over, so that a
  later run on the same folder converts what was added.
- Patterns: an argument with `*` or `?` that names no file stands for the names it matches. That
  makes `xisfconv *.xisf` work in cmd and PowerShell, which do not expand patterns; on Unix it
  applies to a pattern in quotes. `--verify` takes patterns too. On Windows, `xisfconv /?` shows
  the help.
- A run on a directory or a pattern ends with its counts: files converted, passed over, failed.

Changed
- Two inputs with one output name: the second is an error. Up to 0.16 it was one only without
  `--force`, and with `--force` the second replaced the output of the first.
- An input whose output is another input of the same run (`xisfconv -f frame.xisf frame.fits`) is
  an error. Up to 0.16 `--force` converted each over the other.
- A directory given where a file is meant is no longer an error ("is a directory, not a file"):
  it is converted. With `-o` it is refused, with exit status 2.
- A file given twice (`a.xisf ./a.xisf`, or a directory and a file of it) is converted once. Up to
  0.16 the second was an error ("already exists"), and with `--force` it was converted again.
- An output name that is a link leading nowhere is not replaced without `--force`. Up to 0.16 the
  link was taken for free.
- `-q` also silences the warning that a directory given to `--verify` holds no image files.

Fixed
- `--verify` on a directory whose listing broke off half way (an I/O error) checked what it had
  read and said nothing of the rest: it is an error now.

## 0.16.0 (7 October 2026), released

**Distributed XISF units.** An XISF unit may be one file, or a header file (`.xish`) and the files
it names, where the data is (`.xisb`). xisfconv reads and writes such units, in the tool, the C
API and the Python package.

Added
- Reading a unit by the name of its header file, wherever an XISF file is taken; `--info` shows
  the unit and the files its header names, `--verify` reads every block from where the header
  says.
- Writing one: `-t xish`, or an output name that ends in `.xish`; in the library and in Python the
  name of the output decides. `xisfconv frame.xish -t xisf` packs a unit into one file,
  `xisfconv frame.xisf -t xish` unpacks one, `--in-place` rewrites a unit where it is.
- `--external-files header-dir|anywhere|none`: how far a header is followed. By default only to
  files in its own directory and below, and only from a header file named `.xish`. With
  `anywhere`, a monolithic `.xisf` file that keeps a block in another file is read as well (up to
  0.15 such a file was not read at all). Nothing is ever fetched from a network.
- C API: `xisfconv_context_set_external_files`, `xisfconv_context_external_files`,
  `xisfconv_external_count`, `xisfconv_external_file`, `xisfconv_external_status`,
  `xisfconv_unit_size`, the detail `"unit"` of a file, the status `XISFCONV_ERR_NOT_ALLOWED`.
- Python: `File.unit`, `File.external_files`, `File.unit_size`, `NotAllowedError` (a
  `PermissionError`), and `external_files=` on the functions that read a file.
- A manual of the library for C, C++ and Python, [`docs/manual.html`](docs/manual.html), with
  example programs in the three languages (`examples/`).
- The documents: [`MANUAL.md`](MANUAL.md) for the tool, this changelog, `SECURITY.md`,
  `CONTRIBUTING.md`, `CITATION.cff`, a man page (`man/xisfconv.1`), and the layout of the XISF
  properties in FITS and ASDF files as a document of its own
  ([`docs/xisf-properties-in-fits-and-asdf.md`](docs/xisf-properties-in-fits-and-asdf.md)).

Changed
- A file that `--force` replaces is no longer removed first. It stays until the new file has its
  name; where a file cannot be renamed over another, the old one is set aside as
  `<name>.replaced` for that moment and put back if the new one cannot take its place. The data
  blocks file of a unit that is replaced is set aside that way on every system, until the header
  has its name.

## 0.15.0 (6 October 2026)

Added
- Python: `xisfconv.xisf`, the interface of the `xisf` package (`XISF(fname)`, `read_image`,
  `get_images_metadata`, `XISF.write` and the rest), on this library. A program written for that
  package runs with its import line changed.
- XISF properties written from the values of a program: `xisfconv_properties_new`, `_set`,
  `_set_array` and `_set_as_read` in the C API; `PropertyDict`, `properties=` and
  `file_properties=` of `write` in Python. `read_image` returns the properties of an image with
  their types, comments and formats, and `write` writes them.
- `xisfconv_property_read` reads a vector or a matrix in the type of its elements, complex ones
  included; `xisfconv_property_stored` says how a property is stored; `xisfconv_wcs_digest`.
- LZ4 and LZ4HC written to XISF (`--codec lz4|lz4hc`), with a compressor of the library's own.
- For images written from memory: a compression level, byte shuffling off, and the name of the
  creating application.

Changed
- Python: vectors and matrices among the properties are read in the type of their elements (up
  to 0.14 all as float64, and complex ones not at all).
- An astrometric solution that was read from a file is written only with an image of the size
  and with the WCS keywords it was read with; otherwise it is left out and made from the
  keywords.

Fixed
- The WCS keywords of an image in memory were converted between the row orders twice.
- `<!-->` was read as a whole XML comment.
- A String of the header with a blank or a line break at one of its ends came back from FITS as
  a data block, which an XML reader does not read as it read the header.
- What the properties of a file may declare was checked for each read and not for the file.

## 0.14.1 (6 October 2026), released

Fixed
- 0.14.0 did not compile with Microsoft's compiler. No change in what the program does.

## 0.14.0 (6 October 2026)

Added
- Smaller pictures for TIFF and PNG output: `--bin <n>` averages n × n pixels into one,
  `--resize` takes the longest side (`256`), a box to fit (`1024x768`) or a percentage (`50%`).
  The picture is made before a stretch and is never larger than the image. In the C API
  `fit_width`, `fit_height`, `scale` and `bin` of `xisfconv_convert_options`; in Python
  `convert(bin=, resize=, scale=)`.
- Previews of XISF, FITS and ASDF files in Linux file managers: a thumbnailer entry and the file
  types, installed by CMake and packed into the Linux release archive.

## 0.13.0 (6 October 2026)

Added
- XISF properties go through FITS and ASDF and come back: every property of every image and of
  the file, with its type, its exact value, its comment and its format. In FITS a binary table
  behind each image (`XISF_PROPERTIES`, and `XISF_METADATA` for the file), in ASDF the key `xisf`
  of the tree. The astrometric solution of PixInsight comes back as PixInsight wrote it, as long
  as the WCS keywords of the file are still what they were.
- `--no-properties` (`properties` in the options of the library, `properties=` in Python) turns
  that off in both directions.
- `--info` and the property functions of the library show the properties a FITS or ASDF file
  carries. `xisfconv_property_format`, `Properties.format`.

Changed
- A FITS file converted from an XISF file with properties has an extension more for each image
  that has some, and one for those of the file; an ASDF file has one more key. With
  `--no-properties` the output is what 0.12 wrote.

## 0.12.1 (6 October 2026), released

Fixed
- The wheel of the Python package for Windows could not be built. No change in what the program
  does.

## 0.12.0 (6 October 2026), released

Added
- Tile-compressed FITS is written, without loss (`image.fits.fz`, the format of fpack): `RICE_1`
  for integers of 8, 16 and 32 bits, `GZIP_2` for floating point, from XISF, ASDF and FITS input.
  `-c`, `--codec zlib`, or an output name that ends in `.fz`; the codec of the options in the
  library; `codec=True` or the name in Python.

Changed
- `xisfconv -c image.xisf` writes `image.fits.fz` where it wrote `image.fits`: up to 0.11 `-c` had
  no effect on FITS output. `--codec zstd` with FITS output is an error.
- An image name too long for a FITS card is shortened.

## 0.11.1 (5 October 2026), released

Fixed
- On Windows, a directory given as input was reported as a file that cannot be opened.

## 0.11.0 (5 October 2026)

Added
- The Python package `xisfconv`: `read`, `read_image`, `open`, `write`, `convert`, `rewrite`,
  `rewrite_in_place`, `verify`, `auto_stretch`, `apply_stretch`, with NumPy arrays in and out.
  `xisfconv.astropy` makes `CCDData.read("image.xisf")` work and reads and writes HDU lists.
  Ctrl-C stops a conversion between its steps and leaves no partly written file.
- C API: `xisfconv_fits_keywords`, `xisfconv_keywords_fits_text`; messages kept in the context
  (`xisfconv_context_keep_messages` and its companions); `xisfconv_context_cancel` and
  `xisfconv_context_running`; a second kind of progress handler for hosts
  (`xisfconv_context_set_host_progress`).

Changed
- A keyword value that is text without quotes is written to FITS in quotes.
- A keyword that cannot be written as a FITS card (a `=` in a long name, no room for the value)
  is left out with a warning.
- An output name that is a directory or a device is refused.
- A FITS or ASDF file that changes between being opened and being read is an error.

## 0.10.1 (5 October 2026)

Changed
- The same file converts to the same bytes on every processor. On arm64 (Apple Silicon) the last
  digits of some results differed from those on x86-64: of a fitted WCS, a stretched picture, an
  unpacked `.fits.fz`.

## 0.10.0 (5 October 2026)

Added
- **libxisfconv**: the converter as a library with a plain C API (`include/xisfconv.h`): files of
  the three formats as one model, pixels in any sample format and row order, keywords,
  properties, WCS keywords for any row order, conversion, rewriting, verification, writing arrays
  as XISF, FITS, ASDF, TIFF or PNG, the screen stretch. Static by default, shared with
  `-DBUILD_SHARED_LIBS=ON`, with a pkg-config file and a CMake package. Its licence is the LGPL,
  version 3 or later; the tool stays under the GPL.

Changed
- On Windows, file names are Unicode.
- Numbers are read and written with a decimal point whatever locale is set.

Fixed
- A FITS IMAGE extension with `PCOUNT` or `GCOUNT` other than 0 and 1 crashed the reader.
- A rewrite of a file that declares an absurd block size wrote zeros without end.
- The conversion of WCS keywords between the row orders missed the alternate descriptions, the old
  form `PC001002` and a missing `CRPIX2`.

## 0.9.2 (5 October 2026)

Changed
- Writing a SHA3-256 or SHA3-512 checksum prints a warning: PixInsight 1.9.3 does not open such
  files. `xisfconv --checksum sha256 --in-place file.xisf` repairs one.

## 0.9.1 (4 October 2026)

- The code split into a library and the tool. No change in what the program does.

## 0.9.0 (4 October 2026)

Added
- Tile-compressed FITS (`.fits.fz`, as fpack, CFITSIO and astropy write it) is read: `RICE_1`,
  `GZIP_1`, `GZIP_2`, `PLIO_1` and `NOCOMPRESS`, quantized floating point with its dithering.
  `HCOMPRESS_1` is not read. `-t fits` writes such a file as a plain FITS file.

## 0.8.0 (4 October 2026)

Added
- XISF to XISF: a file rewritten with its data blocks stored another way (`-t xisf`,
  `-o name.xisf`, `--in-place`). `-c` and `--codec` compress or decompress, `--checksum` adds,
  replaces or removes checksums, `--image` keeps one image. The output is read back and compared
  with the input.
- `--verify` checks XISF, FITS and ASDF files, and the directories given, without converting.
- SHA3-256 and SHA3-512 checksums are verified and can be written.

Changed
- A `<name>.part` file that already exists is not overwritten without `--force`, and never when it
  is the input.

## 0.7.0 (4 October 2026)

Added
- TIFF and PNG export from FITS and ASDF input, with `--bits`, `--compress` and `--stretch`.

## 0.6.0 (2 October 2026), released

Added
- ASDF in both directions: `-t asdf` from XISF and FITS, and ASDF input converted to XISF or
  FITS. The images are stored the way astropy's `asdf` packages read as a list of HDUs.

## 0.5.0 (2 October 2026), released

Added
- FITS to XISF also writes the astrometric solution as PixInsight's own properties, because
  PixInsight reads only the linear part of WCS keywords. A SIP distortion becomes a spline that
  PixInsight rebuilds.

## 0.4.0 (2 October 2026)

Added
- FITS to XISF: a FITS input is converted to XISF. The primary HDU and IMAGE extensions, every
  `BITPIX`, the conventions for unsigned data, `BSCALE` and `BZERO`, `CONTINUE` and `HIERARCH`
  cards. XISF output with Zstandard or zlib and byte shuffling, subblocks, and SHA-1, SHA-256
  or SHA-512 checksums.
- `--codec`, `--checksum`, `--bounds`; `--info` for FITS files.

Changed
- Long strings are written to FITS on `CONTINUE` cards and no longer cut.

## 0.3.3 (1 October 2026), released

Fixed
- Compressed TIFF files of 64-bit integers could not be read by libtiff before 4.4: they are
  written without a predictor.

## 0.3.2 and 0.3.1 (1 October 2026), released without binaries

- The licence is the GPL, version 3 or later. FITS files name the program that wrote them
  (`PROGRAM`). Builds and tests on Linux, macOS and Windows, and binaries built for a release.

## 0.3.0 (1 October 2026)

Added
- PNG output.
- WCS keywords from PixInsight's astrometric solutions, with SIP polynomials fitted to its
  distortion model.

## 0.2.0 (1 October 2026)

Added
- `--stretch`: the screen stretch PixInsight saved in the file, or an automatic one, linked or per
  channel.

## 0.1.1 (1 October 2026)

Changed
- FITS rows are written bottom-up, as FITS viewers expect them, so that an image shows the same
  way up as in PixInsight. `--top-down` keeps the order of XISF.
- Numbers in keywords are written with the upper-case `E` the FITS standard asks for.

## 0.1.0 (1 October 2026)

- XISF to FITS and TIFF.
