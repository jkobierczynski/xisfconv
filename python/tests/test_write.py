# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Writing: files written through the package, read by other software."""

import os

import numpy as np
import pytest

import xisfconv
from util import DTYPES, planes_last, sample, same, xisf_keywords, xisf_read

fits = pytest.importorskip("astropy.io.fits")


def shape_for(channels):
    return (11, 19) if channels == 1 else (11, 19, channels)


# --- XISF, read by the xisf package -----------------------------------------------------------

@pytest.mark.parametrize("dtype", ["uint8", "uint16", "uint32", "float32", "float64"])
@pytest.mark.parametrize("channels", [1, 3])
@pytest.mark.parametrize("codec", [None, "zlib", "zstd"])
def test_xisf_read_by_the_xisf_package(tmp_path, dtype, channels, codec):
    pytest.importorskip("xisf")
    if codec == "zstd" and not xisfconv.codec_available("zstd", writing=True):
        pytest.skip("this build of the library has no Zstandard")
    data = sample(dtype, shape_for(channels))
    path = tmp_path / "w.xisf"
    xisfconv.write(path, data, codec=codec, name="Target_1")
    read, metadata = xisf_read(path)
    assert same(read, planes_last(data))
    assert metadata["id"] == "Target_1" and metadata["colorSpace"] == ("Gray" if channels == 1 else "RGB")
    assert "compression" not in metadata or codec        # noise is stored plain where compressing gains nothing
    if codec:
        smooth = (np.indices(data.shape).sum(axis=0) % 7).astype(data.dtype)
        xisfconv.write(tmp_path / "smooth.xisf", smooth, codec=codec)
        read, smooth_metadata = xisf_read(tmp_path / "smooth.xisf")
        assert same(read, planes_last(smooth))
        stored = smooth_metadata["compression"]
        stored = stored.split(":")[0] if isinstance(stored, str) else stored[0]
        assert stored == (codec + "+sh" if data.dtype.itemsize > 1 else codec)
    if data.dtype.kind == "f":
        assert [float(x) for x in metadata["bounds"].split(":")] == [0.0, 1.0]
    # the other layouts of the same image
    xisfconv.write(path, data[::-1], row_order="bottom-up", overwrite=True)
    assert same(xisf_read(path)[0], planes_last(data))
    if channels > 1:
        xisfconv.write(path, np.moveaxis(data, -1, 0), channels="first", overwrite=True)
        assert same(xisf_read(path)[0], data)


def test_xisf_keywords_read_by_the_xisf_package(tmp_path):
    pytest.importorskip("xisf")
    path = tmp_path / "k.xisf"
    xisfconv.write(path, sample("uint16", (6, 8)), keywords=[
        ("OBJECT", "M 31", "the target"), ("EXPTIME", 300.5, "seconds"), ("GAIN", 139), ("COOLED", True),
        ("WARM", False), ("NOTHING", None, "no value"), ("QUOTE", "it's"), ("TINY", 1.5e-07),
        ("HISTORY", "one"), ("HISTORY", "two"), ("COMMENT", "said")])
    cards = xisf_keywords(xisf_read(path)[1])
    assert cards["OBJECT"] == [("M 31", "the target")] and cards["QUOTE"][0][0].replace("''", "'") == "it's"
    assert float(cards["EXPTIME"][0][0]) == 300.5 and cards["EXPTIME"][0][1] == "seconds"
    assert cards["GAIN"][0][0] == "139" and cards["COOLED"][0][0] == "T" and cards["WARM"][0][0] == "F"
    assert float(cards["TINY"][0][0]) == 1.5e-07
    assert [text for _, text in cards["HISTORY"]] == ["one", "two"] and cards["COMMENT"][0][1] == "said"
    assert cards["NOTHING"] == [("", "no value")]


def test_bounds_and_several_images(tmp_path):
    pytest.importorskip("xisf")
    path = tmp_path / "b.xisf"
    counts = (sample("float32", (5, 7)) * 3000).astype(np.float32)
    xisfconv.write(path, [xisfconv.Image(counts, name="auto"), xisfconv.Image(counts, name="stated", bounds=(0, 4096)),
                          sample("uint8", (4, 4, 3))])
    from xisf import XISF

    metadata = XISF(str(path)).get_images_metadata()
    assert len(metadata) == 3
    assert [float(x) for x in metadata[0]["bounds"].split(":")] == [0.0, 65535.0]   # the data fits 0..65535
    assert [float(x) for x in metadata[1]["bounds"].split(":")] == [0.0, 4096.0]
    assert metadata[0]["id"] == "auto" and metadata[1]["id"] == "stated" and metadata[2]["colorSpace"] == "RGB"
    with xisfconv.open(path) as file:
        assert [entry.name for entry in file][:2] == ["auto", "stated"]
        assert file["stated"].bounds == (0.0, 4096.0) and same(file["stated"].read(), counts)


