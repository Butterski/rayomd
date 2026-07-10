#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

struct InlineSpan {
    std::string text;
    std::string url;
    bool bold = false;
    bool italic = false;
    bool strike = false;
};

std::vector<InlineSpan> ParseInlineSpans(std::string_view input);

} // namespace TinyPdf::Internal