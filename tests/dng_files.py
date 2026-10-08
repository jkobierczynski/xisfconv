"""DNG files for the tests, written byte by byte without xisfconv.

SPDX-License-Identifier: GPL-3.0-or-later

A DNG file is a TIFF file with tags of its own (Adobe's DNG specification 1.7). write_dng()
writes the raw image of a camera the way cameras and Adobe DNG Converter do: a small preview in
IFD 0 and the raw data in a SubIFD (or the raw data in IFD 0 itself), uncompressed (samples of 1
to 32 bits), compressed with lossless JPEG (ITU-T T.81, process 14, the encoder below), or with
Deflate and a horizontal predictor; in strips or in tiles; with the EXIF directory, an active
area, a linearization table, black and white levels.

The lossless JPEG encoder ljpeg() is written from T.81 alone. The tests check it against
imagecodecs, an independent decoder, wherever that is installed.
"""
import struct
import zlib

import numpy as np

# TIFF field types
BYTE, ASCII, SHORT, LONG, RATIONAL, SBYTE, UNDEFINED, SSHORT, SLONG, SRATIONAL, FLOAT, DOUBLE = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12
_SIZE = {BYTE: 1, ASCII: 1, SHORT: 2, LONG: 4, RATIONAL: 8, SBYTE: 1, UNDEFINED: 1, SSHORT: 2, SLONG: 4, SRATIONAL: 8,
         FLOAT: 4, DOUBLE: 8}
_FORMAT = {BYTE: "B", ASCII: "B", SHORT: "H", LONG: "I", SBYTE: "b", UNDEFINED: "B", SSHORT: "h", SLONG: "i", FLOAT: "f",
           DOUBLE: "d"}


# ------------------------------------------------------------------------------------------
# Lossless JPEG
# ------------------------------------------------------------------------------------------

def _category(d):
    return 0 if d == 0 else abs(d).bit_length()


def huffman_table(kind="flat"):
    """(counts of codes of each length 1..16, the symbols): the categories 0..16 of a difference.
    flat: every code 5 bits long. skewed: codes from 2 to 16 bits, the longest for the small
    categories (which are the common ones), so that a decoder's way for long codes is taken."""
    if kind == "flat":
        lengths = {s: 5 for s in range(17)}
    else:
        order = [16, 15, 14, 13, 12, 11, 10, 9, 0, 1, 2, 3, 4, 5, 6, 7, 8]    # short codes first
        lengths = dict(zip(order, [2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 16]))
    counts = [0] * 16
    symbols = sorted(range(17), key=lambda s: (lengths[s], s))
    for s in symbols:
        counts[lengths[s] - 1] += 1
    return counts, symbols


