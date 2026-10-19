// SPDX-License-Identifier: Apache-2.0

#include "book.h"

#include "dialect.h"
#include "embeds.h"
#include "footnotes.h"
#include "inline_markdown.h"
#include "markdown_parser.h"
#include "vault.h"
#include "../common/file_io.h"
#include "../common/thread_object.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace RayoMd::Book {
namespace {

using RayoMd::File::Path;
using RayoMd::File::PathFromUtf8;
using RayoMd::File::PathToUtf8;
using RayoMd::File::PathView;
using TinyPdf::Internal::Block;
using TinyPdf::Internal::BlockType;
using TinyPdf::Internal::IsWithinRoot;
using TinyPdf::Internal::ResolvedPath;

// ASCII classes, as <cctype> gives them in the "C" locale RayoMD runs in.
bool IsAlpha(unsigned char ch) {
    return (unsigned char)((ch | 32) - 'a') < 26;
}
bool IsAlnum(unsigned char ch) {
    return IsAlpha(ch) || (unsigned char)(ch - '0') < 10;
}
bool IsPunct(unsigned char ch) {
    return ch > ' ' && ch < 127 && !IsAlnum(ch);
}
char LowerAscii(char ch) {
    return ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
}

std::string Lower(std::string_view text) {
    std::string lower(text);
    for (char& ch : lower) ch = LowerAscii(ch);
    return lower;
}

// A set of strings (a map whose values nothing reads: the type the parser's footnotes and the
// image registry use, so it adds no code).
using StringSet = std::unordered_map<std::string, int>;

// A file identity as a key of a StringSet: its volume and index.
std::string KeyOf(const RayoMd::File::FileIdentity& identity) {
    std::string key(16, '\0');
    std::memcpy(&key[0], &identity.volume, 8);
    std::memcpy(&key[8], &identity.index, 8);
    return key;
}

// Whether a path's extension(), in any case, is ".md" or ".markdown" (a name that only begins with a
// dot has none).
bool HasMarkdownExtension(std::string_view path) {
#ifdef _WIN32
    const size_t slash = path.find_last_of("/\\");
#else
    const size_t slash = path.rfind('/');
#endif
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return false;
    const std::string extension = Lower(name.substr(dot));
    return extension == ".md" || extension == ".markdown";
}

std::string_view Trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

// A link's target with its %XX escapes decoded, as a browser reads it.
std::string Decoded(std::string_view text) {
    const auto digit = [](char ch) {
        return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    };
    std::string out;
    out.reserve(text.size());
    for (size_t at = 0; at < text.size(); at++) {
        int high = -1;
        int low = -1;
        if (text[at] == '%' && at + 2 < text.size() && (high = digit(text[at + 1])) >= 0 && (low = digit(text[at + 2])) >= 0) {
            out += (char)(high * 16 + low);
            at += 2;
        } else {
            out += text[at];
        }
    }
    return out;
}

// Whether a link names a URL ("https:", "mailto:") rather than a file.
bool HasScheme(std::string_view url) {
    if (url.empty() || !IsAlpha((unsigned char)url[0])) return false;
    for (size_t at = 1; at < url.size(); at++) {
        const unsigned char ch = (unsigned char)url[at];
        if (ch == ':') return true;
        if (!IsAlnum(ch) && ch != '+' && ch != '-' && ch != '.') return false;
    }
    return false;
}

// Heading text without the bytes that mark formulas in it.
std::string Plain(const std::string& text) {
    std::string plain;
    plain.reserve(text.size());
    for (const char ch : text) {
        if ((unsigned char)ch > 0x03) plain += ch;
    }
    return std::string(Trimmed(plain));
}

// The files that the links of inline text name, the text of each link its title; with
// `firstOnly`, that of its first link only.
void AddLinks(const std::string& text, int depth, bool firstOnly, Summary& summary) {
    const std::vector<TinyPdf::Internal::InlineSpan> spans = TinyPdf::Internal::ParseInlineSpans(text, false);
    for (size_t at = 0; at < spans.size();) {
        const std::string_view url = spans[at].url;
        if (url.empty()) {
            at++;
            continue;
        }
        // A link's text may be several spans, as "[A **B**](a.md)" is.
        std::string title;
        while (at < spans.size() && spans[at].url == url) title += spans[at++].text;
        if (url[0] == '#' || HasScheme(url)) continue;
        summary.entries.push_back({ std::string(Trimmed(title)), Decoded(std::string_view(url).substr(0, url.find('#'))), depth, false });
        if (firstOnly) return;
    }
}

// The link targets of inline text, in order, each link once (its spans share its target).
void AddUrls(const std::string& text, std::vector<std::string>& urls) {
    if (text.find('(') == std::string::npos && text.find('<') == std::string::npos && text.find(':') == std::string::npos) return;
    const std::vector<TinyPdf::Internal::InlineSpan> spans = TinyPdf::Internal::ParseInlineSpans(text, false);
    std::string_view previous;
    for (const TinyPdf::Internal::InlineSpan& span : spans) {
        const std::string_view url = span.url;
        if (url.empty() || url == previous) continue;
        previous = url;
        urls.emplace_back(url);
    }
}

// The link targets of blocks: those of paragraphs, list items, quotes and tables without formulas
// (whose cells are inline Markdown), and of linked images.
void AddBlockUrls(const std::vector<Block>& blocks, std::vector<std::string>& urls) {
    for (const Block& block : blocks) {
        switch (block.type) {
        case BlockType::Paragraph:
        case BlockType::Bullet:
        case BlockType::Numbered:
        case BlockType::Quote:
            AddUrls(block.text, urls);
            break;
        case BlockType::Table:
            if (block.hasMath) break;
            for (const std::vector<std::string>& row : block.rows) {
                for (const std::string& cell : row) AddUrls(cell, urls);
            }
            break;
        case BlockType::Image:
            if (block.image == nullptr) break;
            if (!block.image->link.empty()) urls.push_back(block.image->link);
            for (const TinyPdf::Internal::ImageSource& image : block.image->row) {
                if (!image.link.empty()) urls.push_back(image.link);
            }
            break;
        default:
            break;
        }
        AddBlockUrls(block.children, urls);
    }
}

// The link targets of a document of the thread's dialect (its wiki links are links then), its
// footnotes' too, in reading order.
std::vector<std::string> LinksOf(const std::string& markdown) {
    TinyPdf::Internal::Footnotes footnotes;
    const std::vector<Block> blocks = TinyPdf::Internal::ParseMarkdown(markdown, &footnotes);
    std::vector<std::string> urls;
    AddBlockUrls(blocks, urls);
    AddBlockUrls(footnotes.notes, urls);
    return urls;
}

// List items, each the file of its first link, and the items nested in them a level deeper.
void AddItems(const std::vector<Block>& blocks, int depth, Summary& summary) {
    for (const Block& block : blocks) {
        if (block.type != BlockType::Bullet && block.type != BlockType::Numbered) continue;
        AddLinks(block.text, depth, true, summary);
        AddItems(block.children, depth + 1, summary);
    }
}

// A file of the book, of at most 128 MiB, as the command line reads one document.
constexpr size_t kMaxFileBytes = 128u * 1024u * 1024u;

// ReadAll opens regular files only.
bool ReadFile(const Path& path, std::string& text) {
    return RayoMd::File::ReadAll(path, kMaxFileBytes, text);
}

// What a book's files may have in all: what one document may have, so that a book takes no more
// than the largest document. The largest books (documentation sets in mdBook) have a few to tens
// of MiB. A SUMMARY.md that listed one large file under many names (links, letter case,
// "a/../a.md") read it once for each, 128 MiB a name.
constexpr size_t kMaxBookBytes = kMaxFileBytes;

// The files of a book, read within kMaxBookBytes in all; each known by its identity, so that a
// file listed again under another name can be told. With a `root` (the resolved folder of a
// SUMMARY.md or of a book given as a folder), only files that lie in it once their links and ".."
// are resolved: a book from elsewhere must not put the reader's other files into its PDF.
class ChapterReader {
public:
    enum class Result { Read, Unreadable, Again, PastBudget, Outside };

