// Distributed XISF units: where a data block is (the location attribute), the files beside the
// header that hold blocks, and the XISF data blocks file (.xisb).
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.hpp"

namespace xisfconv {

// The location attribute of an element with a data block (XISF 1.0, section 10.3):
//   inline:encoding                        the block is the text of the element
//   embedded                               ... of its Data child element
//   attachment:position:size               bytes of the monolithic file the header is in
//   path(@header_dir/rel-file-path)[:id]   a file beside the header (or below its directory)
//   path(abs-file-path)[:id]               a file anywhere on this machine
//   url(URL)[:id]                          a local or a remote file
// With an id the file is an XISF data blocks file and the block is the one its index has under
// that id; without one the block is the whole file.
struct XisfLocation {
    enum class Kind { Inline, Embedded, Attachment, Path, Url };
    Kind kind = Kind::Inline;
    std::string encoding;              // inline: base64 or hex
    uint64_t position = 0, size = 0;   // attachment
    // path: the file path with its escapes taken off, without the @header_dir/ prefix if it had
    // one (headerDir). url: the URL as it is written.
    std::string target;
    bool headerDir = false;
    bool hasId = false;
    uint64_t id = 0;

    bool external() const { return kind == Kind::Path || kind == Kind::Url; }
};

// Throws Error for a value that is none of the forms above. `what` names the element.
XisfLocation parseXisfLocation(const std::string& text, const std::string& what);
// True for a path(...) or url(...) location, by its beginning (it may still be malformed).
bool isExternalXisfLocation(const std::string& text);
// path(@header_dir/name):0x<16 hexadecimal digits>, with the parentheses of the name escaped.
// This is the value of the attribute; in a header it is written as xmlAttributeValue of it.
std::string xisfBlocksFileLocation(const std::string& name, uint64_t id);
// A text as it stands between the quotes of an XML attribute: & < > " as entities, and a tab or a
// line break as a character reference (written as it is, an XML reader would make a blank of it).
std::string xmlAttributeValue(const std::string& text);
// True for a name the data blocks file of a unit can have: one a header can hold (text that is
// valid UTF-8 and XML), without a slash.
bool isXisfBlocksFileName(const std::string& name);

// Which files a header may send the reader to. A header is data that came from somewhere: one
// that names /etc/passwd as the pixels of an image would have a conversion copy that file into
// its output.
enum class ExternalFiles {
    HeaderDirectory,   // files in the directory of the header and below it (symbolic links followed first)
    Anywhere,          // also absolute paths, file: URLs and what links lead to
    None               // no file but the header itself
};

// The policy of the calling thread, for the XISF files it opens while the object lives (as
// MessageScope is for messages). Without one: HeaderDirectory.
class ExternalFilesScope {
public:
    explicit ExternalFilesScope(ExternalFiles policy);
    ~ExternalFilesScope();
    ExternalFilesScope(const ExternalFilesScope&) = delete;
    ExternalFilesScope& operator=(const ExternalFilesScope&) = delete;

private:
    ExternalFiles previous_;
};
ExternalFiles externalFilesPolicy();

// A header asks for a file the policy does not let it have.
struct NotAllowed : Error {
    explicit NotAllowed(const std::string& message) : Error(message, ErrorKind::NotAllowed) {}
};

// One element of the block index of an XISF data blocks file (section 9.4).
struct XisbElement {
    uint64_t id = 0;
    uint64_t position = 0;             // from the beginning of the file; 0: a free element, which points to no block
    uint64_t length = 0;               // as stored
    uint64_t uncompressedLength = 0;   // of a compressed block; 0 for any other
};

struct XisbIndex {
    uint64_t fileSize = 0;
    std::vector<XisbElement> elements;                // in the order of the index, free ones included
    std::unordered_map<uint64_t, size_t> byId;        // the first element of each id
    size_t nodes = 0;
    // What the specification does not allow, and reading survives: `problems` where the index
    // cannot be right (a block beyond the end of the file, two blocks under one identifier),
    // `remarks` where it only holds something a reader has no use for (a reserved field that
    // is not zero).
    std::vector<std::string> problems, remarks;
};

constexpr uint64_t kXisbPreamble = 16;        // the signature and the reserved field
constexpr uint64_t kXisbNodeHeader = 16;      // length, reserved, next node
constexpr uint64_t kXisbElement = 40;

// Reads the block index of the file at `path` (a name for it in messages: `shown`). Throws Error
// for a file that is not a data blocks file, or whose index cannot be followed.
XisbIndex readXisbIndex(const std::string& path, const std::string& shown);

// The name of the root element of an XML document, as far as its beginning tells (the XML
// declaration, comments, processing instructions and a document type declaration are passed
// over); "" if there is no element in what was given, or the text is no XML.
std::string xmlRootNameAtStart(const std::string& beginning);
// How much of a file is looked at to tell that it is an XISF header file.
constexpr size_t kXmlBeginning = 65536;
// True if the file is an XML document whose root element is xisf, as far as its beginning
// tells: the header file of a distributed unit.
bool looksLikeXisfHeaderFile(const std::string& path);

// True for the name of an XISF header file: it ends in .xish, in any case of the letters. (The
// specification ties the kind of a file to its suffix: .xisf monolithic, .xish a header, .xisb
// data blocks.) A unit that is written under such a name is written distributed.
bool isXisfHeaderName(const std::string& path);
// The data blocks file that is written with a header file: of its name, with .xisb for .xish.
std::string xisfBlocksPathFor(const std::string& headerPath);

// A block to be stored in a data blocks file that is written.
struct XisbOutBlock {
    uint64_t id = 0;
    uint64_t position = 0, length = 0, uncompressedLength = 0;
};
// Identifiers for `count` blocks: random, different from each other and not zero. (Random, so
// that a header never finds its blocks in a data blocks file written for another one.)
std::vector<uint64_t> newXisbIds(size_t count);
// The bytes a data blocks file begins with: signature, reserved field, and an index of one node
// with these elements. Its size is xisbIndexSize(count), whatever the elements say.
std::vector<uint8_t> xisbIndexBytes(const std::vector<XisbOutBlock>& blocks);
uint64_t xisbIndexSize(size_t count);

// " (The file ... has that block ...)" if beside the data blocks file at `path` a file is that a
// replacement of the unit set aside (<path>.replaced, .replaced1, ...) and that holds the block
// `id`; "" if there is none, or no identifier to look for.
std::string xisfSetAsideHint(const std::string& path, const uint64_t* id);

// A file of the header's directory that is somewhere else for now: a unit that was written and
// is read back before its files get their names has its data blocks in a temporary file.
struct XisfBlocksRedirect {
    std::string name;   // as the header has it behind @header_dir/
    std::string path;   // where that file is
};

// The files beside the header of one XISF unit, as far as its blocks were looked for.
class XisfExternalFiles {
public:
    // `headerPath`: the file the header is in. The policy is the calling thread's at this moment.
    // What is wrong with the index of a data blocks file and does not keep it from being read is
    // a warning when the index is read; with `problemsAreKept` it is only kept (indexProblems):
    // for a verification, which reports it as what it is.
    explicit XisfExternalFiles(const std::string& headerPath, const XisfBlocksRedirect* redirect = nullptr,
                               bool problemsAreKept = false);
    // The file is not one that is followed to others by its own word: a monolithic file, which
    // holds all of its data by the specification, or a header file that is not named as one
    // (.xish). `why` says which, for the message. Such a file is followed only where a header
    // may lead anywhere.
    void notAHeaderToFollow(const std::string& why) { notFollowed_ = why; }

