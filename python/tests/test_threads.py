# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Work spread over threads (0.26): set_threads, and the same bytes and pixels with one thread
and with many, also from Python threads at once."""

import threading

import numpy as np
import pytest

import xisfconv
from util import sample

ZSTD = "zstd" if xisfconv.codec_available("zstd") else "zlib"


@pytest.fixture(autouse=True)
def all_cores_after():
    yield
    xisfconv.set_threads(0)


def test_the_setting():
    assert xisfconv.hardware_threads() >= 1
    assert xisfconv.threads() == 0
    xisfconv.set_threads(3)
    assert xisfconv.threads() == 3
    for wrong, kind in ((-1, ValueError), (257, ValueError), (1.5, TypeError), (True, TypeError), ("2", TypeError)):
        with pytest.raises(kind):
            xisfconv.set_threads(wrong)
    assert xisfconv.threads() == 3


def _bytes_of(path):
    data = path.read_bytes()
    at = data.find(b'XISF:CreationTime" type="TimePoint" value="')
    return data[:at] + data[at + 70:] if at >= 0 else data


def test_the_same_with_any_number_of_threads(tmp_path):
    data = sample("uint16", (211, 307, 3)) // 64   # (compressible)
    made = {}
    for threads in (1, 2, 7):
        xisfconv.set_threads(threads)
        for name, kw in (("z.xisf", dict(codec=ZSTD, checksum="sha256", subblock_size=20000)),
                         ("l.xisf", dict(codec="lz4hc", subblock_size=30000)),
                         ("t.fits.fz", dict(codec="zlib")), ("p.tif", dict(codec="zlib"))):
            (tmp_path / str(threads)).mkdir(exist_ok=True)
            path = tmp_path / str(threads) / name     # (the name is the id of the image)
            xisfconv.write(path, data, **kw)
            made.setdefault(name, []).append(_bytes_of(path))
            if not name.endswith(".tif"):
                assert np.array_equal(np.asarray(xisfconv.read_image(path).data), data)
    for name, versions in made.items():
        assert versions[0] == versions[1] == versions[2], name


def test_python_threads_at_once(tmp_path):
    """Python threads that read and write at the same time share the pool of the library."""
    data = np.round(sample("float32", (300, 400)) * 64) / 64
    source = tmp_path / "source.xisf"
    xisfconv.write(source, data, codec=ZSTD, checksum="sha1", subblock_size=16384)
    failures = []

    def work(k):
        try:
            for round_ in range(3):
                out = tmp_path / ("out-%d-%d.fits.fz" % (k, round_))
                xisfconv.convert(source, out, codec="zlib")
                if not np.array_equal(np.asarray(xisfconv.read_image(out).data), data):
                    failures.append((k, round_))
        except Exception as e:   # noqa: BLE001 - reported below
            failures.append(e)

    workers = [threading.Thread(target=work, args=(k,)) for k in range(4)]
    for t in workers:
        t.start()
    for t in workers:
        t.join()
    assert not failures
