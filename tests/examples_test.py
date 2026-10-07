#!/usr/bin/env python3
"""Tests of the example programs of the manual (docs/manual.html), in C and in C++.

SPDX-License-Identifier: GPL-3.0-or-later

The manual shows parts of examples/first.c, first.cpp, tour.c and tour.cpp and what they print. Here
the programs, as CMake built them (-DXISFCONV_BUILD_TESTS=ON), run on a small XISF file that this script
writes byte by byte, and what they print and write is checked: an example that no longer compiles, or
no longer does what the manual says, fails here. (examples/first.py and tour.py are run by
python/tests/test_examples.py.)

Requirements: none but Python
Usage: python3 tests/examples_test.py <directory with xisfconv_tour_c and the others>
       (the tool xisfconv, if it is in that directory too, verifies the files the tours write)
"""
import array
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

if len(sys.argv) < 2:
    sys.exit(__doc__)
WHERE = os.path.abspath(sys.argv[1])
TMP = tempfile.mkdtemp(prefix="xisfconv-examples-")
CHAPTERS = ["inspect", "pixels", "stretch", "write", "convert", "rewrite", "units", "wcs", "progress"]
WRITTEN = ["back.xisf", "crop.xisf", "frame.fits", "packed.xisf", "preview.png", "smaller.xisf", "stretched.png",
           "there.fits", "unit.xisb", "unit.xish"]

passed = 0
failures = []


def check(cond, msg):
    global passed
    if cond:
        passed += 1
    else:
        failures.append(msg)
        print("FAIL:", msg)


def program(name):
    for candidate in (name, name + ".exe", os.path.join("Release", name + ".exe"), os.path.join("Release", name)):
        if os.path.exists(os.path.join(WHERE, candidate)):
            return os.path.join(WHERE, candidate)
    return None


def run(command, cwd=None):
    done = subprocess.run(command, cwd=cwd, capture_output=True)
    return done.returncode, done.stdout.decode("utf-8", "replace").replace("\r\n", "\n"), done.stderr.decode("utf-8", "replace")


def small_frame(path, width=96, height=64):
    """A monolithic XISF file with what the tour asks about: one image of 32-bit floating point, keywords,
    a property in the header and one in a data block of its own, and properties of the file."""
    seed, samples = 12345, array.array("f")
    for i in range(width * height):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        star = 0.8 if seed % 211 == 0 else 0.0
        samples.append(min(1.0, 0.1 + (seed % 1000) / 20000.0 + star + (i % width) / (20.0 * width)))
    if sys.byteorder == "big":
        samples.byteswap()
    pixels = samples.tobytes()
    history = ("<history>" + "calibrated, registered, integrated; " * 150 + "</history>").encode()
    first = 4096
    header = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" '
        'xsi:schemaLocation="http://www.pixinsight.com/xisf http://pixinsight.com/xisf/xisf-1.0.xsd">\n'
        '<Image id="small_frame" geometry="%d:%d:1" sampleFormat="Float32" bounds="0:1" colorSpace="Gray" '
        'location="attachment:%d:%d">\n'
        '<FITSKeyword name="INSTRUME" value="\'A camera\'" comment="Name of instrument"/>\n'
        '<FITSKeyword name="OBJECT" value="\'a test\'" comment="Name of observed object"/>\n'
        '<FITSKeyword name="EXPTIME" value="300." comment="seconds"/>\n'
        '<Property id="Observation:Object:Name" type="String">a test</Property>\n'
        '<Property id="Tour:History" type="String" location="attachment:%d:%d"/>\n'
        '</Image>\n'
        '<Metadata>\n'
        '<Property id="XISF:CreationTime" type="String">2026-10-07T12:00:00Z</Property>\n'
        '<Property id="XISF:CreatorApplication" type="String">tests/examples_test.py</Property>\n'
        '</Metadata>\n'
        '</xisf>\n' % (width, height, first, len(pixels), first + len(pixels), len(history))).encode()
    assert 16 + len(header) <= first
    with open(path, "wb") as f:
        f.write(b"XISF0100" + struct.pack("<II", len(header), 0) + header)
        f.write(bytes(first - 16 - len(header)))
        f.write(pixels + history)
    return width, height


