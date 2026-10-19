// SPDX-License-Identifier: Apache-2.0

// Needs <atomic>, <chrono>, <string>, <thread>, <vector>, and Build's RAYOMD_TEST_SOURCE_DIR.
// BuildSession (tiny_pdf.h, src/core/build_session.cpp): documents edited at random places, each
// version built by one session and by BuildPdf, the bytes, results and source maps compared at every
// step: both renderers, the options whose layout a session takes from its last build and those it
// lays out whole (footnotes, PDF/UA), cancelled builds between, and a check that a session does
// reuse the blocks an edit leaves alone. Included inside the anonymous namespace of
// tests/core_tests.cpp.

namespace session_tests {

// The same edits on every platform: xorshift64 from fixed seeds.
struct Random {
    uint64_t state;
    uint64_t Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    size_t Below(size_t n) { return n == 0 ? 0 : static_cast<size_t>(Next() % n); }
};

// A document of `sections` sections of every kind of block; `text` is the script of its prose.
std::string Document(size_t sections, const std::string& text) {
    std::string out = "# A document of sections\n\nIntro " + text + " with a [link](https://example.com/a).\n\n";
    for (size_t index = 0; index < sections; index++) {
        const std::string n = std::to_string(index);
        out += "## Section " + n + "\n\n";
        out += "A paragraph of section " + n + ": " + text + " **bold**, *italic*, `code` and [a reference][ref]. "
            "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et "
            "dolore magna aliqua.\n\n";
        out += "- one " + n + "\n- two with *emphasis*\n  - nested " + text + "\n- [ ] a task\n\n";
        out += "1. first\n2. second\n\n";
        out += "> A quote of section " + n + " and " + text + ".\n\n";
        if (index % 3 == 0) out += "> [!NOTE]\n> An alert.\n\n";
        out += "```python\ndef f" + n + "(x):\n    return x * " + n + "  # comment\n```\n\n";
        out += "| Name | Value |\n|---|---:|\n| alpha " + n + " | 1 |\n| " + text + " | 22 |\n\n";
        if (index % 2 == 0) out += "Inline math $x_" + n + "^2 + y$ in a line.\n\n$$\n\\frac{a}{b} + \\sqrt{" + n + "}\n$$\n\n";
        if (index % 4 == 1) out += "| f | v |\n|---|---|\n| $\\alpha$ | " + n + " |\n\n";
        if (index % 4 == 2) out += "<div align=\"center\">\n\nCentred text " + n + ".\n\n</div>\n\n";
        if (index % 5 == 3) out += "<div style=\"page-break-after: always\"></div>\n\n";
        if (index % 3 == 1) out += "<a id=\"anchor-" + n + "\"></a>\n\n### A heading after an anchor\n\n";
        if (index % 6 == 5) out += "![mascot](docs/assets/branding/rayomd.png)\n\n![missing](does-not-exist.png)\n\n";
        out += "    indented code " + n + "\n\n---\n\n";
        out += "Closing words of section " + n + ", see [the start](#a-document-of-sections).\n\n";
    }
    out += "[ref]: https://example.com/ref\n";
    return out;
}

// What an editor types, Markdown syntax and structure included.
const char* const kSnippets[] = { "a", " ", "\n", "\n\n", "word ", "**bold**", "*", "`x`", "[l](https://e.com)",
    "# Heading\n", "\n## Sub\n\n", "- item\n", "> quote\n", "\n```\ncode\n```\n", "```", "\n---\n", "| a | b |\n|---|---|\n",
    "$x^2$", "$$\n\\frac{a}{b}\n$$\n", "[TOC]\n\n", "<!-- c -->", "\n\n<a id=\"here\"></a>\n\n", "  ", "\\", "<br>",
    "1. one\n", "\n<div style=\"page-break-after: always\"></div>\n", "![m](docs/assets/branding/rayomd.png)",
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor. " };

size_t LineStart(const std::string& text, size_t at) {
    while (at > 0 && text[at - 1] != '\n') at--;
    return at;
}

size_t LineEnd(const std::string& text, size_t at) {
    while (at < text.size() && text[at] != '\n') at++;
    return at;
}

// One edit at a random place: a snippet in, bytes out, a line out or twice, a range replaced, a letter.
void Edit(Random& random, std::string& text, const std::string& script) {
    size_t at = random.Below(text.size() + 1);
    // Not inside a character of more than one byte: the document stays UTF-8.
    while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) at++;
    const char* const snippet = kSnippets[random.Below(sizeof kSnippets / sizeof kSnippets[0])];
    switch (random.Below(7)) {
    case 0: text.insert(at, snippet); break;
    case 1: text.insert(at, script); break;
    case 2: {
        size_t end = std::min(text.size(), at + 1 + random.Below(8));
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) end++;
        text.erase(at, end - at);
        break;
    }
    case 3: {
        const size_t start = LineStart(text, at);
        text.erase(start, std::min(text.size(), LineEnd(text, at) + 1) - start);
        break;
    }
    case 4: {
        const size_t start = LineStart(text, at);
        std::string line = text.substr(start, std::min(text.size(), LineEnd(text, at) + 1) - start);
        if (line.empty() || line.back() != '\n') line += '\n';
        text.insert(start, line);
        break;
    }
    case 5: {
        size_t end = std::min(text.size(), at + random.Below(40));
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) end++;
        text.replace(at, end - at, snippet);
        break;
    }
    default: text.insert(at, 1, static_cast<char>('a' + random.Below(26))); break;
    }
}

