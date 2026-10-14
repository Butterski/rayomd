// SPDX-License-Identifier: Apache-2.0

#pragma once

// Re-encoded PNGs kept between processes (PdfOptions::imageCacheDirectory): a file per image, named
// by the SHA-256 of the PNG's bytes, holds the RayoMD version that wrote it, the encoding's
// generation, the image's size and colour components, its colour and soft-mask streams, and an
// Adler-32 of all that. Only those streams reach a PDF; the caller writes the dictionaries. A file
// that is short, of another version or generation, of another size or whose checksum fails is a
// miss. A file is written under a temporary name and renamed into place, so no process reads half
// of one, and the directory is trimmed, least recently used first, past about 256 MB. Cold code.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace TinyPdf::Internal {

// Images under this many pixels are not kept: a 128x128 one is re-encoded in 0.26 ms, about
// what finding, reading and checking its file would take (2026-10-13).
constexpr size_t kImageCacheMinPixels = size_t(1) << 15;

// The streams kept for the PNG whose SHA-256 is `digest` (64 hex digits), if `directory` holds a
// sound entry for an image of that size and number of colour components, of streams within
// `maxBytes`. A use makes the entry the most recent. `fast`: the entry of streams compressed in
// --fast-images' mode, a file of its own with a magic of its own.
bool LoadCachedImage(const std::string& directory, const std::string& digest, uint32_t width, uint32_t height,
    uint32_t components, size_t maxBytes, std::string& color, std::string& mask, bool fast = false);

// Keeps those streams, when they are within `maxBytes`; any failure leaves the directory as it was.
void StoreCachedImage(const std::string& directory, const std::string& digest, uint32_t width, uint32_t height,
    uint32_t components, size_t maxBytes, std::string_view color, std::string_view mask, bool fast = false);

} // namespace TinyPdf::Internal
