#pragma once

#include <string>
#include <vector>

namespace RayoMd::Text {

std::string Trim(std::string value);
std::vector<std::string> SplitLines(const std::string& text);
std::string FormatDouble(double value);
bool IsAsciiDocument(const std::string& text);

} // namespace RayoMd::Text