// RayoMD Windows command-line tool: the modes of src/cli/main_cli.cpp with wide paths.
// SPDX-License-Identifier: Apache-2.0

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include "../core/rayomd_pdf_source.h"
#endif
#include <algorithm>
#include <string>
#include <vector>
#include <atomic>
#include <cwctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string_view>
#include <utility>
#include <chrono>

#ifndef RAYOMD_VERSION
#define RAYOMD_VERSION "0.0.0"
#endif

#ifdef _WIN32
#include "rayomd/tiny_pdf.h"
#include "../common/text_utils.h"
#include "../common/batch_report.h"
#include "../common/path.h"
#include "../common/profiling.h"
#include "../core/book.h"
#include "../core/export_options.h"
#if RAYOMD_SERVE_JSON
#include "../cli/serve_request.h"
#endif
#define RAYOMD_HELP_TITLE "RayoMD Windows CLI"
#define RAYOMD_HELP_PROGRAM "rayomd.exe"
#define RAYOMD_HELP_IMAGE_CACHE "%LOCALAPPDATA%\\RayoMD\\cache\\images"
#include "../cli/help.h"
#include "../cli/messages.h"
#endif

constexpr long long kMaxMarkdownInputBytes = 128LL * 1024LL * 1024LL;

struct WinExportOptions {
    TinyPdf::PdfStyle style = TinyPdf::PdfStyle::Elegant;
    TinyPdf::PdfMargin margin = TinyPdf::PdfMargin::Normal();
    TinyPdf::PdfPageSize pageSize;
    bool enableUrlImages = false;
    bool allowUnsafeLocalImages = false;
    unsigned workers = 0;
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
    std::string vaultRoot;       // --vault=DIR, UTF-8
    bool embedsAsLinks = false;  // --embeds=link
    bool backlinks = false;      // --backlinks
    int followLinks = 0;         // --follow-links[=N]: 0 for none
    bool blackText = false;
    bool hideTags = false;
    TinyPdf::PdfTheme theme;
    // Batch modes: subfolders too, mirrored under the output folder; documents whose PDF is
    // newer than their Markdown left as they are; a JSON Lines report ("-" for stdout).
    bool recursive = false;
    bool skipUnchanged = false;
    std::string report;
};

// ============================================================================
// Utility Functions
// ============================================================================
std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    wstr.resize(len - 1);
    return wstr;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    str.resize(len - 1);
    return str;
}

// `text` on standard error in one write, so the lines of batch workers do not mix.
void WriteStderr(const std::string& text) {
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr == INVALID_HANDLE_VALUE || hErr == nullptr) return;
    DWORD written = 0;
    WriteFile(hErr, text.data(), (DWORD)text.size(), &written, nullptr);
}

void WriteStdoutLine(const std::string& line) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE || hOut == nullptr) return;
    DWORD written = 0;
    std::string output = line + "\n";
    WriteFile(hOut, output.data(), (DWORD)output.size(), &written, nullptr);
}

// `text` on standard output, all of it; false, GetLastError set, when a write failed.
bool WriteStdoutAll(const std::string& text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr) return false;
    for (size_t at = 0; at < text.size();) {
        DWORD written = 0;
        if (!WriteFile(out, text.data() + at, static_cast<DWORD>(text.size() - at), &written, nullptr) || written == 0) return false;
        at += written;
    }
    return true;
}

// `message` and a line break on standard error.
void PrintError(const std::string& message) {
    WriteStderr(message + "\n");
}

// An argument error and the line pointing to --help (help.h): exit code 2.
int PrintArgumentError(const std::string& message) {
    WriteStderr("Error: " + message + "\n" + RayoMd::Cli::kHelpHint);
    return 2;
}

// An export's error: on standard error at once, or in `deferredError` for a batch worker, whose
// errors are written once all have finished.
void ReportExportError(std::string* deferredError, const std::string& message) {
    if (deferredError) *deferredError = message;
    else PrintError(message);
}

