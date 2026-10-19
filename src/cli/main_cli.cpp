// SPDX-License-Identifier: Apache-2.0

#include "rayomd/tiny_pdf.h"
#include "../common/file_io.h"
#include "../common/text_utils.h"
#include "../common/batch_report.h"
#include "../common/profiling.h"
#include "../core/book.h"
#include "../core/export_options.h"
#include "../core/rayomd_pdf_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using RayoMd::File::Path;
using RayoMd::File::PathToUtf8;

#ifndef RAYOMD_VERSION
#define RAYOMD_VERSION "0.0.0"
#endif

constexpr long long kMaxMarkdownInputBytes = 128LL * 1024LL * 1024LL;

// The command line prints through C stdio (see file_io.h). A message is one fwrite, and stderr
// is unbuffered, so the lines of batch workers do not mix.
void Print(std::FILE* stream, std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stream);
}

// `message` and a line break on standard error.
void PrintError(std::string message) {
    message += '\n';
    Print(stderr, message);
}

// " (why)" after a message, or nothing.
std::string Reason(const std::string& why) {
    return why.empty() ? std::string() : " (" + why + ")";
}

// A line of standard input without its line break; false at the end. Standard output is flushed
// first, as std::cin's tie to std::cout did, so a --serve client gets each reply before it sends
// the next path.
bool ReadLine(std::string& line) {
    std::fflush(stdout);
    line.clear();
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), stdin)) {
        const size_t length = std::strlen(buffer);
        if (length != 0 && buffer[length - 1] == '\n') {
            line.append(buffer, length - 1);
            return true;
        }
        line.append(buffer, length);
    }
    return !line.empty();
}

bool ReadUtf8FilePortable(const Path& path, std::string& content) {
    RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Io);
    return RayoMd::File::ReadAll(path, (size_t)kMaxMarkdownInputBytes, content);
}

bool ReadStdinPortable(std::string& content) {
    content.clear();
    char buffer[64 * 1024];
    for (;;) {
        const size_t read = std::fread(buffer, 1, sizeof(buffer), stdin);
        if (read > 0) {
            if ((long long)(content.size() + read) > kMaxMarkdownInputBytes) return false;
            content.append(buffer, read);
        }
        if (read < sizeof(buffer)) return !std::ferror(stdin);
    }
}

bool WriteBinaryFilePortable(const Path& path, const std::string& content) {
    RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Io);
    return RayoMd::File::WriteAll(path, content);
}

bool WriteUtf8FilePortable(const Path& path, const std::string& content) {
    return WriteBinaryFilePortable(path, content);
}

bool ReadBinaryFileLimited(const Path& path, size_t maximum, std::string& content) {
    return RayoMd::File::ReadAll(path, maximum, content);
}

bool WriteNewFileAtomicPortable(const Path& path, const std::string& content, bool& existed) {
    existed = false;
    RayoMd::File::FileStatus status;
    if (RayoMd::File::StatusOf(path, status)) {
        existed = true;
        return false;
    }
    static std::atomic<uint64_t> counter{0};
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        // ".NAME.rayomd-N.tmp" beside it.
        Path name = RayoMd::File::PathOfAscii(".");
        name += RayoMd::File::FileNameOf(RayoMd::File::PathView(path));
        name += RayoMd::File::PathOfAscii(".rayomd-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp");
        Path temporary(RayoMd::File::ParentOf(RayoMd::File::PathView(path)));
        RayoMd::File::AppendPath(temporary, name);
#ifndef _WIN32
        int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        size_t written = 0;
        bool writeOk = true;
        while (written < content.size()) {
            ssize_t count = ::write(fd, content.data() + written, content.size() - written);
            if (count <= 0) {
                if (count < 0 && errno == EINTR) continue;
                writeOk = false;
                break;
            }
            written += static_cast<size_t>(count);
        }
        if (::close(fd) != 0) writeOk = false;
        if (!writeOk) {
            ::unlink(temporary.c_str());
            return false;
        }
        if (::link(temporary.c_str(), path.c_str()) == 0) {
            ::unlink(temporary.c_str());
            return true;
        }
        int linkError = errno;
        ::unlink(temporary.c_str());
        if (linkError == EEXIST) {
            existed = true;
            return false;
        }
        return false;
#else
        if (RayoMd::File::StatusOf(temporary, status)) continue;
        if (!WriteBinaryFilePortable(temporary, content)) return false;
        if (RayoMd::File::Rename(temporary, path)) return true;
        RayoMd::File::Remove(temporary);
        return false;
#endif
    }
    return false;
}

int RecoveryExitCode(RayoMd::PdfSource::Status status) {
    using Status = RayoMd::PdfSource::Status;
    switch (status) {
    case Status::Ok: return 0;
    case Status::NotReversible: return 30;
    case Status::UnsupportedProfile: return 31;
    case Status::LimitExceeded: return 33;
    case Status::CorruptPdf:
    case Status::IntegrityMismatch:
    case Status::InvalidUtf8: return 32;
    }
    return 32;
}

int RunInspectSource(const Path& inputPath) {
    std::string pdf;
    if (!ReadBinaryFileLimited(inputPath, RayoMd::PdfSource::kMaxPdfBytes, pdf)) {
        PrintError("Error: could not read PDF or input exceeds " + std::to_string(RayoMd::PdfSource::kMaxPdfBytes) +
            " bytes.");
        return 3;
    }
    RayoMd::PdfSource::Result result = RayoMd::PdfSource::Inspect(pdf, false);
    if (!result.Ok()) {
        PrintError(std::string("Error: ") + RayoMd::PdfSource::StatusMessage(result.status) + ".");
        return RecoveryExitCode(result.status);
    }
    Print(stdout, "status=intact\nprofile=" + result.info.profile + "\nproducer_version=" + result.info.producerVersion +
        "\nsource_bytes=" + std::to_string(result.info.sourceBytes) + "\nencoding=" + result.info.encoding +
        "\ndigest=valid\nattachment=" + result.info.attachmentName + "\n");
    return 0;
}

