#include "rayomd/tiny_pdf.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

#ifndef RAYOMD_VERSION
#define RAYOMD_VERSION "0.0.0"
#endif

constexpr long long kMaxMarkdownInputBytes = 128LL * 1024LL * 1024LL;

bool ReadUtf8FilePortable(const fs::path& path, std::string& content) {
#ifndef _WIN32
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        struct stat st = {};
        if (::fstat(fd, &st) == 0 && st.st_size >= 0 && st.st_size <= kMaxMarkdownInputBytes) {
            if (st.st_size == 0) {
                content.clear();
                ::close(fd);
                return true;
            }
            void* mapped = ::mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (mapped != MAP_FAILED) {
                const char* data = static_cast<const char*>(mapped);
                content.assign(data, data + (size_t)st.st_size);
                ::munmap(mapped, (size_t)st.st_size);
                ::close(fd);
                return true;
            }
        }
        ::close(fd);
    }
#endif

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamsize size = file.tellg();
    if (size < 0 || size > kMaxMarkdownInputBytes) return false;
    file.seekg(0, std::ios::beg);
    content.assign((size_t)size, '\0');
    return size == 0 || (bool)file.read(content.data(), size);
}

bool WriteBinaryFilePortable(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(content.data(), (std::streamsize)content.size());
    return (bool)file;
}

bool WriteUtf8FilePortable(const fs::path& path, const std::string& content) {
    return WriteBinaryFilePortable(path, content);
}

std::string PathToUtf8(const fs::path& path) {
    return path.u8string();
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

int ParseStyleArg(const char* arg) {
    if (!arg) return 0;
    std::string value = Lower(arg);
    if (value == "modern") return 1;
    if (value == "tech") return 2;
    if (value == "elegant") return 0;
    int style = std::atoi(arg);
    if (style < 0) style = 0;
    if (style > 2) style = 2;
    return style;
}

bool IsStyleArg(const char* arg) {
    if (!arg) return false;
    std::string value = Lower(arg);
    return value == "elegant" || value == "modern" || value == "tech" ||
        (value.size() == 1 && value[0] >= '0' && value[0] <= '2');
}

int ParseMarginArg(const char* arg) {
    if (!arg) return 1;
    std::string value = Lower(arg);
    if (value == "compact") return 0;
    if (value == "normal") return 1;
    if (value == "wide") return 2;
    if (value.rfind("margin=", 0) == 0) value.erase(0, 7);

    char* end = nullptr;
    double number = std::strtod(value.c_str(), &end);
    if (number <= 0.0) return 1;

    double points = number * 72.0;
    if (end && std::strncmp(end, "pt", 2) == 0) points = number;
    else if (end && std::strncmp(end, "in", 2) == 0) points = number * 72.0;

    points = std::max(18.0, std::min(144.0, points));
    return 1000 + (int)(points + 0.5);
}

bool IsMarginArg(const char* arg) {
    if (!arg) return false;
    std::string value = Lower(arg);
    return value == "compact" || value == "normal" || value == "wide" ||
        value.rfind("margin=", 0) == 0;
}

struct CliExportOptions {
    int engine = 0;
    int style = 0;
    int margin = 1;
    bool enableUrlImages = false;
    bool allowUnsafeLocalImages = false;
};

bool ParseExportOptions(int argc, char** argv, int start, CliExportOptions& options,
    std::string& error) {
    for (int i = start; i < argc; i++) {
        std::string value = Lower(argv[i]);
        if (value == "pandoc") options.engine = 1;
        else if (value == "native") options.engine = 0;
        else if (value == "--allow-url-images") options.enableUrlImages = true;
        else if (value == "--allow-unsafe-local-images" || value == "--unsafe-local-images") options.allowUnsafeLocalImages = true;
        else if (IsMarginArg(argv[i])) options.margin = ParseMarginArg(argv[i]);
        else if (IsStyleArg(argv[i])) options.style = ParseStyleArg(argv[i]);
        else {
            error = "unrecognized export option '" + std::string(argv[i]) + "'";
            return false;
        }
    }
    return true;
}

fs::path PdfNameForMarkdown(const fs::path& path) {
    fs::path out = path.filename();
    out.replace_extension(".pdf");
    return out;
}

int BuildNativePdfFile(const fs::path& inputPath, const fs::path& outputPath,
    const CliExportOptions& options, std::string& pdfBuffer) {
    std::string markdown;
    if (!ReadUtf8FilePortable(inputPath, markdown)) {
        std::cerr << "Error: could not read input Markdown file: " << PathToUtf8(inputPath) << "\n";
        return 3;
    }
    pdfBuffer.clear();
    TinyPdf::BuildOptions pdfOptions;
    pdfOptions.styleIdx = options.style;
    pdfOptions.marginIdx = options.margin;
    pdfOptions.sourcePath = PathToUtf8(inputPath);
    pdfOptions.enableUrlImages = options.enableUrlImages;
    pdfOptions.allowUnsafeLocalImages = options.allowUnsafeLocalImages;
    if (!TinyPdf::BuildPdfBytes(markdown, pdfOptions, pdfBuffer)) {
        int code = 10 + TinyPdf::g_lastError;
        std::cerr << "Error: native PDF export failed for " << PathToUtf8(inputPath)
            << " (code " << code << ").\n";
        return code;
    }
    if (!WriteBinaryFilePortable(outputPath, pdfBuffer)) {
        std::cerr << "Error: could not write PDF file: " << PathToUtf8(outputPath) << "\n";
        return 12;
    }
    return 0;
}

int RunBatchExport(const fs::path& inputDir, const fs::path& outputDir, const CliExportOptions& options) {
    if (options.engine != 0) {
        std::cerr << "Error: Pandoc mode is currently only wired into the Windows build.\n";
        return 20;
    }
    fs::create_directories(outputDir);

    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    int failures = 0;
    int lastResult = 0;
    for (const auto& entry : fs::directory_iterator(inputDir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".md") continue;
        fs::path outputPath = outputDir / PdfNameForMarkdown(entry.path());
        int result = BuildNativePdfFile(entry.path(), outputPath, options, pdfBuffer);
        if (result != 0) {
            failures++;
            lastResult = result;
        }
    }
    if (failures != 0) {
        std::cerr << "Error: batch export failed for " << failures << " file(s).\n";
        return lastResult;
    }
    return 0;
}

int RunStdinBatchExport(const fs::path& outputDir, const CliExportOptions& options) {
    if (options.engine != 0) {
        std::cerr << "Error: Pandoc mode is currently only wired into the Windows build.\n";
        return 20;
    }
    fs::create_directories(outputDir);

    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    int failures = 0;
    int lastResult = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        line = TinyPdf::Trim(line);
        if (line.empty()) continue;
        fs::path inputPath(line);
        fs::path outputPath = outputDir / PdfNameForMarkdown(inputPath);
        int result = BuildNativePdfFile(inputPath, outputPath, options, pdfBuffer);
        if (result != 0) {
            failures++;
            lastResult = result;
        }
    }
    if (failures != 0) {
        std::cerr << "Error: stdin batch export failed for " << failures << " file(s).\n";
        return lastResult;
    }
    return 0;
}

