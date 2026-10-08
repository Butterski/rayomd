// Needs <chrono>, <iostream>, <string>, <string_view>, <vector>; calls InlineVisible of tests/core_tests.cpp.
// Markdown front end of native math: which bytes become formulas, display blocks, or stay text.
// Included inside the anonymous namespace of tests/core_tests.cpp, after its helpers.

namespace math_markdown_tests {

// Inline spans in a compact form: text as is, formulas as <m:tex> or <d:tex>, code as <c:text>.
std::string InlineShape(std::string_view input) {
    using TinyPdf::Internal::InlineMath;
    std::string shape;
    for (const TinyPdf::Internal::InlineSpan& span : TinyPdf::Internal::ParseInlineSpans(input)) {
        if (span.math == InlineMath::Inline) shape += "<m:" + span.text + ">";
        else if (span.math == InlineMath::Display) shape += "<d:" + span.text + ">";
        else if (span.code) shape += "<c:" + span.text + ">";
        else shape += span.text;
    }
    return shape;
}

// Block structure in a compact form, one token per block. Heading and table text is shown
// as stored, with the formula sentinels as { and }; a line feed is shown as /.
std::string BlockShape(const std::vector<TinyPdf::Internal::Block>& blocks) {
    using TinyPdf::Internal::BlockType;
    std::string shape;
    for (const TinyPdf::Internal::Block& block : blocks) {
        if (!shape.empty()) shape += " | ";
        switch (block.type) {
        case BlockType::Heading: shape += block.hasMath ? "H*:" : "H:"; shape += block.text; break;
        case BlockType::Paragraph: shape += "P:" + InlineShape(block.text); break;
        case BlockType::MathBlock: shape += "MATH:" + block.text; break;
        case BlockType::Code: shape += "CODE:" + block.text; break;
        case BlockType::Bullet: shape += "B:" + InlineShape(block.text) + "{" + BlockShape(block.children) + "}"; break;
        case BlockType::Numbered: shape += "N:" + InlineShape(block.text) + "{" + BlockShape(block.children) + "}"; break;
        case BlockType::Quote: shape += "Q{" + BlockShape(block.children) + "}"; break;
        case BlockType::PageBreak: shape += "BREAK"; break;
        case BlockType::Table:
            shape += block.hasMath ? "T*" : "T";
            for (const auto& row : block.rows) {
                shape += "[";
                for (size_t c = 0; c < row.size(); c++) shape += (c ? "," : "") + row[c];
                shape += "]";
            }
            break;
        default: shape += "?"; break;
        }
    }
    for (char& ch : shape) {
        if (ch == TinyPdf::Internal::kMathTextOpen) ch = '{';
        else if (ch == TinyPdf::Internal::kMathTextClose) ch = '}';
        else if (ch == '\n') ch = '/';
    }
    return shape;
}

// Reads the inline text of every paragraph, list item and quote the way the renderers do:
// split at hard breaks, then one ParseInlineSpans call per piece. Returns the span count.
size_t ParseAllInline(const std::vector<TinyPdf::Internal::Block>& blocks) {
    using TinyPdf::Internal::BlockType;
    size_t spans = 0;
    for (const TinyPdf::Internal::Block& block : blocks) {
        if (block.type == BlockType::Paragraph || block.type == BlockType::Bullet ||
            block.type == BlockType::Numbered || block.type == BlockType::Quote) {
            std::string_view text = block.text;
            while (!text.empty()) {
                size_t end = text.find('\n');
                spans += TinyPdf::Internal::ParseInlineSpans(text.substr(0, end)).size();
                if (end == std::string_view::npos) break;
                text.remove_prefix(end + 1);
            }
        }
        spans += ParseAllInline(block.children);
    }
    return spans;
}

std::string Repeat(std::string_view unit, size_t count) {
    std::string out;
    out.reserve(unit.size() * count);
    for (size_t i = 0; i < count; i++) out.append(unit);
    return out;
}

} // namespace math_markdown_tests

