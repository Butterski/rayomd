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
#include "markdown_parser.h"
#include "math_layout.h"
#include "page_notes.h"

#include <cstddef>
#include <cstdint>
#include <string>
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

// What a build laid out, kept for the next one: its blocks, the state before each and after the
// last (marks), the page content as laid out (before --compress and --black-text), and what the
// renderer noted besides (pages, links, headings, anchors, the table of contents' lines, the source
// map, the CIDs and formula glyphs drawn, long pages' bottoms).
struct LayoutTape {
    uint8_t renderer = 0;                    // LayoutSession::Begin's `renderer`
    const void* fonts[kLayoutCidSets] = {};  // and its fonts
    std::vector<Block> blocks;
    std::vector<ContentsEntry> contents;
    std::vector<HeadingNumber> numbers;
    std::vector<LayoutMark> marks;           // before each block, and after the last
    std::string content;
    std::vector<size_t> pageStarts;
    std::vector<std::vector<LinkRect>> pageLinks;
    std::vector<HeadingMark> headings;
    std::vector<HeadingMark> ids;
    std::vector<ContentsLine> contentsLines;
    std::vector<SourceBlock> sourceMap;
    std::vector<uint32_t> sourceMapBlocks;   // the block of each entry of sourceMap
    std::vector<uint16_t> cids[kLayoutCidSets];   // in the order each set recorded them
    std::vector<MathGlyphUse> glyphs;        // LayoutMark::glyphs
    std::vector<double> pageBottoms;
    // Where each heading's and anchor's mark views its text: its top-level block and the offset in
    // that block's text, or kNested in a block inside it (Commit notes them while the blocks are
    // in the cache, so that a replay reads the old blocks for nested marks only).
    struct MarkPlace {
        static constexpr uint32_t kNested = 0xFFFFFFFFu;
        uint32_t block;
        uint32_t offset;
    };
    std::vector<MarkPlace> headingPlaces;
    std::vector<MarkPlace> idPlaces;

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
};

// The session of the build running on this thread (BuildSession::Build), or null.
LayoutSession* CurrentLayoutSession();

} // namespace Internal
} // namespace TinyPdf
