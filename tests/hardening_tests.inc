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
    for (int i = 0; i < 30000; i++) blocks += "Paragraph " + std::to_string(i) + " of many blocks, with **bold** text.\n\n";
    for (int i = 0; i < 12000; i++) table += "| row " + std::to_string(i) + " | *cell* | `x` |\n";
    for (int i = 0; i < 150000; i++) code += "int line" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    code += "```\n";
    for (int i = 0; i < 400; i++) bookText += "Paragraph " + std::to_string(i) + " of a file.\n\n";
    TinyPdf::Book longBook;
    for (int file = 0; file < 200; file++) longBook.chapters.push_back({ bookText, (root / "f.md").u8string(), "", 0, false });
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
        // The whole build first, then one cancelled halfway, past the parse, which does not stop;
        // then the cancelled document again, whose image must not be taken for a failed one.
        std::string full;
        const auto start = std::chrono::steady_clock::now();
        const TinyPdf::BuildResult whole = build(plain, full, 1);
        const double fullMs = MillisecondsSince(start);
        cancel = false;
        TinyPdf::BuildResult cancelled;
        std::string cancelledPdf;
        std::thread worker([&]() { cancelled = build(options, cancelledPdf, 2); });
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(fullMs / 2.0));
        const auto set = std::chrono::steady_clock::now();
        cancel = true;
        worker.join();
        const double latency = MillisecondsSince(set);
        std::string again;
        const TinyPdf::BuildResult after = build(plain, again, 2);
        std::cout << "cancel " << item.name << ": returned " << latency << " ms after the flag, the whole build "
                  << fullMs << " ms" << std::endl;
        if (cancelled.error != TinyPdf::BuildError::Cancelled || !cancelledPdf.empty() || !whole.Ok() ||
            whole.failedImages != 0 || !ValidPdf(full) || !after.Ok() || after.failedImages != 0 ||
            latency > std::max(25.0, fullMs / 5.0)) {
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

bool CheckHardening() {
    return CheckCancel() && CheckSpecialFiles();
}

} // namespace hardening_tests
