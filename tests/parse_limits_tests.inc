// SPDX-License-Identifier: Apache-2.0

// Needs <chrono>, <iostream>, <string>, <string_view>, <vector>; uses math_markdown_tests::Repeat
// and ParseAllInline, so it is included after math_markdown_tests.inc, and PeakRssBytes of
// tests/core_tests.cpp.
// What the parser does with hostile input must stay linear in time and memory, and what it does
// with ordinary input must keep its bytes.
// Included inside the anonymous namespace of tests/core_tests.cpp.

namespace parse_limits_tests {

// The status symbols as they were rewritten before one pass did it: symbol by symbol, each
// replaced in place.
std::string NormalizeSymbolsReference(std::string value) {
    const std::pair<std::string_view, std::string_view> symbols[] = {
        { "\xE2\x9C\x85", "[OK]" }, { "\xE2\x9A\xA0\xEF\xB8\x8F", "[!]" }, { "\xE2\x9A\xA0", "[!]" },
        { "\xE2\x9D\x8C", "[X]" },
    };
    for (const auto& [from, to] : symbols) {
        for (size_t at = value.find(from); at != std::string::npos; at = value.find(from, at + to.size())) {
            value.replace(at, from.size(), to);
        }
    }
    return value;
}

// Runs `work` and fails when it takes longer than `seconds` or, where the peak resident size
// can be read, raises it by more than `megabytes`.
template <typename Work>
bool Bounded(const char* name, double seconds, size_t megabytes, Work work) {
    const size_t peakBefore = PeakRssBytes();
    const auto started = std::chrono::steady_clock::now();
    if (!work()) {
        std::cerr << name << ": wrong result" << std::endl;
        return false;
    }
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const size_t peakAfter = PeakRssBytes();
    const size_t grew = peakAfter > peakBefore ? peakAfter - peakBefore : 0;
    if (took > seconds || grew > megabytes * 1024 * 1024) {
        std::cerr << name << ": " << took << " s, peak +" << grew / (1024 * 1024) << " MB" << std::endl;
        return false;
    }
    return true;
}

bool CheckStatusSymbols() {
    using math_markdown_tests::Repeat;
    // The one pass gives what symbol-by-symbol replacing gave, also for broken and adjacent ones.
    const char* const pieces[] = {
        "\xE2\x9C\x85", "\xE2\x9A\xA0", "\xEF\xB8\x8F", "\xE2\x9D\x8C", "\xE2", "\x9C", "\xE2\x9A", "\x85", "a", " ",
    };
    uint64_t state = 0x2545F4914F6CDD1Dull;
    auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32_t>(state >> 16);
    };
    for (int round = 0; round < 3000; round++) {
        std::string text;
        const uint32_t count = next() % 24;
        for (uint32_t i = 0; i < count; i++) text += pieces[next() % 10];
        if (TinyPdf::Internal::NormalizeSymbols(text) != NormalizeSymbolsReference(text)) {
            std::cerr << "status symbols differ from symbol-by-symbol replacing" << std::endl;
            return false;
        }
    }
    // A paragraph of nothing but symbols: replaced in place, each moved the rest of the text,
    // 10^11 bytes for the 4 MB here (87 s before). A document the standard fonts can show is
    // transcoded to WinAnsi in one pass first, its symbols with it; one character beyond them
    // (U+4E2D) sends it to the Unicode renderer, which normalizes each paragraph: exporting
    // 1.2 MB of symbols took 6 s.
    const std::string symbols = Repeat("\xE2\x9C\x85\xE2\x9D\x8C\xE2\x9A\xA0\xEF\xB8\x8F", 4 * 1024 * 1024 / 12);
    if (!Bounded("4 MB of status symbols", 5.0, 256, [&]() {
            const std::string normalized = TinyPdf::Internal::NormalizeSymbols(symbols);
            return normalized.size() == symbols.size() / 12 * 10 && normalized.compare(0, 10, "[OK][X][!]") == 0;
        })) {
        return false;
    }
    const std::string paragraph = "\xE4\xB8\xAD *a* " + Repeat("\xE2\x9C\x85", 1024 * 1024 / 3) + "\n";
    return Bounded("a 1 MB paragraph of status symbols in the Unicode renderer", 20.0, 512, [&]() {
        std::string pdf;
        return Build(paragraph, pdf);
    });
}

