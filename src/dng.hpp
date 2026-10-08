// DNG reader: the raw image of a Digital Negative file (Adobe's DNG specification, a TIFF file
// with tags of its own), as the camera recorded it.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fitsread.hpp"

namespace xisfconv {

// Reads the raw image of a DNG file: the full-resolution image of the file (NewSubFileType 0)
// whose samples are those of the sensor (a colour filter array) or its linear colour planes,
// cut to the area of the sensor that saw light (ActiveArea) and with the file's linearization
// table applied, and nothing else done to it: not demosaiced, no black level taken off, no
// white balance. Its metadata becomes FITS keywords (INSTRUME, EXPTIME, ISOSPEED, DATE-OBS or
// DATE-LOC, FOCALLEN, BAYERPAT, BLKLEVEL, WHTLEVEL); a colour filter pattern also goes to
// FitsImage::cfa*. The rows are top-down. With headersOnly the samples are not read.
// Throws Error on a malformed file and Unsupported for what is not read (lossy and JPEG XL
// compression, floating point data).
FitsFile readDng(const std::string& path, bool headersOnly = false);

// Reads the raw image as readDng does and reports what it found; the previews are not read.
VerifyReport verifyDng(const std::string& path);

// True for a TIFF file whose first directory says it is a DNG file (DNGVersion).
bool looksLikeDng(const std::string& path);

// Lossless JPEG (ITU-T T.81, the lossless process with Huffman coding, SOF3), as DNG uses it for
// raw data. Decodes one complete JPEG stream into its samples, line after line, the components of
// a sample next to each other. Throws Error on a damaged stream and Unsupported for a JPEG that is
// not lossless or uses what DNG files do not (several scans, subsampled components).
struct LosslessJpeg {
    unsigned width = 0, height = 0, components = 0, precision = 0;
    std::vector<uint16_t> samples;
};
LosslessJpeg decodeLosslessJpeg(const uint8_t* data, size_t size, uint64_t maxSamples);

}  // namespace xisfconv
