#include "markdown_parser.h"
#include "inline_markdown.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#include <emmintrin.h>
#define FAST_MD_SSE2 1
#endif

namespace TinyPdf::Internal {
static bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static std::string_view LTrimView(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && IsSpace(s[i])) i++;
    return s.substr(i);
}

static std::string_view RTrimView(std::string_view s) {
    while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
    return s;
}

static std::string_view TrimView(std::string_view s) {
    return RTrimView(LTrimView(s));
}

static bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

static std::string ToString(std::string_view s) {
    return std::string(s.data(), s.size());
}

static std::string LTrim(std::string s) {
    std::string_view v = LTrimView(s);
    return ToString(v);
}

static std::string RTrim(std::string s) {
    std::string_view v = RTrimView(s);
    return ToString(v);
}

static std::string Trim(std::string s) {
    std::string_view v = TrimView(s);
    return ToString(v);
}

static bool StartsWith(const std::string& s, const char* prefix) {
    return StartsWith(std::string_view(s), std::string_view(prefix, strlen(prefix)));
}

static std::vector<std::string_view> SplitLineViews(const std::string& text) {
    std::vector<std::string_view> lines;
    size_t start = StartsWith(std::string_view(text), std::string_view("\xEF\xBB\xBF", 3)) ? 3 : 0;
    lines.reserve(std::max<size_t>(8, text.size() / 48));

    const char* base = text.data();
    const char* ptr = base + start;
    const char* end = base + text.size();
    size_t lineStart = start;

#ifdef FAST_MD_SSE2
    const __m128i nl = _mm_set1_epi8('\n');
    while (ptr + 16 <= end) {
        __m128i chunk = _mm_loadu_si128((const __m128i*)ptr);
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, nl));
        while (mask) {
            int bit = 0;
            while (((mask >> bit) & 1) == 0) bit++;
            size_t pos = (size_t)(ptr - base) + (size_t)bit;
            size_t lineEnd = pos;
            if (lineEnd > lineStart && base[lineEnd - 1] == '\r') lineEnd--;
            lines.emplace_back(base + lineStart, lineEnd - lineStart);
            lineStart = pos + 1;
            mask &= ~(1 << bit);
        }
        ptr += 16;
    }
#endif

    while (ptr < end) {
        if (*ptr == '\n') {
            size_t pos = (size_t)(ptr - base);
            size_t lineEnd = pos;
            if (lineEnd > lineStart && base[lineEnd - 1] == '\r') lineEnd--;
            lines.emplace_back(base + lineStart, lineEnd - lineStart);
            lineStart = pos + 1;
        }
        ptr++;
    }

    size_t lineEnd = text.size();
    if (lineEnd > lineStart && base[lineEnd - 1] == '\r') lineEnd--;
    lines.emplace_back(base + lineStart, lineEnd - lineStart);
    return lines;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string_view> views = SplitLineViews(text);
    std::vector<std::string> lines;
    lines.reserve(views.size());
    for (std::string_view v : views) lines.emplace_back(v.data(), v.size());
    return lines;
}

static bool IsRuleLine(std::string_view line) {
    size_t i = 0;
    int indent = 0;
    while (i < line.size()) {
        if (line[i] == ' ') {
            indent++;
            i++;
        } else if (line[i] == '\t') {
            indent += 4 - (indent % 4);
            i++;
        } else {
            break;
        }
        if (indent > 3) return false;
    }

    char rule = 0;
    int count = 0;
    for (; i < line.size(); i++) {
        char c = line[i];
        if (c == ' ' || c == '\t') continue;
        if (rule == 0) {
            if (c != '-' && c != '*' && c != '_') return false;
            rule = c;
        } else if (c != rule) {
            return false;
        }
        count++;
    }
    return count >= 3;
}

static bool EqualsAsciiInsensitive(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        unsigned char ac = (unsigned char)a[i];
        unsigned char bc = (unsigned char)b[i];
        if (std::tolower(ac) != std::tolower(bc)) return false;
    }
    return true;
}

static bool IsPageBreakLine(std::string_view trimmed) {
    if (trimmed.empty()) return false;
    if (trimmed[0] == '\\') return trimmed == "\\pagebreak" || trimmed == "\\newpage";
    if (trimmed[0] != '<') return false;
    if (trimmed == "<!-- pagebreak -->" || trimmed == "<!--pagebreak-->" ||
        trimmed == "<!-- page-break -->" || trimmed == "<!-- page break -->") {
        return true;
    }

    constexpr std::string_view open = "<!--";
    constexpr std::string_view close = "-->";
    if (!StartsWith(trimmed, open) || trimmed.size() < open.size() + close.size()) return false;
    if (trimmed.substr(trimmed.size() - close.size()) != close) return false;

    std::string_view marker = TrimView(trimmed.substr(open.size(), trimmed.size() - open.size() - close.size()));
    return EqualsAsciiInsensitive(marker, "pagebreak") ||
        EqualsAsciiInsensitive(marker, "page break") ||
        EqualsAsciiInsensitive(marker, "page-break");
}

static std::string_view TrimHeadingClosingSequence(std::string_view view) {
    view = RTrimView(view);
    size_t hashStart = view.size();
    while (hashStart > 0 && view[hashStart - 1] == '#') hashStart--;
    if (hashStart == view.size()) return view;
    if (hashStart == 0 || !IsSpace(view[hashStart - 1])) return view;
    return RTrimView(view.substr(0, hashStart));
}

