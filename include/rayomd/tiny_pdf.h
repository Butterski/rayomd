// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace TinyPdf {

enum class PdfStyle : uint8_t {
    Elegant,
    Modern,
    Tech,
};

enum class MarginPreset : uint8_t {
    Compact,
    Normal,
    Wide,
    Custom,
};

struct PdfMargin {
    MarginPreset preset = MarginPreset::Normal;
    double customPoints = 72.0;

    static PdfMargin Compact() { return { MarginPreset::Compact, 0.0 }; }
    static PdfMargin Normal() { return { MarginPreset::Normal, 0.0 }; }
    static PdfMargin Wide() { return { MarginPreset::Wide, 0.0 }; }
    static PdfMargin CustomPoints(double points) { return { MarginPreset::Custom, points }; }
};

// Page width and height in points (1/72 inch). Each side is kept between 144 and 14,400
// points, and a margin too wide for the page shrinks to leave 72 points of text.
struct PdfPageSize {
    double width = 595.0;
    double height = 842.0;

    static PdfPageSize A4() { return { 595.0, 842.0 }; }
    static PdfPageSize A3() { return { 842.0, 1191.0 }; }
    static PdfPageSize A5() { return { 420.0, 595.0 }; }
    static PdfPageSize Letter() { return { 612.0, 792.0 }; }
    static PdfPageSize Legal() { return { 612.0, 1008.0 }; }
};

// A company look over the style. Empty or unset fields leave the style as it is. Its font and logo
// paths are local files: on Windows a UNC (\\server\share), device (\\.\, \\?\) or mapped network
// drive path is never opened, so a theme cannot make a build connect to a share; such a font
// fails the build (ThemeFontUnavailable), such a logo is a failed image.
struct PdfTheme {
    static constexpr int32_t kStyleColor = -1;

    // A TrueType or OpenType font (.ttf, .otf, or .ttc for its first face) for all text, embedded
    // as a subset; the standard fonts are then not used.
    std::string fontPath;
    // Its bold, italic and bold italic faces, each optional: bold text, headings, table header
    // rows and alert titles in the bold face, emphasis in the italic one. Without one, that style
    // is drawn from `fontPath` (bold by a second copy, italic slanted); bold italic falls back to
    // the bold face slanted, else the italic face drawn bold. They need `fontPath`.
    std::string fontBoldPath;
    std::string fontItalicPath;
    std::string fontBoldItalicPath;
    // Colours as 0xRRGGBB, or kStyleColor.
    int32_t headingColor = kStyleColor;
    int32_t linkColor = kStyleColor;
    int32_t accentColor = kStyleColor;   // rules and the bar beside block quotes
    // Text in the top and bottom margin of every page, at its left, centre and right, cut short
    // with an ellipsis when it does not fit. {title}, {author}, {subject}, {date}, {page} and
    // {pages} are replaced from the front matter and the page; a field that is just {logo}
    // shows the logo. With pageNumbers and no footerCenter, the footer centre is "{page} / {pages}".
    std::string headerLeft;
    std::string headerCenter;
    std::string headerRight;
    std::string footerLeft;
    std::string footerCenter;
    std::string footerRight;
    // A PNG, JPEG or SVG image for {logo} and the cover. One that cannot be read counts as a
    // failed image.
    std::string logoPath;
    // A first page with the logo, the title, the subject, the author and the date of the front
    // matter; {page} and {pages} count the pages after it.
    bool cover = false;
};

enum class BuildError : uint8_t {
    None,
    // No longer returned: a document whose characters need a TrueType font that cannot be
    // found is drawn in the standard fonts, and BuildResult::missingCharacters counts what
    // they could not show.
    FontUnavailable = 1,
    SourceTooLarge = 4,
    InvalidSourceUtf8 = 5,
    ReversiblePdfTooLarge = 6,
    // PdfTheme::fontPath cannot be read or is no TrueType or OpenType font.
    ThemeFontUnavailable = 7,
    // PdfOptions::pdfa or pdfua, but no TrueType or OpenType font to embed: neither RAYOMD_FONT
    // nor a system font.
    PdfaFontUnavailable = 8,
};

struct BuildResult {
    BuildError error = BuildError::None;
    // Characters no available font could show, drawn as their base letter or as '?'.
    uint32_t missingCharacters = 0;
    // Pages in the PDF.
    uint32_t pages = 0;
    // Standalone images that could not be loaded or decoded and show their fallback text.
    uint32_t failedImages = 0;

    constexpr bool Ok() const { return error == BuildError::None; }
    constexpr explicit operator bool() const { return Ok(); }
};

