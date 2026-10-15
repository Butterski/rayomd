// SPDX-License-Identifier: Apache-2.0

// Needs <atomic>, <chrono>, <filesystem>, <functional>, <iostream>, <string>, <thread>, <vector>,
// rayomd/tiny_pdf.h, src/common/file_io.h and src/core/flate.h; on POSIX <fcntl.h>, <sys/stat.h>
// and <unistd.h>.
// Hostile and abandoned builds: cancellation (PdfOptions::cancel), files that are no regular
// files, fonts changed after they were read, deep composite glyphs, image containment, the
// decode budget and the image cache's permissions. Included inside the anonymous namespace of
// tests/core_tests.cpp.

namespace hardening_tests {

namespace fs = std::filesystem;

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t at = 0; at < size; at++) {
        crc ^= data[at];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void AppendBigEndian(std::string& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back((char)(value >> shift));
}

void AppendChunk(std::string& png, const char* type, const std::string& data) {
    AppendBigEndian(png, (uint32_t)data.size());
    const size_t start = png.size();
    png.append(type, 4);
    png += data;
    AppendBigEndian(png, Crc32(reinterpret_cast<const uint8_t*>(png.data() + start), png.size() - start));
}

// An 8-bit RGBA PNG of `width` x `height` pixels, filter 0 on every row, each byte `pixel(x, y, c)`.
template <typename Pixel>
std::string MakeRgbaPng(uint32_t width, uint32_t height, Pixel pixel) {
    std::string raw;
    raw.reserve((size_t)(width * 4 + 1) * height);
    for (uint32_t y = 0; y < height; y++) {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; x++) {
            for (int c = 0; c < 4; c++) raw.push_back((char)pixel(x, y, c));
        }
    }
    std::string zlib(RayoMd::Flate::ZlibBound(raw.size()), '\0');
    RayoMd::Flate::Scratch scratch;
    zlib.resize(RayoMd::Flate::ZlibCompress(raw, reinterpret_cast<uint8_t*>(&zlib[0]), scratch));
    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string header;
    AppendBigEndian(header, width);
    AppendBigEndian(header, height);
    header += std::string("\x08\x06\x00\x00\x00", 5);
    AppendChunk(png, "IHDR", header);
    AppendChunk(png, "IDAT", zlib);
    AppendChunk(png, "IEND", std::string());
    return png;
}

