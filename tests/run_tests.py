#!/usr/bin/env python3
"""End-to-end tests for xisfconv.

SPDX-License-Identifier: GPL-3.0-or-later

XISF inputs come from two independent sources:
  * the `xisf` PyPI package (sergio-dr/xisf) for all codecs with/without byte shuffling;
  * a small hand-written XISF encoder below for features that package doesn't emit
    (Normal pixel storage, big-endian data, inline/embedded blocks, subblocks, checksums,
    CFA, ICC profile, multiple images, odd shuffle remainders, tricky FITS keywords).

Outputs are checked with astropy (FITS) and with libtiff's tiffcp + tifffile (TIFF).

Requirements: pip install numpy astropy tifffile xisf lz4 zstandard
Optional:     libtiff tools (tiffcp) and fitsverify, used as extra independent checkers
Usage: python3 tests/run_tests.py path/to/xisfconv
"""
import base64
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zlib

import numpy as np
import tifffile
from astropy.io import fits
from xisf import XISF

try:
    import lz4.block
    import zstandard
except ImportError:  # pragma: no cover
    lz4 = zstandard = None

EXE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/xisfconv")
TMP = tempfile.mkdtemp(prefix="xisfconv-test-")
HAVE_TIFFCP = shutil.which("tiffcp") is not None
HAVE_FITSVERIFY = shutil.which("fitsverify") is not None
failures = []
passed = 0


def check(cond, msg):
    global passed
    if cond:
        passed += 1
    else:
        failures.append(msg)
        print("FAIL:", msg)


def run(*args, expect_ok=True):
    r = subprocess.run([EXE, *args], capture_output=True, text=True)
    if expect_ok and r.returncode != 0:
        raise RuntimeError(f"xisfconv {' '.join(args)} failed:\n{r.stdout}{r.stderr}")
    return r


