#include "math_layout.h"

#include "math_parser.h"
#include "../common/text_utils.h"

#include <algorithm>
#include <cmath>

// Box layout and PDF emission of the native TeX-subset math module.
// Contract: docs/development/native_math.md. All lengths are points; "s" is the font size
// of the current math style; children are laid out at the origin and moved with Shift.

namespace TinyPdf::Internal {
namespace {

#include "math_font_metrics.inc"

using RayoMd::Text::AppendFixed2;

// ---- draw list -----------------------------------------------------------------------------
enum : uint8_t { kItemGlyph, kItemRule, kItemShape, kItemFallbackText };
constexpr uint8_t kItemSlant = kGlyphSlant;       // text matrix 1 0 0.18 1
constexpr uint8_t kItemOutline = kGlyphOutline;   // text render mode 1
constexpr uint8_t kItemHeavy = kGlyphHeavy;       // text render mode 2
constexpr uint8_t kItemJoin = 0x10;               // append my code to the string of the item before me
constexpr uint8_t kItemStyleMask = kItemSlant | kItemOutline | kItemHeavy;
constexpr uint8_t kShapeMirror = 0x01;            // local x -> width - x
constexpr uint8_t kShapeTranspose = 0x02;         // local x and y exchanged
enum : uint8_t {
    kShapeSurd, kShapeParen, kShapeBracket, kShapeFloor, kShapeCeil, kShapeBrace, kShapeVert,
    kShapeDblVert, kShapeAngle, kShapeArrow, kShapeWideHat, kShapeWideTilde, kShapeFrame,
    kShapeLine, kShapeCircle
};

// ---- limits ----------------------------------------------------------------------------------
constexpr size_t kMathMaxItems = 8192;
constexpr int kMathMaxLayoutDepth = 64;

// ---- parameters, in em of the current style size ----------------------------------------------
constexpr double kXHeight = 0.450, kAxis = 0.2605;
constexpr double kSup1 = 0.42, kSup2 = 0.37, kSup3 = 0.30, kSub1 = 0.16, kSub2 = 0.25;
constexpr double kSupDrop = 0.386, kSubDrop = 0.050, kSubSupGap = 0.16, kScriptSpace = 0.05;
constexpr double kNum1 = 0.70, kNum2 = 0.41, kNum3 = 0.46, kDenom1 = 0.70, kDenom2 = 0.36;
constexpr double kFracGapD = 0.14, kFracGapT = 0.05, kStackGapD = 0.38, kStackGapT = 0.16;
constexpr double kFracPad = 0.10, kDelim1 = 2.39, kDelim2 = 1.01;
constexpr double kBos1 = 0.11, kBos2 = 0.17, kBos3 = 0.20, kBos4 = 0.60, kBos5 = 0.10;
constexpr double kDelimFactor = 0.901, kDelimShortfall = 0.50, kDelimGlyphMax = 1.02;
constexpr double kRadicalPad = 0.08, kNullDelimiter = 0.12, kPairIc = 0.5;
constexpr double kPrimeAdvance = 0.92 * 0.247, kPrimeLead = 0.02, kPrimeTop = 0.735;
constexpr double kAtomPadEm = 5.0 / 18.0;

// Space before the right atom: 0 none, 1 thin, 2 medium, 3 thick; negative = display and text
// styles only. Rows: class of the left atom, columns: class of the right atom.
constexpr int8_t kSpacing[8][8] = {
    { 0,  1, -2, -3,  0,  0,  0, -1},   // Ord
    { 1,  1,  0, -3,  0,  0,  0, -1},   // Op
    {-2, -2,  0,  0, -2,  0,  0, -2},   // Bin
    {-3, -3,  0,  0, -3,  0,  0, -3},   // Rel
    { 0,  0,  0,  0,  0,  0,  0,  0},   // Open
    { 0,  1, -2, -3,  0,  0,  0, -1},   // Close
    {-1, -1,  0, -1, -1, -1, -1, -1},   // Punct
    {-1,  1, -2, -3, -1,  0, -1, -1},   // Inner
};

// Display scale of the big-operator groups (MathBigOp): [group][display].
constexpr double kBigOpScale[kBigOpCount][2] = {
    {1.00, 1.00}, {1.00, 1.45}, {1.00, 1.90}, {1.10, 1.60}, {1.10, 1.50}};

// Vector shape of delimiter ids 1..14 and its width = min(base + slope * H/s, cap) * s.
struct DelimShape { uint8_t shape; float base, slope, cap; };
constexpr DelimShape kDelimShapes[15] = {
    {kShapeVert, 0, 0, 0},
    {kShapeParen, 0.26f, 0.075f, 0.62f}, {kShapeParen, 0.26f, 0.075f, 0.62f},
    {kShapeBracket, 0.30f, 0.03f, 0.50f}, {kShapeBracket, 0.30f, 0.03f, 0.50f},
    {kShapeBrace, 0.36f, 0.06f, 0.70f}, {kShapeBrace, 0.36f, 0.06f, 0.70f},
    {kShapeVert, 0.22f, 0, 0.22f}, {kShapeDblVert, 0.38f, 0, 0.38f},
    {kShapeAngle, 0.26f, 0.07f, 0.70f}, {kShapeAngle, 0.26f, 0.07f, 0.70f},
    {kShapeFloor, 0.30f, 0.03f, 0.50f}, {kShapeFloor, 0.30f, 0.03f, 0.50f},
    {kShapeCeil, 0.30f, 0.03f, 0.50f}, {kShapeCeil, 0.30f, 0.03f, 0.50f},
};

double Round2(double value) { return static_cast<double>(std::llround(value * 100.0)) / 100.0; }
double RuleThickness(double s) { return std::max(0.05 * s, 0.30); }
double SurdWidth(double height, double s) {
    return (0.56 + std::min(0.20, 0.05 * std::max(0.0, height / s - 1.0))) * s;
}

const MathGlyphMetrics& Metrics(unsigned font, unsigned code) {
    return kMathGlyphMetrics[font - 1u < 5u ? font - 1u : 0u][code - 0x20u < 224u ? code - 0x20u : 0u];
}

struct Style {
    uint8_t level;   // 0 display, 1 text, 2 script, 3 scriptscript
    bool cramped;    // lowers superscripts
};
Style Sup(Style st) { return {static_cast<uint8_t>(st.level <= 1 ? 2 : 3), st.cramped}; }
Style Sub(Style st) { return {static_cast<uint8_t>(st.level <= 1 ? 2 : 3), true}; }
Style Num(Style st) { return {static_cast<uint8_t>(st.level < 3 ? st.level + 1 : 3), st.cramped}; }
Style Den(Style st) { return {static_cast<uint8_t>(st.level < 3 ? st.level + 1 : 3), true}; }
Style Cramp(Style st) { return {st.level, true}; }

struct Box {
    double w = 0, h = 0, d = 0;   // width, height above and depth below the baseline
    double ic = 0;                // trailing italic correction, not part of w
    double lc = 0;                // leading overhang, not part of w
    double skew = 0;              // accent attach point minus w / 2
    uint32_t first = 0, end = 0;  // item range
    uint8_t font = 0;             // of a single glyph (LayoutLeftRight keeps a \middle id here)
    bool singleGlyph = false;     // a character for script placement
    bool italicVar = false;       // single glyph of font 2 or 4: italic pair rule
    bool opGlyph = false;         // big operator: italic correction moves the limits
    bool plain = false;           // exactly one unpadded glyph item of font 1..4 without flags, at the origin
};

enum : unsigned { kTopLevel = 1, kTextItems = 2, kStopAtMiddle = 4 };

struct Layouter {
    std::vector<MathItem>& items;
    std::string& text;
    const MathFallbackFont* fallback;
    bool bold;

    const MathNode* nodes = nullptr;
    size_t nodeCount = 0;
    std::string_view source{};        // bytes of TextRun and Fallback nodes
    std::vector<Box> scratch{};       // grid cells and \middle segments, used as a stack
    double sizes[4] = {};
    size_t visits = 0, visitLimit = 0;
    int depth = 0;
    bool overflow = false;            // a layout limit was hit: the pass is void
    bool stroke = false;
    uint16_t stopNode = 0;            // Middle that ended the last kStopAtMiddle list
    Style stopStyle{1, false};

    // Result of the last pass. With a tag: items [0, tagFirst) are the body, the rest the tag
    // (or the tag column of a top-level grid), whose right edge is at `natural`.
    double natural = 0, height = 0, drop = 0, bodyWidth = 0, tagWidth = 0;
    uint32_t tagFirst = 0;
    uint16_t tagGrid = 0;
    bool hasTag = false;

    const MathNode& Node(unsigned index) const { return nodes[index < nodeCount ? index : 0]; }
    double Size(Style st) const { return sizes[st.level & 3]; }

