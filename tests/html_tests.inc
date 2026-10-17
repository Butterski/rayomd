// SPDX-License-Identifier: Apache-2.0

// Needs <chrono>, <iostream>, <string>, <string_view>, <vector> and src/core/html.h.
// The HTML engine: tag names, CommonMark's raw HTML tags (spec 0.31.2, examples 613-632) and
// HTML block conditions (section 4.6), the limits of a tag, and which URLs a link may have.
// Included inside the anonymous namespace of tests/core_tests.cpp.

namespace html_tests {

using namespace TinyPdf::Internal::Html;

// The tags of `text` as the inline parser meets them: at each '<', a tag is taken whole.
std::vector<std::string> TagsOf(std::string_view text) {
    std::vector<std::string> tags;
    QuoteCache quotes;
    TagToken token;
    for (size_t at = text.find('<'); at != std::string_view::npos; at = text.find('<', at + 1)) {
        if (const uint32_t length = ScanTag(text, at, quotes, token)) {
            tags.emplace_back(text.substr(at, length));
            at += length - 1;
        }
    }
    return tags;
}

bool CheckTagNames() {
    for (size_t index = 1; index < static_cast<size_t>(Tag::Count); index++) {
        const Tag tag = static_cast<Tag>(index);
        std::string upper(TagName(tag));
        for (char& c : upper) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        if (TagName(tag).empty() || LookupTag(TagName(tag)) != tag || LookupTag(upper) != tag) {
            std::cerr << "HTML tag lookup mismatch: " << TagName(tag) << std::endl;
            return false;
        }
    }
    for (std::string_view name : { "", "x-foo", "blockquotes", "abcdefghijk", "h7", "foo", "spa n" }) {
        if (LookupTag(name) != Tag::Unknown) {
            std::cerr << "HTML tag lookup found an unknown name: " << name << std::endl;
            return false;
        }
    }
    if (!IsBlockName(Tag::Div) || !IsBlockName(Tag::Search) || !IsBlockName(Tag::H6) || IsBlockName(Tag::Span) ||
        IsBlockName(Tag::Pre) || IsBlockName(Tag::Script) || !IsDroppedWithContent(Tag::Script) ||
        !IsDroppedWithContent(Tag::Title) || !IsDroppedWithContent(Tag::Head) || IsDroppedWithContent(Tag::Div)) {
        std::cerr << "HTML tag classes mismatch" << std::endl;
        return false;
    }
    return true;
}

bool CheckRawHtml() {
    struct Example { int number; std::string_view markdown; std::vector<std::string> tags; };
    const Example examples[] = {
        { 613, "<a><bab><c2c>\n", { "<a>", "<bab>", "<c2c>" } },
        { 614, "<a/><b2/>\n", { "<a/>", "<b2/>" } },
        { 615, "<a  /><b2\ndata=\"foo\" >\n", { "<a  />", "<b2\ndata=\"foo\" >" } },
        { 616, "<a foo=\"bar\" bam = 'baz <em>\"</em>'\n_boolean zoop:33=zoop:33 />\n",
            { "<a foo=\"bar\" bam = 'baz <em>\"</em>'\n_boolean zoop:33=zoop:33 />" } },
        { 617, "Foo <responsive-image src=\"foo.jpg\" />\n", { "<responsive-image src=\"foo.jpg\" />" } },
        { 618, "<33> <__>\n", {} },
        { 619, "<a h*#ref=\"hi\">\n", {} },
        { 620, "<a href=\"hi'> <a href=hi'>\n", {} },
        { 621, "< a><\nfoo><bar/ >\n<foo bar=baz\nbim!bop />\n", {} },
        { 622, "<a href='bar'title=title>\n", {} },
        { 623, "</a></foo >\n", { "</a>", "</foo >" } },
        { 624, "</a href=\"foo\">\n", {} },
        { 630, "foo <a href=\"&ouml;\">\n", { "<a href=\"&ouml;\">" } },
        { 631, "foo <a href=\"\\*\">\n", { "<a href=\"\\*\">" } },
        { 632, "<a href=\"\\\"\">\n", {} },
    };
    for (const Example& example : examples) {
        if (TagsOf(example.markdown) != example.tags) {
            std::cerr << "raw HTML mismatch in CommonMark example " << example.number << std::endl;
            return false;
        }
    }
    // The attributes RayoMD reads, in any case, unquoted or quoted; the first of a repeated one.
    QuoteCache quotes;
    TagToken img;
    TagToken a;
    TagToken details;
    TagToken br;
    TagToken close;
    const std::string_view image = "<IMG SRC=\"a.png\" alt='A \"b\"' Width=100 height = \"50\" src=b.png>";
    const std::string_view link = "<a href=x id=top name=\"n\" align=center start=3>";
    if (ScanTag(image, 0, quotes, img) != image.size() || img.tag != Tag::Img || img.src != "a.png" ||
        img.alt != "A \"b\"" || img.width != "100" || img.height != "50" || img.closing || img.selfClosing ||
        ScanTag(link, 0, quotes, a) != link.size() || a.tag != Tag::A || a.href != "x" || a.id != "top" || a.name != "n" ||
        a.align != "center" || a.start != "3" || a.hasOpen ||
        ScanTag("<details open>", 0, quotes, details) != 14 || !details.hasOpen || !details.open.empty() ||
        ScanTag("<br/>", 0, quotes, br) != 5 || !br.selfClosing || br.tag != Tag::Br ||
        ScanTag("</DIV >", 0, quotes, close) != 7 || !close.closing || close.tag != Tag::Div ||
        ScanTag("x<b>", 0, quotes, close) != 0) {
        std::cerr << "HTML tag attributes mismatch" << std::endl;
        return false;
    }
    return true;
}

bool CheckHtmlBlockConditions() {
    struct Start { std::string_view line; int condition; };
    const Start starts[] = {
        { "<pre language=\"haskell\"><code>", 1 }, { "<script type=\"text/javascript\">", 1 }, { "<style", 1 },
        { "<TEXTAREA>", 1 }, { "<pre>", 1 }, { "<prefix>", 7 }, { "<!-- Foo", 2 }, { "<!-->", 2 }, { "<?php", 3 },
        { "<!DOCTYPE html>", 4 }, { "<![CDATA[", 5 }, { "<table><tr><td>", 6 }, { "<div>", 6 }, { "</div>", 6 },
        { "<DIV CLASS=\"foo\">", 6 }, { "<div id=\"foo\"", 6 }, { "<div *?\?\?-&&&-<---", 6 }, { "<td>", 6 },
        { "<hr/>", 6 }, { "<search>", 6 }, { "<div2>", 7 }, { "<div-x>", 7 }, { "<a href=\"foo\">", 7 },
        { "<Warning>", 7 }, { "<i class=\"foo\">", 7 }, { "</ins>", 7 }, { "<del>  ", 7 }, { "</pre>", 7 },
        { "<del>*foo*</del>", 0 }, { "<a href=\"foo", 0 }, { "<foo", 0 }, { "<abc>x", 0 }, { "< div>", 0 },
        { "<", 0 }, { "text", 0 }, { "<!x", 4 }, { "<!1", 0 },
    };
    for (const Start& start : starts) {
        if (HtmlBlockStart(start.line) != start.condition) {
            std::cerr << "HTML block start mismatch: " << start.line << " gives " << HtmlBlockStart(start.line) << std::endl;
            return false;
        }
    }
    struct End { std::string_view line; int condition; bool ends; };
    const End ends[] = {
        { "</pre>", 1, true }, { "foo </SCRIPT> bar", 1, true }, { "</style >", 1, false }, { "</textarea>", 1, true },
        { "</div>", 1, false }, { "-->", 2, true }, { "<!-- x", 2, false }, { "a ?> b", 3, true }, { ">", 4, true },
        { "]]>", 5, true }, { "]]", 5, false }, { "", 6, true }, { " \t", 7, true }, { "x", 6, false }, { " x", 7, false },
    };
    for (const End& end : ends) {
        if (HtmlBlockEnds(end.line, end.condition) != end.ends) {
            std::cerr << "HTML block end mismatch: " << end.line << " (" << end.condition << ")" << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckTagLimits() {
    QuoteCache quotes;
    TagToken token;
    // A tag of exactly kMaxTagBytes is read; one byte more, and 65 attributes, are not.
    const std::string longest = "<a x=\"" + std::string(kMaxTagBytes - 8, 'y') + "\">";
    const std::string longer = "<a x=\"" + std::string(kMaxTagBytes - 7, 'y') + "\">";
    std::string attributes64 = "<a";
    for (int index = 0; index < 64; index++) attributes64 += " b" + std::to_string(index);
    const std::string attributes65 = attributes64 + " c>";
    attributes64 += ">";
    if (longest.size() != kMaxTagBytes || ScanTag(longest, 0, quotes, token) != kMaxTagBytes ||
        ScanTag(longer, 0, quotes, token) != 0 ||
        ScanTag(attributes64, 0, quotes, token) != attributes64.size() || ScanTag(attributes65, 0, quotes, token) != 0) {
        std::cerr << "HTML tag limits mismatch" << std::endl;
        return false;
    }
    // Unterminated quotes, and a tag that begins in the quoted value of one that failed, whose
    // quote lies before the last search: linear, and the answer that search would give.
    std::string unterminated;
    for (int index = 0; index < 100000; index++) unterminated += "<a x=\"";
    const auto started = std::chrono::steady_clock::now();
    const size_t found = TagsOf(unterminated).size();
    const auto took = std::chrono::steady_clock::now() - started;
    if (found != 0 || took > std::chrono::seconds(2) || TagsOf("<a x=\"<b y=\" z=\">\"!") !=
            std::vector<std::string>{ "<b y=\" z=\">" }) {
        std::cerr << "HTML quote scanning mismatch" << std::endl;
        return false;
    }
    return true;
}

bool CheckSafeUrls() {
    for (std::string_view unsafe : { "javascript:alert(1)", " JavaScript:x", "java\tscript:x", "\x01javascript:x",
             "VBScript:x", "data:text/html,x", "data:image/png;base64,x", "file:///etc/passwd", "FILE://host/x",
             "\\\\host\\share", "\\/host", "/\\host", " \\\\host", "C:\\Windows\\x.exe", "c:/x", "\x7fjavascript:x",
             "java\x01script:x", "fi\x1fle:///x", "c\x7f:/x", "C\x02:\\x", "\\\x01\\host", "d\x7f" "ata:text/html,x" }) {
        if (SafeUrl(unsafe)) {
            std::cerr << "unsafe URL allowed: " << unsafe << std::endl;
            return false;
        }
    }
    for (std::string_view safe : { "https://example.com", "#top", "mailto:a@b.c", "javascriptish", "docs/a.md", "",
             "//example.com/x", "/docs/a.md", "filename.md", "c:x", "ab:/x" }) {
        if (!SafeUrl(safe)) {
            std::cerr << "safe URL refused: " << safe << std::endl;
            return false;
        }
    }
    if (!SafeUrl("data:image/png;base64,x", true) || SafeUrl("data:text/plain,x", true) || SafeUrl("javascript:x", true) ||
        !SafeUrl("file.png", true)) {
        std::cerr << "image URL safety mismatch" << std::endl;
        return false;
    }
    return true;
}

// Inline HTML as GitHub shows it: each kind of element's effect on the runs, an element left
// open lasting to the end of its paragraph, a script element's content left out, an unsafe link
// kept as text, and <sup> and <sub> as runs of their own; in both renderers, the fill of <mark>,
// and scripts at the size of a note's number.
bool CheckInlineHtml() {
    using TinyPdf::Internal::InlineMath;
    using TinyPdf::Internal::InlineSpan;
    const std::vector<InlineSpan> spans = TinyPdf::Internal::ParseInlineSpans(
        "a <b>b</b> <i>c</i> <s>d</s> <kbd>e</kbd> <u>f</u> <mark>g</mark> <a href=\"https://x.y/&amp;\">h</a> "
        "x<sup>2</sup> H<sub>2</sub>O <q>q</q> <img src=a.png alt=\"Logo\"> <span>s</span> "
        "<script>bad()</script><a href=\"javascript:x\">j</a> <STRONG>end");
    std::string visible;
    for (const InlineSpan& span : spans) visible += span.text;
    const auto find = [&spans](std::string_view text, InlineMath math = InlineMath::None) -> const InlineSpan* {
        for (const InlineSpan& span : spans) {
            if (span.text == text && span.math == math) return &span;
        }
        return nullptr;
    };
    const InlineSpan* b = find("b");
    const InlineSpan* c = find("c");
    const InlineSpan* d = find("d");
    const InlineSpan* e = find("e");
    const InlineSpan* f = find("f");
    const InlineSpan* g = find("g");
    const InlineSpan* h = find("h");
    const InlineSpan* j = nullptr;     // plain text, joined with the text around it
    for (const InlineSpan& span : spans) {
        if (span.text.find(" j ") != std::string::npos) j = &span;
    }
    const InlineSpan* end = find("end");
    if (visible != "a b c d e f g h x2 H2O \xE2\x80\x9Cq\xE2\x80\x9D image: Logo s j end" || !b || !b->bold || !c ||
        !c->italic || c->bold || !d || !d->strike || !e || !e->code || !f || f->decor != TinyPdf::Internal::kDecorUnderline ||
        !g || g->decor != TinyPdf::Internal::kDecorMark || !h || h->url != "https://x.y/&" ||
        !find("2", InlineMath::Sup) || !find("2", InlineMath::Sub) || !j || !j->url.empty() || !end || !end->bold) {
        std::cerr << "inline HTML spans mismatch: " << visible << std::endl;
        return false;
    }
    const std::string text = "A <mark>m</mark>, <u>u</u> and x<sup>2</sup>.\n";
    for (const std::string& document : { text, "\xC5\xBC " + text }) {
        std::string pdf;
        if (!Build(document, pdf) || pdf.find("1 0.973 0.773 rg") == std::string::npos ||
            pdf.find(" 8.05 Tf") == std::string::npos || pdf.find("<sup>") != std::string::npos) {
            std::cerr << "inline HTML painting mismatch" << std::endl;
            return false;
        }
    }
    return true;
}

// HTML blocks as ParseMarkdown maps them: an element left open aligning the Markdown after it,
// anchors, details and summary, nested lists with a start, a table with its header alignment,
// <pre>, a quote, headings with ids, sized and linked images in rows, elements dropped with their
// content, Markdown syntax shown as text, and start condition 7 not interrupting a paragraph.
bool CheckHtmlBlocks() {
    using TinyPdf::Internal::Block;
    using TinyPdf::Internal::BlockType;
    using TinyPdf::Internal::ParseMarkdown;
    const auto fail = [](const char* what) {
        std::cerr << "HTML block mismatch: " << what << std::endl;
        return false;
    };
    std::vector<Block> blocks = ParseMarkdown("<div align=\"center\">\n\n# Title\n\nText\n\n</div>\n\nAfter\n");
    if (blocks.size() != 3 || blocks[0].type != BlockType::Heading || blocks[0].align != 2 || blocks[1].text != "Text" ||
        blocks[1].align != 2 || blocks[2].text != "After" || blocks[2].align != 0) {
        return fail("open div");
    }
    blocks = ParseMarkdown("<p align=\"right\"><a href=\"#top\">back</a></p>\n");
    if (blocks.size() != 1 || blocks[0].type != BlockType::Paragraph || blocks[0].align != 3 ||
        blocks[0].text != "<a href=\"#top\">back</a>") {
        return fail("aligned paragraph");
    }
    blocks = ParseMarkdown("<a id=\"top\"></a>\n\n# H\n\n<a name=\"low\"></a>\n\n<h2 id=\"sec\">Section *x*</h2>\n");
    if (blocks.size() != 5 || blocks[0].type != BlockType::Anchor || blocks[0].text != "top" ||
        blocks[2].type != BlockType::Anchor || blocks[2].text != "low" || blocks[3].type != BlockType::Anchor ||
        blocks[3].text != "sec" || blocks[4].type != BlockType::Heading || blocks[4].level != 2 ||
        blocks[4].text != "Section *x*") {
        return fail("anchors and headings");
    }
    blocks = ParseMarkdown("<details open>\n<summary>More</summary>\n\nHidden **md**\n\n</details>\n");
    if (blocks.size() != 2 || blocks[0].text != "<b>More" || blocks[1].text != "Hidden **md**") return fail("details");
    blocks = ParseMarkdown("<ul>\n<li>one</li>\n<li>two\n<ol start=\"3\"><li>a</li><li>b</li></ol></li>\n</ul>\n");
    if (blocks.size() != 2 || blocks[0].type != BlockType::Bullet || blocks[0].text != "one" || blocks[1].text != "two" ||
        blocks[1].children.size() != 2 || blocks[1].children[0].type != BlockType::Numbered ||
        blocks[1].children[0].number != 3 || blocks[1].children[1].number != 4 || blocks[1].children[1].text != "b") {
        return fail("lists");
    }
    blocks = ParseMarkdown("<table>\n<tr><th align=\"center\">A</th><th>B</th></tr>\n<tr><td><b>1</b></td></tr>\n</table>\n");
    if (blocks.size() != 1 || blocks[0].type != BlockType::Table || blocks[0].rows.size() != 2 ||
        blocks[0].rows[0] != std::vector<std::string>{ "A", "B" } || blocks[0].rows[1] != std::vector<std::string>{ "<b>1</b>", "" } ||
        blocks[0].aligns != std::vector<int>{ 0, -1 }) {
        return fail("table");
    }
    blocks = ParseMarkdown("<table><tr><td><p>a</p><p>b</p></td></tr></table>\n");
    if (blocks.size() != 2 || blocks[0].text != "a" || blocks[1].text != "b") return fail("table of blocks");
    blocks = ParseMarkdown("<pre>\n  x &lt; y\n<b>z</b>\n</pre>\n\n<blockquote>\n<p>quoted</p>\n</blockquote>\n\n<hr>\n");
    if (blocks.size() != 3 || blocks[0].type != BlockType::Code || blocks[0].text != "  x < y\nz" ||
        blocks[1].type != BlockType::Quote || blocks[1].children.size() != 1 || blocks[1].children[0].text != "quoted" ||
        blocks[2].type != BlockType::Rule) {
        return fail("pre, blockquote, hr");
    }
    blocks = ParseMarkdown("<p>\n<img src=\"a.png\" width=\"170\" alt=\"A\">\n</p>\n\n"
        "[![a](a.png)](https://x.y) <a href=\"https://b.c\"><img src=\"b.png\" alt=\"B\" width=\"50%\"></a><br> ![c](c.png)\n");
    if (blocks.size() != 2 || blocks[0].type != BlockType::Image || blocks[0].text != "A" || !blocks[0].image ||
        blocks[0].image->src != "a.png" || blocks[0].image->widthHint != 127.5f || !blocks[0].image->row.empty() ||
        blocks[1].type != BlockType::Image || blocks[1].image->link != "https://x.y" || blocks[1].image->row.size() != 2 ||
        blocks[1].image->row[0].src != "b.png" || blocks[1].image->row[0].link != "https://b.c" ||
        !blocks[1].image->row[0].widthPercent || blocks[1].image->row[0].widthHint != 0.5f ||
        blocks[1].image->row[0].alt != "B" || blocks[1].image->row[1].src != "c.png") {
        return fail("images");
    }
    blocks = ParseMarkdown("<div>\n<a id=\"row\"></a> <img src=\"a.png\"> <img src=\"b.png\">\n</div>\n");
    if (blocks.size() != 2 || blocks[0].type != BlockType::Anchor || blocks[0].text != "row" ||
        blocks[1].type != BlockType::Image || blocks[1].image->row.size() != 1) {
        return fail("an anchor before images");
    }
    blocks = ParseMarkdown("<script>\nalert(1)\n</script>\n\n<div>\na*b*c $x$ ~~s~~ [l](u)\n</div>\n\ntext\n<span>\nmore\n");
    if (blocks.size() != 2 || blocks[1].text != "text <span> more") return fail("dropped content and condition 7");
    const std::vector<TinyPdf::Internal::InlineSpan> spans = TinyPdf::Internal::ParseInlineSpans(blocks[0].text);
    if (spans.size() != 1 || spans[0].text != "a*b*c $x$ ~~s~~ [l](u)" || spans[0].bold || spans[0].italic || spans[0].strike) {
        return fail("Markdown inside HTML");
    }
    // An id is a link target, the name exactly as written; a link to an id no element has is dropped.
    std::string pdf;
    if (!Build("<a id=\"Target-X\"></a>\n\n# Title\n\n\\pagebreak\n\n[to](#Target-X) and [gone](#nope).\n", pdf) ||
        CountOccurrences(pdf, "/Subtype /Link") != 1 || pdf.find("/Dest [") == std::string::npos) {
        return fail("links to ids");
    }
    return true;
}

// What HTML blocks change on the page, in both renderers: the line of a right-aligned paragraph
// ends at the right margin with its link, a width hint sizes an image a centred element centres,
// and a paragraph of images is a row on one bottom, left to right.
bool CheckHtmlRendering() {
    // The number after the first `prefix` in the PDF.
    const auto number = [](const std::string& pdf, const char* prefix, double& value) {
        const size_t at = pdf.find(prefix);
        return at != std::string::npos && std::sscanf(pdf.c_str() + at + std::strlen(prefix), "%lf", &value) == 1;
    };
    TinyPdf::PdfOptions options;
    options.style = TinyPdf::PdfStyle::Modern;
    options.margin = TinyPdf::PdfMargin::Normal();
    options.sourcePath = std::string(RAYOMD_TEST_SOURCE_DIR) + "/tester.md";
    for (const char* text : { "Right", "Right \xE2\x86\x92" }) {   // the standard and the Unicode renderer
        const std::string document = std::string("<p align=\"right\">") + text + " <a href=\"https://e.com\">link</a></p>\n\n"
            "<div align=\"center\">\n<img src=\"docs/assets/branding/rayomd.png\" width=\"170\" alt=\"M\">\n</div>\n\n"
            "<img src=\"docs/assets/branding/rayomd.png\" width=\"40\"> <img src=\"docs/assets/branding/rayomd.png\" height=\"40\">\n";
        std::string pdf;
        double right = 0.0, first = 0.0, second = 0.0;
        const bool unicode = std::strlen(text) > 5;
        if (!TinyPdf::BuildPdf(document, options, pdf).Ok() || unicode != (pdf.find("/Type0") != std::string::npos) ||
            !number(pdf, "/Rect [", right) || std::sscanf(pdf.c_str() + pdf.find("/Rect [") + 7, "%*f %*f %lf", &right) != 1 ||
            right < 540.99 || right > 541.01 || pdf.find("q 127.5 0 0 127.5 233.75 ") == std::string::npos ||
            !number(pdf, "q 30 0 0 30 54 ", first) || !number(pdf, "q 30 0 0 30 90 ", second) || first != second) {
            std::cerr << "HTML rendering mismatch: " << text << std::endl;
            return false;
        }
    }
    return true;
}

bool CheckHtmlEngine() {
    return CheckTagNames() && CheckRawHtml() && CheckHtmlBlockConditions() && CheckTagLimits() && CheckSafeUrls() &&
        CheckInlineHtml() && CheckHtmlBlocks() && CheckHtmlRendering();
}

} // namespace html_tests
