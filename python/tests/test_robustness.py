# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Things that went wrong once, or could: lifetimes, changed files, odd keywords, wrong arguments."""

import gc
import os
import sys
import warnings

import numpy as np
import pytest

import xisfconv
from xisfconv import Keywords
from util import sample, same

fits = pytest.importorskip("astropy.io.fits")

linux_only = pytest.mark.skipif(not sys.platform.startswith("linux"), reason="counts open files through /proc")


def open_files():
    return len(os.listdir("/proc/self/fd"))


def patched(source, target, old, new):
    """A copy of a file with one byte string replaced by another of the same length."""
    assert len(old) == len(new)
    raw = source.read_bytes()
    assert raw.count(old) >= 1
    target.write_bytes(raw.replace(old, new, 1))


# --- lifetimes --------------------------------------------------------------------------------

def test_a_file_without_images(tmp_path):
    """Closing such a file and dropping it used to free the handle twice."""
    fits.PrimaryHDU(np.zeros(7, np.float32)).writeto(tmp_path / "spectrum.fits")      # 1-D: not an image
    for _ in range(20):
        with pytest.raises(xisfconv.FormatError, match="holds no image"):
            xisfconv.read(tmp_path / "spectrum.fits")
        with xisfconv.open(tmp_path / "spectrum.fits") as file:
            assert len(file) == 0 and list(file) == [] and file.images == ()
        del file
        file = xisfconv.open(tmp_path / "spectrum.fits")
        file.close()
        file.close()
        del file
        gc.collect()
    assert same(xisfconv.auto_stretch(sample("uint8", (4, 4)))[0].highlights, 1.0)   # the process is well


def test_closed_and_collected_files(tmp_path):
    path = tmp_path / "f.xisf"
    data = sample("uint16", (6, 7))
    xisfconv.write(path, data, keywords={"OBJECT": "M 5"})
    # what was taken from a file outlives it
    file = xisfconv.open(path)
    entry, keywords, properties = file[0], file[0].keywords, file[0].properties
    del file
    gc.collect()
    assert same(entry.read(), data) and keywords["OBJECT"] == "M 5" and len(properties) == 0
    entry._file.close()
    with pytest.raises(ValueError, match="closed"):
        entry.read()
    assert keywords["OBJECT"] == "M 5"

    # An object in a reference cycle that uses its file when it is collected: the collector has
    # freed the file's handle by then, and the file says that it is closed.
    seen = []

    class Viewer:
        def __init__(self):
            self.file = xisfconv.open(path)
            self.me = self

        def __del__(self):
            try:
                seen.append(self.file.closed)
                self.file[0].name
                seen.append("read after the handle was freed")
            except ValueError as e:
                seen.append(str(e))
            self.file.close()

    Viewer()
    gc.collect()
    assert seen == [True, "the file is closed"]


@linux_only
def test_files_are_closed_without_the_collector(tmp_path):
    path = tmp_path / "f.xisf"
    xisfconv.write(path, sample("uint8", (4, 4)))
    before = open_files()
    gc.disable()
    try:
        for _ in range(100):
            file = xisfconv.open(path)
            assert file[0].name and same(file[0].read(), xisfconv.read(path))
            del file
        assert open_files() <= before + 1
    finally:
        gc.enable()


@linux_only
def test_nothing_leaks_when_a_warning_is_an_error(tmp_path):
    """Opening a file that the library warns about, with warnings turned into errors: the open
    fails, and the handle the library had already made is given back."""
    xisfconv.write(tmp_path / "good.xisf", sample("uint8", (4, 4)))
    patched(tmp_path / "good.xisf", tmp_path / "newer.xisf", b'<xisf version="1.0"', b'<xisf version="1.1"')
    with pytest.warns(xisfconv.XisfconvWarning):
        xisfconv.open(tmp_path / "newer.xisf").close()
    before = open_files()
    with warnings.catch_warnings():
        warnings.simplefilter("error", xisfconv.XisfconvWarning)
        for _ in range(50):
            with pytest.raises(xisfconv.XisfconvWarning):
                xisfconv.open(tmp_path / "newer.xisf")
            with pytest.raises(xisfconv.XisfconvWarning):
                xisfconv.verify(tmp_path / "newer.xisf")
    assert open_files() <= before + 1


