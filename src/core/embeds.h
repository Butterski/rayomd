// SPDX-License-Identifier: Apache-2.0

#pragma once

// Note embeds of Obsidian's dialect: "![[note]]" alone on a line of a document's top level prints
// that note (or its "#heading" section or "#^id" block) in its place. BuildPdf reads the notes
// before the build (ResolveEmbeds, through a Vault, vault.h) and keeps them for its thread
// (EmbedScope); the parser splices each one's blocks in at its line (SpliceEmbeds). Built at -Os:
// documents without such an embed never run it.

#include "rayomd/tiny_pdf.h"
#include "markdown_parser.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace TinyPdf::Internal {

struct FootnoteParse;

// What an embed brings in: a note, or a section or block of it.
struct EmbedEntry {
    // Its Markdown, as written (UTF-8; transcoded to WinAnsi when the standard fonts show the
    // document): the front matter cut, its footnote labels made its own ("[^1]" as "[^1~e3]").
    // Empty for entry 0, the document, which the builder parses itself.
    std::string text;
    // EmbedSet::folders: where its relative images resolve from (EmbedImageBase gives its images
    // this plus one).
    uint32_t folder = 0;
    // The lines of `text` (0-based, as the parser counts them) that embed a note, each with the
    // entry it brings in, in line order.
    std::vector<std::pair<uint32_t, uint32_t>> embeds;
};

// The notes a document embeds, all of them read before the build.
struct EmbedSet {
    std::vector<EmbedEntry> entries;              // [0]: the document
    std::vector<std::filesystem::path> folders;   // the entries' notes' folders, resolved
    std::filesystem::path root;                   // the vault, which local images must lie in
    std::string all;                              // the document's text and every entry's, for the choice of fonts
    // The images of the entries' blocks in the build's parse, by address, and their folder plus
    // one: a side table, so that ImageSource and the parser stay as they are for every other
    // document (a field in ImageSource moved what GCC inlined into ParseMarkdownImpl).
    mutable std::unordered_map<const ImageSource*, uint32_t> imageBases;

    // At most this many embeds, nested ones included, and this many bytes of embedded text a
    // build, and embeds this many levels deep; an embed past a limit stays a link.
    static constexpr size_t kMaxEmbeds = 1000;
    static constexpr size_t kMaxBytes = size_t(32) << 20;
    static constexpr int kMaxDepth = 8;
};

// Whether BuildPdf looks for note embeds in `markdown` (Obsidian's dialect): with a source path,
// not options.embedsAsLinks, and "![[" in the text. Nothing else is read for other documents.
bool MayEmbedNotes(const std::string& markdown, const PdfOptions& options);

// Reads the notes `markdown`'s standalone embeds name, and theirs, into `set` (its `all` too);
// false when none could be read, the document then built as it is. Stops early, with what it has,
// once options.cancel is set.
bool ResolveEmbeds(const std::string& markdown, const PdfOptions& options, EmbedSet& set);

// The set the build on this thread embeds, while it runs (BuildPdf); null outside one.
class EmbedScope {
public:
    explicit EmbedScope(const EmbedSet& set);
    ~EmbedScope();
    EmbedScope(const EmbedScope&) = delete;
    EmbedScope& operator=(const EmbedScope&) = delete;

private:
    const EmbedSet* previous;
};
const EmbedSet* CurrentEmbeds();

// The folder an image's base (EmbedImageBase, 1 and up) names in this thread's build, null for none.
const std::filesystem::path* EmbedFolder(uint32_t base);
// 0 for an image of the document, which resolves from its folder; for one of a note it embeds,
// that note's folder plus one.
uint32_t EmbedImageBase(const ImageSource& image);

// Whether this thread's build embeds notes (EmbedScope), and ParseWithFootnotes of its document
// (already rewritten from Obsidian's dialect) then: its embed lines made empty HTML comments, so
// each ends the block before it and gives none, and the notes' blocks spliced in there.
bool EmbedsActive();
std::vector<Block> ParseWithEmbeds(const std::string& markdown, Footnotes& footnotes);

// `blocks`, the top-level parse of `entry`'s text so made, with the blocks of each entry its
// embeds bring in at their lines: parsed at depth 0 as the document is (their own link
// definitions, the document's ParseBudget and footnotes), each stamped with the embed's line,
// their images resolving from their note's folder, their own embeds spliced in too.
void SpliceEmbeds(std::vector<Block>& blocks, const EmbedEntry& entry, FootnoteParse* notes);

} // namespace TinyPdf::Internal
