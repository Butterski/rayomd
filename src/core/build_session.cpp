// SPDX-License-Identifier: Apache-2.0

#include "build_session.h"

#include "highlight.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <utility>

namespace TinyPdf::Internal {

namespace {

thread_local LayoutSession* g_session = nullptr;

// Whether two images of blocks are the same image, sized and linked the same way.
bool SameImage(const ImageSource& a, const ImageSource& b) {
    if (a.src != b.src || a.link != b.link || a.alt != b.alt || a.widthPercent != b.widthPercent ||
        std::memcmp(&a.widthHint, &b.widthHint, sizeof a.widthHint) != 0 ||
        std::memcmp(&a.heightHint, &b.heightHint, sizeof a.heightHint) != 0 || a.row.size() != b.row.size()) {
        return false;
    }
    for (size_t index = 0; index < a.row.size(); index++) {
        if (!SameImage(a.row[index], b.row[index])) return false;
    }
    return true;
}

// Whether two blocks are laid out the same way: every field the renderers read, all but where the
// block stands in the Markdown (sourceLine, sourceLines), which only the source map takes.
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Block) == 136, "a new field of Block: compare it in SameBlock");
#endif
bool SameBlock(const Block& a, const Block& b) {
    if (a.type != b.type || a.level != b.level || a.number != b.number || a.task != b.task || a.alert != b.alert ||
        a.notesPending != b.notesPending || a.codeLanguage != b.codeLanguage || a.hasMath != b.hasMath ||
        a.align != b.align || a.text != b.text || a.aligns != b.aligns || a.rows != b.rows ||
        (a.image == nullptr) != (b.image == nullptr) || (a.image != nullptr && !SameImage(*a.image, *b.image)) ||
        a.children.size() != b.children.size()) {
        return false;
    }
    for (size_t index = 0; index < a.children.size(); index++) {
        if (!SameBlock(a.children[index], b.children[index])) return false;
    }
    return true;
}

// Whether laying out the block reads an image (a file or URL that may have changed since) or draws a
// diagram: such blocks are always laid out again, so the image registry sees what a build without
// a session sees, in its order.
bool TouchesImages(const Block& block) {
    if (block.type == BlockType::Image || (block.type == BlockType::Code && block.codeLanguage == kMermaidCode)) return true;
    for (const Block& child : block.children) {
        if (TouchesImages(child)) return true;
    }
    return false;
}

bool SameContents(const std::vector<ContentsEntry>& a, const std::vector<ContentsEntry>& b) {
    if (a.size() != b.size()) return false;
    for (size_t index = 0; index < a.size(); index++) {
        if (a[index].level != b[index].level || a[index].text != b[index].text) return false;
    }
    return true;
}

// --number-headings: the number each top-level heading draws (Renderer::NumberOf), null for none.
std::vector<const std::string*> NumbersOf(const std::vector<Block>& blocks, const std::vector<HeadingNumber>& numbers) {
    std::vector<const std::string*> of(blocks.size(), nullptr);
    size_t next = 0;
    for (size_t index = 0; index < blocks.size() && next < numbers.size(); index++) {
        if (numbers[next].heading == blocks[index].text.data()) of[index] = &numbers[next++].number;
    }
    return of;
}

// Where the text `text` of a block of `from` lies in its equal `to`: a heading's or an anchor's mark
// views its block's text. Null when it is in neither.
const char* Relocate(std::string_view text, const Block& from, const Block& to) {
    const char* const at = text.data();
    if (at >= from.text.data() && at + text.size() <= from.text.data() + from.text.size()) {
        return to.text.data() + (at - from.text.data());
    }
    for (size_t index = 0; index < from.children.size(); index++) {
        if (const char* found = Relocate(text, from.children[index], to.children[index])) return found;
    }
    return nullptr;
}

// Whether `cids` holds the first `count` CIDs of `recorded`, another build's.
bool Covers(const LayoutCids& cids, const std::vector<uint16_t>& recorded, size_t count) {
    if (count == 0) return true;
    const std::vector<uint16_t>& own = cids.recorded(cids.set);
    // As a rule both builds recorded the same CIDs in the same order up to here.
    if (own.size() >= count && std::memcmp(own.data(), recorded.data(), count * sizeof(uint16_t)) == 0) return true;
    for (size_t index = 0; index < count; index++) {
        if (!cids.has(cids.set, recorded[index])) return false;
    }
    return true;
}