int RunRecoverSource(const Path& inputPath, const Path& outputPath) {
    std::string pdf;
    if (!ReadBinaryFileLimited(inputPath, RayoMd::PdfSource::kMaxPdfBytes, pdf)) {
        PrintError("Error: could not read PDF or input exceeds " + std::to_string(RayoMd::PdfSource::kMaxPdfBytes) +
            " bytes.");
        return 3;
    }
    RayoMd::PdfSource::Result result = RayoMd::PdfSource::Inspect(pdf, true);
    if (!result.Ok()) {
        PrintError(std::string("Error: ") + RayoMd::PdfSource::StatusMessage(result.status) + ".");
        return RecoveryExitCode(result.status);
    }
    bool existed = false;
    if (!WriteNewFileAtomicPortable(outputPath, result.source, existed)) {
        PrintError(std::string(existed ? "Error: destination already exists: " : "Error: could not write recovered source: ") +
            PathToUtf8(outputPath));
        return existed ? 34 : 12;
    }
    Print(stdout, "Recovered " + std::to_string(result.info.sourceBytes) + " bytes to " + PathToUtf8(outputPath) + "\n");
    return 0;
}

bool IsDirectoryPortable(const Path& path, std::string& error) {
    RayoMd::File::FileStatus status;
    int code = 0;
    if (!RayoMd::File::StatusOf(path, status, &code)) {
        error = code == ENOENT || code == ENOTDIR ? "does not exist" : RayoMd::File::ErrorText(code);
        return false;
    }
    if (status.type != RayoMd::File::FileType::Directory) {
        error = "not a directory";
        return false;
    }
    error.clear();
    return true;
}

