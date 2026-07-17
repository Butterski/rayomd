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
    size_t close = source.find(']', start + labelOffset);
    if (close == std::string_view::npos || close + 1 >= source.size() || source[close + 1] != '(') {
        return false;
    }
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
    bool strike, std::string_view url = {}, bool code = false) {
    if (text.empty()) return;
    if (!spans.empty() && spans.back().bold == bold && spans.back().italic == italic &&
        spans.back().strike == strike && spans.back().url == url && spans.back().code == code) {
        spans.back().text.append(text.data(), text.size());
        return;
    }
    spans.push_back({ std::string(text), std::string(url), bold, italic, strike, code });
}

bool IsClassicEscapable(char ch) {
    constexpr std::string_view punctuation = R"(\`*{}[]()#+-.!_)";
    return punctuation.find(ch) != std::string_view::npos;
}

size_t DelimiterRun(std::string_view source, size_t start, char delimiter) {
    size_t end = start;
    while (end < source.size() && source[end] == delimiter) end++;
    return end - start;
}

size_t FindExactDelimiterRun(std::string_view source, size_t start, char delimiter, size_t length) {
    size_t position = start;
    while (position < source.size()) {
        position = source.find(delimiter, position);
        if (position == std::string_view::npos) return position;
        size_t run = DelimiterRun(source, position, delimiter);
        if (run == length) return position;
        position += run;
    }
    return std::string_view::npos;
}

bool LooksLikeEmail(std::string_view value) {
    size_t at = value.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= value.size()) return false;
    if (value.find('@', at + 1) != std::string_view::npos) return false;
    size_t dot = value.find('.', at + 1);
    if (dot == std::string_view::npos || dot == at + 1 || dot + 1 >= value.size()) return false;
    for (char ch : value) {
        if (!(std::isalnum((unsigned char)ch) || ch == '@' || ch == '.' || ch == '-' || ch == '_' ||
            ch == '+' || ch == '%')) return false;
    }
    return true;
}

bool IntrawordUnderscore(std::string_view source, size_t start, size_t length) {
    bool leftWord = start > 0 && std::isalnum((unsigned char)source[start - 1]);
    bool rightWord = start + length < source.size() &&
        std::isalnum((unsigned char)source[start + length]);
    return leftWord && rightWord;
}

bool HasClosingDelimiter(std::string_view source, size_t start, char marker, size_t length) {
    for (size_t i = start; i + length <= source.size();) {
        if (source[i] == '\\' && i + 1 < source.size()) {
            i += 2;
            continue;
        }
        if (source[i] == '`') {
            size_t run = DelimiterRun(source, i, '`');
            size_t end = FindExactDelimiterRun(source, i + run, '`', run);
            if (end == std::string_view::npos) return false;
            i = end + run;
            continue;
        }
        if (source[i] != marker || DelimiterRun(source, i, marker) < length) {
            i++;
            continue;
        }
        bool hasContent = i > start && !IsSpace(source[i - 1]);
        if (hasContent && !(marker == '_' && IntrawordUnderscore(source, i, length))) return true;
        i += length;
    }
    return false;
}

} // namespace

std::string NormalizeReferenceLabel(std::string_view label) {
    label = TrimView(label);
    std::string normalized;
    normalized.reserve(label.size());
    bool pendingSpace = false;
    for (char ch : label) {
        if (IsSpace(ch)) {
            pendingSpace = !normalized.empty();
            continue;
        }
        if (pendingSpace) normalized.push_back(' ');
        pendingSpace = false;
        normalized.push_back((char)std::tolower((unsigned char)ch));
    }
    return normalized;
}

