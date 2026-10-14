#include "inflate.h"

#include "flate.h"
#include "../common/compiler.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOMD_INFLATE_SSE2 1
#endif

namespace RayoMd::Inflate {
namespace {

constexpr size_t kWindow = 32768;
// Past the target the fast loop may write two literals and a match of 258 bytes, whose copy goes
// up to 31 bytes further.
constexpr size_t kSlack = 2 + 258 + 32 + 8;

// A decode table entry: the value in the high half (a literal, a base length or distance, or a
// subtable's offset), the code's kind, a field, and in the low byte only the bits to take, so
// that a shift by the entry needs no mask on x86. A length or distance takes its code and extra
// bits at once, the field giving the code's bits, under which the extra ones start; a subtable's
// field gives its bits.
enum : uint32_t {
    kLiteral = 0,   // also a distance or a code length
    kLength = 1u << 13,
    kEnd = 2u << 13,
    kSubtable = 3u << 13,
    kInvalid = 4u << 13,
};
constexpr uint32_t kKindMask = 7u << 13;

constexpr uint32_t Entry(uint32_t value, uint32_t field, uint32_t kind, uint32_t bits) {
    return value << 16 | kind | field << 8 | bits;
}
inline unsigned TakenBits(uint32_t entry) { return entry & 63; }
inline unsigned Field(uint32_t entry) { return (entry >> 8) & 31; }
inline uint32_t Value(uint32_t entry) { return entry >> 16; }
// The base plus the extra bits of a length or distance `entry` read from `bits`.
inline size_t ValuePlusExtra(uint32_t entry, uint64_t bits) {
    return Value(entry) + (size_t)((bits & ((uint64_t(1) << TakenBits(entry)) - 1)) >> Field(entry));
}

// Codes are looked up by their first kRootBits bits; a longer code's prefix leads to a subtable of
// its remaining bits, at most 15 in all.
constexpr unsigned kLitLenRoot = 11;
constexpr unsigned kDistRoot = 8;
constexpr unsigned kPrecodeRoot = 7;
constexpr size_t kLitLenTableSize = (size_t(1) << kLitLenRoot) + 288 * (size_t(1) << (15 - kLitLenRoot));
constexpr size_t kDistTableSize = (size_t(1) << kDistRoot) + 32 * (size_t(1) << (15 - kDistRoot));

constexpr uint16_t kLengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83,
    99, 115, 131, 163, 195, 227, 258 };
constexpr uint8_t kLengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5,
    5, 0 };
constexpr uint16_t kDistanceBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
constexpr uint8_t kDistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11,
    11, 12, 12, 13, 13 };
constexpr uint8_t kPrecodeOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

struct LitLenEntry {
    uint32_t operator()(unsigned symbol, unsigned bits) const {
        if (symbol < 256) return Entry(symbol, 0, kLiteral, bits);
        if (symbol == 256) return Entry(0, 0, kEnd, bits);
        if (symbol < 286) return Entry(kLengthBase[symbol - 257], bits, kLength, bits + kLengthExtra[symbol - 257]);
        return Entry(0, 0, kInvalid, bits);   // 286 and 287 have fixed codes but no meaning
    }
};

struct DistanceEntry {
    uint32_t operator()(unsigned symbol, unsigned bits) const {
        if (symbol < 30) return Entry(kDistanceBase[symbol], bits, kLiteral, bits + kDistanceExtra[symbol]);
        return Entry(0, 0, kInvalid, bits);
    }
};

struct PrecodeEntry {
    uint32_t operator()(unsigned symbol, unsigned bits) const { return Entry(symbol, 0, kLiteral, bits); }
};

struct ByteReversal {
    uint8_t of[256] = {};
    constexpr ByteReversal() {
        for (unsigned byte = 0; byte < 256; byte++) {
            for (unsigned bit = 0; bit < 8; bit++) of[byte] |= (uint8_t)(((byte >> bit) & 1) << (7 - bit));
        }
    }
};
constexpr ByteReversal kReversed;