def test_image(dtype, h, w, c, seed):
    """Compressible test image (smooth ramp + noise) as HxWxC."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w]
    base = (x * 3 + y * 7)[..., None] + np.arange(c)[None, None, :] * 11
    if np.issubdtype(dtype, np.floating):
        # quantized to 1/1024 steps so the data compresses well, plus a few non-trivial values
        a = (base % 1000) / 1024.0
        a[0, 0, 0] = np.pi / 4
        a[-1, -1, -1] = 1 / 3
        return a.astype(dtype)
    info = np.iinfo(dtype)
    noise = rng.integers(0, 4 if np.dtype(dtype).itemsize > 1 else 1, (h, w, c))
    a = (base * 37 + noise) % (min(info.max, 2**40) + 1)
    a = a.astype(np.uint64)
    a[0, 0, 0] = info.max  # exercise the extremes
    a[0, 1 % w, 0] = 0
    return a.astype(dtype)


# ---------------------------------------------------------------- output verification

def fits_planes(path, hdu=0, raw=False):
    """Returns (C,H,W) data in XISF (top-down) row order unless raw=True, plus the header."""
    if HAVE_FITSVERIFY:
        r = subprocess.run(["fitsverify", "-q", path], capture_output=True, text=True)
        check("verification OK" in r.stdout, f"fitsverify {os.path.basename(path)}: {r.stdout.strip()}")
    with fits.open(path) as hdul:
        hdul.verify("exception")
        d = hdul[hdu].data
        hdr = hdul[hdu].header
        d = np.array(d)
    if d.ndim == 2:
        d = d[None, ...]
    if not raw and hdr.get("ROWORDER") == "BOTTOM-UP":
        d = d[:, ::-1, :]
    return d, hdr  # (C, H, W)


def tiff_array(path):
    """Decode via libtiff (tiffcp -> uncompressed) when available, then read with tifffile."""
    src = path
    if HAVE_TIFFCP:
        plain = path + ".plain.tif"
        subprocess.run(["tiffcp", "-c", "none", path, plain], check=True, capture_output=True)
        src = plain
    with tifffile.TiffFile(src) as t:
        pages = [p.asarray() for p in t.pages]
    return pages


def as_planes(a):  # HxWxC -> CxHxW
    return np.transpose(a, (2, 0, 1))


def compare(label, got, expected):
    ok = got.shape == expected.shape and np.array_equal(got.astype(expected.dtype), expected)
    if not ok and got.shape == expected.shape:
        diff = np.abs(got.astype(np.float64) - expected.astype(np.float64)).max()
        label += f" (max diff {diff})"
    check(ok, f"{label}: shape {got.shape} vs {expected.shape}")


def roundtrip(label, path, expected_hwc, fits_check=True, tiff_check=True, extra=()):
    exp = as_planes(expected_hwc)
    if fits_check:
        out = os.path.join(TMP, os.path.basename(path) + ".fits")
        run(path, "-o", out, "-f", "-q", *extra)
        got, _ = fits_planes(out)
        compare(f"{label} -> FITS", got, exp)
    if tiff_check:
        for comp in ([], ["-c"]):
            out = os.path.join(TMP, os.path.basename(path) + (".c" if comp else "") + ".tif")
            run(path, "-o", out, "-f", "-q", *comp, *extra)
            page = tiff_array(out)[0]
            if page.ndim == 2:
                page = page[..., None]
            compare(f"{label} -> TIFF{' deflate' if comp else ''}", as_planes(page), exp)


# ---------------------------------------------------------------- hand-written XISF encoder

def shuffle_bytes(data, item):
    n = len(data) // item
    body = np.frombuffer(data[: n * item], np.uint8).reshape(n, item).T.tobytes()
    return body + data[n * item:]


def compress(data, codec, item=None):
    if item:
        data = shuffle_bytes(data, item)
    if codec == "zlib":
        return zlib.compress(data)
    if codec in ("lz4", "lz4hc"):
        mode = "high_compression" if codec == "lz4hc" else "default"
        return lz4.block.compress(data, mode=mode, store_size=False)
    if codec == "zstd":
        return zstandard.compress(data)
    raise ValueError(codec)


class Block:
    """A data block attached to an element; returns XML attributes and (for attachments) bytes."""

    def __init__(self, raw, codec=None, shuffle_item=None, subblocks=None, checksum=None, location="attachment",
                 encoding="base64", corrupt_checksum=False):
        self.raw, self.codec, self.item = raw, codec, shuffle_item
        self.subblocks, self.checksum, self.location, self.encoding = subblocks, checksum, location, encoding
        self.attrs, self.text, self.payload = {}, "", b""
        stored = raw
        if codec:
            name = codec + ("+sh" if shuffle_item else "")
            comp = f"{name}:{len(raw)}" + (f":{shuffle_item}" if shuffle_item else "")
            if subblocks:
                data = shuffle_bytes(raw, shuffle_item) if shuffle_item else raw
                parts, pairs, step = [], [], -(-len(data) // subblocks)
                for i in range(0, len(data), step):
                    piece = data[i:i + step]
                    cp = compress(piece, codec)
                    parts.append(cp)
                    pairs.append(f"{len(cp)},{len(piece)}")
                stored = b"".join(parts)
                self.attrs["subblocks"] = ":".join(pairs)
            else:
                stored = compress(raw, codec, shuffle_item)
            self.attrs["compression"] = comp
        if checksum:
            digest = hashlib.new(checksum.replace("-", ""), stored).hexdigest()
            if corrupt_checksum:
                digest = ("0" if digest[0] != "0" else "1") + digest[1:]
            self.attrs["checksum"] = f"{checksum}:{digest}"
        self.stored = stored
        if location == "attachment":
            self.payload = stored
        else:
            txt = base64.b64encode(stored).decode() if encoding == "base64" else stored.hex()
            if encoding == "base64":  # wrap lines, as the spec examples do
                txt = "\n".join(txt[i:i + 76] for i in range(0, len(txt), 76))
            self.text = txt


def write_xisf(path, images, file_props=""):
    """images: list of dicts with keys block (Block), attrs (dict), children (xml string)."""
    # Two passes: header size determines attachment positions.
    def build(positions):
        parts = ['<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf"'
                 ' xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">']
        for img, pos in zip(images, positions):
            b = img["block"]
            attrs = dict(img["attrs"])
            inner = img.get("children", "")
            if b.location == "attachment":
                attrs["location"] = f"attachment:{pos}:{len(b.payload)}"
                attrs.update(b.attrs)
            elif b.location == "inline":
                attrs["location"] = f"inline:{b.encoding}"
                attrs.update(b.attrs)
                inner = b.text + inner
            else:  # embedded: codec/checksum attributes go on <Data>
                attrs["location"] = "embedded"
                da = " ".join(f'{k}="{v}"' for k, v in b.attrs.items())
                inner = f'<Data encoding="{b.encoding}" {da}>{b.text}</Data>' + inner
            a = " ".join(f'{k}="{v}"' for k, v in attrs.items())
            parts.append(f"<Image {a}>{inner}</Image>")
        parts.append(f"<Metadata>{file_props}"
                     '<Property id="XISF:CreatorApplication" type="String">xisfconv tests</Property></Metadata>')
        parts.append("</xisf>")
        return "".join(parts).encode()

    hdr = build([0] * len(images))
    for _ in range(3):
        base = 16 + len(hdr)
        base = (base + 4095) // 4096 * 4096
        positions, p = [], base
        for img in images:
            positions.append(p)
            p += len(img["block"].payload)
        hdr = build(positions)
    with open(path, "wb") as f:
        f.write(b"XISF0100" + len(hdr).to_bytes(4, "little") + b"\0\0\0\0" + hdr)
        f.write(b"\0" * (positions[0] - f.tell()))
        for img in images:
            f.write(img["block"].payload)


SF = {np.uint8: "UInt8", np.uint16: "UInt16", np.uint32: "UInt32", np.uint64: "UInt64",
      np.float32: "Float32", np.float64: "Float64"}


def image_entry(a_hwc, planar=True, big=False, color=None, children="", **block_kw):
    h, w, c = a_hwc.shape
    arr = as_planes(a_hwc) if planar else a_hwc
    dt = a_hwc.dtype.newbyteorder(">" if big else "<")
    raw = np.ascontiguousarray(arr).astype(dt).tobytes()
    attrs = {"geometry": f"{w}:{h}:{c}", "sampleFormat": SF[a_hwc.dtype.type],
             "colorSpace": color or ("RGB" if c == 3 else "Gray")}
    if np.issubdtype(a_hwc.dtype, np.floating):
        attrs["bounds"] = "0:1"
    if not planar:
        attrs["pixelStorage"] = "Normal"
    if big:
        attrs["byteOrder"] = "big"
    return {"block": Block(raw, **block_kw), "attrs": attrs, "children": children}


# ---------------------------------------------------------------- tests

def test_python_xisf_codecs():
    for dtype in (np.uint8, np.uint16, np.uint32, np.float32, np.float64):
        for c in (1, 3):
            a = test_image(dtype, 37, 53, c, seed=c)
            for codec in (None, "zlib", "lz4", "lz4hc", "zstd"):
                for sh in ((False,) if codec is None else (False, True)):
                    name = f"py_{np.dtype(dtype).name}_{c}ch_{codec}{'_sh' if sh else ''}.xisf"
                    path = os.path.join(TMP, name)
                    used = XISF.write(path, a, codec=codec, shuffle=sh)[1]
                    want = codec and codec + ("+sh" if sh else "")
                    check(used == want, f"{name}: xisf package wrote codec {used}, wanted {want}")
                    roundtrip(name, path, a)


def test_hand_written():
    rgb16 = test_image(np.uint16, 29, 41, 3, seed=7)
    gray32f = test_image(np.float32, 31, 17, 1, seed=8)
    rgb64f = test_image(np.float64, 13, 11, 3, seed=9)
    cases = {
        "normal_storage": image_entry(rgb16, planar=False),
        "big_endian": image_entry(rgb16, big=True),
        "big_endian_normal_f64": image_entry(rgb64f, planar=False, big=True),
        "inline_base64": image_entry(gray32f, location="inline"),
        "inline_hex": image_entry(gray32f, location="inline", encoding="hex"),
        "embedded_zlib_sha1": image_entry(rgb16, location="embedded", codec="zlib", checksum="sha-1"),
        "subblocks_zlib": image_entry(rgb16, codec="zlib", subblocks=3),
        "subblocks_lz4_shuffle": image_entry(rgb16, codec="lz4", shuffle_item=2, subblocks=4),
        "subblocks_zstd_sha256": image_entry(rgb64f, codec="zstd", shuffle_item=8, subblocks=2, checksum="sha-256"),
        "sha512": image_entry(gray32f, codec="lz4hc", checksum="sha-512"),
        "sha1_uncompressed": image_entry(gray32f, checksum="sha-1"),
        # shuffle item size that doesn't divide the block: trailing bytes stay unshuffled
        "shuffle_remainder": image_entry(test_image(np.uint8, 5, 7, 1, 1), codec="zlib", shuffle_item=4),
        "uint64": image_entry(test_image(np.uint64, 9, 6, 1, 3), codec="zstd", shuffle_item=8),
    }
    for name, entry in cases.items():
        path = os.path.join(TMP, f"hw_{name}.xisf")
        write_xisf(path, [entry])
        # recover the source array from the raw bytes we encoded
        attrs = entry["attrs"]
        W, H, C = [int(v) for v in attrs["geometry"].split(":")]
        dt = np.dtype({v: k for k, v in SF.items()}[attrs["sampleFormat"]]).newbyteorder(
            ">" if attrs.get("byteOrder") == "big" else "<")
        flat = np.frombuffer(entry["block"].raw, dt)
        if attrs.get("pixelStorage") == "Normal":
            hwc = flat.reshape(H, W, C)
        else:
            hwc = np.transpose(flat.reshape(C, H, W), (1, 2, 0))
        hwc = hwc.astype(hwc.dtype.newbyteorder("="))
        roundtrip(f"hw_{name}", path, hwc)


def test_checksum_mismatch():
    a = test_image(np.uint16, 8, 8, 1, 2)
    path = os.path.join(TMP, "bad_checksum.xisf")
    write_xisf(path, [image_entry(a, codec="zlib", checksum="sha-1", corrupt_checksum=True)])
    r = run(path, "-o", os.path.join(TMP, "bad.fits"), "-f", expect_ok=False)
    check(r.returncode != 0 and "checksum mismatch" in r.stderr, "corrupt checksum must be rejected")
    r = run(path, "-o", os.path.join(TMP, "bad.fits"), "-f", "--no-verify", "-q")
    check(r.returncode == 0, "--no-verify should convert despite checksum mismatch")


def test_truncated_and_garbage():
    a = test_image(np.uint16, 64, 64, 1, 2)
    path = os.path.join(TMP, "trunc.xisf")
    write_xisf(path, [image_entry(a)])
    data = open(path, "rb").read()
    open(path, "wb").write(data[:-100])
    r = run(path, "-o", os.path.join(TMP, "trunc.fits"), "-f", expect_ok=False)
    check(r.returncode != 0 and "beyond the end" in r.stderr, "truncated file must be reported")
    check(not os.path.exists(os.path.join(TMP, "trunc.fits.part")), "partial output must be removed")
    garbage = os.path.join(TMP, "garbage.xisf")
    open(garbage, "wb").write(b"XISF0100" + (50).to_bytes(4, "little") + b"\0" * 4 + b"<xisf><Image" + b"x" * 38)
    r = run(garbage, "-o", os.path.join(TMP, "g.fits"), "-f", expect_ok=False)
    check(r.returncode != 0 and "malformed XML" in r.stderr, "garbage header must be rejected cleanly")
    # corrupt lz4 stream must fail cleanly, not crash
    path = os.path.join(TMP, "badlz4.xisf")
    e = image_entry(a, codec="lz4")
    e["block"].payload = bytes(len(e["block"].payload))
    write_xisf(path, [e])
    r = run(path, "-o", os.path.join(TMP, "badlz4.fits"), "-f", expect_ok=False)
    check(r.returncode == 1 and "lz4" in r.stderr, f"corrupt lz4 must fail cleanly: {r.stderr.strip()}")


def test_keywords_and_properties():
    a = test_image(np.uint16, 20, 30, 1, 5)
    kws = "".join([
        '<FITSKeyword name="SIMPLE" value="T" comment="should be dropped"/>',
        '<FITSKeyword name="BZERO" value="32768" comment="should be dropped"/>',
        '<FITSKeyword name="NAXIS1" value="999" comment="should be dropped"/>',
        '<FITSKeyword name="OBJECT" value="\'M 31 &amp; friends\'" comment="target"/>',
        '<FITSKeyword name="OBSERVER" value="\'O\'\'Brien\'" comment="quote inside"/>',
        '<FITSKeyword name="GAIN" value="120" comment="sensor gain"/>',
        '<FITSKeyword name="PSFFLX00" value="1.7870e+04" comment="PixInsight-style lower-case exponent"/>',
        '<FITSKeyword name="XPIXSZ" value="3.76" comment="&#956;m pixel"/>',
        '<FITSKeyword name="HISTORY" value="" comment="calibrated with master dark"/>',
        '<FITSKeyword name="COMMENT" value="" comment="' + "x" * 150 + '"/>',
        '<FITSKeyword name="LONGNAMEKEY" value="1.5" comment="needs HIERARCH"/>',
        '<FITSKeyword name="NOTES" value="\'' + "y" * 90 + '\'" comment="too long"/>',
    ])
    props = "".join([
        '<Property id="Instrument:ExposureTime" type="Float32" value="300"/>',
        '<Property id="Observation:Time:Start" type="TimePoint" value="2024-03-05T21:15:02.5Z"/>',
        '<Property id="Instrument:Telescope:FocalLength" type="Float32" value="0.53"/>',
        '<Property id="Instrument:Camera:Name" type="String">ZWO ASI2600MM Pro</Property>',
        '<Property id="Instrument:Filter:Name" type="String" value="Ha 3nm"/>',
        '<Property id="PCL:Signature" type="I32Vector" length="4" location="inline:hex">01000000020000000300000004000000</Property>',
    ])
    cfa = '<ColorFilterArray pattern="RGGB" width="2" height="2" name="RGGB Bayer"/>'
    entry = image_entry(a, children=kws + props + cfa)
    entry["attrs"]["imageType"] = "Light"
    entry["attrs"]["id"] = "integration"
    path = os.path.join(TMP, "keywords.xisf")
    write_xisf(path, [entry])
    out = os.path.join(TMP, "keywords.fits")
    r = run(path, "-o", out, "-f")
    check("truncated" in r.stderr, "long string keyword should warn about truncation")
    _, hdr = fits_planes(out)
    check(hdr["OBJECT"] == "M 31 & friends", f"OBJECT={hdr.get('OBJECT')!r}")
    check(hdr["OBSERVER"] == "O'Brien", f"OBSERVER={hdr.get('OBSERVER')!r}")
    check(hdr["GAIN"] == 120, "GAIN")
    check(hdr["BZERO"] == 32768 and hdr["NAXIS1"] == 30, "structural keywords must come from the writer")
    check(list(hdr.keys()).count("BZERO") == 1, "BZERO must appear once")
    check("calibrated with master dark" in str(hdr["HISTORY"]), "HISTORY kept")
    check(hdr["LONGNAMEKEY"] == 1.5, "HIERARCH keyword")
    check(hdr["EXPTIME"] == 300.0, "EXPTIME from property")
    check(hdr["DATE-OBS"] == "2024-03-05T21:15:02.5", f"DATE-OBS={hdr.get('DATE-OBS')!r}")
    check(abs(hdr["FOCALLEN"] - 530.0) < 1e-6, "FOCALLEN converted m -> mm")
    check(hdr["INSTRUME"] == "ZWO ASI2600MM Pro", "INSTRUME from String property element text")
    check(hdr["FILTER"] == "Ha 3nm", "FILTER")
    check(hdr["XPIXSZ"] == 3.76, "existing XPIXSZ keyword preserved")
    check(hdr["BAYERPAT"] == "GBRG", "BAYERPAT from ColorFilterArray, flipped with the rows (even height)")
    check(hdr["IMAGETYP"] == "Light", "IMAGETYP")
    check(hdr["EXTNAME"] == "integration", "EXTNAME from image id")
    check(hdr["ROWORDER"] == "BOTTOM-UP", "ROWORDER default")
    check(hdr["PSFFLX00"] == 17870.0, "lower-case exponent value")
    check(len(hdr["NOTES"]) < 90 and hdr["NOTES"].startswith("yyy"), "long string truncated")

    # --no-property-keywords
    run(path, "-o", out, "-f", "-q", "--no-property-keywords")
    _, hdr = fits_planes(out)
    check("EXPTIME" not in hdr and "BAYERPAT" not in hdr, "--no-property-keywords")

    # default bottom-up: stored rows reversed relative to XISF
    got, hdr = fits_planes(out, raw=True)
    compare("bottom-up rows", got, as_planes(a[::-1]))
    # --top-down keeps XISF order and the CFA pattern as is
    run(path, "-o", out, "-f", "-q", "--top-down")
    got, hdr = fits_planes(out, raw=True)
    compare("top-down rows", got, as_planes(a))
    check(hdr["ROWORDER"] == "TOP-DOWN", "ROWORDER top-down")
    check(hdr["BAYERPAT"] == "RGGB", f"BAYERPAT top-down: {hdr.get('BAYERPAT')}")

    # --info lists keywords and properties
    r = run(path, "--info")
    check("Instrument:ExposureTime" in r.stdout and "OBJECT" in r.stdout and "RGGB" in r.stdout, "--info output")


def test_multi_image_icc_resolution():
    a = test_image(np.uint16, 16, 24, 3, 1)
    b = test_image(np.float32, 10, 12, 1, 2)
    icc = bytes(range(256)) * 2
    icc_xml = f'<ICCProfile location="inline:base64">{base64.b64encode(icc).decode()}</ICCProfile>'
    res = '<Resolution horizontal="300" vertical="300" unit="inch"/>'
    e1 = image_entry(a, codec="zlib", children=icc_xml + res)
    e1["attrs"]["id"] = "main"
    e2 = image_entry(b, codec="lz4")
    e2["attrs"]["id"] = "mask"
    path = os.path.join(TMP, "multi.xisf")
    write_xisf(path, [e1, e2])

    out = os.path.join(TMP, "multi.fits")
    run(path, "-o", out, "-f", "-q")
    with fits.open(out) as hdul:
        hdul.verify("exception")
        check(len(hdul) == 2, "two HDUs")
        compare("multi primary", np.array(hdul[0].data)[:, ::-1, :], as_planes(a))
        compare("multi extension", np.array(hdul[1].data)[None, ::-1, :], as_planes(b))
        check(hdul[1].header["EXTNAME"] == "mask", "extension EXTNAME")

    out = os.path.join(TMP, "multi.tif")
    run(path, "-o", out, "-f", "-q")
    with tifffile.TiffFile(out) as t:
        check(len(t.pages) == 2, "two TIFF pages")
        p0 = t.pages[0]
        check(p0.tags["InterColorProfile"].value == icc, "ICC profile copied")
        check(p0.tags["XResolution"].value == (300, 1), "XResolution")
        compare("multi tiff page 2", t.pages[1].asarray()[None], as_planes(b))

    run(path, "-o", os.path.join(TMP, "second.fits"), "-f", "-q", "-i", "1")
    got, _ = fits_planes(os.path.join(TMP, "second.fits"))
    compare("--image 1", got, as_planes(b))


def test_bits_conversion():
    a16 = test_image(np.uint16, 10, 10, 1, 4)
    p = os.path.join(TMP, "conv16.xisf")
    XISF.write(p, a16)
    out = os.path.join(TMP, "conv8.tif")
    run(p, "-o", out, "-f", "-q", "-b", "u8")
    got = tiff_array(out)[0]
    exp = np.floor(a16[..., 0].astype(np.float64) * 255 / 65535 + 0.5).astype(np.uint8)
    compare("u16 -> u8", got, exp)

    run(p, "-o", os.path.join(TMP, "conv_f32.fits"), "-f", "-q", "-b", "f32")
    got, _ = fits_planes(os.path.join(TMP, "conv_f32.fits"))
    compare("u16 -> f32", got[0], (a16[..., 0] / 65535.0).astype(np.float32))

    f = np.clip(test_image(np.float32, 10, 10, 3, 4) * 1.2 - 0.1, -0.5, 1.5).astype(np.float32)
    p = os.path.join(TMP, "convf.xisf")
    XISF.write(p, f)
    run(p, "-o", os.path.join(TMP, "convf16.fits"), "-f", "-q", "-b", "u16")
    got, _ = fits_planes(os.path.join(TMP, "convf16.fits"))
    exp = np.floor(np.clip(f.astype(np.float64), 0, 1) * 65535 + 0.5).astype(np.uint16)
    compare("f32 -> u16 (clipped)", got, as_planes(exp))


def test_batch_and_outdir():
    d = os.path.join(TMP, "batch")
    os.makedirs(d, exist_ok=True)
    outd = os.path.join(TMP, "batch_out")
    os.makedirs(outd, exist_ok=True)
    paths = []
    for i in range(3):
        p = os.path.join(d, f"frame_{i}.xisf")
        XISF.write(p, test_image(np.uint16, 8, 8, 1, i), codec="lz4hc", shuffle=True)
        paths.append(p)
    open(os.path.join(d, "broken.xisf"), "wb").write(b"not xisf at all")
    r = run(*paths, os.path.join(d, "broken.xisf"), "-d", outd, "-t", "tiff", "-q", expect_ok=False)
    check(r.returncode == 1, "batch with one bad file exits 1")
    check(sorted(os.listdir(outd)) == ["frame_0.tif", "frame_1.tif", "frame_2.tif"], f"batch outputs {os.listdir(outd)}")
    r = run(paths[0], "-d", outd, "-t", "tiff", expect_ok=False)
    check(r.returncode == 1 and "already exists" in r.stderr, "refuses to overwrite without --force")


def ref_mtf(m, x):
    x = np.clip(x, 0, 1)
    if m == 0.5:
        return x
    return (m - 1) * x / ((2 * m - 1) * x - m)


def ref_autostf(planes, linked):
    """Reference PixInsight AutoSTF: returns [(shadows, midtones)] per channel."""
    med = [np.median(p) for p in planes]
    madn = [1.4826 * np.median(np.abs(p - m)) for p, m in zip(planes, med)]
    def one(m, d):
        c0 = max(0.0, m - 2.8 * d)
        return c0, (ref_mtf(0.25, m - c0) if m > c0 else 0.5)
    if linked:
        p = one(np.mean(med), np.mean(madn))
        return [p] * len(planes)
    return [one(m, d) for m, d in zip(med, madn)]


def ref_apply(planes, params):
    out = []
    for p, (s, m) in zip(planes, params):
        out.append(ref_mtf(m, np.clip((p - s) / (1 - s), 0, 1)))
    return np.array(out)


def astro_image(h, w, c, seed):
    """Linear 'astro' frame: dim background with per-channel offsets, noise and a few stars."""
    rng = np.random.default_rng(seed)
    a = np.empty((h, w, c))
    for ch in range(c):
        a[..., ch] = 0.03 + 0.02 * ch + rng.normal(0, 0.003, (h, w))
    for _ in range(25):
        y, x = rng.integers(2, h - 2), rng.integers(2, w - 2)
        a[y - 1:y + 2, x - 1:x + 2, :] += rng.uniform(0.2, 0.9)
    return np.clip(a, 0, 1).astype(np.float32)


def test_stretch():
    img = astro_image(60, 80, 3, 11)
    p = os.path.join(TMP, "linear_rgb.xisf")
    write_xisf(p, [image_entry(img)])
    planes = as_planes(img).astype(np.float64)
    for mode, linked in (("linked", True), ("unlinked", False), ("auto", True)):
        out = os.path.join(TMP, f"stretch_{mode}.fits")
        r = run(p, "-o", out, "-f", f"--stretch={mode}" if mode != "auto" else "-s")
        got, hdr = fits_planes(out)
        exp = ref_apply(planes, ref_autostf(list(planes), linked))
        check(got.dtype.kind == "f" and got.dtype.itemsize == 4, f"stretch {mode}: FITS stays float")
        check(np.abs(got - exp).max() < 2e-5, f"stretch {mode}: max diff {np.abs(got - exp).max()}")
        check("Stretched with" in str(hdr["HISTORY"]), f"stretch {mode}: HISTORY note")
        check("auto-STF" in r.stderr, f"stretch {mode}: reports parameters")
    # background lands near the 0.25 target for every channel when unlinked
    got, _ = fits_planes(os.path.join(TMP, "stretch_unlinked.fits"))
    check(all(abs(np.median(g) - 0.25) < 0.02 for g in got), "unlinked medians ~0.25")
    # linked keeps channel order of brightness (color balance)
    got, _ = fits_planes(os.path.join(TMP, "stretch_linked.fits"))
    m = [np.median(g) for g in got]
    check(m[0] < m[1] < m[2], f"linked keeps color balance {m}")

    # TIFF: float stretched -> 16-bit by default
    out = os.path.join(TMP, "stretch.tif")
    run(p, "-o", out, "-f", "-q", "-s")
    t = tiff_array(out)[0]
    exp = ref_apply(planes, ref_autostf(list(planes), True))
    check(t.dtype == np.uint16, "stretched TIFF is 16-bit")
    check(np.abs(as_planes(t) / 65535.0 - exp).max() < 1.0 / 65535 + 2e-5, "stretched TIFF values")

    # stored PixInsight STF is used by auto and stf modes
    df = '<DisplayFunction m="0.1:0.2:0.3:0.5" s="0.01:0.02:0.03:0" h="0.9:0.95:1:1" l="0:0:0:0" r="1:1:1:1" name="STF"/>'
    p2 = os.path.join(TMP, "linear_stf.xisf")
    write_xisf(p2, [image_entry(img, children=df)])
    exp = []
    for pl, (s_, m_, h_) in zip(planes, ((0.01, 0.1, 0.9), (0.02, 0.2, 0.95), (0.03, 0.3, 1.0))):
        exp.append(ref_mtf(m_, np.clip((pl - s_) / (h_ - s_), 0, 1)))
    exp = np.array(exp)
    for flag in ("-s", "--stretch=stf"):
        out = os.path.join(TMP, "stretch_stf.fits")
        r = run(p2, "-o", out, "-f", flag)
        got, _ = fits_planes(out)
        check(np.abs(got - exp).max() < 2e-5 and "PixInsight STF" in r.stderr, f"stored STF via {flag}")
    r = run(p2, "--info")
    check("STF:" in r.stdout and "m=0.1" in r.stdout, "--info shows STF")
    r = run(p, "-o", os.path.join(TMP, "x.fits"), "-f", "--stretch=stf", expect_ok=False)
    check(r.returncode == 1 and "no saved STF" in r.stderr, "--stretch=stf without STF fails clearly")

    # 16-bit integer gray input with an alpha-like extra channel: only the first channel is stretched
    a16 = (astro_image(40, 50, 2, 3) * 65535).astype(np.uint16)
    p3 = os.path.join(TMP, "gray_alpha.xisf")
    e = image_entry(a16)
    e["attrs"]["colorSpace"] = "Gray"
    write_xisf(p3, [e])
    out = os.path.join(TMP, "gray_alpha.fits")
    run(p3, "-o", out, "-f", "-q", "-s", "-b", "f32")
    got, _ = fits_planes(out)
    pl = as_planes(a16).astype(np.float64) / 65535
    exp0 = ref_apply(pl[:1], ref_autostf([pl[0]], True))[0]
    check(np.abs(got[0] - exp0).max() < 2e-5, "integer input stretched")
    check(np.abs(got[1] - pl[1]).max() < 2e-6, "alpha channel only normalized")


# ---------------------------------------------------------------- WCS from PixInsight astrometric solutions

def f64_prop(pid, values, rows=None, cols=None):
    b64 = base64.b64encode(np.asarray(values, "<f8").tobytes()).decode()
    if rows:
        return (f'<Property id="{pid}" type="F64Matrix" rows="{rows}" columns="{cols}" '
                f'location="inline:base64">{b64}</Property>')
    return f'<Property id="{pid}" type="F64Vector" length="{len(values)}" location="inline:base64">{b64}</Property>'


def test_wcs():
    from astropy.wcs import WCS
    import warnings
    warnings.simplefilter("ignore")
    W, H = 600, 400
    a = test_image(np.uint16, H, W, 1, 21)
    ref_img = np.array([W / 2 + 0.3, H / 2 - 0.7])   # PixInsight coordinates: corner origin, y down
    ref_cel = np.array([328.178, 47.358])
    M = np.array([[-2.3565e-4, 1.1696e-5], [-1.1715e-5, -2.3575e-4]])  # deg/px, north up, east left
    rng = np.random.default_rng(3)
    xy = rng.uniform([5, 5], [W - 5, H - 5], (300, 2))
    d = xy - ref_img
    r2 = ((d / 300.0) ** 2).sum(1)[:, None]
    uv_lin = d @ M.T
    uv = uv_lin * (1 + 4e-4 * r2)                      # radial (barrel/pincushion-like) distortion
    P = "PCL:AstrometricSolution:"
    def props(world):
        return "".join([
            f'<Property id="{P}ProjectionSystem" type="String">Gnomonic</Property>',
            f64_prop(P + "ReferenceCelestialCoordinates", ref_cel),
            f64_prop(P + "ReferenceImageCoordinates", ref_img),
            f64_prop(P + "ReferenceNativeCoordinates", [0, 90]),
            f64_prop(P + "CelestialPoleNativeCoordinates", [180, 90]),
            f64_prop(P + "LinearTransformationMatrix", M.ravel(), 2, 2),
            f64_prop(P + "SplineWorldTransformation:ControlPoints:Image", xy.ravel()),
            f64_prop(P + "SplineWorldTransformation:ControlPoints:World", world.ravel()),
            '<Property id="Observation:CelestialReferenceSystem" type="String">ICRS</Property>',
        ])
    tan = WCS(naxis=2)
    tan.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    tan.wcs.crval = ref_cel
    tan.wcs.crpix = [0, 0]
    tan.wcs.cd = np.eye(2)
    tan.wcs.lonpole = 180

    def star_error(path, world, bottom_up):
        hdr = fits.getheader(path)
        w = WCS(hdr)
        i0 = xy[:, 0] - 0.5
        j0 = (H - 0.5 - xy[:, 1]) if bottom_up else (xy[:, 1] - 0.5)
        rd = np.array(w.all_pix2world(i0, j0, 0)).T
        exp = np.array(tan.wcs_pix2world(world[:, 0], world[:, 1], 1)).T
        sep = np.hypot((rd[:, 0] - exp[:, 0]) * np.cos(np.radians(exp[:, 1])), rd[:, 1] - exp[:, 1]) * 3600
        return sep.max(), hdr

    # distortion-free solution: the linear WCS must be exact, in both row orders
    p = os.path.join(TMP, "wcs_linear.xisf")
    write_xisf(p, [image_entry(a, children=props(uv_lin))])
    for flag, bottom in (([], True), (["--top-down"], False)):
        out = os.path.join(TMP, "wcs_linear.fits")
        run(p, "-o", out, "-f", "-q", "--sip-order", "0", *flag)
        err, hdr = star_error(out, uv_lin, bottom)
        check(err < 1e-4, f"linear WCS {'bottom-up' if bottom else 'top-down'}: max error {err:.2e} arcsec")
        check(hdr["CTYPE1"] == "RA---TAN" and hdr["RADESYS"] == "ICRS", "linear CTYPE/RADESYS")

    # distorted solution: SIP fitted to the control points must model it
    p = os.path.join(TMP, "wcs_sip.xisf")
    write_xisf(p, [image_entry(a, children=props(uv))])
    out = os.path.join(TMP, "wcs_sip.fits")
    run(p, "-o", out, "-f", "-q", "--sip-order", "0")
    err_lin, _ = star_error(out, uv, True)
    for flag, bottom in (([], True), (["--top-down"], False)):
        r = run(p, "-o", out, "-f", *flag)
        err, hdr = star_error(out, uv, bottom)
        check(hdr["CTYPE1"] == "RA---TAN-SIP" and hdr["A_ORDER"] == 3, "SIP keywords")
        check(err < 0.02 and err < err_lin / 20, f"SIP WCS max error {err:.4f}\" (linear {err_lin:.2f}\")")
        check("SIP order 3" in r.stderr, "WCS summary printed")
        # inverse (AP/BP) round trip
        w = WCS(hdr)
        world = w.all_pix2world([[10.0, 20.0], [W - 10.0, H - 15.0]], 0)
        back = w.all_world2pix(world, 0)
        check(np.abs(back - [[10, 20], [W - 10, H - 15]]).max() < 1e-3, "WCS round trip")
    run(p, "-o", out, "-f", "-q", "--no-wcs")
    check("CTYPE1" not in fits.getheader(out), "--no-wcs")


# ---------------------------------------------------------------- PNG output

def decode_png(path):
    """Minimal independent PNG decoder (non-interlaced, 8/16-bit) -> HxWxC array."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, chunks = 8, b"", []
    while pos < len(data):
        n = int.from_bytes(data[pos:pos + 4], "big")
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        assert zlib.crc32(typ + body) == int.from_bytes(data[pos + 8 + n:pos + 12 + n], "big")
        chunks.append(typ.decode())
        if typ == b"IHDR":
            w, h, depth, ctype = int.from_bytes(body[0:4], "big"), int.from_bytes(body[4:8], "big"), body[8], body[9]
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    bpp = ch * depth // 8
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = np.zeros((h, stride), np.uint8)
    prev = np.zeros(stride, np.int32)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = np.frombuffer(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)], np.uint8).astype(np.int32)
        cur = np.zeros(stride, np.int32)
        for i in range(stride):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 0: pr = 0
            elif f == 1: pr = a
            elif f == 2: pr = b
            elif f == 3: pr = (a + b) // 2
            else:
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            cur[i] = (line[i] + pr) & 255
        out[y] = cur
        prev = cur
    arr = out.reshape(h, w, ch * depth // 8)
    if depth == 16:
        arr = out.view(">u2").reshape(h, w, ch)
    return arr.reshape(h, w, ch), depth, chunks


def test_png():
    have_pngcheck = shutil.which("pngcheck") is not None
    cases = {
        "gray8": (test_image(np.uint8, 23, 31, 1, 1), "Gray"),
        "gray16": (test_image(np.uint16, 23, 31, 1, 2), "Gray"),
        "rgb8": (test_image(np.uint8, 19, 27, 3, 3), "RGB"),
        "rgb16": (test_image(np.uint16, 19, 27, 3, 4), "RGB"),
        "rgba16": (test_image(np.uint16, 17, 13, 4, 5), "RGB"),
        "grayalpha8": (test_image(np.uint8, 17, 13, 2, 6), "Gray"),
    }
    for name, (arr, cs) in cases.items():
        p = os.path.join(TMP, f"png_{name}.xisf")
        e = image_entry(arr, codec="zlib")
        e["attrs"]["colorSpace"] = cs
        write_xisf(p, [e])
        out = os.path.join(TMP, f"png_{name}.png")
        run(p, "-o", out, "-f", "-q")
        got, depth, chunks = decode_png(out)
        compare(f"PNG {name}", got, arr)
        check(depth == arr.dtype.itemsize * 8, f"PNG {name} depth {depth}")
        if have_pngcheck:
            r = subprocess.run(["pngcheck", out], capture_output=True, text=True)
            check(r.returncode == 0 and "OK" in r.stdout, f"pngcheck {name}: {r.stdout.strip()}")
        if name in ("gray8", "rgb8"):
            from PIL import Image
            pil = np.array(Image.open(out))
            compare(f"PIL reads PNG {name}", pil.reshape(arr.shape), arr)

    # float -> 16-bit via bounds; stretch -> 16-bit; --bits u8
    f = test_image(np.float32, 21, 25, 3, 7)
    p = os.path.join(TMP, "png_float.xisf")
    write_xisf(p, [image_entry(f)])
    out = os.path.join(TMP, "png_float.png")
    r = run(p, "-o", out, "-f")
    got, depth, _ = decode_png(out)
    exp = np.floor(np.clip(f.astype(np.float64), 0, 1) * 65535 + 0.5).astype(np.uint16)
    compare("PNG float -> u16", got, exp)
    check("--stretch" in r.stderr, "PNG hints at --stretch for float data")
    run(p, "-t", "png", "-o", out, "-f", "-q", "-s", "-b", "u8")
    got, depth, _ = decode_png(out)
    check(depth == 8 and got.shape == f.shape, "stretched 8-bit PNG")
    r = run(p, "-o", out, "-f", "-b", "f32", expect_ok=False)
    check(r.returncode != 0 and "u8 or u16" in r.stderr, "PNG rejects float --bits")

    # ICC profile and resolution
    icc = bytes(range(200))
    e = image_entry(cases["rgb8"][0], children=f'<ICCProfile location="inline:base64">{base64.b64encode(icc).decode()}</ICCProfile>'
                    '<Resolution horizontal="300" vertical="300" unit="inch"/>')
    p = os.path.join(TMP, "png_icc.xisf")
    write_xisf(p, [e])
    out = os.path.join(TMP, "png_icc.png")
    run(p, "-o", out, "-f", "-q")
    from PIL import Image
    im = Image.open(out)
    check(im.info.get("icc_profile") == icc, "PNG iCCP")
    check(abs(im.info.get("dpi", (0, 0))[0] - 300) < 0.5, f"PNG pHYs {im.info.get('dpi')}")

    # multi-image file: PNG takes the first image (or --image n) with a warning
    p = os.path.join(TMP, "multi.xisf")
    out = os.path.join(TMP, "multi.png")
    r = run(p, "-o", out, "-f")
    check("only" in r.stderr, "PNG multi-image warning")


if __name__ == "__main__":
    print("xisfconv:", EXE)
    print(subprocess.run([EXE, "--version"], capture_output=True, text=True).stdout.strip())
    print("libtiff tiffcp:", "yes" if HAVE_TIFFCP else "no (TIFF decoded by tifffile only)")
    print("NASA fitsverify:", "yes" if HAVE_FITSVERIFY else "no (FITS checked by astropy only)")
    for t in (test_python_xisf_codecs, test_hand_written, test_checksum_mismatch, test_truncated_and_garbage,
              test_keywords_and_properties, test_multi_image_icc_resolution, test_bits_conversion,
              test_batch_and_outdir, test_stretch, test_wcs, test_png):
        try:
            t()
        except Exception as e:  # noqa: BLE001
            failures.append(f"{t.__name__}: {type(e).__name__}: {e}")
            print("ERROR in", t.__name__, ":", e)
    print(f"\n{passed} checks passed, {len(failures)} failed")
    if not failures:
        shutil.rmtree(TMP, ignore_errors=True)
    else:
        print("temp files kept in", TMP)
    sys.exit(1 if failures else 0)
