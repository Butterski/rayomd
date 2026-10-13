#pragma once

// Whole-file reads and writes through C stdio. RayoMD does not use iostreams: statically linked,
// their locale and stream code adds about 460 KB to the command line program. Paths open as
// wide strings on Windows.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace RayoMd::File {

// fopen of `path` with fopen's mode ("rb", "wb"); null when it cannot be opened.
std::FILE* Open(const std::filesystem::path& path, const char* mode);

// The size of an open file, -1 when it has none (a pipe). Leaves the position at the end.
long long SizeOf(std::FILE* file);

// Moves an open file to byte `offset`.
bool SeekTo(std::FILE* file, long long offset);

// The whole file, at most `maximum` bytes; false when it cannot be read or is larger.
bool ReadAll(const std::filesystem::path& path, size_t maximum, std::string& content);
bool ReadAll(const std::filesystem::path& path, size_t maximum, std::vector<uint8_t>& content);

// Creates or replaces `path` with `content`. On POSIX systems an existing file is overwritten in
// place and then cut to size (no O_TRUNC; see file_io.cpp).
bool WriteAll(const std::filesystem::path& path, std::string_view content);

// What the file at a path is now: its size, its last write, its last status change (POSIX) or
// creation (Windows), and its identity (POSIX); all zero when there is none. A file rewritten or
// replaced at the path gets another stamp, unless it keeps its size and is rewritten within the
// file system's time resolution.
struct Stamp {
    uint64_t size = 0;
    uint64_t modified = 0;
    uint64_t changed = 0;
    uint64_t id = 0;

    bool operator==(const Stamp& other) const {
        return size == other.size && modified == other.modified && changed == other.changed && id == other.id;
    }
    bool operator!=(const Stamp& other) const { return !(*this == other); }
};
Stamp StampOf(const std::filesystem::path& path);

} // namespace RayoMd::File
