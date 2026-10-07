#!/usr/bin/env python3
"""Tests of libxisfconv through its C API, called with ctypes.

SPDX-License-Identifier: GPL-3.0-or-later

What the library writes from arrays is read back by independent software (astropy, the `xisf`
package, Python's asdf, tifffile, Pillow), and what it reads from files written by that software
is compared with the software's own reading. The structs below are declared a second time, for
ctypes: if they did not match the header, nothing here would work.

Requirements: pip install numpy astropy tifffile xisf pillow    (asdf asdf-astropy: optional)
Usage: python3 tests/library_tests.py path/to/libxisfconv.so [path/to/xisfconv]
       (libxisfconv.dylib on macOS, libxisfconv.dll on Windows; build with -DBUILD_SHARED_LIBS=ON)
"""
import ctypes as C
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import zlib

import numpy as np
import tifffile
from astropy.io import fits
from PIL import Image
from xisf import XISF

try:
    import asdf
    import asdf_astropy  # noqa: F401
    HAVE_ASDF = True
except ImportError:  # pragma: no cover
    HAVE_ASDF = False

# astropy does not map files into memory here: Windows refuses to replace a file that is still
# mapped, and several tests write a file again after reading it. (Linux and Wine allow it, so only
# a real Windows run shows a test that forgot.)
fits.conf.use_memmap = False

if len(sys.argv) < 2:
    sys.exit(__doc__)
LIB_PATH = os.path.abspath(sys.argv[1])
EXE = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
TMP = tempfile.mkdtemp(prefix="libxisfconv-test-")
lib = C.CDLL(LIB_PATH)

passed = 0
failures = []


def check(cond, msg):
    global passed
    if cond:
        passed += 1
    else:
        failures.append(msg)
        print("FAIL:", msg)


# ---------------------------------------------------------------------------------------------
# The API, declared for ctypes
# ---------------------------------------------------------------------------------------------

OK, ERR_ARGUMENT, ERR_IO, ERR_FORMAT, ERR_UNSUPPORTED, ERR_CHECKSUM, ERR_MEMORY, ERR_INDEX, ERR_EXISTS, ERR_BUFFER, \
    ERR_NOT_FOUND, ERR_CANCELLED = range(12)
FORMAT_AUTO, FORMAT_XISF, FORMAT_FITS, FORMAT_ASDF, FORMAT_TIFF, FORMAT_PNG = range(6)
AS_STORED, UINT8, UINT16, UINT32, UINT64, FLOAT32, FLOAT64 = range(7)
ROWS_DEFAULT, ROWS_TOP_DOWN, ROWS_BOTTOM_UP = range(3)
CODEC_NONE, CODEC_ZLIB, CODEC_LZ4, CODEC_LZ4HC, CODEC_ZSTD, CODEC_DEFAULT = range(6)
CHECKSUM_NONE, SHA1, SHA256, SHA512, SHA3_256, SHA3_512 = range(6)
ALL_IMAGES = C.c_size_t(-1).value
FILE_PROPERTIES = ALL_IMAGES

SAMPLE = {np.dtype(np.uint8): UINT8, np.dtype(np.uint16): UINT16, np.dtype(np.uint32): UINT32, np.dtype(np.uint64): UINT64,
          np.dtype(np.float32): FLOAT32, np.dtype(np.float64): FLOAT64}
DTYPE = {v: k for k, v in SAMPLE.items()}

i32, u64, f64, size_t, text, ptr = C.c_int32, C.c_uint64, C.c_double, C.c_size_t, C.c_char_p, C.c_void_p


class ImageInfo(C.Structure):
    _fields_ = [("struct_size", size_t), ("width", u64), ("height", u64), ("channels", u64),
                ("sample_format", i32), ("data_known", i32), ("lower_bound", f64), ("upper_bound", f64),
                ("color_space", i32), ("row_order", i32), ("row_order_declared", i32),
                ("convertible", i32), ("has_icc_profile", i32), ("has_display_function", i32), ("has_stored_stretch", i32),
                ("has_astrometric_solution", i32), ("has_cfa", i32), ("cfa_width", i32), ("cfa_height", i32),
                ("cfa_pattern", C.c_char * 68), ("resolution_unit", i32), ("resolution_x", f64), ("resolution_y", f64),
                ("bitpix", i32), ("plain_array", i32), ("bscale", f64), ("bzero", f64), ("source_index", u64),
                ("wcs_row_order", i32)]


class ReadOptions(C.Structure):
    _fields_ = [("struct_size", size_t), ("sample_format", i32), ("row_order", i32), ("verify_checksums", i32),
                ("use_bounds", i32), ("lower_bound", f64), ("upper_bound", f64)]


class ConvertOptions(C.Structure):
    _fields_ = [("struct_size", size_t), ("output_format", i32), ("sample_format", i32), ("image", size_t), ("stretch", i32),
                ("codec", i32), ("checksum", i32), ("subblock_size", u64), ("row_order", i32), ("property_keywords", i32),
                ("wcs", i32), ("sip_order", i32), ("verify_checksums", i32), ("use_bounds", i32), ("overwrite", i32),
                ("lower_bound", f64), ("upper_bound", f64), ("properties", i32), ("reserved", i32), ("fit_width", u64),
                ("fit_height", u64), ("scale", f64), ("bin", i32), ("reserved2", i32)]


class WriteOptions(C.Structure):
    _fields_ = [("struct_size", size_t), ("format", i32), ("codec", i32), ("checksum", i32), ("row_order", i32),
                ("subblock_size", u64), ("wcs", i32), ("overwrite", i32), ("shuffle", i32), ("compression_level", i32),
                ("properties", ptr), ("creator_application", text)]


class ImageIn(C.Structure):
    _fields_ = [("struct_size", size_t), ("pixels", ptr), ("width", u64), ("height", u64), ("channels", u64),
                ("sample_format", i32), ("row_order", i32), ("use_bounds", i32), ("lower_bound", f64), ("upper_bound", f64),
                ("name", text), ("keywords", ptr), ("icc_profile", ptr), ("icc_profile_size", size_t), ("wcs_row_order", i32),
                ("reserved", i32), ("properties", ptr)]


class StretchParams(C.Structure):
    _fields_ = [("shadows", f64), ("midtones", f64), ("highlights", f64), ("low", f64), ("high", f64)]


MESSAGE_FN = C.CFUNCTYPE(None, ptr, i32, text, text)
PROGRESS_FN = C.CFUNCTYPE(i32, ptr, text, u64, u64)


def declare(name, restype, *argtypes):
    f = getattr(lib, "xisfconv_" + name)
    f.restype = restype
    f.argtypes = list(argtypes)
    return f


version = declare("version", text)
context_new = declare("context_new", ptr)
context_free = declare("context_free", None, ptr)
set_message_handler = declare("context_set_message_handler", None, ptr, MESSAGE_FN, ptr)
set_progress_handler = declare("context_set_progress_handler", None, ptr, PROGRESS_FN, ptr)
keep_messages = declare("context_keep_messages", None, ptr, i32)
message_count = declare("context_message_count", size_t, ptr)
message_get = declare("context_message", i32, ptr, size_t, C.POINTER(i32), C.POINTER(text), C.POINTER(text))
clear_messages = declare("context_clear_messages", None, ptr)
context_cancel = declare("context_cancel", None, ptr)
error_message = declare("error_message", text, ptr)
keywords_new = declare("keywords_new", i32, ptr, C.POINTER(ptr))
keywords_free = declare("keywords_free", None, ptr)
keywords_count = declare("keywords_count", size_t, ptr)
keywords_get = declare("keywords_get", i32, ptr, size_t, C.POINTER(text), C.POINTER(text), C.POINTER(text))
keywords_append = declare("keywords_append", i32, ptr, text, text, text)
keywords_append_string = declare("keywords_append_string", i32, ptr, text, text, text)
keywords_append_number = declare("keywords_append_number", i32, ptr, text, f64, text)
file_open = declare("open", i32, ptr, text, C.POINTER(ptr))
file_close = declare("close", None, ptr)
file_format = declare("file_format", i32, ptr)
image_count = declare("image_count", size_t, ptr)
image_info_init = declare("image_info_init", None, C.POINTER(ImageInfo), size_t)
image_info_get = declare("image_info_get", i32, ptr, size_t, C.POINTER(ImageInfo))
image_name = declare("image_name", text, ptr, size_t)
image_detail = declare("image_detail", text, ptr, size_t, text)
image_keywords = declare("image_keywords", i32, ptr, size_t, C.POINTER(ptr))
property_count = declare("property_count", size_t, ptr, size_t)
property_find = declare("property_find", C.c_int64, ptr, size_t, text)
property_get = declare("property_get", i32, ptr, size_t, size_t, C.POINTER(text), C.POINTER(text), C.POINTER(text),
                       C.POINTER(text), C.POINTER(i32))
property_read_f64 = declare("property_read_f64", i32, ptr, size_t, text, C.POINTER(f64), size_t, C.POINTER(size_t),
                            C.POINTER(size_t))
read_options_init = declare("read_options_init", None, C.POINTER(ReadOptions), size_t)
load_pixels = declare("load_pixels", i32, ptr, size_t, i32)
pixels_size = declare("pixels_size", i32, ptr, size_t, C.POINTER(ReadOptions), C.POINTER(u64))
read_pixels = declare("read_pixels", i32, ptr, size_t, C.POINTER(ReadOptions), ptr, u64)
read_icc_profile = declare("read_icc_profile", i32, ptr, size_t, ptr, size_t, C.POINTER(size_t))
auto_stretch = declare("auto_stretch", i32, ptr, ptr, u64, u64, u64, i32, f64, f64, size_t, i32, C.POINTER(StretchParams))
apply_stretch = declare("apply_stretch", i32, ptr, ptr, u64, u64, u64, i32, f64, f64, C.POINTER(StretchParams), size_t, ptr)
wcs_keywords = declare("wcs_keywords", i32, ptr, size_t, i32, i32, C.POINTER(ptr), C.POINTER(text))
convert_options_init = declare("convert_options_init", None, C.POINTER(ConvertOptions), size_t)
convert = declare("convert", i32, ptr, text, text, C.POINTER(ConvertOptions))
verify = declare("verify", i32, ptr, text, C.POINTER(ptr))
report_free = declare("report_free", None, ptr)
report_verdict = declare("report_verdict", i32, ptr)
image_init = declare("image_init", None, C.POINTER(ImageIn), size_t)
write_options_init = declare("write_options_init", None, C.POINTER(WriteOptions), size_t)
writer_new = declare("writer_new", i32, ptr, text, C.POINTER(WriteOptions), C.POINTER(ptr))
writer_add_image = declare("writer_add_image", i32, ptr, C.POINTER(ImageIn))
writer_finish = declare("writer_finish", i32, ptr)
writer_discard = declare("writer_discard", None, ptr)
codec_available = declare("codec_available", i32, i32, i32)
rewrite = declare("rewrite", i32, ptr, text, text, ptr, ptr)
asdf_tree_json = declare("asdf_tree_json", i32, ptr, text, ptr, size_t, C.POINTER(size_t))
skipped_count = declare("skipped_count", size_t, ptr)
set_external_files = declare("context_set_external_files", i32, ptr, i32)
get_external_files = declare("context_external_files", i32, ptr)
external_count = declare("external_count", size_t, ptr)
external_file = declare("external_file", text, ptr, size_t)
unit_size = declare("unit_size", u64, ptr)
file_size = declare("file_size", u64, ptr)
file_detail = declare("file_detail", text, ptr, text)

ctx = context_new()


def enc(path):
    return path.encode("utf-8")


def err(context=None):
    return error_message(context or ctx).decode("utf-8", "replace")


# ---------------------------------------------------------------------------------------------
# A small binding, enough for the tests
# ---------------------------------------------------------------------------------------------

def make_keywords(cards, context=None):
    kw = ptr()
    assert keywords_new(context or ctx, C.byref(kw)) == OK
    for name, value, comment in cards:
        c = comment.encode() if comment else None
        if isinstance(value, str):
            assert keywords_append_string(kw, name.encode(), value.encode(), c) == OK
        elif isinstance(value, float):
            assert keywords_append_number(kw, name.encode(), value, c) == OK
        else:
            assert keywords_append(kw, name.encode(), str(value).encode(), c) == OK
    return kw


def cards_of(kw):
    out = []
    for k in range(keywords_count(kw)):
        n, v, c = text(), text(), text()
        assert keywords_get(kw, k, C.byref(n), C.byref(v), C.byref(c)) == OK
        out.append((n.value.decode(), v.value.decode(), c.value.decode()))
    return out


