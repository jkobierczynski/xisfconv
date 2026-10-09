// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "xisfblocks.hpp"

#include "property.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <set>

namespace xisfconv {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxIndexNodes = 65536;
constexpr size_t kMaxIndexElements = size_t(1) << 22;   // four million blocks in one file

thread_local ExternalFiles tlsExternalFiles = ExternalFiles::HeaderDirectory;

// Where a path leads once every link on the way is followed, and whether a name is a link: what
// the rule about the directory of a header rests on. On Windows the system is asked itself: not
// every C++ library knows the symbolic links and junctions of that system (MinGW's takes them
// for what they lead to, and a link out of the directory would pass for a file in it).
#ifdef _WIN32
fs::path realPath(const fs::path& path, std::error_code& ec) {
    ec.clear();
    const HANDLE file = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return {};
    }
    std::wstring name(1024, L'\0');
    DWORD length = GetFinalPathNameByHandleW(file, &name[0], static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= name.size()) {
        name.assign(static_cast<size_t>(length) + 1, L'\0');
        length = GetFinalPathNameByHandleW(file, &name[0], static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(file);
    // (a volume that has no drive letter to give, as some RAM disks: what the C++ library says)
    if (length == 0 || length >= name.size()) return fs::canonical(path, ec);
    name.resize(length);
    // \\?\C:\data\frame.xisb and \\?\UNC\server\share\frame.xisb, as they are usually written
    if (name.compare(0, 8, L"\\\\?\\UNC\\") == 0) name = L"\\\\" + name.substr(8);
    else if (name.compare(0, 4, L"\\\\?\\") == 0) name.erase(0, 4);
    return fs::path(name);
}

bool isLink(const fs::path& path) {
    WIN32_FIND_DATAW found;
    if (path.native().find_first_of(L"*?") != std::wstring::npos) return false;   // (no file has such a name; a search would take them for a pattern)
    const HANDLE search = FindFirstFileW(path.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    FindClose(search);
    return (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           (found.dwReserved0 == IO_REPARSE_TAG_SYMLINK || found.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
}
#else
fs::path realPath(const fs::path& path, std::error_code& ec) { return fs::canonical(path, ec); }

bool isLink(const fs::path& path) {
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(path, ec));
}
#endif

uint64_t le64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

uint32_t le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

void putLe64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// The identifier of a block index element as text: an unsigned integer, decimal or (as the
// specification recommends) hexadecimal with 0x in front.
bool parseIndexId(const std::string& text, uint64_t& out) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        if (text.size() > 18) return false;
        uint64_t v = 0;
        for (size_t i = 2; i < text.size(); ++i) {
            const char c = text[i];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return false;
            v = v << 4 | static_cast<uint64_t>(digit);
        }
        out = v;
        return true;
    }
    if (text.empty()) return false;
    for (char c : text)
        if (c < '0' || c > '9') return false;
    return parseUInt64(text, out);
}

bool isAbsoluteXisfPath(const std::string& p) {
    if (!p.empty() && p[0] == '/') return true;
    // (a drive of Windows, with the slashes the specification asks for: C:/data/frame.xisb)
    return p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && p[2] == '/';
}

// %41 and the like of a URL as the bytes they stand for.
std::string percentDecoded(const std::string& s) {
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// The local path a file: URL names; false for any other URL (and for a file on another host).
bool localPathOfUrl(const std::string& url, std::string& path) {
    const size_t colon = url.find(':');
    if (colon == std::string::npos || toLower(url.substr(0, colon)) != "file") return false;
    std::string rest = url.substr(colon + 1);
    if (startsWith(rest, "//")) {
        const size_t slash = rest.find('/', 2);
        if (slash == std::string::npos) return false;
        const std::string host = toLower(rest.substr(2, slash - 2));
        if (!host.empty() && host != "localhost") return false;
        rest = rest.substr(slash);
    }
    if (rest.empty() || rest[0] != '/') return false;
    rest = rest.substr(0, rest.find_first_of("?#"));
    path = percentDecoded(rest);
    if (path.find('\0') != std::string::npos) return false;
    // file:///C:/data/frame.xisb
    if (path.size() >= 4 && path[2] == ':' && path[3] == '/' && isAbsoluteXisfPath(path.substr(1))) path.erase(0, 1);
    return true;
}

std::string hexId(uint64_t id) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%016llx", static_cast<unsigned long long>(id));
    return text;
}

}  // namespace

// The files beside a data blocks file that have the names a replacement of the unit sets the old
// data blocks file aside with (pipeline.cpp, replaceUnit), as far as they are data blocks files:
// each with its index. A run that was stopped between its steps leaves one.
std::vector<std::pair<std::string, XisbIndex>> xisfSetAsideFiles(const std::string& path) {
    std::vector<std::pair<std::string, XisbIndex>> files;
    for (int n = 0; n < 100; ++n) {
        const std::string aside = path + ".replaced" + (n ? std::to_string(n) : std::string());
        std::error_code ec;
        if (!fs::is_regular_file(fs::symlink_status(toPath(aside), ec))) continue;
        try {
            files.emplace_back(aside, readXisbIndex(aside, aside));
        } catch (const Error&) {
        }
    }
    return files;
}

// What to say about a data blocks file that is not there, or does not have a block, if one of
// those files has it. Said only of a file that does hold the block: a name alone proves nothing.
std::string setAsideHint(const std::vector<std::pair<std::string, XisbIndex>>& files, const std::string& path, uint64_t id) {
    for (const auto& file : files) {
        const auto found = file.second.byId.find(id);
        if (found == file.second.byId.end() || file.second.elements[found->second].position == 0) continue;
        return " (The file " + file.first + " has that block: it may be what a replacement of this unit that was "
               "interrupted set aside. If so, renamed to " + path + " it is the data blocks file as it was before.)";
    }
    return {};
}

std::string xisfSetAsideHint(const std::string& path, const uint64_t* id) {
    return id ? setAsideHint(xisfSetAsideFiles(path), path, *id) : std::string();
}

std::string XisfExternalFiles::setAsideHintFor(const std::string& path, const uint64_t* id) const {
    if (!id) return {};
    auto found = setAside_.find(path);   // (looked at once: a unit may name the file for a thousand blocks)
    if (found == setAside_.end()) found = setAside_.emplace(path, xisfSetAsideFiles(path)).first;
    return setAsideHint(found->second, path, *id);
}

ExternalFilesScope::ExternalFilesScope(ExternalFiles policy) : previous_(tlsExternalFiles) { tlsExternalFiles = policy; }
ExternalFilesScope::~ExternalFilesScope() { tlsExternalFiles = previous_; }
ExternalFiles externalFilesPolicy() { return tlsExternalFiles; }

bool isExternalXisfLocation(const std::string& text) { return startsWith(text, "url(") || startsWith(text, "path("); }

XisfLocation parseXisfLocation(const std::string& text, const std::string& what) {
    XisfLocation loc;
    const auto malformed = [&]() -> Error { return Error("malformed location '" + text + "' in " + what); };
    if (startsWith(text, "attachment:")) {
        const auto parts = split(text, ':');
        if (parts.size() != 3 || !parseUInt64(parts[1], loc.position) || !parseUInt64(parts[2], loc.size)) throw malformed();
        loc.kind = XisfLocation::Kind::Attachment;
        return loc;
    }
    if (startsWith(text, "inline:")) {
        loc.kind = XisfLocation::Kind::Inline;
        loc.encoding = text.substr(7);
        return loc;
    }
    if (text == "embedded") {
        loc.kind = XisfLocation::Kind::Embedded;
        return loc;
    }
    if (!isExternalXisfLocation(text)) throw Error("unsupported location '" + text + "' in " + what);

    const bool path = text[0] == 'p';
    const size_t open = path ? 4 : 3;
    // The closing parenthesis is the last one: what stands behind it is the identifier, or nothing.
    const size_t close = text.rfind(')');
    if (close == std::string::npos || close <= open) throw malformed();
    const std::string rest = text.substr(close + 1);
    if (!rest.empty()) {
        if (rest[0] != ':' || !parseIndexId(rest.substr(1), loc.id)) throw malformed();
        loc.hasId = true;
    }
    std::string body = text.substr(open + 1, close - open - 1);
    if (!path) {
        if (trim(body).empty()) throw malformed();
        loc.kind = XisfLocation::Kind::Url;
        loc.target = body;
        return loc;
    }
    // In a path a parenthesis is written with a backslash before it. (One that ends the text of
    // the path with a backslash would be such a parenthesis, and the location has no end.)
    if (!body.empty() && body.back() == '\\') throw malformed();
    std::string plain;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size() && (body[i + 1] == '(' || body[i + 1] == ')')) continue;
        plain += body[i];
    }
    loc.kind = XisfLocation::Kind::Path;
    static const std::string kHeaderDir = "@header_dir/";
    if (startsWith(plain, kHeaderDir)) {
        loc.headerDir = true;
        loc.target = plain.substr(kHeaderDir.size());
        while (!loc.target.empty() && loc.target[0] == '/') loc.target.erase(0, 1);   // (@header_dir//name)
        if (loc.target.empty() || isAbsoluteXisfPath(loc.target)) throw malformed();
    } else {
        if (!isAbsoluteXisfPath(plain) && !startsWith(plain, "//")) {
            throw Error("location '" + text + "' in " + what + " is neither an absolute path nor one that begins with "
                        "@header_dir/, the directory of the header");
        }
        loc.target = plain;
    }
    return loc;
}