static bool ParseHeading(std::string_view line, int& level, std::string& text) {
    std::string_view s = LTrimView(line);
    int count = 0;
    while (count < (int)s.size() && s[count] == '#') count++;
    if (count < 1 || count > 6) return false;
    if ((int)s.size() > count && !IsSpace(s[count])) return false;
    level = count;
    std::string_view view = TrimHeadingClosingSequence(TrimView(s.substr(count)));
    text = ToString(view);
    return true;
}

static bool ParseHeadingView(std::string_view line, int& level, std::string_view& text) {
    std::string_view s = LTrimView(line);
    int count = 0;
    while (count < (int)s.size() && s[count] == '#') count++;
    if (count < 1 || count > 6) return false;
    if ((int)s.size() > count && !IsSpace(s[count])) return false;
    level = count;
    text = TrimHeadingClosingSequence(TrimView(s.substr(count)));
    return true;
}

static int CountIndent(std::string_view line) {
    int indent = 0;
    for (char c : line) {
        if (c == ' ') indent++;
        else if (c == '\t') indent += 4;
        else break;
    }
    return indent;
}

static bool ParseBullet(std::string_view line, int& level, std::string& text) {
    std::string_view s = LTrimView(line);
    if (s.size() < 2) return false;
    if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && IsSpace(s[1])) {
        level = std::min(4, CountIndent(line) / 4);
        text = ToString(TrimView(s.substr(2)));
        return true;
    }
    return false;
}

static bool ParseBulletView(std::string_view line, int& level, std::string_view& text) {
    std::string_view s = LTrimView(line);
    if (s.size() < 2) return false;
    if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && IsSpace(s[1])) {
        level = std::min(4, CountIndent(line) / 4);
        text = TrimView(s.substr(2));
        return true;
    }
    return false;
}

static bool ParseNumbered(std::string_view line, int& level, int& number, std::string& text) {
    std::string_view s = LTrimView(line);
    size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
    if (i == 0 || i >= s.size()) return false;
    if ((s[i] != '.' && s[i] != ')') || i + 1 >= s.size() || !IsSpace(s[i + 1])) return false;
    level = std::min(4, CountIndent(line) / 4);
    number = 0;
    for (size_t j = 0; j < i; j++) number = number * 10 + (s[j] - '0');
    text = ToString(TrimView(s.substr(i + 2)));
    return true;
}

static bool ParseNumberedView(std::string_view line, int& level, int& number, std::string_view& text) {
    std::string_view s = LTrimView(line);
    size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
    if (i == 0 || i >= s.size()) return false;
    if ((s[i] != '.' && s[i] != ')') || i + 1 >= s.size() || !IsSpace(s[i + 1])) return false;
    level = std::min(4, CountIndent(line) / 4);
    number = 0;
    for (size_t j = 0; j < i; j++) number = number * 10 + (s[j] - '0');
    text = TrimView(s.substr(i + 2));
    return true;
}

static std::vector<std::string> SplitTableRow(std::string_view line) {
    std::string_view s = TrimView(line);
    if (!s.empty() && s.front() == '|') s.remove_prefix(1);
    if (!s.empty() && s.back() == '|') s.remove_suffix(1);

    std::vector<std::string> cells;
    cells.reserve(4);
    std::string cell;
    bool escaped = false;
    for (char c : s) {
        if (escaped) {
            cell.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '|') {
            std::string_view v = TrimView(cell);
            cells.emplace_back(v.data(), v.size());
            cell.clear();
        } else {
            cell.push_back(c);
        }
    }
    std::string_view v = TrimView(cell);
    cells.emplace_back(v.data(), v.size());
    return cells;
}

// Same cell boundaries as SplitTableRow, but keeps every backslash except the one
// that escapes a pipe. Only used for rows that may contain math, where "\alpha"
// must survive until the inline parser has seen it.
static std::vector<std::string> SplitTableRowRaw(std::string_view line) {
    std::string_view s = TrimView(line);
    if (!s.empty() && s.front() == '|') s.remove_prefix(1);
    if (!s.empty() && s.back() == '|') s.remove_suffix(1);

    std::vector<std::string> cells;
    cells.reserve(4);
    std::string cell;
    bool escaped = false;
    for (char c : s) {
        if (escaped) {
            if (c != '|') cell.push_back('\\');
            cell.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '|') {
            std::string_view v = TrimView(cell);
            cells.emplace_back(v.data(), v.size());
            cell.clear();
        } else {
            cell.push_back(c);
        }
    }
    std::string_view v = TrimView(cell);
    cells.emplace_back(v.data(), v.size());
    return cells;
}

static bool IsTableSeparatorCell(std::string_view cell, int& align) {
    std::string_view s = TrimView(cell);
    if (s.size() < 3) return false;

    bool left = s.front() == ':';
    bool right = s.back() == ':';
    size_t start = left ? 1 : 0;
    size_t end = right ? s.size() - 1 : s.size();
    if (end <= start) return false;

    for (size_t i = start; i < end; i++) {
        if (s[i] != '-') return false;
    }

    align = left && right ? 0 : (right ? 1 : -1);
    return true;
}

static bool ParseTableSeparator(std::string_view line, std::vector<int>& aligns) {
    std::vector<std::string> cells = SplitTableRow(line);
    if (cells.empty()) return false;

    std::vector<int> parsed;
    for (const auto& cell : cells) {
        int align = -1;
        if (!IsTableSeparatorCell(cell, align)) return false;
        parsed.push_back(align);
    }

    aligns = parsed;
    return true;
}

