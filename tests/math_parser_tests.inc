// Needs <cstdint> <cstdio> <iostream> <string> <string_view> <vector> and "../src/core/math_parser.h"; calls no helper of core_tests.cpp.
// Tests of the TeX-subset math parser (src/core/math_parser.cpp). Everything of this fragment
// lives in namespace math_parser_tests; only CheckMathParser() is defined outside it.

namespace math_parser_tests {

using namespace TinyPdf::Internal;

const char* const kKindNames[] = {
    "Null", "Glyph", "Composite", "TextRun", "Fallback", "Space", "Style", "Group", "Scripts", "Fraction",
    "Radical", "LeftRight", "Middle", "SizedDelim", "Accent", "OverUnder", "Stack", "Enclose", "Phantom",
    "Text", "OpName", "Array", "Row", "Cell", "Col"};
const char* const kClassNames[] = {"Ord", "Op", "Bin", "Rel", "Open", "Close", "Punct", "Inner"};
constexpr int kKindCount = static_cast<int>(sizeof(kKindNames) / sizeof(kKindNames[0]));
constexpr int kCompositeCount = 57;

const char* KindName(MathNodeKind kind) {
    return static_cast<int>(kind) < kKindCount ? kKindNames[static_cast<int>(kind)] : "?";
}

const char* ClassName(MathClass cls) {
    return static_cast<int>(cls) < 8 ? kClassNames[static_cast<int>(cls)] : "None";
}

MathParseResult Parse(std::string_view tex, bool display = false, bool fallback = false) {
    MathParseResult result;
    ParseMath(tex, display, fallback, result);
    return result;
}

// ---- the canonical arena text rayomd-math-arena/1 -----------------------------------------
// Pre-order numbering: a node, its child lists 0, 1, 2, then its next sibling; the root list
// first, then the tag list. Text bytes are rewritten in that order.
void Number(const MathParseResult& result, uint16_t head, std::vector<uint16_t>& order,
            std::vector<uint16_t>& number) {
    for (uint16_t i = head; i != 0 && i < result.nodes.size() && !number[i]; i = result.nodes[i].next) {
        order.push_back(i);
        number[i] = static_cast<uint16_t>(order.size());
        for (uint16_t child : result.nodes[i].child) Number(result, child, order, number);
    }
}

// From the "root" line to "end": the part of an .arena file that depends on the parser.
std::string ArenaText(const MathParseResult& result) {
    std::vector<uint16_t> order;
    std::vector<uint16_t> number(result.nodes.size(), 0);
    Number(result, result.root, order, number);
    Number(result, result.tag, order, number);
    const auto renumbered = [&](uint16_t index) { return index < number.size() ? number[index] : 0; };
    std::string text;
    std::string lines;
    char line[160];
    for (uint16_t index : order) {
        const MathNode& node = result.nodes[index];
        long a = node.a;
        if (node.kind == MathNodeKind::TextRun || node.kind == MathNodeKind::Fallback) {
            a = static_cast<long>(text.size());
            if (static_cast<size_t>(node.a) + node.b <= result.text.size()) text.append(result.text, node.a, node.b);
        } else if (node.kind == MathNodeKind::Space) {
            a = static_cast<int16_t>(node.a);
        }
        std::snprintf(line, sizeof(line), "%d %s %s %d %d %ld %d %d %d %d %d\n", renumbered(index),
                      KindName(node.kind), ClassName(node.cls), node.flags, node.aux, a, node.b,
                      renumbered(node.next), renumbered(node.child[0]), renumbered(node.child[1]),
                      renumbered(node.child[2]));
        lines += line;
    }
    std::snprintf(line, sizeof(line), "root %d tag %d tagparens %d sourcefallback %d\ntext ",
                  renumbered(result.root), renumbered(result.tag), result.tagParens ? 1 : 0,
                  result.sourceFallback ? 1 : 0);
    std::string out = line;
    for (unsigned char byte : text) {
        std::snprintf(line, sizeof(line), "%02x", byte);
        out += line;
    }
    if (text.empty()) out += '-';
    std::snprintf(line, sizeof(line), "\nnodes %d\n", static_cast<int>(order.size()));
    out += line;
    out += lines;
    out += "end\n";
    return out;
}

uint64_t Fnv1a64(std::string_view data) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : data) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

// ---- arena rules (contract: every index in range, every node referenced exactly once, payload
// ranges per node kind). Returns an empty string when the arena obeys them. ----------------------
bool WellFormedUtf8(std::string_view bytes) {
    size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char lead = static_cast<unsigned char>(bytes[i]);
        size_t extra = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead < 0x20 || lead == 0x7F) return false;          // control bytes never reach the painter
        if (lead < 0x80) extra = 0;
        else if (lead >= 0xC2 && lead <= 0xDF) extra = 1;
        else if (lead >= 0xE0 && lead <= 0xEF) { extra = 2; if (lead == 0xE0) low = 0xA0; if (lead == 0xED) high = 0x9F; }
        else if (lead >= 0xF0 && lead <= 0xF4) { extra = 3; if (lead == 0xF0) low = 0x90; if (lead == 0xF4) high = 0x8F; }
        else return false;
        for (size_t k = 1; k <= extra; k++) {
            if (i + k >= bytes.size()) return false;
            const unsigned char next = static_cast<unsigned char>(bytes[i + k]);
            if (next < low || next > high) return false;
            low = 0x80;
            high = 0xBF;
        }
        i += extra + 1;
    }
    return true;
}

std::string ArenaDefect(const MathParseResult& result) {
    const std::vector<MathNode>& nodes = result.nodes;
    const size_t count = nodes.size();
    if (count == 0) return "no null node";
    if (count - 1 > static_cast<size_t>(kMathMaxNodes)) return "more than kMathMaxNodes nodes";
    const MathNode& null = nodes[0];
    if (null.kind != MathNodeKind::Null || null.cls != MathClass::None || null.flags || null.aux || null.a ||
        null.b || null.next || null.child[0] || null.child[1] || null.child[2]) {
        return "the null node was written to";
    }
    std::vector<uint8_t> references(count, 0);
    const auto refer = [&](uint16_t index) {
        if (index >= count) return false;
        if (index && references[index] < 2) references[index]++;
        return true;
    };
    if (!refer(result.root) || !refer(result.tag)) return "root or tag out of range";
    for (size_t i = 1; i < count; i++) {
        const MathNode& node = nodes[i];
        if (!refer(node.next) || !refer(node.child[0]) || !refer(node.child[1]) || !refer(node.child[2])) {
            return "index out of range";
        }
        if (node.kind == MathNodeKind::Null || static_cast<int>(node.kind) >= kKindCount) return "bad node kind";
        if (static_cast<int>(node.cls) > 7 && node.cls != MathClass::None) return "bad class";
        const size_t end = static_cast<size_t>(node.a) + node.b;
        switch (node.kind) {
        case MathNodeKind::Glyph:
            if (node.aux < 1 || node.aux > 5 || node.a < 0x20 || node.a > 0xFF) return "glyph font or code";
            if (node.b >= kBigOpCount) return "glyph big-operator group";
            break;
        case MathNodeKind::Composite:
            if (node.a >= kCompositeCount || node.b >= kBigOpCount) return "composite id";
            break;
        case MathNodeKind::TextRun:
            if (node.aux < 1 || node.aux > 4 || node.b == 0 || end > result.text.size()) return "text run font or range";
            for (size_t k = node.a; k < end; k++) {
                const unsigned char byte = static_cast<unsigned char>(result.text[k]);
                if ((byte < 0x20 || byte > 0x7E) && byte != 0xBC) return "text run byte";
            }
            break;
        case MathNodeKind::Fallback:
            if (node.b == 0 || end > result.text.size()) return "fallback range";
            if (!WellFormedUtf8(std::string_view(result.text).substr(node.a, node.b))) return "fallback bytes";
            break;
        case MathNodeKind::Style:
            if (node.aux > 3) return "style level";
            break;
        case MathNodeKind::Scripts:
            if (node.aux > kMathMaxPrimes || node.a > kLimitsOff) return "scripts primes or limits mode";
            if (!node.child[0] || nodes[node.child[0]].next) return "scripts nucleus is not one node";
            if (!node.child[1] && !node.child[2] && !node.aux && node.a == kLimitsDefault) return "empty scripts node";
            if (nodes[node.child[0]].cls != node.cls) return "scripts class differs from its nucleus";
            break;
        case MathNodeKind::Fraction:
        case MathNodeKind::LeftRight:
            if (node.a >= kDelimCount || node.b >= kDelimCount) return "delimiter id";
            break;
        case MathNodeKind::Middle:
            if (node.a >= kDelimCount) return "delimiter id";
            break;
        case MathNodeKind::SizedDelim:
            if (node.a >= kDelimCount || node.aux > 4) return "sized delimiter";
            break;
        case MathNodeKind::Accent:
            if (node.aux >= kAccentCount) return "accent id";
            break;
        case MathNodeKind::OverUnder:
            if (node.aux > kUnderBrace) return "over/under id";
            break;
        case MathNodeKind::Stack:
            if (node.aux > kStackLeftArrow) return "stack arrow";
            break;
        case MathNodeKind::Enclose:
            if (node.aux > kEncloseXCancel) return "enclose kind";
            break;
        case MathNodeKind::Phantom:
            if (node.aux > kPhantomHeight) return "phantom kind";
            break;
        case MathNodeKind::Array: {
            if ((node.a & 0xFF) >= kDelimCount || (node.a >> 8) >= kDelimCount) return "array delimiter";
            if ((node.aux & kArrayStyleMask) > 2) return "array cell style";
            int rows = 0;
            int columns = 0;
            for (uint16_t row = node.child[0]; row && row < count && rows <= kMathMaxRows; row = nodes[row].next) {
                if (nodes[row].kind != MathNodeKind::Row) return "array child is not a row";
                rows++;
                int cells = 0;
                for (uint16_t cell = nodes[row].child[0]; cell && cell < count && cells <= kMathMaxColumns; cell = nodes[cell].next) {
                    if (nodes[cell].kind != MathNodeKind::Cell) return "row child is not a cell";
                    cells++;
                }
                if (cells < 1 || cells > kMathMaxColumns) return "cells of a row";
            }
            for (uint16_t column = node.child[1]; column && column < count && columns <= kMathMaxColumns; column = nodes[column].next) {
                if (nodes[column].kind != MathNodeKind::Col || nodes[column].aux > kColumnRight) return "array column";
                columns++;
            }
            if (rows < 1 || rows > kMathMaxRows || columns < 1 || columns > kMathMaxColumns) return "array rows or columns";
            break;
        }
        case MathNodeKind::Row:
            if (node.aux > 3) return "row rule count";
            break;
        default:
            break;
        }
    }
    for (size_t i = 1; i < count; i++) {
        if (references[i] != 1) return references[i] ? "node referenced twice" : "node not referenced";
    }
    return std::string();
}

