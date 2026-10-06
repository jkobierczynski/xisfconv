// Reader for monolithic XISF 1.0 files.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common.hpp"
#include "property.hpp"
#include "xml.hpp"

namespace xisfconv {

struct XisfProperty {
    std::string id;
    std::string type;
    std::string value;     // scalar value, or decoded text for String properties
    std::string comment;
    std::string format;
    bool hasBlockData = false;  // vector/matrix (or unread) data stored in a data block
    // A String in the header whose text has a carriage return that is meant (written as a
    // character reference), or one in a value attribute that would not come back from an
    // element as it is: see Property::block.
    bool exactText = false;
    std::string location;
    const xml::Node* node = nullptr;
};

struct ColorFilterArray {
    bool present = false;
    std::string pattern;
    int width = 0;
    int height = 0;
    std::string name;
};

struct Resolution {
    bool present = false;
    double horizontal = 72;
    double vertical = 72;
    std::string unit = "inch";  // "inch" or "cm"
};

// PixInsight screen transfer function (XISF DisplayFunction element). Index 0 = red/gray,
// 1 = green, 2 = blue, 3 = luminance.
struct DisplayFunction {
    bool present = false;
    double m[4] = {0.5, 0.5, 0.5, 0.5};  // midtones balance
    double s[4] = {0, 0, 0, 0};          // shadows clipping point
    double h[4] = {1, 1, 1, 1};          // highlights clipping point
    double l[4] = {0, 0, 0, 0};          // shadows dynamic range expansion
    double r[4] = {1, 1, 1, 1};          // highlights dynamic range expansion
    std::string name;
    bool isIdentity() const;
};

struct XisfImage {
    std::string id;
    uint64_t width = 0, height = 0, channels = 1;
    SampleFormat format = SampleFormat::UInt16;
    std::string sampleFormatText;
    double lowerBound = 0, upperBound = 1;
    bool boundsDeclared = false;
    std::string colorSpace = "Gray";
    bool planar = true;
    bool bigEndian = false;
    std::string imageType, orientation;
    std::string location, compression, subblocks, checksum;  // raw attribute text, for display
    std::string unsupported;  // non-empty if pixel data can't be converted (reason)

    std::vector<FitsKeyword> keywords;
    std::vector<XisfProperty> properties;
    ColorFilterArray cfa;
    Resolution resolution;
    DisplayFunction displayFunction;
    bool hasIccProfile = false;

    const xml::Node* node = nullptr;
    const xml::Node* iccNode = nullptr;
};

// The compression attribute of a data block: codec[+sh]:uncompressedSize[:itemSize].
struct XisfCompression {
    std::string codec;  // zlib, lz4, lz4hc, zstd
    bool shuffled = false;
    uint64_t uncompressedSize = 0;
    uint64_t itemSize = 1;
};
XisfCompression parseXisfCompression(const std::string& text);

// Hex digest for a checksum algorithm name (sha1 / sha-1, sha256 / sha-256, sha512 / sha-512,
// sha3-256, sha3-512). Returns false for names that are not known.
bool xisfDigest(const std::string& algorithm, const uint8_t* data, size_t size, std::string& hex);

// Warns when a checksum algorithm is about to be written that PixInsight itself cannot read:
// SHA3-256 and SHA3-512 are named by the XISF 1.0 specification, but PixInsight (1.9.3) knows
// only SHA-1, SHA-256 and SHA-512 and does not open an image whose block carries another one.
void warnIfChecksumUnknownToPixInsight(const std::string& algorithm);

// A data block as it is stored in the file, with the attributes that describe it.
struct XisfStoredBlock {
    std::vector<uint8_t> bytes;                     // attachment bytes, or the decoded inline/embedded text
    std::string compression, subblocks, checksum;   // attribute text (empty if absent)
    bool attachment = false;
    uint64_t position = 0;                          // attachments: offset in the file
};

enum class XisfChecksumState { None, Verified, Unsupported };

class XisfFile {
public:
    explicit XisfFile(const std::string& path);

