# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""The stretch functions, against the formulas written out in NumPy."""

import numpy as np
import pytest

import xisfconv
from xisfconv import StretchParams
from util import same


def mtf(m, x):
    """PixInsight's midtones transfer function."""
    x = np.asarray(x, np.float64)
    return (m - 1) * x / ((2 * m - 1) * x - m)


def stretched(x, p):
    x = np.asarray(x, np.float64)
    x1 = np.clip((x - p.shadows) / (p.highlights - p.shadows), 0, 1)
    return np.clip((mtf(p.midtones, x1) - p.low) / (p.high - p.low), 0, 1)


def auto_stf(x):
    """Auto-STF of a dark image: shadows at median - 2.8 MADN, the background to 0.25."""
    x = np.asarray(x, np.float64).reshape(-1)
    median = np.median(x)
    madn = 1.4826 * np.median(np.abs(x - median))
    shadows = max(0.0, median - 2.8 * madn)
    return shadows, float(mtf(0.25, median - shadows))


def night_sky(shape, seed=4, level=0.08):
    rng = np.random.default_rng(seed)
    sky = rng.normal(level, 0.01, shape)
    sky[rng.random(shape) > 0.995] += 0.6                    # a few stars
    return np.clip(sky, 0, 1).astype(np.float32)


def test_apply_stretch():
    data = night_sky((50, 70))
    params = StretchParams(0.05, 0.1, 0.9)
    assert params.low == 0.0 and params.high == 1.0
    out = xisfconv.apply_stretch(data, [params])
    assert out.dtype == np.float32 and out.shape == data.shape and out.min() >= 0 and out.max() <= 1
    assert np.allclose(out, stretched(data, params), atol=2e-6)
    full = StretchParams(0.02, 0.3, 0.8, 0.1, 0.95)
    assert np.allclose(xisfconv.apply_stretch(data, [tuple(full)]), stretched(data, full), atol=2e-6)

    # integers use their full range; the result is laid out like the input
    counts = np.round(data * 65535).astype(np.uint16)
    assert np.allclose(xisfconv.apply_stretch(counts, [params]), stretched(counts / 65535.0, params), atol=2e-6)
    colour = np.stack([data, data * 0.5, data * 0.25], axis=-1)
    three = [StretchParams(0.01, 0.2, 1.0), StretchParams(0.02, 0.3, 1.0), StretchParams(0.0, 0.4, 0.9)]
    out = xisfconv.apply_stretch(colour, three)
    assert out.shape == colour.shape
    for c in range(3):
        assert np.allclose(out[:, :, c], stretched(colour[:, :, c], three[c]), atol=2e-6)
    first = xisfconv.apply_stretch(np.moveaxis(colour, -1, 0), three, channels="first")
    assert same(first, np.moveaxis(out, -1, 0))
    # a range other than 0..1
    scaled = (data * 4000).astype(np.float32)
    assert np.allclose(xisfconv.apply_stretch(scaled, [params], bounds=(0, 4000)), stretched(data, params), atol=5e-6)
    # fewer stretches than channels: the rest (alpha) is only normalized
    with_alpha = np.stack([data, data, data, np.full_like(data, 0.5)], axis=-1)
    out = xisfconv.apply_stretch(with_alpha, three)
    assert np.allclose(out[:, :, 3], 0.5, atol=1e-6)

    with pytest.raises(ValueError):
        xisfconv.apply_stretch(data, [])
    with pytest.raises(ValueError):
        xisfconv.apply_stretch(data, three)                  # three stretches for one channel
    with pytest.raises(TypeError):
        xisfconv.apply_stretch(data, [5])
    with pytest.raises(TypeError):
        xisfconv.apply_stretch(data.astype(np.int16), [params])


def test_auto_stretch():
    data = night_sky((120, 160))
    [params] = xisfconv.auto_stretch(data)
    shadows, midtones = auto_stf(data)
    assert isinstance(params, StretchParams) and params.highlights == 1.0
    assert params.shadows == pytest.approx(shadows, abs=2e-5) and params.midtones == pytest.approx(midtones, abs=2e-4)
    # the background lands near 0.25
    assert np.median(xisfconv.apply_stretch(data, [params])) == pytest.approx(0.25, abs=0.02)

    colour = np.stack([night_sky((120, 160), 1, 0.05), night_sky((120, 160), 2, 0.10), night_sky((120, 160), 3, 0.15)],
                      axis=-1)
    unlinked = xisfconv.auto_stretch(colour, linked=False)
    assert len(unlinked) == 3
    for c in range(3):
        shadows, midtones = auto_stf(colour[:, :, c])
        assert unlinked[c].shadows == pytest.approx(shadows, abs=2e-5)
        assert unlinked[c].midtones == pytest.approx(midtones, abs=2e-4)
    linked = xisfconv.auto_stretch(colour)
    assert len(linked) == 3 and linked[0] == linked[1] == linked[2]      # one stretch keeps the colour balance
    assert unlinked[0] != unlinked[2]
    assert xisfconv.auto_stretch(np.moveaxis(colour, -1, 0), channels="first") == linked
    assert len(xisfconv.auto_stretch(colour, color_channels=1)) == 1
    with pytest.raises(ValueError):
        xisfconv.auto_stretch(colour, color_channels=4)

    # integers and another range give the same stretch
    counts = np.round(data * 65535).astype(np.uint16)
    [from_counts] = xisfconv.auto_stretch(counts)
    assert from_counts.shadows == pytest.approx(params.shadows, abs=2e-5)
    [from_range] = xisfconv.auto_stretch((data * 1000).astype(np.float32), bounds=(0, 1000))
    assert from_range.shadows == pytest.approx(params.shadows, abs=2e-5)


def test_stretch_as_the_converter_does_it(tmp_path):
    """convert(stretch="linked") writes what auto_stretch and apply_stretch compute."""
    tifffile = pytest.importorskip("tifffile")
    colour = np.stack([night_sky((60, 80), 1, 0.05), night_sky((60, 80), 2, 0.10), night_sky((60, 80), 3, 0.15)], axis=-1)
    xisfconv.write(tmp_path / "in.xisf", colour)
    for mode, linked in (("linked", True), ("unlinked", False), ("auto", True)):
        out = tmp_path / (mode + ".tif")
        xisfconv.convert(tmp_path / "in.xisf", out, stretch=mode, sample_format="float32")
        mine = xisfconv.apply_stretch(colour, xisfconv.auto_stretch(colour, linked=linked))
        assert np.allclose(tifffile.imread(out), mine, atol=2e-6)
    with xisfconv.open(tmp_path / "in.xisf") as file:
        assert file[0].stored_stretch is None and not file[0].has_stored_stretch
