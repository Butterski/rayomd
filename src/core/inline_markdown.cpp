#include "inline_markdown.h"

#include <cctype>

namespace TinyPdf::Internal {
namespace {

bool IsSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string_view TrimView(std::string_view value) {
    while (!value.empty() && IsSpace(value.front())) value.remove_prefix(1);
    while (!value.empty() && IsSpace(value.back())) value.remove_suffix(1);
    return value;
}

void ReplaceAll(std::string& value, const char* from, const char* to) {
    size_t position = 0;
    size_t fromLength = std::char_traits<char>::length(from);
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, fromLength, to);
        position += std::char_traits<char>::length(to);
    }
}

std::string NormalizeSymbols(std::string value) {
    if (value.find('\xE2') == std::string::npos) return value;
    ReplaceAll(value, "\xE2\x9C\x85", "[OK]");
    ReplaceAll(value, "\xE2\x9A\xA0\xEF\xB8\x8F", "[!]");
    ReplaceAll(value, "\xE2\x9A\xA0", "[!]");
    ReplaceAll(value, "\xE2\x9D\x8C", "[X]");
    return value;
}

struct InlineLink {
    std::string_view label;
    std::string_view target;
    size_t end = std::string_view::npos;
};

size_t FindDestinationEnd(std::string_view source, size_t openParen) {
    if (openParen >= source.size() || source[openParen] != '(') return std::string_view::npos;
    int depth = 0;
    bool escaped = false;
    bool inAngleDestination = false;
    for (size_t i = openParen + 1; i < source.size(); i++) {
        char ch = source[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (inAngleDestination) {
            if (ch == '>') inAngleDestination = false;
            continue;
        }
        if (ch == '<') {
            inAngleDestination = true;
            continue;
        }
        if (ch == '(') {
            depth++;
            continue;
        }
        if (ch == ')') {
            if (depth == 0) return i;
            depth--;
        }
    }
    return std::string_view::npos;
}

bool ParseLinkAt(std::string_view source, size_t start, size_t labelOffset, InlineLink& link) {
    if (start + labelOffset >= source.size()) return false;
    size_t close = source.find("](", start + labelOffset);
    if (close == std::string_view::npos) return false;
    size_t end = FindDestinationEnd(source, close + 1);
    if (end == std::string_view::npos) return false;
    link.label = source.substr(start + labelOffset, close - (start + labelOffset));
    link.target = source.substr(close + 2, end - (close + 2));
    link.end = end + 1;
    return true;
}

std::string UnescapeDestination(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); i++) {
        if (value[i] == '\\' && i + 1 < value.size() && std::ispunct((unsigned char)value[i + 1])) {
            result.push_back(value[++i]);
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}

bool ExtractDestination(std::string_view target, std::string& destination) {
    target = TrimView(target);
    if (target.empty()) return false;
    std::string_view value;
    if (target.front() == '<') {
        size_t end = target.find('>');
        if (end == std::string_view::npos || end == 1) return false;
        value = target.substr(1, end - 1);
    } else {
        size_t end = 0;
        while (end < target.size() && !IsSpace(target[end])) end++;
        value = target.substr(0, end);
    }
    value = TrimView(value);
    destination = UnescapeDestination(value);
    return !destination.empty();
}

void PushSpan(std::vector<InlineSpan>& spans, std::string_view text, bool bold, bool italic,
    bool strike, std::string_view url = {}) {
    if (text.empty()) return;
    if (!spans.empty() && spans.back().bold == bold && spans.back().italic == italic &&
        spans.back().strike == strike && spans.back().url == url) {
        spans.back().text.append(text.data(), text.size());
        return;
    }
    spans.push_back({ std::string(text), std::string(url), bold, italic, strike });
}

} // namespace

std::vector<InlineSpan> ParseInlineSpans(std::string_view input) {
    std::string source = NormalizeSymbols(std::string(input));
    std::vector<InlineSpan> spans;
    spans.reserve(4);
    std::string buffer;
    bool bold = false;
    bool italic = false;
    bool strike = false;

    auto flush = [&]() {
        PushSpan(spans, buffer, bold, italic, strike);
        buffer.clear();
    };

    for (size_t i = 0; i < source.size();) {
        if (source[i] == '!' && i + 1 < source.size() && source[i + 1] == '[') {
            InlineLink image;
            if (ParseLinkAt(source, i, 2, image)) {
                buffer += "image: ";
                buffer.append(image.label.data(), image.label.size());
                i = image.end;
                continue;
            }
        }
        if (source[i] == '[') {
            InlineLink link;
            if (ParseLinkAt(source, i, 1, link)) {
                flush();
                std::string url;
                if (ExtractDestination(link.target, url)) PushSpan(spans, link.label, bold, italic, strike, url);
                else PushSpan(spans, link.label, bold, italic, strike);
                i = link.end;
                continue;
            }
        }
        if (source[i] == '`') {
            size_t end = source.find('`', i + 1);
            if (end != std::string::npos) {
                flush();
                PushSpan(spans, std::string_view(source.data() + i + 1, end - i - 1), false, false, false);
                i = end + 1;
                continue;
            }
        }
        if (source[i] == '$' && !(i + 1 < source.size() && source[i + 1] == '$')) {
            size_t end = source.find('$', i + 1);
            if (end != std::string::npos) {
                flush();
                PushSpan(spans, std::string_view(source.data() + i + 1, end - i - 1), false, true, false);
                i = end + 1;
                continue;
            }
        }
        if (source[i] == '\\' && i + 1 < source.size()) {
            buffer.push_back(source[i + 1]);
            i += 2;
            continue;
        }
        if (i + 2 < source.size() && source.compare(i, 3, "***") == 0) {
            flush();
            bool enabled = !(bold && italic);
            bold = enabled;
            italic = enabled;
            i += 3;
            continue;
        }
        if (i + 1 < source.size() && source.compare(i, 2, "**") == 0) {
            flush();
            bold = !bold;
            i += 2;
            continue;
        }
        if (i + 1 < source.size() && source.compare(i, 2, "~~") == 0) {
            flush();
            strike = !strike;
            i += 2;
            continue;
        }
        if (source[i] == '*') {
            flush();
            italic = !italic;
            i++;
            continue;
        }
        if (source[i] == '_') {
            bool leftWord = i > 0 && std::isalnum((unsigned char)source[i - 1]);
            bool rightWord = i + 1 < source.size() && std::isalnum((unsigned char)source[i + 1]);
            if (!leftWord || !rightWord) {
                flush();
                italic = !italic;
                i++;
                continue;
            }
        }
        buffer.push_back(source[i++]);
    }

    flush();
    return spans;
}

} // namespace TinyPdf::Internal