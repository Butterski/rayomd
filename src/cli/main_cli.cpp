// SPDX-License-Identifier: Apache-2.0

#include "rayomd/tiny_pdf.h"
#include "../common/file_io.h"
#include "../common/text_utils.h"
#include "../common/batch_report.h"
#include "../common/profiling.h"
#include "../core/book.h"
#include "../core/export_options.h"
#include "../core/rayomd_pdf_source.h"
#if RAYOMD_SERVE_JSON
#include "serve_request.h"
#endif
#define RAYOMD_HELP_TITLE "RayoMD portable CLI"
#define RAYOMD_HELP_PROGRAM "rayomd"
#define RAYOMD_HELP_IMAGE_CACHE "$XDG_CACHE_HOME/rayomd/images, else ~/.cache/rayomd/images"
#include "help.h"
#include "messages.h"

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
#else
#include <io.h>
#endif

namespace {

using RayoMd::File::Path;
using RayoMd::File::PathToUtf8;
using RayoMd::Cli::Reason;

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

// Standard input, all of it: 0; -1 when it has more than kMaxMarkdownInputBytes; else the read's
// error (errno).
int ReadStdinPortable(std::string& content) {
    content.clear();
    char buffer[64 * 1024];
    for (;;) {
        const size_t read = std::fread(buffer, 1, sizeof(buffer), stdin);
        if (read > 0) {
            if ((long long)(content.size() + read) > kMaxMarkdownInputBytes) return -1;
            content.append(buffer, read);
        }
        if (read < sizeof(buffer)) return std::ferror(stdin) ? (errno != 0 ? errno : EIO) : 0;
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
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::ReadError("PDF", inputPath, RayoMd::PdfSource::kMaxPdfBytes, error));
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
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::ReadError("PDF", inputPath, RayoMd::PdfSource::kMaxPdfBytes, error));
        return 3;
    }
    RayoMd::PdfSource::Result result = RayoMd::PdfSource::Inspect(pdf, true);
    if (!result.Ok()) {
        PrintError(std::string("Error: ") + RayoMd::PdfSource::StatusMessage(result.status) + ".");
        return RecoveryExitCode(result.status);
    }
    bool existed = false;
    if (!WriteNewFileAtomicPortable(outputPath, result.source, existed)) {
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(existed ? "Error: destination already exists: " + PathToUtf8(outputPath)
                           : RayoMd::Cli::WriteError("recovered source", outputPath, error));
        return existed ? 34 : 12;
    }
    Print(stdout, "Recovered " + std::to_string(result.info.sourceBytes) + " bytes to " + PathToUtf8(outputPath) + "\n");
    return 0;
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
        // `--name VALUE` is `--name=VALUE` for the options that take one, and a digit after
        // --number-headings or --follow-links is their level.
        std::string joined;
        const char* argument = argv[i];
        if (i + 1 < argc && (RayoMd::Cli::TakesValue(argument) || RayoMd::Cli::TakesLevel(argument, argv[i + 1]))) {
            joined = std::string(argument) + "=" + argv[i + 1];
            argument = joined.c_str();
            i++;
        }
        std::string value = Lower(argument);
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
            options.vaultRoot = argument + 8;
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
            const Path themePath = RayoMd::File::PathFromUtf8(argument + 8);
            std::string text;
            std::string themeError;
            if (themePath.empty()) {
                error = "--theme needs a file";
                return false;
            }
            if (!ReadUtf8FilePortable(themePath, text)) {
                const int readError = RayoMd::Cli::LastSystemError();
                error = "--theme: cannot read " + PathToUtf8(themePath) +
                    Reason(RayoMd::Cli::ReadFailure(themePath, (uint64_t)kMaxMarkdownInputBytes, readError));
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
            options.report = argument + 9;
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
            if (TinyPdf::Internal::ParsePdfMargin(argument, margin)) options.margin = margin;
            else if (TinyPdf::Internal::ParsePdfStyle(argument, style)) options.style = style;
            else {
                error = "unrecognized export option '" + std::string(argument) + "'." + RayoMd::Cli::UnknownOptionHint(argument);
                return false;
            }
        }
    }
    return true;
}