int RunServeExport(const fs::path& outputDir, const CliExportOptions& options) {
    if (options.engine != 0) {
        std::cerr << "Error: Pandoc mode is currently only wired into the Windows build.\n";
        return 20;
    }
    fs::create_directories(outputDir);

    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    int failures = 0;
    int lastResult = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        line = TinyPdf::Trim(line);
        if (line.empty()) continue;
        if (line == "quit" || line == "exit") break;

        fs::path inputPath(line);
        fs::path outputPath = outputDir / PdfNameForMarkdown(inputPath);
        auto start = std::chrono::steady_clock::now();
        int result = BuildNativePdfFile(inputPath, outputPath, options, pdfBuffer);
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        if (result != 0) {
            failures++;
            lastResult = result;
        }
        std::cout << (result == 0 ? "OK\t" : "ERR\t") << TinyPdf::F(ms) << "\t" << outputPath.string() << "\n";
    }
    return failures == 0 ? 0 : lastResult;
}

int RunNativeBench(const fs::path& inputPath, const fs::path& outputDir, int iterations, const CliExportOptions& cliOptions) {
    if (iterations < 1) iterations = 1;
    fs::create_directories(outputDir);

    std::string markdown;
    if (!ReadUtf8FilePortable(inputPath, markdown)) {
        std::cerr << "Error: could not read input Markdown file: " << PathToUtf8(inputPath) << "\n";
        return 3;
    }

    std::string pdfBytes;
    TinyPdf::BuildOptions options;
    options.styleIdx = cliOptions.style;
    options.marginIdx = cliOptions.margin;
    options.sourcePath = PathToUtf8(inputPath);
    options.enableUrlImages = cliOptions.enableUrlImages;
    options.allowUnsafeLocalImages = cliOptions.allowUnsafeLocalImages;
    if (!TinyPdf::BuildPdfBytes(markdown, options, pdfBytes)) {
        int code = 10 + TinyPdf::g_lastError;
        std::cerr << "Error: native PDF benchmark export failed for " << PathToUtf8(inputPath)
            << " (code " << code << ").\n";
        return code;
    }
    if (!WriteBinaryFilePortable(outputDir / "sample.pdf", pdfBytes)) {
        std::cerr << "Error: could not write benchmark sample PDF in " << PathToUtf8(outputDir) << "\n";
        return 12;
    }

    auto start = std::chrono::steady_clock::now();
    size_t totalBytes = 0;
    for (int i = 0; i < iterations; i++) {
        pdfBytes.clear();
        if (!TinyPdf::BuildPdfBytes(markdown, options, pdfBytes)) {
            int code = 10 + TinyPdf::g_lastError;
            std::cerr << "Error: native PDF benchmark export failed for " << PathToUtf8(inputPath)
                << " (code " << code << ").\n";
            return code;
        }
        totalBytes += pdfBytes.size();
    }
    auto end = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
    double avgMs = totalMs / (double)iterations;

    std::ostringstream report;
    report << "input_bytes=" << markdown.size() << "\n";
    report << "iterations=" << iterations << "\n";
    report << "total_ms=" << TinyPdf::F(totalMs) << "\n";
    report << "avg_ms=" << TinyPdf::F(avgMs) << "\n";
    report << "avg_pdf_bytes=" << (totalBytes / (size_t)iterations) << "\n";
    report << "path=" << (TinyPdf::IsAsciiDocument(markdown) ? "standard-font-ascii" : "unicode-embedded-font") << "\n";

    if (!WriteUtf8FilePortable(outputDir / "bench-results.txt", report.str())) {
        std::cerr << "Error: could not write benchmark results in " << PathToUtf8(outputDir) << "\n";
        return 12;
    }
    return 0;
}

