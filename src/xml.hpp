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

    const std::string* attr(const std::string& key) const;
    const Node* child(const std::string& childName) const;
    std::vector<const Node*> childrenNamed(const std::string& childName) const;
};

// Throws xisfconv::Error on malformed input.
std::unique_ptr<Node> parse(const std::string& document);

}  // namespace xisfconv::xml
