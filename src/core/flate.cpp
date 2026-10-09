#include "flate.h"

#include <array>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOMD_FLATE_SSE2 1
#endif

namespace RayoMd::Flate {
namespace {

constexpr uint32_t kWindow = 32768;
constexpr uint32_t kMinMatch = 4;
constexpr uint32_t kMaxMatch = 258;
// A token may start only before blockStart + kBlockStart, so a block covers at most
// 65277 + 258 = 65535 input bytes and a stored fallback is a single stored block.
constexpr size_t kBlockStart = 65278;
constexpr unsigned kLitLenSymbols = 286;
constexpr unsigned kDistanceSymbols = 30;
constexpr unsigned kPrecodeSymbols = 19;
constexpr uint64_t kLongMultiplier = 0x9E3779B97F4A7C15ull;
constexpr uint32_t kShortMultiplier = 0x1E35A7BDu;
// Inputs are under 2 GB, so a call starting at a base up to here neither overflows its positions
// nor the base it leaves for the next call.
constexpr uint32_t kBaseLimit = (1u << 31) - kWindow;
// Inside a match up to kInsertAll long every position enters the tables, inside a longer one only
// the first and last kInsertEdge. On RayoMD's streams that keeps the output within 0.5 % of
// inserting every position, and is a fifth faster on pages and four times on runs of zeros.
constexpr uint32_t kInsertAll = 16;
constexpr size_t kInsertEdge = 4;
// After 32 literals in a row the data is taken to be incompressible there, as font outlines
// mostly are: each further literal takes the next (run >> kSkipShift) bytes along unsearched,
// as LZ4 does. Fonts compress 14 % faster for 1.2 % more bytes; pages rarely have such runs.
constexpr unsigned kSkipShift = 5;

constexpr uint16_t kLengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67,
    83, 99, 115, 131, 163, 195, 227, 258 };
constexpr uint8_t kLengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5,
    5, 5, 0 };
constexpr uint16_t kDistanceBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
constexpr uint8_t kDistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13 };
constexpr uint8_t kPrecodeOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
constexpr uint8_t kPrecodeExtra[19] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7 };

// The length symbol (0..28 for 257..285) of each match length 3..258; 258 is 285, never 284.
constexpr std::array<uint8_t, 259> MakeLengthSymbols() {
    std::array<uint8_t, 259> symbols{};
    for (unsigned symbol = 0; symbol < 28; symbol++) {
        for (unsigned length = kLengthBase[symbol]; length < kLengthBase[symbol + 1]; length++) symbols[length] = (uint8_t)symbol;
    }
    symbols[258] = 28;
    return symbols;
}
constexpr std::array<uint8_t, 259> kLengthSymbol = MakeLengthSymbols();

// Each byte with its bits in reverse order: Huffman codes go out most significant bit first.
constexpr std::array<uint8_t, 256> MakeReversedBytes() {
    std::array<uint8_t, 256> reversed{};
    for (unsigned value = 0; value < 256; value++) {
        for (unsigned bit = 0; bit < 8; bit++) reversed[value] |= (uint8_t)(((value >> bit) & 1) << (7 - bit));
    }
    return reversed;
}
constexpr std::array<uint8_t, 256> kReversedByte = MakeReversedBytes();

inline unsigned HighestBit(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return 31u - (unsigned)__builtin_clz(value);
#else
    unsigned bit = 0;
    while (value >>= 1) bit++;
    return bit;
#endif
}

inline unsigned FirstDifferentByte(uint64_t difference) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(difference) >> 3;
#else
    unsigned bytes = 0;
    while ((difference & 0xFF) == 0) {
        difference >>= 8;
        bytes++;
    }
    return bytes;
#endif
}

// Little-endian loads and stores on every host, so hashing and output never depend on it.
inline uint64_t Load64(const uint8_t* at) {
    uint64_t value;
    memcpy(&value, at, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap64(value);
#endif
    return value;
}

inline void Store64(uint8_t* at, uint64_t value) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap64(value);
#endif
    memcpy(at, &value, 8);
}

