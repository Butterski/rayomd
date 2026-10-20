// SPDX-License-Identifier: Apache-2.0

#pragma once

// Inlining hints for GCC and Clang; other compilers get plain `inline` or nothing.
// RAYOMD_HOT_INLINE: always inlined (helpers on per-byte and per-word paths, inlined even at -Os).
// RAYOMD_HOT_LAMBDA: the same for a lambda, after its parameter list.
// RAYOMD_NOINLINE: never inlined (rare paths kept out of hot callers).
// RAYOMD_COLD: never inlined and optimised for size.
// RAYOMD_SHARED: one copy of a sizeable helper several callers share (no inlining, no clones).
#if defined(__GNUC__) || defined(__clang__)
#define RAYOMD_HOT_INLINE inline __attribute__((always_inline))
#define RAYOMD_HOT_LAMBDA __attribute__((always_inline))
#define RAYOMD_NOINLINE __attribute__((noinline))
#define RAYOMD_COLD __attribute__((cold, noinline))
#if defined(__clang__)
#define RAYOMD_SHARED __attribute__((noinline))
#else
#define RAYOMD_SHARED __attribute__((noinline, noclone))
#endif
#else
#define RAYOMD_HOT_INLINE inline
#define RAYOMD_HOT_LAMBDA
#define RAYOMD_NOINLINE
#define RAYOMD_COLD
#define RAYOMD_SHARED
#endif
