#!/usr/bin/env python3
"""End-to-end tests for xisfconv.

SPDX-License-Identifier: GPL-3.0-or-later

XISF inputs come from two independent sources:
  * the `xisf` PyPI package (sergio-dr/xisf) for all codecs with/without byte shuffling;
  * a small hand-written XISF encoder below for features that package doesn't emit
    (Normal pixel storage, big-endian data, inline/embedded blocks, subblocks, checksums,
    CFA, ICC profile, multiple images, odd shuffle remainders, tricky FITS keywords).

Outputs are checked with astropy (FITS) and with libtiff's tiffcp + tifffile (TIFF).
ASDF is checked in both directions against Python's asdf library with asdf-astropy, and with
files assembled byte by byte below.

Requirements: pip install numpy astropy tifffile imagecodecs xisf lz4 zstandard pillow
Optional:     pip install asdf asdf-astropy asdf-compression  (without them the ASDF checks that
              need the library are skipped; xisfconv's own reader still checks its output)
              libtiff tools (tiffcp), fitsverify and pngcheck, used as extra independent checkers;
              fpack and funpack (CFITSIO) as a second source of tile-compressed FITS files.
              Without tiffcp and imagecodecs, compressed float TIFF checks are skipped.
Usage: python3 tests/run_tests.py path/to/xisfconv
"""
import base64
import hashlib
import os
import shutil
import struct
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

try:
    import yaml
    HAVE_YAML = True
except ImportError:  # pragma: no cover
    HAVE_YAML = False
try:
    import asdf
    import asdf_astropy  # noqa: F401  (turns the FITS tag into an astropy HDUList)
    HAVE_ASDF = True
except ImportError:  # pragma: no cover
    asdf = None
    HAVE_ASDF = False
try:
    import asdf_compression  # noqa: F401  (zstd blocks for the asdf library)
    HAVE_ASDF_ZSTD = HAVE_ASDF and zstandard is not None
except ImportError:  # pragma: no cover
    HAVE_ASDF_ZSTD = False

# astropy does not map files into memory here: Windows refuses to replace a file that is still
# mapped, and several tests write a file again after reading it. (Linux and Wine allow it, so only
# a real Windows run shows a test that forgot.)
fits.conf.use_memmap = False

EXE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/xisfconv")
TMP = tempfile.mkdtemp(prefix="xisfconv-test-")
HAVE_TIFFCP = shutil.which("tiffcp") is not None
HAVE_FITSVERIFY = shutil.which("fitsverify") is not None
try:
    import imagecodecs  # noqa: F401  (lets tifffile decode the floating-point predictor)
    HAVE_IMAGECODECS = True
except ImportError:
    HAVE_IMAGECODECS = False
# Deflate-compressed float TIFFs use predictor 3, which needs libtiff's tiffcp or imagecodecs.
CAN_DECODE_FLOAT_PREDICTOR = HAVE_TIFFCP or HAVE_IMAGECODECS
skipped = []
LAST_BIT = []   # tile-compressed FITS: comparisons that were equal but for a rounding (see rounding_slack)

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
    # What the program prints is UTF-8 on every system. (Left to the system, Windows reads it in its
    # own code page, and a file name with an umlaut comes back as another name.)
    r = subprocess.run([EXE, *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
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
            if comp and np.issubdtype(expected_hwc.dtype, np.floating) and not CAN_DECODE_FLOAT_PREDICTOR:
                skipped.append(f"{label} -> TIFF deflate (float predictor)")
                continue
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
        '<FITSKeyword name="AMPEND" value="\'' + "a" * 70 + '&amp;\'" comment="ends in an ampersand"/>',
        '<FITSKeyword name="AMPSPLIT" value="\'' + "b" * 65 + '&amp;&amp;c\'" comment="one where the text is split"/>',
        '<FITSKeyword name="AMPONLY" value="\'' + "c" * 66 + '&amp;\'" comment=""/>',
        '<FITSKeyword name="AMPSHORT" value="\'short&amp;\'" comment="fits in one card"/>',
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
    check("truncated" not in r.stderr, "long strings are written with CONTINUE, not truncated")
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
    check(hdr["NOTES"] == "y" * 90, f"long string written with CONTINUE cards ({len(hdr['NOTES'])} chars)")
    # a long text that ends in '&', which is also the mark that the text goes on
    amps = {"AMPEND": "a" * 70 + "&", "AMPSPLIT": "b" * 65 + "&&c", "AMPONLY": "c" * 66 + "&", "AMPSHORT": "short&"}
    for name, text in amps.items():
        check(hdr[name] == text, f"{name}: a text with '&' at its end or where it is split is read back by astropy: {hdr[name][-8:]!r}")
    check(hdr.comments["AMPEND"] == "ends in an ampersand", "and keeps its comment")
    back = os.path.join(TMP, "keywords-back.xisf")
    again = os.path.join(TMP, "keywords-again.fits")
    run(out, "-o", back, "-f", "-q")
    run(back, "-o", again, "-f", "-q")
    _, hdr2 = fits_planes(again)
    check(all(hdr2[name] == text for name, text in amps.items()) and hdr2["NOTES"] == "y" * 90,
          "and comes back the same through FITS -> XISF -> FITS")

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
    # the largest 64-bit number is how the library says "all images": it is not an image index
    r = run(paths[0], "-d", outd, "-f", "-i", "18446744073709551615", expect_ok=False)
    check(r.returncode == 2 and "invalid image index" in r.stderr, "an image index that cannot be one is refused")
    # a NUL byte in a header (a damaged file) does not cut the listing short
    nul = os.path.join(d, "nul.fits")
    h = fits.PrimaryHDU(test_image(np.uint16, 8, 8, 1, 3)[:, :, 0])
    h.header["OBJECT"] = "ab cd"
    h.header["TELESCOP"] = "after"
    h.writeto(nul, overwrite=True)
    raw = open(nul, "rb").read()
    open(nul, "wb").write(raw.replace(b"'ab cd", b"'ab\0cd"))
    r = run("--info", nul)
    check("'ab cd" in r.stdout and "TELESCOP= 'after" in r.stdout and "\0" not in r.stdout, "--info of a FITS header with a NUL byte")


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


def xisf_property(path, pid):
    """Reads one image property straight from the XISF header: a str for scalars and strings,
    a float64 array for F64Vector / F64Matrix (inline or attached). None if absent."""
    import re
    raw = open(path, "rb").read()
    hdr = raw[16:16 + int.from_bytes(raw[8:12], "little")].decode()
    m = re.search(r'<Property id="%s"([^>]*?)(/>|>([^<]*)</Property>)' % re.escape(pid), hdr)
    if not m:
        return None
    attrs = m.group(1)
    loc = re.search(r'location="([^"]+)"', attrs)
    if not loc:
        v = re.search(r'value="([^"]*)"', attrs)
        return v.group(1) if v else m.group(3)
    if loc.group(1).startswith("inline:base64"):
        data = base64.b64decode(m.group(3))
    else:
        _, pos, size = loc.group(1).split(":")
        data = raw[int(pos):int(pos) + int(size)]
    v = np.frombuffer(data, "<f8")
    shape = re.search(r'rows="(\d+)" columns="(\d+)"', attrs)
    return v.reshape(int(shape.group(1)), int(shape.group(2))) if shape else v


def hide_wcs_keywords(src, dst):
    """Copies an XISF file with its WCS FITS keywords renamed (same length, so block offsets stay
    valid), leaving the PixInsight solution properties as the only astrometry in the file."""
    import re
    raw = open(src, "rb").read()
    hlen = int.from_bytes(raw[8:12], "little")
    hdr = raw[16:16 + hlen]
    pat = rb'(<FITSKeyword name=")(WCSAXES|CTYPE\d|CUNIT\d|CRVAL\d|CRPIX\d|CD\d_\d|LONPOLE|LATPOLE|RADESYS|(?:A|B|AP|BP)_\w+)(")'
    hdr2 = re.sub(pat, lambda m: m.group(1) + b"X" + m.group(2)[1:] + m.group(3), hdr)
    assert len(hdr2) == len(hdr) and hdr2 != hdr
    open(dst, "wb").write(raw[:16] + hdr2 + raw[16 + hlen:])


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

    # FITS -> XISF -> FITS keeps the astrometry (including SIP) in every row-order combination
    for first in ([], ["--top-down"]):
        f1 = os.path.join(TMP, "wcs_rt1.fits")
        x2 = os.path.join(TMP, "wcs_rt.xisf")
        # (--no-properties: the FITS file is to bring WCS keywords alone, as a file of any other
        # program does; with the properties the solution would come back as it was, see
        # test_property_round_trip)
        run(p, "-o", f1, "-f", "-q", "--no-properties", *first)
        run(f1, "-o", x2, "-f", "-q")
        compare(f"WCS round trip pixels ({first or 'bottom-up'})", XISF.read(x2), a)

        # FITS -> XISF also writes PixInsight's native solution properties. The control points
        # sampled from the SIP model must reproduce the distortion the test started from.
        S = P + "SplineWorldTransformation:"
        pts = xisf_property(x2, S + "ControlPoints:Image").reshape(-1, 2)
        wld = xisf_property(x2, S + "ControlPoints:World").reshape(-1, 2)
        dd = pts - ref_img
        truth = (dd @ M.T) * (1 + 4e-4 * ((dd / 300.0) ** 2).sum(1)[:, None])
        err = np.hypot(*(wld - truth).T).max() * 3600
        check(len(pts) >= 200 and err < 0.02, f"solution properties {first}: {len(pts)} control points, max error {err:.4f} arcsec")
        check(pts[:, 0].min() == 0 and pts[:, 0].max() == W and pts[:, 1].min() == 0 and pts[:, 1].max() == H,
              "control points cover the image up to its borders")
        check(xisf_property(x2, P + "ProjectionSystem") == "Gnomonic" and
              xisf_property(x2, S + "Version") == "2.0" and
              xisf_property(x2, S + "RBFType") == "DDMThinPlateSpline" and
              float(xisf_property(x2, S + "SplineSmoothness")) == 0 and
              xisf_property(x2, S + "UseSimplifiers") == "false" and
              xisf_property(x2, "Observation:CelestialReferenceSystem") == "ICRS", "solution property identifiers")
        check(np.allclose(xisf_property(x2, P + "ReferenceCelestialCoordinates"), ref_cel, atol=1e-9) and
              np.allclose(xisf_property(x2, P + "ReferenceNativeCoordinates"), [0, 90]) and
              np.allclose(xisf_property(x2, P + "CelestialPoleNativeCoordinates"), [180, 90]), "reference coordinates")
        lin = xisf_property(x2, S + "LinearApproximation")
        ref_xy = xisf_property(x2, P + "ReferenceImageCoordinates")
        check(lin.shape == (2, 3) and np.allclose(lin[:, :2], xisf_property(x2, P + "LinearTransformationMatrix")) and
              np.allclose(lin[:, :2] @ ref_xy + lin[:, 2], 0, atol=1e-12), "linear approximation is consistent")
        # With the WCS keywords hidden, the properties alone must carry the solution back to FITS.
        x3 = os.path.join(TMP, "wcs_rt_props_only.xisf")
        hide_wcs_keywords(x2, x3)
        for second, bottom in (([], True), (["--top-down"], False)):
            f3 = os.path.join(TMP, "wcs_rt3.fits")
            r3 = run(x3, "-o", f3, "-f", *second)
            err, hdr = star_error(f3, uv, bottom)
            check("SIP order 3" in r3.stderr and err < 0.02,
                  f"solution properties alone -> FITS{second}: max error {err:.4f} arcsec")
        for second, bottom in (([], True), (["--top-down"], False)):
            f2 = os.path.join(TMP, "wcs_rt2.fits")
            run(x2, "-o", f2, "-f", "-q", *second)
            err, hdr = star_error(f2, uv, bottom)
            check(err < 0.02 and hdr["CTYPE1"] == "RA---TAN-SIP",
                  f"WCS after FITS{first}->XISF->FITS{second}: max error {err:.4f} arcsec")


def test_solution_properties_forms():
    """PixInsight solution properties from the different ways FITS expresses the linear WCS."""
    from astropy.wcs import WCS
    import warnings
    warnings.simplefilter("ignore")
    H, W = 40, 60
    data = test_image(np.uint16, H, W, 1, 61)[..., 0]
    P = "PCL:AstrometricSolution:"
    out = os.path.join(TMP, "sp.xisf")

    def convert(cards, *flags, expect_props=True):
        hdu = fits.PrimaryHDU(data)
        for k, v in cards.items():
            hdu.header[k] = v
        src = os.path.join(TMP, "sp.fits")
        hdu.writeto(src, overwrite=True)
        r = run(src, "-o", out, "-f", *flags)
        m = xisf_property(out, P + "LinearTransformationMatrix")
        check((m is not None) == expect_props, f"solution properties {'written' if expect_props else 'absent'} for {list(cards)[:3]} {flags}")
        return src, m, r

    base = {"CTYPE1": "RA---TAN", "CTYPE2": "DEC--TAN", "CRVAL1": 150.25, "CRVAL2": -32.5, "CRPIX1": 30.5, "CRPIX2": 21.25}
    forms = {
        "CD matrix": {"CD1_1": -2.1e-4, "CD1_2": 3.0e-5, "CD2_1": 2.9e-5, "CD2_2": 2.2e-4},
        "CDELT + CROTA2": {"CDELT1": -2.0e-4, "CDELT2": 2.0e-4, "CROTA2": 31.0},
        "PC + CDELT": {"CDELT1": -2.0e-4, "CDELT2": 2.1e-4, "PC1_1": 0.9, "PC1_2": -0.4, "PC2_1": 0.42, "PC2_2": 0.91},
        "CDELT only": {"CDELT1": -1.9e-4, "CDELT2": 1.9e-4},
    }
    for name, extra in forms.items():
        src, m, r = convert({**base, **extra})
        cd = WCS(fits.getheader(src)).pixel_scale_matrix        # astropy is the oracle for the CD matrix
        check(np.allclose(m, [[cd[0, 0], -cd[0, 1]], [cd[1, 0], -cd[1, 1]]], rtol=1e-9, atol=1e-15), f"{name}: matrix {m.ravel()}")
        check(np.allclose(xisf_property(out, P + "ReferenceImageCoordinates"), [30.5 - 0.5, H + 0.5 - 21.25]) and
              np.allclose(xisf_property(out, P + "ReferenceCelestialCoordinates"), [150.25, -32.5]), f"{name}: reference point")
        check(xisf_property(out, P + "SplineWorldTransformation:Version") is None and "linear" in r.stderr,
              f"{name}: linear solution has no spline properties")

    # a top-down FITS describes the same sky: after conversion the properties must be identical
    cdform = {**base, **forms["CD matrix"]}
    _, m_bottom, _ = convert(cdform)
    ref_bottom = xisf_property(out, P + "ReferenceImageCoordinates")
    flipped = dict(cdform, CRPIX2=H + 1 - cdform["CRPIX2"], CD1_2=-cdform["CD1_2"], CD2_2=-cdform["CD2_2"], ROWORDER="TOP-DOWN")
    _, m_top, _ = convert(flipped)
    check(np.allclose(m_top, m_bottom) and np.allclose(xisf_property(out, P + "ReferenceImageCoordinates"), ref_bottom),
          "top-down FITS gives the same solution properties")

    _, _, r = convert(cdform, "--no-wcs", expect_props=False)
    check(b"PCL:AstrometricSolution" not in open(out, "rb").read(20000), "--no-wcs writes no solution properties")
    _, _, r = convert({**cdform, "CTYPE1": "RA---CAR", "CTYPE2": "DEC--CAR"}, expect_props=False)
    check("no PixInsight solution properties" in r.stderr and "CAR" in r.stderr, "unsupported projection is reported")
    _, _, r = convert({**cdform, "CTYPE1": "GLON-TAN", "CTYPE2": "GLAT-TAN"}, expect_props=False)
    check("not RA/Dec" in r.stderr, "non-equatorial WCS is reported")
    _, _, r = convert({}, expect_props=False)
    check("solution" not in r.stderr, "no WCS, no message")
    compare("pixels unaffected by properties", XISF.read(out), fits_expected(os.path.join(TMP, "sp.fits")))


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


def test_tiff_predictors():
    """Each sample format gets the predictor that libtiff >= 4.0 can decode (no predictor 2 on 64-bit)."""
    expected = {np.uint8: 2, np.uint16: 2, np.uint32: 2, np.uint64: 1, np.float32: 3, np.float64: 3}
    for dtype, pred in expected.items():
        a = test_image(dtype, 9, 11, 1, 1)
        p = os.path.join(TMP, f"pred_{np.dtype(dtype).name}.xisf")
        write_xisf(p, [image_entry(a)])
        out = p + ".tif"
        run(p, "-o", out, "-f", "-q", "-c")
        with tifffile.TiffFile(out) as t:
            got = int(t.pages[0].tags["Predictor"].value) if "Predictor" in t.pages[0].tags else 1
        check(got == pred, f"TIFF predictor for {np.dtype(dtype).name}: {got}, expected {pred}")


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


# ---------------------------------------------------------------- FITS -> XISF

def fits_expected(path, hdu=0, flip=True):
    """What the XISF pixels should be, using astropy as the independent FITS reader: HxWxC, top-down."""
    d = np.array(fits.getdata(path, hdu))
    if d.ndim == 2:
        d = d[None]
    d = np.transpose(d, (1, 2, 0))
    d = d[::-1] if flip else d
    return np.ascontiguousarray(d.astype(d.dtype.newbyteorder("=")))


def read_xisf_any(path, n=0):
    """Reads image n as HxWxC. Uses the `xisf` package when it can, otherwise a small independent
    decoder (the package implements neither subblocks nor UInt64). Checksums are verified here."""
    import re
    raw = open(path, "rb").read()
    hlen = int.from_bytes(raw[8:12], "little")
    tag = re.findall(r"<Image [^>]*>", raw[16:16 + hlen].decode())[n]
    attr = dict(re.findall(r'(\w+)="([^"]*)"', tag))
    _, pos, size = attr["location"].split(":")
    stored = raw[int(pos):int(pos) + int(size)]
    if "checksum" in attr:
        algo, digest = attr["checksum"].split(":")
        assert hashlib.new(algo.replace("-", ""), stored).hexdigest() == digest, "checksum mismatch in " + path
    if "subblocks" not in attr and attr["sampleFormat"] != "UInt64":
        return XISF(path).read_image(n)
    w, h, c = [int(v) for v in attr["geometry"].split(":")]
    dtype = np.dtype({v: k for k, v in SF.items()}[attr["sampleFormat"]])
    data = stored
    if "compression" in attr:
        parts = attr["compression"].split(":")
        codec = parts[0].split("+")[0]
        dec = (lambda b, u: zlib.decompress(b)) if codec == "zlib" else \
              (lambda b, u: lz4.block.decompress(b, uncompressed_size=u)) if codec in ("lz4", "lz4hc") else \
              (lambda b, u: zstandard.decompress(b, max_output_size=u))
        if "subblocks" in attr:
            out, off = b"", 0
            for pair in attr["subblocks"].split(":"):
                cs, us = [int(v) for v in pair.split(",")]
                out += dec(stored[off:off + cs], us)
                off += cs
            assert off == len(stored)
            data = out
        else:
            data = dec(stored, int(parts[1]))
        assert len(data) == int(parts[1])
        if parts[0].endswith("+sh"):
            item = int(parts[2])
            cnt = len(data) // item
            data = np.frombuffer(data[:cnt * item], np.uint8).reshape(item, cnt).T.tobytes() + data[cnt * item:]
    return np.transpose(np.frombuffer(data, dtype.newbyteorder("<")).reshape(c, h, w), (1, 2, 0)).astype(dtype)


def unq(value):
    """FITS string value without its enclosing quotes (the xisf package strips them, the spec keeps them)."""
    v = value.strip()
    return v[1:-1] if len(v) >= 2 and v[0] == "'" and v[-1] == "'" else v


def xisf_keywords(path, n=0):
    return XISF(path).get_images_metadata()[n]["FITSKeywords"]


def test_fits_to_xisf_formats():
    rng = np.random.default_rng(11)
    H, W = 21, 34
    ramp = (np.mgrid[0:H, 0:W][0] * 97 + np.mgrid[0:H, 0:W][1] * 13)
    cases = {
        # name: (data written by astropy, expected XISF dtype)
        "u8": (test_image(np.uint8, H, W, 1, 1)[..., 0], np.uint8),
        "i16_nonneg": ((ramp % 30000).astype(np.int16), np.uint16),
        "i16_negative": ((ramp % 30000 - 15000).astype(np.int16), np.float32),
        "u16": (test_image(np.uint16, H, W, 1, 2)[..., 0], np.uint16),
        "i32_negative": ((ramp * 100000 - 7).astype(np.int32), np.float64),
        "u32": (test_image(np.uint32, H, W, 1, 3)[..., 0], np.uint32),
        "i64_nonneg": ((ramp.astype(np.int64) * 10**12), np.uint64),
        "f32": (test_image(np.float32, H, W, 1, 4)[..., 0], np.float32),
        "f64": (test_image(np.float64, H, W, 1, 5)[..., 0], np.float64),
        "rgb_u16": (as_planes(test_image(np.uint16, H, W, 3, 6)), np.uint16),
        "rgb_f32": (as_planes(test_image(np.float32, H, W, 3, 7)), np.float32),
    }
    variants = [[], ["-c"], ["--codec", "zlib"], ["-c", "--checksum", "sha1"], ["--checksum", "sha512"],
                ["--codec", "zlib", "--checksum", "sha256", "--xisf-subblock-size", "700"],
                ["-c", "--xisf-subblock-size", "1000"], ["--codec", "lz4"], ["--codec", "lz4hc", "--checksum", "sha1"],
                ["--codec", "lz4", "--xisf-subblock-size", "600"], ["--codec", "lz4hc", "--xisf-subblock-size", "900"]]
    for k, (name, (data, want)) in enumerate(cases.items()):
        src = os.path.join(TMP, f"f2x_{name}.fits")
        fits.PrimaryHDU(data).writeto(src, overwrite=True)
        exp = fits_expected(src)
        for j, flags in enumerate([variants[k % len(variants)], variants[(k + 3) % len(variants)], variants[(k + 7) % len(variants)]]):
            out = os.path.join(TMP, f"f2x_{name}_{j}.xisf")
            run(src, "-o", out, "-f", "-q", *flags)
            got = read_xisf_any(out)
            label = f"FITS {name} -> XISF {' '.join(flags) or 'plain'}"
            check(got.dtype == np.dtype(want), f"{label}: dtype {got.dtype}, expected {np.dtype(want)}")
            compare(label, got, exp.astype(want))
            # and back: xisfconv must read its own XISF and reproduce the FITS values
            back = out + ".fits"
            run(out, "-o", back, "-f", "-q")
            got2, _ = fits_planes(back)
            compare(f"{label} -> FITS", got2, as_planes(exp.astype(want)))
        if name in ("u16", "rgb_f32"):
            m = XISF(out).get_images_metadata()[0]
            check(m["geometry"] == (W, H, exp.shape[2]) and m["colorSpace"] == ("RGB" if exp.shape[2] == 3 else "Gray"),
                  f"{name}: geometry/colorSpace {m['geometry']} {m['colorSpace']}")

    # arbitrary BSCALE/BZERO are applied (astropy is the oracle)
    src = os.path.join(TMP, "f2x_scaled.fits")
    hdu = fits.PrimaryHDU((ramp % 2000).astype(np.int16))
    hdu.scale("int16", bscale=0.5, bzero=10)
    hdu.writeto(src, overwrite=True)
    hdr = fits.getheader(src)
    check(hdr.get("BSCALE") == 0.5 and hdr.get("BZERO") == 10, "test FITS really has BSCALE/BZERO")
    out = os.path.join(TMP, "f2x_scaled.xisf")
    run(src, "-o", out, "-f", "-q")
    got = XISF.read(out)
    check(got.dtype == np.float32 and np.allclose(got, fits_expected(src), rtol=0, atol=1e-4), "BSCALE/BZERO applied")

    # subblocks really are written, and compression attributes are what PixInsight uses
    src = os.path.join(TMP, "f2x_u16.fits")
    out = os.path.join(TMP, "f2x_sub.xisf")
    run(src, "-o", out, "-f", "-q", "-c", "--xisf-subblock-size", "500", "--checksum", "sha256")
    info = run(out, "--info").stdout
    check("subblocks:" in info and "zstd+sh:" in info and "checksum:    sha256:" in info, "subblocks/codec/checksum attributes")
    compare("subblocked XISF read by the independent decoder", read_xisf_any(out), fits_expected(src))
    check(open(out, "rb").read(8) == b"XISF0100", "XISF signature")
    # LZ4 and LZ4HC, written since 0.15: the names in the file, and the lz4 library reads the blocks
    for codec in ("lz4", "lz4hc"):
        run(src, "-o", out, "-f", "-q", "--codec", codec, "--xisf-subblock-size", "500")
        info = run(out, "--info").stdout
        check("subblocks:" in info and f"{codec}+sh:" in info, f"--codec {codec}: the attributes of the block")
        compare(f"--codec {codec}: read by the independent decoder", read_xisf_any(out), fits_expected(src))
        check(run(out, "--verify", expect_ok=False).returncode == 0, f"--codec {codec}: the file verifies")
        _, hdr = xisf_header(out)
        check(f'<Property id="XISF:CompressionCodecs" type="String">{codec}+sh</Property>' in hdr, f"--codec {codec}: named in the metadata")
    r = run(src, "-o", out, "-f", "-q", "--codec", "brotli", expect_ok=False)
    check(r.returncode != 0 and "unknown codec 'brotli' (use zlib, zstd, lz4, lz4hc or none)" in r.stderr,
          f"a codec that is none: {r.stderr.strip()[:120]}")
    for target in ("lz4.asdf", "lz4.fits"):
        r = run(out, "-o", os.path.join(TMP, target), "-f", "-q", "--codec", "lz4", expect_ok=False)
        check(r.returncode != 0 and ("XISF only" in r.stderr or "FITS has no LZ4 compression" in r.stderr) and
              not os.path.exists(os.path.join(TMP, target)), f"--codec lz4 is for XISF, not for {target}: {r.stderr.strip()[:140]}")
        r = run(src, "-o", os.path.join(TMP, target), "-f", "-q", "--codec", "lz4hc", expect_ok=False)
        check(r.returncode != 0 and not os.path.exists(os.path.join(TMP, target)) and not os.path.exists(os.path.join(TMP, target + ".part")),
              f"nor from FITS, and nothing is left of {target}")


def test_fits_to_xisf_metadata():
    H, W = 20, 30
    a = test_image(np.uint16, H, W, 1, 31)[..., 0]
    long_text = "A long description " + "x" * 90 + " end"
    hdu = fits.PrimaryHDU(a)
    h = hdu.header
    h["OBJECT"] = ("M 31 & friends", "target <name>")
    h["OBSERVER"] = "O'Brien"
    h["EXPTIME"] = (300.5, "seconds")
    h["GAIN"] = 120
    h["FLAG"] = True
    h["LONGSTR"] = long_text
    h["HIERARCH ESO DET CHIP TEMP"] = (-10.5, "deg C")
    h["BAYERPAT"] = "RGGB"
    h["HISTORY"] = "calibrated with master dark"
    h["COMMENT"] = "a comment line"
    src = os.path.join(TMP, "f2x_meta.fits")
    hdu.writeto(src, overwrite=True)
    check("CONTINUE" in open(src, "rb").read(5760).decode("ascii", "replace"), "test FITS uses CONTINUE cards")

    out = os.path.join(TMP, "f2x_meta.xisf")
    r = run(src, "-o", out, "-f")
    check("flipped" in r.stderr, "reports the row flip")
    kw = xisf_keywords(out)
    v = lambda k: kw[k][0]["value"]
    check(unq(v("OBJECT")) == "M 31 & friends" and kw["OBJECT"][0]["comment"] == "target <name>", f"OBJECT {kw.get('OBJECT')}")
    check(unq(v("OBSERVER")) == "O''Brien", f"OBSERVER {v('OBSERVER')}")
    header_xml = open(out, "rb").read(6000).decode("ascii", "replace")
    check("value=\"'M 31 &amp; friends'\"" in header_xml and "value=\"'O''Brien'\"" in header_xml,
          "string keyword values keep their FITS quotes in the XML, as PixInsight writes them")
    check(float(v("EXPTIME")) == 300.5 and v("GAIN") == "120" and v("FLAG") == "T", "numeric/logical keywords")
    check(unq(v("LONGSTR")) == long_text, f"CONTINUE joined: {v('LONGSTR')[:40]}...")
    check(float(v("ESO DET CHIP TEMP")) == -10.5, "HIERARCH keyword")
    check(any("calibrated with master dark" in e["comment"] for e in kw["HISTORY"]), "HISTORY kept")
    check(any("Converted from FITS by xisfconv" in e["comment"] for e in kw["HISTORY"]), "provenance HISTORY")
    check(any("a comment line" in e["comment"] for e in kw["COMMENT"]), "COMMENT kept")
    for structural in ("SIMPLE", "BITPIX", "NAXIS", "NAXIS1", "BZERO", "BSCALE", "EXTEND", "ROWORDER"):
        check(structural not in kw, f"{structural} must not be copied")
    # even image height: flipping the rows turns RGGB into GBRG, in the keyword and the CFA element
    check(unq(v("BAYERPAT")) == "GBRG", f"BAYERPAT flipped with the rows: {v('BAYERPAT')}")
    info = run(out, "--info").stdout
    check("CFA:         GBRG (2x2)" in info, "ColorFilterArray element written")
    check('Image 0 "f2x_meta"' in info, "image id from the file name")

    # back to FITS: keywords survive the round trip
    back = os.path.join(TMP, "f2x_meta_back.fits")
    run(out, "-o", back, "-f", "-q")
    hb = fits.getheader(back)
    check(hb["LONGSTR"] == long_text, "long string survives FITS -> XISF -> FITS")
    check(hb["OBJECT"] == "M 31 & friends" and hb["OBSERVER"] == "O'Brien" and hb["EXPTIME"] == 300.5 and
          hb["GAIN"] == 120 and hb["FLAG"] is True and hb["BAYERPAT"] == "RGGB" and
          hb["ESO DET CHIP TEMP"] == -10.5, "keywords after FITS -> XISF -> FITS")
    compare("pixels after FITS -> XISF -> FITS", np.array(fits.getdata(back)), a)

    # row order: ROWORDER keyword and the command-line overrides
    exp_flip, exp_keep = fits_expected(src), fits_expected(src, flip=False)
    top = os.path.join(TMP, "f2x_top.fits")
    hdu.header["ROWORDER"] = "TOP-DOWN"
    hdu.writeto(top, overwrite=True)
    for path, flags, exp, pattern, what in (
            (top, [], exp_keep, "RGGB", "ROWORDER=TOP-DOWN is honoured"),
            (top, ["--bottom-up"], exp_flip, "GBRG", "--bottom-up overrides ROWORDER"),
            (src, ["--top-down"], exp_keep, "RGGB", "--top-down without ROWORDER"),
            (src, [], exp_flip, "GBRG", "no ROWORDER means bottom-up")):
        run(path, "-o", out, "-f", "-q", *flags)
        compare(what, XISF.read(out), exp)
        check(unq(xisf_keywords(out)["BAYERPAT"][0]["value"]) == pattern, f"{what}: BAYERPAT {pattern}")

    # --info on a FITS file
    info = run(top, "--info").stdout
    check("BITPIX 16" in info and "top-down (ROWORDER)" in info and "OBSERVER" in info, "--info for FITS input")


def test_fits_to_xisf_bounds_bits_hdus():
    H, W = 12, 16
    base = test_image(np.float32, H, W, 1, 41)[..., 0]
    out = os.path.join(TMP, "f2x_b.xisf")

    def bounds_of(data, *flags):
        src = os.path.join(TMP, "f2x_b.fits")
        fits.PrimaryHDU(data).writeto(src, overwrite=True)
        r = run(src, "-o", out, "-f", *flags)
        line = [l for l in run(out, "--info").stdout.splitlines() if "bounds:" in l][0]
        lo, hi = [float(x) for x in line.split("bounds:")[1].split(":")]
        compare(f"float data unchanged ({flags})", XISF.read(out), fits_expected(src))
        return lo, hi, r.stderr

    lo, hi, err = bounds_of(base)
    check((lo, hi) == (0, 1) and "bounds" not in err, f"data in [0,1] -> bounds 0:1 ({lo}:{hi})")
    lo, hi, err = bounds_of((base * 40000).astype(np.float32))
    check((lo, hi) == (0, 65535) and "0:65535" in err, f"ADU-range floats -> bounds 0:65535 ({lo}:{hi})")
    neg = (base * 10 - 3).astype(np.float32)
    lo, hi, err = bounds_of(neg)
    attr = open(out, "rb").read(3000).decode("ascii", "replace").split('bounds="')[1].split('"')[0]
    lo, hi = [float(x) for x in attr.split(":")]
    check(lo == float(neg.min()) and hi == float(neg.max()), f"other floats -> min:max ({lo}:{hi})")
    lo, hi, err = bounds_of(neg, "--bounds", "-5:20")
    check((lo, hi) == (-5, 20), "--bounds override")

    # --bits
    u16 = test_image(np.uint16, H, W, 1, 42)[..., 0]
    src = os.path.join(TMP, "f2x_bits.fits")
    fits.PrimaryHDU(u16).writeto(src, overwrite=True)
    run(src, "-o", out, "-f", "-q", "-b", "f32")
    compare("FITS u16 -> XISF f32", XISF.read(out), (fits_expected(src) / 65535.0).astype(np.float32))
    fits.PrimaryHDU(base).writeto(src, overwrite=True)
    run(src, "-o", out, "-f", "-q", "-b", "u16")
    exp = np.floor(np.clip(fits_expected(src).astype(np.float64), 0, 1) * 65535 + 0.5).astype(np.uint16)
    compare("FITS f32 -> XISF u16", XISF.read(out), exp)

    # several HDUs: image extensions become XISF images, tables are skipped
    a = test_image(np.uint16, H, W, 3, 43)
    b = test_image(np.float32, 9, 7, 1, 44)[..., 0]
    table = fits.BinTableHDU.from_columns([fits.Column(name="x", format="E", array=np.arange(5.0))])
    src = os.path.join(TMP, "f2x_multi.fits")
    fits.HDUList([fits.PrimaryHDU(as_planes(a)), table, fits.ImageHDU(b, name="MASK 1")]).writeto(src, overwrite=True)
    r = run(src, "-o", out, "-f")
    check("BINTABLE" in r.stderr and "not an image" in r.stderr, "table HDU reported as skipped")
    x = XISF(out)
    meta = x.get_images_metadata()
    check(len(meta) == 2, f"two images written ({len(meta)})")
    compare("multi-HDU image 0", x.read_image(0), fits_expected(src, 0))
    compare("multi-HDU image 1", x.read_image(1), fits_expected(src, 2))
    info = run(out, "--info").stdout
    check('Image 1 "MASK_1"' in info, "EXTNAME becomes a valid image id")
    run(src, "-o", out, "-f", "-q", "-i", "1")
    compare("--image 1 on FITS", XISF.read(out), fits_expected(src, 2))

    # empty primary HDU followed by an image extension
    src = os.path.join(TMP, "f2x_emptyprimary.fits")
    fits.HDUList([fits.PrimaryHDU(), fits.ImageHDU(b)]).writeto(src, overwrite=True)
    run(src, "-o", out, "-f", "-q")
    compare("empty primary + extension", XISF.read(out), fits_expected(src, 1))

    # default output name and format, batch of mixed inputs
    d = os.path.join(TMP, "f2x_batch")
    os.makedirs(d, exist_ok=True)
    fpath, xpath = os.path.join(d, "one.fits"), os.path.join(d, "two.xisf")
    fits.PrimaryHDU(u16).writeto(fpath, overwrite=True)
    XISF.write(xpath, u16[..., None])
    run(fpath, xpath, "-f", "-q")
    check(os.path.exists(os.path.join(d, "one.xisf")) and os.path.exists(os.path.join(d, "two.fits")),
          "mixed batch: each input converted to the other format")

    # errors
    r = run(fpath, "-t", "fits", "-f", expect_ok=False)
    check(r.returncode == 1 and "already a FITS" in r.stderr, "FITS -> FITS is refused clearly")
    r = run(fpath, "-s", "-f", expect_ok=False)
    check(r.returncode == 1 and "--stretch" in r.stderr, "--stretch refused for XISF output")
    r = run(xpath, "-t", "xisf", "-f", expect_ok=False)
    check(r.returncode == 1 and "add --in-place" in r.stderr, "XISF -> XISF onto itself needs --in-place")
    r = run(fpath, expect_ok=False)
    check(r.returncode == 1 and "already exists" in r.stderr, "refuses to overwrite an XISF without --force")
    data = open(fpath, "rb").read()
    trunc = os.path.join(d, "trunc.fits")
    open(trunc, "wb").write(data[:2880 + 100])
    r = run(trunc, "-f", expect_ok=False)
    check(r.returncode == 1 and "beyond the end" in r.stderr, "truncated FITS reported")
    check(not os.path.exists(os.path.join(d, "trunc.xisf.part")), "no partial XISF left behind")


def test_xisf_fits_xisf_roundtrip():
    """XISF -> FITS -> XISF returns the original pixels for every format and both row orders."""
    for dtype in (np.uint8, np.uint16, np.uint32, np.uint64, np.float32, np.float64):
        for c in (1, 3):
            a = test_image(dtype, 15, 22, c, 51)
            p = os.path.join(TMP, f"rt_{np.dtype(dtype).name}_{c}.xisf")
            write_xisf(p, [image_entry(a, codec="zlib", shuffle_item=np.dtype(dtype).itemsize)])
            for flags in ([], ["--top-down"]):
                f = p + ".fits"
                back = p + ".back.xisf"
                run(p, "-o", f, "-f", "-q", *flags)
                run(f, "-o", back, "-f", "-q", "-c")
                got = read_xisf_any(back)
                check(got.dtype == np.dtype(dtype), f"round trip {np.dtype(dtype).name}: dtype {got.dtype}")
                compare(f"XISF -> FITS{flags} -> XISF {np.dtype(dtype).name} {c}ch", got, a)


# ---------------------------------------------------------------- ASDF

ASDF_HEAD = "#ASDF 1.0.0\n#ASDF_STANDARD 1.5.0\n%YAML 1.1\n%TAG ! tag:stsci.edu:asdf/\n--- !core/asdf-1.1.0\n"
STRUCTURAL = {"SIMPLE", "BITPIX", "NAXIS", "NAXIS1", "NAXIS2", "NAXIS3", "EXTEND", "BZERO", "BSCALE", "XTENSION",
              "PCOUNT", "GCOUNT", "LONGSTRN"}
ZSTD_BUILD = "zstd" in subprocess.run([EXE, "--version"], capture_output=True, text=True).stdout


def asdf_block(data, compression=b"", flags=0, header_size=48, extra_alloc=0, checksum="ok", data_size=None):
    """One ASDF binary block, written independently of xisfconv and of the asdf library."""
    stored = data
    if compression == b"zlib":
        stored = zlib.compress(data)
    elif compression == b"zstd":
        stored = zstandard.ZstdCompressor().compress(data)
    elif compression == b"lz4":  # the asdf library's framing: chunks with a big-endian size prefix
        stored = b""
        for i in range(0, len(data), 1000):
            c = lz4.block.compress(data[i:i + 1000])
            stored += len(c).to_bytes(4, "big") + c
    digest = {"ok": hashlib.md5(stored).digest(), "zero": b"\0" * 16, "bad": b"\x01" * 16,
              "uncompressed": hashlib.md5(data).digest()}[checksum]
    used = 0 if flags & 1 else len(stored)
    return (b"\xd3BLK" + header_size.to_bytes(2, "big") + flags.to_bytes(4, "big") + compression.ljust(4, b"\0") +
            (used + extra_alloc).to_bytes(8, "big") + used.to_bytes(8, "big") +
            (0 if flags & 1 else len(data) if data_size is None else data_size).to_bytes(8, "big") + digest +
            b"\0" * (header_size - 48) + stored + b"\0" * extra_alloc)


def write_asdf_raw(path, tree, blocks, pad=b"", newline="\n", head=ASDF_HEAD, tail=b""):
    text = (head + tree + "...\n").replace("\n", newline).encode()
    with open(path, "wb") as f:
        f.write(text + pad + b"".join(blocks) + tail)


def user_cards(header):
    """(keyword, value, comment) of every card that is not structural."""
    out = []
    for c in header.cards:
        if c.keyword in STRUCTURAL:
            continue
        out.append((c.keyword, None if undefined(c.value) else c.value, c.comment))
    return out


def undefined(value):
    return value is None or isinstance(value, fits.card.Undefined)


def xisf_keyword_values(path):
    """{name: value} of the FITS keywords of the first image, read straight from the XISF header."""
    import re
    raw = open(path, "rb").read()
    hdr = raw[16:16 + int.from_bytes(raw[8:12], "little")].decode()
    first = hdr[:hdr.index("</Image>")] if "</Image>" in hdr else hdr
    import html
    return {m.group(1): html.unescape(m.group(2)) for m in re.finditer(r'<FITSKeyword name="([^"]*)" value="([^"]*)"', first)}


def open_asdf(path, validate=True):
    """Opens a file with the asdf library, treating every warning of asdf and astropy as an error.
    Returns the tree's HDUs as (array, header) pairs plus the asdf_library entry.
    (Reading validates the tree against the schemas; validate() also re-serializes the HDU list,
    which asdf-astropy cannot do for uint64 data because of the BZERO = 2^63 card astropy adds.)"""
    import warnings
    from astropy.utils.exceptions import AstropyWarning
    with warnings.catch_warnings():
        warnings.simplefilter("error", asdf.exceptions.AsdfWarning)
        warnings.simplefilter("error", AstropyWarning)
        with asdf.open(path, validate_checksums=True, memmap=False) as af:
            if validate:
                af.validate()
            hdul = af["fits"]
            assert isinstance(hdul, fits.HDUList), type(hdul)
            hdus = [(np.array(h.data), h.header.copy()) for h in hdul]
            return hdus, dict(af["asdf_library"])


def test_asdf_yaml():
    """The YAML reader against PyYAML (the parser behind the asdf library), on random documents
    in every output style PyYAML has, and on constructs PyYAML never writes itself."""
    if not HAVE_YAML:
        skipped.append("YAML reader vs PyYAML (pip install pyyaml)")
        return
    import json
    import random

    class Loader(yaml.SafeLoader):
        pass
    Loader.add_constructor("tag:yaml.org,2002:timestamp", lambda l, n: l.construct_scalar(n))
    strings = ["", " lead", "trail ", "a: b", "a #b", "- x", "yes", "No", "null", "~", "1e5", "1.5", "0x1F", "012",
               "1_000", "12:30:00", "2024-01-01", "multi\nline", "tab\there", "\u00e9", "\u65e5\u672c\u8a9e",
               "\U0001F600", "it's", 'say "hi"', "back\\slash", "{a}", "[b]", "a, b", "!tag", "&anc", "*ali", "|", ">",
               "%", "@", "`", "a\n", "\n", "a\n\nb", "  indented\n lines\n", "x" * 150, "word " * 40, "a:b",
               "http://x.y/z?q=1#frag", "key", "value with spaces", "-", "--- x", "...", "?", ": x", "#c", "a\tb",
               "ends with colon:", "'", '"', "\\", "<<", "=", "TRUE", "off", "0o17", "+12", ".5", "-.inf", ".NaN", "1.",
               "0", "-0", "\x07bell", "\u00a0nbsp", "trailing\n\n\n", " \n ", "a\r\nb", "1.5e5", "1.5e+5", "-.5",
               "x" * 80 + "\t tail words here", "tab\t \t" + "y" * 70 + " \t z", "1.0e+400"]
    rnd = random.Random(20261002)

    def value(depth=0):
        r = rnd.random()
        if depth > 4 or r < 0.45:
            k = rnd.randrange(8)
            if k == 0:
                return None
            if k == 1:
                return rnd.choice([True, False])
            if k == 2:
                return rnd.choice([0, -1, 7, 2 ** 70, -2 ** 63, 123456789])
            if k == 3:
                return rnd.choice([1.5, -0.0, 1e300, 1e-300, float("inf"), float("-inf"), float("nan"), 3.0, 0.1,
                                   -2.5e-7, 1e16])
            return rnd.choice(strings)
        if r < 0.72:
            return [value(depth + 1) for _ in range(rnd.randrange(5))]
        out = {}
        for _ in range(rnd.randrange(5)):
            key = (rnd.choice([rnd.choice(strings), rnd.randrange(100), 2.5, True, None]) if rnd.random() < 0.5
                   else "key%d" % rnd.randrange(1000))
            out[key] = value(depth + 1)
        return out

    def norm(o):   # PyYAML's result
        if isinstance(o, dict):
            return ("m", [(norm(k), norm(v)) for k, v in o.items()])
        if isinstance(o, list):
            return [norm(x) for x in o]
        if isinstance(o, float):
            return ("f", "nan" if o != o else repr(o))
        return o

    def mine(o):   # xisfconv's JSON dump of its parse
        if isinstance(o, dict):
            if "f" in o:
                return ("f", "nan" if o["f"] == "nan" else repr(float(o["f"])))
            return ("m", [(mine(k), mine(v)) for k, v in o["m"]])
        if isinstance(o, list):
            return [mine(x) for x in o]
        return o

    def parsed(text, newline="\n"):
        path = os.path.join(TMP, "yaml_case.asdf")
        with open(path, "wb") as f:
            f.write(("#ASDF 1.0.0" + newline + "%YAML 1.1" + newline + text + "..." + newline).encode())
        r = subprocess.run([EXE, path, "--asdf-tree-json"], capture_output=True)   # bytes: the output is UTF-8
        if r.returncode != 0:
            return ("error", r.stderr.decode("utf-8", "replace").strip())
        return mine(json.loads(r.stdout.decode("utf-8")))

    dumpers = [yaml.SafeDumper] + ([yaml.CSafeDumper] if hasattr(yaml, "CSafeDumper") else [])
    bad = []
    n = 0
    while n < 300:
        obj = value()
        if rnd.random() < 0.3 and isinstance(obj, (list, dict)):
            shared = [1, {"a": [2, 3]}, "s"]            # emitted with anchors and aliases
            obj = {"x": shared, "y": [shared, obj], "z": obj}
        opts = dict(default_flow_style=rnd.choice([None, False, True]), width=rnd.choice([20, 80, 1000]),
                    indent=rnd.choice([2, 4, 7]), allow_unicode=rnd.choice([True, False]),
                    default_style=rnd.choice([None, None, None, '"', "'", "|", ">"]), explicit_start=True,
                    sort_keys=False, line_break=rnd.choice([None, "\r\n"]))
        try:
            text = yaml.dump(obj, Dumper=rnd.choice(dumpers), **opts)
            ref = norm(yaml.load(text, Loader=Loader))
        except yaml.YAMLError:
            continue
        n += 1
        got = parsed(text, "\r\n" if opts["line_break"] else "\n")
        if got != ref:
            bad.append((opts, text, got))
    check(not bad, f"YAML reader agrees with PyYAML on 300 random documents ({len(bad)} differ"
                   + (f"; first: {bad[0][0]}\n{bad[0][1][:800]}\n-> {str(bad[0][2])[:300]}" if bad else "") + ")")

    cases = {
        "comments and blank lines": "# top\n---\na: 1   # trailing\n\n# between\nb:\n  # inside\n  - x\n\n  - y # c\n",
        "sequence at the key's indentation": "---\nk:\n- 1\n- 2\nm:\n- a: 1\n  b: 2\n- - nested\n  - seq\n",
        "tags and handles": "%TAG !e! tag:example.org,2000:\n--- !e!root\na: !!str 123\nb: !!float 3\nc: !e!x {p: 1}\n"
                            "d: !<tag:verbatim:x> [1, 2]\ne: !!int \"42\"\nf: !local value\n",
        "anchors and aliases": "---\nbase: &b {x: 1, y: [1, 2]}\nagain: *b\nlist: &l\n- 1\n- 2\ncopy: *l\ns: &s text\nt: *s\n",
        "block scalars": "---\nlit: |\n  line one\n    indented\n\n  after blank\nfold: >\n  folded\n  text\n\n  para\n"
                         "keep: |+\n  kept\n\nstrip: >-\n  stripped\nind: |2\n   one extra space\n",
        "multi-line flow": "---\na: [1, 2,\n  3, {x: 1,\n       y: [a, b]},\n  'q']\nb: {k: v, 'k2': \"v2\",\n  k3: [  ]}\n",
        "multi-line scalars": "---\nplain: first\n  second\n  third\n\n  after blank\nsingle: 'it''s\n  folded'\n"
                              "double: \"esc \\t tab \\u00e9 \\x41 \\\n  continued\"\n",
        "complex keys": "---\n? [1, 2]\n: pair\n? plain key\n: value\n? no value\n",
        "complex scalar keys": "---\n? plain key\n: value\n? no value\n? |\n  block key\n: - a\n  - b\n",
        "document end and second document": "---\na: 1\n...\n---\nb: 2\n",
        "comments after properties": "---\nk:\n- &a # note: v\n    - x\n- y\nimg: !!map # block 0: the frame\n  source: 0\n"
                                     "seq: !!seq # items: two\n- 1\n- 2\n",
        "escaped white space at a fold": '---\na: "x\\t\n  y"\nb: "one\\ \n  two"\nc: "one\\\n\n  two"\nd: "p \n\n  q"\n',
        "comment after a closing bracket or quote": "---\na: [1, 2]# c\nd: 'e'#f\ng: \"h\"# i\nj: {k: 1}# l\n",
        "floats beyond the double range": "---\n- 1.0e+400\n- -1.0e+400\n- 5.0e-324\n- 1.0e-400\n",
        "alias as a flow key": "---\n- &a x\n- {*a : 1}\n- [*a, *a]\n- &b-1_c y\n- *b-1_c\n",
        "numbers": "---\n- 0x1F\n- 0b101\n- 017\n- 1_000\n- +12\n- 190:20:30\n- 1.5e+3\n- 1.5e3\n- 1e3\n- .5\n- -.5\n- 5.\n"
                   "- .inf\n- -.Inf\n- .NAN\n- 1:30.5\n- ~\n- Null\n- yes\n- Off\n- TRUE\n- y\n- n\n- 0o17\n- 1__0.0_1\n",
    }
    for name, text in cases.items():
        got = parsed(text)
        try:   # PyYAML itself cannot build custom tags or unhashable keys
            ref = norm(yaml.load(text.split("\n...\n")[0] + "\n", Loader=Loader))
        except yaml.YAMLError:
            ref = None
        if ref is not None:
            check(got == ref, f"YAML {name}: {str(got)[:300]} vs PyYAML {str(ref)[:300]}")
        else:
            check(got[0] == "m", f"YAML {name}: parsed ({str(got)[:200]})")
    # Tags: the JSON dump carries the tags of mappings.
    path = os.path.join(TMP, "yaml_tags.asdf")
    write_asdf_raw(path, "a: !core/software-1.0.0 {name: x}\nb: !<tag:verbatim:t>\n  k: v\nc: !!map {k: v}\n", [])
    out = run(path, "--asdf-tree-json").stdout
    for tag in ("tag:stsci.edu:asdf/core/asdf-1.1.0", "tag:stsci.edu:asdf/core/software-1.0.0", "tag:verbatim:t",
                "tag:yaml.org,2002:map"):
        check(f'"t":"{tag}"' in out, f"YAML tag {tag} resolved")
    for name, text, msg in [("unknown alias", "---\na: *nope\n", "unknown anchor"),
                            ("unterminated flow", "---\na: [1, 2\n", "flow"),
                            ("unterminated string", "---\na: \"abc\n", "unterminated"),
                            ("bad indentation", "---\na:\n    b: 1\n  c: 2\n", "indentation"),
                            ("undefined handle", "---\na: !x!y 1\n", "handle"),
                            ("too deep", "---\n" + "[" * 500 + "]" * 500 + "\n", "deep")]:
        got = parsed(text)
        check(got[0] == "error" and msg in got[1], f"YAML error for {name}: {str(got)[:200]}")


def test_asdf_hand_written():
    """ASDF files assembled byte by byte: block layout options, compression, checksums, damage."""
    d = os.path.join(TMP, "asdfraw")
    os.makedirs(d, exist_ok=True)
    rng = np.random.default_rng(71)
    a = rng.integers(0, 65535, (3, 5, 7), dtype=np.uint16)
    tree = ("fits: !fits/fits-1.0.0\n- header:\n  - [SIMPLE, true, conforms]\n  - [BITPIX, 16]\n  - [NAXIS, 3]\n"
            "  - [EXTNAME, sci image, name]\n  - [OBJECT, M 31, target]\n  - [EXPTIME, 300.5, seconds]\n  - [GAIN, 120]\n"
            "  - [FLAG, true]\n  - [\"NO\", false]\n  - [PSF, 1.7870e+04, flux]\n"
            "  - [DATE-OBS, '2026-08-12T01:02:13.428']\n  - [NUMSTR, '123']\n  - [UNDEF]\n"
            "  - [NULLV, null, undefined value]\n  - [HISTORY, processed with something]\n  - [COMMENT, a comment]\n"
            "  - [HIERARCH ESO DET NAME, abc, long name]\n  - [lower case key, 17]\n  - [BZERO, 32768]\n"
            "  - [CPLX, !core/complex-1.0.0 1.5-2.5j]\n  - [NEGZ, -0.5]\n  - [BIGF, 6.02e+23]\n  - []\n"
            "  - [ROWORDER, TOP-DOWN]\n"
            "  data: !core/ndarray-1.0.0\n    source: 0\n    datatype: uint16\n    byteorder: big\n    shape: [3, 5, 7]\n")
    p = os.path.join(d, "stsci.asdf")
    # old FITS tag, big-endian data, long block header, padding after the tree and in the block, CRLF, no index
    write_asdf_raw(p, tree, [asdf_block(a.astype(">u2").tobytes(), header_size=56, extra_alloc=13)],
                   pad=b"\0" * 37, newline="\r\n")
    out = os.path.join(d, "stsci.fits")
    r = run(p, "-o", out, "-f")
    check("rows top-down (kept)" in r.stderr, f"ASDF -> FITS reports the row order: {r.stderr.strip()}")
    with fits.open(out) as h:
        compare("hand-written ASDF -> FITS pixels", np.array(h[0].data), a)
        hd = h[0].header
        want = {"EXTNAME": "sci image", "OBJECT": "M 31", "EXPTIME": 300.5, "GAIN": 120, "FLAG": True, "NO": False,
                "PSF": 17870.0, "DATE-OBS": "2026-08-12T01:02:13.428", "NUMSTR": "123", "ESO DET NAME": "abc",
                "lower case key": 17, "ROWORDER": "TOP-DOWN", "CPLX": complex(1.5, -2.5), "NEGZ": -0.5, "BIGF": 6.02e23,
                "BZERO": 32768}
        for k, v in want.items():
            check(k in hd and hd[k] == v and type(hd[k]) is type(v), f"ASDF header {k}: {hd.get(k)!r} vs {v!r}")
        check(hd.comments["OBJECT"] == "target" and hd.comments["ESO DET NAME"] == "long name", "ASDF header comments")
        check(undefined(hd["UNDEF"]) and undefined(hd["NULLV"]) and hd.comments["NULLV"] == "undefined value",
              "undefined values stay undefined")
        check("processed with something" in str(hd["HISTORY"]) and "a comment" in str(hd["COMMENT"]), "commentary cards")
    if HAVE_FITSVERIFY:
        v = subprocess.run(["fitsverify", "-q", out], capture_output=True, text=True)
        check("0 errors" in v.stdout, f"fitsverify on ASDF -> FITS: {v.stdout.strip()}")
    x = os.path.join(d, "stsci.xisf")
    run(p, "-o", x, "-f", "-q")
    compare("hand-written ASDF -> XISF (top-down rows are not flipped)", read_xisf_any(x), np.transpose(a, (1, 2, 0)))
    info = run(p, "--info").stdout
    check("standard 1.5.0, 1 binary block(s)" in info and 'fits[0].data "sci image": 7 x 5 x 3, uint16, big-endian, block 0'
          in info and "top-down (ROWORDER)" in info, f"--info on ASDF: {info[:300]}")
    dump = subprocess.run([EXE, p, "--dump-header"], capture_output=True).stdout   # bytes: the tree as stored
    check(dump.startswith(b"%YAML 1.1\r\n%TAG ! tag:stsci.edu:asdf/\r\n--- ") and dump.endswith(b"\r\n...\r\n"),
          f"--dump-header prints the tree byte for byte: {dump[:40]!r}")

    # generic arrays: two arrays in one block (offset), explicit C strides, an alias, a streamed block
    f32 = rng.random((4, 6), dtype=np.float32)
    i16 = rng.integers(-1000, 1000, (2, 4, 6)).astype(np.int16)
    stream = rng.integers(0, 255, (9, 6), dtype=np.uint8)
    tree = ("meta: {instrument: cam, exposure: 30.5}\n"
            "first: &arr !core/ndarray-1.0.0 {source: 0, datatype: float32, byteorder: little, shape: [4, 6], offset: 16}\n"
            "again: *arr\n"
            "group:\n  second: !core/ndarray-1.0.0\n    source: 1\n    datatype: int16\n    byteorder: big\n"
            "    shape: [2, 4, 6]\n    strides: [48, 12, 2]\n"
            "  small: !core/ndarray-1.0.0 {source: 0, datatype: float32, byteorder: little, shape: [4]}\n"
            "rows: !core/ndarray-1.0.0 {source: 2, datatype: uint8, byteorder: little, shape: ['*', 6]}\n")
    p = os.path.join(d, "generic.asdf")
    write_asdf_raw(p, tree, [asdf_block(b"\xff" * 16 + f32.tobytes()), asdf_block(i16.astype(">i2").tobytes(), b"zlib"),
                             asdf_block(stream.tobytes(), flags=1)])
    out = os.path.join(d, "generic.fits")
    r = run(p, "-o", out, "-f")
    with fits.open(out) as h:
        check(len(h) == 3 and [x.header["EXTNAME"] for x in h] == ["first", "group.second", "rows"],
              f"generic arrays become HDUs named by their tree path: {[x.header.get('EXTNAME') for x in h]}")
        compare("array at an offset in its block", np.array(h[0].data), f32)
        compare("big-endian int16 array in a zlib block", np.array(h[1].data).astype(np.int16), i16)
        compare("streamed block", np.array(h[2].data), stream)
    check("int16 with negative values -> Float32" in r.stderr, "signed data with negative values is reported")
    x = os.path.join(d, "generic.xisf")
    r = run(p, "-o", x, "-f", "-i", "0")
    compare("generic ASDF array -> XISF (rows assumed bottom-up)", read_xisf_any(x), f32[::-1, :, None])
    check("assumed bottom-up" in r.stderr, "the assumption about the row order is reported")
    run(p, "-o", x, "-f", "-q", "-i", "0", "--top-down")
    compare("generic ASDF array -> XISF with --top-down", read_xisf_any(x), f32[:, :, None])
    r = run(p, "-o", x, "-f", "-i", "7", expect_ok=False)
    check(r.returncode == 1 and "out of range" in r.stderr, "ASDF image index out of range")

    # [rows, columns, channels] arrays are recognized
    rgb = rng.integers(0, 255, (6, 8, 3), dtype=np.uint8)
    p = os.path.join(d, "rgb.asdf")
    write_asdf_raw(p, "img: !core/ndarray-1.0.0 {source: 0, datatype: uint8, byteorder: little, shape: [6, 8, 3]}\n",
                   [asdf_block(rgb.tobytes())])
    run(p, "-o", x, "-f", "-q", "--top-down")
    compare("[rows, columns, channels] array", read_xisf_any(x), rgb)

    # a data product in the style of mission pipelines: custom tags, arrays next to metadata,
    # one of them wrapped in a quantity
    sci = rng.random((6, 9), dtype=np.float32)
    dq = rng.integers(0, 2 ** 31, (6, 9), dtype=np.uint32)
    err = rng.random((6, 9)).astype(np.float16)
    tree = ("history:\n  extensions:\n  - !core/extension_metadata-1.0.0\n    extension_class: some.Extension\n"
            "    software: !core/software-1.0.0 {name: pipeline, version: 1.2.3}\n"
            "roman: !<asdf://stsci.edu/datamodels/roman/tags/wfi_image-1.0.0>\n"
            "  meta: !<asdf://example.org/tags/meta-1.0.0>\n    exposure: {start_time: !time/time-1.1.0 2026-01-01T00:00:00.000, "
            "type: WFI_IMAGE}\n    wcs: !<tag:stsci.edu:gwcs/wcs-1.2.0>\n      name: w\n      steps: []\n"
            "  data: !unit/quantity-1.1.0\n    unit: !unit/unit-1.0.0 DN / s\n"
            "    value: !core/ndarray-1.1.0\n      source: 0\n      datatype: float32\n      byteorder: little\n"
            "      shape: [6, 9]\n"
            "  dq: !core/ndarray-1.1.0 {source: 1, datatype: uint32, byteorder: little, shape: [6, 9]}\n"
            "  err: !core/ndarray-1.1.0 {source: 2, datatype: float16, byteorder: little, shape: [6, 9]}\n")
    p = os.path.join(d, "product.asdf")
    codec = b"lz4" if lz4 else b"zlib"
    write_asdf_raw(p, tree, [asdf_block(sci.tobytes(), codec), asdf_block(dq.tobytes(), codec), asdf_block(err.tobytes())],
                   head=ASDF_HEAD.replace("1.5.0", "1.6.0"))
    r = run(p, "-o", out, "-f")
    with fits.open(out) as h:
        check([x.header["EXTNAME"] for x in h] == ["roman.data.value", "roman.dq", "roman.err"],
              f"data product: arrays found under custom tags: {[x.header.get('EXTNAME') for x in h]}")
        compare("data product: science array", np.array(h[0].data), sci)
        compare("data product: uint32 array", np.array(h[1].data), dq)
        compare("data product: float16 array", np.array(h[2].data), err.astype(np.float32))
    run(p, "-o", x, "-f", "-q", "-i", "0")
    compare("data product -> XISF", read_xisf_any(x), sci[::-1, :, None])

    # compression codecs
    data = rng.integers(0, 4000, (40, 50), dtype=np.uint16)
    one = "img: !core/ndarray-1.0.0 {source: 0, datatype: uint16, byteorder: little, shape: [40, 50]}\n"
    codecs = [b"", b"zlib"] + ([b"lz4"] if lz4 else []) + ([b"zstd"] if zstandard and ZSTD_BUILD else [])
    for codec in codecs:
        p = os.path.join(d, f"codec_{codec.decode() or 'none'}.asdf")
        write_asdf_raw(p, one, [asdf_block(data.tobytes(), codec)])
        run(p, "-o", out, "-f", "-q")
        compare(f"block compression {codec.decode() or 'none'}", np.array(fits.getdata(out)), data)
    for label, codec, msg in [("bzp2", b"bzp2", "bzip2 compression is not supported"), ("blsc", b"blsc", "not supported")]:
        p = os.path.join(d, f"codec_{label}.asdf")
        write_asdf_raw(p, one, [asdf_block(data.tobytes(), codec)])
        r = run(p, "-o", out, "-f", expect_ok=False)
        check(r.returncode == 1 and msg in r.stderr, f"{label} block: {r.stderr.strip()}")

    # checksums
    p = os.path.join(d, "badsum.asdf")
    write_asdf_raw(p, one, [asdf_block(data.tobytes(), checksum="bad")])
    r = run(p, "-o", out, "-f", expect_ok=False)
    check(r.returncode == 1 and "MD5 checksum mismatch" in r.stderr, "wrong MD5 checksum is reported")
    check(not os.path.exists(out + ".part"), "no partial file after a checksum error")
    run(p, "-o", out, "-f", "-q", "--no-verify")
    compare("--no-verify reads a block with a wrong checksum", np.array(fits.getdata(out)), data)
    write_asdf_raw(p, one, [asdf_block(data.tobytes(), b"zlib", checksum="bad")])
    r = run(p, "-o", out, "-f", expect_ok=False)
    check(r.returncode == 1 and "MD5 checksum mismatch" in r.stderr, "wrong MD5 checksum of a compressed block")
    write_asdf_raw(p, one, [asdf_block(data.tobytes(), checksum="zero")])
    run(p, "-o", out, "-f", "-q")
    compare("block without checksum", np.array(fits.getdata(out)), data)
    # asdf 2.x stored the MD5 of the uncompressed data in compressed blocks
    write_asdf_raw(p, one, [asdf_block(data.tobytes(), b"zlib", checksum="uncompressed")])
    run(p, "-o", out, "-f", "-q")
    compare("compressed block with the checksum convention of asdf 2.x", np.array(fits.getdata(out)), data)

    # damaged and unusual files
    good = asdf_block(data.tobytes())
    bad_cases = [
        ("truncated block", one, [good[:-100]], "beyond the end of the file"),
        ("missing block", one.replace("source: 0", "source: 3"), [good], "the file has 1 block(s)"),
        ("array larger than its block", one.replace("[40, 50]", "[41, 50]"), [good], "holds 4000"),
        ("corrupt zlib data", one, [asdf_block(data.tobytes(), b"zlib", data_size=5000)], "block 0"),
        ("unterminated tree", None, [], "end of the YAML tree was not found"),
        ("YAML error", "a: [1, 2\n", [good], "YAML"),
        ("no arrays", "a: 1\n", [], "no image data found in this ASDF file"),
        ("inline array", "img: !core/ndarray-1.0.0 {data: [[1, 2], [3, 4]], datatype: int64, shape: [2, 2]}\n", [],
         "no image data"),
        ("external array", one.replace("source: 0", "source: other.asdf"), [good], "no image data"),
        ("complex data", one.replace("uint16", "complex64").replace("[40, 50]", "[5, 50]"), [good], "no image data"),
        ("bad shape", one.replace("[40, 50]", "[40, x]"), [good], "invalid shape"),
    ]
    for name, t, blocks, msg in bad_cases:
        p = os.path.join(d, "bad.asdf")
        if t is None:
            open(p, "wb").write((ASDF_HEAD + "a: 1\n").encode())
        else:
            write_asdf_raw(p, t, blocks)
        r = run(p, "-o", out, "-f", expect_ok=False)
        check(r.returncode == 1 and msg in r.stderr, f"{name}: {r.stderr.strip()[:200]}")
    open(p, "wb").write(b"#ASDF 1.0.0\n#ASDF_STANDARD 1.5.0\n" + good)
    r = run(p, "-o", out, "-f", expect_ok=False)
    check(r.returncode == 1 and "has no tree" in r.stderr, f"file without a tree: {r.stderr.strip()}")
    p = os.path.join(d, "skips.asdf")
    write_asdf_raw(p, one + "view: !core/ndarray-1.0.0 {source: 0, datatype: uint16, byteorder: little, shape: [20, 50], "
                   "strides: [200, 2]}\ncube: !core/ndarray-1.0.0 {source: 0, datatype: uint8, byteorder: little, "
                   "shape: [2, 2, 20, 50]}\n", [good])
    r = run(p, "-o", out, "-f")
    check("skipped view: " in r.stderr and "strides" in r.stderr and "skipped cube: 4-dimensional" in r.stderr,
          f"unreadable arrays are named: {r.stderr.strip()[:300]}")
    r = run(out, "--asdf-tree-json", "-f", expect_ok=False)
    check(r.returncode == 1 and "needs an ASDF file" in r.stderr, "--asdf-tree-json refuses other formats")
    # refusals
    for args, msg in [(["-t", "asdf"], "already an ASDF"), (["-s"], "--stretch"), (["-t", "fits", "-s"], "--stretch")]:
        r = run(p, "-f", *args, expect_ok=False)
        check(r.returncode == 1 and msg in r.stderr, f"ASDF input with {args}: {r.stderr.strip()}")
    run(p, "-f", "-q")
    check(os.path.exists(os.path.join(d, "skips.xisf")), "ASDF input is converted to XISF by default")
    r = run(p, expect_ok=False)
    check(r.returncode == 1 and "already exists" in r.stderr, "ASDF input: refuses to overwrite without --force")


def with_id(entry, name):
    entry["attrs"]["id"] = name
    return entry


def torture_keywords():
    return "".join([
        '<FITSKeyword name="SIMPLE" value="T" comment="should be dropped"/>',
        '<FITSKeyword name="OBJECT" value="\'M 31 &amp; friends\'" comment="target"/>',
        '<FITSKeyword name="OBSERVER" value="\'O\'\'Brien\'" comment="quote inside"/>',
        '<FITSKeyword name="DQUOTE" value="\'say &quot;hi&quot; \\ there\'" comment="double quotes, backslash"/>',
        '<FITSKeyword name="GAIN" value="120" comment="sensor gain"/>',
        '<FITSKeyword name="LEADZERO" value="007" comment="not octal"/>',
        '<FITSKeyword name="PLUS" value="+5" comment=""/>',
        '<FITSKeyword name="PSFFLX00" value="1.7870e+04" comment="lower-case exponent"/>',
        '<FITSKeyword name="NODOT" value="1E5" comment="no decimal point"/>',
        '<FITSKeyword name="NOSIGN" value="1.5E5" comment="no exponent sign"/>',
        '<FITSKeyword name="DEXP" value="1.D3" comment="Fortran exponent"/>',
        '<FITSKeyword name="HALF" value=".5" comment=""/>',
        '<FITSKeyword name="NEGHALF" value="-.5" comment=""/>',
        '<FITSKeyword name="YES" value="T" comment="logical"/>',
        '<FITSKeyword name="NO" value="F" comment="logical, and a YAML word"/>',
        '<FITSKeyword name="ON" value="\'off\'" comment="YAML words as text"/>',
        '<FITSKeyword name="NUMSTR" value="\'123\'" comment="a string of digits"/>',
        '<FITSKeyword name="DATE-OBS" value="\'2026-08-12T01:02:13\'" comment="looks like a timestamp"/>',
        '<FITSKeyword name="COLON" value="\'a: b #c [d] {e}, f\'" comment="YAML: syntax, [in] {a} #comment"/>',
        '<FITSKeyword name="UNDEF" value="" comment="no value"/>',
        '<FITSKeyword name="BIGINT" value="9223372036854775807" comment="largest ASDF integer"/>',
        '<FITSKeyword name="HUGEINT" value="9223372036854775808" comment="too large"/>',
        '<FITSKeyword name="CPLX" value="(1.5, -2.5E3)" comment="complex"/>',
        '<FITSKeyword name="UTF" value="\'caf&#233; &#956;m\'" comment="&#956;m pixel"/>',
        '<FITSKeyword name="HISTORY" value="" comment="calibrated with master dark"/>',
        '<FITSKeyword name="COMMENT" value="" comment="' + "x" * 150 + '"/>',
        '<FITSKeyword name="LONGNAMEKEY" value="1.5" comment="needs HIERARCH"/>',
        '<FITSKeyword name="lower" value="2" comment="lower-case name"/>',
        '<FITSKeyword name="NOTES" value="\'' + "y" * 90 + '\'" comment="long"/>',
    ])


def test_asdf_output():
    """XISF -> ASDF, read back with Python's asdf library: no warnings, schema-valid, checksums right,
    and the same HDUs that XISF -> FITS produces."""
    if not HAVE_ASDF:
        skipped.append("ASDF output vs the asdf library (pip install asdf asdf-astropy)")
        return
    import warnings
    d = os.path.join(TMP, "asdfout")
    os.makedirs(d, exist_ok=True)
    codec_sets = [[], ["-c"], ["--codec", "zlib"]] + ([["--codec", "zstd"]] if HAVE_ASDF_ZSTD and ZSTD_BUILD else [])
    n = 0
    for dtype in (np.uint8, np.uint16, np.uint32, np.uint64, np.float32, np.float64):
        for c in (1, 3):
            a = test_image(dtype, 15, 22, c, 80 + c)
            p = os.path.join(d, f"o_{np.dtype(dtype).name}_{c}.xisf")
            kws = ('<FITSKeyword name="OBJECT" value="\'M 31\'" comment="target"/>'
                   '<FITSKeyword name="EXPTIME" value="300.5" comment="seconds"/>'
                   '<FITSKeyword name="GAIN" value="120" comment="sensor gain"/>'
                   '<FITSKeyword name="HISTORY" value="" comment="calibrated"/>')
            write_xisf(p, [image_entry(a, children=kws, codec="zlib", shuffle_item=np.dtype(dtype).itemsize)])
            for row in ([], ["--top-down"]):
                flags = codec_sets[n % len(codec_sets)]
                n += 1
                out, ref = p + ".asdf", p + ".fits"
                run(p, "-o", out, "-f", "-q", *row, *flags)
                run(p, "-o", ref, "-f", "-q", *row)
                label = f"XISF -> ASDF {np.dtype(dtype).name} {c}ch {row + flags}"
                try:
                    hdus, lib = open_asdf(out, validate=dtype is not np.uint64)
                except Exception as e:  # noqa: BLE001
                    check(False, f"{label}: the asdf library rejects the file: {type(e).__name__}: {e}")
                    continue
                data, hd = hdus[0]
                check(data.dtype == np.dtype(dtype), f"{label}: stored as {data.dtype}")
                with fits.open(ref) as h:
                    compare(label + " pixels equal FITS output", data, np.array(h[0].data))
                    check(user_cards(hd) == user_cards(h[0].header),
                          f"{label}: header equals FITS output\n{user_cards(hd)}\n{user_cards(h[0].header)}")
                exp = as_planes(a if row else a[::-1])
                compare(label + " pixels", data, exp[0] if c == 1 else exp)
                check(lib.get("name") == "xisfconv" and str(lib.get("version")) == run("--version").stdout.split()[1],
                      f"asdf_library entry: {lib}")
                raw = open(out, "rb").read()
                want = b"zstd" if "zstd" in flags else b"zlib" if flags else b"\0\0\0\0"
                check(raw[raw.index(b"\xd3BLK") + 10:][:4] == want, f"{label}: block compression label")
                check(raw.rstrip().endswith(b"...") and b"#ASDF BLOCK INDEX" in raw, f"{label}: block index written")

    # keywords of every kind
    a = test_image(np.uint16, 12, 16, 1, 90)
    p = os.path.join(d, "kw.xisf")
    write_xisf(p, [with_id(image_entry(a, children=torture_keywords()), "main_image")])
    out = os.path.join(d, "kw.asdf")
    r = run(p, "-o", out, "-f")
    check("HUGEINT" in r.stderr and "written as a string" in r.stderr, "integer beyond ASDF's range is reported")
    hdus, _ = open_asdf(out)
    hd = hdus[0][1]
    want = {"OBJECT": "M 31 & friends", "OBSERVER": "O'Brien", "DQUOTE": 'say "hi" \\ there', "GAIN": 120, "LEADZERO": 7,
            "PLUS": 5, "PSFFLX00": 17870.0, "NODOT": 1e5, "NOSIGN": 1.5e5, "DEXP": 1000.0, "HALF": 0.5, "NEGHALF": -0.5,
            "YES": True, "NO": False, "ON": "off", "NUMSTR": "123", "DATE-OBS": "2026-08-12T01:02:13",
            "COLON": "a: b #c [d] {e}, f", "BIGINT": 9223372036854775807, "HUGEINT": "9223372036854775808",
            "CPLX": complex(1.5, -2500.0), "UTF": "caf? ?m", "LONGNAMEKEY": 1.5, "LOWER": 2, "NOTES": "y" * 90,
            "ROWORDER": "BOTTOM-UP", "EXTNAME": "main_image", "PROGRAM": "xisfconv " + run("--version").stdout.split()[1]}
    for k, v in want.items():
        check(k in hd and hd[k] == v and type(hd[k]) is type(v), f"ASDF keyword {k}: {hd.get(k)!r} vs {v!r}")
    check(undefined(hd["UNDEF"]) and hd.comments["UNDEF"] == "no value", f"keyword without a value: {hd['UNDEF']!r}")
    check(hd.comments["COLON"] == "YAML: syntax, [in] {a} #comment" and hd.comments["UTF"] == "?m pixel",
          f"ASDF keyword comments: {hd.comments['COLON']!r}")
    check("SIMPLE" in hd and hd.comments["SIMPLE"] != "should be dropped", "structural keywords are not carried over")
    check("calibrated with master dark" in str(hd["HISTORY"]) and "x" * 150 in str(hd["COMMENT"]).replace("\n", ""),
          "commentary keywords in ASDF")
    # astropy can write the HDU list it got as a FITS file
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with asdf.open(out) as af:
            af["fits"].writeto(os.path.join(d, "kw_astropy.fits"), overwrite=True, output_verify="exception")
    with fits.open(os.path.join(d, "kw_astropy.fits")) as h:
        compare("FITS written by astropy from the ASDF HDU list", np.array(h[0].data), a[::-1, :, 0])
        check(h[0].header["LONGNAMEKEY"] == 1.5 and h[0].header["NOTES"] == "y" * 90, "astropy keeps HIERARCH and long strings")
    # the same header through xisfconv's own reader
    back = os.path.join(d, "kw_back.fits")
    run(out, "-o", back, "-f", "-q")
    ref = os.path.join(d, "kw_ref.fits")
    run(p, "-o", ref, "-f", "-q")
    with fits.open(back) as hb, fits.open(ref) as hr:
        cb = [c for c in user_cards(hb[0].header) if c[0] != "HUGEINT" and "from ASDF" not in str(c[1])]
        cr = [c for c in user_cards(hr[0].header) if c[0] != "HUGEINT"]
        check(cb == cr, "XISF -> ASDF -> FITS has the header of XISF -> FITS\n" +
              f"{[c for c in cb if c not in cr]}\n{[c for c in cr if c not in cb]}")

    # several images
    imgs = [test_image(np.uint16, 10, 14, 1, 91), test_image(np.float32, 8, 9, 3, 92), test_image(np.uint8, 6, 7, 1, 93)]
    p = os.path.join(d, "multi.xisf")
    write_xisf(p, [with_id(image_entry(x), f"img{i}") for i, x in enumerate(imgs)])
    out = os.path.join(d, "multi.asdf")
    run(p, "-o", out, "-f", "-q", "-c")
    hdus, _ = open_asdf(out)
    check(len(hdus) == 3 and [h["EXTNAME"] for _, h in hdus] == ["img0", "img1", "img2"], "multi-image XISF -> ASDF HDUs")
    for i, (data, _) in enumerate(hdus):
        exp = as_planes(imgs[i][::-1])
        compare(f"multi-image ASDF HDU {i}", data, exp[0] if exp.shape[0] == 1 else exp)
    run(p, "-t", "asdf", "-f", "-q", "-i", "1")
    hdus, _ = open_asdf(os.path.join(d, "multi.asdf"))
    check(len(hdus) == 1 and hdus[0][1]["EXTNAME"] == "img1", "-t asdf with --image")
    # --bits and --stretch apply as for FITS output
    run(p, "-o", out, "-f", "-q", "-i", "1", "-b", "u16")
    ref = os.path.join(d, "bits.fits")
    run(p, "-o", ref, "-f", "-q", "-i", "1", "-b", "u16")
    hdus, _ = open_asdf(out)
    check(hdus[0][0].dtype == np.uint16, "--bits u16 for ASDF output")
    compare("--bits for ASDF output equals FITS output", hdus[0][0], np.array(fits.getdata(ref)))
    run(p, "-o", out, "-f", "-q", "-i", "1", "--stretch=linked")
    run(p, "-o", ref, "-f", "-q", "-i", "1", "--stretch=linked")
    hdus, _ = open_asdf(out)
    compare("--stretch for ASDF output equals FITS output", hdus[0][0], np.array(fits.getdata(ref)))


def test_asdf_python_files():
    """Files written by Python's asdf library and by asdf-astropy."""
    if not HAVE_ASDF:
        skipped.append("ASDF files written by the asdf library (pip install asdf asdf-astropy)")
        return
    import warnings
    d = os.path.join(TMP, "asdfpy")
    os.makedirs(d, exist_ok=True)
    rng = np.random.default_rng(72)
    base = rng.random((10, 12))
    arrays = {
        "u8": rng.integers(0, 255, (5, 7), dtype=np.uint8), "u16": rng.integers(0, 65535, (5, 7), dtype=np.uint16),
        "u32": rng.integers(0, 2 ** 32 - 1, (5, 7), dtype=np.uint32), "u64": rng.integers(0, 2 ** 63, (5, 7), dtype=np.uint64),
        "i8": rng.integers(-100, 100, (5, 7), dtype=np.int8), "i16pos": rng.integers(0, 30000, (5, 7), dtype=np.int16),
        "i16neg": rng.integers(-30000, 30000, (5, 7), dtype=np.int16),
        "i32": rng.integers(-2 ** 31, 2 ** 31 - 1, (5, 7), dtype=np.int32),
        "i64": rng.integers(0, 2 ** 62, (5, 7), dtype=np.int64), "f16": rng.random((5, 7)).astype(np.float16),
        "f32": rng.random((5, 7), dtype=np.float32), "f64": rng.random((3, 5, 7)),
        "be": rng.random((5, 7)).astype(">f4"), "be16": rng.integers(0, 65535, (2, 5, 7)).astype(">u2"),
        "rgb": rng.integers(0, 255, (6, 8, 3), dtype=np.uint8), "lead1": rng.random((1, 1, 5, 7)).astype(np.float32),
        "rows": base[4:8],
    }
    skips = {"cube4": rng.random((2, 3, 5, 7)), "bool": rng.random((5, 7)) > 0.5,
             "cplx": rng.random((5, 7)).astype(np.complex64), "view": base[2:7, 3:10],
             "fortran": np.asfortranarray(rng.random((5, 7)))}
    tree = {"meta": {"instrument": "cam", "exposure": 30.5, "n": 3, "flag": True, "note": None, "list": [1, 2, 3],
                     "text": "multi\nline text", "long": "word " * 60},
            "vec": np.arange(10.0), "nested": {"deep": [{"img": arrays["u16"]}]}, **arrays, **skips}
    expected = dict(arrays)
    expected["nested.deep[0].img"] = expected.pop("u16")   # the shared array appears once, at its first place
    expected["rgb"] = arrays["rgb"].transpose(2, 0, 1)
    expected["lead1"] = arrays["lead1"][0, 0]
    variants = [("none", {}), ("zlib", {"all_array_compression": "zlib"})]
    if lz4:
        variants.append(("lz4", {"all_array_compression": "lz4"}))
    if HAVE_ASDF_ZSTD and ZSTD_BUILD:
        variants.append(("zstd", {"all_array_compression": "zstd"}))
    out = os.path.join(d, "gen.fits")
    for name, kw in variants:
        p = os.path.join(d, f"gen_{name}.asdf")
        asdf.AsdfFile(tree).write_to(p, **kw)
        r = run(p, "-o", out, "-f")
        with fits.open(out) as h:
            names = [x.header["EXTNAME"] for x in h]
            check(sorted(names) == sorted(expected), f"asdf-written file ({name}): images {names}")
            for x in h:
                exp = expected.get(x.header["EXTNAME"])
                if exp is not None:
                    got = np.array(x.data)
                    check(got.shape == exp.shape and np.array_equal(got.astype(np.float64), exp.astype(np.float64)),
                          f"asdf-written file ({name}): array {x.header['EXTNAME']} ({exp.dtype} -> {got.dtype})")
        for key in ("bool", "cplx", "cube4", "view", "fortran"):
            check(f"skipped {key}: " in r.stderr, f"asdf-written file ({name}): {key} is reported as skipped")
        check("skipped vec" not in r.stderr and "skipped meta" not in r.stderr, "arrays that are no images are passed over")
    info = run(os.path.join(d, "gen_zlib.asdf"), "--info").stdout
    check("Image 0 at be: 7 x 5 x 1, float32, big-endian, block" in info and "zlib compressed" in info and
          "assumed bottom-up" in info and "Skipped bool: datatype bool8" in info, f"--info on a generic file: {info[:200]}")
    p = os.path.join(d, "gen_bz2.asdf")
    asdf.AsdfFile(tree).write_to(p, all_array_compression="bzp2")
    r = run(p, "-o", out, "-f", expect_ok=False)
    check(r.returncode == 1 and "bzip2 compression is not supported" in r.stderr, "bzip2 block from the asdf library")
    # a stream, as the library writes it: source -1, first axis open, data appended after the tree
    try:
        from asdf.tags.core import Stream
    except ImportError:  # pragma: no cover
        Stream = None
    if Stream is not None:
        p = os.path.join(d, "stream.asdf")
        rows = rng.integers(0, 255, (9, 6), dtype=np.uint8)
        with open(p, "wb") as fd:
            asdf.AsdfFile({"img": arrays["u16"], "stream": Stream([6], np.uint8)}).write_to(fd)
            fd.write(rows.tobytes())
        run(p, "-o", out, "-f", "-q")
        with fits.open(out) as h:
            check([x.header["EXTNAME"] for x in h] == ["img", "stream"], "streamed array written by the asdf library is found")
            compare("streamed array written by the asdf library", np.array(h[-1].data), rows)
    p = os.path.join(d, "gen_inline.asdf")
    asdf.AsdfFile({"img": arrays["u16"]}).write_to(p, all_array_storage="inline")
    r = run(p, "-o", out, "-f", expect_ok=False)
    check(r.returncode == 1 and "stored inline" in r.stderr and "no image data" in r.stderr, "inline arrays are explained")

    # an HDU list serialized by asdf-astropy (its newest tag and standard version, big-endian data)
    hd = fits.Header()
    hd["OBJECT"] = ("M 31", "target")
    hd["EXPTIME"] = (300.5, "[s] exposure")
    hd["GAIN"] = 120
    hd["FLAG"] = True
    hd["SMALL"] = 1.5e-12
    hd["QUOTE"] = "it's a 'test'"
    hd["YES"] = "yes"
    hd["NUMSTR"] = "123"
    hd["DATE-OBS"] = "2026-08-12T01:02:13.428"
    hd["LONGSTR"] = "x" * 150 + " end"
    hd["HIERARCH ESO DET CHIP1 NAME"] = ("abc", "eso")
    hd["HIERARCH lower case key"] = 17
    hd["CPLX"] = complex(1.5, -2.5)
    hd["UNDEF"] = fits.card.Undefined()
    hd.add_comment("a comment")
    hd.add_history("history line one")
    hd.add_history("h" * 100)
    d0 = rng.integers(0, 65535, (5, 7), dtype=np.uint16)
    d1 = rng.random((3, 4, 6)).astype(np.float32)
    d2 = rng.integers(-1000, 1000, (4, 6)).astype(np.int32)
    h1 = fits.Header()
    h1["EXTNAME"] = "second"
    h1["BUNIT"] = "adu"
    src = os.path.join(d, "hl.fits")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        fits.HDUList([fits.PrimaryHDU(d0, hd), fits.ImageHDU(d1, h1), fits.ImageHDU(d2)]).writeto(src, overwrite=True)
        with fits.open(src) as orig:
            for name, kw in [("plain", {}), ("zlib", {"all_array_compression": "zlib"})]:
                p = os.path.join(d, f"hl_astropy_{name}.asdf")
                asdf.AsdfFile({"hdul": orig, "other": {"x": 1}}).write_to(p, **kw)
                back = os.path.join(d, "hl_back.fits")
                r = run(p, "-o", back, "-f")
                with fits.open(back) as b:
                    check(len(b) == 3, f"asdf-astropy HDU list ({name}): 3 HDUs")
                    for i in range(3):
                        compare(f"asdf-astropy HDU list ({name}) HDU {i} pixels", np.array(b[i].data).astype(np.float64),
                                np.array(orig[i].data).astype(np.float64))
                    cb = [c for c in user_cards(b[0].header) if c[0] not in ("ROWORDER", "PROGRAM") and "xisfconv" not in str(c[1])]
                    check(cb == user_cards(orig[0].header),
                          f"asdf-astropy HDU list ({name}): header\n{[c for c in cb if c not in user_cards(orig[0].header)]}\n"
                          f"{[c for c in user_cards(orig[0].header) if c not in cb]}")
                    check(b[1].header["EXTNAME"] == "second" and b[1].header["BUNIT"] == "adu", "extension header")
                check("hdul[0].data" in r.stderr and "rows bottom-up (kept)" in r.stderr, "HDU list rows are bottom-up")
                x = os.path.join(d, "hl.xisf")
                run(p, "-o", x, "-f", "-q")
                compare(f"asdf-astropy HDU list ({name}) -> XISF", read_xisf_any(x), d0[::-1, :, None])
                compare(f"asdf-astropy HDU list ({name}) -> XISF, HDU 1", read_xisf_any(x, 1),
                        np.transpose(d1, (1, 2, 0))[::-1])

            # FITS -> ASDF by xisfconv, read by the asdf library
            for flags in ([], ["-c"]):
                p = os.path.join(d, "hl_x.asdf")
                run(src, "-o", p, "-f", "-q", *flags)
                hdus, _ = open_asdf(p)
                check(len(hdus) == 3, "FITS -> ASDF: 3 HDUs")
                for i, (data, header) in enumerate(hdus):
                    compare(f"FITS -> ASDF {flags} HDU {i} pixels", data.astype(np.float64),
                            np.array(orig[i].data).astype(np.float64))
                    got = [c for c in user_cards(header) if c[0] not in ("ROWORDER", "PROGRAM", "EXTNAME") and
                           "xisfconv" not in str(c[1])]
                    exp = [c for c in user_cards(orig[i].header) if c[0] != "EXTNAME"]
                    check(got == exp, f"FITS -> ASDF {flags} HDU {i}: header\n{[c for c in got if c not in exp]}\n"
                                      f"{[c for c in exp if c not in got]}")
                check(hdus[0][0].dtype == np.uint16 and hdus[1][1]["EXTNAME"] == "second" and
                      hdus[0][1]["ROWORDER"] == "BOTTOM-UP", "FITS -> ASDF: types, names and row order")


def test_asdf_roundtrips():
    """xisfconv reading its own ASDF output."""
    d = os.path.join(TMP, "asdfrt")
    os.makedirs(d, exist_ok=True)
    n = 0
    for dtype in (np.uint8, np.uint16, np.uint32, np.uint64, np.float32, np.float64):
        for c in (1, 3):
            a = test_image(dtype, 15, 22, c, 60 + c)
            p = os.path.join(d, f"rt_{np.dtype(dtype).name}_{c}.xisf")
            kws = '<FITSKeyword name="OBJECT" value="\'M 31\'" comment="target"/>'
            write_xisf(p, [image_entry(a, children=kws, codec="zlib", shuffle_item=np.dtype(dtype).itemsize)])
            for row in ([], ["--top-down"]):
                comp = [[], ["-c"]][n % 2]
                n += 1
                mid, back = p + ".asdf", p + ".back.xisf"
                run(p, "-o", mid, "-f", "-q", *row, *comp)
                run(mid, "-o", back, "-f", "-q", "-c")
                got = read_xisf_any(back)
                check(got.dtype == np.dtype(dtype), f"XISF -> ASDF -> XISF {np.dtype(dtype).name}: dtype {got.dtype}")
                compare(f"XISF -> ASDF{row + comp} -> XISF {np.dtype(dtype).name} {c}ch", got, a)
                kw = xisf_keyword_values(back)
                check(unq(kw.get("OBJECT", "")).strip() == "M 31", f"keyword survives XISF -> ASDF -> XISF: {kw.get('OBJECT')}")
                # ASDF -> FITS is the file XISF -> FITS gives
                f1, f2 = p + ".a.fits", p + ".x.fits"
                run(mid, "-o", f1, "-f", "-q")
                run(p, "-o", f2, "-f", "-q", *row)
                with fits.open(f1) as h1, fits.open(f2) as h2:
                    compare("ASDF -> FITS pixels equal XISF -> FITS", np.array(h1[0].data), np.array(h2[0].data))
                    check(h1[0].header["ROWORDER"] == h2[0].header["ROWORDER"], "ASDF -> FITS keeps ROWORDER")
                    c1 = [x for x in user_cards(h1[0].header) if "from ASDF" not in str(x[1])]
                    check(c1 == user_cards(h2[0].header), "ASDF -> FITS header equals XISF -> FITS")
                # FITS -> ASDF -> FITS
                mid2, f3 = p + ".f.asdf", p + ".f.fits"
                run(f2, "-o", mid2, "-f", "-q", *comp)
                run(mid2, "-o", f3, "-f", "-q")
                with fits.open(f3) as h3, fits.open(f2) as h2:
                    compare("FITS -> ASDF -> FITS pixels", np.array(h3[0].data), np.array(h2[0].data))
                    c3 = [x for x in user_cards(h3[0].header) if "xisfconv" not in str(x[1]) or x[0] != "HISTORY"]
                    c2 = [x for x in user_cards(h2[0].header) if "xisfconv" not in str(x[1]) or x[0] != "HISTORY"]
                    check(c3 == c2, "FITS -> ASDF -> FITS header")
    if HAVE_FITSVERIFY:
        v = subprocess.run(["fitsverify", "-q", f3], capture_output=True, text=True)
        check("verification OK" in v.stdout, f"fitsverify on FITS -> ASDF -> FITS: {v.stdout.strip()}")

    # --bits and the row order options when repackaging
    a = test_image(np.float32, 12, 16, 1, 66)
    f = os.path.join(d, "bits.fits")
    fits.PrimaryHDU(a[:, :, 0]).writeto(f, overwrite=True)
    mid = os.path.join(d, "bits.asdf")
    r = run(f, "-o", mid, "-f", "-b", "u16", "--top-down")
    back = os.path.join(d, "bits_back.fits")
    run(mid, "-o", back, "-f", "-q")
    with fits.open(back) as h:
        compare("FITS -> ASDF --bits u16", np.array(h[0].data), np.round(a[:, :, 0].astype(np.float64) * 65535).astype(np.uint16))
        check(h[0].header["ROWORDER"] == "TOP-DOWN" and "rows top-down (kept)" in r.stderr, "--top-down marks the stored rows")
    x = os.path.join(d, "bits.xisf")
    run(mid, "-o", x, "-f", "-q")
    compare("top-down ASDF -> XISF is not flipped", read_xisf_any(x),
            np.round(a.astype(np.float64) * 65535).astype(np.uint16))
    # output naming
    run(f, "-t", "asdf", "-f", "-q", "-d", d)
    check(os.path.exists(os.path.join(d, "bits.asdf")), "-t asdf names the output .asdf")
    r = run(f, "-o", os.path.join(d, "x.asdf"), "--codec", "zstd", "-f", "-q", expect_ok=False)
    check((r.returncode == 0) == ZSTD_BUILD, "--codec zstd for ASDF output follows the build")

    # WCS: a PixInsight solution becomes the same keywords in ASDF as in FITS
    P = "PCL:AstrometricSolution:"
    props = "".join([
        f'<Property id="{P}ProjectionSystem" type="String">Gnomonic</Property>',
        f64_prop(P + "ReferenceCelestialCoordinates", [328.178, 47.358]),
        f64_prop(P + "ReferenceImageCoordinates", [300.3, 199.3]),
        f64_prop(P + "ReferenceNativeCoordinates", [0, 90]),
        f64_prop(P + "CelestialPoleNativeCoordinates", [180, 90]),
        f64_prop(P + "LinearTransformationMatrix", [-2.3565e-4, 1.1696e-5, -1.1715e-5, -2.3575e-4], 2, 2),
    ])
    a = test_image(np.uint16, 400, 600, 1, 67)
    p = os.path.join(d, "wcs.xisf")
    write_xisf(p, [image_entry(a, children=props)])
    for row in ([], ["--top-down"]):
        mid, ref, viaf = os.path.join(d, "wcs.asdf"), os.path.join(d, "wcs.fits"), os.path.join(d, "wcs_via.fits")
        r = run(p, "-o", mid, "-f", *row)
        check("WCS TAN" in r.stderr, "XISF -> ASDF writes WCS keywords from the solution")
        run(p, "-o", ref, "-f", "-q", *row)
        run(mid, "-o", viaf, "-f", "-q")
        h1, h2 = fits.getheader(viaf), fits.getheader(ref)
        keys = ["CTYPE1", "CTYPE2", "CRVAL1", "CRVAL2", "CRPIX1", "CRPIX2", "CD1_1", "CD1_2", "CD2_1", "CD2_2"]
        check(all(h1[k] == h2[k] for k in keys), f"WCS keywords in ASDF equal those in FITS {row}")
        # and they come back as PixInsight solution properties
        x = os.path.join(d, "wcs_back.xisf")
        r = run(mid, "-o", x, "-f", "--no-properties")
        check("PixInsight solution properties" in r.stderr, f"ASDF -> XISF restores the solution {row}")
        m = xisf_property(x, P + "LinearTransformationMatrix")
        check(m is not None and np.allclose(np.ravel(m), [-2.3565e-4, 1.1696e-5, -1.1715e-5, -2.3575e-4], rtol=1e-9),
              f"solution matrix after XISF -> ASDF -> XISF {row}: {m}")


def test_export_from_fits_and_asdf():
    """TIFF and PNG export from FITS and ASDF input. The reference is the export of the XISF file
    the FITS/ASDF file was made from: same pixels in, same pixels out, whatever the options."""
    d = os.path.join(TMP, "export")
    os.makedirs(d, exist_ok=True)
    tiff_flags = [[], ["-c", "-s"], ["-b", "u8", "--stretch=unlinked"], ["-b", "u16"], ["-b", "f32"]]
    png_flags = [[], ["-s"], ["-b", "u8", "-s"]]
    for dtype in (np.uint8, np.uint16, np.uint32, np.uint64, np.float32, np.float64):
        for c in (1, 3):
            a = test_image(dtype, 15, 22, c, 70 + c)
            name = f"{np.dtype(dtype).name}_{c}"
            x = os.path.join(d, name + ".xisf")
            write_xisf(x, [image_entry(a)])
            sources = {"FITS": x + ".fits", "top-down FITS": x + ".td.fits", "ASDF": x + ".asdf"}
            run(x, "-o", sources["FITS"], "-f", "-q")
            run(x, "-o", sources["top-down FITS"], "-f", "-q", "--top-down")
            run(x, "-o", sources["ASDF"], "-f", "-q", "-c")
            for flags in tiff_flags:
                ref = os.path.join(d, "ref.tif")
                run(x, "-o", ref, "-f", "-q", *flags)
                want = tiff_array(ref)
                for kind, src in sources.items():
                    out = os.path.join(d, "out.tif")
                    run(src, "-o", out, "-f", "-q", *flags)
                    got = tiff_array(out)
                    check(len(got) == len(want) and got[0].dtype == want[0].dtype and np.array_equal(got[0], want[0]),
                          f"{kind} -> TIFF {flags} {name}: equals XISF -> TIFF ({got[0].dtype} vs {want[0].dtype})")
            for flags in png_flags:
                ref = os.path.join(d, "ref.png")
                run(x, "-o", ref, "-f", "-q", *flags)
                for kind, src in sources.items():
                    out = os.path.join(d, "out.png")
                    run(src, "-o", out, "-f", "-q", *flags)
                    check(open(out, "rb").read() == open(ref, "rb").read(), f"{kind} -> PNG {flags} {name}: equals XISF -> PNG")
            # one independent look at the pixels per image
            out = os.path.join(d, "plain.tif")
            run(sources["FITS"], "-o", out, "-f", "-q")
            got = tiff_array(out)[0]
            compare(f"FITS -> TIFF {name}: pixels, rows top-down", got.reshape(a.shape), a)

    # floating point data in ADU (0..65535), as ASTAP and others write it
    rng = np.random.default_rng(75)
    adu = (rng.random((12, 16)) * 60000).astype(np.float32)
    adu[0, 0], adu[0, 1] = 0, 60000
    f = os.path.join(d, "adu.fits")
    fits.PrimaryHDU(adu).writeto(f, overwrite=True)
    top = adu[::-1]                       # FITS rows are bottom-up
    out = os.path.join(d, "adu.tif")
    r = run(f, "-o", out, "-f")
    got = tiff_array(out)[0]
    check(got.dtype == np.float32 and np.allclose(got, top / 65535.0, rtol=1e-6, atol=0),
          "ADU float FITS -> TIFF: scaled to 0..1 through the 0:65535 range")
    check("0:65535 taken as black:white" in r.stderr and "scaled to 0..1" in r.stderr and "bottom-up (flipped)" in r.stderr,
          f"ADU float FITS -> TIFF: the scaling is reported: {r.stderr.strip()}")
    run(f, "-o", out, "-f", "-q", "-b", "u16")
    compare("ADU float FITS -> 16-bit TIFF keeps the ADU values", tiff_array(out)[0],
            np.floor(top.astype(np.float64) + 0.5).astype(np.uint16))
    png = os.path.join(d, "adu.png")
    r = run(f, "-o", png, "-f")
    arr, depth, _ = decode_png(png)
    check(depth == 16 and np.array_equal(arr[:, :, 0], np.floor(top.astype(np.float64) + 0.5).astype(np.uint16)),
          "ADU float FITS -> PNG: 16-bit with the ADU values")
    check("add --stretch" in r.stderr, "PNG from linear float data suggests --stretch")
    run(f, "-o", out, "-f", "-q", "-b", "u8", "--bounds", "0:60000")
    compare("--bounds sets black and white for the export", tiff_array(out)[0],
            np.floor(top.astype(np.float64) / 60000 * 255 + 0.5).astype(np.uint8))
    # the stretch works on the same normalized values as for an XISF file with bounds 0:65535
    e = image_entry(top[:, :, None])
    e["attrs"]["bounds"] = "0:65535"
    x = os.path.join(d, "adu.xisf")
    write_xisf(x, [e])
    ref = os.path.join(d, "adu_ref.tif")
    run(x, "-o", ref, "-f", "-q", "-s")
    r = run(f, "-o", out, "-f", "-s")
    compare("stretched ADU float FITS equals the stretched XISF with bounds 0:65535", tiff_array(out)[0], tiff_array(ref)[0])
    check("linked auto-STF" in r.stderr, "the stretch parameters are reported")

    # signed data with negative values: read as float, its range becomes black:white
    neg = rng.integers(-2000, 3000, (10, 14)).astype(np.int16)
    f = os.path.join(d, "neg.fits")
    fits.PrimaryHDU(neg).writeto(f, overwrite=True)
    r = run(f, "-o", out, "-f")
    got = tiff_array(out)[0]
    lo, hi = float(neg.min()), float(neg.max())
    check(np.allclose(got, (neg[::-1].astype(np.float64) - lo) / (hi - lo), rtol=0, atol=1e-6) and
          "that range taken as black:white" in r.stderr, "signed FITS data -> TIFF: scaled from its own range")
    nan = adu / 65535
    nan[3, 4] = np.nan
    f = os.path.join(d, "nan.fits")
    fits.PrimaryHDU(nan).writeto(f, overwrite=True)
    r = run(f, "-o", png, "-f")
    arr, _, _ = decode_png(png)
    check(arr[12 - 1 - 3, 4, 0] == 0 and "NaN" in r.stderr, "NaN pixels are black in PNG and reported")

    # several HDUs: TIFF pages; a cube that is not RGB becomes one page per plane; PNG takes one image
    h0 = rng.integers(0, 65535, (8, 10), dtype=np.uint16)
    h1 = rng.random((3, 6, 7)).astype(np.float32)
    h2 = rng.integers(0, 255, (5, 4, 6), dtype=np.uint8)
    f = os.path.join(d, "multi.fits")
    fits.HDUList([fits.PrimaryHDU(h0), fits.ImageHDU(h1, name="RGB"), fits.ImageHDU(h2, name="CUBE")]).writeto(f, overwrite=True)
    run(f, "-o", out, "-f", "-q")
    with tifffile.TiffFile(out) as t:
        check(len(t.pages) == 7, f"multi-HDU FITS -> TIFF: 1 + 1 + 5 pages ({len(t.pages)})")
        compare("TIFF page of the primary HDU", t.pages[0].asarray(), h0[::-1])
        compare("TIFF page of the RGB cube", t.pages[1].asarray(), np.transpose(h1, (1, 2, 0))[::-1])
        check(t.pages[1].photometric.name == "RGB" and t.pages[2].photometric.name == "MINISBLACK", "RGB and gray pages")
        for k in range(5):
            compare(f"TIFF page of cube plane {k}", t.pages[2 + k].asarray(), h2[k][::-1])
        check(t.pages[1].description == "RGB" and t.pages[4].description == "CUBE plane 2", "TIFF pages are named")
    r = run(f, "-o", png, "-f")
    arr, depth, _ = decode_png(png)
    check("PNG holds one image" in r.stderr and depth == 16 and np.array_equal(arr[:, :, 0], h0[::-1]),
          "multi-HDU FITS -> PNG writes the first image")
    r = run(f, "-o", png, "-f", "-i", "2")
    arr, depth, _ = decode_png(png)
    check("first of 5 planes" in r.stderr and depth == 8 and np.array_equal(arr[:, :, 0], h2[0][::-1]),
          "cube -> PNG writes the first plane")
    run(f, "-o", png, "-f", "-q", "-i", "1", "-b", "u8")
    arr, depth, _ = decode_png(png)
    check(arr.shape == (6, 7, 3) and np.array_equal(arr, np.floor(np.transpose(h1, (1, 2, 0))[::-1].astype(np.float64) * 255 + 0.5)),
          "RGB cube -> 8-bit RGB PNG")

    # plain ASDF arrays: bottom-up assumed, --top-down for arrays stored the other way
    img = rng.integers(0, 4000, (9, 11), dtype=np.uint16)
    rgb = rng.integers(0, 255, (6, 8, 3), dtype=np.uint8)
    p = os.path.join(d, "plain.asdf")
    write_asdf_raw(p, "img: !core/ndarray-1.0.0 {source: 0, datatype: uint16, byteorder: little, shape: [9, 11]}\n"
                      "rgb: !core/ndarray-1.0.0 {source: 1, datatype: uint8, byteorder: little, shape: [6, 8, 3]}\n",
                   [asdf_block(img.tobytes()), asdf_block(rgb.tobytes(), b"zlib")])
    r = run(p, "-o", out, "-f")
    pages = tiff_array(out)
    check(len(pages) == 2 and np.array_equal(pages[0], img[::-1]) and np.array_equal(pages[1], rgb[::-1]) and
          "assumed bottom-up" in r.stderr, "plain ASDF arrays -> TIFF (rows assumed bottom-up)")
    run(p, "-o", out, "-f", "-q", "--top-down")
    pages = tiff_array(out)
    check(np.array_equal(pages[0], img) and np.array_equal(pages[1], rgb), "plain ASDF arrays -> TIFF with --top-down")

    # naming, refusals
    run(f, "-t", "png", "-f", "-q", "-d", d)
    check(os.path.exists(os.path.join(d, "multi.png")), "-t png names the output .png")
    for args, msg in [(["-t", "tiff", "--stretch=stf"], "no saved STF"), (["-t", "png", "-b", "f32"], "PNG supports only"),
                      (["-t", "xisf", "-s"], "TIFF and PNG output"), (["-t", "asdf", "-s"], "TIFF and PNG output")]:
        r = run(f, "-f", *args, expect_ok=False)
        check(r.returncode == 1 and msg in r.stderr, f"FITS input with {args}: {r.stderr.strip()}")
    r = run(p, "-f", "-t", "png", "--stretch=stf", expect_ok=False)
    check(r.returncode == 1 and "ASDF files carry no saved STF" in r.stderr, "ASDF input with --stretch=stf")


# ---------------------------------------------------------------- XISF -> XISF, --verify

def xisf_header(path):
    raw = open(path, "rb").read()
    if raw[:8] != b"XISF0100":     # an XISF header file (.xish): the header is the whole of it
        return raw, raw.decode()
    return raw, raw[16:16 + int.from_bytes(raw[8:12], "little")].decode()


def read_xisb(path):
    """An XISF data blocks file, read without xisfconv: its signature and reserved field, the nodes
    of its block index (position, reserved field, next), and its elements in the order of the index
    as dicts (id, position, length, uncompressed, reserved, stored bytes; a free one has position 0)."""
    raw = open(path, "rb").read()
    out = {"signature": raw[:8], "reserved": raw[8:16], "nodes": [], "elements": [], "raw": raw}
    at, seen = 16, set()
    while True:
        assert at not in seen and at + 16 <= len(raw), "the block index of %s cannot be followed" % path
        seen.add(at)
        length, reserved, nxt = struct.unpack_from("<IIQ", raw, at)
        out["nodes"].append({"position": at, "length": length, "reserved": reserved, "next": nxt})
        for k in range(length):
            bid, pos, size, ulen, res = struct.unpack_from("<5Q", raw, at + 16 + 40 * k)
            out["elements"].append({"id": bid, "position": pos, "length": size, "uncompressed": ulen, "reserved": res,
                                    "stored": raw[pos:pos + size] if pos else b""})
        if not nxt:
            return out
        at = nxt


def external_block(path, loc):
    """The stored bytes of a block that the header at `path` locates with path(...): read without xisfconv."""
    import re
    m = re.fullmatch(r"path\((.*)\)(?::(0[xX][0-9a-fA-F]+|[0-9]+))?", loc, re.S)
    assert m, "not a path location: " + loc
    name = m.group(1).replace("\\(", "(").replace("\\)", ")")
    if name.startswith("@header_dir/"):
        name = os.path.join(os.path.dirname(os.path.abspath(path)), *name[len("@header_dir/"):].split("/"))
    if m.group(2) is None:
        return open(name, "rb").read()          # the block is the whole file
    wanted = int(m.group(2), 0)
    found = [e for e in read_xisb(name)["elements"] if e["id"] == wanted]
    assert len(found) == 1 and found[0]["position"], "block %s of %s" % (m.group(2), name)
    assert len(found[0]["stored"]) == found[0]["length"], "block beyond the end of " + name
    return found[0]["stored"]


def xisf_blocks(path, root=None):
    """Every data block of an XISF file, read without xisfconv: a list of dicts with the element name,
    its id, the location kind, the storage attributes, the stored bytes and the decoded bytes."""
    import xml.etree.ElementTree as ET
    raw, hdr = xisf_header(path)
    out = []
    for el in (ET.fromstring(hdr) if root is None else root).iter():
        loc = el.get("location")
        if loc is None:
            continue
        src = el
        if loc.startswith("attachment:"):
            _, pos, size = loc.split(":")
            stored = raw[int(pos):int(pos) + int(size)]
            assert len(stored) == int(size), "attachment beyond the end of " + path
        elif loc.startswith("path("):
            stored = external_block(path, loc)
        else:
            if loc == "embedded":
                src = [c for c in el if c.tag.endswith("}Data") or c.tag == "Data"][0]
                encoding = src.get("encoding")
            else:
                encoding = loc.split(":")[1]
            text = "".join((src.text or "").split())
            stored = base64.b64decode(text) if encoding == "base64" else bytes.fromhex(text)
        attr = {k: src.get(k, el.get(k)) for k in ("compression", "subblocks", "checksum")}
        if attr["checksum"] and not attr["checksum"].startswith("sha3"):
            algo, digest = attr["checksum"].split(":")
            assert hashlib.new(algo.replace("-", ""), stored).hexdigest() == digest, "checksum mismatch in " + path
        data = stored
        if attr["compression"]:
            parts = attr["compression"].split(":")
            codec = parts[0].split("+")[0]
            dec = {"zlib": lambda b, u: zlib.decompress(b), "zstd": lambda b, u: zstandard.decompress(b, max_output_size=u),
                   "lz4": lambda b, u: lz4.block.decompress(b, uncompressed_size=u),
                   "lz4hc": lambda b, u: lz4.block.decompress(b, uncompressed_size=u)}[codec]
            if attr["subblocks"]:
                data, off = b"", 0
                for pair in attr["subblocks"].split(":"):
                    cs, us = [int(v) for v in pair.split(",")]
                    data += dec(stored[off:off + cs], us)
                    off += cs
                assert off == len(stored)
            else:
                data = dec(stored, int(parts[1]))
            assert len(data) == int(parts[1])
            if parts[0].endswith("+sh"):
                item = int(parts[2])
                cnt = len(data) // item
                data = np.frombuffer(data[:cnt * item], np.uint8).reshape(item, cnt).T.tobytes() + data[cnt * item:]
        out.append({"tag": el.tag.split("}")[-1], "id": el.get("id"), "kind": "path" if loc.startswith("path(") else loc.split(":")[0], "attr": attr,
                    "stored": stored, "data": data, "location": loc, "element": el})
    return out


def header_without_storage(hdr):
    """The header with everything xisfconv may change when rewriting removed: the storage attributes of
    attached blocks and the file properties that describe the block storage."""
    import re
    def strip(m):
        return re.sub(r'\s+(location|compression|subblocks|checksum)="[^"]*"', "", m.group(0))
    hdr = re.sub(r'<[^<>]*\slocation="(?:attachment:|path\()[^<>]*>', strip, hdr)
    return re.sub(r'<Property id="XISF:(CompressionCodecs|CompressionLevel|BlockAlignmentSize)"[^>]*?(/>|>[^<]*</Property>)\s*',
                  "", hdr)


def write_xisf_blocks(path, template, blocks, align=1):
    """template: the header, with {0}, {1}, ... where the attributes of attached block n belong."""
    positions = [0] * len(blocks)
    for _ in range(5):
        attrs = []
        for b, pos in zip(blocks, positions):
            a = {"location": f"attachment:{pos}:{len(b.payload)}", **b.attrs}
            attrs.append(" ".join(f'{k}="{v}"' for k, v in a.items()))
        hdr = template.format(*attrs).encode()
        p, positions = 16 + len(hdr), []
        for b in blocks:
            p = -(-p // align) * align
            positions.append(p)
            p += len(b.payload)
    with open(path, "wb") as f:
        f.write(b"XISF0100" + len(hdr).to_bytes(4, "little") + b"\0\0\0\0" + hdr)
        for b, pos in zip(blocks, positions):
            f.write(b"\0" * (pos - f.tell()))
            f.write(b.payload)


def write_xisb(path, blocks, ids, nodes=1, free=0, align=1, tail=b""):
    """An XISF data blocks file, made without xisfconv. blocks: (stored bytes, uncompressed length);
    the index has `nodes` nodes (the first behind the signature, the others behind the data) and
    `free` free elements at its beginning."""
    n = len(blocks)
    per = max(1, -(-n // nodes))
    groups = [list(range(i, min(i + per, n))) for i in range(0, n, per)] or [[]]
    pos = 16 + 16 + 40 * (len(groups[0]) + free)
    positions = []
    for stored, _ in blocks:
        pos = -(-pos // align) * align
        positions.append(pos)
        pos += len(stored)
    node_at = [16]
    for g in groups[1:]:
        node_at.append(pos)
        pos += 16 + 40 * len(g)
    out = bytearray(pos)
    out[:8] = b"XISB0100"
    for number, g in enumerate(groups):
        at = node_at[number]
        struct.pack_into("<IIQ", out, at, len(g) + (free if number == 0 else 0), 0, node_at[number + 1] if number + 1 < len(groups) else 0)
        k = 0
        for f in range(free if number == 0 else 0):
            struct.pack_into("<5Q", out, at + 16 + 40 * k, 0xF0000000 + f, 0, 0, 0, 0)
            k += 1
        for i in g:
            struct.pack_into("<5Q", out, at + 16 + 40 * k, ids[i], positions[i], len(blocks[i][0]), blocks[i][1], 0)
            k += 1
    for (stored, _), at in zip(blocks, positions):
        out[at:at + len(stored)] = stored
    open(path, "wb").write(bytes(out) + tail)
    return positions


def write_unit(path, template, blocks, align=1, name=None, ids=None, decimal=(), **xisb):
    """A distributed XISF unit, made without xisfconv: the header file `path` (the template, with {0},
    {1}, ... where the attributes of block n belong) and beside it a data blocks file with the
    blocks. `decimal`: the blocks whose identifier the header writes as a decimal number."""
    name = name or os.path.splitext(os.path.basename(path))[0] + ".xisb"
    ids = ids or [0x4d373e33756e480f + 977 * i for i in range(len(blocks))]
    escaped = name.replace("(", "\\(").replace(")", "\\)")
    attrs = []
    for i, (b, bid) in enumerate(zip(blocks, ids)):
        a = {"location": "path(@header_dir/%s):%s" % (escaped, bid if i in decimal else "0x%016x" % bid), **b.attrs}
        attrs.append(" ".join(f'{k}="{v}"' for k, v in a.items()))
    open(path, "wb").write(template.format(*attrs).encode())
    target = os.path.join(os.path.dirname(path), *name.split("/"))
    os.makedirs(os.path.dirname(target), exist_ok=True)
    write_xisb(target, [(b.payload, len(b.raw) if b.codec else 0) for b in blocks], ids, align=align, **xisb)
    return ids


def rich_xisf(path, storage, align=1, extra_metadata="", writer=write_xisf_blocks, **writer_options):
    """A file with everything a rewrite must carry over: three images (attached, embedded, attached),
    attached and inline properties, an ICC profile, a thumbnail, comments, CDATA, entities, an
    element xisfconv does not know. `storage` gives the Block options of the attached blocks."""
    rng = np.random.default_rng(5)
    rgb = test_image(np.uint16, 23, 31, 3, 41)
    mask = test_image(np.float32, 9, 12, 1, 42)
    small = test_image(np.uint8, 7, 8, 1, 43)
    vec = (np.arange(400) * 0.25).astype("<f8").tobytes()
    icc = bytes(rng.integers(0, 255, 600, dtype=np.uint8)) + b"\0" * 300
    thumb = bytes(range(12))
    noise = bytes(rng.integers(0, 255, 3000, dtype=np.uint8))        # does not compress
    def blk(raw, item):
        kw = dict(storage)
        if kw.get("shuffle_item"):
            kw["shuffle_item"] = item if item > 1 else None
        return Block(raw, **kw)
    blocks = [blk(as_planes(rgb).astype("<u2").tobytes(), 2), blk(vec, 8), blk(icc, 1), blk(thumb, 1), blk(noise, 1),
              blk(as_planes(small).tobytes(), 1)]
    emb = Block(as_planes(mask).astype("<f4").tobytes(), codec="zlib", shuffle_item=4, checksum="sha1", location="embedded")
    emb_attrs = " ".join(f'{k}="{v}"' for k, v in emb.attrs.items())
    template = (
        '<?xml version="1.0" encoding="UTF-8"?>\n<!--\nExtensible Image Serialization Format - XISF version 1.0\n'
        'Created with the test suite\n-->\n'
        '<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" '
        'xsi:schemaLocation="http://www.pixinsight.com/xisf http://pixinsight.com/xisf/xisf-1.0.xsd">\n'
        '<Image id="main" geometry="31:23:3" sampleFormat="UInt16" colorSpace="RGB" {0} imageType="Light" >\n'
        '  <FITSKeyword name="OBJECT" value="\'M 31 &amp; &lt;friends&gt;\'" comment=\'single "quoted" attribute\'/>\n'
        '  <FITSKeyword name="HISTORY" value="" comment="caf&#233; &#x3BC;m"/>\n'
        '  <!-- a comment inside the image -->\n'
        '  <Property id="Note" type="String">text with &lt;entities&gt; &amp; "quotes" and <![CDATA[<raw> & data]]></Property>\n'
        '  <Property id="Exposure" type="Float32" value="300"/>\n'
        '  <Property id="Vec" type="F64Vector" length="400" {1}/>\n'
        '  <Property id="Inline" type="UI8Vector" length="5" location="inline:hex">0102030405</Property>\n'
        '  <ICCProfile {2}/>\n'
        '  <Thumbnail geometry="4:3:1" sampleFormat="UInt8" colorSpace="Gray" {3}/>\n'
        '  <Property id="Noise" type="UI8Vector" length="3000" {4}/>\n'
        '  <Resolution horizontal="300" vertical="300" unit="inch"/>\n'
        '  <Weird foo="bar"><Child a="1"/>some text</Weird>\n'
        '</Image>\n'
        f'<Image id="mask" geometry="12:9:1" sampleFormat="Float32" bounds="0:1" colorSpace="Gray" location="embedded">'
        f'<Data encoding="base64" {emb_attrs}>{emb.text}</Data><FITSKeyword name="GAIN" value="120" comment=""/></Image>\n'
        '<Image id="small" geometry="8:7:1" sampleFormat="UInt8" colorSpace="Gray" {5}/>\n'
        '<Metadata>\n<Property id="XISF:CreationTime" type="TimePoint" value="2026-01-02T03:04:05Z"/>\n'
        '<Property id="XISF:CreatorApplication" type="String">PixInsight 1.9.3</Property>\n'
        '<Property id="XISF:BlockAlignmentSize" type="UInt16" value="' + str(align) + '"/>\n' + extra_metadata +
        '</Metadata>\n</xisf>\n')
    writer(path, template, blocks, align, **writer_options)
    return {"main": rgb, "mask": mask, "small": small}


def test_xisf_rewrite():
    """XISF -> XISF: every block decodes to the same bytes, the header is the same text apart from
    the storage attributes, and the blocks are stored the way the options say."""
    import re
    d = os.path.join(TMP, "rewrite")
    os.makedirs(d, exist_ok=True)
    zstd_ok = zstandard is not None and ZSTD_BUILD
    sources = {
        "plain": (dict(), 4096, ""),
        "zlib+sh, sha1": (dict(codec="zlib", shuffle_item=1, checksum="sha1"), 1,
                          '<Property id="XISF:CompressionCodecs" type="String">zlib+sh</Property>\n'
                          '<Property id="XISF:CompressionLevel" type="Int32" value="0"/>\n'),
        "lz4hc": (dict(codec="lz4hc"), 16, '<Property id="XISF:CompressionCodecs" type="String" value="lz4hc"/>\n'),
        "sha-512 only": (dict(checksum="sha-512"), 1, ""),
    }
    if zstd_ok:
        sources["zstd+sh in subblocks, sha-256"] = (dict(codec="zstd", shuffle_item=1, subblocks=3, checksum="sha-256"), 1, "")
    default_codec = "zstd" if ZSTD_BUILD else "zlib"
    option_sets = [[], ["-c"], ["--codec", "zlib"], ["--codec", "none"], ["--checksum", "sha512"], ["--checksum", "none"],
                   ["-c", "--checksum", "sha1"], ["--codec", "zlib", "--xisf-subblock-size", "500"],
                   ["--codec", "lz4"], ["--codec", "lz4hc", "--checksum", "sha256", "--xisf-subblock-size", "500"]]
    for name, (storage, align, meta) in sources.items():
        src = os.path.join(d, "src.xisf")
        images = rich_xisf(src, storage, align, meta)
        before = xisf_blocks(src)
        _, hdr0 = xisf_header(src)
        ref_fits = os.path.join(d, "src.fits")
        run(src, "-o", ref_fits, "-f", "-q")
        for flags in option_sets:
            label = f"rewrite {name} {flags}"
            out = os.path.join(d, "out.xisf")
            r = run(src, "-o", out, "-f", *flags)
            check("read back and compared" in r.stdout, f"{label}: the output is read back")
            try:
                after = xisf_blocks(out)   # also checks every checksum in the output
            except Exception as e:  # noqa: BLE001
                check(False, f"{label}: output unreadable: {type(e).__name__}: {e}")
                continue
            _, hdr1 = xisf_header(out)
            check(len(after) == len(before) and all(a["data"] == b["data"] for a, b in zip(after, before)),
                  f"{label}: all {len(before)} blocks decode to the same bytes")
            check(header_without_storage(hdr1) == header_without_storage(hdr0),
                  f"{label}: the header is unchanged apart from the storage attributes")
            codec = default_codec if "-c" in flags else flags[flags.index("--codec") + 1] if "--codec" in flags else None
            want_sum = flags[flags.index("--checksum") + 1] if "--checksum" in flags else None
            for a, b in zip(after, before):
                if a["kind"] != "attachment":
                    check(a["stored"] == b["stored"] and a["attr"] == b["attr"], f"{label}: {a['kind']} block untouched")
                    continue
                comp = a["attr"]["compression"]
                if codec is None:
                    check(a["stored"] == b["stored"] and comp == b["attr"]["compression"], f"{label}: block {a['id']} copied as stored")
                elif codec == "none":
                    check(comp is None and a["stored"] == b["data"], f"{label}: block {a['id']} decompressed")
                else:
                    compressible = a["id"] != "Noise" and len(a["data"]) > 300
                    check((comp or "").startswith(codec) if compressible else True, f"{label}: block {a['id']} compressed with {codec}: {comp}")
                    check(comp is None or comp.startswith(codec), f"{label}: block {a['id']} is in no other codec: {comp}")
                    check(comp is None or a["stored"] == b["stored"] or len(a["stored"]) < len(a["data"]),
                          f"{label}: block {a['id']} is not larger than its data")
                    if comp and a["stored"] != b["stored"] and "500" in flags and len(a["data"]) > 500:
                        check(a["attr"]["subblocks"] and len(a["attr"]["subblocks"].split(":")) == -(-len(a["data"]) // 500),
                              f"{label}: block {a['id']} written in subblocks")
                had = (b["attr"]["checksum"] or "").split(":")[0].replace("-", "")
                has = (a["attr"]["checksum"] or "").split(":")[0].replace("-", "")
                check(has == ("" if want_sum == "none" else want_sum or had), f"{label}: block {a['id']} checksum {has!r}")
                if not comp:
                    check(int(a["location"].split(":")[1]) % 4096 == 0, f"{label}: uncompressed block {a['id']} is aligned")
            # metadata that describes the storage follows the change
            changed = codec is not None and any(a["attr"]["compression"] != b["attr"]["compression"] for a, b in zip(after, before))
            codecs = re.findall(r'<Property id="XISF:CompressionCodecs"[^>]*?(?:value="([^"]*)"/>|>([^<]*)</Property>)', hdr1)
            codecs = [x or y for x, y in codecs]
            if changed and codec == "none":
                check(not codecs and "XISF:CompressionLevel" not in hdr1, f"{label}: compression metadata removed")
            elif changed:
                check(codecs == [codec + "+sh"] and "XISF:CompressionLevel" not in hdr1, f"{label}: compression metadata {codecs}")
            else:
                check(re.findall(r'XISF:Compression\w+"[^>]*>[^<]*', hdr1) == re.findall(r'XISF:Compression\w+"[^>]*>[^<]*', hdr0),
                      f"{label}: compression metadata untouched")
            aligned = all(int(a["location"].split(":")[1]) % 4096 == 0 for a in after if a["kind"] == "attachment")
            check(('id="XISF:BlockAlignmentSize" type="UInt16" value="4096"' in hdr1) if aligned
                  else "XISF:BlockAlignmentSize" not in hdr1, f"{label}: block alignment stated only if it holds")
            # xisfconv reads its own output the same way
            v = run(out, "--verify", expect_ok=False)
            check(v.returncode == 0 and ": OK (XISF, 3 images, 8 data blocks" in v.stdout, f"{label}: --verify: {v.stdout.strip()}")
            fo = os.path.join(d, "out.fits")
            run(out, "-o", fo, "-f", "-q")
            check(open(fo, "rb").read() == open(ref_fits, "rb").read(), f"{label}: converts to the same FITS file as the input")

    # one image out of several
    src = os.path.join(d, "src.xisf")
    images = rich_xisf(src, dict(codec="zlib", shuffle_item=1, checksum="sha1"), 1)
    before = xisf_blocks(src)
    for index, (ident, count) in enumerate([("main", 6), ("mask", 1), ("small", 1)]):
        out = os.path.join(d, f"one_{index}.xisf")
        run(src, "-o", out, "-f", "-q", "-i", str(index))
        after = xisf_blocks(out)
        _, hdr1 = xisf_header(out)
        check(re.findall(r'<Image id="(\w+)"', hdr1) == [ident] and len(after) == count,
              f"--image {index}: only image {ident} and its {count} blocks remain")
        want = [b for b in before if b["id"] == ident or (index == 0 and b["id"] not in ("mask", "small"))]
        check([a["data"] for a in after] == [b["data"] for b in want], f"--image {index}: its blocks are intact")
        check("XISF:CreatorApplication" in hdr1 and "</Metadata>" in hdr1, f"--image {index}: file metadata kept")
        pixels = [a for a in after if a["tag"] == "Image"][0]["data"]
        check(pixels == as_planes(images[ident]).astype(images[ident].dtype.newbyteorder("<")).tobytes(), f"--image {index}: pixels")
    check(os.path.getsize(os.path.join(d, "one_2.xisf")) < os.path.getsize(os.path.join(d, "one_0.xisf")) - 2000,
          "the blocks of the other images are left out of the file")
    r = run(src, "-o", out, "-f", "-i", "5", expect_ok=False)
    check(r.returncode == 1 and "out of range" in r.stderr, "XISF -> XISF image index out of range")

    # replacing the input
    src = os.path.join(d, "inplace.xisf")
    rich_xisf(src, dict(), 4096)
    before = xisf_blocks(src)
    size0 = os.path.getsize(src)
    r = run(src, expect_ok=False, *["-t", "xisf"])
    check(r.returncode == 1 and "add --in-place" in r.stderr and os.path.getsize(src) == size0, "no silent overwrite of the input")
    r = run(src, "--in-place", "-c", "--checksum", "sha1")
    after = xisf_blocks(src)
    check(os.path.getsize(src) < size0 and [a["data"] for a in after] == [b["data"] for b in before] and
          not os.path.exists(src + ".part") and "read back and compared" in r.stdout, "--in-place replaces the file with the compressed one")
    stamp, content = os.path.getmtime(src), open(src, "rb").read()
    r = run(src, "--in-place", "-c", "--checksum", "sha1")
    check("left unchanged" in r.stdout and open(src, "rb").read() == content and os.path.getmtime(src) == stamp,
          "--in-place leaves a file alone that is already stored as requested")
    r = run(src, "--in-place", "--no-verify", "--codec", "none")
    check("read back and compared" in r.stdout and os.path.getsize(src) > len(content), "--in-place always reads the new file back")
    for args, msg in [(["--in-place", "-o", os.path.join(d, "x.xisf")], "cannot be combined"), (["--in-place", "-t", "fits"], "--in-place is for"),
                      (["-t", "xisf", "-b", "u8", "-o", os.path.join(d, "x.xisf")], "do not apply"),
                      (["-t", "xisf", "-s", "-o", os.path.join(d, "x.xisf")], "do not apply")]:
        r = run(src, *args, expect_ok=False)
        check(r.returncode != 0 and msg in r.stderr, f"XISF -> XISF with {args[:3]}: {r.stderr.strip()[:120]}")
    f = os.path.join(d, "some.fits")
    fits.PrimaryHDU(np.zeros((40, 40), np.uint16)).writeto(f, overwrite=True)
    r = run(f, "--in-place", expect_ok=False)
    check(r.returncode == 1 and "--in-place is for" in r.stderr, "--in-place on a FITS file is refused")
    # "none" is also accepted where a file is created from another format
    x = os.path.join(d, "some.xisf")
    run(f, "-o", x, "-f", "-q", "--codec", "none", "--checksum", "none")
    blk = xisf_blocks(x)[0]
    check(blk["attr"]["compression"] is None and blk["attr"]["checksum"] is None, "--codec none --checksum none for FITS -> XISF")
    run(f, "-o", x, "-f", "-q", "-c", "--checksum", "sha1")
    blk = xisf_blocks(x)[0]
    check(blk["attr"]["compression"] and blk["attr"]["checksum"].startswith("sha1:"), "-c --checksum sha1 for FITS -> XISF")

    # a damaged input is never turned into an output, and never replaced
    src = os.path.join(d, "damaged.xisf")
    rich_xisf(src, dict(codec="zlib", shuffle_item=1, checksum="sha1"), 1)
    raw = bytearray(open(src, "rb").read())
    pos = int(xisf_blocks(src)[0]["location"].split(":")[1])
    raw[pos + 40] ^= 0xFF
    open(src, "wb").write(raw)
    for flags in (["--codec", "none"], ["--codec", "zlib", "--checksum", "none"], ["--checksum", "sha256"]):
        r = run(src, "--in-place", *flags, expect_ok=False)
        check(r.returncode == 1 and "checksum mismatch" in r.stderr and open(src, "rb").read() == bytes(raw) and
              not os.path.exists(src + ".part"), f"damaged file with {flags}: refused, original untouched")
    plain = os.path.join(d, "damaged_plain.xisf")
    rich_xisf(plain, dict(codec="zlib", shuffle_item=1), 1)     # no checksum: the damage shows when decompressing
    raw = bytearray(open(plain, "rb").read())
    raw[int(xisf_blocks(plain)[0]["location"].split(":")[1]) + 40] ^= 0xFF
    open(plain, "wb").write(raw)
    r = run(plain, "--in-place", "--checksum", "sha1", expect_ok=False)
    check(r.returncode == 1 and open(plain, "rb").read() == bytes(raw), "damaged compressed block without checksum: no checksum is added")

    # SHA-3 checksums are verified and computed like the others
    src = os.path.join(d, "sha3.xisf")
    a = test_image(np.uint16, 12, 14, 1, 44)
    out = os.path.join(d, "sha3_out.xisf")
    for algo in ("sha3-256", "sha3-512"):
        e = image_entry(a)
        e["block"].attrs["checksum"] = algo + ":" + hashlib.new(algo.replace("-", "_"), e["block"].payload).hexdigest()
        write_xisf(src, [e])
        run(src, "-o", out, "-f", "-q", "-c")
        blk = xisf_blocks(out)[0]
        check(blk["attr"]["checksum"] == algo + ":" + hashlib.new(algo.replace("-", "_"), blk["stored"]).hexdigest() and
              blk["attr"]["compression"], f"{algo} checksum is computed again for the compressed block")
        raw = bytearray(open(src, "rb").read())
        raw[-20] ^= 1
        open(src, "wb").write(raw)
        r = run(src, "-o", out, "-f", "-c", expect_ok=False)
        check(r.returncode == 1 and "checksum mismatch" in r.stderr, f"a damaged block under a {algo} checksum is refused")
    write_xisf(src, [image_entry(a)])
    run(src, "-o", out, "-f", "-q", "--checksum", "sha3-512")
    blk = xisf_blocks(out)[0]
    check(blk["attr"]["checksum"] == "sha3-512:" + hashlib.sha3_512(blk["stored"]).hexdigest(), "--checksum sha3-512")
    # PixInsight does not open images with SHA-3 checksums: writing one comes with a warning
    note = "PixInsight (1.9.3) does not open images that carry them"
    fsrc = os.path.join(d, "sha3_src.fits")
    fits.PrimaryHDU(a).writeto(fsrc, overwrite=True)
    for algo in ("sha3-256", "sha3-512"):
        r = run(src, "-o", out, "-f", "--checksum", algo)
        check(r.stderr.count(note) == 1 and r.stderr.startswith("warning: " + src + ": " + algo), f"XISF -> XISF --checksum {algo} warns about PixInsight")
        r = run(fsrc, "-o", out, "-f", "--checksum", algo)
        check(r.stderr.count(note) == 1 and xisf_blocks(out)[0]["attr"]["checksum"].startswith(algo + ":"),
              f"FITS -> XISF --checksum {algo} warns about PixInsight")
    r = run(fsrc, "-o", out, "-f", "-q", "--checksum", "sha3-256")
    check(r.stderr == "", "-q silences the warning about SHA-3 checksums")
    for algo in ("sha1", "sha256", "sha512"):
        r1 = run(src, "-o", out, "-f", "--checksum", algo)
        r2 = run(fsrc, "-o", out, "-f", "--checksum", algo)
        check(note not in r1.stderr + r2.stderr, f"no such warning for {algo}")
    # a file that has a SHA-3 checksum already keeps it without comment; replacing it is the way out
    run(src, "-o", out, "-f", "-q", "--checksum", "sha3-256")
    r = run(out, "--in-place", "--codec", "zlib")
    check(note not in r.stderr and xisf_blocks(out)[0]["attr"]["checksum"].startswith("sha3-256:"), "an existing SHA-3 checksum is kept without a warning")
    r = run(out, "--in-place", "--checksum", "sha256")
    blk = xisf_blocks(out)[0]
    check(r.returncode == 0 and blk["attr"]["checksum"] == "sha256:" + hashlib.sha256(blk["stored"]).hexdigest(),
          "--in-place --checksum sha256 replaces a SHA-3 checksum")
    # a checksum of an unknown kind: kept on a copied block, never silently dropped from one stored differently
    e = image_entry(a)
    e["block"].attrs["checksum"] = "whirlpool:" + "ab" * 64
    write_xisf(src, [e])
    run(src, "-o", out, "-f", "-q")
    check(xisf_header(out)[1].count('checksum="whirlpool:') == 1, "a checksum of an unknown kind is kept when the block is copied")
    r = run(src, "-o", out, "-f", "-c", expect_ok=False)
    check(r.returncode == 1 and "cannot verify" in r.stderr, f"...and the block is not stored differently: {r.stderr.strip()[:160]}")
    r = run(src, "-o", out, "-f", "-c", "--no-verify")
    check("checksum" not in xisf_header(out)[1] and "cannot be recomputed" in r.stderr, "...unless --no-verify is given (checksum dropped, with a warning)")
    r = run(src, "--verify", expect_ok=False)
    check(r.returncode == 0 and "NOT FULLY CHECKED" in r.stdout and "whirlpool" in r.stdout, f"--verify says what it could not check: {r.stdout.strip()[:200]}")

    # blocks in other files; elements that use a location attribute for something else
    hdr = ('<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf"><Image geometry="4:4:1" sampleFormat="UInt8" '
           'colorSpace="Gray" location="url(file:///data.xisb):16:16"/></xisf>').encode()
    open(src, "wb").write(b"XISF0100" + len(hdr).to_bytes(4, "little") + b"\0\0\0\0" + hdr)
    r = run(src, "-o", out, "-f", expect_ok=False)
    check(r.returncode == 1 and "malformed location" in r.stderr, "a location that is none of the forms of the specification is refused")
    e = image_entry(a, children='<Observatory location="La Palma"/><Property id="Site" type="String" location="Roque"/>')
    write_xisf(src, [e])
    r = run(src, "-o", out, "-f", "-c")
    check(r.returncode == 0 and '<Observatory location="La Palma"/>' in xisf_header(out)[1] and
          run(out, "--verify", expect_ok=False).returncode == 0, "a location attribute that is no block reference is left alone")
    emb = Block(as_planes(a).astype("<u2").tobytes(), location="embedded", encoding="hex")
    hdr = ('<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf"><Image geometry="14:12:1" sampleFormat="UInt16" '
           f'colorSpace="Gray" location="embedded"><Data encoding="hex" location="attachment:9999:5">{emb.text}</Data></Image>'
           '</xisf>').encode()
    open(src, "wb").write(b"XISF0100" + len(hdr).to_bytes(4, "little") + b"\0\0\0\0" + hdr)
    run(src, "-o", out, "-f", "-q", "-c", "--checksum", "sha1")
    fo = os.path.join(d, "emb.fits")
    run(out, "-o", fo, "-f", "-q", "--top-down")
    check(np.array_equal(fits.getdata(fo), a[:, :, 0]) and run(out, "--verify", expect_ok=False).returncode == 0,
          "the <Data> element of an embedded block is not taken for a block of its own")

    # temporary files: never the input, never somebody else's file
    part = os.path.join(d, "img.xisf.part")
    rich_xisf(part, dict(), 4096)
    content = open(part, "rb").read()
    for flags in (["-c"], ["-t", "fits"]):
        target = os.path.join(d, "img.xisf") if flags == ["-c"] else os.path.join(d, "img.xisf")
        r = run(part, "-o", os.path.join(d, "img.xisf"), *flags, expect_ok=False)
        check(r.returncode == 1 and "is the input file" in r.stderr and open(part, "rb").read() == content,
              f"an input named like the temporary file is not destroyed ({flags})")
    f = os.path.join(d, "t.fits")
    fits.PrimaryHDU(np.zeros((40, 40), np.uint16)).writeto(f, overwrite=True)
    r = run(f, "-o", os.path.join(d, "img.xisf"), expect_ok=False)
    check(r.returncode == 1 and open(part, "rb").read() == content, "a file named like the temporary file is not overwritten (FITS -> XISF)")
    src = os.path.join(d, "keep.xisf")
    rich_xisf(src, dict(), 4096)
    open(src + ".part", "w").write("my notes")
    open(src[:-5] + ".fits.part", "w").write("my notes")
    for flags, part in ((["--in-place", "-c"], src + ".part"), (["--in-place", "-i", "9"], src + ".part"),
                        (["-t", "fits"], src[:-5] + ".fits.part")):
        r = run(src, *flags, expect_ok=False)
        check(r.returncode == 1 and open(part).read() == "my notes" and ("exists" in r.stderr or "out of range" in r.stderr),
              f"a foreign .part file is left alone ({flags}): {r.stderr.strip()[:100]}")
    run(src, "--in-place", "-c", "--force", "-q")
    check(not os.path.exists(src + ".part") and xisf_blocks(src)[0]["attr"]["compression"], "--force lets a leftover .part be overwritten")
    r = run(src, "--in-place", "-c", "-i", "7", expect_ok=False)
    check(r.returncode == 1 and "out of range" in r.stderr, "--in-place with an image index out of range is an error")

    # replacing through a link, and file permissions (POSIX)
    if os.name == "posix":
        real = os.path.join(d, "real.xisf")
        link = os.path.join(d, "link.xisf")
        rich_xisf(real, dict(), 4096)
        if os.path.lexists(link):
            os.remove(link)
        os.symlink(real, link)
        size0 = os.path.getsize(real)
        os.chmod(real, 0o640)
        run(link, "--in-place", "-c", "-q")
        check(os.path.islink(link) and os.path.getsize(real) < size0, "--in-place through a symbolic link rewrites the file, not the link")
        check((os.stat(real).st_mode & 0o777) == 0o640, f"--in-place keeps the file's permissions ({oct(os.stat(real).st_mode & 0o777)})")
        os.chmod(real, 0o440)
        content = open(real, "rb").read()
        r = run(real, "--in-place", "--codec", "none", expect_ok=False)
        check(r.returncode == 1 and "read-only" in r.stderr and open(real, "rb").read() == content, "--in-place does not replace a read-only file")
        os.chmod(real, 0o640)


def test_verify():
    """--verify on intact and damaged XISF, FITS and ASDF files, and on a directory."""
    import warnings
    d = os.path.join(TMP, "verify")
    os.makedirs(os.path.join(d, "sub", "deeper"), exist_ok=True)
    def verify(*args):
        return run("--verify", *args, expect_ok=False)
    def damaged(path, offset, name):
        raw = bytearray(open(path, "rb").read())
        raw[offset] ^= 0xFF
        out = os.path.join(d, name)
        open(out, "wb").write(raw)
        return out

    good = os.path.join(d, "good.xisf")
    rich_xisf(good, dict(codec="zlib", shuffle_item=1, checksum="sha1"), 1)
    r = verify(good)
    check(r.returncode == 0 and r.stdout.strip().endswith(": OK (XISF, 3 images, 8 data blocks; 7 checksums verified, 1 without checksum)"),
          f"--verify on an intact XISF file: {r.stdout.strip()}")
    pos = int(xisf_blocks(good)[0]["location"].split(":")[1])
    r = verify(damaged(good, pos + 40, "bad_sum.xisf"))
    check(r.returncode == 1 and "FAILED" in r.stdout and "checksum mismatch on image 0" in r.stdout and "--no-verify" not in r.stdout,
          f"--verify finds a flipped byte under a checksum: {r.stdout.strip()}")
    nosum = os.path.join(d, "nosum.xisf")
    rich_xisf(nosum, dict(codec="zlib", shuffle_item=1), 1)
    r = verify(nosum)
    check(r.returncode == 0 and "1 checksum verified, 7 without checksum" in r.stdout, f"--verify without checksums: {r.stdout.strip()}")
    pos = int(xisf_blocks(nosum)[1]["location"].split(":")[1])
    r = verify(damaged(nosum, pos + 20, "bad_zlib.xisf"))
    check(r.returncode == 1 and "FAILED" in r.stdout and "Vec" in r.stdout, f"--verify finds damaged compressed data: {r.stdout.strip()}")
    trunc = os.path.join(d, "trunc.xisf")
    open(trunc, "wb").write(open(good, "rb").read()[:-200])
    r = verify(trunc)
    check(r.returncode == 1 and "beyond the end of the file" in r.stdout, "--verify finds a truncated file")
    a = test_image(np.uint16, 12, 14, 1, 45)
    e = image_entry(a)
    e["attrs"]["geometry"] = "14:13:1"
    wrong = os.path.join(d, "geometry.xisf")
    write_xisf(wrong, [e])
    r = verify(wrong)
    check(r.returncode == 1 and "geometry requires" in r.stdout, "--verify compares the pixel data with the geometry")

    # FITS: CHECKSUM / DATASUM as astropy writes them
    rng = np.random.default_rng(46)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        f = os.path.join(d, "sums.fits")
        fits.HDUList([fits.PrimaryHDU(rng.integers(0, 65535, (30, 40), dtype=np.uint16)),
                      fits.ImageHDU(rng.random((3, 20, 25)).astype(np.float32), name="B"), fits.ImageHDU()]).writeto(
                          f, overwrite=True, checksum=True)
        plain = os.path.join(d, "plain.fits")
        fits.PrimaryHDU(rng.integers(0, 255, (9, 9), dtype=np.uint8)).writeto(plain, overwrite=True)
        with fits.open(f, checksum=True) as h:   # astropy agrees that the sums are right
            w = []
            with warnings.catch_warnings(record=True) as w:
                warnings.simplefilter("always")
                [x.data for x in h]
            check(not w, "astropy accepts the checksums of the test file")
    r = verify(f)
    check(r.returncode == 0 and ": OK (FITS, 3 HDUs; 3 checksums verified)" in r.stdout, f"--verify on FITS with checksums: {r.stdout.strip()}")
    r = verify(plain)
    check(r.returncode == 0 and "1 HDU; no checksums in the file" in r.stdout, f"--verify on FITS without checksums: {r.stdout.strip()}")
    r = verify(damaged(f, 2880 + 500, "bad_data.fits"))
    check(r.returncode == 1 and "HDU 0: DATASUM mismatch" in r.stdout and "HDU 0: CHECKSUM mismatch" in r.stdout and
          "HDU 1" not in r.stdout, f"--verify finds a damaged FITS data unit: {r.stdout.strip()}")
    header = open(f, "rb").read()[:2880]
    r = verify(damaged(f, header.index(b"BITPIX") + 40, "bad_header.fits"))
    check(r.returncode == 1 and "HDU 0: CHECKSUM mismatch: the header was changed" in r.stdout and "DATASUM" not in r.stdout,
          f"--verify finds a changed FITS header: {r.stdout.strip()}")
    open(os.path.join(d, "short.fits"), "wb").write(open(plain, "rb").read()[:2880 + 10])
    r = verify(os.path.join(d, "short.fits"))
    check(r.returncode == 1 and "beyond the end" in r.stdout, "--verify finds a truncated FITS file")
    whole = open(f, "rb").read()
    second = whole.index(b"XTENSION")
    open(os.path.join(d, "cut_header.fits"), "wb").write(whole[:second + 1000])
    r = verify(os.path.join(d, "cut_header.fits"))
    check(r.returncode == 1 and "after HDU 0 are not padding" in r.stdout, f"--verify finds a FITS file cut inside a header: {r.stdout.strip()}")
    r = verify(damaged(f, second + 1, "bad_xtension.fits"))
    check(r.returncode == 1 and "after HDU 0 are not padding" in r.stdout, "--verify finds a damaged XTENSION card")
    third = whole.index(b"XTENSION", second + 1)
    open(os.path.join(d, "unpadded.fits"), "wb").write(whole[:second + 2880 + 3 * 20 * 25 * 4])   # HDU 1 without its padding
    r = verify(os.path.join(d, "unpadded.fits"))
    check(r.returncode == 0 and "2 HDUs; 2 checksums verified" in r.stdout and third > second,
          f"--verify accepts an intact last HDU that is not padded: {r.stdout.strip()}")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        g = os.path.join(d, "groups.fits")
        gdata = fits.GroupData(rng.random((5, 1, 1, 4, 3)).astype(np.float32), parnames=["U", "V"], bitpix=-32,
                               pardata=[np.arange(5.0), np.arange(5.0) * 2])
        fits.GroupsHDU(gdata).writeto(g, overwrite=True, checksum=True)
    r = verify(g)
    check(r.returncode == 0 and "1 HDU; 1 checksum verified" in r.stdout, f"--verify on random groups FITS: {r.stdout.strip()}")
    r = verify(damaged(g, os.path.getsize(g) - 2880 + 7, "bad_groups.fits"))
    check(r.returncode == 1 and "DATASUM mismatch" in r.stdout, "--verify finds damage in random groups data")

    # ASDF: MD5 of every block
    s = os.path.join(d, "good.asdf")
    run(f, "-o", s, "-f", "-q", "-c")
    r = verify(s)
    check(r.returncode == 0 and ": OK (ASDF, 2 binary blocks; 2 checksums verified)" in r.stdout, f"--verify on ASDF: {r.stdout.strip()}")
    r = verify(damaged(s, open(s, "rb").read().index(b"\xd3BLK") + 60, "bad.asdf"))
    check(r.returncode == 1 and "block 0: MD5 checksum mismatch" in r.stdout and "--no-verify" not in r.stdout,
          f"--verify finds a damaged ASDF block: {r.stdout.strip()}")
    whole = open(s, "rb").read()
    first = whole.index(b"\xd3BLK")
    second = whole.index(b"\xd3BLK", first + 1)
    for name, cut in [("at the end of the tree", first), ("inside the first block header", first + 20),
                      ("at the second block", second), ("inside the second block header", second + 30),
                      ("inside the second block", second + 80)]:
        open(os.path.join(d, "cut.asdf"), "wb").write(whole[:cut])
        r = verify(os.path.join(d, "cut.asdf"))
        check(r.returncode == 1 and "FAILED" in r.stdout, f"--verify finds an ASDF file cut {name}: {r.stdout.strip()[:160]}")
    r = verify(damaged(s, second, "bad_magic.asdf"))
    check(r.returncode == 1 and "damaged block header" in r.stdout, f"--verify finds a damaged ASDF block magic: {r.stdout.strip()[:200]}")
    bz = os.path.join(d, "bz.asdf")
    write_asdf_raw(bz, "img: !core/ndarray-1.0.0 {source: 0, datatype: uint8, byteorder: little, shape: [4, 5]}\n",
                   [asdf_block(bytes(20), b"bzp2")])
    r = verify(bz, plain)
    check(r.returncode == 0 and "bz.asdf: NOT FULLY CHECKED" in r.stdout and "not checked: block 0: bzip2" in r.stdout and
          "1 file OK, 1 not fully checked, 0 failed" in r.stdout, f"--verify: what cannot be checked is not a failure: {r.stdout.strip()}")

    # other files, directories, exit status, --quiet
    text = os.path.join(d, "notes.txt")
    open(text, "w").write("not an image, just some notes\n" * 3)
    r = verify(text)
    check(r.returncode == 1 and "FAILED" in r.stdout and "not an XISF" in r.stdout, "--verify on a file of another kind")
    r = verify(os.path.join(d, "missing.xisf"))
    check(r.returncode == 1 and "FAILED" in r.stdout, "--verify on a missing file")
    # a directory where one file is meant (a directory stands for its files since 0.17: test_directories_and_patterns)
    r = subprocess.run([EXE, d, "-o", os.path.join(d, "from-directory.fits")], capture_output=True, text=True)
    check(r.returncode == 2 and "is a directory" in r.stderr and not os.path.exists(os.path.join(d, "from-directory.fits")),
          f"a directory given with -o is called a directory: {r.stderr.strip()}")
    # an output name that is a directory is not replaced by the output, with or without --force
    taken = os.path.join(d, "taken.fits")
    os.mkdir(taken)
    for force in ([], ["-f"]):
        r = subprocess.run([EXE, good, "-o", taken] + force, capture_output=True, text=True)
        check(r.returncode == 1 and "is a directory; it is not replaced" in r.stderr and os.path.isdir(taken) and
              not os.path.exists(taken + ".part"), f"an output that is a directory is refused {force}: {r.stderr.strip()}")
    os.rmdir(taken)
    tree = os.path.join(d, "sub")
    shutil.copy(good, os.path.join(tree, "a.xisf"))
    shutil.copy(f, os.path.join(tree, "deeper", "b.fits"))
    shutil.copy(s, os.path.join(tree, "deeper", "c.asdf"))
    shutil.copy(text, os.path.join(tree, "deeper", "notes.txt"))
    r = verify(tree)
    check(r.returncode == 0 and r.stdout.count(": OK (") == 3 and "3 files OK, 0 failed" in r.stdout and "notes.txt" not in r.stdout,
          f"--verify on a directory looks at the image files below it: {r.stdout.strip()[-200:]}")
    shutil.copy(os.path.join(d, "bad_sum.xisf"), os.path.join(tree, "deeper", "z.xisf"))
    r = verify(tree, plain)
    check(r.returncode == 1 and "4 files OK, 1 failed" in r.stdout, "--verify: exit status 1 when a file fails")
    r = verify("-q", tree, plain)
    check(r.returncode == 1 and ": OK" not in r.stdout and "z.xisf: FAILED" in r.stdout, "--verify --quiet prints the failures only")
    empty = os.path.join(d, "empty")
    os.makedirs(empty, exist_ok=True)
    r = verify(empty)
    check(r.returncode == 0 and "no XISF, FITS or ASDF files found" in r.stderr, "--verify on a directory without image files")
    if os.name == "posix" and os.geteuid() != 0:   # (root reads every directory)
        locked = os.path.join(tree, "locked")
        os.makedirs(locked, exist_ok=True)
        os.chmod(locked, 0)
        r = verify(tree)
        os.chmod(locked, 0o755)
        check(r.returncode == 1 and "locked" in r.stdout and "cannot be read" in r.stdout, "--verify reports a directory it cannot read")
    before = {n: os.path.getmtime(os.path.join(d, n)) for n in os.listdir(d)}
    verify(d)
    check(before == {n: os.path.getmtime(os.path.join(d, n)) for n in os.listdir(d)}, "--verify writes nothing")


# ---------------------------------------------------------------- tile-compressed FITS

HAVE_FPACK = shutil.which("fpack") is not None and shutil.which("funpack") is not None


def comp_hdu(data, tile=None, **kw):
    """astropy's tile-compressed image HDU (the tile size argument changed its name in astropy 5.3)."""
    if tile is None:
        return fits.CompImageHDU(data, **kw)
    try:
        return fits.CompImageHDU(data, tile_shape=tile, **kw)
    except TypeError:  # pragma: no cover
        return fits.CompImageHDU(data, tile_size=tile[::-1], **kw)


def test_fits_tile_compressed():
    """Tile-compressed FITS images (.fits.fz). The reference is what astropy decompresses from the same
    file, bit for bit; for files made by fpack, what funpack writes."""
    import warnings
    d = os.path.join(TMP, "fz")
    os.makedirs(d, exist_ok=True)
    rng = np.random.default_rng(81)

    def image(dtype, shape):
        y = np.indices(shape).sum(0)
        if np.issubdtype(dtype, np.floating):
            return (np.sin(y / 7.0) * 1000 + rng.normal(0, 5, shape) + 2000).astype(dtype)
        info = np.iinfo(dtype)
        a = ((y * 37 + rng.integers(0, 50, shape)) % (int(info.max) - int(info.min) + 1) + int(info.min)).astype(dtype)
        a.flat[0], a.flat[1] = info.max, info.min
        return a

    def same(got, ref, slack=None, label=""):
        """Equal bit for bit. For quantized floating point (`slack` given, see rounding_slack) a
        difference from rounding is accepted and counted."""
        if got.shape != ref.shape:
            return False
        if np.array_equal(got.astype(np.float64), ref.astype(np.float64), equal_nan=True):
            return True
        if slack is None or got.dtype.kind != "f" or ref.dtype.kind != "f":
            return False
        a, b = got.astype(np.float64), ref.astype(np.float64)
        if not np.array_equal(np.isnan(a), np.isnan(b)):
            return False
        ok = ~np.isnan(a)
        if got.dtype.itemsize == 4 and ref.dtype.itemsize == 4:
            # rounded to single precision afterwards: the neighbouring value at most
            near = np.abs(a[ok] - b[ok]) <= np.spacing(np.maximum(np.abs(got[ok]), np.abs(ref[ok])).astype(np.float32))
        else:
            near = np.abs(a[ok] - b[ok]) <= slack
        if not np.all(near):
            return False
        last_bit.append(label)
        return True

    def rounding_slack(path, hdu=1):
        """How far the values restored from a quantized image may differ between two correct
        programs, or None if the image is not quantized (then they must be equal).

        A value is restored as integer * scale + zero. Whether the product is rounded before
        the addition depends on how a program was compiled: on arm64, C compilers fuse the two
        into one instruction with one rounding, NumPy never does, and xisfconv is built not to.
        The results differ by at most the rounding of the product, which is about as large as
        the zero point (astropy's and CFITSIO's second dithering method uses zero points of
        several 1e8 for data around 1e3, so that is more than the last bit of the result)."""
        with fits.open(path, disable_image_compression=True) as h:
            table = h[hdu]
            if "ZZERO" not in (table.columns.names or []):
                return None
            zero = float(np.max(np.abs(table.data["ZZERO"]))) if len(table.data) else 0.0
            scale = float(np.max(np.abs(table.data["ZSCALE"]))) if len(table.data) else 0.0
        return 2 * float(np.spacing(2 * zero + 2.0 ** 31 * scale))

    last_bit = LAST_BIT

    src = os.path.join(d, "c.fits.fz")
    out = os.path.join(d, "out.fits")

    def convert_and_compare(label, hdu=1):
        with fits.open(src) as h:
            ref = np.array(h[hdu].data)
        r = run(src, "-o", out, "-f", "-q", "-t", "fits", expect_ok=False)
        if r.returncode != 0:
            check(False, f"{label}: {r.stderr.strip()[:200]}")
            return
        got = np.array(fits.getdata(out))
        check(same(got, ref, rounding_slack(src, hdu), label), f"{label}: pixels equal astropy's decompression ({got.dtype}, {got.shape})")

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        # integers: every algorithm, default tiles (rows), small tiles, the whole image as one tile
        for dtype in (np.uint8, np.int16, np.uint16, np.int32, np.uint32):
            for shape in ((37, 53), (3, 20, 31)):
                a = image(dtype, shape)
                for algo in ("RICE_1", "GZIP_1", "GZIP_2", "NOCOMPRESS"):
                    for tile in (None, tuple(min(s, 16) for s in shape), shape):
                        fits.HDUList([fits.PrimaryHDU(), comp_hdu(a, tile, compression_type=algo)]).writeto(src, overwrite=True)
                        convert_and_compare(f"{np.dtype(dtype).name} {shape} {algo} tiles {tile}")
        # masks: PLIO run-length coding
        for dtype in (np.uint8, np.int16, np.int32):
            m = (rng.random((40, 60)) > 0.8).astype(dtype) * rng.integers(1, 200, (40, 60)).astype(dtype)
            m[5:10, :] = 7
            m[20] = 0
            for tile in (None, (8, 60), (40, 60)):
                fits.HDUList([fits.PrimaryHDU(), comp_hdu(m, tile, compression_type="PLIO_1")]).writeto(src, overwrite=True)
                convert_and_compare(f"PLIO_1 {np.dtype(dtype).name} tiles {tile}")
        # floating point: quantized with each dithering method (seeds at both ends of the range), NaN
        # and exact zeros, and lossless
        for dtype in (np.float32, np.float64):
            for shape in ((37, 53), (2, 24, 31)):
                a = image(dtype, shape)
                a.flat[5] = np.nan
                a[..., 10:14, 3:9] = 0.0
                n = 0
                for algo in ("RICE_1", "GZIP_1", "GZIP_2"):
                    for method in (-1, 1, 2):
                        for level, seed in ((16.0, 1), (4.0, 777), (-0.01, 10000)):
                            n += 1
                            if n % 2 and shape != (37, 53):
                                continue   # half of the combinations for the cube
                            fits.HDUList([fits.PrimaryHDU(), comp_hdu(a, None, compression_type=algo, quantize_level=level,
                                                                      quantize_method=method, dither_seed=seed)]).writeto(src, overwrite=True)
                            convert_and_compare(f"{np.dtype(dtype).name} {shape} {algo} quantized, level {level}, method {method}, seed {seed}")
                for algo in ("GZIP_1", "GZIP_2", "NOCOMPRESS"):
                    fits.HDUList([fits.PrimaryHDU(), comp_hdu(a, None, compression_type=algo, quantize_level=0.0)]).writeto(src, overwrite=True)
                    with fits.open(src) as h:
                        check(same(np.array(h[1].data), a), "astropy's own lossless round trip")
                    convert_and_compare(f"{np.dtype(dtype).name} {shape} {algo} lossless")

        # the header: the image's keywords stay, the table's and the compression's go
        a = image(np.uint16, (30, 44))
        hd = fits.Header()
        hd["OBJECT"] = ("M 31", "target")
        hd["EXPTIME"] = (30.5, "seconds")
        hd["ZEBRA"] = (7, "a keyword that merely starts with Z")
        hd["TTYPEX"] = "not a column keyword"
        hd["ROWORDER"] = "TOP-DOWN"
        hd.add_history("calibrated")
        fits.HDUList([fits.PrimaryHDU(), comp_hdu(a, None, header=hd, compression_type="RICE_1"),
                      comp_hdu(image(np.float32, (20, 21)), None, name="SECOND", compression_type="GZIP_2", quantize_level=0.0),
                      fits.ImageHDU(image(np.int16, (9, 11)), name="PLAIN"),
                      fits.BinTableHDU.from_columns([fits.Column(name="x", format="E", array=np.arange(5.0))])]).writeto(src, overwrite=True)
        r = run(src, "-o", out, "-f", "-t", "fits")
        check("RICE_1 tile compression" in r.stderr and "skipped HDU 4: BINTABLE" in r.stderr, f"messages for a mixed file: {r.stderr.strip()[:300]}")
        with fits.open(out) as h, fits.open(src) as ref:
            check(len(h) == 3 and [x.header.get("EXTNAME") for x in h] == [None, "SECOND", "PLAIN"], "compressed and plain images, in order, with their names")
            for i in range(3):
                check(same(np.array(h[i].data), np.array(ref[i + 1].data)), f"mixed file, image {i}")
            hdr = h[0].header
            check(hdr["OBJECT"] == "M 31" and hdr.comments["OBJECT"] == "target" and hdr["EXPTIME"] == 30.5 and hdr["ZEBRA"] == 7 and
                  hdr["TTYPEX"] == "not a column keyword" and "calibrated" in str(hdr["HISTORY"]), "the image's keywords are carried over")
            left = [k for k in hdr if k.startswith(("ZIMAGE", "ZCMPTYPE", "ZBITPIX", "ZNAXIS", "ZTILE", "ZNAME", "ZVAL", "TFIELDS", "TFORM",
                                                    "TTYPE1", "PCOUNT", "THEAP", "ZQUANTIZ", "ZDITHER"))]
            check(not left, f"table and compression keywords are not carried over: {left}")
            check(hdr["ROWORDER"] == "TOP-DOWN" and hdr["BZERO"] == 32768, "row order and the unsigned convention survive")
        if HAVE_FITSVERIFY:
            v = subprocess.run(["fitsverify", "-q", out], capture_output=True, text=True)
            check("verification OK" in v.stdout, f"fitsverify on the decompressed file: {v.stdout.strip()}")
        x = os.path.join(d, "c.xisf")
        run(src, "-o", x, "-f", "-q")
        compare(".fits.fz -> XISF, top-down image", read_xisf_any(x), a[:, :, None])
        with fits.open(src) as ref:
            compare(".fits.fz -> XISF, second image (rows flipped)", read_xisf_any(x, 1), np.array(ref[2].data)[::-1, :, None])
        info = run(src, "--info").stdout
        check("HDU 1: 44 x 30 x 1, BITPIX 16, tile-compressed (RICE_1)" in info and 'HDU 2 "SECOND": 21 x 20 x 1, BITPIX -32, tile-compressed (GZIP_2)' in info,
              f"--info names the compression: {info[:200]}")
        # names: image.fits.fz -> image.xisf / image.fits; plain FITS -> FITS stays refused
        named = os.path.join(d, "image.fits.fz")
        shutil.copy(src, named)
        run(named, "-f", "-q")
        run(named, "-f", "-q", "-t", "fits")
        run(named, "-f", "-q", "-t", "png", "-s")
        check(all(os.path.exists(os.path.join(d, n)) for n in ("image.xisf", "image.fits", "image.png")), "image.fits.fz is converted to image.xisf / .fits / .png")
        r = run(os.path.join(d, "image.fits"), "-t", "fits", "-f", expect_ok=False)
        check(r.returncode == 1 and "already a FITS file" in r.stderr, "FITS -> FITS is only for decompressing")

        # what is not implemented is named and skipped; the rest is converted
        b = (np.arange(64 * 64).reshape(64, 64) % 5000).astype(np.int16)
        fits.HDUList([fits.PrimaryHDU(), comp_hdu(b, None, compression_type="HCOMPRESS_1"),
                      comp_hdu(b, None, name="OK", compression_type="RICE_1")]).writeto(src, overwrite=True, checksum=True)
        r = run(src, "-o", out, "-f", "-t", "fits")
        check("skipped HDU 1: tile-compressed image (HCOMPRESS_1), which is not supported" in r.stderr and
              same(np.array(fits.getdata(out)), b), "HCOMPRESS_1 is skipped with a message, the other image is converted")
        r = run("--verify", src, expect_ok=False)
        check(r.returncode == 0 and "NOT FULLY CHECKED" in r.stdout and "HDU 1: tile compression HCOMPRESS_1 is not supported" in r.stdout and
              "3 checksums verified" in r.stdout, f"--verify on a file with HCOMPRESS_1: {r.stdout.strip()[:300]}")

        # --verify and damage
        fits.HDUList([fits.PrimaryHDU(), comp_hdu(image(np.int16, (60, 80)), None, compression_type="RICE_1")]).writeto(src, overwrite=True, checksum=True)
        raw = bytearray(open(src, "rb").read())
        r = run("--verify", src, expect_ok=False)
        check(r.returncode == 0 and "OK (FITS, 2 HDUs; 2 checksums verified)" in r.stdout, f"--verify on .fits.fz: {r.stdout.strip()}")
        table = raw.index(b"XTENSION")
        heap = table + (raw[table:].index(b"END" + b" " * 77) // 2880 + 1) * 2880 + 60 * 8   # behind the 60 row descriptors
        bad = bytearray(raw)
        bad[heap + 400] ^= 0xFF
        open(src, "wb").write(bad)
        r = run("--verify", src, expect_ok=False)
        check(r.returncode == 1 and "DATASUM mismatch" in r.stdout, f"--verify finds a damaged tile: {r.stdout.strip()[:200]}")
        plain = os.path.join(d, "nosum.fits.fz")
        fits.HDUList([fits.PrimaryHDU(), comp_hdu(image(np.int16, (60, 80)), None, compression_type="GZIP_1")]).writeto(plain, overwrite=True)
        raw = bytearray(open(plain, "rb").read())
        table = raw.index(b"XTENSION")
        heap = table + (raw[table:].index(b"END" + b" " * 77) // 2880 + 1) * 2880 + 60 * 8
        raw[heap + 400] ^= 0xFF
        open(plain, "wb").write(raw)
        r = run("--verify", plain, expect_ok=False)
        check(r.returncode == 1 and "tile" in r.stdout, f"--verify finds a tile that does not decompress: {r.stdout.strip()[:200]}")
        r = run(plain, "-o", out, "-f", "-t", "fits", expect_ok=False)
        check(r.returncode == 1 and "tile-compressed image: tile" in r.stderr and not os.path.exists(out + ".part"), "a damaged tile stops the conversion")
        cut = os.path.join(d, "cut.fits.fz")
        whole = open(named, "rb").read()
        table = whole.index(b"XTENSION")
        data = table + (whole[table:].index(b"END" + b" " * 77) // 2880 + 1) * 2880
        open(cut, "wb").write(whole[:data + 100])
        r = run(cut, "-o", out, "-f", "-t", "fits", expect_ok=False)
        check(r.returncode == 1 and "beyond the end" in r.stderr, "a truncated .fits.fz is reported")
        # a table that does not match its header
        fits.HDUList([fits.PrimaryHDU(), comp_hdu(image(np.int16, (60, 80)), None, compression_type="RICE_1")]).writeto(src, overwrite=True)
        raw = open(src, "rb").read()
        for old, new, msg in ((b"ZNAXIS2 =                   60", b"ZNAXIS2 =                   61", "the image needs 61 tiles"),
                              (b"ZVAL2   =                    2", b"ZVAL2   =                    3", "bytes per pixel"),
                              (b"ZBITPIX =                   16", b"ZBITPIX =                   17", "ZBITPIX")):
            check(old in raw, "test file layout")
            open(src, "wb").write(raw.replace(old, new))
            r = run(src, "-o", out, "-f", "-t", "fits", expect_ok=False)
            check(r.returncode == 1 and msg in r.stderr, f"inconsistent header ({new.decode().split()[0]}): {r.stderr.strip()[:160]}")

        # a header that promises far more pixels than the table can hold must not cost the memory
        huge = raw.replace(b"ZNAXIS1 =                   80", b"ZNAXIS1 =            800000000").replace(
            b"ZTILE1  =                   80", b"ZTILE1  =            800000000")
        check(huge != raw and len(huge) == len(raw), "test file layout")
        open(src, "wb").write(huge)
        r = run(src, "-o", out, "-f", "-t", "fits", expect_ok=False)
        check(r.returncode == 1 and "implausible" in r.stderr, f"an implausible image size is refused: {r.stderr.strip()[:160]}")
        # --verify finds .fits.fz files in a directory
        sub = os.path.join(d, "dir")
        os.makedirs(sub)
        fits.HDUList([fits.PrimaryHDU(), comp_hdu(image(np.int16, (60, 80)), None, compression_type="GZIP_2")]).writeto(
            os.path.join(sub, "a.fits.fz"), overwrite=True, checksum=True)
        open(os.path.join(sub, "notes.fz"), "wb").write(b"not an image")
        r = run("--verify", sub, expect_ok=False)
        check(r.returncode == 0 and "a.fits.fz: OK (FITS, 2 HDUs; 2 checksums verified)" in r.stdout and "notes" not in r.stdout,
              f"--verify on a directory with a .fits.fz: {r.stdout.strip()[:200]}")

    # CFITSIO's own tools, when installed: fpack writes, funpack is the reference
    if not HAVE_FPACK:
        skipped.append("tile-compressed FITS written by fpack (install the CFITSIO tools fpack and funpack)")
        return
    n = 0
    for dtype in (np.uint8, np.int16, np.uint16, np.int32, np.float32, np.float64):
        shape = (41, 67) if n % 2 else (3, 30, 45)
        n += 1
        a = image(dtype, shape)
        h = fits.PrimaryHDU(a)
        h.header["OBJECT"] = "M 31"
        base = os.path.join(d, "p.fits")
        fits.HDUList([h, fits.ImageHDU(image(np.int16, (20, 33)), name="SECOND")]).writeto(base, overwrite=True)
        options = [[], ["-g"], ["-g2"], ["-w"]]
        if np.issubdtype(dtype, np.floating):
            options += [["-q", "0", "-g"], ["-q", "4"], ["-q", "-0.5"]]
        for o in options:
            for f in (base + ".fz", os.path.join(d, "u.fits")):
                if os.path.exists(f):
                    os.remove(f)
            p1 = subprocess.run(["fpack", *o, base], capture_output=True, text=True)
            p2 = subprocess.run(["funpack", "-O", os.path.join(d, "u.fits"), base + ".fz"], capture_output=True, text=True)
            if p1.returncode or p2.returncode:
                check(False, f"fpack {o} / funpack failed: {p1.stderr.strip()[:100]} {p2.stderr.strip()[:100]}")
                continue
            run(base + ".fz", "-o", out, "-f", "-q", "-t", "fits")
            slack = [rounding_slack(base + ".fz", i + 1) for i in range(2)]
            with fits.open(os.path.join(d, "u.fits")) as hu, fits.open(out) as hx:
                check(len(hx) == 2 and all(same(np.array(hx[i].data), np.array(hu[i].data), slack[i], "fpack") for i in range(2)) and
                      hx[0].header["OBJECT"] == "M 31" and hx[1].header["EXTNAME"] == "SECOND",
                      f"fpack {' '.join(o) or '(default)'} {np.dtype(dtype).name} {shape}: equals funpack's output")


def raw_tiles(path, hdu=1):
    """The binary table of a tile-compressed image as it is stored: its header and the compressed tiles."""
    with fits.open(path, disable_image_compression=True) as h:
        table = h[hdu]
        return table.header.copy(), [np.asarray(x, dtype=np.uint8).tobytes() for x in table.data["COMPRESSED_DATA"]]


def fits_units(path):
    """The HDUs of a FITS file as they are stored: (cards, data unit bytes) each."""
    raw = open(path, "rb").read()
    units, pos = [], 0
    while pos < len(raw):
        cards = []
        while True:
            block = raw[pos:pos + 2880]
            pos += 2880
            cards += [block[i:i + 80].decode("ascii") for i in range(0, 2880, 80)]
            if any(c.startswith("END" + " " * 77) for c in cards[-36:]):
                break
        cards = cards[:next(i for i, c in enumerate(cards) if c.startswith("END" + " " * 77))]
        value = {c[:8].strip(): c[10:30].strip() for c in cards if c[8:10] == "= "}
        size = abs(int(value["BITPIX"])) // 8 * (int(value.get("PCOUNT", 0)) + (
            int(np.prod([int(value[f"NAXIS{i + 1}"]) for i in range(int(value["NAXIS"]))])) if int(value["NAXIS"]) else 0))
        units.append((cards, raw[pos:pos + size]))
        pos += (size + 2879) // 2880 * 2880
    return units


STORAGE_CARDS = ("SIMPLE", "XTENSION", "BITPIX", "NAXIS", "NAXIS1", "NAXIS2", "NAXIS3", "EXTEND", "PCOUNT", "GCOUNT",
                 "BZERO", "BSCALE", "CHECKSUM", "DATASUM")


def test_fits_tile_writing():
    """Writing tile-compressed FITS (-c). The references: the plain FITS file the same input
    gives, from which astropy must read the same pixels and cards; astropy's (CFITSIO's) Rice
    encoder and fpack, which must produce the same bytes; Python's gzip for the GZIP tiles;
    funpack, which must restore the plain file; fitsverify; and xisfconv's own reader."""
    import gzip
    import warnings
    d = os.path.join(TMP, "fzw")
    os.makedirs(d, exist_ok=True)
    rng = np.random.default_rng(1203)

    def image(dtype, shape, kind="smooth"):
        """HxWxC. smooth: compressible; random: every block is stored plain; steps: the
        differences of neighbours wrap around; flat: all differences are zero."""
        if np.issubdtype(dtype, np.floating):
            a = np.sin(np.indices(shape).sum(0) / 9.0) * 0.4 + 0.5 + rng.normal(0, 0.01, shape)
            if kind == "random":
                a = rng.normal(0, 1e6, shape)
            a = a.astype(dtype)
            a.flat[0], a.flat[-1] = np.pi / 4, -0.0
            return a
        info = np.iinfo(dtype)
        if kind == "random":
            return rng.integers(0, info.max, shape, dtype=dtype, endpoint=True)
        if kind == "steps":
            return np.where(np.indices(shape).sum(0) % 3 == 0, np.full(shape, info.max, dtype), (np.indices(shape)[1] % 3).astype(dtype))
        if kind == "flat":
            return np.full(shape, info.max // 3, dtype)
        a = ((np.indices(shape).sum(0) * 5 + rng.integers(0, 12, shape)) % (min(int(info.max), 2 ** 40) + 1)).astype(dtype)
        a.flat[0], a.flat[1 % a.size] = info.max, 0
        return a

    def fitsverify(path):
        r = subprocess.run(["fitsverify", "-q", path], capture_output=True, text=True)
        return r.stdout.strip().replace(path, "").replace(" ,", ",")

    def checked(path, label, like=None):
        """fitsverify has nothing to say, or (`like`: the plain file) what it says about the plain
        file: a keyword without a value earns a warning in both."""
        if HAVE_FITSVERIFY:
            verdict = fitsverify(path)
            check("verification OK" in verdict or (like is not None and verdict.split(":")[-1] == fitsverify(like).split(":")[-1]),
                  f"{label}: fitsverify: {verdict[:200]}")
        check(os.path.getsize(path) % 2880 == 0 and not os.path.exists(path + ".part"), f"{label}: whole blocks, no .part file")
        r = run("--verify", path, expect_ok=False)
        check(r.returncode == 0 and ": OK (FITS" in r.stdout, f"{label}: --verify: {r.stdout.strip()[:160]}")

    def compare_files(label, plain, packed, algorithms):
        """`packed` holds the images of `plain`, tile-compressed with `algorithms` (one per image)."""
        checked(packed, label, plain)
        units = fits_units(plain)
        stored_plain = [a is None for a in algorithms]   # 64-bit integers are not compressed
        first = 0 if stored_plain[0] else 1              # the HDU of the first image
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with fits.open(plain) as hp, fits.open(packed) as hc:
                check(len(hc) == len(hp) + first and (first == 0 or (hc[0].data is None and hc[0].header["NAXIS"] == 0)),
                      f"{label}: {'an empty primary HDU, then ' if first else ''}{len(hp)} image(s)")
                for i, algorithm in enumerate(algorithms):
                    a, b = np.array(hp[i].data), np.array(hc[i + first].data)
                    check(type(hc[i + first]).__name__ == ("CompImageHDU" if algorithm else "ImageHDU" if i + first else "PrimaryHDU") and
                          a.shape == b.shape and a.dtype.newbyteorder("=") == b.dtype.newbyteorder("=") and a.tobytes() == b.astype(a.dtype).tobytes(),
                          f"{label}: astropy reads image {i} with the pixels of the plain file ({a.dtype}, {a.shape}, {algorithm or 'not compressed'})")
                    mine = [c.image for c in hp[i].header.cards if c.keyword not in STORAGE_CARDS]
                    theirs = [c.image for c in hc[i + first].header.cards if c.keyword not in STORAGE_CARDS and
                              not (c.keyword == "EXTNAME" and c.value == "COMPRESSED_IMAGE")]
                    check(mine == theirs, f"{label}: image {i} has the cards of the plain file: {set(mine) ^ set(theirs)}")
            for i, algorithm in enumerate(algorithms):
                if algorithm is None:
                    continue
                header, tiles = raw_tiles(packed, i + first)
                cards, data = units[i]
                value = {c[:8].strip(): c[10:30].strip() for c in cards if c[8:10] == "= "}
                width, sample = int(value["NAXIS1"]), abs(int(value["BITPIX"])) // 8
                rows = [data[k:k + width * sample] for k in range(0, len(data), width * sample)]
                check(header["ZCMPTYPE"] == algorithm and header["NAXIS2"] == len(rows) == len(tiles) and
                      header["PCOUNT"] == sum(map(len, tiles)) and header["TFORM1"] == f"1PB({max(map(len, tiles))})" and
                      header["ZTILE1"] == width and header["ZTILE2"] == 1 and header.get("ZTILE3", 1) == 1 and
                      ("ZSIMPLE" in header) == (i == 0) and (header.get("ZTENSION") == "IMAGE") == (i > 0) and
                      (header.get("ZPCOUNT"), header.get("ZGCOUNT")) == ((0, 1) if i else (None, None)) and
                      header.get("ZQUANTIZ") == ("NONE" if int(value["BITPIX"]) < 0 else None),
                      f"{label}: image {i}: {algorithm}, a tile per row, sizes as stored "
                      f"({header['ZCMPTYPE']}, {header['NAXIS2']} rows, PCOUNT {header['PCOUNT']}, {header['TFORM1']})")
                if algorithm == "RICE_1":
                    # the same bytes as astropy's encoder, which is CFITSIO's
                    check(header["ZNAME1"] == "BLOCKSIZE" and header["ZVAL1"] == 32 and header["ZNAME2"] == "BYTEPIX" and
                          header["ZVAL2"] == sample, f"{label}: image {i}: Rice parameters")
                    ref = os.path.join(d, "ref.fits.fz")
                    with fits.open(plain) as hp:
                        fits.HDUList([fits.PrimaryHDU(), fits.CompImageHDU(np.array(hp[i].data), compression_type="RICE_1")]).writeto(ref, overwrite=True)
                    theirs, their_tiles = raw_tiles(ref)
                    if theirs["ZVAL2"] == sample and theirs["ZTILE1"] == width and theirs.get("ZTILE2", 1) == 1:
                        check(tiles == their_tiles, f"{label}: image {i}: the Rice-coded tiles are astropy's, byte for byte "
                              f"({sum(a == b for a, b in zip(tiles, their_tiles))} of {len(tiles)})")
                    else:  # pragma: no cover
                        skipped.append("Rice tiles against astropy's encoder (it chose other parameters)")
                else:
                    # gzip streams that Python's gzip decodes to the rows, bytes regrouped by significance
                    ok = True
                    for tile, row in zip(tiles, rows):
                        plain_bytes = np.frombuffer(gzip.decompress(tile), np.uint8)
                        if algorithm == "GZIP_2":
                            plain_bytes = plain_bytes.reshape(sample, -1).T
                        ok = ok and plain_bytes.tobytes() == row and tile[:4] == b"\x1f\x8b\x08\x00" and tile[4:8] == b"\0\0\0\0" and tile[9] == 255
                    check(ok, f"{label}: image {i}: every tile is a gzip stream of its row, with nothing in its header that "
                          "depends on the system or the time")
        # CFITSIO: funpack restores the plain file
        if HAVE_FPACK:
            restored = os.path.join(d, "funpacked.fits")
            if os.path.exists(restored):
                os.remove(restored)
            r = subprocess.run(["funpack", "-O", restored, packed], capture_output=True, text=True)
            if r.returncode:
                check(False, f"{label}: funpack: {r.stderr.strip()[:200]}")
            else:
                theirs = [([c for c in cards if c[:8].strip() not in ("CHECKSUM", "DATASUM")], data) for cards, data in fits_units(restored)]
                check(theirs == units, f"{label}: funpack restores the plain file, cards and data")
        # and xisfconv itself
        back = os.path.join(d, "unpacked.fits")
        r = run(packed, "-o", back, "-f", "-q", "-t", "fits", expect_ok=False)
        if all(stored_plain):
            check(r.returncode != 0 and "already a FITS file" in r.stderr, f"{label}: nothing in it is compressed: a plain FITS file")
        else:
            check(r.returncode == 0 and [data for _, data in fits_units(back)] == [data for _, data in units],
                  f"{label}: xisfconv unpacks it to the same data: {r.stderr.strip()[:120]}")

    def expected(dtype, codec):
        if dtype == np.uint64:
            return None   # stays a plain image
        if codec == "zlib" or dtype in (np.float32, np.float64):
            return "GZIP_1" if dtype == np.uint8 else "GZIP_2"
        return "RICE_1"

    src = os.path.join(d, "in.xisf")
    plain = os.path.join(d, "plain.fits")
    packed = os.path.join(d, "packed.fits.fz")

    # every sample type, gray and RGB, both codecs; widths around the Rice block size of 32 pixels
    for dtype in (np.uint8, np.uint16, np.uint32, np.uint64, np.float32, np.float64):
        for shape in ((37, 53, 1), (20, 31, 3), (9, 64, 1), (6, 65, 3), (5, 1, 1), (1, 7, 1), (1, 1, 1), (3, 33, 1)):
            write_xisf(src, [image_entry(image(dtype, shape))])
            run(src, "-o", plain, "-f", "-q")
            for flags in (["-c"], ["--codec", "zlib"]):
                r = run(src, "-o", packed, "-f", *flags)
                compare_files(f"{np.dtype(dtype).name} {shape} {' '.join(flags)}", plain, packed, [expected(dtype, flags[-1])])
                check(("64-bit integer images are not tile-compressed" in r.stderr) == (dtype == np.uint64),
                      f"{np.dtype(dtype).name}: a warning when an image is left uncompressed, and only then: {r.stderr.strip()[:120]}")
    # data that does not compress, differences that wrap around, constant rows
    for dtype in (np.uint8, np.uint16, np.uint32, np.float32):
        for kind in ("random", "steps", "flat"):
            write_xisf(src, [image_entry(image(dtype, (24, 131, 1), kind))])
            run(src, "-o", plain, "-f", "-q")
            run(src, "-o", packed, "-f", "-q", "-c")
            compare_files(f"{np.dtype(dtype).name} {kind}", plain, packed, [expected(dtype, "")])
            if kind == "random":
                check(os.path.getsize(packed) <= os.path.getsize(plain) * 1.05 + 4 * 2880,
                      f"{np.dtype(dtype).name}: data that does not compress grows by little ({os.path.getsize(plain)} -> {os.path.getsize(packed)})")
    # Rice coding at every split position: noise of every strength, and one outlier in a quiet
    # row (a long run of zero bits in the code); gzip on rows that do not compress at all
    for dtype in (np.uint8, np.uint16, np.uint32):
        bits = 8 * np.dtype(dtype).itemsize
        rows = []
        for k in range(bits):
            noise = np.rint(rng.normal(0, 2.0 ** k / 3, 257)).astype(np.int64)
            rows.append(((1 << (bits - 1)) + noise) % (1 << bits))
            spike = np.full(257, 1000 % (1 << bits), np.int64)
            spike[rng.integers(0, 257)] = (1 << bits) - 1 - (k % 3)
            spike[k * 7 % 257] = k
            rows.append(spike)
        write_xisf(src, [image_entry(np.array(rows).astype(dtype)[:, :, None])])
        run(src, "-o", plain, "-f", "-q")
        run(src, "-o", packed, "-f", "-q", "-c")
        compare_files(f"{np.dtype(dtype).name} noise of every strength and outliers", plain, packed, ["RICE_1"])
    write_xisf(src, [image_entry(image(np.float32, (3, 5000, 1), "random"))])
    run(src, "-o", plain, "-f", "-q")
    run(src, "-o", packed, "-f", "-q", "-c")
    compare_files("float32 rows of 20 kB that do not compress", plain, packed, ["GZIP_2"])
    # floating point values of every kind come back bit for bit: NaN (also one with a payload),
    # infinities, the negative zero, the smallest and the largest numbers
    for dtype, bits in ((np.float32, np.uint32), (np.float64, np.uint64)):
        a = image(dtype, (12, 40, 1))
        tiny = np.finfo(dtype)
        a[1, :9, 0] = [np.nan, np.inf, -np.inf, -0.0, tiny.tiny, tiny.smallest_subnormal, tiny.max, -tiny.max, tiny.eps]
        a[2, 3, 0] = np.array([np.array(np.nan, dtype).view(bits) | 0x1234], bits).view(dtype)[0]
        write_xisf(src, [image_entry(a)])
        run(src, "-o", plain, "-f", "-q")
        run(src, "-o", packed, "-f", "-q", "-c")
        compare_files(f"{np.dtype(dtype).name} special values", plain, packed, ["GZIP_2"])
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            got = np.array(fits.getdata(packed, 1))[::-1]
        check(got.astype(dtype).tobytes() == a[:, :, 0].tobytes(), f"{np.dtype(dtype).name}: NaN, infinities and -0.0 survive bit for bit")
    # the same input gives the same bytes
    first = open(packed, "rb").read()
    run(src, "-o", packed, "-f", "-q", "-c")
    check(open(packed, "rb").read() == first, "writing twice gives the same file")

    # several images, names, keywords of every kind, top-down rows, another sample format
    e1 = with_id(image_entry(image(np.uint16, (16, 24, 3)), children=torture_keywords(), codec="zlib"), "main")
    e2 = with_id(image_entry(image(np.float32, (10, 12, 1)), codec="lz4"), "mask")
    e3 = image_entry(image(np.uint8, (7, 9, 1)))
    write_xisf(src, [e1, e2, e3])
    for flags in ([], ["--top-down"], ["-b", "u16"], ["-i", "1"]):
        run(src, "-o", plain, "-f", "-q", *flags)
        run(src, "-o", packed, "-f", "-q", "-c", *flags)
        algorithms = ["GZIP_2"] if flags == ["-i", "1"] else ["RICE_1", "RICE_1" if "-b" in flags else "GZIP_2", "RICE_1"]
        compare_files(f"three images {' '.join(flags)}".strip(), plain, packed, algorithms)
    header, _ = raw_tiles(packed)
    check(header["EXTNAME"] == "mask", "an image keeps its name as EXTNAME")
    # 64-bit integers among other images: the plain image is the primary HDU when it comes first
    wide = with_id(image_entry(image(np.uint64, (6, 9, 1))), "wide")
    for label, entries, algorithms in (("64-bit integers first", [wide, e1, e2], [None, "RICE_1", "GZIP_2"]),
                                        ("64-bit integers between", [e3, wide, e1], ["RICE_1", None, "RICE_1"])):
        write_xisf(src, entries)
        run(src, "-o", plain, "-f", "-q")
        r = run(src, "-o", packed, "-f", "-c")
        compare_files(label, plain, packed, algorithms)
        check("image 'wide' is stored as it is" in r.stderr, f"{label}: the warning names the image: {r.stderr.strip()[:160]}")
    write_xisf(src, [e1, e2, e3])
    run(src, "-o", packed, "-f", "-q", "-c")
    names = [raw_tiles(packed, i)[0]["EXTNAME"] for i in (1, 2, 3)]
    check(names == ["main", "mask", "COMPRESSED_IMAGE"], f"an image without a name is a table named COMPRESSED_IMAGE: {names}")
    r = run(packed, "-I")
    check(r.stdout.count("tile-compressed (RICE_1)") == 2 and "tile-compressed (GZIP_2)" in r.stdout and "COMPRESSED_IMAGE" not in r.stdout,
          "--info shows the compression, not the name of the table")
    # the way back to XISF is the same from the plain and from the compressed file
    run(src, "-o", plain, "-f", "-q")
    x1, x2 = os.path.join(d, "from_plain.xisf"), os.path.join(d, "from_packed.xisf")
    run(plain, "-o", x1, "-f", "-q")
    run(packed, "-o", x2, "-f", "-q")
    for n in range(3):
        check(xisf_keywords(x1, n) == xisf_keywords(x2, n) and np.array_equal(read_xisf_any(x1, n), read_xisf_any(x2, n)),
              f"image {n}: plain and tile-compressed FITS convert to the same XISF image")
    # two images without a name: both tables are called COMPRESSED_IMAGE, as in fpack's files
    write_xisf(src, [image_entry(image(np.uint16, (5, 6, 1))), image_entry(image(np.float32, (4, 7, 1)))])
    run(src, "-o", plain, "-f", "-q")
    run(src, "-o", packed, "-f", "-q", "-c")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with fits.open(plain) as hp, fits.open(packed) as hc:
            check(len(hc) == 3 and all(np.array(hp[i].data).tobytes() == np.array(hc[i + 1].data).astype(hp[i].data.dtype).tobytes() for i in (0, 1)),
                  "two images without a name")
    # a name too long for a card is shortened, in the plain and in the compressed file
    long_name = "N" + "o" * 80 + "'s"
    write_xisf(src, [with_id(image_entry(image(np.uint16, (5, 6, 1))), "L" * 90)])
    for target, flags, hdu in ((plain, [], 0), (packed, ["-c"], 1)):
        r = run(src, "-o", target, "-f", *flags)
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            with fits.open(target) as h:
                h.verify("exception")
                check(h[hdu].header["EXTNAME"] == "L" * 68 and "shortened to fit a FITS card" in r.stderr,
                      f"a long image name is shortened ({'compressed' if flags else 'plain'}): {h[hdu].header['EXTNAME']!r}")
    # Keywords that describe a compressed image are the writer's: an image that brings its own
    # (they would be taken for the real ones) loses them in a compressed file, with a warning.
    theirs = "".join(f'<FITSKeyword name="{name}" value="{value}" comment=""/>' for name, value in (
        ("THEAP", "5760"), ("TSCAL1", "2.0"), ("TZERO1", "5.0"), ("ZSCALE", "2.0"), ("ZZERO", "7.0"), ("ZBLANK", "3"), ("ZNAME3", "'NOISEBIT'"),
        ("ZTILE1", "3"), ("TFORM1", "'1PJ'"), ("ZBITPIX", "8"), ("ZNAXIS1", "5"), ("ZSIMPLE", "F"), ("ZPCOUNT", "10"), ("ZHECKSUM", "'x'"),
        ("ZDATASUM", "'1'"), ("TFIELDS", "3"), ("ZIMAGE", "F"), ("ZCMPTYPE", "'PLIO_1'"), ("ZVAL1", "16"), ("ZQUANTIZ", "'NO_DITHER'"),
        ("TTYPE1", "'X'"), ("OBJECT", "'M 31'"), ("ZENITH", "12.5"), ("TELESCOP", "'T'")))
    for dtype in (np.uint16, np.float32):
        write_xisf(src, [image_entry(image(dtype, (6, 10, 1)), children=theirs)])
        run(src, "-o", plain, "-f", "-q")
        r = run(src, "-o", packed, "-f", "-c")
        checked(packed, f"{np.dtype(dtype).name} with keywords of a compressed image")
        header, _ = raw_tiles(packed)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with fits.open(packed) as hc:
                kept = hc[1].header
                check(np.array(hc[1].data).astype(dtype).tobytes() == np.array(fits.getdata(plain)).astype(dtype).tobytes() and kept["OBJECT"] == "M 31" and
                      kept["ZENITH"] == 12.5 and kept["TELESCOP"] == "T" and "ZSCALE" not in header and "THEAP" not in header and
                      header["TFIELDS"] == 1 and header["ZBITPIX"] == (16 if dtype == np.uint16 else -32) and
                      len([c for c in header.cards if c.keyword == "TFORM1"]) == 1 and header["ZCMPTYPE"] in ("RICE_1", "GZIP_2"),
                      f"{np.dtype(dtype).name}: keywords of a compressed image are not taken over, the others are")
        check(r.stderr.count("describes a tile-compressed image") == 21 and "keyword ZSCALE" in r.stderr and "ZENITH" not in r.stderr,
              f"each of them is named in a warning, once: {r.stderr.count('describes a tile-compressed image')}")
        if HAVE_FPACK:
            restored = os.path.join(d, "funpacked.fits")
            if os.path.exists(restored):
                os.remove(restored)
            subprocess.run(["funpack", "-O", restored, packed], capture_output=True, text=True)
            check(os.path.exists(restored) and fits_units(restored)[0][1] == fits_units(plain)[0][1], "and funpack restores the pixels")
    # an EXTNAME among the keywords of an image without a name is its name
    write_xisf(src, [image_entry(image(np.uint16, (5, 6, 1)), children='<FITSKeyword name="EXTNAME" value="\'SCI\'" comment=""/>')])
    run(src, "-o", packed, "-f", "-q", "-c")
    cards = [c.image for c in raw_tiles(packed)[0].cards if c.keyword == "EXTNAME"]
    check(len(cards) == 1 and "'SCI" in cards[0], f"an EXTNAME keyword is not doubled: {cards}")

    # names: -c names the output image.fits.fz; a name that ends in .fz asks for compression by itself
    sub = os.path.join(d, "names")
    os.makedirs(sub, exist_ok=True)
    one = os.path.join(sub, "frame.xisf")
    write_xisf(one, [image_entry(image(np.uint16, (12, 40, 1)))])
    r = run(one, "-c")
    check(os.path.exists(os.path.join(sub, "frame.fits.fz")) and "frame.fits.fz" in r.stdout and not os.path.exists(os.path.join(sub, "frame.fits")),
          f"-c writes image.fits.fz: {r.stdout.strip()}")
    r = run(one, "-o", os.path.join(sub, "byname.fits.fz"))
    check(raw_tiles(os.path.join(sub, "byname.fits.fz"))[0]["ZCMPTYPE"] == "RICE_1", "-o image.fits.fz is written tile-compressed without -c")
    run(one, "-o", os.path.join(sub, "other.fz"), "-t", "fits")
    check(raw_tiles(os.path.join(sub, "other.fz"))[0]["ZCMPTYPE"] == "RICE_1", "-o name.fz -t fits is written tile-compressed")
    run(one, "-o", os.path.join(sub, "named.fits"), "-c")
    check(raw_tiles(os.path.join(sub, "named.fits"))[0]["ZCMPTYPE"] == "RICE_1", "-o image.fits -c is tile-compressed under that name")
    run(one, "-o", os.path.join(sub, "plain.fits"), "--codec", "none")
    with fits.open(os.path.join(sub, "plain.fits")) as h:
        check(len(h) == 1 and h[0].data is not None, "--codec none leaves FITS output plain")
    run(one, "-o", os.path.join(sub, "none.fits.fz"), "--codec", "none")
    check(raw_tiles(os.path.join(sub, "none.fits.fz"))[0]["ZCMPTYPE"] == "RICE_1", "but the name image.fits.fz wins over --codec none")
    r = run(one, "-o", os.path.join(sub, "x.fts.fz"), expect_ok=False)
    check(r.returncode == 0 and raw_tiles(os.path.join(sub, "x.fts.fz"))[0]["ZIMAGE"], ".fts.fz is a FITS name, too")
    r = run(one, "-o", os.path.join(sub, "x.tif.fz"), expect_ok=False)
    check(r.returncode != 0 and "cannot infer output format" in r.stderr, f"image.tif.fz is no output name: {r.stderr.strip()[:120]}")
    r = run(one, "-o", os.path.join(sub, "z.fits"), "--codec", "zstd", expect_ok=False)
    zstd_built = "zstd" in subprocess.run([EXE, "--version"], capture_output=True, text=True).stdout
    check(r.returncode != 0 and ("FITS has no Zstandard compression" in r.stderr or not zstd_built) and
          not os.path.exists(os.path.join(sub, "z.fits")) and not os.path.exists(os.path.join(sub, "z.fits.part")),
          f"--codec zstd is refused for FITS: {r.stderr.strip()[:160]}")
    r = run(one, "-o", os.path.join(sub, "byname.fits.fz"), "-c", expect_ok=False)
    check(r.returncode != 0 and "already exists" in r.stderr, "an existing output is kept without --force")

    # FITS -> FITS: packing a plain file, as fpack does; the tiles are fpack's
    base = os.path.join(sub, "camera.fits")
    a = image(np.uint16, (33, 70))
    h = fits.PrimaryHDU(a)
    h.header["OBJECT"] = "M 31"
    h.header["EXPTIME"] = (120.0, "seconds")
    fits.HDUList([h, fits.ImageHDU(image(np.uint8, (9, 11)), name="THUMB")]).writeto(base, overwrite=True)
    r = run(base, "-t", "fits", expect_ok=False)
    check(r.returncode != 0 and "--compress" in r.stderr, f"FITS to plain FITS is still refused, and says how to compress: {r.stderr.strip()[:200]}")
    r = run(base, "-t", "fits", "-c", "-q")
    out = os.path.join(sub, "camera.fits.fz")
    check(os.path.exists(out), f"FITS -> FITS with -c writes image.fits.fz: {r.stdout.strip()}")
    checked(out, "FITS -> tile-compressed FITS")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with fits.open(out) as hc:
            check(len(hc) == 3 and np.array_equal(hc[1].data, a) and hc[1].data.dtype == np.uint16 and hc[1].header["OBJECT"] == "M 31" and
                  hc[1].header["EXPTIME"] == 120.0 and hc[2].header["EXTNAME"] == "THUMB" and hc[2].data.dtype == np.uint8,
                  "the packed file holds the images and keywords of the plain one")
    if HAVE_FPACK:
        ref = os.path.join(sub, "fpacked.fits.fz")
        subprocess.run(["fpack", "-O", ref, base], check=True, capture_output=True)
        check(all(raw_tiles(out, i)[1] == raw_tiles(ref, i)[1] for i in (1, 2)), "the tiles are those fpack writes for the same file")
    r = run(out, "-t", "fits", "-c", "-f", expect_ok=False)
    check(r.returncode != 0 and os.path.exists(out), f"a file is not packed onto itself: {r.stderr.strip()[:160]}")
    again = os.path.join(sub, "again.fits.fz")
    run(out, "-o", again, "-q", "--codec", "zlib")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with fits.open(again) as hc:
            check(raw_tiles(again)[0]["ZCMPTYPE"] == "GZIP_2" and np.array_equal(hc[1].data, a) and np.array_equal(hc[2].data, fits.getdata(base, 1)),
                  "a tile-compressed file is written again with another algorithm")
    # ASDF -> tile-compressed FITS
    asdf_file = os.path.join(sub, "frame.asdf")
    run(one, "-o", asdf_file, "-q")
    run(asdf_file, "-t", "fits", "-c", "-q", "-f")
    run(one, "-o", plain, "-f", "-q")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        check(np.array_equal(fits.getdata(os.path.join(sub, "frame.fits.fz"), 1), fits.getdata(plain)), "ASDF -> tile-compressed FITS")


# ---------------------------------------------------------------- XISF properties through FITS and ASDF

ELEMENT_BYTES = {"I8": 1, "UI8": 1, "I16": 2, "UI16": 2, "I32": 4, "UI32": 4, "I64": 8, "UI64": 8, "F32": 4, "F64": 8,
                 "C32": 8, "C64": 16, "I": 4, "UI": 4, "F": 4, "": 8}   # (IVector is I32Vector, Vector is F64Vector ...)
ELEMENT_DTYPE = {"I8": "i1", "UI8": "u1", "I16": "<i2", "UI16": "<u2", "I32": "<i4", "UI32": "<u4", "I64": "<i8",
                 "UI64": "<u8", "F32": "<f4", "F64": "<f8", "C32": "<c8", "C64": "<c16", "I": "<i4", "UI": "<u4", "F": "<f4",
                 "": "<f8"}


def element_of(ptype):
    """The element type of a vector or matrix type, None for every other type."""
    if ptype == "ByteArray":
        return "UI8"
    if ptype[-6:] in ("Vector", "Matrix") and ptype[:-6] in ELEMENT_BYTES:
        return ptype[:-6]
    return None


def xisf_properties(path):
    """The properties of an XISF file, read without xisfconv: (one list per image, the list of the file).
    A property is a dict with id, type, comment, format and value: ("text", bytes) for what is text,
    ("text in a block", bytes) for a String that is stored as a data block, and
    ("data", (length, rows, columns), little-endian bytes) for every other data block."""
    import xml.etree.ElementTree as ET
    raw, hdr = xisf_header(path)
    root = ET.fromstring(hdr)
    blocks = {id(b["element"]): b for b in xisf_blocks(path, root)}

    def local(el):
        return el.tag.split("}")[-1]

    def read(parent):
        out = []
        for el in parent:
            if local(el) != "Property":
                continue
            p = {"id": el.get("id"), "type": el.get("type"), "comment": el.get("comment", ""), "format": el.get("format", "")}
            b = blocks.get(id(el))
            if b is None:
                p["value"] = ("text", (el.get("value") if el.get("value") is not None else el.text or "").encode())
            elif p["type"] == "String":
                p["value"] = ("text in a block", b["data"])
            else:
                data = b["data"]
                name = element_of(p["type"])
                if name and el.get("byteOrder") == "big":
                    part = ELEMENT_BYTES[name] // (2 if name.startswith("C") else 1)
                    data = np.frombuffer(data, np.uint8).reshape(-1, part)[:, ::-1].tobytes()
                shape = tuple(None if el.get(k) is None else int(el.get(k)) for k in ("length", "rows", "columns"))
                p["value"] = ("data", shape, data)
            out.append(p)
        return out

    return [read(el) for el in root if local(el) == "Image"], [p for el in root if local(el) == "Metadata" for p in read(el)]


def property_zoo():
    """Properties of every type and in every form XISF has for them: (their XML, {id: the text of a
    scalar after it went through an ASDF tree, where that is not the text it had})."""
    import html
    xml, through_asdf = [], {}

    def attribute(text):
        return html.escape(text, quote=True).replace("\t", "&#9;").replace("\n", "&#10;").replace("\r", "&#13;")

    def scalar(pid, ptype, value, asdf_text=None, extra=""):
        xml.append(f'<Property id="{pid}" type="{ptype}" value="{attribute(value)}"{extra}/>')
        if asdf_text is not None:
            through_asdf[pid] = asdf_text

    def text(pid, value, extra=""):
        xml.append(f'<Property id="{pid}" type="String"{extra}>{html.escape(value, quote=False)}</Property>')

    def block(pid, ptype, data, shape="", **kw):
        b = Block(data, location="inline", **kw)
        attrs = "".join(f' {k}="{v}"' for k, v in b.attrs.items())
        xml.append(f'<Property id="{pid}" type="{ptype}"{shape} location="inline:{b.encoding}"{attrs}>{b.text}</Property>')

    rng = np.random.default_rng(77)

    def numbers(name, n):
        dt = np.dtype(ELEMENT_DTYPE[name])
        if dt.kind == "c":
            return (rng.normal(size=n) + 1j * rng.normal(size=n)).astype(dt)
        if dt.kind == "f":
            a = rng.normal(size=n).astype(dt)
            if n > 3:
                a[1], a[2], a[3] = np.nan, -np.inf, -0.0
            return a
        info = np.iinfo(dt)
        a = rng.integers(info.min, info.max, n, dtype=dt, endpoint=True)
        if n > 1:
            a[0], a[1] = info.min, info.max
        return a

    # --- scalars
    scalar("S:True", "Boolean", "true")
    scalar("S:False", "Boolean", "false")
    scalar("S:One", "Boolean", "1", "true")
    scalar("S:Zero", "Boolean", "0", "false")
    scalar("S:Capital", "Boolean", "True", "true")     # as the xisf package of Python writes it
    for ptype, value in (("Int8", "-128"), ("UInt8", "255"), ("Int16", "-32768"), ("UInt16", "65535"),
                         ("Int32", "-2147483648"), ("UInt32", "4294967295"), ("Int64", "-9223372036854775808"),
                         ("Int64", "9223372036854775807"), ("UInt64", "18446744073709551615"), ("UInt64", "0")):
        scalar(f"S:{ptype}:{value}", ptype, value)
    scalar("S:Plus", "Int32", "+7", "7")
    scalar("S:LeadingZeros", "Int32", "007", "7")
    scalar("S:Hexadecimal", "UInt32", "0x1F")          # not a decimal number: taken along as text
    scalar("S:NotAnInteger", "Int16", "twelve")
    for n, (value, after) in enumerate((("0.1", None), ("3.141592653589793", None), ("2000", None), ("-12", None),
                                        ("1.5E+300", None), ("-2.5e-07", None), ("0.30000000000000004", None),
                                        ("1e-05", "1.0e-05"), ("6.02e23", "6.02e+23"), (".5", "0.5"), ("5.", "5.0"),
                                        ("-0", "-0.0"), ("+1.25", "1.25"), ("nan", "nan"), ("NaN", "nan"), ("inf", "inf"),
                                        ("-inf", "-inf"), ("not a number", None))):
        scalar(f"S:Float64:{n}", "Float64", value, after)
    scalar("S:Float32", "Float32", "0.1")
    scalar("S:Complex32", "Complex32", "(1.5,-2)", "(1.5,-2.0)")
    scalar("S:Complex64", "Complex64", "(0.25, 1e-3)", "(0.25,0.001)")
    scalar("S:ComplexOdd", "Complex64", "1+2i")        # not the form of a complex number: taken along as text
    scalar("S:Time", "TimePoint", "2024-03-05T21:15:02.5Z")
    scalar("S:Commented", "Float32", "0.25", extra=' comment="exposure in &#956;s, &quot;quoted&quot; &amp; more" format="%.3f"')
    scalar("X:Thing", "Frobnication", "7 of 9")        # a type nobody knows
    # --- strings
    text("T:Plain", "hello world")
    scalar("T:Attribute", "String", "in an attribute")
    text("T:Utf8", "Ångström ☄ \U0001f30c مرحبا")
    text("T:Markup", '<a href="x">1 & 2</a> ]]> \'single\'')
    text("T:Empty", "")
    text("T:Lines", "line 1\nline 2\n\ttabbed")
    xml.append('<Property id="T:CDATA" type="String"><![CDATA[<raw> & data]]> and more</Property>')
    block("T:Padded", "String", b"  padded \n")
    block("T:Control", "String", b"bell\x07 escape\x1b unit\x1f delete\x7f")
    block("T:Nul", "String", b"a\x00b")
    block("T:NotUtf8", "String", b"caf\xe9 \xff\xfe\xc0\x80")
    # as PixInsight stores the serialization of a spline when it is long: a compressed block of text with CR LF
    block("T:BlockCrLf", "String", b"5000 3000\r\n0000 BA00\r\n" * 300 + b"\0\0", codec="zlib")
    scalar("T:LoneCr", "String", "classic\rline ends")
    scalar("T:Breaks", "String", "tab\there\nnew line")
    text("T:Long", "".join(chr(0x61 + i % 26) + (" " if i % 7 == 0 else "") for i in range(19999)))
    text("T:EdgeBlanks", "  blanks at both ends, in the header  ")
    text("T:YamlTraps", 'yes: [no, {a: b}] # not a comment \\ "quoted" \'single\'  \u0085﻿  - ? : | > % @ `')
    for n, value in enumerate(("1.5", "null", "~", "true", "0x10", "2024-03-05", "!tag", "&anchor", "*alias", "...", "---",
                               "[]", "{}", "- item", "key: value", "# comment", "'", '"')):
        text(f"T:Looks{n}", value)
    text("T:CommentOnly", "x", extra=' comment="  blanks at both ends  "')
    text("A:" + ":".join(["VeryLongIdentifier"] * 7), "an id of 135 characters")
    text("A:" + "x" * 1100, "an id beyond the 1024 characters that a key of a YAML mapping may have when it is written the short way")
    text("A:EndsInABlank ", "an id with a blank at its end")
    scalar("X:Blanks", "Frobnication", "  \u00fcn\u00ef \u2604  ")   # blanks at both ends of a value that is no String
    # --- vectors and matrices
    for name in ELEMENT_BYTES:
        block(f"V:{name or 'Short'}", name + "Vector", numbers(name, 5).tobytes(), ' length="5"')
        block(f"M:{name or 'Short'}", name + "Matrix", numbers(name, 6).tobytes(), ' rows="2" columns="3"')
    block("V:Bytes", "ByteArray", bytes(range(7)), ' length="7"')
    block("V:Empty", "F64Vector", b"", ' length="0"')
    block("M:Empty", "F32Matrix", b"", ' rows="0" columns="4"')
    # more than xisfconv writes into the header of an XISF file: these are attached there
    block("V:Large", "F64Vector", numbers("F64", 700).tobytes(), ' length="700"', codec="zlib", shuffle_item=8, checksum="sha1")
    block("M:Large", "F32Matrix", numbers("F32", 1200).tobytes(), ' rows="60" columns="20"', encoding="hex")
    block("M:LargeComplex", "C32Matrix", numbers("C32", 500).tobytes(), ' rows="50" columns="10"')
    block("V:BigEndian", "UI16Vector", np.arange(1, 6).astype(">u2").tobytes(), ' length="5" byteOrder="big"')
    block("V:BigEndianComplex", "C64Vector", numbers("C64", 3).astype(">c16").tobytes(), ' length="3" byteOrder="big"')
    xml.append('<Property id="V:Embedded" type="I32Vector" length="3" location="embedded"><Data encoding="base64">' +
               base64.b64encode(np.array([1, -2, 3], "<i4").tobytes()).decode() + '</Data></Property>')
    # data blocks of types nobody knows
    block("X:Blob", "Opaque", b"\x00\x11\x22\x33\x44", ' length="2"')
    block("X:BlobNoLength", "Opaque", b"\x00\x11\x22")
    block("X:Pairs", "QuaternionMatrix", bytes(range(16)), ' rows="1" columns="2"')
    return "".join(xml), through_asdf


def test_property_round_trip():
    """XISF -> FITS or ASDF -> XISF: every property comes back with its type, value, comment and format.
    (astropy reads the FITS files here with memmap=False: a table that stays mapped into memory keeps
    Windows from replacing the file, and the files are written again and again.)"""
    import warnings
    d = os.path.join(TMP, "properties")
    os.makedirs(d, exist_ok=True)
    zoo, through_asdf = property_zoo()
    few = ('<Property id="Instrument:Filter:Name" type="String">Ha</Property>' +
           f64_prop("Lab:Dark", np.linspace(0, 1, 12), 3, 4))
    metadata = ('<Property id="XISF:CreationTime" type="TimePoint" value="2026-01-02T03:04:05Z"/>'
                '<Property id="XISF:CompressionCodecs" type="String">zlib+sh</Property>'
                '<Property id="Observatory:Name" type="String">Dark Sky &amp; Co</Property>' +
                f64_prop("Observatory:Location", [4.35, 50.85, 120.0]))
    images = [test_image(np.uint16, 6, 8, 1, 71), test_image(np.float32, 5, 7, 3, 72), test_image(np.uint8, 4, 4, 1, 73)]
    src = os.path.join(d, "zoo.xisf")
    write_xisf(src, [with_id(image_entry(images[0], children=zoo), "first"), with_id(image_entry(images[1]), "second"),
                     with_id(image_entry(images[2], children=few), "third")], file_props=metadata)
    original, original_file = xisf_properties(src)
    check(len(original[0]) > 120 and not original[1] and len(original[2]) == 2 and len(original_file) == 5,
          f"the test file has its properties: {[len(i) for i in original]}, {len(original_file)}")
    storage = ("XISF:CreationTime", "XISF:CreatorApplication", "XISF:CreatorModule", "XISF:CreatorOS", "XISF:BlockAlignmentSize",
               "XISF:MaxInlineBlockSize", "XISF:CompressionCodecs", "XISF:CompressionLevel")

    def expected(properties, asdf_leg):
        out = []
        for p in properties:
            q = dict(p)
            if asdf_leg and p["id"] in through_asdf:
                q["value"] = ("text", through_asdf[p["id"]].encode())
            if p["id"] == "T:LoneCr":
                # a carriage return that a value attribute holds (as a character reference) is one for every
                # reader; as text in a header it would be a line feed to an XML reader: such a text goes into a
                # block. (A text of the header with blanks at its ends, T:EdgeBlanks, is written as it was.)
                q["value"] = ("text in a block", p["value"][1])
            out.append(q)
        return out

    def same(label, path, asdf_leg, image_numbers=(0, 1, 2)):
        got, got_file = xisf_properties(path)
        ok = len(got) == len(image_numbers)
        for n, source in zip(range(len(got)), image_numbers):
            want = expected(original[source], asdf_leg)
            if got[n] != want:
                ok = False
                ids = [p["id"] for p in want]
                have = {p["id"]: p for p in got[n]}
                wrong = [(p["id"], have.get(p["id"], {}).get("value", "absent")) for p in want if have.get(p["id"]) != p][:3]
                print(f"   {label}, image {source}: {len(got[n])} of {len(want)} properties, order "
                      f"{'kept' if [p['id'] for p in got[n]] == ids else 'changed'}; first differences: {repr(wrong)[:600]}")
        check(ok, f"{label}: the properties of the images are those of the XISF file, in their order")
        want_file = [p for p in expected(original_file, asdf_leg) if p["id"] not in storage]
        check([p for p in got_file if p["id"] not in storage] == want_file, f"{label}: and so are those of the file")
        check([p["id"] for p in got_file if p["id"] in storage][:2] == ["XISF:CreationTime", "XISF:CreatorApplication"] and
              got_file[0]["value"][1] != b"2026-01-02T03:04:05Z",
              f"{label}: but for what describes the XISF file itself, which is the new file's own")
        # (the pixels straight from their blocks: the xisf package does not read every value of the properties)
        planes = [b["data"] for b in xisf_blocks(path) if b["tag"] == "Image"]
        check(len(planes) == len(image_numbers) and all(
            plane == as_planes(images[source]).astype(images[source].dtype.newbyteorder("<")).tobytes()
            for plane, source in zip(planes, image_numbers)), f"{label}: and the pixels")

    # ---- the routes there and back
    X = os.path.join(d, "back.xisf")
    routes = {
        "FITS": (["a.fits"], [], False),
        "FITS, rows top-down": (["a.fits"], ["--top-down"], False),
        "tile-compressed FITS": (["a.fits.fz"], [], False),
        "ASDF": (["a.asdf"], [], True),
        "compressed ASDF": (["a.asdf"], ["-c"], True),
        "FITS -> ASDF": (["a.fits", "b.asdf"], [], True),
        "ASDF -> FITS": (["a.asdf", "b.fits"], [], True),
        "FITS -> tile-compressed FITS -> ASDF -> FITS": (["a.fits", "b.fits.fz", "c.asdf", "d.fits"], [], True),
        "tile-compressed FITS -> FITS": (["a.fits.fz", "b.fits"], [], False),
    }
    for label, (files, flags, asdf_leg) in routes.items():
        previous = src
        for n, name in enumerate(files):
            out = os.path.join(d, name)
            r = run(previous, "-o", out, "-f", *(flags if n == 0 else []))
            check("warning" not in r.stderr, f"XISF -> {label}: no warning at {name}: {r.stderr.strip()[-300:]}")
            if n == 0:
                check("XISF properties taken along" in r.stderr, f"XISF -> {label}: the note says that properties are taken along")
            previous = out
        r = run(previous, "-o", X, "-f")
        check(r.stderr.count("XISF properties restored") == 2 and "warning" not in r.stderr,
              f"{label} -> XISF: the note says that they are restored: {r.stderr.strip()[-300:]}")
        same(f"XISF -> {label} -> XISF", X, asdf_leg)
        if label in ("FITS", "ASDF"):
            # the file xisfconv wrote has its large properties attached, compressed and with checksums:
            # the way back from there, and that file read by a program that is not xisfconv
            packed, again = os.path.join(d, "packed.xisf"), os.path.join(d, "again.xisf")
            run(previous, "-o", packed, "-f", "-q", "-c", "--checksum", "sha1")
            same(f"XISF -> {label} -> compressed XISF", packed, asdf_leg)
            attached = [b for b in xisf_blocks(packed) if b["tag"] == "Property" and b["kind"] == "attachment"]
            check({b["id"] for b in attached} >= {"V:Large", "M:Large", "M:LargeComplex", "T:BlockCrLf"} and
                  all(b["attr"]["checksum"] for b in attached) and any(b["attr"]["compression"] for b in attached),
                  f"{label}: large values are attached to the XISF file, with checksums and compressed where it pays")
            comp = {b["id"]: b["attr"]["compression"] for b in attached}
            check(":8" in (comp.get("V:Large") or "") and ":4" in (comp.get("M:LargeComplex") or ""),
                  f"numbers are shuffled by their size, a complex number by that of its parts: {comp.get('V:Large')}, {comp.get('M:LargeComplex')}")
            run(packed, "-o", os.path.join(d, "second" + os.path.splitext(files[0])[1]), "-f", "-q")
            run(os.path.join(d, "second" + os.path.splitext(files[0])[1]), "-o", again, "-f", "-q")
            same(f"a second time through {label}", again, asdf_leg)
            check(run(packed, "--verify").returncode == 0, f"{label}: --verify accepts the XISF file with its attached properties")

    # one image of several takes its own properties along
    for kind in ("fits", "asdf"):
        one = os.path.join(d, "third." + kind)
        run(src, "-o", one, "-f", "-q", "--image", "2")
        run(one, "-o", X, "-f", "-q")
        same(f"--image 2 through {kind.upper()}", X, kind == "asdf", image_numbers=(2,))
        run(os.path.join(d, "a." + kind), "-o", X, "-f", "-q", "--image", "0")
        same(f"--image 0 of the {kind.upper()} file", X, kind == "asdf", image_numbers=(0,))

    # ---- what other programs see
    f = os.path.join(d, "a.fits")
    run(src, "-o", f, "-f", "-q")
    fits_planes(f)   # fitsverify, and astropy's own verification
    with fits.open(f, memmap=False) as hdul:
        names = [(h.name, h.ver) for h in hdul]
        check(names == [("first", 1), ("XISF_PROPERTIES", 1), ("second", 1), ("third", 1), ("XISF_PROPERTIES", 3), ("XISF_METADATA", 1)],
              f"FITS: a table behind each image that has properties, and one for the file: {names}")
        table = hdul[1].data
        check(table.columns.names == ["ID", "TYPE", "BLOCK", "ROWS", "COLUMNS", "VALUE", "COMMENT", "FORMAT"],
              f"the columns of the table: {table.columns.names}")
        rows = []
        for row in table:
            value = bytes(np.asarray(row["VALUE"], np.uint8))
            name = element_of(row["TYPE"])
            if row["BLOCK"] and row["TYPE"] == "String":
                value = ("text in a block", value)
            elif row["BLOCK"]:
                shape = ((None, int(row["ROWS"]), int(row["COLUMNS"])) if row["TYPE"].endswith("Matrix")
                         else (int(row["ROWS"]) if name or row["ROWS"] else None, None, None))
                value = ("data", shape, value)
            else:
                value = ("text", value)
            # (astropy takes blanks at the end of a text column for padding: one id ends in one)
            rows.append({"id": row["ID"] + (" " if row["ID"] == "A:EndsInABlank" else ""), "type": row["TYPE"], "comment": bytes(np.asarray(row["COMMENT"], np.uint8)).decode(),
                         "format": bytes(np.asarray(row["FORMAT"], np.uint8)).decode(), "value": value})
        # (a text with a carriage return that is meant is carried as data, byte for byte: in a header it
        # would be written as it is, and an XML reader would then make a line feed of it)
        carried = [dict(p, value=("text in a block", p["value"][1])) if p["id"] == "T:LoneCr" else p for p in original[0]]
        if rows != carried:
            print("   astropy:", repr([(r["id"], r["value"]) for r, o in zip(rows, carried) if r != o][:3])[:500])
        check(rows == carried, "astropy reads the table: every id, type, comment, format and value")
        m = {r["id"]: r for r in rows}["M:F64"]["value"]
        check(np.array_equal(np.frombuffer(m[2], "<f8").reshape(m[1][1:]), np.frombuffer(
            [p for p in original[0] if p["id"] == "M:F64"][0]["value"][2], "<f8").reshape(2, 3), equal_nan=True),
            "a matrix is its numbers in little-endian order, row after row")
        check(len(hdul[1].header["WCSDIGST"]) == 40 and "WCSDIGST" not in hdul[5].header, "the table of an image names the WCS it was written with")
        check([r["ID"] for r in hdul[5].data] == ["Observatory:Name", "Observatory:Location"],
              "the table of the file leaves out what describes the XISF file itself")
    def info(path):   # (one of the properties is text that is not UTF-8)
        return subprocess.run([EXE, path, "--info"], capture_output=True, check=True).stdout.decode(errors="replace")

    listing = info(f)
    check("XISF properties (%d):" % len(original[0]) in listing and "XISF file metadata (2):" in listing and
          "Observatory:Name (String) = Dark Sky & Co" in listing and "M:F64 (F64Matrix) [data block]" in listing and
          "table" not in listing, "--info lists the properties a FITS file carries (and not their tables as skipped HDUs)")
    check(run(f, "--verify").returncode == 0, "--verify accepts the FITS file with its tables")
    a = os.path.join(d, "a.asdf")
    run(src, "-o", a, "-f", "-q")
    listing = info(a)
    check("XISF properties (%d):" % len(original[0]) in listing and "XISF file metadata (2):" in listing and
          listing.count("\nImage ") == 3, "--info lists the properties an ASDF file carries, and its three images only")
    check(run(a, "--verify").returncode == 0, "--verify accepts the ASDF file with the blocks of its properties")
    if HAVE_FPACK:
        # CFITSIO's programs keep the tables
        packed, plain = os.path.join(d, "cfitsio.fits.fz"), os.path.join(d, "cfitsio.fits")
        subprocess.run(["fpack", "-O", packed, f], check=True, capture_output=True)
        run(packed, "-o", X, "-f", "-q")
        same("XISF -> FITS -> fpack -> XISF", X, False)
        run(src, "-o", os.path.join(d, "ours.fits.fz"), "-f", "-q")
        subprocess.run(["funpack", "-O", plain, os.path.join(d, "ours.fits.fz")], check=True, capture_output=True)
        run(plain, "-o", X, "-f", "-q")
        same("XISF -> tile-compressed FITS -> funpack -> XISF", X, False)
    else:
        skipped.append("properties through fpack and funpack")
    if HAVE_ASDF:
        import asdf as asdf_library
        with warnings.catch_warnings():
            warnings.simplefilter("error", asdf_library.exceptions.AsdfWarning)
            with asdf_library.open(a, validate_checksums=True, memmap=False) as af:
                af.validate()
                tree = af["xisf"]
                p = tree["images"][0]["properties"]
                check(list(tree) == ["images", "metadata"] and len(tree["images"]) == 3 and tree["images"][1] == {} and
                      list(p) == [q["id"] for q in original[0]] and list(tree["metadata"]) == ["Observatory:Name", "Observatory:Location"],
                      "asdf reads the tree: the properties of the images and of the file, by their ids and in their order")
                kinds = {"S:True": True, "S:One": True, "S:Zero": False, "S:Int8:-128": -128, "S:UInt32:4294967295": 4294967295,
                         "S:Int64:9223372036854775807": 9223372036854775807, "S:Int64:-9223372036854775808": "-9223372036854775808",
                         "S:UInt64:18446744073709551615": "18446744073709551615", "S:Plus": 7, "S:Hexadecimal": "0x1F",
                         "S:Float64:0": 0.1, "S:Float64:2": 2000, "S:Float64:7": 1e-05, "S:Float64:8": 6.02e23, "S:Float64:9": 0.5,
                         "S:Float64:16": -np.inf, "S:Float64:17": "not a number", "S:Complex32": 1.5 - 2j, "S:Complex64": 0.25 + 0.001j,
                         "S:Time": "2024-03-05T21:15:02.5Z", "T:Utf8": "Ångström ☄ \U0001f30c مرحبا",
                         "T:Empty": "", "T:Lines": "line 1\nline 2\n\ttabbed", "T:Control": "bell\x07 escape\x1b unit\x1f delete\x7f",
                         "T:Nul": "a\x00b", "T:LoneCr": "classic\rline ends", "T:Looks1": "null",
                         "T:Looks3": "true", "T:Looks0": "1.5", "X:Thing": "7 of 9",
                         "T:YamlTraps": 'yes: [no, {a: b}] # not a comment \\ "quoted" \'single\'  \u0085﻿  - ? : | > % @ `'}
                wrong = [(k, p[k]["value"]) for k, v in kinds.items() if type(p[k]["value"]) is not type(v) or p[k]["value"] != v]
                check(not wrong, f"scalars are YAML scalars of their kind: booleans, numbers, text: {wrong[:4]}")
                check(np.isnan(p["S:Float64:13"]["value"]) and np.isnan(p["S:Float64:14"]["value"]) and p["S:Float64:15"]["value"] == np.inf,
                      "values that are not finite numbers are YAML's .nan and .inf")
                check(p["S:Commented"] == {"type": "Float32", "value": 0.25, "comment": "exposure in μs, \"quoted\" & more", "format": "%.3f"} and
                      p["T:CommentOnly"]["comment"] == "  blanks at both ends  " and set(p["T:Plain"]) == {"type", "value"},
                      "an entry has the type and the value, and the comment and the format if there are any")
                check(p["T:Padded"] == {"type": "String", "value": "  padded \n", "block": True} and
                      p["T:BlockCrLf"]["value"] == "5000 3000\r\n0000 BA00\r\n" * 300 + "\0\0" and p["T:BlockCrLf"]["block"] is True,
                      "a String that XISF keeps in a data block is text too, and says where it was")
                check(p["A:" + "x" * 1100]["type"] == "String" and p["A:EndsInABlank "]["value"] == "an id with a blank at its end" and
                      p["X:Blanks"]["value"] == "  \u00fcn\u00ef \u2604  ", "ids that are long or end in a blank are keys as they are")
                arrays_ok = True
                for q in original[0]:
                    name = element_of(q["type"])
                    if name is None:
                        continue
                    v = np.asarray(p[q["id"]]["value"])
                    shape = q["value"][1][1:] if q["type"].endswith("Matrix") else (q["value"][1][0],)
                    if v.dtype != np.dtype(ELEMENT_DTYPE[name]) or v.shape != shape or v.astype(ELEMENT_DTYPE[name]).tobytes() != q["value"][2]:
                        arrays_ok = False
                        print("   asdf:", q["id"], v.dtype, v.shape, shape)
                check(arrays_ok, "vectors and matrices are arrays of their element type and shape, with every number as it was")
                check(bytes(np.asarray(p["T:NotUtf8"]["value"])) == b"caf\xe9 \xff\xfe\xc0\x80" and p["T:NotUtf8"]["block"] is True and
                      bytes(np.asarray(p["X:Blob"]["value"])) == b"\x00\x11\x22\x33\x44" and p["X:Blob"]["length"] == 2 and
                      "length" not in p["X:BlobNoLength"] and (p["X:Pairs"]["rows"], p["X:Pairs"]["columns"]) == (1, 2),
                      "text that is not UTF-8 and blocks of unknown types are arrays of bytes")
                check(len(tree["images"][0]["wcs_digest"]) == 40, "an image names the WCS its properties were written with")
                # changed with the asdf library and written again, in its own way
                p["T:Plain"]["value"] = "changed in Python"
                p["Python:New"] = {"type": "F32Vector", "value": np.array([1.5, 2.5], ">f4"), "comment": "added in Python"}
                p["Python:Scalar"] = {"type": "Float64", "value": 0.125}
                del p["T:Utf8"]
                edited = os.path.join(d, "edited.asdf")
                af.write_to(edited, all_array_compression="zlib")
        r = run(edited, "-o", X, "-f")
        got, _ = xisf_properties(X)
        want = [q for q in expected(original[0], True) if q["id"] != "T:Utf8"]
        for q in want:
            if q["id"] == "T:Plain":
                q["value"] = ("text", b"changed in Python")
        want += [{"id": "Python:New", "type": "F32Vector", "comment": "added in Python", "format": "",
                  "value": ("data", (2, None, None), np.array([1.5, 2.5], "<f4").tobytes())},
                 {"id": "Python:Scalar", "type": "Float64", "comment": "", "format": "", "value": ("text", b"0.125")}]
        def by_value(properties):   # Python writes a number in its own way: 1.5E+300 as 1.5e+300
            for q in properties:
                if q["type"].startswith("Float") and q["value"][0] == "text":
                    try:
                        q["value"] = ("text", repr(float(q["value"][1])).encode())
                    except ValueError:
                        pass
            return properties

        # (and it writes the entries of a mapping sorted by their keys: the properties come in the
        # order of their ids, which is the order PixInsight writes them in too)
        got[0], want = by_value(got[0]), sorted(by_value(want), key=lambda q: q["id"])
        if got[0] != want:
            have = {q["id"]: q for q in got[0]}
            print("   edited:", len(got[0]), len(want), repr([(q["id"], have.get(q["id"])) for q in want if have.get(q["id"]) != q][:3])[:700])
        check(got[0] == want and "warning" not in r.stderr,
              f"a file the asdf library changed and wrote again gives its properties to XISF: {r.stderr.strip()[-200:]}")
    else:
        skipped.append("XISF properties in the tree read by the asdf library")

    # ---- the text of a String with CR LF line ends, as PixInsight writes it into the header
    crlf = os.path.join(d, "crlf.xisf")
    write_xisf(crlf, [image_entry(images[0], children='<Property id="P:Serialization" type="String">5000 3000\r\n0000 BA00\r\n10</Property>')])
    for kind in ("fits", "asdf"):
        run(crlf, "-o", os.path.join(d, "crlf." + kind), "-f", "-q")
        run(os.path.join(d, "crlf." + kind), "-o", X, "-f", "-q")
        check(b'type="String">5000 3000\r\n0000 BA00\r\n10</Property>' in open(X, "rb").read(),
              f"through {kind.upper()}: a String with CR LF line ends is the same bytes in the header again")

    # ---- two properties of one id (which XISF does not allow)
    twice = os.path.join(d, "twice.xisf")
    write_xisf(twice, [image_entry(images[0], children='<Property id="Twice" type="Int32" value="1"/>'
                                                         '<Property id="Other" type="Int32" value="2"/>'
                                                         '<Property id="Twice" type="Int32" value="3"/>')])
    run(twice, "-o", os.path.join(d, "twice.fits"), "-f", "-q")
    run(os.path.join(d, "twice.fits"), "-o", X, "-f", "-q")
    check([(p["id"], p["value"][1]) for p in xisf_properties(X)[0][0]] == [("Twice", b"1"), ("Other", b"2"), ("Twice", b"3")],
          "FITS carries two properties of the same id as they are")
    r = run(twice, "-o", os.path.join(d, "twice.asdf"), "-f")
    run(os.path.join(d, "twice.asdf"), "-o", X, "-f", "-q")
    check("more than once" in r.stderr and [(p["id"], p["value"][1]) for p in xisf_properties(X)[0][0]] == [("Twice", b"1"), ("Other", b"2")],
          "an ASDF tree, where the id is the key, keeps the first, and a warning says so")

    # ---- --no-properties
    run(src, "-o", f, "-f", "-q", "--no-properties")
    with fits.open(f, memmap=False) as hdul:
        check([h.name for h in hdul] == ["first", "second", "third"], "--no-properties: XISF -> FITS writes the images alone")
    run(src, "-o", a, "-f", "-q", "--no-properties")
    check(b"\nxisf:" not in open(a, "rb").read(), "--no-properties: XISF -> ASDF writes no xisf key")
    run(src, "-o", f, "-f", "-q")
    r = run(f, "-o", X, "-f", "--no-properties")
    check(xisf_properties(X) == ([[], [], []], [p for p in xisf_properties(X)[1] if p["id"] in storage]) and "restored" not in r.stderr,
          "--no-properties: FITS -> XISF leaves the properties the file carries where they are")
    run(f, "-o", a, "-f", "-q", "--no-properties")
    check(b"\nxisf:" not in open(a, "rb").read(), "--no-properties: FITS -> ASDF does not take them along")
    run(src, "-o", a, "-f", "-q")
    run(a, "-o", f, "-f", "-q", "--no-properties")
    with fits.open(f, memmap=False) as hdul:
        check(len(hdul) == 3, "--no-properties: ASDF -> FITS does not take them along")
    # a file of another program: nothing changes for it
    plain = os.path.join(d, "plain.fits")
    fits.PrimaryHDU(images[0][..., 0]).writeto(plain, overwrite=True)
    r = run(plain, "-o", X, "-f")
    check(xisf_properties(X)[0] == [[]] and "restored" not in r.stderr, "a FITS file without properties gets none")

    # ---- tables that are damaged, and tables that are not ours
    run(src, "-o", f, "-f", "-q")
    raw = bytearray(open(f, "rb").read())

    def damaged(label, change, message, kept_images=3, expect_first=None):
        bad = os.path.join(d, "damaged.fits")
        data = bytearray(raw)
        change(data)
        open(bad, "wb").write(data)
        r = run(bad, "-o", X, "-f", expect_ok=False)
        got = xisf_properties(X)[0] if r.returncode == 0 else None
        check(r.returncode == 0 and message in r.stderr and len(got) == kept_images and
              (expect_first is None or [p["id"] for p in got[0]] == expect_first),
              f"{label}: {r.stderr.strip()[-260:]}")

    def replace(old, new):
        def change(data):
            at = data.find(old)
            assert at > 0 and len(old) == len(new), old
            data[at:at + len(old)] = new
        return change

    damaged("a table whose columns are others", replace(b"TTYPE6  = 'VALUE   '", b"TTYPE6  = 'WERT    '"),
            "the table of XISF properties is not used", expect_first=[])
    damaged("a table with a column of another type", replace(b"TFORM4  = '1K      '", b"TFORM4  = '1D      '"),
            "the table of XISF properties is not used", expect_first=[])
    at = raw.find(b"XTENSION= 'BINTABLE'")
    rows_at = at + 2880 * (-(-(raw.find(b"END     ", at) + 80 - at) // 2880))   # the first row of the first table
    width = int(fits.getheader(f, 1)["NAXIS1"])
    ids = [p["id"] for p in original[0]]
    id_width = len(max(ids, key=len))
    type_width = max(len(p["type"]) for p in original[0])

    def spoil_descriptor(data):   # the value of the second property is said to lie far beyond the heap
        row = rows_at + width
        data[row + id_width + type_width + 17 + 4:row + id_width + type_width + 17 + 8] = (0x7FFFFFF0).to_bytes(4, "big")
    damaged("a value that lies beyond the table", spoil_descriptor, "lies beyond the end of the table", expect_first=ids[:1] + ids[2:])

    def spoil_shape(data):   # a vector that says it is longer than its data
        n = ids.index("V:F64")
        row = rows_at + n * width
        data[row + id_width + type_width + 1:row + id_width + type_width + 9] = (6).to_bytes(8, "big")
    damaged("a vector whose data is not what its length asks for", spoil_shape, "V:F64 is left out",
            expect_first=[i for i in ids if i != "V:F64"])

    open(os.path.join(d, "cut.fits"), "wb").write(raw[:rows_at + 5760])
    r = run(os.path.join(d, "cut.fits"), "-o", X, "-f", expect_ok=False)
    check(r.returncode != 0 and "truncated" in r.stderr, f"a file that ends in the middle of a table is a truncated file: {r.stderr.strip()[-160:]}")
    with fits.open(f, memmap=False) as hdul:
        foreign = fits.BinTableHDU.from_columns([fits.Column(name="ID", format="8A", array=np.array(["x"])),
                                                 fits.Column(name="FLUX", format="D", array=np.array([1.5]))], name="CATALOG")
        fits.HDUList([hdul[0], foreign, hdul[1]]).writeto(os.path.join(d, "foreign.fits"), overwrite=True)
    r = run(os.path.join(d, "foreign.fits"), "-o", X, "-f")
    check("skipped" in r.stderr and "CATALOG" not in r.stdout and xisf_properties(X)[0] == [[]],
          f"a table of properties that does not follow an image belongs to none, and other tables are skipped as before: {r.stderr.strip()[-200:]}")
    if HAVE_ASDF:
        # trees that say other things than xisfconv writes
        def tree_of(entries):
            return ("fits: !<tag:astropy.org:astropy/fits/fits-1.0.0>\n- header:\n  - [EXTNAME, x]\n  data: !core/ndarray-1.0.0\n"
                    "    {source: 0, datatype: uint16, byteorder: little, shape: [6, 8]}\n"
                    "xisf:\n  images:\n  - properties:\n" + "".join("      " + e + "\n" for e in entries))
        pixels = asdf_block(images[0][..., 0].astype("<u2").tobytes())
        vector = asdf_block(np.array([1.0, 2.0, 3.0], ">f8").tobytes(), compression=b"zlib")
        cases = [
            ('"Good": {type: F64Vector, value: !core/ndarray-1.0.0 {source: 1, datatype: float64, byteorder: big, shape: [3]}}', None),
            ('Plain: {type: String, value: unquoted text}', None),
            ('"Int": {type: Int32, value: 0x10}', None),
            ('"Wrong:Datatype": {type: F64Vector, value: !core/ndarray-1.0.0 {source: 1, datatype: int64, byteorder: big, shape: [3]}}', "datatype"),
            ('"Wrong:Shape": {type: F64Matrix, value: !core/ndarray-1.0.0 {source: 1, datatype: float64, byteorder: big, shape: [3]}}', "dimension"),
            ('"Wrong:Block": {type: F64Vector, value: !core/ndarray-1.0.0 {source: 7, datatype: float64, byteorder: big, shape: [3]}}', "block 7"),
            ('"Wrong:Size": {type: F64Vector, value: !core/ndarray-1.0.0 {source: 1, datatype: float64, byteorder: big, shape: [4]}}', "needs 32 bytes"),
            ('"Wrong:Inline": {type: F64Vector, value: !core/ndarray-1.0.0 {data: [1.0, 2.0], datatype: float64, shape: [2]}}', "binary block"),
            ('"Wrong:List": {type: String, value: [a, b]}', "list"),
            ('"Wrong:Scalar": {type: F64Vector, value: 1.5}', "array"),
            ('"Wrong:Array": {type: Int32, value: !core/ndarray-1.0.0 {source: 1, datatype: uint8, shape: [24]}}', "not the value"),
            ('"Wrong:NoType": {value: 1.5}', "no type"),
            ('"Wrong:NotAMapping": just text', "mapping"),
        ]
        odd = os.path.join(d, "odd.asdf")
        write_asdf_raw(odd, tree_of([c[0] for c in cases]), [pixels, vector])
        r = run(odd, "-o", X, "-f")
        got = xisf_properties(X)[0][0]
        check([(p["id"], p["type"], p["value"]) for p in got] ==
              [("Good", "F64Vector", ("data", (3, None, None), np.array([1.0, 2.0, 3.0], "<f8").tobytes())),
               ("Plain", "String", ("text", b"unquoted text")), ("Int", "Int32", ("text", b"16"))],
              f"a tree written by hand: what is a property is one, in big-endian order and compressed too: {[p['id'] for p in got]}")
        missing = [c[0].split(":")[1].split('"')[0] for c in cases if c[1] and not any(
            c[0].split('"')[1] in line and c[1] in line for line in r.stderr.splitlines())]
        check(not missing, f"and each entry that is none is left out with a warning that says why: {missing} {r.stderr[-400:] if missing else ''}")
        write_asdf_raw(odd, tree_of([]).replace("  - properties:\n", "  - 17\n") + "  metadata: [1, 2]\n", [pixels])
        r = run(odd, "-o", X, "-f")
        check(xisf_properties(X)[0] == [[]], "an xisf key that holds something else is passed over")


    # ---- what cannot go everywhere, and files that ask for more than they hold
    def well_formed(path):
        import xml.etree.ElementTree as ET
        try:
            ET.fromstring(xisf_header_bytes(path))
            return True
        except ET.ParseError as e:
            return str(e)

    def xisf_header_bytes(path):
        raw = open(path, "rb").read()
        return raw[16:16 + int.from_bytes(raw[8:12], "little")]

    awkward = os.path.join(d, "awkward.xisf")
    write_xisf(awkward, [image_entry(images[0], children=(
        '<Property id="Good" type="Int32" value="1"/>'
        '<Property id="" type="Int32" value="2"/>'
        '<Property id="Structure" type="Table"><Row><Cell value="1"/></Row></Property>'
        '<Property id="Odd:Type" type="TYPEBYTES" value="3"/>'
        '<Property id="Odd:Comment" type="Int32" value="4" comment="COMMENTBYTES"/>'
        '<Property id="Odd:Control" type="Int32" value="5" comment="bell&#7;here" format="&#27;[1m"/>'
        '<Property id="Odd:Value" type="Frobnication" value="escape&#27;d"/>'
        '<Property id="Last" type="String">kept</Property>'))])
    raw = open(awkward, "rb").read()
    raw = raw.replace(b"TYPEBYTES", b"T\xff\xfe\xe9YPE \xc3").replace(b"COMMENTBYTES", b"caf\xe9 \xff\xfe comm")   # bytes that are no UTF-8
    open(awkward, "wb").write(raw)
    for kind in ("fits", "asdf"):
        mid = os.path.join(d, "awkward." + kind)
        r1 = subprocess.run([EXE, awkward, "-o", mid, "-f"], capture_output=True)
        e1 = r1.stderr.decode(errors="replace")
        check(r1.returncode == 0 and "a property without an id is left out" in e1 and "Structure is left out: it is made of <Row>" in e1 and
              ("whose id or " in e1 or "its id or its type" in e1), f"XISF -> {kind}: what is no property that can be carried is named: {e1[-300:]}")
        r2 = subprocess.run([EXE, mid, "-o", X, "-f"], capture_output=True)
        e2 = r2.stderr.decode(errors="replace")
        ok = well_formed(X)
        got = xisf_properties(X)[0][0] if ok is True else []
        check(ok is True and [p["id"] for p in got] == ["Good", "Odd:Comment", "Odd:Control", "Odd:Value", "Last"],
              f"{kind} -> XISF: the header is XML whatever the properties held: {ok} {[p['id'] for p in got]}")
        by = {p["id"]: p for p in got}
        check(ok is True and by["Odd:Control"]["comment"] == "bell here" and by["Odd:Control"]["format"] == " [1m" and
              by["Odd:Value"]["value"] == ("text", b"escape d") and by["Odd:Comment"]["comment"] == "caf? ?? comm" and
              (e1 + e2).count("Odd:Control: its comment") == 1 and (e1 + e2).count("Odd:Value: its value") == 1,
              f"{kind} -> XISF: a character XML cannot hold is replaced, and a warning says so once: {e2[-300:]}")
        if kind == "asdf" and HAVE_ASDF:
            with asdf.open(mid, memmap=False) as af:
                p = af["xisf"]["images"][0]["properties"]
                check(list(p) == ["Good", "Odd:Comment", "Odd:Control", "Odd:Value", "Last"] and p["Odd:Control"]["comment"] == "bell\x07here" and
                      p["Odd:Value"]["value"] == "escape\x1bd", "the asdf library reads such a tree, control characters included")

    # a tree of another program that has a key "xisf" for its own things
    theirs = os.path.join(d, "theirs.asdf")
    write_asdf_raw(theirs, "xisf:\n  image: !core/ndarray-1.0.0 {source: 0, datatype: uint16, byteorder: little, shape: [6, 8]}\n  note: mine\n",
                   [asdf_block(images[0][..., 0].astype("<u2").tobytes())])
    r = run(theirs, "-o", X, "-f")
    check(np.array_equal(read_xisf_any(X), images[0][::-1]), "an ASDF file with a key xisf that holds an image of its own is read as before")

    # many properties that share one block: read once, and no more of them than a file of that size can hold
    import time
    zeros = asdf_block(bytes(4 << 20), compression=b"zlib")
    tree = ("fits: !<tag:astropy.org:astropy/fits/fits-1.0.0>\n- header:\n  - [EXTNAME, x]\n  data: !core/ndarray-1.0.0\n"
            "    {source: 0, datatype: uint16, byteorder: little, shape: [6, 8]}\nxisf:\n  images:\n  - properties:\n" +
            "".join(f'      "P{n}": {{type: UI8Vector, value: !core/ndarray-1.0.0 {{source: 1, datatype: uint8, byteorder: little, shape: [4194304]}}}}\n'
                    for n in range(300)))
    greedy = os.path.join(d, "greedy.asdf")
    write_asdf_raw(greedy, tree, [asdf_block(images[0][..., 0].astype("<u2").tobytes()), zeros])
    started = time.time()
    r = run(greedy, "--info")
    check(r.stdout.count("(UI8Vector)") == 64 and r.stderr.count("is left out") == 236 and "more data than a file of its size" in r.stderr and
          time.time() - started < 30,
          f"ASDF: 300 properties of 4 MiB that share one compressed block of a small file: 64 are read ({r.stdout.count('(UI8Vector)')}), "
          f"in {time.time() - started:.1f} s")
    blocks = [Block(as_planes(images[0]).astype("<u2").tobytes()), Block(bytes(1000), codec="zlib")]
    template = ('<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">'
                '<Image geometry="8:6:1" sampleFormat="UInt16" colorSpace="Gray" {0}>' +
                "".join(f'<Property id="P{n}" type="UI8Vector" length="1500000000" {{1}}/>' for n in range(40)) + '</Image></xisf>')
    greedy = os.path.join(d, "greedy.xisf")
    write_xisf_blocks(greedy, template, blocks)
    raw = open(greedy, "rb").read().replace(b'compression="zlib:1000"', b'compression="zlib:1500000000"')
    hlen = raw.index(b"</xisf>") + 7 - 16
    assert hlen == int.from_bytes(raw[8:12], "little") + 6 * 40, "(the header grew by six digits per property)"
    # (the positions of the attachments have to follow the longer header: written again with them)
    template = template.replace('length="1500000000" {1}', 'length="1500000000" {1} ')
    blocks[1].attrs["compression"] = "zlib:1500000000"
    write_xisf_blocks(greedy, template, blocks)
    started = time.time()
    r = run(greedy, "-o", os.path.join(d, "greedy.fits"), "-f")
    with fits.open(os.path.join(d, "greedy.fits"), memmap=False) as hdul:
        check(len(hdul) == 1 and r.stderr.count("is left out") == 40 and time.time() - started < 20,
              f"XISF: properties that declare 1.5 GB each in a file of a few kilobytes are left out without being read: {time.time() - started:.1f} s")

    # texts in data blocks are read when a file is opened: the same holds for them
    blocks = [Block(as_planes(images[0]).astype("<u2").tobytes()), Block(b"spline " * (8 << 17), codec="zlib")]
    template = ('<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">'
                '<Image geometry="8:6:1" sampleFormat="UInt16" colorSpace="Gray" {0}>' +
                "".join(f'<Property id="S{n}" type="String" {{1}}/>' for n in range(100)) + '</Image></xisf>')
    greedy = os.path.join(d, "greedy_text.xisf")
    write_xisf_blocks(greedy, template, blocks)
    started = time.time()
    r = run(greedy, "--info")
    check(r.stdout.count("(String) = spline spline") == 36 and r.stdout.count("(String) [data block]") == 64 and
          r.stderr.count("more data than a file of its size") == 64 and time.time() - started < 30,
          f"XISF: 100 texts of 7 MiB that share one compressed block of a small file: 36 are read "
          f"({r.stdout.count('(String) = spline spline')}), in {time.time() - started:.1f} s")

    # ---- the astrometric solution comes back as it was, as long as the WCS is what it was
    from astropy.wcs import WCS
    W, H = 300, 200
    sky = test_image(np.uint16, H, W, 1, 81)
    P = "PCL:AstrometricSolution:"
    grid = np.random.default_rng(5).normal(size=(40, 40))
    solution = "".join([
        f'<Property id="{P}ProjectionSystem" type="String">Gnomonic</Property>',
        f'<Property id="{P}CreatorApplication" type="String">PixInsight 1.9.3</Property>',
        f64_prop(P + "ReferenceCelestialCoordinates", [328.178, 47.358]),
        f64_prop(P + "ReferenceImageCoordinates", [150.3, 99.3]),
        f64_prop(P + "ReferenceNativeCoordinates", [0, 90]),
        f64_prop(P + "CelestialPoleNativeCoordinates", [180, 90]),
        f64_prop(P + "LinearTransformationMatrix", [-2.3565e-4, 1.1696e-5, -1.1715e-5, -2.3575e-4], 2, 2),
        f64_prop(P + "SplineWorldTransformation:PointGridInterpolation:ImageToNative:GridX", grid.ravel(), 40, 40),
        '<Property id="Observation:CelestialReferenceSystem" type="String">ICRS</Property>',
        '<Property id="Instrument:Filter:Name" type="String">OIII</Property>',
    ])
    solved = os.path.join(d, "solved.xisf")
    write_xisf(solved, [image_entry(sky, children=solution)])
    truth = xisf_properties(solved)[0][0]

    def restored(path):
        """exact: the properties are those of the file; rebuilt: the solution is xisfconv's, made from WCS keywords"""
        got = xisf_properties(path)[0][0]
        by_id = {p["id"]: p for p in got}
        if got == truth:
            return "exact"
        if (by_id.get(P + "CreatorApplication", {}).get("value", ("", b""))[1].startswith(b"xisfconv") and
                P + "SplineWorldTransformation:PointGridInterpolation:ImageToNative:GridX" not in by_id and
                by_id.get("Instrument:Filter:Name") == truth[-1]):
            return "rebuilt"
        if not any(i.startswith(P) for i in by_id) and by_id.get("Instrument:Filter:Name") == truth[-1]:
            return "none"
        return "other: " + repr(sorted(by_id))[:300]

    for kind in ("fits", "asdf", "fits.fz"):
        for out_flags in ([], ["--top-down"], ["--no-wcs"], ["--sip-order", "0"]):
            mid = os.path.join(d, "solved." + kind)
            run(solved, "-o", mid, "-f", "-q", *out_flags)
            r = run(mid, "-o", X, "-f")
            check(restored(X) == "exact" and "the astrometric solution among them" in r.stderr,
                  f"XISF -> {kind} {out_flags} -> XISF: the solution is the one PixInsight wrote, number for number: {restored(X)}")
            r = run(mid, "-o", X, "-f", "--no-wcs")
            check(restored(X) == "exact", f"XISF -> {kind} {out_flags} -> XISF --no-wcs: and with --no-wcs too: {restored(X)}")
            # the rows taken in the other order than the file says: the solution cannot be the same
            r = run(mid, "-o", X, "-f", "--bottom-up" if out_flags == ["--top-down"] else "--top-down")
            check(restored(X) == ("none" if out_flags == ["--no-wcs"] else "rebuilt") and "the rows are taken in the other order" in r.stderr,
                  f"XISF -> {kind} {out_flags} -> XISF with the rows the other way: {restored(X)}; {r.stderr.strip()[-200:]}")
    mid = os.path.join(d, "solved.fits")
    run(solved, "-o", mid, "-f", "-q")
    changed = os.path.join(d, "solved_changed.fits")

    def edit(label, change, want, message):
        with fits.open(mid, memmap=False) as hdul:
            change(hdul)
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                hdul.writeto(changed, overwrite=True)
        r = run(changed, "-o", X, "-f")
        check(restored(X) == want and message in r.stderr, f"{label}: the solution is {want}: {restored(X)}; {r.stderr.strip()[-200:]}")
        return r

    def reformat(hdul):   # the same numbers, written another way, and other keywords changed
        hdul[0].header["OBJECT"] = "renamed"
        hdul[0].header["CRVAL1"] = float(hdul[0].header["CRVAL1"])
        hdul[0].data = hdul[0].data + 1
        cards = [c for c in hdul[0].header.cards if c.keyword.startswith(("CD", "CRPIX"))]
        for c in cards:
            del hdul[0].header[c.keyword]
        for c in reversed(cards):
            hdul[0].header.append((c.keyword, c.value, "moved"), end=True)
    edit("the file written again by astropy with its WCS as it was", reformat, "exact", "the astrometric solution among them")

    def shift(hdul):
        hdul[0].header["CRVAL1"] += 0.25
    edit("CRVAL1 changed", shift, "rebuilt", "the WCS keywords, the size of the image or the order of its rows changed")
    check(abs(np.frombuffer({p["id"]: p for p in xisf_properties(X)[0][0]}[P + "ReferenceCelestialCoordinates"]["value"][2], "<f8")[0] - 328.428) < 1e-9,
          "and it is made from the keywords as they are now")
    for label, name, value in (("the equinox in its old spelling", "EPOCH", 1950.0), ("a matrix in its old spelling", "PC001002", 0.1),
                               ("a distortion table", "CPDIS1", "LOOKUP"), ("IRAF's distortion", "WAT1_001", "wtype=tnx")):
        def add(hdul, name=name, value=value):
            hdul[0].header[name] = value
        edit(label + " added (" + name + ")", add, "rebuilt", "the WCS keywords, the size of the image or the order of its rows changed")

    def crop(hdul):
        hdul[0].data = hdul[0].data[:150, :]
    edit("the image cropped", crop, "rebuilt", "the WCS keywords, the size of the image or the order of its rows changed")

    def unsolve(hdul):
        for k in [k for k in hdul[0].header if k.startswith(("CTYPE", "CRVAL", "CRPIX", "CD", "A_", "B_", "AP_", "BP_", "LONPOLE", "LATPOLE", "RADESYS", "EQUINOX"))]:
            del hdul[0].header[k]
    edit("the WCS keywords removed", unsolve, "none", "not the astrometric solution among them")

    def forget(hdul):
        del hdul[1].header["WCSDIGST"]
    edit("a table that does not say which WCS it was written with", forget, "rebuilt", "not the astrometric solution among them")
    # a solution and WCS keywords next to it, one of them an integer that is minus zero: the tree of an
    # ASDF file has no such integer, and the WCS is the same for it
    both = os.path.join(d, "both.xisf")
    write_xisf(both, [image_entry(sky, children="".join(
        f'<FITSKeyword name="{k}" value="{v}" comment=""/>' for k, v in (
            ("CTYPE1", "'RA---TAN'"), ("CTYPE2", "'DEC--TAN'"), ("CRVAL1", "328.178"), ("CRVAL2", "47.358"), ("CRPIX1", "150.8"),
            ("CRPIX2", "100.2"), ("CD1_1", "-2.3565E-4"), ("CD1_2", "-0"), ("CD2_1", "0"), ("CD2_2", "2.3575E-4"))) + solution)])
    truth_both = xisf_properties(both)[0][0]
    for kind in ("fits", "asdf"):
        mid = os.path.join(d, "both." + kind)
        run(both, "-o", mid, "-f", "-q")
        r = run(mid, "-o", X, "-f")
        check(xisf_properties(X)[0][0] == truth_both and "the astrometric solution among them" in r.stderr,
              f"a solution next to WCS keywords, one of them -0, comes back from {kind.upper()} as it was")
    # WCS keywords without a solution: nothing is made of them that the XISF file did not have
    keywords_only = os.path.join(d, "keywords_only.xisf")
    cards = {"CTYPE1": "'RA---TAN'", "CTYPE2": "'DEC--TAN'", "CRVAL1": "328.178", "CRVAL2": "47.358", "CRPIX1": "150.8",
             "CRPIX2": "100.2", "CD1_1": "-2.3565E-4", "CD1_2": "1.1696E-5", "CD2_1": "-1.1715E-5", "CD2_2": "2.3575D-4"}
    write_xisf(keywords_only, [image_entry(sky, children="".join(
        f'<FITSKeyword name="{k}" value="{v}" comment=""/>' for k, v in cards.items()) +
        '<Property id="Instrument:Filter:Name" type="String">OIII</Property>')])
    check("CTYPE1" in xisf_header(keywords_only)[1] and P not in xisf_header(keywords_only)[1], "(a file with WCS keywords and no solution)")
    for kind in ("fits", "asdf"):
        mid = os.path.join(d, "keywords_only." + kind)
        run(keywords_only, "-o", mid, "-f", "-q")
        r = run(mid, "-o", X, "-f")
        check(not any(p["id"].startswith(P) for p in xisf_properties(X)[0][0]) and "1 XISF property restored" in r.stderr,
              f"an XISF file with WCS keywords and no solution comes back from {kind.upper()} without one: {r.stderr.strip()[-200:]}")
    r = run(mid, "-o", X, "-f", "--no-properties")
    check(any(p["id"].startswith(P) for p in xisf_properties(X)[0][0]), "while the same keywords alone are made into a solution, as before")


# ---------------------------------------------------------------- smaller pictures and the thumbnailer

def area_weights(n, m, use=None):
    """The share of each of the first `use` pixels of n that each of m pixels covers: an m x n matrix,
    computed with fractions, so that nothing here rounds the way xisfconv might."""
    from fractions import Fraction
    use = n if use is None else use
    w = np.zeros((m, n))
    for j in range(m):
        a, b = Fraction(j * use, m), Fraction((j + 1) * use, m)
        for i in range(int(a), min(use, int(b) + 1)):
            share = min(b, i + 1) - max(a, i)
            if share > 0:
                w[j, i] = float(share)
    return w


def reference_picture(plane, out_h, out_w, use_h=None, use_w=None):
    """The mean of the pixels each pixel of the picture covers (float64), NaN and Inf left out."""
    wy, wx = area_weights(plane.shape[0], out_h, use_h), area_weights(plane.shape[1], out_w, use_w)
    a = plane.astype(np.float64)
    good = np.isfinite(a)
    total = wy @ np.where(good, a, 0.0) @ wx.T
    share = wy @ good.astype(np.float64) @ wx.T
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(share > 0, total / share, np.nan)


def picture_matches(got, plane, out_h, out_w, use_h=None, use_w=None):
    """True if `got` is the reference picture: integers rounded to the nearest (either neighbour where the
    mean is half way), floating point to the precision of its type."""
    ref = reference_picture(plane, out_h, out_w, use_h, use_w)
    if got.shape != ref.shape:
        return False
    if np.issubdtype(got.dtype, np.integer):
        g = got.astype(np.float64)
        slack = 0.5 + 1e-9 * np.maximum(1.0, np.abs(ref))
        if got.dtype.itemsize == 8:
            slack = slack + np.abs(ref) * 2.0 ** -51      # a double holds 53 bits of a 64-bit sample
        return bool(np.all(np.abs(g - ref) <= slack))
    eps = 4 * np.finfo(got.dtype).eps
    return bool(np.array_equal(np.isnan(got), np.isnan(ref)) and
                np.allclose(got[np.isfinite(ref)], ref[np.isfinite(ref)], rtol=eps, atol=eps * 1e-3))


def test_downsampling_and_thumbnailer():
    """--bin and --resize for TIFF and PNG output, and the thumbnailer entry that uses them."""
    import configparser
    import shlex
    d = os.path.join(TMP, "smaller")
    os.makedirs(d, exist_ok=True)
    H, W = 23, 31   # no multiple of 2, 3 or 4
    out = os.path.join(d, "out.tif")

    def png(path):
        return decode_png(path)[0]

    def pages(path, *flags, name="out.tif"):
        target = os.path.join(d, name)
        r = run(path, "-o", target, "-f", *flags)
        return [p if p.ndim == 3 else p[..., None] for p in tiff_array(target)], r

    def expect(label, got, source, out_h, out_w, use_h=None, use_w=None):
        ok = got.shape == (out_h, out_w, source.shape[2]) and got.dtype == source.dtype and all(
            picture_matches(got[..., c], source[..., c], out_h, out_w, use_h, use_w) for c in range(source.shape[2]))
        check(ok, f"{label}: {got.shape} {got.dtype}")

    # ---- every sample type, gray and colour, from XISF
    for dtype in (np.uint8, np.uint16, np.uint32, np.uint64, np.float32, np.float64):
        for channels in (1, 3):
            a = test_image(dtype, H, W, channels, 91)
            name = np.dtype(dtype).name
            p = os.path.join(d, f"s_{name}_{channels}.xisf")
            write_xisf(p, [image_entry(a, codec="zlib", shuffle_item=np.dtype(dtype).itemsize)])
            got, r = pages(p, "--bin", "2")
            expect(f"--bin 2, {name} x{channels}", got[0], a, 11, 15, 22, 30)
            check("23 pixels averaged to 15 x 11" in r.stderr.replace(f"{W} x ", "") and "1 column(s) and 1 row(s)" in r.stderr,
                  f"--bin 2: the note says what became of the image: {r.stderr.strip()[-200:]}")
            got, _ = pages(p, "--bin", "3")
            expect(f"--bin 3, {name} x{channels}", got[0], a, 7, 10, 21, 30)
            got, _ = pages(p, "--resize", "10")
            expect(f"--resize 10 (the longest side), {name} x{channels}", got[0], a, 7, 10)
            got, _ = pages(p, "--resize", "40%")
            expect(f"--resize 40%, {name} x{channels}", got[0], a, 9, 12)
    a = test_image(np.uint16, H, W, 1, 92)
    p = os.path.join(d, "gray.xisf")
    write_xisf(p, [image_entry(a)])
    pages(p)
    plain = open(out, "rb").read()
    # an exact case, written out: 2 x 2 blocks of 16-bit integers
    blocks = a[:22, :30, 0].astype(np.float64).reshape(11, 2, 15, 2).mean(axis=(1, 3))
    got, _ = pages(p, "--bin", "2")
    check(np.array_equal(got[0][..., 0], np.floor(blocks + 0.5).astype(np.uint16)), "--bin 2 is the mean of every 2 x 2 pixels, rounded to the nearest")

    # ---- the forms of --resize, and what they do not do
    for spec, (oh, ow) in (("12x5", (5, 7)), ("5x12", (4, 5)), ("31x23", (H, W)), ("16", (12, 16)), ("1", (1, 1)), ("100%", (H, W)),
                           ("50%", (12, 16)), ("3.3%", (1, 1)), ("1X200", (1, 1)), ("200x1", (1, 1))):
        got, _ = pages(p, "--resize", spec)
        expect(f"--resize {spec}", got[0], a, oh, ow)
    for flags in (["--resize", "100"], ["--resize", "400x300"], ["--bin", "1"], ["--resize", "31"], ["--resize", "100%"]):
        _, r = pages(p, *flags)
        check(open(out, "rb").read() == plain and "averaged" not in r.stderr, f"{flags}: a picture is never larger than the image; the file is the one without the option")
    got, _ = pages(p, "--bin", "50")
    check(got[0].shape == (1, 1, 1) and abs(float(got[0][0, 0, 0]) - a.astype(np.float64).mean()) <= 0.5, "--bin 50 of a small image is one pixel: its mean")
    got, _ = pages(p, "--bin", "2", "--resize", "5")
    expect("--bin 2 --resize 5: the blocks of the bin, then the size", got[0], a, 4, 5, 22, 30)
    got, _ = pages(p, "--resize", "50%", "--resize", "6")
    expect("of two --resize the last one counts", got[0], a, 4, 6)
    for flags, message in ((["--bin", "0"], "--bin expects"), (["--bin", "two"], "--bin expects"), (["--bin", "-2"], "--bin expects"),
                           (["--resize", "0"], "--resize expects"), (["--resize", "10x"], "--resize expects"),
                           (["--resize", "x10"], "--resize expects"), (["--resize", "0x10"], "--resize expects"),
                           (["--resize", "150%"], "--resize expects"), (["--resize", "0%"], "--resize expects"),
                           (["--resize", "%"], "--resize expects"), (["--resize", "big"], "--resize expects"),
                           (["--resize", "-5"], "--resize expects"), (["--resize", "10x10x10"], "--resize expects")):
        r = run(p, "-o", out, "-f", *flags, expect_ok=False)
        check(r.returncode != 0 and message in r.stderr, f"{flags} is refused: {r.stderr.strip()[-120:]}")
    f_in = os.path.join(d, "gray.fits")
    run(p, "-o", f_in, "-f", "-q")
    for source, target in ((p, "x.fits"), (p, "x.asdf"), (p, "x.xisf"), (p, "x.fits.fz"), (f_in, "y.xisf"), (f_in, "y.asdf"), (f_in, "y.fits.fz")):
        for flags in (["--bin", "2"], ["--resize", "10"], ["--resize", "50%"], ["--resize", "100%"], ["--resize", "5000"]):
            r = run(source, "-o", os.path.join(d, target), "-f", *flags, expect_ok=False)
            check(r.returncode != 0 and "TIFF and PNG" in r.stderr and not os.path.exists(os.path.join(d, target)),
                  f"{flags} for {target} from {os.path.basename(source)} is refused: an image that is data keeps its pixels")

    # ---- floating point samples that are no numbers are left out of the mean
    holes = test_image(np.float32, 12, 16, 1, 93)[..., 0].astype(np.float32)
    holes[0:4, 0:4] = np.nan           # two blocks of 4 x 4 ... no: one whole block of 4 x 4
    holes[5, 9] = np.nan               # one pixel of a block
    holes[8:10, 12:16] = np.inf        # half a block
    hf = os.path.join(d, "holes.fits")
    fits.PrimaryHDU(holes[::-1]).writeto(hf, overwrite=True)   # (stored bottom-up: `holes` is the picture from the top)
    got, r = pages(hf, "--bin", "4", "--bounds", "0:1")
    ref = reference_picture(holes, 3, 4)
    check(got[0].shape == (3, 4, 1) and np.isnan(got[0][0, 0, 0]) and np.isnan(ref[0, 0]) and np.isfinite(got[0][1:, :, 0]).all() and
          np.isfinite(got[0][0, 1:, 0]).all() and picture_matches(got[0][..., 0], holes, 3, 4),
          "NaN and Inf are left out of the mean; a pixel that covers nothing else is NaN")
    check("NaN/Inf" in r.stderr, "and the warning about them stays, as the picture has one")
    holes[0:4, 0:4] = 0.25
    fits.PrimaryHDU(holes[::-1]).writeto(hf, overwrite=True)
    got, r = pages(hf, "--bin", "4", "--bounds", "0:1")
    check(np.isfinite(got[0]).all() and "NaN/Inf" not in r.stderr and "NaN/Inf" in pages(hf, "--bounds", "0:1")[1].stderr,
          "a picture in which none is left gets no warning about them (the image as it is does)")
    # samples at the edge of what the sums can hold: the mean of equal values is that value
    for dtype, value in ((np.uint64, 2 ** 52 + 1), (np.uint64, 2 ** 53 - 1), (np.uint64, 2 ** 64 - 1), (np.uint32, 2 ** 32 - 1),
                         (np.float64, 1e308), (np.float64, -1.7e308), (np.float32, 3.4e38), (np.float64, 5e-324)):
        flat = np.full((6, 9, 1), value, dtype)
        fx = os.path.join(d, "flat.xisf")
        write_xisf(fx, [image_entry(flat)])
        for flags in (["--bin", "3"], ["--resize", "4"]):
            got, _ = pages(fx, *flags)
            # (the sums are doubles: a mean of 64-bit samples is right to their last bit or two)
            stored = float(np.dtype(dtype).type(value))
            off = np.abs(got[0].astype(np.float64) - stored).max()
            slack = abs(stored) * 2.0 ** -51 if np.dtype(dtype).itemsize == 8 else 0.0
            check(got[0].dtype == dtype and np.isfinite(got[0].astype(np.float64)).all() and off <= slack,
                  f"{flags}: a flat image of {value!r} ({np.dtype(dtype).name}) stays {value!r}: {got[0].ravel()[:2]}")

    # ---- FITS and ASDF input: the picture is that of the image the right way up
    odd = test_image(np.uint16, H, W, 1, 94)
    of = os.path.join(d, "odd.fits")
    fits.PrimaryHDU(odd[::-1, :, 0]).writeto(of, overwrite=True)
    oa = os.path.join(d, "odd.asdf")
    run(of, "-o", oa, "-f", "-q")
    for source in (of, oa):
        got, _ = pages(source, "--bin", "2")
        expect(f"--bin 2 from {os.path.splitext(source)[1][1:].upper()}: the rows left out are the last of the picture", got[0], odd, 11, 15, 22, 30)
        got, _ = pages(source, "--resize", "9x9")
        expect(f"--resize 9x9 from {os.path.splitext(source)[1][1:].upper()}", got[0], odd, 7, 9)
    top = os.path.join(d, "top.fits")
    fits.PrimaryHDU(odd[..., 0], header=fits.Header([("ROWORDER", "TOP-DOWN")])).writeto(top, overwrite=True)
    got, _ = pages(top, "--bin", "2")
    expect("--bin 2 from a FITS file with its rows top-down", got[0], odd, 11, 15, 22, 30)

    # ---- several images: every page; a cube that is no colour image: every plane
    b = test_image(np.float32, 10, 14, 3, 95)
    two = os.path.join(d, "two.xisf")
    write_xisf(two, [with_id(image_entry(a), "one"), with_id(image_entry(b), "two")])
    got, _ = pages(two, "--bin", "2")
    check(len(got) == 2, "two images, two pages")
    expect("the first page", got[0], a, 11, 15, 22, 30)
    expect("the second page", got[1], b, 5, 7)
    cube = np.stack([test_image(np.uint16, 8, 10, 1, 96 + k)[..., 0] for k in range(5)])
    cf = os.path.join(d, "cube.fits")
    fits.PrimaryHDU(cube[:, ::-1, :]).writeto(cf, overwrite=True)
    got, _ = pages(cf, "--resize", "50%")
    check(len(got) == 5 and all(picture_matches(got[k][..., 0], cube[k], 4, 5) for k in range(5)), "the planes of a cube become smaller pages")

    # ---- a stretch is applied to the picture, not the other way round
    sky = astro_image(120, 180, 3, 97)
    sx = os.path.join(d, "sky.xisf")
    write_xisf(sx, [image_entry(sky)])
    binned = np.stack([reference_picture(sky[..., c], 40, 60) for c in range(3)], axis=-1).astype(np.float32)
    bx = os.path.join(d, "sky_binned.xisf")
    write_xisf(bx, [image_entry(binned)])
    for kind, flags in (("png", ["-s", "-b", "u8"]), ("png", ["--stretch=unlinked", "-b", "u16"]), ("tif", ["-s"]), ("png", [])):
        small, same = os.path.join(d, "sky_small." + kind), os.path.join(d, "sky_same." + kind)
        r = run(sx, "-o", small, "-f", "--bin", "3", *flags)
        run(bx, "-o", same, "-f", "-q", *flags)
        g1 = png(small) if kind == "png" else tiff_array(small)[0]
        g2 = png(same) if kind == "png" else tiff_array(same)[0]
        check(g1.shape[:2] == (40, 60) and np.array_equal(g1, g2),
              f"--bin 3 {flags} -> {kind}: the picture is what the binned image gives with the same options")
        if "-s" in flags:
            check(r.stderr.index("averaged to") < r.stderr.index("auto-STF"), "the notes come in the order of the work: averaged, then stretched")
    ff = os.path.join(d, "sky.fits")
    run(sx, "-o", ff, "-f", "-q")
    small, same = os.path.join(d, "sky_small_f.png"), os.path.join(d, "sky_same_f.png")
    run(ff, "-o", small, "-f", "-q", "--bin", "3", "-s", "-b", "u8")
    run(bx, "-o", os.path.join(d, "sky_binned.fits"), "-f", "-q")
    run(os.path.join(d, "sky_binned.fits"), "-o", same, "-f", "-q", "-s", "-b", "u8")
    check(np.array_equal(png(small), png(same)), "and so it is from FITS")

    # ---- an alpha channel is averaged like the others; the resolution follows the size
    rgba = test_image(np.uint8, 12, 16, 4, 98)
    ax = os.path.join(d, "rgba.xisf")
    entry = image_entry(rgba, color="RGB", children='<Resolution horizontal="300" vertical="300" unit="inch"/>')
    write_xisf(ax, [entry])
    ap = os.path.join(d, "rgba.png")
    run(ax, "-o", ap, "-f", "-q", "--bin", "2")
    got = png(ap)
    check(got.shape == (6, 8, 4) and all(picture_matches(got[..., c], rgba[..., c], 6, 8) for c in range(4)), "RGB with alpha -> PNG, --bin 2")
    run(ax, "-o", out, "-f", "-q", "--bin", "2")
    with tifffile.TiffFile(out) as t:
        check(t.pages[0].tags["XResolution"].value == (150, 1) and t.pages[0].tags["YResolution"].value == (150, 1),
              f"half the pixels per inch, so that the picture is as large on paper: {t.pages[0].tags['XResolution'].value}")
    # (each side by itself, and counted from the part of the image that is used)
    uneven = image_entry(test_image(np.uint8, 23, 31, 1, 98), children='<Resolution horizontal="300" vertical="150" unit="inch"/>')
    ux = os.path.join(d, "uneven.xisf")
    write_xisf(ux, [uneven])
    for flags, (xres, yres) in ((["--bin", "2"], (150.0, 75.0)), (["--bin", "40"], (300 / 31, 150 / 23)), (["--resize", "10"], (300 * 10 / 31, 150 * 7 / 23))):
        run(ux, "-o", out, "-f", "-q", *flags)
        with tifffile.TiffFile(out) as t:
            x, y = (t.pages[0].tags[k].value for k in ("XResolution", "YResolution"))
            check(abs(x[0] / x[1] - xres) < 1e-3 and abs(y[0] / y[1] - yres) < 1e-3,
                  f"{flags}: {x[0] / x[1]:.3f} x {y[0] / y[1]:.3f} pixels per inch, for {xres:.3f} x {yres:.3f}")

    # ---- the thumbnailer entry: the command a file manager runs
    desktop = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "desktop")
    entry = configparser.ConfigParser(interpolation=None)
    entry.optionxform = str
    entry.read(os.path.join(desktop, "xisfconv.thumbnailer"), encoding="utf-8")
    check(entry.sections() == ["Thumbnailer Entry"] and set(entry["Thumbnailer Entry"]) == {"TryExec", "Exec", "MimeType"},
          f"the thumbnailer entry has its three keys: {dict(entry['Thumbnailer Entry']) if entry.sections() else None}")
    command = shlex.split(entry["Thumbnailer Entry"]["Exec"])
    check(command[0] == entry["Thumbnailer Entry"]["TryExec"] == "xisfconv" and command.count("%s") == 1 and command.count("%i") == 1 and
          command.count("%o") == 1 and command[-2:] == ["--", "%i"],
          f"it names the program, the size, the output and, as the last word and after '--', the input: {command}")
    types = entry["Thumbnailer Entry"]["MimeType"]
    check(types.endswith(";") and {"image/x-xisf", "application/fits", "image/fits", "application/x-asdf"} <= set(types.split(";")),
          f"and the file types: {types}")

    def thumbnail(source, size, name="thumb.png"):
        target = os.path.join(d, name)
        if os.path.exists(target):
            os.remove(target)
        args = [EXE] + [str(size) if c == "%s" else source if c == "%i" else target if c == "%o" else c for c in command[1:]]
        r = subprocess.run(args, capture_output=True, text=True)
        return (png(target) if r.returncode == 0 and os.path.exists(target) else None), r

    big = astro_image(300, 450, 3, 99)
    bigx = os.path.join(d, "big.xisf")
    write_xisf(bigx, [image_entry(big, codec="zlib", shuffle_item=4)])
    sources = {"XISF": bigx}
    for label, name, flags in (("FITS", "big.fits", []), ("tile-compressed FITS", "big.fits.fz", []), ("ASDF", "big.asdf", [])):
        sources[label] = os.path.join(d, name)
        run(bigx, "-o", sources[label], "-f", "-q", *flags)
    reference = None
    for label, source in sources.items():
        for size in (128, 256):
            got, r = thumbnail(source, size)
            check(got is not None and got.shape == (round(300 * size / 450), size, 3) and got.dtype == np.uint8 and r.stdout == "" and r.stderr == "",
                  f"thumbnail of {label}, {size} pixels: {None if got is None else got.shape} {r.stderr.strip()[-200:]}")
            if got is not None and size == 256:
                # a picture of the sky: dark, not black, with stars, and the same from every format
                check(40 < np.median(got) < 90 and got.max() == 255, f"thumbnail of {label}: stretched for viewing (median {np.median(got)})")
                reference = got if reference is None else reference
                check(np.array_equal(got, reference), f"thumbnail of {label}: the picture the XISF file gives")
    got, r = thumbnail(bigx, 1024)
    check(got is not None and got.shape == (300, 450, 3), "an image smaller than the thumbnail asked for is not made larger")
    got, r = thumbnail(bigx, 128, name="thumbnail-without-extension")
    check(got is not None and got.shape == (85, 128, 3), "the output is PNG whatever its name")
    got, r = thumbnail(two, 16)
    check(got is not None and got.shape == (12, 16, 1), "a file with several images: the first one")
    # the header file of a distributed unit, where the file beside it may be read
    run(bigx, "-o", os.path.join(d, "big.xish"), "-f", "-q")
    got, r = thumbnail(os.path.join(d, "big.xish"), 256)
    check(got is not None and np.array_equal(got, reference) and r.stderr == "", f"thumbnail of a distributed unit: {r.stderr.strip()[-200:]}")
    os.rename(os.path.join(d, "big.xisb"), os.path.join(d, "big-away.xisb"))
    got, r = thumbnail(os.path.join(d, "big.xish"), 256)
    check(got is None and r.returncode != 0 and not os.path.exists(os.path.join(d, "thumb.png.part")),
          "... and none, with an exit status that says so, where it may not (a sandbox that holds the header alone)")
    got, r = thumbnail(os.path.join(d, "none.xisf"), 128)
    check(got is None and r.returncode != 0, "a file that is not there: no thumbnail, and an exit status that says so")
    open(os.path.join(d, "garbage.xisf"), "wb").write(b"XISF0100" + bytes(100))
    got, r = thumbnail(os.path.join(d, "garbage.xisf"), 128)
    check(got is None and r.returncode != 0 and not os.path.exists(os.path.join(d, "thumb.png.part")), "a damaged file: no thumbnail and nothing left behind")

    # ---- the file types the entry names
    import xml.etree.ElementTree as ET
    ns = "{http://www.freedesktop.org/standards/shared-mime-info}"
    root = ET.parse(os.path.join(desktop, "xisfconv.xml")).getroot()
    defined = {t.get("type"): t for t in root.findall(ns + "mime-type")}
    check(root.tag == ns + "mime-info" and set(defined) == {"image/x-xisf", "application/x-asdf"} and
          set(defined) <= set(types.split(";")), f"the file types the desktop does not know are defined: {sorted(defined)}")
    globs = {name: sorted(g.get("pattern") for g in t.findall(ns + "glob")) for name, t in defined.items()}
    check(globs == {"image/x-xisf": ["*.xisf", "*.xish"], "application/x-asdf": ["*.asdf"]}, f"by the names of the files: {globs}")
    for name, path in (("image/x-xisf", bigx), ("application/x-asdf", sources["ASDF"])):
        match = defined[name].find(ns + "magic").find(ns + "match")
        value = match.get("value").encode()
        head = open(path, "rb").read(64)
        check(match.get("type") == "string" and match.get("offset") == "0" and head.startswith(value),
              f"and {name} by how such a file begins: {value}")
        for inner in match.findall(ns + "match"):   # what has to follow, somewhere in a range of offsets
            first, last = (int(v) for v in inner.get("offset").split(":"))
            at = head.find(inner.get("value").encode())
            check(first <= at <= last, f"{name}: {inner.get('value')!r} follows at offset {at}, within {first}..{last}")
    if shutil.which("update-mime-database"):
        base = os.path.join(d, "share", "mime")
        os.makedirs(os.path.join(base, "packages"), exist_ok=True)
        shutil.copy(os.path.join(desktop, "xisfconv.xml"), os.path.join(base, "packages"))
        r = subprocess.run(["update-mime-database", base], capture_output=True, text=True)
        listed = open(os.path.join(base, "globs2")).read() if os.path.exists(os.path.join(base, "globs2")) else ""
        check(r.returncode == 0 and "image/x-xisf:*.xisf" in listed and "image/x-xisf:*.xish" in listed and "application/x-asdf:*.asdf" in listed and
              os.path.exists(os.path.join(base, "image", "x-xisf.xml")),
              f"update-mime-database takes the file: {r.stderr.strip()[-200:]}")
    else:
        skipped.append("the file types through update-mime-database")


# ---------------------------------------------------------------- distributed XISF units

# The directory with the sample programs of OpenXISF (github.com/openxisf/openxisf), another
# implementation of the specification: where it is given, each reads what the other wrote.
OPENXISF = os.environ.get("OPENXISF_BIN", "")


def same_place(shown, path):
    """True if the path a program printed is that file. (A Windows program that is run on another
    system prints the paths of its own world: those are held against the end of the path.)"""
    try:
        return os.path.samefile(shown, path)
    except OSError:
        return shown.replace("\\", "/").split("/")[-2:] == os.path.abspath(path).replace("\\", "/").split("/")[-2:]


def unit_structure(label, xish, rewritten=False):
    """The files of a distributed unit xisfconv wrote, held against the specification (sections 9.3,
    9.4 and 10.3), without xisfconv. Returns the blocks of the unit. A unit that is written from
    pixels has every block at a multiple of 4096 bytes; one that is rewritten from another XISF
    file has the uncompressed ones there, and the compressed ones behind each other, as in a
    monolithic file."""
    import re
    raw, hdr = xisf_header(xish)
    xisb_path = os.path.splitext(xish)[0] + ".xisb"
    check(raw.startswith(b'<?xml version="1.0" encoding="UTF-8"?>') and raw.rstrip().endswith(b"</xisf>"),
          f"{label}: the header file is the XML header and nothing else")
    check("attachment:" not in hdr and "XISF:BlockAlignmentSize" not in hdr, f"{label}: nothing is attached to a header file")
    x = read_xisb(xisb_path)
    check(x["signature"] == b"XISB0100" and x["reserved"] == bytes(8), f"{label}: signature and reserved field of the data blocks file")
    check(len(x["nodes"]) == 1 and x["nodes"][0] == {"position": 16, "length": len(x["elements"]), "reserved": 0, "next": 0},
          f"{label}: a block index of one node behind the signature: {x['nodes']}")
    blocks = xisf_blocks(xish)
    outside = [b for b in blocks if b["kind"] == "path"]
    name = re.escape(os.path.basename(xisb_path))
    named = [re.fullmatch(r"path\(@header_dir/%s\):(0x[0-9a-f]{16})" % name, b["location"]) for b in outside]
    check(all(named) and not [b for b in blocks if b["kind"] not in ("path", "inline", "embedded")],
          f"{label}: the header names its blocks as path(@header_dir/<name>.xisb):0x<16 digits>")
    ids = [e["id"] for e in x["elements"]]
    check(sorted(int(m.group(1), 16) for m in named if m) == sorted(ids) and len(set(ids)) == len(ids) and 0 not in ids,
          f"{label}: the index has the blocks the header names, each under an identifier of its own")
    by_id = {e["id"]: e for e in x["elements"]}
    lengths = True
    used = bytearray(len(x["raw"]))
    used[:32 + 40 * len(ids)] = b"\1" * (32 + 40 * len(ids))
    for b, m in zip(outside, named):
        e = by_id.get(int(m.group(1), 16)) if m else None
        if not e:
            continue
        compression = b["attr"]["compression"]
        lengths = lengths and e["uncompressed"] == (int(compression.split(":")[1]) if compression else 0) and e["reserved"] == 0
        lengths = lengths and (e["position"] % 4096 == 0 or (rewritten and compression)) and e["length"] == len(b["stored"])
        used[e["position"]:e["position"] + e["length"]] = b"\1" * e["length"]
    check(lengths, f"{label}: each element has the length of its block, the uncompressed length of a compressed one, and an aligned position")
    check(not any(byte for byte, taken in zip(x["raw"], used) if not taken), f"{label}: the unused space of the data blocks file is zero")
    return blocks


def test_distributed_units():
    """XISF units of a header file (.xish) and the files it names (data blocks files, .xisb, and
    others): written by xisfconv and read without it, made without it and read by it, packed into
    monolithic files and unpacked from them; what a header may not be followed to; and files that
    are damaged."""
    import re
    d = os.path.join(TMP, "distributed")
    os.makedirs(d, exist_ok=True)
    zstd_ok = zstandard is not None and ZSTD_BUILD
    leftovers = lambda where: sorted(n for n in os.listdir(where) if n.endswith(".part"))   # noqa: E731

    # ---- from FITS: written by xisfconv, read without it
    variants = [(np.uint16, 1, []), (np.uint8, 3, ["--codec", "zlib", "--checksum", "sha256"]), (np.float64, 1, ["--codec", "lz4hc"]),
                (np.uint32, 3, ["--codec", "lz4", "--checksum", "sha1"])]
    if zstd_ok:
        variants.append((np.float32, 3, ["-c", "--checksum", "sha512"]))
    for number, (dtype, channels, options) in enumerate(variants):
        label = f"FITS -> .xish ({np.dtype(dtype).name}, {channels} ch, {' '.join(options) or 'plain'})"
        a = test_image(dtype, 61, 83, channels, 300 + number)
        planes = as_planes(a)                                   # (C, H, W), rows top-down
        src = os.path.join(d, f"in{number}.fits")
        fits.PrimaryHDU(np.squeeze(planes[:, ::-1, :])).writeto(src, overwrite=True)
        out = os.path.join(d, f"unit{number}.xish")
        r = run(src, "-o", out, *options)
        check(sorted(n for n in os.listdir(d) if n.startswith(f"unit{number}.")) == [f"unit{number}.xisb", f"unit{number}.xish"]
              and not leftovers(d), f"{label}: a header file and a data blocks file, and nothing else: {r.stdout.strip()}")
        blocks = unit_structure(label, out)
        image = [b for b in blocks if b["tag"] == "Image"][0]
        got = np.frombuffer(image["data"], np.dtype(dtype).newbyteorder("<")).reshape(planes.shape)
        compare(label + ": the pixels, decoded without xisfconv", got, planes)
        if options:
            check(image["attr"]["compression"] is not None, f"{label}: the block in the data blocks file is compressed")
        r = run("--verify", out)
        check("OK" in r.stdout and "a distributed unit with data in 1 other file" in r.stdout, f"{label}: --verify: {r.stdout.strip()}")
        back = os.path.join(d, f"back{number}.fits")
        run(out, "-o", back, "-f", "-q")
        compare(label + ": and back to FITS", fits_planes(back)[0], planes)
        if OPENXISF:
            r = subprocess.run([os.path.join(OPENXISF, "read_pixels"), out], capture_output=True, text=True)
            means = [float(v) for v in re.findall(r"mean of channel \d+: (\S+)", r.stdout)]
            check(r.returncode == 0 and len(means) == channels and
                  all(abs(m - float(planes[c].astype(np.float64).mean())) <= 2e-5 * max(1.0, abs(m)) for c, m in enumerate(means)),
                  f"{label}: OpenXISF reads the unit: {r.stdout.strip()[:200]} {r.stderr.strip()[:200]}")
    # -t xish names the files; an output that exists is not written over, neither of the two
    src = os.path.join(d, "in0.fits")
    named = os.path.join(d, "named")
    os.makedirs(named, exist_ok=True)
    r = run(src, "-t", "xish", "-d", named)
    check(sorted(os.listdir(named)) == ["in0.xisb", "in0.xish"] and r.stdout.strip().endswith("in0.xish"), f"-t xish: {sorted(os.listdir(named))}")
    before = {n: open(os.path.join(named, n), "rb").read() for n in os.listdir(named)}
    r = run(src, "-t", "xish", "-d", named, expect_ok=False)
    check(r.returncode == 1 and "in0.xish already exists" in r.stderr, f"an existing header file is not written over: {r.stderr.strip()[:160]}")
    os.remove(os.path.join(named, "in0.xish"))
    r = run(src, "-t", "xish", "-d", named, expect_ok=False)
    check(r.returncode == 1 and "in0.xisb already exists" in r.stderr and os.listdir(named) == ["in0.xisb"] and
          open(os.path.join(named, "in0.xisb"), "rb").read() == before["in0.xisb"],
          f"nor is an existing data blocks file, and no header is written then: {r.stderr.strip()[:160]}")
    r = run(src, "-t", "xish", "-d", named, "-f")
    ids_before = re.findall(rb"0x[0-9a-f]{16}", before["in0.xish"])
    ids_after = re.findall(rb"0x[0-9a-f]{16}", open(os.path.join(named, "in0.xish"), "rb").read())
    check(r.returncode == 0 and len(ids_after) == len(ids_before) >= 1 and not set(ids_after) & set(ids_before) and not leftovers(named),
          "with --force both are written, with identifiers no earlier header has")
    r = run(src, "-t", "xish", "-o", os.path.join(named, "wrong.xisf"), expect_ok=False)
    check(r.returncode != 0 and ".xish" in r.stderr and not os.path.exists(os.path.join(named, "wrong.xisf")),
          f"-t xish with a name that is not one of a header file: {r.stderr.strip()[:160]}")
    r = run(src, "-o", os.path.join(named, "UPPER.XISH"))
    check(sorted(n for n in os.listdir(named) if n.startswith("UPPER")) == ["UPPER.XISB", "UPPER.XISH"], "a name in capitals keeps them")

    # ---- made without xisfconv: every form the specification has, read by xisfconv
    storages = [("plain", {}, {}), ("zlib, shuffled, sha1", dict(codec="zlib", shuffle_item=2, checksum="sha1"), dict(nodes=3, free=2, decimal=(1, 3))),
                ("lz4hc in subblocks", dict(codec="lz4hc", subblocks=3), dict(nodes=2, name="blöcke (1).xisb")),
                ("lz4, sha256, in a directory below", dict(codec="lz4", checksum="sha256"), dict(nodes=6, free=1, name="sub/dir/data.xisb", align=4096))]
    if zstd_ok:
        storages.append(("zstd, shuffled", dict(codec="zstd", shuffle_item=2), dict(free=3, tail=b"\0" * 100)))
    for number, (what, storage, layout) in enumerate(storages):
        label = f"a unit made without xisfconv ({what})"
        unit = os.path.join(d, f"made{number}.xish")
        expected = rich_xisf(unit, storage, writer=write_unit, **layout)
        theirs = xisf_blocks(unit)
        r = run("--info", unit)
        files = re.findall(r"^  data in:     (.*)$", r.stdout, re.M)
        check("distributed unit" in r.stdout.splitlines()[0] and "in 2 files" in r.stdout and len(files) == 1 and
              same_place(files[0], os.path.join(d, *layout.get("name", f"made{number}.xisb").split("/"))),
              f"{label}: --info names the unit and its data blocks file: {r.stdout.splitlines()[0]} {files}")
        r = run("--verify", unit)
        check(r.returncode == 0 and ": OK" in r.stdout, f"{label}: --verify: {r.stdout.strip()[:300]}")
        out = os.path.join(d, f"made{number}.fits")
        r = run(unit, "-o", out, "-f", "-q")
        with fits.open(out) as hdul:
            pictures = [i for i, h in enumerate(hdul) if isinstance(h, (fits.PrimaryHDU, fits.ImageHDU))]
        check(len(pictures) == 3, f"{label}: three images in the FITS file: {pictures}")
        for hdu, name in zip(pictures, ("main", "mask", "small")):
            compare(f"{label}: image {name} to FITS", fits_planes(out, hdu)[0], as_planes(expected[name]))
        # packed into one file: every block as it was, and the header the same text
        packed = os.path.join(d, f"packed{number}.xisf")
        r = run(unit, "-o", packed, "-f")
        mine = xisf_blocks(packed)
        check([b["data"] for b in mine] == [b["data"] for b in theirs] and [b["stored"] for b in mine] == [b["stored"] for b in theirs]
              and [b["attr"] for b in mine] == [b["attr"] for b in theirs], f"{label}: packed into a monolithic file, every block is the bytes it was")
        check([b["kind"] for b in mine] == [("attachment" if b["kind"] == "path" else b["kind"]) for b in theirs] and
              header_without_storage(xisf_header(packed)[1]) == header_without_storage(xisf_header(unit)[1]) and
              "read back and compared" in r.stdout, f"{label}: ... attached where it was in another file, and the header is the same text")
        # and unpacked again, by xisfconv, into a unit of its own making
        again = os.path.join(d, f"again{number}.xish")
        r = run(packed, "-o", again, "-f")
        blocks = unit_structure(f"{label}, packed and unpacked", again, rewritten=True)
        check([b["data"] for b in blocks] == [b["data"] for b in theirs] and [b["attr"] for b in blocks] == [b["attr"] for b in theirs] and
              header_without_storage(xisf_header(again)[1]) == header_without_storage(xisf_header(unit)[1]),
              f"{label}: unpacked again, the same blocks and the same header")
        if OPENXISF:
            r = subprocess.run([os.path.join(OPENXISF, "read_info"), again], capture_output=True, text=True)
            check(r.returncode == 0 and "header file" in r.stdout and "31 x 23 pixels" in r.stdout and "12 x 9 pixels" in r.stdout,
                  f"{label}: OpenXISF reads the unit xisfconv made of it: {r.stdout.strip()[:120]} {r.stderr.strip()[:200]}")

    # ---- what a header is followed to, and what it is not
    a = test_image(np.uint16, 6, 8, 1, 7)
    pixels = as_planes(a).astype("<u2").tobytes()
    vector = np.arange(400, dtype="<f8").tobytes()
    template = ('<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">'
                '<Image geometry="8:6:1" sampleFormat="UInt16" colorSpace="Gray" %s>'
                '<Property id="P:Vector" type="F64Vector" length="400" %s/></Image></xisf>')
    policy = os.path.join(d, "policy")
    elsewhere = os.path.join(d, "elsewhere")
    for directory in (policy, elsewhere, os.path.join(policy, "below")):
        os.makedirs(directory, exist_ok=True)

    def unit_of(name, image_location, vector_location='location="inline:base64">%s</Property><Property id="P:None" type="Int32" value="1"' %
                base64.b64encode(vector).decode()):
        path = os.path.join(policy, name)
        open(path, "wb").write((template % (image_location, vector_location)).encode())
        return path

    def converted(unit, *options):
        out = os.path.join(policy, "out.fits")
        if os.path.exists(out):
            os.remove(out)
        r = run(unit, "-o", out, *options, expect_ok=False)
        return r, (fits_planes(out)[0] if r.returncode == 0 and os.path.exists(out) else None)

    refused = "a header is followed only to files in its own directory"
    for where, name in ((policy, "whole.dat"), (os.path.join(policy, "below"), "whole.dat"), (elsewhere, "whole (1).dat"), (elsewhere, "with space.dat")):
        open(os.path.join(where, name), "wb").write(pixels)
    slashes = lambda path: os.path.abspath(path).replace(os.sep, "/")   # noqa: E731
    outside = slashes(os.path.join(elsewhere, "whole (1).dat")).replace("(", "\\(").replace(")", "\\)")
    cases = [
        ("a file that is one block, beside the header", 'location="path(@header_dir/whole.dat)"', None, None),
        ("... in a directory below", 'location="path(@header_dir/below/whole.dat)"', None, None),
        ("... with more slashes than needed", 'location="path(@header_dir//below///whole.dat)"', None, None),
        ("an absolute path", 'location="path(%s)"' % outside, "which the header names by an absolute path; " + refused, None),
        ("a path that climbs out of the directory", 'location="path(@header_dir/../elsewhere/with space.dat)"',
         "which leads out of the directory of the header; " + refused, None),
        ("a path that climbs out and comes back", 'location="path(@header_dir/below/../../policy/whole.dat)"', "leads out of the directory", None),
        ("a file: URL", 'location="url(file://%s%s)"' % ("" if slashes(elsewhere).startswith("/") else "/",
                                                        slashes(os.path.join(elsewhere, "with space.dat")).replace(" ", "%20")),
         "which the header names by a URL; " + refused, None),
        ("a URL on a network", 'location="url(https://example.com/pixels.dat)"', "nothing is fetched from a network", "nothing is fetched from a network"),
        ("a path that is neither", 'location="path(whole.dat)"', "neither an absolute path nor one that begins with @header_dir/",
         "neither an absolute path nor one that begins with @header_dir/"),
        ("a path without an end", 'location="path(@header_dir/whole.dat"', "malformed location", "malformed location"),
        ("an identifier that is no number", 'location="path(@header_dir/whole.dat):0xZZ"', "malformed location", "malformed location"),
        ("an identifier beyond 64 bits", 'location="path(@header_dir/whole.dat):0x10000000000000000"', "malformed location", "malformed location"),
        ("a path of nothing", 'location="path(@header_dir/)"', "malformed location", "malformed location"),
        ("an attachment", 'location="attachment:4096:96"', "nothing is attached to it", "nothing is attached to it"),
    ]
    for number, (what, location, default_error, anywhere_error) in enumerate(cases):
        unit = unit_of(f"case{number}.xish", location)
        r, got = converted(unit)
        if default_error is None:
            check(got is not None and np.array_equal(got, as_planes(a)), f"{what}: read: {r.stderr.strip()[:200]}")
        else:
            check(r.returncode == 1 and default_error in r.stderr and got is None and not leftovers(policy),
                  f"{what} is not followed: {r.stderr.strip()[:260]}")
        r, got = converted(unit, "--external-files", "anywhere")
        if anywhere_error is None:
            check(got is not None and np.array_equal(got, as_planes(a)), f"{what}: read with --external-files anywhere: {r.stderr.strip()[:200]}")
        else:
            check(r.returncode == 1 and anywhere_error in r.stderr and got is None, f"{what}: not with --external-files anywhere either: {r.stderr.strip()[:200]}")
        r, got = converted(unit, "--external-files", "none")
        check(r.returncode == 1 and got is None and ("no file but the header is to be opened" in r.stderr or anywhere_error and anywhere_error in r.stderr),
              f"{what}: --external-files none: {r.stderr.strip()[:200]}")
    r = run(unit_of("x.xish", 'location="path(@header_dir/whole.dat)"'), "--external-files", "everywhere", expect_ok=False)
    check(r.returncode == 2 and "header-dir, anywhere or none" in r.stderr, "--external-files with another word")
    # a link in the directory that leads out of it
    link = os.path.join(policy, "link.dat")
    try:
        if os.path.lexists(link):
            os.remove(link)
        os.symlink(os.path.join(elsewhere, "with space.dat"), link)
    except (OSError, NotImplementedError, AttributeError):
        skipped.append("symbolic links (a link that leads out of the directory of a header)")
    else:
        unit = unit_of("link.xish", 'location="path(@header_dir/link.dat)"')
        r, got = converted(unit)
    if os.path.islink(link) and os.sep == "/" and "\\" in run("--info", unit).stdout.split("data in:")[1].splitlines()[0]:
        # (a Windows program that is run on another system does not see that system's links: they are files to it)
        skipped.append("symbolic links that lead out of the directory of a header (the program is not of this system)")
    elif os.path.islink(link):
        check(r.returncode == 1 and "behind a symbolic link that does not lead to a file in the directory of the header" in r.stderr and got is None and
              "with space.dat" not in r.stderr, f"a link that leads out is not followed, and where it leads is not told: {r.stderr.strip()[:300]}")
        # ... nor whether there is something where it leads: a link to nothing is told the same way
        nowhere = os.path.join(policy, "nowhere.dat")
        if os.path.lexists(nowhere):
            os.remove(nowhere)
        os.symlink(os.path.join(elsewhere, "no such file"), nowhere)
        r2, got = converted(unit_of("nowhere.xish", 'location="path(@header_dir/nowhere.dat)"'))
        check(r2.returncode == 1 and got is None and
              r2.stderr.replace("nowhere", "link") == r.stderr, f"a link out of the directory to nothing reads the same: {r2.stderr.strip()[:300]}")
        r, got = converted(unit, "--external-files", "anywhere")
        check(got is not None and np.array_equal(got, as_planes(a)), "... unless that is allowed")
        inside = os.path.join(policy, "inside.dat")
        if os.path.lexists(inside):
            os.remove(inside)
        os.symlink("whole.dat", inside)
        r, got = converted(unit_of("inside.xish", 'location="path(@header_dir/inside.dat)"'))
        check(got is not None and np.array_equal(got, as_planes(a)), f"a link that stays in the directory is followed: {r.stderr.strip()[:200]}")
    # a block that is one of many is left out with a warning; the image is converted
    unit = unit_of("property.xish", 'location="path(@header_dir/whole.dat)"', 'location="path(%s)"' % outside)
    r, got = converted(unit)
    check(got is not None and np.array_equal(got, as_planes(a)) and "property P:Vector is left out: it is in " in r.stderr and refused in r.stderr,
          f"a property in a file the header is not followed to is left out, with a warning: {r.stderr.strip()[:260]}")
    r = run("--verify", unit, expect_ok=False)
    check(r.returncode == 0 and "NOT FULLY CHECKED" in r.stdout and "P:Vector" in r.stdout, f"--verify calls it not checked: {r.stdout.strip()[:260]}")
    r = run("--info", unit)
    check(r.stdout.count("  data in:") == 2 and slashes(os.path.join(elsewhere, "whole (1).dat")) in r.stdout.replace("\\", "/"),
          f"--info names the files of the unit, also one it does not read: {r.stdout[:400]}")
    # a monolithic file holds all of its data: one that names a block in another file is not followed
    # there by its own word, not even to the file beside it (it is what people are sent as "an image")
    mono = os.path.join(policy, "mono.xisf")
    hdr = (template % ('location="path(@header_dir/whole.dat)"', 'location="inline:hex">%s</Property><Property id="P:None" type="Int32" value="1"' % vector.hex())).encode()
    open(mono, "wb").write(b"XISF0100" + len(hdr).to_bytes(4, "little") + bytes(4) + hdr)
    r, got = converted(mono)
    check(r.returncode == 1 and got is None and "this is a monolithic XISF file, which holds all of its data: only a header file (.xish) is followed" in r.stderr,
          f"a monolithic file with a block in another file is not followed there: {r.stderr.strip()[:300]}")
    r, got = converted(mono, "--external-files", "anywhere")
    check(got is not None and np.array_equal(got, as_planes(a)) and "names data in other files" in r.stderr,
          f"... unless a header may lead anywhere; then it is read, with a warning: {r.stderr.strip()[:200]}")
    # the same holds for a header file that is not named as one
    misnamed = unit_of("misnamed.xisf", 'location="path(@header_dir/whole.dat)"')
    r, got = converted(misnamed)
    check(r.returncode == 1 and got is None and "this header file does not have the name of one: only a header file (.xish) is followed" in r.stderr,
          f"a header file under another name is not followed either: {r.stderr.strip()[:300]}")
    r, got = converted(misnamed, "--external-files", "anywhere")
    check(got is not None and np.array_equal(got, as_planes(a)), "... unless a header may lead anywhere")
    r = run("--info", misnamed)
    check("distributed unit" in r.stdout and "in 1 file," in r.stdout and "not read: the header is not followed there" in r.stdout,
          f"--info says what it is and what is not read: {r.stdout[:300]}")
    # what a file declares for its properties is held against the bytes it brings, not against a large
    # file its header happens to name
    big = os.path.join(policy, "big.bin")
    with open(big, "wb") as f:
        f.truncate(64 << 20)        # (with the 256 MiB every file is allowed, enough for the 300 MiB that are declared)
    bomb = 'location="inline:base64" compression="zlib:314572800">%s</Property><Property id="P:None" type="Int32" value="1"' % base64.b64encode(zlib.compress(bytes(64))).decode()
    said = "declare more data than a file of its size can hold"
    for name, extra in (("budget.xish", ""), ("budget-named.xish", '<Extra location="path(@header_dir/big.bin)"/>')):
        path = unit_of(name, 'location="path(@header_dir/whole.dat)"', bomb)
        text = open(path).read().replace("</xisf>", extra + "</xisf>")
        open(path, "w").write(text)
        r, got = converted(path)
        check(said in r.stderr, f"a property that declares 300 MiB in a header of a few hundred bytes ({name}): {r.stderr.strip()[:300]}")
    os.remove(big)
    # the pixels of an image that are a whole file of another size
    open(os.path.join(policy, "short.dat"), "wb").write(pixels[:-2])
    r, got = converted(unit_of("short.xish", 'location="path(@header_dir/short.dat)"'))
    check(r.returncode == 1 and "geometry requires 96" in r.stderr, f"a file that is not the size of the image: {r.stderr.strip()[:200]}")
    # a header whose root element has a namespace prefix is a header
    prefixed = unit_of("prefixed.xish", 'location="path(@header_dir/whole.dat)"')
    text = open(prefixed).read().replace('<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">', '<x:xisf version="1.0" xmlns:x="http://www.pixinsight.com/xisf">')
    open(prefixed, "w").write(text.replace("<Image", "<x:Image").replace("</Image>", "</x:Image>").replace("<Property", "<x:Property")
                              .replace("</Property>", "</x:Property>").replace("</xisf>", "</x:xisf>"))
    r, got = converted(prefixed)
    check(got is not None and np.array_equal(got, as_planes(a)), f"a header with a namespace prefix: {r.stderr.strip()[:200]}")
    # what is no header file
    for name, content, message in (("blocks.xisb", b"XISB0100" + bytes(24), "XISF data blocks file (.xisb)"),
                                   ("picture.xish", b'<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg"/>', "not an XISF header"),
                                   ("broken.xish", b'<?xml version="1.0"?>\n<xisf version="1.0"><Image', "XML"),
                                   ("empty.xish", b"", "too short")):
        path = os.path.join(policy, name)
        open(path, "wb").write(content)
        r, got = converted(path)
        check(r.returncode == 1 and message in r.stderr and got is None, f"{name} is no header file: {r.stderr.strip()[:200]}")

    # ---- data blocks files that are damaged, or not what the header says
    broken = os.path.join(d, "broken")
    os.makedirs(broken, exist_ok=True)
    simple = ('<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">'
              '<Image geometry="8:6:1" sampleFormat="UInt16" colorSpace="Gray" {0}>'
              '<Property id="P:Vector" type="F64Vector" length="400" {1}/></Image></xisf>')

    def broken_unit(name, change=None, header=None, **layout):
        path = os.path.join(broken, name + ".xish")
        write_unit(path, simple, [Block(pixels), Block(vector, codec="zlib", checksum="sha1")], **layout)
        data = os.path.join(broken, name + ".xisb")
        if change:
            raw = bytearray(open(data, "rb").read())
            raw = change(raw)
            open(data, "wb").write(bytes(raw))
        if header:
            text = header(open(path, "rb").read())
            open(path, "wb").write(text)
        out = os.path.join(broken, "out.fits")
        if os.path.exists(out):
            os.remove(out)
        r = run(path, "-o", out, expect_ok=False)
        return r, os.path.exists(out), path

    r, written, good = broken_unit("good")
    check(r.returncode == 0 and written, "the unit these are made of is read")

    def poke(position, fmt, *values):
        def change(raw):
            struct.pack_into(fmt, raw, position, *values)
            return raw
        return change

    first = 16 + 16      # the first element of the index
    damage = [
        ("truncated in the pixels", lambda raw: raw[:16 + 16 + 80 + 40], {}, "lies beyond the end of the file"),
        ("truncated in the index", lambda raw: raw[:40], {}, "more than the file has room for"),
        ("without its signature", lambda raw: b"XISF0100" + raw[8:], {}, "is not an XISF data blocks file"),
        ("of a few bytes", lambda raw: raw[:10], {}, "too short to be an XISF data blocks file"),
        ("with an index that runs in a circle", poke(16 + 8, "<Q", 16), {}, "runs in a circle"),
        ("with a node beyond the file", poke(16 + 8, "<Q", 1 << 40), {}, "beyond the end of the file"),
        ("with a node of more elements than fit", poke(16, "<I", 0xFFFFFFFF), {}, "more than the file has room for"),
        ("whose block lies beyond it", poke(first + 8, "<Q", 1 << 50), {}, "lies beyond the end of the file"),
        ("whose block has no end", poke(first + 16, "<Q", 0xFFFFFFFFFFFFFFFF), {}, "lies beyond the end of the file"),
        ("whose block is a free element", poke(first + 8, "<QQ", 0, 0), {}, "is a free index element"),
        ("that has another block under that identifier", poke(first, "<Q", 12345), {}, "has no block 0x4d373e33756e480f (2 in its index)"),
    ]
    for what, change, layout, message in damage:
        r, written, _ = broken_unit("damaged", change, **layout)
        check(r.returncode == 1 and message in r.stderr and not written and not leftovers(broken),
              f"a data blocks file {what}: {r.stderr.strip()[:240]}")
    os.remove(os.path.join(broken, "damaged.xisb"))
    out = os.path.join(broken, "out.fits")
    r = run(os.path.join(broken, "damaged.xish"), "-o", out, expect_ok=False)
    check(r.returncode == 1 and "is not there" in r.stderr and "damaged.xisb" in r.stderr and not os.path.exists(out),
          f"a unit without its data blocks file: {r.stderr.strip()[:240]}")
    r = run("--verify", os.path.join(broken, "damaged.xish"), expect_ok=False)
    check(r.returncode == 1 and "FAILED" in r.stdout and "is not there" in r.stdout, "--verify of such a unit fails")
    r = run("--info", os.path.join(broken, "damaged.xish"))
    check("in 1 file," in r.stdout and "damaged.xisb  (not there" in r.stdout, f"--info counts the files that are there: {r.stdout[:300]}")
    # an index whose nodes lie in each other: a megabyte of them would name a gigabyte of blocks
    def overlapping(raw):
        end = len(raw)
        struct.pack_into("<Q", raw, 16 + 8, end)                       # the first node leads to one at the end,
        raw += struct.pack("<IIQ", 1, 0, 0) + bytes(40)                # which is complete,
        struct.pack_into("<Q", raw, end + 8, 16 + 16 + 32)              # and leads to one that lies in the first
        return raw
    r, written, _ = broken_unit("overlap", overlapping)
    check(r.returncode == 1 and "lies in another of its nodes" in r.stderr and not written, f"an index whose nodes overlap: {r.stderr.strip()[:240]}")
    # two blocks under one identifier: the first is the block, and that is said
    r, written, path = broken_unit("twice", poke(first + 40, "<Q", 0x4d373e33756e480f), header=lambda h: h.replace(b"0x4d373e33756e4be0", b"0x4d373e33756e480f"))
    check("has the identifier of an earlier one" in r.stderr, f"two blocks under one identifier are named: {r.stderr.strip()[:200]}")
    # the index and the header disagree about a block
    r, written, path = broken_unit("lengths", poke(first + 40 + 24, "<Q", 1234))
    v = run("--verify", path, expect_ok=False)
    check(r.returncode == 0 and written and v.returncode == 1 and "has an uncompressed length of 1234, the header one of 3200" in v.stdout,
          f"an uncompressed length in the index that is not the header's: read as the header says, and --verify names it: {v.stdout.strip()[:260]}")
    r, written, path = broken_unit("claimed", poke(first + 24, "<Q", 96))
    check(r.returncode == 0 and written and "calls the block compressed, the header does not" in r.stderr, "... also for a block that is not compressed")
    # a damaged block is found by its checksum, in the other file as in this one
    r, written, path = broken_unit("flipped", lambda raw: raw[:-5] + bytes([raw[-5] ^ 0x40]) + raw[-4:])
    v = run("--verify", path, expect_ok=False)
    check("checksum mismatch" in r.stderr and v.returncode == 1 and "checksum mismatch on property P:Vector" in v.stdout,
          f"a damaged block in the data blocks file: {v.stdout.strip()[:200]}")
    # reserved fields that are not zero are read past, and named
    r, written, path = broken_unit("reserved", lambda raw: raw[:8] + b"\1" + raw[9:20] + b"\1" + raw[21:])
    check(r.returncode == 0 and written and "reserved field of the file is not zero" in r.stderr and "reserved field of the index node" in r.stderr,
          f"reserved fields that are not zero: {r.stderr.strip()[:200]}")

    # ---- XISF to XISF: one kind of unit to the other, and each to itself
    rewrite = os.path.join(d, "rewrite")
    os.makedirs(rewrite, exist_ok=True)
    mono = os.path.join(rewrite, "rich.xisf")
    expected = rich_xisf(mono, dict(codec="zlib", shuffle_item=2, checksum="sha1"), align=16)
    original = xisf_blocks(mono)
    r = run(mono, "-t", "xish")
    unit = os.path.join(rewrite, "rich.xish")
    blocks = unit_structure("a monolithic file, unpacked", unit, rewritten=True)
    check([b["stored"] for b in blocks] == [b["stored"] for b in original] and [b["attr"] for b in blocks] == [b["attr"] for b in original] and
          header_without_storage(xisf_header(unit)[1]) == header_without_storage(xisf_header(mono)[1]) and "kept as stored" in r.stdout,
          f"-t xish: the blocks as they were stored, the header the same text: {r.stdout.strip()}")
    # stored another way on the way
    for options, attribute in ((["--codec", "none", "--checksum", "none"], lambda attr: attr["compression"] is None and attr["checksum"] is None),
                               (["--codec", "lz4hc", "--checksum", "sha256"], lambda attr: (attr["compression"] or "lz4hc").startswith("lz4hc") and attr["checksum"].startswith("sha256:"))):
        out = os.path.join(rewrite, "other.xish")
        r = run(mono, "-o", out, "-f", *options)
        blocks = unit_structure(f"unpacked with {' '.join(options)}", out, rewritten=True)
        check([b["data"] for b in blocks] == [b["data"] for b in original] and all(attribute(b["attr"]) for b in blocks if b["kind"] == "path"),
              f"unpacked with {' '.join(options)}: the same data, stored as asked: {r.stdout.strip()}")
        back = os.path.join(rewrite, "other.xisf")
        r = run(out, "-o", back, "-f")
        mine = xisf_blocks(back)
        check([b["stored"] for b in mine] == [b["stored"] for b in blocks] and all(b["kind"] != "path" for b in mine),
              f"... and packed again: {r.stdout.strip()}")
    # one image of several
    out = os.path.join(rewrite, "one.xish")
    r = run(mono, "-o", out, "-f", "-i", "2")
    blocks = unit_structure("one image of a file, unpacked", out, rewritten=True)
    check(len(blocks) == 1 and blocks[0]["data"] == as_planes(expected["small"]).tobytes(), "--image with -t xish: that image and its block alone")
    # in place: the unit is replaced by itself, in its own two files
    work = os.path.join(rewrite, "work")
    os.makedirs(work, exist_ok=True)
    unit = os.path.join(work, "frame.xish")
    run(mono, "-o", unit, "--codec", "none", "--checksum", "none")
    plain = {n: open(os.path.join(work, n), "rb").read() for n in os.listdir(work)}
    r = run(unit, "--in-place", "--codec", "zlib", "--checksum", "sha1")
    blocks = unit_structure("a unit rewritten in place", unit, rewritten=True)
    check(sorted(os.listdir(work)) == ["frame.xisb", "frame.xish"] and [b["data"] for b in blocks] == [b["data"] for b in original] and
          len(open(os.path.join(work, "frame.xisb"), "rb").read()) < len(plain["frame.xisb"]) and "read back and compared" in r.stdout,
          f"--in-place on a header file replaces the header and its data blocks file: {r.stdout.strip()}")
    now = {n: open(os.path.join(work, n), "rb").read() for n in os.listdir(work)}
    r = run(unit, "--in-place", "--codec", "zlib", "--checksum", "sha1")
    check("left unchanged" in r.stdout and now == {n: open(os.path.join(work, n), "rb").read() for n in os.listdir(work)},
          "... and leaves a unit alone that is stored as asked")
    r = run(unit, "-t", "xish", expect_ok=False)
    check(r.returncode == 1 and "--in-place" in r.stderr and now == {n: open(os.path.join(work, n), "rb").read() for n in os.listdir(work)},
          f"a unit is not written over itself without --in-place: {r.stderr.strip()[:160]}")
    # the data of the input is not written over: another header that would use its data blocks file
    r = run(unit, "-o", os.path.join(work, "frame.xisb"), "-t", "xisf", "-f", expect_ok=False)
    check(r.returncode == 1 and "a file the input reads its data from" in r.stderr and now["frame.xisb"] == open(os.path.join(work, "frame.xisb"), "rb").read(),
          f"an output that is the data blocks file of the input: {r.stderr.strip()[:200]}")
    shutil.copy(unit, os.path.join(work, "other.xish"))
    os.rename(os.path.join(work, "frame.xish"), os.path.join(work, "moved.xish"))
    r = run(os.path.join(work, "moved.xish"), "-o", os.path.join(work, "frame.xish"), "-f", expect_ok=False)
    check(r.returncode == 1 and "is a file the input reads its data from" in r.stderr and now["frame.xisb"] == open(os.path.join(work, "frame.xisb"), "rb").read()
          and not os.path.exists(os.path.join(work, "frame.xish")) and not leftovers(work),
          f"an output whose data blocks file is the one the input reads: {r.stderr.strip()[:200]}")
    # a header whose blocks are in other files than the one of its name, and in a file that is one block:
    # in place, they all come into the data blocks file of the header's name, and the others stay
    r = run(os.path.join(work, "moved.xish"), "--in-place", "--codec", "none", "--checksum", "none")
    blocks = unit_structure("a unit whose data was in a file of another name, in place", os.path.join(work, "moved.xish"), rewritten=True)
    check(sorted(os.listdir(work)) == ["frame.xisb", "moved.xisb", "moved.xish", "other.xish"] and now["frame.xisb"] == open(os.path.join(work, "frame.xisb"), "rb").read()
          and [b["data"] for b in blocks] == [b["data"] for b in original], f"in place, the blocks come into the file of the header's name: {sorted(os.listdir(work))}")
    r = run("--verify", os.path.join(work, "other.xish"))
    check(": OK" in r.stdout, "... and the file they were in is still what another header reads")
    # a data blocks file that holds more than this header names (another header's blocks) is not replaced in place
    shared = os.path.join(rewrite, "shared")
    os.makedirs(shared, exist_ok=True)
    one = test_image(np.uint16, 5, 6, 1, 71)
    two = test_image(np.uint16, 5, 6, 1, 72)
    shape = ('<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">'
             '<Image geometry="6:5:1" sampleFormat="UInt16" colorSpace="Gray" location="path(@header_dir/a.xisb):%d"/></xisf>')
    write_xisb(os.path.join(shared, "a.xisb"), [(as_planes(one).astype("<u2").tobytes(), 0), (as_planes(two).astype("<u2").tobytes(), 0)], [11, 22])
    open(os.path.join(shared, "a.xish"), "w").write(shape % 11)
    open(os.path.join(shared, "b.xish"), "w").write(shape % 22)
    kept = open(os.path.join(shared, "a.xisb"), "rb").read()
    r = run(os.path.join(shared, "a.xish"), "--in-place", "--checksum", "sha1", expect_ok=False)
    check(r.returncode == 1 and "also holds 1 block that this header does not name" in r.stderr and
          kept == open(os.path.join(shared, "a.xisb"), "rb").read() and not leftovers(shared),
          f"in place, a data blocks file with another header's block is not replaced: {r.stderr.strip()[:300]}")
    r = run("--verify", os.path.join(shared, "b.xish"))
    check(": OK" in r.stdout, "... and the other header still has its image")
    r = run(os.path.join(shared, "a.xish"), "-o", os.path.join(shared, "a-own.xish"), "--codec", "zlib")
    check(r.returncode == 0 and ": OK" in run("--verify", os.path.join(shared, "a-own.xish")).stdout and
          kept == open(os.path.join(shared, "a.xisb"), "rb").read(), "under another name the unit gets a data blocks file of its own")
    r = run(os.path.join(shared, "a.xish"), "--in-place", "--checksum", "sha1", "--force")
    check(r.returncode == 0 and len(read_xisb(os.path.join(shared, "a.xisb"))["elements"]) == 1, "--force replaces it all the same")
    # ... whatever way this header has of naming the file: as a whole, or by a block that is not read
    inline = base64.b64encode(as_planes(one).astype("<u2").tobytes()).decode()
    for name, body in (("whole", '<Image geometry="6:5:1" sampleFormat="UInt16" colorSpace="Gray" location="inline:base64">%s</Image>'
                                 '<Extra location="path(@header_dir/whole.xisb)"/>' % inline),
                       ("unread", '<Image geometry="6:5:1" sampleFormat="UInt16" colorSpace="Gray" location="inline:base64">%s</Image>'
                                  '<Image geometry="6:5:1" sampleFormat="UInt16" colorSpace="Gray" location="path(@header_dir/unread.xisb):153"/>' % inline)):
        write_xisb(os.path.join(shared, name + ".xisb"), [(as_planes(one).astype("<u2").tobytes(), 0), (as_planes(two).astype("<u2").tobytes(), 0)], [1, 2])
        open(os.path.join(shared, name + ".xish"), "w").write(
            '<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">%s</xisf>' % body)
        kept = open(os.path.join(shared, name + ".xisb"), "rb").read()
        r = run(os.path.join(shared, name + ".xish"), "--in-place", "--checksum", "sha1", "-i", "0", expect_ok=False)
        check(r.returncode == 1 and kept == open(os.path.join(shared, name + ".xisb"), "rb").read() and not leftovers(shared) and
              ("does not name" in r.stderr if name == "whole" else "holds none of the blocks this header names" in r.stderr),
              f"in place, a data blocks file of others' blocks that this header names {name} is not replaced: {r.stderr.strip()[:300]}")
    r = run(os.path.join(shared, "unread.xish"), "--in-place", "--checksum", "sha1", "-i", "0", "--force", expect_ok=False)
    check(r.returncode == 1 and "holds none of the blocks this header names" in r.stderr and
          kept == open(os.path.join(shared, "unread.xisb"), "rb").read(), "a file that has none of the header's blocks is not replaced with --force either")
    # ... nor a file the header is not followed to, or one whose index cannot be read: nothing is left to the
    # reading, which --image may never come to
    two_images = ('<Image geometry="6:5:1" sampleFormat="UInt16" colorSpace="Gray" location="inline:base64">%s</Image>'
                  '<Image geometry="6:5:1" sampleFormat="UInt16" colorSpace="Gray" location="%%s"/>' % inline)
    wrap = '<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">%s</xisf>'
    blocks_of_two = [(as_planes(one).astype("<u2").tobytes(), 0), (as_planes(two).astype("<u2").tobytes(), 0)]
    slashed = os.path.abspath(shared).replace(os.sep, "/")
    for name, location, options, said in (
            ("refused", "path(@header_dir/refused.xisb):1", ["--external-files", "none"], "is not followed to it"),
            ("absolute", "path(%s/absolute.xisb):1" % slashed, [], "is not followed to it"),
            ("byurl", "url(file://%s%s/byurl.xisb):1" % ("" if slashed.startswith("/") else "/", slashed.replace(" ", "%20")), [], "is not followed to it"),
            ("noindex", "path(@header_dir/noindex.xisb):1", [], "cannot be read as one")):
        target = os.path.join(shared, name + ".xisb")
        if name == "noindex":
            open(target, "wb").write(b"not an index at all, " * 20)
        else:
            write_xisb(target, blocks_of_two, [1, 2])
        open(os.path.join(shared, name + ".xish"), "w").write(wrap % (two_images % location))
        kept = open(target, "rb").read()
        r = run(os.path.join(shared, name + ".xish"), "--in-place", "--checksum", "sha1", "-i", "0", *options, expect_ok=False)
        check(r.returncode == 1 and said in r.stderr and kept == open(target, "rb").read() and not leftovers(shared),
              f"in place with --image, a data blocks file that is {name} is not replaced unread: {r.stderr.strip()[:300]}")
    # a file that begins as a data blocks file and whose index cannot be followed, named as a whole
    open(os.path.join(shared, "circle.xisb"), "wb").write(b"XISB0100" + bytes(8) + struct.pack("<IIQ", 0, 0, 16) + bytes(64))
    open(os.path.join(shared, "circle.xish"), "w").write(wrap % (two_images % "path(@header_dir/circle.xisb)"))
    kept = open(os.path.join(shared, "circle.xisb"), "rb").read()
    r = run(os.path.join(shared, "circle.xish"), "--in-place", "--checksum", "sha1", "-i", "0", expect_ok=False)
    check(r.returncode == 1 and "cannot be read as one" in r.stderr and kept == open(os.path.join(shared, "circle.xisb"), "rb").read(),
          f"... nor one that is named as a whole and is a data blocks file nobody can read: {r.stderr.strip()[:300]}")
    # the blocks a header names are its own whichever way it writes the file, in whatever order
    for name, first_location, second_location, image in (
            ("mixed1", "path(%s/mixed1.xisb):1" % slashed, "path(@header_dir/mixed1.xisb):2", "1"),
            ("mixed2", "path(@header_dir/nothing here/../mixed2.xisb):1", "path(@header_dir/mixed2.xisb):2", "1")):
        write_xisb(os.path.join(shared, name + ".xisb"), blocks_of_two, [1, 2])
        both = ('<Image geometry="6:5:1" sampleFormat="UInt16" colorSpace="Gray" location="%s"/>'
                '<Image geometry="6:5:1" sampleFormat="UInt16" colorSpace="Gray" location="%s"/>' % (first_location, second_location))
        open(os.path.join(shared, name + ".xish"), "w").write(wrap % both)
        r = run("--info", os.path.join(shared, name + ".xish"))
        # (a Windows program run elsewhere takes /tmp/... and Z:\tmp\... for two files to show, which they are not to open)
        hybrid = os.sep == "/" and "\\" in r.stdout.split("data in:")[1].splitlines()[0]
        check("in 2 files" in r.stdout and (hybrid or ("not read" not in r.stdout and "not there" not in r.stdout)),
              f"a file that is written two ways, of which the second is followed, is read ({name}): {r.stdout[:200]}")
        r = run(os.path.join(shared, name + ".xish"), "--in-place", "--checksum", "sha1", "-i", image, expect_ok=False)
        check(r.returncode == 0 and len(read_xisb(os.path.join(shared, name + ".xisb"))["elements"]) == 1,
              f"... and replaced in place: all of its blocks are this header's ({name}): {r.stderr.strip()[:300]}")
    # a path that goes through a link and back is the file it leads to, not the one its words spell
    try:
        os.makedirs(os.path.join(shared, "a"), exist_ok=True)
        os.makedirs(os.path.join(shared, "x"), exist_ok=True)
        if not os.path.lexists(os.path.join(shared, "a", "L")):
            os.symlink(os.path.join("..", "x"), os.path.join(shared, "a", "L"))
        if not os.path.islink(os.path.join(shared, "a", "L")):
            raise OSError("no link was made")       # (Wine says yes and makes none)
    except (OSError, NotImplementedError, AttributeError):
        skipped.append("symbolic links (a path through a link and back)")
    else:
        write_xisb(os.path.join(shared, "back.xisb"), blocks_of_two, [1, 2])
        open(os.path.join(shared, "back.xish"), "w").write(wrap % (two_images % "path(@header_dir/a/L/../back.xisb):1"))
        kept = open(os.path.join(shared, "back.xisb"), "rb").read()
        if os.name != "posix":
            # (Windows takes ".." out of a path by its words before it follows any link: there the path spells another file)
            skipped.append("a path through a link and back (on Windows, \"..\" is taken by the words of the path)")
        elif "\\" in run("--info", os.path.join(shared, "back.xish")).stdout.split("data in:")[1].splitlines()[0]:
            skipped.append("a path through a link and back (the program is not of this system)")
        else:
            for options in (["--external-files", "none"], ["--external-files", "none", "--force"]):
                r = run(os.path.join(shared, "back.xish"), "--in-place", "--checksum", "sha1", "-i", "0", *options, expect_ok=False)
                check(r.returncode == 1 and "is not followed to it" in r.stderr and kept == open(os.path.join(shared, "back.xisb"), "rb").read(),
                      f"in place, a file named through a link and back is known for the file it is ({' '.join(options)}): {r.stderr.strip()[:300]}")
            shutil.copy(os.path.join(shared, "back.xish"), os.path.join(shared, "front.xish"))
            r = run(os.path.join(shared, "front.xish"), "-o", os.path.join(shared, "back.xish"), "-i", "0", "--force", "--external-files", "none", expect_ok=False)
            check(r.returncode == 1 and "is a file the input reads its data from" in r.stderr and kept == open(os.path.join(shared, "back.xisb"), "rb").read(),
                  f"... and is no output of a rewrite of the unit that names it so: {r.stderr.strip()[:300]}")
        # in place through a link to the header from another directory: the data is beside the link
        os.makedirs(os.path.join(shared, "linked"), exist_ok=True)
        if not os.path.lexists(os.path.join(shared, "linked", "l.xish")):
            os.symlink(os.path.join("..", "x", "real.xish"), os.path.join(shared, "linked", "l.xish"))
        run(mono, "-o", os.path.join(shared, "x", "real.xish"), "-f", "-q")
        shutil.copy(os.path.join(shared, "x", "real.xisb"), os.path.join(shared, "linked", "real.xisb"))
        r = run(os.path.join(shared, "linked", "l.xish"), "--in-place", "--checksum", "sha256", expect_ok=False)
        if os.sep == "/" and "\\" in run("--info", os.path.join(shared, "back.xish")).stdout.split("data in:")[1].splitlines()[0]:
            skipped.append("in place through a link to a header (the program is not of this system)")
        else:
            check(r.returncode == 1 and "link from another directory" in r.stderr and
                  ": OK" in run("--verify", os.path.join(shared, "x", "real.xish")).stdout,
                  f"in place through a link to the header from another directory is refused: {r.stderr.strip()[:300]}")
    # the files a unit reads are no outputs and no temporary files, of a rewrite or of a conversion
    alias = os.path.join(rewrite, "alias")
    os.makedirs(alias, exist_ok=True)
    unit = os.path.join(alias, "in.xish")
    run(mono, "-o", unit, "-q")
    data = open(os.path.join(alias, "in.xisb"), "rb").read()
    r = run(unit, "-o", os.path.join(alias, "in.xisb"), "-t", "fits", "-f", expect_ok=False)
    check(r.returncode == 1 and "is a file the input reads its data from" in r.stderr and data == open(os.path.join(alias, "in.xisb"), "rb").read(),
          f"a conversion does not write over the data of its input: {r.stderr.strip()[:200]}")
    r = run(unit, "-o", os.path.join(alias, "in.xisb"), "-t", "xisf", expect_ok=False)
    check(r.returncode == 1 and "is a file the input reads its data from" in r.stderr, "that is said with or without --force")
    try:
        os.link(os.path.join(alias, "in.xisb"), os.path.join(alias, "out.xisb.part"))
        os.link(os.path.join(alias, "in.xisb"), os.path.join(alias, "out.fits.part"))
    except (OSError, NotImplementedError, AttributeError):
        skipped.append("hard links (a temporary file that is the data of the input)")
    else:
        for args in (["-o", os.path.join(alias, "out.xish")], ["-o", os.path.join(alias, "out.fits")]):
            r = run(unit, *args, "-f", expect_ok=False)
            check(r.returncode == 1 and "is a file the input reads its data from" in r.stderr and
                  data == open(os.path.join(alias, "in.xisb"), "rb").read() and not os.path.exists(args[1]),
                  f"a temporary file that is the data of the input is not written to ({os.path.basename(args[1])}): {r.stderr.strip()[:200]}")
        os.remove(os.path.join(alias, "out.xisb.part"))
        os.remove(os.path.join(alias, "out.fits.part"))
    # two files are not replaced in one step: if one of them cannot get its name, the unit that was there stays
    def immutable(path, on):
        try:
            return subprocess.run(["chattr", "+i" if on else "-i", path], capture_output=True).returncode == 0
        except OSError:
            return False
    locked = os.path.join(alias, "locked.xish")
    run(mono, "-o", locked, "-q")
    was = {n: open(os.path.join(alias, n), "rb").read() for n in ("locked.xish", "locked.xisb")}
    if immutable(locked, True) and immutable(locked, False):
        for part in ("locked.xish", "locked.xisb"):
            for args, said in (([src, "-o", locked, "-f"], "the unit that was there is as it was"),
                               ([mono, "-o", locked, "-f", "--checksum", "sha256"], "the unit that was there is as it was"),
                               ([locked, "--in-place", "--checksum", "sha256"], "as it was")):
                immutable(os.path.join(alias, part), True)
                try:
                    r = run(*args, expect_ok=False)
                finally:
                    immutable(os.path.join(alias, part), False)
                now = {n: open(os.path.join(alias, n), "rb").read() for n in was}
                litter = sorted(n for n in os.listdir(alias) if n.startswith("locked.") and n not in was)
                check(r.returncode == 1 and said in r.stderr and now == was and not litter,
                      f"{part} cannot be replaced ({' '.join(os.path.basename(a) for a in args)}): the unit stays, nothing is left: "
                      f"{r.stderr.strip()[:200]} {litter}")
        r = run(locked, "--in-place", "--checksum", "sha256")
        check(r.returncode == 0 and ": OK" in run("--verify", locked).stdout and
              sorted(n for n in os.listdir(alias) if n.startswith("locked.")) == ["locked.xisb", "locked.xish"], "and is replaced when it can be")
    else:
        skipped.append("a unit of which one file cannot be replaced, by a file that cannot be changed (needs chattr +i, as root on ext4 and the like)")
    # The same with renames that fail for another reason (an I/O error): each step of the replacement, and
    # the putting back. What was there before is there after, or the message says under which name it is.
    shim = os.path.join(alias, "rename_shim.so")
    source = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rename_shim.c")
    built = False
    if sys.platform.startswith("linux") and shutil.which("cc") and os.path.exists(source):
        built = subprocess.run(["cc", "-shared", "-fPIC", "-o", shim, source, "-ldl"], capture_output=True).returncode == 0
        if built:   # (a program that is linked statically takes no such library: then nothing fails)
            probe = os.path.join(alias, "probe.xisf")
            r = subprocess.run([EXE, mono, "-o", probe, "-q"], capture_output=True, text=True,
                               env=dict(os.environ, LD_PRELOAD=shim, XISFCONV_TEST_FAIL_RENAME=".xisf.part>.xisf"))
            built = r.returncode == 1 and "cannot rename" in r.stderr and not os.path.exists(probe)
            for name in ("probe.xisf", "probe.xisf.part"):
                if os.path.exists(os.path.join(alias, name)):
                    os.remove(os.path.join(alias, name))
    if built:
        def failing(rules, *args):
            return subprocess.run([EXE, *args], capture_output=True, text=True,
                                  env=dict(os.environ, LD_PRELOAD=shim, XISFCONV_TEST_FAIL_RENAME=rules))
        def files(prefix):
            return {n: open(os.path.join(alias, n), "rb").read() for n in sorted(os.listdir(alias)) if n.startswith(prefix)}
        steps = {"setting the old data aside": ".xisb>.replaced", "the new data": ".xisb.part>.xisb", "the new header": ".xish.part>.xish"}
        commands = {"a conversion with --force": [src, "-o", locked, "-f"], "a rewrite with --force": [mono, "-o", locked, "-f", "--checksum", "sha512"],
                    "in place": [locked, "--in-place", "--checksum", "sha512"]}
        run(mono, "-o", locked, "-f", "-q")
        was = files("locked.")
        for step, rule in steps.items():
            for what, args in commands.items():
                r = failing(rule, *args)
                check(r.returncode == 1 and "as it was" in r.stderr and files("locked.") == was and ": OK" in run("--verify", locked).stdout,
                      f"{what}, and {step} fails: the unit that was there is as it was, and nothing else is left: "
                      f"{r.stderr.strip()[-160:]} {sorted(files('locked.'))}")
            # ... and if the old data cannot be put back either, the message says where it is
            if step != "setting the old data aside":
                for what, args in commands.items():
                    r = failing(rule + ",.replaced>.xisb", *args)
                    now = files("locked.")
                    check(r.returncode == 1 and "is kept as" in r.stderr and "locked.xisb.replaced" in r.stderr and "as it was" not in r.stderr.split("is kept as")[0]
                          and now.get("locked.xisb.replaced") == was["locked.xisb"] and now["locked.xish"] == was["locked.xish"],
                          f"{what}, {step} fails and the old data cannot be put back: it is kept, and the message says where: {r.stderr.strip()[-200:]}")
                    v = run("--verify", locked, expect_ok=False)
                    check(v.returncode == 1 and "locked.xisb.replaced has that block" in v.stdout and "renamed to" in v.stdout,
                          f"... and reading the unit says what that file is: {v.stdout.strip()[-260:]}")
                    if os.path.exists(os.path.join(alias, "locked.xisb.replaced")):
                        os.replace(os.path.join(alias, "locked.xisb.replaced"), os.path.join(alias, "locked.xisb"))
                    check(files("locked.") == was, "... renamed back, the unit is as it was")
        # a new unit: nothing is left of it; a file of the header's name that was there stays
        fresh = os.path.join(alias, "fresh.xish")
        for step, rule in list(steps.items())[1:]:
            r = failing(rule, src, "-o", fresh)
            check(r.returncode == 1 and "the unit is not written" in r.stderr and not files("fresh."), f"a new unit, and {step} fails: nothing is left: {r.stderr.strip()[-160:]}")
        open(fresh, "wb").write(b"a file that was there")
        r = failing(".xish.part>.xish", src, "-o", fresh, "-f")
        check(r.returncode == 1 and files("fresh.") == {"fresh.xish": b"a file that was there"}, f"a file that was there under the name of the header stays: {sorted(files('fresh.'))}")
        os.remove(fresh)
        # one file: the file that is there stays until the new one has its name
        single_out = os.path.join(alias, "kept.xisf")
        shutil.copy(mono, single_out)
        kept = open(single_out, "rb").read()
        r = failing(".xisf.part>.xisf", src, "-o", single_out, "-f")
        check(r.returncode == 1 and files("kept.") == {"kept.xisf": kept}, f"a monolithic output whose rename fails leaves the file that was there: {sorted(files('kept.'))}")
    else:
        skipped.append("renames that fail while a unit is replaced (Linux, a C compiler, and a program that loads tests/rename_shim.c)")
    run(mono, "-o", locked, "-f", "-q")
    r = run(locked, "--in-place", "--checksum", "sha256", "--external-files", "none", expect_ok=False)
    check(r.returncode == 1 and "is not followed to it: a file that is not read is not replaced" in r.stderr and "does not name" not in r.stderr and
          "already exists" not in r.stderr and ": OK" in run("--verify", locked).stdout,
          f"--in-place on a unit whose data may not be read says that, and replaces nothing: {r.stderr.strip()[:200]}")
    # -t says the kind of unit in so many words: a name that says the other one is an error, either way
    r = run(mono, "-t", "xisf", "-o", os.path.join(alias, "said.xish"), expect_ok=False)
    check(r.returncode == 2 and "-t xisf writes one monolithic file" in r.stderr and not os.path.exists(os.path.join(alias, "said.xish")),
          f"-t xisf with the name of a header file: {r.stderr.strip()[:200]}")
    r = run(locked, "--in-place", "-t", "xisf", "--checksum", "sha512")
    check(r.returncode == 0 and os.path.exists(os.path.join(alias, "locked.xisb")) and "distributed unit" in run("--verify", locked).stdout,
          f"--in-place -t xisf on a header file is XISF to XISF as ever, and the unit stays the kind its name says: {r.stderr.strip()[:200]}")
    # the kind of a unit goes with its name: in place it stays what it is
    single = os.path.join(alias, "single.xisf")
    shutil.copy(mono, single)
    before = open(single, "rb").read()
    r = run(single, "--in-place", "-t", "xish", expect_ok=False)
    check(r.returncode == 1 and "--in-place keeps the kind of unit" in r.stderr and before == open(single, "rb").read() and
          not os.path.exists(os.path.join(alias, "single.xish")), f"--in-place -t xish on a monolithic file: {r.stderr.strip()[:200]}")
    # names a header has to write with care: every one is read back, and is XML for any reader
    import xml.etree.ElementTree as ET
    names = ["amp&er", "par(en)s", "a&amp;b", "semi;colon and blank", "caf\u00e9"]
    odd = os.path.join(rewrite, "names")
    os.makedirs(odd, exist_ok=True)
    # (what Windows has no file names for: asked of the program, which may be a Windows program run elsewhere)
    if os.name != "nt" and run(src, "-o", os.path.join(odd, 'pro"be.fits'), "-q", expect_ok=False).returncode == 0:
        names += ['quo"te', "lt<gt>", "tab\there", "back\\slash(", "new\nline"]
    for name in names:
        for source in (mono, src):
            out = os.path.join(odd, name + ".xish")
            r = run(source, "-o", out, "-f", "-q", expect_ok=False)
            v = run("--verify", out, expect_ok=False)
            try:
                located = [e.get("location") for e in ET.parse(out).getroot().iter() if (e.get("location") or "").startswith("path(")]
            except ET.ParseError as e:
                located = [str(e)]
            wanted = "path(@header_dir/%s.xisb):0x" % name.replace("(", "\\(").replace(")", "\\)")
            check(r.returncode == 0 and ": OK" in v.stdout and located and all(text.startswith(wanted) for text in located) and
                  os.path.exists(os.path.join(odd, name + ".xisb")),
                  f"a unit named {name!r} ({'rewritten' if source == mono else 'written'}): {r.stderr.strip()[:120]} {v.stdout.strip()[:160]} {located[:1]}")
    # a directory: header files are units, data blocks files are not
    r = run("--verify", work)
    check(r.stdout.count(": OK") == 2 and "frame.xisb" not in r.stdout and "moved.xisb" not in r.stdout, f"--verify of a directory: {r.stdout.strip()[:300]}")

    # ---- OpenXISF, if it is here: what it writes is read
    if OPENXISF:
        theirs = os.path.join(d, "openxisf")
        os.makedirs(theirs, exist_ok=True)
        unit = os.path.join(theirs, "theirs.xish")
        r = subprocess.run([os.path.join(OPENXISF, "distributed"), unit], capture_output=True, text=True)
        ramp = (np.arange(256 * 192) % 65536).astype(np.uint16).reshape(1, 192, 256)
        out = os.path.join(theirs, "theirs.fits")
        r2 = run(unit, "-o", out)
        compare("a distributed unit OpenXISF wrote", fits_planes(out)[0], ramp)
        v = run("--verify", unit)
        packed = os.path.join(theirs, "packed.xisf")
        run(unit, "-o", packed)
        again = os.path.join(theirs, "again.xish")
        run(packed, "-o", again)
        r3 = subprocess.run([os.path.join(OPENXISF, "read_pixels"), again], capture_output=True, text=True)
        check(r.returncode == 0 and ": OK" in v.stdout and r3.returncode == 0 and "mean of channel 0: %g" % ramp.mean() in r3.stdout,
              f"... verified, packed, unpacked, and read by OpenXISF again: {r3.stdout.strip()[:160]} {r3.stderr.strip()[:160]}")
    else:
        skipped.append("OpenXISF reads what xisfconv wrote and the reverse (set OPENXISF_BIN to the directory of its sample programs)")


def tool(*args, cwd=None):
    """Runs the program, in a directory of its own if one is given. Returns the finished process."""
    return subprocess.run([EXE, *args], cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")


def files_below(root):
    """Every file below a directory, as its path below it with "/" between the parts."""
    out = []
    for where, _, names in os.walk(root):
        out += [os.path.relpath(os.path.join(where, n), root).replace(os.sep, "/") for n in names]
    return sorted(out)


def as_one_file(path):
    """The bytes of a file xisfconv wrote, without the one thing that differs from run to run: the time an
    XISF file says it was made at."""
    import re
    return re.sub(rb'(id="XISF:CreationTime" type="TimePoint" value=")[^"]*', rb"\1", open(path, "rb").read())


KIND_OF_NAME = {".xisf": "xisf", ".xish": "xish", ".fits": "fits", ".fit": "fits", ".fts": "fits", ".fz": "fz", ".asdf": "asdf"}


def outputs_expected(files, target, compress=False):
    """What a conversion of a directory to `target` writes, written down without xisfconv: {input: output} for
    the files of the directory that are images by their names, are not hidden and are not yet what is made."""
    written = {"fits": "fz" if compress else "fits"}.get(target, target)
    ending = {"fits": ".fits.fz" if compress else ".fits", "tiff": ".tif"}.get(target, "." + target)
    out = {}
    for f in files:
        kind = KIND_OF_NAME.get(os.path.splitext(f)[1].lower())
        if kind is None or kind == written or any(part.startswith(".") for part in f.split("/")):
            continue
        stem = f[:-len(".fits.fz")] if f.lower().endswith(".fits.fz") else os.path.splitext(f)[0]
        out[f] = stem + ending
    return out


def pattern_matches(pattern, name):
    """The rule for patterns, written down a second time: * any characters, ? one, everything else itself, and
    a dot at the start of a name only for a dot at the start of the pattern."""
    import re
    if name.startswith(".") and not pattern.startswith("."):
        return False
    rx = "".join(".*" if c == "*" else "." if c == "?" else re.escape(c) for c in pattern)
    return re.fullmatch(rx, name, re.S | (re.I if os.name == "nt" else 0)) is not None


def test_directories_and_patterns():
    """Directories and patterns as inputs: a directory converts as its files do one by one, the files that are
    what is asked for already are passed over, the tree is kept below -d; a pattern stands for the names it
    matches, on every system; and a run writes no file twice and none over a file it reads."""
    import random
    import re
    base = os.path.join(TMP, "directories")
    os.makedirs(base)
    windows = os.name == "nt"
    rel = lambda p: p.replace("/", os.sep)   # noqa: E731

    # ---- a tree as a project has it: frames of every kind in nested directories, and what is none
    tree = os.path.join(base, "tree")
    for sub in ("night1/darks", "night2", ".cache"):
        os.makedirs(os.path.join(tree, rel(sub)))
    odd = "Sp ace [Ha] é.fts"            # a blank, brackets and a letter outside ASCII in a name
    seed = [200]

    def image(dtype, channels=1):
        seed[0] += 1
        return test_image(dtype, 20, 30, channels, seed[0])

    def xisf_file(path, dtype=np.uint16, channels=1):
        write_xisf(os.path.join(tree, rel(path)), [image_entry(image(dtype, channels))])

    def fits_file(path, dtype=np.uint16):
        fits.PrimaryHDU(np.squeeze(as_planes(image(dtype)))).writeto(os.path.join(tree, rel(path)))

    xisf_file("m31.xisf", np.float32, 3)
    xisf_file("night1/light_001.xisf")
    xisf_file("night1/light_002.xisf", np.uint8)
    fits_file("night1/darks/dark.fit")
    fits_file("night2/flat.fits", np.float32)
    fits_file("night2/" + odd, np.int16)
    fits.HDUList([fits.PrimaryHDU(), fits.CompImageHDU(np.squeeze(as_planes(image(np.uint16))), compression_type="RICE_1")]).writeto(
        os.path.join(tree, rel("night2/packed.fits.fz")))
    xisf_file("night2/seed_a.xisf", np.uint16, 3)
    xisf_file("night2/seed_u.xisf", np.float64)
    run(os.path.join(tree, rel("night2/seed_a.xisf")), "-o", os.path.join(tree, rel("night2/tree.asdf")), "-q")
    run(os.path.join(tree, rel("night2/seed_u.xisf")), "-o", os.path.join(tree, rel("night2/unit.xish")), "-q")
    os.remove(os.path.join(tree, rel("night2/seed_a.xisf")))
    os.remove(os.path.join(tree, rel("night2/seed_u.xisf")))
    xisf_file(".cache/hidden.xisf")
    open(os.path.join(tree, rel("night1/._light_001.xisf")), "wb").write(b"\x00\x05\x16\x07 what macOS leaves on other file systems")
    open(os.path.join(tree, "notes.txt"), "w").write("M 31, two nights\n")
    open(os.path.join(tree, rel("night1/table.csv")), "w").write("frame,fwhm\n1,2.3\n")
    before = files_below(tree)
    check(len(before) == 14 and "night2/unit.xisb" in before, f"the tree of the tests is there: {before}")

    def fresh(name):
        where = os.path.join(base, name)
        shutil.copytree(tree, where, copy_function=shutil.copy)   # (copy2 asks Windows for what Wine does not have)
        return where

    def lines_of(r):
        return [line for line in r.stdout.splitlines() if " -> " in line]

    def summary_of(r):
        m = re.search(r"^(\d+) files? (?:converted|done), (\d+) passed over, (\d+) failed$", r.stdout, re.M)
        return tuple(int(v) for v in m.groups()) if m else None

    # ---- a directory to each format: the files that are not that format yet, each as it converts alone
    for number, (target, flags) in enumerate([("fits", []), ("fits", ["-c"]), ("xisf", []), ("xish", []), ("asdf", []), ("tiff", []),
                                              ("png", ["-b", "u8"]), ("xisf", ["--codec", "zlib", "--checksum", "sha1"])]):
        label = f"a directory, -t {target} {' '.join(flags)}".rstrip()
        where = fresh(f"to{number}")
        expected = outputs_expected(before, target, compress="-c" in flags)
        r = tool("-t", target, *flags, where)
        after = files_below(where)
        new = sorted(set(after) - set(before))
        made = sorted(expected.values()) + sorted(v[:-1] + "b" for v in expected.values() if v.endswith(".xish"))
        passed = sum(1 for f in before if KIND_OF_NAME.get(os.path.splitext(f)[1].lower()) and f not in expected
                     and not any(part.startswith(".") for part in f.split("/")))
        check(r.returncode == 0 and new == sorted(made) and len(expected) >= 4,
              f"{label}: writes the {len(expected)} files expected and nothing else: {new} for {sorted(made)}; {r.stderr[-300:]}")
        told = [(a, b) for a, b in (line.split(": ")[0].split(" -> ") for line in lines_of(r))]
        wanted = [(os.path.join(where, rel(f)), os.path.join(where, rel(expected[f]))) for f in sorted(expected)]
        check(told == wanted, f"{label}: says so for each, in the order of the names: {told[:3]} ... for {wanted[:3]} ...")
        check(summary_of(r) == (len(expected), passed, 0) and r.stderr.count("passed over") == (1 if passed else 0),
              f"{label}: ends with the counts ({len(expected)} converted, {passed} passed over): {r.stdout.splitlines()[-1:]} "
              f"{[line for line in r.stderr.splitlines() if 'passed over' in line]}")
        check(all(open(os.path.join(where, rel(f)), "rb").read() == open(os.path.join(tree, rel(f)), "rb").read() for f in before),
              f"{label}: leaves every file that was there as it was")
        alone = os.path.join(base, f"alone{number}")
        os.makedirs(alone)
        different = []
        for k, f in enumerate(sorted(expected)):
            single = os.path.join(alone, f"{k}{os.path.splitext(expected[f])[1] if not expected[f].endswith('.fits.fz') else '.fits.fz'}")
            run(os.path.join(tree, rel(f)), "-t", target, *flags, "-o", single, "-q")
            got = os.path.join(where, rel(expected[f]))
            if target == "xish":      # the blocks of a unit get identifiers of their own each time: the images are compared
                run(got, "-o", single + ".a.fits", "-q")
                run(single, "-o", single + ".b.fits", "-q")
                got, single = single + ".a.fits", single + ".b.fits"
            if as_one_file(got) != as_one_file(single):
                different.append(f)
        check(not different, f"{label}: each output is what the file converts to when it is named alone: {different}")
        check(not [f for f in after if f.endswith(".part")], f"{label}: no temporary file is left")

    # ---- below -d the files keep their places, and the tree itself is not written to
    where = fresh("mirror")
    out = os.path.join(base, "mirror-out")
    os.makedirs(out)
    expected = outputs_expected(before, "fits")
    r = tool("-t", "fits", where + os.sep, "-d", out)      # (a directory may be given with its separator at the end)
    check(r.returncode == 0 and files_below(out) == sorted(expected.values()) and files_below(where) == before,
          f"-d: the outputs are below it at the places of their inputs, and nowhere else: {files_below(out)}; {r.stderr[-300:]}")
    r2 = tool("-t", "fits", where, "-d", out)
    check(r2.returncode == 1 and summary_of(r2) == (0, 3, len(expected)) and r2.stderr.count("already exists") == len(expected) and
          "--skip-existing" in r2.stdout.splitlines()[-1] and f"({len(expected)} outputs were there already" in r2.stdout,
          f"-d, once more: nothing is written over, and the last line says what to do about it: {r2.stdout.splitlines()[-2:]}")
    # the directory of the outputs, inside the directory that is read: it is not searched
    inside = os.path.join(where, "converted")
    os.makedirs(inside)
    r = tool("-t", "fits", where, "-d", inside, "-q")
    again = tool("-t", "png", "-b", "u8", where, "-d", inside)
    pictures = sorted(f for f in files_below(inside) if f.endswith(".png"))
    check(r.returncode == 0 and again.returncode == 0 and sorted(f for f in files_below(inside) if f.endswith(".fits")) == sorted(expected.values())
          and pictures == sorted(outputs_expected(before, "png").values()),
          f"-d inside the directory: what was written there is not taken for input the next time: {pictures}; {again.stderr[-300:]}")

    # ---- without -t: a directory of one format goes the way a file of it goes; one of both is not guessed at
    where = fresh("default")
    r = tool(where)
    check(r.returncode == 2 and files_below(where) == before and "4 XISF and 5 FITS or ASDF files" in r.stderr and "say with -t" in r.stderr
          and r.stdout == "", f"a directory of XISF and of FITS without -t: nothing is done, exit status 2: {r.stderr.strip()}")
    r = tool(os.path.join(where, "night1", "darks"))
    check(r.returncode == 0 and "night1/darks/dark.xisf" in files_below(where) and summary_of(r) == (1, 0, 0),
          f"a directory of FITS files without -t: to XISF: {r.stdout.strip()} {r.stderr[-200:]}")
    os.remove(os.path.join(where, "night1", "darks", "dark.xisf"))
    os.remove(os.path.join(where, "night1", "darks", "dark.fit"))
    r = tool(os.path.join(where, "night1"), "-c")
    check(r.returncode == 0 and sorted(set(files_below(where)) - set(before)) == ["night1/light_001.fits.fz", "night1/light_002.fits.fz"],
          f"a directory of XISF files without -t: to FITS (with -c tile-compressed): {sorted(set(files_below(where)) - set(before))}")
    empty = os.path.join(base, "empty")
    os.makedirs(os.path.join(empty, "sub"))
    open(os.path.join(empty, "readme.txt"), "w").write("nothing here\n")
    r = tool(empty)
    check(r.returncode == 0 and "no XISF, FITS or ASDF files found" in r.stderr and summary_of(r) == (0, 0, 0),
          f"a directory without images: a warning, and nothing to do: {r.stderr.strip()}")
    for flags, why in ((["-o", os.path.join(base, "one.fits")], "-o with a directory"),):
        r = tool(where, *flags)
        check(r.returncode == 2 and "-d" in r.stderr and not os.path.exists(os.path.join(base, "one.fits")), f"{why}: refused: {r.stderr.strip()}")
    for flags in (["--skip-existing", "--force"], ["--skip-existing", "--in-place"]):
        r = tool(where, *flags)
        check(r.returncode == 2 and "--skip-existing" in r.stderr, f"{' '.join(flags)}: refused: {r.stderr.strip()}")

    # ---- --skip-existing: what is there is left, what was added is converted
    where = fresh("skip")
    expected = outputs_expected(before, "fits")
    first = tool("-t", "fits", where, "--skip-existing")
    stamps = {f: (os.path.getmtime(os.path.join(where, rel(f))), open(os.path.join(where, rel(f)), "rb").read()) for f in files_below(where)}
    xisf_new = os.path.join(where, "night1", "light_003.xisf")
    write_xisf(xisf_new, [image_entry(test_image(np.uint16, 20, 30, 1, 299))])
    second = tool("-t", "fits", where, "--skip-existing")
    unchanged = all(stamps[f] == (os.path.getmtime(os.path.join(where, rel(f))), open(os.path.join(where, rel(f)), "rb").read()) for f in stamps)
    check(first.returncode == 0 and summary_of(first) == (len(expected), 3, 0) and second.returncode == 0 and
          summary_of(second) == (1, 3 + 2 * len(expected), 0) and lines_of(second) == [f"{xisf_new} -> {xisf_new[:-5]}.fits"] and unchanged and
          second.stdout.count("exists; passed over") == len(expected) and "--force" not in second.stdout,
          f"--skip-existing: the second run converts the one new file and touches nothing else: {second.stdout.splitlines()[-3:]}")
    quiet = tool("-t", "fits", where, "--skip-existing", "-q")
    check(quiet.returncode == 0 and quiet.stdout == "" and quiet.stderr == "", f"... and with -q says nothing: {quiet.stdout!r} {quiet.stderr!r}")

    # ---- --in-place: the XISF files of the directory, each where it is
    where = fresh("inplace")
    r = tool("--in-place", "--codec", "zlib", "--checksum", "sha256", where)
    changed = sorted(f for f in before if open(os.path.join(where, rel(f)), "rb").read() != open(os.path.join(tree, rel(f)), "rb").read())
    stored = [all((b["attr"]["checksum"] or "").replace("-", "").startswith("sha256:") for b in xisf_blocks(os.path.join(where, rel(f))))
              for f in ("m31.xisf", "night1/light_001.xisf", "night1/light_002.xisf", "night2/unit.xish")]
    check(r.returncode == 0 and changed == ["m31.xisf", "night1/light_001.xisf", "night1/light_002.xisf", "night2/unit.xisb", "night2/unit.xish"]
          and files_below(where) == before and summary_of(r) == (4, 5, 0) and all(stored),
          f"--in-place on a directory: its XISF files and units are rewritten, the rest is passed over: {changed} {r.stdout.splitlines()[-1:]} "
          f"{stored} {r.stderr[-300:]}")
    check(all(": OK" in tool("--verify", os.path.join(where, rel(f))).stdout for f in changed if not f.endswith(".xisb")),
          "... and each of them verifies")

    # ---- --info and --verify take a directory as the list of its files
    r = tool("--info", tree)
    heads = [line.split(": ")[0] for line in r.stdout.splitlines() if re.match(r".*: (XISF 1\.0|FITS|ASDF)", line)]
    check(r.returncode == 0 and heads == [os.path.join(tree, rel(f)) for f in sorted(outputs_expected(before, "png"))] and summary_of(r) is None,
          f"--info on a directory: every image file of it, hidden ones left out: {len(heads)} files")
    r = tool("--verify", tree)
    check(r.returncode == 1 and r.stdout.count(": OK") == 10 and "._light_001.xisf: FAILED" in r.stdout and "10 files OK, 1 failed" in r.stdout,
          f"--verify on a directory checks hidden files too, as it did: {r.stdout.splitlines()[-1:]}")

    # ---- no file is written twice in a run, and none over a file the run reads
    g = os.path.join(base, "guards")
    os.makedirs(os.path.join(g, "a"))
    os.makedirs(os.path.join(g, "b"))
    for sub, value in (("a", 11), ("b", 22)):
        fits.PrimaryHDU(np.full((6, 8), value, np.uint16)).writeto(os.path.join(g, sub, "frame.fits"))
    out = os.path.join(g, "out")
    os.makedirs(out)
    for flags in ([], ["--force"], ["--skip-existing"]):
        shutil.rmtree(out)
        os.makedirs(out)
        r = tool(os.path.join(g, "a", "frame.fits"), os.path.join(g, "b", "frame.fits"), "-t", "tiff", "-d", out, *flags)
        check(r.returncode == 1 and files_below(out) == ["frame.tif"] and int(tifffile.imread(os.path.join(out, "frame.tif"))[0, 0]) == 11 and
              "was written in this run already, from " + os.path.join(g, "a", "frame.fits") in r.stderr and len(lines_of(r)) == 1,
              f"two inputs with one output {flags}: the first is written, the second refused: {r.stderr.strip()[:200]}")
    r = tool(os.path.join(g, "a"), os.path.join(g, "b"), "-d", out, "-f")
    check(r.returncode == 1 and summary_of(r) == (1, 0, 1) and "was written in this run already" in r.stderr,
          f"two directories with a file of one name, into one directory: the same: {r.stdout.splitlines()[-1:]}")
    pair = os.path.join(g, "pair")
    os.makedirs(pair)
    fits.PrimaryHDU(np.full((6, 8), 5, np.uint16)).writeto(os.path.join(pair, "frame.fits"))
    write_xisf(os.path.join(pair, "frame.xisf"), [image_entry(test_image(np.uint16, 6, 8, 1, 298))])
    kept = {n: open(os.path.join(pair, n), "rb").read() for n in os.listdir(pair)}
    for flags in ([], ["--force"]):
        r = tool(os.path.join(pair, "frame.fits"), os.path.join(pair, "frame.xisf"), *flags)
        check(r.returncode == 1 and r.stderr.count("is an input of this run and is not written over") == 2 and
              {n: open(os.path.join(pair, n), "rb").read() for n in os.listdir(pair)} == kept,
              f"a file and its counterpart, each to the other {flags}: neither is written over: {r.stderr.strip()[:160]}")
    r = tool(os.path.join(pair, "frame.fits"), os.path.join(pair, "frame.xisf"), "--skip-existing")
    check(r.returncode == 0 and r.stdout.count("exists; passed over") == 2 and
          {n: open(os.path.join(pair, n), "rb").read() for n in os.listdir(pair)} == kept, "... and with --skip-existing both are passed over")

    # ---- patterns: an argument with * or ? that names no file
    p = fresh("patterns")
    for name in ("M31 [Ha].xisf", "M31 H.xisf", "café.xisf", "cafe.xisf", ".dot.xisf"):
        write_xisf(os.path.join(p, name), [image_entry(test_image(np.uint8, 4, 5, 1, 297))])

    def matched(*args, cwd=p):
        r = tool("--info", *args, cwd=cwd)
        return [line.split(": ")[0].replace(os.sep, "/") for line in r.stdout.splitlines() if re.match(r".*: (XISF 1\.0|FITS|ASDF)", line)], r

    for pattern, names in (
            ("*.xisf", ["M31 H.xisf", "M31 [Ha].xisf", "cafe.xisf", "café.xisf", "m31.xisf"]),
            ("night1/*.xisf", ["night1/light_001.xisf", "night1/light_002.xisf"]),
            ("night?/*.fi*", ["night1/darks/dark.fit"][:0] + ["night2/flat.fits", "night2/packed.fits.fz"]),
            ("night*/*/*", ["night1/darks/dark.fit"]),
            ("*/light_00?.xisf", ["night1/light_001.xisf", "night1/light_002.xisf"]),
            ("M31 [Ha]*", ["M31 [Ha].xisf"]),                       # a bracket is a bracket
            ("caf?.xisf", ["cafe.xisf", "café.xisf"]),            # ? is one character, of however many bytes
            ("caf??.xisf", []),
            (".*.xisf", [".dot.xisf"]),                             # a dot at the start only for a dot
            ("*1", ["night1/light_001.xisf", "night1/light_002.xisf", "night1/darks/dark.fit"]),   # a directory: what is in it
            ("./m3?.*", ["./m31.xisf"]),
            ("n*2/unit.xish", ["night2/unit.xish"])):
        got, r = matched(pattern)
        wanted = sorted(names) if pattern != "*1" else ["night1/darks/dark.fit", "night1/light_001.xisf", "night1/light_002.xisf"]
        check(got == wanted and (r.returncode == 0) == bool(names), f"the pattern {pattern}: {got} for {wanted}; {r.stderr.strip()[:200]}")
    got, r = matched(os.path.join(p, "night1", "*.xisf"), cwd=base)
    check(got == [os.path.join(p, "night1", n).replace(os.sep, "/") for n in ("light_001.xisf", "light_002.xisf")],
          f"a pattern with an absolute path: {got}")
    got, r = matched("*.XISF")
    check(len(got) == (5 if windows else 0) and (r.returncode == 0) == windows,
          f"letters match as the system compares names (here: {'whatever their case' if windows else 'as they are written'}): {got}")
    r = tool("-t", "tiff", "*.nothing", "night1/*.xisf", "zz*/x?", "-d", ".", cwd=p)
    check(r.returncode == 1 and r.stderr.count(": no file matches this pattern") == 2 and summary_of(r) == (2, 0, 2) and
          sorted(f for f in files_below(p) if f.endswith(".tif")) == ["light_001.tif", "light_002.tif"],
          f"patterns that match nothing are errors, and the rest is converted (a pattern's files are not kept in a tree below -d): "
          f"{r.stdout.splitlines()[-1:]} {r.stderr.strip()[:200]}")
    r = tool("--verify", "night2/*.fits*", "*.none", cwd=p)
    check(r.returncode == 1 and r.stdout.count(": OK") == 2 and "2 files OK, 1 failed" in r.stdout, f"--verify takes patterns: {r.stdout.splitlines()[-1:]}")
    r = tool("*.xisf", "-o", "one.fits", cwd=p)
    check(r.returncode == 2 and "single input" in r.stderr and not os.path.exists(os.path.join(p, "one.fits")),
          f"-o with a pattern of several files: refused: {r.stderr.strip()}")
    r = tool("café.*", "-o", "one.fits", "-q", cwd=p)
    check(r.returncode == 0 and os.path.exists(os.path.join(p, "one.fits")), f"-o with a pattern of one file: {r.stderr.strip()}")
    if not windows:
        literal = os.path.join(p, "what?.xisf")
        shutil.copy(os.path.join(p, "cafe.xisf"), literal)
        shutil.copy(os.path.join(p, "cafe.xisf"), os.path.join(p, "whatX.xisf"))
        got, r = matched("what?.xisf")
        check(got == ["what?.xisf"], f"a file that is called what?.xisf is that file, not a pattern: {got}")
        os.remove(literal)
        got, r = matched("what?.xisf")
        check(got == ["whatX.xisf"], f"... and a pattern once it is gone: {got}")
    else:
        got, r = matched("night1\\*.xisf")
        check(got == ["night1/light_001.xisf", "night1/light_002.xisf"], f"Windows: a pattern behind a backslash: {got}")
        long_form = "\\\\?\\" + os.path.abspath(p)
        got, r = matched(long_form + "\\caf?.xisf", cwd=base)
        check(got == [(long_form + "\\" + n).replace("\\", "/") for n in ("cafe.xisf", "café.xisf")],
              f"Windows: a pattern behind a long path (\\\\?\\): {got} {r.stderr.strip()[:200]}")
        got, r = matched(long_form + "\\cafe.xisf", cwd=base)
        check(len(got) == 1, f"Windows: the question mark of a long path is no wildcard: {got} {r.stderr.strip()[:200]}")
        got, r = matched(long_form + "\\missing.xisf", cwd=base)
        check(r.returncode == 1 and "no file matches" not in r.stderr and "missing.xisf" in r.stderr,
              f"Windows: ... also not for a file that is not there: {r.stderr.strip()[:200]}")
        r = tool("/?", cwd=p)
        check(r.returncode == 0 and r.stdout.startswith("xisfconv ") and "Usage:" in r.stdout, f"Windows: /? asks for the help: {r.stdout[:60]!r}")

    got, r = matched("night*/")
    check(got == ["night1/darks/dark.fit", "night1/light_001.xisf", "night1/light_002.xisf", "night2/Sp ace [Ha] é.fts", "night2/flat.fits",
                  "night2/packed.fits.fz", "night2/tree.asdf", "night2/unit.xish"], f"a pattern that ends in a separator means directories: {got}")
    open(os.path.join(p, "nightly.fits"), "wb").write(open(os.path.join(p, "night2", "flat.fits"), "rb").read())
    got, r = matched("night*/")
    got2, r2 = matched("night*")
    check("nightly.fits" not in got and len(got) == 8 and "nightly.fits" in got2 and len(got2) == 9,
          f"... and not a file of such a name, which the pattern without it takes: {len(got)}, {len(got2)}")
    os.remove(os.path.join(p, "nightly.fits"))
    if hasattr(os, "mkfifo"):
        os.mkfifo(os.path.join(p, "night1", "pipe.xisf"))
        try:
            r = subprocess.run([EXE, "--info", "night1/*.xisf"], cwd=p, capture_output=True, text=True, timeout=120)
            check(r.returncode == 0 and "pipe.xisf" not in r.stdout + r.stderr, f"a pattern does not stand for a pipe: {r.stderr.strip()[:200]}")
        except subprocess.TimeoutExpired:
            check(False, "a pattern does not stand for a pipe: the program waits on it")
        os.remove(os.path.join(p, "night1", "pipe.xisf"))

    # ---- a name twice, a picture of an earlier run among the inputs, the directory of the outputs under a pattern
    q = fresh("again")
    r = tool("-t", "tiff", "m31.xisf", "./m31.xisf", os.path.join(q, "m31.xisf"), cwd=q)
    check(r.returncode == 0 and len(lines_of(r)) == 1 and "m31.tif" in files_below(q),
          f"a file that is given three times is converted once: {r.stdout.strip()} {r.stderr.strip()[:200]}")
    r = tool("-t", "png", "-b", "u8", "night1", os.path.join("night1", "light_001.xisf"), "night1" + os.sep, cwd=q)
    check(r.returncode == 0 and summary_of(r) == (3, 0, 0),
          f"... and a directory with one of its files, and once more: {r.stdout.splitlines()[-1:]} {r.stderr.strip()[:200]}")
    r = tool("-t", "tiff", "-f", "m31.xisf", "m31.tif", cwd=q)
    check(r.returncode == 1 and lines_of(r) == ["m31.xisf -> m31.tif"] and "error: m31.tif: " in r.stderr and "is an input of this run" not in r.stderr,
          f"a picture of an earlier run among the inputs is an error of its own and does not keep its source from being converted: "
          f"{r.stderr.strip()[:200]}")
    os.makedirs(os.path.join(q, "out"))
    for name in ("notes.txt", "m31.tif"):     # ("*" brings along what is there, as a shell's does; here only images and directories)
        os.remove(os.path.join(q, name))
    first = tool("-t", "fits", "*", "-d", "out", cwd=q)
    second = tool("-t", "tiff", "*", "-d", "out", "-f", cwd=q)
    pictures = sorted(f for f in files_below(os.path.join(q, "out")) if f.endswith(".tif"))
    wanted = sorted(v.split("/", 1)[1] if "/" in v else v for v in outputs_expected(before, "tiff").values())
    check(first.returncode == 0 and second.returncode == 0 and "out: the directory of the outputs (-d) is not searched" in second.stderr and
          not [f for f in files_below(q) if f.startswith("out/out/")] and pictures == wanted,
          f"a pattern that also matches the directory of the outputs does not mean it: {pictures} for {wanted}; "
          f"{second.stdout.splitlines()[-1:]} {second.stderr.strip()[-300:]}")
    os.makedirs(os.path.join(q, "night1", "pictures"))
    other_spelling = os.path.join("night1", "..", "night1", "pictures")
    r = tool("-t", "fits", "night1", "-d", other_spelling, cwd=q)       # (FITS files there, which a search below night1 would find)
    r2 = tool("-t", "tiff", "night1", "-d", other_spelling, "-f", cwd=q)
    r3 = tool("-t", "tiff", "night1", "-d", other_spelling, "-f", cwd=q)
    check(r.returncode == 0 and r2.returncode == 0 and r3.returncode == 0 and summary_of(r) == (2, 1, 0) and summary_of(r3) == (3, 0, 0) and
          not [f for f in files_below(q) if "pictures/pictures" in f] and "outputs were there" not in r3.stdout,
          f"the directory of the outputs is known under another spelling, and --force gets no hint about outputs in the way: "
          f"{r3.stdout.splitlines()[-1:]} {[f for f in files_below(q) if 'pictures' in f]}")

    # ---- the guards go by the file, not by how its name is written
    g2 = os.path.join(base, "spellings")
    os.makedirs(os.path.join(g2, "pair"))
    fits.PrimaryHDU(np.full((6, 8), 5, np.uint16)).writeto(os.path.join(g2, "pair", "frame.fits"))
    write_xisf(os.path.join(g2, "pair", "frame.xisf"), [image_entry(test_image(np.uint16, 6, 8, 1, 295))])
    pair_was = {n: open(os.path.join(g2, "pair", n), "rb").read() for n in os.listdir(os.path.join(g2, "pair"))}
    spellings = [("a relative and an absolute name", [os.path.join(g2, "pair", "frame.fits"), "frame.xisf"], os.path.join(g2, "pair")),
                 ("a name through ..", [os.path.join("pair", "frame.fits"), os.path.join("pair", "..", "pair", "frame.xisf")], g2)]
    if hasattr(os, "symlink") and not windows:
        os.symlink(os.path.join(g2, "pair"), os.path.join(g2, "link"))
        spellings.append(("a name through a link to the directory", [os.path.join("link", "frame.fits"), os.path.join("pair", "frame.xisf")], g2))
    for what, names, cwd in spellings:
        r = tool("-f", *names, cwd=cwd)
        check(r.returncode == 1 and r.stderr.count("is an input of this run") == 2 and
              {n: open(os.path.join(g2, "pair", n), "rb").read() for n in os.listdir(os.path.join(g2, "pair"))} == pair_was,
              f"an output that is an input, {what}: not written over: {r.stderr.strip()[:160]}")
    for sub in ("d1", "d2", "out"):
        os.makedirs(os.path.join(g2, sub))
    for sub, name, value in (("d1", "Frame.fits", 11), ("d2", "frame.fits", 22)):
        fits.PrimaryHDU(np.full((6, 8), value, np.uint16)).writeto(os.path.join(g2, sub, name))
    open(os.path.join(g2, "CaseProbe"), "w").close()
    one_name = os.path.exists(os.path.join(g2, "caseprobe"))
    r = tool(os.path.join("d1", "Frame.fits"), os.path.join("d2", "frame.fits"), "-t", "tiff", "-d", "out", "-f", cwd=g2)
    made = files_below(os.path.join(g2, "out"))
    if one_name:      # Windows, and macOS as it comes: the two names are one name there
        check(r.returncode == 1 and made == ["Frame.tif"] and int(tifffile.imread(os.path.join(g2, "out", "Frame.tif"))[0, 0]) == 11 and
              "was written in this run already" in r.stderr,
              f"two outputs whose names differ in their case, where that is one name: the second is refused: {made} {r.stderr.strip()[:160]}")
    else:
        check(r.returncode == 0 and made == ["Frame.tif", "frame.tif"], f"two outputs whose names differ in their case, where those are two names: {made}")
    if hasattr(os, "link") and not windows:
        hard = os.path.join(g2, "hard")
        os.makedirs(hard)
        fits.PrimaryHDU(np.full((6, 8), 7, np.uint16)).writeto(os.path.join(hard, "other.fits"))
        os.link(os.path.join(hard, "other.fits"), os.path.join(hard, "frame.fits"))
        write_xisf(os.path.join(hard, "frame.xisf"), [image_entry(test_image(np.uint16, 6, 8, 1, 294))])
        hard_was = {n: open(os.path.join(hard, n), "rb").read() for n in os.listdir(hard)}
        r = tool("-f", "frame.xisf", "other.fits", "-t", "fits", cwd=hard)
        check(r.returncode == 1 and "frame.xisf: its output frame.fits is an input of this run" in r.stderr and
              {n: open(os.path.join(hard, n), "rb").read() for n in hard_was} == hard_was and
              os.path.samefile(os.path.join(hard, "other.fits"), os.path.join(hard, "frame.fits")),
              f"an output that is another name of an input (a hard link): not written over: {r.stderr.strip()[:200]}")
        # a link that leads nowhere, where an output would go: it is something that is there
        dangling = os.path.join(g2, "dangling")
        os.makedirs(dangling)
        write_xisf(os.path.join(dangling, "a.xisf"), [image_entry(test_image(np.uint16, 6, 8, 1, 293))])
        os.symlink(os.path.join(g2, "nowhere.dat"), os.path.join(dangling, "a.fits"))
        r = tool(dangling)
        r2 = tool(dangling, "--skip-existing")
        check(r.returncode == 1 and "is a link that leads nowhere" in r.stderr and r2.returncode == 0 and summary_of(r2) == (0, 1, 0) and
              os.path.islink(os.path.join(dangling, "a.fits")) and not os.path.exists(os.path.join(g2, "nowhere.dat")),
              f"an output name that is a link to nowhere is not taken for free: {r.stderr.strip()[:160]}")
    if hasattr(os, "symlink") and not windows:
        # an output name that is a link to a file: written over with --force, and then it is the output of this run
        lk = os.path.join(g2, "linked-out")
        for sub in ("a", "b", "out"):
            os.makedirs(os.path.join(lk, sub))
        for sub, value in (("a", 1), ("b", 2)):
            fits.PrimaryHDU(np.full((6, 8), value, np.uint16)).writeto(os.path.join(lk, sub, "x.fits"))
        open(os.path.join(lk, "other.tif"), "wb").write(b"a file the link leads to")
        os.symlink(os.path.join(lk, "other.tif"), os.path.join(lk, "out", "x.tif"))
        r = tool("-t", "tiff", "-d", "out", "-f", os.path.join("a", "x.fits"), os.path.join("b", "x.fits"), cwd=lk)
        check(r.returncode == 1 and "was written in this run already" in r.stderr and
              int(tifffile.imread(os.path.join(lk, "out", "x.tif"))[0, 0]) == 1,
              f"two inputs with one output whose name was a link: the second is refused all the same: {r.stderr.strip()[:200]}")
        # a link to a file of a directory is a name of its own; the first mention of a file decides where its output goes
        sl = os.path.join(g2, "sl")
        os.makedirs(os.path.join(sl, "n1"))
        write_xisf(os.path.join(sl, "n1", "a.xisf"), [image_entry(test_image(np.uint16, 6, 8, 1, 291))])
        os.symlink("a.xisf", os.path.join(sl, "n1", "current.xisf"))
        r = tool("-t", "tiff", sl)
        check(r.returncode == 0 and summary_of(r) == (2, 0, 0) and sorted(os.listdir(os.path.join(sl, "n1"))) ==
              ["a.tif", "a.xisf", "current.tif", "current.xisf"], f"a link to a file of the directory has an output of its own: {r.stdout.splitlines()[-1:]}")
        for args, place in (([os.path.join(sl, "n1", "a.xisf"), sl], "a.fits"), ([sl, os.path.join(sl, "n1", "a.xisf")], os.path.join("n1", "a.fits"))):
            out = os.path.join(g2, "first-" + str(len(place)))
            os.makedirs(out)
            r = tool("-t", "fits", *args, "-d", out, "-q")
            check(r.returncode == 0 and os.path.exists(os.path.join(out, place)) and len(files_below(out)) == 2,
                  f"a file named alone and found in a directory: converted once, where its first mention puts it ({place}): {files_below(out)}")
    if hasattr(os, "link") and not windows:
        hl = os.path.join(g2, "hl")
        os.makedirs(os.path.join(hl, "out"))
        write_xisf(os.path.join(hl, "a.xisf"), [image_entry(test_image(np.uint16, 6, 8, 1, 290))])
        os.link(os.path.join(hl, "a.xisf"), os.path.join(hl, "out", "a.xisf"))
        r = tool("-t", "xisf", "--codec", "zlib", "a.xisf", "-d", "out", "--skip-existing", cwd=hl)
        check(r.returncode == 0 and "exists; passed over" in r.stdout and os.path.samefile(os.path.join(hl, "a.xisf"), os.path.join(hl, "out", "a.xisf")),
              f"--skip-existing, and the output is another name of the input: passed over: {r.stdout.strip()} {r.stderr.strip()[:160]}")
    pics = os.path.join(g2, "pictures")
    os.makedirs(pics)
    write_xisf(os.path.join(pics, "m31.xisf"), [image_entry(test_image(np.uint16, 6, 8, 1, 289))])
    run(os.path.join(pics, "m31.xisf"), "-q")
    open(os.path.join(pics, "m31.tif"), "wb").write(b"a picture of an earlier run")
    r = tool("-t", "fits", "--skip-existing", "*", cwd=pics)
    check(r.returncode == 1 and "error: m31.tif: " in r.stderr and "m31.tif: " not in r.stdout and "m31.xisf: m31.fits exists; passed over" in r.stdout,
          f"--skip-existing, and a picture among the inputs: an error of its own, not passed over: {r.stderr.strip()[:200]} {r.stdout.strip()[-200:]}")

    # a file that fails leaves no directory behind below -d
    broken = os.path.join(g2, "broken")
    os.makedirs(os.path.join(broken, "deep", "deeper"))
    open(os.path.join(broken, "deep", "deeper", "junk.xisf"), "wb").write(b"XISF0100 and nothing that follows")
    write_xisf(os.path.join(broken, "good.xisf"), [image_entry(test_image(np.uint16, 6, 8, 1, 292))])
    os.makedirs(os.path.join(g2, "out2"))
    r = tool(broken, "-d", os.path.join(g2, "out2"))
    check(r.returncode == 1 and summary_of(r) == (1, 0, 1) and sorted(os.listdir(os.path.join(g2, "out2"))) == ["good.fits"],
          f"a file that fails leaves no empty directory below -d: {sorted(os.listdir(os.path.join(g2, 'out2')))}")
    r = tool(empty, "-q")
    check(r.returncode == 0 and r.stdout == "" and r.stderr == "", f"-q: a directory without images says nothing: {r.stderr!r}")

    # ---- a directory whose listing breaks off (an I/O error half way): that is said, and it is an error
    shim = os.path.join(base, "shim.so")
    source = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rename_shim.c")
    flat = os.path.join(base, "flat")
    os.makedirs(flat)
    for k in range(6):
        write_xisf(os.path.join(flat, f"f{k}.xisf"), [image_entry(test_image(np.uint8, 4, 5, 1, 280 + k))])
    loaded = False
    if sys.platform.startswith("linux") and shutil.which("cc") and os.path.exists(source):
        if subprocess.run(["cc", "-shared", "-fPIC", "-o", shim, source, "-ldl"], capture_output=True).returncode == 0:
            # Is the library taken by the program? Its rename() says so, which has nothing to do with directories. (A program
            # that is linked statically takes no such library, and one built with AddressSanitizer refuses it.)
            probe = subprocess.run([EXE, os.path.join(flat, "f0.xisf"), "-o", os.path.join(base, "probe.tif")], capture_output=True, text=True,
                                   env=dict(os.environ, LD_PRELOAD=shim, XISFCONV_TEST_FAIL_RENAME=".tif.part>.tif"))
            loaded = probe.returncode == 1 and "cannot rename" in probe.stderr and not os.path.exists(os.path.join(base, "probe.tif"))
    if loaded:
        for what, args in (("a conversion", ["-t", "tiff", flat]), ("--verify", ["--verify", flat]),
                           ("a pattern", ["--verify", os.path.join(flat, "f*.xisf")])):
            r = subprocess.run([EXE, *args], capture_output=True, text=True,
                               env=dict(os.environ, LD_PRELOAD=shim, XISFCONV_TEST_FAIL_READDIR="5"))
            seen = r.stdout.count(" -> ") + r.stdout.count(": OK")
            check(r.returncode == 1 and 0 < seen < 6 and "Input/output error" in r.stderr + r.stdout,
                  f"{what}, and the listing of the directory breaks off: an error, exit status 1 ({seen} of 6 files were seen): "
                  f"{(r.stderr + r.stdout).strip()[-200:]}")
            for n in os.listdir(flat):
                if n.endswith(".tif"):
                    os.remove(os.path.join(flat, n))
        r = subprocess.run([EXE, "-t", "tiff", os.path.join(flat, "f*.xisf")], capture_output=True, text=True,
                           env=dict(os.environ, LD_PRELOAD=shim, XISFCONV_TEST_FAIL_READDIR="0"))
        check(r.returncode == 1 and summary_of(r) == (0, 0, 1) and "no file matches" not in r.stderr and "Input/output error" in r.stderr,
              f"a pattern whose directory cannot be listed at all: one error, that one: {r.stderr.strip()[:200]} {r.stdout.strip()[-80:]}")
    else:
        skipped.append("a directory whose listing breaks off (Linux, a C compiler, and a program that loads tests/rename_shim.c)")

    # the rule held against its second writing-down, on names and patterns drawn at random
    names_dir = os.path.join(base, "names")
    os.makedirs(names_dir)
    rng = random.Random(20261008)
    letters = ["a", "b", "A", ".", "[", "]", "_", " ", "é", "É", "中"]
    for _ in range(60):
        name = "".join(rng.choice(letters) for _ in range(rng.randint(0, 4))).lstrip(" ") + "x"
        open(os.path.join(names_dir, name), "w").close()
    there = os.listdir(names_dir)
    wrong, tried = [], 0
    while tried < 160:
        pattern = "".join(rng.choice(letters + ["*", "*", "?", "?"]) for _ in range(rng.randint(1, 5))).strip(" ")
        if not ("*" in pattern or "?" in pattern) or pattern.startswith("-"):
            continue
        tried += 1
        r = tool("--verify", pattern, cwd=names_dir)
        got = [line[:-len(": FAILED")] for line in r.stdout.splitlines() if line.endswith(": FAILED") and not line.startswith("  ")]
        wanted = sorted(n for n in there if pattern_matches(pattern, n))
        if got != wanted or (not wanted) != ("no file matches this pattern" in r.stderr):
            wrong.append((pattern, got, wanted))
    check(len(there) > 30 and not wrong, f"{tried} patterns drawn at random match what the rule says, in the order of the names: {ascii(wrong[:3])}")

    # ---- links and directories that cannot be read
    if hasattr(os, "symlink") and not windows:
        where = fresh("links")
        outside = os.path.join(base, "outside")
        os.makedirs(outside)
        write_xisf(os.path.join(outside, "far.xisf"), [image_entry(test_image(np.uint16, 4, 5, 1, 296))])
        os.symlink(outside, os.path.join(where, "linked-dir"))
        os.symlink(os.path.join(outside, "far.xisf"), os.path.join(where, "linked.xisf"))
        os.symlink(where, os.path.join(where, "night1", "up"))                 # a circle
        r = tool("-t", "tiff", where)
        new = sorted(set(files_below(where)) - set(before))
        told = [line.split(" -> ")[0][len(where) + 1:] for line in lines_of(r)]
        check(r.returncode == 0 and "linked.tif" in new and told == sorted(list(outputs_expected(before, "tiff")) + ["linked.xisf"]) and
              files_below(outside) == ["far.xisf"],
              f"a link to a file is the file, a link to a directory is not followed: {told}")
        if os.geteuid() != 0:
            locked = os.path.join(where, "night2")
            os.chmod(locked, 0)
            try:
                r = tool("-t", "png", "-b", "u8", where)
            finally:
                os.chmod(locked, 0o755)
            check(r.returncode == 1 and "night2: " in r.stderr and summary_of(r) is not None and summary_of(r)[0] >= 4 and summary_of(r)[2] == 1,
                  f"a directory that cannot be read is an error, and the others are converted: {r.stdout.splitlines()[-1:]} {r.stderr.strip()[-200:]}")
        else:
            skipped.append("a directory that cannot be read (the tests run as root, which reads everything)")


def markdown_headings(text):
    """The anchors GitHub gives the headings of a Markdown document."""
    import re
    anchors, seen, fenced = set(), {}, False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
        m = None if fenced else re.match(r"#{1,6}\s+(.*?)\s*#*\s*$", line)
        if not m:
            continue
        title = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", m.group(1)).replace("`", "").replace("*", "")
        slug = re.sub(r"[^\w\- ]", "", title.lower()).replace(" ", "-")
        n = seen.get(slug, 0)
        seen[slug] = n + 1
        anchors.add(slug if n == 0 else f"{slug}-{n}")
    return anchors


def markdown_links(text):
    """The targets of the links and pictures of a Markdown document, those in code left out."""
    import re
    out, fenced = [], False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
        if not fenced:
            line = re.sub(r"`[^`]*`", "", line)
            out += re.findall(r"\]\(([^)\s]+)\)", line)
            out += re.findall(r'\b(?:src|srcset|href)="([^"\s]+)"', line)      # (pictures and links written as HTML)
    return out


def test_documents():
    """The documents beside the program say what the program does: the man page has the options of
    --help, the man page, CITATION.cff and CHANGELOG.md the version of the program, the links between
    the documents lead somewhere, and the examples of docs/xisf-properties-in-fits-and-asdf.md and
    examples/wcs_digest.py do with the files of xisfconv what the document says."""
    import importlib.util
    import re
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    here = lambda *parts: os.path.join(root, *parts)   # noqa: E731
    text_of = lambda *parts: open(here(*parts), encoding="utf-8").read()   # noqa: E731
    if not all(os.path.exists(here(*p)) for p in (("man", "xisfconv.1"), ("CITATION.cff",), ("CHANGELOG.md",),
                                                  ("docs", "xisf-properties-in-fits-and-asdf.md"), ("examples", "wcs_digest.py"))):
        skipped.append("the documents (the tests are not run from a source tree)")
        return
    version = run("--version").stdout.split()[1]
    months = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November",
              "December"]

    # ---- the man page: the options of --help, in its order, and the values an option takes
    man = text_of("man", "xisfconv.1")
    help_text = run("--help").stdout
    in_help, values = [], {}
    for line in help_text.splitlines():
        m = re.match(r"  (?:(-\w), |    )(--[a-z0-9-]+)(?: <([^>]*\|[^>]*)>)?", line)
        if m:
            in_help.append((m.group(1), m.group(2)))
            if m.group(3):
                values[m.group(2)] = m.group(3).split("|")
    options = re.search(r"^\.SH OPTIONS\n(.*?)^\.SH ", man, re.S | re.M).group(1)
    in_man, paragraphs = [], {}
    for part in re.split(r"^\.TP\n", options, flags=re.M)[1:]:
        tag, _, body = part.partition("\n")
        tag = tag.replace("\\-", "-")
        short = re.search(r"(?<![\w-])(-\w)(?![\w-])", tag)
        name = re.search(r"--[a-z0-9-]+", tag).group(0)
        in_man.append((short.group(1) if short else None, name))
        paragraphs[name] = re.split(r"^\.S[SH] ", body, flags=re.M)[0].replace("\\-", "-")
    check(len(in_help) > 25 and in_man == in_help,
          f"the man page has the options of --help, in its order: only in the page {[o for o in in_man if o not in in_help]}, "
          f"only in --help {[o for o in in_help if o not in in_man]}")
    missing = [(name, v) for name, vs in values.items() for v in vs
               if not re.search(r"(?<![\w-])%s(?![\w-])" % re.escape(v), paragraphs.get(name, ""))]
    check(len(values) >= 4 and not missing, f"... and for {sorted(values)} every value --help names: missing {missing}")
    written = "\n".join(line for line in man.splitlines() if not line.startswith('.\\"'))
    check("\\-\\-" in options and not re.search(r"(?<!\\)--", written),
          "... with the hyphens of its options written as minus signs (\\-), as man wants them")

    # ---- one version and one date in the man page, the citation file and the changelog
    th = re.search(r'^\.TH XISFCONV 1 "(\d{4}-\d\d-\d\d)" "xisfconv ([\d.]+)"', man, re.M)
    check(th is not None and th.group(2) == version, f"the man page is that of xisfconv {version}: {th and th.group(2)}")
    cff = text_of("CITATION.cff")
    cff_version = re.search(r'^version: "?([\d.]+)"?\s*$', cff, re.M)
    cff_date = re.search(r'^date-released: "?(\d{4}-\d\d-\d\d)"?\s*$', cff, re.M)
    check(cff_version is not None and cff_version.group(1) == version,
          f"CITATION.cff cites xisfconv {version}: {cff_version and cff_version.group(1)}")
    if HAVE_YAML:
        try:
            tree = yaml.safe_load(cff)
        except yaml.YAMLError as e:
            tree = str(e)
        check(isinstance(tree, dict) and tree.get("cff-version") == "1.2.0" and tree.get("type") == "software" and
              all(tree.get(k) for k in ("message", "title", "authors")) and tree.get("version") == version,
              f"... and is a citation file with what the format requires: {str(tree)[:200]}")
    changelog = text_of("CHANGELOG.md")
    sections = re.findall(r"^## (\d+)\.(\d+)\.(\d+)\b[^(\n]*\((\d+) (\w+) (\d{4})\)", changelog, re.M)
    first = sections[0] if sections else None
    check(first is not None and ".".join(first[:3]) == version,
          f"CHANGELOG.md begins with xisfconv {version}: {first and '.'.join(first[:3])}")
    numbers = [tuple(int(n) for n in s[:3]) for s in sections]
    check(len(numbers) > 20 and numbers == sorted(numbers, reverse=True) and len(set(numbers)) == len(numbers),
          f"... and has its {len(numbers)} versions with the newest first")
    said = first and "%s-%02d-%02d" % (first[5], months.index(first[4]) + 1 if first[4] in months else 0, int(first[3]))
    check(th is not None and cff_date is not None and th.group(1) == cff_date.group(1) == said,
          f"the three give that version one date: {th and th.group(1)}, {cff_date and cff_date.group(1)}, {said}")

    # ---- the links between the documents lead to a file, and to a heading of it
    documents = [here(n) for n in sorted(os.listdir(root)) if n.endswith(".md")]
    for sub in (("docs",), ("python",), (".github", "ISSUE_TEMPLATE")):
        if os.path.isdir(here(*sub)):
            documents += [here(*sub, n) for n in sorted(os.listdir(here(*sub))) if n.endswith(".md")]
    dead, followed = [], 0
    for doc in documents:
        text = open(doc, encoding="utf-8").read()
        for target in markdown_links(text):
            if re.match(r"[a-z][a-z0-9+.-]*:", target):
                continue                                         # another site
            path, _, anchor = target.partition("#")
            file = os.path.normpath(os.path.join(os.path.dirname(doc), path)) if path else doc
            followed += 1
            if not os.path.exists(file):
                dead.append(f"{os.path.relpath(doc, root)}: {target}")
            elif anchor and file.endswith(".md"):
                if anchor not in markdown_headings(open(file, encoding="utf-8").read()):
                    dead.append(f"{os.path.relpath(doc, root)}: {target} (no such heading)")
            elif anchor and file.endswith(".html"):
                if not re.search(r'\bid="%s"' % re.escape(anchor), open(file, encoding="utf-8").read()):
                    dead.append(f"{os.path.relpath(doc, root)}: {target} (no such id)")
    check(followed > 40 and not dead, f"the {followed} links between the {len(documents)} documents lead somewhere: {dead[:6]}")
    for name in ("file-that-fails.md", "something-else.md"):
        if os.path.exists(here(".github", "ISSUE_TEMPLATE", name)):
            head = re.match(r"---\n(.*?)\n---\n", text_of(".github", "ISSUE_TEMPLATE", name), re.S)
            check(head is not None and re.search(r"^name: \S", head.group(1), re.M) and re.search(r"^about: \S", head.group(1), re.M),
                  f"the issue template {name} has the name and the description GitHub shows")

    # ---- the format note: its examples, run as they stand on files xisfconv made
    d = os.path.join(TMP, "documents")
    os.makedirs(d, exist_ok=True)
    W, H = 240, 160
    ref_img = np.array([W / 2 + 0.3, H / 2 - 0.7])
    M = np.array([[-2.3565e-4, 1.1696e-5], [-1.1715e-5, -2.3575e-4]])
    xy = np.random.default_rng(5).uniform([5, 5], [W - 5, H - 5], (200, 2))
    offset = xy - ref_img
    world = (offset @ M.T) * (1 + 4e-4 * ((offset / 120.0) ** 2).sum(1)[:, None])
    P = "PCL:AstrometricSolution:"
    properties = "".join([
        f'<Property id="{P}ProjectionSystem" type="String">Gnomonic</Property>',
        f64_prop(P + "ReferenceCelestialCoordinates", [328.178, 47.358]),
        f64_prop(P + "ReferenceImageCoordinates", ref_img),
        f64_prop(P + "ReferenceNativeCoordinates", [0, 90]),
        f64_prop(P + "CelestialPoleNativeCoordinates", [180, 90]),
        f64_prop(P + "LinearTransformationMatrix", M.ravel(), 2, 2),
        f64_prop(P + "SplineWorldTransformation:ControlPoints:Image", xy.ravel()),
        f64_prop(P + "SplineWorldTransformation:ControlPoints:World", world.ravel()),
        '<Property id="Observation:CelestialReferenceSystem" type="String">ICRS</Property>',
        '<Property id="Instrument:Telescope:FocalLength" type="Float64" value="0.922597"/>'])
    source = os.path.join(d, "image.xisf")
    write_xisf(source, [image_entry(test_image(np.uint16, H, W, 1, 77), children=properties)])
    run(source, "-o", os.path.join(d, "image.fits"), "-q")
    run(source, "-o", os.path.join(d, "image.asdf"), "-q")
    note = text_of("docs", "xisf-properties-in-fits-and-asdf.md")
    blocks = re.findall(r"^```python\n(.*?)^```", note, re.S | re.M)
    by_file = {name: [b for b in blocks if f'"{name}"' in b] for name in ("image.fits", "with-properties.fits", "image.asdf")}
    check(len(blocks) == 3 and all(len(b) == 1 for b in by_file.values()),
          f"the note has its three examples in Python: {len(blocks)}, {[len(b) for b in by_file.values()]}")

    def python(code):
        return subprocess.run([sys.executable, "-c", code], cwd=d, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", env=dict(os.environ, PYTHONIOENCODING="utf-8"))

    if all(len(b) == 1 for b in by_file.values()):
        r = python(by_file["image.fits"][0])
        matrix = re.search(r"LinearTransformationMatrix \[\[\s*(\S+)\s+(\S+)\]\s*\[\s*(\S+)\s+(\S+)\]\]", r.stdout)
        check(r.returncode == 0 and "Instrument:Telescope:FocalLength Float64 0.922597" in r.stdout.splitlines() and
              "PCL:AstrometricSolution:ProjectionSystem String Gnomonic" in r.stdout.splitlines() and matrix is not None and
              np.array_equal(np.array([float(v) for v in matrix.groups()]), np.array([float("%.8g" % v) for v in M.ravel()])),
              f"reading with astropy, as the note does it, prints the properties: {r.stdout[:300]!r} {r.stderr[-300:]}")
        r = python(by_file["with-properties.fits"][0])
        check(r.returncode == 0 and os.path.exists(os.path.join(d, "with-properties.fits")),
              f"writing a table with astropy, as the note does it: {r.stderr[-300:]}")
        if r.returncode == 0:
            r = run(os.path.join(d, "with-properties.fits"))
            got = {p["id"]: p for p in xisf_properties(os.path.join(d, "with-properties.xisf"))[0][0]}
            name, gain, flags, grid = (got.get(k, {}) for k in ("Observation:Object:Name", "My:Gain", "My:Flags", "My:Matrix"))
            check(name.get("type") == "String" and name.get("value") == ("text", b"M 31") and
                  gain.get("type") == "Float64" and gain.get("value") == ("text", b"1.25") and gain.get("comment") == "electrons per ADU",
                  f"... which xisfconv turns into the properties of an XISF image: {name} {gain}")
            check(flags.get("type") == "UI16Vector" and flags.get("value") == ("data", (3, None, None), np.array([1, 2, 3], "<u2").tobytes())
                  and grid.get("type") == "F64Matrix" and
                  grid.get("value") == ("data", (None, 2, 2), np.array([[1, 2], [3, 4]], "<f8").tobytes()),
                  f"... the vector and the matrix with their values: {flags} {grid}")
            # the same table with its columns in another order and one the convention does not have
            with fits.open(os.path.join(d, "with-properties.fits")) as hdul:
                table = hdul["XISF_PROPERTIES"]
                columns = [fits.Column(name="LATER", format="J", array=np.arange(len(table.data)))]
                for c in list(table.columns)[::-1]:           # (made anew: astropy does not copy a heap column as it is)
                    values = table.data[c.name]
                    if c.format.startswith("P"):
                        values = np.array([np.asarray(v, np.uint8) for v in values] + [None], dtype=object)[:-1]
                    columns.append(fits.Column(name=c.name, format="PB()" if c.format.startswith("P") else c.format, array=values))
                other = fits.BinTableHDU.from_columns(columns, name="XISF_PROPERTIES")
                fits.HDUList([fits.PrimaryHDU(hdul[0].data), other]).writeto(os.path.join(d, "other-order.fits"), overwrite=True)
            run(os.path.join(d, "other-order.fits"), "-f")
            again = {p["id"]: p for p in xisf_properties(os.path.join(d, "other-order.xisf"))[0][0]}
            check(again == got and len(got) == 4, f"... and the same from a table with its columns in another order and one more: {sorted(again)}")
        if HAVE_ASDF:
            r = python(by_file["image.asdf"][0] + "    print(repr(focal), type(focal).__name__, matrix.shape, matrix.dtype, matrix.tolist())\n")
            check(r.returncode == 0 and r.stdout.split("]]")[0] + "]]" ==
                  "0.922597 float (2, 2) float64 " + repr(M.tolist()),
                  f"reading with the asdf library, as the note does it, gives a float and a matrix: {r.stdout[:300]!r} {r.stderr[-300:]}")
        else:
            skipped.append("the ASDF example of the format note (pip install asdf asdf-astropy)")

    # ---- the digest of the WCS: the example program computes what xisfconv stores
    spec = importlib.util.spec_from_file_location("wcs_digest_example", here("examples", "wcs_digest.py"))
    example = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(example)
    # (xisfconv writes the keywords of a solution with 12 digits. An image that has WCS keywords of its own keeps them
    # as they are written, and those are the numbers that need the rule of 15, 16 or 17 digits, and the forms a
    # FITS card may have: more digits than a double holds, a D for the exponent, a point without a fraction.)
    written = {"CRVAL1": repr(985 / 3), "CRVAL2": repr(47 + 0.1 + 0.2), "CRPIX1": "120.8", "CRPIX2": "81.2",
               "CD1_1": "-2.3565000000000001E-04", "CD1_2": "-1.1696e-05", "CD2_1": repr(-1.1715e-5 / 3), "CD2_2": "2.3575D-04",
               "LONPOLE": "180.", "EQUINOX": "2000", "CRVAL1A": "-0.0"}
    # (... and the values that are not numbers: the records of the distortion paper, of which astropy makes keywords
    # of other names with numbers for values; a text with a quote in it and blanks at its end; a complex value; none)
    others = [("CPDIS1", "'LOOKUP'"), ("DP1", "'EXTVER: 1'"), ("DP1", "'NAXES: 2'"), ("DP1", "'AXIS.1: 1'"), ("PS1_0", "'it''s  '"),
              ("PV1_1", "(1.0, 2.0)"), ("PV1_2", ""), ("PV1_3", ".5"), ("PV1_3", "5."), ("CRVAL1000", "1.5"), ("D2IMARR ONE", "'x y'")]
    keywords = "".join(f'<FITSKeyword name="{k}" value="{v}" comment=""/>' for k, v in
                       [("WCSAXES", "2"), ("CTYPE1", "'RA---TAN'"), ("CTYPE2", "'DEC--TAN'"), ("RADESYS", "'ICRS    '")] +
                       list(written.items()) + others)
    own = os.path.join(d, "keywords.xisf")
    write_xisf(own, [image_entry(test_image(np.uint16, H, W, 1, 78), children=keywords + "".join([
        f'<Property id="{P}ProjectionSystem" type="String">Gnomonic</Property>',
        f64_prop(P + "ReferenceCelestialCoordinates", [985 / 3, 47 + 0.1 + 0.2]),
        f64_prop(P + "ReferenceImageCoordinates", [120.3, 79.3]),
        f64_prop(P + "LinearTransformationMatrix", [-2.3565e-4, 1.1696e-5, -1.1715e-5 / 3, -2.3575e-4], 2, 2)]))])
    bare = os.path.join(d, "bare.xisf")       # properties without a solution among them have a digest too
    write_xisf(bare, [image_entry(test_image(np.uint16, H, W, 1, 79),
                                  children='<Property id="Instrument:Telescope:FocalLength" type="Float64" value="0.92"/>')])
    digests = {}
    for label, origin, name, flags in (("bottom-up", source, "image.fits", []), ("top-down", source, "top-down.fits", ["--top-down"]),
                                       ("tile-compressed", source, "packed.fits.fz", []),
                                       ("without SIP", source, "linear.fits", ["--sip-order", "0"]),
                                       ("keywords of its own", own, "keywords.fits", []), ("no solution", bare, "bare.fits", [])):
        out = os.path.join(d, name)
        run(origin, "-o", out, "-f", "-q", *flags)
        with fits.open(out) as hdul:
            packed = hdul[0].data is None
            image = hdul[1] if packed else hdul[0]
            stored = hdul["XISF_PROPERTIES"].header.get("WCSDIGST")
            header = image.header.copy()
            size = image.data.shape[-1], image.data.shape[-2]
        up = label != "top-down"
        digests[label] = mine = example.wcs_digest(header, *size, up)
        check(stored is not None and re.fullmatch(r"[0-9a-f]{40}", stored) and mine == stored and
              (label in ("without SIP", "keywords of its own", "no solution")) != ("A_ORDER" in header) and
              (label == "tile-compressed") == packed,
              f"the digest of the example program is that of the file ({label}): {mine}, {stored}")
        if label == "keywords of its own":
            texts = {k: example.number_text(float(header[k])) for k in written}
            check(texts == {"CRVAL1": "328.3333333333333", "CRVAL2": "47.300000000000004", "CRPIX1": "120.8", "CRPIX2": "81.2",
                            "CD1_1": "-0.00023565", "CD1_2": "-1.1696e-05", "CD2_1": "-3.905e-06", "CD2_2": "0.00023575",
                            "LONPOLE": "180", "EQUINOX": "2000", "CRVAL1A": "0"} and
                  [len(t.replace(".", "")) for t in (texts["CRVAL1"], texts["CRVAL2"])] == [16, 17],
                  f"... with numbers of 16 and of 17 digits, and numbers written in other ways, each as its value: {texts}")
            lines = sorted("%s=%s" % (c.rawkeyword, example.value_text(c)) for c in header.cards
                           if c.rawkeyword in {k for k, _ in others})
            long_names = sorted(c.image.split("=")[0].rstrip() for c in header.cards if c.image.startswith("HIERARCH"))
            check(lines == ["CPDIS1=LOOKUP", "CRVAL1000=1.5", "D2IMARR ONE=x y", "DP1=AXIS.1: 1", "DP1=EXTVER: 1", "DP1=NAXES: 2",
                            "PS1_0=it's", "PV1_1=(1.0, 2.0)", "PV1_2=", "PV1_3=0.5", "PV1_3=5"] and "DP1.EXTVER" in header and
                  long_names == ["HIERARCH CRVAL1000", "HIERARCH D2IMARR ONE"],
                  f"... and with records, a text, a complex value, none, a keyword that stands twice and HIERARCH cards: {lines} "
                  f"{long_names}")
        r = subprocess.run([sys.executable, here("examples", "wcs_digest.py"), out], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        check(r.returncode == 0 and "the WCS keywords, the size and the row order are what they were" in r.stdout and
              "changed" not in r.stdout,
              f"... and the program says so: {r.stdout.strip()} {r.stderr[-200:]}")
        if label == "bottom-up":
            for what, change in (("another row order", lambda h: None), ("another size", lambda h: None),
                                 ("a keyword changed in its last digit", lambda h: h.set("CRVAL1", np.nextafter(h["CRVAL1"], 400))),
                                 ("a keyword removed", lambda h: h.remove("CD1_2")),
                                 ("a keyword added", lambda h: h.set("PV1_1", 1.0))):
                other = header.copy()
                change(other)
                theirs = example.wcs_digest(other, size[0] + (what == "another size"), size[1], what != "another row order")
                check(theirs != stored, f"... {what} gives another digest")
            same = header.copy()
            same["OBJECT"] = "M 31"
            same["CRVAL2"] = (same["CRVAL2"], "the comment of a card is not its value")
            check(example.wcs_digest(same, *size, up) == stored, "... and a keyword that is none of the WCS, or a comment, the same")
    check(len(set(digests.values())) == 5 and digests["bottom-up"] == digests["tile-compressed"],
          f"the row order and the distortion are in the digest, the compression of the file is not: {digests}")
    # what the digest is for: a solution that was changed in the FITS file is not overruled by the properties
    edited = os.path.join(d, "edited.fits")
    shutil.copy(os.path.join(d, "image.fits"), edited)
    with fits.open(edited, mode="update") as hdul:
        hdul[0].header["CRVAL1"] = 10.5
    r = subprocess.run([sys.executable, here("examples", "wcs_digest.py"), edited], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    check(r.returncode == 0 and "the WCS keywords, the size or the row order changed since" in r.stdout,
          f"after a change of CRVAL1 the program says that the keywords changed: {r.stdout.strip()} {r.stderr[-200:]}")
    run(edited, "-o", os.path.join(d, "edited.xisf"), "-f", "-q")
    center = xisf_property(os.path.join(d, "edited.xisf"), P + "ReferenceCelestialCoordinates")
    run(os.path.join(d, "image.fits"), "-o", os.path.join(d, "back.xisf"), "-f", "-q")
    kept = xisf_property(os.path.join(d, "back.xisf"), P + "ReferenceCelestialCoordinates")
    check(center is not None and abs(center[0] - 10.5) < 1e-9 and kept is not None and np.array_equal(kept, [328.178, 47.358]),
          f"... and xisfconv takes the solution of the keywords then, and that of the properties otherwise: {center}, {kept}")


if __name__ == "__main__":
    print("xisfconv:", EXE)
    print(subprocess.run([EXE, "--version"], capture_output=True, text=True).stdout.strip())
    print("libtiff tiffcp:", "yes" if HAVE_TIFFCP else "no (TIFF decoded by tifffile only)")
    print("NASA fitsverify:", "yes" if HAVE_FITSVERIFY else "no (FITS checked by astropy only)")
    print("CFITSIO fpack/funpack:", "yes" if HAVE_FPACK else "no (tile-compressed FITS checked against astropy only)")
    print("asdf + asdf-astropy:", "yes" if HAVE_ASDF else "no -- ASDF files are checked by xisfconv's own reader only "
          "(pip install asdf asdf-astropy)")
    print("imagecodecs:", "yes" if HAVE_IMAGECODECS else
          ("no (tiffcp decodes compressed float TIFFs)" if HAVE_TIFFCP
           else "no -- compressed float TIFF checks will be SKIPPED (pip install imagecodecs)"))
    for t in (test_python_xisf_codecs, test_hand_written, test_checksum_mismatch, test_truncated_and_garbage,
              test_keywords_and_properties, test_multi_image_icc_resolution, test_bits_conversion,
              test_batch_and_outdir, test_stretch, test_wcs, test_solution_properties_forms, test_tiff_predictors, test_png,
              test_fits_to_xisf_formats, test_fits_to_xisf_metadata, test_fits_to_xisf_bounds_bits_hdus,
              test_xisf_fits_xisf_roundtrip, test_asdf_yaml, test_asdf_hand_written, test_asdf_output,
              test_asdf_python_files, test_asdf_roundtrips, test_export_from_fits_and_asdf, test_xisf_rewrite,
              test_verify, test_fits_tile_compressed, test_fits_tile_writing, test_property_round_trip,
              test_downsampling_and_thumbnailer, test_distributed_units, test_directories_and_patterns,
              test_documents):
        try:
            t()
        except Exception as e:  # noqa: BLE001
            failures.append(f"{t.__name__}: {type(e).__name__}: {e}")
            print("ERROR in", t.__name__, ":", e)
    if LAST_BIT:
        print(f"\n{len(LAST_BIT)} comparisons of quantized floating point were equal but for the rounding of one multiplication:\n"
              "  the other software fuses multiplication and addition on this machine, xisfconv does not (see MANUAL.md)")
    if skipped:
        print(f"\nskipped {len(skipped)} checks that need optional tools:")
        for what in sorted(set(skipped))[:8]:
            print("  -", what)
    print(f"\n{passed} checks passed, {len(failures)} failed")
    if not failures:
        shutil.rmtree(TMP, ignore_errors=True)
    else:
        print("temp files kept in", TMP)
    sys.exit(1 if failures else 0)
