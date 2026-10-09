#include "text_utils.h"

#include <cstdint>
#include <cstring>
#include <string_view>

namespace RayoMd::Text {
namespace {

bool IsSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

// Code points of WinAnsiEncoding codes 0x80..0x9F; 0 for the five codes without a glyph.
// Codes 0xA0..0xFF are U+00A0..U+00FF.
constexpr uint16_t kWinAnsiHighCodePoints[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

} // namespace

int WinAnsiCode(uint32_t codePoint) {
    if (codePoint >= 0xA0 && codePoint <= 0xFF) return static_cast<int>(codePoint);
    for (int index = 0; index < 32; index++) {
        if (kWinAnsiHighCodePoints[index] == codePoint) return 0x80 + index;
    }
    return -1;
}

namespace {

// Bytes from text[at] that are ASCII, eight at a time.
size_t AsciiRunLength(std::string_view text, size_t at) {
    const size_t start = at;
    for (; at + 8 <= text.size(); at += 8) {
        uint64_t word;
        std::memcpy(&word, text.data() + at, 8);
        if (word & 0x8080808080808080ull) break;
    }
    while (at < text.size() && static_cast<unsigned char>(text[at]) < 0x80) at++;
    return at - start;
}

// Base letters of Latin Extended-A (U+0100..U+017F), for text no font on the system can
// show: "Zażółć" reads as "Zazolc" rather than "Za??o??". '?' where there is none.
constexpr char kLatinExtendedABase[] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi??JjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOoOo??RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

// The WinAnsi bytes of `utf8` into `out` (nullptr only checks). Without `replaced`, false at
// the first character WinAnsiEncoding has no code for; with it, such a character becomes its
// base letter or '?', counted in *replaced.
bool ToWinAnsi(std::string_view utf8, std::string* out, size_t* replaced) {
    if (out) {
        out->clear();
        out->reserve(utf8.size());
    }
    // The last three bytes written: the renderers read 0xE2 0x9C 0x85, 0xE2 0x9A 0xA0 and
    // 0xE2 0x9D 0x8C as the UTF-8 of a status symbol, and in WinAnsi 0xE2 is "a" circumflex.
    uint32_t last = 0;
    for (size_t at = 0; at < utf8.size();) {
        const size_t ascii = AsciiRunLength(utf8, at);
        if (ascii != 0) {
            if (out) out->append(utf8.data() + at, ascii);
            last = static_cast<unsigned char>(utf8[at + ascii - 1]);
            at += ascii;
            continue;
        }
        uint32_t codePoint = 0;
        size_t length = 0;
        int code = -1;
        if (DecodeUtf8(utf8, at, codePoint, length)) {
            at += length;
            const char* symbol = codePoint == 0x2705 ? "[OK]" : codePoint == 0x26A0 ? "[!]" :
                codePoint == 0x274C ? "[X]" : nullptr;
            if (symbol) {
                if (out) out->append(symbol);
                last = ']';
                continue;
            }
            // Variation selector, byte-order mark and soft hyphen: nothing to show.
            if (codePoint == 0xFE0F || codePoint == 0xFEFF || codePoint == 0x00AD) continue;
            code = WinAnsiCode(codePoint);
        } else {
            at++;      // not UTF-8: one byte at a time
        }
        if (code >= 0) {
            const uint32_t next = ((last << 8) | static_cast<uint32_t>(code)) & 0xFFFFFFu;
            if (next == 0xE29C85u || next == 0xE29AA0u || next == 0xE29D8Cu) code = -1;
        }
        if (code < 0) {
            if (!replaced) return false;
            ++*replaced;
            code = codePoint >= 0x100 && codePoint < 0x180 ? kLatinExtendedABase[codePoint - 0x100] : '?';
        }
        last = ((last << 8) | static_cast<uint32_t>(code)) & 0xFFFFFFu;
        if (out) out->push_back(static_cast<char>(code));
    }
    return true;
}

} // namespace

bool TranscodeToWinAnsi(std::string_view utf8, std::string* out) {
    return ToWinAnsi(utf8, out, nullptr);
}

size_t TranscodeToWinAnsiLossy(std::string_view utf8, std::string& out) {
    size_t replaced = 0;
    ToWinAnsi(utf8, &out, &replaced);
    return replaced;
}

const char* RendererPathName(std::string_view text) {
    if (AsciiRunLength(text, 0) == text.size()) return "standard-font-ascii";
    return TranscodeToWinAnsi(text, nullptr) ? "standard-font-winansi" : "unicode-embedded-font";
}

std::string WinAnsiToUtf8(std::string_view winAnsi) {
    std::string utf8;
    utf8.reserve(winAnsi.size() + winAnsi.size() / 2);
    for (const char ch : winAnsi) {
        const unsigned char code = static_cast<unsigned char>(ch);
        uint32_t codePoint = code;
        if (code >= 0x80 && code < 0xA0) codePoint = kWinAnsiHighCodePoints[code - 0x80];
        if (codePoint < 0x80) {
            if (code < 0x80) utf8.push_back(ch);
            continue;      // a code without a glyph
        }
        if (codePoint < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        } else {
            utf8.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        }
        utf8.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    return utf8;
}

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

} // namespace RayoMd::Text