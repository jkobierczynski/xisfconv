# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""xisfconv: PixInsight XISF images as NumPy arrays, and conversion between XISF, FITS and ASDF.

Reading::

    import xisfconv

    data = xisfconv.read("m31.xisf")              # [height, width] or [height, width, channels]
    image = xisfconv.read_image("m31.xisf")       # the pixels with keywords, name, properties
    print(image.keywords["EXPTIME"])

    with xisfconv.open("m31.xisf") as f:          # XISF, FITS (also .fits.fz) or ASDF
        for entry in f:
            print(entry.name, entry.shape, entry.dtype)
        wcs = f[0].wcs_keywords()                 # also from a PixInsight astrometric solution

Writing, to XISF, FITS, ASDF, TIFF or PNG::

    xisfconv.write("out.xisf", data, keywords={"OBJECT": "M 31"}, codec="zstd", checksum="sha256")

Whole files, as the command line tool does it::

    xisfconv.convert("m31.xisf", "m31.fits")
    xisfconv.rewrite("m31.xisf", "smaller.xisf", codec="zstd")
    print(xisfconv.verify("m31.xisf").verdict)

Arrays have row 0 at the top of the image and the channels last, unless ``row_order`` and
``channels`` say otherwise. With astropy: ``import xisfconv.astropy`` makes
``CCDData.read("m31.xisf")`` work and gives the FITS conventions; see that module.

Warnings of the library are Python warnings of the class :class:`XisfconvWarning`; its notes
on how a conversion was done go to the logger "xisfconv" at level INFO.

FITS and ASDF are supported as far as images need them. For tables and everything else in
those formats, use astropy or the asdf package.
"""

from ._core import (ArgumentError, Cancelled, Card, ChecksumError, Error, File, FileError, FileImage, FormatError,
                    Image, ImageIndexError, InputNotFoundError, InternalError, Keywords, NotFoundError,
                    OutputExistsError, Properties, Report, RewriteResult, StretchParams, UnsupportedError,
                    XisfconvWarning, apply_stretch, auto_stretch, codec_available, convert, detect_format,
                    library_path, library_version, open, read, read_image, rewrite, rewrite_in_place,
                    stored_as_requested, verify, wcs_flip_rows, write)

try:
    from importlib.metadata import PackageNotFoundError, version as _version

    try:
        __version__ = _version("xisfconv")
    except PackageNotFoundError:   # used from the source tree: the package is as new as the library
        __version__ = library_version()
    del PackageNotFoundError, _version
except ImportError:   # pragma: no cover
    __version__ = library_version()

__all__ = [
    "ArgumentError", "Cancelled", "Card", "ChecksumError", "Error", "File", "FileError", "FileImage", "FormatError",
    "Image", "ImageIndexError", "InputNotFoundError", "InternalError", "Keywords", "NotFoundError",
    "OutputExistsError", "Properties", "Report", "RewriteResult", "StretchParams", "UnsupportedError",
    "XisfconvWarning", "apply_stretch", "auto_stretch", "codec_available", "convert", "detect_format",
    "library_path", "library_version", "open", "read", "read_image", "rewrite", "rewrite_in_place",
    "stored_as_requested", "verify", "wcs_flip_rows", "write",
]