// The low `bits` bits of `code` in reverse order, `bits` at most 16.
inline unsigned Reverse(unsigned code, unsigned bits) {
    return ((unsigned)kReversed.of[code & 255] << 8 | kReversed.of[(code >> 8) & 255]) >> (16 - bits);
}

// The decode table of the canonical code with `lengths` (0 for an unused symbol, at most 15),
// as zlib builds it: false for an over-subscribed code, and for an incomplete one unless it has a
// single code of one bit or, where `allowEmpty`, no code at all. `incompleteAllowed` false refuses
// every incomplete code (the code length code).
template <typename EntryOf>
bool BuildTable(const uint8_t* lengths, unsigned count, unsigned root, uint32_t* table, bool incompleteAllowed,
    bool allowEmpty, EntryOf entryOf) {
    unsigned counts[16] = {};
    for (unsigned symbol = 0; symbol < count; symbol++) counts[lengths[symbol]]++;
    counts[0] = 0;
    int left = 1;
    unsigned codes = 0;
    unsigned longest = 0;
    for (unsigned bits = 1; bits <= 15; bits++) {
        left <<= 1;
        left -= (int)counts[bits];
        if (left < 0) return false;
        codes += counts[bits];
        if (counts[bits]) longest = bits;
    }
    const size_t rootSize = size_t(1) << root;
    if (left > 0) {
        const bool single = codes == 1 && counts[1] == 1;
        if (!incompleteAllowed || !(single || (codes == 0 && allowEmpty))) return false;
        for (size_t index = 0; index < rootSize; index++) table[index] = Entry(0, 0, kInvalid, 1);
    }
    unsigned next[16] = {};
    for (unsigned bits = 1, code = 0; bits <= 15; bits++) {
        code = (code + counts[bits - 1]) << 1;
        next[bits] = code;
    }
    const unsigned subBits = 15 - root;
    size_t subNext = rootSize;
    uint16_t subOf[1 << 11];   // the subtable of each root prefix, 0 for none
    if (longest > root) std::memset(subOf, 0, sizeof(uint16_t) << root);
    for (unsigned symbol = 0; symbol < count; symbol++) {
        const unsigned bits = lengths[symbol];
        if (bits == 0) continue;
        const unsigned reversed = Reverse(next[bits]++, bits);
        const uint32_t entry = entryOf(symbol, bits);
        if (bits <= root) {
            for (size_t index = reversed; index < rootSize; index += size_t(1) << bits) table[index] = entry;
            continue;
        }
        const unsigned prefix = reversed & (unsigned)(rootSize - 1);
        if (subOf[prefix] == 0) {
            subOf[prefix] = (uint16_t)subNext;
            table[prefix] = Entry((uint32_t)subNext, subBits, kSubtable, 0);   // takes no bits itself
            subNext += size_t(1) << subBits;
        }
        uint32_t* const sub = table + subOf[prefix];
        for (size_t index = reversed >> root; index < (size_t(1) << subBits); index += size_t(1) << (bits - root)) {
            sub[index] = entry;
        }
    }
    return true;
}

inline uint64_t Load64(const uint8_t* at) {
    uint64_t value;
    std::memcpy(&value, at, 8);
    return value;   // little-endian targets only, as the rest of RayoMD
}

inline void Store64(uint8_t* at, uint64_t value) { std::memcpy(at, &value, 8); }

// The entry for the code at the bottom of `bits`, through its subtable when it is longer than `root`.
RAYOMD_HOT_INLINE uint32_t Lookup(const uint32_t* table, uint64_t bits, unsigned root) {
    uint32_t entry = table[bits & ((uint64_t(1) << root) - 1)];
    if ((entry & kKindMask) == kSubtable) {
        entry = table[Value(entry) + ((bits >> root) & ((uint64_t(1) << Field(entry)) - 1))];
    }
    return entry;
}