bool EnsureDirectoryPortable(const Path& path, std::string& error) {
    int code = 0;
    if (!RayoMd::File::CreateDirectories(path, &code)) {
        error = RayoMd::File::ErrorText(code);
        return false;
    }
    return IsDirectoryPortable(path, error);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

struct CliExportOptions {
    TinyPdf::PdfStyle style = TinyPdf::PdfStyle::Elegant;
    TinyPdf::PdfMargin margin = TinyPdf::PdfMargin::Normal();
    TinyPdf::PdfPageSize pageSize;
    bool enableUrlImages = false;
    bool allowUnsafeLocalImages = false;
    bool embedSource = false;
    bool imageCache = true;
    bool pageNumbers = false;
    bool compress = false;
    bool fastImages = false;
    bool pdfa = false;
    bool pdfua = false;
    bool highlightCode = true;
    bool diagrams = true;
    bool toc = false;
    int tocDepth = 3;
    int pageBreakBeforeLevel = 0;
    int numberHeadings = 0;
    TinyPdf::MarkdownDialect dialect = TinyPdf::MarkdownDialect::GitHub;
    TinyPdf::FootnotePlacement footnotes = TinyPdf::FootnotePlacement::End;
    std::string vaultRoot;       // --vault=DIR
    bool embedsAsLinks = false;  // --embeds=link
    bool backlinks = false;      // --backlinks
    int followLinks = 0;         // --follow-links[=N]: 0 for none
    bool blackText = false;
    bool hideTags = false;
    TinyPdf::PdfTheme theme;
    unsigned workers = 0;
    // Batch modes: subfolders too, mirrored under the output folder; documents whose PDF is
    // newer than their Markdown left as they are; a JSON Lines report ("-" for stdout).
    bool recursive = false;
    bool skipUnchanged = false;
    std::string report;
};

bool ParseExportOptions(int argc, char** argv, int start, CliExportOptions& options,
    std::string& error) {
    for (int i = start; i < argc; i++) {
        std::string value = Lower(argv[i]);
        if (value == "pandoc") {
            error = "the pandoc engine was removed; RayoMD exports with its native renderer only";
            return false;
        }
        else if (value == "native") continue;   // the only engine, still accepted from older command lines
        else if (value == "--allow-url-images") options.enableUrlImages = true;
        else if (value == "--allow-unsafe-local-images" || value == "--unsafe-local-images") options.allowUnsafeLocalImages = true;
        else if (value == "--embed-source") options.embedSource = true;
        else if (value == "--no-image-cache") options.imageCache = false;
        else if (value == "--page-numbers") options.pageNumbers = true;
        else if (value == "--compress") options.compress = true;
        else if (value == "--fast-images") options.fastImages = true;
        else if (value == "--pdfa") options.pdfa = true;
        else if (value == "--pdfua") options.pdfua = true;
        else if (value == "--no-highlight") options.highlightCode = false;
        else if (value == "--no-diagrams") options.diagrams = false;
        else if (value == "--black-text") options.blackText = true;
        else if (value == "--hide-tags") options.hideTags = true;
        else if (value == "--toc") options.toc = true;
        else if (value.rfind("--toc-depth=", 0) == 0) {
            if (value.size() != 13 || value[12] < '1' || value[12] > '6') {
                error = "--toc-depth must be 1 to 6";
                return false;
            }
            options.tocDepth = value[12] - '0';
        }
        else if (value.rfind("--page-break-before=", 0) == 0) {
            if (value.size() != 22 || (value[20] != 'h' && value[20] != 'H') || value[21] < '1' || value[21] > '6') {
                error = "--page-break-before must be h1 to h6";
                return false;
            }
            options.pageBreakBeforeLevel = value[21] - '0';
        }
        else if (value == "--number-headings") options.numberHeadings = 3;
        else if (value.rfind("--number-headings=", 0) == 0) {
            if (value.size() != 19 || value[18] < '1' || value[18] > '6') {
                error = "--number-headings must be 1 to 6";
                return false;
            }
            options.numberHeadings = value[18] - '0';
        }
        else if (value == "--dialect=github") options.dialect = TinyPdf::MarkdownDialect::GitHub;
        else if (value == "--dialect=obsidian") options.dialect = TinyPdf::MarkdownDialect::Obsidian;
        else if (value.rfind("--dialect=", 0) == 0) {
            error = "--dialect must be github or obsidian";
            return false;
        }
        else if (value == "--footnotes=end") options.footnotes = TinyPdf::FootnotePlacement::End;
        else if (value == "--footnotes=page") options.footnotes = TinyPdf::FootnotePlacement::Page;
        else if (value.rfind("--footnotes=", 0) == 0) {
            error = "--footnotes must be end or page";
            return false;
        }
        else if (value.rfind("--vault=", 0) == 0) {
            options.vaultRoot = argv[i] + 8;
            if (options.vaultRoot.empty()) {
                error = "--vault needs a folder";
                return false;
            }
        }
        else if (value == "--embeds=link") options.embedsAsLinks = true;
        else if (value == "--embeds=content") options.embedsAsLinks = false;
        else if (value.rfind("--embeds=", 0) == 0) {
            error = "--embeds must be link or content";
            return false;
        }
        else if (value == "--backlinks") options.backlinks = true;
        else if (value == "--follow-links") options.followLinks = 1;
        else if (value.rfind("--follow-links=", 0) == 0) {
            if (value.size() != 16 || value[15] < '1' || value[15] > '6') {
                error = "--follow-links must be 1 to 6";
                return false;
            }
            options.followLinks = value[15] - '0';
        }
        else if (value.rfind("--theme=", 0) == 0) {
            const Path themePath = RayoMd::File::PathFromUtf8(argv[i] + 8);
            std::string text;
            std::string themeError;
            if (themePath.empty() || !ReadUtf8FilePortable(themePath, text)) {
                error = "--theme: cannot read " + PathToUtf8(themePath);
                return false;
            }
            if (!TinyPdf::Internal::ParseTheme(text, PathToUtf8(Path(RayoMd::File::ParentOf(RayoMd::File::PathView(themePath)))),
                    options.theme, themeError)) {
                error = "--theme " + PathToUtf8(themePath) + ": " + themeError;
                return false;
            }
        }
        else if (value.rfind("--page-size=", 0) == 0) {
            if (!TinyPdf::Internal::ParsePageSize(std::string_view(value).substr(12), options.pageSize)) {
                error = "--page-size must be a4, a3, a5, letter or legal (each also with -landscape), "
                    "WIDTHxHEIGHT in mm, cm, in or pt, such as 210x297mm, or WIDTHxauto, such as 210mmxauto";
                return false;
            }
        }
        else if (value == "--recursive") options.recursive = true;
        else if (value == "--skip-unchanged") options.skipUnchanged = true;
        else if (value.rfind("--report=", 0) == 0) {
            options.report = argv[i] + 9;
            if (options.report.empty()) {
                error = "--report needs a file name, or - for standard output";
                return false;
            }
        }
        else if (value.rfind("--workers=", 0) == 0) {
            std::string count = value.substr(10);
            char* end = nullptr;
            unsigned long parsed = std::strtoul(count.c_str(), &end, 10);
            if (!end || *end != char(0) || parsed < 1 || parsed > 64) {
                error = "--workers must be between 1 and 64";
                return false;
            }
            options.workers = static_cast<unsigned>(parsed);
        }
        else {
            TinyPdf::PdfMargin margin;
            TinyPdf::PdfStyle style;
            if (TinyPdf::Internal::ParsePdfMargin(argv[i], margin)) options.margin = margin;
            else if (TinyPdf::Internal::ParsePdfStyle(argv[i], style)) options.style = style;
            else {
                error = "unrecognized export option '" + std::string(argv[i]) + "'";
                return false;
            }
        }
    }
    return true;
}
void ReportExportError(std::string* deferredError, const std::string& message) {
    if (deferredError) {
        *deferredError = message;
    } else {
        PrintError(message);
    }
}
// A warning, not an error: the PDF is written. One write, so batch workers do not mix lines.
void WarnMissingCharacters(const std::string& inputLabel, uint32_t count) {
    std::string message = "Warning: ";
    message += inputLabel.empty() ? std::string("the document") : inputLabel;
    message += ": ";
    message += std::to_string(count);
    message += " character(s) have no glyph in the fonts found and are shown as a base letter or '?'."
        " Install a font that has them (DejaVu Sans or Noto Sans for most alphabets, Noto Sans CJK or"
        " WenQuanYi Micro Hei for Chinese, Japanese and Korean), or set RAYOMD_FONT or RAYOMD_FALLBACK_FONT"
        " to a .ttf, .otf or .ttc file.\n";
    Print(stderr, message);
}
Path PdfNameForMarkdown(const Path& path) {
    Path out(RayoMd::File::FileNameOf(RayoMd::File::PathView(path)));
    RayoMd::File::ReplaceExtension(out, RayoMd::File::PathOfAscii(".pdf"));
    return out;
}

// `base` and then each of `names`, joined by the platform's separator.
Path Joined(Path base, std::initializer_list<const char*> names) {
    for (const char* name : names) RayoMd::File::AppendPath(base, RayoMd::File::PathOfAscii(name));
    return base;
}

// Where re-encoded images are kept between runs: %LOCALAPPDATA%\RayoMD\cache\images on Windows,
// $XDG_CACHE_HOME/rayomd/images or ~/.cache/rayomd/images elsewhere; empty, so none, without them.
const std::string& ImageCacheDirectory() {
    static const std::string directory = []() {
#ifdef _WIN32
        const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
        return base && *base ? PathToUtf8(Joined(base, { "RayoMD", "cache", "images" })) : std::string();
#else
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg && *xdg == '/') return PathToUtf8(Joined(xdg, { "rayomd", "images" }));
        const char* home = std::getenv("HOME");
        return home && *home ? PathToUtf8(Joined(home, { ".cache", "rayomd", "images" })) : std::string();
#endif
    }();
    return directory;
}

