#include "fast_markdown/tiny_pdf.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <codecvt>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <locale>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef FAST_MARKDOWN_USE_SIMDUTF
#include "simdutf.h"
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#include <emmintrin.h>
#define FAST_MD_SSE2 1
#endif

namespace TinyPdf {

using CidList = std::vector<uint16_t>;

static std::wstring Utf8ToWideFallback(std::string_view str) {
    if (str.empty()) return L"";
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstr[0], len);
    return wstr;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.from_bytes(str.data(), str.data() + str.size());
#endif
}

static std::wstring Utf8ToWide(std::string_view str) {
    if (str.empty()) return L"";
#ifdef FAST_MARKDOWN_USE_SIMDUTF
#if defined(_WIN32)
    const size_t units = simdutf::utf16_length_from_utf8(str.data(), str.size());
    if (units == 0) return Utf8ToWideFallback(str);
    static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows wchar_t must be UTF-16 sized");
    std::wstring wstr(units, 0);
    size_t written = simdutf::convert_valid_utf8_to_utf16le(
        str.data(), str.size(), reinterpret_cast<char16_t*>(&wstr[0]));
    if (written == 0) return Utf8ToWideFallback(str);
    wstr.resize(written);
    return wstr;
#else
    const size_t units = simdutf::utf16_length_from_utf8(str.data(), str.size());
    if (units == 0) return Utf8ToWideFallback(str);
    std::u16string utf16(units, 0);
    size_t written = simdutf::convert_valid_utf8_to_utf16le(str.data(), str.size(), &utf16[0]);
    if (written == 0) return Utf8ToWideFallback(str);
    std::wstring wstr(written, 0);
    for (size_t i = 0; i < written; i++) {
        wstr[i] = (wchar_t)utf16[i];
    }
    return wstr;
#endif
#else
    return Utf8ToWideFallback(str);
#endif
}

constexpr double PAGE_W = 595.0;  // A4, points
constexpr double PAGE_H = 842.0;
int g_lastError = 0;

static double ResolveMarginPoints(int marginSetting) {
    static const double presets[] = { 34.0, 54.0, 72.0 };
    if (marginSetting >= 0 && marginSetting <= 2) return presets[marginSetting];
    if (marginSetting >= 1000) {
        return std::max(18.0, std::min(144.0, (double)(marginSetting - 1000)));
    }
    return presets[1];
}

enum class BlockType {
    Heading,
    Paragraph,
    Bullet,
    Numbered,
    Quote,
    Code,
    MathBlock,
    Table,
    Rule
};

struct Block {
    BlockType type = BlockType::Paragraph;
    int level = 0;
    int number = 0;
    std::string text;
    std::vector<std::vector<std::string>> rows;
    std::vector<int> aligns;
};

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

std::string Trim(std::string s) {
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
    char rule = 0;
    int count = 0;
    for (char c : TrimView(line)) {
        if (c == ' ') continue;
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

static bool ParseHeading(std::string_view line, int& level, std::string& text) {
    std::string_view s = LTrimView(line);
    int count = 0;
    while (count < (int)s.size() && s[count] == '#') count++;
    if (count < 1 || count > 6) return false;
    if ((int)s.size() > count && !IsSpace(s[count])) return false;
    level = count;
    std::string_view view = TrimView(s.substr(count));
    while (!view.empty() && view.back() == '#') view.remove_suffix(1);
    view = RTrimView(view);
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
    text = TrimView(s.substr(count));
    while (!text.empty() && text.back() == '#') text.remove_suffix(1);
    text = RTrimView(text);
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

static std::string NormalizeSymbols(std::string s) {
    if (s.find('\xE2') == std::string::npos) return s;
    ReplaceAll(s, std::string("\xE2\x9C\x85"), "[OK]");
    ReplaceAll(s, std::string("\xE2\x9A\xA0\xEF\xB8\x8F"), "[!]");
    ReplaceAll(s, std::string("\xE2\x9A\xA0"), "[!]");
    ReplaceAll(s, std::string("\xE2\x9D\x8C"), "[X]");
    return s;
}

static std::string StripInlineMarkdown(std::string_view input) {
    if (input.find_first_of("!*_~`$[\\") == std::string_view::npos &&
        input.find('\xE2') == std::string_view::npos) {
        return ToString(TrimView(input));
    }

    std::string source = NormalizeSymbols(ToString(input));
    std::string out;
    out.reserve(source.size());

    for (size_t i = 0; i < source.size();) {
        if (source[i] == '!' && i + 1 < source.size() && source[i + 1] == '[') {
            size_t close = source.find("](", i + 2);
            size_t end = close == std::string::npos ? std::string::npos : source.find(')', close + 2);
            if (close != std::string::npos && end != std::string::npos) {
                out += "image: ";
                out += source.substr(i + 2, close - (i + 2));
                i = end + 1;
                continue;
            }
        }

        if (source[i] == '[') {
            size_t close = source.find("](", i + 1);
            size_t end = close == std::string::npos ? std::string::npos : source.find(')', close + 2);
            if (close != std::string::npos && end != std::string::npos) {
                out += source.substr(i + 1, close - (i + 1));
                std::string url = source.substr(close + 2, end - (close + 2));
                if (!url.empty()) {
                    out += " <";
                    out += url;
                    out += ">";
                }
                i = end + 1;
                continue;
            }
        }

        if (source[i] == '`') {
            size_t end = source.find('`', i + 1);
            if (end != std::string::npos) {
                out += source.substr(i + 1, end - i - 1);
                i = end + 1;
                continue;
            }
        }

        if (source[i] == '$' && !(i + 1 < source.size() && source[i + 1] == '$')) {
            size_t end = source.find('$', i + 1);
            if (end != std::string::npos) {
                out += source.substr(i + 1, end - i - 1);
                i = end + 1;
                continue;
            }
        }

        if (source[i] == '\\' && i + 1 < source.size()) {
            out.push_back(source[i + 1]);
            i += 2;
            continue;
        }

        if (source[i] == '*') {
            i++;
            continue;
        }

        if (source[i] == '_') {
            bool leftWord = i > 0 && std::isalnum((unsigned char)source[i - 1]);
            bool rightWord = i + 1 < source.size() && std::isalnum((unsigned char)source[i + 1]);
            if (!leftWord || !rightWord) {
                i++;
                continue;
            }
        }

        if (source[i] == '~' && i + 1 < source.size() && source[i + 1] == '~') {
            i += 2;
            continue;
        }

        out.push_back(source[i++]);
    }

    return Trim(out);
}

enum class LineKind {
    Plain,
    Empty,
    Fence,
    Math,
    Rule,
    Heading,
    Bullet,
    Numbered,
    Quote
};

struct LineInfo {
    std::string_view raw;
    std::string_view trimmed;
    std::string_view text;
    LineKind kind = LineKind::Plain;
    int level = 0;
    int number = 0;
};

static LineInfo ClassifyLine(std::string_view line) {
    LineInfo info;
    info.raw = line;
    info.trimmed = TrimView(line);
    if (info.trimmed.empty()) {
        info.kind = LineKind::Empty;
        return info;
    }
    if (StartsWith(info.trimmed, "```") || StartsWith(info.trimmed, "~~~")) {
        info.kind = LineKind::Fence;
        info.text = info.trimmed.substr(0, 3);
        return info;
    }
    if (StartsWith(info.trimmed, "$$")) {
        info.kind = LineKind::Math;
        info.text = TrimView(info.trimmed.substr(2));
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

static std::vector<Block> ParseMarkdown(const std::string& markdown) {
    std::vector<std::string_view> lines = SplitLineViews(markdown);
    std::vector<LineInfo> infos;
    infos.reserve(lines.size());
    for (std::string_view line : lines) infos.push_back(ClassifyLine(line));

    std::vector<Block> blocks;
    blocks.reserve(std::max<size_t>(8, lines.size() / 2));
    size_t i = 0;

    if (!infos.empty() && infos[0].trimmed == "---") {
        i = 1;
        while (i < infos.size() && infos[i].trimmed != "---" && infos[i].trimmed != "...") i++;
        if (i < lines.size()) i++;
    }

    while (i < lines.size()) {
        const LineInfo& info = infos[i];
        std::string_view line = info.raw;
        std::string_view trimmed = info.trimmed;
        if (info.kind == LineKind::Empty) {
            i++;
            continue;
        }

        if (info.kind == LineKind::Fence) {
            std::string_view fence = info.text;
            std::string text;
            i++;
            while (i < lines.size() && !(infos[i].kind == LineKind::Fence && infos[i].text == fence)) {
                text.append(lines[i].data(), lines[i].size());
                if (i + 1 < lines.size()) text += "\n";
                i++;
            }
            if (i < lines.size()) i++;
            blocks.push_back({ BlockType::Code, 0, 0, text });
            continue;
        }

        if (info.kind == LineKind::Math) {
            std::string text;
            std::string_view rest = info.text;
            if (!rest.empty()) {
                if (rest.size() >= 2 && rest.compare(rest.size() - 2, 2, "$$") == 0) {
                    rest = TrimView(rest.substr(0, rest.size() - 2));
                    blocks.push_back({ BlockType::MathBlock, 0, 0, ToString(rest) });
                    i++;
                    continue;
                }
                text = ToString(rest);
            }

            i++;
            while (i < lines.size() && infos[i].kind != LineKind::Math) {
                if (!text.empty()) text += "\n";
                std::string_view v = infos[i].trimmed;
                text.append(v.data(), v.size());
                i++;
            }
            if (i < lines.size()) i++;
            blocks.push_back({ BlockType::MathBlock, 0, 0, text });
            continue;
        }

        if (IsTableStart(lines, i)) {
            std::vector<std::vector<std::string>> rows;
            std::vector<int> aligns;
            rows.push_back(SplitTableRow(lines[i]));
            ParseTableSeparator(lines[i + 1], aligns);
            i += 2;

            while (i < lines.size() && infos[i].kind != LineKind::Empty) {
                std::vector<std::string> row = SplitTableRow(lines[i]);
                if (row.size() < 2) break;
                for (auto& cell : row) cell = StripInlineMarkdown(cell);
                rows.push_back(row);
                i++;
            }

            for (auto& cell : rows[0]) cell = StripInlineMarkdown(cell);
            Block table;
            table.type = BlockType::Table;
            table.rows = std::move(rows);
            table.aligns = std::move(aligns);
            blocks.push_back(std::move(table));
            continue;
        }

        if (info.kind == LineKind::Heading) {
            blocks.push_back({ BlockType::Heading, info.level, 0, StripInlineMarkdown(info.text) });
            i++;
            continue;
        }

        if (info.kind == LineKind::Rule) {
            blocks.push_back({ BlockType::Rule, 0, 0, "" });
            i++;
            continue;
        }

        if (info.kind == LineKind::Bullet) {
            blocks.push_back({ BlockType::Bullet, info.level, 0, ToString(info.text) });
            i++;
            continue;
        }

        if (info.kind == LineKind::Numbered) {
            blocks.push_back({ BlockType::Numbered, info.level, info.number, ToString(info.text) });
            i++;
            continue;
        }

        if (info.kind == LineKind::Quote) {
            std::string quote;
            while (i < lines.size() && infos[i].kind == LineKind::Quote) {
                if (!quote.empty()) quote += " ";
                quote.append(infos[i].text.data(), infos[i].text.size());
                i++;
            }
            blocks.push_back({ BlockType::Quote, 0, 0, StripInlineMarkdown(quote) });
            continue;
        }

        std::string paragraph;
        while (i < lines.size() && !IsBlockStart(infos[i]) && !IsTableStart(lines, i)) {
            if (!paragraph.empty()) paragraph += " ";
            std::string_view v = infos[i].trimmed;
            paragraph.append(v.data(), v.size());
            i++;
        }
        if (paragraph.empty()) {
            paragraph = ToString(trimmed);
            i++;
        }
        blocks.push_back({ BlockType::Paragraph, 0, 0, paragraph });
    }

    return blocks;
}

static uint16_t ReadU16(const std::vector<uint8_t>& data, size_t off) {
    if (off + 2 > data.size()) return 0;
    return (uint16_t)((data[off] << 8) | data[off + 1]);
}

static int16_t ReadS16(const std::vector<uint8_t>& data, size_t off) {
    return (int16_t)ReadU16(data, off);
}

static uint32_t ReadU32(const std::vector<uint8_t>& data, size_t off) {
    if (off + 4 > data.size()) return 0;
    return ((uint32_t)data[off] << 24) | ((uint32_t)data[off + 1] << 16) |
        ((uint32_t)data[off + 2] << 8) | data[off + 3];
}

static bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamsize size = file.tellg();
    if (size <= 0 || size > 64LL * 1024LL * 1024LL) return false;
    file.seekg(0, std::ios::beg);
    bytes.resize((size_t)size);
    return (bool)file.read((char*)bytes.data(), size);
}

#ifdef _WIN32
static bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& bytes) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 || size.QuadPart > 64LL * 1024LL * 1024LL) {
        CloseHandle(hFile);
        return false;
    }

    bytes.resize((size_t)size.QuadPart);
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(bytes.size() - offset, 1024 * 1024);
        DWORD read = 0;
        if (!ReadFile(hFile, bytes.data() + offset, chunk, &read, nullptr) || read == 0) {
            CloseHandle(hFile);
            return false;
        }
        offset += read;
    }

    CloseHandle(hFile);
    return true;
}
#endif

