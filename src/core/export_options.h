#pragma once

#include "rayomd/tiny_pdf.h"

#include <string_view>

namespace TinyPdf::Internal {

bool ParsePdfStyle(std::string_view value, PdfStyle& style);
bool ParsePdfMargin(std::string_view value, PdfMargin& margin);

const char* PdfStyleName(PdfStyle style);
const char* PdfMarginName(const PdfMargin& margin);
double ResolveMarginPoints(const PdfMargin& margin);

PdfStyle PdfStyleFromLegacyIndex(int index);
PdfMargin PdfMarginFromLegacySetting(int setting);

PdfOptions PdfOptionsFromLegacy(const BuildOptions& options);

} // namespace TinyPdf::Internal