// The library's options for a document whose relative images resolve from `sourcePath`.
TinyPdf::PdfOptions PdfOptionsFor(const CliExportOptions& options, const std::string& sourcePath) {
    TinyPdf::PdfOptions pdfOptions;
    pdfOptions.style = options.style;
    pdfOptions.margin = options.margin;
    pdfOptions.pageSize = options.pageSize;
    pdfOptions.sourcePath = sourcePath;
    pdfOptions.enableUrlImages = options.enableUrlImages;
    pdfOptions.allowUnsafeLocalImages = options.allowUnsafeLocalImages;
    pdfOptions.embedSource = options.embedSource;
    if (options.imageCache) pdfOptions.imageCacheDirectory = ImageCacheDirectory();
    pdfOptions.pageNumbers = options.pageNumbers;
    pdfOptions.compress = options.compress;
    pdfOptions.fastImages = options.fastImages;
    pdfOptions.pdfa = options.pdfa;
    pdfOptions.pdfua = options.pdfua;
    pdfOptions.highlightCode = options.highlightCode;
    pdfOptions.diagrams = options.diagrams;
    pdfOptions.toc = options.toc;
    pdfOptions.tocDepth = options.tocDepth;
    pdfOptions.pageBreakBeforeLevel = options.pageBreakBeforeLevel;
    pdfOptions.numberHeadings = options.numberHeadings;
    pdfOptions.dialect = options.dialect;
    pdfOptions.footnotes = options.footnotes;
    pdfOptions.vaultRoot = options.vaultRoot;
    pdfOptions.embedsAsLinks = options.embedsAsLinks;
    pdfOptions.backlinks = options.backlinks;
    pdfOptions.blackText = options.blackText;
    pdfOptions.hideTags = options.hideTags;
    pdfOptions.theme = options.theme;
    return pdfOptions;
}

// "Error: native PDF export failed for <label> (code N)." and what the code means.
std::string ExportFailure(const TinyPdf::BuildResult& result, const std::string& inputLabel, const CliExportOptions& options) {
    std::string message = "Error: native PDF export failed";
    if (!inputLabel.empty()) message += " for " + inputLabel;
    message += " (code " + std::to_string(10 + static_cast<int>(result.error)) + ").";
    if (result.error == TinyPdf::BuildError::ThemeFontUnavailable) {
        std::string paths;
        for (const std::string* path : { &options.theme.fontPath, &options.theme.fontBoldPath, &options.theme.fontItalicPath,
                 &options.theme.fontBoldItalicPath }) {
            if (path->empty()) continue;
            if (!paths.empty()) paths += ", ";
            paths += *path;
        }
        message += " The theme's fonts cannot be read or are no TrueType/OpenType fonts: " + paths;
    }
    if (result.error == TinyPdf::BuildError::PdfaFontUnavailable) {
        message += " PDF/A embeds every font, but no TrueType/OpenType font was found; set RAYOMD_FONT to one.";
    }
    return message;
}

// `details`, when given, receives the result of a build that succeeded: pages, characters
// without a glyph, failed images.
int BuildNativePdfMarkdown(const std::string& markdown, const std::string& sourcePath,
    const Path& outputPath, const CliExportOptions& options, std::string& pdfBuffer,
    const std::string& inputLabel, std::string* deferredError = nullptr, TinyPdf::BuildResult* details = nullptr) {
    pdfBuffer.clear();
    TinyPdf::BuildResult buildResult = TinyPdf::BuildPdf(markdown, PdfOptionsFor(options, sourcePath), pdfBuffer);
    if (!buildResult) {
        ReportExportError(deferredError, ExportFailure(buildResult, inputLabel, options));
        return 10 + static_cast<int>(buildResult.error);
    }
    if (buildResult.missingCharacters != 0) WarnMissingCharacters(inputLabel, buildResult.missingCharacters);
    if (details) *details = buildResult;
    if (!WriteBinaryFilePortable(outputPath, pdfBuffer)) {
        ReportExportError(deferredError, "Error: could not write PDF file: " + PathToUtf8(outputPath));
        return 12;
    }
    return 0;
}

int BuildNativePdfFile(const Path& inputPath, const Path& outputPath, const CliExportOptions& options,
    std::string& pdfBuffer, std::string* deferredError = nullptr, TinyPdf::BuildResult* details = nullptr) {
    const auto profileBefore = RayoMd::Profiling::Capture();
    std::string markdown;
    if (!ReadUtf8FilePortable(inputPath, markdown)) {
        ReportExportError(deferredError, "Error: could not read input Markdown file: " + PathToUtf8(inputPath));
        RayoMd::Profiling::EmitDelta("export", profileBefore, RayoMd::Profiling::Capture());
        return 3;
    }
    int result = BuildNativePdfMarkdown(markdown, PathToUtf8(inputPath), outputPath, options,
        pdfBuffer, PathToUtf8(inputPath), deferredError, details);
    RayoMd::Profiling::EmitDelta("export", profileBefore, RayoMd::Profiling::Capture());
    return result;
}

// A book (--book, --follow-links) as one PDF.
int WriteBookPdf(const TinyPdf::Book& book, const Path& outputPath, const CliExportOptions& options) {
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildBookPdf(book, PdfOptionsFor(options, std::string()), pdf);
    if (!result) {
        PrintError(ExportFailure(result, "the book", options));
        return 10 + static_cast<int>(result.error);
    }
    if (result.missingCharacters != 0) WarnMissingCharacters("the book", result.missingCharacters);
    if (!WriteBinaryFilePortable(outputPath, pdf)) {
        PrintError("Error: could not write PDF file: " + PathToUtf8(outputPath));
        return 12;
    }
    return 0;
}

// --book: the files of a SUMMARY.md, a folder or the command line (RayoMd::Book::ReadBook) as one PDF.
int RunBookExport(const std::vector<Path>& inputs, const Path& outputPath, const CliExportOptions& options) {
    TinyPdf::Book book;
    std::string error;
    if (!RayoMd::Book::ReadBook(inputs, book, error)) {
        PrintError("Error: " + error);
        return 3;
    }
    return WriteBookPdf(book, outputPath, options);
}