bool CoversGlyphs(const MathGlyphUse& own, const MathGlyphUse& other) {
    for (int face = 0; face < kMathFontCount; face++) {
        for (int word = 0; word < 4; word++) {
            if ((own.bits[face][word] & other.bits[face][word]) != other.bits[face][word]) return false;
        }
    }
    return true;
}

void AddGlyphs(MathGlyphUse& own, const MathGlyphUse& other) {
    for (int face = 0; face < kMathFontCount; face++) {
        for (int word = 0; word < 4; word++) own.bits[face][word] |= other.bits[face][word];
    }
}

} // namespace

LayoutSession* CurrentLayoutSession() {
    return g_session;
}

void LayoutTape::Clear() {
    renderer = 0;
    std::fill(std::begin(fonts), std::end(fonts), nullptr);
    blocks.clear();
    contents.clear();
    numbers.clear();
    marks.clear();
    content.clear();
    pageStarts.clear();
    pageLinks.clear();
    headings.clear();
    ids.clear();
    contentsLines.clear();
    sourceMap.clear();
    sourceMapBlocks.clear();
    for (std::vector<uint16_t>& list : cids) list.clear();
    glyphs.clear();
    pageBottoms.clear();
    headingPlaces.clear();
    idPlaces.clear();
}

// The LayoutSession of a BuildSession.
class SessionLayout final : public LayoutSession {
public:
    SessionLayout() { calls = SessionRendererCalls(); }
    void Begin(uint8_t renderer, const void* const fonts[kLayoutCidSets], const std::vector<Block>& blocks,
        const std::vector<ContentsEntry>& contents, const std::vector<HeadingNumber>& numbers) override;
    std::vector<SourceBlock>& SourceMap() override { return recording->sourceMap; }
    void Mark(size_t index, LayoutMark& mark, const LayoutTarget& target) override;
    size_t Replayable(size_t index, const LayoutTarget& target) override;
    LayoutMark Replay(size_t index, size_t end, LayoutTarget& target) override;
    void EndLayout(const LayoutTarget& target, std::vector<SourceBlock>* sourceMap) override;
    void Commit(LayoutTarget& target, std::vector<Block>& blocks, std::vector<ContentsEntry>& contents,
        std::vector<HeadingNumber>& numbers) override;
    void Forget() override;

    BuildSession::Reuse reuse;

private:
    static constexpr uint32_t kUnknown = 0xFFFFFFFFu;

    // The last build's block for block `index` of this one: the same before the first that changed,
    // and after it with as many blocks as before; else shifted by the blocks added or removed.
    size_t OldIndex(size_t index) const { return index >= suffixStart ? index - blockCount + oldCount : index; }
    // Whether block `index` is laid out as the last build's block `old` was: equal, with the same
    // table of contents for a table and the same number for a numbered heading.
    bool Same(size_t index, size_t old) const;
    // The end of the run of blocks from `index` (not known yet) that may be replayed, compared now.
    size_t RunFrom(size_t index);

    std::unique_ptr<LayoutTape> last;        // the last build's tape, or null
    std::unique_ptr<LayoutTape> recording;   // the tape this build makes (the memory of an older one)
    bool active = false;                     // between Begin and Commit or Forget
    const std::vector<Block>* blocks = nullptr;
    size_t blockCount = 0;
    size_t oldCount = 0;
    size_t suffixStart = 0;                  // the first block shifted by the blocks added or removed
    bool contentsSame = false;
    std::vector<const std::string*> newNumbers;   // --number-headings: each block's number, if any
    std::vector<const std::string*> oldNumbers;
    // For each block, the end of the run of blocks from it that the last build laid out the same way
    // (equal, no image, one shift); 0 where none starts, kUnknown where not compared yet.
    std::vector<uint32_t> runEnd;
    // The run Replayable chose: its blocks in the last build and its marks moved to this one's blocks.
    size_t oldFrom = 0;
    size_t oldTo = 0;
    std::vector<HeadingMark> movedHeadings;
    std::vector<HeadingMark> movedIds;
};

