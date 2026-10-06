# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Helpers of the tests: test images, and the other packages as the tests use them."""

import base64

import numpy as np

DTYPES = ["uint8", "uint16", "uint32", "uint64", "float32", "float64"]


def sample(dtype, shape, seed=1):
    """A test image: every row, column and channel different, integers over their full range,
    floating point within 0..1. `shape` is (height, width) or (height, width, channels)."""
    rng = np.random.default_rng(seed)
    dtype = np.dtype(dtype)
    if dtype.kind == "f":
        data = rng.random(shape).astype(dtype)
    else:
        data = rng.integers(0, np.iinfo(dtype).max, shape, dtype=dtype, endpoint=True)
    flat = data.reshape(-1)
    flat[0] = 0
    flat[-1] = 1 if dtype.kind == "f" else np.iinfo(dtype).max
    return data


def same(a, b):
    """Equal in shape, sample type (whatever the byte order) and every value."""
    a, b = np.asarray(a), np.asarray(b)
    return a.shape == b.shape and a.dtype.kind == b.dtype.kind and a.dtype.itemsize == b.dtype.itemsize \
        and np.array_equal(a, b, equal_nan=a.dtype.kind == "f")


def planes_last(a):
    """[height, width] or [height, width, channels] as [height, width, channels]."""
    return a[:, :, np.newaxis] if a.ndim == 2 else a


def xisf_write(path, data, keywords=None, **options):
    """Writes an XISF file with the xisf package. `data` has row 0 at the top and the channels
    last; `keywords` is {name: (value text, comment)}."""
    from xisf import XISF

    metadata = {}
    if keywords:
        metadata["FITSKeywords"] = {name: [{"value": value, "comment": comment}]
                                    for name, (value, comment) in keywords.items()}
    XISF.write(str(path), planes_last(data), image_metadata=metadata, xisf_metadata={}, **options)


def xisf_read(path, image=0):
    """(array [height, width, channels], metadata) of an image, read by the xisf package."""
    from xisf import XISF

    file = XISF(str(path))
    return np.asarray(file.read_image(image)), file.get_images_metadata()[image]


def xisf_keywords(metadata):
    """{name: [(value, comment), ...]} as the xisf package read them."""
    return {name: [(card["value"], card["comment"]) for card in cards]
            for name, cards in metadata.get("FITSKeywords", {}).items()}


def handmade(path, elements, attachments=()):
    """An XISF file from the text of the elements inside <xisf>. An attachment is put where
    "@n" stands in the text: attachment:@0 becomes attachment:position:size."""
    def header(positions):
        text = ('<?xml version="1.0" encoding="UTF-8"?>\n<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">'
                + elements + '</xisf>')
        for number, block in enumerate(attachments):
            text = text.replace("@%d" % number, "%d:%d" % (positions[number], len(block)))
        return text.encode()
    positions = [0] * len(attachments)
    for _ in range(4):       # (the positions have digits, which move the positions)
        at = 16 + len(header(positions))
        placed = []
        for block in attachments:
            placed.append(at)
            at += len(block)
        if placed == positions:
            break
        positions = placed
    xml = header(positions)
    path.write_bytes(b"XISF0100" + len(xml).to_bytes(4, "little") + bytes(4) + xml + b"".join(attachments))
    return path


def inline(array):
    return 'location="inline:base64">%s' % base64.b64encode(np.asarray(array).tobytes()).decode()
