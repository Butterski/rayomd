// SPDX-License-Identifier: Apache-2.0

// --black-text: colour operators of finished content as greys, in place (print_colors.h).
// Page content is mostly text in strings, so a SIMD pass (AVX2 where the CPU has it, else SSE2)
// takes 64 bytes a step and stops only at the bytes that open or close a string or end a colour
// operator. RayoMD writes few colours, each many times: an operator whose bytes and operands'
// bytes were met before becomes what they became then (OperandMemo); only a new one is read
// backwards and parsed (Operator).

#include "print_colors.h"

#include "../common/text_utils.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOMD_PRINT_SSE2 1
// AVX2 (with BMI's bit operations) where GCC or Clang can compile it for one function: the build
// stays baseline x86-64 and the CPU is asked once (UseAvx2), as png_filter.cpp does.
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define RAYOMD_PRINT_AVX2 1
#define RAYOMD_AVX2 __attribute__((target("avx2,bmi")))
#endif
#endif

namespace TinyPdf::Internal {

namespace {

enum : uint8_t { kSpace = 1, kDelimiter = 2, kNumber = 4, kRun = kSpace | kNumber };

constexpr std::array<uint8_t, 256> MakeClasses() {
    std::array<uint8_t, 256> classes{};
    for (const char c : { ' ', '\n', '\r', '\t', '\f', '\0' }) classes[static_cast<unsigned char>(c)] = kSpace;
    for (const char c : { '(', ')', '<', '>', '[', ']', '{', '}', '/', '%' }) classes[static_cast<unsigned char>(c)] = kDelimiter;
    for (const char c : { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '-', '+' }) {
        classes[static_cast<unsigned char>(c)] = kNumber;
    }
    return classes;
}
constexpr std::array<uint8_t, 256> kClasses = MakeClasses();

uint8_t ClassOf(char c) {
    return kClasses[static_cast<unsigned char>(c)];
}

// Exactly `count` numbers from `at` up to `end`, the white space before an operator (so every
// digit run stops before it), into `values` in millionths (digits past the sixth decimal dropped):
// an optional sign, at most 12 digits, an optional point and digits. `at` is a number's first byte.
bool ParseOperands(const char* at, const char* end, int count, int64_t* values) {
    static constexpr int64_t kScales[] = { 1000000, 100000, 10000, 1000, 100, 10, 1 };
    for (int read = 0; read < count; read++) {
        const bool negative = *at == '-';
        if (*at == '-' || *at == '+') at++;
        const char* const first = at;
        uint64_t whole = 0;   // wraps past 19 digits, refused below
        for (unsigned digit; (digit = static_cast<unsigned>(*at - '0')) <= 9; at++) whole = whole * 10 + digit;
        bool digits = at != first;
        if (at - first > 12) return false;
        int64_t fraction = 0;
        int decimals = 0;
        if (*at == '.') {
            for (unsigned digit; (digit = static_cast<unsigned>(*++at - '0')) <= 9; digits = true) {
                if (decimals < 6) {
                    fraction = fraction * 10 + digit;
                    decimals++;
                }
            }
        }
        if (!digits || (ClassOf(*at) & kSpace) == 0) return false;
        const int64_t value = static_cast<int64_t>(whole) * 1000000 + fraction * kScales[decimals];
        values[read] = negative ? -value : value;
        while (at < end && (ClassOf(*at) & kSpace) != 0) at++;
    }
    return at == end;
}

constexpr ptrdiff_t kWindow = 32;   // the bytes a memo entry keys: an operator's, its operands' and the next

// What colour operators became, by their last bytes. An operator's outcome depends only on its
// bytes, the byte after it and its operands' back to the byte before them, the last that is no
// number or white space, and on that byte only by being a delimiter or not: an operator whose
// `length` bytes with the byte after it, ending at `tail`, and the byte before them are an
// entry's becomes what that entry's did, the kWindow bytes before `tail` XOR the entry's `delta`.
// Two entries for each hash of the last 8 bytes, the one met last first.
class OperandMemo {
public:
    // The delta of the operator whose next byte ends at `tail`, more than kWindow bytes from the
    // content's start (the window and the byte before it are read), when one was met before, else
    // nullptr. Simd::Same gives a bit for each byte of a window equal to an entry's key.
    template <class Simd>
    RAYOMD_HOT_INLINE const char* Find(const char* tail) {
        Entry* const set = sets[Set(tail)];
        if (set[0].Matches<Simd>(tail)) return set[0].delta;
        if (!set[1].Matches<Simd>(tail)) return nullptr;
        Swap(set);
        return set[0].delta;
    }