def odd_frames(directory):
    """Two more frames, for what the first does not ask of the tours: three channels of 16-bit integers with
    WCS keywords and BAYERPAT and without the name of an object, and floating point with the bounds 0:65535."""
    wrap = ('<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">\n%s</xisf>\n')
    seed, samples = 99, array.array("H")
    for _ in range(21 * 12 * 3):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        samples.append(seed % 60000)
    if sys.byteorder == "big":
        samples.byteswap()
    cards = "".join('<FITSKeyword name="%s" value="%s" comment=""/>\n' % card for card in (
        ("BAYERPAT", "'RGGB'"), ("CTYPE1", "'RA---TAN'"), ("CTYPE2", "'DEC--TAN'"), ("CRPIX1", "10.5"), ("CRPIX2", "6.5"),
        ("CRVAL1", "10."), ("CRVAL2", "20."), ("CD1_1", "-0.0002"), ("CD1_2", "0."), ("CD2_1", "0."), ("CD2_2", "0.0002"),
        ("TELESCOP", "'a telescope'")))
    floats = array.array("f", [5000.0 + ((i * 7919) % 4000) for i in range(40 * 30)])
    if sys.byteorder == "big":
        floats.byteswap()
    frames = {"rgb16.xisf": ('<Image id="rgb16" geometry="21:12:3" sampleFormat="UInt16" colorSpace="RGB" '
                             'location="attachment:4096:%d">\n%s</Image>\n' % (len(samples) * 2, cards), samples.tobytes()),
              "wide.xisf": ('<Image id="wide" geometry="40:30:1" sampleFormat="Float32" bounds="0:65535" colorSpace="Gray" '
                            'location="attachment:4096:%d"/>\n' % (len(floats) * 4), floats.tobytes())}
    for name, (image, data) in frames.items():
        header = (wrap % image).encode()
        with open(os.path.join(directory, name), "wb") as f:
            f.write(b"XISF0100" + struct.pack("<II", len(header), 0) + header + bytes(4096 - 16 - len(header)) + data)


def chapters_of(text):
    found, name = {}, None
    for line in text.split("\n"):
        if line.startswith("== "):
            name = line[3:]
            found[name] = []
        elif name is not None and line:
            found[name].append(line)
    return found


def test_first(frame, width, height):
    said = []
    for lang in ("c", "cpp"):
        exe = program("xisfconv_first_" + lang)
        check(exe is not None, "xisfconv_first_%s is built" % lang)
        if exe is None:
            continue
        code, out, err = run([exe, frame])
        check(code == 0 and out == "small_frame: %d x %d pixels, 1 channel(s)\n" % (width, height),
              "first.%s says what is in the file: %d %r %r" % (lang, code, out, err))
        said.append(out)
        tool = program("xisfconv")
        if tool:                                                  # ... also in a FITS file
            as_fits = os.path.join(TMP, "small-%s.fits" % lang)
            run([tool, frame, "-o", as_fits, "-q"])
            code, out, err = run([exe, as_fits])
            check(code == 0 and out.endswith(": %d x %d pixels, 1 channel(s)\n" % (width, height)) and out.count("\n") == 1,
                  "first.%s says what is in a FITS file: %d %r %r" % (lang, code, out, err))
        code, out, err = run([exe, os.path.join(TMP, "not there.xisf")])
        check(code == 1 and out == "" and "cannot open" in err, "first.%s on a file that is not there: %d %r %r" % (lang, code, out, err))
        code, out, err = run([exe])
        check(code == 2 and out == "" and err == "", "first.%s without a file: %d %r" % (lang, code, err))
    check(len(set(said)) == 1, "first.c and first.cpp print the same")