// The PDF of `markdown` must have the size and SHA-256 it had before the limits: ordinary input
// keeps its bytes. Standard fonts only, so the digests hold on every platform.
bool SameBytes(const char* name, const std::string& markdown, size_t size, const char* sha256) {
    std::string pdf;
    if (!Build(markdown, pdf) || pdf.size() != size || RayoMd::PdfSource::Sha256Hex(pdf) != sha256) {
        std::cerr << name << " golden mismatch: " << pdf.size() << " " << RayoMd::PdfSource::Sha256Hex(pdf) << std::endl;
        return false;
    }
    return true;
}

bool CheckReferenceLinks() {
    using math_markdown_tests::Repeat;
    using TinyPdf::Internal::BlockType;
    using TinyPdf::Internal::ParseMarkdown;
    // Reference links of every form, at every level, keep their bytes.
    const std::string ordinary =
        "# References [home]\n\n"
        "Full [the docs][docs], collapsed [docs][], shortcut [docs], an image ![logo][img] and an\n"
        "unknown [label][nowhere], with \\[docs] escaped and `[docs]` as code.\n\n"
        "- In a list: [Docs][DOCS] and [home].\n"
        "  - nested [docs]\n\n"
        "> In a quote: [the home page][home].\n\n"
        "| Column | Link |\n|---|---|\n| one | [docs] |\n| two | [site][home] |\n\n"
        "A footnote[^1] and a title link [titled].\n\n"
        "[![logo][img]][home]\n\n"
        "[^1]: The note links to [docs].\n\n"
        "[docs]: https://docs.example.com/guide?a=1&b=2\n"
        "[home]: <https://example.com/a b> \"Home\"\n"
        "[img]: docs/assets/branding/rayomd.png\n"
        "[titled]: https://example.com/t\n"
        "    'A title'\n";
    if (!SameBytes("reference links", ordinary, 7632, "19bb62266e34877010e466e0904a5f11fbad861f03929ee04cf18a02d15d65d5")) {
        return false;
    }

    // A destination longer than the limit makes no definition: the line stays text.
    const std::string longest(8192, 'x');
    std::vector<TinyPdf::Internal::Block> blocks = ParseMarkdown("[a]: " + longest + "\n\n[a]\n");
    if (blocks.size() != 1 || blocks[0].text != "[a](<" + longest + ">)") {
        std::cerr << "a destination of 8,192 bytes makes no definition" << std::endl;
        return false;
    }
    blocks = ParseMarkdown("[a]: " + longest + "y\n\n[a]\n");
    if (blocks.size() != 2 || blocks[0].text != "[a]: " + longest + "y" || blocks[1].text != "[a]") {
        std::cerr << "a destination of 8,193 bytes makes a definition" << std::endl;
        return false;
    }

    // Each use copied its destination: 100,000 uses of 8,000 bytes (500 KB of text) made 800 MB
    // of paragraph text and as many bytes of link targets for the renderer. Past the document's
    // budget (twice its size and a mebibyte) a use stays text.
    const std::string many = "[a]: " + std::string(8000, 'y') + "\n\n" + Repeat("[a], ", 100000) + "\n";
    if (!Bounded("100,000 uses of an 8,000-byte destination", 20.0, 192, [&]() {
            const std::vector<TinyPdf::Internal::Block> parsed = ParseMarkdown(many);
            if (parsed.size() != 1) return false;
            const size_t links = CountOccurrences(parsed[0].text, "](<");
            const size_t budget = 2 * many.size() + (size_t(1) << 20);
            if (links == 0 || links > budget / 8000 || CountOccurrences(parsed[0].text, "[a], ") < 99000) return false;
            std::string pdf;
            return Build(many, pdf) && pdf.find("/Subtype /Link") != std::string::npos;
        })) {
        return false;
    }

    // Nesting must not multiply it: in the quote each of 1,000 uses of "a" becomes 2,000 uses
    // of "b", a definition of the quote's own, which the nested parse resolved into 16 GB.
    const std::string nested = "[a]: <" + Repeat("[b]", 2000) + ">\n\n> [b]: <" + std::string(8000, 'z') + ">\n>\n> " +
        Repeat("[a] ", 1000) + "\n";
    return Bounded("references resolving to references in a quote", 20.0, 192, [&]() {
        const std::vector<TinyPdf::Internal::Block> parsed = ParseMarkdown(nested);
        if (parsed.size() != 1 || parsed[0].type != BlockType::Quote) return false;
        size_t bytes = parsed[0].text.size();
        for (const TinyPdf::Internal::Block& child : parsed[0].children) bytes += child.text.size();
        if (bytes == 0 || bytes > 3 * nested.size() + (size_t(1) << 20)) return false;
        std::string pdf;
        return Build(nested, pdf);
    });
}

