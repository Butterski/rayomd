// SPDX-License-Identifier: Apache-2.0

// Word bounds and advance sums for the standard renderer's wrapping (word_bounds.h). A step takes
// 64 bytes: a bit for each white-space byte, then the bits where that changes, which are where
// words start and end, written as positions; so a paragraph costs a few branches a step instead
// of one that mispredicts at the end of each word. The advances are gathered eight at a time and
// summed in the vector. Every load ends inside the text: the last step's loads end at its end and
// overlap the bytes before, so no text is copied and no byte past it read.

#include "word_bounds.h"

#include "../common/compiler.h"

#include <atomic>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOMD_WORDS_SSE2 1
// AVX2 where GCC or Clang can compile it for one function: the build stays baseline x86-64 and
// the CPU is asked once (UseAvx2), as png_filter.cpp does.
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define RAYOMD_WORDS_AVX2 1
#include "../common/cpu_features.h"
#define RAYOMD_AVX2 __attribute__((target("avx2")))
#endif
#endif

namespace TinyPdf::Internal {

namespace {

inline bool IsWordBlank(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline unsigned LowestBit(uint64_t bits) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_ctzll(bits));
#else
    unsigned bit = 0;
    for (; (bits & 1) == 0; bits >>= 1) bit++;
    return bit;
#endif
}

// The positions of the set bits of `changes`, plus `base`, at bounds[k]; returns the new k.
inline size_t FlattenExact(uint32_t* bounds, size_t k, uint64_t changes, uint32_t base, size_t = 64) {
    for (; changes != 0; changes &= changes - 1) bounds[k++] = base + LowestBit(changes);
    return k;
}

// A bit for each white-space byte of text[0, n), n at most 64, and every bit from n on: a byte at
// a time.
inline uint64_t BlankBytes(const unsigned char* bytes, size_t n) {
    uint64_t blank = 0;
    for (size_t i = 0; i < n; i++) blank |= static_cast<uint64_t>(IsWordBlank(bytes[i])) << i;
    return n < 64 ? blank | ~uint64_t(0) << n : blank;
}

// The words of text[0, n) from `mask` (a bit for each white-space byte of the 64 at a pointer)
// and `tail` (the same for text[base, n), the bits from n - base on set): before the text counts
// as white space, and so does its end. `flatten` writes the positions of a step's changes, of
// which only the first `bits` can be set (FlattenExact's contract, and up to kWordBoundsSlack
// entries past them). Returns the number of words.
template <typename Mask, typename Tail, typename Flatten>
RAYOMD_HOT_INLINE size_t Bounds(const char* text, size_t n, uint32_t* bounds, Mask mask, Tail tail, Flatten flatten) {
    uint64_t carry = 1;
    size_t k = 0;
    size_t base = 0;
    for (; base + 64 <= n; base += 64) {
        const uint64_t blank = mask(text + base);
        const uint64_t changes = blank ^ (blank << 1 | carry);
        carry = blank >> 63;
        k = flatten(bounds, k, changes, static_cast<uint32_t>(base), 64);
    }
    if (base < n) {
        const uint64_t blank = tail(text, base, n);
        k = flatten(bounds, k, blank ^ (blank << 1 | carry), static_cast<uint32_t>(base), n - base + 1);
    } else if (carry == 0) {
        bounds[k++] = static_cast<uint32_t>(n);     // the end of a word that ends with the text
    }
    return k / 2;
}

inline void SumScalar(const unsigned char* bytes, size_t n, const uint16_t* advances, uint32_t* prefix) {
    uint32_t sum = prefix[0];
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const uint32_t a = advances[bytes[i]], b = advances[bytes[i + 1]];
        const uint32_t c = advances[bytes[i + 2]], d = advances[bytes[i + 3]];
        prefix[i + 1] = sum += a;
        prefix[i + 2] = sum += b;
        prefix[i + 3] = sum += c;
        prefix[i + 4] = sum += d;
    }
    for (; i < n; i++) prefix[i + 1] = sum += advances[bytes[i]];
}

#if RAYOMD_WORDS_SSE2
inline uint64_t BlankLanes(__m128i v) {
    const __m128i spaces = _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8(' ')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\t')));
    const __m128i breaks = _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('\r')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\n')));
    return static_cast<uint32_t>(_mm_movemask_epi8(_mm_or_si128(spaces, breaks)));
}

