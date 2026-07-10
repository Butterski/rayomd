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

enum class BuildError : uint8_t {
    None,
    FontUnavailable,
};

struct BuildResult {
    BuildError error = BuildError::None;

    constexpr bool Ok() const { return error == BuildError::None; }
    constexpr explicit operator bool() const { return Ok(); }
};

struct PdfOptions {
    PdfStyle style = PdfStyle::Elegant;
    PdfMargin margin = PdfMargin::Normal();
    std::string sourcePath;
    bool enableUrlImages = false;
    bool allowUnsafeLocalImages = false;
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
};

BuildResult BuildPdf(const std::string& markdown, const PdfOptions& options, std::string& pdfBytes);

// Compatibility entry points for the pre-typed C++ API. They intentionally
// keep their established success/error behavior while all in-tree callers use
// BuildPdf().
bool BuildPdfBytes(const std::string& markdown, const BuildOptions& options, std::string& pdfBytes);
bool BuildPdfBytes(const std::string& markdown, int styleIdx, int marginIdx, std::string& pdfBytes);

} // namespace TinyPdf
