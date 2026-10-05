# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Reading: files written by other software, read through the package."""

import numpy as np
import pytest

import xisfconv
from util import DTYPES, sample, same, xisf_write

fits = pytest.importorskip("astropy.io.fits")


# --- XISF, written by the xisf package -------------------------------------------------------

@pytest.mark.parametrize("dtype", ["uint8", "uint16", "uint32", "float32", "float64"])
@pytest.mark.parametrize("channels", [1, 3])
@pytest.mark.parametrize("codec", [None, "zlib", "lz4", "lz4hc", "zstd"])
def test_xisf_of_the_xisf_package(tmp_path, dtype, channels, codec):
    pytest.importorskip("xisf")
    if codec == "zstd" and not xisfconv.codec_available("zstd"):
        pytest.skip("this build of the library has no Zstandard")
    data = sample(dtype, (40, 64) if channels == 1 else (40, 64, channels))
    path = tmp_path / "a.xisf"
    xisf_write(path, data, codec=codec, shuffle=codec is not None)

    assert same(xisfconv.read(path), data)
    assert same(xisfconv.read(path, channels="first"), data if channels == 1 else np.moveaxis(data, -1, 0))
    assert same(xisfconv.read(path, row_order="bottom-up"), data[::-1])
    assert same(xisfconv.read(path, row_order=None), data)   # as stored: XISF is top-down

    with xisfconv.open(path) as file:
        assert file.format == "xisf" and len(file) == 1 and file.size == path.stat().st_size
        entry = file[0]
        assert entry.shape == data.shape and entry.dtype == data.dtype
        assert (entry.width, entry.height, entry.channels) == (64, 40, channels)
        assert entry.color_space == ("gray" if channels == 1 else "rgb")
        assert entry.row_order == "top-down" and entry.readable
        assert entry.bounds == ((0.0, 1.0) if data.dtype.kind == "f" else None)


def test_xisf_keywords_and_values(tmp_path):
    pytest.importorskip("xisf")
    cards = {
        "OBJECT": ("'M 31    '", "the target"),
        "EXPTIME": ("300.5", "seconds"),
        "GAIN": ("139", ""),
        "FLIPPED": ("T", "a logical"),
        "COOLED": ("F", ""),
        "SMALL": ("1.5e-07", "lower-case exponent, as PixInsight writes"),
        "DOUBLE": ("1.5D+03", "Fortran exponent"),
        "QUOTED": ("'it''s'", "a quote in a string"),
        "EMPTY": ("''", "an empty string"),
    }
    path = tmp_path / "k.xisf"
    xisf_write(path, sample("uint16", (8, 9)), keywords=cards)
    with xisfconv.open(path) as file:
        keywords = file[0].keywords
    assert keywords["OBJECT"] == "M 31" and keywords["object"] == "M 31"
    assert keywords["EXPTIME"] == 300.5 and isinstance(keywords["EXPTIME"], float)
    assert keywords["GAIN"] == 139 and isinstance(keywords["GAIN"], int)
    assert keywords["FLIPPED"] is True and keywords["COOLED"] is False
    assert keywords["SMALL"] == 1.5e-07 and keywords["DOUBLE"] == 1500.0
    assert keywords["QUOTED"] == "it's" and keywords["EMPTY"] == ""
    assert keywords.get("NOTHERE") is None and keywords.get("NOTHERE", 5) == 5 and "NOTHERE" not in keywords
    with pytest.raises(KeyError):
        keywords["NOTHERE"]
    assert [card.comment for card in keywords if card.name == "EXPTIME"] == ["seconds"]
    assert set(keywords.names()) == set(cards) and len(keywords) == len(cards)
    assert keywords.to_dict()["GAIN"] == 139


# --- sample conversion ------------------------------------------------------------------------

def test_sample_format_rescales(tmp_path):
    pytest.importorskip("xisf")
    path = tmp_path / "u16.xisf"
    data = sample("uint16", (16, 20))
    xisf_write(path, data)
    as_float = xisfconv.read(path, sample_format="float32")
    assert as_float.dtype == np.float32 and np.allclose(as_float, data / 65535.0, atol=1e-7)
    assert as_float.min() == 0.0 and as_float.max() == 1.0
    as_bytes = xisfconv.read(path, sample_format=np.uint8)
    assert as_bytes.dtype == np.uint8 and np.abs(as_bytes.astype(float) - data / 257.0).max() <= 0.5 + 1e-9
    assert same(xisfconv.read(path, sample_format="u16"), data) and same(xisfconv.read(path, sample_format=np.dtype("uint16")), data)
    wide = xisfconv.read(path, sample_format="uint32")
    assert wide.dtype == np.uint32 and wide.max() == 2**32 - 1 and wide.min() == 0

    path = tmp_path / "f32.xisf"
    data = sample("float32", (16, 20))
    xisf_write(path, data)
    as_u16 = xisfconv.read(path, sample_format="uint16")
    assert np.abs(as_u16.astype(float) - data.astype(float) * 65535).max() <= 0.5 + 1e-3
    half = xisfconv.read(path, sample_format="uint16", bounds=(0.0, 2.0))    # the stated range mapped to the integers
    assert np.abs(half.astype(float) - data.astype(float) * 65535 / 2).max() <= 0.5 + 1e-3
    assert same(xisfconv.read(path, sample_format="float64"), data.astype(np.float64))   # float to float: the values

    for bad in ("int16", "float16", "complex64", "nonsense", np.int32):
        with pytest.raises(ValueError):
            xisfconv.read(path, sample_format=bad)
    with pytest.raises(ValueError):
        xisfconv.read(path, sample_format="uint16", bounds=(1.0, 1.0))
    with pytest.raises(ValueError):
        xisfconv.read(path, row_order="sideways")
    with pytest.raises(ValueError):
        xisfconv.read(path, channels="middle")