std::string xisfBlocksFileLocation(const std::string& name, uint64_t id) {
    std::string escaped;
    for (char c : name) {
        if (c == '(' || c == ')') escaped += '\\';
        escaped += c;
    }
    return "path(@header_dir/" + escaped + "):" + hexId(id);
}

std::string xmlAttributeValue(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\t': out += "&#9;"; break;
            case '\n': out += "&#10;"; break;
            case '\r': out += "&#13;"; break;
            default: out += c;
        }
    }
    return out;
}

bool isXisfBlocksFileName(const std::string& name) {
    return !name.empty() && isXmlText(name) && name.find('/') == std::string::npos && name.find('\0') == std::string::npos;
}

// ------------------------------------------------------------------------------------------
// The data blocks file
// ------------------------------------------------------------------------------------------

XisbIndex readXisbIndex(const std::string& path, const std::string& shown) {
    std::ifstream file(toPath(path), std::ios::binary);
    if (!file) throw Error("cannot open " + shown, ErrorKind::Io);
    file.seekg(0, std::ios::end);
    XisbIndex index;
    index.fileSize = static_cast<uint64_t>(file.tellg());
    file.seekg(0);
    unsigned char head[16];
    if (index.fileSize < kXisbPreamble || !file.read(reinterpret_cast<char*>(head), 16)) {
        throw Error(shown + " is too short to be an XISF data blocks file");
    }
    if (std::memcmp(head, "XISB0100", 8) != 0) {
        throw Error(shown + " is not an XISF data blocks file (it does not begin with XISB0100), but a block is asked for "
                    "by its identifier there");
    }
    if (le64(head + 8) != 0) index.remarks.push_back("the reserved field of the file is not zero");

    // The nodes that were read, by where they begin and end. No two share a byte: a node that
    // lies in another would have the same elements counted again and again, and an index of a
    // megabyte would name more blocks than a gigabyte of memory holds.
    std::map<uint64_t, uint64_t> nodes;
    uint64_t at = kXisbPreamble;
    std::vector<unsigned char> buffer;
    for (;;) {
        if (index.nodes >= kMaxIndexNodes) {
            throw Error("the block index of " + shown + " has more than " + std::to_string(kMaxIndexNodes) + " nodes");
        }
        if (at < kXisbPreamble || at > index.fileSize || index.fileSize - at < kXisbNodeHeader) {
            throw Error("the block index of " + shown + " leads to a node at " + std::to_string(at) +
                        ", beyond the end of the file (" + std::to_string(index.fileSize) + " bytes)");
        }
        if (nodes.count(at)) throw Error("the block index of " + shown + " runs in a circle");
        file.clear();
        file.seekg(static_cast<std::streamoff>(at));
        if (!file.read(reinterpret_cast<char*>(head), 16)) throw Error("read error in the block index of " + shown, ErrorKind::Io);
        const uint64_t length = le32(head);
        const uint64_t next = le64(head + 8);
        if (le32(head + 4) != 0 && index.remarks.size() < 8) {
            index.remarks.push_back("the reserved field of the index node at " + std::to_string(at) + " is not zero");
        }
        if (length > (index.fileSize - at - kXisbNodeHeader) / kXisbElement) {
            throw Error("the block index of " + shown + " has a node of " + std::to_string(length) +
                        " elements at " + std::to_string(at) + ", more than the file has room for");
        }
        if (length > kMaxIndexElements - index.elements.size()) {
            throw Error("the block index of " + shown + " has more than " + std::to_string(kMaxIndexElements) + " elements");
        }
        {
            const uint64_t end = at + kXisbNodeHeader + length * kXisbElement;
            const auto behind = nodes.lower_bound(at);          // the first node that begins at or behind this one
            const bool intoNext = behind != nodes.end() && behind->first < end;
            const bool inPrevious = behind != nodes.begin() && std::prev(behind)->second > at;
            if (intoNext || inPrevious) {
                throw Error("the block index of " + shown + " has a node at " + std::to_string(at) + " that lies in another of its nodes");
            }
            nodes.emplace(at, end);
        }
        ++index.nodes;
        // (some thousand elements at a time: a node of many needs no large buffer)
        for (uint64_t done = 0; done < length;) {
            const uint64_t count = std::min<uint64_t>(length - done, 4096);
            buffer.resize(static_cast<size_t>(count * kXisbElement));
            if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()))) {
                throw Error("read error in the block index of " + shown, ErrorKind::Io);
            }
            for (uint64_t k = 0; k < count; ++k) {
                const unsigned char* p = buffer.data() + k * kXisbElement;
                XisbElement e;
                e.id = le64(p);
                e.position = le64(p + 8);
                e.length = le64(p + 16);
                e.uncompressedLength = le64(p + 24);
                const auto problem = [&](const std::string& text) {
                    if (index.problems.size() < 16) index.problems.push_back("block " + hexId(e.id) + " of the index " + text);
                };
                if (le64(p + 32) != 0 && index.remarks.size() < 8) {
                    index.remarks.push_back("element " + hexId(e.id) + " of the index has a reserved field that is not zero");
                }
                if (e.position == 0) {
                    // A free element: it points to no block, and its identifier is nobody's. (It is
                    // found by that identifier all the same, so that a header that names it is
                    // told what it names; a block of the same identifier takes its place.)
                    if (e.length != 0 || e.uncompressedLength != 0) problem("is a free element with a length");
                    index.byId.emplace(e.id, index.elements.size());
                } else {
                    if (e.position < kXisbPreamble || e.position > index.fileSize || e.length > index.fileSize - e.position) {
                        problem("lies beyond the end of the file");
                    }
                    const auto placed = index.byId.emplace(e.id, index.elements.size());
                    if (!placed.second) {
                        if (index.elements[placed.first->second].position == 0) placed.first->second = index.elements.size();
                        else problem("has the identifier of an earlier one");
                    }
                }
                index.elements.push_back(e);
            }
            done += count;
        }
        if (next == 0) break;
        at = next;
    }
    return index;
}

