#pragma once

// Rounding, square root and remainder without libm. Each has exactly one correct result for
// every input (IEEE 754), so any correct implementation returns libm's bits; NaN, infinity,
// -0.0 and out-of-range conversions come out as glibc's do on x86-64 (only Fmod by a NaN, which
// RayoMD never divides by, may give a NaN of another sign), and tests/core_tests.cpp compares
// every function with libm. On Linux they are RayoMD's own, so the dynamic build does
// not load libm.so.6 (0.07 ms and 10 page faults per process); elsewhere they call <cmath>.
// Use these in RayoMD's code instead of std::ceil, std::floor, std::trunc, std::lround,
// std::llround, std::sqrt and std::fmod. The transcendental functions SVG needs stay glibc's
// (svg.cpp loads them when an SVG first needs one).

// Functions of their arguments alone: declared so, a call does not make the caller reload what
// it keeps in memory, as one of libm's builtins does not.
#if defined(__GNUC__)
#define RAYOMD_EXACT_MATH_FN __attribute__((const))
#else
#define RAYOMD_EXACT_MATH_FN
#endif

namespace RayoMd::ExactMath {

RAYOMD_EXACT_MATH_FN double Ceil(double x);
RAYOMD_EXACT_MATH_FN double Floor(double x);
RAYOMD_EXACT_MATH_FN double Trunc(double x);
RAYOMD_EXACT_MATH_FN long LRound(double x);         // half away from zero
RAYOMD_EXACT_MATH_FN long long LlRound(double x);   // half away from zero
RAYOMD_EXACT_MATH_FN double Sqrt(double x);
RAYOMD_EXACT_MATH_FN double Fmod(double x, double y);

} // namespace RayoMd::ExactMath