    // ---- items ---------------------------------------------------------------------------
    void Push(const MathItem& item) {
        if (items.size() >= kMathMaxItems) overflow = true;
        else items.push_back(item);
    }
    void Glyph(double x, double y, double size, unsigned font, unsigned code, unsigned flags) {
        MathItem item{};
        item.x = static_cast<float>(x);
        item.y = static_cast<float>(y);
        item.a = static_cast<float>(size);
        item.kind = kItemGlyph;
        item.font = static_cast<uint8_t>(font);
        item.code = static_cast<uint8_t>(code);
        item.flags = static_cast<uint8_t>(flags);
        if (flags & (kItemOutline | kItemHeavy)) stroke = true;
        Push(item);
    }
    void Rule(double x, double y, double width, double height) {
        if (!(width > 0.0) || !(height > 0.0)) return;
        MathItem item{};
        item.x = static_cast<float>(x);
        item.y = static_cast<float>(y);
        item.a = static_cast<float>(width);
        item.b = static_cast<float>(height);
        item.kind = kItemRule;
        if (std::min(width, height) <= 1.0) stroke = true;
        Push(item);
    }
    void Shape(unsigned id, double x, double y, double a, double b, double c, unsigned flags = 0) {
        if (!(c > 0.0)) return;
        if (id == kShapeLine) {
            if (a == 0.0 && b == 0.0) return;
        } else if (!(a > 0.0) || (id != kShapeArrow && id != kShapeCircle && !(b > 0.0))) {
            return;
        }
        MathItem item{};
        item.x = static_cast<float>(x);
        item.y = static_cast<float>(y);
        item.a = static_cast<float>(a);
        item.b = static_cast<float>(b);
        item.c = static_cast<float>(c);
        item.kind = kItemShape;
        item.code = static_cast<uint8_t>(id);
        item.flags = static_cast<uint8_t>(flags);
        if (id >= kShapeAngle && id != kShapeWideHat) stroke = true;
        Push(item);
    }
    void Shift(const Box& box, double dx, double dy) {
        if (dx == 0.0 && dy == 0.0) return;
        const size_t end = std::min<size_t>(box.end, items.size());
        for (size_t i = box.first; i < end; ++i) {
            items[i].x += static_cast<float>(dx);
            items[i].y += static_cast<float>(dy);
        }
    }
    Box Open() const {
        Box box;
        box.first = box.end = static_cast<uint32_t>(items.size());
        return box;
    }
    void Close(Box& box) const { box.end = static_cast<uint32_t>(items.size()); }
    static void Cover(Box& out, const Box& part, double dy = 0.0) {
        out.h = std::max(out.h, part.h + dy);
        out.d = std::max(out.d, part.d - dy);
    }

    // ---- glyphs and composites ---------------------------------------------------------------
    Box GlyphBox(unsigned font, unsigned code, unsigned flags, double size) {
        if (font - 1u >= 5u) font = 1;
        if (bold) {
            if (font == 1 || font == 2) font += 2;
            else if (font == 5) flags |= kGlyphHeavy;
        }
        const double e = Round2(size * ((flags & kGlyphSmall) ? 0.90 : 1.0));
        const double u = e / 1000.0;
        const MathGlyphMetrics& m = Metrics(font, code);
        const bool slant = (flags & kGlyphSlant) != 0;
        const double sigma = slant ? 0.18 : 0.0;
        const double dx = slant ? -0.035 * e : 0.0;
        Box box = Open();
        box.w = m.advance * u;
        box.h = std::max<int>(0, m.yMax) * u;
        box.d = std::max<int>(0, -m.yMin) * u;
        box.ic = std::max(0.0, m.xMax * u + 0.60 * sigma * box.h + dx - box.w);
        box.lc = std::max(0.0, -(m.xMin * u + sigma * std::min<int>(0, m.yMin) * u + dx));
        const double tau = font == 2 ? 0.277 : font == 4 ? 0.268 : sigma;
        box.skew = (m.xMin + m.xMax) * 0.5 * u + dx + 0.5 * tau * box.h - box.w * 0.5;
        Glyph(dx, 0.0, e, font, code, flags & kItemStyleMask);
        Close(box);
        box.font = static_cast<uint8_t>(font);
        box.singleGlyph = true;
        box.italicVar = font == 2 || font == 4;
        box.plain = font <= 4 && (flags & kItemStyleMask) == 0;
        return box;
    }

    Box CompositeBox(unsigned id, unsigned flags, double e) {
        Box box = Open();
        if (id >= static_cast<unsigned>(kMathCompositeCount)) return box;
        const MathComposite& composite = kMathComposites[id];
        const double u = e / 1000.0;
        const bool heavy = bold || (flags & kGlyphHeavy) != 0;
        box.w = composite.advance * u;
        box.h = std::max<int>(0, composite.bbox[3]) * u;
        box.d = std::max<int>(0, -composite.bbox[1]) * u;
        box.ic = std::max(0.0, composite.bbox[2] * u - box.w);
        box.lc = std::max(0.0, -composite.bbox[0] * u);
        box.skew = (composite.bbox[0] + composite.bbox[2]) * 0.5 * u - box.w * 0.5;
        for (unsigned i = 0; i < composite.pieceCount; ++i) {
            const MathCompositePiece& piece = kMathCompositePieces[composite.firstPiece + i];
            const int16_t* v = piece.v;
            if (piece.kind == kPieceGlyph) {
                Glyph(v[1] * u, v[2] * u, Round2(e * v[0] / 1000.0), piece.font, piece.code,
                      (piece.transform << 5) | (heavy ? kItemHeavy : 0));
            } else if (piece.kind == kPieceRect) {
                const double grow = heavy ? 15.0 * u : 0.0;
                Rule(v[0] * u - grow, v[1] * u - grow, (v[2] - v[0]) * u + 2.0 * grow, (v[3] - v[1]) * u + 2.0 * grow);
            } else if (piece.kind == kPieceLine) {
                Shape(kShapeLine, v[0] * u, v[1] * u, (v[2] - v[0]) * u, (v[3] - v[1]) * u, (v[4] + (heavy ? 30 : 0)) * u);
            } else {
                Shape(kShapeCircle, v[0] * u, v[1] * u, v[2] * u, 0.0, (v[3] + (heavy ? 30 : 0)) * u);
            }
        }
        Close(box);
        box.singleGlyph = true;
        return box;
    }

    Box LayoutBigOp(const MathNode& node, Style st) {
        const double s = Size(st);
        const bool display = st.level == 0;
        const unsigned group = node.b < kBigOpCount ? node.b : static_cast<unsigned>(kBigOpSum);
        const double e = Round2(s * kBigOpScale[group][display]);
        Box box = node.kind == MathNodeKind::Composite ? CompositeBox(node.a, node.flags, e)
                                                       : GlyphBox(node.aux, node.a, node.flags, e);
        const double dy = kAxis * s - (box.h - box.d) / 2.0;
        Shift(box, 0.0, dy);
        box.h = std::max(0.0, box.h + dy);
        box.d = std::max(0.0, box.d - dy);
        box.ic = group == kBigOpIntegral ? (display ? 0.30 : 0.25) * 0.274 * e : 0.0;
        box.lc = box.skew = 0.0;
        box.opGlyph = true;
        box.singleGlyph = box.italicVar = box.plain = false;
        return box;
    }

    // ---- text ------------------------------------------------------------------------------
    Box LayoutRun(const MathNode& node, Style st) {
        Box box = Open();
        const size_t offset = std::min<size_t>(node.a, source.size());
        const size_t length = std::min<size_t>(node.b, source.size() - offset);
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(source.data()) + offset;
        unsigned font = node.aux - 1u < 4u ? node.aux : 1u;
        if (bold && font <= 2) font += 2;
        const double size = Size(st), u = size / 1000.0;
        size_t lo = 0, hi = length;
        while (lo < hi && bytes[lo] == ' ') ++lo;
        while (hi > lo && bytes[hi - 1] == ' ') --hi;
        double x = 0.0, inkLeft = 0.0, inkRight = 0.0;
        for (size_t i = 0; i < length; ++i) {
            const MathGlyphMetrics& m = Metrics(font, bytes[i]);
            if (i >= lo && i < hi) {
                Glyph(x, 0.0, size, font, bytes[i], i > lo ? kItemJoin : 0);
                box.h = std::max(box.h, m.yMax * u);
                box.d = std::max(box.d, -m.yMin * u);
                if (i == lo) inkLeft = x + m.xMin * u;
                if (bytes[i] != ' ') inkRight = x + m.xMax * u;
            }
            x += m.advance * u;
        }
        box.w = x;
        box.lc = std::max(0.0, -inkLeft);
        box.ic = std::max(0.0, inkRight - x);
        Close(box);
        return box;
    }

    Box LayoutFallback(const MathNode& node, Style st) {
        const double size = Size(st);
        if (fallback == nullptr || fallback->measure == nullptr) return GlyphBox(1, '?', 0, size);
        Box box = Open();
        const size_t offset = std::min<size_t>(node.a, source.size());
        const std::string_view utf8 = source.substr(offset, node.b);
        double width = fallback->measure(fallback->context, utf8, size);
        if (!(width >= 0.0)) width = 0.0;
        double ascent = fallback->ascent, descent = fallback->descent;
        if (!std::isfinite(ascent)) ascent = 0.74;
        if (!std::isfinite(descent)) descent = 0.21;
        MathItem item{};
        item.a = static_cast<float>(size);
        item.textOffset = static_cast<uint32_t>(text.size());
        item.textLength = static_cast<uint32_t>(utf8.size());
        item.kind = kItemFallbackText;
        if (items.size() < kMathMaxItems) text.append(utf8);
        Push(item);
        box.w = std::min(width, 1.0e7);
        box.h = std::clamp(ascent, 0.0, 2.0) * size;
        box.d = std::clamp(descent, 0.0, 2.0) * size;
        Close(box);
        return box;
    }

    // ---- lists -----------------------------------------------------------------------------
    Box Field(unsigned head, Style st, unsigned flags = 0) {
        Box box = LayoutList(head, st, flags);
        box.w += box.ic;
        box.ic = 0.0;
        return box;
    }