def test_a_file_replaced_after_it_was_opened(tmp_path):
    """FITS pixels are read from the file again when asked for: if another image is there by
    then, that is an error, not a buffer half filled."""
    path = tmp_path / "stale.fits"
    xisfconv.write(path, np.full((6, 8), 7, np.uint16))
    file = xisfconv.open(path)
    xisfconv.write(path, np.array([[1, 2], [3, 4]], np.uint16), overwrite=True)
    with pytest.raises(xisfconv.FormatError, match="changed"):
        file[0].read()
    xisfconv.write(path, np.full((6, 8), 9, np.uint16), overwrite=True)     # the same shape again: readable
    assert same(file[0].read(), np.full((6, 8), 9, np.uint16))
    file.close()


def test_the_working_directory_may_change(tmp_path, monkeypatch):
    data = sample("uint16", (5, 6))
    monkeypatch.chdir(tmp_path)
    xisfconv.write("relative.fits", data)
    xisfconv.write("relative.xisf", data)
    with xisfconv.open("relative.fits") as fits_file, xisfconv.open("relative.xisf") as xisf_file:
        (tmp_path / "elsewhere").mkdir()
        monkeypatch.chdir(tmp_path / "elsewhere")
        assert same(fits_file[0].read(), data) and same(xisf_file[0].read(), data)


# --- keywords ---------------------------------------------------------------------------------

def test_cards_without_a_name(tmp_path):
    """astropy leaves blank cards in a header, and files hold cards of text only."""
    hdu = fits.PrimaryHDU(sample("uint16", (4, 5)))
    hdu.header["OBJECT"] = "M 9"
    hdu.header[""] = "a card of text only"
    hdu.header.append(("", "", ""), end=True)                              # an entirely blank card
    hdu.writeto(tmp_path / "blank.fits")
    image = xisfconv.read_image(tmp_path / "blank.fits")
    assert image.keywords[""] == "a card of text only"
    for extension in ("fits", "xisf", "asdf"):
        out = tmp_path / ("out." + extension)
        xisfconv.write(out, image)
        back = xisfconv.read_image(out).keywords
        assert back["OBJECT"] == "M 9" and "a card of text only" in [card.value for card in back.cards("")]
    with fits.open(tmp_path / "out.fits") as hdus:
        hdus.verify("exception")
        assert "a card of text only" in list(hdus[0].header[""])
    with fits.open(tmp_path / "blank.fits") as hdus:                       # the header as astropy has it
        xisfconv.write(tmp_path / "direct.xisf", hdus[0].data, keywords=hdus[0].header, row_order="bottom-up")
    assert xisfconv.read_image(tmp_path / "direct.xisf").keywords["OBJECT"] == "M 9"
    assert Keywords([("", "text")]).fits_text().rstrip() == "        text"


def test_cards_that_fits_cannot_hold(tmp_path):
    data = sample("uint8", (4, 4))
    long_name = "X" * 60
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        xisfconv.write(tmp_path / "odd.fits", data, keywords=[
            (long_name, "a text that is too long for what is left of the card"), ("A=B", "v"), ("PLAIN", 1),
            ("Lower Case Name", "kept")])
    told = " ".join(str(w.message) for w in caught)
    assert "truncated" in told and "A=B" in told and "skipped" in told
    with fits.open(tmp_path / "odd.fits") as hdus:
        hdus.verify("exception")                                           # every card that was written is valid
        header = hdus[0].header
        assert header["PLAIN"] == 1 and header["Lower Case Name"] == "kept" and "A=B" not in header and "A" not in header
        assert "a text that is too long".startswith(header[long_name]) and len(header[long_name]) >= 1
    # so long a name that not even an empty string fits behind it
    with pytest.warns(xisfconv.XisfconvWarning, match="skipped"):
        assert Keywords([("Y" * 68, "text")]).fits_text() == ""