// --export with --follow-links: the note and the notes it links to (RayoMd::Book::ReadLinkedBook)
// as one PDF; past the book's limits a warning says how many notes it left out.
int RunLinkedExport(const Path& inputPath, const Path& outputPath, const CliExportOptions& options) {
    TinyPdf::Book book;
    std::string error;
    size_t leftOut = 0;
    if (!RayoMd::Book::ReadLinkedBook(inputPath, PdfOptionsFor(options, std::string()), options.followLinks, book, leftOut, error)) {
        PrintError("Error: " + error);
        return 3;
    }
    if (leftOut != 0) {
        Print(stderr, "Warning: " + std::to_string(leftOut) + " linked note(s) left out: a book of linked notes holds at most " +
            std::to_string(RayoMd::Book::kMaxLinkedNotes) + " notes of 128 MiB in all.\n");
    }
    return WriteBookPdf(book, outputPath, options);
}

int RunStdinExport(const Path& outputPath, const CliExportOptions& options) {
    std::string markdown;
    if (!ReadStdinPortable(markdown)) {
        PrintError("Error: could not read Markdown from stdin or input exceeded " + std::to_string(kMaxMarkdownInputBytes) +
            " bytes.");
        return 3;
    }

    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    return BuildNativePdfMarkdown(markdown, std::string(), outputPath, options, pdfBuffer, std::string());
}

// The size of the regular file at `path`, UINT64_MAX for none (as the standard library's filesystem's file_size).
uint64_t FileSize(const Path& path) {
    RayoMd::File::FileStatus status;
    return RayoMd::File::StatusOf(path, status) && status.type == RayoMd::File::FileType::Regular ? status.size : UINT64_MAX;
}

struct BatchJob {
    BatchJob(Path input, Path output) : inputPath(std::move(input)), outputPath(std::move(output)) {}

    Path inputPath;
    Path outputPath;
    int result = 0;
    std::string error;
    bool skipped = false;
    TinyPdf::BuildResult details;
    uint64_t bytes = 0;
    double milliseconds = 0.0;
};

RayoMd::Batch::Record RecordFor(const BatchJob& job) {
    RayoMd::Batch::Record record;
    record.input = PathToUtf8(job.inputPath);
    record.output = PathToUtf8(job.outputPath);
    record.milliseconds = job.milliseconds;
    if (job.result != 0) {
        record.status = "error";
        record.code = job.result;
        record.error = job.error.rfind("Error: ", 0) == 0 ? job.error.substr(7) : job.error;
        return record;
    }
    record.bytes = job.bytes;
    if (job.skipped) {
        record.status = "skipped";
        return record;
    }
    record.pages = job.details.pages;
    record.missingCharacters = job.details.missingCharacters;
    record.failedImages = job.details.failedImages;
    return record;
}

// Opens --report, or says why it cannot; true without a report.
bool OpenReport(RayoMd::Batch::Report& report, const CliExportOptions& options) {
    if (options.report.empty() || report.Open(options.report)) return true;
    PrintError("Error: could not create the report file: " + options.report);
    return false;
}

int ExecuteBatchJobs(std::vector<BatchJob>& jobs, const CliExportOptions& options, const char* label) {
    RayoMd::Batch::Report report;
    if (!OpenReport(report, options)) return 12;
    RayoMd::Batch::RunJobs(jobs.size(), options.workers, [&](size_t index) { return FileSize(jobs[index].inputPath); },
        [&](size_t index, std::string& pdfBuffer) {
            BatchJob& job = jobs[index];
            if (job.result == 0 && options.skipUnchanged && RayoMd::Batch::OutputUpToDate(job.inputPath, job.outputPath)) {
                job.skipped = true;
                job.bytes = FileSize(job.outputPath);
            } else if (job.result == 0) {
                const auto start = std::chrono::steady_clock::now();
                job.result = BuildNativePdfFile(job.inputPath, job.outputPath, options, pdfBuffer, &job.error, &job.details);
                job.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (job.result == 0) job.bytes = pdfBuffer.size();
            }
            if (report.IsOpen()) report.Write(RecordFor(job));
        });
    const bool reported = report.Close();

    int failures = 0;
    int lastResult = 0;
    for (const BatchJob& job : jobs) {
        if (job.result == 0) continue;
        failures++;
        lastResult = job.result;
        if (!job.error.empty()) PrintError(job.error);
    }
    if (!reported) PrintError("Error: could not write the report file: " + options.report);
    if (failures != 0) {
        PrintError("Error: " + std::string(label) + " failed for " + std::to_string(failures) + " file(s).");
        return lastResult;
    }
    return reported ? 0 : 12;
}

int RunBatchExport(const Path& inputDir, const Path& outputDir, const CliExportOptions& options) {
    std::string fsError;
    if (!IsDirectoryPortable(inputDir, fsError)) {
        PrintError("Error: input folder is not a readable directory: " + PathToUtf8(inputDir) + Reason(fsError));
        return 3;
    }
    if (!EnsureDirectoryPortable(outputDir, fsError)) {
        PrintError("Error: could not create output folder: " + PathToUtf8(outputDir) + Reason(fsError));
        return 12;
    }

    std::vector<RayoMd::Batch::File> files;
    if (!RayoMd::Batch::CollectMarkdown(inputDir, outputDir, options.recursive, files, fsError)) {
        PrintError("Error: could not read input folder: " + PathToUtf8(inputDir) + " (" + fsError + ")");
        return 3;
    }
    std::vector<BatchJob> jobs;
    jobs.reserve(files.size());
    // A subfolder's PDFs go to a subfolder of the output folder, made before the workers start, once
    // for each run of its files in the sorted list, not for every file (two stats each).
    Path madeFolder;
    for (RayoMd::Batch::File& file : files) {
        BatchJob job{ std::move(file.input), std::move(file.output) };
        const RayoMd::File::PathView folder = RayoMd::File::ParentOf(RayoMd::File::PathView(job.outputPath));
        if (options.recursive && folder != outputDir && folder != madeFolder) {
            const Path made(folder);
            if (EnsureDirectoryPortable(made, fsError)) {
                madeFolder = made;
            } else {
                job.result = 12;
                job.error = "Error: could not create output folder: " + PathToUtf8(made);
            }
        }
        jobs.push_back(std::move(job));
    }
    return ExecuteBatchJobs(jobs, options, "batch export");
}

