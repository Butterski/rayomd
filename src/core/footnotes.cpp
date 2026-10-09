#include "footnotes.h"

#include "../common/text_utils.h"

#include <algorithm>
#include <iterator>
#include <string_view>

namespace TinyPdf::Internal {
namespace {

bool MayContainMath(std::string_view text) {
    return text.find('$') != std::string_view::npos || text.find("\\(") != std::string_view::npos;
}

// Text without the bytes that mark formulas, as the parser writes it.
void AppendWithoutMarkers(std::string& out, std::string_view text) {
    for (const char ch : text) {
        if (ch != kMathTextOpen && ch != kMathTextClose) out.push_back(ch);
    }
}

std::string Trimmed(std::string text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    return text.substr(begin, text.find_last_not_of(" \t\r\n") + 1 - begin);
}

// Columns of indentation, a tab to the next multiple of four, as the parser counts them, and
// in `bytes` how many bytes they take.
int IndentColumns(std::string_view line, size_t& bytes) {
    int columns = 0;
    for (bytes = 0; bytes < line.size() && (line[bytes] == ' ' || line[bytes] == '\t'); bytes++) {
        columns = line[bytes] == ' ' ? columns + 1 : columns + 4 - columns % 4;
    }
    return columns;
}

// The line without its first four columns of indentation (fewer when it has fewer).
std::string_view WithoutFourColumns(std::string_view line) {
    int columns = 0;
    size_t at = 0;
    for (; at < line.size() && columns < 4 && (line[at] == ' ' || line[at] == '\t'); at++) {
        columns = line[at] == ' ' ? columns + 1 : columns + 4 - columns % 4;
    }
    return line.substr(at);
}

bool IsBlank(std::string_view line) {
    return line.find_first_not_of(" \t\r") == std::string_view::npos;
}

// The position of the colon of "[^label]:" at the start of `value`, the label 1 to 999 bytes
// without white space or ']' as cmark-gfm reads it, or npos.
size_t FootnoteDefinitionColon(std::string_view value) {
    if (value.size() < 5 || value[0] != '[' || value[1] != '^') return std::string_view::npos;
    size_t end = 2;
    for (; end < value.size() && value[end] != ']'; end++) {
        const char ch = value[end];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0' || end - 2 >= 999) {
            return std::string_view::npos;
        }
    }
    return end > 2 && end + 1 < value.size() && value[end + 1] == ':' ? end + 1 : std::string_view::npos;
}

// After the parse: numbers the footnotes the text refers to, in reading order, and makes the
// headings and tables that waited for them plain (Block::notesPending).
class FootnoteNumbering {
public:
    explicit FootnoteNumbering(FootnoteParse& state) : parse(state), labels(state.out.numbers) {}

    void Run(std::vector<Block>& blocks) {
        Walk(blocks);
        // References in a note number the notes they bring in after all those before them.
        for (size_t index = 0; index < parse.numbered.size(); index++) {
            std::vector<Block> note = std::move(parse.numbered[index]);
            Walk(note);
            parse.numbered[index] = std::move(note);
        }
    }

private:
    int Number(std::string_view label) {
        const auto found = parse.out.numbers.find(NormalizeReferenceLabel(label));
        if (found == parse.out.numbers.end()) return 0;
        if (found->second == 0) {
            found->second = static_cast<int>(parse.numbered.size()) + 1;
            parse.numbered.push_back(std::move(parse.bodies[parse.bodyIndex.at(found->first)]));
        }
        return found->second;
    }

    void Walk(std::vector<Block>& blocks) {
        for (Block& block : blocks) {
            if (block.notesPending != 0) {
                Settle(block);
            } else if (block.type == BlockType::Paragraph || block.type == BlockType::Bullet ||
                block.type == BlockType::Numbered || block.type == BlockType::Quote) {
                if (block.text.find("[^") != std::string::npos) {
                    ParseInlineRuns(block.text, runs);
                    for (const InlineRun& run : runs.runs) {
                        if (run.math == InlineMath::Note) Number(runs.Text(run));
                    }
                }
            }
            Walk(block.children);
        }
    }

