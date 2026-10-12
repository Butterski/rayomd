#pragma once

// PNG row filters (PNG specification, section 9): undoing them when RayoMD decodes a PNG, and
// choosing and applying them when it re-encodes decoded pixels as a /Predictor 15 image stream.
// One row per call; `up` is the row above, or zeros for the first row, and `bpp` the bytes per
// pixel (1 to 4 here). SSE2 where the target has it. The scalar functions are the reference that
// the vector ones must match byte for byte; the tests compare them.

#include <cstddef>
#include <cstdint>

namespace TinyPdf::Internal {

// The filter, 0 to 4, that the PNG encoder heuristic picks for `row`: the smallest sum of the
// residuals' magnitudes read as signed bytes, the lowest filter on ties.
int ChoosePngFilter(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp);
int ChoosePngFilterScalar(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp);

// Writes the residuals of `row` under `filter` (0 to 4) to `out`, rowBytes bytes.
void FilterPngRow(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out);
void FilterPngRowScalar(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp,
    uint8_t* out);

// Undoes `filter` on the residuals `raw` into `row`. False for a filter past 4.
bool UnfilterPngRow(uint8_t filter, const uint8_t* raw, const uint8_t* up, size_t rowBytes, size_t bpp,
    uint8_t* row);

} // namespace TinyPdf::Internal
