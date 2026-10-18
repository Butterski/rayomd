// SPDX-License-Identifier: Apache-2.0

#include "png_filter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if RAYOMD_PNG_SSE2
#include <emmintrin.h>
#endif
// AVX2 where GCC or Clang can compile it for one function: the build stays baseline x86-64 and
// the CPU is asked once (UseAvx2).
#if RAYOMD_PNG_SSE2 && (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include "../common/cpu_features.h"

#include <immintrin.h>
#define RAYOMD_PNG_AVX2 1
#define RAYOMD_AVX2 __attribute__((target("avx2")))
#endif

namespace TinyPdf::Internal {
namespace {

// The magnitude of a residual read as a signed byte: r < 128 ? r : 256 - r.
inline uint64_t ResidualCost(int residual) {
    uint32_t r = (uint32_t)residual & 0xffu;
    return r < 128 ? r : 256 - r;
}

// The Paeth predictor (whichever of a, b, c is nearest to p = a + b - c, in that order on ties)
// without branches: p - a = b - c, p - b = a - c, p - c = (b - c) + (a - c).
inline int Paeth(int a, int b, int c) {
    int pa = std::abs(b - c);
    int pb = std::abs(a - c);
    int pc = std::abs(a + b - 2 * c);
    int bc = pb <= pc ? b : c;
    return (pa <= pb && pa <= pc) ? a : bc;
}

struct FilterCosts {
    uint64_t sum[5] = {};

    // Bytes [from, to): the first `bpp` of the row have no left neighbour, so left = upLeft = 0
    // there and Paeth(0, b, 0) is b.
    void Add(const uint8_t* row, const uint8_t* up, size_t from, size_t to, size_t bpp) {
        size_t x = from;
        for (; x < to && x < bpp; x++) {
            int v = row[x], b = up[x];
            sum[0] += ResidualCost(v);
            sum[1] += ResidualCost(v);
            sum[2] += ResidualCost(v - b);
            sum[3] += ResidualCost(v - (b >> 1));
            sum[4] += ResidualCost(v - b);
        }
        for (; x < to; x++) {
            int v = row[x], a = row[x - bpp], b = up[x], c = up[x - bpp];
            sum[0] += ResidualCost(v);
            sum[1] += ResidualCost(v - a);
            sum[2] += ResidualCost(v - b);
            sum[3] += ResidualCost(v - ((a + b) >> 1));
            sum[4] += ResidualCost(v - Paeth(a, b, c));
        }
    }

    int Best() const {
        int best = 0;
        for (int filter = 1; filter < 5; filter++) {
            if (sum[filter] < sum[best]) best = filter;
        }
        return best;
    }
};

#if RAYOMD_PNG_SSE2
// Sixteen residual magnitudes, min(r, 256 - r) per byte, added up into two 64-bit lanes.
inline __m128i Cost16(__m128i value, __m128i predictor) {
    const __m128i zero = _mm_setzero_si128();
    __m128i r = _mm_sub_epi8(value, predictor);
    return _mm_sad_epu8(_mm_min_epu8(r, _mm_sub_epi8(zero, r)), zero);
}

// floor((a + b) / 2) per byte: pavgb rounds up, so take back the odd bit.
inline __m128i Average16(__m128i a, __m128i b) {
    return _mm_sub_epi8(_mm_avg_epu8(a, b), _mm_and_si128(_mm_xor_si128(a, b), _mm_set1_epi8(1)));
}

inline __m128i Abs16(__m128i x) {
    return _mm_max_epi16(x, _mm_sub_epi16(_mm_setzero_si128(), x));
}

// Paeth on eight 16-bit lanes, the same comparisons as the scalar one.
inline __m128i PaethLanes(__m128i a, __m128i b, __m128i c) {
    __m128i bc = _mm_sub_epi16(b, c);
    __m128i ac = _mm_sub_epi16(a, c);
    __m128i pa = Abs16(bc);
    __m128i pb = Abs16(ac);
    __m128i pc = Abs16(_mm_add_epi16(bc, ac));
    __m128i notA = _mm_or_si128(_mm_cmpgt_epi16(pa, pb), _mm_cmpgt_epi16(pa, pc));
    __m128i useC = _mm_cmpgt_epi16(pb, pc);
    __m128i bOrC = _mm_or_si128(_mm_andnot_si128(useC, b), _mm_and_si128(useC, c));
    return _mm_or_si128(_mm_andnot_si128(notA, a), _mm_and_si128(notA, bOrC));
}

inline __m128i Paeth16(__m128i a, __m128i b, __m128i c) {
    const __m128i zero = _mm_setzero_si128();
    __m128i lo = PaethLanes(_mm_unpacklo_epi8(a, zero), _mm_unpacklo_epi8(b, zero), _mm_unpacklo_epi8(c, zero));
    __m128i hi = PaethLanes(_mm_unpackhi_epi8(a, zero), _mm_unpackhi_epi8(b, zero), _mm_unpackhi_epi8(c, zero));
    return _mm_packus_epi16(lo, hi);
}

inline __m128i Load16(const uint8_t* p) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
}

inline uint64_t LaneSum(__m128i sums) {
    uint64_t lanes[2];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(lanes), sums);
    return lanes[0] + lanes[1];
}
#endif

