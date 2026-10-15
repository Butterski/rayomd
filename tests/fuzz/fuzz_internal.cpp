// SPDX-License-Identifier: Apache-2.0

// The targets on code tiny_pdf.cpp keeps to itself (its font loader and subsetter, its image
// decoding and its SVG hosts): that file is compiled here, inside this translation unit, instead
// of on its own, so these functions see its internal names. Nothing in the engine changes for it.
//
// The font loader maps its file where it can; here it reads it into a buffer of exactly the file's
// size instead, so AddressSanitizer sees a read past its end.

#include <sys/mman.h>
#define mmap(address, length, protection, flags, file, offset) \
    ((void)(address), (void)(length), (void)(protection), (void)(flags), (void)(file), (void)(offset), MAP_FAILED)
#include "core/tiny_pdf.cpp"
#undef mmap

#include "fuzz_targets.h"

#include <charconv>

namespace RayoMdFuzz {
namespace {

[[noreturn]] void Fail(const char* what) {
    std::fprintf(stderr, "rayomd-fuzz: %s\n", what);
    std::abort();
}

// The number after `key` in a PDF dictionary's text, or -1.
long DictNumber(std::string_view dict, std::string_view key) {
    const size_t at = dict.find(key);
    if (at == std::string_view::npos) return -1;
    size_t from = at + key.size();
    while (from < dict.size() && dict[from] == ' ') from++;
    long value = -1;
    std::from_chars(dict.data() + from, dict.data() + dict.size(), value);
    return value;
}

// A FlateDecode image stream with PNG predictors inflates to exactly its rows: one filter byte
// and the row's samples for each of `height` rows.
void CheckPredictorStream(const std::string& stream, const std::string& parms, int height, const char* what) {
    const long columns = DictNumber(parms, "/Columns");
    const long colors = DictNumber(parms, "/Colors");
    const long bits = DictNumber(parms, "/BitsPerComponent");
    if (columns <= 0 || colors <= 0 || bits <= 0 || height <= 0) Fail(what);
    const size_t rowBytes = 1 + (static_cast<size_t>(columns) * static_cast<size_t>(colors) * static_cast<size_t>(bits) + 7) / 8;
    const size_t total = rowBytes * static_cast<size_t>(height);
    if (total > (size_t(256) << 20)) return;
    RayoMd::Inflate::ZlibReader reader;
    const auto* bytes = reinterpret_cast<const uint8_t*>(stream.data());
    if (!reader.Start(bytes, stream.size(), total, rowBytes)) Fail(what);
    for (int row = 0; row < height; row++) {
        const uint8_t* taken = reader.Take(rowBytes);
        if (taken == nullptr || taken[0] > 4) Fail(what);
    }
    if (!reader.Finish()) Fail(what);
}

void CheckImage(const std::vector<uint8_t>& bytes, const TinyPdf::PdfImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.stream.empty()) Fail("a decoded image without size or data");
    if (image.filter == "/DCTDecode") {
        if (image.stream.size() != bytes.size() || memcmp(image.stream.data(), bytes.data(), bytes.size()) != 0) {
            Fail("a JPEG is not passed through as it is");
        }
    } else if (image.decodeParms.find("/Predictor 15") != std::string::npos) {
        // A PNG the engine passes through keeps its own IDAT data, as valid as the file's; those it
        // decodes are written by the engine's encoder, which must make their rows exactly.
        TinyPdf::PngInfo png;
        TinyPdf::PdfImage passed;
        if (!(TinyPdf::ParsePngChunks(bytes, png) && TinyPdf::BuildFastPngImage(png, bytes, passed))) {
            CheckPredictorStream(image.stream, image.decodeParms, image.height, "an image stream does not inflate to its rows");
        }
    }
    if (!image.maskStream.empty() && image.maskDecodeParms.find("/Predictor 15") != std::string::npos) {
        CheckPredictorStream(image.maskStream, image.maskDecodeParms, image.height, "a soft mask does not inflate to its rows");
    }
    (void)BuildImageStreamDict(image, image.maskStream.empty() ? 0 : 7);
}

constexpr const char kFontText[] =
    "# Font héading ÄÖÜ\n\n"
    "Plain ASCII text, **bold**, *italic*, ***both*** and `code`: The quick brown fox jumps over the lazy dog 0123456789.\n\n"
    "Latin-1 and more: àéîõü ß Œ œ Ÿ € ‰ “quotes” — dashes – • ellipsis…\n\n"
    "Greek αβγδ ΩΣ, Cyrillic Привет, Hebrew שלום, Arabic مرحبا, CJK 中文字 かな 한글, emoji 😀 ✅.\n\n"
    "| Column | Другой |\n|---|---|\n| ﬁ ligature | ẞ |\n\n"
    "- [ ] task ☐\n- [x] done ☑\n\n"
    "Combining: e\xCC\x81 a\xCC\x8A, zero width: a\xE2\x80\x8B" "b, private use \xEE\x80\x80.\n";

} // namespace

