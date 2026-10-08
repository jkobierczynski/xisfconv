# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""DNG files, written byte by byte by tests/dng_files.py, read through the package."""

import os
import subprocess
import sys

import numpy as np
import pytest

import xisfconv
from util import same

_HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests")
if not os.path.exists(os.path.join(_HERE, "dng_files.py")):   # pragma: no cover
    pytest.skip("tests/dng_files.py of the source tree is not there", allow_module_level=True)
sys.path.insert(0, _HERE)
import dng_files as D  # noqa: E402


def test_bayer(tmp_path):
    raw = D.bayer_scene(24, 30, 14, 1)
    path = tmp_path / "IMG_0001.DNG"
    D.write_dng(path, raw, bits=14, compression=7, tile=(16, 16), active_area=(2, 4, 22, 28), black=512, white=16383)
    want = raw[2:22, 4:28]
    assert xisfconv.detect_format(path) == "dng"
    assert same(xisfconv.read(path), want)
    assert same(xisfconv.read(path, row_order="bottom-up"), want[::-1])
    with xisfconv.open(path) as f:
        assert f.format == "dng" and len(f) == 1
        assert f.detail("format") == "DNG 1.4.0.0, Canon EOS R5 (dng_files.py)"
        assert f.skipped == ["IFD 0: a preview of 12 x 8 (uncompressed), not read"]
        entry = f[0]
        assert entry.shape == (20, 24) and entry.dtype == np.uint16 and entry.row_order == "top-down"
        assert entry.cfa == ("RGGB", 2, 2) and entry.detail("source") == "IFD 0 / SubIFD 0"
        assert entry.detail("storage") == "JPEG (lossless), 4 tiles of 16 x 16" and entry.bitpix == 16
        keywords = entry.keywords
        assert keywords["INSTRUME"] == "Canon EOS R5" and keywords["BAYERPAT"] == "RGGB"
        assert keywords["DATE-OBS"] == "2026-03-14T20:05:09.25" and keywords["BLKLEVEL"] == 512
        assert "BAYERPAT" in f.header_text and f.header_text.startswith("IFD 0 / SubIFD 0\n")
    image = xisfconv.read_image(path, row_order="bottom-up")
    assert image.keywords["BAYERPAT"] == "GBRG"   # turned with the rows (an even height)
    report = xisfconv.verify(path)
    assert report.ok and report.format == "dng" and report.summary.startswith("raw image 24 x 20")


def test_xtrans_and_linear_raw(tmp_path):
    raw = D.bayer_scene(12, 18, 14, 2)
    path = tmp_path / "xt.dng"
    D.write_dng(path, raw, bits=14, pattern=D.XTRANS, pattern_size=(6, 6))
    with xisfconv.open(path) as f:
        assert f[0].cfa == ("".join("RGB"[k] for k in D.XTRANS), 6, 6) and "BAYERPAT" not in f[0].keywords
    rgb = np.stack([D.bayer_scene(10, 14, 16, k) for k in range(3)], axis=2)
    path = tmp_path / "rgb.dng"
    D.write_dng(path, rgb, bits=16, photometric="linear", compression=8, predictor=2)
    with xisfconv.open(path) as f:
        assert f[0].color_space == "rgb" and f[0].cfa is None
        assert same(f[0].read(), rgb)


def test_convert_and_errors(tmp_path, tool):
    raw = D.bayer_scene(16, 20, 12, 3)
    path = tmp_path / "a.dng"
    D.write_dng(path, raw, bits=12)
    xisfconv.convert(path, tmp_path / "mine.xisf")
    subprocess.run([tool, str(path), "-o", str(tmp_path / "tool.xisf"), "-q"], check=True)
    assert same(xisfconv.read(tmp_path / "mine.xisf"), raw)
    with xisfconv.open(tmp_path / "mine.xisf") as f:
        assert f[0].cfa == ("RGGB", 2, 2)
    with pytest.raises(xisfconv.Error, match="DNG is read, not written"):
        xisfconv.convert(path, tmp_path / "out.dng")
    with pytest.raises(ValueError):
        xisfconv.convert(path, tmp_path / "out.x", format="dng")
    lossy = tmp_path / "lossy.dng"
    D.write_dng(lossy, raw, bits=12, compression=34892)
    with pytest.raises(xisfconv.Error, match="lossy DNG"):
        xisfconv.open(lossy)
    report = xisfconv.verify(lossy)
    assert report.verdict == "not fully checked" and "lossy DNG" in report.not_checked[0]