int RunStdinBatchExport(const Path& outputDir, const CliExportOptions& options) {
    std::string fsError;
    if (!EnsureDirectoryPortable(outputDir, fsError)) {
        PrintError("Error: could not create output folder: " + PathToUtf8(outputDir) + Reason(fsError));
        return 12;
    }

    std::vector<BatchJob> jobs;
    std::set<std::string> outputs;
    std::string line;
    while (ReadLine(line)) {
        line = RayoMd::Text::Trim(line);
        if (line.empty()) continue;
        const Path inputPath = RayoMd::File::PathFromUtf8(line);
        Path outputPath = outputDir;
        RayoMd::File::AppendPath(outputPath, PdfNameForMarkdown(inputPath));
        BatchJob job{inputPath, outputPath};
        Path normal = outputPath;
        RayoMd::File::Normalize(normal);
        if (!outputs.insert(PathToUtf8(normal)).second) {
            job.result = 12;
            job.error = "Error: multiple inputs map to the same output PDF: " + PathToUtf8(outputPath);
        }
        jobs.push_back(std::move(job));
    }
    return ExecuteBatchJobs(jobs, options, "stdin batch export");
}

int RunServeExport(const Path& outputDir, const CliExportOptions& options) {
    std::string fsError;
    if (!EnsureDirectoryPortable(outputDir, fsError)) {
        PrintError("Error: could not create output folder: " + PathToUtf8(outputDir) + Reason(fsError));
        return 12;
    }

    RayoMd::Batch::Report report;
    if (!OpenReport(report, options)) return 12;
    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    int failures = 0;
    int lastResult = 0;
    std::string line;
    while (ReadLine(line)) {
        line = RayoMd::Text::Trim(line);
        if (line.empty()) continue;
        if (line == "quit" || line == "exit") break;

        BatchJob job{ RayoMd::File::PathFromUtf8(line), outputDir };
        RayoMd::File::AppendPath(job.outputPath, PdfNameForMarkdown(job.inputPath));
        auto start = std::chrono::steady_clock::now();
        job.result = BuildNativePdfFile(job.inputPath, job.outputPath, options, pdfBuffer, &job.error, &job.details);
        auto end = std::chrono::steady_clock::now();
        job.milliseconds = std::chrono::duration<double, std::milli>(end - start).count();
        if (job.result != 0) {
            failures++;
            lastResult = job.result;
            PrintError(job.error);
        } else {
            job.bytes = pdfBuffer.size();
        }
        if (report.IsOpen()) report.Write(RecordFor(job));
        Print(stdout, std::string(job.result == 0 ? "OK\t" : "ERR\t") + RayoMd::Text::FormatDouble(job.milliseconds) +
            "\t" + PathToUtf8(job.outputPath) + "\n");
    }
    if (!report.Close()) {
        PrintError("Error: could not write the report file: " + options.report);
        if (failures == 0) return 12;
    }
    return failures == 0 ? 0 : lastResult;
}

int RunNativeBench(const Path& inputPath, const Path& outputDir, int iterations, const CliExportOptions& cliOptions) {
    if (iterations < 1) iterations = 1;
    std::string fsError;
    if (!EnsureDirectoryPortable(outputDir, fsError)) {
        PrintError("Error: could not create benchmark output folder: " + PathToUtf8(outputDir) + Reason(fsError));
        return 12;
    }

    std::string markdown;
    if (!ReadUtf8FilePortable(inputPath, markdown)) {
        PrintError("Error: could not read input Markdown file: " + PathToUtf8(inputPath));
        return 3;
    }

    std::string pdfBytes;
    // A benchmark measures the work: no image is read from or kept in the persistent cache.
    TinyPdf::PdfOptions options = PdfOptionsFor(cliOptions, PathToUtf8(inputPath));
    options.imageCacheDirectory.clear();
    TinyPdf::BuildResult buildResult = TinyPdf::BuildPdf(markdown, options, pdfBytes);
    if (!buildResult) {
        int code = 10 + static_cast<int>(buildResult.error);
        PrintError("Error: native PDF benchmark export failed for " + PathToUtf8(inputPath) + " (code " +
            std::to_string(code) + ").");
        return code;
    }
    if (!WriteBinaryFilePortable(Joined(outputDir, { "sample.pdf" }), pdfBytes)) {
        PrintError("Error: could not write benchmark sample PDF in " + PathToUtf8(outputDir));
        return 12;
    }

    auto start = std::chrono::steady_clock::now();
    size_t totalBytes = 0;
    for (int i = 0; i < iterations; i++) {
        pdfBytes.clear();
        buildResult = TinyPdf::BuildPdf(markdown, options, pdfBytes);
        if (!buildResult) {
            int code = 10 + static_cast<int>(buildResult.error);
            PrintError("Error: native PDF benchmark export failed for " + PathToUtf8(inputPath) + " (code " +
                std::to_string(code) + ").");
            return code;
        }
        totalBytes += pdfBytes.size();
    }
    auto end = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
    double avgMs = totalMs / (double)iterations;

    std::string report = "input_bytes=" + std::to_string(markdown.size()) + "\n";
    report += "iterations=" + std::to_string(iterations) + "\n";
    report += "total_ms=" + RayoMd::Text::FormatDouble(totalMs) + "\n";
    report += "avg_ms=" + RayoMd::Text::FormatDouble(avgMs) + "\n";
    report += "avg_pdf_bytes=" + std::to_string(totalBytes / (size_t)iterations) + "\n";
    report += std::string("path=") + RayoMd::Text::RendererPathName(markdown) + "\n";

    if (!WriteUtf8FilePortable(Joined(outputDir, { "bench-results.txt" }), report)) {
        PrintError("Error: could not write benchmark results in " + PathToUtf8(outputDir));
        return 12;
    }
    return 0;
}

