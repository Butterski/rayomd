// SPDX-License-Identifier: Apache-2.0

#include "path.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include <cerrno>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace RayoMd::File {

namespace {

// ---- Lexical, on either kind of string (libstdc++'s rules, Linux's or Windows' separators).

#ifdef _WIN32
constexpr char kSeparator = '\\';
template <class C>
bool IsSeparator(C c) {
    return c == '/' || c == '\\';
}
#else
constexpr char kSeparator = '/';
template <class C>
bool IsSeparator(C c) {
    return c == '/';
}
#endif

template <class C>
bool IsDot(std::basic_string_view<C> name) {
    return name.size() == 1 && name[0] == '.';
}

template <class C>
bool IsDotDot(std::basic_string_view<C> name) {
    return name.size() == 2 && name[0] == '.' && name[1] == '.';
}

template <class C>
size_t RootLengthOf(std::basic_string_view<C> path) {
#ifdef _WIN32
    size_t length = 0;
    if (path.size() >= 3 && IsSeparator(path[0]) && IsSeparator(path[1]) && !IsSeparator(path[2])) {
        length = 2;
        while (length < path.size() && !IsSeparator(path[length])) length++;
    } else if (path.size() >= 2 && path[1] == ':') {
        length = 2;
    }
    return length < path.size() && IsSeparator(path[length]) ? length + 1 : length;
#else
    return !path.empty() && path[0] == '/' ? 1 : 0;
#endif
}

template <class C>
bool HasRootDirectory(std::basic_string_view<C> path, size_t rootLength) {
    return rootLength > 0 && IsSeparator(path[rootLength - 1]);
}

template <class C>
bool IsAbsoluteOf(std::basic_string_view<C> path) {
    const size_t rootLength = RootLengthOf(path);
#ifdef _WIN32
    return HasRootDirectory(path, rootLength) && rootLength > 1;
#else
    return HasRootDirectory(path, rootLength);
#endif
}

// After the root, and any further separators of the root directory ("///a" is "a").
template <class C>
std::basic_string_view<C> RelativePartOf(std::basic_string_view<C> path) {
    size_t at = RootLengthOf(path);
    while (at < path.size() && IsSeparator(path[at])) at++;
    return path.substr(at);
}

template <class C>
std::basic_string_view<C> ParentOfPath(std::basic_string_view<C> path) {
    const size_t rootLength = RootLengthOf(path);
    size_t end = path.size();
    while (end > rootLength && !IsSeparator(path[end - 1])) end--;
    while (end > rootLength && IsSeparator(path[end - 1])) end--;
    return path.substr(0, end);
}

template <class C>
std::basic_string_view<C> FileNameOfPath(std::basic_string_view<C> path) {
    size_t start = path.size();
    while (start > 0 && !IsSeparator(path[start - 1])) start--;
    return path.substr(std::max(start, std::min(RootLengthOf(path), path.size())));
}

// Where the extension of a file name begins, npos for none: its last dot, unless that is its
// first character ("." and ".." included).
template <class C>
size_t ExtensionStart(std::basic_string_view<C> name) {
    if (IsDot(name) || IsDotDot(name)) return std::basic_string_view<C>::npos;
    const size_t dot = name.rfind(C('.'));
    return dot == 0 ? std::basic_string_view<C>::npos : dot;
}

template <class C>
std::basic_string_view<C> StemOfPath(std::basic_string_view<C> path) {
    const std::basic_string_view<C> name = FileNameOfPath(path);
    const size_t dot = ExtensionStart(name);
    return dot == std::basic_string_view<C>::npos ? name : name.substr(0, dot);
}

template <class C>
std::basic_string_view<C> ExtensionOfPath(std::basic_string_view<C> path) {
    const std::basic_string_view<C> name = FileNameOfPath(path);
    const size_t dot = ExtensionStart(name);
    return dot == std::basic_string_view<C>::npos ? std::basic_string_view<C>() : name.substr(dot);
}

template <class C>
void AppendPathTo(std::basic_string<C>& base, std::basic_string_view<C> path) {
    const size_t rootLength = RootLengthOf(path);
    const bool rootDirectory = HasRootDirectory(path, rootLength);
    const std::basic_string_view<C> rootName = path.substr(0, rootLength - rootDirectory);
    const size_t baseRoot = RootLengthOf(std::basic_string_view<C>(base));
    const size_t baseName = baseRoot - (baseRoot > 0 && IsSeparator(base[baseRoot - 1]));
#ifdef _WIN32
    const bool absolute = rootDirectory && !rootName.empty();
#else
    const bool absolute = rootDirectory;
#endif
    if (absolute || (!rootName.empty() && rootName != std::basic_string_view<C>(base).substr(0, baseName))) {
        base.assign(path);
        return;
    }
    if (rootDirectory) base.resize(baseName);
    else if (base.size() > baseRoot && !IsSeparator(base.back())) base += C(kSeparator);
    base.append(path.substr(rootName.size()));
}

template <class C>
void ReplaceExtensionOf(std::basic_string<C>& path, std::basic_string_view<C> extension) {
    const std::basic_string_view<C> name = FileNameOfPath(std::basic_string_view<C>(path));
    const size_t dot = ExtensionStart(name);
    if (dot != std::basic_string_view<C>::npos) path.resize(path.size() - name.size() + dot);
    if (!extension.empty() && extension[0] != '.') path += C('.');
    path.append(extension);
}

// libstdc++'s lexically_normal step by step: a name is appended after a separator unless the text
// ends in one; "." and a trailing empty name add a separator after a name; ".." takes out the name
// before it (or the name before a trailing separator), is dropped at a root directory and kept
// after another "..". Output never outruns the input, so it is written over it.
template <class C>
void NormalizePath(std::basic_string<C>& path) {
    if (path.empty()) return;
    const size_t size = path.size();
    const size_t rootLength = RootLengthOf(std::basic_string_view<C>(path));
    C* const text = &path[0];
    for (size_t at = 0; at < rootLength; at++) {
        if (IsSeparator(text[at])) text[at] = C(kSeparator);
    }
    const bool rootDirectory = rootLength > 0 && text[rootLength - 1] == C(kSeparator);
    size_t out = rootLength;
    const auto nameStart = [&](size_t end) {
        while (end > rootLength && text[end - 1] != C(kSeparator)) end--;
        return end;
    };
    const auto isDotDot = [&](size_t start, size_t length) {
        return IsDotDot(std::basic_string_view<C>(text + start, length));
    };
    const auto endsInName = [&]() { return out > rootLength && text[out - 1] != C(kSeparator); };
    const auto append = [&](size_t start, size_t length) {
        if (endsInName()) text[out++] = C(kSeparator);
        std::memmove(text + out, text + start, length * sizeof(C));
        out += length;
    };
    for (size_t at = rootLength; at < size;) {
        const size_t start = at;
        while (at < size && !IsSeparator(text[at])) at++;
        const size_t length = at - start;
        const bool separated = at < size;
        while (at < size && IsSeparator(text[at])) at++;
        if (length == 1 && text[start] == '.') {
            if (endsInName()) text[out++] = C(kSeparator);
        } else if (isDotDot(start, length)) {
            if (endsInName()) {
                const size_t name = nameStart(out);
                if (isDotDot(name, out - name)) append(start, 2);
                else out = name;
            } else if (out == rootLength) {
                if (!rootDirectory) append(start, 2);
            } else {
                const size_t name = nameStart(out - 1);
                if (isDotDot(name, out - 1 - name)) append(start, 2);
                else out = name;
            }
        } else if (length > 0) {
            append(start, length);
        }
        // A separator ending the path is an empty last name.
        if (separated && at == size && endsInName()) text[out++] = C(kSeparator);
    }
    if (out > rootLength && text[out - 1] == C(kSeparator)) {
        const size_t name = nameStart(out - 1);
        if (isDotDot(name, out - 1 - name)) out--;
    }
    if (out == 0) text[out++] = '.';
    path.resize(out);
}

// A path's root ("C:\", "\\server\", "/"; root name and root directory) and its names after it, as
// the standard library's filesystem::path's iterator gives them: each name, then an empty one when separators end the
// path after a name.
template <class C>
struct PathNames {
    std::basic_string_view<C> path;
    size_t rootLength;
    size_t at;
    bool afterName = false;