struct TtfFont {
    struct Table {
        uint32_t offset = 0;
        uint32_t length = 0;
    };

    std::vector<uint8_t> bytes;
    std::map<std::string, Table> tables;
    std::unordered_map<uint16_t, uint16_t> glyphForBmp;
    std::vector<uint16_t> advances;
    std::vector<uint32_t> loca;
    mutable std::array<uint16_t, 65536> widthCache{};
    uint16_t unitsPerEm = 1000;
    uint16_t glyphCount = 0;
    uint16_t metricCount = 0;
    int16_t indexToLocFormat = 1;
    int16_t ascent = 900;
    int16_t descent = -220;
    int16_t xMin = 0;
    int16_t yMin = -220;
    int16_t xMax = 1000;
    int16_t yMax = 900;
    bool loaded = false;

    bool Load() {
#ifdef _WIN32
        wchar_t winDir[MAX_PATH] = {};
        if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return false;

        std::vector<std::wstring> candidates;
        std::wstring fontsDir = std::wstring(winDir) + L"\\Fonts\\";
        candidates.push_back(fontsDir + L"segoeui.ttf");
        candidates.push_back(fontsDir + L"arial.ttf");
        candidates.push_back(fontsDir + L"tahoma.ttf");
#else
        std::vector<std::string> candidates;
        if (const char* explicitFont = std::getenv("FAST_MARKDOWN_FONT")) {
            candidates.push_back(explicitFont);
        }
        candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        candidates.push_back("/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf");
        candidates.push_back("/usr/share/fonts/truetype/freefont/FreeSans.ttf");
        candidates.push_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
        candidates.push_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
#endif

        for (const auto& path : candidates) {
            bytes.clear();
            tables.clear();
            glyphForBmp.clear();
            advances.clear();
            loca.clear();
            widthCache.fill(0);
            if (ReadWholeFile(path, bytes) && Parse()) {
                loaded = true;
                return true;
            }
        }
        return false;
    }

    bool Parse() {
        if (bytes.size() < 12) return false;
        uint16_t numTables = ReadU16(bytes, 4);
        if (12 + (size_t)numTables * 16 > bytes.size()) return false;

        for (uint16_t i = 0; i < numTables; i++) {
            size_t off = 12 + (size_t)i * 16;
            std::string tag((const char*)bytes.data() + off, 4);
            Table t{ ReadU32(bytes, off + 8), ReadU32(bytes, off + 12) };
            if ((size_t)t.offset + t.length <= bytes.size()) tables[tag] = t;
        }

        if (!tables.count("head") || !tables.count("hhea") || !tables.count("maxp") ||
            !tables.count("hmtx") || !tables.count("cmap") || !tables.count("loca") ||
            !tables.count("glyf")) {
            return false;
        }

        Table head = tables["head"];
        unitsPerEm = ReadU16(bytes, head.offset + 18);
        xMin = ReadS16(bytes, head.offset + 36);
        yMin = ReadS16(bytes, head.offset + 38);
        xMax = ReadS16(bytes, head.offset + 40);
        yMax = ReadS16(bytes, head.offset + 42);
        indexToLocFormat = ReadS16(bytes, head.offset + 50);
        if (unitsPerEm == 0) unitsPerEm = 1000;

        Table hhea = tables["hhea"];
        ascent = ReadS16(bytes, hhea.offset + 4);
        descent = ReadS16(bytes, hhea.offset + 6);
        metricCount = ReadU16(bytes, hhea.offset + 34);

        Table maxp = tables["maxp"];
        glyphCount = ReadU16(bytes, maxp.offset + 4);
        if (glyphCount == 0 || metricCount == 0) return false;

        Table hmtx = tables["hmtx"];
        advances.assign(glyphCount, 500);
        uint16_t lastAdvance = 500;
        for (uint16_t i = 0; i < glyphCount; i++) {
            if (i < metricCount) {
                size_t off = hmtx.offset + (size_t)i * 4;
                if (off + 2 > bytes.size()) return false;
                lastAdvance = ReadU16(bytes, off);
                advances[i] = lastAdvance;
            } else {
                advances[i] = lastAdvance;
            }
        }

        if (!ParseCmap()) return false;
        if (!ParseLoca()) return false;
        return GlyphFor('A') != 0 && GlyphFor(' ') != 0;
    }

    bool ParseCmap() {
        glyphForBmp.clear();
        glyphForBmp.reserve(4096);
        Table cmap = tables["cmap"];
        if (cmap.offset + 4 > bytes.size()) return false;
        uint16_t count = ReadU16(bytes, cmap.offset + 2);
        size_t chosen = 0;

        for (uint16_t i = 0; i < count; i++) {
            size_t rec = cmap.offset + 4 + (size_t)i * 8;
            if (rec + 8 > bytes.size()) return false;
            uint16_t platform = ReadU16(bytes, rec);
            uint16_t encoding = ReadU16(bytes, rec + 2);
            uint32_t subOffset = ReadU32(bytes, rec + 4);
            size_t sub = cmap.offset + subOffset;
            if (sub + 2 > bytes.size()) continue;
            uint16_t format = ReadU16(bytes, sub);
            if (format == 4 && platform == 3 && (encoding == 1 || encoding == 10)) {
                chosen = sub;
                break;
            }
            if (format == 4 && chosen == 0) chosen = sub;
        }

        if (chosen == 0) return false;
        uint16_t length = ReadU16(bytes, chosen + 2);
        uint16_t segCount = ReadU16(bytes, chosen + 6) / 2;
        if (segCount == 0 || chosen + length > bytes.size()) return false;

        size_t endCode = chosen + 14;
        size_t startCode = endCode + (size_t)segCount * 2 + 2;
        size_t idDelta = startCode + (size_t)segCount * 2;
        size_t idRangeOffset = idDelta + (size_t)segCount * 2;
        if (idRangeOffset + (size_t)segCount * 2 > chosen + length) return false;

        for (uint16_t i = 0; i < segCount; i++) {
            uint16_t start = ReadU16(bytes, startCode + (size_t)i * 2);
            uint16_t end = ReadU16(bytes, endCode + (size_t)i * 2);
            int16_t delta = ReadS16(bytes, idDelta + (size_t)i * 2);
            uint16_t range = ReadU16(bytes, idRangeOffset + (size_t)i * 2);
            if (start > end) continue;

            for (uint32_t cp = start; cp <= end && cp < 65536; cp++) {
                uint16_t glyph = 0;
                if (range == 0) {
                    glyph = (uint16_t)((cp + delta) & 0xffff);
                } else {
                    size_t glyphOff = idRangeOffset + (size_t)i * 2 + range + (size_t)(cp - start) * 2;
                    if (glyphOff + 2 <= chosen + length) {
                        glyph = ReadU16(bytes, glyphOff);
                        if (glyph != 0) glyph = (uint16_t)((glyph + delta) & 0xffff);
                    }
                }
                if (glyph != 0) glyphForBmp[(uint16_t)cp] = glyph;
                if (cp == 0xffff) break;
            }
        }

        return true;
    }

