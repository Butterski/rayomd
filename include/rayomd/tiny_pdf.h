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

enum class BuildError : uint8_t {
    None,
    // No longer returned: a document whose characters need a TrueType font that cannot be
    // found is drawn in the standard fonts, and BuildResult::missingCharacters counts what
    // they could not show.
    FontUnavailable = 1,
    SourceTooLarge = 4,
    InvalidSourceUtf8 = 5,
    ReversiblePdfTooLarge = 6,
};

struct BuildResult {
    BuildError error = BuildError::None;
    // Characters no available font could show, drawn as their base letter or as '?'.
    uint32_t missingCharacters = 0;

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
