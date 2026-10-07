#!/usr/bin/env python3
"""A tour of the xisfconv package: every chapter of the manual (docs/manual.html) as one function.

    pip install .                                     (in a checkout of xisfconv)
    python tour.py integrated_light.xisf out/        (the directory must exist; files in it are replaced)

The lines between a pair of marks like [inspect] and [/inspect] are what the manual shows.

SPDX-License-Identifier: LGPL-3.0-or-later
Copyright (C) 2026 Jurgen Kobierczynski
"""
import datetime
import logging
import os
import sys

import numpy as np

import xisfconv

OUT = "."


def out_path(name):
    return os.path.join(OUT, name)


def shortened(text):
    """A text for one line of output: at most 44 bytes of it (as the C tour counts), and whole characters."""
    raw = text.encode()
    return text if len(raw) <= 44 else raw[:44].decode(errors="ignore") + "..."


# [errors]
# A call that fails raises: xisfconv.Error, or the class below it that says what kind of failure it
# was. Most of them are also the exception Python itself has for the case (FileNotFoundError,
# FileExistsError, PermissionError, ValueError), so that code written for those catches them.
def report(what, error):
    print("%s: %s" % (what, error), file=sys.stderr)
    return 1


# Notes of the library on how a conversion was done go to the logger "xisfconv"; its warnings are
# Python warnings of the class xisfconv.XisfconvWarning.
class Notes(logging.Handler):
    def emit(self, record):
        print("  [note] %s" % record.getMessage())
# [/errors]


# [inspect]
def inspect(path):
    # Opening reads the header only.
    with xisfconv.open(path) as f:
        print("%d image(s) in %d bytes, XISF %s, %s" % (len(f), f.size, f.detail("version"), f.unit))

        image = f[0]
        # (bounds are the range of floating point samples; integers have the range of their type)
        low, high = image.bounds or (0, np.iinfo(image.dtype).max)
        print('image 0 "%s": %d x %d pixels, %d channel(s), %s, range %g to %g'
              % (image.name, image.width, image.height, image.channels, image.dtype, low, high))
        print('stored at %s, compression "%s"' % (image.detail("location"), image.detail("compression")))

        # The FITS keywords of the image: cards of name, value and comment, in their order.
        cards = image.keywords
        print("%d keywords, the first of them:" % len(cards))
        for card in cards[:4]:
            print("  %-8s= %r / %s" % (card.name, card.value, card.comment))
        # One card by its name: the value, as the Python type it is.
        print("the object is %s" % cards.get("OBJECT", "not named"))

        # XISF properties: typed values with an id, of the image and of the file. A vector or a
        # matrix is a NumPy array.
        for title, properties in (("image", image.properties), ("file", f.properties)):
            print("%d properties of the %s:" % (len(properties), title))
            for key, value in properties.items():
                text = "<a vector or matrix>" if isinstance(value, np.ndarray) else shortened(str(value))
                print("  %s (%s) = %s" % (key, properties.type(key), text))
    return 0
# [/inspect]


