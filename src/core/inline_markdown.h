#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace TinyPdf::Internal {

enum class InlineMath : unsigned char {
    None,
    Inline,   // $...$ or \( ... \)
    Display,  // $$...$$ inside paragraph text
};

// Source-size caps for math recognised inside inline text, so one stray '$' can
// never turn a whole paragraph into a single unbreakable box. A complete candidate
// with a longer body is not typeset: it becomes one code span that holds its
// source, delimiters included, byte for byte.
constexpr size_t kMaxInlineMathBytes = 1024;
constexpr size_t kMaxDisplayMathBytes = 8192;

struct InlineSpan {
    std::string text;   // math spans: the TeX source without its delimiters
    std::string url;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    InlineMath math = InlineMath::None;
};

struct ReferenceDefinition {
    std::string destination;
    std::string title;
};

using ReferenceDefinitions = std::unordered_map<std::string, ReferenceDefinition>;

std::string NormalizeReferenceLabel(std::string_view label);
std::string ResolveReferenceLinks(std::string_view input, const ReferenceDefinitions& definitions);
// recognizeMath == false keeps every '$' and "\(" literal. It exists for text that has
// already been through the inline parser once and is only being re-read as plain text.
std::vector<InlineSpan> ParseInlineSpans(std::string_view input, bool recognizeMath = true);

} // namespace TinyPdf::Internal