inline uint64_t Blank16(const char* at) {
    return BlankLanes(_mm_loadu_si128(reinterpret_cast<const __m128i*>(at)));
}

inline uint64_t BlankMaskSse2(const char* at) {
    return Blank16(at) | Blank16(at + 16) << 16 | Blank16(at + 32) << 32 | Blank16(at + 48) << 48;
}

// text[base, n), 1 to 63 bytes of a text of at least 16: 16 bytes a load, the last one ending at
// n (over bytes already read, which give the same bits); or one 64-byte step ending at n.
inline uint64_t TailSse2(const char* text, size_t base, size_t n) {
    const size_t rest = n - base;
    if (n >= 64) return BlankMaskSse2(text + n - 64) >> (64 - rest) | ~uint64_t(0) << rest;
    uint64_t blank = Blank16(text + n - 16) << (n - 16);
    for (size_t at = 0; at + 16 < n; at += 16) blank |= Blank16(text + at) << at;
    return blank | ~uint64_t(0) << rest;
}
#endif

// ---- wide text -----------------------------------------------------------------------------------

inline bool IsWideBlank(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

inline bool IsHighSurrogate(uint32_t unit) { return unit >= 0xD800 && unit <= 0xDBFF; }
inline bool IsLowSurrogate(uint32_t unit) { return unit >= 0xDC00 && unit <= 0xDFFF; }

// The units of text[i, end) one at a time into prefix[i + 1, end + 1) from `sum`, and their white
// space into `blank` from bit i - base: a surrogate pair's code point at its first unit, nothing
// at its second, which may lie at `end` or past it (then `pairEnd` says so for the next call); a
// unit the cache has no entry for from `width`. Returns the sum.
inline uint32_t WideUnitsScalar(const wchar_t* text, size_t n, size_t i, size_t end, const uint16_t* cache,
    WideWidth width, const void* context, uint32_t* prefix, uint32_t sum, uint64_t& blank, size_t base, bool& pairEnd) {
    if (pairEnd && i < end) {           // the second unit of a pair the call before began
        prefix[i + 1] = sum;
        i++;
        pairEnd = false;
    }
    for (; i < end; i++) {
        const uint32_t unit = static_cast<uint16_t>(text[i]);
        blank |= static_cast<uint64_t>(IsWideBlank(text[i])) << (i - base);
        uint32_t units;
        if (IsHighSurrogate(unit) && i + 1 < n && IsLowSurrogate(static_cast<uint16_t>(text[i + 1]))) {
            units = width(context, 0x10000 + ((unit - 0xD800) << 10) + (static_cast<uint16_t>(text[i + 1]) - 0xDC00));
            prefix[i + 1] = sum += units;
            if (i + 1 == end) {
                pairEnd = true;
                return sum;
            }
            i++;
            prefix[i + 1] = sum;
            continue;
        }
        units = cache[unit];
        if (units == 0) units = width(context, unit);
        prefix[i + 1] = sum += units;
    }
    return sum;
}

#if RAYOMD_WORDS_AVX2
// A byte is white space when it equals the entry of its low four bits: ' ' for 0, the byte
// itself for '\t', '\n' and '\r', 0x80 (which no byte below 0x80 equals) for the others; bytes
// from 0x80 on look up 0.
RAYOMD_AVX2 inline __m256i BlankTable() {
    return _mm256_setr_epi8(' ', -128, -128, -128, -128, -128, -128, -128, -128, '\t', '\n', -128, -128, '\r', -128,
        -128, ' ', -128, -128, -128, -128, -128, -128, -128, -128, '\t', '\n', -128, -128, '\r', -128, -128);
}

RAYOMD_AVX2 inline uint64_t Blank32(const char* at) {
    const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(at));
    return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_shuffle_epi8(BlankTable(), v))));
}

RAYOMD_AVX2 inline uint64_t Blank64(const char* at) {
    return Blank32(at) | Blank32(at + 32) << 32;
}

// TailSse2 in 32-byte loads; texts of 16 to 31 bytes in two 16-byte ones.
RAYOMD_AVX2 inline uint64_t TailAvx2(const char* text, size_t base, size_t n) {
    const size_t rest = n - base;
    uint64_t blank;
    if (n >= 64) {
        blank = Blank64(text + n - 64) >> (64 - rest);
    } else if (n >= 32) {
        blank = Blank32(text) | Blank32(text + n - 32) << (n - 32);
    } else {
        const __m128i table = _mm256_castsi256_si128(BlankTable());
        const __m128i first = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text));
        const __m128i last = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text + n - 16));
        blank = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(first, _mm_shuffle_epi8(table, first)))) |
            static_cast<uint64_t>(static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(last, _mm_shuffle_epi8(table, last)))))
                << (n - 16);
    }
    return blank | ~uint64_t(0) << rest;
}