// ---- readable shapes for assertions: Kind:Class{child list}{child list}... ------------------
void AppendShape(const MathParseResult& result, uint16_t head, std::string& out, int depth = 0) {
    bool first = true;
    for (uint16_t i = head; i != 0 && i < result.nodes.size() && depth < 100; i = result.nodes[i].next) {
        const MathNode& node = result.nodes[i];
        if (!first) out += ' ';
        first = false;
        out += KindName(node.kind);
        if (node.cls != MathClass::None) {
            out += ':';
            out += ClassName(node.cls);
        }
        int last = 2;
        while (last >= 0 && !node.child[last]) last--;
        for (int c = 0; c <= last; c++) {
            out += '{';
            AppendShape(result, node.child[c], out, depth + 1);
            out += '}';
        }
    }
}

std::string Shape(std::string_view tex, bool display = true, bool fallback = false) {
    const MathParseResult result = Parse(tex, display, fallback);
    std::string out;
    AppendShape(result, result.root, out);
    return out;
}

// Classes of the items of the top-level list ("-" for an item that is not an atom).
std::string Classes(std::string_view tex) {
    const MathParseResult result = Parse(tex);
    std::string out;
    for (uint16_t i = result.root; i != 0; i = result.nodes[i].next) {
        if (!out.empty()) out += ' ';
        out += result.nodes[i].cls == MathClass::None ? "-" : ClassName(result.nodes[i].cls);
    }
    return out;
}

std::string RunText(const MathParseResult& result, uint16_t node) {
    return result.text.substr(result.nodes[node].a, result.nodes[node].b);
}

bool Expect(bool condition, const char* what, std::string_view tex = std::string_view()) {
    if (!condition) std::cerr << "math parser: " << what << (tex.empty() ? "" : ": ") << tex << std::endl;
    return condition;
}

bool ExpectEqual(const std::string& actual, const char* expected, const char* what, std::string_view tex) {
    if (actual == expected) return true;
    std::cerr << "math parser: " << what << ": " << tex << "\n  expected " << expected << "\n  actual   " << actual
              << std::endl;
    return false;
}

std::string Repeat(std::string_view piece, size_t count) {
    std::string out;
    out.reserve(piece.size() * count);
    for (size_t i = 0; i < count; i++) out += piece;
    return out;
}