    explicit ChapterReader(std::string folder = std::string()) : root(std::move(folder)) {}

    // `path` into `text`; Again, unread, when `once` and it is a file read before.
    Result Read(const Path& path, std::string& text, bool once) {
        Path resolved;
        if (!root.empty()) {
            // Resolved as the path it is: on Windows a file name with no UTF-8 form (an unpaired
            // surrogate, which NTFS allows) is read under its own name, not U+FFFD's. Only the
            // containment test reads it as UTF-8, where the root's own components are exact.
            resolved = RayoMd::File::Resolved(path);
            if (resolved.empty()) return Result::Unreadable;
#ifdef _WIN32
            if (!IsWithinRoot(PathToUtf8(resolved), root)) return Result::Outside;
#else
            if (!IsWithinRoot(resolved, root)) return Result::Outside;
#endif
        }
        const Path& file = root.empty() ? path : resolved;
        // ReadAll opens regular files only.
        RayoMd::File::FileIdentity identity;
        const bool whole = RayoMd::File::ReadAll(file, std::min(kMaxFileBytes, left), text, identity);
        if (once && identity.opened && read.find(KeyOf(identity)) != read.end()) {
            text.clear();
            return Result::Again;
        }
        if (!whole) {
            return identity.opened && identity.size > left && identity.size <= kMaxFileBytes ? Result::PastBudget
                : Result::Unreadable;
        }
        read[KeyOf(identity)] = 1;
        left -= text.size();
        return Result::Read;
    }