    Box LayoutList(unsigned head, Style st, unsigned flags) {
        Box out = Open();
        Box pb;
        double x = 0.0, right = 0.0, lastEnd = 0.0, lastLead = 0.0;
        int prevClass = -1;
        unsigned atoms = 0, stopped = 0;
        bool spaceSeen = false, anySpace = false, prevMath = false;
        for (unsigned index = head; index != 0 && index < nodeCount; index = nodes[index].next) {
            const MathNode& node = nodes[index];
            if (++visits > visitLimit) {
                overflow = true;
                break;
            }
            if (node.kind == MathNodeKind::Space) {
                int width = static_cast<int16_t>(node.a);
                if (st.level == 0 && node.b != 0) width = static_cast<int16_t>(node.b);
                x += (node.flags & kSpaceAbsolute) ? width / 100.0 : width / 1000.0 * Size(st);
                if (x < 0.0) x = 0.0;
                spaceSeen = anySpace = true;
                continue;
            }
            if (node.kind == MathNodeKind::Style) {
                st.level = node.aux & 3;
                continue;
            }
            if (node.kind == MathNodeKind::Middle) {
                if (!(flags & kStopAtMiddle)) continue;
                stopped = index;
                break;
            }
            const Box box = LayoutNode(index, st);
            const int cls = static_cast<unsigned>(node.cls) < 8u ? static_cast<int>(node.cls) : -1;
            const bool textItems = (flags & kTextItems) != 0;
            double glue = 0.0;
            if (!textItems && prevClass >= 0 && cls >= 0) {
                int code = kSpacing[prevClass][cls];
                if (code < 0) code = st.level <= 1 ? -code : 0;
                if (code > 0) glue = (code + 2) / 18.0 * Size(st);
            }
            const bool tight = glue == 0.0 && !spaceSeen;
            const bool sameFont = atoms != 0 && tight && pb.font == box.font;
            const bool plainPair = sameFont && pb.plain && box.plain && box.first == pb.end &&
                box.end == box.first + 1 && pb.end == pb.first + 1 && items[pb.first].a == items[box.first].a;
            const bool upright = plainPair && (box.font == 1 || box.font == 3);
            const bool italicPair = sameFont && pb.italicVar && box.italicVar;
            double ic = 0.0, lead = 0.0;
            if (atoms != 0 && !upright && (prevMath || !textItems)) ic = pb.ic * (italicPair ? kPairIc : 1.0);
            if (box.lc > 0.0 && tight && !italicPair && !upright && !textItems &&
                !((flags & kTopLevel) && atoms == 0)) {
                lead = box.lc;
            }
            x += ic + glue + lead;
            if (plainPair && ic == 0.0 && lead == 0.0) items[box.first].flags |= kItemJoin;
            Shift(box, x, 0.0);
            x += box.w;
            right = std::max(right, x);
            Cover(out, box);
            if (atoms == 0 && textItems) out.lc = box.lc;
            if (cls >= 0) prevClass = cls;
            prevMath = cls >= 0;
            pb = box;
            lastEnd = x;
            lastLead = lead;
            spaceSeen = false;
            ++atoms;
        }
        if (flags & kStopAtMiddle) {
            stopNode = static_cast<uint16_t>(stopped);
            stopStyle = st;
        }
        Close(out);
        out.w = std::max(x, right);
        if (atoms != 0) out.ic = std::max(0.0, lastEnd + pb.ic - out.w);
        if (atoms == 1 && !anySpace && pb.singleGlyph) {
            out.singleGlyph = true;
            out.italicVar = pb.italicVar;
            out.font = pb.font;
            out.skew = pb.skew + lastLead / 2.0;
            out.plain = pb.plain && lastLead == 0.0;
        }
        return out;
    }

    // The formula tag or a row tag: text items between parentheses in M1, on the baseline.
    Box LayoutTag(unsigned head, bool parens, Style st) {
        Box out = Open();
        const double s = Size(st);
        if (parens) {
            const Box open = GlyphBox(1, '(', 0, s);
            out.w = open.w;
            Cover(out, open);
        }
        const Box body = Field(head, st, kTextItems);
        Shift(body, out.w, 0.0);
        out.w += body.w;
        Cover(out, body);
        if (parens) {
            const Box close = GlyphBox(1, ')', 0, s);
            Shift(close, out.w, 0.0);
            out.w += close.w;
        }
        Close(out);
        return out;
    }

    // ---- atoms -----------------------------------------------------------------------------
    Box LayoutNode(unsigned index, Style st) {
        Box box = Open();
        const MathNode& node = Node(index);
        if (++visits > visitLimit || depth >= kMathMaxLayoutDepth) {
            overflow = true;
            return box;
        }
        ++depth;
        const double s = Size(st);
        switch (node.kind) {
        case MathNodeKind::Glyph:
            box = node.b != 0 ? LayoutBigOp(node, st) : GlyphBox(node.aux, node.a, node.flags, s);
            break;
        case MathNodeKind::Composite:
            box = node.b != 0 ? LayoutBigOp(node, st) : CompositeBox(node.a, node.flags, s);
            break;
        case MathNodeKind::TextRun:
            box = LayoutRun(node, st);
            break;
        case MathNodeKind::Fallback:
            box = LayoutFallback(node, st);
            break;
        case MathNodeKind::Group:
            if (node.flags & kGroupInText) st.level = std::max<uint8_t>(st.level, 1);
            box = LayoutList(node.child[0], st, 0);
            break;
        case MathNodeKind::Text:
        case MathNodeKind::OpName:
            box = LayoutList(node.child[0], st, kTextItems);
            break;
        case MathNodeKind::Scripts:
            box = LayoutScripts(node, st);
            break;
        case MathNodeKind::Fraction:
            box = LayoutFraction(node, st);
            break;
        case MathNodeKind::Radical:
            box = LayoutRadical(node, st);
            break;
        case MathNodeKind::LeftRight:
            box = LayoutLeftRight(node, st);
            break;
        case MathNodeKind::SizedDelim: {
            static constexpr double kLevels[5] = {1.0, 1.2, 1.8, 2.4, 3.0};
            const unsigned level = std::min<unsigned>(node.aux, 4);
            box = LayoutDelimiter(node.a, kLevels[level] * s, st, level == 0);
            if (node.a == kDelimNone) box.w = kNullDelimiter * s;
            box.singleGlyph = level == 0;
            break;
        }
        case MathNodeKind::Accent:
            box = LayoutAccent(node, st);
            break;
        case MathNodeKind::OverUnder:
            box = node.aux >= kOverRightArrow && node.aux <= kOverLeftRightArrow ? LayoutAccent(node, st)
                                                                                  : LayoutOverUnder(node, st);
            break;
        case MathNodeKind::Stack:
            box = LayoutStack(node, st);
            break;
        case MathNodeKind::Enclose:
            box = LayoutEnclose(node, st);
            break;
        case MathNodeKind::Phantom: {
            const size_t textSize = text.size();
            const Box body = Field(node.child[0], st);
            items.resize(box.first);
            text.resize(textSize);
            if (node.aux != kPhantomHeight) box.w = body.w;
            if (node.aux != kPhantomWidth) Cover(box, body);
            break;
        }
        case MathNodeKind::Array:
            box = LayoutGrid(index, st);
            break;
        default:
            break;   // Null, Space, Style, Middle, Row, Cell, Col: nothing to draw here
        }
        --depth;
        if (node.cls == MathClass::None) return box;
        if (node.flags & kAtomNegated) {
            // Slash through the ink centre, on the axis.
            double cx = box.w / 2.0;
            if (box.singleGlyph) {
                cx += box.skew;
                if (box.font == 2) cx -= 0.5 * 0.277 * box.h;
                else if (box.font == 4) cx -= 0.5 * 0.268 * box.h;
            }
            const double hy = (box.w < 0.400 * s ? 0.180 : 0.290) * s;
            Shape(kShapeLine, cx - 0.115 * s, kAxis * s - hy, 0.230 * s, 2.0 * hy, 0.052 * s);
            box.h = std::max(box.h, kAxis * s + hy);
            box.d = std::max(box.d, hy - kAxis * s);
            box.lc = std::max(box.lc, 0.141 * s - cx);
            box.ic = std::max(box.ic, cx + 0.141 * s - box.w);
            box.italicVar = box.plain = false;
            Close(box);
        }
        if (node.flags & kAtomPad) {
            const double pad = kAtomPadEm * s;
            Shift(box, pad, 0.0);
            box.w += 2.0 * pad;
            box.ic = std::max(0.0, box.ic - pad);
            box.lc = std::max(0.0, box.lc - pad);
            box.italicVar = box.plain = false;
        }
        return box;
    }

    void Primes(unsigned count, double x, double y, double s) {
        for (unsigned i = 0; i < count; ++i) {
            Glyph(x + i * kPrimeAdvance * s, y, s, 5, 0xA2, bold ? kItemHeavy : 0);
        }
    }