void SessionLayout::Begin(uint8_t renderer, const void* const fonts[kLayoutCidSets], const std::vector<Block>& newBlocks,
    const std::vector<ContentsEntry>& contents, const std::vector<HeadingNumber>& numbers) {
    if (!recording) recording = std::make_unique<LayoutTape>();
    else recording->Clear();
    recording->renderer = renderer;
    std::copy(fonts, fonts + kLayoutCidSets, recording->fonts);
    active = true;
    blocks = &newBlocks;
    blockCount = newBlocks.size();
    reuse = { static_cast<uint32_t>(blockCount), static_cast<uint32_t>(blockCount), 0 };
    runEnd.assign(blockCount, 0);
    suffixStart = blockCount;
    newNumbers.clear();
    oldNumbers.clear();
    if (!last || last->renderer != renderer || !std::equal(fonts, fonts + kLayoutCidSets, last->fonts) ||
        last->marks.size() != last->blocks.size() + 1) {
        return;
    }
    oldCount = last->blocks.size();
    contentsSame = SameContents(contents, last->contents);
    if (!numbers.empty() || !last->numbers.empty()) {
        newNumbers = NumbersOf(newBlocks, numbers);
        oldNumbers = NumbersOf(last->blocks, last->numbers);
    }
    // The blocks before the first that changed keep their places. The others are compared only when
    // a layout reaches one in the state the last build had there, which after an edit that moved
    // the text on the page may never happen: then nothing was compared in vain.
    const size_t common = std::min(blockCount, oldCount);
    size_t prefix = 0;
    while (prefix < common && Same(prefix, prefix)) prefix++;
    // With as many blocks as before, the blocks after the change keep their places too; else they
    // are shifted by as many as were added or removed.
    if (blockCount != oldCount) suffixStart = prefix;
    std::fill(runEnd.begin() + prefix, runEnd.end(), kUnknown);
    for (size_t index = prefix; index-- > 0;) {
        if (TouchesImages(newBlocks[index])) continue;
        runEnd[index] = index + 1 < prefix && runEnd[index + 1] != 0 ? runEnd[index + 1] : static_cast<uint32_t>(index + 1);
    }
}

bool SessionLayout::Same(size_t index, size_t old) const {
    const Block& block = (*blocks)[index];
    if (!SameBlock(block, last->blocks[old]) || (block.type == BlockType::Contents && !contentsSame)) return false;
    if (newNumbers.empty()) return true;
    const std::string* const a = newNumbers[index];
    const std::string* const b = oldNumbers[old];
    return a == nullptr ? b == nullptr : b != nullptr && *a == *b;
}

size_t SessionLayout::RunFrom(size_t index) {
    size_t end = index;
    while (end < blockCount) {
        if (runEnd[end] != kUnknown) {
            if (runEnd[end] != 0) end = runEnd[end];
            break;
        }
        const bool mapped = end < suffixStart || end + oldCount >= blockCount;
        if (!mapped || OldIndex(end) >= oldCount || TouchesImages((*blocks)[end]) || !Same(end, OldIndex(end))) {
            runEnd[end] = 0;
            break;
        }
        end++;
    }
    for (size_t at = index; at < end; at++) runEnd[at] = static_cast<uint32_t>(end);
    return end;
}

void SessionLayout::Mark(size_t index, LayoutMark& mark, const LayoutTarget& target) {
    if (!active) return;
    LayoutTape& tape = *recording;
    // The source map's entries since the last mark are the block's before this one.
    while (tape.sourceMapBlocks.size() < tape.sourceMap.size()) tape.sourceMapBlocks.push_back(static_cast<uint32_t>(index - 1));
    mark.content = target.content->size();
    mark.page = static_cast<uint32_t>(target.pageStarts->size() - 1);
    mark.links = static_cast<uint32_t>(target.pageLinks->back().size());
    mark.headings = static_cast<uint32_t>(target.headings->size());
    mark.ids = static_cast<uint32_t>(target.ids->size());
    mark.contentsLines = static_cast<uint32_t>(target.contentsLines->size());
    mark.sourceBlocks = static_cast<uint32_t>(tape.sourceMap.size());
    mark.bottoms = target.pageBottoms != nullptr ? static_cast<uint32_t>(target.pageBottoms->size()) : 0;
    for (size_t set = 0; set < kLayoutCidSets; set++) {
        const LayoutCids& cids = target.cids[set];
        if (cids.set == nullptr) continue;
        mark.cids[set] = static_cast<uint32_t>(cids.recorded(cids.set).size());
        mark.missing[set] = *cids.missing;
    }
    if (target.cids[0].set != nullptr) mark.missingBmp = *target.cids[0].missingBmp;
    if (mark.content > target.pageStarts->back()) mark.flags |= LayoutMark::kPageStarted;
    if (!target.contentsLines->empty()) mark.flags |= LayoutMark::kContentsDrawn;
    if (tape.glyphs.empty() || std::memcmp(&tape.glyphs.back(), target.glyphs, sizeof(MathGlyphUse)) != 0) {
        tape.glyphs.push_back(*target.glyphs);
    }
    mark.glyphs = static_cast<uint32_t>(tape.glyphs.size() - 1);
    tape.marks.push_back(mark);
}