    static std::string PastBudget(const std::string& path) {
        return "the book's files total more than 128 MiB with " + path;
    }

private:
    std::string root;
    StringSet read;   // the files read, by KeyOf
    size_t left = kMaxBookBytes;
};

// mdBook's title: `title = "..."` in the book.toml beside SUMMARY.md's directory or in it.
std::string BookTomlTitle(const std::string& directory) {
    for (int up = 0; up < 2; up++) {
        std::string toml(up == 0 ? std::string_view(directory) : RayoMd::File::ParentOf(directory));
        RayoMd::File::AppendPath(toml, "book.toml");
        std::string text;
        if (!ReadFile(PathFromUtf8(toml), text)) continue;
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string_view line = Trimmed(std::string_view(text).substr(at, end - at));
            at = end + 1;
            if (line.substr(0, 5) != "title") continue;
            line = Trimmed(line.substr(5));
            if (line.empty() || line[0] != '=') continue;
            line = Trimmed(line.substr(1));
            if (line.empty() || line[0] != '"') continue;
            std::string title;
            for (size_t index = 1; index < line.size() && line[index] != '"'; index++) {
                if (line[index] == '\\' && index + 1 < line.size()) index++;
                title += line[index];
            }
            return title;
        }
    }
    return std::string();
}

// The Markdown files of a directory, not those of its subdirectories nor those whose names begin
// with '_' or '.', sorted by name byte by byte, README.md or else index.md first.
bool ListDirectory(const Path& directory, std::vector<Path>& files, std::string& error) {
    std::vector<std::pair<std::string, Path>> names;   // each file's name in UTF-8, and its path
    const bool listed = RayoMd::File::ForEachEntry(directory, [&](const RayoMd::File::Entry& entry) {
        std::string name = PathToUtf8(Path(entry.name));
        if (name.empty() || name[0] == '_' || name[0] == '.') return true;
        if (HasMarkdownExtension(name) && entry.type == RayoMd::File::FileType::Regular) {
            Path file = directory;
            RayoMd::File::AppendPath(file, entry.name);
            names.emplace_back(std::move(name), std::move(file));
        }
        return true;
    });
    if (!listed) {
        error = "could not list the folder " + PathToUtf8(directory);
        return false;
    }
    std::sort(names.begin(), names.end());
    for (const char* first : { "readme.md", "index.md" }) {
        const auto found = std::find_if(names.begin(), names.end(), [first](const auto& name) { return Lower(name.first) == first; });
        if (found == names.end()) continue;
        std::rotate(names.begin(), found, found + 1);
        break;
    }
    for (auto& name : names) files.push_back(std::move(name.second));
    return true;
}

