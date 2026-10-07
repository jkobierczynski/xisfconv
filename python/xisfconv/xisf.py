# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""The interface of the ``xisf`` package, on libxisfconv.

The ``xisf`` package of Sergio Díaz (https://github.com/sergio-dr/xisf) is the usual way to
read and write XISF files in Python. This module has a class of the same name, with the same
methods, the same arguments and the same structures for what they return, so that a program
written for that package runs on this library with its import changed::

    from xisfconv.xisf import XISF          # was: from xisf import XISF

    frame = XISF("light.xisf")
    about_file = frame.get_file_metadata()
    about_images = frame.get_images_metadata()
    pixels = frame.read_image(0)
    XISF.write("calibrated.xisf", pixels - 0.01, creator_app="calibrate 2.1",
               image_metadata=about_images[0], xisf_metadata=about_file, codec="zstd", shuffle=True)

    pixels = XISF.read("light.xisf")

None of the code of that package is used here: the files are read and written by the library
of xisfconv. That is where the two differ, and the differences are meant.

Reading
    - Checksums are verified: a damaged file raises :class:`xisfconv.ChecksumError` where the
      ``xisf`` package returns what it finds.
    - What the library reads is read: blocks compressed in subblocks, big-endian samples, the
      "Normal" pixel storage, 64-bit integer samples, data embedded in the header, ByteArray
      and complex vectors, a vector without elements, headers with text beyond ASCII.
    - So is a distributed unit: ``XISF("frame.xish")`` reads the header file, and the data
      blocks from the files that header names in its own directory (``frame.xisb``). The
      ``location`` of such a block is ``("path", path, identifier)``. A header that names a
      file somewhere else is not followed there: :class:`xisfconv.NotAllowedError`
      (:func:`xisfconv.open` has an argument for that).
    - Values have the type the file states: a Float64 written as ``3`` is the float 3.0, a
      Boolean written as ``1`` is True, a complex scalar is a complex number, not-a-number and
      infinity are read. A String without text is ``""``. A String that has a ``value``
      attribute has that value.
    - A property that cannot be read (a damaged block, a table, a vector of a type nobody
      knows) is left out with a warning; it does not cost the file. The same holds for the
      properties of the file, which the ``xisf`` package marks with False. Of two properties
      with one id the first is the property, as everywhere in the library.
    - A file without a Metadata element has no file properties; that is not an error.
    - A header with a document type declaration (DOCTYPE) is refused: it could define text that
      is not in the file. (:func:`xisfconv.open` reads such a file, and ignores the declaration.)
    - Arrays are writable, and their memory is their own.
    - What the structures of the ``xisf`` package have no place for is kept on the side, so
      that :meth:`XISF.write` can write a value that was not touched as the file has it. A
      keyword value is the text that package gives (quotes and blanks around it taken off) and
      has the text of the file in its ``raw`` attribute; a text of the header whose line breaks
      are written as such is the text an XML reader gives, every line break a line feed, and
      has in ``raw`` what the file has (PixInsight on Windows writes CR LF); the dictionary of a property remembers the text of its value, and
      one of an astrometric solution which WCS keywords and image size it was read with. A
      copy of such a dictionary (``dict(entry)``) has the values and not what is remembered.

Writing
    - The file is written under another name and renamed when it is complete.
    - Under a name that ends in ``.xish`` the unit is written distributed: that file is the
      header, and the data blocks go into the file of the same name that ends in ``.xisb``.
    - An array with the channels first is written with the geometry it has (the ``xisf``
      package writes its shape in the wrong order); a 2-D array is one channel.
    - Keyword values that are text are written as FITS strings, in quotes. Numbers, ``T`` and
      ``F`` are written as they are. The keywords that describe how a FITS file stores its
      data (SIMPLE, BITPIX, NAXIS and the like) are left out.
    - A property that was read and not changed is written with the text the file has for its
      value, and a text of the header is written into the header again with the bytes it has.
      A new value is
      written as XISF has such values: a Boolean as ``true`` or ``false``, a complex number as
      ``(re,im)``, a number in the shortest form that reads back as the same one. Comments and
      formats of properties are kept. A new text of more than 3072 bytes, with a carriage
      return or with white space at its ends is stored as data, where every reader finds it as
      it is. Data of more than 3072 bytes (texts, vectors, matrices) is compressed with the
      codec of the pixels.
    - An astrometric solution (``PCL:AstrometricSolution:...``) that was read from a file is
      written only with an image of the size and with the WCS keywords it was read with. For
      a cropped image, or with other keywords, it is left out as a whole, and a solution is
      made from the WCS keywords if the image has them; a warning says so if it has none, or
      if properties of the solution that the program had set are left out with it. (The
      ``xisf`` package writes the old solution, which PixInsight then believes.) Properties in
      dictionaries of the program's own are written as they are.
    - The name of the creator application is one line: line breaks and tabs in it become
      blanks.
    - The id of an image is made a name XISF takes: "my frame" becomes "my_frame".
    - How the file is stored is the writer's to say: ``XISF:CreationTime``,
      ``XISF:CreatorApplication``, ``XISF:CreatorModule`` (which names xisfconv),
      ``XISF:BlockAlignmentSize`` and ``XISF:CompressionCodecs`` are its own, the others of that
      kind are not written (``XISF:CompressionLevel`` is not the level of a codec), and the
      dictionary given as ``xisf_metadata`` is not changed.
    - The bounds of floating point samples are 0:1 if the data fits, else 0:65535 if it fits,
      else the minimum and the maximum (the ``xisf`` package always writes 0:1).
    - A BAYERPAT keyword of 2 x 2 also becomes the ColorFilterArray of the image.
    - Samples of one byte are not shuffled, and the codec the call returns does not say they
      were ("zlib" where the ``xisf`` package returns "zlib+sh").
    - A compression level is one the codec has: zlib 1 to 9, lz4hc 1 to 12, zstd 1 to 22.
    - The ``xisf`` package reads what is written here as far as it reads such things from any
      file. It does not open a file with samples of 64-bit integers, with a property that is
      not-a-number or infinite, or with a vector or a matrix without elements, and it has no
      value for a ByteArray.

Errors are those of xisfconv. Where the ``xisf`` package raises ``ValueError`` (a file that is
not XISF, an image number the file does not have) or ``NotImplementedError`` (something that is
not supported), the error raised here is one of those as well.
"""

import platform
import re
import xml.etree.ElementTree as ET
from collections.abc import Mapping

import numpy as np

from . import _core, _lib
from ._core import (FormatError, ImageIndexError, InternalError, UnsupportedError,
                    library_version as _library_version)

__all__ = ["XISF"]

try:
    from . import __version__
except ImportError:   # pragma: no cover
    __version__ = _library_version()

_NAMESPACE = "http://www.pixinsight.com/xisf"
_DECLARATION = re.compile(r"\s*<\?xml[^>]*\?>")
_CODECS = ("zlib", "lz4", "lz4hc", "zstd")


class NotXisfError(FormatError, ValueError):
    """The file is not an XISF file, or one that cannot be read. (A ``ValueError`` too: that is
    what the ``xisf`` package raises.)"""


class ImageNumberError(ImageIndexError, ValueError):
    """The file has no image of that number."""


class NotSupportedError(UnsupportedError, NotImplementedError):
    """Something the library does not read or write."""


def _as(kind, error):
    """The error of the library as one that is also what the ``xisf`` package raises."""
    new = kind(*error.args)
    new.__dict__.update(error.__dict__)
    return new.with_traceback(error.__traceback__)


class _KeywordValue(str):
    """A keyword value without the quotes and blanks around it, which remembers in ``raw``
    how the file wrote it."""

    def __new__(cls, raw):
        bare = raw.strip("'")
        self = str.__new__(cls, bare.strip(" "))
        self.raw = raw
        return self

    def __reduce__(self):
        return (_KeywordValue, (self.raw,))


class _Text(str):
    """The text of a String property as an XML reader gives it (every line break a line feed),
    which remembers in ``raw`` what the file has (PixInsight on Windows writes CR LF)."""

    def __new__(cls, text, raw):
        self = str.__new__(cls, text)
        self.raw = raw
        return self

    def __reduce__(self):
        return (_Text, (str(self), self.raw))


class _Property(dict):
    """A property as a dictionary, which remembers the text the file has for its value and the
    value that text was read as: a value that is not changed is written as that text. One of an
    astrometric solution also remembers which WCS keywords and image size it was read with."""

    as_read = None        # (type, text, value, whether the file keeps the text as a data block)
    solution_of = None    # (digest, value)


def _has_doctype(header):
    """True if the header has a document type declaration: one stands before the first element,
    behind nothing but the XML declaration, comments, processing instructions and white space."""
    at = 3 if header.startswith(b"\xef\xbb\xbf") else 0
    while True:
        while at < len(header) and header[at:at + 1] in b" \t\r\n":
            at += 1
        if header.startswith(b"<?", at):
            end = header.find(b"?>", at)
        elif header.startswith(b"<!--", at):
            end = header.find(b"-->", at + 4)
            end += 1 if end >= 0 else 0
        else:
            return header[at:at + 9].upper() == b"<!DOCTYPE"
        if end < 0:
            return False     # (not XML: the parser says so)
        at = end + 2


def _line_feeds(text):
    """Line breaks as XML defines them for the text of an element."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def _local(tag):
    """The name of an element without its namespace."""
    return tag.rsplit("}", 1)[-1] if isinstance(tag, str) else ""


_EXTERNAL = re.compile(r"(path|url)\((.*)\)(?::(0[xX][0-9a-fA-F]+|[0-9]+))?\Z", re.S)


def _location(text):
    """("attachment", position, size), ["inline", encoding] or ["embedded"]; for a block in
    another file ("path", path, identifier) or ("url", URL, identifier), with None for the
    identifier if the block is the whole file."""
    outside = _EXTERNAL.match(text)
    if outside:
        kind, where, identifier = outside.groups()
        return (kind, where, None if identifier is None else int(identifier, 0 if identifier[:2] in ("0x", "0X") else 10))
    parts = text.split(":")
    if parts[0] == "attachment" and len(parts) == 3:
        try:
            return (parts[0], int(parts[1]), int(parts[2]))
        except ValueError:
            pass
    return parts


def _compression(text):
    """(codec, uncompressed size, item size); the item size is None without byte shuffling."""
    parts = text.split(":")
    try:
        return (parts[0], int(parts[1]), int(parts[2]) if len(parts) == 3 else None)
    except (ValueError, IndexError):
        return text   # (a block that is read says what is wrong with it)


class XISF:
    """An XISF file: its metadata, read when the object is made, and its images. This is the
    class of the ``xisf`` package as far as a program sees it; the documentation of the module
    says where the two differ.

    What is read: monolithic XISF files and the header files of distributed units (.xish, with
    the data blocks in the files beside it that the header names), their Image elements (grayscale and colour, 8 to 64
    bits unsigned and 32 and 64 bits floating point), FITS keywords, and the properties of the
    images and of the file: scalars, strings, time points, vectors and matrices.
    """

    def __init__(self, fname):
        """Opens an XISF file and reads its header. The metadata is then to be had from
        :meth:`get_file_metadata` and :meth:`get_images_metadata`, the pixels from
        :meth:`read_image`."""
        try:
            file = _core.File(fname)
        except FormatError as e:
            raise _as(NotXisfError, e) from None
        with file:
            if file.format != "xisf":
                raise NotXisfError("%s: not an XISF file (it is %s)" % (file._name, file.format.upper()))
            self._path = fname
            self._root = self._tree(file)       # the header, as ElementTree has it
            # The images of the library are the Image elements of the root, in their order.
            elements = [child for child in self._root if _local(child.tag) == "Image"]
            if len(elements) != len(file):
                raise InternalError("%s: %d Image elements, and %d images" % (file._name, len(elements), len(file)))
            self._images = [self._image(file[n], element) for n, element in enumerate(elements)]    # a dictionary each
            # Its file properties: those of each Metadata element, and those of the root itself.
            elements = []
            for child in self._root:
                name = _local(child.tag)
                if name == "Metadata":
                    elements.extend(grandchild for grandchild in child if _local(grandchild.tag) == "Property")
                elif name == "Property":
                    elements.append(child)
            self._about_file = self._properties(file, file.properties, elements)

    # --- reading the header -------------------------------------------------------------------

    @staticmethod
    def _tree(file):
        """The header as ElementTree reads it. The library has read it already and found it to
        be XML; this gives the dictionaries every attribute as it is written."""
        header = file._header_bytes()
        # A document type declaration may define attributes and text that are not in the file,
        # and entities that unfold to any size. The library ignores one; nothing here needs one.
        if _has_doctype(header):
            raise NotXisfError("%s: the header has a document type declaration (DOCTYPE), which is not read here; "
                               "xisfconv.open reads the file" % (file._name,))
        # As text, and without the line that names an encoding: the header is UTF-8, whatever
        # that line calls it ("utf8", as the xisf package writes it, is no name the parser knows).
        text = _DECLARATION.sub("", header.decode("utf-8-sig", "replace"), count=1)
        try:
            return ET.fromstring(text)
        except ET.ParseError as e:
            raise NotXisfError("%s: the XML header is not well-formed: %s" % (file._name, e)) from None

    def _image(self, entry, element):
        keywords = {}
        for child in element:
            if _local(child.tag) != "FITSKeyword":
                continue
            card = {"value": _KeywordValue(child.attrib.get("value", "")), "comment": child.attrib.get("comment", "")}
            name = child.attrib.get("name", "")
            if name in keywords:
                keywords[name].append(card)
            else:
                keywords[name] = [card]
        # The attributes of the element as they are written; some of them in another form, in
        # their places; and what the element holds behind them.
        about = dict(element.attrib)
        try:
            about["geometry"] = tuple(int(part) for part in about.get("geometry", "").split(":"))
        except ValueError:
            about.setdefault("geometry", "")
        about["location"] = _location(about.get("location", ""))
        about["dtype"] = entry.dtype
        about["FITSKeywords"] = keywords
        about["XISFProperties"] = found = self._properties(entry._file, entry.properties,
                                                           [child for child in element if _local(child.tag) == "Property"])
        solved = [found[key] for key in found if key.startswith(_core._SOLUTION)]
        if solved:
            # What the solution was read with: write() leaves it out of an image that has not
            # these keywords and this size any more.
            context, cards = entry._file._context, _keywords(keywords)
            with context.lock:
                digest = _core._wcs_digest(context, cards, entry.width, entry.height, "bottom-up")
            for one in solved:
                one.solution_of = (digest, one["value"])
        if "compression" in about:
            about["compression"] = _compression(about["compression"])
        return about

    def _properties(self, file, properties, elements):
        if properties._count() != len(elements):
            raise InternalError("%s: %d Property elements, and %d properties" %
                                (file._name, len(elements), properties._count()))
        out = {}
        for index, element in enumerate(elements):
            found = self._property(file, properties, index, element)
            if found is not None and found["id"] not in out:     # (of two with one id the library has the first)
                out[found["id"]] = found
        return out

    def _property(self, file, properties, index, element):
        """One property as a dictionary: the attributes of its element, with ``value`` as a
        Python value. None for one that is not read."""
        found = _Property(element.attrib)
        identifier, kind, text, _, _ = properties._at(index)
        stored = properties._stored(index)

        def left_out(why):
            _core._warn("%s: property %s is left out: %s" % (file._name, identifier or "(without an id)", why))

        if not identifier or "id" not in found:
            return left_out("it has no id")
        try:
            if stored == _lib.PROPERTY_UNREAD:
                if kind == "String":   # (the library says why when the file is opened)
                    return left_out("its text cannot be read")
                return left_out("the type %s is not read" % (kind or "(none)"))
            if stored == _lib.PROPERTY_ARRAY:
                _, matrix = _core._element_of(kind)
                value = properties._read_block(index, kind)
                found["value"] = value
                found["dtype"] = value.dtype
                if matrix:
                    found["rows"], found["columns"] = value.shape
                else:
                    found["length"] = value.size
            elif "value" not in element.attrib and not (kind == "String" or text.strip()):
                return left_out("the type %s is not read" % (kind or "(none)"))
            else:
                if kind == "String":
                    value = text
                    if "location" not in found and "value" not in found:
                        # In the header, the text is what an XML reader makes of it, as the xisf
                        # package has it; the library keeps the line breaks the file has.
                        seen = element.text or ""
                        if seen != text and _line_feeds(text) == seen:
                            value = _Text(seen, text)
                elif kind == "TimePoint":
                    value = text
                elif kind == "Boolean":
                    value = text.strip().lower() in ("true", "1")
                else:
                    value = _core._scalar_value(kind, text)
                found["value"] = value
                found.as_read = (kind, properties._as_stored(index, text, stored), value, stored == _lib.PROPERTY_TEXT_BLOCK)
        except _core.Error as e:
            return left_out(str(e))
        if "location" in found:
            found["location"] = _location(found["location"])
            if "compression" in found:
                found["compression"] = _compression(found["compression"])
        return found

    # --- what a program sees ------------------------------------------------------------------

    def get_images_metadata(self):
        """What the header says about the images: a list with one dictionary per image.

        A dictionary has the attributes of the Image element under their names, as the text the
        file has ("id", "sampleFormat", "colorSpace", "bounds" and whatever else), except for
        three that are taken apart, and four entries that are added:

        ``geometry``
            ``(width, height, channels)``
        ``location``
            ``("attachment", position, size)``, ``["inline", encoding]`` or ``["embedded"]``;
            for a block in another file ``("path", path, identifier)`` or
            ``("url", URL, identifier)``: the path as the header has it (it begins with
            ``@header_dir/`` for a file beside the header), the identifier the block has in
            the index of a data blocks file, or None if the block is the whole file
        ``compression``
            ``(codec, uncompressed size, item size)`` if the pixels are compressed; the item
            size is None without byte shuffling
        ``dtype``
            the NumPy type of the samples; None for an image whose pixels are not read
        ``FITSKeywords``
            ``{name: [{"value": ..., "comment": ...}, ...]}``, a list for each name because a
            keyword may be there several times
        ``XISFProperties``
            ``{id: {"id": ..., "type": ..., "value": ...}}``, with ``comment`` and ``format``
            where the file has them. The value is a bool, an int, a float, a complex number or
            a str, or a NumPy array for a vector or a matrix: then the dictionary also has
            ``dtype`` and ``length``, or ``rows`` and ``columns``, and the ``location`` of the
            data.
        """
        return self._images

    def get_file_metadata(self):
        """The properties of the file (its Metadata element), as a dictionary like the
        ``XISFProperties`` of an image."""
        return self._about_file

    def get_metadata_xml(self):
        """The XML header of the file as an ``xml.etree.ElementTree.Element``."""
        return self._root

    def read_image(self, n=0, data_format="channels_last"):
        """The pixels of an image as a NumPy array.

        n
            the number of the image in the list :meth:`get_images_metadata` returns
        data_format
            "channels_last" (the default): ``[height, width, channels]``; "channels_first":
            ``[channels, height, width]``. The array has three dimensions also for one channel.
        """
        count = len(self._images)
        try:
            index = range(count)[n]
        except IndexError:
            if count == 0:
                raise ImageNumberError("%s: the file holds no image" % (self._path,)) from None
            raise ImageNumberError("%s: there is no image %s: the file has the images 0 to %d" %
                                   (self._path, n, count - 1)) from None
        about = self._images[index]
        try:
            with _core.File(self._path) as file:
                # (the file is opened again for the pixels: it has to be the file that was read)
                entry = file[index] if file.format == "xisf" and len(file) == count else None
                shape = about["geometry"]
                if entry is None or about["dtype"] != entry.dtype or \
                        (isinstance(shape, tuple) and len(shape) == 3 and shape != (entry.width, entry.height, entry.channels)):
                    raise NotXisfError("%s: the file is not the one it was when it was opened" % (file._name,))
                planar = entry.read(row_order=None, channels="first")
        except UnsupportedError as e:
            raise _as(NotSupportedError, e) from None
        except ImageIndexError as e:
            raise _as(ImageNumberError, e) from None
        except NotXisfError:
            raise
        except FormatError as e:
            raise _as(NotXisfError, e) from None
        if planar.ndim == 2:
            planar = planar[np.newaxis]
        if data_format == "channels_last":
            return planar.transpose(1, 2, 0)
        return planar

    @staticmethod
    def read(fname, n=0, image_metadata=None, xisf_metadata=None):
        """Opens a file and returns the pixels of one image as ``[height, width, channels]``:
        ``pixels = XISF.read("light.xisf")``.

        ``image_metadata`` and ``xisf_metadata``, if given, are dictionaries that are filled
        with the metadata of the image and of the file."""
        opened = XISF(fname)
        pixels = opened.read_image(n)          # (says so if there is no such image)
        if image_metadata is not None:
            image_metadata.update(opened.get_images_metadata()[n])
        if xisf_metadata is not None:
            xisf_metadata.update(opened.get_file_metadata())
        return pixels

    @staticmethod
    def write(fname, im_data, creator_app=None, image_metadata=None, xisf_metadata=None, codec=None, shuffle=False,
              level=None):
        """Writes an image to an XISF file, replacing a file of that name. Under a name that
        ends in ``.xish`` a distributed unit is written: the header there, the data blocks in
        the file of the same name that ends in ``.xisb`` (which is replaced as well).

        im_data
            the pixels: ``[height, width, channels]`` with 1 or 3 channels, any other 3-D array
            as ``[channels, height, width]``, or a 2-D array. uint8, uint16, uint32, uint64,
            float32 or float64.
        creator_app
            the program that writes the file (``XISF:CreatorApplication``); None: "Python"
            and its version.
        image_metadata
            a dictionary like those of :meth:`get_images_metadata`. Its ``FITSKeywords``,
            its ``XISFProperties`` and its ``id`` are written; the rest follows from the array.
        xisf_metadata
            properties of the file, like what :meth:`get_file_metadata` returns.
        codec
            "zlib", "lz4", "lz4hc" or "zstd"; None: no compression. A block that compression
            does not make smaller is written as it is.
        shuffle
            byte shuffling before compression, which is worth having for samples of more than
            one byte.
        level
            the compression level: zlib 1 to 9 (6 if None), lz4hc 1 to 12 (9), zstd 1 to 22 (3).

        Returns ``(bytes_written, codec)``: the size of the file (of a distributed unit: of
        its two files), and the codec as the file
        names it ("zstd+sh" with byte shuffling), None if the pixels are not compressed.
        """
        data = np.asarray(im_data)
        if data.ndim == 2:
            data = data[:, :, np.newaxis]
        elif data.ndim != 3:
            raise ValueError("an image is a 2-D or a 3-D array; this one has %d dimension(s)" % data.ndim)
        if data.dtype.name not in _core._SAMPLES:
            raise NotImplementedError("samples of the type %s are not written (uint8, uint16, uint32, uint64, float32 "
                                      "and float64 are)" % data.dtype.name)
        if codec is not None and codec not in _CODECS:
            raise NotImplementedError("%r is not a codec that is written (%s)" % (codec, ", ".join(_CODECS)))
        if image_metadata is None:
            image_metadata = {}
        # (the rule of the xisf package: 1 or 3 in the last place are channels)
        last = data.shape[2] in (1, 3)
        height, width = data.shape[:2] if last else data.shape[1:]
        keywords = _keywords(image_metadata.get("FITSKeywords"))
        properties = _properties(image_metadata.get("XISFProperties"), "XISFProperties")
        # An astrometric solution that was read from a file is that of the keywords and the size
        # it was read with. An image that was cropped since, or has other keywords, does not get
        # it: the solution is made from its WCS keywords, as for an image that has none.
        context = _core._Context.borrow()
        with context.lock:
            context.about(fname, reading=False)
            stale, own = _core._stale_solution(context, properties, keywords, width, height, "bottom-up")
        if stale:
            properties = properties._without(stale)
            _core._solution_left_out(context._name(), own, keywords, True)
        image = _core.Image(data, keywords=keywords, name=image_metadata.get("id", "image"),
                            channels="last" if last else "first", properties=properties,
                            # PixInsight's convention: the keywords go out as they came in
                            wcs_row_order="bottom-up")
        creator = " ".join(creator_app.split()) if isinstance(creator_app, str) else creator_app   # (a name is one line)
        try:
            _core.write(fname, image, format="xisf", codec=codec, shuffle=bool(shuffle),
                        level=(level or None) if codec in ("zlib", "lz4hc", "zstd") else None,
                        creator=creator if creator else "Python %s" % platform.python_version(),
                        file_properties=_properties(xisf_metadata, "xisf_metadata"), wcs=bool(stale), overwrite=True)
        except UnsupportedError as e:
            raise _as(NotSupportedError, e) from None
        with _core.File(fname) as file:
            stored = file[0].detail("compression")
            size = file.unit_size     # (of a distributed unit: the header file and its data blocks file)
        return size, (stored.split(":")[0] if stored else None)


# --- from the dictionaries a program holds to what xisfconv writes ---------------------------

def _unquoted(raw):
    """The text of a FITS string value."""
    text = raw.strip()
    text = text[1:-1] if len(text) >= 2 and text.endswith("'") else text[1:]
    return text.replace("''", "'").rstrip(" ")


def _raw_value(name, value):
    """How a keyword value given as text is written, as FITS formats values; None for text that
    is written as a string (or as the text of a COMMENT or HISTORY card)."""
    if name.strip().upper() in _core._COMMENTARY:
        return None
    raw = getattr(value, "raw", None)
    if isinstance(raw, str) and _KeywordValue(raw) == value:
        return raw    # as the file it was read from had it
    text = value.strip()
    if text in ("", "T", "F") or _core._REAL.match(text) or _core._COMPLEX.match(text):
        return text
    return None


def _keywords(fits_keywords):
    """{name: [{'value': ..., 'comment': ...}, ...]} as the keywords of an image."""
    out = _core.Keywords()
    if not fits_keywords:
        return out
    if not isinstance(fits_keywords, Mapping):
        raise TypeError("FITSKeywords is a dict {name: [{'value': ..., 'comment': ...}]}, not %s" %
                        type(fits_keywords).__name__)
    for name, entries in fits_keywords.items():
        if isinstance(entries, Mapping):
            entries = [entries]
        for entry in entries:
            value, comment = entry.get("value", ""), entry.get("comment", "") or ""
            raw = _raw_value(name, value) if isinstance(value, str) else None
            if raw is None:
                out.append(name, value, comment)
                continue
            card = _core._make_card(name, _core._parse_value(raw, _unquoted(raw)), comment)
            out._cards.append(card)
            out._raw.append(raw)
    return out


def _lenient(kind, value):
    """The value of a property as a program written for the ``xisf`` package may hold it, as
    the value this package writes: a number that is text, a complex number that is a pair."""
    if isinstance(value, np.ndarray) or kind is None:
        return value
    try:
        if isinstance(value, str):
            text = value.strip()
            if kind == "Boolean" and text.lower() in ("true", "false", "1", "0"):
                return text.lower() in ("true", "1")
            if kind in _core._WHOLE_TYPES:
                return int(text)
            if kind in _core._REAL_TYPES:
                return float(text)
        if kind in _core._COMPLEX_TYPES and isinstance(value, (tuple, list)) and len(value) == 2:
            return complex(float(value[0]), float(value[1]))
    except ValueError:
        pass
    if kind == "String" and value is None:
        return ""
    return value


def _properties(mapping, what):
    """{id: {'id': ..., 'type': ..., 'value': ...}} as the properties of an image or of a file."""
    out = _core.PropertyDict()
    if not mapping:
        return out
    if not isinstance(mapping, Mapping):
        raise TypeError("%s is a dict {id: {'id': ..., 'type': ..., 'value': ...}}, not %s" %
                        (what, type(mapping).__name__))
    for key, entry in mapping.items():
        if entry is None or entry is False:   # (what the xisf package has for a type it does not read)
            continue
        if not isinstance(entry, Mapping):
            raise TypeError("%s: property %s is a dict {'id': ..., 'type': ..., 'value': ...}, not %s" %
                            (what, key, type(entry).__name__))
        identifier, kind, value = entry.get("id", key), entry.get("type"), entry.get("value")
        comment, form = entry.get("comment", ""), entry.get("format", "")
        read = getattr(entry, "as_read", None)
        solved = getattr(entry, "solution_of", None)
        if read is not None and read[0] == (kind or "") and _core._same_value(value, read[2]):
            # as it was read: written as the file has it
            out.set(identifier, value, kind, comment, form)
            out._read[identifier] = (read[1], value, read[3])
            if not kind:
                out._untyped.add(identifier)
        else:
            value = _lenient(kind, value)
            raw = getattr(value, "raw", None)
            out.set(identifier, value, kind, comment, form)
            if kind == "String" and isinstance(value, str) and isinstance(raw, str) and _line_feeds(raw) == value:
                # a text of a header, with the line breaks of the file it was read from
                out._read[identifier] = (raw, value, False)
        if solved is not None and isinstance(identifier, str) and identifier.startswith(_core._SOLUTION):
            out._solution[identifier] = solved
    return out
