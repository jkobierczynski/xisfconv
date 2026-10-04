// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "xml.hpp"

#include <cstdint>
#include <cstring>

#include "common.hpp"

namespace xisfconv::xml {

const std::string* Node::attr(const std::string& key) const {
    for (const auto& a : attributes) {
        if (a.first == key) return &a.second;
    }
    return nullptr;
}

const Node* Node::child(const std::string& childName) const {
    for (const auto& c : children) {
        if (c->name == childName) return c.get();
    }
    return nullptr;
}

std::vector<const Node*> Node::childrenNamed(const std::string& childName) const {
    std::vector<const Node*> out;
    for (const auto& c : children) {
        if (c->name == childName) out.push_back(c.get());
    }
    return out;
}

namespace {

constexpr int kMaxDepth = 256;

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
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    std::unique_ptr<Node> parseDocument() {
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) p_ = 3;  // UTF-8 BOM
        skipMisc();
        if (p_ >= s_.size() || s_[p_] != '<') fail("expected root element");
        auto root = parseElement(0);
        skipMisc();
        // Trailing NUL or whitespace padding is tolerated.
        while (p_ < s_.size() && (s_[p_] == '\0' || isSpace(s_[p_]))) ++p_;
        if (p_ != s_.size()) fail("unexpected content after root element");
        return root;
    }

private:
    const std::string& s_;
    size_t p_ = 0;

    [[noreturn]] void fail(const std::string& msg) const {
        throw Error("malformed XML header: " + msg + " (offset " + std::to_string(p_) + ")");
    }

    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    static bool isNameChar(char c) {
        return !(isSpace(c) || c == '=' || c == '>' || c == '/' || c == '<' || c == '"' ||
                 c == '\'' || c == '\0');
    }

    bool at(const char* t) const { return s_.compare(p_, std::strlen(t), t) == 0; }

    void skipSpace() {
        while (p_ < s_.size() && isSpace(s_[p_])) ++p_;
    }

    void skipPast(const char* terminator) {
        const size_t q = s_.find(terminator, p_);
        if (q == std::string::npos) fail(std::string("unterminated construct, expected ") + terminator);
        p_ = q + std::strlen(terminator);
    }

    // Skips whitespace, comments, processing instructions and DOCTYPE.
    void skipMisc() {
        for (;;) {
            skipSpace();
            if (at("<?")) skipPast("?>");
            else if (at("<!--")) skipPast("-->");
            else if (at("<!DOCTYPE")) skipDoctype();
            else break;
        }
    }

    void skipDoctype() {
        int depth = 0;
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '[') ++depth;
            else if (c == ']') --depth;
            else if (c == '>' && depth <= 0) return;
        }
        fail("unterminated DOCTYPE");
    }

    std::string parseName() {
        const size_t b = p_;
        while (p_ < s_.size() && isNameChar(s_[p_])) ++p_;
        if (p_ == b) fail("expected a name");
        return s_.substr(b, p_ - b);
    }

    static std::string localName(const std::string& qname) {
        const size_t colon = qname.find(':');
        return colon == std::string::npos ? qname : qname.substr(colon + 1);
    }

    std::string decodeEntities(size_t b, size_t e) const {
        std::string out;
        out.reserve(e - b);
        size_t i = b;
        while (i < e) {
            const char c = s_[i];
            if (c != '&') { out += c; ++i; continue; }
            const size_t semi = s_.find(';', i);
            if (semi == std::string::npos || semi >= e) {
                out += c;  // lenient: stray ampersand
                ++i;
                continue;
            }
            const std::string ent = s_.substr(i + 1, semi - i - 1);
            if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "amp") out += '&';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (!ent.empty() && ent[0] == '#') {
                uint32_t cp = 0;
                bool ok = ent.size() > 1;
                const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
                for (size_t k = hex ? 2 : 1; ok && k < ent.size(); ++k) {
                    const char d = ent[k];
                    uint32_t v;
                    if (d >= '0' && d <= '9') v = static_cast<uint32_t>(d - '0');
                    else if (hex && d >= 'a' && d <= 'f') v = static_cast<uint32_t>(d - 'a' + 10);
                    else if (hex && d >= 'A' && d <= 'F') v = static_cast<uint32_t>(d - 'A' + 10);
                    else { ok = false; break; }
                    cp = cp * (hex ? 16 : 10) + v;
                    if (cp > 0x10FFFF) ok = false;
                }
                if (!ok || (hex && ent.size() == 2)) fail("bad character reference &" + ent + ";");
                appendUtf8(out, cp);
            } else {
                fail("unknown entity &" + ent + ";");
            }
            i = semi + 1;
        }
        return out;
    }

    std::unique_ptr<Node> parseElement(int depth) {
        if (depth > kMaxDepth) fail("elements nested too deeply");
        auto node = std::make_unique<Node>();
        node->start = p_;
        ++p_;  // '<'
        const std::string qname = parseName();
        node->name = localName(qname);

        // Attributes
        for (;;) {
            skipSpace();
            if (p_ >= s_.size()) fail("unterminated start tag");
            if (s_[p_] == '/') {
                if (!at("/>")) fail("expected '/>'");
                node->tagClose = p_;
                node->selfClosing = true;
                p_ += 2;
                node->contentEnd = node->end = p_;
                return node;
            }
            if (s_[p_] == '>') {
                node->tagClose = p_;
                ++p_;
                break;
            }
            const size_t attributeStart = p_;
            const std::string aname = parseName();
            skipSpace();
            if (p_ >= s_.size() || s_[p_] != '=') fail("expected '=' after attribute " + aname);
            ++p_;
            skipSpace();
            if (p_ >= s_.size() || (s_[p_] != '"' && s_[p_] != '\'')) fail("expected quoted attribute value");
            const char q = s_[p_++];
            const size_t end = s_.find(q, p_);
            if (end == std::string::npos) fail("unterminated attribute value");
            node->attributes.emplace_back(aname, decodeEntities(p_, end));
            p_ = end + 1;
            node->attributeSpans.emplace_back(attributeStart, p_);
        }

        // Content
        for (;;) {
            if (p_ >= s_.size()) fail("unterminated element <" + qname + ">");
            if (at("</")) {
                node->contentEnd = p_;
                p_ += 2;
                const std::string closing = parseName();
                if (closing != qname) fail("mismatched closing tag </" + closing + "> for <" + qname + ">");
                skipSpace();
                if (p_ >= s_.size() || s_[p_] != '>') fail("expected '>'");
                ++p_;
                node->end = p_;
                return node;
            }
            if (at("<!--")) { skipPast("-->"); continue; }
            if (at("<![CDATA[")) {
                p_ += 9;
                const size_t end = s_.find("]]>", p_);
                if (end == std::string::npos) fail("unterminated CDATA section");
                node->text.append(s_, p_, end - p_);
                p_ = end + 3;
                continue;
            }
            if (at("<?")) { skipPast("?>"); continue; }
            if (s_[p_] == '<') {
                node->children.push_back(parseElement(depth + 1));
                continue;
            }
            size_t end = s_.find('<', p_);
            if (end == std::string::npos) end = s_.size();
            node->text += decodeEntities(p_, end);
            p_ = end;
        }
    }
};

}  // namespace

std::unique_ptr<Node> parse(const std::string& document) { return Parser(document).parseDocument(); }

}  // namespace xisfconv::xml