// How much of the absolute, normalized paths `a` and `b` they share: their root, then each name
// both have in turn; 0 when their roots differ.
size_t SharedLength(PathView a, PathView b) {
    const size_t root = RayoMd::File::RootLength(a);
    if (root != RayoMd::File::RootLength(b) || a.substr(0, root) != b.substr(0, root)) return 0;
    size_t shared = root;
    for (size_t at = root; at < a.size() && at < b.size();) {
        size_t end = at;
        while (end < a.size() && a[end] != '/' && a[end] != '\\') end++;
        if (b.size() < end || b.substr(at, end - at) != a.substr(at, end - at) ||
            (end < b.size() && b[end] != '/' && b[end] != '\\')) {
            break;
        }
        shared = end;
        at = end + 1;
    }
    return shared;
}

// The deepest directory that holds all of `files`, empty when none does (other drives).
Path CommonDirectory(const std::vector<Path>& files) {
    Path common;
    for (size_t index = 0; index < files.size(); index++) {
        Path directory;
        if (!RayoMd::File::Absolute(files[index], directory)) return Path();
        RayoMd::File::Normalize(directory);
        directory.resize(RayoMd::File::ParentOf(PathView(directory)).size());
        if (index == 0) common = std::move(directory);
        else common.resize(SharedLength(common, directory));
    }
    return common;
}

bool ReadSummaryBook(const Path& summaryPath, TinyPdf::Book& book, std::string& error) {
    const std::string summaryName = PathToUtf8(summaryPath);
    std::string text;
    if (!ReadFile(summaryPath, text)) {
        error = "could not read " + summaryName;
        return false;
    }
    const Summary summary = ParseSummary(text);
    std::string directory(RayoMd::File::ParentOf(summaryName));
    if (directory.empty()) directory = ".";
    book.directory = directory;
    book.title = BookTomlTitle(directory);
    if (book.title.empty()) book.title = summary.title;
    bool listsFile = false;
    // The files must lie in SUMMARY.md's folder (for mdBook, its src/).
    std::string root = ResolvedPath(directory);
    if (root.empty()) {
        error = "could not read the folder of " + summaryName;
        return false;
    }
    ChapterReader reader(std::move(root));
    for (const SummaryEntry& entry : summary.entries) {
        TinyPdf::BookChapter chapter;
        chapter.title = entry.title;
        chapter.depth = entry.depth;
        chapter.part = entry.part;
        if (!entry.part) {
            chapter.path = directory;
            RayoMd::File::AppendPath(chapter.path, entry.path);
            RayoMd::File::Normalize(chapter.path);
            listsFile = true;
            // A file listed again, under its name or another (a link, other letter case, "a/../"):
            // each is read once.
            const ChapterReader::Result read = reader.Read(PathFromUtf8(chapter.path), chapter.markdown, true);
            if (read == ChapterReader::Result::Again) {
                error = summaryName + " lists " + entry.path + " twice";
                return false;
            }
            if (read == ChapterReader::Result::Outside) {
                error = summaryName + " lists " + entry.path + ", which lies outside its folder";
                return false;
            }
            if (read != ChapterReader::Result::Read) {
                error = read == ChapterReader::Result::PastBudget ? ChapterReader::PastBudget(chapter.path)
                    : "could not read " + chapter.path + ", which " + summaryName + " lists";
                return false;
            }
        }
        book.chapters.push_back(std::move(chapter));
    }
    if (!listsFile) {
        error = summaryName + " lists no file";
        return false;
    }
    return true;
}

} // namespace