// The positions of the set bits of each byte value, in order, and their count.
struct BitPositions {
    uint8_t at[256][8];
    uint8_t count[256];
};

constexpr BitPositions MakeBitPositions() {
    BitPositions table{};
    for (unsigned byte = 0; byte < 256; byte++) {
        unsigned count = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            if (byte >> bit & 1) table.at[byte][count++] = static_cast<uint8_t>(bit);
        }
        table.count[byte] = static_cast<uint8_t>(count);
    }
    return table;
}

alignas(64) constexpr BitPositions kBitPositions = MakeBitPositions();

// The positions of a step's changes a byte of them at a time: the byte's positions from the table,
// widened, offset and stored as eight entries whatever its count, so no branch depends on where
// words end. Eight instructions a byte, against five an edge one by one (a step of ordinary text
// has about twenty).
RAYOMD_AVX2 inline size_t FlattenTable(uint32_t* bounds, size_t k, uint64_t changes, uint32_t base, size_t bits) {
    uint32_t* at = bounds + k;
    __m256i offset = _mm256_set1_epi32(static_cast<int>(base));
    for (size_t bit = 0; bit < bits; bit += 8) {
        const unsigned byte = static_cast<unsigned>(changes >> bit) & 0xFF;
        const __m256i positions =
            _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(kBitPositions.at[byte])));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(at), _mm256_add_epi32(positions, offset));
        at += kBitPositions.count[byte];
        offset = _mm256_add_epi32(offset, _mm256_set1_epi32(8));
    }
    return static_cast<size_t>(at - bounds);
}

// The sums of bytes[at, at + 8) on `before` (the sum of the bytes before them, in every lane),
// stored at prefix + at + 1: gathered as 32 bits at twice the byte (the advance and the next,
// which the mask drops), summed across the lanes in three additions.
RAYOMD_AVX2 inline __m256i SumStep(const unsigned char* bytes, size_t at, const uint16_t* advances, uint32_t* prefix,
    __m256i before) {
    const __m256i index = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes + at)));
    __m256i units = _mm256_and_si256(
        _mm256_i32gather_epi32(reinterpret_cast<const int*>(advances), index, 2), _mm256_set1_epi32(0xFFFF));
    units = _mm256_add_epi32(units, _mm256_slli_si256(units, 4));
    units = _mm256_add_epi32(units, _mm256_slli_si256(units, 8));
    const __m256i lane = _mm256_shuffle_epi32(units, 0xFF);
    units = _mm256_add_epi32(units, _mm256_permute2x128_si256(lane, lane, 0x08));
    units = _mm256_add_epi32(units, before);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(prefix + at + 1), units);
    return units;
}

// Eight advances a step; the last eight bytes again on the sum before them, which leaves the
// sums already written as they were. At least eight bytes, or none.
RAYOMD_AVX2 inline void SumAvx2(const unsigned char* bytes, size_t n, const uint16_t* advances, uint32_t* prefix) {
    if (n < 8) {
        SumScalar(bytes, n, advances, prefix);
        return;
    }
    __m256i sum = _mm256_set1_epi32(static_cast<int>(prefix[0]));
    size_t i = 0;
    for (; i + 8 <= n; i += 8) sum = _mm256_permutevar8x32_epi32(SumStep(bytes, i, advances, prefix, sum), _mm256_set1_epi32(7));
    if (i < n) SumStep(bytes, n - 8, advances, prefix, _mm256_set1_epi32(static_cast<int>(prefix[n - 8])));
}

// Text of at least 16 bytes.
RAYOMD_AVX2 size_t MeasureWordsAvx2(const char* text, size_t n, size_t summed, const uint16_t* advances,
    uint32_t* prefix, uint32_t* bounds) {
    prefix[0] = 0;
    const size_t words = Bounds(text, n, bounds, [](const char* at) RAYOMD_AVX2 { return Blank64(at); },
        [](const char* from, size_t base, size_t end) RAYOMD_AVX2 { return TailAvx2(from, base, end); },
        [](uint32_t* out, size_t k, uint64_t changes, uint32_t base, size_t bits) RAYOMD_AVX2 {
            return FlattenTable(out, k, changes, base, bits);
        });
    SumAvx2(reinterpret_cast<const unsigned char*>(text), summed, advances, prefix);
    return words;
}

