#pragma once

#include "rayomd/tiny_pdf.h"

#include <string>
#include <string_view>

namespace TinyPdf::Internal {

// The sides PdfPageSize keeps between, in points: 2 inches and 200 inches (PDF's own limit).
constexpr double kMinPageSide = 144.0;
constexpr double kMaxPageSide = 14400.0;

bool ParsePdfStyle(std::string_view value, PdfStyle& style);
bool ParsePdfMargin(std::string_view value, PdfMargin& margin);
// "a4", "a3", "a5", "letter" or "legal", each also with "-landscape", or "WIDTHxHEIGHT" with a
// unit, mm, cm, in or pt ("210x297mm", "8.5x11in"), in any case. False for anything else or a
// side outside [kMinPageSide, kMaxPageSide].
bool ParsePageSize(std::string_view value, PdfPageSize& size);
// "#RRGGBB" or "#RGB", as 0xRRGGBB.
bool ParseColor(std::string_view value, int32_t& rgb);
// A theme file: "key = value" lines (a value may be in double quotes), blank lines, and lines
// starting with '#' or ';'. Keys: font, logo, heading-color, link-color, accent-color,
// header-left, header-center, header-right, footer-left, footer-center, footer-right, cover
// (yes or no). Relative font and logo paths are taken in `directory`, the theme file's folder
// (UTF-8; empty for the current one). False, with `error` naming the line, for an unknown key or
// a value that does not parse.
bool ParseTheme(std::string_view text, std::string_view directory, PdfTheme& theme, std::string& error);

const char* PdfStyleName(PdfStyle style);
const char* PdfMarginName(const PdfMargin& margin);
double ResolveMarginPoints(const PdfMargin& margin);
// The page size with each side kept within [kMinPageSide, kMaxPageSide].
PdfPageSize ResolvePageSize(const PdfPageSize& size);
// The margin on a page of the resolved size `page`, narrowed to leave 72 points of text.
double ResolveMarginPoints(const PdfMargin& margin, const PdfPageSize& page);

PdfStyle PdfStyleFromLegacyIndex(int index);
PdfMargin PdfMarginFromLegacySetting(int setting);

PdfOptions PdfOptionsFromLegacy(const BuildOptions& options);

} // namespace TinyPdf::Internal