bool WriteFile(const fs::path& path, const std::string& bytes) {
    return RayoMd::File::WriteAll(path, bytes);
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// A build abandoned through PdfOptions::cancel: before it starts, it returns Cancelled with no
// PDF (also a book, and GetLastError says so); while it runs, set from another thread, it returns
// within a small part of the whole build's time from many blocks, a long table, a long code block,
// a large PNG and a book of many files. A cancelled decode is not remembered as a failed image:
// the same document built again afterwards shows it. A flag that stays false changes no byte.
bool CheckCancel() {
    const fs::path root = fs::temp_directory_path() / "rayomd-test-cancel";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string png = MakeRgbaPng(2600, 2600, [](uint32_t x, uint32_t y, int c) {
        uint32_t v = x * 2654435761u ^ y * 40503u ^ (uint32_t)c * 977u;
        v ^= v >> 13;
        return c == 3 ? (uint8_t)(128 + (v & 63)) : (uint8_t)(v & 31);
    });
    if (!WriteFile(root / "big.png", png)) {
        std::cerr << "cannot write the cancel test's PNG" << std::endl;
        return false;
    }

    TinyPdf::PdfOptions options;
    options.sourcePath = (root / "doc.md").u8string();
    std::atomic<bool> cancel{ true };
    options.cancel = &cancel;
    std::string pdf = "old bytes";
    const TinyPdf::BuildResult early = TinyPdf::BuildPdf("# Title\n\nText.\n", options, pdf);
    TinyPdf::Book book;
    book.title = "Book";
    for (int file = 0; file < 3; file++) book.chapters.push_back({ "# File\n\nText.\n", (root / "f.md").u8string(), "", 0, false });
    std::string bookPdf = "old bytes";
    const TinyPdf::BuildResult earlyBook = TinyPdf::BuildBookPdf(book, options, bookPdf);
    if (early.error != TinyPdf::BuildError::Cancelled || !pdf.empty() || TinyPdf::GetLastError() != 9 ||
        earlyBook.error != TinyPdf::BuildError::Cancelled || !bookPdf.empty()) {
        std::cerr << "a cancelled build made a PDF or no Cancelled error" << std::endl;
        return false;
    }
    cancel = false;
    std::string withFlag, without;
    TinyPdf::PdfOptions plain = options;
    plain.cancel = nullptr;
    if (!TinyPdf::BuildPdf("# Title\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n```\ncode\n```\n", options, withFlag).Ok() ||
        !TinyPdf::BuildPdf("# Title\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n```\ncode\n```\n", plain, without).Ok() ||
        withFlag != without) {
        std::cerr << "a cancel flag that stays false changed the PDF" << std::endl;
        return false;
    }

    std::string blocks, table = "| a | b | c |\n|---|---|---|\n", code = "```\n", bookText;
    for (int i = 0; i < 60000; i++) blocks += "Paragraph " + std::to_string(i) + " of many blocks, with **bold** text.\n\n";
    for (int i = 0; i < 40000; i++) table += "| row " + std::to_string(i) + " | *cell* | `x` |\n";
    for (int i = 0; i < 300000; i++) code += "int line" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    code += "```\n";
    for (int i = 0; i < 400; i++) bookText += "Paragraph " + std::to_string(i) + " of a file.\n\n";
    TinyPdf::Book longBook;
    for (int file = 0; file < 400; file++) longBook.chapters.push_back({ bookText, (root / "f.md").u8string(), "", 0, false });
    struct Case {
        const char* name;
        std::string markdown;   // "{}" stands for the image's number
        const TinyPdf::Book* book;
    };
    const Case cases[] = {
        { "blocks", blocks, nullptr },
        { "table", table, nullptr },
        { "code", code, nullptr },
        { "image", "![big](big{}.png)\n", nullptr },
        { "book", std::string(), &longBook },
    };
    bool ok = true;
    for (const Case& item : cases) {
        // A copy of the PNG for each build: the process keeps decoded images by path.
        const auto build = [&](TinyPdf::PdfOptions& buildOptions, std::string& out, int copy) {
            if (item.book != nullptr) return TinyPdf::BuildBookPdf(*item.book, buildOptions, out);
            std::string markdown = item.markdown;
            const size_t at = markdown.find("{}");
            if (at != std::string::npos) {
                fs::copy_file(root / "big.png", root / ("big" + std::to_string(copy) + ".png"), fs::copy_options::overwrite_existing);
                markdown.replace(at, 2, std::to_string(copy));
            }
            return TinyPdf::BuildPdf(markdown, buildOptions, out);
        };
        // The whole build first, then one cancelled a third of the way, past the parse, which does
        // not stop; then the cancelled document again, whose image must not be taken for a failed
        // one. A try is inconclusive when the build ended before the flag (Windows sleeps whole
        // timer ticks of 15.6 ms) or a busy machine stalled its thread past the bound: up to three.
        bool passed = false;
        for (int attempt = 0; attempt < 3 && !passed; attempt++) {
            std::string full;
            const auto start = std::chrono::steady_clock::now();
            const TinyPdf::BuildResult whole = build(plain, full, 1);
            const double fullMs = MillisecondsSince(start);
            cancel = false;
            TinyPdf::BuildResult cancelled;
            std::string cancelledPdf;
            std::thread worker([&]() { cancelled = build(options, cancelledPdf, 2); });
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(fullMs / 3.0));
            const auto set = std::chrono::steady_clock::now();
            cancel = true;
            worker.join();
            const double latency = MillisecondsSince(set);
            std::string again;
            const TinyPdf::BuildResult after = build(plain, again, 2);
            std::cout << "cancel " << item.name << ": returned " << latency << " ms after the flag, the whole build "
                      << fullMs << " ms" << std::endl;
            if (!whole.Ok() || whole.failedImages != 0 || !ValidPdf(full) || !after.Ok() || after.failedImages != 0 ||
                (cancelled.error != TinyPdf::BuildError::Cancelled && !cancelled.Ok()) ||
                (cancelled.error == TinyPdf::BuildError::Cancelled && !cancelledPdf.empty())) {
                break;
            }
            passed = cancelled.error == TinyPdf::BuildError::Cancelled && latency <= std::max(25.0, fullMs / 5.0);
        }
        if (!passed) {
            std::cerr << "cancelling the " << item.name << " build failed" << std::endl;
            ok = false;
        }
    }
    fs::remove_all(root);
    return ok;
}

