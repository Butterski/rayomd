#include "batch_report.h"

#include "text_utils.h"

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

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
#ifdef _WIN32
        file = _wfopen(fs::u8path(path).c_str(), L"wb");
#else
        file = std::fopen(path.c_str(), "wb");
#endif
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

bool OutputUpToDate(const fs::path& input, const fs::path& output) {
    std::error_code error;
    const auto outputTime = fs::last_write_time(output, error);
    if (error) return false;
    const auto inputTime = fs::last_write_time(input, error);
    if (error || outputTime < inputTime) return false;
    std::ifstream file(output, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    const std::streamoff tail = std::min<std::streamoff>(size, 32);
    if (tail < 5) return false;
    char bytes[32];
    file.seekg(size - tail);
    if (!file.read(bytes, tail)) return false;
    return std::string_view(bytes, static_cast<size_t>(tail)).find("%%EOF") != std::string_view::npos;
}

bool CollectMarkdown(const fs::path& inputDir, const fs::path& outputDir, bool recursive, std::vector<File>& files,
    std::string& error) {
    files.clear();
    std::error_code iterError;
    const auto add = [&](const fs::directory_entry& entry) {
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError || entry.path().extension() != ".md") return;
        fs::path output = outputDir / entry.path().lexically_relative(inputDir);
        output.replace_extension(".pdf");
        files.push_back({ entry.path(), std::move(output) });
    };
    if (recursive) {
        fs::recursive_directory_iterator it(inputDir, fs::directory_options::skip_permission_denied, iterError);
        for (const fs::recursive_directory_iterator end; !iterError && it != end; it.increment(iterError)) {
            std::error_code entryError;
            if (it->is_directory(entryError)) {
                const std::string name = it->path().filename().u8string();
                if (!name.empty() && name[0] == '.') it.disable_recursion_pending();
                continue;
            }
            add(*it);
        }
    } else {
        for (fs::directory_iterator it(inputDir, iterError), end; !iterError && it != end; it.increment(iterError)) add(*it);
    }
    if (iterError) {
        error = iterError.message();
        return false;
    }
    std::sort(files.begin(), files.end(), [](const File& left, const File& right) {
        return left.input.u8string() < right.input.u8string();
    });
    return true;
}

} // namespace RayoMd::Batch
