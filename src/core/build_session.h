// SPDX-License-Identifier: Apache-2.0

#pragma once

// Builds of a document that changes a little between them (TinyPdf::BuildSession, an editor's
// preview): the layout of the last build kept as a tape, and the runs of top-level blocks whose
// layout a new build takes from it instead of laying them out again. The renderers' side (the
// block loop, what a renderer's state is between two blocks) is in tiny_pdf.cpp, which knows the
// renderers; the rest, comparing the blocks, keeping the tape and replaying it, is here, in
// build_session.cpp, at -Os and out of tiny_pdf.cpp's inlining budget. The design and its rules:
// docs/development/incremental_builds.md.

#include "rayomd/tiny_pdf.h"
#include "contents.h"
#include "inline_markdown.h"
#include "markdown_parser.h"
#include "math_layout.h"
#include "page_notes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf {

// Where an entry of the table of contents ends: the page, baseline and right end of its last line,
// for the page number and the dots the builder adds to it (ContentsNumbers).
struct ContentsLine {
    size_t entry = 0;
    size_t page = 0;
    double baseline = 0.0;
    double end = 0.0;
};

namespace Internal {

// The block loop of a document-level parse at the start of one of its passes (ParseMarkdownImpl):
// the line, the blocks made before it, what of the ParseBudget it had used, and whether no HTML
// element was open (Html::HtmlState), which with the padded cells used is all of the loop's state
// that a later pass reads.
struct ParsePass {
    uint32_t line;
    uint32_t blocks;
    uint64_t referenceBytes;   // reference destination bytes used since the loop began
    uint32_t paddedCells;      // empty table cells used since the loop began
    uint32_t closed;           // 1 when no HTML element was open
};

// The lines of a session's parse and their classes (session_parse.cpp), which the next parse shifts
// past its edit.
struct ParseLines;
struct ParseLinesFree {
    void operator()(ParseLines* lines) const;
};

// A text a session keeps from build to build (SessionText: the markdown and its WinAnsi text),
// changed in place where the next build's differs, so that the bytes before and after the change
// stay where they were: a parse's record views it instead of copying it, and the next parse knows
// the change instead of comparing the texts.
struct KeptText {
    std::string bytes;
    uint64_t version = 0;    // of `bytes`, never the same for other bytes in the session; 0 for none
    // The version `bytes` were before the last change, 0 when not known; the change kept their
    // [0, differs) and their bytes from oldEnd on, from newEnd on now.
    uint64_t base = 0;
    size_t differs = 0;
    size_t oldEnd = 0;
    size_t newEnd = 0;
    size_t pairs = 0;        // "]:" in `bytes`, of a link or footnote definition