// A FIFO, which blocks whoever opens it for reading until a writer comes, as a document's image, a
// theme's font and logo, and through RayoMd::File::ReadAll and Open: each read fails at once, as
// for any file that is not a regular file (a directory, a device), instead of waiting.
bool CheckSpecialFiles() {
#ifdef _WIN32
    return true;
#else
    const fs::path root = fs::temp_directory_path() / "rayomd-test-special";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path fifo = root / "pipe.png";
    const fs::path fontFifo = root / "pipe.ttf";
    if (::mkfifo(fifo.c_str(), 0600) != 0 || ::mkfifo(fontFifo.c_str(), 0600) != 0) {
        std::cerr << "mkfifo failed" << std::endl;
        return false;
    }
    bool ok = true;
    // Runs `step` on a thread; one still running after 5 s waits on the FIFO, which a writer
    // opened here then releases.
    const auto quickly = [&](const char* what, const std::function<bool()>& step) {
        std::atomic<bool> done{ false };
        bool passed = false;
        std::thread worker([&]() { passed = step(); done = true; });
        const auto start = std::chrono::steady_clock::now();
        while (!done && MillisecondsSince(start) < 5000.0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!done) {
            for (const fs::path& path : { fifo, fontFifo }) {
                const int writer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
                if (writer >= 0) ::close(writer);
            }
            std::cerr << what << " waited on a FIFO" << std::endl;
        }
        worker.join();
        if (!passed) std::cerr << what << " did not fail as a file that is no regular file" << std::endl;
        ok = ok && done && passed;
    };
    TinyPdf::PdfOptions options;
    options.sourcePath = (root / "doc.md").u8string();
    quickly("an image", [&]() {
        std::string pdf;
        const TinyPdf::BuildResult result = TinyPdf::BuildPdf("![pipe](pipe.png)\n", options, pdf);
        return result.Ok() && result.failedImages == 1;
    });
    quickly("a theme's logo", [&]() {
        TinyPdf::PdfOptions themed = options;
        themed.theme.logoPath = fifo.u8string();
        themed.theme.headerLeft = "{logo}";
        std::string pdf;
        const TinyPdf::BuildResult result = TinyPdf::BuildPdf("# Logo\n", themed, pdf);
        return result.Ok() && result.failedImages == 1;
    });
    quickly("a theme's font", [&]() {
        TinyPdf::PdfOptions themed = options;
        themed.theme.fontPath = fontFifo.u8string();
        std::string pdf;
        return TinyPdf::BuildPdf("# Font\n", themed, pdf).error == TinyPdf::BuildError::ThemeFontUnavailable;
    });
    quickly("ReadAll and Open", [&]() {
        std::string text;
        std::FILE* file = RayoMd::File::Open(fifo, "rb");
        if (file) std::fclose(file);
        return !RayoMd::File::ReadAll(fifo, 1024, text) && file == nullptr && !RayoMd::File::ReadAll(root, 1024, text) &&
            !RayoMd::File::ReadAll("/dev/zero", 1024, text);
    });
    // Close-on-exec, so no descriptor leaks into a child process.
    std::FILE* written = RayoMd::File::Open(root / "out.txt", "wb");
    std::FILE* read = written ? (std::fclose(written), RayoMd::File::Open(root / "out.txt", "rb")) : nullptr;
    if (!read || (::fcntl(::fileno(read), F_GETFD) & FD_CLOEXEC) == 0) {
        std::cerr << "RayoMD::File::Open left a descriptor inheritable" << std::endl;
        ok = false;
    }
    if (read) std::fclose(read);
    written = RayoMd::File::Open(root / "out.txt", "wb");
    if (!written || (::fcntl(::fileno(written), F_GETFD) & FD_CLOEXEC) == 0) {
        std::cerr << "RayoMD::File::Open left a descriptor inheritable" << std::endl;
        ok = false;
    }
    if (written) std::fclose(written);
    fs::remove_all(root);
    return ok;
#endif
}

uint32_t BigEndianAt(const std::string& bytes, size_t at, int size) {
    uint32_t value = 0;
    for (int i = 0; i < size; i++) value = value << 8 | (uint8_t)bytes[at + (size_t)i];
    return value;
}

// The offset and length of table `tag` in a single sfnt font, false without it.
bool FindTable(const std::string& font, const char* tag, uint32_t& offset, uint32_t& length) {
    const uint32_t tables = BigEndianAt(font, 4, 2);
    for (uint32_t i = 0; i < tables && 12 + (i + 1) * 16 <= font.size(); i++) {
        if (font.compare(12 + i * 16, 4, tag) == 0) {
            offset = BigEndianAt(font, 12 + i * 16 + 8, 4);
            length = BigEndianAt(font, 12 + i * 16 + 12, 4);
            return (size_t)offset + length <= font.size();
        }
    }
    return false;
}

// The system's TrueType font for tests, empty when it has none.
std::string TestFontPath() {
#ifdef _WIN32
    const char* windows = std::getenv("WINDIR");
    const std::string path = windows ? std::string(windows) + "\\Fonts\\arial.ttf" : std::string();
#else
    const std::string path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
    std::error_code error;
    return !path.empty() && fs::is_regular_file(path, error) ? path : std::string();
}

