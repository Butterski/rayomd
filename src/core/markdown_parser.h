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
};

// Where an image block's picture comes from, and for a linked image, "[![alt](src)](target)",
// what it links to.
struct ImageSource {
    std::string src;
    std::string link;
};

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
};

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
std::string NormalizeSymbols(std::string text);
std::string StripInlineMarkdown(std::string_view input, bool recognizeMath = true);
// Like StripInlineMarkdown, but keeps formulas as kMathTextOpen + TeX + kMathTextClose.
// When the input has no formula the result is byte-identical to StripInlineMarkdown
// and hasMath is false; otherwise literal kMathText* bytes are removed from the input.
std::string StripInlineMarkdownKeepMath(std::string_view input, bool& hasMath);
// With `footnotes`, "[^label]: text" starts a footnote definition, which leaves the text, and
// "[^label]" refers to it; see Footnotes. Without, both are text.
std::vector<Block> ParseMarkdown(const std::string& markdown, Footnotes* footnotes = nullptr);

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