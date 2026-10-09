# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""A compression level and byte shuffling for conversions and rewrites (since 0.19)."""

import re
import zlib

import numpy as np
import pytest

import xisfconv
from util import same

fits = pytest.importorskip("astropy.io.fits")


def smooth():
    y, x = np.mgrid[0:200, 0:300]
    return ((x * 3 + y * 5) % 4000 + np.random.default_rng(3).integers(0, 8, (200, 300))).astype(np.uint16)


def first_block(path):
    """(compression attribute, stored bytes) of the first attached block, read without the library."""
    raw = open(path, "rb").read()
    header = raw[16:16 + int.from_bytes(raw[8:12], "little")].decode()
    tag = re.search(r"<Image [^>]*>", header).group(0)
    _, at, size = re.search(r'location="([^"]*)"', tag).group(1).split(":")
    return re.search(r'compression="([^"]*)"', tag).group(1), raw[int(at):int(at) + int(size)]


def test_convert(tmp_path):
    data = smooth()
    fits.PrimaryHDU(data).writeto(tmp_path / "a.fits")
    for level, flevel in ((1, 0), (9, 3), (None, 2)):
        out = tmp_path / ("a%s.xisf" % level)
        xisfconv.convert(tmp_path / "a.fits", out, codec="zlib", level=level)
        compression, stored = first_block(out)
        assert compression.startswith("zlib+sh:") and stored[1] >> 6 == flevel     # zlib's header: the class of its level
        assert same(xisfconv.read(out, row_order="bottom-up"), data)
    xisfconv.convert(tmp_path / "a.fits", tmp_path / "plain.xisf", codec="zlib", shuffle=False)
    compression, stored = first_block(tmp_path / "plain.xisf")
    assert compression.startswith("zlib:") and len(zlib.decompress(stored)) == data.nbytes
    with pytest.raises(xisfconv.ArgumentError, match=r"compression level 10: zlib has the levels 1 to 9"):
        xisfconv.convert(tmp_path / "a.fits", tmp_path / "b.xisf", codec="zlib", level=10)
    with pytest.raises(xisfconv.ArgumentError, match=r"a compression level \(level\) needs a codec that compresses \(codec\)"):
        xisfconv.convert(tmp_path / "a.fits", tmp_path / "b.xisf", level=5)
    with pytest.raises(xisfconv.ArgumentError, match=r"byte shuffling off \(shuffle=False\) is for XISF output"):
        xisfconv.convert(tmp_path / "a.fits", tmp_path / "b.tif", codec=True, shuffle=False)
    with pytest.raises(ValueError):
        xisfconv.convert(tmp_path / "a.fits", tmp_path / "b.xisf", codec="zlib", level=0)
    with pytest.raises(TypeError):
        xisfconv.convert(tmp_path / "a.fits", tmp_path / "b.xisf", codec="zlib", level=True)
    with pytest.raises(ValueError, match="no codec has it"):
        xisfconv.convert(tmp_path / "a.fits", tmp_path / "b.xisf", codec="zlib", level=2 ** 32 + 9)   # (not level 9)
    with pytest.raises(ValueError, match="no codec has it"):
        xisfconv.write(tmp_path / "w.xisf", data, codec="zlib", level=2 ** 32 + 9)
    xisfconv.convert(tmp_path / "a.fits", tmp_path / "none.xisf", codec="zlib", shuffle=None)   # None: the default
    assert first_block(tmp_path / "none.xisf")[0].startswith("zlib+sh:")


def test_rewrite(tmp_path):
    data = smooth()
    xisfconv.write(tmp_path / "a.xisf", data)
    done = xisfconv.rewrite(tmp_path / "a.xisf", tmp_path / "b.xisf", codec="zlib", level=9)
    assert done.compressed == 1 and first_block(tmp_path / "b.xisf")[1][1] >> 6 == 3
    assert not xisfconv.stored_as_requested(tmp_path / "b.xisf", codec="zlib", level=9)     # (no file says its level)
    assert xisfconv.stored_as_requested(tmp_path / "b.xisf", codec="zlib")
    assert not xisfconv.stored_as_requested(tmp_path / "b.xisf", codec="zlib", shuffle=False)
    done = xisfconv.rewrite_in_place(tmp_path / "b.xisf", codec="zlib", shuffle=False)
    assert done.changed and first_block(tmp_path / "b.xisf")[0].startswith("zlib:")
    assert not xisfconv.rewrite_in_place(tmp_path / "b.xisf", codec="zlib", shuffle=False).changed
    assert same(xisfconv.read(tmp_path / "b.xisf"), data)
    with pytest.raises(xisfconv.ArgumentError, match="a compression level without a codec"):
        xisfconv.rewrite(tmp_path / "a.xisf", tmp_path / "c.xisf", level=3)


def test_tool_writes_the_same(tmp_path, tool):
    import subprocess
    data = smooth()
    fits.PrimaryHDU(data).writeto(tmp_path / "a.fits")
    xisfconv.convert(tmp_path / "a.fits", tmp_path / "mine.xisf", codec="zlib", level=3, shuffle=False)
    subprocess.run([tool, str(tmp_path / "a.fits"), "-o", str(tmp_path / "tool.xisf"), "--codec", "zlib", "--level", "3",
                    "--no-shuffle", "-q"], check=True)
    assert first_block(tmp_path / "mine.xisf") == first_block(tmp_path / "tool.xisf")