// The distance symbol of a distance less one.
inline unsigned DistanceSymbol(uint32_t distanceLessOne) {
    if (distanceLessOne < 4) return distanceLessOne;
    const unsigned high = HighestBit(distanceLessOne);
    return 2 * high + ((distanceLessOne >> (high - 1)) & 1);
}

// How many leading bytes `earlier` and `here` share, at most `limit`; `here` + `limit` is
// within the input, and `earlier` lies before `here`.
inline uint32_t MatchLength(const uint8_t* earlier, const uint8_t* here, uint32_t limit) {
    uint32_t length = 0;
    while (length + 8 <= limit) {
        const uint64_t difference = Load64(earlier + length) ^ Load64(here + length);
        if (difference != 0) return length + FirstDifferentByte(difference);
        length += 8;
    }
    while (length < limit && earlier[length] == here[length]) length++;
    return length;
}

// Writes bits from the least significant on, as DEFLATE wants. At most 55 bits are pending
// before a Flush (7 left over and a 48-bit match), which stores eight bytes at once: the output
// has eight bytes of slack.
struct BitWriter {
    uint8_t* out;
    uint64_t buffer = 0;
    unsigned count = 0;

    void Put(uint64_t bits, unsigned width) {
        buffer |= bits << count;
        count += width;
    }
    void Flush() {
        Store64(out, buffer);
        out += count >> 3;
        buffer >>= count & ~7u;
        count &= 7;
    }
};

// Code lengths of at most `maxLength` bits for the `count` symbols with frequencies `freq` (none
// for a frequency of 0): a complete prefix code with at least two codes, which zlib's inflate
// requires. Huffman's tree, its levels limited as libdeflate does, then the shortest codes to the
// most frequent symbols.
void BuildLengths(const uint32_t* freq, unsigned count, unsigned maxLength, uint8_t* lengths) {
    std::fill(lengths, lengths + count, uint8_t(0));
    uint64_t leaves[kLitLenSymbols];   // frequency << 9 | symbol: sorts by frequency, then symbol
    unsigned used = 0;
    for (unsigned symbol = 0; symbol < count; symbol++) {
        if (freq[symbol] != 0) leaves[used++] = (uint64_t)freq[symbol] << 9 | symbol;
    }
    if (used < 2) {
        const unsigned first = used == 1 ? (unsigned)(leaves[0] & 511) : 0;
        lengths[first] = 1;
        lengths[first == 0 ? 1 : 0] = 1;
        return;
    }
    std::sort(leaves, leaves + used);

    // Two queues: the leaves in order, and the inner nodes, which are made in order of weight.
    uint64_t weight[2 * kLitLenSymbols];
    uint16_t parent[2 * kLitLenSymbols];
    for (unsigned leaf = 0; leaf < used; leaf++) weight[leaf] = leaves[leaf] >> 9;
    unsigned nextLeaf = 0;
    unsigned nextInner = used;
    unsigned made = used;
    const auto take = [&]() {
        if (nextLeaf < used && (nextInner == made || weight[nextLeaf] <= weight[nextInner])) return nextLeaf++;
        return nextInner++;
    };
    while (made < 2 * used - 1) {
        const unsigned first = take();
        const unsigned second = take();
        weight[made] = weight[first] + weight[second];
        parent[first] = parent[second] = (uint16_t)made;
        made++;
    }
    // Codes per length: two leaves under the root, then each inner node, parents first, turns
    // one leaf of its level into two of the next. Inner nodes come out of the queue deepest
    // first, so one at the limit or below takes the deepest leaf left above the limit instead:
    // every step keeps the code complete.
    unsigned lengthCount[16] = {};
    lengthCount[1] = 2;
    uint16_t depth[2 * kLitLenSymbols];
    depth[made - 1] = 0;
    for (unsigned node = made - 1; node-- > used;) {
        unsigned bits = depth[parent[node]] + 1u;
        depth[node] = (uint16_t)bits;
        if (bits >= maxLength) {
            bits = maxLength;
            do bits--; while (lengthCount[bits] == 0);
        }
        lengthCount[bits]--;
        lengthCount[bits + 1] += 2;
    }
    unsigned leaf = 0;
    for (unsigned bits = maxLength; bits >= 1; bits--) {
        for (unsigned k = lengthCount[bits]; k > 0; k--) lengths[leaves[leaf++] & 511] = (uint8_t)bits;
    }
}