    Box LayoutScripts(const MathNode& node, Style st) {
        const double s = Size(st);
        Box out = Open();
        Box nuc = node.child[0] != 0 ? LayoutNode(node.child[0], st) : out;
        const unsigned sub = node.child[1], sup = node.child[2];
        const unsigned primes = std::min<unsigned>(node.aux, kMathMaxPrimes);
        if (sub == 0 && sup == 0 && primes == 0) return nuc;
        const Style supStyle = Sup(st), subStyle = Sub(st);
        const double p = (st.cramped ? kSup3 : st.level == 0 ? kSup1 : kSup2) * s;
        double u = 0.0, v = 0.0;
        if (!nuc.singleGlyph) {
            u = nuc.h - kSupDrop * Size(supStyle);
            v = nuc.d + kSubDrop * Size(subStyle);
        }
        const double primeRaise = std::max(0.0, u - p);
        const double primeWidth = primes != 0 ? (primes * kPrimeAdvance + kPrimeLead) * s : 0.0;
        Box supBox, subBox;
        if (sup != 0) supBox = Field(sup, supStyle);
        if (sub != 0) subBox = Field(sub, subStyle);
        const bool limits = (sub != 0 || sup != 0) &&
            (node.a == kLimitsOn || (node.a != kLimitsOff && st.level == 0 && (node.flags & kOpDisplayLimits)));
        out.h = nuc.h;
        out.d = nuc.d;
        double primeX;
        if (limits) {
            const double delta = nuc.opGlyph ? nuc.ic : 0.0;
            const double width = std::max({nuc.w, supBox.w, subBox.w});
            double xn = (width - nuc.w) / 2.0;
            double xs = (width - supBox.w) / 2.0 + delta / 2.0;
            double xb = (width - subBox.w) / 2.0 - delta / 2.0;
            // The italic correction moves the limits apart; keep both inside the box.
            const double lo = sub != 0 ? std::min(0.0, xb) : 0.0;
            const double hi = sup != 0 ? std::max(width, xs + supBox.w) : width;
            xn -= lo;
            xs -= lo;
            xb -= lo;
            Shift(nuc, xn, 0.0);
            if (sup != 0) {
                const double y = nuc.h + std::max(kBos1 * s, kBos3 * s - supBox.d) + supBox.d;
                Shift(supBox, xs, y);
                out.h = y + supBox.h + kBos5 * s;
            }
            if (sub != 0) {
                const double y = nuc.d + std::max(kBos2 * s, kBos4 * s - subBox.h) + subBox.h;
                Shift(subBox, xb, -y);
                out.d = y + subBox.d + kBos5 * s;
            }
            primeX = xn + nuc.w + delta + kPrimeLead * s;
            out.w = std::max(hi - lo, primes != 0 ? primeX + primeWidth : 0.0);
        } else {
            if (sup != 0) u = std::max({u, p, supBox.d + kXHeight * s / 4.0});
            if (sub != 0 && sup == 0) v = std::max({v, kSub1 * s, subBox.h - 0.8 * kXHeight * s});
            if (sub != 0 && sup != 0) {
                v = std::max(v, kSub2 * s);
                const double gap = (u - supBox.d) - (subBox.h - v);
                if (gap < kSubSupGap * s) {
                    v += kSubSupGap * s - gap;
                    const double psi = 0.8 * kXHeight * s - (u - supBox.d);
                    if (psi > 0.0) {
                        u += psi;
                        v -= psi;
                    }
                }
            }
            primeX = nuc.w + nuc.ic + kPrimeLead * s;
            const double supX = nuc.w + nuc.ic + primeWidth;
            double right = supX;
            if (sup != 0) {
                Shift(supBox, supX, u);
                right += supBox.w;
                Cover(out, supBox, u);
            }
            if (sub != 0) {
                Shift(subBox, nuc.w, -v);   // not moved by the italic correction: it tucks under T, V, f
                right = std::max(right, nuc.w + subBox.w);
                Cover(out, subBox, -v);
            }
            out.w = right + kScriptSpace * s;
            out.lc = nuc.lc;
        }
        if (primes != 0) {
            Primes(primes, primeX, primeRaise, s);
            out.h = std::max(out.h, primeRaise + kPrimeTop * s);
        }
        Close(out);
        return out;
    }

    Box LayoutFraction(const MathNode& node, Style st) {
        if (node.flags & kFractionDisplay) st.level = 0;
        else if (node.flags & kFractionText) st.level = 1;
        const double s = Size(st), axis = kAxis * s;
        const bool display = st.level == 0, bar = (node.flags & kFractionBar) != 0;
        const double th = bar ? RuleThickness(s) : 0.0;
        const double target = (display ? kDelim1 : kDelim2) * s;
        Box out = Open();
        Box left, right;
        double x0 = kFracPad * s;
        if (node.a != kDelimNone) {
            left = LayoutDelimiter(node.a, target, st, false);
            x0 = left.w;
        }
        const Box num = Field(node.child[0], Num(st));
        const Box den = Field(node.child[1], Den(st));
        const double width = std::max(num.w, den.w);
        double u = (display ? kNum1 : bar ? kNum2 : kNum3) * s;
        double v = (display ? kDenom1 : kDenom2) * s;
        if (bar) {
            const double phi = (display ? kFracGapD : kFracGapT) * s;
            const double gapNum = (u - num.d) - (axis + th / 2.0);
            if (gapNum < phi) u += phi - gapNum;
            const double gapDen = (axis - th / 2.0) - (den.h - v);
            if (gapDen < phi) v += phi - gapDen;
        } else {
            const double phi = (display ? kStackGapD : kStackGapT) * s;
            const double psi = (u - num.d) - (den.h - v);
            if (psi < phi) {
                u += (phi - psi) / 2.0;
                v += (phi - psi) / 2.0;
            }
        }
        Shift(num, x0 + (width - num.w) / 2.0, u);
        Shift(den, x0 + (width - den.w) / 2.0, -v);
        Rule(x0, axis - th / 2.0, width, th);
        double x = x0 + width;
        if (node.b != kDelimNone) {
            right = LayoutDelimiter(node.b, target, st, false);
            Shift(right, x, 0.0);
            x += right.w;
        } else {
            x += kFracPad * s;
        }
        out.w = x;
        out.h = std::max({num.h + u, left.h, right.h});
        out.d = std::max({den.d + v, left.d, right.d});
        Close(out);
        return out;
    }

    Box LayoutRadical(const MathNode& node, Style st) {
        const double s = Size(st), th = RuleThickness(s);
        Box out = Open();
        Box index;
        if (node.child[1] != 0) index = Field(node.child[1], Style{3, st.cramped});
        const Box body = Field(node.child[0], Cramp(st));
        double psi = st.level == 0 ? th + kXHeight * s / 4.0 : 1.5 * th;
        const double need = body.h + body.d + psi + th;
        const double height = std::max(need, s);
        psi += (height - need) / 2.0;   // the excess is split above and below
        const double top = body.h + psi + th, bottom = top - height;
        const double surd = SurdWidth(height, s);
        double x = 0.0;
        out.h = top + th;
        out.d = std::max(body.d, -bottom);
        if (node.child[1] != 0) {
            const double raise = bottom + 0.60 * height;
            Shift(index, 0.0, raise);
            x = index.w - std::min(index.w, 0.55 * surd);
            Cover(out, index, raise);
        }
        Shape(kShapeSurd, x, bottom, height, body.w + kRadicalPad * s, s);
        Shift(body, x + surd, 0.0);
        out.w = x + surd + body.w + kRadicalPad * s;
        Close(out);
        return out;
    }

    // A delimiter of total height >= target on the axis, or its natural form. kDelimNone
    // gives an empty box; what "none" means is the caller's business.
    Box LayoutDelimiter(unsigned id, double target, Style st, bool natural) {
        const double s = Size(st);
        Box box = Open();
        if (id == kDelimNone || id >= kDelimCount) return box;
        const MathDelimBaseEntry& base = kMathDelimBase[id];
        if (base.base == kDelimBaseComposite) {
            box = CompositeBox(base.code, 0, s);
        } else if (base.base == kDelimBaseGlyph) {
            const MathGlyphMetrics& m = Metrics(base.font, base.code);
            if (natural || id >= kDelimSlash || target <= kDelimGlyphMax * (m.yMax - m.yMin) / 1000.0 * s) {
                box = GlyphBox(base.font, base.code, 0, s);
            }
        }
        if (box.end != box.first || id >= kDelimSlash) {
            box.ic = box.lc = box.skew = 0.0;
            box.singleGlyph = box.italicVar = box.plain = false;
            return box;
        }
        const double r = target / s;
        double height = s;
        if (!natural && !(base.base == kDelimBaseVector && r <= 1.0)) {
            height = r <= 1.2 ? 1.2 * s : std::min(60.0, 1.2 + 0.3 * std::ceil((r - 1.2) / 0.3 - 1e-9)) * s;
        }
        const DelimShape& shape = kDelimShapes[id];
        const double width = std::min<double>(shape.base + shape.slope * (height / s), shape.cap) * s;
        Shape(shape.shape, 0.0, kAxis * s - height / 2.0, height, width, s, (id & 1) ? 0 : kShapeMirror);
        box.w = width;
        box.h = kAxis * s + height / 2.0;
        box.d = height / 2.0 - kAxis * s;
        Close(box);
        return box;
    }

    Box LayoutLeftRight(const MathNode& node, Style st) {
        const double s = Size(st), axis = kAxis * s;
        Box out = Open();
        const size_t base = scratch.size();
        double delta = 0.0;
        unsigned head = node.child[0];
        Style segmentStyle = st;
        // The body is one segment per \middle; all delimiters are built for one target.
        for (;;) {
            Box segment = Field(head, segmentStyle, kStopAtMiddle);
            const unsigned stop = stopNode;
            delta = std::max({delta, segment.h - axis, segment.d + axis});
            segment.opGlyph = stop != 0;                                   // a \middle follows
            segment.font = static_cast<uint8_t>(Node(stop).a);             // its delimiter
            scratch.push_back(segment);
            if (stop == 0 || overflow) break;
            head = Node(stop).next;
            segmentStyle = stopStyle;
        }
        const double target = std::max(2.0 * delta * kDelimFactor, 2.0 * delta - kDelimShortfall * s);
        unsigned id = node.a;
        double x = 0.0;
        for (size_t i = base;; ++i) {
            Box delimiter = LayoutDelimiter(id, target, st, false);
            if (id == kDelimNone) delimiter.w = kNullDelimiter * s;
            Shift(delimiter, x, 0.0);
            x += delimiter.w;
            Cover(out, delimiter);
            if (i >= scratch.size()) break;
            const Box segment = scratch[i];
            Shift(segment, x, 0.0);
            x += segment.w;
            Cover(out, segment);
            id = segment.opGlyph ? segment.font : node.b;
        }
        scratch.resize(base);
        out.w = x;
        Close(out);
        return out;
    }

