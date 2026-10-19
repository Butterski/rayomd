// SPDX-License-Identifier: Apache-2.0

#include "batch_report.h"

#include "file_io.h"
#include "text_utils.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <string_view>
#include <thread>

namespace RayoMd::Batch {

Report::~Report() {
    Close();
}

bool Report::Open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    if (path == "-") {
        file = stdout;
        ownsFile = false;
    } else {
        file = RayoMd::File::Open(RayoMd::File::PathFromUtf8(path), "wb");
        ownsFile = file != nullptr;
    }
    failed = false;
    buffer.clear();
    lastWrite = std::chrono::steady_clock::now();
    return file != nullptr;
}

void Report::Write(const Record& record) {
    std::string line;
    line.reserve(160 + record.input.size() + record.output.size() + record.error.size());
    line += "{\"input\":\"";
    AppendJsonString(line, record.input);
    line += "\",\"output\":\"";
    AppendJsonString(line, record.output);
    line += "\",\"status\":\"";
    line += record.status;
    line += "\",\"code\":";
    line += std::to_string(record.code);
    if (!record.error.empty()) {
        line += ",\"error\":\"";
        AppendJsonString(line, record.error);
        line += '"';
    }
    char numbers[160];
    std::snprintf(numbers, sizeof(numbers),
        ",\"pages\":%u,\"bytes\":%llu,\"ms\":%.3f,\"missing_characters\":%u,\"failed_images\":%u}\n",
        record.pages, static_cast<unsigned long long>(record.bytes), record.milliseconds, record.missingCharacters,
        record.failedImages);
    line += numbers;

    std::lock_guard<std::mutex> lock(mutex);
    if (!file) return;
    buffer += line;
    const auto now = std::chrono::steady_clock::now();
    if (buffer.size() >= 64 * 1024 || now - lastWrite >= std::chrono::seconds(1)) {
        FlushLocked();
        lastWrite = now;
    }
}

void Report::FlushLocked() {
    if (!buffer.empty() && std::fwrite(buffer.data(), 1, buffer.size(), file) != buffer.size()) failed = true;
    if (std::fflush(file) != 0) failed = true;
    buffer.clear();
}

bool Report::Close() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!file) return !failed;
    FlushLocked();
    if (ownsFile && std::fclose(file) != 0) failed = true;
    file = nullptr;
    return !failed;
}

void AppendJsonString(std::string& out, std::string_view text) {
    static const char hex[] = "0123456789abcdef";
    for (size_t at = 0; at < text.size();) {
        const unsigned char byte = static_cast<unsigned char>(text[at]);
        if (byte >= 0x80) {
            uint32_t codePoint = 0;
            size_t length = 0;
            if (RayoMd::Text::DecodeUtf8(text, at, codePoint, length)) {
                out.append(text.data() + at, length);
                at += length;
            } else {
                out += "\\ufffd";
                at++;
            }
            continue;
        }
        if (byte == '"' || byte == '\\') {
            out += '\\';
            out += static_cast<char>(byte);
        } else if (byte == '\n') {
            out += "\\n";
        } else if (byte == '\r') {
            out += "\\r";
        } else if (byte == '\t') {
            out += "\\t";
        } else if (byte < 0x20 || byte == 0x7F) {
            out += "\\u00";
            out += hex[byte >> 4];
            out += hex[byte & 15];
        } else {
            out += static_cast<char>(byte);
        }
        at++;
    }
}

bool OutputUpToDate(const RayoMd::File::Path& input, const RayoMd::File::Path& output) {
    RayoMd::File::FileStatus outputStatus;
    RayoMd::File::FileStatus inputStatus;
    if (!RayoMd::File::StatusOf(output, outputStatus) || !RayoMd::File::StatusOf(input, inputStatus) ||
        outputStatus.modified < inputStatus.modified) {
        return false;
    }
    std::FILE* file = RayoMd::File::Open(output, "rb");
    if (!file) return false;
    const long long size = RayoMd::File::SizeOf(file);
    const long long tail = std::min<long long>(size, 32);
    char bytes[32];
    const bool read = tail >= 5 && RayoMd::File::SeekTo(file, size - tail) &&
        std::fread(bytes, 1, static_cast<size_t>(tail), file) == static_cast<size_t>(tail);
    std::fclose(file);
    return read && std::string_view(bytes, static_cast<size_t>(tail)).find("%%EOF") != std::string_view::npos;
}