#if RAYOMD_PNG_AVX2
// The SSE2 helpers above on 32 bytes. A residual's magnitude read as a signed byte is its absolute
// value as one (-128 gives 128).
RAYOMD_AVX2 inline __m256i Cost32(__m256i value, __m256i predictor) {
    return _mm256_sad_epu8(_mm256_abs_epi8(_mm256_sub_epi8(value, predictor)), _mm256_setzero_si256());
}

RAYOMD_AVX2 inline __m256i Average32(__m256i a, __m256i b) {
    return _mm256_sub_epi8(_mm256_avg_epu8(a, b), _mm256_and_si256(_mm256_xor_si256(a, b), _mm256_set1_epi8(1)));
}

// Paeth on 32 bytes without widening. pa = |b - c| and pb = |a - c| fit a byte; pc = |a + b - 2c|
// is pa + pb when a - c and b - c have the same sign, which never makes pc the smallest, so a
// saturated sum compares the same, and |pa - pb| when the signs differ. Equal to the scalar
// predictor for all 2^24 inputs (2026-10-13).
RAYOMD_AVX2 inline __m256i Paeth32(__m256i a, __m256i b, __m256i c) {
    const __m256i pa = _mm256_sub_epi8(_mm256_max_epu8(b, c), _mm256_min_epu8(b, c));
    const __m256i pb = _mm256_sub_epi8(_mm256_max_epu8(a, c), _mm256_min_epu8(a, c));
    const __m256i bAbove = _mm256_cmpeq_epi8(_mm256_max_epu8(b, c), b);
    const __m256i aAbove = _mm256_cmpeq_epi8(_mm256_max_epu8(a, c), a);
    const __m256i pc = _mm256_blendv_epi8(_mm256_sub_epi8(_mm256_max_epu8(pa, pb), _mm256_min_epu8(pa, pb)),
        _mm256_adds_epu8(pa, pb), _mm256_cmpeq_epi8(bAbove, aAbove));
    const __m256i paSmallest = _mm256_and_si256(_mm256_cmpeq_epi8(_mm256_min_epu8(pa, pb), pa),
        _mm256_cmpeq_epi8(_mm256_min_epu8(pa, pc), pa));
    const __m256i bOrC = _mm256_blendv_epi8(c, b, _mm256_cmpeq_epi8(_mm256_min_epu8(pb, pc), pb));
    return _mm256_blendv_epi8(bOrC, a, paSmallest);
}

RAYOMD_AVX2 inline __m256i Load32(const uint8_t* p) {
    return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
}

RAYOMD_AVX2 inline uint64_t LaneSum32(__m256i sums) {
    alignas(32) uint64_t lanes[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), sums);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

bool UseAvx2() {
    static const bool avx2 = RayoMd::Cpu::HasAvx2();
    return avx2;
}
#endif

} // namespace

int ChoosePngFilterScalar(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp) {
    FilterCosts costs;
    costs.Add(row, up, 0, rowBytes, bpp);
    return costs.Best();
}

