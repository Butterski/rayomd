// SPDX-License-Identifier: Apache-2.0

#pragma once

// What both command lines say of an export, in the same words on Linux and Windows: the error of
// a build that failed, the warning of characters no font could show, and why a folder cannot be
// read or made. Each command line writes these on standard error.

#include "rayomd/tiny_pdf.h"
#include "../common/path.h"

#include <cerrno>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winerror.h>
#endif

namespace RayoMd::Cli {

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

// Why `path` is no folder to read files from: empty when it is one.
inline std::string NotAFolder(const RayoMd::File::Path& path) {
    RayoMd::File::FileStatus status;
    int code = 0;
    if (!RayoMd::File::StatusOf(path, status, &code)) {
#ifdef _WIN32
        const bool missing = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
#else
        const bool missing = code == ENOENT || code == ENOTDIR;
#endif
        return missing ? std::string("does not exist") : RayoMd::File::ErrorText(code);
    }
    return status.type == RayoMd::File::FileType::Directory ? std::string() : std::string("not a directory");
}

// Makes the folder `path` and the folders above it that are missing: empty when it is a folder
// then, else why not.
inline std::string MakeFolder(const RayoMd::File::Path& path) {
    int code = 0;
    if (!RayoMd::File::CreateDirectories(path, &code)) return RayoMd::File::ErrorText(code);
    return NotAFolder(path);
}

// " (why)" after a message, or nothing.
inline std::string Reason(const std::string& why) {
    return why.empty() ? std::string() : " (" + why + ")";
}

} // namespace RayoMd::Cli
