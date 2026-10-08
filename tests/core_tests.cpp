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
        {12.30, "12.3"}, {12.345, "12.35"}, {595.0, "595"}, {0.004, "0"}
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
        blocks[5].imageSrc != "docs/assets/branding/rayomd.png") {
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
    const std::string cellWord(56, 'w');
    const std::string headingWord(120, 'h');
    std::string longWordPdf;
    if (!Build("| a | b | c | d | e | f |\n|---|---|---|---|---|---|\n| $x$" + cellWord + " | 2 | 3 | 4 | 5 | 6 |\n\n"
            "# Heading $x$" + headingWord + "\n", longWordPdf) ||
        longWordPdf.find(cellWord) != std::string::npos || longWordPdf.find(headingWord) != std::string::npos ||
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
// rendered byte-for-byte as before. The digests were recorded from the last release
// without native math (2.6.0, commit 0d4c53a) and are identical on Windows and Linux.
bool CheckNoMathGolden() {
    struct Golden { TinyPdf::PdfStyle style; size_t size; const char* sha256; };
    const Golden goldens[] = {
        {TinyPdf::PdfStyle::Modern, 6065, "1633be2ad219bc569e4892542531dc8512f0f7e5f0001261a2f7e54443ed4b94"},
        {TinyPdf::PdfStyle::Tech, 6059, "e2967c50a305de1ce2102ed713ceebff196a576563b5c83c8bafeb3fbdd9b60f"},
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