#if RAYOMD_PNG_SSE2
int ChoosePngFilterSse2(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp) {
    FilterCosts costs;
    size_t x = std::min(bpp, rowBytes);
    costs.Add(row, up, 0, x, bpp);
    __m128i s0 = _mm_setzero_si128(), s1 = s0, s2 = s0, s3 = s0, s4 = s0;
    for (; x + 16 <= rowBytes; x += 16) {
        __m128i v = Load16(row + x);
        __m128i a = Load16(row + x - bpp);
        __m128i b = Load16(up + x);
        __m128i c = Load16(up + x - bpp);
        s0 = _mm_add_epi64(s0, Cost16(v, _mm_setzero_si128()));
        s1 = _mm_add_epi64(s1, Cost16(v, a));
        s2 = _mm_add_epi64(s2, Cost16(v, b));
        s3 = _mm_add_epi64(s3, Cost16(v, Average16(a, b)));
        s4 = _mm_add_epi64(s4, Cost16(v, Paeth16(a, b, c)));
    }
    costs.Add(row, up, x, rowBytes, bpp);
    costs.sum[0] += LaneSum(s0);
    costs.sum[1] += LaneSum(s1);
    costs.sum[2] += LaneSum(s2);
    costs.sum[3] += LaneSum(s3);
    costs.sum[4] += LaneSum(s4);
    return costs.Best();
}
#endif

#if RAYOMD_PNG_AVX2
// ChoosePngFilterSse2 32 bytes a step: twice as fast on an RGB row of the 1024x1024 mascot.
RAYOMD_AVX2 static int ChoosePngFilterAvx2(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp) {
    FilterCosts costs;
    size_t x = std::min(bpp, rowBytes);
    costs.Add(row, up, 0, x, bpp);
    __m256i s0 = _mm256_setzero_si256(), s1 = s0, s2 = s0, s3 = s0, s4 = s0;
    for (; x + 32 <= rowBytes; x += 32) {
        __m256i v = Load32(row + x);
        __m256i a = Load32(row + x - bpp);
        __m256i b = Load32(up + x);
        __m256i c = Load32(up + x - bpp);
        s0 = _mm256_add_epi64(s0, Cost32(v, _mm256_setzero_si256()));
        s1 = _mm256_add_epi64(s1, Cost32(v, a));
        s2 = _mm256_add_epi64(s2, Cost32(v, b));
        s3 = _mm256_add_epi64(s3, Cost32(v, Average32(a, b)));
        s4 = _mm256_add_epi64(s4, Cost32(v, Paeth32(a, b, c)));
    }
    costs.Add(row, up, x, rowBytes, bpp);
    costs.sum[0] += LaneSum32(s0);
    costs.sum[1] += LaneSum32(s1);
    costs.sum[2] += LaneSum32(s2);
    costs.sum[3] += LaneSum32(s3);
    costs.sum[4] += LaneSum32(s4);
    return costs.Best();
}
#endif

int ChoosePngFilter(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp) {
#if RAYOMD_PNG_AVX2
    if (UseAvx2()) return ChoosePngFilterAvx2(row, up, rowBytes, bpp);
#endif
#if RAYOMD_PNG_SSE2
    return ChoosePngFilterSse2(row, up, rowBytes, bpp);
#else
    return ChoosePngFilterScalar(row, up, rowBytes, bpp);
#endif
}

// The residuals of bytes [from, to) under filter 1 to 4; the row's first `bpp` bytes have no left
// neighbour.
static void FilterBytes(int filter, const uint8_t* row, const uint8_t* up, size_t from, size_t to, size_t bpp,
    uint8_t* out) {
    size_t x = from;
    for (; x < to && x < bpp; x++) {
        int b = up[x];
        out[x] = (uint8_t)(row[x] - (filter == 1 ? 0 : filter == 3 ? b >> 1 : b));
    }
    switch (filter) {
    case 1:
        for (; x < to; x++) out[x] = (uint8_t)(row[x] - row[x - bpp]);
        break;
    case 2:
        for (; x < to; x++) out[x] = (uint8_t)(row[x] - up[x]);
        break;
    case 3:
        for (; x < to; x++) out[x] = (uint8_t)(row[x] - ((row[x - bpp] + up[x]) >> 1));
        break;
    default:
        for (; x < to; x++) out[x] = (uint8_t)(row[x] - Paeth(row[x - bpp], up[x], up[x - bpp]));
        break;
    }
}

