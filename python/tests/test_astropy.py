# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""xisfconv.astropy: CCDData through astropy's registry, and HDU lists."""

import bz2
import gzip
import os
import io
import lzma
import tempfile
import warnings

import numpy as np
import pytest

import xisfconv
from util import planes_last, sample, same, xisf_read, xisf_write

pytest.importorskip("astropy")
import astropy.units as u                                             # noqa: E402
from astropy.io import fits                                           # noqa: E402
from astropy.nddata import CCDData, StdDevUncertainty, VarianceUncertainty   # noqa: E402
from astropy.wcs import WCS                                           # noqa: E402

import xisfconv.astropy as xa                                         # noqa: E402

HEIGHT, WIDTH = 40, 60


def sky_header():
    header = fits.Header()
    header["CTYPE1"], header["CTYPE2"] = "RA---TAN", "DEC--TAN"
    header["CRVAL1"], header["CRVAL2"] = 83.82, -5.39
    header["CRPIX1"], header["CRPIX2"] = 27.5, 13.25
    header["CD1_1"], header["CD1_2"] = -2.7e-4, 4.1e-5
    header["CD2_1"], header["CD2_2"] = 3.6e-5, 2.9e-4
    return header


STORAGE = ("SIMPLE", "BITPIX", "NAXIS", "NAXIS1", "NAXIS2", "NAXIS3", "EXTEND", "BSCALE", "BZERO")


def cards_of(header):
    """The cards of a header in order, without those on how the data is stored (astropy and
    the converter word them differently) and without the converter's signature."""
    return [tuple(card) for card in header.cards if card.keyword not in STORAGE + ("PROGRAM", "HISTORY")]


def quiet_wcs(header):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return WCS(header)


def same_sky(a, b):
    x = np.array([0.0, 59.0, 0.0, 59.0, 30.2])
    y = np.array([0.0, 0.0, 39.0, 39.0, 17.6])
    return np.allclose(a.all_pix2world(x, y, 0), b.all_pix2world(x, y, 0), rtol=0, atol=1e-9)


@pytest.fixture
def solved(tmp_path):
    """An XISF file of the xisf package: top-down rows, FITS keywords with a WCS (which in XISF
    counts rows bottom-up) and a unit."""
    pytest.importorskip("xisf")
    data = sample("uint16", (HEIGHT, WIDTH))
    cards = {name: (repr(value) if isinstance(value, float) else "'%s'" % value, "") for name, value in
             sky_header().items()}
    cards["BUNIT"] = ("'adu     '", "")
    cards["OBJECT"] = ("'M 42    '", "target")
    cards["EXPTIME"] = ("120.", "")
    path = tmp_path / "solved.xisf"
    xisf_write(path, data, keywords=cards)
    return path, data


def test_ccddata_read(solved):
    path, data = solved
    ccd = CCDData.read(path)
    assert isinstance(ccd, CCDData) and ccd.unit == u.adu
    assert same(ccd.data, data[::-1])                                 # FITS convention: row 0 is the bottom
    assert ccd.meta["OBJECT"] == "M 42" and ccd.meta["EXPTIME"] == 120.0
    assert same_sky(ccd.wcs, quiet_wcs(sky_header()))
    # the same with the format named, with a string, and with the unit given
    assert same(CCDData.read(str(path), format="xisf").data, ccd.data)
    assert CCDData.read(path, unit="electron").unit == u.electron
    assert same(xa.read_ccddata(path).data, ccd.data)
    converted = CCDData.read(path, sample_format="float32")
    assert converted.data.dtype.kind == "f" and converted.data.dtype.itemsize == 4 and converted.data.max() == 1.0