def test_long_text_that_ends_in_an_ampersand(tmp_path):
    """Long text is split over cards, each piece but the last ending in '&'. A text that itself
    ends in '&' must keep it, also when astropy reads the cards."""
    texts = ["x" * n + tail for n in (0, 5, 60, 65, 66, 67, 68, 69, 70, 130, 131, 132, 133, 134, 135, 200)
             for tail in ("&", "&&", "a&", "'&", "&'", "& ", "&b")]
    cards = Keywords([("T%d" % n, text, "c") for n, text in enumerate(texts)])
    header = fits.Header.fromstring(cards.fits_text())
    xisfconv.write(tmp_path / "amp.fits", sample("uint8", (2, 2)), keywords=cards)
    xisfconv.convert(tmp_path / "amp.fits", tmp_path / "amp.xisf")
    back = xisfconv.read_image(tmp_path / "amp.fits").keywords
    through = xisfconv.read_image(tmp_path / "amp.xisf").keywords
    with fits.open(tmp_path / "amp.fits") as hdus:
        hdus.verify("exception")
        for n, text in enumerate(texts):
            name = "T%d" % n
            assert header[name] == hdus[0].header[name] == back[name] == through[name] == text.rstrip(), (n, text[-6:])


def test_text_keywords_after_commentary(tmp_path):
    """A COMMENT, HISTORY or blank card before a text keyword, as CFITSIO writes headers: the
    reader used the same variable for the two and failed on the text keyword."""
    header = fits.Header()
    header["COMMENT"] = "flat-fielded with master_flat_3"
    header.append(("OBJECT", "M 31"), end=True)
    header["HISTORY"] = "stacked"
    header.append(("", "a blank card"), end=True)
    header.append(("FILTER", "Ha", "narrow band"), end=True)
    header.append(("GAIN", 1.5), end=True)
    data = sample("uint16", (4, 5))
    fits.PrimaryHDU(data, header=header).writeto(tmp_path / "c.fits")
    cards = [("COMMENT", "first"), ("OBJECT", "M 31"), ("HISTORY", "second"), ("", "third"), ("FILTER", "Ha", "narrow band"),
             ("COMMENT", "fourth"), ("NOTE", "x" * 100), ("GAIN", 1.5)]
    xisfconv.write(tmp_path / "c.xisf", data, keywords=cards)
    for name in ("c.fits", "c.xisf"):
        image = xisfconv.read_image(tmp_path / name)
        assert image.keywords["OBJECT"] == "M 31" and image.keywords["FILTER"] == "Ha" and image.keywords["GAIN"] == 1.5, name
        assert image.keywords.cards("FILTER")[0].comment == "narrow band"
        with xisfconv.open(tmp_path / name) as file:
            assert file[0].keywords["OBJECT"] == "M 31"
            assert file[0].fits_keywords()["FILTER"] == "Ha"
    assert list(xisfconv.read_image(tmp_path / "c.xisf").keywords) == [Card for Card in Keywords(cards)]
    xa = pytest.importorskip("xisfconv.astropy")
    assert xa.read_hdulist(tmp_path / "c.fits")[0].header["OBJECT"] == "M 31"
    assert xa.read_hdulist(tmp_path / "c.xisf")[0].header["NOTE"] == "x" * 100


