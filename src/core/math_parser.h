#pragma once

// TeX-subset math parser: source text -> flat node arena. Renderer- and layout-neutral.
// Contract: docs/development/native_math.md. Tables: math_symbols.inc (generated).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

// ---- hard limits (one outcome for all of them: the source fallback) ----------------
inline constexpr std::size_t kMathMaxSourceBytes = 16384;
inline constexpr int kMathMaxNodes = 8192;          // arena nodes, the null node 0 not counted
inline constexpr int kMathMaxDepth = 24;            // nested lists; the top-level list is 1
inline constexpr int kMathMaxRows = 64;
inline constexpr int kMathMaxColumns = 24;         // cells of one row; columns of a column specification
// ---- soft limits (clamped or ignored, never a fallback) ------------------------------
inline constexpr int kMathMaxPrimes = 8;
inline constexpr int kMathMaxEnvName = 32;
inline constexpr int kMathMaxRawArgBytes = 256;
inline constexpr int kMathMaxSpaceMilliEm = 20000;  // +-20 em
inline constexpr int kMathMaxSpaceCentiPt = 24000;  // +-240 pt
// ---- source fallback text ------------------------------------------------------------
inline constexpr std::size_t kMathFallbackScanBytes = 1024;
inline constexpr std::size_t kMathFallbackTextBytes = 200;

enum class MathClass : uint8_t { Ord, Op, Bin, Rel, Open, Close, Punct, Inner, None = 0xFF };

enum class MathNodeKind : uint8_t {
    Null,        // nodes[0] only
    Glyph, Composite, TextRun, Fallback, Space, Style,
    Group, Scripts, Fraction, Radical, LeftRight, Middle, SizedDelim,
    Accent, OverUnder, Stack, Enclose, Phantom, Text, OpName,
    Array, Row, Cell, Col
};

// MathNode::flags -- glyph rendering bits (Glyph; kGlyphHeavy also on Composite).
inline constexpr uint8_t kGlyphSlant = 0x01;       // text matrix 1 0 0.18 1
inline constexpr uint8_t kGlyphSmall = 0x02;       // drawn at 0.90 of the style size
inline constexpr uint8_t kGlyphOutline = 0x04;     // text render mode 1, line width 0.025 em
inline constexpr uint8_t kGlyphHeavy = 0x08;       // text render mode 2, line width 0.03 em
// MathNode::flags -- atom bits (any node whose cls is not None).
inline constexpr uint8_t kAtomNegated = 0x10;      // negation slash over the atom
inline constexpr uint8_t kAtomPad = 0x20;          // 5/18 em before and after (\implies \impliedby \iff)
// Glyph, Composite, OpName, Group (\mathop), Scripts: scripts become limits in display style.
inline constexpr uint8_t kOpDisplayLimits = 0x40;
// MathNode::flags -- kind-specific bits.
inline constexpr uint8_t kGroupInText = 0x80;      // Group: $...$ or \(...\) inside \text
inline constexpr uint8_t kOpNameUnknown = 0x80;    // OpName: unknown control word
inline constexpr uint8_t kSpaceAbsolute = 0x01;    // Space: a is 1/100 pt instead of 1/1000 em
inline constexpr uint8_t kFractionBar = 0x01;
inline constexpr uint8_t kFractionDisplay = 0x02;  // \dfrac \cfrac \dbinom
inline constexpr uint8_t kFractionText = 0x04;     // \tfrac \tbinom
inline constexpr uint8_t kArrayRulesBelowMask = 0x03;  // Array: \hline count below the last row, clamped to 3
inline constexpr uint8_t kRowTagNoParens = 0x01;   // Row: \tag*
// Array::aux.
inline constexpr uint8_t kArrayStyleMask = 0x03;   // minimum cell style level: 0 display, 1 text, 2 script
inline constexpr uint8_t kArrayCases = 0x20;       // cases dcases rcases: taller struts, no delimiter pad
inline constexpr uint8_t kArrayRowGap = 0x40;      // aligned/gathered/eqnarray families and implicit rows
inline constexpr uint8_t kArrayAligned = 0x80;     // r l r l ... column pairs

