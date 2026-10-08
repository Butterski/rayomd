// Needs <algorithm>, <chrono>, <cmath>, <cstdint>, <iostream>, <string>, <string_view>, <utility>, <vector>; calls no helper of tests/core_tests.cpp.
// Box layout and PDF emission of native math (src/core/math_layout.h, math_parser.h).
// Included inside the anonymous namespace of tests/core_tests.cpp, after its helpers.

namespace math_layout_tests {

using TinyPdf::Internal::MathFallbackFont;
using TinyPdf::Internal::MathFormula;

bool failed = false;
bool anySourceFallback = false;   // set by Lay(); only the two fallback cases may set it
double worstHostileMs = 0.0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    failed = true;
    std::cerr << "math layout: " << what << std::endl;
}

MathFormula Lay(std::string_view tex, bool display = false, double maxWidth = 0.0, double maxHeight = 0.0,
                double size = 10.0, const MathFallbackFont* fallback = nullptr, bool bold = false) {
    MathFormula formula = MathFormula::Layout(tex, size, display, fallback, maxWidth, bold, maxHeight);
    if (formula.SourceFallback()) anySourceFallback = true;
    return formula;
}

std::string Emitted(const MathFormula& formula, double x = 72.0, double baseline = 700.0) {
    std::string content;
    formula.Emit(content, x, baseline, "0.1 0.2 0.3");
    return content;
}

size_t Count(std::string_view text, std::string_view needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) ++count;
    return count;
}

bool Has(std::string_view text, std::string_view needle) { return text.find(needle) != std::string_view::npos; }

// A PDF number as the module writes it: sign, digits, at most two decimals.
double Number(std::string_view token) {
    double value = 0.0, place = 1.0;
    bool negative = false, fraction = false;
    for (const char ch : token) {
        if (ch == '-') negative = true;
        else if (ch == '.') fraction = true;
        else if (fraction) value += (ch - '0') * (place /= 10.0);
        else value = value * 10.0 + (ch - '0');
    }
    return negative ? -value : value;
}

// What the operators of one emitted formula say about its structure.
struct Stream {
    int q = 0, Q = 0, bt = 0, et = 0, tj = 0;
    bool ascii = true, foreignFont = false, badLineWidth = false, badRectangle = false, finite = true;
    std::vector<std::string> lineWidths;         // every operand of w, as written
    std::vector<std::string> lineWidthsInText;   // those between BT and ET
};

Stream Scan(const std::string& content) {
    Stream stream;
    std::vector<std::string> operands;
    bool inText = false;
    for (size_t i = 0; i < content.size();) {
        const unsigned char ch = static_cast<unsigned char>(content[i]);
        if (ch >= 0x7F || (ch < 0x20 && ch != '\n')) stream.ascii = false;
        if (ch == ' ' || ch == '\n') {
            ++i;
            continue;
        }
        size_t end = i;
        if (ch == '(') {
            while (end < content.size() && content[end] != ')') end += content[end] == '\\' ? 2 : 1;
            ++end;
        } else {
            while (end < content.size() && content[end] != ' ' && content[end] != '\n') ++end;
        }
        const std::string token = content.substr(i, end - i);
        i = end;
        const char first = token[0];
        if ((first >= '0' && first <= '9') || first == '-' || first == '.') {
            if (token.find_first_not_of("0123456789.-") != std::string::npos) stream.finite = false;
            operands.push_back(token);
            continue;
        }
        if (first == '(' || first == '<') {
            operands.push_back(token);
            continue;
        }
        if (first == '/') {
            if (token.size() != 3 || token[1] != 'M' || token[2] < '1' || token[2] > '5') stream.foreignFont = true;
            continue;
        }
        const size_t n = operands.size();
        if (token == "q") ++stream.q;
        else if (token == "Q") ++stream.Q;
        else if (token == "BT") { ++stream.bt; inText = true; }
        else if (token == "ET") { ++stream.et; inText = false; }
        else if (token == "Tj") ++stream.tj;
        else if (token == "w" && n >= 1) {
            if (!(Number(operands[n - 1]) > 0.0)) stream.badLineWidth = true;
            stream.lineWidths.push_back(operands[n - 1]);
            if (inText) stream.lineWidthsInText.push_back(operands[n - 1]);
        } else if (token == "re" && n >= 4) {
            if (!(Number(operands[n - 2]) > 0.0) || !(Number(operands[n - 1]) > 0.0)) stream.badRectangle = true;
        }
        operands.clear();
    }
    return stream;
}

