# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""The example programs of the manual (docs/manual.html) in Python, and the manual itself.

examples/first.py and examples/tour.py run here on a small file, and what they print and write is
checked: an example that no longer does what the manual says fails. (The C and C++ examples are run
by tests/examples_test.py.) The manual is made by docs/make_manual.py from the examples, the header
and the docstrings of the package; the last test says whether it was made from them as they are."""

import os
import re
import struct
import subprocess
import sys

import numpy as np
import pytest

import xisfconv

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
EXAMPLES = os.path.join(ROOT, "examples")
CHAPTERS = ["inspect", "pixels", "stretch", "write", "convert", "rewrite", "units", "wcs", "progress"]
PYTHON_ONLY = ["astropy", "compat"]

pytestmark = pytest.mark.skipif(not os.path.exists(os.path.join(EXAMPLES, "tour.py")),
                                reason="examples/ is not here (the tests run outside the source tree)")


def environment():
    """The environment of this process for a program that runs in another directory: PYTHONPATH and
    XISFCONV_LIBRARY may name places from here (PYTHONPATH=python), and are made to name them from anywhere."""
    env = dict(os.environ)
    if env.get("PYTHONPATH"):
        env["PYTHONPATH"] = os.pathsep.join(os.path.abspath(part) if part else part for part in env["PYTHONPATH"].split(os.pathsep))
    if env.get("XISFCONV_LIBRARY") and os.path.exists(env["XISFCONV_LIBRARY"]):
        env["XISFCONV_LIBRARY"] = os.path.abspath(env["XISFCONV_LIBRARY"])
    return env


ENVIRONMENT = environment()      # (taken when the tests are collected: in the directory they were started in)


def run(arguments, cwd):
    done = subprocess.run([sys.executable] + arguments, cwd=str(cwd), capture_output=True, env=ENVIRONMENT)
    return (done.returncode, done.stdout.decode("utf-8", "replace").replace("\r\n", "\n"),
            done.stderr.decode("utf-8", "replace"))


def chapters_of(text):
    found, name = {}, None
    for line in text.split("\n"):
        if line.startswith("== "):
            name = line[3:]
            found[name] = []
        elif name is not None and line:
            found[name].append(line)
    return found


@pytest.fixture()
def work(tmp_path):
    """A directory with small.xisf, a frame with what the tour asks about, and out/ for what it writes."""
    rng = np.random.default_rng(7)
    data = (0.1 + rng.random((64, 96)) * 0.05).astype(np.float32)
    data[rng.integers(0, 64, 30), rng.integers(0, 96, 30)] = 0.9
    history = "<history>" + "calibrated, registered, integrated; " * 150 + "</history>"    # too long for the header
    xisfconv.write(str(tmp_path / "small.xisf"), data, name="small_frame",
                   keywords=[("INSTRUME", "A camera", "Name of instrument"), ("OBJECT", "a test", "Name of observed object"),
                             ("EXPTIME", 300.0, "seconds")],
                   properties={"Observation:Object:Name": "a test", "Tour:History": history}, creator="test_examples.py")
    (tmp_path / "out").mkdir()
    return tmp_path


def test_first(work):
    code, out, err = run([os.path.join(EXAMPLES, "first.py"), "small.xisf"], work)
    assert (code, out) == (0, "small_frame: 96 x 64 pixels, 1 channel(s)\n"), err
    xisfconv.convert(str(work / "small.xisf"), str(work / "small.fits"))          # ... also in a FITS file
    code, out, err = run([os.path.join(EXAMPLES, "first.py"), "small.fits"], work)
    assert code == 0 and out.endswith(": 96 x 64 pixels, 1 channel(s)\n") and out.count("\n") == 1, err
    code, out, err = run([os.path.join(EXAMPLES, "first.py"), "not there.xisf"], work)
    assert code == 1 and out == "" and "not there.xisf" in err and "cannot open" in err
    code, out, err = run([os.path.join(EXAMPLES, "first.py")], work)
    assert code == 2


def test_tour(work):
    tour = os.path.join(EXAMPLES, "tour.py")
    code, out, err = run([tour, "small.xisf", "out"], work)
    assert code == 0, err
    found = chapters_of(out)
    assert list(found) == CHAPTERS + PYTHON_ONLY
    text = {name: "\n".join(lines) for name, lines in found.items()}

    assert found["inspect"][0] == "1 image(s) in %d bytes, XISF 1.0, monolithic" % os.path.getsize(str(work / "small.xisf"))
    assert 'image 0 "small_frame": 96 x 64 pixels, 1 channel(s), float32, range 0 to 1' in text["inspect"]
    assert "3 keywords, the first of them:" in text["inspect"]
    assert "  OBJECT  = 'a test' / Name of observed object" in text["inspect"] and "the object is a test" in text["inspect"]
    assert "2 properties of the image:" in text["inspect"] and "Observation:Object:Name (String) = a test" in text["inspect"]
    assert "  Tour:History (String) = <history>calibrated, registered, integrated;...\n" in text["inspect"] + "\n"
    assert "XISF:CreatorApplication (String) = test_examples.py" in text["inspect"]

    numbers = re.fullmatch(r"(\d+) samples: minimum ([\d.]+), maximum ([\d.]+), mean ([\d.]+)", found["pixels"][0])
    assert numbers and int(numbers.group(1)) == 96 * 64
    assert 0.1 <= float(numbers.group(2)) < float(numbers.group(4)) < float(numbers.group(3)) <= 1.0
    assert found["pixels"][2] == "read bottom-up, the first row is the last row of the other: yes"

    assert re.fullmatch(r"shadows [\d.]+, midtones [\d.]+, highlights [\d.]+", found["stretch"][0])
    assert found["stretch"][1] == "wrote stretched.png, 96 x 64, 8 bits"
    assert found["write"] == ["wrote crop.xisf: 64 x 64 pixels, compressed with zlib, SHA-256 checksum"]
    assert found["convert"] == ["wrote frame.fits", "wrote preview.png"]
    assert re.fullmatch(r"\d+ -> \d+ bytes: 2 block\(s\) compressed, 0 kept, 2 checksum\(s\), read back: yes", found["rewrite"][0])
    assert found["rewrite"][1:] == ["smaller.xisf is stored as asked: yes", "verdict OK: 1 image, 2 data blocks; 2 checksum(s) verified"]
    assert re.fullmatch(r"unit.xish is a distributed unit: header \d+ bytes, \d+ bytes with its data", found["units"][0])
    assert found["units"][1:] == ["  data in unit.xisb", 'with external_files="none": not allowed', "with the default: read",
                                  "packed into packed.xisf"]
    assert found["wcs"] == ["astrometric solution: none", "this image has no solution to make WCS keywords from"]
    assert found["progress"][0].startswith("InputNotFoundError (a FileNotFoundError: yes): no such file.xisf")
    notes = [line for line in found["progress"] if line.startswith("  [note] ")]
    assert len(notes) == 3 and "taken along" in notes[0] and "32-bit float" in notes[1] and "restored" in notes[2]
    assert found["progress"][-3:] == ["  rewriting 0 of 2", "  rewriting 1 of 2", "stopped: by the handler; a file is left: no"]

    written = ["back.xisf", "crop.xisf", "frame.fits", "packed.xisf", "preview.png", "smaller.xisf", "stretched.png",
               "there.fits", "unit.xisb", "unit.xish"]
    try:
        import astropy  # noqa: F401
        assert found["astropy"][0].startswith("CCDData of (64, 96), ") and "in adu" in found["astropy"][0]
        assert found["astropy"][1] == "OBJECT = a test, ROWORDER = BOTTOM-UP"
        assert found["astropy"][2] == "row 0 of the CCDData is the last row of xisfconv.read(): yes"
        assert found["astropy"][3:] == ["wrote ccd.xisf", found["astropy"][4], "wrote hdus.xisf"]
        assert found["astropy"][4].startswith("1 HDU(s), the first of (64, 96) with ")
        written += ["ccd.xisf", "hdus.xisf"]
    except ImportError:
        assert found["astropy"] == ["astropy is not installed: this chapter is left out"]
    assert found["compat"] == ["geometry (96, 64, 1), Float32, pixels of (64, 96, 1)", "made by test_examples.py",
                               "3 FITS keywords, 2 XISF properties", "wrote compat.xisf"]
    assert sorted(os.listdir(str(work / "out"))) == sorted(written + ["compat.xisf"])

    # what the chapters wrote
    out_dir = work / "out"
    crop = xisfconv.read_image(str(out_dir / "crop.xisf"))
    whole = xisfconv.read(str(work / "small.xisf"))
    assert np.array_equal(crop.data, whole[0:64, 16:80])
    assert crop.keywords.names() == ["INSTRUME", "OBJECT", "EXPTIME", "CROPPED", "CROPSIZE"]
    assert crop.keywords["CROPSIZE"] == 64 and crop.keywords["OBJECT"] == "a test" and crop.name == "crop"
    assert crop.bounds == (0.0, 1.0)
    assert crop.properties["Tour:Side"] == 64 and crop.properties.type("Tour:Side") == "UInt32"
    assert crop.properties["Tour:Made"] == "2026-10-07T12:00:00Z" and crop.properties.type("Tour:Made") == "TimePoint"
    assert np.array_equal(crop.properties["Tour:Scale"], [0.85, 0.85]) and crop.properties.comment("Tour:Scale") == "arcseconds per pixel"
    with xisfconv.open(str(out_dir / "crop.xisf")) as f:
        assert f[0].detail("compression").startswith("zlib+sh") and f[0].detail("checksum").startswith("sha256")
        assert f.properties["XISF:CreatorApplication"] == "tour.py"
    for name in ("smaller.xisf", "unit.xish", "packed.xisf", "back.xisf", "compat.xisf"):
        assert xisfconv.verify(str(out_dir / name)).ok, name
        assert np.array_equal(xisfconv.read(str(out_dir / name)), whole), name
    with open(str(out_dir / "stretched.png"), "rb") as png:                 # 8 bits of gray, the size of the frame
        start = png.read(26)
    assert start[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">IIBB", start[16:26]) == (96, 64, 8, 0)

    # A second run in the same directory replaces what the first one wrote, and prints the same.
    code, again, err = run([tour, "small.xisf", "out"], work)
    assert (code, again) == (0, out), err
    code, one, err = run([tour, "small.xisf", "out", "pixels"], work)
    assert code == 0 and list(chapters_of(one)) == ["pixels"]
    code, _, err = run([tour, "small.xisf"], work)
    assert code == 2 and "usage:" in err
    code, one, err = run([tour, os.path.join("out", "frame.fits"), "out"], work)     # the chapters are about XISF
    assert code == 2 and one == "" and "is not an XISF file" in err
    code, one, err = run([tour, "small.xisf", "out", "no such chapter"], work)
    assert code == 2 and one == "" and "no chapter" in err
    # every chapter runs on its own, in a directory where no other has been
    for name in CHAPTERS + ["compat"]:
        (work / ("alone-" + name)).mkdir()
        code, one, err = run([tour, "small.xisf", "alone-" + name, name], work)
        assert code == 0 and one.replace("alone-%s%s" % (name, os.sep), "out" + os.sep) == "== %s\n%s\n" % (name, text[name]), err
    code, _, err = run([tour, "not there.xisf", "out", "inspect"], work)
    assert code == 1 and "cannot open" in err


def test_tour_on_other_frames(tmp_path):
    """Colour, integers, keywords that tell where a pixel is, and a range that is not 0 to 1."""
    tour = os.path.join(EXAMPLES, "tour.py")
    rng = np.random.default_rng(3)
    xisfconv.write(str(tmp_path / "rgb16.xisf"), rng.integers(0, 60000, (12, 21, 3), dtype=np.uint16),
                   keywords=[("BAYERPAT", "RGGB"), ("CTYPE1", "RA---TAN"), ("CTYPE2", "DEC--TAN"), ("CRPIX1", 10.5),
                             ("CRPIX2", 6.5), ("CRVAL1", 10.0), ("CRVAL2", 20.0), ("CD1_1", -0.0002), ("CD1_2", 0.0),
                             ("CD2_1", 0.0), ("CD2_2", 0.0002), ("TELESCOP", "a telescope")], wcs=False)
    xisfconv.write(str(tmp_path / "wide.xisf"), (5000 + rng.random((30, 40)) * 4000).astype(np.float32), bounds=(0, 65535))
    for name in ("rgb16", "wide"):
        (tmp_path / name).mkdir()
        code, out, err = run([tour, name + ".xisf", name], tmp_path)
        assert code == 0, err
        found = chapters_of(out)
        assert list(found) == CHAPTERS + PYTHON_ONLY
        crop = xisfconv.read_image(str(tmp_path / name / "crop.xisf"))
        if name == "rgb16":
            assert 'image 0 "rgb16": 21 x 12 pixels, 3 channel(s), uint16, range 0 to 65535' in found["inspect"]
            assert "the object is not named" in found["inspect"]
            assert found["pixels"][0].startswith("%d samples: minimum 0." % (21 * 12 * 3))        # integers come as 0 to 1
            assert found["pixels"][2] == "read bottom-up, the first row is the last row of the other: yes"
            assert found["wcs"][0] == "astrometric solution: yes" and found["wcs"][1].startswith("10 WCS keywords")
            # the crop does not have the keywords that tell where a pixel of the frame is
            assert crop.data.shape == (12, 12, 3) and crop.keywords.names() == ["TELESCOP", "CROPPED", "CROPSIZE"]
            assert not any(key.startswith("PCL:AstrometricSolution") for key in crop.properties)
            with xisfconv.open(str(tmp_path / name / "crop.xisf")) as f:
                assert f[0].cfa is None and not f[0].has_astrometric_solution
        else:
            numbers = re.fullmatch(r"shadows ([\d.]+), midtones ([\d.]+), highlights ([\d.]+)", found["stretch"][0])
            assert numbers and 0.01 < float(numbers.group(1)) < 0.14 and float(numbers.group(2)) < 0.5     # of the range 0 to 65535
            assert re.match(r"1200 samples: minimum 5\d\d\d\.", found["pixels"][0])                       # as they are stored
            assert crop.bounds == (0.0, 65535.0)


def test_the_three_tours_have_the_same_chapters():
    """The manual shows each chapter in C, in C++ and in Python: every tour marks every chapter."""
    marks = {"tour.c": r"^/\* \[(/?\w+)\] \*/$", "tour.cpp": r"^// \[(/?\w+)\]$", "tour.py": r"^# \[(/?\w+)\]$"}
    for name, pattern in marks.items():
        with open(os.path.join(EXAMPLES, name), encoding="utf-8") as source:
            found = re.findall(pattern, source.read(), re.M)
        opened = [mark for mark in found if not mark.startswith("/")]
        assert found == [mark for name_ in opened for mark in (name_, "/" + name_)], name      # each one closed, none nested
        assert [mark for mark in opened if mark in CHAPTERS] == CHAPTERS, name
        assert "main" in opened, name


def test_manual_is_up_to_date():
    """docs/manual.html is made from the examples, the header, the package and docs/manual.in.html; whoever
    changes one of them makes the manual again: python docs/make_manual.py"""
    maker = os.path.join(ROOT, "docs", "make_manual.py")
    if not os.path.exists(maker):
        pytest.skip("docs/ is not here (the tests run outside the source tree)")
    pytest.importorskip("astropy")          # (the manual has the reference of xisfconv.astropy)
    code, out, err = run([maker, "--check"], ROOT)
    assert code == 0, out + err
