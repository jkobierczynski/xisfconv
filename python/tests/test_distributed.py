# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Distributed XISF units: a header file (.xish) and the files it names (data blocks files,
.xisb, and others). What the package writes is taken apart here without it; what it reads is
made here without it."""

import base64
import hashlib
import os
import re
import struct
import subprocess
import sys
import threading
import zlib

import numpy as np
import pytest

import xisfconv
from util import sample, same

HEADER = ('<?xml version="1.0" encoding="UTF-8"?>\n'
          '<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">%s</xisf>\n')


# --- the two files, without the package -------------------------------------------------------

def blocks_file(path):
    """The index of an XISF data blocks file (XISF 1.0, section 9.4): {id: stored bytes}, the
    uncompressed lengths by id, and the positions."""
    raw = open(path, "rb").read()
    assert raw[:8] == b"XISB0100" and raw[8:16] == bytes(8)
    stored, uncompressed, positions = {}, {}, {}
    node = 16
    while node:
        length, reserved, following = struct.unpack_from("<IIQ", raw, node)
        assert reserved == 0
        for element in range(length):
            identifier, position, size, plain, zero = struct.unpack_from("<QQQQQ", raw, node + 16 + 40 * element)
            assert zero == 0
            if position:
                assert identifier not in stored
                stored[identifier] = raw[position:position + size]
                uncompressed[identifier] = plain
                positions[identifier] = position
        node = following
    return stored, uncompressed, positions


def header_of(path):
    text = open(path, "rb").read()
    assert text.startswith(b'<?xml version="1.0" encoding="UTF-8"?>') and text.rstrip().endswith(b"</xisf>")
    return text.decode("utf-8")


def pixels_of(path):
    """The pixels of the first image of a unit the package wrote, [channels, height, width]."""
    header = header_of(path)
    image = re.search(r"<Image\b[^>]*>", header).group(0)
    attribute = lambda name: (re.search(r'\b%s="([^"]*)"' % name, image) or [None, None])[1]   # noqa: E731
    width, height, channels = (int(n) for n in attribute("geometry").split(":"))
    name, identifier = re.fullmatch(r"path\(@header_dir/([^)]*)\):(0x[0-9a-f]{16})", attribute("location")).groups()
    stored, uncompressed, _ = blocks_file(os.path.join(os.path.dirname(path), name))
    data = stored[int(identifier, 16)]
    compression = attribute("compression")
    if compression:
        codec, size = compression.split(":")[:2]
        assert uncompressed[int(identifier, 16)] == int(size)
        if codec.startswith("zlib"):
            data = zlib.decompress(data)
            if codec.endswith("+sh"):
                item = int(compression.split(":")[2])
                data = np.frombuffer(data, np.uint8).reshape(item, -1).T.tobytes()
        else:
            pytest.skip("no decoder for %s here" % codec)
    else:
        assert uncompressed[int(identifier, 16)] == 0
    dtype = {"UInt8": "u1", "UInt16": "<u2", "UInt32": "<u4", "Float32": "<f4", "Float64": "<f8"}[attribute("sampleFormat")]
    return np.frombuffer(data, dtype).reshape(channels, height, width)


def write_blocks_file(path, blocks, nodes=1, align=16):
    """Writes blocks {id: bytes or (stored bytes, uncompressed length)} into a data blocks file,
    its index in `nodes` nodes that lie between the blocks, with a free element in each."""
    items = list(blocks.items())
    per_node = -(-len(items) // nodes)
    out = bytearray(b"XISB0100" + bytes(8))
    previous = None
    for first in range(0, max(len(items), 1), per_node):
        chunk = items[first:first + per_node]
        while len(out) % 8:
            out += b"\0"
        node = len(out)
        if previous is not None:
            struct.pack_into("<Q", out, previous + 8, node)
        out += struct.pack("<IIQ", len(chunk) + 1, 0, 0) + bytes(40 * (len(chunk) + 1))
        for number, (identifier, block) in enumerate(chunk):
            data, plain = block if isinstance(block, tuple) else (block, 0)
            while len(out) % align:
                out += b"\0"
            struct.pack_into("<QQQQQ", out, node + 16 + 40 * number, identifier, len(out), len(data), plain, 0)
            out += data
        previous = node
    with open(path, "wb") as f:
        f.write(out)


def gray_unit(directory, name="made.xish", location="path(@header_dir/made.dat)", more=""):
    """A header file for an 8 x 6 image of 16 bits whose pixels are at `location`; the pixels."""
    pixels = sample("uint16", (6, 8), seed=5)
    path = os.path.join(str(directory), name)
    image = '<Image geometry="8:6:1" sampleFormat="UInt16" colorSpace="Gray" location="%s">%s</Image>' % (location, more)
    with open(path, "wb") as f:
        f.write((HEADER % image).encode("utf-8"))
    return path, pixels


def slashes(path):
    return os.path.abspath(str(path)).replace(os.sep, "/")


# --- written ------------------------------------------------------------------------------------

@pytest.mark.parametrize("dtype", ["uint8", "uint16", "uint32", "float32", "float64"])
@pytest.mark.parametrize("channels", [1, 3])
@pytest.mark.parametrize("codec", [None, "zlib"])
def test_a_unit_is_written_as_the_specification_says(tmp_path, dtype, channels, codec):
    data = sample(dtype, (9, 13) if channels == 1 else (9, 13, channels))
    if codec:
        data = (np.indices(data.shape).sum(axis=0) % 5).astype(dtype)     # something that compresses
    path = tmp_path / "frame.xish"
    xisfconv.write(path, data, codec=codec, checksum="sha256")
    assert sorted(os.listdir(tmp_path)) == ["frame.xisb", "frame.xish"]
    header = header_of(path)
    assert "attachment:" not in header and "XISF:BlockAlignmentSize" not in header
    planes = pixels_of(str(path))
    assert same(planes, np.moveaxis(data.reshape(9, 13, channels), 2, 0))
    stored, _, positions = blocks_file(tmp_path / "frame.xisb")
    assert len(stored) == 1 and all(position % 4096 == 0 for position in positions.values())
    # the checksum of the header is that of the bytes in the other file
    name, digest = re.search(r'checksum="([^:"]+):([0-9a-f]+)"', header).groups()
    assert name in ("sha-256", "sha256") and hashlib.sha256(next(iter(stored.values()))).hexdigest() == digest
    # and the package reads it back
    assert same(xisfconv.read(path), data)
    with xisfconv.open(path) as file:
        assert file.format == "xisf" and file.unit == "distributed" and file.detail("version") == "1.0"
        assert [os.path.normcase(f) for f in file.external_files] == [os.path.normcase(os.path.abspath(tmp_path / "frame.xisb"))]
        assert file.size == os.path.getsize(path)
        assert file.unit_size == os.path.getsize(path) + os.path.getsize(tmp_path / "frame.xisb")
        assert file[0].detail("compression").startswith("zlib") == bool(codec)
    report = xisfconv.verify(path)
    assert report.ok and report.verified == 1 and "distributed unit with data in 1 other file" in report.summary


def test_identifiers_are_new_for_every_unit(tmp_path):
    data = sample("uint16", (5, 7))
    seen = set()
    for number in range(4):
        xisfconv.write(tmp_path / "u.xish", data, overwrite=True)
        identifier = re.search(r"0x[0-9a-f]{16}", header_of(tmp_path / "u.xish")).group(0)
        assert identifier not in seen and int(identifier, 16) != 0
        seen.add(identifier)
    # so that a header never finds its pixels in the data blocks file of another one
    xisfconv.write(tmp_path / "v.xish", data + 1)
    os.replace(tmp_path / "v.xisb", tmp_path / "u.xisb")
    with pytest.raises(xisfconv.FormatError, match="has no block 0x"):
        xisfconv.read(tmp_path / "u.xish")


def test_an_existing_unit_is_not_written_over(tmp_path):
    data = sample("uint16", (5, 7))
    path = tmp_path / "u.xish"
    xisfconv.write(path, data)
    before = {name: open(tmp_path / name, "rb").read() for name in os.listdir(tmp_path)}
    with pytest.raises(FileExistsError, match="u.xish already exists"):
        xisfconv.write(path, data + 1)
    os.remove(path)
    with pytest.raises(FileExistsError, match=r"u.xisb already exists \(use overwrite=True"):
        xisfconv.write(path, data + 1)
    assert os.listdir(tmp_path) == ["u.xisb"] and open(tmp_path / "u.xisb", "rb").read() == before["u.xisb"]
    xisfconv.write(path, data + 1, overwrite=True)
    assert same(xisfconv.read(path), data + 1) and sorted(os.listdir(tmp_path)) == ["u.xisb", "u.xish"]
    # in capitals, the other file is in capitals too
    xisfconv.write(tmp_path / "FRAME.XISH", data)
    assert "FRAME.XISB" in os.listdir(tmp_path) and same(xisfconv.read(tmp_path / "FRAME.XISH"), data)
    # another format under such a name is that format, in one file
    xisfconv.write(tmp_path / "fits.xish", data, format="fits")
    assert open(tmp_path / "fits.xish", "rb").read(9) == b"SIMPLE  =" and not os.path.exists(tmp_path / "fits.xisb")


def test_several_images_keywords_and_properties(tmp_path):
    first = xisfconv.Image(sample("float32", (6, 9, 3)), name="colour", keywords={"OBJECT": "M 31", "EXPTIME": 30.5},
                           properties={"Observation:Object:Name": "M 31", "Test:Vector": np.arange(500, dtype=np.float64),
                                       "Test:Matrix": np.arange(12, dtype=np.int32).reshape(3, 4)})
    second = xisfconv.Image(sample("uint8", (4, 5)), name="mask")
    path = tmp_path / "set.xish"
    xisfconv.write(path, [first, second], codec="zlib", file_properties={"Test:Text": "of the file"})
    stored, _, _ = blocks_file(tmp_path / "set.xisb")
    assert len(stored) >= 3                               # two images, and the vector at least
    with xisfconv.open(path) as file:
        assert len(file) == 2 and [image.name for image in file] == ["colour", "mask"]
        assert same(file[0].read(), first.data) and same(file[1].read(), second.data)
        assert file[0].keywords["OBJECT"] == "M 31" and file[0].keywords["EXPTIME"] == 30.5
        properties = file[0].properties
        assert np.array_equal(properties["Test:Vector"], np.arange(500.0))
        assert np.array_equal(properties["Test:Matrix"], np.arange(12).reshape(3, 4))
        assert file.properties["Test:Text"] == "of the file"
    image = xisfconv.read_image(path, image="mask")
    assert same(image.data, second.data)


# --- rewritten ----------------------------------------------------------------------------------

def test_packed_unpacked_and_in_place(tmp_path):
    data = (np.indices((40, 50)).sum(axis=0) % 9).astype(np.uint16)
    mono = tmp_path / "mono.xisf"
    xisfconv.write(mono, data, codec="zlib", checksum="sha1", properties={"Test:Vector": np.arange(3000.0)})
    unit = tmp_path / "unit.xish"
    result = xisfconv.rewrite(mono, unit)
    assert result.changed and result.read_back and result.kept == result.blocks == 2
    assert result.input_size == os.path.getsize(mono)
    assert result.output_size == os.path.getsize(unit) + os.path.getsize(tmp_path / "unit.xisb")
    assert same(xisfconv.read(unit), data) and same(pixels_of(str(unit))[0], data)
    # the blocks are the bytes they were
    stored, _, _ = blocks_file(tmp_path / "unit.xisb")
    raw = open(mono, "rb").read()
    assert all(block in raw for block in stored.values()) and len(stored) == 2
    with pytest.raises(FileExistsError):
        xisfconv.rewrite(mono, unit)
    with pytest.raises(ValueError, match="is a file the input reads its data from"):
        xisfconv.rewrite(unit, tmp_path / "unit.xisb", overwrite=True)
    with pytest.raises(ValueError, match="rewrite_in_place"):
        xisfconv.rewrite(unit, unit, overwrite=True)
    # packed again: one file, and the same blocks
    packed = tmp_path / "packed.xisf"
    result = xisfconv.rewrite(unit, packed)
    assert result.changed and result.kept == 2 and result.input_size == os.path.getsize(unit) + os.path.getsize(tmp_path / "unit.xisb")
    with xisfconv.open(packed) as file:
        assert file.unit == "monolithic" and file.external_files == [] and file.unit_size == file.size
        assert same(file[0].read(), data) and np.array_equal(file[0].properties["Test:Vector"], np.arange(3000.0))
    again = open(packed, "rb").read()
    assert all(block in again for block in stored.values())
    # in place: both files are replaced, and nothing is left beside them
    assert xisfconv.stored_as_requested(unit, codec="zlib", checksum="sha1")
    assert not xisfconv.rewrite_in_place(unit, codec="zlib", checksum="sha1").changed
    assert not xisfconv.stored_as_requested(unit, codec="none")
    size = os.path.getsize(tmp_path / "unit.xisb")
    result = xisfconv.rewrite_in_place(unit, codec="none", checksum="none")
    assert result.changed and result.decompressed == 2 and result.checksums_removed == 2 and result.read_back
    assert os.path.getsize(tmp_path / "unit.xisb") > size and same(xisfconv.read(unit), data)
    assert sorted(os.listdir(tmp_path)) == ["mono.xisf", "packed.xisf", "unit.xisb", "unit.xish"]
    assert same(pixels_of(str(unit))[0], data)
    # one image of several, into a unit
    xisfconv.write(tmp_path / "two.xisf", [data, data[:5, :6] + 1])
    xisfconv.rewrite(tmp_path / "two.xisf", tmp_path / "one.xish", image=1)
    with xisfconv.open(tmp_path / "one.xish") as file:
        assert len(file) == 1 and same(file[0].read(), data[:5, :6] + 1)


def test_converted_from_a_unit_and_to_one(tmp_path):
    fits = pytest.importorskip("astropy.io.fits")
    data = sample("uint16", (12, 17))
    unit = tmp_path / "frame.xish"
    xisfconv.write(unit, data, keywords={"OBJECT": "NGC 7000"})
    xisfconv.convert(unit, tmp_path / "frame.fits")
    with fits.open(tmp_path / "frame.fits") as hdul:
        assert same(hdul[0].data[::-1], data) and hdul[0].header["OBJECT"] == "NGC 7000"
    xisfconv.convert(tmp_path / "frame.fits", tmp_path / "back.xish", codec="zlib")
    assert same(pixels_of(str(tmp_path / "back.xish"))[0], data)
    assert xisfconv.File(tmp_path / "back.xish").unit == "distributed"
    xisfconv.convert(unit, tmp_path / "frame.png", sample_format="uint8")
    assert open(tmp_path / "frame.png", "rb").read(4) == b"\x89PNG"
    with pytest.raises(ValueError, match="rewrite"):
        xisfconv.convert(unit, tmp_path / "copy.xish")


# --- made without the package, and read -------------------------------------------------------

@pytest.mark.parametrize("nodes", [1, 3])
def test_a_unit_made_by_hand(tmp_path, nodes):
    pixels = sample("float32", (7, 11, 3), seed=3)
    planes = np.moveaxis(pixels, 2, 0).astype("<f4").tobytes()
    vector = np.linspace(0, 1, 77).astype("<f8").tobytes()
    packed = zlib.compress(vector)
    icc = bytes(range(200))
    blocks = {0x1122334455667788: planes, 7: (packed, len(vector)), 0xFFFFFFFFFFFFFFFF: icc}
    directory = tmp_path / "data files"
    directory.mkdir()
    write_blocks_file(directory / "all (1).xisb", blocks, nodes=nodes)
    where = "path(@header_dir/data files/all \\(1\\).xisb)"
    body = ('<Image geometry="11:7:3" sampleFormat="Float32" bounds="0:1" colorSpace="RGB" location="%s:0x1122334455667788">'
            '<Property id="Test:Vector" type="F64Vector" length="77" location="%s:7" compression="zlib:%d"/>'
            '<ICCProfile location="%s:18446744073709551615"/>'
            '<Property id="Test:Inline" type="UI8Vector" length="4" location="inline:base64">%s</Property>'
            '</Image>') % (where, where, len(vector), where, base64.b64encode(bytes([1, 2, 3, 4])).decode())
    unit = tmp_path / "hand.xish"
    unit.write_bytes((HEADER % body).encode("utf-8"))
    with xisfconv.open(unit) as file:
        assert file.unit == "distributed" and len(file.external_files) == 1
        assert os.path.samefile(file.external_files[0], directory / "all (1).xisb")
        assert file.unit_size == os.path.getsize(unit) + os.path.getsize(directory / "all (1).xisb")
        assert same(file[0].read(), pixels)
        assert np.array_equal(file[0].properties["Test:Vector"], np.linspace(0, 1, 77))
        assert list(file[0].properties["Test:Inline"]) == [1, 2, 3, 4]
        assert file[0].icc_profile == icc
    assert xisfconv.verify(unit).verdict in ("ok", "not fully checked") and not xisfconv.verify(unit).problems
    # packed into one file, every block of it
    xisfconv.rewrite(unit, tmp_path / "hand.xisf")
    raw = open(tmp_path / "hand.xisf", "rb").read()
    assert planes in raw and packed in raw and icc in raw
    with xisfconv.open(tmp_path / "hand.xisf") as file:
        assert same(file[0].read(), pixels) and file[0].icc_profile == icc and file.external_files == []


def test_a_file_that_is_one_block(tmp_path):
    unit, pixels = gray_unit(tmp_path)
    (tmp_path / "made.dat").write_bytes(pixels.astype("<u2").tobytes())
    with xisfconv.open(unit) as file:
        assert file.unit_size == os.path.getsize(unit) + 96 and same(file[0].read(), pixels)
    (tmp_path / "made.dat").write_bytes(pixels.astype("<u2").tobytes() + b"\0")
    with pytest.raises(xisfconv.FormatError, match="geometry requires 96"):
        xisfconv.read(unit)
    os.remove(tmp_path / "made.dat")
    with xisfconv.open(unit) as file:                     # the header is all there is to open
        assert file[0].shape == (6, 8) and file.unit_size == file.size and len(file.external_files) == 1
        with pytest.raises(OSError, match="made.dat"):
            file[0].read()
    report = xisfconv.verify(unit)
    assert report.failed and "made.dat" in report.problems[0]


def test_what_is_no_header_file(tmp_path):
    data = sample("uint16", (5, 7))
    xisfconv.write(tmp_path / "u.xish", data)
    with pytest.raises(xisfconv.FormatError, match=r"data blocks file \(\.xisb\).*\(\.xish\)"):
        xisfconv.open(tmp_path / "u.xisb")
    with pytest.raises(xisfconv.FormatError, match=r"\.xish"):
        xisfconv.read(tmp_path / "u.xisb")
    report = xisfconv.verify(tmp_path / "u.xisb")
    assert report.failed and ".xish" in report.problems[0]
    (tmp_path / "svg.xish").write_text('<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg"/>')
    with pytest.raises(xisfconv.FormatError, match="not an XISF header"):
        xisfconv.open(tmp_path / "svg.xish")
    (tmp_path / "attached.xish").write_bytes((HEADER % '<Image geometry="7:5:1" sampleFormat="UInt16" colorSpace="Gray" '
                                              'location="attachment:4096:70"/>').encode())
    with pytest.raises(xisfconv.FormatError, match="nothing is attached"):
        xisfconv.read(tmp_path / "attached.xish")
    # a header file under another name is one all the same, it is what it holds; but it is not
    # followed to its data by its own word: only a file that is named as a header file is
    os.replace(tmp_path / "u.xish", tmp_path / "u.xml")
    with xisfconv.open(tmp_path / "u.xml") as file:
        assert file.unit == "distributed" and file[0].shape == (5, 7) and file.unit_size == file.size
        with pytest.raises(xisfconv.NotAllowedError, match=r"does not have the name of one: only a header file \(\.xish\)"):
            file[0].read()
    assert same(xisfconv.read(tmp_path / "u.xml", external_files="anywhere"), data)
    # a large XML file that is something else is told from its beginning, not read
    (tmp_path / "large.xish").write_bytes(b'<?xml version="1.0"?>\n<!-- ' + b"x" * 200000 + b' -->\n<xisf version="1.0"/>')
    with pytest.raises(xisfconv.FormatError, match="does not begin with a root element <xisf>"):
        xisfconv.open(tmp_path / "large.xish")


def test_a_monolithic_file_is_not_followed_to_other_files(tmp_path):
    """What people are sent as "an image" (.xisf) holds all of its data by the specification. One
    that names the file beside it is not followed there."""
    pixels = sample("uint16", (6, 8), seed=5)
    (tmp_path / "id_ed25519").write_bytes(pixels.astype("<u2").tobytes())
    header = (HEADER % '<Image geometry="8:6:1" sampleFormat="UInt16" colorSpace="Gray" '
              'location="path(@header_dir/id_ed25519)"/>').encode()
    mono = tmp_path / "m42.xisf"
    mono.write_bytes(b"XISF0100" + struct.pack("<II", len(header), 0) + header)
    with pytest.warns(xisfconv.XisfconvWarning, match="names data in other files"):
        file = xisfconv.open(mono)
    with file:
        assert file.unit == "monolithic" and len(file.external_files) == 1 and file.unit_size == file.size
        with pytest.raises(xisfconv.NotAllowedError, match="this is a monolithic XISF file, which holds all of its data"):
            file[0].read()
    for call in (lambda: xisfconv.convert(mono, tmp_path / "out.fits"), lambda: xisfconv.rewrite(mono, tmp_path / "out.xisf"),
                 lambda: xisfconv.rewrite(mono, tmp_path / "out.xish")):
        with pytest.warns(xisfconv.XisfconvWarning), pytest.raises(xisfconv.NotAllowedError):
            call()
    assert sorted(os.listdir(tmp_path)) == ["id_ed25519", "m42.xisf"]
    with pytest.warns(xisfconv.XisfconvWarning, match="names data in other files"):
        assert same(xisfconv.read(mono, external_files="anywhere"), pixels)


# --- what a header is followed to -------------------------------------------------------------

def test_a_header_is_followed_to_its_own_directory_only(tmp_path):
    inside = tmp_path / "unit"
    below = inside / "below"
    outside = tmp_path / "elsewhere"
    for directory in (inside, below, outside):
        directory.mkdir()
    unit, pixels = gray_unit(inside, "in.xish", "path(@header_dir/below/made.dat)")
    raw = pixels.astype("<u2").tobytes()
    (below / "made.dat").write_bytes(raw)
    (outside / "made.dat").write_bytes(raw)
    assert same(xisfconv.read(unit), pixels)
    assert same(xisfconv.read(unit, external_files="header-dir"), pixels)

    absolute = gray_unit(inside, "absolute.xish", "path(%s)" % slashes(outside / "made.dat"))[0]
    climbing = gray_unit(inside, "climbing.xish", "path(@header_dir/../elsewhere/made.dat)")[0]
    returning = gray_unit(inside, "returning.xish", "path(@header_dir/below/../../unit/below/made.dat)")[0]
    url = gray_unit(inside, "url.xish", "url(file://%s%s)" % ("" if slashes(outside).startswith("/") else "/",
                                                             slashes(outside / "made.dat")))[0]
    for path, words in ((absolute, "by an absolute path"), (climbing, "leads out of the directory of the header"),
                        (returning, "leads out of the directory of the header"), (url, "by a URL")):
        with pytest.raises(xisfconv.NotAllowedError, match=words) as caught:
            xisfconv.read(path)
        assert isinstance(caught.value, PermissionError) and isinstance(caught.value, xisfconv.Error)
        assert 'external_files="anywhere"' in str(caught.value) and "--external-files" not in str(caught.value)
        assert os.path.basename(path) in str(caught.value)
        with pytest.raises(xisfconv.NotAllowedError):
            xisfconv.read_image(path)
        with pytest.raises(xisfconv.NotAllowedError):
            xisfconv.convert(path, tmp_path / "out.fits")
        with pytest.raises(xisfconv.NotAllowedError):
            xisfconv.rewrite(path, tmp_path / "out.xisf")
        with pytest.raises(xisfconv.NotAllowedError):
            xisfconv.rewrite_in_place(path, codec="zlib")
        assert not os.path.exists(tmp_path / "out.fits") and not os.path.exists(tmp_path / "out.xisf")
        assert not [name for name in os.listdir(inside) + os.listdir(tmp_path) if name.endswith(".part")]
        report = xisfconv.verify(path)
        assert report.verdict == "not fully checked" and len(report.not_checked) == 1 and not report.problems
        # ... unless that is asked for, call by call
        assert same(xisfconv.read(path, external_files="anywhere"), pixels)
        assert same(xisfconv.read_image(path, external_files="anywhere").data, pixels)
        assert xisfconv.verify(path, external_files="anywhere").verdict != "failed"
        xisfconv.convert(path, tmp_path / "out.tif", external_files="anywhere", overwrite=True)
        xisfconv.rewrite(path, tmp_path / "packed.xisf", external_files="anywhere", overwrite=True)
        assert same(xisfconv.read(tmp_path / "packed.xisf"), pixels)
        with pytest.raises(xisfconv.NotAllowedError):       # the call before does not decide for this one
            xisfconv.read(path)
        with xisfconv.open(path) as file:                   # the header itself is read, and says where it leads
            assert len(file.external_files) == 1 and file.unit_size == file.size
            with pytest.raises(xisfconv.NotAllowedError):
                file[0].read()
    # no file but the header
    for path in (unit, absolute):
        with pytest.raises(xisfconv.NotAllowedError, match="no file but the header"):
            xisfconv.read(path, external_files="none")
        assert xisfconv.verify(path, external_files="none").verdict == "not fully checked"
    with pytest.raises(ValueError, match="external_files"):
        xisfconv.read(unit, external_files="everywhere")
    with pytest.raises(TypeError, match="external_files"):
        xisfconv.open(unit, external_files=True)
    # nothing comes from a network, whatever is allowed
    network = gray_unit(inside, "network.xish", "url(https://example.com/made.dat)")[0]
    for allowed in (None, "anywhere"):
        with pytest.raises(xisfconv.UnsupportedError, match="nothing is fetched from a network"):
            xisfconv.read(network, external_files=allowed)
    with xisfconv.open(network) as file:
        assert file.external_files == ["https://example.com/made.dat"]


def test_an_open_file_keeps_what_it_was_opened_with(tmp_path):
    inside = tmp_path / "unit"
    outside = tmp_path / "elsewhere"
    inside.mkdir()
    outside.mkdir()
    unit, pixels = gray_unit(inside, "absolute.xish", "path(%s)" % slashes(outside / "made.dat"))
    (outside / "made.dat").write_bytes(pixels.astype("<u2").tobytes())
    other = tmp_path / "other.xisf"
    xisfconv.write(other, pixels)
    with xisfconv.open(unit, external_files="anywhere") as allowed, xisfconv.open(unit) as refused:
        assert same(xisfconv.read(other), pixels)             # (a call with the default in between)
        with pytest.raises(xisfconv.NotAllowedError):
            xisfconv.read(unit)
        assert same(allowed[0].read(), pixels)
        with pytest.raises(xisfconv.NotAllowedError):
            refused[0].read()
        assert allowed.unit_size == allowed.size + 96 and refused.unit_size == refused.size
    # threads do not decide for each other
    results = {}

    def reader(name, external_files):
        try:
            for _ in range(20):
                xisfconv.read(unit, external_files=external_files)
            results[name] = "read"
        except xisfconv.NotAllowedError:
            results[name] = "refused"

    threads = [threading.Thread(target=reader, args=("anywhere", "anywhere")), threading.Thread(target=reader, args=("default", None))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert results == {"anywhere": "read", "default": "refused"}


def test_links_that_lead_out_of_the_directory(tmp_path):
    inside = tmp_path / "unit"
    outside = tmp_path / "elsewhere"
    inside.mkdir()
    outside.mkdir()
    unit, pixels = gray_unit(inside, "link.xish", "path(@header_dir/link.dat)")
    (outside / "made.dat").write_bytes(pixels.astype("<u2").tobytes())
    try:
        os.symlink(outside / "made.dat", inside / "link.dat")
    except (OSError, NotImplementedError, AttributeError):
        pytest.skip("no symbolic links here")
    try:
        with pytest.raises(xisfconv.NotAllowedError, match="behind a symbolic link that does not lead to a file in the directory") as caught:
            xisfconv.read(unit)
    except OSError:
        if sys.platform != "win32":
            raise
        pytest.skip("this build of the library does not follow symbolic links (MinGW)")
    assert "elsewhere" not in str(caught.value)          # where the link leads is not told
    assert same(xisfconv.read(unit, external_files="anywhere"), pixels)
    # ... nor whether there is something where it leads
    os.symlink(outside / "nothing there", inside / "dangling.dat")
    dangling = gray_unit(inside, "dangling.xish", "path(@header_dir/dangling.dat)")[0]
    with pytest.raises(xisfconv.NotAllowedError) as other:
        xisfconv.read(dangling)
    assert str(other.value).replace("dangling", "link") == str(caught.value)
    # a link that stays inside is followed, and so is a header that is itself reached through a link:
    # its directory is the one it is named in
    (inside / "made.dat").write_bytes(pixels.astype("<u2").tobytes())
    os.remove(inside / "link.dat")
    os.symlink("made.dat", inside / "link.dat")
    assert same(xisfconv.read(unit), pixels)
    # a directory that is a link out of the directory
    os.symlink(outside, inside / "sub", target_is_directory=True)
    through = gray_unit(inside, "through.xish", "path(@header_dir/sub/made.dat)")[0]
    with pytest.raises(xisfconv.NotAllowedError, match="behind a symbolic link"):
        xisfconv.read(through)


def test_a_property_in_a_file_that_is_not_read_is_left_out(tmp_path):
    inside = tmp_path / "unit"
    outside = tmp_path / "elsewhere"
    inside.mkdir()
    outside.mkdir()
    vector = np.arange(50, dtype="<f8")
    (outside / "vector.dat").write_bytes(vector.tobytes())
    more = '<Property id="Test:Vector" type="F64Vector" length="50" location="path(%s)"/>' \
           '<Property id="Test:Number" type="Int32" value="42"/>' % slashes(outside / "vector.dat")
    unit, pixels = gray_unit(inside, more=more)
    (inside / "made.dat").write_bytes(pixels.astype("<u2").tobytes())
    with xisfconv.open(unit) as file:
        assert len(file.external_files) == 2
        assert same(file[0].read(), pixels)
        # asked for by its name, the property is what is not allowed; the others are there
        assert file[0].properties["Test:Number"] == 42 and "Test:Vector" in file[0].properties
        with pytest.raises(xisfconv.NotAllowedError, match="Test:Vector"):
            file[0].properties["Test:Vector"]
    # an image that is read with what belongs to it has that property without a value, and says why
    with pytest.warns(xisfconv.XisfconvWarning, match="Test:Vector") as caught:
        image = xisfconv.read_image(unit)
    assert same(image.data, pixels) and image.properties["Test:Number"] == 42 and image.properties["Test:Vector"] is None
    assert 'external_files="anywhere"' in str(caught[0].message)
    image = xisfconv.read_image(unit, external_files="anywhere")
    assert np.array_equal(image.properties["Test:Vector"], vector)
    with xisfconv.open(unit, external_files="anywhere") as file:
        assert np.array_equal(file[0].properties["Test:Vector"], vector)
        assert file.unit_size == file.size + 96 + 400
    # a conversion goes on without it, and says so; the file it was in is not copied anywhere
    with pytest.warns(xisfconv.XisfconvWarning, match="Test:Vector is left out"):
        xisfconv.convert(unit, tmp_path / "out.asdf")
    assert vector.tobytes() not in open(tmp_path / "out.asdf", "rb").read()
    xisfconv.convert(unit, tmp_path / "all.asdf", external_files="anywhere")
    assert vector.tobytes() in open(tmp_path / "all.asdf", "rb").read()


# --- data blocks files that are damaged -------------------------------------------------------

def test_damaged_data_blocks_files(tmp_path):
    pixels = sample("uint16", (6, 8), seed=5)
    raw = pixels.astype("<u2").tobytes()
    unit = gray_unit(tmp_path, "d.xish", "path(@header_dir/d.xisb):0x10")[0]
    data = tmp_path / "d.xisb"

    def damaged(change, error, words):
        write_blocks_file(data, {0x10: raw})
        blob = bytearray(data.read_bytes())
        blob = change(blob)
        data.write_bytes(bytes(blob))
        with pytest.raises(error, match=words):
            xisfconv.read(unit)
        report = xisfconv.verify(unit)
        assert report.failed and re.search(words, " ".join(report.problems))

    def poke(position, fmt, *values):
        def change(blob):
            struct.pack_into(fmt, blob, position, *values)
            return blob
        return change

    write_blocks_file(data, {0x10: raw})
    assert same(xisfconv.read(unit), pixels)
    first = 16 + 16
    damaged(lambda blob: blob[:-10], xisfconv.FormatError, "lies beyond the end of the file")
    damaged(lambda blob: blob[:20], xisfconv.FormatError, "beyond the end of the file")
    damaged(lambda blob: b"XISF0100" + blob[8:], xisfconv.FormatError, "is not an XISF data blocks file")
    damaged(poke(16 + 8, "<Q", 16), xisfconv.FormatError, "runs in a circle")
    damaged(poke(16 + 8, "<Q", 1 << 60), xisfconv.FormatError, "beyond the end of the file")
    damaged(poke(16, "<I", 0xFFFFFFFF), xisfconv.FormatError, "more than the file has room for")
    damaged(poke(first + 8, "<Q", (1 << 64) - 8), xisfconv.FormatError, "lies beyond the end of the file")
    damaged(poke(first + 16, "<Q", (1 << 64) - 1), xisfconv.FormatError, "lies beyond the end of the file")
    damaged(poke(first + 8, "<QQ", 0, 0), xisfconv.FormatError, "free index element")
    damaged(poke(first, "<Q", 0x11), xisfconv.FormatError, "has no block 0x0000000000000010")
    damaged(poke(first + 16, "<Q", 90), xisfconv.FormatError, "geometry requires 96")
    data.write_bytes(b"")
    with pytest.raises(xisfconv.FormatError, match="too short to be an XISF data blocks file"):
        xisfconv.read(unit)
    os.remove(data)
    os.mkdir(data)
    with pytest.raises(OSError, match="not a regular file"):
        xisfconv.read(unit)


# --- the interface of the xisf package --------------------------------------------------------

def test_the_xisf_interface_reads_and_writes_units(tmp_path):
    from xisfconv.xisf import XISF

    data = sample("uint16", (9, 14, 1))
    path = str(tmp_path / "frame.xish")
    written, codec = XISF.write(path, data, creator_app="test", codec="zlib", shuffle=True,
                                image_metadata={"FITSKeywords": {"OBJECT": [{"value": "M 42", "comment": ""}]},
                                                "XISFProperties": {"Test:Vector": {"id": "Test:Vector", "type": "F64Vector",
                                                                                   "value": np.arange(400.0)}}})
    assert written == os.path.getsize(path) + os.path.getsize(tmp_path / "frame.xisb")
    frame = XISF(path)
    about = frame.get_images_metadata()[0]
    kind, where, identifier = about["location"]
    assert kind == "path" and where == "@header_dir/frame.xisb" and isinstance(identifier, int) and identifier > 0
    stored, _, _ = blocks_file(tmp_path / "frame.xisb")
    assert identifier in stored
    assert same(frame.read_image(0), data)
    assert about["FITSKeywords"]["OBJECT"][0]["value"] == "M 42"
    vector = about["XISFProperties"]["Test:Vector"]
    assert np.array_equal(vector["value"], np.arange(400.0)) and vector["location"][0] == "path" and vector["location"][2] in stored
    assert same(XISF.read(path), data)
    # written again over itself, both files, and to a monolithic file with everything it had
    XISF.write(path, data + 1, image_metadata=about, xisf_metadata=frame.get_file_metadata())
    assert same(XISF.read(path), data + 1) and sorted(os.listdir(tmp_path)) == ["frame.xisb", "frame.xish"]
    XISF.write(str(tmp_path / "mono.xisf"), data, image_metadata=about)
    mono = XISF(str(tmp_path / "mono.xisf")).get_images_metadata()[0]
    assert mono["location"][0] == "attachment" and np.array_equal(mono["XISFProperties"]["Test:Vector"]["value"], np.arange(400.0))
    # a whole file, without an identifier; and a header that leads elsewhere
    unit, pixels = gray_unit(tmp_path)
    (tmp_path / "made.dat").write_bytes(pixels.astype("<u2").tobytes())
    hand = XISF(unit)
    assert hand.get_images_metadata()[0]["location"] == ("path", "@header_dir/made.dat", None)
    assert same(hand.read_image(0)[:, :, 0], pixels)
    elsewhere = gray_unit(tmp_path, "abs.xish", "path(%s)" % slashes(tmp_path / "made.dat"))[0]
    with pytest.raises(xisfconv.NotAllowedError):
        XISF(elsewhere).read_image(0)


# --- astropy ------------------------------------------------------------------------------------

def test_astropy_reads_and_writes_units(tmp_path, monkeypatch):
    pytest.importorskip("astropy")
    import io

    from astropy.nddata import CCDData

    import xisfconv.astropy

    data = sample("uint16", (10, 12))
    ccd = CCDData(data, unit="adu")
    ccd.header["OBJECT"] = "M 13"
    path = str(tmp_path / "ccd.xish")
    ccd.write(path)                                           # by the name: XISF, a distributed unit
    assert sorted(os.listdir(tmp_path)) == ["ccd.xisb", "ccd.xish"]
    back = CCDData.read(path)                                 # recognized by name and beginning
    assert np.array_equal(back.data, data) and back.header["OBJECT"] == "M 13" and back.unit == "adu"
    with open(path, "rb") as stream:                          # an open file with a name: read where it is
        assert np.array_equal(CCDData.read(stream, format="xisf").data, data)
    hdulist = xisfconv.astropy.read_hdulist(path)
    assert np.array_equal(hdulist[0].data, data)
    xisfconv.astropy.write_hdulist(hdulist, str(tmp_path / "copy.xish"))
    assert np.array_equal(xisfconv.astropy.read_hdulist(str(tmp_path / "copy.xish"))[0].data, data)
    # a header that comes from nowhere has no directory to look in: not the one for temporary files
    stream = io.BytesIO(open(path, "rb").read())
    with pytest.raises(xisfconv.NotAllowedError, match=r"ccd\.xisb.*by the name of its header file"):
        xisfconv.astropy.read_hdulist(stream)
    # a file that astropy fetched for the caller is in a directory nobody chose, under a name that
    # says nothing: it is not followed to the files beside it
    fetched = tmp_path / "astropy-download-1234-abcdef"
    fetched.write_bytes(open(path, "rb").read())
    with open(fetched, "rb") as stream, pytest.raises(xisfconv.NotAllowedError, match="does not have the name of one"):
        xisfconv.astropy.read_hdulist(stream)
    # ... and the real thing: a URL, which astropy fetches into the directory for temporary files
    # (one of this test's own, with what the header would find beside itself there)
    import pathlib
    import tempfile
    temporary = tmp_path / "temporary files"
    temporary.mkdir()
    (temporary / "ccd.xisb").write_bytes(open(tmp_path / "ccd.xisb", "rb").read())
    monkeypatch.setattr(tempfile, "tempdir", str(temporary))
    if sys.platform != "win32":      # (how astropy keeps the file it fetched is its own matter there)
        with pytest.raises(xisfconv.NotAllowedError):
            CCDData.read(pathlib.Path(path).as_uri(), cache=False)
    # a file that is not XML is not taken for XISF by its name
    (tmp_path / "other.xish").write_text("no XML at all")
    from astropy.io.registry import IORegistryError
    with pytest.raises(IORegistryError, match="(?i)format could not be identified"):
        CCDData.read(str(tmp_path / "other.xish"))


# --- the command line tool ----------------------------------------------------------------------

def test_the_tool_and_the_package_read_each_other(tmp_path, tool):
    data = sample("float32", (8, 9, 3))
    mine = tmp_path / "mine.xish"
    xisfconv.write(mine, data, codec="zlib", checksum="sha1")
    done = subprocess.run([tool, "--verify", str(mine)], capture_output=True, text=True)
    assert done.returncode == 0 and ": OK" in done.stdout and "distributed unit" in done.stdout
    done = subprocess.run([tool, str(mine), "-o", str(tmp_path / "theirs.xisf")], capture_output=True, text=True)
    assert done.returncode == 0 and same(xisfconv.read(tmp_path / "theirs.xisf"), data)
    done = subprocess.run([tool, str(tmp_path / "theirs.xisf"), "-t", "xish", "-d", str(tmp_path), "--codec", "none"],
                          capture_output=True, text=True)
    assert done.returncode == 0 and same(xisfconv.read(tmp_path / "theirs.xish"), data)
    assert same(pixels_of(str(tmp_path / "theirs.xish")), np.moveaxis(data, 2, 0))
    # the tool has the same default, and the same word for more
    unit, pixels = gray_unit(tmp_path, "abs.xish", "path(%s)" % slashes(tmp_path / "elsewhere.dat"))
    (tmp_path / "elsewhere.dat").write_bytes(pixels.astype("<u2").tobytes())
    done = subprocess.run([tool, unit, "-o", str(tmp_path / "abs.fits")], capture_output=True, text=True)
    assert done.returncode == 1 and "--external-files anywhere" in done.stderr and not os.path.exists(tmp_path / "abs.fits")
    done = subprocess.run([tool, unit, "-o", str(tmp_path / "abs.fits"), "--external-files", "anywhere"], capture_output=True, text=True)
    assert done.returncode == 0 and os.path.exists(tmp_path / "abs.fits")


@pytest.mark.parametrize("name", ["amp&er", "par(en)s", "a&amp;b", "caf\u00e9 au lait", 'quo"te', "lt<gt>", "tab\there"])
def test_names_a_header_has_to_write_with_care(tmp_path, name):
    import xml.etree.ElementTree as ET

    if sys.platform == "win32" and set(name) & set('"<>\t'):
        pytest.skip("no such file names on Windows")
    data = sample("uint16", (5, 7))
    unit = tmp_path / (name + ".xish")
    xisfconv.write(unit, data)
    assert sorted(os.listdir(tmp_path)) == [name + ".xisb", name + ".xish"]
    assert same(xisfconv.read(unit), data) and xisfconv.verify(unit).ok
    # the header is XML for any reader, and names the file as the specification says
    location = ET.parse(unit).getroot()[0].get("location")
    assert location.startswith("path(@header_dir/%s.xisb):0x" % name.replace("(", "\\(").replace(")", "\\)"))
    xisfconv.rewrite(unit, tmp_path / "packed.xisf")
    xisfconv.rewrite(tmp_path / "packed.xisf", unit, overwrite=True, codec="zlib")
    assert same(xisfconv.read(unit), data) and ET.parse(unit).getroot()[0].get("location").startswith("path(@header_dir/")


def test_the_data_of_the_input_is_no_output(tmp_path):
    data = sample("uint16", (5, 7))
    unit = tmp_path / "in.xish"
    xisfconv.write(unit, data, checksum="sha1")
    before = (tmp_path / "in.xisb").read_bytes()
    with pytest.raises(ValueError, match="is a file the input reads its data from"):
        xisfconv.convert(unit, tmp_path / "in.xisb", format="fits", overwrite=True)
    with pytest.raises(ValueError, match="is a file the input reads its data from"):
        xisfconv.rewrite(unit, tmp_path / "in.xisb")                      # an argument that is wrong, with or without overwrite
    assert (tmp_path / "in.xisb").read_bytes() == before and same(xisfconv.read(unit), data)
    # a data blocks file that another header reads too is not replaced in place
    other = tmp_path / "other.xish"
    xisfconv.write(other, data + 1)
    stored, _, positions = blocks_file(tmp_path / "other.xisb")
    mine, _, _ = blocks_file(tmp_path / "in.xisb")
    write_blocks_file(tmp_path / "in.xisb", {**mine, **stored})           # both blocks in one file
    other.write_text(header_of(other).replace("other.xisb", "in.xisb"))
    os.remove(tmp_path / "other.xisb")
    assert same(xisfconv.read(other), data + 1) and same(xisfconv.read(unit), data)
    with pytest.raises(FileExistsError, match=r"also holds 1 block that this header does not name.*overwrite=True"):
        xisfconv.rewrite_in_place(unit, checksum="sha256")
    assert same(xisfconv.read(other), data + 1)
    assert xisfconv.rewrite_in_place(unit, checksum="sha256", overwrite=True).changed
    with pytest.raises(xisfconv.FormatError, match="has no block"):
        xisfconv.read(other)