#ifdef _WIN32
bool WriteUtf8File(const std::wstring& path, const std::string& content) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    size_t offset = 0;
    bool ok = true;
    while (offset < content.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(content.size() - offset, 1024 * 1024);
        DWORD written = 0;
        if (!WriteFile(hFile, content.data() + offset, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        offset += written;
    }
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(hFile);
    if (!ok) SetLastError(error);
    return ok;
}

// The file at `path`, at most `maximumBytes`; false when it cannot be read (GetLastError tells why,
// when the system said).
bool ReadFileLimited(const std::wstring& path, long long maximumBytes, std::string& content) {
    RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Io);
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 0 || size.QuadPart > maximumBytes) {
        const DWORD error = GetLastError();
        CloseHandle(hFile);
        SetLastError(error);
        return false;
    }

    if (size.QuadPart == 0) {
        content.clear();
        CloseHandle(hFile);
        return true;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (hMap) {
        const char* data = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (data) {
            content.assign(data, data + (size_t)size.QuadPart);
            UnmapViewOfFile(data);
            CloseHandle(hMap);
            CloseHandle(hFile);
            return true;
        }
        CloseHandle(hMap);
    }

    content.assign((size_t)size.QuadPart, '\0');
    size_t offset = 0;
    while (offset < content.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(content.size() - offset, 1024 * 1024);
        DWORD read = 0;
        if (!ReadFile(hFile, &content[0] + offset, chunk, &read, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(hFile);
            SetLastError(error);
            return false;
        }
        if (read == 0) break;
        offset += read;
    }
    content.resize(offset);

    CloseHandle(hFile);
    return true;
}
bool ReadUtf8File(const std::wstring& path, std::string& content) {
    return ReadFileLimited(path, kMaxMarkdownInputBytes, content);
}


std::wstring MakeTempSiblingPath(const std::wstring& path, const wchar_t* finalExtension = L".tmp") {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
    std::wstring filename = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (filename.empty()) filename = L"output.pdf";

    static std::atomic<unsigned long> counter{0};
    unsigned long id = counter.fetch_add(1, std::memory_order_relaxed);
    // ".rayomd-PID-TID-ID-TICKS" and the extension, digits by hand: swprintf links MinGW's printf.
    std::wstring name = dir + L"." + filename + L".rayomd";
    for (unsigned long long value : { (unsigned long long)GetCurrentProcessId(),
             (unsigned long long)GetCurrentThreadId(), (unsigned long long)id,
             (unsigned long long)GetTickCount64() }) {
        wchar_t digits[20];
        size_t count = 0;
        do {
            digits[count++] = static_cast<wchar_t>(L'0' + value % 10);
            value /= 10;
        } while (value != 0);
        name += L'-';
        while (count != 0) name += digits[--count];
    }
    name += finalExtension ? finalExtension : L".tmp";
    return name;
}

bool WriteBinaryTempFile(const std::wstring& tempPath, const std::string& content) {
    HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    size_t offset = 0;
    bool ok = true;
    while (offset < content.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(content.size() - offset, 1024 * 1024);
        DWORD written = 0;
        if (!WriteFile(hFile, content.data() + offset, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        offset += written;
    }

    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(hFile);
    if (!ok) {
        DeleteFileW(tempPath.c_str());
        SetLastError(error);
    }
    return ok;
}

// Moves the written temporary file over `finalPath`; false, the move's error kept for the caller
// (GetLastError), when it cannot.
bool ReplaceWithTempFile(const std::wstring& tempPath, const std::wstring& finalPath) {
    if (MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    const DWORD error = GetLastError();
    DeleteFileW(tempPath.c_str());
    SetLastError(error);
    return false;
}

bool WriteBinaryFile(const std::wstring& path, const std::string& content) {
    RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Io);
    for (int attempt = 0; attempt < 16; attempt++) {
        std::wstring tempPath = MakeTempSiblingPath(path);
        if (WriteBinaryTempFile(tempPath, content)) {
            return ReplaceWithTempFile(tempPath, path);
        }
        if (GetLastError() != ERROR_FILE_EXISTS) return false;
    }
    return false;
}
bool WriteNewBinaryFile(const std::wstring& path, const std::string& content, bool& existed) {
    existed = false;
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        existed = true;
        return false;
    }
    for (int attempt = 0; attempt < 16; attempt++) {
        std::wstring temporary = MakeTempSiblingPath(path);
        if (!WriteBinaryTempFile(temporary, content)) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return false;
        }
        if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        SetLastError(error);
        existed = error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS;
        return false;
    }
    return false;
}

#endif

// Where re-encoded images are kept between runs: %LOCALAPPDATA%\RayoMD\cache\images, or none
// without that variable.
const std::string& ImageCacheDirectory() {
    static const std::string directory = []() {
        const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
        return base && *base ? WideToUtf8(std::wstring(base) + L"\\RayoMD\\cache\\images") : std::string();
    }();
    return directory;
}

// The library's options for a document whose relative images resolve from `sourcePath`.
TinyPdf::PdfOptions PdfOptionsFor(const WinExportOptions& exportOptions, const std::wstring& sourcePath) {
    TinyPdf::PdfOptions options;
    options.style = exportOptions.style;
    options.margin = exportOptions.margin;
    options.pageSize = exportOptions.pageSize;
    options.sourcePath = WideToUtf8(sourcePath);
    options.enableUrlImages = exportOptions.enableUrlImages;
    options.allowUnsafeLocalImages = exportOptions.allowUnsafeLocalImages;
    options.embedSource = exportOptions.embedSource;
    if (exportOptions.imageCache) options.imageCacheDirectory = ImageCacheDirectory();
    options.pageNumbers = exportOptions.pageNumbers;
    options.compress = exportOptions.compress;
    options.fastImages = exportOptions.fastImages;
    options.pdfa = exportOptions.pdfa;
    options.pdfua = exportOptions.pdfua;
    options.highlightCode = exportOptions.highlightCode;
    options.diagrams = exportOptions.diagrams;
    options.toc = exportOptions.toc;
    options.tocDepth = exportOptions.tocDepth;
    options.pageBreakBeforeLevel = exportOptions.pageBreakBeforeLevel;
    options.numberHeadings = exportOptions.numberHeadings;
    options.dialect = exportOptions.dialect;
    options.footnotes = exportOptions.footnotes;
    options.vaultRoot = exportOptions.vaultRoot;
    options.embedsAsLinks = exportOptions.embedsAsLinks;
    options.backlinks = exportOptions.backlinks;
    options.blackText = exportOptions.blackText;
    options.hideTags = exportOptions.hideTags;
    options.theme = exportOptions.theme;
    return options;
}

// "-": --export and --stdin write the PDF to standard output.
bool IsStandardOutput(const std::wstring& path) {
    return path == L"-";
}

// The PDF to `path`, or to standard output for "-"; false, GetLastError set, when it cannot be.
bool WritePdf(const std::wstring& path, const std::string& pdf) {
    return IsStandardOutput(path) ? WriteStdoutAll(pdf) : WriteBinaryFile(path, pdf);
}

// Builds `markdown` into `outputPath`: 0, or the exit code with its error (ReportExportError).
// `details`, when given, receives the result of a build that succeeded: pages, characters without
// a glyph, failed images.
int ExportMarkdown(const std::string& markdown, const std::wstring& sourcePath, const std::wstring& outputPath,
    const WinExportOptions& options, std::string& pdfBuffer, const std::string& inputLabel,
    std::string* deferredError = nullptr, TinyPdf::BuildResult* details = nullptr) {
    pdfBuffer.clear();
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf(markdown, PdfOptionsFor(options, sourcePath), pdfBuffer);
    if (!result) {
        ReportExportError(deferredError, RayoMd::Cli::ExportFailure(result, inputLabel, options.theme));
        return 10 + static_cast<int>(result.error);
    }
    if (result.missingCharacters != 0) {
        WriteStderr(RayoMd::Cli::MissingCharactersWarning(inputLabel, result.missingCharacters));
    }
    if (details) *details = result;
    if (!WritePdf(outputPath, pdfBuffer)) {
        const int error = RayoMd::Cli::LastSystemError();
        ReportExportError(deferredError, RayoMd::Cli::WriteError("PDF file", outputPath, error));
        return 12;
    }
    return 0;
}

// The Markdown file `inputPath` read and built into `outputPath`, as ExportMarkdown.
int ExportFile(const std::wstring& inputPath, const std::wstring& outputPath, const WinExportOptions& options,
    std::string& pdfBuffer, std::string* deferredError = nullptr, TinyPdf::BuildResult* details = nullptr) {
    const auto profileBefore = RayoMd::Profiling::Capture();
    std::string markdown;
    int result = 3;
    if (!ReadUtf8File(inputPath, markdown)) {
        const int error = RayoMd::Cli::LastSystemError();
        ReportExportError(deferredError,
            RayoMd::Cli::ReadError("input Markdown file", inputPath, (uint64_t)kMaxMarkdownInputBytes, error));
    } else if (RayoMd::Cli::IsPdf(markdown)) {
        ReportExportError(deferredError, RayoMd::Cli::PdfGivenAsMarkdown(WideToUtf8(inputPath)));
    } else {
        result = ExportMarkdown(markdown, inputPath, outputPath, options, pdfBuffer, WideToUtf8(inputPath),
            deferredError, details);
    }
    RayoMd::Profiling::EmitDelta("export", profileBefore, RayoMd::Profiling::Capture());
    return result;
}

bool ParseExportOptions(int argc, LPWSTR* argv, int start, WinExportOptions& options, std::wstring& error) {
    for (int i = start; i < argc; i++) {
        // `--name VALUE` is `--name=VALUE` for the options that take one, and a digit after
        // --number-headings or --follow-links is their level.
        std::wstring joined;
        const wchar_t* arg = argv[i];
        if (i + 1 < argc && (RayoMd::Cli::TakesValue(WideToUtf8(arg)) || RayoMd::Cli::TakesLevel(WideToUtf8(arg), WideToUtf8(argv[i + 1])))) {
            joined = std::wstring(arg) + L"=" + argv[i + 1];
            arg = joined.c_str();
            i++;
        }
        if (lstrcmpiW(arg, L"pandoc") == 0) {
            error = L"the pandoc engine was removed; RayoMD exports with its native renderer only";
            return false;
        } else if (lstrcmpiW(arg, L"native") == 0) {
            // The only engine, still accepted from older command lines.
        } else if (lstrcmpiW(arg, L"--allow-url-images") == 0) {
            options.enableUrlImages = true;
        } else if (lstrcmpiW(arg, L"--allow-unsafe-local-images") == 0 || lstrcmpiW(arg, L"--unsafe-local-images") == 0) {
            options.allowUnsafeLocalImages = true;
        } else if (lstrcmpiW(arg, L"--embed-source") == 0) {
            options.embedSource = true;
        } else if (lstrcmpiW(arg, L"--no-image-cache") == 0) {
            options.imageCache = false;
        } else if (lstrcmpiW(arg, L"--page-numbers") == 0) {
            options.pageNumbers = true;
        } else if (lstrcmpiW(arg, L"--compress") == 0) {
            options.compress = true;
        } else if (lstrcmpiW(arg, L"--fast-images") == 0) {
            options.fastImages = true;
        } else if (lstrcmpiW(arg, L"--pdfa") == 0) {
            options.pdfa = true;
        } else if (lstrcmpiW(arg, L"--pdfua") == 0) {
            options.pdfua = true;
        } else if (lstrcmpiW(arg, L"--no-highlight") == 0) {
            options.highlightCode = false;
        } else if (lstrcmpiW(arg, L"--no-diagrams") == 0) {
            options.diagrams = false;
        } else if (lstrcmpiW(arg, L"--black-text") == 0) {
            options.blackText = true;
        } else if (lstrcmpiW(arg, L"--hide-tags") == 0) {
            options.hideTags = true;
        } else if (lstrcmpiW(arg, L"--toc") == 0) {
            options.toc = true;
        } else if (_wcsnicmp(arg, L"--toc-depth=", 12) == 0) {
            const wchar_t* depth = arg + 12;
            if (depth[0] < L'1' || depth[0] > L'6' || depth[1] != L'\0') {
                error = L"--toc-depth must be 1 to 6";
                return false;
            }
            options.tocDepth = depth[0] - L'0';
        } else if (_wcsnicmp(arg, L"--page-break-before=", 20) == 0) {
            const wchar_t* level = arg + 20;
            if ((level[0] != L'h' && level[0] != L'H') || level[1] < L'1' || level[1] > L'6' || level[2] != L'\0') {
                error = L"--page-break-before must be h1 to h6";
                return false;
            }
            options.pageBreakBeforeLevel = level[1] - L'0';
        } else if (lstrcmpiW(arg, L"--number-headings") == 0) {
            options.numberHeadings = 3;
        } else if (_wcsnicmp(arg, L"--number-headings=", 18) == 0) {
            const wchar_t* depth = arg + 18;
            if (depth[0] < L'1' || depth[0] > L'6' || depth[1] != L'\0') {
                error = L"--number-headings must be 1 to 6";
                return false;
            }
            options.numberHeadings = depth[0] - L'0';
        } else if (lstrcmpiW(arg, L"--dialect=github") == 0) {
            options.dialect = TinyPdf::MarkdownDialect::GitHub;
        } else if (lstrcmpiW(arg, L"--dialect=obsidian") == 0) {
            options.dialect = TinyPdf::MarkdownDialect::Obsidian;
        } else if (_wcsnicmp(arg, L"--dialect=", 10) == 0) {
            error = L"--dialect must be github or obsidian";
            return false;
        } else if (lstrcmpiW(arg, L"--footnotes=end") == 0) {
            options.footnotes = TinyPdf::FootnotePlacement::End;
        } else if (lstrcmpiW(arg, L"--footnotes=page") == 0) {
            options.footnotes = TinyPdf::FootnotePlacement::Page;
        } else if (_wcsnicmp(arg, L"--footnotes=", 12) == 0) {
            error = L"--footnotes must be end or page";
            return false;
        } else if (_wcsnicmp(arg, L"--vault=", 8) == 0) {
            options.vaultRoot = WideToUtf8(arg + 8);
            if (options.vaultRoot.empty()) {
                error = L"--vault needs a folder";
                return false;
            }
        } else if (lstrcmpiW(arg, L"--embeds=link") == 0) {
            options.embedsAsLinks = true;
        } else if (lstrcmpiW(arg, L"--embeds=content") == 0) {
            options.embedsAsLinks = false;
        } else if (_wcsnicmp(arg, L"--embeds=", 9) == 0) {
            error = L"--embeds must be link or content";
            return false;
        } else if (lstrcmpiW(arg, L"--backlinks") == 0) {
            options.backlinks = true;
        } else if (lstrcmpiW(arg, L"--follow-links") == 0) {
            options.followLinks = 1;
        } else if (_wcsnicmp(arg, L"--follow-links=", 15) == 0) {
            const wchar_t* depth = arg + 15;
            if (depth[0] < L'1' || depth[0] > L'6' || depth[1] != L'\0') {
                error = L"--follow-links must be 1 to 6";
                return false;
            }
            options.followLinks = depth[0] - L'0';
        } else if (_wcsnicmp(arg, L"--theme=", 8) == 0) {
            const std::wstring themePath = arg + 8;
            const size_t slash = themePath.find_last_of(L"\\/");
            const std::wstring directory = slash == std::wstring::npos ? std::wstring() : themePath.substr(0, slash);
            std::string text;
            std::string themeError;
            if (themePath.empty()) {
                error = L"--theme needs a file";
                return false;
            }
            if (!ReadUtf8File(themePath, text)) {
                const int readError = RayoMd::Cli::LastSystemError();
                error = L"--theme: cannot read " + themePath +
                    Utf8ToWide(RayoMd::Cli::Reason(RayoMd::Cli::ReadFailure(themePath, (uint64_t)kMaxMarkdownInputBytes, readError)));
                return false;
            }
            if (!TinyPdf::Internal::ParseTheme(text, WideToUtf8(directory), options.theme, themeError)) {
                error = L"--theme " + themePath + L": " + Utf8ToWide(themeError);
                return false;
            }
        } else if (_wcsnicmp(arg, L"--page-size=", 12) == 0) {
            if (!TinyPdf::Internal::ParsePageSize(WideToUtf8(arg + 12), options.pageSize)) {
                error = L"--page-size must be a4, a3, a5, letter or legal (each also with -landscape), "
                    L"WIDTHxHEIGHT in mm, cm, in or pt, such as 210x297mm, or WIDTHxauto, such as 210mmxauto";
                return false;
            }
        } else if (lstrcmpiW(arg, L"--recursive") == 0) {
            options.recursive = true;
        } else if (lstrcmpiW(arg, L"--skip-unchanged") == 0) {
            options.skipUnchanged = true;
        } else if (_wcsnicmp(arg, L"--report=", 9) == 0) {
            options.report = WideToUtf8(arg + 9);
            if (options.report.empty()) {
                error = L"--report needs a file name, or - for standard output";
                return false;
            }
        } else if (_wcsnicmp(arg, L"--workers=", 10) == 0) {
            wchar_t* end = nullptr;
            unsigned long parsed = std::wcstoul(arg + 10, &end, 10);
            if (!end || *end != wchar_t(0) || parsed < 1 || parsed > 64) {
                error = L"--workers must be between 1 and 64";
                return false;
            }
            options.workers = static_cast<unsigned>(parsed);
        } else {
            std::string token = WideToUtf8(arg);
            TinyPdf::PdfMargin margin;
            TinyPdf::PdfStyle style;
            if (TinyPdf::Internal::ParsePdfMargin(token, margin)) options.margin = margin;
            else if (TinyPdf::Internal::ParsePdfStyle(token, style)) options.style = style;
            else {
                error = L"unrecognized export option '" + std::wstring(arg) + L"'." + Utf8ToWide(RayoMd::Cli::UnknownOptionHint(token));
                return false;
            }
        }
    }
    return true;
}
std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

// The size of the regular file at `path`, UINT64_MAX for none (as the standard library's file_size).
uint64_t FileSize(const std::wstring& path) {
    RayoMd::File::FileStatus status;
    return RayoMd::File::StatusOf(path, status) && status.type == RayoMd::File::FileType::Regular ? status.size : UINT64_MAX;
}

// A book (--book, --follow-links) as one PDF.
int WriteBookPdf(const TinyPdf::Book& book, const std::wstring& outputPath, const WinExportOptions& exportOptions) {
    std::string pdfBytes;
    const TinyPdf::BuildResult result = TinyPdf::BuildBookPdf(book, PdfOptionsFor(exportOptions, L""), pdfBytes);
    if (!result) {
        PrintError(RayoMd::Cli::ExportFailure(result, "the book", exportOptions.theme));
        return 10 + static_cast<int>(result.error);
    }
    if (result.missingCharacters != 0) {
        WriteStderr(RayoMd::Cli::MissingCharactersWarning("the book", result.missingCharacters));
    }
    if (!WritePdf(outputPath, pdfBytes)) {
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::WriteError("PDF file", outputPath, error));
        return 12;
    }
    return 0;
}

