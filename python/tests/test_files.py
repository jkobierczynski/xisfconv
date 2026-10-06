# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Whole files: convert, rewrite, verify."""

import importlib.util
import os
import re
import subprocess

import numpy as np
import pytest

import xisfconv
from util import planes_last, sample, same, xisf_read, xisf_write

fits = pytest.importorskip("astropy.io.fits")


@pytest.fixture
def light(tmp_path):
    """An XISF file of the xisf package: float32 RGB with keywords."""
    pytest.importorskip("xisf")
    data = sample("float32", (30, 44, 3))
    path = tmp_path / "light.xisf"
    xisf_write(path, data, keywords={"OBJECT": ("'M 45    '", "target"), "EXPTIME": ("30.", "")}, codec="zlib",
               shuffle=True)
    return path, data


def test_convert_to_fits_asdf_tiff_png(tmp_path, light):
    path, data = light
    xisfconv.convert(path, tmp_path / "out.fits")
    with fits.open(tmp_path / "out.fits") as hdus:
        hdus.verify("exception")
        assert same(hdus[0].data, np.moveaxis(data[::-1], -1, 0))
        assert hdus[0].header["OBJECT"] == "M 45" and hdus[0].header["EXPTIME"] == 30.0
        assert "Converted from XISF" in str(hdus[0].header["HISTORY"])

    xisfconv.convert(path, tmp_path / "top.fits", row_order="top-down", sample_format="uint16")
    with fits.open(tmp_path / "top.fits") as hdus:
        assert hdus[0].data.dtype.kind == "u" and hdus[0].header["ROWORDER"] == "TOP-DOWN"
        assert np.abs(hdus[0].data.astype(float) - np.moveaxis(data, -1, 0).astype(float) * 65535).max() <= 0.5 + 1e-3

    asdf = pytest.importorskip("asdf")
    xisfconv.convert(path, tmp_path / "out.asdf", codec="zlib")
    with asdf.open(tmp_path / "out.asdf") as tree:
        assert same(np.asarray(tree["fits"][0].data), np.moveaxis(data[::-1], -1, 0))

    tifffile = pytest.importorskip("tifffile")
    xisfconv.convert(path, tmp_path / "out.tif")
    assert same(tifffile.imread(tmp_path / "out.tif"), data)
    xisfconv.convert(path, tmp_path / "out16.tif", sample_format=np.uint16, codec=True)
    assert tifffile.imread(tmp_path / "out16.tif").dtype == np.uint16

    PIL = pytest.importorskip("PIL.Image")
    xisfconv.convert(path, tmp_path / "out.png", sample_format="uint8")
    with PIL.open(tmp_path / "out.png") as picture:
        assert np.abs(np.array(picture).astype(float) - data.astype(float) * 255).max() <= 0.5 + 1e-3
    xisfconv.convert(path, tmp_path / "stretched.png", sample_format="uint8", stretch="auto")
    with PIL.open(tmp_path / "stretched.png") as picture:
        assert not np.array_equal(np.array(picture), np.round(data * 255).astype(np.uint8))
    xisfconv.convert(path, tmp_path / "named.dat", format="tiff")
    assert same(tifffile.imread(tmp_path / "named.dat"), data)