bool SameSourceMaps(const std::vector<TinyPdf::SourceBlock>& a, const std::vector<TinyPdf::SourceBlock>& b) {
    if (a.size() != b.size()) return false;
    for (size_t index = 0; index < a.size(); index++) {
        if (a[index].firstLine != b[index].firstLine || a[index].endLine != b[index].endLine ||
            a[index].page != b[index].page || a[index].endPage != b[index].endPage || a[index].top != b[index].top ||
            a[index].bottom != b[index].bottom) {
            return false;
        }
    }
    return true;
}

// `steps` edits of `text`, every version built by a session and by BuildPdf with `options`; every
// fifth step a build of the session cancelled part way first. `replayed` adds up what the session
// took from its last builds.
bool Run(const char* name, std::string text, const std::string& script, TinyPdf::PdfOptions options, int steps,
    uint64_t seed, uint64_t& replayed) {
    Random random{ seed };
    TinyPdf::BuildSession session;
    std::vector<TinyPdf::SourceBlock> sessionMap, freshMap;
    const bool mapped = seed % 4 != 0;
    std::string sessionPdf, freshPdf;
    for (int step = 0; step <= steps; step++) {
        if (step > 0) Edit(random, text, script);
        if (step % 5 == 4) {
            std::atomic<bool> cancel{ false };
            TinyPdf::PdfOptions cancelled = options;
            cancelled.cancel = &cancel;
            const auto delay = std::chrono::microseconds(random.Below(400));
            std::thread stopper([&cancel, delay] {
                std::this_thread::sleep_for(delay);
                cancel.store(true);
            });
            std::string scrap;
            session.Build(text, cancelled, scrap);
            stopper.join();
        }
        TinyPdf::PdfOptions sessionOptions = options;
        TinyPdf::PdfOptions freshOptions = options;
        sessionOptions.sourceMap = mapped ? &sessionMap : nullptr;
        freshOptions.sourceMap = mapped ? &freshMap : nullptr;
        const TinyPdf::BuildResult a = session.Build(text, sessionOptions, sessionPdf);
        const TinyPdf::BuildResult b = TinyPdf::BuildPdf(text, freshOptions, freshPdf);
        if (a.error != b.error || a.pages != b.pages || a.missingCharacters != b.missingCharacters ||
            a.failedImages != b.failedImages || sessionPdf != freshPdf || (mapped && !SameSourceMaps(sessionMap, freshMap))) {
            std::cerr << "BuildSession differs from BuildPdf: " << name << ", seed " << seed << ", step " << step
                      << " (" << sessionPdf.size() << " and " << freshPdf.size() << " bytes)" << std::endl;
            return false;
        }
        replayed += session.LastReuse().replayed;
    }
    return true;
}

