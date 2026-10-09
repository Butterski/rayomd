#include "rayomd/tiny_pdf.h"
#include "../src/common/batch_report.h"
#include "../src/core/flate.h"
#include "../src/common/text_utils.h"
#include "../src/core/export_options.h"
#include "../src/core/inline_markdown.h"
#include "../src/core/markdown_parser.h"
#include "../src/core/math_layout.h"
#include "../src/core/math_parser.h"
#include "../src/core/rayomd_pdf_source.h"
#include "../src/core/pdfa.h"
#include "../src/core/highlight.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef RAYOMD_USE_ZLIB
#include <zlib.h>
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
    for (const InlineSpan& span : spans) if (!span.url.empty()) urls.push_back(span.url);
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
        if (!span.url.empty()) urls.push_back(span.url);
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

    const std::string invalidAutolinks = "<not-an-email> and <user@localhost>";
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

// ---- Native math -------------------------------------------------------------------

#include "no_math_golden.inc"
#include "math_markdown_tests.inc"
#include "math_parser_tests.inc"
#include "math_layout_tests.inc"
#include "inline_lookahead_tests.inc"

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
    const char* const mathFonts[] = {
        "/BaseFont /Times-Roman >>", "/BaseFont /Times-Italic >>", "/BaseFont /Times-Bold >>",
        "/BaseFont /Times-BoldItalic >>",
        "/BaseFont /Symbol /FirstChar 32 /LastChar 254 /Widths [250 333 713 ",
        "/Type /FontDescriptor /FontName /Symbol /Flags 4 "
    };
    const std::string mathBody =
        "# Energy $E=mc^2$\n\n"
        "Inline $a^2+b^2=c^2$ and a tall $\\frac{\\frac{a}{b}}{\\frac{c}{d}}$ fraction.\n\n"
        "$$\n\\int_{-\\infty}^{\\infty} e^{-x^2}\\,dx = \\sqrt{\\pi}\n$$\n\n"
        "- item $x_i$\n\n> quote $q$\n\n"
        "| f | v |\n|---|---|\n| $\\alpha$ | $\\frac{a}{b}$ |\n";

    // An ASCII document with math stays on the standard-font path and gains exactly the
    // six math font objects; a document without math gains nothing.
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
    for (const char* font : mathFonts) {
        if (CountOccurrences(asciiPdf, font) != 1 || CountOccurrences(unicodePdf, font) != 1 ||
            plainPdf.find(font) != std::string::npos) {
            std::cerr << "math font object mismatch: " << font << std::endl;
            return false;
        }
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
// WinAnsiEncoding, CommonMark list nesting, source-faithful word spacing and code tiles that
// no longer cover the descenders and underscores of the line above. That renderer does not
// depend on the platform or on installed fonts, so they hold on Windows and Linux.
bool CheckNoMathGolden() {
    struct Golden { TinyPdf::PdfStyle style; size_t size; const char* sha256; };
    const Golden goldens[] = {
        {TinyPdf::PdfStyle::Modern, 7355, "d19e491636cc75bc83d5079a4878ae1220690aa1ff5c6084708eb1aa409e9e22"},
        {TinyPdf::PdfStyle::Tech, 7409, "8a61b40ae384e02a880a7b2f68290252e6ed9eaaf6f3c6dad42f45f6ebc7d427"},
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
// U+4E2D, 1000 units per em and the bounding box [0 0 700 700].
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
    std::string loca;
    for (uint32_t offset : {0u, 0u, 0u, 34u, 68u, 102u}) u32(loca, offset);  // 34 bytes a square; 0 and 1 empty
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
    for (int value : {800, -200, 0, 800, 0, 0, 700, 1, 0, 0, 0, 0, 0, 0, 0, 5}) u16(hhea, value);
    std::string maxp;
    u32(maxp, 0x00010000);
    for (int value : {5, 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0}) u16(maxp, value);
    std::string hmtx;
    for (int glyph = 0; glyph < 5; glyph++) {
        u16(hmtx, 800);
        u16(hmtx, 0);
    }
    std::string cmap;
    for (int value : {0, 1, 3, 1}) u16(cmap, value);
    u32(cmap, 12);
    const int codes[] = {0x20, 0x3F, 0x41, 0x4E2D, 0xFFFF};
    const int glyphs[] = {1, 3, 2, 4, 0};
    // Format 4: length, language, two bytes per segment, search range, selector, range shift.
    for (int value : {4, 14 + 2 + 5 * 8, 0, 10, 8, 2, 2}) u16(cmap, value);
    for (int code : codes) u16(cmap, code);                             // end codes
    u16(cmap, 0);
    for (int code : codes) u16(cmap, code);                             // start codes
    for (int index = 0; index < 5; index++) u16(cmap, (glyphs[index] - codes[index]) & 0xFFFF);
    for (int index = 0; index < 5; index++) u16(cmap, 0);

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

#ifdef RAYOMD_USE_ZLIB
bool Inflates(std::string_view packed, const std::string& expected) {
    std::string plain(expected.size() + 1, '\0');
    uLongf size = plain.size();
    return uncompress(reinterpret_cast<Bytef*>(&plain[0]), &size, reinterpret_cast<const Bytef*>(packed.data()),
               packed.size()) == Z_OK && size == expected.size() && plain.compare(0, size, expected) == 0;
}
#endif

// --compress: every content stream, font program and CMap is FlateDecode where that makes it
// smaller and inflates to what the uncompressed PDF has, the font keeping its own size as
// /Length1; a stream compression cannot shrink stays as it is. Output does not depend on what
// the thread compressed before, nor on other threads compressing at the same time, and the
// reversible profile still recovers its source. In both renderers.
bool CheckCompression() {
#ifdef RAYOMD_USE_ZLIB
    // The encoder alone, at the edges of its blocks, window and match lengths, on runs and on
    // data it cannot compress, within the size it promises.
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
    for (const std::string& input : inputs) {
        std::string packed(RayoMd::Flate::ZlibBound(input.size()), '\0');
        packed.resize(RayoMd::Flate::ZlibCompress(input, reinterpret_cast<uint8_t*>(&packed[0]), scratch));
        if (packed.size() > RayoMd::Flate::ZlibBound(input.size()) - 8 || !Inflates(packed, input)) {
            std::cerr << "DEFLATE round trip failed for " << input.size() << " bytes" << std::endl;
            return false;
        }
    }
#endif
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
#ifdef RAYOMD_USE_ZLIB
            ok = ok && Inflates(payload, plainStreams[i].second);
#endif
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
    if (!result.Ok() || result.pages < 2 || pdf.rfind("%PDF-1.7\n", 0) != 0 || pdf.find("/Subtype /Type1") != npos ||
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

// The text of the run strings of the first line drawn in `color`: the literal strings "(...)"
// (standard renderer) or hex strings "<...>" of CIDs, which are code points (Unicode renderer), up
// to the end of its text object.
std::string CodeLineText(const std::string& pdf, std::string_view color) {
    std::string text;
    const size_t start = pdf.find(std::string(color) + " rg BT");
    const size_t end = start == std::string::npos ? start : pdf.find(" ET", start);
    for (size_t at = start; end != std::string::npos && at < end; at++) {
        if (pdf[at] == '(') {
            for (at++; pdf[at] != ')'; at++) text.push_back(pdf[at] == '\\' ? pdf[++at] : pdf[at]);
        } else if (pdf[at] == '<') {
            for (at++; pdf[at] != '>'; at += 4) text.push_back((char)std::stoi(pdf.substr(at, 4), nullptr, 16));
        }
    }
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
            CodeLineText(colored, ".812 .133 .18") != "def f(x): return 'a'  # note" ||
            plain.find(".812 .133 .18 rg") != std::string::npos ||
            CodeLineText(plain, "0.12 0.12 0.12") != "def f(x): return 'a'  # note") {
            std::cerr << "highlighted code mismatch (" << (text == block ? "standard" : "Unicode") << " renderer)" << std::endl;
            return false;
        }
    }
    return true;
}

// A list item that starts with "[ ]" or "[x]" shows a checkbox, with a check mark when done,
// where its bullet or number would be, and its text without the marker. "[ ]" elsewhere, and
// a task item in a quote, keep it as text. In both renderers.
bool CheckTaskLists() {
    const std::string document = "- [ ] open\n- [x] done\n1. [X] numbered\n- normal\n\nText [ ] literal.\n\n> - [ ] quoted\n";
    for (const std::string& text : { document, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n\n" + document }) {
        std::string pdf;
        const bool unicode = text != document;
        if (!Build(text, pdf) || CountOccurrences(pdf, "0.42 0.47 0.53 RG") != 3 || CountOccurrences(pdf, " 1 J 1 j ") != 2 ||
            (!unicode && (pdf.find("(open) Tj") == std::string::npos || pdf.find("(- normal) Tj") == std::string::npos ||
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
    if (!Build(document, pdf) || pdf.find("0077006F0072006C0064") == std::string::npos ||   // "world"
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
    if (RayoMd::PdfSource::Sha256Hex("abc") !=
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        std::cerr << "SHA-256 test vector failed" << std::endl;
        return 7;
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