// ---- the pinned corpus ----------------------------------------------------------------------
// 90 formulas from school algebra to machine-learning papers and the recovery table of the
// design, each with the FNV-1a 64-bit hash of its canonical arena text (ArenaText). The hashes
// were recorded when the parser agreed byte for byte with the reference parser of the design
// work, which is not part of the repository; a hash that changes means a tree changed.
struct Pinned {
    bool display;
    bool fallback;
    const char* tex;
    uint64_t hash;
};
const Pinned kPinned[] = {
    {1, 0, "x = \\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}", 0x05844fd2053de3c1ull},
    {0, 0, "(a+b)^2 = a^2 + 2ab + b^2", 0x9b141924f6418bcaull},
    {0, 0, "\\sqrt[3]{27} = 3", 0xd7d3d7bb08af481bull},
    {0, 0, "|x - 3| \\leq 5 \\iff -2 \\le x \\le 8", 0x1d674e150695c7b0ull},
    {0, 0, "\\frac{x^2 - 1}{x - 1} = x + 1, \\quad x \\neq 1", 0x4fc0d40428dd6a4dull},
    {0, 0, "\\log_a (xy) = \\log_a x + \\log_a y", 0x36fb0cf7888a5694ull},
    {0, 0, "\\sin^2\\alpha + \\cos^2\\alpha = 1", 0xa5b4d42340e42212ull},
    {0, 0, "\\pi \\approx 3,14 \\text{ oraz } 1{,}5 \\cdot 2 = 3", 0x22b414abbea883b5ull},
    {0, 0, "\\frac{\\cancel{(x-1)}(x+1)}{\\cancel{(x-1)}} = x+1", 0xc0f235deeb1eb448ull},
    {0, 0, "\\angle ABC = 90^\\circ,\\ \\triangle ABC \\sim \\triangle DEF,\\ AB \\parallel CD,\\ AB \\perp EF", 0x518ecb2135105e28ull},
    {0, 0, "A \\cup B = \\{x : x \\in A \\lor x \\in B\\},\\quad A \\cap B = \\emptyset,\\quad A \\setminus B \\subseteq A", 0xdfc12a1a7d407ff6ull},
    {0, 0, "x_{1,2} = \\mp\\sqrt{2},\\quad a_n = a_1 q^{n-1},\\quad n! = 1 \\cdot 2 \\cdots n", 0x21ff98ad75ff0d47ull},
    {1, 0, "\\lim_{x \\to 0} \\frac{\\sin x}{x} = 1", 0x24d69d9f372ad6e3ull},
    {1, 0, "f'(x) = \\lim_{h \\to 0} \\frac{f(x+h) - f(x)}{h}", 0x7f1b1b94a33795a0ull},
    {1, 0, "\\int_{-\\infty}^{\\infty} e^{-x^2}\\,dx = \\sqrt{\\pi}", 0xb2bd8322ad82e2bcull},
    {0, 0, "\\frac{d}{dx}\\left( x^n \\right) = n x^{n-1}", 0x6ec459f9eff07404ull},
    {1, 0, "\\frac{\\partial^2 u}{\\partial t^2} = c^2 \\nabla^2 u", 0x0264b082b2f4d83aull},
    {1, 0, "\\sum_{n=1}^{\\infty} \\frac{1}{n^2} = \\frac{\\pi^2}{6}", 0xc21487c42a71522bull},
    {0, 0, "e^{i\\pi} + 1 = 0", 0x816fc9b3118c4b11ull},
    {1, 0, "\\oint_C \\mathbf{F} \\cdot d\\mathbf{r} = \\iint_S (\\nabla \\times \\mathbf{F}) \\cdot d\\mathbf{S}", 0x8db95844acede5cdull},
    {1, 0, "f(x) = \\sum_{n=0}^\\infty \\frac{f^{(n)}(a)}{n!}(x-a)^n", 0x9b52ab27f7da7b61ull},
    {1, 0, "\\left. \\frac{dy}{dx} \\right|_{x=0} = \\Big[ F(x) \\Big]_a^b = \\int_a^b f(x)\\,\\mathrm{d}x", 0x4156a476abb849beull},
    {1, 0, "\\lim_{n\\to\\infty}\\left(1+\\frac1n\\right)^n = e", 0xcd66621eaea3cd90ull},
    {1, 0, "\\cfrac{1}{1+\\cfrac{1}{1+\\cfrac{1}{x}}}", 0xa528de2d41e86c85ull},
    {1, 0, "A = \\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}, \\quad \\det A = ad - bc", 0x9bc67d32087b6711ull},
    {1, 0, "I_n = \\begin{bmatrix} 1 & 0 & \\cdots & 0 \\\\ 0 & 1 & \\cdots & 0 \\\\ \\vdots & \\vdots & \\ddots & \\vdots \\\\ 0 & 0 & \\cdots & 1 \\end{bmatrix}", 0x14b01de1935d9d83ull},
    {0, 0, "\\mathbf{A}\\mathbf{x} = \\lambda \\mathbf{x}", 0xdecde3f2b04b4baaull},
    {0, 0, "\\|x\\|_2 = \\sqrt{\\sum_{i=1}^n x_i^2}", 0xbb3669a03bf6f390ull},
    {1, 0, "\\left[\\begin{array}{cc|c} 1 & 2 & 3 \\\\ 4 & 5 & 6 \\end{array}\\right]", 0x4b70dee1380f6c3eull},
    {0, 0, "A^{-1} = \\frac{1}{\\det A} \\operatorname{adj}(A),\\quad (AB)^\\top = B^\\top A^\\top", 0xd78b080ae22ef8e1ull},
    {1, 0, "\\begin{vmatrix} a & b \\\\ c & d \\end{vmatrix} = ad - bc", 0xb3d3f42534091447ull},
    {0, 0, "\\langle u, v \\rangle = \\sum_i u_i \\overline{v_i},\\quad \\vec{a} \\cdot \\vec{b} = |\\vec a||\\vec b|\\cos\\theta", 0x2cc6a2e0949ddff3ull},
    {0, 0, "A \\in \\mathbb{R}^{m \\times n},\\ \\operatorname{rank}(A) + \\dim\\ker A = n", 0x8d5fc1388f43af04ull},
    {1, 0, "P(A \\mid B) = \\frac{P(B \\mid A)\\,P(A)}{P(B)}", 0x6bc12d4e7b68a479ull},
    {0, 0, "\\operatorname{Var}(X) = \\mathbb{E}[X^2] - (\\mathbb{E}[X])^2", 0xaddefea1383b441eull},
    {0, 0, "X \\sim \\mathcal{N}(\\mu, \\sigma^2)", 0x9844636152206585ull},
    {1, 0, "f(x) = \\frac{1}{\\sigma\\sqrt{2\\pi}} \\exp\\left( -\\frac{(x-\\mu)^2}{2\\sigma^2} \\right)", 0xb533593f3ad2b2faull},
    {0, 0, "\\bar{x} = \\frac{1}{n}\\sum_{i=1}^{n} x_i,\\quad \\hat{\\beta} = (X^\\top X)^{-1} X^\\top y", 0x74882bcdb1300812ull},
    {0, 0, "\\binom{n}{k} = \\frac{n!}{k!(n-k)!}", 0x781f17ed6767513cull},
    {1, 0, "\\Pr\\left( |\\bar X_n - \\mu| \\geq \\varepsilon \\right) \\leq \\frac{\\sigma^2}{n\\varepsilon^2}", 0xaced1df2f5a0bf03ull},
    {0, 0, "E = mc^2", 0xb50b2962dd793017ull},
    {1, 0, "F = G\\frac{m_1 m_2}{r^2}", 0xc7216a8a53ab2d7full},
    {1, 0, "i\\hbar\\frac{\\partial}{\\partial t}\\Psi(\\mathbf{r},t) = \\hat{H}\\Psi(\\mathbf{r},t)", 0x00f65a90b3fa488bull},
    {1, 0, "\\nabla \\times \\mathbf{B} = \\mu_0\\mathbf{J} + \\mu_0\\varepsilon_0\\frac{\\partial \\mathbf{E}}{\\partial t}", 0x566a3bc225c91090ull},
    {0, 0, "\\Delta x\\,\\Delta p \\geq \\frac{\\hbar}{2}", 0x6a4ebbafadbab55eull},
    {0, 0, "{}^{14}_{6}\\mathrm{C} \\rightarrow {}^{14}_{7}\\mathrm{N} + e^- + \\bar{\\nu}_e", 0xfb56790bd03d6adaull},
    {0, 0, "c \\approx 3 \\times 10^8\\ \\mathrm{m/s},\\quad g = 9.81\\ \\text{m/s}^2", 0x4db33e231b33be98ull},
    {0, 0, "\\ddot{x} + 2\\zeta\\omega_0\\dot{x} + \\omega_0^2 x = 0", 0xcfa253bb4d226de1ull},
    {0, 0, "2H_2 + O_2 \\rightleftharpoons 2H_2O,\\quad \\mathrm{CH_4 + 2O_2 \\longrightarrow CO_2 + 2H_2O}", 0xa0aae0f238751916ull},
    {0, 0, "T(n) = 2T\\left(\\frac{n}{2}\\right) + O(n) = \\Theta(n \\log n)", 0xfb58776aba39409bull},
    {0, 0, "\\mathsf{P} \\neq \\mathsf{NP}", 0x46fe74ce1373dc23ull},
    {0, 0, "f(n) = \\mathcal{O}(g(n)) \\iff \\exists c > 0\\ \\exists n_0\\ \\forall n \\geq n_0 : f(n) \\leq c \\cdot g(n)", 0xb1579c8dde6e7a12ull},
    {0, 0, "\\lfloor x \\rfloor \\leq x < \\lfloor x \\rfloor + 1,\\quad \\lceil \\log_2 n \\rceil", 0xe4ef0c20ced85330ull},
    {0, 0, "a \\equiv b \\pmod{n},\\quad r = a \\bmod n,\\quad \\gcd(a, b) = 1", 0x8f1ba4cd66a5acefull},
    {1, 0, "\\frac{\\Gamma \\vdash e_1 : \\tau_1 \\to \\tau_2 \\quad \\Gamma \\vdash e_2 : \\tau_1}{\\Gamma \\vdash e_1\\,e_2 : \\tau_2}", 0x64a6a2220079f050ull},
    {0, 0, "\\forall x \\in S,\\ \\neg P(x) \\lor Q(x) \\implies R(x)", 0x361cc37116b63850ull},
    {0, 0, "A \\subseteq B \\land B \\subseteq A \\Rightarrow A = B,\\quad p \\not\\equiv q,\\quad x \\notin A", 0x27bcc00e5bab0bbdull},
    {0, 0, "x \\oplus y,\\quad \\bigoplus_{i=1}^n x_i,\\quad k \\ll n,\\quad x \\mapsto x^2,\\quad f \\circ g,\\quad f\\colon A \\to B", 0x2b590303b9b95fdeull},
    {1, 0, "\\mathcal{L}(\\theta) = -\\frac{1}{N}\\sum_{i=1}^{N} \\left[ y_i \\log \\hat{y}_i + (1-y_i)\\log(1-\\hat{y}_i) \\right]", 0x441a23292cbff433ull},
    {0, 0, "\\theta_{t+1} = \\theta_t - \\eta \\nabla_\\theta \\mathcal{L}(\\theta_t)", 0xa28736ad4a906cecull},
    {1, 0, "\\operatorname{softmax}(z)_i = \\frac{e^{z_i}}{\\sum_{j=1}^{K} e^{z_j}}", 0x39be88638b88e6afull},
    {1, 0, "\\mathrm{Attention}(Q, K, V) = \\mathrm{softmax}\\left(\\frac{QK^T}{\\sqrt{d_k}}\\right)V", 0x9a3e11fb94cd4c93ull},
    {1, 0, "\\theta^* = \\arg\\max_\\theta \\mathbb{E}_{x \\sim p_{\\text{data}}} \\left[ \\log p_\\theta(x) \\right] = \\underset{\\theta}{\\operatorname{argmax}}\\; J(\\theta)", 0xdf4f2db7bd7ae20dull},
    {1, 0, "D_{\\mathrm{KL}}(P \\,\\|\\, Q) = \\sum_{x} P(x) \\log \\frac{P(x)}{Q(x)}", 0xc3f1c9d3460161e1ull},
    {0, 0, "\\boldsymbol{h}_t = \\tanh(\\boldsymbol{W}\\boldsymbol{x}_t + \\boldsymbol{U}\\boldsymbol{h}_{t-1} + \\boldsymbol{b}),\\quad c_t = f_t \\odot c_{t-1} + i_t \\odot \\tilde{c}_t", 0x55f85f854835ac0full},
    {1, 0, "\\min_G \\max_D V(D, G) = \\mathbb{E}_{x \\sim p_{\\text{data}}(x)}[\\log D(x)] + \\mathbb{E}_{z \\sim p_z(z)}[\\log(1 - D(G(z)))]", 0x91383e778d2eb9ffull},
    {0, 0, "\\lVert w \\rVert_2^2 + \\lambda\\|w\\|_1,\\quad \\mathbb{1}[x > 0],\\quad \\mathbf{1}_{\\{y_i = k\\}}", 0xee3d7d137a87da02ull},
    {1, 0, "\\underbrace{\\sum_i (y_i - \\hat y_i)^2}_{\\text{data fit}} + \\lambda \\underbrace{\\|w\\|^2}_{\\text{regulariser}}", 0xc1c728a0efae120bull},
    {1, 0, "\\begin{aligned}\012\\nabla_w L &= \\frac{\\partial}{\\partial w} \\frac{1}{2}\\|Xw - y\\|^2 \\\\\012&= X^\\top (Xw - y) \\\\\012&+ \\lambda w\012\\end{aligned}", 0xad098dd18386173eull},
    {1, 0, "\\mathrm{ReLU}(x) = \\begin{cases} x & \\text{if } x > 0 \\\\ 0 & \\text{otherwise} \\end{cases}", 0x209227a39223954aull},
    {1, 0, "\\sum_{\\substack{i=1 \\\\ i \\neq j}}^{n} a_{ij} \\qquad \\prod_{p \\text{ prime}} \\frac{1}{1 - p^{-s}}", 0x13507c0fc95e0d79ull},
    {0, 0, "x \\xrightarrow{f} y \\overset{\\text{def}}{=} g(x),\\quad a \\stackrel{\?}{=} b,\\quad a \\triangleq b", 0xf9c73ca5ee9bb4e3ull},
    {1, 0, "\\boxed{E = mc^2} \\tag{1}", 0xdf02ece822f1a118ull},
    {1, 0, "v = \\frac{s}{t} \\quad \\text{gdzie $s$ to droga, a $t$ \342\200\224 czas (je\305\233li } t \\neq 0\\text{)}", 0xf68ebe74f09bbe95ull},
    {1, 1, "v = \\frac{s}{t} \\quad \\text{gdzie $s$ to droga, a $t$ \342\200\224 czas (je\305\233li } t \\neq 0\\text{)}", 0x9bf2727c13dcd623ull},
    {0, 0, "\316\261 + \316\262 \342\211\244 \316\263 \342\206\222 \342\210\236,  x\302\262 + y\302\262 = r\302\262,  \342\210\221\341\265\242 x\341\265\242 \342\211\245 \342\210\2322 \342\211\210 1.414,  \342\204\235\342\201\277", 0xe175e4339172f7e9ull},
    {0, 1, "\316\261 + \316\262 \342\211\244 \316\263 \342\206\222 \342\210\236,  x\302\262 + y\302\262 = r\302\262,  \342\210\221\341\265\242 x\341\265\242 \342\211\245 \342\210\2322 \342\211\210 1.414,  \342\204\235\342\201\277", 0xe175e4339172f7e9ull},
    {0, 0, "\\left\\{ x \\in \\mathbb{R} \\,\\middle|\\, x^2 < 2 \\right\\} \\subsetneq \\mathbb{Q}^c", 0xa384da248e71ef6cull},
    {0, 0, "{n \\choose k} = {n! \\over k!(n-k)!},\\quad {\\bf v} = {\\rm const}", 0x0fa499e4d33039b7ull},
    {0, 0, "\\tilde{O}(n^{2.373}),\\ \\widehat{AB},\\ \\overline{z_1 z_2},\\ \\overrightarrow{AB},\\ \\dot{q},\\ \\vec{v}\\,", 0xec921504f992869dull},
    {0, 0, "\\int\\!\\!\\!\\int_D f\\,dA \\;=\\; \\iint_D f \\, dx\\,dy \\quad\\text{and}\\quad a\\hspace{1em}b", 0x891e835473f77e9full},
    {1, 0, "a &= b + c \\\\\012  &= d", 0x7a788ae01c09a731ull},
    {1, 0, "\\begin{equation}\012  \\hat{y} = \\sigma(Wx + b) \\label{eq:fwd}\012\\end{equation}", 0x1e1f590eae1602d7ull},
    {0, 0, "\\langle \\psi | \\hat{H} | \\psi \\rangle = \\braket{\\psi | \\hat H | \\psi},\\quad \\ket{0} \\otimes \\ket{1},\\quad A^\\dagger", 0x0e030e134874bafeull},
    {1, 0, "\\hat{w} = \\operatorname*{arg\\,min}_{w \\in \\mathbb{R}^d} \\; \\frac1n \\sum_{i=1}^n \\ell(w; x_i, y_i) + \\lambda \\norm{w}_2^2", 0x46e0ac5f930bbe6cull},
    {1, 0, "\\begin{bmatrix*}[r] 1 & -2 \\\\ -3 & 4 \\end{bmatrix*} \\quad \\begin{alignedat}{2} x &= 1 &\\quad y &= 2 \\end{alignedat}", 0x0e52261caee55057ull},
    {0, 0, "1, 2, \\dots, n \\quad a_1 + \\dots + a_n \\quad \\big( x \\big) \\Bigl\\{ y \\Bigr\\} \\quad 1{,}000{,}000 \\ne 1,000,000", 0xda165a47cc13b127ull},
    {0, 0, "\\overbrace{a + b}^{2} \\xrightarrow[\\text{below}]{\\text{above}} c \\not\\subset d \\nleq e \\in \\R^n,\\ \\N \\subset \\Z", 0x44401a72f9d8d5dbull},
    {0, 0, "\\mathfrak{g} \\hookrightarrow \\mathfrak{gl}(V),\\quad A \\succeq 0,\\quad \\ell_1", 0x13569289292f00aeull},
    {0, 0, "\\color{red} x + \\textcolor{blue}{y} + \\phantom{z} + \\mathclap{w}", 0x1e7fdde2e820cef3ull},
    {0, 0, "\\E[X] + \\norm{x} + \\xRightarrow{a} + \\sgn(x) + \\tg x", 0x94beb1bb7262ea03ull},
    {1, 0, "\\begin{CD} A @>>> B \\end{CD} \\quad \\begin{tabular}{ll} a & b \\\\ c & d \\end{tabular}", 0x82c5551132711dbdull},
    {1, 0, "\\frac{a+b}{c", 0x0ea36d60d16d8292ull},
    {1, 0, "x + {y + z", 0x34b9fcca108d3721ull},
    {1, 0, "a + b} + c", 0xa2e43d0e937d2d13ull},
    {1, 0, "x^", 0xb2cd6df5e6bfc977ull},
    {1, 0, "{x^} + 1", 0xcecb801a26f20df6ull},
    {1, 0, "x^^2", 0x4aeb9054e8e5deffull},
    {1, 0, "^2 + _i", 0xaca37ff596505875ull},
    {1, 0, "x^2^3", 0x45d8c787d2331a9full},
    {1, 0, "x_i_j", 0x2158ea670d738685ull},
    {1, 0, "x^2'", 0x3187fb4b97851e6cull},
    {1, 0, "x'^2 + f''(x) + f^{\\prime\\prime} + g^\\prime", 0x87813c9ad757cfecull},
    {1, 0, "\\frac{a}", 0x63d30cedd0f19f55ull},
    {1, 0, "\\frac", 0xa2da5f68cf738be3ull},
    {1, 0, "\\sqrt[3", 0x7d400094a5229f63ull},
    {1, 0, "a + b \\right) + c", 0x8abd8b03c977400aull},
    {1, 0, "\\left( a + b", 0xb3d3e6f0b006ca04ull},
    {1, 0, "\\left a + b \\right)", 0x9fc4c02969e09bc5ull},
    {1, 0, "a \\middle| b", 0x13d534e61ddaee7dull},
    {1, 0, "{ \\left( a + b } \\right)", 0xd3fbc2f2eaa60cacull},
    {1, 0, "\\left( {a + b \\right)", 0x3a6c8f934a369c2bull},
    {1, 0, "\\begin{matrix} \\left( a & b \\right) \\end{matrix}", 0x54de68f02bb26d10ull},
    {1, 0, "\\begin{pmatrix} a & {b \\\\ c & d \\end{pmatrix}", 0xb99f5846f36a88bfull},
    {1, 0, "\\begin{foo} a & b \\\\ c & d \\end{foo}", 0x1990a8bb37758be4ull},
    {1, 0, "\\begin{matrix} a & b \\\\ c & d", 0x1990a8bb37758be4ull},
    {1, 0, "a + b \\end{matrix} + c", 0xa2e43d0e937d2d13ull},
    {1, 0, "\\begin{pmatrix} a \\begin{cases} b \\end{pmatrix} c", 0x12b80855d70146ceull},
    {1, 0, "\\begin x", 0x76ecb74cc1b05baaull},
    {1, 0, "a & b", 0xed531aa9e39a0735ull},
    {1, 0, "a \\\\ b", 0x9c0a771f75988e58ull},
    {1, 0, "\\frac{a & b}{c}", 0x0246034d44782daeull},
    {1, 0, "\\foo{x} + \\bar", 0x6b7c6aa7e0750f0dull},
    {1, 0, "a \\\? b", 0x4c4599aaf1d759e4ull},
    {1, 0, "x + \\", 0xbe6474bfcbb88215ull},
    {1, 0, "\\begin{cases} 1 & \\text{if x \\\\ 0 & \\text{else} \\end{cases}", 0xe53ae1be21af8908ull},
    {1, 0, "\\text{for $x > 0}", 0xcf4c82e96325fbaaull},
    {1, 0, "x\\limits_a", 0x7f2b6ff28c9c807dull},
    {1, 0, "a \\over b \\over c", 0xab10cf368390df10ull},
    {1, 0, "\\big x", 0xb2cd6df5e6bfc977ull},
    {1, 0, "x \\tag{1} \\tag{2}", 0x8c8a0aed4b392191ull},
    {1, 0, "a\\hspace{1qq}b\\kern3ptc\\\\[2pt]d\\\\[x]", 0xe40e947b4ca8f6faull},
    {1, 0, "a # b $ c", 0x9baf2ecb34d2395eull},
    {1, 0, "50% + 25\\%", 0x8c3037670c619101ull},
    {1, 0, "a + b % note\012 + c", 0xa2e43d0e937d2d13ull},
    {1, 0, "a \\not", 0x04c6f47a4e305b26ull},
    {1, 0, "\\begin{bmatrix} 1 & 2 \\\\ 3 & 4 \\\\ \\end{bmatrix}", 0x07efdc20eea42608ull},
    {1, 0, "\\begin{array}{|c|c|} \\hline a & b \\\\ \\hline c & d \\\\ \\hline \\end{array}", 0x924186c8bbe4c9a4ull},
    {1, 0, "f''''''''''''", 0x1e6eeb76cddc911bull},
    {1, 0, "", 0x8b57e7b3f634a19eull},
    {1, 0, "   \012  ", 0x8b57e7b3f634a19eull},
    {1, 0, "x + \321\217 + \344\270\255", 0x03c171d657d63acbull},
    {1, 1, "x + \321\217 + \344\270\255", 0xc33f094cabd6e234ull},
};