def test_text_without_quotes_is_quoted(tmp_path):
    """An XISF keyword may hold  Ha  where 'Ha' is meant; a FITS card needs the quotes."""
    pytest.importorskip("xisf")
    from util import xisf_write

    xisf_write(tmp_path / "bare.xisf", sample("uint16", (4, 4)), keywords={
        "FILTER": ("Ha", "no quotes"), "DATE": ("2026-01-01", ""), "HEX": ("0x1F", ""), "NOTNUM": ("NaN", ""),
        "REAL": ("1.5e3", ""), "INT": ("-12", ""), "LOGIC": ("T", ""), "PAIR": ("(1.0, 2.5)", "")})
    xisfconv.convert(tmp_path / "bare.xisf", tmp_path / "bare.fits")
    with fits.open(tmp_path / "bare.fits") as hdus:
        hdus.verify("exception")
        header = hdus[0].header
        assert header["FILTER"] == "Ha" and header["DATE"] == "2026-01-01" and header["HEX"] == "0x1F"
        assert header["NOTNUM"] == "NaN" and header["REAL"] == 1500.0 and header["INT"] == -12
        assert header["LOGIC"] is True and header["PAIR"] == complex(1.0, 2.5)


def test_keyword_names_and_values():
    padded = Keywords([("OBJECT ", "M 31"), (" GAIN", 5)])
    assert padded["OBJECT"] == "M 31" and "OBJECT " in padded and padded.names() == ["OBJECT", "GAIN"]
    padded["OBJECT"] = "M 33"
    assert len(padded) == 2 and padded[0] == ("OBJECT", "M 33", "")
    for text in ("OBJECT", b"OBJECT"):
        with pytest.raises(TypeError, match="not a string"):
            Keywords(text)
    with pytest.raises(TypeError):
        xisfconv.write("never.xisf", sample("uint8", (2, 2)), keywords="OBJECT")
    from xisfconv._core import _parse_value

    assert _parse_value("9" * 5000, None) == "9" * 5000                   # more digits than Python converts
    assert _parse_value("+12", None) == 12 and _parse_value("1.5D+03", None) == 1500.0
    assert _parse_value("(1.0, -2.5E0)", None) == complex(1.0, -2.5) and _parse_value("garbage", None) == "garbage"
    # only the digits 0 to 9 make a number
    for text in ("\u0661\u0662\u0663", "\uff11\uff12", "1.\u0665", "(\u0661, 2)"):
        assert _parse_value(text, None) == text
    # a COMMENT or HISTORY card has one text, wherever it is given
    cards = Keywords([("COMMENT", "text", "more"), ("HISTORY", None, "only"), ("COMMENT", "just"), ("", "", "blank")])
    assert list(cards) == [("COMMENT", "text more", ""), ("HISTORY", "only", ""), ("COMMENT", "just", ""), ("", "blank", "")]


def test_messages_name_the_file_and_blame_the_caller(tmp_path):
    data = sample("uint16", (4, 4))
    xisfconv.write(tmp_path / "two.xisf", [data, data])
    with pytest.raises(IndexError, match=r"two\.xisf: image index 9 out of range \(the file has 2\)"):
        xisfconv.read(tmp_path / "two.xisf", 9)
    with pytest.raises(IndexError, match=r"two\.xisf"):
        xisfconv.open(tmp_path / "two.xisf")[-3]
    # a file that makes the library warn when it is opened: the warning is blamed on the line
    # that called the package, also through xisfconv.astropy and its `with` statements
    raw = (tmp_path / "two.xisf").read_bytes().replace(b'<xisf version="1.0"', b'<xisf version="1.7"', 1)
    (tmp_path / "newer.xisf").write_bytes(raw)
    xa = pytest.importorskip("xisfconv.astropy")
    for call in (lambda: xisfconv.open(tmp_path / "newer.xisf"), lambda: xisfconv.read(tmp_path / "newer.xisf"),
                 lambda: xa.read_hdulist(tmp_path / "newer.xisf"), lambda: xa.read_ccddata(tmp_path / "newer.xisf", unit="adu")):
        with pytest.warns(xisfconv.XisfconvWarning) as caught:
            call()
        assert caught[0].filename == __file__, caught[0].filename


