// SPDX-License-Identifier: Apache-2.0

#include "book.h"

#include "dialect.h"
#include "embeds.h"
#include "footnotes.h"
#include "inline_markdown.h"
#include "markdown_parser.h"
#include "vault.h"
#include "../common/file_io.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace RayoMd::Book {
namespace {

namespace fs = std::filesystem;
using RayoMd::File::PathFromUtf8;
using RayoMd::File::PathToUtf8;
using TinyPdf::Internal::Block;
using TinyPdf::Internal::BlockType;
using TinyPdf::Internal::IsWithinRoot;
using TinyPdf::Internal::ResolvedPath;
#ifdef _WIN32
using TinyPdf::Internal::ResolvedWidePath;
#endif

std::string Lower(std::string_view text) {
    std::string lower(text);
    for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
    return lower;
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
    if (url.empty() || !std::isalpha((unsigned char)url[0])) return false;
    for (size_t at = 1; at < url.size(); at++) {
        const unsigned char ch = (unsigned char)url[at];
        if (ch == ':') return true;
        if (!std::isalnum(ch) && ch != '+' && ch != '-' && ch != '.') return false;
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

bool ReadFile(const fs::path& path, std::string& text) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && RayoMd::File::ReadAll(path, kMaxFileBytes, text);
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
    Result Read(const fs::path& path, std::string& text, bool once) {
        std::error_code ec;
        fs::path file = path;
        if (!root.empty()) {
#ifdef _WIN32
            // Resolved as the wide path it is: a file name with no UTF-8 form (an unpaired
            // surrogate, which NTFS allows) is read under its own name, not U+FFFD's. Only the
            // containment test reads it as UTF-8, where the root's own components are exact.
            file = ResolvedWidePath(path);
            if (file.empty()) return Result::Unreadable;
            if (!IsWithinRoot(PathToUtf8(file), root)) return Result::Outside;
#else
            const std::string resolved = ResolvedPath(PathToUtf8(path));
            if (resolved.empty()) return Result::Unreadable;
            if (!IsWithinRoot(resolved, root)) return Result::Outside;
            file = PathFromUtf8(resolved);
#endif
        }
        RayoMd::File::FileIdentity identity;
        const bool whole = fs::is_regular_file(file, ec) &&
            RayoMd::File::ReadAll(file, std::min(kMaxFileBytes, left), text, identity);
        if (once && identity.opened && read.count(identity) != 0) {
            text.clear();
            return Result::Again;
        }
        if (!whole) {
            return identity.opened && identity.size > left && identity.size <= kMaxFileBytes ? Result::PastBudget
                : Result::Unreadable;
        }
        read.insert(identity);
        left -= text.size();
        return Result::Read;
    }

    static std::string PastBudget(const std::string& path) {
        return "the book's files total more than 128 MiB with " + path;
    }

private:
    std::string root;
    std::set<RayoMd::File::FileIdentity> read;
    size_t left = kMaxBookBytes;
};

// mdBook's title: `title = "..."` in the book.toml beside SUMMARY.md's directory or in it.
std::string BookTomlTitle(const fs::path& directory) {
    for (const fs::path& toml : { directory / "book.toml", directory.parent_path() / "book.toml" }) {
        std::string text;
        if (!ReadFile(toml, text)) continue;
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
bool ListDirectory(const fs::path& directory, std::vector<fs::path>& files, std::string& error) {
    std::vector<std::pair<std::string, fs::path>> names;   // each file's name in UTF-8, and its path
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = PathToUtf8(it->path().filename());
        if (name.empty() || name[0] == '_' || name[0] == '.') continue;
        std::error_code typeError;
        if (HasMarkdownExtension(name) && it->is_regular_file(typeError)) {
            names.emplace_back(std::move(name), it->path());
        }
    }
    if (ec) {
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

// The deepest directory that holds all of `files`, empty when none does (other drives).
fs::path CommonDirectory(const std::vector<fs::path>& files) {
    fs::path common;
    for (size_t index = 0; index < files.size(); index++) {
        std::error_code ec;
        const fs::path directory = fs::absolute(files[index], ec).lexically_normal().parent_path();
        if (ec) return fs::path();
        if (index == 0) {
            common = directory;
            continue;
        }
        fs::path shared;
        for (auto a = common.begin(), b = directory.begin(); a != common.end() && b != directory.end() && *a == *b; ++a, ++b) {
            shared /= *a;
        }
        common = shared;
    }
    return common;
}

bool ReadSummaryBook(const fs::path& summaryPath, TinyPdf::Book& book, std::string& error) {
    std::string text;
    if (!ReadFile(summaryPath, text)) {
        error = "could not read " + PathToUtf8(summaryPath);
        return false;
    }
    const Summary summary = ParseSummary(text);
    fs::path directory = summaryPath.parent_path();
    if (directory.empty()) directory = ".";
    book.directory = PathToUtf8(directory);
    book.title = BookTomlTitle(directory);
    if (book.title.empty()) book.title = summary.title;
    bool listsFile = false;
    // The files must lie in SUMMARY.md's folder (for mdBook, its src/).
    std::string root = ResolvedPath(PathToUtf8(directory));
    if (root.empty()) {
        error = "could not read the folder of " + PathToUtf8(summaryPath);
        return false;
    }
    ChapterReader reader(std::move(root));
    for (const SummaryEntry& entry : summary.entries) {
        TinyPdf::BookChapter chapter;
        chapter.title = entry.title;
        chapter.depth = entry.depth;
        chapter.part = entry.part;
        if (!entry.part) {
            const fs::path path = (directory / PathFromUtf8(entry.path)).lexically_normal();
            chapter.path = PathToUtf8(path);
            listsFile = true;
            // A file listed again, under its name or another (a link, other letter case, "a/../"):
            // each is read once.
            const ChapterReader::Result read = reader.Read(path, chapter.markdown, true);
            if (read == ChapterReader::Result::Again) {
                error = PathToUtf8(summaryPath) + " lists " + entry.path + " twice";
                return false;
            }
            if (read == ChapterReader::Result::Outside) {
                error = PathToUtf8(summaryPath) + " lists " + entry.path + ", which lies outside its folder";
                return false;
            }
            if (read != ChapterReader::Result::Read) {
                error = read == ChapterReader::Result::PastBudget ? ChapterReader::PastBudget(chapter.path)
                    : "could not read " + chapter.path + ", which " + PathToUtf8(summaryPath) + " lists";
                return false;
            }
        }
        book.chapters.push_back(std::move(chapter));
    }
    if (!listsFile) {
        error = PathToUtf8(summaryPath) + " lists no file";
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

bool ReadBook(const std::vector<fs::path>& inputs, TinyPdf::Book& book, std::string& error) {
    book = TinyPdf::Book();
    std::error_code ec;
    fs::path summary;
    std::vector<fs::path> files;
    std::string root;   // a folder's: its files (links among them) must lie in it
    if (inputs.size() == 1 && fs::is_directory(inputs[0], ec)) {
        for (const fs::path& candidate : { inputs[0] / "SUMMARY.md", inputs[0] / "src" / "SUMMARY.md" }) {
            if (!fs::is_regular_file(candidate, ec)) continue;
            summary = candidate;
            break;
        }
        if (summary.empty()) {
            if (!ListDirectory(inputs[0], files, error)) return false;
            if (files.empty()) {
                error = "the folder " + PathToUtf8(inputs[0]) + " has no Markdown file";
                return false;
            }
            book.directory = PathToUtf8(inputs[0]);
            root = ResolvedPath(PathToUtf8(inputs[0]));
            if (root.empty()) {
                error = "could not read the folder " + PathToUtf8(inputs[0]);
                return false;
            }
        }
    } else if (inputs.size() == 1 && Lower(PathToUtf8(inputs[0].filename())) == "summary.md") {
        summary = inputs[0];
    } else {
        files = inputs;
        book.directory = PathToUtf8(CommonDirectory(inputs));
    }
    if (!summary.empty()) return ReadSummaryBook(summary, book, error);
    ChapterReader reader(std::move(root));
    for (const fs::path& file : files) {
        TinyPdf::BookChapter chapter;
        const ChapterReader::Result read = reader.Read(file, chapter.markdown, false);
        if (read == ChapterReader::Result::Outside) {
            error = PathToUtf8(file) + " leads outside the folder " + PathToUtf8(inputs[0]);
            return false;
        }
        if (read != ChapterReader::Result::Read) {
            error = read == ChapterReader::Result::PastBudget ? ChapterReader::PastBudget(PathToUtf8(file))
                : "could not read input Markdown file: " + PathToUtf8(file);
            return false;
        }
        chapter.path = PathToUtf8(file);
        book.chapters.push_back(std::move(chapter));
    }
    return true;
}

bool ReadLinkedBook(const fs::path& note, const TinyPdf::PdfOptions& options, int depth, TinyPdf::Book& book,
    size_t& leftOut, std::string& error) {
    book = TinyPdf::Book();
    leftOut = 0;
    const TinyPdf::Internal::DialectScope dialect(options.dialect);
    const bool byName = options.dialect == TinyPdf::MarkdownDialect::Obsidian;
    const std::string notePath = PathToUtf8(note);
    TinyPdf::Internal::Vault vault(options.vaultRoot, notePath);
    std::string first = ResolvedPath(notePath);
    if (first.empty()) {
        error = "could not read input Markdown file: " + notePath;
        return false;
    }
    if (!vault.Valid()) {
        error = "could not read the folder " + (options.vaultRoot.empty() ? PathToUtf8(note.parent_path()) : options.vaultRoot);
        return false;
    }
    book.directory = vault.Root();
    // The notes found, each once by its resolved path, in the order the book takes them.
    struct Found {
        std::string path;
        int depth;
    };
    std::set<std::string> known{ first };
    std::vector<Found> found{ { std::move(first), 0 } };
    std::set<RayoMd::File::FileIdentity> read;
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
        const bool whole = vault.Read(found[at].path, std::min(kMaxFileBytes, left), chapter.markdown, identity);
        if (!whole && identity.opened && identity.size > left && identity.size <= kMaxFileBytes) {
            leftOut = found.size() - at;
            break;
        }
        if (!whole || !read.insert(identity).second) {
            // A note that cannot be read, or one read before under another name, adds nothing.
            if (at == 0) {
                error = "could not read input Markdown file: " + notePath;
                return false;
            }
            continue;
        }
        left -= chapter.markdown.size();
        chapter.path = found[at].path;
        if (at > 0) chapter.title = PathToUtf8(PathFromUtf8(found[at].path).stem());
        if (found[at].depth < depth) {
            const std::string folder(TinyPdf::Internal::ParentOf(found[at].path));
            for (const std::string& url : LinksOf(chapter.markdown)) {
                if (url.empty() || url[0] == '#' || HasScheme(url)) continue;
                const std::string target = Decoded(std::string_view(url).substr(0, url.find('#')));
                // A wiki link names a note also when its name has a dot ("v1.2 notes"): ".md" is
                // added as for an embed, unless it names an image, a PDF, audio or video.
                if (!HasMarkdownExtension(target) && (!byName || !TinyPdf::Internal::IsNoteTarget(target))) continue;
                std::string path = vault.Find(folder, target, byName);
                if (!path.empty() && known.insert(path).second) found.push_back({ std::move(path), found[at].depth + 1 });
            }
        }
        book.chapters.push_back(std::move(chapter));
    }
    return true;
}

} // namespace RayoMd::Book

namespace TinyPdf::Internal {

namespace fs = std::filesystem;
using RayoMd::File::PathFromUtf8;
using RayoMd::File::PathToUtf8;

BookFiles::BookFiles() = default;
BookFiles::~BookFiles() = default;

void ParseBookFiles(const std::vector<BookChapter>& chapters, const std::vector<std::string>& texts,
    const std::vector<std::string>& titles, const PdfOptions& options, BookFiles& files) {
    const size_t count = chapters.size();
    files.blocks.resize(count);
    files.footnotes.resize(count);
    bool marker = false;
    for (size_t index = 0; index < count; index++) {
        if (options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed)) return;
        if (chapters[index].part) continue;
        std::vector<Block>& blocks = files.blocks[index];
        blocks = ParseMarkdown(texts[index], &files.footnotes[index]);
        marker = MarkContents(blocks) || marker;
        if (!titles[index].empty() && !BeginsWithTitle(blocks)) {
            Block title;
            title.type = BlockType::Heading;
            title.level = 1;
            title.text = titles[index];
            blocks.insert(blocks.begin(), std::move(title));
        }
        if (options.pageBreakBeforeLevel > 0) BreakBeforeHeadings(blocks, options.pageBreakBeforeLevel);
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
    const fs::path named = (PathFromUtf8(chapters[from].path).parent_path() / PathFromUtf8(path)).lexically_normal();
    // A book has tens or hundreds of files, so they are looked through in turn.
    for (const char* index : { "", "README.md", "index.md" }) {
        const std::string key = PathToUtf8(*index == '\0' ? named : (named / index).lexically_normal());
        for (size_t file = 0; file < paths.size(); file++) {
            if (!paths[file].empty() && paths[file] == key) return file;
        }
    }
    if (!ObsidianDialect()) return kNone;
    // A wiki link names a note by its name, or the end of its path: the first file whose path
    // ends in it, in any case.
    const std::string written = PathToUtf8(PathFromUtf8(path).lexically_normal());
    // A note's name with a dot ("v1.2 notes") is also that name with ".md", as Vault::Find takes it.
    for (const std::string& name : { written, written + ".md" }) {
        for (size_t file = 0; file < paths.size(); file++) {
            const std::string& candidate = paths[file];
            if (name.empty() || candidate.size() < name.size()) continue;
            const size_t at = candidate.size() - name.size();
            if (at > 0 && candidate[at - 1] != '/' && candidate[at - 1] != '\\') continue;
            size_t index = 0;
            while (index < name.size() && std::tolower((unsigned char)candidate[at + index]) == std::tolower((unsigned char)name[index])) index++;
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
        if (!chapters[index].part) paths[index] = PathToUtf8(PathFromUtf8(chapters[index].path).lexically_normal());
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
        const fs::path folder = PathFromUtf8(paths[to]).parent_path();
        std::string paragraph = "\n\nLinked from: ";
        bool first = true;
        for (const size_t from : linking[to]) {
            const fs::path relative = PathFromUtf8(paths[from]).lexically_relative(folder);
            if (relative.empty()) continue;
            if (!first) paragraph += ", ";
            first = false;
            // The name with every ASCII punctuation character escaped: text, whatever it holds.
            const std::string& title = chapters[from].title;
            const std::string name = title.empty() ? PathToUtf8(PathFromUtf8(paths[from]).stem()) : title;
            paragraph += '[';
            for (const char ch : name) {
                if (std::ispunct((unsigned char)ch)) paragraph += '\\';
                paragraph += ch;
            }
            // The path percent-encoded but for unreserved bytes and '/': no syntax of Markdown or
            // Obsidian's dialect in it, and BookFileOfLink decodes it.
            paragraph += "](<";
            std::string target = PathToUtf8(relative);
#ifdef _WIN32
            std::replace(target.begin(), target.end(), '\\', '/');
#endif
            for (const char ch : target) {
                const unsigned char byte = (unsigned char)ch;
                if (byte >= 0x80 || std::isalnum(byte) || ch == '-' || ch == '.' || ch == '_' || ch == '~' || ch == '/') {
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