// Rows of the recovery table by node kinds and classes (display style, no fallback font).
struct ShapeRow {
    const char* tex;
    const char* shape;
};
const ShapeRow kRecoveryShapes[] = {
    {"\\frac{a+b}{c", "Fraction:Ord{Glyph:Ord Glyph:Bin Glyph:Ord}{Glyph:Ord}"},
    {"x + {y + z", "Glyph:Ord Glyph:Bin Group:Ord{Glyph:Ord Glyph:Bin Glyph:Ord}"},
    {"a + b} + c", "Glyph:Ord Glyph:Bin Glyph:Ord Glyph:Bin Glyph:Ord"},
    {"x^", "Glyph:Ord"},
    {"{x^} + 1", "Group:Ord{Glyph:Ord} Glyph:Bin Glyph:Ord"},
    {"x^^2", "Scripts:Ord{Glyph:Ord}{}{Glyph:Ord}"},
    {"^2 + _i", "Scripts:Ord{Group:Ord}{}{Glyph:Ord} Scripts:Ord{Glyph:Ord}{Glyph:Ord}"},
    {"x^2^3", "Scripts:Ord{Glyph:Ord}{}{Glyph:Ord} Scripts:Ord{Group:Ord}{}{Glyph:Ord}"},
    {"x_i_j", "Scripts:Ord{Glyph:Ord}{Glyph:Ord} Scripts:Ord{Group:Ord}{Glyph:Ord}"},
    {"x^2'", "Scripts:Ord{Glyph:Ord}{}{Glyph:Ord}"},
    {"\\frac{a}", "Fraction:Ord{Glyph:Ord}"},
    {"\\frac", "Fraction:Ord"},
    {"\\sqrt[3", "Radical:Ord{}{Glyph:Ord}"},
    {"a + b \\right) + c", "Glyph:Ord Glyph:Bin Glyph:Ord Glyph:Close Glyph:Bin Glyph:Ord"},
    {"\\left( a + b", "LeftRight:Inner{Glyph:Ord Glyph:Bin Glyph:Ord}"},
    {"\\left a + b \\right)", "LeftRight:Inner{Glyph:Ord Glyph:Bin Glyph:Ord}"},
    {"a \\middle| b", "Glyph:Ord Glyph:Ord Glyph:Ord"},
    {"{ \\left( a + b } \\right)", "Group:Ord{LeftRight:Inner{Glyph:Ord Glyph:Bin Glyph:Ord}} Glyph:Close"},
    {"\\left( {a + b \\right)", "LeftRight:Inner{Group:Ord{Glyph:Ord Glyph:Bin Glyph:Ord}}"},
    {"\\begin{matrix} \\left( a & b \\right) \\end{matrix}", "Array:Ord{Row{Cell{LeftRight:Inner{Glyph:Ord}} Cell{Glyph:Ord Glyph:Close}}}{Col Col}"},
    {"\\begin{pmatrix} a & {b \\\\ c & d \\end{pmatrix}", "Array:Inner{Row{Cell{Glyph:Ord} Cell{Group:Ord{Glyph:Ord}}} Row{Cell{Glyph:Ord} Cell{Glyph:Ord}}}{Col Col}"},
    {"\\begin{foo} a & b \\\\ c & d \\end{foo}", "Array:Ord{Row{Cell{Glyph:Ord} Cell{Glyph:Ord}} Row{Cell{Glyph:Ord} Cell{Glyph:Ord}}}{Col Col}"},
    {"\\begin{matrix} a & b \\\\ c & d", "Array:Ord{Row{Cell{Glyph:Ord} Cell{Glyph:Ord}} Row{Cell{Glyph:Ord} Cell{Glyph:Ord}}}{Col Col}"},
    {"a + b \\end{matrix} + c", "Glyph:Ord Glyph:Bin Glyph:Ord Glyph:Bin Glyph:Ord"},
    {"\\begin{pmatrix} a \\begin{cases} b \\end{pmatrix} c", "Array:Inner{Row{Cell{Glyph:Ord Array:Inner{Row{Cell{Glyph:Ord}}}{Col}}}}{Col} Glyph:Ord"},
    {"\\begin x", "OpName:Op{TextRun} Glyph:Ord"},
    {"a & b", "Array:Ord{Row{Cell{Glyph:Ord} Cell{Group:Ord Glyph:Ord}}}{Col Col}"},
    {"a \\\\ b", "Array:Ord{Row{Cell{Glyph:Ord}} Row{Cell{Glyph:Ord}}}{Col}"},
    {"\\frac{a & b}{c}", "Fraction:Ord{Glyph:Ord Glyph:Ord}{Glyph:Ord}"},
    {"\\foo{x} + \\bar", "OpName:Op{TextRun} Group:Ord{Glyph:Ord} Glyph:Bin Accent:Ord"},
    {"a \\\? b", "Glyph:Ord Glyph:Close Glyph:Ord"},
    {"x + \\", "Glyph:Ord Glyph:Ord"},
    {"\\begin{cases} 1 & \\text{if x \\\\ 0 & \\text{else} \\end{cases}", "Array:Inner{Row{Cell{Glyph:Ord} Cell{Text:Ord{TextRun}}} Row{Cell{Glyph:Ord} Cell{Text:Ord{TextRun}}}}{Col Col}"},
    {"\\text{for $x > 0}", "Text:Ord{TextRun Group:Ord{Glyph:Ord Glyph:Rel Glyph:Ord}}"},
    {"x\\limits_a", "Scripts:Ord{Glyph:Ord}{Glyph:Ord}"},
    {"a \\over b \\over c", "Fraction:Ord{Glyph:Ord}{Glyph:Ord Glyph:Ord}"},
    {"\\big x", "Glyph:Ord"},
    {"a\\hspace{1qq}b\\kern3ptc\\\\[2pt]d\\\\[x]", "Array:Ord{Row{Cell{Glyph:Ord Glyph:Ord Space Glyph:Ord}} Row{Cell{Glyph:Ord}} Row{Cell{Glyph:Open Glyph:Ord Glyph:Close}}}{Col}"},
    {"a # b $ c", "Glyph:Ord Glyph:Ord Glyph:Ord Glyph:Ord Glyph:Ord"},
    {"a \\not", "Glyph:Ord"},
    {"\\begin{bmatrix} 1 & 2 \\\\ 3 & 4 \\\\ \\end{bmatrix}", "Array:Inner{Row{Cell{Glyph:Ord} Cell{Glyph:Ord}} Row{Cell{Glyph:Ord} Cell{Glyph:Ord}}}{Col Col}"},
    {"f''''''''''''", "Scripts:Ord{Glyph:Ord}"},
    {"   \012  ", ""},
    {"x + \321\217 + \344\270\255", "Glyph:Ord Glyph:Bin Glyph:Ord Glyph:Bin Glyph:Ord"},
};

