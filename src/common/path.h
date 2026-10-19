// SPDX-License-Identifier: Apache-2.0

#pragma once

// File-system paths and operations without the C++17 filesystem library. Statically linked,
// libstdc++'s pulls its path parser, the codecvt facets and, on MinGW, whose libstdc++.a has no
// per-function sections, every locale facet and the file streams: about 760 KB of rayomd.exe.
//
// A Path is the platform's native string: UTF-16 on Windows, bytes elsewhere. A name from the
// system (the command line, a folder's entry) stays a Path until it is opened, so a name with no
// UTF-8 form (an unpaired surrogate, which NTFS allows) still opens; UTF-8 (PathToUtf8, file_io.h)
// is for messages, reports, sort keys and the vault's index. The lexical functions read and write
// both kinds (UTF-8 strings on Windows too) as libstdc++'s path class does on Linux, with
// Windows' separators ('/' and '\') and roots ("C:", "\\server") there; the core tests
// (tests/path_tests.inc) hold them to the library.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace RayoMd::File {

#ifdef _WIN32
using PathChar = wchar_t;
#else
using PathChar = char;
#endif
using Path = std::basic_string<PathChar>;
using PathView = std::basic_string_view<PathChar>;

// An ASCII name as a Path.
inline Path PathOfAscii(std::string_view ascii) {
    return Path(ascii.begin(), ascii.end());
}

// ---- Lexical: what the standard library's filesystem::path's members of the same purpose give, on the text.

// The bytes of `path` that ".." never takes out: its root name ("C:", "\\server"; Windows) and root
// directory (the separator after it, or "/"); 0 for a relative path.
size_t RootLength(std::string_view path);
// is_absolute(): a root directory (POSIX), a root name and a root directory (Windows).
bool IsAbsolute(std::string_view path);
// relative_path(): what follows the root.
std::string_view RelativePart(std::string_view path);
// parent_path(): up to the last separator before the last name, or the root; empty for a bare name.
std::string_view ParentOf(std::string_view path);
// filename(): the last component, empty when `path` ends in a separator or is a root.
std::string_view FileNameOf(std::string_view path);
// stem() and extension(): the file name without and from its last dot; a name that only begins
// with a dot, "." and ".." have no extension.
std::string_view StemOf(std::string_view path);
std::string_view ExtensionOf(std::string_view path);
// `base` /= `path`: `path` alone when it is absolute (or names another drive), after `base`'s root
// when it begins at a root directory, else after a separator unless `base` is empty, ends in one or
// is a root name alone.
void AppendPath(std::string& base, std::string_view path);
// replace_extension(`extension`): the extension removed, then `extension`, with a dot first unless
// it has one (or is empty).
void ReplaceExtension(std::string& path, std::string_view extension);
// lexically_normal(), in place: the platform's separators, one between components, no "."
// component, each ".." with the component before it taken out (one at the root dropped), a
// trailing separator kept, "." for an empty result.
void Normalize(std::string& path);
// lexically_relative(base): empty when the two have different roots, or `base` leads above where
// `path` begins.
std::string RelativePath(std::string_view path, std::string_view base);

#ifdef _WIN32
size_t RootLength(std::wstring_view path);
bool IsAbsolute(std::wstring_view path);
std::wstring_view RelativePart(std::wstring_view path);
std::wstring_view ParentOf(std::wstring_view path);
std::wstring_view FileNameOf(std::wstring_view path);
std::wstring_view StemOf(std::wstring_view path);
std::wstring_view ExtensionOf(std::wstring_view path);
void AppendPath(std::wstring& base, std::wstring_view path);
void ReplaceExtension(std::wstring& path, std::wstring_view extension);
void Normalize(std::wstring& path);
std::wstring RelativePath(std::wstring_view path, std::wstring_view base);
#endif

// ---- The file system. Each call answers for the moment it asks.

enum class FileType : uint8_t { None, Regular, Directory, Other };

// What stat (POSIX, through links) or GetFileAttributesExW (Windows) says of a path: its type
// (None when there is nothing or it cannot be read), its size and its last write (POSIX
// nanoseconds, Windows 100 ns since 1601, as FileTimeNow counts).
struct FileStatus {
    FileType type = FileType::None;
    uint64_t size = 0;
    uint64_t modified = 0;
};
// False, `error` set (errno, or GetLastError's code), when it cannot tell.
bool StatusOf(const Path& path, FileStatus& status, int* error = nullptr);
bool IsDirectory(const Path& path);
bool IsRegularFile(const Path& path);
uint64_t FileTimeNow();