int RunDoctor() {
    std::string facts = std::string("RayoMD doctor ") + RAYOMD_VERSION + "\n";
#ifdef _WIN32
    facts += "platform=windows\n";
#else
    facts += "platform=linux\n";
#endif
#ifdef RAYOMD_USE_CURL
    facts += "curl=enabled\n";
#else
    facts += "curl=disabled\n";
#endif
#ifdef _WIN32
    facts += "platform_images=winhttp,wic\n";
#else
    facts += "platform_images=native";
#ifdef RAYOMD_USE_CURL
    facts += ",http";
#endif
    facts += "\n";
#endif
    Print(stdout, facts);

    TinyPdf::PdfOptions options;
    std::string pdfBytes;
    // Polish letters are in every Unicode text font but not in WinAnsiEncoding, so they take
    // the TrueType path; without a font they are counted as missing.
    TinyPdf::BuildResult result = TinyPdf::BuildPdf(
        "# RayoMD Doctor\n\nUnicode: Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87.\n", options, pdfBytes);
    bool fontOk = result.Ok() && result.missingCharacters == 0;
    Print(stdout, fontOk ? "unicode_font=ok\n" : "unicode_font=unavailable\n");

    const Path tempDir = RayoMd::File::TempDirectory();
    bool tempOk = !tempDir.empty();
    if (tempOk && result.Ok()) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const Path smokePath = Joined(tempDir, { ("rayomd-doctor-" + std::to_string(stamp) + ".pdf").c_str() });
        tempOk = WriteBinaryFilePortable(smokePath, pdfBytes);
        if (tempOk) {
            const uint64_t size = FileSize(smokePath);
            tempOk = size > 0 && size != UINT64_MAX;
        }
        RayoMd::File::Remove(smokePath);
    }
    Print(stdout, tempOk ? "temp_output=ok\n" : "temp_output=failed\n");
    Print(stdout, result.Ok() && tempOk ? "smoke_export=ok\n" : "smoke_export=failed\n");
    if (fontOk && tempOk) {
        Print(stdout, "status=ok\n");
        return 0;
    }
    Print(stdout, "status=failed\n");
    return 1;
}
void PrintUsage() {
    Print(stderr, std::string("RayoMD portable CLI ") + RAYOMD_VERSION + "\n"
        "Usage:\n"
        "  rayomd --version\n"
        "  rayomd --doctor\n"
        "  rayomd --inspect-source <input.pdf>\n"
        "  rayomd --recover-source <input.pdf> <output.md>\n"
        "  rayomd --export <input.md> <output.pdf> [native] [style] [margin]\n"
        "  rayomd --book <SUMMARY.md | folder | files.md...> <output.pdf> [native] [style] [margin]\n"
        "  rayomd --stdin <output.pdf> [native] [style] [margin]\n"
        "  rayomd --batch <input-folder> <output-folder> [native] [style] [margin]\n"
        "  rayomd --stdin-batch <output-folder> [native] [style] [margin]\n"
        "  rayomd --serve <output-folder> [native] [style] [margin]\n"
        "  rayomd --bench <input.md> <output-folder> <iterations> [style] [margin]\n"
        "\n"
        "Defaults: native elegant normal, URL images off, local images contained to the input directory.\n"
        "Styles: elegant, modern, tech. Margins: compact, normal, wide, margin=0.75in, margin=54pt.\n"
        "Resource flags: --allow-url-images, --allow-unsafe-local-images, --embed-source, --no-image-cache\n"
        "  (by default PNGs with transparency are re-encoded once and kept in ~/.cache/rayomd/images,\n"
        "  or $XDG_CACHE_HOME/rayomd/images: at most about 256 MB, the PDF the same either way).\n"
        "Output flags: --compress (FlateDecode streams: smaller files, slower export), --fast-images\n"
        "  (PNGs with transparency re-encoded faster, their image streams larger), --pdfa (PDF/A-3b:\n"
        "  every font embedded, the formula fonts too), --pdfua (PDF/UA-1, tagged for screen readers:\n"
        "  headings, lists, tables, figures with their alt text, links and notes; fonts as with --pdfa,\n"
        "  formulas as TeX source; the language is the front matter's lang, else en), --theme=FILE (key =\n"
        "  value lines: font, font-bold, font-italic, font-bold-italic, logo, heading-color, link-color,\n"
        "  accent-color, header-left/center/right, footer-left/center/right with {title} {author}\n"
        "  {subject} {date} {page} {pages} {logo}, cover), --no-highlight (code\n"
        "  blocks in one colour; by default fenced code in a language GitHub knows is coloured),\n"
        "  --no-diagrams (```mermaid blocks as code; by default flowcharts, sequence diagrams and pie\n"
        "  charts are drawn), --toc\n"
        "  (a table of contents, first or at a [TOC] or [[_TOC_]] paragraph, which always gets one),\n"
        "  --toc-depth=N (heading levels it lists, 1-6; default 3), --number-headings[=N] (1, 1.1, 1.1.1\n"
        "  before the headings of N levels, 1-6, default 3, and in the table of contents and bookmarks),\n"
        "  --dialect=obsidian (Obsidian's callouts, ==highlights==, %%comments%%, [[wiki links]],\n"
        "  ![[images]], ^block ids left out, and ![[note]] alone on a line printing that note, its\n"
        "  #heading section or #^id block, read from the vault; default github), --vault=DIR (the\n"
        "  folder embedded and linked notes must lie in and are found in by name, local images too;\n"
        "  default the input's folder), --embeds=link (embedded notes stay links; default content),\n"
        "  --hide-tags (with --dialect=obsidian: #tags left out), --footnotes=page (each footnote at\n"
        "  the foot of the page of its first reference; default end, after the text), --black-text\n"
        "  (for black-and-white printers: text, links and code black, fills and diagrams light grey;\n"
        "  images keep their colours).\n"
        "Page flags: --page-numbers (\"N / M\" at the foot of every page), --page-size=SIZE (a4, a3, a5,\n"
        "  letter or legal, each also with -landscape, or WIDTHxHEIGHT in mm, cm, in or pt; default a4;\n"
        "  or WIDTHxauto, such as 210mmxauto: one long page cut below its text, a new one only at page\n"
        "  breaks or past 200 in), --page-break-before=hN (a new page before each heading of level 1\n"
        "  to N; h1 for chapters).\n"
        "Batch flags: --workers=N (1-64; automatic mode uses at most 6), --recursive (subfolders,\n"
        "  mirrored), --skip-unchanged (keep PDFs newer than their Markdown), --report=FILE (one JSON\n"
        "  line per document; - for standard output).\n"
        "Books: --book makes one PDF of the files a SUMMARY.md lists (mdBook, GitBook), of a folder's\n"
        "  Markdown files by name with README.md first, or of the files given: each from a new page, the\n"
        "  links between them kept, one outline, and with --toc one table of contents for all of them.\n"
        "  --follow-links[=N] (with --export) makes a book of the note and the notes it links to, N links\n"
        "  deep (1-6, default 1; at most 1000 notes); --backlinks ends each file of a book with links to\n"
        "  the files that link to it.\n");
}

