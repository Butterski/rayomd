#include "book.h"

#include "inline_markdown.h"
#include "markdown_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <system_error>

namespace RayoMd::Book {
namespace {

namespace fs = std::filesystem;
using TinyPdf::Internal::Block;
using TinyPdf::Internal::BlockType;

std::string Lower(std::string_view text) {
    std::string lower(text);
    for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
    return lower;
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
        const std::string& url = spans[at].url;
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

// List items, each the file of its first link, and the items nested in them a level deeper.
void AddItems(const std::vector<Block>& blocks, int depth, Summary& summary) {
    for (const Block& block : blocks) {
        if (block.type != BlockType::Bullet && block.type != BlockType::Numbered) continue;
        AddLinks(block.text, depth, true, summary);
        AddItems(block.children, depth + 1, summary);
    }
}

// A file of the book, of at most 128 MiB, as the command line reads one document.
bool ReadFile(const fs::path& path, std::string& text) {
    constexpr std::uintmax_t kMaxFileBytes = 128u * 1024u * 1024u;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxFileBytes) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    text.assign((size_t)size, '\0');
    return size == 0 || (bool)file.read(&text[0], (std::streamsize)size);
}

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
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().u8string();
        if (name.empty() || name[0] == '_' || name[0] == '.') continue;
        const std::string extension = Lower(it->path().extension().u8string());
        std::error_code typeError;
        if ((extension == ".md" || extension == ".markdown") && it->is_regular_file(typeError)) names.push_back(std::move(name));
    }
    if (ec) {
        error = "could not list the folder " + directory.u8string();
        return false;
    }
    std::sort(names.begin(), names.end());
    for (const char* first : { "readme.md", "index.md" }) {
        const auto found = std::find_if(names.begin(), names.end(), [first](const std::string& name) { return Lower(name) == first; });
        if (found == names.end()) continue;
        std::rotate(names.begin(), found, found + 1);
        break;
    }
    for (const std::string& name : names) files.push_back(directory / fs::u8path(name));
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
        error = "could not read " + summaryPath.u8string();
        return false;
    }
    const Summary summary = ParseSummary(text);
    fs::path directory = summaryPath.parent_path();
    if (directory.empty()) directory = ".";
    book.directory = directory.u8string();
    book.title = BookTomlTitle(directory);
    if (book.title.empty()) book.title = summary.title;
    std::vector<std::string> listed;
    for (const SummaryEntry& entry : summary.entries) {
        TinyPdf::BookChapter chapter;
        chapter.title = entry.title;
        chapter.depth = entry.depth;
        chapter.part = entry.part;
        if (!entry.part) {
            const fs::path path = (directory / fs::u8path(entry.path)).lexically_normal();
            chapter.path = path.u8string();
            if (std::find(listed.begin(), listed.end(), chapter.path) != listed.end()) {
                error = summaryPath.u8string() + " lists " + entry.path + " twice";
                return false;
            }
            listed.push_back(chapter.path);
            if (!ReadFile(path, chapter.markdown)) {
                error = "could not read " + chapter.path + ", which " + summaryPath.u8string() + " lists";
                return false;
            }
        }
        book.chapters.push_back(std::move(chapter));
    }
    if (listed.empty()) {
        error = summaryPath.u8string() + " lists no file";
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
    if (inputs.size() == 1 && fs::is_directory(inputs[0], ec)) {
        for (const fs::path& candidate : { inputs[0] / "SUMMARY.md", inputs[0] / "src" / "SUMMARY.md" }) {
            if (!fs::is_regular_file(candidate, ec)) continue;
            summary = candidate;
            break;
        }
        if (summary.empty()) {
            if (!ListDirectory(inputs[0], files, error)) return false;
            if (files.empty()) {
                error = "the folder " + inputs[0].u8string() + " has no Markdown file";
                return false;
            }
            book.directory = inputs[0].u8string();
        }
    } else if (inputs.size() == 1 && Lower(inputs[0].filename().u8string()) == "summary.md") {
        summary = inputs[0];
    } else {
        files = inputs;
        book.directory = CommonDirectory(inputs).u8string();
    }
    if (!summary.empty()) return ReadSummaryBook(summary, book, error);
    for (const fs::path& file : files) {
        TinyPdf::BookChapter chapter;
        if (!ReadFile(file, chapter.markdown)) {
            error = "could not read input Markdown file: " + file.u8string();
            return false;
        }
        chapter.path = file.u8string();
        book.chapters.push_back(std::move(chapter));
    }
    return true;
}

} // namespace RayoMd::Book

namespace TinyPdf::Internal {

BookFiles::BookFiles() = default;
BookFiles::~BookFiles() = default;

void ParseBookFiles(const std::vector<BookChapter>& chapters, const std::vector<std::string>& texts,
    const std::vector<std::string>& titles, bool toc, int tocDepth, BookFiles& files) {
    const size_t count = chapters.size();
    files.blocks.resize(count);
    files.footnotes.resize(count);
    bool marker = false;
    for (size_t index = 0; index < count; index++) {
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
        files.notes = files.notes || !files.footnotes[index].notes.empty();
    }
    if (!toc && !marker) return;
    std::vector<BookContentsFile> listed(count);
    for (size_t index = 0; index < count; index++) {
        const BookChapter& chapter = chapters[index];
        listed[index] = { chapter.part ? nullptr : &files.blocks[index], &titles[index], std::max(0, chapter.depth) };
    }
    files.contents = BookContents(listed, tocDepth);
}

} // namespace TinyPdf::Internal
