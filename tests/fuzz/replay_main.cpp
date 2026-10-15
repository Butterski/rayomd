// SPDX-License-Identifier: Apache-2.0

// Runs inputs through the fuzz targets without libFuzzer, for ctest: the regressions a fuzzer
// found (tests/fuzz/regressions) and the seeds (tests/fuzz/seeds), each a folder per target.
//
//   rayomd-fuzz-replay DIR...            every file of DIR/<target>/ through that target
//   rayomd-fuzz-replay --target NAME FILE...
//   rayomd-fuzz-replay --expand FILE.rep OUT   writes the bytes a .rep file stands for
//
// A file ending in .rep describes a large input in a few lines, each `COUNT*TEXT`: TEXT repeated
// COUNT times, with the escapes \n \r \t \\ \0 \xHH and \{N:B:M}, the N-byte (1, 2 or 4)
// big-endian number B + i * M in repetition i. Lines starting with '#' are comments.
//
// An input fails when its target aborts or crashes, takes longer than RAYOMD_FUZZ_REPLAY_SECONDS
// (20 by default), or, without AddressSanitizer, allocates past 4 GiB.

#include "fuzz_targets.h"

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#define RAYOMD_REPLAY_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RAYOMD_REPLAY_ASAN 1
#endif
#endif

namespace {

namespace fs = std::filesystem;

char g_current[4096];

void OnAlarm(int) {
    static const char message[] = "rayomd-fuzz-replay: timed out: ";
    (void)!write(2, message, sizeof(message) - 1);
    (void)!write(2, g_current, strlen(g_current));
    (void)!write(2, "\n", 1);
    _exit(1);
}

int HexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool ExpandLine(std::string_view line, std::string& out, std::string& error) {
    const size_t star = line.find('*');
    if (star == std::string_view::npos || star == 0) {
        error = "expected COUNT*TEXT";
        return false;
    }
    unsigned long long count = 0;
    for (const char ch : line.substr(0, star)) {
        if (ch < '0' || ch > '9' || count > 100000000ull) {
            error = "bad count";
            return false;
        }
        count = count * 10 + static_cast<unsigned>(ch - '0');
    }
    const std::string_view text = line.substr(star + 1);
    for (unsigned long long i = 0; i < count; i++) {
        for (size_t at = 0; at < text.size(); at++) {
            if (text[at] != '\\') {
                out += text[at];
                continue;
            }
            if (++at == text.size()) {
                error = "a line ends in a backslash";
                return false;
            }
            switch (text[at]) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '0': out += '\0'; break;
            case '\\': out += '\\'; break;
            case 'x': {
                const int high = at + 2 < text.size() ? HexDigit(text[at + 1]) : -1;
                const int low = high >= 0 ? HexDigit(text[at + 2]) : -1;
                if (low < 0) {
                    error = "bad \\x escape";
                    return false;
                }
                out += static_cast<char>(high << 4 | low);
                at += 2;
                break;
            }
            case '{': {
                const size_t close = text.find('}', at);
                unsigned width = 0;
                long long base = 0, step = 0;
                if (close == std::string_view::npos ||
                    std::sscanf(std::string(text.substr(at + 1, close - at - 1)).c_str(), "%u:%lld:%lld", &width, &base, &step) != 3 ||
                    (width != 1 && width != 2 && width != 4)) {
                    error = "bad \\{N:B:M}";
                    return false;
                }
                const unsigned long long value = static_cast<unsigned long long>(base + static_cast<long long>(i) * step);
                for (unsigned byte = width; byte-- > 0;) out += static_cast<char>((value >> (8 * byte)) & 0xff);
                at = close;
                break;
            }
            default:
                error = "unknown escape";
                return false;
            }
        }
    }
    return true;
}