def test_ccddata_equals_the_converted_fits(tmp_path, solved):
    """CCDData.read of the XISF file is CCDData.read of the FITS file the converter writes."""
    path, data = solved
    xisfconv.convert(path, tmp_path / "converted.fits")
    mine, theirs = CCDData.read(path), CCDData.read(tmp_path / "converted.fits")
    assert same(mine.data, theirs.data) and mine.unit == theirs.unit
    assert mine.wcs.to_header(relax=True) == theirs.wcs.to_header(relax=True)
    # all but the cards on how the data is stored, which astropy writes its own way, and the
    # converter's signature on the file it writes
    ours, converted = cards_of(mine.meta), cards_of(theirs.meta)
    assert ours == converted and len(ours) >= 4


def test_read_hdulist_equals_the_converted_fits(tmp_path, solved):
    path, data = solved
    xisfconv.convert(path, tmp_path / "converted.fits")
    mine = xa.read_hdulist(path)
    with fits.open(tmp_path / "converted.fits") as theirs:
        assert len(mine) == len(theirs) == 1 and isinstance(mine[0], fits.PrimaryHDU)
        assert same(mine[0].data, theirs[0].data)
        assert cards_of(mine[0].header) == cards_of(theirs[0].header) and len(cards_of(mine[0].header)) >= 14
    mine.verify("exception")
    mine.writeto(tmp_path / "written.fits")                           # and astropy writes it
    with fits.open(tmp_path / "written.fits") as again:
        assert same(again[0].data, data[::-1])


def test_ccddata_write_and_read_back(tmp_path):
    pytest.importorskip("xisf")
    data = sample("float32", (HEIGHT, WIDTH)) * 1000
    mask = data > 900
    ccd = CCDData(data, unit="electron", wcs=quiet_wcs(sky_header()), mask=mask,
                  uncertainty=StdDevUncertainty(np.sqrt(data)), meta={"OBJECT": "M 51", "EXPTIME": 60.0})
    path = tmp_path / "ccd.xisf"
    ccd.write(path, codec="zlib", checksum="sha256")

    # the file, seen by the xisf package: three images, the first top-down
    from xisf import XISF

    metadata = XISF(str(path)).get_images_metadata()
    assert [m["id"] for m in metadata] == [metadata[0]["id"], "MASK", "UNCERT"]
    assert same(xisf_read(path)[0][:, :, 0], data[::-1])
    assert metadata[0]["FITSKeywords"]["BUNIT"][0]["value"] == "electron"
    assert xisfconv.verify(path).verified == 3

    back = CCDData.read(path)
    assert same(back.data, data) and back.unit == u.electron
    assert back.mask.dtype == bool and np.array_equal(back.mask, mask)
    assert isinstance(back.uncertainty, StdDevUncertainty) and same(back.uncertainty.array, np.sqrt(data))
    assert back.meta["OBJECT"] == "M 51" and back.meta["EXPTIME"] == 60.0
    assert same_sky(back.wcs, ccd.wcs)
    # the solution is also there for PixInsight, and for the plain reader
    with xisfconv.open(path) as file:
        assert file[0].has_astrometric_solution and "PCL:AstrometricSolution:ProjectionSystem" in file[0].properties
        assert same(file[0].read(), data[::-1])

    # the same CCDData through FITS gives the same again
    ccd.write(tmp_path / "ccd.fits")
    through_fits = CCDData.read(tmp_path / "ccd.fits")
    assert same(through_fits.data, back.data) and np.array_equal(through_fits.mask, back.mask)
    assert same(through_fits.uncertainty.array, back.uncertainty.array)
    assert through_fits.wcs.to_header(relax=True) == back.wcs.to_header(relax=True)

    variance = CCDData(data, unit="adu", uncertainty=VarianceUncertainty(data))
    variance.write(tmp_path / "variance.xisf")
    assert isinstance(CCDData.read(tmp_path / "variance.xisf").uncertainty, VarianceUncertainty)

    with pytest.raises(FileExistsError):
        ccd.write(path)
    ccd.write(path, overwrite=True)
    with pytest.raises(TypeError):
        ccd.write(io.BytesIO(), format="xisf")