// A theme font is mapped, not copied, so a file changed in place while a build uses it changes
// what the build reads: loca entries that now point past glyf (and past the file) must not be
// followed when the font is subset; the next build loads the changed file again (and fails, its
// loca being out of range), and the one after, the file put back, loads it again too.
bool CheckFontChangedInPlace() {
    const std::string source = TestFontPath();
    if (source.empty()) return true;
    const fs::path root = fs::temp_directory_path() / "rayomd-test-changed-font";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path path = root / "theme.ttf";
    std::string original;
    uint32_t head = 0, headLength = 0, loca = 0, locaLength = 0;
    if (!RayoMd::File::ReadAll(source, 64u << 20, original) || !FindTable(original, "head", head, headLength) ||
        !FindTable(original, "loca", loca, locaLength) || headLength < 54 || !WriteFile(path, original)) {
        std::cerr << "cannot prepare the changed-font test" << std::endl;
        return false;
    }
    std::string corrupt = original;
    const int entry = BigEndianAt(original, head + 50, 2) == 0 ? 2 : 4;
    for (size_t at = loca, index = 0; at + (size_t)entry <= (size_t)loca + locaLength; at += (size_t)entry, index++) {
        const uint32_t value = index % 2 == 0 ? 0 : (entry == 2 ? 0xFFFFu : 0x7FFFFFF0u);
        for (int i = 0; i < entry; i++) corrupt[at + (size_t)i] = (char)(value >> (8 * (entry - 1 - i)));
    }
    std::string document;
    for (int i = 0; i < 20000; i++) document += u8"Zażółć gęślą jaźń, paragraph " + std::to_string(i) + ".\n\n";
    TinyPdf::PdfOptions options;
    options.theme.fontPath = path.u8string();
    std::string pdf;
    const auto start = std::chrono::steady_clock::now();
    bool ok = TinyPdf::BuildPdf(document, options, pdf).Ok() && ValidPdf(pdf);
    const double fullMs = MillisecondsSince(start);
    // Built again with the font loaded above; the file is rewritten in place while it lays out.
    std::string changedPdf;
    std::thread worker([&]() { TinyPdf::BuildPdf(document, options, changedPdf); });
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(fullMs / 3.0));
    ok = RayoMd::File::WriteAll(path, corrupt) && ok;
    worker.join();
    std::string after;
    ok = ok && TinyPdf::BuildPdf("# Text\n", options, after).error == TinyPdf::BuildError::ThemeFontUnavailable;
    ok = ok && RayoMd::File::WriteAll(path, original) && TinyPdf::BuildPdf("# Text\n", options, after).Ok() && ValidPdf(after);
    if (!ok) std::cerr << "a theme font changed in place was not read again as it now is" << std::endl;
    fs::remove_all(root);
    return ok;
}

void AppendBig16(std::string& out, uint32_t value) {
    out.push_back((char)(value >> 8));
    out.push_back((char)value);
}

// A TrueType font of 65,535 glyphs in which 'A' (glyph 1) is a composite of glyph 2, which is one
// of glyph 3, and so on to the last, an empty glyph that is also the space.
std::string MakeCompositeChainFont() {
    const uint32_t glyphs = 65535;
    std::string glyf, loca, hmtx, cmap, head(54, '\0'), hhea(36, '\0'), maxp;
    for (uint32_t glyph = 0; glyph < glyphs; glyph++) {
        AppendBigEndian(loca, (uint32_t)glyf.size());
        if (glyph == 0 || glyph + 1 == glyphs) continue;      // .notdef and the last: empty
        AppendBig16(glyf, 0xFFFF);                           // numberOfContours -1: composite
        for (int i = 0; i < 4; i++) AppendBig16(glyf, 0);    // bounding box
        AppendBig16(glyf, 0x0002);                           // ARGS_ARE_XY_VALUES, bytes, no more
        AppendBig16(glyf, glyph + 1);
        AppendBig16(glyf, 0);                                // two byte arguments
    }
    AppendBigEndian(loca, (uint32_t)glyf.size());
    AppendBig16(hmtx, 500);
    AppendBig16(hmtx, 0);
    AppendBig16(cmap, 0);                                    // version
    AppendBig16(cmap, 1);                                    // one subtable: Windows, UCS-4
    AppendBig16(cmap, 3);
    AppendBig16(cmap, 10);
    AppendBigEndian(cmap, 12);
    AppendBig16(cmap, 12);                                   // format 12
    AppendBig16(cmap, 0);
    AppendBigEndian(cmap, 16 + 2 * 12);
    AppendBigEndian(cmap, 0);
    AppendBigEndian(cmap, 2);
    for (const uint32_t group : { 0x20u, 0x41u }) {
        AppendBigEndian(cmap, group);
        AppendBigEndian(cmap, group);
        AppendBigEndian(cmap, group == 0x20u ? glyphs - 1 : 1);
    }
    head[0] = 0; head[1] = 1;                                // version 1.0
    head[18] = (char)(1000 >> 8); head[19] = (char)(1000 & 0xFF);   // unitsPerEm
    head[51] = 1;                                            // indexToLocFormat: long
    hhea[0] = 0; hhea[1] = 1;
    hhea[35] = 1;                                            // numberOfHMetrics
    AppendBigEndian(maxp, 0x00005000);
    AppendBig16(maxp, glyphs);
    const std::pair<const char*, const std::string*> tables[] = {
        { "cmap", &cmap }, { "glyf", &glyf }, { "head", &head }, { "hhea", &hhea }, { "hmtx", &hmtx },
        { "loca", &loca }, { "maxp", &maxp } };
    std::string font;
    AppendBigEndian(font, 0x00010000);
    AppendBig16(font, 7);
    for (int i = 0; i < 3; i++) AppendBig16(font, 0);
    uint32_t offset = 12 + 7 * 16;
    for (const auto& [tag, data] : tables) {
        font.append(tag, 4);
        AppendBigEndian(font, 0);
        AppendBigEndian(font, offset);
        AppendBigEndian(font, (uint32_t)data->size());
        offset += (uint32_t)(data->size() + 3) & ~3u;
    }
    for (const auto& table : tables) {
        font += *table.second;
        font.resize((font.size() + 3) & ~(size_t)3, '\0');
    }
    return font;
}

