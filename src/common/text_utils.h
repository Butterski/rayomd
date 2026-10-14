#pragma once

#include "compiler.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOMD_TEXT_SSE2 1
#endif

namespace RayoMd::Text {

// Longest text WriteFixed2 produces: sign, 20 integer digits, point, two decimals.
constexpr size_t kFixed2MaxChars = 24;

namespace Detail {
// "00" to "99": two digits a copy.
inline constexpr char kDigitPairs[201] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

inline char* WritePair(char* out, unsigned value) {
    std::memcpy(out, kDigitPairs + 2 * value, 2);
    return out + 2;
}
} // namespace Detail

// Writes `value` with at most two decimals, the format of every PDF operand, and
// returns the end of the text. `out` must have room for kFixed2MaxChars characters;
// the byte after the end may be overwritten.
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
        // From 2^52 on a double is a whole number, so rounding is the conversion: what llround
        // (ExactMath::LlRound) gives, also INT64_MIN's bits past 2^63 and for NaN.
        scaled = hundredths < 9223372036854775808.0 ? static_cast<uint64_t>(static_cast<int64_t>(hundredths))
                                                     : static_cast<uint64_t>(INT64_MIN);
    }
    if (negative) *out++ = '-';
    unsigned fraction;
    if (scaled < 1000000) {
        // At most four integer digits, the usual operand: two digits a step.
        const unsigned all = static_cast<unsigned>(scaled);
        const unsigned whole = all / 100;
        fraction = all - whole * 100;
        if (whole >= 100) {
            const unsigned high = whole / 100;
            if (high >= 10) out = Detail::WritePair(out, high);
            else *out++ = static_cast<char>('0' + high);
            out = Detail::WritePair(out, whole - high * 100);
        } else if (whole >= 10) {
            out = Detail::WritePair(out, whole);
        } else {
            *out++ = static_cast<char>('0' + whole);
        }
    } else {
        fraction = static_cast<unsigned>(scaled % 100);
        out = std::to_chars(out, out + 20, scaled / 100).ptr;
    }
    if (fraction == 0) return out;
    *out = '.';
    Detail::WritePair(out + 1, fraction);
    return out + (Detail::kDigitPairs[2 * fraction + 1] == '0' ? 2 : 3);
}

inline void AppendFixed2(std::string& out, double value) {
    char buffer[kFixed2MaxChars];
    out.append(buffer, static_cast<size_t>(WriteFixed2(buffer, value) - buffer));
}

// Byte classes for one-pass scans of Markdown text. std::string::find_first_of with a
// set of characters is several times slower on paragraph-sized input.
enum ByteClass : unsigned char {
    kByteInlineSyntax = 1,   // ! * _ ~ ` $ [ < & and the backslash: may start inline Markdown or a character reference
    kByteLineFeed = 2,       // '\n'
    kByteSymbolLead = 4,     // 0xE2, the first byte of the status symbols NormalizeSymbols rewrites
    kByteLiteralSpecial = 8, // ( ) \ and the control bytes 0..31 and 127: not copied as is into a PDF literal string
    kByteAutolinkLead = 16,  // ':' and '@', where the inline parser finds a bare URL or email address
};

namespace Detail {
constexpr std::array<unsigned char, 256> MakeByteClasses() {
    std::array<unsigned char, 256> classes{};
    for (size_t value = 0; value < classes.size(); value++) {
        if (value < 32 || value == 127) classes[value] = kByteLiteralSpecial;
    }
    for (char ch : { '(', ')', '\\' }) classes[static_cast<unsigned char>(ch)] = kByteLiteralSpecial;
    for (char ch : { '!', '*', '_', '~', '`', '$', '[', '<', '&', '\\' }) {
        classes[static_cast<unsigned char>(ch)] |= kByteInlineSyntax;
    }
    classes[static_cast<unsigned char>('\n')] |= kByteLineFeed;
    classes[0xE2] |= kByteSymbolLead;
    classes[static_cast<unsigned char>(':')] |= kByteAutolinkLead;
    classes[static_cast<unsigned char>('@')] |= kByteAutolinkLead;
    return classes;
}
inline constexpr std::array<unsigned char, 256> kByteClasses = MakeByteClasses();
} // namespace Detail