def test_convert_to_a_smaller_picture(tmp_path, light):
    """bin, resize and scale: the mean of the pixels a pixel of the picture covers."""
    tifffile = pytest.importorskip("tifffile")
    path, data = light                       # 30 rows, 44 columns, 3 channels

    def mean(rows, columns):                 # that many pixels become one
        return data.astype(np.float64).reshape(30 // rows, rows, 44 // columns, columns, 3).mean(axis=(1, 3)).astype(np.float32)

    def picture(**options):
        xisfconv.convert(path, tmp_path / "small.tif", overwrite=True, **options)
        return tifffile.imread(tmp_path / "small.tif")

    assert same(picture(bin=2), mean(2, 2))
    assert same(picture(bin=np.int64(2)), mean(2, 2))
    assert same(picture(scale=0.5), mean(2, 2))
    assert same(picture(resize=22), mean(2, 2))            # the longest side
    assert same(picture(resize=(1000, 15)), mean(2, 2))    # a box: width, height
    assert same(picture(resize=(11, 1000)), picture(resize=11))
    assert picture(resize=11).shape == (8, 11, 3)          # 30 * 11 / 44 = 7.5, rounded
    assert picture(bin=4).shape == (7, 11, 3)              # two rows are left over
    assert same(picture(bin=4), data[:28].astype(np.float64).reshape(7, 4, 11, 4, 3).mean(axis=(1, 3)).astype(np.float32))
    assert same(picture(resize=500), data) and same(picture(scale=1), data) and same(picture(bin=1), data)
    assert same(picture(resize=[22, 22]), mean(2, 2)) and same(picture(resize=np.array([22, 22])), mean(2, 2))
    assert picture(bin=2, scale=0.5).shape == (8, 11, 3)    # the blocks first, then half of that: 7.5 rows, rounded
    # before a stretch: the picture of the binned image
    xisf_write(tmp_path / "binned.xisf", mean(2, 2))
    xisfconv.convert(path, tmp_path / "a.tif", bin=2, stretch="linked")
    xisfconv.convert(tmp_path / "binned.xisf", tmp_path / "b.tif", stretch="linked")
    assert same(tifffile.imread(tmp_path / "a.tif"), tifffile.imread(tmp_path / "b.tif"))
    PIL = pytest.importorskip("PIL.Image")
    xisfconv.convert(path, tmp_path / "thumb.png", resize=16, stretch="auto", sample_format="uint8")
    with PIL.open(tmp_path / "thumb.png") as thumb:
        assert thumb.size == (16, 11) and thumb.mode == "RGB"
    # pictures only
    for name in ("x.fits", "x.asdf"):
        with pytest.raises(ValueError, match="bin, resize and scale make a smaller picture") as refused:
            xisfconv.convert(path, tmp_path / name, bin=2)
        assert "--" not in str(refused.value) and not (tmp_path / name).exists()
    for options in ({"bin": 0}, {"bin": -2}, {"bin": 1.5}, {"bin": True}, {"bin": None}, {"bin": "2"}, {"resize": 0}, {"resize": -5},
                    {"resize": 2.5}, {"resize": (10,)}, {"resize": (10, 0)}, {"resize": (10, 10, 10)}, {"resize": "10x10"},
                    {"resize": True}, {"resize": iter([10, 10])}, {"resize": (x for x in (-5, 3))}, {"resize": {5: 1, 6: 2}},
                    {"resize": b"\x05\x06"}, {"resize": (2 ** 70, 1)}, {"resize": (10.0, 10)}, {"scale": 0}, {"scale": 1.5}, {"scale": -0.5}, {"scale": float("nan")}, {"scale": "half"},
                    {"scale": True}):
        with pytest.raises(ValueError, match="expects"):
            xisfconv.convert(path, tmp_path / "bad.tif", **options)
        assert not (tmp_path / "bad.tif").exists()


def test_convert_from_fits_and_back(tmp_path):
    pytest.importorskip("xisf")
    data = sample("uint16", (3, 20, 30))
    hdu = fits.PrimaryHDU(data)
    hdu.header["OBJECT"] = "M 33"
    hdu.writeto(tmp_path / "in.fits")
    xisfconv.convert(tmp_path / "in.fits", tmp_path / "mid.xisf", codec="zstd" if xisfconv.codec_available("zstd", True)
                     else "zlib", checksum="sha256")
    read, metadata = xisf_read(tmp_path / "mid.xisf")
    assert same(read, np.moveaxis(data, 0, -1)[::-1])               # XISF: top-down, channels last
    assert metadata["FITSKeywords"]["OBJECT"][0]["value"] == "M 33"
    xisfconv.convert(tmp_path / "mid.xisf", tmp_path / "out.fits")
    with fits.open(tmp_path / "out.fits") as hdus:
        assert same(hdus[0].data, data) and hdus[0].header["OBJECT"] == "M 33"
    # one image of several
    fits.HDUList([fits.PrimaryHDU(data[0]), fits.ImageHDU(data[1], name="SECOND")]).writeto(tmp_path / "two.fits")
    xisfconv.convert(tmp_path / "two.fits", tmp_path / "second.xisf", image=1)
    assert same(xisf_read(tmp_path / "second.xisf")[0][:, :, 0], data[1][::-1])


def test_convert_refusals(tmp_path, light):
    path, data = light
    xisfconv.convert(path, tmp_path / "out.fits")
    before = (tmp_path / "out.fits").read_bytes()
    with pytest.raises(FileExistsError):
        xisfconv.convert(path, tmp_path / "out.fits")
    assert (tmp_path / "out.fits").read_bytes() == before
    xisfconv.convert(path, tmp_path / "out.fits", overwrite=True)
    with pytest.raises(xisfconv.ArgumentError):
        xisfconv.convert(path, tmp_path / "again.xisf")              # XISF to XISF is a rewrite
    with pytest.raises(xisfconv.ArgumentError):
        xisfconv.convert(path, tmp_path / "out.unknown")
    with pytest.raises(xisfconv.ImageIndexError):
        xisfconv.convert(path, tmp_path / "none.fits", image=5)
    with pytest.raises(xisfconv.ImageIndexError):
        xisfconv.convert(path, tmp_path / "none.fits", image=-1)
    with pytest.raises(FileNotFoundError):
        xisfconv.convert(tmp_path / "missing.xisf", tmp_path / "none.fits")
    with pytest.raises(xisfconv.NotFoundError):
        xisfconv.convert(path, tmp_path / "none.png", stretch="stored")   # the file has no saved STF
    with pytest.raises(ValueError):
        xisfconv.convert(path, tmp_path / "none.fits", stretch="sideways")
    with pytest.raises(xisfconv.ArgumentError):
        xisfconv.convert(path, tmp_path / "none.fits", sip_order=9)
    assert sorted(os.listdir(tmp_path)) == ["light.xisf", "out.fits"]


def test_rewrite(tmp_path):
    data = (np.indices((200, 300)).sum(axis=0) % 251).astype(np.uint16)     # compresses well
    plain = tmp_path / "plain.xisf"
    xisfconv.write(plain, data, keywords={"OBJECT": "M 2"})
    codec = "zstd" if xisfconv.codec_available("zstd", writing=True) else "zlib"

    assert xisfconv.stored_as_requested(plain) and xisfconv.stored_as_requested(plain, codec="none")
    assert not xisfconv.stored_as_requested(plain, codec=codec)
    result = xisfconv.rewrite(plain, tmp_path / "small.xisf", codec=codec, checksum="sha256")
    assert isinstance(result, xisfconv.RewriteResult) and result.changed and result.read_back
    assert result.blocks == 1 and result.compressed == 1 and result.checksums == 1
    assert result.input_size == plain.stat().st_size and result.output_size == (tmp_path / "small.xisf").stat().st_size
    assert result.output_size < result.input_size / 2
    assert xisfconv.stored_as_requested(tmp_path / "small.xisf", codec=codec, checksum="sha256")
    if importlib.util.find_spec("xisf"):
        read, metadata = xisf_read(tmp_path / "small.xisf")
        assert same(read, planes_last(data)) and metadata["FITSKeywords"]["OBJECT"][0]["value"] == "M 2"
    assert xisfconv.verify(tmp_path / "small.xisf").verified == 1

    back = xisfconv.rewrite(tmp_path / "small.xisf", tmp_path / "back.xisf", codec="none", checksum="none")
    assert back.decompressed == 1 and back.checksums_removed == 1
    assert same(xisfconv.read(tmp_path / "back.xisf"), data)
    assert xisfconv.stored_as_requested(tmp_path / "back.xisf", codec="none", checksum="none")
    kept = xisfconv.rewrite(tmp_path / "small.xisf", tmp_path / "kept.xisf")      # nothing asked for
    assert kept.kept == 1 and same(xisfconv.read(tmp_path / "kept.xisf"), data)

    with pytest.raises(FileExistsError):
        xisfconv.rewrite(plain, tmp_path / "small.xisf", codec=codec)
    with pytest.raises(xisfconv.ArgumentError):
        xisfconv.rewrite(plain, plain, codec=codec)                  # onto itself: that is rewrite_in_place
    with pytest.raises(ValueError):
        xisfconv.rewrite(plain, tmp_path / "x.xisf", codec="gzip")
    fits.PrimaryHDU(data).writeto(tmp_path / "not.fits")
    with pytest.raises(xisfconv.Error):
        xisfconv.rewrite(tmp_path / "not.fits", tmp_path / "x.xisf", codec=codec)
    assert not (tmp_path / "x.xisf").exists() and not (tmp_path / "x.xisf.part").exists()


def test_rewrite_in_place(tmp_path):
    data = (np.indices((200, 300)).sum(axis=0) % 251).astype(np.uint16)
    path = tmp_path / "file.xisf"
    xisfconv.write(path, [xisfconv.Image(data, name="first"), xisfconv.Image(data[::-1].copy(), name="second")])
    size = path.stat().st_size
    result = xisfconv.rewrite_in_place(path, codec="zlib")
    assert result.changed and result.read_back and result.compressed == 2 and path.stat().st_size < size
    assert same(xisfconv.read(path, "first"), data) and same(xisfconv.read(path, "second"), data[::-1])
    again = xisfconv.rewrite_in_place(path, codec="zlib")            # already as requested: left alone
    assert not again.changed
    one = xisfconv.rewrite_in_place(path, image=1)
    with xisfconv.open(path) as file:
        assert one.changed and [entry.name for entry in file] == ["second"]
    assert sorted(os.listdir(tmp_path)) == ["file.xisf"]
    os.chmod(path, 0o444)
    try:
        if os.access(path, os.W_OK) and getattr(os, "geteuid", lambda: 1)() != 0:
            pytest.skip("the file system does not make files read-only")
        with pytest.raises(xisfconv.FileError):
            xisfconv.rewrite_in_place(path, codec="none")
    finally:
        os.chmod(path, 0o644)


def test_verify(tmp_path):
    data = sample("uint16", (50, 60))
    good = tmp_path / "good.xisf"
    xisfconv.write(good, data, checksum="sha256")
    report = xisfconv.verify(good)
    assert report.ok and not report.failed and report.verdict == "ok" and report.format == "xisf"
    assert report.verified == 1 and report.unchecked == 0 and report.problems == [] and report.not_checked == []
    assert "1 image" in report.summary and "ok" in repr(report)

    plain = tmp_path / "plain.xisf"
    xisfconv.write(plain, data)
    report = xisfconv.verify(plain)
    assert report.ok and report.verified == 0 and report.unchecked == 1

    # one byte of the pixel data changed
    raw = bytearray(good.read_bytes())
    raw[-10] ^= 0x40
    bad = tmp_path / "bad.xisf"
    bad.write_bytes(bytes(raw))
    report = xisfconv.verify(bad)
    assert report.failed and not report.ok and report.verdict == "failed" and len(report.problems) == 1
    assert "checksum" in report.problems[0].lower()
    with pytest.raises(xisfconv.ChecksumError):
        xisfconv.read(bad)
    changed = xisfconv.read(bad, verify=False)                       # read anyway
    assert changed.shape == data.shape and not same(changed, data)
    with pytest.raises(xisfconv.ChecksumError):
        xisfconv.convert(bad, tmp_path / "bad.fits")
    assert not (tmp_path / "bad.fits").exists()
    xisfconv.convert(bad, tmp_path / "bad.fits", verify=False)

    truncated = tmp_path / "cut.xisf"
    truncated.write_bytes(good.read_bytes()[:-100])
    assert xisfconv.verify(truncated).failed
    assert xisfconv.verify(tmp_path / "missing.xisf").failed          # a report, not an exception

    fits.PrimaryHDU(data).writeto(tmp_path / "plain.fits")
    report = xisfconv.verify(tmp_path / "plain.fits")
    assert report.format == "fits" and report.ok
    fits.PrimaryHDU(data).writeto(tmp_path / "sum.fits", checksum=True)
    report = xisfconv.verify(tmp_path / "sum.fits")
    assert report.ok and report.verified >= 1


def test_the_tool_and_the_package_agree(tmp_path, tool, light):
    """The command line tool and the package are the same library: the same files come out."""
    path, data = light
    for extension, options, arguments in [("fits", {}, []), ("asdf", {}, []), ("tif", {"sample_format": "uint16"}, ["-b", "u16"]),
                                          ("png", {"stretch": "linked", "sample_format": "uint8"}, ["--stretch=linked", "-b", "u8"])]:
        mine, theirs = tmp_path / ("mine." + extension), tmp_path / ("tool." + extension)
        xisfconv.convert(path, mine, **options)
        subprocess.run([tool, "-q", str(path), "-o", str(theirs)] + arguments, check=True)
        assert mine.read_bytes() == theirs.read_bytes(), extension
    subprocess.run([tool, "-q", str(tmp_path / "tool.fits"), "-o", str(tmp_path / "tool.xisf")], check=True)
    xisfconv.convert(tmp_path / "mine.fits", tmp_path / "mine.xisf")

    def without_the_time(file):
        # an XISF file says when it was written, and the two were not written in the same second
        return re.sub(rb'(CreationTime" type="TimePoint" value=")[^"]*', rb"\1", file.read_bytes())

    assert b"XISF:CreationTime" in (tmp_path / "mine.xisf").read_bytes()
    assert without_the_time(tmp_path / "mine.xisf") == without_the_time(tmp_path / "tool.xisf")