// The rules every emission obeys (B 5.4): one q ... Q, at most one BT ... ET, 7-bit ASCII, only
// /M1../M5, positive line widths and rectangle sides, the same bytes from every call.
void ExpectWellFormed(const MathFormula& formula, const char* what) {
    const std::string content = Emitted(formula);
    const Stream stream = Scan(content);
    Expect(!content.empty() && content.rfind("q 0.1 0.2 0.3 rg", 0) == 0, what);
    Expect(content.size() > 3 && content.compare(content.size() - 3, 3, " Q\n") == 0, what);
    Expect(Count(content, "\n") == 1, what);
    Expect(stream.q == 1 && stream.Q == 1 && stream.bt <= 1 && stream.bt == stream.et, what);
    Expect(stream.ascii && stream.finite && !stream.foreignFont, what);
    Expect(!stream.badLineWidth && !stream.badRectangle, what);
    Expect(!Has(content, "/F"), what);
    Expect(content == Emitted(formula), what);
    Expect(std::isfinite(formula.Width()) && formula.Width() >= 0.0 && formula.Ascent() >= 0.0 && formula.Descent() >= 0.0, what);
}

std::string Repeat(std::string_view unit, size_t bytes) {
    std::string out;
    while (out.size() + unit.size() <= bytes) out += unit;
    return out;
}

// The inputs of the parser's adversarial set (16 KiB each), an 8,000-atom formula nested 23
// deep and a 64 x 24 matrix.
std::vector<std::string> HostileInputs() {
    const size_t limit = 16384;
    std::vector<std::string> inputs = {
        Repeat("\\text{", limit), Repeat("\\text{a & ", limit), Repeat("\\text{$", limit),
        "\\text{" + Repeat("$x", limit - 16) + "}", Repeat("{", limit), Repeat("}", limit),
        Repeat("\\hat", limit), Repeat("\\left(", limit), Repeat("\\begin{matrix}", limit),
        Repeat("\\end{matrix}", limit), Repeat("x^", limit), Repeat("&", 31) + Repeat("\\\\", 120),
        "{" + Repeat("&", limit - 2) + "}", "{" + Repeat("&", 30), Repeat("{a&}", limit),
        Repeat(Repeat("{a&", 93) + "\\\\", 95 * 60), "x" + Repeat("'", limit - 1), Repeat("\\", limit),
        Repeat("%\n", limit), Repeat("\\sqrt[", limit), Repeat("\\frac", limit),
        Repeat("\\operatorname", limit), Repeat("\\not", limit), Repeat("a\\over ", limit),
        Repeat("\\substack{", limit), Repeat("\xC2\xB2", limit),
        "\\text{" + Repeat("{", limit - 7) + "}",
        Repeat("{", 22) + Repeat("x", 8000) + Repeat("}", 22),
    };
    std::string matrix = "\\begin{matrix}";
    for (int row = 0; row < 64; ++row) matrix += Repeat("a&", 46) + "a\\\\";
    inputs.push_back(matrix + "\\end{matrix}");
    return inputs;
}