    // Accents (Accent) and the over-arrows (OverUnder 2..4).
    Box LayoutAccent(const MathNode& node, Style st) {
        const double s = Size(st), u = s / 1000.0;
        Box out = Open();
        const Box body = LayoutList(node.child[0], Cramp(st), 0);
        const double skew = body.singleGlyph ? body.skew : 0.0;
        const double centre = body.w / 2.0 + skew;
        double top = 0.0, xl = 0.0, xr = body.w;
        const unsigned kind = node.aux;
        if (node.kind == MathNodeKind::OverUnder) {
            const double y = body.h + 0.16 * s, length = body.w + 0.05 * s;
            if (kind == kOverLeftRightArrow) {
                Shape(kShapeArrow, 0.0, y, length / 2.0, 0.0, s, kShapeMirror);
                Shape(kShapeArrow, length / 2.0, y, length / 2.0, 0.0, s);
            } else {
                Shape(kShapeArrow, 0.0, y, length, 0.0, s, kind == kOverLeftArrow ? kShapeMirror : 0);
            }
            top = y + 0.11 * s;
            xr = length;
        } else if (kind <= kAccentRing || kind == kAccentDddot) {
            // Times accent glyph, drawn for an x-height base and raised for a taller one.
            const unsigned font = bold ? 3 : 1;
            const unsigned code = kind == kAccentDddot ? kMathAccentCodes[kAccentDdot] : kMathAccentCodes[kind];
            const MathGlyphMetrics& m = Metrics(font, code);
            const double dy = std::max(0.0, body.h - kXHeight * s);
            double ax = centre - (m.xMin + m.xMax) * 0.5 * u;
            top = dy + m.yMax * u;
            if (kind == kAccentDddot) {
                // The dieresis and a third dot at the pitch of its two.
                const MathGlyphMetrics& dot = Metrics(font, kMathAccentCodes[kAccentDot]);
                const double half = (dot.xMax - dot.xMin) * 0.5;
                const double pitch = (m.xMax - m.xMin) - 2.0 * half;
                ax = centre - (pitch + m.xMin + half) * u;
                Glyph(centre + (pitch - (dot.xMin + dot.xMax) * 0.5) * u, dy, s, font, kMathAccentCodes[kAccentDot], 0);
                xr = centre + (pitch + half) * u;
            } else {
                xr = ax + m.xMax * u;
            }
            xl = ax + m.xMin * u;
            if (kind == kAccentBar) {
                // The macron as a rule with the glyph's own box: TrueType substitutes of Times
                // draw this code as a long overscore.
                Rule(xl, dy + m.yMin * u, (m.xMax - m.xMin) * u, (m.yMax - m.yMin) * u);
            } else {
                Glyph(ax, dy, s, font, code, 0);
            }
        } else if (kind == kAccentVec) {
            const double y = std::max(body.h, kXHeight * s) + 0.13 * s;
            xl = centre - 0.21 * s;
            xr = xl + 0.46 * s;
            Shape(kShapeArrow, xl, y, 0.46 * s, 0.0, s);
            top = y + 0.11 * s;
        } else if (kind == kAccentWideHat || kind == kAccentWideTilde) {
            const double width = std::max(body.w, 0.5 * s);
            const double height = std::min(0.22 * s, 0.10 * s + 0.06 * width);
            const double y = body.h + 0.07 * s;
            xl = (body.w - width) / 2.0 + 0.5 * skew;
            xr = xl + width;
            Shape(kind == kAccentWideHat ? kShapeWideHat : kShapeWideTilde, xl, y, width, height, s);
            top = y + height + 0.02 * s;
        }
        out.w = body.w;
        out.h = std::max(body.h, top);
        out.d = body.d;
        out.lc = std::max(body.lc, -xl);        // accent ink wider than the letter enters the box
        out.ic = std::max(body.ic, xr - body.w);
        out.skew = body.skew;
        out.font = body.font;
        out.singleGlyph = body.singleGlyph;
        Close(out);
        return out;
    }

    // \overline, \underline and the horizontal braces.
    Box LayoutOverUnder(const MathNode& node, Style st) {
        const double s = Size(st), th = RuleThickness(s);
        const bool over = node.aux == kOverLine || node.aux == kOverBrace;
        Box out = Open();
        const Box body = Field(node.child[0], over ? Cramp(st) : st);
        out.w = body.w;
        out.h = body.h;
        out.d = body.d;
        if (node.aux >= kOverBrace) {
            // The vertical brace, transposed: 0.5 em tall, cusp pointing away from the body.
            const double gap = 0.10 * s, tall = 0.50 * s;
            if (over) {
                Shape(kShapeBrace, 0.0, body.h + gap, body.w, tall, s, kShapeTranspose | kShapeMirror);
                out.h = body.h + gap + tall;
            } else {
                Shape(kShapeBrace, 0.0, -body.d - gap - tall, body.w, tall, s, kShapeTranspose);
                out.d = body.d + gap + tall;
            }
        } else if (over) {
            Rule(0.0, body.h + 3.0 * th, body.w, th);
            out.h = body.h + 5.0 * th;
        } else {
            Rule(0.0, -body.d - 4.0 * th, body.w, th);
            out.d = body.d + 5.0 * th;
        }
        Close(out);
        return out;
    }

    // \overset, \underset, \stackrel and the extensible arrows.
    Box LayoutStack(const MathNode& node, Style st) {
        const double s = Size(st);
        Box out = Open();
        Box base, over, under;
        if (node.aux == kStackNoArrow) base = Field(node.child[0], st);
        if (node.child[1] != 0) over = Field(node.child[1], Sup(st));
        if (node.child[2] != 0) under = Field(node.child[2], Sub(st));
        if (node.aux != kStackNoArrow) {
            base = Open();
            base.w = std::max(over.w, under.w) + 0.6 * s;
            base.h = (kAxis + 0.085) * s;
            base.d = (0.085 - kAxis) * s;   // negative: the arrow floats on the axis
            Shape(kShapeArrow, 0.0, kAxis * s, base.w, 0.0, s, node.aux == kStackLeftArrow ? kShapeMirror : 0);
            Close(base);
        }
        const double width = std::max({base.w, over.w, under.w});
        Shift(base, (width - base.w) / 2.0, 0.0);
        out.h = base.h;
        out.d = std::max(0.0, base.d);
        if (node.child[1] != 0) {
            const double y = base.h + std::max(kBos1 * s, kBos3 * s - over.d) + over.d;
            Shift(over, (width - over.w) / 2.0, y);
            out.h = y + over.h;
        }
        if (node.child[2] != 0) {
            const double y = base.d + std::max(kBos2 * s, kBos4 * s - under.h) + under.h;
            Shift(under, (width - under.w) / 2.0, -y);
            out.d = y + under.d;
        }
        out.w = width;
        Close(out);
        return out;
    }

    // \boxed and the cancel strokes.
    Box LayoutEnclose(const MathNode& node, Style st) {
        const double s = Size(st);
        Box out = Open();
        const Box body = Field(node.child[0], st);
        if (node.aux == kEncloseBox) {
            const double line = 0.04 * s, pad = 0.30 * s + line;
            Shift(body, pad, 0.0);
            out.w = body.w + 2.0 * pad;
            out.h = body.h + pad;
            out.d = body.d + pad;
            Shape(kShapeFrame, line / 2.0, -out.d + line / 2.0, out.w - line, out.h + out.d - line, line);
        } else {
            const double th = RuleThickness(s), tall = body.h + body.d;
            out.w = body.w;
            out.h = body.h;
            out.d = body.d;
            if (node.aux != kEncloseBCancel) Shape(kShapeLine, 0.0, -body.d, body.w, tall, th);
            if (node.aux != kEncloseCancel) Shape(kShapeLine, 0.0, body.h, body.w, -tall, th);
        }
        Close(out);
        return out;
    }

