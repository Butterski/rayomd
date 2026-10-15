// SPDX-License-Identifier: Apache-2.0

// A one-page document built through rayomd::engine, as another front end builds it.

#include "rayomd/tiny_pdf.h"

#include <cstdio>
#include <string>

int main() {
    TinyPdf::PdfOptions options;
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf("# Engine consumer\n\nHello.\n", options, pdf);
    if (!result.Ok() || result.pages != 1 || pdf.compare(0, 5, "%PDF-") != 0) {
        std::printf("engine consumer: failed\n");
        return 1;
    }
    std::printf("engine consumer: ok, %zu bytes\n", pdf.size());
    return 0;
}
