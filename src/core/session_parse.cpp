// SPDX-License-Identifier: Apache-2.0

// The document level of a BuildSession's parse (session_parse.h): markdown_parser.cpp compiled a
// second time, in the engine library only (RAYOMD_BUILD_SESSION), for ParseSessionDocument, the
// level parse with the session's hooks (markdown_parser_level.inc with RAYOMD_SESSION_LEVEL 1). A
// second copy in markdown_parser.cpp itself, or the hooks in ParseMarkdownImpl, changed what GCC
// made of the parse every other build runs (+0.4 to 2 % instructions on BuildPdf); here that one is
// the same code as without sessions. The externally visible functions of the copy are renamed, so
// that they do not clash with markdown_parser.cpp's; only ParseSessionDocument is used.

#define SplitLines SessionCopySplitLines
#define StripInlineMarkdown SessionCopyStripInlineMarkdown
#define StripInlineMarkdownKeepMath SessionCopyStripInlineMarkdownKeepMath
#define ParseImageAt SessionCopyParseImageAt
#define ParseMarkdownLevel SessionCopyParseMarkdownLevel
#define ParseMarkdown SessionCopyParseMarkdown
#include "markdown_parser.cpp"

#include "build_session.h"

namespace TinyPdf::Internal {

static_assert(static_cast<uint8_t>(LineKind::Empty) == kEmptyLineKind, "session_parse.h's kEmptyLineKind");

// The lines of a session's parse, views of its record's text, with their classes, kept for the
// next parse (ParseRecord::lines).
struct ParseLines {
    std::vector<std::string_view> lines;
    // Each line's class after ResolveMathOpeners, as the block loop began; the loop may turn a
    // MathOpen line into Plain text, so the record's `kinds` are the classes to take from here.
    std::vector<LineInfo> infos;
    // The Fence lines that opened or closed a fence, pairing every Fence line in order as
    // ResolveMathOpeners and the definitions' pass do (without footnote definitions, which may
    // take a Fence line).
    std::vector<uint32_t> fences;
    bool notes = false;   // a footnote definition took lines
};

void ParseLinesFree::operator()(ParseLines* lines) const {
    delete lines;
}

namespace {

// The document parse of a session's build on this thread, which ParseMarkdownLevel hands to
// ParseSessionDocument; null for every other parse.
thread_local DocumentParse* tSessionDocument = nullptr;

// What the definitions' pass does at a line that may start a footnote definition.
enum class NoteLines { Read, Stop, Skip };

// The definitions' pass of ParseMarkdownImpl over lines [from, end), with no fence open at `from`:
// the definitions go to `add` in their order, the lines they take are marked in `suppressed` (when
// not null). At a line that may start a footnote definition it reads one into `notes` (Read),
// returns false (Stop), or goes on as at a line that starts none (Skip).
template <class Add>
bool DefinitionsPass(const std::vector<std::string_view>& lines, const std::vector<LineInfo>& infos,
    unsigned char* suppressed, size_t from, size_t end, NoteLines noteLines, FootnoteParse* notes, bool& tookNotes,
    const Add& add) {
    std::string_view activeFence;
    for (size_t index = from; index < end;) {
        if (infos[index].kind == LineKind::Fence) {
            if (activeFence.empty()) activeFence = infos[index].text;
            else if (IsMatchingClosingFence(infos[index], activeFence)) activeFence = {};
            index++;
            continue;
        }
        if (!activeFence.empty()) {
            index++;
            continue;
        }
        std::string_view ignoredIndented;
        if (RemoveIndentLevel(lines[index], ignoredIndented)) {
            index++;
            continue;
        }
        const std::string_view head = infos[index].trimmed;
        if (head.size() > 4 && head[0] == '[' && head[1] == '^' && noteLines != NoteLines::Skip) {
            if (noteLines == NoteLines::Stop) return false;
            const size_t taken = ReadFootnoteDefinition(lines, index, IsTextLine, &infos, *notes);
            if (taken != 0) {
                for (size_t offset = 0; offset < taken; offset++) suppressed[index + offset] = 2;
                tookNotes = true;
                index += taken;
                continue;
            }
        }
        std::string label;
        ReferenceDefinition definition;
        size_t consumed = 0;
        if (ParseReferenceDefinition(lines, index, label, definition, consumed)) {
            add(std::move(label), std::move(definition));
            if (suppressed != nullptr) {
                for (size_t offset = 0; offset < consumed; offset++) suppressed[index + offset] = 1;
            }
            index += consumed;
            continue;
        }
        index++;
    }
    return true;
}

// The lines of `next`'s text, their classes, the definitions and the lines they take, as
// ParseMarkdownImpl makes them, and the fences.
void ClassifyWhole(ParseRecord& next, ParseLines& out, ReferenceDefinitions& definitions, bool definitionSyntax,
    FootnoteParse& notes) {
    std::vector<std::string_view>& lines = out.lines;
    std::vector<LineInfo>& infos = out.infos;
    SplitLineViews(next.text, lines);
    infos.clear();
    infos.resize(lines.size());
    bool hasMathOpeners = false;
    for (size_t index = 0; index < lines.size(); index++) {
        ClassifyLine(lines[index], infos[index]);
        hasMathOpeners = hasMathOpeners || infos[index].kind == LineKind::MathOpen;
    }
    if (hasMathOpeners) ResolveMathOpeners(lines, infos);
    out.fences.clear();
    std::string_view activeFence;
    std::vector<uint8_t>& kinds = next.kinds;
    kinds.resize(infos.size());
    for (size_t index = 0; index < infos.size(); index++) {
        const LineInfo& info = infos[index];
        kinds[index] = static_cast<uint8_t>(info.kind);
        if (info.kind != LineKind::Fence) continue;
        if (activeFence.empty()) {
            activeFence = info.text;
            out.fences.push_back(static_cast<uint32_t>(index));
        } else if (IsMatchingClosingFence(info, activeFence)) {
            activeFence = {};
            out.fences.push_back(static_cast<uint32_t>(index));
        }
    }
    definitions.clear();
    out.notes = false;
    next.suppressed.clear();
    if (!definitionSyntax) return;
    next.suppressed.assign(lines.size(), 0);
    DefinitionsPass(lines, infos, next.suppressed.data(), 0, lines.size(), NoteLines::Read, &notes, out.notes,
        [&](std::string&& label, ReferenceDefinition&& definition) { definitions.emplace(std::move(label), std::move(definition)); });
}

// `view` as a view of the same bytes in another text, which holds them `by` bytes further on.
inline std::string_view Moved(std::string_view view, uintptr_t by) {
    return std::string_view(reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(view.data()) + by), view.size());
}

// Lines [from, end) of the last parse as lines [from + shift, ...) of this one, whose text has
// their bytes `by` bytes further on: views moved, the classes the last block loop began with.
void ShiftLines(const ParseLines& old, const std::vector<uint8_t>& oldKinds, size_t from, size_t end, ptrdiff_t shift,
    uintptr_t by, ParseLines& out) {
    const std::string_view* source = old.lines.data();
    const LineInfo* sourceInfo = old.infos.data();
    std::string_view* target = out.lines.data();
    LineInfo* targetInfo = out.infos.data();
    for (size_t index = from; index < end; index++) {
        const size_t to = static_cast<size_t>(static_cast<ptrdiff_t>(index) + shift);
        target[to] = Moved(source[index], by);
        const LineInfo& info = sourceInfo[index];
        LineInfo& into = targetInfo[to];
        into.trimmed = Moved(info.trimmed, by);
        into.text = info.text.data() == nullptr ? std::string_view() : Moved(info.text, by);
        into.kind = static_cast<LineKind>(oldKinds[index]);
        into.level = info.level;
        into.number = info.number;
    }
}

bool SameFound(const std::vector<std::pair<std::string, ReferenceDefinition>>& a,
    const std::vector<std::pair<std::string, ReferenceDefinition>>& b) {
    if (a.size() != b.size()) return false;
    for (size_t index = 0; index < a.size(); index++) {
        if (a[index].first != b[index].first || a[index].second.destination != b[index].second.destination ||
            a[index].second.title != b[index].second.title) {
            return false;
        }
    }
    return true;
}

// The lines of `next`'s text from the last parse's: those before and after the edit shifted, the
// changed ones split and classified, and ResolveMathOpeners and the definitions' pass run again
// from a blank line before them to one after them at which both texts have no fence open (from
// there on both passes go as in the last parse). False when it cannot (the caller classifies
// whole; what it wrote is then of no use): a footnote definition in the last text or near the
// edit, a byte order mark that may come or go, other definitions near the edit.
bool ClassifyShifted(const ParseRecord& last, ParseRecord& next, ParseLines& out, ReferenceDefinitions& definitions,
    bool definitionSyntax, LineChange& change) {
    const ParseLines& old = *last.lines;
    if (old.notes || last.definitionSyntax != definitionSyntax || last.winAnsi != WinAnsiReferences::Active()) return false;
    const std::string& oldText = last.text;
    const std::string& text = next.text;
    const size_t oldCount = old.lines.size();
    const size_t oldSize = oldText.size();
    const size_t size = text.size();
    if (oldCount >= 0x7FFFFFFFu || size >= 0x7FFFFFFFu || oldSize >= 0x7FFFFFFFu) return false;
    const size_t common = std::min(oldSize, size);
    const size_t differs = FirstDifference(oldText.data(), text.data(), common);
    const char* const oldBase = oldText.data();
    const char* const base = text.data();
    const uintptr_t by = reinterpret_cast<uintptr_t>(base) - reinterpret_cast<uintptr_t>(oldBase);
    std::vector<std::string_view>& lines = out.lines;
    std::vector<LineInfo>& infos = out.infos;
    std::vector<uint8_t>& kinds = next.kinds;
    std::vector<unsigned char>& suppressed = next.suppressed;
    if (differs == common && oldSize == size) {
        lines.resize(oldCount);
        infos.resize(oldCount);
        ShiftLines(old, last.kinds, 0, oldCount, 0, by, out);
        kinds = last.kinds;
        suppressed = last.suppressed;
        out.fences = old.fences;
        out.notes = false;
        definitions = last.definitions;
        change = { true, differs, 0, oldCount, oldCount };
        return true;
    }
    if (differs < 3) return false;
    const size_t same = CommonEnd(oldBase + oldSize, base + size, common - differs);
    const auto start = [&](size_t line) { return static_cast<size_t>(old.lines[line].data() - oldBase); };
    // The lines before the one that holds the first byte that differs (a line holds its line feed)
    // are the same, at the same place.
    size_t low = 1;
    size_t high = oldCount;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (start(middle) <= differs) low = middle + 1;
        else high = middle;
    }
    const size_t front = low - 1;
    // The lines whose line feed before them is in the end both texts share are the same too.
    low = front + 1;
    high = oldCount;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (start(middle) >= oldSize - same + 1) high = middle;
        else low = middle + 1;
    }
    const size_t end = low;
    const ptrdiff_t delta = static_cast<ptrdiff_t>(size) - static_cast<ptrdiff_t>(oldSize);
    // The new lines between them: up to the line feed before the shared end, or to the text's end.
    const size_t middleStart = start(front);
    const size_t middleBytesEnd = end < oldCount ? static_cast<size_t>(static_cast<ptrdiff_t>(start(end)) + delta) : size;
    size_t middle = end < oldCount ? 0 : 1;
    for (const char* at = base + middleStart; at < base + middleBytesEnd; at++) {
        at = static_cast<const char*>(std::memchr(at, '\n', static_cast<size_t>(base + middleBytesEnd - at)));
        if (at == nullptr) break;
        middle++;
    }
    const size_t middleEnd = front + middle;
    const size_t count = middleEnd + oldCount - end;
    const ptrdiff_t shift = static_cast<ptrdiff_t>(middleEnd) - static_cast<ptrdiff_t>(end);
    lines.resize(count);
    infos.resize(count);
    kinds.resize(count);
    ShiftLines(old, last.kinds, 0, front, 0, by, out);
    ShiftLines(old, last.kinds, end, oldCount, shift, by + static_cast<uintptr_t>(delta), out);
    std::memcpy(kinds.data(), last.kinds.data(), front);
    std::memcpy(kinds.data() + middleEnd, last.kinds.data() + end, oldCount - end);
    if (definitionSyntax) {
        suppressed.resize(count);
        std::memcpy(suppressed.data(), last.suppressed.data(), front);
        std::memcpy(suppressed.data() + middleEnd, last.suppressed.data() + end, oldCount - end);
    } else {
        suppressed.clear();
    }
    {
        size_t at = middleStart;
        for (size_t line = front; line < middleEnd; line++) {
            const void* const feed = std::memchr(base + at, '\n', size - at);
            const size_t lineEnd = feed != nullptr ? static_cast<size_t>(static_cast<const char*>(feed) - base) : size;
            size_t stop = lineEnd;
            if (stop > at && base[stop - 1] == '\r') stop--;
            lines[line] = std::string_view(base + at, stop - at);
            infos[line] = LineInfo();
            ClassifyLine(lines[line], infos[line]);
            at = lineEnd + 1;
        }
    }
    // The passes start again at the last blank line before the edit at which no fence is open
    // (nothing before it reads past it), else at the first line.
    const std::vector<uint32_t>& oldFences = old.fences;
    size_t below = static_cast<size_t>(std::lower_bound(oldFences.begin(), oldFences.end(), front) - oldFences.begin());
    size_t restart = 0;
    size_t fencesBefore = 0;
    for (size_t line = front; line-- > 0;) {
        while (below > 0 && oldFences[below - 1] >= line) below--;
        if (last.kinds[line] == kEmptyLineKind && below % 2 == 0) {
            restart = line;
            fencesBefore = below;
            break;
        }
    }
    // ResolveMathOpeners from there on, on lines classified again (the last parse's classes are
    // after it), up to a blank line after the edit at which neither text has a fence open.
    out.fences.assign(oldFences.begin(), oldFences.begin() + static_cast<ptrdiff_t>(fencesBefore));
    size_t oldCursor = static_cast<size_t>(std::lower_bound(oldFences.begin(), oldFences.end(), end) - oldFences.begin());
    size_t fresh = restart;
    const auto classify = [&](size_t through) {
        for (; fresh <= through; fresh++) {
            if (fresh >= front && fresh < middleEnd) continue;
            infos[fresh] = LineInfo();
            ClassifyLine(lines[fresh], infos[fresh]);
        }
    };
    std::string_view activeFence;
    size_t settle = count;
    for (size_t line = restart; line < count; line++) {
        if (line >= middleEnd) {
            const size_t oldLine = static_cast<size_t>(static_cast<ptrdiff_t>(line) - shift);
            while (oldCursor < oldFences.size() && oldFences[oldCursor] < oldLine) oldCursor++;
            if (activeFence.empty() && oldCursor % 2 == 0 && last.kinds[oldLine] == kEmptyLineKind) {
                settle = line;
                break;
            }
        }
        classify(line);
        LineInfo& info = infos[line];
        if (info.kind == LineKind::Fence) {
            if (activeFence.empty()) {
                activeFence = info.text;
                out.fences.push_back(static_cast<uint32_t>(line));
            } else if (IsMatchingClosingFence(info, activeFence)) {
                activeFence = {};
                out.fences.push_back(static_cast<uint32_t>(line));
            }
            continue;
        }
        if (!activeFence.empty() || info.kind != LineKind::MathOpen) continue;
        std::string_view ignored;
        if (RemoveIndentLevel(lines[line], ignored)) continue;
        const size_t close = FindMathBlockClose(infos, line, ignored);
        if (close == std::string_view::npos) {
            info.kind = LineKind::Plain;
            info.level = 0;
            info.text = {};
        } else {
            classify(close);
            line = close;
        }
    }
    if (settle < count) {
        for (size_t index = oldCursor; index < oldFences.size(); index++) {
            out.fences.push_back(static_cast<uint32_t>(static_cast<ptrdiff_t>(oldFences[index]) + shift));
        }
    }
    for (size_t line = restart; line < settle; line++) kinds[line] = static_cast<uint8_t>(infos[line].kind);
    // The definitions' pass over the same lines: the definitions are the last parse's when it finds
    // those it found there.
    if (definitionSyntax) {
        std::fill(suppressed.begin() + static_cast<ptrdiff_t>(restart), suppressed.begin() + static_cast<ptrdiff_t>(settle), 0);
        std::vector<std::pair<std::string, ReferenceDefinition>> found;
        bool tookNotes = false;
        const auto add = [](std::vector<std::pair<std::string, ReferenceDefinition>>& into) {
            return [&into](std::string&& label, ReferenceDefinition&& definition) { into.emplace_back(std::move(label), std::move(definition)); };
        };
        if (!DefinitionsPass(lines, infos, suppressed.data(), restart, settle, NoteLines::Stop, nullptr, tookNotes, add(found))) {
            return false;
        }
        const size_t oldSettle = static_cast<size_t>(static_cast<ptrdiff_t>(settle) - shift);
        bool hadDefinitions = false;
        for (size_t line = restart; line < oldSettle && !hadDefinitions; line++) hadDefinitions = last.suppressed[line] != 0;
        if (hadDefinitions || !found.empty()) {
            std::vector<std::pair<std::string, ReferenceDefinition>> before;
            DefinitionsPass(old.lines, old.infos, nullptr, restart, oldSettle, NoteLines::Skip, nullptr, tookNotes, add(before));
            if (!SameFound(found, before)) return false;
        }
    }
    out.notes = false;
    definitions = last.definitions;
    change = { true, differs, same, restart, settle };
    return true;
}

