#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace RayoMd::Text {

inline void AppendFixed2(std::string& out, double value) {
    const bool negative = std::signbit(value);
    const uint64_t scaled = static_cast<uint64_t>(std::llround(std::fabs(value) * 100.0));
    const uint64_t whole = scaled / 100;
    const unsigned fraction = static_cast<unsigned>(scaled % 100);
    if (negative) out.push_back('-');
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), whole);
    out.append(buffer, result.ptr);
    if (fraction == 0) return;
    out.push_back('.');
    out.push_back(static_cast<char>('0' + fraction / 10));
    if (fraction % 10 != 0) out.push_back(static_cast<char>('0' + fraction % 10));
}

std::string Trim(std::string value);
std::vector<std::string> SplitLines(const std::string& text);
std::string FormatDouble(double value);
bool IsAsciiDocument(const std::string& text);

} // namespace RayoMd::Text