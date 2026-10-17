// SPDX-License-Identifier: Apache-2.0

#pragma once

// The Mermaid diagrams the process has drawn (tiny_pdf.cpp, ImageRegistry::AddDiagram): a preview
// builds the same ones again on every edit, each costing 50 to 100 microseconds to lay out and
// convert. Kept by everything that decides the form, at most 16 MiB, all dropped when a new one
// would pass that. Out of tiny_pdf.cpp, whose inlining its containers would move. Built at -Os.

#include "mermaid.h"

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

} // namespace TinyPdf::Internal