    // Matrices, cases, alignments, \substack and implicit rows.
    Box LayoutGrid(unsigned arrayIndex, Style st) {
        const MathNode& node = Node(arrayIndex);
        Box out = Open();
        unsigned rows = 0, cols = 0;
        for (unsigned r = node.child[0]; r != 0 && r < nodeCount; r = nodes[r].next) {
            unsigned cells = 0;
            for (unsigned c = nodes[r].child[0]; c != 0 && c < nodeCount; c = nodes[c].next) ++cells;
            cols = std::max(cols, cells);
            if (++rows > static_cast<unsigned>(kMathMaxRows) || cells > static_cast<unsigned>(kMathMaxColumns)) {
                overflow = true;
                return out;
            }
        }
        unsigned specified = 0;
        for (unsigned c = node.child[1]; c != 0 && c < nodeCount; c = nodes[c].next) {
            if (++specified > static_cast<unsigned>(kMathMaxColumns)) {
                overflow = true;
                return out;
            }
        }
        if (rows == 0 || specified == 0) return out;
        cols = std::max(cols, specified);

        const Style cellStyle{std::max<uint8_t>(st.level, node.aux & kArrayStyleMask), st.cramped};
        const double s = Size(st), sc = Size(cellStyle), th = RuleThickness(s);
        const bool script = (node.aux & kArrayStyleMask) == 2, cases = (node.aux & kArrayCases) != 0;
        const double stretch = cases ? 1.2 : 1.0;
        const double strutHeight = (script ? 0.70 : 0.84) * sc * stretch;
        const double strutDepth = (script ? 0.15 : 0.36) * sc * stretch;
        const double rowGap = !script && (node.aux & kArrayRowGap) ? 0.25 * sc : 0.0;
        const double colGap = node.b / 1000.0 * sc;

        // Scratch: the cells, then one slot per column (w = width, font = alignment, ic = rules
        // before it), then one per row (h, d, w = baseline, skew = tag width, first/end = tag).
        const size_t base = scratch.size(), colBase = base + rows * cols, rowBase = colBase + cols;
        scratch.resize(rowBase + rows);
        unsigned rulesAfter = 0;
        {
            unsigned c = 0, col = node.child[1];
            for (; c < cols; ++c) {
                const MathNode& spec = Node(col);
                uint8_t align = kColumnCentre;   // columns beyond the specification repeat its last alignment
                if (col != 0) align = spec.aux;
                else if (c != 0) align = scratch[colBase + c - 1].font;
                scratch[colBase + c].font = align;
                scratch[colBase + c].ic = col != 0 ? spec.a : 0;
                if (col != 0) {
                    rulesAfter = spec.b;
                    col = spec.next;
                }
            }
        }
        double total = 0.0;
        {
            unsigned r = 0;
            for (unsigned row = node.child[0]; r < rows; row = Node(row).next, ++r) {
                double h = strutHeight, d = strutDepth;
                unsigned c = 0;
                for (unsigned cell = Node(row).child[0]; cell != 0 && c < cols; cell = Node(cell).next, ++c) {
                    const Box box = Field(Node(cell).child[0], cellStyle);
                    scratch[base + r * cols + c] = box;
                    scratch[colBase + c].w = std::max(scratch[colBase + c].w, box.w);
                    h = std::max(h, box.h);
                    d = std::max(d, box.d);
                }
                scratch[rowBase + r].h = h;
                scratch[rowBase + r].d = d;
                total += h + d + (r != 0 ? rowGap : 0.0);
            }
        }
        const double top = kAxis * s + total / 2.0;
        const double target = std::max(total * kDelimFactor, total - kDelimShortfall * s);
        const unsigned leftId = node.a & 0xFF, rightId = node.a >> 8;
        const double delimPad = cases ? 0.0 : 0.10 * s;
        Box left, right;
        double xBody = 0.0;
        if (leftId != kDelimNone) {
            left = LayoutDelimiter(leftId, target, st, false);
            xBody = left.w + delimPad;
        }
        // Column positions; a vertical rule sits in the middle of its gap, or in half an em
        // of extra space at a grid edge.
        double x = xBody;
        for (unsigned c = 0; c < cols; ++c) {
            Box& column = scratch[colBase + c];
            const bool rule = column.ic != 0.0;
            if (c == 0 && rule) x += 0.5 * sc;
            const double gapBefore = c == 0 ? (rule ? 0.5 * sc : 0.0) : x - column.skew;
            if (rule) Rule(x - gapBefore / 2.0 - th / 2.0, top - total, th, total);
            column.ic = x;                                   // left edge of the column
            x += column.w;
            if (c + 1 < cols) {
                scratch[colBase + c + 1].skew = x;           // right edge of the previous column
                if (!((node.aux & kArrayAligned) && (c & 1) == 0)) x += colGap;
            }
        }
        if (rulesAfter != 0) {
            Rule(x + 0.25 * sc - th / 2.0, top - total, th, total);
            x += 0.5 * sc;
        }
        const double xEnd = x;
        double y = top;
        {
            unsigned r = 0;
            for (unsigned row = node.child[0]; r < rows; row = Node(row).next, ++r) {
                Box& metrics = scratch[rowBase + r];
                if (Node(row).aux != 0) Rule(xBody, y + (r != 0 ? rowGap / 2.0 : 0.0) - th / 2.0, xEnd - xBody, th);
                const double baseline = y - metrics.h;
                metrics.w = baseline;
                for (unsigned c = 0; c < cols; ++c) {
                    const Box& column = scratch[colBase + c];
                    const Box& cell = scratch[base + r * cols + c];
                    const double slack = column.w - cell.w;
                    const double offset = column.font == kColumnLeft ? 0.0 : column.font == kColumnRight ? slack : slack / 2.0;
                    Shift(cell, column.ic + offset, baseline);
                }
                y = baseline - metrics.d - rowGap;
            }
        }
        // A rule on the outer edge stands with half its thickness outside the rows.
        const double ruleAbove = Node(node.child[0]).aux != 0 ? th / 2.0 : 0.0;
        const double ruleBelow = (node.flags & kArrayRulesBelowMask) ? th / 2.0 : 0.0;
        if (ruleBelow != 0.0) Rule(xBody, top - total - th / 2.0, xEnd - xBody, th);
        if (rightId != kDelimNone) {
            x += delimPad;
            right = LayoutDelimiter(rightId, target, st, false);
            Shift(right, x, 0.0);
            x += right.w;
        }
        out.w = x;
        out.h = std::max({top + ruleAbove, left.h, right.h});
        out.d = std::max({0.0, total - top + ruleBelow, left.d, right.d});

        // Row tags: a right-aligned column two em right of the grid.
        const uint32_t firstTag = static_cast<uint32_t>(items.size());
        const Style tagStyle{std::max<uint8_t>(cellStyle.level, 1), false};
        double tagColumn = 0.0;
        {
            unsigned r = 0;
            for (unsigned row = node.child[0]; r < rows; row = Node(row).next, ++r) {
                const MathNode& rowNode = Node(row);
                if (rowNode.child[1] == 0) continue;
                const Box tag = LayoutTag(rowNode.child[1], !(rowNode.flags & kRowTagNoParens), tagStyle);
                Box& metrics = scratch[rowBase + r];
                metrics.first = tag.first;
                metrics.end = tag.end;
                metrics.skew = tag.w;
                tagColumn = std::max(tagColumn, tag.w);
                Cover(out, tag, metrics.w);
            }
        }
        if (items.size() != firstTag) {
            const double edge = x + 2.0 * Size(tagStyle) + tagColumn;
            for (unsigned r = 0; r < rows; ++r) {
                const Box& metrics = scratch[rowBase + r];
                if (metrics.end != metrics.first) Shift(metrics, edge - metrics.skew, metrics.w);
            }
            tagGrid = static_cast<uint16_t>(arrayIndex);
            tagFirst = firstTag;
            bodyWidth = x;
            tagWidth = tagColumn;
            out.w = edge;
        }
        scratch.resize(base);
        Close(out);
        return out;
    }

    // ---- one pass over an arena ----------------------------------------------------------------
    void Run(const MathParseResult& arena, double size, bool display) {
        static constexpr double kRatio[4] = {1.0, 1.0, 0.72, 0.56};
        nodes = arena.nodes.data();
        nodeCount = arena.nodes.size();
        source = arena.text;
        items.clear();
        text.clear();
        scratch.clear();
        for (int i = 0; i < 4; ++i) sizes[i] = Round2(std::max(size * kRatio[i], std::min(size, 5.0)));
        visits = 0;
        visitLimit = 8 * nodeCount + 64;
        depth = 0;
        overflow = stroke = false;
        tagGrid = 0;
        const Box body = Field(arena.root, Style{static_cast<uint8_t>(display ? 0 : 1), false}, kTopLevel);
        natural = body.w;
        height = body.h;
        drop = body.d;
        hasTag = false;
        if (arena.tag != 0) {
            const double gap = 2.0 * sizes[1];
            tagFirst = static_cast<uint32_t>(items.size());
            const Box tag = LayoutTag(arena.tag, arena.tagParens, Style{1, false});
            Shift(tag, body.w + gap, 0.0);
            bodyWidth = body.w;
            tagWidth = tag.w;
            natural = body.w + gap + tag.w;
            height = std::max(height, tag.h);
            drop = std::max(drop, tag.d);
            hasTag = true;
        } else if (tagGrid != 0 && tagGrid == arena.root && Node(arena.root).next == 0) {
            hasTag = true;   // the tag column of a top-level grid may go flush right
        }
    }

    double FitRatio(double maxWidth, double maxHeight) const {
        double ratio = 1.0;
        if (maxWidth > 0.0 && natural > maxWidth) ratio = maxWidth / natural;
        if (maxHeight > 0.0 && height + drop > maxHeight) ratio = std::min(ratio, maxHeight / (height + drop));
        return ratio;
    }
};

// ---- emission ---------------------------------------------------------------------------------
void Number(std::string& out, double value) {
    out += ' ';
    AppendFixed2(out, value);
}

// Writes "<w> w" when the width, in 1/100 pt, differs from the last one written in this formula.
void LineWidth(std::string& out, double width, long long& last) {
    const long long centi = std::max<long long>(1, std::llround(width * 100.0));
    if (centi == last) return;
    last = centi;
    Number(out, static_cast<double>(centi) * 0.01);
    out += " w";
}

struct PathOut {
    std::string& out;
    double ox, oy, width;
    bool mirror, transpose;

