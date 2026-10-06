// XISF properties with their values in memory, as they are carried from one format to another.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"

namespace xisfconv {

// One property: what a <Property> element of an XISF file says, with the content of its data
// block if it has one. Nothing is interpreted more than a conversion needs: a property of a
// type this library does not know is carried as it is.
struct Property {
    std::string id;
    std::string type;       // the XISF type name: Boolean, Int32, Float64, String, TimePoint, F64Vector, UI16Matrix, ...
    std::string comment;    // the comment attribute, if any
    std::string format;     // the format attribute, if any
    bool array = false;     // the value is in `data`: a vector or a matrix (or an unknown type stored in a data block)
    std::string text;       // otherwise: the value as XISF writes it (scalars, time points), or the text of a String (UTF-8)
    std::vector<uint8_t> data;        // the elements, little-endian (XISF's own order), row after row
    uint64_t rows = 0, columns = 0;   // a matrix: its shape. A vector: rows = its length, columns = 0.
    // A String that the XISF file keeps in a data block, not as text in its header: `text` is
    // that block, byte for byte, and an XISF file that is written keeps it in a block again.
    // (What a reader makes of line ends and of blanks in the header is its own matter; a block
    // is what it is.)
    bool block = false;
    // Written into the header of an XISF file whatever its size: the properties this library
    // makes itself (the astrometric solution from WCS keywords), which PixInsight was seen to
    // accept in that form. Not carried anywhere.
    bool inHeader = false;
};

// The most data the properties of one file may hold together when they are loaded: what a file
// of that size can hold uncompressed, and 256 MiB on top for what is compressed. (PixInsight's
// splines come to some megabytes.) A file that declares more is damaged, or made to use up the
// memory: many properties that all point at the same compressed block. What is beyond is left out.
uint64_t propertyBudget(uint64_t fileSize);

// The elements of a vector or matrix type.
struct PropertyElement {
    size_t size = 0;        // bytes of one element (a complex number: both parts)
    char kind = 0;          // 'i' signed integer, 'u' unsigned integer, 'f' floating point, 'c' complex
    bool matrix = false;
};
// False for the scalar types and for types this library does not know.
bool propertyElement(const std::string& type, PropertyElement& element);

// True if the type name ends in "Matrix" (also for element types that are not known).
bool isMatrixPropertyType(const std::string& type);

// Checks that the data of an array property has the size its shape asks for. Returns what is
// wrong, or an empty string. (Nothing can be said about an unknown type.)
std::string propertyProblem(const Property& property);

// The file-level properties that describe how one particular XISF file was made and is stored:
// XISF:CreationTime, XISF:CreatorApplication, XISF:CreatorModule, XISF:CreatorOS,
// XISF:BlockAlignmentSize, XISF:MaxInlineBlockSize, XISF:CompressionCodecs, XISF:CompressionLevel.
// A file that is written sets its own.
bool isFileStorageProperty(const std::string& id);

// True for the properties of PixInsight's astrometric solution (PCL:AstrometricSolution:...).
bool isSolutionProperty(const std::string& id);

// True if the text is UTF-8: no overlong forms, no surrogates, nothing beyond Unicode.
bool isValidUtf8(const std::string& text);
// True if the text can be the value of an XML attribute and come back as it is: UTF-8 without
// control characters other than tab and line breaks (which are written as character
// references), and without the two code points XML excludes.
bool isXmlText(const std::string& text);

const Property* findProperty(const std::vector<Property>& properties, const std::string& id);

// A scalar, a vector and a matrix of 64-bit floating point numbers.
Property scalarProperty(const std::string& id, const char* type, const std::string& value);
Property vectorProperty(const std::string& id, const std::vector<double>& values);
Property matrixProperty(const std::string& id, size_t rows, size_t columns, const std::vector<double>& values);

// The elements of a numeric vector or matrix as doubles, row after row. False for complex
// elements, unknown types and data of the wrong size.
bool propertyNumbers(const Property& property, std::vector<double>& out);

// What tells whether the WCS keywords of an image are still those a carried astrometric
// solution was written with: a digest of the keywords that describe the WCS (values as numbers,
// not as text, so that another program may write 1.0E-5 as 1e-05), the size of the image and
// the order of its rows.
std::string wcsDigest(const std::vector<FitsKeyword>& keywords, uint64_t width, uint64_t height, bool bottomUp);

}  // namespace xisfconv
