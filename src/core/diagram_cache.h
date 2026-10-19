// SPDX-License-Identifier: Apache-2.0

#pragma once

// The forms the process has drawn for documents' renderers (tiny_pdf.cpp's ImageRegistry): Mermaid
// diagrams (AddDiagram) and SVG files (Decode). A preview builds the same ones again on every edit:
// a diagram costs 50 to 100 microseconds to lay out and convert, an SVG file of 10 KB 0.2 ms to read
// and convert, one with a data: PNG of 250 KB 1 ms. Kept by everything that decides the form, at
// most 16 MiB of diagrams and 32 MiB of SVG files, each kind all dropped when a new one would pass
// that. Out of tiny_pdf.cpp, whose inlining its containers would move. Built at -Os.

#include "mermaid.h"
#include "../common/file_io.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

// A run of a form's text in the Unicode renderer: in a theme's bold face or the text face. A
// cached diagram replays its runs into the document's glyph sets, so its fonts are subset as they
// were for the diagram's first build.
struct SvgTextRun {
    bool bold = false;
    std::wstring text;
};

struct DrawnDiagram {
    std::shared_ptr<const void> image;   // the PdfImage, opaque here
    std::vector<SvgTextRun> runs;
};

// What decides a diagram's form: the renderer (the Unicode one with its fonts, by their load,
// TtfFont::loadId; the bold face's 0 without one), the palette and the source.
struct DiagramKey {
    bool unicode = false;
    uint64_t textFont = 0;
    uint64_t boldFont = 0;
    const Mermaid::Palette* palette = nullptr;
    std::string_view source;
};

std::shared_ptr<const DrawnDiagram> FindDrawnDiagram(const DiagramKey& key);
// Keeps `diagram`, whose form's streams take `formBytes`; nothing past 4 MiB.
void KeepDrawnDiagram(const DiagramKey& key, std::shared_ptr<const DrawnDiagram> diagram, size_t formBytes);
// One more diagram laid out for a document (Mermaid::Layouts).
void CountDiagramLayout();
// The containers' work here, out of tiny_pdf.cpp: a new diagram to fill, and a run noted.
std::shared_ptr<DrawnDiagram> NewDrawnDiagram();
void NoteSvgTextRun(std::vector<SvgTextRun>& runs, bool bold, const std::wstring& text);

// An SVG file's (or URL's) form as a renderer converted it, and the data: images it registered
// before it (ImageRegistry::AddDecoded): the document's images from `base` on, which the form names
// by their index. `stamp` is the file's when it was read (none for a URL).
struct ConvertedSvg : DrawnDiagram {
    std::vector<std::shared_ptr<const void>> images;   // PdfImages, opaque here
    size_t base = 0;
    RayoMd::File::Stamp stamp;
};

// What decides an SVG file's form besides its bytes, which the stamp stands for: the renderer, as
// for a diagram, and the file (the registry's key: its canonical path, or its URL).
struct SvgFileKey {
    bool unicode = false;
    uint64_t textFont = 0;
    uint64_t boldFont = 0;
    std::string_view file;
};

// The form kept for `key` read with `stamp` whose images went at `base`, or null.
std::shared_ptr<const ConvertedSvg> FindConvertedSvg(const SvgFileKey& key, const RayoMd::File::Stamp& stamp, size_t base);
// Keeps `svg` in place of what the file had, its form and images taking `bytes`; nothing past 8 MiB.
void KeepConvertedSvg(const SvgFileKey& key, std::shared_ptr<const ConvertedSvg> svg, size_t bytes);
std::shared_ptr<ConvertedSvg> NewConvertedSvg();
// One more SVG file converted for a document (SvgFileConversions).
void CountSvgFileConversion();

} // namespace TinyPdf::Internal