bool CheckTables() {
    using math_markdown_tests::Repeat;
    using TinyPdf::Internal::Block;
    using TinyPdf::Internal::BlockType;
    using TinyPdf::Internal::ParseMarkdown;
    // Rows shorter and longer than their header, a separator shorter than it: the same bytes.
    const std::string ragged =
        "# Ragged tables\n\n"
        "| a | b | c |\n|---|:-:|--:|\n| 1 |\n| 1 | 2 | 3 | 4 |\n| \\| x | `y|z` |\nno pipes here\n\n"
        "| h1 | h2 | h3 |\n|---|---|\n| short |\n|  |\n\n"
        "> | q | r |\n> |---|---|\n> | only |\n\n"
        "- item\n\n  | s | t | u | v |\n  |---|---|---|---|\n  | 1 |\n";
    if (!SameBytes("ragged tables", ragged, 5447, "2a42e434fe1acd7f088c00ad41c7d005f69c6becf9d1a16950582bc28843f7d7")) {
        return false;
    }

    // A header of 1,000 cells makes a table, one of 1,001 does not.
    const auto header = [](size_t cells) { return Repeat("| a ", cells) + "|\n|---|\nx\n"; };
    std::vector<Block> blocks = ParseMarkdown(header(1000));
    if (blocks.size() != 1 || blocks[0].type != BlockType::Table || blocks[0].rows.size() != 2 ||
        blocks[0].rows[1].size() != 1000) {
        std::cerr << "a table of 1,000 columns is no table" << std::endl;
        return false;
    }
    blocks = ParseMarkdown(header(1001));
    if (blocks.empty() || blocks[0].type != BlockType::Paragraph) {
        std::cerr << "a table of 1,001 columns is a table" << std::endl;
        return false;
    }

    // Every line after a header got the header's cells: 100,000 cells and 10,000 one-word lines
    // (120 KB) made 10^9 cells, 32 GB of empty strings.
    const std::string wide = std::string(100001, '|') + "\n|---|\n" + Repeat("x\n", 10000);
    if (!Bounded("a header of 100,000 cells over 10,000 lines", 20.0, 192, [&]() {
            const std::vector<Block> parsed = ParseMarkdown(wide);
            std::string pdf;
            return !parsed.empty() && parsed[0].type == BlockType::Paragraph && Build(wide, pdf);
        })) {
        return false;
    }

    // Within the column limit, the document's empty cells are limited to 2^19 (cmark-gfm's
    // limit): 1,000 columns over 20,000 one-word lines were 2 * 10^7 cells. The row that would
    // pass the limit ends the table; it and the lines after it are a paragraph.
    const std::string tall = Repeat("| a ", 1000) + "|\n|---|\n" + Repeat("x\n", 20000);
    if (!Bounded("1,000 columns over 20,000 one-word lines", 20.0, 512, [&]() {
            const std::vector<Block> parsed = ParseMarkdown(tall);
            const size_t rows = (size_t(1) << 19) / 999;
            std::string pdf;
            return parsed.size() == 2 && parsed[0].type == BlockType::Table && parsed[0].rows.size() == 1 + rows &&
                parsed[1].type == BlockType::Paragraph && parsed[1].text.size() == 2 * (20000 - rows) - 1 &&
                Build(tall, pdf);
        })) {
        return false;
    }
    // The limit is the document's, also for tables in quotes, each parsed on its own.
    const std::string quoted = Repeat("> " + Repeat("| a ", 1000) + "|\n> |---|\n> x\n\n", 600);
    return Bounded("600 quoted tables of 1,000 columns", 20.0, 512, [&]() {
        const std::vector<Block> parsed = ParseMarkdown(quoted);
        size_t full = 0;
        for (const Block& quote : parsed) {
            if (quote.type != BlockType::Quote || quote.children.empty() || quote.children[0].type != BlockType::Table) {
                return false;
            }
            full += quote.children[0].rows.size() == 2;
        }
        std::string pdf;
        return parsed.size() == 600 && full == (size_t(1) << 19) / 999 && Build(quoted, pdf);
    });
}