def test_a_file_that_could_not_be_opened(tmp_path):
    """What is left of the attempt can be used and dropped."""
    for bad, kind in (("", ValueError), (b"", ValueError), ("a\0b", ValueError), (tmp_path / "none.xisf", FileNotFoundError),
                      (tmp_path, OSError), (3.5, TypeError)):
        with pytest.raises(kind):
            xisfconv.open(bad)
        file = xisfconv.File.__new__(xisfconv.File)
        with pytest.raises(kind):
            file.__init__(bad)
        assert file.closed and len(file) == 0 and list(file) == []
        file.close()
        with pytest.raises(IndexError):
            file[0]
        with pytest.raises(ValueError, match="closed"):
            file.__enter__()
        assert "closed" in repr(file)
        del file
    gc.collect()


# --- properties -------------------------------------------------------------------------------

def test_a_property_that_is_not_read(tmp_path):
    """A vector of a type the library has no reader for (elements nobody knows): its value is
    None, the image is read all the same, and writing it leaves the property out and says so."""
    header = fits.Header()
    header["CTYPE1"], header["CTYPE2"], header["CRVAL1"], header["CRVAL2"] = "RA---TAN", "DEC--TAN", 10.0, 20.0
    header["CRPIX1"], header["CRPIX2"], header["CD1_1"], header["CD1_2"] = 3.0, 3.0, -1e-4, 0.0
    header["CD2_1"], header["CD2_2"] = 0.0, 1e-4
    xisfconv.write(tmp_path / "solved.xisf", sample("float32", (6, 6)), keywords=header, wcs_row_order="bottom-up")
    name = b"PCL:AstrometricSolution:ReferenceCelestialCoordinates"
    raw = (tmp_path / "solved.xisf").read_bytes()
    at = raw.index(b'type="F64Vector"', raw.index(name))
    (tmp_path / "odd.xisf").write_bytes(raw[:at] + b'type="Q64Vector"' + raw[at + 16:])
    with xisfconv.open(tmp_path / "odd.xisf") as file:
        properties = file[0].properties
        assert properties.type(name.decode()) == "Q64Vector" and properties[name.decode()] is None
        assert properties["PCL:AstrometricSolution:ReferenceImageCoordinates"].shape == (2,)
    image = xisfconv.read_image(tmp_path / "odd.xisf")
    assert image.properties[name.decode()] is None and image.data.shape == (6, 6)
    assert image.properties.type(name.decode()) == "Q64Vector"
    with pytest.warns(xisfconv.XisfconvWarning, match="has no value and is not written"):
        xisfconv.write(tmp_path / "again.xisf", image)
    with xisfconv.open(tmp_path / "again.xisf") as file:
        assert name.decode() not in file[0].properties and len(file[0].properties) == len(image.properties) - 1
    # the same elements as complex numbers are read, since 0.15
    (tmp_path / "complex.xisf").write_bytes(raw[:at] + b'type="C32Vector"' + raw[at + 16:])
    with xisfconv.open(tmp_path / "complex.xisf") as file:
        value = file[0].properties[name.decode()]
        assert value.dtype == np.complex64 and value.shape == (2,)


def test_the_colour_filter_array_is_carried(tmp_path):
    """A file that states its filter array without a BAYERPAT keyword, as PixInsight may: an
    image read from it has the keyword, so that writing it states the array again."""
    data = sample("uint16", (6, 8))
    xisfconv.write(tmp_path / "cfa.xisf", data, keywords={"BAYERPAT": "GRBG"})
    raw = (tmp_path / "cfa.xisf").read_bytes()
    start = raw.index(b"<FITSKeyword")
    end = raw.index(b"/>", start) + 2
    (tmp_path / "element-only.xisf").write_bytes(raw[:start] + b" " * (end - start) + raw[end:])
    with xisfconv.open(tmp_path / "element-only.xisf") as file:
        assert len(file[0].keywords) == 0 and file[0].cfa == ("GRBG", 2, 2)
    image = xisfconv.read_image(tmp_path / "element-only.xisf")
    assert image.keywords["BAYERPAT"] == "GRBG"
    assert xisfconv.read_image(tmp_path / "element-only.xisf", row_order="bottom-up").keywords["BAYERPAT"] == "BGGR"
    xisfconv.write(tmp_path / "again.xisf", image)
    with xisfconv.open(tmp_path / "again.xisf") as file:
        assert file[0].cfa == ("GRBG", 2, 2) and same(file[0].read(), data)