enum MathDelimiter : uint8_t {
    kDelimNone, kDelimLParen, kDelimRParen, kDelimLBrack, kDelimRBrack, kDelimLBrace, kDelimRBrace,
    kDelimVert, kDelimDblVert, kDelimLAngle, kDelimRAngle, kDelimLFloor, kDelimRFloor, kDelimLCeil,
    kDelimRCeil, kDelimSlash, kDelimBackslash, kDelimUpArrow, kDelimDownArrow, kDelimUpDownArrow,
    kDelimDblUpArrow, kDelimDblDownArrow, kDelimDblUpDownArrow, kDelimCount
};
enum MathAccent : uint8_t {
    kAccentHat, kAccentCheck, kAccentTilde, kAccentAcute, kAccentGrave, kAccentDot, kAccentDdot,
    kAccentBreve, kAccentBar, kAccentRing,                       // 0..9: Times accent glyphs
    kAccentVec, kAccentDddot, kAccentWideHat, kAccentWideTilde, kAccentCount
};
enum MathOverUnder : uint8_t {
    kOverLine, kUnderLine, kOverRightArrow, kOverLeftArrow, kOverLeftRightArrow, kOverBrace, kUnderBrace
};
enum MathEnclose : uint8_t { kEncloseBox, kEncloseCancel, kEncloseBCancel, kEncloseXCancel };
enum MathPhantom : uint8_t { kPhantomBoth, kPhantomWidth, kPhantomHeight };
enum MathBigOp : uint8_t { kBigOpNone, kBigOpSum, kBigOpIntegral, kBigOpCup, kBigOpVee, kBigOpCount };
enum MathLimitsMode : uint8_t { kLimitsDefault, kLimitsOn, kLimitsOff };
enum MathStackArrow : uint8_t { kStackNoArrow, kStackRightArrow, kStackLeftArrow };
enum MathColumnAlign : uint8_t { kColumnLeft, kColumnCentre, kColumnRight };

struct MathNode {                 // 16 bytes, trivially copyable; field meaning per kind: see the contract
    MathNodeKind kind = MathNodeKind::Null;
    MathClass cls = MathClass::None;   // final class (Bin->Ord already applied); None for non-atoms
    uint8_t flags = 0;
    uint8_t aux = 0;
    uint16_t a = 0;
    uint16_t b = 0;
    uint16_t next = 0;                 // next sibling in the same list, 0 = end
    uint16_t child[3] = {0, 0, 0};     // heads of up to three child lists, 0 = empty
};
static_assert(sizeof(MathNode) == 16, "MathNode is a 16-byte record");

struct MathParseResult {
    std::vector<MathNode> nodes;   // nodes[0] is the null node; indices fit uint16_t
    std::string text;              // bytes of TextRun (font codes) and Fallback (UTF-8) nodes
    uint16_t root = 0;             // head of the top-level list; 0 = empty formula
    uint16_t tag = 0;              // head of the formula tag's text items; 0 = no tag
    bool tagParens = true;         // false for \tag*
    bool sourceFallback = false;   // a hard limit was hit: root is the source as upright text
};

// Parses `tex` (UTF-8, without Markdown delimiters; display blocks keep their line feeds).
// Never fails and never throws: malformed input is recovered, and a hard limit yields the
// source fallback. Any byte sequence is accepted: control bytes (0x00-0x1F other than white
// space, and 0x7F) are ignored in every mode, and each byte that does not begin a well-formed
// UTF-8 sequence reads as U+FFFD. `out` is cleared first; its capacity is reused across calls.
// `hasFallbackFont` must be true exactly when the layout was given a MathFallbackFont.
void ParseMath(std::string_view tex, bool display, bool hasFallbackFont, MathParseResult& out);

// Replaces `out` by the source fallback of `tex`: one Text atom holding the source with runs
// of white space (space, \t, \n, \v, \f, \r) collapsed to one space and control bytes dropped,
// cut to kMathFallbackTextBytes on a character boundary and followed by " ..." when anything
// was cut. Sets out.sourceFallback. Used by ParseMath for its own limits and by the layout
// when a layout limit is hit.
void BuildMathSourceFallback(std::string_view tex, bool hasFallbackFont, MathParseResult& out);

} // namespace TinyPdf::Internal
