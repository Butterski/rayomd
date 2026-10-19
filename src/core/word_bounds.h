// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>

// The words of a paragraph and the advances of its bytes, for the standard renderer's wrapping:
// one pass finds where every word starts and ends (AVX2 where the CPU has it, else SSE2), another
// sums the advances of all bytes, so a word's width is one subtraction and no loop ends at each
// word.
namespace TinyPdf::Internal {

// Entries MeasureWords may write into `bounds` past the ones it returns.
constexpr size_t kWordBoundsSlack = 8;

// The words of text[0, n), the runs of bytes other than ' ', '\t', '\r' and '\n' (the bytes the
// standard fonts' word advances make 0), each as its first byte and one past its last, in order,
// into `bounds` (two entries a word; it holds n + 1 + kWordBoundsSlack). Also prefix[0] = 0 and
// prefix[i + 1] = prefix[i] + advances[text[i]] for i in [0, summed), modulo 2^32 (summed <= n): the units of
// text[a, b) are prefix[b] - prefix[a] while below 2^32, which words of fewer than
// kPrefixWordBytes are with advances below 1,024. n is below 2^32. Returns the number of words. Texts under 16 bytes are
// read a byte at a time: the callers measure those inline (a call costs more than the work).
size_t MeasureWords(const char* text, size_t n, size_t summed, const uint16_t* advances, uint32_t* prefix,
    uint32_t* bounds);
size_t MeasureWordsPortable(const char* text, size_t n, size_t summed, const uint16_t* advances, uint32_t* prefix,
    uint32_t* bounds);
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
size_t MeasureWordsSse2(const char* text, size_t n, size_t summed, const uint16_t* advances, uint32_t* prefix,
    uint32_t* bounds);
#endif

// The prefix sums of MeasureWords alone, for text[0, n), from the prefix[0] given.
void SumAdvances(const char* text, size_t n, const uint16_t* advances, uint32_t* prefix);

constexpr size_t kPrefixWordBytes = size_t(1) << 22;

// The units of a code point MeasureWideWords finds no cached width for.
using WideWidth = uint32_t (*)(const void* context, uint32_t codepoint);

// MeasureWords for wide text[0, n) (UTF-16 code units, one a wchar_t), words being runs of units
// other than L' ', L'\t', L'\r' and L'\n', and prefix[i + 1] = prefix[i] + the units of text[i]:
// cache[text[i] & 0xFFFF] when that is not 0 (`cache` has 65,537 entries readable: the vector code
// reads two at a time), else width(context, unit); a surrogate pair counts width(context, its code
// point) at its first unit and nothing at its second. `bounds` holds n + 1 + kWordBoundsSlack. n
// is below 2^32. Returns the number of words.
size_t MeasureWideWords(const wchar_t* text, size_t n, const uint16_t* cache, WideWidth width, const void* context,
    uint32_t* prefix, uint32_t* bounds);
size_t MeasureWideWordsPortable(const wchar_t* text, size_t n, const uint16_t* cache, WideWidth width,
    const void* context, uint32_t* prefix, uint32_t* bounds);

// A run of a line of highlighted code: where it ends, and the colour of its characters.
struct ColorRun {
    uint32_t end;
    uint8_t color;
};

// The runs of one colour of a line of code text[0, n), whose characters have the classes `classes`
// (each below classCount, at most 16) and the colours colorOf[class] (each below 255): a run ends where a character
// other than ' ' has another colour than the one before it that is not ' '; spaces take the colour
// of the run they are in, leading ones that of the first run. The last run ends at n (colour 0 for
// a line of spaces). Into `runs`, which holds n + 1; returns how many. AVX2 sixteen characters a
// step (their colours looked up, carried over spaces, compared with the one before), else one at
// a time.
size_t SplitColorRuns(const char* text, size_t n, const uint8_t* classes, const uint8_t* colorOf, size_t classCount, ColorRun* runs);
size_t SplitColorRuns(const wchar_t* text, size_t n, const uint8_t* classes, const uint8_t* colorOf, size_t classCount, ColorRun* runs);
template <typename Char>
size_t SplitColorRunsScalar(const Char* text, size_t n, const uint8_t* classes, const uint8_t* colorOf, ColorRun* runs,
    size_t from = 0, uint8_t color = 0xFF, size_t count = 0) {
    for (size_t at = from; at < n; at++) {
        if (text[at] == ' ') continue;
        const uint8_t next = colorOf[classes[at]];
        if (next == color) continue;
        if (color != 0xFF) runs[count++] = { static_cast<uint32_t>(at), color };
        color = next;
    }
    runs[count++] = { static_cast<uint32_t>(n), color == 0xFF ? static_cast<uint8_t>(0) : color };
    return count;
}

} // namespace TinyPdf::Internal
