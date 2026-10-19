// SPDX-License-Identifier: Apache-2.0

#include "batch_report.h"

#include "file_io.h"
#include "text_utils.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace RayoMd::Batch {

namespace {

void AppendNumber(std::string& out, unsigned long long value) {
    char digits[20];
    out.append(digits, static_cast<size_t>(std::to_chars(digits, digits + sizeof(digits), value).ptr - digits));
}

// printf's "%.3f", without printf (which links MinGW's pformat and gdtoa): the double's exact
// value rounded to thousandths, ties to even.
void AppendThousandths(std::string& out, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if (bits >> 63) out += '-';
    const int biased = static_cast<int>((bits >> 52) & 0x7FF);
    uint64_t mantissa = bits & ((uint64_t(1) << 52) - 1);
    if (biased == 0x7FF) {
        out += mantissa ? "nan" : "inf";
        return;
    }
    if (biased != 0) mantissa |= uint64_t(1) << 52;
    const int exponent = (biased != 0 ? biased : 1) - 1075;  // value = mantissa * 2^exponent
    if (exponent >= 0) {
        // An integer of 16 digits or more: mantissa * 2^exponent in base-10^9 limbs, low first.
        std::vector<uint64_t> limbs{ mantissa % 1000000000, mantissa / 1000000000 % 1000000000,
                                     mantissa / 1000000000000000000 };
        for (int left = exponent; left > 0; left -= 30) {
            const int shift = std::min(left, 30);
            uint64_t carry = 0;
            for (uint64_t& limb : limbs) {
                const uint64_t product = (limb << shift) + carry;
                limb = product % 1000000000;
                carry = product / 1000000000;
            }
            if (carry != 0) limbs.push_back(carry);
        }
        while (limbs.size() > 1 && limbs.back() == 0) limbs.pop_back();
        AppendNumber(out, limbs.back());
        for (size_t i = limbs.size() - 1; i-- > 0;) {
            char digits[9];
            for (int d = 8; d >= 0; --d, limbs[i] /= 10) digits[d] = static_cast<char>('0' + limbs[i] % 10);
            out.append(digits, 9);
        }
        out += ".000";
        return;
    }
    // mantissa * 1000 < 2^63: the thousandths are its quotient by 2^-exponent, rounded.
    const uint64_t scaled = mantissa * 1000;
    const int shift = -exponent;
    uint64_t thousandths = 0;
    if (shift < 64) {
        thousandths = scaled >> shift;
        const uint64_t rest = scaled & ((uint64_t(1) << shift) - 1);
        const uint64_t half = uint64_t(1) << (shift - 1);
        if (rest > half || (rest == half && (thousandths & 1) != 0)) ++thousandths;
    }
    AppendNumber(out, thousandths / 1000);
    const unsigned fraction = static_cast<unsigned>(thousandths % 1000);
    out += '.';
    out += static_cast<char>('0' + fraction / 100);
    out += static_cast<char>('0' + fraction / 10 % 10);
    out += static_cast<char>('0' + fraction % 10);
}

} // namespace

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
    line += ",\"pages\":";
    AppendNumber(line, record.pages);
    line += ",\"bytes\":";
    AppendNumber(line, static_cast<unsigned long long>(record.bytes));
    line += ",\"ms\":";
    AppendThousandths(line, record.milliseconds);
    line += ",\"missing_characters\":";
    AppendNumber(line, record.missingCharacters);
    line += ",\"failed_images\":";
    AppendNumber(line, record.failedImages);
    line += "}\n";

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

} // namespace RayoMd::Batch