struct Piece {
    const char* data;
    size_t size;
};
// Tokens of the fuzz loop: syntax, commands, non-ASCII characters, raw control bytes and
// malformed UTF-8, the latter also directly after a backslash.
const Piece kFuzzAlphabet[] = {
    {"{", 1}, {"}", 1}, {"^", 1}, {"_", 1}, {"&", 1}, {"'", 1}, {"\\\\", 2}, {"\\left", 5}, {"\\right", 6},
    {"\\middle", 7}, {"(", 1}, {")", 1}, {"[", 1}, {"]", 1}, {"|", 1}, {".", 1}, {"\\frac", 5}, {"\\sqrt", 5},
    {"\\begin{matrix}", 14}, {"\\end{matrix}", 12}, {"\\begin{cases}", 13}, {"\\end{cases}", 11},
    {"\\begin{aligned}", 15}, {"\\end{aligned}", 13}, {"\\begin{array}{c|c}", 18}, {"\\end{array}", 11},
    {"\\begin{foo}", 11}, {"\\end{foo}", 9}, {"\\text{", 6}, {"\\text", 5}, {"$", 1}, {"\\over", 5},
    {"\\choose", 7}, {"\\limits", 7}, {"\\sum", 4}, {"\\int", 4}, {"x", 1}, {"y", 1}, {"1", 1}, {"2", 1}, {"+", 1},
    {"-", 1}, {"=", 1}, {",", 1}, {" ", 1}, {"\012", 1}, {"\\alpha", 6}, {"\\not", 4}, {"\\hat", 4}, {"\\big", 4},
    {"\\Bigl", 5}, {"\\tag", 4}, {"\\substack{", 10}, {"\\operatorname", 13}, {"\\operatorname*", 14},
    {"\\mathbf", 7}, {"\\bf", 3}, {"\\hspace{", 8}, {"1em}", 4}, {"\\kern", 5}, {"\\", 1}, {"%", 1}, {"#", 1},
    {"~", 1}, {"\\,", 2}, {"\\{", 2}, {"\\}", 2}, {"\\|", 2}, {"\304\205", 2}, {"\321\217", 2}, {"\302\262", 2},
    {"\342\202\201", 3}, {"\342\200\262", 3}, {"\342\210\232", 3}, {"\\xrightarrow", 12}, {"\\underbrace", 11},
    {"\\binom", 6}, {"\\hline", 6}, {"\\displaystyle", 13}, {"\\foo", 4}, {"\\begin", 6}, {"\\end", 4},
    {"\\boxed", 6}, {"\\overset", 8}, {"\\pmod", 5}, {"\\dots", 5}, {"\\left.", 6}, {"\\right.", 7},
    {"\\middle|", 8}, {"\\\\[2pt]", 7}, {"\\\\*", 3}, {"\\prime", 6}, {"\\color{red}", 11}, {"\\textbf{", 8},
    {"\\mathrm{", 8}, {"\\label{", 7}, {"\\phantom", 8}, {"\000", 1}, {"\001", 1}, {"\002", 1}, {"\013", 1},
    {"\037", 1}, {"\177", 1}, {"\200", 1}, {"\274", 1}, {"\300", 1}, {"\305", 1}, {"\342\202", 2},
    {"\355\240\200", 3}, {"\364\220\200\200", 4}, {"\377", 1}, {"\\\000", 2}, {"\\\001", 2}, {"\\\002", 2},
    {"\\\013", 2}, {"\\\037", 2}, {"\\\177", 2}, {"\\\200", 2}, {"\\\274", 2}, {"\\\300", 2}, {"\\\305", 2},
    {"\\\342\202", 3}, {"\\\355\240\200", 4}, {"\\\364\220\200\200", 5}, {"\\\377", 2},
};