// The document's lines, their classes and definitions, in the record this parse leaves.
ParseLines& ClassifyInSession(const std::string& markdown, bool definitionSyntax, ReferenceDefinitions& definitions,
    FootnoteParse& notes, LineChange& change) {
    DocumentParse& parse = *tSessionDocument;
    ParseRecord& next = NextRecord(parse);
    const ParseRecord* const last = LastRecord(parse);
    if (!next.lines) {
        next.lines.reset(new ParseLines());
        // A session's first records: as large as the last, so that they do not grow a step at a time.
        if (last != nullptr && last->lines) {
            const size_t count = last->lines->lines.size();
            next.lines->lines.reserve(count + count / 16);
            next.lines->infos.reserve(count + count / 16);
            next.lines->fences.reserve(last->lines->fences.size() + 16);
        }
    }
    ParseLines& out = *next.lines;
    // Room for a text a sixteenth longer, so that each letter typed does not move it.
    if (next.text.capacity() < markdown.size()) next.text.reserve(markdown.size() + markdown.size() / 16);
    next.text.assign(markdown);
    change = {};
    if (last == nullptr || !last->lines || !ClassifyShifted(*last, next, out, definitions, definitionSyntax, change)) {
        change = {};
        ClassifyWhole(next, out, definitions, definitionSyntax, notes);
    }
    return out;
}

// BeginDocumentParse with the record's lines and classes.
size_t BeginInSession(const std::vector<std::string_view>& lines, const std::vector<unsigned char>& suppressed,
    const ReferenceDefinitions& definitions, bool definitionSyntax, size_t first, const LineChange& change,
    const FootnoteParse* notes) {
    return BeginDocumentParse(*tSessionDocument, lines, suppressed, definitions, definitionSyntax, first, change, *notes);
}

// EndDocumentParse, after which the thread has no document parse.
void EndInSession(std::vector<Block>& blocks, size_t line, ReferenceDefinitions& definitions, const FootnoteParse* notes) {
    DocumentParse& session = *tSessionDocument;
    tSessionDocument = nullptr;
    EndDocumentParse(session, blocks, line, definitions, *notes);
}

} // namespace

void SetDocumentParse(DocumentParse* parse) {
    tSessionDocument = parse;
}

bool SessionDocumentSet() {
    return tSessionDocument != nullptr;
}

std::vector<Block> ParseSessionDocument(const std::string& markdown, int depth, FootnoteParse* notes) {
#define RAYOMD_SESSION_LEVEL 1
#include "markdown_parser_level.inc"
#undef RAYOMD_SESSION_LEVEL
}

} // namespace TinyPdf::Internal