    bool ParseLoca() {
        Table table = tables["loca"];
        loca.assign((size_t)glyphCount + 1, 0);
        if (indexToLocFormat == 0) {
            if ((size_t)table.offset + (size_t)(glyphCount + 1) * 2 > bytes.size()) return false;
            for (uint32_t i = 0; i <= glyphCount; i++) {
                loca[i] = (uint32_t)ReadU16(bytes, table.offset + (size_t)i * 2) * 2u;
            }
        } else {
            if ((size_t)table.offset + (size_t)(glyphCount + 1) * 4 > bytes.size()) return false;
            for (uint32_t i = 0; i <= glyphCount; i++) {
                loca[i] = ReadU32(bytes, table.offset + (size_t)i * 4);
            }
        }

        Table glyf = tables["glyf"];
        for (uint32_t off : loca) {
            if (off > glyf.length) return false;
        }
        return true;
    }

    uint16_t GlyphFor(uint32_t cp) const {
        if (cp <= 0xffff) {
            auto it = glyphForBmp.find((uint16_t)cp);
            if (it != glyphForBmp.end()) return it->second;
        }
        if (cp != '?') return GlyphFor('?');
        return 0;
    }

    uint16_t WidthForCid(uint16_t cid) const {
        uint16_t cached = widthCache[cid];
        if (cached != 0) return cached;
        uint16_t glyph = GlyphFor(cid);
        if (glyph < advances.size()) {
            uint16_t width = (uint16_t)((advances[glyph] * 1000u + unitsPerEm / 2u) / unitsPerEm);
            widthCache[cid] = width;
            return width;
        }
        widthCache[cid] = 500;
        return 500;
    }

    int Metric(int value) const {
        return (int)((value * 1000.0) / unitsPerEm);
    }
};

template<typename Fn>
static void ForEachCodepoint(std::wstring_view text, Fn fn) {
    for (size_t i = 0; i < text.size(); i++) {
        uint32_t cp = (uint16_t)text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
            uint32_t lo = (uint16_t)text[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        fn(cp);
    }
}

static uint16_t CidForCodepoint(const TtfFont& font, uint32_t cp) {
    return (cp <= 0xffff && font.GlyphFor(cp) != 0) ? (uint16_t)cp : (uint16_t)'?';
}

static double CodepointWidth(const TtfFont& font, uint32_t cp, double scale) {
    return font.WidthForCid(CidForCodepoint(font, cp)) * scale;
}

static double TextWidth(const TtfFont& font, std::wstring_view text, double size) {
    double w = 0.0;
    const double scale = size * 0.001;
    ForEachCodepoint(text, [&](uint32_t cp) {
        w += CodepointWidth(font, cp, scale);
    });
    return w;
}

static bool IsWideSpace(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r';
}

static void PushWrappedWord(const TtfFont& font, std::wstring_view word, double wordWidth,
    double maxWidth, double size, double spaceWidth, std::wstring& line,
    double& lineWidth, std::vector<std::wstring>& lines) {
    if (word.empty()) return;

    if (!line.empty() && lineWidth + spaceWidth + wordWidth <= maxWidth) {
        line.push_back(L' ');
        line.append(word.data(), word.size());
        lineWidth += spaceWidth + wordWidth;
        return;
    }

    if (line.empty() && wordWidth <= maxWidth) {
        line.assign(word.data(), word.size());
        lineWidth = wordWidth;
        return;
    }

    if (!line.empty()) {
        lines.push_back(line);
        line.clear();
        lineWidth = 0.0;
    }

    if (wordWidth <= maxWidth) {
        line.assign(word.data(), word.size());
        lineWidth = wordWidth;
        return;
    }

    std::wstring part;
    part.reserve(word.size());
    double partWidth = 0.0;
    const double scale = size * 0.001;
    for (wchar_t ch : word) {
        double chWidth = CodepointWidth(font, (uint16_t)ch, scale);
        if (!part.empty() && partWidth + chWidth > maxWidth) {
            lines.push_back(part);
            part.clear();
            partWidth = 0.0;
        }
        part.push_back(ch);
        partWidth += chWidth;
    }
    line = std::move(part);
    lineWidth = partWidth;
}

static std::vector<std::wstring> WrapText(const TtfFont& font, std::wstring_view raw, double maxWidth, double size) {
    std::vector<std::wstring> lines;
    lines.reserve(std::max<size_t>(1, raw.size() / 72));
    std::wstring line;
    line.reserve(std::min<size_t>(raw.size(), 256));
    double lineWidth = 0.0;
    double spaceWidth = TextWidth(font, L" ", size);
    size_t i = 0;

    while (i < raw.size()) {
        while (i < raw.size() && IsWideSpace(raw[i])) i++;
        size_t start = i;
        while (i < raw.size() && !IsWideSpace(raw[i])) i++;
        std::wstring_view word = raw.substr(start, i - start);
        double wordWidth = TextWidth(font, word, size);
        PushWrappedWord(font, word, wordWidth, maxWidth, size, spaceWidth, line, lineWidth, lines);
    }

    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back(L"");
    return lines;
}

static std::vector<std::wstring> WrapCodeLine(const TtfFont& font, std::wstring_view line, double maxWidth, double size) {
    std::vector<std::wstring> lines;
    std::wstring part;
    part.reserve(std::min<size_t>(line.size(), 256));
    double partWidth = 0.0;
    const double scale = size * 0.001;
    for (wchar_t ch : line) {
        if (ch == L'\t') ch = L' ';
        double chWidth = CodepointWidth(font, (uint16_t)ch, scale);
        if (!part.empty() && partWidth + chWidth > maxWidth) {
            lines.push_back(part);
            part.clear();
            partWidth = 0.0;
        }
        part.push_back(ch);
        partWidth += chWidth;
    }
    lines.push_back(part);
    return lines;
}

static void AppendF(std::string& out, double v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%.2f", v);
    while (n > 1 && buf[n - 1] == '0') n--;
    if (n > 0 && buf[n - 1] == '.') n--;
    out.append(buf, (size_t)n);
}

std::string F(double v) {
    std::string s;
    s.reserve(16);
    AppendF(s, v);
    return s;
}

static void AppendInt(std::string& out, int value) {
    char buf[24];
    auto result = std::to_chars(buf, buf + sizeof(buf), value);
    if (result.ec == std::errc()) out.append(buf, (size_t)(result.ptr - buf));
}

static void AppendSize(std::string& out, size_t value) {
    char buf[32];
    auto result = std::to_chars(buf, buf + sizeof(buf), value);
    if (result.ec == std::errc()) out.append(buf, (size_t)(result.ptr - buf));
}

static void AppendHex4(std::string& out, uint16_t value) {
    static char table[65536][4];
    static bool init = false;
    if (!init) {
        static const char* h = "0123456789ABCDEF";
        for (uint32_t i = 0; i < 65536; i++) {
            table[i][0] = h[(i >> 12) & 0xf];
            table[i][1] = h[(i >> 8) & 0xf];
            table[i][2] = h[(i >> 4) & 0xf];
            table[i][3] = h[i & 0xf];
        }
        init = true;
    }
    size_t old = out.size();
    out.resize(old + 4);
    memcpy(&out[old], table[value], 4);
}

class UsedCidSet {
public:
    void Add(uint16_t cid) {
        uint64_t mask = 1ull << (cid & 63);
        uint64_t& word = bits[cid >> 6];
        if ((word & mask) != 0) return;
        word |= mask;
        values.push_back(cid);
        sorted = false;
    }

    const CidList& Values() const {
        if (!sorted) {
            std::sort(values.begin(), values.end());
            sorted = true;
        }
        return values;
    }

private:
    std::array<uint64_t, 1024> bits{};
    mutable CidList values;
    mutable bool sorted = true;
};

static std::string HexText(const TtfFont& font, const std::wstring& text, UsedCidSet& usedCids) {
    std::string out;
    out.reserve(2 + text.size() * 4);
    out.push_back('<');
    ForEachCodepoint(std::wstring_view(text), [&](uint32_t cp) {
        uint16_t cid = CidForCodepoint(font, cp);
        usedCids.Add(cid);
        AppendHex4(out, cid);
    });
    out.push_back('>');
    return out;
}

static void AppendU16(std::string& out, uint16_t v) {
    out.push_back((char)((v >> 8) & 0xff));
    out.push_back((char)(v & 0xff));
}

static void AppendU32(std::string& out, uint32_t v) {
    out.push_back((char)((v >> 24) & 0xff));
    out.push_back((char)((v >> 16) & 0xff));
    out.push_back((char)((v >> 8) & 0xff));
    out.push_back((char)(v & 0xff));
}

static void SetU16(std::string& out, size_t off, uint16_t v) {
    if (off + 2 > out.size()) return;
    out[off] = (char)((v >> 8) & 0xff);
    out[off + 1] = (char)(v & 0xff);
}

static void SetU32(std::string& out, size_t off, uint32_t v) {
    if (off + 4 > out.size()) return;
    out[off] = (char)((v >> 24) & 0xff);
    out[off + 1] = (char)((v >> 16) & 0xff);
    out[off + 2] = (char)((v >> 8) & 0xff);
    out[off + 3] = (char)(v & 0xff);
}

static uint32_t ChecksumBytes(const char* data, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i += 4) {
        uint32_t word = 0;
        word |= (uint32_t)(unsigned char)data[i] << 24;
        if (i + 1 < size) word |= (uint32_t)(unsigned char)data[i + 1] << 16;
        if (i + 2 < size) word |= (uint32_t)(unsigned char)data[i + 2] << 8;
        if (i + 3 < size) word |= (uint32_t)(unsigned char)data[i + 3];
        sum += word;
    }
    return sum;
}