// Copies a match of `length` bytes from `back` bytes before `to`, up to 31 bytes past its end.
RAYOMD_HOT_INLINE void CopyMatch(uint8_t* to, size_t back, size_t length) {
    const uint8_t* from = to - back;
    uint8_t* const stop = to + length;
#if RAYOMD_INFLATE_SSE2
    if (back >= 16) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to), _mm_loadu_si128(reinterpret_cast<const __m128i*>(from)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + 16),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + 16)));
        for (to += 32, from += 32; to < stop; to += 16, from += 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(to), _mm_loadu_si128(reinterpret_cast<const __m128i*>(from)));
        }
        return;
    }
    if (back == 1) {
        const __m128i repeated = _mm_set1_epi8((char)from[0]);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to), repeated);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + 16), repeated);
        for (to += 32; to < stop; to += 16) _mm_storeu_si128(reinterpret_cast<__m128i*>(to), repeated);
        return;
    }
#endif
    if (back >= 8) {
        Store64(to, Load64(from));
        Store64(to + 8, Load64(from + 8));
        for (to += 16, from += 16; to < stop; to += 8, from += 8) Store64(to, Load64(from));
        return;
    }
    if (back == 1) {
        const uint64_t repeated = 0x0101010101010101ull * from[0];
        Store64(to, repeated);
        Store64(to + 8, repeated);
        for (to += 16; to < stop; to += 8) Store64(to, repeated);
        return;
    }
    // Words a period apart, each right in its first `back` bytes, until the whole periods that fit
    // in a word are there; then words that far apart. Each load reads what the store before wrote.
    const size_t step = back * (8 / back);
    uint8_t* at = to;
    for (uint8_t* const periods = to + step; at < periods; at += back) Store64(at, Load64(at - back));
    for (; at < stop; at += step) Store64(at, Load64(at - step));
}

struct FixedTables {
    uint32_t litlen[kLitLenTableSize];
    uint32_t distance[kDistTableSize];
    FixedTables() {
        uint8_t lengths[288];
        std::memset(lengths, 8, 144);
        std::memset(lengths + 144, 9, 112);
        std::memset(lengths + 256, 7, 24);
        std::memset(lengths + 280, 8, 8);
        BuildTable(lengths, 288, kLitLenRoot, litlen, false, false, LitLenEntry());
        std::memset(lengths, 5, 32);
        BuildTable(lengths, 32, kDistRoot, distance, false, false, DistanceEntry());
    }
};

const FixedTables& Fixed() {
    static const FixedTables tables;
    return tables;
}

struct FreeWindow {
    void operator()(uint8_t* window) const { std::free(window); }
};

} // namespace

struct ZlibReader::State {
    enum class Mode { Header, Stored, Codes, Done, Error };

    const uint8_t* in = nullptr;
    const uint8_t* end = nullptr;
    uint64_t bitbuf = 0;
    unsigned bitsleft = 0;
    unsigned overread = 0;   // zero bytes put in the bit buffer past the end of the input

    std::unique_ptr<uint8_t[], FreeWindow> window;
    size_t capacity = 0;
    size_t out = 0;          // in the window: the next byte to write
    size_t read = 0;         // in the window: the next byte to hand out
    size_t checked = 0;      // in the window: the Adler-32 covers the bytes before
    uint64_t base = 0;       // output before the window's first byte
    uint64_t outputSize = 0;
    uint32_t adler = 1;

    Mode mode = Mode::Header;
    bool final = false;
    size_t storedLeft = 0;
    const uint32_t* litlen = nullptr;
    const uint32_t* distance = nullptr;
    uint32_t dynamicLitlen[kLitLenTableSize];
    uint32_t dynamicDistance[kDistTableSize];

    void Refill() {
        if (end - in >= 8) {
            bitbuf |= Load64(in) << bitsleft;
            in += (63 - bitsleft) >> 3;
            bitsleft |= 56;
            return;
        }
        while (bitsleft <= 56) {
            uint64_t byte = 0;
            if (in < end) byte = *in++;
            else overread++;
            bitbuf |= byte << bitsleft;
            bitsleft += 8;
        }
    }