    explicit PathNames(std::basic_string_view<C> text) : path(text), rootLength(RootLengthOf(text)), at(rootLength) {}
    bool RootDirectory() const { return HasRootDirectory(path, rootLength); }
    std::basic_string_view<C> RootName() const { return path.substr(0, rootLength - RootDirectory()); }
    bool Next(std::basic_string_view<C>& name) {
        name = std::basic_string_view<C>();
        if (at > path.size()) return false;
        size_t start = at;
        while (start < path.size() && IsSeparator(path[start])) start++;
        if (start == path.size()) {
            const bool trailing = afterName && start > at;
            at = path.size() + 1;
            return trailing;
        }
        at = start;
        while (at < path.size() && !IsSeparator(path[at])) at++;
        name = path.substr(start, at - start);
        afterName = true;
        return true;
    }
};

template <class C>
std::basic_string<C> RelativePathOf(std::basic_string_view<C> path, std::basic_string_view<C> base) {
    PathNames<C> from(path);
    PathNames<C> to(base);
    std::basic_string<C> out;
    if (from.RootName() != to.RootName() || IsAbsoluteOf(path) != IsAbsoluteOf(base) || (!from.RootDirectory() && to.RootDirectory())) {
        return out;
    }
    // A root directory only `path` has (Windows: "\a" against "b") is a mismatch there, and
    // appending it takes back the ".." before it.
    const bool rooted = from.RootDirectory() && !to.RootDirectory();
    std::basic_string_view<C> a, b;
    bool hasA = from.Next(a);
    bool hasB = to.Next(b);
    while (!rooted && hasA && hasB && a == b) {
        hasA = from.Next(a);
        hasB = to.Next(b);
    }
    int up = 0;
    for (; hasB; hasB = to.Next(b)) up += IsDotDot(b) ? -1 : !b.empty() && !IsDot(b);
    if (!rooted && up == 0 && a.empty()) {
        out += C('.');
        return out;
    }
    if (up < 0) return out;
    // As the root directory is written (operator/= keeps it), then names with the platform's.
    if (rooted) out += path[from.rootLength - 1];
    for (; !rooted && up > 0; up--) {
        if (!out.empty()) out += C(kSeparator);
        out += C('.');
        out += C('.');
    }
    for (; hasA; hasA = from.Next(a)) {
        if (!out.empty() && !IsSeparator(out.back())) out += C(kSeparator);
        out.append(a);
    }
    return out;
}

} // namespace