    // The outcome of the operator whose next byte ends at `tail`: its window `was` before, what it
    // is now, the `length` bytes of it, its operands and the next at the end (at most kWindow),
    // the class of the byte before them.
    void Keep(const char* was, const char* tail, size_t length, uint8_t before) {
        Entry* const set = sets[Set(was + kWindow)];
        set[1] = set[0];
        Entry& entry = set[0];
        memcpy(entry.key, was, kWindow);
        for (size_t at = 0; at < kWindow; at += 8) {
            uint64_t old;
            uint64_t now;
            memcpy(&old, was + at, 8);
            memcpy(&now, tail - kWindow + at, 8);
            now ^= old;
            memcpy(entry.delta + at, &now, 8);
        }
        entry.ignored = ~(~0u << (kWindow - static_cast<ptrdiff_t>(length)));
        entry.length = static_cast<uint8_t>(length);
        entry.before = before;
    }

private:
    struct Entry {
        char key[kWindow];     // the window's bytes before
        char delta[kWindow];   // XOR what they became: zero but for the operator and its operands
        uint32_t ignored;      // the bits of the bytes that do not count, before the others; 0
        uint8_t length;        // when empty, which no window matches, as an operator ends in 'g'
        uint8_t before;        // ClassOf the byte before the bytes that count

        template <class Simd>
        RAYOMD_HOT_INLINE bool Matches(const char* tail) const {
            return before == ClassOf(tail[-static_cast<ptrdiff_t>(length) - 1]) &&
                (Simd::Same(tail - kWindow, key) | ignored) == ~0u;
        }
    };

    static size_t Set(const char* tail) {
        uint64_t last;
        memcpy(&last, tail - 8, sizeof(last));
        return static_cast<size_t>((last * 0x9E3779B97F4A7C15ull) >> 58);
    }

    static RAYOMD_COLD void Swap(Entry* set) {
        const Entry first = set[0];
        set[0] = set[1];
        set[1] = first;
    }