    // Heading or cell text as StripInlineMarkdownKeepMath makes it, its footnote references as
    // kMathTextOpen + kNoteMark + number + kMathTextClose; `special` when it has either.
    std::string Visible(std::string_view input, bool& special) {
        special = false;
        if (!MayContainMath(input) && input.find("[^") == std::string_view::npos) return StripInlineMarkdown(input);
        ParseInlineRuns(input, runs);
        for (const InlineRun& run : runs.runs) special = special || run.math != InlineMath::None;
        if (!special) return Trimmed(runs.text);
        std::string visible;
        visible.reserve(input.size() + 8);
        for (const InlineRun& run : runs.runs) {
            if (run.math == InlineMath::None) {
                AppendWithoutMarkers(visible, runs.Text(run));
                continue;
            }
            visible.push_back(kMathTextOpen);
            if (run.math == InlineMath::Note) {
                visible.push_back(kNoteMark);
                visible += std::to_string(Number(runs.Text(run)));
            } else {
                AppendWithoutMarkers(visible, runs.Text(run));
            }
            visible.push_back(kMathTextClose);
        }
        return Trimmed(std::move(visible));
    }

    void Settle(Block& block) {
        block.notesPending = 0;
        if (block.type == BlockType::Heading) {
            block.text = Visible(block.text, block.hasMath);
            return;
        }
        // A table with a formula or a footnote reference shows its other cells as plain text,
        // as the parser makes a table with formulas.
        std::vector<std::vector<std::string>> visible(block.rows.size());
        std::vector<std::vector<unsigned char>> special(block.rows.size());
        bool any = false;
        for (size_t row = 0; row < block.rows.size(); row++) {
            for (const std::string& cell : block.rows[row]) {
                bool cellSpecial = false;
                visible[row].push_back(Visible(cell, cellSpecial));
                special[row].push_back(cellSpecial ? 1 : 0);
                any = any || cellSpecial;
            }
        }
        if (!any) return;
        constexpr unsigned char kSyntax = RayoMd::Text::kByteInlineSyntax | RayoMd::Text::kByteSymbolLead;
        for (size_t row = 0; row < block.rows.size(); row++) {
            for (size_t column = 0; column < block.rows[row].size(); column++) {
                std::string& cell = block.rows[row][column];
                if (special[row][column] != 0) {
                    cell = std::move(visible[row][column]);
                    continue;
                }
                if (RayoMd::Text::ContainsByteClass(cell, kSyntax)) cell = StripInlineMarkdown(cell);
                cell.erase(std::remove_if(cell.begin(), cell.end(),
                    [](char ch) { return ch == kMathTextOpen || ch == kMathTextClose; }), cell.end());
            }
        }
        block.hasMath = true;
    }

    FootnoteParse& parse;
    FootnoteLabels labels;      // the inline parser reads references to the labels defined
    InlineRuns runs;
};

} // namespace

void DefineFootnotes(size_t first, const ReferenceDefinitions& definitions, int depth, FootnoteParse& notes) {
    // Taken out first: parsing a note's text adds the definitions in it to notes.found.
    std::vector<std::pair<std::string, std::string>> level(std::make_move_iterator(notes.found.begin() + first),
        std::make_move_iterator(notes.found.end()));
    notes.found.resize(first);
    for (auto& [label, body] : level) {
        if (!notes.out.numbers.emplace(label, 0).second) continue;
        if (!definitions.empty() && body.find('[') != std::string::npos) body = ResolveReferenceLinks(body, definitions);
        std::vector<Block> blocks;
        if (depth < 8) {
            blocks = ParseMarkdownLevel(body, depth + 1, &notes);
        } else {
            blocks.emplace_back();
            blocks.back().text = StripInlineMarkdown(body);
        }
        notes.bodyIndex.emplace(std::move(label), notes.bodies.size());
        notes.bodies.push_back(std::move(blocks));
    }
}

