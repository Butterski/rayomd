// SPDX-License-Identifier: Apache-2.0

// The document level of a BuildSession's parse (session_parse.h): markdown_parser.cpp compiled a
// second time, in the engine library only (RAYOMD_BUILD_SESSION), for ParseSessionDocument, the
// level parse with the session's hooks (markdown_parser_level.inc with RAYOMD_SESSION_LEVEL 1). A
// second copy in markdown_parser.cpp itself, or the hooks in ParseMarkdownImpl, changed what GCC
// made of the parse every other build runs (+0.4 to 2 % instructions on BuildPdf); here that one is
// the same code as without sessions. The externally visible functions of the copy are renamed, so
// that they do not clash with markdown_parser.cpp's; only ParseSessionDocument is used.

#define SplitLines SessionCopySplitLines
#define StripInlineMarkdown SessionCopyStripInlineMarkdown
#define StripInlineMarkdownKeepMath SessionCopyStripInlineMarkdownKeepMath
#define ParseImageAt SessionCopyParseImageAt
#define ParseMarkdownLevel SessionCopyParseMarkdownLevel
#define ParseMarkdown SessionCopyParseMarkdown
#include "markdown_parser.cpp"

namespace TinyPdf::Internal {

static_assert(static_cast<uint8_t>(LineKind::Empty) == kEmptyLineKind, "session_parse.h's kEmptyLineKind");

namespace {

// The document parse of a session's build on this thread, which ParseMarkdownLevel hands to
// ParseSessionDocument; null for every other parse.
thread_local DocumentParse* tSessionDocument = nullptr;

// BeginDocumentParse with the lines' classes.
size_t BeginInSession(const std::string& markdown, const std::vector<std::string_view>& lines,
    const std::vector<LineInfo>& infos, const unsigned char* suppressed, size_t suppressedSize,
    const ReferenceDefinitions& definitions, bool definitionSyntax, size_t first, const FootnoteParse* notes) {
    DocumentParse& session = *tSessionDocument;
    std::vector<uint8_t>& kinds = DocumentKinds(session);
    kinds.resize(infos.size());
    for (size_t index = 0; index < infos.size(); index++) kinds[index] = static_cast<uint8_t>(infos[index].kind);
    std::vector<unsigned char>& marks = DocumentSuppressed(session);
    marks.assign(suppressed, suppressed + suppressedSize);
    return BeginDocumentParse(session, markdown, lines, marks, definitions, definitionSyntax, first, *notes);
}

// EndDocumentParse, after which the thread has no document parse.
void EndInSession(std::vector<Block>& blocks, size_t line, ReferenceDefinitions& definitions, const FootnoteParse* notes) {
    DocumentParse& session = *tSessionDocument;
    tSessionDocument = nullptr;
    EndDocumentParse(session, blocks, line, definitions, *notes);
}

} // namespace

void SetDocumentParse(DocumentParse* parse) {
    tSessionDocument = parse;
}

bool SessionDocumentSet() {
    return tSessionDocument != nullptr;
}

std::vector<Block> ParseSessionDocument(const std::string& markdown, int depth, FootnoteParse* notes) {
#define RAYOMD_SESSION_LEVEL 1
#include "markdown_parser_level.inc"
#undef RAYOMD_SESSION_LEVEL
}

} // namespace TinyPdf::Internal
