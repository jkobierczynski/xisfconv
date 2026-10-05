# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Images with what describes them: read_image and write, astrometry, properties."""

import numpy as np
import pytest

import xisfconv
from util import sample, same, xisf_read, xisf_write

fits = pytest.importorskip("astropy.io.fits")
WCS = pytest.importorskip("astropy.wcs").WCS

HEIGHT, WIDTH = 40, 60


def sky_header(sip=False):
    """A FITS header with a rotated, slightly skewed TAN solution for a 60x40 image whose
    rows are bottom-up."""
    header = fits.Header()
    header["CTYPE1"], header["CTYPE2"] = ("RA---TAN-SIP", "DEC--TAN-SIP") if sip else ("RA---TAN", "DEC--TAN")
    header["CRVAL1"], header["CRVAL2"] = 83.82, -5.39
    header["CRPIX1"], header["CRPIX2"] = 27.5, 13.25
    header["CD1_1"], header["CD1_2"] = -2.7e-4, 4.1e-5
    header["CD2_1"], header["CD2_2"] = 3.6e-5, 2.9e-4
    if sip:
        header["A_ORDER"], header["B_ORDER"] = 2, 2
        header["A_2_0"], header["A_1_1"], header["A_0_2"] = 2.1e-5, -3.2e-6, 4.3e-6
        header["B_2_0"], header["B_1_1"], header["B_0_2"] = -1.4e-6, 3.5e-5, 2.6e-6
    return header


def sky(header, x, y):
    """Sky position of pixels (0-based, y counted from the first row of the array)."""
    import warnings

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        wcs = WCS(header)
        return np.array(wcs.all_pix2world(np.asarray(x, float), np.asarray(y, float), 0))


POINTS_X = np.array([0.0, 59.0, 0.0, 59.0, 30.2, 11.7])
POINTS_Y = np.array([0.0, 0.0, 39.0, 39.0, 17.6, 33.1])


def header_of(keywords):
    return fits.Header.fromstring(xisfconv.Keywords(keywords).fits_text())


@pytest.mark.parametrize("sip", [False, True])
def test_wcs_flip_rows_against_astropy(sip):
    bottom_up = sky_header(sip)
    top_down = xisfconv.wcs_flip_rows(bottom_up, HEIGHT)
    # the same pixel, counted from the other end
    assert np.allclose(sky(header_of(top_down), POINTS_X, HEIGHT - 1 - POINTS_Y), sky(bottom_up, POINTS_X, POINTS_Y),
                       rtol=0, atol=1e-10)
    again = xisfconv.wcs_flip_rows(top_down, HEIGHT)
    for name in bottom_up:
        assert again[name] == pytest.approx(bottom_up[name], rel=1e-12, abs=1e-15) if not isinstance(bottom_up[name], str) \
            else again[name] == bottom_up[name]
    with pytest.raises(xisfconv.NotFoundError):
        xisfconv.wcs_flip_rows({"OBJECT": "no WCS here"}, HEIGHT)
    with pytest.raises(ValueError):
        xisfconv.wcs_flip_rows(bottom_up, 0)


