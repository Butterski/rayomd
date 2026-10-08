#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RayoMd::Text {

// Longest text WriteFixed2 produces: sign, 20 integer digits, point, two decimals.
constexpr size_t kFixed2MaxChars = 24;

// Writes `value` with at most two decimals, the format of every PDF operand, and
// returns the end of the text. `out` must have room for kFixed2MaxChars characters.
inline char* WriteFixed2(char* out, double value) {
    const bool negative = std::signbit(value);
    const double hundredths = std::fabs(value) * 100.0;
    uint64_t scaled;
    if (hundredths < 4503599627370496.0) {
        // Below 2^52 the fraction of a double is exact, so rounding half away from zero
        // needs no library call. Every page coordinate takes this branch.
        scaled = static_cast<uint64_t>(hundredths);
        if (hundredths - static_cast<double>(scaled) >= 0.5) scaled++;
    } else {
        scaled = static_cast<uint64_t>(std::llround(hundredths));
    }
    if (negative) *out++ = '-';
    if (scaled < 1000000) {
        // At most four integer digits: the usual operand, written without a division loop.
        const unsigned whole = static_cast<unsigned>(scaled) / 100;
        const unsigned fraction = static_cast<unsigned>(scaled) % 100;
        if (whole >= 1000) *out++ = static_cast<char>('0' + whole / 1000);
        if (whole >= 100) *out++ = static_cast<char>('0' + whole / 100 % 10);
        if (whole >= 10) *out++ = static_cast<char>('0' + whole / 10 % 10);
        *out++ = static_cast<char>('0' + whole % 10);
        if (fraction == 0) return out;
        *out++ = '.';
        *out++ = static_cast<char>('0' + fraction / 10);
        if (fraction % 10 != 0) *out++ = static_cast<char>('0' + fraction % 10);
        return out;
    }
    const uint64_t whole = scaled / 100;
    const unsigned fraction = static_cast<unsigned>(scaled % 100);
    out = std::to_chars(out, out + 20, whole).ptr;
    if (fraction == 0) return out;
    *out++ = '.';
    *out++ = static_cast<char>('0' + fraction / 10);
    if (fraction % 10 != 0) *out++ = static_cast<char>('0' + fraction % 10);
    return out;
}

inline void AppendFixed2(std::string& out, double value) {
    char buffer[kFixed2MaxChars];
    out.append(buffer, static_cast<size_t>(WriteFixed2(buffer, value) - buffer));
}

// Byte classes for one-pass scans of Markdown text. std::string::find_first_of with a
// set of characters is several times slower on paragraph-sized input.
enum ByteClass : unsigned char {
    kByteInlineSyntax = 1,   // ! * _ ~ ` $ [ < and the backslash: may start inline Markdown
    kByteLineFeed = 2,       // '\n'
    kByteSymbolLead = 4,     // 0xE2, the first byte of the status symbols NormalizeSymbols rewrites
    kByteLiteralSpecial = 8, // ( ) \ and the control bytes 0..31 and 127: not copied as is into a PDF literal string
};

namespace Detail {
constexpr std::array<unsigned char, 256> MakeByteClasses() {
    std::array<unsigned char, 256> classes{};
    for (size_t value = 0; value < classes.size(); value++) {
        if (value < 32 || value == 127) classes[value] = kByteLiteralSpecial;
    }
    for (char ch : { '(', ')', '\\' }) classes[static_cast<unsigned char>(ch)] = kByteLiteralSpecial;
    for (char ch : { '!', '*', '_', '~', '`', '$', '[', '<', '\\' }) {
        classes[static_cast<unsigned char>(ch)] |= kByteInlineSyntax;
    }
    classes[static_cast<unsigned char>('\n')] |= kByteLineFeed;
    classes[0xE2] |= kByteSymbolLead;
    return classes;
}
inline constexpr std::array<unsigned char, 256> kByteClasses = MakeByteClasses();
} // namespace Detail

// True when `text` contains a byte of any class in `mask`.
inline bool ContainsByteClass(std::string_view text, unsigned char mask) {
    const unsigned char* at = reinterpret_cast<const unsigned char*>(text.data());
    size_t left = text.size();
    const auto& classes = Detail::kByteClasses;
    for (; left >= 8; at += 8, left -= 8) {
        const unsigned char seen = classes[at[0]] | classes[at[1]] | classes[at[2]] | classes[at[3]] |
            classes[at[4]] | classes[at[5]] | classes[at[6]] | classes[at[7]];
        if (seen & mask) return true;
    }
    for (; left > 0; at++, left--) {
        if (classes[*at] & mask) return true;
    }
    return false;
}

std::string Trim(std::string value);
std::vector<std::string> SplitLines(const std::string& text);
std::string FormatDouble(double value);

// Transcodes UTF-8 text to WinAnsiEncoding (Windows-1252), the encoding of the PDF standard
// fonts, into `out` (nullptr only checks). The status symbols the renderers rewrite become
// [OK], [!] and [X]; U+FE0F, U+FEFF and the soft hyphen are dropped. False for text that is
// not UTF-8 or holds a character WinAnsiEncoding has no code for.
bool TranscodeToWinAnsi(std::string_view utf8, std::string* out);
// The same for text that no font on the system can show: a character without a WinAnsi
// code becomes its base letter (Latin Extended-A) or '?'. Returns how many did.
size_t TranscodeToWinAnsiLossy(std::string_view utf8, std::string& out);
// The renderer and fonts a document gets, as --bench reports it: "standard-font-ascii",
// "standard-font-winansi" (Latin text in the standard fonts), or "unicode-embedded-font".
const char* RendererPathName(std::string_view text);
// Transcoded text back to UTF-8, for link targets, image paths and formulas.
std::string WinAnsiToUtf8(std::string_view winAnsi);

} // namespace RayoMd::Text
