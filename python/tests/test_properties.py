# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""XISF properties from Python values: what write takes, what the file then holds, and what
comes back; the LZ4 codecs, compression levels and the other options of the writer that came
with them."""

import copy
import datetime
import logging
import pickle
import re
import warnings
import zlib

import numpy as np
import pytest

import xisfconv
from xisfconv import PropertyDict
from util import handmade, inline, sample, same, xisf_read

UTC = datetime.timezone.utc

# A value of every kind, with the XISF type it is written with when none is stated.
VALUES = [
    ("Lab:Flag", True, "Boolean"),
    ("Lab:Off", False, "Boolean"),
    ("Lab:Count", -5, "Int32"),
    ("Lab:Large", 2 ** 40, "Int64"),
    ("Lab:Huge", 2 ** 64 - 1, "UInt64"),
    ("Lab:Ratio", 0.1, "Float64"),
    ("Lab:Phase", 1.5 - 2.25j, "Complex64"),
    ("Lab:Name", "café <&> \"au lait\"", "String"),
    ("Lab:Lines", "one\ntwo\n\tthree", "String"),
    ("Lab:Empty", "", "String"),
    ("Lab:When", datetime.datetime(2026, 10, 6, 18, 30, 0, 250000, tzinfo=UTC), "TimePoint"),
    ("Lab:Single", np.float32(0.1), "Float32"),
    ("Lab:Word", np.uint16(65535), "UInt16"),
    ("Lab:Tiny", np.int8(-128), "Int8"),
    ("Lab:Pair", np.complex64(1 + 2j), "Complex32"),
    ("Lab:Vector", np.array([1.5, -2.5, 1e-20], np.float32), "F32Vector"),
    ("Lab:Matrix", np.arange(6, dtype=np.int16).reshape(2, 3) - 3, "I16Matrix"),
    ("Lab:Doubles", np.array([[1.0, 2.0], [3.0, 4.5]]), "F64Matrix"),
    ("Lab:Complex", np.array([1 + 1j, 2 - 2j], np.complex64), "C32Vector"),
    ("Lab:Wide", np.array([[1j, 2], [3, 4j]], np.complex128), "C64Matrix"),
    ("Lab:Big", np.array([0, 2 ** 64 - 1], np.uint64), "UI64Vector"),
    ("Lab:List", [1, 2, 3], "I32Vector"),
    ("Lab:Floats", (0.5, 1.5), "F64Vector"),
    ("Lab:Rows", [[1, 2], [3, 4], [5, 6]], "I32Matrix"),
    ("Lab:Bytes", b"\x00\x01\xfe\xff", "ByteArray"),
    ("Lab:Many", np.linspace(0.0, 1.0, 2000), "F64Vector"),      # more than the header takes: attached
    ("Lab:None", np.array([], np.uint32), "UI32Vector"),
]


def expected(value):
    """The value as it is read back."""
    if isinstance(value, datetime.datetime):
        return "2026-10-06T18:30:00.250000Z"
    if isinstance(value, bytes):
        return np.frombuffer(value, np.uint8)
    if isinstance(value, (list, tuple)):
        value = np.asarray(value)
        return value.astype(np.int32) if value.dtype.kind == "i" else value
    if isinstance(value, np.float32):
        return float(str(value))      # a Float32 is read as the number its text says: 0.1, not 0.10000000149011612
    if isinstance(value, np.generic):
        return value.item()
    return value


def equal(a, b):
    if isinstance(a, np.ndarray) or isinstance(b, np.ndarray):
        return isinstance(a, np.ndarray) and isinstance(b, np.ndarray) and a.dtype == b.dtype and a.shape == b.shape \
            and np.array_equal(a, b)
    return type(a) is type(b) and a == b


def test_every_kind_of_value(tmp_path):
    """Each value is written with the type that goes with it and read back as it was, by the
    library and by the xisf package."""
    data = sample("uint16", (9, 11))
    path = tmp_path / "values.xisf"
    xisfconv.write(path, data, properties={key: value for key, value, _ in VALUES})
    with xisfconv.open(path) as file:
        properties = file[0].properties
        assert list(properties) == [key for key, _, _ in VALUES]
        for key, value, kind in VALUES:
            assert properties.type(key) == kind, key
            assert equal(properties[key], expected(value)), (key, properties[key])
        assert same(file[0].read(), data)
        header = file.header_text
    # as XISF writes them
    assert '<Property id="Lab:Flag" type="Boolean" value="true"/>' in header
    assert '<Property id="Lab:Off" type="Boolean" value="false"/>' in header
    assert '<Property id="Lab:Ratio" type="Float64" value="0.1"/>' in header
    assert '<Property id="Lab:Single" type="Float32" value="0.1"/>' in header      # not 0.10000000149011612
    assert '<Property id="Lab:Phase" type="Complex64" value="(1.5,-2.25)"/>' in header
    assert '<Property id="Lab:When" type="TimePoint" value="2026-10-06T18:30:00.250000Z"/>' in header
    assert '<Property id="Lab:Name" type="String">café &lt;&amp;&gt; &quot;au lait&quot;</Property>' in header
    assert re.search(r'<Property id="Lab:Many" type="F64Vector" length="2000" location="attachment:\d+:16000"/>', header)
    assert re.search(r'<Property id="Lab:Rows" type="I32Matrix" rows="3" columns="2" location="inline:base64">', header)

    pytest.importorskip("xisf")
    import contextlib
    import io
    # (the xisf package does not get past a vector without elements: the file again, without it)
    xisfconv.write(path, data, properties={key: value for key, value, _ in VALUES if key != "Lab:None"}, overwrite=True)
    with contextlib.redirect_stdout(io.StringIO()):     # (it prints what it does not read: ByteArray)
        theirs = xisf_read(path)[1]["XISFProperties"]
    for key, value, kind in VALUES:
        if kind == "ByteArray" or key == "Lab:None":
            continue
        assert theirs[key]["type"] == kind
        want = expected(value)
        if kind.startswith("Complex"):
            assert complex(*theirs[key]["value"]) == want
        elif isinstance(want, np.ndarray):
            assert equal(np.array(theirs[key]["value"]), want), key
        else:
            got = "" if kind == "String" and theirs[key]["value"] is None else theirs[key]["value"]   # (no text is None there)
            assert got == want and type(got) is type(want), key


def test_stated_types_comments_and_formats(tmp_path):
    properties = PropertyDict()
    properties.set("A:Gain", 120, type="Float32", comment="of the camera", format="%.1f")
    properties.set("A:Byte", 255, "UInt8")
    properties.set("A:Short", -7, "Short")                # another name of Int16
    properties.set("A:Double", 2, "Double")
    properties.set("A:Flag", 1, "Boolean")
    properties.set("A:Start", "2026-10-06T18:30:00Z", "TimePoint", comment="when it began")
    properties.set("A:Day", datetime.date(2026, 10, 6), "TimePoint")
    properties.set("A:Vector", [1, 2, 3], "F32Vector")
    properties.set("A:Matrix", [[1, 2], [3, 4]], "UI8Matrix")
    properties.set("A:Complex", [1, 2.5], "C64Vector")
    properties.set("A:Blob", b"\x07\x08", "UI8Vector")
    properties.set("A:Note", "plain", comment="a remark")
    properties["A:Inferred"] = 3
    assert properties.type("A:Gain") == "Float32" and properties.comment("A:Gain") == "of the camera"
    assert properties.format("A:Gain") == "%.1f" and properties.type("A:Inferred") == "Int32"
    assert properties.comment("A:Inferred") == "" and properties.format("A:Note") == ""
    path = tmp_path / "stated.xisf"
    xisfconv.write(path, sample("uint8", (4, 5)), properties=properties)
    with xisfconv.open(path) as file:
        got = file[0].properties
        assert [got.type(key) for key in got] == ["Float32", "UInt8", "Short", "Double", "Boolean", "TimePoint",
                                                  "TimePoint", "F32Vector", "UI8Matrix", "C64Vector", "UI8Vector",
                                                  "String", "Int32"]
        assert got["A:Gain"] == 120.0 and isinstance(got["A:Gain"], float) and got.comment("A:Gain") == "of the camera"
        assert got.format("A:Gain") == "%.1f" and got["A:Byte"] == 255 and got["A:Short"] == -7
        assert got["A:Double"] == 2.0 and got["A:Flag"] is True
        assert got["A:Start"] == "2026-10-06T18:30:00Z" and got.comment("A:Start") == "when it began"
        assert got["A:Day"] == "2026-10-06"
        assert equal(got["A:Vector"], np.array([1, 2, 3], np.float32))
        assert equal(got["A:Matrix"], np.array([[1, 2], [3, 4]], np.uint8))
        assert equal(got["A:Complex"], np.array([1, 2.5], np.complex128))
        assert equal(got["A:Blob"], np.array([7, 8], np.uint8))
        assert got["A:Note"] == "plain" and got.comment("A:Note") == "a remark"
        assert 'comment="of the camera" format="%.1f" value="120.0"' in file.header_text


def test_time_points(tmp_path):
    cet = datetime.timezone(datetime.timedelta(hours=2))
    properties = {
        "T:Utc": datetime.datetime(2026, 1, 2, 3, 4, 5, tzinfo=UTC),
        "T:Zone": datetime.datetime(2026, 1, 2, 3, 4, 5, tzinfo=cet),
        "T:Naive": datetime.datetime(2026, 1, 2, 3, 4, 5, 123000),
        "T:Date": datetime.date(1999, 12, 31),
        "T:Numpy": np.datetime64("2026-01-02T03:04:05.678"),
        "T:Nanoseconds": np.datetime64("2026-01-02T03:04:05.123456789"),
        "T:Minutes": np.datetime64("2026-01-02T03:04"),
        "T:Day": np.datetime64("2026-01-02"),
        # an offset of hours, minutes and seconds (local mean time) is written as the time it is
        "T:Seconds": datetime.datetime(1850, 1, 1, 12, 0, 0, tzinfo=datetime.timezone(datetime.timedelta(minutes=17, seconds=30))),
        "T:Leap": datetime.date(2024, 2, 29),
        "T:Pico": np.datetime64("1970-01-02T03:04:05.123456789012"),      # (picoseconds reach 106 days from 1970)
        "T:Year": np.datetime64("2026"),
        "T:Atto": np.datetime64("1970-01-01T00:00:01.5", "as"),
    }
    path = tmp_path / "time.xisf"
    xisfconv.write(path, sample("uint8", (3, 3)), properties=properties)
    with xisfconv.open(path) as file:
        got = file[0].properties
        assert all(got.type(key) == "TimePoint" for key in got)
        assert dict(got) == {"T:Utc": "2026-01-02T03:04:05Z", "T:Zone": "2026-01-02T03:04:05+02:00",
                             "T:Naive": "2026-01-02T03:04:05.123000", "T:Date": "1999-12-31",
                             "T:Numpy": "2026-01-02T03:04:05.678", "T:Nanoseconds": "2026-01-02T03:04:05.123456789",
                             "T:Minutes": "2026-01-02T03:04:00", "T:Day": "2026-01-02", "T:Seconds": "1850-01-01T11:42:30Z",
                             "T:Leap": "2024-02-29", "T:Pico": "1970-01-02T03:04:05.123456789012", "T:Year": "2026-01-01",
                             "T:Atto": "1970-01-01T00:00:01.500000000000000000"}
    for bad in (np.datetime64("-0044-03-15"), np.datetime64(10 ** 15, "D"), np.datetime64("10000-01-01"),
                np.datetime64(2 ** 62, "s"),
                datetime.datetime(1, 1, 1, tzinfo=datetime.timezone(datetime.timedelta(seconds=1))),
                datetime.datetime(9999, 12, 31, 23, 59, 59, tzinfo=datetime.timezone(datetime.timedelta(seconds=-1)))):
        with pytest.raises(ValueError, match="property T: .*0001 to 9999"):
            xisfconv.write(path, sample("uint8", (3, 3)), properties={"T": bad}, overwrite=True)
    for bad in ("yesterday", "2026-13", "06/10/2026", "2026-10-06 18:30", "", "2026-02-30", "2025-02-29T00:00:00Z", "2026-06-31"):
        with pytest.raises(xisfconv.ArgumentError, match="ISO 8601"):
            xisfconv.write(path, sample("uint8", (3, 3)), properties=_typed("T", bad, "TimePoint"), overwrite=True)
    with pytest.raises(ValueError, match="not a time"):
        xisfconv.write(path, sample("uint8", (3, 3)), properties={"T": np.datetime64("NaT")}, overwrite=True)
    with pytest.raises(TypeError, match="TimePoint"):
        xisfconv.write(path, sample("uint8", (3, 3)), properties=_typed("T", 5, "TimePoint"), overwrite=True)


def _typed(key, value, kind):
    properties = PropertyDict()
    properties.set(key, value, kind)
    return properties


def test_property_dict():
    properties = PropertyDict({"A": 1, "B": "two"})
    assert properties == {"A": 1, "B": "two"} and isinstance(properties, dict) and len(properties) == 2
    assert properties.type("A") == "Int32" and properties.type("B") == "String"
    properties.set("C", 0.5, "Float32", "half", "%.2f")
    assert properties["C"] == 0.5 and (properties.type("C"), properties.comment("C"), properties.format("C")) == \
        ("Float32", "half", "%.2f")
    # a new value keeps what was stated; deleting forgets it
    properties["C"] = 0.75
    assert properties.type("C") == "Float32" and properties.comment("C") == "half"
    del properties["C"]
    properties["C"] = 0.75
    assert properties.type("C") == "Float64" and properties.comment("C") == ""
    properties.set("D", 1, "UInt8")
    assert properties.pop("D") == 1 and properties.pop("D", None) is None
    properties["D"] = 1
    assert properties.type("D") == "Int32"
    properties.set("E", 2, "UInt16")
    assert properties.popitem() == ("E", 2)
    properties["E"] = 2
    assert properties.type("E") == "Int32"
    properties.set("C", 0.5, "Float32")
    properties.set("C", 0.5)                      # stating nothing forgets the type
    assert properties.type("C") == "Float64"
    with pytest.raises(KeyError):
        properties.type("No:Such")
    with pytest.raises(KeyError):
        properties.comment("No:Such")
    with pytest.raises(KeyError):
        properties.format("No:Such")
    with pytest.raises(TypeError):
        properties.set("F", 1, type=5)
    # made as a dict is made
    assert PropertyDict([("A", 1)]) == {"A": 1} and PropertyDict(A=1, B=2) == {"A": 1, "B": 2}
    assert PropertyDict({"A": 1}, B=2) == {"A": 1, "B": 2} and PropertyDict() == {} and PropertyDict(None) == {}
    assert PropertyDict.fromkeys(["A", "B"], 0) == {"A": 0, "B": 0}
    with pytest.raises(TypeError):
        PropertyDict({"A": 1}, {"B": 2})
    with pytest.raises((TypeError, ValueError)):
        PropertyDict("AB")
    with pytest.raises(TypeError):
        properties["G"] = object()
        properties.type("G")
    del properties["G"]
    properties["H"] = None
    assert properties.type("H") == ""             # nothing to write, and nothing to call it

    # copies carry what is stated; a plain dict of it does not need to
    properties.set("K", 3, "UInt8", "three")
    for other in (properties.copy(), copy.copy(properties), copy.deepcopy(properties), PropertyDict(properties),
                  pickle.loads(pickle.dumps(properties))):
        assert isinstance(other, PropertyDict) and other == properties and other is not properties
        assert other.type("K") == "UInt8" and other.comment("K") == "three" and list(other) == list(properties)
        other.set("K", 4, "Int64")
        assert properties.type("K") == "UInt8" and properties["K"] == 3
    assert dict(properties) == properties and type(dict(properties)) is dict

    # what is stated goes along when dictionaries are put together
    typed = PropertyDict()
    typed.set("T:Start", "2026-10-06T18:30:00Z", "TimePoint", "began")
    typed.set("T:Gain", 0.5, "Float32")
    for merged in (PropertyDict({"A": 1}) | typed, {"A": 1} | typed, typed | {"A": 1}):
        assert isinstance(merged, PropertyDict) and set(merged) == {"A", "T:Start", "T:Gain"}
        assert merged.type("T:Start") == "TimePoint" and merged.comment("T:Start") == "began"
        assert merged.type("T:Gain") == "Float32" and merged.type("A") == "Int32"
    into = PropertyDict({"T:Gain": 7})
    into.update(typed)
    assert into.type("T:Gain") == "Float32" and into["T:Gain"] == 0.5 and into.type("T:Start") == "TimePoint"
    into |= PropertyDict({"T:Gain": 9})                 # a value that states nothing takes the place of one that did
    assert into.type("T:Gain") == "Int32" and into["T:Gain"] == 9
    into.update({"T:Start": "2027-01-01T00:00:00Z"})     # from a plain dict: a new value, the type stays
    assert into.type("T:Start") == "TimePoint" and into.comment("T:Start") == "began"
    into.update(B=2)
    assert into["B"] == 2
    with pytest.raises(TypeError):
        into.update({}, {})
    assert typed.__or__(5) is NotImplemented
    properties.clear()
    properties["K"] = 3
    assert len(properties) == 1 and properties.type("K") == "Int32"
    assert repr(PropertyDict({"A": 1})) == "PropertyDict({'A': 1})"

    image = xisfconv.Image(np.zeros((2, 2), np.uint8), properties={"A": 1})
    assert isinstance(image.properties, PropertyDict) and image.properties == {"A": 1}
    assert isinstance(xisfconv.Image(np.zeros((2, 2), np.uint8)).properties, PropertyDict)
    kept = PropertyDict({"B": 2})
    assert xisfconv.Image(np.zeros((2, 2), np.uint8), properties=kept).properties is kept


def test_values_that_are_not_written(tmp_path):
    path = tmp_path / "no.xisf"
    data = sample("uint8", (4, 4))

    def write(properties, **options):
        xisfconv.write(path, data, properties=properties, overwrite=True, **options)

    for value, error in ((object(), TypeError), ({"a": 1}, TypeError), (np.zeros((2, 2, 2)), ValueError),
                         (np.zeros(()), ValueError), (np.array([True, False]), TypeError),
                         (np.array(["a", "b"]), TypeError), ([[1, 2], [3]], (TypeError, ValueError)),
                         (2 ** 64, ValueError), (-2 ** 63 - 1, ValueError), (np.float16(1.0), TypeError),
                         (np.zeros(3, np.float16), TypeError)):
        with pytest.raises(error):
            write({"X:Value": value})
    # a type that is stated, and a value that is not of it
    for value, kind, error in (("five", "Int32", TypeError), (1.5, "Int32", TypeError), (300, "UInt8", xisfconv.ArgumentError),
                               (-1, "UInt16", xisfconv.ArgumentError), ("x", "Float64", TypeError),
                               (1 + 2j, "Float64", TypeError), (2, "Boolean", TypeError), ("true", "Boolean", TypeError),
                               (1e300, "Float32", ValueError), (5, "String", TypeError), ("x", "Complex64", TypeError),
                               ([1.5, 2.5], "I32Vector", TypeError), ([1, 2 ** 40], "I32Vector", ValueError),
                               ([-1], "UI8Vector", ValueError), ([1 + 2j], "F64Vector", TypeError),
                               ([[1, 2]], "F64Vector", ValueError), ([1, 2], "F64Matrix", ValueError),
                               (5, "F64Vector", ValueError), ("1 2 3", "Table", ValueError), (1, "float64", ValueError),
                               ([1, 2], "Int128", TypeError), (1, "", None), (1, "No Such Type", ValueError),
                               ([1e300], "F32Vector", ValueError), ([1e300 + 0j], "C32Vector", ValueError)):
        if error is None:        # an empty type is none: the value says what it is
            write(_typed("X:Value", value, kind))
            continue
        with pytest.raises(error):
            write(_typed("X:Value", value, kind))
    assert issubclass(xisfconv.ArgumentError, ValueError)
    # what is not an id
    for key, error in ((5, TypeError), ("", xisfconv.ArgumentError), ("a\x00b", ValueError), ("a\x01b", xisfconv.ArgumentError)):
        with pytest.raises(error):
            write({key: 1})
    with pytest.raises(xisfconv.ArgumentError):
        write(_with_comment("X", 1, "bad \x02 comment"))
    with pytest.raises((TypeError, ValueError)):
        write("XY")
    with pytest.raises(TypeError, match="dict"):
        xisfconv.write(path, data, file_properties="X", overwrite=True)
    with pytest.raises(TypeError, match="dict"):
        xisfconv.write(path, data, file_properties=[("X", 1)], overwrite=True)
    # the 128-bit types of XISF are too wide to check: a value is written as the number or the text it is
    wide = PropertyDict()
    wide.set("W:Int", 2 ** 100, "Int128")
    wide.set("W:Float", "1.18973149535723176508575932662800702e+4932", "Float128")
    wide.set("W:Complex", 1 - 2j, "Complex128")
    write(wide)
    with xisfconv.open(path) as file:
        assert dict(file[0].properties) == {"W:Int": 2 ** 100, "W:Float": float("inf"), "W:Complex": 1 - 2j}
        assert 'type="Float128" value="1.18973149535723176508575932662800702e+4932"' in file.header_text
        assert 'type="Int128" value="%d"' % 2 ** 100 in file.header_text
    # text with a character XML cannot hold is a String all the same: it goes into a data block
    write({"X:Odd": "bell \x07 and null-free"})
    with xisfconv.open(path) as file:
        assert file[0].properties["X:Odd"] == "bell \x07 and null-free"
        assert 'id="X:Odd" type="String" location="inline:base64"' in file.header_text
    # nothing of a failed call is left
    with pytest.raises(TypeError):
        xisfconv.write(tmp_path / "never.xisf", data, properties={"X": object()})
    assert not (tmp_path / "never.xisf").exists() and not (tmp_path / "never.xisf.part").exists()
    # a property without a value is left out, and said to be
    with pytest.warns(xisfconv.XisfconvWarning, match="X:Nothing has no value"):
        write({"X:Nothing": None, "X:Something": 1})
    with xisfconv.open(path) as file:
        assert dict(file[0].properties) == {"X:Something": 1}
    with pytest.warns(xisfconv.XisfconvWarning):
        write({"X:Nothing": None})
    with xisfconv.open(path) as file:
        assert len(file[0].properties) == 0


def _with_comment(key, value, comment):
    properties = PropertyDict()
    properties.set(key, value, comment=comment)
    return properties


def read_all(path):
    """[(id, type, value, comment, format)] of the image and of the file."""
    with xisfconv.open(path) as file:
        out = []
        for properties in (file[0].properties, file.properties):
            out.append([(key, properties.type(key), properties[key], properties.comment(key), properties.format(key))
                        for key in properties])
        return out


def same_properties(a, b):
    return len(a) == len(b) and all(x[:2] == y[:2] and x[3:] == y[3:] and equal(x[2], y[2]) for x, y in zip(a, b))


def test_an_image_that_is_read_is_written_with_its_properties(tmp_path):
    """read_image and write: the properties with their types, comments and formats, to XISF,
    and through FITS and ASDF."""
    first = PropertyDict({key: value for key, value, _ in VALUES})
    first.set("Lab:Gain", 0.25, "Float32", "a quarter", "%.3f")
    first.set("Lab:Start", "2026-10-06T18:30:00Z", "TimePoint")
    data = sample("float32", (8, 9))
    path = tmp_path / "first.xisf"
    xisfconv.write(path, data, properties=first, file_properties={"Note:Author": "somebody", "Note:Count": np.uint8(3)},
                   codec="zlib")
    image_properties, file_properties = read_all(path)
    assert len(image_properties) == len(VALUES) + 2 and [p[:3] for p in file_properties if p[0].startswith("Note:")] == \
        [("Note:Author", "String", "somebody"), ("Note:Count", "UInt8", 3)]

    image = xisfconv.read_image(path)
    assert isinstance(image.properties, PropertyDict) and image.properties.type("Lab:Gain") == "Float32"
    assert image.properties.comment("Lab:Gain") == "a quarter" and image.properties.format("Lab:Gain") == "%.3f"
    assert image.properties.type("Lab:Start") == "TimePoint" and image.properties["Lab:Vector"].dtype == np.float32
    with xisfconv.open(path) as file:
        notes = PropertyDict(file.properties)          # read from the open file, with their types
        assert notes.type("Note:Count") == "UInt8" and "XISF:CreationTime" in notes

    for name in ("again.xisf", "again.fits", "again.asdf", "again.fits.fz"):
        xisfconv.write(tmp_path / name, image, file_properties=notes)
        got_image, got_file = read_all(tmp_path / name)
        assert same_properties(got_image, image_properties), name
        assert [p for p in got_file if p[0].startswith("Note:")] == [p for p in file_properties if p[0].startswith("Note:")]
        if not name.endswith(".xisf"):
            # the properties that describe the storage of the XISF file stayed behind
            assert [p[0] for p in got_file] == ["Note:Author", "Note:Count"]
            xisfconv.convert(tmp_path / name, tmp_path / "back.xisf", overwrite=True)
            got_image, got_file = read_all(tmp_path / "back.xisf")
            assert same_properties(got_image, image_properties), name
            assert [p[:3] for p in got_file if p[0].startswith("Note:")] == \
                [("Note:Author", "String", "somebody"), ("Note:Count", "UInt8", 3)]
        assert same(xisfconv.read(tmp_path / name), data)
    # TIFF and PNG have no place for them
    xisfconv.write(tmp_path / "picture.tif", image)

    # the properties of an open file, as they are, and a value that is changed
    with xisfconv.open(path) as file:
        xisfconv.write(tmp_path / "direct.xisf", data, properties=file[0].properties, file_properties=file.properties)
    assert same_properties(read_all(tmp_path / "direct.xisf")[0], image_properties)
    image.properties["Lab:Gain"] = 0.5
    image.properties["Lab:New"] = "added"
    del image.properties["Lab:Many"]
    xisfconv.write(tmp_path / "changed.xisf", image)
    changed = read_all(tmp_path / "changed.xisf")[0]
    assert ("Lab:Gain", "Float32", 0.5, "a quarter", "%.3f") in changed and changed[-1][:3] == ("Lab:New", "String", "added")
    assert "Lab:Many" not in [p[0] for p in changed] and len(changed) == len(image_properties)


def test_the_properties_of_the_file(tmp_path):
    path = tmp_path / "file.xisf"
    data = sample("uint16", (5, 6))
    stored = {"XISF:CreationTime": "1999-01-01T00:00:00Z", "XISF:CreatorApplication": "somebody else",
              "XISF:CreatorModule": "x", "XISF:CreatorOS": "an abacus", "XISF:BlockAlignmentSize": 1,
              "XISF:MaxInlineBlockSize": 1, "XISF:CompressionCodecs": "none", "XISF:CompressionLevel": 99}
    xisfconv.write(path, data, file_properties=dict(stored, **{"Note:Author": "me", "XISF:Other": 5}), creator="my script 1.0")
    with xisfconv.open(path) as file:
        got = dict(file.properties)
        assert got["Note:Author"] == "me" and got["XISF:Other"] == 5
        # the storage is described by the writer, whatever was given
        assert got["XISF:CreatorApplication"] == "my script 1.0" and got["XISF:CreatorModule"] == "xisfconv " + xisfconv.library_version()
        assert got["XISF:BlockAlignmentSize"] == 4096 and got["XISF:CreationTime"] != "1999-01-01T00:00:00Z"
        assert "XISF:CreatorOS" not in got and "XISF:CompressionCodecs" not in got and "XISF:CompressionLevel" not in got
        assert len(file[0].properties) == 0
    xisfconv.write(path, data, overwrite=True)
    with xisfconv.open(path) as file:
        assert file.properties["XISF:CreatorApplication"] == "xisfconv " + xisfconv.library_version()
        assert "XISF:CreatorModule" not in file.properties
    for bad, error in ((5, TypeError), ("two\nlines", xisfconv.ArgumentError), ("a\x00b", ValueError)):
        with pytest.raises(error):
            xisfconv.write(path, data, creator=bad, overwrite=True)
    xisfconv.write(path, data, creator="  ", overwrite=True)          # blanks are no name
    with xisfconv.open(path) as file:
        assert file.properties["XISF:CreatorApplication"].startswith("xisfconv ")
    # several images, each with its own
    xisfconv.write(path, [xisfconv.Image(data, name="a", properties={"N": 1}), xisfconv.Image(data, name="b"),
                          xisfconv.Image(data, name="c", properties={"N": 3, "M": [1.5]})], overwrite=True)
    with xisfconv.open(path) as file:
        assert [dict(entry.properties).keys() for entry in file] == [{"N": 1}.keys(), {}.keys(), {"N": 3, "M": 0}.keys()]
        assert file[2].properties["N"] == 3 and file[0].properties["N"] == 1


SKY = {"CTYPE1": "RA---TAN", "CTYPE2": "DEC--TAN", "CRVAL1": 83.82, "CRVAL2": -5.39, "CRPIX1": 5.0, "CRPIX2": 4.0,
       "CD1_1": -2e-4, "CD1_2": 1e-5, "CD2_1": 1e-5, "CD2_2": 2e-4}
SYSTEM = "PCL:AstrometricSolution:ProjectionSystem"


def test_an_astrometric_solution_is_made_unless_one_is_brought(tmp_path):
    data = sample("float32", (8, 10))
    path = tmp_path / "sky.xisf"

    def properties(**options):
        xisfconv.write(path, data, keywords=SKY, wcs_row_order="bottom-up", overwrite=True, **options)
        with xisfconv.open(path) as file:
            return dict(file[0].properties)

    made = properties()
    assert made[SYSTEM] == "Gnomonic" and len(made) >= 6
    # other properties do not stand in its way
    with_others = properties(properties={"Lab:Gain": 0.5})
    assert with_others[SYSTEM] == "Gnomonic" and with_others["Lab:Gain"] == 0.5 and len(with_others) == len(made) + 1
    assert properties(properties={"Lab:Gain": 0.5}, wcs=False) == {"Lab:Gain": 0.5}
    # a solution that is brought is the solution
    brought = properties(properties={SYSTEM: "Mercator", "Lab:Gain": 0.5})
    assert brought == {SYSTEM: "Mercator", "Lab:Gain": 0.5}
    # and stays that through FITS, where it is carried as the solution of these keywords
    for name in ("sky.fits", "sky.asdf"):
        xisfconv.write(tmp_path / name, data, keywords=SKY, wcs_row_order="bottom-up",
                       properties={SYSTEM: "Mercator", "Lab:Gain": 0.5})
        xisfconv.convert(tmp_path / name, tmp_path / "back.xisf", overwrite=True)
        with xisfconv.open(tmp_path / "back.xisf") as file:
            assert dict(file[0].properties) == {SYSTEM: "Mercator", "Lab:Gain": 0.5}
        # without one, the conversion makes it from the keywords
        xisfconv.write(tmp_path / name, data, keywords=SKY, wcs_row_order="bottom-up", properties={"Lab:Gain": 0.5},
                       overwrite=True)
        xisfconv.convert(tmp_path / name, tmp_path / "back.xisf", overwrite=True)
        with xisfconv.open(tmp_path / "back.xisf") as file:
            assert file[0].properties[SYSTEM] == "Gnomonic" and file[0].properties["Lab:Gain"] == 0.5


REFERENCE = "PCL:AstrometricSolution:ReferenceCelestialCoordinates"


def test_a_solution_that_was_read_belongs_to_the_keywords_it_was_read_with(tmp_path, caplog):
    """An image is read with its solution, its WCS keywords are replaced, and it is written:
    the old solution must not be written next to the new keywords."""
    data = sample("float32", (8, 10))
    path = tmp_path / "sky.xisf"
    xisfconv.write(path, data, keywords=SKY, wcs_row_order="bottom-up", properties={"Lab:Gain": 0.5})

    def solved(image, name="out.xisf", **options):
        xisfconv.write(tmp_path / name, image, overwrite=True, **options)
        with xisfconv.open(tmp_path / name) as file:
            return dict(file[0].properties)

    image = xisfconv.read_image(path)
    made = dict(image.properties)
    assert image.properties.solution_of and made[SYSTEM] == "Gnomonic" and made["Lab:Gain"] == 0.5
    # as it was read: the solution is written as it is
    again = solved(image)
    assert list(again) == list(made) and again["PCL:AstrometricSolution:CreationTime"] == made["PCL:AstrometricSolution:CreationTime"]
    assert np.array_equal(again[REFERENCE], made[REFERENCE])
    # new keywords: the solution is made from them
    caplog.set_level(logging.INFO, logger="xisfconv")
    image.keywords["CRVAL1"] = 90.0
    changed = solved(image)
    assert changed[REFERENCE][0] == pytest.approx(90.0) and changed["Lab:Gain"] == 0.5
    assert any("is not written" in record.getMessage() and "made from the WCS keywords" in record.getMessage()
               for record in caplog.records)
    # (what is left without it: the properties that are not the solution's own; a file that
    # ends up without a solution is worth a warning)
    rest = {"Observation:CelestialReferenceSystem": "ICRS", "Observation:Equinox": 2000.0, "Lab:Gain": 0.5}
    with pytest.warns(xisfconv.XisfconvWarning, match="the file has no astrometric solution: none is made .* without wcs=True"):
        assert solved(image, wcs=False) == rest
    # a crop: the keywords are the same, the image is not
    image = xisfconv.read_image(path)
    image.data = image.data[:6]
    with pytest.warns(xisfconv.XisfconvWarning, match="has not the size or the WCS keywords it was read with"):
        cropped = solved(image, wcs=False)
    assert cropped == rest
    assert solved(image)[REFERENCE][0] == pytest.approx(83.82)
    with xisfconv.open(tmp_path / "out.xisf") as file:
        assert file[0].height == 6 and file[0].properties["Lab:Gain"] == 0.5
    # no keywords any more: no solution either, unless the program says that it stands
    image = xisfconv.read_image(path)
    image.keywords = None
    with pytest.warns(xisfconv.XisfconvWarning, match=r"the file has no astrometric solution \(properties.solution_of = None"):
        assert solved(image) == rest
    image.properties.solution_of = None
    alone = solved(image, "solution-only.xisf")
    assert list(alone) == list(made) and np.array_equal(alone[REFERENCE], made[REFERENCE])
    with xisfconv.open(tmp_path / "solution-only.xisf") as file:
        assert file[0].has_astrometric_solution and len(file[0].keywords) == 0
        assert file[0].wcs_keywords()["CRVAL1"] == pytest.approx(83.82)
    # what says so goes along with copies, and with the solution into another dictionary
    image = xisfconv.read_image(path)
    for other in (image.properties.copy(), pickle.loads(pickle.dumps(image.properties)), PropertyDict() | image.properties):
        assert other.solution_of == image.properties.solution_of
    assert PropertyDict(dict(image.properties)).solution_of is None
    assert image.properties._without([key for key in image.properties if key.startswith("PCL:AstrometricSolution:")]) == rest

    # the properties of an open file say it as well: given as they are, and in a dictionary
    with xisfconv.open(path) as file:
        assert PropertyDict(file[0].properties).solution_of == image.properties.solution_of
        whole, part = file[0].read(), file[0].read()[:6]
        kept = solved(xisfconv.Image(whole, keywords=file[0].keywords, wcs_row_order="bottom-up", properties=file[0].properties))
        assert list(kept) == list(made) and kept["PCL:AstrometricSolution:CreationTime"] == made["PCL:AstrometricSolution:CreationTime"]
        for given in (file[0].properties, PropertyDict(file[0].properties), PropertyDict({"Lab:Mine": 1}) | file[0].properties):
            with pytest.warns(xisfconv.XisfconvWarning, match="has not the size"):
                assert SYSTEM not in solved(xisfconv.Image(part, properties=given))
    # A solution is one thing. A property of it that the program sets, with the others as they
    # were read: while the image is the one that was read, all of it is written ...
    image = xisfconv.read_image(path)
    image.properties[SYSTEM] = "Mercator"
    mixed = solved(image)
    assert mixed[SYSTEM] == "Mercator" and np.array_equal(mixed[REFERENCE], made[REFERENCE])
    # ... and with another image none of it: half a solution is none, and that is said aloud
    image.data = image.data[:6]
    with pytest.warns(xisfconv.XisfconvWarning, match="the 1 of its properties that were set since are left out with it"):
        mixed = solved(image)
    assert mixed[SYSTEM] == "Gnomonic" and mixed[REFERENCE][0] == pytest.approx(83.82) and mixed["Lab:Gain"] == 0.5
    image.keywords["CRVAL1"] = 83.9                       # (the keyword and the property changed together)
    image.properties[REFERENCE] = np.array([83.9, -5.39])
    with pytest.warns(xisfconv.XisfconvWarning, match="the 2 of its properties"):
        mixed = solved(image)
    assert mixed[SYSTEM] == "Gnomonic" and mixed[REFERENCE][0] == pytest.approx(83.9) and len(mixed) == len(made)
    # other formats: a FITS file has the keywords, and an image for the screen has no properties at all
    image = xisfconv.read_image(path)
    image.data = image.data[:6]
    caplog.clear()
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        xisfconv.write(tmp_path / "crop.fits", image, wcs=False)
        xisfconv.write(tmp_path / "crop.png", xisfconv.Image((image.data * 255).astype("uint8"), properties=image.properties))
    assert [record.getMessage() for record in caplog.records if "is not written" in record.getMessage()] == \
        ["%s: the astrometric solution that was read from a file is not written: the image has not the size or the WCS "
         "keywords it was read with; the file has the WCS keywords" % (tmp_path / "crop.fits")]
    with xisfconv.open(tmp_path / "crop.fits") as file:
        assert file[0].detail("carriedSolution") == "" and file[0].properties["Lab:Gain"] == 0.5
    image.keywords = None
    with pytest.warns(xisfconv.XisfconvWarning, match="and the image has no WCS keywords either"):
        xisfconv.write(tmp_path / "bare.fits", image)
    # other names for a format, and keywords in another form: what is said is still what is done
    image = xisfconv.read_image(path)
    image.data = image.data[:6]
    caplog.clear()
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        xisfconv.write(tmp_path / "crop.dat", image, format="fit")
        xisfconv.write(tmp_path / "crop.pic", xisfconv.Image((image.data * 255).astype("uint8"), properties=image.properties),
                       format="tif")
        image.keywords = [(card.name, card.value) for card in image.keywords]
        assert SYSTEM in solved(image)
    said = [record.getMessage().split("; ")[-1] for record in caplog.records if "is not written" in record.getMessage()]
    assert said == ["the file has the WCS keywords", "the solution is made from the WCS keywords, if they describe one"]
    image.keywords = {"CTYPE1": "", "CTYPE2": ""}          # keywords that name no WCS
    with pytest.warns(xisfconv.XisfconvWarning, match="the file has no astrometric solution"):
        assert SYSTEM not in solved(image)
    # an error while an image is prepared is an error, whatever was found out about its solution before
    image = xisfconv.read_image(path)
    image.data = image.data[:6]
    image.properties[SYSTEM] = "Mercator"
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        with pytest.raises(xisfconv.XisfconvWarning, match="left out with it"):
            xisfconv.write(tmp_path / "no.xisf", image)
    image.properties["Lab:Bad"] = object()
    with pytest.warns(xisfconv.XisfconvWarning), pytest.raises(TypeError):
        xisfconv.write(tmp_path / "no.xisf", image)
    assert not (tmp_path / "no.xisf").exists()
    # several images: the message names the one
    image = xisfconv.read_image(path)
    part = xisfconv.Image(image.data[:6], keywords=image.keywords, wcs_row_order="bottom-up", properties=image.properties)
    caplog.clear()
    xisfconv.write(tmp_path / "two.xisf", [image, part])
    said = [record.getMessage() for record in caplog.records if "is not written" in record.getMessage()]
    assert len(said) == 1 and "two.xisf (image 1): the astrometric solution" in said[0]
    # a dictionary that is updated with itself is what it was
    image.properties.update(image.properties)
    image.properties |= image.properties
    assert image.properties.solution_of and image.properties.type("Lab:Gain") == "Float64" and image.properties._read
    # a solution of the program's own in the place of the one that was read
    image = xisfconv.read_image(path)
    image.data = image.data[:6]
    for key in [key for key in image.properties if key.startswith("PCL:AstrometricSolution:")]:
        del image.properties[key]
    assert image.properties.solution_of is None
    image.properties[SYSTEM] = "Mercator"
    image.properties[REFERENCE] = np.array([1.0, 2.0])
    own = solved(image)
    assert own[SYSTEM] == "Mercator" and list(own[REFERENCE]) == [1.0, 2.0]
    image = xisfconv.read_image(path)
    image.data = image.data[:6]
    image.properties.update(PropertyDict({key: 0.0 for key in image.properties if key.startswith("PCL:AstrometricSolution:")}))
    assert image.properties.solution_of is None and solved(image)[SYSTEM] == 0.0
    # an array of it that is changed in place is still the one that was read
    image = xisfconv.read_image(path)
    image.properties[REFERENCE][0] = 1.0
    assert image.properties.solution_of
    assert solved(image)[REFERENCE][0] == 1.0
    image.data = image.data[:6]
    assert solved(image)[REFERENCE][0] == pytest.approx(83.82)


def test_a_solution_a_fits_file_carries_is_read_only_while_its_keywords_stand(tmp_path, caplog):
    fits = pytest.importorskip("astropy.io.fits")
    data = sample("float32", (8, 10))
    xisfconv.write(tmp_path / "sky.xisf", data, keywords=SKY, wcs_row_order="bottom-up", properties={"Lab:Gain": 0.5})
    xisfconv.convert(tmp_path / "sky.xisf", tmp_path / "sky.fits")
    with xisfconv.open(tmp_path / "sky.fits") as file:
        assert file[0].detail("carriedSolution") == "current"
    image = xisfconv.read_image(tmp_path / "sky.fits")
    assert image.properties[SYSTEM] == "Gnomonic" and image.properties.solution_of
    xisfconv.write(tmp_path / "back.xisf", image)
    with xisfconv.open(tmp_path / "back.xisf") as file:
        assert file[0].properties[REFERENCE][0] == pytest.approx(83.82)
    with xisfconv.open(tmp_path / "sky.xisf") as file:
        assert file[0].detail("carriedSolution") == ""          # (an XISF file carries nothing: it has it)
    # another program solves the frame again and writes new keywords into the FITS file
    with fits.open(tmp_path / "sky.fits", mode="update", memmap=False) as hdus:
        hdus[0].header["CRVAL1"] = 90.0
    with xisfconv.open(tmp_path / "sky.fits") as file:
        assert file[0].detail("carriedSolution") == "stale" and SYSTEM in file[0].properties
    caplog.set_level(logging.INFO, logger="xisfconv")
    image = xisfconv.read_image(tmp_path / "sky.fits")
    assert not any(key.startswith("PCL:AstrometricSolution:") for key in image.properties)
    assert image.properties["Lab:Gain"] == 0.5 and image.properties.solution_of is None
    assert any("is left out" in record.getMessage() for record in caplog.records)
    xisfconv.write(tmp_path / "back.xisf", image, overwrite=True)
    xisfconv.convert(tmp_path / "sky.fits", tmp_path / "converted.xisf")
    for name in ("back.xisf", "converted.xisf"):          # as a conversion of the file has it
        with xisfconv.open(tmp_path / name) as file:
            assert file[0].properties[REFERENCE][0] == pytest.approx(90.0) and file[0].properties["Lab:Gain"] == 0.5
            assert file[0].keywords["CRVAL1"] == 90.0
    with xisfconv.open(tmp_path / "back.xisf") as file:
        assert file[0].detail("carriedSolution") == ""
    with xisfconv.open(tmp_path / "sky.fits") as file:
        assert file[0].detail("carriedSolution") == "stale" and file[0].detail("no such detail") == ""


def test_what_the_keywords_say_comes_before_a_property_that_says_otherwise(tmp_path):
    data = sample("float32", (8, 10))
    given = {"Observation:Equinox": 1950.0, "Observation:CelestialReferenceSystem": "FK4", "Lab:Gain": 0.5}
    with pytest.warns(xisfconv.XisfconvWarning) as caught:
        xisfconv.write(tmp_path / "sky.xisf", data, keywords=SKY, wcs_row_order="bottom-up", properties=given)
    said = sorted(str(warning.message) for warning in caught)
    assert len(said) == 2 and "Observation:CelestialReferenceSystem is written as the WCS keywords have it (ICRS)" in said[0]
    assert "not as it was given (FK4)" in said[0] and "Observation:Equinox" in said[1]
    with xisfconv.open(tmp_path / "sky.xisf") as file:
        assert file[0].properties["Observation:Equinox"] == 2000.0 and file[0].properties["Lab:Gain"] == 0.5
    # the same value is nothing to speak of, and without a solution to make the property stands
    xisfconv.write(tmp_path / "same.xisf", data, keywords=SKY, wcs_row_order="bottom-up",
                   properties={"Observation:CelestialReferenceSystem": "ICRS"})
    xisfconv.write(tmp_path / "kept.xisf", data, keywords=SKY, wcs_row_order="bottom-up", properties=given, wcs=False)
    with xisfconv.open(tmp_path / "kept.xisf") as file:
        assert dict(file[0].properties) == given


IMAGE = '<Image id="odd" geometry="4:3:1" sampleFormat="UInt16" colorSpace="Gray" %s' % inline(np.arange(12, dtype="<u2"))
ODD = ('<Property id="O:Wide" type="Float128" value="1.5"/>'
       '<Property id="O:Whole" type="UInt128" value="340282366920938463463374607431768211455"/>'
       '<Property id="O:TooLarge" type="Int32" value="99999999999"/>'
       '<Property id="O:Unit" type="Float64" value="1.5 mm"/>'
       '<Property id="O:When" type="TimePoint" value="2026-10-06 18:30"/>'
       '<Property id="O:Pair" type="Complex64" value="1+2i"/>'
       '<Property id="O:NoNumber" type="Int32" value="x"/>'
       '<Property id="O:Spelled" type="Float64" value="2000"/>'
       '<Property id="O:Exponent" type="Float32" value="1.50E+02" comment="as written"/>'
       '<Property id="O:One" type="Boolean" value="1"/>'
       '<Property id="O:Padded" type="Int32" value=" 7 "/>'
       '<Property id="O:Unknown" type="Rational" value="1/3"/>')


def attributes(elements):
    """The attributes of the Property elements in a text, element by element."""
    return [dict(re.findall(r'(\w+)="([^"]*)"', element)) for element in re.findall(r"<Property [^>]*>", elements)]


def properties_text(path):
    with xisfconv.open(path) as file:
        return attributes(file.header_text.split("<Metadata")[0])


def test_what_a_file_has_is_written_as_the_file_has_it(tmp_path):
    """A property that is not touched goes back out with the text it has: also one of a type
    the library has no name for, and one whose value is not what its type says. An odd property
    does not cost the image."""
    source = handmade(tmp_path / "odd.xisf", IMAGE + ODD + "</Image>")
    image = xisfconv.read_image(source)
    values = image.properties
    assert values["O:Wide"] == 1.5 and values["O:Whole"] == 2 ** 128 - 1 and values["O:TooLarge"] == 99999999999
    assert values["O:Unit"] == "1.5 mm" and values["O:NoNumber"] == "x" and values["O:Spelled"] == 2000.0
    assert values["O:One"] is True and values["O:Padded"] == 7 and values["O:Unknown"] == "1/3"
    assert values.type("O:Wide") == "Float128" and values.type("O:Unknown") == "Rational"
    for name in ("copy.xisf", "copy.fits", "copy.asdf"):
        xisfconv.write(tmp_path / name, image)
        if not name.endswith(".xisf"):
            xisfconv.convert(tmp_path / name, tmp_path / "back.xisf", overwrite=True)
        got = properties_text(tmp_path / ("copy.xisf" if name.endswith(".xisf") else "back.xisf"))
        if name.endswith(".asdf"):
            # (the tree of an ASDF file holds values, not their spelling: true for 1, 7 for " 7 ")
            assert [(a["id"], a["type"]) for a in got] == [(a["id"], a["type"]) for a in attributes(ODD)]
            assert {a["id"]: a["value"] for a in got}["O:Wide"] == "1.5"
            with xisfconv.open(tmp_path / "back.xisf") as file:
                assert dict(file[0].properties) == dict(values)
        else:
            assert got == attributes(ODD), name
    with xisfconv.open(source) as file:               # and the properties of an open file, given as they are
        xisfconv.write(tmp_path / "direct.xisf", image.data, properties=file[0].properties)
    assert properties_text(tmp_path / "direct.xisf") == attributes(ODD)
    # zero has a sign, and not-a-number is the value it was: also in a copy
    signed = handmade(tmp_path / "signed.xisf", IMAGE + '<Property id="Z:Minus" type="Float64" value="-0"/>'
                      '<Property id="Z:Plus" type="Float64" value="0"/><Property id="Z:Nan" type="Float32" value="NaN"/>'
                      '<Property id="Z:Pair" type="Complex64" value="(-0,0)"/></Image>')
    zeros = pickle.loads(pickle.dumps(xisfconv.read_image(signed)))
    xisfconv.write(tmp_path / "zeros.xisf", zeros)
    assert [a["value"] for a in properties_text(tmp_path / "zeros.xisf")] == ["-0", "0", "NaN", "(-0,0)"]
    zeros.properties["Z:Minus"], zeros.properties["Z:Plus"], zeros.properties["Z:Pair"] = 0.0, -0.0, complex(0.0, 0.0)
    xisfconv.write(tmp_path / "zeros.xisf", zeros, overwrite=True)
    assert [a["value"] for a in properties_text(tmp_path / "zeros.xisf")] == ["0.0", "-0.0", "NaN", "(0.0,0.0)"]
    # a value that is touched is a new value: written as its type has it, or not at all
    image.properties["O:Spelled"] = 2000.0
    image.properties["O:Exponent"] = 150
    image.properties["O:One"] = True
    image.properties["O:Wide"] = 2.5
    del image.properties["O:TooLarge"], image.properties["O:Unit"], image.properties["O:When"]
    del image.properties["O:Pair"], image.properties["O:NoNumber"], image.properties["O:Padded"], image.properties["O:Unknown"]
    xisfconv.write(tmp_path / "touched.xisf", image)
    assert properties_text(tmp_path / "touched.xisf") == attributes(
        '<Property id="O:Wide" type="Float128" value="2.5"/>'
        '<Property id="O:Whole" type="UInt128" value="340282366920938463463374607431768211455"/>'
        '<Property id="O:Spelled" type="Float64" value="2000"/>'          # the same number: not touched
        '<Property id="O:Exponent" type="Float32" comment="as written" value="150.0"/>'
        '<Property id="O:One" type="Boolean" value="1"/>')
    image.properties["O:TooLarge"] = 99999999999
    image.properties.set("O:TooLarge", 99999999999, "Int32")
    with pytest.raises(xisfconv.ArgumentError, match="Int32"):
        xisfconv.write(tmp_path / "no.xisf", image)
    image.properties.set("O:TooLarge", 1, "Rational")
    with pytest.raises(ValueError, match="Rational is not a type"):
        xisfconv.write(tmp_path / "no.xisf", image)
    assert not (tmp_path / "no.xisf").exists()


def test_a_property_cannot_ask_for_more_than_the_file_could_hold(tmp_path):
    """A small file whose property declares 300 MiB: it is not read, by any way of reading."""
    packer = zlib.compressobj(9)
    packed = b"".join(packer.compress(bytes(1 << 20)) for _ in range(300)) + packer.flush()
    declared = 300 << 20
    source = handmade(tmp_path / "bomb.xisf", IMAGE +
                      '<Property id="B:Huge" type="UI8Vector" length="%d" compression="zlib:%d" location="attachment:@0"/>'
                      '<Property id="B:Fine" type="UI8Vector" length="3" %s</Property>'
                      '<Property id="B:Shape" type="F64Matrix" rows="18446744073709551615" columns="0" location="inline:base64">'
                      '</Property></Image>' % (declared, declared, inline(np.array([1, 2, 3], np.uint8))), [packed])
    assert source.stat().st_size < 1 << 20
    with xisfconv.open(source) as file:
        properties = file[0].properties
        with pytest.raises(xisfconv.FormatError, match="more data than a file of its size can hold"):
            properties["B:Huge"]
        with pytest.raises(xisfconv.FormatError, match="shape"):
            properties["B:Shape"]
        assert properties["B:Fine"].tolist() == [1, 2, 3] and properties["B:Fine"].tolist() == [1, 2, 3]
    with pytest.warns(xisfconv.XisfconvWarning) as caught:
        image = xisfconv.read_image(source)
    assert len(caught) == 2 and "B:Huge is left out" in str(caught[0].message) and "B:Shape" in str(caught[1].message)
    assert image.properties["B:Huge"] is None and image.properties["B:Shape"] is None
    assert image.properties["B:Fine"].tolist() == [1, 2, 3] and image.data.shape == (3, 4)
    with pytest.warns(xisfconv.XisfconvWarning):
        from xisfconv.xisf import XISF
        assert list(XISF(source).get_images_metadata()[0]["XISFProperties"]) == ["B:Fine"]


def test_long_texts_are_data_blocks(tmp_path):
    long_text = "word " * 2000 + "end"
    short_text = "x" * 3072
    path = tmp_path / "text.xisf"
    xisfconv.write(path, sample("uint8", (4, 4)), properties={"T:Long": long_text, "T:Short": short_text}, codec="zlib")
    with xisfconv.open(path) as file:
        header = file.header_text
        assert file[0].properties["T:Long"] == long_text and file[0].properties["T:Short"] == short_text
    assert re.search(r'<Property id="T:Long" type="String" location="attachment:\d+:\d+" compression="zlib:%d"/>' % len(long_text),
                     header)
    assert '<Property id="T:Short" type="String">' + short_text + "</Property>" in header
    assert path.stat().st_size < 12000
    image = xisfconv.read_image(path)                     # and read and written again
    xisfconv.write(tmp_path / "again.xisf", image)
    with xisfconv.open(tmp_path / "again.xisf") as file:
        assert file[0].properties["T:Long"] == long_text and 'id="T:Long" type="String" location="attachment:' in file.header_text


def test_keyword_text_beyond_ascii(tmp_path):
    """An XISF file is XML: what its keywords say beyond ASCII is kept. A FITS header is not."""
    cards = {"OBSERVER": ("Jürgen", "at 20° and 3 µm"), "OBJECT": ("Nordamerikanebel ☃", "")}
    xisfconv.write(tmp_path / "k.xisf", sample("uint8", (4, 4)), keywords=cards)
    for name in ("k.xisf", "again.xisf"):
        with xisfconv.open(tmp_path / name) as file:
            keywords = file[0].keywords
            assert keywords["OBSERVER"] == "Jürgen" and keywords["OBJECT"] == "Nordamerikanebel ☃"
            assert keywords[0].comment == "at 20° and 3 µm"
        xisfconv.write(tmp_path / "again.xisf", xisfconv.read_image(tmp_path / name), overwrite=True)
    xisfconv.rewrite(tmp_path / "k.xisf", tmp_path / "rewritten.xisf")
    assert xisfconv.read_image(tmp_path / "rewritten.xisf").keywords["OBSERVER"] == "Jürgen"
    xisfconv.convert(tmp_path / "k.xisf", tmp_path / "k.fits")
    with xisfconv.open(tmp_path / "k.fits") as file:
        assert all(ord(c) < 128 for card in file[0].keywords for c in str(card.value) + card.comment)
        assert file[0].keywords["OBSERVER"].startswith("J") and "rgen" in file[0].keywords["OBSERVER"]


def test_keywords_go_out_as_they_came_in(tmp_path):
    """Read and written again, the WCS keywords are the text they were: nothing is computed
    with them on the way, whichever way up the rows are handed over."""
    fits = pytest.importorskip("astropy.io.fits")
    rng = np.random.default_rng(3)
    for number in range(40):
        height = int(rng.integers(5, 4000))
        cards = [("CTYPE1", "'RA---TAN'"), ("CTYPE2", "'DEC--TAN'"), ("CRVAL1", "%.13f" % rng.uniform(0, 360)),
                 ("CRVAL2", "%.13f" % rng.uniform(-80, 80)), ("CRPIX1", "%.13f" % rng.uniform(1, 3000)),
                 ("CRPIX2", "%.15E" % rng.uniform(1, height)), ("CD1_1", "%.15E" % rng.normal(0, 1e-4)),
                 ("CD1_2", "%.15e" % rng.normal(0, 1e-4)), ("CD2_1", "%.16E" % rng.normal(0, 1e-4)),
                 ("CD2_2", "%.12E" % rng.normal(0, 1e-4)), ("BAYERPAT", "'RGGB'")]
        pixels = np.zeros((height, 3), "<u2")
        source = handmade(tmp_path / "wcs.xisf",
                          '<Image geometry="3:%d:1" sampleFormat="UInt16" colorSpace="Gray" %s' % (height, inline(pixels)) +
                          "".join('<FITSKeyword name="%s" value="%s" comment=""/>' % card for card in cards) + "</Image>")
        for row_order in ("top-down", "bottom-up"):
            image = xisfconv.read_image(source, row_order=row_order)
            xisfconv.write(tmp_path / "copy.xisf", image, wcs=False, overwrite=True)
            with xisfconv.open(tmp_path / "copy.xisf") as file:
                written = re.findall(r'<FITSKeyword name="(\w+)" value="([^"]*)"', file.header_text)
            # (BAYERPAT is turned over with the rows and back again: the same pattern, written anew)
            assert written[:-1] == cards[:-1] and written[-1][1].replace(" ", "") == "'RGGB'", (number, row_order)
            assert written == cards or row_order == "bottom-up"
    # the same through FITS: a file that is read and written again has the cards it had
    xisfconv.convert(source, tmp_path / "wcs.fits")
    for row_order in ("top-down", "bottom-up"):
        xisfconv.write(tmp_path / "copy.fits", xisfconv.read_image(tmp_path / "wcs.fits", row_order=row_order), overwrite=True)
        with fits.open(tmp_path / "wcs.fits", memmap=False) as a, fits.open(tmp_path / "copy.fits", memmap=False) as b:
            for name in ("CRVAL1", "CRPIX1", "CRPIX2", "CD1_1", "CD1_2", "CD2_1", "CD2_2", "BAYERPAT"):
                assert str(a[0].header.cards[name]) == str(b[0].header.cards[name]), (name, row_order)


def test_lz4_levels_and_shuffling(tmp_path):
    rng = np.random.default_rng(5)
    data = (np.add.outer(np.arange(90), np.arange(120)) * 3 % 2000 + rng.integers(0, 5, (90, 120))).astype(np.uint16)
    path = tmp_path / "codec.xisf"
    sizes = {}
    for codec in ("lz4", "lz4hc", "zlib") + (("zstd",) if xisfconv.codec_available("zstd", writing=True) else ()):
        for shuffle in (True, False):
            xisfconv.write(path, data, codec=codec, shuffle=shuffle, overwrite=True)
            with xisfconv.open(path) as file:
                assert file[0].detail("compression").startswith("%s%s:%d" % (codec, "+sh" if shuffle else "", data.nbytes))
                assert same(file[0].read(), data)
                assert file.properties["XISF:CompressionCodecs"] == codec + ("+sh" if shuffle else "")
                sizes[codec, shuffle] = int(file[0].detail("location").split(":")[2])
            assert xisfconv.verify(path).verdict == "ok"
    assert sizes["lz4hc", True] < sizes["lz4", True] < data.nbytes
    assert sizes["lz4", True] < sizes["lz4", False]            # which is what the shuffling is for
    # levels
    by_level = {}
    for level in (1, 9, 12):
        xisfconv.write(path, data, codec="lz4hc", level=level, overwrite=True)
        with xisfconv.open(path) as file:
            assert same(file[0].read(), data) and "XISF:CompressionLevel" not in file.properties
            by_level[level] = int(file[0].detail("location").split(":")[2])
    assert by_level[12] <= by_level[9] < by_level[1] and by_level[9] == sizes["lz4hc", True]
    xisfconv.write(path, data, codec="zlib", level=np.int64(9), overwrite=True)
    for options, error in ((dict(codec="zlib", level=10), xisfconv.ArgumentError),
                           (dict(codec="lz4hc", level=13), xisfconv.ArgumentError),
                           (dict(codec="lz4", level=1), xisfconv.ArgumentError),
                           (dict(level=3), xisfconv.ArgumentError), (dict(codec="zlib", level=0), ValueError),
                           (dict(codec="zlib", level=-1), ValueError), (dict(codec="zlib", level=True), TypeError),
                           (dict(codec="zlib", level=2.5), TypeError), (dict(codec="zlib", level="9"), TypeError)):
        with pytest.raises(error):
            xisfconv.write(path, data, overwrite=True, **options)
    with pytest.raises(xisfconv.ArgumentError, match="1 to 9"):
        xisfconv.write(path, data, codec="zlib", level=10, overwrite=True)
    # a conversion and a rewrite with LZ4
    xisfconv.write(tmp_path / "plain.fits", data)
    xisfconv.convert(tmp_path / "plain.fits", tmp_path / "from-fits.xisf", codec="lz4hc")
    with xisfconv.open(tmp_path / "from-fits.xisf") as file:
        assert file[0].detail("compression").startswith("lz4hc+sh:") and same(file[0].read(), data)
    result = xisfconv.rewrite(tmp_path / "from-fits.xisf", tmp_path / "rewritten.xisf", codec="lz4")
    assert result.changed and result.compressed == 1 and result.read_back
    assert xisfconv.stored_as_requested(tmp_path / "rewritten.xisf", codec="lz4")
    assert not xisfconv.stored_as_requested(tmp_path / "rewritten.xisf", codec="lz4hc")
    assert same(xisfconv.read(tmp_path / "rewritten.xisf"), data)
    with pytest.raises(xisfconv.ArgumentError, match="XISF only"):
        xisfconv.convert(tmp_path / "from-fits.xisf", tmp_path / "no.asdf", codec="lz4")
    with pytest.raises(xisfconv.ArgumentError, match="no LZ4"):
        xisfconv.convert(tmp_path / "from-fits.xisf", tmp_path / "no.fits", codec="lz4")
    assert not (tmp_path / "no.asdf").exists() and not (tmp_path / "no.fits").exists()
    # the properties are compressed with the codec too, and the xisf package reads them
    pytest.importorskip("xisf")
    many = np.arange(4000, dtype=np.float64) % 17
    xisfconv.write(path, data, codec="lz4hc", properties={"Lab:Many": many}, overwrite=True)
    theirs = xisf_read(path)[1]["XISFProperties"]["Lab:Many"]
    assert theirs["compression"][0] == "lz4hc+sh" and equal(np.array(theirs["value"]), many)
    assert same(xisf_read(path)[0][:, :, 0], data)
