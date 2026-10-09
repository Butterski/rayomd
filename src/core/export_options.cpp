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

bool ParsePageSize(std::string_view value, PdfPageSize& size) {
    std::string normalized = Lower(value);
    constexpr std::string_view kLandscape = "-landscape";
    const bool landscape = normalized.size() > kLandscape.size() &&
        normalized.compare(normalized.size() - kLandscape.size(), kLandscape.size(), kLandscape) == 0;
    if (landscape) normalized.resize(normalized.size() - kLandscape.size());
    PdfPageSize parsed;
    if (normalized == "a4") parsed = PdfPageSize::A4();
    else if (normalized == "a3") parsed = PdfPageSize::A3();
    else if (normalized == "a5") parsed = PdfPageSize::A5();
    else if (normalized == "letter") parsed = PdfPageSize::Letter();
    else if (normalized == "legal") parsed = PdfPageSize::Legal();
    else {
        if (landscape) return false;  // a custom size states its own orientation
        const char* widthStart = normalized.c_str();
        char* end = nullptr;
        const double width = std::strtod(widthStart, &end);
        if (end == widthStart || *end != 'x') return false;
        const char* heightStart = end + 1;
        const double height = std::strtod(heightStart, &end);
        if (end == heightStart) return false;
        double unit = 0.0;
        if (std::strcmp(end, "mm") == 0) unit = 72.0 / 25.4;
        else if (std::strcmp(end, "cm") == 0) unit = 72.0 / 2.54;
        else if (std::strcmp(end, "in") == 0) unit = 72.0;
        else if (std::strcmp(end, "pt") == 0) unit = 1.0;
        else return false;
        parsed = { width * unit, height * unit };
        const auto inRange = [](double side) { return side >= kMinPageSide && side <= kMaxPageSide; };
        if (!inRange(parsed.width) || !inRange(parsed.height)) return false;
    }
    if (landscape) std::swap(parsed.width, parsed.height);
    size = parsed;
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

PdfPageSize ResolvePageSize(const PdfPageSize& size) {
    // Not ">=" fails for NaN too, which then gets the smallest side.
    const auto side = [](double points) { return points >= kMinPageSide ? std::min(points, kMaxPageSide) : kMinPageSide; };
    return { side(size.width), side(size.height) };
}

double ResolveMarginPoints(const PdfMargin& margin, const PdfPageSize& page) {
    return std::min(ResolveMarginPoints(margin), (std::min(page.width, page.height) - 72.0) * 0.5);
}

bool ParseColor(std::string_view value, int32_t& rgb) {
    if ((value.size() != 4 && value.size() != 7) || value.front() != '#') return false;
    uint32_t parsed = 0;
    for (size_t index = 1; index < value.size(); index++) {
        const char ch = value[index];
        uint32_t digit = 0;
        if (ch >= '0' && ch <= '9') digit = (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') digit = (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') digit = (uint32_t)(ch - 'A' + 10);
        else return false;
        parsed = parsed << 4 | digit;
        if (value.size() == 4) parsed = parsed << 4 | digit;   // "#RGB" doubles each digit
    }
    rgb = (int32_t)parsed;
    return true;
}

bool ParseTheme(std::string_view text, std::string_view directory, PdfTheme& theme, std::string& error) {
    const auto trim = [](std::string_view value) {
        while (!value.empty() && std::isspace((unsigned char)value.front())) value.remove_prefix(1);
        while (!value.empty() && std::isspace((unsigned char)value.back())) value.remove_suffix(1);
        return value;
    };
    // A relative path is the theme file's: joined to its folder.
    const auto path = [directory](std::string_view value) {
        const bool absolute = !value.empty() && (value.front() == '/' || value.front() == '\\' ||
            (value.size() > 1 && value[1] == ':'));
        std::string joined;
        if (!absolute && !directory.empty() && !value.empty()) {
            joined = directory;
            if (joined.back() != '/' && joined.back() != '\\') joined += '/';
        }
        joined += value;
        return joined;
    };
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.remove_prefix(3);
    size_t lineNumber = 0;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = trim(text.substr(at, end - at));
        at = end + 1;
        lineNumber++;
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const std::string where = "line " + std::to_string(lineNumber) + ": ";
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            error = where + "expected key = value";
            return false;
        }
        const std::string key = Lower(trim(line.substr(0, equals)));
        std::string_view value = trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        int32_t* color = key == "heading-color" ? &theme.headingColor : key == "link-color" ? &theme.linkColor
            : key == "accent-color" ? &theme.accentColor : nullptr;
        std::string* field = key == "header-left" ? &theme.headerLeft : key == "header-center" ? &theme.headerCenter
            : key == "header-right" ? &theme.headerRight : key == "footer-left" ? &theme.footerLeft
            : key == "footer-center" ? &theme.footerCenter : key == "footer-right" ? &theme.footerRight : nullptr;
        if (color) {
            if (!ParseColor(value, *color)) {
                error = where + key + " must be a colour such as #0B3D91";
                return false;
            }
        } else if (field) {
            *field = value;
        } else if (key == "font" || key == "logo") {
            (key == "font" ? theme.fontPath : theme.logoPath) = path(value);
        } else if (key == "cover") {
            const std::string flag = Lower(value);
            if (flag != "yes" && flag != "no" && flag != "true" && flag != "false") {
                error = where + "cover must be yes or no";
                return false;
            }
            theme.cover = flag == "yes" || flag == "true";
        } else {
            error = where + "unknown key '" + key + "'";
            return false;
        }
    }
    return true;
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