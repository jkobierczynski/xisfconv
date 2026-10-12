# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Files in memory and streams (0.23): open, read, detect_format, verify, convert, rewrite, write
and Writer take bytes-like data and binary file objects where they take a file name."""

import gc
import io
import os
import subprocess
import sys
import textwrap

import numpy as np
import pytest

import xisfconv
from util import same, sample

_PYTHON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def float_image(height=29, width=37, channels=3):
    y, x, c = np.indices((height, width, channels))
    return ((x * 0.7 + y * 1.3 + c * 5.0) / 40.0).astype(np.float32)


@pytest.fixture
def files(tmp_path):
    """A file of each format, with two images: {name: path}."""
    made = {}
    images = [float_image(), sample("uint16", (29, 37), seed=4)]
    for name, options in (("plain.xisf", {}), ("zlib.xisf", {"codec": "zlib", "checksum": "sha1"}),
                          ("plain.fits", {}), ("tiles.fits.fz", {"codec": True}), ("zlib.asdf", {"codec": "zlib"})):
        path = tmp_path / name
        xisfconv.write(path, images, **options)
        made[name] = path
    return made


def images_of(source):
    with xisfconv.open(source) as f:
        return [image.read() for image in f]


def run(program, *arguments, **keywords):
    environment = dict(os.environ, PYTHONPATH=os.pathsep.join([_PYTHON, os.environ.get("PYTHONPATH", "")]))
    environment.update(keywords.pop("env", {}))
    return subprocess.run([sys.executable, "-c", textwrap.dedent(program)] + [str(a) for a in arguments],
                          env=environment, capture_output=True, **keywords)


def test_bytes_in_memory_are_read_as_the_file(files):
    for name, path in files.items():
        data = path.read_bytes()
        expected = images_of(path)
        for given in (memoryview(data), bytearray(data), np.frombuffer(data, dtype=np.uint8)):
            with xisfconv.open(given) as f:
                assert f.path == "<memory>" and f.size == len(data)
                assert f.format == xisfconv.open(path).format
                got = [image.read() for image in f]
                assert all(same(a, b) for a, b in zip(got, expected)) and len(got) == len(expected), name
                # in bands too: what is decoded first is decoded from memory
                bands = np.concatenate([band for _, band in f[0].bands(5)])
                assert same(bands, expected[0])
            assert xisfconv.detect_format(given) == xisfconv.detect_format(path)
            report = xisfconv.verify(given)
            assert report.ok and report.path == "<memory>", report
        assert same(xisfconv.read(memoryview(data), 1), expected[1])


def test_file_objects_are_read_to_their_end(files):
    for name, path in files.items():
        expected = images_of(path)
        with open(path, "rb") as stream:
            with xisfconv.open(stream) as f:
                assert f.path == str(path)       # (the name of the file object)
                assert all(same(a, b) for a, b in zip([image.read() for image in f], expected))
        buffer = io.BytesIO(path.read_bytes())
        assert all(same(a, b) for a, b in zip(images_of(buffer), expected))
        assert xisfconv.verify(io.BytesIO(path.read_bytes())).ok

        class Trickle(io.RawIOBase):
            """A stream that gives a few bytes at a time, by read() alone."""
            def __init__(self, data):
                self.data, self.at = data, 0

            def read(self, size=-1):
                piece = self.data[self.at:self.at + min(size, 7)]
                self.at += len(piece)
                return piece

        trickle = Trickle(path.read_bytes())
        assert all(same(a, b) for a, b in zip(images_of(trickle), expected)) and trickle.at == len(trickle.data)


def test_borrowed_memory_is_kept_while_the_file_is_open(files):
    data = bytearray(files["zlib.xisf"].read_bytes())
    f = xisfconv.open(data)
    with pytest.raises(BufferError):
        data.extend(b"more")             # (the bytes are where the library reads them)
    expected = images_of(files["zlib.xisf"])
    del data
    gc.collect()
    assert same(f[0].read(), expected[0])
    f.close()


def test_more_kinds_of_data(files, tmp_path):
    import mmap
    path = files["zlib.xisf"]
    expected = images_of(path)
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
        mapped.seek(100)
        with xisfconv.open(mapped) as opened:          # borrowed where it is, not read from where it stands
            assert opened.path == "<memory>" and same(opened[0].read(), expected[0])
        assert mapped.tell() == 100
        assert xisfconv.detect_format(mapped) == "xisf"
    data = np.frombuffer(path.read_bytes(), dtype=np.uint8)
    with pytest.raises(TypeError, match="not contiguous"):
        xisfconv.open(memoryview(np.repeat(data, 2))[::2])
    with xisfconv.open(memoryview(data).cast("B", (1, data.size))) as f:    # (any shape of contiguous bytes)
        assert same(f[1].read(), expected[1])
    # a file that is closed lets go of its data
    lent = bytearray(path.read_bytes())
    f = xisfconv.open(lent)
    f.close()
    lent.extend(b"more")

    class Lazy(io.RawIOBase):
        def writable(self):
            return True

        def write(self, data):
            return 0

    with pytest.raises(OSError, match="took 0 of"):
        xisfconv.convert(memoryview(path.read_bytes()), Lazy(), format="fits")


def test_calls_from_a_progress_function(files):
    """A call made while another runs has its own files in memory, under the same names."""
    data = files["plain.xisf"].read_bytes()
    seen = []

    def progress(stage, done, total):
        if len(seen) < 3:
            seen.append(xisfconv.read(memoryview(data)).shape)

    out = io.BytesIO()
    xisfconv.convert(memoryview(data), out, format="fits", progress=progress)
    assert seen and all(shape == (29, 37, 3) for shape in seen)
    assert out.getvalue() == xisfconv_bytes(files["plain.xisf"], "fits")


def xisfconv_bytes(path, format):
    out = io.BytesIO()
    xisfconv.convert(path, out, format=format)
    return out.getvalue()


def test_conversions_to_streams_give_the_bytes_of_files(files, tmp_path):
    source = files["plain.xisf"]
    data = source.read_bytes()
    for ending in (".fits", ".fits.fz", ".asdf", ".tif", ".png"):
        path = tmp_path / ("out" + ending)
        xisfconv.convert(source, path, image=0)
        out = io.BytesIO()
        xisfconv.convert(memoryview(data), out, format=ending.rsplit(".", 1)[1].replace("fz", "fits").replace("tif", "tiff"),
                         image=0, codec=True if ending == ".fits.fz" else None)
        assert out.getvalue() == path.read_bytes(), ending
    # a file object's name tells the format, as a file name does
    path = tmp_path / "named.fits.fz"
    with open(path, "wb") as named:
        xisfconv.convert(io.BytesIO(data), named)
    xisfconv.convert(source, tmp_path / "all.fits.fz")
    assert path.read_bytes() == (tmp_path / "all.fits.fz").read_bytes()
    with pytest.raises(xisfconv.ArgumentError, match="cannot infer output format"):
        xisfconv.convert(memoryview(data), io.BytesIO())
    # XISF from FITS, in memory both ways
    out = io.BytesIO()
    xisfconv.convert(memoryview(files["plain.fits"].read_bytes()), out, format="xisf")
    assert all(same(a, b) for a, b in zip(images_of(out.getbuffer()), images_of(files["plain.fits"])))


def test_rewrites_and_writers_to_streams(files, tmp_path):
    data = files["plain.xisf"].read_bytes()
    out = io.BytesIO()
    result = xisfconv.rewrite(memoryview(data), out, codec="zlib", checksum="sha256")
    assert result.output_size == len(out.getvalue()) and result.input_size == len(data) and result.blocks == 2
    report = xisfconv.verify(out.getbuffer())
    assert report.ok and report.verified == 2
    assert all(same(a, b) for a, b in zip(images_of(out.getbuffer()), images_of(files["plain.xisf"])))
    path = tmp_path / "rewritten.xisf"
    xisfconv.rewrite(io.BytesIO(data), path)
    assert all(same(a, b) for a, b in zip(images_of(path), images_of(files["plain.xisf"])))
    with pytest.raises(xisfconv.ArgumentError, match="one file"):
        xisfconv.rewrite(memoryview(data), _Named("unit.xish"))

    image = sample("uint16", (29, 37), seed=9)
    out = io.BytesIO()
    xisfconv.write(out, image, format="fits")
    xisfconv.write(tmp_path / "written.fits", image)
    assert out.getvalue() == (tmp_path / "written.fits").read_bytes()
    out = io.BytesIO()
    with xisfconv.Writer(out, format="fits") as writer:
        writer.begin_image(37, 29, "uint16")
        for first in range(0, 29, 8):
            writer.add_rows(image[first:first + 8])
        assert out.getvalue() == b""           # (written when the writer is closed)
    assert out.getvalue() == (tmp_path / "written.fits").read_bytes()
    assert xisfconv.Writer(io.BytesIO(), format="png").path == "<output stream>"


class _Named(io.BytesIO):
    """An output with a name, as a file object has one."""
    def __init__(self, name):
        super().__init__()
        self.name = name


def test_what_goes_wrong(files, tmp_path):
    data = files["plain.xisf"].read_bytes()

    class Failing(io.RawIOBase):
        def readable(self):
            return True

        def writable(self):
            return True

        def readinto(self, buffer):
            raise RuntimeError("the input broke")

        def write(self, buffer):
            raise OSError(28, "No space left on device")

    with pytest.raises(RuntimeError, match="the input broke"):
        xisfconv.open(Failing())
    with pytest.raises(OSError, match="No space left"):
        xisfconv.convert(memoryview(data), Failing(), format="fits")
    out = io.BytesIO()
    with pytest.raises(xisfconv.FormatError, match="<memory>"):
        xisfconv.convert(memoryview(b"no file of any format"), out, format="fits")
    assert out.getvalue() == b""             # (a conversion that fails writes nothing)
    with pytest.raises(TypeError, match="binary mode"):
        xisfconv.open(io.StringIO("text"))
    with pytest.raises(TypeError, match="binary mode"):
        xisfconv.convert(memoryview(data), io.StringIO(), format="fits")
    with pytest.raises(TypeError, match="not float"):
        xisfconv.open(1.5)
    with pytest.raises(ValueError, match="memoryview"):
        xisfconv.open(data)                  # (bytes are a file name)
    with pytest.raises(xisfconv.ArgumentError, match="one file"):
        xisfconv.write(_Named("unit.xish"), sample("uint8", (4, 4)))
    with pytest.raises(xisfconv.ArgumentError, match="same name"):
        xisfconv.convert(_Named("a.fits"), _Named("a.fits"))
    with pytest.raises(xisfconv.ArgumentError, match="same name"):
        xisfconv.convert(files["plain.fits"], _Named(str(files["plain.fits"])), format="xisf")
    with open(files["plain.xisf"], "rb") as named:                    # (its name is that of the file)
        with pytest.raises(xisfconv.ArgumentError, match="same name"):
            xisfconv.rewrite(named, files["plain.xisf"], overwrite=True)

    # a header in memory has no directory, where its data blocks file would be
    xisfconv.write(tmp_path / "unit.xish", sample("uint8", (4, 4)))
    with xisfconv.open(memoryview((tmp_path / "unit.xish").read_bytes())) as f:
        with pytest.raises(xisfconv.NotAllowedError, match="in memory"):
            f[0].read()

    # a progress function stops the reading of a stream
    class Stop(Exception):
        pass

    def stop(stage, done, total):
        raise Stop(stage)

    with pytest.raises(Stop, match="reading"):
        xisfconv.convert(io.BytesIO(data), tmp_path / "never.fits", progress=stop)
    assert not (tmp_path / "never.fits").exists()


def test_pipes(files, tmp_path):
    """stdin and stdout of a program, and a stream beyond the memory limit (in a temporary file)."""
    program = """
        import sys, xisfconv
        xisfconv.convert(sys.stdin.buffer, sys.stdout.buffer, format=sys.argv[1])
    """
    xisfconv.convert(files["zlib.xisf"], tmp_path / "expected.fits")
    temporary = tmp_path / "temporary"
    temporary.mkdir()
    for limit in ("256M", "4096"):
        done = run(program, "fits", input=files["zlib.xisf"].read_bytes(),
                   env={"XISFCONV_MEMORY_LIMIT": limit, "TMPDIR": str(temporary), "TEMP": str(temporary),
                        "TMP": str(temporary)})
        assert done.returncode == 0 and done.stderr == b"", done.stderr
        assert done.stdout == (tmp_path / "expected.fits").read_bytes(), limit
    assert list(temporary.iterdir()) == []
    done = run(program, "fits", input=b"")
    assert done.returncode != 0 and b"FormatError" in done.stderr and done.stdout == b""
