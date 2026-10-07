# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""XISF files for astropy.

Importing this module registers the format "xisf" with astropy's unified I/O, for
``CCDData``::

    import xisfconv.astropy
    from astropy.nddata import CCDData

    ccd = CCDData.read("m31.xisf", unit="adu")     # data, header, WCS, unit; mask and uncertainty
    ccd.write("calibrated.xisf", codec="zstd")

and it has the same two steps for ``astropy.io.fits``::

    hdulist = xisfconv.astropy.read_hdulist("m31.xisf")
    xisfconv.astropy.write_hdulist(hdulist, "copy.xisf")

Everything here follows the FITS conventions, as astropy does: row 0 of an array is the
bottom of the image, a colour image is ``[channels, height, width]``, and the header describes
the pixels that way, WCS included. A header is what the command line tool writes when it
converts the file to FITS: the FITS keywords of the image, keywords derived from XISF
properties where the file has none, and WCS keywords, also from a PixInsight astrometric
solution. Two cards say what the file itself does not: EXTNAME holds the name of the image
and ROWORDER says BOTTOM-UP.

The file may be any that xisfconv reads: XISF, and ASDF or FITS too (for FITS, astropy's
own reader does more). When reading, a file is recognized as XISF by its first bytes; when
writing, by the extension ``.xisf``.