std::string xmlRootNameAtStart(const std::string& head) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t at = startsWith(head, "\xEF\xBB\xBF") ? 3 : 0;
    for (;;) {
        while (at < head.size() && blank(head[at])) ++at;
        if (at >= head.size() || head[at] != '<') return {};
        if (head.compare(at, 2, "<?") == 0) {            // the XML declaration, a processing instruction
            const size_t end = head.find("?>", at + 2);
            if (end == std::string::npos) return {};
            at = end + 2;
        } else if (head.compare(at, 4, "<!--") == 0) {   // a comment
            const size_t end = head.find("-->", at + 4);
            if (end == std::string::npos) return {};
            at = end + 3;
        } else if (head.compare(at, 2, "<!") == 0) {     // a document type declaration, with what it holds in [ ]
            int depth = 0;
            char quote = 0;
            size_t end = at + 2;
            for (; end < head.size(); ++end) {
                const char c = head[end];
                if (quote) {
                    if (c == quote) quote = 0;
                } else if (head.compare(end, 4, "<!--") == 0) {   // (a comment in it, with whatever quotes)
                    end = head.find("-->", end + 4);
                    if (end == std::string::npos) return {};
                    end += 2;
                } else if (head.compare(end, 2, "<?") == 0) {     // (and a processing instruction)
                    end = head.find("?>", end + 2);
                    if (end == std::string::npos) return {};
                    end += 1;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '[') {
                    ++depth;
                } else if (c == ']') {
                    --depth;
                } else if (c == '>' && depth <= 0) {
                    break;
                }
            }
            if (end >= head.size()) return {};
            at = end + 1;
        } else {
            size_t end = at + 1;
            while (end < head.size() && !blank(head[end]) && head[end] != '>' && head[end] != '/') ++end;
            if (end >= head.size()) return {};           // (cut off in the name)
            // (the name without a namespace prefix: <x:xisf xmlns:x="..."> is the element xisf)
            const std::string name = head.substr(at + 1, end - at - 1);
            const size_t colon = name.find(':');   // (as the parser of the header has it)
            return colon == std::string::npos ? name : name.substr(colon + 1);
        }
    }
}