bool CheckMathInlineSyntax() {
    using math_markdown_tests::InlineShape;
    struct Case { const char* input; const char* shape; };
    const Case cases[] = {
        // Pandoc's dollar rule; the first closing delimiter decides
        {"Energy $E = mc^2$ holds.", "Energy <m:E = mc^2> holds."},
        {"$5 and $10", "$5 and $10"},
        {"costs $5.", "costs $5."},
        {"a$b$c", "a<m:b>c"},
        {"$ x $", "$ x $"},
        {"$x $ and $ y$", "$x $ and $ y$"},
        {"between $20,000 and $30,000 a year", "between $20,000 and $30,000 a year"},
        {"range $5-$10 per unit", "range $5-$10 per unit"},
        {"price is $x$5 here", "price is $x$5 here"},
        {"H$_2$O and 10$^{6}$", "H<m:_2>O and 10<m:^{6}>"},
        {"($x$), $n$-th and $k$'s", "(<m:x>), <m:n>-th and <m:k>'s"},
        {"pay \\$5 and \\$10, or \\$x\\$", "pay $5 and $10, or $x$"},
        {"$a \\$ b$ end", "<m:a \\$ b> end"},
        {"set $\\{x \\mid x > 0\\}$ done", "set <m:\\{x \\mid x > 0\\}> done"},
        {"\"$HOME\" and \"$PATH\"", "\"<m:HOME\" and \">PATH\""},
        {"$HOME and $PATH", "$HOME and $PATH"},
        {"an unmatched $x stays", "an unmatched $x stays"},
        {"$$", "$$"},
        {"text $$ more", "text $$ more"},
        {"$x$$y$", "<m:x><m:y>"},
        {"$a$a$a$a$a$a", "<m:a>a<m:a>a<m:a>a"},
        // code spans are opaque
        {"`$x$` and $y$", "<c:$x$> and <m:y>"},
        {"costs $5, see `$HOME`", "costs $5, see <c:$HOME>"},
        {"$a `b$` c$", "<m:a `b$` c>"},
        // emphasis markers inside math are not emphasis
        {"$a*b*c$ and $x_i$ and $b_j$", "<m:a*b*c> and <m:x_i> and <m:b_j>"},
        {"the _key idea: $x_{1}$ wins", "the _key idea: <m:x_{1}> wins"},
        {"**bold $x^2$ bold** and ~~gone $y$~~", "bold <m:x^2> bold and gone <m:y>"},
        {"$a_1$_text_ and $*$", "<m:a_1>text and <m:*>"},
        // an autolink keeps its dollars; math after it still works
        {"<https://example.com/?a=$1&b=$2> then $z$", "https://example.com/?a=$1&b=$2 then <m:z>"},
        // inline display math
        {"so $$x = 1$$ follows", "so <d:x = 1> follows"},
        {"a $$ x $$ b $$ y $$ c", "a <d:x> b <d:y> c"},
        {"empty $$$$ and $$ $$ here", "empty $$$$ and $$ $$ here"},
        {"echo $$ prints the pid", "echo $$ prints the pid"},
        // TeX parentheses: padded form only; classic escapes are untouched
        {"padded \\( x^2 \\) is math", "padded <m:x^2> is math"},
        {"tight \\(x^2\\) is not", "tight (x^2) is not"},
        {"\\( open and \\[not a link\\]", "( open and [not a link]"},
        {"f\\(x\\) and [link](\\(foo\\)) and \\( a \\) \\)", "f(x) and link and <m:a> )"},
        {"\\_also literal\\_, \\[not a link\\], and \\`not code\\`.", "_also literal_, [not a link], and `not code`."},
    };
    for (const Case& item : cases) {
        std::string shape = InlineShape(item.input);
        if (shape != item.shape) {
            std::cerr << "math inline mismatch: " << item.input << " -> " << shape << std::endl;
            return false;
        }
    }
    std::vector<TinyPdf::Internal::InlineSpan> emphasis =
        TinyPdf::Internal::ParseInlineSpans("*for all $a*b$ values*");
    if (emphasis.size() != 3 || !emphasis[0].italic || emphasis[1].italic ||
        emphasis[1].math != TinyPdf::Internal::InlineMath::Inline || !emphasis[2].italic) {
        std::cerr << "math inside emphasis mismatch" << std::endl;
        return false;
    }
    // Links keep their label and destination verbatim; math after them still works.
    std::vector<TinyPdf::Internal::InlineSpan> link =
        TinyPdf::Internal::ParseInlineSpans("[cost $5](https://example.com/a?x=$1) and $y$");
    if (link.size() != 3 || link[0].text != "cost $5" || link[0].url != "https://example.com/a?x=$1" ||
        link[2].math != TinyPdf::Internal::InlineMath::Inline || link[2].text != "y") {
        std::cerr << "math next to a link mismatch" << std::endl;
        return false;
    }
    // Existing callers that join span texts keep seeing the TeX source.
    if (InlineVisible("Energy $E=mc^2$") != "Energy E=mc^2" ||
        TinyPdf::Internal::StripInlineMarkdown("Energy $E=mc^2$") != "Energy E=mc^2") {
        std::cerr << "math visible-text compatibility mismatch" << std::endl;
        return false;
    }
    // Text that is read a second time as plain text keeps its dollars and "\(".
    if (TinyPdf::Internal::StripInlineMarkdown("Use $x$ and \\$5 and \\( y \\)", false) !=
            "Use $x$ and \\$5 and ( y )" ||
        TinyPdf::Internal::ParseInlineSpans("*a $b*c$ d*", false).size() != 2) {
        std::cerr << "math-inert inline parse mismatch" << std::endl;
        return false;
    }

    // Size caps. A candidate at the cap is a formula. One byte over, it is not typeset and
    // not re-read as Markdown either: one code span holds it, delimiters included.
    const size_t inlineCap = TinyPdf::Internal::kMaxInlineMathBytes;
    const size_t displayCap = TinyPdf::Internal::kMaxDisplayMathBytes;
    const std::string atCap(inlineCap, 'x');
    const std::string overCap(inlineCap + 1, 'x');
    const std::string displayAtCap(displayCap, 'y');
    const std::string displayOverCap(displayCap + 1, 'y');
    if (InlineShape("$" + atCap + "$") != "<m:" + atCap + ">" ||
        InlineShape("$" + overCap + "$") != "<c:$" + overCap + "$>" ||
        InlineShape("a \\( " + atCap + " \\) b") != "a <m:" + atCap + "> b" ||
        InlineShape("a \\( " + overCap + " \\) b") != "a <c:\\( " + overCap + " \\)> b" ||
        InlineShape("a $$ " + displayAtCap + " $$ b") != "a <d:" + displayAtCap + "> b" ||
        InlineShape("a $$ " + displayOverCap + " $$ b") != "a <c:$$ " + displayOverCap + " $$> b") {
        std::cerr << "inline math size cap mismatch" << std::endl;
        return false;
    }
    // The over-cap source comes back byte for byte: no '_', '*' or backslash is consumed.
    std::string terms;
    for (int i = 0; i < 45; i++) {
        if (i) terms += " + ";
        terms += "x_{" + std::to_string(i) + "}^{*} \\cdot \\{a\\}_i";
    }
    std::vector<TinyPdf::Internal::InlineSpan> verbatim =
        TinyPdf::Internal::ParseInlineSpans("Sum $" + terms + "$ end.");
    if (terms.size() != 1202 || verbatim.size() != 3 || verbatim[0].text != "Sum " ||
        !verbatim[1].code || verbatim[1].math != TinyPdf::Internal::InlineMath::None ||
        verbatim[1].bold || verbatim[1].italic || verbatim[1].text != "$" + terms + "$" ||
        verbatim[2].text != " end.") {
        std::cerr << "over-cap inline math is not verbatim" << std::endl;
        return false;
    }
    // Emphasis markers inside an over-cap candidate stay inert, like those inside a formula.
    const std::string starred = "$" + std::string(inlineCap, 'x') + "*y$";
    std::vector<TinyPdf::Internal::InlineSpan> around =
        TinyPdf::Internal::ParseInlineSpans("*a " + starred + " b*");
    if (around.size() != 3 || !around[0].italic || around[0].text != "a " || !around[1].code ||
        around[1].italic || around[1].text != starred || !around[2].italic || around[2].text != " b") {
        std::cerr << "emphasis around over-cap inline math mismatch" << std::endl;
        return false;
    }
    return true;
}

