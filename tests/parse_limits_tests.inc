// SPDX-License-Identifier: Apache-2.0

// Needs <chrono>, <iostream>, <string>, <string_view>, <vector>; uses math_markdown_tests::Repeat
// and ParseAllInline, so it is included after math_markdown_tests.inc, and PeakRssBytes of
// tests/core_tests.cpp.
// What the parser does with hostile input must stay linear in time and memory, and what it does
// with ordinary input must keep its bytes.
// Included inside the anonymous namespace of tests/core_tests.cpp.

namespace parse_limits_tests {

// The status symbols as they were rewritten before one pass did it: symbol by symbol, each
// replaced in place.
std::string NormalizeSymbolsReference(std::string value) {
    const std::pair<std::string_view, std::string_view> symbols[] = {
        { "\xE2\x9C\x85", "[OK]" }, { "\xE2\x9A\xA0\xEF\xB8\x8F", "[!]" }, { "\xE2\x9A\xA0", "[!]" },
        { "\xE2\x9D\x8C", "[X]" },
    };
    for (const auto& [from, to] : symbols) {
        for (size_t at = value.find(from); at != std::string::npos; at = value.find(from, at + to.size())) {
            value.replace(at, from.size(), to);
        }
    }
    return value;
}

// Runs `work` and fails when it takes longer than `seconds` or, where the peak resident size
// can be read, raises it by more than `megabytes`.
template <typename Work>
bool Bounded(const char* name, double seconds, size_t megabytes, Work work) {
    const size_t peakBefore = PeakRssBytes();
    const auto started = std::chrono::steady_clock::now();
    if (!work()) {
        std::cerr << name << ": wrong result" << std::endl;
        return false;
    }
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const size_t peakAfter = PeakRssBytes();
    const size_t grew = peakAfter > peakBefore ? peakAfter - peakBefore : 0;
    if (took > seconds || grew > megabytes * 1024 * 1024) {
        std::cerr << name << ": " << took << " s, peak +" << grew / (1024 * 1024) << " MB" << std::endl;
        return false;
    }
    return true;
}

bool CheckStatusSymbols() {
    using math_markdown_tests::Repeat;
    // The one pass gives what symbol-by-symbol replacing gave, also for broken and adjacent ones.
    const char* const pieces[] = {
        "\xE2\x9C\x85", "\xE2\x9A\xA0", "\xEF\xB8\x8F", "\xE2\x9D\x8C", "\xE2", "\x9C", "\xE2\x9A", "\x85", "a", " ",
    };
    uint64_t state = 0x2545F4914F6CDD1Dull;
    auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32_t>(state >> 16);
    };
    for (int round = 0; round < 3000; round++) {
        std::string text;
        const uint32_t count = next() % 24;
        for (uint32_t i = 0; i < count; i++) text += pieces[next() % 10];
        if (TinyPdf::Internal::NormalizeSymbols(text) != NormalizeSymbolsReference(text)) {
            std::cerr << "status symbols differ from symbol-by-symbol replacing" << std::endl;
            return false;
        }
    }
    // A paragraph of nothing but symbols: replaced in place, each moved the rest of the text,
    // 10^11 bytes for the 4 MB here (minutes); a paragraph of 256 KB took seconds to export.
    const std::string symbols = Repeat("\xE2\x9C\x85\xE2\x9D\x8C\xE2\x9A\xA0\xEF\xB8\x8F", 4 * 1024 * 1024 / 12);
    if (!Bounded("4 MB of status symbols", 5.0, 256, [&]() {
            const std::string normalized = TinyPdf::Internal::NormalizeSymbols(symbols);
            return normalized.size() == symbols.size() / 12 * 10 && normalized.compare(0, 10, "[OK][X][!]") == 0;
        })) {
        return false;
    }
    const std::string paragraph = Repeat("\xE2\x9C\x85", 256 * 1024 / 3) + "\n";
    return Bounded("a 256 KB paragraph of status symbols", 20.0, 512, [&]() {
        std::string pdf;
        return Build(paragraph, pdf);
    });
}

} // namespace parse_limits_tests

bool CheckParseLimits() {
    return parse_limits_tests::CheckStatusSymbols();
}
