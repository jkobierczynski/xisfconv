// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "yaml.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>

#include "common.hpp"

namespace xisfconv {

namespace {

constexpr int kMaxDepth = 200;

bool isWs(char c) { return c == ' ' || c == '\t'; }
bool isBreak(char c) { return c == '\n' || c == '\r'; }
bool isFlowIndicator(char c) { return c == ',' || c == '[' || c == ']' || c == '{' || c == '}'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += "\xEF\xBF\xBD";  // replacement character
    }
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct Props {
    bool hasTag = false;
    bool nonSpecific = false;  // a lone "!": PyYAML then types the scalar by its text, quoted or not
    std::string tag;
    std::string anchor;
    bool any() const { return hasTag || nonSpecific || !anchor.empty(); }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s(text) {}

    YamlPtr parseDocument() {
        if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;
        handles["!"] = "!";
        handles["!!"] = "tag:yaml.org,2002:";
        // Directives, comments and blank lines before the document.
        while (!eof()) {
            size_t q = pos;
            while (isWs(at(q))) ++q;
            if (cur() == '%') {
                size_t e = pos;
                while (!endAt(e)) ++e;
                directive(s.substr(pos, e - pos));
                pos = e;
            } else if (endAt(q) || at(q) == '#') {
                pos = q;
                while (!endAt(pos)) ++pos;
            } else {
                break;
            }
            skipBreak();
        }
        if (s.compare(pos, 3, "---") == 0 && wsOrEndAt(pos + 3)) pos += 3;
        YamlPtr root = parseNode(-1, false);
        if (skipToContent()) fail("unexpected content after the end of the document");
        return root;
    }

private:
    const std::string& s;
    size_t pos = 0;
    int depth = 0;
    size_t cachePos = 0, cacheStart = 0;  // for column()
    std::map<std::string, std::string> handles;
    std::map<std::string, YamlPtr> anchors;

    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& parser) : p(parser) {
            if (++p.depth > kMaxDepth) p.fail("nesting is too deep");
        }
        ~DepthGuard() { --p.depth; }
    };

    [[noreturn]] void fail(const std::string& what) const {
        size_t line = 1;
        const size_t end = pos < s.size() ? pos : s.size();
        for (size_t i = 0; i < end; ++i)
            if (s[i] == '\n') ++line;
        throw Error("YAML: " + what + " (line " + std::to_string(line) + ")");
    }

    bool eof() const { return pos >= s.size(); }
    char at(size_t p) const { return p < s.size() ? s[p] : '\0'; }
    char cur() const { return at(pos); }
    bool endAt(size_t p) const { return p >= s.size() || isBreak(s[p]); }
    bool wsOrEndAt(size_t p) const { return endAt(p) || isWs(s[p]); }
    bool lineStartAt(size_t p) const { return p == 0 || s[p - 1] == '\n' || s[p - 1] == '\r'; }
    // A '#' starts a comment at the start of a line, after white space, and (as in PyYAML)
    // right after a closing quote or bracket. Plain scalars check for " #" themselves.
    bool commentAt(size_t p) const {
        if (at(p) != '#') return false;
        if (lineStartAt(p)) return true;
        const char prev = s[p - 1];
        return isWs(prev) || prev == ']' || prev == '}' || prev == '"' || prev == '\'';
    }

    int column(size_t p) {
        size_t start = 0;
        bool found = false;
        const size_t stop = p >= cachePos ? cachePos : 0;
        for (size_t i = p; i > stop; --i) {
            if (s[i - 1] == '\n' || s[i - 1] == '\r') {
                start = i;
                found = true;
                break;
            }
        }
        if (!found) start = p >= cachePos ? cacheStart : 0;
        if (p >= cachePos) {
            cachePos = p;
            cacheStart = start;
        }
        if (p - start > static_cast<size_t>(std::numeric_limits<int>::max() / 2)) fail("line too long");
        return static_cast<int>(p - start);
    }
    int column() { return column(pos); }

    bool docMarkerAt(size_t p) const {
        return lineStartAt(p) && (s.compare(p, 3, "---") == 0 || s.compare(p, 3, "...") == 0) && wsOrEndAt(p + 3);
    }

