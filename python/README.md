# xisfconv for Python

PixInsight **XISF** images as NumPy arrays, and conversion between **XISF**, **FITS** and
**ASDF**, with TIFF and PNG export; the raw images of **DNG** files (camera raw) are read too. This is the Python package of
[xisfconv](https://github.com/jkobierczynski/xisfconv): the same library as the command line
tool, with nothing else to install.

```
pip install xisfconv            # NumPy is the only dependency
pip install "xisfconv[astropy]" # with astropy, for CCDData and HDUList
```

The [manual of the library](https://github.com/jkobierczynski/xisfconv/blob/main/docs/manual.html)
(`docs/manual.html` in the repository: one file, to be opened in a browser) goes through the package
with programs that ran on a real frame, next to the same programs in C and C++, and has the
reference of every function and class.

## Reading

```python
import xisfconv

data = xisfconv.read("m31.xisf")            # [height, width] or [height, width, channels]

image = xisfconv.read_image("m31.xisf")     # the pixels and what describes them
image.data, image.name, image.bounds
image.keywords["EXPTIME"]                   # FITS keywords
image.properties["Instrument:Telescope:FocalLength"]   # XISF properties

with xisfconv.open("m31.xisf") as f:        # XISF, FITS (also tile-compressed .fits.fz), ASDF or DNG
    print(f.format, len(f))
    for entry in f:
        print(entry.name, entry.shape, entry.dtype, entry.color_space)
    data = f[0].read(row_order="bottom-up", channels="first")   # the FITS conventions
    wcs = f[0].wcs_keywords()               # also from a PixInsight astrometric solution
```

Arrays have **row 0 at the top** of the image and the **channels last**, like the images of
Pillow, matplotlib and tifffile. `row_order="bottom-up"` and `channels="first"` give the FITS
conventions instead. A colour image with the channels last is a view of planar memory;
`numpy.ascontiguousarray` makes it contiguous where that is needed.

A DNG file holds one image to read: the raw image as the sensor recorded it, not demosaiced
(`xisfconv.read("IMG_0001.dng")` gives the mosaic as 16-bit integers, `f[0].cfa` its colour filter
pattern, such as `("RGGB", 2, 2)`, and the keywords hold the camera, exposure and time).

`sample_format` converts the samples while reading: `xisfconv.read(path, sample_format="uint16")`.
This rescales, it does not cast: integers to integers over the full ranges, integers to floating
point normalized to 0..1, floating point to integers with the bounds of the image mapped to the
full integer range.

## Writing

```python
xisfconv.write("plain.xisf", data)                                 # XISF, FITS, ASDF, TIFF or PNG
xisfconv.write("out.xisf", data, keywords={"OBJECT": "M 31", "EXPTIME": (300.0, "seconds")},
               name="M31_L", codec="zstd", checksum="sha256")
xisfconv.write("out.fits", [image1, image2])                       # several images: FITS HDUs
xisfconv.write("out.fits.fz", data)                                # tile-compressed FITS, lossless
xisfconv.write("copy.fits", xisfconv.read_image("m31.xisf"))       # the image with its keywords
xisfconv.write("out.xisf", data, overwrite=True)                   # an existing file is kept otherwise
```

XISF properties are written from Python values, to the image and to the file:

```python
import datetime
import numpy as np

xisfconv.write("out.xisf", data, codec="lz4hc", creator="my script 1.0",
               properties={"Instrument:Telescope:FocalLength": 0.53,            # Float64
                           "Instrument:Camera:Gain": np.float32(120),           # Float32
                           "Observation:Object:Name": "M 31",                   # String
                           "Observation:Time:Start": datetime.datetime.now(datetime.timezone.utc),   # TimePoint
                           "Lab:Flat": np.array([[1.0, 0.5], [0.5, 1.0]])},     # F64Matrix
               file_properties={"Note:Author": "somebody"})

properties = xisfconv.PropertyDict()
properties.set("Instrument:Sensor:XPixelSize", 3.76, type="Float32", comment="micrometres", format="%.2f")
xisfconv.write("typed.xisf", data, properties=properties)
```

The XISF type follows from the value: `bool` is Boolean, `int` Int32 (Int64 or UInt64 if it
does not fit), `float` Float64, `complex` Complex64, `str` String, a `datetime` a TimePoint, a
1-D array the vector and a 2-D array the matrix of its element type (`uint16` gives UI16Vector,
`complex64` C32Matrix); NumPy scalars keep their width and `bytes` are a ByteArray.
`PropertyDict.set` states a type, a comment and a format where that is not what is wanted. The
properties of an image that was read (`read_image(...).properties`) are such a `PropertyDict`
and have what the file states, so `write` writes them as they were: a value that is not touched
is written with the very text the file has, also where that is a type of 128 bits or a value
this library would not write itself. To FITS and ASDF the properties go the way `convert` takes
them along; TIFF and PNG have no place for them. A new text of more than 3072 bytes, with a
carriage return in it or with white space at its ends is stored as a data block, where every
reader finds it as it is (in the header an XML reader makes a line feed of CR LF, and may take
blanks at the ends for layout); a text that was read from a header is written there again with
the bytes it has. Vectors, matrices and texts of more than 3072 bytes are compressed with the
codec of the pixels. A property the library has no value for (a table, a data block of a type
it has no name for) is None, and is not written: a warning says so. `convert` does carry such
a data block.

An astrometric solution among the properties (`PCL:AstrometricSolution:...`) that you give is
written as it is; without one, it is made from WCS keywords (`wcs=False` turns that off). A
solution that was read from a file describes the WCS keywords and the size of that image:
`write` writes it while they are the same. If the image was cropped or the keywords were
changed, the solution is left out, so that the two never contradict each other, and one is made
from the WCS keywords if the image has them. If it has none (PixInsight often keeps the
solution in the properties alone), the file is written without a solution, and a warning says
so. A solution is one thing: if you gave some of its properties new values and left the others
as they were read, all of it is left out with such an image, with a warning; a solution you put
in the place of the one that was read is written as it is. `properties.solution_of = None`
says that the solution is right as it stands.

A `datetime` without a zone and a `numpy.datetime64` are written without one; a `datetime` with
`tzinfo` is written with its offset from UTC.

`codec` is "zlib", "zstd", "lz4" or "lz4hc" for XISF; `level` sets the compression level
(zlib 1 to 9, lz4hc 1 to 12, zstd 1 to 22) and `shuffle=False` turns byte shuffling off; `convert`
and `rewrite` take both as well, for XISF output (since 0.19).
`codec=True` compresses with the usual codec of the format. For FITS that is tile compression
without loss (RICE_1 for integers, GZIP_2 for floating point; the format of fpack, which astropy
and CFITSIO read), and a name that ends in `.fz` is written that way whatever `codec` says. (Up
to 0.11 `codec` had no effect on FITS output.)

The samples are uint8, uint16, uint32, uint64, float32 or float64. Keywords describe the array
as it is given; WCS keywords and BAYERPAT are converted when the rows are stored in the other
order (FITS and ASDF are written bottom-up). A header read from a FITS file can be passed as it
is: the cards that describe how FITS stores its data (SIMPLE, BITPIX, NAXIS, BZERO and the
like) are left out.

An image read with `read_image` and written again has its pixels, its keywords, its name, its
bounds and its ICC profile. It does not have everything an XISF file may hold: see "Good to
know". To copy an XISF file with everything in it, use `rewrite`.

## Whole files

What the command line tool does, with the same options:

```python
xisfconv.convert("m31.xisf", "m31.fits")                        # any pair of the formats
xisfconv.convert("m31.xisf", "preview.png", stretch="auto")     # PixInsight's auto-STF
xisfconv.convert("m31.xisf", "small.png", stretch="auto", sample_format="uint8", resize=512)
xisfconv.rewrite("m31.xisf", "smaller.xisf", codec="zstd", checksum="sha256")
xisfconv.rewrite_in_place("m31.xisf", codec="zstd")             # read back and compared first
report = xisfconv.verify("m31.xisf")                            # report.verdict, report.problems
```

`bin=2`, `resize=512` (the longest side), `resize=(1024, 768)` (a box to fit) and `scale=0.5` make
a smaller TIFF or PNG picture: every pixel the mean of the pixels it covers, taken before a
stretch.

`progress=` takes a function `progress(stage, done, total)`. It is called between the steps of
the work, in the thread that made the call. A rewrite and a verification have a step per data
block. A conversion has a step per image while it reads an XISF file; it reads a FITS or ASDF
file in one step and writes its output in one, as `write` does. Since 0.20 a step that goes on
is reported again every 8 MiB or so of data, with the stage and numbers of the last report: a
large image gives many reports too. An exception that the function raises stops the work there
and leaves no partly written file.

Ctrl-C stops the work at the same places, and so does any other signal whose handler raises
(a SIGTERM handler that ends the program, an alarm that sets a time limit): the exception is
raised from the call once the library has stopped and cleaned up. Pressed during the last
step, Ctrl-C is raised when the file is complete.

## Distributed XISF units

An XISF unit is one file, the monolithic `.xisf`, or it is distributed: a header file (`.xish`),
which is the XML header alone, and the files that header names, where the data blocks are:
XISF data blocks files (`.xisb`) and any other files, each of which is one block. PixInsight
itself reads and writes monolithic files only; other software reads and writes both.

```python
xisfconv.write("m31.xish", data, codec="zstd")          # m31.xish (the header) and m31.xisb (the data)
data = xisfconv.read("m31.xish")                        # the header file is the one to name
xisfconv.rewrite("m31.xish", "m31.xisf")                # packed into one file, every block as it is
xisfconv.rewrite("m31.xisf", "m31.xish")                # ... and unpacked
xisfconv.rewrite_in_place("m31.xish", codec="zlib")     # both files replaced, read back first

with xisfconv.open("m31.xish") as f:
    f.unit                # "distributed" ("monolithic" for m31.xisf)
    f.external_files      # ["/data/m31.xisb"]: the files the header names
    f.unit_size           # the size of them all; f.size is that of the header file
```

The kind of unit follows the name: `.xish` is a header file with its data blocks in the file of
the same name that ends in `.xisb`, any other name is one monolithic file. That holds for
`write`, `convert`, `rewrite`, `XISF.write` and `CCDData.write`. An existing file of either name
is kept unless `overwrite=True`.

A header is data that came from somewhere, and it says which files are read: one that names a
file of this machine as the pixels of an image would have a conversion copy that file into its
output. So a header is followed only to files in its own directory and below it, and only a
header file that is named as one (`.xish`) is followed at all: a monolithic `.xisf` file holds
all of its data, so one that names the file beside it is not followed there. A header that
names a file elsewhere (an absolute path, a `file:` URL, a path with `..`, a symbolic link that
leads out) raises `xisfconv.NotAllowedError`, which is also a `PermissionError`:

```python
xisfconv.read("frame.xish", external_files="anywhere")  # this header may lead anywhere on the machine
xisfconv.read("frame.xish", external_files="none")      # ... or to no other file at all
```

`open`, `read`, `read_image`, `convert`, `rewrite`, `rewrite_in_place`, `stored_as_requested` and
`verify` take the argument, and each call says it for itself: nothing is remembered, and an open
file keeps what it was opened with. A property in a file that is not read is left out with a
warning (`read_image`, `convert`) or raises when it is asked for by name; `verify` reports the
block as not checked. Nothing is ever fetched from a network: a block at an `http:` URL raises
`xisfconv.UnsupportedError`. `xisfconv.astropy` reads a unit by the name of its header file; a
header that comes from a stream without a file name, or that astropy fetched from a URL, has
no directory of its own to look in, and is refused if it names other files.

A data blocks file is written anew whenever its unit is written, with the blocks of that unit
alone. `rewrite_in_place` therefore refuses (`FileExistsError`) to replace one that also holds
blocks its header does not name, unless `overwrite=True`; a data blocks file that several
headers share is for reading. The name of a unit has to be valid UTF-8, since the header holds
the name of the data blocks file.

## Coming from the xisf package

[`xisf`](https://github.com/sergio-dr/xisf) is the usual package for XISF files in Python. The
module `xisfconv.xisf` has its class, with the same methods, arguments and return values, so
that a program written for it runs on this library with one line changed:

```python
from xisfconv.xisf import XISF           # was: from xisf import XISF

xisf = XISF("file.xisf")
file_meta = xisf.get_file_metadata()     # {id: {"id": ..., "type": ..., "value": ...}}
ims_meta = xisf.get_images_metadata()    # geometry, dtype, FITSKeywords, XISFProperties, ...
im_data = xisf.read_image(0)             # [height, width, channels]
XISF.write("output.xisf", im_data, creator_app="My script v1.0",
           image_metadata=ims_meta[0], xisf_metadata=file_meta, codec="lz4hc", shuffle=True)
im_data = XISF.read("file.xisf")
```

None of the code of that package is used; the files are read and written by the library. What
you get with it:

- Checksums are verified. Blocks compressed in subblocks, big-endian samples, the "Normal" pixel
  storage, 64-bit integer samples, data embedded in the header, ByteArray and complex vectors are
  read.
- A file that is read and written again keeps its keywords as they were written (strings with
  their quotes and blanks) and its properties with their types, comments, formats and line
  breaks: a keyword value is a `str` that also remembers, in `raw`, what the file has, and so
  is a text of the header that an XML reader does not give as it is written (one with CR LF).
  Numbers among the properties are numbers, and one that is not changed is
  written with the text the file has (`2000` stays `2000`); a new number is written in the
  shortest form that reads back as the same one.
- Files are written under another name and renamed when complete; vectors, matrices and texts
  of more than 3072 bytes among the properties are compressed with the codec of the pixels.

Where it differs from the package, on purpose:

| | `xisf` | `xisfconv.xisf` |
|---|---|---|
| a damaged block | is returned | `ChecksumError` |
| a distributed unit (`.xish`) | not read | read and written; `location` is `("path", path, identifier)` |
| Float64 written as `3` | the int 3 | the float 3.0 |
| Boolean written as `1` | False | True |
| a complex scalar | a pair of numbers | a complex number |
| a String without text | None | `""` |
| a String with a `value` attribute and a text | the text | the value |
| not-a-number, infinity | an error | read |
| a property that cannot be read | an error, or printed and False | left out, with a warning |
| two properties with one id | the last | the first, as everywhere in the library |
| a file without Metadata | an error | no file properties |
| a header with a DOCTYPE | read | refused: it could define text that is not in the file |
| arrays | read-only views | writable |
| an array with the channels first | written with a wrong geometry | written as it is |
| a 2-D array | an error | one channel |
| keyword text | written without quotes | a FITS string; numbers, `T` and `F` as they are |
| SIMPLE, BITPIX, NAXIS and the like | written | left out |
| a Boolean property | written as `True` | written as `true` |
| an image id like "my frame" | written | written as `my_frame`, a name XISF takes |
| a solution of PixInsight that was read, with a cropped image or other WCS keywords | written | left out; made from the WCS keywords if there are any, else a warning |
| a new text with a carriage return or with blanks at its ends | in the header, where an XML reader may lose them | a data block |
| a creator name of several lines | written | one line |
| `XISF:CreationTime` and the like | set in the caller's dictionary | the writer's own; the dictionary is not changed |
| bounds of floating point samples | always 0:1 | 0:1, 0:65535 or minimum:maximum, as the data needs |
| 8-bit samples with `shuffle=True` | "zlib+sh" | "zlib": there is nothing to shuffle |

Errors are those of xisfconv; where the package raises `ValueError` (not an XISF file, an image
number the file does not have) or `NotImplementedError`, the error raised is one of those as
well, so `except ValueError` still catches.

The `xisf` package reads what this module writes as far as it reads such things from any file:
it does not open a file with 64-bit integer samples, with a property that is not-a-number or
infinite, or with a vector or matrix without elements, and it has no value for a ByteArray.
The dictionaries remember what was read (the text of a value, the keywords a solution belongs
to); a copy made with `dict(entry)` has the values only, and is written as a program's own.

## With astropy

```python
import xisfconv.astropy                      # registers the format "xisf" for CCDData
from astropy.nddata import CCDData

ccd = CCDData.read("m31.xisf", unit="adu")   # data, header, WCS; mask and uncertainty
ccd.write("calibrated.xisf", codec="zstd")

hdulist = xisfconv.astropy.read_hdulist("m31.xisf")     # astropy.io.fits.HDUList, in memory
xisfconv.astropy.write_hdulist(hdulist, "from-hdulist.xisf")
```

This module follows the FITS conventions, as astropy does: rows bottom-up, a colour image as
`[channels, height, width]`. The header is the one the tool writes when it converts the file to
FITS, with WCS keywords built from a PixInsight astrometric solution if the file has no others;
EXTNAME holds the name of the image and ROWORDER says BOTTOM-UP. Mask, uncertainty and PSF of a
`CCDData` are stored as further images named as astropy names their HDUs. Files packed with
gzip, bzip2 or xz are read.

`CCDData.read` hands the image to astropy's own FITS reader as a FITS file in memory, so that
units, mask and uncertainty behave exactly as with FITS. That takes about four times the size of
the image in memory; `xisfconv.read` takes about twice. Most of the time of such a call is spent
in astropy's code, which catches some exceptions itself: an exception of the class `Exception`
that a signal handler raises there (an alarm's time limit) can be lost. `KeyboardInterrupt` and
`SystemExit` are not.

## Good to know

- Warnings of the library are Python warnings of the class `xisfconv.XisfconvWarning`, raised
  when the call is back; its notes on how a conversion was done go to the logger `"xisfconv"` at
  level INFO. Errors of the library are subclasses of `xisfconv.Error`, and several are also
  `OSError`, `FileNotFoundError`, `FileExistsError`, `PermissionError`, `ValueError`, `IndexError` or `LookupError`,
  as fits. An argument of the wrong kind raises `ValueError` or `TypeError` as usual, and an
  image name that the file does not have `KeyError`.
- FITS and ASDF are supported as far as images need them: signed integers are read as unsigned
  if none is negative and as floating point otherwise, and tables are left alone. For
  everything else in those formats use astropy or the asdf package.
- An array is read and written as a whole, in memory. Reading takes about twice the size of the
  image for a moment. Writing takes once the size of the image on top of the array, twice for a
  colour image with the channels last, and for a compressed XISF file at most 256 MiB more
  (beyond that, temporary files beside it; see "Large images" in the manual of the tool).
  `convert` and `rewrite` hold no image whole: they read and write it a piece at a time.
- Not everything of an XISF file is carried by `read_image` and `write`: the saved screen
  stretch, the resolution and the thumbnail are not. The XISF properties are (since 0.15; up to
  0.14 they were read and not written), with the astrometric solution of PixInsight among them,
  and a colour filter array is, as a BAYERPAT keyword. `rewrite` copies an XISF file with
  everything in it.
- Vectors and matrices among the properties are NumPy arrays in the type of their elements,
  complex ones too. Up to 0.14 they were float64 arrays, and complex ones None.
- `convert` does take the XISF properties along: to FITS as a table behind each image, to ASDF
  under the key `xisf` of the tree, and back to XISF as the properties they were, the
  astrometric solution of PixInsight included (as long as the WCS keywords were not changed on
  the way). `properties=False` leaves them out. A FITS or ASDF file that carries properties shows
  them as `file[0].properties` and `file.properties`.
- FITS keywords are ASCII: in a FITS or ASDF file, other characters in a keyword text are
  written as `?`. An XISF file keeps them.
- A `File` and the images read from it belong to one thread at a time; separate files, and the
  functions that take file names, can be used from several threads at once.
- Signal handlers run between the steps of a call only in the main thread, as everywhere in
  Python. Reading one image is one step: Ctrl-C during `read` takes effect when the image has
  been read.
- A progress function or a signal handler may use the package while a call is at work. If a
  handler closes an open file that the program is reading from, the reading stops with
  `ValueError("the file is closed")`.
- Two limits that come from Python itself. On Python 3.10, of two signal handlers that raise
  at the same moment one exception is printed and the call raises `xisfconv.Cancelled`. On
  Python 3.14 (seen with 3.14.0rc2) an exception from a signal handler can leave a lock held,
  in any program; here an open `File` that is shared between threads can then stay locked by
  the thread that was interrupted.
- When Python ends while a daemon thread is inside a call, the call is stopped and raises
  `xisfconv.Cancelled` in that thread, and no partly written file is left. A daemon thread that
  goes on to start another call is cut off where it is, as daemon threads are, and may leave a
  `.part` file.
- On Windows a file cannot be replaced while it is open somewhere, and that includes a FITS file
  that astropy has mapped into memory in the same program (as long as an array read from it is
  alive): `write(..., overwrite=True)` then fails with `xisfconv.FileError`.
- The wheels hold the shared library `libxisfconv`. Another build of it is used when the
  environment variable `XISFCONV_LIBRARY` names it.

Licence: LGPL-3.0-or-later. The wheels contain Zstandard (those for Windows zlib as well); see
`THIRD-PARTY-NOTICES.md`.