// A composite glyph nested 65,534 deep: subsetting the font for "A" stops at the depth limit
// instead of recursing down the chain (a stack overflow before), and the PDF is made.
bool CheckDeepCompositeGlyph() {
    const fs::path root = fs::temp_directory_path() / "rayomd-test-composite";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path path = root / "chain.ttf";
    bool ok = WriteFile(path, MakeCompositeChainFont());
    TinyPdf::PdfOptions options;
    options.theme.fontPath = path.u8string();
    std::string pdf;
    // On a thread with a 1 MiB stack, as Windows gives its threads (the main one has 8 MiB here).
    const std::function<void()> build = [&]() {
        const TinyPdf::BuildResult result = TinyPdf::BuildPdf("# A\n\nA A A\n", options, pdf);
        ok = ok && result.Ok() && ValidPdf(pdf);
    };
#ifdef _WIN32
    std::thread worker(build);
    worker.join();
#else
    pthread_attr_t attributes;
    pthread_t thread;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1u << 20);
    const auto run = [](void* function) -> void* {
        (*static_cast<const std::function<void()>*>(function))();
        return nullptr;
    };
    if (pthread_create(&thread, &attributes, run, const_cast<std::function<void()>*>(&build)) != 0) {
        ok = false;
    } else {
        pthread_join(thread, nullptr);
    }
    pthread_attr_destroy(&attributes);
#endif
    if (!ok) std::cerr << "a font of deeply nested composite glyphs failed the export" << std::endl;
    fs::remove_all(root);
    return ok;
}

// An image the policy found inside the document's folder is read only while it is still there:
// RayoMd::File::ReadCanonical refuses a canonical path one of whose directories became a link to
// elsewhere since it was worked out (the check-then-open race of AuditEngine F11); a document
// whose image directory is swapped back and forth while it is built never shows the outside file.
bool CheckImageContainment() {
#ifndef __linux__
    return true;
#else
    const fs::path root = fs::temp_directory_path() / "rayomd-test-containment";
    fs::remove_all(root);
    const fs::path doc = root / "doc";
    fs::create_directories(doc / "pics");
    fs::create_directories(root / "outside");
    const fs::path assets = fs::path(RAYOMD_TEST_SOURCE_DIR) / "docs" / "assets";
    fs::copy_file(assets / "branding" / "rayomd.png", doc / "pics" / "pic.png");
    fs::copy_file(assets / "demo" / "ui.png", root / "outside" / "pic.png");
    fs::create_directory_symlink(root / "outside", doc / "swap");
    const fs::path canonical = fs::canonical(doc / "pics" / "pic.png");
    std::vector<uint8_t> bytes;
    bool ok = RayoMd::File::ReadCanonical(canonical, 64u << 20, bytes) && !bytes.empty();
    // Swapped: the same canonical path now leads through a link.
    ok = ok && ::renameat2(AT_FDCWD, (doc / "pics").c_str(), AT_FDCWD, (doc / "swap").c_str(), RENAME_EXCHANGE) == 0;
    ok = ok && !RayoMd::File::ReadCanonical(canonical, 64u << 20, bytes);
    ok = ok && ::renameat2(AT_FDCWD, (doc / "pics").c_str(), AT_FDCWD, (doc / "swap").c_str(), RENAME_EXCHANGE) == 0;
    if (!ok) std::cerr << "a canonical path through a swapped-in link was read" << std::endl;
    // The race itself: builds while another thread keeps swapping the folder for the link.
    TinyPdf::PdfOptions options;
    options.sourcePath = (doc / "doc.md").u8string();
    std::string inside;
    ok = ok && TinyPdf::BuildPdf("![p](pics/pic.png)\n", options, inside).Ok() && inside.find("/Subtype /Image") != std::string::npos;
    std::atomic<bool> stop{ false };
    std::thread swapper([&]() {
        while (!stop) ::renameat2(AT_FDCWD, (doc / "pics").c_str(), AT_FDCWD, (doc / "swap").c_str(), RENAME_EXCHANGE);
    });
    size_t leaked = 0;
    for (int build = 0; build < 300; build++) {
        std::string pdf;
        TinyPdf::BuildPdf("![p](pics/pic.png)\n", options, pdf);
        leaked += pdf.find("/Subtype /Image") != std::string::npos && pdf != inside;
    }
    stop = true;
    swapper.join();
    if (leaked != 0) {
        std::cerr << "the outside image was read " << leaked << " times" << std::endl;
        ok = false;
    }
    fs::remove_all(root);
    return ok;
#endif
}