bool CollectMarkdown(const RayoMd::File::Path& inputDir, const RayoMd::File::Path& outputDir, bool recursive,
    std::vector<File>& files, std::string& error) {
    using RayoMd::File::Entry;
    using RayoMd::File::Path;
    files.clear();
    // A regular file (through a link too) named *.md, its PDF at the same place below `outputDir`.
    const auto add = [&](const Entry& entry, const Path& path) {
        const RayoMd::File::PathView extension = RayoMd::File::ExtensionOf(entry.name);
        if (entry.type != RayoMd::File::FileType::Regular || extension.size() != 3 || extension[0] != '.' ||
            extension[1] != 'm' || extension[2] != 'd') {
            return;
        }
        Path output = outputDir;
        RayoMd::File::AppendPath(output, RayoMd::File::RelativePath(path, inputDir));
        RayoMd::File::ReplaceExtension(output, RayoMd::File::PathOfAscii(".pdf"));
        files.push_back({ path, std::move(output) });
    };
    int code = 0;
    bool listed;
    if (recursive) {
        listed = RayoMd::File::Walk(inputDir, [&](const Entry& entry) {
            if (entry.type == RayoMd::File::FileType::Directory) {
                // Hidden folders (".git") are not entered; Walk enters no link.
                return entry.name[0] == '.' ? RayoMd::File::WalkStep::Skip : RayoMd::File::WalkStep::Enter;
            }
            add(entry, *entry.path);
            return RayoMd::File::WalkStep::Skip;
        }, &code);
    } else {
        Path path;
        listed = RayoMd::File::ForEachEntry(inputDir, [&](const Entry& entry) {
            path = inputDir;
            RayoMd::File::AppendPath(path, entry.name);
            add(entry, path);
            return true;
        }, &code);
    }
    if (!listed) {
        error = RayoMd::File::ErrorText(code);
        return false;
    }
    // In the order of their UTF-8 names, each made once: sorted as indices, as a sort of the files
    // themselves (two paths each) took 20 KB of code.
    std::vector<std::string> names(files.size());
    std::vector<size_t> order(files.size());
    for (size_t i = 0; i < files.size(); i++) {
        names[i] = RayoMd::File::PathToUtf8(files[i].input);
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&names](size_t left, size_t right) { return names[left] < names[right]; });
    std::vector<File> sorted;
    sorted.reserve(files.size());
    for (const size_t index : order) sorted.push_back(std::move(files[index]));
    files = std::move(sorted);
    return true;
}

void RunJobs(size_t count, unsigned workers, uint64_t (*size)(void*, size_t),
    void (*run)(void*, size_t, std::string&), void* context) {
    if (count == 0) return;
    if (workers == 0) {
        workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 2;
        workers = std::min(workers, 6u);
    }
    workers = static_cast<unsigned>(std::min<size_t>(workers, count));
    std::atomic<size_t> sizing{ 0 };
    std::atomic<size_t> next{ 0 };
    std::mutex mutex;
    std::condition_variable sized;
    unsigned arrived = 0;
    unsigned active = workers;
    uint64_t largest = 0;
    const auto work = [&](unsigned id) {
        if (workers > 1) {
            uint64_t mine = 0;
            for (size_t index; (index = sizing.fetch_add(1, std::memory_order_relaxed)) < count;) {
                const uint64_t bytes = size(context, index);
                if (bytes != UINT64_MAX) mine = std::max(mine, bytes);
            }
            std::unique_lock<std::mutex> lock(mutex);
            largest = std::max(largest, mine);
            if (++arrived == workers) {
                if (largest > 64u * 1024u * 1024u) active = 1;
                else if (largest > 16u * 1024u * 1024u) active = std::min(workers, 2u);
                sized.notify_all();
            } else {
                sized.wait(lock, [&]() { return arrived == workers; });
            }
            if (id >= active) return;
        }
        std::string buffer;
        buffer.reserve(1024 * 1024);
        for (size_t index; (index = next.fetch_add(1, std::memory_order_relaxed)) < count;) run(context, index, buffer);
    };
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned id = 0; id < workers; id++) threads.emplace_back(work, id);
    for (std::thread& thread : threads) thread.join();
}

} // namespace RayoMd::Batch