def test_colour_and_integers(tmp_path):
    pytest.importorskip("xisf")
    colour = sample("uint16", (3, HEIGHT, WIDTH))                    # FITS convention: planes first
    CCDData(colour, unit="adu").write(tmp_path / "rgb.xisf")
    read, metadata = xisf_read(tmp_path / "rgb.xisf")
    assert metadata["colorSpace"] == "RGB" and same(read, np.moveaxis(colour, 0, -1)[::-1])
    assert same(CCDData.read(tmp_path / "rgb.xisf").data, colour)

    signed = np.arange(HEIGHT * WIDTH, dtype=np.int32).reshape(HEIGHT, WIDTH)
    CCDData(signed, unit="adu").write(tmp_path / "counts.xisf")       # no negative value: unsigned
    assert same(CCDData.read(tmp_path / "counts.xisf").data, signed.astype(np.uint32))
    CCDData(signed - 100, unit="adu").write(tmp_path / "negative.xisf")
    assert same(CCDData.read(tmp_path / "negative.xisf").data, (signed - 100).astype(np.float64))
    CCDData(signed.astype(np.int16) % 100 - 50, unit="adu").write(tmp_path / "short.xisf")
    short = CCDData.read(tmp_path / "short.xisf").data
    assert short.dtype.kind == "f" and short.dtype.itemsize == 4 and short.min() == -50


def test_streams_and_compressed_files(tmp_path, solved):
    path, data = solved
    raw = path.read_bytes()
    assert same(CCDData.read(io.BytesIO(raw), format="xisf").data, data[::-1])
    with open(path, "rb") as stream:
        assert same(CCDData.read(stream).data, data[::-1])            # recognized by its first bytes
    with gzip.open(tmp_path / "packed.xisf.gz", "wb") as packed:
        packed.write(raw)
    assert same(CCDData.read(tmp_path / "packed.xisf.gz").data, data[::-1])   # astropy unpacks, xisfconv reads
    assert same(xa.read_hdulist(io.BytesIO(raw))[0].data, data[::-1])
    with pytest.raises(TypeError):
        xa.read_hdulist(12345)


def test_hdulist(tmp_path):
    pytest.importorskip("xisf")
    first = sample("uint16", (HEIGHT, WIDTH))
    second = sample("float32", (3, 20, 30), seed=2)
    top_down = sample("uint8", (10, 12), seed=3)
    table = fits.BinTableHDU.from_columns([fits.Column(name="x", format="E", array=np.arange(4, dtype=np.float32))])
    hdus = fits.HDUList([fits.PrimaryHDU(first, header=sky_header()), fits.ImageHDU(second, name="COLOUR"), table,
                         fits.ImageHDU(top_down, name="TOPDOWN")])
    hdus[0].header["OBJECT"] = "M 8"
    hdus["TOPDOWN"].header["ROWORDER"] = "TOP-DOWN"
    path = tmp_path / "list.xisf"
    with pytest.warns(xisfconv.XisfconvWarning, match="not an image"):
        xa.write_hdulist(hdus, path)
    from xisf import XISF

    metadata = XISF(str(path)).get_images_metadata()
    assert [m["id"] for m in metadata][1:] == ["COLOUR", "TOPDOWN"]
    assert same(xisf_read(path, 0)[0], planes_last(first[::-1]))
    assert same(xisf_read(path, 1)[0], np.moveaxis(second, 0, -1)[::-1])
    assert same(xisf_read(path, 2)[0], planes_last(top_down))        # its rows were top-down already
    assert set(metadata[0]["FITSKeywords"]) >= {"OBJECT", "CTYPE1", "CRPIX2"}
    assert not set(metadata[0]["FITSKeywords"]) & {"SIMPLE", "BITPIX", "NAXIS", "NAXIS1", "BZERO", "BSCALE", "EXTEND"}
    assert "EXTNAME" not in metadata[1]["FITSKeywords"] and "ROWORDER" not in metadata[2]["FITSKeywords"]

    back = xa.read_hdulist(path)
    assert [hdu.name for hdu in back][1:] == ["COLOUR", "TOPDOWN"] and isinstance(back[1], fits.ImageHDU)
    assert same(back[0].data, first) and same(back["COLOUR"].data, second) and same(back["TOPDOWN"].data, top_down[::-1])
    assert back[0].header["OBJECT"] == "M 8" and same_sky(quiet_wcs(back[0].header), quiet_wcs(sky_header()))
    assert all(hdu.header["ROWORDER"] == "BOTTOM-UP" for hdu in back)
    one = xa.read_hdulist(path, "COLOUR")
    assert len(one) == 1 and isinstance(one[0], fits.PrimaryHDU) and same(one[0].data, second)
    assert same(xa.read_hdulist(path, 2, sample_format="uint16")[0].data, top_down[::-1].astype(np.uint16) * 257)

    xa.write_hdulist(hdus[0], tmp_path / "single.fits")               # one HDU, and another format
    with fits.open(tmp_path / "single.fits") as written:
        assert same(written[0].data, first) and written[0].header["OBJECT"] == "M 8"
    with pytest.raises(ValueError):
        xa.write_hdulist(fits.HDUList([fits.PrimaryHDU()]), tmp_path / "empty.xisf")
    with pytest.warns(xisfconv.XisfconvWarning), pytest.raises(ValueError):
        xa.write_hdulist(fits.HDUList([fits.PrimaryHDU(), table]), tmp_path / "empty.xisf")


