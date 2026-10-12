# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""An image of an open FITS or ASDF file is read from where it was found when the file was opened
(0.24): the headers before it, and the tree of an ASDF file, are not read again. The pixels are
those of a read of the whole file, also among HDUs that are not images, and a file that changed
since it was opened is said to have changed."""

import io
import time
import warnings

import numpy as np
import pytest

import xisfconv
from util import same, sample

fits = pytest.importorskip("astropy.io.fits")


def every_image(source, **options):
    with xisfconv.open(source) as f:
        return [image.read(**options) for image in f], [np.concatenate([b for _, b in image.bands(5, **options)])
                                                        for image in f]


def mixed_fits(path):
    """Images among HDUs that are not: a table, an empty HDU, a cube, tile-compressed images, BLANK."""
    rng = np.random.default_rng(24)
    table = fits.BinTableHDU.from_columns([fits.Column(name="x", format="J", array=np.arange(5))])
    blank = fits.ImageHDU(rng.integers(0, 4000, (13, 17)).astype(np.int16))
    blank.header["BLANK"] = -1
    hdus = [fits.PrimaryHDU(rng.integers(0, 60000, (11, 19)).astype(np.uint16)), table,
            fits.ImageHDU(rng.random((3, 9, 8)).astype(np.float32)), fits.ImageHDU(),
            fits.CompImageHDU(rng.integers(0, 4000, (21, 23)).astype(np.int16), compression_type="RICE_1",
                              tile_shape=(4, 23)),
            table.copy(), blank,
            fits.CompImageHDU(rng.random((17, 15)).astype(np.float32), compression_type="GZIP_2", quantize_level=0),
            fits.ImageHDU(rng.normal(0, 1e6, (7, 9)))]
    fits.HDUList(hdus).writeto(path)


def test_fits_images_among_other_hdus(tmp_path):
    path = tmp_path / "mixed.fits"
    mixed_fits(path)
    with warnings.catch_warnings():
        warnings.filterwarnings("ignore", ".*BLANK", xisfconv.XisfconvWarning)
        fits_images_among_other_hdus(path)
    with pytest.warns(xisfconv.XisfconvWarning, match="HDU 6: BLANK"):
        f = xisfconv.open(path)                                     # (said when the file is opened,)
    with pytest.warns(xisfconv.XisfconvWarning, match="HDU 6: BLANK") as told:
        f[3].read()                                                 # (and of that image when it is read)
    assert len(told) == 1
    f.close()


def fits_images_among_other_hdus(path):
    for options in ({}, {"row_order": "bottom-up"}, {"sample_format": "float32"}):
        whole, bands = every_image(path, **options)
        assert len(whole) == 6
        with xisfconv.open(path) as f:
            for index, image in enumerate(f):
                assert same(whole[index], bands[index])
                # as before 0.24: the image read by its number from the start of the file
                assert same(whole[index], xisfconv.read(path, index, **options))
    with xisfconv.open(path) as f:
        assert same(f[1].read(channels="first"), fits.getdata(path, 2)[:, ::-1, :])   # (rows top-down)
    # from memory too
    data = path.read_bytes()
    assert all(same(a, b) for a, b in zip(every_image(memoryview(data))[0], every_image(path)[0]))
    assert all(same(a, b) for a, b in zip(every_image(io.BytesIO(data))[0], every_image(path)[0]))


def test_asdf_arrays_of_every_kind(tmp_path):
    asdf = pytest.importorskip("asdf")
    rng = np.random.default_rng(25)
    rgb = rng.integers(0, 255, (12, 14, 3)).astype(np.uint8)
    big = rng.random((9, 10)).astype(">f8")
    base = rng.integers(0, 60000, (20, 16)).astype(np.uint16)
    tree = {"first": rng.random((8, 6)).astype(np.float32), "nested": {"rgb": rgb, "big": big, "cube": rng.random((2, 5, 7))},
            "view": base[4:12], "base": base, "text": "not an image"}
    for compression in (None, "zlib"):
        path = tmp_path / ("arrays-%s.asdf" % compression)
        af = asdf.AsdfFile(tree)
        if compression:
            for name in ("first", "big"):
                af.set_array_compression(af.tree[name] if name == "first" else af.tree["nested"][name], compression)
        af.write_to(path)
        whole, bands = every_image(path)
        assert len(whole) >= 5
        for index in range(len(whole)):
            assert same(whole[index], bands[index])
            assert same(whole[index], xisfconv.read(path, index))
        with xisfconv.open(path) as f:
            names = [image.name for image in f]
        # (an array without ROWORDER has its rows bottom-up, as in FITS: read() turns them top-down)
        assert same(whole[names.index("nested.rgb")], rgb[::-1])
        assert np.array_equal(whole[names.index("nested.big")], big.astype("<f8")[::-1])
    # what the package writes: an HDU list, compressed
    images = [sample("uint16", (13, 11), seed=s) for s in range(4)]
    xisfconv.write(tmp_path / "list.asdf", images, codec="zlib", checksum=None)
    assert all(same(a, b) for a, b in zip(every_image(tmp_path / "list.asdf")[0], images))


def test_a_thousand_images_are_read_one_by_one_in_little_time(tmp_path):
    """0.23 went through the headers before an image each time: 7 s here for a FITS file of a
    thousand HDUs, 12 s for such an ASDF file; 0.24 takes a fifth of a second at most."""
    images = [np.full((8, 8), i, dtype=np.uint16) for i in range(1000)]
    for name in ("many.fits", "many.asdf"):
        xisfconv.write(tmp_path / name, images)
        start = time.perf_counter()
        with xisfconv.open(tmp_path / name) as f:
            assert all(int(image.read()[0, 0]) == i for i, image in enumerate(f))
        assert time.perf_counter() - start < 3, name


def test_a_file_that_changed_since_it_was_opened(tmp_path):
    images = [sample("uint16", (9, 7), seed=s) for s in range(3)]
    for name in ("changed.fits", "changed.asdf"):
        path = tmp_path / name
        xisfconv.write(path, images)
        with xisfconv.open(path) as f:
            last = f[2]
            xisfconv.write(path, images[:1], overwrite=True)        # (renamed over it: the open file reads the new one)
            with pytest.raises(xisfconv.FormatError, match="changed"):
                last.read()
        with xisfconv.open(path) as f:
            first = f[0]
            xisfconv.write(path, [sample("uint16", (5, 5))], overwrite=True)
            with pytest.raises(xisfconv.FormatError, match="changed"):
                first.read()


def test_an_asdf_tree_that_changed_in_place(tmp_path):
    """The tree says what an array is: a tree that was edited since the file was opened (with the
    blocks where they were) is not taken for the one the image was found in."""
    image = sample("uint16", (6, 7), seed=5)
    path = tmp_path / "edited.asdf"
    xisfconv.write(path, image)
    original = path.read_bytes()
    edits = [(b"datatype: uint16", b"datatype: int16 "), (b"byteorder: little", b"byteorder: big   "),
             (b"shape: [6, 7]", b"shape: [7, 6]")]
    for old, new in edits:
        assert original.count(old) == 1, old
        with xisfconv.open(path) as f:
            first = f[0]
            path.write_bytes(original.replace(old, new))       # (in place: the same length, the blocks where they were)
            with pytest.raises(xisfconv.FormatError, match="changed"):
                first.read()
            path.write_bytes(original)
            assert same(first.read(), image)
    # a file with smaller arrays at the same places
    with xisfconv.open(path) as f:
        first = f[0]
        xisfconv.write(path, sample("uint16", (3, 4)), overwrite=True)
        with pytest.raises(xisfconv.FormatError, match="changed"):
            first.read()


def test_an_asdf_file_by_its_size_and_time(tmp_path):
    """Of a file that was not changed a moment before it was opened, the size and the time of the
    last change tell whether it is the same: the tree is read again only where they do not."""
    import os
    image = sample("uint16", (6, 7), seed=6)
    path = tmp_path / "dated.asdf"
    xisfconv.write(path, image)
    original = path.read_bytes()
    edited = original.replace(b"datatype: uint16", b"datatype: int16 ")
    hour_ago = time.time() - 3600
    os.utime(path, (hour_ago, hour_ago))
    with xisfconv.open(path) as f:
        first = f[0]
        assert same(first.read(), image)
        path.write_bytes(edited)                     # (now: another time)
        with pytest.raises(xisfconv.FormatError, match="changed"):
            first.read()
        os.utime(path, (hour_ago, hour_ago))         # the time set back: taken for the file that was opened
        assert same(first.read(), image)
        path.write_bytes(original)
        assert same(first.read(), image)


def test_an_asdf_file_changed_a_moment_before_it_was_opened(tmp_path):
    """Its time tells nothing at first (a change in the same tick keeps it), so the tree is read
    to tell; once the moment is past, the time tells again, and a change is still seen."""
    image = sample("uint16", (6, 7), seed=7)
    path = tmp_path / "fresh.asdf"
    xisfconv.write(path, image)
    original = path.read_bytes()
    with xisfconv.open(path) as f:                   # (opened right after it was written)
        first = f[0]
        path.write_bytes(original.replace(b"datatype: uint16", b"datatype: int16 "))
        with pytest.raises(xisfconv.FormatError, match="changed"):
            first.read()
        path.write_bytes(original)
        time.sleep(3.5)
        assert same(first.read(), image)             # (read, and now known to be the file that was opened)
        path.write_bytes(original.replace(b"byteorder: little", b"byteorder: big   "))
        with pytest.raises(xisfconv.FormatError, match="changed"):
            first.read()
