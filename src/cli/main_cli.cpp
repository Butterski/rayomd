#include "rayomd/tiny_pdf.h"
#include "../common/text_utils.h"
#include "../common/profiling.h"
#include "../core/export_options.h"
#include "../core/rayomd_pdf_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

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
    RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Io);
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
    RayoMd::Profiling::ScopedPhase profile(RayoMd::Profiling::Phase::Io);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(content.data(), (std::streamsize)content.size());
    return (bool)file;
}

bool WriteUtf8FilePortable(const fs::path& path, const std::string& content) {
    return WriteBinaryFilePortable(path, content);
}
std::string PathToUtf8(const fs::path& path);

bool ReadBinaryFileLimited(const fs::path& path, size_t maximum, std::string& content) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamsize size = file.tellg();
    if (size < 0 || static_cast<uintmax_t>(size) > maximum) return false;
    file.seekg(0, std::ios::beg);
    content.assign(static_cast<size_t>(size), '\0');
    return size == 0 || static_cast<bool>(file.read(content.data(), size));
}

bool WriteNewFileAtomicPortable(const fs::path& path, const std::string& content, bool& existed) {
    existed = false;
    std::error_code error;
    if (fs::exists(path, error)) {
        existed = !error;
        return false;
    }
    static std::atomic<uint64_t> counter{0};
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        fs::path temporary = path.parent_path() /
            ("." + path.filename().string() + ".rayomd-" +
             std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp");
#ifndef _WIN32
        int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        size_t written = 0;
        bool writeOk = true;
        while (written < content.size()) {
            ssize_t count = ::write(fd, content.data() + written, content.size() - written);
            if (count <= 0) {
                if (count < 0 && errno == EINTR) continue;
                writeOk = false;
                break;
            }
            written += static_cast<size_t>(count);
        }
        if (::close(fd) != 0) writeOk = false;
        if (!writeOk) {
            ::unlink(temporary.c_str());
            return false;
        }
        if (::link(temporary.c_str(), path.c_str()) == 0) {
            ::unlink(temporary.c_str());
            return true;
        }
        int linkError = errno;
        ::unlink(temporary.c_str());
        if (linkError == EEXIST) {
            existed = true;
            return false;
        }
        return false;
#else
        if (fs::exists(temporary, error)) continue;
        if (!WriteBinaryFilePortable(temporary, content)) return false;
        fs::rename(temporary, path, error);
        if (!error) return true;
        fs::remove(temporary, error);
        return false;
#endif
    }
    return false;
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

int RunInspectSource(const fs::path& inputPath) {
    std::string pdf;
    if (!ReadBinaryFileLimited(inputPath, RayoMd::PdfSource::kMaxPdfBytes, pdf)) {
        std::cerr << "Error: could not read PDF or input exceeds "
            << RayoMd::PdfSource::kMaxPdfBytes << " bytes.\n";
        return 3;
    }
    RayoMd::PdfSource::Result result = RayoMd::PdfSource::Inspect(pdf, false);
    if (!result.Ok()) {
        std::cerr << "Error: " << RayoMd::PdfSource::StatusMessage(result.status) << ".\n";
        return RecoveryExitCode(result.status);
    }
    std::cout << "status=intact\nprofile=" << result.info.profile
        << "\nproducer_version=" << result.info.producerVersion
        << "\nsource_bytes=" << result.info.sourceBytes
        << "\nencoding=" << result.info.encoding
        << "\ndigest=valid\nattachment=" << result.info.attachmentName << "\n";
    return 0;
}

int RunRecoverSource(const fs::path& inputPath, const fs::path& outputPath) {
    std::string pdf;
    if (!ReadBinaryFileLimited(inputPath, RayoMd::PdfSource::kMaxPdfBytes, pdf)) {
        std::cerr << "Error: could not read PDF or input exceeds "
            << RayoMd::PdfSource::kMaxPdfBytes << " bytes.\n";
        return 3;
    }
    RayoMd::PdfSource::Result result = RayoMd::PdfSource::Inspect(pdf, true);
    if (!result.Ok()) {
        std::cerr << "Error: " << RayoMd::PdfSource::StatusMessage(result.status) << ".\n";
        return RecoveryExitCode(result.status);
    }
    bool existed = false;
    if (!WriteNewFileAtomicPortable(outputPath, result.source, existed)) {
        std::cerr << (existed ? "Error: destination already exists: " : "Error: could not write recovered source: ")
            << PathToUtf8(outputPath) << "\n";
        return existed ? 34 : 12;
    }
    std::cout << "Recovered " << result.info.sourceBytes << " bytes to " << PathToUtf8(outputPath) << "\n";
    return 0;
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
    bool embedSource = false;
    unsigned workers = 0;
};

bool ParseExportOptions(int argc, char** argv, int start, CliExportOptions& options,
    std::string& error) {
    for (int i = start; i < argc; i++) {
        std::string value = Lower(argv[i]);
        if (value == "pandoc") options.engine = 1;
        else if (value == "native") options.engine = 0;
        else if (value == "--allow-url-images") options.enableUrlImages = true;
        else if (value == "--allow-unsafe-local-images" || value == "--unsafe-local-images") options.allowUnsafeLocalImages = true;
        else if (value == "--embed-source") options.embedSource = true;
        else if (value.rfind("--workers=", 0) == 0) {
            std::string count = value.substr(10);
            char* end = nullptr;
            unsigned long parsed = std::strtoul(count.c_str(), &end, 10);
            if (!end || *end != char(0) || parsed < 1 || parsed > 64) {
                error = "--workers must be between 1 and 64";
                return false;
            }
            options.workers = static_cast<unsigned>(parsed);
        }
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
void ReportExportError(std::string* deferredError, const std::string& message) {
    if (deferredError) {
        *deferredError = message;
    } else {
        std::cerr << message << std::endl;
    }
}
fs::path PdfNameForMarkdown(const fs::path& path) {
    fs::path out = path.filename();
    out.replace_extension(".pdf");
    return out;
}

int BuildNativePdfMarkdown(const std::string& markdown, const std::string& sourcePath,
    const fs::path& outputPath, const CliExportOptions& options, std::string& pdfBuffer,
    const std::string& inputLabel, std::string* deferredError = nullptr) {
    pdfBuffer.clear();
    TinyPdf::PdfOptions pdfOptions;
    pdfOptions.style = options.style;
    pdfOptions.margin = options.margin;
    pdfOptions.sourcePath = sourcePath;
    pdfOptions.enableUrlImages = options.enableUrlImages;
    pdfOptions.allowUnsafeLocalImages = options.allowUnsafeLocalImages;
    pdfOptions.embedSource = options.embedSource;
    TinyPdf::BuildResult buildResult = TinyPdf::BuildPdf(markdown, pdfOptions, pdfBuffer);
    if (!buildResult) {
        int code = 10 + static_cast<int>(buildResult.error);
        std::ostringstream message;
        message << "Error: native PDF export failed";
        if (!inputLabel.empty()) message << " for " << inputLabel;
        message << " (code " << code << ").";
        ReportExportError(deferredError, message.str());
        return code;
    }
    if (!WriteBinaryFilePortable(outputPath, pdfBuffer)) {
        ReportExportError(deferredError, "Error: could not write PDF file: " + PathToUtf8(outputPath));
        return 12;
    }
    return 0;
}

int BuildNativePdfFile(const fs::path& inputPath, const fs::path& outputPath,
    const CliExportOptions& options, std::string& pdfBuffer, std::string* deferredError = nullptr) {
    const auto profileBefore = RayoMd::Profiling::Capture();
    std::string markdown;
    if (!ReadUtf8FilePortable(inputPath, markdown)) {
        ReportExportError(deferredError, "Error: could not read input Markdown file: " + PathToUtf8(inputPath));
        RayoMd::Profiling::EmitDelta("export", profileBefore, RayoMd::Profiling::Capture());
        return 3;
    }
    int result = BuildNativePdfMarkdown(markdown, PathToUtf8(inputPath), outputPath, options,
        pdfBuffer, PathToUtf8(inputPath), deferredError);
    RayoMd::Profiling::EmitDelta("export", profileBefore, RayoMd::Profiling::Capture());
    return result;
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

struct BatchJob {
    fs::path inputPath;
    fs::path outputPath;
    int result = 0;
    std::string error;
};

unsigned ResolveWorkerCount(const CliExportOptions& options, const std::vector<BatchJob>& jobs) {
    if (jobs.empty()) return 1;
    unsigned workers = options.workers;
    if (workers == 0) {
        workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 2;
        workers = std::min(workers, 6u);
    }

    uintmax_t largestInput = 0;
    for (const BatchJob& job : jobs) {
        std::error_code error;
        uintmax_t size = fs::file_size(job.inputPath, error);
        if (!error) largestInput = std::max(largestInput, size);
    }
    if (largestInput > 64u * 1024u * 1024u) workers = 1;
    else if (largestInput > 16u * 1024u * 1024u) workers = std::min(workers, 2u);
    return std::max(1u, std::min(workers, static_cast<unsigned>(jobs.size())));
}

int ExecuteBatchJobs(std::vector<BatchJob>& jobs, const CliExportOptions& options, const char* label) {
    const unsigned workerCount = ResolveWorkerCount(options, jobs);
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
                BatchJob& job = jobs[index];
                if (job.result != 0) continue;
                job.result = BuildNativePdfFile(job.inputPath, job.outputPath, options, pdfBuffer, &job.error);
            }
        });
    }
    for (std::thread& worker : workers) worker.join();

    int failures = 0;
    int lastResult = 0;
    for (const BatchJob& job : jobs) {
        if (job.result == 0) continue;
        failures++;
        lastResult = job.result;
        if (!job.error.empty()) std::cerr << job.error << std::endl;
    }
    if (failures != 0) {
        std::cerr << "Error: " << label << " failed for " << failures << " file(s)." << std::endl;
        return lastResult;
    }
    return 0;
}