def test_header_function():
    header = xa.header({"OBJECT": ("M 1", "target"), "EXPTIME": 3.5, "A Long Name": True})
    assert isinstance(header, fits.Header) and header["OBJECT"] == "M 1" and header.comments["OBJECT"] == "target"
    assert header["EXPTIME"] == 3.5 and header["A Long Name"] is True
    assert len(xa.header([])) == 0


def test_pixinsight_solution_becomes_the_wcs(tmp_path):
    """A file with PixInsight's solution properties and no WCS keywords, as PixInsight writes."""
    data = sample("float32", (HEIGHT, WIDTH))
    xisfconv.write(tmp_path / "both.xisf", data, keywords=sky_header(), wcs_row_order="bottom-up")
    # the keywords taken out again: the solution properties stay
    raw = (tmp_path / "both.xisf").read_bytes()
    start = raw.index(b"<FITSKeyword")
    end = raw.rindex(b"<FITSKeyword")
    end = raw.index(b"/>", end) + 2
    header_length = int.from_bytes(raw[8:12], "little")
    stripped = raw[:start] + b" " * (end - start) + raw[end:]       # blanks, so that nothing moves
    assert header_length > end
    (tmp_path / "solution.xisf").write_bytes(stripped)
    with xisfconv.open(tmp_path / "solution.xisf") as file:
        assert len(file[0].keywords) == 0 and file[0].has_astrometric_solution
        assert file[0].wcs_keywords().fit_summary.startswith("WCS")
    ccd = CCDData.read(tmp_path / "solution.xisf", unit="adu")
    assert ccd.wcs is not None and same_sky(ccd.wcs, quiet_wcs(sky_header()))
    assert CCDData.read(tmp_path / "solution.xisf", unit="adu", wcs=False).wcs is None


# --- what a review found -----------------------------------------------------------------------