// Whether an argument where --export's output may stand is an option (a flag, a style, a margin)
// and so no output; "-" is standard output.
bool IsOptionWord(const char* argument) {
    if (argument[0] == '-') return argument[1] != '\0';
    const std::string word = Lower(argument);
    TinyPdf::PdfMargin margin;
    TinyPdf::PdfStyle style;
    return word == "native" || word == "pandoc" || TinyPdf::Internal::ParsePdfMargin(argument, margin) ||
        TinyPdf::Internal::ParsePdfStyle(argument, style);
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
    Print(stderr, RayoMd::Cli::MissingCharactersWarning(inputLabel, count));
}
Path PdfNameForMarkdown(const Path& path) {
    Path out(RayoMd::File::FileNameOf(RayoMd::File::PathView(path)));
    RayoMd::File::ReplaceExtension(out, RayoMd::File::PathOfAscii(".pdf"));
    return out;
}

// Whether `path` names a PDF: its extension is .pdf in any letter case.
bool NamesPdf(const Path& path) {
    return Lower(std::string(RayoMd::File::ExtensionOf(path))) == ".pdf";
}

// "-": --export and --stdin write the PDF to standard output.
bool IsStandardOutput(const Path& path) {
    return path.size() == 1 && path[0] == '-';
}

// Whether `path` names a folder for a PDF to go in: it ends in a separator, or a folder is there.
bool NamesFolder(const Path& path) {
    return !path.empty() && (path.back() == '/' || RayoMd::File::IsDirectory(path));
}

// Where --export puts the PDF of `input`: `named` as given, with .pdf added unless it names a PDF;
// "-", standard output; a folder, the input's name with .pdf in it; no name (null or empty), that
// name beside the input. Empty, `error` said, when it would replace the input, or for "-" while
// standard output is a terminal.
Path ExportOutputPath(const Path& input, const char* named, std::string& error) {
    Path output(named != nullptr ? named : "");
    if (IsStandardOutput(output)) {
        if (!::isatty(STDOUT_FILENO)) return output;
        error = RayoMd::Cli::kTerminalOutput;
        return Path();
    }
    if (output.empty()) {
        output = input;
        RayoMd::File::ReplaceExtension(output, ".pdf");
    } else if (NamesFolder(output)) {
        RayoMd::File::AppendPath(output, PdfNameForMarkdown(input));
    } else if (!NamesPdf(output)) {
        output += ".pdf";
    }
    Path absoluteOutput;
    Path absoluteInput;
    if (RayoMd::File::Absolute(output, absoluteOutput) && RayoMd::File::Absolute(input, absoluteInput)) {
        RayoMd::File::Normalize(absoluteOutput);
        RayoMd::File::Normalize(absoluteInput);
        if (absoluteOutput == absoluteInput) {
            error = "the PDF would replace the input " + PathToUtf8(input) + ": name the output.";
            return Path();
        }
    }
    return output;
}