static bool IsTableStart(const std::vector<std::string_view>& lines, size_t i) {
    if (i + 1 >= lines.size()) return false;
    if (lines[i].find('|') == std::string_view::npos ||
        lines[i + 1].find('|') == std::string_view::npos ||
        lines[i + 1].find('-') == std::string_view::npos) {
        return false;
    }
    if (SplitTableRow(lines[i]).size() < 2) return false;
    std::vector<int> aligns;
    return ParseTableSeparator(lines[i + 1], aligns);
}

static void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string NormalizeSymbols(std::string s) {
    if (s.find('\xE2') == std::string::npos) return s;
    ReplaceAll(s, std::string("\xE2\x9C\x85"), "[OK]");
    ReplaceAll(s, std::string("\xE2\x9A\xA0\xEF\xB8\x8F"), "[!]");
    ReplaceAll(s, std::string("\xE2\x9A\xA0"), "[!]");
    ReplaceAll(s, std::string("\xE2\x9D\x8C"), "[X]");
    return s;
}

struct MarkdownInlineLink {
    std::string_view label;
    std::string_view target;
    size_t end = std::string_view::npos;
};

static size_t FindInlineDestinationEnd(std::string_view source, size_t openParen) {
    if (openParen >= source.size() || source[openParen] != '(') return std::string_view::npos;

    int parenDepth = 0;
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
            parenDepth++;
            continue;
        }
        if (ch == ')') {
            if (parenDepth == 0) return i;
            parenDepth--;
        }
    }
    return std::string_view::npos;
}

static bool ParseInlineLinkSyntaxAt(std::string_view source, size_t start, size_t labelOffset,
    MarkdownInlineLink& link) {
    if (start + labelOffset >= source.size()) return false;
    size_t close = source.find("](", start + labelOffset);
    if (close == std::string_view::npos) return false;
    size_t openParen = close + 1;
    size_t end = FindInlineDestinationEnd(source, openParen);
    if (end == std::string_view::npos) return false;

    link.label = source.substr(start + labelOffset, close - (start + labelOffset));
    link.target = source.substr(openParen + 1, end - (openParen + 1));
    link.end = end + 1;
    return true;
}

static bool ExtractMarkdownDestination(std::string_view target, std::string& dest);

std::string StripInlineMarkdown(std::string_view input, bool recognizeMath) {
    if (input.find_first_of("!*_~`$[<\\") == std::string_view::npos &&
        input.find('\xE2') == std::string_view::npos) {
        return ToString(TrimView(input));
    }
    std::vector<InlineSpan> spans = ParseInlineSpans(input, recognizeMath);
    std::string visible;
    visible.reserve(input.size());
    for (const InlineSpan& span : spans) visible += span.text;
    return Trim(std::move(visible));
}

static bool MayContainMath(std::string_view text) {
    return text.find('$') != std::string_view::npos || text.find("\\(") != std::string_view::npos;
}

static void AppendWithoutMathTextBytes(std::string& out, std::string_view text) {
    for (char ch : text) {
        if (ch != kMathTextOpen && ch != kMathTextClose) out.push_back(ch);
    }
}

static void EraseMathTextBytes(std::string& text) {
    if (text.find(kMathTextOpen) == std::string::npos && text.find(kMathTextClose) == std::string::npos) return;
    std::string cleaned;
    cleaned.reserve(text.size());
    AppendWithoutMathTextBytes(cleaned, text);
    text.swap(cleaned);
}

std::string StripInlineMarkdownKeepMath(std::string_view input, bool& hasMath) {
    hasMath = false;
    if (!MayContainMath(input)) return StripInlineMarkdown(input);
    std::vector<InlineSpan> spans = ParseInlineSpans(input);
    for (const InlineSpan& span : spans) hasMath = hasMath || span.math != InlineMath::None;
    std::string visible;
    visible.reserve(input.size() + 8);
    if (!hasMath) {
        for (const InlineSpan& span : spans) visible += span.text;
        return Trim(std::move(visible));
    }
    for (const InlineSpan& span : spans) {
        if (span.math == InlineMath::None) {
            AppendWithoutMathTextBytes(visible, span.text);
            continue;
        }
        visible.push_back(kMathTextOpen);
        AppendWithoutMathTextBytes(visible, span.text);
        visible.push_back(kMathTextClose);
    }
    return Trim(std::move(visible));
}

struct ImageSyntax {
    std::string alt;
    std::string src;
};

static std::string UnescapeMarkdownDestination(std::string_view dest) {
    std::string out;
    out.reserve(dest.size());
    for (size_t i = 0; i < dest.size(); i++) {
        if (dest[i] == '\\' && i + 1 < dest.size() && std::ispunct((unsigned char)dest[i + 1])) {
            out.push_back(dest[i + 1]);
            i++;
            continue;
        }
        out.push_back(dest[i]);
    }
    return out;
}

static bool ExtractMarkdownDestination(std::string_view target, std::string& dest) {
    target = TrimView(target);
    if (target.empty()) return false;

    std::string_view view;
    if (target.front() == '<') {
        size_t end = target.find('>');
        if (end == std::string_view::npos || end == 1) return false;
        view = target.substr(1, end - 1);
    } else {
        size_t end = 0;
        while (end < target.size() && !IsSpace(target[end])) end++;
        view = target.substr(0, end);
    }

    view = TrimView(view);
    dest = UnescapeMarkdownDestination(view);
    return !dest.empty();
}

static bool ParseStandaloneImage(std::string_view line, ImageSyntax* image) {
    std::string_view s = TrimView(line);
    if (s.size() < 5 || s[0] != '!' || s[1] != '[') return false;

    MarkdownInlineLink link;
    if (!ParseInlineLinkSyntaxAt(s, 0, 2, link)) return false;
    if (!TrimView(s.substr(link.end)).empty()) return false;

    std::string src;
    if (!ExtractMarkdownDestination(link.target, src)) return false;

    if (image) {
        image->alt = StripInlineMarkdown(link.label);
        image->src = std::move(src);
    }
    return true;
}

