# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""xisfconv.xisf: the interface of the xisf package. What it returns is compared with what
that package returns for the same files, structure by structure; where the two are meant to
differ, the difference is what is tested."""

import base64
import hashlib
import os
import pickle
import platform
import re
import xml.etree.ElementTree as ET

import numpy as np
import pytest

import xisfconv
from xisfconv.xisf import XISF
from util import handmade, inline, sample, same

NS = "{http://www.pixinsight.com/xisf}"


def their_class():
    return pytest.importorskip("xisf").XISF


def differences(a, b, where=""):
    """What is not the same in two structures: types, keys and their order, values, arrays with
    their dtypes."""
    if isinstance(a, dict) and isinstance(b, dict):
        out = [] if list(a) == list(b) else ["%s: keys %r and %r" % (where, list(a), list(b))]
        for key in a:
            if key in b:
                out += differences(a[key], b[key], "%s/%s" % (where, key))
        return out
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        if type(a) is not type(b) or len(a) != len(b):
            return ["%s: %r and %r" % (where, a, b)]
        out = []
        for index, (x, y) in enumerate(zip(a, b)):
            out += differences(x, y, "%s[%d]" % (where, index))
        return out
    if isinstance(a, np.ndarray) or isinstance(b, np.ndarray):
        ok = isinstance(a, np.ndarray) and isinstance(b, np.ndarray) and a.dtype == b.dtype and a.shape == b.shape and \
            np.array_equal(a, b)
        return [] if ok else ["%s: arrays %r and %r" % (where, a, b)]
    if isinstance(a, str) and isinstance(b, str):
        return [] if a == b else ["%s: %r and %r" % (where, a, b)]
    if type(a) is not type(b) or a != b:
        return ["%s: %r (%s) and %r (%s)" % (where, a, type(a).__name__, b, type(b).__name__)]
    return []


def compare(path):
    """Opens the file with both and compares everything they return. Returns the two objects."""
    Theirs = their_class()
    mine, theirs = XISF(str(path)), Theirs(str(path))
    found = differences(mine.get_images_metadata(), theirs.get_images_metadata(), "images")
    found += differences(mine.get_file_metadata(), theirs.get_file_metadata(), "file")
    assert not found, found
    for n in range(len(theirs.get_images_metadata())):
        for data_format in ("channels_last", "channels_first"):
            a, b = mine.read_image(n, data_format), theirs.read_image(n, data_format)
            assert a.shape == b.shape and a.dtype == b.dtype and np.array_equal(a, b), (n, data_format)
    return mine, theirs


KEYWORDS = {
    "OBJECT": [{"value": "M 42", "comment": "the target"}],
    "EXPTIME": [{"value": "60.5", "comment": "seconds"}],
    "NCOMBINE": [{"value": "12", "comment": ""}],
    "COOLED": [{"value": "T", "comment": ""}],
    "HISTORY": [{"value": "", "comment": "first step"}, {"value": "", "comment": "second step"}],
}
LONG_TEXT = "a text that does not fit the header of the file, " * 80 + "and ends without a blank"


def their_properties():
    """Properties as the xisf package writes them (it has no way to write a Boolean it reads
    as true, and no comments)."""
    def entry(key, kind, value):
        return {"id": key, "type": kind, "value": value}
    return {
        "Instrument:Camera:Gain": entry("Instrument:Camera:Gain", "Float64", 120.5),
        "Instrument:Camera:XBinning": entry("Instrument:Camera:XBinning", "Int32", 2),
        "Instrument:Sensor:Bits": entry("Instrument:Sensor:Bits", "UInt8", 16),
        "Instrument:Telescope:FocalLength": entry("Instrument:Telescope:FocalLength", "Float32", 0.53),
        "Observation:Object:Name": entry("Observation:Object:Name", "String", "M 42 <the> \"great\" & nebula"),
        "Observation:Time:Start": entry("Observation:Time:Start", "TimePoint", "2026-10-06T18:30:00.250Z"),
        "Lab:Long": entry("Lab:Long", "String", LONG_TEXT),
        "Lab:Vector": entry("Lab:Vector", "F64Vector", np.array([1.5, -2.5, 3.25])),
        "Lab:Matrix": entry("Lab:Matrix", "F32Matrix", (np.arange(2000, dtype=np.float32) / 7).reshape(40, 50)),
        "Lab:Counts": entry("Lab:Counts", "UI16Vector", np.arange(5, dtype=np.uint16)),
        "Lab:Signed": entry("Lab:Signed", "I32Matrix", np.arange(-3, 3, dtype=np.int32).reshape(2, 3)),
    }


@pytest.mark.parametrize("dtype", ["uint8", "uint16", "uint32", "float32", "float64"])
@pytest.mark.parametrize("channels", [1, 3])
def test_files_of_the_xisf_package(tmp_path, dtype, channels):
    """Files that package wrote: the metadata and the pixels are returned as it returns them."""
    Theirs = their_class()
    data = sample(dtype, (13, 17, channels))
    for number, (codec, shuffle) in enumerate(((None, False), ("zlib", True), ("lz4", False), ("lz4hc", True),
                                                 ("zstd", True))):
        if codec == "zstd" and not xisfconv.codec_available("zstd"):
            continue
        path = tmp_path / ("theirs%d.xisf" % number)
        Theirs.write(str(path), data, creator_app="the tests", codec=codec, shuffle=shuffle,
                     image_metadata={"id": "frame", "FITSKeywords": KEYWORDS, "XISFProperties": their_properties()},
                     xisf_metadata={"Note:Author": {"id": "Note:Author", "type": "String", "value": "somebody"}})
        mine, theirs = compare(path)
        meta = mine.get_images_metadata()[0]
        assert meta["geometry"] == (17, 13, channels) and meta["dtype"] == np.dtype(dtype) and meta["id"] == "frame"
        assert meta["FITSKeywords"]["OBJECT"][0] == {"value": "M 42", "comment": "the target"}
        assert len(meta["XISFProperties"]) == 11 and same(mine.read_image(0), data)
        assert mine.get_file_metadata()["XISF:CreatorApplication"]["value"] == "the tests"


def test_files_of_xisfconv(tmp_path):
    """Files this library wrote: several images, checksums, compressed properties, comments."""
    their_class()
    first = xisfconv.PropertyDict({"Lab:Vector": np.linspace(0, 1, 3000), "Lab:Text": "some text", "Lab:Half": 0.5,
                                   "Lab:Count": 7, "Lab:Wide": np.arange(12, dtype=np.int64).reshape(3, 4)})
    first.set("Lab:Gain", 1.25, "Float32", "of the camera", "%.2f")
    images = [xisfconv.Image(sample("uint16", (11, 14)), name="first", properties=first,
                             keywords={"OBJECT": ("M 31", "target"), "EXPTIME": 30.5, "COOLED": True}),
              xisfconv.Image(sample("float32", (9, 12, 3)), name="second"),
              xisfconv.Image(sample("uint8", (5, 6)), name="third", properties={"Lab:Only": "here"})]
    for number, options in enumerate((dict(), dict(codec="zlib", checksum="sha1"), dict(codec="lz4hc", checksum="sha512"),
                                      dict(codec="zstd", shuffle=False, level=7))):
        if options.get("codec") == "zstd" and not xisfconv.codec_available("zstd", writing=True):
            continue
        path = tmp_path / ("mine%d.xisf" % number)
        xisfconv.write(path, images, file_properties={"Note:Author": "somebody"}, creator="the tests", **options)
        mine, _ = compare(path)
        metas = mine.get_images_metadata()
        assert [meta["id"] for meta in metas] == ["first", "second", "third"]
        assert [meta["geometry"] for meta in metas] == [(14, 11, 1), (12, 9, 3), (6, 5, 1)]
        assert metas[0]["XISFProperties"]["Lab:Gain"] == {"id": "Lab:Gain", "type": "Float32", "comment": "of the camera",
                                                           "format": "%.2f", "value": 1.25}
        assert list(metas[2]["XISFProperties"]) == ["Lab:Only"] and metas[1]["XISFProperties"] == {}
        assert metas[0]["FITSKeywords"]["OBJECT"][0]["value"] == "M 31"
        assert ("checksum" in metas[0]) == ("checksum" in options)
        if "codec" in options:
            vector = metas[0]["XISFProperties"]["Lab:Vector"]
            assert vector["location"][0] == "attachment" and vector["compression"][0].startswith(options["codec"])
        assert same(mine.read_image(1), images[1].data) and same(mine.read_image(2)[:, :, 0], images[2].data)


def test_read(tmp_path):
    Theirs = their_class()
    path = tmp_path / "one.xisf"
    data = sample("uint16", (7, 9, 3))
    Theirs.write(str(path), data, image_metadata={"FITSKeywords": KEYWORDS, "XISFProperties": their_properties()},
                 xisf_metadata={})
    image_meta, file_meta, their_image_meta, their_file_meta = {}, {}, {}, {}
    mine = XISF.read(str(path), 0, image_meta, file_meta)
    theirs = Theirs.read(str(path), 0, their_image_meta, their_file_meta)
    assert same(mine, theirs) and same(mine, data)
    assert not differences(image_meta, their_image_meta) and not differences(file_meta, their_file_meta)
    assert same(XISF.read(str(path)), data) and same(XISF.read(path, n=0), data)        # a path object is a name too
    assert same(XISF.read(str(path), image_metadata=image_meta), data)
    # an object keeps what it read, and returns the same structures every time
    xisf = XISF(str(path))
    assert xisf.get_images_metadata() is xisf.get_images_metadata() and xisf.get_file_metadata() is xisf.get_file_metadata()
    array = xisf.read_image()
    assert array.shape == (7, 9, 3) and array.flags.writeable and xisf.read_image(0, "channels_first").shape == (3, 7, 9)
    assert xisf.read_image(-1).shape == (7, 9, 3)


PIXELS = np.arange(12, dtype="<u2").reshape(3, 4)
IMAGE = '<Image id="odd" geometry="4:3:1" sampleFormat="UInt16" colorSpace="Gray" %s' % inline(PIXELS)


def test_what_is_read_differently(tmp_path):
    """The differences the documentation of the module names."""
    elements = (
        IMAGE +
        '<FITSKeyword name="OBJECT" value="\'O\'\'Neil  \'" comment="a name"/>'
        '<FITSKeyword name="EXPTIME" value="1.0E+03" comment=""/>'
        '<FITSKeyword name="FILTER" value="\'123     \'" comment="text that looks like a number"/>'
        '<Property id="A:Whole" type="Float64" value="3"/>'
        '<Property id="A:One" type="Boolean" value="1"/>'
        '<Property id="A:True" type="Boolean" value="true"/>'
        '<Property id="A:Zero" type="Boolean" value="0"/>'
        '<Property id="A:Complex" type="Complex32" value="(1.5,-2)"/>'
        '<Property id="A:Nan" type="Float32" value="nan"/>'
        '<Property id="A:Inf" type="Float64" value="-inf"/>'
        '<Property id="A:Empty" type="String"></Property>'
        '<Property id="A:Lines" type="String">one\r\ntwo\r\nthree</Property>'
        '<Property id="A:Bytes" type="ByteArray" length="3" ' + inline(np.array([1, 2, 255], np.uint8)) + '</Property>'
        '<Property id="A:Pairs" type="C32Vector" length="2" ' + inline(np.array([1 + 2j, 3 - 4j], "<c8")) + '</Property>'
        '<Property id="A:Short" type="Vector" ' + inline(np.array([0.5, 1.5], "<f8")) + '</Property>'
        '<Property id="A:Table" type="Table"><Structure/><Row/></Property>'
        '<Property id="A:Odd" type="Q64Vector" length="1" ' + inline(np.array([1.0], "<f8")) + '</Property>'
        '<Property id="A:Damaged" type="F64Vector" length="1" checksum="sha1:' + "0" * 40 + '" '
        + inline(np.array([1.0], "<f8")) + '</Property>'
        '<Property id="A:Big" type="UInt64" value="18446744073709551615"/>'
        '</Image>')
    path = handmade(tmp_path / "odd.xisf", elements)
    with pytest.warns(xisfconv.XisfconvWarning) as caught:
        xisf = XISF(str(path))
    said = sorted(str(warning.message) for warning in caught)
    assert all(warning.filename == __file__ for warning in caught)      # blamed on the caller, not on the package
    assert len(said) == 3 and "A:Damaged is left out" in said[0] and "checksum" in said[0]
    assert "A:Odd is left out: the type Q64Vector is not read" in said[1] and "A:Table is left out" in said[2]
    meta = xisf.get_images_metadata()[0]
    properties = meta["XISFProperties"]
    assert list(properties) == ["A:Whole", "A:One", "A:True", "A:Zero", "A:Complex", "A:Nan", "A:Inf", "A:Empty",
                                "A:Lines", "A:Bytes", "A:Pairs", "A:Short", "A:Big"]
    value = {key: entry["value"] for key, entry in properties.items()}
    assert value["A:Whole"] == 3.0 and type(value["A:Whole"]) is float           # the xisf package: the int 3
    assert value["A:One"] is True and value["A:True"] is True and value["A:Zero"] is False
    assert value["A:Complex"] == complex(1.5, -2) and type(value["A:Complex"]) is complex
    assert np.isnan(value["A:Nan"]) and value["A:Inf"] == float("-inf") and value["A:Empty"] == ""
    assert value["A:Big"] == 2 ** 64 - 1
    assert same(value["A:Bytes"], np.array([1, 2, 255], np.uint8)) and properties["A:Bytes"]["length"] == 3
    assert properties["A:Bytes"]["dtype"] == np.uint8 and properties["A:Bytes"]["location"] == ["inline", "base64"]
    assert value["A:Pairs"].dtype == np.complex64 and value["A:Pairs"].tolist() == [1 + 2j, 3 - 4j]
    assert same(value["A:Short"], np.array([0.5, 1.5])) and properties["A:Short"]["length"] == 2    # the block says how long
    # a String: line breaks as an XML reader gives them, and as the file has them
    assert value["A:Lines"] == "one\ntwo\nthree" and value["A:Lines"].raw == "one\r\ntwo\r\nthree"
    assert isinstance(value["A:Lines"], str) and type(value["A:Empty"]) is str
    # a keyword value: as the xisf package gives it, and as the file wrote it
    keywords = meta["FITSKeywords"]
    assert keywords["OBJECT"][0]["value"] == "O''Neil" and keywords["OBJECT"][0]["value"].raw == "'O''Neil  '"
    assert keywords["EXPTIME"][0]["value"] == "1.0E+03" and keywords["FILTER"][0]["value"] == "123"
    assert isinstance(keywords["OBJECT"][0]["value"], str) and keywords["OBJECT"][0]["comment"] == "a name"
    for text in (value["A:Lines"], keywords["OBJECT"][0]["value"]):
        again = pickle.loads(pickle.dumps(text))
        assert again == text and again.raw == text.raw
    # no Metadata element: no properties of the file
    assert xisf.get_file_metadata() == {}
    assert same(xisf.read_image(0), PIXELS[:, :, None]) and meta["dtype"] == np.uint16
    assert meta["location"] == ["inline", "base64"] and meta["geometry"] == (4, 3, 1) and meta["sampleFormat"] == "UInt16"

    # what the xisf package gets wrong, or does not read at all
    big = np.arange(12, dtype=">u4").reshape(1, 3, 4)
    normal = np.arange(24, dtype="<u2").reshape(3, 4, 2)          # pixel after pixel: the "Normal" storage
    wide = (np.arange(12, dtype="<u8") + 2 ** 63).reshape(3, 4)
    zipped = np.arange(600, dtype="<u2").reshape(20, 30)
    import zlib
    halves = [zlib.compress(zipped.tobytes()[:600]), zlib.compress(zipped.tobytes()[600:])]
    embedded = '<Data encoding="base64">%s</Data>' % base64.b64encode(PIXELS.tobytes()).decode()
    path = handmade(tmp_path / "more.xisf", (
        '<Image geometry="4:3:1" sampleFormat="UInt32" byteOrder="big" colorSpace="Gray" ' + inline(big) + '</Image>'
        '<Image geometry="4:3:2" sampleFormat="UInt16" pixelStorage="Normal" colorSpace="Gray" ' + inline(normal) + '</Image>'
        '<Image geometry="4:3:1" sampleFormat="UInt64" colorSpace="Gray" ' + inline(wide) + '</Image>'
        '<Image geometry="30:20:1" sampleFormat="UInt16" colorSpace="Gray" location="attachment:@0" '
        'compression="zlib:1200" subblocks="%d,600:%d,600"/>' % (len(halves[0]), len(halves[1])) +
        '<Image geometry="4:3:1" sampleFormat="UInt16" colorSpace="Gray" location="embedded">' + embedded + '</Image>'
        '<Image geometry="4:3:1" sampleFormat="Complex32" colorSpace="Gray" ' + inline(np.zeros(12, "<c8")) + '</Image>'
        '<Metadata/><Property id="Root:Level" type="Int32" value="5"/>'), [b"".join(halves)])
    xisf = XISF(path)
    metas = xisf.get_images_metadata()
    assert [meta["dtype"] for meta in metas] == [np.dtype("uint32"), np.dtype("uint16"), np.dtype("uint64"),
                                                 np.dtype("uint16"), np.dtype("uint16"), None]
    assert same(xisf.read_image(0), big.astype("<u4").transpose(1, 2, 0))
    assert same(xisf.read_image(1), normal) and same(xisf.read_image(1, "channels_first"), normal.transpose(2, 0, 1))
    assert same(xisf.read_image(2)[:, :, 0], wide) and same(xisf.read_image(3)[:, :, 0], zipped)
    assert same(xisf.read_image(4)[:, :, 0], PIXELS) and metas[4]["location"] == ["embedded"]
    assert metas[3]["compression"] == ("zlib", 1200, None) and metas[3]["location"][0] == "attachment"
    assert metas[3]["subblocks"] == "%d,600:%d,600" % (len(halves[0]), len(halves[1]))
    with pytest.raises(NotImplementedError) as unsupported:
        xisf.read_image(5)
    assert isinstance(unsupported.value, xisfconv.UnsupportedError) and "Complex32" in str(unsupported.value)
    assert xisf.get_file_metadata() == {"Root:Level": {"id": "Root:Level", "type": "Int32", "value": 5}}


def test_a_damaged_file_is_not_read(tmp_path):
    digest = hashlib.sha1(PIXELS.tobytes()).hexdigest()
    good = handmade(tmp_path / "good.xisf", IMAGE.replace('location=', 'checksum="sha1:%s" location=' % digest) + '</Image>')
    assert same(XISF(good).read_image(0)[:, :, 0], PIXELS)
    bad = handmade(tmp_path / "bad.xisf",
                   IMAGE.replace('location=', 'checksum="sha1:%s" location=' % digest.replace(digest[0], "f" if digest[0] != "f" else "0", 1))
                   + '</Image>')
    xisf = XISF(bad)                                  # the header is read
    assert xisf.get_images_metadata()[0]["checksum"].startswith("sha1:")
    with pytest.raises(xisfconv.ChecksumError):
        xisf.read_image(0)


def test_errors_are_also_those_of_the_xisf_package(tmp_path):
    (tmp_path / "text.xisf").write_text("this is not an image, and long enough to have a header\n" * 3)
    with pytest.raises(ValueError) as error:
        XISF(str(tmp_path / "text.xisf"))
    assert isinstance(error.value, xisfconv.FormatError) and "text.xisf" in str(error.value)
    xisfconv.write(tmp_path / "image.fits", sample("uint8", (4, 4)))
    with pytest.raises(ValueError, match="not an XISF file") as error:
        XISF(str(tmp_path / "image.fits"))
    assert isinstance(error.value, xisfconv.FormatError)
    handmade(tmp_path / "broken.xisf", IMAGE)          # an element that is not closed
    with pytest.raises(ValueError) as error:
        XISF(str(tmp_path / "broken.xisf"))
    assert isinstance(error.value, xisfconv.FormatError)
    with pytest.raises(FileNotFoundError) as error:
        XISF(str(tmp_path / "no-such.xisf"))
    assert isinstance(error.value, xisfconv.Error)
    with pytest.raises(FileNotFoundError):
        XISF.read(str(tmp_path / "no-such.xisf"))

    path = tmp_path / "two.xisf"
    xisfconv.write(path, [sample("uint8", (4, 5)), sample("uint8", (6, 7))])
    xisf = XISF(str(path))
    for number in (2, 9, -3):
        with pytest.raises(ValueError, match="the file has the images 0 to 1") as error:
            xisf.read_image(number)
        assert isinstance(error.value, IndexError) and isinstance(error.value, xisfconv.ImageIndexError)
        with pytest.raises(ValueError):
            XISF.read(str(path), number)
        with pytest.raises(ValueError):
            XISF.read(str(path), number, {}, {})
    assert xisf.read_image(-1).shape == (6, 7, 1) and xisf.read_image(1).shape == (6, 7, 1)
    with pytest.raises(TypeError):
        xisf.read_image("first")
    # a file that holds no image
    empty = XISF(handmade(tmp_path / "empty.xisf", "<Metadata/>"))
    assert empty.get_images_metadata() == [] and empty.get_file_metadata() == {}
    with pytest.raises(ValueError, match="holds no image"):
        empty.read_image(0)
    # the pixels are read from the file that was opened, or not at all: another file under its
    # name is not that file
    other = tmp_path / "other.xisf"
    xisfconv.write(other, [sample("uint8", (4, 5)), sample("uint16", (6, 7))])
    os.replace(other, path)
    assert xisf.read_image(0).shape == (4, 5, 1)             # the same image as far as anyone can tell
    with pytest.raises(ValueError, match="not the one it was") as error:
        xisf.read_image(1)                                   # other samples
    assert isinstance(error.value, xisfconv.FormatError)
    xisfconv.write(path, sample("uint8", (4, 5)), format="fits", overwrite=True)
    with pytest.raises(ValueError, match="not the one it was"):
        xisf.read_image(0)
    os.remove(path)
    with pytest.raises(FileNotFoundError):
        xisf.read_image(0)


def test_the_header(tmp_path):
    path = tmp_path / "header.xisf"
    xisfconv.write(path, sample("uint16", (4, 5)), keywords={"OBJECT": "M 1"}, properties={"Lab:Value": 1})
    xisf = XISF(str(path))
    root = xisf.get_metadata_xml()
    assert root.tag == NS + "xisf" and root.attrib["version"] == "1.0" and root is xisf.get_metadata_xml()
    image = root.find(NS + "Image")
    assert image.attrib["geometry"] == "5:4:1" and image.find(NS + "FITSKeyword").attrib["name"] == "OBJECT"
    assert root.find(NS + "Metadata") is not None
    text = ET.tostring(root, encoding="unicode")
    assert text.startswith("<xisf ") and "<Image " in text and "ns0:" not in text       # as with the xisf package
    assert ET.fromstring(text).find(NS + "Image/" + NS + "Property").attrib["id"] == "Lab:Value"


CODECS = [(None, False, None), ("zlib", False, None), ("zlib", True, 9), ("lz4", False, None), ("lz4", True, 5),
          ("lz4hc", True, None), ("lz4hc", False, 12), ("zstd", True, None), ("zstd", False, 19)]


@pytest.mark.parametrize("dtype", ["uint8", "uint16", "uint32", "float32", "float64"])
def test_what_is_written_is_read_by_the_xisf_package(tmp_path, dtype):
    Theirs = their_class()
    rng = np.random.default_rng(4)
    smooth = np.add.outer(np.arange(40), np.arange(50)) * 3 % 200 + rng.integers(0, 3, (40, 50))
    for channels in (1, 3):
        data = np.stack([smooth + 5 * c for c in range(channels)], axis=2)
        data = (data / 256.0).astype(dtype) if np.dtype(dtype).kind == "f" else data.astype(dtype)
        for number, (codec, shuffle, level) in enumerate(CODECS):
            if codec == "zstd" and not xisfconv.codec_available("zstd", writing=True):
                continue
            mine, theirs = tmp_path / "mine.xisf", tmp_path / "theirs.xisf"
            arguments = dict(creator_app="the tests", codec=codec, shuffle=shuffle, level=level)
            written = XISF.write(str(mine), data, image_metadata={"id": "frame", "FITSKeywords": KEYWORDS,
                                                                  "XISFProperties": their_properties()},
                                 xisf_metadata={"Note:Author": {"id": "Note:Author", "type": "String", "value": "me"}},
                                 **arguments)
            expected = Theirs.write(str(theirs), data, image_metadata={"XISFProperties": {}}, xisf_metadata={}, **arguments)
            # (bytes of one byte are not shuffled, and the file does not say they were)
            shuffled = shuffle and data.itemsize > 1
            assert written[0] == os.path.getsize(mine), (dtype, channels, codec, shuffle)
            assert written[1] == (expected[1] if shuffled or not shuffle else codec), (dtype, channels, codec, shuffle)
            if codec is not None:
                assert written[1] == codec + ("+sh" if shuffled else "") and written[0] < data.nbytes + 60000
            xisf = Theirs(str(mine))
            meta = xisf.get_images_metadata()[0]
            assert same(xisf.read_image(0), data) and meta["id"] == "frame" and meta["geometry"] == (50, 40, channels)
            assert meta["colorSpace"] == ("RGB" if channels == 3 else "Gray")
            assert {name: [(card["value"], card["comment"]) for card in cards] for name, cards in meta["FITSKeywords"].items()} == \
                {name: [(card["value"], card["comment"]) for card in cards] for name, cards in KEYWORDS.items()}
            want = their_properties()
            assert list(meta["XISFProperties"]) == list(want)
            for key, entry in want.items():
                got = meta["XISFProperties"][key]
                assert got["type"] == entry["type"] and not differences(got["value"], entry["value"]), key
            file_meta = xisf.get_file_metadata()
            assert file_meta["Note:Author"]["value"] == "me" and file_meta["XISF:CreatorApplication"]["value"] == "the tests"
            assert file_meta["XISF:CreatorModule"]["value"] == "xisfconv " + xisfconv.library_version()
            if codec is not None:
                assert file_meta["XISF:CompressionCodecs"]["value"] == codec + ("+sh" if shuffle else "")
                # the long text and the large matrix are data blocks, compressed like the pixels
                # (a block that does not get smaller is stored as it is: the text always does)
                for key in ("Lab:Long", "Lab:Matrix"):
                    entry = meta["XISFProperties"][key]
                    assert entry["location"][0] == "attachment", key
                    assert key != "Lab:Long" or "compression" in entry
                    assert "compression" not in entry or entry["compression"][0].startswith(codec), key
            assert "XISF:CompressionLevel" not in file_meta        # (which is no level of a codec)
            assert xisfconv.verify(mine).verdict == "ok"
            # and the two read it alike
            compare(mine)


def element_list(path, name):
    """The elements of that name in the first image of a file: their attributes, and the data
    of each that has some."""
    with xisfconv.open(path) as file:
        root = ET.fromstring(file.header_text.encode())
        properties = file[0].properties
        elements = [child for child in root.find(NS + "Image") if child.tag == NS + name]
        out = []
        for index, child in enumerate(elements):
            attributes = {key: value for key, value in child.attrib.items() if key not in ("location", "compression")}
            data = None
            if name == "Property":
                _, kind, text, _, block = properties._at(index)
                data = properties._read_block(index, kind) if block else text
                data = data.tobytes() if isinstance(data, np.ndarray) else data
            out.append((attributes, data))
        return out


def test_a_file_goes_through_unchanged(tmp_path):
    """Read and written again through this interface: the keywords as they were written,
    strings with their quotes and their blanks, and the properties with their types, comments
    and formats, and the line breaks of their texts."""
    cards = [("OBJECT", "'M 42    '", "the target"), ("OBSERVER", "'O''Neil'", ""), ("FILTER", "'7       '", "a string"),
             ("EXPTIME", "6.0E+01", "[s]"), ("NCOMBINE", "12", ""), ("COOLED", "T", ""), ("GAIN", "120.", ""),
             ("EMPTY", "", "no value"), ("CTYPE1", "'RA---TAN'", ""), ("CTYPE2", "'DEC--TAN'", ""), ("CRVAL1", "83.82", ""),
             ("CRVAL2", "-5.39", ""), ("CRPIX1", "5.5", ""), ("CRPIX2", "4.25", ""), ("CD1_1", "-2.0E-04", ""),
             ("CD1_2", "1.0E-05", ""), ("CD2_1", "1.0E-05", ""), ("CD2_2", "2.0E-04", ""),
             ("COMMENT", "", "a remark"), ("HISTORY", "", "step one"), ("HISTORY", "", "step two")]
    properties = (
        '<Property id="A:Gain" type="Float32" value="0.1" comment="of the camera" format="%.2f"/>'
        '<Property id="A:Count" type="UInt16" value="65535"/>'
        '<Property id="A:On" type="Boolean" value="true"/>'
        '<Property id="A:Start" type="TimePoint" value="2026-10-06T18:30:00.250Z"/>'
        '<Property id="A:Phase" type="Complex64" value="(1.5,-2.5)"/>'
        '<Property id="A:Name" type="String">M 42 &lt;the&gt; nebula</Property>'
        '<Property id="A:Lines" type="String" comment="from Windows">one\r\ntwo\r\nthree</Property>'
        '<Property id="A:Whole" type="Float64" value="2000"/>'
        '<Property id="A:Spelled" type="Float32" value="1.50E+02"/>'
        '<Property id="A:Wide" type="Float128" value="1.5"/>'
        '<Property id="A:Odd" type="Int32" value="not a number"/>'
        '<Property id="A:Vector" type="F32Vector" length="3" ' + inline(np.array([1.5, 2.5, 1e-20], "<f4")) + '</Property>'
        '<Property id="A:Matrix" type="I16Matrix" rows="2" columns="2" ' + inline(np.array([-1, 2, -3, 4], "<i2")) + '</Property>'
        '<Property id="A:Pairs" type="C64Vector" length="1" ' + inline(np.array([1 + 2j], "<c16")) + '</Property>'
        '<Property id="A:Bytes" type="ByteArray" length="2" ' + inline(np.array([7, 250], np.uint8)) + '</Property>'
        '<Property id="A:Many" type="F64Vector" length="900" location="attachment:@0"/>'
        '<Property id="PCL:AstrometricSolution:ProjectionSystem" type="String">Gnomonic</Property>')
    many = (np.arange(900, dtype="<f8") / 3).tobytes()
    elements = (IMAGE + "".join('<FITSKeyword name="%s" value="%s" comment="%s"/>' % card for card in cards) + properties +
                '</Image><Metadata><Property id="Note:Author" type="String">somebody</Property>'
                '<Property id="XISF:CreatorOS" type="String">an abacus</Property></Metadata>')
    source = handmade(tmp_path / "source.xisf", elements, [many])
    xisf = XISF(source)
    meta = xisf.get_images_metadata()[0]
    assert len(meta["XISFProperties"]) == 17 and sum(len(entries) for entries in meta["FITSKeywords"].values()) == len(cards)
    assert meta["XISFProperties"]["A:Whole"]["value"] == 2000.0 and type(meta["XISFProperties"]["A:Whole"]["value"]) is float
    assert meta["XISFProperties"]["A:Wide"]["value"] == 1.5 and meta["XISFProperties"]["A:Odd"]["value"] == "not a number"
    for number, (codec, shuffle) in enumerate(((None, False), ("lz4hc", True), ("zlib", False))):
        copy = tmp_path / ("copy%d.xisf" % number)
        XISF.write(copy, xisf.read_image(0), image_metadata=meta, xisf_metadata=xisf.get_file_metadata(), codec=codec,
                   shuffle=shuffle)
        assert element_list(copy, "FITSKeyword") == element_list(source, "FITSKeyword")
        assert element_list(copy, "Property") == element_list(source, "Property")
        again = XISF(copy)
        assert same(again.read_image(0), xisf.read_image(0)) and again.get_images_metadata()[0]["id"] == "odd"
        assert again.get_file_metadata()["Note:Author"]["value"] == "somebody"
        assert "XISF:CreatorOS" not in again.get_file_metadata()        # how a file is stored is the writer's to say
        assert again.get_images_metadata()[0]["XISFProperties"]["A:Lines"]["value"].raw == "one\r\ntwo\r\nthree"
        if codec:
            assert again.get_images_metadata()[0]["XISFProperties"]["A:Many"]["compression"][0].startswith(codec)
    # what was read is not changed by writing it
    assert xisf.get_file_metadata()["XISF:CreatorOS"]["value"] == "an abacus" and len(xisf.get_file_metadata()) == 2
    # a value that is replaced by plain text is written as text says
    meta["FITSKeywords"]["OBJECT"][0]["value"] = "M 43"
    meta["FITSKeywords"]["FILTER"][0]["value"] = "8"
    meta["FITSKeywords"]["NEW"] = [{"value": "2.5", "comment": "added"}]
    meta["XISFProperties"]["A:Lines"]["value"] = "four\nfive"
    meta["XISFProperties"]["A:Whole"]["value"] = 2001.0                  # a new number is written as numbers are
    meta["XISFProperties"]["A:Spelled"] = dict(meta["XISFProperties"]["A:Spelled"])   # as is a copy of what was read
    del meta["XISFProperties"]["A:Odd"]
    XISF.write(tmp_path / "changed.xisf", xisf.read_image(0), image_metadata=meta)
    with xisfconv.open(tmp_path / "changed.xisf") as file:
        header = file.header_text
        assert '<Property id="A:Whole" type="Float64" value="2001.0"/>' in header
        assert '<Property id="A:Spelled" type="Float32" value="150.0"/>' in header
        assert '<Property id="A:Wide" type="Float128" value="1.5"/>' in header
        assert '<FITSKeyword name="OBJECT" value="\'M 43    \'" comment="the target"/>' in header     # a FITS string
        assert '<FITSKeyword name="FILTER" value="8" comment="a string"/>' in header     # text that is a number is one
        assert '<FITSKeyword name="NEW" value="2.5" comment="added"/>' in header
        assert file[0].properties["A:Lines"] == "four\nfive" and file[0].properties.comment("A:Lines") == "from Windows"
        assert file[0].keywords["OBSERVER"] == "O'Neil"


def test_headers_as_other_programs_write_them(tmp_path):
    """Text beyond ASCII under the encoding name the xisf package writes, a String with both a
    value and a text, line breaks written as character references, and a header that brings
    definitions of its own."""
    Theirs = their_class()
    path = tmp_path / "umlaut.xisf"
    data = sample("uint16", (5, 6, 1))
    Theirs.write(str(path), data, xisf_metadata={}, image_metadata={
        "FITSKeywords": {"OBJECT": [{"value": "NGC 7000", "comment": "Nordamerikanebel, 20°"}]},
        "XISFProperties": {"Observation:Object:Name": {"id": "Observation:Object:Name", "type": "String", "value": "Jürgen ☃"}}})
    assert b"encoding='utf8'" in path.read_bytes()[:80]
    xisf = XISF(path)
    meta = xisf.get_images_metadata()[0]
    assert meta["FITSKeywords"]["OBJECT"][0]["comment"] == "Nordamerikanebel, 20°"
    assert meta["XISFProperties"]["Observation:Object:Name"]["value"] == "Jürgen ☃" and same(xisf.read_image(0), data)
    XISF.write(tmp_path / "again.xisf", xisf.read_image(0), image_metadata=meta)      # and written again
    again = XISF(tmp_path / "again.xisf").get_images_metadata()[0]
    assert again["FITSKeywords"]["OBJECT"][0]["comment"] == "Nordamerikanebel, 20°"
    assert again["XISFProperties"]["Observation:Object:Name"]["value"] == "Jürgen ☃"

    source = handmade(tmp_path / "forms.xisf", IMAGE +
                      '<Property id="S:Both" type="String" value="the attribute">the text</Property>'
                      '<Property id="S:References" type="String">a&#13;&#10;b</Property>'
                      '<Property id="S:Data" type="String"><![CDATA[<tags> & such]]> and more</Property>'
                      '<Property id="S:Blanks" type="String">  kept as they are  </Property></Image><Metadata/>')
    value = {key: entry["value"] for key, entry in XISF(source).get_images_metadata()[0]["XISFProperties"].items()}
    assert value["S:Both"] == "the attribute"                 # the xisf package: the text
    assert value["S:References"] == "a\r\nb" and not hasattr(value["S:References"], "raw")     # written so: meant so
    assert value["S:Data"] == "<tags> & such and more" and value["S:Blanks"] == "  kept as they are  "
    theirs = {key: entry["value"] for key, entry in Theirs(str(source)).get_images_metadata()[0]["XISFProperties"].items()}
    assert {key: value[key] for key in ("S:References", "S:Data", "S:Blanks")} == \
        {key: theirs[key] for key in ("S:References", "S:Data", "S:Blanks")}

    # a document type declaration: attributes and text that are not in the file, of any size
    pixels = 'location="inline:base64">' + base64.b64encode(PIXELS.tobytes()).decode()
    header = ('<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE xisf [<!ENTITY a "aaaaaaaaaa"><!ENTITY b "&a;&a;&a;&a;&a;">'
              '<!ATTLIST Image compression CDATA "zlib:&b;">]>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">'
              '<Image geometry="4:3:1" sampleFormat="UInt16" colorSpace="Gray" ' + pixels + '</Image></xisf>').encode()
    (tmp_path / "doctype.xisf").write_bytes(b"XISF0100" + len(header).to_bytes(4, "little") + bytes(4) + header)
    assert same(xisfconv.read(tmp_path / "doctype.xisf"), PIXELS)          # the library reads past it
    with pytest.raises(ValueError, match="DOCTYPE") as error:
        XISF(tmp_path / "doctype.xisf")
    assert isinstance(error.value, xisfconv.FormatError)
    # the word in a comment, in a text and behind a comment is no declaration, or is one
    source = handmade(tmp_path / "word.xisf", '<!-- <!DOCTYPE html> -->' + IMAGE +
                      '<Property id="W:Text" type="String"><![CDATA[<!DOCTYPE html><p>x</p>]]></Property></Image>')
    assert XISF(source).get_images_metadata()[0]["XISFProperties"]["W:Text"]["value"] == "<!DOCTYPE html><p>x</p>"
    # (a comment begins with four characters and ends with three others: "<!-->" is open)
    source = handmade(tmp_path / "open.xisf", IMAGE + '<!--><Property id="W:Hidden" type="Int32" value="1"/>-->'
                      '<Property id="W:Seen" type="Int32" value="2"/></Image>')
    assert list(XISF(source).get_images_metadata()[0]["XISFProperties"]) == ["W:Seen"]
    with xisfconv.open(source) as file:
        assert list(file[0].properties) == ["W:Seen"]
    # (and a processing instruction has a name)
    source = handmade(tmp_path / "nameless.xisf", IMAGE + '<Property id="W:A" type="String">a<?>b</Property>'
                      '<Property id="W:B" type="String">q<?z?>r</Property></Image>')
    for reader in (xisfconv.open, XISF):
        with pytest.raises(xisfconv.FormatError, match="processing instruction"):
            reader(source)
    hidden = header.replace(b"<!DOCTYPE", b"<!-- a remark --> <?target data?>\n<!DOCTYPE")
    (tmp_path / "hidden.xisf").write_bytes(b"XISF0100" + len(hidden).to_bytes(4, "little") + bytes(4) + hidden)
    with pytest.raises(ValueError, match="DOCTYPE"):
        XISF(tmp_path / "hidden.xisf")
    # a byte order mark, and a header that is padded with zeros
    header = "\ufeff".encode() + header.replace(header[header.index(b"<!DOCTYPE"):header.index(b"<xisf")], b"") + bytes(40)
    (tmp_path / "mark.xisf").write_bytes(b"XISF0100" + len(header).to_bytes(4, "little") + bytes(4) + header)
    assert same(XISF(tmp_path / "mark.xisf").read_image(0)[:, :, 0], PIXELS)


def test_line_ends_of_texts(tmp_path):
    """A text of a header with its line ends as they are written, a long one too; a carriage
    return that is written as a character reference; and a text in a data block. Each is read
    again by an XML reader, after being written, as the same text as before."""
    Theirs = their_class()
    long_text = "\r\n".join("line %04d of a text that PixInsight wrote on Windows" % n for n in range(70))
    assert len(long_text) > 3072
    block = "one\r\ntwo\rthree"
    source = handmade(tmp_path / "lines.xisf", IMAGE +
                      '<Property id="L:Long" type="String">' + long_text + '</Property>'
                      '<Property id="L:Short" type="String">one\r\ntwo</Property>'
                      '<Property id="L:Meant" type="String">one&#13;&#10;two</Property>'
                      '<Property id="L:Hex" type="String">one&#xD;two</Property>'
                      '<Property id="L:Ends" type="String"> line one\r\nline two\r\n</Property>'
                      '<Property id="L:Mac" type="String">one\rtwo</Property>'
                      '<Property id="L:Tab" type="String">\tone\ntwo\n\n</Property>'
                      '<Property id="L:Value" type="String" value=" blanks, and one&#13;&#10;two "/>'
                      '<Property id="L:Data" type="String"><![CDATA[a < b\r\nc & d]]></Property>'
                      '<Property id="L:Block" type="String" location="inline:base64">' +
                      base64.b64encode(block.encode()).decode() + '</Property></Image><Metadata/>')
    before = {key: entry["value"] for key, entry in Theirs(str(source)).get_images_metadata()[0]["XISFProperties"].items()}
    assert before["L:Long"] == long_text.replace("\r\n", "\n") and before["L:Meant"] == "one\r\ntwo" and before["L:Block"] == block
    assert before["L:Ends"] == " line one\nline two\n" and before["L:Mac"] == "one\ntwo" and before["L:Data"] == "a < b\nc & d"
    assert before["L:Value"] is None                      # (that package looks for the text of the element)
    before["L:Value"] = " blanks, and one\r\ntwo "         # what the attribute says, to any XML reader
    xisf = XISF(source)
    meta = xisf.get_images_metadata()[0]
    assert {key: str(entry["value"]) for key, entry in meta["XISFProperties"].items()} == before
    assert meta["XISFProperties"]["L:Long"]["value"].raw == long_text

    def check(path, as_read=True):
        with xisfconv.open(path) as file:
            header = file.header_text
            assert file[0].properties["L:Long"] == long_text and file[0].properties["L:Meant"] == "one\r\ntwo"
        # the texts of the header are in the header, as they were, whatever their length
        assert '<Property id="L:Long" type="String">' + long_text + "</Property>" in header
        assert '<Property id="L:Short" type="String">one\r\ntwo</Property>' in header
        # ... and with whatever they have at their ends: the same bytes, which each reader reads as before
        assert '<Property id="L:Ends" type="String"> line one\r\nline two\r\n</Property>' in header
        assert '<Property id="L:Mac" type="String">one\rtwo</Property>' in header
        # (a text that is the program's own, with white space at its ends, is kept as data)
        assert ('<Property id="L:Tab" type="String">\tone\ntwo\n\n</Property>' in header) == as_read
        assert '<Property id="L:Data" type="String">a &lt; b\r\nc &amp; d</Property>' in header
        # a carriage return that is meant is kept as data, where no reader takes it away
        for key in ("L:Meant", "L:Hex", "L:Block", "L:Value"):
            assert re.search('<Property id="%s" type="String" location="(inline:base64|attachment:[0-9:]+)"' % key, header), key
        after = {key: entry["value"] for key, entry in Theirs(str(path)).get_images_metadata()[0]["XISFProperties"].items()}
        assert after == before
        assert {key: str(entry["value"]) for key, entry in XISF(path).get_images_metadata()[0]["XISFProperties"].items()} == before

    XISF.write(tmp_path / "module.xisf", xisf.read_image(0), image_metadata=meta)
    check(tmp_path / "module.xisf")
    # with a copy of the dictionaries, which has the values and not what else was read
    XISF.write(tmp_path / "copied.xisf", xisf.read_image(0),
               image_metadata={"XISFProperties": {key: dict(entry) for key, entry in meta["XISFProperties"].items()}})
    check(tmp_path / "copied.xisf", as_read=False)
    xisfconv.write(tmp_path / "core.xisf", xisfconv.read_image(source))
    check(tmp_path / "core.xisf")
    xisfconv.convert(source, tmp_path / "lines.fits")
    xisfconv.convert(tmp_path / "lines.fits", tmp_path / "converted.xisf")
    check(tmp_path / "converted.xisf")
    xisfconv.rewrite(source, tmp_path / "rewritten.xisf", codec="zlib")
    # (a rewrite copies the header: the value attribute is still one, which that package does not look at)
    assert {key: entry["value"] for key, entry in Theirs(str(tmp_path / "rewritten.xisf")).get_images_metadata()[0]
            ["XISFProperties"].items()} == dict(before, **{"L:Value": None})
    # a new text with a carriage return is data too: it is what the program said
    XISF.write(tmp_path / "new.xisf", xisf.read_image(0), image_metadata={"XISFProperties": {
        "N:Lines": {"id": "N:Lines", "type": "String", "value": "a\r\nb"},
        "N:Plain": {"id": "N:Plain", "type": "String", "value": "a\nb"}}})
    after = Theirs(str(tmp_path / "new.xisf")).get_images_metadata()[0]["XISFProperties"]
    assert after["N:Lines"]["value"] == "a\r\nb" and after["N:Plain"]["value"] == "a\nb" and "location" not in after["N:Plain"]


def test_a_solution_that_was_read_is_not_written_with_another_image(tmp_path):
    """The xisf package writes what it is given. A solution that was read with an image of
    another size, or with other WCS keywords, is not written here: PixInsight would believe it."""
    cards = {"CTYPE1": "RA---TAN", "CTYPE2": "DEC--TAN", "CRVAL1": 83.82, "CRVAL2": -5.39, "CRPIX1": 5.0, "CRPIX2": 4.0,
             "CD1_1": -2e-4, "CD1_2": 1e-5, "CD2_1": 1e-5, "CD2_2": 2e-4}
    reference = "PCL:AstrometricSolution:ReferenceCelestialCoordinates"
    xisfconv.write(tmp_path / "sky.xisf", sample("float32", (8, 10)), keywords=cards, wcs_row_order="bottom-up")
    meta = {}
    data = XISF.read(tmp_path / "sky.xisf", image_metadata=meta)
    assert reference in meta["XISFProperties"]

    def written(pixels, metadata):
        XISF.write(tmp_path / "out.xisf", pixels, image_metadata=metadata)
        return XISF(tmp_path / "out.xisf").get_images_metadata()[0]

    same_again = written(data, meta)
    assert list(same_again["XISFProperties"]) == list(meta["XISFProperties"])
    assert same_again["XISFProperties"]["PCL:AstrometricSolution:CreationTime"]["value"] == \
        meta["XISFProperties"]["PCL:AstrometricSolution:CreationTime"]["value"]
    # a crop: the solution is made from the keywords the image has
    cropped = written(data[:6], meta)
    assert cropped["geometry"] == (10, 6, 1) and reference in cropped["XISFProperties"]
    with xisfconv.open(tmp_path / "out.xisf") as file:
        assert file[0].has_astrometric_solution and file[0].wcs_keywords()["CRVAL1"] == pytest.approx(83.82)
    # other keywords: the same
    meta["FITSKeywords"]["CRVAL1"][0]["value"] = "90.0"
    moved = written(data, meta)
    assert moved["XISFProperties"][reference]["value"][0] == pytest.approx(90.0)
    # no keywords: nothing to make one from, and a warning says that the file has none
    solution_only = {"XISFProperties": meta["XISFProperties"]}
    with pytest.warns(xisfconv.XisfconvWarning, match="the file has no astrometric solution"):
        alone = written(data[:6], solution_only)
    assert not any(key.startswith("PCL:AstrometricSolution:") for key in alone["XISFProperties"])
    # dictionaries that were copied are the program's own, and written as they are (as that package does)
    copied = {"XISFProperties": {key: dict(entry) for key, entry in meta["XISFProperties"].items()}}
    assert reference in written(data[:6], copied)["XISFProperties"]


def test_what_a_file_has_twice_or_not_as_a_value(tmp_path):
    """Two properties of one id, a table, a property inside another, one without an id: what is
    not read is left out with a warning, here and where the file is written again."""
    source = handmade(tmp_path / "odd.xisf", IMAGE +
                      '<Property id="T:Twice" type="Int32" value="1"/><Property id="T:Twice" type="Int32" value="2"/>'
                      '<Property id="T:Table" type="Table"><Structure><Field id="a" type="Int32"/></Structure></Property>'
                      '<Property id="T:Nested" type="String">text<Property id="T:Inner" type="Int32" value="5"/></Property>'
                      '<Property type="Int32" value="3"/><Property type="Int32" value="6"/>'
                      '<Property id="T:Fine" type="Int32" value="4"/></Image>')
    with pytest.warns(xisfconv.XisfconvWarning) as caught:
        meta = XISF(source).get_images_metadata()[0]
    said = " | ".join(str(warning.message) for warning in caught)
    assert "T:Table is left out" in said and "T:Nested is left out" in said and "(without an id) is left out" in said
    assert {key: entry["value"] for key, entry in meta["XISFProperties"].items()} == {"T:Twice": 1, "T:Fine": 4}
    with xisfconv.open(source) as file:
        properties = file[0].properties
        assert properties["T:Twice"] == 1 and properties["T:Table"] is None and properties["T:Nested"] is None
        assert properties.type("T:Table") == "Table"
    with pytest.warns(xisfconv.XisfconvWarning) as caught:
        image = xisfconv.read_image(source)
    said = sorted(str(warning.message).split(": ", 1)[1] for warning in caught)
    assert said == ["1 property has the id of an earlier one, and is left out", "2 properties without an id are left out"]
    assert dict(image.properties) == {"T:Twice": 1, "T:Table": None, "T:Nested": None, "T:Fine": 4}
    with pytest.warns(xisfconv.XisfconvWarning) as caught:
        xisfconv.write(tmp_path / "again.xisf", image)
    said = " | ".join(str(warning.message) for warning in caught)
    assert "T:Table (a Table) has no value and is not written" in said and "T:Nested" in said and len(caught) == 2
    with xisfconv.open(tmp_path / "again.xisf") as file:
        assert dict(file[0].properties) == {"T:Twice": 1, "T:Fine": 4}
    with pytest.warns(xisfconv.XisfconvWarning):          # as a conversion has it
        xisfconv.convert(source, tmp_path / "odd.fits")
    with xisfconv.open(tmp_path / "odd.fits") as file:
        assert dict(file[0].properties) == {"T:Twice": 1, "T:Fine": 4}


def test_bytes_that_are_no_text_and_a_property_without_a_type(tmp_path):
    """A text block holds bytes; they go back out as they are also where they are not UTF-8. A
    property without a type attribute is written without a type again."""
    odd = b"ab\xff\xfecd"
    source = handmade(tmp_path / "bytes.xisf", IMAGE +
                      '<Property id="B:Bytes" type="String" location="inline:base64">' + base64.b64encode(odd).decode() +
                      '</Property><Property id="B:Bare" value="1"/><Property id="B:Fine" type="Int32" value="4"/></Image>')
    wanted = 'location="inline:base64">' + base64.b64encode(odd).decode() + "</Property>"
    image = xisfconv.read_image(source)
    assert image.properties["B:Bytes"] == "ab\ufffd\ufffdcd" and image.properties["B:Bare"] == "1"
    xisfconv.write(tmp_path / "core.xisf", image)
    meta = XISF(source).get_images_metadata()[0]
    XISF.write(tmp_path / "module.xisf", PIXELS, image_metadata=meta)
    assert image.properties.type("B:Bare") == ""
    for name in ("core.xisf", "module.xisf"):
        with xisfconv.open(tmp_path / name) as file:
            assert wanted in file.header_text and '<Property id="B:Bare" type="" value="1"/>' in file.header_text, name
            assert file[0].properties.type("B:Bare") == "" and file[0].properties["B:Fine"] == 4
    # What XML cannot hold (a control character, which the library reads from a reference and
    # an XML parser refuses): a blank in its place in a value, and a text as the data it is.
    control = handmade(tmp_path / "control.xisf", IMAGE +
                       '<Property id="B:Bell" value="a&#1;b"/><Property id="B:When" type="TimePoint" value="2020-01-01&#1;"/>'
                       '<Property id="B:Text" type="String">a&#1;b</Property></Image>')
    with pytest.raises(ValueError, match="not well-formed"):
        XISF(control)
    with pytest.warns(xisfconv.XisfconvWarning, match="B:(Bell|When): its value is not text that XML can hold; characters are replaced"):
        xisfconv.write(tmp_path / "control-again.xisf", xisfconv.read_image(control))
    with xisfconv.open(tmp_path / "control-again.xisf") as file:
        assert file[0].properties["B:Bell"] == "a b" and file[0].properties["B:When"] == "2020-01-01 "
        assert file[0].properties["B:Text"] == "a\x01b" and 'id="B:Text" type="String" location="inline:base64"' in file.header_text
    XISF(tmp_path / "control-again.xisf")                 # (which is XML again)
    # a text that is changed is the program's: what Python has for it
    image.properties["B:Bytes"] = "new"
    image.properties["B:Bare"] = 2
    xisfconv.write(tmp_path / "core.xisf", image, overwrite=True)
    with xisfconv.open(tmp_path / "core.xisf") as file:
        assert file[0].properties["B:Bytes"] == "new" and file[0].properties.type("B:Bare") == "Int32"


def test_the_name_of_the_program(tmp_path):
    XISF.write(tmp_path / "named.xisf", PIXELS, creator_app="  my script\n2.0\t(beta) ")
    assert XISF(tmp_path / "named.xisf").get_file_metadata()["XISF:CreatorApplication"]["value"] == "my script 2.0 (beta)"
    XISF.write(tmp_path / "named.xisf", PIXELS, creator_app="")
    assert XISF(tmp_path / "named.xisf").get_file_metadata()["XISF:CreatorApplication"]["value"].startswith("Python ")


def test_the_forms_of_an_array(tmp_path):
    Theirs = their_class()
    path = tmp_path / "forms.xisf"
    flat = sample("uint16", (6, 8))
    XISF.write(path, flat)                                             # two dimensions: one channel
    assert XISF(path).get_images_metadata()[0]["geometry"] == (8, 6, 1) and same(XISF.read(path)[:, :, 0], flat)
    XISF.write(path, flat[:, :, None])
    assert same(Theirs.read(str(path))[:, :, 0], flat)
    colour = sample("float32", (6, 8, 3))
    XISF.write(path, colour)
    meta = XISF(path).get_images_metadata()[0]
    assert meta["geometry"] == (8, 6, 3) and meta["colorSpace"] == "RGB" and meta["bounds"] == "0:1"
    assert same(Theirs.read(str(path)), colour) and meta["id"] == "image"
    # any other shape has the channels first, and is written with the geometry it has
    stack = sample("uint8", (2, 6, 8))
    XISF.write(path, stack)
    xisf = XISF(path)
    assert xisf.get_images_metadata()[0]["geometry"] == (8, 6, 2) and same(xisf.read_image(0, "channels_first"), stack)
    assert same(Theirs(str(path)).read_image(0, "channels_first"), stack)
    # 64-bit integers, which the xisf package does not have
    wide = sample("uint64", (4, 5))
    XISF.write(path, wide)
    assert same(XISF.read(path)[:, :, 0], wide)
    # a list is an array
    XISF.write(path, [[1.0, 0.5], [0.25, 0.0]])
    assert XISF.read(path).dtype == np.float64
    # floating point beyond 0:1 has the bounds it needs
    XISF.write(path, (colour * 40000).astype(np.float32))
    assert XISF(path).get_images_metadata()[0]["bounds"] == "0:65535"
    for bad in (np.zeros(5, np.uint8), np.zeros((2, 2, 2, 2), np.uint8)):
        with pytest.raises(ValueError):
            XISF.write(path, bad)
    for dtype in ("int16", "int32", "bool", "float16", "complex64"):
        with pytest.raises(NotImplementedError, match="samples of the type %s are not written" % dtype):
            XISF.write(path, np.zeros((3, 3), dtype))
    with pytest.raises(NotImplementedError, match="'brotli' is not a codec"):
        XISF.write(path, flat, codec="brotli")
    with pytest.raises(xisfconv.ArgumentError):
        XISF.write(path, flat, codec="zlib", level=10)
    assert same(XISF.read(path), (colour * 40000).astype(np.float32))       # a call that fails leaves the file as it was
    # what the file is called does not say what it is
    XISF.write(tmp_path / "frame.fits", flat)
    assert xisfconv.detect_format(tmp_path / "frame.fits") == "xisf"


def test_the_forms_of_metadata(tmp_path):
    path = tmp_path / "meta.xisf"
    data = sample("uint16", (5, 6))
    file_meta = {"Note:Author": {"id": "Note:Author", "type": "String", "value": "me"},
                 "XISF:CreationTime": {"id": "XISF:CreationTime", "type": "String", "value": "1999-01-01T00:00:00"}}
    before = pickle.dumps(file_meta)
    written = XISF.write(path, data, xisf_metadata=file_meta)
    assert written == (os.path.getsize(path), None) and pickle.dumps(file_meta) == before       # the dictionary is the caller's
    with xisfconv.open(path) as file:
        assert file.properties["XISF:CreatorApplication"] == "Python " + platform.python_version()
        assert file.properties["XISF:CreatorModule"].startswith("xisfconv ") and file.properties["Note:Author"] == "me"
        assert file.properties["XISF:CreationTime"] != "1999-01-01T00:00:00" and file[0].name == "image"
        assert len(file[0].keywords) == 0 and len(file[0].properties) == 0
    # values as a program may hold them: numbers, text that is a number, pairs for complex numbers
    meta = {
        "id": "my frame",
        "FITSKeywords": {"EXPTIME": [{"value": 30.5, "comment": "a number"}], "COOLED": [{"value": True, "comment": ""}],
                         "NCOMBINE": {"value": 12, "comment": "one card, not a list"},
                         "OBJECT": [{"value": "M 31"}], "RATIO": [{"value": "1.5e-3", "comment": None}],
                         "SIMPLE": [{"value": "T", "comment": "of a FITS file"}],
                         "HISTORY": [{"value": "", "comment": "one"}, {"value": "two", "comment": ""}]},
        "XISFProperties": {
            "A:Text": {"id": "A:Text", "type": "String", "value": "text"},
            "A:Number": {"id": "A:Number", "type": "Float64", "value": "2.5"},
            "A:Whole": {"id": "A:Whole", "type": "Int32", "value": "7"},
            "A:Flag": {"id": "A:Flag", "type": "Boolean", "value": "true"},
            "A:Pair": {"id": "A:Pair", "type": "Complex64", "value": (1.0, 2.0)},
            "A:NoText": {"id": "A:NoText", "type": "String", "value": None},
            "A:Untyped": {"id": "A:Untyped", "value": 0.5},
            "A:Keyed": {"type": "UInt8", "value": 9, "comment": "the key is the id", "format": "%d"},
            "A:Skipped": False,
            "A:Gone": None,
        },
    }
    data = np.full((40, 50), 1234, np.uint16)        # (which compresses)
    XISF.write(path, data, creator_app="mine 2.0", image_metadata=meta, xisf_metadata=None, codec="zlib", shuffle=1, level=0)
    with xisfconv.open(path) as file:
        assert file[0].name == "my_frame"              # an id of XISF has no blank
        keywords = file[0].keywords
        assert [card.name for card in keywords] == ["EXPTIME", "COOLED", "NCOMBINE", "OBJECT", "RATIO", "HISTORY", "HISTORY"]
        assert keywords["EXPTIME"] == 30.5 and keywords["COOLED"] is True and keywords["NCOMBINE"] == 12
        assert keywords["OBJECT"] == "M 31" and keywords["RATIO"] == 1.5e-3
        assert [card.value for card in keywords.cards("HISTORY")] == ["one", "two"]
        properties = file[0].properties
        assert dict(properties) == {"A:Text": "text", "A:Number": 2.5, "A:Whole": 7, "A:Flag": True, "A:Pair": 1 + 2j,
                                    "A:NoText": "", "A:Untyped": 0.5, "A:Keyed": 9}
        assert properties.type("A:Untyped") == "Float64" and properties.type("A:Keyed") == "UInt8"
        assert properties.comment("A:Keyed") == "the key is the id" and properties.format("A:Keyed") == "%d"
        assert file.properties["XISF:CreatorApplication"] == "mine 2.0"
        assert file[0].detail("compression").startswith("zlib+sh:") and "XISF:CompressionLevel" not in file.properties
    for bad, error in (({"FITSKeywords": "OBJECT"}, TypeError), ({"XISFProperties": [1]}, TypeError),
                       ({"XISFProperties": {"A": 5}}, TypeError),
                       ({"XISFProperties": {"A": {"type": "Int32", "value": "seven"}}}, TypeError),
                       ({"XISFProperties": {"A": {"type": "UInt8", "value": 300}}}, ValueError),
                       ({"XISFProperties": {"A": {"type": "Table", "value": "x"}}}, ValueError)):
        with pytest.raises(error):
            XISF.write(path, data, image_metadata=bad)
    with pytest.raises(TypeError):
        XISF.write(path, data, xisf_metadata=[1])


def test_the_module():
    import xisfconv.xisf as module
    assert module.__all__ == ["XISF"] and module.__version__ == xisfconv.__version__
    for kind, bases in ((module.NotXisfError, (xisfconv.FormatError, ValueError)),
                        (module.NotSupportedError, (xisfconv.UnsupportedError, NotImplementedError)),
                        (module.ImageNumberError, (xisfconv.ImageIndexError, IndexError, ValueError))):
        assert all(issubclass(kind, base) for base in bases)
    assert "xisf" in module.__doc__ and XISF.__doc__ and XISF.write.__doc__ and XISF.read_image.__doc__
