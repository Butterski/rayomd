// SPDX-License-Identifier: Apache-2.0

#include "build_session.h"

#include "../common/text_utils.h"

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

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOMD_SESSION_SSE2 1
#endif

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
// block stands in the Markdown (sourceLine, sourceLines), which only the source map takes; but
// whether it has a place there, which gives it an entry of the map or none (a table of contents
// inserted by --toc has none, one at a "[TOC]" paragraph the paragraph's lines).
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Block) == 136, "a new field of Block: compare it in SameBlock");
#endif
bool SameBlock(const Block& a, const Block& b) {
    if (a.type != b.type || a.level != b.level || a.number != b.number || a.task != b.task || a.alert != b.alert ||
        a.notesPending != b.notesPending || a.codeLanguage != b.codeLanguage || a.hasMath != b.hasMath ||
        a.align != b.align || (a.sourceLine == kNoSourceLine) != (b.sourceLine == kNoSourceLine) || a.text != b.text ||
        a.aligns != b.aligns || a.rows != b.rows ||
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
    blockData = nullptr;
    blockNumbers.clear();
    contents.clear();
    numbers.clear();
    marks.clear();
    pieces.clear();
    buffers.clear();
    pageStarts.clear();
    pageFree.clear();
    pageLinks.clear();
    headings.clear();
    ids.clear();
    contentsLines.clear();
    sourceMap.clear();
    sourceMapBlocks.clear();
    for (std::vector<uint16_t>& list : cids) list.clear();
    glyphs.clear();
    pageBottoms.clear();
    images.clear();
    headingPlaces.clear();
    idPlaces.clear();
}

void LayoutTape::Reserve(const LayoutTape& like) {
    marks.reserve(like.marks.size() + like.marks.size() / 16);
    pieces.reserve(like.pieces.size() + like.pieces.size() / 16);
    buffers.reserve(like.buffers.size() + 1);
    pageStarts.reserve(like.pageStarts.size() + like.pageStarts.size() / 16);
    pageFree.reserve(like.pageFree.size() + like.pageFree.size() / 16);
    sourceMap.reserve(like.sourceMap.size() + like.sourceMap.size() / 16);
    sourceMapBlocks.reserve(like.sourceMapBlocks.size() + like.sourceMapBlocks.size() / 16);
    for (size_t set = 0; set < kLayoutCidSets; set++) cids[set].reserve(like.cids[set].size() + 64);
    glyphs.reserve(like.glyphs.size() + 16);
    pageBottoms.reserve(like.pageBottoms.size() + like.pageBottoms.size() / 16);
    images.reserve(like.images.size() + like.images.size() / 16);
    headingPlaces.reserve(like.headingPlaces.size() + like.headingPlaces.size() / 16);
    idPlaces.reserve(like.idPlaces.size() + like.idPlaces.size() / 16);
}

namespace {

// "]:" in `text`.
size_t CountPairs(std::string_view text) {
    size_t count = 0;
    for (size_t at = text.find("]:"); at != std::string_view::npos; at = text.find("]:", at + 2)) count++;
    return count;
}

// The pairs a change of bytes [from, to) of `text` may make or take: those with a byte in it, or
// right before it ("]" before, ":" first).
size_t PairsAround(std::string_view text, size_t from, size_t to) {
    const size_t low = from > 0 ? from - 1 : 0;
    return CountPairs(text.substr(low, std::min(text.size(), to + 1) - low));
}

// The bytes from 0x80 of `text`: sixteen at a time where SSE2 is (each a 1 or a 0 added up by
// PSADBW), else eight (at -Os a byte at a time took 4 instructions a byte).
size_t CountHigh(std::string_view text) {
    const char* at = text.data();
    const char* const end = at + text.size();
    size_t count = 0;
#ifdef RAYOMD_SESSION_SSE2
    const __m128i ones = _mm_set1_epi8(1);
    __m128i sums = _mm_setzero_si128();
    for (; end - at >= 16; at += 16) {
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(at));
        sums = _mm_add_epi64(sums, _mm_sad_epu8(_mm_and_si128(_mm_srli_epi64(bytes, 7), ones), _mm_setzero_si128()));
    }
    count = static_cast<size_t>(_mm_cvtsi128_si64(sums)) + static_cast<size_t>(_mm_cvtsi128_si64(_mm_unpackhi_epi64(sums, sums)));
#else
    constexpr uint64_t kOnes = 0x0101010101010101ull;
    while (end - at >= 8) {
        // Each byte of `sum` counts to at most 255 before it is added up.
        uint64_t sum = 0;
        for (int step = 0; step < 255 && end - at >= 8; step++, at += 8) {
            uint64_t word;
            std::memcpy(&word, at, 8);
            sum += (word >> 7) & kOnes;
        }
        // Added up in four 16-bit lanes of at most 510, then to at most 2,040.
        const uint64_t lanes = (sum & 0x00FF00FF00FF00FFull) + ((sum >> 8) & 0x00FF00FF00FF00FFull);
        count += static_cast<size_t>((lanes * 0x0001000100010001ull) >> 48);
    }
#endif
    for (; at != end; at++) count += static_cast<unsigned char>(*at) >> 7;
    return count;
}

} // namespace