size_t SessionLayout::Replayable(size_t index, const LayoutTarget& target) {
    if (!active || index >= blockCount || runEnd[index] == 0) return index;
    // A block after the change that no block of the last build stands for (more blocks than before).
    if (index >= suffixStart && index + oldCount < blockCount) return index;
    const LayoutMark& now = recording->marks[index];
    const size_t from = OldIndex(index);
    if (from >= oldCount) return index;
    const LayoutMark& before = last->marks[from];
    constexpr uint8_t kSame = LayoutMark::kPageStarted | LayoutMark::kPending | LayoutMark::kMathActive |
        LayoutMark::kContentsDrawn | LayoutMark::kResumable;
    if ((now.flags & LayoutMark::kResumable) == 0 || ((now.flags ^ before.flags) & kSame) != 0 ||
        std::memcmp(&now.y, &before.y, sizeof now.y) != 0) {
        return index;
    }
    // A heading keeps room for the blocks after it (HeadingKeep): the headings and anchors right after
    // it and whether a block follows them. A run ends after another block, or with both documents.
    size_t end = runEnd[index] == kUnknown ? RunFrom(index) : runEnd[index];
    if (end <= index) return index;
    if (!(end == blockCount && OldIndex(end - 1) + 1 == oldCount)) {
        while (end > index && ((*blocks)[end - 1].type == BlockType::Heading || (*blocks)[end - 1].type == BlockType::Anchor)) end--;
    }
    if (end == index) return index;
    const size_t to = OldIndex(end - 1) + 1;
    const LayoutMark& after = last->marks[to];
    if ((after.flags & LayoutMark::kResumable) == 0) return index;
    // A layout that probes for a fallback font (Renderer::ProbeFallback) stops at the first block that
    // drew a character its font lacks: such blocks are laid out.
    if (target.probing && after.missingBmp != before.missingBmp) return index;
    // What the run draws is in the sets as the last build had them before it, so that adding what it
    // added gives what laying it out gives.
    for (size_t set = 0; set < kLayoutCidSets; set++) {
        const LayoutCids& cids = target.cids[set];
        if (cids.set == nullptr ? before.cids[set] != 0 : !Covers(cids, last->cids[set], before.cids[set])) return index;
    }
    if (!CoversGlyphs(*target.glyphs, last->glyphs[before.glyphs]) || (now.facesUsed & before.facesUsed) != before.facesUsed) {
        return index;
    }
    // The run's headings and anchors view its blocks' text: moved to this build's blocks, which are
    // equal to the last build's, by where they were in them.
    const auto move = [&](const std::vector<HeadingMark>& marks, const std::vector<LayoutTape::MarkPlace>& places,
                          uint32_t first, uint32_t stop, std::vector<HeadingMark>& moved) {
        moved.clear();
        for (uint32_t mark = first; mark < stop; mark++) {
            HeadingMark heading = marks[mark];
            const LayoutTape::MarkPlace place = places[mark];
            const Block& block = (*blocks)[place.block - from + index];
            const char* text = nullptr;
            if (place.offset != LayoutTape::MarkPlace::kNested) {
                if (place.offset + heading.text.size() <= block.text.size()) text = block.text.data() + place.offset;
            } else {
                text = Relocate(heading.text, last->blocks[place.block], block);
            }
            if (text == nullptr) return false;
            heading.text = std::string_view(text, heading.text.size());
            moved.push_back(heading);
        }
        return true;
    };
    if (!move(last->headings, last->headingPlaces, before.headings, after.headings, movedHeadings) ||
        !move(last->ids, last->idPlaces, before.ids, after.ids, movedIds)) {
        return index;
    }
    oldFrom = from;
    oldTo = to;
    return end;
}