// The persistent image cache is its user's alone: the directory it creates 0700, each entry 0600.
bool CheckCachePermissions() {
#ifdef _WIN32
    return true;
#else
    const fs::path root = fs::temp_directory_path() / "rayomd-test-cache-perms";
    fs::remove_all(root);
    fs::create_directories(root / "doc");
    const std::string png = MakeRgbaPng(256, 256, [](uint32_t x, uint32_t y, int c) {
        return c == 3 ? (uint8_t)(x ^ y) : (uint8_t)(x * 7 + y * 3 + (uint32_t)c);
    });
    bool ok = WriteFile(root / "doc" / "alpha.png", png);
    TinyPdf::PdfOptions options;
    options.sourcePath = (root / "doc" / "doc.md").u8string();
    options.imageCacheDirectory = (root / "cache" / "images").u8string();
    std::string pdf;
    const ::mode_t mask = ::umask(022);
    ok = ok && TinyPdf::BuildPdf("![a](alpha.png)\n", options, pdf).Ok();
    ::umask(mask);
    struct stat info {};
    size_t entries = 0;
    ok = ok && ::stat((root / "cache" / "images").c_str(), &info) == 0 && (info.st_mode & 0777) == 0700;
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(root / "cache" / "images", error)) {
        if (!entry.is_regular_file()) continue;
        entries++;
        ok = ok && ::stat(entry.path().c_str(), &info) == 0 && (info.st_mode & 0777) == 0600;
    }
    if (!ok || entries == 0) std::cerr << "the image cache is readable by others" << std::endl;
    fs::remove_all(root);
    return ok && entries != 0;
#endif
}

// An 8-bit RGBA PNG of `width` x `height` transparent black pixels but the first, whose red is
// `mark`: a "bomb" of a few dozen KB that decodes to width * height * 4 bytes.
std::string MakeBombPng(uint32_t width, uint32_t height, uint8_t mark) {
    std::string raw((size_t)(width * 4 + 1) * height, '\0');
    raw[1] = (char)mark;
    std::string zlib(RayoMd::Flate::ZlibBound(raw.size()), '\0');
    RayoMd::Flate::Scratch scratch;
    zlib.resize(RayoMd::Flate::ZlibCompress(raw, reinterpret_cast<uint8_t*>(&zlib[0]), scratch));
    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string header;
    AppendBigEndian(header, width);
    AppendBigEndian(header, height);
    header += std::string("\x08\x06\x00\x00\x00", 5);
    AppendChunk(png, "IHDR", header);
    AppendChunk(png, "IDAT", zlib);
    AppendChunk(png, "IEND", std::string());
    return png;
}

std::string Base64(const std::string& bytes) {
    static const char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t at = 0; at < bytes.size(); at += 3) {
        const size_t left = bytes.size() - at;
        const uint32_t v = (uint32_t)(uint8_t)bytes[at] << 16 | (left > 1 ? (uint32_t)(uint8_t)bytes[at + 1] << 8 : 0u) |
            (left > 2 ? (uint32_t)(uint8_t)bytes[at + 2] : 0u);
        out += kDigits[v >> 18];
        out += kDigits[(v >> 12) & 63];
        out += left > 1 ? kDigits[(v >> 6) & 63] : '=';
        out += left > 2 ? kDigits[v & 63] : '=';
    }
    return out;
}

// An SVG of a red square and the PNG `png` as a data: image.
std::string SvgWithImage(const std::string& png) {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\"><rect width=\"5\" height=\"5\" fill=\"red\"/>"
        "<image x=\"5\" y=\"5\" width=\"10\" height=\"10\" href=\"data:image/png;base64," + Base64(png) + "\"/></svg>";
}