def write_images(path, arrays, context=None, rows=ROWS_TOP_DOWN, cards=None, names=None, icc=None, bounds=None, wcs_rows=None,
                 **options):
    """arrays: planar [channels, height, width] (or [height, width]). Returns the status."""
    context = context or ctx
    wo = WriteOptions()
    write_options_init(C.byref(wo), C.sizeof(wo))
    wo.overwrite = 1
    for key, value in options.items():
        setattr(wo, key, value)
    w = ptr()
    st = writer_new(context, enc(path), C.byref(wo), C.byref(w))
    if st != OK:
        return st
    kw = make_keywords(cards, context) if cards else None
    keep = []
    for n, a in enumerate(arrays):
        a = np.ascontiguousarray(a if a.ndim == 3 else a[None])
        keep.append(a)
        img = ImageIn()
        image_init(C.byref(img), C.sizeof(img))
        img.pixels = a.ctypes.data
        img.channels, img.height, img.width = a.shape
        img.sample_format = SAMPLE[a.dtype]
        img.row_order = rows
        if names:
            img.name = names[n].encode()
        if kw:
            img.keywords = kw
        if icc:
            img.icc_profile = C.cast(C.c_char_p(icc), ptr)
            img.icc_profile_size = len(icc)
        if bounds:
            img.use_bounds, img.lower_bound, img.upper_bound = 1, bounds[0], bounds[1]
        if wcs_rows is not None:
            img.wcs_row_order = wcs_rows
        st = writer_add_image(w, C.byref(img))
        if st != OK:
            writer_discard(w)
            if kw:
                keywords_free(kw)
            return st
    st = writer_finish(w)
    if kw:
        keywords_free(kw)
    return st


class Opened:
    def __init__(self, path, context=None):
        self.context = context or ctx
        self.handle = ptr()
        self.status = file_open(self.context, enc(path), C.byref(self.handle))

    def __enter__(self):
        if self.status != OK:
            raise RuntimeError(err(self.context))
        return self

    def __exit__(self, *exc):
        file_close(self.handle)

    def info(self, image=0):
        info = ImageInfo()
        image_info_init(C.byref(info), C.sizeof(info))
        st = image_info_get(self.handle, image, C.byref(info))
        if st != OK:
            raise RuntimeError(err(self.context))
        return info

    def detail(self, name, image=0):
        return image_detail(self.handle, image, name.encode()).decode()

    def cards(self, image=0):
        kw = ptr()
        assert image_keywords(self.handle, image, C.byref(kw)) == OK
        return cards_of(kw)

    def read(self, image=0, dtype=None, rows=ROWS_DEFAULT, bounds=None):
        """Returns [channels, height, width], or the status if reading fails."""
        ro = ReadOptions()
        read_options_init(C.byref(ro), C.sizeof(ro))
        ro.row_order = rows
        if dtype is not None:
            ro.sample_format = SAMPLE[np.dtype(dtype)]
        if bounds:
            ro.use_bounds, ro.lower_bound, ro.upper_bound = 1, bounds[0], bounds[1]
        size = u64()
        st = pixels_size(self.handle, image, C.byref(ro), C.byref(size))
        if st != OK:
            return st
        info = self.info(image)
        kind = DTYPE[ro.sample_format] if dtype is not None else DTYPE[info.sample_format]
        a = np.empty((info.channels, info.height, info.width), kind)
        assert a.nbytes == size.value, (a.nbytes, size.value)
        st = read_pixels(self.handle, image, C.byref(ro), a.ctypes.data, a.nbytes)
        return a if st == OK else st


def same(a, b):
    """Same shape, same kind and size of sample (whatever the byte order), same values."""
    return (isinstance(a, np.ndarray) and a.shape == b.shape and a.dtype.kind == b.dtype.kind and a.dtype.itemsize == b.dtype.itemsize and
            np.array_equal(a, b, equal_nan=True))


def xisf_bounds(path):
    lo, hi = XISF(path).get_images_metadata()[0]["bounds"].split(":")
    return float(lo), float(hi)


def image(dtype, shape, seed=0):
    """A test image [channels, height, width] with every part of the value range."""
    rng = np.random.default_rng(seed + sum(shape))
    c, h, w = shape
    y, x = np.mgrid[0:h, 0:w]
    base = (x * 3 + y * 7)[None] + np.arange(c)[:, None, None] * 11
    if np.issubdtype(dtype, np.floating):
        a = ((base % 1000) / 1024.0 + rng.random(base.shape) / 4096).astype(dtype)
        a[0, 0, 0] = 0
        a[-1, -1, -1] = 1
        return a
    info = np.iinfo(dtype)
    a = (base.astype(np.uint64) * 37 + rng.integers(0, 4, base.shape).astype(np.uint64)) % np.uint64(min(info.max, 2 ** 40) + 1)
    a = a.astype(dtype)
    a[0, 0, 0] = info.max
    a[0, 0, 1] = 0
    return a


CARDS = [("OBJECT", "M 31", "the target"), ("EXPTIME", 30.5, "exposure [s]"), ("NCOMBINE", 12, None),
         ("HISTORY", "", "written by the library tests")]
DTYPES = (np.uint8, np.uint16, np.uint32, np.uint64, np.float32, np.float64)
SHAPES = ((1, 23, 31), (3, 23, 31), (4, 9, 13))


# ---------------------------------------------------------------------------------------------
# Writing arrays
# ---------------------------------------------------------------------------------------------

def test_write_fits():
    path = os.path.join(TMP, "w.fits")
    for dtype in DTYPES:
        for shape in SHAPES:
            a = image(dtype, shape)
            label = f"{np.dtype(dtype).name} {shape}"
            # top-down arrays are stored bottom-up, the FITS convention
            check(write_images(path, [a], cards=CARDS, names=["first"]) == OK, f"write FITS {label}: {err()}")
            with fits.open(path) as h:
                d = np.atleast_3d(np.array(h[0].data)) if a.shape[0] == 1 else np.array(h[0].data)
                if a.shape[0] == 1:
                    d = np.array(h[0].data)[None]
                check(same(d, a[:, ::-1, :]), f"FITS {label}: astropy reads the array, bottom-up")
                hd = h[0].header
                check(hd["OBJECT"] == "M 31" and hd["EXPTIME"] == 30.5 and hd["NCOMBINE"] == 12 and hd["ROWORDER"] == "BOTTOM-UP" and
                      hd["EXTNAME"] == "first" and "written by the library tests" in str(hd["HISTORY"]),
                      f"FITS {label}: keywords, ROWORDER and name")
            # the same array handed over bottom-up is stored as it is
            check(write_images(path, [a], rows=ROWS_BOTTOM_UP) == OK, f"write FITS {label}, bottom-up buffer")
            d = np.array(fits.getdata(path))
            check(same(d if d.ndim == 3 else d[None], a), f"FITS {label}: a bottom-up buffer is stored unchanged")
            # and top-down on request
            check(write_images(path, [a], row_order=ROWS_TOP_DOWN) == OK, f"write FITS {label}, top-down rows")
            with fits.open(path) as h:
                d = np.array(h[0].data)
                check(same(d if d.ndim == 3 else d[None], a) and h[0].header["ROWORDER"] == "TOP-DOWN",
                      f"FITS {label}: top-down rows on request, with ROWORDER")
    # several images become several HDUs
    a, b = image(np.uint16, (1, 20, 30)), image(np.float32, (3, 8, 9))
    check(write_images(path, [a, b], names=["one", "two"]) == OK, "write two images")
    with fits.open(path) as h:
        check(len(h) == 2 and same(np.array(h[0].data)[None], a[:, ::-1]) and same(np.array(h[1].data), b[:, ::-1]) and
              h[1].header["EXTNAME"] == "two", "FITS: two HDUs")
    r = subprocess.run(["fitsverify", "-q", path], capture_output=True, text=True) if shutil.which("fitsverify") else None
    if r is not None:
        check(r.returncode == 0, f"fitsverify accepts the file: {r.stdout.strip()[:200]}")


def test_write_fits_tile_compressed():
    """FITS with a codec: tile-compressed without loss. astropy decompresses it, and its Rice
    encoder gives the same tiles."""
    import warnings
    path = os.path.join(TMP, "w.fits.fz")

    def table(n=1):
        with fits.open(path, disable_image_compression=True) as h:
            return h[n].header.copy(), [np.asarray(x, np.uint8).tobytes() for x in h[n].data["COMPRESSED_DATA"]]

    for dtype in DTYPES:
        for shape in SHAPES:
            a = image(dtype, shape)
            label = f"{np.dtype(dtype).name} {shape}"
            for codec, gzip_only in ((CODEC_DEFAULT, False), (CODEC_ZLIB, True)):
                check(write_images(path, [a], cards=CARDS, names=["first"], codec=codec) == OK, f"write tile-compressed FITS {label}: {err()}")
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore")
                    with fits.open(path) as h:
                        d = np.array(h[-1].data)
                        check(same(d if d.ndim == 3 else d[None], a[:, ::-1, :]) and h[-1].header["OBJECT"] == "M 31" and
                              h[-1].header["EXTNAME"] == "first", f"tile-compressed FITS {label}: astropy reads the array and the keywords")
                        compressed = type(h[-1]).__name__ == "CompImageHDU"
                    if np.dtype(dtype) == np.uint64:
                        check(not compressed, f"{label}: 64-bit integers stay a plain image")
                        continue
                    header, mine = table()
                    rice = not gzip_only and np.dtype(dtype).kind == "u"
                    check(compressed and header["ZCMPTYPE"] == ("RICE_1" if rice else "GZIP_1" if np.dtype(dtype).itemsize == 1 else "GZIP_2"),
                          f"{label}: {header['ZCMPTYPE']}")
                    if rice:
                        ref = os.path.join(TMP, "ref.fits.fz")
                        fits.HDUList([fits.PrimaryHDU(), fits.CompImageHDU(a[:, ::-1, :] if a.shape[0] > 1 else a[0, ::-1], compression_type="RICE_1")]).writeto(
                            ref, overwrite=True)
                        with fits.open(ref, disable_image_compression=True) as h:
                            theirs = [np.asarray(x, np.uint8).tobytes() for x in h[1].data["COMPRESSED_DATA"]]
                            comparable = h[1].header["ZVAL2"] == header["ZVAL2"] and h[1].header.get("ZTILE2", 1) == 1
                        check(comparable and mine == theirs, f"{label}: the Rice tiles are astropy's, byte for byte")
            with Opened(path) as f:
                d = f.read(rows=ROWS_TOP_DOWN)
                check(same(d, a) and f.detail("tileCompression") == ("" if np.dtype(dtype) == np.uint64 else header["ZCMPTYPE"]),
                      f"{label}: the library reads its own file back")
    # by the name alone, several images, and what FITS does not have
    a, b = image(np.uint16, (1, 20, 30)), image(np.float32, (3, 8, 9))
    check(write_images(path, [a, b], names=["one", "two"]) == OK, "write two images to a name that ends in .fz")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with fits.open(path) as h:
            check([type(x).__name__ for x in h] == ["PrimaryHDU", "CompImageHDU", "CompImageHDU"] and same(np.array(h[1].data)[None], a[:, ::-1]) and
                  same(np.array(h[2].data), b[:, ::-1]) and h[2].header["EXTNAME"] == "two", "two tile-compressed images")
    check(table(1)[0]["ZCMPTYPE"] == "RICE_1" and table(2)[0]["ZCMPTYPE"] == "GZIP_2", "RICE_1 for integers, GZIP_2 for floating point")
    r = subprocess.run(["fitsverify", "-q", path], capture_output=True, text=True) if shutil.which("fitsverify") else None
    if r is not None:
        check(r.returncode == 0, f"fitsverify accepts the file: {r.stdout.strip()[:200]}")
    zstd = os.path.join(TMP, "zstd.fits")
    st = write_images(zstd, [a], codec=CODEC_ZSTD)
    check(st in (ERR_ARGUMENT, ERR_UNSUPPORTED) and not os.path.exists(zstd) and not os.path.exists(zstd + ".part"),
          f"FITS has no Zstandard: {err()}")


def xisf_read(path, n=0):
    x = XISF(path)
    return x, np.moveaxis(np.asarray(x.read_image(n)), -1, 0)   # channels last -> first