static int LeadingColumns(std::string_view line, size_t* bytes = nullptr) {
    int columns = 0;
    size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ') {
            columns++;
            i++;
        } else if (line[i] == '\t') {
            columns += 4 - (columns % 4);
            i++;
        } else {
            break;
        }
    }
    if (bytes) *bytes = i;
    return columns;
}

static bool RemoveIndentLevel(std::string_view line, std::string_view& content) {
    int columns = 0;
    size_t i = 0;
    while (i < line.size() && columns < 4) {
        if (line[i] == ' ') {
            columns++;
            i++;
        } else if (line[i] == '\t') {
            columns = 4;
            i++;
        } else {
            break;
        }
    }
    if (columns < 4) return false;
    content = line.substr(i);
    return true;
}

static bool ParseSetextUnderline(std::string_view line, int& level) {
    size_t indentBytes = 0;
    if (LeadingColumns(line, &indentBytes) > 3) return false;
    std::string_view marker = TrimView(line.substr(indentBytes));
    if (marker.empty()) return false;
    char ch = marker.front();
    if (ch != '=' && ch != '-') return false;
    for (char value : marker) if (value != ch) return false;
    level = ch == '=' ? 1 : 2;
    return true;
}

static bool HasHardLineBreak(std::string_view line) {
    return line.size() >= 2 && line[line.size() - 1] == ' ' && line[line.size() - 2] == ' ';
}

