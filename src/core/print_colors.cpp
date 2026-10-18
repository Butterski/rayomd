// SPDX-License-Identifier: Apache-2.0

// --black-text: colour operators of finished content as greys, in place (print_colors.h).
// Page content is mostly text in strings, so one SIMD pass finds the few bytes that open or close
// a string or end a colour operator, and an operator's operands are read backwards only then.

#include "print_colors.h"

#include "../common/text_utils.h"

#include <array>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOMD_PRINT_SSE2 1
#endif

namespace TinyPdf::Internal {

namespace {

enum : uint8_t { kSpace = 1, kDelimiter = 2, kNumber = 4 };

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

unsigned HighestBit(unsigned bits) {
#if defined(__GNUC__) || defined(__clang__)
    return 31u - static_cast<unsigned>(__builtin_clz(bits));
#else
    unsigned bit = 0;
    while (bits >>= 1) bit++;
    return bit;
#endif
}

// The first byte of the run of number bytes and white space (' ', '\n', '\r', '\t') that ends
// at `at`, not before `floor`: in SSE2 16 bytes a step backwards.
char* RunStart(char* floor, char* at) {
#if RAYOMD_PRINT_SSE2
    const __m128i zero = _mm_set1_epi8('0'), nine = _mm_set1_epi8(9), point = _mm_set1_epi8('.'),
                  minus = _mm_set1_epi8('-'), plus = _mm_set1_epi8('+'), space = _mm_set1_epi8(' '),
                  feed = _mm_set1_epi8('\n'), ret = _mm_set1_epi8('\r'), tab = _mm_set1_epi8('\t');
    while (at - floor >= 16) {
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(at - 16));
        const __m128i offset = _mm_sub_epi8(bytes, zero);
        const __m128i digit = _mm_cmpeq_epi8(_mm_min_epu8(offset, nine), offset);
        const __m128i run = _mm_or_si128(
            _mm_or_si128(_mm_or_si128(digit, _mm_cmpeq_epi8(bytes, point)),
                         _mm_or_si128(_mm_cmpeq_epi8(bytes, minus), _mm_cmpeq_epi8(bytes, plus))),
            _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(bytes, space), _mm_cmpeq_epi8(bytes, feed)),
                         _mm_or_si128(_mm_cmpeq_epi8(bytes, ret), _mm_cmpeq_epi8(bytes, tab))));
        const unsigned other = ~static_cast<unsigned>(_mm_movemask_epi8(run)) & 0xFFFFu;
        if (other != 0) return at - 16 + HighestBit(other) + 1;
        at -= 16;
    }
#endif
    while (at > floor && (ClassOf(at[-1]) & (kSpace | kNumber)) != 0) at--;
    return at;
}

// "Y g" (or "Y G") over [from, to), padded with spaces: Y, `milli` thousandths, as RayoMD writes
// colours (at most three decimals), "0" below 0.5. At most 7 bytes: the operator and its operands
// have at least 8 for "r g b rg" and 3 for "v g", which takes this only below 0.5. Written with
// overlapping fixed-size copies, not a call for each operator.
void WriteGray(char* from, char* to, int64_t milli, char op) {
    char text[32];
    memset(text, ' ', sizeof(text));
    size_t size = 0;
    if (milli < 500) {
        text[size++] = '0';
    } else if (milli >= 1000) {
        text[size++] = '1';
    } else {
        text[size++] = '0';
        text[size++] = '.';
        text[size++] = static_cast<char>('0' + milli / 100);
        if (milli % 100 != 0) {
            text[size++] = static_cast<char>('0' + milli / 10 % 10);
            if (milli % 10 != 0) text[size++] = static_cast<char>('0' + milli % 10);
        }
    }
    text[size++] = ' ';
    text[size++] = op;
    const size_t length = static_cast<size_t>(to - from);
    if (length > sizeof(text)) {
        memcpy(from, text, size);
        memset(from + size, ' ', length - size);
    } else if (length >= 16) {
        memcpy(from, text, 16);
        memcpy(to - 16, text + length - 16, 16);
    } else if (length >= 8) {
        memcpy(from, text, 8);
        memcpy(to - 8, text + length - 8, 8);
    } else if (length >= 4) {
        memcpy(from, text, 4);
        memcpy(to - 4, text + length - 4, 4);
    } else {
        for (size_t index = 0; index < length; index++) from[index] = text[index];
    }
}

// The greys of the last few "r g b" operands, by their bytes: RayoMD writes few colours, each
// many times (the body text's before every run of text), and comparing 24 bytes is cheaper than
// reading three numbers.
class OperandMemo {
public:
    // The bytes of [from, to) as the key, when they fit in it and 24 bytes from `from` are
    // readable; the bytes past `to` masked off in little-endian words (no key on other machines).
    bool Key(const char* from, const char* to, const char* end) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        return false;
#endif
        const size_t length = static_cast<size_t>(to - from);
        if (length > sizeof(key.words) || end - from < static_cast<ptrdiff_t>(sizeof(key.words))) return false;
        memcpy(key.words, from, sizeof(key.words));
        for (size_t word = 0; word < 3; word++) {
            const size_t bytes = length > word * 8 ? length - word * 8 : 0;
            if (bytes < 8) key.words[word] &= bytes == 0 ? 0 : ~uint64_t(0) >> (64 - 8 * bytes);
        }
        key.length = length;
        return true;
    }
    bool Find(int64_t& milli) const {
        for (const Entry& entry : entries) {
            if (entry.length == key.length && entry.words[0] == key.words[0] && entry.words[1] == key.words[1] &&
                entry.words[2] == key.words[2]) {
                milli = entry.milli;
                return true;
            }
        }
        return false;
    }
    void Keep(int64_t milli) {
        entries[next] = key;
        entries[next].milli = milli;
        next = (next + 1) % 4;
    }

