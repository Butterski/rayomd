#pragma once

// The table of contents: where a document has it, and the headings it lists. The page numbers are
// known only once the pages are, so the renderers lay out the entries in the flow of the text and
// the builder adds the numbers to the table's pages afterwards. Built at -Os: once per document.

#include "markdown_parser.h"

#include <string>
#include <vector>

namespace TinyPdf::Internal {

// A heading the table lists: its level below the table's top (1 for the top), its text as it shows,
// without the markers of formulas and footnotes, and the text of the heading's block, by whose
// address the renderers' heading marks find it.
struct ContentsEntry {
    int level = 1;
    std::string text;
    const char* heading = nullptr;
};

// Makes the first paragraph "[TOC]" or "[[_TOC_]]", in any case, of the document's top level its
// table of contents (BlockType::Contents), and later ones Contents blocks the renderers do not draw:
// one table, as on GitLab and Azure DevOps. With `insert` and no such paragraph, the table goes
// first, or after the title heading. Returns the table's entries, none for a document without one:
// the headings of the top level of them and `depth` - 1 levels below it (`depth` taken as 1 to 6).
// The first heading of a document whose only level-1 heading it is, is its title, which the table
// leaves out, as it does a heading of footnote references alone.
std::vector<ContentsEntry> PlaceContents(std::vector<Block>& blocks, bool insert, int depth);

} // namespace TinyPdf::Internal
