// SPDX-License-Identifier: Apache-2.0

// Needs <string>, <utility>, <vector>; uses math_markdown_tests::Repeat, parse_limits_tests::Bounded
// and CountOccurrences of tests/core_tests.cpp, so it is included after parse_limits_tests.inc.
// Obsidian's dialect (PdfOptions::dialect): its syntax turned into RayoMD's line for line, its
// callouts in both renderers, a book's wiki links by a note's name, and time linear in its openers.
// Included inside the anonymous namespace of tests/core_tests.cpp.

namespace dialect_tests {

bool CheckConversion() {
    const std::pair<std::string, std::string> cases[] = {
        { "a ==b c== d", "a <mark>b c</mark> d" },
        { "x %%y%% z", "x  z" },
        { "a %%\nb [[c]]\nd%% e", "a \n\n e" },
        { "[[Note#Part two|the part]]", "[the part](<Note.md#Part two>)" },
        { "[[Note#Part]] [[#Top]] [[#^id]]", "[Note > Part](<Note.md#Part>) [Top](<#Top>) ^id" },
        { "[[sub/a.pdf]]", "[sub/a.pdf](<sub/a.pdf>)" },
        { "| [[x\\|y]] |", "| [y](<x.md>) |" },
        { "[[a_b]]", "[a\\_b](<a_b.md>)" },
        { "![[i.png|30]]", "<img src=\"i.png\" alt=\"i.png\" width=\"30\">" },
        { "> ![[d/i.png|30x20]]", "> <img src=\"d/i.png\" alt=\"i.png\" width=\"30\" height=\"20\">" },
        { "t ![[i.png|30]] ![[i.png|a cat]]", "t ![i.png](<i.png>) ![a cat](<i.png>)" },
        { "![[Note]]", "[Note](<Note.md>)" },
        { "`[[a]] ==b==` $x==y$ \\==c== [[_TOC_]]", "`[[a]] ==b==` $x==y$ \\==c== [[_TOC_]]" },
        { "```\n[[a]] ==b==\n```\n==c==", "```\n[[a]] ==b==\n```\n<mark>c</mark>" },
        { "$$\na==b==c\n$$", "$$\na==b==c\n$$" },
        { "Title\n===\n==a== ==", "Title\n===\n<mark>a</mark> ==" },
        { "---\nt: [[x]]\n---\n[[y]]", "---\nt: [[x]]\n---\n[y](<y.md>)" },
        { "a == b ==c ==", "a == b ==c ==" },
    };
    for (const auto& [obsidian, expected] : cases) {
        const std::string converted = TinyPdf::Internal::ObsidianToCommonMark(obsidian);
        if (converted != expected) {
            std::cerr << "Obsidian conversion mismatch: " << obsidian << " -> " << converted << std::endl;
            return false;
        }
    }
    return true;
}

// Callouts: their own titles (or their types'), Obsidian's colours, nested, folded and alone, in
// both renderers; GitHub's dialect keeps such a quote a plain one.
bool CheckCallouts() {
    const std::string callouts = "> [!tip]- Read **this**\n> Body.\n> > [!bug]\n> > Inner.\n\n> [!quote] Alone\n\nText\n";
    TinyPdf::PdfOptions options;
    options.dialect = TinyPdf::MarkdownDialect::Obsidian;
    for (const char* suffix : { "", "\xC5\xBC\n" }) {   // the standard and the Unicode renderer
        std::string pdf;
        const bool unicode = *suffix != '\0';
        const auto has = [&pdf, unicode](const char* text) {
            return unicode || pdf.find(std::string("(") + text + ") Tj") != std::string::npos;
        };
        if (!TinyPdf::BuildPdf(callouts + suffix, options, pdf).Ok() || !has("Read this") || !has("Body.") || !has("Bug") ||
            !has("Inner.") || !has("Alone") || pdf.find("0.00 0.58 0.62 rg") == std::string::npos ||
            pdf.find("0.82 0.14 0.18 rg") == std::string::npos || pdf.find("0.50 0.50 0.50 rg") == std::string::npos ||
            (!unicode && pdf.find("([!") != std::string::npos)) {
            std::cerr << "Obsidian callouts mismatch" << (unicode ? " (Unicode)" : "") << std::endl;
            return false;
        }
    }
    std::string github;
    if (!TinyPdf::BuildPdf(callouts, TinyPdf::PdfOptions(), github).Ok() || github.find("0.00 0.58 0.62 rg") != std::string::npos ||
        github.find("([!tip]- Read) Tj") == std::string::npos) {
        std::cerr << "a callout in GitHub's dialect mismatch" << std::endl;
        return false;
    }
    return true;
}

// A wiki link in a book goes to the note of its name wherever in the book it is; one to a note
// outside the book stays a link to the file.
bool CheckBookWikiLinks() {
    TinyPdf::Book book;
    book.chapters.push_back({ "# A\n\nSee [[Deep note#Inner]] and [[Missing]].\n", "a.md", "A", 0, false });
    book.chapters.push_back({ "# Deep note\n\n## Inner\n", "sub/Deep note.md", "Deep note", 0, false });
    TinyPdf::PdfOptions options;
    options.dialect = TinyPdf::MarkdownDialect::Obsidian;
    std::string pdf;
    if (!TinyPdf::BuildBookPdf(book, options, pdf).Ok() || CountOccurrences(pdf, "/URI (Missing.md)") != 1 ||
        pdf.find("/URI (Deep") != std::string::npos || CountOccurrences(pdf, "/Subtype /Link") != 2) {
        std::cerr << "Obsidian book links mismatch" << std::endl;
        return false;
    }
    return true;
}

// Openers that nothing closes, on one line of 2.5 MB: each byte is looked at a bounded number of
// times.
bool CheckOpeners() {
    using math_markdown_tests::Repeat;
    const std::string line = Repeat("[[", size_t(1) << 18) + Repeat("![[a", size_t(1) << 16) + Repeat("`` ` ```", size_t(1) << 16) +
        Repeat("==a ", size_t(1) << 16) + Repeat("$a ", size_t(1) << 16) + Repeat("[[a]", size_t(1) << 16) + "%%";
    return parse_limits_tests::Bounded("2.5 MB of Obsidian openers on one line", 5.0, 256, [&]() {
        const std::string converted = TinyPdf::Internal::ObsidianToCommonMark(line);
        const TinyPdf::Internal::DialectScope scope(TinyPdf::MarkdownDialect::Obsidian);
        return converted.size() == line.size() - 2 && !TinyPdf::Internal::ParseMarkdown(line).empty();
    });
}

bool CheckObsidian() {
    return CheckConversion() && CheckCallouts() && CheckBookWikiLinks() && CheckOpeners();
}

} // namespace dialect_tests