static uint32_t ChecksumString(const std::string& s) {
    return ChecksumBytes(s.data(), s.size());
}

static std::string MakeCidKey(const CidList& used) {
    std::string key;
    key.reserve(used.size() * 2);
    for (uint16_t cid : used) {
        key.push_back((char)((cid >> 8) & 0xff));
        key.push_back((char)(cid & 0xff));
    }
    return key;
}

class PdfObjects {
public:
    int Reserve() {
        objects.push_back("");
        return (int)objects.size();
    }

    int Add(const std::string& body) {
        objects.push_back(body);
        return (int)objects.size();
    }

    void Set(int id, const std::string& body) {
        if (id > 0 && id <= (int)objects.size()) objects[id - 1] = body;
    }

    int AddStream(const std::string& dict, const std::string& data) {
        std::string body;
        body.reserve(data.size() + dict.size() + 64);
        body += "<< ";
        body += dict;
        body += " /Length ";
        AppendSize(body, data.size());
        body += " >>\nstream\n";
        body += data;
        body += "\nendstream";
        return Add(body);
    }

    std::string Build(int rootId, int infoId) const {
        std::string pdf;
        size_t reserveBytes = 64 * 1024;
        for (const auto& object : objects) reserveBytes += object.size() + 64;
        pdf.reserve(reserveBytes);
        pdf += "%PDF-1.7\n%";
        pdf.push_back((char)0xE2);
        pdf.push_back((char)0xE3);
        pdf.push_back((char)0xCF);
        pdf.push_back((char)0xD3);
        pdf += "\n";

        std::vector<size_t> offsets(objects.size() + 1, 0);
        for (size_t i = 0; i < objects.size(); i++) {
            offsets[i + 1] = pdf.size();
            AppendSize(pdf, i + 1);
            pdf += " 0 obj\n";
            pdf += objects[i];
            pdf += "\nendobj\n";
        }

        size_t xref = pdf.size();
        pdf += "xref\n0 ";
        AppendSize(pdf, objects.size() + 1);
        pdf += "\n0000000000 65535 f \n";

        char entry[32];
        for (size_t i = 1; i <= objects.size(); i++) {
            snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offsets[i]);
            pdf += entry;
        }

        pdf += "trailer\n<< /Size ";
        AppendSize(pdf, objects.size() + 1);
        pdf += " /Root ";
        AppendInt(pdf, rootId);
        pdf += " 0 R /Info ";
        AppendInt(pdf, infoId);
        pdf += " 0 R >>\nstartxref\n";
        AppendSize(pdf, xref);
        pdf += "\n%%EOF\n";
        return pdf;
    }

private:
    std::vector<std::string> objects;
};

class Renderer {
public:
    Renderer(const TtfFont& f, int fontObject, int styleIndex, int marginIndex)
        : font(f), fontId(fontObject), style(styleIndex), marginStyle(marginIndex) {
        margin = ResolveMarginPoints(marginStyle);
        bodySize = style == 2 ? 10.5 : 11.5;
        lineHeight = bodySize * 1.35;
        NewPage();
    }

    void Render(const std::vector<Block>& blocks) {
        if (blocks.empty()) {
            RenderParagraph("Empty document");
            return;
        }

        for (const Block& block : blocks) {
            switch (block.type) {
            case BlockType::Heading:
                RenderHeading(block);
                break;
            case BlockType::Paragraph:
                RenderParagraph(block.text);
                break;
            case BlockType::Bullet:
                RenderListItem("- ", block);
                y -= 2.0;
                break;
            case BlockType::Numbered:
            {
                std::string marker;
                marker.reserve(8);
                AppendInt(marker, block.number);
                marker += ". ";
                RenderListItem(marker, block);
                y -= 2.0;
                break;
            }
            case BlockType::Quote:
                RenderQuote(block.text);
                break;
            case BlockType::Code:
                RenderCode(block.text);
                break;
            case BlockType::MathBlock:
                RenderMath(block.text);
                break;
            case BlockType::Table:
                RenderTable(block.rows, block.aligns);
                break;
            case BlockType::Rule:
                RenderRule();
                break;
            }
        }
    }

    const std::vector<std::string>& Pages() const { return pages; }
    const CidList& UsedCids() const { return usedCids.Values(); }

private:
    const TtfFont& font;
    int fontId = 0;
    int style = 0;
    int marginStyle = 1;
    double margin = 62.0;
    double bodySize = 11.5;
    double lineHeight = 15.5;
    double y = 0.0;
    std::vector<std::string> pages;
    UsedCidSet usedCids;

    struct StyledSpan {
        std::wstring text;
        bool bold = false;
        bool italic = false;
        bool strike = false;
    };

    struct StyledWord {
        std::wstring text;
        bool bold = false;
        bool italic = false;
        bool strike = false;
    };

    void PushSpan(std::vector<StyledSpan>& spans, std::string_view text, bool bold, bool italic, bool strike) {
        if (text.empty()) return;
        std::wstring wide = Utf8ToWide(text);
        if (!spans.empty() && spans.back().bold == bold && spans.back().italic == italic && spans.back().strike == strike) {
            spans.back().text += wide;
        } else {
            spans.push_back({ wide, bold, italic, strike });
        }
    }