size_t RootLength(std::string_view path) {
    return RootLengthOf(path);
}
bool IsAbsolute(std::string_view path) {
    return IsAbsoluteOf(path);
}
std::string_view RelativePart(std::string_view path) {
    return RelativePartOf(path);
}
std::string_view ParentOf(std::string_view path) {
    return ParentOfPath(path);
}
std::string_view FileNameOf(std::string_view path) {
    return FileNameOfPath(path);
}
std::string_view StemOf(std::string_view path) {
    return StemOfPath(path);
}
std::string_view ExtensionOf(std::string_view path) {
    return ExtensionOfPath(path);
}
void AppendPath(std::string& base, std::string_view path) {
    AppendPathTo(base, path);
}
void ReplaceExtension(std::string& path, std::string_view extension) {
    ReplaceExtensionOf(path, extension);
}
void Normalize(std::string& path) {
    NormalizePath(path);
}
std::string RelativePath(std::string_view path, std::string_view base) {
    return RelativePathOf(path, base);
}

#ifdef _WIN32
size_t RootLength(std::wstring_view path) {
    return RootLengthOf(path);
}
bool IsAbsolute(std::wstring_view path) {
    return IsAbsoluteOf(path);
}
std::wstring_view RelativePart(std::wstring_view path) {
    return RelativePartOf(path);
}
std::wstring_view ParentOf(std::wstring_view path) {
    return ParentOfPath(path);
}
std::wstring_view FileNameOf(std::wstring_view path) {
    return FileNameOfPath(path);
}
std::wstring_view StemOf(std::wstring_view path) {
    return StemOfPath(path);
}
std::wstring_view ExtensionOf(std::wstring_view path) {
    return ExtensionOfPath(path);
}
void AppendPath(std::wstring& base, std::wstring_view path) {
    AppendPathTo(base, path);
}
void ReplaceExtension(std::wstring& path, std::wstring_view extension) {
    ReplaceExtensionOf(path, extension);
}
void Normalize(std::wstring& path) {
    NormalizePath(path);
}
std::wstring RelativePath(std::wstring_view path, std::wstring_view base) {
    return RelativePathOf(path, base);
}
#endif

