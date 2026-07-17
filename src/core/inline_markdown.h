#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace TinyPdf::Internal {

struct InlineSpan {
    std::string text;
    std::string url;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
};

struct ReferenceDefinition {
    std::string destination;
    std::string title;
};

using ReferenceDefinitions = std::unordered_map<std::string, ReferenceDefinition>;

std::string NormalizeReferenceLabel(std::string_view label);
std::string ResolveReferenceLinks(std::string_view input, const ReferenceDefinitions& definitions);
std::vector<InlineSpan> ParseInlineSpans(std::string_view input);

} // namespace TinyPdf::Internal