void KeptText::Change(size_t from, size_t to, std::string_view with, uint64_t next) {
    replacedPairs = pairs;
    pairs -= PairsAround(bytes, from, to);
    replaced.assign(bytes, from, to - from);
    // Room for a text a sixteenth longer, so that a letter typed does not move it.
    const size_t size = bytes.size() - (to - from) + with.size();
    if (size > bytes.capacity()) bytes.reserve(size + size / 16);
    bytes.replace(from, to - from, with.data(), with.size());
    pairs += PairsAround(bytes, from, from + with.size());
    base = version;
    version = next;
    differs = from;
    oldEnd = to;
    newEnd = from + with.size();
    undoable = true;
}

void KeptText::Set(std::string_view with, uint64_t next) {
    if (with.size() > bytes.capacity()) bytes.reserve(with.size() + with.size() / 16);
    bytes.assign(with.data(), with.size());
    pairs = CountPairs(bytes);
    version = next;
    base = 0;
    undoable = false;
}

void KeptText::Take(std::string& with, uint64_t next) {
    bytes.swap(with);
    pairs = CountPairs(bytes);
    version = next;
    base = 0;
    undoable = false;
}

void KeptText::Undo(uint64_t fresh) {
    if (undoable) {
        bytes.replace(differs, newEnd - differs, replaced);
        pairs = replacedPairs;
        version = base;
    } else {
        version = fresh;
    }
    base = 0;
    undoable = false;
}

// A session's text from build to build (BuildSession::Build): the markdown and, while the standard
// fonts show it in WinAnsiEncoding, its WinAnsi text, each changed in place where the next build's
// differs; and what ChooseFonts reads of the markdown (bytes from 0x80, character references) as
// counts kept up to date, so that the fonts are chosen without reading all of it.
class SessionText {
public:
    // This build's markdown becomes the kept text, which the build then reads.
    const std::string& Begin(const std::string& text);
    // The build was made; or it was not, and the last build's text is back.
    void Commit() { changed = winAnsiChanged = false; }
    void Undo();
    // LayoutSession::TextFonts.
    uint8_t Fonts(const std::string& text, const std::string*& winAnsiText);
    const KeptText* Kept(const std::string& text) const {
        return &text == &markdown.bytes ? &markdown : &text == &winAnsi.bytes ? &winAnsi : nullptr;
    }

private:
    // A place in the markdown at the start of a character and where its WinAnsi text has it.
    struct Checkpoint {
        size_t text;
        size_t winAnsi;
    };
    // A character reference reads at most ten bytes after its '&' (MatchCharacterReference).
    static constexpr size_t kReferenceReach = 16;
    static constexpr size_t kChunk = 16384;   // between checkpoints, as the whole text is transcoded

    // Adds `sign` times the references of `text` that start in [from, to) to `references`.
    static void CountReferences(std::string_view text, size_t from, size_t to, size_t* counts, int sign);
    // winAnsi becomes the WinAnsi text of the markdown; false when the markdown has a character
    // WinAnsiEncoding lacks (RayoMd::Text::TranscodeToWinAnsi).
    bool TranscodeChange();
    bool TranscodeWhole();
    // Where the markdown's character at `at` is in winAnsi, from the checkpoint before it.
    size_t WinAnsiAt(size_t at);

    KeptText markdown;
    KeptText winAnsi;
    uint64_t versions = 0;
    uint64_t winAnsiOf = 0;      // the markdown's version winAnsi is the WinAnsi text of; 0 none
    size_t nonAscii = 0;         // bytes from 0x80 in the markdown
    size_t references[2] = {};   // its character references that need WinAnsiEncoding, a Unicode font
    std::vector<Checkpoint> checkpoints;
    // What Undo puts back.
    bool changed = false;
    bool winAnsiChanged = false;
    uint64_t lastWinAnsiOf = 0;
    size_t lastNonAscii = 0;
    size_t lastReferences[2] = {};
    std::vector<Checkpoint> lastCheckpoints;
    std::string scratch;
    std::string piece;          // a change or a piece, transcoded
    std::string previous;
};

void SessionText::CountReferences(std::string_view text, size_t from, size_t to, size_t* counts, int sign) {
    for (size_t at = text.find('&', from); at < to; at = text.find('&', at + 1)) {
        const ReferenceNeed need = CharacterReferenceNeedAt(text, at);
        if (need != ReferenceNeed::Ascii) counts[need == ReferenceNeed::WinAnsi ? 0 : 1] += static_cast<size_t>(sign);
    }
}

const std::string& SessionText::Begin(const std::string& text) {
    changed = winAnsiChanged = false;
    std::string& kept = markdown.bytes;
    if (markdown.version == 0) {
        markdown.Set(text, ++versions);
        nonAscii = CountHigh(kept);
        references[0] = references[1] = 0;
        CountReferences(kept, 0, kept.size(), references, 1);
        changed = true;
        return kept;
    }
    const size_t common = std::min(kept.size(), text.size());
    const size_t differs = FirstDifference(kept.data(), text.data(), common);
    if (differs == common && kept.size() == text.size()) return kept;
    const size_t same = CommonEnd(kept.data() + kept.size(), text.data() + text.size(), common - differs);
    const size_t oldEnd = kept.size() - same;
    const size_t newEnd = text.size() - same;
    lastNonAscii = nonAscii;
    std::copy(std::begin(references), std::end(references), lastReferences);
    const auto high = [](const std::string& bytes, size_t from, size_t to) {
        return CountHigh(std::string_view(bytes).substr(from, to - from));
    };
    // The references that read a byte of the change: those that start up to kReferenceReach before it.
    const size_t reach = differs > kReferenceReach ? differs - kReferenceReach : 0;
    nonAscii -= high(kept, differs, oldEnd);
    CountReferences(kept, reach, oldEnd, references, -1);
    markdown.Change(differs, oldEnd, std::string_view(text).substr(differs, newEnd - differs), ++versions);
    nonAscii += high(kept, differs, newEnd);
    CountReferences(kept, reach, newEnd, references, 1);
    changed = true;
    return kept;
}

