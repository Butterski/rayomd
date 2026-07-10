#include "rayomd/tiny_pdf.h"
#include "../common/text_utils.h"
#include "../core/export_options.h"

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

bool ReadStdinPortable(std::string& content) {
    content.clear();
    char buffer[64 * 1024];
    while (std::cin) {
        std::cin.read(buffer, sizeof(buffer));
        std::streamsize read = std::cin.gcount();
        if (read > 0) {
            if ((long long)content.size() + read > kMaxMarkdownInputBytes) return false;
            content.append(buffer, buffer + read);
        }
        if (!std::cin) {
            if (std::cin.eof()) break;
            return false;
        }
    }
    return true;
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

bool IsDirectoryPortable(const fs::path& path, std::string& error) {
    std::error_code ec;
    bool exists = fs::exists(path, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    if (!exists) {
        error = "does not exist";
        return false;
    }
    bool isDirectory = fs::is_directory(path, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    if (!isDirectory) {
        error = "not a directory";
        return false;
    }
    error.clear();
    return true;
}

bool EnsureDirectoryPortable(const fs::path& path, std::string& error) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return IsDirectoryPortable(path, error);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

struct CliExportOptions {
    int engine = 0;
    TinyPdf::PdfStyle style = TinyPdf::PdfStyle::Elegant;
    TinyPdf::PdfMargin margin = TinyPdf::PdfMargin::Normal();
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
        else {
            TinyPdf::PdfMargin margin;
            TinyPdf::PdfStyle style;
            if (TinyPdf::Internal::ParsePdfMargin(argv[i], margin)) options.margin = margin;
            else if (TinyPdf::Internal::ParsePdfStyle(argv[i], style)) options.style = style;
            else {
                error = "unrecognized export option '" + std::string(argv[i]) + "'";
                return false;
            }
        }
    }
    return true;
}
fs::path PdfNameForMarkdown(const fs::path& path) {
    fs::path out = path.filename();
    out.replace_extension(".pdf");
    return out;
}

int BuildNativePdfMarkdown(const std::string& markdown, const std::string& sourcePath,
    const fs::path& outputPath, const CliExportOptions& options, std::string& pdfBuffer,
    const std::string& inputLabel) {
    pdfBuffer.clear();
    TinyPdf::PdfOptions pdfOptions;
    pdfOptions.style = options.style;
    pdfOptions.margin = options.margin;
    pdfOptions.sourcePath = sourcePath;
    pdfOptions.enableUrlImages = options.enableUrlImages;
    pdfOptions.allowUnsafeLocalImages = options.allowUnsafeLocalImages;
    TinyPdf::BuildResult buildResult = TinyPdf::BuildPdf(markdown, pdfOptions, pdfBuffer);
    if (!buildResult) {
        int code = 10 + static_cast<int>(buildResult.error);
        std::cerr << "Error: native PDF export failed";
        if (!inputLabel.empty()) std::cerr << " for " << inputLabel;
        std::cerr << " (code " << code << ").\n";
        return code;
    }
    if (!WriteBinaryFilePortable(outputPath, pdfBuffer)) {
        std::cerr << "Error: could not write PDF file: " << PathToUtf8(outputPath) << "\n";
        return 12;
    }
    return 0;
}

int BuildNativePdfFile(const fs::path& inputPath, const fs::path& outputPath,
    const CliExportOptions& options, std::string& pdfBuffer) {
    std::string markdown;
    if (!ReadUtf8FilePortable(inputPath, markdown)) {
        std::cerr << "Error: could not read input Markdown file: " << PathToUtf8(inputPath) << "\n";
        return 3;
    }
    return BuildNativePdfMarkdown(markdown, PathToUtf8(inputPath), outputPath, options,
        pdfBuffer, PathToUtf8(inputPath));
}

int RunStdinExport(const fs::path& outputPath, const CliExportOptions& options) {
    if (options.engine != 0) {
        std::cerr << "Error: stdin export only supports native mode.\n";
        return 20;
    }

    std::string markdown;
    if (!ReadStdinPortable(markdown)) {
        std::cerr << "Error: could not read Markdown from stdin or input exceeded "
            << kMaxMarkdownInputBytes << " bytes.\n";
        return 3;
    }

    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    return BuildNativePdfMarkdown(markdown, std::string(), outputPath, options, pdfBuffer, std::string());
}

int RunBatchExport(const fs::path& inputDir, const fs::path& outputDir, const CliExportOptions& options) {
    if (options.engine != 0) {
        std::cerr << "Error: Pandoc mode is currently only wired into the Windows build.\n";
        return 20;
    }

    std::string fsError;
    if (!IsDirectoryPortable(inputDir, fsError)) {
        std::cerr << "Error: input folder is not a readable directory: " << PathToUtf8(inputDir);
        if (!fsError.empty()) std::cerr << " (" << fsError << ")";
        std::cerr << "\n";
        return 3;
    }
    if (!EnsureDirectoryPortable(outputDir, fsError)) {
        std::cerr << "Error: could not create output folder: " << PathToUtf8(outputDir);
        if (!fsError.empty()) std::cerr << " (" << fsError << ")";
        std::cerr << "\n";
        return 12;
    }

    std::error_code iterError;
    fs::directory_iterator it(inputDir, iterError);
    if (iterError) {
        std::cerr << "Error: could not read input folder: " << PathToUtf8(inputDir)
            << " (" << iterError.message() << ")\n";
        return 3;
    }

    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    int failures = 0;
    int lastResult = 0;
    for (fs::directory_iterator end; it != end; it.increment(iterError)) {
        if (iterError) {
            std::cerr << "Error: could not continue reading input folder: " << PathToUtf8(inputDir)
                << " (" << iterError.message() << ")\n";
            return 3;
        }
        const fs::directory_entry& entry = *it;
        std::error_code entryError;
        if (!entry.is_regular_file(entryError)) {
            if (entryError) {
                failures++;
                lastResult = 3;
                std::cerr << "Error: could not inspect batch entry: " << PathToUtf8(entry.path())
                    << " (" << entryError.message() << ")\n";
            }
            continue;
        }
        if (entry.path().extension() != ".md") continue;
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
    std::string fsError;
    if (!EnsureDirectoryPortable(outputDir, fsError)) {
        std::cerr << "Error: could not create output folder: " << PathToUtf8(outputDir);
        if (!fsError.empty()) std::cerr << " (" << fsError << ")";
        std::cerr << "\n";
        return 12;
    }

    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    int failures = 0;
    int lastResult = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        line = RayoMd::Text::Trim(line);
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
    std::string fsError;
    if (!EnsureDirectoryPortable(outputDir, fsError)) {
        std::cerr << "Error: could not create output folder: " << PathToUtf8(outputDir);
        if (!fsError.empty()) std::cerr << " (" << fsError << ")";
        std::cerr << "\n";
        return 12;
    }

    std::string pdfBuffer;
    pdfBuffer.reserve(1024 * 1024);
    int failures = 0;
    int lastResult = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        line = RayoMd::Text::Trim(line);
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
        std::cout << (result == 0 ? "OK\t" : "ERR\t") << RayoMd::Text::FormatDouble(ms) << "\t" << outputPath.string() << "\n";
    }
    return failures == 0 ? 0 : lastResult;
}

int RunNativeBench(const fs::path& inputPath, const fs::path& outputDir, int iterations, const CliExportOptions& cliOptions) {
    if (iterations < 1) iterations = 1;
    std::string fsError;
    if (!EnsureDirectoryPortable(outputDir, fsError)) {
        std::cerr << "Error: could not create benchmark output folder: " << PathToUtf8(outputDir);
        if (!fsError.empty()) std::cerr << " (" << fsError << ")";
        std::cerr << "\n";
        return 12;
    }

    std::string markdown;
    if (!ReadUtf8FilePortable(inputPath, markdown)) {
        std::cerr << "Error: could not read input Markdown file: " << PathToUtf8(inputPath) << "\n";
        return 3;
    }

    std::string pdfBytes;
    TinyPdf::PdfOptions options;
    options.style = cliOptions.style;
    options.margin = cliOptions.margin;
    options.sourcePath = PathToUtf8(inputPath);
    options.enableUrlImages = cliOptions.enableUrlImages;
    options.allowUnsafeLocalImages = cliOptions.allowUnsafeLocalImages;
    TinyPdf::BuildResult buildResult = TinyPdf::BuildPdf(markdown, options, pdfBytes);
    if (!buildResult) {
        int code = 10 + static_cast<int>(buildResult.error);
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
        TinyPdf::BuildResult buildResult = TinyPdf::BuildPdf(markdown, options, pdfBytes);
    if (!buildResult) {
            int code = 10 + static_cast<int>(buildResult.error);
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
    report << "total_ms=" << RayoMd::Text::FormatDouble(totalMs) << "\n";
    report << "avg_ms=" << RayoMd::Text::FormatDouble(avgMs) << "\n";
    report << "avg_pdf_bytes=" << (totalBytes / (size_t)iterations) << "\n";
    report << "path=" << (RayoMd::Text::IsAsciiDocument(markdown) ? "standard-font-ascii" : "unicode-embedded-font") << "\n";

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
        << "  rayomd --stdin <output.pdf> [native] [style] [margin]\n"
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

    if (command == "--stdin") {
        if (argc < 3) return PrintArgumentError("--stdin requires <output.pdf>.");
        std::string error;
        if (!ParseExportOptions(argc, argv, 3, options, error)) return PrintArgumentError(error);
        fs::path outputPath(argv[2]);
        if (outputPath.extension() != ".pdf") outputPath += ".pdf";
        return RunStdinExport(outputPath, options);
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
