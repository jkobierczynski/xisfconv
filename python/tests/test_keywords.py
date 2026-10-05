# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""The Keywords class, and keywords on their way through files."""

import numpy as np
import pytest

import xisfconv
from xisfconv import Card, Keywords
from util import sample

fits = pytest.importorskip("astropy.io.fits")


def test_made_from():
    as_list = Keywords([("A", 1), ("B", "two", "a comment"), Card("C", 3.5), ("D",)])
    assert [tuple(card) for card in as_list] == [("A", 1, ""), ("B", "two", "a comment"), ("C", 3.5, ""), ("D", None, "")]
    as_dict = Keywords({"A": 1, "B": ("two", "a comment"), "C": 3.5, "D": None})
    assert as_dict == as_list
    assert Keywords(as_list) == as_list and Keywords(as_list) is not as_list
    header = fits.Header([("A", 1), ("B", "two", "a comment"), ("C", 3.5), ("D", None)])
    assert Keywords(header) == as_list
    assert Keywords() == [] and len(Keywords(None)) == 0 and not Keywords()
    assert Keywords(iter([("A", 1)])) == [("A", 1, "")]


def test_values():
    keywords = Keywords()
    keywords.append("INT", np.int16(7))
    keywords.append("FLOAT", np.float32(0.5))
    keywords.append("BOOL", np.bool_(True))
    keywords.append("STR", np.str_("text"))
    assert [type(card.value) for card in keywords] == [int, float, bool, str]
    keywords.append("HISTORY", "as a value")
    keywords.append("HISTORY", None, "as a comment")
    keywords.append("COMMENT", "", "also")
    assert [tuple(card) for card in keywords[4:]] == [("HISTORY", "as a value", ""), ("HISTORY", "as a comment", ""),
                                                     ("COMMENT", "also", "")]
    for bad in ([1, 2], {"a": 1}, b"bytes", object()):
        with pytest.raises(TypeError):
            keywords.append("BAD", bad)
    with pytest.raises(TypeError):
        keywords.append(5, 1)


def test_as_a_list_and_by_name():
    keywords = Keywords([("A", 1), ("B", 2), ("HISTORY", "x"), ("b", 3), ("HISTORY", "y")])
    assert len(keywords) == 5 and keywords[0] == ("A", 1, "") and keywords[-1].value == "y"
    assert keywords["B"] == 2 and keywords["b"] == 2          # the first of the name, whatever the case
    assert [card.value for card in keywords.cards("b")] == [2, 3]
    assert keywords.names() == ["A", "B", "HISTORY", "b"]
    assert keywords.to_dict() == {"A": 1, "B": 2, "b": 3}
    assert "A" in keywords and "a" in keywords and "Z" not in keywords and ("A", 1, "") in keywords
    assert isinstance(keywords[1:3], Keywords) and keywords[1:3] == [("B", 2, ""), ("HISTORY", "x", "")]

    keywords["A"] = 10                                        # replaces the value, keeps the comment
    keywords["NEW"] = ("value", "with a comment")
    keywords[1] = ("B", 20, "by index")
    assert keywords["A"] == 10 and keywords["NEW"] == "value" and keywords[-1].comment == "with a comment"
    assert keywords[1] == ("B", 20, "by index")
    keywords.insert(0, ("FIRST", True))
    assert keywords[0].name == "FIRST"
    keywords.extend([("X", 1), ("Y", 2)])
    assert keywords.names()[-2:] == ["X", "Y"]
    del keywords["history"]
    assert "HISTORY" not in keywords
    del keywords[0]
    assert keywords[0].name == "A"
    with pytest.raises(KeyError):
        del keywords["HISTORY"]
    with pytest.raises(IndexError):
        keywords[99]
    copy = keywords.copy()
    copy["A"] = 0
    assert keywords["A"] == 10
    assert "Keywords" in repr(keywords) and "Keywords" in repr(Keywords([("A", 1)]))
    with pytest.raises(TypeError):
        hash(keywords)


