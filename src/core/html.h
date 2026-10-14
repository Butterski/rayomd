#pragma once

// HTML inside Markdown: where CommonMark 0.31.2 recognises it (raw HTML tags in text, the seven
// kinds of HTML block) and which elements RayoMD knows, GitHub's sanitizer allowlist with `u`
// and `center`, CommonMark's block names, and the elements dropped with their content. Every
// scan is bounded: a tag is at most kMaxTagBytes long with kMaxTagAttributes attributes, and
// QuoteCache answers "where is the next quote" for forward queries in amortized constant time,
// so that text with many unterminated quotes stays linear. Kept out of tiny_pdf.cpp and the
// parser's translation units, which sit at GCC's inlining limit.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {
struct Block;
}

namespace TinyPdf::Internal::Html {

enum class Tag : uint8_t {
    Unknown,
    // GitHub's allowlist (html-pipeline's SanitizationFilter).
    H1, H2, H3, H4, H5, H6, Br, B, I, Strong, Em, A, Pre, Code, Img, Tt, Div, Ins, Del, Sup, Sub,
    P, Picture, Ol, Ul, Table, Thead, Tbody, Tfoot, Blockquote, Dl, Dt, Dd, Kbd, Q, Samp, Var, Hr,
    Ruby, Rt, Rp, Li, Tr, Td, Th, S, Strike, Summary, Details, Caption, Figure, Figcaption, Abbr,
    Bdo, Cite, Dfn, Mark, Small, Source, Span, Time, Wbr,
    // Shown by GitHub through other means, kept here.
    U, Center,
    // The other names of CommonMark's block start condition 6.
    Address, Article, Aside, Base, Basefont, Body, Col, Colgroup, Dialog, Dir, Fieldset, Footer,
    Form, Frame, Frameset, Head, Header, Html, Iframe, Legend, Link, Main, Menu, Menuitem, Nav,
    Noframes, Optgroup, Option, Param, Search, Section, Title, Track,
    // Dropped with their content (Head, Iframe and Title above are too).
    Script, Style, Template, Textarea, Noscript, Object, Embed, Svg, Math,
    Count
};

// The lowercase name of `tag`; empty for Unknown.
std::string_view TagName(Tag tag);
// The tag `name` (any case) names, or Unknown: also for names longer than 10 characters and
// names with characters other than letters and digits, such as custom elements.
Tag LookupTag(std::string_view name);
// A name of CommonMark's block start condition 6.
bool IsBlockName(Tag tag);
// An element whose content is not shown: scripts, styles, templates, frames, embedded objects.
bool IsDroppedWithContent(Tag tag);

constexpr size_t kMaxTagBytes = 4096;
constexpr size_t kMaxTagAttributes = 64;

// The next '"' or '\'' of a text at or after a position. Queries for one text that move forward
// search each byte at most once; one that goes back before the last search (a tag that began
// inside the quoted value of a tag that failed) searches only up to where that search began.
class QuoteCache {
public:
    // The position of the first `quote` in `text` at or after `from`, or text.size().
    size_t Next(std::string_view text, size_t from, char quote);

private:
    const char* base = nullptr;
    size_t size = 0;
    bool known[2] = {};          // '"', '\''
    size_t searchedFrom[2] = {};
    size_t found[2] = {};        // the first quote at or after searchedFrom, or size
};

// An open or closing tag: its element, form and length, and the values of the attributes RayoMD
// uses, as views into the text without their quotes and with character references left as written.
struct TagToken {
    Tag tag = Tag::Unknown;
    bool closing = false;
    bool selfClosing = false;
    bool hasOpen = false;   // the `open` attribute, which has no value
    uint32_t length = 0;
    std::string_view align, width, height, src, alt, href, start, id, name, open;
};

// The length of the CommonMark open or closing tag at text[at] == '<', with `token` filled in,
// or 0: also for tags longer than kMaxTagBytes or with more than kMaxTagAttributes attributes.
// Attributes may be separated by line endings. `quotes` must only see positions that grow.
uint32_t ScanTag(std::string_view text, size_t at, QuoteCache& quotes, TagToken& token);

// CommonMark's HTML block start condition, 1 to 7, of a line whose indentation of up to three
// spaces is already gone; 0 for none. Only 1 to 6 interrupt a paragraph.
int HtmlBlockStart(std::string_view line);
// Whether `line` ends an HTML block of start condition `condition`. For 1 to 5 the line holds the
// end marker and belongs to the block (the block's first line may already hold it); for 6 and 7
// the line is blank and does not.
bool HtmlBlockEnds(std::string_view line, int condition);

// Whether a link or image may point at `url`: not, compared case-insensitively after leading
// white space and control characters and ignoring tabs and line breaks as browsers do, to a
// javascript:, vbscript: or data: URL, except data:image/ for an image's source.
bool SafeUrl(std::string_view url, bool image = false);

// The HTML elements open at one level of a document (the document, a list item, a quote) while
// ParseMarkdown reads it. An element an HTML block leaves open stays open for the blocks after
// it, so `<div align="center">`, Markdown, `</div>` centres the Markdown; a later HTML block's
// close tag ends it. At most kMaxOpen deep; elements beyond are transparent.
struct HtmlState {
    static constexpr size_t kMaxOpen = 32;
    struct Open {
        Tag tag;
        uint8_t align;      // Block::align
    };
    Open open[kMaxOpen];
    size_t depth = 0;

    // The alignment of the innermost open element that has one; 0 for none.
    uint8_t Align() const;
};

// Appends the blocks of `html`, an HTML block as CommonMark ends it, to `out`: text and inline
// tags as paragraphs for the inline parser (white space collapsed, Markdown syntax escaped, as
// CommonMark reads no Markdown inside HTML), and p, div, headings, hr, pre, blockquote, lists,
// tables, dl, details/summary, captions and images as the blocks they stand for. Each block gets
// the alignment of the innermost element around it; an id makes an Anchor block.
void MapBlock(std::string_view html, HtmlState& state, std::vector<Block>& out);

// Where an HTML block's blocks are in a level's blocks, [mapStart, mapEnd), and the alignment the
// elements it leaves open give the blocks after it, up to the next HTML block (ApplyAlignments).
struct Alignment {
    size_t mapStart;
    size_t mapEnd;
    uint8_t align;
};

// MapBlock for the HTML block that starts at lines[start] with start condition `condition`: up to
// the line that meets its end condition (1-5) or up to a blank line (6, 7), or to the end of the
// level, its Alignment appended to `alignments`. Returns the index of the line after it.
size_t MapBlockLines(const std::vector<std::string_view>& lines, size_t start, int condition, HtmlState& state,
    std::vector<Block>& out, std::vector<Alignment>& alignments);

// Gives each block between HTML blocks that has no alignment of its own the one of the elements
// left open before it (<div align="center">, Markdown, </div>).
void ApplyAlignments(std::vector<Block>& blocks, const std::vector<Alignment>& alignments);

// A Markdown paragraph that is HTML's: only an <a id> or <a name> (an Anchor block), or only
// images, Markdown or <img>, each perhaps linked, apart by white space or <br> (an Image block,
// with a row for more than one). Appends the block and returns true, or returns false.
bool MapParagraph(std::string_view paragraph, std::vector<Block>& out);

} // namespace TinyPdf::Internal::Html
