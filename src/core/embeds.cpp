// SPDX-License-Identifier: Apache-2.0

#include "embeds.h"

#include "dialect.h"
#include "footnotes.h"
#include "inline_markdown.h"
#include "vault.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace TinyPdf::Internal {

namespace {

using namespace std::string_view_literals;   // per-line tests: no strlen a literal at -Os

constexpr size_t kNone = std::string_view::npos;

thread_local const EmbedSet* tEmbeds = nullptr;

bool IsBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsBlank(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsBlank(text.back())) text.remove_suffix(1);
    return text;
}

char LowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

bool EqualsIgnoringCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t index = 0; index < a.size(); index++) {
        if (LowerAscii(a[index]) != LowerAscii(b[index])) return false;
    }
    return true;
}

// The lines of `text` as the parser counts them: each up to a '\n', the last one too when it
// is empty. A byte order mark is no part of the first.
std::vector<std::string_view> Lines(std::string_view text) {
    std::vector<std::string_view> lines;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    for (size_t start = 0;;) {
        const size_t end = text.find('\n', start);
        if (end == kNone) {
            lines.push_back(text.substr(start));
            return lines;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

// The fence a line opens or closes, after blanks and quote markers, as Obsidian's rewrite reads
// it (dialect_rewrite.cpp's FenceOf): three or more backticks, with no backtick after them, or
// tildes.
struct Fence {
    char marker = 0;
    size_t length = 0;
    bool bare = false;
};

Fence FenceOf(std::string_view line) {
    size_t at = 0;
    while (at < line.size() && (IsBlank(line[at]) || line[at] == '>')) at++;
    if (at == line.size() || (line[at] != '`' && line[at] != '~')) return {};
    const char marker = line[at];
    size_t end = at;
    while (end < line.size() && line[end] == marker) end++;
    if (end - at < 3) return {};
    const std::string_view rest = line.substr(end);
    if (marker == '`' && rest.find('`') != kNone) return {};
    return { marker, end - at, Trim(rest).empty() };
}

// Reads lines in turn, as the rewrite and the parser see them: which are fenced code, display
// math, an Obsidian or HTML comment, or the front matter, none of which holds an embed.
class LineState {
public:
    // Whether `line` (number `index`) is ordinary Markdown outside all of those.
    bool Outside(const std::vector<std::string_view>& lines, size_t index) {
        const std::string_view line = lines[index];
        if (index == 0 && Trim(line) == "---") {
            for (size_t next = 1; next < lines.size(); next++) {
                const std::string_view trimmed = Trim(lines[next]);
                if (trimmed == "---" || trimmed == "...") {
                    frontMatterEnd = next;
                    return false;
                }
            }
        }
        if (index <= frontMatterEnd && frontMatterEnd != kNone) return false;
        if (htmlComment) {
            htmlComment = line.find("-->"sv) == kNone;
            return false;
        }
        if (comment) {
            comment = (Pairs(line) & 1) == 0;
            return false;
        }
        if (fence.marker != 0) {
            const Fence closing = FenceOf(line);
            if (closing.marker == fence.marker && closing.length >= fence.length && closing.bare) fence = {};
            return false;
        }
        if (displayMath) {
            if (line.find("$$"sv) != kNone || Trim(line).empty()) displayMath = false;
            return false;
        }
        if (bracketMath) {
            const std::string_view text = Trim(line);
            if (text.empty() || (text.size() >= 2 && text.substr(text.size() - 2) == "\\]"sv)) bracketMath = false;
            return false;
        }
        if (const Fence opening = FenceOf(line); opening.marker != 0) {
            fence = opening;
            return false;
        }
        size_t at = 0;
        while (at < line.size() && (IsBlank(line[at]) || line[at] == '>')) at++;
        if (line.substr(at, 2) == "$$"sv && line.find("$$"sv, at + 2) == kNone) {
            displayMath = true;
            return false;
        }
        const std::string_view trimmed = Trim(line);
        if (trimmed == "\\["sv) {
            bracketMath = true;
            return false;
        }
        if (trimmed.substr(0, 4) == "<!--"sv && trimmed.find("-->"sv, 4) == kNone) {
            htmlComment = true;
            return false;
        }
        if ((Pairs(line) & 1) != 0) {
            comment = true;
            return false;
        }
        return true;
    }

private:
    // How many "%%" the line has.
    static size_t Pairs(std::string_view line) {
        size_t count = 0;
        for (size_t at = line.find("%%"sv); at != kNone; at = line.find("%%"sv, at + 2)) count++;
        return count;
    }

    size_t frontMatterEnd = kNone;
    Fence fence;
    bool displayMath = false;
    bool bracketMath = false;
    bool comment = false;
    bool htmlComment = false;
};

// "![[inner]]" alone on a line from its first column: the inner text, else empty. As the rewrite
// reads an embed (Converter::Convert): it ends at the first "]]", with no '[' before it. An
// indented one is in a list item or a quote's lazy text, where an embed stays a link.
std::string_view StandaloneEmbed(std::string_view line) {
    if (line.substr(0, 3) != "![["sv) return {};
    const size_t close = line.find("]]"sv, 3);
    if (close == kNone || close == 3 || !Trim(line.substr(close + 2)).empty()) return {};
    const std::string_view inner = line.substr(3, close - 3);
    if (inner.find('[') != kNone || inner.find("%%"sv) != kNone) return {};
    return inner;
}

// What an embed names: a note, and in it a heading's section or a block.
struct EmbedTarget {
    std::string_view note;
    std::string_view anchor;   // a heading, or a block's id without its '^'
    bool block = false;
};

// Files an embed shows as what they are, not as notes: images, PDFs, audio and video.
bool IsMediaName(std::string_view note) {
    const size_t dot = note.rfind('.');
    if (dot == kNone) return false;
    const size_t slash = note.find_last_of("/\\"sv);
    if (slash != kNone && dot < slash) return false;
    const std::string_view extension = note.substr(dot + 1);
    for (const std::string_view media : { "png"sv, "jpg"sv, "jpeg"sv, "svg"sv, "gif"sv, "webp"sv, "bmp"sv, "tif"sv, "tiff"sv,
             "avif"sv, "pdf"sv, "mp3"sv, "wav"sv, "m4a"sv, "ogg"sv, "3gp"sv, "flac"sv, "mp4"sv, "webm"sv, "ogv"sv, "mov"sv, "mkv"sv }) {
        if (EqualsIgnoringCase(extension, media)) return true;
    }
    return false;
}

bool ParseTarget(std::string_view inner, EmbedTarget& target) {
    std::string_view name = inner.substr(0, inner.find('|'));
    if (!name.empty() && name.back() == '\\') name.remove_suffix(1);
    name = Trim(name);
    const size_t hash = name.find('#');
    target.note = Trim(name.substr(0, hash));
    if (target.note.empty() || IsMediaName(target.note)) return false;
    if (hash != kNone) {
        const std::string_view headings = name.substr(hash + 1);
        const size_t last = headings.rfind('#');
        target.anchor = Trim(last == kNone ? headings : headings.substr(last + 1));
        target.block = !target.anchor.empty() && target.anchor[0] == '^';
        if (target.block) target.anchor.remove_prefix(1);
    }
    return true;
}

// The level of an ATX heading line and its text, 0 for none.
int HeadingOf(std::string_view line, std::string_view& text) {
    size_t at = 0;
    while (at < line.size() && line[at] == ' ') at++;
    if (at > 3) return 0;
    size_t level = 0;
    while (at + level < line.size() && line[at + level] == '#') level++;
    if (level == 0 || level > 6) return 0;
    if (at + level < line.size() && line[at + level] != ' ' && line[at + level] != '\t') return 0;
    text = Trim(line.substr(at + level));
    while (!text.empty() && text.back() == '#') text.remove_suffix(1);
    text = Trim(text);
    return (int)level;
}

// The lines [first, end) of `lines` joined again.
std::string Join(const std::vector<std::string_view>& lines, size_t first, size_t end) {
    std::string out;
    for (size_t index = first; index < end; index++) {
        out.append(lines[index]);
        out += '\n';
    }
    return out;
}

// The section a heading begins: up to the next heading of its level or a higher one, outside
// fenced code. Empty when the note has no such heading.
std::string Section(const std::vector<std::string_view>& lines, size_t first, std::string_view heading) {
    Fence fence;
    size_t start = kNone;
    int level = 0;
    for (size_t index = first; index < lines.size(); index++) {
        if (fence.marker != 0) {
            const Fence closing = FenceOf(lines[index]);
            if (closing.marker == fence.marker && closing.length >= fence.length && closing.bare) fence = {};
            continue;
        }
        if (const Fence opening = FenceOf(lines[index]); opening.marker != 0) {
            fence = opening;
            continue;
        }
        std::string_view text;
        const int found = HeadingOf(lines[index], text);
        if (found == 0) continue;
        if (start == kNone) {
            if (EqualsIgnoringCase(text, heading)) {
                start = index;
                level = found;
            }
        } else if (found <= level) {
            return Join(lines, start, index);
        }
    }
    return start == kNone ? std::string() : Join(lines, start, lines.size());
}

// Where " ^id" ends `line` (or `line` is "^id" alone): the position of its '^', else kNone.
size_t BlockIdAt(std::string_view line, std::string_view id) {
    std::string_view text = line;
    while (!text.empty() && IsBlank(text.back())) text.remove_suffix(1);
    if (text.size() < id.size() + 1 || text.substr(text.size() - id.size()) != id || text[text.size() - id.size() - 1] != '^') {
        return kNone;
    }
    const size_t caret = text.size() - id.size() - 1;
    if (caret > 0 && !IsBlank(text[caret - 1])) return kNone;
    return caret;
}

bool IsBlankLine(std::string_view line) {
    return Trim(line).empty();
}

bool IsListLine(std::string_view line) {
    const std::string_view text = Trim(line);
    if (text.size() >= 2 && (text[0] == '-' || text[0] == '*' || text[0] == '+') && (text[1] == ' ' || text[1] == '\t')) return true;
    size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') digits++;
    return digits > 0 && digits + 1 < text.size() && (text[digits] == '.' || text[digits] == ')') &&
        (text[digits + 1] == ' ' || text[digits + 1] == '\t');
}

// The block "^id" marks (Obsidian's block reference): the paragraph or list line that ends in
// " ^id", or the block a line of "^id" alone follows; the id left out. Empty without one.
std::string BlockText(const std::vector<std::string_view>& lines, size_t first, std::string_view id) {
    Fence fence;
    for (size_t index = first; index < lines.size(); index++) {
        if (fence.marker != 0) {
            const Fence closing = FenceOf(lines[index]);
            if (closing.marker == fence.marker && closing.length >= fence.length && closing.bare) fence = {};
            continue;
        }
        if (const Fence opening = FenceOf(lines[index]); opening.marker != 0) {
            fence = opening;
            continue;
        }
        const size_t caret = BlockIdAt(lines[index], id);
        if (caret == kNone) continue;
        if (Trim(lines[index].substr(0, caret)).empty()) {
            // The block before the id's line, blank lines between them skipped.
            size_t end = index;
            while (end > first && IsBlankLine(lines[end - 1])) end--;
            size_t start = end;
            while (start > first && !IsBlankLine(lines[start - 1])) start--;
            return Join(lines, start, end);
        }
        std::string text(Trim(lines[index].substr(0, caret)));
        if (IsListLine(lines[index])) return text + '\n';
        size_t start = index;
        while (start > first && !IsBlankLine(lines[start - 1]) && !IsListLine(lines[start - 1])) {
            std::string_view heading;
            if (HeadingOf(lines[start - 1], heading) != 0) break;
            start--;
        }
        return Join(lines, start, index) + text + '\n';
    }
    return std::string();
}

// Where a note's body begins: after its byte order mark and its front matter (the first line
// "---" up to a line "---" or "..."); past the end (text.size() + 1) when no line follows that.
size_t BodyOffset(std::string_view text) {
    const size_t start = text.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0;
    size_t end = text.find('\n', start);
    if (end == kNone || Trim(text.substr(start, end - start)) != "---") return start;
    for (size_t at = end + 1;; at = end + 1) {
        end = text.find('\n', at);
        const std::string_view line = Trim(text.substr(at, end == kNone ? kNone : end - at));
        if (line == "---" || line == "...") return end == kNone ? text.size() + 1 : end + 1;
        if (end == kNone) return start;
    }
}

// The end of the footnote label at value[2...], as a definition "[^label]:" has it: up to ']',
// without white space, at most 999 bytes; kNone for none.
size_t LabelEnd(std::string_view value, size_t from) {
    size_t end = from;
    for (; end < value.size() && value[end] != ']'; end++) {
        const char ch = value[end];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\0' || ch == '[' || end - from >= 999) return kNone;
    }
    return end > from && end < value.size() ? end : kNone;
}

// `text` with the footnote labels it defines made its own, in place: "[^label]" as "[^label~eN]"
// in its definitions and references, outside fenced code and code spans. Another note's "[^1]" is
// then another note.
void OwnFootnotes(std::string& text, uint32_t entry) {
    if (text.find("[^") == std::string::npos || text.find("]:") == std::string::npos) return;
    const std::vector<std::string_view> lines = Lines(text);
    std::unordered_set<std::string> defined;
    Fence fence;
    std::vector<unsigned char> code(lines.size(), 0);
    for (size_t index = 0; index < lines.size(); index++) {
        const std::string_view line = lines[index];
        if (fence.marker != 0) {
            const Fence closing = FenceOf(line);
            if (closing.marker == fence.marker && closing.length >= fence.length && closing.bare) fence = {};
            code[index] = 1;
            continue;
        }
        if (const Fence opening = FenceOf(line); opening.marker != 0) {
            fence = opening;
            code[index] = 1;
            continue;
        }
        size_t at = 0;
        while (at < line.size() && (IsBlank(line[at]) || line[at] == '>')) at++;
        if (line.substr(at, 2) != "[^") continue;
        const size_t end = LabelEnd(line, at + 2);
        if (end != kNone && end + 1 < line.size() && line[end + 1] == ':') {
            defined.insert(NormalizeReferenceLabel(line.substr(at + 2, end - at - 2)));
        }
    }
    if (defined.empty()) return;
    const std::string suffix = "~e" + std::to_string(entry);
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    struct Run {
        size_t at;
        size_t length;
        size_t close;   // the run that closes its code span, kNone for none
    };
    std::vector<Run> runs;
    std::vector<size_t> nextOfLength;
    for (size_t index = 0; index < lines.size(); index++) {
        const std::string_view line = lines[index];
        if (index > 0) out += '\n';
        if (code[index] != 0 || line.find("[^") == kNone) {
            out.append(line);
            continue;
        }
        // The line's backtick runs, each with the next run as long, which closes its code span:
        // found in one pass back over them, so a line of unmatched runs stays linear.
        runs.clear();
        size_t longest = 0;
        for (size_t at = 0; at < line.size();) {
            if (line[at] != '`') {
                at++;
                continue;
            }
            size_t end = at;
            while (end < line.size() && line[end] == '`') end++;
            runs.push_back({ at, end - at, kNone });
            longest = std::max(longest, end - at);
            at = end;
        }
        nextOfLength.assign(runs.empty() ? 0 : longest + 1, kNone);
        for (size_t run = runs.size(); run-- > 0;) {
            runs[run].close = nextOfLength[runs[run].length];
            nextOfLength[runs[run].length] = run;
        }
        size_t run = 0;
        for (size_t at = 0; at < line.size();) {
            if (run < runs.size() && runs[run].at == at) {
                const Run& open = runs[run];
                const size_t end = open.close == kNone ? at + open.length : runs[open.close].at + open.length;
                run = open.close == kNone ? run + 1 : open.close + 1;
                out.append(line.substr(at, end - at));
                at = end;
                continue;
            }
            if (line[at] == '[' && at + 1 < line.size() && line[at + 1] == '^') {
                const size_t end = LabelEnd(line, at + 2);
                if (end != kNone && (run >= runs.size() || runs[run].at > end) &&
                    defined.count(NormalizeReferenceLabel(line.substr(at + 2, end - at - 2))) != 0) {
                    out.append(line.substr(at, end - at));
                    out += suffix;
                    at = end;
                    continue;
                }
            }
            out += line[at++];
        }
    }
    text = std::move(out);
}

// The standalone embeds of a text: their lines and what they name, in line order.
std::vector<std::pair<uint32_t, std::string_view>> StandaloneEmbeds(std::string_view text) {
    std::vector<std::pair<uint32_t, std::string_view>> found;
    if (text.find("![[") == kNone) return found;
    const std::vector<std::string_view> lines = Lines(text);
    LineState state;
    for (size_t index = 0; index < lines.size() && index < UINT32_MAX; index++) {
        if (!state.Outside(lines, index)) continue;
        const std::string_view inner = StandaloneEmbed(lines[index]);
        if (!inner.empty()) found.emplace_back((uint32_t)index, inner);
    }
    return found;
}

// Reads the notes of a document's embeds, depth first, within EmbedSet's limits.
class Resolver {
public:
    Resolver(const PdfOptions& buildOptions, EmbedSet& embedSet)
        : options(buildOptions), set(embedSet), vault(buildOptions.vaultRoot, buildOptions.sourcePath) {}

    bool Run(const std::string& markdown) {
        if (!vault.Valid()) return false;
        // The document's folder, which is the vault's root unless --vault names one.
        std::string folder = vault.Root();
        if (!options.vaultRoot.empty()) {
            const std::string_view parent = ParentOf(options.sourcePath);
            folder = ResolvedPath(parent.empty() ? std::string_view(".") : parent);
            if (folder.empty()) return false;
        }
        Number('f', folder, {}, 0);
        set.folders.push_back(std::move(folder));
        // Note 0 is the document, by its resolved path: never read, always on the chain.
        NoteOf(ResolvedPath(options.sourcePath));
        chain.push_back(0);
        // No reallocation while Expand holds an entry's text.
        set.entries.reserve(EmbedSet::kMaxEmbeds + 1);
        set.entries.emplace_back();
        Expand(0, markdown, 0, 0);
        if (set.entries.size() <= 1) return false;
        size_t total = markdown.size();
        for (const EmbedEntry& entry : set.entries) total += entry.text.size() + 1;
        set.all.reserve(total);
        set.all = markdown;
        for (const EmbedEntry& entry : set.entries) {
            set.all += '\n';
            set.all += entry.text;
        }
        return true;
    }

private:
    // At most this many lookups of a note a build: a document of many embeds of missing notes
    // costs a few file system calls each, and no more of them. Notes read come to at most
    // kMaxReadBytes, and the sections and blocks looked for in them read at most kMaxScanBytes
    // (one note's section embedded a thousand times, each looked for again).
    static constexpr size_t kMaxLookups = 4 * EmbedSet::kMaxEmbeds;
    static constexpr size_t kMaxReadBytes = 4 * EmbedSet::kMaxBytes;
    static constexpr size_t kMaxScanBytes = 8 * EmbedSet::kMaxBytes;
    static constexpr int kNoNote = -1;

    // A note found, by its path (each path once, whatever names it), read on its first embed.
    struct Note {
        std::string path;
        std::string text;
        RayoMd::File::FileIdentity identity;
        size_t body = 0;      // BodyOffset
        int folder = 0;       // EmbedSet::folders
        bool tried = false;
    };

    bool Cancelled() const {
        return options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed);
    }

    // The embeds of entry `index`, whose text is `text`, in the note in EmbedSet::folders[folder],
    // `depth` embeds down from the document.
    void Expand(uint32_t index, std::string_view text, int folder, int depth) {
        for (const auto& [line, inner] : StandaloneEmbeds(text)) {
            if (stopped || set.entries.size() > EmbedSet::kMaxEmbeds) {
                stopped = true;
                return;
            }
            EmbedTarget target;
            if (depth >= EmbedSet::kMaxDepth || !ParseTarget(inner, target)) continue;
            const int id = Find(folder, target.note);
            if (id == kNoNote || std::find(chain.begin(), chain.end(), (size_t)id) != chain.end()) continue;
            if (Cancelled()) {
                stopped = true;
                return;
            }
            // No note is added before the recursion below, so the reference holds.
            Note& note = notes[(size_t)id];
            if (!Load(note, folder)) continue;
            // The same file under another path (a hard link) is a cycle too; the document is known
            // by its path only.
            bool cycle = false;
            for (size_t at = 1; at < chain.size(); at++) {
                const RayoMd::File::FileIdentity& other = notes[chain[at]].identity;
                cycle = cycle || (other.volume == note.identity.volume && other.index == note.identity.index);
            }
            if (cycle) continue;
            std::string entryText;
            if (target.anchor.empty()) {
                if (note.text.size() > EmbedSet::kMaxBytes - bytes) continue;
                // Its lines from the body on, each ended by '\n', as the parser reads them.
                if (note.body <= note.text.size()) {
                    entryText.reserve(note.text.size() - note.body + 1);
                    entryText.assign(note.text, note.body, std::string::npos);
                    entryText += '\n';
                }
            } else if (note.text.size() <= kMaxScanBytes - scanned) {
                scanned += note.text.size();
                const std::vector<std::string_view> lines = Lines(note.text);
                // The body's first line: as many lines before it as line ends.
                const size_t start = note.text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
                const size_t body = note.body > note.text.size() ? lines.size()
                    : (size_t)std::count(note.text.begin() + (ptrdiff_t)start, note.text.begin() + (ptrdiff_t)note.body, '\n');
                entryText = target.block ? BlockText(lines, body, target.anchor) : Section(lines, body, target.anchor);
            }
            if (entryText.empty() || entryText.size() > EmbedSet::kMaxBytes - bytes) continue;
            const uint32_t child = (uint32_t)set.entries.size();
            OwnFootnotes(entryText, child);
            // A first line "---" would begin a front matter again.
            const std::string_view first = std::string_view(entryText).substr(0, entryText.find('\n'));
            if (Trim(first) == "---"sv) entryText.insert(0, "\n");
            EmbedEntry& entry = set.entries.emplace_back();
            entry.text = std::move(entryText);
            entry.folder = (uint32_t)note.folder;
            bytes += entry.text.size();
            set.entries[index].embeds.emplace_back(line, child);
            chain.push_back((size_t)id);
            Expand(child, entry.text, (int)entry.folder, depth + 1);
            chain.pop_back();
        }
    }

    // The value of `kind`, `a` and `b` in `keys` (made in `key`), kUnknown for none.
    static constexpr int kUnknown = -2;
    int Known(char kind, std::string_view a, std::string_view b = {}) {
        key.assign(1, kind);
        key.append(a);
        key.append(b);
        const auto found = keys.find(key);
        return found == keys.end() ? kUnknown : found->second;
    }
    // `value` for `kind`, `a` and `b`, which Known did not find.
    int Number(char kind, std::string_view a, std::string_view b, int value) {
        key.assign(1, kind);
        key.append(a);
        key.append(b);
        keys[key] = value;
        return value;
    }

    // The note `name` names from EmbedSet::folders[folder], kNoNote for none; each folder and name
    // looked up once.
    int Find(int folder, std::string_view name) {
        const std::string_view number(reinterpret_cast<const char*>(&folder), sizeof(folder));
        const int known = Known('n', number, name);
        if (known != kUnknown) return known;
        if (lookups >= kMaxLookups) return kNoNote;
        lookups++;
        std::string path = vault.Find(set.folders[(size_t)folder], name, true);
        return Number('n', number, name, path.empty() ? kNoNote : NoteOf(std::move(path)));
    }

    int NoteOf(std::string path) {
        const int known = Known('p', path);
        if (known != kUnknown) return known;
        notes.emplace_back().path = std::move(path);
        return Number('p', notes.back().path, {}, (int)notes.size() - 1);
    }

    // Whether `note` could be read: once a build, within the bytes left. `from`: the folder it was
    // found from, most often its own.
    bool Load(Note& note, int from) {
        if (!note.tried) {
            note.tried = true;
            if (vault.Read(note.path, std::min(EmbedSet::kMaxBytes, kMaxReadBytes - read), note.text, note.identity)) {
                read += note.text.size();
                note.body = BodyOffset(note.text);
                const std::string_view parent = ParentOf(note.path);
                note.folder = parent == set.folders[(size_t)from] ? from : Known('f', parent);
                if (note.folder == kUnknown) {
                    note.folder = Number('f', parent, {}, (int)set.folders.size());
                    set.folders.emplace_back(parent);
                }
            } else {
                note.identity.opened = false;
            }
        }
        return note.identity.opened;
    }

    const PdfOptions& options;
    EmbedSet& set;
    Vault vault;
    std::vector<Note> notes;      // [0]: the document
    std::vector<size_t> chain;    // the notes embedding the one being read
    // What was looked up, in one table: 'n', a folder's number and a name: the note it names
    // (kNoNote: none); 'p' and a path: its note; 'f' and a folder: its number in EmbedSet::folders.
    std::unordered_map<std::string, int> keys;
    std::string key;              // a lookup's key, its storage reused
    size_t bytes = 0;             // embedded text
    size_t read = 0;              // notes read
    size_t scanned = 0;           // notes looked through for a section or block
    size_t lookups = 0;
    bool stopped = false;
};

// Every image of `blocks` not yet given a folder resolves from EmbedSet::folders[base - 1].
void SetImageBase(const std::vector<Block>& blocks, uint32_t base) {
    for (const Block& block : blocks) {
        if (block.image != nullptr) {
            // emplace: an image of a nested note keeps its own folder.
            tEmbeds->imageBases.emplace(block.image.get(), base);
            for (const ImageSource& image : block.image->row) tEmbeds->imageBases.emplace(&image, base);
        }
        if (!block.children.empty()) SetImageBase(block.children, base);
    }
}

// `text` with the lines of `entry`'s embeds as empty HTML comments ("<!---->"): each ends the
// paragraph, list, quote or table before it as any comment line does, and gives no block, so the
// blocks of a later line follow it. Lines stay where they were. Done before the rewrite from
// Obsidian's dialect, which leaves such a line as it is and then has no embed to turn into a link.
std::string Blank(std::string_view text, const EmbedEntry& entry) {
    std::string out;
    out.reserve(text.size());
    size_t start = text.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0;
    size_t line = 0;
    size_t copied = 0;
    for (const auto& embed : entry.embeds) {
        for (; line < embed.first && start <= text.size(); line++) {
            const size_t next = text.find('\n', start);
            start = next == std::string_view::npos ? text.size() + 1 : next + 1;
        }
        if (start > text.size()) break;
        const size_t end = std::min(text.find('\n', start), text.size());
        out.append(text.substr(copied, start - copied));
        out += "<!---->";
        copied = end;
    }
    out.append(text.substr(copied));
    return out;
}

// The blocks of entry `index` of the thread's set (see SpliceEmbeds), appended to `out`.
void AppendEntry(std::vector<Block>& out, uint32_t index, FootnoteParse* notes) {
    const EmbedEntry& entry = tEmbeds->entries[index];
    std::string blanked;
    if (!entry.embeds.empty()) blanked = Blank(entry.text, entry);
    const std::string& source = entry.embeds.empty() ? entry.text : blanked;
    std::string rewritten;
    const std::string& text = RewriteObsidian(source, rewritten) ? rewritten : source;
    // Its own parse at the top level reads whether it may have footnotes; the document's stays.
    const bool possible = notes != nullptr && notes->possible;
    std::vector<Block> parsed = ParseMarkdownLevel(text, 0, notes);
    if (notes != nullptr) notes->possible = possible;
    SpliceEmbeds(parsed, entry, notes);
    SetImageBase(parsed, entry.folder + 1);
    out.insert(out.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
}

} // namespace

bool IsNoteTarget(std::string_view name) {
    return !IsMediaName(name);
}

bool MayEmbedNotes(const std::string& markdown, const PdfOptions& options) {
    return !options.embedsAsLinks && !options.sourcePath.empty() && markdown.find("![[") != std::string::npos;
}

bool ResolveEmbeds(const std::string& markdown, const PdfOptions& options, EmbedSet& set) {
    return Resolver(options, set).Run(markdown);
}

EmbedScope::EmbedScope(const EmbedSet& set) : previous(tEmbeds) {
    tEmbeds = &set;
}

EmbedScope::~EmbedScope() {
    tEmbeds = previous;
}

const EmbedSet* CurrentEmbeds() {
    return tEmbeds;
}

const std::string* EmbedFolder(uint32_t base) {
    if (tEmbeds == nullptr || base == 0 || base > tEmbeds->folders.size()) return nullptr;
    return &tEmbeds->folders[base - 1];
}

bool EmbedsActive() {
    return tEmbeds != nullptr;
}

uint32_t EmbedImageBase(const ImageSource& image) {
    if (tEmbeds == nullptr) return 0;
    const auto found = tEmbeds->imageBases.find(&image);
    return found == tEmbeds->imageBases.end() ? 0 : found->second;
}

bool RewriteDocument(std::string_view markdown, std::string& out) {
    if (tEmbeds == nullptr) return RewriteObsidian(markdown, out);
    std::string blanked = Blank(markdown, tEmbeds->entries[0]);
    if (!RewriteObsidian(blanked, out)) out = std::move(blanked);
    return true;
}

std::vector<Block> ParseWithEmbeds(const std::string& markdown, Footnotes& footnotes) {
    // A build parses again (another font): the images of the last parse are gone.
    tEmbeds->imageBases.clear();
    // The document's budget covers what it embeds.
    const ParseBudget budget(markdown.size() + tEmbeds->all.size());
    return ParseWithFootnotes(markdown, footnotes, &tEmbeds->entries[0]);
}

void SpliceEmbeds(std::vector<Block>& blocks, const EmbedEntry& entry, FootnoteParse* notes) {
    if (entry.embeds.empty()) return;
    std::vector<Block> out;
    out.reserve(blocks.size() + 4 * entry.embeds.size());
    size_t at = 0;
    for (const auto& [line, child] : entry.embeds) {
        while (at < blocks.size() && blocks[at].sourceLine < line) out.push_back(std::move(blocks[at++]));
        const size_t first = out.size();
        AppendEntry(out, child, notes);
        // The embed's line in the source map, for each of its blocks.
        for (size_t index = first; index < out.size(); index++) {
            out[index].sourceLine = line;
            out[index].sourceLines = 1;
        }
    }
    while (at < blocks.size()) out.push_back(std::move(blocks[at++]));
    blocks = std::move(out);
}

} // namespace TinyPdf::Internal