    Entry sets[64][2];
};

// The thread's memo, made on first use: an outcome depends only on the bytes it is kept by, so it
// holds for every stream and build after it, and a preview built again on each edit finds its
// colours there. On the heap, so threads that print nothing grow by a pointer only.
OperandMemo& ThreadMemo() {
    static thread_local std::unique_ptr<OperandMemo> memo;
    if (!memo) memo.reset(new OperandMemo());
    return *memo;
}

// The colour operator whose last byte, 'g' or 'G', is at `at` with white space or a delimiter
// after it, when it is one: "g", "G", "rg" or "RG" alone between white space after its operands,
// made grey: "Y g" (or "Y G") over the operator and its operands, padded with spaces, Y in
// thousandths, as RayoMD writes colours (at most three decimals), "0" below 0.5; kept in `memo`
// unless that is nullptr. Out of line: only an operator the memo has not met comes here.
RAYOMD_COLD void Operator(char* begin, char* at, char* end, OperandMemo* memo) {
    const char c = *at;
    char* const after = at + 1;
    char* op = at;
    if (op > begin && op[-1] == (c == 'g' ? 'r' : 'R')) op--;
    if (op == begin || (ClassOf(op[-1]) & kSpace) == 0) return;
    // The operands: the numbers and white space before the operator, from the token they follow.
    char* run = op - 1;
    while (run > begin && (ClassOf(run[-1]) & kRun) != 0) run--;
    char* start = run;
    if (start > begin && (ClassOf(start[-1]) & kDelimiter) == 0) {
        while (start < op && (ClassOf(*start) & kSpace) == 0) start++;   // the end of a name or operator, as "x1"
    }
    while ((ClassOf(*start) & kSpace) != 0) start++;
    char gray[8];      // "0.123 g" at most: the operator and its operands have at least 8 for
    size_t size = 0;   // "r g b rg" and 3 for "v g", which takes this only below 0.5
    int64_t values[3];
    if (start == op) {
        // no operands
    } else if (op != at) {
        if (ParseOperands(start, op, 3, values)) {
            int64_t billionths = static_cast<int64_t>(299 * static_cast<uint64_t>(values[0]) +
                587 * static_cast<uint64_t>(values[1]) + 114 * static_cast<uint64_t>(values[2]));
            if (billionths < 0) billionths = 0;
            const int64_t milli = (billionths + 500000) / 1000000;
            gray[size++] = milli >= 1000 ? '1' : '0';
            if (milli >= 500 && milli < 1000) {
                gray[size++] = '.';
                gray[size++] = static_cast<char>('0' + milli / 100);
                if (milli % 100 != 0) {
                    gray[size++] = static_cast<char>('0' + milli / 10 % 10);
                    if (milli % 10 != 0) gray[size++] = static_cast<char>('0' + milli % 10);
                }
            }
        }
    } else if (ParseOperands(start, op, 1, values) && values[0] < 500000 && !(op - start == 2 && *start == '0')) {
        gray[size++] = '0';
    }
    char was[kWindow];
    char* const tail = after + 1;
    const bool keep = memo != nullptr && after < end && tail - begin >= kWindow && run > begin && tail - run <= kWindow;
    if (keep) memcpy(was, tail - kWindow, sizeof(was));
    if (size != 0) {
        gray[size++] = ' ';
        gray[size++] = c;
        memcpy(start, gray, size);
        memset(start + size, ' ', static_cast<size_t>(after - start) - size);
    }
    if (keep) memo->Keep(was, tail, static_cast<size_t>(tail - run), ClassOf(run[-1]));
}

// A byte the scan stops at: '(' ')' '\\' '<' '>' 'g' 'G'.
bool IsStop(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u & 0xFE) == 0x28 || (u & 0xFD) == 0x3C || (u | 0x20) == 'g' || u == '\\';
}

// Byte by byte, without the memo: the reference, and the scan of streams under 64 bytes and of
// machines without SSE2.
RAYOMD_COLD void Reference(char* begin, char* end) {
    char* skip = begin;   // bytes before this one are read: an escaped byte, "<<"'s second '<'
    int depth = 0;        // the nesting of the literal string the scan is in, 0 outside
    bool hex = false;     // in a hex string
    for (char* at = begin; at < end; at++) {
        const char c = *at;
        if (!IsStop(c) || at < skip) continue;
        if (depth > 0) {
            if (c == '\\') skip = at + 2;
            else if (c == '(') depth++;
            else if (c == ')') depth--;
        } else if (hex) {
            hex = c != '>';
        } else if (c == '(') {
            depth = 1;
        } else if (c == '<') {
            if (at + 1 < end && at[1] == '<') skip = at + 2;
            else hex = true;
        } else if ((c | 0x20) == 'g' && (at + 1 == end || (ClassOf(at[1]) & (kSpace | kDelimiter)) != 0)) {
            Operator(begin, at, end, nullptr);
        }
    }
}

#if RAYOMD_PRINT_SSE2
// The stops of 64 bytes, a bit each, and a window's bytes against a memo entry's (Simd::Stops,
// Simd::Same, Simd::Apply), in SSE2 and in AVX2.
struct Sse2 {
    static __m128i Load(const char* at) {
        return _mm_loadu_si128(reinterpret_cast<const __m128i*>(at));
    }
    static uint64_t Stops16(const char* at) {
        const __m128i bytes = Load(at);
        const __m128i hit = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(_mm_and_si128(bytes, _mm_set1_epi8(static_cast<char>(0xFE))), _mm_set1_epi8(0x28)),
                         _mm_cmpeq_epi8(_mm_and_si128(bytes, _mm_set1_epi8(static_cast<char>(0xFD))), _mm_set1_epi8(0x3C))),
            _mm_or_si128(_mm_cmpeq_epi8(_mm_or_si128(bytes, _mm_set1_epi8(0x20)), _mm_set1_epi8('g')),
                         _mm_cmpeq_epi8(bytes, _mm_set1_epi8('\\'))));
        return static_cast<unsigned>(_mm_movemask_epi8(hit));
    }
    // Out of line: inlined at each of the scan's steps, four times seven compares came to 3 KB.
    static RAYOMD_NOINLINE uint64_t Stops(const char* at) {
        return Stops16(at) | Stops16(at + 16) << 16 | Stops16(at + 32) << 32 | Stops16(at + 48) << 48;
    }
    static uint32_t Same(const char* window, const char* key) {
        return static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(Load(window), Load(key)))) |
            static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(Load(window + 16), Load(key + 16)))) << 16;
    }
    static void Apply(char* window, const char* delta) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(window), _mm_xor_si128(Load(window), Load(delta)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(window + 16), _mm_xor_si128(Load(window + 16), Load(delta + 16)));
    }
};

