#pragma once

// Footnotes in the parser (see Footnotes in markdown_parser.h): the definitions it collects, the
// numbering once the whole document is read, and the plain text of the headings and tables that
// waited for it. Kept apart from markdown_parser.cpp, built at -Os: inside, this code changed
// what GCC inlines in the line classifier and the block parser (+1.7 % instructions on
// baseline.md, a document without footnotes).

#include "inline_markdown.h"
#include "markdown_parser.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace TinyPdf::Internal {

// What ParseMarkdown collects of the footnotes while it parses. out.numbers holds every label
// defined, 0 until the text refers to it; the first definition of a label counts.
struct FootnoteParse {
    explicit FootnoteParse(Footnotes& footnotes) : out(footnotes) {}
    Footnotes& out;
    std::vector<std::vector<Block>> bodies;     // by definition
    std::vector<std::vector<Block>> numbered;   // note N at [N - 1]
    std::unordered_map<std::string, size_t> bodyIndex;
    // The definitions read but not parsed yet, (normalized label, text): those of the levels
    // being parsed, each level's from where its parse began.
    std::vector<std::pair<std::string, std::string>> found;
    bool possible = false;  // the document has "]:", which every definition has
    bool pending = false;   // a heading or table waits (Block::notesPending)
};

// One level of a document, as ParseMarkdown parses it: a nested one for a footnote's text.
std::vector<Block> ParseMarkdownLevel(const std::string& markdown, int depth, FootnoteParse* notes);

// A footnote definition at lines[index], as GitHub reads one: "[^label]:" after at most three
// spaces, its text after the colon and on the lines after it that are indented by four columns
// (taken off), blank lines between those, and lazy lines that go on its last paragraph, which
// are the lines `isText(context, line)` says the parser takes for paragraph text. It joins
// notes.found; returns the number of lines it takes, 0 when none starts there.
size_t ReadFootnoteDefinition(const std::vector<std::string_view>& lines, size_t index,
    bool (*isText)(const void* context, size_t line), const void* context, FootnoteParse& notes);

// A heading that may refer to a footnote, at the end of `blocks`: its text stays Markdown until
// all footnotes are known.
void AppendPendingHeading(std::vector<Block>& blocks, int level, std::string_view text,
    const ReferenceDefinitions& definitions, FootnoteParse& notes);

// The footnotes defined at one level, notes.found from `first` on: each label's first is parsed
// as blocks of its own, its references resolved with the link definitions of that level.
void DefineFootnotes(size_t first, const ReferenceDefinitions& definitions, int depth, FootnoteParse& notes);

// ParseMarkdown with footnotes.
std::vector<Block> ParseWithFootnotes(const std::string& markdown, Footnotes& footnotes);

} // namespace TinyPdf::Internal
