# xisfconv: the manual

How the command line tool `xisfconv` is used and what it does with each format, option by option
and direction by direction; what is known not to work; and what was checked against PixInsight.
[`README.md`](README.md) is the short version, and `man xisfconv` the options at hand: the manual
page ([`man/xisfconv.1`](man/xisfconv.1)) is installed with the tool on Linux and macOS.

Programs use the same code as a library. For that there is a manual of its own,
[`docs/manual.html`](docs/manual.html) (C, C++ and Python, with example programs and the reference of
every function; one file, to be opened in a browser), and [`python/README.md`](python/README.md) for
the Python package. The chapters [The library](#the-library-libxisfconv) and [Python](#python) below
say what to know before reading those.

## Contents

- [Examples](#examples)
- [Options](#options)
- [Directories and patterns](#directories-and-patterns)
- [Reading XISF](#reading-xisf)
- [FITS output](#fits-output)
- [Astrometry (WCS) from PixInsight plate solutions](#astrometry-wcs-from-pixinsight-plate-solutions)
- [FITS → XISF](#fits--xisf)
- [XISF properties through FITS and ASDF, and back](#xisf-properties-through-fits-and-asdf-and-back)
- [Distributed XISF units](#distributed-xisf-units)
- [Rewriting XISF files (XISF → XISF)](#rewriting-xisf-files-xisf--xisf)
- [Tile-compressed FITS](#tile-compressed-fits)
- [Verifying files](#verifying-files)
- [ASDF output](#asdf-output)
- [ASDF input](#asdf-input)
- [DNG input](#dng-input)
- [TIFF output](#tiff-output)
- [PNG output](#png-output)
- [TIFF and PNG from FITS and ASDF input](#tiff-and-png-from-fits-and-asdf-input)
- [Stretch for viewing](#stretch-for-viewing)
- [Smaller pictures](#smaller-pictures)
- [Previews in the file manager](#previews-in-the-file-manager)
- [Sample conversion](#sample-conversion)
- [The library (libxisfconv)](#the-library-libxisfconv)
- [Python](#python)
- [Limitations](#limitations)
- [Verified against PixInsight](#verified-against-pixinsight)

## Examples

```
xisfconv M31_integration.xisf                 # -> M31_integration.fits
xisfconv -c M31_integration.xisf              # -> M31_integration.fits.fz (tile-compressed, lossless)
xisfconv -c light_0001.fits                   # -> light_0001.xisf (zstd-compressed)
xisfconv -t asdf M31_integration.xisf         # -> M31_integration.asdf
xisfconv observation.asdf                     # -> observation.xisf
xisfconv IMG_0001.dng                         # -> IMG_0001.xisf: the camera's raw image, not demosaiced
xisfconv -t fits raw/                         # every DNG (and XISF, ASDF) file below raw/ -> FITS
xisfconv -t tiff -c -b u16 *.xisf -d export/  # batch to 16-bit Deflate TIFFs
xisfconv -t fits lights/                      # every XISF file below lights/ -> a FITS file next to it
xisfconv -t fits lights/ --skip-existing      # ... and later again: what was added since
xisfconv -t png -s -b u8 --resize 1024 lights/ -d previews/   # previews of a whole tree, in the same folders below previews/
xisfconv -t tiff -s -b u8 integration.xisf     # stretched 8-bit TIFF for GIMP
xisfconv -t png -s -b u8 integration.xisf      # stretched 8-bit PNG for the web
xisfconv -t png -s -b u8 light_0001.fits       # quick look at a raw FITS frame
xisfconv -t png -s -b u8 --resize 1024 *.xisf  # previews, the longest side 1024 pixels
xisfconv -c --in-place *.xisf                 # recompress XISF files with zstd, replacing them
xisfconv -c --in-place archive/               # ... every XISF file below archive/
xisfconv -t xish light_0001.xisf              # -> light_0001.xish + light_0001.xisb (a distributed unit)
xisfconv light_0001.xish -t xisf              # ... and packed into one file again
xisfconv --verify ~/astro/2026                # check every XISF, FITS, ASDF and DNG file below a folder
xisfconv --info light_0001.xisf               # geometry, codecs, FITS keywords, properties
```

## Options

```
xisfconv [options] <file or directory>...   # any of XISF, FITS, ASDF -> any other of them, or TIFF/PNG
                                            # DNG -> any of XISF, FITS, ASDF, TIFF, PNG
                                            # a directory: the image files in it and below it
                                            # an argument with * or ? that names no file: a pattern

  -t, --to <fits|asdf|tiff|png|xisf|xish>
                              output format (default: fits for XISF input, xisf for FITS, ASDF, DNG)
                              xish: XISF as a distributed unit, <name>.xish and <name>.xisb
  -o, --output <file>         output file name (single input only)
  -d, --outdir <dir>          directory for output files (default: next to each input); the files
                              of a directory given as input keep their places below it
  -f, --force                 overwrite existing output files
      --skip-existing         leave an output that exists as it is and pass its input over
      --in-place              XISF -> XISF: replace the input file (after reading the new one back)
  -b, --bits <fmt>            output sample format: u8, u16, u32, f32, f64 (default: as stored)
  -i, --image <n>             convert only image n (0-based); default: all images
  -c, --compress              FITS: tile compression, lossless (image.fits.fz): RICE_1, GZIP_2 for floats
                              TIFF: Deflate with predictor; XISF: zstd + byte shuffling; ASDF: zlib
                              XISF -> XISF: every attached data block
  -s, --stretch[=mode]        screen stretch for viewing: auto (default), linked, unlinked, stf
      --bin <n>               TIFF and PNG: a smaller picture, n x n pixels averaged into one
      --resize <size>         TIFF and PNG: a smaller picture: 256 (the longest side), 1024x768 (a box
                              to fit) or 50%; never larger than the image; made before a stretch
      --top-down              from XISF and DNG: keep the top-down row order in FITS/ASDF (default: bottom-up)
                              from FITS/ASDF: the rows are stored top-down
      --bottom-up             from FITS/ASDF: the rows are stored bottom-up, whatever ROWORDER says
      --no-property-keywords  from XISF: don't derive missing keywords from XISF properties
      --no-properties         from XISF: don't take the XISF properties along to FITS and ASDF
                              from FITS/ASDF: leave the XISF properties a file carries where they are
      --no-wcs                from XISF: don't write WCS from a PixInsight astrometric solution
                              to XISF: don't write PixInsight solution properties from WCS
      --sip-order <n>         from XISF: SIP distortion order (2-7, default 3; 0 = linear only)
      --no-verify             don't verify data block checksums
      --external-files <header-dir|anywhere|none>
                              XISF input: which files the header of a distributed unit may name for
                              its data: those in its own directory and below (default), any file
                              of this machine, or none
      --codec <zlib|zstd|lz4|lz4hc|none>
                              XISF and ASDF output: compression codec (a codec implies -c; lz4 and
                              lz4hc are for XISF only); none = uncompressed (XISF -> XISF: decompress)
                              FITS output: zlib = gzip tiles for every sample type; no zstd or lz4
      --checksum <sha1|sha256|sha512|sha3-256|sha3-512|none>
                              XISF output: checksum of the pixel data;
                              XISF -> XISF: of every attached block (none removes them)
      --bounds <lo:hi>        from FITS/ASDF: range of floating point data (XISF bounds; black:white
                              for TIFF and PNG)
      --verify                check the files, and the image files in the directories, given;
                              converts nothing; exit status 1 if a file is damaged
  -I, --info                  print image geometry, keywords and properties; no conversion
      --dump-header           print the raw XML header (XISF), all keywords (FITS, DNG) or the YAML tree (ASDF)
  -q, --quiet                 suppress warnings
```

Output is written to `<name>.part` and renamed when complete, so an interrupted run never leaves a
half-written file under the final name. A file that `--force` replaces stays until the new one has
its name: where one file cannot be renamed over another, the old one is set aside as
`<name>.replaced` for that moment, and put back if the new one cannot take its place. A `<name>.part` that already exists (the leftover of an
interrupted run, or another file) is not overwritten unless `--force` is given, and never when it
is the input itself. An output name that is a directory or a device (`/dev/null`) is refused, with
or without `--force`: the output would take its place. With several inputs, a failing file is
reported and the rest are still converted (exit status 1).

A run writes no file twice and none over a file it reads. Of two inputs that would get one output
name (`a/frame.fits` and `b/frame.fits` into one directory, or `frame.xisf` and `frame.fits` to
`frame.png`) the first is converted and the second is an error, with or without `--force`: the
second would otherwise replace the first without a word. And an input whose output is another
input of the same run (`xisfconv frame.xisf frame.fits`, each to the other) is an error as well.
Up to 0.16 `--force` did both.

## Directories and patterns

Whole folders, and `*.xisf` on Windows.

- **A directory stands for the image files in it and below it**: the files named `.xisf`, `.xish`
  (the header of a distributed unit; its `.xisb` belongs to it), `.fits`, `.fit`, `.fts`,
  `.fits.fz`, `.asdf` and `.dng`, in the order of their names (and of any case: `.DNG`, `.FITS`).
  Each is converted as it would be if it were named alone, with the same output.
- **Of a directory, the files that are not yet what is asked for are converted**; a file that is
  named on the command line is converted whatever it is. `-t` says what is made, and with it a
  file of that kind is passed over:

  | | converts | passes over |
  |---|---|---|
  | `-t fits` | XISF, ASDF, DNG, and tile-compressed FITS (unpacked) | `.fits`, `.fit`, `.fts` |
  | `-t fits -c` | XISF, ASDF, DNG, and plain FITS (packed) | `.fits.fz` |
  | `-t xisf` | FITS, ASDF, DNG, and distributed units (packed into one file) | `.xisf` |
  | `-t xish` | FITS, ASDF, DNG, and monolithic XISF files (unpacked) | `.xish` |
  | `-t asdf` | XISF, FITS and DNG | `.asdf` |
  | `-t tiff`, `-t png` | every image file | |

  That is why `xisfconv -t fits lights/` can be run on a folder that already holds FITS frames from
  the camera: they are passed over, where each of them, named alone, would be an error (a FITS
  file is not converted to FITS; it can only be packed or unpacked). A note says how many files
  were passed over and why. What a file is goes by its name here; the conversion itself reads the
  file and goes by what it finds.
- **With `--force`, a file that is passed over can still be replaced**: by the output of another
  file of the folder that has its name. `frame.xisf` with `-t fits -f` replaces the `frame.fits`
  beside it, which xisfconv cannot tell from the camera's own `frame.fits`. Without `--force` it
  is not replaced.
- **Without `-t`**, a directory of XISF files is converted to FITS and a directory of FITS, ASDF
  and DNG files to XISF, as a file of each is. A directory that holds both is not guessed at: nothing
  is read, the message says how many of each there are, and the exit status is 2. (Converting
  each to the other would double a folder of raw frames.)
- **Outputs are written next to their inputs.** With `-d`, the files of a directory keep their
  places below it: `xisfconv -t fits lights/ -d export/` writes `lights/night1/a.xisf` to
  `export/night1/a.fits`, and makes `export/night1` if it is not there (and takes it away again
  if the file fails and it stays empty). The directory given with `-d` is not searched for inputs
  where it lies inside a directory that is converted, or where a pattern matches it (`-d out *`);
  named as an input, it is one. Files that are named, or matched by a pattern, are written into
  `-d` itself, as before.
- **`--skip-existing`** leaves an output that is there already as it is and passes its input
  over, without an error: a run on a directory then converts what was added since the last one.
  It goes by the name alone and does not compare dates: an input that changed after it was
  converted is not converted again (`--force` replaces every output). Without either option an
  output that exists is an error, as for a single file, and the last line of the run says so. A
  `.part` file that an interrupted run left behind is in the way of its output: delete it. A
  link that leads nowhere, under the name of an output, is something that is there: it is not
  replaced without `--force`.
- **`--in-place`** on a directory rewrites its XISF files and distributed units, each where it
  is, and passes the rest over. `--info` and `--dump-header` print every image file of a
  directory. `--verify` has taken directories since 0.8; it now takes patterns as well, and a
  directory whose listing breaks off is a failure there too.
- **What is passed over in a directory without being counted**: files of other names; files and
  directories whose names begin with a dot (`.Trash`, and the `._frame.xisf` that macOS leaves on
  disks of other file systems, which are not images); links to directories, which are not
  followed (a link to a file is that file). `--verify` does look at files with a dot: it is there
  to find what is wrong.
- **A run on a directory ends with its counts**: `12 files converted, 3 passed over, 0 failed`.
  The exit status is 1 if a file failed or a directory could not be read (also when its listing
  breaks off half way), and the others are converted all the same. A file that is given twice (a
  directory and a file in it, `a.xisf ./a.xisf`) is converted once, where its first mention puts
  the output: `xisfconv lights/n1/a.xisf lights -d out` writes `out/a.fits`, the other order
  `out/n1/a.fits`. A link to a file is a name of its own and has an output of its own.
- **A pattern**: an argument with `*` or `?` that is not the name of something that is there
  stands for the names it matches, in their order. `*` is any characters, also none; `?` is
  exactly one character; every other character is itself. The shells of Unix do this before the
  program starts; cmd and PowerShell do not, so the program does it, and it does it on every
  system. (On Unix that is for a pattern in quotes, `xisfconv 'lights/*.xisf'`, which helps where
  a folder has more files than a command line takes.) The wildcards may be in any part of the
  path, `night*/lights/*.xisf`. A pattern that matches a directory stands for that directory; one
  that ends in a separator (`night*/`) matches directories only. A pattern stands for files and
  directories, not for a pipe or a device. It brings along every file it matches, as the shells
  do: `*` also matches `notes.txt` and the pictures of an earlier run, and each of them is an
  error of its own (it does not keep the image it was made from from being converted). A pattern
  that matches nothing is an error, and the other inputs are still converted.
  - Brackets are not special: `M31 [Ha].xisf` is a name on Windows, and its shells give `[` no
    meaning.
  - A name that begins with a dot is matched only by a pattern that begins with a dot, as in the
    shells of Unix.
  - Letters match as the system compares names on Windows, whatever their case; elsewhere as they
    are written, character by character. (macOS may store a letter with an accent as two
    characters; a pattern with that letter written as one does not match it there.)
  - On Windows the long form of a path (`\\?\C:\...`) and the name of a share
    (`\\server\share\...`) are taken as they stand, and `xisfconv /?` shows the help.
  - On Unix a file may be called `what?.xisf`. If there is such a file, the argument is that file.

## Reading XISF

XISF 1.0: monolithic files and distributed units.

- Sample formats UInt8/16/32/64, Float32/64; Gray, RGB (and extra/alpha channels)
- Planar and Normal (interleaved) pixel storage, little- and big-endian data
- Compression: zlib, LZ4, LZ4HC (the library's own decoder and, for writing, compressor), Zstandard
  (via libzstd), each with or without byte shuffling, including compressed **subblocks**
- Data blocks as attachments, `inline:base64`/`inline:hex`, or `embedded` `<Data>` elements, and
  in other files (`path(...)`): see "Distributed XISF units" below
- Checksum verification: SHA-1, SHA-256, SHA-512, SHA3-256 and SHA3-512
- FITS keywords, XISF properties of every type (scalars, strings, time points, vectors and
  matrices, complex numbers included), ColorFilterArray, Resolution, ICC profile, multiple images

## FITS output

- BITPIX 8/16/32/64/-32/-64 with the standard BZERO offsets for unsigned data
- All original FITS keywords carried over; structural keywords (SIMPLE, BITPIX, NAXISn, BZERO, ...) are
  regenerated, long names use HIERARCH, long strings are split over CONTINUE cards
- A keyword value that is text without quotes (`Ha` for `'Ha'`) is written in quotes, so that every
  card is valid FITS; a card that cannot be written (a `=` in a long name, no room for the value) is
  left out with a warning
- Missing keywords filled from XISF properties: OBJECT, EXPTIME, DATE-OBS, TELESCOP, INSTRUME, FILTER,
  CCD-TEMP, XPIXSZ/YPIXSZ, FOCALLEN, APTDIA, IMAGETYP, and BAYERPAT from the CFA element
  (disable with `--no-property-keywords`; existing keywords always win)
- Additional images become IMAGE extensions (EXTNAME = XISF image id)
- `-c` writes the images tile-compressed and without loss (`image.fits.fz`, the format of fpack):
  see "Tile-compressed FITS" below
- Row order: XISF stores rows top-down, FITS viewers expect the first row at the bottom. By default
  rows are flipped to the FITS convention (`ROWORDER = 'BOTTOM-UP'`) and BAYERPAT is adjusted to
  match, so the image shows the same way up as in PixInsight. `--top-down` keeps XISF's order and
  writes `ROWORDER = 'TOP-DOWN'` for readers that honor that keyword.
- Numeric keyword values with a lower-case exponent (PixInsight writes `1.7870e+04`) are written
  with the upper-case `E` the FITS standard requires.

## Astrometry (WCS) from PixInsight plate solutions

- Images solved with PixInsight's ImageSolver carry the solution as `PCL:AstrometricSolution:*`
  properties rather than FITS keywords, and PixInsight's own FITS export (1.9.3) writes no WCS
  keywords at all, only the approximate center as `RA`/`DEC`. xisfconv turns it into standard WCS keywords (`CTYPE`,
  `CRVAL`, `CRPIX`, `CD`, `RADESYS`) that match the row order written (bottom-up or `--top-down`).
- PixInsight models field distortion with a thin-plate spline. xisfconv fits SIP distortion
  polynomials (order 3 by default, `--sip-order 2..7`, `0` = linear only) to the spline's point grid,
  including the inverse (AP/BP) terms, and prints the fit quality against the matched stars.
- Projections: Gnomonic (TAN), Stereographic, Plate carrée, Mercator, Hammer-Aitoff, zenithal
  equal-area/equidistant, orthographic. Existing WCS keywords always win; `--no-wcs` turns this off.

## FITS → XISF

A FITS input is converted to XISF automatically.

- Reads the primary HDU and IMAGE extensions: BITPIX 8/16/32/64/-32/-64, 2-D images and 3-D cubes
  (channels). Each image HDU becomes an XISF image; tables are skipped with a warning.
- Reads **tile-compressed images** (`.fits.fz`, as written by fpack, CFITSIO and astropy) directly,
  without funpack: see below.
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
- Output is a monolithic XISF 1.0 file, or under a name that ends in `.xish` (`-t xish`) a
  distributed unit: see "Distributed XISF units" below. `-c` compresses with Zstandard + byte shuffling (the same
  settings PixInsight uses; `--codec zlib` for zlib), blocks over 1 GiB are written as subblocks, and
  `--checksum sha1|sha256|sha512` adds an integrity checksum (`sha3-256` and `sha3-512` are also
  written, but PixInsight does not open such files: see below).

## XISF properties through FITS and ASDF, and back

- PixInsight keeps much of what it knows about an image outside the FITS keywords, in XISF
  properties: the processing history, the instrument and the observation, the astrometric solution
  with its splines. FITS and ASDF have no place for them, so xisfconv takes them along: every
  property of every image and of the file, with its type, its exact value, its comment and format.
  Converted to XISF again, the file gives them back.
- **FITS**: a binary table extension named `XISF_PROPERTIES` behind the image it belongs to, and
  `XISF_METADATA` at the end for the properties of the file. A row per property, with the columns
  `ID`, `TYPE` (the XISF type name), `BLOCK`, `ROWS`, `COLUMNS`, `VALUE`, `COMMENT` and `FORMAT`.
  `VALUE` is an array of bytes: UTF-8 text, and for vectors and matrices their elements as
  little-endian numbers, row after row, with the shape in `ROWS` and `COLUMNS`. `BLOCK` says
  whether XISF keeps the value in a data block (vectors, matrices, and the texts PixInsight
  stores that way) or as text in its header. It is plain FITS: fitsverify accepts it, fpack and
  funpack keep it, a program that knows nothing of it passes over one more extension.
  (HIERARCH keywords cannot do this. The plate-solved test frame has 77 properties, identifiers
  of up to 113 characters among them, and 9 MB of spline data.)

  ```python
  from astropy.io import fits
  import numpy as np
  with fits.open("image.fits") as hdul:
      for row in hdul["XISF_PROPERTIES"].data:
          value = bytes(np.asarray(row["VALUE"], np.uint8))
          if row["TYPE"] == "F64Matrix":
              print(row["ID"], np.frombuffer(value, "<f8").reshape(row["ROWS"], row["COLUMNS"]))
          elif row["TYPE"] == "String" or not row["BLOCK"]:
              print(row["ID"], row["TYPE"], value.decode())
  ```
- **ASDF**: in the tree, under the key `xisf`: `images` has an entry per HDU of `fits` with the
  `properties` of that image, `metadata` holds those of the file. A property is
  `id: {type, value, comment, format}`. The value is a YAML scalar of its kind (true, 42, 1.5,
  "text", a complex number) and for vectors and matrices an array of their element type and shape
  in a binary block. A String that XISF keeps in a data block has `block: true`.

  ```python
  import asdf
  with asdf.open("image.asdf") as af:
      properties = af["xisf"]["images"][0]["properties"]
      focal = properties["Instrument:Telescope:FocalLength"]["value"]              # a float
      matrix = properties["PCL:AstrometricSolution:LinearTransformationMatrix"]["value"]   # a 2x2 array
  ```
- **Back to XISF** they are the properties of the image again, in their order. From FITS every
  value is the text or the bytes it was, and it is stored where it was: a text in the header as
  text in the header, a data block as a data block. (With `-c --checksum sha1`, the long spline
  serializations of the test frame as PixInsight saved it with Zstandard come out as the same
  compressed bytes, with the same checksum.) In an ASDF tree numbers are numbers: their value is the same, their text may
  be written another way (`1e-05` comes back as `1.0e-05`, `True` as `true`), and a file that the
  asdf library wrote again has its properties sorted by id.
- **The astrometric solution comes back as PixInsight wrote it**, splines included, number for
  number. That holds as long as the WCS keywords of the file, the size of the image and the order of
  its rows are what they were when the file was written: a digest of them is stored with the
  properties (`WCSDIGST`). If another program changed them (a new plate solution, a crop, rows
  stored in the other order), the solution that was carried is left out and PixInsight's solution
  properties are made from the keywords, as for any FITS file; the other properties are still
  restored. A note says which of the two happened. Numbers written another way, cards in another
  order and keywords that are not about the WCS change nothing. A program that turns or mirrors
  the pixels and leaves the WCS keywords as they were cannot be noticed: its FITS file says
  the wrong thing already.
- From FITS to ASDF and back, and from FITS to FITS (`-c` to pack, `-t fits` to unpack), the
  properties go along as they are. `--info` lists them for FITS and ASDF files as it does for XISF.
- The properties of the file that describe that one XISF file (`XISF:CreationTime`,
  `XISF:CreatorApplication`, `XISF:CreatorModule`, `XISF:CreatorOS`, `XISF:BlockAlignmentSize`,
  `XISF:MaxInlineBlockSize`, `XISF:CompressionCodecs`, `XISF:CompressionLevel`) are not taken
  along: the next XISF file has its own.
- `--no-properties` turns it off in both directions: XISF → FITS and ASDF writes the images alone,
  and from FITS and ASDF the properties a file carries are left where they are.
- For a program that reads these tables and trees itself, or writes them for xisfconv to read,
  [`docs/xisf-properties-in-fits-and-asdf.md`](docs/xisf-properties-in-fits-and-asdf.md) has the
  layout in full: every column and key, the types, what a table needs to be accepted, and what
  goes into the digest, with [a program](examples/wcs_digest.py) that computes it without xisfconv.

## Distributed XISF units

The files `.xish` and `.xisb`; the options `-t xish` and `--external-files`.

- An XISF unit is one file, the monolithic `.xisf`, or it is distributed: a **header file**
  (`.xish`), which is the XML header and nothing else, and the files that header names, where the
  data blocks are. Those are **XISF data blocks files** (`.xisb`: many blocks behind an index, each
  found by a 64-bit identifier) or any other files, each of which is one block. The header says
  where: `location="path(@header_dir/frame.xisb):0x4d373e33756e480f"`.
- **Only the header file is given**, wherever an XISF file is: `xisfconv frame.xish` converts the
  unit, `--info`, `--verify` and `--dump-header` take it, a directory given to `--verify` stands
  for its `.xish` files too. The other files are found through the header. A `.xisb` file is no
  input: the error names the header file.
- **Writing.** The kind of unit follows the name of the output: `.xish` is a header file, with
  every block that is not in the header in the file of the same name that ends in `.xisb`; any
  other name is a monolithic file. `-t xish` gives those names (`light.fits` → `light.xish` and
  `light.xisb`); `-o frame.xish` does the same. (`-t xisf -o frame.xish` and `-t xish -o
  frame.xisf` say two things, and are errors.) Compression, checksums and subblocks are what
  they are in a monolithic file. The blocks get random identifiers, so that a header never finds
  its pixels in the data blocks file written for another one; the index is one node behind the
  signature, and the blocks are aligned to 4096 bytes (in a unit that is rewritten from another
  XISF file the uncompressed ones are, as in a monolithic file). Both files are written as
  `<name>.part` and renamed when they are complete, the data blocks file first and the header
  last. An existing file of either name is overwritten only with `--force`; the data blocks file
  that is there is then set aside as `<name>.xisb.replaced` until both new files have their
  names, and put back if one of them cannot get its name, so that the unit that was there is
  there still. If a run is stopped in the middle (a power cut, `kill -9`), a file of that name may
  be left. Look at `xisfconv --verify <name>.xish` then: if it fails, its message names the file
  that has the blocks of this header, and renaming that file to `<name>.xisb` gives the unit as
  it was; if the verdict is `OK`, the replacement was complete and the file is a leftover that
  can be deleted. (The name is `.replaced1`, `.replaced2` and so on if `.replaced` is taken, and
  for a data blocks file that is a symbolic link the file is beside what the link leads to.) The name of the
  data blocks file is written into the header, so it has to be valid UTF-8; `&`, quotes and
  parentheses in it are written the way XML and the specification ask.
- **Packing and unpacking** is a rewrite (see below): `xisfconv frame.xish -t xisf` packs a unit
  into one file, `xisfconv frame.xisf -t xish` unpacks one, and both leave every block as it is
  stored unless `-c`, `--codec` or `--checksum` ask for something else. Whatever files the input
  has its blocks in (several data blocks files, files that are one block each), all of them end up
  in the output. `--in-place` on a header file replaces the header and the data blocks file of its
  name; other files the header named before stay where they are. Two files cannot be replaced in
  one step: the old data blocks file is set aside (`<name>.xisb.replaced`), the new files take
  their places, and if one of them cannot, the old one is put back and the unit is as it was.
  A data blocks file that is a symbolic link the header is followed through is treated as the
  header is: the file is replaced, the link stays. A header that is itself a link from another
  directory is not rewritten in place under the link's name (its data is looked for beside the
  link, and would be written beside the file): name the file. A data blocks file may hold
  the blocks of several headers: one that holds blocks this header does not name is not replaced
  in place without `--force`, since those blocks would be gone; one that holds none of the
  blocks this header names, or that the header is not followed to (`--external-files`), is not
  replaced at all; and one that cannot be read as a data blocks file needs `--force` as well.
  (A second header that names the *same* blocks cannot be seen, and is left without them: give
  each unit its own data blocks file.) The files a unit reads are never an output or a temporary file of a run that reads
  them.
- **Which files a header is followed to.** A header is data that came from somewhere, and it says
  which files are read: one that names `/etc/passwd`, or a file of another user, as the pixels of
  an image would have a conversion copy that file into its output. So by default a header is
  followed only to files **in its own directory and below it**, named `path(@header_dir/...)`; a
  path that leaves the directory (`..`, a symbolic link that leads out), an absolute path and a
  `file:` URL are refused, and the message says how to allow them:
  `--external-files anywhere`. `--external-files none` opens no file but the header. And only a
  **header file that is named as one** (`.xish`) is followed at all: a monolithic `.xisf` file
  holds all of its data by the specification, so one that names the file beside it (what
  somebody was sent as "an image", or what a thumbnailer finds in a download folder) is not
  followed there, and neither is an XML header under another name. A block that
  is not read for one of these reasons is an error where it is the pixels of an image, a warning
  where it is one property of many, and "not checked" for `--verify`. **Nothing is ever fetched
  from a network**: a block at an `http:` or `ftp:` URL is reported as not supported. Only regular
  files are read (no devices, no pipes). Where a symbolic link leads, and whether there is
  something, is not told in the refusal. What a header declares for its properties is held
  against the bytes the unit brings (the header, and the blocks it names in data blocks files),
  not against the size of whatever large file it names. These rules are for files from people
  you do not know; they are not a sandbox, and do not hold against somebody who changes the
  directory while a file is read.
- **Reading is lenient where the specification is strict about writers**: an identifier may be
  decimal or hexadecimal, an index may have several nodes and free elements, and reserved fields
  that are not zero are named and passed over. (A monolithic file that names a block in another
  file is read with `--external-files anywhere`, and a warning.) What cannot be right is an
  error: an index that runs in a circle, leads beyond the file or has nodes that lie in each
  other, a block that lies beyond the end of its file, a header that
  asks for an identifier the file does not have ("is it the file that was written with this
  header?"), a header file with an attached block. `--verify` reports what is wrong with an index
  as a failure even where reading goes on.
- `--info` shows the unit: `XISF 1.0, distributed unit, 71303168 bytes in 2 files, header 9210
  bytes, 1 image(s)` and a `data in:` line for each file the header names, which says so if the
  file is not there or is not read.
- **PixInsight reads and writes monolithic files only** (1.9.3): a distributed unit is for other
  software, and is packed into one file for PixInsight. What xisfconv writes is read by
  [OpenXISF](https://github.com/openxisf/openxisf), and what OpenXISF writes is read by xisfconv
  (see "Verified" below; OpenXISF 0.5.0 does not take the backslash off a parenthesis in a file
  name, which the specification puts there, so keep parentheses out of the names of units it
  has to read).

## Rewriting XISF files (XISF → XISF)

Another compression, checksums, one image of several, the other kind of unit: `-t xisf`, `-t xish`,
`-o name.xisf` or `--in-place`.

- Rewrites a file with its data blocks stored another way (those that are attached to it, or in the
  other files of a distributed unit), for example to shrink an archive
  of uncompressed files: `-c` compresses every such block with Zstandard and byte shuffling
  (`--codec zlib` for zlib, `--codec none` to store everything uncompressed). On the uncompressed
  71 MiB test frame from PixInsight that gives 52 MiB, the size PixInsight's own zstd files have.
- `--checksum sha1|sha256|sha512|sha3-256|sha3-512` adds a checksum to every attached block
  (replacing others); `--checksum none` removes them. Without the option, checksums the file has are
  kept, and computed again with the same algorithm for blocks whose stored bytes change.
- **Use `sha1`, `sha256` or `sha512` for files PixInsight has to read.** SHA3-256 and SHA3-512 are
  part of the XISF 1.0 specification, but PixInsight 1.9.3 does not implement them and refuses the
  whole image ("Unknown/unsupported checksum algorithm"). xisfconv warns when it writes one. A file
  that has one is repaired with `xisfconv --checksum sha256 --in-place file.xisf`.
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
- All codecs PixInsight writes are read (zlib, LZ4, LZ4HC, Zstandard, with subblocks) and written:
  `-c` uses Zstandard, `--codec zlib|zstd|lz4|lz4hc` names one (LZ4 and LZ4HC since 0.15, with a
  compressor of the library's own whose blocks the lz4 library decodes). Tested on PixInsight 1.9.3
  files in each of those codecs, Float32, Float64
  and UInt32, with SHA-1/256/512 checksums: every block of every rewritten file decodes to the
  original bytes.

## Tile-compressed FITS

Files named like `image.fits.fz`.

- Images stored with the FITS tiled image compression convention are decompressed on reading and
  then treated like any other FITS image: to XISF (default), ASDF, TIFF or PNG. `-t fits` writes
  them as a plain FITS file, which is what funpack does; `image.fits.fz` gives `image.xisf`,
  `image.fits` and so on.
- Algorithms: `RICE_1` (fpack's default), `GZIP_1`, `GZIP_2`, `PLIO_1` and `NOCOMPRESS`, for all
  BITPIX values, any tile shape, 2-D images and cubes. `HCOMPRESS_1` is not implemented: such an
  image is skipped with a message (funpack can decompress it).
- Integer images are lossless. Floating point images are stored either losslessly (gzip, `fpack -g
  -q 0`) or **quantized** to integers with a scale per tile, which is fpack's default for floats and
  is lossy: xisfconv restores the values CFITSIO and astropy restore (`NO_DITHER`,
  `SUBTRACTIVE_DITHER_1` and `_2`, with the same random sequence; undefined pixels come back as
  NaN), but those are not the values of the image before it was packed. "The same values" holds
  bit for bit on x86-64. On arm64 (Apple Silicon) the last digits of some values can differ from
  what CFITSIO or astropy give there, by far less than the quantization step: a value is
  restored as integer × scale + zero, and C compilers for arm64 fuse the multiplication and the
  addition into one instruction with a single rounding, unless told not to. xisfconv is built to
  round each step, so that it gives the same values on every machine.
- The image's own keywords are carried over; the keywords that describe the table and the
  compression (`ZIMAGE`, `ZCMPTYPE`, `ZTILEn`, `TFORMn`, ...) are dropped, as is the table name
  `COMPRESSED_IMAGE`. `--info` shows the algorithm.
- **Writing**: `-c` (or an output name that ends in `.fz`) writes FITS output tile-compressed,
  from XISF, ASDF and FITS input alike; without `-o` the file is named `image.fits.fz`, as fpack
  names it. The compression is **lossless**: `RICE_1` for integers of 8, 16 and 32 bits, `GZIP_2`
  for floating point, where every bit of every value comes back (NaN and infinities included).
  `--codec zlib` uses gzip for integers as well (`GZIP_2`; `GZIP_1` for 8-bit data, where the two
  are the same). A tile is one row of the image, the default
  of fpack and astropy, and the file is laid out as fpack lays it out: an empty primary HDU, then
  each image as a binary table that says where it belongs (`ZSIMPLE`, `ZTENSION`).
- The Rice-coded tiles are byte for byte those CFITSIO and astropy produce, and funpack
  restores from the file exactly the plain FITS file xisfconv writes without `-c` (plus its own
  `CHECKSUM` cards). A 16-bit camera frame becomes a little more than half its size, as with
  fpack; floating point data, whose low bits are noise, shrinks by a quarter or so.
- What is not written: quantized (lossy) floating point, which is fpack's default for floats and
  much smaller, and `HCOMPRESS_1`. Images of 64-bit integers stay uncompressed in the file, with
  a warning: CFITSIO neither writes nor reads them tile-compressed. Keywords that describe a
  compressed image and its table (`TFORMn`, `ZCMPTYPE`, `ZSCALE`, ...) are the writer's: an image
  that brings its own loses them in a compressed file, with a warning.
- Up to 0.11, `-c` and `--codec` had no effect on FITS output. Now `xisfconv -c image.xisf`
  writes `image.fits.fz` where it wrote `image.fits`, a name that ends in `.fz` is written
  tile-compressed whatever `--codec` says, and `--codec zstd` with FITS output is an error:
  FITS has no Zstandard.
- `xisfconv -t fits -c image.fits` packs a plain FITS file, and `-t fits` unpacks one. Both are
  conversions, not copies: xisfconv writes the images as it reads them (see "FITS → XISF" above for
  how signed integers are mapped: with negative values they become floating point), adds a
  `HISTORY` card and leaves tables out. To pack a FITS file exactly as it is, use fpack.

## Verifying files

The option `--verify <file or directory>...`

- Reads every file completely without converting anything and says whether it is intact. A
  directory stands for the `.xisf`, `.fits`/`.fit`/`.fts`, `.fits.fz`, `.asdf` and `.dng` files in
  it and below it.
- XISF: every data block (pixels, properties, ICC profile, thumbnail; attached, inline, embedded or
  in another file of a distributed unit) has its checksum verified where it has one, is decompressed, and for images compared with the
  size the geometry requires.
- FITS: the structure of every HDU is checked, and the `CHECKSUM` and `DATASUM` keywords where the
  file has them (most capture programs do not write them; astropy and CFITSIO can). Every tile of
  a tile-compressed image is decompressed.
- ASDF: the tree is parsed, and every binary block has its MD5 checksum verified and is decompressed.
- DNG: the raw image is read completely, every strip or tile decompressed. The MD5 digest of the
  raw data that cameras and Adobe DNG Converter store (`NewRawImageDigest`) is not computed (it
  is taken over the data in a form of the DNG SDK's own): such a file is `NOT FULLY CHECKED`, and
  so is one whose raw image is stored in a way xisfconv does not read (lossy DNG, JPEG XL). The
  previews are not read.
- One line per file, `OK` with what was checked or `FAILED` with the reasons; with several files a
  count at the end. The exit status is 1 if any file failed (or a directory could not be read), so
  it can be used in scripts. `-q` prints the failures only. A file with a part xisfconv cannot
  check (a bzip2-compressed ASDF block, a checksum of an unknown kind) is reported as
  `NOT FULLY CHECKED`, with that part named; it does not count as a failure. A file without checksums can still fail (truncated, compressed data that
  does not decompress), but a changed pixel in uncompressed data goes unnoticed: add checksums with
  `xisfconv --checksum sha1 --in-place` to be able to tell later.

## ASDF output

`-t asdf` or `-o name.asdf`, from XISF or FITS.

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

## ASDF input

An ASDF file is converted to XISF by default, or to FITS with `-t fits`.

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

## DNG input

A DNG file (Adobe's Digital Negative, which some cameras write themselves and Adobe DNG Converter
makes of the raw files of every other camera) is converted to XISF by default, or to FITS, ASDF,
TIFF or PNG. What is read is its raw image: the samples of the sensor as the camera recorded them,
for a calibration and stacking program to work on.

- **Nothing is done to the samples but what the file says is part of reading them**: the image is
  cut to the area of the sensor that saw light (`ActiveArea`; the masked border is left out), and
  the file's linearization table is applied where it has one. It is not demosaiced, no black
  level is taken off, no white balance or colour matrix is applied, and the orientation the
  camera was held in is not applied either: calibration frames (darks, flats, bias) must be taken
  the same way, and a demosaicing program wants the mosaic as the sensor has it.
- **The colour filter array**: a mosaic image becomes a grayscale image with its pattern. In XISF
  that is the `ColorFilterArray` element, of any size (a Bayer pattern is 2 x 2, Fujifilm's X-Trans
  6 x 6), as PixInsight writes it for the raw frames it reads itself. In FITS (and in XISF as well)
  a 2 x 2 RGB pattern is also the keyword `BAYERPAT`, which Siril and PixInsight read. A pattern of
  another size or of other colours (X-Trans, CMYG) has no keyword: in FITS and ASDF output it is
  not recorded at all, so convert such files to XISF. A DNG file of a camera that
  stores colour planes (`LinearRaw`: an iPhone's ProRAW, or a file Adobe DNG Converter has
  demosaiced) gives an RGB image.
- **Rows**: DNG rows are top-down, as XISF, TIFF and PNG have them. FITS and ASDF output has them
  bottom-up, the FITS convention, with `BAYERPAT` turned to match (`--top-down` keeps them as they
  are, with `ROWORDER = 'TOP-DOWN'`), as from XISF.
- **Samples**: 16-bit unsigned integers, whatever the camera's precision (10, 12 or 14 bits are
  the most common; the values are not scaled). Samples of more than 16 bits become 32-bit integers.
- **Keywords** from the file's own tags and its EXIF directory: `INSTRUME` (make and model of the
  camera), `DATE-OBS` (the time the exposure began, in UTC when the camera recorded its offset from
  UTC, with fractions of a second; without that offset the camera's clock is written as `DATE-LOC`,
  since its time zone is not known), `EXPTIME`, `ISOSPEED`, `FOCALLEN`, `BAYERPAT`, `BLKLEVEL` (the
  black level; the mean where the file gives one per position of the pattern) and `WHTLEVEL` (the
  largest value a sample can have).
- **Compression**: uncompressed (samples of 1 to 16, 24 and 32 bits, packed as DNG packs them),
  lossless JPEG (as cameras and Adobe DNG Converter write it: the lossless process of the JPEG
  standard, with every predictor, point transforms and restart markers; in tiles or strips), and
  Deflate (with and without the predictors of DNG). Lossy DNG and JPEG XL compression are not read
  (a lossy DNG no longer holds what the sensor recorded); floating point DNG files (HDR merges) are
  not read.
- `--info` shows the raw image, its pattern and keywords, and what else the file holds (previews,
  masks), which are not read. `--verify` reads the raw image completely.
- Nothing is written as DNG.

## TIFF output

- 8/16/32/64-bit unsigned or 32/64-bit IEEE float samples, chunky (interleaved) layout
- Optional Deflate compression (`-c`) with horizontal or floating-point predictor
- ICC profile and resolution copied; extra channels written as ExtraSamples (first one = alpha)
- Multiple images become multiple pages

## PNG output

With `-t png` or `-o name.png`.

- 8- or 16-bit grayscale, gray+alpha, RGB or RGBA; ICC profile (iCCP) and resolution (pHYs) copied.
- Float data is scaled through its bounds to 16-bit; add `--stretch` for linear data.
- PNG holds one image: multi-image files write the first one (or the one chosen with `--image`).

## TIFF and PNG from FITS and ASDF input

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

## Stretch for viewing

The options `-s` and `--stretch[=auto|linked|unlinked|stf]`.

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

## Smaller pictures

`--bin <n>`, `--resize <size>`, for TIFF and PNG output.

- `--bin 2` makes one pixel of every 2 x 2 (`--bin 3` of 3 x 3, and so on): their mean. Columns
  and rows that do not fill a block, at the right and at the bottom, are left out (an image
  narrower or lower than one block counts as one block there).
- `--resize 1024` makes the longest side 1024 pixels; `--resize 1024x768` fits the picture into
  that box, its proportions kept; `--resize 50%` halves width and height. A picture is never
  larger than the image: an image that is small enough already is written as it is.
- Every pixel of the picture is the mean of the part of the image it covers, each pixel of the
  image counted by the share of it that is covered. No pixel is left out or counted twice, so
  nothing shimmers or rings, stars do not vanish between samples, and the noise goes down as it
  would with larger pixels. For whole ratios that is binning. Integers are rounded to the nearest
  value; floating point samples that are not numbers (NaN, Inf) are left out of the mean. (The
  sums are 64-bit floating point, so a mean of 64-bit samples is right to their last bit or two.)
- The picture is made of the image as it is stored, and a `--stretch` is applied to the picture:
  the mean of linear data is what a sensor with larger pixels would have recorded, and the
  auto-STF is computed for the picture that is written. (It is a little deeper than the stretch
  of the full image, because the picture has less noise.) It is also why a preview of a large
  frame takes no longer than reading it.
- With both options the blocks of `--bin` come first, and `--resize` is of the binned image. The
  resolution a TIFF or PNG file states (pixels per inch) follows the size.
- They are for pictures. FITS, ASDF and XISF output keep their pixels, and the options are
  refused there: with a smaller image the WCS, the astrometric solution and the colour filter
  pattern would all have to change with it.

## Previews in the file manager

On Linux.

- `desktop/xisfconv.thumbnailer` tells the file managers that use thumbnailer entries (GNOME
  Files, Nemo, Caja, Thunar, PCManFM) to make their previews of XISF, FITS (also `.fits.fz`) and
  ASDF files with xisfconv: a stretched 8-bit PNG of the first image, as large as the file
  manager asks for. `desktop/xisfconv.xml` teaches the desktop the file types it does not know
  (XISF and ASDF; FITS it knows, and recognizes a `.fits.fz` file by how it begins).
- `sudo cmake --install build` puts both in place (`share/thumbnailers`, `share/mime/packages`);
  then `sudo update-mime-database /usr/local/share/mime`. With a downloaded binary:

  ```
  sudo install -m 755 xisfconv /usr/local/bin/
  mkdir -p ~/.local/share/thumbnailers ~/.local/share/mime/packages
  cp desktop/xisfconv.thumbnailer ~/.local/share/thumbnailers/
  cp desktop/xisfconv.xml ~/.local/share/mime/packages/
  update-mime-database ~/.local/share/mime
  rm -rf ~/.cache/thumbnails/fail        # forget the files that had no preview before
  ```
- The program itself has to be under `/usr` (`/usr/local/bin` is): GNOME runs thumbnailers in a
  sandbox that sees the system and not your home directory, so a copy in `~/bin` or
  `~/.local/bin` makes no previews there.
- The header file of a distributed unit (`.xish`) gets its preview where the thumbnailer may read
  the data blocks file beside it: in Nemo, Caja, Thunar and PCManFM. GNOME's sandbox holds the
  one file it was asked about, so GNOME Files shows no preview of a `.xish` file.
- File managers make no previews of files above a size they set, and astronomical images are
  often larger: raise the limit. Nemo and Caja have it in their preferences; for GNOME Files it
  is a setting, in megabytes: `gsettings set org.gnome.nautilus.preferences thumbnail-limit 4096`.
- KDE's Dolphin makes its previews with plugins of its own and does not read thumbnailer entries.
- The entry is one line, and what it runs can be tried by hand:
  `xisfconv -q -f -t png -s -b u8 --resize 256 -o preview.png image.xisf`. On the 62 MB test
  frame (4656 x 3520, 32-bit floating point) that takes a tenth of a second.

## Sample conversion

The option `-b u8|u16|u32|f32|f64`.

- integer → integer: rescaled over the full range (65535 → 255)
- integer → float: normalized to [0,1]
- float → integer: the XISF `bounds` range (normally [0,1]) maps to the full integer range, clipped
- float → float: values unchanged

## The library (libxisfconv)

Everything the tool does is done by a library with a plain C API, declared in
[`include/xisfconv.h`](include/xisfconv.h). The tool itself uses nothing else. The library reads
XISF, FITS and ASDF images into arrays, writes arrays as XISF, FITS (plain or tile-compressed),
ASDF, TIFF or PNG, converts and
rewrites files, verifies them, translates astrometric solutions and applies PixInsight's screen
stretch.

Its purpose is XISF and the conversion between XISF, FITS and ASDF. FITS and ASDF are supported as
far as images need them: for tables and everything else in those formats, CFITSIO, astropy and
Python's `asdf` are the libraries to use.

```
cmake -S . -B build -DBUILD_SHARED_LIBS=ON
cmake --build build -j
sudo cmake --install build      # libxisfconv, xisfconv.h, xisfconv.pc, the CMake package, the tool
```

```c
#include "xisfconv.h"
/* cc app.c $(pkg-config --cflags --libs xisfconv)      (add --static for the static library)
   or, in CMake:  find_package(xisfconv REQUIRED)
                  target_link_libraries(app PRIVATE xisfconv::xisfconv) */

xisfconv_context *ctx = xisfconv_context_new();
xisfconv_file *file = NULL;
if (xisfconv_open(ctx, "M31.xisf", &file) != XISFCONV_OK) {
    fprintf(stderr, "%s\n", xisfconv_error_message(ctx));
} else {
    xisfconv_read_options ro;
    uint64_t size;
    xisfconv_read_options_init(&ro, sizeof ro);
    ro.sample_format = XISFCONV_SAMPLE_FLOAT32;      /* whatever the file holds */
    xisfconv_pixels_size(file, 0, &ro, &size);
    float *pixels = malloc(size);                    /* [channels][height][width], top row first */
    xisfconv_read_pixels(file, 0, &ro, pixels, size);
    ...
    xisfconv_close(file);
}
xisfconv_convert(ctx, "M31.xisf", "M31.fits", NULL); /* what the tool does, in one call */
xisfconv_context_free(ctx);
```

**The manual of the library.** [`docs/manual.html`](docs/manual.html) is the manual of the library for
C, C++ and Python: how to get and link it, the rules that hold everywhere (failures, pixels, keywords and
properties), a tour of nine short programs in each of the three languages with what they printed
for a real frame, and the complete reference of the C API and of the Python package. It is one
file that needs nothing else: download it and open it in a browser (GitHub shows its source, not
the page). The programs it shows are in [`examples/`](examples): `first.c`, `first.cpp` and
`first.py` say what is in a file, and `tour.c`, `tour.cpp` and `tour.py` go through inspecting a
file, reading pixels, the screen stretch, writing an image with keywords and properties,
converting, rewriting and verifying, distributed units, astrometry, and progress and failures.
There is one interface for C and C++, the header; `tour.cpp` begins with the forty lines that give
a C++ program handles that free themselves and exceptions.
[`examples/example.c`](examples/example.c) is one more complete program, in one function.

What to know:

- **Errors.** Functions return a status (`XISFCONV_OK` is 0); the text of the last failure is kept
  in the context. No exception leaves the library, and it prints nothing: warnings and notes go to
  a message handler, if one is set, or are kept in the context for the caller to fetch
  (`xisfconv_context_keep_messages`). A progress handler can cancel a long call, and so can another
  thread, with `xisfconv_context_cancel`. For a host that calls the library from an interpreter
  there is a second kind of progress handler, with one argument and an answer that tells a
  handler that failed from one that says "go on" (`xisfconv_context_set_host_progress`); the
  Python package uses it to let Ctrl-C through.
- **Pixels** are planar and in host byte order: `[channels, height, width]` for NumPy. The row
  order is always stated: XISF, TIFF and PNG are top-down, FITS is bottom-up unless `ROWORDER` says
  otherwise, and reading and writing take the order the caller wants.
- **Keywords** come as the file has them. WCS keywords describe the rows in the order the image
  info names (`wcs_row_order`): the stored order in FITS and ASDF, always bottom-up in XISF, as
  PixInsight writes them. `xisfconv_wcs_keywords` returns them for any row order, and the writer
  converts them when it stores the rows the other way round. `xisfconv_fits_keywords` gives the
  whole header an image gets in a conversion to FITS, and `xisfconv_keywords_fits_text` any
  keyword list as FITS cards, for handing to another FITS library. The writer leaves out the cards
  that describe how a FITS file stores its data (SIMPLE, BITPIX, NAXIS, BZERO and the like), so a
  header read from a FITS file can be passed as it is.
- **Distributed XISF units** (since 0.16). Every function that takes an XISF file takes the header
  file of a distributed unit (`.xish`); `xisfconv_writer_new`, `xisfconv_convert` and
  `xisfconv_rewrite` write one under a name that ends in `.xish`, with the data blocks in the file
  of the same name that ends in `.xisb`. `xisfconv_external_count` and `xisfconv_external_file`
  list the files a header names, `xisfconv_unit_size` is the size of them all, and
  `xisfconv_file_detail(file, "unit")` says "monolithic" or "distributed". How far a header is
  followed is a setting of the context, `xisfconv_context_set_external_files`: to its own
  directory (the default), anywhere on the machine, or to no other file. A block in a file the
  header is not followed to gives `XISFCONV_ERR_NOT_ALLOWED`, and `xisfconv_external_status` says
  for each file whether it is read. A program that opens files from people it does not know
  should leave the default as it is.
- **Numbers** in files have a decimal point whatever locale the program has set.
- **File names** are UTF-8 on every platform, Windows included.
- **Threads.** There is no global state. A context and the handles made from it belong to one
  thread at a time; different contexts are independent.
- **From other languages.** The structs start with their size and enumerations are 32-bit integers,
  so the header maps directly to Python's `ctypes` or `cffi`, Rust's bindgen and Perl's
  FFI::Platypus. The Python package in `python/` is built that way, on `ctypes`.
- **Stability.** Version 0.x: the API may still change between releases, and the shared library's
  version changes with each of them (`libxisfconv.so.0.16`).
- **Messages** are the tool's and some name its options (`--force`, `--bounds`): the option names
  say which setting is meant.
- The CMake package (`find_package(xisfconv)`) is installed with the shared library; a static
  library comes with the pkg-config file only, to be used with `pkg-config --static`.
- An ICC profile handed to the writer is stored in XISF as an inline `ICCProfile` block. The
  library reads it back; whether PixInsight accepts it has not been checked yet.
- **XISF properties from the caller's values** (since 0.15): a property list
  (`xisfconv_properties_new`, `_set` for scalars, strings and time points, `_set_array` for vectors
  and matrices) is given to an image and to the write options. They are written to XISF as the
  properties of the image and of the file, and to FITS and ASDF the way a conversion takes them
  along. An astrometric solution among them is written as it is given; without one, it is made
  from WCS keywords as before. `xisfconv_properties_set` checks a value against its type;
  `xisfconv_properties_set_as_read` takes a property as a file had it, whatever it says, so that
  a program which reads a file and writes it again loses nothing of it, and
  `xisfconv_property_stored` says how the file has it (a value in the header, a text that is
  kept as data, an array, or something that is not read, like a table). `xisfconv_property_read`
  gives a vector or a matrix in the type of its elements (complex ones too), where
  `xisfconv_property_read_f64` gives doubles. `xisfconv_wcs_digest` tells such a program whether
  the WCS keywords of an image are still those its solution was read with, and the detail
  `carriedSolution` of an image in a FITS or ASDF file says the same of the solution it carries.
- The writer also takes a compression level, byte shuffling on or off, and the name of the program
  that writes the file (`compression_level`, `shuffle` and `creator_application` of
  `xisfconv_write_options`).

The library is licensed under the LGPL (version 3 or later), so that programs under other licences
can use it; the command line tool remains under the GPL.

## Python

The package `xisfconv` is the library with NumPy arrays in and out. [`python/README.md`](python/README.md)
describes it; in short:

```python
import xisfconv

data = xisfconv.read("m31.xisf")                    # [height, width] or [height, width, channels]
image = xisfconv.read_image("m31.xisf")             # with keywords, name, bounds, XISF properties
xisfconv.write("out.xisf", data, keywords={"OBJECT": "M 31"}, codec="zstd", checksum="sha256")
xisfconv.write("out.xisf", data, properties={"Instrument:Telescope:FocalLength": 0.53})   # XISF properties
xisfconv.write("copy.xisf", image)                  # what was read: pixels, keywords and properties
xisfconv.write("out.fits.fz", data)                 # tile-compressed FITS, lossless
xisfconv.convert("m31.xisf", "m31.fits")            # what the command line tool does
print(xisfconv.verify("m31.xisf").verdict)
xisfconv.write("m31.xish", data)                    # a distributed unit: m31.xish and m31.xisb
xisfconv.rewrite("m31.xish", "packed.xisf")         # ... packed into one file (and the reverse)

import xisfconv.astropy                             # CCDData.read("m31.xisf"), ccd.write("x.xisf"),
from astropy.nddata import CCDData                  # and astropy.io.fits HDU lists
ccd = CCDData.read("m31.xisf", unit="adu")

from xisfconv.xisf import XISF                      # the interface of the xisf package
im_data = XISF("m31.xisf").read_image(0)
```

**Coming from the `xisf` package.** `xisfconv.xisf` has the class of that package, with its methods,
its arguments and the structures it returns: `XISF(fname)`, `get_images_metadata()`,
`get_file_metadata()`, `get_metadata_xml()`, `read_image()`, `XISF.read()` and `XISF.write()`. A
program written for it runs with its import line changed to `from xisfconv.xisf import XISF`, and
then has what the library does: checksums are verified, subblocks, big-endian samples, the Normal
pixel storage, 64-bit integers and embedded data are read, properties keep their types, comments
and formats, and files are written under another name and renamed. No code of that package is
used. [`python/README.md`](python/README.md) lists where the two differ on purpose.

```
pip install .                    # from a checkout: builds the library and installs the package
pip install ".[astropy]"         # with astropy
```

The package needs Python 3.10 or later and NumPy; astropy is optional. It holds the shared library
and calls it through `ctypes`, so one wheel per platform serves every Python version. The wheels
for Linux (x86_64, arm64), macOS (Apple Silicon) and Windows (x64) are built by
`.github/workflows/wheels.yml`; Zstandard is linked into them, so they need nothing but the C and
C++ runtime and, on Linux and macOS, the zlib of the system. The package is not on PyPI yet: see
[Releasing](DEVELOPMENT.md#releasing).

Good to know:

- Arrays have row 0 at the top and the channels last, as Pillow, matplotlib and tifffile have them;
  `row_order="bottom-up"` and `channels="first"` give the FITS conventions, and `xisfconv.astropy`
  uses those throughout.
- `sample_format` rescales, as `--bits` does; it does not cast.
- Warnings of the library are Python warnings (`xisfconv.XisfconvWarning`), its notes go to the
  logger `xisfconv`, its errors are exceptions derived from `xisfconv.Error`.
- Ctrl-C stops a conversion, a rewrite or a verification between its steps and leaves no partly
  written file; during the last step it takes effect when the file is complete. (A rewrite and a
  verification have a step per data block; a conversion has one per image while it reads an XISF
  file, and writes its output in one, or with a step every few megabytes when the output is
  tile-compressed FITS.) The same holds for any signal whose handler raises, such as an alarm that sets a
  time limit. A function given as `progress=` is called between the steps, in the caller's
  thread, and stops the work by raising an exception.
- `read_image` reads the XISF properties of an image with their types, and `write` writes them
  (since 0.15; before, they were read and not written): to XISF as the properties they were, to
  FITS and ASDF the way `convert` takes them along. `properties=` and `file_properties=` of `write`
  take a dict of Python values: numbers, strings, `datetime`, NumPy arrays for vectors and
  matrices. Vectors and matrices are read in the type of their elements, complex ones included
  (up to 0.14 as float64). A property that is not changed is written with the text the file
  has for it, and PixInsight's astrometric solution is written as long as the image has the
  size and the WCS keywords the solution was read with. After a crop, or with other keywords,
  it is left out as a whole, and a solution is made from the WCS keywords if the image has
  them; a warning says so if it has none (`properties.solution_of = None` says the solution is
  right as it stands). The saved screen stretch, the resolution and the thumbnail of an XISF
  image are not carried by `read_image` and `write`; `rewrite` copies an XISF file with everything
  in it. A FITS or ASDF file that was converted from XISF shows the properties it carries as
  `file[0].properties` and `file.properties`, like an XISF file.
- A distributed XISF unit is read by the name of its header file (`.xish`) and written under such
  a name (since 0.16); `File.unit`, `File.external_files` and `File.unit_size` describe it. A
  header file (`.xish`) is followed to the files in its own directory; one that names a file
  elsewhere, and a monolithic `.xisf` file that names any other file, raises
  `xisfconv.NotAllowedError` (a `PermissionError`) unless the call says
  `external_files="anywhere"`. Each call says it for itself: there is no setting that stays.
- An image is read and written as a whole, in memory. Reading takes about twice the size of the
  image for a moment, three times for a compressed file. Writing takes once its size on top of
  the array, twice for a colour image with the channels last, and about four times when the
  file is compressed (not for tile-compressed FITS, which is compressed row by row).
- `CCDData.read` hands the image to astropy's own FITS reader as a FITS file in memory, so that
  units, mask and uncertainty behave exactly as with FITS; that takes about four times the size
  of the image in memory.
- The messages of the library are those of the command line tool. In the Python package they
  name its arguments (`overwrite=True`) where the tool's name options (`--force`).

## Limitations

- Distributed XISF units: a block at a network URL (`url(http://...)`) is never fetched. A unit is
  written with one data blocks file, of the header's name; an existing data blocks file is never
  added to or edited, it is written anew (so a data blocks file that several headers share is
  for reading: rewrite each unit under a name of its own). A unit cannot be written under a name
  that is not valid UTF-8. The properties of a file may declare, together, 256 MiB more than
  the bytes the unit brings; a property that is stored as a file of its own (not in a data
  blocks file) counts in full against that, so one of more than 256 MiB is left out with a
  warning. The thumbnailer of the desktop integration shows
  `.xish` files only where thumbnailers may read the files beside the one they are given (GNOME
  runs them in a sandbox that holds the one file).
- Complex sample formats and images with more than two dimensions are skipped.
- CIELab images are written as raw 3-channel data without color conversion.
- TIFF output is classic TIFF (4 GiB limit); BigTIFF is not implemented.
- WCS keywords in an XISF header are taken to follow the FITS bottom-up convention (PixInsight's);
  they are converted when writing top-down FITS. Distortion models other than SIP (TPV, TNX) are
  copied without that conversion.
- FITS → XISF writes the solution properties in the layout PixInsight 1.9.3 uses. For a FITS file
  that was not converted from XISF, other XISF properties (observation time, instrument) are not
  created; PixInsight derives those from the keywords. Distortion other than SIP (TPV, TNX) and
  non-zenithal projections stay keyword-only.
- A PixInsight spline solution that goes XISF → FITS → XISF comes back as the original. If the
  WCS keywords were changed on the way, or the properties were left out (`--no-properties`), it is
  a spline rebuilt from the SIP approximation: on the test frame the two agree to 0.5 arcsec rms.
- XISF → FITS or ASDF → XISF returns the pixels and every property. It does not return the file
  as it was: the saved screen stretch (STF), the resolution, the thumbnail and the ICC profile
  have no place in FITS and ASDF and are not taken along, the image attributes survive as far as
  keywords say them (`IMAGETYP`, `BAYERPAT`, the id as `EXTNAME`), and the keywords gain what the
  conversion wrote: keywords derived from properties, WCS keywords made from a solution, a
  `HISTORY` line for each conversion. (`-t xisf` rewrites an XISF file with everything in it.)
- What does not come back as a property, each with a warning: a property that is built of other
  elements (the tables and structures of the specification), one without an id, one whose id or
  type is not plain ASCII (FITS) or not UTF-8 (ASDF), the second of two properties with the same
  id (ASDF), and what goes beyond the size limit below. A comment, a format or the value of a
  scalar with a character XML has no way to write (a control character) gets a blank in its place.
- A String that is text in the XISF header comes back as text in the header with the same bytes,
  whatever line ends and blanks it has (PixInsight on Windows writes its spline serializations
  with CR LF): whether that reads as CR LF or as LF, and with or without the blanks at its ends,
  is a matter of the reader, for the original and for the result alike. (Up to 0.14 a text with a
  blank at either end or a carriage return on its own came back as a data block, which an XML
  reader does not read as it read the header.) A String comes back as a data block if it was one,
  if its header text has a carriage return written as a character reference (`&#13;`), which
  every reader keeps, or if it was a `value` attribute with a carriage return or with blanks at
  its ends.
- The properties of a file are held in memory together. More than the size of the file plus
  256 MiB is not accepted (a damaged file, or one made to exhaust the memory, could otherwise
  declare any amount): what is beyond is left out with a warning.
- XISF properties of an image that is exported with `--stretch` or `--bits` describe the image
  as it was (the processing history does not mention the stretch).
- Files with XISF properties read by xisfconv 0.12 or older: the tables of a FITS file are
  reported as skipped HDUs, and the matrices in an ASDF tree are taken for images.
- `--bin` and `--resize` make TIFF and PNG pictures only. The previews are made from the pixels;
  a thumbnail that PixInsight stored in an XISF file is not used. The thumbnailer entry has been
  run as a command, not yet inside a file manager.
- FITS input: tables are not read, other than those of the XISF properties; BLANK pixels of
  integer images are kept as ordinary values. Tile-compressed images: `HCOMPRESS_1` is not read.
- Tile-compressed FITS is written without loss only (`RICE_1`, `GZIP_2`), a row per tile: no
  quantized floating point, no `HCOMPRESS_1`, no choice of tile shape. Images of 64-bit integers
  are left uncompressed. Packing a FITS file (`-t fits -c`) converts it, as every other path
  does; fpack copies it.
- ASDF input: arrays stored inline in the tree or in another file, non-contiguous views, Fortran-ordered
  arrays, tables and structured or complex data types are skipped with a message; bzip2- and
  Blosc-compressed blocks are not read. Line breaks written as U+0085, U+2028 or U+2029 inside the
  tree are not recognized as such.
- DNG input: the raw image only, as it was recorded (see [DNG input](#dng-input)): no
  demosaicing, black level, white balance, colour matrix, lens corrections or orientation; the
  camera's opcode lists (`OpcodeList1` to `3`: corrections for bad pixels, lens shading and
  distortion that a raw converter applies) are not applied. Not read: lossy DNG, JPEG XL
  compression, floating point data, BigTIFF, a colour filter layout that is not rectangular
  (`CFALayout` other than 1; Fujifilm's old SuperCCD), a second raw image (the first one is read),
  the `NewRawImageDigest` (not computed by `--verify`). The maker notes are not read.
- ASDF output always uses the FITS HDU list layout described above; it does not write generalized WCS
  (gwcs) objects or instrument-specific data models.
- A compression level and byte shuffling can be chosen when an image is written from memory (the
  library's writer, `xisfconv.write`), not for a conversion or a rewrite: the tool has no option
  for them. LZ4 is written to XISF only (ASDF's own LZ4 layout is read, not written).
- XISF files with SHA3-256 or SHA3-512 checksums are valid but cannot be opened by PixInsight 1.9.3.
- XISF → XISF does not move blocks between the header (inline, embedded) and attachments. Replacing a
  file in place gives it a new inode: other hard links to the old file keep the old content.
- FITS output carries no CHECKSUM/DATASUM keywords yet; `--verify` checks them where a file has them.
- Directories: what a file is goes by its name when a directory is searched, so a tile-compressed
  FITS file that is named `.fits` counts as a plain one, and a file without one of the extensions
  is not found (name it, and it is read whatever it is called). To rewrite XISF files into another
  directory, name them (`lights/*.xisf`): of a directory, the files that already are what is
  written are passed over. The files are converted one after the other. `--skip-existing` does
  not compare dates.
- Patterns: `*` and `?` only; `**` is `*`, and there are no brackets or braces. A file whose name
  has a `*` or `?` in it (Unix) cannot be matched by a pattern that would have to quote it; it
  can be named.
- On Windows, file names are handled as Unicode (the console is switched to UTF-8 while the tool
  runs).
- The PixInsight spline distortion model is approximated by SIP polynomials, not carried over exactly.
- Please report any file that fails to convert, ideally with `xisfconv --info` output.

## Verified against PixInsight

Tested with files saved by PixInsight 1.9.3 (XISF module 1.1.3): Float32, Float64 and UInt32 images
compressed with zlib, LZ4, LZ4HC and Zstandard (all with byte shuffling), and SHA-1/SHA-256/SHA-512
checksums. Every variant decodes bit-identical to the original data and to an independent decoder,
and a corrupted byte is caught by each checksum type. PixInsight 1.9.3 does not open images whose
block has a SHA3-256 or SHA3-512 checksum. Pixel data matches PixInsight's own FITS export
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

The properties of the plate-solved test frame (77 of them: instrument, observation, processing
history, previews, and the astrometric solution with 9 MB of spline grids) go through FITS,
tile-compressed FITS and ASDF and come back identical, as the `xisf` package reads both files:
every value and every array. That was checked with the frame as PixInsight saves it in nine ways
(uncompressed, zlib, LZ4, LZ4HC and Zstandard, with and without checksums). That PixInsight opens
the file that comes back and reports the same solution as for the original has not been checked
yet.

Not checked with PixInsight yet either, both since 0.15: XISF files compressed with LZ4 and LZ4HC
by the library's own compressor (the lz4 library and the `xisf` package decode every block, and
PixInsight reads those codecs from its own files), and properties written from the values of a
program. The same nine files, read and written again through `xisfconv.xisf`, keep every keyword
and every property as PixInsight wrote it: type, comment, format and the text of each value, and
the content of each data block byte for byte (how a block is stored, its codec and checksum and
whether it is attached or in the header, is the writer's). Of two more frames, which PixInsight
saved uncompressed, every keyword and property element of the header comes back as the same bytes.

Distributed units (since 0.16) are not a matter for PixInsight, which reads and writes monolithic
files only. They were checked against the specification, by test programs that take the two files
apart and build them without xisfconv, and against [OpenXISF](https://github.com/openxisf/openxisf)
0.5.0, an independent implementation: its reader reads the units xisfconv writes (uncompressed and
with zlib, LZ4, LZ4HC and Zstandard, one and three channels, and units unpacked from monolithic
files with several images and properties), and xisfconv reads, verifies, packs and unpacks the
unit its writer makes. The test suite runs those checks when `OPENXISF_BIN` names the directory
of OpenXISF's sample programs. The eleven frames PixInsight saved (the plate-solved test frame
in nine codec and checksum variants, and two more) were unpacked into units, which OpenXISF
reads; packed again, each has every data block byte for byte and the same header text as
PixInsight's file, the attributes that say where a block is aside.