    const std::string& path() const { return path_; }
    uint64_t fileSize() const { return fileSize_; }
    const std::string& headerXml() const { return headerXml_; }
    const std::string& version() const { return version_; }
    const std::vector<XisfImage>& images() const { return images_; }
    const std::vector<XisfProperty>& fileProperties() const { return fileProperties_; }

    // Decodes the pixel data of image `index` into host byte order, planar layout.
    PixelBuffer readPixels(size_t index, bool verifyChecksum);

    // Reads an image's embedded ICC profile (empty if none).
    std::vector<uint8_t> readIccProfile(size_t index, bool verifyChecksum);

    // Low-level access to data blocks, for rewriting and verification. `element` is any header
    // element with a location attribute; `what` names it in error messages.
    const xml::Node& root() const { return *root_; }
    XisfStoredBlock readStoredBlock(const xml::Node& element, const std::string& what);
    // Throws on a mismatch; Unsupported for algorithms that are not implemented.
    static XisfChecksumState verifyBlockChecksum(const XisfStoredBlock& block, const std::string& what);
    // Decompresses and unshuffles the stored bytes (a copy of them if the block is not compressed).
    // expectedSize 0 = unknown.
    static std::vector<uint8_t> decodeBlock(const XisfStoredBlock& block, const std::string& what, uint64_t expectedSize = 0);
    std::vector<uint8_t> readBlock(const xml::Node& element, bool verifyChecksum, const std::string& what,
                                   uint64_t expectedSize = 0);  // 0 = unknown

    // Looks up a property by id: image properties first, then file-level metadata.
    const XisfProperty* findProperty(size_t imageIndex, const std::string& id) const;

    // A property with its value: the content of its data block read, decompressed and put in
    // little-endian order. Throws if the block cannot be read or does not fit the shape.
    Property loadProperty(const XisfProperty& property, bool verifyChecksum);
    // The same for a caller that reads the properties one by one, in any order and as often as
    // it likes: what they declare together stays within propertyBudget, each counted once. One
    // that would go beyond it is not read (the error says so).
    Property loadPropertyCounted(const XisfProperty& property, bool verifyChecksum);
    // How a property is stored, without reading it: a value in the header, a String that is
    // kept as a data block (Property::block), a vector or a matrix, or something that is not
    // read (a table, a block of a type without a name here, a text whose block is damaged).
    enum class PropertyStorage { Header, TextBlock, Array, Unread };
    PropertyStorage propertyStorage(const XisfProperty& property) const;
    // All properties of an image, or (kFileProperties) of the file. One that cannot be read is
    // left out with a warning.
    static constexpr size_t kFileProperties = static_cast<size_t>(-1);
    std::vector<Property> loadProperties(size_t imageIndex, bool verifyChecksum);

    // Reads a numeric vector or matrix property (I8..UI64, F32, F64 Vector/Matrix) as doubles,
    // row-major. Returns false if the property is missing or not numeric.
    bool readNumericProperty(size_t imageIndex, const std::string& id, std::vector<double>& out,
                             size_t* rows = nullptr, size_t* columns = nullptr);

private:
    std::string path_;
    std::ifstream file_;
    uint64_t fileSize_ = 0;
    std::string headerXml_;
    std::string version_;
    std::unique_ptr<xml::Node> root_;
    std::vector<XisfImage> images_;
    std::vector<XisfProperty> fileProperties_;
    uint64_t propertyBytes_ = 0;   // what loadProperties has loaded so far (see propertyBudget)
    uint64_t countedBytes_ = 0;    // the same for loadPropertyCounted,
    std::set<const XisfProperty*> counted_;   // and the properties that are in that sum
    uint64_t stringBytes_ = 0;     // what the String properties in data blocks hold that were read when the file was opened
    uint64_t declaredBlockSize(const xml::Node& element) const;

    void parseImage(const xml::Node& node);
    XisfProperty parseProperty(const xml::Node& node);
    std::vector<uint8_t> readAttachment(uint64_t position, uint64_t size);
};

// True for the types of vector and matrix properties whose values XisfFile::readNumericProperty
// reads as numbers (F64Vector, I32Matrix, ...); false for the others (complex elements, for one).
bool isNumericPropertyType(const std::string& type);

}  // namespace xisfconv