struct PdfOptions {
    PdfStyle style = PdfStyle::Elegant;
    PdfMargin margin = PdfMargin::Normal();
    PdfPageSize pageSize = PdfPageSize::A4();
    std::string sourcePath;
    bool enableUrlImages = false;
    bool allowUnsafeLocalImages = false;
    bool embedSource = false;
    // A directory (UTF-8) where re-encoded PNGs with transparency are kept between runs, each
    // named by the SHA-256 of the PNG: a later export of the same image, in any process, reads it
    // instead of decoding the PNG again. The PDF bytes are the same either way. At most about
    // 256 MB, the least recently used images removed first. Empty, the default: nothing is kept.
    std::string imageCacheDirectory;
    // "N / M" centred in the bottom margin of every page.
    bool pageNumbers = false;
    // FlateDecode page content, font program and CMaps where that makes the file smaller:
    // text-heavy PDFs two to five times smaller, at the cost of compressing them.
    bool compress = false;
    // PNGs with transparency re-encoded faster (a 1024x1024 one 1 ms sooner), their image streams
    // a few percent larger; they decode to the same pixels.
    bool fastImages = false;
    // PDF/A-3b (ISO 19005-3): every font embedded, so all text takes a TrueType or OpenType font
    // (formulas their own embedded fonts); an sRGB output intent, XMP metadata and a file identifier.
    bool pdfa = false;
    // PDF/UA-1 (ISO 14289-1), a tagged PDF for assistive technology: a structure tree of the
    // headings, paragraphs, lists, tables, figures (their alt text), links and notes in reading
    // order, decoration marked as artifacts, and, as with pdfa, every font embedded and formulas
    // as their TeX source. The language is the front matter's `lang`, else "en".
    bool pdfua = false;
    // Fenced code whose info string names a language GitHub knows (python, js, bash, ...) in
    // the colours of GitHub's light theme; false shows every code block in one colour.
    bool highlightCode = true;
    // A table of contents, with links and page numbers: where the document has a paragraph
    // "[TOC]" or "[[_TOC_]]" (which always gets one), else first, after the title heading.
    bool toc = false;
    // The heading levels the table lists, from its top one: 1 to 6.
    int tocDepth = 3;
    PdfTheme theme;
};

// Legacy error reporting is retained for the bool-returning compatibility
// overloads. New callers should use BuildPdf() and inspect BuildResult.
int GetLastError();

// Legacy raw-index options. New callers should use PdfOptions, PdfStyle, and
// PdfMargin so custom margins never need an integer encoding.
struct BuildOptions {
    int styleIdx = 0;
    int marginIdx = 1;
    std::string sourcePath;
    bool enableUrlImages = false;
    bool allowUnsafeLocalImages = false;
    bool embedSource = false;
};

BuildResult BuildPdf(const std::string& markdown, const PdfOptions& options, std::string& pdfBytes);

// A file of a book, or the title of a part of it on a page of its own (BuildBookPdf).
struct BookChapter {
    std::string markdown;   // the file's text; none for a part
    std::string path;       // the file (UTF-8), which its relative images and links to it resolve from
    // A part's title; a file's title in the book's list, put before it as a level-1 heading when
    // it does not begin with one, or none.
    std::string title;
    // The levels its bookmarks move down in the book: 0 at its top, 1 in a part or under the file
    // the list nests it in, and so on.
    int depth = 0;
    bool part = false;
};

struct Book {
    std::string title;      // on a page of its own first (not with a theme's cover), and the PDF's title
    std::string directory;  // local images must lie in it; with none, a book shows no local image
    std::vector<BookChapter> chapters;
};

// One PDF of a book's files, each from a new page. Each file is a document of its own, with
// its link definitions, footnotes and heading anchors; a link to another file of the book,
// "b.md" or "b.md#heading", goes to it. The bookmarks nest the files' headings under their
// parts, and a table of contents (options.toc, or the first "[TOC]" paragraph of any file)
// lists them all. options.sourcePath and options.embedSource do not apply: a book is not
// reversible.
BuildResult BuildBookPdf(const Book& book, const PdfOptions& options, std::string& pdfBytes);

// Compatibility entry points for the pre-typed C++ API. They intentionally
// keep their established success/error behavior while all in-tree callers use
// BuildPdf().
bool BuildPdfBytes(const std::string& markdown, const BuildOptions& options, std::string& pdfBytes);
bool BuildPdfBytes(const std::string& markdown, int styleIdx, int marginIdx, std::string& pdfBytes);

} // namespace TinyPdf