    std::vector<StyledSpan> ParseInlineStyled(const std::string& input) {
        std::string source = NormalizeSymbols(input);
        std::vector<StyledSpan> spans;
        std::string buf;
        bool bold = false, italic = false, strike = false;

        auto flush = [&]() {
            PushSpan(spans, std::string_view(buf), bold, italic, strike);
            buf.clear();
        };

        for (size_t i = 0; i < source.size();) {
            if (source[i] == '!' && i + 1 < source.size() && source[i + 1] == '[') {
                size_t close = source.find("](", i + 2);
                size_t end = close == std::string::npos ? std::string::npos : source.find(')', close + 2);
                if (close != std::string::npos && end != std::string::npos) {
                    buf += "image: ";
                    buf += source.substr(i + 2, close - (i + 2));
                    i = end + 1;
                    continue;
                }
            }

            if (source[i] == '[') {
                size_t close = source.find("](", i + 1);
                size_t end = close == std::string::npos ? std::string::npos : source.find(')', close + 2);
                if (close != std::string::npos && end != std::string::npos) {
                    buf += source.substr(i + 1, close - (i + 1));
                    std::string url = source.substr(close + 2, end - (close + 2));
                    if (!url.empty()) {
                        buf += " <";
                        buf += url;
                        buf += ">";
                    }
                    i = end + 1;
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
                buf.push_back(source[i + 1]);
                i += 2;
                continue;
            }

            if (i + 2 < source.size() && source.compare(i, 3, "***") == 0) {
                flush();
                bool turnOn = !(bold && italic);
                bold = turnOn;
                italic = turnOn;
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

            buf.push_back(source[i++]);
        }

        flush();
        return spans;
    }

    std::vector<StyledWord> SplitStyledWords(const std::vector<StyledSpan>& spans) {
        std::vector<StyledWord> words;
        words.reserve(spans.size() * 3);
        for (const auto& span : spans) {
            std::wstring current;
            for (wchar_t ch : span.text) {
                if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r') {
                    if (!current.empty()) {
                        words.push_back({ current, span.bold, span.italic, span.strike });
                        current.clear();
                    }
                } else {
                    current.push_back(ch);
                }
            }
            if (!current.empty()) words.push_back({ current, span.bold, span.italic, span.strike });
        }
        return words;
    }

    void AppendStyledSpan(std::vector<StyledSpan>& line, std::wstring text, bool bold, bool italic, bool strike) {
        if (text.empty()) return;
        if (!line.empty() && line.back().bold == bold && line.back().italic == italic && line.back().strike == strike) {
            line.back().text += text;
            return;
        }
        line.push_back({ std::move(text), bold, italic, strike });
    }

    bool IsClosingPunctuation(const std::wstring& text) {
        return text.size() == 1 && (text[0] == L'.' || text[0] == L',' || text[0] == L';' ||
            text[0] == L':' || text[0] == L'!' || text[0] == L'?' || text[0] == L')');
    }

    std::vector<std::vector<StyledSpan>> WrapStyled(const std::string& text, double width, double size) {
        if (text.find_first_of("!*_~`$[\\") == std::string::npos) {
            std::vector<std::vector<StyledSpan>> lines;
            std::wstring wide = Utf8ToWide(NormalizeSymbols(text));
            for (const auto& line : WrapText(font, std::wstring_view(wide), width, size)) {
                lines.push_back({ { line, false, false, false } });
            }
            if (lines.empty()) lines.push_back({});
            return lines;
        }

        std::vector<StyledWord> words = SplitStyledWords(ParseInlineStyled(text));
        std::vector<std::vector<StyledSpan>> lines;
        lines.reserve(std::max<size_t>(1, text.size() / 72));
        std::vector<StyledSpan> line;
        line.reserve(16);
        double lineWidth = 0.0;
        double spaceWidth = TextWidth(font, L" ", size);

        for (const auto& word : words) {
            double wordWidth = TextWidth(font, word.text, size);
            bool needsSpace = !line.empty() && !IsClosingPunctuation(word.text);
            double addWidth = wordWidth + (needsSpace ? spaceWidth : 0.0);

            if (!line.empty() && lineWidth + addWidth > width) {
                lines.push_back(line);
                line.clear();
                lineWidth = 0.0;
                needsSpace = false;
                addWidth = wordWidth;
            }

            std::wstring textRun = word.text;
            if (needsSpace) {
                if (word.strike) {
                    AppendStyledSpan(line, L" ", false, false, false);
                    lineWidth += spaceWidth;
                } else {
                    textRun.insert(textRun.begin(), L' ');
                    wordWidth += spaceWidth;
                }
            }
            AppendStyledSpan(line, std::move(textRun), word.bold, word.italic, word.strike);
            lineWidth += wordWidth;
        }

        if (!line.empty()) lines.push_back(line);
        if (lines.empty()) lines.push_back({});
        return lines;
    }

    void NewPage() {
        pages.push_back("");
        pages.back().reserve(64 * 1024);
        y = PAGE_H - margin;
    }

    void Ensure(double needed) {
        if (y - needed < margin) NewPage();
    }

    void PaintText(double x, double baseline, double size, const std::wstring& line, const char* color,
        bool fauxBold = false, bool italic = false, bool strike = false) {
        std::string& c = pages.back();
        c += "q ";
        c += color;
        c += " rg BT /F1 ";
        AppendF(c, size);
        c += " Tf 1 0 ";
        c += italic ? "0.18 " : "0 ";
        c += "1 ";
        AppendF(c, x);
        c += " ";
        AppendF(c, baseline);
        c += " Tm ";
        std::string hex = HexText(font, line, usedCids);
        c += hex;
        c += " Tj";
        if (fauxBold) {
            c += " 1 0 ";
            c += italic ? "0.18 " : "0 ";
            c += "1 ";
            AppendF(c, x + 0.28);
            c += " ";
            AppendF(c, baseline);
            c += " Tm ";
            c += hex;
            c += " Tj";
        }
        c += " ET";
        if (strike && !line.empty()) {
            double width = TextWidth(font, line, size);
            c += " ";
            c += color;
            c += " RG 0.55 w ";
            AppendF(c, x);
            c += " ";
            AppendF(c, baseline + size * 0.34);
            c += " m ";
            AppendF(c, x + width);
            c += " ";
            AppendF(c, baseline + size * 0.34);
            c += " l S";
        }
        c += " Q\n";
    }

    void DrawTextLine(double x, double size, const std::wstring& line, const char* color = "0.08 0.08 0.08") {
        double lh = size * 1.35;
        Ensure(lh);
        PaintText(x, y - size, size, line, color);
        y -= lh;
    }

    void DrawRect(double x, double top, double w, double h, const char* color) {
        std::string& c = pages.back();
        c += "q ";
        c += color;
        c += " rg ";
        AppendF(c, x);
        c += " ";
        AppendF(c, top - h);
        c += " ";
        AppendF(c, w);
        c += " ";
        AppendF(c, h);
        c += " re f Q\n";
    }

    void DrawStrokeRect(double x, double top, double w, double h, const char* color = "0.72 0.72 0.72", double lineWidth = 0.45) {
        std::string& c = pages.back();
        c += "q ";
        c += color;
        c += " RG ";
        AppendF(c, lineWidth);
        c += " w ";
        AppendF(c, x);
        c += " ";
        AppendF(c, top - h);
        c += " ";
        AppendF(c, w);
        c += " ";
        AppendF(c, h);
        c += " re S Q\n";
    }

    void RenderHeading(const Block& block) {
        static const double sizes[] = { 0, 26, 22, 18, 15, 13, 12 };
        int level = std::max(1, std::min(6, block.level));
        double size = sizes[level];
        if (y < PAGE_H - margin - 4.0) y -= level <= 2 ? 12.0 : 8.0;

        std::wstring text = Utf8ToWide(block.text);
        for (const auto& line : WrapText(font, text, PAGE_W - margin * 2.0, size)) {
            DrawTextLine(margin, size, line, "0.02 0.02 0.02");
        }
        y -= level <= 2 ? 8.0 : 5.0;
    }

    void RenderParagraph(const std::string& text) {
        RenderParagraph(text, margin, PAGE_W - margin * 2.0);
    }

    void RenderParagraph(const std::string& text, double x, double width) {
        double lh = bodySize * 1.35;
        for (const auto& line : WrapStyled(text, width, bodySize)) {
            Ensure(lh);
            double cursor = x;
            double baseline = y - bodySize;
            for (const auto& span : line) {
                PaintText(cursor, baseline, bodySize, span.text, "0.08 0.08 0.08", span.bold, span.italic, span.strike);
                cursor += TextWidth(font, span.text, bodySize);
            }
            y -= lh;
        }
        y -= 5.0;
    }

    void RenderListItem(const std::string& marker, const Block& block) {
        double x = margin + 16.0 + block.level * 18.0;
        double width = PAGE_W - margin * 2.0 - 16.0 - block.level * 18.0;
        RenderParagraph(marker + block.text, x, width);
    }

    void RenderQuote(const std::string& text) {
        std::wstring wide = Utf8ToWide(text);
        double x = margin + 14.0;
        double width = PAGE_W - margin * 2.0 - 22.0;
        for (const auto& line : WrapText(font, wide, width, bodySize)) {
            Ensure(lineHeight + 2.0);
            DrawRect(margin, y + 2.0, PAGE_W - margin * 2.0, lineHeight + 3.0, "0.94 0.95 0.96");
            DrawRect(margin, y + 2.0, 3.0, lineHeight + 3.0, "0.45 0.62 0.72");
            DrawTextLine(x, bodySize, line, "0.18 0.22 0.25");
        }
        y -= 7.0;
    }

    void RenderCode(const std::string& text) {
        std::vector<std::string> raw = SplitLines(text);
        double size = 9.5;
        double lh = size * 1.35;
        double x = margin + 8.0;
        double width = PAGE_W - margin * 2.0 - 16.0;
        for (const auto& rawLine : raw) {
            std::wstring wide = Utf8ToWide(rawLine);
            for (const auto& line : WrapCodeLine(font, wide, width, size)) {
                Ensure(lh + 4.0);
                DrawRect(margin, y + 3.0, PAGE_W - margin * 2.0, lh + 5.0, "0.95 0.95 0.93");
                DrawTextLine(x, size, line, "0.12 0.12 0.12");
            }
        }
        y -= 8.0;
    }

    void RenderMath(const std::string& text) {
        std::vector<std::string> raw = SplitLines(text);
        double size = 10.5;
        double lh = size * 1.45;
        double x = margin + 12.0;
        double width = PAGE_W - margin * 2.0 - 24.0;
        for (const auto& rawLine : raw) {
            std::wstring wide = Utf8ToWide(rawLine);
            for (const auto& line : WrapCodeLine(font, wide, width, size)) {
                Ensure(lh + 6.0);
                DrawRect(margin, y + 4.0, PAGE_W - margin * 2.0, lh + 7.0, "0.97 0.97 0.95");
                DrawStrokeRect(margin, y + 4.0, PAGE_W - margin * 2.0, lh + 7.0, "0.82 0.78 0.62", 0.4);
                DrawTextLine(x, size, line, "0.10 0.10 0.10");
            }
        }
        y -= 8.0;
    }

    void RenderTable(const std::vector<std::vector<std::string>>& rows, const std::vector<int>& aligns) {
        if (rows.empty()) return;

        size_t columns = 0;
        for (const auto& row : rows) columns = std::max(columns, row.size());
        if (columns == 0) return;

        double tableWidth = PAGE_W - margin * 2.0;
        double colWidth = tableWidth / columns;
        double size = 9.6;
        double lh = size * 1.32;
        double pad = 5.0;
        double cellTextWidth = std::max(16.0, colWidth - pad * 2.0);

        y -= 3.0;
        for (size_t r = 0; r < rows.size(); r++) {
            std::vector<std::vector<std::wstring>> wrapped(columns);
            size_t maxLines = 1;

            for (size_t c = 0; c < columns; c++) {
                std::string cell = c < rows[r].size() ? rows[r][c] : "";
                wrapped[c] = WrapText(font, Utf8ToWide(cell), cellTextWidth, size);
                maxLines = std::max(maxLines, wrapped[c].size());
            }

            double rowHeight = maxLines * lh + pad * 2.0;
            Ensure(rowHeight + 5.0);
            double top = y;
            if (r == 0) DrawRect(margin, top, tableWidth, rowHeight, "0.91 0.93 0.95");

            for (size_t c = 0; c < columns; c++) {
                double cellX = margin + c * colWidth;
                DrawStrokeRect(cellX, top, colWidth, rowHeight);

                int align = c < aligns.size() ? aligns[c] : -1;
                for (size_t lineIdx = 0; lineIdx < wrapped[c].size(); lineIdx++) {
                    const std::wstring& line = wrapped[c][lineIdx];
                    double tx = cellX + pad;
                    double lineWidth = TextWidth(font, line, size);
                    if (align == 0) tx = cellX + (colWidth - lineWidth) * 0.5;
                    else if (align == 1) tx = cellX + colWidth - pad - lineWidth;

                    double baseline = top - pad - size - lineIdx * lh;
                    PaintText(tx, baseline, size, line, r == 0 ? "0.04 0.04 0.04" : "0.10 0.10 0.10", r == 0);
                }
            }

            y -= rowHeight;
        }
        y -= 9.0;
    }

    void RenderRule() {
        Ensure(18.0);
        y -= 5.0;
        std::string& c = pages.back();
        c += "q 0.68 0.68 0.68 RG 0.8 w ";
        AppendF(c, margin);
        c += " ";
        AppendF(c, y);
        c += " m ";
        AppendF(c, PAGE_W - margin);
        c += " ";
        AppendF(c, y);
        c += " l S Q\n";
        y -= 13.0;
    }
};

static bool AddGlyphClosure(const TtfFont& font, uint16_t glyph, std::vector<uint8_t>& include) {
    if (glyph >= include.size()) return false;
    if (include[glyph]) return true;
    include[glyph] = 1;

    if (glyph + 1 >= font.loca.size()) return true;
    uint32_t start = font.loca[glyph];
    uint32_t end = font.loca[glyph + 1];
    if (end <= start) return true;

    auto it = font.tables.find("glyf");
    if (it == font.tables.end()) return false;
    const TtfFont::Table& glyf = it->second;
    size_t glyphStart = (size_t)glyf.offset + start;
    size_t glyphEnd = (size_t)glyf.offset + end;
    if (glyphEnd > font.bytes.size() || glyphStart + 10 > glyphEnd) return false;

    int16_t contours = ReadS16(font.bytes, glyphStart);
    if (contours >= 0) return true;

    size_t off = glyphStart + 10;
    bool more = true;
    while (more) {
        if (off + 4 > glyphEnd) return false;
        uint16_t flags = ReadU16(font.bytes, off);
        uint16_t componentGlyph = ReadU16(font.bytes, off + 2);
        off += 4;
        if (!AddGlyphClosure(font, componentGlyph, include)) return false;
        off += (flags & 0x0001) ? 4 : 2;
        if (flags & 0x0008) off += 2;
        else if (flags & 0x0040) off += 4;
        else if (flags & 0x0080) off += 8;
        more = (flags & 0x0020) != 0;
    }
    return off <= glyphEnd;
}

static bool OriginalTableBytes(const TtfFont& font, const std::string& tag, std::string& out) {
    auto it = font.tables.find(tag);
    if (it == font.tables.end()) return false;
    const TtfFont::Table& t = it->second;
    if ((size_t)t.offset + t.length > font.bytes.size()) return false;
    out.assign((const char*)font.bytes.data() + t.offset, t.length);
    return true;
}

static bool BuildSubsetFontBytes(const TtfFont& font, const CidList& used, std::string& out) {
    if (font.glyphCount == 0 || font.loca.size() < (size_t)font.glyphCount + 1) return false;

    std::vector<uint8_t> include((size_t)font.glyphCount, 0);
    AddGlyphClosure(font, 0, include);
    AddGlyphClosure(font, font.GlyphFor('?'), include);
    AddGlyphClosure(font, font.GlyphFor(' '), include);
    for (uint16_t cid : used) {
        AddGlyphClosure(font, font.GlyphFor(cid), include);
    }

    auto glyfIt = font.tables.find("glyf");
    if (glyfIt == font.tables.end()) return false;
    const TtfFont::Table& oldGlyf = glyfIt->second;

    std::string glyfData;
    glyfData.reserve(std::min<size_t>(font.bytes.size(), std::max<size_t>(32 * 1024, used.size() * 256)));
    std::string locaData;
    locaData.reserve(((size_t)font.glyphCount + 1) * 4);

    for (uint32_t gid = 0; gid < font.glyphCount; gid++) {
        AppendU32(locaData, (uint32_t)glyfData.size());
        uint32_t start = font.loca[gid];
        uint32_t end = font.loca[gid + 1];
        if (include[gid] && end > start) {
            size_t oldStart = (size_t)oldGlyf.offset + start;
            size_t oldEnd = (size_t)oldGlyf.offset + end;
            if (oldEnd > font.bytes.size() || oldEnd < oldStart) return false;
            glyfData.append((const char*)font.bytes.data() + oldStart, oldEnd - oldStart);
            while (glyfData.size() & 3) glyfData.push_back('\0');
        }
    }
    AppendU32(locaData, (uint32_t)glyfData.size());

    struct SubsetTable {
        std::string tag;
        std::string data;
        uint32_t checksum = 0;
        uint32_t offset = 0;
    };

    static const char* wanted[] = {
        "OS/2", "cmap", "cvt ", "fpgm", "gasp", "glyf", "head", "hhea",
        "hmtx", "loca", "maxp", "name", "post", "prep"
    };

    std::vector<SubsetTable> tables;
    for (const char* tag : wanted) {
        std::string table;
        std::string tagString(tag, 4);
        if (tagString == "glyf") {
            table = glyfData;
        } else if (tagString == "loca") {
            table = locaData;
        } else {
            if (!OriginalTableBytes(font, tagString, table)) continue;
            if (tagString == "head") {
                if (table.size() < 54) return false;
                SetU32(table, 8, 0);
                SetU16(table, 50, 1);
            }
        }
        tables.push_back({ tagString, std::move(table), 0, 0 });
    }

    if (tables.empty()) return false;
    std::sort(tables.begin(), tables.end(),
        [](const SubsetTable& a, const SubsetTable& b) { return a.tag < b.tag; });

    uint16_t numTables = (uint16_t)tables.size();
    uint16_t maxPower = 1;
    uint16_t entrySelector = 0;
    while ((uint16_t)(maxPower * 2) <= numTables) {
        maxPower *= 2;
        entrySelector++;
    }
    uint16_t searchRange = maxPower * 16;
    uint16_t rangeShift = numTables * 16 - searchRange;

    out.clear();
    out.reserve(12 + (size_t)numTables * 16 + glyfData.size() + locaData.size() + 96 * numTables);
    AppendU32(out, ReadU32(font.bytes, 0));
    AppendU16(out, numTables);
    AppendU16(out, searchRange);
    AppendU16(out, entrySelector);
    AppendU16(out, rangeShift);
    out.resize(12 + (size_t)numTables * 16, '\0');

    size_t headAdjustmentOffset = std::string::npos;
    for (SubsetTable& table : tables) {
        while (out.size() & 3) out.push_back('\0');
        table.offset = (uint32_t)out.size();
        table.checksum = ChecksumString(table.data);
        out.append(table.data);
        while (out.size() & 3) out.push_back('\0');
        if (table.tag == "head") headAdjustmentOffset = table.offset + 8;
    }

    for (size_t i = 0; i < tables.size(); i++) {
        size_t rec = 12 + i * 16;
        memcpy(&out[rec], tables[i].tag.data(), 4);
        SetU32(out, rec + 4, tables[i].checksum);
        SetU32(out, rec + 8, tables[i].offset);
        SetU32(out, rec + 12, (uint32_t)tables[i].data.size());
    }

    if (headAdjustmentOffset == std::string::npos) return false;
    uint32_t fileSum = ChecksumString(out);
    SetU32(out, headAdjustmentOffset, 0xB1B0AFBAu - fileSum);
    return true;
}

static std::string BuildFontFileObject(const std::string& fontBytes) {
    std::string body;
    body.reserve(fontBytes.size() + 128);
    body += "<< /Length ";
    AppendSize(body, fontBytes.size());
    body += " /Length1 ";
    AppendSize(body, fontBytes.size());
    body += " >>\nstream\n";
    body.append(fontBytes.data(), fontBytes.size());
    body += "\nendstream";
    return body;
}

static const std::string& CachedFontFileObject(const TtfFont& font, const CidList& used, const std::string& key) {
    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    std::string subset;
    const std::string* fontBytes = nullptr;
    if (BuildSubsetFontBytes(font, used, subset) && subset.size() < font.bytes.size()) {
        fontBytes = &subset;
    }

    std::string full;
    if (!fontBytes) {
        full.assign((const char*)font.bytes.data(), font.bytes.size());
        fontBytes = &full;
    }

    auto inserted = cache.emplace(key, BuildFontFileObject(*fontBytes));
    return inserted.first->second;
}

static const std::string& MakeCidToGidMap(const TtfFont& font, const CidList& used, const std::string& key) {
    static std::unordered_map<std::string, std::string> cache;
    auto cached = cache.find(key);
    if (cached != cache.end()) return cached->second;

    uint16_t maxCid = 255;
    for (uint16_t cid : used) maxCid = std::max(maxCid, cid);

    std::string map((size_t)(maxCid + 1) * 2, '\0');
    for (uint32_t cid = 0; cid <= maxCid; cid++) {
        uint16_t glyph = font.GlyphFor(cid);
        map[(size_t)cid * 2] = (char)((glyph >> 8) & 0xff);
        map[(size_t)cid * 2 + 1] = (char)(glyph & 0xff);
    }
    auto inserted = cache.emplace(key, std::move(map));
    return inserted.first->second;
}

static const std::string& MakeToUnicodeCMap(const CidList& used, const std::string& key) {
    static std::unordered_map<std::string, std::string> cache;
    auto cached = cache.find(key);
    if (cached != cache.end()) return cached->second;

    std::string out;
    out.reserve(512 + used.size() * 20);
    out += "/CIDInit /ProcSet findresource begin\n"
           "12 dict begin\nbegincmap\n"
           "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
           "/CMapName /FastMarkdownUnicode def\n"
           "/CMapType 2 def\n"
           "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";

    int count = 0;
    for (auto it = used.begin(); it != used.end();) {
        int chunk = std::min<int>(100, (int)used.size() - count);
        AppendInt(out, chunk);
        out += " beginbfchar\n";
        for (int i = 0; i < chunk; i++, ++it) {
            out.push_back('<');
            AppendHex4(out, *it);
            out += "> <";
            AppendHex4(out, *it);
            out += ">\n";
            count++;
        }
        out += "endbfchar\n";
    }

    if (count == 0) {
        out += "1 beginbfchar\n<0020> <0020>\nendbfchar\n";
    }

    out += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    auto inserted = cache.emplace(key, std::move(out));
    return inserted.first->second;
}

static const std::string& MakeWidths(const TtfFont& font, const CidList& used, const std::string& key) {
    static std::unordered_map<std::string, std::string> cache;
    auto cached = cache.find(key);
    if (cached != cache.end()) return cached->second;

    std::string out;
    out.reserve(16 + used.size() * 14);
    out += "[ ";
    if (used.empty()) {
        out += "32 [ ";
        AppendInt(out, font.WidthForCid(32));
        out += " ] ";
    } else {
        for (uint16_t cid : used) {
            AppendInt(out, cid);
            out += " [ ";
            AppendInt(out, font.WidthForCid(cid));
            out += " ] ";
        }
    }
    out += "]";
    auto inserted = cache.emplace(key, std::move(out));
    return inserted.first->second;
}

static std::string EscapeLiteral(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
        if ((unsigned char)c >= 32 && (unsigned char)c < 127) out.push_back(c);
    }
    return out;
}