bool CheckClasses() {
    struct Row { const char* tex; const char* classes; };
    const Row rows[] = {
        // Bin -> Ord (TeX Appendix G, rules 5 and 6)
        {"-x", "Ord Ord"}, {"a - b", "Ord Bin Ord"}, {"a = -b", "Ord Rel Ord Ord"}, {"(-b)", "Open Ord Ord Close"},
        {"a, -b", "Ord Punct Ord Ord"}, {"- - x", "Ord Bin Ord"}, {"a +", "Ord Ord"}, {"a + = b", "Ord Ord Rel Ord"},
        // a comma is always Punct; a decimal comma is written {,}
        {"3,14", "Ord Punct Ord Ord"}, {"3{,}14", "Ord Ord Ord Ord"}, {"(1,2)", "Open Ord Punct Ord Close"},
        // % is a percent sign in a one-line source and a comment in a multi-line one
        {"50% + 25\\%", "Ord Ord Ord Bin Ord Ord Ord"}, {"a + b % note\n + c", "Ord Bin Ord Bin Ord"},
        {"a + b % note", "Ord Bin Ord Ord Ord Ord Ord Ord"},
    };
    bool ok = true;
    for (const Row& row : rows) ok = ExpectEqual(Classes(row.tex), row.classes, "classes", row.tex) && ok;
    const ShapeRow shapes[] = {
        {"x^-", "Scripts:Ord{Glyph:Ord}{}{Glyph:Ord}"},
        {"\\left( -x \\middle| +y \\right)", "LeftRight:Inner{Glyph:Ord Glyph:Ord Middle Glyph:Ord Glyph:Ord}"},
        {"a &+ b", "Array:Ord{Row{Cell{Glyph:Ord} Cell{Group:Ord Glyph:Bin Glyph:Ord}}}{Col Col}"},
        {"3{,}14", "Glyph:Ord Group:Ord{Glyph:Punct} Glyph:Ord Glyph:Ord"},
    };
    for (const ShapeRow& row : shapes) ok = ExpectEqual(Shape(row.tex), row.shape, "shape", row.tex) && ok;
    for (const ShapeRow& row : kRecoveryShapes) ok = ExpectEqual(Shape(row.tex), row.shape, "recovery", row.tex) && ok;
    return ok;
}