A distributed XISF unit is read and written by the name of its header file (``.xish``): the
data blocks are in the file beside it that the header names (``.xisb``), and writing under
such a name writes both. A header that comes from a stream without a file name, or from a
packed file, has no directory where its data could be looked for: reading it is an error if it
names other files. The same holds for what astropy fetched from a URL for the caller: that
file is in a directory for temporary files, under a name that is not a header file's, and no
file beside it is read.
"""

import atexit
import bz2
import contextlib
import functools
import gzip
import io
import lzma
import numbers
import operator
import os
import shutil
import tempfile
import uuid
import zlib

import numpy as np
from astropy.io import fits
from astropy.io import registry as io_registry
from astropy.nddata import CCDData, fits_ccddata_reader

from . import _core
from ._core import Image, Keywords, write as _write

__all__ = ["header", "read_ccddata", "read_hdulist", "write_ccddata", "write_hdulist"]

_SIGNATURE = b"XISF0100"
_PATH_TYPES = (str, bytes, os.PathLike)
_PACKED = ((b"\x1f\x8b", gzip.open), (b"BZh", bz2.open), (b"\xfd7zXZ\x00", lzma.open))


def _same_file(stream, name):
    """True if a stream, read from its start, gives what the file of its name holds."""
    try:
        if stream.tell() != 0:
            return False
        ours, theirs = os.fstat(stream.fileno()), os.stat(name)
        if (ours.st_dev, ours.st_ino) != (theirs.st_dev, theirs.st_ino):
            return False
        # one file, but the stream may unpack it on the way (astropy opens .gz files so)
        start = stream.read(64)
        stream.seek(0)
        with io.open(name, "rb") as plain:
            return plain.read(64) == start
    except (OSError, ValueError, AttributeError, TypeError):
        return False


def _label(source):
    """What to call a stream in messages."""
    name = getattr(source, "name", None)
    if isinstance(name, bytes):
        name = os.fsdecode(name)
    return name if isinstance(name, str) and name else "<%s>" % type(source).__name__


# The temporary files that exist now. Should an interrupt keep one from being removed where
# it was used, it is removed when Python ends.
_temporary = set()


def _remove_temporary():
    for copy in list(_temporary):
        with contextlib.suppress(OSError):
            os.remove(copy)


atexit.register(_remove_temporary)


# How a temporary copy is opened: for its owner only. (Not a Python function: an interrupt
# between the opening and the return would leave the file open.)
_private = functools.partial(os.open, mode=0o600)


def _copy(stream, temporary):
    """Copies a stream into a temporary file, whose name is added to `temporary`."""
    # The name is noted before the file exists, so that the file is removed whatever happens
    # from here on (tempfile.mkstemp would make the file first and tell its name afterwards).
    copy = os.path.join(tempfile.gettempdir(), "xisfconv-%s.tmp" % uuid.uuid4().hex)
    _temporary.add(copy)
    temporary.append(copy)
    with io.open(copy, "xb", opener=_private) as out:        # "x": the file must not exist yet
        shutil.copyfileobj(stream, out)
    return copy


@contextlib.contextmanager
def _input(source):
    """An open :class:`xisfconv.File` for what the caller gave.

    An open file (astropy's registry hands over the file it has opened) is read by its name if
    it is at its start and gives the bytes of that file; any other stream is copied into a
    temporary file. A file packed with gzip, bzip2 or xz is unpacked into a temporary file, as
    astropy does for FITS. Messages name what the caller gave, not a copy.
    """
    temporary = []
    try:
        shown = None
        if isinstance(source, _PATH_TYPES):
            name = source
        elif not hasattr(source, "read"):
            raise TypeError("xisfconv reads from a file name or an open binary file, not from %s" %
                            type(source).__name__)
        else:
            name = getattr(source, "name", None)
            if not (isinstance(name, (str, bytes)) and _same_file(source, name)):
                shown = _label(source)
                name = _copy(source, temporary)
        try:
            with io.open(name, "rb") as plain:
                start = plain.read(8)
        except OSError:
            start = b""             # the library says what is wrong with the file
        unpack = next((opener for magic, opener in _PACKED if start.startswith(magic)), None)
        if unpack is not None:
            if shown is None:
                shown = os.fsdecode(os.fspath(name))
            try:
                with unpack(name, "rb") as packed:
                    name = _copy(packed, temporary)
            except (EOFError, zlib.error, lzma.LZMAError, OSError) as e:
                raise _core.FormatError("%s: the packed file cannot be unpacked: %s" % (shown, e)) from None
        # A copy is not where the file was: the header of a distributed unit names files beside
        # itself, and beside a copy in the directory for temporary files there are other
        # people's files. Such a header is followed to none.
        with _core.File(name, external_files="none" if temporary else None, _shown=shown) as file:
            if temporary and file.external_files:
                raise _core.NotAllowedError(
                    "%s: this XISF header has its data in other files (%s), which are looked for beside the header "
                    "file: read a distributed unit by the name of its header file (.xish), not from a stream or a "
                    "packed file" % (file._name, ", ".join(os.path.basename(other) for other in file.external_files)))
            yield file
    finally:
        for copy in temporary:
            with contextlib.suppress(OSError):
                os.remove(copy)
            _temporary.discard(copy)


def header(keywords):
    """An ``astropy.io.fits.Header`` from :class:`xisfconv.Keywords` (or anything they are
    made from), with the cards as xisfconv writes them to a FITS file."""
    return fits.Header.fromstring(Keywords(keywords).fits_text())


def _ascii(text):
    """Text reduced to what a FITS header can hold: printable ASCII."""
    return "".join(c if " " <= c <= "~" else "?" if ord(c) > 127 else " " for c in text)


def _xisf_id(name):
    """A name as the XISF writer makes an image id of it: letters, digits and underscores."""
    out = ""
    for c in name:
        if c.isascii() and (c.isalnum() or c == "_"):
            out += c
        elif out and not out.endswith("_"):
            out += "_"
    out = out.rstrip("_")
    return "_" + out if out[:1].isdigit() else out


def _named(file, name):
    """The image of a name, as astropy finds an HDU: whatever the case. A name that XISF could
    not hold as it is (``BAD-PIX`` is stored as ``BAD_PIX``) is found too."""
    wanted = (name.upper(), _xisf_id(name).upper())
    for entry in file:
        if entry.name == name:
            return entry
    for entry in file:
        if entry.name and entry.name.upper() in wanted:
            return entry
    raise KeyError(name)


def _chosen(file, image):
    if isinstance(image, str):
        return _named(file, image)
    try:
        operator.index(image)
    except TypeError:
        raise TypeError("an image is chosen by its number or its name, not by %r" % (image,)) from None
    return _core._one_image(file, image)


def _hdu(entry, primary, sample_format, property_keywords, wcs, sip_order, verify, name=None):
    data = entry.read(sample_format, row_order="bottom-up", channels="first", verify=verify)
    cards = header(entry.fits_keywords("bottom-up", property_keywords=property_keywords, wcs=wcs, sip_order=sip_order))
    name = entry.name if name is None else name
    if name:
        cards.remove("EXTNAME", ignore_missing=True, remove_all=True)
    hdu = fits.PrimaryHDU(data) if primary else fits.ImageHDU(data)
    if name:
        hdu.header["EXTNAME"] = (_ascii(name), "image identifier")
    hdu.header["ROWORDER"] = ("BOTTOM-UP", "order of image rows")
    hdu.header.extend(cards, bottom=True)
    return hdu


def read_hdulist(path, image=None, *, sample_format=None, property_keywords=True, wcs=True, sip_order=3, verify=True):
    """Reads a file into an ``astropy.io.fits.HDUList``, in memory: the first image as the
    primary HDU, the others as IMAGE extensions. Pixels and headers are those of the FITS file
    that ``xisfconv.convert(path, "x.fits")`` writes, without the two cards with which the
    converter signs its file (PROGRAM and a HISTORY line). ``path`` is a file name, or an open
    binary file.

    image
        Only this image (a number or a name); None: every image that can be read.
    sample_format
        Convert the samples, as for :func:`xisfconv.read`; None: as stored.
    property_keywords, wcs, sip_order
        As for :meth:`xisfconv.FileImage.fits_keywords`.
    verify
        Verify the checksums of the file.
    """
    hdus = []
    with _input(path) as file:
        name = file._context.path
        for entry in (list(file) if image is None else [_chosen(file, image)]):
            if not entry.readable:
                if image is not None:
                    raise _core.UnsupportedError("image %d: %s" % (entry.index, entry.unsupported_reason))
                _core._warn("%s: skipping image %d: %s" % (name, entry.index, entry.unsupported_reason))
                continue
            hdus.append(_hdu(entry, not hdus, sample_format, property_keywords, wcs, sip_order, verify))
    if not hdus:
        raise _core.FormatError("%s: no image that can be read" % (path,))
    return fits.HDUList(hdus)


def _samples(hdu, label):
    """The pixels of an HDU as xisfconv writes them, by the rules of its FITS reader: unsigned
    integers and floating point as they are, signed integers as unsigned if none is negative
    and as floating point otherwise. Data that astropy was told not to scale is scaled here."""
    data = np.asarray(hdu.data)
    kind, size = data.dtype.kind, data.dtype.itemsize
    scale, zero = hdu.header.get("BSCALE", 1), hdu.header.get("BZERO", 0)
    if not isinstance(scale, numbers.Real) or not isinstance(zero, numbers.Real) or isinstance(scale, bool):
        scale, zero = 1, 0
    # Scaling that is still in the header of data in one of the types FITS stores (bytes, signed
    # integers, floating point) has not been applied: astropy takes the cards out when it applies
    # them. Data of the other types (unsigned, signed bytes) has been scaled by astropy, which
    # keeps a BZERO of its own in their header and disregards what else is there.
    stored_type = (kind == "u" and size == 1) or (kind == "i" and size > 1) or (kind == "f" and size in (4, 8))
    if stored_type and (scale != 1 or zero != 0):
        if kind == "i" and scale == 1 and zero == 2 ** (8 * size - 1):
            # the FITS way of storing unsigned integers: the same bits with the top one turned
            native = np.ascontiguousarray(data, dtype=data.dtype.newbyteorder("="))
            unsigned = native.view(native.dtype.str.replace("i", "u")).copy()
            unsigned ^= unsigned.dtype.type(2 ** (8 * size - 1))
            return unsigned
        floating = np.float32 if size <= 2 else np.float64
        return (data.astype(floating) * floating(scale) + floating(zero)).astype(floating)
    if kind == "u" or (kind == "f" and size in (4, 8)):
        return data
    if kind == "b":
        return data.astype(np.uint8)
    if kind == "f":   # float16, extended precision
        return data.astype(np.float32 if size < 4 else np.float64)
    if kind == "i":
        if data.size == 0 or data.min() >= 0:
            return data.astype(data.dtype.newbyteorder("=").str.replace("i", "u"))
        return data.astype(np.float32 if size <= 2 else np.float64)
    raise TypeError("%s: data of type %s cannot be written as an image" % (label, data.dtype.name))


def write_hdulist(hdulist, path, *, overwrite=False, format=None, codec=None, checksum=None, stored_row_order=None,
                  subblock_size=None, wcs=True):
    """Writes the image HDUs of an ``astropy.io.fits.HDUList`` (or one HDU) to an XISF file,
    or to any format :func:`xisfconv.write` writes (``format``; None: by the extension).

    Each HDU with a 2-D or 3-D array becomes an image: its EXTNAME the name, its header the
    keywords, its rows bottom-up unless its ROWORDER keyword says TOP-DOWN. HDUs without
    such data (an empty primary HDU, tables) are left out, tables with a warning. Signed
    integers are written as unsigned integers if none is negative and as floating point
    otherwise, as when the command line tool reads a FITS file. The other options are those
    of :func:`xisfconv.write`.

    XISF names its images with letters, digits and underscores, and a cube of one plane is
    an ordinary image there: EXTNAME ``BAD-PIX`` comes back as ``BAD_PIX``, and data of the
    shape (1, 40, 60) as (40, 60).
    """
    if not isinstance(hdulist, (list, tuple, fits.HDUList)):
        hdulist = [hdulist]
    images = []
    for number, hdu in enumerate(hdulist):
        label = "HDU %d" % number
        if not isinstance(hdu, (fits.PrimaryHDU, fits.ImageHDU, fits.CompImageHDU)):
            _core._warn("%s (%s) is not an image; it is not written" % (label, type(hdu).__name__))
            continue
        if hdu.data is None:
            continue
        if np.ndim(hdu.data) not in (2, 3) or np.size(hdu.data) == 0:
            _core._warn("%s holds %d-dimensional data, not an image; it is not written" % (label, np.ndim(hdu.data)))
            continue
        cards = hdu.header
        name = str(cards.get("EXTNAME", "") or "").strip()
        keywords = Keywords()
        for card in cards.cards:
            if card.keyword == "EXTNAME" and name:
                continue
            keywords.append(card.keyword, card.value, card.comment)
        order = str(cards.get("ROWORDER", "") or "").strip().upper()
        images.append(Image(_samples(hdu, label), keywords=keywords, name=name or None,
                            row_order="top-down" if order == "TOP-DOWN" else "bottom-up", channels="first"))
    if not images:
        raise ValueError("there is no image HDU to write")
    # An image without a name is named after the file. That must not be the name of another
    # image of the list (a file "mask.xisf" with a mask in it): astropy finds HDUs by name.
    if isinstance(path, _PATH_TYPES):
        stem = (_xisf_id(os.path.splitext(os.path.basename(os.fsdecode(os.fspath(path))))[0]) or "image").upper()
        taken = {_xisf_id(item.name).upper() for item in images if item.name}
        if stem in taken:
            free = ("image%s" % ("" if n == 1 else "_%d" % n) for n in range(1, len(images) + len(taken) + 2))
            for item in images:
                if not item.name:
                    item.name = next(name for name in free if name.upper() not in taken)
    _write(path, images, format=format, codec=codec, checksum=checksum, stored_row_order=stored_row_order,
           subblock_size=subblock_size, wcs=wcs, overwrite=overwrite)


def read_ccddata(filename, hdu=0, unit=None, hdu_uncertainty="UNCERT", hdu_mask="MASK", hdu_flags=None,
                 key_uncertainty_type="UTYPE", hdu_psf="PSFIMAGE", *, sample_format=None, property_keywords=True,
                 wcs=True, sip_order=3, verify=True):
    """Reads an image of a file as ``CCDData``. This is what ``CCDData.read`` calls for the
    format "xisf", which astropy recognizes by the first bytes of the file.

    The arguments up to ``hdu_psf`` mean what they mean for astropy's FITS reader
    (``astropy.nddata.fits_ccddata_reader``), with the images of the file in the place of
    HDUs: ``hdu`` is the number or the name of the image, ``unit`` the unit of the data if
    its BUNIT keyword does not say, and images named UNCERT, MASK and PSFIMAGE become the
    uncertainty, the mask and the PSF. That is how :func:`write_ccddata` stores them. Names
    are compared as astropy compares the names of HDUs: whatever the case. The other
    arguments are those of :func:`read_hdulist`.

    ``filename`` may also be an open binary file, which is what astropy's registry passes,
    and a file packed with gzip, bzip2 or xz.

    The image is handed to astropy's FITS reader as a FITS file in memory, so that every
    detail is astropy's own. That takes about four times the size of the image in memory.
    """
    roles = [name for name in (hdu_uncertainty, hdu_mask, hdu_psf) if name]

    def role_of(name):
        """The HDU name under which astropy will look for an image, or None."""
        key = name.upper()
        for role in roles:
            if key in (role.upper(), _xisf_id(role).upper()):
                return role
        if hdu_flags and (key == hdu_flags.upper() or key.startswith(hdu_flags.upper() + "_")):
            return name
        return None

    with _input(filename) as file:
        path = file._context.path
        main = _chosen(file, hdu)
        if not main.readable:
            raise _core.UnsupportedError("image %d: %s" % (main.index, main.unsupported_reason))
        hdus = [_hdu(main, True, sample_format, property_keywords, wcs, sip_order, verify)]
        if main.name and role_of(main.name) is not None:
            # the image itself must not be taken for its own mask
            hdus[0].header.remove("EXTNAME", ignore_missing=True)
        for entry in file:
            role = None if entry.index == main.index or not entry.name else role_of(entry.name)
            if role is None:
                continue
            if not entry.readable:
                _core._warn("%s: image %d (%s) is left out: %s" % (path, entry.index, entry.name,
                                                                  entry.unsupported_reason))
                continue
            hdus.append(_hdu(entry, False, None, property_keywords, wcs, sip_order, verify, name=role))
    memory = io.BytesIO()
    # (cards that are not quite standard are astropy's to judge when it reads them back)
    fits.HDUList(hdus).writeto(memory, output_verify="silentfix+ignore")
    del hdus
    memory.seek(0)
    return fits_ccddata_reader(memory, hdu=0, unit=unit, hdu_uncertainty=hdu_uncertainty, hdu_mask=hdu_mask,
                               hdu_flags=hdu_flags, key_uncertainty_type=key_uncertainty_type, hdu_psf=hdu_psf)


def write_ccddata(ccd_data, filename, hdu_mask="MASK", hdu_uncertainty="UNCERT", hdu_flags=None,
                  key_uncertainty_type="UTYPE", as_image_hdu=False, hdu_psf="PSFIMAGE", *, overwrite=False,
                  format="xisf", codec=None, checksum=None, stored_row_order=None, subblock_size=None, wcs=True):
    """Writes ``CCDData`` to an XISF file. This is what ``ccd.write`` calls for the format
    "xisf", which astropy chooses for a file name that ends in ``.xisf``.

    The data becomes the first image, with the header and the WCS as its keywords and the
    unit in BUNIT; the mask, the uncertainty and the PSF become further images named as
    astropy names their HDUs (``hdu_mask``, ``hdu_uncertainty``, ``hdu_psf``), which
    :func:`read_ccddata` reads back. The arguments up to ``hdu_psf`` are those of
    ``CCDData.to_hdu``; the others those of :func:`write_hdulist`. ``format`` may name
    another format that xisfconv writes; None goes by the extension of the file name.
    """
    if not isinstance(filename, _PATH_TYPES):
        raise TypeError("xisfconv writes to a file name, not to an open file")
    options = {"hdu_mask": hdu_mask, "hdu_uncertainty": hdu_uncertainty, "key_uncertainty_type": key_uncertainty_type}
    # only what was asked for: older versions of astropy do not know all of these
    if hdu_flags is not None:
        options["hdu_flags"] = hdu_flags
    if as_image_hdu:
        options["as_image_hdu"] = as_image_hdu
    if hdu_psf != "PSFIMAGE":
        options["hdu_psf"] = hdu_psf
    hdulist = ccd_data.to_hdu(**options)
    write_hdulist(hdulist, filename, overwrite=overwrite, format=format, codec=codec, checksum=checksum,
                  stored_row_order=stored_row_order, subblock_size=subblock_size, wcs=wcs)


def _is_xisf(origin, path, fileobj, *args, **kwargs):
    try:
        name = "" if path is None else os.fsdecode(os.fspath(path)).lower()
    except TypeError:
        name = ""
    if fileobj is not None:
        try:
            position = fileobj.tell()
            start = fileobj.read(len(_SIGNATURE))
            fileobj.seek(position)
        except (OSError, AttributeError, ValueError):
            return False
        # A monolithic file by its signature. The header file of a distributed unit is an XML
        # document like many others: it is one by its name (.xish), if it begins like one.
        return start == _SIGNATURE or (name.endswith(".xish") and start.lstrip(b"\xef\xbb\xbf \t\r\n")[:1] == b"<")
    return name.endswith((".xisf", ".xish"))


io_registry.register_reader("xisf", CCDData, read_ccddata, force=True)
io_registry.register_writer("xisf", CCDData, write_ccddata, force=True)
io_registry.register_identifier("xisf", CCDData, _is_xisf, force=True)
