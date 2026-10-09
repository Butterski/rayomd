#include "rayomd/tiny_pdf.h"
#include "../src/common/text_utils.h"
#include "../src/core/inline_markdown.h"
#include "../src/core/markdown_parser.h"
#include "../src/core/math_layout.h"
#include "../src/core/math_parser.h"
#include "../src/core/rayomd_pdf_source.h"

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
// rendered byte-for-byte the same with and without it. The digests were recorded when
// headings started to make the PDF outline, after code backgrounds started at the first
// glyph of the code, the standard-font renderer started to show bold, italic and
// strike-through, exact AFM widths, WinAnsiEncoding, CommonMark list nesting and
// source-faithful word spacing. That renderer does not depend on the platform or on
// installed fonts, so they hold on Windows and Linux alike.
bool CheckNoMathGolden() {
    struct Golden { TinyPdf::PdfStyle style; size_t size; const char* sha256; };
    const Golden goldens[] = {
        {TinyPdf::PdfStyle::Modern, 7334, "37f7d35459892732bf96e34f3ba21c1ba46d4483231e2108d846c422e2358044"},
        {TinyPdf::PdfStyle::Tech, 7388, "57d73415b6b112cb6c5fc67c630bde61030ff24c610489470059f21ee7324517"},
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

// "[^1]: text" is a footnote definition and stays visible text; only "[label]: target" is
// a link reference definition, which disappears.
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
