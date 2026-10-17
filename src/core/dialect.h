// SPDX-License-Identifier: Apache-2.0

#pragma once

// Obsidian's Markdown (PdfOptions::dialect): what it adds to GitHub's is turned into what RayoMD
// reads before the document is parsed, line for line, except its callouts, which the parser reads
// itself. Built at -Os: documents of GitHub's Markdown never run it.

#include "rayomd/tiny_pdf.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace TinyPdf::Internal {

// The dialect of the document built on this thread, kept by BuildPdf and BuildBookPdf while they
// run (as ParseBudget is by ParseMarkdown); GitHub's outside one.
class DialectScope {
public:
    explicit DialectScope(MarkdownDialect dialect);
    ~DialectScope();
    DialectScope(const DialectScope&) = delete;
    DialectScope& operator=(const DialectScope&) = delete;

private:
    MarkdownDialect previous;
};
bool ObsidianDialect();

// Obsidian's syntax as the CommonMark and HTML RayoMD reads, in one pass, every line where it was
// (so a source map's lines stay): "%%comments%%" (also over lines) removed, "==text==" as
// "<mark>text</mark>", "[[note#heading|text]]" as a link to note.md#heading (to "#heading" without
// a note), "![[image.png|300]]" as an image (an HTML <img> of that width when it has a line of its
// own), and "![[note]]" as a link to the note. Fenced code, code spans, formulas, the front matter
// and "[[_TOC_]]" are left as they are.
std::string ObsidianToCommonMark(std::string_view markdown);

// ObsidianToCommonMark into `out` when `markdown` has any of what it rewrites ("[[", "==",
// "%%"); false, `out` untouched, without: the parser reads the document itself.
bool RewriteObsidian(std::string_view markdown, std::string& out);

// A callout: the first line of a quote "[!type]", with "+" or "-" (folded or not: both show
// unfolded) and a title, any of Obsidian's types (and GitHub's, in Obsidian's colours), taken out
// of `quote`. Returns the colour (Block::alert: 1 note, 2 success, 3 example, 4 warning, 5 danger,
// 6 tip, 7 quote; a type Obsidian does not know is a note) and sets `title` to the title's text, or
// to the type's name in title case without one; 0 when the line is no callout.
uint8_t TakeCallout(std::string& quote, std::string& title);

} // namespace TinyPdf::Internal