    struct Block {
        std::vector<uint8_t> bytes;
        bool indexed = false;              // found through the index of a data blocks file
        uint64_t uncompressedLength = 0;   // what that index says
    };
    // Reads the block a path(...) or url(...) location names. Throws NotAllowed for a file the
    // policy keeps the header from, Unsupported for a URL that is not a local file, and Error
    // (I/O, format) for a file that is not there or does not hold the block.
    Block read(const XisfLocation& location, const std::string& what);
    // How many bytes that block is stored with, without reading it. Throws as read does.
    uint64_t storedSize(const XisfLocation& location, const std::string& what);
    // How many bytes the blocks with these identifiers are stored with in the data blocks file at
    // `path` (as resolve gives it), together, and no more than the file has; 0 for a file whose
    // index cannot be read. Does not throw.
    uint64_t indexedBytes(const std::string& path, const std::set<uint64_t>& ids);

    // Where the file of a location is looked for: an absolute path, or the URL if it is not a
    // local file. Does not throw: an answer is given also for a file that may not be read.
    std::string where(const XisfLocation& location) const;
    // The path the system is asked for that file, as the header has it (no ".." taken out of it
    // by its words: behind a link, ".." is not what it looks like); "" for a URL that is no
    // local file. For telling whether another path is this file.
    std::string asked(const XisfLocation& location) const;
    // The same after the checks of read(): the path of a file that is there and may be read.
    // `forMessage`: the error of a file that is not there may point to a file beside it that a
    // replacement set aside (xisfSetAsideHint), which costs a look into that file: for errors
    // somebody reads, not for a header that is only asked which files it names.
    std::string resolve(const XisfLocation& location, const std::string& what, bool forMessage = false) const;

    ExternalFiles policy() const { return policy_; }
    // The problems of the indexes that were read so far, each with the name of its file.
    const std::vector<std::string>& indexProblems() const { return indexProblems_; }

private:
    std::string headerPath_;
    std::filesystem::path headerDir_;
    ExternalFiles policy_;
    XisfBlocksRedirect redirect_;
    bool problemsAreKept_ = false;
    std::string notFollowed_;
    std::vector<std::string> indexProblems_;
    // by resolved path; an index that could not be read is not read again for the next block
    struct ReadIndex {
        std::shared_ptr<XisbIndex> index;
        std::string error;
        ErrorKind kind = ErrorKind::Format;
    };
    std::map<std::string, ReadIndex> indexes_;

    const XisbIndex& index(const std::string& path, const std::string& shown);
    const XisbElement& element(const XisfLocation& location, const std::string& path, const std::string& what);
    std::filesystem::path wanted(const XisfLocation& location, std::string* url) const;
    std::string setAsideHintFor(const std::string& path, const uint64_t* id) const;
    mutable std::map<std::string, std::vector<std::pair<std::string, XisbIndex>>> setAside_;   // see xisfSetAsideHint
};

}  // namespace xisfconv
