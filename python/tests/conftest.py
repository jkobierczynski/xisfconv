# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Tests of the Python package of xisfconv.

    pytest python/tests

They run against the installed package, or against the source tree:

    XISFCONV_LIBRARY=build-shared/libxisfconv.so PYTHONPATH=python pytest python/tests

What the package reads and writes is compared with other software wherever there is some:
astropy (FITS, WCS), the xisf package (XISF), the asdf package, tifffile and Pillow. Set
XISFCONV_TOOL to the command line tool to also compare with its output.
"""

import os

import pytest

import xisfconv


def pytest_report_header(config):
    return ["xisfconv %s, libxisfconv %s from %s" % (xisfconv.__version__, xisfconv.library_version(),
                                                    xisfconv.library_path),
            "command line tool: %s" % (os.environ.get("XISFCONV_TOOL") or "not given (XISFCONV_TOOL)")]


@pytest.fixture(scope="session")
def tool():
    """The command line tool, for the tests that compare with it."""
    path = os.environ.get("XISFCONV_TOOL")
    if not path:
        pytest.skip("XISFCONV_TOOL is not set")
    return os.path.abspath(path)


@pytest.fixture(autouse=True)
def _warnings_are_errors():
    """A warning of the library that a test does not expect is a failure."""
    import warnings

    with warnings.catch_warnings():
        warnings.simplefilter("error", xisfconv.XisfconvWarning)
        yield