bool CheckLimits() {
    struct Row { const char* name; std::string tex; bool fallback; };
    const Row rows[] = {
        {"23 nested groups", Repeat("{", 23) + "x" + Repeat("}", 23), false},
        {"24 nested groups", Repeat("{", 24) + "x" + Repeat("}", 24), true},
        {"8192 atoms", Repeat("x", 8192), false},
        {"64 rows", "\\begin{matrix}" + Repeat("a\\\\", 63) + "a\\end{matrix}", false},
        {"64 rows and a trailing separator", "\\begin{matrix}" + Repeat("a\\\\", 64) + "\\end{matrix}", false},
        {"24 columns", "\\begin{matrix}" + Repeat("a&", 23) + "a\\end{matrix}", false},
        {"25 columns", "\\begin{matrix}" + Repeat("a&", 24) + "a\\end{matrix}", true},
        {"16384 bytes", Repeat(" ", 16384), false},
        {"8193 atoms", Repeat("x", 8193), true},
        {"65 rows", "\\begin{matrix}" + Repeat("a\\\\", 65) + "\\end{matrix}", true},
        {"33 columns", "\\begin{matrix}" + Repeat("a&", 33) + "\\end{matrix}", true},
        {"16385 bytes", Repeat(" ", 16385), true},
        {"24-column specification", "\\begin{array}{" + Repeat("c", 24) + "} a \\end{array}", false},
        {"25-column specification", "\\begin{array}{" + Repeat("c", 25) + "} a \\end{array}", true},
    };
    bool ok = true;
    for (const Row& row : rows) {
        const MathParseResult result = Parse(row.tex, true);
        ok = Expect(result.sourceFallback == row.fallback, "limit outcome", row.name) && ok;
        const std::string defect = ArenaDefect(result);
        ok = Expect(defect.empty(), defect.c_str(), row.name) && ok;
        if (result.sourceFallback) {
            // The same arena as the public fallback builder makes: one Text atom.
            MathParseResult direct;
            BuildMathSourceFallback(row.tex, false, direct);
            ok = Expect(ArenaText(result) == ArenaText(direct) && result.root != 0 &&
                        result.nodes[result.root].kind == MathNodeKind::Text && result.nodes[result.root].next == 0,
                        "limit fallback arena", row.name) && ok;
        }
    }
    const MathParseResult wide = Parse(rows[12].tex, true);
    int columns = 0;
    int cells = 0;
    if (wide.root && wide.nodes[wide.root].kind == MathNodeKind::Array) {
        for (uint16_t i = wide.nodes[wide.root].child[1]; i; i = wide.nodes[i].next) columns++;
        const uint16_t row = wide.nodes[wide.root].child[0];
        for (uint16_t i = row ? wide.nodes[row].child[0] : 0; i; i = wide.nodes[i].next) cells++;
    }
    ok = Expect(columns == 24 && cells == 1, "24-column specification: 24 Col nodes and one cell") && ok;

    // The fallback text: white space collapsed, control bytes dropped, 200 bytes, then " ...".
    MathParseResult fallback;
    BuildMathSourceFallback("a \t\n b\x01" "c", false, fallback);
    ok = Expect(fallback.sourceFallback && fallback.text == "a bc", "source fallback text") && ok;
    BuildMathSourceFallback(Repeat("x", 8193), false, fallback);
    ok = Expect(fallback.text == Repeat("x", kMathFallbackTextBytes) + " ...", "source fallback cut") && ok;
    BuildMathSourceFallback(Repeat(" ", 16385), false, fallback);
    ok = Expect(fallback.text == " ...", "source fallback of white space") && ok;
    BuildMathSourceFallback(Repeat("x", 199) + "\xC5\xBC" "y", false, fallback);
    ok = Expect(fallback.text == Repeat("x", 199) + " ...", "source fallback cut on a character boundary") && ok;
    BuildMathSourceFallback("\xC5\xBC \xFF", true, fallback);
    ok = Expect(fallback.text == "\xC5\xBC \xEF\xBF\xBD" && ArenaDefect(fallback).empty(), "source fallback with a fallback font") && ok;
    return ok;
}

bool CheckTextAndBytes() {
    bool ok = true;
    // A word with a non-ASCII byte goes whole to the fallback font, or shows '?' per character.
    MathParseResult result = Parse("\\text{za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 x}", false, true);
    uint16_t text = result.root;
    uint16_t first = text ? result.nodes[text].child[0] : 0;
    uint16_t second = first ? result.nodes[first].next : 0;
    ok = Expect(text && result.nodes[text].kind == MathNodeKind::Text && first && second &&
                result.nodes[first].kind == MathNodeKind::Fallback && result.nodes[first].cls == MathClass::None &&
                RunText(result, first) == "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87" &&
                result.nodes[second].kind == MathNodeKind::TextRun && result.nodes[second].aux == 1 &&
                RunText(result, second) == " x" && result.nodes[second].next == 0,
                "text word with a fallback font") && ok;
    result = Parse("\\text{za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 x}", false, false);
    first = result.root ? result.nodes[result.root].child[0] : 0;
    ok = Expect(first && result.nodes[first].kind == MathNodeKind::TextRun && result.nodes[first].next == 0 &&
                RunText(result, first) == "za\?\?\?\? x", "text word without a fallback font") && ok;

    // Control bytes are dropped in every mode, also directly after a backslash.
    const std::string_view twoAtoms[] = {std::string_view("x\x01y", 3), std::string_view("x\\\x01y", 4),
                                         std::string_view("x\\\x7Fy", 4), std::string_view("x\0y", 3)};
    for (std::string_view tex : twoAtoms) {
        result = Parse(tex);
        const uint16_t y = result.root ? result.nodes[result.root].next : 0;
        ok = Expect(result.nodes.size() == 3 && y && result.nodes[result.root].a == 'x' && result.nodes[y].a == 'y' &&
                    result.nodes[y].kind == MathNodeKind::Glyph, "control byte in math mode") && ok;
    }
    result = Parse("\\text{a\x01" "b\x7F" "c}");
    first = result.root ? result.nodes[result.root].child[0] : 0;
    ok = Expect(first && result.nodes[first].kind == MathNodeKind::TextRun && result.nodes[first].next == 0 &&
                RunText(result, first) == "abc", "control bytes in text mode") && ok;

    // A byte that begins no well-formed UTF-8 sequence is U+FFFD: a Fallback atom, or '?'.
    result = Parse("a\xFF" "b", false, true);
    uint16_t middle = result.root ? result.nodes[result.root].next : 0;
    ok = Expect(result.nodes.size() == 4 && middle && result.nodes[middle].kind == MathNodeKind::Fallback &&
                result.nodes[middle].cls == MathClass::Ord && RunText(result, middle) == "\xEF\xBF\xBD" &&
                result.nodes[middle].next && result.nodes[result.nodes[middle].next].a == 'b',
                "malformed byte with a fallback font") && ok;
    result = Parse("a\xFF" "b", false, false);
    middle = result.root ? result.nodes[result.root].next : 0;
    ok = Expect(result.nodes.size() == 4 && middle && result.nodes[middle].kind == MathNodeKind::Glyph &&
                result.nodes[middle].aux == 1 && result.nodes[middle].a == '?' && result.text.empty(),
                "malformed byte without a fallback font") && ok;
    result = Parse("\xED\xA0\x80\xF4\x90\x80\x80", false, true);     // a surrogate, a value above U+10FFFF
    ok = Expect(result.nodes.size() == 8 && result.text == Repeat("\xEF\xBF\xBD", 7) && ArenaDefect(result).empty(),
                "one U+FFFD per malformed byte") && ok;
    return ok;
}

