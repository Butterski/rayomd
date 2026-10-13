#pragma once

// A DEFLATE (RFC 1951) encoder in the zlib format (RFC 1950), for the FlateDecode streams of
// PDF files: page content, font subsets, CMaps. One call compresses a whole buffer; no
// dependency. Greedy matching over two hash tables (8-byte and 4-byte keys) and, per block of at
// most 64 KB, the cheapest of a stored, a fixed-code and a dynamic-code block by exact bit
// count. Output depends only on the input.

#include <algorithm>
#include <atomic>
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

// How many of an input's first bytes another thread has written, for ZlibCompressImage to start
// before the input is complete: the writer stores the count with release order after the bytes,
// or kAbandoned, which makes the encoder stop and return 0.
struct InputProgress {
    static constexpr size_t kAbandoned = ~size_t(0);
    std::atomic<size_t> written{ 0 };
};

// ZlibCompress for the rows of a PNG-predicted image (/Predictor 15): the same format, bounds and
// scratch, but only the 8-byte table is searched. On filtered pixels the 4-byte table finds short
// distant matches that cost as many bits as the literals they replace and, where they break up
// runs of literals, up to six times the time (2026-10-13, docs/development/performance.md). With
// `progress`, each 64 KB block waits until its bytes, and the longest match past them, are
// written; the output is the same as for the whole input at once.
size_t ZlibCompressImage(std::string_view input, uint8_t* out, Scratch& scratch,
    const InputProgress* progress = nullptr);

// The Adler-32 checksum (RFC 1950) of `size` bytes, as zlib's adler32(adler, data, size): the
// trailer of a zlib stream. Start with 1; pass the result on to continue over more bytes. SSE2
// where the target has it.
uint32_t Adler32(const uint8_t* data, size_t size, uint32_t adler = 1);

} // namespace RayoMd::Flate