Summary ParseSummary(const std::string& markdown) {
    Summary summary;
    const std::vector<Block> blocks = TinyPdf::Internal::ParseMarkdown(markdown);
    bool parts = false;   // a part has begun: the list items that follow are in it
    for (size_t index = 0; index < blocks.size(); index++) {
        const Block& block = blocks[index];
        if (block.type == BlockType::Heading) {
            std::string title = Plain(block.text);
            if (index == 0) {
                const std::string lower = Lower(title);
                if (lower != "summary" && lower != "table of contents" && lower != "contents") summary.title = std::move(title);
            } else if (block.level <= 2 && !title.empty()) {
                summary.entries.push_back({ std::move(title), std::string(), 0, true });
                parts = true;
            }
        } else if (block.type == BlockType::Paragraph) {
            // Prefix and suffix chapters, which are in no part.
            AddLinks(block.text, 0, false, summary);
        } else if (block.type == BlockType::Bullet || block.type == BlockType::Numbered) {
            AddLinks(block.text, parts ? 1 : 0, true, summary);
            AddItems(block.children, parts ? 2 : 1, summary);
        }
    }
    return summary;
}

bool ReadBook(const std::vector<Path>& inputs, TinyPdf::Book& book, std::string& error) {
    book = TinyPdf::Book();
    Path summary;
    std::vector<Path> files;
    std::string root;   // a folder's: its files (links among them) must lie in it
    const std::string input = inputs.size() == 1 ? PathToUtf8(inputs[0]) : std::string();
    if (inputs.size() == 1 && RayoMd::File::IsDirectory(inputs[0])) {
        // SUMMARY.md, then src/SUMMARY.md (mdBook), each name joined as operator/= joins it.
        for (int depth = 0; depth < 2; depth++) {
            Path candidate = inputs[0];
            if (depth == 1) RayoMd::File::AppendPath(candidate, RayoMd::File::PathOfAscii("src"));
            RayoMd::File::AppendPath(candidate, RayoMd::File::PathOfAscii("SUMMARY.md"));
            if (!RayoMd::File::IsRegularFile(candidate)) continue;
            summary = std::move(candidate);
            break;
        }
        if (summary.empty()) {
            if (!ListDirectory(inputs[0], files, error)) return false;
            if (files.empty()) {
                error = "the folder " + input + " has no Markdown file";
                return false;
            }
            book.directory = input;
            root = ResolvedPath(input);
            if (root.empty()) {
                error = "could not read the folder " + input;
                return false;
            }
        }
    } else if (inputs.size() == 1 && Lower(RayoMd::File::FileNameOf(input)) == "summary.md") {
        summary = inputs[0];
    } else {
        files = inputs;
        book.directory = PathToUtf8(CommonDirectory(inputs));
    }
    if (!summary.empty()) return ReadSummaryBook(summary, book, error);
    ChapterReader reader(std::move(root));
    for (const Path& file : files) {
        TinyPdf::BookChapter chapter;
        chapter.path = PathToUtf8(file);
        const ChapterReader::Result read = reader.Read(file, chapter.markdown, false);
        if (read == ChapterReader::Result::Outside) {
            error = chapter.path + " leads outside the folder " + input;
            return false;
        }
        if (read != ChapterReader::Result::Read) {
            error = read == ChapterReader::Result::PastBudget ? ChapterReader::PastBudget(chapter.path)
                : "could not read input Markdown file: " + chapter.path;
            return false;
        }
        book.chapters.push_back(std::move(chapter));
    }
    return true;
}