    // Bytes [from, to) become `with`, version `next`; Undo puts them back.
    void Change(size_t from, size_t to, std::string_view with, uint64_t next);
    // All of the bytes become `with` (Take swaps them with it), version `next`, which Undo cannot put back.
    void Set(std::string_view with, uint64_t next);
    void Take(std::string& with, uint64_t next);
    // The bytes before the last Change or Set again: the same version when it can, else `fresh`.
    void Undo(uint64_t fresh);
    // What the last Change replaced.
    std::string_view Replaced() const { return replaced; }

private:
    std::string replaced;    // what the last Change replaced
    size_t replacedPairs = 0;
    bool undoable = false;
};

// What a session's document-level parse leaves for the next (session_parse.h): the text it read,
// its lines, each line's class and definition mark, the link definitions, and every pass of its
// block loop.
struct ParseRecord {
    std::string own;                 // the text it read, when that is not one the session keeps
    std::string_view text;           // the text it read: `own`, or a KeptText's bytes when it read them
    const KeptText* kept = nullptr;  // that KeptText, whose `version` the text was
    uint64_t version = 0;
    std::unique_ptr<ParseLines, ParseLinesFree> lines;   // views of `text`
    std::vector<uint8_t> kinds;
    std::vector<unsigned char> suppressed;
    ReferenceDefinitions definitions;
    std::vector<ParsePass> passes;
    size_t first = 0;               // the line the block loop began at (after a front matter)
    size_t blocks = 0;
    uint64_t referenceBytes = 0;    // used by the whole loop
    size_t referenceLeft = 0;       // left when it ended
    size_t paddedCellsAtStart = 0;  // the budget's when the loop began
    bool definitionSyntax = false;
    bool winAnsi = false;           // WinAnsiReferences::Active()
};

// The KeptText whose bytes `text` is, in the session of this thread's build; else null.
const KeptText* KeptTextOf(const std::string& text);

// A session's blocks: the last build's, which the next one's parse keeps where the edit left them
// alone (ParseInSession, session_parse.h). The builders parse into `blocks` (BuildBlocks); until
// the build is committed, `displaced` holds the last build's blocks the parse took out, so that the
// layout compares with them (LayoutSession) and a build that does not finish puts them back.
struct SessionBlocks {
    std::vector<Block> blocks;
    std::vector<Block> displaced;
    std::unique_ptr<ParseRecord> record;    // of the last committed parse, or null
    std::unique_ptr<ParseRecord> pending;   // of this build's, or null (not to be reused)
    std::unique_ptr<ParseRecord> spare;     // the memory of an older one
    bool reusable = false;   // the options leave the parse's blocks as they are (no --toc, page breaks)
    bool parsed = false;     // this build parsed: committed or put back at its end
    size_t made = 0;         // the blocks the last parse made (BuildSession::Reuse::parsed)
    // The parse kept the last build's blocks [0, front), took out its [front, front + oldWindow)
    // into `displaced`, made [front, front + newWindow), and kept the last build's after them,
    // their source lines moved by lineShift. Else (whole) `displaced` holds the last build's.
    bool spliced = false;
    size_t front = 0;
    size_t oldWindow = 0;
    size_t newWindow = 0;
    int64_t lineShift = 0;

    // The last build's block `index`.
    const Block& Old(size_t index) const {
        if (!spliced) return displaced[index];
        if (index < front) return blocks[index];
        if (index < front + oldWindow) return displaced[index - front];
        return blocks[index - oldWindow + newWindow];
    }
    // Whether block `index` is the last build's block `old` itself.
    bool Kept(size_t index, size_t old) const {
        return spliced && (index < front ? old == index : index >= front + newWindow && old + newWindow == index + oldWindow);
    }
    // The number of the last build's blocks.
    size_t OldCount() const { return spliced ? blocks.size() - newWindow + oldWindow : displaced.size(); }
};

// This build's parse is the session's: the displaced blocks go, the record is the next build's.
void CommitParse(SessionBlocks& session);
// This build's parse did not make a build: the last build's blocks and record are as they were.
void UndoParse(SessionBlocks& session);

// The Unicode renderer's CID sets: its font's and a theme's bold, italic and bold italic faces'.
constexpr size_t kLayoutCidSets = 4;

// A renderer's state between two top-level blocks, as far as the layout of the blocks after them
// reads it, and the sizes of what it made so far.
struct LayoutMark {
    // LayoutMark::flags.
    static constexpr uint8_t kPageStarted = 1;    // the page has content (RenderPageBreak breaks it)
    static constexpr uint8_t kPending = 2;        // a PageBreak block came last
    static constexpr uint8_t kMathActive = 4;     // MathPool::Active()
    static constexpr uint8_t kMathUsed = 8;       // MathPool::Used()
    static constexpr uint8_t kMathLaidOut = 16;   // MathPool::LaidOut()
    static constexpr uint8_t kContentsDrawn = 32; // the table of contents is drawn (RenderContents)
    // The renderer's settings that blocks change and set back (margin, sizes, list depth, the quote
    // bar, ...) are as the build began: a layout may start or end here.
    static constexpr uint8_t kResumable = 64;

