// Converting whole files: XISF to FITS, ASDF, TIFF or PNG; FITS and ASDF to XISF, to each other,
// or to TIFF or PNG; and rewriting an XISF file, also in place.
// Nothing here prints: warnings and notes go to the message handler (see common.hpp).
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "fitsread.hpp"
#include "xisf.hpp"
#include "xisfrewrite.hpp"

namespace xisfconv {

enum class Format { Fits, Tiff, Png, Xisf, Asdf };   // what can be written

enum class InputFormat { Xisf, Fits, Asdf };

enum class Stretch { None, Auto, Linked, Unlinked, Stored };

// FITS and ASDF are recognized by their signature; everything else is taken for XISF, whose
// reader says what is wrong with a file that is not.
InputFormat detectInputFormat(const std::string& path);

struct ConvertOptions {
    Stretch stretch = Stretch::None;
    std::optional<SampleFormat> bits;       // output sample format; default: as stored
    std::optional<size_t> imageIndex;       // convert only this image; default: all
    bool compress = false;                  // TIFF: Deflate; XISF and ASDF: `codec`, or the default codec
    std::string codec;                      // XISF and ASDF output: "zlib" or "zstd"
    std::string checksum;                   // XISF output: "sha1", "sha256", "sha512", "sha3-256", "sha3-512"
    uint64_t subblockSize = 1u << 30;       // XISF output
    bool bottomUp = true;                   // from XISF: rows of FITS and ASDF output; from FITS and ASDF with
    bool rowOrderGiven = false;             //   rowOrderGiven: the order the input rows are stored in
    std::optional<std::pair<double, double>> bounds;  // FITS and ASDF input: range of floating point data
    bool propertyKeywords = true;           // from XISF: derive missing keywords from properties
    bool verify = true;                     // verify the checksums of the input
    bool wcs = true;                        // translate the astrometric solution
    int sipOrder = 3;                       // from XISF: order of the SIP fit (0 = linear only)
    bool force = false;                     // overwrite an existing output
    bool properties = true;                 // from XISF to FITS and ASDF: take the XISF properties along; from FITS
                                            //   and ASDF: use the properties a file carries (see carriedProperties)
};

// The output is written to "<output>.part" and renamed when it is complete.
void convertXisfFile(const std::string& input, const std::string& output, Format format, const ConvertOptions& options);
void convertFitsOrAsdfFile(const std::string& input, InputFormat kind, const std::string& output, Format format,
                           const ConvertOptions& options);

// Where a set of images comes from: a FITS or ASDF file, or the caller's memory.
struct ImageSetOrigin {
    std::string format;       // "FITS" or "ASDF"; empty for images handed over in memory
    std::string input;        // the file they were read from (never overwritten); empty if none
    std::string defaultName;  // for an image without a name: the name of the input file without extension
    bool notes = true;        // report how each image was mapped (sample type, row order, bounds)
};

// Writes images that are in memory in the form the FITS and ASDF readers deliver, to any output
// format. This is the second half of convertFitsOrAsdfFile; the images are consumed.
void writeImageSet(FitsFile& images, const ImageSetOrigin& origin, const std::string& output, Format format,
                   const ConvertOptions& options);

// Reverses the row order of an image and of what describes it: WCS keywords and BAYERPAT.
void flipImageRows(FitsImage& image);
// The same for the keywords alone.
void flipKeywordRows(std::vector<FitsKeyword>& keywords, uint64_t height);

// The cards an XISF image gets when it is written to FITS or ASDF with its rows bottom-up or
// top-down: its FITS keywords, keywords derived from properties where it has none, BAYERPAT and
// WCS keywords in the row order asked for, and WCS keywords built from a PixInsight solution.
// `wcsSummary`, if given, receives the line about a fitted solution.
std::vector<FitsKeyword> xisfImageFitsKeywords(XisfFile& file, size_t index, bool bottomUp, bool propertyKeywords,
                                               bool wcs, int sipOrder, std::string* wcsSummary);

// The XISF properties that go with an image, or with the file (XisfFile::kFileProperties), when
// it is converted to FITS or ASDF: all of them with their values, but for the properties of
// the file that describe how that one XISF file was made and stored. A property that cannot be
// read is left out with a warning.
std::vector<Property> carriedProperties(XisfFile& file, size_t index, bool verify);

// The range given to floating point data from FITS and ASDF when nothing else is said: 0:1 if
// the data fits, else 0:65535 if it fits, else its minimum and maximum.
std::pair<double, double> automaticBounds(const FitsImage& image);

struct XisfFileRewrite {
    XisfRewriteResult result;
    uint64_t inputSize = 0;
    std::string output;       // the file that was written (in place: the file a link leads to)
    bool unchanged = false;   // in place: the file already stored everything as requested and was left alone
};

// XISF -> XISF: the same file with its data blocks stored another way. In place (`output` is
// not used then) the new file is written next to the input, read back and compared, flushed to
// the disk and only then renamed over it.
XisfFileRewrite rewriteXisfFile(const std::string& input, const std::string& output, bool inPlace, bool force,
                                XisfRewriteOptions options);

}  // namespace xisfconv
