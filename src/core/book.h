// SPDX-License-Identifier: Apache-2.0

#pragma once

// A book: reading its files for --book (RayoMd::Book), their order and titles, from a SUMMARY.md
// (the list of mdBook and GitBook), a directory, or files named on the command line; a note and
// the notes it links to for --follow-links (ReadLinkedBook); and parsing them for BuildBookPdf
// (TinyPdf::Internal), with --backlinks' paragraphs. Built at -Os: once per book.

#include "rayomd/tiny_pdf.h"
#include "contents.h"
#include "markdown_parser.h"
#include "../common/path.h"

#include <string>
#include <string_view>
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
// it first, as mdBook prints it. The book has a table of contents with `options.toc`, or where the
// first file with a "[TOC]" paragraph has it, listing `options.tocDepth` levels (BookContents);
// each file has the page breaks of `options.pageBreakBeforeLevel` (BreakBeforeHeadings). Stops at
// the next file once `options.cancel` is set, the build then abandoned. From 256 KiB of text the
// files are parsed on up to six threads (one a CPU the process may run on), each under this
// thread's dialect and character references; the result is the same.
void ParseBookFiles(const std::vector<BookChapter>& chapters, const std::vector<std::string>& texts,
    const std::vector<std::string>& titles, const PdfOptions& options, BookFiles& files);

// The file of a book a link of file `from` names, std::string::npos for one outside it: `link`
// (UTF-8, percent-encoded as a URL is, without its "#fragment") as a path relative to the file, or
// a folder, whose README.md or index.md it names; in Obsidian's dialect also a note's name or the
// end of its path, in any case, wherever in the book it is (the first such file). `paths` are the
// files' paths, normalized (lexically_normal), none for a part.
size_t BookFileOfLink(const std::vector<BookChapter>& chapters, const std::vector<std::string>& paths, size_t from,
    std::string_view link);

// --backlinks (PdfOptions::backlinks): ends each of `texts`, the Markdown of `book`'s files, that
// other files link to with a paragraph "Linked from: [A](<a.md>), [B](<b.md>)", those files in
// the book's order, each by its title, else its file name without extension. The links resolve as
// any link between the book's files does. Read under the thread's dialect.
void AddBacklinks(const Book& book, std::vector<std::string>& texts);

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
// first; or the files given, in their order. Reads every file, each at most 128 MiB and all of
// them at most 128 MiB too, as much as one document. The files a SUMMARY.md lists or a directory
// holds must lie in that directory once links, junctions and ".." are resolved, so a book from
// elsewhere cannot put other files of the reader's into its PDF. False with `error` set when a
// file cannot be read, lies outside, the files pass the budget, or SUMMARY.md lists one twice,
// under its name or another (a link, other letter case): the same file, by device and inode or
// volume and file index.
bool ReadBook(const std::vector<RayoMd::File::Path>& inputs, TinyPdf::Book& book, std::string& error);

// At most this many notes in a book of linked notes.
constexpr size_t kMaxLinkedNotes = 1000;

// --follow-links=N: the book of `note` and the notes it links to, N (1 to 6) links deep, breadth
// first in the order the links come, each note once (by its file, whatever its names). Links are
// the Markdown links to relative ".md" and ".markdown" files of paragraphs, list items, quotes,
// tables, footnotes and linked images, and under options.dialect Obsidian's wiki links and embeds;
// each resolves through a Vault (vault.h) of options.vaultRoot, else of the note's folder, which
// the notes must lie in, and is that book's folder. A note other than the first gets its file
// name as its title. Past 128 MiB of notes or kMaxLinkedNotes notes, the walk stops: `leftOut`
// says how many notes it found and left out. False with `error` set when the note itself cannot
// be read, or options.cancel is set.
bool ReadLinkedBook(const RayoMd::File::Path& note, const TinyPdf::PdfOptions& options, int depth, TinyPdf::Book& book,
    size_t& leftOut, std::string& error);

} // namespace RayoMd::Book
