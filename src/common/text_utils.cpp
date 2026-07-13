#include "text_utils.h"

#include <string_view>

namespace RayoMd::Text {
namespace {

bool IsSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

bool IsNormalizedAscii(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 128) {
            i++;
            continue;
        }
        if (i + 3 <= text.size() && text.compare(i, 3, "\xE2\x9C\x85") == 0) {
            i += 3;
            continue;
        }
        if (i + 6 <= text.size() && text.compare(i, 6, "\xE2\x9A\xA0\xEF\xB8\x8F") == 0) {
            i += 6;
            continue;
        }
        if (i + 3 <= text.size() && (text.compare(i, 3, "\xE2\x9A\xA0") == 0 ||
            text.compare(i, 3, "\xE2\x9D\x8C") == 0)) {
            i += 3;
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

std::string Trim(std::string value) {
    size_t first = 0;
    while (first < value.size() && IsSpace(value[first])) first++;
    size_t last = value.size();
    while (last > first && IsSpace(value[last - 1])) last--;
    return value.substr(first, last - first);
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    for (size_t i = start; i < text.size(); i++) {
        if (text[i] != '\n') continue;
        size_t end = i;
        if (end > start && text[end - 1] == '\r') end--;
        lines.emplace_back(text.data() + start, end - start);
        start = i + 1;
    }
    size_t end = text.size();
    if (end > start && text[end - 1] == '\r') end--;
    lines.emplace_back(text.data() + start, end - start);
    return lines;
}

std::string FormatDouble(double value) {
    std::string result;
    result.reserve(16);
    AppendFixed2(result, value);
    return result;
}

bool IsAsciiDocument(const std::string& text) {
    return IsNormalizedAscii(text);
}

} // namespace RayoMd::Text