def _codes(counts, symbols):
    """The canonical codes (T.81 Annex C): symbol -> (code, length)."""
    out, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            out[symbols[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return out


class _Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, value, n):
        self.acc = (self.acc << n) | (value & ((1 << n) - 1))
        self.n += n
        while self.n >= 8:
            self.n -= 8
            b = (self.acc >> self.n) & 0xFF
            self.out.append(b)
            if b == 0xFF:
                self.out.append(0)
        self.acc &= (1 << self.n) - 1

    def pad(self):   # to a whole byte, with 1 bits
        if self.n:
            self.put((1 << (8 - self.n)) - 1, 8 - self.n)


def predict(p, a, b, c):
    return {1: a, 2: b, 3: c, 4: a + b - c, 5: a + ((b - c) >> 1), 6: b + ((a - c) >> 1), 7: (a + b) >> 1}[p]


def ljpeg(samples, precision, predictor=1, transform=0, restart_lines=0, table="flat", tables_per_component=False,
          markers_before=b"", fill_ff=False):
    """Encodes `samples` (lines x width x components, integers below 2**precision) as one lossless
    JPEG stream: SOF3, one interleaved scan, the given predictor and point transform, a restart
    interval of `restart_lines` lines if not 0. The components are interleaved, one sample of each
    per pixel (T.81: an MCU of a non-subsampled lossless scan)."""
    a = np.asarray(samples)
    if a.ndim == 2:
        a = a[:, :, None]
    lines, width, comps = a.shape
    counts, symbols = huffman_table(table)
    codes = _codes(counts, symbols)
    out = bytearray(b"\xFF\xD8") + markers_before
    nt = comps if tables_per_component else 1
    dht = bytearray()
    for t in range(nt):
        dht += bytes([t]) + bytes(counts) + bytes(symbols)
    out += b"\xFF\xC4" + struct.pack(">H", 2 + len(dht)) + dht
    sof = struct.pack(">BHHB", precision, lines, width, comps)
    for c in range(comps):
        sof += bytes([c + 1, 0x11, 0])
    out += b"\xFF\xC3" + struct.pack(">H", 2 + len(sof)) + sof
    if restart_lines:
        out += b"\xFF\xDD" + struct.pack(">HH", 4, restart_lines * width)
    sos = bytes([comps]) + b"".join(bytes([c + 1, (c if tables_per_component else 0) << 4]) for c in range(comps))
    sos += bytes([predictor, 0, transform])
    out += b"\xFF\xDA" + struct.pack(">H", 2 + len(sos)) + sos
    bits = _Bits()
    x = (a.astype(np.int64) >> transform)
    initial = 1 << (precision - transform - 1)
    rst = 0
    for y in range(lines):
        first = y == 0
        if restart_lines and y and y % restart_lines == 0:
            bits.pad()
            out += bits.out
            bits.out = bytearray()
            out += (b"\xFF" if fill_ff else b"") + bytes([0xFF, 0xD0 + rst])
            rst = (rst + 1) & 7
            first = True
        for i in range(width):
            for c in range(comps):
                if first:
                    pred = initial if i == 0 else int(x[y, i - 1, c])
                elif i == 0:
                    pred = int(x[y - 1, i, c])
                else:
                    pred = predict(predictor, int(x[y, i - 1, c]), int(x[y - 1, i, c]), int(x[y - 1, i - 1, c]))
                d = (int(x[y, i, c]) - pred) & 0xFFFF
                if d >= 32768:
                    d -= 65536
                s = _category(d)
                code, length = codes[s]
                bits.put(code, length)
                if 0 < s < 16:
                    bits.put(d if d > 0 else d + (1 << s) - 1, s)
    bits.pad()
    out += bits.out
    out += b"\xFF\xD9"
    return bytes(out)


# ------------------------------------------------------------------------------------------
# The samples of a strip or tile
# ------------------------------------------------------------------------------------------

def pack_rows(chunk, bits, order):
    """A strip or tile as DNG has it uncompressed: rows of samples; 8, 16 and 32 bits in the byte
    order of the file, every other size packed with the highest bit first (DNG's rule, also in a
    little-endian file), each row filled to a whole byte."""
    rows = chunk.reshape(chunk.shape[0], -1).astype(np.uint64)
    if bits in (8, 16, 32):
        dt = {8: "u1", 16: "u2", 32: "u4"}[bits]
        return rows.astype(order + dt).tobytes()
    out = bytearray()   # (DNG: every other size, 24 bits too, with the highest bit first in either byte order)
    for row in rows:
        acc, n = 0, 0
        line = bytearray()
        for v in row:
            acc = (acc << bits) | int(v)
            n += bits
            while n >= 8:
                n -= 8
                line.append((acc >> n) & 0xFF)
            acc &= (1 << n) - 1
        if n:
            line.append((acc << (8 - n)) & 0xFF)
        out += line
    return bytes(out)


def difference(chunk, bits, back):
    """TIFF's horizontal predictor: each sample less the one `back` samples to its left in its row."""
    rows = chunk.reshape(chunk.shape[0], -1).astype(np.int64)
    out = rows.copy()
    out[:, back:] = rows[:, back:] - rows[:, :-back]
    return (out & ((1 << bits) - 1)).astype(np.uint64)


# ------------------------------------------------------------------------------------------
# The TIFF structure
# ------------------------------------------------------------------------------------------

def _value(order, typ, values):
    if typ == ASCII:
        data = values.encode("latin-1") + b"\0" if isinstance(values, str) else bytes(values)
        return data, len(data)
    if typ == UNDEFINED and isinstance(values, (bytes, bytearray)):
        return bytes(values), len(values)
    if not isinstance(values, (list, tuple)):
        values = [values]
    if typ in (RATIONAL, SRATIONAL):
        data = b"".join(struct.pack(order + ("II" if typ == RATIONAL else "ii"), *v) for v in values)
    else:
        data = struct.pack(order + _FORMAT[typ] * len(values), *values)
    return data, len(values)


def ifd_bytes(order, offset, entries, next_ifd=0):
    """One directory at `offset`: entries is {tag: (type, values)}; values longer than 4 bytes
    follow the directory."""
    tags = sorted(entries)
    head = struct.pack(order + "H", len(tags))
    extra = bytearray()
    start = offset + 2 + 12 * len(tags) + 4
    body = bytearray()
    for tag in tags:
        typ, values = entries[tag]
        data, count = _value(order, typ, values)
        if len(data) <= 4:
            field = data + b"\0" * (4 - len(data))
        else:
            if (start + len(extra)) % 2:
                extra += b"\0"
            field = struct.pack(order + "I", start + len(extra))
            extra += data
        body += struct.pack(order + "HHI", tag, typ, count) + field
    return head + bytes(body) + struct.pack(order + "I", next_ifd) + bytes(extra)


class Tiff:
    """A TIFF file put together piece by piece: blobs first, directories last."""

    def __init__(self, order="<"):
        self.order = order
        self.data = bytearray(b"II*\0" if order == "<" else b"MM\0*") + b"\0\0\0\0"

    def add(self, blob):
        if len(self.data) % 2:
            self.data += b"\0"
        at = len(self.data)
        self.data += blob
        return at

    def add_ifd(self, entries, next_ifd=0):
        if len(self.data) % 2:
            self.data += b"\0"
        at = len(self.data)
        self.data += ifd_bytes(self.order, at, entries, next_ifd)
        return at

    def finish(self, first):
        self.data[4:8] = struct.pack(self.order + "I", first)
        return bytes(self.data)


# ------------------------------------------------------------------------------------------
# DNG
# ------------------------------------------------------------------------------------------

CFA = {"RGGB": [0, 1, 1, 2], "BGGR": [2, 1, 1, 0], "GRBG": [1, 0, 2, 1], "GBRG": [1, 2, 0, 1]}
# Fujifilm's X-Trans pattern, as a DNG file of an X-T camera has it
XTRANS = [1, 1, 0, 1, 1, 2,
          1, 1, 2, 1, 1, 0,
          2, 0, 1, 0, 2, 1,
          1, 1, 2, 1, 1, 0,
          1, 1, 0, 1, 1, 2,
          0, 2, 1, 2, 0, 1]


def raw_chunks(raw, tile):
    """(x0, y0, the samples of the strip or tile) in the order of the file. tile: (width, height),
    or (None, rows per strip)."""
    h, w = raw.shape[:2]
    tw, th = tile
    if tw is None:
        for y in range(0, h, th):
            yield 0, y, raw[y:y + th]
        return
    for y in range(0, h, th):
        for x in range(0, w, tw):
            part = raw[y:y + th, x:x + tw]
            full = np.zeros((th, tw) + raw.shape[2:], raw.dtype)   # tiles at the edges are filled up
            full[:part.shape[0], :part.shape[1]] = part
            yield x, y, full


def write_dng(path, raw, bits=16, order="<", photometric="cfa", pattern="RGGB", pattern_size=(2, 2), plane_color=None,
              compression=1, tile=None, rows_per_strip=None, predictor=1, jpeg=None, active_area=None, linearization=None,
              black=None, white=None, raw_in_ifd0=False, preview=True, exif=True, camera=("Canon", "Canon EOS R5"),
              unique="Canon EOS R5", software="dng_files.py", version=(1, 4, 0, 0), digest=False, extra_raw=None,
              extra_ifd0=None, more_ifds=(), second_raw=False, chunk_bytes=None, edit_raw=None):
    """Writes a DNG file whose raw image is `raw` (lines x width for a colour filter array,
    lines x width x samples for LinearRaw). Returns the bytes written.

    jpeg: options of ljpeg() for compression 7, and "shape": "same" (each tile one JPEG of its
    size), "two" (a JPEG half as wide with two components, as Adobe DNG Converter writes),
    "tall" (twice as wide and half as high), "valid" (a tile at an edge as wide as its part of the
    image), or "imagecodecs" (encoded by imagecodecs instead of ljpeg()).
    chunk_bytes(k, data): what is written for strip or tile k instead of its data (a damaged file).
    edit_raw(entries): changes the entries of the raw image's directory once they are complete."""
    raw = np.asarray(raw)
    t = Tiff(order)
    h, w = raw.shape[:2]
    spp = 1 if raw.ndim == 2 else raw.shape[2]
    if tile is None:
        tile = (None, rows_per_strip or h)
    jpeg = dict(jpeg or {})
    shape = jpeg.pop("shape", "same")
    offsets, counts = [], []
    for x0, y0, chunk in raw_chunks(raw, tile):
        if compression == 1:
            blob = pack_rows(chunk, bits, order)
        elif compression == 8:
            if predictor == 1:
                samples = chunk
            else:
                back = spp * {2: 1, 34892: 2, 34893: 4}[predictor]
                samples = difference(chunk, bits, back)
            blob = zlib.compress(pack_rows(samples, bits, order), 6)
        elif compression == 7:
            c = chunk if chunk.ndim == 3 else chunk[:, :, None]
            if shape == "two":
                c = c.reshape(c.shape[0], c.shape[1] // 2, 2 * c.shape[2])
            elif shape == "tall":
                c = c.reshape(c.shape[0] // 2, c.shape[1] * 2, c.shape[2])
            elif shape == "valid":
                c = c[:min(c.shape[0], h - y0), :min(c.shape[1], w - x0)]
            if shape == "imagecodecs":
                import imagecodecs
                blob = imagecodecs.ljpeg_encode(np.ascontiguousarray(c[:, :, 0] if c.shape[2] == 1 else c).astype(np.uint16),
                                                bitspersample=bits)
            else:
                blob = ljpeg(c, bits, **jpeg)
        else:
            blob = b"\0" * 16
        if chunk_bytes is not None:
            blob = chunk_bytes(len(offsets), blob)
        offsets.append(t.add(blob))
        counts.append(len(blob))

    def rational(v):
        return (int(round(v * 1000000)), 1000000)

    raw_entries = {
        254: (LONG, 0), 256: (LONG, w), 257: (LONG, h), 258: (SHORT, [bits] * spp), 259: (SHORT, compression),
        262: (SHORT, 32803 if photometric == "cfa" else 34892), 277: (SHORT, spp), 284: (SHORT, 1),
    }
    if predictor != 1:
        raw_entries[317] = (SHORT, predictor)
    if tile[0] is None:
        raw_entries.update({273: (LONG, offsets), 278: (LONG, tile[1]), 279: (LONG, counts)})
    else:
        raw_entries.update({322: (LONG, tile[0]), 323: (LONG, tile[1]), 324: (LONG, offsets), 325: (LONG, counts)})
    if photometric == "cfa":
        cells = CFA[pattern] if isinstance(pattern, str) else list(pattern)
        raw_entries.update({33421: (SHORT, [pattern_size[1], pattern_size[0]]), 33422: (BYTE, cells),
                            50710: (BYTE, plane_color or [0, 1, 2]), 50711: (SHORT, 1)})
    if active_area is not None:
        raw_entries[50829] = (LONG, list(active_area))
    if linearization is not None:
        raw_entries[50712] = (SHORT, list(linearization))
    if black is not None:
        values = black if isinstance(black, (list, tuple)) else [black]
        raw_entries[50714] = (RATIONAL, [rational(v) for v in values])
        if len(values) == 4:
            raw_entries[50713] = (SHORT, [2, 2])
    if white is not None:
        raw_entries[50717] = (LONG, white)
    raw_entries.update(extra_raw or {})
    if edit_raw is not None:
        edit_raw(raw_entries)

    ifd0 = {254: (LONG, 1), 271: (ASCII, camera[0]), 272: (ASCII, camera[1]), 305: (ASCII, software),
            50706: (BYTE, list(version)), 50707: (BYTE, [1, 1, 0, 0]), 50708: (ASCII, unique)}
    if digest:
        ifd0[51111] = (BYTE, list(range(16)))
    if exif:
        e = {33434: (RATIONAL, [(1, 250)]), 34855: (SHORT, 800), 36867: (ASCII, "2026:03:14 22:05:09"),
             36881: (ASCII, "+02:00"), 37521: (ASCII, "25"), 37386: (RATIONAL, [(135, 1)])}
        if isinstance(exif, dict):
            e.update(exif)
            e = {k: v for k, v in e.items() if v is not None}
        ifd0[34665] = (LONG, t.add_ifd(e))
    if preview and not raw_in_ifd0:
        thumb = np.full((8, 12, 3), 128, np.uint8)
        at = t.add(thumb.tobytes())
        ifd0.update({256: (LONG, 12), 257: (LONG, 8), 258: (SHORT, [8, 8, 8]), 259: (SHORT, 1), 262: (SHORT, 2),
                     273: (LONG, at), 277: (SHORT, 3), 278: (LONG, 8), 279: (LONG, thumb.size), 284: (SHORT, 1)})
    subs = []
    if raw_in_ifd0:
        ifd0.update(raw_entries)
        ifd0[254] = (LONG, 0)
    else:
        subs.append(t.add_ifd(raw_entries))
    if second_raw:
        subs.append(t.add_ifd(dict(raw_entries)))
    for entries in more_ifds:
        subs.append(t.add_ifd(entries))
    if subs:
        ifd0[330] = (LONG, subs)
    ifd0.update(extra_ifd0 or {})
    first = t.add_ifd(ifd0)
    data = t.finish(first)
    with open(path, "wb") as f:
        f.write(data)
    return data


def bayer_scene(h, w, bits, seed=0):
    """Samples a sensor might have recorded: a smooth gradient, stars and noise, below 2**bits."""
    rng = np.random.default_rng(seed)
    top = (1 << bits) - 1
    yy, xx = np.mgrid[0:h, 0:w]
    base = (0.1 + 0.3 * xx / max(w - 1, 1) + 0.2 * yy / max(h - 1, 1)) * top
    base += rng.normal(0, top * 0.02, (h, w))
    for _ in range(max(1, h * w // 200)):
        y, x = rng.integers(0, h), rng.integers(0, w)
        base[y, x] = top
    base[0, 0], base[-1, -1] = 0, top   # both ends of the range
    return np.clip(np.rint(base), 0, top).astype(np.uint32 if bits > 16 else np.uint16)
