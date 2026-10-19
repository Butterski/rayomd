// SPDX-License-Identifier: Apache-2.0

#include "rayomd/tiny_pdf.h"
#include "../src/core/access_policy.h"
#include "../src/common/batch_report.h"
#include "../src/common/exact_math.h"
#include "../src/common/file_io.h"
#include "../src/core/flate.h"
#include "../src/core/image_encoder.h"
#include "../src/core/inflate.h"
#include "../src/common/text_utils.h"
#include "../src/core/export_options.h"
#include "../src/core/long_page.h"
#include "../src/core/inline_markdown.h"
#include "../src/core/markdown_parser.h"
#include "../src/core/math_layout.h"
#include "../src/core/math_parser.h"
#include "../src/core/rayomd_pdf_source.h"
#include "../src/core/pdfa.h"
#include "../src/core/highlight.h"
#include "../src/core/contents.h"
#include "../src/core/dialect.h"
#include "../src/core/embeds.h"
#include "../src/core/vault.h"
#include "../src/core/book.h"
#include "../src/core/png_filter.h"
#include "../src/core/cff.h"
#include "../src/core/html.h"
#include "../src/core/svg.h"
#include "../src/core/mermaid.h"
#include "../src/core/print_colors.h"
#include "../src/core/theme.h"
#include "../src/core/word_bounds.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "libdeflate.h"
#if defined(__linux__)
#include <sys/resource.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef RAYOMD_TEST_SOURCE_DIR
#define RAYOMD_TEST_SOURCE_DIR "."
#endif

namespace {
bool ValidPdf(const std::string& pdf) {
    return pdf.rfind("%PDF-", 0) == 0 && pdf.find("%%EOF") != std::string::npos &&
        pdf.find("/Type /Page") != std::string::npos;
}
bool CheckFormatter() {
    struct Case { double value; const char* expected; };
    const Case cases[] = {
        {0.0, "0"}, {-0.0, "-0"}, {1.0, "1"}, {-1.25, "-1.25"},
        {12.30, "12.3"}, {12.345, "12.35"}, {595.0, "595"}, {0.004, "0"},
        // Halves round away from zero on the value the double holds, whatever its size.
        {0.005, "0.01"}, {0.0049, "0"}, {0.995, "1"}, {0.994, "0.99"}, {2.675, "2.68"}, {9.995, "9.99"},
        {99.995, "100"}, {999.995, "1000"}, {9999.99, "9999.99"}, {9999.995, "10000"}, {10000.0, "10000"},
        {12345.678, "12345.68"}, {841.89, "841.89"}, {-841.895, "-841.9"}, {999999.99, "999999.99"},
        {1000000.0, "1000000"}, {123456789.125, "123456789.13"}, {-0.004, "-0"}, {-0.005, "-0.01"},
        {0.1 + 0.2, "0.3"}, {1.005, "1"}, {4503599627370.5, "4503599627370.5"}, {1e15, "1000000000000000"}
    };
    for (const Case& item : cases) {
        std::string actual;
        RayoMd::Text::AppendFixed2(actual, item.value);
        if (actual != item.expected) {
            std::cerr << "formatter mismatch: " << actual << " != " << item.expected << std::endl;
            return false;
        }
    }
    return true;
}
bool Build(const std::string& markdown, std::string& output) {
    TinyPdf::PdfOptions options;
    options.style = TinyPdf::PdfStyle::Modern;
    options.margin = TinyPdf::PdfMargin::Normal();
    options.sourcePath = std::string(RAYOMD_TEST_SOURCE_DIR) + "/tester.md";
    return TinyPdf::BuildPdf(markdown, options, output).Ok() && ValidPdf(output);
}
bool BuildReversible(const std::string& markdown, std::string& output) {
    TinyPdf::PdfOptions options;
    options.style = TinyPdf::PdfStyle::Modern;
    options.margin = TinyPdf::PdfMargin::Normal();
    options.sourcePath = std::string(RAYOMD_TEST_SOURCE_DIR) + "/tester.md";
    options.embedSource = true;
    return TinyPdf::BuildPdf(markdown, options, output).Ok() && ValidPdf(output);
}

size_t CountOccurrences(const std::string& value, std::string_view needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        count++;
        position += needle.size();
    }
    return count;
}

std::string InlineVisible(std::string_view input) {
    std::string visible;
    for (const TinyPdf::Internal::InlineSpan& span : TinyPdf::Internal::ParseInlineSpans(input)) {
        visible += span.text;
    }
    return visible;
}

bool FindLinkRectangle(const std::string& pdf, std::string_view url, std::array<double, 4>& rectangle) {
    std::string uri = "/URI (";
    uri.append(url);
    uri.push_back(')');
    size_t uriPosition = pdf.find(uri);
    if (uriPosition == std::string::npos) return false;
    size_t rectanglePosition = pdf.rfind("/Rect [", uriPosition);
    if (rectanglePosition == std::string::npos) return false;
    size_t valuesStart = rectanglePosition + 7;
    size_t valuesEnd = pdf.find(']', valuesStart);
    if (valuesEnd == std::string::npos || valuesEnd > uriPosition) return false;
    std::istringstream values(pdf.substr(valuesStart, valuesEnd - valuesStart));
    for (double& coordinate : rectangle) {
        if (!(values >> coordinate)) return false;
    }
    return rectangle[0] >= 0.0 && rectangle[1] >= 0.0 &&
        rectangle[2] > rectangle[0] && rectangle[3] > rectangle[1] &&
        rectangle[2] <= 595.0 && rectangle[3] <= 842.0;
}

bool CheckClassicMarkdownPhaseOne() {
    using TinyPdf::Internal::Block;
    using TinyPdf::Internal::BlockType;
    using TinyPdf::Internal::InlineSpan;
    const std::string source =
        "Primary title\n=====\n\nSecondary title\n-----\n\n"
        "[zero][target], [one] [target], [Target][], <https://example.net>, and <person@example.net>.\n\n"
        "    alpha\n      beta\n\n"
        "first line  \nsecond line\n\n"
        "![Rayo][logo]\n\n"
        "[target]: <https://example.com/path>  \"Optional title\"\n"
        "[logo]: docs/assets/branding/rayomd.png\n";
    std::vector<Block> blocks = TinyPdf::Internal::ParseMarkdown(source);
    if (blocks.size() != 6 || blocks[0].type != BlockType::Heading || blocks[0].level != 1 ||
        blocks[1].type != BlockType::Heading || blocks[1].level != 2 ||
        blocks[2].type != BlockType::Paragraph || blocks[3].type != BlockType::Code ||
        blocks[3].text != "alpha\n  beta" || blocks[4].type != BlockType::Paragraph ||
        blocks[4].text != "first line\nsecond line" || blocks[5].type != BlockType::Image ||
        !blocks[5].image || blocks[5].image->src != "docs/assets/branding/rayomd.png") {
        std::cerr << "classic block parsing mismatch" << std::endl;
        return false;
    }

    std::vector<InlineSpan> spans = TinyPdf::Internal::ParseInlineSpans(blocks[2].text);
    std::vector<std::string> urls;
    for (const InlineSpan& span : spans) if (!span.url.empty()) urls.emplace_back(span.url.View());
    const std::vector<std::string> expectedUrls = {
        "https://example.com/path", "https://example.com/path", "https://example.com/path",
        "https://example.net", "mailto:person@example.net"
    };
    if (urls != expectedUrls) {
        std::cerr << "classic link resolution mismatch" << std::endl;
        return false;
    }

    spans = TinyPdf::Internal::ParseInlineSpans("``literal ` tick`` and \\*literal\\* \\a");
    bool foundCode = false;
    std::string visible;
    for (const InlineSpan& span : spans) {
        foundCode = foundCode || (span.code && span.text == "literal ` tick");
        visible += span.text;
    }
    if (!foundCode || visible != "literal ` tick and *literal* \\a") {
        std::cerr << "code-span or escape semantics mismatch: " << visible << std::endl;
        return false;
    }

    std::string pdf;
    if (!Build(source, pdf) || CountOccurrences(pdf, "/Subtype /Link") != expectedUrls.size() ||
        pdf.find("/Subtype /Image") == std::string::npos) {
        std::cerr << "classic PDF annotation/image smoke mismatch" << std::endl;
        return false;
    }
    std::array<double, 4> referenceRectangle{};
    std::array<double, 4> urlRectangle{};
    std::array<double, 4> emailRectangle{};
    if (!FindLinkRectangle(pdf, "https://example.com/path", referenceRectangle) ||
        !FindLinkRectangle(pdf, "https://example.net", urlRectangle) ||
        !FindLinkRectangle(pdf, "mailto:person@example.net", emailRectangle)) {
        std::cerr << "classic PDF link rectangle mismatch" << std::endl;
        return false;
    }
    const std::string unicodeReference =
        u8"Zażółć [odnośnik][unicode].\n\n[unicode]: https://example.com/unicode\n";
    std::string unicodePdf;
    if (!Build(unicodeReference, unicodePdf) || CountOccurrences(unicodePdf, "/Subtype /Link") != 1) {
        std::cerr << "Unicode reference-link smoke mismatch" << std::endl;
        return false;
    }
    const std::string missingReferenceImage =
        "![missing][asset]\n\n[asset]: docs/assets/branding/does-not-exist.png\n";
    std::string missingPdf;
    if (!Build(missingReferenceImage, missingPdf) || missingPdf.find("/Subtype /Image") != std::string::npos) {
        std::cerr << "reference-image fallback smoke mismatch" << std::endl;
        return false;
    }
    return true;
}

bool CheckStructuredContainers() {
    using TinyPdf::Internal::Block;
    using TinyPdf::Internal::BlockType;
    const std::string quoteSource =
        "> first paragraph\n"
        ">\n"
        "> second [paragraph][ref]\n"
        "> > nested quote\n"
        "> - nested item\n"
        ">   continuation\n"
        ">\n"
        "> ## child heading\n"
        ">\n"
        ">     quoted code\n\n"
        "[ref]: https://example.com/nested\n";
    std::vector<Block> quoteBlocks = TinyPdf::Internal::ParseMarkdown(quoteSource);
    if (quoteBlocks.size() != 1 || quoteBlocks[0].type != BlockType::Quote ||
        quoteBlocks[0].children.size() != 6 ||
        quoteBlocks[0].children[0].type != BlockType::Paragraph ||
        quoteBlocks[0].children[1].type != BlockType::Paragraph ||
        quoteBlocks[0].children[2].type != BlockType::Quote ||
        quoteBlocks[0].children[2].children.empty() ||
        quoteBlocks[0].children[3].type != BlockType::Bullet ||
        quoteBlocks[0].children[3].text != "nested item continuation" ||
        quoteBlocks[0].children[4].type != BlockType::Heading ||
        quoteBlocks[0].children[5].type != BlockType::Code) {
        std::cerr << "structured blockquote mismatch" << std::endl;
        return false;
    }

    const std::string listSource =
        "- first line\n"
        "  wrapped continuation\n"
        "\n"
        "    second paragraph\n"
        "\n"
        "    > child quote\n"
        "\n"
        "        code line\n"
        "\n"
        "    - nested item\n"
        "- sibling\n";
    std::vector<Block> listBlocks = TinyPdf::Internal::ParseMarkdown(listSource);
    if (listBlocks.size() != 2 || listBlocks[0].type != BlockType::Bullet ||
        listBlocks[0].text != "first line wrapped continuation" || listBlocks[0].children.size() < 4 ||
        listBlocks[0].children[0].type != BlockType::Paragraph ||
        listBlocks[0].children[1].type != BlockType::Quote ||
        listBlocks[0].children[2].type != BlockType::Code ||
        listBlocks[0].children[3].type != BlockType::Bullet ||
        listBlocks[1].type != BlockType::Bullet || listBlocks[1].text != "sibling") {
        std::cerr << "structured list mismatch" << std::endl;
        return false;
    }

    std::string deeplyNested(24, '>');
    deeplyNested += " bounded\n";
    std::vector<Block> deepBlocks = TinyPdf::Internal::ParseMarkdown(deeplyNested);
    size_t observedDepth = 0;
    const Block* nested = deepBlocks.empty() ? nullptr : &deepBlocks.front();
    while (nested && nested->type == BlockType::Quote) {
        observedDepth++;
        nested = nested->children.empty() ? nullptr : &nested->children.front();
    }
    if (observedDepth < 8 || observedDepth > 9) {
        std::cerr << "container nesting bound mismatch: " << observedDepth << std::endl;
        return false;
    }

    std::string pdf;
    if (!Build(quoteSource + "\n" + listSource, pdf) ||
        CountOccurrences(pdf, "/Subtype /Link") != 1) {
        std::cerr << "structured container PDF smoke mismatch" << std::endl;
        return false;
    }
    return true;
}

bool CheckClassicInlineCases() {
    using TinyPdf::Internal::InlineSpan;
    struct VisibleCase { const char* input; const char* visible; };
    const VisibleCase visibleCases[] = {
        {"__strong__", "strong"},
        {"_emphasis_", "emphasis"},
        {"***combined***", "combined"},
        {"**outer _inner_**", "outer inner"},
        {"word_with_internal_underscores", "word_with_internal_underscores"},
        {"unmatched * marker", "unmatched * marker"},
        {"\\q preserves slash", "\\q preserves slash"},
        {"\\> also preserves slash", "\\> also preserves slash"},
        {"[missing][] and [good](https://example.com)", "[missing][] and good"},
        {"before ![alt](missing.png) after", "before image: alt after"},
    };
    for (const VisibleCase& item : visibleCases) {
        std::vector<InlineSpan> spans = TinyPdf::Internal::ParseInlineSpans(item.input);
        std::string visible;
        for (const InlineSpan& span : spans) visible += span.text;
        if (visible != item.visible) {
            std::cerr << "inline visible mismatch: " << visible << " != " << item.visible << std::endl;
            return false;
        }
    }

    std::vector<InlineSpan> strong = TinyPdf::Internal::ParseInlineSpans("__strong__");
    std::vector<InlineSpan> emphasis = TinyPdf::Internal::ParseInlineSpans("_emphasis_");
    std::vector<InlineSpan> combined = TinyPdf::Internal::ParseInlineSpans("***combined***");
    std::vector<InlineSpan> nested = TinyPdf::Internal::ParseInlineSpans("**outer _inner_**");
    if (strong.size() != 1 || !strong[0].bold || emphasis.size() != 1 || !emphasis[0].italic ||
        combined.size() != 1 || !combined[0].bold || !combined[0].italic) {
        std::cerr << "inline emphasis flags mismatch" << std::endl;
        return false;
    }
    bool nestedCombined = false;
    for (const InlineSpan& span : nested) {
        nestedCombined = nestedCombined || (span.text == "inner" && span.bold && span.italic);
    }
    if (!nestedCombined) {
        std::cerr << "nested emphasis flags mismatch" << std::endl;
        return false;
    }

    const std::string definitions =
        "[first]: <https://example.com/first>\n"
        "    \"title on next line\"\n\n"
        "[first][] and `[first][]`\n";
    std::vector<TinyPdf::Internal::Block> blocks = TinyPdf::Internal::ParseMarkdown(definitions);
    if (blocks.size() != 1 || blocks[0].text.find("https://example.com/first") == std::string::npos ||
        blocks[0].text.find("`[first][]`") == std::string::npos) {
        std::cerr << "reference definition continuation/code exclusion mismatch" << std::endl;
        return false;
    }
    return true;
}

bool CheckClassicBlockAmbiguities() {
    using TinyPdf::Internal::Block;
    using TinyPdf::Internal::BlockType;
    const std::string blockSource =
        "Heading one\n===\n\n"
        "Heading two\n---\n\n"
        "---\n\n"
        "\talpha\n\t\tbeta\n\n"
        "soft\nline\n\n"
        "hard  \nbreak\n";
    std::vector<Block> blocks = TinyPdf::Internal::ParseMarkdown(blockSource);
    if (blocks.size() != 6 || blocks[0].type != BlockType::Heading || blocks[0].level != 1 ||
        blocks[1].type != BlockType::Heading || blocks[1].level != 2 ||
        blocks[2].type != BlockType::Rule || blocks[3].type != BlockType::Code ||
        blocks[3].text != "alpha\n\tbeta" || blocks[4].type != BlockType::Paragraph ||
        blocks[4].text != "soft line" || blocks[5].type != BlockType::Paragraph ||
        blocks[5].text != "hard\nbreak") {
        std::cerr << "classic block ambiguity mismatch" << std::endl;
        return false;
    }

    blocks = TinyPdf::Internal::ParseMarkdown("    not heading\n    ---\n");
    if (blocks.size() != 1 || blocks[0].type != BlockType::Code ||
        blocks[0].text != "not heading\n---") {
        std::cerr << "indented Setext ambiguity mismatch" << std::endl;
        return false;
    }

    const std::string references =
        "[plain][ID], [spaced] [id], [implicit][], [collapsed][many spaces], and [missing][nope].\n\n"
        "[id]: https://example.com/id\n"
        "[implicit]: https://example.com/implicit\n"
        "[many   spaces]: https://example.com/collapsed\n";
    blocks = TinyPdf::Internal::ParseMarkdown(references);
    if (blocks.size() != 1 || blocks[0].type != BlockType::Paragraph) {
        std::cerr << "reference definition suppression mismatch" << std::endl;
        return false;
    }
    std::vector<std::string> urls;
    std::string visible;
    for (const TinyPdf::Internal::InlineSpan& span :
        TinyPdf::Internal::ParseInlineSpans(blocks[0].text)) {
        visible += span.text;
        if (!span.url.empty()) urls.emplace_back(span.url.View());
    }
    const std::vector<std::string> expectedUrls = {
        "https://example.com/id", "https://example.com/id",
        "https://example.com/implicit", "https://example.com/collapsed"
    };
    if (urls != expectedUrls || visible != "plain, spaced, implicit, collapsed, and [missing][nope].") {
        std::cerr << "reference normalization/negative mismatch" << std::endl;
        return false;
    }

    const std::string codeDefinitions =
        "`[fake][]`\n\n"
        "    [fake]: https://example.com/fake\n\n"
        "```\n[other]: https://example.com/other\n```\n";
    blocks = TinyPdf::Internal::ParseMarkdown(codeDefinitions);
    if (blocks.size() != 3 || blocks[0].type != BlockType::Paragraph ||
        blocks[0].text != "`[fake][]`" || blocks[1].type != BlockType::Code ||
        blocks[2].type != BlockType::Code) {
        std::cerr << "reference definition code exclusion mismatch: blocks=" << blocks.size();
        for (const Block& block : blocks) std::cerr << " [" << (int)block.type << ":" << block.text << "]";
        std::cerr << std::endl;
        return false;
    }

    const std::string inlineReferenceImage =
        "before ![alt][asset] after\n\n"
        "[asset]: docs/assets/branding/rayomd.png\n";
    blocks = TinyPdf::Internal::ParseMarkdown(inlineReferenceImage);
    if (blocks.size() != 1 || blocks[0].type != BlockType::Paragraph ||
        InlineVisible(blocks[0].text) != "before image: alt after") {
        std::cerr << "inline reference image fallback mismatch" << std::endl;
        return false;
    }

    const std::string containerTables =
        "> A | B\n"
        "> --- | ---\n"
        "> 1 | 2\n\n"
        "- table item\n\n"
        "    C | D\n"
        "    --- | ---\n"
        "    3 | 4\n";
    blocks = TinyPdf::Internal::ParseMarkdown(containerTables);
    if (blocks.size() != 2 || blocks[0].type != BlockType::Quote ||
        blocks[0].children.size() != 1 || blocks[0].children[0].type != BlockType::Table ||
        blocks[1].type != BlockType::Bullet || blocks[1].children.size() != 1 ||
        blocks[1].children[0].type != BlockType::Table) {
        std::cerr << "container table parsing mismatch" << std::endl;
        return false;
    }

    blocks = TinyPdf::Internal::ParseMarkdown(
        "2) ordered item\n   wrapped continuation\n\n    second paragraph\n");
    if (blocks.size() != 1 || blocks[0].type != BlockType::Numbered || blocks[0].number != 2 ||
        blocks[0].text != "ordered item wrapped continuation" || blocks[0].children.size() != 1 ||
        blocks[0].children[0].type != BlockType::Paragraph ||
        blocks[0].children[0].text != "second paragraph") {
        std::cerr << "structured ordered-list mismatch" << std::endl;
        return false;
    }
    return true;
}

bool CheckClassicInlineExactness() {
    using TinyPdf::Internal::InlineSpan;
    struct CodeCase { const char* input; const char* visible; const char* codeText; };
    const CodeCase codeCases[] = {
        {"`code`", "code", "code"},
        {"``literal ` tick``", "literal ` tick", "literal ` tick"},
        {"` foo `", "foo", "foo"},
        {"`  `", "  ", "  "},
        {"`line\nbreak`", "line break", "line break"},
        {"```two `` ticks```", "two `` ticks", "two `` ticks"},
        {"unmatched ` tick", "unmatched ` tick", nullptr},
    };
    for (const CodeCase& item : codeCases) {
        std::vector<InlineSpan> spans = TinyPdf::Internal::ParseInlineSpans(item.input);
        std::string visible;
        bool matchedCode = false;
        bool anyCode = false;
        for (const InlineSpan& span : spans) {
            visible += span.text;
            anyCode = anyCode || span.code;
            matchedCode = matchedCode || (item.codeText && span.code && span.text == item.codeText);
        }
        if (visible != item.visible || (item.codeText ? !matchedCode : anyCode)) {
            std::cerr << "matching-run code span mismatch: " << item.input << std::endl;
            return false;
        }
    }

    struct EmphasisCase { const char* input; const char* text; bool bold; bool italic; };
    const EmphasisCase emphasisCases[] = {
        {"*asterisk*", "asterisk", false, true},
        {"**asterisk**", "asterisk", true, false},
        {"***asterisk***", "asterisk", true, true},
        {"_underscore_", "underscore", false, true},
        {"__underscore__", "underscore", true, false},
        {"___underscore___", "underscore", true, true},
        {"**outer _inner_**", "inner", true, true},
    };
    for (const EmphasisCase& item : emphasisCases) {
        bool found = false;
        for (const InlineSpan& span : TinyPdf::Internal::ParseInlineSpans(item.input)) {
            found = found || (span.text == item.text && span.bold == item.bold && span.italic == item.italic);
        }
        if (!found) {
            std::cerr << "classic emphasis mismatch: " << item.input << std::endl;
            return false;
        }
    }
    if (InlineVisible("word_with_internal_underscores") != "word_with_internal_underscores" ||
        InlineVisible("unmatched * marker") != "unmatched * marker") {
        std::cerr << "emphasis negative-case mismatch" << std::endl;
        return false;
    }

    const std::string escapable = "\\`*{}[]()#+-.!_";
    for (char punctuation : escapable) {
        std::string input;
        input.push_back('\\');
        input.push_back(punctuation);
        std::string expected(1, punctuation);
        if (InlineVisible(input) != expected) {
            std::cerr << "classic escape mismatch for byte " << (int)(unsigned char)punctuation << std::endl;
            return false;
        }
    }
    for (char punctuation : std::string(">q/@:=")) {
        std::string input;
        input.push_back('\\');
        input.push_back(punctuation);
        if (InlineVisible(input) != input) {
            std::cerr << "non-escapable slash mismatch for byte " << (int)(unsigned char)punctuation << std::endl;
            return false;
        }
    }

    std::vector<InlineSpan> code = TinyPdf::Internal::ParseInlineSpans("`\\* _`");
    if (code.size() != 1 || !code[0].code || code[0].text != "\\* _") {
        std::cerr << "code-span escape isolation mismatch" << std::endl;
        return false;
    }
    std::vector<TinyPdf::Internal::Block> blocks =
        TinyPdf::Internal::ParseMarkdown("    \\* _\n");
    if (blocks.size() != 1 || blocks[0].type != TinyPdf::Internal::BlockType::Code ||
        blocks[0].text != "\\* _") {
        std::cerr << "code-block escape isolation mismatch" << std::endl;
        return false;
    }
    // A fence's text is one copy of the source where its lines lie there as they are, and is
    // joined line by line where a "\r" left a line's end: the same text either way.
    const std::pair<const char*, const char*> fences[] = {
        { "```\na\n\nb\n```\n", "a\n\nb\n" }, { "```\r\na\r\nb\r\n```\r\n", "a\nb\n" },
        { "```\na\r\nb\n```", "a\nb\n" }, { "```\na\nb\r\n```", "a\nb\n" }, { "```\na\nb", "a\nb" },
        { "```\na\nb\r", "a\nb" }, { "```\n```", "" }, { "```\n\n```\nx", "\n" }, { "x\n\n~~~ c\n a\t\n~~~", " a\t\n" },
    };
    for (const auto& [source, text] : fences) {
        blocks = TinyPdf::Internal::ParseMarkdown(source);
        const TinyPdf::Internal::Block* fenced = nullptr;
        for (const TinyPdf::Internal::Block& block : blocks) {
            if (block.type == TinyPdf::Internal::BlockType::Code) fenced = &block;
        }
        if (fenced == nullptr || fenced->text != text) {
            std::cerr << "fenced code text mismatch: " << source << std::endl;
            return false;
        }
    }

    // "<not-an-email>" would be an HTML tag (a custom element), which is left out as on GitHub.
    const std::string invalidAutolinks = "<1-not-an-email> and <user@localhost>";
    std::vector<InlineSpan> invalid = TinyPdf::Internal::ParseInlineSpans(invalidAutolinks);
    bool hasUrl = false;
    std::string invalidVisible;
    for (const InlineSpan& span : invalid) {
        hasUrl = hasUrl || !span.url.empty();
        invalidVisible += span.text;
    }
    if (hasUrl || invalidVisible != invalidAutolinks) {
        std::cerr << "autolink negative-case mismatch" << std::endl;
        return false;
    }
    return true;
}

bool CheckContainerPdfLayout() {
    const std::string layout =
        "[root](https://example.com/root)\n\n"
        "> [quote](https://example.com/quote)\n"
        "> > [nested](https://example.com/nested)\n"
        ">\n"
        "> ## Quote heading\n"
        ">\n"
        ">     quote_code()\n\n"
        "- list item\n\n"
        "    [child](https://example.com/child)\n\n"
        "        list_code()\n";
    std::string standardPdf;
    if (!Build(layout, standardPdf) ||
        standardPdf.find("RayoMD Native Standard PDF") == std::string::npos ||
        CountOccurrences(standardPdf, "/Subtype /Link") != 4) {
        std::cerr << "standard container PDF build mismatch" << std::endl;
        return false;
    }
    std::array<double, 4> root{};
    std::array<double, 4> quote{};
    std::array<double, 4> nested{};
    std::array<double, 4> child{};
    if (!FindLinkRectangle(standardPdf, "https://example.com/root", root) ||
        !FindLinkRectangle(standardPdf, "https://example.com/quote", quote) ||
        !FindLinkRectangle(standardPdf, "https://example.com/nested", nested) ||
        !FindLinkRectangle(standardPdf, "https://example.com/child", child) ||
        quote[0] <= root[0] + 5.0 || nested[0] <= quote[0] + 5.0 || child[0] <= root[0] + 5.0) {
        std::cerr << "standard container indentation/link alignment mismatch" << std::endl;
        return false;
    }
    size_t heading = standardPdf.find("(Quote heading) Tj");
    size_t headingFont = heading == std::string::npos ? std::string::npos :
        standardPdf.rfind("/F2 ", heading);
    if (heading == std::string::npos || headingFont == std::string::npos || heading - headingFont > 160) {
        std::cerr << "quote heading style mismatch" << std::endl;
        return false;
    }

    std::string unicodePdf;
    const std::string unicodeLayout = u8"Za\u017C\u00F3\u0142\u0107\n\n" + layout;
    if (!Build(unicodeLayout, unicodePdf) ||
        unicodePdf.find("RayoMD Native Tiny PDF") == std::string::npos ||
        CountOccurrences(unicodePdf, "/Subtype /Link") != 4) {
        std::cerr << "Unicode container PDF build mismatch" << std::endl;
        return false;
    }
    if (!FindLinkRectangle(unicodePdf, "https://example.com/root", root) ||
        !FindLinkRectangle(unicodePdf, "https://example.com/quote", quote) ||
        !FindLinkRectangle(unicodePdf, "https://example.com/nested", nested) ||
        !FindLinkRectangle(unicodePdf, "https://example.com/child", child) ||
        quote[0] <= root[0] + 5.0 || nested[0] <= quote[0] + 5.0 || child[0] <= root[0] + 5.0) {
        std::cerr << "Unicode container indentation/link alignment mismatch" << std::endl;
        return false;
    }
    return true;
}
struct FixtureOptions {
    std::string attachmentName = "source.md";
    std::string profile = "rayomd-source/1";
    std::string catalogExtra;
    std::string sourceExtra;
    std::string fileSpecExtra;
    std::string metadataExtra;
    std::string trailerExtra;
    size_t sourceReference = 2;
    size_t metadataReference = 4;
    long long sourceLengthDelta = 0;
    bool includeMetadata = true;
    bool duplicateNameEntry = false;
    bool additionalNameEntry = false;
};