// --book: the files of a SUMMARY.md, a folder or the command line (RayoMd::Book::ReadBook) as one PDF.
int RunBookExport(const std::vector<RayoMd::File::Path>& inputs, const std::wstring& outputPath,
    const WinExportOptions& exportOptions) {
    TinyPdf::Book book;
    std::string error;
    if (!RayoMd::Book::ReadBook(inputs, book, error)) {
        PrintError("Error: " + error);
        return 3;
    }
    return WriteBookPdf(book, outputPath, exportOptions);
}

// --export with --follow-links: the note and the notes it links to (RayoMd::Book::ReadLinkedBook)
// as one PDF; past the book's limits a warning says how many notes it left out.
int RunLinkedExport(const std::wstring& inputPath, const std::wstring& outputPath, const WinExportOptions& exportOptions) {
    TinyPdf::Book book;
    std::string error;
    size_t leftOut = 0;
    if (!RayoMd::Book::ReadLinkedBook(inputPath, PdfOptionsFor(exportOptions, L""),
            exportOptions.followLinks, book, leftOut, error)) {
        PrintError("Error: " + error);
        return 3;
    }
    if (leftOut != 0) {
        WriteStderr("Warning: " + std::to_string(leftOut) + " linked note(s) left out: a book of linked notes holds at most " +
            std::to_string(RayoMd::Book::kMaxLinkedNotes) + " notes of 128 MiB in all.\n");
    }
    return WriteBookPdf(book, outputPath, exportOptions);
}