static std::string_view StripQuoteMarker(std::string_view line) {
    std::string_view value = LTrimView(line);
    if (value.empty() || value.front() != '>') return line;
    value.remove_prefix(1);
    if (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    return value;
}

static std::string_view StripLeadingColumns(std::string_view line, int columnsToRemove) {
    int columns = 0;
    size_t index = 0;
    while (index < line.size() && columns < columnsToRemove) {
        if (line[index] == ' ') {
            columns++;
            index++;
        } else if (line[index] == '\t') {
            columns += 4 - (columns % 4);
            index++;
        } else {
            break;
        }
    }
    return line.substr(index);
}

static bool ParseReferenceTitle(std::string_view value, std::string& title) {
    value = TrimView(value);
    if (value.size() < 2) return false;
    char open = value.front();
    char close = open == '(' ? ')' : open;
    if (open != '\'' && open != '"' && open != '(') return false;
    if (value.back() != close) return false;
    title = ToString(value.substr(1, value.size() - 2));
    return true;
}

static bool ParseReferenceDefinition(const std::vector<std::string_view>& lines, size_t index,
    std::string& label, ReferenceDefinition& definition, size_t& consumed) {
    consumed = 0;
    std::string_view line = lines[index];
    size_t indentBytes = 0;
    if (LeadingColumns(line, &indentBytes) > 3) return false;
    std::string_view value = line.substr(indentBytes);
    if (value.empty() || value.front() != '[') return false;
    size_t close = value.find(']');
    if (close == std::string_view::npos || close == 1 || close + 1 >= value.size() || value[close + 1] != ':') {
        return false;
    }
    label = NormalizeReferenceLabel(value.substr(1, close - 1));
    if (label.empty()) return false;
    std::string_view rest = LTrimView(value.substr(close + 2));
    if (rest.empty()) return false;

    std::string_view destination;
    size_t cursor = 0;
    if (rest.front() == '<') {
        size_t end = rest.find('>');
        if (end == std::string_view::npos || end == 1) return false;
        destination = rest.substr(1, end - 1);
        cursor = end + 1;
    } else {
        while (cursor < rest.size() && !IsSpace(rest[cursor])) cursor++;
        destination = rest.substr(0, cursor);
    }
    if (destination.empty()) return false;
    definition.destination = UnescapeMarkdownDestination(destination);
    definition.title.clear();
    std::string_view trailing = TrimView(rest.substr(cursor));
    if (!trailing.empty() && !ParseReferenceTitle(trailing, definition.title)) return false;
    consumed = 1;
    if (trailing.empty() && index + 1 < lines.size()) {
        std::string nextTitle;
        if (LeadingColumns(lines[index + 1]) >= 1 && ParseReferenceTitle(lines[index + 1], nextTitle)) {
            definition.title = std::move(nextTitle);
            consumed = 2;
        }
    }
    return true;
}

enum class LineKind {
    Plain,
    Empty,
    Fence,
    Math,      // a complete one-line display formula; LineInfo::text is the TeX
    MathOpen,  // "$$" or "\[" that needs a closing line; LineInfo::level is a MathFence
    Rule,
    Heading,
    Bullet,
    Numbered,
    Quote,
    PageBreak,
    Image
};

struct LineInfo {
    std::string_view raw;
    std::string_view trimmed;
    std::string_view text;
    LineKind kind = LineKind::Plain;
    int level = 0;
    int number = 0;
};

static std::string_view ParseFenceMarker(std::string_view trimmed) {
    if (trimmed.empty()) return {};
    char marker = trimmed[0];
    if (marker != '`' && marker != '~') return {};
    size_t count = 0;
    while (count < trimmed.size() && trimmed[count] == marker) count++;
    if (count < 3) return {};
    return trimmed.substr(0, count);
}

static bool IsMatchingClosingFence(const LineInfo& info, std::string_view openingFence) {
    if (info.kind != LineKind::Fence || openingFence.empty() || info.text.empty()) return false;
    if (info.text[0] != openingFence[0] || info.text.size() < openingFence.size()) return false;
    return info.trimmed.size() == info.text.size();
}

enum MathFence : int { kDollarFence = 0, kBracketFence = 1 };

static bool IsEscapedAt(std::string_view text, size_t position) {
    size_t slashes = 0;
    while (position > slashes && text[position - slashes - 1] == '\\') slashes++;
    return (slashes & 1) != 0;
}

// First "$$" in text that is not preceded by an odd number of backslashes.
static size_t FindDisplayClose(std::string_view text) {
    for (size_t position = text.find("$$"); position != std::string_view::npos;
         position = text.find("$$", position + 1)) {
        if (!IsEscapedAt(text, position)) return position;
    }
    return std::string_view::npos;
}

// True when text ends with an unescaped "\]".
static bool EndsWithBracketClose(std::string_view text) {
    return text.size() >= 2 && text[text.size() - 1] == ']' && text[text.size() - 2] == '\\' &&
        !IsEscapedAt(text, text.size() - 2);
}

static bool IsBlankChar(char ch) {
    return ch == ' ' || ch == '\t';
}

// Block-level display math. A line that starts with "$$" or "\[" is one of:
//   Math      "$$ tex $$" or "\[ tex \]" complete on this line;
//   MathOpen  an opening delimiter whose closing line is looked up later;
//   (false)   neither: the line is classified like any other text.
static bool ClassifyMathLine(LineInfo& info) {
    std::string_view trimmed = info.trimmed;
    if (StartsWith(trimmed, "$$")) {
        std::string_view rest = trimmed.substr(2);
        size_t close = FindDisplayClose(rest);
        if (close == std::string_view::npos) {
            info.kind = LineKind::MathOpen;
            info.level = kDollarFence;
            info.text = TrimView(rest);
            return true;
        }
        std::string_view content = TrimView(rest.substr(0, close));
        if (content.empty() || !TrimView(rest.substr(close + 2)).empty()) return false;
        info.kind = LineKind::Math;
        info.text = content;
        return true;
    }
    if (!StartsWith(trimmed, "\\[")) return false;
    if (trimmed.size() == 2) {
        info.kind = LineKind::MathOpen;
        info.level = kBracketFence;
        info.text = {};
        return true;
    }
    // One-line "\[ tex \]" needs the padded form: "\[Illustration\]" and
    // "\[citation needed\]" are ordinary escaped brackets.
    if (trimmed.size() < 7 || !EndsWithBracketClose(trimmed) || !IsBlankChar(trimmed[2]) ||
        !IsBlankChar(trimmed[trimmed.size() - 3])) {
        return false;
    }
    std::string_view content = TrimView(trimmed.substr(2, trimmed.size() - 4));
    if (content.empty()) return false;
    info.kind = LineKind::Math;
    info.text = content;
    return true;
}

static LineInfo ClassifyLine(std::string_view line) {
    LineInfo info;
    info.raw = line;
    info.trimmed = TrimView(line);
    if (info.trimmed.empty()) {
        info.kind = LineKind::Empty;
        return info;
    }
    std::string_view fence = ParseFenceMarker(info.trimmed);
    if (!fence.empty()) {
        info.kind = LineKind::Fence;
        info.text = fence;
        return info;
    }
    if (info.trimmed[0] == '$' || info.trimmed[0] == '\\') {
        if (ClassifyMathLine(info)) return info;
    }
    if (IsPageBreakLine(info.trimmed)) {
        info.kind = LineKind::PageBreak;
        return info;
    }
    if (ParseStandaloneImage(info.trimmed, nullptr)) {
        info.kind = LineKind::Image;
        return info;
    }
    if (IsRuleLine(line)) {
        info.kind = LineKind::Rule;
        return info;
    }
    if (ParseHeadingView(line, info.level, info.text)) {
        info.kind = LineKind::Heading;
        return info;
    }
    if (ParseBulletView(line, info.level, info.text)) {
        info.kind = LineKind::Bullet;
        return info;
    }
    if (ParseNumberedView(line, info.level, info.number, info.text)) {
        info.kind = LineKind::Numbered;
        return info;
    }
    std::string_view left = LTrimView(line);
    if (StartsWith(left, ">")) {
        info.kind = LineKind::Quote;
        info.text = left.substr(1);
        if (!info.text.empty() && info.text[0] == ' ') info.text.remove_prefix(1);
        info.text = TrimView(info.text);
        return info;
    }
    return info;
}

static bool IsBlockStart(const LineInfo& info) {
    return info.kind != LineKind::Plain;
}

// Looks for the line that closes the MathOpen line at `open`. The search ends at
// the first line that holds the closing delimiter, a blank line, a code fence, or
// another opener of the same kind, so every line is visited at most twice per
// document no matter how many unterminated openers it contains. On success
// `lastContent` receives the text that precedes the delimiter on the closing line.
static size_t FindMathBlockClose(const std::vector<LineInfo>& infos, size_t open, std::string_view& lastContent) {
    const bool bracket = infos[open].level == kBracketFence;
    bool hasContent = !infos[open].text.empty();
    for (size_t index = open + 1; index < infos.size(); index++) {
        const LineInfo& info = infos[index];
        if (info.kind == LineKind::Empty || info.kind == LineKind::Fence) break;
        std::string_view trimmed = info.trimmed;
        if (bracket) {
            if (trimmed == "\\[") break;
            if (EndsWithBracketClose(trimmed)) {
                lastContent = TrimView(trimmed.substr(0, trimmed.size() - 2));
                return hasContent || !lastContent.empty() ? index : std::string_view::npos;
            }
        } else {
            size_t close = FindDisplayClose(trimmed);
            if (close != std::string_view::npos) {
                if (!TrimView(trimmed.substr(close + 2)).empty()) break;
                lastContent = TrimView(trimmed.substr(0, close));
                return hasContent || !lastContent.empty() ? index : std::string_view::npos;
            }
        }
        hasContent = true;
    }
    return std::string_view::npos;
}

// Turns every MathOpen line that has no closing line back into ordinary text, so
// an unterminated "$$" can neither swallow the rest of the document nor interrupt
// the paragraph it belongs to.
static void ResolveMathOpeners(const std::vector<std::string_view>& lines, std::vector<LineInfo>& infos) {
    std::string_view activeFence;
    for (size_t index = 0; index < infos.size(); index++) {
        LineInfo& info = infos[index];
        if (info.kind == LineKind::Fence) {
            if (activeFence.empty()) activeFence = info.text;
            else if (IsMatchingClosingFence(info, activeFence)) activeFence = {};
            continue;
        }
        if (!activeFence.empty() || info.kind != LineKind::MathOpen) continue;
        std::string_view ignored;
        if (RemoveIndentLevel(lines[index], ignored)) continue;
        size_t close = FindMathBlockClose(infos, index, ignored);
        if (close == std::string_view::npos) {
            info.kind = LineKind::Plain;
            info.level = 0;
            info.text = {};
        } else {
            index = close;
        }
    }
}

static bool IsMathFenceInfo(std::string_view infoString) {
    return EqualsAsciiInsensitive(TrimView(infoString), "math");
}

// TeX source of a display block: trimmed lines [first, end) joined by '\n', with
// leading and trailing blank lines dropped.
static std::string JoinTrimmedLines(const std::vector<LineInfo>& infos, size_t first, size_t end) {
    while (first < end && infos[first].trimmed.empty()) first++;
    while (end > first && infos[end - 1].trimmed.empty()) end--;
    std::string text;
    for (size_t index = first; index < end; index++) {
        if (index > first) text.push_back('\n');
        text.append(infos[index].trimmed.data(), infos[index].trimmed.size());
    }
    return text;
}

struct TableMathCell {
    size_t row = 0;
    size_t column = 0;
    std::string text;
};

// Turns the cells of one table row (as split by SplitTableRow) into visible text.
// A row without '$' or "\(" takes the path it always took. Any other row is read
// again with its backslashes intact, so "\alpha" reaches the inline parser and
// "\$" stays an escaped dollar; its formula cells are parked in mathCells until
// the whole table is known.
static void StripTableRow(std::string_view line, size_t rowIndex, std::vector<std::string>& row,
    std::vector<TableMathCell>& mathCells) {
    if (!MayContainMath(line)) {
        for (auto& cell : row) cell = StripInlineMarkdown(cell);
        return;
    }
    std::vector<std::string> raw = SplitTableRowRaw(line);
    for (size_t column = 0; column < row.size(); column++) {
        bool hasMath = false;
        std::string text = column < raw.size() ? StripInlineMarkdownKeepMath(raw[column], hasMath) : std::string();
        if (hasMath) {
            mathCells.push_back({ rowIndex, column, std::move(text) });
            row[column].clear();
        } else {
            row[column] = std::move(text);
        }
    }
}

static std::vector<Block> ParseMarkdownImpl(const std::string& markdown, int depth) {
    std::vector<std::string_view> lines = SplitLineViews(markdown);
    std::vector<LineInfo> infos;
    infos.reserve(lines.size());
    bool hasMathOpeners = false;
    for (std::string_view line : lines) {
        infos.push_back(ClassifyLine(line));
        hasMathOpeners = hasMathOpeners || infos.back().kind == LineKind::MathOpen;
    }
    if (hasMathOpeners) ResolveMathOpeners(lines, infos);

    ReferenceDefinitions definitions;
    std::vector<unsigned char> suppressed;
    if (markdown.find("]:") != std::string::npos) {
        suppressed.assign(lines.size(), 0);
        std::string_view activeFence;
        for (size_t index = 0; index < lines.size();) {
            if (infos[index].kind == LineKind::Fence) {
                if (activeFence.empty()) activeFence = infos[index].text;
                else if (IsMatchingClosingFence(infos[index], activeFence)) activeFence = {};
                index++;
                continue;
            }
            if (!activeFence.empty()) {
                index++;
                continue;
            }
            std::string_view ignoredIndented;
            if (RemoveIndentLevel(lines[index], ignoredIndented)) {
                index++;
                continue;
            }
            std::string label;
            ReferenceDefinition definition;
            size_t consumed = 0;
            if (ParseReferenceDefinition(lines, index, label, definition, consumed)) {
                definitions.emplace(std::move(label), std::move(definition));
                for (size_t offset = 0; offset < consumed; offset++) suppressed[index + offset] = 1;
                index += consumed;
                continue;
            }
            index++;
        }
    }
    auto isSuppressed = [&](size_t index) {
        return !suppressed.empty() && suppressed[index] != 0;
    };

    std::vector<Block> blocks;
    blocks.reserve(std::max<size_t>(8, lines.size() / 2));
    size_t i = 0;

    if (!infos.empty() && infos[0].trimmed == "---") {
        size_t frontMatterEnd = 1;
        while (frontMatterEnd < infos.size() &&
            infos[frontMatterEnd].trimmed != "---" &&
            infos[frontMatterEnd].trimmed != "...") {
            frontMatterEnd++;
        }
        if (frontMatterEnd < infos.size()) i = frontMatterEnd + 1;
    }

    while (i < lines.size()) {
        if (isSuppressed(i)) {
            i++;
            continue;
        }
        const LineInfo& info = infos[i];
        std::string_view line = info.raw;
        std::string_view trimmed = info.trimmed;
        if (info.kind == LineKind::Empty) {
            i++;
            continue;
        }

        int setextLevel = 0;
        if (info.kind == LineKind::Plain && i + 1 < lines.size() && !isSuppressed(i + 1) &&
            ParseSetextUnderline(lines[i + 1], setextLevel)) {
            bool headingHasMath = false;
            std::string heading = definitions.empty() || trimmed.find('[') == std::string_view::npos ?
                StripInlineMarkdownKeepMath(trimmed, headingHasMath) :
                StripInlineMarkdownKeepMath(ResolveReferenceLinks(trimmed, definitions), headingHasMath);
            blocks.push_back({ BlockType::Heading, setextLevel, 0, std::move(heading) });
            blocks.back().hasMath = headingHasMath;
            i += 2;
            continue;
        }

        std::string_view indentedContent;
        if (RemoveIndentLevel(line, indentedContent)) {
            std::vector<std::string_view> codeLines;
            while (i < lines.size()) {
                std::string_view content;
                if (RemoveIndentLevel(lines[i], content)) {
                    codeLines.push_back(content);
                    i++;
                    continue;
                }
                if (infos[i].kind == LineKind::Empty) {
                    codeLines.push_back({});
                    i++;
                    continue;
                }
                break;
            }
            while (!codeLines.empty() && codeLines.back().empty()) codeLines.pop_back();
            std::string text;
            for (size_t codeIndex = 0; codeIndex < codeLines.size(); codeIndex++) {
                if (codeIndex) text.push_back('\n');
                text.append(codeLines[codeIndex]);
            }
            blocks.push_back({ BlockType::Code, 0, 0, std::move(text) });
            continue;
        }

        if (!definitions.empty() && line.find("![") != std::string_view::npos) {
            std::string resolvedLine = ResolveReferenceLinks(line, definitions);
            ImageSyntax resolvedImage;
            if (ParseStandaloneImage(resolvedLine, &resolvedImage)) {
                Block block;
                block.type = BlockType::Image;
                block.text = std::move(resolvedImage.alt);
                block.imageSrc = std::move(resolvedImage.src);
                blocks.push_back(std::move(block));
                i++;
                continue;
            }
        }

        if (info.kind == LineKind::Fence) {
            std::string_view fence = info.text;
            const bool mathFence = IsMathFenceInfo(trimmed.substr(fence.size()));
            const size_t firstContent = i + 1;
            std::string text;
            i++;
            while (i < lines.size() && !IsMatchingClosingFence(infos[i], fence)) {
                text.append(lines[i].data(), lines[i].size());
                if (i + 1 < lines.size()) text += "\n";
                i++;
            }
            const size_t contentEnd = i;
            const bool closed = i < lines.size();
            if (i < lines.size()) i++;
            if (mathFence && closed) {
                // ```math ... ``` is display math. An unterminated or empty math
                // fence stays the code block it has always been.
                std::string tex = JoinTrimmedLines(infos, firstContent, contentEnd);
                if (!tex.empty()) {
                    blocks.push_back({ BlockType::MathBlock, 0, 0, std::move(tex) });
                    continue;
                }
            }
            blocks.push_back({ BlockType::Code, 0, 0, text });
            continue;
        }

        if (info.kind == LineKind::Math) {
            blocks.push_back({ BlockType::MathBlock, 0, 0, ToString(info.text) });
            i++;
            continue;
        }

        if (info.kind == LineKind::MathOpen) {
            std::string_view lastContent;
            size_t close = FindMathBlockClose(infos, i, lastContent);
            if (close != std::string_view::npos) {
                std::string text = ToString(info.text);
                std::string body = JoinTrimmedLines(infos, i + 1, close);
                if (!body.empty()) {
                    if (!text.empty()) text += "\n";
                    text += body;
                }
                if (!lastContent.empty()) {
                    if (!text.empty()) text += "\n";
                    text.append(lastContent.data(), lastContent.size());
                }
                blocks.push_back({ BlockType::MathBlock, 0, 0, std::move(text) });
                i = close + 1;
                continue;
            }
            // Only an opener that ResolveMathOpeners skipped can get here without a
            // closing line (it follows a fence marker that turned out to be indented
            // code). Keep the line as paragraph text.
            infos[i].kind = LineKind::Plain;
        }

        if (IsTableStart(lines, i)) {
            std::vector<std::vector<std::string>> rows;
            std::vector<int> aligns;
            std::vector<TableMathCell> mathCells;
            const size_t headerLine = i;
            rows.push_back(SplitTableRow(lines[i]));
            ParseTableSeparator(lines[i + 1], aligns);
            i += 2;

            while (i < lines.size() && infos[i].kind != LineKind::Empty) {
                std::vector<std::string> row = SplitTableRow(lines[i]);
                if (row.size() < 2) break;
                StripTableRow(lines[i], rows.size(), row, mathCells);
                rows.push_back(row);
                i++;
            }

            StripTableRow(lines[headerLine], 0, rows[0], mathCells);
            Block table;
            table.type = BlockType::Table;
            table.rows = std::move(rows);
            table.aligns = std::move(aligns);
            if (!mathCells.empty()) {
                // Literal control bytes leave every cell first, so the only
                // kMathText* bytes in this table are the ones written below.
                for (auto& row : table.rows) for (auto& cell : row) EraseMathTextBytes(cell);
                for (TableMathCell& cell : mathCells) table.rows[cell.row][cell.column] = std::move(cell.text);
                table.hasMath = true;
            }
            blocks.push_back(std::move(table));
            continue;
        }

        if (info.kind == LineKind::Heading) {
            bool headingHasMath = false;
            std::string heading = definitions.empty() || info.text.find('[') == std::string_view::npos ?
                StripInlineMarkdownKeepMath(info.text, headingHasMath) :
                StripInlineMarkdownKeepMath(ResolveReferenceLinks(info.text, definitions), headingHasMath);
            blocks.push_back({ BlockType::Heading, info.level, 0, std::move(heading) });
            blocks.back().hasMath = headingHasMath;
            i++;
            continue;
        }

        if (info.kind == LineKind::Rule) {
            blocks.push_back({ BlockType::Rule, 0, 0, "" });
            i++;
            continue;
        }

        if (info.kind == LineKind::PageBreak) {
            blocks.push_back({ BlockType::PageBreak, 0, 0, "" });
            i++;
            continue;
        }

        if (info.kind == LineKind::Image) {
            ImageSyntax image;
            ParseStandaloneImage(line, &image);
            Block block;
            block.type = BlockType::Image;
            block.text = std::move(image.alt);
            block.imageSrc = std::move(image.src);
            blocks.push_back(std::move(block));
            i++;
            continue;
        }

        if (info.kind == LineKind::Bullet || info.kind == LineKind::Numbered) {
            Block item;
            item.type = info.kind == LineKind::Bullet ? BlockType::Bullet : BlockType::Numbered;
            item.level = info.level;
            item.number = info.number;
            int baseIndent = LeadingColumns(line);
            std::string itemMarkdown(info.text);
            bool sawBlank = false;
            i++;
            while (i < lines.size()) {
                if (isSuppressed(i)) {
                    i++;
                    continue;
                }
                if (infos[i].kind == LineKind::Empty) {
                    itemMarkdown += "\n\n";
                    sawBlank = true;
                    i++;
                    continue;
                }
                if ((infos[i].kind == LineKind::Bullet || infos[i].kind == LineKind::Numbered) &&
                    infos[i].level <= info.level) break;
                int continuationIndent = LeadingColumns(lines[i]);
                bool indented = continuationIndent > baseIndent;
                if (!indented && (sawBlank || IsBlockStart(infos[i]))) break;
                std::string_view continuation = indented ?
                    StripLeadingColumns(lines[i], baseIndent + 2) : infos[i].trimmed;
                if (!itemMarkdown.empty() && itemMarkdown.back() != '\n') itemMarkdown.push_back('\n');
                itemMarkdown.append(continuation);
                sawBlank = false;
                i++;
            }
            if (!definitions.empty() && itemMarkdown.find('[') != std::string::npos) itemMarkdown = ResolveReferenceLinks(itemMarkdown, definitions);
            if (depth < 8) {
                item.children = ParseMarkdownImpl(itemMarkdown, depth + 1);
            } else {
                item.text = StripInlineMarkdown(itemMarkdown);
            }
            if (!item.children.empty() && item.children.front().type == BlockType::Paragraph) {
                item.text = std::move(item.children.front().text);
                item.children.erase(item.children.begin());
            }
            blocks.push_back(std::move(item));
            continue;
        }

        if (info.kind == LineKind::Quote) {
            std::string quoteMarkdown;
            bool allowLazyContinuation = true;
            while (i < lines.size()) {
                if (infos[i].kind == LineKind::Quote) {
                    if (!quoteMarkdown.empty()) quoteMarkdown.push_back('\n');
                    std::string_view quoted = StripQuoteMarker(lines[i]);
                    quoteMarkdown.append(quoted);
                    allowLazyContinuation = !TrimView(quoted).empty();
                    i++;
                    continue;
                }
                if (allowLazyContinuation && infos[i].kind == LineKind::Plain) {
                    quoteMarkdown.push_back('\n');
                    quoteMarkdown.append(lines[i]);
                    i++;
                    continue;
                }
                break;
            }
            if (!definitions.empty() && quoteMarkdown.find('[') != std::string::npos) quoteMarkdown = ResolveReferenceLinks(quoteMarkdown, definitions);
            Block quote;
            quote.type = BlockType::Quote;
            if (depth < 8) {
                quote.children = ParseMarkdownImpl(quoteMarkdown, depth + 1);
            } else {
                quote.text = StripInlineMarkdown(quoteMarkdown);
            }
            if (quote.children.empty()) quote.text = StripInlineMarkdown(quoteMarkdown);
            blocks.push_back(std::move(quote));
            continue;
        }

        std::string paragraph;
        bool previousHardBreak = false;
        while (i < lines.size() && !isSuppressed(i) && !IsBlockStart(infos[i]) && !IsTableStart(lines, i)) {
            if (!paragraph.empty()) paragraph += previousHardBreak ? "\n" : " ";
            std::string_view v = HasHardLineBreak(lines[i]) ? RTrimView(lines[i]) : infos[i].trimmed;
            paragraph.append(v.data(), v.size());
            previousHardBreak = HasHardLineBreak(lines[i]);
            i++;
        }
        if (paragraph.empty()) {
            paragraph = ToString(trimmed);
            i++;
        }
        if (!definitions.empty() && paragraph.find('[') != std::string::npos) paragraph = ResolveReferenceLinks(paragraph, definitions);
        blocks.push_back({ BlockType::Paragraph, 0, 0, std::move(paragraph) });
    }

    return blocks;
}
std::vector<Block> ParseMarkdown(const std::string& markdown) {
    return ParseMarkdownImpl(markdown, 0);
}



} // namespace TinyPdf::Internal
