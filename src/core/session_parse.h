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

// Sets the document parse of this thread's next depth-0 ParseMarkdownImpl with footnotes, the
// document's (markdown_parser.cpp); null for none. Its block loop's end clears it.
void SetDocumentParse(DocumentParse* parse);
// Where the document's parse takes `kinds` (the class of each line after ResolveMathOpeners) and the
// lines the definitions take (its `suppressed`).
std::vector<uint8_t>& DocumentKinds(DocumentParse& parse);
std::vector<unsigned char>& DocumentSuppressed(DocumentParse& parse);
// The parser has read the document's lines (`lines`, views of `text`), their classes (in
// DocumentKinds), the lines the definitions take (`suppressed`, empty without "]:"), the link
// definitions and the footnotes defined at the top level; its block loop would start at line
// `first` (after a front matter). Returns the line the loop starts at: `first`, or a line past it
// where the last build's parse was in the state this one is, from which the blocks before it are
// that parse's. Sets the ParseBudget for that line.
size_t BeginDocumentParse(DocumentParse& parse, const std::string& text, const std::vector<std::string_view>& lines,
    const std::vector<unsigned char>& suppressed, const ReferenceDefinitions& definitions, bool definitionSyntax,
    size_t first, const FootnoteParse& notes);
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