@pytest.mark.parametrize("sip", [False, True])
@pytest.mark.parametrize("extension", ["fits", "asdf", "xisf"])
def test_wcs_through_files(tmp_path, sip, extension):
    """An array given top-down with a WCS that counts rows bottom-up, or with one that counts
    them top-down: either way the file describes the same sky."""
    bottom_up = sky_header(sip)
    expect = sky(bottom_up, POINTS_X, POINTS_Y)
    data = sample("uint16", (HEIGHT, WIDTH))                       # row 0 is the top
    top_down = xisfconv.wcs_flip_rows(bottom_up, HEIGHT)
    for number, options in enumerate([dict(keywords=bottom_up, wcs_row_order="bottom-up"),
                                      dict(keywords=top_down),
                                      dict(keywords=top_down, wcs_row_order="top-down")]):
        path = tmp_path / ("w%d.%s" % (number, extension))
        xisfconv.write(path, data, **options)
        with xisfconv.open(path) as file:
            entry = file[0]
            assert entry.has_astrometric_solution
            assert same(entry.read(), data)
            got = entry.wcs_keywords("bottom-up")
            assert np.allclose(sky(header_of(got), POINTS_X, POINTS_Y), expect, rtol=0, atol=1e-9)
            got = entry.wcs_keywords("top-down")
            assert np.allclose(sky(header_of(got), POINTS_X, HEIGHT - 1 - POINTS_Y), expect, rtol=0, atol=1e-9)
            assert "OBJECT" not in entry.wcs_keywords() and "CTYPE1" in entry.wcs_keywords()
            # the header for FITS, in either row order
            assert np.allclose(sky(header_of(entry.fits_keywords()), POINTS_X, POINTS_Y), expect, rtol=0, atol=1e-9)
            assert np.allclose(sky(header_of(entry.fits_keywords("top-down")), POINTS_X, HEIGHT - 1 - POINTS_Y), expect,
                               rtol=0, atol=1e-9)
        if extension == "fits":
            with fits.open(path) as hdus:                          # astropy reads the file itself
                assert same(hdus[0].data, data[::-1])
                assert np.allclose(sky(hdus[0].header, POINTS_X, POINTS_Y), expect, rtol=0, atol=1e-9)
        else:
            # and after a conversion to FITS
            xisfconv.convert(path, tmp_path / ("c%d.fits" % number))
            with fits.open(tmp_path / ("c%d.fits" % number)) as hdus:
                assert same(hdus[0].data, data[::-1])
                assert np.allclose(sky(hdus[0].header, POINTS_X, POINTS_Y), expect, rtol=0, atol=1e-9)


def test_pixinsight_solution_is_written_and_read(tmp_path):
    """WCS keywords written to XISF also become PixInsight's solution properties, and the
    solution alone gives the WCS back."""
    pytest.importorskip("xisf")
    bottom_up = sky_header()
    expect = sky(bottom_up, POINTS_X, POINTS_Y)
    data = sample("float32", (HEIGHT, WIDTH))
    path = tmp_path / "solved.xisf"
    xisfconv.write(path, data, keywords=bottom_up, wcs_row_order="bottom-up")
    with xisfconv.open(path) as file:
        properties = file[0].properties
        matrix = properties["PCL:AstrometricSolution:LinearTransformationMatrix"]
        assert matrix.shape == (2, 2) and properties.type("PCL:AstrometricSolution:LinearTransformationMatrix") == "F64Matrix"
        reference = properties["PCL:AstrometricSolution:ReferenceCelestialCoordinates"]
        assert reference.shape == (2,) and np.allclose(reference, [83.82, -5.39])
        assert properties["PCL:AstrometricSolution:ProjectionSystem"] == "Gnomonic"
        assert "PCL:AstrometricSolution:ProjectionSystem" in properties and "No:Such" not in properties
        with pytest.raises(KeyError):
            properties["No:Such"]
        assert len(properties) == len(list(properties)) == len(dict(properties))
        # the xisf package reads the same properties
        theirs = xisf_read(path)[1]["XISFProperties"]
        assert set(theirs) == set(properties)
        assert np.allclose(np.asarray(theirs["PCL:AstrometricSolution:LinearTransformationMatrix"]["value"], float), matrix)
        assert np.allclose(np.asarray(theirs["PCL:AstrometricSolution:ReferenceCelestialCoordinates"]["value"], float),
                           reference)

    # A file with the solution and no WCS keywords: what PixInsight writes. It is made here by
    # writing the same pixels without keywords and with the properties of the first file.
    with xisfconv.open(path) as file:
        assert file[0].keywords.get("CTYPE1") is not None
    xisfconv.write(tmp_path / "nowcs.xisf", data, wcs=False)
    with xisfconv.open(tmp_path / "nowcs.xisf") as file:
        assert not file[0].has_astrometric_solution and len(file[0].properties) == 0
        with pytest.raises(xisfconv.NotFoundError):
            file[0].wcs_keywords()
        assert "CTYPE1" not in file[0].fits_keywords()
    xisfconv.write(tmp_path / "keywords-only.xisf", data, keywords=bottom_up, wcs_row_order="bottom-up", wcs=False)
    with xisfconv.open(tmp_path / "keywords-only.xisf") as file:
        assert len(file[0].properties) == 0
        assert np.allclose(sky(header_of(file[0].wcs_keywords()), POINTS_X, POINTS_Y), expect, rtol=0, atol=1e-9)