int RunBatchExport(const fs::path& inputDir, const fs::path& outputDir, const CliExportOptions& options) {
    if (options.engine != 0) {
        std::cerr << "Error: Pandoc mode is currently only wired into the Windows build." << std::endl;
        return 20;
    }

    std::string fsError;
    if (!IsDirectoryPortable(inputDir, fsError)) {
        std::cerr << "Error: input folder is not a readable directory: " << PathToUtf8(inputDir);
        if (!fsError.empty()) std::cerr << " (" << fsError << ")";
        std::cerr << std::endl;
        return 3;
    }
    if (!EnsureDirectoryPortable(outputDir, fsError)) {
        std::cerr << "Error: could not create output folder: " << PathToUtf8(outputDir);
        if (!fsError.empty()) std::cerr << " (" << fsError << ")";
        std::cerr << std::endl;
        return 12;
    }

    std::vector<BatchJob> jobs;
    std::error_code iterError;
    for (fs::directory_iterator it(inputDir, iterError), end; !iterError && it != end; it.increment(iterError)) {
        std::error_code entryError;
        if (!it->is_regular_file(entryError) || entryError || it->path().extension() != ".md") continue;
        jobs.push_back({it->path(), outputDir / PdfNameForMarkdown(it->path())});
    }
    if (iterError) {
        std::cerr << "Error: could not read input folder: " << PathToUtf8(inputDir)
            << " (" << iterError.message() << ")" << std::endl;
        return 3;
    }
    std::sort(jobs.begin(), jobs.end(), [](const BatchJob& left, const BatchJob& right) {
        return left.inputPath.u8string() < right.inputPath.u8string();
    });
    return ExecuteBatchJobs(jobs, options, "batch export");
}