// True when `text` contains a byte of any class in `mask`. Always inlined: its callers are hot
// scanners, and tiny_pdf.cpp sits at GCC's inlining limit, where an edit left it out of line.
RAYOMD_HOT_INLINE bool ContainsByteClass(std::string_view text, unsigned char mask) {
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

namespace Detail {
template <unsigned char Mask>
constexpr size_t ClassByteCount() {
    size_t count = 0;
    for (unsigned char value : kByteClasses) count += (value & Mask) != 0;
    return count;
}
// The bytes of the classes in `Mask`, in ascending order.
template <unsigned char Mask>
constexpr std::array<unsigned char, ClassByteCount<Mask>()> ClassBytes() {
    std::array<unsigned char, ClassByteCount<Mask>()> bytes{};
    size_t count = 0;
    for (size_t value = 0; value < kByteClasses.size(); value++) {
        if (kByteClasses[value] & Mask) bytes[count++] = static_cast<unsigned char>(value);
    }
    return bytes;
}
#if RAYOMD_TEXT_SSE2
// 0xFF in each lane of `chunk` that holds a byte of the classes in `Mask`: one compare a byte.
template <unsigned char Mask, size_t... Index>
inline __m128i MatchClassBytes(__m128i chunk, std::index_sequence<Index...>) {
    constexpr auto bytes = ClassBytes<Mask>();
    __m128i hit = _mm_setzero_si128();
    ((hit = _mm_or_si128(hit, _mm_cmpeq_epi8(chunk, _mm_set1_epi8(static_cast<char>(bytes[Index]))))), ...);
    return hit;
}
#endif
inline unsigned LowestBit(unsigned bits) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_ctz(bits));
#else
    unsigned bit = 0;
    while ((bits & 1u) == 0) {
        bits >>= 1;
        bit++;
    }
    return bit;
#endif
}
} // namespace Detail

// The index of the first byte of `text` from `from` on with a class in `Mask`, or text.size().
// In SSE2 16 bytes a step, one compare per byte of the classes (a mask of a few bytes only);
// the last step reads the text's last 16 bytes again.
template <unsigned char Mask>
inline size_t FindByteClass(std::string_view text, size_t from) {
    const unsigned char* const data = reinterpret_cast<const unsigned char*>(text.data());
    const size_t size = text.size();
    size_t at = from;
#if RAYOMD_TEXT_SSE2
    constexpr size_t kBytes = Detail::ClassByteCount<Mask>();
    static_assert(kBytes <= 16, "FindByteClass compares each byte of the classes");
    if (size >= 16) {
        for (; at + 16 <= size; at += 16) {
            const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + at));
            const unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(
                Detail::MatchClassBytes<Mask>(chunk, std::make_index_sequence<kBytes>())));
            if (bits != 0) return at + Detail::LowestBit(bits);
        }
        if (at == size) return size;
        // The last bytes: the text's last 16, without those already read.
        const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + size - 16));
        const unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(
            Detail::MatchClassBytes<Mask>(chunk, std::make_index_sequence<kBytes>()))) >> (at + 16 - size);
        return bits != 0 ? at + Detail::LowestBit(bits) : size;
    }
#endif
    const auto& classes = Detail::kByteClasses;
    while (at < size && (classes[data[at]] & Mask) == 0) at++;
    return at;
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
// code becomes its base letter (Latin Extended-A), the dash or space it looks like, or '?'.
// Returns how many did.
size_t TranscodeToWinAnsiLossy(std::string_view utf8, std::string& out);
// The renderer and fonts a document gets, as --bench reports it: "standard-font-ascii",
// "standard-font-winansi" (Latin text in the standard fonts), or "unicode-embedded-font".
const char* RendererPathName(std::string_view text);
// Transcoded text back to UTF-8, for link targets, image paths and formulas.
std::string WinAnsiToUtf8(std::string_view winAnsi);
// The WinAnsiEncoding code of a code point from U+0080 on, or -1 when it has none.
int WinAnsiCode(uint32_t codePoint);

// The scalar value of the UTF-8 sequence at text[at] and its length in bytes, or false when it
// is not well formed: overlong, a surrogate, beyond U+10FFFF or cut short.
inline bool DecodeUtf8(std::string_view text, size_t at, uint32_t& codePoint, size_t& length) {
    const unsigned char lead = static_cast<unsigned char>(text[at]);
    uint32_t minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        codePoint = lead & 0x1Fu;
        minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        codePoint = lead & 0x0Fu;
        minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        codePoint = lead & 0x07u;
        minimum = 0x10000;
    } else {
        return false;
    }
    if (at + length > text.size()) return false;
    for (size_t index = 1; index < length; index++) {
        const unsigned char next = static_cast<unsigned char>(text[at + index]);
        if ((next & 0xC0u) != 0x80u) return false;
        codePoint = (codePoint << 6) | (next & 0x3Fu);
    }
    return codePoint >= minimum && codePoint <= 0x10FFFF && !(codePoint >= 0xD800 && codePoint <= 0xDFFF);
}

} // namespace RayoMd::Text
