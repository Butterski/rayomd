// SPDX-License-Identifier: Apache-2.0

#pragma once

// --page-numbers: "N" centred in the bottom margin of each page, small and grey, in Helvetica. A
// content stream of its own after the page's: a long page knows its bottom only once it is laid
// out, and a session's page dictionaries (AddSessionPage) name the number's stream apart from the
// page's. A build's number objects are written one after another into a buffer kept for the
// thread, which the PDF's objects view, so a page costs no allocation (a build in steady state
// none at all). On pages of a fixed size an object's text before its number is the same for every
// number of as many digits (Helvetica's digits are all as wide) and is formatted once for each
// count of digits; the label counts up in place. Kept apart from the engine (tiny_pdf.cpp), which
// sits at GCC's inlining limit.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

class PageNumberObjects {
public:
    // The whole body of the object of page `index` (from 0): "<<  /Length n >>\nstream\n",
    // the content and "\nendstream", as PdfObjects::AddStream writes a stream.
    std::string_view Object(size_t index) const {
        const size_t begin = index == 0 ? 0 : ends[index - 1];
        return std::string_view(bytes).substr(begin, ends[index] - begin);
    }

private:
    friend const PageNumberObjects& WritePageNumbers(size_t, double, double, const char*, const double*, bool);
    std::string bytes;           // every page's object, one after another
    std::vector<size_t> ends;    // where each ends in `bytes`
};

// The objects of pages 1 to `pages`, in the thread's buffer: valid until the thread's next call,
// so past the build's BuildInto. `font` is the name the pages give Helvetica, `bottoms` the
// bottom of each long page's box (Renderer::PageBottoms; nullptr for pages of a fixed size, which
// start at 0), and with `black` the grey is the black of --black-text.
const PageNumberObjects& WritePageNumbers(size_t pages, double margin, double pageWidth, const char* font,
    const double* bottoms, bool black);

} // namespace TinyPdf::Internal