# [pixels]
def pixels(path):
    # The first image as 32-bit floating point, whatever the file holds: [height, width] for one
    # channel, [height, width, channels] for several, row 0 at the top.
    data = xisfconv.read(path, sample_format="float32")
    print("%d samples: minimum %.6f, maximum %.6f, mean %.6f"
          % (data.size, data.min(), data.max(), data.mean(dtype=np.float64)))
    height, width = data.shape[:2]
    print("the pixel in the middle of channel 0: %.6f" % data[height // 2, width // 2, ...].flat[0])

    # Rows counted from the bottom, as FITS has them, and the planes first, [channels, height, width],
    # as much array code wants a colour image: the same call says so.
    other = xisfconv.read(path, sample_format="float32", row_order="bottom-up", channels="first")
    print("read bottom-up, the first row is the last row of the other: %s"
          % ("yes" if np.array_equal(other[..., 0, :], data[-1].T) else "no"))
    return 0
# [/pixels]


# [stretch]
def stretch(path):
    # The image with what describes it. Its bounds are the range of the samples: those of the file
    # for floating point, 0 to 1 for integers that were read as floating point.
    image = xisfconv.read_image(path, sample_format="float32")

    # PixInsight's automatic screen stretch: shadows, midtones and highlights for each colour
    # channel, found from the data; here with the statistics of the channels shared. The
    # functions are told the range of the samples.
    params = xisfconv.auto_stretch(image.data, linked=True, bounds=image.bounds)
    shown = xisfconv.apply_stretch(image.data, params, bounds=image.bounds)
    print("shadows %.6f, midtones %.6f, highlights %.6f"
          % (params[0].shadows, params[0].midtones, params[0].highlights))

    # The stretched samples are 0 to 1: as bytes they are a picture any program shows. The format
    # follows the name of the file.
    picture = np.floor(shown * 255 + 0.5).astype(np.uint8)
    xisfconv.write(out_path("stretched.png"), picture, overwrite=True)
    print("wrote stretched.png, %d x %d, 8 bits" % (picture.shape[1], picture.shape[0]))
    return 0
# [/stretch]


# [write]
def write_crop(path):
    # The image with everything that describes it: keywords, name, bounds, properties.
    image = xisfconv.read_image(path, sample_format="float32")
    height, width = image.data.shape[:2]

    # A square from the middle of the frame.
    side = min(width, height, 512)
    left, top = (width - side) // 2, (height - side) // 2
    crop = image.data[top:top + side, left:left + side]

    # Keywords. A part of a frame is another image. The cards that tell of the instrument and of
    # the observation hold for it as well; those that tell where a pixel is (WCS keywords,
    # BAYERPAT) would be wrong for it. So the cards that hold are taken by their names, and two
    # of our own are added.
    kept = ("INSTRUME", "TELESCOP", "OBJECT", "DATE-OBS", "EXPTIME")
    cards = xisfconv.Keywords([card for name in kept for card in image.keywords.cards(name)[:1]])
    cards.append("CROPPED", "the middle of the frame", "what this is")
    cards.append("CROPSIZE", side, "pixels")

    # XISF properties: a number, a text, a date and a vector. A value is written with the XISF type
    # that goes with its Python type; set() states the type where another one is wanted.
    properties = xisfconv.PropertyDict()
    properties.set("Tour:Side", side, type="UInt32")
    properties["Tour:Note"] = "cut out by tour.py"
    properties["Tour:Made"] = datetime.datetime(2026, 10, 7, 12, 0, tzinfo=datetime.timezone.utc)
    properties.set("Tour:Scale", np.array([0.85, 0.85]), comment="arcseconds per pixel")

    xisfconv.write(out_path("crop.xisf"), crop, name="crop", keywords=cards, properties=properties,
                   bounds=image.bounds,   # the range of the frame, not one guessed from this part of it
                   codec="zlib",          # with byte shuffling, as PixInsight compresses
                   checksum="sha256", creator="tour.py", overwrite=True)
    print("wrote crop.xisf: %d x %d pixels, compressed with zlib, SHA-256 checksum" % (side, side))
    return 0
# [/write]


# [convert]
def convert(path):
    # To FITS, with everything the tool does: rows turned bottom-up, keywords, properties.
    xisfconv.convert(path, out_path("frame.fits"), overwrite=True)
    print("wrote frame.fits")

    # A picture to look at: stretched, 8 bits, its longest side 800 pixels.
    xisfconv.convert(path, out_path("preview.png"), stretch="auto", sample_format="uint8",
                     resize=800, overwrite=True)
    print("wrote preview.png")
    return 0
# [/convert]


# [rewrite]
def rewrite_and_verify(path):
    # The same file with its data blocks compressed and a checksum on each. Pixels, keywords and
    # properties are not touched: the blocks are stored another way, nothing else.
    how = dict(codec="default",           # Zstandard, or zlib in a build without it
               checksum="sha1")
    result = xisfconv.rewrite(path, out_path("smaller.xisf"), overwrite=True, **how)
    print("%d -> %d bytes: %d block(s) compressed, %d kept, %d checksum(s), read back: %s"
          % (result.input_size, result.output_size, result.compressed, result.kept, result.checksums,
             "yes" if result.read_back else "no"))

    # Is a file stored the way these options ask? (The header tells; nothing else is read.)
    as_asked = xisfconv.stored_as_requested(out_path("smaller.xisf"), **how)
    print("smaller.xisf is stored as asked: %s" % ("yes" if as_asked else "no"))

    # Verification reads everything and converts nothing. A damaged file is not an exception: it is
    # a report that says "failed", and why.
    report = xisfconv.verify(out_path("smaller.xisf"))
    print("verdict %s: %s; %d checksum(s) verified"
          % ("OK" if report.ok else "not OK", report.summary, report.verified))
    for problem in report.problems:
        print("  problem: %s" % problem)
    return 0
# [/rewrite]


# [units]
def units(path):
    # The kind of unit follows the name of the output: under a name that ends in .xish the header
    # goes there and every data block into the file of that name that ends in .xisb.
    xisfconv.rewrite(path, out_path("unit.xish"), overwrite=True)

    # Only the header file is ever named. It says where the data is.
    with xisfconv.open(out_path("unit.xish")) as f:
        print("unit.xish is a %s unit: header %d bytes, %d bytes with its data"
              % (f.unit, f.size, f.unit_size))
        for other in f.external_files:
            print("  data in %s" % os.path.basename(other))

    # A header is followed to files in its own directory. With the setting that follows it to no
    # other file, the pixels are refused, and the exception says that this is the reason.
    try:
        xisfconv.read(out_path("unit.xish"), external_files="none")
        print('with external_files="none": read')
    except xisfconv.NotAllowedError:          # which is a PermissionError
        print('with external_files="none": not allowed')
    xisfconv.read(out_path("unit.xish"))
    print("with the default: read")

    # And back into one file, for PixInsight, which opens monolithic files only.
    xisfconv.rewrite(out_path("unit.xish"), out_path("packed.xisf"), overwrite=True)
    print("packed into packed.xisf")
    return 0
# [/units]


# [wcs]
def wcs(path):
    with xisfconv.open(path) as f:
        image = f[0]
        print("astrometric solution: %s" % ("yes" if image.has_astrometric_solution else "none"))

        # WCS keywords for rows counted from the bottom, as FITS has them: those of the file, or
        # made from PixInsight's solution, its distortion fitted with SIP polynomials of order 3.
        try:
            cards = image.wcs_keywords("bottom-up", sip_order=3)
        except xisfconv.NotFoundError:
            print("this image has no solution to make WCS keywords from")
            return 0
        print("%d WCS keywords%s" % (len(cards), "; " + cards.fit_summary if cards.fit_summary else ""))
        for card in cards[:8]:
            print("  %-8s= %r" % (card.name, card.value))
    return 0
# [/wcs]


# [progress]
class Stop(Exception):
    pass


def progress_and_errors(path):
    # What a failure looks like: an exception of the class for the case, with the text for it.
    try:
        xisfconv.open("no such file.xisf")
    except xisfconv.Error as error:
        print("%s (a FileNotFoundError: %s): %s"
              % (type(error).__name__, "yes" if isinstance(error, FileNotFoundError) else "no", error))

    # Messages: the notes of a conversion to FITS and back, through the logger "xisfconv".
    log = logging.getLogger("xisfconv")
    notes = Notes()
    log.addHandler(notes)
    log.setLevel(logging.INFO)
    try:
        xisfconv.convert(path, out_path("there.fits"), overwrite=True)
        xisfconv.convert(out_path("there.fits"), out_path("back.xisf"), overwrite=True)
    finally:
        log.removeHandler(notes)

    # Progress, and a call that is stopped by its handler: the exception it raises ends the work,
    # leaves no partly written file, and comes out of the call.
    calls = []

    def watch(stage, done, total):
        calls.append(stage)
        print("  %s %d of %d" % (stage, done, total))
        if len(calls) >= 2:
            raise Stop()

    try:
        xisfconv.rewrite(path, out_path("stopped.xisf"), codec="zlib", overwrite=True, progress=watch)
        return 1
    except Stop:
        left = os.path.exists(out_path("stopped.xisf"))
        print("stopped: by the handler; a file is left: %s" % ("yes" if left else "no"))
    return 0
# [/progress]


# [astropy]
def with_astropy(path):
    try:
        from astropy.nddata import CCDData
        import xisfconv.astropy             # registers the format "xisf" with astropy
    except ImportError:
        print("astropy is not installed: this chapter is left out")
        return 0

    # CCDData reads an XISF file like any other: astropy knows it by its first bytes.
    ccd = CCDData.read(path, unit="adu")
    print("CCDData of %s, %s, in %s, with %d header cards"
          % (ccd.shape, ccd.dtype, ccd.unit, len(ccd.header)))
    print("OBJECT = %s, ROWORDER = %s" % (ccd.header.get("OBJECT"), ccd.header.get("ROWORDER")))

    # Everything here follows the FITS conventions, as astropy does: row 0 is the bottom of the
    # image, and a colour image is [channels, height, width].
    top_down = xisfconv.read(path)
    print("row 0 of the CCDData is the last row of xisfconv.read(): %s"
          % ("yes" if np.array_equal(ccd.data[..., 0, :], top_down[-1].T) else "no"))

    # ... and writes one, under a name that ends in .xisf.
    ccd.write(out_path("ccd.xisf"), codec="zlib", overwrite=True)
    print("wrote ccd.xisf")

    # The same two steps for astropy.io.fits: every image of the file as an HDU.
    hdulist = xisfconv.astropy.read_hdulist(path)
    first = hdulist[0]
    print("%d HDU(s), the first of %s with %d cards" % (len(hdulist), first.data.shape, len(first.header)))
    xisfconv.astropy.write_hdulist(hdulist, out_path("hdus.xisf"), overwrite=True)
    print("wrote hdus.xisf")
    return 0
# [/astropy]


# [compat]
def like_the_xisf_package(path):
    from xisfconv.xisf import XISF          # was: from xisf import XISF

    frame = XISF(path)
    about_file = frame.get_file_metadata()
    about_images = frame.get_images_metadata()
    pixels = frame.read_image(0)            # [height, width, channels], also for one channel
    about = about_images[0]
    print("geometry %s, %s, pixels of %s" % (about["geometry"], about["sampleFormat"], pixels.shape))
    print("made by %s" % about_file.get("XISF:CreatorApplication", {}).get("value", "a program without a name"))
    print("%d FITS keywords, %d XISF properties" % (len(about["FITSKeywords"]), len(about["XISFProperties"])))

    XISF.write(out_path("compat.xisf"), pixels, creator_app="tour.py", image_metadata=about,
               xisf_metadata=about_file, codec="zlib", shuffle=True)
    print("wrote compat.xisf")
    return 0
# [/compat]


# [main]
CHAPTERS = [("inspect", inspect), ("pixels", pixels), ("stretch", stretch), ("write", write_crop),
            ("convert", convert), ("rewrite", rewrite_and_verify), ("units", units), ("wcs", wcs),
            ("progress", progress_and_errors), ("astropy", with_astropy), ("compat", like_the_xisf_package)]


def main(argv):
    global OUT
    if len(argv) < 3:
        print("usage: %s <image.xisf> <output directory> [chapter]" % argv[0], file=sys.stderr)
        return 2
    OUT = argv[2]

    # The chapters ask about what an XISF file has. (first.py takes a FITS or ASDF file too.)
    try:
        if xisfconv.detect_format(argv[1]) != "xisf":
            print("%s is not an XISF file" % argv[1], file=sys.stderr)
            return 2
    except xisfconv.Error as error:
        return report(argv[1], error)
    bad = ran = 0
    for name, run in CHAPTERS:
        if len(argv) > 3 and argv[3] != name:
            continue
        print("== %s" % name)
        ran += 1
        try:
            bad += run(argv[1])
        except Exception as error:            # xisfconv.Error, or what NumPy or astropy raise
            bad += report(name, error)
    if not ran:
        print('%s: there is no chapter "%s"' % (argv[0], argv[3]), file=sys.stderr)
        return 2
    return 1 if bad else 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):    # (where the output cannot show a character, a mark stands for it)
        sys.stdout.reconfigure(errors="backslashreplace")
    sys.exit(main(sys.argv))
# [/main]