def test_data_that_astropy_was_told_not_to_scale(tmp_path):
    """An HDU opened with do_not_scale_image_data holds the stored integers and the scaling in
    its header: the pixels that are written are the scaled ones."""
    pytest.importorskip("xisf")
    unsigned = np.array([[0, 1000, 65535], [1, 2, 3]], np.uint16)
    fits.PrimaryHDU(unsigned).writeto(tmp_path / "camera.fits")
    scaled = fits.PrimaryHDU(np.array([[0, 10, 20]], np.int16))
    scaled.header["BSCALE"], scaled.header["BZERO"] = 0.5, 10.0
    scaled.writeto(tmp_path / "scaled.fits")
    for options in ({}, {"do_not_scale_image_data": True}):
        with fits.open(tmp_path / "camera.fits", **options) as hdus:
            xa.write_hdulist(hdus, tmp_path / "camera.xisf", overwrite=True)
        assert same(xisf_read(tmp_path / "camera.xisf")[0][:, :, 0], unsigned[::-1])
        with fits.open(tmp_path / "scaled.fits", **options) as hdus:
            xa.write_hdulist(hdus, tmp_path / "scaled.xisf", overwrite=True)
        assert np.array_equal(xisfconv.read(tmp_path / "scaled.xisf"), [[10.0, 15.0, 20.0]])
    with fits.open(tmp_path / "scaled.fits") as hdus:                    # astropy's blank cards do not get in the way
        hdus[0].data
        xisfconv.write(tmp_path / "direct.xisf", hdus[0].data, keywords=hdus[0].header, row_order="bottom-up")


@pytest.mark.parametrize("name", ["frame.xisf", "mask.xisf", "MASK.xisf", "uncert.xisf", "psfimage.xisf"])
def test_a_file_named_like_its_mask(tmp_path, name):
    """An image without a name is named after its file; that must not make it its own mask."""
    data = np.arange(20, dtype=np.float32).reshape(4, 5)
    ccd = CCDData(data, unit="adu", mask=data > 17, uncertainty=StdDevUncertainty(data / 10))
    ccd.write(tmp_path / name)
    back = CCDData.read(tmp_path / name)
    assert same(back.data, data) and back.mask.sum() == 2 and same(back.uncertainty.array, data / 10)
    with xisfconv.open(tmp_path / name) as file:
        names = [entry.name for entry in file]
    assert names[1:] == ["MASK", "UNCERT"] and names[0].upper() not in ("MASK", "UNCERT")


def test_names_are_found_whatever_the_case(tmp_path):
    data = np.arange(20, dtype=np.float32).reshape(4, 5)
    xisfconv.write(tmp_path / "lower.xisf", [
        xisfconv.Image(data, name="light", row_order="bottom-up", keywords={"BUNIT": "adu"}),
        xisfconv.Image((data > 17).astype(np.uint8), name="mask", row_order="bottom-up"),
        xisfconv.Image(data / 10, name="uncert", row_order="bottom-up")])
    back = CCDData.read(tmp_path / "lower.xisf")
    assert back.mask.sum() == 2 and same(back.uncertainty.array, data / 10)
    assert same(CCDData.read(tmp_path / "lower.xisf", hdu="LIGHT").data, data)
    assert same(CCDData.read(tmp_path / "lower.xisf", hdu="mask", unit="adu").data, (data > 17).astype(np.uint8))
    assert same(xa.read_hdulist(tmp_path / "lower.xisf", "UNCERT")[0].data, data / 10)
    with pytest.raises(KeyError):
        CCDData.read(tmp_path / "lower.xisf", hdu="nothing")
    with pytest.raises(TypeError):
        CCDData.read(tmp_path / "lower.xisf", hdu=("light", 1))

    # names that XISF cannot hold as they are
    ccd = CCDData(data, unit="adu", mask=data > 17, uncertainty=StdDevUncertainty(data / 10))
    ccd.write(tmp_path / "odd.xisf", hdu_mask="BAD-PIX", hdu_uncertainty="ERR 1")
    with xisfconv.open(tmp_path / "odd.xisf") as file:
        assert [entry.name for entry in file][1:] == ["BAD_PIX", "ERR_1"]
    back = CCDData.read(tmp_path / "odd.xisf", hdu_mask="BAD-PIX", hdu_uncertainty="ERR 1")
    assert back.mask.sum() == 2 and same(back.uncertainty.array, data / 10)
    plain = CCDData.read(tmp_path / "odd.xisf")                          # not asked for: not there
    assert plain.mask is None and plain.uncertainty is None
    assert CCDData.read(tmp_path / "lower.xisf", hdu_mask=None).mask is None