#if RAYOMD_PRINT_AVX2
struct Avx2 {
    RAYOMD_AVX2 static __m256i Load(const char* at) {
        return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(at));
    }
    // A byte equal to its low four bits' entry in either table: '(' ')' '<' '>' 'g', '\\' 'G'. A
    // byte from 0x80 looks up 0, which it is not. No constant is the same in every byte, so the
    // compiler loads the tables where a call took them, instead of building them in three steps.
    RAYOMD_AVX2 static uint32_t Stops32(const char* at) {
        const __m256i first = _mm256_setr_epi8(-1, -1, -1, -1, -1, -1, -1, 'g', '(', ')', -1, -1, '<', -1, '>', -1,
            -1, -1, -1, -1, -1, -1, -1, 'g', '(', ')', -1, -1, '<', -1, '>', -1);
        const __m256i second = _mm256_setr_epi8(-1, -1, -1, -1, -1, -1, -1, 'G', -1, -1, -1, -1, '\\', -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, 'G', -1, -1, -1, -1, '\\', -1, -1, -1);
        const __m256i bytes = Load(at);
        return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_or_si256(
            _mm256_cmpeq_epi8(_mm256_shuffle_epi8(first, bytes), bytes), _mm256_cmpeq_epi8(_mm256_shuffle_epi8(second, bytes), bytes))));
    }
    RAYOMD_AVX2 static uint64_t Stops(const char* at) {
        return Stops32(at) | static_cast<uint64_t>(Stops32(at + 32)) << 32;
    }
    RAYOMD_AVX2 static uint32_t Same(const char* window, const char* key) {
        return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(Load(window), Load(key))));
    }
    RAYOMD_AVX2 static void Apply(char* window, const char* delta) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(window), _mm256_xor_si256(Load(window), Load(delta)));
    }
};

bool UseAvx2() {
    static const bool avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi");
    return avx2;
}
#endif

