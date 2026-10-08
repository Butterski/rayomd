// Needs <chrono>, <iostream>, <string>, <string_view>, <vector>; uses math_markdown_tests::Repeat
// and ParseAllInline, so it is included after math_markdown_tests.inc.
// The inline parser looks ahead for closing delimiters. Past a byte budget it answers those
// lookups from tables instead of scanning: the spans must be the same either way, and text
// made of thousands of unterminated openers must stay fast.
// Included inside the anonymous namespace of tests/core_tests.cpp.

namespace inline_lookahead_tests {

bool SameSpans(const std::vector<TinyPdf::Internal::InlineSpan>& a, const std::vector<TinyPdf::Internal::InlineSpan>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].text != b[i].text || a[i].url != b[i].url || a[i].bold != b[i].bold || a[i].italic != b[i].italic ||
            a[i].strike != b[i].strike || a[i].code != b[i].code || a[i].math != b[i].math) return false;
    }
    return true;
}

// Scans only, tables only, a switch after a few bytes, and the default must agree.
bool SameEitherWay(const std::string& text) {
    constexpr size_t kScansOnly = static_cast<size_t>(-2);
    for (bool math : { true, false }) {
        const std::vector<TinyPdf::Internal::InlineSpan> scanned = TinyPdf::Internal::ParseInlineSpans(text, math, kScansOnly);
        for (size_t budget : { size_t(0), size_t(48), TinyPdf::Internal::kDefaultLookaheadBudget }) {
            if (!SameSpans(scanned, TinyPdf::Internal::ParseInlineSpans(text, math, budget))) {
                std::cerr << "inline look-ahead mismatch (budget " << budget << ", math " << math << "): "
                    << text.substr(0, 160) << std::endl;
                return false;
            }
        }
    }
    return true;
}

} // namespace inline_lookahead_tests