// Canonical codes (RFC 1951 3.2.2) for `lengths`, bit-reversed for the least-significant-first
// writer.
void CanonicalCodes(const uint8_t* lengths, unsigned count, uint16_t* codes) {
    unsigned lengthCount[16] = {};
    for (unsigned symbol = 0; symbol < count; symbol++) lengthCount[lengths[symbol]]++;
    lengthCount[0] = 0;
    unsigned next[16] = {};
    unsigned code = 0;
    for (unsigned bits = 1; bits < 16; bits++) {
        code = (code + lengthCount[bits - 1]) << 1;
        next[bits] = code;
    }
    for (unsigned symbol = 0; symbol < count; symbol++) {
        const unsigned bits = lengths[symbol];
        if (bits == 0) continue;
        const unsigned value = next[bits]++;
        const unsigned reversed = (unsigned)kReversedByte[value & 0xFF] << 8 | kReversedByte[value >> 8];
        codes[symbol] = (uint16_t)(reversed >> (16 - bits));
    }
}

struct Code {
    uint8_t litLength[kLitLenSymbols];
    uint16_t litCode[kLitLenSymbols];
    uint8_t distanceLength[kDistanceSymbols];
    uint16_t distanceCode[kDistanceSymbols];
};

// The fixed code of RFC 1951 3.2.6.
const Code& FixedCode() {
    static const Code code = [] {
        Code fixed{};
        for (unsigned symbol = 0; symbol < kLitLenSymbols; symbol++) {
            fixed.litLength[symbol] = symbol < 144 ? 8 : symbol < 256 ? 9 : symbol < 280 ? 7 : 8;
        }
        std::fill(fixed.distanceLength, fixed.distanceLength + kDistanceSymbols, uint8_t(5));
        // The canonical codes of the full 288-symbol alphabet: 286 and 287 only fix the numbering.
        uint8_t lengths[288];
        std::copy(fixed.litLength, fixed.litLength + kLitLenSymbols, lengths);
        lengths[286] = lengths[287] = 8;
        uint16_t codes[288];
        CanonicalCodes(lengths, 288, codes);
        std::copy(codes, codes + kLitLenSymbols, fixed.litCode);
        CanonicalCodes(fixed.distanceLength, kDistanceSymbols, fixed.distanceCode);
        return fixed;
    }();
    return code;
}

void EmitTokens(BitWriter& target, const uint32_t* tokens, size_t count, const Code& code) {
    // A local copy: stores through `out` could alias the caller's writer, which would then be
    // reloaded from memory for every token.
    BitWriter writer = target;
    for (size_t index = 0; index < count; index++) {
        const uint32_t token = tokens[index];
        if (token < 256) {
            writer.Put(code.litCode[token], code.litLength[token]);
        } else {
            const unsigned length = token >> 16;
            const unsigned lengthSymbol = kLengthSymbol[length];
            writer.Put(code.litCode[257 + lengthSymbol], code.litLength[257 + lengthSymbol]);
            writer.Put(length - kLengthBase[lengthSymbol], kLengthExtra[lengthSymbol]);
            const uint32_t distanceLessOne = token & 0xFFFF;
            const unsigned distanceSymbol = DistanceSymbol(distanceLessOne);
            writer.Put(code.distanceCode[distanceSymbol], code.distanceLength[distanceSymbol]);
            writer.Put(distanceLessOne + 1 - kDistanceBase[distanceSymbol], kDistanceExtra[distanceSymbol]);
        }
        writer.Flush();
    }
    writer.Put(code.litCode[256], code.litLength[256]);
    writer.Flush();
    target = writer;
}