struct FallbackProbe {
    int measured = 0, emitted = 0;
    std::string text;
    double x = 0.0, baseline = 0.0, size = 0.0;
};
double ProbeMeasure(void* context, std::string_view, double size) {
    ++static_cast<FallbackProbe*>(context)->measured;
    return 3.0 * size;
}
void ProbeEmit(void* context, std::string& content, std::string_view utf8, double x, double baseline, double size, const char*) {
    FallbackProbe& probe = *static_cast<FallbackProbe*>(context);
    ++probe.emitted;
    probe.text = std::string(utf8);
    probe.x = x;
    probe.baseline = baseline;
    probe.size = size;
    content += "% fallback\n";
}

bool Near(double value, double expected, double tolerance) { return std::fabs(value - expected) <= tolerance; }

bool Run() {
    using TinyPdf::Internal::MathFontObject;
    using TinyPdf::Internal::MathNode;
    using TinyPdf::Internal::MathNodeKind;
    using TinyPdf::Internal::MathParseResult;
    using TinyPdf::Internal::MathSymbolDescriptorObject;
    using TinyPdf::Internal::StandardTextFont;
    using TinyPdf::Internal::StandardTextWidth;
    failed = false;
    anySourceFallback = false;

    // Empty formulas and struts (L15).
    {
        Expect(Lay("").Empty(), "an empty source is Empty()");
        Expect(Lay("{}").Empty() && Lay("\\!").Empty(), "a formula without extent is Empty()");
        Expect(Emitted(Lay("{}")).empty(), "an empty formula emits nothing");
        const MathFormula strut = Lay("\\vphantom{\\frac12}");
        Expect(!strut.Empty() && strut.Width() == 0.0 && strut.Ascent() > 0.0 && strut.Descent() > 0.0, "a strut has height and no width");
        Expect(Emitted(strut).empty(), "a strut paints nothing");
        MathFormula copy = strut;
        std::vector<MathFormula> pool(3);
        pool.push_back(std::move(copy));
        Expect(pool[0].Empty() && !pool.back().Empty() && pool.back().Ascent() == strut.Ascent(), "formulas are default-constructible and movable");
    }

    // Metrics of one letter; exact text widths of the standard renderer's fonts.
    {
        const MathFormula x = Lay("x");
        Expect(Near(x.Width(), 4.47, 0.005) && Near(x.Ascent(), 4.41, 0.005) && Near(x.Descent(), 0.11, 0.005), "metrics of $x$ at 10 pt");
        const std::string_view sample = "Plain line before. Inline math ";
        Expect(Near(StandardTextWidth(sample, 11.5, StandardTextFont::Regular), 148.2925, 1e-9), "Helvetica width");
        Expect(Near(StandardTextWidth(sample, 11.5, StandardTextFont::Bold), 159.137, 1e-9), "Helvetica-Bold width");
        Expect(Near(StandardTextWidth(sample, 11.5, StandardTextFont::Mono), 31 * 6.9, 1e-9), "Courier width");
        Expect(StandardTextWidth("\x01\x7F\xC5\x82", 10.0, StandardTextFont::Regular) == 0.0, "bytes outside 32..126 have no width");
    }

    // The six font objects.
    {
        using namespace TinyPdf::Internal;
        Expect(kMathFontCount == 5 && kMathFontObjectCount == 6, "font counts");
        Expect(std::string(kMathFonts[0].resourceName) == "M1" && std::string(kMathFonts[4].resourceName) == "M5" &&
               std::string(kMathFonts[4].baseFont) == "Symbol", "font table");
        Expect(MathSymbolDescriptorObject() ==
               "<< /Type /FontDescriptor /FontName /Symbol /Flags 4 /FontBBox [-180 -293 1090 1010] /ItalicAngle 0 "
               "/Ascent 1010 /Descent -293 /CapHeight 673 /StemV 85 >>", "Symbol descriptor");
        Expect(MathFontObject(0, 7) == "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman >>", "Times-Roman object");
        Expect(MathFontObject(1, 7) == "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Italic >>", "Times-Italic object");
        Expect(MathFontObject(2, 7) == "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Bold >>", "Times-Bold object");
        Expect(MathFontObject(3, 7) == "<< /Type /Font /Subtype /Type1 /BaseFont /Times-BoldItalic >>", "Times-BoldItalic object");
        Expect(MathFontObject(5, 7).empty() && MathFontObject(-1, 7).empty(), "font index outside 0..4");
        const std::string symbol = MathFontObject(4, 37);
        const std::string_view head = "<< /Type /Font /Subtype /Type1 /BaseFont /Symbol /FirstChar 32 /LastChar 254 /Widths [250 333 713 ";
        const std::string_view tail = " 494 494 494] /FontDescriptor 37 0 R >>";
        Expect(symbol.rfind(head, 0) == 0, "Symbol font object, head");
        Expect(symbol.size() > tail.size() && symbol.compare(symbol.size() - tail.size(), tail.size(), tail) == 0, "Symbol font object, tail");
        const size_t open = symbol.find('['), close = symbol.find(']');
        long sum = 0;
        int widths = 0;
        if (open != std::string::npos && close != std::string::npos) {
            Expect(close - open - 1 == 827, "Symbol /Widths is 827 bytes long");
            for (size_t i = open + 1; i < close;) {
                size_t end = symbol.find_first_of(" ]", i);
                sum += static_cast<long>(Number(std::string_view(symbol).substr(i, end - i)));
                ++widths;
                i = end + 1;
            }
        }
        Expect(widths == 223 && sum == 110936, "Symbol /Widths: 223 values summing to 110,936");
    }

    // Emission structure.
    {
        const MathFormula fraction = Lay("\\frac{a}{b} = \\sqrt{2}", true);
        const std::string content = Emitted(fraction);
        ExpectWellFormed(fraction, "emission of a fraction and a radical");
        Expect(Has(content, "/M2 10 Tf") && Has(content, "(a) Tj") && Has(content, "<3D> Tj") && Has(content, "(2) Tj"), "fraction: glyphs");
        Expect(Has(content, " 0.1 0.2 0.3 RG") && Has(content, " w ") && Has(content, " l S"), "fraction: the bar is a stroked rule");
        Expect(Has(content, " h f"), "radical: a filled path");

        const MathFormula alpha = Lay("\\alpha^2");
        const std::string greek = Emitted(alpha);
        ExpectWellFormed(alpha, "emission of a slanted Greek letter");
        Expect(Has(greek, "/M5 9 Tf 1 0 0.18 1 ") && Has(greek, " Tm <61> Tj"), "alpha: small, slanted, hex string");
        Expect(Has(greek, " Tf 1 0 0 1 ") && Has(greek, " Tm (2) Tj"), "the run after a slanted one resets the text matrix");
        Expect(!Has(greek, " RG"), "no stroke colour without strokes");

        const MathFormula blackboard = Lay("\\mathbb{R}");
        const std::string outline = Emitted(blackboard);
        ExpectWellFormed(blackboard, "emission of a blackboard letter");
        Expect(Has(outline, "/M3 10 Tf 1 Tr 0.25 w ") && Has(outline, "(R) Tj") && Has(outline, " RG"), "blackboard: outlined Times-Bold");

        const MathFormula negated = Lay("a \\not\\equiv b");
        const std::string slash = Emitted(negated);
        ExpectWellFormed(negated, "emission of a negated relation");
        Expect(Has(slash, "<BA> Tj") && Has(slash, "0.52 w ") && Has(slash, " l S"), "negation: the relation and a stroked slash");

        const MathFormula contour = Lay("\\oint", true);
        const std::string ring = Emitted(contour);
        ExpectWellFormed(contour, "emission of a contour integral");
        Expect(Has(ring, "/M5 19 Tf") && Has(ring, "<F2> Tj") && Count(ring, " c") == 4 && Has(ring, " h S"), "contour integral: glyph and ring");

        const MathFormula rotated = Lay("\\mp");
        Expect(Has(Emitted(rotated), "-1 0 0 -1 ") && Has(Emitted(rotated), " Tm <B1> Tj"), "a rotated composite piece");
        ExpectWellFormed(rotated, "emission of a composite");

        const std::string heavy = Emitted(Lay("x + \\alpha", false, 0.0, 0.0, 10.0, nullptr, true));
        Expect(Has(heavy, "/M4 10 Tf") && Has(heavy, "2 Tr") && !Has(heavy, "/M2 "), "bold context: bold faces and heavy Symbol");

        std::string shifted;
        fraction.Emit(shifted, 0.0, 0.0, "0 0 0");
        Expect(shifted.rfind("q 0 0 0 rg 0 0 0 RG BT ", 0) == 0, "colour is used for fill and stroke");
    }

    // Joining glyphs into strings (L3).
    {
        Expect(Has(Emitted(Lay("\\mathrm{abc}")), "(abc) Tj") && Count(Emitted(Lay("\\mathrm{abc}")), " Tj") == 1, "\\mathrm{abc} is one string");
        Expect(Has(Emitted(Lay("\\operatorname{softmax}")), "(softmax) Tj"), "an operator name is one string");
        Expect(Has(Emitted(Lay("ab")), "(ab) Tj") && !Has(Emitted(Lay("fg")), "(fg)"), "italic letters join only without italic correction");
        const std::string dot = Emitted(Lay("\\dot{m}c"));
        Expect(Has(dot, "<C7> Tj") && Has(dot, "(m) Tj") && Has(dot, "(c) Tj") && Count(dot, " Tj") == 3, "the letter after \\dot{m} starts its own string");
        const std::string hat = Emitted(Lay("\\mathrm{\\hat{K}K}"));
        Expect(Has(hat, "<C3> Tj") && Count(hat, "(K) Tj") == 2 && Count(hat, " Tj") == 3, "the letter after \\hat{K} starts its own string");
        // The macron is drawn as a rule: there is no accent glyph to append to.
        const std::string bar = Emitted(Lay("\\bar{a}b"));
        Expect(Has(bar, "(a) Tj") && Has(bar, "(b) Tj") && Count(bar, " Tj") == 2 && Has(bar, " l S"), "the letter after \\bar{a} starts its own string");
        const std::string barK = Emitted(Lay("\\mathrm{\\bar{K}K}"));
        Expect(Count(barK, "(K) Tj") == 2 && Count(barK, " Tj") == 2, "the letter after \\bar{K} starts its own string");
        Expect(Count(Emitted(Lay("\\alpha\\beta")), " Tj") == 2, "Symbol glyphs are never joined");
        Expect(Has(Emitted(Lay("\\text{if } x")), "(if) Tj"), "a trailing space of a text run only advances");
        Expect(Has(Emitted(Lay("\\text{a b}")), "(a b) Tj"), "an inner space is part of the string");
    }

    // Line width follows the size of outlined glyphs.
    {
        const Stream stream = Scan(Emitted(Lay("\\mathbb{R}^{\\mathbb{N}}")));
        Expect(stream.lineWidthsInText.size() == 2 && stream.lineWidthsInText[0] != stream.lineWidthsInText[1],
               "\\mathbb{R}^{\\mathbb{N}} writes two line widths inside BT ... ET");
    }

    // Negative spaces never give negative sizes (L18).
    for (const char* tex : {"\\!x\\,", "\\overline{\\hspace{-2em}}\\hspace{3em}", "\\sqrt{\\!\\!\\!\\!}", "\\boxed{\\hspace{-3em}}",
                            "\\hspace{-5em}x", "x\\hspace{-5em}", "\\frac{\\!\\!}{\\kern-3em}", "\\underline{\\!}", "\\cancel{\\!}"}) {
        const MathFormula formula = Lay(tex);
        const Stream stream = Scan(Emitted(formula));
        Expect(formula.Width() >= 0.0 && formula.Ascent() >= 0.0 && formula.Descent() >= 0.0, tex);
        Expect(!stream.badLineWidth && !stream.badRectangle && stream.q == stream.Q, tex);
    }

    // Primes (L8).
    {
        Expect(Has(Emitted(Lay("\\sum'_{n} a_n", true)), "<A2> Tj"), "the prime of a sum with limits is drawn");
        Expect(Lay("\\left(\\frac{f}{g}\\right)'", true).Ascent() > Lay("\\left(\\frac{f}{g}\\right)", true).Ascent() + 0.1,
               "the prime of a tall nucleus is raised");
        Expect(Near(Lay("x'").Ascent(), 7.35, 0.005), "the prime of a letter is not raised");
        Expect(Count(Emitted(Lay("f'''")), "<A2> Tj") == 3, "three primes");
    }

    // \middle (L16).
    {
        const MathFormula plain = Lay("\\left(\\frac{a}{b}\\right)", true);
        const MathFormula middle = Lay("\\left(\\frac{a}{b}\\middle| c\\right)", true);
        const MathFormula without = Lay("\\left(\\frac{a}{b} c\\right)", true);
        Expect(Near(middle.Ascent(), plain.Ascent(), 1e-9) && Near(middle.Descent(), plain.Descent(), 1e-9), "\\middle is built for the target of its pair");
        Expect(middle.Width() > without.Width() + 1.0, "\\middle takes room");
        ExpectWellFormed(middle, "emission of \\middle");
    }

    // Script-level arrays (L11).
    {
        const double extra = Lay("\\sum_{\\substack{a\\\\b}}", true).Descent() - Lay("\\sum_{a}", true).Descent();
        Expect(extra >= 7.0 && extra <= 9.0, "a second \\substack line adds 0.7 to 0.9 em of depth");
        const MathFormula small = Lay("\\left(\\begin{smallmatrix}a&b\\\\c&d\\end{smallmatrix}\\right)");
        Expect(small.Ascent() + small.Descent() <= 13.5, "an inline smallmatrix is at most 1.35 em tall");
        const MathFormula full = Lay("\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}");
        Expect(full.Ascent() + full.Descent() > 20.0, "a pmatrix keeps the body strut");
    }

    // Fit (L14 with the 50 % floor on the overall size).
    {
        const std::string wide = "a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p = q";
        const double natural = Lay(wide, true).Width();
        Expect(natural > 200.0, "natural width of the fit sample");
        for (const double factor : {1.2, 1.8}) {
            const MathFormula fitted = Lay(wide, true, natural / factor);
            Expect(fitted.Width() <= natural / factor && fitted.Width() > 0.9 * natural / factor, "a formula 1.2x or 1.8x too wide is fitted");
            ExpectWellFormed(fitted, "emission of a fitted formula");
        }
        Expect(Has(Emitted(Lay(wide, true, natural / 1.8)), " cm BT"), "the uniform scale is one cm operator");
        Expect(Lay(wide, true, natural / 2.2).Width() > natural / 2.2, "below half its size a formula reports its overflow");
        Expect(Lay(wide, true, natural / 10.0).Width() > natural / 10.0, "a formula 10x too wide reports its overflow");
        Expect(Lay(wide, true, natural + 1.0).Width() == natural, "a formula that fits is not touched");

        const std::string tagged = wide + " \\tag{1}";
        const double taggedNatural = Lay(tagged, true).Width();
        Expect(taggedNatural > natural + 20.0, "a tag follows the body after two em");
        for (const double factor : {1.2, 1.8}) {
            Expect(Lay(tagged, true, taggedNatural / factor).Width() == taggedNatural / factor, "a tagged formula that fits is exactly as wide as the line");
        }
        Expect(Lay(tagged, true, taggedNatural * 2.0).Width() == taggedNatural * 2.0, "a tag is set flush right");
        Expect(Lay(tagged, false, taggedNatural * 2.0).Width() == taggedNatural, "an inline tag stays in place");
        Expect(Lay(tagged, true, taggedNatural / 10.0).Width() > taggedNatural / 10.0, "a tagged formula 10x too wide reports its overflow");
        Expect(Lay("x \\tag{" + std::string(120, 'W') + "}", true, 300.0).Width() > 300.0, "a tag wider than the line reports its overflow");
        Expect(Has(Emitted(Lay("x \\tag{7}", true, 300.0)), "(\\() Tj") && Has(Emitted(Lay("x \\tag*{7}", true, 300.0)), "(7) Tj") &&
               !Has(Emitted(Lay("x \\tag*{7}", true, 300.0)), "(\\()"), "tag parentheses");

        const std::string block = "\\begin{aligned} a &= b + c + d + e + f + g + h \\\\ x &= y \\\\ u &= v \\\\ p &= q \\end{aligned}";
        const MathFormula free = Lay(block, true);
        const double maxWidth = free.Width() / 1.2, maxHeight = (free.Ascent() + free.Descent()) / 1.3;
        const MathFormula both = Lay(block, true, maxWidth, maxHeight);
        Expect(both.Width() <= maxWidth && both.Ascent() + both.Descent() <= maxHeight, "width and height limits are met together");
        const MathFormula onlyHeight = Lay(block, true, 0.0, maxHeight);
        Expect(onlyHeight.Ascent() + onlyHeight.Descent() <= maxHeight && onlyHeight.Width() < free.Width(), "the height limit alone shrinks the formula");

        const std::string rows = "\\begin{align} a &= b \\tag{1} \\\\ c &= d \\tag{2} \\end{align}";
        Expect(Lay(rows, true, 400.0).Width() == 400.0 && Lay(rows, true).Width() < 200.0, "row tags of a top-level grid go flush right");
    }
    Expect(!anySourceFallback, "no ordinary case is a source fallback");

    // Source fallback: text, never shrunk.
    {
        const std::string longText = "\\text{" + std::string(10000, 'm') + "}";
        const MathFormula unlimited = MathFormula::Layout(longText, 10.0, true);
        const MathFormula limited = MathFormula::Layout(longText, 10.0, true, nullptr, 300.0, false, 40.0);
        Expect(unlimited.SourceFallback() && limited.SourceFallback(), "10,000 glyphs are a layout limit");
        Expect(unlimited.Width() == limited.Width() && unlimited.Width() > 300.0, "a source fallback is not fitted");
        Expect(Has(Emitted(unlimited), "(\\\\text{mmmm") && Has(Emitted(unlimited), "...) Tj"), "a source fallback shows the cut source");
        ExpectWellFormed(unlimited, "emission of a source fallback");

        MathParseResult deep;
        deep.nodes.assign(72, MathNode{});
        for (uint16_t i = 1; i <= 70; ++i) {
            deep.nodes[i].kind = MathNodeKind::Group;
            deep.nodes[i].cls = TinyPdf::Internal::MathClass::Ord;
            deep.nodes[i].child[0] = static_cast<uint16_t>(i + 1);
        }
        deep.nodes[71].kind = MathNodeKind::Glyph;
        deep.nodes[71].cls = TinyPdf::Internal::MathClass::Ord;
        deep.nodes[71].aux = 2;
        deep.nodes[71].a = 'x';
        deep.root = 1;
        const MathFormula nested = MathFormula::LayoutParsed(deep, "nested {x}", 10.0, false);
        const MathFormula nestedLimited = MathFormula::LayoutParsed(deep, "nested {x}", 10.0, false, nullptr, 5.0);
        Expect(nested.SourceFallback() && nestedLimited.SourceFallback() && nested.Width() == nestedLimited.Width() && nested.Width() > 5.0,
               "70 nested groups are a layout limit");
        Expect(Has(Emitted(nested), "(nested {x}) Tj"), "the layout's source fallback shows the source");
        deep.nodes[10].child[0] = 71;   // 10 levels deep
        Expect(!MathFormula::LayoutParsed(deep, "x", 10.0, false).SourceFallback(), "10 nested groups are laid out");
    }

    // Fallback font: measured by the caller, boxed by the given ascent and descent.
    {
        FallbackProbe probe;
        const MathFallbackFont font{&probe, &ProbeMeasure, &ProbeEmit, 0.8, 0.3};
        const std::string word = "\\text{za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87}";
        const MathFormula withFont = MathFormula::Layout(word, 10.0, false, &font);
        Expect(probe.measured == 1 && probe.emitted == 0, "measure is called during layout only");
        Expect(Near(withFont.Width(), 30.0, 1e-9) && Near(withFont.Ascent(), 8.0, 1e-9) && Near(withFont.Descent(), 3.0, 1e-9), "fallback box");
        std::string content;
        withFont.Emit(content, 100.0, 200.0, "0 0 0");
        Expect(probe.emitted == 1 && probe.measured == 1 && content == "% fallback\n", "emit is called once, after the formula");
        Expect(probe.text == "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87" && Near(probe.x, 100.0, 1e-6) && Near(probe.baseline, 200.0, 1e-6) && Near(probe.size, 10.0, 1e-6),
               "emit gets the word, its place and its size");
        const MathFormula mixed = MathFormula::Layout("x_{" + word + "}", 10.0, false, &font, 0.5 * 30.0);
        content.clear();
        mixed.Emit(content, 0.0, 0.0, "0 0 0");
        Expect(probe.measured == 3 && probe.emitted == 2 && probe.size < 7.2 && Has(content, " Q\n% fallback\n"),
               "fallback text is measured in each pass, scaled, and painted after Q");

        const MathFormula withoutFont = MathFormula::Layout(word, 10.0, false);
        Expect(Has(Emitted(withoutFont), "(za???" "?) Tj") && probe.measured == 3 && probe.emitted == 2, "without a fallback font the letters become ?");
        const MathFallbackFont broken{&probe, &ProbeMeasure, &ProbeEmit, std::nan(""), 1e300};
        const MathFormula clamped = MathFormula::Layout(word, 10.0, false, &broken);
        Expect(Near(clamped.Ascent(), 7.4, 1e-9) && Near(clamped.Descent(), 20.0, 1e-9), "fallback metrics are sanitised");
    }

    // Hostile inputs: bounded time, well-formed output.
    {
        const std::vector<std::string> inputs = HostileInputs();
        const auto started = std::chrono::steady_clock::now();
        worstHostileMs = 0.0;
        size_t typeset = 0;
        for (const std::string& tex : inputs) {
            const auto before = std::chrono::steady_clock::now();
            const MathFormula formula = MathFormula::Layout(tex, 10.0, true, nullptr, 400.0, false, 600.0);
            std::string content;
            formula.Emit(content, 72.0, 700.0, "0 0 0");
            worstHostileMs = std::max(worstHostileMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
            const Stream stream = Scan(content);
            Expect(std::isfinite(formula.Width()) && formula.Width() >= 0.0 && formula.Ascent() >= 0.0 && formula.Descent() >= 0.0, "hostile input: metrics");
            Expect(stream.ascii && stream.finite && stream.q == stream.Q && stream.bt == stream.et && stream.bt <= 1 &&
                   !stream.badLineWidth && !stream.badRectangle && !stream.foreignFont, "hostile input: emission");
            if (!formula.SourceFallback() && !formula.Empty()) ++typeset;
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        Expect(seconds < 2.0, "the hostile set takes less than two seconds");
        Expect(typeset >= 2, "the 8,000-atom formula and the 64 x 24 matrix are typeset");
    }

    return !failed;
}

} // namespace math_layout_tests

bool CheckMathLayout() { return math_layout_tests::Run(); }