std::string BuildClassicProfileFixture(const std::string& source, const FixtureOptions& options = {}) {
    std::string xmp = RayoMd::PdfSource::BuildXmpMetadata(source, "fixture");
    size_t profile = xmp.find("rayomd-source/1");
    if (profile != std::string::npos) xmp.replace(profile, 15, options.profile);
    std::string names = "[(" + options.attachmentName + ") 3 0 R";
    if (options.duplicateNameEntry) names += " (" + options.attachmentName + ") 3 0 R";
    if (options.additionalNameEntry) names += " (notes.md) 3 0 R";
    names += "]";
    std::string catalog = "<< /Type /Catalog";
    if (options.includeMetadata) catalog += " /Metadata " + std::to_string(options.metadataReference) + " 0 R";
    catalog += " /Names << /EmbeddedFiles << /Names " + names + " >> >> /AF [3 0 R]" +
        options.catalogExtra + " >>";
    long long declaredLength = static_cast<long long>(source.size()) + options.sourceLengthDelta;
    std::string sourceObject = "<< /Type /EmbeddedFile /Subtype /text#2Fmarkdown /Params << /Size " +
        std::to_string(source.size()) + " >> /Length " + std::to_string(declaredLength) + options.sourceExtra +
        " >>\nstream\n" + source + "\nendstream";
    std::string fileSpec = "<< /Type /Filespec /F (" + options.attachmentName + ") /UF (" +
        options.attachmentName + ") /EF << /F " + std::to_string(options.sourceReference) +
        " 0 R /UF " + std::to_string(options.sourceReference) +
        " 0 R >> /AFRelationship /Source" + options.fileSpecExtra + " >>";
    std::string metadata = "<< /Type /Metadata /Subtype /XML /Length " + std::to_string(xmp.size()) +
        options.metadataExtra + " >>\nstream\n" + xmp + "\nendstream";
    std::vector<std::string> objects = {catalog, sourceObject, fileSpec, metadata};

    std::string pdf = "%PDF-2.0\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets(1, 0);
    for (size_t i = 0; i < objects.size(); i++) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    char entry[32];
    for (size_t i = 1; i < offsets.size(); i++) {
        std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offsets[i]);
        pdf += entry;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R" +
        options.trailerExtra + " >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

bool ExpectStatus(const std::string& pdf, RayoMd::PdfSource::Status status, const char* label) {
    RayoMd::PdfSource::Status actual = RayoMd::PdfSource::Inspect(pdf, true).status;
    if (actual == status) return true;
    std::cerr << label << " status mismatch: " << static_cast<int>(actual) << " != "
        << static_cast<int>(status) << std::endl;
    return false;
}

bool ExpectImagePolicyResult(const std::string& markdown, const TinyPdf::PdfOptions& options,
    bool expectImage, const char* label) {
    std::string pdf;
    bool built = TinyPdf::BuildPdf(markdown, options, pdf).Ok() && ValidPdf(pdf);
    bool hasImage = built && pdf.find("/Subtype /Image") != std::string::npos;
    if (built && hasImage == expectImage) return true;
    std::cerr << label << " image policy mismatch" << std::endl;
    return false;
}

bool CheckImagePolicyCacheIsolation() {
    std::string sourceRoot = RAYOMD_TEST_SOURCE_DIR;
    for (char& ch : sourceRoot) if (ch == '\\') ch = '/';

    const std::string relativeSource = "docs/assets/branding/rayomd.png";
    const std::string relativeMarkdown = "![relative](" + relativeSource + ")\n";
    TinyPdf::PdfOptions safe;
    safe.sourcePath = sourceRoot + "/tester.md";
    if (!ExpectImagePolicyResult(relativeMarkdown, safe, true, "contained image")) return false;

    TinyPdf::PdfOptions noRoot = safe;
    noRoot.sourcePath.clear();
    if (!ExpectImagePolicyResult(relativeMarkdown, noRoot, false, "source-less image")) return false;

    TinyPdf::PdfOptions wrongRoot = safe;
    wrongRoot.sourcePath = sourceRoot + "/docs/tester.md";
    if (!ExpectImagePolicyResult(relativeMarkdown, wrongRoot, false, "different source root")) return false;

    // Reach the already cached mascot through an escaping parent path. The
    // decoded-image cache must never substitute for authorization.
    TinyPdf::PdfOptions escapingRoot = safe;
    escapingRoot.sourcePath = sourceRoot + "/docs/development/tester.md";
    const std::string escapingMarkdown =
        "![escape](../assets/branding/rayomd.png)\n";
    if (!ExpectImagePolicyResult(escapingMarkdown, escapingRoot, false, "parent escape")) return false;

    const std::string absoluteMarkdown =
        "![absolute](<" + sourceRoot + "/" + relativeSource + ">)\n";
    TinyPdf::PdfOptions unsafe = safe;
    unsafe.allowUnsafeLocalImages = true;
    if (!ExpectImagePolicyResult(absoluteMarkdown, unsafe, true, "unsafe absolute image")) return false;
    if (!ExpectImagePolicyResult(absoluteMarkdown, safe, false, "safe absolute image")) return false;
    return true;
}

// ---- What builds may reach (access_policy.h) ---------------------------------------

// URL images: addresses of this machine and of private networks are refused, IPv6 forms that
// carry an IPv4 address by that address; public ones pass.
bool CheckUrlAddressFilter() {
    const auto v4 = [](uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a << 24 | b << 16 | c << 8 | d; };
    const std::pair<uint32_t, bool> ipv4[] = {
        { v4(0, 0, 0, 0), true }, { v4(10, 1, 2, 3), true }, { v4(127, 0, 0, 1), true },
        { v4(169, 254, 169, 254), true }, { v4(172, 16, 0, 1), true }, { v4(172, 31, 255, 255), true },
        { v4(172, 32, 0, 1), false }, { v4(192, 168, 1, 1), true },
        { v4(100, 64, 0, 1), true }, { v4(100, 100, 1, 5), true }, { v4(100, 127, 255, 255), true },
        { v4(100, 63, 255, 255), false }, { v4(100, 128, 0, 1), false },
        { v4(198, 18, 0, 1), true }, { v4(198, 19, 255, 255), true }, { v4(198, 17, 255, 255), false },
        { v4(198, 20, 0, 0), false }, { v4(192, 0, 0, 8), true }, { v4(192, 0, 1, 1), false },
        { v4(224, 0, 0, 1), true }, { v4(240, 0, 0, 1), true }, { v4(255, 255, 255, 255), true },
        { v4(8, 8, 8, 8), false }, { v4(1, 1, 1, 1), false }, { v4(93, 184, 216, 34), false },
    };
    for (const auto& [address, unsafe] : ipv4) {
        if (TinyPdf::Internal::IsUnsafeIpv4Address(address) != unsafe) {
            std::cerr << "IPv4 address " << (address >> 24) << '.' << (address >> 16 & 255) << '.' << (address >> 8 & 255)
                      << '.' << (address & 255) << (unsafe ? " allowed" : " refused") << std::endl;
            return false;
        }
    }
    using Groups = std::array<uint32_t, 8>;
    const std::pair<Groups, bool> ipv6[] = {
        { { 0, 0, 0, 0, 0, 0, 0, 0 }, true },                          // ::
        { { 0, 0, 0, 0, 0, 0, 0, 1 }, true },                          // ::1
        { { 0, 0, 0, 0, 0, 0xffff, 0x7f00, 0x0001 }, true },           // ::ffff:127.0.0.1
        { { 0, 0, 0, 0, 0, 0xffff, 0x0808, 0x0808 }, false },          // ::ffff:8.8.8.8
        { { 0, 0, 0, 0, 0, 0, 0x0a00, 0x0001 }, true },                // ::10.0.0.1
        { { 0, 0, 0, 0, 0, 0, 0x0808, 0x0808 }, false },               // ::8.8.8.8
        { { 0, 0, 0, 0, 0xffff, 0, 0x0a00, 0x0001 }, true },           // ::ffff:0:10.0.0.1
        { { 0x64, 0xff9b, 0, 0, 0, 0, 0xc0a8, 0x0001 }, true },        // 64:ff9b::192.168.0.1
        { { 0x64, 0xff9b, 0, 0, 0, 0, 0x6440, 0x0001 }, true },        // 64:ff9b::100.64.0.1
        { { 0x64, 0xff9b, 0, 0, 0, 0, 0x0808, 0x0808 }, false },       // 64:ff9b::8.8.8.8
        { { 0x64, 0xff9b, 1, 0, 0, 0, 0, 1 }, true },                  // 64:ff9b:1::1
        { { 0x2002, 0x7f00, 0x0001, 0, 0, 0, 0, 0 }, true },           // 2002:7f00:1::
        { { 0x2002, 0xa9fe, 0xa9fe, 0, 0, 0, 0, 1 }, true },           // 2002:169.254.169.254::1
        { { 0x2002, 0x0808, 0x0808, 0, 0, 0, 0, 1 }, false },          // 2002:8.8.8.8::1
        { { 0xfc00, 0, 0, 0, 0, 0, 0, 1 }, true }, { { 0xfd12, 0x3456, 0, 0, 0, 0, 0, 1 }, true },
        { { 0xfe80, 0, 0, 0, 0, 0, 0, 1 }, true }, { { 0xfec0, 0, 0, 0, 0, 0, 0, 1 }, true },
        { { 0xff02, 0, 0, 0, 0, 0, 0, 1 }, true },
        { { 0x2001, 0x4860, 0x4860, 0, 0, 0, 0, 0x8888 }, false }, { { 0x2606, 0x4700, 0, 0, 0, 0, 0, 0x1111 }, false },
    };
    for (const auto& [groups, unsafe] : ipv6) {
        unsigned char bytes[16];
        for (size_t i = 0; i < groups.size(); i++) {
            bytes[2 * i] = (unsigned char)(groups[i] >> 8);
            bytes[2 * i + 1] = (unsigned char)groups[i];
        }
        if (TinyPdf::Internal::IsUnsafeIpv6Address(bytes) != unsafe) {
            std::cerr << "IPv6 address " << std::hex << groups[0] << ':' << groups[1] << ':' << groups[2] << "... "
                      << std::dec << (unsafe ? "allowed" : "refused") << std::endl;
            return false;
        }
    }
    return true;
}

// Theme fonts and logos are local files: on Windows a UNC, device or drive-relative path is
// refused by the parser and by the build, before anything opens it (no SMB or WebDAV
// connection, no NTLM). The classification itself is checked on every platform.
bool CheckThemePathPolicy() {
    const std::pair<const char*, bool> paths[] = {
        { "\\\\server\\share\\font.ttf", true }, { "//server/share/font.ttf", true }, { "\\/server/share/f.ttf", true },
        { "\\\\attacker.example@SSL\\s\\f.ttf", true }, { "\\\\?\\C:\\font.ttf", true },
        { "\\\\?\\UNC\\server\\share\\f.ttf", true }, { "\\\\.\\pipe\\x", true }, { "\\??\\UNC\\server\\s\\f.ttf", true },
        { "C:font.ttf", true }, { "C:", true }, { "C:\\fonts\\NUL.ttf", true }, { "fonts/con", true },
        { "themes/COM1.png", true }, { "lpt9 .txt", true }, { "conin$", true },
        { "C:\\Windows\\Fonts\\arial.ttf", false }, { "c:/fonts/a.ttf", false }, { "fonts/acme.ttf", false },
        { "\\fonts\\a.ttf", false }, { "/usr/share/fonts/a.ttf", false }, { "console.ttf", false },
        { "COM10.ttf", false }, { "", false }, { "themes/../fonts/a.ttf", false },
    };
    for (const auto& [path, refused] : paths) {
        if (TinyPdf::Internal::IsNetworkOrDevicePath(path) != refused) {
            std::cerr << "theme path " << path << (refused ? " allowed" : " refused") << std::endl;
            return false;
        }
    }
#ifdef _WIN32
    // A fresh theme for each parse: ParseTheme sets fields as it reads them, and a failed one keeps them.
    for (const char* text : { "font = \\\\attacker.example\\s\\f.ttf\n", "logo = //attacker.example@SSL/s/l.png\n",
             "font = a.ttf\nfont-bold = C:b.ttf\n", "font = a.ttf\nlogo = img\\NUL.png\n" }) {
        TinyPdf::PdfTheme theme;
        std::string error;
        if (TinyPdf::Internal::ParseTheme(text, "C:\\themes", theme, error) || error.find("local drive") == std::string::npos) {
            std::cerr << "a theme's network or device path was accepted: " << text << std::endl;
            return false;
        }
    }
    // A theme file in a share: its relative paths are in the share too.
    TinyPdf::PdfTheme shared;
    TinyPdf::PdfTheme local;
    std::string error;
    if (TinyPdf::Internal::ParseTheme("font = a.ttf\n", "\\\\server\\themes", shared, error) ||
        !TinyPdf::Internal::ParseTheme("font = fonts\\a.ttf\nlogo = /img/l.png\n", "C:\\themes", local, error)) {
        std::cerr << "theme paths in a share or on a local drive mismatch: " << error << std::endl;
        return false;
    }
    // A theme an API caller filled in: the build refuses what the parser would, without opening it
    // (this administrative share of the machine itself would open).
    const std::string share = "\\\\127.0.0.1\\c$\\Windows\\Fonts\\arial.ttf";
    TinyPdf::PdfOptions options;
    options.theme.fontPath = share;
    std::string pdf;
    if (TinyPdf::Internal::IsLocalThemeFile(share) ||
        TinyPdf::BuildPdf("# Theme\n", options, pdf).error != TinyPdf::BuildError::ThemeFontUnavailable) {
        std::cerr << "a theme font on a share was opened" << std::endl;
        return false;
    }
    TinyPdf::PdfOptions logo;
    logo.theme.headerLeft = "{logo}";
    logo.theme.logoPath = "\\\\127.0.0.1\\c$\\Windows\\Web\\Screen\\img100.jpg";
    const TinyPdf::BuildResult logoResult = TinyPdf::BuildPdf("# Theme\n", logo, pdf);
    if (!logoResult.Ok() || logoResult.failedImages != 1) {
        std::cerr << "a theme logo on a share was read" << std::endl;
        return false;
    }
    if (const char* windows = std::getenv("WINDIR")) {
        const std::string arial = std::string(windows) + "\\Fonts\\arial.ttf";
        if (std::ifstream(arial, std::ios::binary) && !TinyPdf::Internal::IsLocalThemeFile(arial)) {
            std::cerr << "a font on the system drive is no local theme file" << std::endl;
            return false;
        }
    }
#endif
    return true;
}

// Images are PNG, JPEG and SVG: other formats show their alt text on every platform, and on
// Windows never reach the system's codecs (WIC decodes only PNG and JPEG files RayoMD's own
// readers do not take, with Microsoft's decoder for that format).
bool CheckImageFormats() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "rayomd-test-image-formats";
    fs::remove_all(root);
    fs::create_directories(root);
    // A 1x1 24-bit BMP, which WIC decoded before.
    const unsigned char bmp[] = { 'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
        1, 0, 24, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0x13, 0x0b, 0, 0, 0x13, 0x0b, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x00, 0x00, 0xff, 0x00 };
    const unsigned char gif[] = { 'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x2c,
        0, 0, 0, 0, 1, 0, 1, 0, 0, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b };
    bool ok = RayoMd::File::WriteAll(root / "pixel.bmp", std::string_view((const char*)bmp, sizeof(bmp))) &&
        RayoMd::File::WriteAll(root / "pixel.gif", std::string_view((const char*)gif, sizeof(gif)));
    fs::copy_file(fs::path(RAYOMD_TEST_SOURCE_DIR) / "docs" / "assets" / "branding" / "rayomd.png", root / "pixel.png");
    TinyPdf::PdfOptions options;
    options.sourcePath = (root / "doc.md").u8string();
    for (const auto& [markdown, failures] : { std::pair<const char*, uint32_t>{ "![b](pixel.bmp)\n", 1 },
             { "![g](pixel.gif)\n", 1 }, { "![p](pixel.png)\n", 0 } }) {
        std::string pdf;
        const TinyPdf::BuildResult result = ok ? TinyPdf::BuildPdf(markdown, options, pdf) : TinyPdf::BuildResult();
        if (!result.Ok() || result.failedImages != failures ||
            (pdf.find("/Subtype /Image") != std::string::npos) != (failures == 0)) {
            std::cerr << "image format policy mismatch for " << markdown;
            ok = false;
            break;
        }
    }
    fs::remove_all(root);
    return ok;
}

#ifdef _WIN32
// The direct local-image path of Windows follows no link: an image that is a symbolic link, or
// lies in a linked folder, is refused before its target is opened, even inside the document's
// folder (a link could as well lead to \\attacker\share). Skipped where links cannot be made.
bool CheckDirectImageLinks() {
    namespace fs = std::filesystem;
    // The working directory, not the temporary one: a short (8.3) name in the root's path takes
    // the other, canonical-path route.
    const fs::path root = fs::current_path() / "rayomd-test-image-links";
    fs::remove_all(root);
    fs::create_directories(root / "sub");
    const fs::path mascot = fs::path(RAYOMD_TEST_SOURCE_DIR) / "docs" / "assets" / "branding" / "rayomd.png";
    fs::copy_file(mascot, root / "real.png");
    fs::copy_file(mascot, root / "sub" / "real.png");
    const DWORD unprivileged = 0x2;   // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (Developer Mode)
    const bool linked = CreateSymbolicLinkW((root / "link.png").c_str(), L"real.png", unprivileged) &&
        CreateSymbolicLinkW((root / "linkdir").c_str(), L"sub", SYMBOLIC_LINK_FLAG_DIRECTORY | unprivileged);
    bool ok = true;
    if (!linked) {
        std::cerr << "skipped the image link check: symbolic links cannot be made here" << std::endl;
    } else {
        TinyPdf::PdfOptions options;
        options.sourcePath = (root / "doc.md").u8string();
        for (const auto& [markdown, failures] : { std::pair<const char*, uint32_t>{ "![r](real.png)\n", 0 },
                 { "![s](sub/real.png)\n", 0 }, { "![l](link.png)\n", 1 }, { "![d](linkdir/real.png)\n", 1 } }) {
            std::string pdf;
            const TinyPdf::BuildResult result = TinyPdf::BuildPdf(markdown, options, pdf);
            if (!result.Ok() || result.failedImages != failures) {
                std::cerr << "image link policy mismatch for " << markdown;
                ok = false;
                break;
            }
        }
    }
    fs::remove_all(root);
    return ok;
}
#endif

// ---- Native math -------------------------------------------------------------------

#include "no_math_golden.inc"
#include "math_markdown_tests.inc"
#include "math_parser_tests.inc"
#include "math_layout_tests.inc"
#include "inline_lookahead_tests.inc"

// The peak resident size of this process in bytes, 0 where it cannot be read: off Linux and under
// AddressSanitizer, whose shadow memory and quarantine make it say little about the code.
size_t PeakRssBytes() {
#if defined(__linux__) && !defined(__SANITIZE_ADDRESS__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) return static_cast<size_t>(usage.ru_maxrss) * 1024;
#endif
    return 0;
}

#include "parse_limits_tests.inc"
#include "html_tests.inc"
#include "hardening_tests.inc"
#include "dialect_tests.inc"
#include "embeds_tests.inc"
#include "path_tests.inc"
#include "mermaid_tests.inc"
#include "long_page_tests.inc"
#include "print_tests.inc"

// Heights of the filled rectangles drawn with the given fill colour ("r g b").
std::vector<double> FillRectHeights(const std::string& pdf, std::string_view color) {
    std::vector<double> heights;
    std::string needle = "q ";
    needle.append(color);
    needle += " rg ";
    size_t position = 0;
    while ((position = pdf.find(needle, position)) != std::string::npos) {
        position += needle.size();
        size_t end = pdf.find(" re f Q", position);
        if (end == std::string::npos) break;
        std::istringstream values(pdf.substr(position, end - position));
        double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
        if (values >> x >> y >> w >> h) heights.push_back(h);
    }
    return heights;
}

// Largest x at which a text run of the renderers starts ("... Tf 1 0 0 1 x y Tm").
double MaxTextStartX(const std::string& pdf) {
    double maxX = 0.0;
    const std::string_view needle = " Tf 1 0 0 1 ";
    size_t position = 0;
    while ((position = pdf.find(needle, position)) != std::string::npos) {
        position += needle.size();
        maxX = std::max(maxX, std::atof(pdf.c_str() + position));
    }
    return maxX;
}

bool CheckMathPdf() {
    const char* const sourceBox = "0.97 0.97 0.95 rg";
    // Each face as the font object names it, embedded; the heading's formula paints the bold faces.
    const char* const mathFonts[] = {
        "+RayoMDMath-Roman /FirstChar ", "+RayoMDMath-Italic /FirstChar ", "+RayoMDMath-Bold /FirstChar ",
        "+RayoMDMath-BoldItalic /FirstChar ", "+RayoMDMath-Symbol /FirstChar "
    };
    const std::string mathBody =
        "# Energy $E=mc^2$\n\n"
        "Inline $a^2+b^2=c^2$ and a tall $\\frac{\\frac{a}{b}}{\\frac{c}{d}}$ fraction.\n\n"
        "$$\n\\int_{-\\infty}^{\\infty} e^{-x^2}\\,dx = \\sqrt{\\pi}\n$$\n\n"
        "- item $x_i$\n\n> quote $q$\n\n"
        "| f | v |\n|---|---|\n| $\\alpha$ | $\\frac{a}{b}$ |\n";

    // An ASCII document with math stays on the standard-font path and gains an embedded font for
    // each math face it paints; a document without math gains nothing.
    std::string asciiPdf;
    if (!Build(mathBody, asciiPdf) || asciiPdf.find("RayoMD Native Standard PDF") == std::string::npos) {
        std::cerr << "ASCII math build mismatch" << std::endl;
        return false;
    }
    std::string unicodePdf;
    if (!Build(u8"Za\u017C\u00F3\u0142\u0107 $\\text{g\u0119\u015Bl\u0105}$\n\n" + mathBody, unicodePdf) ||
        unicodePdf.find("RayoMD Native Tiny PDF") == std::string::npos) {
        std::cerr << "Unicode math build mismatch" << std::endl;
        return false;
    }
    std::string plainPdf;
    if (!Build("# Plain\n\nNo math here, only $5 and $10.\n", plainPdf)) return false;
    // The Unicode renderer sets headings in the regular face: no bold math faces there.
    for (const char* font : mathFonts) {
        const bool bold = std::string_view(font).find("Bold") != std::string_view::npos;
        if (CountOccurrences(asciiPdf, font) != 1 || CountOccurrences(unicodePdf, font) != (bold ? 0u : 1u) ||
            plainPdf.find(font) != std::string::npos) {
            std::cerr << "math font object mismatch: " << font << std::endl;
            return false;
        }
    }
    if (CountOccurrences(asciiPdf, "/Subtype /Type1C") != 5 || CountOccurrences(unicodePdf, "/Subtype /Type1C") != 3 ||
        plainPdf.find("RayoMDMath") != std::string::npos || asciiPdf.find("/BaseFont /Times-") != std::string::npos ||
        asciiPdf.find("/BaseFont /Symbol") != std::string::npos) {
        std::cerr << "math fonts: not embedded once per face" << std::endl;
        return false;
    }
    if (asciiPdf.find("/F3 4 0 R /M1 ") == std::string::npos || asciiPdf.find(" /M5 ") == std::string::npos ||
        unicodePdf.find(" /M1 ") == std::string::npos || plainPdf.find("/M1 ") != std::string::npos ||
        plainPdf.find("only $5 and $10.) Tj") == std::string::npos) {
        std::cerr << "math font resource mismatch" << std::endl;
        return false;
    }

    // Formulas are typeset: no TeX source reaches the page, Greek comes from Symbol and a
    // fraction draws its rule.
    for (const char* source : { "\\frac", "\\int", "\\alpha", "\\sqrt", "\\infty", "\\text" }) {
        if (asciiPdf.find(source) != std::string::npos || unicodePdf.find(source) != std::string::npos) {
            std::cerr << "TeX source left in a math document: " << source << std::endl;
            return false;
        }
    }
    std::string alphaPdf;
    std::string fractionPdf;
    if (!Build("Greek $\\alpha$ here.\n", alphaPdf) || alphaPdf.find("/M5 ") == std::string::npos ||
        alphaPdf.find("<61> Tj") == std::string::npos ||
        !Build("Half $\\frac{a}{b}$ here.\n", fractionPdf) || fractionPdf.find("(a) Tj") == std::string::npos ||
        fractionPdf.find("(b) Tj") == std::string::npos || fractionPdf.find(" l S") == std::string::npos) {
        std::cerr << "typeset formula mismatch" << std::endl;
        return false;
    }

    // A link on a line made taller by a formula keeps its rectangle on the text baseline:
    // same x and same height as without the formula, only lower on the page.
    const std::string tall = "$\\frac{\\frac{a}{b}}{\\frac{c}{d}}$";
    std::string linkPlain;
    std::string linkTall;
    std::array<double, 4> plainRect{};
    std::array<double, 4> tallRect{};
    if (!Build("[a](https://example.com/a) and text\n", linkPlain) ||
        !Build("[a](https://example.com/a) and " + tall + "\n", linkTall) ||
        !FindLinkRectangle(linkPlain, "https://example.com/a", plainRect) ||
        !FindLinkRectangle(linkTall, "https://example.com/a", tallRect)) {
        std::cerr << "math link build mismatch" << std::endl;
        return false;
    }
    const double plainHeight = plainRect[3] - plainRect[1];
    const double tallHeight = tallRect[3] - tallRect[1];
    if (std::abs(plainRect[0] - tallRect[0]) > 0.011 || std::abs(plainHeight - tallHeight) > 0.011 ||
        tallRect[1] > plainRect[1] - 1.0) {
        std::cerr << "math link rectangle mismatch" << std::endl;
        return false;
    }

    // The quote strip grows with a tall line; without a formula it keeps its height.
    std::string quotePlain;
    std::string quoteTall;
    if (!Build("> quote text\n", quotePlain) || !Build("> quote " + tall + "\n", quoteTall)) return false;
    std::vector<double> plainStrips = FillRectHeights(quotePlain, "0.94 0.95 0.96");
    std::vector<double> tallStrips = FillRectHeights(quoteTall, "0.94 0.95 0.96");
    if (plainStrips.size() != 1 || tallStrips.size() != 1 || tallStrips[0] < plainStrips[0] + 1.0) {
        std::cerr << "math quote strip mismatch" << std::endl;
        return false;
    }

    // A long word next to a formula still wraps: in a table cell and in a heading it is
    // split, and nothing starts to the right of the text area (page 595 pt, margin 54 pt).
    // The outline holds the heading whole, so only shown text, "(...) Tj", is searched there.
    const std::string cellWord(56, 'w');
    const std::string headingWord(120, 'h');
    std::string longWordPdf;
    if (!Build("| a | b | c | d | e | f |\n|---|---|---|---|---|---|\n| $x$" + cellWord + " | 2 | 3 | 4 | 5 | 6 |\n\n"
            "# Heading $x$" + headingWord + "\n", longWordPdf) ||
        longWordPdf.find(cellWord) != std::string::npos || longWordPdf.find(headingWord + ") Tj") != std::string::npos ||
        MaxTextStartX(longWordPdf) > 595.0 - 54.0) {
        std::cerr << "long word next to a formula is not wrapped" << std::endl;
        return false;
    }
    // The source of an inline formula that cannot fit is shown in code style and wraps too.
    const std::string wideInline(300, 'b');
    std::string wideInlinePdf;
    if (!Build("Text $" + wideInline + "$ after.\n", wideInlinePdf) ||
        wideInlinePdf.find(wideInline) != std::string::npos || MaxTextStartX(wideInlinePdf) > 595.0 - 54.0) {
        std::cerr << "over-wide inline math fallback mismatch" << std::endl;
        return false;
    }

    // A formula that cannot fit the page falls back to the source box instead of
    // overflowing, and the build still succeeds.
    std::string widePdf;
    if (!Build("$$\n" + std::string(4000, 'x') + "\n$$\n", widePdf) || widePdf.find(sourceBox) == std::string::npos) {
        std::cerr << "over-wide display math fallback mismatch" << std::endl;
        return false;
    }
    // ... also with a tag: the body must not be painted over the tag or past the page edge.
    std::string wideTagged = "$$\n";
    for (int i = 0; i < 400; i++) wideTagged += "x + ";
    wideTagged += "y \\tag{1}\n$$\n";
    std::string taggedPdf;
    if (!Build(wideTagged, taggedPdf) || taggedPdf.find(sourceBox) == std::string::npos) {
        std::cerr << "over-wide tagged display math fallback mismatch" << std::endl;
        return false;
    }
    // A large aligned block (about 525 x 897 pt at natural size) is shrunk and typeset.
    std::string aligned = "$$\n\\begin{aligned}\n";
    for (int row = 0; row < 50; row++) {
        aligned += "a &= b";
        for (int i = 0; i < 19; i++) aligned += " + c";
        aligned += row + 1 < 50 ? " \\\\\n" : "\n";
    }
    aligned += "\\end{aligned}\n$$\n";
    std::string alignedPdf;
    if (!Build(aligned, alignedPdf) || alignedPdf.find(sourceBox) != std::string::npos ||
        alignedPdf.find("/M2 ") == std::string::npos) {
        std::cerr << "large aligned block is not typeset" << std::endl;
        return false;
    }
    // A block over the module's source limit shows its complete source, not a cut.
    std::string huge = "$$\n";
    while (huge.size() < 18000) huge += "x + ";
    huge += "y\nENDMARKER\n$$\n";
    std::string hugePdf;
    if (!Build(huge, hugePdf) || hugePdf.find(sourceBox) == std::string::npos ||
        hugePdf.find("(ENDMARKER) Tj") == std::string::npos) {
        std::cerr << "over-limit display math does not show its complete source" << std::endl;
        return false;
    }

    // Hostile TeX reaches the module (as blocks: inline, most of these are over the inline
    // cap) and every build terminates with a valid PDF.
    auto repeated = [](const char* piece, size_t count) {
        std::string value;
        for (size_t i = 0; i < count; i++) value += piece;
        return value;
    };
    const std::string hostile[] = {
        repeated("\\frac{", 1300), repeated("{", 8000), repeated("^", 8000), repeated("\\left(", 1300),
        "\\begin{matrix}" + repeated("a&", 4000), repeated("\\sqrt{", 1300), repeated("x_", 4000),
        repeated("\\begin{matrix}", 500), std::string(8000, '\\'),
        repeated("\\right)", 1000) + "\\tag{" + repeated("{", 2000)
    };
    for (const std::string& tex : hostile) {
        std::string pdf;
        if (!Build("$$\n" + tex + "\n$$\n\nAfter.\n", pdf) || pdf.find("(After.) Tj") == std::string::npos) {
            std::cerr << "hostile TeX build mismatch (" << tex.size() << " bytes)" << std::endl;
            return false;
        }
    }

    // Reversible export still recovers the exact source of a math document.
    std::string reversible;
    if (!BuildReversible(mathBody, reversible) ||
        RayoMd::PdfSource::Inspect(reversible, true).source != mathBody) {
        std::cerr << "math reversible round trip mismatch" << std::endl;
        return false;
    }
    return true;
}