// the standard library's filesystem::create_directories: each missing folder of `path`, made in turn. True when
// `path` was made (`created`) or already was a folder; false with `error` (errno, or GetLastError's
// code; ENOTDIR or ERROR_DIRECTORY for something else in the way).
bool CreateDirectories(const Path& path, int* error = nullptr, bool* created = nullptr);
// Removes the file or empty folder at `path`; false when nothing was removed.
bool Remove(const Path& path);
// Moves `from` to `to`, which is replaced (rename; Windows MoveFileExW with REPLACE_EXISTING).
bool Rename(const Path& from, const Path& to);
// The last write of the file at `path` set to now.
bool SetWriteTimeNow(const Path& path);
// Only its owner may read, write and enter the folder at `path` (0700; nothing on Windows, whose
// files inherit their folder's ACL).
bool SetOwnerOnly(const Path& path);
// the standard library's filesystem::absolute: `path` itself when absolute, else the working folder's (POSIX: getcwd
// and `path` appended; Windows: GetFullPathNameW). False for an empty path or when it fails.
bool Absolute(const Path& path, Path& absolute);
// The path `path` names with its links, junctions, "." and ".." resolved; empty when it names
// nothing. POSIX realpath(3); on Windows GetFinalPathNameByHandleW (the standard library there
// resolves no link), "\\?\C:\" as "C:\" and "\\?\UNC\server\" as "\\server\".
Path Resolved(const Path& path);
// The folder for temporary files, as the standard library's temp_directory_path gives it: the
// first of TMPDIR, TMP, TEMP and TEMPDIR that is set, else /tmp (POSIX), or GetTempPathW's answer
// (Windows); empty when that is no folder.
Path TempDirectory();
// What an `error` of these calls says, as std::error_code::message() gave it: strerror's text
// (Windows: of the errno the C runtime gives such an error).
std::string ErrorText(int error);

// An entry of a folder. `type` is what it is, a link followed (POSIX: d_type, else fstatat; None
// for a dangling link); `link` tells a symbolic link (POSIX) or a name-surrogate reparse point
// (Windows: symbolic links, junctions, mount points; FindFirstFileExW gives a folder's link as a
// folder, a file's as a file). In Walk, `path` is its whole path (the folder's and its name, as
// AppendPath joins them) and `depth` the folders between it and the root.
struct Entry {
    PathView name;
    FileType type = FileType::None;
    bool link = false;
    int depth = 0;
    const Path* path = nullptr;
};

// The entries of `folder` but "." and "..", in the order the system lists them (readdir,
// FindFirstFileExW), while `visit` returns true. False with `error` when the folder cannot be read.
bool ForEachEntry(const Path& folder, bool (*visit)(void*, const Entry&), void* context, int* error = nullptr);
template <class Visit>
bool ForEachEntry(const Path& folder, Visit&& visit, int* error = nullptr) {
    using Callable = std::remove_reference_t<Visit>;
    return ForEachEntry(folder, [](void* self, const Entry& entry) -> bool { return (*static_cast<Callable*>(self))(entry); },
        const_cast<void*>(static_cast<const void*>(&visit)), error);
}

// What Walk does after an entry: go on (entering it when it is a folder and no link), go on
// without entering it, or stop.
enum class WalkStep : uint8_t { Enter, Skip, Stop };
// The entries below `root`, each folder's entries right after it (depth first, as
// the standard library's filesystem::recursive_directory_iterator with skip_permission_denied visits them): a folder
// that may not be read is passed over, links to folders are never entered. False with `error`
// when `root` or a folder below it cannot be read for another reason, the walk then ended.
bool Walk(const Path& root, WalkStep (*visit)(void*, const Entry&), void* context, int* error = nullptr);
template <class Visit>
bool Walk(const Path& root, Visit&& visit, int* error = nullptr) {
    using Callable = std::remove_reference_t<Visit>;
    return Walk(root, [](void* self, const Entry& entry) -> WalkStep { return (*static_cast<Callable*>(self))(entry); },
        const_cast<void*>(static_cast<const void*>(&visit)), error);
}

} // namespace RayoMd::File
