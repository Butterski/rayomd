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
    std::vector<std::vector<std::string>> rows;
    std::vector<int> aligns;
};

std::vector<std::string> SplitLines(const std::string& text);
std::string NormalizeSymbols(std::string text);
std::string StripInlineMarkdown(std::string_view input);
std::vector<Block> ParseMarkdown(const std::string& markdown);

} // namespace TinyPdf::Internal