// Markdown links take the URL policy of HTML links (Html::SafeUrl): a target that runs script,
// carries data or opens a local file or network share keeps its text and gets no annotation.
bool CheckLinkSchemes() {
    using TinyPdf::Internal::InlineSpan;
    const char* const unsafe[] = {
        "[x](javascript:alert(1))", "[x](JaVaScRiPt:alert(1))", "[x](<java\tscript:a>)", "[x](&#106;avascript:a)",
        "[x](vbscript:msgbox)", "[x](data:text/html,a)", "[x](file:///etc/passwd)", "[x](\\\\\\\\host\\\\share)",
        "[x](<\\\\\\\\host\\\\share>)", "[x](C:\\\\Windows\\\\calc.exe)", "[x](<file://host/a b>)", "[![x](a.png)](javascript:a)",
        "[*x*](javascript:a)",
    };
    for (const char* text : unsafe) {
        std::string visible;
        for (const InlineSpan& span : TinyPdf::Internal::ParseInlineSpans(text)) {
            if (!span.url.empty()) {
                std::cerr << "unsafe Markdown link made: " << text << " -> " << span.url << std::endl;
                return false;
            }
            visible += span.text;
        }
        if (visible.find('x') == std::string::npos) {
            std::cerr << "unsafe Markdown link lost its text: " << text << std::endl;
            return false;
        }
    }
    const std::pair<const char*, const char*> safe[] = {
        { "[x](https://example.com/a)", "https://example.com/a" }, { "[x](mailto:a@b.c)", "mailto:a@b.c" },
        { "[x](docs/a.md#b)", "docs/a.md#b" }, { "[x](#top)", "#top" }, { "[x](//example.com)", "//example.com" },
        { "<https://example.com>", "https://example.com" }, { "see https://example.com/x", "https://example.com/x" },
        { "a@example.com", "mailto:a@example.com" },
    };
    for (const auto& [text, url] : safe) {
        bool found = false;
        for (const InlineSpan& span : TinyPdf::Internal::ParseInlineSpans(text)) found = found || span.url == url;
        if (!found) {
            std::cerr << "safe Markdown link not made: " << text << std::endl;
            return false;
        }
    }

    // In a document: references, a linked image block, a list, a table and HTML; the PDF has the
    // text and only the one safe link.
    const std::string document =
        "[a][js] [b][file] [c] and [safe](https://example.com/ok)\n\n"
        "[![logo](docs/assets/branding/rayomd.png)](javascript:alert(1))\n\n"
        "- [d](vbscript:x)\n\n| e | f |\n|---|---|\n| [g](\\\\\\\\host\\\\x) | <a href=\"file:///x\">h</a> |\n\n"
        "[js]: javascript:alert(1)\n[file]: <file:///etc/passwd>\n[c]: data:text/html,x\n";
    const std::vector<TinyPdf::Internal::Block> blocks = TinyPdf::Internal::ParseMarkdown(document);
    if (blocks.size() < 2 || blocks[1].type != TinyPdf::Internal::BlockType::Image || !blocks[1].image ||
        !blocks[1].image->link.empty()) {
        std::cerr << "a linked image keeps an unsafe link" << std::endl;
        return false;
    }
    std::string pdf;
    if (!Build(document, pdf) || CountOccurrences(pdf, "/Subtype /Link") != 1 ||
        pdf.find("/URI (https://example.com/ok)") == std::string::npos) {
        std::cerr << "unsafe links in the PDF: " << CountOccurrences(pdf, "/Subtype /Link") << std::endl;
        return false;
    }
    return true;
}

} // namespace parse_limits_tests

bool CheckParseLimits() {
    return parse_limits_tests::CheckStatusSymbols() && parse_limits_tests::CheckReferenceLinks() &&
        parse_limits_tests::CheckTables() && parse_limits_tests::CheckLinkSchemes();
}
