// SPDX-License-Identifier: Apache-2.0

#pragma once

// --black-text (PdfOptions::blackText): the colours of finished page content turned into greys
// for a black-and-white printer, in place. Every "r g b rg" or "r g b RG" becomes DeviceGray of
// its luminance Y = 0.299 r + 0.587 g + 0.114 b, every "v g" or "v G" stays grey: below 0.5 as
// black ("0 g"), else at Y, so text, links, bars and code turn black and fills stay light. The
// replacement is written over the operator and its operands and padded with spaces, so offsets
// and stream lengths do not move. The tokenizer reads the content RayoMD writes: literal strings
// with escapes, hex strings, "<< >>" property lists, names, numbers; other operands pass through.
// Built on its own, out of tiny_pdf.cpp's inlining budget: only this mode runs it.

#include <cstddef>
#include <string>
#include <vector>

namespace TinyPdf::Internal {

// The colour operators of the content in [begin, end) as greys.
void PrintColors(char* begin, char* end);

inline void PrintColors(std::string& stream) {
    if (!stream.empty()) PrintColors(&stream[0], &stream[0] + stream.size());
}

// A build's content: its pages, rendered into `pdf` from `starts[0]` to its end, and the streams
// laid over them (a theme's bands, its cover, the table of contents' numbers).
void PrintColors(std::string& pdf, const std::vector<size_t>& starts, std::vector<std::string>& bands,
    std::string& cover, std::vector<std::string>& overlays);

} // namespace TinyPdf::Internal