LayoutMark SessionLayout::Replay(size_t index, size_t end, LayoutTarget& target) {
    LayoutTape& tape = *recording;
    const LayoutTape& old = *last;
    const LayoutMark now = tape.marks[index];
    const LayoutMark& before = old.marks[oldFrom];
    const LayoutMark& after = old.marks[oldTo];
    // Counts in this build: those of the last moved by what each had before the run.
    const auto move = [](uint32_t value, uint32_t from, uint32_t to) { return static_cast<uint32_t>(value - from + to); };
    const size_t pageShift = static_cast<size_t>(now.page) - before.page;   // modulo 2^64, as the pages' indices
    target.content->append(old.content, before.content, after.content - before.content);
    for (size_t page = before.page + 1; page <= after.page; page++) {
        target.pageStarts->push_back(old.pageStarts[page] - before.content + now.content);
    }
    std::vector<std::vector<LinkRect>>& links = *target.pageLinks;
    const std::vector<LinkRect>& firstLinks = old.pageLinks[before.page];
    if (after.page == before.page) {
        links.back().insert(links.back().end(), firstLinks.begin() + before.links, firstLinks.begin() + after.links);
    } else {
        links.back().insert(links.back().end(), firstLinks.begin() + before.links, firstLinks.end());
        for (size_t page = before.page + 1; page < after.page; page++) links.push_back(old.pageLinks[page]);
        const std::vector<LinkRect>& lastLinks = old.pageLinks[after.page];
        links.emplace_back(lastLinks.begin(), lastLinks.begin() + after.links);
    }
    if (target.pageBottoms != nullptr) {
        target.pageBottoms->insert(target.pageBottoms->end(), old.pageBottoms.begin() + before.bottoms,
            old.pageBottoms.begin() + after.bottoms);
    }
    for (HeadingMark& mark : movedHeadings) {
        mark.page += pageShift;
        target.headings->push_back(mark);
    }
    for (HeadingMark& mark : movedIds) {
        mark.page += pageShift;
        target.ids->push_back(mark);
    }
    for (uint32_t line = before.contentsLines; line < after.contentsLines; line++) {
        ContentsLine moved = old.contentsLines[line];
        moved.page += pageShift;
        target.contentsLines->push_back(moved);
    }
    const size_t blockShift = index - oldFrom;
    for (uint32_t entry = before.sourceBlocks; entry < after.sourceBlocks; entry++) {
        SourceBlock moved = old.sourceMap[entry];
        const size_t block = old.sourceMapBlocks[entry] + blockShift;
        const Block& source = (*blocks)[block];
        moved.firstLine = source.sourceLine;
        moved.endLine = source.sourceLine + source.sourceLines;
        moved.page = static_cast<uint32_t>(moved.page + pageShift);
        moved.endPage = static_cast<uint32_t>(moved.endPage + pageShift);
        tape.sourceMap.push_back(moved);
        tape.sourceMapBlocks.push_back(static_cast<uint32_t>(block));
    }
    // The marks between, the CIDs each block recorded first added in their order.
    const MathGlyphUse glyphs = *target.glyphs;
    uint32_t glyphsOf = before.glyphs;   // the last build's glyphs the last mark moved here had
    constexpr uint8_t kMath = LayoutMark::kMathUsed | LayoutMark::kMathLaidOut;
    for (size_t at = oldFrom + 1; at <= oldTo; at++) {
        const LayoutMark& prior = old.marks[at - 1];
        const LayoutMark& mark = old.marks[at];
        for (size_t set = 0; set < kLayoutCidSets; set++) {
            const LayoutCids& cids = target.cids[set];
            if (cids.set == nullptr) continue;
            for (uint32_t cid = prior.cids[set]; cid < mark.cids[set]; cid++) cids.add(cids.set, old.cids[set][cid]);
        }
        if (at == oldTo) break;
        LayoutMark moved = mark;
        moved.content = mark.content - before.content + now.content;
        moved.page = static_cast<uint32_t>(mark.page + pageShift);
        if (mark.page == before.page) moved.links = move(mark.links, before.links, now.links);
        moved.headings = move(mark.headings, before.headings, now.headings);
        moved.ids = move(mark.ids, before.ids, now.ids);
        moved.contentsLines = move(mark.contentsLines, before.contentsLines, now.contentsLines);
        moved.sourceBlocks = move(mark.sourceBlocks, before.sourceBlocks, now.sourceBlocks);
        moved.nextNumber = move(mark.nextNumber, before.nextNumber, now.nextNumber);
        moved.bottoms = move(mark.bottoms, before.bottoms, now.bottoms);
        for (size_t set = 0; set < kLayoutCidSets; set++) {
            const LayoutCids& cids = target.cids[set];
            moved.cids[set] = cids.set != nullptr ? static_cast<uint32_t>(cids.recorded(cids.set).size()) : 0;
            moved.missing[set] = move(mark.missing[set], before.missing[set], now.missing[set]);
        }
        moved.missingBmp = move(mark.missingBmp, before.missingBmp, now.missingBmp);
        moved.facesUsed = static_cast<uint8_t>(mark.facesUsed | now.facesUsed);
        moved.flags = static_cast<uint8_t>((mark.flags & ~kMath) | ((mark.flags | now.flags) & kMath));
        if (mark.glyphs != glyphsOf) {
            glyphsOf = mark.glyphs;
            MathGlyphUse drawn = glyphs;
            AddGlyphs(drawn, old.glyphs[mark.glyphs]);
            if (std::memcmp(&tape.glyphs.back(), &drawn, sizeof drawn) != 0) tape.glyphs.push_back(drawn);
        }
        moved.glyphs = static_cast<uint32_t>(tape.glyphs.size() - 1);
        tape.marks.push_back(moved);
    }
    for (size_t set = 0; set < kLayoutCidSets; set++) {
        const LayoutCids& cids = target.cids[set];
        if (cids.set == nullptr) continue;
        *cids.missing += after.missing[set] - before.missing[set];
        if (set == 0) *cids.missingBmp += after.missingBmp - before.missingBmp;
    }
    AddGlyphs(*target.glyphs, old.glyphs[after.glyphs]);
    reuse.laidOut -= static_cast<uint32_t>(end - index);
    reuse.replayed += static_cast<uint32_t>(end - index);
    LayoutMark state = after;
    state.nextNumber = move(after.nextNumber, before.nextNumber, now.nextNumber);
    state.facesUsed = static_cast<uint8_t>(after.facesUsed | now.facesUsed);
    state.flags = static_cast<uint8_t>((after.flags & ~kMath) | ((after.flags | now.flags) & kMath));
    return state;
}