def test_fits_text_is_what_astropy_parses():
    keywords = Keywords([
        ("OBJECT", "M 31", "target"), ("EXPTIME", 300.5, "seconds"), ("GAIN", 139), ("FLAG", True), ("OFF", False),
        ("NOVALUE", None, "nothing"), ("QUOTE", "it's"), ("LONG", "word " * 40), ("A Long Name", 1.5),
        ("TINY", 1.25e-300), ("HUGE", -9.5e+250), ("WHOLE", 3.0), ("HISTORY", "h" * 100), ("COMMENT", "said"),
        ("UNICODE", "café ☃"), ("BZERO", 32768), ("NAXIS1", 10), ("SIMPLE", True),
    ])
    text = keywords.fits_text()
    assert len(text) % 80 == 0 and all(" " <= c <= "~" for c in text)
    header = fits.Header.fromstring(text)
    assert header["OBJECT"] == "M 31" and header.comments["OBJECT"] == "target"
    assert header["EXPTIME"] == 300.5 and header["GAIN"] == 139 and header["FLAG"] is True and header["OFF"] is False
    assert header["NOVALUE"] is None or isinstance(header["NOVALUE"], fits.card.Undefined)
    assert header.comments["NOVALUE"] == "nothing" and header["QUOTE"] == "it's"
    assert header["LONG"] == ("word " * 40).rstrip() and header["A Long Name"] == 1.5
    assert header["TINY"] == 1.25e-300 and header["HUGE"] == -9.5e+250
    assert header["WHOLE"] == 3.0 and isinstance(header["WHOLE"], float)
    assert "".join(header["HISTORY"]) == "h" * 100 and list(header["COMMENT"]) == ["said"]
    assert header["UNICODE"] == "caf? ?"                    # FITS headers are ASCII
    for structural in ("BZERO", "NAXIS1", "SIMPLE"):
        assert structural not in header
    assert Keywords().fits_text() == ""
    # and back: a header of astropy as keywords
    again = Keywords(header)
    assert again["LONG"] == ("word " * 40).rstrip() and again["A Long Name"] == 1.5 and again["NOVALUE"] is None


@pytest.mark.parametrize("extension", ["xisf", "fits", "asdf"])
def test_keywords_come_back(tmp_path, extension):
    cards = [("OBJECT", "NGC 7000", "target"), ("EXPTIME", 12.5, "seconds"), ("NCOMBINE", 40, ""), ("DARKSUB", True, ""),
             ("FLATDIV", False, ""), ("QUOTE", "it's", ""), ("HISTORY", "step one", ""), ("HISTORY", "step two", ""),
             ("COMMENT", "said", ""), ("TINY", 1.25e-12, "small")]
    path = tmp_path / ("k." + extension)
    xisfconv.write(path, sample("uint16", (5, 6)), keywords=cards)
    with xisfconv.open(path) as file:
        back = file[0].keywords
    back = [card for card in back if card.name != "PROGRAM"]     # the FITS and ASDF writers name themselves
    assert [tuple(card) for card in back] == cards
    assert [type(card.value) for card in back] == [type(value) for _, value, _ in cards]


def test_values_keep_their_text(tmp_path):
    """A value that is not touched goes back out as it was written in the file."""
    pytest.importorskip("xisf")
    from util import xisf_keywords, xisf_read, xisf_write

    odd = {"EXPTIME": ("3.0000000E+02", "padded"), "RATIO": ("0.10", "trailing zero"), "BIGINT": ("0042", "leading zeros")}
    xisf_write(tmp_path / "in.xisf", sample("uint8", (4, 4)), keywords=odd)
    image = xisfconv.read_image(tmp_path / "in.xisf")
    assert image.keywords["EXPTIME"] == 300.0 and image.keywords["BIGINT"] == 42
    xisfconv.write(tmp_path / "same.xisf", image)
    cards = xisf_keywords(xisf_read(tmp_path / "same.xisf")[1])
    assert {name: cards[name][0][0] for name in odd} == {name: text for name, (text, _) in odd.items()}
    image.keywords["RATIO"] = 0.25                           # a replaced value is written anew
    xisfconv.write(tmp_path / "changed.xisf", image)
    cards = xisf_keywords(xisf_read(tmp_path / "changed.xisf")[1])
    assert cards["RATIO"][0] == ("0.25", "trailing zero") and cards["EXPTIME"][0][0] == "3.0000000E+02"