    double y = 0.0;
    size_t content = 0;          // page content bytes
    uint32_t page = 0;           // the page, from 0
    uint32_t links = 0;          // the links on that page
    uint32_t headings = 0;
    uint32_t ids = 0;
    uint32_t contentsLines = 0;
    uint32_t sourceBlocks = 0;   // entries of the source map
    uint32_t nextNumber = 0;     // --number-headings: the next number to draw
    uint32_t glyphs = 0;         // the formulas' glyphs: an index into LayoutTape::glyphs
    uint32_t bottoms = 0;        // long pages: the bottoms noted
    uint32_t cids[kLayoutCidSets] = {};      // CIDs each set recorded
    uint32_t missing[kLayoutCidSets] = {};   // characters each drew as '?'
    uint32_t missingBmp = 0;     // of them in the BMP, in the document's font (ProbeFallback)
    uint8_t facesUsed = 0;       // the standard renderer's faces (StandardRenderer::facesUsed)
    uint8_t flags = 0;
};

// What a build laid out, kept for the next one: the state before each of its blocks and after the
// last (marks; the blocks are the session's, SessionBlocks), the page content as laid out (before
// --compress and --black-text, without the gaps between pages, in pieces of buffers that later
// tapes share), and what the renderer noted besides (pages, links, headings, anchors, the table of
// contents' lines, the source map, the CIDs and formula glyphs drawn, long pages' bottoms).
struct LayoutTape {
    uint8_t renderer = 0;                    // LayoutSession::Begin's `renderer`
    const void* fonts[kLayoutCidSets] = {};  // and its fonts
    size_t blockCount = 0;
    const Block* blockData = nullptr;        // where its blocks were (a block that did not move keeps its text's place)
    std::vector<ContentsEntry> contents;
    std::vector<HeadingNumber> numbers;
    std::vector<const std::string*> blockNumbers;   // --number-headings: each block's, in `numbers`
    std::vector<LayoutMark> marks;           // before each block, and after the last
    // The page content without the gaps: piece i holds its bytes [at, pieces[i + 1].at), from `data`
    // in buffers[buffer]; a last piece with no data ends it.
    struct Piece {
        size_t at;
        const char* data;
        uint32_t buffer;
    };
    std::vector<Piece> pieces;
    std::vector<std::shared_ptr<std::string>> buffers;
    std::vector<size_t> pageStarts;
    std::vector<size_t> pageFree;            // where each page's content starts without the gaps; then its end
    // Where each page's content lies in the PDF the build made (BuildInto put it there); empty when
    // that is not known (--compress and --black-text rewrite it, and leave no gaps).
    std::vector<size_t> pageFile;
    std::vector<std::vector<LinkRect>> pageLinks;
    std::vector<HeadingMark> headings;
    std::vector<HeadingMark> ids;
    std::vector<ContentsLine> contentsLines;
    std::vector<SourceBlock> sourceMap;
    std::vector<uint32_t> sourceMapBlocks;   // the block of each entry of sourceMap
    std::vector<uint16_t> cids[kLayoutCidSets];   // in the order each set recorded them
    std::vector<MathGlyphUse> glyphs;        // LayoutMark::glyphs
    std::vector<double> pageBottoms;
    std::vector<uint8_t> images;             // 1 for each block that reads an image or draws a diagram (TouchesImages)
    // For each page, the last build's page whose content a replay put on it, LayoutSession::kNoPage
    // where none did (and past the end), for the assembly (AssemblyTape).
    std::vector<uint32_t> pageOrigins;
    // Where each heading's and anchor's mark views its text: its top-level block and the offset in
    // that block's text, or kNested in a block inside it (noted as the layout goes, while the
    // block is in the cache, so that a replay reads the old blocks for nested marks only).
    struct MarkPlace {
        static constexpr uint32_t kNested = 0xFFFFFFFFu;
        uint32_t block;
        uint32_t offset;
    };
    std::vector<MarkPlace> headingPlaces;
    std::vector<MarkPlace> idPlaces;