void FilterPngRowScalar(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp,
    uint8_t* out) {
    if (filter == 0) std::memcpy(out, row, rowBytes);
    else FilterBytes(filter, row, up, 0, rowBytes, bpp, out);
}

#if RAYOMD_PNG_SSE2
void FilterPngRowSse2(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out) {
    if (filter == 0) {
        std::memcpy(out, row, rowBytes);
        return;
    }
    size_t x = std::min(bpp, rowBytes);
    FilterBytes(filter, row, up, 0, x, bpp, out);
    for (; x + 16 <= rowBytes; x += 16) {
        __m128i v = Load16(row + x);
        __m128i a = Load16(row + x - bpp);
        __m128i b = Load16(up + x);
        __m128i predictor = filter == 1 ? a : filter == 2 ? b : filter == 3 ? Average16(a, b)
            : Paeth16(a, b, Load16(up + x - bpp));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x), _mm_sub_epi8(v, predictor));
    }
    FilterBytes(filter, row, up, x, rowBytes, bpp, out);
}
#endif

#if RAYOMD_PNG_AVX2
RAYOMD_AVX2 static void FilterPngRowAvx2(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp,
    uint8_t* out) {
    if (filter == 0) {
        std::memcpy(out, row, rowBytes);
        return;
    }
    size_t x = std::min(bpp, rowBytes);
    FilterBytes(filter, row, up, 0, x, bpp, out);
    for (; x + 32 <= rowBytes; x += 32) {
        __m256i v = Load32(row + x);
        __m256i a = Load32(row + x - bpp);
        __m256i b = Load32(up + x);
        __m256i predictor = filter == 1 ? a : filter == 2 ? b : filter == 3 ? Average32(a, b)
            : Paeth32(a, b, Load32(up + x - bpp));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + x), _mm256_sub_epi8(v, predictor));
    }
    FilterBytes(filter, row, up, x, rowBytes, bpp, out);
}
#endif

void FilterPngRow(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out) {
#if RAYOMD_PNG_AVX2
    if (UseAvx2()) {
        FilterPngRowAvx2(filter, row, up, rowBytes, bpp, out);
        return;
    }
#endif
#if RAYOMD_PNG_SSE2
    FilterPngRowSse2(filter, row, up, rowBytes, bpp, out);
#else
    FilterPngRowScalar(filter, row, up, rowBytes, bpp, out);
#endif
}

static bool UnfilterBytes(uint8_t filter, const uint8_t* raw, const uint8_t* up, size_t rowBytes, size_t bpp,
    uint8_t* row) {
    size_t lead = std::min(bpp, rowBytes);
    switch (filter) {
    case 0:
        std::memcpy(row, raw, rowBytes);
        return true;
    case 1:
        std::memcpy(row, raw, lead);
        for (size_t x = lead; x < rowBytes; x++) row[x] = (uint8_t)(raw[x] + row[x - bpp]);
        return true;
    case 2:
        for (size_t x = 0; x < rowBytes; x++) row[x] = (uint8_t)(raw[x] + up[x]);
        return true;
    case 3:
        for (size_t x = 0; x < lead; x++) row[x] = (uint8_t)(raw[x] + (up[x] >> 1));
        for (size_t x = lead; x < rowBytes; x++) row[x] = (uint8_t)(raw[x] + ((row[x - bpp] + up[x]) >> 1));
        return true;
    case 4:
        for (size_t x = 0; x < lead; x++) row[x] = (uint8_t)(raw[x] + up[x]);
        for (size_t x = lead; x < rowBytes; x++) row[x] = (uint8_t)(raw[x] + Paeth(row[x - bpp], up[x], up[x - bpp]));
        return true;
    default:
        return false;
    }
}

#if RAYOMD_PNG_SSE2
template <size_t Bpp>
static inline __m128i LoadPixel(const uint8_t* p) {
    uint32_t value = 0;
    std::memcpy(&value, p, Bpp);
    return _mm_cvtsi32_si128((int)value);
}

