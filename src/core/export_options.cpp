#include "export_options.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace TinyPdf::Internal {
namespace {

std::string Lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return (char)std::tolower(ch); });
    return result;
}

PdfStyle StyleFromIndex(int index) {
    if (index <= 0) return PdfStyle::Elegant;
    if (index == 1) return PdfStyle::Modern;
    return PdfStyle::Tech;
}

PdfMargin MarginFromLegacy(int margin) {
    switch (margin) {
    case 0: return PdfMargin::Compact();
    case 1: return PdfMargin::Normal();
    case 2: return PdfMargin::Wide();
    default:
        return margin >= 1000
            ? PdfMargin::CustomPoints((double)(margin - 1000))
            : PdfMargin::Normal();
    }
}

} // namespace

bool ParsePdfStyle(std::string_view value, PdfStyle& style) {
    std::string normalized = Lower(value);
    if (normalized == "elegant" || normalized == "0") {
        style = PdfStyle::Elegant;
        return true;
    }
    if (normalized == "modern" || normalized == "1") {
        style = PdfStyle::Modern;
        return true;
    }
    if (normalized == "tech" || normalized == "2") {
        style = PdfStyle::Tech;
        return true;
    }
    return false;
}

bool ParsePdfMargin(std::string_view value, PdfMargin& margin) {
    std::string normalized = Lower(value);
    if (normalized == "compact") {
        margin = PdfMargin::Compact();
        return true;
    }
    if (normalized == "normal") {
        margin = PdfMargin::Normal();
        return true;
    }
    if (normalized == "wide") {
        margin = PdfMargin::Wide();
        return true;
    }
    if (normalized.rfind("margin=", 0) != 0) return false;

    const char* numberStart = normalized.c_str() + 7;
    char* end = nullptr;
    double points = std::strtod(numberStart, &end);
    if (end == numberStart || points <= 0.0) return false;
    if (*end == '\0' || std::strcmp(end, "in") == 0) points *= 72.0;
    else if (std::strcmp(end, "pt") != 0) return false;

    margin = PdfMargin::CustomPoints(points);
    return true;
}

const char* PdfStyleName(PdfStyle style) {
    switch (style) {
    case PdfStyle::Elegant: return "elegant";
    case PdfStyle::Modern: return "modern";
    case PdfStyle::Tech: return "tech";
    }
    return "elegant";
}

const char* PdfMarginName(const PdfMargin& margin) {
    switch (margin.preset) {
    case MarginPreset::Compact: return "compact";
    case MarginPreset::Normal: return "normal";
    case MarginPreset::Wide: return "wide";
    case MarginPreset::Custom: return "custom";
    }
    return "normal";
}

double ResolveMarginPoints(const PdfMargin& margin) {
    switch (margin.preset) {
    case MarginPreset::Compact: return 34.0;
    case MarginPreset::Normal: return 54.0;
    case MarginPreset::Wide: return 72.0;
    case MarginPreset::Custom:
        return std::max(18.0, std::min(144.0, margin.customPoints));
    }
    return 54.0;
}

PdfStyle PdfStyleFromLegacyIndex(int index) {
    return StyleFromIndex(index);
}

PdfMargin PdfMarginFromLegacySetting(int setting) {
    return MarginFromLegacy(setting);
}

PdfOptions PdfOptionsFromLegacy(const BuildOptions& options) {
    PdfOptions result;
    result.style = PdfStyleFromLegacyIndex(options.styleIdx);
    result.margin = PdfMarginFromLegacySetting(options.marginIdx);
    result.sourcePath = options.sourcePath;
    result.enableUrlImages = options.enableUrlImages;
    result.allowUnsafeLocalImages = options.allowUnsafeLocalImages;
    result.embedSource = options.embedSource;
    return result;
}

} // namespace TinyPdf::Internal