// The hard regression rule of the math feature: a document without math syntax is
// rendered byte-for-byte the same with and without it. The digests were recorded when the
// document title started to come from the first heading, after headings started to make the
// PDF outline, code backgrounds started at the first glyph of the code, the standard-font
// renderer started to show bold, italic and strike-through, exact AFM widths,
// WinAnsiEncoding, CommonMark list nesting, source-faithful word spacing, code tiles that
// no longer cover the descenders and underscores of the line above, and (4.5.0) bullets
// drawn as markers. That renderer does not depend on the platform or on installed fonts, so
// they hold on Windows and Linux.
bool CheckNoMathGolden() {
    struct Golden { TinyPdf::PdfStyle style; size_t size; const char* sha256; };
    const Golden goldens[] = {
        {TinyPdf::PdfStyle::Modern, 7883, "8df480b6a98e4f4f9d38e7147d08f90e4471c9757335de167ee1ec70ac8e1d4b"},
        {TinyPdf::PdfStyle::Tech, 7911, "df9f7235d18e915b290b467d056890f2c6b9ec0fd80b44f2f26bc3d27c07445f"},
    };
    for (const Golden& golden : goldens) {
        TinyPdf::PdfOptions options;
        options.style = golden.style;
        options.margin = TinyPdf::PdfMargin::Normal();
        std::string pdf;
        if (!TinyPdf::BuildPdf(kNoMathGoldenDocument, options, pdf).Ok() || pdf.size() != golden.size ||
            RayoMd::PdfSource::Sha256Hex(pdf) != golden.sha256) {
            std::cerr << "no-math golden mismatch: " << pdf.size() << " "
                << RayoMd::PdfSource::Sha256Hex(pdf) << std::endl;
            return false;
        }
    }
    return true;
}

// Pages are rendered straight into the caller's output buffer and the file is assembled
// in place around them. Whatever the buffer held before and however large it is, the
// bytes must equal those of a build into a new string.
bool CheckOutputBufferReuse() {
    std::string longUnicode = u8"# Unicode\n\n";
    for (int i = 0; i < 400; i++) longUnicode += u8"Zażółć gęślą jaźń [link](https://example.com). ";
    longUnicode += "\n";
    const std::vector<std::string> documents = {
        "# Small\n\nOne paragraph with a [link](https://example.com).\n",
        "# Multipage\n\n" + std::string(60000, 'x') + "\n\n- item\n\n| a | b |\n|---|---|\n| 1 | 2 |\n",
        longUnicode,
        "# Image\n\n![RayoMD](docs/assets/branding/rayomd.png)\n\n" + std::string(9000, 'y') + "\n",
        "$$\n\\frac{a}{b}\n$$\n\n" + std::string(9000, 'z') + "\n",
        std::string()
    };
    std::vector<std::string> fresh(documents.size());
    for (size_t i = 0; i < documents.size(); i++) {
        if (!Build(documents[i], fresh[i])) {
            std::cerr << "buffer reuse: baseline build failed for document " << i << std::endl;
            return false;
        }
    }
    // "] /Count 1 >>" ends a one-page /Pages object; outline entries have counts too.
    if (fresh[1].find("] /Count 1 >>") != std::string::npos || fresh[3].find("/Subtype /Image") == std::string::npos) {
        std::cerr << "buffer reuse: the documents do not cover several pages and an image" << std::endl;
        return false;
    }

    // One buffer for every document: a large file after a small one and the reverse.
    std::string reused;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t step = 0; step < documents.size(); step++) {
            const size_t i = pass == 0 ? step : documents.size() - 1 - step;
            if (!Build(documents[i], reused) || reused != fresh[i]) {
                std::cerr << "buffer reuse: document " << i << " differs in a reused buffer" << std::endl;
                return false;
            }
            std::string reversible;
            std::string reversibleReused = reused;
            if (!BuildReversible(documents[i], reversible) || !BuildReversible(documents[i], reversibleReused) ||
                reversible != reversibleReused || RayoMd::PdfSource::Inspect(reversibleReused, true).source != documents[i]) {
                std::cerr << "buffer reuse: reversible document " << i << " differs in a reused buffer" << std::endl;
                return false;
            }
        }
    }

    // Each page's content is rendered after the gap the thread's last build had before it, so a
    // document built again keeps its content in place, and an edit that changes what lies between
    // pages moves the pages after it towards either end. Every build must equal one on a new
    // thread, which leaves no gaps, in its bytes and its source map, also in both renderers.
    std::string pages;
    for (int i = 0; i < 120; i++) {
        if (i % 7 == 0) pages += "## Part " + std::to_string(i) + "\n\n";
        pages += "Paragraph " + std::to_string(i) + " has words that fill the page, line after line.\n\n";
    }
    const std::string unicodePages = u8"Zażółć\n\n" + pages;
    const std::string edits[] = { pages, pages, "[a](https://a.example) " + pages, pages + "[b](https://b.example)\n",
        "x\n\n" + pages, unicodePages, unicodePages, "[c](https://c.example)\n\n" + unicodePages, documents[3] + pages, pages };
    const auto mapped = [](const std::string& markdown, std::string& pdf, std::vector<TinyPdf::SourceBlock>& map) {
        TinyPdf::PdfOptions options;
        options.sourceMap = &map;
        return TinyPdf::BuildPdf(markdown, options, pdf).Ok();
    };
    const auto sameMaps = [](const std::vector<TinyPdf::SourceBlock>& a, const std::vector<TinyPdf::SourceBlock>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i].firstLine != b[i].firstLine || a[i].endLine != b[i].endLine || a[i].page != b[i].page ||
                a[i].endPage != b[i].endPage || a[i].top != b[i].top || a[i].bottom != b[i].bottom) return false;
        }
        return true;
    };
    std::string warm;
    for (size_t i = 0; i < sizeof(edits) / sizeof(edits[0]); i++) {
        std::vector<TinyPdf::SourceBlock> warmMap, coldMap;
        std::string cold;
        bool coldOk = false;
        std::thread thread([&]() { coldOk = mapped(edits[i], cold, coldMap); });
        thread.join();
        if (!mapped(edits[i], warm, warmMap) || !coldOk || warm != cold || !sameMaps(warmMap, coldMap) ||
            CountOccurrences(cold, "/Type /Page ") < 3) {
            std::cerr << "buffer reuse: edit " << i << " differs from a build on a new thread" << std::endl;
            return false;
        }
    }

    // A buffer far larger than the file is given back instead of being kept.
    std::string oversized;
    oversized.reserve(48u * 1024u * 1024u);
    oversized.assign(4096, '#');
    for (size_t i = 0; i < documents.size(); i++) {
        if (i > 0) oversized.reserve(48u * 1024u * 1024u);
        if (!Build(documents[i], oversized) || oversized != fresh[i] || oversized.capacity() > 8u * 1024u * 1024u) {
            std::cerr << "buffer reuse: document " << i << " differs in an oversized buffer" << std::endl;
            return false;
        }
    }

    // Source and output in the same string.
    for (size_t i = 0; i < documents.size(); i++) {
        std::string aliased = documents[i];
        if (!Build(aliased, aliased) || aliased != fresh[i]) {
            std::cerr << "buffer reuse: document " << i << " differs when it is built over its own source" << std::endl;
            return false;
        }
    }
    return true;
}

// ---- Wrapping and layout -----------------------------------------------------------

// The text operations of the standard-font renderer in content order,
// "BT /F1 9.6 Tf 1 0 0 1 x y Tm (text) Tj", with the literal string unescaped.
struct StandardTextOp {
    std::string font;
    double size = 0.0;
    double x = 0.0;
    double y = 0.0;
    std::string text;
};

std::vector<StandardTextOp> StandardTextOps(const std::string& pdf) {
    std::vector<StandardTextOp> ops;
    size_t position = 0;
    while ((position = pdf.find("BT /", position)) != std::string::npos) {
        const size_t operands = pdf.find(' ', position + 4);
        const size_t literal = pdf.find(" Tm (", position);
        if (operands == std::string::npos || literal == std::string::npos) break;
        StandardTextOp op;
        op.font = pdf.substr(position + 4, operands - position - 4);
        std::istringstream values(pdf.substr(operands, literal - operands));
        std::string operatorName;
        double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
        values >> op.size >> operatorName >> a >> b >> c >> d >> op.x >> op.y;
        for (position = literal + 5; position < pdf.size() && pdf[position] != ')'; position++) {
            if (pdf[position] == '\\' && position + 1 < pdf.size()) position++;
            op.text.push_back(pdf[position]);
        }
        if (operatorName == "Tf") ops.push_back(op);
    }
    return ops;
}

// Smallest baseline at which a text run of the renderers starts ("... Tf 1 0 0 1 x y Tm").
double MinTextStartY(const std::string& pdf) {
    double minY = 842.0;
    const std::string_view needle = " Tf 1 0 0 1 ";
    size_t position = 0;
    while ((position = pdf.find(needle, position)) != std::string::npos) {
        position += needle.size();
        std::istringstream values(pdf.substr(position, 48));
        double x = 0.0, y = 0.0;
        if (values >> x >> y) minY = std::min(minY, y);
    }
    return minY;
}

// A table row taller than a page goes on over as many pages as it needs, in both renderers.
// Its lines used to run past the bottom of the page and get lost.
bool CheckTallTableRows() {
    std::string cell;
    for (int word = 0; word < 1500; word++) cell += "w" + std::to_string(10000 + word) + " ";
    const std::string table = "| Key | Value |\n|---|---|\n| tall | " + cell + "|\n| after | row |\n";
    const double bottomMargin = 54.0;   // PdfMargin::Normal()
    std::string pdf;
    if (!Build(table, pdf) || pdf.find("RayoMD Native Standard PDF") == std::string::npos) {
        std::cerr << "tall table row: standard PDF build mismatch" << std::endl;
        return false;
    }
    std::string shown;
    for (const StandardTextOp& op : StandardTextOps(pdf)) shown += op.text + " ";
    for (int word = 0; word < 1500; word++) {
        if (CountOccurrences(shown, "w" + std::to_string(10000 + word)) != 1) {
            std::cerr << "tall table row: word " << word << " is not shown exactly once" << std::endl;
            return false;
        }
    }
    if (shown.find("after row") == std::string::npos || MinTextStartY(pdf) < bottomMargin) {
        std::cerr << "tall table row: standard text below the page margin: " << MinTextStartY(pdf) << std::endl;
        return false;
    }
    std::string unicodePdf;
    if (!Build(u8"Za\u017C\u00F3\u0142\u0107\n\n" + table, unicodePdf) ||
        unicodePdf.find("RayoMD Native Tiny PDF") == std::string::npos || MinTextStartY(unicodePdf) < bottomMargin) {
        std::cerr << "tall table row: Unicode text below the page margin: " << MinTextStartY(unicodePdf) << std::endl;
        return false;
    }
    return true;
}

// Without footnotes "[^1]: text" stays visible text, never a link reference definition, which
// disappears ("[label]: target"); CheckFootnotes covers footnotes.
bool CheckFootnoteDefinitions() {
    using TinyPdf::Internal::Block;
    using TinyPdf::Internal::BlockType;
    const std::string source = "Claim[^1] with [a link][ref].\n\n[^1]: Source.\n\n[ref]: https://example.com/ref\n";
    std::vector<Block> blocks = TinyPdf::Internal::ParseMarkdown(source);
    if (blocks.size() != 2 || blocks[1].type != BlockType::Paragraph || blocks[1].text != "[^1]: Source.") {
        std::cerr << "footnote definition is not kept as text" << std::endl;
        return false;
    }
    bool resolved = false;
    for (const TinyPdf::Internal::InlineSpan& span : TinyPdf::Internal::ParseInlineSpans(blocks[0].text)) {
        resolved = resolved || span.url == "https://example.com/ref";
    }
    if (!resolved) {
        std::cerr << "reference definition next to a footnote no longer resolves" << std::endl;
        return false;
    }
    return true;
}

// List items nest by the column of their content, as in CommonMark: two spaces put an item
// under "- ", three under "1. ".
bool CheckListNestingByContentColumn() {
    using TinyPdf::Internal::Block;
    using TinyPdf::Internal::BlockType;
    const std::string source =
        "- parent\n"
        "  - child\n"
        "    - grandchild\n"
        "- sibling\n"
        "\n"
        "1. first\n"
        "   1. nested\n"
        "2. second\n";
    std::vector<Block> blocks = TinyPdf::Internal::ParseMarkdown(source);
    auto item = [](const Block& block, BlockType type, const char* text) {
        return block.type == type && block.text == text;
    };
    if (blocks.size() != 4 || !item(blocks[0], BlockType::Bullet, "parent") || blocks[0].children.size() != 1 ||
        !item(blocks[0].children[0], BlockType::Bullet, "child") || blocks[0].children[0].children.size() != 1 ||
        !item(blocks[0].children[0].children[0], BlockType::Bullet, "grandchild") ||
        !item(blocks[1], BlockType::Bullet, "sibling") || !item(blocks[2], BlockType::Numbered, "first") ||
        blocks[2].children.size() != 1 || !item(blocks[2].children[0], BlockType::Numbered, "nested") ||
        !item(blocks[3], BlockType::Numbered, "second")) {
        std::cerr << "list nesting by content column mismatch" << std::endl;
        return false;
    }
    return true;
}

// The standard fonts are declared with WinAnsiEncoding, under which ' and ` are a straight
// quote and a grave accent; without an encoding, viewers show curly quotes for them.
bool CheckStandardFontEncoding() {
    std::string pdf;
    if (!Build("It's `code` with 'quotes'.\n", pdf) || pdf.find("RayoMD Native Standard PDF") == std::string::npos ||
        CountOccurrences(pdf, "/Encoding /WinAnsiEncoding") != 3) {
        std::cerr << "standard fonts lack WinAnsiEncoding" << std::endl;
        return false;
    }
    return true;
}

// Standard-font lines are measured with the AFM advances of the font that shows them, so no
// line ends past the right margin, also in capitals and in bold headings.
bool CheckStandardLinesFitMargin() {
    struct Advance { char c; int regular; int bold; };     // Helvetica / Helvetica-Bold AFM
    const Advance advances[] = { {'W', 944, 944}, {'M', 833, 833}, {'i', 222, 278}, {' ', 278, 278} };
    std::string words;
    for (int i = 0; i < 60; i++) words += i % 3 == 0 ? "WWWWWW " : i % 3 == 1 ? "MiMiM " : "WiWi ";
    std::string pdf;
    const std::string boldWords = "**" + words.substr(0, words.size() - 1) + "**";
    if (!Build("# " + words + "\n\n" + words + "\n\n" + boldWords + "\n\n> " + words + "\n", pdf) ||
        pdf.find("RayoMD Native Standard PDF") == std::string::npos) {
        std::cerr << "standard line width: build mismatch" << std::endl;
        return false;
    }
    const double rightEdge = 595.0 - 54.0;     // A4 width less the PdfMargin::Normal() margin
    size_t lines = 0;
    for (const StandardTextOp& op : StandardTextOps(pdf)) {
        int units = 0;
        for (char c : op.text) {
            const Advance* advance = std::find_if(std::begin(advances), std::end(advances),
                [c](const Advance& entry) { return entry.c == c; });
            if (advance == std::end(advances) || (op.font != "F1" && op.font != "F2")) {
                std::cerr << "standard line width: unexpected text " << op.font << " (" << op.text << ")" << std::endl;
                return false;
            }
            units += op.font == "F2" ? advance->bold : advance->regular;
        }
        if (op.x + units * op.size / 1000.0 > rightEdge + 0.01) {
            std::cerr << "standard line ends past the margin: " << op.x + units * op.size / 1000.0 << std::endl;
            return false;
        }
        lines++;
    }
    if (lines < 12) {
        std::cerr << "standard line width: the text did not wrap" << std::endl;
        return false;
    }
    return true;
}

// A word is everything between two white spaces of the source, so link, code and emphasis
// boundaries inside it add no space: "[GitHub](url)." shows "GitHub.".
bool CheckWordsAcrossInlineBoundaries() {
    std::string pdf;
    if (!Build("See [GitHub](https://github.com). Call `foo()`, then **bar**.\n", pdf)) {
        std::cerr << "inline boundary words: build failed" << std::endl;
        return false;
    }
    std::string shown;
    for (const StandardTextOp& op : StandardTextOps(pdf)) shown += op.text;
    if (shown != "See GitHub. Call foo(), then bar.") {
        std::cerr << "inline boundary words: shown as \"" << shown << "\"" << std::endl;
        return false;
    }
    return true;
}

// The standard fonts show emphasis: bold in Helvetica-Bold, italic in Helvetica-Oblique,
// both in Helvetica-BoldOblique, strike-through as a line; code stays in Courier. The
// italic faces go only into documents that use them.
bool CheckStandardFontEmphasis() {
    std::string pdf;
    if (!Build("Plain **bold** *italic* ***both*** ~~gone~~ `code` end.\n", pdf) ||
        pdf.find("/BaseFont /Helvetica-Oblique ") == std::string::npos ||
        pdf.find("/BaseFont /Helvetica-BoldOblique ") == std::string::npos ||
        pdf.find(" RG 0.55 w ") == std::string::npos) {
        std::cerr << "standard-font emphasis: fonts or strike line missing" << std::endl;
        return false;
    }
    const std::pair<const char*, const char*> expected[] = {
        {"Plain", "F1"}, {"bold", "F2"}, {"italic", "F4"}, {"both", "F5"}, {"gone", "F1"}, {"code", "F3"}};
    const std::vector<StandardTextOp> ops = StandardTextOps(pdf);
    for (const auto& [word, font] : expected) {
        const auto op = std::find_if(ops.begin(), ops.end(),
            [&](const StandardTextOp& candidate) { return candidate.text.find(word) != std::string::npos; });
        if (op == ops.end() || op->font != font) {
            std::cerr << "standard-font emphasis: \"" << word << "\" is not in " << font << std::endl;
            return false;
        }
    }
    std::string plainPdf;
    if (!Build("Plain words only.\n", plainPdf) || plainPdf.find("Oblique") != std::string::npos) {
        std::cerr << "standard-font emphasis: italic faces in a document without emphasis" << std::endl;
        return false;
    }
    return true;
}

// Link text is inline Markdown of its own: emphasis and code inside it keep the link, the
// emphasis around a link applies to it, and "[![alt](src)](target)" is a linked image, inline
// as its "image: alt" text and on a line of its own as an image block with a link.
bool CheckLinkText() {
    using TinyPdf::Internal::InlineSpan;
    struct Case { const char* input; const char* text; const char* url; bool bold; bool italic; bool code; };
    const Case cases[] = {
        {"[**bold** text](https://e.com/a)", "bold", "https://e.com/a", true, false, false},
        {"[**bold** text](https://e.com/a)", " text", "https://e.com/a", false, false, false},
        {"*[x](https://e.com/b)*", "x", "https://e.com/b", false, true, false},
        {"[a\\*b](https://e.com/c)", "a*b", "https://e.com/c", false, false, false},
        {"[`f()`](https://e.com/d)", "f()", "https://e.com/d", false, false, true},
        {"[![CI](b.svg)](https://e.com/ci) after", "image: CI", "https://e.com/ci", false, false, false},
        {"[![CI](b.svg)](https://e.com/ci) after", " after", "", false, false, false},
        {"[![x](y) and", "[image: x and", "", false, false, false},
    };
    for (const Case& item : cases) {
        bool found = false;
        for (const InlineSpan& span : TinyPdf::Internal::ParseInlineSpans(item.input)) {
            found = found || (span.text == item.text && span.url == item.url && span.bold == item.bold &&
                span.italic == item.italic && span.code == item.code);
        }
        if (!found) {
            std::cerr << "link text mismatch: " << item.input << " has no \"" << item.text << "\"" << std::endl;
            return false;
        }
    }

    using TinyPdf::Internal::BlockType;
    const auto blocks = TinyPdf::Internal::ParseMarkdown("[![Logo](docs/assets/branding/rayomd.png)](https://e.com/logo)\n");
    if (blocks.size() != 1 || blocks[0].type != BlockType::Image || blocks[0].text != "Logo" || !blocks[0].image ||
        blocks[0].image->src != "docs/assets/branding/rayomd.png" || blocks[0].image->link != "https://e.com/logo") {
        std::cerr << "linked image block mismatch" << std::endl;
        return false;
    }
    // One annotation for link text in several styles, over all of it.
    const std::string images =
        "[![Logo](docs/assets/branding/rayomd.png)](https://e.com/logo)\n\n"
        "[![Gone](missing-image.png)](https://e.com/gone)\n\n"
        "See [**bold** and `code`](https://e.com/one).\n";
    for (const std::string& document : {images, u8"Za\u017C\u00F3\u0142\u0107\n\n" + images}) {
        std::string pdf;
        std::array<double, 4> logo{};
        std::array<double, 4> gone{};
        std::array<double, 4> one{};
        if (!Build(document, pdf) || !FindLinkRectangle(pdf, "https://e.com/logo", logo) ||
            !FindLinkRectangle(pdf, "https://e.com/gone", gone) || logo[2] - logo[0] < 50.0 ||
            logo[3] - logo[1] < 50.0 || gone[3] > logo[1] || !FindLinkRectangle(pdf, "https://e.com/one", one) ||
            CountOccurrences(pdf, "/URI (https://e.com/one)") != 1 || one[2] - one[0] < 60.0) {
            std::cerr << "linked image or multi-style link annotations mismatch" << std::endl;
            return false;
        }
    }
    return true;
}

// A TrueType collection that holds one font: square glyphs for ' ' (empty), 'A', '?' and
// U+4E2D, 1000 units per em and the bounding box [0 0 700 700], and for U+AC00 a composite
// glyph with no advance of its own, whose one component (U+4E2D's square) is flagged
// USE_MY_METRICS.
std::string MinimalTrueTypeCollection() {
    auto u16 = [](std::string& out, int value) {
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
        out.push_back(static_cast<char>(value & 0xFF));
    };
    auto u32 = [&](std::string& out, uint32_t value) {
        u16(out, static_cast<int>(value >> 16));
        u16(out, static_cast<int>(value & 0xFFFF));
    };
    std::string square;
    for (int value : {1, 0, 0, 700, 700, 3, 0}) u16(square, value);    // contours, box, end point, no hints
    square.append(4, '\x01');                                           // on-curve points, 16-bit deltas
    for (int value : {0, 700, 0, -700, 0, 0, 700, 0}) u16(square, value);
    std::string glyf = square + square + square;                        // glyphs 2 'A', 3 '?', 4 U+4E2D
    for (int value : {-1, 0, 0, 700, 700, 0x0202, 4}) u16(glyf, value); // glyph 5 U+AC00: one component,
    u16(glyf, 0);                                                       // byte offsets 0 0
    std::string loca;
    for (uint32_t offset : {0u, 0u, 0u, 34u, 68u, 102u, 118u}) u32(loca, offset);  // 34 bytes a square; 0 and 1 empty
    std::string head;
    u32(head, 0x00010000);
    u32(head, 0x00010000);
    u32(head, 0);
    u32(head, 0x5F0F3CF5);
    for (int value : {0, 1000}) u16(head, value);
    head.append(16, '\0');                                              // created, modified
    for (int value : {0, 0, 700, 700, 0, 8, 2, 1, 0}) u16(head, value); // box, style, ppem, hint, long loca
    std::string hhea;
    u32(hhea, 0x00010000);
    for (int value : {800, -200, 0, 800, 0, 0, 700, 1, 0, 0, 0, 0, 0, 0, 0, 6}) u16(hhea, value);
    std::string maxp;
    u32(maxp, 0x00010000);
    for (int value : {6, 4, 1, 4, 1, 2, 0, 0, 0, 0, 0, 0, 1, 1}) u16(maxp, value);
    std::string hmtx;
    for (int glyph = 0; glyph < 6; glyph++) {
        u16(hmtx, glyph == 5 ? 0 : 800);
        u16(hmtx, 0);
    }
    std::string cmap;
    for (int value : {0, 1, 3, 1}) u16(cmap, value);
    u32(cmap, 12);
    const int codes[] = {0x20, 0x3F, 0x41, 0x4E2D, 0xAC00, 0xFFFF};
    const int glyphs[] = {1, 3, 2, 4, 5, 0};
    // Format 4: length, language, two bytes per segment, search range, selector, range shift.
    for (int value : {4, 14 + 2 + 6 * 8, 0, 12, 8, 2, 4}) u16(cmap, value);
    for (int code : codes) u16(cmap, code);                             // end codes
    u16(cmap, 0);
    for (int code : codes) u16(cmap, code);                             // start codes
    for (int index = 0; index < 6; index++) u16(cmap, (glyphs[index] - codes[index]) & 0xFFFF);
    for (int index = 0; index < 6; index++) u16(cmap, 0);

    const std::pair<const char*, const std::string*> tables[] = {
        {"cmap", &cmap}, {"glyf", &glyf}, {"head", &head}, {"hhea", &hhea}, {"hmtx", &hmtx}, {"loca", &loca},
        {"maxp", &maxp}};
    const size_t base = 16;                                             // "ttcf", version, count, one offset
    std::string font;
    u32(font, 0x00010000);
    for (int value : {7, 64, 2, 48}) u16(font, value);
    size_t offset = base + 12 + 7 * 16;
    std::string data;
    for (const auto& [tag, table] : tables) {
        font.append(tag, 4);
        u32(font, 0);
        u32(font, static_cast<uint32_t>(offset + data.size()));
        u32(font, static_cast<uint32_t>(table->size()));
        data += *table;
        while (data.size() % 4) data.push_back('\0');
    }
    std::string collection = "ttcf";
    u32(collection, 0x00010000);
    u32(collection, 1);
    u32(collection, base);
    return collection + font + data;
}

// A bare http(s) URL or email address in text is a link, as on GitHub: without the
// punctuation that ends a sentence or a ')' that closes no '(' of its own, and with its
// character references decoded. Not in code, not inside other links, not after a letter, and
// not an address without a dot in its domain. In both renderers and in table cells.
bool CheckAutolinks() {
    const std::string document =
        "See https://a.example/p?x=1&amp;y=2. Or (http://b.example/c_(d)) and https://e.example/f), done!\n"
        "Mail j.doe+tag@mail.example.com, not user@localhost, @name, `https://code.example` or xhttps://g.example.\n\n"
        "| https://h.example | team@example.org |\n|---|---|\n| [label](https://i.example) | x |\n";
    for (const std::string& text : { document, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + document }) {
        std::string pdf;
        if (!Build(text, pdf)) return false;
        std::vector<std::string> uris;
        for (size_t at = pdf.find("/URI ("); at != std::string::npos; at = pdf.find("/URI (", at + 1)) {
            const size_t end = pdf.find(") >>", at);
            uris.push_back(pdf.substr(at + 6, end - at - 6));
        }
        const std::vector<std::string> expected = { "https://a.example/p?x=1&y=2", "http://b.example/c_\\(d\\)",
            "https://e.example/f", "mailto:j.doe+tag@mail.example.com", "https://h.example", "mailto:team@example.org",
            "https://i.example" };
        if (uris != expected) {
            std::cerr << "autolinks mismatch (" << (text == document ? "standard" : "Unicode") << " renderer):";
            for (const std::string& uri : uris) std::cerr << " " << uri;
            std::cerr << std::endl;
            return false;
        }
    }
    return true;
}

// "[label]" and "![label]" alone are links and images when the label is defined, in any case
// and spacing. Undefined labels, escaped and code brackets and footnote markers stay text; of
// "[[docs]]", whose outer label holds a '[', the inner one resolves. In both renderers.
bool CheckShortcutReferences() {
    const std::string document =
        "See [Docs] and [the  DOCS] and ![Logo] and [missing], \\[docs], `[docs]`, [^1], [[docs]], [docs][] and "
        "[x][docs].\n\n[docs]: https://docs.example\n[the docs]: https://the.example\n[logo]: logo.png\n";
    for (const std::string& text : { document, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + document }) {
        std::string pdf;
        const bool unicode = text != document;
        if (!Build(text, pdf)) return false;
        std::vector<std::string> uris;
        for (size_t at = pdf.find("/URI ("); at != std::string::npos; at = pdf.find("/URI (", at + 1)) {
            const size_t end = pdf.find(") >>", at);
            uris.push_back(pdf.substr(at + 6, end - at - 6));
        }
        const std::vector<std::string> expected = { "https://docs.example", "https://the.example", "https://docs.example",
            "https://docs.example", "https://docs.example" };
        if (uris != expected || (!unicode && (pdf.find("image: Logo") == std::string::npos ||
            pdf.find("[missing], [docs],") == std::string::npos || pdf.find("(, [^1],) Tj") == std::string::npos))) {
            std::cerr << "shortcut references mismatch (" << (unicode ? "Unicode" : "standard") << " renderer):";
            for (const std::string& uri : uris) std::cerr << " " << uri;
            std::cerr << std::endl;
            return false;
        }
    }
    return true;
}