bool ReadLinkedBook(const Path& note, const TinyPdf::PdfOptions& options, int depth, TinyPdf::Book& book,
    size_t& leftOut, std::string& error) {
    book = TinyPdf::Book();
    leftOut = 0;
    const TinyPdf::Internal::DialectScope dialect(options.dialect);
    const bool byName = options.dialect == TinyPdf::MarkdownDialect::Obsidian;
    const std::string notePath = PathToUtf8(note);
    TinyPdf::Internal::Vault vault(options.vaultRoot, notePath);
    // The note's folder is the root without --vault: one lstat then, no realpath walk.
    std::string first = options.vaultRoot.empty() && vault.Valid() ? TinyPdf::Internal::ResolvedFilePath(notePath, vault.Root())
                                                                   : ResolvedPath(notePath);
    if (first.empty()) {
        error = "could not read input Markdown file: " + notePath;
        return false;
    }
    if (!vault.Valid()) {
        error = "could not read the folder " + (options.vaultRoot.empty() ? std::string(RayoMd::File::ParentOf(notePath)) : options.vaultRoot);
        return false;
    }
    book.directory = vault.Root();
    // The notes found, each once by its resolved path, in the order the book takes them.
    struct Found {
        std::string path;
        int depth;
    };
    StringSet known;
    known[first] = 1;
    std::vector<Found> found{ { std::move(first), 0 } };
    StringSet read;   // the notes read, by KeyOf
    size_t left = kMaxBookBytes;
    for (size_t at = 0; at < found.size(); at++) {
        if (options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed)) {
            error = "cancelled";
            return false;
        }
        if (book.chapters.size() == kMaxLinkedNotes) {
            leftOut = found.size() - at;
            break;
        }
        TinyPdf::BookChapter chapter;
        RayoMd::File::FileIdentity identity;
        const std::shared_ptr<const std::string> text = vault.Read(found[at].path, std::min(kMaxFileBytes, left), identity);
        const bool whole = text != nullptr;
        if (!whole && identity.opened && identity.size > left && identity.size <= kMaxFileBytes) {
            leftOut = found.size() - at;
            break;
        }
        if (!whole || read[KeyOf(identity)]++ != 0) {
            // A note that cannot be read, or one read before under another name, adds nothing.
            if (at == 0) {
                error = "could not read input Markdown file: " + notePath;
                return false;
            }
            continue;
        }
        chapter.markdown = *text;
        left -= chapter.markdown.size();
        chapter.path = found[at].path;
        if (at > 0) chapter.title = RayoMd::File::StemOf(found[at].path);
        if (found[at].depth < depth) {
            const std::string folder(RayoMd::File::ParentOf(found[at].path));
            for (const std::string& url : LinksOf(chapter.markdown)) {
                if (url.empty() || url[0] == '#' || HasScheme(url)) continue;
                const std::string target = Decoded(std::string_view(url).substr(0, url.find('#')));
                // A wiki link names a note also when its name has a dot ("v1.2 notes"): ".md" is
                // added as for an embed, unless it names an image, a PDF, audio or video.
                if (!HasMarkdownExtension(target) && (!byName || !TinyPdf::Internal::IsNoteTarget(target))) continue;
                std::string path = vault.Find(folder, target, byName);
                if (!path.empty() && known[path]++ == 0) found.push_back({ std::move(path), found[at].depth + 1 });
            }
        }
        book.chapters.push_back(std::move(chapter));
    }
    return true;
}

} // namespace RayoMd::Book