void SessionText::Undo() {
    if (winAnsiChanged) {
        winAnsi.Undo(++versions);
        checkpoints.swap(lastCheckpoints);
        winAnsiOf = lastWinAnsiOf;
    }
    if (changed) {
        // A first text has nothing before it: the same bytes as another version.
        if (markdown.base == 0) {
            markdown.Undo(++versions);
            winAnsiOf = 0;
        } else {
            markdown.Undo(0);
            nonAscii = lastNonAscii;
            std::copy(std::begin(lastReferences), std::end(lastReferences), references);
        }
    }
    changed = winAnsiChanged = false;
}

uint8_t SessionText::Fonts(const std::string& text, const std::string*& winAnsiText) {
    if (&text != &markdown.bytes) return LayoutSession::kNotKept;
    // As ChooseFonts: ASCII without references beyond it, then the WinAnsi text when the references
    // need no Unicode font and every character has a WinAnsi code; else a TrueType font.
    if (references[1] != 0) return 2;
    if (nonAscii == 0 && references[0] == 0) return 0;
    if (winAnsiOf != markdown.version && !(winAnsiOf != 0 && winAnsiOf == markdown.base ? TranscodeChange() : TranscodeWhole())) {
        return 2;
    }
    winAnsiText = &winAnsi.bytes;
    return 1;
}

size_t SessionText::WinAnsiAt(size_t at) {
    const auto after = std::upper_bound(checkpoints.begin(), checkpoints.end(), at,
        [](size_t value, const Checkpoint& point) { return value < point.text; });
    const Checkpoint& from = *(after - 1);
    RayoMd::Text::TranscodeToWinAnsi(std::string_view(markdown.bytes).substr(from.text, at - from.text), &scratch);
    return from.winAnsi + scratch.size();
}

bool SessionText::TranscodeChange() {
    // The markdown's change as whole characters [start, end) (each transcodes on its own, as
    // TranscodeToWinAnsi does it, when the whole text has a WinAnsi text) and its last version's.
    const std::string& text = markdown.bytes;
    const auto continuation = [](char ch) { return (static_cast<unsigned char>(ch) & 0xC0) == 0x80; };
    size_t start = markdown.differs;
    while (start > 0 && continuation(text[start])) start--;
    size_t end = markdown.newEnd;
    while (end < text.size() && continuation(text[end])) end++;
    const size_t oldEnd = markdown.oldEnd + (end - markdown.newEnd);
    if (!RayoMd::Text::TranscodeToWinAnsi(std::string_view(text).substr(start, end - start), &piece)) return false;
    const size_t at = WinAnsiAt(start);
    scratch.assign(text, start, markdown.differs - start);
    scratch += markdown.Replaced();
    scratch.append(text, markdown.newEnd, end - markdown.newEnd);
    if (!RayoMd::Text::TranscodeToWinAnsi(scratch, &previous)) return TranscodeWhole();
    lastCheckpoints = checkpoints;
    lastWinAnsiOf = winAnsiOf;
    winAnsi.Change(at, at + previous.size(), piece, ++versions);
    winAnsiChanged = true;
    // TranscodeToWinAnsi refuses a text whose WinAnsi bytes would read as a status symbol's UTF-8
    // (E2 9C 85, E2 9A A0, E2 9D 8C): the change may make one with the bytes around it.
    const std::string& bytes = winAnsi.bytes;
    const size_t stop = std::min(bytes.size(), at + piece.size() + 2);
    for (size_t index = at > 2 ? at - 2 : 0; index + 2 < stop; index++) {
        if (static_cast<unsigned char>(bytes[index]) != 0xE2) continue;
        const unsigned pair = (static_cast<unsigned>(static_cast<unsigned char>(bytes[index + 1])) << 8) |
            static_cast<unsigned char>(bytes[index + 2]);
        if (pair == 0x9C85u || pair == 0x9AA0u || pair == 0x9D8Cu) {
            winAnsi.Undo(++versions);
            winAnsiChanged = false;
            return false;
        }
    }
    // The checkpoints in the change go; those after it move with it.
    const ptrdiff_t textShift = static_cast<ptrdiff_t>(end) - static_cast<ptrdiff_t>(oldEnd);
    const ptrdiff_t winAnsiShift = static_cast<ptrdiff_t>(piece.size()) - static_cast<ptrdiff_t>(previous.size());
    size_t keep = static_cast<size_t>(std::upper_bound(checkpoints.begin(), checkpoints.end(), start,
        [](size_t value, const Checkpoint& point) { return value < point.text; }) - checkpoints.begin());
    size_t from = static_cast<size_t>(std::upper_bound(checkpoints.begin(), checkpoints.end(), oldEnd,
        [](size_t value, const Checkpoint& point) { return value < point.text; }) - checkpoints.begin());
    std::vector<Checkpoint> moved(checkpoints.begin() + static_cast<ptrdiff_t>(from), checkpoints.end());
    checkpoints.resize(keep);
    if (end < text.size()) checkpoints.push_back({ end, at + piece.size() });
    for (const Checkpoint& point : moved) {
        checkpoints.push_back({ static_cast<size_t>(static_cast<ptrdiff_t>(point.text) + textShift),
            static_cast<size_t>(static_cast<ptrdiff_t>(point.winAnsi) + winAnsiShift) });
    }
    winAnsiOf = markdown.version;
    return true;
}