    void Point(double x, double y) {
        if (mirror) x = width - x;
        if (transpose) std::swap(x, y);
        Number(out, ox + x);
        Number(out, oy + y);
    }
    void Move(double x, double y) {
        Point(x, y);
        out += " m";
    }
    void Line(double x, double y) {
        Point(x, y);
        out += " l";
    }
    void Curve(double x1, double y1, double x2, double y2, double x3, double y3) {
        Point(x1, y1);
        Point(x2, y2);
        Point(x3, y3);
        out += " c";
    }
    void Polygon(const double* xy, int count, const char* paint) {
        Move(xy[0], xy[1]);
        for (int i = 1; i < count; ++i) Line(xy[2 * i], xy[2 * i + 1]);
        out += paint;
    }
};

void EmitRule(std::string& out, const MathItem& item, double ox, double oy, long long& lineWidth) {
    const double x = ox + item.x, y = oy + item.y, w = item.a, h = item.b;
    if (std::min(w, h) > 1.0) {
        Number(out, x);
        Number(out, y);
        Number(out, w);
        Number(out, h);
        out += " re f";
        return;
    }
    // Thin rules are stroked along their longer side: viewers grey out thin fills.
    const bool horizontal = h <= w;
    LineWidth(out, horizontal ? h : w, lineWidth);
    Number(out, horizontal ? x : x + w / 2.0);
    Number(out, horizontal ? y + h / 2.0 : y);
    out += " m";
    Number(out, horizontal ? x + w : x + w / 2.0);
    Number(out, horizontal ? y + h / 2.0 : y + h);
    out += " l S";
}

void EmitShape(std::string& out, const MathItem& item, double ox, double oy, long long& lineWidth) {
    const double a = item.a, b = item.b, c = item.c;
    PathOut path{out, ox + item.x, oy + item.y, item.code == kShapeArrow ? a : b,
                 (item.flags & kShapeMirror) != 0, (item.flags & kShapeTranspose) != 0};
    // Delimiters and the surd: a = height, b = width (surd: vinculum length), c = style size.
    const double H = a, W = b, s = c, r = H / s;
    switch (item.code) {
    case kShapeSurd: {
        const double sw = SurdWidth(H, s), th = RuleThickness(s);
        const double hook = r <= 1.6 ? 0.51 * H : (0.816 + 0.20 * (r - 1.6)) * s;
        const double xb = 0.45 * sw, xr = xb + 0.014 * s, tx = 0.050 * s, iy = 0.115 * s;
        const double slope = (sw - xr) / (H - th);
        const double points[] = {0.040 * s, hook - 0.118 * s, 0.022 * s, hook - 0.092 * s, 0.150 * s, hook,
                                 xr - tx + slope * iy, iy, sw - tx + slope * th, H, sw + W, H,
                                 sw + W, H - th, sw, H - th, xr, 0.0, xb - 0.014 * s, 0.0,
                                 0.085 * s, hook - 0.075 * s};
        path.Polygon(points, 11, " h f");
        break;
    }
    case kShapeParen: {
        const double tm = std::min(0.062 + 0.014 * r, 0.13) * s, tt = 0.020 * s;
        const double xl = 0.07 * s, xr = W - 0.035 * s, wi = xr - xl, cy = 0.27 * H;
        const double ko = xr - tt - (wi - tt) / 0.75, ki = xr - (wi - tm) / 0.75;
        path.Move(xr - tt, H);
        path.Curve(ko, H - cy, ko, cy, xr - tt, 0.0);
        path.Line(xr, 0.0);
        path.Curve(ki, cy, ki, H - cy, xr, H);
        out += " h f";
        break;
    }
    case kShapeBracket:
    case kShapeFloor:
    case kShapeCeil: {
        const double tv = std::min(0.070 + 0.006 * r, 0.11) * s, ts = std::min(0.040 + 0.002 * r, 0.06) * s;
        const double x0 = 0.09 * s, x1 = W - 0.03 * s;
        double points[16];
        int n = 0;
        const auto add = [&](double x, double y) {
            points[n++] = x;
            points[n++] = y;
        };
        add(x0, 0.0);
        if (item.code != kShapeCeil) {
            add(x1, 0.0);
            add(x1, ts);
            add(x0 + tv, ts);
        } else {
            add(x0 + tv, 0.0);
        }
        if (item.code != kShapeFloor) {
            add(x0 + tv, H - ts);
            add(x1, H - ts);
            add(x1, H);
        } else {
            add(x0 + tv, H);
        }
        add(x0, H);
        path.Polygon(points, n / 2, " h f");
        break;
    }
    case kShapeBrace: {
        const double ts = std::min(0.066 + 0.008 * r, 0.11) * s, tt = 0.022 * s;
        const double xl = 0.06 * s, xr = W - 0.05 * s, xm = (xl + xr) / 2.0;
        const double q = std::min(0.25 * H, (0.40 + 0.06 * r) * s);
        const double A = xm - ts / 2.0, B = xm + ts / 2.0, T = H / 2.0;
        for (int half = 0; half < 2; ++half) {
            const double sign = half == 0 ? 1.0 : -1.0;
            const auto Y = [&](double v) { return T + sign * v; };
            path.Move(xr, Y(T));
            path.Curve(A + 0.25 * (xr - A), Y(T), A, Y(T - 0.30 * q), A, Y(T - q));
            path.Line(A, Y(q));
            path.Curve(A, Y(0.45 * q), A - 0.35 * (A - xl), Y(0.06 * q), xl, Y(0.0));
            path.Curve(xl + 0.75 * (B - xl), Y(0.02 * q), B, Y(0.35 * q), B, Y(q));
            path.Line(B, Y(T - q));
            path.Curve(B, Y(T - 0.50 * q), B + 0.30 * (xr - B), Y(T - tt), xr, Y(T - tt));
            out += " h f";
        }
        break;
    }
    case kShapeVert:
    case kShapeDblVert: {
        const double tv = 0.062 * s, gap = 0.13 * s;
        const int bars = item.code == kShapeDblVert ? 2 : 1;
        const double x0 = (W - bars * tv - (bars - 1) * gap) / 2.0;
        for (int i = 0; i < bars; ++i) {
            const double x = x0 + i * (tv + gap);
            const double points[] = {x, 0.0, x + tv, 0.0, x + tv, H, x, H};
            path.Polygon(points, 4, " h f");
        }
        break;
    }
    case kShapeAngle: {
        LineWidth(out, 0.052 * s, lineWidth);
        const double points[] = {W - 0.05 * s, H, 0.07 * s, H / 2.0, W - 0.05 * s, 0.0};
        path.Polygon(points, 3, " S");
        break;
    }
    case kShapeArrow: {   // a = length, c = style size; the shaft is stroked, the head filled
        LineWidth(out, 0.040 * s, lineWidth);
        if (a > 0.08 * s) {
            path.Move(0.0, 0.0);
            path.Line(a - 0.08 * s, 0.0);
            out += " S";
        }
        path.Move(a, 0.0);
        path.Curve(a - 0.06 * s, 0.01 * s, a - 0.12 * s, 0.05 * s, a - 0.17 * s, 0.085 * s);
        path.Line(a - 0.11 * s, 0.0);
        path.Line(a - 0.17 * s, -0.085 * s);
        path.Curve(a - 0.12 * s, -0.05 * s, a - 0.06 * s, -0.01 * s, a, 0.0);
        out += " h f";
        break;
    }
    case kShapeWideHat: {   // a = width, b = height
        const double points[] = {0.0, 0.0, a / 2.0, b, a, 0.0, a / 2.0, b - 0.055 * s};
        path.Polygon(points, 4, " h f");
        break;
    }
    case kShapeWideTilde:
        LineWidth(out, 0.050 * s, lineWidth);
        out += " 1 J";
        path.Move(0.0, 0.25 * b);
        path.Curve(0.22 * a, 1.25 * b, 0.38 * a, 0.95 * b, 0.5 * a, 0.55 * b);
        path.Curve(0.62 * a, 0.15 * b, 0.78 * a, -0.15 * b, a, 0.85 * b);
        out += " S 0 J";
        break;
    case kShapeFrame: {   // a = width, b = height, c = line width
        LineWidth(out, c, lineWidth);
        const double points[] = {0.0, 0.0, a, 0.0, a, b, 0.0, b};
        path.Polygon(points, 4, " h S");
        break;
    }
    case kShapeLine:      // a, b = delta, c = line width
        LineWidth(out, c, lineWidth);
        path.Move(0.0, 0.0);
        path.Line(a, b);
        out += " S";
        break;
    default: {            // kShapeCircle: centre, a = radius, c = line width
        LineWidth(out, c, lineWidth);
        const double k = 0.5523 * a;
        double ex = 1.0, ey = 0.0;
        path.Move(a, 0.0);
        for (int quarter = 0; quarter < 4; ++quarter) {
            const double nx = -ey, ny = ex;
            path.Curve(a * ex + k * nx, a * ey + k * ny, k * ex + a * nx, k * ey + a * ny, a * nx, a * ny);
            ex = nx;
            ey = ny;
        }
        out += " h S";
        break;
    }
    }
}

// One BT ... ET with every glyph: a run is a glyph item plus the joined items that follow it.
void EmitText(std::string& out, const std::vector<MathItem>& items, double ox, double oy, long long& lineWidth) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    out += " BT";
    unsigned font = 0, mode = 0;
    float size = -1.0f;
    long long lastX = 0, lastY = 0;
    bool transformed = false;   // the text line matrix is not a plain translation
    const size_t count = items.size();
    for (size_t i = 0; i < count;) {
        const MathItem& head = items[i];
        if (head.kind != kItemGlyph) {
            ++i;
            continue;
        }
        size_t end = i + 1;
        if (head.font != 5 && (head.flags & ~kItemJoin) == 0) {
            while (end < count && items[end].kind == kItemGlyph && items[end].flags == kItemJoin &&
                   items[end].font == head.font && items[end].a == head.a) {
                ++end;
            }
        }
        if (head.font != font || head.a != size) {
            font = head.font;
            size = head.a;
            out += " /M";
            out += static_cast<char>('0' + font);
            Number(out, size);
            out += " Tf";
        }
        const unsigned wanted = (head.flags & kItemHeavy) ? 2 : (head.flags & kItemOutline) ? 1 : 0;
        if (wanted != mode) {
            mode = wanted;
            out += ' ';
            out += static_cast<char>('0' + mode);
            out += " Tr";
        }
        if (mode != 0) LineWidth(out, (mode == 1 ? 0.025 : 0.03) * size, lineWidth);
        const long long x = std::llround((ox + head.x) * 100.0), y = std::llround((oy + head.y) * 100.0);
        const unsigned transform = head.flags >> 5;
        if ((head.flags & kItemSlant) || transform != 0 || transformed) {
            transformed = (head.flags & kItemSlant) || transform != 0;
            out += ' ';
            out += (head.flags & kItemSlant) ? "1 0 0.18 1" : kMathTransformOps[transform];
            Number(out, static_cast<double>(x) * 0.01);
            Number(out, static_cast<double>(y) * 0.01);
            out += " Tm";
        } else {
            Number(out, static_cast<double>(x - lastX) * 0.01);
            Number(out, static_cast<double>(y - lastY) * 0.01);
            out += " Td";
        }
        lastX = x;
        lastY = y;
        bool hex = head.font == 5;
        for (size_t k = i; k < end; ++k) hex = hex || items[k].code < 0x20 || items[k].code > 0x7E;
        out += hex ? " <" : " (";
        for (size_t k = i; k < end; ++k) {
            const unsigned char code = items[k].code;
            if (hex) {
                out += kHex[code >> 4];
                out += kHex[code & 15];
            } else {
                if (code == '(' || code == ')' || code == '\\') out += '\\';
                out += static_cast<char>(code);
            }
        }
        out += hex ? "> Tj" : ") Tj";
        i = end;
    }
    out += " ET";
}

} // namespace