# --- FITS, written by astropy -----------------------------------------------------------------

@pytest.mark.parametrize("dtype", DTYPES)
def test_fits_of_astropy(tmp_path, dtype):
    path = tmp_path / "a.fits"
    data = sample(dtype, (12, 17))
    fits.PrimaryHDU(data).writeto(path)
    # FITS rows are bottom-up: row 0 of the FITS array is the bottom of the image
    assert same(xisfconv.read(path, row_order="bottom-up"), data)
    assert same(xisfconv.read(path), data[::-1])
    assert same(xisfconv.read(path, row_order=None), data)
    with xisfconv.open(path) as file:
        entry = file[0]
        assert file.format == "fits" and entry.row_order == "bottom-up" and not entry.row_order_declared
        assert entry.dtype is None and entry.bounds is None   # known once the data has been read
        entry.read()
        assert entry.dtype == data.dtype


def test_fits_signed_integers(tmp_path):
    rng = np.random.default_rng(3)
    positive = rng.integers(0, 30000, (9, 11)).astype(np.int16)
    negative = rng.integers(-1000, 1000, (9, 11)).astype(np.int16)
    wide = rng.integers(-100000, 100000, (9, 11)).astype(np.int32)
    fits.HDUList([fits.PrimaryHDU(positive), fits.ImageHDU(negative, name="NEG"),
                  fits.ImageHDU(wide, name="WIDE")]).writeto(tmp_path / "s.fits")
    with xisfconv.open(tmp_path / "s.fits") as file:
        assert [entry.name for entry in file] == ["", "NEG", "WIDE"]
        assert same(file[0].read(row_order=None), positive.astype(np.uint16))   # no negative value: unsigned
        assert same(file["NEG"].read(row_order=None), negative.astype(np.float32))
        assert same(file["WIDE"].read(row_order=None), wide.astype(np.float64))
        assert file[1].bitpix == 16 and file[2].bitpix == 32 and file[2].source_index == 2
        with pytest.raises(KeyError):
            file["NOSUCH"]
        with pytest.raises(xisfconv.ImageIndexError):
            file[3]
        with pytest.raises(IndexError):
            file[7]
    assert same(xisfconv.read(tmp_path / "s.fits", "WIDE", row_order=None), wide.astype(np.float64))
    assert same(xisfconv.read(tmp_path / "s.fits", -1, row_order=None), wide.astype(np.float64))
    with pytest.raises(xisfconv.ImageIndexError):
        xisfconv.read(tmp_path / "s.fits", 3)


def test_fits_cube_tables_and_tile_compression(tmp_path):
    cube = sample("uint16", (3, 10, 14))            # FITS: [planes, rows, columns]
    tiled = sample("uint16", (30, 40), seed=5)
    lossless = sample("float32", (30, 40), seed=6)
    table = fits.BinTableHDU.from_columns([fits.Column(name="x", format="E", array=np.arange(4, dtype=np.float32))])
    fits.HDUList([fits.PrimaryHDU(cube), table,
                  fits.CompImageHDU(tiled, name="RICE", compression_type="RICE_1"),
                  fits.CompImageHDU(lossless, name="GZIP", compression_type="GZIP_2", quantize_level=0)
                  ]).writeto(tmp_path / "c.fits")
    with xisfconv.open(tmp_path / "c.fits") as file:
        assert len(file) == 3 and len(file.skipped) == 1 and "table" in file.skipped[0].lower()
        assert file[0].shape == (10, 14, 3)
        assert same(file[0].read(row_order=None, channels="first"), cube)
        assert same(file[0].read(row_order=None), np.moveaxis(cube, 0, -1))
        assert same(file["RICE"].read(row_order=None), tiled)
        assert file["RICE"].detail("tileCompression") == "RICE_1"
        assert same(file["GZIP"].read(row_order=None), lossless)