# --- arguments --------------------------------------------------------------------------------

def test_arguments(tmp_path):
    path = tmp_path / "a.xisf"
    data = sample("uint8", (5, 7))
    xisfconv.write(path, [data, data])
    for wrong in (1.9, None, [0], b"0"):
        with pytest.raises(TypeError):
            xisfconv.read(path, wrong)
    assert same(xisfconv.read(path, np.int64(1)), data) and same(xisfconv.read(path, True), data)
    with pytest.raises(KeyError):
        xisfconv.read(path, "no such name")
    with pytest.raises(TypeError):
        xisfconv.convert(path, tmp_path / "x.fits", image=1.5)
    for wrong in (7, "text", np.zeros(4, np.uint8), [1, 2]):
        with pytest.raises(TypeError, match="ICC profile"):
            xisfconv.write(tmp_path / "icc.xisf", data, icc_profile=wrong)
    xisfconv.write(tmp_path / "icc.xisf", data, icc_profile=memoryview(b"profile"))
    xisfconv.write(tmp_path / "noicc.xisf", data, icc_profile=b"")
    assert xisfconv.read_image(tmp_path / "icc.xisf").icc_profile == b"profile"
    assert xisfconv.read_image(tmp_path / "noicc.xisf").icc_profile is None
    assert not (tmp_path / "x.fits").exists()

    # the stretch comes back in the shape it was given
    one = sample("float32", (5, 7))
    params = [xisfconv.StretchParams(0.1, 0.3, 0.9)]
    assert xisfconv.apply_stretch(one, params).shape == (5, 7)
    assert xisfconv.apply_stretch(one[:, :, None], params).shape == (5, 7, 1)
    assert xisfconv.apply_stretch(one[None], params, channels="first").shape == (1, 5, 7)

    with xisfconv.open(path) as file:
        assert file[0].dtype == np.uint8


def test_an_output_that_is_not_a_file(tmp_path):
    """A directory of the output's name is not replaced, whatever overwrite says."""
    data = sample("uint8", (4, 4))
    (tmp_path / "empty.xisf").mkdir()
    (tmp_path / "full.fits").mkdir()
    (tmp_path / "full.fits" / "something").write_text("kept")
    xisfconv.write(tmp_path / "in.xisf", data)
    for overwrite in (False, True):
        with pytest.raises(xisfconv.FileError, match="is a directory"):
            xisfconv.write(tmp_path / "empty.xisf", data, overwrite=overwrite)
        with pytest.raises(xisfconv.FileError, match="is a directory"):
            xisfconv.convert(tmp_path / "in.xisf", tmp_path / "full.fits", overwrite=overwrite)
        with pytest.raises(xisfconv.FileError, match="is a directory"):
            xisfconv.rewrite(tmp_path / "in.xisf", tmp_path / "empty.xisf", codec="zlib", overwrite=overwrite)
    assert (tmp_path / "empty.xisf").is_dir() and (tmp_path / "full.fits" / "something").read_text() == "kept"
    assert sorted(os.listdir(tmp_path)) == ["empty.xisf", "full.fits", "in.xisf"]


