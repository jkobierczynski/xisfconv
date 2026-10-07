# XISF properties in FITS and ASDF files

How xisfconv carries the XISF properties of an image through a FITS or an ASDF file, written down
for programs that want to read them there, or write them: an extension of a FITS file and a key of
an ASDF tree, each made of what its format already has.

PixInsight keeps much of what it knows about an image in XISF properties, outside the FITS
keywords: the processing history, the instrument and the observation, the astrometric solution
with its splines. FITS and ASDF have no place for them. Since version 0.13.0 a conversion from XISF
writes them along, and a conversion to XISF gives them back as they were.

This is a convention of xisfconv. PixInsight does not read it, and no other program is known to.
It has not changed since 0.13.0. A program that knows nothing of it loses nothing: it sees one more
table extension, or one more key in a tree.

- [What is carried](#what-is-carried)
- [In a FITS file](#in-a-fits-file)
- [In an ASDF file](#in-an-asdf-file)
- [The digest of the WCS](#the-digest-of-the-wcs)
- [What xisfconv does with what it finds](#what-xisfconv-does-with-what-it-finds)

## What is carried

Every property of every image and of the file (the `Metadata` element), in its order, with five
things each:

| | |
|---|---|
| id | `Instrument:Telescope:FocalLength` |
| type | The XISF type name: `Float64`, `String`, `TimePoint`, `F64Matrix`, ... |
| value | The text the XISF header has for it, or the bytes of its data block |
| comment | The `comment` attribute; often empty |
| format | The `format` attribute; often empty |

A value is carried in the form XISF has it in, because that is what makes the way back a copy.
A scalar (`Boolean`, the integers, `Float32`, `Float64`, the complex types, `TimePoint`) and a
`String` in the header are text: `3.8`, `true`, `2026-08-12T01:02:13.428Z`. A vector or a matrix is
the bytes of its data block, uncompressed: its elements as little-endian numbers, a matrix row
after row. A `String` that XISF keeps in a data block (PixInsight stores its long spline
serializations that way) is the bytes of that block, and it is said that it was one.

The element of a vector or matrix follows from the type name:

| Type names | Element | Bytes |
|---|---|---|
| `I8Vector`, `I8Matrix` | signed integer | 1 |
| `UI8Vector`, `UI8Matrix`, `ByteVector`, `ByteMatrix`, `ByteArray` | unsigned integer | 1 |
| `I16...`, `UI16...` | integer | 2 |
| `I32...`, `UI32...`, and the short names `IVector`, `UIVector`, `IMatrix`, `UIMatrix` | integer | 4 |
| `I64...`, `UI64...` | integer | 8 |
| `F32...`, and `FVector`, `FMatrix` | IEEE 754 | 4 |
| `F64...`, and `Vector`, `Matrix` | IEEE 754 | 8 |
| `C32...` | complex: real, then imaginary, each a 4-byte float | 8 |
| `C64...` | complex: two 8-byte floats | 16 |

Not carried: the properties of the file that describe that one XISF file and would be untrue of
the next (`XISF:CreationTime`, `XISF:CreatorApplication`, `XISF:CreatorModule`, `XISF:CreatorOS`,
`XISF:BlockAlignmentSize`, `XISF:MaxInlineBlockSize`, `XISF:CompressionCodecs`,
`XISF:CompressionLevel`), a property that is built of other elements (the tables and structures
of the specification), and what is not a property at all: the saved screen stretch, the
resolution, the ICC profile, the thumbnail.

## In a FITS file

A binary table extension, in the standard's own terms, with a row for each property.

- `EXTNAME = 'XISF_PROPERTIES'` holds the properties of an image. It stands directly behind the
  HDU of that image; in a tile-compressed file, behind the table that holds the image. `EXTVER`
  is the number of the image among the HDUs xisfconv wrote, counted from 1. A reader goes by the
  place: the table belongs to the image before it.
- `EXTNAME = 'XISF_METADATA'` holds the properties of the file. It stands at the end, with
  `EXTVER = 1`.
- A table is only there if there is a property to hold.

The columns:

| Column | `TFORM` | Content |
|---|---|---|
| `ID` | `nA` | The id. `n` is the length of the longest one in the table; shorter ones end in NUL characters |
| `TYPE` | `nA` | The XISF type name |
| `BLOCK` | `1L` | `T` if the value is what XISF keeps in a data block: every vector and matrix, and a `String` that was stored as a block. `F` for text of the header |
| `ROWS` | `1K` | The rows of a matrix, the length of a vector, 0 otherwise |
| `COLUMNS` | `1K` | The columns of a matrix, 0 otherwise |
| `VALUE` | `1PB(max)` | The value, as bytes: UTF-8 text, or the data block |
| `COMMENT` | `1PB(max)` | The comment, UTF-8 text |
| `FORMAT` | `1PB(max)` | The format specification, UTF-8 text |

`VALUE`, `COMMENT` and `FORMAT` are variable-length arrays of bytes in the heap of the table
(`1QB`, with 64-bit descriptors, if the heap is larger than 2 GiB). Comment and format are bytes
and not text columns because a FITS text column is ASCII and a comment may be any UTF-8.

One keyword of the header belongs to the convention: `WCSDIGST`, in the `XISF_PROPERTIES` table of
an image that was converted from XISF. See [The digest of the WCS](#the-digest-of-the-wcs).

Reading, with astropy:

```python
from astropy.io import fits
import numpy as np

with fits.open("image.fits") as hdul:
    for row in hdul["XISF_PROPERTIES"].data:
        value = bytes(np.asarray(row["VALUE"], np.uint8))
        if row["TYPE"] == "F64Matrix":
            print(row["ID"], np.frombuffer(value, "<f8").reshape(row["ROWS"], row["COLUMNS"]))
        elif row["TYPE"] == "F64Vector":
            print(row["ID"], np.frombuffer(value, "<f8"))
        elif row["TYPE"] == "String" or not row["BLOCK"]:
            print(row["ID"], row["TYPE"], value.decode())
```

Writing one. xisfconv takes a table that another program made, if it has these columns:

```python
from astropy.io import fits
import numpy as np

rows = [("Observation:Object:Name", "String", False, 0, 0, b"M 31", b""),
        ("My:Gain", "Float64", False, 0, 0, b"1.25", b"electrons per ADU"),
        ("My:Flags", "UI16Vector", True, 3, 0, np.array([1, 2, 3], "<u2").tobytes(), b""),
        ("My:Matrix", "F64Matrix", True, 2, 2, np.array([[1, 2], [3, 4]], "<f8").tobytes(), b"")]

def column(name, form, k):
    if form.startswith("P"):
        values = np.array([np.frombuffer(row[k], np.uint8) for row in rows], dtype=object)
        return fits.Column(name=name, format=form, array=values)
    return fits.Column(name=name, format=form, array=[row[k] for row in rows])

table = fits.BinTableHDU.from_columns(
    [column("ID", "40A", 0), column("TYPE", "12A", 1), column("BLOCK", "L", 2), column("ROWS", "K", 3),
     column("COLUMNS", "K", 4), column("VALUE", "PB()", 5), column("COMMENT", "PB()", 6)],
    name="XISF_PROPERTIES")
image = fits.PrimaryHDU(np.zeros((30, 40), np.float32))
fits.HDUList([image, table]).writeto("with-properties.fits")
```

`xisfconv with-properties.fits` then writes an XISF file whose image has the four properties.

What a table needs to be read:

- The columns are found by their names, in any order. `ID` and `TYPE` are text columns, `VALUE` a
  variable-length array of bytes. These three are required.
- `COMMENT` and `FORMAT` may be missing (empty then), and so may `ROWS` and `COLUMNS` (0 then, so
  they are needed for a vector or a matrix) and `BLOCK`. Without `BLOCK`, a value counts as a data
  block if its type is a vector or matrix type, or if `ROWS` or `COLUMNS` is not 0.
- Text in `ID` and `TYPE` ends at a NUL character, or at the blanks a program padded it with.
- `THEAP` is honoured.
- A column of another name is passed over.

## In an ASDF file

A key of the tree, `xisf`, beside the key `fits` that holds the images as a list of HDUs:

```yaml
xisf:
  images:
  - wcs_digest: "dd5c3b203e9986a5bf522f4f151f5fb8699bc0a2"
    properties:
      "Instrument:Telescope:FocalLength": {type: "Float64", value: 0.922597}
      "Observation:Time:Start": {type: "TimePoint", value: "2026-08-12T01:02:13.428Z"}
      "PCL:AstrometricSolution:LinearTransformationMatrix": {type: "F64Matrix", value: !core/ndarray-1.0.0 {
          source: 3, datatype: float64, byteorder: little, shape: [2, 2]}}
  metadata:
    "Observation:Description": {type: "String", value: "first light", comment: "of the file"}
```

- `images` is a list with an entry for each HDU of `fits`, in the same order; the entry of an image
  without properties is empty (`{}`). `properties` maps the id of a property to what it is.
  `wcs_digest` is what `WCSDIGST` is in FITS, for every image that has properties. It is an empty
  text where there was no digest to take along: for the properties of a FITS table that another
  program wrote without one.
- `metadata` maps the ids of the properties of the file. It is only there if there are some.
- A property is a mapping with `type` and `value`, and `comment` and `format` where they are not
  empty.

A value is a YAML value of its kind, so that a Python program gets a float, a bool, a matrix:

| XISF type | In the tree |
|---|---|
| `Boolean` | `true` or `false` |
| `Int8` to `UInt64` | an integer |
| `Float32`, `Float64` | a float, with the digits the XISF file has (`.nan`, `.inf`, `-.inf` where it says so) |
| `Complex32`, `Complex64` | `!core/complex-1.0.0 1.5+2.0j` |
| `String`, `TimePoint` | a string |
| a vector or a matrix | `!core/ndarray-1.0.0`, a binary block of the element type (`int8` to `uint64`, `float32`, `float64`, `complex64`, `complex128`), little-endian, with the shape `[length]` or `[rows, columns]` |
| a `String` that XISF keeps in a data block | a string, and `block: true` in the mapping |

What does not fit its type goes along all the same, as something the tree can hold:

- A scalar whose text is no value of its type (`Float64` with the text `n/a`), and an integer too
  large for the tree, is a string, and comes back as that text.
- A value of a type xisfconv has no name for, with a data block, is an array of `uint8` with the
  bytes of the block, and `rows` and `columns` or `length` say what the XISF element said of its
  shape.
- A `String` whose bytes are not UTF-8 is an array of `uint8`.

Reading, with the asdf library:

```python
import asdf

with asdf.open("image.asdf") as af:
    properties = af["xisf"]["images"][0]["properties"]
    focal = properties["Instrument:Telescope:FocalLength"]["value"]              # a float
    matrix = properties["PCL:AstrometricSolution:LinearTransformationMatrix"]["value"]   # a 2x2 array
```

Two things follow from the tree being a tree. A number is a number there: its value comes back
the same, its text may be written another way (`1e-05` as `1.0e-05`, `True` as `true`). And the
id is the key, so of two properties with one id (which XISF does not allow) only the first is
kept, and a file that the asdf library wrote again has its properties sorted by id.

For xisfconv to take a key `xisf` for this, it has to hold `images` as a list and `metadata` as a
mapping (either may be missing) and nothing else. A key of that name with anything else in it is
somebody else's, and is read as every other key of a tree is: an array of two or three dimensions
in it counts as an image. Inside, a key xisfconv does not know in the entry of an image or in a
property is passed over.

## The digest of the WCS

An astrometric solution among the properties (`PCL:AstrometricSolution:...`) and the WCS keywords
of the image are two descriptions of one thing, and a FITS file can be changed by a program that
knows only the keywords: a new plate solution, a crop, the rows stored the other way round. The
solution that was carried would then contradict the image it comes back to.

So a digest of what the properties were written with goes along with them: `WCSDIGST` in the
header of the table, `wcs_digest` in the tree. xisfconv computes it when it converts an image with
properties from XISF, with or without a solution among them, and takes it along unchanged from
FITS to FITS and to ASDF and back (a table that another program wrote without a digest stays
without one).
When a file is converted to XISF, the digest is computed again from the file as it is now.

- If it is the same, the properties are the whole truth about the astrometry: a solution among
  them is written as it is, splines included, and nothing is made from the WCS keywords. (An XISF
  image that had WCS keywords and no solution comes back with its keywords and without one, as it
  was.)
- If it is not the same, or if there is none, the solution properties that were carried are left
  out and a solution is made from the WCS keywords, as for any FITS file.

The other properties are restored either way, with one exception in the second case: a property
that the solution made from the keywords sets itself (the reference system and the equinox of the
observation among them) is written as the keywords have it, not as it was carried.

The digest is the SHA-1, as 40 hexadecimal digits in lower case, of a text of lines that each end
in a line feed:

1. `<width>x<height> bottom-up` or `<width>x<height> top-down`: the size of the image in pixels
   and the order its rows are stored in, in this file.
2. A line `NAME=value` for every WCS keyword of the image, the lines sorted as bytes.

What `value` is depends on what the card holds:

- **A number** (an integer or a real number as FITS writes them, with `E` or `D` before an
  exponent, that a double can hold) is written with the fewest of 15, 16 or 17 significant digits
  that give the same double back (`%.15g` and so on), and zero as `0`. That way the digest does
  not depend on how a program writes its cards: `1.0E-5`, `1e-05` and `1D-5` are one value.
- **A text** is its content: without the quotes, a doubled quote as one, without the blanks at
  its end, a long one that goes on in `CONTINUE` cards as one text. A record-valued keyword of
  the distortion paper is a text like any other: `DP1 = 'EXTVER: 1'` gives the line
  `DP1=EXTVER: 1`. (astropy makes a keyword `DP1.EXTVER` with the value 1.0 of that card;
  `Card.rawkeyword` and `Card.rawvalue` have what the card says.)
- **Anything else** is the value field as the card has it, without the blanks around it: `T` or
  `F`, a complex value `(1.0, 2.0)`, nothing at all for a card without a value.

A name counts in upper case, also that of a `HIERARCH` card. A keyword that stands twice gives two
lines. The order of the cards does not count, and neither does any keyword that is not about the
WCS, nor the comment of a card.

That is said for cards that are valid FITS. Of other cards (a value that does not begin in column
11 behind `= `, a `CONTINUE` card that no `&` announced, a complex value with a lower-case `e`)
libraries read different things, and a program that reads with one of them may get another digest
than xisfconv, which takes such a card as its own FITS reader does (`src/fitsread.cpp`).

The WCS keywords are those of the WCS papers, SIP and the distortion conventions, in the primary
description and the alternate ones (a letter at the end), and in their old spellings:

- `WCSAXES`, `CTYPEn`, `CUNITn`, `CRVALn`, `CRPIXn`, `CDELTn`, `CROTAn`, `CDi_j`, `PCi_j`, `PVi_m`,
  `PSi_m`, `LONPOLE`, `LATPOLE`, `RADESYS`, `EQUINOX`
- `RADECSYS`, `EPOCH`, and the matrix written as `PC001002` or `CD001002`
- SIP: `A_ORDER`, `B_ORDER`, `AP_ORDER`, `BP_ORDER`, `A_DMAX`, `B_DMAX`, `A_p_q`, `B_p_q`, `AP_p_q`,
  `BP_p_q`
- `WATn_nnn` (TNX and ZPX), `CPDISn`, `CQDISn`, `CPERRn`, `CQERRn`, `DPn`, `DQn`, and the keywords
  that begin with `D2IM`

[`examples/wcs_digest.py`](../examples/wcs_digest.py) computes it in fifty lines of Python, without
xisfconv; the tests of xisfconv hold the two against each other.

A program that changes the WCS of such a file has nothing to do: the digest no longer fits, and
that says it. A program that writes a solution of its own into the properties writes the digest
that goes with its keywords; without one, the solution it wrote is not used.

There are cases in which xisfconv does not get its own digest back. The digest is taken from the
keywords of the XISF image, and a WCS keyword that cannot be written as a FITS card as it is (a
text with a character outside ASCII or a tab, a number too long for a card or too large for a
double, a complex value with a lower-case `e`) is written changed, or left out with a warning.
In an ASDF tree a value that is no plain number or text may come back written another way
(`(1,2)` as `(1.0, 2.0)`). The solution of such an image is then made from the keywords on the
way back, as if another program had changed them. These are not values a plate solution has: the
plate-solved frames of PixInsight that xisfconv was tried on come back with their solution.

## What xisfconv does with what it finds

- A property without an id is left out, and so is one whose value does not have the size its type
  and its shape ask for. Each with a warning that names it; the other properties and the image are
  not affected.
- A table whose columns are not those above is not used, with a warning. The image is converted
  all the same. A file that ends inside the rows of a table is refused, as every FITS file is
  whose data is cut short. One that ends inside the header of a table is converted without the
  table and without a word, as a file with something behind its last HDU is; `--verify` reports
  it.
- The properties of a file are held in memory together, and may hold the size of the file plus
  256 MiB: what is beyond is left out with a warning. (Rows that all point at the same bytes of
  the heap, or properties that share one block of a tree, would otherwise ask for more memory
  than the file could fill.)
- From FITS to ASDF and back, and from FITS to FITS, the properties go along as they are.
- `--no-properties` (the option `properties` of the library, `properties=False` in Python) leaves
  them out when a file is written, and leaves them where they are when one is read.
- Versions before 0.13 do not know the convention: they name the tables of a FITS file as skipped
  HDUs, and take the matrices in an ASDF tree for images.
