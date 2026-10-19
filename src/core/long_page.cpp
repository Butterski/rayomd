// SPDX-License-Identifier: Apache-2.0

#include "long_page.h"

#include "../common/text_utils.h"
#include "../common/thread_object.h"

#include <algorithm>
#include <string_view>

namespace TinyPdf::Internal {

std::vector<double>* LongPageBottoms() {
    RAYOMD_THREAD_OBJECT(std::vector<double>, bottoms);
    bottoms.clear();
    return &bottoms;
}

void EndLongPage(std::vector<double>& bottoms, double lowest, double margin) {
    bottoms.push_back(std::min(std::max(kLongPageTop - kMaxPageSide, lowest - margin), kLongPageTop - kMinPageSide));
}

void AppendLongMediaBox(std::string& page, double width, double bottom) {
    constexpr std::string_view kHead = " 0 R /MediaBox [0 ";
    char box[kHead.size() + 3 * (RayoMd::Text::kFixed2MaxChars + 1)];   // three numbers and their spaces
    char* end = std::copy(kHead.begin(), kHead.end(), box);
    end = RayoMd::Text::WriteFixed2(end, bottom);
    *end++ = ' ';
    end = RayoMd::Text::WriteFixed2(end, width);
    *end++ = ' ';
    end = RayoMd::Text::WriteFixed2(end, kLongPageTop);
    page.append(box, static_cast<size_t>(end - box));
}

} // namespace TinyPdf::Internal
