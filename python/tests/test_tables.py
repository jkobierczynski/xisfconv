# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""XISF tables and data blocks of types without a name here (0.25): read into xisfconv.Table and
xisfconv.DataBlock, written again by write and Writer; tables in the forms the specification has
(their own structure, a structure at the root they refer to, a table at the root an image refers
to); what is not read; what conversions do with them."""

import datetime
import warnings

import numpy as np
import pytest

import xisfconv
from util import handmade, inline, sample

STARS = xisfconv.Table([("name", "String", "Star"), ("ra", "Float64", "RA (deg)", "%.4f"), ("n", "Int32"),
                        ("seen", "TimePoint"), ("ok", "Boolean"), ("z", "Complex64"), ("small", "UInt8")],
                       [("Vega", 279.2347, 1, "2026-10-12T20:00:00Z", True, 1 + 2j, 255),
                        ("De\tneb\r\n x  ", 310.358, -2, "2026-10-13", False, 0j, 0),
                        ("", float("inf"), 0, "2026-10-13T01:02:03.5+02:00", True, -1.5j, 7)], caption="Bright <stars> & \"more\"")


def test_tables_and_blocks_are_written_and_read_again(tmp_path):
    block = xisfconv.DataBlock("Float128Vector", bytes(range(32)), rows=2)
    big = xisfconv.DataBlock("Q32Matrix", bytes(range(24)), rows=2, columns=3, big_endian=True)
    properties = xisfconv.PropertyDict({"Cat:Stars": STARS, "Odd:Quad": block, "Odd:Big": big, "A:Number": 5})
    properties.set("Cat:Stars", STARS, comment="from a catalog")
    whole = xisfconv.PropertyDict({"File:Table": xisfconv.Table([("a", "UInt16")], [(1,), (2,), (65535,)])})
    path = tmp_path / "tables.xisf"
    xisfconv.write(path, sample("uint16", (5, 4)), properties=properties, file_properties=whole)
    with xisfconv.open(path) as f:
        got = f[0].properties
        assert list(got) == ["Cat:Stars", "Odd:Quad", "Odd:Big", "A:Number"]
        assert got["Cat:Stars"] == STARS and got.type("Cat:Stars") == "Table" and got.comment("Cat:Stars") == "from a catalog"
        assert got["Odd:Quad"] == block and got["Odd:Big"] == big and got.type("Odd:Big") == "Q32Matrix"
        assert f.properties["File:Table"] == whole["File:Table"]
        assert f.properties["File:Table"].column("a") == [1, 2, 65535]
    image = xisfconv.read_image(path)
    assert image.properties["Cat:Stars"] == STARS and image.properties.comment("Cat:Stars") == "from a catalog"
    # written again as it was read, by write and by Writer
    xisfconv.write(tmp_path / "again.xisf", image, file_properties=xisfconv.open(path).properties)
    with xisfconv.Writer(tmp_path / "writer.xisf") as writer:
        writer.add(image)
    for name in ("again.xisf", "writer.xisf"):
        again = xisfconv.read_image(tmp_path / name)
        assert again.properties["Cat:Stars"] == STARS and again.properties["Odd:Quad"] == block
        assert again.properties["Odd:Big"] == big
    assert xisfconv.open(tmp_path / "again.xisf").properties["File:Table"] == whole["File:Table"]
    # a rewrite keeps them as they are
    xisfconv.rewrite(path, tmp_path / "packed.xisf", codec="zlib")
    assert xisfconv.read_image(tmp_path / "packed.xisf").properties["Cat:Stars"] == STARS
    # cells from Python values of every kind
    when = datetime.datetime(2026, 10, 12, 20, 0, tzinfo=datetime.timezone.utc)
    typed = xisfconv.Table([("t", "TimePoint"), ("f", "Float32"), ("i", "Int64")], [(when, np.float32(0.1), 2 ** 40)])
    xisfconv.write(tmp_path / "typed.xisf", sample("uint8", (2, 2)), properties={"T": typed})
    assert xisfconv.read_image(tmp_path / "typed.xisf").properties["T"].rows == [("2026-10-12T20:00:00Z", 0.1, 2 ** 40)]


def test_what_is_not_a_table_that_is_written(tmp_path):
    data = sample("uint8", (2, 2))
    path = tmp_path / "bad.xisf"
    for table, error in ((xisfconv.Table([]), ValueError),
                         (xisfconv.Table([("a", "F64Vector")], [([1.0],)]), ValueError),
                         (xisfconv.Table([("a", "Int32"), ("a", "Int32")], [(1, 2)]), xisfconv.ArgumentError),
                         (xisfconv.Table([("a", "Int32")], [("seven",)]), TypeError),
                         (xisfconv.Table([("a", "UInt8")], [(300,)]), xisfconv.ArgumentError),
                         (xisfconv.Table([("a", "Int32")], [(1, 2)]), ValueError),
                         (xisfconv.Table([("a", "Int32")], [(None,)]), ValueError),
                         (xisfconv.Table([("", "Int32")], [(1,)]), xisfconv.ArgumentError),
                         (xisfconv.Table([("a", "String")], [("bell \x07",)]), xisfconv.ArgumentError)):
        with pytest.raises(error):
            xisfconv.write(path, data, properties={"T": table}, overwrite=True)
    with pytest.raises(ValueError, match="Table"):
        xisfconv.write(path, data, properties=_typed("T", "not a table", "Table"), overwrite=True)
    for block in (xisfconv.DataBlock("Int32", b"1234"), xisfconv.DataBlock("F64Vector", bytes(8)),
                  xisfconv.DataBlock("String", b"x"), xisfconv.DataBlock("Table", b"")):
        with pytest.raises(xisfconv.ArgumentError, match="has a name here"):
            xisfconv.write(path, data, properties={"B": block}, overwrite=True)
    with pytest.raises(TypeError):
        xisfconv.DataBlock("", b"")


def _typed(key, value, kind):
    properties = xisfconv.PropertyDict()
    properties.set(key, value, type=kind)
    return properties


IMAGE = '<Image id="t" geometry="2:2:1" sampleFormat="UInt8" colorSpace="Gray" %s</Image>'


def test_the_forms_of_the_specification(tmp_path):
    pixels = inline(np.arange(4, dtype=np.uint8))
    elements = (
        '<Structure uid="S1"><Field id="name" type="String" header="Name"/><Field id="v" type="Float64"/></Structure>'
        '<Table uid="T1" id="Shared:Table" caption="shared"><Reference ref="S1"/><Row><Cell>a</Cell><Cell value="1.5"/></Row></Table>'
        + IMAGE.replace("</Image>", "") % pixels +
        '<Property id="P:One" type="Int32" value="1"/>'
        '<Table id="Own:Table"><Structure><Field id="s" type="String"/><Field id="b" type="Boolean"/></Structure>'
        '<Row><Cell value="old form"/><Cell value="true"/></Row><Row><Cell>new form</Cell><Cell value="false"/></Row></Table>'
        '<Reference ref="T1"/>'
        '<Table id="Vec:Table"><Structure><Field id="v" type="F64Vector"/></Structure></Table>'
        '<Table id="Short:Row"><Structure><Field id="a" type="Int32"/><Field id="b" type="Int32"/></Structure>'
        '<Row><Cell value="1"/></Row></Table>'
        '<Table id="Block:Cell"><Structure><Field id="a" type="String"/></Structure>'
        '<Row><Cell location="inline:base64">eA==</Cell></Row></Table>'
        '<Table id="Rows:Wrong" rows="5"><Structure><Field id="a" type="Int32"/></Structure><Row><Cell value="1"/></Row></Table>'
        '<Table id="No:Structure"><Row/></Table>'
        '<Property id="Odd:Bytes" type="Q64Vector" length="1" ' + inline(np.array([1.0], "<f8")) + '</Property>'
        '<Property id="Odd:Big" type="Q64Vector" length="1" byteOrder="big" ' + inline(np.array([1.0], ">f8")) + '</Property>'
        '</Image>'
        '<Table id="File:Table"><Structure><Field id="x" type="UInt16"/></Structure><Row><Cell value="7"/></Row></Table>'
        '<Metadata><Property id="XISF:CreatorApplication" type="String">hand</Property></Metadata>')
    path = handmade(tmp_path / "forms.xisf", elements)
    with xisfconv.open(path) as f:
        p = f[0].properties
        assert list(p) == ["P:One", "Own:Table", "Shared:Table", "Vec:Table", "Short:Row", "Block:Cell", "Rows:Wrong",
                           "No:Structure", "Odd:Bytes", "Odd:Big"]
        assert p["Own:Table"].rows == [("old form", True), ("new form", False)]
        shared = p["Shared:Table"]
        assert shared.caption == "shared" and shared.fields[0] == xisfconv.TableField("name", "String", "Name", "")
        assert shared.rows == [("a", 1.5)]
        for unread in ("Vec:Table", "Short:Row", "Block:Cell", "Rows:Wrong", "No:Structure"):
            assert p[unread] is None and p.type(unread) == "Table", unread
        assert p["Odd:Bytes"] == xisfconv.DataBlock("Q64Vector", np.array([1.0], "<f8").tobytes(), 1)
        p_big = xisfconv.DataBlock("Q64Vector", np.array([1.0], ">f8").tobytes(), 1, big_endian=True)
        assert p["Odd:Big"] == p_big
        # a table at the root that an image refers to is the image's; one no image refers to, the file's
        assert list(f.properties) == ["File:Table", "XISF:CreatorApplication"]
        assert f.properties["File:Table"].rows == [(7,)]
    image = xisfconv.read_image(path)
    assert image.properties["Shared:Table"] == shared and image.properties["Vec:Table"] is None
    with warnings.catch_warnings(record=True) as told:
        warnings.simplefilter("always")
        xisfconv.write(tmp_path / "again.xisf", image)
    said = sorted(str(w.message) for w in told)
    assert len(said) == 5 and all("(a Table) has no value and is not written" in line for line in said), said
    with xisfconv.open(tmp_path / "again.xisf") as f:
        assert list(f[0].properties) == ["P:One", "Own:Table", "Shared:Table", "Odd:Bytes", "Odd:Big"]
        assert f[0].properties["Shared:Table"] == shared and f[0].properties["Odd:Big"] == p_big


def test_conversions(tmp_path):
    """FITS and ASDF carry a data block of a type without a name as its bytes (not big-endian
    ones), and no tables: a warning says so."""
    block = xisfconv.DataBlock("Float128Vector", bytes(range(16)), rows=1)
    big = xisfconv.DataBlock("Float128Vector", bytes(range(16)), rows=1, big_endian=True)
    path = tmp_path / "source.xisf"
    xisfconv.write(path, sample("uint16", (4, 4)), properties={"T": STARS, "B": block, "Big": big},
                   file_properties={"F": STARS})
    for ending in ("fits", "asdf"):
        out = tmp_path / ("out." + ending)
        with pytest.warns(xisfconv.XisfconvWarning) as told:
            xisfconv.convert(path, out)
        said = " ".join(str(w.message) for w in told)
        assert "table T is not carried" in said and "table F is not carried" in said and "property Big" in said
        with xisfconv.open(out) as f:
            assert dict(f[0].properties) == {"B": block} and len(f.properties) == 0
        back = tmp_path / ("back-" + ending + ".xisf")
        xisfconv.convert(out, back)
        assert xisfconv.read_image(back).properties["B"] == block
    # the writer to FITS: the same
    with pytest.warns(xisfconv.XisfconvWarning, match="table T is not carried to FITS"):
        xisfconv.write(tmp_path / "w.fits", sample("uint16", (4, 4)), properties={"T": STARS, "B": block})
    assert xisfconv.read_image(tmp_path / "w.fits").properties["B"] == block


def test_the_interface_of_the_xisf_package_leaves_tables_out(tmp_path):
    from xisfconv.xisf import XISF
    path = tmp_path / "t.xisf"
    xisfconv.write(path, sample("uint16", (4, 4)), properties={"T": STARS, "A": 1}, file_properties={"F": STARS, "G": 2})
    with pytest.warns(xisfconv.XisfconvWarning) as told:
        xisf = XISF(str(path))
        meta = xisf.get_images_metadata()[0]
        about = xisf.get_file_metadata()
    said = " ".join(str(w.message) for w in told)
    assert "T is left out: it is a table" in said and "F is left out: it is a table" in said
    assert list(meta["XISFProperties"]) == ["A"] and "G" in about and "F" not in about


ODD = ('<Table id="Odd:Cells" caption="odd"><Structure><Field id="i" type="Int32"/><Field id="u" type="UInt8"/>'
       '<Field id="b" type="Boolean"/><Field id="s" type="String"/><Field id="i" type="Int32"/></Structure>'
       '<Row><Cell value="1.0"/><Cell value="256"/><Cell value="yes"/><Cell>a&#9;b&#13;</Cell><Cell value=" 7 "/></Row>'
       '</Table>')


def test_a_table_that_was_read_is_written_again_as_the_file_has_it(tmp_path):
    """Cells that are not values of their type, and fields with the same id, come back as the
    file has them, through read_image and write; a table that is changed is written as it is
    now."""
    path = handmade(tmp_path / "odd.xisf", IMAGE % (inline(np.arange(4, dtype=np.uint8)) + ODD))
    image = xisfconv.read_image(path)
    table = image.properties["Odd:Cells"]
    assert table.rows == [("1.0", 256, False, "a\tb\r", 7)]
    xisfconv.write(tmp_path / "again.xisf", image)
    with xisfconv.open(tmp_path / "again.xisf") as f:
        again = f[0].properties["Odd:Cells"]
        assert again == table and [field.id for field in again.fields] == ["i", "u", "b", "s", "i"]
    assert b'value="yes"' in (tmp_path / "again.xisf").read_bytes()
    # changed in place: written as it is now, and checked
    table.rows[0] = (1, 255, True, "x", 7)
    with pytest.raises(xisfconv.ArgumentError, match="two fields have the id i"):
        xisfconv.write(tmp_path / "changed.xisf", image)
    table.fields[4] = xisfconv.TableField("j", "Int32")
    xisfconv.write(tmp_path / "changed.xisf", image)
    assert xisfconv.read_image(tmp_path / "changed.xisf").properties["Odd:Cells"].rows == [(1, 255, True, "x", 7)]


def test_many_references_to_one_table(tmp_path):
    rows = "".join('<Row><Cell value="%d"/></Row>' % n for n in range(2000))
    shared = '<Table uid="T" id="Shared"><Structure><Field id="n" type="Int32"/></Structure>%s</Table>' % rows
    references = '<Reference ref="T"/>' * 3000
    path = handmade(tmp_path / "many.xisf", shared + IMAGE % (inline(np.arange(4, dtype=np.uint8)) + references))
    with xisfconv.open(path) as f:
        p = f[0].properties
        assert len(p._load()) == 1 and p["Shared"].column("n")[-1] == 1999


def test_the_xisf_package_takes_uids_as_the_library_does(tmp_path):
    from xisfconv.xisf import XISF
    elements = ('<Table uid="T" id="First"><Structure><Field id="n" type="Int32"/></Structure></Table>'
                '<Table uid="T" id="Second"><Structure><Field id="n" type="Int32"/></Structure></Table>'
                '<Table uid="" id="Empty"><Structure><Field id="n" type="Int32"/></Structure></Table>'
                + IMAGE % (inline(np.arange(4, dtype=np.uint8)) + '<Reference ref="T"/><Reference/>'))
    path = handmade(tmp_path / "uids.xisf", elements)
    with xisfconv.open(path) as f:
        assert list(f[0].properties) == ["First", "Empty"] and list(f.properties) == ["Second"]
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        xisf = XISF(str(path))
        assert xisf.get_images_metadata()[0]["XISFProperties"] == {}
        assert "Second" not in xisf.get_file_metadata()