namespace TinyPdf::Internal {

using RayoMd::File::AppendPath;
using RayoMd::File::Normalize;
using RayoMd::File::ParentOf;
using RayoMd::File::RelativePath;
using RayoMd::File::StemOf;

BookFiles::BookFiles() = default;
BookFiles::~BookFiles() = default;

namespace {

// A book's files are parsed on several threads once they hold this much text, a thread for each
// kBookBytesPerThread of it: a parse costs about 7 ms a megabyte, starting a helper up to 0.3 ms.
constexpr size_t kParallelBookBytes = 256 * 1024;
constexpr size_t kBookBytesPerThread = 128 * 1024;

// `parse(index)` for each file of the book, on `threads` threads (this one among them) that each
// take the next file, until the build is abandoned; false then. Each helper parses under the
// calling thread's parser state: its dialect, hidden tags and WinAnsi character references (the
// embeds of a document never are a book's).
template <class Parse>
bool ForEachBookFile(size_t count, unsigned threads, const PdfOptions& options, Parse&& parse) {
    std::atomic<size_t> next{ 0 };
    std::atomic<bool> stopped{ false };
    const auto work = [&]() {
        for (size_t index; !stopped.load(std::memory_order_relaxed) && (index = next.fetch_add(1)) < count;) {
            if (options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed)) {
                stopped.store(true, std::memory_order_relaxed);
                return;
            }
            parse(index);
        }
    };
    const MarkdownDialect dialect = ObsidianDialect() ? MarkdownDialect::Obsidian : MarkdownDialect::GitHub;
    const bool hideTags = HiddenTags();
    const bool winAnsi = WinAnsiReferences::Active();
    std::vector<std::thread> helpers;
    helpers.reserve(threads - 1);
    for (unsigned helper = 1; helper < threads; helper++) {
        helpers.emplace_back([&]() {
            const DialectScope scope(dialect, hideTags);
            if (!winAnsi) return work();
            const WinAnsiReferences references;
            work();
        });
    }
    work();
    for (std::thread& helper : helpers) helper.join();
    return !stopped.load(std::memory_order_relaxed);
}

} // namespace

void ParseBookFiles(const std::vector<BookChapter>& chapters, const std::vector<std::string>& texts,
    const std::vector<std::string>& titles, const PdfOptions& options, BookFiles& files) {
    const size_t count = chapters.size();
    files.blocks.resize(count);
    files.footnotes.resize(count);
    std::vector<uint8_t> markers(count, 0);
    size_t bytes = 0;
    for (size_t index = 0; index < count; index++) bytes += chapters[index].part ? 0 : texts[index].size();
    unsigned threads = 1;
    if (bytes >= kParallelBookBytes && !EmbedsActive()) {
        threads = (unsigned)std::min<size_t>({ RayoMd::Thread::UsableCpus(), 6, count, bytes / kBookBytesPerThread });
    }
    const bool parsed = ForEachBookFile(count, std::max(threads, 1u), options, [&](size_t index) {
        if (chapters[index].part) return;
        std::vector<Block>& blocks = files.blocks[index];
        blocks = ParseMarkdown(texts[index], &files.footnotes[index]);
        markers[index] = MarkContents(blocks);
        if (!titles[index].empty() && !BeginsWithTitle(blocks)) {
            Block title;
            title.type = BlockType::Heading;
            title.level = 1;
            title.text = titles[index];
            blocks.insert(blocks.begin(), std::move(title));
        }
        if (options.pageBreakBeforeLevel > 0) BreakBeforeHeadings(blocks, options.pageBreakBeforeLevel);
    });
    if (!parsed) return;
    bool marker = false;
    for (size_t index = 0; index < count; index++) {
        marker = marker || markers[index] != 0;
        files.notes = files.notes || !files.footnotes[index].notes.empty();
    }
    if (!options.toc && !marker) return;
    std::vector<BookContentsFile> listed(count);
    for (size_t index = 0; index < count; index++) {
        const BookChapter& chapter = chapters[index];
        listed[index] = { chapter.part ? nullptr : &files.blocks[index], &titles[index], std::max(0, chapter.depth) };
    }
    files.contents = BookContents(listed, options.tocDepth);
}

