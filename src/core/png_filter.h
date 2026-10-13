#pragma once

// PNG row filters (PNG specification, section 9): undoing them when RayoMD decodes a PNG, and
// choosing and applying them when it re-encodes decoded pixels as a /Predictor 15 image stream.
// One row per call; `up` is the row above, or zeros for the first row, and `bpp` the bytes per
// pixel (1 to 4 here). Choosing and applying run in AVX2 where the CPU has it and GCC or Clang
// builds, else in SSE2 where the target has it. The scalar functions are the reference that the
// vector ones must match byte for byte; the tests compare them, and the SSE2 ones too.

#include <cstddef>
#include <cstdint>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define RAYOMD_PNG_SSE2 1
#endif

namespace TinyPdf::Internal {

// The filter, 0 to 4, that the PNG encoder heuristic picks for `row`: the smallest sum of the
// residuals' magnitudes read as signed bytes, the lowest filter on ties.
int ChoosePngFilter(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp);
int ChoosePngFilterScalar(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp);
#if RAYOMD_PNG_SSE2
int ChoosePngFilterSse2(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp);
#endif

// Writes the residuals of `row` under `filter` (0 to 4) to `out`, rowBytes bytes.
void FilterPngRow(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out);
void FilterPngRowScalar(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp,
    uint8_t* out);
#if RAYOMD_PNG_SSE2
void FilterPngRowSse2(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out);
#endif

// Undoes `filter` on the residuals `raw` into `row`. False for a filter past 4.
bool UnfilterPngRow(uint8_t filter, const uint8_t* raw, const uint8_t* up, size_t rowBytes, size_t bpp,
    uint8_t* row);

} // namespace TinyPdf::Internal