bool IsAsciiDocument(const std::string& s) {
    auto isAsciiAfterSymbolNormalization = [](std::string_view text) {
        for (size_t i = 0; i < text.size();) {
            unsigned char c = (unsigned char)text[i];
            if (c < 128) {
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
            if (i + 3 <= text.size() && text.compare(i, 3, "\xE2\x9A\xA0") == 0) {
                i += 3;
                continue;
            }
            if (i + 3 <= text.size() && text.compare(i, 3, "\xE2\x9D\x8C") == 0) {
                i += 3;
                continue;
            }
            return false;
        }
        return true;
    };

#ifdef FAST_MD_SSE2
    const char* ptr = s.data();
    const char* end = ptr + s.size();
    while (ptr + 16 <= end) {
        __m128i chunk = _mm_loadu_si128((const __m128i*)ptr);
        if (_mm_movemask_epi8(chunk) != 0) return isAsciiAfterSymbolNormalization(s);
        ptr += 16;
    }
    while (ptr < end) {
        if ((unsigned char)*ptr >= 128) return isAsciiAfterSymbolNormalization(s);
        ptr++;
    }
    return true;
#else
    return isAsciiAfterSymbolNormalization(s);
#endif
}

static double AsciiCharWidth(char c, double size, bool mono = false) {
    if (mono) return size * 0.60;
    if (c == ' ') return size * 0.28;
    if (c == 'i' || c == 'l' || c == 'I' || c == '!' || c == '.' || c == ',' || c == ':' || c == ';') return size * 0.25;
    if (c == 'm' || c == 'w' || c == 'M' || c == 'W') return size * 0.78;
    if (c >= '0' && c <= '9') return size * 0.56;
    if (c >= 'A' && c <= 'Z') return size * 0.62;
    return size * 0.52;
}

static double AsciiTextWidth(std::string_view text, double size, bool mono = false) {
    double width = 0.0;
    for (char c : text) width += AsciiCharWidth(c, size, mono);
    return width;
}

static std::vector<std::string> WrapAsciiText(const std::string& raw, double maxWidth, double size, bool mono = false) {
    std::string text = StripInlineMarkdown(raw);
    std::vector<std::string> lines;
    std::string line;
    line.reserve(std::min<size_t>(text.size(), 256));
    double lineWidth = 0.0;
    double spaceWidth = AsciiTextWidth(" ", size, mono);
    size_t i = 0;

    while (i < text.size()) {
        while (i < text.size() && IsSpace(text[i])) i++;
        size_t start = i;
        while (i < text.size() && !IsSpace(text[i])) i++;
        std::string_view word(text.data() + start, i - start);
        if (word.empty()) continue;
        double wordWidth = AsciiTextWidth(word, size, mono);

        if (!line.empty() && lineWidth + spaceWidth + wordWidth <= maxWidth) {
            line.push_back(' ');
            line.append(word.data(), word.size());
            lineWidth += spaceWidth + wordWidth;
            continue;
        }

        if (line.empty() && wordWidth <= maxWidth) {
            line.assign(word.data(), word.size());
            lineWidth = wordWidth;
            continue;
        }

        if (!line.empty()) {
            lines.push_back(line);
            line.clear();
            lineWidth = 0.0;
        }

        if (wordWidth <= maxWidth) {
            line.assign(word.data(), word.size());
            lineWidth = wordWidth;
            continue;
        }

        std::string part;
        part.reserve(word.size());
        double partWidth = 0.0;
        for (char c : word) {
            double cw = AsciiCharWidth(c, size, mono);
            if (!part.empty() && partWidth + cw > maxWidth) {
                lines.push_back(part);
                part.clear();
                partWidth = 0.0;
            }
            part.push_back(c);
            partWidth += cw;
        }
        line = std::move(part);
        lineWidth = partWidth;
    }

    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back("");
    return lines;
}

class StandardRenderer {
public:
    StandardRenderer(int styleIndex, int marginIndex) : style(styleIndex) {
        margin = ResolveMarginPoints(marginIndex);
        bodySize = style == 2 ? 10.5 : 11.5;
        lineHeight = bodySize * 1.35;
        NewPage();
    }

    void Render(const std::vector<Block>& blocks) {
        if (blocks.empty()) {
            RenderParagraph("Empty document");
            return;
        }

        for (const Block& block : blocks) {
            switch (block.type) {
            case BlockType::Heading:
                RenderHeading(block);
                break;
            case BlockType::Paragraph:
                RenderParagraph(block.text);
                break;
            case BlockType::Bullet:
                RenderParagraph("- " + block.text, margin + 16.0 + block.level * 18.0,
                    PAGE_W - margin * 2.0 - 16.0 - block.level * 18.0);
                y -= 2.0;
                break;
            case BlockType::Numbered:
            {
                std::string item;
                item.reserve(block.text.size() + 8);
                AppendInt(item, block.number);
                item += ". ";
                item += block.text;
                RenderParagraph(item, margin + 16.0 + block.level * 18.0,
                    PAGE_W - margin * 2.0 - 16.0 - block.level * 18.0);
                y -= 2.0;
                break;
            }
            case BlockType::Quote:
                RenderQuote(block.text);
                break;
            case BlockType::Code:
                RenderCode(block.text);
                break;
            case BlockType::MathBlock:
                RenderMath(block.text);
                break;
            case BlockType::Table:
                RenderTable(block.rows, block.aligns);
                break;
            case BlockType::Rule:
                RenderRule();
                break;
            }
        }
    }

    const std::vector<std::string>& Pages() const { return pages; }

private:
    int style = 0;
    double margin = 54.0;
    double bodySize = 11.5;
    double lineHeight = 15.5;
    double y = 0.0;
    std::vector<std::string> pages;

    void NewPage() {
        pages.push_back("");
        pages.back().reserve(64 * 1024);
        y = PAGE_H - margin;
    }

    void Ensure(double needed) {
        if (y - needed < margin) NewPage();
    }

    void Rect(double x, double top, double w, double h, const char* color, bool stroke = false) {
        std::string& c = pages.back();
        c += "q ";
        c += color;
        c += stroke ? " RG 0.45 w " : " rg ";
        AppendF(c, x);
        c += " ";
        AppendF(c, top - h);
        c += " ";
        AppendF(c, w);
        c += " ";
        AppendF(c, h);
        c += " re ";
        c += stroke ? "S Q\n" : "f Q\n";
    }

    void Text(double x, double baseline, double size, const std::string& text, const char* fontName, const char* color = "0.08 0.08 0.08") {
        if (text.empty()) return;
        std::string& c = pages.back();
        c += "q ";
        c += color;
        c += " rg BT /";
        c += fontName;
        c += " ";
        AppendF(c, size);
        c += " Tf 1 0 0 1 ";
        AppendF(c, x);
        c += " ";
        AppendF(c, baseline);
        c += " Tm (";
        c += EscapeLiteral(text);
        c += ") Tj ET Q\n";
    }

    void DrawTextLine(double x, double size, const std::string& line, const char* fontName = "F1", const char* color = "0.08 0.08 0.08", bool mono = false) {
        double lh = size * 1.35;
        Ensure(lh);
        Text(x, y - size, size, line, fontName, color);
        y -= lh;
    }

    void RenderHeading(const Block& block) {
        static const double sizes[] = { 0, 26, 22, 18, 15, 13, 12 };
        int level = std::max(1, std::min(6, block.level));
        double size = sizes[level];
        if (y < PAGE_H - margin - 4.0) y -= level <= 2 ? 12.0 : 8.0;
        for (const auto& line : WrapAsciiText(block.text, PAGE_W - margin * 2.0, size)) {
            DrawTextLine(margin, size, line, "F2", "0.02 0.02 0.02");
        }
        y -= level <= 2 ? 8.0 : 5.0;
    }

    void RenderParagraph(const std::string& text) {
        RenderParagraph(text, margin, PAGE_W - margin * 2.0);
    }

    void RenderParagraph(const std::string& text, double x, double width) {
        for (const auto& line : WrapAsciiText(text, width, bodySize)) {
            DrawTextLine(x, bodySize, line);
        }
        y -= 5.0;
    }

    void RenderQuote(const std::string& text) {
        double x = margin + 14.0;
        double width = PAGE_W - margin * 2.0 - 22.0;
        for (const auto& line : WrapAsciiText(text, width, bodySize)) {
            Ensure(lineHeight + 2.0);
            Rect(margin, y + 2.0, PAGE_W - margin * 2.0, lineHeight + 3.0, "0.94 0.95 0.96");
            Rect(margin, y + 2.0, 3.0, lineHeight + 3.0, "0.45 0.62 0.72");
            DrawTextLine(x, bodySize, line, "F1", "0.18 0.22 0.25");
        }
        y -= 7.0;
    }

    void RenderCode(const std::string& text) {
        std::vector<std::string> raw = SplitLines(text);
        double size = 9.5;
        double lh = size * 1.35;
        double x = margin + 8.0;
        double width = PAGE_W - margin * 2.0 - 16.0;
        for (const auto& rawLine : raw) {
            for (const auto& line : WrapAsciiText(rawLine, width, size, true)) {
                Ensure(lh + 4.0);
                Rect(margin, y + 3.0, PAGE_W - margin * 2.0, lh + 5.0, "0.95 0.95 0.93");
                DrawTextLine(x, size, line, "F3", "0.12 0.12 0.12", true);
            }
        }
        y -= 8.0;
    }

    void RenderMath(const std::string& text) {
        double size = 10.5;
        double lh = size * 1.45;
        for (const auto& rawLine : SplitLines(text)) {
            for (const auto& line : WrapAsciiText(rawLine, PAGE_W - margin * 2.0 - 24.0, size, true)) {
                Ensure(lh + 6.0);
                Rect(margin, y + 4.0, PAGE_W - margin * 2.0, lh + 7.0, "0.97 0.97 0.95");
                Rect(margin, y + 4.0, PAGE_W - margin * 2.0, lh + 7.0, "0.82 0.78 0.62", true);
                DrawTextLine(margin + 12.0, size, line, "F3", "0.10 0.10 0.10", true);
            }
        }
        y -= 8.0;
    }

    void RenderTable(const std::vector<std::vector<std::string>>& rows, const std::vector<int>& aligns) {
        if (rows.empty()) return;
        size_t columns = 0;
        for (const auto& row : rows) columns = std::max(columns, row.size());
        if (columns == 0) return;

        double tableWidth = PAGE_W - margin * 2.0;
        double colWidth = tableWidth / columns;
        double size = 9.6;
        double lh = size * 1.32;
        double pad = 5.0;
        y -= 3.0;

        for (size_t r = 0; r < rows.size(); r++) {
            std::vector<std::vector<std::string>> wrapped(columns);
            size_t maxLines = 1;
            for (size_t c = 0; c < columns; c++) {
                std::string cell = c < rows[r].size() ? rows[r][c] : "";
                wrapped[c] = WrapAsciiText(cell, std::max(16.0, colWidth - pad * 2.0), size);
                maxLines = std::max(maxLines, wrapped[c].size());
            }

            double rowHeight = maxLines * lh + pad * 2.0;
            Ensure(rowHeight + 5.0);
            double top = y;
            if (r == 0) Rect(margin, top, tableWidth, rowHeight, "0.91 0.93 0.95");
            for (size_t c = 0; c < columns; c++) {
                double cellX = margin + c * colWidth;
                Rect(cellX, top, colWidth, rowHeight, "0.72 0.72 0.72", true);
                int align = c < aligns.size() ? aligns[c] : -1;
                for (size_t li = 0; li < wrapped[c].size(); li++) {
                    const std::string& line = wrapped[c][li];
                    double tx = cellX + pad;
                    double lw = AsciiTextWidth(line, size);
                    if (align == 0) tx = cellX + (colWidth - lw) * 0.5;
                    else if (align == 1) tx = cellX + colWidth - pad - lw;
                    Text(tx, top - pad - size - li * lh, size, line, r == 0 ? "F2" : "F1", "0.08 0.08 0.08");
                }
            }
            y -= rowHeight;
        }
        y -= 9.0;
    }

    void RenderRule() {
        Ensure(18.0);
        y -= 5.0;
        std::string& c = pages.back();
        c += "q 0.68 0.68 0.68 RG 0.8 w ";
        AppendF(c, margin);
        c += " ";
        AppendF(c, y);
        c += " m ";
        AppendF(c, PAGE_W - margin);
        c += " ";
        AppendF(c, y);
        c += " l S Q\n";
        y -= 13.0;
    }
};

static bool BuildStandardPdfBytes(const std::string& markdown, int styleIdx, int marginIdx, std::string& pdfBytes) {
    std::vector<Block> blocks = ParseMarkdown(markdown);
    PdfObjects pdf;
    int pagesId = pdf.Reserve();
    int fontRegularId = pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    int fontBoldId = pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>");
    int fontMonoId = pdf.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>");

    StandardRenderer renderer(styleIdx, marginIdx);
    renderer.Render(blocks);

    std::vector<int> pageIds;
    for (const std::string& pageContent : renderer.Pages()) {
        int contentId = pdf.AddStream("", pageContent);
        std::string page;
        page.reserve(192);
        page += "<< /Type /Page /Parent ";
        AppendInt(page, pagesId);
        page += " 0 R /MediaBox [0 0 ";
        AppendF(page, PAGE_W);
        page += " ";
        AppendF(page, PAGE_H);
        page += "] /Resources << /Font << /F1 ";
        AppendInt(page, fontRegularId);
        page += " 0 R /F2 ";
        AppendInt(page, fontBoldId);
        page += " 0 R /F3 ";
        AppendInt(page, fontMonoId);
        page += " 0 R >> >> /Contents ";
        AppendInt(page, contentId);
        page += " 0 R >>";
        pageIds.push_back(pdf.Add(page));
    }

    std::string pages;
    pages.reserve(48 + pageIds.size() * 8);
    pages += "<< /Type /Pages /Kids [";
    for (int id : pageIds) {
        AppendInt(pages, id);
        pages += " 0 R ";
    }
    pages += "] /Count ";
    AppendSize(pages, pageIds.size());
    pages += " >>";
    pdf.Set(pagesId, pages);

    std::string catalog;
    catalog.reserve(48);
    catalog += "<< /Type /Catalog /Pages ";
    AppendInt(catalog, pagesId);
    catalog += " 0 R >>";
    int catalogId = pdf.Add(catalog);
    int infoId = pdf.Add("<< /Producer (Fast Markdown Native Standard PDF) /Creator (Fast Markdown) /Title (" +
        EscapeLiteral("Markdown Export") + ") >>");

    pdfBytes = pdf.Build(catalogId, infoId);
    return true;
}

static const TtfFont* GetCachedFont() {
    static TtfFont font;
    static bool tried = false;
    static bool loaded = false;

    if (!tried) {
        loaded = font.Load();
        tried = true;
    }
    return loaded ? &font : nullptr;
}

static bool BuildUnicodePdfBytes(const std::string& markdown, int styleIdx, int marginIdx, std::string& pdfBytes) {
    const TtfFont* fontPtr = GetCachedFont();
    if (!fontPtr) {
        g_lastError = 1;
        return false;
    }
    const TtfFont& font = *fontPtr;

    std::vector<Block> blocks = ParseMarkdown(markdown);
    PdfObjects pdf;

    int fontFileId = pdf.Reserve();
    int cidMapId = pdf.Reserve();
    int toUnicodeId = pdf.Reserve();
    int descriptorId = pdf.Reserve();
    int cidFontId = pdf.Reserve();
    int type0FontId = pdf.Reserve();
    int pagesId = pdf.Reserve();

    Renderer renderer(font, type0FontId, styleIdx, marginIdx);
    renderer.Render(blocks);

    const CidList& used = renderer.UsedCids();
    std::string cidKey = MakeCidKey(used);
    const std::string& cidMapBytes = MakeCidToGidMap(font, used, cidKey);
    const std::string& toUnicodeBytes = MakeToUnicodeCMap(used, cidKey);

    pdf.Set(fontFileId, CachedFontFileObject(font, used, cidKey));
    std::string cidMapObject;
    cidMapObject.reserve(cidMapBytes.size() + 48);
    cidMapObject += "<< /Length ";
    AppendSize(cidMapObject, cidMapBytes.size());
    cidMapObject += " >>\nstream\n";
    cidMapObject += cidMapBytes;
    cidMapObject += "\nendstream";
    pdf.Set(cidMapId, cidMapObject);

    std::string toUnicodeObject;
    toUnicodeObject.reserve(toUnicodeBytes.size() + 48);
    toUnicodeObject += "<< /Length ";
    AppendSize(toUnicodeObject, toUnicodeBytes.size());
    toUnicodeObject += " >>\nstream\n";
    toUnicodeObject += toUnicodeBytes;
    toUnicodeObject += "\nendstream";
    pdf.Set(toUnicodeId, toUnicodeObject);

    std::string desc;
    desc.reserve(224);
    desc += "<< /Type /FontDescriptor /FontName /FastMarkdownSegoe /Flags 32 /FontBBox [";
    AppendInt(desc, font.Metric(font.xMin));
    desc += " ";
    AppendInt(desc, font.Metric(font.yMin));
    desc += " ";
    AppendInt(desc, font.Metric(font.xMax));
    desc += " ";
    AppendInt(desc, font.Metric(font.yMax));
    desc += "] /ItalicAngle 0 /Ascent ";
    AppendInt(desc, font.Metric(font.ascent));
    desc += " /Descent ";
    AppendInt(desc, font.Metric(font.descent));
    desc += " /CapHeight ";
    AppendInt(desc, (int)(font.Metric(font.ascent) * 0.72));
    desc += " /StemV 80 /FontFile2 ";
    AppendInt(desc, fontFileId);
    desc += " 0 R >>";
    pdf.Set(descriptorId, desc);

    std::string cidFont;
    cidFont.reserve(256);
    cidFont += "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /FastMarkdownSegoe"
        " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>"
        " /FontDescriptor ";
    AppendInt(cidFont, descriptorId);
    cidFont += " 0 R /CIDToGIDMap ";
    AppendInt(cidFont, cidMapId);
    cidFont += " 0 R /DW 500 /W ";
    cidFont += MakeWidths(font, used, cidKey);
    cidFont += " >>";
    pdf.Set(cidFontId, cidFont);

    std::string type0;
    type0.reserve(160);
    type0 += "<< /Type /Font /Subtype /Type0 /BaseFont /FastMarkdownSegoe"
        " /Encoding /Identity-H /DescendantFonts [";
    AppendInt(type0, cidFontId);
    type0 += " 0 R] /ToUnicode ";
    AppendInt(type0, toUnicodeId);
    type0 += " 0 R >>";
    pdf.Set(type0FontId, type0);

    std::vector<int> pageIds;
    for (const std::string& pageContent : renderer.Pages()) {
        int contentId = pdf.AddStream("", pageContent);
        std::string page;
        page.reserve(160);
        page += "<< /Type /Page /Parent ";
        AppendInt(page, pagesId);
        page += " 0 R /MediaBox [0 0 ";
        AppendF(page, PAGE_W);
        page += " ";
        AppendF(page, PAGE_H);
        page += "] /Resources << /Font << /F1 ";
        AppendInt(page, type0FontId);
        page += " 0 R >> >> /Contents ";
        AppendInt(page, contentId);
        page += " 0 R >>";
        pageIds.push_back(pdf.Add(page));
    }

    std::string pages;
    pages.reserve(48 + pageIds.size() * 8);
    pages += "<< /Type /Pages /Kids [";
    for (int id : pageIds) {
        AppendInt(pages, id);
        pages += " 0 R ";
    }
    pages += "] /Count ";
    AppendSize(pages, pageIds.size());
    pages += " >>";
    pdf.Set(pagesId, pages);

    std::string catalog;
    catalog.reserve(48);
    catalog += "<< /Type /Catalog /Pages ";
    AppendInt(catalog, pagesId);
    catalog += " 0 R >>";
    int catalogId = pdf.Add(catalog);
    int infoId = pdf.Add("<< /Producer (Fast Markdown Native Tiny PDF) /Creator (Fast Markdown) /Title (" +
        EscapeLiteral("Markdown Export") + ") >>");

    pdfBytes = pdf.Build(catalogId, infoId);
    return true;
}

bool BuildPdfBytes(const std::string& markdown, int styleIdx, int marginIdx, std::string& pdfBytes) {
    g_lastError = 0;
    if (IsAsciiDocument(markdown)) {
        return BuildStandardPdfBytes(markdown, styleIdx, marginIdx, pdfBytes);
    }
    return BuildUnicodePdfBytes(markdown, styleIdx, marginIdx, pdfBytes);
}

} // namespace TinyPdf
