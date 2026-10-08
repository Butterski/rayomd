#pragma once

#include <string>
#include <string_view>
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

struct Block {
    BlockType type = BlockType::Paragraph;
    int level = 0;
    int number = 0;
    std::string text;
    std::string imageSrc;
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

std::vector<std::string> SplitLines(const std::string& text);
std::string NormalizeSymbols(std::string text);
std::string StripInlineMarkdown(std::string_view input, bool recognizeMath = true);
// Like StripInlineMarkdown, but keeps formulas as kMathTextOpen + TeX + kMathTextClose.
// When the input has no formula the result is byte-identical to StripInlineMarkdown
// and hasMath is false; otherwise literal kMathText* bytes are removed from the input.
std::string StripInlineMarkdownKeepMath(std::string_view input, bool& hasMath);
std::vector<Block> ParseMarkdown(const std::string& markdown);

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