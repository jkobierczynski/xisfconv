// Minimal non-validating XML DOM parser, sufficient for XISF headers.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xisfconv::xml {

struct Node {
    std::string name;  // local name (namespace prefix removed)
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;  // concatenated character data of this element (not of children)
    std::vector<std::unique_ptr<Node>> children;

    // Where the element sits in the parsed document (byte offsets), for editing the text in place.
    size_t start = 0;       // the '<' of the start tag
    size_t tagClose = 0;    // the '>' (or the '/' of "/>") that ends the start tag
    size_t contentEnd = 0;  // the '<' of the end tag (= end for an empty-element tag)
    size_t end = 0;         // one past the element
    bool selfClosing = false;
    // For each attribute, in order: [begin of its name, one past its closing quote).
    std::vector<std::pair<size_t, size_t>> attributeSpans;

    const std::string* attr(const std::string& key) const;
    const Node* child(const std::string& childName) const;
    std::vector<const Node*> childrenNamed(const std::string& childName) const;
};

// Throws xisfconv::Error on malformed input.
std::unique_ptr<Node> parse(const std::string& document);

}  // namespace xisfconv::xml