bool CheckInlineLookahead() {
    using inline_lookahead_tests::SameEitherWay;
    using math_markdown_tests::Repeat;

    // Interactions between the things a lookup has to step over.
    const char* const cases[] = {
        "*a *b* c*", "**a *b** c*", "***a** b*", "*a **b* c**", "_a_b_ c_", "__a__ _b_", "*a* *", "****", "*****a*****",
        "*a `b* c` d*", "*a `b c*", "*a ``b` c`` d*", "`a `` b` ``", "``` a ` b `` c", "*a \\* b*", "*a \\\\* b*", "*a\\",
        "*a $b* c$ d*", "$a *b$ c* $d$", "*a $$b*$$ c*", "*a \\( b* \\) c*", "*a $ b* $c", "$a$b$c$ *d $e$ f*", "*a $b `c$ d` e*",
        // A dollar inside a link is no formula for the parser, but a lookup that starts
        // before the link reads one from there, and the next lookup starts inside it.
        "*x [t]($a) *y b* c$ d", "*x [t]($a) *y b$ c* d*", "*x <http://a$b> *y c* d$ e", "_x [t]($a) _y b_ c$ d",
        "**x [t]($$a) **y b** c$$ d", "*x [t](\\( a) *y b* c \\) d", "*x [t]($a) *y `b* c$` d*",
        "[a](b(c)d) [e](f", "[a]((x) [b](y)", "[a](<b) c> d)", "[a](<b [c](d)", "[a](b\\)c) [d](e", "[a](b\\\\) c) [d](e)",
        "[a] [b](c) ]", "[[[a](b)", "![a](b ![c](d)", "[a](\\<b) [c](<d>)", "[a](b<c) d>e) f)",
        "<a <b@c.de> >", "<http://x <y>", "<a@b@c.de> <d@e.fg>", "< <a@b.c>", "<a <b <c",
        "~~a ~~b~~ c~~", "*a ~~b* c~~", "\\\\\\*a*", "*a_b_c*", "_a*b*c_", "* a*", "*a *", " *a* ", "**", "*", "",
    };
    for (const char* text : cases) {
        if (!SameEitherWay(text)) return false;
    }

    // Long texts over small alphabets, in which openers, closers, escapes, code spans,
    // links and formulas keep meeting. The sequence is fixed, so a failure reproduces.
    const char* const alphabets[][10] = {
        { "*", "**", "a", " ", "***", "b", " ", "*", "_", "a" },
        { "_", "__", "a", " ", "a_b", "___", " ", "_", "*", "b" },
        { "*", "a", " ", "`", "``", "b", " ", "`", "\\", "*" },
        { "*", "a", " ", "$", "b", "$", "$$", " ", "\\( ", " \\)" },
        { "[", "a", "]", "(", ")", "<", ">", "\\", " ", "](" },
        { "<", "a", "@", ".", "b", ">", " ", "http://", "-", "<" },
        { "`", "``", "```", "a", " ", "````", "b", "\\", "*", "`" },
        { "*", "_", "`", "$", "[", "]", "(", ")", "<", "\\" },
        { "~~", "a", " ", "~", "*", "b", "~~", "_", "c", " " },
        { "*", "a", "$", "b", "$", "c", "*", " ", "_", "$" },
        { "*", "a", " ", "[t](", ")", "$", "b", "*", "<http://x", ">" },
        { "_", "a", " ", "[t](", ")", "$$", "`", "_", "\\( ", " \\)" },
    };
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32_t>(state >> 16);
    };
    for (int round = 0; round < 1500; round++) {
        const char* const* alphabet = alphabets[next() % (sizeof(alphabets) / sizeof(alphabets[0]))];
        std::string text;
        const uint32_t pieces = 1 + next() % (round % 5 == 0 ? 500 : 80);
        for (uint32_t i = 0; i < pieces; i++) text += alphabet[next() % 10];
        if (!SameEitherWay(text)) return false;
    }

    // One piece repeated: the scans are quadratic here, so these stay small for the comparison.
    const char* const pieces[] = {
        "*a ", "**a ", "_a ", "***a ", "*a $ ", "*a $b$ ", "*a `b ", "*a\\* ", "~~a ", "[a ", "[a](", "[a](<", "![a](", "<a ",
        "<a@b.c ", "$a ", "$$a ", "\\( a ", "[a][b] ", "`", "*", "_", "(", "\\", "*a` ", "_a_ ", "*a* ", "[a](b) ", "`a` ",
    };
    const char* const tails[] = { "", "b*", "b_", "]", ")", ">", "`", "$", "b**", ">)", "\\)" };
    for (const char* piece : pieces) {
        for (const char* tail : tails) {
            if (!SameEitherWay(Repeat(piece, 300) + tail)) return false;
        }
    }
    std::string growingRuns;
    for (size_t run = 1; run <= 60; run++) growingRuns += std::string(run, '`') + "a ";
    if (!SameEitherWay(growingRuns) || !SameEitherWay("a" + std::string(2000, '*') + "b") ||
        !SameEitherWay("a " + std::string(2000, '_') + " b") || !SameEitherWay(std::string(2000, '`') + "a")) {
        return false;
    }

    // The same pieces at a size where one scan per opener took seconds for each input.
    std::vector<std::string> large;
    for (const char* piece : pieces) large.push_back(Repeat(piece, 120000 / std::string(piece).size()) + "\n");
    large.push_back(Repeat("*a ", 40000) + "b*\n");
    large.push_back(Repeat("[a](", 30000) + ")\n");
    large.push_back(Repeat("<a ", 40000) + ">\n");
    large.push_back(Repeat("[a ", 40000) + "]\n");
    large.push_back("| a | b |\n|---|---|\n| " + Repeat("*a ", 30000) + " | x |\n");
    large.push_back("- " + Repeat("_a ", 30000) + "\n");
    large.push_back("> " + Repeat("**a ", 25000) + "\n");
    std::string manyRuns;
    for (size_t run = 1; manyRuns.size() < 120000; run++) manyRuns += std::string(run, '`') + "a ";
    large.push_back(manyRuns + "\n");
    large.push_back("a" + std::string(120000, '*') + "b\n");
    large.push_back("a " + std::string(120000, '`') + " b\n");
    const auto started = std::chrono::steady_clock::now();
    for (const std::string& input : large) {
        std::vector<TinyPdf::Internal::Block> blocks = TinyPdf::Internal::ParseMarkdown(input);
        if (blocks.empty() || math_markdown_tests::ParseAllInline(blocks) > input.size()) {
            std::cerr << "hostile inline input parse mismatch" << std::endl;
            return false;
        }
        std::string pdf;
        if (!Build(input, pdf)) {
            std::cerr << "hostile inline input build failed" << std::endl;
            return false;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (seconds > 20.0) {
        std::cerr << "hostile inline inputs took " << seconds << " s" << std::endl;
        return false;
    }
    return true;
}
