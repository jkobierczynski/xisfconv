# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Reading and writing an image a band of rows at a time (0.22): FileImage.read_rows and
FileImage.bands, xisfconv.Writer.begin_image and add_rows."""

import os
import subprocess
import sys

import numpy as np
import pytest

import xisfconv
from util import same, sample

_TESTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests")


def float_image(height=29, width=37, channels=3):
    """Floating point samples beyond 0:1 both ways, every row different."""
    y, x, c = np.indices((height, width, channels))
    return ((x * 0.7 + y * 1.3 + c * 5.0) / 40.0 - 0.2).astype(np.float32)


def files(tmp_path):
    """One image of each kind of storage the readers have: (name, path, image index)."""
    data = float_image()
    gray = sample("uint16", (29, 37), seed=4)
    made = []
    zstd = "zstd" if xisfconv.codec_available("zstd", True) else "zlib"     # (a build without libzstd)
    for name, options in (("plain.xisf", {}), ("zlib.xisf", {"codec": "zlib", "checksum": "sha1"}),
                          ("zstd-big.xisf", {"codec": zstd}), ("plain.fits", {}),
                          ("tiles.fits.fz", {"codec": True}), ("zlib.asdf", {"codec": "zlib"})):
        path = tmp_path / name
        xisfconv.write(path, [data, gray], **options)
        made += [(name, path, 0), (name, path, 1)]
    # FITS whose sample format depends on the data: signed integers that are all positive are
    # read as unsigned ones
    astropy = pytest.importorskip("astropy.io.fits")
    for name, values in (("signed.fits", np.arange(-500, 29 * 37 - 500, dtype=np.int16).reshape(29, 37)),
                         ("positive.fits", np.arange(0, 29 * 37, dtype=np.int16).reshape(29, 37))):
        path = tmp_path / name
        astropy.PrimaryHDU(values).writeto(path)
        made.append((name, path, 0))
    if os.path.exists(os.path.join(_TESTS, "dng_files.py")):
        sys.path.insert(0, _TESTS)
        import dng_files as D
        path = tmp_path / "raw.dng"
        D.write_dng(path, D.bayer_scene(24, 30, 14, 1), bits=14, compression=7, tile=(16, 16))
        made.append(("raw.dng", path, 0))
    return made


@pytest.mark.parametrize("options", [{}, {"sample_format": "uint8"}, {"row_order": "bottom-up"}, {"row_order": None},
                                     {"sample_format": "float64", "bounds": (-1, 2), "row_order": "bottom-up"},
                                     {"channels": "first"}])
def test_rows_are_the_rows_of_the_image(tmp_path, options):
    for name, path, index in files(tmp_path):
        with xisfconv.open(path) as f:
            image = f[index]
            whole = image.read(**options)
            first = options.get("channels") == "first" and whole.ndim == 3
            for rows in (1, 7, image.height, None):
                parts = list(image.bands(rows, **options))
                firsts = [y for y, _ in parts]
                joined = np.concatenate([band for _, band in parts], axis=1 if first else 0)
                assert same(joined, whole), (name, index, options, rows)
                if rows is not None:
                    assert firsts == list(range(0, image.height, rows)), (name, rows)
                else:
                    assert firsts == [0], name   # (small images: one band of about 4 MiB)
            middle = image.read_rows(5, 11, **options)
            assert same(middle, whole[:, 5:16] if first else whole[5:16]), (name, index, options)
            assert image.read_rows(image.height, 0, **options).shape[1 if first else 0] == 0


def test_the_sample_format_is_known_once_rows_are_read(tmp_path):
    astropy = pytest.importorskip("astropy.io.fits")
    path = tmp_path / "positive.fits"
    astropy.PrimaryHDU(np.arange(0, 12, dtype=np.int16).reshape(3, 4)).writeto(path)
    with xisfconv.open(path) as f:
        band = f[0].read_rows(0, 1, row_order="bottom-up")
        assert band.dtype == np.uint16 and f[0].dtype == np.uint16 and same(band, np.array([[0, 1, 2, 3]], np.uint16))


def test_rows_that_are_not_there(tmp_path):
    path = tmp_path / "small.xisf"
    xisfconv.write(path, sample("uint8", (6, 5)))
    with xisfconv.open(path) as f:
        with pytest.raises(xisfconv.ArgumentError, match="rows 4 to 8 of an image of 6 rows"):
            f[0].read_rows(4, 4)
        with pytest.raises(ValueError):
            f[0].read_rows(-1, 2)
        with pytest.raises(TypeError):
            f[0].read_rows(0.5, 2)
        with pytest.raises(ValueError):
            list(f[0].bands(0))
        bands = f[0].bands(4)      # (a generator: nothing is read before it is asked for)
    with pytest.raises(ValueError, match="closed"):
        next(bands)


def test_a_compressed_block_is_decoded_once(tmp_path, monkeypatch):
    path = tmp_path / "zlib.xisf"
    data = sample("uint16", (300, 200), seed=5)
    xisfconv.write(path, data, codec="zlib", checksum="sha256")
    with xisfconv.open(path) as f:
        assert same(np.concatenate([b for _, b in f[0].bands(16)]), data)
        # a damaged block: the checksum is verified when the first rows are read
    raw = bytearray(path.read_bytes())
    raw[-10] ^= 0xFF
    path.write_bytes(bytes(raw))
    with xisfconv.open(path) as f:
        with pytest.raises(xisfconv.ChecksumError):
            f[0].read_rows(0, 1)
        assert f[0].read_rows(0, 1, verify=False).shape == (1, 200)


def test_a_writer_writes_what_write_writes(tmp_path):
    data = sample("uint16", (29, 37, 3), seed=6)
    keywords = {"OBJECT": "M 31", "BAYERPAT": "RGGB", "EXPTIME": 120.0}
    for name, options in (("a.xisf", {"codec": "zlib", "checksum": "sha1", "creator": "band test"}), ("a.fits", {}),
                          ("a.fits.fz", {}), ("a.asdf", {"codec": "zlib"}), ("a.tif", {"codec": True}),
                          ("a.png", {})):
        whole, banded = tmp_path / ("whole-" + name), tmp_path / ("banded-" + name)
        image = dict(keywords=keywords, name="M31", row_order="bottom-up", properties={"Instrument:Filter:Name": "Ha"})
        xisfconv.write(whole, data, **options, **image)
        with xisfconv.Writer(banded, **options) as writer:
            writer.begin_image(37, 29, "uint16", channels=3, **image)
            for y in (0, 4, 5, 15):
                writer.add_rows(data[y:{0: 4, 4: 5, 5: 15, 15: 29}[y]])
        if name.endswith(".xisf"):     # (the time it was written is in the file)
            assert same(xisfconv.read(banded, row_order="bottom-up"), data)
            assert len(banded.read_bytes()) == len(whole.read_bytes())
            with xisfconv.open(whole) as a, xisfconv.open(banded) as b:
                assert a[0].keywords.to_dict() == b[0].keywords.to_dict()
                assert dict(a[0].properties) == dict(b[0].properties)
        else:
            assert banded.read_bytes() == whole.read_bytes(), name


def test_images_given_whole_and_in_bands(tmp_path):
    a, b, c = sample("float32", (8, 9), seed=7), sample("uint8", (5, 6, 2), seed=8), sample("float64", (4, 3), seed=9)
    path = tmp_path / "three.fits"
    with xisfconv.Writer(path) as writer:
        writer.add(a, name="first")
        writer.begin_image(6, 5, np.uint8, channels=2, name="second")
        writer.add_rows(np.moveaxis(b[:2], -1, 0), channels="first")
        writer.add_rows(b[2:])
        writer.add(xisfconv.Image(c, name="third"))
        assert repr(writer) == "<xisfconv.Writer %r, 3 image(s)>" % path
    assert writer.closed
    with xisfconv.open(path) as f:
        assert [image.name for image in f] == ["first", "second", "third"]
        assert same(f[0].read(), a) and same(f[1].read(), b) and same(f[2].read(), c)


def test_what_a_writer_refuses(tmp_path):
    path = tmp_path / "refused.xisf"
    with xisfconv.Writer(path) as writer:
        with pytest.raises(ValueError, match="no image is begun"):
            writer.add_rows(np.zeros((1, 4), np.uint16))
        writer.begin_image(4, 3, "uint16")
        with pytest.raises(TypeError, match="the image is uint16, these rows are float32"):
            writer.add_rows(np.zeros((1, 4), np.float32))
        with pytest.raises(ValueError, match="rows of 1 channel"):
            writer.add_rows(np.zeros((1, 5), np.uint16))
        with pytest.raises(ValueError, match="4 rows more for an image of 3 rows"):
            writer.add_rows(np.zeros((4, 4), np.uint16))
        writer.add_rows(np.zeros((2, 4), np.uint16))
        with pytest.raises(ValueError, match="has had 2 of its 3 rows"):
            writer.add(np.zeros((2, 2), np.uint8))
        with pytest.raises(ValueError, match="has had 2 of its 3 rows"):
            writer.begin_image(2, 2, "uint8")
        writer.add_rows(np.full((1, 4), 7, np.uint16))
    assert same(xisfconv.read(path), np.r_[np.zeros((2, 4), np.uint16), np.full((1, 4), 7, np.uint16)])
    with pytest.raises(ValueError, match="sample format"):
        xisfconv.Writer(tmp_path / "x.xisf").begin_image(2, 2, None)

    # closed before the last row: nothing is written
    incomplete = tmp_path / "incomplete.fits"
    writer = xisfconv.Writer(incomplete)
    writer.begin_image(4, 3, "uint16")
    writer.add_rows(np.zeros((1, 4), np.uint16))
    with pytest.raises(xisfconv.ArgumentError, match="1 of its 3 rows"):
        writer.close()
    assert writer.closed and not incomplete.exists() and not os.path.exists(str(incomplete) + ".part")
    with pytest.raises(ValueError, match="closed"):
        writer.add(np.zeros((2, 2), np.uint8))

    # a block that ends in an exception writes nothing
    with pytest.raises(RuntimeError):
        with xisfconv.Writer(tmp_path / "nothing.tif") as writer:
            writer.add(np.zeros((2, 2), np.uint8))
            raise RuntimeError("stop")
    assert not (tmp_path / "nothing.tif").exists()
    # and a writer that is dropped writes nothing either
    writer = xisfconv.Writer(tmp_path / "dropped.png")
    writer.add(np.zeros((2, 2), np.uint8))
    del writer
    assert not (tmp_path / "dropped.png").exists()
    # a file that is there is not written over without leave
    (tmp_path / "there.fits").write_bytes(b"x")
    with pytest.raises(xisfconv.OutputExistsError):
        with xisfconv.Writer(tmp_path / "there.fits") as writer:
            writer.add(np.zeros((2, 2), np.uint8))
    assert (tmp_path / "there.fits").read_bytes() == b"x"
    with xisfconv.Writer(tmp_path / "there.fits", overwrite=True) as writer:
        writer.add(np.zeros((2, 2), np.uint8))
    assert same(xisfconv.read(tmp_path / "there.fits"), np.zeros((2, 2), np.uint8))


def test_a_writer_reports_progress(tmp_path):
    calls = []
    with xisfconv.Writer(tmp_path / "p.xisf", codec="zlib", progress=lambda *a: calls.append(a)) as writer:
        writer.begin_image(64, 32, "uint16")
        writer.add_rows(sample("uint16", (32, 64)))
    assert calls and all(isinstance(stage, str) for stage, _, _ in calls)

    def stop(*a):
        raise KeyboardInterrupt

    with pytest.raises(KeyboardInterrupt):
        with xisfconv.Writer(tmp_path / "stopped.xisf", codec="zlib", progress=stop) as writer:
            writer.add(sample("uint16", (32, 64)))
    assert not (tmp_path / "stopped.xisf").exists()


def test_an_interrupted_band_and_numbers_beyond_64_bits(tmp_path, monkeypatch):
    path = tmp_path / "interrupted.fits"
    writer = xisfconv.Writer(path)
    writer.begin_image(4, 3, "uint16")
    real = xisfconv._core._library.xisfconv_writer_add_rows

    def taken_then_interrupted(*arguments):
        status = real(*arguments)
        raise KeyboardInterrupt     # (as a Ctrl-C that comes when the library has taken the rows)

    monkeypatch.setattr(xisfconv._core._library, "xisfconv_writer_add_rows", taken_then_interrupted)
    with pytest.raises(KeyboardInterrupt):
        writer.add_rows(np.zeros((1, 4), np.uint16))
    monkeypatch.undo()
    with pytest.raises(ValueError, match="interrupted"):
        writer.add_rows(np.zeros((1, 4), np.uint16))
    with pytest.raises(ValueError, match="interrupted"):
        writer.add(np.zeros((2, 2), np.uint8))
    writer.discard()
    assert not path.exists()

    small = tmp_path / "small.xisf"
    xisfconv.write(small, sample("uint8", (6, 5)))
    with xisfconv.open(small) as f:
        with pytest.raises(ValueError, match="64 bits"):
            f[0].read_rows((1 << 64) + 1, 1)
    with pytest.raises(ValueError, match="64 bits"):
        xisfconv.Writer(tmp_path / "w.xisf").begin_image(1 << 64, 2, "uint8")
    # a file that is there is said when the writer is made, before any image is given
    with pytest.raises(xisfconv.OutputExistsError):
        xisfconv.Writer(small)


@pytest.mark.skipif(sys.platform == "win32", reason="the peak memory of a process: resource is POSIX")
def test_an_image_larger_than_the_memory_it_takes(tmp_path):
    """An image of 128 MB is written and read back in bands, with no memory for held data: the
    process takes far less than the image."""
    program = r"""