bool looksLikeXisfHeaderFile(const std::string& path) {
    std::ifstream file(toPath(path), std::ios::binary);
    if (!file) return false;
    std::string head(kXmlBeginning, '\0');
    file.read(&head[0], static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(file.gcount()));
    return xmlRootNameAtStart(head) == "xisf";
}

bool isXisfHeaderName(const std::string& path) { return toLower(fromPath(toPath(path).extension())) == ".xish"; }

std::string xisfBlocksPathFor(const std::string& headerPath) {
    fs::path blocks = toPath(headerPath);
    // (FRAME.XISH gets FRAME.XISB: the case of the name is its owner's)
    const std::string suffix = fromPath(blocks.extension());
    blocks.replace_extension(suffix.size() == 5 && suffix[4] == 'H' ? ".XISB" : ".xisb");
    return fromPath(blocks);
}

std::vector<uint64_t> newXisbIds(size_t count) {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
    std::mt19937_64 generator(seed);
    std::set<uint64_t> used;
    std::vector<uint64_t> ids;
    ids.reserve(count);
    while (ids.size() < count) {
        const uint64_t id = generator();
        if (id != 0 && used.insert(id).second) ids.push_back(id);
    }
    return ids;
}

uint64_t xisbIndexSize(size_t count) { return kXisbPreamble + kXisbNodeHeader + kXisbElement * static_cast<uint64_t>(count); }