    void Clear();
    // Room for what `like` holds, and some.
    void Reserve(const LayoutTape& like);
};

// What a session's build assembled of its PDF that the next one takes again, byte for byte, where
// what made it is the same (tiny_pdf.cpp's SessionAnnotations, OutlineMemo, SessionPages): the
// external link annotations with their rectangles and targets, the outline's entries with their
// titles and the objects they name, and the page dictionaries. A session keeps two: the last
// build's, read, and this build's, written, whose bytes the PDF's objects view until BuildInto.
struct AssemblyTape {
    struct Annotation {
        LinkRect link;    // holds the target's text, by whose address it is compared
        size_t index;     // in its page's links
        size_t end;       // of its bytes in `annotations`
    };
    struct Entry {
        uint64_t top;     // the bits of its heading's top
        int ids[6];       // its parent, previous, next, first and last entry (0 for none), its page
        size_t count;     // the entries under it
        size_t titleEnd;  // of its title, as the heading gave it, in `titles`
        size_t end;       // of its bytes in `outline`
    };
    // A view of `annotations` or `pages` that object `id` gets once they no longer grow.
    struct Pending {
        int id;
        size_t begin;
        size_t end;
    };
    bool winAnsi = false;                    // the targets and titles were WinAnsi text
    bool print = false;                      // /F 4 on each annotation
    std::vector<Annotation> annotationList;  // external links of fewer than kSharedActionUrlBytes
    std::vector<size_t> pageAnnotations;     // the first of each page's, then their end
    std::string annotations;
    std::vector<Entry> entries;
    size_t entriesStart = 0;                 // where the entries' bytes begin in `outline`, after its root's
    std::string titles;
    std::string outline;                     // AddOutline's storage
    std::string pagePrefix;                  // a page dictionary before its /Contents, the same on every page
    std::string pages;
    std::vector<Pending> pending;

    void Clear();
};

// A renderer's CID set as the session sees it (tiny_pdf.cpp's UsedCidSet): null `set` for none.
struct LayoutCids {
    void* set = nullptr;
    bool (*has)(const void* set, uint16_t cid) = nullptr;
    void (*add)(void* set, uint16_t cid) = nullptr;
    // The CIDs recorded, in their order (the set sorts them only for the font, after the layout).
    const std::vector<uint16_t>& (*recorded)(const void* set) = nullptr;
    uint32_t* missing = nullptr;
    uint32_t* missingBmp = nullptr;
};

// The renderer's containers that a replayed run appends to (tiny_pdf.cpp's SessionTarget).
struct LayoutTarget {
    std::string* content = nullptr;
    std::vector<size_t>* pageStarts = nullptr;
    std::vector<std::vector<LinkRect>>* pageLinks = nullptr;
    std::vector<HeadingMark>* headings = nullptr;
    std::vector<HeadingMark>* ids = nullptr;
    std::vector<ContentsLine>* contentsLines = nullptr;
    std::vector<double>* pageBottoms = nullptr;   // long pages only
    MathGlyphUse* glyphs = nullptr;
    LayoutCids cids[kLayoutCidSets];
    bool probing = false;                         // Renderer::ProbeFallback
    // A new page's content starts after the gap the builder leaves (tiny_pdf.cpp's StartPageContent),
    // `gapBefore(page)` bytes long.
    void (*startPage)(std::string& content, std::vector<size_t>& pageStarts) = nullptr;
    size_t (*gapBefore)(size_t page) = nullptr;
    // Makes `content` `size` bytes long without writing what lies past its old size (tiny_pdf.cpp's
    // ResizeForOverwrite), or null where the string fills it.
    void (*grow)(std::string& content, size_t size) = nullptr;
    // Commit: after BuildInto, the bytes it put before each page's content after the one before
    // (PdfObjects' gaps), or null when it left no gaps.
    const std::vector<size_t>* fileGaps = nullptr;
};

class LayoutSession;

// tiny_pdf.cpp's side of a session's build, for each renderer ([0] the standard fonts', [1] the
// TrueType one's), which it is given as a pointer: the layout a block at a time with runs from the
// last build (RenderInSession), its end before the builder changes the content, and the commit at
// the end of the build. The builders reach it only through a session (LayoutSession::calls, from
// SessionRendererCalls), so that executables that make no session, such as the command lines, leave
// all of it out (--gc-sections).
struct SessionCalls {
    void (*render[2])(void* renderer, const std::vector<Block>& blocks, const Footnotes& footnotes,
        const std::vector<ContentsEntry>& contents, const std::vector<HeadingNumber>& numbers, bool notesAtFoot,
        LayoutSession& session, uint8_t kind);
    void (*endLayout[2])(void* renderer, LayoutSession& session, std::vector<SourceBlock>* sourceMap);
    void (*commit[2])(void* renderer, LayoutSession& session, std::vector<Block>& blocks,
        std::vector<ContentsEntry>& contents, std::vector<HeadingNumber>& numbers);
};

// Defined in tiny_pdf.cpp; called by build_session.cpp alone.
SessionCalls SessionRendererCalls();

// The layout side of a BuildSession, which the builders in tiny_pdf.cpp reach through
// CurrentLayoutSession(). Abstract, so that the command lines, which make no session, link none of
// build_session.cpp.
class LayoutSession {
public:
    virtual ~LayoutSession() = default;