bool CanParseCommandLineLocally(LPCWSTR commandLine) {
    if (!commandLine || !*commandLine || *commandLine == L' ' || *commandLine == L'\t') {
        return false;
    }

    const wchar_t* cursor = commandLine;
    while (*cursor) {
        if (*cursor == L'"') {
            cursor++;
            if (*cursor == L'"') return false;
            while (*cursor && *cursor != L'"') {
                if (*cursor == L'\\' && cursor[1] == L'"') return false;
                cursor++;
            }
            if (*cursor != L'"') return false;
            cursor++;
            if (*cursor && *cursor != L' ' && *cursor != L'\t') return false;
        } else {
            while (*cursor && *cursor != L' ' && *cursor != L'\t') {
                if (*cursor == L'"') return false;
                cursor++;
            }
        }
        while (*cursor == L' ' || *cursor == L'\t') cursor++;
    }
    return true;
}

LPWSTR* ParseCommandLineLocally(LPCWSTR commandLine, int* argumentCount) {
    if (!argumentCount) return nullptr;
    *argumentCount = 0;

    std::vector<std::wstring> arguments;
    const wchar_t* cursor = commandLine ? commandLine : L"";
    while (*cursor) {
        while (*cursor == L' ' || *cursor == L'\t') cursor++;
        if (!*cursor) break;

        std::wstring argument;
        bool quoted = false;
        while (*cursor) {
            if (!quoted && (*cursor == L' ' || *cursor == L'\t')) break;

            size_t slashCount = 0;
            while (*cursor == L'\\') {
                slashCount++;
                cursor++;
            }
            if (*cursor == L'"') {
                argument.append(slashCount / 2, L'\\');
                if ((slashCount & 1u) != 0) {
                    argument.push_back(L'"');
                    cursor++;
                } else {
                    quoted = !quoted;
                    cursor++;
                }
                continue;
            }

            argument.append(slashCount, L'\\');
            if (!*cursor || (!quoted && (*cursor == L' ' || *cursor == L'\t'))) break;
            argument.push_back(*cursor++);
        }
        arguments.push_back(std::move(argument));
        while (*cursor == L' ' || *cursor == L'\t') cursor++;
    }

    size_t pointerBytes = (arguments.size() + 1) * sizeof(LPWSTR);
    size_t characterCount = 0;
    for (const std::wstring& argument : arguments) characterCount += argument.size() + 1;
    if (characterCount > (SIZE_MAX - pointerBytes) / sizeof(wchar_t)) return nullptr;

    size_t allocationBytes = pointerBytes + characterCount * sizeof(wchar_t);
    LPWSTR* result = static_cast<LPWSTR*>(LocalAlloc(LMEM_FIXED, allocationBytes));
    if (!result) return nullptr;
    wchar_t* destination = reinterpret_cast<wchar_t*>(
        reinterpret_cast<unsigned char*>(result) + pointerBytes);
    for (size_t index = 0; index < arguments.size(); index++) {
        result[index] = destination;
        const std::wstring& argument = arguments[index];
        std::copy(argument.begin(), argument.end(), destination);
        destination += argument.size();
        *destination++ = L'\0';
    }
    result[arguments.size()] = nullptr;
    *argumentCount = static_cast<int>(arguments.size());
    return result;
}

std::wstring PdfNameForMarkdown(const std::wstring& name) {
    size_t slash = name.find_last_of(L"\\/");
    std::wstring base = slash == std::wstring::npos ? name : name.substr(slash + 1);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);
    return base + L".pdf";
}

std::wstring OutputPathKey(const std::wstring& path) {
    DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    std::wstring key;
    if (needed > 0) {
        key.assign(needed, L'\0');
        DWORD written = GetFullPathNameW(path.c_str(), needed, &key[0], nullptr);
        if (written > 0 && written < needed) key.resize(written);
        else key = path;
    } else {
        key = path;
    }
    for (wchar_t& ch : key) {
        if (ch == L'/') ch = L'\\';
        ch = (wchar_t)std::towlower(ch);
    }
    return key;
}

// Whether `path` names a PDF: its extension is .pdf in any letter case.
bool NamesPdf(const std::wstring& path) {
    return path.size() >= 4 && _wcsicmp(path.c_str() + path.size() - 4, L".pdf") == 0;
}

// Whether `path` names a folder for a PDF to go in: it ends in a separator, or a folder is there.
bool NamesFolder(const std::wstring& path) {
    return !path.empty() && (path.back() == L'\\' || path.back() == L'/' || RayoMd::File::IsDirectory(path));
}

// Whether standard output is a console, where a PDF's bytes would only scramble the screen.
bool StandardOutputIsConsole() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    return out != INVALID_HANDLE_VALUE && out != nullptr && GetConsoleMode(out, &mode);
}

// Whether an argument where --export's output may stand is an option (a flag, a style, a margin)
// and so no output; "-" is standard output.
bool IsOptionWord(const wchar_t* argument) {
    if (argument[0] == L'-') return argument[1] != L'\0';
    if (lstrcmpiW(argument, L"native") == 0 || lstrcmpiW(argument, L"pandoc") == 0) return true;
    const std::string word = WideToUtf8(argument);
    TinyPdf::PdfMargin margin;
    TinyPdf::PdfStyle style;
    return TinyPdf::Internal::ParsePdfMargin(word, margin) || TinyPdf::Internal::ParsePdfStyle(word, style);
}

