// SPDX-License-Identifier: Apache-2.0

#include "embeds.h"

#include "dialect.h"
#include "footnotes.h"
#include "inline_markdown.h"
#include "vault.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <string_view>
#include <unordered_set>

namespace TinyPdf::Internal {

namespace fs = std::filesystem;

namespace {

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
            htmlComment = line.find("-->") == kNone;
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
            if (line.find("$$") != kNone || Trim(line).empty()) displayMath = false;
            return false;
        }
        if (bracketMath) {
            const std::string_view text = Trim(line);
            if (text.empty() || (text.size() >= 2 && text.substr(text.size() - 2) == "\\]")) bracketMath = false;
            return false;
        }
        if (const Fence opening = FenceOf(line); opening.marker != 0) {
            fence = opening;
            return false;
        }
        size_t at = 0;
        while (at < line.size() && (IsBlank(line[at]) || line[at] == '>')) at++;
        if (line.substr(at, 2) == "$$" && line.find("$$", at + 2) == kNone) {
            displayMath = true;
            return false;
        }
        const std::string_view trimmed = Trim(line);
        if (trimmed == "\\[") {
            bracketMath = true;
            return false;
        }
        if (trimmed.substr(0, 4) == "<!--" && trimmed.find("-->", 4) == kNone) {
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
        for (size_t at = line.find("%%"); at != kNone; at = line.find("%%", at + 2)) count++;
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
    if (line.substr(0, 3) != "![[") return {};
    const size_t close = line.find("]]", 3);
    if (close == kNone || close == 3 || !Trim(line.substr(close + 2)).empty()) return {};
    const std::string_view inner = line.substr(3, close - 3);
    if (inner.find('[') != kNone || inner.find("%%") != kNone) return {};
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
    const size_t slash = note.find_last_of("/\\");
    const size_t dot = note.rfind('.');
    if (dot == kNone || (slash != kNone && dot < slash)) return false;
    const std::string_view extension = note.substr(dot + 1);
    for (const std::string_view media : { "png", "jpg", "jpeg", "svg", "gif", "webp", "bmp", "tif", "tiff", "avif", "pdf",
             "mp3", "wav", "m4a", "ogg", "3gp", "flac", "mp4", "webm", "ogv", "mov", "mkv" }) {
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

// The first line after a note's front matter (the first line "---" up to a line "---" or "...").
size_t BodyStart(const std::vector<std::string_view>& lines) {
    if (lines.empty() || Trim(lines[0]) != "---") return 0;
    for (size_t index = 1; index < lines.size(); index++) {
        const std::string_view trimmed = Trim(lines[index]);
        if (trimmed == "---" || trimmed == "...") return index + 1;
    }
    return 0;
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

// `text` with the footnote labels it defines made its own: "[^label]" as "[^label~eN]" in its
// definitions and references, outside fenced code and code spans. Another note's "[^1]" is then
// another note.
std::string OwnFootnotes(const std::string& text, uint32_t entry) {
    if (text.find("[^") == std::string::npos || text.find("]:") == std::string::npos) return text;
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
    if (defined.empty()) return text;
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
    return out;
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
        fs::path document = RayoMd::File::PathFromUtf8(options.sourcePath);
        fs::path folder = document.parent_path();
        if (folder.empty()) folder = ".";
        folder = ResolvedPath(folder);
        if (folder.empty()) return false;
        set.root = vault.Root();
        // No reallocation while Expand holds an entry's text.
        set.entries.reserve(EmbedSet::kMaxEmbeds + 1);
        set.entries.emplace_back();
        chain.push_back(ResolvedPath(document));
        Expand(0, markdown, folder, 0);
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

    struct NoteFile {
        std::string text;
        RayoMd::File::FileIdentity identity;
        std::vector<std::string_view> lines;   // of `text`
        size_t body = 0;                       // its first line after the front matter
        bool read = false;
    };

    bool Cancelled() const {
        return options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed);
    }

    // The embeds of entry `index`, whose text is `text`, in the note in `folder`, `depth` embeds
    // down from the document.
    void Expand(uint32_t index, std::string_view text, const fs::path& folder, int depth) {
        for (const auto& [line, inner] : StandaloneEmbeds(text)) {
            if (stopped || set.entries.size() > EmbedSet::kMaxEmbeds) {
                stopped = true;
                return;
            }
            EmbedTarget target;
            if (depth >= EmbedSet::kMaxDepth || !ParseTarget(inner, target)) continue;
            const fs::path path = Find(folder, target.note);
            if (path.empty() || std::find(chain.begin(), chain.end(), path) != chain.end()) continue;
            if (Cancelled()) {
                stopped = true;
                return;
            }
            const NoteFile* file = Load(path);
            if (file == nullptr) continue;
            bool cycle = false;
            for (const RayoMd::File::FileIdentity& identity : identities) {
                cycle = cycle || (!(identity < file->identity) && !(file->identity < identity));
            }
            if (cycle) continue;
            std::string entryText;
            if (target.anchor.empty()) {
                if (file->text.size() > EmbedSet::kMaxBytes - bytes) continue;
                entryText = Join(file->lines, file->body, file->lines.size());
            } else if (file->text.size() <= kMaxScanBytes - scanned) {
                scanned += file->text.size();
                entryText = target.block ? BlockText(file->lines, file->body, target.anchor)
                    : Section(file->lines, file->body, target.anchor);
            }
            if (entryText.empty() || entryText.size() > EmbedSet::kMaxBytes - bytes) continue;
            const uint32_t child = (uint32_t)set.entries.size();
            set.entries.emplace_back();
            EmbedEntry& entry = set.entries.back();
            entry.text = OwnFootnotes(entryText, child);
            // A first line "---" would begin a front matter again.
            if (Trim(entry.text.substr(0, entry.text.find('\n'))) == "---") entry.text.insert(0, "\n");
            const fs::path noteFolder = path.parent_path();
            entry.folder = FolderIndex(noteFolder);
            bytes += entry.text.size();
            set.entries[index].embeds.emplace_back(line, child);
            chain.push_back(path);
            identities.push_back(file->identity);
            Expand(child, set.entries[child].text, noteFolder, depth + 1);
            chain.pop_back();
            identities.pop_back();
        }
    }

    fs::path Find(const fs::path& folder, std::string_view note) {
        auto key = std::make_pair(RayoMd::File::PathToUtf8(folder), std::string(note));
        const auto known = found.find(key);
        if (known != found.end()) return known->second;
        if (lookups >= kMaxLookups) return fs::path();
        lookups++;
        fs::path path = vault.Find(folder, note, true);
        found.emplace(std::move(key), path);
        return path;
    }

    // The note at `path`, read once a build; null when it cannot be read or passes the bytes left.
    const NoteFile* Load(const fs::path& path) {
        NoteFile& file = files[RayoMd::File::PathToUtf8(path)];
        if (!file.read) {
            file.read = true;
            if (vault.Read(path, std::min(EmbedSet::kMaxBytes, kMaxReadBytes - read), file.text, file.identity)) {
                read += file.text.size();
                file.lines = Lines(file.text);
                file.body = BodyStart(file.lines);
            } else {
                file.identity.opened = false;
            }
        }
        return file.identity.opened ? &file : nullptr;
    }

    uint32_t FolderIndex(const fs::path& folder) {
        const auto [at, added] = folders.emplace(RayoMd::File::PathToUtf8(folder), (uint32_t)set.folders.size());
        if (added) set.folders.push_back(folder);
        return at->second;
    }

    const PdfOptions& options;
    EmbedSet& set;
    Vault vault;
    std::vector<fs::path> chain;                                  // the notes embedding the one being read
    std::vector<RayoMd::File::FileIdentity> identities;           // and which files they are
    std::map<std::pair<std::string, std::string>, fs::path> found;   // by folder and name
    std::map<std::string, NoteFile> files;
    std::map<std::string, uint32_t> folders;
    size_t bytes = 0;     // embedded text
    size_t read = 0;      // notes read
    size_t scanned = 0;   // notes looked through for a section or block
    size_t lookups = 0;
    bool stopped = false;
};

// Every image of `blocks` not yet given a folder resolves from EmbedSet::folders[folder].
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

// `markdown`, with `rewrite` rewritten from Obsidian's dialect, with the lines of its embeds as
// empty HTML comments ("<!---->"): each ends the paragraph, list, quote or table before it as any
// comment line does, and gives no block, so the blocks of a later line follow it. Lines stay
// where they were.
std::string ParseText(const std::string& markdown, const EmbedEntry& entry, bool rewrite) {
    std::string rewritten;
    const std::string& text = rewrite && RewriteObsidian(markdown, rewritten) ? rewritten : markdown;
    std::string out;
    out.reserve(text.size());
    size_t start = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    out.append(text, 0, start);
    size_t line = 0;
    size_t next = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (next < entry.embeds.size() && entry.embeds[next].first == line) {
            out += "<!---->";
            next++;
        } else {
            out.append(text, start, end - start);
        }
        if (end == text.size()) break;
        out += '\n';
        start = end + 1;
        line++;
    }
    return out;
}

// The blocks of entry `index` of the thread's set (see SpliceEmbeds), appended to `out`.
void AppendEntry(std::vector<Block>& out, uint32_t index, FootnoteParse* notes) {
    const EmbedEntry& entry = tEmbeds->entries[index];
    const std::string text = ParseText(entry.text, entry, true);
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

const fs::path* EmbedFolder(uint32_t base) {
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

std::vector<Block> ParseWithEmbeds(const std::string& markdown, Footnotes& footnotes) {
    // A build parses again (another font): the images of the last parse are gone.
    tEmbeds->imageBases.clear();
    const EmbedEntry& document = tEmbeds->entries[0];
    const std::string text = ParseText(markdown, document, false);
    // The document's budget covers what it embeds.
    const ParseBudget budget(text.size() + tEmbeds->all.size());
    return ParseWithFootnotes(text, footnotes, &document);
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
