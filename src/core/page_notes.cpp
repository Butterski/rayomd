// SPDX-License-Identifier: Apache-2.0

#include "page_notes.h"

#include <algorithm>

namespace TinyPdf::Internal {
namespace {

// Room is compared after sums taken in another order (Ensure's): a cluster that fits there fits here.
constexpr double kTolerance = 1e-6;

} // namespace

PageNotes::PageNotes(size_t count, size_t referencedNotes, double pageMargin, double pageBodySize)
    : margin(pageMargin), bodySize(pageBodySize), info(new Info[count]()), noteCount(count), slots(new int[count * 3]),
      clusterHeights(new double[count]), referenced(std::min(referencedNotes, count)) {
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
