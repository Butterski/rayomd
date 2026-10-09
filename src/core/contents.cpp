#include "contents.h"

#include <algorithm>
#include <string_view>

namespace TinyPdf::Internal {
namespace {

constexpr size_t kNone = static_cast<size_t>(-1);

// Whether a paragraph is "[TOC]" or "[[_TOC_]]" in any case. The parser trims a paragraph, and
// a document that defines the link label "toc" has "[TOC]" resolved to that link.
bool IsContentsMarker(const std::string& text) {
    const auto same = [&text](std::string_view marker) {
        if (text.size() != marker.size()) return false;
        for (size_t at = 0; at < marker.size(); at++) {
            const char ch = text[at] >= 'A' && text[at] <= 'Z' ? (char)(text[at] | 0x20) : text[at];
            if (ch != marker[at]) return false;
        }
        return true;
    };
    return same("[toc]") || same("[[_toc_]]");
}

// Whether a heading shows text: not only footnote references and spaces, which leave nothing to list.
bool IsListedHeading(const Block& block) {
    if (block.type != BlockType::Heading) return false;
    const std::string& text = block.text;
    for (size_t at = 0; at < text.size(); at++) {
        const char ch = text[at];
        if (ch == kMathTextOpen && at + 1 < text.size() && text[at + 1] == kNoteMark) {
            const size_t close = text.find(kMathTextClose, at);
            if (close == std::string::npos) return false;
            at = close;
        } else if (ch != kMathTextOpen && ch != kMathTextClose && ch != ' ' && ch != '\t') {
            return true;
        }
    }
    return false;
}

// The listed headings of a document by level, and its title heading: the first one, when that is
// its only one of level 1 (kNone otherwise), which the table leaves out and goes after.
struct HeadingCensus {
    size_t perLevel[7] = {};
    size_t title = kNone;

    explicit HeadingCensus(const std::vector<Block>& blocks) {
        size_t first = kNone;
        for (size_t index = 0; index < blocks.size(); index++) {
            if (!IsListedHeading(blocks[index])) continue;
            if (first == kNone) first = index;
            perLevel[std::clamp(blocks[index].level, 1, 6)]++;
        }
        if (first != kNone && blocks[first].level <= 1 && perLevel[1] == 1) {
            title = first;
            perLevel[1] = 0;
        }
    }
};

// Heading text as it shows: a formula keeps its TeX, a footnote reference goes.
std::string VisibleHeading(const std::string& text) {
    if (text.find(kMathTextOpen) == std::string::npos) return text;
    std::string visible;
    visible.reserve(text.size());
    for (size_t at = 0; at < text.size(); at++) {
        if (text[at] == kMathTextOpen && at + 1 < text.size() && text[at + 1] == kNoteMark) {
            const size_t close = text.find(kMathTextClose, at);
            if (close == std::string::npos) break;
            at = close;
        } else if (text[at] != kMathTextOpen && text[at] != kMathTextClose) {
            visible += text[at];
        }
    }
    return visible;
}

// The entries of a table that lists the headings `census` counted, to `depth` levels from its top.
std::vector<ContentsEntry> ListedEntries(const std::vector<Block>& blocks, const HeadingCensus& census, int depth) {
    int top = 1;
    while (top < 7 && census.perLevel[top] == 0) top++;
    const int last = std::min(6, top + std::clamp(depth, 1, 6) - 1);
    size_t count = 0;
    for (int level = top; level <= last; level++) count += census.perLevel[level];
    std::vector<ContentsEntry> entries(count);
    for (size_t index = 0, entry = 0; entry < count; index++) {
        const Block& block = blocks[index];
        const int level = std::clamp(block.level, 1, 6);
        if (!IsListedHeading(block) || index == census.title || level > last) continue;
        entries[entry++] = { level - top + 1, VisibleHeading(block.text), block.text.data() };
    }
    return entries;
}

// The first marker paragraph of the document's top level, or kNone. Every document comes through
// here: one pass over the blocks that looks at the length of a block's text first, a marker's
// being 5 or 9 bytes.
size_t FirstMarker(const std::vector<Block>& blocks) {
    const Block* const begin = blocks.data();
    const Block* const end = begin + blocks.size();
    for (const Block* block = begin; block != end; block++) {
        const size_t length = block->text.size();
        if ((length == 5 || length == 9) && block->type == BlockType::Paragraph && IsContentsMarker(block->text)) {
            return (size_t)(block - begin);
        }
    }
    return kNone;
}

// Makes the marker paragraphs from `found` on Contents blocks: the renderers draw the first of
// them that they reach, later ones are empty.
void RetypeMarkers(std::vector<Block>& blocks, size_t found) {
    for (size_t index = found; index < blocks.size(); index++) {
        Block& block = blocks[index];
        if (block.type != BlockType::Paragraph || !IsContentsMarker(block.text)) continue;
        block.type = BlockType::Contents;
        block.text.clear();
    }
}

} // namespace

std::vector<ContentsEntry> PlaceContents(std::vector<Block>& blocks, bool insert, int depth) {
    const size_t found = FirstMarker(blocks);
    if (found == kNone && !insert) return {};
    // Counted before the table goes in, which it does after the title: the title's index stays.
    const HeadingCensus census(blocks);
    if (found != kNone) {
        RetypeMarkers(blocks, found);
    } else {
        Block contents;
        contents.type = BlockType::Contents;
        blocks.insert(blocks.begin() + (std::ptrdiff_t)(census.title == kNone ? 0 : census.title + 1), std::move(contents));
    }
    return ListedEntries(blocks, census, depth);
}

bool MarkContents(std::vector<Block>& blocks) {
    const size_t found = FirstMarker(blocks);
    if (found == kNone) return false;
    RetypeMarkers(blocks, found);
    return true;
}

bool BeginsWithTitle(const std::vector<Block>& blocks) {
    for (const Block& block : blocks) {
        if (IsListedHeading(block)) return block.level <= 1;
    }
    return false;
}

std::vector<ContentsEntry> BookContents(const std::vector<BookContentsFile>& files, int depth) {
    // The entries at their levels in the book, as its bookmarks have them (RenderBook).
    std::vector<ContentsEntry> entries;
    for (const BookContentsFile& file : files) {
        if (file.blocks == nullptr) {
            if (!file.title->empty()) entries.push_back({ 1, *file.title, file.title->data() });
            continue;
        }
        for (const Block& block : *file.blocks) {
            if (!IsListedHeading(block)) continue;
            entries.push_back({ std::min(6, std::clamp(block.level, 1, 6) + file.shift), VisibleHeading(block.text), block.text.data() });
        }
    }
    int top = 7;
    for (const ContentsEntry& entry : entries) top = std::min(top, entry.level);
    const int last = std::min(6, top + std::clamp(depth, 1, 6) - 1);
    size_t kept = 0;
    for (size_t index = 0; index < entries.size(); index++) {
        if (entries[index].level > last) continue;
        entries[index].level -= top - 1;
        if (kept != index) entries[kept] = std::move(entries[index]);
        kept++;
    }
    entries.erase(entries.begin() + (std::ptrdiff_t)kept, entries.end());
    return entries;
}

} // namespace TinyPdf::Internal
