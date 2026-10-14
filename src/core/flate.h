// SPDX-License-Identifier: Apache-2.0

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

// The rows of a PNG-predicted image (/Predictor 15) are compressed in segments, each on its own
// and in any order or thread, the stream being the zlib header, the segments in order, and the
// Adler-32 of all the rows. A segment's matches reach back into the kImageSegmentHistory bytes
// before it, not past its end, and a segment but the last ends on a byte with an empty stored
// block, so the bytes do not depend on how many threads compress. Only the 8-byte table is
// searched: on filtered pixels the 4-byte table finds short distant matches that cost as many
// bits as the literals they replace and up to six times the time. On the test images the
// segments cost 1.7 % of the time and 0.02 % of the size of one segment for a whole plane; 8 KiB
// of history saved 0.01 % of the size for 3 % more time (2026-10-13).
constexpr size_t kImageSegmentHistory = size_t(2) << 10;

// The size of the next segment when `remaining` bytes are left: up to 256 KiB one, then 1 MiB
// ones while more than 2 MiB are left, then each half the rest, down to 128 KiB, so that the
// segment left when the last rows arrive is small.
inline size_t ImageSegmentSize(size_t remaining) {
    constexpr size_t kSmall = size_t(256) << 10;
    constexpr size_t kLarge = size_t(1) << 20;
    if (remaining <= kSmall) return remaining;
    if (remaining > 2 * kLarge) return kLarge;
    return std::max(kSmall / 2, (remaining + 1) / 2);
}

// The most bytes DeflateImageSegment writes for a segment of `size` bytes, plus its 8 bytes of
// slack.
inline size_t ImageSegmentBound(size_t size) {
    const size_t blocks = std::max<size_t>(1, (size + 65277) / 65278);
    return size + 5 * blocks + 5 + 8;
}

// The bytes of a buffer that another thread is still writing.
struct InputProgress {
    static constexpr size_t kAbandoned = ~size_t(0);
    const std::atomic<size_t>* written = nullptr;   // so far, stored with release order after them, or
                                                   // kAbandoned when no more will be
    size_t offset = 0;                              // where the compressed data starts in that buffer
};

// Raw DEFLATE blocks of the segment data[history, history + size): `data` starts `history` bytes
// before the segment (at most kImageSegmentHistory). Writes at most ImageSegmentBound(size) - 8
// bytes to `out`; returns how many. `last` gives the final block. With `progress`, each block
// waits for its bytes, and an abandoned buffer returns 0.
size_t DeflateImageSegment(const uint8_t* data, size_t history, size_t size, bool last, uint8_t* out,
    Scratch& scratch, const InputProgress* progress = nullptr);

// The most bytes ZlibCompressImage writes for `size` input bytes, plus 8 bytes of slack.
inline size_t ZlibImageBound(size_t size) {
    size_t bound = 2 + 4 + 8;
    size_t start = 0;
    do {
        const size_t segment = ImageSegmentSize(size - start);
        bound += ImageSegmentBound(segment) - 8;
        start += segment;
    } while (start < size);
    return bound;
}

// The whole stream of an image's rows, its segments compressed in turn: the bytes that segments
// compressed in any threads and put together make. `out` has room for ZlibImageBound(input.size())
// bytes.
size_t ZlibCompressImage(std::string_view input, uint8_t* out, Scratch& scratch);

// The Adler-32 of two byte runs one after the other, from their own: zlib's adler32_combine.
uint32_t Adler32Combine(uint32_t first, uint32_t second, size_t secondSize);

// The Adler-32 checksum (RFC 1950) of `size` bytes, as zlib's adler32(adler, data, size): the
// trailer of a zlib stream. Start with 1; pass the result on to continue over more bytes. SSE2
// where the target has it.
uint32_t Adler32(const uint8_t* data, size_t size, uint32_t adler = 1);

} // namespace RayoMd::Flate