// The pixels one build may decode (AuditEngine F12): 2^30, which 64 PNGs of 4096 x 4096 fill. A
// document of 66 SVG files with the same such data: image and 30 copies of such a PNG decodes the
// image once (the process keeps it by its SHA-256, charging each use what its decode took), and
// shows neither the last two SVGs' image nor any copy, each a failed image, with no time spent
// decoding them; built again, it is the same PDF. An SVG's data: image is decoded once a process:
// a second build of its document decodes nothing and writes the same bytes.
bool CheckDecodeBudget() {
    const fs::path root = fs::temp_directory_path() / "rayomd-test-decode-budget";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string bomb = MakeBombPng(4096, 4096, 1);
    const std::string svg = SvgWithImage(bomb);
    std::string svgs, copies;
    bool ok = true;
    for (int i = 0; i < 66; i++) {
        ok = ok && WriteFile(root / ("s" + std::to_string(i) + ".svg"), svg);
        svgs += "![s](s" + std::to_string(i) + ".svg)\n\n";
    }
    for (int i = 0; i < 30; i++) {
        ok = ok && WriteFile(root / ("c" + std::to_string(i) + ".png"), bomb);
        copies += "![c](c" + std::to_string(i) + ".png)\n\n";
    }
    const std::string small = MakeRgbaPng(64, 48, [](uint32_t x, uint32_t y, int c) {
        return c == 3 ? (uint8_t)(x * 4) : (uint8_t)(x * 3 + y * 5 + (uint32_t)c * 40);
    });
    ok = ok && WriteFile(root / "small.svg", SvgWithImage(small));
    if (!ok) {
        std::cerr << "cannot write the decode budget test's files" << std::endl;
        return false;
    }
    TinyPdf::PdfOptions options;
    options.sourcePath = (root / "doc.md").u8string();

    const uint64_t before = TinyPdf::Internal::SvgImageDecodes();
    std::string first, again, withCopies;
    auto start = std::chrono::steady_clock::now();
    const TinyPdf::BuildResult spent = TinyPdf::BuildPdf(svgs, options, first);
    const double firstMs = MillisecondsSince(start);
    const uint64_t decodes = TinyPdf::Internal::SvgImageDecodes() - before;
    const TinyPdf::BuildResult spentAgain = TinyPdf::BuildPdf(svgs, options, again);
    start = std::chrono::steady_clock::now();
    const TinyPdf::BuildResult copied = TinyPdf::BuildPdf(svgs + copies, options, withCopies);
    const double copiesMs = MillisecondsSince(start);
    std::cout << "decode budget: 66 SVGs in " << firstMs << " ms, with 30 PNGs past the budget " << copiesMs << " ms"
              << std::endl;
    if (!spent.Ok() || spent.failedImages != 2 || decodes != 1 || !ValidPdf(first)) {
        std::cerr << "the decode budget: " << spent.failedImages << " failed images, " << decodes << " decodes" << std::endl;
        ok = false;
    }
    if (!spentAgain.Ok() || spentAgain.failedImages != 2 || again != first ||
        TinyPdf::Internal::SvgImageDecodes() - before != 1) {
        std::cerr << "a document past the decode budget changed when built again" << std::endl;
        ok = false;
    }
    // Decoded, the 30 copies would take 30 times the one decode of the first build.
    if (!copied.Ok() || copied.failedImages != 32 || copiesMs > 2.0 * firstMs + 50.0) {
        std::cerr << "PNGs past the decode budget: " << copied.failedImages << " failed images in " << copiesMs << " ms"
                  << std::endl;
        ok = false;
    }

    const uint64_t smallBefore = TinyPdf::Internal::SvgImageDecodes();
    std::string once, twice;
    const TinyPdf::BuildResult built = TinyPdf::BuildPdf("![s](small.svg)\n", options, once);
    const uint64_t firstDecodes = TinyPdf::Internal::SvgImageDecodes() - smallBefore;
    const TinyPdf::BuildResult rebuilt = TinyPdf::BuildPdf("![s](small.svg)\n", options, twice);
    if (!built.Ok() || !rebuilt.Ok() || built.failedImages != 0 || firstDecodes != 1 ||
        TinyPdf::Internal::SvgImageDecodes() - smallBefore != 1 || once != twice ||
        once.find("/Subtype /Image") == std::string::npos) {
        std::cerr << "an SVG's data: image was decoded again, or its PDF changed" << std::endl;
        ok = false;
    }
    fs::remove_all(root);
    return ok;
}

// Names that are no UTF-8 (AuditEngine F13): a document's image paths (raw and percent-encoded)
// and links, its source path, a theme's logo and fonts (also through ParseTheme), the files a
// book's SUMMARY.md lists and a book's links to its files; on Windows also a file name with an
// unpaired surrogate in a book's folder and a batch folder. std::filesystem's conversions threw
// for these on Windows, which ends a process built without exceptions (-fno-exceptions, and the
// GUI's build thread). Every build ends: an image shows its alt text, a theme font that cannot
// be read is ThemeFontUnavailable, a book that lists a missing file is refused.
bool CheckInvalidUtf8Names() {
    const fs::path root = fs::temp_directory_path() / "rayomd-test-names";
    fs::remove_all(root);
    fs::create_directories(root / "book");
    const fs::path assets = fs::path(RAYOMD_TEST_SOURCE_DIR) / "docs" / "assets";
    std::error_code copyError;
    fs::copy_file(assets / "branding" / "rayomd.png", root / "pic.png", copyError);
    const std::string bad = "\xFF\xFE\xC3x\xED\xA0\x80";   // stray bytes, a cut sequence, an encoded surrogate
    const std::string dir = root.u8string();
    bool ok = !copyError;
    const auto check = [&](const char* what, bool passed) {
        if (!passed) std::cerr << "a name that is no UTF-8: " << what << std::endl;
        ok = ok && passed;
    };
    // The conversions themselves: UTF-8 round trips; elsewhere bytes stay bytes, on Windows a
    // sequence that is no UTF-8 (or UTF-16) becomes U+FFFD.
    check("PathFromUtf8 and PathToUtf8",
        RayoMd::File::PathToUtf8(RayoMd::File::PathFromUtf8("a/\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80.png")) ==
            "a/\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80.png" &&
#ifdef _WIN32
        RayoMd::File::PathToUtf8(RayoMd::File::PathFromUtf8("\xFF" "a")) == "\xEF\xBF\xBD" "a" &&
        RayoMd::File::PathToUtf8(fs::path(std::wstring(L"b\xDC00"))) == "b\xEF\xBF\xBD");
#else
        RayoMd::File::PathToUtf8(RayoMd::File::PathFromUtf8("\xFF" "a")) == "\xFF" "a");