bool CheckMathBlockSyntax() {
    using math_markdown_tests::BlockShape;
    using math_markdown_tests::InlineShape;
    struct Case { const char* input; const char* shape; };
    const Case cases[] = {
        // $$ blocks
        {"$$ x^2 $$", "MATH:x^2"},
        {"$$\n\\int_0^1 x\\,dx\n= \\frac12\n$$", "MATH:\\int_0^1 x\\,dx/= \\frac12"},
        {"$$ a +\nb $$", "MATH:a +/b"},
        {"$$ x^2 $$ trailing", "P:<d:x^2> trailing"},
        {"$$\nx\n$$ (1)", "P:<d:x> (1)"},
        {"$$\nunterminated\n\nnext", "P:$$ unterminated | P:next"},
        {"$$ cheap\n\ntext\n\n$$\nx = 1\n$$", "P:$$ cheap | P:text | MATH:x = 1"},
        {"$$\n\nx\n\n$$", "P:$$ | P:x | P:$$"},
        {"$$\n$$", "P:$$ $$"},
        {"before\n$$y$$\nafter", "P:before | MATH:y | P:after"},
        {"where $$\nx = 1\n$$ holds", "P:where <d:x = 1> holds"},
        {"$$\na\n+ b\n- c\n# d\n$$", "MATH:a/+ b/- c/# d"},
        {"$$ \\text{cost } \\$5 $$", "MATH:\\text{cost } \\$5"},
        // fences
        {"```math\n\\frac{a}{b}\n```", "MATH:\\frac{a}{b}"},
        {"~~~ Math\nx\n~~~", "MATH:x"},
        {"```math\nnever closed", "CODE:never closed"},
        {"```math\n```", "CODE:"},
        {"```latex\nx\n```", "CODE:x/"},
        {"```\n$x$ and $$y$$\n```", "CODE:$x$ and $$y$$/"},
        {"    $$\n    code\n    $$", "CODE:$$/code/$$"},
        // \[ \]
        {"\\[\nx = 1\n\\]", "MATH:x = 1"},
        {"\\[ E = mc^2 \\]", "MATH:E = mc^2"},
        {"\\[Illustration\\]", "P:[Illustration]"},
        {"\\[1\\] A reference line", "P:[1] A reference line"},
        {"\\[\nnever closed\n\ntext", "P:[ never closed | P:text"},
        {"\\pagebreak", "BREAK"},
        // paragraphs, lists, quotes
        {"sum $a +\nb$ soft", "P:sum <m:a + b> soft"},
        {"see [price $5][p] and $x$\n\n[p]: https://example.com/p?a=$b", "P:see price $5 and <m:x>"},
        {"- item $x$\n\n    $$\n    y = 1\n    $$\n", "B:item <m:x>{MATH:y = 1}"},
        {"- item with $x$ and\n  $$\n  y = 1\n  $$\n- next", "B:item with <m:x> and{MATH:y = 1} | B:next{}"},
        {"- $$z$$", "B:{MATH:z}"},
        {"1. first $$a$$ inline display", "N:first <d:a> inline display{}"},
        {"> quoted $q$\n> $$\n> r = 2\n> $$\n> tail", "Q{P:quoted <m:q> | MATH:r = 2 | P:tail}"},
        // headings
        {"# Energy $E=mc^2$ title", "H*:Energy {E=mc^2} title"},
        {"Mass $m$\n===", "H*:Mass {m}"},
        {"## Costs $5 and $10", "H:Costs $5 and $10"},
        {"# Both $$a$$ and \\( b \\)", "H*:Both {a} and {b}"},
        // tables: cells split at every unescaped pipe, exactly as without math; a table without
        // formulas keeps the inline Markdown of its cells for the renderers
        {"| f | v |\n|---|---|\n| $\\alpha$ | $\\frac{a}{b}$ |", "T*[f,v][{\\alpha},{\\frac{a}{b}}]"},
        {"| a | b |\n|---|---|\n| $|x|$ | abs |", "T[a,b][$,x,$,abs]"},
        {"| a | b |\n|---|---|\n| $\\|x\\|$ | \\( y \\) |", "T*[a,b][{|x|},{y}]"},
        {"| a | b |\n|---|---|\n| $\\|x\\|$ | $a \\mid b$ |", "T*[a,b][{|x|},{a \\mid b}]"},
        {"| a | b |\n|---|---|\n| $5 | 10$ |", "T[a,b][$5,10$]"},
        {"| a | b |\n|---|---|\n| \\$5 | x\\|y \\*z\\* |", "T[a,b][\\$5,x|y \\*z\\*]"},
        {"| a | b |\n|---|---|\n| \\( x \\) | `$y$` |", "T*[a,b][{x},$y$]"},
        // every row keeps its backslashes for the inline parser
        {"| a | b |\n|---|---|\n| C:\\dir | $x$ |\n| C:\\dir | y |", "T*[a,b][C:\\dir,{x}][C:\\dir,y]"},
    };
    for (const Case& item : cases) {
        std::string shape = BlockShape(TinyPdf::Internal::ParseMarkdown(item.input));
        if (shape != item.shape) {
            std::cerr << "math block mismatch: " << item.input << " -> " << shape << std::endl;
            return false;
        }
    }

    // A hard line break splits inline text before it is parsed, so math cannot span it.
    std::vector<TinyPdf::Internal::Block> hard = TinyPdf::Internal::ParseMarkdown("sum $a +  \nb$ hard");
    if (hard.size() != 1 || hard[0].text != "sum $a +\nb$ hard" ||
        InlineShape("sum $a +") != "sum $a +" || InlineShape("b$ hard") != "b$ hard") {
        std::cerr << "math across a hard break mismatch" << std::endl;
        return false;
    }

    // Control bytes in ordinary input can never be read as a formula: without math the
    // text is untouched and the flag stays clear; with math the literal bytes are removed.
    std::vector<TinyPdf::Internal::Block> plain = TinyPdf::Internal::ParseMarkdown("# Ctl \x01x\x02 none");
    std::vector<TinyPdf::Internal::Block> mixed = TinyPdf::Internal::ParseMarkdown("# Ctl \x01x\x02 with $y$");
    if (plain.size() != 1 || plain[0].hasMath || plain[0].text != "Ctl \x01x\x02 none" ||
        mixed.size() != 1 || !mixed[0].hasMath || mixed[0].text != "Ctl x with \x01y\x02") {
        std::cerr << "math sentinel isolation mismatch (heading)" << std::endl;
        return false;
    }
    std::vector<TinyPdf::Internal::Block> table = TinyPdf::Internal::ParseMarkdown(
        "| a | $n$ |\n|---|---|\n| plain \x01raw\x02 | 2 |\n");
    if (table.size() != 1 || !table[0].hasMath || table[0].rows.size() != 2 ||
        table[0].rows[0][1] != "\x01n\x02" || table[0].rows[1][0] != "plain raw") {
        std::cerr << "math sentinel isolation mismatch (table)" << std::endl;
        return false;
    }
    // A sentinel byte typed inside a formula does not survive either.
    std::vector<TinyPdf::Internal::Block> inside = TinyPdf::Internal::ParseMarkdown("# T $a\x02\x01" "b$");
    if (inside.size() != 1 || !inside[0].hasMath || inside[0].text != "T \x01" "ab\x02") {
        std::cerr << "math sentinel isolation mismatch (formula)" << std::endl;
        return false;
    }

    // The renderers read such text back through ForEachMathTextSegment: text and TeX in
    // order, empty segments skipped.
    std::string segments;
    auto collect = [&segments](std::string_view segment, bool isMath) {
        segments += isMath ? "{" : "[";
        segments.append(segment);
        segments += isMath ? "}" : "]";
    };
    TinyPdf::Internal::ForEachMathTextSegment(mixed[0].text, collect);
    TinyPdf::Internal::ForEachMathTextSegment("\x01" "a\x02\x01\x02\x01" "b\x02 end", collect);
    TinyPdf::Internal::ForEachMathTextSegment("plain", collect);
    TinyPdf::Internal::ForEachMathTextSegment("", collect);
    if (segments != "[Ctl x with ]{y}" "{a}{b}[ end]" "[plain]") {
        std::cerr << "math text segment mismatch: " << segments << std::endl;
        return false;
    }

    // An over-cap candidate in a heading or a table cell is plain text with the same bytes:
    // no flag, no sentinel, backslashes and markers intact.
    const std::string longMath = "$" + std::string(TinyPdf::Internal::kMaxInlineMathBytes, 'x') + "\\alpha_i*$";
    std::vector<TinyPdf::Internal::Block> longHeading = TinyPdf::Internal::ParseMarkdown("# Long " + longMath);
    std::vector<TinyPdf::Internal::Block> longCell = TinyPdf::Internal::ParseMarkdown(
        "| a | b |\n|---|---|\n| " + longMath + " | 2 |\n");
    if (longHeading.size() != 1 || longHeading[0].hasMath || longHeading[0].text != "Long " + longMath ||
        longCell.size() != 1 || longCell[0].hasMath || longCell[0].rows.size() != 2 ||
        longCell[0].rows[1][0] != longMath) {
        std::cerr << "over-cap math in a heading or table cell mismatch" << std::endl;
        return false;
    }
    return true;
}

