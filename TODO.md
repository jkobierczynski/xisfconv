# To do

Planned features, roughly in order. Done items move to the README.

- [x] TIFF and PNG export from FITS and ASDF input, with `--stretch` and `--bits` (0.7.0)
- [ ] XISF -> XISF rewriting: recompress existing files (zstd/zlib), add checksums, extract one image
      of a multi-image file; a `--verify` mode that checks the checksums of many files without
      converting them
- [ ] Read tile-compressed FITS (`.fits.fz`: RICE_1, GZIP_1, GZIP_2) instead of asking for funpack;
      writing later
- [ ] Lossless property round trip: carry all XISF properties through FITS (HIERARCH keywords) and
      ASDF (tree entries) and restore them on the way back
- [ ] Downsampling (`--resize` / `--bin`) for TIFF and PNG export, and a `.thumbnailer` entry so Linux
      file managers show previews of `.xisf` and `.fits` files
- [ ] More platforms and packaging: Linux arm64 and Intel macOS release builds; Homebrew formula,
      AUR package, winget manifest; man page

Smaller items, each closing a limitation listed in the README:

- [ ] BigTIFF for output over 4 GiB
- [ ] Distributed XISF units (`.xish` + `.xisb`)
- [ ] JPEG output
- [ ] TPV distortion alongside SIP
- [ ] Recursive directory conversion

Open check:

- [ ] Confirm the tag-release job on macOS and Windows publishes working binaries
