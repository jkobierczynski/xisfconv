#!/usr/bin/env python3
"""The digest of the WCS that xisfconv stores with XISF properties in a FITS file (WCSDIGST), computed
without xisfconv: for programs that read or write those tables themselves.

    python wcs_digest.py image.fits            (a file xisfconv converted from an XISF image with properties)

docs/xisf-properties-in-fits-and-asdf.md says what the digest is for and what goes into it. Needs astropy.

SPDX-License-Identifier: LGPL-3.0-or-later
Copyright (C) 2026 Jurgen Kobierczynski
"""
import hashlib
import math
import re
import sys


def is_wcs_keyword(name):
    """True for the keywords that describe a world coordinate system, its distortion included, in the
    primary description and in the alternate ones (CRVAL1A), in their old spellings too."""
    if name in ("A_ORDER", "B_ORDER", "AP_ORDER", "BP_ORDER", "A_DMAX", "B_DMAX", "EPOCH"):
        return True
    patterns = (r"(A|B|AP|BP)_\d+_\d+",                                           # the SIP polynomials
                r"WAT\d_\d{3}",                                                   # IRAF: TNX, ZPX
                r"(CPDIS|CQDIS|CPERR|CQERR|D2IMDIS|D2IMERR|D2IM).+",              # the distortion paper
                r"(DP|DQ)\d+[A-Z]?",
                r"(PC|CD)\d{6}",                                                  # PC001002, as it once was
                r"(WCSAXES|LONPOLE|LATPOLE|RADESYS|RADECSYS|EQUINOX)[A-Z]?",
                r"(CTYPE|CUNIT|CRVAL|CRPIX|CDELT|CROTA)\d+[A-Z]?",
                r"(CD|PC|PV|PS)\d+_\d+[A-Z]?")
    return any(re.fullmatch(pattern, name) for pattern in patterns)


def number_text(value):
    """A number as the digest has it: the fewest of 15, 16 or 17 digits that give the same double back."""
    if value == 0:
        return "0"                              # (also for -0)
    for precision in (15, 16, 17):
        text = "%.*g" % (precision, value)
        if float(text) == value:
            break
    return text


def value_text(card):
    """The value of a card (an astropy Card) as the digest has it."""
    image = card.image
    if image.startswith("HIERARCH "):
        field = image.partition("=")[2]
    else:
        field = image[10:] if image[8:10] == "= " else None
    if field is None:
        return ""                               # a card without "= " in columns 9 and 10 has no value
    value = card.rawvalue                       # (of DP1 = 'EXTVER: 1' the text: astropy makes a number of it)
    if isinstance(value, str):
        return value.rstrip(" ")                # a text counts as its content, without the quotes
    written = field.split("/")[0].strip()       # everything else is taken as the card writes it
    number = written.upper().replace("D", "E")
    if re.fullmatch(r"[+-]?(\d+\.?\d*|\.\d+)(E[+-]?\d+)?", number) and math.isfinite(float(number)):
        return number_text(float(number))       # a number as its value, however the card writes it
    return written                              # T or F, a complex value, no value at all


def wcs_digest(header, width, height, bottom_up):
    """The digest for the header of an image (an astropy Header), the size of the image, and the order its
    rows are stored in."""
    entries = []
    for card in header.cards:
        name = card.rawkeyword.upper()
        if is_wcs_keyword(name):
            entries.append("%s=%s" % (name, value_text(card)))
    lines = ["%dx%d %s" % (width, height, "bottom-up" if bottom_up else "top-down")] + sorted(entries)
    return hashlib.sha1("".join(line + "\n" for line in lines).encode()).hexdigest()


def main(path):
    from astropy.io import fits
    with fits.open(path) as hdul:
        for at, hdu in enumerate(hdul[:-1]):
            table = hdul[at + 1]
            if table.name != "XISF_PROPERTIES" or hdu.data is None:
                continue
            stored = table.header.get("WCSDIGST")
            order = hdu.header.get("ROWORDER")
            bottom_up = not (isinstance(order, str) and order.rstrip(" ").upper() == "TOP-DOWN")
            mine = wcs_digest(hdu.header, hdu.data.shape[-1], hdu.data.shape[-2], bottom_up)
            if stored is None:
                print("HDU %d: %s (the table has no digest: its properties were not written from an XISF file)" % (at, mine))
            else:
                print("HDU %d: %s, the table says %s: %s" % (
                    at, mine, stored, "the WCS keywords, the size and the row order are what they were" if mine == stored
                    else "the WCS keywords, the size or the row order changed since"))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
