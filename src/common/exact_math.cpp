// SPDX-License-Identifier: Apache-2.0

#include "exact_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(__linux__) && defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace RayoMd::ExactMath {

#if defined(__linux__)

namespace {

constexpr uint64_t kSign = 0x8000000000000000ull;
constexpr uint64_t kMantissa = 0x000FFFFFFFFFFFFFull;
constexpr double kTwo52 = 4503599627370496.0;

uint64_t Bits(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double FromBits(uint64_t bits) {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double Abs(double value) { return FromBits(Bits(value) & ~kSign); }

// 2^exponent, for -1074 <= exponent <= 1023.
double Power2(int exponent) {
    return exponent >= -1022 ? FromBits((uint64_t)(exponent + 1023) << 52) : FromBits(1ull << (exponent + 1074));
}

// x rounded half away from zero, in the integer type T, from the bits of x: the significand
// plus half a unit, shifted. Past T's range, infinity and NaN give T's minimum, which is what
// glibc's conversion (cvttsd2si) gives there.
template <class T>
T RoundHalfAway(double x) {
    const uint64_t bits = Bits(x);
    const bool negative = (bits & kSign) != 0;
    const int exponent = static_cast<int>((bits >> 52) & 0x7FF) - 1023;
    if (exponent < 0) return exponent == -1 ? (negative ? -1 : 1) : 0;   // |x| < 1
    if (exponent >= std::numeric_limits<T>::digits) return std::numeric_limits<T>::min();
    uint64_t magnitude = (bits & kMantissa) | (1ull << 52);
    magnitude = exponent >= 52 ? magnitude << (exponent - 52)
                               : (magnitude + (1ull << (51 - exponent))) >> (52 - exponent);
    return negative ? static_cast<T>(-static_cast<int64_t>(magnitude)) : static_cast<T>(magnitude);
}

} // namespace

double Trunc(double x) {
    // From 2^52 on every double is a whole number; infinities and NaN stay as they are.
    if (!(Abs(x) < kTwo52)) return x;
    // The conversion drops the fraction exactly; the sign bit keeps -0.0 for -0.5 < x <= -0.0.
    return FromBits(Bits(static_cast<double>(static_cast<int64_t>(x))) | (Bits(x) & kSign));
}

double Floor(double x) {
    const double whole = Trunc(x);
    return whole > x ? whole - 1.0 : whole;
}

double Ceil(double x) {
    const double whole = Trunc(x);
    return whole < x ? whole + 1.0 : whole;
}

long LRound(double x) { return RoundHalfAway<long>(x); }

long long LlRound(double x) { return RoundHalfAway<long long>(x); }

double Sqrt(double x) {
#if defined(__SSE2__)
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x)));
#elif defined(__aarch64__)
    double root;
    __asm__("fsqrt %d0, %d1" : "=w"(root) : "w"(x));
    return root;
#else
    return std::sqrt(x);
#endif
}

// The remainder of x / y with the sign of x, exact: the integer significands are reduced
// modulo y's, eleven bits of x's exponent a step, and the remainder scaled back.
double Fmod(double x, double y) {
    const uint64_t ux = Bits(x);
    const uint64_t uy = Bits(y);
    const uint64_t ax = ux & ~kSign;
    const uint64_t ay = uy & ~kSign;
    const uint64_t infinity = 0x7FF0000000000000ull;
    if (ay == 0 || ay > infinity || ax >= infinity) return (x * y) / (x * y);
    if (ax < ay) return x;
    const int bx = static_cast<int>(ax >> 52);
    const int by = static_cast<int>(ay >> 52);
    const uint64_t mx = bx != 0 ? (ax & kMantissa) | (1ull << 52) : ax;
    const uint64_t my = by != 0 ? (ay & kMantissa) | (1ull << 52) : ay;
    const int ex = (bx != 0 ? bx : 1) - 1075;
    const int ey = (by != 0 ? by : 1) - 1075;
    // |x| >= |y| makes ex >= ey: a subnormal x has a subnormal y.
    uint64_t remainder = mx % my;
    for (int left = ex - ey; left > 0;) {
        const int shift = left < 11 ? left : 11;
        remainder = (remainder << shift) % my;
        left -= shift;
    }
    // remainder * 2^ey is representable (below |y|, a multiple of y's last bit): the product is exact.
    const double magnitude = remainder == 0 ? 0.0 : static_cast<double>(remainder) * Power2(ey);
    return FromBits(Bits(magnitude) | (ux & kSign));
}

#else

double Ceil(double x) { return std::ceil(x); }
double Floor(double x) { return std::floor(x); }
double Trunc(double x) { return std::trunc(x); }
long LRound(double x) { return std::lround(x); }
long long LlRound(double x) { return std::llround(x); }
double Sqrt(double x) { return std::sqrt(x); }
double Fmod(double x, double y) { return std::fmod(x, y); }

#endif

} // namespace RayoMd::ExactMath