int RunStdinBatchExport(const fs::path& outputDir, const CliExportOptions& options) {
    if (options.engine != 0) {
        std::cerr << "Error: Pandoc mode is currently only wired into the Windows build." << std::endl;
        return 20;
    }
    std::string fsError;
    if (!EnsureDirectoryPortable(outputDir, fsError)) {
        std::cerr << "Error: could not create output folder: " << PathToUtf8(outputDir);
        if (!fsError.empty()) std::cerr << " (" << fsError << ")";
        std::cerr << std::endl;
        return 12;
    }

    std::vector<BatchJob> jobs;
    std::set<std::string> outputs;
    std::string line;
    while (std::getline(std::cin, line)) {
        line = RayoMd::Text::Trim(line);
        if (line.empty()) continue;
        fs::path inputPath(line);
        fs::path outputPath = outputDir / PdfNameForMarkdown(inputPath);
        BatchJob job{inputPath, outputPath};
        if (!outputs.insert(outputPath.lexically_normal().u8string()).second) {
            job.result = 12;
            job.error = "Error: multiple inputs map to the same output PDF: " + PathToUtf8(outputPath);
        }
        jobs.push_back(std::move(job));
    }
    return ExecuteBatchJobs(jobs, options, "stdin batch export");
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
    options.embedSource = cliOptions.embedSource;
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

int RunDoctor() {
    std::cout << "RayoMD doctor " << RAYOMD_VERSION << "\n";
    std::cout << "platform="
#ifdef _WIN32
        << "windows\n";
#else
        << "linux\n";
#endif
    std::cout << "zlib="
#ifdef RAYOMD_USE_ZLIB
        << "enabled\n";
#else
        << "disabled\n";
#endif
    std::cout << "curl="
#ifdef RAYOMD_USE_CURL
        << "enabled\n";
#else
        << "disabled\n";
#endif
    std::cout << "platform_images="
#ifdef _WIN32
        << "winhttp,wic\n";
#else
        << "native";
#ifdef RAYOMD_USE_CURL
    std::cout << ",http";
#endif
    std::cout << "\n";
#endif

    TinyPdf::PdfOptions options;
    std::string pdfBytes;
    TinyPdf::BuildResult result = TinyPdf::BuildPdf("# RayoMD Doctor\n\nUnicode: \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.\n", options, pdfBytes);
    bool fontOk = result.Ok();
    std::cout << "unicode_font=" << (fontOk ? "ok" : "unavailable") << "\n";

    std::error_code ec;
    fs::path tempDir = fs::temp_directory_path(ec);
    bool tempOk = !ec && fs::is_directory(tempDir, ec) && !ec;
    fs::path smokePath;
    if (tempOk && fontOk) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        smokePath = tempDir / ("rayomd-doctor-" + std::to_string(stamp) + ".pdf");
        tempOk = WriteBinaryFilePortable(smokePath, pdfBytes);
        if (tempOk) {
            std::error_code sizeError;
            tempOk = fs::file_size(smokePath, sizeError) > 0 && !sizeError;
        }
        std::error_code removeError;
        fs::remove(smokePath, removeError);
    }
    std::cout << "temp_output=" << (tempOk ? "ok" : "failed") << "\n";
    std::cout << "smoke_export=" << (fontOk && tempOk ? "ok" : "failed") << "\n";
    if (fontOk && tempOk) {
        std::cout << "status=ok\n";
        return 0;
    }
    std::cout << "status=failed\n";
    return 1;
}
void PrintUsage() {
    std::cerr
        << "RayoMD portable CLI " << RAYOMD_VERSION << "\n"
        << "Usage:\n"
        << "  rayomd --version\n"
        << "  rayomd --doctor\n"
        << "  rayomd --inspect-source <input.pdf>\n"
        << "  rayomd --recover-source <input.pdf> <output.md>\n"
        << "  rayomd --export <input.md> <output.pdf> [native] [style] [margin]\n"
        << "  rayomd --stdin <output.pdf> [native] [style] [margin]\n"
        << "  rayomd --batch <input-folder> <output-folder> [native] [style] [margin]\n"
        << "  rayomd --stdin-batch <output-folder> [native] [style] [margin]\n"
        << "  rayomd --serve <output-folder> [native] [style] [margin]\n"
        << "  rayomd --bench <input.md> <output-folder> <iterations> [style] [margin]\n"
        << "\n"
        << "Defaults: native elegant normal, URL images off, local images contained to the input directory.\n"
        << "Styles: elegant, modern, tech. Margins: compact, normal, wide, margin=0.75in, margin=54pt.\n"
        << "Resource flags: --allow-url-images, --allow-unsafe-local-images, --embed-source.\n"
        << "Batch flag: --workers=N (1-64; automatic mode uses at most 6).\n";
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

    if (command == "--doctor") return RunDoctor();

    if (command == "--inspect-source") {
        if (argc != 3) return PrintArgumentError("--inspect-source requires <input.pdf>.");
        return RunInspectSource(argv[2]);
    }
    if (command == "--recover-source") {
        if (argc != 4) return PrintArgumentError("--recover-source requires <input.pdf> <output.md>.");
        return RunRecoverSource(argv[2], argv[3]);
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