// A top-level quote whose first line is "[!NOTE]", "[!TIP]", "[!IMPORTANT]", "[!WARNING]" or
// "[!CAUTION]" (any case) with text after it is an alert: a bar in its colour and its title,
// without the marker. A marker alone, an unknown one and one in a list stay quoted text.
bool CheckAlerts() {
    const std::string document =
        "> [!NOTE]\n> Useful information.\n\n> [!tip]\n> A helpful hint.\n\n> [!IMPORTANT]\n> Key information.\n"
        "> > A nested quote.\n\n> [!WARNING]\n> Urgent.\n\n> [!CAUTION]\n> Risks.\n\n> [!NOTE]\n\n"
        "> [!FOO]\n> Not an alert.\n\n- > [!NOTE]\n  > In a list.\n";
    for (const std::string& text : { document, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + document }) {
        std::string pdf;
        const bool unicode = text != document;
        bool ok = Build(text, pdf);
        for (const char* color : { "0.04 0.41 0.85 rg", "0.10 0.50 0.22 rg", "0.51 0.31 0.87 rg", "0.60 0.40 0.00 rg",
                 "0.82 0.14 0.18 rg" }) {
            ok = ok && pdf.find(color) != std::string::npos;
        }
        if (!unicode) {
            for (const char* title : { "(Note) Tj", "(Tip) Tj", "(Important) Tj", "(Warning) Tj", "(Caution) Tj" }) {
                ok = ok && CountOccurrences(pdf, title) == 1;
            }
            ok = ok && CountOccurrences(pdf, "[!NOTE]") == 2 && CountOccurrences(pdf, "[!FOO]") == 1 &&
                pdf.find("[!tip]") == std::string::npos;
        }
        if (!ok) {
            std::cerr << "alerts mismatch (" << (unicode ? "Unicode" : "standard") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

// A heading never ends a page: its page keeps room under its first line for the space after it
// and two lines of the text that follows, or the heading starts the next page. Swept across
// the foot of a page, in both renderers (Modern style, normal margin: body lines 15.525 high).
bool CheckHeadingKeep() {
    const double lowest = 54.0 + 22.0 * 1.35 + 8.0 + 2.0 * 11.5 * 1.35;
    for (const bool unicode : { false, true }) {
        for (int lines = 25; lines < 60; lines++) {
            std::string document = unicode ? "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" : "";
            for (int k = 0; k < lines; k++) document += "Filler " + std::to_string(k) + ".\n\n";
            document += "## Keep\n\nText after the heading.\n";
            std::string pdf;
            if (!Build(document, pdf)) return false;
            const size_t title = pdf.find("/Title (Keep)");
            const size_t dest = title == std::string::npos ? title : pdf.find(" /XYZ null ", title);
            const double top = dest == std::string::npos ? 0.0 : std::strtod(pdf.c_str() + dest + 11, nullptr);
            if (top < lowest - 0.01) {
                std::cerr << "heading keep: \"## Keep\" at y " << top << " after " << lines << " paragraphs ("
                          << (unicode ? "Unicode" : "standard") << " renderer)" << std::endl;
                return false;
            }
        }
    }
    return true;
}

// A table that goes on over page breaks repeats its header row, tinted and bold, at the top of
// every page it reaches; a table on one page shows it once. In both renderers, and for a table
// with a formula, which another loop draws.
bool CheckTableHeaderRepeat() {
    std::string table = "| Name | Value |\n|---|---|\n";
    for (int row = 0; row < 120; row++) table += "| item " + std::to_string(row) + " | " + std::to_string(row * 7) + " |\n";
    for (const std::string& prefix : { std::string(), std::string("Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n") }) {
        for (const std::string& body : { table, table + "| $x^2$ | y |\n", std::string("| Name | Value |\n|---|---|\n| a | 1 |\n") }) {
            std::string pdf;
            if (!Build(prefix + body, pdf)) return false;
            const size_t pages = CountOccurrences(pdf, "/Type /Page ");
            const size_t headers = CountOccurrences(pdf, "0.91 0.93 0.95 rg");
            const bool single = body.size() < 100;
            if ((single ? pages != 1 : pages < 3) || headers != pages ||
                (prefix.empty() && CountOccurrences(pdf, "(Name) Tj") != pages)) {
                std::cerr << "table header repeat: " << headers << " headers on " << pages << " pages ("
                          << (prefix.empty() ? "standard" : "Unicode") << " renderer)" << std::endl;
                return false;
            }
        }
    }
    return true;
}

// The page size sets every media box, the width text wraps to, where "#top" leads and where
// page numbers sit; sides stay within 144 to 14,400 points, and a margin too wide for the page
// shrinks to leave 72 points of text. ParsePageSize takes the presets, "-landscape" and sizes
// with a unit, and nothing else. In both renderers.
bool CheckPageSize() {
    using TinyPdf::Internal::ParsePageSize;
    TinyPdf::PdfPageSize size;
    const bool parsed = ParsePageSize("Letter", size) && size.width == 612.0 && size.height == 792.0 &&
        ParsePageSize("a4-landscape", size) && size.width == 842.0 && size.height == 595.0 &&
        ParsePageSize("8.5x11in", size) && size.width == 612.0 && size.height == 792.0 &&
        ParsePageSize("21x29.7cm", size) && std::abs(size.width - 595.2756) < 0.001 &&
        !ParsePageSize("210x297", size) && !ParsePageSize("x297mm", size) && !ParsePageSize("1x1mm", size) &&
        !ParsePageSize("letter-portrait", size) && !ParsePageSize("210x297mm-landscape", size) &&
        !ParsePageSize("b5", size);
    if (!parsed) {
        std::cerr << "page size parsing mismatch" << std::endl;
        return false;
    }
    std::string document = "# Size\n\n[top](#top)\n\n";
    for (int line = 0; line < 80; line++) document += "Line " + std::to_string(line) + " of words that wrap across the page.\n\n";
    const auto numberX = [](const std::string& pdf) {
        const size_t text = pdf.find(" Tm (1 / ");
        const size_t matrix = text == std::string::npos ? text : pdf.rfind("1 0 0 1 ", text);
        return matrix == std::string::npos ? -1.0 : std::strtod(pdf.c_str() + matrix + 8, nullptr);
    };
    for (const std::string& text : { document, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + document }) {
        TinyPdf::PdfOptions options;
        options.style = TinyPdf::PdfStyle::Modern;
        options.pageNumbers = true;
        std::string a4;
        std::string letter;
        std::string landscape;
        std::string tiny;
        bool ok = TinyPdf::BuildPdf(text, options, a4).Ok() && ValidPdf(a4);
        options.pageSize = TinyPdf::PdfPageSize::Letter();
        ok = ok && TinyPdf::BuildPdf(text, options, letter).Ok() && ValidPdf(letter);
        options.pageSize = { 842.0, 595.0 };
        ok = ok && TinyPdf::BuildPdf(text, options, landscape).Ok() && ValidPdf(landscape);
        options.pageSize = { 10.0, 1.0e9 };
        options.margin = TinyPdf::PdfMargin::Wide();
        ok = ok && TinyPdf::BuildPdf(text, options, tiny).Ok() && ValidPdf(tiny);
        const size_t landscapePages = CountOccurrences(landscape, "/Type /Page ");
        ok = ok && CountOccurrences(letter, "/MediaBox [0 0 612 792]") == CountOccurrences(letter, "/Type /Page ") &&
            letter.find("/XYZ null 792 null") != std::string::npos &&
            CountOccurrences(landscape, "/MediaBox [0 0 842 595]") == landscapePages &&
            landscapePages > CountOccurrences(a4, "/Type /Page ") && tiny.find("/MediaBox [0 0 144 14400]") != std::string::npos &&
            std::abs(numberX(letter) - numberX(a4) - 8.5) < 0.02;
        if (!ok) {
            std::cerr << "page size mismatch (" << (text == document ? "standard" : "Unicode") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

// A batch report line stays valid JSON for any file name: quotes, backslashes and control
// characters are escaped, UTF-8 passes through, and bytes that are not UTF-8 (possible in Linux
// names) become U+FFFD instead of breaking the line.
bool CheckReportJson() {
    std::string escaped;
    RayoMd::Batch::AppendJsonString(escaped, std::string("q\"b\\n\nt\t\x01\x7F") + "\xC5\xBC" + "\xFF" + "\xC5");
    const std::string expected = std::string("q\\\"b\\\\n\\nt\\t\\u0001\\u007f") + "\xC5\xBC" + "\\ufffd\\ufffd";
    if (escaped != expected) {
        std::cerr << "report JSON escaping mismatch: " << escaped << std::endl;
        return false;
    }
    return true;
}

// The streams of a PDF in file order, each its dictionary and payload.
std::vector<std::pair<std::string, std::string>> PdfStreams(const std::string& pdf) {
    std::vector<std::pair<std::string, std::string>> streams;
    for (size_t at = pdf.find(" 0 obj\n"); at != std::string::npos; at = pdf.find(" 0 obj\n", at)) {
        at += 7;
        const size_t open = pdf.find("\nstream\n", at);
        if (open == std::string::npos || pdf.find("\nendobj\n", at) < open) continue;
        std::string dictionary = pdf.substr(at, open - at);
        const size_t length = std::strtoull(dictionary.c_str() + dictionary.find("/Length ") + 8, nullptr, 10);
        at = open + 8 + length;
        streams.emplace_back(std::move(dictionary), pdf.substr(open + 8, length));
    }
    return streams;
}

// Whether the zlib stream `packed` holds exactly `expected`, checked as zlib's uncompress() would:
// its header, its deflate data (libdeflate's decompressor) and, computed here a byte at a time,
// its Adler-32 trailer.
bool Inflates(std::string_view packed, const std::string& expected) {
    const auto* in = reinterpret_cast<const uint8_t*>(packed.data());
    if (packed.size() < 6 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return false;
    std::string plain(expected.size(), '\0');
    libdeflate_decompressor* const decompressor = libdeflate_alloc_decompressor();
    size_t consumed = 0;
    size_t size = 0;
    const bool inflated = decompressor && libdeflate_deflate_decompress_ex(decompressor, in + 2, packed.size() - 2,
        &plain[0], plain.size(), &consumed, &size) == LIBDEFLATE_SUCCESS;
    libdeflate_free_decompressor(decompressor);
    uint32_t a = 1;
    uint32_t b = 0;
    for (unsigned char ch : plain) {
        a = (a + ch) % 65521;
        b = (b + a) % 65521;
    }
    const uint8_t* const trailer = in + 2 + consumed;
    return inflated && size == expected.size() && plain == expected && packed.size() == consumed + 6 &&
        ((uint32_t)trailer[0] << 24 | (uint32_t)trailer[1] << 16 | (uint32_t)trailer[2] << 8 | trailer[3]) == (b << 16 | a);
}

// --compress: every content stream, font program and CMap is FlateDecode where that makes it
// smaller and inflates to what the uncompressed PDF has, the font keeping its own size as
// /Length1; a stream compression cannot shrink stays as it is. Output does not depend on what
// the thread compressed before, nor on other threads compressing at the same time, and the
// reversible profile still recovers its source. In both renderers.
bool CheckCompression() {
    // The encoder alone, in its three modes (image rows also fast), at the edges of its blocks,
    // window and match lengths, on runs and on data it cannot compress, within the size it promises.
    RayoMd::Flate::Scratch scratch;
    uint64_t seed = 88172645463325252ull;
    const auto next = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    std::vector<std::string> inputs = { "", "a", std::string(300000, '\0') };
    for (size_t size : { 7, 8, 9, 258, 259, 32768, 32769, 65277, 65278, 65279, 65535, 65536, 140000 }) {
        std::string noise(size, '\0');
        std::string text(size, '\0');
        for (size_t i = 0; i < size; i++) {
            noise[i] = (char)next();
            text[i] = (char)("BT 0 0 1 rg ()Tj\n"[next() % 17]);
        }
        inputs.push_back(std::move(noise));
        inputs.push_back(std::move(text));
    }
    std::string skewed;   // Fibonacci counts: Huffman lengths past 15 bits before limiting
    for (size_t symbol = 0, count = 1, previous = 1; symbol < 24; symbol++, std::swap(count, previous), count += previous) {
        for (size_t k = 0; k < count; k++) skewed.push_back((char)symbol);
    }
    for (size_t i = skewed.size(); i > 1; i--) std::swap(skewed[i - 1], skewed[next() % i]);
    inputs.push_back(std::move(skewed));
    // Image rows of several segments, repeating with a period longer than a segment's history.
    std::string rows(size_t(2300000), '\0');
    for (size_t i = 0; i < rows.size(); i++) rows[i] = (char)(i % 20000 < 9000 ? i % 251 : next() % 7);
    inputs.push_back(std::move(rows));
    using Compress = size_t (*)(std::string_view, uint8_t*, RayoMd::Flate::Scratch&);
    const Compress modes[] = { RayoMd::Flate::ZlibCompress, [](std::string_view in, uint8_t* out, RayoMd::Flate::Scratch& work) {
        return RayoMd::Flate::ZlibCompressImage(in, out, work);
    }, [](std::string_view in, uint8_t* out, RayoMd::Flate::Scratch& work) {
        return RayoMd::Flate::ZlibCompressImage(in, out, work, true);
    } };
    for (const std::string& input : inputs) {
        for (const Compress compress : modes) {
            const size_t bound = compress == modes[0] ? RayoMd::Flate::ZlibBound(input.size())
                                                      : RayoMd::Flate::ZlibImageBound(input.size());
            std::string packed(bound, '\0');
            packed.resize(compress(input, reinterpret_cast<uint8_t*>(&packed[0]), scratch));
            if (packed.size() > bound - 8 || !Inflates(packed, input)) {
                std::cerr << "DEFLATE round trip failed for " << input.size() << " bytes" << std::endl;
                return false;
            }
        }
    }
    // Segments of any sizes, each ending at any bit, put together make a valid stream; every other
    // trial in the fast mode, with the tables the other mode filled.
    for (const std::string& input : inputs) {
        if (input.size() > 300000) continue;
        for (int trial = 0; trial < 8; trial++) {
            const bool fast = (trial & 1) != 0;
            std::string packed("\x78\x5E", 2);
            for (size_t start = 0;;) {
                const size_t size = std::min<size_t>(input.size() - start, next() % 9000);
                const size_t history = std::min(start, RayoMd::Flate::kImageSegmentHistory);
                const bool last = start + size == input.size();
                std::string segment(RayoMd::Flate::ImageSegmentBound(size), '\0');
                segment.resize(RayoMd::Flate::DeflateImageSegment(
                    reinterpret_cast<const uint8_t*>(input.data()) + start - history, history, size, last,
                    reinterpret_cast<uint8_t*>(&segment[0]), scratch, nullptr, fast));
                packed += segment;
                start += size;
                if (last) break;
            }
            const uint32_t adler = RayoMd::Flate::Adler32(reinterpret_cast<const uint8_t*>(input.data()), input.size());
            for (int shift = 24; shift >= 0; shift -= 8) packed.push_back((char)(adler >> shift));
            if (!Inflates(packed, input)) {
                std::cerr << "DEFLATE segments of " << input.size() << " bytes do not inflate" << std::endl;
                return false;
            }
        }
    }
    // Image planes written a piece at a time, up to 300,000 bytes, side by side, straight into the
    // segment buffers or copied, and compressed by helper threads meanwhile, are the bytes of each
    // plane compressed whole, however many helpers; an abandoned plane gets no stream, and an
    // encoder left half written stops.
    const std::string& alphaRows = inputs.back();
    std::string wholeAlpha(RayoMd::Flate::ZlibImageBound(alphaRows.size()), '\0');
    wholeAlpha.resize(RayoMd::Flate::ZlibCompressImage(alphaRows, reinterpret_cast<uint8_t*>(&wholeAlpha[0]), scratch));
    for (const std::string& input : inputs) {
        std::string whole(RayoMd::Flate::ZlibImageBound(input.size()), '\0');
        whole.resize(RayoMd::Flate::ZlibCompressImage(input, reinterpret_cast<uint8_t*>(&whole[0]), scratch));
        for (const unsigned helpers : { 0u, 1u, 3u }) {
            for (const int ending : { 0, 1, 2 }) {   // finished, alpha abandoned, left half written
                const std::string* const planes[2] = { &input, &alphaRows };
                const size_t sizes[2] = { input.size(), alphaRows.size() };
                TinyPdf::Internal::ImagePlanesEncoder encoder(sizes, 2, helpers);
                size_t at[2] = {};
                const size_t ends[2] = { ending == 2 ? input.size() / 2 : input.size(), ending == 1 ? 0 : alphaRows.size() };
                bool written = true;
                while (written && (at[0] < ends[0] || at[1] < ends[1])) {
                    for (size_t plane = 0; plane < 2 && written; plane++) {
                        const size_t piece = std::min<size_t>(ends[plane] - at[plane], next() % 300000 + 1);
                        if (piece == 0) continue;
                        const char* const bytes = planes[plane]->data() + at[plane];
                        if (uint8_t* const reserved = encoder.Reserve(plane, piece)) {
                            std::memcpy(reserved, bytes, piece);
                            encoder.Commit(plane, piece);
                        } else {
                            written = encoder.Append(plane, reinterpret_cast<const uint8_t*>(bytes), piece);
                        }
                        at[plane] += piece;
                    }
                    if (helpers == 0) encoder.EncodeReady();
                }
                if (ending == 2) continue;   // the destructor abandons both planes
                if (ending == 1) encoder.Abandon(1);
                std::string streams[2];
                if (!written || !encoder.Finish(streams) || streams[0] != whole ||
                    streams[1] != (ending == 1 ? std::string() : wholeAlpha)) {
                    std::cerr << "Image planes of " << input.size() << " bytes differ with " << helpers << " helpers"
                              << std::endl;
                    return false;
                }
            }
        }
    }
    // In the fast mode too a plane's bytes do not depend on the helpers.
    std::string fastAlpha(RayoMd::Flate::ZlibImageBound(alphaRows.size()), '\0');
    fastAlpha.resize(RayoMd::Flate::ZlibCompressImage(alphaRows, reinterpret_cast<uint8_t*>(&fastAlpha[0]), scratch, true));
    for (const unsigned helpers : { 0u, 3u }) {
        const size_t sizes[1] = { alphaRows.size() };
        TinyPdf::Internal::ImagePlanesEncoder encoder(sizes, 1, helpers, true);
        bool written = true;
        for (size_t at = 0; written && at < alphaRows.size();) {
            const size_t piece = std::min<size_t>(alphaRows.size() - at, next() % 300000 + 1);
            written = encoder.Append(0, reinterpret_cast<const uint8_t*>(alphaRows.data()) + at, piece);
            at += piece;
            if (helpers == 0) encoder.EncodeReady();
        }
        std::string streams[1];
        if (!written || !encoder.Finish(streams) || streams[0] != fastAlpha || !Inflates(fastAlpha, alphaRows)) {
            std::cerr << "Fast image planes differ with " << helpers << " helpers" << std::endl;
            return false;
        }
    }
    // RayoMD's inflater gives back every input of both modes, taken in pieces of any size.
    const auto inflateInPieces = [&](const std::string& packed, size_t size, std::string& out) {
        RayoMd::Inflate::ZlibReader reader;
        const size_t largest = next() % 70000 + 1;
        if (!reader.Start(reinterpret_cast<const uint8_t*>(packed.data()), packed.size(), size, largest)) return false;
        out.clear();
        while (out.size() < size) {
            const size_t piece = std::min<size_t>(size - out.size(), next() % largest + 1);
            const uint8_t* const taken = reader.Take(piece);
            if (!taken) return false;
            out.append(reinterpret_cast<const char*>(taken), piece);
        }
        return reader.Finish();
    };
    for (const std::string& input : inputs) {
        for (const Compress compress : modes) {
            const size_t bound = compress == modes[0] ? RayoMd::Flate::ZlibBound(input.size())
                                                      : RayoMd::Flate::ZlibImageBound(input.size());
            std::string packed(bound, '\0');
            packed.resize(compress(input, reinterpret_cast<uint8_t*>(&packed[0]), scratch));
            std::string out;
            if (!inflateInPieces(packed, input.size(), out) || out != input) {
                std::cerr << "RayoMD's inflater fails on " << input.size() << " bytes" << std::endl;
                return false;
            }
        }
    }
    // It refuses what zlib refuses: a stored block whose length and its complement disagree, a
    // reserved block type, a distance before the start of the output, a wrong checksum, a stream
    // cut short, and output longer or shorter than the image.
    const std::string stored("\x78\x01\x01\x05\x00\xFA\xFF" "hello" "\x06\x2C\x02\x15", 16);
    std::string out;
    if (!inflateInPieces(stored, 5, out) || out != "hello") {
        std::cerr << "RayoMD's inflater fails on a stored block" << std::endl;
        return false;
    }
    std::string badComplement = stored;
    badComplement[5] = '\xFB';
    std::string badChecksum = stored;
    badChecksum[15] = '\x16';
    const std::pair<std::string, size_t> refused[] = { { badComplement, 5 }, { std::string("\x78\x01\x07\x00", 4), 1 },
        { std::string("\x78\x01\x03\x02\x00\x00\x00\x00\x00", 9), 3 }, { badChecksum, 5 }, { stored.substr(0, 14), 5 },
        { stored, 4 }, { stored, 6 } };
    for (const auto& [stream, size] : refused) {
        if (inflateInPieces(stream, size, out)) {
            std::cerr << "RayoMD's inflater accepts a stream zlib refuses" << std::endl;
            return false;
        }
    }
    // On streams of both modes with up to four bytes flipped, set, cut, inserted or removed, it
    // accepts exactly what libdeflate accepts, with the same bytes; libdeflate agreed with zlib's
    // uncompress() on 100,000 such streams (2026-10-13).
    std::vector<std::pair<std::string, size_t>> seeds;
    for (const std::string& input : inputs) {
        if (input.size() > 70000) continue;
        for (const Compress compress : modes) {
            std::string packed(RayoMd::Flate::ZlibImageBound(input.size()) + RayoMd::Flate::ZlibBound(input.size()), '\0');
            packed.resize(compress(input, reinterpret_cast<uint8_t*>(&packed[0]), scratch));
            seeds.emplace_back(std::move(packed), input.size());
        }
    }
    libdeflate_decompressor* const reference = libdeflate_alloc_decompressor();
    bool agreed = reference != nullptr;
    for (int mutation = 0; mutation < 6000 && agreed; mutation++) {
        auto [stream, size] = seeds[next() % seeds.size()];
        for (int edit = 0, edits = (int)(next() % 4) + 1; edit < edits && !stream.empty(); edit++) {
            const size_t at = next() % stream.size();
            switch (next() % 6) {
            case 0:
            case 1: stream[at] = (char)(stream[at] ^ (1 << (next() % 8))); break;
            case 2: stream[at] = (char)next(); break;
            case 3: stream.resize(at); break;
            case 4: stream.insert(at, 1, (char)next()); break;
            default: stream.erase(at, next() % 8 + 1); break;
            }
        }
        if (stream.size() >= 2 && next() % 4) stream.replace(0, 2, "\x78\x5E");   // reach the deflate data
        // libdeflate's raw decompressor in zlib's framing: header, exactly `size` bytes, checksum.
        const auto* const in = reinterpret_cast<const uint8_t*>(stream.data());
        std::string theirs(size, '\0');
        size_t used = 0;
        size_t got = 0;
        bool accepted = stream.size() >= 2 && (in[0] & 15) == 8 && (in[0] >> 4) <= 7 && ((in[0] << 8) | in[1]) % 31 == 0 &&
            !(in[1] & 0x20) && libdeflate_deflate_decompress_ex(reference, in + 2, stream.size() - 2, &theirs[0], size,
                &used, &got) == LIBDEFLATE_SUCCESS && got == size && stream.size() - 2 - used >= 4;
        if (accepted) {
            const uint8_t* const trailer = in + 2 + used;
            accepted = ((uint32_t)trailer[0] << 24 | (uint32_t)trailer[1] << 16 | (uint32_t)trailer[2] << 8 | trailer[3]) ==
                RayoMd::Flate::Adler32(reinterpret_cast<const uint8_t*>(theirs.data()), size);
        }
        agreed = inflateInPieces(stream, size, out) == accepted && (!accepted || out == theirs);
    }
    libdeflate_free_decompressor(reference);
    if (!agreed) {
        std::cerr << "RayoMD's inflater and libdeflate disagree on a mutated stream" << std::endl;
        return false;
    }
    for (const uint32_t first : { 1u, 0x12345678u, 0xFFF0FFF0u }) {
        for (const std::string& input : inputs) {
            const uint8_t* const data = reinterpret_cast<const uint8_t*>(input.data());
            const size_t split = input.size() / 3;
            const uint32_t head = RayoMd::Flate::Adler32(data, split, first);
            const uint32_t tail = RayoMd::Flate::Adler32(data + split, input.size() - split);
            if (RayoMd::Flate::Adler32Combine(head, tail, input.size() - split) !=
                RayoMd::Flate::Adler32(data + split, input.size() - split, head)) {
                std::cerr << "Adler32Combine differs for " << input.size() << " bytes" << std::endl;
                return false;
            }
        }
    }
    std::string document = "# Packed\n\nA [link](#packed) and `code`.\n\n";
    for (int line = 0; line < 120; line++) document += "Line " + std::to_string(line) + " of words that repeat on every page.\n\n";
    const std::string unicode = "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0 \xCE\x94\n\n" + document;
    for (const std::string& text : { document, unicode }) {
        const char* const renderer = text == document ? "standard" : "Unicode";
        const std::string& otherText = text == document ? unicode : document;
        TinyPdf::PdfOptions options;
        options.pageNumbers = true;
        std::string plain;
        std::string packed;
        std::string other;
        std::string again;
        bool ok = TinyPdf::BuildPdf(text, options, plain).Ok();
        options.compress = true;
        ok = ok && TinyPdf::BuildPdf(text, options, packed).Ok() && ValidPdf(packed) &&
            TinyPdf::BuildPdf(otherText, options, other).Ok() && TinyPdf::BuildPdf(text, options, again).Ok() &&
            again == packed && packed.size() * 2 < plain.size();
        const auto plainStreams = PdfStreams(plain);
        const auto packedStreams = PdfStreams(packed);
        size_t flate = 0;
        ok = ok && plainStreams.size() == packedStreams.size();
        for (size_t i = 0; ok && i < plainStreams.size(); i++) {
            const auto& [dictionary, payload] = packedStreams[i];
            if (dictionary == plainStreams[i].first) {
                // Page numbers, and nothing of this text large enough to shrink.
                ok = payload == plainStreams[i].second && payload.size() < 256;
                continue;
            }
            flate++;
            const size_t font = plainStreams[i].first.find("/Length1 ");
            ok = dictionary.find("/Filter /FlateDecode ") != std::string::npos && payload.size() < plainStreams[i].second.size() &&
                (font == std::string::npos || dictionary.find(plainStreams[i].first.substr(font)) != std::string::npos);
            ok = ok && Inflates(payload, plainStreams[i].second);
        }
        // At least every page but the last, and for a font its program and both CMaps.
        const size_t pages = CountOccurrences(plain, "/Type /Page ");
        if (!ok || flate + 1 < pages + (text == document ? 0 : 3)) {
            std::cerr << "compression mismatch (" << renderer << " renderer)" << std::endl;
            return false;
        }
        std::vector<std::string> threaded(4);
        std::vector<std::thread> threads;
        for (std::string& output : threaded) {
            threads.emplace_back([&]() {
                std::string between;
                TinyPdf::BuildPdf(text, options, output);
                TinyPdf::BuildPdf(otherText, options, between);
                TinyPdf::BuildPdf(text, options, output);
            });
        }
        for (std::thread& thread : threads) thread.join();
        for (const std::string& output : threaded) {
            if (output != packed) {
                std::cerr << "compression differs between threads (" << renderer << " renderer)" << std::endl;
                return false;
            }
        }
    }
    std::string reversible;
    TinyPdf::PdfOptions options;
    options.embedSource = true;
    options.compress = true;
    if (!TinyPdf::BuildPdf(unicode, options, reversible).Ok() ||
        reversible.find("/Filter /FlateDecode") == std::string::npos ||
        RayoMd::PdfSource::Inspect(reversible, true).source != unicode) {
        std::cerr << "reversible profile with compression mismatch" << std::endl;
        return false;
    }
    std::string word;   // a page stream too short to shrink
    options.embedSource = false;
    if (!TinyPdf::BuildPdf("Hi.\n", options, word).Ok() || word.find("/FlateDecode") != std::string::npos ||
        word.find("(Hi.) Tj") == std::string::npos) {
        std::cerr << "a stream compression cannot shrink was compressed" << std::endl;
        return false;
    }
    return true;
}

// A theme file: comments and blank lines are skipped, a value may be quoted, "#RGB" doubles its
// digits, relative paths are the theme folder's, and an unknown key or a bad value is an error
// naming its line. Its colours reach headings, links, rules and the bar of plain quotes (not
// alerts) in both renderers; its font shows even ASCII text, and one that cannot be read fails
// the export. Its header and footer, logo and cover are checked below.
bool CheckTheme() {
    TinyPdf::PdfTheme theme;
    std::string error;
    const bool parsed = TinyPdf::Internal::ParseTheme(
        "\xEF\xBB\xBF# ACME\n\nfont = fonts/acme.ttf\r\nheading-color = #0B3D91\nlink-color = \"#c39\"\n; done\n"
        "accent-color=#F2A900\nlogo = img/logo.png\nheader-left = {logo}\nfooter-right = \" {page} \"\ncover = Yes\n",
        "themes", theme, error);
    TinyPdf::PdfTheme bad;
    std::string unknown;
    std::string color;
    std::string flag;
    if (!parsed || theme.fontPath != "themes/fonts/acme.ttf" || theme.headingColor != 0x0B3D91 ||
        theme.linkColor != 0xCC3399 || theme.accentColor != 0xF2A900 || theme.logoPath != "themes/img/logo.png" ||
        theme.headerLeft != "{logo}" || theme.footerRight != " {page} " || !theme.cover ||
        TinyPdf::Internal::ParseTheme("font = /abs.ttf\ncolour = red\n", "x", bad, unknown) || bad.fontPath != "/abs.ttf" ||
        unknown.find("line 2") == std::string::npos ||
        TinyPdf::Internal::ParseTheme("heading-color = #12345\n", "", bad, color) || color.find("line 1") == std::string::npos ||
        TinyPdf::Internal::ParseTheme("cover = maybe\n", "", bad, flag) || flag.find("line 1") == std::string::npos) {
        std::cerr << "theme parsing mismatch: " << error << unknown << color << flag << std::endl;
        return false;
    }
    // A theme's bold, italic and bold italic faces: paths of the theme's folder, and only with a font.
    TinyPdf::PdfTheme faced;
    TinyPdf::PdfTheme fontless;
    std::string facesError;
    std::string orphan;
    if (!TinyPdf::Internal::ParseTheme("font = a.ttf\nfont-bold = b.ttf\nfont-italic = /i.ttf\nfont-bold-italic = \"bi.ttf\"\n",
            "t", faced, facesError) || faced.fontBoldPath != "t/b.ttf" || faced.fontItalicPath != "/i.ttf" ||
        faced.fontBoldItalicPath != "t/bi.ttf" || TinyPdf::Internal::ParseTheme("font-italic = i.ttf\n", "t", fontless, orphan) ||
        orphan.find("need font") == std::string::npos) {
        std::cerr << "theme faces parsing mismatch: " << facesError << orphan << std::endl;
        return false;
    }
    theme = TinyPdf::PdfTheme();
    theme.headingColor = 0x0B3D91;
    theme.linkColor = 0xCC3399;
    theme.accentColor = 0xF2A900;
    const std::string document = "# Head\n\nA [link](https://example.com).\n\n---\n\n> Quoted.\n\n> [!NOTE]\n> Alert.\n";
    for (const std::string& text : { document, document + "\nZa\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n" }) {
        TinyPdf::PdfOptions options;
        options.theme = theme;
        std::string pdf;
        if (!TinyPdf::BuildPdf(text, options, pdf).Ok() || pdf.find("0.04 0.24 0.57 rg") == std::string::npos ||
            pdf.find("0.8 0.2 0.6 rg") == std::string::npos || pdf.find("q 0.95 0.66 0 RG 0.8 w") == std::string::npos ||
            pdf.find("0.95 0.66 0 rg") == std::string::npos || pdf.find("0.04 0.41 0.85 rg") == std::string::npos ||
            pdf.find("0.02 0.02 0.02 rg") != std::string::npos || pdf.find("0.05 0.30 0.68") != std::string::npos) {
            std::cerr << "theme colours mismatch (" << (text == document ? "standard" : "Unicode") << " renderer)" << std::endl;
            return false;
        }
    }
    TinyPdf::PdfOptions options;
    options.theme.fontPath = "no such font.ttf";
    std::string pdf;
    if (TinyPdf::BuildPdf(document, options, pdf).error != TinyPdf::BuildError::ThemeFontUnavailable) {
        std::cerr << "an unreadable theme font did not fail the export" << std::endl;
        return false;
    }
    std::string fontPath;
#ifdef _WIN32
    if (const char* windows = std::getenv("WINDIR")) fontPath = std::string(windows) + "\\Fonts\\arial.ttf";
#else
    fontPath = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
    if (std::ifstream(fontPath, std::ios::binary)) {
        options.theme.fontPath = fontPath;
        if (!TinyPdf::BuildPdf(document, options, pdf).Ok() || pdf.find("/FontFile2") == std::string::npos ||
            pdf.find("/Helvetica") != std::string::npos) {
            std::cerr << "the theme font did not show the ASCII document" << std::endl;
            return false;
        }
    }
    // A theme's bold face draws bold text and headings as /F2; bold italic without a face of its
    // own is the bold face slanted; a face that drew nothing stays out of the pages' resources.
    const std::string boldPath = fontPath.empty() ? std::string()
        : fontPath.substr(0, fontPath.size() - 4) + (fontPath.find("arial") != std::string::npos ? "bd.ttf" : "-Bold.ttf");
    if (std::ifstream(fontPath, std::ios::binary) && std::ifstream(boldPath, std::ios::binary)) {
        TinyPdf::PdfOptions boldTheme;
        boldTheme.theme.fontPath = fontPath;
        boldTheme.theme.fontBoldPath = boldPath;
        std::string plain;
        if (!TinyPdf::BuildPdf("# Head\n\nPlain, **bold** and ***both***.\n", boldTheme, pdf).Ok() ||
            CountOccurrences(pdf, "BT /F2 ") < 2 || pdf.find("BT /F2 11.5 Tf 1 0 0.18 1 ") == std::string::npos ||
            pdf.find(" /F2 ") == std::string::npos || CountOccurrences(pdf, "/Subtype /Type0 ") != 2 ||
            !TinyPdf::BuildPdf("Plain.\n", boldTheme, plain).Ok() || plain.find("/F2 ") != std::string::npos ||
            CountOccurrences(plain, "/Subtype /Type0 ") != 2) {
            std::cerr << "the theme's bold face did not draw bold text" << std::endl;
            return false;
        }
    }
    const auto npos = std::string::npos;
    // Header and footer: placeholders from the front matter and the page, an unknown one kept, a
    // field cut short with an ellipsis, the page numbers in the footer's centre, and the logo.
    const std::string fronted = "---\ntitle: Report\nauthor: Ann\ndate: 2026-10-09\n---\n\n# Head\n\nOne.\n\n\\pagebreak\n\nTwo.\n";
    TinyPdf::PdfOptions banded;
    banded.pageNumbers = true;
    banded.theme.headerLeft = "{logo}";
    banded.theme.headerRight = "{title} by {author} {unknown}";
    banded.theme.footerLeft = std::string(300, 'x');
    banded.theme.footerRight = "{date}";
    banded.theme.logoPath = std::string(RAYOMD_TEST_SOURCE_DIR) + "/docs/assets/branding/rayomd.png";
    TinyPdf::BuildResult result = TinyPdf::BuildPdf(fronted, banded, pdf);
    if (!result.Ok() || result.failedImages != 0 || result.pages != 2 || pdf.find("(Report by Ann {unknown}) Tj") == npos ||
        pdf.find("(2026-10-09) Tj") == npos || pdf.find("(1 / 2) Tj") == npos || pdf.find("(2 / 2) Tj") == npos ||
        pdf.find("x\x85) Tj") == npos || pdf.find(std::string(260, 'x')) != npos || pdf.find(" cm /Im1 Do") == npos) {
        std::cerr << "theme header and footer mismatch" << std::endl;
        return false;
    }
    banded.theme.logoPath = "no such logo.png";
    result = TinyPdf::BuildPdf(fronted, banded, pdf);
    if (!result.Ok() || result.failedImages != 1 || pdf.find(" Do") != npos) {
        std::cerr << "a theme logo that cannot be read was not a failed image" << std::endl;
        return false;
    }
    // Header text the standard fonts cannot show takes the document to a Unicode font, whose
    // subset gets its glyphs; the page numbers are then in that font, too.
    TinyPdf::PdfOptions greek;
    greek.pageNumbers = true;
    greek.theme.headerCenter = "\xCE\xA9 Corp";
    if (!TinyPdf::BuildPdf("# Plain\n", greek, pdf).Ok() || pdf.find("/Type0") == npos ||
        pdf.find("<03A9> <03A9>") == npos || pdf.find("/Helvetica") != npos) {
        std::cerr << "theme text outside WinAnsi did not take a Unicode font" << std::endl;
        return false;
    }
    // A cover is the first page, labelled so; the footer and the outline count only the pages
    // after it. In both renderers.
    TinyPdf::PdfOptions covered;
    covered.theme.cover = true;
    covered.theme.footerCenter = "{page} / {pages}";
    for (const std::string& text : { fronted, fronted + "\nZa\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n" }) {
        result = TinyPdf::BuildPdf(text, covered, pdf);
        const size_t kids = pdf.find("/Kids [");
        const std::string coverId = kids == npos ? "" : pdf.substr(kids + 7, pdf.find(' ', kids + 7) - kids - 7);
        if (!result.Ok() || result.pages != 3 || coverId.empty() ||
            pdf.find(" /PageLabels << /Nums [0 << /P (Cover) >> 1 << /S /D >>] >>") == npos ||
            pdf.find("/Dest [" + coverId + " 0 R") != npos || pdf.find("/Count 3") == npos ||
            (text == fronted && (pdf.find("(Report) Tj") == npos || pdf.find("(Ann) Tj") == npos ||
                                    pdf.find("(2 / 2) Tj") == npos || pdf.find("(3 / 3) Tj") != npos))) {
            std::cerr << "theme cover mismatch (" << (text == fronted ? "standard" : "Unicode") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

// The UTF-8 of the Info dictionary's text string `key`, "(ASCII)" or "<FEFF...>" (UTF-16BE). The
// outline's entries have titles too; the Info dictionary is the file's last object.
std::string InfoText(const std::string& pdf, const std::string& key) {
    const size_t info = pdf.rfind("<< /Producer (");
    const size_t at = info == std::string::npos ? info : pdf.find(key + " ", info);
    if (at == std::string::npos) return {};
    const size_t start = at + key.size() + 1;
    if (pdf[start] == '(') return pdf.substr(start + 1, pdf.find(')', start) - start - 1);
    std::u16string units;
    for (size_t digit = start + 5; pdf[digit] != '>'; digit += 4) units += (char16_t)std::stoul(pdf.substr(digit, 4), nullptr, 16);
    std::string utf8;
    for (size_t i = 0; i < units.size(); i++) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < units.size()) cp = 0x10000 + ((cp - 0xD800) << 10) + (units[++i] - 0xDC00);
        if (cp < 0x80) utf8 += (char)cp;
        else if (cp < 0x800) utf8 += {(char)(0xC0 | cp >> 6), (char)(0x80 | (cp & 0x3F))};
        else if (cp < 0x10000) utf8 += {(char)(0xE0 | cp >> 12), (char)(0x80 | (cp >> 6 & 0x3F)), (char)(0x80 | (cp & 0x3F))};
        else utf8 += {(char)(0xF0 | cp >> 18), (char)(0x80 | (cp >> 12 & 0x3F)), (char)(0x80 | (cp >> 6 & 0x3F)), (char)(0x80 | (cp & 0x3F))};
    }
    return utf8;
}

// The text of an XMP element or attribute that ends at `end`, after `start`, its references resolved.
std::string XmpText(const std::string& pdf, const std::string& start, char end) {
    const size_t at = pdf.find(start);
    if (at == std::string::npos) return {};
    const std::string raw = pdf.substr(at + start.size(), pdf.find(end, at + start.size()) - at - start.size());
    std::string text;
    for (size_t i = 0; i < raw.size(); i++) {
        bool replaced = false;
        for (const auto& [entity, ch] : { std::pair<std::string_view, char>{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                 {"&quot;", '"'}, {"&apos;", '\''} }) {
            if (raw.compare(i, entity.size(), entity) == 0) {
                text += ch;
                i += entity.size() - 1;
                replaced = true;
                break;
            }
        }
        if (!replaced) text += raw[i];
    }
    return text;
}

// PDF/A-3b: every font embedded, the page numbers' too, as formulas show their source; links
// printable; an sRGB output intent; an XMP packet that tells what the Info dictionary does, also
// of the title a heading gives; and a trailer /ID, the same for the same document. With the
// source embedded the file stays PDF 1.7 and recovers it, even with metadata that XMP would hold
// past what recovery reads; a PDF 1.7 file whose packet claims no PDF/A-3 does not.
bool CheckPdfA() {
    using TinyPdf::Internal::PdfaText;
    const auto npos = std::string::npos;
    const std::string ampersands(600, '&');
    if (PdfaText("a\x01\tb\xFF\xC3(c\x7F\xEF\xBF\xBE" "d\xF0\x9F\x98\x80") != "ab(cd\xF0\x9F\x98\x80" ||
        PdfaText(ampersands).size() != 409 || PdfaText(std::string(2047, 'x') + "\xC5\xBC").size() != 2047) {
        std::cerr << "PDF/A text mismatch" << std::endl;
        return false;
    }
    std::string document = "---\ntitle: \"Q3 <A&B> \\\"x\\\" \xF0\x9F\x98\x80\"\nauthor: [Ann, Bob]\nkeywords: [a, b]\n---\n\n"
        "# Report $x^2$\n\nSee [site](https://example.com), [below](#end) and $a+b$:\n\n$$\\sum_i i$$\n\n";
    for (int line = 0; line < 80; line++) document += "Line " + std::to_string(line) + " of text.\n\n";
    document += "## End\n";
    TinyPdf::PdfOptions options;
    options.pdfa = true;
    options.pageNumbers = true;
    std::string pdf;
    std::string again;
    std::string other;
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf(document, options, pdf);
    TinyPdf::BuildPdf(document, options, again);
    TinyPdf::BuildPdf("# Other\n", options, other);
    const auto fileId = [](const std::string& file) {
        const size_t at = file.rfind("/ID [<");
        if (at == std::string::npos || file.compare(at + 38, 3, "> <") != 0 || file.compare(at + 73, 2, ">]") != 0) return std::string();
        const std::string first = file.substr(at + 6, 32);
        return first == file.substr(at + 41, 32) && first.find_first_not_of("0123456789ABCDEF") == std::string::npos ? first
            : std::string();
    };
    const size_t links = CountOccurrences(pdf, "/Subtype /Link");
    // Formulas are typeset in the embedded math fonts: every Type1 font has its program (Type1C).
    if (!result.Ok() || result.pages < 2 || pdf.rfind("%PDF-1.7\n", 0) != 0 || pdf.find(" /M5 ") == npos ||
        CountOccurrences(pdf, "/Subtype /Type1 ") != CountOccurrences(pdf, "/Subtype /Type1C") ||
        pdf.find("/Subtype /Type1C") == npos || pdf.find("\\sum") != npos ||
        pdf.find("/FontFile2 ") == npos || links != 2 || CountOccurrences(pdf, "/Subtype /Link /F 4 ") != links ||
        CountOccurrences(pdf, "q 0.45 0.45 0.45 rg\nBT /F1 9 Tf") != result.pages ||
        pdf.find("/OutputIntents [<< /Type /OutputIntent /S /GTS_PDFA1 ") == npos ||
        pdf.find("<< /N 3 /Length 480 >>\nstream\n") == npos || pdf.find("mntrRGB XYZ ") == npos ||
        pdf.find("pdfaid:part=\"3\" pdfaid:conformance=\"B\"") == npos ||
        InfoText(pdf, "/Title") != "Q3 <A&B> \"x\" \xF0\x9F\x98\x80" ||
        XmpText(pdf, "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">", '<') != InfoText(pdf, "/Title") ||
        XmpText(pdf, "<dc:creator><rdf:Seq><rdf:li>", '<') != "Ann; Bob" || InfoText(pdf, "/Author") != "Ann; Bob" ||
        XmpText(pdf, "pdf:Keywords=\"", '"') != "a, b" || InfoText(pdf, "/Keywords") != "a, b" ||
        XmpText(pdf, "pdf:Producer=\"", '"') != InfoText(pdf, "/Producer") || fileId(pdf).empty() || again != pdf ||
        fileId(other).empty() || fileId(other) == fileId(pdf) ||
        InfoText(other, "/Title") != "Other" || XmpText(other, "xml:lang=\"x-default\">", '<') != "Other") {
        std::cerr << "PDF/A structure mismatch" << std::endl;
        return false;
    }
    std::string reversible;
    options.embedSource = true;
    const std::string fronted = "---\ntitle: " + std::string(3000, 'x') + "\nkeywords: k" + std::string(3000, '<') +
        "\nsubject: s" + std::string(3000, '"') + "\nauthor: a" + std::string(3000, '&') + "\n---\n\n" + document;
    if (!TinyPdf::BuildPdf(fronted, options, reversible).Ok() || reversible.rfind("%PDF-1.7\n", 0) != 0 ||
        RayoMd::PdfSource::Inspect(reversible, true).source != fronted) {
        std::cerr << "PDF/A reversible profile mismatch" << std::endl;
        return false;
    }
    const size_t part = reversible.find("pdfaid:part=\"3\"");
    reversible[part + 13] = '2';
    if (RayoMd::PdfSource::Inspect(reversible, false).status != RayoMd::PdfSource::Status::CorruptPdf) {
        std::cerr << "a PDF 1.7 source profile without PDF/A-3 was accepted" << std::endl;
        return false;
    }
    return true;
}

// Footnotes as GitHub reads them: numbered by first reference in reading order, the notes'
// own references after all those of the text; a repeated reference keeps its number, a label
// is matched without case and its first definition counts; undefined references, links, code
// and escapes stay text, and an unused definition (with what it refers to) is left out. A
// definition interrupts a paragraph, goes on in lazy lines and in lines indented four columns
// (eight make code), and may stand in a quote or a list item. Headings and table cells get
// references as markers. Both renderers draw each reference as a link to its note and each
// note's number as a link back.
bool CheckFootnotes() {
    using TinyPdf::Internal::Block;
    using TinyPdf::Internal::BlockType;
    using TinyPdf::Internal::Footnotes;
    using TinyPdf::Internal::ParseMarkdown;
    const std::string order = "Text[^a] and[^b] again[^a] and [^missing].\n\n[^b]: Bee.\n[^a]: Ay with [^c].\n"
        "[^c]: Cee.\n[^unused]: Unused[^x].\n[^x]: Ex.\n";
    Footnotes notes;
    std::vector<Block> blocks = ParseMarkdown(order, &notes);
    const std::unordered_map<std::string, int> orderNumbers = {{"a", 1}, {"b", 2}, {"c", 3}};
    if (blocks.size() != 1 || blocks[0].text != "Text[^a] and[^b] again[^a] and [^missing]." || notes.notes.size() != 3 ||
        notes.numbers != orderNumbers || notes.notes[0].text != "[1.](#^r1) Ay with [^c]." ||
        notes.notes[1].text != "[2.](#^r2) Bee." || notes.notes[2].text != "[3.](#^r3) Cee.") {
        std::cerr << "footnote numbering mismatch" << std::endl;
        return false;
    }
    const std::string syntax = "Interrupt[^1]\n[^1]: def\nlazy line\n\nafter[^Code]\n\n[^code]: Para one.\n\n"
        "        indented code\n\n    Para two.\n\nNot in note.\n\n> quoted[^q]\n> [^q]: in quote\n\n- item[^l]\n\n"
        "  [^l]: in list\n\n[^z](https://e.com) `[^z]` \\[^z] [^two words]\n\n[^z]: zed\n[^Code]: duplicate\n";
    blocks = ParseMarkdown(syntax, &notes);
    const std::unordered_map<std::string, int> syntaxNumbers = {{"1", 1}, {"code", 2}, {"q", 3}, {"l", 4}};
    if (blocks.size() != 6 || blocks[0].text != "Interrupt[^1]" || blocks[1].text != "after[^Code]" ||
        blocks[2].text != "Not in note." || blocks[3].type != BlockType::Quote || blocks[3].children.size() != 1 ||
        blocks[4].type != BlockType::Bullet || blocks[4].text != "item[^l]" || notes.numbers != syntaxNumbers ||
        notes.notes.size() != 4 || notes.notes[0].text != "[1.](#^r1) def lazy line" ||
        notes.notes[1].text != "[2.](#^r2) Para one." || notes.notes[1].children.size() != 2 ||
        notes.notes[1].children[0].type != BlockType::Code || notes.notes[1].children[0].text != "indented code" ||
        notes.notes[1].children[1].text != "Para two." || notes.notes[2].text != "[3.](#^r3) in quote" ||
        notes.notes[3].text != "[4.](#^r4) in list") {
        std::cerr << "footnote definition syntax mismatch" << std::endl;
        return false;
    }
    blocks = ParseMarkdown("Case[^Note] and[^NOTE].\n\n[^note]: first\n[^NOTE]: second\n", &notes);
    if (notes.notes.size() != 1 || notes.notes[0].text != "[1.](#^r1) first" || notes.numbers.size() != 1) {
        std::cerr << "footnote label matching mismatch" << std::endl;
        return false;
    }
    blocks = ParseMarkdown("# Head[^h] `[^h]`\n\n| a | b[^t] |\n|---|---|\n| $x$ | **2** |\n\n[^h]: H.\n[^t]: T.\n", &notes);
    if (blocks.size() != 2 || !blocks[0].hasMath || blocks[0].text != "Head\x01\x03" "1\x02 [^h]" || !blocks[1].hasMath ||
        blocks[1].rows[0][1] != "b\x01\x03" "2\x02" || blocks[1].rows[1][0] != "\x01x\x02" || blocks[1].rows[1][1] != "2" ||
        notes.notes.size() != 2) {
        std::cerr << "footnote heading and table mismatch" << std::endl;
        return false;
    }
    for (const std::string& text : { order, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + order }) {
        std::string pdf;
        // References a, b, a and c in note 1, and three notes' numbers back.
        if (!Build(text, pdf) || CountOccurrences(pdf, "/Dest [") != 7 || pdf.find(" 8.05 Tf") == std::string::npos) {
            std::cerr << "footnote rendering mismatch" << std::endl;
            return false;
        }
    }
    return true;
}

// The tint tile of a line of code after the first one on its page starts below the descenders of
// the line above, where Courier's underscore reaches 1.2 points under the baseline: drawn over
// them, it hid underscores. The tiles of a block still overlap, so that it is one tint. In both
// renderers.
bool CheckCodeTiles() {
    const std::string code = "```\nsnake_case_1\nsnake_case_2\nsnake_case_3\n```\n";
    const std::string tint = "0.95 0.95 0.93 rg ";
    for (const std::string& text : { code, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + code }) {
        std::string pdf;
        const size_t first = Build(text, pdf) ? pdf.find(tint) : std::string::npos;
        const size_t streamEnd = pdf.find("endstream", first == std::string::npos ? 0 : first);
        if (first == std::string::npos || streamEnd == std::string::npos) return false;
        // The tiles and the lines of text in the order the page draws them.
        int tiles = 0;
        double baseline = 0.0;
        double previousBottom = 0.0;
        bool covers = false;
        bool gap = false;
        for (size_t at = pdf.rfind("stream", first);;) {
            const size_t tile = pdf.find(tint, at);
            const size_t line = pdf.find(" Tm ", at);
            if (std::min(tile, line) >= streamEnd) break;
            if (tile < line) {
                double x = 0.0, bottom = 0.0, width = 0.0, height = 0.0;
                if (std::sscanf(pdf.c_str() + tile + tint.size(), "%lf %lf %lf %lf re", &x, &bottom, &width, &height) != 4) {
                    return false;
                }
                covers = covers || (tiles > 0 && bottom + height > baseline - 2.0);
                gap = gap || (tiles > 0 && bottom + height < previousBottom);
                previousBottom = bottom;
                tiles++;
                at = tile + tint.size();
            } else {
                baseline = std::atof(pdf.c_str() + pdf.rfind(' ', line - 1) + 1);
                at = line + 4;
            }
        }
        // Three lines and the empty one after them.
        if (tiles != 4 || covers || gap) {
            std::cerr << "code tiles mismatch: " << tiles << " tiles" << (covers ? ", one covers the line above" : "")
                << (gap ? ", a gap between two" : "") << std::endl;
            return false;
        }
    }
    return true;
}

// The text of the run strings of the first line drawn in `color`, up to the end of its text
// object: its literal strings unescaped, two bytes a character when `cids` (the Unicode renderer's
// CIDs, which are code points; ASCII ones here), else one (the standard renderer).
std::string CodeLineText(const std::string& pdf, std::string_view color, bool cids) {
    std::string bytes;
    const size_t start = pdf.find(std::string(color) + " rg BT");
    const size_t end = start == std::string::npos ? start : pdf.find(" ET", start);
    for (size_t at = start; end != std::string::npos && at < end; at++) {
        if (pdf[at] != '(') continue;
        for (at++; pdf[at] != ')'; at++) bytes.push_back(pdf[at] == '\\' ? pdf[++at] : pdf[at]);
    }
    if (!cids) return bytes;
    std::string text;
    for (size_t at = 1; at < bytes.size(); at += 2) text.push_back(bytes[at]);
    return text;
}

// Fenced code in a language GitHub knows takes the classes of its tokens. The first word of the info
// string names the language, in any case, as GitHub's aliases and Pandoc's {.lang} do; strings,
// comments and interpolations start and end where each language has them. The PDF draws each run of
// one colour after its colour, and the line's text as it is; PdfOptions::highlightCode off draws the
// block in one colour. In both renderers.
bool CheckHighlighting() {
    using TinyPdf::Internal::ClassifyCode;
    using TinyPdf::Internal::CodeLanguage;
    const std::pair<const char*, const char*> sameLanguage[] = {
        {"Python", "py"}, {"{.python .numberLines}", "python"}, {"rust,ignore", "rs"}, {"js title=\"a.js\"", "javascript"},
        {"C#", "csharp"}, {"c++", "cpp"}, {"YML", "yaml"}, {"shell-session", "console"}, {"Dockerfile", "docker"},
    };
    for (const auto& [info, same] : sameLanguage) {
        if (CodeLanguage(info) == 0 || CodeLanguage(info) != CodeLanguage(same)) {
            std::cerr << "highlight language mismatch: " << info << std::endl;
            return false;
        }
    }
    for (const char* none : { "", "  ", "math", "text", "plaintext", "unknownlang", "mermaid" }) {
        if (CodeLanguage(none) != 0) {
            std::cerr << "highlight language for '" << none << "'" << std::endl;
            return false;
        }
    }
    // The class of each byte, as a letter: plain ' ', comment 'c', string 's', number 'n', keyword
    // 'K', constant 'C', entity 'E', tag 'T', variable 'V', diff '+', '-' and '@'; '?' any.
    struct Case { const char* info; const char* code; const char* classes; };
    const Case cases[] = {
        {"python", "x = \"# no\" # yes", "    ssssss ccccc"},
        {"js", "`a${f(1)}b`", "ssssE n sss"},
        {"js", "x = a / b; y = /c\\/d/g;", "               sssssss "},
        {"rust", "/* a /* b */ c */ x", "ccccccccccccccccc  "},
        {"rust", "'a 'b'", "KK sss"},
        {"sh", "cat <<EOF\n$x # no\nEOF\necho # yes", "    sssss?sssssss?sss?CCCC ccccc"},
        {"c", "#include <a.h> // c", "KKKKKKKK sssss cccc"},
        {"yaml", "key: \"v\" # c", "TTT  sss ccc"},
        {"json", "{\"k\": \"v\", \"n\": 1}", " CCC  sss  CCC  n "},
        {"html", "<a href=\"x\">t</a>", " T EEEE sss    T "},
        {"css", "a { color: #fff; }", "T   CCCCC  CCCC   "},
        {"sql", "SELECT 'it''s' -- c", "KKKKKK sssssss cccc"},
        {"diff", "-a\n+b\n@@ x", "--?++?@@@@"},
    };
    static const char letters[] = " csnKCETV+-@";
    for (const Case& item : cases) {
        const std::string code = item.code;
        std::vector<uint8_t> classes(code.size());
        ClassifyCode(CodeLanguage(item.info), code, classes.data());
        std::string actual;
        for (size_t at = 0; at < code.size(); at++) actual.push_back(item.classes[at] == '?' ? '?' : letters[classes[at]]);
        if (actual != item.classes) {
            std::cerr << "highlight classes mismatch (" << item.info << "): '" << actual << "' for '" << code << "'" << std::endl;
            return false;
        }
    }
    const std::string block = "```python\ndef f(x): return 'a'  # note\n```\n";
    for (const std::string& text : { block, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + block }) {
        TinyPdf::PdfOptions options;
        std::string colored;
        std::string plain;
        const bool built = TinyPdf::BuildPdf(text, options, colored).Ok();
        options.highlightCode = false;
        if (!built || !TinyPdf::BuildPdf(text, options, plain).Ok() || CountOccurrences(colored, ".812 .133 .18 rg") != 2 ||
            colored.find(".349 .388 .431 rg") == std::string::npos ||
            CodeLineText(colored, ".812 .133 .18", text != block) != "def f(x): return 'a'  # note" ||
            plain.find(".812 .133 .18 rg") != std::string::npos ||
            CodeLineText(plain, "0.12 0.12 0.12", text != block) != "def f(x): return 'a'  # note") {
            std::cerr << "highlighted code mismatch (" << (text == block ? "standard" : "Unicode") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

// A table of contents: the first paragraph "[TOC]" or "[[_TOC_]]", in any case, becomes it and
// later ones go, unless a link definition makes "[TOC]" a link; PdfOptions::toc puts one first, or
// after a title heading. It lists the headings to its depth below its top level, a document's only
// level-1 heading, its first, being its title; each entry links to its heading and shows its page,
// after a row of dots below the top level. In both renderers.
bool CheckContents() {
    using TinyPdf::Internal::Block;
    using TinyPdf::Internal::BlockType;
    using TinyPdf::Internal::ParseMarkdown;
    using TinyPdf::Internal::PlaceContents;
    std::vector<Block> blocks = ParseMarkdown("# Title\n\nText.\n\n[toc]\n\n## A\n\n### B\n\n#### C\n\n[[_TOC_]]\n\n[TOC] text\n");
    const auto contentsBlocks = [&blocks]() {
        return std::count_if(blocks.begin(), blocks.end(), [](const Block& block) { return block.type == BlockType::Contents; });
    };
    const auto entries = PlaceContents(blocks, false, 2);
    if (blocks.size() != 8 || blocks[2].type != BlockType::Contents || blocks[6].type != BlockType::Contents ||
        contentsBlocks() != 2 || blocks.back().text != "[TOC] text" || entries.size() != 2 || entries[0].text != "A" ||
        entries[0].level != 1 || entries[1].text != "B" || entries[1].level != 2 || entries[0].heading != blocks[3].text.data()) {
        std::cerr << "table of contents marker mismatch" << std::endl;
        return false;
    }
    blocks = ParseMarkdown("[TOC]\n\n# Heading\n\n[toc]: https://example.com\n");
    const bool linked = !PlaceContents(blocks, false, 3).empty() || contentsBlocks() != 0;
    blocks = ParseMarkdown("# Title\n\nText.\n\n## A\n");
    const bool afterTitle = PlaceContents(blocks, true, 3).size() == 1 && blocks[1].type == BlockType::Contents;
    blocks = ParseMarkdown("# One\n\n# Two\n");
    const bool first = PlaceContents(blocks, true, 1).size() == 2 && blocks[0].type == BlockType::Contents;
    // A heading of footnote references alone shows nothing to list; a depth out of 1 to 6 is taken as the nearest.
    const auto listedEntries = [](int depth) {
        TinyPdf::Internal::Footnotes notes;
        std::vector<Block> parsed =
            ParseMarkdown("# One\n\n[TOC]\n\n## [^1]\n\n## Two\n\n### Three\n\n#### Four\n\n[^1]: Note.\n", &notes);
        return PlaceContents(parsed, false, depth);   // only the levels and texts are compared
    };
    const auto all = listedEntries(1000);
    const bool listed = all.size() == 3 && all[0].text == "Two" && all[2].text == "Four" && all[2].level == 3 &&
        listedEntries(-5).size() == 1;
    if (linked || !afterTitle || !first || !listed) {
        std::cerr << "table of contents placement mismatch" << std::endl;
        return false;
    }
    const std::string document = "# Title\n\n[TOC]\n\n## One\n\nText.\n\n### Two\n\nText.\n\n[[_toc_]]\n";
    for (const std::string& text : { document, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + document }) {
        std::string pdf;
        // Three bookmarks and two entries' links, one table; dots before the second entry's number only.
        if (!Build(text, pdf) || CountOccurrences(pdf, "/Dest [") != 5 || CountOccurrences(pdf, " Tc 1 0 0 1 ") != 1 ||
            (text == document && (CountOccurrences(pdf, "(Contents) Tj") != 1 || pdf.find("/F2 11.5 Tf 1 0 0 1 ") == std::string::npos ||
                pdf.find("(1) Tj ET Q") == std::string::npos))) {
            std::cerr << "table of contents mismatch (" << (text == document ? "standard" : "Unicode") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

// The object numbers of the "N 0 R" references in `list`.
std::vector<int> References(const std::string& list) {
    std::vector<int> numbers;
    std::istringstream in(list);
    int number = 0;
    int generation = 0;
    std::string r;
    while (in >> number >> generation >> r) numbers.push_back(number);
    return numbers;
}

// Each page's links in their order: the page a link goes to, from 0, or -1 for one to a URI.
std::vector<std::vector<int>> LinkTargetsByPage(const std::string& pdf) {
    const auto object = [&pdf](int number) {
        const std::string head = "\n" + std::to_string(number) + " 0 obj\n";
        const size_t start = pdf.find(head);
        return start == std::string::npos ? std::string() : pdf.substr(start, pdf.find("\nendobj", start) - start);
    };
    const auto between = [](const std::string& text, std::string_view open) {
        const size_t start = text.find(open);
        return start == std::string::npos ? std::string() : text.substr(start + open.size(), text.find(']', start) - start - open.size());
    };
    const std::vector<int> pages = References(between(pdf, "/Type /Pages /Kids ["));
    std::vector<std::vector<int>> targets;
    for (const int page : pages) {
        std::vector<int>& links = targets.emplace_back();
        for (const int annotation : References(between(object(page), "/Annots ["))) {
            const std::vector<int> destination = References(between(object(annotation), "/Dest ["));
            const auto found = destination.empty() ? pages.end() : std::find(pages.begin(), pages.end(), destination[0]);
            links.push_back(found == pages.end() ? -1 : (int)(found - pages.begin()));
        }
    }
    return targets;
}

// A book (BuildBookPdf): its title and each part on a page of their own, each file from a new
// page, a file without a level-1 heading of its own after its title in the book's list. Notes and
// anchors are a file's own; a link to another file of the book goes there, one to a file outside
// it stays a URI. SUMMARY.md nests a part's files a level down, and its prefix and suffix
// chapters not.
bool CheckBook() {
    const RayoMd::Book::Summary summary = RayoMd::Book::ParseSummary(
        "# Summary\n\n[Intro](README.md)\n\n# Part A\n\n- [One](one.md)\n  - [Two *x*](sub/two%20x.md#frag)\n"
        "- [Draft]()\n- [Web](https://example.com)\n\n---\n\n[End](end.md)\n");
    const auto entry = [&summary](size_t index, const char* title, const char* path, int depth, bool part) {
        return index < summary.entries.size() && summary.entries[index].title == title && summary.entries[index].path == path &&
            summary.entries[index].depth == depth && summary.entries[index].part == part;
    };
    if (!summary.title.empty() || summary.entries.size() != 5 || !entry(0, "Intro", "README.md", 0, false) ||
        !entry(1, "Part A", "", 0, true) || !entry(2, "One", "one.md", 1, false) || !entry(3, "Two x", "sub/two x.md", 2, false) ||
        !entry(4, "End", "end.md", 0, false)) {
        std::cerr << "SUMMARY.md mismatch" << std::endl;
        return false;
    }
    TinyPdf::Book book;
    book.title = "Guide";
    book.chapters.push_back({ "", "", "Part one", 0, true });
    book.chapters.push_back({ "## Setup\n\nNote[^1]. To [b](b.md#setup), [b top](b.md), [out](c.md), [web](https://example.com).\n\n"
        "[^1]: A note.\n", "book/a.md", "Alpha title", 1, false });
    book.chapters.push_back({ "# Beta\n\n## Setup\n\nNote[^1]. Back to [a](a.md#setup) and [here](#setup).\n\n[^1]: B note.\n",
        "book/b.md", "Beta title", 1, false });
    TinyPdf::PdfOptions options;
    options.style = TinyPdf::PdfStyle::Modern;
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildBookPdf(book, options, pdf);
    const std::vector<std::vector<int>> links = LinkTargetsByPage(pdf);
    const std::vector<std::vector<int>> expected = { {}, {}, { 2, 3, 3, -1, -1, 2 }, { 3, 2, 3, 3 } };
    if (!result.Ok() || result.pages != 4 || links != expected || pdf.find("(Alpha title) Tj") == std::string::npos ||
        pdf.find("(Beta title)") != std::string::npos || pdf.find("/Title (Part one) /Parent") == std::string::npos ||
        CountOccurrences(pdf, "/Count 4 /Dest") != 1) {
        std::cerr << "book mismatch" << std::endl;
        return false;
    }
    return true;
}

// --number-headings: 1, 1.1, 1.1.1 from the top level of the listed headings, the title (a
// document's only level-1 heading, first) and headings of footnote references alone without, a
// skipped level shown as 0, and none past the depth; in the text, the table of contents and the
// bookmarks of both renderers, while #anchor links still go by the heading's own text; across a
// book's files at their levels in the book.
bool CheckHeadingNumbers() {
    using TinyPdf::Internal::HeadingNumber;
    const auto numbersOf = [](const std::string& markdown, int depth) {
        const std::vector<TinyPdf::Internal::Block> blocks = TinyPdf::Internal::ParseMarkdown(markdown);
        std::string out;
        for (const HeadingNumber& number : TinyPdf::Internal::NumberHeadings(blocks, depth)) out += number.number + ";";
        return out;
    };
    if (numbersOf("# T\n\n## A\n\n### B\n\n### C\n\n## D\n\n#### E\n", 6) != "1;1.1;1.2;2;2.0.1;" ||
        numbersOf("# T\n\n## A\n\n### B\n\n#### C\n", 2) != "1;1.1;" || numbersOf("# A\n\n# B\n\n## C\n", 3) != "1;2;2.1;" ||
        numbersOf("## A\n\n### B\n", 3) != "1;1.1;" || numbersOf("Text\n", 3) != "") {
        std::cerr << "heading numbers mismatch" << std::endl;
        return false;
    }
    TinyPdf::PdfOptions options;
    options.numberHeadings = 3;
    options.toc = true;
    for (const char* prefix : { "", "\xC5\xBC" }) {   // the standard and the Unicode renderer
        const std::string document = std::string("# Title\n\n## Intro") + prefix + "\n\nSee [the method](#method).\n\n## Method\n\n"
            "### Data\n\n> ## Quoted\n";
        std::string pdf;
        const bool unicode = *prefix != '\0';
        const auto has = [&pdf, unicode](const char* text) {
            if (!unicode) return pdf.find(std::string("(") + text + ") Tj") != std::string::npos;
            return true;   // the Unicode renderer draws glyph ids; its numbers are checked in the bookmarks
        };
        if (!TinyPdf::BuildPdf(document, options, pdf).Ok() || !has("Title") || !has("2 Method") || !has("2.1 Data") ||
            !has("Quoted") || pdf.find("/Title (2 Method) /Parent") == std::string::npos ||
            pdf.find("/Title (2.1 Data) /Parent") == std::string::npos || pdf.find("/Title (Title) /Parent") == std::string::npos ||
            pdf.find("/Title (Quoted)") == std::string::npos || pdf.find("/URI (#method)") != std::string::npos ||
            CountOccurrences(pdf, "/Subtype /Link") != 1 + 3) {   // the link and the table's three entries
            std::cerr << "numbered headings mismatch" << (unicode ? " (Unicode)" : "") << std::endl;
            return false;
        }
    }
    TinyPdf::Book book;
    book.chapters.push_back({ "# One\n\n## A\n", "a.md", "One", 0, false });
    book.chapters.push_back({ "", "", "Part", 0, true });
    book.chapters.push_back({ "# Two\n\n## B\n", "b.md", "Two", 1, false });
    std::string bookPdf;
    if (!TinyPdf::BuildBookPdf(book, options, bookPdf).Ok() || bookPdf.find("/Title (1 One)") == std::string::npos ||
        bookPdf.find("/Title (1.1 A)") == std::string::npos || bookPdf.find("/Title (Part)") == std::string::npos ||
        bookPdf.find("/Title (1.2 Two)") == std::string::npos || bookPdf.find("/Title (1.2.1 B)") == std::string::npos) {
        std::cerr << "numbered book mismatch" << std::endl;
        return false;
    }
    return true;
}

// An SVG image is drawn as a form XObject over the unit square, as images are drawn: its
// gradient as a shading, its opacity as a graphics state, its text in the document's font (the
// standard fonts' literal strings, the minus sign as a hyphen, the Unicode renderer's glyph ids).
// Style sheets of type, class and id selectors apply by specificity, as Illustrator writes them.
// One that needs content RayoMD does not draw (HTML in a foreignObject, a descendant selector
// that sets a fill) shows its alt text, and counts as failed.
bool CheckSvg() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "rayomd-test-svg";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "chart.svg", std::ios::binary) <<
        "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\" viewBox=\"0 0 200 100\">"
        "<defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#1f77b4\"/><stop offset=\"1\" stop-color=\"red\"/>"
        "</linearGradient><clipPath id=\"c\"><rect width=\"150\" height=\"100\"/></clipPath></defs>"
        "<g clip-path=\"url(#c)\"><rect x=\"10\" y=\"10\" width=\"80\" height=\"60\" fill=\"url(#g)\"/>"
        "<path d=\"M100 80 A30 30 0 0 1 160 80\" fill=\"none\" stroke=\"#333\" stroke-width=\"2\"/></g>"
        "<circle cx=\"170\" cy=\"30\" r=\"15\" fill=\"green\" fill-opacity=\"0.5\"/>"
        "<text x=\"100\" y=\"95\" text-anchor=\"middle\" font-size=\"12\">Sales</text>"
        "<text x=\"5\" y=\"20\">\xE2\x88\x92" "1</text></svg>";
    std::ofstream(directory / "html.svg", std::ios::binary) <<
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><foreignObject width=\"10\" height=\"10\">"
        "<div xmlns=\"http://www.w3.org/1999/xhtml\">label</div></foreignObject></svg>";
    const std::string sheet = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\"><style>";
    const std::string boxes = "<rect class=\"st0\" width=\"10\" height=\"10\"/><rect class=\"st1\" x=\"12\" width=\"10\" "
        "height=\"10\"/><rect id=\"b\" class=\"st0\" x=\"24\" width=\"10\" height=\"10\"/></svg>";
    std::ofstream(directory / "classes.svg", std::ios::binary) <<
        sheet << ".st0{fill:#E30613}rect.st1{fill:#1D71B8}.st1{fill:black}#b{fill:#FFED00}</style>" << boxes;
    std::ofstream(directory / "descendant.svg", std::ios::binary) << sheet << "svg .st0{fill:#E30613}</style>" << boxes;
    TinyPdf::PdfOptions options;
    options.sourcePath = (directory / "doc.md").u8string();
    const std::string document = "# Chart\n\n![chart](chart.svg)\n";
    for (const std::string& text : { document, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + document }) {
        const bool standard = text == document;
        std::string pdf;
        const TinyPdf::BuildResult result = TinyPdf::BuildPdf(text, options, pdf);
        if (!result.Ok() || result.failedImages != 0 || CountOccurrences(pdf, "/Subtype /Form") != 1 ||
            pdf.find("/Subtype /Image") != std::string::npos || pdf.find("/Matrix [0.00666667 0 0 0.01333333 0 0]") == std::string::npos ||
            pdf.find("/Shading << /Sh1") == std::string::npos || pdf.find("/ExtGState << /GS1") == std::string::npos ||
            pdf.find(standard ? "(Sales) Tj" : std::string("(\0S\0a\0l\0e\0s) Tj", 15)) == std::string::npos ||
            pdf.find(standard ? "(-1) Tj" : std::string("(\x22\x12\0" "1) Tj", 9)) == std::string::npos) {
            std::cerr << "SVG image mismatch (" << (standard ? "standard" : "Unicode") << " renderer)" << std::endl;
            return false;
        }
    }
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf(
        "![classes](classes.svg)\n\n![labels](html.svg)\n\n![descendant](descendant.svg)\n", options, pdf);
    std::filesystem::remove_all(directory);
    if (!result.Ok() || result.failedImages != 2 || CountOccurrences(pdf, "/Subtype /Form") != 1 ||
        pdf.find("0.89 0.024 0.075 rg\n0 15 7.5 -7.5 re") == std::string::npos ||
        pdf.find("0.114 0.443 0.722 rg\n9 15") == std::string::npos || pdf.find("1 0.929 0 rg\n18 15") == std::string::npos) {
        std::cerr << "SVG style sheet or fallback mismatch" << std::endl;
        return false;
    }
    return true;
}

// The Unicode renderer writes text as literal strings of two-byte CIDs (the code points): a byte
// that is a parenthesis or a backslash takes a backslash, and a CR is written as \r, which a
// reader would otherwise take for LF. 'č' (U+010D) has CR for its low byte.
bool CheckCidStrings() {
    std::string pdf;
    if (!Build("Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 (a) b\\\\c \xC4\x8D\n", pdf) || pdf.find("/Type0") == std::string::npos) {
        std::cerr << "CID string document did not build with a TrueType font" << std::endl;
        return false;
    }
    // "Zażółć (a) b\c č": ż U+017C, ó U+00F3, ł U+0142, ć U+0107; nothing else in the content.
    static const char expected[] = " Tm (\0Z\0a\x01|\0\xF3\x01" "B\x01\x07\0 \0\\(\0a\0\\)\0 \0b\0\\\\\0c\0 \x01\\r) Tj ET";
    if (pdf.find(std::string(expected, sizeof(expected) - 1)) == std::string::npos) {
        std::cerr << "CID string escapes mismatch" << std::endl;
        return false;
    }
    return true;
}

// RayoMd::ExactMath gives libm's bits: every function against <cmath> on edge cases and on
// millions of doubles (random bit patterns, whole numbers and halves with their neighbours,
// every exponent, subnormals), NaN, infinities and -0.0 included.
bool CheckExactMath() {
    namespace X = RayoMd::ExactMath;
    auto bits = [](double value) {
        uint64_t b;
        std::memcpy(&b, &value, sizeof(b));
        return b;
    };
    auto fromBits = [](uint64_t b) {
        double value;
        std::memcpy(&value, &b, sizeof(value));
        return value;
    };
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&state] {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    // A double of a kind chosen by `kind`: any bit pattern, a whole number or a half or a
    // neighbour of one, any exponent with a random significand, or a subnormal.
    auto sample = [&](uint64_t kind) {
        const uint64_t r = next();
        const uint64_t sign = r & 0x8000000000000000ull;
        switch (kind % 5) {
        case 0: return fromBits(r);
        case 1: {
            const double whole = (double)(int64_t)(next() >> (11 + r % 53)) * ((r >> 8) & 1 ? 1.0 : 0.5);
            const double value = (r >> 9) & 1 ? -whole : whole;
            const int step = (int)((r >> 10) % 5) - 2;   // -2 .. 2 ulps away
            return step == 0 ? value : fromBits(bits(value) + (uint64_t)(int64_t)step);
        }
        case 2: return fromBits(sign | ((r >> 12) % 2047) << 52 | (next() & 0x000FFFFFFFFFFFFFull));
        case 3: return fromBits(sign | (next() & 0x000FFFFFFFFFFFFFull));
        default: return (double)(int64_t)(r % 2000001) / 1000.0 - 1000.0;
        }
    };
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double edges[] = {0.0, -0.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 0.49999999999999994, -0.49999999999999994,
        0.5000000000000001, 4503599627370495.5, -4503599627370495.5, 4503599627370496.0, 9007199254740991.0,
        9007199254740993.0, 9223372036854774784.0, -9223372036854775808.0, 9223372036854775808.0, 1e300, -1e300,
        4.9406564584124654e-324, -4.9406564584124654e-324, 2.2250738585072014e-308, 1.7976931348623157e308,
        -1.7976931348623157e308, inf, -inf, nan, -nan, 1.0, -1.0, 0.1, -0.1, 360.0, -720.5, 1e-9};
    size_t failures = 0;
    auto unary = [&](double x) {
        const bool ok = bits(X::Ceil(x)) == bits(std::ceil(x)) && bits(X::Floor(x)) == bits(std::floor(x)) &&
            bits(X::Trunc(x)) == bits(std::trunc(x)) && X::LRound(x) == std::lround(x) &&
            X::LlRound(x) == std::llround(x) && bits(X::Sqrt(x)) == bits(std::sqrt(x));
        if (!ok && failures++ < 5) std::cerr << "ExactMath differs from libm for " << std::hexfloat << x << std::defaultfloat << std::endl;
    };
    // A NaN divisor (RayoMD divides by constants) gives a NaN whose sign and payload differ
    // between glibc versions; every other case must match bit for bit.
    auto binary = [&](double x, double y) {
        const double ours = X::Fmod(x, y), theirs = std::fmod(x, y);
        const bool same = std::isnan(y) ? std::isnan(ours) && std::isnan(theirs) : bits(ours) == bits(theirs);
        if (!same && failures++ < 5) {
            std::cerr << "ExactMath::Fmod differs from libm for " << std::hexfloat << x << ", " << y << std::defaultfloat << std::endl;
        }
    };
    for (double x : edges) {
        unary(x);
        for (double y : edges) binary(x, y);
    }
    for (uint32_t i = 0; i < 10000000; i++) unary(sample(i));
    for (uint32_t i = 0; i < 2000000; i++) {
        const double x = sample(i), y = sample(i / 5);
        binary(x, y);
        binary(x, (double)(int64_t)(next() % 720) - 360.0);   // as SVG angles and hues are reduced
    }
    return failures == 0;
}

// Threads that each build documents and end, as batch workers and the GUI's build threads do: the
// per-thread buffers of the parser and renderers (links, bullets, tables, HTML tags, compression,
// print colours) must be destroyed at thread exit without touching freed thread storage. With
// MinGW's emulated TLS their thread_local destructors ran on freed memory (thread_object.h), and
// such a thread crashed as it ended; under Wine every `--batch --workers=4` run did.
bool CheckThreadExit() {
    const std::string documents[] = {
        "# Links\n\n[one](https://example.com/one), [two](#links) and <b>bold</b> <i>html</i>.\n\n- a\n  - b\n    - c\n\n"
        "| a | b |\n|---|---|\n| 1 | [x](https://example.com/x) |\n\n```cpp\nint main() { return 0; }\n```\n\nNote[^n].\n\n[^n]: A note.\n",
        u8"# Za\u017C\u00F3\u0142\u0107\n\nG\u0119\u015Bl\u0105 [ja\u017A\u0144](https://example.com) \u65E5\u672C.\n\n- x\n- y\n" };
    std::atomic<int> built{ 0 };
    for (int round = 0; round < 4; round++) {
        std::vector<std::thread> threads;
        for (int index = 0; index < 8; index++) {
            threads.emplace_back([&documents, &built, index] {
                TinyPdf::PdfOptions options;
                options.compress = index % 2 == 0;
                options.blackText = index % 3 == 0;
                std::string pdf;
                for (const std::string& markdown : documents) {
                    if (TinyPdf::BuildPdf(markdown, options, pdf).Ok() && pdf.size() > 100) built++;
                }
            });
        }
        for (std::thread& thread : threads) thread.join();
    }
    if (built.load() == 4 * 8 * 2) return true;
    std::cerr << "thread exit: " << built.load() << " of 64 builds" << std::endl;
    return false;
}

// WriteAll overwrites an existing file in place on POSIX systems: a shorter content must cut off
// the old tail, a longer one replace it all, and an empty one leave an empty file.
bool CheckWriteAll() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "rayomd-test-write-all.bin";
    const std::string longer(70000, 'L'), shorter = "short";
    for (const std::string* content : {&longer, &shorter, &longer, static_cast<const std::string*>(nullptr), &shorter}) {
        const std::string_view bytes = content ? std::string_view(*content) : std::string_view();
        std::string back;
        if (!RayoMd::File::WriteAll(path, bytes) || !RayoMd::File::ReadAll(path, 1 << 20, back) || back != bytes) {
            std::cerr << "WriteAll did not replace the file with " << bytes.size() << " bytes" << std::endl;
            return false;
        }
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    return true;
}

// A process keeps decoded images and failed paths for later documents (--serve, --batch, the app):
// an image file created, replaced or removed in between must be read again.
bool CheckChangedImageFiles() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "rayomd-test-changed-image";
    const fs::path assets = fs::path(RAYOMD_TEST_SOURCE_DIR) / "docs" / "assets";
    const fs::path mascot = assets / "branding" / "rayomd.png";
    const fs::path screenshot = assets / "demo" / "ui.png";
    fs::remove_all(root);
    for (const char* name : { "changing", "mascot", "screenshot" }) fs::create_directories(root / name);
    fs::copy_file(mascot, root / "mascot" / "pic.png");
    fs::copy_file(screenshot, root / "screenshot" / "pic.png");
    const auto build = [&](const char* directory, uint32_t failures, std::string& pdf) {
        TinyPdf::PdfOptions options;
        options.sourcePath = (root / directory / "doc.md").u8string();
        const TinyPdf::BuildResult result = TinyPdf::BuildPdf("![picture](pic.png)\n", options, pdf);
        return result.Ok() && result.failedImages == failures && ValidPdf(pdf);
    };
    std::string mascotPdf, screenshotPdf, missing, created, replaced, removed;
    const fs::path changing = root / "changing" / "pic.png";
    bool ok = build("mascot", 0, mascotPdf) && build("screenshot", 0, screenshotPdf) && mascotPdf != screenshotPdf &&
        build("changing", 1, missing);
    if (ok) {
        fs::copy_file(mascot, changing);
        ok = build("changing", 0, created) && created == mascotPdf;
    }
    if (ok) {
        fs::copy_file(screenshot, changing, fs::copy_options::overwrite_existing);
        ok = build("changing", 0, replaced) && replaced == screenshotPdf;
    }
    if (ok) {
        fs::remove(changing);
        ok = build("changing", 1, removed) && removed == missing;
    }
    fs::remove_all(root);
    if (!ok) std::cerr << "a changed image file was not read again" << std::endl;
    return ok;
}

// PDF/UA-1: a structure tree in reading order. Headings from H1, one level deeper at most; lists
// as L, LI, Lbl and LBody, an item's text the body's own content and a nested list in that body,
// a task's box its label with the state as ActualText; a table's header cells TH with a column
// scope, its header row repeated on a new page an artifact; an image a Figure with its alt text,
// without one an artifact; links and footnotes Link, Reference and Note elements whose
// annotations point back at them, their text in literal strings where it is ASCII. Every page's
// marked content numbered from 0, every sequence closed, bold text drawn once, and the catalog,
// pages and XMP that PDF/UA requires, without claiming PDF/A.
bool CheckTaggedPdf() {
    const auto npos = std::string::npos;
    std::string document = "## First\n\nSee [the site](https://example.com) and **bold**.[^n]\n\n"
        "- one\n  - inner\n- [x] done\n\n#### Deep\n\n![RayoMD](docs/assets/branding/rayomd.png)\n\n"
        "![](docs/assets/branding/rayomd.png)\n\n| A | B |\n|---|---|\n";
    for (int row = 0; row < 60; row++) document += "| r" + std::to_string(row) + " | v |\n";
    document += "\n[^n]: The note.\n";
    TinyPdf::PdfOptions options;
    options.pdfua = true;
    options.sourcePath = std::string(RAYOMD_TEST_SOURCE_DIR) + "/tester.md";
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf(document, options, pdf);
    bool numbered = true;
    int expected = 0;
    for (size_t at = pdf.find("<</MCID "); at != npos; at = pdf.find("<</MCID ", at + 1)) {
        const int mcid = std::atoi(pdf.c_str() + at + 8);
        numbered = numbered && (mcid == 0 || mcid == expected);
        expected = mcid + 1;
    }
    const size_t links = CountOccurrences(pdf, "/Subtype /Link");
    if (!result.Ok() || result.pages < 2 || !numbered || pdf.rfind("%PDF-1.7\n", 0) != 0 ||
        pdf.find("/Subtype /Type1") != npos || pdf.find("/MarkInfo << /Marked true >> /StructTreeRoot ") == npos ||
        pdf.find("/ViewerPreferences << /DisplayDocTitle true >>") == npos || pdf.find("/Lang (en)") == npos ||
        pdf.find("pdfuaid:part=\"1\"") == npos || pdf.find("pdfaid:part") != npos || pdf.find("/OutputIntents") != npos ||
        XmpText(pdf, "xml:lang=\"x-default\">", '<') != "First" ||
        CountOccurrences(pdf, "BDC\n") + CountOccurrences(pdf, "BMC\n") != CountOccurrences(pdf, "EMC\n") ||
        CountOccurrences(pdf, "/Tabs /S") != result.pages || CountOccurrences(pdf, "/StructParents ") != result.pages ||
        links < 3 || CountOccurrences(pdf, "/Type /OBJR") != links || CountOccurrences(pdf, "/StructParent ") != links ||
        CountOccurrences(pdf, " /Contents (") + CountOccurrences(pdf, " /Contents <FEFF") != links ||
        CountOccurrences(pdf, "/S /P ") != 2 ||
        pdf.find("/S /H1 ") == npos || pdf.find("/S /H2 ") == npos || pdf.find("/S /H4 ") != npos ||
        CountOccurrences(pdf, "/S /L ") != 2 || CountOccurrences(pdf, "/S /LI ") != 3 || CountOccurrences(pdf, "/S /LBody ") != 3 ||
        pdf.find("/ActualText <FEFF2611>") == npos || pdf.find("/ActualText <FEFF2022>") == npos ||
        pdf.find("/ActualText <FEFF25E6>") == npos || pdf.find("/ListNumbering /Disc") == npos ||
        pdf.find("/ListNumbering /Circle") == npos || CountOccurrences(pdf, "/S /TH ") != 2 ||
        CountOccurrences(pdf, "/Scope /Column") != 2 || CountOccurrences(pdf, "/S /TD ") != 120 ||
        CountOccurrences(pdf, "/S /Figure ") != 1 || pdf.find("/Alt (RayoMD)") == npos ||
        CountOccurrences(pdf, " Do Q") != 2 || pdf.find("/S /Note ") == npos || pdf.find("/IDTree ") == npos ||
        pdf.find("/S /Reference ") == npos || CountOccurrences(pdf, std::string("\0b\0o\0l\0d", 8)) != 1) {
        std::cerr << "tagged PDF mismatch" << std::endl;
        return false;
    }
    // Claimed with PDF/A too, the packet declares PDF/UA's schema to PDF/A.
    options.pdfa = true;
    if (!TinyPdf::BuildPdf(document, options, pdf).Ok() || pdf.find("pdfaid:part=\"3\"") == npos ||
        pdf.find("pdfuaid:part=\"1\"") == npos || pdf.find("<pdfaSchema:prefix>pdfuaid</pdfaSchema:prefix>") == npos) {
        std::cerr << "tagged PDF/A mismatch" << std::endl;
        return false;
    }
    return true;
}

// A list item that starts with "[ ]" or "[x]" shows a checkbox, with a check mark when done,
// where its bullet or number would be, and its text without the marker. "[ ]" elsewhere, and
// a task item in a quote, keep it as text. A bullet is drawn, not written: a disc, a circle one
// level deeper, then a square. In both renderers.
bool CheckTaskLists() {
    const std::string document = "- [ ] open\n- [x] done\n1. [X] numbered\n- normal\n  - nested\n    - deeper\n\n"
        "Text [ ] literal.\n\n> - [ ] quoted\n";
    for (const std::string& text : { document, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + document }) {
        std::string pdf;
        const bool unicode = text != document;
        if (!Build(text, pdf) || CountOccurrences(pdf, "0.42 0.47 0.53 RG") != 3 || CountOccurrences(pdf, " 1 J 1 j ") != 2 ||
            CountOccurrences(pdf, " h f Q\n") != 1 || CountOccurrences(pdf, " h S Q\n") != 1 ||
            CountOccurrences(pdf, " cm 0.08 g ") != 2 || CountOccurrences(pdf, " cm 0.08 G ") != 1 ||
            (!unicode && (pdf.find("(open) Tj") == std::string::npos || pdf.find("(normal) Tj") == std::string::npos ||
                pdf.find("(deeper) Tj") == std::string::npos || pdf.find("(- normal) Tj") != std::string::npos ||
                pdf.find("(Text [ ] literal.) Tj") == std::string::npos || pdf.find("(- [ ] quoted) Tj") == std::string::npos))) {
            std::cerr << "task lists mismatch (" << (unicode ? "Unicode" : "standard") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

// HTML comments are not shown, <br> ends a line, also in a table cell, and character references
// show the characters they stand for, except in code. "&copy;" keeps a document on the
// standard fonts in WinAnsiEncoding; "&rarr;" needs the Unicode renderer.
bool CheckHtmlInText() {
    std::string pdf;
    if (!Build("Before <!-- hidden --> after.\n\n<!-- block\nhidden too -->\n\nOne<br>Two<BR/>Three\n\n"
            "| x<br />y | &lt;1&gt; |\n|---|---|\n| &amp; | &#65;&#x42; |\n\n`&amp; <br>`\n", pdf) ||
        pdf.find("hidden") != std::string::npos || pdf.find("(Before after.) Tj") == std::string::npos ||
        pdf.find("(One) Tj") == std::string::npos || pdf.find("(Two) Tj") == std::string::npos ||
        pdf.find("(Three) Tj") == std::string::npos || pdf.find("(x) Tj") == std::string::npos ||
        pdf.find("(y) Tj") == std::string::npos || pdf.find("(<1>) Tj") == std::string::npos ||
        pdf.find("(&) Tj") == std::string::npos || pdf.find("(AB) Tj") == std::string::npos ||
        pdf.find("(&amp; <br>) Tj") == std::string::npos) {
        std::cerr << "HTML in text mismatch" << std::endl;
        return false;
    }
    std::string latin;
    std::string unicode;
    if (!Build("Year &copy; 2026.\n", latin) || latin.find("(Year \xA9 2026.) Tj") == std::string::npos ||
        latin.find("/Type0") != std::string::npos || !Build("A &rarr; B.\n", unicode) ||
        unicode.find("/Type0") == std::string::npos || unicode.find("2192") == std::string::npos) {
        std::cerr << "character reference routing mismatch" << std::endl;
        return false;
    }
    return true;
}

// With pageNumbers every page gets "N / M" in a content stream of its own, in Helvetica (/FN
// on Unicode pages); without it a page has one stream. In both renderers.
bool CheckPageNumbers() {
    std::string document = "# Pages\n\n";
    for (int line = 0; line < 120; line++) document += "Line " + std::to_string(line) + " of text that fills pages.\n\n";
    for (const std::string& text : { document, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + document }) {
        const bool unicode = text != document;
        TinyPdf::PdfOptions options;
        options.pageNumbers = true;
        std::string numbered;
        std::string plain;
        if (!TinyPdf::BuildPdf(text, options, numbered) || !Build(text, plain)) return false;
        const size_t pages = CountOccurrences(plain, "/Type /Page ");
        bool ok = pages >= 3 && plain.find("/Contents [") == std::string::npos &&
            CountOccurrences(numbered, "/Contents [") == pages && (numbered.find(" /FN ") != std::string::npos) == unicode;
        for (size_t page = 1; ok && page <= pages; page++) {
            ok = numbered.find("(" + std::to_string(page) + " / " + std::to_string(pages) + ") Tj") != std::string::npos;
        }
        if (!ok) {
            std::cerr << "page numbers mismatch (" << (unicode ? "Unicode" : "standard") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

// A document's title is the `title:` of its front matter, quoted or plain, else the text of its
// first heading; a document with neither gets none instead of a made-up one. Author, subject
// (else the description) and keywords come from the front matter too, as scalars, bracket
// lists or "- " lists, whose map items (Pandoc's authors) give their name; several authors are
// joined with "; ", keywords with ", ". A well-formed `lang:` is the catalog's /Lang. A front
// matter without its closing line gives nothing. In both renderers.
bool CheckDocumentMetadata() {
    struct Case { std::string markdown; std::vector<std::string> expected; std::vector<std::string> absent; };
    const Case cases[] = {
        { "---\ntitle: \"Q3 \\\"final\\\"\" # draft\nauthor: x\n---\n\n# Heading\n", { "/Title (Q3 \"final\")", "/Author (x)" }, {} },
        { "---\ntitle: it's (plain) # comment\n---\n\n# Heading\n", { "/Title (it's \\(plain\\))" }, { "/Author" } },
        { "Intro.\n\n## First **bold** heading\n\n# Second\n", { "/Title (First bold heading)" }, {} },
        { "# Gr\xC3\xB6\xC3\x9F" "e\n", { "/Title <FEFF0047007200F600DF0065>" }, {} },
        { "## Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n", { "/Title <FEFF005A0061017C00F301420107>" }, {} },
        { "No heading.\n", {}, { "/Title" } },
        { "---\ntitle: Report\nauthor: [Alice, \"Bob, Jr.\"]\nsubject: Quarterly figures\nkeywords: [report, 'q3, final']\n"
          "lang: en-US\n---\n\n# Heading\n",
          { "/Title (Report)", "/Author (Alice; Bob, Jr.)", "/Subject (Quarterly figures)", "/Keywords (report, q3, final)",
            "/Lang (en-US)" }, {} },
        { "---\nauthor:\n  - name: Alice\n    affiliation: Lab\n  - affiliation: Uni\n    name: Zo\xC3\xAB\n  - Carol\n"
          "keywords:\n- one\n# a comment\n- two\ndescription: From the description\nlang: not a tag!\n---\n\n# Heading\n",
          { "/Title (Heading)", "/Author <FEFF0041006C006900630065003B0020005A006F00EB003B0020004300610072006F006C>",
            "/Subject (From the description)", "/Keywords (one, two)" }, { "/Lang" } },
        { "---\nauthor: Nobody\nlang: en\n\n# Heading\n", {}, { "/Author", "/Lang" } },
    };
    for (const Case& c : cases) {
        // The standard fonts, and a Unicode font for the same metadata.
        for (const std::string& markdown : { c.markdown, c.markdown + "\nZa\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n" }) {
            std::string pdf;
            if (!Build(markdown, pdf)) return false;
            const size_t info = pdf.find("<< /Producer (RayoMD");
            const size_t catalog = pdf.find("<< /Type /Catalog");
            const std::string dictionaries = info == std::string::npos || catalog == std::string::npos ? std::string()
                : pdf.substr(info, pdf.find(">>", info) - info) + pdf.substr(catalog, pdf.find("\nendobj", catalog) - catalog);
            bool ok = !dictionaries.empty();
            for (const std::string& entry : c.expected) ok = ok && dictionaries.find(entry) != std::string::npos;
            for (const std::string& entry : c.absent) ok = ok && dictionaries.find(entry) == std::string::npos;
            if (!ok) {
                std::cerr << "document metadata mismatch: " << dictionaries << std::endl;
                return false;
            }
        }
    }
    return true;
}

// Text that is not UTF-8 is drawn with U+FFFD for the bytes that break it and the rest as it
// is: the Unicode renderer's decoder threw on such bytes and ended the process. In the front
// matter, a heading, a paragraph, link text and table cells.
bool CheckInvalidUtf8() {
    const std::string document =
        "---\ntitle: Bad \xFF title\n---\n\n# Head \xC3 ing\n\nhello \xC3\x28 world \xE2\x82\n\n"
        "[li\xFFnk](https://example.com) after\n\n| x\xE0 | y |\n|---|---|\n| 1 | 2\xF0\x9F |\n";
    std::string pdf;
    if (!Build(document, pdf) || pdf.find(std::string("\0w\0o\0r\0l\0d", 10)) == std::string::npos ||   // "world"
        pdf.find("/Type0") == std::string::npos || pdf.compare(pdf.size() - 6, 6, "%%EOF\n") != 0) {
        std::cerr << "invalid UTF-8 mismatch" << std::endl;
        return false;
    }
    return true;
}

// Headings get an outline entry, nested by level, and a link to "#anchor" goes where the entry
// of its heading goes: anchors as GitHub makes them, "-1" for a repeated one, the fragment
// percent-decoded and in any case. A link to an anchor the document lacks is dropped, and
// "#top" goes to the top of the first page. In both renderers.
bool CheckHeadingAnchors() {
    const auto destAfter = [](const std::string& pdf, size_t from) {
        const size_t start = from == std::string::npos ? from : pdf.find("/Dest [", from);
        return start == std::string::npos ? std::string() : pdf.substr(start, pdf.find(']', start) + 1 - start);
    };
    const std::string document =
        "# Intro\n\nSee [later](#Later-Part), [again](#again-1), [gone](#nope), [size](#gr%C3%B6%C3%9Fe) "
        "and [top](#top).\n\n## Later Part\n\n## Again\n\n## Again\n\n### Gr\xC3\xB6\xC3\x9F" "e\n";
    for (const std::string& text : { document, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + document }) {
        std::string pdf;
        if (!Build(text, pdf)) return false;
        std::vector<std::string> links;
        for (size_t at = pdf.find("/Subtype /Link"); at != std::string::npos; at = pdf.find("/Subtype /Link", at + 1)) {
            links.push_back(destAfter(pdf, at));
        }
        const size_t firstAgain = pdf.find("/Title (Again)");
        const std::string later = destAfter(pdf, pdf.find("/Title (Later Part)"));
        const std::string again = destAfter(pdf, firstAgain == std::string::npos ? firstAgain : pdf.find("/Title (Again)", firstAgain + 1));
        const std::string size = destAfter(pdf, pdf.find("/Title <FEFF0047007200F600DF0065>"));
        const size_t root = pdf.find("<< /Type /Outlines ");
        const size_t intro = pdf.find("/Title (Intro) ");
        if (root == std::string::npos || pdf.find(" /Count 5 >>", root) != pdf.find(" >>", root) - 9 ||
            intro == std::string::npos || pdf.find(" /Count 4 /Dest [", intro) > pdf.find("endobj", intro) ||
            later.empty() || again.empty() || size.empty() || again == destAfter(pdf, firstAgain) ||
            links != std::vector<std::string>{ later, again, size, links.size() == 4 ? links[3] : "" } ||
            links[3].find(" 0 R /XYZ null 842 null]") == std::string::npos) {
            std::cerr << "heading anchors mismatch (" << (text == document ? "standard" : "Unicode") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

// A document with characters the default font has no glyph for is drawn in a font that has
// them: here a TrueType collection given in RAYOMD_FALLBACK_FONT. What no font can show, a
// character beyond the BMP, is counted.
bool CheckFallbackFont() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "rayomd-test-fallback.ttc";
    {
        std::ofstream file(path, std::ios::binary);
        const std::string bytes = MinimalTrueTypeCollection();
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
#ifdef _WIN32
    _wputenv_s(L"RAYOMD_FALLBACK_FONT", path.wstring().c_str());
#else
    setenv("RAYOMD_FALLBACK_FONT", path.string().c_str(), 1);
#endif
    TinyPdf::PdfOptions options;
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf(u8"A \u4E2D \U0001F680 A\n", options, pdf);
    std::error_code error;
    std::filesystem::remove(path, error);
    if (!result.Ok() || pdf.find("RayoMD Native Tiny PDF") == std::string::npos || result.missingCharacters != 1) {
        std::cerr << "fallback font: build mismatch or " << result.missingCharacters << " missing characters" << std::endl;
        return false;
    }
    // A default font with CJK glyphs needs no fallback; otherwise the test font draws the
    // document, as a TrueType program of its own taken out of the collection.
    if (pdf.find("/FontBBox [0 0 700 700]") == std::string::npos) return true;
    const size_t file = pdf.find("/Length1 ");
    const size_t stream = file == std::string::npos ? file : pdf.find("stream\n", file);
    if (stream == std::string::npos || pdf.compare(stream + 7, 4, std::string("\x00\x01\x00\x00", 4)) != 0) {
        std::cerr << "fallback font: the font program is not a TrueType font" << std::endl;
        return false;
    }
    return true;
}

// A composite glyph is as wide as its component flagged USE_MY_METRICS, not as its own hmtx
// entry says (0 here; wqy-microhei's Hangul syllables list a quarter em there): in /W, which
// the layout shares.
bool CheckComposedAdvance() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "rayomd-test-composed.ttc";
    {
        std::ofstream file(path, std::ios::binary);
        const std::string bytes = MinimalTrueTypeCollection();
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
#ifdef _WIN32
    _wputenv_s(L"RAYOMD_FALLBACK_FONT", path.wstring().c_str());
#else
    setenv("RAYOMD_FALLBACK_FONT", path.string().c_str(), 1);
#endif
    TinyPdf::PdfOptions options;
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf(u8"A \uAC00\uAC00 A\n", options, pdf);
    std::error_code error;
    std::filesystem::remove(path, error);
    if (!result.Ok()) {
        std::cerr << "composed advance: build failed" << std::endl;
        return false;
    }
    // A default font with Hangul glyphs needs no fallback.
    if (pdf.find("/FontBBox [0 0 700 700]") == std::string::npos) return true;
    if (pdf.find("44032 [ 800 ]") == std::string::npos) {
        std::cerr << "composed advance: /W misses the component's advance for U+AC00" << std::endl;
        return false;
    }
    return true;
}

// OpenType fonts with PostScript outlines: a glyph whose charstring calls a local subroutine and
// has a hint mask comes out of Desubroutinize self-contained (the call and its operand gone, the
// subroutine inline, the mask with its byte), seac and endless recursion fail, and the CID-keyed
// subset and the simple font written of it read back as CFF fonts with that glyph.
bool CheckCffSubset() {
    using namespace TinyPdf::Internal::Cff;
    auto index = [](const std::string& item) {     // an INDEX of one item, offsets of one byte
        std::string out("\x00\x01\x01\x01", 4);
        out.push_back(static_cast<char>(item.size() + 1));
        return out + item;
    };
    auto int5 = [](uint32_t value) {
        std::string out(1, '\x1D');
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((value >> shift) & 0xFF));
        return out;
    };
    // A name-keyed CFF of one glyph: the top DICT gives CharStrings and Private, the private
    // DICT its Subrs right after it.
    auto font = [&](const std::string& glyph, const std::string& subr) {
        const std::string head = std::string("\x01\x00\x04\x01", 4) + index("T");
        const size_t topIndexBytes = 5 + 17;
        const size_t charStringsAt = head.size() + topIndexBytes + 2 + 2;
        const std::string charStrings = index(glyph);
        const size_t privateAt = charStringsAt + charStrings.size();
        const std::string top = int5(static_cast<uint32_t>(charStringsAt)) + "\x11" + int5(6) +
            int5(static_cast<uint32_t>(privateAt)) + "\x12";
        return head + index(top) + std::string(4, '\0') + charStrings + int5(6) + "\x13" + index(subr);
    };
    auto flatten = [](const std::string& bytes, uint16_t glyphs, uint16_t glyph, CffFont& parsed, std::string& out) {
        return ParseCff(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), glyphs, parsed) &&
            Desubroutinize(parsed, glyph, out);
    };
    // hstemhm (two stems), hintmask after one implicit vstem (a mask byte), callsubr -107
    // (subroutine 0: rmoveto, return), hlineto, endchar.
    const std::string glyph("\x8B\x95\xBD\x9F\x12" "\x8B\x95\x13\xE0" "\x20\x0A" "\x95\x06" "\x0E", 15);
    const std::string expected("\x8B\x95\xBD\x9F\x12" "\x8B\x95\x13\xE0" "\x9F\x9F\x15" "\x95\x06" "\x0E", 15);
    const std::string source = font(glyph, std::string("\x9F\x9F\x15\x0B", 4));
    CffFont parsed;
    std::string flat;
    if (!flatten(source, 1, 0, parsed, flat) || flat != expected || parsed.cidKeyed) {
        std::cerr << "CFF: the desubroutinized glyph differs" << std::endl;
        return false;
    }
    CffFont other;
    std::string ignored;
    if (flatten(font(std::string("\x8B\x8B\x8B\x8B\x0E", 5), std::string("\x0B", 1)), 1, 0, other, ignored) ||
        flatten(font(std::string("\x20\x0A\x0E", 3), std::string("\x20\x0A\x0B", 3)), 1, 0, other, ignored) ||
        ParseCff(reinterpret_cast<const uint8_t*>(source.data()), source.size(), 2, other)) {
        std::cerr << "CFF: seac, endless recursion or a wrong glyph count passed" << std::endl;
        return false;
    }
    std::string subset;
    CffFont cid;
    if (!WriteCidKeyedSubset(parsed, "T", { { 0x4E2D, 0 } }, subset) || !flatten(subset, 2, 1, cid, flat) ||
        flat != expected || !cid.cidKeyed || cid.fds.size() != 1 ||
        subset.find(std::string("\x00\x00\x4E\x2D", 4)) == std::string::npos) {
        std::cerr << "CFF: the CID-keyed subset does not read back" << std::endl;
        return false;
    }
    const SimpleGlyph simpleGlyph{ 'A', 34, {}, expected };
    const int16_t box[4] = { 0, -10, 500, 700 };
    std::string simple;
    CffFont named;
    if (!WriteSimpleFont("S", &simpleGlyph, 1, box, simple) || !flatten(simple, 2, 1, named, flat) ||
        flat != expected || named.cidKeyed) {
        std::cerr << "CFF: the simple font does not read back" << std::endl;
        return false;
    }
    return true;
}

// GFM table rows: a table goes on up to a blank line or the start of another block, a line
// without pipes is a row too, and every row has the header's number of cells.
bool CheckTableRows() {
    using TinyPdf::Internal::BlockType;
    const auto blocks = TinyPdf::Internal::ParseMarkdown(
        "| a | b |\n|---|---|\n| one |\nbare text\n| 1 | 2 | 3 |\n- item\n\n"
        "| c | d |\n|---|---|\n| x | y |\n\nafter\n");
    const std::vector<std::vector<std::string>> first = {{"a", "b"}, {"one", ""}, {"bare text", ""}, {"1", "2"}};
    const std::vector<std::vector<std::string>> second = {{"c", "d"}, {"x", "y"}};
    if (blocks.size() != 4 || blocks[0].type != BlockType::Table || blocks[0].rows != first ||
        blocks[1].type != BlockType::Bullet || blocks[1].text != "item" || blocks[2].type != BlockType::Table ||
        blocks[2].rows != second || blocks[3].type != BlockType::Paragraph || blocks[3].text != "after") {
        std::cerr << "GFM table rows mismatch" << std::endl;
        return false;
    }
    return true;
}

// Table cells show their inline Markdown in both renderers: emphasis in its face, code in
// Courier on a background, links that can be clicked where the cell shows them; escaped
// markers and other backslashes stay as written.
bool CheckTableCellMarkdown() {
    const std::string table =
        "| Name | *Note* |\n"
        "|---|---:|\n"
        "| **bold** cell | [site](https://example.com/cell) |\n"
        "| `code` x | \\*stars\\* C:\\dir |\n";
    const double cellRight = 595.0 - 54.0 - 5.0;     // A4 less the normal margin and the cell padding
    std::string pdf;
    if (!Build(table, pdf) || pdf.find("RayoMD Native Standard PDF") == std::string::npos) {
        std::cerr << "table cell Markdown: standard build mismatch" << std::endl;
        return false;
    }
    const std::pair<const char*, const char*> expected[] = {
        {"Name", "F2"}, {"Note", "F5"}, {"bold", "F2"}, {" cell", "F1"}, {"site", "F1"}, {"code", "F3"},
        {" x", "F1"}, {"*stars* C:\\dir", "F1"}};
    const std::vector<StandardTextOp> ops = StandardTextOps(pdf);
    for (const auto& [text, font] : expected) {
        const auto op = std::find_if(ops.begin(), ops.end(),
            [&](const StandardTextOp& candidate) { return candidate.text == text; });
        if (op == ops.end() || op->font != font) {
            std::cerr << "table cell Markdown: \"" << text << "\" is not shown in " << font << std::endl;
            return false;
        }
    }
    std::array<double, 4> link{};
    if (!FindLinkRectangle(pdf, "https://example.com/cell", link) || std::abs(link[2] - cellRight) > 0.05) {
        std::cerr << "table cell Markdown: the link is not where its right-aligned cell shows it" << std::endl;
        return false;
    }
    std::string unicodePdf;
    if (!Build(u8"Za\u017C\u00F3\u0142\u0107\n\n" + table, unicodePdf) ||
        unicodePdf.find("RayoMD Native Tiny PDF") == std::string::npos ||
        !FindLinkRectangle(unicodePdf, "https://example.com/cell", link) || std::abs(link[2] - cellRight) > 0.05 ||
        unicodePdf.find("0.94 0.94 0.92 rg") == std::string::npos) {
        std::cerr << "table cell Markdown: Unicode link or code background mismatch" << std::endl;
        return false;
    }
    return true;
}

// Latin text needs no font file: documents whose characters all have a code in
// WinAnsiEncoding are transcoded and shown in the standard fonts.
bool CheckLatinText() {
    using RayoMd::Text::TranscodeToWinAnsi;
    using RayoMd::Text::WinAnsiToUtf8;
    std::string out;
    if (!TranscodeToWinAnsi(u8"caf\u00E9 \u20AC \u201Cq\u201D \u2013 x\u2026", &out) ||
        out != "caf\xE9 \x80 \x93q\x94 \x96 x\x85") {
        std::cerr << "WinAnsi transcoding mismatch" << std::endl;
        return false;
    }
    // Not UTF-8 (overlong, surrogate, cut short), or a character WinAnsi has no code for.
    for (const char* rejected : {"\xC0\xAF", "\xED\xA0\x80", "caf\xC3", u8"Za\u017C\u00F3\u0142\u0107", u8"\u2192"}) {
        if (TranscodeToWinAnsi(rejected, nullptr)) {
            std::cerr << "WinAnsi transcoding accepted text it cannot show" << std::endl;
            return false;
        }
    }
    // The status symbols become their ASCII forms; invisible marks go. Text whose WinAnsi
    // bytes would read as a symbol again ("a circumflex, oe, ellipsis") stays Unicode.
    if (!TranscodeToWinAnsi(u8"\uFEFFok \u2705 \u26A0\uFE0F \u274C so\u00ADft", &out) ||
        out != "ok [OK] [!] [X] soft" || TranscodeToWinAnsi(u8"\u00E2\u0153\u2026", nullptr)) {
        std::cerr << "WinAnsi transcoding of symbols and invisible marks mismatch" << std::endl;
        return false;
    }
    // Without any TrueType font, Latin Extended-A letters fall back to their base letter and
    // other characters, and bytes that are not UTF-8, to '?'; each is counted.
    if (RayoMd::Text::TranscodeToWinAnsiLossy(u8"Za\u017C\u00F3\u0142\u0107 \u65E5\u672C \u2705 x\xFF", out) != 6 ||
        out != "Zaz\xF3lc ?? [OK] x?") {
        std::cerr << "lossy WinAnsi transcoding mismatch: " << out << std::endl;
        return false;
    }
    for (int code = 0x20; code < 0x100; code++) {
        const std::string single(1, static_cast<char>(code));
        const std::string utf8 = WinAnsiToUtf8(single);
        if (utf8.empty() || code == 0x7F || code == 0xAD) continue;    // no glyph; the soft hyphen is dropped
        if (!TranscodeToWinAnsi(utf8, &out) || out != single) {
            std::cerr << "WinAnsi code " << code << " does not survive a round trip" << std::endl;
            return false;
        }
    }

    const std::string latin =
        u8"# Gr\u00F6\u00DFe\n\n"
        u8"Caf\u00E9 \u201Ccr\u00E8me\u201D \u2013 5\u00A0\u20AC, [Gr\u00F6\u00DFe](https://de.wikipedia.org/wiki/Gr\u00F6\u00DFe) "
        u8"and $\\text{caf\u00E9}^2$ \u2705.\n\n```\nprint('\u2705 ok')\n```\n";
    std::string pdf;
    if (!Build(latin, pdf) || pdf.find("RayoMD Native Standard PDF") == std::string::npos ||
        pdf.find("/FontFile2") != std::string::npos) {
        std::cerr << "Latin text does not take the standard fonts" << std::endl;
        return false;
    }
    std::string shown;
    for (const StandardTextOp& op : StandardTextOps(pdf)) shown += op.text + "\n";
    // The no-break space joins "5" and the Euro sign into one word; the formula's Latin text
    // is drawn in Helvetica by the math fallback.
    if (shown.find("Gr\xF6\xDF" "e") == std::string::npos || shown.find("Caf\xE9 \x93" "cr\xE8me\x94 \x96 5\xA0\x80") ==
        std::string::npos || shown.find("caf\xE9") == std::string::npos ||
        shown.find("[OK]") == std::string::npos || shown.find("print('[OK] ok')") == std::string::npos) {
        std::cerr << "Latin text shown as:\n" << shown << std::endl;
        return false;
    }
    if (pdf.find("/URI (https://de.wikipedia.org/wiki/Gr%C3%B6%C3%9Fe)") == std::string::npos) {
        std::cerr << "Latin link target is not percent-encoded UTF-8" << std::endl;
        return false;
    }
    std::string unicodePdf;
    if (!Build(u8"Za\u017C\u00F3\u0142\u0107 [Gr\u00F6\u00DFe](https://de.wikipedia.org/wiki/Gr\u00F6\u00DFe)\n", unicodePdf) ||
        unicodePdf.find("RayoMD Native Tiny PDF") == std::string::npos ||
        unicodePdf.find("/URI (https://de.wikipedia.org/wiki/Gr%C3%B6%C3%9Fe)") == std::string::npos) {
        std::cerr << "Unicode link target is not percent-encoded UTF-8" << std::endl;
        return false;
    }
    std::string reversible;
    if (!BuildReversible(latin, reversible) || RayoMd::PdfSource::Inspect(reversible, true).source != latin) {
        std::cerr << "reversible Latin PDF does not embed the UTF-8 source" << std::endl;
        return false;
    }
    return true;
}

// MeasureWords and SumAdvances (word_bounds.h) against a byte loop: every length from 0 to 300
// and some longer, texts of words, runs of each white-space byte, bytes from 0x80 and tables
// whose entry 256 is not 0 (which no sum may read); the dispatched code (AVX2 where the
// CPU has it), the SSE2 code and the portable code, a slice of a larger buffer each time, so a
// read past its end would read other bytes.
bool CheckWordBounds() {
    using namespace TinyPdf::Internal;
    uint32_t seed = 20261019u;
    auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 16;
    };
    std::array<uint16_t, 257> advances{};
    for (size_t i = 0; i < 256; i++) advances[i] = static_cast<uint16_t>(1 + next() % 999);
    for (const unsigned char white : { ' ', '\t', '\r', '\n' }) advances[white] = 0;
    advances[256] = 0xBEEF;
    std::vector<size_t> lengths;
    for (size_t n = 0; n <= 300; n++) lengths.push_back(n);
    lengths.insert(lengths.end(), { 511, 512, 513, 4095, 4096, 70000 });
    std::string buffer;
    for (size_t n : lengths) {
        for (int pattern = 0; pattern < 6; pattern++) {
            buffer.assign(n + 128, 'x');
            const size_t offset = 64 + next() % 32;
            buffer.resize(offset + n + 64, 'y');
            char* const text = &buffer[offset];
            for (size_t i = 0; i < n; i++) {
                const uint32_t r = next();
                switch (pattern) {
                case 0: text[i] = r % 6 == 0 ? ' ' : static_cast<char>('a' + r % 26); break;   // words
                case 1: text[i] = static_cast<char>(" \t\r\n"[r % 4]); break;                 // white only
                case 2: text[i] = static_cast<char>(r); break;                                // any byte
                case 3: text[i] = r % 3 == 0 ? static_cast<char>(" \t\r\n"[r % 4]) : static_cast<char>(0x80 | r); break;
                case 4: text[i] = 'w'; break;                                                 // one word
                default: text[i] = i % 64 < 32 ? ' ' : 'z'; break;                            // across steps
                }
            }
            std::vector<uint32_t> bounds;
            std::vector<uint32_t> sums(n + 1, 0);
            bool inWord = false;
            for (size_t i = 0; i < n; i++) {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                sums[i + 1] = sums[i] + advances[c];
                const bool white = c == ' ' || c == '\t' || c == '\r' || c == '\n';
                if (white == inWord) {
                    bounds.push_back(static_cast<uint32_t>(i));
                    inWord = !white;
                }
            }
            if (inWord) bounds.push_back(static_cast<uint32_t>(n));
            const size_t summed = n - next() % (n + 1) / 2;
            using Measure = size_t (*)(const char*, size_t, size_t, const uint16_t*, uint32_t*, uint32_t*);
            std::vector<std::pair<const char*, Measure>> variants = { { "dispatched", MeasureWords },
                { "portable", MeasureWordsPortable } };
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
            if (n >= 16) variants.push_back({ "SSE2", MeasureWordsSse2 });
#endif
            for (const auto& variant : variants) {
                std::vector<uint32_t> gotBounds(n + 1 + kWordBoundsSlack, 0xFFFFFFFFu);
                std::vector<uint32_t> gotSums(n + 1, 0xFFFFFFFFu);
                const size_t words = variant.second(text, n, summed, advances.data(), gotSums.data(), gotBounds.data());
                bool same = words * 2 == bounds.size() && std::equal(bounds.begin(), bounds.end(), gotBounds.begin()) &&
                    std::equal(sums.begin(), sums.begin() + summed + 1, gotSums.begin());
                for (size_t i = summed + 1; same && i <= n; i++) same = gotSums[i] == 0xFFFFFFFFu;
                if (!same) {
                    std::cerr << "MeasureWords (" << variant.first << ") wrong: " << n << " bytes, pattern " << pattern
                              << ", " << summed << " summed" << std::endl;
                    return false;
                }
            }
            std::vector<uint32_t> gotSums(n + 1, 0xFFFFFFFFu);
            gotSums[0] = 7;
            SumAdvances(text, n, advances.data(), gotSums.data());
            for (size_t i = 0; i <= n; i++) {
                if (gotSums[i] != sums[i] + 7) {
                    std::cerr << "SumAdvances wrong: " << n << " bytes, pattern " << pattern << std::endl;
                    return false;
                }
            }
        }
    }
    // MeasureWideWords against a loop over code units, as ScanWideUnits reads them: widths from a
    // cache with holes (entry 0: asked of the callback, which here does not fill it), surrogate
    // pairs (also across 8- and 64-unit steps), lone surrogates, every white space and the
    // characters next to it.
    std::vector<uint16_t> cache(65537, 0);
    for (size_t i = 0; i < cache.size(); i++) cache[i] = next() % 7 == 0 ? 0 : static_cast<uint16_t>(1 + next() % 1500);
    struct Asked { const std::vector<uint16_t>* cache; } asked{ &cache };
    const WideWidth width = [](const void* context, uint32_t codepoint) -> uint32_t {
        (void)context;
        return codepoint > 0xFFFF ? 777u : 3u + codepoint % 11u;
    };
    for (size_t n : lengths) {
        if (n > 5000) continue;
        for (int pattern = 0; pattern < 6; pattern++) {
            std::vector<wchar_t> text(n + 64, L'x');
            for (size_t i = 0; i < n; i++) {
                const uint32_t r = next();
                switch (pattern) {
                case 0: text[i] = r % 6 == 0 ? L' ' : static_cast<wchar_t>(L'a' + r % 26); break;
                case 1: text[i] = static_cast<wchar_t>(L" \t\r\n"[r % 4]); break;
                case 2: text[i] = static_cast<wchar_t>(r & 0xFFFF); break;                       // any unit
                case 3: text[i] = static_cast<wchar_t>(r % 4 == 0 ? 0xD800 + r % 0x400 : r % 4 == 1 ? 0xDC00 + r % 0x400
                    : r % 4 == 2 ? L' ' : 0x4E00 + r % 0x100); break;                             // pairs and lone halves
                case 4: text[i] = static_cast<wchar_t>(i % 2 == 0 ? 0xD83D : 0xDE00); break;   // a pair every two units
                default: text[i] = i % 64 < 31 ? L'\t' : static_cast<wchar_t>(0x3042 + i % 50); break;
                }
            }
            if (pattern == 4 && n > 0 && next() % 2 == 0) text[0] = L'y';                   // pairs at odd offsets
            std::vector<uint32_t> bounds;
            std::vector<uint32_t> sums(n + 1, 0);
            bool inWord = false;
            for (size_t i = 0; i < n; i++) {
                const wchar_t c = text[i];
                const bool white = c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
                if (white == inWord) {
                    bounds.push_back(static_cast<uint32_t>(i));
                    inWord = !white;
                }
                const uint32_t unit = static_cast<uint16_t>(c);
                if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < n && static_cast<uint16_t>(text[i + 1]) >= 0xDC00 &&
                    static_cast<uint16_t>(text[i + 1]) <= 0xDFFF) {
                    sums[i + 1] = sums[i] + width(&asked, 0x10000 + ((unit - 0xD800) << 10) + (static_cast<uint16_t>(text[i + 1]) - 0xDC00));
                    i++;
                    sums[i + 1] = sums[i];
                    continue;
                }
                sums[i + 1] = sums[i] + (cache[unit] != 0 ? cache[unit] : width(&asked, unit));
            }
            if (inWord) bounds.push_back(static_cast<uint32_t>(n));
            using MeasureWide = size_t (*)(const wchar_t*, size_t, const uint16_t*, WideWidth, const void*, uint32_t*, uint32_t*);
            for (const auto& variant : { std::pair<const char*, MeasureWide>{ "dispatched", MeasureWideWords },
                     std::pair<const char*, MeasureWide>{ "portable", MeasureWideWordsPortable } }) {
                std::vector<uint32_t> gotBounds(n + 1 + kWordBoundsSlack, 0xFFFFFFFFu);
                std::vector<uint32_t> gotSums(n + 1, 0xFFFFFFFFu);
                const size_t words = variant.second(text.data(), n, cache.data(), width, &asked, gotSums.data(), gotBounds.data());
                if (words * 2 != bounds.size() || !std::equal(bounds.begin(), bounds.end(), gotBounds.begin()) || gotSums != sums) {
                    std::cerr << "MeasureWideWords (" << variant.first << ") wrong: " << n << " units, pattern " << pattern
                              << std::endl;
                    return false;
                }
            }
        }
    }
    return true;
}

// SplitColorRuns (word_bounds.h) against the loop over characters: every length from 0 to 200 and
// some longer, in narrow and wide characters, lines of spaces, runs of one colour, colours that
// change at every character and across 16-character steps, classes whose colours repeat
// (kCodeColorOfClass), a slice of a larger buffer each time.
template <typename Char>
bool CheckColorRunsOf(const char* name) {
    using namespace TinyPdf::Internal;
    uint32_t seed = 20261020u;
    auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 16;
    };
    std::vector<size_t> lengths;
    for (size_t n = 0; n <= 200; n++) lengths.push_back(n);
    lengths.insert(lengths.end(), { 255, 256, 257, 1023, 4096 });
    for (size_t n : lengths) {
        for (int pattern = 0; pattern < 6; pattern++) {
            std::vector<Char> text(n + 64, static_cast<Char>('q'));
            std::vector<uint8_t> classes(n + 64, 0);
            for (size_t i = 0; i < n; i++) {
                const uint32_t r = next();
                bool space = false;
                uint8_t cls = 0;
                switch (pattern) {
                case 0: space = r % 4 == 0; cls = static_cast<uint8_t>(r / 7 % kCodeClassCount); break;
                case 1: space = true; break;                                            // spaces only
                case 2: space = false; cls = static_cast<uint8_t>(r % kCodeClassCount); break;
                case 3: space = i < 40 || r % 2 == 0; cls = static_cast<uint8_t>(i / 16 % kCodeClassCount); break;
                case 4: space = r % 9 == 0; cls = 3; break;                             // one colour
                default: space = i % 32 >= 16; cls = static_cast<uint8_t>(i / 32 % kCodeClassCount); break;
                }
                text[i] = space ? static_cast<Char>(' ') : static_cast<Char>(r % 3 == 0 ? (sizeof(Char) > 1 ? 0x2520 : 0xA0) : 'a' + r % 26);
                classes[i] = space ? static_cast<uint8_t>(r % kCodeClassCount) : cls;
            }
            std::vector<ColorRun> want(n + 1), got(n + 1, ColorRun{ 0xFFFFFFFFu, 0xEE });
            const size_t wantCount = SplitColorRunsScalar(text.data(), n, classes.data(), kCodeColorOfClass, want.data());
            const size_t gotCount = SplitColorRuns(text.data(), n, classes.data(), kCodeColorOfClass, got.data());
            bool same = wantCount == gotCount;
            for (size_t i = 0; same && i < wantCount; i++) same = want[i].end == got[i].end && want[i].color == got[i].color;
            if (!same) {
                std::cerr << "SplitColorRuns (" << name << ") wrong: " << n << " characters, pattern " << pattern << std::endl;
                return false;
            }
        }
    }
    return true;
}

bool CheckColorRuns() {
    return CheckColorRunsOf<char>("char") && CheckColorRunsOf<wchar_t>("wchar_t");
}

// The PNG row filters against the textbook definitions (PNG specification, section 9): the
// residuals of every filter, the encoder heuristic's choice (least sum of residual magnitudes as
// signed bytes, the lowest filter on ties), the dispatched code (AVX2 where the CPU has it) and
// the SSE2 code against the scalar reference, and unfiltering back to the row. Rows of 1 to 4
// bytes per pixel and every width around the 16- and 32-byte vectors, with patterns that tie,
// saturate, or favour each filter.
bool CheckPngFilters() {
    using namespace TinyPdf::Internal;
    uint32_t seed = 20261013u;
    auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return (uint8_t)(seed >> 24);
    };
    auto paeth = [](int a, int b, int c) {
        int p = a + b - c;
        int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
        if (pa <= pb && pa <= pc) return a;
        return pb <= pc ? b : c;
    };
    std::vector<size_t> widths;
    for (size_t width = 1; width <= 80; width++) widths.push_back(width);
    widths.insert(widths.end(), {255, 256, 1000, 4099});
    std::vector<uint8_t> up, row, expected, filtered, scalar, back;
    for (size_t bpp = 1; bpp <= 4; bpp++) {
        for (size_t rowBytes : widths) {
            for (int pattern = 0; pattern < 7; pattern++) {
                up.assign(rowBytes, 0);
                row.assign(rowBytes, 0);
                for (size_t x = 0; x < rowBytes; x++) {
                    uint8_t noise = next();
                    switch (pattern) {
                    case 0: up[x] = next(); row[x] = noise; break;                          // noise
                    case 1: up[x] = 77; row[x] = 77; break;                                  // flat: ties
                    case 2: row[x] = noise; break;                                           // first row
                    case 3: up[x] = (uint8_t)(x * 5); row[x] = (uint8_t)(x * 5 + 3); break;  // gradient
                    case 4: up[x] = next(); row[x] = (uint8_t)(up[x] + (noise & 3)); break;  // like the row above
                    case 5: up[x] = (uint8_t)(noise & 128 ? 255 : 0); row[x] = (uint8_t)(noise & 1 ? 128 : 0); break;
                    default: up[x] = next(); row[x] = (uint8_t)(x >= bpp ? row[x - bpp] + (noise & 1) : noise); break;
                    }
                }
                uint64_t costs[5] = {};
                for (int filter = 0; filter < 5; filter++) {
                    expected.assign(rowBytes, 0);
                    for (size_t x = 0; x < rowBytes; x++) {
                        int a = x >= bpp ? row[x - bpp] : 0, b = up[x], c = x >= bpp ? up[x - bpp] : 0;
                        int predictor = filter == 0 ? 0 : filter == 1 ? a : filter == 2 ? b
                            : filter == 3 ? (a + b) / 2 : paeth(a, b, c);
                        expected[x] = (uint8_t)(row[x] - predictor);
                        costs[filter] += (uint64_t)std::abs((int)(int8_t)expected[x]);
                    }
                    filtered.assign(rowBytes, 0);
                    scalar.assign(rowBytes, 0);
                    back.assign(rowBytes, 0);
                    FilterPngRow(filter, row.data(), up.data(), rowBytes, bpp, filtered.data());
                    FilterPngRowScalar(filter, row.data(), up.data(), rowBytes, bpp, scalar.data());
                    bool unfiltered = UnfilterPngRow((uint8_t)filter, filtered.data(), up.data(), rowBytes, bpp, back.data());
                    bool sse2 = true;
#if RAYOMD_PNG_SSE2
                    std::vector<uint8_t> sse2Filtered(rowBytes, 0);
                    FilterPngRowSse2(filter, row.data(), up.data(), rowBytes, bpp, sse2Filtered.data());
                    sse2 = sse2Filtered == expected;
#endif
                    if (filtered != expected || scalar != expected || !sse2 || !unfiltered || back != row) {
                        std::cerr << "PNG filter " << filter << " wrong: " << bpp << " bytes per pixel, " << rowBytes
                                  << " bytes, pattern " << pattern << std::endl;
                        return false;
                    }
                }
                int best = 0;
                for (int filter = 1; filter < 5; filter++) {
                    if (costs[filter] < costs[best]) best = filter;
                }
                int chosen = ChoosePngFilter(row.data(), up.data(), rowBytes, bpp);
                int reference = ChoosePngFilterScalar(row.data(), up.data(), rowBytes, bpp);
#if RAYOMD_PNG_SSE2
                if (ChoosePngFilterSse2(row.data(), up.data(), rowBytes, bpp) != best) reference = -1;
#endif
                if (chosen != best || reference != best) {
                    std::cerr << "PNG filter choice " << chosen << " / " << reference << ", expected " << best << ": "
                              << bpp << " bytes per pixel, " << rowBytes << " bytes, pattern " << pattern << std::endl;
                    return false;
                }
            }
        }
    }
    // Two rows unfiltered together give both rows back, for every pair of filters: two Paeth rows
    // of 3 or 4 bytes a pixel take the paired AVX2 code where the CPU has it.
    for (size_t bpp = 1; bpp <= 4; bpp++) {
        for (size_t pixels : { 1, 2, 3, 5, 17, 64, 333 }) {
            const size_t rowBytes = pixels * bpp;
            std::vector<uint8_t> above(rowBytes), first(rowBytes), second(rowBytes);
            for (size_t x = 0; x < rowBytes; x++) {
                above[x] = next();
                first[x] = (uint8_t)(x % 7 < 4 ? above[x] + next() % 9 : next());
                second[x] = (uint8_t)(x % 5 < 3 ? first[x] - next() % 9 : next());
            }
            for (int filterA = 0; filterA < 5; filterA++) {
                for (int filterB = 0; filterB < 5; filterB++) {
                    std::vector<uint8_t> rawA(rowBytes), rawB(rowBytes), backA(rowBytes, 0), backB(rowBytes, 0);
                    FilterPngRowScalar(filterA, first.data(), above.data(), rowBytes, bpp, rawA.data());
                    FilterPngRowScalar(filterB, second.data(), first.data(), rowBytes, bpp, rawB.data());
                    if (!UnfilterPngRows((uint8_t)filterA, rawA.data(), (uint8_t)filterB, rawB.data(), above.data(), rowBytes,
                            bpp, backA.data(), backB.data()) || backA != first || backB != second) {
                        std::cerr << "PNG rows unfiltered together wrong: filters " << filterA << " and " << filterB
                                  << ", " << bpp << " bytes per pixel, " << pixels << " pixels" << std::endl;
                        return false;
                    }
                }
            }
        }
    }
    uint8_t one = 1, out = 0;
    if (UnfilterPngRow(5, &one, &one, 1, 1, &out)) {
        std::cerr << "PNG filter 5 accepted" << std::endl;
        return false;
    }
    return true;
}

// PdfOptions::sourceMap: each top-level block's lines and where it is laid out, in layout order,
// footnote definitions without an entry; the PDF bytes are those of a build without it, also in
// WinAnsi and in a TrueType font. A block after a page break starts at the top of the next page,
// one across pages ends on a later one, a cover page moves every page on, and a book leaves the
// map as it was.
bool CheckSourceMap() {
    using TinyPdf::SourceBlock;
    using Lines = std::vector<std::pair<uint32_t, uint32_t>>;
    const auto lines = [](const std::vector<SourceBlock>& map) {
        Lines ranges;
        for (const SourceBlock& entry : map) ranges.emplace_back(entry.firstLine, entry.endLine);
        return ranges;
    };
    const auto build = [](const std::string& markdown, TinyPdf::PdfOptions options, std::vector<SourceBlock>* map,
        std::string& pdf) {
        options.sourceMap = map;
        return TinyPdf::BuildPdf(markdown, options, pdf).Ok();
    };
    const std::string document =
        "# Title\n\n"
        "First paragraph\ncontinues here.\n\n"
        "- one\n- two\n\n"
        "```text\ncode\n```\n\n"
        "| a | b |\n|---|---|\n| 1 | 2 |\n\n"
        "Note[^n].\n\n"
        "[^n]: The note.\n";
    const TinyPdf::PdfOptions defaults;
    std::vector<SourceBlock> first;
    std::string mapped, plain;
    if (!build(document, defaults, &first, mapped) || !build(document, defaults, nullptr, plain)) {
        std::cerr << "source map: the document did not build" << std::endl;
        return false;
    }
    if (lines(first) != Lines{ {0, 1}, {2, 4}, {5, 6}, {6, 8}, {8, 11}, {12, 15}, {16, 17} }) {
        std::cerr << "source map: the blocks' lines differ" << std::endl;
        return false;
    }
    if (first[0].top != 54.0f) {
        std::cerr << "source map: the first block does not start at the margin: " << first[0].top << std::endl;
        return false;
    }
    for (size_t k = 0; k < first.size(); k++) {
        if (first[k].page != 0 || first[k].endPage != 0 || !(first[k].top < first[k].bottom) ||
            (k + 1 < first.size() && first[k].bottom != first[k + 1].top)) {
            std::cerr << "source map: block " << k << " is not where the one before it ends" << std::endl;
            return false;
        }
    }
    if (mapped != plain) {
        std::cerr << "source map: the PDF bytes differ with a map" << std::endl;
        return false;
    }
    for (const std::string& text : { std::string("# Caf\xC3\xA9\n\nNa\xC3\xAFve.\n"),
             std::string("# \xE4\xB8\xAD\xE6\x96\x87\n\nText.\n") }) {
        std::vector<SourceBlock> map;
        if (!build(text, defaults, &map, mapped) || !build(text, defaults, nullptr, plain)) {
            std::cerr << "source map: " << text << " did not build" << std::endl;
            return false;
        }
        if (mapped != plain || lines(map) != Lines{ {0, 1}, {2, 3} }) {
            std::cerr << "source map: the bytes or lines of " << text << " differ" << std::endl;
            return false;
        }
    }
    std::vector<SourceBlock> map;
    if (!build("First\n\n\\pagebreak\n\nSecond\n", defaults, &map, mapped) || lines(map) != Lines{ {0, 1}, {4, 5} } ||
        map[1].page != 1 || map[1].top != 54.0f) {
        std::cerr << "source map: the block after a page break is not at the top of the next page" << std::endl;
        return false;
    }
    std::string words;
    for (int word = 0; word < 2000; word++) words += "word ";
    if (!build(words + "\n", defaults, &map, mapped) || map.size() != 1 || !(map[0].endPage > map[0].page)) {
        std::cerr << "source map: a paragraph across pages does not end on a later page" << std::endl;
        return false;
    }
    TinyPdf::PdfOptions covered;
    covered.theme.cover = true;
    if (!build(document, covered, &map, mapped) || map.size() != first.size()) {
        std::cerr << "source map: the document with a cover page differs" << std::endl;
        return false;
    }
    for (size_t k = 0; k < map.size(); k++) {
        if (map[k].firstLine != first[k].firstLine || map[k].endLine != first[k].endLine || map[k].page != first[k].page + 1 ||
            map[k].endPage != first[k].endPage + 1 || map[k].top != first[k].top || map[k].bottom != first[k].bottom) {
            std::cerr << "source map: block " << k << " is not one page on after a cover page" << std::endl;
            return false;
        }
    }
    TinyPdf::Book book;
    book.chapters.push_back({ "# One\n\nText.\n", "one.md", "", 0, false });
    book.chapters.push_back({ "# Two\n\nMore.\n", "two.md", "", 0, false });
    TinyPdf::PdfOptions bookOptions;
    map = { { 7, 8, 9, 9, 1.0f, 2.0f } };
    bookOptions.sourceMap = &map;
    if (!TinyPdf::BuildBookPdf(book, bookOptions, mapped).Ok() || map.size() != 1 || map[0].firstLine != 7 ||
        map[0].endLine != 8 || map[0].page != 9 || map[0].endPage != 9 || map[0].top != 1.0f || map[0].bottom != 2.0f) {
        std::cerr << "source map: a book changed it" << std::endl;
        return false;
    }
    return true;
}

#include "page_notes_tests.inc"

} // namespace

int main() {
    if (!CheckFormatter()) return 1;
    if (!CheckClassicMarkdownPhaseOne()) return 48;
    if (!CheckStructuredContainers()) return 49;
    if (!CheckClassicInlineCases()) return 50;
    if (!CheckClassicBlockAmbiguities()) return 51;
    if (!CheckClassicInlineExactness()) return 52;
    if (!CheckContainerPdfLayout()) return 53;
    if (!CheckImagePolicyCacheIsolation()) return 54;
    if (!CheckMathInlineSyntax()) return 55;
    if (!CheckMathBlockSyntax()) return 56;
    if (!CheckMathHostileInput()) return 57;
    if (!CheckMathPdf()) return 58;
    if (!CheckNoMathGolden()) return 59;
    if (!CheckMathParser()) return 60;
    if (!CheckMathLayout()) return 61;
    if (!CheckOutputBufferReuse()) return 62;
    if (!CheckInlineLookahead()) return 63;
    if (!CheckTallTableRows()) return 64;
    if (!CheckFootnoteDefinitions()) return 65;
    if (!CheckListNestingByContentColumn()) return 66;
    if (!CheckStandardFontEncoding()) return 67;
    if (!CheckStandardLinesFitMargin()) return 68;
    if (!CheckWordsAcrossInlineBoundaries()) return 69;
    if (!CheckLatinText()) return 70;
    if (!CheckStandardFontEmphasis()) return 71;
    if (!CheckTableCellMarkdown()) return 72;
    if (!CheckLinkText()) return 73;
    if (!CheckTableRows()) return 74;
    if (!CheckFallbackFont()) return 75;
    if (!CheckHeadingAnchors()) return 76;
    if (!CheckInvalidUtf8()) return 77;
    if (!CheckDocumentMetadata()) return 78;
    if (!CheckPageNumbers()) return 79;
    if (!CheckHtmlInText()) return 80;
    if (!CheckTaskLists()) return 81;
    if (!CheckAutolinks()) return 82;
    if (!CheckShortcutReferences()) return 83;
    if (!CheckAlerts()) return 84;
    if (!CheckHeadingKeep()) return 85;
    if (!CheckTableHeaderRepeat()) return 86;
    if (!CheckPageSize()) return 87;
    if (!CheckReportJson()) return 88;
    if (!CheckCompression()) return 89;
    if (!CheckTheme()) return 90;
    if (!CheckPdfA()) return 91;
    if (!CheckFootnotes()) return 92;
    if (!CheckCodeTiles()) return 93;
    if (!CheckHighlighting()) return 94;
    if (!CheckContents()) return 95;
    if (!CheckBook()) return 96;
    if (!CheckSvg()) return 97;
    if (!CheckTaggedPdf()) return 98;
    if (!CheckPngFilters()) return 99;
    if (!CheckWordBounds()) return 133;
    if (!CheckColorRuns()) return 134;
    if (!CheckChangedImageFiles()) return 100;
    if (!CheckCidStrings()) return 101;
    if (!CheckWriteAll()) return 102;
    if (!CheckExactMath()) return 103;
    if (!CheckComposedAdvance()) return 104;
    if (!CheckCffSubset()) return 105;
    if (!html_tests::CheckHtmlEngine()) return 106;
    if (!CheckParseLimits()) return 107;
    if (!hardening_tests::CheckHardening()) return 108;
    if (!CheckUrlAddressFilter()) return 120;
    if (!CheckThemePathPolicy()) return 121;
    if (!CheckImageFormats()) return 122;
    if (!CheckSourceMap()) return 124;
    if (!CheckHeadingNumbers()) return 125;
    if (!dialect_tests::CheckObsidian()) return 126;
    if (!mermaid_tests::CheckMermaid()) return 127;
    if (!long_page_tests::CheckLongPage()) return 129;
    if (!page_notes_tests::CheckPageNotes()) return 132;
    if (!path_tests::CheckPathLayer()) return 135;
    if (!CheckThreadExit()) return 136;
    if (!embeds_tests::CheckEmbeds()) return 130;
    if (!embeds_tests::CheckLinkedNotes()) return 131;
    if (!print_tests::CheckPrint()) return 128;
#ifdef _WIN32
    if (!CheckDirectImageLinks()) return 123;
#endif
    const std::vector<std::string> documents = {
        "# ASCII\n\nFast **native** export with a paragraph and a rule.\n\n---\n",
        u8"# Unicode\n\nZa\u017C\u00F3\u0142\u0107 g\u0119\u015Bl\u0105 ja\u017A\u0144. \u65E5\u672C\u8A9E \u0395\u03BB\u03BB\u03B7\u03BD\u03B9\u03BA\u03AC.\n",
        "# Links\n\n[one](https://example.com/one) and [two](https://example.com/two).\n",
        "# Local image\n\n![RayoMD](docs/assets/branding/rayomd.png)\n",
        "# Failed image\n\n![missing](docs/assets/branding/does-not-exist.png)\n",
        "# Mixed\n\n| Left | Right |\n|---|---:|\n| alpha | 42 |\n| wrapped cell content | 9000 |\n\n> quote\n\n- one\n- two\n",
        "# Remote fallback\n\n![blocked](https://127.0.0.1/image.png)\n",
        "# Multipage\n\n" + std::string(24000, 'x') + "\n",
        // Appended last so the index-based checks below keep their meaning.
        "# Math $E=mc^2$\n\nInline $\\frac{a}{b}$ and ($x_i$).\n\n$$\n\\sum_{i=1}^{n} i = \\frac{n(n+1)}{2}\n$$\n\n"
            "| f | v |\n|---|---|\n| $\\alpha$ | 1 |\n",
        u8"# Matematyka $\\alpha$\n\nZa\u017C\u00F3\u0142\u0107 $\\text{g\u0119\u015Bl\u0105} + x^2$.\n\n$$ a^2 + b^2 = c^2 $$\n"
    };
    std::vector<std::string> expected(documents.size());
    for (size_t i = 0; i < documents.size(); i++) {
        if (!Build(documents[i], expected[i])) {
            std::cerr << "baseline build failed for document " << i << std::endl;
            return 2;
        }
    }
    if (expected[2].find("/Subtype /Link") == std::string::npos) return 3;
    if (expected[3].find("/Subtype /Image") == std::string::npos) return 4;
    // FIPS 180-4's vectors in both implementations, and the dispatched one (the SHA extensions
    // where the CPU has them) equal to the portable one at every tail length and across blocks.
    const std::pair<const char*, const char*> vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (const auto& [message, digest] : vectors) {
        if (RayoMd::PdfSource::Sha256Hex(message) != digest || RayoMd::PdfSource::Sha256HexPortable(message) != digest) {
            std::cerr << "SHA-256 test vector failed for \"" << message << "\"" << std::endl;
            return 7;
        }
    }
    std::string shaInput;
    for (size_t length = 0; length <= (1u << 20); length = length < 300 ? length + 1 : length * 4) {
        while (shaInput.size() < length) shaInput.push_back((char)(shaInput.size() * 2654435761u >> 11));
        const std::string_view prefix(shaInput.data(), length);
        if (RayoMd::PdfSource::Sha256Hex(prefix) != RayoMd::PdfSource::Sha256HexPortable(prefix)) {
            std::cerr << "SHA-256 differs from the portable code for " << length << " bytes" << std::endl;
            return 7;
        }
    }
    const std::vector<std::string> exactSources = {
        std::string(),
        std::string("# ASCII LF\n\nplain\n"),
        std::string("# Exact\r\n\r\n<!-- hidden -->\r\n"),
        std::string("# Unicode\n\nEmoji: \xF0\x9F\x98\x80\n"),
        std::string("\xEF\xBB\xBF# BOM\n"),
        std::string("---\nprivate: true\n---\n\n:::unsupported value\n"),
        std::string(" \t\n\r\n  \n"),
        std::string("a\0b\n", 4)
    };
    for (const std::string& source : exactSources) {
        std::string reversible;
        if (!BuildReversible(source, reversible) || reversible.rfind("%PDF-2.0", 0) != 0) {
            std::cerr << "reversible build failed" << std::endl;
            return 8;
        }
        RayoMd::PdfSource::Result recovered = RayoMd::PdfSource::Inspect(reversible, true);
        if (!recovered.Ok() || recovered.source != source || !recovered.info.digestValid) {
            std::cerr << "exact recovery failed" << std::endl;
            return 9;
        }
        size_t payload = source.empty() ? std::string::npos : reversible.find(source);
        if (payload != std::string::npos) {
            reversible[payload] ^= 1;
            if (RayoMd::PdfSource::Inspect(reversible, false).status !=
                RayoMd::PdfSource::Status::IntegrityMismatch) {
                std::cerr << "tamper detection failed" << std::endl;
                return 10;
            }
        }
    }
    if (RayoMd::PdfSource::Inspect(expected.front(), false).status !=
        RayoMd::PdfSource::Status::NotReversible) return 11;
    std::string invalidUtf8(1, static_cast<char>(0xff));
    TinyPdf::PdfOptions invalidOptions;
    invalidOptions.embedSource = true;
    std::string ignoredPdf;
    if (TinyPdf::BuildPdf(invalidUtf8, invalidOptions, ignoredPdf).error !=
        TinyPdf::BuildError::InvalidSourceUtf8) return 12;

    for (TinyPdf::PdfStyle style : {TinyPdf::PdfStyle::Elegant, TinyPdf::PdfStyle::Modern, TinyPdf::PdfStyle::Tech}) {
        for (TinyPdf::PdfMargin margin : {TinyPdf::PdfMargin::Compact(), TinyPdf::PdfMargin::Normal(),
             TinyPdf::PdfMargin::Wide(), TinyPdf::PdfMargin::CustomPoints(54.0)}) {
            TinyPdf::PdfOptions options;
            options.style = style;
            options.margin = margin;
            options.embedSource = true;
            std::string source = style == TinyPdf::PdfStyle::Tech ? "# Matrix \xF0\x9F\x98\x80\n" : "# Matrix\n";
            std::string pdf;
            if (!TinyPdf::BuildPdf(source, options, pdf).Ok() ||
                RayoMd::PdfSource::Inspect(pdf, true).source != source) return 13;
        }
    }

    for (size_t i = 0; i < documents.size(); i++) {
        const std::string& source = documents[i];
        std::string pdf;
        if (!BuildReversible(source, pdf) || RayoMd::PdfSource::Inspect(pdf, true).source != source) return 14;
        if (i == 2 && pdf.find("/Subtype /Link") == std::string::npos) return 33;
        if (i == 3 && pdf.find("/Subtype /Image") == std::string::npos) return 34;
        if (i == 7 && pdf.find("/Count 1 /Kids") != std::string::npos) return 35;
    }

    const std::string fixtureSource = "# hostile fixture\n";
    std::string fixture = BuildClassicProfileFixture(fixtureSource);
    RayoMd::PdfSource::Result fixtureResult = RayoMd::PdfSource::Inspect(fixture, true);
    if (!fixtureResult.Ok() || fixtureResult.source != fixtureSource) return 15;

    FixtureOptions hostile;
    hostile.sourceExtra = " /Type /EmbeddedFile";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "duplicate key")) return 16;
    hostile = {};
    hostile.sourceExtra = " /Filter /FlateDecode";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "unsupported filter")) return 17;
    hostile.sourceExtra = " /Filter [/FlateDecode /FlateDecode]";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "repeated filters")) return 18;
    hostile = {};
    hostile.sourceLengthDelta = 1;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "bad stream length")) return 19;
    hostile = {};
    hostile.profile = "rayomd-source/2";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::UnsupportedProfile, "unsupported profile")) return 20;
    hostile = {};
    hostile.attachmentName = "../evil.md";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "path traversal name")) return 21;
    hostile.attachmentName = "C:\\\\evil.md";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "absolute name")) return 22;
    hostile.attachmentName = "\\\\server\\share\\evil.md";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "UNC name")) return 44;
    hostile.attachmentName = "\\\\?\\C:\\evil.md";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "device name")) return 45;
    hostile = {};
    hostile.duplicateNameEntry = true;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "duplicate attachment")) return 23;
    hostile = {};
    hostile.additionalNameEntry = true;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "multiple attachments")) return 46;
    hostile = {};
    hostile.sourceReference = 3;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "cyclic source reference")) return 24;
    hostile = {};
    hostile.metadataReference = 1000001;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::NotReversible, "huge object id")) return 25;
    hostile = {};
    hostile.trailerExtra = " /Prev 1";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "incremental update")) return 26;
    hostile.trailerExtra = " /XRefStm 1";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "xref stream")) return 27;
    hostile.trailerExtra = " /Encrypt 4 0 R";
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "encrypted PDF")) return 28;
    hostile = {};
    hostile.catalogExtra = " /Deep " + std::string(18, '[') + "0" + std::string(18, ']');
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "deep arrays")) return 29;
    hostile.catalogExtra = " /Deep " + std::string(17, '<') + std::string(17, '<') +
        " /Value 0 " + std::string(17, '>') + std::string(17, '>');
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::CorruptPdf, "deep dictionaries")) return 47;
    hostile = {};
    hostile.includeMetadata = false;
    if (!ExpectStatus(BuildClassicProfileFixture(fixtureSource, hostile),
        RayoMd::PdfSource::Status::NotReversible, "unrelated attachment")) return 30;
    std::string invalidFixture(1, static_cast<char>(0xff));
    if (!ExpectStatus(BuildClassicProfileFixture(invalidFixture),
        RayoMd::PdfSource::Status::InvalidUtf8, "invalid UTF-8")) return 31;
    std::string truncated = fixture.substr(0, fixture.size() - 8);
    if (!ExpectStatus(truncated, RayoMd::PdfSource::Status::CorruptPdf, "truncated PDF")) return 32;
    std::string badOffset = fixture;
    size_t freeEntry = badOffset.find("0000000000 65535 f \n");
    if (freeEntry == std::string::npos) return 36;
    badOffset[freeEntry + 20] = '9';
    if (!ExpectStatus(badOffset, RayoMd::PdfSource::Status::CorruptPdf, "bad xref offset")) return 37;
    std::string badState = fixture;
    size_t activeState = badState.find(" 00000 n \n");
    if (activeState == std::string::npos) return 38;
    badState[activeState + 7] = 'q';
    if (!ExpectStatus(badState, RayoMd::PdfSource::Status::CorruptPdf, "malformed xref entry")) return 39;
    std::string badObjectId = fixture;
    size_t firstObject = badObjectId.find("1 0 obj");
    if (firstObject == std::string::npos) return 40;
    badObjectId[firstObject] = '9';
    if (!ExpectStatus(badObjectId, RayoMd::PdfSource::Status::CorruptPdf, "object id mismatch")) return 41;
    if (!ExpectStatus(fixture + "junk", RayoMd::PdfSource::Status::CorruptPdf, "trailing bytes")) return 42;

    std::string tooLarge(RayoMd::PdfSource::kMaxSourceBytes + 1, 'x');
    TinyPdf::PdfOptions tooLargeOptions;
    tooLargeOptions.embedSource = true;
    if (TinyPdf::BuildPdf(tooLarge, tooLargeOptions, ignoredPdf).error !=
        TinyPdf::BuildError::SourceTooLarge) return 43;

    if (expected[4].find("/Subtype /Image") != std::string::npos) return 5;

    for (unsigned workerCount : {1u, 2u, 4u, 6u}) {
        std::atomic<bool> failed{false};
        std::vector<std::thread> workers;
        for (unsigned worker = 0; worker < workerCount; worker++) {
            workers.emplace_back([&, worker] {
                for (unsigned iteration = 0; iteration < 20 && !failed.load(); iteration++) {
                    size_t index = (worker + iteration) % documents.size();
                    std::string pdf;
                    if (!Build(documents[index], pdf) || pdf != expected[index]) {
                        failed.store(true);
                        break;
                    }
                }
            });
        }
        for (std::thread& worker : workers) worker.join();
        if (failed.load()) {
            std::cerr << "concurrency stress failed with " << workerCount << " worker(s)" << std::endl;
            return 6;
        }
    }
    std::cout << "RayoMD formatter, reversible-profile hardening, and 1/2/4/6-worker concurrency stress passed"
              << std::endl;
    return 0;
}