def test_read_image_and_write_it_back(tmp_path):
    pytest.importorskip("xisf")
    data = sample("float32", (HEIGHT, WIDTH, 3))
    cards = {"OBJECT": ("'M 42    '", "target"), "EXPTIME": ("60.", "seconds"), "BAYERPAT": ("'RGGB    '", "")}
    xisf_write(tmp_path / "in.xisf", data, keywords=cards)

    image = xisfconv.read_image(tmp_path / "in.xisf")
    assert isinstance(image, xisfconv.Image) and same(image.data, data)
    assert image.keywords["OBJECT"] == "M 42" and image.bounds == (0.0, 1.0) and image.color_space == "rgb"
    assert image.row_order == "top-down" and image.channels == "last" and image.wcs_row_order == "bottom-up"
    assert "float32" in repr(image) and image.icc_profile is None and image.properties == {}

    xisfconv.write(tmp_path / "out.xisf", image, codec="zlib")
    back = xisfconv.read_image(tmp_path / "out.xisf")
    assert same(back.data, data) and back.keywords == image.keywords and back.bounds == image.bounds
    theirs, metadata = xisf_read(tmp_path / "out.xisf")
    assert same(theirs, data)

    # the other layouts carry the same picture into the file
    for row_order in ("top-down", "bottom-up"):
        for channels in ("last", "first"):
            image = xisfconv.read_image(tmp_path / "in.xisf", row_order=row_order, channels=channels)
            assert image.row_order == row_order and image.channels == channels
            xisfconv.write(tmp_path / "layout.xisf", image, overwrite=True)
            assert same(xisf_read(tmp_path / "layout.xisf")[0], data)
            assert xisfconv.read_image(tmp_path / "layout.xisf").keywords == back.keywords


def test_fits_to_xisf_and_back_by_hand(tmp_path):
    """FITS read as an image and written as XISF, then the reverse: the FITS data returns."""
    pytest.importorskip("xisf")
    original = sample("uint16", (HEIGHT, WIDTH))
    hdu = fits.PrimaryHDU(original, header=sky_header())
    hdu.header["OBJECT"] = "M 1"
    hdu.writeto(tmp_path / "in.fits")
    expect = sky(sky_header(), POINTS_X, POINTS_Y)

    image = xisfconv.read_image(tmp_path / "in.fits")
    assert same(image.data, original[::-1]) and image.wcs_row_order == "bottom-up" and image.bounds is None
    xisfconv.write(tmp_path / "mid.xisf", image)
    assert same(xisf_read(tmp_path / "mid.xisf")[0][:, :, 0], original[::-1])       # XISF: top-down
    xisfconv.write(tmp_path / "out.fits", xisfconv.read_image(tmp_path / "mid.xisf"))
    with fits.open(tmp_path / "out.fits") as hdus:
        assert same(hdus[0].data, original) and hdus[0].header["OBJECT"] == "M 1"
        assert np.allclose(sky(hdus[0].header, POINTS_X, POINTS_Y), expect, rtol=0, atol=1e-9)