// Unterminated openers must not make parsing quadratic. Several of these inputs took tens
// of seconds with a naive "search to the end for every opener" implementation.
bool CheckMathHostileInput() {
    using math_markdown_tests::Repeat;
    const std::string inputs[] = {
        Repeat("$$x\n", 100000),
        Repeat("\\[\n", 100000),
        Repeat("\\( a ", 100000) + "\n",
        Repeat("$a ", 100000) + "\n",
        Repeat("x $$a ", 100000) + "\n",
        std::string(200000, '$') + "\n",
        "$a" + Repeat("\\$", 150000) + "\n",
        "| a | b |\n|---|---|\n" + Repeat("| $x | y$ |\n", 50000),
        std::string(30, '>') + " $$\n" + std::string(30, '>') + " x\n" + std::string(30, '>') + " $$\n",
        // over-cap candidates: every one is a verbatim span, none is rescanned
        Repeat("$" + std::string(1100, 'x') + "$ ", 500) + "\n",
        "# " + Repeat("$" + std::string(1100, '*') + "$ ", 500) + "\n",
        "*a " + Repeat("$$" + std::string(9000, '*') + "$$ ", 60) + "\n",
    };
    const auto started = std::chrono::steady_clock::now();
    for (const std::string& input : inputs) {
        std::vector<TinyPdf::Internal::Block> blocks = TinyPdf::Internal::ParseMarkdown(input);
        // Paragraph text is parsed by the renderers; do the same here so the inline
        // scanner sees these inputs too.
        if (blocks.empty() || math_markdown_tests::ParseAllInline(blocks) > input.size()) {
            std::cerr << "hostile math input parse mismatch" << std::endl;
            return false;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (seconds > 10.0) {
        std::cerr << "hostile math inputs took " << seconds << " s" << std::endl;
        return false;
    }
    return true;
}