// Where --export puts the PDF of `input`: `named` as given, with .pdf added unless it names a PDF;
// "-", standard output; a folder, the input's name with .pdf in it; no name (null or empty), that
// name beside the input. Empty, `error` said, when it would replace the input, or for "-" while
// standard output is a console.
std::wstring ExportOutputPath(const std::wstring& input, const wchar_t* named, std::string& error) {
    std::wstring output = named != nullptr ? named : L"";
    if (IsStandardOutput(output)) {
        if (!StandardOutputIsConsole()) return output;
        error = RayoMd::Cli::kTerminalOutput;
        return std::wstring();
    }
    if (output.empty()) {
        output = input;
        RayoMd::File::ReplaceExtension(output, L".pdf");
    } else if (NamesFolder(output)) {
        output = JoinPath(output, PdfNameForMarkdown(input));
    } else if (!NamesPdf(output)) {
        output += L".pdf";
    }
    if (OutputPathKey(output) == OutputPathKey(input)) {
        error = "the PDF would replace the input " + WideToUtf8(input) + ": name the output.";
        return std::wstring();
    }
    return output;
}

struct WinBatchJob {
    WinBatchJob(std::wstring input, std::wstring output) : inputPath(std::move(input)), outputPath(std::move(output)) {}

    std::wstring inputPath;
    std::wstring outputPath;
    int result = 0;
    std::string error;
    bool skipped = false;
    TinyPdf::BuildResult details;
    uint64_t bytes = 0;
    double milliseconds = 0.0;
};

RayoMd::Batch::Record WinRecordFor(const WinBatchJob& job) {
    RayoMd::Batch::Record record;
    record.input = WideToUtf8(job.inputPath);
    record.output = WideToUtf8(job.outputPath);
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
bool OpenWinReport(RayoMd::Batch::Report& report, const WinExportOptions& options) {
    if (options.report.empty() || report.Open(options.report)) return true;
    const int error = RayoMd::Cli::LastSystemError();
    PrintError(RayoMd::Cli::WriteError("the report file", Utf8ToWide(options.report), error));
    return false;
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int ExecuteWinBatchJobs(std::vector<WinBatchJob>& jobs, const WinExportOptions& options, const char* label) {
    RayoMd::Batch::Report report;
    if (!OpenWinReport(report, options)) return 12;
    const auto upToDate = [&](const WinBatchJob& job) {
        return options.skipUnchanged && RayoMd::Batch::OutputUpToDate(job.inputPath, job.outputPath);
    };
    const auto skip = [](WinBatchJob& job) {
        job.skipped = true;
        job.bytes = FileSize(job.outputPath);
    };
    RayoMd::Batch::RunJobs(jobs.size(), options.workers, [&](size_t index) { return FileSize(jobs[index].inputPath); },
        [&](size_t index, std::string& pdfBuffer) {
            WinBatchJob& job = jobs[index];
            if (job.result == 0 && upToDate(job)) {
                skip(job);
            } else if (job.result == 0) {
                const auto start = std::chrono::steady_clock::now();
                job.result = ExportFile(job.inputPath, job.outputPath, options, pdfBuffer, &job.error, &job.details);
                job.milliseconds = MillisecondsSince(start);
                if (job.result == 0) job.bytes = pdfBuffer.size();
            }
            if (report.IsOpen()) report.Write(WinRecordFor(job));
        });
    const bool reported = report.Close();

    int failures = 0;
    int lastResult = 0;
    for (const WinBatchJob& job : jobs) {
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

int RunBatchExport(const std::wstring& inputDir, const std::wstring& outputDir, const WinExportOptions& options) {
    std::string fsError = RayoMd::Cli::NotAFolder(inputDir);
    if (!fsError.empty()) {
        PrintError("Error: input folder is not a readable directory: " + WideToUtf8(inputDir) + RayoMd::Cli::Reason(fsError));
        return 3;
    }
    fsError = RayoMd::Cli::MakeFolder(outputDir);
    if (!fsError.empty()) {
        PrintError("Error: could not create output folder: " + WideToUtf8(outputDir) + RayoMd::Cli::Reason(fsError));
        return 12;
    }
    std::vector<RayoMd::Batch::File> files;
    if (!RayoMd::Batch::CollectMarkdown(inputDir, outputDir, options.recursive, files, fsError)) {
        PrintError("Error: could not read input folder: " + WideToUtf8(inputDir) + " (" + fsError + ")");
        return 3;
    }

    std::vector<WinBatchJob> jobs;
    jobs.reserve(files.size());
    std::set<std::wstring> seenOutputs;
    // A subfolder's PDFs go to a subfolder of the output folder, made before the workers start, once
    // for each run of its files in the sorted list, not for every file.
    std::wstring madeFolder;
    for (RayoMd::Batch::File& file : files) {
        WinBatchJob job(std::move(file.input), std::move(file.output));
        const std::wstring_view folder = RayoMd::File::ParentOf(std::wstring_view(job.outputPath));
        if (!seenOutputs.insert(OutputPathKey(job.outputPath)).second) {
            job.result = 12;
            job.error = "Error: multiple inputs map to the same output PDF: " + WideToUtf8(job.outputPath);
        } else if (options.recursive && (madeFolder.empty() || folder != madeFolder)) {
            if (RayoMd::Cli::MakeFolder(std::wstring(folder)).empty()) {
                madeFolder.assign(folder);
            } else {
                job.result = 12;
                job.error = "Error: could not create output folder: " + WideToUtf8(std::wstring(folder));
            }
        }
        jobs.push_back(std::move(job));
    }
    return ExecuteWinBatchJobs(jobs, options, "batch export");
}

bool IsCleanStdinEof(DWORD error) {
    return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_NO_DATA;
}

// Standard input, all of it, at most `maxBytes` (0: no limit): 0; -1 when it has more; else the
// read's error (GetLastError's code).
int ReadAllStdin(std::string& input, size_t maxBytes = (size_t)kMaxMarkdownInputBytes) {
    input.clear();
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == nullptr) return ERROR_INVALID_HANDLE;

    char buf[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(hIn, buf, sizeof(buf), &read, nullptr)) {
            const DWORD error = GetLastError();
            return IsCleanStdinEof(error) ? 0 : static_cast<int>(error);
        }
        if (read == 0) return 0;
        if (maxBytes != 0 && input.size() + read > maxBytes) return -1;
        input.append(buf, buf + read);
    }
}

int RunStdinBatchExport(const std::wstring& outputDir, const WinExportOptions& options) {
    const std::string fsError = RayoMd::Cli::MakeFolder(outputDir);
    if (!fsError.empty()) {
        PrintError("Error: could not create output folder: " + WideToUtf8(outputDir) + RayoMd::Cli::Reason(fsError));
        return 12;
    }
    std::string list;
    if (const int read = ReadAllStdin(list, 0); read != 0) {
        PrintError("Error: could not read the Markdown paths from standard input" +
            RayoMd::Cli::Reason(RayoMd::Cli::SystemErrorText(read)));
        return 3;
    }

    std::vector<WinBatchJob> jobs;
    std::set<std::wstring> seenOutputs;
    for (std::string line : RayoMd::Text::SplitLines(list)) {
        line = RayoMd::Text::Trim(line);
        if (line.empty()) continue;
        std::wstring inputPath = Utf8ToWide(line);
        std::wstring outputPath = JoinPath(outputDir, PdfNameForMarkdown(inputPath));
        WinBatchJob job(inputPath, outputPath);
        if (!seenOutputs.insert(OutputPathKey(outputPath)).second) {
            job.result = 12;
            job.error = "Error: multiple inputs map to the same output PDF: " + WideToUtf8(outputPath);
        }
        jobs.push_back(std::move(job));
    }
    return ExecuteWinBatchJobs(jobs, options, "stdin batch export");
}
int RunStdinExport(const std::wstring& outputPath, const WinExportOptions& options) {
    std::string markdown;
    const int read = ReadAllStdin(markdown);
    if (read != 0) {
        PrintError(read < 0 ? "Error: Markdown on standard input is larger than " +
                                  std::to_string(kMaxMarkdownInputBytes >> 20) + " MiB"
                            : "Error: could not read standard input" + RayoMd::Cli::Reason(RayoMd::Cli::SystemErrorText(read)));
        return 3;
    }
    if (RayoMd::Cli::IsPdf(markdown)) {
        PrintError(RayoMd::Cli::PdfGivenAsMarkdown(std::string()));
        return 3;
    }
    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    return ExportMarkdown(markdown, std::wstring(), outputPath, options, pdfBuffer, std::string());
}

int RunServeExport(const std::wstring& outputDir, const WinExportOptions& options) {
    const std::string fsError = RayoMd::Cli::MakeFolder(outputDir);
    if (!fsError.empty()) {
        PrintError("Error: could not create output folder: " + WideToUtf8(outputDir) + RayoMd::Cli::Reason(fsError));
        return 12;
    }

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == nullptr) {
        PrintError("Error: could not read standard input.");
        return 3;
    }

    LARGE_INTEGER freq = {};
    QueryPerformanceFrequency(&freq);

    RayoMd::Batch::Report report;
    if (!OpenWinReport(report, options)) return 12;
    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    std::string pending;
    char buf[4096];
    DWORD read = 0;
    int failures = 0;
    int lastResult = 0;

    // One path a line: false for quit.
    auto processLine = [&](std::string line) {
        line = RayoMd::Text::Trim(line);
        if (line.empty()) return true;
        if (line == "quit" || line == "exit") return false;

        WinBatchJob job(Utf8ToWide(line), std::wstring());
        job.outputPath = JoinPath(outputDir, PdfNameForMarkdown(job.inputPath));

        LARGE_INTEGER start = {}, end = {};
        QueryPerformanceCounter(&start);
        job.result = ExportFile(job.inputPath, job.outputPath, options, pdfBuffer, &job.error, &job.details);
        QueryPerformanceCounter(&end);

        const bool ok = job.result == 0;
        double ms = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
        job.milliseconds = ms;
        if (ok) {
            job.bytes = pdfBuffer.size();
        } else {
            failures++;
            lastResult = job.result;
            PrintError(job.error);
        }
        if (report.IsOpen()) report.Write(WinRecordFor(job));
        WriteStdoutLine(std::string(ok ? "OK\t" : "ERR\t") + RayoMd::Text::FormatDouble(ms) + "\t" + WideToUtf8(job.outputPath));
        return true;
    };

    bool quit = false;
    while (!quit && ReadFile(hIn, buf, sizeof(buf), &read, nullptr) && read > 0) {
        pending.append(buf, buf + read);
        size_t pos = 0;
        while (!quit && (pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending.erase(0, pos + 1);
            quit = !processLine(line);
        }
    }
    if (!quit && !pending.empty()) processLine(pending);
    if (!report.Close()) {
        PrintError("Error: could not write the report file: " + options.report);
        if (failures == 0) return 12;
    }
    return failures == 0 ? 0 : lastResult;
}

#if RAYOMD_SERVE_JSON
// What standard input has, up to `size` bytes; 0 at its end or on an error.
size_t ReadStandardInput(void*, char* buffer, size_t size) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD read = 0;
    if (in == INVALID_HANDLE_VALUE || in == nullptr || !ReadFile(in, buffer, static_cast<DWORD>(size), &read, nullptr)) return 0;
    return read;
}

// One request of --serve --json into `record`: its options are the session's with the request's
// `args`, and its PDF the bytes --export writes with them.
void ServeJsonRequest(const RayoMd::Serve::Request& request, const WinExportOptions& sessionOptions,
    std::string& pdfBuffer, RayoMd::Batch::Record& record) {
    std::wstring outputPath = Utf8ToWide(request.output);
    if (outputPath.size() < 4 || _wcsicmp(outputPath.c_str() + outputPath.size() - 4, L".pdf") != 0) outputPath += L".pdf";
    record.output = WideToUtf8(outputPath);
    record.input = request.hasInput ? request.input : request.sourcePath;
    const auto fail = [&](int code, std::string error) {
        record.status = "error";
        record.code = code;
        record.error = error.rfind("Error: ", 0) == 0 ? error.substr(7) : std::move(error);
    };
    WinExportOptions options = sessionOptions;
    std::vector<std::wstring> arguments;
    arguments.reserve(request.args.size());
    for (const std::string& arg : request.args) {
        if (RayoMd::Serve::IsBatchFlag(arg)) return fail(2, arg + " does not apply to a serve request");
        arguments.push_back(Utf8ToWide(arg));
    }
    std::vector<LPWSTR> argv;
    argv.reserve(arguments.size());
    for (std::wstring& argument : arguments) argv.push_back(&argument[0]);
    std::wstring error;
    if (!ParseExportOptions(static_cast<int>(argv.size()), argv.data(), 0, options, error)) return fail(2, WideToUtf8(error));

    TinyPdf::BuildResult details;
    std::string exportError;
    const auto start = std::chrono::steady_clock::now();
    const int code = request.hasInput
        ? ExportFile(Utf8ToWide(request.input), outputPath, options, pdfBuffer, &exportError, &details)
        : ExportMarkdown(request.markdown, Utf8ToWide(request.sourcePath), outputPath, options, pdfBuffer,
              request.sourcePath, &exportError, &details);
    record.milliseconds = MillisecondsSince(start);
    if (code != 0) return fail(code, exportError);
    record.pages = details.pages;
    record.bytes = pdfBuffer.size();
    record.missingCharacters = details.missingCharacters;
    record.failedImages = details.failedImages;
}

// --serve --json: protocol 1, one JSON request a line and one report record a request, in order
// (docs: the wiki's CLI reference). Standard output carries the protocol only.
int RunServeJson(const WinExportOptions& options) {
    std::string answer = std::string("{\"rayomd\":\"") + RAYOMD_VERSION + "\",\"protocol\":" +
        std::to_string(RayoMd::Serve::kProtocol) + "}\n";
    bool written = WriteStdoutAll(answer);

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
        written = WriteStdoutAll(answer);
    }
    return written ? 0 : 12;
}
#endif

