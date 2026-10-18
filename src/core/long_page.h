// SPDX-License-Identifier: Apache-2.0

#pragma once

// One long page (PdfPageSize::autoHeight): the renderers lay out on pages kMaxPageSide high, from
// their top at kLongPageTop down to below 0, and note where each page's layout ended; its box then
// starts a margin below that. Kept apart from the engine (tiny_pdf.cpp), which sits at GCC's
// inlining limit, so that pages of a fixed size keep their code as well as their bytes.

#include "export_options.h"

#include <string>
#include <vector>

namespace TinyPdf::Internal {

// The top of a long page. Its layout reaches kMaxPageSide below, to kLongPageTop - kMaxPageSide,
// so every coordinate on it stays under 10,000 in magnitude, which WriteFixed2 writes without a
// division per digit; from 14,400 down to 0, a third of them took std::to_chars (+2.7 %
// instructions on tester-noimg). Whole, so that rounding to hundredths moves nothing.
constexpr double kLongPageTop = 9900.0;

// The box bottoms of a build's long pages, empty: kept for the thread (a build has one renderer
// at a time), so a renderer holds only a pointer, null for pages of a fixed size.
std::vector<double>* LongPageBottoms();
// Notes the end of a long page whose layout ended at `lowest`, the cursor: what reaches below it
// into the margin, as on any page (descenders, quote strips, code tiles), stays inside a box that
// starts `margin` lower, but kMaxPageSide below the top at the lowest and kMinPageSide at least.
void EndLongPage(std::vector<double>& bottoms, double lowest, double margin);
// " 0 R /MediaBox [0 bottom width kLongPageTop": the end of a page's "/Parent n" and the start of
// its box.
void AppendLongMediaBox(std::string& page, double width, double bottom);

} // namespace TinyPdf::Internal
