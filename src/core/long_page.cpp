// SPDX-License-Identifier: Apache-2.0

#include "long_page.h"

#include "export_options.h"
#include "../common/text_utils.h"

#include <algorithm>

namespace TinyPdf::Internal {

std::vector<double>* LongPageBottoms() {
    static thread_local std::vector<double> bottoms;
    bottoms.clear();
    return &bottoms;
}

void EndLongPage(std::vector<double>& bottoms, double lowest, double margin, double pageH) {
    bottoms.push_back(std::min(std::max(0.0, lowest - margin), pageH - kMinPageSide));
}

void AppendLongMediaBox(std::string& page, double width, double height, double bottom) {
    page += " 0 R /MediaBox [0 ";
    RayoMd::Text::AppendFixed2(page, bottom);
    page += ' ';
    RayoMd::Text::AppendFixed2(page, width);
    page += ' ';
    RayoMd::Text::AppendFixed2(page, height);
}

} // namespace TinyPdf::Internal