def test_fits_keywords_of_astropy(tmp_path):
    hdu = fits.PrimaryHDU(sample("uint16", (6, 7)))
    hdu.header["OBJECT"] = ("NGC 7000", "target")
    hdu.header["EXPTIME"] = (120.0, "[s]")
    hdu.header["NCOMBINE"] = 12
    hdu.header["CALSTAT"] = True
    hdu.header["LONGTEXT"] = "x" * 150
    hdu.header["HIERARCH Long Keyword Name"] = 3.25
    hdu.header.add_history("first step")
    hdu.header.add_history("second step")
    hdu.header.add_comment("a remark")
    hdu.writeto(tmp_path / "k.fits")
    with xisfconv.open(tmp_path / "k.fits") as file:
        keywords = file[0].keywords
    assert keywords["OBJECT"] == "NGC 7000" and keywords["EXPTIME"] == 120.0 and keywords["NCOMBINE"] == 12
    assert keywords["CALSTAT"] is True and keywords["LONGTEXT"] == "x" * 150
    assert keywords["Long Keyword Name"] == 3.25
    assert [card.value for card in keywords.cards("HISTORY")] == ["first step", "second step"]
    assert keywords["COMMENT"] == "a remark"
    for structural in ("SIMPLE", "BITPIX", "NAXIS", "NAXIS1", "BZERO", "BSCALE", "EXTEND"):
        assert structural not in keywords
    assert "HISTORY" not in keywords.to_dict() and keywords.to_dict()["NCOMBINE"] == 12


# --- ASDF, written by the asdf package --------------------------------------------------------

def test_asdf_of_the_asdf_package(tmp_path):
    asdf = pytest.importorskip("asdf")
    first = sample("float32", (9, 12))
    second = sample("uint16", (3, 9, 12), seed=2)
    path = tmp_path / "a.asdf"
    asdf.AsdfFile({"science": first, "nested": {"cube": second}, "note": "text"}).write_to(path)
    with xisfconv.open(path) as file:
        assert file.format == "asdf" and len(file) == 2
        by_name = {entry.name: entry for entry in file}
        assert set(by_name) == {"science", "nested.cube"} or set(by_name) == {"science", "nested/cube"}
        science = by_name["science"]
        assert science.plain_array and same(science.read(row_order=None), first)
        cube = next(entry for name, entry in by_name.items() if name != "science")
        assert same(cube.read(row_order=None, channels="first"), second)
        assert "science" in file.header_text


# --- files and errors -------------------------------------------------------------------------

def test_file_objects(tmp_path):
    pytest.importorskip("xisf")
    path = tmp_path / "f.xisf"
    data = sample("uint8", (5, 6))
    xisf_write(path, data)
    file = xisfconv.open(str(path))
    assert not file.closed and "1 image" in repr(file) and file.detail("version") == "1.0"
    assert file.images == tuple(file) and file[-1] == file[0] and file[0:1] == (file[0],)
    assert len({file[0], file[-1]}) == 1 and file[0] != xisfconv.open(str(path))[0]
    assert "<xisf" in file.header_text and file.skipped == []
    entry = file[0]
    file.close()
    file.close()                                     # twice is fine
    assert file.closed and "closed" in repr(file) and "closed" in repr(entry)
    for use in (lambda: entry.read(), lambda: entry.name, lambda: entry.keywords, lambda: file.format,
                lambda: entry.shape, lambda: list(entry.properties), lambda: file.header_text):
        with pytest.raises(ValueError, match="closed"):
            use()
    with pytest.raises(ValueError):
        file.__enter__()
    # a name as bytes, and as a path object
    assert same(xisfconv.read(str(path).encode()), data) and same(xisfconv.read(path), data)


def test_errors(tmp_path):
    missing = tmp_path / "missing.xisf"
    with pytest.raises(FileNotFoundError) as caught:
        xisfconv.open(missing)
    assert isinstance(caught.value, xisfconv.Error) and isinstance(caught.value, OSError)
    assert caught.value.status == 2 and "missing.xisf" in str(caught.value)
    with pytest.raises(xisfconv.FileError):
        xisfconv.detect_format(missing)

    junk = tmp_path / "junk.xisf"
    junk.write_bytes(b"this is not an image file at all, whatever its name says")
    with pytest.raises(xisfconv.FormatError):
        xisfconv.open(junk)
    with pytest.raises(xisfconv.FormatError):
        xisfconv.detect_format(junk)
    with pytest.raises(xisfconv.Error):
        xisfconv.read(junk)

    with pytest.raises(ValueError):
        xisfconv.open("")
    with pytest.raises(ValueError):
        xisfconv.open("a\0b.xisf")
    with pytest.raises(TypeError):
        xisfconv.open(None)
    with pytest.raises(xisfconv.FileError):
        xisfconv.open(tmp_path)                      # a directory


def test_detect_format(tmp_path):
    pytest.importorskip("xisf")
    xisf_write(tmp_path / "a.dat", sample("uint8", (4, 4)))
    fits.PrimaryHDU(sample("uint8", (4, 4))).writeto(tmp_path / "b.dat")
    assert xisfconv.detect_format(tmp_path / "a.dat") == "xisf"
    assert xisfconv.detect_format(tmp_path / "b.dat") == "fits"
    assert xisfconv.open(tmp_path / "b.dat").format == "fits"   # by content, not by name