// The scan of at least 64 bytes, 64 a step (the last step overlapping the one before), each stop
// taken by the state of strings it is in: outside, in a literal string (the standard renderer's
// text, the Unicode renderer's two-byte codes: anything but '(' ')' '\\' passes) or in a hex
// string (to its '>'). Inlined into each Simd's caller, so the vector code is the caller's.
template <class Simd>
RAYOMD_HOT_INLINE void Scan(char* const begin, char* const end, OperandMemo& memo) {
    char* const stop = end - 64;            // the last step's bytes
    char* const keyed = begin + kWindow;    // from here an operator's window and the byte before it are read
    char* block = begin;                    // the stops' bits are of the 64 bytes from here
    uint64_t bits = Simd::Stops(block);
    const auto next = [&]() RAYOMD_HOT_LAMBDA -> char* {
        while (bits == 0) {
            char* const from = block + 64;
            if (from <= stop) {
                block = from;
                bits = Simd::Stops(block);
            } else {   // the last step, without the bytes the step before read
                if (block == stop) return nullptr;
                block = stop;
                bits = Simd::Stops(block) & (~uint64_t(0) << (from - stop));
            }
        }
        char* const at = block + RayoMd::Text::Detail::LowestBit64(bits);
        bits &= bits - 1;
        return at;
    };
    // The byte after `at` is read: an escaped byte, "<<"'s second '<'. When that is the next
    // step's first, the next step starts a byte later.
    const auto skipNext = [&](char* at) RAYOMD_HOT_LAMBDA {
        const auto index = static_cast<unsigned>(at - block);
        if (index < 63) bits &= ~(uint64_t(2) << index);
        else if (block < stop) block++;
    };
    for (char* at; (at = next()) != nullptr;) {
        const char c = *at;
        if (c == '(') {
            for (int depth = 1; depth > 0;) {
                if ((at = next()) == nullptr) return;
                const char inner = *at;
                if (inner == ')') depth--;
                else if (inner == '(') depth++;
                else if (inner == '\\') skipNext(at);
            }
        } else if (c == '<') {
            if (at + 1 < end && at[1] == '<') {
                skipNext(at);
            } else {
                do {
                    if ((at = next()) == nullptr) return;
                } while (*at != '>');
            }
        } else if ((c | 0x20) == 'g') {
            char* const after = at + 1;
            const char* const delta = after < end && after >= keyed ? memo.Find<Simd>(after + 1) : nullptr;
            if (delta != nullptr) Simd::Apply(after + 1 - kWindow, delta);
            else if (after == end || (ClassOf(*after) & (kSpace | kDelimiter)) != 0) Operator(begin, at, end, &memo);
        }
    }
}

RAYOMD_SHARED void ScanSse2(char* begin, char* end, OperandMemo& memo) {
    Scan<Sse2>(begin, end, memo);
}

#if RAYOMD_PRINT_AVX2
RAYOMD_AVX2 RAYOMD_SHARED void ScanAvx2(char* begin, char* end, OperandMemo& memo) {
    Scan<Avx2>(begin, end, memo);
}
#endif
#endif

} // namespace

void PrintColors(char* begin, char* end) {
#if RAYOMD_PRINT_SSE2
    if (end - begin >= 64) {
#if RAYOMD_PRINT_AVX2
        if (UseAvx2()) return ScanAvx2(begin, end, ThreadMemo());
#endif
        return ScanSse2(begin, end, ThreadMemo());
    }
#endif
    Reference(begin, end);
}

void PrintColorsScalar(char* begin, char* end) {
    Reference(begin, end);
}

#if RAYOMD_PRINT_SSE2
void PrintColorsSse2(char* begin, char* end) {
    if (end - begin >= 64) ScanSse2(begin, end, ThreadMemo());
    else Reference(begin, end);
}
#endif

void PrintColors(std::string& pdf, const std::vector<size_t>& starts, std::vector<std::string>& bands,
    std::string& cover, std::vector<std::string>& overlays) {
    if (!starts.empty()) PrintColors(pdf.data() + starts.front(), pdf.data() + pdf.size());
    for (std::string& band : bands) PrintColors(band);
    PrintColors(cover);
    for (std::string& overlay : overlays) PrintColors(overlay);
}

} // namespace TinyPdf::Internal
