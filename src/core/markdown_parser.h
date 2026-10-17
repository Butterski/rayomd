// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace TinyPdf::Internal {

enum class BlockType {
    Heading,
    Paragraph,
    Bullet,
    Numbered,
    Quote,
    Code,
    MathBlock,
    Table,
    Rule,
    PageBreak,
    Image,
    // The table of contents (contents.h), which the builder places after the parse.
    Contents,
    // An HTML element's id (or an <a name>): a target for "#id" links where it stands. Its
    // text is the id; it draws nothing.
    Anchor,
};

// Where an image block's picture comes from, and for a linked image, "[![alt](src)](target)",
// what it links to.
struct ImageSource {
    std::string src;
    std::string link;
    // An HTML <img>'s width and height attributes in points (pixels times 0.75), 0 when not
    // given; with `widthPercent`, widthHint is a fraction of the text width.
    float widthHint = 0.0f;
    float heightHint = 0.0f;
    bool widthPercent = false;
    // In a row of images (a paragraph of nothing else): the alt text of this one, and in the
    // first, the images after it. The first image's alt text is its block's text.
    std::string alt;
    std::vector<ImageSource> row;
};

constexpr uint32_t kNoSourceLine = 0xFFFFFFFFu;

struct Block {
    BlockType type = BlockType::Paragraph;
    int level = 0;
    int number = 0;
    // Bullet and Numbered only: 1 for a task list item "[ ]", 2 for a done one "[x]". In the
    // padding after `number`, which keeps the moves of a block as they were.
    uint8_t task = 0;
    // Quote only: the GitHub alert it is, 1 to 5 for [!NOTE], [!TIP], [!IMPORTANT], [!WARNING]
    // and [!CAUTION], or 0 for a plain quote. In the same padding.
    uint8_t alert = 0;
    // Heading and Table only, while ParseMarkdown runs: the text or cells are still Markdown
    // that may refer to a footnote, made plain once all footnotes are known. In the same padding.
    uint8_t notesPending = 0;
    // Code only: the language the info string of its fence names (FenceLanguage, highlight.h),
    // kMermaidCode for a Mermaid diagram's, or 0 for one shown in one colour. In the same padding.
    uint8_t codeLanguage = 0;
    std::string text;
    // Set for every Image block and for no other: the other blocks carry a null pointer
    // instead of two empty strings.
    std::unique_ptr<ImageSource> image;
    // Table cells. Inline Markdown that the renderers parse like paragraph text, except
    // in a table with hasMath, whose cells are plain text with formulas.
    std::vector<std::vector<std::string>> rows;
    std::vector<Block> children;
    std::vector<int> aligns;
    // Heading and Table only. Set by the parser when the heading text, or at least
    // one table cell, carries formulas as kMathTextOpen + TeX + kMathTextClose.
    // The renderers look for those bytes only when this flag is set, so control
    // bytes in ordinary input can never be mistaken for a formula.
    bool hasMath = false;
    // The alignment of an HTML element around the block (align="center", <center>): 0 for
    // none, 1 left, 2 centred, 3 right. In the padding after hasMath.
    uint8_t align = 0;
    // Top-level blocks only (ParseMarkdown's depth 0): how many lines of the Markdown the block
    // came from (at most 65,535) and the first of them (0-based, lines end at '\n'), for
    // PdfOptions::sourceMap. Blocks inside list items, quotes and notes, and inserted ones (the
    // --toc table, a book's title), keep kNoSourceLine. In the padding after `align`.
    uint16_t sourceLines = 0;
    uint32_t sourceLine = kNoSourceLine;
};
static_assert(sizeof(Block) <= 136, "Block keeps its size: align lives in the padding");

constexpr char kMathTextOpen = '\x01';
constexpr char kMathTextClose = '\x02';
// A footnote reference in text with formulas (Block::hasMath) is kMathTextOpen, kNoteMark, the
// note's number in decimal and kMathTextClose.
constexpr char kNoteMark = '\x03';

// The notes of a document that its text refers to, numbered by first reference in reading order:
// the text, then each note in turn. Note N is a paragraph, "[N.](#^rN) " and the note's first
// paragraph, its number linking back to the first reference, which links to "#^N"; the note's
// other blocks are its children. `numbers` has the number of every label referred to,
// normalized as link labels are.
struct Footnotes {
    std::vector<Block> notes;      // note N is notes[N - 1]
    std::unordered_map<std::string, int> numbers;
};

std::vector<std::string> SplitLines(const std::string& text);
std::string StripInlineMarkdown(std::string_view input, bool recognizeMath = true);
// Like StripInlineMarkdown, but keeps formulas as kMathTextOpen + TeX + kMathTextClose.
// When the input has no formula the result is byte-identical to StripInlineMarkdown
// and hasMath is false; otherwise literal kMathText* bytes are removed from the input.
std::string StripInlineMarkdownKeepMath(std::string_view input, bool& hasMath);
// With `footnotes`, "[^label]: text" starts a footnote definition, which leaves the text, and
// "[^label]" refers to it; see Footnotes. Without, both are text.
std::vector<Block> ParseMarkdown(const std::string& markdown, Footnotes* footnotes = nullptr);
// The Markdown image "![alt](src)" or linked image "[![alt](src)](target)" at text[at]: the
// index behind it, with its alt text (inline Markdown stripped) and source, or 0 for none.
size_t ParseImageAt(std::string_view text, size_t at, std::string& alt, ImageSource& source);

// Walks text produced by StripInlineMarkdownKeepMath for a block whose hasMath is
// set: fn(segment, isMath) is called for every non-empty text or TeX segment.
template <typename Fn>
void ForEachMathTextSegment(std::string_view text, Fn fn) {
    size_t position = 0;
    while (position < text.size()) {
        size_t open = text.find(kMathTextOpen, position);
        if (open == std::string_view::npos) {
            fn(text.substr(position), false);
            return;
        }
        if (open > position) fn(text.substr(position, open - position), false);
        size_t close = text.find(kMathTextClose, open + 1);
        if (close == std::string_view::npos) return;
        if (close > open + 1) fn(text.substr(open + 1, close - open - 1), true);
        position = close + 1;
    }
}

} // namespace TinyPdf::Internal