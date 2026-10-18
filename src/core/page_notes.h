// SPDX-License-Identifier: Apache-2.0

#pragma once

// Footnotes at the foot of the page (PdfOptions::footnotes, FootnotePlacement::Page): the account
// the renderers keep of them. Each note is laid out once before the text to learn its height and
// the notes it refers to; a line that refers to notes not placed yet needs room for them below it,
// and they wait at the foot of its page until the page ends. A note that only other notes refer to
// goes with the first of them (a cluster, at most kClusterDepth notes deep). A cluster too tall for
// a page with its line is set after the text, as all notes are without this mode. The renderers
// lay out and paint; this unit holds the bookkeeping and the measured layouts' content, built at
// -Os, outside tiny_pdf.cpp and its inlining budget. Every walk visits a note once, so the work
// stays linear in the notes.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