// One block of `size` input bytes at `data`, parsed into `tokens`: stored, fixed or dynamic,
// whichever takes the fewest bits (on a tie in that order).
void EmitBlock(BitWriter& writer, const uint8_t* data, size_t size, const uint32_t* tokens, size_t count,
    const uint32_t* litFreq, const uint32_t* distanceFreq, bool final) {
    uint64_t extraBits = 0;   // the same for the fixed and the dynamic code
    for (unsigned symbol = 0; symbol < 29; symbol++) extraBits += (uint64_t)litFreq[257 + symbol] * kLengthExtra[symbol];
    for (unsigned symbol = 0; symbol < kDistanceSymbols; symbol++) {
        extraBits += (uint64_t)distanceFreq[symbol] * kDistanceExtra[symbol];
    }

    const Code& fixed = FixedCode();
    uint64_t fixedBits = 3 + extraBits;
    for (unsigned symbol = 0; symbol < kLitLenSymbols; symbol++) fixedBits += (uint64_t)litFreq[symbol] * fixed.litLength[symbol];
    for (unsigned symbol = 0; symbol < kDistanceSymbols; symbol++) fixedBits += (uint64_t)distanceFreq[symbol] * 5;

    Code dynamic;
    BuildLengths(litFreq, kLitLenSymbols, 15, dynamic.litLength);
    BuildLengths(distanceFreq, kDistanceSymbols, 15, dynamic.distanceLength);
    unsigned litCount = kLitLenSymbols;
    while (litCount > 257 && dynamic.litLength[litCount - 1] == 0) litCount--;
    unsigned distanceCount = kDistanceSymbols;
    while (distanceCount > 1 && dynamic.distanceLength[distanceCount - 1] == 0) distanceCount--;
    // Both length lists as one sequence, run-length coded with symbols 16, 17 and 18.
    uint8_t lengths[kLitLenSymbols + kDistanceSymbols];
    std::copy(dynamic.litLength, dynamic.litLength + litCount, lengths);
    std::copy(dynamic.distanceLength, dynamic.distanceLength + distanceCount, lengths + litCount);
    const unsigned lengthTotal = litCount + distanceCount;
    uint8_t items[kLitLenSymbols + kDistanceSymbols];
    uint8_t itemExtra[kLitLenSymbols + kDistanceSymbols];
    unsigned itemCount = 0;
    uint32_t precodeFreq[kPrecodeSymbols] = {};
    const auto item = [&](unsigned symbol, unsigned extra) {
        items[itemCount] = (uint8_t)symbol;
        itemExtra[itemCount++] = (uint8_t)extra;
        precodeFreq[symbol]++;
    };
    for (unsigned at = 0; at < lengthTotal;) {
        const uint8_t value = lengths[at];
        unsigned run = 1;
        while (at + run < lengthTotal && lengths[at + run] == value) run++;
        at += run;
        if (value == 0) {
            while (run >= 11) {
                const unsigned take = std::min(run, 138u);
                item(18, take - 11);
                run -= take;
            }
            if (run >= 3) {
                item(17, run - 3);
                run = 0;
            }
            while (run-- > 0) item(0, 0);
        } else {
            item(value, 0);
            run--;
            while (run >= 3) {
                const unsigned take = std::min(run, 6u);
                item(16, take - 3);
                run -= take;
            }
            while (run-- > 0) item(value, 0);
        }
    }
    uint8_t precodeLength[kPrecodeSymbols];
    uint16_t precodeCode[kPrecodeSymbols];
    BuildLengths(precodeFreq, kPrecodeSymbols, 7, precodeLength);
    unsigned precodeCount = kPrecodeSymbols;
    while (precodeCount > 4 && precodeLength[kPrecodeOrder[precodeCount - 1]] == 0) precodeCount--;
    uint64_t dynamicBits = 3 + 5 + 5 + 4 + 3 * (uint64_t)precodeCount + extraBits;
    for (unsigned index = 0; index < itemCount; index++) {
        dynamicBits += precodeLength[items[index]] + kPrecodeExtra[items[index]];
    }
    for (unsigned symbol = 0; symbol < kLitLenSymbols; symbol++) dynamicBits += (uint64_t)litFreq[symbol] * dynamic.litLength[symbol];
    for (unsigned symbol = 0; symbol < kDistanceSymbols; symbol++) {
        dynamicBits += (uint64_t)distanceFreq[symbol] * dynamic.distanceLength[symbol];
    }

    const unsigned pad = (8 - ((writer.count + 3) & 7)) & 7;
    const uint64_t storedBits = 3 + pad + 32 + 8 * (uint64_t)size;

    if (storedBits <= fixedBits && storedBits <= dynamicBits) {
        writer.Put(final ? 1 : 0, 3);
        writer.Put(0, pad);
        writer.Put(size, 16);
        writer.Put(~size & 0xFFFF, 16);
        writer.Flush();
        memcpy(writer.out, data, size);
        writer.out += size;
        return;
    }
    if (fixedBits <= dynamicBits) {
        writer.Put((final ? 1 : 0) | 1 << 1, 3);
        EmitTokens(writer, tokens, count, fixed);
        return;
    }
    CanonicalCodes(dynamic.litLength, kLitLenSymbols, dynamic.litCode);
    CanonicalCodes(dynamic.distanceLength, kDistanceSymbols, dynamic.distanceCode);
    CanonicalCodes(precodeLength, kPrecodeSymbols, precodeCode);
    writer.Put((final ? 1 : 0) | 2 << 1, 3);
    writer.Put(litCount - 257, 5);
    writer.Put(distanceCount - 1, 5);
    writer.Put(precodeCount - 4, 4);
    writer.Flush();
    for (unsigned index = 0; index < precodeCount; index++) {
        writer.Put(precodeLength[kPrecodeOrder[index]], 3);
        if (index % 8 == 7) writer.Flush();
    }
    writer.Flush();
    for (unsigned index = 0; index < itemCount; index++) {
        const unsigned symbol = items[index];
        writer.Put(precodeCode[symbol], precodeLength[symbol]);
        writer.Put(itemExtra[index], kPrecodeExtra[symbol]);
        writer.Flush();
    }
    EmitTokens(writer, tokens, count, dynamic);
}

