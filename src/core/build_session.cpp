// SPDX-License-Identifier: Apache-2.0

#include "build_session.h"

#include "dialect.h"
#include "embeds.h"
#include "footnotes.h"
#include "highlight.h"
#include "session_parse.h"

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
    blockCount = 0;
    blockNumbers.clear();
    contents.clear();
    numbers.clear();
    marks.clear();
    content.clear();
    pageStarts.clear();
    pageGaps.clear();
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
    const std::vector<const std::string*>* oldNumbers = nullptr;
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
    oldNumbers = nullptr;
    // The last build's blocks are the session's (SessionBlocks), as this build's parse left them.
    if (!last || last->renderer != renderer || !std::equal(fonts, fonts + kLayoutCidSets, last->fonts) ||
        last->marks.size() != last->blockCount + 1 || store == nullptr || !store->parsed ||
        store->OldCount() != last->blockCount) {
        return;
    }
    oldCount = last->blockCount;
    contentsSame = SameContents(contents, last->contents);
    if (!numbers.empty() || !last->numbers.empty()) {
        newNumbers = NumbersOf(newBlocks, numbers);
        if (last->blockNumbers.size() != oldCount) last->blockNumbers.assign(oldCount, nullptr);
        oldNumbers = &last->blockNumbers;
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
    // The blocks this build's parse kept from the last are equal to themselves.
    if ((!store->Kept(index, old) && !SameBlock(block, store->Old(old))) ||
        (block.type == BlockType::Contents && !contentsSame)) {
        return false;
    }
    if (newNumbers.empty()) return true;
    const std::string* const a = newNumbers[index];
    const std::string* const b = (*oldNumbers)[old];
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
                // A block the parse kept is the last build's: the mark's view is still good.
                text = store->Kept(place.block - from + index, place.block) ? heading.text.data() :
                    Relocate(heading.text, store->Old(place.block), block);
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
    // The content page by page, each new page after the gap this build leaves before it (the last
    // build's gaps are not copied), so that it lies where BuildInto wants it.
    std::string& content = *target.content;
    const auto pageEnd = [&](size_t page) { return old.pageStarts[page + 1] - old.pageGaps[page + 1]; };
    content.append(old.content, before.content, (after.page == before.page ? after.content : pageEnd(before.page)) - before.content);
    for (size_t page = before.page + 1; page <= after.page; page++) {
        target.startPage(content, *target.pageStarts);
        const size_t stop = page == after.page ? after.content : pageEnd(page);
        content.append(old.content, old.pageStarts[page], stop - old.pageStarts[page]);
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
        moved.content = mark.page == before.page ? mark.content - before.content + now.content :
            mark.content - old.pageStarts[mark.page] + (*target.pageStarts)[mark.page + pageShift];
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
    tape.pageGaps.resize(tape.pageStarts.size());
    for (size_t page = 0; page < tape.pageGaps.size(); page++) tape.pageGaps[page] = target.gapBefore(page);
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
    tape.blockCount = newBlocks.size();
    if (!numbers.empty()) tape.blockNumbers = NumbersOf(newBlocks, numbers);
    if (store != nullptr) CommitParse(*store);
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

// The parse side (session_parse.h). A document's block loop reads the lines from where it is on,
// never back (a pass reads at most the line after the one it ends at, an Empty line not even that),
// and the state it carries from one pass to the next is the line, the HTML elements open, the
// ParseBudget and what it made; the link definitions, the lines' classes (ResolveMathOpeners', the
// definitions' marks) and the footnotes are made before it, from the whole text. So where two texts
// have the same lines up to an Empty line at which the last parse started a pass with no HTML
// element open, the passes before it are the last parse's, and from a pass this parse starts at
// a line of the same text to the end, with the same classes, in the state the last parse had
// there, the passes are the last parse's too: their blocks are taken, moved, not made again. A
// text with footnotes or a contents marker is parsed whole (they change blocks after the loop),
// and so is one whose definitions, front matter, classes' context or budgets differ.
class DocumentParse {
public:
    DocumentParse(SessionBlocks& blocks, const ParseRecord* previous, ParseRecord& into) : store(blocks), last(previous), next(into) {}

    SessionBlocks& store;
    const ParseRecord* last;     // the parse to go on from, null for a whole one
    ParseRecord& next;           // what this one leaves
    ParseBudget* budget = nullptr;
    size_t referenceStart = 0;   // the budget's when the loop began
    size_t cellStart = 0;
    bool reuse = false;          // the loop starts at a pass of the last parse's
    bool ended = false;          // EndDocumentParse ran
    bool failed = false;         // the last parse's blocks may not be taken after all: parse whole
    bool valid = false;          // the next build may go on from this one
    size_t from = 0;             // the last parse's pass the loop starts at
    size_t front = 0;            // the blocks before it
    size_t suffixLine = 0;       // the first line from which the text and classes are the last's
    int64_t lineShift = 0;       // their lines less the last parse's
    size_t cursor = 0;           // the last parse's pass at or after the line looked for
    size_t resync = SIZE_MAX;    // the last parse's pass the loop stopped at
};

size_t FirstDifference(const char* a, const char* b, size_t size) {
    constexpr size_t kChunk = 4096;
    size_t at = 0;
    while (at + kChunk <= size && std::memcmp(a + at, b + at, kChunk) == 0) at += kChunk;
    while (at < size && a[at] == b[at]) at++;
    return at;
}

size_t CommonEnd(const char* aEnd, const char* bEnd, size_t limit) {
    constexpr size_t kChunk = 4096;
    size_t same = 0;
    while (same + kChunk <= limit && std::memcmp(aEnd - same - kChunk, bEnd - same - kChunk, kChunk) == 0) same += kChunk;
    while (same < limit && aEnd[-1 - static_cast<ptrdiff_t>(same)] == bEnd[-1 - static_cast<ptrdiff_t>(same)]) same++;
    return same;
}

namespace {

bool SameDefinitions(const ReferenceDefinitions& a, const ReferenceDefinitions& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [label, definition] : a) {
        const auto found = b.find(label);
        if (found == b.end() || found->second.destination != definition.destination || found->second.title != definition.title) {
            return false;
        }
    }
    return true;
}

// Whether blocks [first, end) hold a paragraph that may be a contents marker ("[TOC]", "[[_TOC_]]"),
// which PlaceContents makes a Contents block after the parse.
bool MayHoldMarker(const std::vector<Block>& blocks, size_t first, size_t end) {
    for (size_t index = first; index < end; index++) {
        const Block& block = blocks[index];
        if (block.type == BlockType::Paragraph && (block.text.size() == 5 || block.text.size() == 9) && block.text[0] == '[') {
            return true;
        }
    }
    return false;
}

// Where BeginDocumentParse may start the loop of `parse`, which goes on from `last`: the line, or
// SIZE_MAX to parse whole. Sets what DocumentPass and EndDocumentParse need.
size_t ResumeLine(DocumentParse& parse, const ParseRecord& last, const std::string& text,
    const std::vector<std::string_view>& lines, const std::vector<unsigned char>& suppressed,
    const ReferenceDefinitions& definitions, bool definitionSyntax, size_t first, const LineChange& change) {
    constexpr size_t kNone = SIZE_MAX;
    const ParseRecord& next = parse.next;
    const size_t count = lines.size();
    const size_t oldCount = last.kinds.size();
    if (last.winAnsi != next.winAnsi || last.definitionSyntax != definitionSyntax || last.first != first ||
        last.paddedCellsAtStart != parse.cellStart || count >= 0x7FFFFFFFu || oldCount >= 0x7FFFFFFFu ||
        last.referenceLeft < kMaxReferenceDestinationBytes || suppressed.empty() != last.suppressed.empty() ||
        parse.budget == nullptr || last.passes.empty() || last.passes[0].line != first ||
        !SameDefinitions(last.definitions, definitions)) {
        return kNone;
    }
    // The lines before the first byte that differs are the same, and so are those after the last.
    const size_t oldSize = last.text.size();
    const size_t size = text.size();
    const size_t common = std::min(oldSize, size);
    const size_t differs = change.known ? change.differs : FirstDifference(last.text.data(), text.data(), common);
    const char* const base = text.data();
    const auto lineStart = [&](size_t line) { return static_cast<size_t>(lines[line].data() - base); };
    size_t changed = count;      // the first line that may differ
    size_t suffix = count;       // the first of the lines after the change that are the last's
    if (differs != common || oldSize != size) {
        // The line that holds the first byte that differs (a line holds its line feed).
        size_t low = 0;
        size_t high = count;
        while (low < high) {
            const size_t middle = low + (high - low) / 2;
            if (lineStart(middle) <= differs) low = middle + 1;
            else high = middle;
        }
        changed = low == 0 ? 0 : low - 1;
        // A line after it whose line feed before it is in the end both texts share.
        const size_t same = change.known ? change.same : CommonEnd(last.text.data() + oldSize, base + size, common - differs);
        low = changed + 1;
        high = count;
        while (low < high) {
            const size_t middle = low + (high - low) / 2;
            if (lineStart(middle) >= size - same + 1) high = middle;
            else low = middle + 1;
        }
        suffix = low;
    }
    const int64_t shift = static_cast<int64_t>(count) - static_cast<int64_t>(oldCount);
    if (static_cast<int64_t>(suffix) - shift < 0 || static_cast<int64_t>(suffix) - shift > static_cast<int64_t>(oldCount)) {
        return kNone;
    }
    // The lines' classes and definition marks, which their context gives, the same as well (where
    // the parse's classes come from the last's, only those it classified again may differ).
    const std::vector<uint8_t>& kinds = next.kinds;
    const auto sameLine = [&](size_t line, size_t old) {
        return kinds[line] == last.kinds[old] && (suppressed.empty() || suppressed[line] == last.suppressed[old]);
    };
    for (size_t line = change.known ? std::min(change.classFirst, changed) : 0; line < changed; line++) {
        if (!sameLine(line, line)) {
            changed = line;
            break;
        }
    }
    for (size_t line = change.known ? std::max(std::min(change.classEnd, count), suffix) : count; line > suffix; line--) {
        if (!sameLine(line - 1, static_cast<size_t>(static_cast<int64_t>(line - 1) - shift))) {
            suffix = line;
            break;
        }
    }
    const std::vector<ParsePass>& passes = last.passes;
    if (changed == count) {
        // The same text: every pass is the last parse's, and the loop starts past the end.
        parse.reuse = true;
        parse.from = passes.size();
        parse.cursor = passes.size();
        parse.front = last.blocks;
        parse.suffixLine = count;
        parse.lineShift = 0;
        if (last.referenceBytes > parse.referenceStart) return kNone;
        parse.budget->referenceBytes = parse.referenceStart - static_cast<size_t>(last.referenceBytes);
        parse.next.passes.assign(passes.begin(), passes.end());
        return count;
    }
    suffix = std::max(suffix, changed + 1);
    // The pass to start at: the last one before the change at an Empty line with no element open
    // (every pass before it read no further than that line), else the loop's first.
    size_t from = 0;
    {
        size_t low = 0;
        size_t high = passes.size();
        while (low < high) {
            const size_t middle = low + (high - low) / 2;
            if (passes[middle].line < changed) low = middle + 1;
            else high = middle;
        }
        for (size_t pass = low; pass-- > 1;) {
            const ParsePass& at = passes[pass];
            if (at.closed != 0 && last.kinds[at.line] == kEmptyLineKind) {
                from = pass;
                break;
            }
        }
    }
    const ParsePass& start = passes[from];
    if (start.referenceBytes > parse.referenceStart || start.paddedCells > parse.cellStart) return kNone;
    parse.reuse = true;
    parse.from = from;
    parse.cursor = from;
    parse.front = start.blocks;
    parse.suffixLine = suffix;
    parse.lineShift = shift;
    parse.budget->referenceBytes = parse.referenceStart - static_cast<size_t>(start.referenceBytes);
    parse.budget->paddedCells = parse.cellStart - start.paddedCells;
    parse.next.passes.assign(passes.begin(), passes.begin() + static_cast<ptrdiff_t>(from));
    return start.line;
}

} // namespace

const ParseRecord* LastRecord(const DocumentParse& parse) {
    return parse.last;
}

ParseRecord& NextRecord(DocumentParse& parse) {
    return parse.next;
}

size_t BeginDocumentParse(DocumentParse& parse, const std::vector<std::string_view>& lines,
    const std::vector<unsigned char>& suppressed, const ReferenceDefinitions& definitions, bool definitionSyntax,
    size_t first, const LineChange& change, const FootnoteParse& notes) {
    ParseRecord& next = parse.next;
    parse.budget = ParseBudget::Current();
    parse.referenceStart = parse.budget != nullptr ? parse.budget->referenceBytes : 0;
    parse.cellStart = parse.budget != nullptr ? parse.budget->paddedCells : 0;
    next.first = first;
    next.definitionSyntax = definitionSyntax;
    next.winAnsi = WinAnsiReferences::Active();
    next.paddedCellsAtStart = parse.cellStart;
    next.passes.clear();
    size_t start = first;
    // The last parse had no footnotes (a record is kept only then); this one's prefix and suffix
    // passes, the same as the last's, then define none and wait for none either.
    (void)notes;
    if (parse.last != nullptr) {
        const size_t resumed = ResumeLine(parse, *parse.last, next.text, lines, suppressed, definitions, definitionSyntax,
            first, change);
        if (resumed != SIZE_MAX) start = resumed;
    }
    return start;
}

bool DocumentPass(DocumentParse& parse, size_t line, size_t blocks, bool closed) {
    const uint64_t references = parse.budget != nullptr ? parse.referenceStart - parse.budget->referenceBytes : 0;
    const uint32_t cells = parse.budget != nullptr ? static_cast<uint32_t>(parse.cellStart - parse.budget->paddedCells) : 0;
    if (parse.reuse && line >= parse.suffixLine) {
        const std::vector<ParsePass>& passes = parse.last->passes;
        const size_t old = static_cast<size_t>(static_cast<int64_t>(line) - parse.lineShift);
        while (parse.cursor < passes.size() && passes[parse.cursor].line < old) parse.cursor++;
        if (parse.cursor < passes.size()) {
            const ParsePass& pass = passes[parse.cursor];
            if (pass.line == old && pass.closed != 0 && closed && pass.paddedCells == cells) {
                parse.resync = parse.cursor;
                return true;
            }
        }
    }
    parse.next.passes.push_back({ static_cast<uint32_t>(line), static_cast<uint32_t>(parse.front + blocks), references, cells,
        closed ? 1u : 0u });
    return false;
}

void EndDocumentParse(DocumentParse& parse, std::vector<Block>& blocks, size_t line, ReferenceDefinitions& definitions,
    const FootnoteParse& notes) {
    (void)line;
    parse.ended = true;
    SessionBlocks& store = parse.store;
    ParseRecord& next = parse.next;
    const uint64_t used = parse.budget != nullptr ? parse.referenceStart - parse.budget->referenceBytes : 0;
    size_t left = parse.budget != nullptr ? parse.budget->referenceBytes : 0;
    // Footnotes number the blocks after the loop, and a marker becomes a Contents block.
    const bool clean = notes.bodies.empty() && !notes.pending;
    next.definitions.swap(definitions);
    if (!parse.reuse) {
        store.displaced.swap(store.blocks);
        store.blocks.clear();
        store.spliced = false;
        next.blocks = blocks.size();
        next.referenceBytes = used;
        next.referenceLeft = left;
        parse.valid = clean && !MayHoldMarker(blocks, 0, blocks.size());
        store.made = blocks.size();
        return;
    }
    const ParseRecord& last = *parse.last;
    const bool resynced = parse.resync != SIZE_MAX;
    uint64_t total = used;
    if (resynced) {
        // The passes after it use what they used in the last parse.
        const uint64_t after = last.referenceBytes - last.passes[parse.resync].referenceBytes;
        total += after;
        left = left >= after ? left - static_cast<size_t>(after) : 0;
    }
    // No reference was refused in either parse (every destination is shorter than what was left at
    // the end), so that each pass used what it would in a whole parse of its text.
    if (left < kMaxReferenceDestinationBytes) {
        parse.failed = true;
        blocks.clear();
        return;
    }
    const size_t oldEnd = resynced ? last.passes[parse.resync].blocks : last.blocks;
    const size_t front = parse.front;
    const size_t oldWindow = oldEnd - front;
    const size_t newWindow = blocks.size();
    std::vector<Block> out = std::move(store.blocks);
    store.displaced.clear();
    store.displaced.reserve(oldWindow);
    for (size_t index = front; index < oldEnd; index++) store.displaced.push_back(std::move(out[index]));
    const size_t common = std::min(oldWindow, newWindow);
    for (size_t index = 0; index < common; index++) out[front + index] = std::move(blocks[index]);
    if (newWindow < oldWindow) {
        out.erase(out.begin() + static_cast<ptrdiff_t>(front + newWindow), out.begin() + static_cast<ptrdiff_t>(oldEnd));
    } else if (newWindow > oldWindow) {
        out.insert(out.begin() + static_cast<ptrdiff_t>(oldEnd), std::make_move_iterator(blocks.begin() + static_cast<ptrdiff_t>(common)),
            std::make_move_iterator(blocks.end()));
    }
    // The blocks after it stand on lines moved by as many as the edit added.
    const uint32_t shift = static_cast<uint32_t>(parse.lineShift);
    if (shift != 0) {
        for (size_t index = front + newWindow; index < out.size(); index++) out[index].sourceLine += shift;
    }
    store.spliced = true;
    store.made = newWindow;
    store.front = front;
    store.oldWindow = oldWindow;
    store.newWindow = newWindow;
    store.lineShift = parse.lineShift;
    if (resynced) {
        const int64_t blockShift = static_cast<int64_t>(newWindow) - static_cast<int64_t>(oldWindow);
        const uint64_t referenceShift = used - last.passes[parse.resync].referenceBytes;
        next.passes.reserve(next.passes.size() + last.passes.size() - parse.resync);
        for (size_t pass = parse.resync; pass < last.passes.size(); pass++) {
            ParsePass moved = last.passes[pass];
            moved.line = static_cast<uint32_t>(moved.line + shift);
            moved.blocks = static_cast<uint32_t>(static_cast<int64_t>(moved.blocks) + blockShift);
            moved.referenceBytes += referenceShift;
            next.passes.push_back(moved);
        }
    }
    next.blocks = out.size();
    next.referenceBytes = total;
    next.referenceLeft = left;
    parse.valid = clean && !MayHoldMarker(out, front, front + newWindow);
    blocks.swap(out);
}

bool SessionParses() {
    return g_session != nullptr && g_session->store != nullptr;
}

std::vector<Block> ParseInSession(const std::string& markdown, Footnotes& footnotes) {
    SessionBlocks& store = *g_session->store;
    // A second parse of this build (in another font): from the last build's blocks again.
    UndoParse(store);
    store.parsed = true;
    store.spliced = false;
    store.displaced.clear();
    const auto parse = [&](DocumentParse* document) {
        SetDocumentParse(document);
        std::vector<Block> blocks;
        if (ObsidianDialect()) {
            std::string converted;
            const std::string& text = RewriteDocument(markdown, converted) ? converted : markdown;
            const ParseBudget budget(text.size());
            blocks = ParseWithFootnotes(text, footnotes);
        } else {
            const ParseBudget budget(markdown.size());
            blocks = ParseWithFootnotes(markdown, footnotes);
        }
        SetDocumentParse(nullptr);
        // A parse that did not reach the document's loop (embeds) keeps none of the last build's.
        if (document == nullptr || !document->ended) {
            store.displaced.swap(store.blocks);
            store.blocks.clear();
        }
        return blocks;
    };
    // Embeds are spliced in after the parse: parsed whole, and nothing kept for the next build.
    if (EmbedsActive()) {
        store.pending.reset();
        std::vector<Block> blocks = parse(nullptr);
        store.made = blocks.size();
        return blocks;
    }
    if (!store.pending) store.pending = store.spare ? std::move(store.spare) : std::make_unique<ParseRecord>();
    const ParseRecord* const last = store.reusable ? store.record.get() : nullptr;
    DocumentParse document(store, last, *store.pending);
    std::vector<Block> blocks = parse(&document);
    bool valid = document.ended && document.valid;
    if (document.failed) {
        DocumentParse whole(store, nullptr, *store.pending);
        blocks = parse(&whole);
        valid = whole.ended && whole.valid;
    }
    if (!valid || !store.reusable) store.spare = std::move(store.pending);
    return blocks;
}

void CommitParse(SessionBlocks& session) {
    if (!session.parsed) return;
    session.parsed = false;
    session.spliced = false;
    session.displaced.clear();
    // The record before goes; its memory makes a later one.
    if (session.record) session.spare = std::move(session.record);
    session.record = std::move(session.pending);
}

void UndoParse(SessionBlocks& session) {
    if (!session.parsed) return;
    session.parsed = false;
    if (session.pending) session.spare = std::move(session.pending);
    std::vector<Block>& blocks = session.blocks;
    if (!session.spliced) {
        blocks.swap(session.displaced);
        session.displaced.clear();
        return;
    }
    session.spliced = false;
    const size_t front = session.front;
    const size_t oldWindow = session.oldWindow;
    const size_t newWindow = session.newWindow;
    const uint32_t shift = static_cast<uint32_t>(session.lineShift);
    if (shift != 0) {
        for (size_t index = front + newWindow; index < blocks.size(); index++) blocks[index].sourceLine -= shift;
    }
    std::vector<Block>& displaced = session.displaced;
    const size_t common = std::min(oldWindow, newWindow);
    for (size_t index = 0; index < common; index++) blocks[front + index] = std::move(displaced[index]);
    if (oldWindow < newWindow) {
        blocks.erase(blocks.begin() + static_cast<ptrdiff_t>(front + oldWindow), blocks.begin() + static_cast<ptrdiff_t>(front + newWindow));
    } else if (oldWindow > newWindow) {
        blocks.insert(blocks.begin() + static_cast<ptrdiff_t>(front + newWindow),
            std::make_move_iterator(displaced.begin() + static_cast<ptrdiff_t>(common)), std::make_move_iterator(displaced.end()));
    }
    displaced.clear();
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
    State() { layout.store = &blocks; }
    Internal::SessionBlocks blocks;
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
        session.blocks.record.reset();
        session.options = options;
        session.options.cancel = nullptr;
        session.options.sourceMap = nullptr;
        session.built = true;
    }
    // Blocks that the build changes after the parse (a table of contents put in, page breaks before
    // headings) are not what a parse makes: the next build parses whole.
    session.blocks.reusable = !options.toc && options.pageBreakBeforeLevel <= 0;
    session.layout.reuse = {};
    BuildResult result;
    {
        const SessionScope scope(&session.layout);
        result = BuildPdf(markdown, options, pdfBytes);
    }
    // A parse the layout did not commit: kept when the build was made (footnotes, PDF/UA, whose
    // layout keeps no tape), else the last build's blocks are put back for the tape that is left.
    if (session.blocks.parsed) {
        if (result.error == BuildError::None) {
            Internal::CommitParse(session.blocks);
            session.layout.Forget();
        } else {
            Internal::UndoParse(session.blocks);
        }
    }
    session.layout.reuse.parsed = static_cast<uint32_t>(session.blocks.made);
    return result;
}

void BuildSession::Clear() {
    state->layout.Forget();
    state->blocks = {};
    state->layout.store = &state->blocks;
    state->built = false;
}

BuildSession::Reuse BuildSession::LastReuse() const {
    return state->layout.reuse;
}

} // namespace TinyPdf
