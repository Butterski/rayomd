// SPDX-License-Identifier: Apache-2.0

#include "page_notes.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "../common/text_utils.h"

namespace TinyPdf::Internal {
namespace {

// Room is compared after sums taken in another order (Ensure's): a cluster that fits there fits here.
constexpr double kTolerance = 1e-6;

} // namespace

PageNotes::PageNotes(size_t count, size_t referencedNotes, double pageMargin, double pageBodySize)
    : margin(pageMargin), bodySize(pageBodySize), info(new Info[count]), noteCount(count), slots(new int[count * 3]),
      clusterHeights(new double[count]), referenced(std::min(referencedNotes, count)) {
    memset(info.get(), 0, count * sizeof(Info));   // kUnplaced, nothing measured
    members = slots.get();
    clusterEnds = members + count;
    pending = clusterEnds + count;
}

PageNotes::~PageNotes() = default;

void PageNotes::BeginMeasure(int note) {
    measuring = note;
}

void PageNotes::Refer(int note, double top) {
    if (note <= 0 || static_cast<size_t>(note) > noteCount || note == measuring) return;
    // A note named again right after itself (a line of references to one note) is one edge.
    const uint32_t begin = measuring > 1 ? info[Index(measuring) - 1].refEnd : 0;
    if (refs.size() > begin && refs.back().note == note) return;
    refs.push_back({ note, top });
}

void PageNotes::EndMeasure(double height, const Ends& ends, bool replay) {
    Info& note = info[Index(measuring)];
    note.height = std::max(0.0, height);
    note.ends = ends;
    note.refEnd = static_cast<uint32_t>(refs.size());
    note.replay = replay;
    measuring = 0;
}

double PageNotes::Walk(int note, int depth) {
    const size_t index = Index(note);
    info[index].walked = walk;
    members[memberCount++] = note;
    double height = info[index].height;
    if (depth == kClusterDepth) return height;
    for (uint32_t at = index != 0 ? info[index - 1].refEnd : 0; at < info[index].refEnd; at++) {
        const int next = refs[at].note;
        const Info& other = info[Index(next)];
        if (Body(next) || other.state != kUnplaced || other.walked == walk) continue;
        height += Walk(next, depth + 1);
    }
    return height;
}

void PageNotes::Settle(double room) {
    // One walk for all of them: a note alone belongs to the first note that refers to it.
    walk++;
    for (size_t note = 1; note <= referenced; note++) {
        if (info[note - 1].state != kUnplaced || info[note - 1].walked == walk) continue;
        memberCount = 0;
        if (Walk(static_cast<int>(note), 0) + kRule <= room) continue;
        for (size_t at = 0; at < memberCount; at++) info[Index(members[at])].state = kEnd;
    }
    memberCount = 0;
}

void PageNotes::AddLine(int note) {
    line.push_back(note);
}

double PageNotes::Collect(double room) {
    memberCount = 0;
    clusterCount = 0;
    clustered = 0.0;
    walk++;
    for (const int note : line) {
        const Info& entry = info[Index(note)];
        if (entry.state != kUnplaced || entry.walked == walk) continue;
        const double height = Walk(note, 0);
        clusterEnds[clusterCount] = static_cast<int>(memberCount);
        clusterHeights[clusterCount++] = height;
        clustered += height;
    }
    line.clear();
    Fit(room);
    if (clusterCount == 0) return 0.0;
    return clustered + (pendingCount == 0 ? kRule : 0.0);
}

void PageNotes::Fit(double room) {
    while (clusterCount != 0 && clustered + kRule > room) {
        clusterCount--;
        const size_t begin = clusterCount == 0 ? 0 : static_cast<size_t>(clusterEnds[clusterCount - 1]);
        for (size_t at = begin; at < memberCount; at++) info[Index(members[at])].state = kEnd;
        memberCount = begin;
        clustered -= clusterHeights[clusterCount];
    }
    if (clusterCount == 0) clustered = 0.0;
}

double PageNotes::Place(double room) {
    if (clusterCount == 0) return 0.0;
    const double rule = pendingCount == 0 ? kRule : 0.0;
    Fit(room + kTolerance - rule + kRule);
    if (clusterCount == 0) return 0.0;
    for (size_t at = 0; at < memberCount; at++) {
        info[Index(members[at])].state = kFoot;
        pending[pendingCount++] = members[at];
    }
    const double taken = clustered + rule;
    memberCount = 0;
    clusterCount = 0;
    clustered = 0.0;
    return taken;
}

void PageNotes::SetTextEnd(size_t page, size_t end) {
    while (textEnds.size() <= page) textEnds.push_back(kNoTextEnd);
    textEnds[page] = end;
}

bool PageNotes::SettleRest() {
    bool any = false;
    for (size_t index = 0; index < noteCount; index++) {
        if (info[index].state == kUnplaced) info[index].state = kEnd;
        any = any || info[index].state == kEnd;
    }
    return any;
}

} // namespace TinyPdf::Internal