void SessionLayout::EndLayout(const LayoutTarget& target, std::vector<SourceBlock>* sourceMap) {
    if (!active) return;
    LayoutTape& tape = *recording;
    tape.content.assign(*target.content);
    tape.pageStarts = *target.pageStarts;
    for (size_t set = 0; set < kLayoutCidSets; set++) {
        const LayoutCids& cids = target.cids[set];
        if (cids.set != nullptr) tape.cids[set] = cids.recorded(cids.set);
    }
    if (target.pageBottoms != nullptr) tape.pageBottoms = *target.pageBottoms;
    if (sourceMap != nullptr) *sourceMap = tape.sourceMap;
}

void SessionLayout::Commit(LayoutTarget& target, std::vector<Block>& newBlocks, std::vector<ContentsEntry>& contents,
    std::vector<HeadingNumber>& numbers) {
    if (!active) return;
    active = false;
    LayoutTape& tape = *recording;
    // Complete only when every block was laid out or replayed.
    if (tape.marks.size() != newBlocks.size() + 1) {
        last.reset();
        return;
    }
    tape.pageLinks = std::move(*target.pageLinks);
    tape.headings = std::move(*target.headings);
    tape.ids = std::move(*target.ids);
    tape.contentsLines = std::move(*target.contentsLines);
    // Where the marks are, with the blocks in the cache.
    const auto place = [&](const std::vector<HeadingMark>& marks, uint32_t LayoutMark::*count,
                           std::vector<LayoutTape::MarkPlace>& places) {
        places.resize(marks.size());
        for (size_t block = 0; block < newBlocks.size() && !marks.empty(); block++) {
            const std::string& text = newBlocks[block].text;
            for (uint32_t mark = tape.marks[block].*count; mark < tape.marks[block + 1].*count; mark++) {
                const char* const at = marks[mark].text.data();
                const bool own = at >= text.data() && at + marks[mark].text.size() <= text.data() + text.size();
                places[mark] = { static_cast<uint32_t>(block),
                    own ? static_cast<uint32_t>(at - text.data()) : LayoutTape::MarkPlace::kNested };
            }
        }
    };
    place(tape.headings, &LayoutMark::headings, tape.headingPlaces);
    place(tape.ids, &LayoutMark::ids, tape.idPlaces);
    tape.blocks = std::move(newBlocks);
    tape.contents = std::move(contents);
    tape.numbers = std::move(numbers);
    std::swap(last, recording);
    // The tape before goes now; its memory makes the next one.
    if (recording) recording->Clear();
    blocks = nullptr;
}

