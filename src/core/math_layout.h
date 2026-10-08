#pragma once

// Native TeX-subset math: box layout and PDF content-stream emission for one formula.
// Renderer-neutral; knows nothing about pages. Contract: docs/development/native_math.md.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

struct MathParseResult;   // math_parser.h

// ---- fonts -----------------------------------------------------------------------------
// PDF standard-14 fonts, never embedded. A renderer that emitted at least one formula adds
// kMathFontObjectCount objects -- MathSymbolDescriptorObject() first, then MathFontObject(0..4,
// descriptorId) -- and lists the five fonts under kMathFonts[i].resourceName in the /Font
// dictionary of every page. A document without a rendered formula adds nothing.
struct MathFontResource {
    const char* resourceName;   // /Font key without the slash: "M1" .. "M5"
    const char* baseFont;       // PDF standard-14 name
};
inline constexpr int kMathFontCount = 5;
inline constexpr int kMathFontObjectCount = 6;
inline constexpr MathFontResource kMathFonts[kMathFontCount] = {
    {"M1", "Times-Roman"}, {"M2", "Times-Italic"}, {"M3", "Times-Bold"},
    {"M4", "Times-BoldItalic"}, {"M5", "Symbol"}};

// Object body "<< /Type /FontDescriptor /FontName /Symbol /Flags 4 ... >>".
std::string MathSymbolDescriptorObject();
// Object body of the font dictionary of kMathFonts[index]. Index 4 (Symbol) carries
// /FirstChar /LastChar /Widths and references `symbolDescriptorId`; 0..3 ignore the id.
// An index outside 0..4 returns an empty string.
std::string MathFontObject(int index, int symbolDescriptorId);

// ---- exact widths of the standard renderer's text fonts -----------------------------------
// /F1 Helvetica, /F2 Helvetica-Bold, /F3 Courier, all in WinAnsiEncoding: the text is ASCII,
// or Latin text transcoded to WinAnsi codes 0x80..0xFF. Bytes below 32 and codes without a
// WinAnsi glyph count as zero width.
enum class StandardTextFont { Regular, Bold, Mono };
double StandardTextWidth(std::string_view text, double size, StandardTextFont font);

// The same advances by byte, for wrapping text a word at a time: index the array with
// static_cast<size_t>(font). byte[] is 0 for the four ASCII white-space bytes, so a single
// lookup both measures a byte and ends a word, and 1 for any other byte the font has no
// glyph for, which keeps that byte inside its word.
struct StandardWordAdvances {
    uint16_t space;         // advance of ' ', AFM units (1/1000 em)
    uint16_t byte[256];     // advance of each byte, AFM units
};
extern const std::array<StandardWordAdvances, 3> kStandardWordAdvances;

// ---- fallback for characters the standard fonts cannot show --------------------------------
// The Unicode renderer provides one, and the standard renderer for Latin text. The struct and
// its context must outlive every MathFormula laid out with it. `measure` is called during
// Layout, `emit` only during Emit.
struct MathFallbackFont {
    void* context;
    double (*measure)(void* context, std::string_view utf8, double size);
    void (*emit)(void* context, std::string& content, std::string_view utf8,
                 double x, double baseline, double size, const char* rgb);
    double ascent;    // ink height above the baseline (not the line ascent), in em of `size`
    double descent;   // ink depth below the baseline, in em of `size`, positive
};

// One positioned drawing primitive (glyph, rule, vector shape or fallback text run).
struct MathItem {                 // 24 bytes, trivially copyable
    float x, y;                   // relative to the formula origin (left end of the baseline), y up, points
    float a;                      // glyph, fallback text: font size | rule: width | shape: per shape
    union { float b; uint32_t textOffset; };   // rule: height | shape: per shape | fallback: offset into text_
    union { float c; uint32_t textLength; };   // shape: per shape | fallback: byte length
    uint8_t kind;
    uint8_t font;                 // glyph: 1..5
    uint8_t code;                 // glyph: character code | shape: shape id
    uint8_t flags;                // glyph: slant, outline, heavy, join, transform id | shape: mirror, transpose
};
static_assert(sizeof(MathItem) == 24, "MathItem is a 24-byte record");

// A laid-out formula. Default-constructible, movable, immutable after layout, safe to Emit many
// times and from several threads. Holds no reference to the TeX source or the node arena.
class MathFormula {
public:
    MathFormula() = default;

    // Parses and lays out `tex` (TeX math subset) at `fontSize` points. Never fails: malformed
    // input is recovered and a hard limit shows the formula's own source as upright text.
    //   display   display style (true) or text style (false).
    //   fallback  painter for non-ASCII text; nullptr makes such characters '?'.
    //   maxWidth  available width in points, 0 = unlimited.
    //   bold      bold context (heading, table header): bold Times faces and heavy Symbol glyphs.
    //   maxHeight available height (ascent + descent) in points, 0 = unlimited.
    // A formula that exceeds a limit is re-laid out smaller (to 75 %) and then scaled uniformly
    // (to 50 %) until it fits both. If even that is not enough, Width() stays above maxWidth or
    // Ascent() + Descent() above maxHeight, and the caller shows the source instead. A source
    // fallback is never shrunk.
    static MathFormula Layout(std::string_view tex, double fontSize, bool display,
                              const MathFallbackFont* fallback = nullptr, double maxWidth = 0.0,
                              bool bold = false, double maxHeight = 0.0);

    // Same, from an already parsed arena. `tex` is used only for the source fallback.
    // `parsed` must obey the arena rules of the contract, as every ParseMath result does.
    static MathFormula LayoutParsed(const MathParseResult& parsed, std::string_view tex,
                                    double fontSize, bool display,
                                    const MathFallbackFont* fallback = nullptr, double maxWidth = 0.0,
                                    bool bold = false, double maxHeight = 0.0);

    double Width() const { return width_; }      // points; includes the trailing italic correction
    double Ascent() const { return ascent_; }    // points above the baseline, >= 0
    double Descent() const { return descent_; }  // points below the baseline, >= 0
    // No extent at all (nothing was laid out): the caller shows the source. A formula with
    // height but no width (a strut such as \vphantom) is not empty; it paints nothing.
    bool Empty() const { return width_ <= 0.0 && ascent_ <= 0.0 && descent_ <= 0.0; }
    // A hard limit was hit: the formula is its own source as upright text (possibly cut).
    bool SourceFallback() const { return sourceFallback_; }

    // Appends content-stream operators that paint the formula with the left end of its baseline
    // at (x, baseline) in colour `rgb` ("r g b", used for fill and stroke). Uses only /M1../M5
    // and, for fallback text, the MathFallbackFont given to Layout. The box
    // [x, x + Width()] x [baseline - Descent(), baseline + Ascent()] covers everything drawn,
    // except that the first glyph may overhang the left edge by at most 0.19 em (italic f, j;
    // an accent wider than its letter); fallback text is boxed by the ascent and descent
    // given, whatever its ink. Leaves the graphics state as found.
    void Emit(std::string& content, double x, double baseline, const char* rgb) const;

private:
    friend struct MathFormulaInspector;          // item access for the layout harness only
    std::vector<MathItem> items_;
    std::string text_;                           // UTF-8 of fallback text items (usually empty)
    const MathFallbackFont* fallback_ = nullptr;
    double width_ = 0.0;
    double ascent_ = 0.0;
    double descent_ = 0.0;
    double scale_ = 1.0;                         // uniform fit scale applied by Emit (0.5 .. 1)
    bool hasStroke_ = false;
    bool sourceFallback_ = false;
};

} // namespace TinyPdf::Internal