namespace TinyPdf {
namespace {

// What the bottom tests compare with while notes are laid out: far below any page, so that
// nothing breaks one.
constexpr double kNoBottom = 1e15;

// The renderer set for laying out notes: the page's margin and the notes' size, no page breaks or
// reservations (footMode), no source map, the buffers its paint paths share swapped for the
// notes' own; all of it back when it ends.
class FootScope {
public:
    FootScope(PageNoteBase& notes, double top)
        : s(notes), h(notes.host), y(h.y), margin(h.margin), bottom(h.textBottom), bodySize(h.bodySize),
          lineHeight(h.lineHeight), listDepth(h.listDepth), quoteBar(h.quoteBar), sourceMap(h.sourceMap) {
        h.call(h.renderer, NoteCall::Swap, 0, nullptr);
        s.footMode = true;
        h.textBottom = -kNoBottom;
        h.margin = s.margin;
        h.y = top;
        h.bodySize = s.bodySize * kNoteTextFactor;
        h.lineHeight = h.bodySize * 1.35;
        h.listDepth = 0;
        h.quoteBar = h.plainQuoteBar;
        h.sourceMap = nullptr;
    }
    ~FootScope() {
        h.call(h.renderer, NoteCall::Swap, 0, nullptr);
        s.footMode = false;
        h.y = y;
        h.margin = margin;
        h.textBottom = bottom;
        h.bodySize = bodySize;
        h.lineHeight = lineHeight;
        h.listDepth = listDepth;
        h.quoteBar = quoteBar;
        h.sourceMap = sourceMap;
    }
    FootScope(const FootScope&) = delete;
    FootScope& operator=(const FootScope&) = delete;

private:
    PageNoteBase& s;
    const NoteHost& h;
    const double y, margin, bottom, bodySize, lineHeight;
    const int listDepth;
    const std::string_view quoteBar;
    std::vector<SourceBlock>* const sourceMap;
};

// Headings or anchors [first, last) of a note's measuring layout, moved to `page` and by `dy`,
// after `to`'s.
void MoveMarks(const std::vector<HeadingMark>& from, uint32_t first, uint32_t last, size_t page, double dy,
    std::vector<HeadingMark>& to) {
    const size_t start = to.size();
    to.insert(to.end(), from.begin() + first, from.begin() + last);
    for (size_t at = start; at < to.size(); at++) {
        to[at].page = page;
        to[at].top += dy;
    }
}

// Note `note` again from its measuring layout, its top at `y` on `page`: the content under one
// translation, its links, headings, anchors and the first references of the notes only notes refer
// to moved by as much. What a second layout would draw, at the cost of a copy. Each note is painted
// once, so its links move out of the stash.
void ReplayNote(PageNoteBase& notes, int note, size_t page, double y) {
    const NoteHost& h = notes.host;
    FootStash& stash = notes.stash;
    const double dy = y - notes.measureTop;
    const Internal::PageNotes::Ends begin = notes.Begin(note);
    const Internal::PageNotes::Ends& end = notes.EndsOf(note);
    h.noteMarks.notes[static_cast<size_t>(note) - 1] = { page, y };
    char head[12 + RayoMd::Text::kFixed2MaxChars + 4];
    memcpy(head, "q 1 0 0 1 0 ", 12);
    char* at = RayoMd::Text::WriteFixed2(head + 12, dy);
    memcpy(at, " cm\n", 4);
    h.content.append(head, static_cast<size_t>(at + 4 - head));
    h.content.append(notes.content.data() + begin.content, end.content - begin.content);
    h.content.append("Q\n", 2);
    if (begin.links != end.links) {
        std::vector<LinkRect>& links = h.pageLinks.back();
        const size_t first = links.size();
        links.insert(links.end(), std::make_move_iterator(stash.links.begin() + begin.links),
            std::make_move_iterator(stash.links.begin() + end.links));
        for (size_t link = first; link < links.size(); link++) {
            links[link].y1 += dy;
            links[link].y2 += dy;
        }
    }
    if (begin.marks != end.marks) MoveMarks(stash.marks, begin.marks, end.marks, page, dy, h.headings);
    if (begin.anchors != end.anchors) MoveMarks(stash.anchors, begin.anchors, end.anchors, page, dy, h.ids);
    for (const Internal::PageNotes::Ref* ref = notes.RefBegin(note); ref < notes.RefEnd(note); ref++) {
        if (!notes.Body(ref->note)) h.noteMarks.MarkReference(ref->note, page, ref->top + dy);
    }
}

// Each note's height and the notes it refers to, from a layout written into the stash (the
// renderer's content, page links, headings and anchors swapped for its buffers meanwhile, so
// nothing is copied or cut back): Paint replays it where it can. Taken back otherwise
// (NoteHost::measure): its images, tags, glyph counts and fallback probe. The glyphs it marks used
// are those the notes paint later, as each is painted once.
void Measure(PageNoteBase& notes) {
    const NoteHost& h = notes.host;
    FootStash& stash = notes.stash;
    const bool tagged = h.measure(notes, true);
    stash.Trade(h.content, notes.content, h.pageLinks.back(), h.headings, h.ids);
    {
        // Anywhere below the top of a page: a note's layout does not depend on where it starts.
        // Its middle (pageH * 0.5 bit for bit on pages of a fixed size).
        notes.measureTop = h.pageH - h.fullH * 0.5;
        FootScope scope(notes, notes.measureTop);
        for (size_t index = 0; index < notes.Count() && !(h.cancel != nullptr && h.cancel->load(std::memory_order_relaxed));
             index++) {
            const uint64_t images = h.imageMark(notes);
            notes.BeginMeasure(static_cast<int>(index) + 1);
            h.y = notes.measureTop;
            h.call(h.renderer, NoteCall::Note, index, nullptr);
            notes.EndMeasure(notes.measureTop - h.y,
                { h.content.size(), static_cast<uint32_t>(h.pageLinks.back().size()), static_cast<uint32_t>(h.headings.size()),
                    static_cast<uint32_t>(h.ids.size()) },
                !tagged && h.imageMark(notes) == images);
        }
    }
    stash.Trade(h.content, notes.content, h.pageLinks.back(), h.headings, h.ids);
    h.measure(notes, false);
    notes.Settle(h.fullH - h.margin * 2.0 - h.lineHeight);
}

// The notes waiting at the foot of the page from `y` down: a rule, then each; where they end.
double PaintPending(PageNoteBase& notes, double y) {
    const NoteHost& h = notes.host;
    const size_t page = h.pageStarts.size() - 1;
    y -= 8.0;
    if (h.tagged) h.call(h.renderer, NoteCall::Artifact, 0, nullptr);
    h.rule(h.content, h.ruleColor, notes.margin, y, (h.pageW - notes.margin * 2.0) / 3.0);
    y -= 10.0;
    for (const int note : notes.Pending()) {
        if (notes.Replayed(note)) {
            ReplayNote(notes, note, page, y);
            y -= notes.Height(note);
        } else {
            const size_t index = static_cast<size_t>(note) - 1;
            h.y = y;
            h.noteMarks.notes[index] = { page, y };
            h.call(h.renderer, NoteCall::Note, index, nullptr);
            y = h.y;
        }
    }
    return y;
}

// The notes waiting at the foot of the page that ends: a rule at the top of the room kept for
// them, then each, down to the page margin. The page's text ends where they begin. On a long page
// (PdfPageSize::autoHeight), whose box ends below what it holds, they follow its text instead, and
// the page ends below them. Only a note laid out again (not Replayed) needs the renderer set aside.
void Paint(PageNoteBase& notes) {
    const NoteHost& h = notes.host;
    if (!notes.Pending().empty()) {
        notes.SetTextEnd(h.pageStarts.size() - 1, h.content.size());
        const bool longPage = h.pageBottoms != nullptr;
        const double top = longPage ? h.y : notes.margin + (h.textBottom - h.margin);
        bool layout = false;
        for (const int note : notes.Pending()) layout = layout || !notes.Replayed(note);
        double end = 0.0;
        if (layout) {
            FootScope scope(notes, top);
            end = PaintPending(notes, top);
        } else {
            end = PaintPending(notes, top);
        }
        if (longPage) h.y = end;
        notes.ClearPending();
    }
    h.textBottom = h.margin + (h.pageH - h.fullH);
}

// The renderer's Ensure.
void Ensure(const NoteHost& h, double needed) {
    if (h.y - needed < h.textBottom) h.call(h.renderer, NoteCall::NewPage, 0, nullptr);
}

} // namespace

FootStash::~FootStash() = default;

void FootStash::Trade(std::string& content, std::string& notesContent, std::vector<LinkRect>& pageLinks,
    std::vector<HeadingMark>& headings, std::vector<HeadingMark>& ids) {
    content.swap(notesContent);
    pageLinks.swap(links);
    headings.swap(marks);
    ids.swap(anchors);
}

PageNoteBase::PageNoteBase(const NoteHost& renderer, size_t count, size_t referencedNotes)
    : Internal::PageNotes(count, referencedNotes, renderer.margin, renderer.bodySize), host(renderer) {}

PageNoteBase::~PageNoteBase() = default;

void PageNoteCore::Begin(PageNoteBase& notes) {
    notes.host.noteMarks.notes.assign(notes.Count(), NoteMarks::Point{});
    Measure(notes);
}

void PageNoteCore::End(PageNoteBase& notes) {
    if (notes.SettleRest()) notes.host.call(notes.host.renderer, NoteCall::EndSection, 0, &notes);
    Paint(notes);
}

bool PageNoteCore::Break(PageNoteBase& notes) {
    if (notes.footMode) return true;
    Paint(notes);
    return false;
}

void PageNoteCore::Reserve(PageNoteBase& notes, double needed) {
    const NoteHost& h = notes.host;
    if (notes.LineEmpty()) {
        Ensure(h, needed);
        return;
    }
    Ensure(h, needed + notes.Collect(h.fullH - h.margin * 2.0 - needed));
    h.textBottom += notes.Place(h.y - needed - h.textBottom);
}

double PageNoteCore::RowNeed(PageNoteBase& notes, double needed) {
    if (notes.LineEmpty()) return 0.0;
    return notes.Collect(notes.host.fullH - notes.host.margin * 2.0 - needed);
}

void PageNoteCore::PlaceRow(PageNoteBase& notes, double needed) {
    const NoteHost& h = notes.host;
    h.textBottom += notes.Place(h.y - needed - h.textBottom);
}

void PageNoteCore::ReserveRow(PageNoteBase& notes, double needed, double need) {
    Ensure(notes.host, needed + need);
    if (need > 0.0) PlaceRow(notes, needed);
}

void NoteReferenced(Internal::PageNotes& notes, NoteMarks& marks, int note, size_t page, double top) {
    if (notes.Measuring()) notes.Refer(note, top);
    else if (!notes.footMode || !notes.Body(note)) marks.MarkReference(note, page, top);
}

} // namespace TinyPdf