uint32_t Adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1;
    uint32_t b = 0;
#ifdef RAYOMD_FLATE_SSE2
    // 32 bytes a step, as libdeflate does: a takes the byte sums, b the earlier steps' sums of a
    // times 32 plus every byte times its distance from the step's end. 4096 bytes keep the 16-bit
    // byte counters within the signed multiply and b's lanes within 32 bits.
    const __m128i zero = _mm_setzero_si128();
    const __m128i weightsA = _mm_setr_epi16(32, 31, 30, 29, 28, 27, 26, 25);
    const __m128i weightsB = _mm_setr_epi16(24, 23, 22, 21, 20, 19, 18, 17);
    const __m128i weightsC = _mm_setr_epi16(16, 15, 14, 13, 12, 11, 10, 9);
    const __m128i weightsD = _mm_setr_epi16(8, 7, 6, 5, 4, 3, 2, 1);
    while (size >= 32) {
        const size_t chunk = std::min<size_t>(size, 4096) & ~size_t(31);
        __m128i sumA = zero;
        __m128i sumB = zero;
        __m128i sumC = zero;
        __m128i sumD = zero;
        __m128i bytes = zero;
        __m128i earlier = zero;
        for (size_t at = 0; at < chunk; at += 32) {
            const __m128i low = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + at));
            const __m128i high = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + at + 16));
            earlier = _mm_add_epi32(earlier, bytes);
            bytes = _mm_add_epi32(bytes, _mm_add_epi32(_mm_sad_epu8(low, zero), _mm_sad_epu8(high, zero)));
            sumA = _mm_add_epi16(sumA, _mm_unpacklo_epi8(low, zero));
            sumB = _mm_add_epi16(sumB, _mm_unpackhi_epi8(low, zero));
            sumC = _mm_add_epi16(sumC, _mm_unpacklo_epi8(high, zero));
            sumD = _mm_add_epi16(sumD, _mm_unpackhi_epi8(high, zero));
        }
        __m128i weighted = _mm_slli_epi32(earlier, 5);
        weighted = _mm_add_epi32(weighted, _mm_madd_epi16(sumA, weightsA));
        weighted = _mm_add_epi32(weighted, _mm_madd_epi16(sumB, weightsB));
        weighted = _mm_add_epi32(weighted, _mm_madd_epi16(sumC, weightsC));
        weighted = _mm_add_epi32(weighted, _mm_madd_epi16(sumD, weightsD));
        alignas(16) uint32_t lanes[8];
        _mm_store_si128(reinterpret_cast<__m128i*>(lanes), bytes);
        _mm_store_si128(reinterpret_cast<__m128i*>(lanes + 4), weighted);
        const uint64_t byteSum = (uint64_t)lanes[0] + lanes[1] + lanes[2] + lanes[3];
        const uint64_t weightedSum = (uint64_t)lanes[4] + lanes[5] + lanes[6] + lanes[7];
        b = (uint32_t)((b + chunk * a + weightedSum) % 65521);
        a = (uint32_t)((a + byteSum) % 65521);
        data += chunk;
        size -= chunk;
    }