def test_write_xisf():
    path = os.path.join(TMP, "w.xisf")
    for dtype in (np.uint8, np.uint16, np.uint32, np.float32, np.float64):   # the xisf package has no UInt64
        for shape in SHAPES:
            a = image(dtype, shape)
            label = f"{np.dtype(dtype).name} {shape}"
            for codec, checksum in ((CODEC_NONE, CHECKSUM_NONE), (CODEC_ZLIB, SHA1), (CODEC_DEFAULT, SHA512)):
                check(write_images(path, [a], cards=CARDS, names=["first"], codec=codec, checksum=checksum) == OK,
                      f"write XISF {label} codec {codec}: {err()}")
                x, d = xisf_read(path)
                meta = x.get_images_metadata()[0]
                check(same(d, a), f"XISF {label} codec {codec}: the xisf package reads the array")
                check(meta["id"] == "first" and meta["colorSpace"] == ("RGB" if shape[0] == 3 else "Gray") and
                      meta["FITSKeywords"]["OBJECT"][0]["value"] == "M 31" and
                      meta["FITSKeywords"]["NCOMBINE"][0]["value"] == "12", f"XISF {label}: id, colour space, keywords")
            # a bottom-up buffer is turned round: XISF is always top-down
            check(write_images(path, [a], rows=ROWS_BOTTOM_UP) == OK, f"write XISF {label}, bottom-up buffer")
            check(same(xisf_read(path)[1], a[:, ::-1, :]), f"XISF {label}: a bottom-up buffer is stored top-down")
    # UInt64 through the library's own reader
    a = image(np.uint64, (1, 12, 17))
    a[0, 1, 1] = 2 ** 64 - 1
    check(write_images(path, [a]) == OK, "write XISF uint64")
    with Opened(path) as f:
        check(same(f.read(), a), "XISF uint64 comes back")
    # bounds of floating point data
    a = image(np.float32, (1, 10, 12)) * 40000
    check(write_images(path, [a]) == OK, "write XISF, float beyond 1")
    check(xisf_bounds(path) == (0.0, 65535.0), "XISF: bounds 0:65535 for ADU-scaled floats")
    check(write_images(path, [a], bounds=(0.0, 50000.0)) == OK, "write XISF with stated bounds")
    check(xisf_bounds(path) == (0.0, 50000.0), "XISF: the bounds that were stated")
    check(write_images(path, [a - 100]) == OK, "write XISF, negative floats")
    lo, hi = xisf_bounds(path)
    check(abs(lo - float((a - 100).min())) < 1e-3 and abs(hi - float((a - 100).max())) < 1e-2, "XISF: bounds minimum:maximum otherwise")
    # ICC profile, and a SHA-3 checksum with its warning
    icc = bytes(range(64)) * 3
    seen = []
    handler = MESSAGE_FN(lambda user, level, p, m: seen.append((level, p, m)))
    set_message_handler(ctx, handler, None)
    check(write_images(path, [image(np.uint16, (3, 6, 7))], icc=icc, checksum=SHA3_256) == OK, "write XISF with ICC profile")
    set_message_handler(ctx, MESSAGE_FN(0), None)
    check(len(seen) == 1 and seen[0][0] == 1 and seen[0][1] == enc(path) and b"PixInsight" in seen[0][2],
          f"a SHA-3 checksum comes with one warning that names the file: {seen}")
    with Opened(path) as f:
        n = size_t()
        check(f.info().has_icc_profile == 1 and read_icc_profile(f.handle, 0, None, 0, C.byref(n)) == OK and n.value == len(icc),
              "XISF: the ICC profile is there")
        buf = C.create_string_buffer(n.value)
        check(read_icc_profile(f.handle, 0, buf, n.value, C.byref(n)) == OK and buf.raw == icc, "XISF: and comes back unchanged")
        check(f.detail("checksum").startswith("sha3-256:"), "XISF: SHA-3 checksum")
    x = XISF(path)
    check(x.get_images_metadata()[0].get("ICCProfile") is not None or b"<ICCProfile" in open(path, "rb").read(4096),
          "XISF: an ICCProfile element")
    # a BAYERPAT keyword becomes the ColorFilterArray, turned round with the rows
    cfa = image(np.uint16, (1, 8, 10))
    check(write_images(path, [cfa], cards=[("BAYERPAT", "RGGB", None)]) == OK, "write a CFA frame")
    with Opened(path) as f:
        i = f.info()
        check(i.has_cfa == 1 and i.cfa_pattern == b"RGGB" and i.cfa_width == 2, "XISF: BAYERPAT RGGB becomes the ColorFilterArray")
    check(write_images(path, [cfa], rows=ROWS_BOTTOM_UP, cards=[("BAYERPAT", "RGGB", None)]) == OK, "write a bottom-up CFA frame")
    with Opened(path) as f:
        check(f.info().cfa_pattern == b"GBRG", "XISF: the pattern follows the rows when they are turned round")


def test_write_asdf():
    if not HAVE_ASDF:
        print("asdf + asdf-astropy not installed: ASDF written by the library is read by the library only")
    path = os.path.join(TMP, "w.asdf")
    for dtype in DTYPES:
        for shape in SHAPES[:2]:
            a = image(dtype, shape)
            label = f"{np.dtype(dtype).name} {shape}"
            for codec in (CODEC_NONE, CODEC_DEFAULT):
                check(write_images(path, [a], cards=CARDS, codec=codec) == OK, f"write ASDF {label}: {err()}")
                with Opened(path) as f:
                    check(file_format(f.handle) == FORMAT_ASDF and same(f.read(rows=ROWS_TOP_DOWN), a),
                          f"ASDF {label} codec {codec}: the library reads its own file")
                if not HAVE_ASDF:
                    continue
                with asdf.open(path) as af:
                    hdus = af["fits"]
                    d = np.array(hdus[0].data)
                    check(same(d if d.ndim == 3 else d[None], a[:, ::-1, :]) and hdus[0].header["OBJECT"] == "M 31",
                          f"ASDF {label} codec {codec}: Python's asdf returns the HDU, bottom-up")


def test_write_tiff_png():
    path = os.path.join(TMP, "w.tif")
    icc = bytes(range(200))
    for dtype in (np.uint8, np.uint16, np.uint32, np.float32):
        for shape in SHAPES[:2]:
            a = image(dtype, shape)
            label = f"{np.dtype(dtype).name} {shape}"
            for codec in (CODEC_NONE, CODEC_DEFAULT):
                if codec != CODEC_NONE and dtype == np.float32:
                    continue   # needs imagecodecs; covered by the tool's tests
                check(write_images(path, [a], codec=codec, icc=icc) == OK, f"write TIFF {label}: {err()}")
                with tifffile.TiffFile(path) as t:
                    d = t.pages[0].asarray()
                    d = d[None] if d.ndim == 2 else np.moveaxis(d, -1, 0)
                    check(same(d, a), f"TIFF {label} codec {codec}: tifffile reads the array, top-down")
                    check(t.pages[0].tags[34675].value == icc, f"TIFF {label}: ICC profile")
    # a cube that is not RGB: one page per plane
    a = image(np.uint16, (4, 9, 13))
    check(write_images(path, [a]) == OK, "write a 4-plane TIFF")
    with tifffile.TiffFile(path) as t:
        check(len(t.pages) == 4 and all(same(t.pages[k].asarray(), a[k]) for k in range(4)), "TIFF: one page per plane")
    # floats beyond 0..1 are scaled: by 65535 if they fit that, else to their own range
    a = image(np.float32, (1, 10, 12)) * 1000 + 500
    check(write_images(path, [a]) == OK, "write a float TIFF with ADU-scaled values")
    check(np.allclose(tifffile.imread(path), a[0] / 65535.0, atol=1e-7), "TIFF: floats within 0..65535 are scaled by 65535")
    check(write_images(path, [a - 2000]) == OK, "write a float TIFF with negative values")
    d = tifffile.imread(path)
    check(abs(float(d.min())) < 1e-6 and abs(float(d.max()) - 1) < 1e-6, "TIFF: other floats are scaled to their own range")
    check(write_images(path, [a], bounds=(0.0, 3000.0)) == OK, "write a float TIFF with stated bounds")
    check(np.allclose(tifffile.imread(path), a[0] / 3000.0, atol=1e-6), "TIFF: scaled by the stated bounds")

    path = os.path.join(TMP, "w.png")
    for dtype in (np.uint8, np.uint16):
        for shape in SHAPES[:2]:
            a = image(dtype, shape)
            check(write_images(path, [a], icc=icc) == OK, f"write PNG {np.dtype(dtype).name} {shape}: {err()}")
            im = Image.open(path)
            d = np.array(im)
            d = d[None] if d.ndim == 2 else np.moveaxis(d, -1, 0)
            if dtype == np.uint16 and shape[0] == 3:
                check(im.size == (shape[2], shape[1]), "PNG 16-bit RGB: size")   # Pillow reduces 16-bit RGB to 8 bits
            else:
                check(same(d.astype(dtype), a), f"PNG {np.dtype(dtype).name} {shape}: Pillow reads the array")
            check(im.info.get("icc_profile") == icc, "PNG: ICC profile")
    a = image(np.float32, (1, 10, 12))
    check(write_images(path, [a]) == OK, "write a float image as PNG")
    d = np.array(Image.open(path)).astype(np.float64)
    check(np.abs(d - np.round(a[0].astype(np.float64) * 65535)).max() <= 1, "PNG: floats 0..1 become 16-bit")


