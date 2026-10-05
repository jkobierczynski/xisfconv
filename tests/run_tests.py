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
        run(p, "-o", f1, "-f", "-q", *first)
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
                ["-c", "--xisf-subblock-size", "1000"]]
    for k, (name, (data, want)) in enumerate(cases.items()):
        src = os.path.join(TMP, f"f2x_{name}.fits")
        fits.PrimaryHDU(data).writeto(src, overwrite=True)
        exp = fits_expected(src)
        for j, flags in enumerate([variants[k % len(variants)], variants[(k + 3) % len(variants)]]):
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
        r = run(mid, "-o", x, "-f")
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
    return raw, raw[16:16 + int.from_bytes(raw[8:12], "little")].decode()


def xisf_blocks(path):
    """Every data block of an XISF file, read without xisfconv: a list of dicts with the element name,
    its id, the location kind, the storage attributes, the stored bytes and the decoded bytes."""
    import xml.etree.ElementTree as ET
    raw, hdr = xisf_header(path)
    out = []
    for el in ET.fromstring(hdr).iter():
        loc = el.get("location")
        if loc is None:
            continue
        src = el
        if loc.startswith("attachment:"):
            _, pos, size = loc.split(":")
            stored = raw[int(pos):int(pos) + int(size)]
            assert len(stored) == int(size), "attachment beyond the end of " + path
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
        out.append({"tag": el.tag.split("}")[-1], "id": el.get("id"), "kind": loc.split(":")[0], "attr": attr,
                    "stored": stored, "data": data, "location": loc})
    return out


def header_without_storage(hdr):
    """The header with everything xisfconv may change when rewriting removed: the storage attributes of
    attached blocks and the file properties that describe the block storage."""
    import re
    def strip(m):
        return re.sub(r'\s+(location|compression|subblocks|checksum)="[^"]*"', "", m.group(0))
    hdr = re.sub(r'<[^<>]*\slocation="attachment:[^<>]*>', strip, hdr)
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


def rich_xisf(path, storage, align=1, extra_metadata=""):
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
    write_xisf_blocks(path, template, blocks, align)
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
                   ["-c", "--checksum", "sha1"], ["--codec", "zlib", "--xisf-subblock-size", "500"]]
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
    check(r.returncode == 1 and "external file" in r.stderr, "distributed XISF is refused when rewriting")
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
    # a directory where a file is meant (--verify takes directories; --info and conversion do not)
    for arguments in (["--info", d], [d, "-o", os.path.join(d, "from-directory.fits")]):
        r = subprocess.run([EXE] + arguments, capture_output=True, text=True)
        check(r.returncode == 1 and "is a directory, not a file" in r.stderr and not os.path.exists(os.path.join(d, "from-directory.fits")),
              f"a directory given as a file is called a directory: {r.stderr.strip()}")
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
              test_verify, test_fits_tile_compressed):
        try:
            t()
        except Exception as e:  # noqa: BLE001
            failures.append(f"{t.__name__}: {type(e).__name__}: {e}")
            print("ERROR in", t.__name__, ":", e)
    if LAST_BIT:
        print(f"\n{len(LAST_BIT)} comparisons of quantized floating point were equal but for the rounding of one multiplication:\n"
              "  the other software fuses multiplication and addition on this machine, xisfconv does not (see README)")
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
