#pragma once

#include <string>
#include <vector>

namespace TinyPdf {

extern int g_lastError;

std::string Trim(std::string s);
std::vector<std::string> SplitLines(const std::string& text);
std::string F(double v);
bool IsAsciiDocument(const std::string& s);
bool BuildPdfBytes(const std::string& markdown, int styleIdx, int marginIdx, std::string& pdfBytes);

} // namespace TinyPdf
