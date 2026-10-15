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
    // 10^11 bytes for the 4 MB here (minutes); a paragraph of 256 KB took seconds to export.
    const std::string symbols = Repeat("\xE2\x9C\x85\xE2\x9D\x8C\xE2\x9A\xA0\xEF\xB8\x8F", 4 * 1024 * 1024 / 12);
    if (!Bounded("4 MB of status symbols", 5.0, 256, [&]() {
            const std::string normalized = TinyPdf::Internal::NormalizeSymbols(symbols);
            return normalized.size() == symbols.size() / 12 * 10 && normalized.compare(0, 10, "[OK][X][!]") == 0;
        })) {
        return false;
    }
    const std::string paragraph = Repeat("\xE2\x9C\x85", 256 * 1024 / 3) + "\n";
    return Bounded("a 256 KB paragraph of status symbols", 20.0, 512, [&]() {
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

} // namespace parse_limits_tests

bool CheckParseLimits() {
    return parse_limits_tests::CheckStatusSymbols() && parse_limits_tests::CheckReferenceLinks();
}