RAYOMD_AVX2 void SumAdvancesAvx2(const char* text, size_t n, const uint16_t* advances, uint32_t* prefix) {
    SumAvx2(reinterpret_cast<const unsigned char*>(text), n, advances, prefix);
}

// Eight code units widened to 32 bits.
RAYOMD_AVX2 inline __m256i LoadWide8(const wchar_t* at) {
    if constexpr (sizeof(wchar_t) == 4) return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(at));
    else return _mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(at)));
}

// Eight units a step: their white space as eight bits, their widths gathered from the cache, a
// step with a surrogate or a width not cached yet one unit at a time (WideUnitsScalar), the
// sums stored. Steps of 64 units, their edges written from the table as MeasureWords does; the
// last units under eight one at a time.
RAYOMD_AVX2 size_t MeasureWideAvx2(const wchar_t* text, size_t n, const uint16_t* cache, WideWidth width,
    const void* context, uint32_t* prefix, uint32_t* bounds) {
    const int* const table = reinterpret_cast<const int*>(cache);
    prefix[0] = 0;
    uint32_t sum = 0;
    bool pairEnd = false;
    uint64_t carry = 1;
    size_t k = 0;
    for (size_t base = 0; base < n; base += 64) {
        const size_t stop = n - base < 64 ? n : base + 64;
        uint64_t blank = 0;
        size_t i = base;
        for (; i + 8 <= stop; i += 8) {
            const __m256i units = LoadWide8(text + i);
            const __m256i spaces = _mm256_or_si256(
                _mm256_or_si256(_mm256_cmpeq_epi32(units, _mm256_set1_epi32(' ')), _mm256_cmpeq_epi32(units, _mm256_set1_epi32('\t'))),
                _mm256_or_si256(_mm256_cmpeq_epi32(units, _mm256_set1_epi32('\r')), _mm256_cmpeq_epi32(units, _mm256_set1_epi32('\n'))));
            const __m256i index = _mm256_and_si256(units, _mm256_set1_epi32(0xFFFF));
            const __m256i widths = _mm256_and_si256(_mm256_i32gather_epi32(table, index, 2), _mm256_set1_epi32(0xFFFF));
            const __m256i surrogate =
                _mm256_cmpeq_epi32(_mm256_and_si256(index, _mm256_set1_epi32(0xF800)), _mm256_set1_epi32(0xD800));
            const __m256i odd = _mm256_or_si256(surrogate, _mm256_cmpeq_epi32(widths, _mm256_setzero_si256()));
            if (pairEnd || !_mm256_testz_si256(odd, odd)) {
                sum = WideUnitsScalar(text, n, i, i + 8, cache, width, context, prefix, sum, blank, base, pairEnd);
                continue;
            }
            blank |= static_cast<uint64_t>(static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(spaces)))) << (i - base);
            __m256i step = _mm256_add_epi32(widths, _mm256_slli_si256(widths, 4));
            step = _mm256_add_epi32(step, _mm256_slli_si256(step, 8));
            const __m256i lane = _mm256_shuffle_epi32(step, 0xFF);
            step = _mm256_add_epi32(step, _mm256_permute2x128_si256(lane, lane, 0x08));
            step = _mm256_add_epi32(step, _mm256_set1_epi32(static_cast<int>(sum)));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(prefix + i + 1), step);
            sum = static_cast<uint32_t>(_mm256_extract_epi32(step, 7));
        }
        if (i < stop) sum = WideUnitsScalar(text, n, i, stop, cache, width, context, prefix, sum, blank, base, pairEnd);
        const size_t rest = stop - base;
        if (rest < 64) blank |= ~uint64_t(0) << rest;
        const uint64_t changes = blank ^ (blank << 1 | carry);
        carry = blank >> 63;
        k = FlattenTable(bounds, k, changes, static_cast<uint32_t>(base), rest < 64 ? rest + 1 : 64);
    }
    if (n % 64 == 0 && carry == 0) bounds[k++] = static_cast<uint32_t>(n);   // a word that ends with the text
    return k / 2;
}