def test_bayer_pattern_follows_the_rows(tmp_path):
    data = sample("uint16", (HEIGHT, WIDTH))                       # an even number of rows
    xisfconv.write(tmp_path / "cfa.fits", data, keywords={"BAYERPAT": "RGGB"})
    with fits.open(tmp_path / "cfa.fits") as hdus:
        assert hdus[0].header["BAYERPAT"] == "GBRG"                # the rows are stored bottom-up
    xisfconv.write(tmp_path / "cfa.xisf", data, keywords={"BAYERPAT": "RGGB"})
    with xisfconv.open(tmp_path / "cfa.xisf") as file:
        assert file[0].cfa == ("RGGB", 2, 2) and file[0].keywords["BAYERPAT"] == "RGGB"
        assert file[0].fits_keywords("bottom-up")["BAYERPAT"] == "GBRG"
        assert file[0].fits_keywords("top-down")["BAYERPAT"] == "RGGB"
    # an image read with its rows in the other order than they are stored brings the pattern along
    assert xisfconv.read_image(tmp_path / "cfa.fits").keywords["BAYERPAT"] == "RGGB"
    assert xisfconv.read_image(tmp_path / "cfa.fits", row_order="bottom-up").keywords["BAYERPAT"] == "GBRG"
    turned = xisfconv.read_image(tmp_path / "cfa.xisf", row_order="bottom-up")
    assert turned.keywords["BAYERPAT"] == "GBRG" and same(turned.data, data[::-1])
    xisfconv.write(tmp_path / "turned.xisf", turned)
    with xisfconv.open(tmp_path / "turned.xisf") as file:
        assert file[0].cfa == ("RGGB", 2, 2) and same(file[0].read(), data)
    odd = sample("uint16", (HEIGHT - 1, WIDTH))                    # an odd number of rows: the pattern stays
    xisfconv.write(tmp_path / "odd.fits", odd, keywords={"BAYERPAT": "RGGB"})
    with fits.open(tmp_path / "odd.fits") as hdus:
        assert hdus[0].header["BAYERPAT"] == "RGGB"


def test_property_keywords(tmp_path):
    """Keywords the tool derives from XISF properties, in the header for FITS."""
    pytest.importorskip("xisf")
    from xisf import XISF

    properties = {
        "Instrument:ExposureTime": {"id": "Instrument:ExposureTime", "type": "Float32", "value": 180.0},
        "Instrument:Telescope:FocalLength": {"id": "Instrument:Telescope:FocalLength", "type": "Float32", "value": 0.53},
        "Observation:Object:Name": {"id": "Observation:Object:Name", "type": "String", "value": "NGC 7000"},
        "Instrument:Camera:Gain": {"id": "Instrument:Camera:Gain", "type": "Int32", "value": 120},
    }
    path = tmp_path / "p.xisf"
    XISF.write(str(path), sample("uint16", (8, 8))[:, :, None], image_metadata={"XISFProperties": properties},
               xisf_metadata={})
    with xisfconv.open(path) as file:
        entry = file[0]
        mine = entry.properties
        assert mine["Instrument:ExposureTime"] == 180.0 and mine.type("Instrument:ExposureTime") == "Float32"
        assert mine["Observation:Object:Name"] == "NGC 7000" and mine["Instrument:Camera:Gain"] == 120
        assert isinstance(mine["Instrument:Camera:Gain"], int) and mine.comment("Instrument:Camera:Gain") == ""
        assert len(entry.keywords) == 0
        derived = entry.fits_keywords()
        assert derived["EXPTIME"] == 180.0 and derived["OBJECT"] == "NGC 7000"
        assert derived["FOCALLEN"] == pytest.approx(530.0)        # metres in XISF, millimetres in FITS
        assert len(entry.fits_keywords(property_keywords=False)) == 0
        image = entry.read_image()
        assert image.properties["Instrument:Camera:Gain"] == 120 and set(image.properties) == set(properties)
        assert entry.read_image(properties=False).properties == {}
    # the properties of the file itself (its Metadata element), as the xisf package reads them
    XISF.write(str(path), sample("uint16", (8, 8))[:, :, None],
               xisf_metadata={"Note:Author": {"id": "Note:Author", "type": "String", "value": "somebody"}})
    theirs = XISF(str(path)).get_file_metadata()
    with xisfconv.open(path) as file:
        assert set(file.properties) == set(theirs) and file.properties["Note:Author"] == "somebody"
        assert len(file[0].properties) == 0
