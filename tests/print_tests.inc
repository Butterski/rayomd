// SPDX-License-Identifier: Apache-2.0

// Needs <string>, <vector>; uses math_markdown_tests::Repeat, parse_limits_tests::Bounded and
// CountOccurrences of tests/core_tests.cpp, so it is included after parse_limits_tests.inc.
// Print output: --black-text (PdfOptions::blackText, print_colors.cpp) in both renderers, and
// --hide-tags (PdfOptions::hideTags, Obsidian's dialect). Included inside the anonymous namespace
// of tests/core_tests.cpp.

namespace print_tests {

struct ColorOp {
    std::string op;
    double values[3] = { 0.0, 0.0, 0.0 };
};

bool IsPdfSpace(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0';
}

bool IsPdfRegular(char c) {
    return !IsPdfSpace(c) && std::string_view("()<>[]{}/%").find(c) == std::string_view::npos;
}

// A content stream without its colour operators and their operands, every run of white space one
// space, and the operators removed in `ops`: a tokenizer of its own, not print_colors.cpp's.
std::string StripContent(std::string_view s, std::vector<ColorOp>& ops) {
    std::string kept;
    std::vector<std::pair<size_t, size_t>> numbers;
    size_t copied = 0;
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (IsPdfSpace(c)) {
            i++;
        } else if (c == '(') {
            int depth = 0;
            for (; i < s.size(); i++) {
                if (s[i] == '\\') i++;
                else if (s[i] == '(') depth++;
                else if (s[i] == ')' && --depth == 0) break;
            }
            i++;
            numbers.clear();
        } else if (c == '<') {
            i = i + 1 < s.size() && s[i + 1] == '<' ? i + 2 : s.find('>', i) + 1;
            numbers.clear();
        } else if (c == '/') {
            for (i++; i < s.size() && IsPdfRegular(s[i]); i++) {}
            numbers.clear();
        } else if (!IsPdfRegular(c)) {
            i++;
            numbers.clear();
        } else {
            size_t end = i;
            while (end < s.size() && IsPdfRegular(s[end])) end++;
            const std::string_view token = s.substr(i, end - i);
            if (token.find_first_not_of("0123456789.-+") == std::string_view::npos) {
                numbers.push_back({ i, end });
            } else {
                const size_t count = token == "rg" || token == "RG" ? 3 : token == "g" || token == "G" ? 1 : 0;
                if (count > 0 && numbers.size() >= count) {
                    ColorOp op;
                    op.op = std::string(token);
                    for (size_t index = 0; index < count; index++) {
                        const auto [from, to] = numbers[numbers.size() - count + index];
                        op.values[index] = std::strtod(std::string(s.substr(from, to - from)).c_str(), nullptr);
                    }
                    ops.push_back(op);
                    const size_t start = numbers[numbers.size() - count].first;
                    kept.append(s.substr(copied, start - copied));
                    copied = end;
                }
                numbers.clear();
            }
            i = end;
        }
    }
    kept.append(s.substr(copied));
    std::string collapsed;
    for (const char c : kept) {
        if (!IsPdfSpace(c)) collapsed += c;
        else if (collapsed.empty() || collapsed.back() != ' ') collapsed += ' ';
    }
    return collapsed;
}

// The PDF with StripContent applied to each stream of plain content (no /Subtype, /Filter, /Type
// or font lengths: pages, overlays, the cover); other streams, images among them, as they are.
std::string StripColors(const std::string& pdf, std::vector<ColorOp>& ops) {
    std::string out;
    size_t copied = 0;
    for (size_t at = pdf.find("stream\n"); at != std::string::npos; at = pdf.find("stream\n", at)) {
        const size_t dictStart = pdf.rfind("<<", at);
        const std::string_view dict(pdf.data() + dictStart, at - dictStart);
        const size_t dataStart = at + 7;
        const size_t dataEnd = pdf.find("endstream", dataStart);
        if (dataEnd == std::string::npos) break;
        if (dict.find("/Subtype") == std::string_view::npos && dict.find("/Filter") == std::string_view::npos &&
            dict.find("/Type") == std::string_view::npos && dict.find("/Length1") == std::string_view::npos) {
            out.append(pdf, copied, dataStart - copied);
            out += StripContent(std::string_view(pdf).substr(dataStart, dataEnd - dataStart), ops);
            copied = dataEnd;
        }
        at = dataEnd + 9;
    }
    out.append(pdf, copied, std::string::npos);
    return out;
}

