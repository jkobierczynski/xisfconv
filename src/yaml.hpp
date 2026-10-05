// Small YAML reader for ASDF trees: block and flow collections, plain, quoted and block
// scalars, tags (with %TAG handles), anchors and aliases. Merge keys are not supported.
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xisfconv {

struct YamlNode;
using YamlPtr = std::shared_ptr<YamlNode>;

struct YamlNode {
    enum class Kind { Scalar, Sequence, Mapping };
    Kind kind = Kind::Scalar;
    std::string tag;     // resolved tag, e.g. tag:stsci.edu:asdf/core/ndarray-1.0.0 (empty if none)
    std::string value;   // scalar text
    bool plain = true;   // unquoted scalar: its type is implied by its text
    std::vector<YamlPtr> items;                       // sequence
    std::vector<std::pair<YamlPtr, YamlPtr>> pairs;   // mapping, in file order

    bool isScalar() const { return kind == Kind::Scalar; }
    bool isSequence() const { return kind == Kind::Sequence; }
    bool isMapping() const { return kind == Kind::Mapping; }
    // Value of a mapping entry with the given scalar key, or nullptr.
    const YamlNode* get(const std::string& key) const;
};

// A scalar with its YAML 1.1 type resolved the way PyYAML does, the parser behind Python's
// asdf library: 1e5 and 1.5e5, for example, are strings, 1.5e+5 is a number.
struct YamlValue {
    enum class Type { Null, Bool, Int, Float, String };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;       // Int and Float
    std::string text;        // String: the text; Int: decimal digits with optional '-'; Float: cleaned-up text
};

YamlValue yamlResolve(const YamlNode& scalar);

// Parses the first document of a YAML stream. Throws xisfconv::Error on malformed input.
YamlPtr parseYaml(const std::string& text);

}  // namespace xisfconv