bool ReadInput(const fs::path& path, std::string& bytes) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    bytes.clear();
    char buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.append(buffer, read);
    std::fclose(file);
    if (path.extension() != ".rep") return true;
    std::string expanded, error;
    size_t lineNumber = 0;
    for (size_t at = 0; at <= bytes.size();) {
        size_t end = bytes.find('\n', at);
        if (end == std::string::npos) end = bytes.size();
        const std::string_view line(bytes.data() + at, end - at);
        lineNumber++;
        at = end + 1;
        if (line.empty() || line[0] == '#') continue;
        if (!ExpandLine(line, expanded, error)) {
            std::fprintf(stderr, "rayomd-fuzz-replay: %s:%zu: %s\n", path.c_str(), lineNumber, error.c_str());
            return false;
        }
    }
    bytes = std::move(expanded);
    return true;
}

RayoMdFuzz::Target FindTarget(std::string_view name) {
    for (size_t index = 0; index < RayoMdFuzz::kTargetCount; index++) {
        if (name == RayoMdFuzz::kTargets[index].name) return RayoMdFuzz::kTargets[index].target;
    }
    return nullptr;
}

unsigned g_seconds = 20;

bool Replay(RayoMdFuzz::Target target, const fs::path& path) {
    std::string bytes;
    if (!ReadInput(path, bytes)) {
        std::fprintf(stderr, "rayomd-fuzz-replay: cannot read %s\n", path.c_str());
        return false;
    }
    std::snprintf(g_current, sizeof(g_current), "%s", path.c_str());
    const auto start = std::chrono::steady_clock::now();
    alarm(g_seconds);
    RayoMdFuzz::RunTarget(target, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    alarm(0);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("ok %s (%zu bytes, %.1f ms)\n", path.c_str(), bytes.size(), ms);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (const char* seconds = std::getenv("RAYOMD_FUZZ_REPLAY_SECONDS"); seconds && std::atoi(seconds) > 0) {
        g_seconds = static_cast<unsigned>(std::atoi(seconds));
    }
    signal(SIGALRM, OnAlarm);
#ifndef RAYOMD_REPLAY_ASAN
    // A memory bomb fails with std::bad_alloc instead of taking the machine's memory.
    const rlimit limit{ 4ull << 30, 4ull << 30 };
    setrlimit(RLIMIT_AS, &limit);
#endif
    // The regressions include a FIFO among the images (fuzz_env.cpp).
    setenv("RAYOMD_FUZZ_FIFO", "1", 0);

    if (argc == 4 && std::strcmp(argv[1], "--expand") == 0) {
        std::string bytes;
        if (!ReadInput(argv[2], bytes)) return 1;
        FILE* out = std::fopen(argv[3], "wb");
        if (!out || std::fwrite(bytes.data(), 1, bytes.size(), out) != bytes.size() || std::fclose(out) != 0) return 1;
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--target") == 0) {
        const RayoMdFuzz::Target target = FindTarget(argv[2]);
        if (!target) {
            std::fprintf(stderr, "rayomd-fuzz-replay: unknown target %s\n", argv[2]);
            return 2;
        }
        for (int index = 3; index < argc; index++) {
            if (!Replay(target, argv[index])) return 1;
        }
        return 0;
    }
    if (argc < 2 || argv[1][0] == '-') {
        std::fprintf(stderr, "Usage: rayomd-fuzz-replay DIR... | --target NAME FILE... | --expand FILE.rep OUT\n");
        return 2;
    }
    size_t inputs = 0;
    for (int index = 1; index < argc; index++) {
        for (size_t t = 0; t < RayoMdFuzz::kTargetCount; t++) {
            const fs::path folder = fs::path(argv[index]) / RayoMdFuzz::kTargets[t].name;
            std::error_code error;
            if (!fs::is_directory(folder, error)) continue;
            std::vector<fs::path> files;
            for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
                if (it->is_regular_file() && it->path().filename().string()[0] != '.') files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) {
                if (!Replay(RayoMdFuzz::kTargets[t].target, file)) return 1;
                inputs++;
            }
        }
    }
    std::printf("rayomd-fuzz-replay: %zu inputs passed\n", inputs);
    return inputs > 0 ? 0 : 1;
}