void SessionLayout::Forget() {
    active = false;
    last.reset();
    recording.reset();
    reuse = {};
}

} // namespace TinyPdf::Internal

namespace TinyPdf {

namespace {

bool SameTheme(const PdfTheme& a, const PdfTheme& b) {
    return a.fontPath == b.fontPath && a.fontBoldPath == b.fontBoldPath && a.fontItalicPath == b.fontItalicPath &&
        a.fontBoldItalicPath == b.fontBoldItalicPath && a.headingColor == b.headingColor && a.linkColor == b.linkColor &&
        a.accentColor == b.accentColor && a.headerLeft == b.headerLeft && a.headerCenter == b.headerCenter &&
        a.headerRight == b.headerRight && a.footerLeft == b.footerLeft && a.footerCenter == b.footerCenter &&
        a.footerRight == b.footerRight && a.logoPath == b.logoPath && a.cover == b.cover;
}

// Whether two builds take the same options: all of them but `cancel` and `sourceMap`.
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(PdfOptions) == 584, "a new option: compare it in SameOptions");
static_assert(sizeof(PdfTheme) == 376, "a new theme field: compare it in SameTheme");
#endif
bool SameOptions(const PdfOptions& a, const PdfOptions& b) {
    return a.style == b.style && a.margin.preset == b.margin.preset &&
        std::memcmp(&a.margin.customPoints, &b.margin.customPoints, sizeof(double)) == 0 &&
        std::memcmp(&a.pageSize.width, &b.pageSize.width, sizeof(double)) == 0 &&
        std::memcmp(&a.pageSize.height, &b.pageSize.height, sizeof(double)) == 0 &&
        a.pageSize.autoHeight == b.pageSize.autoHeight && a.sourcePath == b.sourcePath &&
        a.enableUrlImages == b.enableUrlImages && a.allowUnsafeLocalImages == b.allowUnsafeLocalImages &&
        a.embedSource == b.embedSource && a.imageCacheDirectory == b.imageCacheDirectory &&
        a.pageNumbers == b.pageNumbers && a.compress == b.compress && a.fastImages == b.fastImages && a.pdfa == b.pdfa &&
        a.pdfua == b.pdfua && a.highlightCode == b.highlightCode && a.diagrams == b.diagrams && a.toc == b.toc &&
        a.tocDepth == b.tocDepth && a.pageBreakBeforeLevel == b.pageBreakBeforeLevel &&
        a.numberHeadings == b.numberHeadings && a.dialect == b.dialect && SameTheme(a.theme, b.theme) &&
        a.footnotes == b.footnotes && a.vaultRoot == b.vaultRoot && a.embedsAsLinks == b.embedsAsLinks &&
        a.backlinks == b.backlinks && a.blackText == b.blackText && a.hideTags == b.hideTags;
}

// The session of the builds on this thread while one runs.
class SessionScope {
public:
    explicit SessionScope(Internal::LayoutSession* session) : outer(Internal::g_session) { Internal::g_session = session; }
    ~SessionScope() { Internal::g_session = outer; }
    SessionScope(const SessionScope&) = delete;
    SessionScope& operator=(const SessionScope&) = delete;

private:
    Internal::LayoutSession* outer;
};

} // namespace

struct BuildSession::State {
    Internal::SessionLayout layout;
    PdfOptions options;     // the last build's, without its cancel flag and source map
    bool built = false;
};

BuildSession::BuildSession() : state(std::make_unique<State>()) {}

BuildSession::~BuildSession() = default;

BuildResult BuildSession::Build(const std::string& markdown, const PdfOptions& options, std::string& pdfBytes) {
    State& session = *state;
    // The tape holds a layout made with the options the session has: other options, no tape.
    if (!session.built || !SameOptions(session.options, options)) {
        session.layout.Forget();
        session.options = options;
        session.options.cancel = nullptr;
        session.options.sourceMap = nullptr;
        session.built = true;
    }
    session.layout.reuse = {};
    const SessionScope scope(&session.layout);
    return BuildPdf(markdown, options, pdfBytes);
}

void BuildSession::Clear() {
    state->layout.Forget();
    state->built = false;
}

BuildSession::Reuse BuildSession::LastReuse() const {
    return state->layout.reuse;
}

} // namespace TinyPdf