def test_lz4_and_levels():
    """LZ4 and LZ4HC blocks as the library writes them: decoded by the lz4 library itself, and read by the xisf package
    and by the library. Compression levels, subblocks, and blocks without byte shuffling."""
    import zlib

    import lz4.block
    path = os.path.join(TMP, "lz4.xisf")
    rng = np.random.default_rng(7)

    def stored(meta):
        _, position, size = meta["location"]
        with open(path, "rb") as f:
            f.seek(position)
            return f.read(size)

    def unshuffled(data, item):
        return np.frombuffer(data, np.uint8).reshape(item, -1).T.tobytes() if item else data

    def lz4_decoded(meta):
        """The block of the image, decoded by the lz4 library."""
        raw = stored(meta)
        _, size, item = meta["compression"]
        if meta.get("subblocks"):
            parts, at = [], 0
            for pair in meta["subblocks"].split(":"):
                packed, plain = (int(n) for n in pair.split(","))
                parts.append(lz4.block.decompress(raw[at:at + packed], uncompressed_size=plain))
                at += packed
            data = b"".join(parts)
        else:
            data = lz4.block.decompress(raw, uncompressed_size=size)
        return unshuffled(data, item)

    arrays = {
        "smooth 16 bit": (np.add.outer(np.arange(120), np.arange(160)) * 5 % 3000 + rng.integers(0, 6, (120, 160))).astype(np.uint16)[None],
        "sky in floating point": rng.normal(0.1, 0.002, (1, 90, 110)).astype(np.float32),
        "one value": np.full((1, 70, 90), 513, np.uint16),
        # a period longer than the 65535 bytes a match may lie back: nothing to find, and nothing must be found
        "a long period": np.tile(rng.integers(0, 255, 70001, dtype=np.uint8), 3)[:400 * 500].reshape(1, 400, 500),
        "a short period": np.tile(np.arange(251, dtype=np.uint8), 800)[:300 * 600].reshape(1, 300, 600),
        "runs between noise": np.concatenate([rng.integers(0, 255, 30000, dtype=np.uint8), np.zeros(30000, np.uint8),
                                              rng.integers(0, 255, 30000, dtype=np.uint8), np.full(30000, 9, np.uint8)]).reshape(1, 300, 400),
        "colour": image(np.uint16, (3, 60, 80)),
        "64 bit floating point": image(np.float64, (1, 40, 50)),
    }
    for name, a in arrays.items():
        for codec, word in ((CODEC_LZ4, "lz4"), (CODEC_LZ4HC, "lz4hc")):
            for shuffle in (1, 0):
                label = f"{word}{'' if shuffle else ' without byte shuffling'}, {name}"
                check(write_images(path, [a], codec=codec, shuffle=shuffle) == OK, f"write {label}: {err()}")
                x, d = xisf_read(path)
                meta = x.get_images_metadata()[0]
                check(same(d, a), f"{label}: the xisf package reads the array")
                with Opened(path) as f:
                    check(same(f.read(), a), f"{label}: and so does the library")
                if name == "a long period":
                    check("compression" not in meta or len(stored(meta)) > a.nbytes * 0.9, f"{label}: nothing to gain")
                if "compression" not in meta:
                    check(name in ("a long period", "sky in floating point", "64 bit floating point") or not shuffle,
                          f"{label}: is compressed")
                    continue
                shuffled = shuffle and a.itemsize > 1
                check(meta["compression"] == (word + ("+sh" if shuffled else ""), a.nbytes, a.itemsize if shuffled else None),
                      f"{label}: the compression attribute: {meta['compression']}")
                check(lz4_decoded(meta) == a.tobytes(), f"{label}: the lz4 library decodes the block")
                check(len(stored(meta)) < a.nbytes, f"{label}: the block is smaller")
    check(write_images(path, [rng.integers(0, 65535, (1, 50, 60)).astype(np.uint16)], codec=CODEC_LZ4HC) == OK and
          "compression" not in XISF(path).get_images_metadata()[0], "noise is stored as it is")
    # every size around what the format treats specially: a block of less than 13 bytes has no match
    for n in list(range(1, 42)) + [63, 64, 65, 255, 256, 270, 271, 4096, 65535, 65536, 65537, 65550]:
        a = ((np.arange(n) // 5) % 3).astype(np.uint8).reshape(1, 1, n)
        for codec in (CODEC_LZ4, CODEC_LZ4HC):
            ok = write_images(path, [a], codec=codec) == OK
            x, d = xisf_read(path) if ok else (None, None)
            meta = x.get_images_metadata()[0] if ok else {}
            check(ok and same(d, a) and ("compression" not in meta or lz4_decoded(meta) == a.tobytes()),
                  f"a row of {n} bytes, codec {codec}")
            if n >= 64:
                check("compression" in meta, f"a row of {n} bytes is compressed, codec {codec}")

    # subblocks: each one a block of its own
    a = arrays["smooth 16 bit"]
    for codec, word in ((CODEC_LZ4, "lz4"), (CODEC_LZ4HC, "lz4hc")):
        check(write_images(path, [a], codec=codec, subblock_size=5000) == OK, f"{word} in subblocks: {err()}")
        meta = XISF(path).get_images_metadata()[0]
        check(len(meta.get("subblocks", "").split(":")) == -(-a.nbytes // 5000) and lz4_decoded(meta) == a.tobytes(),
              f"{word}: the subblocks decode one by one")
        with Opened(path) as f:
            check(same(f.read(), a), f"{word} in subblocks: the library reads them")

    # compression levels
    sizes = {}
    for level in range(1, 13):
        check(write_images(path, [a], codec=CODEC_LZ4HC, compression_level=level) == OK, f"lz4hc level {level}: {err()}")
        x = XISF(path)
        meta = x.get_images_metadata()[0]
        sizes[level] = len(stored(meta))
        check(lz4_decoded(meta) == a.tobytes() and "XISF:CompressionLevel" not in x.get_file_metadata(),
              f"lz4hc level {level}: decodes (and XISF:CompressionLevel, which is no level of a codec, is not written)")
    check(sizes[12] <= sizes[9] <= sizes[4] <= sizes[1] and sizes[12] < sizes[1], f"more searching, smaller blocks: {sizes}")
    check(write_images(path, [a], codec=CODEC_LZ4HC) == OK and len(stored(XISF(path).get_images_metadata()[0])) == sizes[9],
          "level 9 is the usual one of lz4hc")
    for codec, levels, decode in ((CODEC_ZLIB, (1, 6, 9), zlib.decompress), (CODEC_ZSTD, (1, 3, 19), None)):
        if codec == CODEC_ZSTD and not codec_available(CODEC_ZSTD, 1):
            continue
        got = []
        for level in levels:
            check(write_images(path, [a], codec=codec, compression_level=level) == OK, f"codec {codec} level {level}: {err()}")
            x, d = xisf_read(path)
            meta = x.get_images_metadata()[0]
            got.append(len(stored(meta)))
            check(same(d, a) and (decode is None or unshuffled(decode(stored(meta)), 2) == a.tobytes()), f"codec {codec} level {level} decodes")
        check(got[2] == min(got) and got[2] < got[0], f"codec {codec}: the highest level makes the smallest block: {got}")
        check(write_images(path, [a], codec=codec) == OK and len(stored(XISF(path).get_images_metadata()[0])) == got[1],
              f"codec {codec}: the usual level is {levels[1]}")
    check(write_images(path, [a], codec=CODEC_LZ4HC, compression_level=13) == ERR_ARGUMENT and "1 to 12" in err() and
          write_images(path, [a], codec=CODEC_ZLIB, compression_level=10) == ERR_ARGUMENT and
          write_images(path, [a], codec=CODEC_ZLIB, compression_level=-2) == ERR_ARGUMENT, "levels a codec does not have")
    check(write_images(path, [a], codec=CODEC_LZ4, compression_level=1) == ERR_ARGUMENT and "no compression levels" in err(),
          "lz4 has no levels")
    check(write_images(path, [a], compression_level=5) == ERR_ARGUMENT, "a level without a codec")
    fits_path = os.path.join(TMP, "lz4.fits")
    check(write_images(fits_path, [a], codec=CODEC_LZ4) == ERR_ARGUMENT and "LZ4" in err() and not os.path.exists(fits_path) and
          write_images(os.path.join(TMP, "lz4.asdf"), [a], codec=CODEC_LZ4HC) == ERR_ARGUMENT and "XISF only" in err(),
          f"LZ4 is for XISF: {err()}")
    check(write_images(fits_path, [a], compression_level=5, shuffle=0) == OK and same(np.array(fits.getdata(fits_path)), a[0][::-1]),
          "what only an XISF file has means nothing to FITS")


def test_writer_arguments():
    a = image(np.uint16, (1, 6, 7))
    path = os.path.join(TMP, "args.xisf")
    check(write_images(path, [a]) == OK, "write")
    check(write_images(path, [a], overwrite=0) == ERR_EXISTS and "already exists" in err(), "an existing file is not overwritten")
    check(write_images(os.path.join(TMP, "no", "such", "dir.xisf"), [a]) == ERR_IO, "a directory that is not there")
    check(not os.path.exists(os.path.join(TMP, "args.xisf.part")), "no temporary file is left")
    check(write_images(os.path.join(TMP, "x.jpeg"), [a]) == ERR_ARGUMENT, "an extension that says nothing")
    check(write_images(os.path.join(TMP, "x.jpeg"), [a], format=FORMAT_TIFF) == OK and
          same(tifffile.imread(os.path.join(TMP, "x.jpeg")), a[0]), "the format can be stated")
    check(write_images(path, [a], codec=77) == ERR_ARGUMENT and write_images(path, [a], checksum=77) == ERR_ARGUMENT and
          write_images(path, [a], row_order=9) == ERR_ARGUMENT and write_images(path, [a], subblock_size=0) == ERR_ARGUMENT,
          "options out of range")
    check(write_images(path, [a.astype(np.float32)], bounds=(1.0, 1.0)) == ERR_ARGUMENT, "empty bounds")
    # a name with characters outside ASCII (on Windows this goes through the wide-character API)
    odd = os.path.join(TMP, "Messier 31 – Andrómeda 星.xisf")
    check(write_images(odd, [a], codec=CODEC_ZLIB) == OK and os.path.exists(odd), f"a file name beyond ASCII: {err()}")
    with Opened(odd) as f:
        check(same(f.read(), a), "and it is read back")
    out = odd[:-5] + ".fits"
    co = ConvertOptions()
    convert_options_init(C.byref(co), C.sizeof(co))
    check(convert(ctx, enc(odd), enc(out), C.byref(co)) == OK and os.path.exists(out) and
          same(np.array(fits.getdata(out))[None], a[:, ::-1]), f"and converted: {err()}")


# ---------------------------------------------------------------------------------------------
# Reading files that other software wrote
# ---------------------------------------------------------------------------------------------

def test_read_fits():
    path = os.path.join(TMP, "r.fits")
    rng = np.random.default_rng(5)
    cases = [("uint8", (rng.random((20, 30)) * 255).astype(np.uint8), np.uint8),
             ("int16 without negative values", (rng.random((20, 30)) * 30000).astype(np.int16), np.uint16),
             ("int16 with negative values", (rng.random((20, 30)) * 60000 - 30000).astype(np.int16), np.float32),
             ("uint16", (rng.random((20, 30)) * 65535).astype(np.uint16), np.uint16),
             ("int32 with negative values", (rng.random((3, 20, 30)) * 2e9 - 1e9).astype(np.int32), np.float64),
             ("uint32", (rng.random((20, 30)) * 4e9).astype(np.uint32), np.uint32),
             ("float32", rng.random((3, 20, 30)).astype(np.float32), np.float32),
             ("float64", (rng.random((20, 30)) * 5000 - 20), np.float64)]
    for label, a, expected in cases:
        h = fits.PrimaryHDU(a)
        h.header["OBJECT"] = "NGC 7000"
        h.writeto(path, overwrite=True)
        ref = np.array(fits.getdata(path))
        ref = ref if ref.ndim == 3 else ref[None]
        with Opened(path) as f:
            before = f.info()
            check(before.data_known == 0 and before.width == a.shape[-1] and before.height == a.shape[-2] and
                  before.row_order == ROWS_BOTTOM_UP and before.row_order_declared == 0,
                  f"FITS {label}: geometry before the pixels are loaded")
            d = f.read()
            after = f.info()
            check(isinstance(d, np.ndarray) and d.dtype == expected and after.data_known == 1 and DTYPE[after.sample_format] == expected,
                  f"FITS {label}: read as {np.dtype(expected).name}")
            check(isinstance(d, np.ndarray) and np.array_equal(d.astype(np.float64), ref.astype(np.float64)),
                  f"FITS {label}: the values astropy reads, in the stored order")
            check(same(f.read(rows=ROWS_TOP_DOWN), d[:, ::-1, :]), f"FITS {label}: top-down on request")
            check(("OBJECT", "'NGC 7000'") in [(n, v.strip()) for n, v, c in f.cards()], f"FITS {label}: keywords")
            if expected == np.float64 and label == "float64":
                check(abs(after.lower_bound - ref.min()) < 1e-9 and abs(after.upper_bound - ref.max()) < 1e-9, "FITS: bounds of floats beyond 0..65535")
                as16 = f.read(dtype=np.uint16)
                want = np.round((ref - ref.min()) / (ref.max() - ref.min()) * 65535)
                check(isinstance(as16, np.ndarray) and np.abs(as16.astype(np.float64) - want).max() <= 1, "FITS: floats to 16 bits over their range")
                as16 = f.read(dtype=np.uint16, bounds=(0.0, 10000.0))
                want = np.round(np.clip(ref / 10000.0, 0, 1) * 65535)
                check(isinstance(as16, np.ndarray) and np.abs(as16.astype(np.float64) - want).max() <= 1, "FITS: or over stated bounds")
            if label == "uint16":
                as8 = f.read(dtype=np.uint8)
                check(isinstance(as8, np.ndarray) and np.abs(as8.astype(np.float64) - ref / 257.0).max() <= 0.5 + 1e-9, "FITS: 16 to 8 bits")
                asf = f.read(dtype=np.float32)
                check(isinstance(asf, np.ndarray) and np.allclose(asf, ref / 65535.0, atol=1e-7), "FITS: integers to [0,1]")
    # several HDUs, a table in between, ROWORDER, a tile-compressed image
    a, b, c = (rng.random((10, 12)) * 1000).astype(np.uint16), rng.random((3, 6, 8)).astype(np.float32), (rng.random((16, 20)) * 900).astype(np.int16)
    second = fits.ImageHDU(b, name="RGB")
    second.header["ROWORDER"] = "TOP-DOWN"
    table = fits.BinTableHDU.from_columns([fits.Column(name="x", format="E", array=np.arange(4, dtype=np.float32))])
    fits.HDUList([fits.PrimaryHDU(a), table, second, fits.CompImageHDU(c, compression_type="RICE_1")]).writeto(path, overwrite=True)
    with Opened(path) as f:
        check(image_count(f.handle) == 3 and lib.xisfconv_skipped_count(C.c_void_p(f.handle.value)) == 1, "FITS: three images, one table skipped")
        i1, i2 = f.info(1), f.info(2)
        check(image_name(f.handle, 1) == b"RGB" and i1.row_order == ROWS_TOP_DOWN and i1.row_order_declared == 1 and i1.source_index == 2,
              "FITS: name, ROWORDER and HDU number of the second image")
        check(f.detail("tileCompression", 2) == "RICE_1" and i2.source_index == 3, "FITS: the third image is tile-compressed")
        d2 = f.read(2)                      # out of order: each image is read on its own
        d0 = f.read(0)
        d1 = f.read(1, rows=ROWS_BOTTOM_UP)
        check(same(d0, a[None]) and same(d1, b[:, ::-1, :]) and same(d2, c.astype(np.uint16)[None]), "FITS: each image, read in any order")
        check(f.read(3) == ERR_INDEX, "FITS: no fourth image")


def test_read_xisf():
    path = os.path.join(TMP, "r.xisf")
    for dtype in (np.uint8, np.uint16, np.uint32, np.float32, np.float64):
        for channels in (1, 3):
            a = np.moveaxis(image(dtype, (channels, 15, 22)), 0, -1)   # the xisf package wants channels last
            for codec in (None, "zlib", "lz4", "lz4hc", "zstd"):
                XISF.write(path, a, codec=codec, shuffle=codec is not None, xisf_metadata={})
                with Opened(path) as f:
                    i = f.info()
                    d = f.read()
                    check(same(d, np.moveaxis(a, -1, 0)) and i.data_known == 1 and DTYPE[i.sample_format] == dtype and
                          i.row_order == ROWS_TOP_DOWN and i.color_space == (1 if channels == 3 else 0),
                          f"XISF {np.dtype(dtype).name} x{channels} {codec}: the array the xisf package wrote")
                    if "compression" in XISF(path).get_images_metadata()[0]:   # the package stores small blocks plain
                        check(f.detail("compression").startswith(codec), f"XISF {codec}: the compression attribute")
                    check(same(f.read(rows=ROWS_BOTTOM_UP), np.moveaxis(a, -1, 0)[:, ::-1, :]), "XISF: bottom-up on request")


def test_smaller_pictures():
    """bin, fit_width/fit_height and scale of the conversion options, against the mean taken with NumPy."""
    rng = np.random.default_rng(41)
    h, w = 24, 36
    for dtype in (np.uint16, np.float32):
        a = (rng.random((h, w, 3)) * (60000 if dtype == np.uint16 else 1)).astype(dtype)
        src = os.path.join(TMP, "full.xisf")
        XISF.write(src, a, xisf_metadata={})

        def picture(target="small.tif", **options):
            co = ConvertOptions()
            convert_options_init(C.byref(co), C.sizeof(co))
            co.overwrite = 1
            for key, value in options.items():
                setattr(co, key, value)
            st = convert(ctx, enc(src), enc(os.path.join(TMP, target)), C.byref(co))
            return st, (tifffile.imread(os.path.join(TMP, target)) if st == OK and target.endswith(".tif") else None)

        def mean(n, m):   # n x m pixels become one
            v = a.astype(np.float64).reshape(h // n, n, w // m, m, 3).mean(axis=(1, 3))
            return np.floor(v + 0.5).astype(dtype) if dtype == np.uint16 else v.astype(dtype)

        name = np.dtype(dtype).name
        st, got = picture(bin=2)
        check(st == OK and got.dtype == dtype and np.array_equal(got, mean(2, 2)), f"{name}: bin = 2 is the mean of 2 x 2 pixels: {err()}")
        st, got = picture(bin=4)
        check(st == OK and np.array_equal(got, mean(4, 4)), f"{name}: bin = 4")
        st, got = picture(scale=0.25)
        check(st == OK and np.array_equal(got, mean(4, 4)), f"{name}: scale = 0.25 is the same picture")
        st, got = picture(fit_width=12, fit_height=1000)
        check(st == OK and np.array_equal(got, mean(3, 3)), f"{name}: fit_width = 12 makes 36 x 24 pixels 12 x 8")
        st, got = picture(fit_width=1000, fit_height=6)
        check(st == OK and np.array_equal(got, mean(4, 4)), f"{name}: fit_height = 6 makes them 9 x 6")
        st, got = picture(fit_width=18, fit_height=6, scale=0.5, bin=2)
        check(st == OK and got.shape == (6, 9, 3), f"{name}: all of them together: the smallest ({None if got is None else got.shape})")
        st, got = picture(fit_width=500, fit_height=500, scale=1.0, bin=1)
        check(st == OK and np.array_equal(got, a), f"{name}: nothing that asks for a smaller picture leaves the image as it is")
        st, got = picture(reserved=12345, reserved2=-7)
        check(st == OK and np.array_equal(got, a), f"{name}: the reserved fields are not looked at")
        st, _ = picture(target="small.fits", scale=1.0)
        check(st == ERR_ARGUMENT, f"{name}: scale = 1 asks for a picture too, which FITS is not")
        for target in ("small.fits", "small.asdf", "small.fits.fz"):
            st, _ = picture(target=target, bin=2)
            check(st == ERR_ARGUMENT and "TIFF and PNG" in err() and not os.path.exists(os.path.join(TMP, target)),
                  f"{name}: bin = 2 for {target} is an argument error: {err()}")
        for options in ({"bin": 0}, {"bin": -1}, {"scale": 1.01}, {"scale": -0.1}, {"scale": float("nan")}):
            st, _ = picture(**options)
            check(st == ERR_ARGUMENT, f"{name}: {options} is an argument error: {err()}")
    # a stretched picture: PNG, 8 bits
    co = ConvertOptions()
    convert_options_init(C.byref(co), C.sizeof(co))
    co.overwrite, co.fit_width, co.fit_height, co.stretch, co.sample_format = 1, 9, 9, 1, UINT8
    check(convert(ctx, enc(src), enc(os.path.join(TMP, "thumb.png")), C.byref(co)) == OK, f"a thumbnail: {err()}")
    with Image.open(os.path.join(TMP, "thumb.png")) as picture:
        check(picture.size == (9, 6) and picture.mode == "RGB", f"PNG, 9 x 6 pixels: {picture.size} {picture.mode}")


def properties_of(f, image=0):
    """[(id, type, value, comment)] through the library: the value is text, or an array for vectors and matrices."""
    out = []
    for n in range(property_count(f.handle, image)):
        pid, kind, value, comment, block = text(), text(), text(), text(), i32()
        assert property_get(f.handle, image, n, C.byref(pid), C.byref(kind), C.byref(value), C.byref(comment), C.byref(block)) == OK
        entry = value.value.decode()
        if block.value:
            rows, columns = size_t(), size_t()
            st = property_read_f64(f.handle, image, pid.value, None, 0, C.byref(rows), C.byref(columns))
            if st == OK:
                entry = np.empty((rows.value, columns.value))
                assert property_read_f64(f.handle, image, pid.value, entry.ctypes.data_as(C.POINTER(f64)), entry.size,
                                         C.byref(rows), C.byref(columns)) == OK
            else:
                entry = None
        out.append((pid.value.decode(), kind.value.decode(), entry, comment.value.decode()))
    return out


def test_carried_properties():
    """The XISF properties of a file the xisf package wrote, through FITS and ASDF and back."""
    rng = np.random.default_rng(31)
    a = (rng.random((9, 14, 1)) * 60000).astype(np.uint16)
    vector, matrix = rng.normal(size=5), rng.normal(size=(3, 4))
    props = {
        "Instrument:ExposureTime": {"id": "Instrument:ExposureTime", "type": "Float32", "value": 180.5},
        "Observation:Object:Name": {"id": "Observation:Object:Name", "type": "String", "value": "NGC 7000 \u2604 <north> & co"},
        "Instrument:Camera:Gain": {"id": "Instrument:Camera:Gain", "type": "Int32", "value": 120},
        "Lab:Vector": {"id": "Lab:Vector", "type": "F64Vector", "value": vector},
        "Lab:Matrix": {"id": "Lab:Matrix", "type": "F64Matrix", "value": matrix},
        "Lab:Counts": {"id": "Lab:Counts", "type": "UI16Vector", "value": np.array([1, 2, 65535], np.uint16)},
        "Lab:Flag": {"id": "Lab:Flag", "type": "Boolean", "value": True},
    }
    src = os.path.join(TMP, "carried.xisf")
    XISF.write(src, a, image_metadata={"XISFProperties": props},
               xisf_metadata={"Note:Author": {"id": "Note:Author", "type": "String", "value": "somebody"}})
    with Opened(src) as f:
        first, first_file = properties_of(f), properties_of(f, FILE_PROPERTIES)
    by_id = {p[0]: p for p in first}
    check(len(first) == len(props) and np.array_equal(by_id["Lab:Matrix"][2], matrix) and by_id["Lab:Flag"][1] == "Boolean",
          f"the properties the xisf package wrote: {[p[0] for p in first]}")
    note = [p for p in first_file if p[0] == "Note:Author"]

    def same_properties(a, b):
        # (a Boolean by its value: the xisf package writes True, which an ASDF tree holds as true)
        return len(a) == len(b) and all(x[0] == y[0] and x[1] == y[1] and x[3] == y[3] and (
            np.array_equal(x[2], y[2]) if isinstance(x[2], np.ndarray) else
            x[2].lower() == y[2].lower() if x[1] == "Boolean" else x[2] == y[2]) for x, y in zip(a, b))

    co = ConvertOptions()
    convert_options_init(C.byref(co), C.sizeof(co))
    check(co.properties == 1, "properties are taken along by default")
    co.overwrite = 1
    for kind, codec in (("fits", CODEC_NONE), ("fits", CODEC_DEFAULT), ("asdf", CODEC_NONE), ("asdf", CODEC_ZLIB)):
        label = f"{kind}{' compressed' if codec else ''}"
        mid, back = os.path.join(TMP, "carried." + kind), os.path.join(TMP, "carried_back.xisf")
        co.codec, co.properties = codec, 1
        check(convert(ctx, enc(src), enc(mid), C.byref(co)) == OK, f"XISF -> {label}: {err()}")
        with Opened(mid) as f:
            check(same_properties(properties_of(f), first) and same_properties(properties_of(f, FILE_PROPERTIES), note),
                  f"{label}: the library reads the properties the file carries, those of the image and of the file")
        if kind == "fits":
            with fits.open(mid, memmap=False) as hdul:   # (not mapped: the file is written again below)
                table = hdul["XISF_PROPERTIES"].data
                row = {r["ID"]: r for r in table}["Lab:Matrix"]
                check(row["TYPE"] == "F64Matrix" and row["BLOCK"] and (row["ROWS"], row["COLUMNS"]) == (3, 4) and
                      np.array_equal(np.frombuffer(bytes(np.asarray(row["VALUE"], np.uint8)), "<f8").reshape(3, 4), matrix),
                      f"{label}: astropy reads the matrix from the table")
                name = {r["ID"]: r for r in table}["Observation:Object:Name"]
                check(bytes(np.asarray(name["VALUE"], np.uint8)).decode() == "NGC 7000 \u2604 <north> & co" and not name["BLOCK"],
                      f"{label}: and the text, as UTF-8")
                check([r["ID"] for r in hdul["XISF_METADATA"].data] == ["Note:Author"], f"{label}: the property of the file has its own table")
        elif HAVE_ASDF:
            with asdf.open(mid, memmap=False) as af:
                tree = af["xisf"]["images"][0]["properties"]
                check(np.array_equal(np.asarray(tree["Lab:Matrix"]["value"]), matrix) and tree["Lab:Flag"]["value"] is True and
                      tree["Instrument:Camera:Gain"]["value"] == 120 and tree["Observation:Object:Name"]["value"] == "NGC 7000 \u2604 <north> & co" and
                      np.asarray(tree["Lab:Counts"]["value"]).dtype == np.uint16 and af["xisf"]["metadata"]["Note:Author"]["value"] == "somebody",
                      f"{label}: the asdf library reads them from the tree")
        co.codec = CODEC_NONE
        check(convert(ctx, enc(mid), enc(back), C.byref(co)) == OK, f"{label} -> XISF: {err()}")
        with Opened(back) as f:
            check(same_properties(properties_of(f), first) and
                  same_properties([p for p in properties_of(f, FILE_PROPERTIES) if p[0] == "Note:Author"], note),
                  f"{label} -> XISF: they are the properties of the XISF file again")
        theirs = XISF(back).get_images_metadata()[0]["XISFProperties"]
        check(list(theirs) == list(props) and np.array_equal(theirs["Lab:Matrix"]["value"], matrix) and
              np.array_equal(theirs["Lab:Counts"]["value"], [1, 2, 65535]) and theirs["Lab:Counts"]["value"].dtype == np.uint16 and
              theirs["Observation:Object:Name"]["value"] == "NGC 7000 \u2604 <north> & co" and theirs["Instrument:Camera:Gain"]["value"] == 120,
              f"{label} -> XISF: and the xisf package reads them as it read the first file")
        co.properties = 0
        check(convert(ctx, enc(mid), enc(back), C.byref(co)) == OK, f"{label} -> XISF without properties")
        with Opened(back) as f:
            check(properties_of(f) == [] and not [p for p in properties_of(f, FILE_PROPERTIES) if p[0] == "Note:Author"],
                  f"{label} -> XISF: properties = 0 leaves them where they are")
        check(convert(ctx, enc(src), enc(mid), C.byref(co)) == OK, f"XISF -> {label} without properties")
        with Opened(mid) as f:
            check(property_count(f.handle, 0) == 0 and property_count(f.handle, FILE_PROPERTIES) == 0,
                  f"XISF -> {label}: properties = 0 leaves them out")


# ---------------------------------------------------------------------------------------------
# Astrometry, stretch
# ---------------------------------------------------------------------------------------------

WCS_CARDS = [("CTYPE1", "RA---TAN", None), ("CTYPE2", "DEC--TAN", None), ("CRVAL1", 10.684, None), ("CRVAL2", 41.269, None),
             ("CRPIX1", 60.5, None), ("CRPIX2", 30.25, None), ("CD1_1", -2.8e-4, None), ("CD1_2", 1.1e-5, None),
             ("CD2_1", 1.2e-5, None), ("CD2_2", 2.8e-4, None), ("RADESYS", "ICRS", None), ("OBJECT", "M 31", None)]


def wcs_of(f, rows, image=0):
    kw, summary = ptr(), text()
    st = wcs_keywords(f.handle, image, rows, 3, C.byref(kw), C.byref(summary))
    if st != OK:
        return st, None
    cards = {n: v for n, v, c in cards_of(kw)}
    keywords_free(kw)
    return st, cards


def test_wcs():
    from astropy.wcs import WCS
    h, w = 80, 120
    a = image(np.float32, (1, h, w))
    path = os.path.join(TMP, "wcs.xisf")
    # The keywords describe the buffer as handed over: top-down rows, CRPIX2 counted from the top.
    check(write_images(path, [a], cards=WCS_CARDS) == OK, f"write XISF with WCS keywords: {err()}")
    with Opened(path) as f:
        check(f.info().has_astrometric_solution == 1 and property_find(f.handle, 0, b"PCL:AstrometricSolution:ProjectionSystem") >= 0,
              "XISF: PixInsight's solution properties are written next to the keywords")
        st, top = wcs_of(f, ROWS_TOP_DOWN)
        check(st == OK and abs(float(top["CRPIX2"]) - 30.25) < 1e-9 and abs(float(top["CD2_2"]) - 2.8e-4) < 1e-12 and "OBJECT" not in top,
              f"WCS for top-down rows is what was handed in: {top}")
        st, bottom = wcs_of(f, ROWS_DEFAULT)
        check(st == OK and abs(float(bottom["CRPIX2"]) - (h + 1 - 30.25)) < 1e-9 and abs(float(bottom["CD2_2"]) + 2.8e-4) < 1e-12,
              "WCS for bottom-up rows: CRPIX2 and the second column of CD are mirrored")
    # The same sky position for the same pixel, through astropy: FITS written from the same buffer.
    fpath = os.path.join(TMP, "wcs.fits")
    check(write_images(fpath, [a], cards=WCS_CARDS) == OK, "write FITS with WCS keywords")
    with fits.open(fpath) as hd:
        stored = WCS(hd[0].header)
        given = WCS(fits.Header([(n, v) for n, v, c in WCS_CARDS if n != "OBJECT"]))
        x, y = 17.0, 5.0                                   # in the top-down buffer, 0-based
        sky_given = given.pixel_to_world_values(x, y)
        sky_stored = stored.pixel_to_world_values(x, h - 1 - y)   # the same pixel in the bottom-up file
        check(np.allclose(sky_given, sky_stored, atol=1e-10), "FITS: the stored WCS points every pixel at the same sky")
    # Without the keywords, the solution properties alone give the WCS back.
    raw = open(path, "rb").read()
    hidden = os.path.join(TMP, "wcs_hidden.xisf")
    for name in ("CTYPE", "CRVAL", "CRPIX", "CD1_", "CD2_", "RADESYS"):
        raw = raw.replace(b'name="' + name.encode(), b'name="' + b"X" + name.encode()[1:])
    open(hidden, "wb").write(raw)
    with Opened(hidden) as f:
        check("CTYPE1" not in [n for n, v, c in f.cards()] and f.info().has_astrometric_solution == 1, "a solution without WCS keywords")
        st, sol = wcs_of(f, ROWS_TOP_DOWN)
        check(st == OK and sol["CTYPE1"].strip("' ") == "RA---TAN", f"WCS keywords from the solution properties: {sol}")
        if st == OK:
            back = WCS(fits.Header([(n, float(v) if n[:2] in ("CR", "CD") else v.strip("' ")) for n, v in sol.items()
                                    if n[:5] in ("CTYPE", "CRVAL", "CRPIX") or n[:2] == "CD"]))
            check(np.allclose(back.pixel_to_world_values(17.0, 5.0), sky_given, atol=1e-7), "which point at the same sky")
    with Opened(fpath) as f:
        check(wcs_keywords(f.handle, 0, ROWS_DEFAULT, 9, C.byref(ptr()), None) == ERR_ARGUMENT, "a SIP order out of range")
    plain = os.path.join(TMP, "nowcs.fits")
    write_images(plain, [a])
    with Opened(plain) as f:
        check(f.info().has_astrometric_solution == 0 and wcs_of(f, ROWS_DEFAULT)[0] == ERR_NOT_FOUND, "no solution, no WCS")


def test_stretch():
    rng = np.random.default_rng(3)
    a = (rng.normal(0.02, 0.004, (3, 60, 80)) + np.linspace(0, 0.3, 80)[None, None, :] ** 3).clip(0, 1).astype(np.float32)
    for linked in (1, 0):
        params = (StretchParams * 3)()
        check(auto_stretch(ctx, a.ctypes.data, 80, 60, 3, FLOAT32, 0.0, 1.0, 3, linked, params) == OK, f"auto-stretch: {err()}")
        if linked:
            check(all(abs(params[k].midtones - params[0].midtones) < 1e-12 for k in range(3)), "linked: one set of parameters")
        out = np.empty_like(a)
        check(apply_stretch(ctx, a.ctypes.data, 80, 60, 3, FLOAT32, 0.0, 1.0, params, 3, out.ctypes.data) == OK, "apply the stretch")
        check(out.min() >= 0 and out.max() <= 1 and 0.15 < float(np.median(out)) < 0.35, "the background ends near 0.25")
        if EXE:
            # the tool's --stretch does the same to the same data
            src, tif = os.path.join(TMP, "s.xisf"), os.path.join(TMP, "s.tif")
            write_images(src, [a])
            r = subprocess.run([EXE, src, "-o", tif, "-f", "-q", "-b", "f32", "--stretch=" + ("linked" if linked else "unlinked")],
                               capture_output=True, text=True)
            check(r.returncode == 0 and np.allclose(np.moveaxis(tifffile.imread(tif), -1, 0), out, atol=2e-6),
                  f"the tool's --stretch={'linked' if linked else 'unlinked'} gives the same pixels")
    # integers use their full range
    b = (a * 65535).astype(np.uint16)
    params = (StretchParams * 3)()
    out = np.empty(b.shape, np.float32)
    check(auto_stretch(ctx, b.ctypes.data, 80, 60, 3, UINT16, 0.0, 1.0, 3, 1, params) == OK and
          apply_stretch(ctx, b.ctypes.data, 80, 60, 3, UINT16, 0.0, 1.0, params, 3, out.ctypes.data) == OK and
          0.15 < float(np.median(out)) < 0.35, "stretch of 16-bit data")


# ---------------------------------------------------------------------------------------------
# Callbacks, threads, silence
# ---------------------------------------------------------------------------------------------

def test_progress_and_cancel():
    src, out = os.path.join(TMP, "p.xisf"), os.path.join(TMP, "p.fits")
    write_images(src, [image(np.uint16, (1, 50, 60)), image(np.uint16, (1, 50, 60), 1)], codec=CODEC_ZLIB)
    stages = []
    handler = PROGRESS_FN(lambda user, stage, done, total: stages.append((stage, done, total)) or 0)
    set_progress_handler(ctx, handler, None)
    co = ConvertOptions()
    convert_options_init(C.byref(co), C.sizeof(co))
    co.overwrite = 1
    check(convert(ctx, enc(src), enc(out), C.byref(co)) == OK and [s for s, d, t in stages] == [b"reading", b"reading", b"writing"] and
          stages[1][1:] == (1, 2), f"progress of a conversion: {stages}")
    stop = PROGRESS_FN(lambda user, stage, done, total: 1 if stage == b"writing" else 0)
    set_progress_handler(ctx, stop, None)
    os.remove(out)
    check(convert(ctx, enc(src), enc(out), C.byref(co)) == ERR_CANCELLED and not os.path.exists(out) and
          not os.path.exists(out + ".part") and err() == "cancelled", "cancelled before writing: nothing is left")
    report = ptr()
    stop_all = PROGRESS_FN(lambda user, stage, done, total: 1)
    set_progress_handler(ctx, stop_all, None)
    check(verify(ctx, enc(src), C.byref(report)) == ERR_CANCELLED and not report.value, "verification can be cancelled")
    set_progress_handler(ctx, PROGRESS_FN(0), None)
    check(verify(ctx, enc(src), C.byref(report)) == OK and report_verdict(report) == 0, "and runs through without a handler")
    report_free(report)


def test_kept_messages_and_cancel_from_another_thread():
    """What a caller needs who cannot be called back in the thread that works (Python and its
    signal handlers): the messages afterwards, and a way to stop the work from outside."""
    context = context_new()
    keep_messages(context, 1)
    src, out = os.path.join(TMP, "kept.xisf"), os.path.join(TMP, "kept.fits")
    check(write_images(src, [image(np.float32, (1, 40, 50))], context=context, checksum=SHA3_256) == OK, "write, keeping messages")
    level, path, message = i32(), text(), text()
    check(message_count(context) == 1 and message_get(context, 0, C.byref(level), C.byref(path), C.byref(message)) == OK and
          level.value == 1 and path.value == enc(src) and b"PixInsight" in message.value, "the warning is kept with its file")
    co = ConvertOptions()
    convert_options_init(C.byref(co), C.sizeof(co))
    co.overwrite = 1
    back = os.path.join(TMP, "kept-back.xisf")
    check(convert(context, enc(src), enc(out), C.byref(co)) == OK and convert(context, enc(out), enc(back), C.byref(co)) == OK and
          message_count(context) >= 2, "notes of conversions are added")
    kinds = []
    for index in range(message_count(context)):
        message_get(context, index, C.byref(level), C.byref(path), C.byref(message))
        kinds.append(level.value)
    check(kinds[0] == 1 and set(kinds[1:]) == {2}, f"in order, warnings and notes told apart: {kinds}")
    clear_messages(context)
    check(message_count(context) == 0 and message_get(context, 0, None, None, None) == ERR_INDEX, "cleared")

    # a file with many data blocks, so that the work has many steps
    many = os.path.join(TMP, "many.xisf")
    arrays = [image(np.uint16, (1, 300, 400), k) for k in range(40)]
    check(write_images(many, arrays, context=context, codec=CODEC_ZLIB, checksum=SHA1) == OK, "a file with 40 images")
    clear_messages(context)
    reports = []
    slow = PROGRESS_FN(lambda user, stage, done, total: reports.append(done) or time.sleep(0.005) or 0)
    set_progress_handler(context, slow, None)
    for name, call in (("verify", lambda: verify(context, enc(many), C.byref(report))),
                       ("rewrite", lambda: rewrite(context, enc(many), enc(target), None, None)),
                       ("convert", lambda: convert(context, enc(many), enc(target_fits), C.byref(co)))):
        report, target, target_fits = ptr(), os.path.join(TMP, "many-out.xisf"), os.path.join(TMP, "many-out.fits")
        for leftover in (target, target_fits):
            if os.path.exists(leftover):
                os.remove(leftover)
        del reports[:]
        status = []
        worker = threading.Thread(target=lambda: status.append(call()))
        worker.start()
        while len(reports) < 3 and worker.is_alive():
            time.sleep(0.001)
        context_cancel(context)                       # from this thread, while the other is inside the call
        worker.join()
        check(status == [ERR_CANCELLED] and 3 <= len(reports) < 40 and err(context) == "cancelled",
              f"{name}: stopped from another thread after {len(reports)} steps: {status}")
        check(not report.value and not any(os.path.exists(f) or os.path.exists(f + ".part") for f in (target, target_fits)),
              f"{name}: nothing is left behind")
    # the request was for those calls: the next one runs through, also after a request made between calls
    context_cancel(context)
    set_progress_handler(context, PROGRESS_FN(0), None)
    report = ptr()
    check(verify(context, enc(many), C.byref(report)) == OK and report_verdict(report) == 0, "the next call is not stopped")
    report_free(report)
    # requests from several threads at once while a call runs: harmless
    stop = threading.Event()

    def pester():
        while not stop.is_set():
            context_cancel(context)

    pests = [threading.Thread(target=pester) for _ in range(3)]
    for t in pests:
        t.start()
    outcomes = set()
    for _ in range(20):
        report = ptr()
        outcomes.add(verify(context, enc(many), C.byref(report)))
        if report.value:
            report_free(report)
    stop.set()
    for t in pests:
        t.join()
    check(outcomes <= {OK, ERR_CANCELLED} and ERR_CANCELLED in outcomes, f"cancel requests from three threads during 20 calls: {outcomes}")
    keep_messages(context, 0)
    context_free(context)


def test_threads():
    """Several threads, each with its own context, at the same time: results and messages stay apart."""
    n = 6
    results, messages = [None] * n, [[] for _ in range(n)]

    def work(k):
        context = context_new()
        handler = MESSAGE_FN(lambda user, level, p, m: messages[k].append(p))
        set_message_handler(context, handler, None)
        a = image((np.uint16, np.float32)[k % 2], (1 + 2 * (k % 2), 300, 400), k)
        path = os.path.join(TMP, f"thread{k}.xisf")
        ok = True
        for _ in range(4):
            ok = ok and write_images(path, [a], context=context, codec=CODEC_ZLIB, checksum=SHA3_256) == OK
            with Opened(path, context) as f:
                ok = ok and same(f.read(), a)
            out = os.path.join(TMP, f"thread{k}.fits")
            co = ConvertOptions()
            convert_options_init(C.byref(co), C.sizeof(co))
            co.overwrite = 1
            ok = ok and convert(context, enc(path), enc(out), C.byref(co)) == OK
            d = np.array(fits.getdata(out))
            ok = ok and same(d if d.ndim == 3 else d[None], a[:, ::-1, :])
        results[k] = ok
        context_free(context)

    threads = [threading.Thread(target=work, args=(k,)) for k in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    check(all(results), f"{n} threads with their own contexts: {results}")
    check(all(len(messages[k]) >= 4 and set(messages[k]) == {enc(os.path.join(TMP, f"thread{k}.xisf"))} for k in range(n)),
          "each context hears only about its own files")


def test_distributed_units():
    """Distributed XISF units: the two files as the specification has them, and which files a
    header is followed to, context by context."""
    import struct
    ERR_NOT_ALLOWED, ERR_UNSUPPORTED = 12, 4
    HEADER_DIRECTORY, ANYWHERE, NONE = 0, 1, 2
    d = os.path.join(TMP, "units")
    elsewhere = os.path.join(TMP, "units-elsewhere")
    os.makedirs(d, exist_ok=True)
    os.makedirs(elsewhere, exist_ok=True)

    # written by the library, taken apart here
    for number, (dtype, channels, codec) in enumerate(((np.uint16, 1, CODEC_NONE), (np.float32, 3, CODEC_ZLIB), (np.uint8, 3, CODEC_ZLIB))):
        a = image(dtype, (channels, 21, 34), number)
        if codec != CODEC_NONE:
            a = (np.indices(a.shape).sum(axis=0) % 7).astype(dtype)
        path = os.path.join(d, f"w{number}.xish")
        check(write_images(path, [a], codec=codec) == OK, f"unit {number}: written: {err()}")
        header = open(path, "rb").read().decode()
        raw = open(os.path.join(d, f"w{number}.xisb"), "rb").read()
        m = re.search(r'location="path\(@header_dir/w%d\.xisb\):(0x[0-9a-f]{16})"' % number, header)
        check(header.startswith('<?xml version="1.0" encoding="UTF-8"?>') and m and "attachment" not in header, f"unit {number}: the header file")
        length, reserved, following = struct.unpack_from("<IIQ", raw, 16)
        elements = [struct.unpack_from("<QQQQQ", raw, 32 + 40 * k) for k in range(length)]
        check(raw[:16] == b"XISB0100" + bytes(8) and reserved == 0 and following == 0 and length == len(elements) >= 1,
              f"unit {number}: the data blocks file begins with its signature and an index of one node")
        by_id = {e[0]: e for e in elements}
        e = by_id.get(int(m.group(1), 16)) if m else None
        check(e is not None and e[4] == 0 and len(by_id) == len(elements), f"unit {number}: the block the header names is in the index")
        if e:
            stored = raw[e[1]:e[1] + e[2]]
            compression = re.search(r'compression="([^"]*)"', header)
            if compression:
                name, size = compression.group(1).split(":")[:2]
                plain = zlib.decompress(stored)
                if name.endswith("+sh"):
                    item = int(compression.group(1).split(":")[2])
                    plain = np.frombuffer(plain, np.uint8).reshape(item, -1).T.tobytes()
                check(e[3] == int(size) == len(plain), f"unit {number}: the index has the uncompressed length")
            else:
                plain = stored
                check(e[3] == 0 and codec == CODEC_NONE, f"unit {number}: no uncompressed length for a block that is not compressed")
            check(same(np.frombuffer(plain, np.dtype(dtype).newbyteorder("<")).reshape(a.shape), a), f"unit {number}: the pixels, decoded here")
        with Opened(path) as f:
            listed = [external_file(f.handle, k).decode() for k in range(external_count(f.handle))]
            check(file_detail(f.handle, b"unit") == b"distributed" and len(listed) == 1 and
                  os.path.samefile(listed[0], os.path.join(d, f"w{number}.xisb")) and external_file(f.handle, 1) == b"" and
                  unit_size(f.handle) == len(header.encode()) + len(raw) and file_size(f.handle) == len(header.encode()),
                  f"unit {number}: the file names its data blocks file and the size of the unit")
            check(same(f.read(), a), f"unit {number}: read back")

    # a header that leads out of its directory: each context decides for itself, also at the same time
    pixels = image(np.uint16, (1, 9, 11), 5)
    open(os.path.join(elsewhere, "p.dat"), "wb").write(pixels.astype("<u2").tobytes())
    open(os.path.join(d, "p.dat"), "wb").write(pixels.astype("<u2").tobytes())
    template = ('<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">'
                '<Image geometry="11:9:1" sampleFormat="UInt16" colorSpace="Gray" location="%s"/></xisf>')
    away = os.path.join(d, "away.xish")
    open(away, "w").write(template % ("path(%s)" % os.path.abspath(os.path.join(elsewhere, "p.dat")).replace(os.sep, "/")))
    beside = os.path.join(d, "beside.xish")
    open(beside, "w").write(template % "path(@header_dir/p.dat)")
    network = os.path.join(d, "network.xish")
    open(network, "w").write(template % "url(http://example.com/p.dat)")

    def reads(context, path):
        with Opened(path, context) as f:
            got = f.read()
            return "read" if isinstance(got, np.ndarray) and same(got, pixels) else got

    check(get_external_files(ctx) == HEADER_DIRECTORY and reads(ctx, beside) == "read" and reads(ctx, away) == ERR_NOT_ALLOWED and
          "absolute path" in err(), f"by default a header is followed to its own directory: {err()}")
    check(reads(ctx, network) == ERR_UNSUPPORTED, "and to no network")
    check(set_external_files(ctx, 9) != OK and get_external_files(ctx) == HEADER_DIRECTORY, "another value is refused")
    n = 6
    results = [None] * n
    wanted = [(ANYWHERE, "read", "read"), (HEADER_DIRECTORY, "read", ERR_NOT_ALLOWED), (NONE, ERR_NOT_ALLOWED, ERR_NOT_ALLOWED)]

    def work(k):
        context = context_new()
        policy, beside_expected, away_expected = wanted[k % 3]
        ok = set_external_files(context, policy) == OK and get_external_files(context) == policy
        for _ in range(40):
            ok = ok and reads(context, beside) == beside_expected and reads(context, away) == away_expected
            out = os.path.join(d, f"thread{k}.fits")
            co = ConvertOptions()
            convert_options_init(C.byref(co), C.sizeof(co))
            co.overwrite = 1
            st = convert(context, enc(away), enc(out), C.byref(co))
            ok = ok and st == (OK if away_expected == "read" else ERR_NOT_ALLOWED)
        results[k] = ok
        context_free(context)

    threads = [threading.Thread(target=work, args=(k,)) for k in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    check(all(results), f"{n} threads, each with its own context and its own setting: {results}")
    check(reads(ctx, away) == ERR_NOT_ALLOWED, "and the first context still has its own")
    # the pixels of a file that is not followed to do not come into an output
    out = os.path.join(d, "away.xisf")
    check(rewrite(ctx, enc(away), enc(out), None, None) == ERR_NOT_ALLOWED and not os.path.exists(out) and
          not [name for name in os.listdir(d) if name.endswith(".part")], "a rewrite does not pack what the header may not name")
    check(set_external_files(ctx, ANYWHERE) == OK and rewrite(ctx, enc(away), enc(out), None, None) == OK and
          pixels.astype("<u2").tobytes() in open(out, "rb").read() and set_external_files(ctx, HEADER_DIRECTORY) == OK,
          "unless the context allows it")
    report = ptr()
    check(verify(ctx, enc(away), C.byref(report)) == OK and report_verdict(report) == 1, "a verification calls it not checked")
    report_free(report)


def test_silence():
    """The library prints nothing: a child process does a round of work with stdout and stderr captured."""
    src = os.path.join(TMP, "silent.fits")
    fits.PrimaryHDU((np.arange(600).reshape(20, 30) - 100).astype(np.int16)).writeto(src, overwrite=True)
    code = f"""
import ctypes as C
lib = C.CDLL({LIB_PATH!r})
lib.xisfconv_context_new.restype = C.c_void_p
ctx = C.c_void_p(lib.xisfconv_context_new())
f, r = C.c_void_p(), C.c_void_p()
assert lib.xisfconv_convert(ctx, {enc(src)!r}, {enc(os.path.join(TMP, 'silent.xisf'))!r}, None) == 0
assert lib.xisfconv_convert(ctx, {enc(src)!r}, {enc(os.path.join(TMP, 'silent.png'))!r}, None) == 0
assert lib.xisfconv_convert(ctx, b'/nonexistent/file.xisf', b'/nonexistent/out.fits', None) != 0
assert lib.xisfconv_open(ctx, {enc(src)!r}, C.byref(f)) == 0
assert lib.xisfconv_load_pixels(f, C.c_size_t(0), 1) == 0
assert lib.xisfconv_verify(ctx, {enc(src)!r}, C.byref(r)) == 0
assert lib.xisfconv_verify(ctx, {enc(LIB_PATH)!r}, C.byref(r)) == 0
"""
    r = subprocess.run([sys.executable, "-c", code], capture_output=True)
    check(r.returncode == 0 and r.stdout == b"" and r.stderr == b"", f"nothing on stdout or stderr: {r.stdout[:200]} {r.stderr[:300]}")


# ---------------------------------------------------------------------------------------------
# Files that are odd, damaged or hostile; a host program with another locale
# ---------------------------------------------------------------------------------------------

def patched_fits(path, hdus, old, new):
    fits.HDUList(hdus).writeto(path, overwrite=True)
    raw = open(path, "rb").read()
    assert old in raw, old
    open(path, "wb").write(raw.replace(old, new.ljust(len(old))[:len(old)]))


def test_odd_files():
    d = os.path.join(TMP, "odd")
    os.makedirs(d)
    a = (np.arange(20 * 16) % 251).astype(np.int16).reshape(20, 16)
    path, out = os.path.join(d, "x.fits"), os.path.join(d, "x.xisf")
    co = ConvertOptions()
    convert_options_init(C.byref(co), C.sizeof(co))
    co.overwrite = 1
    # An IMAGE extension whose PCOUNT or GCOUNT would misplace or drop its pixels is refused, not read.
    for old, new in ((b"GCOUNT  =                    1", b"GCOUNT  =                    0"),
                     (b"GCOUNT  =                    1", b"GCOUNT  =                    2"),
                     (b"PCOUNT  =                    0", b"PCOUNT  =                    9")):
        patched_fits(path, [fits.PrimaryHDU(), fits.ImageHDU(a)], old, new)
        f = Opened(path)
        what = new.split()[0].decode() + " = " + new.split()[-1].decode()
        check(f.status == ERR_FORMAT and "PCOUNT = 0 and GCOUNT = 1" in err(), f"{what} on an image: {err()}")
        check(convert(ctx, enc(path), enc(out), C.byref(co)) == ERR_FORMAT, f"{what}: not converted")
    # A tile-compressed image that cannot be decoded is left out of every reading: the others keep their numbers.
    patched_fits(path, [fits.PrimaryHDU(), fits.CompImageHDU(a, compression_type="RICE_1"), fits.ImageHDU(a + 1, name="PLAIN")],
                 b"ZVAL2   =                    2", b"ZVAL2   =                    3")
    with Opened(path) as f:
        check(image_count(f.handle) == 1 and skipped_count(f.handle) == 1 and image_name(f.handle, 0) == b"PLAIN",
              "an undecodable Rice image is skipped when the file is opened")
        check(same(f.read(0), (a + 1).astype(np.uint16)[None]), "image 0 is the plain one")
    co.image = 0
    check(convert(ctx, enc(path), enc(out), C.byref(co)) == OK and same(xisf_read(out)[1], (a + 1).astype(np.uint16)[None][:, ::-1]),
          f"and image 0 is the same image for a conversion: {err()}")
    co.image = 1
    check(convert(ctx, enc(path), enc(out), C.byref(co)) == ERR_INDEX, "there is no image 1")
    co.image = ALL_IMAGES
    # NUL bytes in a header do not cut the text short
    h = fits.PrimaryHDU(a)
    h.header["OBJECT"] = "ab cd"
    h.header["TELESCOP"] = "after"
    h.writeto(path, overwrite=True)
    raw = open(path, "rb").read()
    open(path, "wb").write(raw.replace(b"'ab cd", b"'ab\0cd"))
    with Opened(path) as f:
        cards = {n: v for n, v, c in f.cards()}
        check(cards.get("OBJECT", "").replace(" ", "") == "'abcd'" and "ab cd" in cards.get("OBJECT", "") and "TELESCOP" in cards,
              f"a NUL byte in a FITS card reads as a space: {cards.get('OBJECT')!r}")
    # A checksum that was not verified when the pixels were loaded is verified when that is asked for.
    apath = os.path.join(d, "x.asdf")
    check(write_images(apath, [a.astype(np.uint16)[None]]) == OK, "write ASDF")
    raw = bytearray(open(apath, "rb").read())
    at = raw.index(b"\xd3BLK") + 60
    raw[at] ^= 0x55
    open(apath, "wb").write(raw)
    with Opened(apath) as f:
        check(load_pixels(f.handle, 0, 0) == OK, "a damaged block loads when its checksum is not verified")
        check(f.read() == ERR_CHECKSUM, "and is refused when it is")
    # A header that declares an absurd size does not decide how much is written.
    xml = (b'<?xml version="1.0" encoding="UTF-8"?><xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">'
           b'<Image geometry="2:2:1" sampleFormat="UInt8" colorSpace="Gray" location="attachment:400:4" '
           b'compression="zlib:9223372036854775808"/></xisf>')
    bomb = os.path.join(d, "bomb.xisf")
    open(bomb, "wb").write((b"XISF0100" + len(xml).to_bytes(4, "little") + b"\0" * 4 + xml).ljust(400, b"\0") + b"abcd")
    st = rewrite(ctx, enc(bomb), enc(os.path.join(d, "bomb-out.xisf")), None, None)
    left = [n for n in os.listdir(d) if n.startswith("bomb-out")]
    check(st != OK and not left, f"a rewrite of a file with an absurd declared size fails and leaves nothing: {st} {left} {err()}")
    # Aliases in an ASDF tree are not expanded without limit.
    laughs = ["#ASDF 1.0.0", "#ASDF_STANDARD 1.5.0", "%YAML 1.1", "--- ", "a0: &a0 [x, x, x, x, x, x, x, x, x, x]"]
    for k in range(1, 12):
        laughs.append(f"a{k}: &a{k} [" + ", ".join([f"*a{k - 1}"] * 10) + "]")
    laughs.append("...")
    open(apath, "wb").write("\n".join(laughs).encode() + b"\n")
    n = size_t()
    check(asdf_tree_json(ctx, enc(apath), None, 0, C.byref(n)) == ERR_FORMAT and "too large" in err(),
          f"a tree that expands without end: {err()}")


def test_wcs_forms():
    """Rows reversed by the writer: every form of WCS still points each pixel at the same sky."""
    import warnings
    from astropy.wcs import WCS
    warnings.simplefilter("ignore")   # astropy remarks on the deprecated PC00i00j form, which is tested on purpose
    h, w = 31, 40
    a = image(np.uint16, (1, h, w))
    base = [("CTYPE1", "RA---TAN", None), ("CTYPE2", "DEC--TAN", None), ("CRVAL1", 150.0, None), ("CRVAL2", 2.0, None),
            ("CRPIX1", 12.0, None)]
    forms = {
        "CD matrix": base + [("CRPIX2", 7.25, None), ("CD1_1", -3e-4, None), ("CD1_2", 2e-5, None), ("CD2_1", 3e-5, None),
                             ("CD2_2", 3e-4, None)],
        "PC and CDELT": base + [("CRPIX2", 7.25, None), ("CDELT1", -3e-4, None), ("CDELT2", 3e-4, None), ("PC1_1", 0.99, None),
                                ("PC1_2", 0.1, None), ("PC2_1", -0.1, None), ("PC2_2", 0.99, None)],
        "old PC00i00j and CDELT": base + [("CRPIX2", 7.25, None), ("CDELT1", -3e-4, None), ("CDELT2", 3e-4, None),
                                          ("PC001001", 0.99, None), ("PC001002", 0.1, None), ("PC002001", -0.1, None),
                                          ("PC002002", 0.99, None)],
        "CDELT only": base + [("CRPIX2", 7.25, None), ("CDELT1", -3e-4, None), ("CDELT2", 3e-4, None)],
        "no CRPIX2": base + [("CD1_1", -3e-4, None), ("CD1_2", 0.0, None), ("CD2_1", 0.0, None), ("CD2_2", 3e-4, None)],
    }
    alternate = [("CTYPE1A", "RA---TAN", None), ("CTYPE2A", "DEC--TAN", None), ("CRVAL1A", 10.0, None), ("CRVAL2A", -5.0, None),
                 ("CRPIX1A", 3.0, None), ("CRPIX2A", 20.5, None), ("CD1_1A", -5e-4, None), ("CD1_2A", 4e-5, None),
                 ("CD2_1A", 6e-5, None), ("CD2_2A", 5e-4, None)]
    forms["an alternate description A"] = forms["CD matrix"] + alternate
    path = os.path.join(TMP, "forms.fits")
    x, y = 9.0, 4.0
    for label, cards in forms.items():
        header = fits.Header([(n, v) for n, v, c in cards])
        check(write_images(path, [a], cards=cards) == OK, f"write FITS, WCS as {label}")
        with fits.open(path) as hd:
            for key in (" ", "A") if "alternate" in label else (" ",):
                given = WCS(header, key=key).pixel_to_world_values(x, y)                  # in the top-down buffer
                stored = WCS(hd[0].header, key=key).pixel_to_world_values(x, h - 1 - y)   # the same pixel, stored bottom-up
                check(np.allclose(given, stored, atol=1e-10), f"WCS as {label}{' (key A)' if key == 'A' else ''}: {given} vs {stored}")
    # Pixels and keywords of an XISF file go back out unchanged when the keywords' row order is passed on.
    src = os.path.join(TMP, "forms.xisf")
    cards = forms["CD matrix"]
    check(write_images(src, [a], cards=cards) == OK, "write XISF with WCS")
    with Opened(src) as f:
        i, px, kws = f.info(), f.read(), f.cards()
    check(i.row_order == ROWS_TOP_DOWN and i.wcs_row_order == ROWS_BOTTOM_UP, "XISF: rows top-down, WCS keywords bottom-up")
    again = [(n, v.strip("' ") if v[:1] == "'" else float(v), None) for n, v, c in kws
             if n[:5] in ("CTYPE", "CRVAL", "CRPIX") or n[:2] == "CD"]
    given = WCS(fits.Header([(n, v) for n, v, c in cards])).pixel_to_world_values(x, y)
    for target, rows in ((os.path.join(TMP, "forms2.fits"), ROWS_TOP_DOWN), (os.path.join(TMP, "forms2.xisf"), ROWS_DEFAULT)):
        check(write_images(target, [px], rows=i.row_order, cards=again, wcs_rows=i.wcs_row_order, row_order=rows) == OK,
              f"write them again: {err()}")
        with Opened(target) as f:
            st, top = wcs_of(f, ROWS_TOP_DOWN)
        back = WCS(fits.Header([(n, float(v) if n[:2] in ("CR", "CD") else v.strip("' ")) for n, v in (top or {}).items()]))
        check(st == OK and np.allclose(back.pixel_to_world_values(x, y), given, atol=1e-10),
              f"XISF -> arrays and keywords -> {os.path.splitext(target)[1][1:]}: the WCS is unchanged")


COMMA_LOCALES = ("de_DE.UTF-8", "nl_BE.UTF-8", "fr_FR.UTF-8", "de_DE.utf8", "de_DE", "German_Germany.1252", "French_France.1252")


def test_locale():
    """A host program that has set a locale with a decimal comma must not change what is written."""
    src, out = os.path.join(TMP, "loc.fits"), os.path.join(TMP, "loc.xisf")
    a = (np.linspace(-0.5, 2.5, 30 * 20).reshape(30, 20)).astype(np.float32)
    h = fits.PrimaryHDU(a)
    for n, v in (("CTYPE1", "RA---TAN"), ("CTYPE2", "DEC--TAN"), ("CRVAL1", 10.684), ("CRVAL2", 41.269), ("CRPIX1", 10.5),
                 ("CRPIX2", 7.25), ("CD1_1", -2.8e-4), ("CD1_2", 0.0), ("CD2_1", 0.0), ("CD2_2", 2.8e-4)):
        h.header[n] = v
    h.header["ROWORDER"] = "TOP-DOWN"
    h.writeto(src, overwrite=True)
    code = f"""
import ctypes as C, locale, sys
name = None
for candidate in {COMMA_LOCALES!r}:
    try:
        locale.setlocale(locale.LC_ALL, candidate)
        name = candidate
        break
    except locale.Error:
        pass
if name is None or locale.localeconv()["decimal_point"] != ",":
    print("no comma locale")
    sys.exit(0)
lib = C.CDLL({LIB_PATH!r})
lib.xisfconv_context_new.restype = C.c_void_p
ctx = C.c_void_p(lib.xisfconv_context_new())
kw, n, v = C.c_void_p(), C.c_char_p(), C.c_char_p()
assert lib.xisfconv_keywords_new(ctx, C.byref(kw)) == 0
lib.xisfconv_keywords_append_number.argtypes = [C.c_void_p, C.c_char_p, C.c_double, C.c_char_p]
assert lib.xisfconv_keywords_append_number(kw, b"CRPIX2", 7.25, None) == 0
assert lib.xisfconv_keywords_append_string(kw, b"CTYPE1", b"RA---TAN", None) == 0
lib.xisfconv_wcs_flip_rows.argtypes = [C.c_void_p, C.c_uint64]
assert lib.xisfconv_wcs_flip_rows(kw, 30) == 0
lib.xisfconv_keywords_get.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p, C.c_void_p]
assert lib.xisfconv_keywords_get(kw, 0, C.byref(n), C.byref(v), None) == 0
print("flipped", v.value.decode())
assert lib.xisfconv_convert(ctx, {enc(src)!r}, {enc(out)!r}, None) == 0, "convert"
print("locale", name)
"""
    env = dict(os.environ)
    r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, env=env)
    if "no comma locale" in r.stdout and shutil.which("localedef"):
        loc = os.path.join(TMP, "locales")
        os.makedirs(loc, exist_ok=True)
        subprocess.run(["localedef", "-i", "de_DE", "-f", "UTF-8", os.path.join(loc, "de_DE.UTF-8")], capture_output=True)
        env["LOCPATH"] = loc
        r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, env=env)
    if "no comma locale" in r.stdout:
        print("no locale with a decimal comma is installed: the locale test is skipped")
        return
    check(r.returncode == 0 and "flipped 23.75" in r.stdout,
          f"numbers keep their decimal point under a comma locale: {r.stdout} {r.stderr[-300:]}")
    if r.returncode == 0:
        meta = XISF(out).get_images_metadata()[0]
        lo, hi = meta["bounds"].split(":")
        check(abs(float(lo) + 0.5) < 1e-6 and abs(float(hi) - 2.5) < 1e-6, f"XISF bounds written under a comma locale: {meta['bounds']}")
        check(meta["FITSKeywords"]["CRVAL1"][0]["value"] == "10.684" and float(meta["FITSKeywords"]["CRPIX2"][0]["value"]) == 23.75,
              f"keywords written under a comma locale: {meta['FITSKeywords']['CRPIX2'][0]['value']}")


if __name__ == "__main__":
    print("libxisfconv:", LIB_PATH, version().decode())
    print("xisfconv:", EXE or "(not given: the comparison with the tool's --stretch is skipped)")
    print("asdf + asdf-astropy:", "yes" if HAVE_ASDF else "no")
    for t in (test_write_fits, test_write_fits_tile_compressed, test_write_xisf, test_lz4_and_levels, test_write_asdf, test_write_tiff_png, test_writer_arguments, test_read_fits,
              test_read_xisf, test_smaller_pictures, test_carried_properties, test_wcs, test_wcs_forms, test_stretch, test_odd_files, test_locale, test_progress_and_cancel,
              test_kept_messages_and_cancel_from_another_thread, test_threads, test_distributed_units, test_silence):
        try:
            t()
        except Exception as e:  # noqa: BLE001
            import traceback
            failures.append(f"{t.__name__}: {type(e).__name__}: {e}")
            print("ERROR in", t.__name__, ":", e)
            traceback.print_exc()
    context_free(ctx)
    print(f"\n{passed} checks passed, {len(failures)} failed")
    if not failures:
        shutil.rmtree(TMP, ignore_errors=True)
    else:
        print("temp files kept in", TMP)
    sys.exit(1 if failures else 0)
