// SPDX-License-Identifier: Apache-2.0

#pragma once

// Footnotes at the foot of the page (PdfOptions::footnotes, FootnotePlacement::Page): the account
// the renderers keep of them. Each note is laid out once before the text to learn its height and
// the notes it refers to; a line that refers to notes not placed yet needs room for them below it,
// and they wait at the foot of its page until the page ends. A note that only other notes refer to
// goes with the first of them (a cluster, at most kClusterDepth notes deep). A cluster too tall for
// a page with its line is set after the text, as all notes are without this mode. The renderers
// lay out and paint; this unit holds the bookkeeping, the measured layouts' content and the layout
// steps both renderers share (PageNoteCore, over a NoteHost), built at -Os, outside tiny_pdf.cpp
// and its inlining budget. Every walk visits a note once, so the work stays linear in the notes.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "shared_url.h"

namespace TinyPdf::Internal {

class PageNotes {
public:
    // The rule above the notes and the space around it: 8 points above, 10 below.
    static constexpr double kRule = 18.0;
    static constexpr int kClusterDepth = 8;
    static constexpr size_t kNoTextEnd = static_cast<size_t>(-1);

    // Where a note's measuring layout ended in the buffers it wrote (its content, links, headings
    // and anchors): note N's start where note N - 1's ended.
    struct Ends {
        size_t content;
        uint32_t links;
        uint32_t marks;
        uint32_t anchors;
    };
    // A reference in a note: the note it refers to, its number's top while measuring.
    struct Ref {
        int note;
        double top;
    };

    // `count` notes, of which the text refers to 1 to `referenced`; `margin` and `bodySize` are
    // the page's, which the notes are set in.
    PageNotes(size_t count, size_t referenced, double margin, double bodySize);
    ~PageNotes();
    PageNotes(const PageNotes&) = delete;
    PageNotes& operator=(const PageNotes&) = delete;

    // Measuring, note after note from 1: the note being laid out refers to `note`, its number's
    // top at `top`; then its height, where its layout ended, and whether painting it again from
    // that layout (moved, Replayed) gives what a layout would.
    void BeginMeasure(int note);
    bool Measuring() const { return measuring != 0; }
    size_t Count() const { return noteCount; }
    void Refer(int note, double top);
    void EndMeasure(double height, const Ends& ends, bool replay);
    // What measuring found: a note's height, its references (RefBegin to RefEnd) and its
    // layout's ranges (from Begin, the last note's Ends, to EndsOf).
    double Height(int note) const { return info[Index(note)].height; }
    bool Replayed(int note) const { return info[Index(note)].replay; }
    const Ref* RefBegin(int note) const { return refs.data() + (note > 1 ? info[Index(note) - 1].refEnd : 0); }
    const Ref* RefEnd(int note) const { return refs.data() + info[Index(note)].refEnd; }
    Ends Begin(int note) const { return note > 1 ? info[Index(note) - 1].ends : Ends{}; }
    const Ends& EndsOf(int note) const { return info[Index(note)].ends; }
    // After measuring: the clusters taller than `room` with the rule go after the text.
    void Settle(double room);

    // A line's references: Waiting tells a note not placed yet, AddLine notes one. Collect then
    // leaves out of the line, for the end, the last clusters that an empty page of `room` beside
    // the line cannot hold, and returns the room the rest takes below the line, the rule included
    // when the page has no notes yet; Place puts them at the foot of the page as far as `room`
    // holds them, and returns the room they take. Collect only with a line that has references.
    bool Waiting(int note) const {
        return note > 0 && static_cast<size_t>(note) <= noteCount && info[Index(note)].state == kUnplaced;
    }
    void AddLine(int note);
    bool LineEmpty() const { return line.empty(); }
    double Collect(double room);
    double Place(double room);

    // The notes waiting at the foot of the page, in the order they are set.
    struct Notes {
        const int* first;
        const int* last;
        const int* begin() const { return first; }
        const int* end() const { return last; }
        bool empty() const { return first == last; }
    };
    Notes Pending() const { return { pending, pending + pendingCount }; }
    void ClearPending() { pendingCount = 0; }
    // Where the text of `page` ends, before its notes (NoteSourceBlock); kNoTextEnd for a page
    // without notes.
    void SetTextEnd(size_t page, size_t end);
    size_t TextEnd(size_t page) const { return page < textEnds.size() ? textEnds[page] : kNoTextEnd; }