constexpr const char* kFollowLinksExportOnly = "--follow-links applies to --export only.";

int PrintArgumentError(const std::string& message) {
    Print(stderr, "Error: " + message + "\n\n");
    PrintUsage();
    return 2;
}

// Ends a one-shot export (--export, --stdin) without destructors or atexit handlers: the kernel
// takes back the 1 MB PDF buffer, the font mapping and the heap at exit anyway, and freeing them
// first costs 8 to 20 us of a 0.5 to 2 ms process. stderr is unbuffered; stdout is flushed.
[[noreturn]] void ExitOneShot(int code) {
    std::fflush(stdout);
    std::_Exit(code);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    std::string command = Lower(argv[1]);
    CliExportOptions options;

    if (command == "--version" || command == "-v") {
        Print(stdout, std::string("rayomd ") + RAYOMD_VERSION + "\n");
        return 0;
    }

    if (command == "--doctor") return RunDoctor();

    if (command == "--inspect-source") {
        if (argc != 3) return PrintArgumentError("--inspect-source requires <input.pdf>.");
        return RunInspectSource(argv[2]);
    }
    if (command == "--recover-source") {
        if (argc != 4) return PrintArgumentError("--recover-source requires <input.pdf> <output.md>.");
        return RunRecoverSource(argv[2], argv[3]);
    }

    if (command == "--stdin") {
        if (argc < 3) return PrintArgumentError("--stdin requires <output.pdf>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 3, options, error)) return PrintArgumentError(error);
        if (options.followLinks != 0) return PrintArgumentError(kFollowLinksExportOnly);
        Path outputPath(argv[2]);
        if (RayoMd::File::ExtensionOf(outputPath) != ".pdf") outputPath += ".pdf";
        ExitOneShot(RunStdinExport(outputPath, options));
    }

    if (command == "--stdin-batch") {
        if (argc < 3) return PrintArgumentError("--stdin-batch requires <output-folder>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 3, options, error)) return PrintArgumentError(error);
        if (options.followLinks != 0) return PrintArgumentError(kFollowLinksExportOnly);
        return RunStdinBatchExport(argv[2], options);
    }

    if (command == "--serve") {
        if (argc < 3) return PrintArgumentError("--serve requires <output-folder>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 3, options, error)) return PrintArgumentError(error);
        if (options.followLinks != 0) return PrintArgumentError(kFollowLinksExportOnly);
        return RunServeExport(argv[2], options);
    }

    if (command == "--bench") {
        if (argc < 5) return PrintArgumentError("--bench requires <input.md> <output-folder> <iterations>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 5, options, error)) return PrintArgumentError(error);
        if (options.followLinks != 0) return PrintArgumentError(kFollowLinksExportOnly);
        return RunNativeBench(argv[2], argv[3], std::atoi(argv[4]), options);
    }

    if (command == "--batch") {
        if (argc < 4) return PrintArgumentError("--batch requires <input-folder> <output-folder>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 4, options, error)) return PrintArgumentError(error);
        if (options.followLinks != 0) return PrintArgumentError(kFollowLinksExportOnly);
        return RunBatchExport(argv[2], argv[3], options);
    }

    if (command == "--export") {
        if (argc < 4) return PrintArgumentError("--export requires <input.md> <output.pdf>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 4, options, error)) return PrintArgumentError(error);
        Path outputPath(argv[3]);
        if (RayoMd::File::ExtensionOf(outputPath) != ".pdf") outputPath += ".pdf";
        if (options.followLinks != 0) {
            // A book: no embedded source, which a book drops with the source map.
            if (options.embedSource) return PrintArgumentError("--embed-source does not apply to --follow-links: a book is not reversible.");
            return RunLinkedExport(argv[2], outputPath, options);
        }
        std::string pdfBuffer;
        pdfBuffer.reserve(1024 * 1024);
        ExitOneShot(BuildNativePdfFile(argv[2], outputPath, options, pdfBuffer));
    }

    if (command == "--book") {
        // The inputs end at the output, the first argument that names a PDF; the options follow it.
        int output = 2;
        while (output < argc && Lower(std::string(RayoMd::File::ExtensionOf(argv[output]))) != ".pdf") output++;
        if (output == 2 || output == argc) {
            return PrintArgumentError("--book requires <SUMMARY.md | folder | files.md...> <output.pdf>.");
        }
        std::string error;
        if (!ParseExportOptions(argc, argv, output + 1, options, error)) return PrintArgumentError(error);
        if (options.followLinks != 0) return PrintArgumentError(kFollowLinksExportOnly);
        if (options.embedSource) return PrintArgumentError("--embed-source does not apply to --book: a book is not reversible.");
        return RunBookExport(std::vector<Path>(argv + 2, argv + output), argv[output], options);
    }

    return PrintArgumentError("unknown command '" + std::string(argv[1]) + "'.");
}
