// SPDX-License-Identifier: Apache-2.0

// The targets on the engine's own interfaces: BuildPdf, PdfSource::Inspect, ParseTheme, the
// CFF reader and Mermaid diagrams. Each aborts when an invariant the engine promises does not
// hold, so the fuzzer reports it as it reports a sanitizer's finding.

#include "fuzz_targets.h"

#include "rayomd/tiny_pdf.h"
#include "core/cff.h"
#include "core/export_options.h"
#include "core/mermaid.h"
#include "core/rayomd_pdf_source.h"
#include "core/svg.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace RayoMdFuzz {
namespace {

[[noreturn]] void Fail(const char* what) {
    std::fprintf(stderr, "rayomd-fuzz: %s\n", what);
    std::abort();
}

std::string ScratchFile(const char* name) {
    return ScratchDir() + "/" + name;
}

// Byte 0: bits 0-1 the style (mod 3), 2-3 the margin (compact, normal, wide, custom 18 pt), 4-6
// the page size (A4, A3, A5, Letter, Legal, 200 x 14400 pt, 14400 x 144 pt, A4 landscape),
// 7 a table of contents. Byte 1: bit 0 page numbers, 1 no highlighting, 2 PDF/A, 3 PDF/UA,
// 4 compress, 5 embed the source, 6 fast images, 7 a table of contents six levels deep.
TinyPdf::PdfOptions OptionsFrom(uint8_t first, uint8_t second) {
    TinyPdf::PdfOptions options;
    options.style = static_cast<TinyPdf::PdfStyle>((first & 3) % 3);
    switch ((first >> 2) & 3) {
    case 0: options.margin = TinyPdf::PdfMargin::Compact(); break;
    case 1: options.margin = TinyPdf::PdfMargin::Normal(); break;
    case 2: options.margin = TinyPdf::PdfMargin::Wide(); break;
    default: options.margin = TinyPdf::PdfMargin::CustomPoints(18.0); break;
    }
    switch ((first >> 4) & 7) {
    case 0: options.pageSize = TinyPdf::PdfPageSize::A4(); break;
    case 1: options.pageSize = TinyPdf::PdfPageSize::A3(); break;
    case 2: options.pageSize = TinyPdf::PdfPageSize::A5(); break;
    case 3: options.pageSize = TinyPdf::PdfPageSize::Letter(); break;
    case 4: options.pageSize = TinyPdf::PdfPageSize::Legal(); break;
    case 5: options.pageSize = { 200.0, 14400.0 }; break;
    case 6: options.pageSize = { 14400.0, 144.0 }; break;
    default: options.pageSize = { 842.0, 595.0 }; break;
    }
    options.toc = (first & 0x80) != 0;
    options.pageNumbers = (second & 1) != 0;
    options.highlightCode = (second & 2) == 0;
    options.pdfa = (second & 4) != 0;
    options.pdfua = (second & 8) != 0;
    options.compress = (second & 16) != 0;
    options.embedSource = (second & 32) != 0;
    options.fastImages = (second & 64) != 0;
    options.tocDepth = (second & 128) ? 6 : 3;
    options.sourcePath = ScratchFile("doc.md");
    return options;
}

void CheckPdf(const std::string& pdf) {
    if (pdf.compare(0, 5, "%PDF-") != 0) Fail("BuildPdf succeeded without a PDF header");
    if (pdf.size() < 7 || pdf.compare(pdf.size() - 7, 7, "\n%%EOF\n") != 0) Fail("BuildPdf succeeded without %%EOF");
}

// A build with `options` that must give a PDF unless the source is invalid for it.
void Build(const std::string& markdown, const TinyPdf::PdfOptions& options) {
    std::string pdf;
    const TinyPdf::BuildResult result = TinyPdf::BuildPdf(markdown, options, pdf);
    if (!result.Ok()) {
        if (options.embedSource && result.error == TinyPdf::BuildError::InvalidSourceUtf8) return;
        if (!options.theme.fontPath.empty() && result.error == TinyPdf::BuildError::ThemeFontUnavailable) return;
        // A system without a TrueType or OpenType font cannot make PDF/A or PDF/UA.
        if ((options.pdfa || options.pdfua) && result.error == TinyPdf::BuildError::PdfaFontUnavailable) return;
        std::fprintf(stderr, "rayomd-fuzz: BuildPdf error %d\n", static_cast<int>(result.error));
        std::abort();
    }
    CheckPdf(pdf);
    if (result.pages == 0) Fail("BuildPdf made a PDF of no pages");
    // The reversible profile: the source comes back byte for byte.
    if (options.embedSource) {
        const RayoMd::PdfSource::Result recovered = RayoMd::PdfSource::Inspect(pdf, true);
        if (!recovered.Ok()) {
            std::fprintf(stderr, "rayomd-fuzz: Inspect of an embedded source: %s\n",
                RayoMd::PdfSource::StatusMessage(recovered.status));
            std::abort();
        }
        if (recovered.source != markdown) Fail("the recovered source differs from the document");
    }
}

} // namespace