// ---- The file system.

#ifdef _WIN32

namespace {

uint64_t FileTime(FILETIME value) {
    return (uint64_t)value.dwHighDateTime << 32 | value.dwLowDateTime;
}

int LastError() {
    return (int)::GetLastError();
}

} // namespace

bool StatusOf(const Path& path, FileStatus& status, int* error) {
    status = FileStatus();
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        if (error != nullptr) *error = path.empty() ? ERROR_PATH_NOT_FOUND : LastError();
        return false;
    }
    status.type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? FileType::Directory : FileType::Regular;
    status.size = (uint64_t)data.nFileSizeHigh << 32 | data.nFileSizeLow;
    status.modified = FileTime(data.ftLastWriteTime);
    return true;
}

uint64_t FileTimeNow() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return FileTime(now);
}

namespace {

// mkdir of one folder, as libstdc++'s create_dir: an existing folder is no error.
bool MakeDirectory(const Path& path, int& error, bool& created) {
    created = CreateDirectoryW(path.c_str(), nullptr) != 0;
    if (created) return true;
    error = LastError();
    if (error == ERROR_ALREADY_EXISTS && IsDirectory(path)) error = 0;
    return error == 0;
}

bool IsNotFound(int error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME ||
        error == ERROR_BAD_NETPATH || error == ERROR_BAD_NET_NAME;
}

constexpr int kNotADirectory = ERROR_DIRECTORY;
constexpr int kInvalidArgument = ERROR_INVALID_PARAMETER;
constexpr int kNameTooLong = ERROR_FILENAME_EXCED_RANGE;

} // namespace

bool Remove(const Path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? RemoveDirectoryW(path.c_str()) != 0 : DeleteFileW(path.c_str()) != 0;
}

bool Rename(const Path& from, const Path& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
}

bool SetWriteTimeNow(const Path& path) {
    const HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const bool set = SetFileTime(file, nullptr, nullptr, &now) != 0;
    CloseHandle(file);
    return set;
}

bool SetOwnerOnly(const Path&) {
    return true;
}

bool Absolute(const Path& path, Path& absolute) {
    if (path.empty()) return false;
    if (IsAbsolute(PathView(path))) {
        absolute = path;
        return true;
    }
    // A root directory alone ("\a") is the current drive's; a share's root ("\\server\share\") is a
    // root name, so absolute already.
    const wchar_t* const text = path.c_str();
    std::wstring full(MAX_PATH, L'\0');
    DWORD length = GetFullPathNameW(text, (DWORD)full.size(), &full[0], nullptr);
    if (length >= full.size()) {
        full.resize(length);
        length = GetFullPathNameW(text, (DWORD)full.size(), &full[0], nullptr);
    }
    if (length == 0 || length >= full.size()) return false;
    full.resize(length);
    absolute = std::move(full);
    return true;
}

