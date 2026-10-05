# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""The package itself: its version, how it finds the library, and whether its declarations
are those of xisfconv.h."""

import ctypes
import ctypes.util
import os
import re
import shutil
import subprocess
import sys

import pytest

import xisfconv
from xisfconv import _lib

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "include", "xisfconv.h")


def header_text():
    if not os.path.exists(HEADER):
        pytest.skip("include/xisfconv.h is not here (the tests run outside the source tree)")
    with open(HEADER, encoding="utf-8") as source:
        return re.sub(r"/\*.*?\*/", " ", source.read(), flags=re.S)        # without comments


def test_versions():
    assert re.fullmatch(r"\d+\.\d+\.\d+", xisfconv.__version__)
    assert xisfconv.library_version() == xisfconv.__version__
    assert tuple(int(n) for n in xisfconv.__version__.split(".")[:2]) == _lib.API_VERSION
    assert os.path.exists(xisfconv.library_path) or os.sep not in xisfconv.library_path
    if os.path.exists(HEADER):
        numbers = [re.search(r"#define XISFCONV_VERSION_%s\s+(\d+)" % part, header_text()).group(1)
                   for part in ("MAJOR", "MINOR", "PATCH")]
        assert ".".join(numbers) == xisfconv.__version__


def test_public_names():
    for name in xisfconv.__all__:
        assert hasattr(xisfconv, name), name
        thing = getattr(xisfconv, name)
        if callable(thing):
            assert thing.__doc__ and thing.__doc__.strip(), name + " has no documentation"
    assert len(set(xisfconv.__all__)) == len(xisfconv.__all__)
    assert issubclass(xisfconv.InputNotFoundError, (xisfconv.FileError, FileNotFoundError))
    for error in ("ArgumentError", "FileError", "FormatError", "UnsupportedError", "ChecksumError", "ImageIndexError",
                  "OutputExistsError", "NotFoundError", "Cancelled", "InternalError"):
        assert issubclass(getattr(xisfconv, error), xisfconv.Error)


def test_codecs():
    assert xisfconv.codec_available("zlib") and xisfconv.codec_available("zlib", writing=True)
    assert xisfconv.codec_available("lz4") and not xisfconv.codec_available("lz4", writing=True)
    assert xisfconv.codec_available("lz4hc") and xisfconv.codec_available("none")
    assert xisfconv.codec_available("zstd") == xisfconv.codec_available("zstd", writing=True)
    with pytest.raises(ValueError):
        xisfconv.codec_available("brotli")


# --- the declarations against the header ---------------------------------------------------

def test_functions_are_those_of_the_header():
    text = header_text()
    declared = {}
    for match in re.finditer(r"XISFCONV_API\s+[^;(]*?\b(xisfconv_\w+)\s*\(([^;]*?)\)\s*;", text, flags=re.S):
        arguments = match.group(2).strip()
        declared[match.group(1)] = 0 if arguments == "void" else arguments.count(",") + 1
    assert len(declared) >= 86
    # functions the package has no use for (it hears from the library through the host hooks and
    # the kept messages, not through the handlers)
    unused = {"xisfconv_keywords_find", "xisfconv_keywords_remove", "xisfconv_asdf_tree_text",
              "xisfconv_asdf_tree_json", "xisfconv_context_set_message_handler",
              "xisfconv_context_set_progress_handler"}
    assert set(_lib._FUNCTIONS) | unused == set(declared)
    assert not set(_lib._FUNCTIONS) & unused
    for name, (_, argtypes) in _lib._FUNCTIONS.items():
        assert len(argtypes) == declared[name], name


def test_constants_are_those_of_the_header():
    text = header_text()
    constants = {name: int(value, 0) for name, value in
                 re.findall(r"\bXISFCONV_(\w+)\s*=\s*(-?(?:0[xX][0-9A-Fa-f]+|\d+))", text)}
    assert len(constants) >= 52 and constants["HOST_GO_ON"] > 1000
    for name, value in constants.items():
        assert getattr(_lib, name) == value, name


STRUCTS = {"xisfconv_image_info": _lib.ImageInfo, "xisfconv_read_options": _lib.ReadOptions,
           "xisfconv_stretch_params": _lib.StretchParams, "xisfconv_convert_options": _lib.ConvertOptions,
           "xisfconv_rewrite_options": _lib.RewriteOptions, "xisfconv_rewrite_result": _lib.RewriteResult,
           "xisfconv_image": _lib.Image, "xisfconv_write_options": _lib.WriteOptions,
           "xisfconv_progress_report": _lib.ProgressReport}


def test_structures_have_the_fields_of_the_header():
    text = header_text()
    found = dict(re.findall(r"typedef\s+struct\s+(xisfconv_\w+)\s*\{(.*?)\}\s*xisfconv_\w+\s*;", text, flags=re.S))
    assert set(found) == set(STRUCTS)
    for name, body in found.items():
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        # a member is the last word of its declaration, or the name in (*name) of a function pointer
        fields = [(re.search(r"\(\s*\*\s*(\w+)\s*\)", declaration) or
                   re.search(r"(\w+)\s*(\[\d+\])?\s*$", declaration.strip())).group(1)
                  for declaration in body.split(";") if declaration.strip()]
        assert fields == [field for field, _ in STRUCTS[name]._fields_], name


def test_structures_have_the_layout_of_the_compiler(tmp_path):
    """Sizes and offsets as a C compiler lays the structures out, against ctypes."""
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not compiler or not os.path.exists(HEADER):
        pytest.skip("needs a C compiler and the header")
    lines = ['#include <stdio.h>', '#include <stddef.h>', '#include "xisfconv.h"', "int main(void) {"]
    for name, cls in STRUCTS.items():
        lines.append('  printf("%s %%zu\\n", sizeof(%s));' % (name, name))
        for field, _ in cls._fields_:
            lines.append('  printf("%s.%s %%zu\\n", offsetof(%s, %s));' % (name, field, name, field))
    lines += ["  return 0;", "}"]
    source = tmp_path / "layout.c"
    source.write_text("\n".join(lines))
    program = tmp_path / ("layout.exe" if sys.platform == "win32" else "layout")
    subprocess.run([compiler, "-std=c99", "-I", os.path.dirname(HEADER), str(source), "-o", str(program)], check=True)
    theirs = dict(line.split() for line in subprocess.run([str(program)], check=True, capture_output=True,
                                                          text=True).stdout.splitlines())
    for name, cls in STRUCTS.items():
        assert int(theirs[name]) == ctypes.sizeof(cls), name
        for field, _ in cls._fields_:
            assert int(theirs["%s.%s" % (name, field)]) == getattr(cls, field).offset, "%s.%s" % (name, field)


# --- finding the library -------------------------------------------------------------------

def run_import(environment):
    env = dict(os.environ)
    env.update(environment)
    package = os.path.dirname(os.path.dirname(os.path.abspath(xisfconv.__file__)))
    env["PYTHONPATH"] = package + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run([sys.executable, "-c", "import xisfconv; print(xisfconv.library_path)"], env=env,
                          capture_output=True, text=True)


def test_the_library_can_be_named(tmp_path):
    copy = tmp_path / ("mine" + os.path.splitext(xisfconv.library_path)[1])
    if not os.path.isfile(xisfconv.library_path):
        pytest.skip("the library in use has no file name of its own")
    shutil.copy(xisfconv.library_path, copy)
    done = run_import({"XISFCONV_LIBRARY": str(copy)})
    assert done.returncode == 0 and done.stdout.strip() == str(copy)


def test_a_wrong_library_is_refused(tmp_path):
    done = run_import({"XISFCONV_LIBRARY": str(tmp_path / "nothing.so")})
    assert done.returncode != 0 and "ImportError" in done.stderr and "XISFCONV_LIBRARY" in done.stderr
    other = ctypes.util.find_library("z") or ctypes.util.find_library("c")
    if other and sys.platform.startswith("linux"):
        done = run_import({"XISFCONV_LIBRARY": other})
        assert done.returncode != 0 and "is not libxisfconv" in done.stderr