template <size_t Bpp>
static inline void StorePixel(uint8_t* p, __m128i pixel) {
    uint32_t value = (uint32_t)_mm_cvtsi128_si32(pixel);
    std::memcpy(p, &value, Bpp);
}

// Sub, Average and Paeth depend on the pixel just decoded, so these take one 3- or 4-byte pixel
// per step, on 16-bit lanes; the first pixel's left and upper-left neighbours are zero.
template <size_t Bpp>
static void UnfilterPixels(uint8_t filter, const uint8_t* raw, const uint8_t* up, size_t rowBytes, uint8_t* row) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i low = _mm_set1_epi16(0xff);
    __m128i a = zero;
    __m128i c = zero;
    for (size_t x = 0; x < rowBytes; x += Bpp) {
        __m128i d = _mm_unpacklo_epi8(LoadPixel<Bpp>(raw + x), zero);
        __m128i predictor;
        if (filter == 1) {
            predictor = a;
        } else {
            __m128i b = _mm_unpacklo_epi8(LoadPixel<Bpp>(up + x), zero);
            predictor = filter == 3 ? _mm_srli_epi16(_mm_add_epi16(a, b), 1) : PaethLanes(a, b, c);
            c = b;
        }
        a = _mm_and_si128(_mm_add_epi16(d, predictor), low);
        StorePixel<Bpp>(row + x, _mm_packus_epi16(a, a));
    }
}

// Sub on four-byte pixels, 16 bytes a step: two shifted adds turn the step's residuals into
// running sums, and the last pixel of the step before carries over. Bytes wrap as the filter does.
static void UnfilterSub4(const uint8_t* raw, size_t rowBytes, uint8_t* row) {
    __m128i carry = _mm_setzero_si128();
    size_t x = 0;
    for (; x + 16 <= rowBytes; x += 16) {
        __m128i v = Load16(raw + x);
        v = _mm_add_epi8(v, _mm_slli_si128(v, 4));
        v = _mm_add_epi8(v, _mm_slli_si128(v, 8));
        v = _mm_add_epi8(v, carry);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(row + x), v);
        carry = _mm_shuffle_epi32(v, 0xFF);
    }
    for (; x < rowBytes; x++) row[x] = (uint8_t)(raw[x] + (x >= 4 ? row[x - 4] : 0));
}
#endif

#if RAYOMD_PNG_AVX2
// Paeth on 16-bit lanes with SSSE3's absolute value and SSE4.1's blends, which shorten the chain
// from one pixel to the next: 12.8 -> 9.1 ms on 2048 RGBA rows of 2048 pixels (2026-10-13).
RAYOMD_AVX2 inline __m128i PaethLanesBlend(__m128i a, __m128i b, __m128i c) {
    __m128i bc = _mm_sub_epi16(b, c);
    __m128i ac = _mm_sub_epi16(a, c);
    __m128i pa = _mm_abs_epi16(bc);
    __m128i pb = _mm_abs_epi16(ac);
    __m128i pc = _mm_abs_epi16(_mm_add_epi16(bc, ac));
    __m128i notA = _mm_or_si128(_mm_cmpgt_epi16(pa, pb), _mm_cmpgt_epi16(pa, pc));
    __m128i bOrC = _mm_blendv_epi8(b, c, _mm_cmpgt_epi16(pb, pc));
    return _mm_blendv_epi8(a, bOrC, notA);
}

template <size_t Bpp>
RAYOMD_AVX2 void UnfilterPaethAvx2(const uint8_t* raw, const uint8_t* up, size_t rowBytes, uint8_t* row) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i low = _mm_set1_epi16(0xff);
    __m128i a = zero;
    __m128i c = zero;
    for (size_t x = 0; x < rowBytes; x += Bpp) {
        const __m128i b = _mm_cvtepu8_epi16(LoadPixel<Bpp>(up + x));
        a = _mm_and_si128(_mm_add_epi16(_mm_cvtepu8_epi16(LoadPixel<Bpp>(raw + x)), PaethLanesBlend(a, b, c)), low);
        c = b;
        StorePixel<Bpp>(row + x, _mm_packus_epi16(a, a));
    }
}