    void skipSpaces() {
        while (isWs(cur())) ++pos;
    }
    void skipBreak() {
        if (cur() == '\r') ++pos;
        if (cur() == '\n') ++pos;
    }
    bool atLineEnd() const { return endAt(pos) || commentAt(pos); }

    // Moves to the next content character, skipping blank lines and comments.
    // Returns false at the end of the document.
    bool skipToContent() {
        for (;;) {
            skipSpaces();
            if (eof()) return false;
            if (commentAt(pos)) {
                while (!endAt(pos)) ++pos;
                continue;
            }
            if (isBreak(cur())) {
                ++pos;
                continue;
            }
            return !docMarkerAt(pos);
        }
    }

    void skipFlowWs() {
        for (;;) {
            if (isWs(cur()) || (!eof() && isBreak(cur()))) {
                ++pos;
            } else if (commentAt(pos)) {
                while (!endAt(pos)) ++pos;
            } else {
                return;
            }
        }
    }

    bool isSeqIndicator() const { return cur() == '-' && wsOrEndAt(pos + 1); }

    void directive(const std::string& line) {
        const auto parts = split(line, ' ');
        std::vector<std::string> words;
        for (const auto& p : parts)
            if (!trim(p).empty()) words.push_back(trim(p));
        if (words.size() >= 3 && words[0] == "%TAG") handles[words[1]] = words[2];
    }

    static YamlPtr makeScalar(std::string text, bool plain) {
        auto n = std::make_shared<YamlNode>();
        n->kind = YamlNode::Kind::Scalar;
        n->value = std::move(text);
        n->plain = plain;
        return n;
    }

    void parseProps(Props& props, bool flow) {
        for (;;) {
            if (cur() == '!') {
                if (at(pos + 1) == '<') {
                    const size_t end = s.find('>', pos);
                    if (end == std::string::npos || s.find('\n', pos) < end) fail("unterminated verbatim tag");
                    props.tag = s.substr(pos + 2, end - pos - 2);
                    props.hasTag = true;
                    pos = end + 1;
                } else {
                    const size_t start = pos;
                    while (!wsOrEndAt(pos) && !(flow && isFlowIndicator(s[pos]))) ++pos;
                    const std::string token = s.substr(start, pos - start);
                    if (token == "!") {
                        props.nonSpecific = true;
                    } else {
                        const size_t second = token.find('!', 1);
                        const std::string handle = second == std::string::npos ? "!" : token.substr(0, second + 1);
                        const std::string suffix = token.substr(second == std::string::npos ? 1 : second + 1);
                        const auto it = handles.find(handle);
                        if (it == handles.end()) fail("undefined tag handle " + handle);
                        std::string decoded;
                        for (size_t i = 0; i < suffix.size(); ++i) {
                            if (suffix[i] == '%' && i + 2 < suffix.size() && hexDigit(suffix[i + 1]) >= 0 &&
                                hexDigit(suffix[i + 2]) >= 0) {
                                decoded += static_cast<char>(hexDigit(suffix[i + 1]) * 16 + hexDigit(suffix[i + 2]));
                                i += 2;
                            } else {
                                decoded += suffix[i];
                            }
                        }
                        props.tag = it->second + decoded;
                        props.hasTag = true;
                    }
                }
            } else if (cur() == '&') {
                ++pos;
                props.anchor = readAnchorName();
            } else {
                return;
            }
            if (flow) skipFlowWs();
            else skipSpaces();
        }
    }

    YamlPtr applyProps(YamlPtr node, const Props& props) {
        if (props.hasTag) node->tag = props.tag;
        if (props.nonSpecific && node->isScalar()) node->plain = true;
        if (!props.anchor.empty()) anchors[props.anchor] = node;
        return node;
    }

    // Anchor and alias names: letters, digits, '-' and '_' (PyYAML's rule).
    std::string readAnchorName() {
        const size_t start = pos;
        while ((cur() >= '0' && cur() <= '9') || (cur() >= 'A' && cur() <= 'Z') || (cur() >= 'a' && cur() <= 'z') ||
               cur() == '-' || cur() == '_') {
            ++pos;
        }
        if (pos == start) fail("empty anchor name");
        return s.substr(start, pos - start);
    }

    YamlPtr parseAlias() {
        ++pos;
        const std::string name = readAnchorName();
        const auto it = anchors.find(name);
        if (it == anchors.end()) fail("alias *" + name + " refers to an unknown anchor");
        return it->second;
    }

