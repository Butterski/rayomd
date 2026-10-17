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
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <chrono>
#include <filesystem>
#include <codecvt>
#include <locale>

#ifndef RAYOMD_VERSION
#define RAYOMD_VERSION "0.0.0"
#endif

#ifdef _WIN32
#include "rayomd/tiny_pdf.h"
#include "../common/text_utils.h"
#include "../common/batch_report.h"
#include "../common/profiling.h"
#include "../core/book.h"
#include "../core/export_options.h"
#endif

thread_local int g_nativePdfLastError = 0;

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
    bool toc = false;
    int tocDepth = 3;
    int pageBreakBeforeLevel = 0;
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
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    wstr.resize(len - 1);
    return wstr;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.from_bytes(str);
#endif
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
#ifdef _WIN32
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    str.resize(len - 1);
    return str;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.to_bytes(wstr);
#endif
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
    CloseHandle(hFile);
    return ok;
}

bool ReadFileLimited(const std::wstring& path, long long maximumBytes, std::string& content) {
    RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Io);
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 0 || size.QuadPart > maximumBytes) {
        CloseHandle(hFile);
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
            CloseHandle(hFile);
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
    wchar_t suffix[128];
    swprintf(suffix, 128, L".rayomd-%lu-%lu-%lu-%llu%s",
        (unsigned long)GetCurrentProcessId(),
        (unsigned long)GetCurrentThreadId(),
        id,
        (unsigned long long)GetTickCount64(),
        finalExtension ? finalExtension : L".tmp");
    return dir + L"." + filename + suffix;
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

    CloseHandle(hFile);
    if (!ok) DeleteFileW(tempPath.c_str());
    return ok;
}