# --- FITS, read by astropy --------------------------------------------------------------------

@pytest.mark.parametrize("dtype", DTYPES)
@pytest.mark.parametrize("channels", [1, 3, 4])
def test_fits_read_by_astropy(tmp_path, dtype, channels):
    data = sample(dtype, shape_for(channels))
    path = tmp_path / "w.fits"
    xisfconv.write(path, data, name="LIGHT", keywords={"OBJECT": "M 31", "EXPTIME": (12.5, "seconds")})
    with fits.open(path) as hdus:
        hdus.verify("exception")
        read, header = hdus[0].data, hdus[0].header
        # FITS is written bottom-up, a colour image as a cube
        expect = data[::-1] if channels == 1 else np.moveaxis(data[::-1], -1, 0)
        assert same(read, expect)
        assert header["ROWORDER"] == "BOTTOM-UP" and header["EXTNAME"] == "LIGHT"
        assert header["OBJECT"] == "M 31" and header["EXPTIME"] == 12.5 and header.comments["EXPTIME"] == "seconds"
    # (into another file: astropy still has the first one mapped into memory through `read`, and
    # Windows does not let a file be replaced while that is so)
    path = tmp_path / "w2.fits"
    xisfconv.write(path, data, stored_row_order="top-down")
    with fits.open(path) as hdus:
        assert same(hdus[0].data, data if channels == 1 else np.moveaxis(data, -1, 0))
        assert hdus[0].header["ROWORDER"] == "TOP-DOWN"


def test_fits_several_hdus(tmp_path):
    path = tmp_path / "m.fits"
    first, second = sample("uint16", (6, 9)), sample("float32", (4, 5), seed=3)
    xisfconv.write(path, [xisfconv.Image(first, name="A", keywords={"N": 1}),
                          xisfconv.Image(second[::-1], name="B", row_order="bottom-up", keywords={"N": 2})])
    with fits.open(path) as hdus:
        assert [hdu.name for hdu in hdus] == ["A", "B"] and [hdu.header["N"] for hdu in hdus] == [1, 2]
        assert same(hdus["A"].data, first[::-1]) and same(hdus["B"].data, second[::-1])


def test_fits_keywords_read_by_astropy(tmp_path):
    path = tmp_path / "k.fits"
    long_text = "a long text " * 12
    keywords = xisfconv.Keywords()
    keywords.append("OBJECT", "NGC 7000", "target")
    keywords.append("EXPTIME", 0.001)
    keywords.append("BIG", 1.2345678901234567e+200)
    keywords.append("COUNT", 2**40)
    keywords.append("NEGATIVE", -17)
    keywords.append("FLAG", True)
    keywords.append("LONGTEXT", long_text.strip(), "continued")
    keywords.append("Focal Length Of The Telescope", 530.0, "a name that needs HIERARCH")
    keywords.append("HISTORY", "calibrated with master dark")
    keywords.append("COMMENT", "x" * 100)                 # more than one card holds
    keywords["ADDED"] = 5                                 # item assignment adds
    keywords["ADDED"] = (6, "replaced")
    xisfconv.write(path, sample("uint8", (4, 5)), keywords=keywords)
    with fits.open(path) as hdus:
        hdus.verify("exception")
        header = hdus[0].header
    assert header["OBJECT"] == "NGC 7000" and header.comments["OBJECT"] == "target"
    assert header["EXPTIME"] == 0.001 and header["BIG"] == 1.2345678901234567e+200
    assert header["COUNT"] == 2**40 and header["NEGATIVE"] == -17 and header["FLAG"] is True
    assert header["LONGTEXT"] == long_text.strip()
    assert header["Focal Length Of The Telescope"] == 530.0
    assert list(header["HISTORY"]) == ["calibrated with master dark"]
    assert "".join(header["COMMENT"]) == "x" * 100
    assert header["ADDED"] == 6 and header.comments["ADDED"] == "replaced"