    // True if the line at pos starts a block mapping entry ("key: ..." or "? ...").
    bool isMappingKeyAhead() const {
        size_t p = pos;
        while (at(p) == '!' || at(p) == '&') {
            if (at(p) == '!' && at(p + 1) == '<') {
                while (!endAt(p) && s[p] != '>') ++p;
            }
            while (!wsOrEndAt(p)) ++p;
            while (isWs(at(p))) ++p;
        }
        if (endAt(p) || s[p] == '#') return false;  // only properties (and maybe a comment) on this line
        const char c = s[p];
        if (c == '?' && wsOrEndAt(p + 1)) return true;
        if (c == '[' || c == '{' || c == '|' || c == '>') return false;
        if (c == '"' || c == '\'') {
            ++p;
            while (!endAt(p)) {
                if (c == '"' && s[p] == '\\') {
                    p += 2;
                    continue;
                }
                if (s[p] == c) {
                    if (c == '\'' && at(p + 1) == '\'') {
                        p += 2;
                        continue;
                    }
                    break;
                }
                ++p;
            }
            if (endAt(p)) return false;
            ++p;
            while (isWs(at(p))) ++p;
            return at(p) == ':' && wsOrEndAt(p + 1);
        }
        for (size_t q = p; !endAt(q); ++q) {
            if (s[q] == ':' && wsOrEndAt(q + 1)) return true;
            if (s[q] == '#' && q > p && isWs(s[q - 1])) return false;
        }
        return false;
    }

    // Reads plain text up to the end of the line, a comment, or (for keys) the ':' indicator.
    std::string readPlainLine(bool stopAtColon) {
        const size_t start = pos;
        while (!endAt(pos)) {
            const char c = s[pos];
            if (c == '#' && pos > start && isWs(s[pos - 1])) break;
            if (stopAtColon && c == ':' && wsOrEndAt(pos + 1)) break;
            ++pos;
        }
        size_t end = pos;
        while (end > start && isWs(s[end - 1])) --end;
        return s.substr(start, end - start);
    }

    YamlPtr parsePlainBlock(int parentIndent) {
        std::string out = readPlainLine(false);
        while (endAt(pos)) {  // a comment ends the scalar
            // Look for a continuation line: more indented than the parent, not a comment.
            size_t p = pos;
            size_t blanks = 0;
            bool cont = false;
            while (p < s.size()) {
                if (s[p] == '\r') ++p;
                if (p < s.size() && s[p] == '\n') ++p;
                const size_t lineStart = p;
                while (isWs(at(p))) ++p;
                if (p >= s.size()) break;
                if (isBreak(s[p])) {
                    ++blanks;
                    continue;
                }
                const long col = static_cast<long>(p - lineStart);
                cont = col > parentIndent && s[p] != '#' && !docMarkerAt(p);
                break;
            }
            if (!cont) break;
            pos = p;
            if (blanks) out.append(blanks, '\n');
            else out += ' ';
            out += readPlainLine(false);
        }
        return makeScalar(out, true);
    }