bool SessionText::TranscodeWhole() {
    // In pieces that end before an ASCII byte, where no status symbol's bytes can be split.
    const std::string& text = markdown.bytes;
    lastCheckpoints.swap(checkpoints);
    checkpoints.clear();
    checkpoints.push_back({ 0, 0 });
    lastWinAnsiOf = 0;   // undone, the WinAnsi text is of no markdown
    scratch.clear();
    scratch.reserve(text.size());
    for (size_t at = 0; at < text.size();) {
        size_t end = std::min(text.size(), at + kChunk);
        while (end < text.size() && static_cast<unsigned char>(text[end]) >= 0x80) end++;
        if (!RayoMd::Text::TranscodeToWinAnsi(std::string_view(text).substr(at, end - at), &piece)) {
            checkpoints.swap(lastCheckpoints);
            return false;
        }
        scratch += piece;
        at = end;
        if (at < text.size()) checkpoints.push_back({ at, scratch.size() });
    }
    winAnsi.Take(scratch, ++versions);
    winAnsiChanged = true;
    winAnsiOf = markdown.version;
    return true;
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
    uint8_t TextFonts(const std::string& markdown, const std::string*& winAnsi) override {
        return sessionText->Fonts(markdown, winAnsi);
    }

    BuildSession::Reuse reuse;
    SessionText* sessionText = nullptr;   // the session's text, which its builds read

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
    // Appends the last build's content [from, to) (without gaps) to `content`, noting where it went.
    void CopyContent(size_t from, size_t to, std::string& content);
    // Moves the buffers no tape holds any more from `tape` to the spares.
    void Recycle(LayoutTape& tape);
    // A buffer of at least `bytes` bytes, empty: a spare one when there is one.
    std::shared_ptr<std::string> Buffer(size_t bytes);

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
    // The content replayed in this build: its place [start, end) in this build's content (on one
    // page) and where it was in the last build's without gaps.
    struct Copied {
        size_t start;
        size_t end;
        size_t from;
    };
    std::vector<Copied> copied;
    std::vector<uint32_t> bufferMap;   // EndLayout: the last tape's buffers in this one's
    std::vector<std::shared_ptr<std::string>> spareBuffers;   // buffers no tape holds, for the next
};