std::vector<uint8_t> xisbIndexBytes(const std::vector<XisbOutBlock>& blocks) {
    if (blocks.size() > 0xFFFFFFFFull) throw Error("too many data blocks for one index node");
    std::vector<uint8_t> out(static_cast<size_t>(xisbIndexSize(blocks.size())), 0);
    std::memcpy(out.data(), "XISB0100", 8);
    const uint32_t length = static_cast<uint32_t>(blocks.size());
    for (int i = 0; i < 4; ++i) out[16 + i] = static_cast<uint8_t>(length >> (8 * i));
    // (reserved: zero. Next node: zero, this is the last one.)
    uint8_t* p = out.data() + kXisbPreamble + kXisbNodeHeader;
    for (const XisbOutBlock& b : blocks) {
        putLe64(p, b.id);
        putLe64(p + 8, b.position);
        putLe64(p + 16, b.length);
        putLe64(p + 24, b.uncompressedLength);
        p += kXisbElement;
    }
    return out;
}

// ------------------------------------------------------------------------------------------
// The files beside a header
// ------------------------------------------------------------------------------------------

XisfExternalFiles::XisfExternalFiles(const std::string& headerPath, const XisfBlocksRedirect* redirect, bool problemsAreKept)
    : headerPath_(headerPath), policy_(externalFilesPolicy()), problemsAreKept_(problemsAreKept) {
    if (redirect) redirect_ = *redirect;
    std::error_code ec;
    const fs::path absolute = fs::absolute(toPath(headerPath), ec);
    headerDir_ = (ec ? toPath(headerPath) : absolute).parent_path();
    if (headerDir_.empty()) headerDir_ = ".";
}

fs::path XisfExternalFiles::wanted(const XisfLocation& location, std::string* url) const {
    if (location.kind == XisfLocation::Kind::Url) {
        std::string local;
        if (!localPathOfUrl(location.target, local)) {
            if (url) *url = location.target;
            return {};
        }
        return toPath(local);
    }
    if (location.headerDir) {
        if (!redirect_.name.empty() && location.target == redirect_.name) return toPath(redirect_.path);
        return headerDir_ / toPath(location.target);
    }
    return toPath(location.target);
}

std::string XisfExternalFiles::asked(const XisfLocation& location) const {
    std::string url;
    const fs::path path = wanted(location, &url);
    return url.empty() ? fromPath(path) : std::string();
}

std::string XisfExternalFiles::where(const XisfLocation& location) const {
    std::string url;
    const fs::path path = wanted(location, &url);
    if (!url.empty()) return url;
    return fromPath(path.lexically_normal());
}

