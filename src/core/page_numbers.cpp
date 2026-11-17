// SPDX-License-Identifier: Apache-2.0

#include "page_numbers.h"

#include "math_layout.h"
#include "print_colors.h"
#include "../common/text_utils.h"
#include "../common/thread_object.h"

#include <cassert>
#include <charconv>
#include <cstring>

namespace TinyPdf::Internal {

namespace {

void Append(std::string& out, std::string_view text) {
    out.append(text.data(), text.size());
}

} // namespace

const PageNumberObjects& WritePageNumbers(size_t pages, double margin, double pageWidth, const char* font,
    const double* bottoms, bool black) {
    RAYOMD_THREAD_OBJECT(PageNumberObjects, objects);
    std::string& bytes = objects.bytes;
    std::vector<size_t>& ends = objects.ends;
    constexpr double kSize = 9.0;
    // The stream up to the number's left edge. --black-text turns its grey into black over the same
    // bytes; nothing after it has a colour.
    std::string head = "q 0.45 0.45 0.45 rg BT /";
    head += font;
    head += " 9 Tf 1 0 0 1 ";
    if (black) PrintColors(head);
    // The baseline on pages of a fixed size, which start at 0.
    std::string fixedBaseline;
    if (bottoms == nullptr) RayoMd::Text::AppendFixed2(fixedBaseline, 0.0 + margin * 0.5 - 3.0);
    std::string left;       // the number's left edge, for numbers of the label's digits
    std::string baseline;   // a long page's baseline
    // Everything of an object before its number: "<<  /Length n >>\nstream\n" (PdfObjects::StreamHead
    // with no dictionary), `head`, the left edge, the baseline and " Tm (". On pages of a fixed size
    // it is the same for every number of as many digits, so it is written at 1, 10, 100 and on.
    std::string prefix;

    // A buffer far larger than this build needs (one of a much longer document) is given back.
    const size_t wanted = pages * (head.size() + 72);
    if (bytes.capacity() > (size_t(1) << 20) && wanted < bytes.capacity() / 4) {
        std::string().swap(bytes);
        std::vector<size_t>().swap(ends);
    }
    bytes.clear();
    bytes.reserve(wanted);
    ends.resize(pages);
    // The label counts up in place, a carry at a time, instead of being converted from the page.
    char label[21] = { '1' };   // the digits of a size_t
    size_t digits = 1;
    size_t widthDigits = 0;     // the digits `left` was measured for
    for (size_t page = 1; page <= pages; page++) {
        const std::string_view text(label, digits);
        // Helvetica's digits are all 556 units wide: every number of as many digits has one width.
        assert(widthDigits != digits || StandardTextWidth(text, kSize, StandardTextFont::Regular) ==
            StandardTextWidth(std::string_view("1000000000000000000000", digits), kSize, StandardTextFont::Regular));
        const bool newWidth = widthDigits != digits;
        if (newWidth) {
            widthDigits = digits;
            left.clear();
            RayoMd::Text::AppendFixed2(left, (pageWidth - StandardTextWidth(text, kSize, StandardTextFont::Regular)) * 0.5);
        }
        if (bottoms != nullptr) {
            baseline.clear();
            RayoMd::Text::AppendFixed2(baseline, bottoms[page - 1] + margin * 0.5 - 3.0);
        }
        if (newWidth || bottoms != nullptr) {
            const std::string_view y = bottoms != nullptr ? baseline : fixedBaseline;
            const size_t length = head.size() + left.size() + 1 + y.size() + 5 + digits + 9;
            char lengthText[20];
            prefix.assign("<<  /Length ");
            prefix.append(lengthText, (size_t)(std::to_chars(lengthText, lengthText + sizeof lengthText, length).ptr - lengthText));
            Append(prefix, " >>\nstream\n");
            Append(prefix, head);
            Append(prefix, left);
            prefix += ' ';
            Append(prefix, y);
            Append(prefix, " Tm (");
        }
        Append(bytes, prefix);
        Append(bytes, text);
        Append(bytes, ") Tj ET Q\nendstream");
        ends[page - 1] = bytes.size();

        size_t at = digits;
        while (at > 0 && label[at - 1] == '9') label[--at] = '0';
        if (at == 0) {
            memmove(label + 1, label, digits++);
            label[0] = '1';
        } else {
            label[at - 1]++;
        }
    }
    return objects;
}

} // namespace TinyPdf::Internal