def test_inputs_that_cannot_be_read(tmp_path):
    """Whatever the names say about the formats, a missing input is a missing input."""
    for name in ("missing.fits", "missing.asdf", "missing.xisf"):
        for target in ("out.xisf", "out.fits", "out.tif"):
            with pytest.raises(FileNotFoundError):
                xisfconv.convert(tmp_path / name, tmp_path / target)
    with pytest.raises(xisfconv.FileError, match="is a directory"):
        xisfconv.convert(tmp_path, tmp_path / "out.xisf")
    assert os.listdir(tmp_path) == []


# --- messages ---------------------------------------------------------------------------------

def test_messages_name_arguments_not_options(tmp_path):
    """The library's messages are those of the command line tool; here they name the arguments
    of the package."""
    data = sample("float32", (4, 4))
    xisfconv.write(tmp_path / "a.xisf", data)
    with pytest.raises(FileExistsError) as caught:
        xisfconv.write(tmp_path / "a.xisf", data)
    assert "overwrite=True" in str(caught.value) and "--" not in str(caught.value)
    with pytest.raises(xisfconv.ArgumentError) as caught:
        xisfconv.write(tmp_path / "a.unknown", data)
    assert "format=" in str(caught.value) and "--" not in str(caught.value)
    with pytest.raises(xisfconv.NotFoundError) as caught:
        xisfconv.convert(tmp_path / "a.xisf", tmp_path / "a.png", stretch="stored")
    assert 'stretch="linked"' in str(caught.value) and "--" not in str(caught.value)
    with pytest.raises(xisfconv.ArgumentError) as caught:
        xisfconv.rewrite(tmp_path / "a.xisf", tmp_path / "a.xisf", codec="zlib")
    assert "rewrite_in_place()" in str(caught.value) and " -o" not in str(caught.value)

    # a file name is not reworded, whatever it looks like
    xisfconv.write(tmp_path / "a.fits", data)
    for name in ["a--force.xisf", "--force.xisf", "x --bits u8 y.xisf"]:
        xisfconv.write(tmp_path / name, data)
        with pytest.raises(FileExistsError) as caught:
            xisfconv.write(tmp_path / name, data)
        assert str(tmp_path / name) in str(caught.value) and "overwrite=True" in str(caught.value)
        with pytest.raises(FileExistsError) as caught:
            xisfconv.convert(tmp_path / "a.fits", tmp_path / name)
        assert str(tmp_path / name) in str(caught.value) and "overwrite=True" in str(caught.value)
        with pytest.warns(xisfconv.XisfconvWarning) as caught:
            xisfconv.write(tmp_path / name, data, checksum="sha3-256", overwrite=True)
        assert all(str(tmp_path / name) in str(w.message) for w in caught)

    # a name that is itself an option, or part of one: where it comes first it is the name
    cwd = os.getcwd()
    os.chdir(tmp_path)
    try:
        for name in ("--force", "-", "-f", "--", "in-place", "--bits"):
            xisfconv.write(name, data, format="xisf")
            with pytest.raises(FileExistsError) as caught:
                xisfconv.write(name, data, format="xisf")
            assert str(caught.value).startswith(name + " ") and "overwrite=True" in str(caught.value), str(caught.value)
            assert "--force to" not in str(caught.value)
            with pytest.warns(xisfconv.XisfconvWarning) as caught:
                xisfconv.convert(name, name + ".tif", sample_format="uint64")
            assert 'sample_format="uint16"' in str(caught[0].message) and "--bits u16" not in str(caught[0].message)
        name = b"a\xff --force b.xisf"                       # not UTF-8: found in the message all the same
        try:
            with open(name, "wb"):
                pass
            os.remove(name)
            such_names = sys.platform != "win32"
        except (OSError, ValueError):
            such_names = False                               # (the file system of macOS has no such names)
        if such_names:
            xisfconv.write(name, data)
            with pytest.raises(FileExistsError) as caught:
                xisfconv.write(name, data)
            assert str(caught.value) == "%s already exists (use overwrite=True to overwrite)" % os.fsdecode(name)
    finally:
        os.chdir(cwd)

    # every option that the sources of the library mention has its wording here
    import re

    from xisfconv._core import _OPTION_WORDS, _wording

    sources = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src")
    if not os.path.isdir(sources):
        pytest.skip("the sources of the library are not here")
    known = [option for option, _ in _OPTION_WORDS]
    for name in sorted(os.listdir(sources)):
        if not name.endswith(".cpp") or name == "main.cpp":
            continue
        with open(os.path.join(sources, name), encoding="utf-8") as source:
            for line in source:
                if line.lstrip().startswith("//"):
                    continue
                for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
                    # --option, and -o: one letter and nothing behind it
                    for option in re.findall(r"(?:^|[ (])(--[a-z][a-z-]*|-[a-zA-Z](?![\w%-]))", literal):
                        assert any(option in entry for entry in known), "%s: %s has no wording" % (name, option)
                        assert not re.search(r"(?:^|[ (])(--[a-z]|-[a-zA-Z](?![\w%-]))", _wording(literal)), literal