bool ReplaceWithTempFile(const std::wstring& tempPath, const std::wstring& finalPath) {
    if (MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(tempPath.c_str());
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
    options.toc = exportOptions.toc;
    options.tocDepth = exportOptions.tocDepth;
    options.pageBreakBeforeLevel = exportOptions.pageBreakBeforeLevel;
    options.theme = exportOptions.theme;
    return options;
}

// `details`, when given, receives the result of the build: pages, characters without a glyph,
// failed images.
bool BuildNativePdfBytes(const std::string& markdown, const WinExportOptions& exportOptions,
    std::string& pdfBytes, const std::wstring& sourcePath = L"", TinyPdf::BuildResult* details = nullptr) {
    TinyPdf::BuildResult result = TinyPdf::BuildPdf(markdown, PdfOptionsFor(exportOptions, sourcePath), pdfBytes);
    if (details) *details = result;
    g_nativePdfLastError = static_cast<int>(result.error);
    return result.Ok();
}

bool ExportNativePdf(const std::string& markdown, const std::wstring& outputPath,
    const WinExportOptions& exportOptions, const std::wstring& sourcePath = L"") {
    const auto profileBefore = RayoMd::Profiling::Capture();
    static thread_local std::string pdfBytes;
    pdfBytes.clear();
    pdfBytes.reserve(1024 * 1024);
    bool ok = BuildNativePdfBytes(markdown, exportOptions, pdfBytes, sourcePath);
    if (ok && !WriteBinaryFile(outputPath, pdfBytes)) {
        g_nativePdfLastError = 2;
        ok = false;
    }
    RayoMd::Profiling::EmitDelta("export", profileBefore, RayoMd::Profiling::Capture());
    return ok;
}

int GetNativePdfLastError() {
    return g_nativePdfLastError;
}
bool ParseExportOptions(int argc, LPWSTR* argv, int start, WinExportOptions& options, std::wstring& error) {
    for (int i = start; i < argc; i++) {
        if (lstrcmpiW(argv[i], L"pandoc") == 0) {
            error = L"the pandoc engine was removed; RayoMD exports with its native renderer only";
            return false;
        } else if (lstrcmpiW(argv[i], L"native") == 0) {
            // The only engine, still accepted from older command lines.
        } else if (lstrcmpiW(argv[i], L"--allow-url-images") == 0) {
            options.enableUrlImages = true;
        } else if (lstrcmpiW(argv[i], L"--allow-unsafe-local-images") == 0 || lstrcmpiW(argv[i], L"--unsafe-local-images") == 0) {
            options.allowUnsafeLocalImages = true;
        } else if (lstrcmpiW(argv[i], L"--embed-source") == 0) {
            options.embedSource = true;
        } else if (lstrcmpiW(argv[i], L"--no-image-cache") == 0) {
            options.imageCache = false;
        } else if (lstrcmpiW(argv[i], L"--page-numbers") == 0) {
            options.pageNumbers = true;
        } else if (lstrcmpiW(argv[i], L"--compress") == 0) {
            options.compress = true;
        } else if (lstrcmpiW(argv[i], L"--fast-images") == 0) {
            options.fastImages = true;
        } else if (lstrcmpiW(argv[i], L"--pdfa") == 0) {
            options.pdfa = true;
        } else if (lstrcmpiW(argv[i], L"--pdfua") == 0) {
            options.pdfua = true;
        } else if (lstrcmpiW(argv[i], L"--no-highlight") == 0) {
            options.highlightCode = false;
        } else if (lstrcmpiW(argv[i], L"--toc") == 0) {
            options.toc = true;
        } else if (_wcsnicmp(argv[i], L"--toc-depth=", 12) == 0) {
            const wchar_t* depth = argv[i] + 12;
            if (depth[0] < L'1' || depth[0] > L'6' || depth[1] != L'\0') {
                error = L"--toc-depth must be 1 to 6";
                return false;
            }
            options.tocDepth = depth[0] - L'0';
        } else if (_wcsnicmp(argv[i], L"--page-break-before=", 20) == 0) {
            const wchar_t* level = argv[i] + 20;
            if ((level[0] != L'h' && level[0] != L'H') || level[1] < L'1' || level[1] > L'6' || level[2] != L'\0') {
                error = L"--page-break-before must be h1 to h6";
                return false;
            }
            options.pageBreakBeforeLevel = level[1] - L'0';
        } else if (_wcsnicmp(argv[i], L"--theme=", 8) == 0) {
            const std::wstring themePath = argv[i] + 8;
            const size_t slash = themePath.find_last_of(L"\\/");
            const std::wstring directory = slash == std::wstring::npos ? std::wstring() : themePath.substr(0, slash);
            std::string text;
            std::string themeError;
            if (themePath.empty() || !ReadUtf8File(themePath, text)) {
                error = L"--theme: cannot read " + themePath;
                return false;
            }
            if (!TinyPdf::Internal::ParseTheme(text, WideToUtf8(directory), options.theme, themeError)) {
                error = L"--theme " + themePath + L": " + Utf8ToWide(themeError);
                return false;
            }
        } else if (_wcsnicmp(argv[i], L"--page-size=", 12) == 0) {
            if (!TinyPdf::Internal::ParsePageSize(WideToUtf8(argv[i] + 12), options.pageSize)) {
                error = L"--page-size must be a4, a3, a5, letter or legal (each also with -landscape), or "
                    L"WIDTHxHEIGHT in mm, cm, in or pt, such as 210x297mm";
                return false;
            }
        } else if (lstrcmpiW(argv[i], L"--recursive") == 0) {
            options.recursive = true;
        } else if (lstrcmpiW(argv[i], L"--skip-unchanged") == 0) {
            options.skipUnchanged = true;
        } else if (_wcsnicmp(argv[i], L"--report=", 9) == 0) {
            options.report = WideToUtf8(argv[i] + 9);
            if (options.report.empty()) {
                error = L"--report needs a file name, or - for standard output";
                return false;
            }
        } else if (_wcsnicmp(argv[i], L"--workers=", 10) == 0) {
            wchar_t* end = nullptr;
            unsigned long parsed = std::wcstoul(argv[i] + 10, &end, 10);
            if (!end || *end != wchar_t(0) || parsed < 1 || parsed > 64) {
                error = L"--workers must be between 1 and 64";
                return false;
            }
            options.workers = static_cast<unsigned>(parsed);
        } else {
            std::string token = WideToUtf8(argv[i]);
            TinyPdf::PdfMargin margin;
            TinyPdf::PdfStyle style;
            if (TinyPdf::Internal::ParsePdfMargin(token, margin)) options.margin = margin;
            else if (TinyPdf::Internal::ParsePdfStyle(token, style)) options.style = style;
            else {
                error = L"unrecognized export option '" + std::wstring(argv[i]) + L"'";
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

bool EnsureDirectoryRecursive(const std::wstring& path) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    if (!ec) return true;
    return std::filesystem::exists(std::filesystem::path(path));
}

void WriteStdoutLine(const std::string& line);

// --book: the files of a SUMMARY.md, a folder or the command line (RayoMd::Book::ReadBook) as one PDF.
int RunBookExport(const std::vector<std::filesystem::path>& inputs, const std::wstring& outputPath,
    const WinExportOptions& exportOptions) {
    TinyPdf::Book book;
    std::string error;
    if (!RayoMd::Book::ReadBook(inputs, book, error)) {
        WriteStdoutLine("Error: " + error);
        return 3;
    }
    std::string pdfBytes;
    const TinyPdf::BuildResult result = TinyPdf::BuildBookPdf(book, PdfOptionsFor(exportOptions, L""), pdfBytes);
    if (!result) {
        WriteStdoutLine("Error: native PDF export failed for the book (code " + std::to_string(10 + static_cast<int>(result.error)) + ").");
        return 10 + static_cast<int>(result.error);
    }
    if (!WriteBinaryFile(outputPath, pdfBytes)) {
        WriteStdoutLine("Error: could not write PDF file: " + WideToUtf8(outputPath));
        return 12;
    }
    return 0;
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

int ExportNativeFileWithBufferResult(const std::wstring& inputPath, const std::wstring& outputPath,
    const WinExportOptions& options, std::string& pdfBuffer, TinyPdf::BuildResult* details = nullptr) {
    const auto profileBefore = RayoMd::Profiling::Capture();
    auto finish = [&](int result) {
        RayoMd::Profiling::EmitDelta("export", profileBefore, RayoMd::Profiling::Capture());
        return result;
    };
    std::string markdown;
    if (!ReadUtf8File(inputPath, markdown)) return finish(3);
    pdfBuffer.clear();
    if (!BuildNativePdfBytes(markdown, options, pdfBuffer, inputPath, details)) {
        return finish(10 + GetNativePdfLastError());
    }
    return finish(WriteBinaryFile(outputPath, pdfBuffer) ? 0 : 12);
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
    WriteStdoutLine("Error: could not create the report file: " + options.report);
    return false;
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

unsigned ResolveWinWorkerCount(const WinExportOptions& options, const std::vector<WinBatchJob>& jobs) {
    if (jobs.empty()) return 1;
    unsigned workers = options.workers;
    if (workers == 0) {
        workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 2;
        workers = std::min(workers, 6u);
    }
    uintmax_t largestInput = 0;
    for (const WinBatchJob& job : jobs) {
        std::error_code error;
        uintmax_t size = std::filesystem::file_size(std::filesystem::path(job.inputPath), error);
        if (!error) largestInput = std::max(largestInput, size);
    }
    if (largestInput > 64u * 1024u * 1024u) workers = 1;
    else if (largestInput > 16u * 1024u * 1024u) workers = std::min(workers, 2u);
    return std::max(1u, std::min(workers, static_cast<unsigned>(jobs.size())));
}

int ExecuteWinBatchJobs(std::vector<WinBatchJob>& jobs, const WinExportOptions& options, const char* label) {
    RayoMd::Batch::Report report;
    if (!OpenWinReport(report, options)) return 12;
    const auto upToDate = [&](const WinBatchJob& job) {
        return options.skipUnchanged && RayoMd::Batch::OutputUpToDate(job.inputPath, job.outputPath);
    };
    const auto skip = [](WinBatchJob& job) {
        std::error_code sizeError;
        job.skipped = true;
        job.bytes = std::filesystem::file_size(std::filesystem::path(job.outputPath), sizeError);
    };
    const unsigned workerCount = ResolveWinWorkerCount(options, jobs);
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned worker = 0; worker < workerCount; worker++) {
        workers.emplace_back([&] {
            std::string pdfBuffer;
            pdfBuffer.reserve(1024 * 1024);
            for (;;) {
                size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= jobs.size()) break;
                WinBatchJob& job = jobs[index];
                if (job.result == 0 && upToDate(job)) {
                    skip(job);
                } else if (job.result == 0) {
                    const auto start = std::chrono::steady_clock::now();
                    job.result = ExportNativeFileWithBufferResult(job.inputPath, job.outputPath, options, pdfBuffer,
                        &job.details);
                    job.milliseconds = MillisecondsSince(start);
                    if (job.result == 0) job.bytes = pdfBuffer.size();
                }
                if (report.IsOpen()) report.Write(WinRecordFor(job));
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    const bool reported = report.Close();
    if (!reported) WriteStdoutLine("Error: could not write the report file: " + options.report);

    int failures = 0;
    int lastResult = 0;
    for (const WinBatchJob& job : jobs) {
        if (job.result == 0) continue;
        failures++;
        lastResult = job.result;
        if (!job.error.empty()) WriteStdoutLine(job.error);
        else WriteStdoutLine("Error: " + std::string(label) + " failed for " +
            WideToUtf8(job.inputPath) + " (code " + std::to_string(job.result) + ").");
    }
    if (failures != 0) {
        WriteStdoutLine("Error: " + std::string(label) + " failed for " +
            std::to_string(failures) + " file(s).");
    }
    return failures == 0 ? (reported ? 0 : 12) : lastResult;
}

int RunBatchExport(const std::wstring& inputDir, const std::wstring& outputDir, const WinExportOptions& options) {
    if (!EnsureDirectoryRecursive(outputDir)) return 12;
    std::vector<RayoMd::Batch::File> files;
    std::string listError;
    if (!RayoMd::Batch::CollectMarkdown(std::filesystem::path(inputDir), std::filesystem::path(outputDir),
            options.recursive, files, listError)) {
        WriteStdoutLine("Error: could not read input folder: " + WideToUtf8(inputDir) + " (" + listError + ")");
        return 3;
    }

    std::vector<WinBatchJob> jobs;
    jobs.reserve(files.size());
    std::set<std::wstring> seenOutputs;
    for (const RayoMd::Batch::File& file : files) {
        WinBatchJob job(file.input.wstring(), file.output.wstring());
        // A subfolder's PDFs go to a subfolder of the output folder, made before the workers start.
        const std::wstring folder = file.output.parent_path().wstring();
        if (!seenOutputs.insert(OutputPathKey(job.outputPath)).second) {
            job.result = 12;
            job.error = "Error: multiple inputs map to the same output PDF: " + WideToUtf8(job.outputPath);
        } else if (options.recursive && !EnsureDirectoryRecursive(folder)) {
            job.result = 12;
            job.error = "Error: could not create output folder: " + WideToUtf8(folder);
        }
        jobs.push_back(std::move(job));
    }
    return ExecuteWinBatchJobs(jobs, options, "batch export");
}

bool IsCleanStdinEof(DWORD error) {
    return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_NO_DATA;
}

bool ReadAllStdin(std::string& input, size_t maxBytes = (size_t)kMaxMarkdownInputBytes) {
    input.clear();
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == nullptr) return false;

    char buf[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(hIn, buf, sizeof(buf), &read, nullptr)) {
            return IsCleanStdinEof(GetLastError());
        }
        if (read == 0) return true;
        if (maxBytes != 0 && input.size() + read > maxBytes) return false;
        input.append(buf, buf + read);
    }
}

void WriteStdoutLine(const std::string& line) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE || hOut == nullptr) return;
    DWORD written = 0;
    std::string output = line + "\n";
    WriteFile(hOut, output.data(), (DWORD)output.size(), &written, nullptr);
}

int RunStdinBatchExport(const std::wstring& outputDir, const WinExportOptions& options) {
    if (!EnsureDirectoryRecursive(outputDir)) return 12;
    std::string list;
    if (!ReadAllStdin(list, 0)) return 3;

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
    if (!ReadAllStdin(markdown)) {
        WriteStdoutLine("Error: could not read Markdown from stdin or input exceeded " +
            std::to_string(kMaxMarkdownInputBytes) + " bytes.");
        return 3;
    }

    return ExportNativePdf(markdown, outputPath, options, L"") ? 0 : 10 + GetNativePdfLastError();
}

int RunServeExport(const std::wstring& outputDir, const WinExportOptions& options) {
    if (!EnsureDirectoryRecursive(outputDir)) return 12;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == nullptr) return 3;

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

    auto processLine = [&](std::string line) {
        line = RayoMd::Text::Trim(line);
        if (line.empty()) return true;
        if (line == "quit" || line == "exit") return false;

        WinBatchJob job(Utf8ToWide(line), std::wstring());
        job.outputPath = JoinPath(outputDir, PdfNameForMarkdown(job.inputPath));

        LARGE_INTEGER start = {}, end = {};
        QueryPerformanceCounter(&start);
        job.result = ExportNativeFileWithBufferResult(job.inputPath, job.outputPath, options, pdfBuffer, &job.details);
        if (job.result == 0) job.bytes = pdfBuffer.size();
        QueryPerformanceCounter(&end);

        const bool ok = job.result == 0;
        double ms = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
        job.milliseconds = ms;
        if (!ok) failures++;
        if (report.IsOpen()) report.Write(WinRecordFor(job));
        WriteStdoutLine(std::string(ok ? "OK\t" : "ERR\t") + RayoMd::Text::FormatDouble(ms) + "\t" + WideToUtf8(job.outputPath));
        return true;
    };

    while (ReadFile(hIn, buf, sizeof(buf), &read, nullptr) && read > 0) {
        pending.append(buf, buf + read);
        size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending.erase(0, pos + 1);
            if (!processLine(line)) {
                report.Close();
                return failures == 0 ? 0 : 10 + GetNativePdfLastError();
            }
        }
    }

    if (!pending.empty()) processLine(pending);
    report.Close();
    return failures == 0 ? 0 : 10 + GetNativePdfLastError();
}

int RunNativeBench(const std::wstring& inputPath, const std::wstring& outputDir,
    int iterations, const WinExportOptions& exportOptions) {
    if (iterations < 1) iterations = 1;
    // A benchmark measures the work: no image is read from or kept in the persistent cache.
    WinExportOptions options = exportOptions;
    options.imageCache = false;
    if (!EnsureDirectoryRecursive(outputDir)) return 12;

    std::string markdown;
    if (!ReadUtf8File(inputPath, markdown)) return 3;

    std::string pdfBytes;
    if (!BuildNativePdfBytes(markdown, options, pdfBytes, inputPath)) {
        return 10 + GetNativePdfLastError();
    }
    WriteBinaryFile(JoinPath(outputDir, L"sample.pdf"), pdfBytes);

    LARGE_INTEGER freq = {}, start = {}, end = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    size_t totalBytes = 0;
    for (int i = 0; i < iterations; i++) {
        pdfBytes.clear();
        if (!BuildNativePdfBytes(markdown, options, pdfBytes, inputPath)) {
            return 10 + GetNativePdfLastError();
        }
        totalBytes += pdfBytes.size();
    }

    QueryPerformanceCounter(&end);
    double totalMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
    double avgMs = totalMs / (double)iterations;

    std::ostringstream report;
    report << "input_bytes=" << markdown.size() << "\n";
    report << "iterations=" << iterations << "\n";
    report << "total_ms=" << RayoMd::Text::FormatDouble(totalMs) << "\n";
    report << "avg_ms=" << RayoMd::Text::FormatDouble(avgMs) << "\n";
    report << "avg_pdf_bytes=" << (totalBytes / (size_t)iterations) << "\n";
    report << "path=" << RayoMd::Text::RendererPathName(markdown) << "\n";

    if (!WriteUtf8File(JoinPath(outputDir, L"bench-results.txt"), report.str())) return 12;
    return 0;
}
int RunDoctor() {
    WriteStdoutLine(std::string("RayoMD doctor ") + RAYOMD_VERSION);
    WriteStdoutLine("platform=windows");
    WriteStdoutLine("curl=disabled");
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
        WriteStdoutLine("Error: could not read PDF or input exceeds configured limit.");
        return 3;
    }
    RayoMd::PdfSource::Result result = RayoMd::PdfSource::Inspect(pdf, false);
    if (!result.Ok()) {
        WriteStdoutLine(std::string("Error: ") + RayoMd::PdfSource::StatusMessage(result.status) + ".");
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
        WriteStdoutLine("Error: could not read PDF or input exceeds configured limit.");
        return 3;
    }
    RayoMd::PdfSource::Result result = RayoMd::PdfSource::Inspect(pdf, true);
    if (!result.Ok()) {
        WriteStdoutLine(std::string("Error: ") + RayoMd::PdfSource::StatusMessage(result.status) + ".");
        return RecoveryExitCode(result.status);
    }
    bool existed = false;
    if (!WriteNewBinaryFile(outputPath, result.source, existed)) {
        WriteStdoutLine(std::string(existed ? "Error: destination already exists: " :
            "Error: could not write recovered source: ") + WideToUtf8(outputPath));
        return existed ? 34 : 12;
    }
    WriteStdoutLine("Recovered " + std::to_string(result.info.sourceBytes) +
        " bytes to " + WideToUtf8(outputPath));
    return 0;
}

void WriteStderr(const std::string& text) {
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr == INVALID_HANDLE_VALUE || hErr == nullptr) return;
    DWORD written = 0;
    WriteFile(hErr, text.data(), (DWORD)text.size(), &written, nullptr);
}