def test_the_format_that_is_named_is_written(tmp_path):
    data = np.arange(20, dtype=np.float32).reshape(4, 5)
    ccd = CCDData(data, unit="adu")
    for name in ("x.fits", "x.tif", "x.img", "x"):
        ccd.write(tmp_path / name, format="xisf")
        assert (tmp_path / name).read_bytes().startswith(b"XISF0100"), name
        assert same(CCDData.read(tmp_path / name).data, data)            # by content, whatever the name
    xa.write_ccddata(ccd, tmp_path / "other.fits", format=None)          # by the extension, if asked for
    assert (tmp_path / "other.fits").read_bytes().startswith(b"SIMPLE")
    xa.write_ccddata(ccd, tmp_path / "named.dat", format="fits")
    assert (tmp_path / "named.dat").read_bytes().startswith(b"SIMPLE")


def test_streams_that_are_not_the_file_of_their_name(tmp_path, monkeypatch):
    """A stream has a name, but what it gives need not be what a file of that name holds."""
    import bz2
    import lzma
    import zipfile

    import tempfile

    first, second = np.full((4, 5), 100, np.uint16), np.full((4, 5), 200, np.uint16)
    monkeypatch.chdir(tmp_path)
    (tmp_path / "tmp").mkdir()
    monkeypatch.setattr(tempfile, "tempdir", str(tmp_path / "tmp"))      # where the copies of streams go
    xisfconv.write("light.xisf", second)
    with zipfile.ZipFile("frames.zip", "w") as archive:
        archive.write("light.xisf")
    xisfconv.write("light.xisf", first, overwrite=True)                  # another file of the same name
    with zipfile.ZipFile("frames.zip") as archive, archive.open("light.xisf") as member:
        assert CCDData.read(member, format="xisf", unit="adu").data[0, 0] == 200
    # two files in one stream, read from where the second starts
    one, two = (tmp_path / "light.xisf").read_bytes(), None
    xisfconv.write("other.xisf", second)
    two = (tmp_path / "other.xisf").read_bytes()
    (tmp_path / "both.bin").write_bytes(one + two)
    with open("both.bin", "rb") as stream:
        stream.seek(len(one))
        assert CCDData.read(stream, format="xisf", unit="adu").data[0, 0] == 200
    with open("light.xisf", "rb") as stream:                             # the plain case
        assert CCDData.read(stream, unit="adu").data[0, 0] == 100
    # packed files, by name: with the format named, as a function call, and for HDU lists
    for suffix, opener in ((".gz", gzip.open), (".bz2", bz2.open), (".xz", lzma.open)):
        with opener("packed.xisf" + suffix, "wb") as packed:
            packed.write(two)
        assert CCDData.read("packed.xisf" + suffix, format="xisf", unit="adu").data[0, 0] == 200
        assert xa.read_ccddata("packed.xisf" + suffix, unit="adu").data[0, 0] == 200
        assert xa.read_hdulist("packed.xisf" + suffix)[0].data[0, 0] == 200
        assert CCDData.read("packed.xisf" + suffix, unit="adu").data[0, 0] == 200
    assert not [name for name in os.listdir(tmp_path / "tmp") if name.endswith(".xisfconv")]   # the copies are gone