int RunNativeBench(const std::wstring& inputPath, const std::wstring& outputDir,
    int iterations, const WinExportOptions& exportOptions) {
    if (iterations < 1) iterations = 1;
    const std::string fsError = RayoMd::Cli::MakeFolder(outputDir);
    if (!fsError.empty()) {
        PrintError("Error: could not create benchmark output folder: " + WideToUtf8(outputDir) +
            RayoMd::Cli::Reason(fsError));
        return 12;
    }

    std::string markdown;
    if (!ReadUtf8File(inputPath, markdown)) {
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::ReadError("input Markdown file", inputPath, (uint64_t)kMaxMarkdownInputBytes, error));
        return 3;
    }

    // A benchmark measures the work: no image is read from or kept in the persistent cache.
    WinExportOptions benchOptions = exportOptions;
    benchOptions.imageCache = false;
    const TinyPdf::PdfOptions options = PdfOptionsFor(benchOptions, inputPath);
    const auto failed = [&](const TinyPdf::BuildResult& result) {
        const int code = 10 + static_cast<int>(result.error);
        PrintError("Error: native PDF benchmark export failed for " + WideToUtf8(inputPath) + " (code " +
            std::to_string(code) + ").");
        return code;
    };
    std::string pdfBytes;
    TinyPdf::BuildResult result = TinyPdf::BuildPdf(markdown, options, pdfBytes);
    if (!result) return failed(result);
    if (!WriteBinaryFile(JoinPath(outputDir, L"sample.pdf"), pdfBytes)) {
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::WriteError("benchmark sample PDF", JoinPath(outputDir, L"sample.pdf"), error));
        return 12;
    }

    LARGE_INTEGER freq = {}, start = {}, end = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    size_t totalBytes = 0;
    for (int i = 0; i < iterations; i++) {
        pdfBytes.clear();
        result = TinyPdf::BuildPdf(markdown, options, pdfBytes);
        if (!result) return failed(result);
        totalBytes += pdfBytes.size();
    }

    QueryPerformanceCounter(&end);
    double totalMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
    double avgMs = totalMs / (double)iterations;

    std::string report = "input_bytes=" + std::to_string(markdown.size()) + "\n";
    report += "iterations=" + std::to_string(iterations) + "\n";
    report += "total_ms=" + RayoMd::Text::FormatDouble(totalMs) + "\n";
    report += "avg_ms=" + RayoMd::Text::FormatDouble(avgMs) + "\n";
    report += "avg_pdf_bytes=" + std::to_string(totalBytes / (size_t)iterations) + "\n";
    report += std::string("path=") + RayoMd::Text::RendererPathName(markdown) + "\n";

    if (!WriteUtf8File(JoinPath(outputDir, L"bench-results.txt"), report)) {
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::WriteError("benchmark results", JoinPath(outputDir, L"bench-results.txt"), error));
        return 12;
    }
    return 0;
}
int RunDoctor() {
    WriteStdoutLine(std::string("RayoMD doctor ") + RAYOMD_VERSION);
    WriteStdoutLine("platform=windows");
    WriteStdoutLine("curl=disabled");
#if RAYOMD_SERVE_JSON
    WriteStdoutLine("serve_json=enabled");
#else
    WriteStdoutLine("serve_json=disabled");
#endif
    WriteStdoutLine("platform_images=winhttp,wic");

    TinyPdf::PdfOptions options;
    std::string pdfBytes;
    // Polish letters are in every Unicode text font but not in WinAnsiEncoding, so they take
    // the TrueType path; without a font they are counted as missing.
    TinyPdf::BuildResult result = TinyPdf::BuildPdf(
        "# RayoMD Doctor\n\nUnicode: Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87.\n", options, pdfBytes);
    const bool fontOk = result.Ok() && result.missingCharacters == 0;
    WriteStdoutLine(std::string("unicode_font=") + (fontOk ? "ok" : "unavailable"));

    wchar_t tempDir[MAX_PATH] = {};
    wchar_t smokePath[MAX_PATH] = {};
    DWORD tempLength = GetTempPathW(MAX_PATH, tempDir);
    bool tempOk = tempLength > 0 && tempLength < MAX_PATH &&
        GetTempFileNameW(tempDir, L"rmd", 0, smokePath) != 0;
    if (tempOk && result.Ok()) {
        tempOk = WriteUtf8File(smokePath, pdfBytes);
        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        tempOk = tempOk && GetFileAttributesExW(
            smokePath, GetFileExInfoStandard, &attributes) &&
            (attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow != 0);
    }
    if (smokePath[0] != L'\0') DeleteFileW(smokePath);
    WriteStdoutLine(std::string("temp_output=") + (tempOk ? "ok" : "failed"));
    WriteStdoutLine(std::string("smoke_export=") + (result.Ok() && tempOk ? "ok" : "failed"));
    WriteStdoutLine(std::string("status=") + (fontOk && tempOk ? "ok" : "failed"));
    return fontOk && tempOk ? 0 : 1;
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

int RunInspectSource(const std::wstring& inputPath) {
    std::string pdf;
    if (!ReadFileLimited(inputPath, static_cast<long long>(RayoMd::PdfSource::kMaxPdfBytes), pdf)) {
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(RayoMd::Cli::ReadError("PDF", inputPath, RayoMd::PdfSource::kMaxPdfBytes, error));
        return 3;
    }
    RayoMd::PdfSource::Result result = RayoMd::PdfSource::Inspect(pdf, false);
    if (!result.Ok()) {
        PrintError(std::string("Error: ") + RayoMd::PdfSource::StatusMessage(result.status) + ".");
        return RecoveryExitCode(result.status);
    }
    WriteStdoutLine("status=intact");
    WriteStdoutLine("profile=" + result.info.profile);
    WriteStdoutLine("producer_version=" + result.info.producerVersion);
    WriteStdoutLine("source_bytes=" + std::to_string(result.info.sourceBytes));
    WriteStdoutLine("encoding=" + result.info.encoding);
    WriteStdoutLine("digest=valid");
    WriteStdoutLine("attachment=" + result.info.attachmentName);
    return 0;
}

int RunRecoverSource(const std::wstring& inputPath, const std::wstring& outputPath) {
    std::string pdf;
    if (!ReadFileLimited(inputPath, static_cast<long long>(RayoMd::PdfSource::kMaxPdfBytes), pdf)) {
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
    if (!WriteNewBinaryFile(outputPath, result.source, existed)) {
        const int error = RayoMd::Cli::LastSystemError();
        PrintError(existed ? "Error: destination already exists: " + WideToUtf8(outputPath)
                           : RayoMd::Cli::WriteError("recovered source", outputPath, error));
        return existed ? 34 : 12;
    }
    WriteStdoutLine("Recovered " + std::to_string(result.info.sourceBytes) +
        " bytes to " + WideToUtf8(outputPath));
    return 0;
}

// The help (help.h): on standard error without arguments, on standard output for --help.
void PrintUsage(DWORD stream = STD_ERROR_HANDLE) {
    const HANDLE handle = GetStdHandle(stream);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;
    const std::string text = RayoMd::Cli::HelpText();
    DWORD written = 0;
    WriteFile(handle, text.data(), (DWORD)text.size(), &written, nullptr);
}

int RunCommandLine() {
    int argc = 0;
    LPCWSTR commandLine = GetCommandLineW();
    LPWSTR* argv = CanParseCommandLineLocally(commandLine)
        ? ParseCommandLineLocally(commandLine, &argc)
        : CommandLineToArgvW(commandLine, &argc);
    if (!argv) {
        WriteStderr("Error: could not read the command line.\n");
        return 2;
    }
    if (argc < 2) {
        PrintUsage();
        LocalFree(argv);
        return 2;
    }

    // --help and -h anywhere print the help, --version the version, as GNU's standards ask (-v
    // only in the command's place, where it cannot be meant as "verbose").
    for (int i = 1; i < argc; i++) {
        if (lstrcmpiW(argv[i], L"--help") == 0 || lstrcmpiW(argv[i], L"-h") == 0) {
            PrintUsage(STD_OUTPUT_HANDLE);
            LocalFree(argv);
            return 0;
        }
        if (lstrcmpiW(argv[i], L"--version") == 0 || (i == 1 && lstrcmpiW(argv[i], L"-v") == 0)) {
            WriteStdoutLine(std::string("rayomd ") + RAYOMD_VERSION);
            LocalFree(argv);
            return 0;
        }
    }
    // Options before the command go after its paths: `rayomd.exe --toc --export a.md` is
    // `rayomd.exe --export a.md --toc`. The array's pointers move; LocalFree takes it as it is.
    if (!RayoMd::Cli::IsCommand(WideToUtf8(argv[1]))) {
        int at = 1;
        while (at < argc && !RayoMd::Cli::IsCommand(WideToUtf8(argv[at])) &&
               (IsOptionWord(argv[at]) || (at > 1 && RayoMd::Cli::TakesValue(WideToUtf8(argv[at - 1]))))) {
            at++;
        }
        if (at < argc && RayoMd::Cli::IsCommand(WideToUtf8(argv[at]))) std::rotate(argv + 1, argv + at, argv + argc);
        if (lstrcmpiW(argv[1], L"-v") == 0) {
            WriteStdoutLine(std::string("rayomd ") + RAYOMD_VERSION);
            LocalFree(argv);
            return 0;
        }
    }

    if (argc >= 2 && lstrcmpiW(argv[1], L"--doctor") == 0) {
        int result = RunDoctor();
        LocalFree(argv);
        return result;
    }
    if (argc >= 2 && lstrcmpiW(argv[1], L"--inspect-source") == 0) {
        if (argc != 3) {
            PrintArgumentError("--inspect-source requires <input.pdf>.");
            LocalFree(argv);
            return 2;
        }
        int result = RunInspectSource(argv[2]);
        LocalFree(argv);
        return result;
    }
    if (argc >= 2 && lstrcmpiW(argv[1], L"--recover-source") == 0) {
        if (argc != 4) {
            PrintArgumentError("--recover-source requires <input.pdf> <output.md>.");
            LocalFree(argv);
            return 2;
        }
        int result = RunRecoverSource(argv[2], argv[3]);
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--export") != 0 &&
        lstrcmpiW(argv[1], L"--stdin") != 0 && lstrcmpiW(argv[1], L"--batch") != 0 &&
        lstrcmpiW(argv[1], L"--stdin-batch") != 0 && lstrcmpiW(argv[1], L"--serve") != 0 &&
        lstrcmpiW(argv[1], L"--bench") != 0 && lstrcmpiW(argv[1], L"--book") != 0) {
        PrintArgumentError("unknown command '" + WideToUtf8(argv[1]) + "'." + RayoMd::Cli::UnknownCommandHint(WideToUtf8(argv[1])));
        LocalFree(argv);
        return 2;
    }

    WinExportOptions options;
    std::wstring error;
    const bool exportCommand = lstrcmpiW(argv[1], L"--export") == 0;
    auto parseOrReport = [&](int start) -> bool {
        error.clear();
        if (!ParseExportOptions(argc, argv, start, options, error)) {
            PrintArgumentError(WideToUtf8(error));
            return false;
        }
        if (options.followLinks != 0 && !exportCommand) {
            PrintArgumentError("--follow-links applies to --export only.");
            return false;
        }
        if (options.followLinks != 0 && options.embedSource) {
            PrintArgumentError("--embed-source does not apply to --follow-links: a book is not reversible.");
            return false;
        }
        return true;
    };

    if (lstrcmpiW(argv[1], L"--stdin") == 0) {
        if (argc < 3 || argv[2][0] == L'\0' || IsOptionWord(argv[2])) {
            PrintArgumentError("--stdin requires <output.pdf>, or - for standard output.");
            LocalFree(argv);
            return 2;
        }
        if (!parseOrReport(3)) {
            LocalFree(argv);
            return 2;
        }
        std::wstring outputPath = argv[2];
        int result = 2;
        if (IsStandardOutput(outputPath) && StandardOutputIsConsole()) {
            PrintArgumentError(RayoMd::Cli::kTerminalOutput);
        } else if (!IsStandardOutput(outputPath) && NamesFolder(outputPath)) {
            PrintArgumentError("--stdin needs a file name for the PDF: " + WideToUtf8(outputPath) + " is a folder.");
        } else {
            if (!IsStandardOutput(outputPath) && !NamesPdf(outputPath)) outputPath += L".pdf";
            result = RunStdinExport(outputPath, options);
        }
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--stdin-batch") == 0) {
        if (argc < 3) {
            PrintArgumentError("--stdin-batch requires <output-folder>.");
            LocalFree(argv);
            return 2;
        }
        if (!parseOrReport(3)) {
            LocalFree(argv);
            return 2;
        }
        int result = RunStdinBatchExport(argv[2], options);
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--serve") == 0) {
        if (argc >= 3 && lstrcmpiW(argv[2], L"--json") == 0) {
            // Standard output carries the protocol only: an argument error goes to standard error.
            int result = 2;
#if RAYOMD_SERVE_JSON
            if (!ParseExportOptions(argc, argv, 3, options, error)) {
                PrintArgumentError(WideToUtf8(error));
            } else if (options.followLinks != 0 || !options.report.empty() || options.workers != 0 || options.recursive ||
                options.skipUnchanged) {
                PrintArgumentError("--serve --json takes no batch flags (--report, --workers, --recursive, "
                    "--skip-unchanged) and no --follow-links: its answers are the report.");
            } else {
                result = RunServeJson(options);
            }
#else
            WriteStderr("Error: this build has no --serve --json (built with RAYOMD_SERVE_JSON=OFF).\n");
#endif
            LocalFree(argv);
            return result;
        }
        if (argc < 3) {
            PrintArgumentError("--serve requires <output-folder>.");
            LocalFree(argv);
            return 2;
        }
        if (!parseOrReport(3)) {
            LocalFree(argv);
            return 2;
        }
        int result = RunServeExport(argv[2], options);
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--book") == 0) {
        // The inputs end at the output, the first argument that names a PDF; the options follow it.
        const auto namesPdf = [](const wchar_t* argument) {
            const size_t length = wcslen(argument);
            return length >= 4 && _wcsicmp(argument + length - 4, L".pdf") == 0;
        };
        int output = 2;
        while (output < argc && !namesPdf(argv[output])) output++;
        int result = 2;
        if (output == 2 || output == argc) {
            PrintArgumentError("--book requires <SUMMARY.md | folder | files.md...> <output.pdf>.");
        } else if (parseOrReport(output + 1)) {
            if (options.embedSource) {
                PrintArgumentError("--embed-source does not apply to --book: a book is not reversible.");
            } else {
                result = RunBookExport(std::vector<RayoMd::File::Path>(argv + 2, argv + output), argv[output], options);
            }
        }
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--bench") == 0) {
        if (argc < 5) {
            PrintArgumentError("--bench requires <input.md> <output-folder> <iterations>.");
            LocalFree(argv);
            return 2;
        }
        if (!parseOrReport(5)) {
            LocalFree(argv);
            return 2;
        }
        wchar_t* end = nullptr;
        const long iterations = std::wcstol(argv[4], &end, 10);
        int result = 2;
        if (end == argv[4] || *end != L'\0' || iterations < 1 || iterations > 100000000) {
            PrintArgumentError("--bench takes 1 to 100000000 iterations, not '" + WideToUtf8(argv[4]) + "'.");
        } else {
            result = RunNativeBench(argv[2], argv[3], static_cast<int>(iterations), options);
        }
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--batch") == 0) {
        int result = 2;
        if (argc < 4) {
            PrintArgumentError("--batch requires <input-folder> <output-folder>.");
        } else if (parseOrReport(4)) {
            result = RunBatchExport(argv[2], argv[3], options);
        }
        LocalFree(argv);
        return result;
    }

    // --export. The output may be left out, nothing or an option in its place: the PDF goes beside
    // the input.
    if (argc < 3) {
        PrintArgumentError("--export requires <input.md> [output.pdf].");
        LocalFree(argv);
        return 2;
    }
    const bool named = argc > 3 && !IsOptionWord(argv[3]);
    if (!parseOrReport(named ? 4 : 3)) {
        LocalFree(argv);
        return 2;
    }
    const std::wstring inputPath = argv[2];
    std::string pathError;
    const std::wstring outputPath = ExportOutputPath(inputPath, named ? argv[3] : nullptr, pathError);
    if (outputPath.empty()) {
        PrintArgumentError(pathError);
        LocalFree(argv);
        return 2;
    }
    if (options.followLinks != 0) {
        const int result = RunLinkedExport(inputPath, outputPath, options);
        LocalFree(argv);
        return result;
    }

    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    const int result = ExportFile(inputPath, outputPath, options, pdfBuffer);
    LocalFree(argv);
    return result;
}

int main() {
    return RunCommandLine();
}