int Font(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    // Fonts are keyed by address in the engine's caches of subset font objects: a ring of fonts,
    // the next one taken after each build, keeps a font's objects from standing in for another's.
    static std::vector<std::unique_ptr<TinyPdf::TtfFont>> ring;
    static size_t next = 0;
    if (ring.empty()) {
        for (int index = 0; index < 256; index++) ring.push_back(std::make_unique<TinyPdf::TtfFont>());
    }
    const std::string path = ScratchDir() + "/input-font";
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) Fail("cannot write the font file");
    const bool written = std::fwrite(data, 1, size, file) == size;
    if (std::fclose(file) != 0 || !written) Fail("cannot write the font file");

    TinyPdf::TtfFont& font = *ring[next];
    if (!font.TryLoad(path)) return 0;
    next = (next + 1) % ring.size();

    TinyPdf::PdfOptions options;
    options.sourcePath = ScratchDir() + "/doc.md";
    std::string pdf;
    size_t missing = 0;
    TinyPdf::BuildResult stats;
    if (!TinyPdf::BuildUnicodePdfBytes(kFontText, font, options, pdf, nullptr, missing, stats)) {
        Fail("a build in a loaded font failed");
    }
    // The font as a theme's every face, compressed, as PDF/A: the faces' own paths.
    options.compress = true;
    options.pdfa = true;
    const TinyPdf::ThemeFaces faces{ &font, &font, &font };
    pdf.clear();
    if (!TinyPdf::BuildUnicodePdfBytes(kFontText, font, options, pdf, nullptr, missing, stats, nullptr, &faces)) {
        Fail("a PDF/A build in a loaded font failed");
    }
    return 0;
}

int Image(const uint8_t* data, size_t size) {
    if (size == 0 || size > TinyPdf::kMaxImageBytes) return 0;
    const std::vector<uint8_t> bytes(data, data + size);
    for (const bool fast : { false, true }) {
        TinyPdf::PdfImage image;
        if (TinyPdf::DecodeImageBytes(bytes, image, std::string(), fast)) CheckImage(bytes, image);
    }
    return 0;
}

int Svg(const uint8_t* data, size_t size) {
    const std::string_view svg(reinterpret_cast<const char*>(data), size);
    TinyPdf::Internal::LooksLikeSvg(svg);
    TinyPdf::PdfOptions options;
    options.sourcePath = ScratchDir() + "/doc.md";
    // The two hosts BuildPdf's renderers give ConvertSvg (ImageRegistry::ConvertSvgImage).
    {
        TinyPdf::ImageRegistry registry(options);
        TinyPdf::StandardSvgHost host(registry);
        TinyPdf::Internal::SvgForm form;
        TinyPdf::Internal::ConvertSvg(svg, host, form);
    }
    if (const TinyPdf::TtfFont* font = TinyPdf::GetCachedFont()) {
        TinyPdf::ImageRegistry registry(options);
        TinyPdf::KeptCidSet cids;
        TinyPdf::UnicodeSvgHost host(*font, cids.Get(), registry, font, &cids.Get());
        TinyPdf::Internal::SvgForm form;
        if (TinyPdf::Internal::ConvertSvg(svg, host, form) && form.content.empty() && form.images.empty()) {
            Fail("an SVG converted to a form that draws nothing");
        }
    }
    return 0;
}

} // namespace RayoMdFuzz