// The PDF to `path`, or to standard output for "-"; false, errno set, when it cannot be written.
bool WritePdf(const Path& path, const std::string& pdf) {
    if (!IsStandardOutput(path)) return WriteBinaryFilePortable(path, pdf);
    return std::fwrite(pdf.data(), 1, pdf.size(), stdout) == pdf.size() && std::fflush(stdout) == 0;
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

// `details`, when given, receives the result of a build that succeeded: pages, characters
// without a glyph, failed images.
int BuildNativePdfMarkdown(const std::string& markdown, const std::string& sourcePath,
    const Path& outputPath, const CliExportOptions& options, std::string& pdfBuffer,
    const std::string& inputLabel, std::string* deferredError = nullptr, TinyPdf::BuildResult* details = nullptr) {
    pdfBuffer.clear();
    TinyPdf::BuildResult buildResult = TinyPdf::BuildPdf(markdown, PdfOptionsFor(options, sourcePath), pdfBuffer);
    if (!buildResult) {
        ReportExportError(deferredError, RayoMd::Cli::ExportFailure(buildResult, inputLabel, options.theme));
        return 10 + static_cast<int>(buildResult.error);
    }
    if (buildResult.missingCharacters != 0) WarnMissingCharacters(inputLabel, buildResult.missingCharacters);
    if (details) *details = buildResult;
    if (!WritePdf(outputPath, pdfBuffer)) {
        const int error = RayoMd::Cli::LastSystemError();
        ReportExportError(deferredError, RayoMd::Cli::WriteError("PDF file", outputPath, error));
        return 12;
    }
    return 0;
}

int BuildNativePdfFile(const Path& inputPath, const Path& outputPath, const CliExportOptions& options,
    std::string& pdfBuffer, std::string* deferredError = nullptr, TinyPdf::BuildResult* details = nullptr) {
    const auto profileBefore = RayoMd::Profiling::Capture();
    std::string markdown;
    int result = 3;
    if (!ReadUtf8FilePortable(inputPath, markdown)) {
        const int error = RayoMd::Cli::LastSystemError();
        ReportExportError(deferredError,
            RayoMd::Cli::ReadError("input Markdown file", inputPath, (uint64_t)kMaxMarkdownInputBytes, error));
    } else if (RayoMd::Cli::IsPdf(markdown)) {
        ReportExportError(deferredError, RayoMd::Cli::PdfGivenAsMarkdown(PathToUtf8(inputPath)));
    } else {
        result = BuildNativePdfMarkdown(markdown, PathToUtf8(inputPath), outputPath, options,
            pdfBuffer, PathToUtf8(inputPath), deferredError, details);
    }
    RayoMd::Profiling::EmitDelta("export", profileBefore, RayoMd::Profiling::Capture());
    return result;
}

// A book (--book, --follow-links) as one PDF.
int WriteBookPdf(const TinyPdf::Book& book, const Path& outputPath, const CliExportOptions& options) {
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildBookPdf(book, PdfOptionsFor(options, std::string()), pdf);
    if (!result) {
        PrintError(RayoMd::Cli::ExportFailure(result, "the book", options.theme));
        return 10 + static_cast<int>(result.error);
    }
    if (result.missingCharacters != 0) WarnMissingCharacters("the book", result.missingCharacters);
    if (!WritePdf(outputPath, pdf)) {
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::WriteError("PDF file", outputPath, error));
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
    const int read = ReadStdinPortable(markdown);
    if (read != 0) {
        PrintError(read < 0 ? "Error: Markdown on standard input is larger than " +
                                  std::to_string(kMaxMarkdownInputBytes >> 20) + " MiB"
                            : "Error: could not read standard input" + Reason(RayoMd::Cli::SystemErrorText(read)));
        return 3;
    }
    if (RayoMd::Cli::IsPdf(markdown)) {
        PrintError(RayoMd::Cli::PdfGivenAsMarkdown(std::string()));
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
    const int error = RayoMd::Cli::LastSystemError();
    PrintError(RayoMd::Cli::WriteError("the report file", RayoMd::File::PathFromUtf8(options.report), error));
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
    std::string fsError = RayoMd::Cli::NotAFolder(inputDir);
    if (!fsError.empty()) {
        PrintError("Error: input folder is not a readable directory: " + PathToUtf8(inputDir) + Reason(fsError));
        return 3;
    }
    fsError = RayoMd::Cli::MakeFolder(outputDir);
    if (!fsError.empty()) {
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
            if (RayoMd::Cli::MakeFolder(made).empty()) {
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
    const std::string fsError = RayoMd::Cli::MakeFolder(outputDir);
    if (!fsError.empty()) {
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
    const std::string fsError = RayoMd::Cli::MakeFolder(outputDir);
    if (!fsError.empty()) {
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

#if RAYOMD_SERVE_JSON
// What standard input has, up to `size` bytes; 0 at its end or on an error. A read(2), not
// fread, which would wait for a full buffer while a client waits for its answer.
size_t ReadStandardInput(void*, char* buffer, size_t size) {
#ifdef _WIN32
    const int got = _read(0, buffer, static_cast<unsigned>(size));
    return got > 0 ? static_cast<size_t>(got) : 0;
#else
    for (;;) {
        const ssize_t got = ::read(STDIN_FILENO, buffer, size);
        if (got >= 0) return static_cast<size_t>(got);
        if (errno != EINTR) return 0;
    }
#endif
}

// One request of --serve --json into `record`: its options are the session's with the request's
// `args`, and its PDF the bytes --export writes with them.
void ServeJsonRequest(const RayoMd::Serve::Request& request, const CliExportOptions& sessionOptions,
    std::string& pdfBuffer, RayoMd::Batch::Record& record) {
    Path outputPath = RayoMd::File::PathFromUtf8(request.output);
    if (!NamesPdf(outputPath)) outputPath += ".pdf";
    record.output = PathToUtf8(outputPath);
    record.input = request.hasInput ? request.input : request.sourcePath;
    const auto fail = [&](int code, std::string error) {
        record.status = "error";
        record.code = code;
        record.error = error.rfind("Error: ", 0) == 0 ? error.substr(7) : std::move(error);
    };
    CliExportOptions options = sessionOptions;
    std::vector<char*> argv;
    argv.reserve(request.args.size());
    for (const std::string& arg : request.args) {
        if (RayoMd::Serve::IsBatchFlag(arg)) return fail(2, arg + " does not apply to a serve request");
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    std::string error;
    if (!ParseExportOptions(static_cast<int>(argv.size()), argv.data(), 0, options, error)) return fail(2, error);

    TinyPdf::BuildResult details;
    const auto start = std::chrono::steady_clock::now();
    const int code = request.hasInput
        ? BuildNativePdfFile(RayoMd::File::PathFromUtf8(request.input), outputPath, options, pdfBuffer, &error, &details)
        : BuildNativePdfMarkdown(request.markdown, request.sourcePath, outputPath, options, pdfBuffer, request.sourcePath,
              &error, &details);
    record.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (code != 0) return fail(code, error);
    record.pages = details.pages;
    record.bytes = pdfBuffer.size();
    record.missingCharacters = details.missingCharacters;
    record.failedImages = details.failedImages;
}

// --serve --json: protocol 1, one JSON request a line and one report record a request, in order
// (docs: the wiki's CLI reference). Standard output carries the protocol only.
int RunServeJson(const CliExportOptions& options) {
    std::string answer = std::string("{\"rayomd\":\"") + RAYOMD_VERSION + "\",\"protocol\":" +
        std::to_string(RayoMd::Serve::kProtocol) + "}\n";
    bool written = std::fwrite(answer.data(), 1, answer.size(), stdout) == answer.size() && std::fflush(stdout) == 0;

    RayoMd::Serve::LineReader lines(ReadStandardInput, nullptr);
    RayoMd::Serve::Request request;
    std::string line;
    std::string error;
    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    while (written) {
        const RayoMd::Serve::LineReader::Status status = lines.Next(line, RayoMd::Serve::kMaxRequestBytes);
        if (status == RayoMd::Serve::LineReader::Status::End) break;
        RayoMd::Batch::Record record;
        if (status == RayoMd::Serve::LineReader::Status::TooLong) {
            record.status = "error";
            record.code = 2;
            record.error = "request line exceeds " + std::to_string(RayoMd::Serve::kMaxRequestBytes) + " bytes";
        } else {
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            if (line.size() < 16) {
                const std::string word = RayoMd::Text::Trim(line);
                if (word == "quit" || word == "exit") break;
            }
            if (RayoMd::Serve::ParseRequest(line, request, error)) {
                record.id = request.id;
                ServeJsonRequest(request, options, pdfBuffer, record);
            } else {
                record.id = request.id;
                record.status = "error";
                record.code = 2;
                record.error = error;
            }
        }
        answer.clear();
        RayoMd::Batch::AppendRecord(answer, record);
        written = std::fwrite(answer.data(), 1, answer.size(), stdout) == answer.size() && std::fflush(stdout) == 0;
    }
    return written ? 0 : 12;
}
#endif

int RunNativeBench(const Path& inputPath, const Path& outputDir, int iterations, const CliExportOptions& cliOptions) {
    if (iterations < 1) iterations = 1;
    const std::string fsError = RayoMd::Cli::MakeFolder(outputDir);
    if (!fsError.empty()) {
        PrintError("Error: could not create benchmark output folder: " + PathToUtf8(outputDir) + Reason(fsError));
        return 12;
    }

    std::string markdown;
    if (!ReadUtf8FilePortable(inputPath, markdown)) {
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::ReadError("input Markdown file", inputPath, (uint64_t)kMaxMarkdownInputBytes, error));
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
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::WriteError("benchmark sample PDF", Joined(outputDir, { "sample.pdf" }), error));
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
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::WriteError("benchmark results", Joined(outputDir, { "bench-results.txt" }), error));
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
#if RAYOMD_SERVE_JSON
    facts += "serve_json=enabled\n";
#else
    facts += "serve_json=disabled\n";
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
// The help (help.h): on standard error without arguments, on standard output for --help.
void PrintUsage(std::FILE* stream) {
    Print(stream, RayoMd::Cli::HelpText());
}

constexpr const char* kFollowLinksExportOnly = "--follow-links applies to --export only.";

// The error and a line pointing to --help, not the whole help, which would push the error off
// the screen.
int PrintArgumentError(const std::string& message) {
    Print(stderr, "Error: " + message + "\n" + RayoMd::Cli::kHelpHint);
    return 2;
}

// Ends a one-shot export (--export, --stdin) without destructors or atexit handlers: the kernel
// takes back the 1 MB PDF buffer, the font mapping and the heap at exit anyway, and freeing them
// first costs 8 to 20 us of a 0.5 to 2 ms process. stderr is unbuffered; stdout is flushed.
[[noreturn]] void ExitOneShot(int code) {
    std::fflush(stdout);
    std::_Exit(code);
}

// What the arguments ask before a command runs: --help and -h anywhere print the help, --version
// the version, as GNU's standards ask (-v only in the command's place, where it cannot be meant
// as "verbose"); true then, for exit code 0. Options before the command are moved after its
// paths: `rayomd --toc --export a.md` is `rayomd --export a.md --toc`.
bool AnsweredBeforeCommand(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const std::string word = Lower(argv[i]);
        if (word == "--help" || word == "-h") {
            PrintUsage(stdout);
            return true;
        }
        if (word == "--version" || (i == 1 && word == "-v")) {
            Print(stdout, std::string("rayomd ") + RAYOMD_VERSION + "\n");
            return true;
        }
    }
    if (RayoMd::Cli::IsCommand(argv[1])) return false;
    int at = 1;
    while (at < argc && !RayoMd::Cli::IsCommand(argv[at]) &&
           (IsOptionWord(argv[at]) || (at > 1 && RayoMd::Cli::TakesValue(argv[at - 1])))) {
        at++;
    }
    if (at == argc || !RayoMd::Cli::IsCommand(argv[at])) return false;
    std::rotate(argv + 1, argv + at, argv + argc);
    if (Lower(argv[1]) != "-v") return false;
    Print(stdout, std::string("rayomd ") + RAYOMD_VERSION + "\n");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(stderr);
        return 2;
    }

    if (AnsweredBeforeCommand(argc, argv)) return 0;

    std::string command = Lower(argv[1]);
    CliExportOptions options;

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
        if (argc < 3 || argv[2][0] == '\0' || IsOptionWord(argv[2])) {
            return PrintArgumentError("--stdin requires <output.pdf>, or - for standard output.");
        }
        std::string error;
        if (!ParseExportOptions(argc, argv, 3, options, error)) return PrintArgumentError(error);
        if (options.followLinks != 0) return PrintArgumentError(kFollowLinksExportOnly);
        Path outputPath(argv[2]);
        if (IsStandardOutput(outputPath)) {
            if (::isatty(STDOUT_FILENO)) return PrintArgumentError(RayoMd::Cli::kTerminalOutput);
        } else if (NamesFolder(outputPath)) {
            return PrintArgumentError("--stdin needs a file name for the PDF: " + std::string(argv[2]) + " is a folder.");
        } else if (!NamesPdf(outputPath)) {
            outputPath += ".pdf";
        }
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
        if (argc >= 3 && Lower(argv[2]) == "--json") {
#if RAYOMD_SERVE_JSON
            std::string error;
            if (!ParseExportOptions(argc, argv, 3, options, error)) return PrintArgumentError(error);
            if (options.followLinks != 0 || !options.report.empty() || options.workers != 0 || options.recursive ||
                options.skipUnchanged) {
                return PrintArgumentError("--serve --json takes no batch flags (--report, --workers, --recursive, "
                    "--skip-unchanged) and no --follow-links: its answers are the report.");
            }
            return RunServeJson(options);
#else
            PrintError("Error: this build has no --serve --json (built with RAYOMD_SERVE_JSON=OFF).");
            return 2;
#endif
        }
        if (argc < 3) return PrintArgumentError("--serve requires <output-folder>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 3, options, error)) return PrintArgumentError(error);
        if (options.followLinks != 0) return PrintArgumentError(kFollowLinksExportOnly);
        return RunServeExport(argv[2], options);
    }

    if (command == "--bench") {
        if (argc < 5) return PrintArgumentError("--bench requires <input.md> <output-folder> <iterations>.");
        char* end = nullptr;
        const long iterations = std::strtol(argv[4], &end, 10);
        if (end == argv[4] || *end != '\0' || iterations < 1 || iterations > 100000000) {
            return PrintArgumentError("--bench takes 1 to 100000000 iterations, not '" + std::string(argv[4]) + "'.");
        }
        std::string error;
        if (!ParseExportOptions(argc, argv, 5, options, error)) return PrintArgumentError(error);
        if (options.followLinks != 0) return PrintArgumentError(kFollowLinksExportOnly);
        return RunNativeBench(argv[2], argv[3], static_cast<int>(iterations), options);
    }

    if (command == "--batch") {
        if (argc < 4) return PrintArgumentError("--batch requires <input-folder> <output-folder>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 4, options, error)) return PrintArgumentError(error);
        if (options.followLinks != 0) return PrintArgumentError(kFollowLinksExportOnly);
        return RunBatchExport(argv[2], argv[3], options);
    }

    if (command == "--export") {
        if (argc < 3) return PrintArgumentError("--export requires <input.md> [output.pdf].");
        // The output may be left out, nothing or an option in its place: the PDF goes beside the input.
        const bool named = argc > 3 && !IsOptionWord(argv[3]);
        std::string error;
        if (!ParseExportOptions(argc, argv, named ? 4 : 3, options, error)) return PrintArgumentError(error);
        const Path outputPath = ExportOutputPath(argv[2], named ? argv[3] : nullptr, error);
        if (outputPath.empty()) return PrintArgumentError(error);
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

    return PrintArgumentError("unknown command '" + std::string(argv[1]) + "'." + RayoMd::Cli::UnknownCommandHint(argv[1]));
}
