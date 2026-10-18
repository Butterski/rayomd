// SPDX-License-Identifier: Apache-2.0

#include "page_notes.h"

#include <algorithm>

namespace TinyPdf::Internal {
namespace {

// Room is compared after sums taken in another order (Ensure's): a cluster that fits there fits here.
constexpr double kTolerance = 1e-6;

} // namespace

PageNotes::PageNotes(size_t count, size_t referencedNotes, double pageMargin, double pageBodySize)
    : margin(pageMargin), bodySize(pageBodySize), heights(count, 0.0), states(count, kUnplaced), walked(count, 0),
      refBegin(count + 1, 0), referenced(std::min(referencedNotes, count)) {}

void PageNotes::BeginMeasure(int note) {
    measuring = note;
    refBegin[static_cast<size_t>(note) - 1] = static_cast<uint32_t>(refs.size());
}

void PageNotes::Refer(int note, double top) {
    if (note <= 0 || static_cast<size_t>(note) > states.size() || note == measuring) return;
    // A note named again right after itself (a line of references to one note) is one edge.
    if (refs.size() > refBegin[static_cast<size_t>(measuring) - 1] && refs.back() == note) return;
    refs.push_back(note);
    refTops.push_back(top);
}

void PageNotes::EndMeasure(double height) {
    heights[static_cast<size_t>(measuring) - 1] = std::max(0.0, height);
    refBegin[static_cast<size_t>(measuring)] = static_cast<uint32_t>(refs.size());
    measuring = 0;
}

double PageNotes::Walk(int note, int depth, std::vector<int>& out) {
    const size_t index = static_cast<size_t>(note) - 1;
    walked[index] = walk;
    out.push_back(note);
    double height = heights[index];
    if (depth == kClusterDepth) return height;
    for (uint32_t at = refBegin[index]; at < refBegin[index + 1]; at++) {
        const int next = refs[at];
        const size_t nextIndex = static_cast<size_t>(next) - 1;
        if (Body(next) || states[nextIndex] != kUnplaced || walked[nextIndex] == walk) continue;
        height += Walk(next, depth + 1, out);
    }
    return height;
}

void PageNotes::Settle(double room) {
    // One walk for all of them: a note alone belongs to the first note that refers to it.
    walk++;
    for (size_t note = 1; note <= referenced; note++) {
        if (states[note - 1] != kUnplaced || walked[note - 1] == walk) continue;
        members.clear();
        if (Walk(static_cast<int>(note), 0, members) + kRule <= room) continue;
        for (const int member : members) states[static_cast<size_t>(member) - 1] = kEnd;
    }
    members.clear();
}

bool PageNotes::Collect() {
    members.clear();
    clusterEnds.clear();
    clusterHeights.clear();
    clustered = 0.0;
    if (line.empty()) return false;
    walk++;
    for (const int note : line) {
        const size_t index = static_cast<size_t>(note) - 1;
        if (states[index] != kUnplaced || walked[index] == walk) continue;
        const double height = Walk(note, 0, members);
        clusterEnds.push_back(static_cast<uint32_t>(members.size()));
        clusterHeights.push_back(height);
        clustered += height;
    }
    return !clusterEnds.empty();
}

void PageNotes::Fit(double room) {
    while (!clusterHeights.empty() && clustered + kRule > room) {
        clusterEnds.pop_back();
        const size_t begin = clusterEnds.empty() ? 0 : clusterEnds.back();
        for (size_t at = begin; at < members.size(); at++) states[static_cast<size_t>(members[at]) - 1] = kEnd;
        members.resize(begin);
        clustered -= clusterHeights.back();
        clusterHeights.pop_back();
    }
    if (clusterHeights.empty()) clustered = 0.0;
}

double PageNotes::Need() const {
    if (clusterHeights.empty()) return 0.0;
    return clustered + (pending.empty() ? kRule : 0.0);
}

double PageNotes::Place(double room) {
    if (clusterHeights.empty()) return 0.0;
    const double rule = pending.empty() ? kRule : 0.0;
    Fit(room + kTolerance - rule + kRule);
    if (clusterHeights.empty()) return 0.0;
    for (const int member : members) {
        states[static_cast<size_t>(member) - 1] = kFoot;
        pending.push_back(member);
    }
    const double taken = clustered + rule;
    members.clear();
    clusterEnds.clear();
    clusterHeights.clear();
    clustered = 0.0;
    return taken;
}

void PageNotes::SetTextEnd(size_t page, size_t end) {
    if (textEnds.size() <= page) textEnds.resize(page + 1, kNoTextEnd);
    textEnds[page] = end;
}

bool PageNotes::SettleRest() {
    bool any = false;
    for (uint8_t& state : states) {
        if (state == kUnplaced) state = kEnd;
        any = any || state == kEnd;
    }
    return any;
}

} // namespace TinyPdf::Internal