private:
    struct Entry {
        uint64_t words[3] = { 0, 0, 0 };
        size_t length = 0;   // 0: empty, as no key has
        int64_t milli = 0;
    };
    Entry key;
    Entry entries[4];
    size_t next = 0;
};

// The colour operator whose last byte, 'g' or 'G', is at `at`, when it is one: "g", "G", "rg" or
// "RG" alone between white space after its operands, which start at or after `floor`, made grey.
// Returns what follows it, the next operands' floor, else nullptr. Out of line: the scan calls it
// for each 'g' outside strings only, a few times a page.
RAYOMD_NOINLINE char* Operator(char* floor, char* at, char* end, OperandMemo& memo) {
    const char c = *at;
    char* const after = at + 1;
    if (after < end && (ClassOf(*after) & (kSpace | kDelimiter)) == 0) return nullptr;
    char* op = at;
    const bool rgb = op > floor && op[-1] == (c == 'g' ? 'r' : 'R');
    if (rgb) op--;
    if (op == floor || (ClassOf(op[-1]) & kSpace) == 0) return nullptr;
    // The operands: the numbers and white space before the operator, from the token they follow.
    char* start = RunStart(floor, op - 1);
    if (start > floor && (ClassOf(start[-1]) & kDelimiter) == 0) {
        while (start < op && (ClassOf(*start) & kSpace) == 0) start++;   // the end of a name or operator, as "x1"
    }
    while ((ClassOf(*start) & kSpace) != 0) start++;
    if (start == op) return after;
    int64_t values[3];
    if (rgb) {
        int64_t milli = 0;
        const bool keyed = memo.Key(start, op, end);
        if (!keyed || !memo.Find(milli)) {
            if (!ParseOperands(start, op, 3, values)) return after;
            int64_t billionths = 299 * values[0] + 587 * values[1] + 114 * values[2];
            if (billionths < 0) billionths = 0;
            milli = (billionths + 500000) / 1000000;
            if (keyed) memo.Keep(milli);
        }
        WriteGray(start, after, milli, c);
    } else if (ParseOperands(start, op, 1, values) && values[0] < 500000 && !(op - start == 2 && *start == '0')) {
        WriteGray(start, after, 0, c);
    }
    return after;
}

// A byte the scan stops at: '(' ')' '\\' '<' '>' 'g' 'G'.
bool IsStop(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u & 0xFE) == 0x28 || (u & 0xFD) == 0x3C || (u | 0x20) == 'g' || u == '\\';
}

} // namespace

// One pass over the bytes that can open or close a string or a property list or end a colour
// operator: in SSE2 16 bytes a step, each such byte then taken from the step's bit mask by the
// state of strings it is in. Hex strings (the Unicode renderer's text) hold none of those bytes but
// their '>'; literal strings (the standard renderer's) their letters 'g' and 'G' besides.
void PrintColors(char* begin, char* end) {
    char* floor = begin;   // what lies before is read: operands start here or after
    char* skip = begin;    // bytes before this one are read: an escaped byte, "<<"'s second '<'
    int depth = 0;         // the nesting of the literal string the scan is in, 0 outside
    OperandMemo memo;
    bool hex = false;      // in a hex string
    const auto stop = [&](char* at) RAYOMD_HOT_LAMBDA {
        if (at < skip) return;
        const char c = *at;
        if (depth > 0) {
            if (c == '\\') skip = at + 2;
            else if (c == '(') depth++;
            else if (c == ')' && --depth == 0) floor = at + 1;
        } else if (hex) {
            if (c == '>') {
                hex = false;
                floor = at + 1;
            }
        } else if (c == '(') {
            depth = 1;
        } else if (c == '<') {
            if (at + 1 < end && at[1] == '<') floor = skip = at + 2;
            else hex = true;
        } else if ((c | 0x20) == 'g') {
            if (char* const next = Operator(floor, at, end, memo)) floor = next;
        }
    };
    char* chunk = begin;
#if RAYOMD_PRINT_SSE2
    const __m128i parens = _mm_set1_epi8(0x28), angles = _mm_set1_epi8(0x3C), gee = _mm_set1_epi8('g'),
                  backslash = _mm_set1_epi8('\\'), pairBit = _mm_set1_epi8(static_cast<char>(0xFE)),
                  angleBit = _mm_set1_epi8(static_cast<char>(0xFD)), caseBit = _mm_set1_epi8(0x20);
    for (; end - chunk >= 16; chunk += 16) {
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(chunk));
        const __m128i hit = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(_mm_and_si128(bytes, pairBit), parens),
                         _mm_cmpeq_epi8(_mm_and_si128(bytes, angleBit), angles)),
            _mm_or_si128(_mm_cmpeq_epi8(_mm_or_si128(bytes, caseBit), gee), _mm_cmpeq_epi8(bytes, backslash)));
        for (unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(hit)); bits != 0; bits &= bits - 1) {
            stop(chunk + RayoMd::Text::Detail::LowestBit(bits));
        }
    }
#endif
    for (; chunk < end; chunk++) {
        if (IsStop(*chunk)) stop(chunk);
    }
}

void PrintColors(std::string& pdf, const std::vector<size_t>& starts, std::vector<std::string>& bands,
    std::string& cover, std::vector<std::string>& overlays) {
    if (!starts.empty()) PrintColors(pdf.data() + starts.front(), pdf.data() + pdf.size());
    for (std::string& band : bands) PrintColors(band);
    PrintColors(cover);
    for (std::string& overlay : overlays) PrintColors(overlay);
}

} // namespace TinyPdf::Internal
