// SPDX-License-Identifier: Apache-2.0

#pragma once

// What both command lines say of an export, in the same words on Linux and Windows: the error of
// a build that failed, the warning of characters no font could show, why a file could not be read
// or written and why a folder cannot be read or made. Each command line writes these on standard
// error.

#include "rayomd/tiny_pdf.h"
#include "../common/file_io.h"
#include "../common/path.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace RayoMd::Cli {

// " (why)" after a message, or nothing.
inline std::string Reason(const std::string& why) {
    return why.empty() ? std::string() : " (" + why + ")";
}

// "Error: native PDF export failed for <label> (code N)." and what the code means; no line break.
inline std::string ExportFailure(const TinyPdf::BuildResult& result, const std::string& inputLabel,
    const TinyPdf::PdfTheme& theme) {
    std::string message = "Error: native PDF export failed";
    if (!inputLabel.empty()) message += " for " + inputLabel;
    message += " (code " + std::to_string(10 + static_cast<int>(result.error)) + ").";
    if (result.error == TinyPdf::BuildError::ThemeFontUnavailable) {
        std::string paths;
        for (const std::string* path : { &theme.fontPath, &theme.fontBoldPath, &theme.fontItalicPath,
                 &theme.fontBoldItalicPath }) {
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

// A warning, not an error: the PDF is written. With its line break, so a batch worker writes it
// in one piece.
inline std::string MissingCharactersWarning(const std::string& inputLabel, uint32_t count) {
    std::string message = "Warning: ";
    message += inputLabel.empty() ? std::string("the document") : inputLabel;
    message += ": ";
    message += std::to_string(count);
    message += " character(s) have no glyph in the fonts found and are shown as a base letter or '?'."
        " Install a font that has them (DejaVu Sans or Noto Sans for most alphabets, Noto Sans CJK or"
        " WenQuanYi Micro Hei for Chinese, Japanese and Korean), or set RAYOMD_FONT or RAYOMD_FALLBACK_FONT"
        " to a .ttf, .otf or .ttc file.\n";
    return message;
}

// The error of the last system call that failed: errno, or GetLastError's code on Windows.
inline int LastSystemError() {
#ifdef _WIN32
    return static_cast<int>(GetLastError());
#else
    return errno;
#endif
}

// What the system says of `error` (LastSystemError): strerror's text, on Windows FormatMessage's,
// in English where Windows has it, else in its own language; without a final period.
inline std::string SystemErrorText(int error) {
#ifdef _WIN32
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    wchar_t* text = nullptr;
    DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(error), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    if (length == 0) {
        length = FormatMessageW(flags, nullptr, static_cast<DWORD>(error), 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    }
    while (length != 0 && (text[length - 1] == L'\r' || text[length - 1] == L'\n' || text[length - 1] == L' ' ||
                              text[length - 1] == L'.')) {
        length--;
    }
    std::string message;
    if (length != 0) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        message.resize(bytes > 0 ? static_cast<size_t>(bytes) : 0);
        if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), &message[0], bytes, nullptr, nullptr);
    }
    if (text) LocalFree(text);
    return message.empty() ? "system error " + std::to_string(error) : message;
#else
    return std::strerror(error);
#endif
}

// Whether `code` (LastSystemError, or what StatusOf gives) says there is nothing at a path.
inline bool IsMissing(int code) {
#ifdef _WIN32
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
#else
    return code == ENOENT || code == ENOTDIR;
#endif
}

// Why the file at `path` could not be read, of at most `maximum` bytes, after a read failed with
// `error` (LastSystemError): what is there now (nothing, a folder, no regular file, a larger
// file), else what the system says of `error`.
inline std::string ReadFailure(const RayoMd::File::Path& path, uint64_t maximum, int error) {
    RayoMd::File::FileStatus status;
    int code = 0;
    if (!RayoMd::File::StatusOf(path, status, &code)) return SystemErrorText(code);
    if (status.type == RayoMd::File::FileType::Directory) return "a folder, not a file";
    if (status.type != RayoMd::File::FileType::Regular) return "not a regular file";
    if (status.size > maximum) return "larger than " + std::to_string(maximum >> 20) + " MiB";
    return error != 0 ? SystemErrorText(error) : std::string();
}

// Why the file `path` could not be written, after the write failed with `error`
// (LastSystemError). A folder of its path that is missing or no folder is said as such: the
// system's "No such file or directory" reads as if the file itself were missing.
inline std::string WriteFailure(const RayoMd::File::Path& path, int error) {
    const RayoMd::File::Path folder(RayoMd::File::ParentOf(RayoMd::File::PathView(path)));
    RayoMd::File::FileStatus status;
    int code = 0;
    if (!folder.empty() && !RayoMd::File::StatusOf(folder, status, &code)) {
        if (IsMissing(code)) return "its folder does not exist";
    } else if (!folder.empty() && status.type != RayoMd::File::FileType::Directory) {
        return "its folder is a file";
    }
    return error != 0 ? SystemErrorText(error) : std::string();
}

// "Error: could not read <what>: <path> (why)", after a read of at most `maximum` bytes failed
// with `error` (LastSystemError); no line break.
inline std::string ReadError(const char* what, const RayoMd::File::Path& path, uint64_t maximum, int error) {
    return std::string("Error: could not read ") + what + ": " + RayoMd::File::PathToUtf8(path) +
        Reason(ReadFailure(path, maximum, error));
}

// "Error: could not write <what>: <path> (why)", after the write failed with `error`; no line
// break.
inline std::string WriteError(const char* what, const RayoMd::File::Path& path, int error) {
    return std::string("Error: could not write ") + what + ": " + RayoMd::File::PathToUtf8(path) +
        Reason(WriteFailure(path, error));
}

// Whether a document's bytes are a PDF's: read as Markdown they would make a PDF of PDF syntax,
// and `--export report.pdf report.pdf` replaced the PDF with that.
inline bool IsPdf(std::string_view bytes) {
    return bytes.size() >= 5 && std::memcmp(bytes.data(), "%PDF-", 5) == 0;
}

// The error for such a document, a file's or (no label) standard input's; no line break.
inline std::string PdfGivenAsMarkdown(const std::string& inputLabel) {
    if (inputLabel.empty()) return "Error: standard input is a PDF, not Markdown";
    return "Error: " + inputLabel + " is a PDF, not Markdown (input and output the other way round?)";
}

// Why `path` is no folder to read files from: empty when it is one.
inline std::string NotAFolder(const RayoMd::File::Path& path) {
    RayoMd::File::FileStatus status;
    int code = 0;
    if (!RayoMd::File::StatusOf(path, status, &code)) return IsMissing(code) ? "does not exist" : SystemErrorText(code);
    return status.type == RayoMd::File::FileType::Directory ? std::string() : std::string("not a directory");
}

// Makes the folder `path` and the folders above it that are missing: empty when it is a folder
// then, else why not.
inline std::string MakeFolder(const RayoMd::File::Path& path) {
    int code = 0;
    if (!RayoMd::File::CreateDirectories(path, &code)) return SystemErrorText(code);
    return NotAFolder(path);
}

} // namespace RayoMd::Cli