// Byte 0 and 1 choose the options of OptionsFrom; the options added since are chosen by the
// document's last byte, which stays in the document, so earlier seeds keep their text. Its bits
// 0-2: the page-break level (mod 7); bits 3-5: the heading numbers' depth (mod 7); bit 6:
// Obsidian's dialect; bit 7: diagrams off (```mermaid blocks as code). Byte 2, the document's
// first byte (read, not taken from it, so earlier seeds keep their text): bit 2 a long page
// (PdfPageSize::autoHeight) of byte 0's width.
int BuildPdf(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    const std::string markdown(reinterpret_cast<const char*>(data) + 2, size - 2);
    TinyPdf::PdfOptions options = OptionsFrom(data[0], data[1]);
    const uint8_t extra = size > 2 ? data[2] : 0;
    options.pageSize.autoHeight = (extra & 4) != 0;
    const uint8_t last = data[size - 1];
    options.pageBreakBeforeLevel = (last & 7) % 7;
    options.numberHeadings = ((last >> 3) & 7) % 7;
    if ((last & 0x40) != 0) options.dialect = TinyPdf::MarkdownDialect::Obsidian;
    if ((last & 0x80) != 0) options.diagrams = false;
    Build(markdown, options);
    return 0;
}

int PdfSource(const uint8_t* data, size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    const RayoMd::PdfSource::Result recovered = RayoMd::PdfSource::Inspect(bytes, true);
    const RayoMd::PdfSource::Result inspected = RayoMd::PdfSource::Inspect(bytes, false);
    if (recovered.Ok()) {
        if (!inspected.Ok()) Fail("Inspect recovers a source it does not accept without recovering");
        if (!recovered.info.digestValid) Fail("Inspect recovered a source whose digest does not match");
        if (!RayoMd::PdfSource::IsValidUtf8(recovered.source)) Fail("Inspect recovered a source that is not UTF-8");
        if (recovered.source.size() != recovered.info.sourceBytes) Fail("Inspect's sourceBytes is not the source's size");
    }
    if (RayoMd::PdfSource::StatusMessage(recovered.status) == nullptr) Fail("no status message");
    if (size <= 4096 && RayoMd::PdfSource::Sha256Hex(bytes) != RayoMd::PdfSource::Sha256HexPortable(bytes)) {
        Fail("SHA-256 differs between the SHA-NI and the portable code");
    }
    return 0;
}