// The CPU's answer, asked once: -1 until then. Every thread that asks gets the same answer, so a
// relaxed store and load are enough, and no guard is taken on each call.
std::atomic<signed char> avx2State{-1};

RAYOMD_NOINLINE bool AskAvx2() {
    const bool avx2 = RayoMd::Cpu::HasAvx2();
    avx2State.store(avx2 ? 1 : 0, std::memory_order_relaxed);
    return avx2;
}

inline bool UseAvx2() {
    const signed char state = avx2State.load(std::memory_order_relaxed);
    return state > 0 || (state < 0 && AskAvx2());
}
#endif

} // namespace

RAYOMD_NOINLINE size_t MeasureWordsPortable(const char* text, size_t n, size_t summed, const uint16_t* advances,
    uint32_t* prefix, uint32_t* bounds) {
    const unsigned char* const bytes = reinterpret_cast<const unsigned char*>(text);
    prefix[0] = 0;
    const size_t words = Bounds(text, n, bounds,
        [bytes, text](const char* at) { return BlankBytes(bytes + (at - text), 64); },
        [bytes](const char*, size_t base, size_t end) { return BlankBytes(bytes + base, end - base); }, FlattenExact);
    SumScalar(bytes, summed, advances, prefix);
    return words;
}

#if RAYOMD_WORDS_SSE2
// Text of at least 16 bytes.
RAYOMD_NOINLINE size_t MeasureWordsSse2(const char* text, size_t n, size_t summed, const uint16_t* advances,
    uint32_t* prefix, uint32_t* bounds) {
    prefix[0] = 0;
    const size_t words = Bounds(text, n, bounds, BlankMaskSse2, TailSse2, FlattenExact);
    SumScalar(reinterpret_cast<const unsigned char*>(text), summed, advances, prefix);
    return words;
}
#endif

size_t MeasureWords(const char* text, size_t n, size_t summed, const uint16_t* advances, uint32_t* prefix,
    uint32_t* bounds) {
#if RAYOMD_WORDS_SSE2
    if (n >= 16) {
#if RAYOMD_WORDS_AVX2
        if (UseAvx2()) return MeasureWordsAvx2(text, n, summed, advances, prefix, bounds);
#endif
        return MeasureWordsSse2(text, n, summed, advances, prefix, bounds);
    }
#endif
    return MeasureWordsPortable(text, n, summed, advances, prefix, bounds);
}

RAYOMD_NOINLINE size_t MeasureWideWordsPortable(const wchar_t* text, size_t n, const uint16_t* cache, WideWidth width,
    const void* context, uint32_t* prefix, uint32_t* bounds) {
    prefix[0] = 0;
    uint32_t sum = 0;
    bool pairEnd = false;
    uint64_t carry = 1;
    size_t k = 0;
    for (size_t base = 0; base < n; base += 64) {
        const size_t stop = n - base < 64 ? n : base + 64;
        uint64_t blank = 0;
        sum = WideUnitsScalar(text, n, base, stop, cache, width, context, prefix, sum, blank, base, pairEnd);
        const size_t rest = stop - base;
        if (rest < 64) blank |= ~uint64_t(0) << rest;
        k = FlattenExact(bounds, k, blank ^ (blank << 1 | carry), static_cast<uint32_t>(base));
        carry = blank >> 63;
    }
    if (n % 64 == 0 && carry == 0) bounds[k++] = static_cast<uint32_t>(n);
    return k / 2;
}

size_t MeasureWideWords(const wchar_t* text, size_t n, const uint16_t* cache, WideWidth width, const void* context,
    uint32_t* prefix, uint32_t* bounds) {
#if RAYOMD_WORDS_AVX2
    if (n >= 16 && UseAvx2()) return MeasureWideAvx2(text, n, cache, width, context, prefix, bounds);
#endif
    return MeasureWideWordsPortable(text, n, cache, width, context, prefix, bounds);
}

void SumAdvances(const char* text, size_t n, const uint16_t* advances, uint32_t* prefix) {
#if RAYOMD_WORDS_AVX2
    if (n >= 8 && UseAvx2()) {
        SumAdvancesAvx2(text, n, advances, prefix);
        return;
    }
#endif
    SumScalar(reinterpret_cast<const unsigned char*>(text), n, advances, prefix);
}

} // namespace TinyPdf::Internal