size_t BookFileOfLink(const std::vector<BookChapter>& chapters, const std::vector<std::string>& paths, size_t from,
    std::string_view link) {
    constexpr size_t kNone = std::string::npos;
    if (link.empty() || link[0] == '/' || link[0] == '\\' || RayoMd::Book::HasScheme(link)) return kNone;
    const std::string path = RayoMd::Book::Decoded(link);
    std::string named(ParentOf(chapters[from].path));
    AppendPath(named, path);
    Normalize(named);
    // A book has tens or hundreds of files, so they are looked through in turn.
    std::string key;
    for (const char* index : { "", "README.md", "index.md" }) {
        key = named;
        if (*index != '\0') {
            AppendPath(key, index);
            Normalize(key);
        }
        for (size_t file = 0; file < paths.size(); file++) {
            if (!paths[file].empty() && paths[file] == key) return file;
        }
    }
    if (!ObsidianDialect()) return kNone;
    // A wiki link names a note by its name, or the end of its path: the first file whose path
    // ends in it, in any case.
    std::string written = path;
    Normalize(written);
    // A note's name with a dot ("v1.2 notes") is also that name with ".md", as Vault::Find takes it.
    for (const std::string& name : { written, written + ".md" }) {
        for (size_t file = 0; file < paths.size(); file++) {
            const std::string& candidate = paths[file];
            if (name.empty() || candidate.size() < name.size()) continue;
            const size_t at = candidate.size() - name.size();
            if (at > 0 && candidate[at - 1] != '/' && candidate[at - 1] != '\\') continue;
            size_t index = 0;
            while (index < name.size() && RayoMd::Book::LowerAscii(candidate[at + index]) == RayoMd::Book::LowerAscii(name[index])) index++;
            if (index == name.size()) return file;
        }
        if (written.empty() || !IsNoteTarget(written)) break;
    }
    return kNone;
}

void AddBacklinks(const Book& book, std::vector<std::string>& texts) {
    const std::vector<BookChapter>& chapters = book.chapters;
    std::vector<std::string> paths(chapters.size());
    for (size_t index = 0; index < chapters.size(); index++) {
        if (chapters[index].part) continue;
        paths[index] = chapters[index].path;
        Normalize(paths[index]);
    }
    // The files linking to each, in the book's order.
    std::vector<std::vector<size_t>> linking(chapters.size());
    for (size_t from = 0; from < chapters.size(); from++) {
        if (chapters[from].part) continue;
        for (const std::string& url : RayoMd::Book::LinksOf(texts[from])) {
            if (url.empty() || url[0] == '#') continue;
            const size_t to = BookFileOfLink(chapters, paths, from, std::string_view(url).substr(0, url.find('#')));
            if (to == std::string::npos || to == from || (!linking[to].empty() && linking[to].back() == from)) continue;
            linking[to].push_back(from);
        }
    }
    for (size_t to = 0; to < chapters.size(); to++) {
        if (linking[to].empty()) continue;
        const std::string_view folder = ParentOf(paths[to]);
        std::string paragraph = "\n\nLinked from: ";
        bool first = true;
        for (const size_t from : linking[to]) {
            std::string target = RelativePath(paths[from], folder);
            if (target.empty()) continue;
            if (!first) paragraph += ", ";
            first = false;
            // The name with every ASCII punctuation character escaped: text, whatever it holds.
            const std::string& title = chapters[from].title;
            const std::string_view name = title.empty() ? StemOf(paths[from]) : std::string_view(title);
            paragraph += '[';
            for (const char ch : name) {
                if (RayoMd::Book::IsPunct((unsigned char)ch)) paragraph += '\\';
                paragraph += ch;
            }
            // The path percent-encoded but for unreserved bytes and '/': no syntax of Markdown or
            // Obsidian's dialect in it, and BookFileOfLink decodes it.
            paragraph += "](<";
#ifdef _WIN32
            std::replace(target.begin(), target.end(), '\\', '/');
#endif
            for (const char ch : target) {
                const unsigned char byte = (unsigned char)ch;
                if (byte >= 0x80 || RayoMd::Book::IsAlnum(byte) || ch == '-' || ch == '.' || ch == '_' || ch == '~' || ch == '/') {
                    paragraph += ch;
                } else {
                    static const char kHex[] = "0123456789ABCDEF";
                    paragraph += '%';
                    paragraph += kHex[byte >> 4];
                    paragraph += kHex[byte & 15];
                }
            }
            paragraph += ">)";
        }
        if (!first) texts[to] += paragraph + '\n';
    }
}

} // namespace TinyPdf::Internal