    // After the text: the notes never placed go after it too. True when any note does.
    bool SettleRest();
    bool AtEnd(int note) const { return info[Index(note)].state == kEnd; }
    // The text refers to `note` (and not only other notes do).
    bool Body(int note) const { return static_cast<size_t>(note) <= referenced; }

    double margin;
    double bodySize;
    bool footMode = false;   // the notes are being laid out: no page breaks, no reservations
    // What the renderer's NewPage calls first (its own renderer as `renderer`): paints the page's
    // notes, or true where no page may end. Set by the renderer.
    bool (*pageEnds)(void* renderer) = nullptr;
    // The notes' measuring layouts, one after another (Ends::content), and where each began.
    std::string content;
    double measureTop = 0.0;

private:
    enum : uint8_t { kUnplaced, kFoot, kEnd };
    struct Info {
        double height;
        Ends ends;
        uint32_t refEnd;     // the note's references end at refs[refEnd]
        uint32_t walked;     // the walk that last met the note
        uint8_t state;
        bool replay;
    };

    static size_t Index(int note) { return static_cast<size_t>(note) - 1; }
    // `note` and, depth first, the notes alone (not referred to by the text) it refers to that wait
    // to be placed and this walk has not met, each once, into the members; returns their height.
    double Walk(int note, int depth);
    // Drops the line's last clusters, for the end, until the rest with the rule fits `room`.
    void Fit(double room);

    std::unique_ptr<Info[]> info;    // note N's at [N - 1], zeroed
    size_t noteCount;
    uint32_t walk = 0;
    std::vector<Ref> refs;
    std::vector<int> line;           // the line's references
    // Room for every note once, so that nothing grows while lines are laid out: the clusters of
    // the line's notes one after another (a walk meets a note once), where each cluster ends, and
    // the notes waiting at the foot of the page (a note waits once).
    std::unique_ptr<int[]> slots;
    std::unique_ptr<double[]> clusterHeights;
    int* members;
    int* clusterEnds;
    int* pending;
    size_t memberCount = 0;
    size_t clusterCount = 0;
    size_t pendingCount = 0;
    double clustered = 0.0;          // the clusters' heights
    std::vector<size_t> textEnds;
    size_t referenced;
    int measuring = 0;
};

} // namespace TinyPdf::Internal

namespace TinyPdf {

struct SourceBlock;
namespace Internal {
struct Block;
}

// Footnotes are set at this share of the body size.
constexpr double kNoteTextFactor = 0.85;

// A link's rectangle on its page.
struct LinkRect {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    Internal::SharedUrl url;   // the link's target, shared by all its rectangles
};

// A heading as drawn: its level, its text as the parser left it (WinAnsi bytes in a Latin
// document of the standard renderer, else UTF-8), its page and the top of its first line.
// The text lies in the document's blocks, which outlive the assembly of the file.
struct HeadingMark {
    int level = 1;
    std::string_view text;
    size_t page = 0;
    double top = 0.0;
};

// Where the footnotes are drawn: note N's text and its first reference, at [N - 1]; a top of
// kUnplaced where it is not drawn (not a negative one: a long page reaches below 0).
struct NoteMarks {
    static constexpr double kUnplaced = -HUGE_VAL;
    struct Point {
        size_t page = 0;
        double top = kUnplaced;
    };
    std::vector<Point> notes;
    std::vector<Point> references;

    void MarkReference(int note, size_t page, double top) {
        if (references.size() < (size_t)note) references.resize((size_t)note);
        Point& point = references[(size_t)note - 1];
        if (point.top == kUnplaced) point = { page, top };
    }
};

// Page footnotes: the links, headings and anchors of the notes' measuring layouts, which write
// straight into it as it takes the renderer's places while they run (Trade, before and after).
struct FootStash {
    std::vector<LinkRect> links;
    std::vector<HeadingMark> marks;
    std::vector<HeadingMark> anchors;

