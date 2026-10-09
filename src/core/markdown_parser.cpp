#include "markdown_parser.h"
#include "inline_markdown.h"
#include "../common/text_utils.h"

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

#if defined(__GNUC__) || defined(__clang__)
#define RAYOMD_NOINLINE __attribute__((noinline))
#else
#define RAYOMD_NOINLINE
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

// The text between the outer pipes of a table row.
static std::string_view TableRowBody(std::string_view line) {
    std::string_view s = TrimView(line);
    if (!s.empty() && s.front() == '|') s.remove_prefix(1);
    if (!s.empty() && s.back() == '|') s.remove_suffix(1);
    return s;
}

// Cells of a row body that holds no backslash: every pipe separates two cells, so
// each cell is a trimmed piece of the line and fn(cell) can read it in place.
template <typename Fn>
static void ForEachPlainTableCell(std::string_view body, Fn fn) {
    for (;;) {
        size_t bar = body.find('|');
        if (bar == std::string_view::npos) {
            fn(TrimView(body));
            return;
        }
        fn(TrimView(body.substr(0, bar)));
        body.remove_prefix(bar + 1);
    }
}

// Number of cells SplitTableRow returns for this line.
static size_t CountTableCells(std::string_view line) {
    std::string_view s = TableRowBody(line);
    size_t cells = 1;
    bool escaped = false;
    for (char c : s) {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == '|') cells++;
    }
    return cells;
}

static std::vector<std::string> SplitTableRow(std::string_view line) {
    std::string_view s = TableRowBody(line);

    std::vector<std::string> cells;
    cells.reserve(4);
    if (s.find('\\') == std::string_view::npos) {
        ForEachPlainTableCell(s, [&](std::string_view cell) { cells.emplace_back(cell.data(), cell.size()); });
        return cells;
    }
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
// that escapes a pipe, so "\*" and "\alpha" reach the inline parser as written.
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
    std::vector<int> parsed;
    if (line.find('\\') == std::string_view::npos) {
        bool valid = true;
        ForEachPlainTableCell(TableRowBody(line), [&](std::string_view cell) {
            int align = -1;
            valid = valid && IsTableSeparatorCell(cell, align);
            if (valid) parsed.push_back(align);
        });
        if (!valid) return false;
    } else {
        std::vector<std::string> cells = SplitTableRow(line);
        if (cells.empty()) return false;
        for (const auto& cell : cells) {
            int align = -1;
            if (!IsTableSeparatorCell(cell, align)) return false;
            parsed.push_back(align);
        }
    }

    aligns = std::move(parsed);
    return true;
}

// True when lines[i] is a table header followed by its separator line. `aligns`, when
// given, receives the column alignments of a table that starts here.
static bool IsTableStart(const std::vector<std::string_view>& lines, size_t i, std::vector<int>* aligns = nullptr) {
    if (i + 1 >= lines.size()) return false;
    if (lines[i].find('|') == std::string_view::npos ||
        lines[i + 1].find('|') == std::string_view::npos ||
        lines[i + 1].find('-') == std::string_view::npos) {
        return false;
    }
    if (CountTableCells(lines[i]) < 2) return false;
    std::vector<int> parsed;
    if (!ParseTableSeparator(lines[i + 1], parsed)) return false;
    if (aligns) *aligns = std::move(parsed);
    return true;
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
    if (!RayoMd::Text::ContainsByteClass(input, RayoMd::Text::kByteInlineSyntax | RayoMd::Text::kByteSymbolLead)) {
        return ToString(TrimView(input));
    }
    // The parsed runs cover the text they were written to without a gap: it is the visible text.
    InlineRuns runs;
    ParseInlineRuns(input, runs, recognizeMath);
    return Trim(std::move(runs.text));
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
    InlineRuns runs;
    ParseInlineRuns(input, runs);
    for (const InlineRun& run : runs.runs) hasMath = hasMath || run.math != InlineMath::None;
    if (!hasMath) return Trim(std::move(runs.text));
    std::string visible;
    visible.reserve(input.size() + 8);
    for (const InlineRun& run : runs.runs) {
        if (run.math == InlineMath::None) {
            AppendWithoutMathTextBytes(visible, runs.Text(run));
            continue;
        }
        visible.push_back(kMathTextOpen);
        AppendWithoutMathTextBytes(visible, runs.Text(run));
        visible.push_back(kMathTextClose);
    }
    return Trim(std::move(visible));
}