void PrintUsage() {
    std::cerr
        << "RayoMD portable CLI " << RAYOMD_VERSION << "\n"
        << "Usage:\n"
        << "  rayomd --version\n"
        << "  rayomd --export <input.md> <output.pdf> [native] [style] [margin]\n"
        << "  rayomd --batch <input-folder> <output-folder> [native] [style] [margin]\n"
        << "  rayomd --stdin-batch <output-folder> [native] [style] [margin]\n"
        << "  rayomd --serve <output-folder> [native] [style] [margin]\n"
        << "  rayomd --bench <input.md> <output-folder> <iterations> [style] [margin]\n"
        << "\n"
        << "Defaults: native elegant normal, URL images off, local images contained to the input directory.\n"
        << "Styles: elegant, modern, tech. Margins: compact, normal, wide, margin=0.75in, margin=54pt.\n"
        << "Resource flags: --allow-url-images, --allow-unsafe-local-images.\n";
}

int PrintArgumentError(const std::string& message) {
    std::cerr << "Error: " << message << "\n\n";
    PrintUsage();
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    std::string command = Lower(argv[1]);
    CliExportOptions options;

    if (command == "--version" || command == "-v") {
        std::cout << "rayomd " << RAYOMD_VERSION << "\n";
        return 0;
    }

    if (command == "--stdin-batch") {
        if (argc < 3) return PrintArgumentError("--stdin-batch requires <output-folder>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 3, options, error)) return PrintArgumentError(error);
        return RunStdinBatchExport(argv[2], options);
    }

    if (command == "--serve") {
        if (argc < 3) return PrintArgumentError("--serve requires <output-folder>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 3, options, error)) return PrintArgumentError(error);
        return RunServeExport(argv[2], options);
    }

    if (command == "--bench") {
        if (argc < 5) return PrintArgumentError("--bench requires <input.md> <output-folder> <iterations>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 5, options, error)) return PrintArgumentError(error);
        return RunNativeBench(argv[2], argv[3], std::atoi(argv[4]), options);
    }

    if (command == "--batch") {
        if (argc < 4) return PrintArgumentError("--batch requires <input-folder> <output-folder>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 4, options, error)) return PrintArgumentError(error);
        return RunBatchExport(argv[2], argv[3], options);
    }

    if (command == "--export") {
        if (argc < 4) return PrintArgumentError("--export requires <input.md> <output.pdf>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 4, options, error)) return PrintArgumentError(error);
        if (options.engine != 0) {
            std::cerr << "Error: Pandoc mode is currently only wired into the Windows build.\n";
            return 20;
        }

        fs::path outputPath(argv[3]);
        if (outputPath.extension() != ".pdf") outputPath += ".pdf";
        std::string pdfBuffer;
        pdfBuffer.reserve(1024 * 1024);
        return BuildNativePdfFile(argv[2], outputPath, options, pdfBuffer);
    }

    return PrintArgumentError("unknown command '" + std::string(argv[1]) + "'.");
}