std::string ResolveReferenceLinks(std::string_view input, const ReferenceDefinitions& definitions) {
    if (definitions.empty() || input.find('[') == std::string_view::npos) return std::string(input);
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        if (input[i] == '`') {
            size_t run = DelimiterRun(input, i, '`');
            size_t end = FindExactDelimiterRun(input, i + run, '`', run);
            if (end == std::string_view::npos) {
                output.append(input.substr(i));
                break;
            }
            output.append(input.substr(i, end + run - i));
            i = end + run;
            continue;
        }
        size_t labelStart = i;
        bool image = input[i] == '!' && i + 1 < input.size() && input[i + 1] == '[';
        if (image) labelStart++;
        if (input[labelStart] != '[') {
            output.push_back(input[i++]);
            continue;
        }
        size_t close = input.find(']', labelStart + 1);
        if (close == std::string_view::npos || (close + 1 < input.size() && input[close + 1] == '(')) {
            output.push_back(input[i++]);
            continue;
        }
        size_t cursor = close + 1;
        while (cursor < input.size() && (input[cursor] == ' ' || input[cursor] == '\t')) cursor++;
        if (cursor >= input.size() || input[cursor] != '[') {
            output.push_back(input[i++]);
            continue;
        }
        size_t referenceClose = input.find(']', cursor + 1);
        if (referenceClose == std::string_view::npos) {
            output.push_back(input[i++]);
            continue;
        }
        std::string_view visible = input.substr(labelStart + 1, close - labelStart - 1);
        std::string_view referenceLabel = input.substr(cursor + 1, referenceClose - cursor - 1);
        if (referenceLabel.empty()) referenceLabel = visible;
        auto found = definitions.find(NormalizeReferenceLabel(referenceLabel));
        if (found == definitions.end()) {
            size_t unresolvedEnd = referenceClose + 1;
            output.append(input.substr(i, unresolvedEnd - i));
            i = unresolvedEnd;
            continue;
        }
        if (image) output.push_back('!');
        output.push_back('[');
        output.append(visible);
        output += "](<";
        output.append(found->second.destination);
        output += ">)";
        i = referenceClose + 1;
    }
    return output;
}

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
            size_t run = DelimiterRun(source, i, '`');
            size_t end = FindExactDelimiterRun(source, i + run, '`', run);
            if (end != std::string::npos) {
                flush();
                std::string content = source.substr(i + run, end - i - run);
                for (char& ch : content) if (ch == '\n' || ch == '\r') ch = ' ';
                if (content.size() >= 2 && content.front() == ' ' && content.back() == ' ' &&
                    content.find_first_not_of(' ') != std::string::npos) {
                    content.erase(content.begin());
                    content.pop_back();
                }
                PushSpan(spans, content, false, false, false, {}, true);
                i = end + run;
                continue;
            }
        }
        if (source[i] == '<') {
            size_t end = source.find('>', i + 1);
            if (end != std::string::npos) {
                std::string_view target(source.data() + i + 1, end - i - 1);
                std::string url;
                if (target.size() > 7 && (target.substr(0, 7) == "http://" ||
                    (target.size() > 8 && target.substr(0, 8) == "https://"))) {
                    url.assign(target);
                } else if (LooksLikeEmail(target)) {
                    url = "mailto:";
                    url.append(target);
                }
                if (!url.empty()) {
                    flush();
                    PushSpan(spans, target, bold, italic, strike, url);
                    i = end + 1;
                    continue;
                }
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
        if (source[i] == '\\' && i + 1 < source.size() && IsClassicEscapable(source[i + 1])) {
            buffer.push_back(source[i + 1]);
            i += 2;
            continue;
        }
        if (source[i] == '*' || source[i] == '_') {
            char marker = source[i];
            size_t run = DelimiterRun(source, i, marker);
            size_t length = std::min<size_t>(3, run);
            if (marker == '_' && IntrawordUnderscore(source, i, length)) {
                buffer.append(run, marker);
                i += run;
                continue;
            }
            bool active = length == 3 ? bold && italic : (length == 2 ? bold : italic);
            bool canClose = i > 0 && !IsSpace(source[i - 1]);
            bool canOpen = i + length < source.size() && !IsSpace(source[i + length]);
            if ((active && canClose) || (canOpen && HasClosingDelimiter(source, i + length, marker, length))) {
                flush();
                if (length == 3) {
                    bold = !bold;
                    italic = !italic;
                } else if (length == 2) {
                    bold = !bold;
                } else {
                    italic = !italic;
                }
                i += length;
                continue;
            }
            buffer.append(length, marker);
            i += length;
            continue;
        }
        if (i + 1 < source.size() && source.compare(i, 2, "~~") == 0) {
            bool canOpen = i + 2 < source.size() && !IsSpace(source[i + 2]);
            bool canClose = i > 0 && !IsSpace(source[i - 1]);
            if ((strike && canClose) || (canOpen && source.find("~~", i + 2) != std::string::npos)) {
                flush();
                strike = !strike;
                i += 2;
                continue;
            }
        }
        buffer.push_back(source[i++]);
    }

    flush();
    return spans;
}

} // namespace TinyPdf::Internal