    // `count` bits, at most 32, after a refill that left them in the buffer.
    uint32_t Bits(unsigned count) {
        const uint32_t value = (uint32_t)(bitbuf & ((uint64_t(1) << count) - 1));
        bitbuf >>= count;
        bitsleft -= count;
        return value;
    }

    // More than the bit buffer holds past the end: a code took bits the input does not have.
    bool Overrun() const { return overread > 8; }

    bool Fail() {
        mode = Mode::Error;
        return false;
    }

    // Puts the whole bytes still in the bit buffer back into the input; false when some of the
    // bits taken were past its end.
    bool AlignToByte() {
        bitbuf >>= bitsleft & 7;
        bitsleft &= ~7u;
        if (overread * 8 > bitsleft) return false;
        in -= bitsleft / 8 - overread;
        bitbuf = 0;
        bitsleft = 0;
        overread = 0;
        return true;
    }

    bool ReadDynamicTables() {
        Refill();
        const unsigned litlenCount = 257 + Bits(5);
        const unsigned distanceCount = 1 + Bits(5);
        const unsigned precodeCount = 4 + Bits(4);
        if (litlenCount > 286 || distanceCount > 30) return false;
        uint8_t precodeLengths[19] = {};
        for (unsigned index = 0; index < precodeCount; index++) {
            if (bitsleft < 3) Refill();
            precodeLengths[kPrecodeOrder[index]] = (uint8_t)Bits(3);
        }
        uint32_t precode[size_t(1) << kPrecodeRoot];
        if (!BuildTable(precodeLengths, 19, kPrecodeRoot, precode, false, false, PrecodeEntry())) return false;
        uint8_t lengths[286 + 30];
        const unsigned total = litlenCount + distanceCount;
        for (unsigned index = 0; index < total;) {
            Refill();
            if (Overrun()) return false;
            const uint32_t entry = precode[bitbuf & ((1u << kPrecodeRoot) - 1)];
            Bits(TakenBits(entry));
            const unsigned symbol = Value(entry);
            if (symbol < 16) {
                lengths[index++] = (uint8_t)symbol;
                continue;
            }
            uint8_t value = 0;
            unsigned repeat = 0;
            if (symbol == 16) {
                if (index == 0) return false;
                value = lengths[index - 1];
                repeat = 3 + Bits(2);
            } else if (symbol == 17) {
                repeat = 3 + Bits(3);
            } else {
                repeat = 11 + Bits(7);
            }
            if (repeat > total - index) return false;
            std::memset(lengths + index, value, repeat);
            index += repeat;
        }
        if (lengths[256] == 0) return false;
        if (!BuildTable(lengths, litlenCount, kLitLenRoot, dynamicLitlen, true, false, LitLenEntry()) ||
            !BuildTable(lengths + litlenCount, distanceCount, kDistRoot, dynamicDistance, true, true, DistanceEntry())) {
            return false;
        }
        litlen = dynamicLitlen;
        distance = dynamicDistance;
        return true;
    }

    bool ReadBlockHeader() {
        if (bitsleft < 3) Refill();
        if (Overrun()) return false;
        final = Bits(1) != 0;
        const unsigned type = Bits(2);
        if (type == 0) {
            if (!AlignToByte() || end - in < 4) return false;
            const unsigned length = in[0] | in[1] << 8;
            const unsigned complement = in[2] | in[3] << 8;
            if (length != (~complement & 0xFFFF)) return false;
            in += 4;
            storedLeft = length;
            mode = length ? Mode::Stored : Mode::Header;
            return true;
        }
        if (type == 1) {
            litlen = Fixed().litlen;
            distance = Fixed().distance;
        } else if (type != 2 || !ReadDynamicTables()) {
            return false;
        }
        mode = Mode::Codes;
        return true;
    }

