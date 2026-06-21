#pragma once

#include <string>
#include <vector>

namespace TinyPdf {

extern int g_lastError;

struct BuildOptions {
    int styleIdx = 0;
    int marginIdx = 1;
    std::string sourcePath;
    bool enableUrlImages = false;
    bool allowUnsafeLocalImages = false;
};

std::string Trim(std::string s);
std::vector<std::string> SplitLines(const std::string& text);
std::string F(double v);
bool IsAsciiDocument(const std::string& s);
bool BuildPdfBytes(const std::string& markdown, const BuildOptions& options, std::string& pdfBytes);
bool BuildPdfBytes(const std::string& markdown, int styleIdx, int marginIdx, std::string& pdfBytes);

} // namespace TinyPdf