// The streams of the PDF's form XObjects, one after another.
std::string FormStreams(const std::string& pdf) {
    std::string forms;
    for (size_t at = pdf.find("/Subtype /Form"); at != std::string::npos; at = pdf.find("/Subtype /Form", at + 1)) {
        const size_t data = pdf.find("stream\n", at) + 7;
        forms += pdf.substr(data, pdf.find("endstream", data) - data);
        forms += '\n';
    }
    return forms;
}

// PrintColors on the content syntax RayoMD writes: strings and property lists pass through,
// every colour operator becomes grey in place (the expected text without its padding).
bool CheckRewrite() {
    const auto collapse = [](const std::string& text) {
        std::string out;
        for (const char c : text) {
            if (c != ' ' || out.empty() || out.back() != ' ') out += c;
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    };
    const std::pair<std::string, std::string> cases[] = {
        { "(a 0 0 1 rg b) Tj 0.05 0.30 0.68 rg", "(a 0 0 1 rg b) Tj 0 g" },
        { "(\\) 0 0 1 rg (x) 1 g) Tj\n0.2 G", "(\\) 0 0 1 rg (x) 1 g) Tj\n0 G" },
        { "<0041 0 g> Tj 0.96 0.97 0.98 rg", "<0041 0 g> Tj 0.968 g" },
        { "/P <</MCID 3>> BDC 0.9 0.9 0.9 RG", "/P <</MCID 3>> BDC 0.9 G" },
        { "/F1 9 Tf 1 g 0 g 0.08 g\n1 1 1 rg Q", "/F1 9 Tf 1 g 0 g 0 g \n1 g Q" },
        { "/g1 gs /Fg 2 Tf 3 rgx 0.5 0.5 0.5 rg", "/g1 gs /Fg 2 Tf 3 rgx 0.5 g" },
        { "0.08 g", "0 g" },
    };
    for (const auto& [content, expected] : cases) {
        std::string stream = content;
        TinyPdf::Internal::PrintColors(stream);
        if (stream.size() != content.size() || collapse(stream) != collapse(expected)) {
            std::cerr << "PrintColors mismatch: " << content << " -> " << stream << std::endl;
            return false;
        }
    }
    return true;
}

// Every colour operator of the default PDF is, in the black-text PDF, its grey: black below a
// luminance of 0.5, else that luminance; nothing else differs. Counts the fills kept light.
bool SameButGrey(const std::string& normal, const std::string& black, const char* what, size_t& lightFills) {
    std::vector<ColorOp> normalOps;
    std::vector<ColorOp> blackOps;
    if (StripColors(normal, normalOps) != StripColors(black, blackOps) || normalOps.size() != blackOps.size() ||
        normalOps.empty()) {
        std::cerr << "black text changed more than colours: " << what << std::endl;
        return false;
    }
    for (size_t index = 0; index < normalOps.size(); index++) {
        const ColorOp& from = normalOps[index];
        const ColorOp& to = blackOps[index];
        const bool rgb = from.op == "rg" || from.op == "RG";
        const double luminance = rgb ? 0.299 * from.values[0] + 0.587 * from.values[1] + 0.114 * from.values[2] : from.values[0];
        const double expected = luminance < 0.4995 ? 0.0 : luminance;   // in thousandths, rounded
        const bool stroke = from.op == "RG" || from.op == "G";
        if (to.op != (stroke ? "G" : "g") || to.values[0] < expected - 0.0006 || to.values[0] > expected + 0.0006) {
            std::cerr << "black text: " << from.op << " " << from.values[0] << " " << from.values[1] << " " << from.values[2]
                      << " became " << to.op << " " << to.values[0] << " (" << what << ")" << std::endl;
            return false;
        }
        if (rgb && expected > 0.0) lightFills++;
    }
    return true;
}

bool CheckBlackText() {
    const std::string ascii =
        "# Print\n\n[TOC]\n\n## Links and marks\n\nA [link](https://example.com), <mark>marked</mark>, `code`, **bold** and *it*;"
        " the text 0 0 1 rg (1 g) \\(x\\) stays.\n\n- [ ] task\n- [x] done\n  - nested\n\n> [!WARNING]\n> Careful.\n\n"
        "> Quote.\n\n```python\ndef f(x):\n    return \"s\" + 1  # comment\n```\n\n| A | B |\n|---|---|\n| 1 | $x^2$ |\n\n"
        "Math $\\frac{a}{b}$ inline.\n\n$$\n\\sum_{i=1}^n i\n$$\n\n---\n\nNote[^1].\n\n[^1]: The note.\n\n"
        "![logo](docs/assets/branding/rayomd.png)\n";
    const std::string unicode = ascii + "\nZa\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 \xE2\x86\x92\n";
    std::vector<TinyPdf::PdfOptions> sets(4);
    sets[1].pageNumbers = true;
    sets[1].toc = true;
    sets[1].numberHeadings = 2;
    sets[2].theme.headingColor = 0x0B3D91;
    sets[2].theme.linkColor = 0xCC3399;
    sets[2].theme.accentColor = 0xF2A900;
    sets[2].theme.headerLeft = "{title}";
    sets[2].theme.footerRight = "{page} / {pages}";
    sets[2].theme.cover = true;
    sets[3].pdfua = true;
    sets[3].pageNumbers = true;
    size_t lightFills = 0;
    size_t built = 0;
    for (size_t set = 0; set < sets.size(); set++) {
        for (const std::string* text : { &ascii, &unicode }) {
            TinyPdf::PdfOptions options = sets[set];
            std::string normal;
            std::string black;
            const bool made = TinyPdf::BuildPdf(*text, options, normal).Ok();
            options.blackText = true;
            if (made != TinyPdf::BuildPdf(*text, options, black).Ok()) {
                std::cerr << "black text: one build failed" << std::endl;
                return false;
            }
            if (!made) continue;   // PDF/UA without a font
            built++;
            const std::string what = "set " + std::to_string(set) + (text == &ascii ? ", standard" : ", Unicode");
            if (normal.size() != black.size() || !SameButGrey(normal, black, what.c_str(), lightFills)) return false;
        }
    }
    if (built < 6 || lightFills == 0) {
        std::cerr << "black text: " << built << " builds, " << lightFills << " light fills" << std::endl;
        return false;
    }
    return true;
}

// A diagram takes the grey palette, also for the colours its source gives, and the cache keeps the
// two drawings apart.
bool CheckBlackDiagram() {
    const std::string document = "```mermaid\nflowchart LR\n  PrintA[Start] --> PrintB{Grey?}\n  style PrintA fill:#f9f\n```\n";
    TinyPdf::PdfOptions options;
    const uint64_t before = TinyPdf::Internal::Mermaid::Layouts();
    std::string normal;
    std::string black;
    std::string normalAgain;
    std::string blackAgain;
    const bool normalMade = TinyPdf::BuildPdf(document, options, normal).Ok();
    options.blackText = true;
    const bool blackMade = TinyPdf::BuildPdf(document, options, black).Ok() && TinyPdf::BuildPdf(document, options, blackAgain).Ok();
    options.blackText = false;
    if (!normalMade || !blackMade || !TinyPdf::BuildPdf(document, options, normalAgain).Ok() ||
        TinyPdf::Internal::Mermaid::Layouts() - before != 2 || normal != normalAgain || black != blackAgain) {
        std::cerr << "black diagram: builds or cache mismatch" << std::endl;
        return false;
    }
    std::vector<ColorOp> normalOps;
    std::vector<ColorOp> blackOps;
    StripContent(FormStreams(normal), normalOps);
    StripContent(FormStreams(black), blackOps);
    bool coloured = false;
    for (const ColorOp& op : normalOps) coloured |= op.values[0] != op.values[1] || op.values[1] != op.values[2];
    bool grey = !blackOps.empty();
    for (const ColorOp& op : blackOps) grey &= op.op == "g" || op.op == "G";
    if (!coloured || !grey) {
        std::cerr << "black diagram: its form is not grey" << std::endl;
        return false;
    }
    return true;
}

bool CheckHideTags() {
    const std::pair<std::string, std::string> cases[] = {
        { "a #tag b", "a b" },
        { "#y1984 x", "x" },
        { "x #a/b-c_d", "x" },
        { "`#a` #b", "`#a`" },
        { "```\n#a\n```\n#b", "```\n#a\n```\n" },
        { "%% #a %% #b c", "%% #a %% c" },
        { "%% #a\n#b\n%% #c d", "%% #a\n#b\n%% d" },
        { "#\xC5\xBC x", "x" },
        { "p\n#a #b\nq", "p\n\nq" },
        { "$x #a$ #b", "$x #a$" },
        { "[[note|alias #a]] #b", "[[note|alias #a]]" },
        { "---\ntags: #a\n---\n#b c", "---\ntags: #a\n---\nc" },
        { "text\n    #tag", "text\n   " },
        { "a\t#t b", "a\tb" },
        { "a #b\r\n", "a\r\n" },
        { "# Head #h", "# Head" },
    };
    for (const auto& [text, expected] : cases) {
        std::string out;
        if (!TinyPdf::Internal::HideTags(text, out) || out != expected) {
            std::cerr << "hidden tags mismatch: " << text << " -> " << out << std::endl;
            return false;
        }
    }
    for (const char* kept : { "#1984 x", "a#b", "http://x/#frag", "# Heading", "    #include <x>\n", "$$\n#a\n$$", "\\#a",
             "##a", "x\n\n    #code" }) {
        std::string out = "untouched";
        if (TinyPdf::Internal::HideTags(kept, out) || out != "untouched") {
            std::cerr << "hidden tags: " << kept << " changed" << std::endl;
            return false;
        }
    }
    // In a build: Obsidian's dialect only, after its own syntax.
    const std::string note = "Keep #tag this.\n\n#solo #tags\nNext ==m== #t\n";
    TinyPdf::PdfOptions options;
    options.hideTags = true;
    std::string github;
    std::string plain;
    std::string hidden;
    const bool githubMade = TinyPdf::BuildPdf(note, options, github).Ok();
    options.hideTags = false;
    const bool plainMade = TinyPdf::BuildPdf(note, options, plain).Ok();
    options.dialect = TinyPdf::MarkdownDialect::Obsidian;
    options.hideTags = true;
    if (!githubMade || !plainMade || github != plain || !TinyPdf::BuildPdf(note, options, hidden).Ok() ||
        hidden.find("(Keep this.) Tj") == std::string::npos || hidden.find("#t") != std::string::npos ||
        hidden.find("solo") != std::string::npos || hidden.find("==") != std::string::npos) {
        std::cerr << "hidden tags in a build mismatch" << std::endl;
        return false;
    }
    return true;
}

// A megabyte of tags, and of tags among openers nothing closes: linear.
bool CheckHideTagsBounded() {
    using math_markdown_tests::Repeat;
    const std::string tags = Repeat("#a ", size_t(350000));
    const std::string openers = Repeat("` #a $ #b [[ #c %", size_t(1) << 16);
    return parse_limits_tests::Bounded("a megabyte of #a #a with hidden tags", 5.0, 256, [&]() {
        std::string out;
        std::string mixed;
        const TinyPdf::Internal::DialectScope scope(TinyPdf::MarkdownDialect::Obsidian, true);
        return TinyPdf::Internal::HideTags(tags, out) && out.empty() && TinyPdf::Internal::HideTags(openers, mixed) &&
            !TinyPdf::Internal::ParseMarkdown(tags + "\n" + openers).empty();
    });
}

bool CheckPrint() {
    return CheckRewrite() && CheckBlackText() && CheckBlackDiagram() && CheckHideTags() && CheckHideTagsBounded();
}

} // namespace print_tests