// Two Paeth rows at once: lanes 0 to 3 hold row A's pixel k, lanes 4 to 7 row B's pixel k - 1,
// whose upper neighbour is row A's pixel k - 1, decoded the step before. Each step's chain is
// barely longer than one row's, for two pixels: 9.1 -> 6.7 ms on the rows above.
template <size_t Bpp>
RAYOMD_AVX2 void UnfilterPaethPairAvx2(const uint8_t* rawA, const uint8_t* rawB, const uint8_t* up, size_t rowBytes,
    uint8_t* rowA, uint8_t* rowB) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i low = _mm_set1_epi16(0xff);
    const size_t pixels = rowBytes / Bpp;
    __m128i a = zero;   // the pixels decoded the step before
    __m128i c = zero;
    for (size_t k = 0; k <= pixels; k++) {
        const bool inA = k < pixels;
        const __m128i upper = inA ? _mm_cvtepu8_epi16(LoadPixel<Bpp>(up + k * Bpp)) : zero;
        const __m128i residualA = inA ? LoadPixel<Bpp>(rawA + k * Bpp) : zero;
        const __m128i residualB = k > 0 ? LoadPixel<Bpp>(rawB + (k - 1) * Bpp) : zero;
        const __m128i d = _mm_cvtepu8_epi16(_mm_unpacklo_epi32(residualA, residualB));
        const __m128i b = _mm_unpacklo_epi64(upper, a);
        a = _mm_and_si128(_mm_add_epi16(d, PaethLanesBlend(a, b, c)), low);
        c = b;
        const __m128i bytes = _mm_packus_epi16(a, a);
        if (inA) StorePixel<Bpp>(rowA + k * Bpp, bytes);
        if (k > 0) StorePixel<Bpp>(rowB + (k - 1) * Bpp, _mm_srli_si128(bytes, 4));
    }
}
#endif

bool UnfilterPngRow(uint8_t filter, const uint8_t* raw, const uint8_t* up, size_t rowBytes, size_t bpp,
    uint8_t* row) {
#if RAYOMD_PNG_SSE2
    if (filter == 2) {
        size_t x = 0;
        for (; x + 16 <= rowBytes; x += 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(row + x), _mm_add_epi8(Load16(raw + x), Load16(up + x)));
        }
        for (; x < rowBytes; x++) row[x] = (uint8_t)(raw[x] + up[x]);
        return true;
    }
    if (filter == 1 && bpp == 4) {
        UnfilterSub4(raw, rowBytes, row);
        return true;
    }
    if ((filter == 1 || filter == 3 || filter == 4) && rowBytes % bpp == 0 && (bpp == 3 || bpp == 4)) {
#if RAYOMD_PNG_AVX2
        if (filter == 4 && UseAvx2()) {
            if (bpp == 4) UnfilterPaethAvx2<4>(raw, up, rowBytes, row);
            else UnfilterPaethAvx2<3>(raw, up, rowBytes, row);
            return true;
        }
#endif
        if (bpp == 4) UnfilterPixels<4>(filter, raw, up, rowBytes, row);
        else UnfilterPixels<3>(filter, raw, up, rowBytes, row);
        return true;
    }
#endif
    return UnfilterBytes(filter, raw, up, rowBytes, bpp, row);
}

bool UnfilterPngRows(uint8_t filterA, const uint8_t* rawA, uint8_t filterB, const uint8_t* rawB, const uint8_t* up,
    size_t rowBytes, size_t bpp, uint8_t* rowA, uint8_t* rowB) {
#if RAYOMD_PNG_AVX2
    if (filterA == 4 && filterB == 4 && (bpp == 3 || bpp == 4) && rowBytes % bpp == 0 && UseAvx2()) {
        if (bpp == 4) UnfilterPaethPairAvx2<4>(rawA, rawB, up, rowBytes, rowA, rowB);
        else UnfilterPaethPairAvx2<3>(rawA, rawB, up, rowBytes, rowA, rowB);
        return true;
    }
#endif
    return UnfilterPngRow(filterA, rawA, up, rowBytes, bpp, rowA) &&
        UnfilterPngRow(filterB, rawB, rowA, rowBytes, bpp, rowB);
}

} // namespace TinyPdf::Internal
