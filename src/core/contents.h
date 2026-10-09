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

// A book's file (BuildBookPdf): makes its marker paragraphs Contents blocks, which draw the book's
// table where the first of the book is reached; returns whether it has one.
bool MarkContents(std::vector<Block>& blocks);

// Whether a file's first heading on its top level is of level 1, its title (ParseBook).
bool BeginsWithTitle(const std::vector<Block>& blocks);

// A file of a book, or a part, as its table of contents lists it (BookContents).
struct BookContentsFile {
    const std::vector<Block>* blocks = nullptr;   // the file's; null for a part
    const std::string* title = nullptr;           // the part's
    int shift = 0;                                // the levels the file's headings move down in the book
};

// The table of contents of a book: each part's title and the files' headings of their top level,
// as many levels down as the bookmarks have them; `depth` levels from the top one (taken as 1 to 6).
std::vector<ContentsEntry> BookContents(const std::vector<BookContentsFile>& files, int depth);

} // namespace TinyPdf::Internal