#endif
    TinyPdf::PdfOptions options;
    options.sourcePath = dir + "/doc.md";
    const std::string images = "![a](" + bad + ".png)\n\n![b](pic.png)\n\n![c](%FF%FE.png)\n\n![d](" + dir + "/" + bad +
        ".png)\n\n[link](" + bad + ".md) [anchor](#" + bad + ")\n";
    for (const std::string& text : { images, "# \xC3\x9Cnicode \xE2\x9C\x93\n\n" + images }) {
        std::string pdf;
        const TinyPdf::BuildResult result = TinyPdf::BuildPdf(text, options, pdf);
        check("image paths", result.Ok() && result.failedImages == 3 && ValidPdf(pdf));
    }
    TinyPdf::PdfOptions badSource = options;
    badSource.sourcePath = dir + "/" + bad + "/doc.md";
    std::string pdf;
    TinyPdf::BuildResult result = TinyPdf::BuildPdf("![b](pic.png)\n", badSource, pdf);
    check("the source path", result.Ok() && result.failedImages == 1);

    TinyPdf::PdfOptions themed = options;
    themed.theme.logoPath = dir + "/" + bad + ".png";
    themed.theme.headerLeft = "{logo}";
    result = TinyPdf::BuildPdf("# Logo\n", themed, pdf);
    check("a theme's logo", result.Ok() && result.failedImages == 1);
    themed.theme.fontPath = dir + "/" + bad + ".ttf";
    result = TinyPdf::BuildPdf("# Font\n", themed, pdf);
    check("a theme's font", result.error == TinyPdf::BuildError::ThemeFontUnavailable);
    TinyPdf::PdfTheme parsed;
    std::string themeError;
    if (TinyPdf::Internal::ParseTheme("logo = " + bad + ".png\nheader-left = {logo}\n", dir, parsed, themeError)) {
        TinyPdf::PdfOptions withTheme = options;
        withTheme.theme = parsed;
        result = TinyPdf::BuildPdf("# Parsed\n", withTheme, pdf);
        check("a parsed theme's logo", result.Ok() && result.failedImages == 1);
    }

    ok = ok && WriteFile(root / "book" / "SUMMARY.md", "# Book\n\n- [One](one.md)\n- [Bad](" + bad + ".md)\n") &&
        WriteFile(root / "book" / "one.md", "# One\n\n[two](two%FF.md#a) [raw](" + bad + ".md) [three](" + bad + "/x.md)\n");
    TinyPdf::Book book;
    std::string bookError;
    check("a SUMMARY.md listing", !RayoMd::Book::ReadBook({ root / "book" / "SUMMARY.md" }, book, bookError) &&
        !bookError.empty());
    TinyPdf::Book linked;
    linked.title = "Book";
    linked.directory = (root / "book").u8string();
    linked.chapters.push_back({ "# One\n\n[two](two%FF.md#a) [raw](" + bad + ".md)\n", (root / "book" / "one.md").u8string(), "", 0, false });
    linked.chapters.push_back({ "# Two\n\n![i](" + bad + ".png)\n", linked.directory + "/" + bad + ".md", "", 0, false });
    std::string bookPdf;
    result = TinyPdf::BuildBookPdf(linked, options, bookPdf);
    check("a book's links and files", result.Ok() && result.failedImages == 1 && ValidPdf(bookPdf));
#ifdef _WIN32
    // An unpaired surrogate in a file name, which NTFS allows and path::u8string threw for.
    const fs::path folder = root / "surrogate";
    fs::create_directories(folder);
    ok = ok && WriteFile(folder / std::wstring(L"a\xD800.md"), "# A\n") && WriteFile(folder / "b.md", "# B\n");
    TinyPdf::Book listed;
    check("a book's folder", RayoMd::Book::ReadBook({ folder }, listed, bookError) && listed.chapters.size() == 2 &&
        listed.chapters[0].markdown == "# A\n");
    std::vector<RayoMd::Batch::File> files;
    std::string listError;
    check("a batch folder", RayoMd::Batch::CollectMarkdown(folder, root / "out", false, files, listError) && files.size() == 2);
#endif
    fs::remove_all(root);
    return ok;
}

bool CheckHardening() {
    return CheckCancel() && CheckSpecialFiles() && CheckFontChangedInPlace() && CheckDeepCompositeGlyph() &&
        CheckImageContainment() && CheckCachePermissions() && CheckDecodeBudget() && CheckInvalidUtf8Names();
}

} // namespace hardening_tests