import resource, sys
import numpy as np
import xisfconv
path, height, width = sys.argv[1], 8192, 4096
with xisfconv.Writer(path) as writer:
    writer.begin_image(width, height, "float32")
    for y in range(0, height, 256):
        writer.add_rows(np.full((256, width), y, np.float32))
total = 0.0
with xisfconv.open(path) as f:
    for y, band in f[0].bands(256):
        assert band[0, 0] == y and band.shape == (256, width)
        total += float(band[:, 0].sum())
try:    # (Linux: the peak of this program; ru_maxrss of a child starts at what its parent had)
    with open("/proc/self/status") as status:
        mib = [int(line.split()[1]) // 1024 for line in status if line.startswith("VmHWM:")][0]
except OSError:
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    mib = peak // (1024 * 1024) if sys.platform == "darwin" else peak // 1024
print(mib, total)
"""
    script = tmp_path / "large.py"
    script.write_text(program)
    env = dict(os.environ, XISFCONV_MEMORY_LIMIT="0")
    done = subprocess.run([sys.executable, str(script), str(tmp_path / "large.xisf")], capture_output=True, text=True,
                          env=env, timeout=300)
    assert done.returncode == 0, done.stderr
    mib, total = done.stdout.split()
    assert float(total) == sum(float(y) * 256 for y in range(0, 8192, 256))
    assert int(mib) < 100, "a 128 MB image written and read in bands took %s MiB" % mib
    assert (tmp_path / "large.xisf").stat().st_size > 128 << 20