def test_structural_keywords_are_left_out(tmp_path):
    """A header taken from a FITS file can be passed as it is."""
    pytest.importorskip("xisf")
    original = fits.PrimaryHDU(sample("uint16", (5, 6)))
    original.header["OBJECT"] = "M 42"
    original.writeto(tmp_path / "in.fits")
    with fits.open(tmp_path / "in.fits") as hdus:
        header = hdus[0].header.copy()
        data = hdus[0].data[::-1]
    assert "BZERO" in header and "NAXIS1" in header
    xisfconv.write(tmp_path / "out.xisf", data, keywords=header)
    cards = xisf_keywords(xisf_read(tmp_path / "out.xisf")[1])
    assert set(cards) == {"OBJECT"}
    xisfconv.write(tmp_path / "out.fits", data, keywords=header)
    with fits.open(tmp_path / "out.fits") as hdus:
        hdus.verify("exception")
        assert hdus[0].header["OBJECT"] == "M 42" and hdus[0].header["BZERO"] == 32768
        assert same(hdus[0].data, data[::-1])


# --- ASDF, TIFF, PNG --------------------------------------------------------------------------

@pytest.mark.parametrize("codec", [None, "zlib"])
def test_asdf_read_by_the_asdf_package(tmp_path, codec):
    asdf = pytest.importorskip("asdf")
    pytest.importorskip("asdf_astropy")
    data = sample("float32", (7, 9, 3))
    path = tmp_path / "w.asdf"
    xisfconv.write(path, data, codec=codec, keywords={"OBJECT": "M 13"})
    with asdf.open(path) as tree:
        hdu = tree["fits"][0]
        assert same(np.asarray(hdu.data), np.moveaxis(data[::-1], -1, 0))
        assert hdu.header["OBJECT"] == "M 13"


@pytest.mark.parametrize("dtype", ["uint8", "uint16", "float32"])
@pytest.mark.parametrize("channels", [1, 3])
def test_tiff_read_by_tifffile(tmp_path, dtype, channels):
    tifffile = pytest.importorskip("tifffile")
    data = sample(dtype, shape_for(channels))
    for codec in (None, "zlib"):
        if codec and dtype == "float32":
            pytest.importorskip("imagecodecs")   # tifffile needs it for compressed floating point
        path = tmp_path / ("w-%s.tif" % codec)
        xisfconv.write(path, data, codec=codec)
        assert same(tifffile.imread(path), data)


@pytest.mark.parametrize("dtype", ["uint8", "uint16"])
@pytest.mark.parametrize("channels", [1, 3])
def test_png_read_by_pillow(tmp_path, dtype, channels):
    PIL = pytest.importorskip("PIL.Image")
    data = sample(dtype, shape_for(channels))
    profile = b"not a real profile, but bytes that have to come back the same" * 3
    path = tmp_path / "w.png"
    xisfconv.write(path, data, icc_profile=profile)
    with PIL.open(path) as picture:
        picture.load()
        assert picture.info.get("icc_profile") == profile
        read = np.array(picture)
    if read.dtype != data.dtype:
        # Pillow has no 16-bit RGB: the samples are compared through another decoder
        imagecodecs = pytest.importorskip("imagecodecs")
        read = imagecodecs.png_decode(path.read_bytes())
    assert same(read, data)


def test_icc_profile_in_xisf_and_tiff(tmp_path):
    tifffile = pytest.importorskip("tifffile")
    profile = bytes(range(256)) * 4
    data = sample("uint16", (5, 5, 3))
    xisfconv.write(tmp_path / "p.xisf", data, icc_profile=profile)
    with xisfconv.open(tmp_path / "p.xisf") as file:
        assert file[0].has_icc_profile and file[0].icc_profile == profile
    xisfconv.write(tmp_path / "p.tif", data, icc_profile=bytearray(profile))
    with tifffile.TiffFile(tmp_path / "p.tif") as tiff:
        assert tiff.pages[0].tags["InterColorProfile"].value == profile
    xisfconv.write(tmp_path / "none.xisf", data)
    with xisfconv.open(tmp_path / "none.xisf") as file:
        assert not file[0].has_icc_profile and file[0].icc_profile is None


# --- what an array may look like --------------------------------------------------------------

