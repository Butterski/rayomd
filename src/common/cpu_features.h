// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the vector code dispatches on, asked of the CPU with cpuid (GCC and Clang on x86), each
// caller keeping the answer in a static. __builtin_cpu_supports gives the same answers but links
// libgcc's feature table (5 KB of code and data), whose constructor runs cpuid several times at
// the start of every process.

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>

namespace RayoMd::Cpu {

// AVX2 that the operating system saves (XCR0's SSE and AVX state), as
// __builtin_cpu_supports("avx2") answers; with `bmi`, also BMI1.
inline bool HasAvx2(bool bmi = false) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & bit_OSXSAVE)) return false;
    unsigned low = 0, high = 0;
    __asm__("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    if ((low & 6) != 6) return false;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
    return (b & bit_AVX2) && (!bmi || (b & bit_BMI));
}

} // namespace RayoMd::Cpu
#endif
