// SPDX-License-Identifier: Apache-2.0

// Needs <filesystem>, <fstream>, <string>, <vector>; uses ValidPdf and CountOccurrences of
// tests/core_tests.cpp. One long page (PdfPageSize::autoHeight, --page-size=WIDTHxauto): pages
// 14,400 points high, each box cut a margin below its layout's end, what fits a page fitted to the
// nominal height (width * sqrt(2)), page numbers and bands inside each box, and the source map as
// on a page of the nominal size. In both renderers. Included inside the anonymous namespace of
// tests/core_tests.cpp.

namespace long_page_tests {

constexpr double kTop = 14400.0;
constexpr double kMargin = 54.0;   // PdfMargin::Normal

struct Box {
    double left = 0.0;
    double bottom = 0.0;
    double right = 0.0;
    double top = 0.0;
};

// The MediaBox of every page object, in object order (a theme's cover comes last).
std::vector<Box> PageBoxes(const std::string& pdf) {
    std::vector<Box> boxes;
    constexpr std::string_view kKey = "/MediaBox [";
    for (size_t at = pdf.find(kKey); at != std::string::npos; at = pdf.find(kKey, at)) {
        at += kKey.size();
        char* end = nullptr;
        Box box;
        box.left = std::strtod(pdf.c_str() + at, &end);
        box.bottom = std::strtod(end, &end);
        box.right = std::strtod(end, &end);
        box.top = std::strtod(end, &end);
        boxes.push_back(box);
    }
    return boxes;
}

// The baselines ("1 0 0 1 x y Tm") of the overlay streams in grey that page numbers and a theme's
// header and footer are drawn in.
std::vector<double> OverlayBaselines(const std::string& pdf) {
    std::vector<double> baselines;
    constexpr std::string_view kOverlay = "stream\nq 0.45 0.45 0.45 rg";
    for (size_t at = pdf.find(kOverlay); at != std::string::npos; at = pdf.find(kOverlay, at)) {
        const size_t end = pdf.find("endstream", at);
        for (size_t matrix = pdf.find("1 0 0 1 ", at); matrix < end; matrix = pdf.find("1 0 0 1 ", matrix + 1)) {
            char* after = nullptr;
            std::strtod(pdf.c_str() + matrix + 8, &after);
            baselines.push_back(std::strtod(after, nullptr));
        }
        at = end;
    }
    return baselines;
}

bool Near(double a, double b) {
    return std::abs(a - b) < 0.02;
}

bool CheckParse() {
    using TinyPdf::Internal::ParsePageSize;
    constexpr double kRoot2 = 1.4142135623730951;
    TinyPdf::PdfPageSize size;
    const auto autoOf = [&](const char* text, double width) {
        size = {};
        return ParsePageSize(text, size) && size.autoHeight && std::abs(size.width - width) < 0.001 &&
            std::abs(size.height - width * kRoot2) < 0.001;
    };
    const bool parsed = autoOf("210mmxauto", 595.2756) && autoOf("21cmxauto", 595.2756) && autoOf("8.5inxauto", 612.0) &&
        autoOf("600ptxauto", 600.0) && autoOf("210MMxAuto", 595.2756) && autoOf("144ptxauto", 144.0) &&
        ParsePageSize("14400ptxauto", size) && size.autoHeight && size.width == 14400.0 && size.height == 14400.0 &&
        ParsePageSize("a4", size) && !size.autoHeight;
    TinyPdf::PdfPageSize kept = TinyPdf::PdfPageSize::Letter();
    const bool refused = !ParsePageSize("a4xauto", kept) && !ParsePageSize("letterxauto", kept) &&
        !ParsePageSize("210mmxauto-landscape", kept) && !ParsePageSize("a4-landscapexauto", kept) &&
        !ParsePageSize("1mmxauto", kept) && !ParsePageSize("201inxauto", kept) && !ParsePageSize("210xauto", kept) &&
        !ParsePageSize("xauto", kept) && !ParsePageSize("mmxauto", kept) && !ParsePageSize("210mmxautox", kept) &&
        !ParsePageSize("210kmxauto", kept) && !ParsePageSize("210mmxauto297mm", kept) && !ParsePageSize("nanmmxauto", kept) &&
        !kept.autoHeight && kept.width == 612.0 && kept.height == 792.0;
    // Through the API the width keeps its bounds, the height given is ignored, and the nominal
    // height (the cover's) follows the width the page got.
    using TinyPdf::Internal::NominalPageHeight;
    bool resolved = NominalPageHeight(144.0) == 144.0 * kRoot2 && NominalPageHeight(14400.0) == 14400.0;
    for (const TinyPdf::PdfPageSize& given : { TinyPdf::PdfPageSize{ 10.0, 1.0, true }, TinyPdf::PdfPageSize{ 1.0e9, 5.0, true } }) {
        const double width = given.width < 144.0 ? 144.0 : 14400.0;
        TinyPdf::PdfOptions options;
        options.pageSize = given;
        options.theme.cover = true;
        std::string pdf;
        const std::vector<Box> boxes = TinyPdf::BuildPdf("# x\n", options, pdf).Ok() ? PageBoxes(pdf) : std::vector<Box>();
        resolved = resolved && boxes.size() == 2 && boxes[0].right == width && boxes[0].top == kTop &&
            boxes[0].bottom <= kTop - 144.0 && boxes[0].bottom > 14100.0 && boxes[1].bottom == 0.0 &&
            Near(boxes[1].top, std::min(width * kRoot2, kTop));
    }
    if (!parsed || !refused || !resolved) {
        std::cerr << "long page: size parsing mismatch (" << parsed << refused << resolved << ")" << std::endl;
        return false;
    }
    return true;
}

// Pages, boxes, page numbers, theme bands, images, a cover and PDF/A in the renderer of `prefix`'s
// text: "" the standard one, a Polish word the Unicode one.
bool CheckRenderer(const std::string& prefix, const std::filesystem::path& directory) {
    const std::string renderer = prefix.empty() ? "standard" : "Unicode";
    const auto fail = [&renderer](const char* what) {
        std::cerr << "long page (" << renderer << " renderer): " << what << std::endl;
        return false;
    };
    TinyPdf::PdfOptions options;
    options.pageSize = { 595.0, 0.0, true };
    options.pageNumbers = true;
    options.sourcePath = (directory / "doc.md").u8string();
    const double nominal = 595.0 * 1.4142135623730951;

    // 1,000 lines: a long page holds about 920 of them, so a second page takes the rest.
    std::string lines = prefix;
    for (int line = 0; line < 1000; line++) lines += "Line " + std::to_string(line) + "  \n";
    lines += "End.\n";
    std::string pdf;
    TinyPdf::BuildResult result = TinyPdf::BuildPdf(lines, options, pdf);
    std::vector<Box> boxes = PageBoxes(pdf);
    if (!result.Ok() || !ValidPdf(pdf) || result.pages != 2 || boxes.size() != 2) return fail("1,000 lines are not two pages");
    for (const Box& box : boxes) {
        if (box.left != 0.0 || box.right != 595.0 || box.top != kTop || box.bottom < 0.0 || kTop - box.bottom < 144.0) {
            return fail("a box is not [0 y0 595 14400]");
        }
    }
    // The first page is full: its box reaches down to the bottom margin's line, about.
    if (boxes[0].bottom > 20.0 || boxes[1].bottom < 12000.0) return fail("the boxes do not end below the text");
    // The page numbers sit in the middle of each box's bottom margin.
    std::vector<double> baselines = OverlayBaselines(pdf);
    if (baselines.size() != 2 || !Near(baselines[0], boxes[0].bottom + kMargin * 0.5 - 3.0) ||
        !Near(baselines[1], boxes[1].bottom + kMargin * 0.5 - 3.0)) {
        return fail("the page numbers are not in the boxes' bottom margins");
    }

    // An explicit break: two pages, each cut below its line, at least 144 points high.
    result = TinyPdf::BuildPdf(prefix + "First\n\n\\pagebreak\n\nSecond\n", options, pdf);
    boxes = PageBoxes(pdf);
    if (!result.Ok() || result.pages != 2 || boxes.size() != 2 || boxes[0].bottom < 14100.0 ||
        boxes[0].bottom > kTop - 144.0 || !Near(boxes[1].bottom, kTop - 144.0)) {
        return fail("a page break does not give two short pages");
    }
    std::string many = prefix + "# Title\n\n";
    for (int line = 0; line < 30; line++) many += "Paragraph " + std::to_string(line) + ".\n\n";
    many += "\\pagebreak\n\nSecond page.\n";

    // A theme's header at the top of each page, its footer in the bottom margin of each box.
    options.theme.headerLeft = "Head";
    options.theme.footerRight = "Foot";
    options.theme.cover = true;
    result = TinyPdf::BuildPdf(many, options, pdf);
    boxes = PageBoxes(pdf);
    baselines = OverlayBaselines(pdf);
    std::vector<double> expected;
    for (size_t page = 0; page + 1 < boxes.size(); page++) {
        expected.push_back(kTop - kMargin * 0.5 - 3.0);           // header
        expected.push_back(boxes[page].bottom + kMargin * 0.5 - 3.0);   // footer centre, "{page} / {pages}"
        expected.push_back(boxes[page].bottom + kMargin * 0.5 - 3.0);   // footer right
    }
    bool bands = result.Ok() && boxes.size() == 3 && baselines.size() == expected.size() && boxes[0].bottom < boxes[1].bottom &&
        boxes[0].bottom > 13000.0;
    for (size_t k = 0; bands && k < expected.size(); k++) bands = Near(baselines[k], expected[k]);
    if (!bands) return fail("the theme's bands are not inside the boxes");
    // The cover keeps the nominal size.
    if (boxes[2].bottom != 0.0 || !Near(boxes[2].top, nominal)) return fail("the cover is not of the nominal size");
    options.theme = {};

    // An image taller than the nominal page is fitted to it, as on a page of that size.
    std::ofstream(directory / "tall.svg", std::ios::binary) <<
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"4000\"><rect width=\"400\" height=\"4000\" fill=\"red\"/></svg>";
    result = TinyPdf::BuildPdf(prefix + "![tall](tall.svg)\n", options, pdf);
    const size_t draw = pdf.find(" cm /Im1 Do");
    const size_t matrix = draw == std::string::npos ? draw : pdf.rfind("q ", draw);
    double height = -1.0;
    if (matrix != std::string::npos) {
        char* end = nullptr;
        std::strtod(pdf.c_str() + matrix + 2, &end);
        std::strtod(end, &end);
        std::strtod(end, &end);
        height = std::strtod(end, nullptr);
    }
    if (!result.Ok() || result.failedImages != 0 || !(height > nominal - kMargin * 2.0 - 0.02) ||
        !(height < nominal - kMargin * 2.0 + 0.02)) {
        return fail("an image is not fitted to the nominal height");
    }

    // PDF/A: page numbers as a footer band in the document's font, inside each box.
    options.pdfa = true;
    result = TinyPdf::BuildPdf(lines, options, pdf);
    boxes = PageBoxes(pdf);
    baselines = OverlayBaselines(pdf);
    if (!result.Ok() || !ValidPdf(pdf) || boxes.size() != 2 || pdf.find("pdfaid:part") == std::string::npos ||
        baselines.size() != 2 || !Near(baselines[0], boxes[0].bottom + kMargin * 0.5 - 3.0) ||
        !Near(baselines[1], boxes[1].bottom + kMargin * 0.5 - 3.0)) {
        return fail("the PDF/A export's page numbers are not in its boxes");
    }
    return true;
}

// A book: its title page keeps the nominal page at the top of its long page, and each file starts
// a long page of its own, cut below its text.
bool CheckBook() {
    TinyPdf::Book book;
    book.title = "Guide";
    book.chapters.push_back({ "# One\n\nText.\n", "one.md", "", 0, false });
    book.chapters.push_back({ "# Two\n\nMore text.\n\nAnd more.\n", "two.md", "", 0, false });
    for (const std::string& prefix : { std::string(), std::string("Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n") }) {
        book.chapters[0].markdown = prefix + "# One\n\nText.\n";
        TinyPdf::PdfOptions options;
        options.pageSize = { 595.0, 0.0, true };
        std::string pdf;
        const TinyPdf::BuildResult result = TinyPdf::BuildBookPdf(book, options, pdf);
        const std::vector<Box> boxes = PageBoxes(pdf);
        if (!result.Ok() || !ValidPdf(pdf) || result.pages != 3 || boxes.size() != 3 ||
            !Near(boxes[0].bottom, kTop - 595.0 * 1.4142135623730951) || boxes[1].bottom < 13900.0 || boxes[2].bottom < 13900.0 ||
            boxes[1].bottom > kTop - 144.0 || boxes[2].bottom > kTop - 144.0) {
            std::cerr << "long page: book mismatch (" << (prefix.empty() ? "standard" : "Unicode") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

// The source map measures from each page's top edge, which a long page keeps: its blocks are where
// they are on a page of the nominal size, and a page break starts the next page at its margin.
bool CheckSourceMapTops() {
    const std::string document = "# Title\n\nFirst paragraph\ncontinues here.\n\n- one\n- two\n\n```text\ncode\n```\n\n"
        "| a | b |\n|---|---|\n| 1 | 2 |\n\nLast.\n";
    for (const std::string& prefix : { std::string(), std::string("Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n") }) {
        TinyPdf::PdfOptions options;
        std::vector<TinyPdf::SourceBlock> a4;
        std::vector<TinyPdf::SourceBlock> longMap;
        std::string pdf;
        options.sourceMap = &a4;
        bool ok = TinyPdf::BuildPdf(prefix + document, options, pdf).Ok();
        options.pageSize = { 595.0, 0.0, true };
        options.sourceMap = &longMap;
        ok = ok && TinyPdf::BuildPdf(prefix + document, options, pdf).Ok() && a4.size() == longMap.size() && !a4.empty();
        for (size_t k = 0; ok && k < a4.size(); k++) {
            ok = longMap[k].firstLine == a4[k].firstLine && longMap[k].endLine == a4[k].endLine && longMap[k].page == 0 &&
                longMap[k].endPage == 0 && std::abs(longMap[k].top - a4[k].top) < 0.01f &&
                std::abs(longMap[k].bottom - a4[k].bottom) < 0.01f;
        }
        ok = ok && TinyPdf::BuildPdf(prefix + "First\n\n\\pagebreak\n\nSecond\n", options, pdf).Ok() && longMap.size() >= 2 &&
            longMap.back().page == 1 && longMap.back().top == 54.0f && longMap[longMap.size() - 2].page == 0;
        if (!ok) {
            std::cerr << "long page: source map mismatch (" << (prefix.empty() ? "standard" : "Unicode") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckLongPage() {
    if (!CheckParse()) return false;
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "rayomd-test-long-page";
    std::filesystem::create_directories(directory);
    bool ok = true;
    for (const std::string& prefix : { std::string(), std::string("Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n") }) {
        ok = ok && CheckRenderer(prefix, directory);
    }
    std::filesystem::remove_all(directory);
    return ok && CheckBook() && CheckSourceMapTops();
}

} // namespace long_page_tests
