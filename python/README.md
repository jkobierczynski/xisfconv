# xisfconv for Python

PixInsight **XISF** images as NumPy arrays, and conversion between **XISF**, **FITS** and
**ASDF**, with TIFF and PNG export. This is the Python package of
[xisfconv](https://github.com/jkobierczynski/xisfconv): the same library as the command line
tool, with nothing else to install.

```
pip install xisfconv            # NumPy is the only dependency
pip install "xisfconv[astropy]" # with astropy, for CCDData and HDUList
```

## Reading

```python
import xisfconv

data = xisfconv.read("m31.xisf")            # [height, width] or [height, width, channels]

image = xisfconv.read_image("m31.xisf")     # the pixels and what describes them
image.data, image.name, image.bounds
image.keywords["EXPTIME"]                   # FITS keywords
image.properties["Instrument:Telescope:FocalLength"]   # XISF properties

with xisfconv.open("m31.xisf") as f:        # XISF, FITS (also tile-compressed .fits.fz) or ASDF
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
xisfconv.write("copy.fits", xisfconv.read_image("m31.xisf"))       # the image with its keywords
xisfconv.write("out.xisf", data, overwrite=True)                   # an existing file is kept otherwise
```

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
xisfconv.rewrite("m31.xisf", "smaller.xisf", codec="zstd", checksum="sha256")
xisfconv.rewrite_in_place("m31.xisf", codec="zstd")             # read back and compared first
report = xisfconv.verify("m31.xisf")                            # report.verdict, report.problems
```

`progress=` takes a function `progress(stage, done, total)`. It is called between the steps of
the work, in the thread that made the call. A rewrite and a verification have a step per data
block. A conversion has a step per image while it reads an XISF file; it reads a FITS or ASDF
file in one step and writes its output in one, as `write` does. So there are many reports for a
file with many images and few for one large image. An exception that the function raises
stops the work there and leaves no partly written file.

Ctrl-C stops the work at the same places, and so does any other signal whose handler raises
(a SIGTERM handler that ends the program, an alarm that sets a time limit): the exception is
raised from the call once the library has stopped and cleaned up. Pressed during the last
step, Ctrl-C is raised when the file is complete.

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
  `OSError`, `FileNotFoundError`, `FileExistsError`, `ValueError`, `IndexError` or `LookupError`,
  as fits. An argument of the wrong kind raises `ValueError` or `TypeError` as usual, and an
  image name that the file does not have `KeyError`.
- FITS and ASDF are supported as far as images need them: signed integers are read as unsigned
  if none is negative and as floating point otherwise, and tables are left alone. For
  everything else in those formats use astropy or the asdf package.
- An image is read and written as a whole, in memory. Reading takes about twice the size of the
  image for a moment (three times for a compressed file). Writing takes once the size of the
  image on top of the array, twice for a colour image with the channels last, and about four
  times when the file is compressed.
- Not everything of an XISF file is carried by `read_image` and `write`. XISF properties are
  read, not written: the astrometric solution goes into a new file as WCS keywords
  (`entry.wcs_keywords()`), from which xisfconv writes PixInsight's solution properties again.
  The saved screen stretch and the resolution are not carried. A colour filter array is, as a
  BAYERPAT keyword.
- FITS keywords are ASCII: other characters in a keyword text are written as `?`.
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
