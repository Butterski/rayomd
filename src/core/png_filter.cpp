#include "png_filter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOMD_PNG_SSE2 1
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

} // namespace

int ChoosePngFilterScalar(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp) {
    FilterCosts costs;
    costs.Add(row, up, 0, rowBytes, bpp);
    return costs.Best();
}

int ChoosePngFilter(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp) {
#if RAYOMD_PNG_SSE2
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

void FilterPngRow(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out) {
#if RAYOMD_PNG_SSE2
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
#else
    FilterPngRowScalar(filter, row, up, rowBytes, bpp, out);
#endif
}

bool UnfilterPngRow(uint8_t filter, const uint8_t* raw, const uint8_t* up, size_t rowBytes, size_t bpp,
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

} // namespace TinyPdf::Internal