struct ImageSyntax {
    std::string alt;
    ImageSource source;
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

// A line that is one image, "![alt](src)", or one linked image, "[![alt](src)](target)".
static bool ParseStandaloneImage(std::string_view line, ImageSyntax* image) {
    std::string_view s = TrimView(line);
    const size_t start = s.size() >= 3 && s[0] == '[' && s[1] == '!' && s[2] == '[' ? 1 : 0;
    if (s.size() < start + 5 || s[start] != '!' || s[start + 1] != '[') return false;

    MarkdownInlineLink link;
    if (!ParseInlineLinkSyntaxAt(s, start, 2, link)) return false;
    size_t end = link.end;
    std::string target;
    if (start == 1) {
        if (end + 1 >= s.size() || s[end] != ']' || s[end + 1] != '(') return false;
        const size_t targetEnd = FindInlineDestinationEnd(s, end + 1);
        if (targetEnd == std::string_view::npos ||
            !ExtractMarkdownDestination(s.substr(end + 2, targetEnd - (end + 2)), target)) {
            return false;
        }
        end = targetEnd + 1;
    }
    if (!TrimView(s.substr(end)).empty()) return false;

    std::string src;
    if (!ExtractMarkdownDestination(link.target, src)) return false;

    if (image) {
        image->alt = StripInlineMarkdown(link.label);
        image->source.src = std::move(src);
        image->source.link = std::move(target);
    }
    return true;
}

static Block ImageBlock(ImageSyntax&& image) {
    Block block;
    block.type = BlockType::Image;
    block.text = std::move(image.alt);
    block.image = std::make_unique<ImageSource>(std::move(image.source));
    return block;
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

// Column at which the content of the list item on `line` starts (CommonMark): the marker's
// column, the marker, and the one to four spaces after it. With no content or five and more
// spaces, the content starts one column after the marker. Lines indented this far belong to
// the item, so "- a\n  - b" and "1. a\n   1. b" nest like four-space indentation does.
static int ListItemContentColumn(std::string_view line, bool bullet) {
    size_t at = 0;
    int column = LeadingColumns(line, &at);
    if (!bullet) {
        while (at < line.size() && line[at] >= '0' && line[at] <= '9') {
            at++;
            column++;
        }
    }
    at++;           // the bullet, or the '.' or ')' after the digits
    column++;
    const int markerEnd = column;
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
        column += line[at] == '\t' ? 4 - column % 4 : 1;
        at++;
    }
    return at >= line.size() || column - markerEnd > 4 ? markerEnd + 1 : column;
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

// The GitHub alert that the first line of a top-level quote names alone: 1 to 5 for "[!NOTE]",
// "[!TIP]", "[!IMPORTANT]", "[!WARNING]" and "[!CAUTION]", in any case, with the line taken
// out of `quote`. 0 for any other quote, and for a marker with no text after it, which stays a
// plain quote as on GitHub. Out of line: inlined, it grew ParseMarkdownImpl past what GCC
// inlines into it, and a string constructor there became a call.
static RAYOMD_NOINLINE uint8_t TakeAlertMarker(std::string& quote) {
    const std::string_view text = quote;
    const std::string_view head = LTrimView(text);
    if (head.size() < 5 || head[0] != '[' || head[1] != '!') return 0;
    const size_t lineEnd = text.find('\n');
    if (lineEnd == std::string_view::npos) return 0;
    const std::string_view line = TrimView(text.substr(0, lineEnd));
    if (line.size() < 5 || line.back() != ']' || TrimView(text.substr(lineEnd + 1)).empty()) return 0;
    static constexpr std::string_view kAlerts[] = { "NOTE", "TIP", "IMPORTANT", "WARNING", "CAUTION" };
    const std::string_view name = line.substr(2, line.size() - 3);
    for (size_t kind = 0; kind < 5; kind++) {
        if (!EqualsAsciiInsensitive(name, kAlerts[kind])) continue;
        quote.erase(0, lineEnd + 1);
        return (uint8_t)(kind + 1);
    }
    return 0;
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
    // "[^label]:" starts a footnote definition (GitHub Flavored Markdown). Footnotes are not
    // rendered, so the line must stay visible text instead of vanishing as a link definition.
    if (value.size() > 1 && value[1] == '^') return false;
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
    Image,
    HtmlComment     // starts with "<!--": not shown, up to the line with "-->"
};

struct LineInfo {
    std::string_view trimmed;
    std::string_view text;
    LineKind kind = LineKind::Plain;
    int level = 0;
    int number = 0;
};

// Every block marker starts with one of these characters (after the indent). A line
// that begins with anything else is paragraph text and needs none of the checks below.
static bool MayStartBlock(char first) {
    switch (first) {
    case '`': case '~':             // code fence
    case '$': case '\\':            // display math, \pagebreak
    case '<':                       // page-break comment, HTML comment
    case '!':                       // standalone image
    case '-': case '*': case '_':   // rule, bullet
    case '+':                       // bullet
    case '#':                       // heading
    case '>':                       // quote
        return true;
    default:
        return std::isdigit((unsigned char)first) != 0;   // numbered item
    }
}

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
    info.trimmed = TrimView(line);
    if (info.trimmed.empty()) {
        info.kind = LineKind::Empty;
        return info;
    }
    // A line that starts with '[' starts a block only as a linked image, "[![".
    if (!MayStartBlock(info.trimmed[0]) && (info.trimmed[0] != '[' || info.trimmed.size() < 3 ||
        info.trimmed[1] != '!' || info.trimmed[2] != '[')) {
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
    if (info.trimmed.size() >= 4 && info.trimmed[0] == '<' && info.trimmed[1] == '!' && info.trimmed[2] == '-' &&
        info.trimmed[3] == '-') {
        info.kind = LineKind::HtmlComment;
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

// True when ParseMarkdownImpl would turn `text` into exactly one paragraph whose text is
// `text` itself: a single trimmed line that starts no block and cannot be a reference
// definition. List items and quotes are mostly such lines and skip the nested parse.
static bool IsSingleParagraphLine(std::string_view text) {
    if (text.empty() || IsSpace(text.front()) || IsSpace(text.back())) return false;
    if (text.find('\n') != std::string_view::npos || text.find("]:") != std::string_view::npos) return false;
    if (StartsWith(text, std::string_view("\xEF\xBB\xBF", 3))) return false;
    return ClassifyLine(text).kind == LineKind::Plain;
}

static Block& AppendBlock(std::vector<Block>& blocks, BlockType type) {
    blocks.emplace_back();
    blocks.back().type = type;
    return blocks.back();
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

// The cells of a table row as inline Markdown: split at every pipe that is not escaped,
// with the backslash of an escaped pipe removed, as GFM does also inside code spans.
// Every other backslash stays for the inline parser.
static std::vector<std::string> SplitTableCells(std::string_view line) {
    return line.find('\\') == std::string_view::npos ? SplitTableRow(line) : SplitTableRowRaw(line);
}

// Parks the formula cells of a row (as split by SplitTableCells) in mathCells, as visible
// text with their formulas, and empties them. Only a row with '$' or "\(" can hold one.
static void FindTableMath(std::string_view line, size_t rowIndex, std::vector<std::string>& row,
    std::vector<TableMathCell>& mathCells) {
    if (!MayContainMath(line)) return;
    for (size_t column = 0; column < row.size(); column++) {
        bool hasMath = false;
        std::string text = StripInlineMarkdownKeepMath(row[column], hasMath);
        if (!hasMath) continue;
        mathCells.push_back({ rowIndex, column, std::move(text) });
        row[column].clear();
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
    blocks.reserve(std::min(lines.size(), std::max<size_t>(8, lines.size() / 2)));
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
        std::string_view line = lines[i];
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
            Block& block = AppendBlock(blocks, BlockType::Heading);
            block.level = setextLevel;
            block.text = std::move(heading);
            block.hasMath = headingHasMath;
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
            AppendBlock(blocks, BlockType::Code).text = std::move(text);
            continue;
        }

        if (!definitions.empty() && line.find("![") != std::string_view::npos) {
            std::string resolvedLine = ResolveReferenceLinks(line, definitions);
            ImageSyntax resolvedImage;
            if (ParseStandaloneImage(resolvedLine, &resolvedImage)) {
                blocks.push_back(ImageBlock(std::move(resolvedImage)));
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
                    AppendBlock(blocks, BlockType::MathBlock).text = std::move(tex);
                    continue;
                }
            }
            AppendBlock(blocks, BlockType::Code).text = std::move(text);
            continue;
        }

        if (info.kind == LineKind::Math) {
            AppendBlock(blocks, BlockType::MathBlock).text = ToString(info.text);
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
                AppendBlock(blocks, BlockType::MathBlock).text = std::move(text);
                i = close + 1;
                continue;
            }
            // Only an opener that ResolveMathOpeners skipped can get here without a
            // closing line (it follows a fence marker that turned out to be indented
            // code). Keep the line as paragraph text.
            infos[i].kind = LineKind::Plain;
        }

        std::vector<int> aligns;
        if (IsTableStart(lines, i, &aligns)) {
            std::vector<std::vector<std::string>> rows;
            std::vector<TableMathCell> mathCells;
            rows.reserve(8);
            // Reference links resolve cell by cell, so a link target cannot split a row.
            auto addRow = [&](std::string_view line, std::vector<std::string> row) {
                if (!definitions.empty()) {
                    for (std::string& cell : row) {
                        if (cell.find('[') != std::string::npos) cell = ResolveReferenceLinks(cell, definitions);
                    }
                }
                FindTableMath(line, rows.size(), row, mathCells);
                rows.push_back(std::move(row));
            };
            addRow(lines[i], SplitTableCells(lines[i]));
            const size_t columns = rows[0].size();
            i += 2;

            // As in GFM, the rows go on up to a blank line or the start of another block, and
            // every row has the cells of the header: missing ones are empty, extra ones dropped.
            while (i < lines.size() && infos[i].kind == LineKind::Plain && !isSuppressed(i)) {
                std::vector<std::string> row = SplitTableCells(lines[i]);
                row.resize(columns);
                addRow(lines[i], std::move(row));
                i++;
            }

            Block table;
            table.type = BlockType::Table;
            table.rows = std::move(rows);
            table.aligns = std::move(aligns);
            if (!mathCells.empty()) {
                // A table with formulas shows its other cells as plain text. Literal control
                // bytes leave every cell first, so the only kMathText* bytes in this table
                // are the ones written below.
                constexpr unsigned char kSyntax = RayoMd::Text::kByteInlineSyntax | RayoMd::Text::kByteSymbolLead;
                for (auto& row : table.rows) {
                    for (auto& cell : row) {
                        if (RayoMd::Text::ContainsByteClass(cell, kSyntax)) cell = StripInlineMarkdown(cell);
                        EraseMathTextBytes(cell);
                    }
                }
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
            Block& block = AppendBlock(blocks, BlockType::Heading);
            block.level = info.level;
            block.text = std::move(heading);
            block.hasMath = headingHasMath;
            i++;
            continue;
        }

        if (info.kind == LineKind::Rule) {
            AppendBlock(blocks, BlockType::Rule);
            i++;
            continue;
        }

        if (info.kind == LineKind::PageBreak) {
            AppendBlock(blocks, BlockType::PageBreak);
            i++;
            continue;
        }

        if (info.kind == LineKind::HtmlComment) {
            // Not shown: everything up to the "-->" that closes it, or to the end of the
            // document when none does, as in CommonMark. Text after "-->" on that line is.
            size_t end = i;
            size_t close = std::string_view::npos;
            for (; end < lines.size(); end++) {
                const std::string_view text = lines[end];
                close = text.find("-->", end == i ? text.find("<!--") + 2 : 0);
                if (close != std::string_view::npos) break;
            }
            if (end < lines.size()) {
                const std::string_view rest = TrimView(lines[end].substr(close + 3));
                if (!rest.empty()) AppendBlock(blocks, BlockType::Paragraph).text = ToString(rest);
            }
            i = end + 1;
            continue;
        }

        if (info.kind == LineKind::Image) {
            ImageSyntax image;
            ParseStandaloneImage(line, &image);
            blocks.push_back(ImageBlock(std::move(image)));
            i++;
            continue;
        }

        if (info.kind == LineKind::Bullet || info.kind == LineKind::Numbered) {
            // Built in place: nothing else is appended to `blocks` while `item` is in use.
            Block& item = AppendBlock(blocks, info.kind == LineKind::Bullet ? BlockType::Bullet : BlockType::Numbered);
            item.level = info.level;
            item.number = info.number;
            int baseIndent = LeadingColumns(line);
            const int contentColumn = ListItemContentColumn(line, info.kind == LineKind::Bullet);
            // "[ ]", "[x]" or "[X]" and white space before the text make a task list item.
            std::string_view first = info.text;
            if (first.size() >= 4 && first[0] == '[' && first[2] == ']' && (first[3] == ' ' || first[3] == '\t') &&
                (first[1] == ' ' || first[1] == 'x' || first[1] == 'X')) {
                item.task = first[1] == ' ' ? 1 : 2;
                first = LTrimView(first.substr(4));
            }
            std::string itemMarkdown(first);
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
                const int continuationIndent = LeadingColumns(lines[i]);
                // A list item that does not reach this item's content column is a sibling or
                // belongs to an outer list; one that does is nested in this item.
                if ((infos[i].kind == LineKind::Bullet || infos[i].kind == LineKind::Numbered) &&
                    continuationIndent < contentColumn) break;
                bool indented = continuationIndent > baseIndent;
                if (!indented && (sawBlank || IsBlockStart(infos[i]))) break;
                std::string_view continuation = indented ?
                    StripLeadingColumns(lines[i], contentColumn) : infos[i].trimmed;
                if (!itemMarkdown.empty() && itemMarkdown.back() != '\n') itemMarkdown.push_back('\n');
                itemMarkdown.append(continuation);
                sawBlank = false;
                i++;
            }
            if (!definitions.empty() && itemMarkdown.find('[') != std::string::npos) itemMarkdown = ResolveReferenceLinks(itemMarkdown, definitions);
            if (depth < 8) {
                // Blank lines after the item only add line feeds, which the nested parse skips.
                std::string_view single = itemMarkdown;
                while (!single.empty() && single.back() == '\n') single.remove_suffix(1);
                if (IsSingleParagraphLine(single)) {
                    itemMarkdown.resize(single.size());
                    item.text = std::move(itemMarkdown);
                } else {
                    item.children = ParseMarkdownImpl(itemMarkdown, depth + 1);
                }
            } else {
                item.text = StripInlineMarkdown(itemMarkdown);
            }
            if (!item.children.empty() && item.children.front().type == BlockType::Paragraph) {
                item.text = std::move(item.children.front().text);
                item.children.erase(item.children.begin());
            }
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
            // A GitHub alert names itself before references resolve: its marker looks like one.
            const uint8_t alert = depth == 0 ? TakeAlertMarker(quoteMarkdown) : 0;
            if (!definitions.empty() && quoteMarkdown.find('[') != std::string::npos) quoteMarkdown = ResolveReferenceLinks(quoteMarkdown, definitions);
            Block& quote = AppendBlock(blocks, BlockType::Quote);
            quote.alert = alert;
            if (depth < 8) {
                if (IsSingleParagraphLine(quoteMarkdown)) {
                    AppendBlock(quote.children, BlockType::Paragraph).text = std::move(quoteMarkdown);
                } else {
                    quote.children = ParseMarkdownImpl(quoteMarkdown, depth + 1);
                }
            } else {
                quote.text = StripInlineMarkdown(quoteMarkdown);
            }
            if (quote.children.empty()) quote.text = StripInlineMarkdown(quoteMarkdown);
            continue;
        }

        std::string paragraph;
        bool previousHardBreak = false;
        // Line i was checked above and starts no table; only the lines after it can.
        const size_t paragraphStart = i;
        while (i < lines.size() && !isSuppressed(i) && !IsBlockStart(infos[i]) &&
            (i == paragraphStart || !IsTableStart(lines, i))) {
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
        AppendBlock(blocks, BlockType::Paragraph).text = std::move(paragraph);
    }

    return blocks;
}
std::vector<Block> ParseMarkdown(const std::string& markdown) {
    return ParseMarkdownImpl(markdown, 0);
}



} // namespace TinyPdf::Internal