    // Decodes codes until the window holds `target` bytes or the block ends; a match may run past
    // the target, into the slack. While 16 input bytes are left, a refill is a single load, and the
    // next code's entry is looked up before it from the bits already there, which keeps the refill
    // off the chain of lookups; up to three literals go between refills. The last bytes go a code
    // at a time.
    bool DecodeCodes(size_t target) {
        uint8_t* const w = window.get();
        size_t o = out;
        const uint32_t* const lit = litlen;
        const uint32_t* const dist = distance;
        if (end - in >= 16) {
            const uint8_t* const fastEnd = end - 16;
            uint64_t bb = bitbuf;
            unsigned bl = bitsleft;
            const uint8_t* ip = in;
            const auto refill = [&]() {
                bb |= Load64(ip) << bl;
                ip += (63 - bl) >> 3;
                bl |= 56;
            };
            const auto consume = [&](uint32_t entry) {
                bb >>= TakenBits(entry);
                bl -= TakenBits(entry);
            };
            constexpr uint64_t kRootMask = (1u << kLitLenRoot) - 1;
            const uint64_t before = base;   // output before the window, constant here
            bool ok = true;
            refill();
            uint32_t entry = lit[bb & kRootMask];
            // At the top at least 56 bits, and `entry` is the next code's.
            while (o < target && ip <= fastEnd) {
                uint64_t saved = bb;
                consume(entry);   // nothing for a subtable
                if ((entry & kKindMask) == kSubtable) {
                    entry = lit[Value(entry) + ((bb >> kLitLenRoot) & ((1u << Field(entry)) - 1))];
                    saved = bb;
                    consume(entry);
                }
                if ((entry & kKindMask) == kLiteral) {
                    // Up to two more codes of 11 bits whole in the root table: 15 + 22 bits.
                    uint8_t literal = (uint8_t)Value(entry);
                    entry = lit[bb & kRootMask];
                    if ((entry & kKindMask) == kLiteral) {
                        consume(entry);
                        w[o++] = literal;
                        literal = (uint8_t)Value(entry);
                        entry = lit[bb & kRootMask];
                        if ((entry & kKindMask) == kLiteral) {
                            consume(entry);
                            w[o++] = literal;
                            literal = (uint8_t)Value(entry);
                            entry = lit[bb & kRootMask];
                        }
                    }
                    refill();   // the bits under the entry stay
                    w[o++] = literal;
                    continue;
                }
                const uint32_t kind = entry & kKindMask;
                if (kind != kLength) {
                    if (kind == kEnd) mode = Mode::Header;
                    else ok = false;
                    break;
                }
                // At least 36 bits left for the distance's 15 and 13.
                const size_t length = ValuePlusExtra(entry, saved);
                const uint32_t code = Lookup(dist, bb, kDistRoot);
                const size_t back = ValuePlusExtra(code, bb);
                consume(code);
                if ((code & kKindMask) == kInvalid || back > before + o) {   // before the start of the output
                    ok = false;
                    break;
                }
                refill();
                entry = lit[bb & kRootMask];
                CopyMatch(w + o, back, length);
                o += length;
            }
            bitbuf = bb;
            bitsleft = bl;
            in = ip;
            if (!ok) return false;
            if (mode != Mode::Codes || o >= target) {
                out = o;
                return true;
            }
        }
        while (o < target) {
            Refill();
            if (Overrun()) return false;
            const uint32_t entry = Lookup(lit, bitbuf, kLitLenRoot);
            const uint64_t lengthBits = bitbuf;
            Bits(TakenBits(entry));
            const uint32_t kind = entry & kKindMask;
            if (kind == kLiteral) {
                w[o++] = (uint8_t)Value(entry);
                continue;
            }
            if (kind == kEnd) {
                mode = Mode::Header;
                break;
            }
            if (kind == kInvalid) return false;
            // At least 56 bits after the refill: 20 for the length, 28 for the distance.
            const size_t length = ValuePlusExtra(entry, lengthBits);
            const uint32_t code = Lookup(dist, bitbuf, kDistRoot);
            const size_t back = ValuePlusExtra(code, bitbuf);
            Bits(TakenBits(code));
            if ((code & kKindMask) == kInvalid) return false;
            if (back > base + o) return false;   // before the start of the output
            CopyMatch(w + o, back, length);
            o += length;
        }
        out = o;
        return true;
    }

