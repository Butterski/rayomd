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

} // namespace TinyPdf::Internal