void SessionLayout::Begin(uint8_t renderer, const void* const fonts[kLayoutCidSets], const std::vector<Block>& newBlocks,
    const std::vector<ContentsEntry>& contents, const std::vector<HeadingNumber>& numbers) {
    if (!recording) {
        recording = std::make_unique<LayoutTape>();
        // A session's first tapes: as large as the last, so that they do not grow a step at a time.
        if (last) recording->Reserve(*last);
    } else {
        Recycle(*recording);
        recording->Clear();
    }
    copied.clear();
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
        last->marks.size() != last->blockCount + 1 || last->images.size() != last->blockCount || store == nullptr ||
        !store->parsed || store->OldCount() != last->blockCount) {
        return;
    }
    oldCount = last->blockCount;
    contentsSame = SameContents(contents, last->contents);
    if (!numbers.empty() || !last->numbers.empty()) {
        newNumbers = NumbersOf(newBlocks, numbers);
        if (last->blockNumbers.size() != oldCount) last->blockNumbers.assign(oldCount, nullptr);
        oldNumbers = &last->blockNumbers;
    }
    // The blocks are compared only when a layout reaches one in the state the last build had there
    // (RunFrom), which after an edit that moved the text on the page may never happen: then nothing
    // was compared in vain.
    std::fill(runEnd.begin(), runEnd.end(), kUnknown);
    // With as many blocks as before, every block stands for the last build's in its place; else
    // those after the first that changed are shifted by as many as were added or removed. The blocks
    // before the first the parse made are the last build's, equal to themselves (a spliced parse has
    // no table of contents).
    if (blockCount != oldCount) {
        const size_t common = std::min(blockCount, oldCount);
        size_t prefix = store->spliced && newNumbers.empty() && contentsSame ? std::min(store->front, common) : 0;
        while (prefix < common && Same(prefix, prefix)) prefix++;
        suffixStart = prefix;
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
    // A run never crosses from the blocks before the change to those after it, shifted.
    const size_t stop = index < suffixStart ? suffixStart : blockCount;
    // The blocks the parse kept stand for the last build's blocks they are (equal; a spliced parse
    // has no table of contents): only whether they read images is looked at.
    const SessionBlocks& kept = *store;
    const bool fast = kept.spliced && newNumbers.empty() && contentsSame;
    const size_t suffix = kept.front + kept.newWindow;
    size_t end = index;
    while (end < stop) {
        if (runEnd[end] != kUnknown) {
            if (runEnd[end] != 0) end = runEnd[end];
            break;
        }
        if (fast) {
            size_t to = end;
            if (end < kept.front) to = std::min(kept.front, stop);
            else if (end >= suffix && (end >= suffixStart || blockCount == oldCount)) to = stop;
            if (to > end) {
                const uint8_t* const bits = last->images.data() + OldIndex(end);
                const void* const image = std::memchr(bits, 1, to - end);
                if (image == nullptr) {
                    end = to;
                    continue;
                }
                end += static_cast<size_t>(static_cast<const uint8_t*>(image) - bits);
                runEnd[end] = 0;
                break;
            }
        }
        const bool mapped = end < suffixStart || end + oldCount >= blockCount;
        // An equal block reads images as the last build's did.
        if (!mapped || OldIndex(end) >= oldCount || !Same(end, OldIndex(end)) || last->images[OldIndex(end)] != 0) {
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
    // Block index - 1 was laid out (a replay notes its own blocks): whether it reads images, the
    // source map's entries since the last mark, and where the marks it made view their text.
    if (index > tape.images.size()) {
        const size_t block = index - 1;
        const Block& laid = (*blocks)[block];
        tape.images.push_back(TouchesImages(laid) ? 1 : 0);
        tape.sourceMapBlocks.resize(tape.sourceMap.size(), static_cast<uint32_t>(block));
        const std::string& text = laid.text;
        const auto place = [&](const std::vector<HeadingMark>& marks, std::vector<LayoutTape::MarkPlace>& places) {
            for (size_t at = places.size(); at < marks.size(); at++) {
                const char* const view = marks[at].text.data();
                const bool own = view >= text.data() && view + marks[at].text.size() <= text.data() + text.size();
                places.push_back({ static_cast<uint32_t>(block),
                    own ? static_cast<uint32_t>(view - text.data()) : LayoutTape::MarkPlace::kNested });
            }
        };
        place(*target.headings, tape.headingPlaces);
        place(*target.ids, tape.idPlaces);
    }
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
    // equal to the last build's, by where they were in them. A block the parse kept, where the last
    // build had it, is the last build's: its marks' views are still good.
    const Block* const data = blocks->data();
    const auto move = [&](const std::vector<HeadingMark>& marks, const std::vector<LayoutTape::MarkPlace>& places,
                          uint32_t first, uint32_t stop, std::vector<HeadingMark>& moved) {
        moved.clear();
        for (uint32_t mark = first; mark < stop; mark++) {
            HeadingMark heading = marks[mark];
            const LayoutTape::MarkPlace place = places[mark];
            const size_t at = place.block - from + index;
            const bool kept = store->Kept(at, place.block);
            if (kept && data + at == last->blockData + place.block) {
                moved.push_back(heading);
                continue;
            }
            const Block& block = data[at];
            const char* text = nullptr;
            if (place.offset != LayoutTape::MarkPlace::kNested) {
                if (place.offset + heading.text.size() <= block.text.size()) text = block.text.data() + place.offset;
            } else {
                // A nested block's text does not move with its top-level block.
                text = kept ? heading.text.data() : Relocate(heading.text, store->Old(place.block), block);
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
    const size_t pageShift = static_cast<size_t>(now.page) - before.page;   // modulo 2^64, as the pages' indices
    // The content page by page, each new page after the gap this build leaves before it (the last
    // build's gaps are not copied), so that it lies where BuildInto wants it.
    std::string& content = *target.content;
    const auto gapless = [&](size_t at, size_t page) { return at - old.pageStarts[page] + old.pageFree[page]; };
    CopyContent(gapless(before.content, before.page),
        after.page == before.page ? gapless(after.content, after.page) : old.pageFree[before.page + 1], content);
    for (size_t page = before.page + 1; page <= after.page; page++) {
        target.startPage(content, *target.pageStarts);
        CopyContent(old.pageFree[page], page == after.page ? gapless(after.content, page) : old.pageFree[page + 1], content);
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
    const size_t blockShift = index - oldFrom;
    for (HeadingMark& mark : movedHeadings) {
        mark.page += pageShift;
        target.headings->push_back(mark);
    }
    for (HeadingMark& mark : movedIds) {
        mark.page += pageShift;
        target.ids->push_back(mark);
    }
    // Where they are, in this build's blocks.
    const auto places = [blockShift](const std::vector<LayoutTape::MarkPlace>& from, uint32_t first, uint32_t stop,
                            std::vector<LayoutTape::MarkPlace>& into) {
        for (uint32_t mark = first; mark < stop; mark++) {
            into.push_back({ static_cast<uint32_t>(from[mark].block + blockShift), from[mark].offset });
        }
    };
    places(old.headingPlaces, before.headings, after.headings, tape.headingPlaces);
    places(old.idPlaces, before.ids, after.ids, tape.idPlaces);
    for (uint32_t line = before.contentsLines; line < after.contentsLines; line++) {
        ContentsLine moved = old.contentsLines[line];
        moved.page += pageShift;
        target.contentsLines->push_back(moved);
    }
    // The source map's entries, with the lines of this build's blocks: those of a block the parse
    // kept are the last build's moved by the lines the edit added before it, else read from the block.
    if (after.sourceBlocks != before.sourceBlocks) {
        const size_t at = tape.sourceMap.size();
        tape.sourceMap.insert(tape.sourceMap.end(), old.sourceMap.begin() + before.sourceBlocks,
            old.sourceMap.begin() + after.sourceBlocks);
        tape.sourceMapBlocks.insert(tape.sourceMapBlocks.end(), old.sourceMapBlocks.begin() + before.sourceBlocks,
            old.sourceMapBlocks.begin() + after.sourceBlocks);
        SourceBlock* moved = tape.sourceMap.data() + at;
        SourceBlock* const stop = tape.sourceMap.data() + tape.sourceMap.size();
        uint32_t* block = tape.sourceMapBlocks.data() + at;
        const SessionBlocks& kept = *store;
        const size_t suffix = kept.front + kept.newWindow;
        const uint32_t lineShift = static_cast<uint32_t>(kept.lineShift);
        for (; moved != stop; moved++, block++) {
            const size_t from = *block;
            const size_t to = from + blockShift;
            *block = static_cast<uint32_t>(to);
            if (kept.spliced && to < kept.front && from == to) {
                // The same lines.
            } else if (kept.spliced && to >= suffix && from + kept.newWindow == to + kept.oldWindow) {
                moved->firstLine += lineShift;
                moved->endLine += lineShift;
            } else {
                const Block& source = (*blocks)[to];
                moved->firstLine = source.sourceLine;
                moved->endLine = source.sourceLine + source.sourceLines;
            }
            moved->page = static_cast<uint32_t>(moved->page + pageShift);
            moved->endPage = static_cast<uint32_t>(moved->endPage + pageShift);
        }
    }
    tape.images.insert(tape.images.end(), old.images.begin() + static_cast<ptrdiff_t>(oldFrom),
        old.images.begin() + static_cast<ptrdiff_t>(oldTo));
    // The marks between: the last build's, moved by what this one had before the run, and the CIDs
    // each block recorded first added in their order (each mark then has the count after them).
    const size_t marks = tape.marks.size();
    tape.marks.insert(tape.marks.end(), old.marks.begin() + static_cast<ptrdiff_t>(oldFrom + 1),
        old.marks.begin() + static_cast<ptrdiff_t>(oldTo));
    LayoutMark* const firstMark = tape.marks.data() + marks;
    LayoutMark* const endMark = tape.marks.data() + tape.marks.size();
    for (size_t set = 0; set < kLayoutCidSets; set++) {
        const LayoutCids& cids = target.cids[set];
        if (cids.set == nullptr) continue;
        for (size_t at = oldFrom + 1; at <= oldTo; at++) {
            for (uint32_t cid = old.marks[at - 1].cids[set]; cid < old.marks[at].cids[set]; cid++) {
                cids.add(cids.set, old.cids[set][cid]);
            }
            if (at < oldTo) firstMark[at - oldFrom - 1].cids[set] = static_cast<uint32_t>(cids.recorded(cids.set).size());
        }
    }
    const MathGlyphUse glyphs = *target.glyphs;
    uint32_t glyphsOf = before.glyphs;   // the last build's glyphs the last mark moved here had
    constexpr uint8_t kMath = LayoutMark::kMathUsed | LayoutMark::kMathLaidOut;
    const uint8_t mathNow = now.flags & kMath;
    const size_t contentShift = now.content - before.content;
    const uint32_t linkShift = now.links - before.links;
    const uint32_t headingShift = now.headings - before.headings;
    const uint32_t idShift = now.ids - before.ids;
    const uint32_t contentsShift = now.contentsLines - before.contentsLines;
    const uint32_t sourceShift = now.sourceBlocks - before.sourceBlocks;
    const uint32_t numberShift = now.nextNumber - before.nextNumber;
    const uint32_t bottomShift = now.bottoms - before.bottoms;
    uint32_t missingShift[kLayoutCidSets];
    for (size_t set = 0; set < kLayoutCidSets; set++) missingShift[set] = now.missing[set] - before.missing[set];
    const uint32_t missingBmpShift = now.missingBmp - before.missingBmp;
    const size_t* const oldStarts = old.pageStarts.data();
    const size_t* const newStarts = target.pageStarts->data();
    const uint32_t firstPage = before.page;
    for (LayoutMark* moved = firstMark; moved != endMark; moved++) {
        if (moved->page == firstPage) {
            moved->content += contentShift;
            moved->links += linkShift;
        } else {
            moved->content = moved->content - oldStarts[moved->page] + newStarts[moved->page + pageShift];
        }
        moved->page = static_cast<uint32_t>(moved->page + pageShift);
        moved->headings += headingShift;
        moved->ids += idShift;
        moved->contentsLines += contentsShift;
        moved->sourceBlocks += sourceShift;
        moved->nextNumber += numberShift;
        moved->bottoms += bottomShift;
        for (size_t set = 0; set < kLayoutCidSets; set++) moved->missing[set] += missingShift[set];
        moved->missingBmp += missingBmpShift;
        moved->facesUsed = static_cast<uint8_t>(moved->facesUsed | now.facesUsed);
        moved->flags = static_cast<uint8_t>(moved->flags | mathNow);
        if (moved->glyphs == glyphsOf) {
            moved->glyphs = static_cast<uint32_t>(tape.glyphs.size() - 1);
            continue;
        }
        glyphsOf = moved->glyphs;
        MathGlyphUse drawn = glyphs;
        AddGlyphs(drawn, old.glyphs[glyphsOf]);
        if (std::memcmp(&tape.glyphs.back(), &drawn, sizeof drawn) != 0) tape.glyphs.push_back(drawn);
        moved->glyphs = static_cast<uint32_t>(tape.glyphs.size() - 1);
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
    state.nextNumber = after.nextNumber + numberShift;
    state.facesUsed = static_cast<uint8_t>(after.facesUsed | now.facesUsed);
    state.flags = static_cast<uint8_t>(after.flags | mathNow);
    return state;
}

void SessionLayout::CopyContent(size_t from, size_t to, std::string& content) {
    if (from == to) return;
    const std::vector<LayoutTape::Piece>& pieces = last->pieces;
    copied.push_back({ content.size(), content.size() + (to - from), from });
    size_t piece = static_cast<size_t>(std::upper_bound(pieces.begin(), pieces.end(), from,
        [](size_t at, const LayoutTape::Piece& it) { return at < it.at; }) - pieces.begin()) - 1;
    while (from < to) {
        const size_t stop = std::min(to, pieces[piece + 1].at);
        content.append(pieces[piece].data + (from - pieces[piece].at), stop - from);
        from = stop;
        piece++;
    }
}

void SessionLayout::Recycle(LayoutTape& tape) {
    constexpr size_t kSpares = 4;
    for (std::shared_ptr<std::string>& buffer : tape.buffers) {
        if (buffer.use_count() == 1 && spareBuffers.size() < kSpares) spareBuffers.push_back(std::move(buffer));
    }
    tape.buffers.clear();
}

std::shared_ptr<std::string> SessionLayout::Buffer(size_t bytes) {
    // The smallest spare that holds them, else the largest.
    size_t best = spareBuffers.size();
    for (size_t index = 0; index < spareBuffers.size(); index++) {
        const size_t capacity = spareBuffers[index]->capacity();
        if (best == spareBuffers.size()) {
            best = index;
            continue;
        }
        const size_t current = spareBuffers[best]->capacity();
        if (current >= bytes ? capacity >= bytes && capacity < current : capacity > current) best = index;
    }
    std::shared_ptr<std::string> buffer;
    if (best < spareBuffers.size()) {
        buffer = std::move(spareBuffers[best]);
        spareBuffers.erase(spareBuffers.begin() + static_cast<ptrdiff_t>(best));
    } else {
        buffer = std::make_shared<std::string>();
    }
    buffer->clear();
    buffer->reserve(bytes);
    return buffer;
}

void SessionLayout::EndLayout(const LayoutTarget& target, std::vector<SourceBlock>* sourceMap) {
    if (!active) return;
    LayoutTape& tape = *recording;
    const std::string& content = *target.content;
    const std::vector<size_t>& starts = *target.pageStarts;
    const size_t pages = starts.size();
    tape.pageStarts = starts;
    const auto pageEnd = [&](size_t page) { return page + 1 < pages ? starts[page + 1] - target.gapBefore(page + 1) : content.size(); };
    tape.pageFree.resize(pages + 1);
    size_t total = 0;
    for (size_t page = 0; page < pages; page++) {
        tape.pageFree[page] = total;
        total += pageEnd(page) - starts[page];
    }
    tape.pageFree[pages] = total;
    // The content without gaps: what this build laid out in a buffer of its own, what it replayed
    // where the last build's tape has it.
    size_t replayed = 0;
    for (const Copied& range : copied) replayed += range.end - range.start;
    std::vector<LayoutTape::Piece>& pieces = tape.pieces;
    pieces.clear();
    tape.buffers.clear();
    std::shared_ptr<std::string> fresh;
    const auto add = [&](size_t at, const char* data, uint32_t buffer) {
        if (!pieces.empty()) {
            const LayoutTape::Piece& before = pieces.back();
            if (before.buffer == buffer && before.data + (at - before.at) == data) return;
        }
        pieces.push_back({ at, data, buffer });
    };
    if (replayed != total) {
        fresh = Buffer(total - replayed);
        tape.buffers.push_back(fresh);
    }
    const size_t oldBuffers = last ? last->buffers.size() : 0;
    bufferMap.assign(oldBuffers, UINT32_MAX);
    size_t next = 0;
    for (size_t page = 0; page < pages; page++) {
        size_t at = starts[page];
        const size_t end = pageEnd(page);
        size_t gapless = tape.pageFree[page];
        while (at < end) {
            if (next < copied.size() && copied[next].start == at) {
                const Copied& range = copied[next++];
                const std::vector<LayoutTape::Piece>& from = last->pieces;
                size_t source = range.from;
                const size_t sourceEnd = source + (range.end - range.start);
                size_t piece = static_cast<size_t>(std::upper_bound(from.begin(), from.end(), source,
                    [](size_t value, const LayoutTape::Piece& it) { return value < it.at; }) - from.begin()) - 1;
                while (source < sourceEnd) {
                    const LayoutTape::Piece& it = from[piece];
                    uint32_t& buffer = bufferMap[it.buffer];
                    if (buffer == UINT32_MAX) {
                        buffer = static_cast<uint32_t>(tape.buffers.size());
                        tape.buffers.push_back(last->buffers[it.buffer]);
                    }
                    add(gapless, it.data + (source - it.at), buffer);
                    const size_t stop = std::min(sourceEnd, from[piece + 1].at);
                    gapless += stop - source;
                    source = stop;
                    piece++;
                }
                at = range.end;
                continue;
            }
            const size_t stop = next < copied.size() && copied[next].start < end ? copied[next].start : end;
            add(gapless, fresh->data() + fresh->size(), 0);
            fresh->append(content, at, stop - at);
            gapless += stop - at;
            at = stop;
        }
    }
    // Buffers of more than twice the content's bytes: all of it in one again.
    size_t held = 0;
    for (const std::shared_ptr<std::string>& buffer : tape.buffers) held += buffer->size();
    if (held > 2 * total) {
        pieces.clear();
        tape.buffers.clear();
        fresh = Buffer(total);
        for (size_t page = 0; page < pages; page++) fresh->append(content, starts[page], pageEnd(page) - starts[page]);
        pieces.push_back({ 0, fresh->data(), 0 });
        tape.buffers.push_back(std::move(fresh));
    }
    pieces.push_back({ total, nullptr, 0 });
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
    // Complete only when every block was laid out or replayed (Mark noted the last one's marks).
    if (tape.marks.size() != newBlocks.size() + 1 || tape.images.size() != newBlocks.size() ||
        tape.headingPlaces.size() != target.headings->size() || tape.idPlaces.size() != target.ids->size()) {
        last.reset();
        return;
    }
    tape.pageLinks = std::move(*target.pageLinks);
    tape.headings = std::move(*target.headings);
    tape.ids = std::move(*target.ids);
    tape.contentsLines = std::move(*target.contentsLines);
    tape.blockData = newBlocks.data();
    tape.blockCount = newBlocks.size();
    if (!numbers.empty()) tape.blockNumbers = NumbersOf(newBlocks, numbers);
    if (store != nullptr) CommitParse(*store);
    tape.contents = std::move(contents);
    tape.numbers = std::move(numbers);
    std::swap(last, recording);
    // The tape before goes now; its memory makes the next one.
    if (recording) {
        Recycle(*recording);
        recording->Clear();
    }
    blocks = nullptr;
}

void SessionLayout::Forget() {
    active = false;
    last.reset();
    recording.reset();
    spareBuffers.clear();
    reuse = {};
}

const KeptText* KeptTextOf(const std::string& text) {
    if (g_session == nullptr) return nullptr;
    const SessionText* const kept = static_cast<const SessionLayout*>(g_session)->sessionText;
    return kept != nullptr ? kept->Kept(text) : nullptr;
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
size_t ResumeLine(DocumentParse& parse, const ParseRecord& last, std::string_view text,
    const std::vector<std::string_view>& lines, const std::vector<unsigned char>& suppressed,
    const ReferenceDefinitions& definitions, bool definitionSyntax, size_t first, const LineChange& change) {
    constexpr size_t kNone = SIZE_MAX;
    const ParseRecord& next = parse.next;
    const size_t count = lines.size();
    const size_t oldCount = last.kinds.size();
    // The last text, changed in place where the change is not known, is gone: no line is known to be the same.
    if (last.winAnsi != next.winAnsi || last.definitionSyntax != definitionSyntax || last.first != first ||
        last.paddedCellsAtStart != parse.cellStart || count >= 0x7FFFFFFFu || oldCount >= 0x7FFFFFFFu ||
        last.referenceLeft < kMaxReferenceDestinationBytes || suppressed.empty() != last.suppressed.empty() ||
        parse.budget == nullptr || last.passes.empty() || last.passes[0].line != first || !change.textKnown ||
        !SameDefinitions(last.definitions, definitions)) {
        return kNone;
    }
    // The lines before the first byte that differs are the same, and so are those after the last.
    const size_t oldSize = last.text.size();
    const size_t size = text.size();
    const size_t common = std::min(oldSize, size);
    const size_t differs = change.differs;
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
        const size_t same = change.same;
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

bool ParseReuses(const DocumentParse& parse) {
    return parse.reuse;
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
    if (!store.pending && store.spare) store.pending = std::move(store.spare);
    if (!store.pending) {
        store.pending = std::make_unique<ParseRecord>();
        // A session's first records: as large as the last, so that they do not grow a step at a time.
        if (store.record) {
            const ParseRecord& like = *store.record;
            if (like.kept == nullptr) store.pending->own.reserve(like.own.size() + like.own.size() / 16);
            store.pending->kinds.reserve(like.kinds.size() + like.kinds.size() / 16);
            store.pending->suppressed.reserve(like.suppressed.size() + like.suppressed.size() / 16);
            store.pending->passes.reserve(like.passes.size() + like.passes.size() / 16);
        }
    }
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
    State() {
        layout.store = &blocks;
        layout.sessionText = &text;
    }
    Internal::SessionBlocks blocks;
    Internal::SessionText text;
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
    // The builders reserve the output for the text's length and no more (tiny_pdf.cpp's
    // PrepareOutput: at most 8 bytes a byte and 256 KiB, at most 64 MiB), so that a text a letter
    // longer than the last moves it to a new buffer of twice the size, all of it memory the process
    // has not touched (t700: 28 MB of page faults at the first keystroke). When it would, room for a
    // text a sixteenth longer; what is not written takes no memory.
    const auto expected = [](size_t bytes) { return std::min<size_t>(bytes * 8 + 256 * 1024, size_t{64} << 20); };
    if (pdfBytes.capacity() < expected(markdown.size())) pdfBytes.reserve(expected(markdown.size() + markdown.size() / 16));
    BuildResult result;
    {
        // The build reads the session's text, which becomes this one where it differs.
        const std::string& text = session.text.Begin(markdown);
        const SessionScope scope(&session.layout);
        result = BuildPdf(text, options, pdfBytes);
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
    if (result.error == BuildError::None) session.text.Commit();
    else session.text.Undo();
    session.layout.reuse.parsed = static_cast<uint32_t>(session.blocks.made);
    return result;
}

void BuildSession::Clear() {
    state->layout.Forget();
    state->blocks = {};
    state->text = Internal::SessionText();
    state->layout.store = &state->blocks;
    state->built = false;
}

BuildSession::Reuse BuildSession::LastReuse() const {
    return state->layout.reuse;
}

} // namespace TinyPdf