#endif
    while (size > 0) {
        size_t chunk = std::min<size_t>(size, 5552);   // the most bytes before b can overflow
        size -= chunk;
        for (; chunk >= 8; chunk -= 8, data += 8) {
            a += data[0]; b += a; a += data[1]; b += a; a += data[2]; b += a; a += data[3]; b += a;
            a += data[4]; b += a; a += data[5]; b += a; a += data[6]; b += a; a += data[7]; b += a;
        }
        for (; chunk > 0; chunk--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

} // namespace

size_t ZlibCompress(std::string_view input, uint8_t* out, Scratch& scratch) {
    const uint8_t* const in = reinterpret_cast<const uint8_t*>(input.data());
    const size_t size = input.size();
    out[0] = 0x78;   // deflate, 32 KB window
    out[1] = 0x5E;   // FLEVEL 1, and (0x785E % 31) == 0
    BitWriter writer{ out + 2 };

    // Tables sized to the input, 2^bits buckets of two, so a small stream touches little of them.
    unsigned bits = 10;
    while (bits < 15 && (size_t(1) << bits) < size) bits++;
    // The tables are kept from call to call. Their positions count from scratch.base, which each
    // call moves a window past the end of its input, so no entry of an earlier call is ever within
    // reach: they are cleared only when the count would overflow.
    if (scratch.longTable.empty() || scratch.base > kBaseLimit) {
        scratch.longTable.assign(size_t(2) << 15, 0);
        scratch.shortTable.assign(size_t(1) << 14, 0);
        scratch.base = kWindow + 1;
    }
    const uint32_t base = scratch.base;
    scratch.base += (uint32_t)size + kWindow;
    const size_t tokenRoom = std::min(size, kBlockStart);
    if (scratch.tokens.size() < tokenRoom) scratch.tokens.resize(tokenRoom);
    uint32_t* const longTable = scratch.longTable.data();
    uint32_t* const shortTable = scratch.shortTable.data();
    uint32_t* const tokens = scratch.tokens.data();
    const unsigned longShift = 64 - bits;
    const unsigned shortShift = 33 - bits;
    const auto insert = [&](size_t at, uint64_t sequence) {
        uint32_t* const bucket = longTable + 2 * (size_t)((sequence * kLongMultiplier) >> longShift);
        bucket[1] = bucket[0];
        bucket[0] = base + (uint32_t)at;
        shortTable[((uint32_t)sequence * kShortMultiplier) >> shortShift] = base + (uint32_t)at;
    };

    size_t at = 0;
    do {
        const size_t blockStart = at;
        const size_t limit = std::min(size, blockStart + kBlockStart);
        uint32_t litFreq[kLitLenSymbols] = {};
        uint32_t distanceFreq[kDistanceSymbols] = {};
        size_t count = 0;
        size_t literals = 0;   // since the last match
        while (at < limit) {
            if (at + 8 > size) {
                tokens[count++] = in[at];
                litFreq[in[at++]]++;
                continue;
            }
            const uint64_t sequence = Load64(in + at);
            uint32_t* const bucket = longTable + 2 * (size_t)((sequence * kLongMultiplier) >> longShift);
            uint32_t* const slot = shortTable + (((uint32_t)sequence * kShortMultiplier) >> shortShift);
            const uint32_t first = bucket[0];
            const uint32_t second = bucket[1];
            const uint32_t recent = *slot;
            const uint32_t here = base + (uint32_t)at;
            bucket[1] = first;
            bucket[0] = here;
            *slot = here;

            const uint32_t limitLength = (uint32_t)std::min<size_t>(kMaxMatch, size - at);
            uint32_t best = 0;
            uint32_t bestDistance = 0;
            if (here - first - 1 < kWindow) {
                best = MatchLength(in + (first - base), in + at, limitLength);
                bestDistance = here - first;
            }
            if (here - second - 1 < kWindow) {
                const uint32_t length = MatchLength(in + (second - base), in + at, limitLength);
                if (length > best) {
                    best = length;
                    bestDistance = here - second;
                }
            }
            if (best < 8 && here - recent - 1 < kWindow) {
                const uint32_t length = MatchLength(in + (recent - base), in + at, limitLength);
                if (length > best) {
                    best = length;
                    bestDistance = here - recent;
                }
            }
            if (best >= kMinMatch) {
                tokens[count++] = best << 16 | (bestDistance - 1);
                litFreq[257 + kLengthSymbol[best]]++;
                distanceFreq[DistanceSymbol(bestDistance - 1)]++;
                const size_t end = at + best;
                const size_t insertEnd = std::min(end, size - 7);   // a position needs 8 bytes to hash
                if (best <= kInsertAll) {
                    for (size_t inside = at + 1; inside < insertEnd; inside++) insert(inside, Load64(in + inside));
                } else {
                    const size_t headEnd = std::min(at + 1 + kInsertEdge, insertEnd);
                    for (size_t inside = at + 1; inside < headEnd; inside++) insert(inside, Load64(in + inside));
                    for (size_t inside = std::max(headEnd, end - kInsertEdge); inside < insertEnd; inside++) {
                        insert(inside, Load64(in + inside));
                    }
                }
                at = end;
                literals = 0;
            } else {
                tokens[count++] = in[at];
                litFreq[in[at++]]++;
                for (size_t skip = ++literals >> kSkipShift; skip > 0 && at < limit; skip--) {
                    tokens[count++] = in[at];
                    litFreq[in[at++]]++;
                }
            }
        }
        litFreq[256] = 1;   // end of block
        EmitBlock(writer, in + blockStart, at - blockStart, tokens, count, litFreq, distanceFreq, at >= size);
    } while (at < size);

    if (writer.count > 0) {
        writer.Put(0, 8 - writer.count);
        writer.Flush();
    }
    uint8_t* const tail = writer.out;
    const uint32_t adler = Adler32(in, size);
    tail[0] = (uint8_t)(adler >> 24);
    tail[1] = (uint8_t)(adler >> 16);
    tail[2] = (uint8_t)(adler >> 8);
    tail[3] = (uint8_t)adler;
    return (size_t)(tail + 4 - out);
}

} // namespace RayoMd::Flate