    SessionCalls calls{};
    SessionBlocks* store = nullptr;   // the session's blocks, which its builds parse into

    // A build's layout of `blocks` begins, by renderer `renderer` (0 the standard fonts, 1 the same in
    // WinAnsiEncoding, 2 a TrueType font) in `fonts`, with the table of contents' `contents` and the
    // headings' `numbers`: compares them with the last build's to know which blocks it may replay.
    virtual void Begin(uint8_t renderer, const void* const fonts[kLayoutCidSets], const std::vector<Block>& blocks,
        const std::vector<ContentsEntry>& contents, const std::vector<HeadingNumber>& numbers) = 0;
    // The source map the layout writes into (all builds of a session note it).
    virtual std::vector<SourceBlock>& SourceMap() = 0;
    // Notes `mark`, the renderer's state before block `index` (after the last for the block count),
    // completing it with the sizes of what `target` holds.
    virtual void Mark(size_t index, LayoutMark& mark, const LayoutTarget& target) = 0;
    // The end of the run of blocks from `index` (whose state Mark just noted) that the last build laid
    // out from this same state and that may be replayed from its tape; `index` when there is none.
    virtual size_t Replayable(size_t index, const LayoutTarget& target) = 0;
    // Replays blocks [index, end) that Replayable gave into `target` and notes their marks; returns
    // the state after them, which the caller gives the renderer (y, the flags, ...).
    virtual LayoutMark Replay(size_t index, size_t end, LayoutTarget& target) = 0;
    // The layout is done: keeps its content and CIDs before the builder changes them, and gives the
    // source map to `sourceMap` when there is one.
    virtual void EndLayout(const LayoutTarget& target, std::vector<SourceBlock>* sourceMap) = 0;
    // The build is made: the tape takes what the next build replays.
    virtual void Commit(LayoutTarget& target, std::vector<Block>& blocks, std::vector<ContentsEntry>& contents,
        std::vector<HeadingNumber>& numbers) = 0;
    // A build of this session that is not laid out by blocks (footnotes, PDF/UA): no tape.
    virtual void Forget() = 0;
    // ChooseFonts (tiny_pdf.cpp) for `markdown`, past what the options choose: 0 the standard fonts
    // (ASCII), 1 the same in WinAnsiEncoding, with `winAnsi` the text transcoded, 2 a TrueType font;
    // kNotKept when `markdown` is not the text the session keeps, which the caller then looks at.
    static constexpr uint8_t kNotKept = 0xFF;
    virtual uint8_t TextFonts(const std::string& markdown, const std::string*& winAnsi) = 0;
    // The assembly the session's last build left (null for none) and the one this build writes,
    // emptied; both null outside a layout by blocks (Forget).
    virtual const AssemblyTape* LastAssembly() const = 0;
    virtual AssemblyTape* NextAssembly() = 0;
    // The last build's page whose content page `page` of this build took by a replay, or kNoPage.
    static constexpr uint32_t kNoPage = 0xFFFFFFFFu;
    virtual uint32_t PageOrigin(size_t page) const = 0;
};

// The session of the build running on this thread (BuildSession::Build), or null. Builds without
// RAYOMD_BUILD_SESSION (the command lines, CMakeLists.txt) have none: their builders keep no session
// code at all, as MinGW's linker keeps unused functions.
#ifdef RAYOMD_BUILD_SESSION
LayoutSession* CurrentLayoutSession();
// The blocks a builder parses into: the session's in a session's build of a document, else `own`.
inline std::vector<Block>& BuildBlocks(std::vector<Block>& own, const void* book) {
    LayoutSession* const session = CurrentLayoutSession();
    return session != nullptr && book == nullptr ? session->store->blocks : own;
}
// Whether this build is a session's whose parse is kept for the next build: only one without a
// paragraph that may be a contents marker is (ParseInSession), where PlaceContents finds none.
bool SessionParseUnmarked();
#else
inline LayoutSession* CurrentLayoutSession() { return nullptr; }
#endif

} // namespace Internal
} // namespace TinyPdf