int Theme(const uint8_t* data, size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    TinyPdf::PdfOptions options;
    options.sourcePath = ScratchFile("doc.md");
    options.pageNumbers = (size & 1) != 0;
    std::string error;
    if (!TinyPdf::Internal::ParseTheme(text, ScratchDir(), options.theme, error)) {
        if (error.empty()) Fail("ParseTheme failed without an error");
        return 0;
    }
    // The files a theme names are the user's own configuration: the paths the fuzzer writes stay
    // in the scratch folder's fonts and images (or a missing file), a fixed set, as the engine
    // keeps each theme font it loads for the life of the process.
    static const char* const kFonts[] = { "tiny.ttf", "tiny.otf", "missing.ttf" };
    static const char* const kLogos[] = { "rgba.png", "rgb.png", "gray.jpg", "color.jpg", "basic.svg", "missing.png" };
    const auto pick = [](const std::string& path, const char* const* names, size_t count) {
        size_t hash = 0;
        for (const char ch : path) hash = hash * 31 + static_cast<unsigned char>(ch);
        return ScratchFile(names[hash % count]);
    };
    for (std::string* font : { &options.theme.fontPath, &options.theme.fontBoldPath, &options.theme.fontItalicPath,
             &options.theme.fontBoldItalicPath }) {
        if (!font->empty()) *font = pick(*font, kFonts, 3);
    }
    if (!options.theme.logoPath.empty()) options.theme.logoPath = pick(options.theme.logoPath, kLogos, 6);
    Build("---\ntitle: A theme\nauthor: Fuzz\nsubject: Themes\ndate: 2026-10-15\n---\n\n# Heading\n\n"
          "Text with **bold**, *italic* and ***both***, a [link](https://example.com).\n\n> A quote\n\n"
          "\\newpage\n\nSecond page with `code`.\n",
        options);
    return 0;
}

int Cff(const uint8_t* data, size_t size) {
    using namespace TinyPdf::Internal;
    if (size < 2) return 0;
    const uint16_t glyphCount = static_cast<uint16_t>(data[0] << 8 | data[1]);
    Cff::CffFont font;
    if (!Cff::ParseCff(data + 2, size - 2, glyphCount, font)) return 0;
    if (font.glyphCount != glyphCount) Fail("ParseCff kept another glyph count");
    std::string program;
    std::vector<Cff::CidGlyph> glyphs;
    const uint16_t shown = glyphCount < 64 ? glyphCount : 64;
    for (uint16_t glyph = 0; glyph < shown; glyph++) {
        if (font.FdFor(glyph) >= font.fds.size() && !font.fds.empty()) Fail("FdFor past the font DICTs");
        program.clear();
        Cff::Desubroutinize(font, glyph, program);
        if (glyph > 0) glyphs.push_back({ static_cast<uint16_t>(glyph * 3), glyph });
    }
    std::string subset;
    if (Cff::WriteCidKeyedSubset(font, "RayoMDFuzz", glyphs, subset)) {
        // A subset is a CFF font of its own, of .notdef and those glyphs.
        Cff::CffFont reread;
        if (!Cff::ParseCff(reinterpret_cast<const uint8_t*>(subset.data()), subset.size(),
                static_cast<uint16_t>(glyphs.size() + 1), reread)) {
            Fail("the engine cannot read the CFF subset it wrote");
        }
    }
    return 0;
}

namespace {

// Text 0.6 em a character: the layout's measure without a font.
class FixedWidthHost : public TinyPdf::Internal::SvgHost {
public:
    double TextWidth(std::string_view text, double size, bool) override {
        size_t characters = 0;
        for (char ch : text) characters += (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
        return 0.6 * size * static_cast<double>(characters);
    }
    void AppendText(std::string& out, std::string_view text, bool) override {
        out += '(';
        for (char ch : text) {
            if (ch == '(' || ch == ')' || ch == '\\') out += '\\';
            out += ch;
        }
        out += ')';
    }
    bool HasBoldFace() const override { return false; }
    int AddImage(std::string_view, int&, int&) override { return -1; }
};

} // namespace

int Mermaid(const uint8_t* data, size_t size) {
    const std::string_view source(reinterpret_cast<const char*>(data), size);
    FixedWidthHost host;
    std::string svg;
    if (!TinyPdf::Internal::Mermaid::ToSvg(source, host, TinyPdf::Internal::Mermaid::kDefaultPalette, svg)) {
        if (!svg.empty()) Fail("a refused Mermaid diagram left an SVG");
        return 0;
    }
    TinyPdf::Internal::SvgForm form;
    if (!TinyPdf::Internal::ConvertSvg(svg, host, form) || form.content.empty()) Fail("the SVG of a Mermaid diagram does not convert");
    return 0;
}

} // namespace RayoMdFuzz