std::string XisfExternalFiles::resolve(const XisfLocation& location, const std::string& what, bool forMessage) const {
    if (!location.external()) throw Error("internal error: " + what + " is not in another file");
    const std::string shown = where(location);
    // (The data blocks file of a unit that was just written and is read back is no file a header
    // sent the reader to: the policy is about what is read, not about what was written.)
    const bool redirected = location.kind == XisfLocation::Kind::Path && location.headerDir && !redirect_.name.empty() &&
                            location.target == redirect_.name;
    if (policy_ == ExternalFiles::None && !redirected) {
        throw NotAllowed(what + " is in another file (" + shown + "), and no file but the header is to be opened");
    }
    std::string url;
    const fs::path path = wanted(location, &url);
    if (!url.empty()) {
        throw Unsupported(what + " is at " + url + ": nothing is fetched from a network (get the file, and name it with "
                          "path(@header_dir/...) in the header)");
    }
    static const char* const kUnlessAllowed = "; a header is followed only to files in its own directory unless that is "
                                              "allowed (--external-files anywhere)";
    if (!notFollowed_.empty() && policy_ != ExternalFiles::Anywhere && !redirected) {
        throw NotAllowed(what + " is in another file (" + shown + "), and " + notFollowed_ + ": only a header file (.xish) is "
                         "followed to its data unless that is allowed (--external-files anywhere)");
    }
    if (!location.headerDir && policy_ != ExternalFiles::Anywhere) {
        throw NotAllowed(what + " is in " + shown + ", which the header names by " +
                         (location.kind == XisfLocation::Kind::Url ? "a URL" : "an absolute path") + kUnlessAllowed);
    }
    if (location.headerDir && !redirected) {
        // (C:name and \\server\share of Windows are no relative paths, whatever stands before them)
        const fs::path relative = toPath(location.target);
        if (relative.has_root_name() || relative.has_root_directory()) {
            throw Error(what + ": '" + location.target + "' is not a path below @header_dir");
        }
        // A path that climbs out of the directory by its own words is not followed, and nothing
        // is looked at to tell whether there is such a file. (Where links lead is seen below.)
        const fs::path plain = relative.lexically_normal();
        if (policy_ == ExternalFiles::HeaderDirectory && !plain.empty() && *plain.begin() == "..") {
            throw NotAllowed(what + " is in @header_dir/" + location.target + ", which leads out of the directory of the header" +
                             kUnlessAllowed);
        }
    }
    std::error_code ec;
    const fs::path real = realPath(path, ec);
    if (policy_ == ExternalFiles::HeaderDirectory && !redirected) {
        // Where a symbolic link below the directory leads is not told, nor whether there is
        // something: a header could ask about any file of the machine that way.
        static const char* const kLink = ", behind a symbolic link that does not lead to a file in the directory of the header";
        bool inside = false;
        if (!ec) {
            std::error_code ec2;
            const fs::path directory = realPath(headerDir_, ec2);
            const fs::path below = ec2 ? fs::path() : real.lexically_relative(directory);
            inside = !(ec2 || below.empty() || *below.begin() == ".." || below.is_absolute() || below.has_root_name());
            if (!inside) throw NotAllowed(what + " is in " + shown + kLink + kUnlessAllowed);
        } else {
            fs::path part = headerDir_;
            for (const fs::path& name : toPath(location.target).lexically_normal()) {
                part /= name;
                if (isLink(part)) throw NotAllowed(what + " is in " + shown + kLink + kUnlessAllowed);
            }
        }
    }
    if (ec) {
        throw Error(what + " is in " + shown + ", which is not there or cannot be reached (" + ec.message() + "); a "
                    "distributed XISF unit is its header file and the files that header names" +
                        (forMessage ? setAsideHintFor(shown, location.hasId ? &location.id : nullptr) : std::string()),
                    ErrorKind::Io);
    }
    if (!fs::is_regular_file(fs::status(real, ec)) || ec) {
        throw Error(what + " is in " + shown + ", which is not a regular file", ErrorKind::Io);
    }
    return fromPath(real);
}