Path Resolved(const Path& path) {
    const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Path();
    std::wstring name(MAX_PATH, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, &name[0], (DWORD)name.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= name.size()) {
        name.resize(length + 1);
        length = GetFinalPathNameByHandleW(handle, &name[0], (DWORD)name.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(handle);
    if (length == 0 || length >= name.size()) return Path();
    name.resize(length);
    // The answer begins "\\?\C:\" or "\\?\UNC\server\share\", which Normalize would read as a root
    // name "\\?" and no call opens once it made it "\?\": the path as the other Windows calls
    // write it.
    if (name.compare(0, 8, L"\\\\?\\UNC\\") == 0) name.replace(0, 8, L"\\\\");
    else if (name.compare(0, 4, L"\\\\?\\") == 0) name.erase(0, 4);
    return name;
}

Path TempDirectory() {
    wchar_t buffer[MAX_PATH + 1];
    const DWORD length = GetTempPathW(MAX_PATH + 1, buffer);
    if (length == 0 || length > MAX_PATH) return Path();
    const Path folder(buffer, length);
    return IsDirectory(folder) ? folder : Path();
}

namespace {

// A folder's entries, one FindFirstFileExW listing (FindExInfoBasic: no short names) at a time.
class Listing {
public:
    // False with `error` when `folder` cannot be listed; an empty folder lists nothing.
    bool Open(const Path& folder, int& error) {
        Path pattern = folder;
        if (!pattern.empty() && !IsSeparator(pattern.back())) pattern += L'\\';
        pattern += L'*';
        find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find != INVALID_HANDLE_VALUE) {
            pending = true;
            return true;
        }
        error = LastError();
        if (error == ERROR_NO_MORE_FILES || (error == ERROR_FILE_NOT_FOUND && IsDirectory(folder))) {
            error = 0;
            return true;
        }
        return false;
    }
    ~Listing() {
        if (find != INVALID_HANDLE_VALUE) FindClose(find);
    }
    // The next entry but "." and ".."; false at the end.
    bool Next(Entry& entry) {
        for (;;) {
            if (find == INVALID_HANDLE_VALUE) return false;
            if (!pending && !FindNextFileW(find, &data)) {
                FindClose(find);
                find = INVALID_HANDLE_VALUE;
                return false;
            }
            pending = false;
            const wchar_t* name = data.cFileName;
            if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) continue;
            entry.name = PathView(name);
            entry.type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? FileType::Directory : FileType::Regular;
            entry.link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(data.dwReserved0);
            return true;
        }
    }

private:
    HANDLE find = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW data{};
    bool pending = false;
};

bool IsPermissionDenied(int error) {
    return error == ERROR_ACCESS_DENIED;
}

} // namespace

#else

namespace {

uint64_t Nanoseconds(const struct timespec& value) {
    return (uint64_t)value.tv_sec * 1000000000u + (uint64_t)value.tv_nsec;
}

FileType TypeOf(mode_t mode) {
    return S_ISREG(mode) ? FileType::Regular : S_ISDIR(mode) ? FileType::Directory : FileType::Other;
}

} // namespace

bool StatusOf(const Path& path, FileStatus& status, int* error) {
    status = FileStatus();
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        if (error != nullptr) *error = errno;
        return false;
    }
    status.type = TypeOf(info.st_mode);
    status.size = (uint64_t)info.st_size;
#ifdef __APPLE__
    status.modified = Nanoseconds(info.st_mtimespec);
#else
    status.modified = Nanoseconds(info.st_mtim);
#endif
    return true;
}

uint64_t FileTimeNow() {
    struct timespec now {};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return Nanoseconds(now);
}

namespace {

bool MakeDirectory(const Path& path, int& error, bool& created) {
    created = ::mkdir(path.c_str(), 0777) == 0;
    if (created) return true;
    error = errno;
    if (error == EEXIST && IsDirectory(path)) error = 0;
    return error == 0;
}

bool IsNotFound(int error) {
    return error == ENOENT || error == ENOTDIR;
}

constexpr int kNotADirectory = ENOTDIR;
constexpr int kInvalidArgument = EINVAL;
constexpr int kNameTooLong = ENAMETOOLONG;

} // namespace

bool Remove(const Path& path) {
    return ::remove(path.c_str()) == 0;
}

bool Rename(const Path& from, const Path& to) {
    return ::rename(from.c_str(), to.c_str()) == 0;
}

bool SetWriteTimeNow(const Path& path) {
    const struct timespec times[2] = { { 0, UTIME_OMIT }, { 0, UTIME_NOW } };
    return ::utimensat(AT_FDCWD, path.c_str(), times, 0) == 0;
}

bool SetOwnerOnly(const Path& path) {
    return ::chmod(path.c_str(), 0700) == 0;
}

