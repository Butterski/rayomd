// SPDX-License-Identifier: Apache-2.0

// Footnotes at the foot of the page (PdfOptions::footnotes, FootnotePlacement::Page), in both
// renderers: each note on the page of its first reference, below it, also when the reference is
// on a page's last line, two on one line, a cluster of notes, table rows with a repeated header,
// headings and quotes, notes with code and lists; a note too tall for a page after the text;
// books, PDF/UA and the source map. Uses References, CountOccurrences and RAYOMD_TEST_SOURCE_DIR
// of tests/core_tests.cpp, so it is included after LinkTargetsByPage, inside its anonymous
// namespace.

namespace page_notes_tests {

const std::string kUnicode = "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n";

// An internal link annotation: its page and rectangle, and the page and top it goes to.
struct NoteLink {
    int page = -1;
    double bottom = 0.0;
    double top = 0.0;
    int destPage = -1;
    double destTop = 0.0;
};

std::vector<NoteLink> InternalLinks(const std::string& pdf) {
    const auto object = [&pdf](int number) {
        const std::string head = "\n" + std::to_string(number) + " 0 obj\n";
        const size_t start = pdf.find(head);
        return start == std::string::npos ? std::string() : pdf.substr(start, pdf.find("\nendobj", start) - start);
    };
    const auto between = [](const std::string& text, std::string_view open) {
        const size_t start = text.find(open);
        return start == std::string::npos ? std::string() : text.substr(start + open.size(), text.find(']', start) - start - open.size());
    };
    const std::vector<int> pages = References(between(pdf, "/Type /Pages /Kids ["));
    std::vector<NoteLink> links;
    for (size_t page = 0; page < pages.size(); page++) {
        for (const int annotation : References(between(object(pages[page]), "/Annots ["))) {
            const std::string body = object(annotation);
            const std::string dest = between(body, "/Dest [");
            if (dest.empty()) continue;
            NoteLink link;
            link.page = static_cast<int>(page);
            double x0 = 0.0, x1 = 0.0;
            if (std::sscanf(between(body, "/Rect [").c_str(), "%lf %lf %lf %lf", &x0, &link.bottom, &x1, &link.top) != 4) return {};
            const int target = std::atoi(dest.c_str());
            const auto found = std::find(pages.begin(), pages.end(), target);
            link.destPage = found == pages.end() ? -1 : static_cast<int>(found - pages.begin());
            const size_t xyz = dest.find("/XYZ null ");
            link.destTop = xyz == std::string::npos ? 0.0 : std::atof(dest.c_str() + xyz + 10);
            links.push_back(link);
        }
    }
    return links;
}

TinyPdf::PdfOptions PageOptions() {
    TinyPdf::PdfOptions options;
    options.style = TinyPdf::PdfStyle::Modern;
    options.sourcePath = std::string(RAYOMD_TEST_SOURCE_DIR) + "/tester.md";
    options.footnotes = TinyPdf::FootnotePlacement::Page;
    return options;
}

bool BuildPage(const std::string& markdown, std::string& pdf, TinyPdf::PdfOptions options = PageOptions()) {
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf(markdown, options, pdf);
    return result.Ok() && pdf.rfind("%PDF-", 0) == 0 && pdf.find("%%EOF") != std::string::npos;
}

// Every note at the foot of the page of its reference: each reference links down its own page
// (`notes` of them, `references` links in all when a note is referred to more than once), each
// note's number back up it.
bool AtFoot(const std::string& pdf, size_t notes, size_t references = 0) {
    const std::vector<NoteLink> links = InternalLinks(pdf);
    size_t down = 0;
    size_t up = 0;
    for (const NoteLink& link : links) {
        if (link.destPage != link.page) return false;
        if (link.destTop < link.bottom) down++;
        else if (link.destTop > link.top) up++;
    }
    return down == (references != 0 ? references : notes) && up == notes;
}

std::string Filler(int lines, const char* word = "Filler") {
    std::string text;
    for (int line = 0; line < lines; line++) text += std::string(word) + " line " + std::to_string(line) + " of the text.\n\n";
    return text;
}

bool CheckBasics() {
    std::string document = "# Notes\n\n";
    std::string definitions;
    for (int note = 1; note <= 12; note++) {
        document += Filler(6) + "A reference to note " + std::to_string(note) + "[^n" + std::to_string(note) + "] in its line.\n\n";
        definitions += "[^n" + std::to_string(note) + "]: Note " + std::to_string(note) + " says a few words.\n";
    }
    document += definitions;
    for (const std::string& text : { document, kUnicode + document }) {
        std::string page, end;
        TinyPdf::PdfOptions endOptions = PageOptions();
        endOptions.footnotes = TinyPdf::FootnotePlacement::End;
        if (!BuildPage(text, page) || !BuildPage(text, end, endOptions) || page == end ||
            CountOccurrences(page, "/Type /Page ") < 2 || !AtFoot(page, 12) || AtFoot(end, 12)) {
            std::cerr << "page notes: basics mismatch (" << (text == document ? "standard" : "Unicode") << ")" << std::endl;
            return false;
        }
        // A document without footnotes is the same either way.
        if (!BuildPage(Filler(80), page) || !BuildPage(Filler(80), end, endOptions) || page != end) {
            std::cerr << "page notes: a document without notes changed" << std::endl;
            return false;
        }
    }
    return true;
}

// A reference on each line position near a page's end: its note goes with it to the next page
// when the page cannot hold both.
bool CheckLastLine() {
    for (const bool unicode : { false, true }) {
        bool moved = false;
        for (int lines = 40; lines < 56; lines++) {
            const std::string text = (unicode ? kUnicode : std::string()) + Filler(lines) +
                "The reference[^a] near the end of a page.\n\n" + Filler(4) +
                "[^a]: A note of three lines, long enough to need its room at the foot of the page, which it "
                "takes below the line that refers to it and nowhere else, or the note goes with that line.\n";
            std::string pdf;
            if (!BuildPage(text, pdf) || !AtFoot(pdf, 1)) {
                std::cerr << "page notes: a reference after " << lines << " lines is not with its note" << std::endl;
                return false;
            }
            moved = moved || InternalLinks(pdf)[0].page == 1;
        }
        if (!moved) {
            std::cerr << "page notes: no reference moved to the next page" << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckLinesAndClusters() {
    // Two references on one line, one used twice; a note only another note refers to goes with it.
    const std::string document = Filler(30) + "Two[^a] on one line[^b], again[^a].\n\n" + Filler(10) + "Later[^c].\n\n"
        "[^a]: First, which refers to [^x].\n[^b]: Second.\n[^c]: Third.\n[^x]: Only a note refers to this one.\n";
    for (const std::string& text : { document, kUnicode + document }) {
        std::string pdf;
        if (!BuildPage(text, pdf) || !AtFoot(pdf, 4, 5)) {
            std::cerr << "page notes: references on one line or a cluster mismatch" << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckBlocks() {
    // A table with references in its rows over several pages, its header repeated; a heading and a
    // quote with references; a note with a code block and a list.
    std::string table = "| Key | Value |\n|---|---|\n";
    for (int row = 0; row < 90; row++) {
        table += "| r" + std::to_string(row) + " | v" + (row % 15 == 0 ? "[^t" + std::to_string(row) + "]" : std::string()) + " |\n";
    }
    std::string document = "## Heading[^h]\n\n> A quote[^q] of a line.\n\n" + table + "\n";
    for (int row = 0; row < 90; row += 15) document += "[^t" + std::to_string(row) + "]: Row " + std::to_string(row) + ".\n";
    document += "[^h]: In a heading.\n[^q]: In a quote.\n";
    document += "\nCode note[^code].\n\n[^code]: A note with code\n\n    ```\n    int x = 1;\n    ```\n\n    - one\n    - two\n";
    for (const std::string& text : { document, kUnicode + document }) {
        std::string pdf;
        const bool unicode = text != document;
        if (!BuildPage(text, pdf) || !AtFoot(pdf, 9) || CountOccurrences(pdf, "/Type /Page ") < 3 ||
            (!unicode && CountOccurrences(pdf, "(Key) Tj") < 3) || pdf.find("0.95 0.95 0.93 rg") == std::string::npos) {
            std::cerr << "page notes: table, heading, quote or code note mismatch (" << (unicode ? "Unicode" : "standard") << ")"
                      << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckTallNote() {
    // A note taller than a page is set after the text, where the end section puts it without page
    // footnotes, and only it; the one beside it at the foot of the page.
    std::string tall = "[^big]: Tall note.";
    for (int line = 0; line < 120; line++) tall += "\n\n    Paragraph " + std::to_string(line) + " of the tall note.";
    const std::string document = "Short[^big] and small[^small].\n\n" + Filler(5) + "[^small]: Small.\n" + tall + "\n";
    for (const std::string& text : { document, kUnicode + document }) {
        std::string pdf, end;
        TinyPdf::PdfOptions endOptions = PageOptions();
        endOptions.footnotes = TinyPdf::FootnotePlacement::End;
        if (!BuildPage(text, pdf) || !BuildPage(text, end, endOptions)) return false;
        const std::vector<NoteLink> links = InternalLinks(pdf);
        const std::vector<NoteLink> endLinks = InternalLinks(end);
        // References to the tall note and the small one, then their numbers linking back.
        if (links.size() != 4 || endLinks.size() != 4 || links[0].destPage != endLinks[0].destPage ||
            links[0].destTop != endLinks[0].destTop || links[1].destPage != 0 || !(links[1].destTop < 100.0) ||
            !(endLinks[1].destTop > links[1].destTop) || CountOccurrences(pdf, "/Type /Page ") < 3) {
            std::cerr << "page notes: a note taller than a page mismatch" << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckBook() {
    TinyPdf::Book book;
    book.chapters.push_back({ "# One\n\n" + Filler(20) + "First[^1].\n\n[^1]: One's note.\n", "one.md", "", 0, false });
    book.chapters.push_back({ "# Two\n\nSecond[^1] and[^2].\n\n[^1]: Two's note.\n[^2]: Another.\n", "two.md", "", 0, false });
    for (const bool unicode : { false, true }) {
        if (unicode) book.chapters[0].markdown = kUnicode + book.chapters[0].markdown;
        std::string pdf;
        if (!TinyPdf::BuildBookPdf(book, PageOptions(), pdf).Ok() || !AtFoot(pdf, 3) ||
            CountOccurrences(pdf, "/Type /Page ") != 2) {
            std::cerr << "page notes: book mismatch" << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckTagged() {
    std::string document = "## First\n\nSee **bold**.[^n]\n\n- item with a note[^l]\n- more\n\n| A | B |\n|---|---|\n";
    for (int row = 0; row < 60; row++) document += "| r" + std::to_string(row) + " | v" + (row == 40 ? "[^t]" : "") + " |\n";
    document += "\n[^n]: The note.\n[^l]: In a list.\n[^t]: In a table.\n";
    TinyPdf::PdfOptions options = PageOptions();
    options.pdfua = true;
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf(document, options, pdf);
    bool numbered = true;
    int expected = 0;
    for (size_t at = pdf.find("<</MCID "); at != std::string::npos; at = pdf.find("<</MCID ", at + 1)) {
        const int mcid = std::atoi(pdf.c_str() + at + 8);
        numbered = numbered && (mcid == 0 || mcid == expected);
        expected = mcid + 1;
    }
    const size_t links = CountOccurrences(pdf, "/Subtype /Link");
    if (!result.Ok() || result.pages < 2 || !numbered || !AtFoot(pdf, 3) ||
        CountOccurrences(pdf, "BDC\n") + CountOccurrences(pdf, "BMC\n") != CountOccurrences(pdf, "EMC\n") ||
        CountOccurrences(pdf, "/S /Note ") != 3 || CountOccurrences(pdf, "/S /Reference ") != 3 ||
        CountOccurrences(pdf, "/Type /OBJR") != links || CountOccurrences(pdf, "/StructParent ") != links ||
        CountOccurrences(pdf, "/S /TD ") != 120 || CountOccurrences(pdf, "/S /LI ") != 2 || pdf.find("/IDTree ") == std::string::npos) {
        std::cerr << "page notes: tagged PDF mismatch" << std::endl;
        return false;
    }
    return true;
}

bool CheckSourceMap() {
    const std::string document = "# Title\n\n" + Filler(45) + "Note[^n] here.\n\nAfter.\n\n[^n]: The note.\n";
    std::vector<TinyPdf::SourceBlock> pageMap, endMap;
    TinyPdf::PdfOptions options = PageOptions();
    std::string mapped, plain;
    options.sourceMap = &pageMap;
    if (!BuildPage(document, mapped, options)) return false;
    options.sourceMap = nullptr;
    if (!BuildPage(document, plain, options) || mapped != plain) {
        std::cerr << "page notes: the source map changed the bytes" << std::endl;
        return false;
    }
    options.footnotes = TinyPdf::FootnotePlacement::End;
    options.sourceMap = &endMap;
    if (!BuildPage(document, plain, options) || pageMap.size() != endMap.size()) {
        std::cerr << "page notes: the source map's entries differ" << std::endl;
        return false;
    }
    for (size_t k = 0; k < pageMap.size(); k++) {
        if (pageMap[k].firstLine != endMap[k].firstLine || pageMap[k].endLine != endMap[k].endLine ||
            (pageMap[k].top >= pageMap[k].bottom && pageMap[k].page == pageMap[k].endPage)) {
            std::cerr << "page notes: source map entry " << k << " mismatch" << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckPageNotes() {
    return CheckBasics() && CheckLastLine() && CheckLinesAndClusters() && CheckBlocks() && CheckTallNote() && CheckBook() &&
        CheckTagged() && CheckSourceMap();
}

} // namespace page_notes_tests