def test_commentary_cards_keep_their_text(tmp_path):
    pytest.importorskip("xisf")
    from util import xisf_write

    xisf_write(tmp_path / "c.xisf", sample("uint8", (4, 4)), keywords={
        "HISTORY": ("0", ""), "COMMENT": ("F", "and a comment")})
    image = xisfconv.read_image(tmp_path / "c.xisf")
    assert image.keywords["HISTORY"] == "0" and image.keywords["COMMENT"] == "F and a comment"
    xisfconv.write(tmp_path / "c.fits", image)
    with fits.open(tmp_path / "c.fits") as hdus:
        assert list(hdus[0].header["HISTORY"]) == ["0"] and list(hdus[0].header["COMMENT"]) == ["F and a comment"]


def test_complex_values_and_exponents(tmp_path):
    values = {"C1": complex(1.5, -2.5), "C2": complex(1e20, 2), "C3": complex(0, 1e-7), "R1": 1e20, "R2": 1e-7}
    xisfconv.write(tmp_path / "c.fits", sample("uint8", (4, 4)), keywords=values)
    xisfconv.write(tmp_path / "c.xisf", sample("uint8", (4, 4)), keywords=values)
    xisfconv.convert(tmp_path / "c.xisf", tmp_path / "converted.fits")
    for name in ("c.fits", "converted.fits"):
        with fits.open(tmp_path / name) as hdus:
            hdus.verify("exception")
            assert {key: hdus[0].header[key] for key in values} == values, name
    assert xisfconv.read_image(tmp_path / "c.fits").keywords.to_dict()["C2"] == complex(1e20, 2)


@pytest.mark.skipif(not hasattr(os, "symlink") or sys.platform == "win32", reason="needs symbolic links")
def test_the_temporary_name_is_not_written_through(tmp_path):
    """The output is written under the name with .part added. A link, a directory or a pipe of
    that name is left alone, whatever overwrite says."""
    data = sample("uint8", (4, 4))
    (tmp_path / "victim.txt").write_text("precious")
    os.symlink(tmp_path / "victim.txt", tmp_path / "linked.fits.part")
    (tmp_path / "folder.fits.part").mkdir()
    for overwrite in (False, True):
        for name in ("linked.fits", "folder.fits"):
            with pytest.raises(xisfconv.FileError, match="not a regular file"):
                xisfconv.write(tmp_path / name, data, overwrite=overwrite)
    assert (tmp_path / "victim.txt").read_text() == "precious" and (tmp_path / "folder.fits.part").is_dir()
    assert not (tmp_path / "linked.fits").exists()
    # a leftover file of that name is a matter of overwrite
    (tmp_path / "left.fits.part").write_bytes(b"from an interrupted run")
    with pytest.raises(FileExistsError):
        xisfconv.write(tmp_path / "left.fits", data)
    xisfconv.write(tmp_path / "left.fits", data, overwrite=True)
    assert not (tmp_path / "left.fits.part").exists()
