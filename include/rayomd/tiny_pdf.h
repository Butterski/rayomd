#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace TinyPdf {

enum class PdfStyle : uint8_t {
    Elegant,
    Modern,
    Tech,
};

enum class MarginPreset : uint8_t {
    Compact,
    Normal,
    Wide,
    Custom,
};

struct PdfMargin {
    MarginPreset preset = MarginPreset::Normal;
    double customPoints = 72.0;

    static PdfMargin Compact() { return { MarginPreset::Compact, 0.0 }; }
    static PdfMargin Normal() { return { MarginPreset::Normal, 0.0 }; }
    static PdfMargin Wide() { return { MarginPreset::Wide, 0.0 }; }
    static PdfMargin CustomPoints(double points) { return { MarginPreset::Custom, points }; }
};

// Page width and height in points (1/72 inch). Each side is kept between 144 and 14,400
// points, and a margin too wide for the page shrinks to leave 72 points of text.
struct PdfPageSize {
    double width = 595.0;
    double height = 842.0;

    static PdfPageSize A4() { return { 595.0, 842.0 }; }
    static PdfPageSize A3() { return { 842.0, 1191.0 }; }
    static PdfPageSize A5() { return { 420.0, 595.0 }; }
    static PdfPageSize Letter() { return { 612.0, 792.0 }; }
    static PdfPageSize Legal() { return { 612.0, 1008.0 }; }
};

// A company look over the style. Empty or unset fields leave the style as it is.
struct PdfTheme {
    static constexpr int32_t kStyleColor = -1;

    // A TrueType font (.ttf, or .ttc for its first face) for all text, embedded as a subset; the
    // standard fonts are then not used. Bold and italic are drawn from it.
    std::string fontPath;
    // Colours as 0xRRGGBB, or kStyleColor.
    int32_t headingColor = kStyleColor;
    int32_t linkColor = kStyleColor;
    int32_t accentColor = kStyleColor;   // rules and the bar beside block quotes
    // Text in the top and bottom margin of every page, at its left, centre and right, cut short
    // with an ellipsis when it does not fit. {title}, {author}, {subject}, {date}, {page} and
    // {pages} are replaced from the front matter and the page; a field that is just {logo}
    // shows the logo. With pageNumbers and no footerCenter, the footer centre is "{page} / {pages}".
    std::string headerLeft;
    std::string headerCenter;
    std::string headerRight;
    std::string footerLeft;
    std::string footerCenter;
    std::string footerRight;
    // A PNG or JPEG image for {logo} and the cover. One that cannot be read counts as a failed
    // image.
    std::string logoPath;
    // A first page with the logo, the title, the subject, the author and the date of the front
    // matter; {page} and {pages} count the pages after it.
    bool cover = false;
};

enum class BuildError : uint8_t {
    None,
    // No longer returned: a document whose characters need a TrueType font that cannot be
    // found is drawn in the standard fonts, and BuildResult::missingCharacters counts what
    // they could not show.
    FontUnavailable = 1,
    SourceTooLarge = 4,
    InvalidSourceUtf8 = 5,
    ReversiblePdfTooLarge = 6,
    // PdfTheme::fontPath cannot be read or is no TrueType font.
    ThemeFontUnavailable = 7,
    // PdfOptions::pdfa, but no TrueType font to embed: neither RAYOMD_FONT nor a system font.
    PdfaFontUnavailable = 8,
};

struct BuildResult {
    BuildError error = BuildError::None;
    // Characters no available font could show, drawn as their base letter or as '?'.
    uint32_t missingCharacters = 0;
    // Pages in the PDF.
    uint32_t pages = 0;
    // Standalone images that could not be loaded or decoded and show their fallback text.
    uint32_t failedImages = 0;

    constexpr bool Ok() const { return error == BuildError::None; }
    constexpr explicit operator bool() const { return Ok(); }
};

struct PdfOptions {
    PdfStyle style = PdfStyle::Elegant;
    PdfMargin margin = PdfMargin::Normal();
    PdfPageSize pageSize = PdfPageSize::A4();
    std::string sourcePath;
    bool enableUrlImages = false;
    bool allowUnsafeLocalImages = false;
    bool embedSource = false;
    // "N / M" centred in the bottom margin of every page.
    bool pageNumbers = false;
    // FlateDecode page content, font program and CMaps where that makes the file smaller:
    // text-heavy PDFs two to five times smaller, at the cost of compressing them.
    bool compress = false;
    // PDF/A-3b (ISO 19005-3): every font embedded, so all text takes a TrueType font and
    // formulas show their TeX source; an sRGB output intent, XMP metadata and a file identifier.
    bool pdfa = false;
    PdfTheme theme;
};

// Legacy error reporting is retained for the bool-returning compatibility
// overloads. New callers should use BuildPdf() and inspect BuildResult.
int GetLastError();

// Legacy raw-index options. New callers should use PdfOptions, PdfStyle, and
// PdfMargin so custom margins never need an integer encoding.
struct BuildOptions {
    int styleIdx = 0;
    int marginIdx = 1;
    std::string sourcePath;
    bool enableUrlImages = false;
    bool allowUnsafeLocalImages = false;
    bool embedSource = false;
};

BuildResult BuildPdf(const std::string& markdown, const PdfOptions& options, std::string& pdfBytes);

// Compatibility entry points for the pre-typed C++ API. They intentionally
// keep their established success/error behavior while all in-tree callers use
// BuildPdf().
bool BuildPdfBytes(const std::string& markdown, const BuildOptions& options, std::string& pdfBytes);
bool BuildPdfBytes(const std::string& markdown, int styleIdx, int marginIdx, std::string& pdfBytes);

} // namespace TinyPdf