    FootStash() = default;
    FootStash(const FootStash&) = delete;
    FootStash& operator=(const FootStash&) = delete;
    ~FootStash();
    void Trade(std::string& content, std::string& notesContent, std::vector<LinkRect>& pageLinks,
        std::vector<HeadingMark>& headings, std::vector<HeadingMark>& ids);
};

struct PageNoteBase;

// What the shared steps ask a renderer to do (NoteHost::call): lay out note `index` from y, as the
// end section does (Note); trade the buffers its paint paths share for the notes' and back (Swap);
// end the page (NewPage); mark decoration (Artifact) or set aside its tags, glyph counts and
// fallback probe while notes are measured and put them back (Measure: `index` 1 to begin and 0 to
// end, `data` where they wait), neither asked of an untagged renderer; set the notes too tall for a
// page after the text (EndSection, `data` the PageNotes); lay out the document's blocks (Render,
// `data` their vector).
enum class NoteCall : uint8_t { Note, Swap, NewPage, Artifact, Measure, EndSection, Render };

// The renderer as the page footnotes' shared steps (PageNoteCore) see it: the fields both renderers
// have, by reference, and what each does its own way, through one call (tiny_pdf.cpp's
// PageNoteLayout fills it). One copy of those steps serves both renderers.
struct NoteHost {
    void* renderer;
    double& y;
    double& margin;
    double& textBottom;
    double& bodySize;
    double& lineHeight;
    double& pageW;
    double& pageH;
    double& fullH;
    int& listDepth;
    std::string_view& quoteBar;
    std::string_view plainQuoteBar;
    std::string_view ruleColor;
    std::vector<SourceBlock>*& sourceMap;
    std::string& content;
    std::vector<std::vector<LinkRect>>& pageLinks;
    std::vector<HeadingMark>& headings;
    std::vector<HeadingMark>& ids;
    NoteMarks& noteMarks;
    std::vector<size_t>& pageStarts;
    std::vector<double>*& pageBottoms;
    bool tagged;                                  // the renderer can be tagged (NoteCall::Artifact)
    void (*call)(void* renderer, NoteCall what, size_t index, const void* data);
    // What the measuring layout changes and takes back (images, tags, glyph counts): `begin` sets
    // it aside and returns whether the build is tagged (no note is replayed then), else puts it back.
    bool (*measure)(PageNoteBase& notes, bool begin);
    uint64_t (*imageMark)(PageNoteBase& notes);   // the images and failed images so far
    void (*rule)(std::string& content, std::string_view color, double x, double y, double length);
    // The build's PdfOptions::cancel, which BuildCancelled reads (tiny_pdf.cpp's thread-local
    // pointer, nullptr for none): a note's measuring layout points it at PageNoteBase::stop.
    const std::atomic<bool>*& cancel;
    const std::vector<Internal::Block>& noteBlocks;   // note N at [N - 1]
};

// A document's page footnotes: their account, the stash of their measuring layouts, the renderer.
struct PageNoteBase : Internal::PageNotes {
    PageNoteBase(const NoteHost& renderer, size_t count, size_t referencedNotes);
    ~PageNoteBase();
    FootStash stash;
    const NoteHost& host;
    // While a note is measured that may stop (Measure): the y below which it is too tall for a page
    // whatever follows, and what BuildCancelled reads meanwhile, set once its layout is below `cut`
    // (Break), so that its blocks stop.
    bool cutting = false;
    double cut = 0.0;
    std::atomic<bool> stop{ false };
};

// Footnotes at the foot of the page, for both renderers. Every note is laid out once before the
// text, where nothing breaks a page, for its height and the notes it refers to (Begin); a line of
// the cold paths with references to notes not placed yet keeps room for them below it (Reserve,
// RowNeed and PlaceRow, ReserveRow, after the renderer's AddLine); and the page's notes are painted
// at its foot when it ends (Break, from NewPage; End, at the end of a document and of each book
// file), down to the page margin, from their measuring layout (replayed) or laid out again with the
// renderer's state set aside, so that a paragraph or table row the page broke goes on as it was.
struct PageNoteCore {
    static void Begin(PageNoteBase& notes);
    static void End(PageNoteBase& notes);
    static bool Break(PageNoteBase& notes);
    static void Reserve(PageNoteBase& notes, double needed);
    static double RowNeed(PageNoteBase& notes, double needed);
    static void PlaceRow(PageNoteBase& notes, double needed);
    static void ReserveRow(PageNoteBase& notes, double needed, double need);
};

// PaintNote with page footnotes: a reference to `note` on `page`, its number's top at `top`. While
// measuring, an edge of the notes' graph; in a note at the foot, the first reference of a note only
// notes refer to, as a note the text refers to links back to the text.
void NoteReferenced(Internal::PageNotes& notes, NoteMarks& marks, int note, size_t page, double top);

} // namespace TinyPdf