bool CheckConstructs() {
    bool ok = true;
    // \mathop{..} and \operatorname*{..} take their scripts as limits in display style.
    for (const char* tex : {"\\mathop{\\mathrm{argmin}}_x", "\\operatorname*{argmin}_x"}) {
        const MathParseResult result = Parse(tex, true);
        const uint16_t scripts = result.root;
        const uint16_t nucleus = scripts ? result.nodes[scripts].child[0] : 0;
        ok = Expect(scripts && nucleus && result.nodes[scripts].kind == MathNodeKind::Scripts &&
                    result.nodes[scripts].cls == MathClass::Op && (result.nodes[scripts].flags & kOpDisplayLimits) &&
                    result.nodes[nucleus].cls == MathClass::Op && (result.nodes[nucleus].flags & kOpDisplayLimits) &&
                    result.nodes[scripts].child[1] && !result.nodes[scripts].child[2],
                    "display limits", tex) && ok;
    }
    MathParseResult result = Parse("\\mathop{\\mathrm{argmin}}_x", true);
    ok = Expect(result.root && result.nodes[result.nodes[result.root].child[0]].kind == MathNodeKind::Group, "\\mathop is a Group") && ok;
    result = Parse("\\operatorname*{argmin}_x", true);
    ok = Expect(result.root && result.nodes[result.nodes[result.root].child[0]].kind == MathNodeKind::OpName, "\\operatorname* is an OpName") && ok;
    result = Parse("\\sum\\nolimits_a \\int\\limits", true);
    ok = Expect(result.root && result.nodes[result.root].a == kLimitsOff && result.nodes[result.root].next &&
                result.nodes[result.nodes[result.root].next].a == kLimitsOn, "limits mode") && ok;

    // The gap between the column pairs of the aligned family; alignat inserts none.
    struct Gap { const char* tex; int gap; };
    for (const Gap& row : {Gap{"\\begin{align} a &= b \\end{align}", 1000},
                           Gap{"\\begin{alignat}{2} a &= b \\end{alignat}", 0}, Gap{"a &= b \\\\ c &= d", 1000}}) {
        result = Parse(row.tex, true);
        ok = Expect(result.root && result.nodes[result.root].kind == MathNodeKind::Array &&
                    result.nodes[result.root].b == row.gap &&
                    result.nodes[result.root].aux == (kArrayRowGap | kArrayAligned), "aligned column gap", row.tex) && ok;
    }

    // Tags: the row tag of a one-row array is the formula tag; other rows keep theirs.
    result = Parse("\\begin{equation} x \\tag{1} \\end{equation}", true);
    uint16_t row = result.root ? result.nodes[result.root].child[0] : 0;
    ok = Expect(result.tag && result.tagParens && result.nodes[result.tag].kind == MathNodeKind::TextRun &&
                RunText(result, result.tag) == "1" && row && !result.nodes[row].child[1], "formula tag of equation") && ok;
    result = Parse("E = mc^2 \\tag*{A} \\tag{2}", true);
    ok = Expect(result.tag && !result.tagParens && RunText(result, result.tag) == "A" && ArenaDefect(result).empty(),
                "the first tag wins; \\tag* has no parentheses") && ok;
    result = Parse("\\begin{align} a &= b \\tag{1} \\\\ c &= d \\tag*{x} \\end{align}", true);
    row = result.root ? result.nodes[result.root].child[0] : 0;
    const uint16_t secondRow = row ? result.nodes[row].next : 0;
    ok = Expect(!result.tag && row && secondRow && result.nodes[row].child[1] && !(result.nodes[row].flags & kRowTagNoParens) &&
                result.nodes[secondRow].child[1] && (result.nodes[secondRow].flags & kRowTagNoParens), "row tags") && ok;

    // Font data: what the layout reads from a Glyph.
    result = Parse("\\alpha \\mathbb{R} \\boldsymbol{\\Gamma} \\sum \\implies \\nleq \\oint");
    const uint16_t alpha = result.root;
    const uint16_t blackboard = alpha ? result.nodes[result.nodes[alpha].next].child[0] : 0;
    ok = Expect(alpha && result.nodes[alpha].aux == 5 && result.nodes[alpha].a == 0x61 &&
                result.nodes[alpha].flags == (kGlyphSlant | kGlyphSmall) && blackboard &&
                result.nodes[blackboard].aux == 3 && result.nodes[blackboard].a == 'R' &&
                result.nodes[blackboard].flags == kGlyphOutline, "Greek and blackboard glyphs") && ok;
    ok = ExpectEqual(Shape("\\alpha \\mathbb{R} \\boldsymbol{\\Gamma} \\sum \\implies \\nleq \\oint", false),
                     "Glyph:Ord Group:Ord{Glyph:Ord} Group:Ord{Glyph:Ord} Glyph:Op Glyph:Rel Glyph:Rel Composite:Op",
                     "shape", "symbols") && ok;
    uint16_t node = alpha;
    for (int i = 0; i < 3 && node; i++) node = result.nodes[node].next;
    const uint16_t sum = node;
    const uint16_t implies = sum ? result.nodes[sum].next : 0;
    const uint16_t notLessEqual = implies ? result.nodes[implies].next : 0;
    const uint16_t contour = notLessEqual ? result.nodes[notLessEqual].next : 0;
    ok = Expect(sum && result.nodes[sum].b == kBigOpSum && (result.nodes[sum].flags & kOpDisplayLimits) &&
                implies && (result.nodes[implies].flags & kAtomPad) && notLessEqual &&
                (result.nodes[notLessEqual].flags & kAtomNegated) && contour &&
                result.nodes[contour].b == kBigOpIntegral && !(result.nodes[contour].flags & kOpDisplayLimits),
                "operator, pad and negation bits") && ok;
    return ok;
}

bool CheckPinned() {
    bool ok = true;
    MathParseResult reused;
    for (const Pinned& item : kPinned) {
        ParseMath(item.tex, item.display, item.fallback, reused);      // one result object for all: capacity is reused
        const std::string defect = ArenaDefect(reused);
        ok = Expect(defect.empty(), defect.c_str(), item.tex) && ok;
        ok = Expect(!reused.sourceFallback && Fnv1a64(ArenaText(reused)) == item.hash, "pinned arena changed", item.tex) && ok;
        ok = Expect(ArenaText(Parse(item.tex, item.display, item.fallback)) == ArenaText(reused), "a reused result differs", item.tex) && ok;
    }
    return ok;
}

uint32_t NextRandom(uint64_t& state) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(state >> 33);
}

bool CheckFuzz() {
    const size_t pieces = sizeof(kFuzzAlphabet) / sizeof(kFuzzAlphabet[0]);
    const int lengths[] = {1, 2, 3, 5, 8, 13, 21, 34, 55, 200, 900};
    uint64_t state = 20261008;
    MathParseResult result;
    std::string tex;
    for (int round = 0; round < 5000; round++) {
        const int length = lengths[NextRandom(state) % 11];
        tex.clear();
        for (int i = 0; i < length; i++) {
            const Piece& piece = kFuzzAlphabet[NextRandom(state) % pieces];
            tex.append(piece.data, piece.size);
        }
        if (NextRandom(state) % 20 == 0) tex.resize(tex.size() < 200 ? NextRandom(state) % (tex.size() + 1) : NextRandom(state) % 200);
        const bool display = NextRandom(state) % 2 != 0;
        const bool fallback = NextRandom(state) % 2 != 0;
        ParseMath(tex, display, fallback, result);
        const std::string defect = ArenaDefect(result);
        if (!defect.empty()) {
            std::cerr << "math parser: fuzz case " << round << ": " << defect << std::endl;
            return false;
        }
        if (!fallback && !result.text.empty()) {
            for (uint16_t i = 1; i < result.nodes.size(); i++) {
                if (result.nodes[i].kind == MathNodeKind::Fallback) return Expect(false, "Fallback node without a fallback font");
            }
        }
    }
    return true;
}

// Inputs built to be expensive or deep (16 KiB each): they must end, obey the arena rules and
// never recurse without bound.
bool CheckHostile() {
    const size_t limit = kMathMaxSourceBytes;
    const std::string cases[] = {
        Repeat("\\text{", limit / 6), Repeat("\\text{a & ", limit / 10), Repeat("\\text{$", limit / 7),
        "\\text{" + Repeat("$x", limit / 2 - 8) + "}", Repeat("{", limit), Repeat("}", limit),
        Repeat("\\hat", limit / 4), Repeat("\\left(", limit / 6), Repeat("\\begin{matrix}", limit / 14),
        Repeat("\\end{matrix}", limit / 12), Repeat("x^", limit / 2), Repeat("&", 31) + Repeat("\\\\", 60),
        "{" + Repeat("&", limit - 2) + "}", "{" + Repeat("&", 30), Repeat("{a&}", limit / 4),
        Repeat(Repeat("{a&", 31) + "\\\\", 60), "x" + Repeat("'", limit - 1), Repeat("\\", limit),
        Repeat("%\n", limit / 2), Repeat("\\sqrt[", limit / 6), Repeat("\\frac", limit / 5),
        Repeat("\\operatorname", limit / 13), Repeat("\\not", limit / 4), Repeat("a\\over ", limit / 7),
        Repeat("\\substack{", limit / 10), Repeat("\xC2\xB2", limit / 2), "\\text{" + Repeat("{", limit - 7) + "}",
        Repeat("\xFF", limit), Repeat("\\rule[", limit / 6), "x" + Repeat("\\tag{a}", (limit - 1) / 7),
        "\\begin{matrix}" + Repeat(Repeat("a&", 23) + "a\\\\", 64) + "\\end{matrix}",
        "\\text{" + Repeat("a\\not{\\tag{1}}", (limit - 7) / 14) + "}",
    };
    bool ok = true;
    MathParseResult result;
    for (const std::string& tex : cases) {
        ParseMath(std::string_view(tex).substr(0, limit), true, true, result);
        const std::string defect = ArenaDefect(result);
        ok = Expect(defect.empty(), defect.c_str(), std::string_view(tex).substr(0, 40)) && ok;
    }
    return ok;
}

} // namespace math_parser_tests

bool CheckMathParser() {
    using namespace math_parser_tests;
    bool ok = CheckClasses();
    ok = CheckLimits() && ok;
    ok = CheckTextAndBytes() && ok;
    ok = CheckConstructs() && ok;
    ok = CheckPinned() && ok;
    ok = CheckFuzz() && ok;
    ok = CheckHostile() && ok;
    return ok;
}