def test_tour(frame, width, height):
    side = min(width, height, 512)
    tool = program("xisfconv")
    printed = {}
    for lang in ("c", "cpp"):
        exe = program("xisfconv_tour_" + lang)
        check(exe is not None, "xisfconv_tour_%s is built" % lang)
        if exe is None:
            continue
        work = os.path.join(TMP, "tour-" + lang)
        os.makedirs(os.path.join(work, "out"))
        shutil.copyfile(frame, os.path.join(work, "small.xisf"))
        code, out, err = run([exe, "small.xisf", "out"], cwd=work)
        check(code == 0 and err == "", "tour.%s runs: %d, stderr %r" % (lang, code, err))
        found = chapters_of(out)
        printed[lang] = out
        check(list(found) == CHAPTERS, "tour.%s runs its chapters in order: %s" % (lang, list(found)))
        check(sorted(os.listdir(os.path.join(work, "out"))) == WRITTEN,
              "tour.%s leaves the files the manual lists and no others: %s" % (lang, sorted(os.listdir(os.path.join(work, "out")))))
        if list(found) != CHAPTERS:
            continue

        text = {name: "\n".join(lines) for name, lines in found.items()}
        check(found["inspect"][0] == "1 image(s) in %d bytes, XISF 1.0, monolithic" % os.path.getsize(frame),
              "tour.%s, inspect, the file: %s" % (lang, found["inspect"][0]))
        check('image 0 "small_frame": %d x %d pixels, 1 channel(s), Float32, range 0 to 1' % (width, height) in text["inspect"],
              "tour.%s, inspect, the image: %s" % (lang, text["inspect"]))
        check("3 keywords, the first of them:" in text["inspect"] and "  OBJECT  = 'a test' / Name of observed object" in text["inspect"]
              and "the object is a test" in text["inspect"], "tour.%s, inspect, the keywords: %s" % (lang, text["inspect"]))
        check("2 properties of the image:" in text["inspect"] and "Observation:Object:Name (String) = a test" in text["inspect"] and
              "  Tour:History (String) = <history>calibrated, registered, integrated;...\n" in text["inspect"] + "\n" and
              "XISF:CreatorApplication (String) = tests/examples_test.py" in text["inspect"],
              "tour.%s, inspect, the properties: %s" % (lang, text["inspect"]))
        numbers = re.match(r"(\d+) samples: minimum ([\d.]+), maximum ([\d.]+), mean ([\d.]+)$", found["pixels"][0])
        check(bool(numbers) and int(numbers.group(1)) == width * height and
              0.1 <= float(numbers.group(2)) < float(numbers.group(4)) < float(numbers.group(3)) <= 1.0,
              "tour.%s, pixels: %s" % (lang, found["pixels"][0]))
        check(re.match(r"shadows [\d.]+, midtones [\d.]+, highlights [\d.]+$", found["stretch"][0]) is not None and
              found["stretch"][1] == "wrote stretched.png, %d x %d, 8 bits" % (width, height), "tour.%s, stretch: %s" % (lang, found["stretch"]))
        check(found["write"] == ["wrote crop.xisf: %d x %d pixels, compressed with zlib, SHA-256 checksum" % (side, side)],
              "tour.%s, write: %s" % (lang, found["write"]))
        check(found["convert"] == ["wrote frame.fits", "wrote preview.png"], "tour.%s, convert: %s" % (lang, found["convert"]))
        check(re.match(r"\d+ -> \d+ bytes: 2 block\(s\) compressed, 0 kept, 2 checksum\(s\), read back: yes$", found["rewrite"][0]) is not None
              and found["rewrite"][1] == "smaller.xisf is stored as asked: yes"
              and found["rewrite"][2] == "verdict OK: 1 image, 2 data blocks; 2 checksum(s) verified" and len(found["rewrite"]) == 3,
              "tour.%s, rewrite: %s" % (lang, found["rewrite"]))
        check(re.match(r"unit.xish is a distributed unit: header \d+ bytes, \d+ bytes with its data$", found["units"][0]) is not None
              and found["units"][1:] == ["  data in unit.xisb", "with XISFCONV_EXTERNAL_NONE: not allowed", "with the default: read",
                                         "packed into packed.xisf"], "tour.%s, units: %s" % (lang, found["units"]))
        check(found["wcs"] == ["astrometric solution: none", "this image has no solution to make WCS keywords from"],
              "tour.%s, wcs: %s" % (lang, found["wcs"]))
        check(found["progress"][0] == 'status 2, "input/output error": cannot open file', "tour.%s, a failure: %s" % (lang, found["progress"][0]))
        notes = [line for line in found["progress"] if line.startswith("  [note] image 0")]
        check(len(notes) == 3 and "taken along" in notes[0] and "32-bit float" in notes[1] and "restored" in notes[2],
              "tour.%s, the notes of a conversion to FITS and back reach the handler: %s" % (lang, found["progress"]))
        check(found["progress"][-3:] == ["  rewriting 0 of 2", "  rewriting 1 of 2", "stopped: cancelled; a file is left: no"],
              "tour.%s, progress and a call that is stopped: %s" % (lang, found["progress"]))

        with open(os.path.join(work, "small.xisf"), "rb") as a, open(os.path.join(work, "out", "packed.xisf"), "rb") as b:
            a, b = a.read(), b.read()
        check(len(a) == len(b) and a[4096:] == b[4096:], "tour.%s: the unit, packed again, has the data of the frame" % lang)

        # A second run in the same directory replaces what the first one wrote, and prints the same.
        code, again, err = run([exe, "small.xisf", "out"], cwd=work)
        check(code == 0 and again == out, "tour.%s runs again in the same directory: %d %r" % (lang, code, err))
        code, one, err = run([exe, "small.xisf", "out", "pixels"], cwd=work)
        check(code == 0 and list(chapters_of(one)) == ["pixels"], "tour.%s runs one chapter: %d %r" % (lang, code, one))
        code, _, err = run([exe, "small.xisf"], cwd=work)
        check(code == 2 and "usage:" in err, "tour.%s without a directory: %d %r" % (lang, code, err))
        if tool:                                                  # a FITS file is turned away: the chapters are about XISF
            code, one, err = run([exe, os.path.join("out", "frame.fits"), "out"], cwd=work)
            check(code == 2 and one == "" and "is not an XISF file" in err, "tour.%s with a FITS file: %d %r %r" % (lang, code, one, err))
        code, one, err = run([exe, "small.xisf", "out", "no such chapter"], cwd=work)
        check(code == 2 and one == "" and "no chapter" in err, "tour.%s with a chapter it does not have: %d %r" % (lang, code, err))
        # every chapter runs on its own, in a directory where no other has been
        for name in CHAPTERS:
            alone = os.path.join(work, "alone-" + name)
            os.mkdir(alone)
            code, one, err = run([exe, "small.xisf", "alone-" + name, name], cwd=work)
            check(code == 0 and err == "" and one == "== %s\n%s\n" % (name, text[name]),
                  "tour.%s runs the chapter %s alone, and it prints what it prints among the others: %d %r %r" % (lang, name, code, one, err))
        code, _, err = run([exe, "not there.xisf", "out", "inspect"], cwd=work)
        check(code == 1 and "cannot open" in err, "tour.%s on a file that is not there: %d %r" % (lang, code, err))

        if tool:
            for name in ("crop.xisf", "smaller.xisf", "unit.xish", "packed.xisf", "back.xisf", "frame.fits", "there.fits"):
                code, said, err = run([tool, "--verify", os.path.join("out", name)], cwd=work)
                check(code == 0 and ": OK" in said, "tour.%s: xisfconv verifies %s: %d %r %r" % (lang, name, code, said, err))
            code, said, err = run([tool, "-I", os.path.join("out", "crop.xisf")], cwd=work)
            check(code == 0 and "%d x %d x 1, Float32" % (side, side) in said and "CROPSIZE= %d / pixels" % side in said and
                  "FITS keywords (5)" in said and "INSTRUME= 'A camera'" in said and "EXPTIME = 300." in said and
                  "Tour:Side (UInt32) = %d" % side in said and "Tour:Made (TimePoint) = 2026-10-07T12:00:00Z" in said and
                  "Tour:Scale (F64Vector)" in said and "zlib" in said and "sha256:" in said and "bounds:      0 : 1" in said,
                  "tour.%s: crop.xisf has the keywords, properties and storage the chapter gives it: %r %r" % (lang, said, err))
    if len(printed) == 2:
        check(printed["c"] == printed["cpp"], "tour.c and tour.cpp print the same")

    # Frames of other kinds: colour, integers, keywords that tell where a pixel is, a range that is not 0 to 1.
    odd_frames(TMP)
    for frame_name in ("rgb16.xisf", "wide.xisf"):
        printed = {}
        for lang in ("c", "cpp"):
            exe = program("xisfconv_tour_" + lang)
            if exe is None:
                continue
            work = os.path.join(TMP, "%s-%s" % (frame_name[:-5], lang))
            os.makedirs(os.path.join(work, "out"))
            shutil.copyfile(os.path.join(TMP, frame_name), os.path.join(work, frame_name))
            code, out, err = run([exe, frame_name, "out"], cwd=work)
            printed[lang] = out
            found = chapters_of(out)
            check(code == 0 and err == "" and list(found) == CHAPTERS, "tour.%s runs on %s: %d %r" % (lang, frame_name, code, err))
            if list(found) != CHAPTERS:
                continue
            text = {name: "\n".join(lines) for name, lines in found.items()}
            crop = run([tool, "-I", os.path.join("out", "crop.xisf")], cwd=work)[1] if tool else None
            if frame_name == "rgb16.xisf":
                check('image 0 "rgb16": 21 x 12 pixels, 3 channel(s), UInt16, range 0 to 65535' in text["inspect"] and
                      "the object is not named" in text["inspect"] and "0 properties of the image:" in text["inspect"],
                      "tour.%s, inspect, three channels of integers and no object: %s" % (lang, text["inspect"]))
                check(found["pixels"][0].startswith("%d samples: minimum 0." % (21 * 12 * 3)), "tour.%s, pixels, integers come as 0 to 1: %s" % (lang, found["pixels"]))
                check(found["write"] == ["wrote crop.xisf: 12 x 12 pixels, compressed with zlib, SHA-256 checksum"], "tour.%s, write, colour: %s" % (lang, found["write"]))
                check(found["wcs"][0] == "astrometric solution: yes" and found["wcs"][1].startswith("10 WCS keywords") and
                      "  CRPIX1  = 10.5" in found["wcs"], "tour.%s, wcs, keywords of the file: %s" % (lang, found["wcs"]))
                if crop:
                    check("12 x 12 x 3, Float32, RGB" in crop and "FITS keywords (3)" in crop and "TELESCOP= 'a telescope'" in crop and
                          "CRPIX" not in crop and "BAYERPAT" not in crop and "AstrometricSolution" not in crop and "CFA" not in crop,
                          "tour.%s: the crop does not have the keywords that tell where a pixel of the frame is: %s" % (lang, crop))
            else:
                numbers = re.match(r"shadows ([\d.]+), midtones ([\d.]+), highlights ([\d.]+)$", found["stretch"][0])
                check(bool(numbers) and 0.01 < float(numbers.group(1)) < 0.14 and float(numbers.group(2)) < 0.5,
                      "tour.%s, stretch, with the range of the image (0 to 65535): %s" % (lang, found["stretch"][0]))
                check("range 0 to 65535" in text["inspect"] and re.match(r"1200 samples: minimum 5\d\d\d\.", found["pixels"][0]) is not None,
                      "tour.%s, floating point comes as it is stored: %s" % (lang, found["pixels"][0]))
                if crop:
                    check("bounds:      0 : 65535" in crop, "tour.%s: the crop has the range of the frame: %s" % (lang, crop[:300]))
        if len(printed) == 2:
            check(printed["c"] == printed["cpp"], "tour.c and tour.cpp print the same for %s" % frame_name)
    if not tool:
        print("xisfconv is not in %s: the files the tours wrote are not verified" % WHERE)


if __name__ == "__main__":
    frame = os.path.join(TMP, "small.xisf")
    size = small_frame(frame)
    for t in (test_first, test_tour):
        try:
            t(frame, *size)
        except Exception as e:  # noqa: BLE001
            import traceback
            failures.append("%s: %s: %s" % (t.__name__, type(e).__name__, e))
            traceback.print_exc()
    print("\n%d checks passed, %d failed" % (passed, len(failures)))
    if not failures:
        shutil.rmtree(TMP, ignore_errors=True)
    else:
        print("temp files kept in", TMP)
    sys.exit(1 if failures else 0)