const XisbIndex& XisfExternalFiles::index(const std::string& path, const std::string& shown) {
    auto found = indexes_.find(path);
    if (found == indexes_.end()) {
        ReadIndex entry;
        try {
            entry.index = std::make_shared<XisbIndex>(readXisbIndex(path, shown));
        } catch (const Error& e) {
            entry.error = e.what();
            entry.kind = e.kind;
        }
        if (entry.index) {
            for (const std::string& remark : entry.index->remarks) warn(shown + ": " + remark);
            for (const std::string& problem : entry.index->problems) {
                indexProblems_.push_back(shown + ": " + problem);
                if (!problemsAreKept_) warn(indexProblems_.back());
            }
        }
        found = indexes_.emplace(path, std::move(entry)).first;
    }
    if (!found->second.index) throw Error(found->second.error, found->second.kind);
    return *found->second.index;
}

const XisbElement& XisfExternalFiles::element(const XisfLocation& location, const std::string& path, const std::string& what) {
    const std::string shown = where(location);
    const XisbIndex& idx = index(path, shown);
    const auto found = idx.byId.find(location.id);
    if (found == idx.byId.end()) {
        throw Error(what + ": the data blocks file " + shown + " has no block " + hexId(location.id) + " (" +
                    std::to_string(idx.elements.size()) + " in its index); is it the file that was written with this header?" +
                    setAsideHintFor(path, &location.id));
    }
    const XisbElement& e = idx.elements[found->second];
    if (e.position == 0) {
        throw Error(what + ": block " + hexId(location.id) + " of " + shown + " is a free index element: it points to no data");
    }
    if (e.position < kXisbPreamble || e.position > idx.fileSize || e.length > idx.fileSize - e.position) {
        throw Error(what + ": block " + hexId(location.id) + " of " + shown + " at " + std::to_string(e.position) + "+" +
                    std::to_string(e.length) + " lies beyond the end of the file (truncated?)");
    }
    return e;
}

uint64_t XisfExternalFiles::storedSize(const XisfLocation& location, const std::string& what) {
    const std::string path = resolve(location, what, true);
    if (location.hasId) return element(location, path, what).length;
    std::error_code ec;
    const uint64_t size = static_cast<uint64_t>(fs::file_size(toPath(path), ec));
    if (ec) throw Error(what + ": cannot tell the size of " + where(location), ErrorKind::Io);
    return size;
}

uint64_t XisfExternalFiles::indexedBytes(const std::string& path, const std::set<uint64_t>& ids) {
    try {
        const XisbIndex& idx = index(path, path);
        uint64_t bytes = 0;
        for (const uint64_t id : ids) {
            const auto found = idx.byId.find(id);
            if (found == idx.byId.end()) continue;
            const XisbElement& e = idx.elements[found->second];
            if (e.position < kXisbPreamble || e.position > idx.fileSize || e.length > idx.fileSize - e.position) continue;
            bytes = e.length > idx.fileSize - bytes ? idx.fileSize : bytes + e.length;
        }
        return bytes;
    } catch (const Error&) {
        return 0;
    }
}

XisfExternalFiles::Place XisfExternalFiles::locate(const XisfLocation& location, const std::string& what) {
    Place place;
    place.path = resolve(location, what, true);
    if (location.hasId) {
        const XisbElement& e = element(location, place.path, what);
        place.position = e.position;
        place.size = e.length;
        place.indexed = true;
        place.uncompressedLength = e.uncompressedLength;
        return place;
    }
    // the block is the whole file
    std::ifstream file(toPath(place.path), std::ios::binary);
    if (!file) throw Error(what + ": cannot open " + where(location), ErrorKind::Io);
    file.seekg(0, std::ios::end);
    place.size = static_cast<uint64_t>(file.tellg());
    return place;
}

XisfExternalFiles::Block XisfExternalFiles::read(const XisfLocation& location, const std::string& what) {
    const Place place = locate(location, what);
    const std::string shown = where(location);
    Block block;
    block.indexed = place.indexed;
    block.uncompressedLength = place.uncompressedLength;
    std::ifstream file(toPath(place.path), std::ios::binary);
    if (!file) throw Error(what + ": cannot open " + shown, ErrorKind::Io);
    if (place.size > std::numeric_limits<size_t>::max()) throw Error("data block too large for this platform");
    block.bytes.resize(static_cast<size_t>(place.size));
    file.clear();
    file.seekg(static_cast<std::streamoff>(place.position));
    if (place.size > 0 && !file.read(reinterpret_cast<char*>(block.bytes.data()), static_cast<std::streamsize>(place.size))) {
        throw Error(what + ": read error in " + shown, ErrorKind::Io);
    }
    return block;
}

}  // namespace xisfconv