void PrintUsage() {
    WriteStderr("RayoMD Windows CLI " RAYOMD_VERSION "\n"
        "Usage:\n"
        "  rayomd.exe --version\n"
        "  rayomd.exe --doctor\n"
        "  rayomd.exe --inspect-source <input.pdf>\n"
        "  rayomd.exe --recover-source <input.pdf> <output.md>\n"
        "  rayomd.exe --export <input.md> <output.pdf> [native] [style] [margin]\n"
        "  rayomd.exe --book <SUMMARY.md | folder | files.md...> <output.pdf> [native] [style] [margin]\n"
        "  rayomd.exe --stdin <output.pdf> [native] [style] [margin]\n"
        "  rayomd.exe --batch <input-folder> <output-folder> [native] [style] [margin]\n"
        "  rayomd.exe --stdin-batch <output-folder> [native] [style] [margin]\n"
        "  rayomd.exe --serve <output-folder> [native] [style] [margin]\n"
        "  rayomd.exe --bench <input.md> <output-folder> <iterations> [style] [margin]\n"
        "\n"
        "Defaults: native elegant normal, URL images off, local images contained to the input directory.\n"
        "Styles: elegant, modern, tech. Margins: compact, normal, wide, margin=0.75in, margin=54pt.\n"
        "Resource flags: --allow-url-images, --allow-unsafe-local-images, --embed-source, --no-image-cache\n"
        "  (by default PNGs with transparency are re-encoded once and kept in\n"
        "  %LOCALAPPDATA%\\RayoMD\\cache\\images: at most about 256 MB, the PDF the same either way).\n"
        "Output flags: --compress (FlateDecode streams: smaller files, slower export), --fast-images\n"
        "  (PNGs with transparency re-encoded faster, their image streams larger), --pdfa (PDF/A-3b:\n"
        "  every font embedded, the formula fonts too), --pdfua (PDF/UA-1, tagged for screen readers:\n"
        "  headings, lists, tables, figures with their alt text, links and notes; fonts as with --pdfa,\n"
        "  formulas as TeX source; the language is the front matter's lang, else en), --theme=FILE (key =\n"
        "  value lines: font, font-bold, font-italic, font-bold-italic, logo, heading-color, link-color,\n"
        "  accent-color, header-left/center/right, footer-left/center/right with {title} {author}\n"
        "  {subject} {date} {page} {pages} {logo}, cover), --no-highlight (code\n"
        "  blocks in one colour; by default fenced code in a language GitHub knows is coloured), --toc\n"
        "  (a table of contents, first or at a [TOC] or [[_TOC_]] paragraph, which always gets one),\n"
        "  --toc-depth=N (heading levels it lists, 1-6; default 3).\n"
        "Page flags: --page-numbers (\"N / M\" at the foot of every page), --page-size=SIZE (a4, a3, a5,\n"
        "  letter or legal, each also with -landscape, or WIDTHxHEIGHT in mm, cm, in or pt; default a4),\n"
        "  --page-break-before=hN (a new page before each heading of level 1 to N; h1 for chapters).\n"
        "Batch flags: --workers=N (1-64; automatic mode uses at most 6), --recursive (subfolders,\n"
        "  mirrored), --skip-unchanged (keep PDFs newer than their Markdown), --report=FILE (one JSON\n"
        "  line per document; - for standard output).\n"
        "Books: --book makes one PDF of the files a SUMMARY.md lists (mdBook, GitBook), of a folder's\n"
        "  Markdown files by name with README.md first, or of the files given: each from a new page, the\n"
        "  links between them kept, one outline, and with --toc one table of contents for all of them.\n");
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

    if (argc >= 2 && (lstrcmpiW(argv[1], L"--version") == 0 || lstrcmpiW(argv[1], L"-v") == 0)) {
        WriteStdoutLine(std::string("rayomd ") + RAYOMD_VERSION);
        LocalFree(argv);
        return 0;
    }

    if (argc >= 2 && lstrcmpiW(argv[1], L"--doctor") == 0) {
        int result = RunDoctor();
        LocalFree(argv);
        return result;
    }
    if (argc >= 2 && lstrcmpiW(argv[1], L"--inspect-source") == 0) {
        if (argc != 3) {
            WriteStdoutLine("Error: --inspect-source requires <input.pdf>.");
            LocalFree(argv);
            return 2;
        }
        int result = RunInspectSource(argv[2]);
        LocalFree(argv);
        return result;
    }
    if (argc >= 2 && lstrcmpiW(argv[1], L"--recover-source") == 0) {
        if (argc != 4) {
            WriteStdoutLine("Error: --recover-source requires <input.pdf> <output.md>.");
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
        WriteStderr("Error: unknown command '" + WideToUtf8(argv[1]) + "'.\n\n");
        PrintUsage();
        LocalFree(argv);
        return 2;
    }

    WinExportOptions options;
    std::wstring error;
    auto parseOrReport = [&](int start) -> bool {
        error.clear();
        if (ParseExportOptions(argc, argv, start, options, error)) return true;
        WriteStdoutLine("Error: " + WideToUtf8(error));
        return false;
    };

    if (lstrcmpiW(argv[1], L"--stdin") == 0) {
        if (argc < 3) {
            WriteStdoutLine("Error: --stdin requires <output.pdf>.");
            LocalFree(argv);
            return 2;
        }
        if (!parseOrReport(3)) {
            LocalFree(argv);
            return 2;
        }
        std::wstring outputPath = argv[2];
        if (outputPath.size() < 4 || _wcsicmp(outputPath.c_str() + outputPath.size() - 4, L".pdf") != 0) {
            outputPath += L".pdf";
        }
        int result = RunStdinExport(outputPath, options);
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--stdin-batch") == 0) {
        if (argc < 3) {
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
        if (argc < 3) {
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
            WriteStdoutLine("Error: --book requires <SUMMARY.md | folder | files.md...> <output.pdf>.");
        } else if (parseOrReport(output + 1)) {
            if (options.embedSource) {
                WriteStdoutLine("Error: --embed-source does not apply to --book: a book is not reversible.");
            } else {
                result = RunBookExport(std::vector<std::filesystem::path>(argv + 2, argv + output), argv[output], options);
            }
        }
        LocalFree(argv);
        return result;
    }

    if (lstrcmpiW(argv[1], L"--bench") == 0) {
        if (argc < 5) {
            LocalFree(argv);
            return 2;
        }
        if (!parseOrReport(5)) {
            LocalFree(argv);
            return 2;
        }
        int iterations = _wtoi(argv[4]);
        int result = RunNativeBench(argv[2], argv[3], iterations, options);
        LocalFree(argv);
        return result;
    }

    if (argc < 4) {
        WriteStdoutLine(lstrcmpiW(argv[1], L"--export") == 0
            ? "Error: --export requires <input.md> <output.pdf>."
            : "Error: --batch requires <input-folder> <output-folder>.");
        LocalFree(argv);
        return 2;
    }

    if (!parseOrReport(4)) {
        LocalFree(argv);
        return 2;
    }

    if (lstrcmpiW(argv[1], L"--batch") == 0) {
        int result = RunBatchExport(argv[2], argv[3], options);
        LocalFree(argv);
        return result;
    }

    std::wstring inputPath = argv[2];
    std::wstring outputPath = argv[3];
    if (outputPath.size() < 4 || _wcsicmp(outputPath.c_str() + outputPath.size() - 4, L".pdf") != 0) {
        outputPath += L".pdf";
    }

    std::string markdown;
    if (!ReadUtf8File(inputPath, markdown)) {
        WriteStdoutLine("Error: could not read input Markdown file: " + WideToUtf8(inputPath));
        LocalFree(argv);
        return 3;
    }
    const bool ok = ExportNativePdf(markdown, outputPath, options, inputPath);

    LocalFree(argv);
    return ok ? 0 : 10 + GetNativePdfLastError();
}

int main() {
    return RunCommandLine();
}