def test_keyword_values_without_quotes(tmp_path):
    """An XISF file with  Ha  for 'Ha' (and other text that is not a FITS value) reads as the
    FITS file made from it does."""
    pytest.importorskip("xisf")
    cards = {"FILTER": ("Ha", ""), "DATE-LOC": ("2026-01-01T20:00:00", ""), "BUNIT": ("'adu'", "")}
    xisf_write(tmp_path / "bare.xisf", sample("uint16", (4, 5)), keywords=cards)
    xisfconv.convert(tmp_path / "bare.xisf", tmp_path / "bare.fits")
    mine, theirs = CCDData.read(tmp_path / "bare.xisf"), CCDData.read(tmp_path / "bare.fits")
    assert mine.meta["FILTER"] == theirs.meta["FILTER"] == "Ha"
    assert mine.meta["DATE-LOC"] == theirs.meta["DATE-LOC"] == "2026-01-01T20:00:00"
    xa.read_hdulist(tmp_path / "bare.xisf").verify("exception")


def test_cards_of_text_only(tmp_path):
    hdu = fits.PrimaryHDU(sample("uint16", (4, 5)))
    hdu.header["BUNIT"] = "adu"
    hdu.header[""] = "a note without a keyword"
    hdu.writeto(tmp_path / "note.fits")
    xisfconv.convert(tmp_path / "note.fits", tmp_path / "note.xisf")
    assert "a note without a keyword" in list(CCDData.read(tmp_path / "note.xisf").meta[""])
    hdus = xa.read_hdulist(tmp_path / "note.xisf")
    hdus.verify("exception")
    xa.write_hdulist(hdus, tmp_path / "again.xisf")
    assert "a note without a keyword" in [card.value for card in xisfconv.read_image(tmp_path / "again.xisf").keywords]


def test_what_differs_from_fits(tmp_path):
    """Stated in the documentation: a cube of one plane is an image, and the header gains
    EXTNAME and ROWORDER."""
    data = np.arange(20, dtype=np.float32).reshape(1, 4, 5)
    CCDData(data, unit="adu").write(tmp_path / "cube.xisf")
    back = CCDData.read(tmp_path / "cube.xisf")
    assert back.shape == (4, 5) and same(back.data, data[0])
    assert back.meta["EXTNAME"] == "cube" and back.meta["ROWORDER"] == "BOTTOM-UP"


def test_scaling_written_as_numpy_numbers(tmp_path):
    """astropy's own hdu.scale() leaves BSCALE and BZERO as NumPy numbers in the header (with
    current versions of NumPy; with old ones they are Python numbers)."""
    data = np.array([[0.0, 0.25, 0.5, 0.75, 1.0, 2.0]], np.float32)
    hdu = fits.PrimaryHDU(data.copy())
    hdu.scale("int16", "minmax")
    assert hdu.data.dtype == np.int16
    xa.write_hdulist(hdu, tmp_path / "scaled.xisf")
    hdu.writeto(tmp_path / "scaled.fits")
    assert np.allclose(xisfconv.read(tmp_path / "scaled.xisf"), fits.getdata(tmp_path / "scaled.fits"), atol=1e-4)
    assert np.allclose(xisfconv.read(tmp_path / "scaled.xisf"), data, atol=1e-4)
    for scale in (np.float32(0.5), np.int64(2), np.float64(0.5), 0.5):
        hdu = fits.PrimaryHDU(np.array([[1, 2, 3]], np.int16))
        hdu.header["BSCALE"] = scale
        xa.write_hdulist(hdu, tmp_path / "s.xisf", overwrite=True)
        assert np.array_equal(xisfconv.read(tmp_path / "s.xisf"), np.array([[1, 2, 3]]) * float(scale))


def test_names_of_images_without_a_name(tmp_path):
    data = np.arange(20, dtype=np.float32).reshape(4, 5)
    hdus = fits.HDUList([fits.PrimaryHDU(data), fits.ImageHDU(data + 1, name="MASK"), fits.ImageHDU(data + 2, name="IMAGE"),
                         fits.ImageHDU(data + 3)])
    for name in ("mask.xisf", "image.xisf", "---.xisf", "frame.xisf"):
        xa.write_hdulist(hdus, tmp_path / name)
        with xisfconv.open(tmp_path / name) as file:
            names = [entry.name for entry in file]
            assert len({n.upper() for n in names}) == 4, names             # no two alike, whatever the case
            assert names[1:3] == ["MASK", "IMAGE"] and same(file["MASK"].read(row_order="bottom-up"), data + 1)