    // Inflates until the window holds `target` bytes or the stream ends.
    bool Fill(size_t target) {
        while (out < target) {
            switch (mode) {
            case Mode::Done:
                return true;
            case Mode::Error:
                return false;
            case Mode::Header:
                if (final) {
                    mode = Mode::Done;
                    break;
                }
                if (!ReadBlockHeader()) return Fail();
                break;
            case Mode::Stored: {
                const size_t count = std::min(storedLeft, target - out);
                if ((size_t)(end - in) < count) return Fail();
                std::memcpy(window.get() + out, in, count);
                in += count;
                out += count;
                storedLeft -= count;
                if (storedLeft == 0) mode = Mode::Header;
                break;
            }
            case Mode::Codes:
                if (!DecodeCodes(target)) return Fail();
                break;
            }
        }
        if (base + out > outputSize) return Fail();   // more output than the image has
        adler = RayoMd::Flate::Adler32(window.get() + checked, out - checked, adler);
        checked = out;
        return true;
    }

    // Moves the bytes still to hand out and the last 32 KB of output to the window's start.
    void Slide() {
        const size_t keep = std::min(read, out > kWindow ? out - kWindow : 0);
        if (keep == 0) return;
        std::memmove(window.get(), window.get() + keep, out - keep);
        out -= keep;
        read -= keep;
        checked -= keep;
        base += keep;
    }
};

ZlibReader::ZlibReader() : state_(new State) {}
ZlibReader::~ZlibReader() = default;

bool ZlibReader::Start(const uint8_t* data, size_t size, size_t outputSize, size_t largestTake) {
    State& state = *state_;
    if (size < 2 || (data[0] & 15) != 8 || (data[0] >> 4) > 7 || ((data[0] << 8) | data[1]) % 31 != 0 ||
        (data[1] & 0x20)) {
        return false;
    }
    state.in = data + 2;
    state.end = data + size;
    state.outputSize = outputSize;
    state.capacity = kWindow + std::max<size_t>(largestTake, size_t(256) << 10) + kSlack;
    state.window.reset(static_cast<uint8_t*>(std::malloc(state.capacity)));
    return state.window != nullptr;
}

const uint8_t* ZlibReader::Take(size_t bytes) {
    State& state = *state_;
    if (state.out - state.read < bytes) {
        if (state.read + bytes > state.capacity - kSlack) state.Slide();
        const uint64_t left = state.outputSize - state.base;
        const size_t target = (size_t)std::min<uint64_t>(state.capacity - kSlack, left);
        if (state.read + bytes > target || !state.Fill(target) || state.out - state.read < bytes) return nullptr;
    }
    const uint8_t* const taken = state.window.get() + state.read;
    state.read += bytes;
    return taken;
}

void ZlibReader::Unread(size_t bytes) { state_->read -= bytes; }

bool ZlibReader::Finish() {
    State& state = *state_;
    if (state.mode == State::Mode::Error || state.base + state.read != state.outputSize) return false;
    // The remaining blocks must be empty: any output past the size fails.
    if (state.out + 1 > state.capacity - kSlack) state.Slide();
    while (state.mode != State::Mode::Done) {
        if (!state.Fill(state.out + 1) || state.base + state.out != state.outputSize) return false;
    }
    if (!state.AlignToByte() || state.end - state.in < 4) return false;
    const uint32_t expected = (uint32_t)state.in[0] << 24 | (uint32_t)state.in[1] << 16 | (uint32_t)state.in[2] << 8 |
        state.in[3];
    return expected == state.adler;
}

} // namespace RayoMd::Inflate