size_t ReadFootnoteDefinition(const std::vector<std::string_view>& lines, size_t index,
    bool (*isText)(const void* context, size_t line), const void* context, FootnoteParse& notes) {
    size_t indentBytes = 0;
    if (IndentColumns(lines[index], indentBytes) > 3) return 0;
    const std::string_view value = lines[index].substr(indentBytes);
    const size_t colon = FootnoteDefinitionColon(value);
    if (colon == std::string_view::npos) return 0;
    std::string label = NormalizeReferenceLabel(value.substr(2, colon - 3));
    if (label.empty()) return 0;
    const size_t textBegin = value.find_first_not_of(" \t\r", colon + 1);
    const std::string_view first = textBegin == std::string_view::npos ? std::string_view() : value.substr(textBegin);
    std::string body(first);
    bool lazy = !first.empty();     // the last line is paragraph text, which a lazy line goes on
    size_t end = index + 1;
    size_t bytes = 0;
    while (end < lines.size()) {
        if (IsBlank(lines[end])) {
            // Blank lines belong to the definition when an indented line follows them.
            size_t next = end + 1;
            while (next < lines.size() && IsBlank(lines[next])) next++;
            if (next == lines.size() || IndentColumns(lines[next], bytes) < 4) break;
            body.append(next - end, '\n');
            end = next;
            lazy = false;
            continue;
        }
        if (IndentColumns(lines[end], bytes) >= 4) {
            const std::string_view content = WithoutFourColumns(lines[end]);
            body.push_back('\n');
            body.append(content.data(), content.size());
            lazy = IndentColumns(content, bytes) < 4;
            end++;
            continue;
        }
        const std::string_view text = lines[end].substr(bytes);
        if (!lazy || !isText(context, end) || FootnoteDefinitionColon(text) != std::string_view::npos) break;
        body.push_back('\n');
        body.append(text.data(), text.find_last_not_of(" \t\r") + 1);
        end++;
    }
    notes.found.emplace_back(std::move(label), std::move(body));
    return end - index;
}

void AppendPendingHeading(std::vector<Block>& blocks, int level, std::string_view text,
    const ReferenceDefinitions& definitions, FootnoteParse& notes) {
    blocks.emplace_back();
    Block& block = blocks.back();
    block.type = BlockType::Heading;
    block.level = level;
    block.text = definitions.empty() ? std::string(text) : ResolveReferenceLinks(text, definitions);
    block.notesPending = 1;
    notes.pending = true;
}

std::vector<Block> ParseWithFootnotes(const std::string& markdown, Footnotes& footnotes) {
    footnotes.notes.clear();
    footnotes.numbers.clear();
    FootnoteParse notes(footnotes);
    std::vector<Block> blocks = ParseMarkdownLevel(markdown, 0, &notes);
    if (!notes.pending && footnotes.numbers.empty()) return blocks;
    FootnoteNumbering(notes).Run(blocks);
    for (auto it = footnotes.numbers.begin(); it != footnotes.numbers.end();) {
        it = it->second == 0 ? footnotes.numbers.erase(it) : std::next(it);
    }
    // Note N: "[N.](#^rN)", its number linking back to its first reference, in front of its first
    // paragraph, whose children are the note's other blocks.
    footnotes.notes.resize(notes.numbered.size());
    for (size_t index = 0; index < notes.numbered.size(); index++) {
        const std::string number = std::to_string(index + 1);
        std::vector<Block>& body = notes.numbered[index];
        Block& note = footnotes.notes[index];
        note.text = "[" + number + ".](#^r" + number + ")";
        if (!body.empty() && body.front().type == BlockType::Paragraph) {
            note.text += ' ';
            note.text += body.front().text;
            body.erase(body.begin());
        }
        note.children = std::move(body);
    }
    return blocks;
}

} // namespace TinyPdf::Internal