    YamlPtr parseDoubleQuoted() {
        std::string out;
        size_t escapedEnd = 0;  // out.size() after the last escape: folding must not strip below it
        ++pos;
        for (;;) {
            if (eof()) fail("unterminated double-quoted string");
            const char c = s[pos];
            if (c == '"') {
                ++pos;
                break;
            }
            if (c == '\\') {
                ++pos;
                if (eof()) fail("unterminated double-quoted string");
                const char e = s[pos++];
                switch (e) {
                    case '0': out += '\0'; break;
                    case 'a': out += '\a'; break;
                    case 'b': out += '\b'; break;
                    case 't': case '\t': out += '\t'; break;
                    case 'n': out += '\n'; break;
                    case 'v': out += '\v'; break;
                    case 'f': out += '\f'; break;
                    case 'r': out += '\r'; break;
                    case 'e': out += '\x1B'; break;
                    case ' ': out += ' '; break;
                    case '"': out += '"'; break;
                    case '/': out += '/'; break;
                    case '\\': out += '\\'; break;
                    case 'N': appendUtf8(out, 0x85); break;
                    case '_': appendUtf8(out, 0xA0); break;
                    case 'L': appendUtf8(out, 0x2028); break;
                    case 'P': appendUtf8(out, 0x2029); break;
                    case 'x': case 'u': case 'U': {
                        uint32_t cp = readHex(e == 'x' ? 2 : e == 'u' ? 4 : 8);
                        if (cp >= 0xD800 && cp < 0xDC00 && cur() == '\\' && at(pos + 1) == 'u') {
                            const size_t save = pos;
                            pos += 2;
                            const uint32_t low = readHex(4);
                            if (low >= 0xDC00 && low < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            else pos = save;
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    case '\r':
                    case '\n':
                        // Escaped line break: the text continues on the next line without a space.
                        // Blank lines that follow each stand for a newline.
                        if (e == '\r' && cur() == '\n') ++pos;
                        for (;;) {
                            skipSpaces();
                            if (eof() || !isBreak(cur())) break;
                            skipBreak();
                            out += '\n';
                        }
                        break;
                    default:
                        fail("unknown escape sequence in double-quoted string");
                }
                escapedEnd = out.size();
            } else if (isBreak(c)) {
                foldQuotedBreaks(out, escapedEnd);
            } else {
                out += c;
                ++pos;
            }
        }
        return makeScalar(out, false);
    }

    uint32_t readHex(int digits) {
        uint32_t v = 0;
        for (int i = 0; i < digits; ++i) {
            const int h = hexDigit(cur());
            if (h < 0) fail("invalid hexadecimal escape");
            v = v * 16 + static_cast<uint32_t>(h);
            ++pos;
        }
        return v;
    }

    // Line folding inside quoted scalars: one break becomes a space, n breaks become n-1 newlines.
    void foldQuotedBreaks(std::string& out, size_t keep) {
        while (out.size() > keep && isWs(out.back())) out.pop_back();
        size_t breaks = 0;
        for (;;) {
            skipSpaces();
            if (eof() || !isBreak(cur())) break;
            skipBreak();
            ++breaks;
        }
        if (breaks <= 1) out += ' ';
        else out.append(breaks - 1, '\n');
    }

    YamlPtr parseSingleQuoted() {
        std::string out;
        ++pos;
        for (;;) {
            if (eof()) fail("unterminated single-quoted string");
            const char c = s[pos];
            if (c == '\'') {
                if (at(pos + 1) == '\'') {
                    out += '\'';
                    pos += 2;
                    continue;
                }
                ++pos;
                break;
            }
            if (isBreak(c)) {
                foldQuotedBreaks(out, 0);
            } else {
                out += c;
                ++pos;
            }
        }
        return makeScalar(out, false);
    }

    // Literal (|) and folded (>) block scalars.
    YamlPtr parseBlockScalar(int parentIndent) {
        const bool folded = cur() == '>';
        ++pos;
        char chomp = 0;
        int explicitIndent = 0;
        for (int i = 0; i < 2; ++i) {
            if (cur() == '+' || cur() == '-') chomp = s[pos++];
            else if (cur() >= '1' && cur() <= '9') explicitIndent = s[pos++] - '0';
        }
        skipSpaces();
        if (!atLineEnd()) fail("unexpected text after block scalar indicator");
        while (!endAt(pos)) ++pos;
        skipBreak();

        long indent = explicitIndent ? (parentIndent < 0 ? 0 : parentIndent) + explicitIndent : -1;
        std::vector<std::string> lines;
        while (!eof()) {
            size_t p = pos;
            while (at(p) == ' ') ++p;
            const long col = static_cast<long>(p - pos);
            if (endAt(p)) {  // blank line
                lines.emplace_back(indent >= 0 && col > indent ? s.substr(pos + static_cast<size_t>(indent), p - pos - static_cast<size_t>(indent)) : std::string());
                pos = p;
                if (eof()) break;
                skipBreak();
                continue;
            }
            if (indent < 0) {
                if (col <= parentIndent) break;
                indent = col;
            }
            if (col < indent || docMarkerAt(pos)) break;
            size_t e = p;
            while (!endAt(e)) ++e;
            lines.push_back(s.substr(pos + static_cast<size_t>(indent), e - pos - static_cast<size_t>(indent)));
            pos = e;
            skipBreak();
        }
        size_t trailing = 0;
        while (!lines.empty() && lines.back().empty()) {
            lines.pop_back();
            ++trailing;
        }
        std::string out;
        size_t pending = 0;
        bool first = true, prevIndented = false;
        for (const auto& line : lines) {
            if (line.empty()) {
                ++pending;
                continue;
            }
            const bool indented = isWs(line[0]);
            if (!folded) {
                if (!first) out += '\n';
                out.append(pending, '\n');
            } else if (first) {
                out.append(pending, '\n');
            } else if (pending) {
                out.append(pending + ((indented || prevIndented) ? 1 : 0), '\n');
            } else {
                out += (indented || prevIndented) ? '\n' : ' ';
            }
            out += line;
            pending = 0;
            first = false;
            prevIndented = indented;
        }
        if (!lines.empty() && chomp != '-') out += '\n';
        if (chomp == '+') out.append(trailing, '\n');
        return makeScalar(out, false);
    }

    YamlPtr parseFlowPlain() {
        std::string out;
        for (;;) {
            const size_t start = pos;
            while (!endAt(pos)) {
                const char c = s[pos];
                if (isFlowIndicator(c)) break;
                if (c == ':' && (wsOrEndAt(pos + 1) || isFlowIndicator(s[pos + 1]))) break;
                if (c == '#' && pos > start && isWs(s[pos - 1])) break;
                ++pos;
            }
            size_t end = pos;
            while (end > start && isWs(s[end - 1])) --end;
            if (end > start) {
                if (!out.empty()) out += ' ';
                out.append(s, start, end - start);
            }
            if (!endAt(pos) && !commentAt(pos)) break;  // stopped at an indicator
            skipFlowWs();
            if (eof()) fail("unterminated flow collection");
            const char c = cur();
            if (isFlowIndicator(c) || (c == ':' && (wsOrEndAt(pos + 1) || isFlowIndicator(at(pos + 1))))) break;
        }
        return makeScalar(out, true);
    }

    bool flowColonAt(size_t p, bool afterQuoted) const {
        return at(p) == ':' && (afterQuoted || wsOrEndAt(p + 1) || isFlowIndicator(at(p + 1)));
    }

    YamlPtr parseFlowNode(bool* quoted = nullptr) {
        DepthGuard guard(*this);
        Props props;
        parseProps(props, true);
        if (quoted) *quoted = false;
        const char c = cur();
        if (eof()) fail("unterminated flow collection");
        if (c == '*') return parseAlias();
        if (c == '[') {
            auto node = std::make_shared<YamlNode>();
            node->kind = YamlNode::Kind::Sequence;
            ++pos;
            for (;;) {
                skipFlowWs();
                if (eof()) fail("unterminated flow sequence");
                if (cur() == ']') {
                    ++pos;
                    break;
                }
                bool q = false;
                YamlPtr item = parseFlowNode(&q);
                skipFlowWs();
                if (flowColonAt(pos, q)) {  // single-pair mapping: [key: value]
                    ++pos;
                    skipFlowWs();
                    auto pair = std::make_shared<YamlNode>();
                    pair->kind = YamlNode::Kind::Mapping;
                    YamlPtr value = (cur() == ',' || cur() == ']') ? makeScalar("", true) : parseFlowNode();
                    pair->pairs.emplace_back(item, value);
                    item = pair;
                    skipFlowWs();
                }
                node->items.push_back(item);
                if (cur() == ',') ++pos;
                else if (cur() != ']') fail("expected ',' or ']' in flow sequence");
            }
            return applyProps(node, props);
        }
        if (c == '{') {
            auto node = std::make_shared<YamlNode>();
            node->kind = YamlNode::Kind::Mapping;
            ++pos;
            for (;;) {
                skipFlowWs();
                if (eof()) fail("unterminated flow mapping");
                if (cur() == '}') {
                    ++pos;
                    break;
                }
                if (cur() == '?' && wsOrEndAt(pos + 1)) {
                    ++pos;
                    skipFlowWs();
                }
                bool q = false;
                YamlPtr key = (cur() == ':' || cur() == ',' || cur() == '}') ? makeScalar("", true) : parseFlowNode(&q);
                skipFlowWs();
                YamlPtr value;
                if (flowColonAt(pos, q) || cur() == ':') {
                    ++pos;
                    skipFlowWs();
                    value = (cur() == ',' || cur() == '}') ? makeScalar("", true) : parseFlowNode();
                    skipFlowWs();
                } else {
                    value = makeScalar("", true);
                }
                node->pairs.emplace_back(key, value);
                if (cur() == ',') ++pos;
                else if (cur() != '}') fail("expected ',' or '}' in flow mapping");
            }
            return applyProps(node, props);
        }
        if (c == '"' || c == '\'') {
            if (quoted) *quoted = true;
            return applyProps(c == '"' ? parseDoubleQuoted() : parseSingleQuoted(), props);
        }
        if (c == ']' || c == '}' || c == ',') {
            if (!props.any()) fail("unexpected '" + std::string(1, c) + "' in flow collection");
            return applyProps(makeScalar("", true), props);
        }
        const size_t before = pos;
        YamlPtr node = parseFlowPlain();
        if (pos == before && !props.any()) fail("unexpected character in flow collection");
        return applyProps(node, props);
    }

    YamlPtr parseBlockSeq(int col) {
        DepthGuard guard(*this);
        auto node = std::make_shared<YamlNode>();
        node->kind = YamlNode::Kind::Sequence;
        for (;;) {
            ++pos;  // the '-'
            node->items.push_back(parseNode(col, false));
            if (!skipToContent() || column() != col || !isSeqIndicator()) break;
        }
        return node;
    }

    YamlPtr parseBlockMap(int col) {
        DepthGuard guard(*this);
        auto node = std::make_shared<YamlNode>();
        node->kind = YamlNode::Kind::Mapping;
        for (;;) {
            YamlPtr key, value;
            if (cur() == '?' && wsOrEndAt(pos + 1)) {
                ++pos;
                key = parseNode(col, false);
                if (skipToContent() && column() == col && cur() == ':' && wsOrEndAt(pos + 1)) {
                    ++pos;
                    value = parseNode(col, true);
                } else {
                    value = makeScalar("", true);
                }
            } else {
                Props props;
                parseProps(props, false);
                if (cur() == '"') key = parseDoubleQuoted();
                else if (cur() == '\'') key = parseSingleQuoted();
                else if (cur() == '*') key = parseAlias();
                else key = makeScalar(readPlainLine(true), true);
                if (props.any()) key = applyProps(key, props);
                skipSpaces();
                if (cur() != ':') fail("expected ':' after a mapping key");
                ++pos;
                value = parseNode(col, true);
            }
            node->pairs.emplace_back(key, value);
            if (!skipToContent()) break;
            const int c = column();
            if (c > col) fail("unexpected indentation");
            if (c < col || isSeqIndicator()) break;
            if (!isMappingKeyAhead()) fail("expected a mapping key");
        }
        return node;
    }

    // Parses the node that follows "key:", "- " or the document start. Content on following
    // lines belongs to it when indented deeper than parentIndent (a block sequence may also sit
    // at the indentation of its parent mapping).
    YamlPtr parseNode(int parentIndent, bool seqAtParent) {
        DepthGuard guard(*this);
        Props props;
        for (;;) {
            skipSpaces();
            if (atLineEnd()) {
                if (!skipToContent()) return applyProps(makeScalar("", true), props);
                const int c = column();
                if (!(c > parentIndent || (seqAtParent && c == parentIndent && isSeqIndicator()))) {
                    return applyProps(makeScalar("", true), props);
                }
            }
            if (isSeqIndicator()) return applyProps(parseBlockSeq(column()), props);
            if (isMappingKeyAhead()) return applyProps(parseBlockMap(column()), props);
            if (cur() == '!' || cur() == '&') {
                parseProps(props, false);
                continue;
            }
            break;
        }
        const char c = cur();
        if (c == '*') return parseAlias();
        if (c == '[' || c == '{') {
            YamlPtr node = parseFlowNode();
            return props.any() ? applyProps(node, props) : node;
        }
        if (c == '|' || c == '>') return applyProps(parseBlockScalar(parentIndent), props);
        if (c == '"') return applyProps(parseDoubleQuoted(), props);
        if (c == '\'') return applyProps(parseSingleQuoted(), props);
        return applyProps(parsePlainBlock(parentIndent), props);
    }
};

// Decimal text to double with strtod's handling of the extremes: overflow gives infinity (as in
// Python), subnormal results are kept. The text has been validated by the caller.
bool toDouble(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size();
}

std::string stripUnderscores(const std::string& t) {
    std::string out;
    for (char c : t)
        if (c != '_') out += c;
    return out;
}

bool resolveNull(const std::string& t) { return t.empty() || t == "~" || t == "null" || t == "Null" || t == "NULL"; }

bool resolveBool(const std::string& t, bool& out) {
    static const char* yes[] = {"yes", "Yes", "YES", "true", "True", "TRUE", "on", "On", "ON"};
    static const char* no[] = {"no", "No", "NO", "false", "False", "FALSE", "off", "Off", "OFF"};
    for (const char* w : yes)
        if (t == w) {
            out = true;
            return true;
        }
    for (const char* w : no)
        if (t == w) {
            out = false;
            return true;
        }
    return false;
}

// Base 60 notation of YAML 1.1 (e.g. 1:30:00). `body` has no sign.
bool sexagesimal(const std::string& body, bool allowFraction, double& out) {
    if (body.find(':') == std::string::npos) return false;
    std::string head = body, fraction;
    const size_t dot = body.find('.');
    if (dot != std::string::npos) {
        if (!allowFraction) return false;
        head = body.substr(0, dot);
        fraction = stripUnderscores(body.substr(dot + 1));
        for (char c : fraction)
            if (!isDigit(c)) return false;
    } else if (allowFraction) {
        return false;
    }
    const auto parts = split(head, ':');
    double v = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string p = i == 0 ? stripUnderscores(parts[i]) : parts[i];
        if (p.empty() || (i > 0 && p.size() > 2)) return false;
        for (char c : p)
            if (!isDigit(c)) return false;
        if (i == 0 && !isDigit(parts[i][0])) return false;
        double d = 0;
        if (!parseDouble(p, d) || (i > 0 && d >= 60)) return false;
        v = v * 60 + d;
    }
    if (!fraction.empty()) {
        double f = 0;
        if (!parseDouble("0." + fraction, f)) return false;
        v += f;
    }
    out = v;
    return true;
}

// Integers as PyYAML reads them (YAML 1.1): decimal, 0b binary, 0x hexadecimal, octal with a
// leading zero, and base 60.
bool resolveInt(const std::string& t, YamlValue& v) {
    if (t.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (t[0] == '+' || t[0] == '-') {
        neg = t[0] == '-';
        i = 1;
    }
    const std::string body = t.substr(i);
    if (body.empty() || !isDigit(body[0])) return false;
    auto radix = [&](const std::string& digits, unsigned base) -> bool {
        const std::string d = stripUnderscores(digits);
        if (d.empty()) return false;
        unsigned long long acc = 0;
        for (char c : d) {
            const int h = hexDigit(c);
            if (h < 0 || static_cast<unsigned>(h) >= base) return false;
            if (acc > (std::numeric_limits<unsigned long long>::max() - static_cast<unsigned>(h)) / base) return false;
            acc = acc * base + static_cast<unsigned>(h);
        }
        v.text = (neg && acc ? "-" : "") + std::to_string(acc);
        v.number = neg ? -static_cast<double>(acc) : static_cast<double>(acc);
        return true;
    };
    bool ok = false;
    if (stripUnderscores(body) == "0") {
        for (char c : body.substr(1))
            if (c != '_') return false;
        v.text = "0";
        v.number = 0;
        ok = true;
    } else if (startsWith(body, "0b")) {
        ok = radix(body.substr(2), 2);
    } else if (startsWith(body, "0x")) {
        ok = radix(body.substr(2), 16);
    } else if (body[0] == '0') {
        ok = radix(body.substr(1), 8);
    } else if (body.find(':') != std::string::npos) {
        double d = 0;
        if (sexagesimal(body, false, d) && d < 9e15) {
            v.text = (neg ? "-" : "") + std::to_string(static_cast<long long>(d));
            v.number = neg ? -d : d;
            ok = true;
        }
    } else {
        const std::string digits = stripUnderscores(body);
        for (char c : digits)
            if (!isDigit(c)) return false;
        double d = 0;
        if (!toDouble(digits, d)) return false;
        v.text = (neg ? "-" : "") + digits;
        v.number = neg ? -d : d;
        ok = true;
    }
    if (ok) v.type = YamlValue::Type::Int;
    return ok;
}

// Floats as PyYAML reads them (YAML 1.1): the mantissa needs a '.', the exponent a sign, and a
// number starting with '.' takes no sign. With `lenient` (explicit !!float tag) any decimal
// notation is accepted.
bool resolveFloat(const std::string& t, YamlValue& v, bool lenient) {
    if (t.empty()) return false;
    size_t i = 0;
    bool neg = false, signedText = false;
    if (t[0] == '+' || t[0] == '-') {
        neg = t[0] == '-';
        signedText = true;
        i = 1;
    }
    const std::string body = t.substr(i);
    auto done = [&](double d, const std::string& text) {
        v.number = d;
        v.text = text;
        v.type = YamlValue::Type::Float;
        return true;
    };
    if (body == ".inf" || body == ".Inf" || body == ".INF") {
        const double inf = std::numeric_limits<double>::infinity();
        return done(neg ? -inf : inf, neg ? "-inf" : "inf");
    }
    if (!signedText && (body == ".nan" || body == ".NaN" || body == ".NAN")) {
        return done(std::numeric_limits<double>::quiet_NaN(), "nan");
    }
    if (body.empty() || !(isDigit(body[0]) || body[0] == '.')) return false;
    double d = 0;
    if (sexagesimal(body, true, d)) return done(neg ? -d : d, formatDouble(neg ? -d : d));
    if (body[0] == '.' && signedText && !lenient) return false;
    size_t p = 0;
    size_t mantissaDigits = 0;
    bool dot = false, exponent = false;
    std::string clean;
    while (p < body.size() && (isDigit(body[p]) || body[p] == '_' || body[p] == '.')) {
        if (body[p] == '.') {
            if (dot) return false;
            dot = true;
            clean += '.';
        } else if (body[p] == '_') {
            if (dot && mantissaDigits == 0) return false;  // "._5"
        } else {
            clean += body[p];
            ++mantissaDigits;
        }
        ++p;
    }
    if (mantissaDigits == 0) return false;
    if (p < body.size()) {
        if (body[p] != 'e' && body[p] != 'E') return false;
        clean += 'e';
        ++p;
        if (p < body.size() && (body[p] == '+' || body[p] == '-')) clean += body[p++];
        else if (!lenient) return false;
        const size_t digitsStart = p;
        while (p < body.size() && isDigit(body[p])) clean += body[p++];
        if (p == digitsStart || p != body.size()) return false;
        exponent = true;
    }
    if (!dot && !(lenient && exponent)) return false;
    if (!toDouble(clean, d)) return false;
    if (std::isinf(d)) return done(neg ? -d : d, neg ? "-inf" : "inf");
    return done(neg ? -d : d, (neg ? "-" : "") + clean);
}

}  // namespace

const YamlNode* YamlNode::get(const std::string& key) const {
    if (kind != Kind::Mapping) return nullptr;
    for (const auto& p : pairs)
        if (p.first && p.first->isScalar() && p.first->value == key) return p.second.get();
    return nullptr;
}

YamlValue yamlResolve(const YamlNode& n) {
    YamlValue v;
    v.type = YamlValue::Type::String;
    v.text = n.value;
    if (!n.isScalar()) return v;
    static const std::string core = "tag:yaml.org,2002:";
    YamlValue r;
    if (startsWith(n.tag, core)) {
        const std::string kind = n.tag.substr(core.size());
        if (kind == "null") {
            r.type = YamlValue::Type::Null;
            return r;
        }
        if (kind == "bool" && resolveBool(n.value, r.boolean)) {
            r.type = YamlValue::Type::Bool;
            return r;
        }
        if (kind == "int" && resolveInt(n.value, r)) return r;
        if (kind == "float" && (resolveFloat(n.value, r, true) || resolveInt(n.value, r))) {
            r.type = YamlValue::Type::Float;
            return r;
        }
        return v;
    }
    if (!n.plain || !n.tag.empty()) return v;
    if (resolveNull(n.value)) {
        r.type = YamlValue::Type::Null;
        return r;
    }
    if (resolveBool(n.value, r.boolean)) {
        r.type = YamlValue::Type::Bool;
        return r;
    }
    if (resolveInt(n.value, r)) return r;
    if (resolveFloat(n.value, r, false)) return r;
    return v;
}

YamlPtr parseYaml(const std::string& text) {
    Parser parser(text);
    return parser.parseDocument();
}

}  // namespace xisfconv
