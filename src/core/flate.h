#pragma once

// A DEFLATE (RFC 1951) encoder in the zlib format (RFC 1950), for the FlateDecode streams of
// PDF files: page content, font subsets, CMaps. One call compresses a whole buffer; no
// dependency. Greedy matching over two hash tables (8-byte and 4-byte keys) and, per block of at
// most 64 KB, the cheapest of a stored, a fixed-code and a dynamic-code block by exact bit
// count. Output depends only on the input.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace RayoMd::Flate {

// The most bytes ZlibCompress writes for `size` input bytes (stored blocks, at worst), plus the
// 8 bytes its whole-word stores may touch past the end of the output.
inline size_t ZlibBound(size_t size) {
    const size_t blocks = std::max<size_t>(1, (size + 65277) / 65278);
    return size + 5 * blocks + 6 + 8;
}

// Working memory for ZlibCompress, kept from call to call: up to about 600 KB. One per thread.
struct Scratch {
    std::vector<uint32_t> longTable;    // two most recent positions per bucket, keyed on 8 bytes
    std::vector<uint32_t> shortTable;   // most recent position per slot, keyed on 4 bytes
    std::vector<uint32_t> tokens;       // literal byte, or length << 16 | (distance - 1)
    uint32_t base = 0;                  // table position of the next input's first byte
};

// The largest input ZlibCompress takes.
constexpr size_t kMaxInput = (size_t(1) << 31) - 1;

// Compresses `input`, at most kMaxInput bytes, to `out`, which has room for
// ZlibBound(input.size()) bytes; returns the number of bytes written (at most
// ZlibBound(input.size()) - 8).
size_t ZlibCompress(std::string_view input, uint8_t* out, Scratch& scratch);

} // namespace RayoMd::Flate
