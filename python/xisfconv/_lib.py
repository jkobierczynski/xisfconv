# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""The shared library libxisfconv, loaded with ctypes: where it is found, and the declarations
of include/xisfconv.h.

Nothing here is part of the public interface of the package.
"""

import ctypes
import ctypes.util
import os
import sys
from ctypes import (CFUNCTYPE, POINTER, Structure, c_char, c_char_p, c_double, c_float, c_int32, c_int64, c_size_t,
                    c_uint64, c_void_p)

# The library version this module was written for. In 0.x every release may change the layout
# of the structures, so another library is refused.
API_VERSION = (0, 19)

# ------------------------------------------------------------------------------------------
# Constants of xisfconv.h
# ------------------------------------------------------------------------------------------

OK = 0
ERR_ARGUMENT = 1
ERR_IO = 2
ERR_FORMAT = 3
ERR_UNSUPPORTED = 4
ERR_CHECKSUM = 5
ERR_MEMORY = 6
ERR_INDEX = 7
ERR_EXISTS = 8
ERR_BUFFER = 9
ERR_NOT_FOUND = 10
ERR_CANCELLED = 11
ERR_NOT_ALLOWED = 12
ERR_INTERNAL = 99

CODEC_KEEP = -1
CODEC_NONE = 0
CODEC_ZLIB = 1
CODEC_LZ4 = 2
CODEC_LZ4HC = 3
CODEC_ZSTD = 4
CODEC_DEFAULT = 5

MESSAGE_WARNING = 1
MESSAGE_INFO = 2

FORMAT_AUTO = 0
FORMAT_XISF = 1
FORMAT_FITS = 2
FORMAT_ASDF = 3
FORMAT_TIFF = 4
FORMAT_PNG = 5
FORMAT_DNG = 6   # input only (since 0.18)

SAMPLE_AS_STORED = 0
SAMPLE_UINT8 = 1
SAMPLE_UINT16 = 2
SAMPLE_UINT32 = 3
SAMPLE_UINT64 = 4
SAMPLE_FLOAT32 = 5
SAMPLE_FLOAT64 = 6

ROWS_DEFAULT = 0
ROWS_TOP_DOWN = 1
ROWS_BOTTOM_UP = 2

COLOR_GRAY = 0
COLOR_RGB = 1
COLOR_OTHER = 2

CHECKSUM_KEEP = -1
CHECKSUM_NONE = 0
CHECKSUM_SHA1 = 1
CHECKSUM_SHA256 = 2
CHECKSUM_SHA512 = 3
CHECKSUM_SHA3_256 = 4
CHECKSUM_SHA3_512 = 5

STRETCH_NONE = 0
STRETCH_AUTO = 1
STRETCH_LINKED = 2
STRETCH_UNLINKED = 3
STRETCH_STORED = 4

VERDICT_OK = 0
VERDICT_NOT_FULLY_CHECKED = 1
VERDICT_FAILED = 2

ELEMENT_NONE = 0
ELEMENT_INT8 = 1
ELEMENT_UINT8 = 2
ELEMENT_INT16 = 3
ELEMENT_UINT16 = 4
ELEMENT_INT32 = 5
ELEMENT_UINT32 = 6
ELEMENT_INT64 = 7
ELEMENT_UINT64 = 8
ELEMENT_FLOAT32 = 9
ELEMENT_FLOAT64 = 10
ELEMENT_COMPLEX32 = 11
ELEMENT_COMPLEX64 = 12

# xisfconv_external_files
EXTERNAL_HEADER_DIRECTORY, EXTERNAL_ANYWHERE, EXTERNAL_NONE = range(3)

# xisfconv_property_storage
PROPERTY_NONE, PROPERTY_VALUE, PROPERTY_TEXT_BLOCK, PROPERTY_ARRAY, PROPERTY_UNREAD = range(5)

ALL_IMAGES = c_size_t(-1).value
FILE_PROPERTIES = c_size_t(-1).value

# ------------------------------------------------------------------------------------------
# Structures and callbacks
# ------------------------------------------------------------------------------------------


# xisfconv_set_host_progress: the progress handler with one argument and a telling answer
HOST_GO_ON = 0x676F6F6E
HOST_STOP = 0x73746F70


class ProgressReport(Structure):
    _fields_ = [("user", c_void_p), ("stage", c_char_p), ("done", c_uint64), ("total", c_uint64)]


HOST_PROGRESS_FN = CFUNCTYPE(c_int32, POINTER(ProgressReport))


class ImageInfo(Structure):
    _fields_ = [
        ("struct_size", c_size_t),
        ("width", c_uint64),
        ("height", c_uint64),
        ("channels", c_uint64),
        ("sample_format", c_int32),
        ("data_known", c_int32),
        ("lower_bound", c_double),
        ("upper_bound", c_double),
        ("color_space", c_int32),
        ("row_order", c_int32),
        ("row_order_declared", c_int32),
        ("convertible", c_int32),
        ("has_icc_profile", c_int32),
        ("has_display_function", c_int32),
        ("has_stored_stretch", c_int32),
        ("has_astrometric_solution", c_int32),
        ("has_cfa", c_int32),
        ("cfa_width", c_int32),
        ("cfa_height", c_int32),
        ("cfa_pattern", c_char * 68),
        ("resolution_unit", c_int32),
        ("resolution_x", c_double),
        ("resolution_y", c_double),
        ("bitpix", c_int32),
        ("plain_array", c_int32),
        ("bscale", c_double),
        ("bzero", c_double),
        ("source_index", c_uint64),
        ("wcs_row_order", c_int32),
    ]


class ReadOptions(Structure):
    _fields_ = [
        ("struct_size", c_size_t),
        ("sample_format", c_int32),
        ("row_order", c_int32),
        ("verify_checksums", c_int32),
        ("use_bounds", c_int32),
        ("lower_bound", c_double),
        ("upper_bound", c_double),
    ]


class StretchParams(Structure):
    _fields_ = [
        ("shadows", c_double),
        ("midtones", c_double),
        ("highlights", c_double),
        ("low", c_double),
        ("high", c_double),
    ]


class ConvertOptions(Structure):
    _fields_ = [
        ("struct_size", c_size_t),
        ("output_format", c_int32),
        ("sample_format", c_int32),
        ("image", c_size_t),
        ("stretch", c_int32),
        ("codec", c_int32),
        ("checksum", c_int32),
        ("subblock_size", c_uint64),
        ("row_order", c_int32),
        ("property_keywords", c_int32),
        ("wcs", c_int32),
        ("sip_order", c_int32),
        ("verify_checksums", c_int32),
        ("use_bounds", c_int32),
        ("overwrite", c_int32),
        ("lower_bound", c_double),
        ("upper_bound", c_double),
        ("properties", c_int32),
        ("reserved", c_int32),
        ("fit_width", c_uint64),
        ("fit_height", c_uint64),
        ("scale", c_double),
        ("bin", c_int32),
        ("debayer", c_int32),
        ("compression_level", c_int32),
        ("shuffle", c_int32),
    ]


class RewriteOptions(Structure):
    _fields_ = [
        ("struct_size", c_size_t),
        ("codec", c_int32),
        ("checksum", c_int32),
        ("image", c_size_t),
        ("verify_input", c_int32),
        ("read_back", c_int32),
        ("subblock_size", c_uint64),
        ("overwrite", c_int32),
        ("reserved", c_int32),
        ("compression_level", c_int32),
        ("shuffle", c_int32),
    ]


class RewriteResult(Structure):
    _fields_ = [
        ("struct_size", c_size_t),
        ("input_size", c_uint64),
        ("output_size", c_uint64),
        ("blocks", c_uint64),
        ("compressed", c_uint64),
        ("decompressed", c_uint64),
        ("kept", c_uint64),
        ("checksums", c_uint64),
        ("checksums_removed", c_uint64),
        ("read_back", c_int32),
        ("changed", c_int32),
    ]


class Image(Structure):
    _fields_ = [
        ("struct_size", c_size_t),
        ("pixels", c_void_p),
        ("width", c_uint64),
        ("height", c_uint64),
        ("channels", c_uint64),
        ("sample_format", c_int32),
        ("row_order", c_int32),
        ("use_bounds", c_int32),
        ("lower_bound", c_double),
        ("upper_bound", c_double),
        ("name", c_char_p),
        ("keywords", c_void_p),
        ("icc_profile", c_void_p),
        ("icc_profile_size", c_size_t),
        ("wcs_row_order", c_int32),
        ("reserved", c_int32),
        ("properties", c_void_p),
    ]


class WriteOptions(Structure):
    _fields_ = [
        ("struct_size", c_size_t),
        ("format", c_int32),
        ("codec", c_int32),
        ("checksum", c_int32),
        ("row_order", c_int32),
        ("subblock_size", c_uint64),
        ("wcs", c_int32),
        ("overwrite", c_int32),
        ("shuffle", c_int32),
        ("compression_level", c_int32),
        ("properties", c_void_p),
        ("creator_application", c_char_p),
    ]


# ------------------------------------------------------------------------------------------
# Function declarations: name -> (result type, argument types)
# ------------------------------------------------------------------------------------------

_p = c_void_p            # a handle: context, file, keywords, report, writer
_pp = POINTER(c_void_p)
_str = c_char_p
_strp = POINTER(c_char_p)
_status = c_int32

_FUNCTIONS = {
    "xisfconv_status_text": (_str, [_status]),
    "xisfconv_version": (_str, []),
    "xisfconv_version_number": (c_int32, []),
    "xisfconv_codec_available": (c_int32, [c_int32, c_int32]),
    "xisfconv_context_new": (_p, []),
    "xisfconv_context_free": (None, [_p]),
    "xisfconv_error_message": (_str, [_p]),
    "xisfconv_context_keep_messages": (None, [_p, c_int32]),
    "xisfconv_context_set_external_files": (_status, [_p, c_int32]),
    "xisfconv_context_external_files": (c_int32, [_p]),
    "xisfconv_context_message_count": (c_size_t, [_p]),
    "xisfconv_context_message": (_status, [_p, c_size_t, POINTER(c_int32), _strp, _strp]),
    "xisfconv_context_clear_messages": (None, [_p]),
    "xisfconv_context_cancel": (c_int32, [_p]),
    "xisfconv_context_running": (c_int32, [_p]),
    "xisfconv_context_set_host_progress": (None, [_p, HOST_PROGRESS_FN, c_void_p]),
    "xisfconv_context_host_progress_failed": (c_int32, [_p]),
    "xisfconv_sample_size": (c_size_t, [c_int32]),
    "xisfconv_keywords_new": (_status, [_p, _pp]),
    "xisfconv_keywords_free": (None, [_p]),
    "xisfconv_keywords_count": (c_size_t, [_p]),
    "xisfconv_keywords_get": (_status, [_p, c_size_t, _strp, _strp, _strp]),
    "xisfconv_keywords_append": (_status, [_p, _str, _str, _str]),
    "xisfconv_keywords_append_string": (_status, [_p, _str, _str, _str]),
    "xisfconv_keywords_append_number": (_status, [_p, _str, c_double, _str]),
    "xisfconv_keywords_get_text": (_status, [_p, c_size_t, _strp]),
    "xisfconv_keywords_fits_text": (_status, [_p, POINTER(c_void_p), POINTER(c_size_t)]),
    "xisfconv_detect_format": (_status, [_p, _str, POINTER(c_int32)]),
    "xisfconv_open": (_status, [_p, _str, _pp]),
    "xisfconv_close": (None, [_p]),
    "xisfconv_file_format": (c_int32, [_p]),
    "xisfconv_file_size": (c_uint64, [_p]),
    "xisfconv_external_count": (c_size_t, [_p]),
    "xisfconv_external_file": (_str, [_p, c_size_t]),
    "xisfconv_external_status": (c_int32, [_p, c_size_t]),
    "xisfconv_unit_size": (c_uint64, [_p]),
    "xisfconv_image_count": (c_size_t, [_p]),
    "xisfconv_file_detail": (_str, [_p, _str]),
    "xisfconv_skipped_count": (c_size_t, [_p]),
    "xisfconv_skipped_text": (_str, [_p, c_size_t]),
    "xisfconv_header_text": (_status, [_p, POINTER(c_void_p), POINTER(c_size_t)]),
    "xisfconv_image_info_init": (None, [POINTER(ImageInfo), c_size_t]),
    "xisfconv_image_info_get": (_status, [_p, c_size_t, POINTER(ImageInfo)]),
    "xisfconv_image_name": (_str, [_p, c_size_t]),
    "xisfconv_image_unsupported_reason": (_str, [_p, c_size_t]),
    "xisfconv_image_detail": (_str, [_p, c_size_t, _str]),
    "xisfconv_image_keywords": (_status, [_p, c_size_t, _pp]),
    "xisfconv_property_count": (c_size_t, [_p, c_size_t]),
    "xisfconv_property_get": (_status, [_p, c_size_t, c_size_t, _strp, _strp, _strp, _strp, POINTER(c_int32)]),
    "xisfconv_property_find": (c_int64, [_p, c_size_t, _str]),
    "xisfconv_property_format": (_str, [_p, c_size_t, c_size_t]),
    "xisfconv_property_stored": (c_int32, [_p, c_size_t, c_size_t]),
    "xisfconv_property_read_f64": (_status, [_p, c_size_t, _str, c_void_p, c_size_t, POINTER(c_size_t),
                                             POINTER(c_size_t)]),
    "xisfconv_property_element": (c_int32, [_str, POINTER(c_int32)]),
    "xisfconv_element_size": (c_size_t, [c_int32]),
    "xisfconv_property_read": (_status, [_p, c_size_t, c_size_t, c_void_p, c_size_t, POINTER(c_size_t),
                                         POINTER(c_size_t), POINTER(c_size_t)]),
    "xisfconv_properties_new": (_status, [_p, _pp]),
    "xisfconv_properties_free": (None, [_p]),
    "xisfconv_properties_count": (c_size_t, [_p]),
    "xisfconv_properties_set": (_status, [_p, _str, _str, _str, _str, _str]),
    "xisfconv_properties_set_as_read": (_status, [_p, _str, _str, _str, _str, _str, c_int32]),
    "xisfconv_properties_set_array": (_status, [_p, _str, _str, c_void_p, c_size_t, c_uint64, c_uint64, _str, _str]),
    "xisfconv_read_options_init": (None, [POINTER(ReadOptions), c_size_t]),
    "xisfconv_load_pixels": (_status, [_p, c_size_t, c_int32]),
    "xisfconv_pixels_size": (_status, [_p, c_size_t, POINTER(ReadOptions), POINTER(c_uint64)]),
    "xisfconv_read_pixels": (_status, [_p, c_size_t, POINTER(ReadOptions), c_void_p, c_uint64]),
    "xisfconv_read_icc_profile": (_status, [_p, c_size_t, c_void_p, c_size_t, POINTER(c_size_t)]),
    "xisfconv_stored_stretch": (_status, [_p, c_size_t, POINTER(StretchParams), c_size_t, POINTER(c_size_t)]),
    "xisfconv_auto_stretch": (_status, [_p, c_void_p, c_uint64, c_uint64, c_uint64, c_int32, c_double, c_double,
                                        c_size_t, c_int32, POINTER(StretchParams)]),
    "xisfconv_apply_stretch": (_status, [_p, c_void_p, c_uint64, c_uint64, c_uint64, c_int32, c_double, c_double,
                                         POINTER(StretchParams), c_size_t, c_void_p]),
    "xisfconv_wcs_keywords": (_status, [_p, c_size_t, c_int32, c_int32, _pp, _strp]),
    "xisfconv_fits_keywords": (_status, [_p, c_size_t, c_int32, c_int32, c_int32, c_int32, _pp, _strp]),
    "xisfconv_wcs_flip_rows": (_status, [_p, c_uint64]),
    "xisfconv_wcs_digest": (_status, [_p, c_uint64, c_uint64, c_int32, _strp]),
    "xisfconv_convert_options_init": (None, [POINTER(ConvertOptions), c_size_t]),
    "xisfconv_convert": (_status, [_p, _str, _str, POINTER(ConvertOptions)]),
    "xisfconv_rewrite_options_init": (None, [POINTER(RewriteOptions), c_size_t]),
    "xisfconv_rewrite_result_init": (None, [POINTER(RewriteResult), c_size_t]),
    "xisfconv_rewrite": (_status, [_p, _str, _str, POINTER(RewriteOptions), POINTER(RewriteResult)]),
    "xisfconv_rewrite_in_place": (_status, [_p, _str, POINTER(RewriteOptions), POINTER(RewriteResult)]),
    "xisfconv_stored_as_requested": (_status, [_p, _str, POINTER(RewriteOptions), POINTER(c_int32)]),
    "xisfconv_verify": (_status, [_p, _str, _pp]),
    "xisfconv_report_free": (None, [_p]),
    "xisfconv_report_verdict": (c_int32, [_p]),
    "xisfconv_report_format": (c_int32, [_p]),
    "xisfconv_report_summary": (_str, [_p]),
    "xisfconv_report_verified": (c_size_t, [_p]),
    "xisfconv_report_unchecked": (c_size_t, [_p]),
    "xisfconv_report_problem_count": (c_size_t, [_p]),
    "xisfconv_report_problem": (_str, [_p, c_size_t]),
    "xisfconv_report_not_checked_count": (c_size_t, [_p]),
    "xisfconv_report_not_checked": (_str, [_p, c_size_t]),
    "xisfconv_image_init": (None, [POINTER(Image), c_size_t]),
    "xisfconv_write_options_init": (None, [POINTER(WriteOptions), c_size_t]),
    "xisfconv_writer_new": (_status, [_p, _str, POINTER(WriteOptions), _pp]),
    "xisfconv_writer_add_image": (_status, [_p, POINTER(Image)]),
    "xisfconv_writer_finish": (_status, [_p]),
    "xisfconv_writer_discard": (None, [_p]),
}

# ------------------------------------------------------------------------------------------
# Finding and loading the library
# ------------------------------------------------------------------------------------------


def _file_names():
    if sys.platform == "win32":
        return ["libxisfconv.dll", "xisfconv.dll"]
    if sys.platform == "darwin":
        return ["libxisfconv.dylib"]
    return ["libxisfconv.so"]


def _candidates():
    """Places to look, in order: the XISFCONV_LIBRARY environment variable, the library that
    came with this package (a wheel), then a library installed on the system."""
    named = os.environ.get("XISFCONV_LIBRARY")
    if named:
        if os.path.exists(named):
            named = os.path.abspath(named)   # a file is meant, not a name for the system to look up
        yield named, "the XISFCONV_LIBRARY environment variable"
        return
    here = os.path.dirname(os.path.abspath(__file__))
    for name in _file_names():
        path = os.path.join(here, name)
        if os.path.exists(path):
            yield path, "the xisfconv package"
            return
    # An editable installation (pip install -e) has the modules in the source tree and the
    # library where it was installed.
    for entry in sys.path:
        for name in _file_names():
            path = os.path.join(entry or ".", "xisfconv", name)
            if os.path.isfile(path):
                yield os.path.abspath(path), "the installed xisfconv package"
                return
    found = ctypes.util.find_library("xisfconv")
    if found:
        yield found, "the system"
    elif sys.platform not in ("win32", "darwin"):
        # find_library needs the development link or a linker cache entry; the versioned name
        # of this release is there without either
        yield "libxisfconv.so.%d.%d" % API_VERSION, "the system"


def load():
    """Returns (library, path). ImportError if no library of the right version is found."""
    problems = []
    for path, origin in _candidates():
        try:
            lib = ctypes.CDLL(path)
        except OSError as e:
            problems.append("%s (from %s): %s" % (path, origin, e))
            continue
        try:
            version_number = lib.xisfconv_version_number
        except AttributeError:
            problems.append("%s (from %s) is not libxisfconv" % (path, origin))
            continue
        version_number.restype = c_int32
        version_number.argtypes = []
        number = version_number()
        version = (number // 10000, number // 100 % 100)
        if version != API_VERSION:
            problems.append("%s (from %s) is libxisfconv %d.%d.%d; this package needs %d.%d" %
                            (path, origin, version[0], version[1], number % 100, API_VERSION[0], API_VERSION[1]))
            continue
        try:
            for name, (restype, argtypes) in _FUNCTIONS.items():
                function = getattr(lib, name)
                function.restype = restype
                function.argtypes = argtypes
        except AttributeError as e:
            problems.append("%s (from %s) lacks a function: %s" % (path, origin, e))
            continue
        return lib, path
    if not problems:
        problems.append("no libxisfconv found next to the package or on the system")
    raise ImportError("xisfconv: cannot load the shared library libxisfconv: " + "; ".join(problems) +
                      ". Install the package from a wheel (pip install xisfconv), or name the library with the "
                      "XISFCONV_LIBRARY environment variable.")


def struct(cls, init):
    """A structure filled with its defaults by its _init function."""
    value = cls()
    init(ctypes.byref(value), ctypes.sizeof(cls))
    return value


__all__ = ["load", "struct", "c_float"]
