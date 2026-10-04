# To do

Planned features, roughly in order. Done items move to the README.

- [x] TIFF and PNG export from FITS and ASDF input, with `--stretch` and `--bits` (0.7.0)
- [x] XISF -> XISF rewriting: recompress or decompress existing files, add or remove checksums,
      extract one image of a multi-image file, `--in-place`; a `--verify` mode that checks XISF, FITS
      and ASDF files (and directories of them) without converting (0.8.0)
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
- [ ] Recursive directory conversion (directories are accepted by `--verify` only)
- [ ] Wildcard expansion on Windows (`*.xisf` is not expanded by cmd or PowerShell)
- [ ] Write CHECKSUM / DATASUM keywords in FITS output

Open check:

- [ ] Confirm the tag-release job on macOS and Windows publishes working binaries
