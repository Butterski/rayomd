// SPDX-License-Identifier: Apache-2.0

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

// fopen of `path` with fopen's mode ("rb", "wb"), close-on-exec; null when it cannot be opened. A
// read mode opens only a regular file (OpenRegular).
std::FILE* Open(const std::filesystem::path& path, const char* mode);

// The regular file at `path` opened for reading, close-on-exec, its size in `size`; a FIFO, a
// device, a socket or a directory is refused before anything can wait on it: on POSIX the open is
// non-blocking (O_NONBLOCK, O_NOCTTY) and fstat must say S_ISREG before the descriptor is made
// blocking again; on Windows GetFileType must say FILE_TYPE_DISK. A file descriptor, -1 when it
// cannot (POSIX), or a HANDLE, INVALID_HANDLE_VALUE when it cannot (Windows, as intptr_t).
#ifdef _WIN32
intptr_t OpenRegular(const std::filesystem::path& path, uint64_t& size);
#else
int OpenRegular(const std::filesystem::path& path, uint64_t& size);
#endif

// The size of an open file, -1 when it has none (a pipe). Leaves the position at the end.
long long SizeOf(std::FILE* file);

// Moves an open file to byte `offset`.
bool SeekTo(std::FILE* file, long long offset);

// The whole regular file (OpenRegular), at most `maximum` bytes; false when it cannot be read, is
// larger, or is no regular file.
bool ReadAll(const std::filesystem::path& path, size_t maximum, std::string& content);
bool ReadAll(const std::filesystem::path& path, size_t maximum, std::vector<uint8_t>& content);

// Which file ReadAll opened, the same for each of its names (links, letter case, "a/../a"): its
// device and inode (POSIX) or volume serial number and file index (Windows); and its size, also
// when that is more than ReadAll's `maximum`. `opened` is false when no regular file was opened.
struct FileIdentity {
    uint64_t volume = 0;
    uint64_t index = 0;
    uint64_t size = 0;
    bool opened = false;

    bool operator<(const FileIdentity& other) const {
        return volume != other.volume ? volume < other.volume : index < other.index;
    }
};
// As ReadAll, and the identity of the file it opened.
bool ReadAll(const std::filesystem::path& path, size_t maximum, std::string& content, FileIdentity& identity);

// As ReadAll, for a `canonical` path (absolute, no symbolic link, no "." or ".."), and only while
// it still names the file opened: on Linux the path the open descriptor names (/proc/self/fd/N)
// must be `canonical`, else a directory of it was swapped for a link since the path was worked
// out, and the file is refused. A check of a path and a later open of it by name cannot tell.
// Elsewhere as ReadAll.
bool ReadCanonical(const std::filesystem::path& canonical, size_t maximum, std::vector<uint8_t>& content);

// Creates or replaces `path` with `content`. On POSIX systems an existing file is overwritten in
// place and then cut to size (no O_TRUNC; see file_io.cpp). `ownerOnly`: a file it creates is
// readable and writable by its owner only (0600) on POSIX.
bool WriteAll(const std::filesystem::path& path, std::string_view content, bool ownerOnly = false);

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

// The path a UTF-8 name names, and a path's name in UTF-8, without exceptions. On Windows,
// std::filesystem::u8path, path(std::string), path::u8string and path::string convert between
// UTF-8 and UTF-16 and throw on a sequence that is neither (invalid UTF-8 in a document's image
// path or link, an unpaired surrogate in a file name): std::terminate where RayoMD is built
// without exceptions. These make such a sequence U+FFFD instead, as MultiByteToWideChar and
// WideCharToMultiByte do: the path then names no file but one with U+FFFD in its name. Elsewhere
// the bytes are the path's, as with u8path.
std::filesystem::path PathFromUtf8(std::string_view utf8);
std::string PathToUtf8(const std::filesystem::path& path);

} // namespace RayoMd::File