bool Absolute(const Path& path, Path& absolute) {
    if (path.empty()) return false;
    if (IsAbsolute(path)) {
        absolute = path;
        return true;
    }
    char* const folder = ::getcwd(nullptr, 0);
    if (folder == nullptr) return false;
    absolute.assign(folder);
    std::free(folder);
    AppendPath(absolute, path);
    return true;
}

Path Resolved(const Path& path) {
    // realpath(3): one call, where the standard library's filesystem::canonical splits and rebuilds the path a
    // component at a time.
    char* const resolved = ::realpath(path.c_str(), nullptr);
    if (resolved == nullptr) return Path();
    Path canonical(resolved);
    std::free(resolved);
    return canonical;
}

Path TempDirectory() {
    // The first of them that is set, even when empty (which is no folder).
    const char* folder = nullptr;
    for (const char* name : { "TMPDIR", "TMP", "TEMP", "TEMPDIR" }) {
        folder = std::getenv(name);
        if (folder != nullptr) break;
    }
    Path path = folder != nullptr ? folder : "/tmp";
    return IsDirectory(path) ? path : Path();
}

namespace {

// A folder's entries through readdir: its type from d_type where the file system gives it, else
// from fstatat; a link's from what it leads to.
class Listing {
public:
    bool Open(const Path& folder, int& error) {
        const int fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
        return Adopt(fd, error);
    }
    // The folder `name` of the folder `parent` lists, not through a link.
    bool OpenBelow(const Listing& parent, const char* name, int& error) {
        const int fd = ::openat(::dirfd(parent.directory), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
        return Adopt(fd, error);
    }
    ~Listing() {
        if (directory != nullptr) ::closedir(directory);
    }
    bool Next(Entry& entry, int& error) {
        for (;;) {
            errno = 0;
            const dirent* const found = ::readdir(directory);
            if (found == nullptr) {
                error = errno;
                return false;
            }
            const char* const name = found->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
            entry.name = PathView(name);
            entry.link = false;
            struct stat info {};
            switch (found->d_type) {
            case DT_REG: entry.type = FileType::Regular; return true;
            case DT_DIR: entry.type = FileType::Directory; return true;
            case DT_LNK: entry.link = true; break;
            case DT_UNKNOWN:
                if (::fstatat(::dirfd(directory), name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
                    entry.type = FileType::None;
                    return true;
                }
                entry.link = S_ISLNK(info.st_mode);
                if (!entry.link) {
                    entry.type = TypeOf(info.st_mode);
                    return true;
                }
                break;
            default: entry.type = FileType::Other; return true;
            }
            entry.type = ::fstatat(::dirfd(directory), name, &info, 0) == 0 ? TypeOf(info.st_mode) : FileType::None;
            return true;
        }
    }

private:
    bool Adopt(int fd, int& error) {
        if (fd >= 0) {
            directory = ::fdopendir(fd);
            if (directory != nullptr) return true;
            error = errno;
            ::close(fd);
            return false;
        }
        error = errno;
        return false;
    }

    DIR* directory = nullptr;
};

bool IsPermissionDenied(int error) {
    return error == EACCES;
}

} // namespace

#endif

bool IsDirectory(const Path& path) {
    FileStatus status;
    return StatusOf(path, status) && status.type == FileType::Directory;
}

bool IsRegularFile(const Path& path) {
    FileStatus status;
    return StatusOf(path, status) && status.type == FileType::Regular;
}

std::string ErrorText(int error) {
#ifdef _WIN32
    // As the C runtime's _wopendir, _wstat and _wmkdir set errno for them.
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME: error = ENOENT; break;
    case ERROR_ACCESS_DENIED: error = EACCES; break;
    case ERROR_DIRECTORY: error = ENOTDIR; break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS: error = EEXIST; break;
    case ERROR_FILENAME_EXCED_RANGE: error = ENAMETOOLONG; break;
    case ERROR_INVALID_PARAMETER: error = EINVAL; break;
    default: error = EIO; break;
    }
#endif
    return std::strerror(error);
}

bool CreateDirectories(const Path& path, int* error, bool* created) {
    // libstdc++'s create_directories: the missing folders found from the end, then made from the
    // first.
    int code = 0;
    bool made = false;
    if (created != nullptr) *created = false;
    const auto fail = [error](int value) {
        if (error != nullptr) *error = value;
        return false;
    };
    if (path.empty()) return fail(kInvalidArgument);
    FileStatus status;
    if (StatusOf(path, status, &code)) {
        if (status.type == FileType::Directory) return true;
        return fail(kNotADirectory);
    }
    if (!IsNotFound(code)) return fail(code);
    std::vector<Path> missing;
    Path current = path;
    // A separator at the end names the folder before it.
    if (RootLength(PathView(current)) < current.size() && FileNameOf(PathView(current)).empty()) {
        current.resize(ParentOf(PathView(current)).size());
    }
    for (;;) {
        const PathView name = FileNameOf(PathView(current));
        if (IsDot(name) || IsDotDot(name)) {
            current.resize(ParentOf(PathView(current)).size());
        } else {
            missing.push_back(current);
            if (missing.size() > 1000) return fail(kNameTooLong);
            current.resize(ParentOf(PathView(current)).size());
        }
        if (current.empty()) break;
        code = 0;
        if (StatusOf(current, status, &code)) {
            if (status.type != FileType::Directory) return fail(kNotADirectory);
            break;
        }
        // Another error (no permission to look): the folders are made, and mkdir says.
        if (!IsNotFound(code)) break;
    }
    while (!missing.empty()) {
        if (!MakeDirectory(missing.back(), code, made)) return fail(code);
        missing.pop_back();
    }
    if (created != nullptr) *created = made;
    return true;
}

bool ForEachEntry(const Path& folder, bool (*visit)(void*, const Entry&), void* context, int* error) {
    int code = 0;
    Listing listing;
    if (!listing.Open(folder, code)) {
        if (error != nullptr) *error = code;
        return false;
    }
    Entry entry;
#ifdef _WIN32
    while (listing.Next(entry)) {
#else
    while (listing.Next(entry, code)) {
#endif
        if (!visit(context, entry)) return true;
    }
    if (code != 0 && error != nullptr) *error = code;
    return code == 0;
}

bool Walk(const Path& root, WalkStep (*visit)(void*, const Entry&), void* context, int* error) {
    struct Level {
        Listing listing;
        size_t length;   // of `path` up to the folder's names
    };
    int code = 0;
    const auto fail = [error, &code]() {
        if (error != nullptr) *error = code;
        return false;
    };
    // `path`: the deepest open folder's, as AppendPath joins a name to it (with a separator unless
    // it ends in one or is a root name alone), then the entry's.
    Path path = root;
    const auto folderLength = [&path]() {
        if (path.size() > RootLength(PathView(path)) && !IsSeparator(path.back())) path += PathChar(kSeparator);
        return path.size();
    };
    // Each open folder's listing, the deepest last.
    std::vector<std::unique_ptr<Level>> levels;
    levels.push_back(std::make_unique<Level>());
    if (!levels.back()->listing.Open(root, code)) return IsPermissionDenied(code) || fail();
    levels.back()->length = folderLength();
    Entry entry;
    while (!levels.empty()) {
        Level& level = *levels.back();
#ifdef _WIN32
        const bool next = level.listing.Next(entry);
#else
        const bool next = level.listing.Next(entry, code);
        if (!next && code != 0) return fail();
#endif
        if (!next) {
            levels.pop_back();
            continue;
        }
        path.resize(level.length);
        path.append(entry.name);
        entry.depth = (int)levels.size() - 1;
        entry.path = &path;
        const WalkStep step = visit(context, entry);
        if (step == WalkStep::Stop) return true;
        if (step != WalkStep::Enter || entry.type != FileType::Directory || entry.link) continue;
        std::unique_ptr<Level> inner = std::make_unique<Level>();
#ifdef _WIN32
        const bool opened = inner->listing.Open(path, code);
#else
        const bool opened = inner->listing.OpenBelow(level.listing, path.c_str() + level.length, code);
#endif
        if (!opened) {
            if (IsPermissionDenied(code)) continue;
            return fail();
        }
        inner->length = folderLength();
        levels.push_back(std::move(inner));
    }
    return true;
}

} // namespace RayoMd::File