def test_array_layouts(tmp_path):
    base = sample("uint16", (12, 20, 3))
    path = tmp_path / "l.fits"

    def written(array, **options):
        xisfconv.write(path, array, stored_row_order="top-down", overwrite=True, **options)
        with fits.open(path) as hdus:
            return hdus[0].data

    planar = np.moveaxis(base, -1, 0)
    assert same(written(base), planar)
    assert same(written(np.asfortranarray(base)), planar)                    # Fortran order
    assert same(written(base[::2, ::3]), planar[:, ::2, ::3])                # a view with steps
    assert same(written(base[:, ::-1]), planar[:, :, ::-1])                  # negative strides
    assert same(written(base.astype(">u2")), planar)                         # the other byte order
    assert same(written(base.tolist() and np.array(base.tolist(), dtype=np.uint16)), planar)
    assert same(written(base[:, :, 0]), base[:, :, 0])                       # 2-D
    assert same(written(base[:, :, :1]), base[:, :, 0])                      # one channel, 3-D
    assert same(written(base[:, :, :2]), planar[:2])                         # two planes
    assert same(written(planar, channels="first"), planar)
    read_only = base.copy()
    read_only.flags.writeable = False
    assert same(written(read_only), planar)
    assert same(base, sample("uint16", (12, 20, 3)))                         # the caller's array is untouched


def test_what_cannot_be_written(tmp_path):
    path = tmp_path / "no.xisf"
    good = sample("uint8", (4, 4))
    for dtype in ("int8", "int16", "int32", "int64", "float16", "complex64", "bool", "object", "U3"):
        with pytest.raises(TypeError, match="uint8, uint16"):
            xisfconv.write(path, np.zeros((4, 4), dtype))
    for shape in ((4,), (2, 2, 2, 2), (0, 4), (4, 0, 3), ()):
        with pytest.raises(ValueError):
            xisfconv.write(path, np.zeros(shape, np.uint8))
    with pytest.raises(TypeError, match="masked"):
        xisfconv.write(path, np.ma.masked_array(good, good > 3))
    with pytest.raises(ValueError):
        xisfconv.write(path, [])
    with pytest.raises(ValueError, match="bound"):
        xisfconv.write(path, good.astype(np.float32), bounds=(1, 0))
    with pytest.raises(ValueError):
        xisfconv.write(path, good, row_order="upside-down")
    with pytest.raises(ValueError):
        xisfconv.write(path, good, codec="brotli")
    with pytest.raises(ValueError):
        xisfconv.write(path, good, checksum="md5")
    with pytest.raises(ValueError):
        xisfconv.write(path, good, format="jpeg")
    with pytest.raises(xisfconv.ArgumentError):
        xisfconv.write(tmp_path / "no.extension", good)
    with pytest.raises(xisfconv.UnsupportedError):
        xisfconv.write(path, good, codec="lz4")           # read, not written
    with pytest.raises(TypeError):
        xisfconv.write(path, good, keywords={"OBJECT": [1, 2]})
    with pytest.raises(ValueError):
        xisfconv.write(path, good, keywords={"EXPTIME": float("nan")})
    with pytest.raises(ValueError):
        xisfconv.write(path, good, keywords={"OBJECT": "a\0b"})
    assert os.listdir(tmp_path) == []                     # nothing was left behind


def test_existing_files_are_kept(tmp_path):
    path = tmp_path / "keep.xisf"
    first, second = sample("uint8", (3, 3)), sample("uint8", (3, 3), seed=9)
    xisfconv.write(path, first)
    with pytest.raises(FileExistsError) as caught:
        xisfconv.write(path, second)
    assert isinstance(caught.value, xisfconv.OutputExistsError)
    assert same(xisfconv.read(path), first)
    xisfconv.write(path, second, overwrite=True)
    assert same(xisfconv.read(path), second)
    assert sorted(os.listdir(tmp_path)) == ["keep.xisf"]
    with pytest.raises(xisfconv.FileError):
        xisfconv.write(tmp_path / "no" / "such" / "directory.xisf", first)
    xisfconv.write(tmp_path / "stated.dat", first, format="fits")
    assert xisfconv.detect_format(tmp_path / "stated.dat") == "fits"


def test_checksums(tmp_path):
    data = sample("uint16", (30, 40))
    for algorithm in ("sha1", "sha256", "sha512"):
        path = tmp_path / (algorithm + ".xisf")
        xisfconv.write(path, data, checksum=algorithm, codec="zlib")
        with xisfconv.open(path) as file:
            assert file[0].detail("checksum").startswith(algorithm + ":")
        report = xisfconv.verify(path)
        assert report.ok and report.verified == 1 and report.unchecked == 0
    with pytest.warns(xisfconv.XisfconvWarning, match="PixInsight"):
        xisfconv.write(tmp_path / "sha3.xisf", data, checksum="sha3-256")
    assert xisfconv.verify(tmp_path / "sha3.xisf").ok
