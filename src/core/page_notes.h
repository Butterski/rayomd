// SPDX-License-Identifier: Apache-2.0

#pragma once

// Footnotes at the foot of the page (PdfOptions::footnotes, FootnotePlacement::Page): the account
// the renderers keep of them. Each note is laid out once before the text to learn its height and
// the notes it refers to; a line that refers to notes not placed yet needs room for them below it,
// and they wait at the foot of its page until the page ends. A note that only other notes refer to
// goes with the first of them (a cluster, at most kClusterDepth notes deep). A cluster too tall for
// a page with its line is set after the text, as all notes are without this mode. The renderers
// lay out and paint; this unit holds only the bookkeeping, built at -Os, outside tiny_pdf.cpp and
// its inlining budget. Every walk visits a note once, so the work stays linear in the notes.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace TinyPdf::Internal {

class PageNotes {
public:
    // The rule above the notes and the space around it: 8 points above, 10 below.
    static constexpr double kRule = 18.0;
    static constexpr int kClusterDepth = 8;
    static constexpr size_t kNoTextEnd = static_cast<size_t>(-1);

    // `count` notes, of which the text refers to 1 to `referenced`; `margin` and `bodySize` are
    // the page's, which the notes are set in.
    PageNotes(size_t count, size_t referenced, double margin, double bodySize);

    // Measuring, note after note from 1: the note being laid out refers to `note`, its number's
    // top at `top`; then its height.
    void BeginMeasure(int note);
    bool Measuring() const { return measuring != 0; }
    void Refer(int note, double top);
    void EndMeasure(double height);
    // What measuring found, for a note set again from its measured layout (a replay): its height,
    // and the references in it, [RefBegin, RefEnd) of RefNote and RefTop.
    double Height(int note) const { return heights[static_cast<size_t>(note) - 1]; }
    uint32_t RefBegin(int note) const { return refBegin[static_cast<size_t>(note) - 1]; }
    uint32_t RefEnd(int note) const { return refBegin[static_cast<size_t>(note)]; }
    int RefNote(uint32_t at) const { return refs[at]; }
    double RefTop(uint32_t at) const { return refTops[at]; }
    // After measuring: the clusters taller than `room` with the rule go after the text.
    void Settle(double room);

    // A line's references (AddLine), then Collect: false when none of them is waiting to be
    // placed. Fit leaves out of the line, for the end, the last clusters that an empty page of
    // `room` beside the line cannot hold; Need is the room the rest takes below the line, the rule
    // included when the page has no notes yet; Place puts them at the foot of the page as far as
    // `room` holds them, and returns the room they take.
    void ClearLine() { line.clear(); }
    void AddLine(int note) {
        if (note > 0 && static_cast<size_t>(note) <= states.size() && states[static_cast<size_t>(note) - 1] == kUnplaced) {
            line.push_back(note);
        }
    }
    bool Collect();
    void Fit(double room);
    double Need() const;
    double Place(double room);

    // The notes waiting at the foot of the page, in the order they are set.
    const std::vector<int>& Pending() const { return pending; }
    void ClearPending() { pending.clear(); }
    // Where the text of `page` ends, before its notes (NoteSourceBlock); kNoTextEnd for a page
    // without notes.
    void SetTextEnd(size_t page, size_t end);
    size_t TextEnd(size_t page) const { return page < textEnds.size() ? textEnds[page] : kNoTextEnd; }

    // After the text: the notes never placed go after it too. True when any note does.
    bool SettleRest();
    bool AtEnd(int note) const { return states[static_cast<size_t>(note) - 1] == kEnd; }
    // The text refers to `note` (and not only other notes do).
    bool Body(int note) const { return static_cast<size_t>(note) <= referenced; }

    double margin;
    double bodySize;
    bool footMode = false;   // the notes are being laid out: no page breaks, no reservations
    // What the renderer's NewPage calls first (its own renderer as `renderer`): paints the page's
    // notes, or true where no page may end. Set by the renderer.
    bool (*pageEnds)(void* renderer) = nullptr;

private:
    enum : uint8_t { kUnplaced, kFoot, kEnd };

    // `note` and, depth first, the notes alone (not referred to by the text) it refers to that wait
    // to be placed and this walk has not met, each once, into `out`; returns their height.
    double Walk(int note, int depth, std::vector<int>& out);

    std::vector<double> heights;     // note N's at [N - 1]
    std::vector<uint8_t> states;
    std::vector<uint32_t> walked;    // the walk that last met each note
    uint32_t walk = 0;
    std::vector<uint32_t> refBegin;  // note N refers to refs[refBegin[N - 1], refBegin[N])
    std::vector<int> refs;
    std::vector<double> refTops;     // where each of refs is painted while measuring
    std::vector<int> line;           // the line's references
    std::vector<int> members;        // the clusters of the line's notes, one after another
    std::vector<uint32_t> clusterEnds;
    std::vector<double> clusterHeights;
    double clustered = 0.0;          // the clusters' heights
    std::vector<int> pending;
    std::vector<size_t> textEnds;
    size_t referenced;
    int measuring = 0;
};

} // namespace TinyPdf::Internal
