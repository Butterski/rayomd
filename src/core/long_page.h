// SPDX-License-Identifier: Apache-2.0

#pragma once

// One long page (PdfPageSize::autoHeight): the renderers lay out on pages kMaxPageSide high and
// note where each page's layout ended; its box then starts a margin below that, and no coordinate
// moves. Kept apart from the engine (tiny_pdf.cpp), which sits at GCC's inlining limit, so that
// pages of a fixed size keep their code as well as their bytes.

#include <string>
#include <vector>

namespace TinyPdf::Internal {

// The box bottoms of a build's long pages, empty: kept for the thread (a build has one renderer
// at a time), so a renderer holds only a pointer, null for pages of a fixed size.
std::vector<double>* LongPageBottoms();
// Notes the end of a long page whose layout ended at `lowest`, the cursor: what reaches below it
// into the margin, as on any page (descenders, quote strips, code tiles), stays inside a box that
// starts `margin` lower, but at 0 at the lowest and at least kMinPageSide below the top, `pageH`.
void EndLongPage(std::vector<double>& bottoms, double lowest, double margin, double pageH);
// " 0 R /MediaBox [0 bottom width height": the end of a page's "/Parent n" and the start of its box.
void AppendLongMediaBox(std::string& page, double width, double height, double bottom);

} // namespace TinyPdf::Internal