def test_packed_files_that_are_damaged(tmp_path):
    xisfconv.write(tmp_path / "good.xisf", sample("uint16", (40, 50)))
    raw = (tmp_path / "good.xisf").read_bytes()
    packed = gzip.compress(raw)
    (tmp_path / "cut.xisf.gz").write_bytes(packed[:len(packed) // 2])
    (tmp_path / "bad.xisf.gz").write_bytes(packed[:20] + bytes(40) + packed[60:])
    for name in ("cut.xisf.gz", "bad.xisf.gz"):
        with pytest.raises(xisfconv.Error, match=name):
            xa.read_hdulist(tmp_path / name)
    # a warning about a packed file names the file, not the copy it was unpacked into
    newer = raw.replace(b'<xisf version="1.0"', b'<xisf version="1.1"', 1)
    (tmp_path / "newer.xisf.gz").write_bytes(gzip.compress(newer))
    with pytest.warns(xisfconv.XisfconvWarning, match=r"newer\.xisf\.gz: "):
        xa.read_hdulist(tmp_path / "newer.xisf.gz")


def test_packed_files_as_open_files(tmp_path, monkeypatch):
    """A packed file that the caller has opened is unpacked like one that is named, and what
    is said about it names it."""
    (tmp_path / "temporary").mkdir()
    monkeypatch.setattr(tempfile, "tempdir", str(tmp_path / "temporary"))
    data = sample("uint16", (40, 50))
    xisfconv.write(tmp_path / "good.xisf", data)
    raw = (tmp_path / "good.xisf").read_bytes()
    for suffix, pack in ((".gz", gzip.compress), (".bz2", bz2.compress), (".xz", lzma.compress)):
        (tmp_path / ("good.xisf" + suffix)).write_bytes(pack(raw))
        with open(tmp_path / ("good.xisf" + suffix), "rb") as stream:         # the packed bytes, with a name
            assert same(xa.read_hdulist(stream)[0].data, data[::-1])
        assert same(xa.read_hdulist(io.BytesIO(pack(raw)))[0].data, data[::-1])  # and without one
        with open(tmp_path / ("good.xisf" + suffix), "rb") as stream:
            assert same(xa.read_ccddata(stream, unit="adu").data, data[::-1])
    assert same(xa.read_hdulist(io.BytesIO(raw))[0].data, data[::-1])
    assert os.listdir(tmp_path / "temporary") == []                          # the copies are gone

    # a file without an image: the message names what was given
    fits.PrimaryHDU(np.zeros(7, np.float32)).writeto(tmp_path / "spectrum.fits")
    (tmp_path / "spectrum.fits.gz").write_bytes(gzip.compress((tmp_path / "spectrum.fits").read_bytes()))
    for given in (tmp_path / "spectrum.fits", tmp_path / "spectrum.fits.gz"):
        with pytest.raises(xisfconv.FormatError) as caught:
            xa.read_hdulist(given, image=0)
        assert str(given) in str(caught.value) and "temporary" not in str(caught.value)
        with pytest.raises(xisfconv.FormatError) as caught:
            xa.read_ccddata(given, unit="adu")
        assert str(given) in str(caught.value) and "temporary" not in str(caught.value)
    with pytest.raises(xisfconv.FormatError, match="<BytesIO>"):
        xa.read_hdulist(io.BytesIO((tmp_path / "spectrum.fits").read_bytes()), image=0)
    with open(tmp_path / "spectrum.fits.gz", "rb") as stream:
        with pytest.raises(xisfconv.FormatError, match=r"spectrum\.fits\.gz"):
            xa.read_hdulist(stream, image=0)
    assert os.listdir(tmp_path / "temporary") == []
