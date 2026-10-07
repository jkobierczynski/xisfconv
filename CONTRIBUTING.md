# Contributing

Reports and changes are welcome. This page says what helps; [`DEVELOPMENT.md`](DEVELOPMENT.md) says
how the program is built, tested and changed, and why it is the way it is.

## A file that does not convert, or converts wrongly

That is the most useful report there is. Open an
[issue](https://github.com/jkobierczynski/xisfconv/issues) with:

- the version (the output of `xisfconv --version`) and the platform (Linux, macOS or Windows);
- the command, and everything it printed;
- what the file is: the output of `xisfconv --info <file>`, and of `xisfconv --verify <file>`;
- which program wrote the file (PixInsight, Siril, N.I.N.A., astropy, ...), and its version if you
  know it;
- what you expected instead, and how you saw the difference (which program showed the result).

The file itself settles most questions. If you can share it, say where it can be fetched; a frame
of 100 MB does not go into an issue. If you cannot, `xisfconv --dump-header <file>` prints the
header without the data blocks: the XML of an XISF file, the tree of an ASDF file (for a FITS file
it prints what `--info` prints). That is the description of the image without its pixels, unless
an XISF file keeps them inside the header, which small images may. Look through it for what you do
not want to publish, such as the location of an observatory, before you attach it.

A problem that touches security (a crash on a crafted file, a file read or written that should not
be) is reported privately instead: [`SECURITY.md`](SECURITY.md) says how.

## An idea or a missing feature

Look at [`TODO.md`](TODO.md) and at the limitations in [`MANUAL.md`](MANUAL.md#limitations) first:
it may be planned, or left out on purpose. If it is neither, open an issue and say what you want
to do with it. The scope is XISF, and the conversion between XISF, FITS and ASDF; FITS and ASDF are
supported as far as images need them.

## A change to the code

Open an issue before a change of any size, so that the work is not done twice or in a direction
that will not be taken. Then a pull request, or a patch made with `git format-patch`.

What a change needs to be taken:

- **It builds without warnings** with GCC and clang (`-Wall -Wextra -Wpedantic`), and it builds
  with Microsoft's compiler, which is stricter than both in places. CI builds on Linux, macOS and
  Windows.
- **It is tested against something that shares no code with it.** That is the rule of the project:
  a feature is done when its output has been compared with an independent implementation
  (astropy, CFITSIO's tools, the `xisf` package, Python's `asdf`, tifffile, Pillow, OpenXISF), or
  with a reader written out in the test script. Readers are also tested with damaged and
  truncated files.
- **The suites pass**: see "Running the tests" in `DEVELOPMENT.md`.
- **It is written down**: what it does and what it does not in `MANUAL.md`, a line in
  `CHANGELOG.md`, the decision behind it in `DEVELOPMENT.md` if there was one to make. If it
  changes an example, the header or a docstring, the manual of the library is made again
  (`python docs/make_manual.py`).
- **No code is copied in from elsewhere.** The readers and writers of every format are the
  project's own, which is what lets the tests compare them with the other implementations and
  keeps the licence simple. In particular nothing is taken from the `xisf` package or from
  OpenXISF, which are used as references to test against.

## Licence

The library (`include/`, everything in `src/` but `main.cpp`, `python/xisfconv`, the build files,
the examples and the manual of the library) is under the GNU Lesser General Public License,
version 3 or later; the command line tool (`src/main.cpp`), its manual page and the tests are
under the GNU General Public License, version 3 or later. Each source file names the one that
applies in a line `SPDX-License-Identifier` near its top. A contribution is made under the licence
of the file it changes; a new file names its licence the same way.