// ---- public API ---------------------------------------------------------------------------------
std::string MathSymbolDescriptorObject() {
    return "<< /Type /FontDescriptor /FontName /Symbol /Flags 4 /FontBBox [-180 -293 1090 1010] "
           "/ItalicAngle 0 /Ascent 1010 /Descent -293 /CapHeight 673 /StemV 85 >>";
}

std::string MathFontObject(int index, int symbolDescriptorId) {
    std::string out;
    if (index < 0 || index >= kMathFontCount) return out;
    out = "<< /Type /Font /Subtype /Type1 /BaseFont /";
    out += kMathFonts[index].baseFont;
    if (index == 4) {
        out += " /FirstChar 32 /LastChar 254 /Widths [";
        for (int code = 0; code < 223; ++code) {
            if (code != 0) out += ' ';
            AppendFixed2(out, kMathGlyphMetrics[4][code].advance);
        }
        out += "] /FontDescriptor ";
        AppendFixed2(out, symbolDescriptorId);
        out += " 0 R";
    }
    out += " >>";
    return out;
}

double StandardTextWidth(std::string_view text, double size, StandardTextFont font) {
    unsigned long long units = 0;
    for (const char ch : text) {
        const unsigned index = static_cast<unsigned char>(ch) - 32u;
        if (index >= 224u) continue;
        const unsigned width = kStandardTextWidths[font == StandardTextFont::Bold][index];
        // A code without a glyph in WinAnsiEncoding has width 0 in every font.
        units += font == StandardTextFont::Mono && width != 0 ? 600u : width;
    }
    return static_cast<double>(units) * size / 1000.0;
}

static constexpr std::array<StandardWordAdvances, 3> MakeStandardWordAdvances() {
    std::array<StandardWordAdvances, 3> tables{};
    for (size_t font = 0; font < tables.size(); ++font) {
        StandardWordAdvances& table = tables[font];
        for (unsigned code = 0; code < 256; ++code) {
            uint16_t advance = 1;
            if (code >= 32) {
                const uint16_t width = kStandardTextWidths[font == 1][code - 32];
                if (width != 0) advance = font == 2 ? 600 : width;
            }
            table.byte[code] = advance;
        }
        table.space = table.byte[static_cast<unsigned char>(' ')];
        // The no-break space (0xA0) keeps its width: it joins the words beside it.
        for (const char white : {' ', '\t', '\r', '\n'}) table.byte[static_cast<unsigned char>(white)] = 0;
    }
    return tables;
}

const std::array<StandardWordAdvances, 3> kStandardWordAdvances = MakeStandardWordAdvances();

MathFormula MathFormula::Layout(std::string_view tex, double fontSize, bool display,
                                const MathFallbackFont* fallback, double maxWidth, bool bold,
                                double maxHeight) {
    MathParseResult parsed;
    ParseMath(tex, display, fallback != nullptr, parsed);
    return LayoutParsed(parsed, tex, fontSize, display, fallback, maxWidth, bold, maxHeight);
}

MathFormula MathFormula::LayoutParsed(const MathParseResult& parsed, std::string_view tex,
                                      double fontSize, bool display, const MathFallbackFont* fallback,
                                      double maxWidth, bool bold, double maxHeight) {
    MathFormula formula;
    if (parsed.nodes.empty() || (parsed.root == 0 && parsed.tag == 0)) return formula;
    const double base = fontSize >= 2.0 ? std::min(fontSize, 200.0) : 2.0;
    formula.fallback_ = fallback;
    formula.items_.reserve(std::min(parsed.nodes.size() + parsed.text.size() + 8, kMathMaxItems));
    Layouter layout{formula.items_, formula.text_, fallback, bold};

    const MathParseResult* arena = &parsed;
    MathParseResult sourceArena;
    formula.sourceFallback_ = parsed.sourceFallback;
    layout.Run(parsed, base, display);
    if (layout.overflow) {
        // A layout limit: the formula becomes its own source, like every parser limit.
        BuildMathSourceFallback(tex, fallback != nullptr, sourceArena);
        arena = &sourceArena;
        formula.sourceFallback_ = true;
        layout.Run(sourceArena, base, display);
    }

    // Fit: one re-layout at a smaller size (to 75 %), then a uniform scale; never below half
    // the nominal size. A source fallback is text and is not shrunk.
    double scale = 1.0;
    if (!formula.sourceFallback_) {
        const double first = layout.FitRatio(maxWidth, maxHeight);
        if (first < 1.0) {
            const double smaller = std::floor(base * (first >= 0.75 ? first * 0.995 : 0.75) * 100.0) / 100.0;
            layout.Run(*arena, smaller, display);
            const double second = layout.FitRatio(maxWidth, maxHeight);
            if (second < 1.0) {
                const double floorScale = std::min(1.0, std::ceil(50.0 * base / smaller - 1e-9) / 100.0);
                scale = std::max(floorScale, std::floor(second * 100.0 - 1e-9) / 100.0);
            }
        }
    }

    formula.width_ = scale * layout.natural;
    if (layout.hasTag && display && maxWidth > 0.0 && formula.width_ <= maxWidth) {
        // Flush right: the box takes the whole line, the body is centred when the gap allows.
        const double line = maxWidth / scale;
        const double gap = layout.natural - layout.bodyWidth - layout.tagWidth;
        double bodyX = (line - layout.bodyWidth) / 2.0;
        if (bodyX + layout.bodyWidth + gap > line - layout.tagWidth) {
            bodyX = std::max(0.0, line - layout.tagWidth - gap - layout.bodyWidth);
        }
        Box part;
        part.end = layout.tagFirst;
        layout.Shift(part, bodyX, 0.0);
        part.first = part.end;
        part.end = static_cast<uint32_t>(formula.items_.size());
        layout.Shift(part, line - layout.natural, 0.0);
        formula.width_ = maxWidth;
    }
    formula.ascent_ = scale * layout.height;
    formula.descent_ = scale * layout.drop;
    formula.scale_ = scale;
    formula.hasStroke_ = layout.stroke;
    return formula;
}

void MathFormula::Emit(std::string& content, double x, double baseline, const char* rgb) const {
    if (rgb == nullptr) rgb = "0 0 0";
    size_t glyphs = 0, paths = 0, fallbacks = 0;
    for (const MathItem& item : items_) {
        if (item.kind == kItemGlyph) ++glyphs;
        else if (item.kind == kItemFallbackText) ++fallbacks;
        else ++paths;
    }
    if (glyphs + paths != 0) {
        content += "q ";
        content += rgb;
        content += " rg";
        if (hasStroke_) {
            content += ' ';
            content += rgb;
            content += " RG";
        }
        double ox = x, oy = baseline;
        if (scale_ != 1.0) {
            // The uniform fit scale; it also carries the placement.
            Number(content, scale_);
            content += " 0 0";
            Number(content, scale_);
            Number(content, x);
            Number(content, baseline);
            content += " cm";
            ox = oy = 0.0;
        }
        long long lineWidth = -1;
        if (glyphs != 0) EmitText(content, items_, ox, oy, lineWidth);
        if (paths != 0) {
            for (const MathItem& item : items_) {
                if (item.kind == kItemRule) EmitRule(content, item, ox, oy, lineWidth);
                else if (item.kind == kItemShape) EmitShape(content, item, ox, oy, lineWidth);
            }
        }
        content += " Q\n";
    }
    if (fallbacks == 0 || fallback_ == nullptr || fallback_->emit == nullptr) return;
    for (const MathItem& item : items_) {
        if (item.kind != kItemFallbackText) continue;
        const size_t offset = std::min<size_t>(item.textOffset, text_.size());
        fallback_->emit(fallback_->context, content, std::string_view(text_).substr(offset, item.textLength),
                        x + scale_ * item.x, baseline + scale_ * item.y, scale_ * item.a, rgb);
    }
}

} // namespace TinyPdf::Internal
