#pragma once

// A book: reading its files for --book (RayoMd::Book), their order and titles, from a SUMMARY.md
// (the list of mdBook and GitBook), a directory, or files named on the command line; and parsing
// them for BuildBookPdf (TinyPdf::Internal). Built at -Os: once per book.

#include "rayomd/tiny_pdf.h"
#include "contents.h"
#include "markdown_parser.h"

#include <filesystem>
#include <string>
#include <vector>

namespace TinyPdf::Internal {

// A book's files parsed each on its own (ParseBookFiles), with its link definitions and footnotes.
struct BookFiles {
    std::vector<std::vector<Block>> blocks;   // each file's, none for a part
    std::vector<Footnotes> footnotes;         // each file's
    std::vector<ContentsEntry> contents;      // the book's table of contents, none without one
    bool notes = false;                       // some file has footnotes

    BookFiles();
    ~BookFiles();
};

// Parses each file of a book, `texts` and `titles` its chapters' Markdown and titles as the
// renderer reads them. A file with a title that does not begin with a level-1 heading gets one of
// it first, as mdBook prints it. The book has a table of contents with `toc`, or where the first
// file with a "[TOC]" paragraph has it, listing `tocDepth` levels (BookContents).
void ParseBookFiles(const std::vector<BookChapter>& chapters, const std::vector<std::string>& texts,
    const std::vector<std::string>& titles, bool toc, int tocDepth, BookFiles& files);

} // namespace TinyPdf::Internal

namespace RayoMd::Book {

// An entry of SUMMARY.md: a part's title, or a file by its link, percent-decoded and relative to
// SUMMARY.md, its bookmarks `depth` levels down: one for each list it is nested in, and one more
// in a part (TinyPdf::BookChapter::depth).
struct SummaryEntry {
    std::string title;
    std::string path;
    int depth = 0;
    bool part = false;
};

struct Summary {
    std::string title;
    std::vector<SummaryEntry> entries;
};

// SUMMARY.md as mdBook and GitBook read it. A first heading is the book's title, unless it names
// the list ("Summary", "Table of contents", "Contents"); a later heading of level 1 (mdBook) or 2
// (GitBook) is a part, which holds the list items after it; a link of a paragraph is a file in no
// part (a prefix or suffix chapter), and the first link of a list item a file nested as the item
// is. A link without a target is a draft without a file; other content and links to a URL are
// ignored.
Summary ParseSummary(const std::string& markdown);

// The book of --book's inputs: one SUMMARY.md; one directory with a SUMMARY.md, or src/SUMMARY.md
// as mdBook keeps it, or else the directory's Markdown files sorted by name, README.md or index.md
// first; or the files given, in their order. Reads every file. False with `error` set when a file
// cannot be read or SUMMARY.md lists one twice.
bool ReadBook(const std::vector<std::filesystem::path>& inputs, TinyPdf::Book& book, std::string& error);

} // namespace RayoMd::Book
