// SPDX-License-Identifier: Apache-2.0

#pragma once

// The parser's side of a BuildSession (build_session.h): the document-level parse of a session's
// build goes on from where the last build's parse was in the same state, and takes the blocks of
// the lines before and after the edit from it. ParseMarkdownImpl calls these at depth 0 only when
// ParseInSession (build_session.cpp) asked for it, and only in builds with RAYOMD_BUILD_SESSION:
// the command lines carry none of it. The rules: docs/development/incremental_builds.md.

#include "inline_markdown.h"
#include "markdown_parser.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

struct FootnoteParse;

// A line's class as the parser's top level reads it (markdown_parser.cpp's LineKind); 1 is Empty.
constexpr uint8_t kEmptyLineKind = 1;

// One document-level parse of a session's build (build_session.cpp).
class DocumentParse;

// Sets the document parse of this thread's next depth-0 ParseMarkdownLevel with footnotes, the
// document's; null for none. Its block loop's end clears it (session_parse.cpp).
void SetDocumentParse(DocumentParse* parse);
// Whether one is set: ParseMarkdownLevel then parses the document with ParseSessionDocument, the
// level parse with the session's hooks, compiled apart (session_parse.cpp).
bool SessionDocumentSet();
std::vector<Block> ParseSessionDocument(const std::string& markdown, int depth, FootnoteParse* notes);
// The first byte at which `a` and `b`, of `size` bytes each, differ; `size` for none.
size_t FirstDifference(const char* a, const char* b, size_t size);
// How many bytes the texts that end at `aEnd` and `bEnd` end with alike, at most `limit`.
size_t CommonEnd(const char* aEnd, const char* bEnd, size_t limit);
struct ParseRecord;
// The record of the last build's parse that `parse` goes on from (null for a whole parse), and the
// one it leaves: its text (the document's, which the parse's lines view), lines, `kinds` (the class
// of each line after ResolveMathOpeners) and `suppressed` (the lines the definitions take).
const ParseRecord* LastRecord(const DocumentParse& parse);
ParseRecord& NextRecord(DocumentParse& parse);
// How the lines of a parse stand to the last's (session_parse.cpp's ClassifyInSession), when
// `known`: the texts' first `differs` bytes and last `same` are alike, and only lines
// [classFirst, classEnd) may have another class or definition mark than the last parse's line in
// that place (those before them the same line, those after it the line as many lines before or
// after as the edit added).
struct LineChange {
    bool known = false;
    size_t differs = 0;
    size_t same = 0;
    size_t classFirst = 0;
    size_t classEnd = 0;
};
// The parser has read the document's lines (`lines`, views of NextRecord's text), their classes
// (NextRecord's kinds), the lines the definitions take (`suppressed`, empty without "]:"), the link
// definitions and the footnotes defined at the top level; its block loop would start at line
// `first` (after a front matter). Returns the line the loop starts at: `first`, or a line past it
// where the last build's parse was in the state this one is, from which the blocks before it are
// that parse's. Sets the ParseBudget for that line.
size_t BeginDocumentParse(DocumentParse& parse, const std::vector<std::string_view>& lines,
    const std::vector<unsigned char>& suppressed, const ReferenceDefinitions& definitions, bool definitionSyntax,
    size_t first, const LineChange& change, const FootnoteParse& notes);
// The document's block loop starts a pass at line `line` with `blocks` blocks made since it began,
// no HTML element open when `closed`: true when the last build's parse was there in the same
// state, so that the blocks from there on are that parse's (the loop stops).
bool DocumentPass(DocumentParse& parse, size_t line, size_t blocks, bool closed);
// The document's loop ended at line `line` with `blocks`: the document's blocks, in place, those of
// the last build's parse spliced around them. Takes `definitions` for the next build's comparison.
void EndDocumentParse(DocumentParse& parse, std::vector<Block>& blocks, size_t line, ReferenceDefinitions& definitions,
    const FootnoteParse& notes);

// Whether a BuildSession's build runs on this thread, whose ParseMarkdown is ParseInSession.
bool SessionParses();
// ParseMarkdown(markdown, &footnotes) of a session's build: the blocks are its (build_session.h).
std::vector<Block> ParseInSession(const std::string& markdown, Footnotes& footnotes);

} // namespace TinyPdf::Internal