bool CheckBuildSession() {
    const std::string ascii = "plain words";
    const std::string latin = "caf\xC3\xA9 na\xC3\xAFve \xC3\x86r\xC3\xB8";
    const std::string unicode = "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 \xCE\x94\xCE\xBF\xCE\xBA\xCE\xB9\xCE\xBC\xCE\xAE "
        "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0";
    TinyPdf::PdfOptions plain;
    plain.style = TinyPdf::PdfStyle::Modern;
    plain.sourcePath = std::string(RAYOMD_TEST_SOURCE_DIR) + "/tester.md";
    uint64_t replayed = 0;
    // Both renderers, each with the options the session lays out by blocks.
    if (!Run("ASCII", Document(6, ascii), ascii, plain, 60, 1, replayed)) return false;
    if (!Run("Latin", Document(6, latin), latin, plain, 60, 2, replayed)) return false;
    if (!Run("Unicode", Document(6, unicode), unicode, plain, 60, 3, replayed)) return false;
    TinyPdf::PdfOptions contents = plain;
    contents.toc = true;
    contents.numberHeadings = 2;
    contents.pageNumbers = true;
    if (!Run("contents", Document(5, unicode), unicode, contents, 40, 5, replayed)) return false;
    TinyPdf::PdfOptions longPage = plain;
    longPage.pageSize = { 420.0, 594.0, true };
    longPage.blackText = true;
    longPage.compress = true;
    if (!Run("long page", Document(5, unicode), unicode, longPage, 40, 6, replayed)) return false;
    TinyPdf::PdfOptions themed = plain;
    themed.style = TinyPdf::PdfStyle::Tech;
    themed.margin = TinyPdf::PdfMargin::Wide();
    themed.pageSize = TinyPdf::PdfPageSize::A5();
    themed.theme.headingColor = 0x204080;
    themed.theme.headerLeft = "{title}";
    themed.theme.footerRight = "{page} / {pages}";
    themed.pageBreakBeforeLevel = 2;
    themed.toc = true;
    themed.highlightCode = false;
    if (!Run("themed", Document(5, ascii), ascii, themed, 40, 7, replayed)) return false;
    // What a session lays out whole: footnotes, PDF/UA, Obsidian's dialect with its callouts.
    if (!Run("footnotes", Document(4, ascii) + "\nA note[^1] and another[^b].\n\n[^1]: The note.\n[^b]: Another.\n",
            ascii, plain, 12, 8, replayed)) {
        return false;
    }
    TinyPdf::PdfOptions tagged = plain;
    tagged.pdfua = true;
    if (!Run("PDF/UA", Document(3, unicode), unicode, tagged, 8, 9, replayed)) return false;
    TinyPdf::PdfOptions obsidian = plain;
    obsidian.dialect = TinyPdf::MarkdownDialect::Obsidian;
    if (!Run("Obsidian", Document(4, ascii) + "\n> [!tip] A callout\n> with ==marks== and [[a link]].\n", ascii, obsidian,
            15, 10, replayed)) {
        return false;
    }
    if (replayed == 0) {
        std::cerr << "BuildSession replayed no block" << std::endl;
        return false;
    }
    // A letter typed in the middle of a long document: the session lays out a few of its blocks.
    for (const std::string* script : { &ascii, &unicode }) {
        std::string text = Document(40, *script);
        TinyPdf::BuildSession session;
        std::string pdf, fresh;
        if (!session.Build(text, plain, pdf).Ok()) return false;
        text.insert(text.find("A paragraph of section 20") + 2, "x");
        if (!session.Build(text, plain, pdf).Ok() || !TinyPdf::BuildPdf(text, plain, fresh).Ok() || pdf != fresh) {
            std::cerr << "BuildSession differs from BuildPdf after a typed letter" << std::endl;
            return false;
        }
        const TinyPdf::BuildSession::Reuse reuse = session.LastReuse();
        if (reuse.replayed + reuse.laidOut != reuse.blocks || reuse.laidOut * 10 > reuse.blocks) {
            std::cerr << "BuildSession laid out " << reuse.laidOut << " of " << reuse.blocks
                      << " blocks after a typed letter" << std::endl;
            return false;
        }
        // Other options: nothing taken from a layout made with the ones before.
        TinyPdf::PdfOptions other = plain;
        other.margin = TinyPdf::PdfMargin::Compact();
        if (!session.Build(text, other, pdf).Ok() || session.LastReuse().replayed != 0 ||
            !TinyPdf::BuildPdf(text, other, fresh).Ok() || pdf != fresh) {
            std::cerr << "BuildSession reused a layout of other options" << std::endl;
            return false;
        }
        session.Clear();
        if (!session.Build(text, other, pdf).Ok() || session.LastReuse().replayed != 0 || pdf != fresh) return false;
    }
    return true;
}

} // namespace session_tests
