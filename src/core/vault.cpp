// SPDX-License-Identifier: Apache-2.0

#include "vault.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif
#include <algorithm>
#include <cstring>
#include <system_error>
#include <tuple>

namespace TinyPdf::Internal {

namespace fs = std::filesystem;
using RayoMd::File::PathFromUtf8;
using RayoMd::File::PathToUtf8;

namespace {

#ifdef _WIN32
constexpr char kSeparator = '\\';
bool IsSeparator(char c) {
    return c == '/' || c == '\\';
}
#else
constexpr char kSeparator = '/';
bool IsSeparator(char c) {
    return c == '/';
}
#endif

char LowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

// The first eight bytes of a name, big-endian and zero-padded: ordered as the names are.
uint64_t KeyOf(std::string_view name) {
    uint64_t key = 0;
    for (size_t at = 0; at < 8; at++) key = key << 8 | (at < name.size() ? (unsigned char)name[at] : 0u);
    return key;
}

// Whether a file name ends in ".md" or ".markdown", in any case.
bool IsMarkdownName(std::string_view name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view extension = name.substr(dot + 1);
    const std::string_view wanted = extension.size() == 2 ? "md" : "markdown";
    if (extension.size() != wanted.size()) return false;
    for (size_t at = 0; at < extension.size(); at++) {
        if (LowerAscii(extension[at]) != wanted[at]) return false;
    }
    return true;
}

// The bytes of `path` that ".." never takes out: "/" (POSIX); a drive "C:" or a share's server
// "\\server" and the separator after it (Windows); none for a relative path.
size_t RootLength(std::string_view path) {
#ifdef _WIN32
    size_t length = 0;
    if (path.size() >= 2 && IsSeparator(path[0]) && IsSeparator(path[1])) {
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

// `path`, which ends in a file name, as lexically_normal() writes it, in place: the platform's
// separators, one between components, no "." component, each ".." with the component before it
// taken out (one at the root dropped). The text only moves forward, so one pass does.
void Normalize(std::string& path) {
    const size_t rootLength = RootLength(path);
    char* const text = path.data();
    for (size_t at = 0; at < rootLength; at++) {
        if (IsSeparator(text[at])) text[at] = kSeparator;
    }
    size_t out = rootLength;
    size_t kept = 0;   // components written that a ".." takes out
    for (size_t at = rootLength; at < path.size();) {
        size_t end = at;
        while (end < path.size() && !IsSeparator(text[end])) end++;
        const size_t start = at;
        const size_t length = end - start;
        at = end + 1;
        if (length == 0 || (length == 1 && text[start] == '.')) continue;
        if (length == 2 && text[start] == '.' && text[start + 1] == '.') {
            if (kept > 0) {
                while (out > rootLength && text[out - 1] != kSeparator) out--;
                if (out > rootLength) out--;
                kept--;
                continue;
            }
            if (rootLength > 0) continue;
        } else {
            kept++;
        }
        if (out > rootLength) text[out++] = kSeparator;
        std::memmove(text + out, text + start, length);
        out += length;
    }
    path.resize(out);
}

// Where `path` (normalized or resolved) continues below the resolved folder `root`: the offset of
// its first component there; 0 when it does not lie in the root.
size_t BelowRoot(std::string_view path, std::string_view root) {
    if (root.empty() || path.size() <= root.size()) return 0;
#ifdef _WIN32
    for (size_t at = 0; at < root.size(); at++) {
        if (LowerAscii(path[at]) != LowerAscii(root[at])) return 0;
    }
#else
    if (std::memcmp(path.data(), root.data(), root.size()) != 0) return 0;
#endif
    if (IsSeparator(root.back())) return root.size();
    return IsSeparator(path[root.size()]) && path.size() > root.size() + 1 ? root.size() + 1 : 0;
}

bool IsRegularFile(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(PathFromUtf8(path), ec);
}

} // namespace

#ifdef _WIN32
fs::path ResolvedWidePath(const fs::path& path) {
    const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return fs::path();
    std::wstring name(MAX_PATH, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, name.data(), (DWORD)name.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= name.size()) {
        name.resize(length + 1);
        length = GetFinalPathNameByHandleW(handle, name.data(), (DWORD)name.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(handle);
    if (length == 0 || length >= name.size()) return fs::path();
    name.resize(length);
    // The answer begins "\\?\C:\" or "\\?\UNC\server\share\", which std::filesystem reads as a root
    // name "\\?" and no call opens once lexically_normal made it "\?\": the path as the other
    // Windows calls write it.
    if (name.compare(0, 8, L"\\\\?\\UNC\\") == 0) name.replace(0, 8, L"\\\\");
    else if (name.compare(0, 4, L"\\\\?\\") == 0) name.erase(0, 4);
    return fs::path(std::move(name));
}
#endif

std::string ResolvedPath(std::string_view path) {
#ifdef _WIN32
    const fs::path resolved = ResolvedWidePath(PathFromUtf8(path));
    return resolved.empty() ? std::string() : PathToUtf8(resolved);
#else
    // realpath(3): one call; std::filesystem::canonical splits and rebuilds the path a component at
    // a time.
    char* resolved = ::realpath(std::string(path).c_str(), nullptr);
    if (resolved == nullptr) return std::string();
    std::string canonical(resolved);
    std::free(resolved);
    return canonical;
#endif
}

std::string_view ParentOf(std::string_view path) {
    const size_t rootLength = RootLength(path);
    size_t end = path.size();
    while (end > rootLength && !IsSeparator(path[end - 1])) end--;
    if (end > rootLength) end--;
    return path.substr(0, end);
}

bool IsWithinRoot(std::string_view file, std::string_view root) {
    return BelowRoot(file, root) != 0;
}

#ifdef _WIN32
// One attribute query a folder (GetFileAttributesW reports a link's own attributes, not its
// target's, and opens nothing beyond it); only a reparse point has its tag read from its entry.
// FindFirstFileExW for every folder made each image of a document in a subfolder cost about
// 20 µs a level and build on Windows (tester.md, three levels: +18 % a build).
bool DirectPathHasLink(const fs::path& root, const fs::path& path) {
    auto part = path.begin();
    for (auto rootPart = root.begin(); rootPart != root.end() && part != path.end(); ++rootPart) ++part;
    fs::path current = root;
    for (; part != path.end() && std::next(part) != path.end(); ++part) {
        current /= *part;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return true;
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) continue;
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileExW(current.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) return true;
        FindClose(find);
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(entry.dwReserved0)) {
            return true;
        }
    }
    return false;
}
#endif

Vault::Vault(const std::string& vaultRoot, const std::string& notePath, size_t maxIndexEntries)
    : indexLimit(maxIndexEntries) {
    std::string_view folder = vaultRoot;
    if (folder.empty()) folder = ParentOf(notePath);
    root = ResolvedPath(folder.empty() ? std::string_view(".") : folder);
    if (root.empty()) return;
#if defined(__linux__) && defined(SYS_openat2)
    // O_DIRECTORY: a root that opens is a folder, and the stat below is not needed.
    rootFd = ::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (rootFd >= 0) return;
#endif
    std::error_code ec;
    if (!fs::is_directory(PathFromUtf8(root), ec)) root.clear();
#ifdef _WIN32
    else rootPath = PathFromUtf8(root);
#endif
}

Vault::~Vault() {
#ifndef _WIN32
    if (openFd >= 0) ::close(openFd);
    if (rootFd >= 0) ::close(rootFd);
#endif
}

#ifndef _WIN32
namespace {

// The regular file open at `fd`, of `identity` (its fstat), whole, at most `maximum` bytes, as
// RayoMd::File::ReadAll reads one: its size, one read; closes it.
bool ReadOpen(int fd, const RayoMd::File::FileIdentity& opened, size_t maximum, std::string& text,
    RayoMd::File::FileIdentity& identity) {
    identity = opened;
    const bool whole = identity.size <= maximum;
    if (whole) {
        text.resize((size_t)identity.size);
        size_t got = 0;
        while (got < text.size()) {
            const ssize_t count = ::read(fd, &text[got], text.size() - got);
            if (count <= 0) break;
            got += (size_t)count;
        }
        text.resize(got);
    }
    ::close(fd);
    return whole;
}

// What fstat says of the file open at `fd`; `opened` false when it is no regular file.
RayoMd::File::FileIdentity IdentityOf(int fd) {
    struct stat status {};
    RayoMd::File::FileIdentity identity;
    identity.opened = ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode);
    identity.volume = (uint64_t)status.st_dev;
    identity.index = (uint64_t)status.st_ino;
    identity.size = (uint64_t)status.st_size;
    return identity;
}

} // namespace

// `relative` (below the root) opened beneath the root: a descriptor, -1 when it names no file
// there (or leaves the root, by "..", a symbolic link or a magic link), -2 when openat2 itself is
// refused (an old kernel, a seccomp filter), the root's descriptor then given up.
int Vault::OpenBeneath(const char* relative) {
#if defined(__linux__) && defined(SYS_openat2)
    struct {
        uint64_t flags;
        uint64_t mode;
        uint64_t resolve;
    } how{ (uint64_t)(O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK), 0, 0x08 /* RESOLVE_BENEATH */ | 0x02 /* RESOLVE_NO_MAGICLINKS */ };
    const long fd = ::syscall(SYS_openat2, rootFd, relative, &how, sizeof(how));
    if (fd >= 0) return (int)fd;
    if (errno == ENOSYS || errno == EPERM || errno == EINVAL || errno == E2BIG) {
        ::close(rootFd);
        rootFd = -1;
        return -2;
    }
    return -1;
#else
    (void)relative;
    return -2;
#endif
}
#endif

bool Vault::Read(const std::string& path, size_t maximum, std::string& text, RayoMd::File::FileIdentity& identity) {
#ifndef _WIN32
    if (openFd >= 0 && openPath == path) {
        const int fd = openFd;
        openFd = -1;
        return ReadOpen(fd, openIdentity, maximum, text, identity);
    }
    if (rootFd >= 0) {
        const size_t below = BelowRoot(path, root);
        if (below != 0) {
            const int fd = OpenBeneath(path.c_str() + below);
            if (fd >= 0) {
                const RayoMd::File::FileIdentity opened = IdentityOf(fd);
                if (opened.opened) return ReadOpen(fd, opened, maximum, text, identity);
                ::close(fd);
                identity = opened;
                return false;
            }
            if (fd == -1) {
                identity = RayoMd::File::FileIdentity();
                return false;
            }
        }
    }
#endif
    return RayoMd::File::ReadAll(PathFromUtf8(path), maximum, text, identity);
}

// Whether `path` (normalized) is a regular file inside the root: through the root's descriptor
// (Linux) as written, the file left open for Read; else resolved, and `path` then the resolved path.
bool Vault::Check(std::string& path) {
#ifndef _WIN32
    if (rootFd >= 0) {
        const size_t below = BelowRoot(path, root);
        if (below == 0) return false;
        const int fd = OpenBeneath(path.c_str() + below);
        if (fd >= 0) {
            const RayoMd::File::FileIdentity opened = IdentityOf(fd);
            if (!opened.opened) {
                ::close(fd);
                return false;
            }
            if (openFd >= 0) ::close(openFd);
            openFd = fd;
            openIdentity = opened;
            openPath.assign(path);
            return true;
        }
        if (fd == -1) return false;
    }
#else
    // In the root by name before anything is opened, and through no link below it (TryOpenDirect's
    // rule for images); the file itself no link either.
    if (BelowRoot(path, root) == 0 || path.find_first_of("*?") != std::string::npos) return false;
    const fs::path file = PathFromUtf8(path);
    if (DirectPathHasLink(rootPath, file)) return false;
    const DWORD attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileExW(file.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) return false;
        FindClose(find);
        if (IsReparseTagNameSurrogate(entry.dwReserved0)) return false;
    }
#endif
    std::string resolved = ResolvedPath(path);
    if (resolved.empty() || !IsWithinRoot(resolved, root) || !IsRegularFile(resolved)) return false;
    path = std::move(resolved);
    return true;
}

std::string Vault::Find(std::string_view from, std::string_view target, bool byName) {
    while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) target.remove_prefix(1);
    while (!target.empty() && (target.back() == ' ' || target.back() == '\t' || target.back() == '\r')) target.remove_suffix(1);
    if (root.empty() || target.empty() || target.find('\0') != std::string_view::npos) return std::string();
    // No absolute path, nor one of a drive (std::filesystem's root name "C:" on Windows).
    if (target[0] == '/' || target[0] == '\\') return std::string();
#ifdef _WIN32
    if (target.size() >= 2 && target[1] == ':') return std::string();
#endif
    const size_t slash = target.find_last_of("/\\");
    const bool addExtension = !IsMarkdownName(slash == std::string_view::npos ? target : target.substr(slash + 1));
    // A name without a "." or empty component below a resolved folder is a normal path as it is.
#ifdef _WIN32
    const bool plain = false;   // its '/' become '\'
#else
    bool plain = target[0] != '.';
    for (size_t at = 0; plain && at + 1 < target.size(); at++) plain = target[at] != '/' || (target[at + 1] != '.' && target[at + 1] != '/');
#endif
    std::string path;
    path.reserve(std::max(from.size(), root.size()) + target.size() + 4);
    for (const std::string_view base : { from, std::string_view(root) }) {
        if (base.empty()) continue;
        path.assign(base);
        if (!IsSeparator(base.back())) path += kSeparator;   // "/" or "C:\" ends in one
        path.append(target);
        if (addExtension) path += ".md";
        if (!plain) Normalize(path);
        if (Check(path)) return path;
    }
    if (!byName) return std::string();
    if (!indexed) BuildIndex();
    if (!indexValid) return std::string();
    // The wanted path's last components, as the index keeps them.
    std::string wanted(target);
    if (addExtension) wanted += ".md";
    Normalize(wanted);
    for (char& c : wanted) c = IsSeparator(c) ? '/' : LowerAscii(c);
    if (wanted.empty() || wanted.compare(0, 3, "../") == 0) return std::string();
    const size_t last = wanted.rfind('/');
    const std::string_view file = last == std::string::npos ? std::string_view(wanted) : std::string_view(wanted).substr(last + 1);
    const uint64_t key = KeyOf(file);
    auto at = std::lower_bound(index.begin(), index.end(), file, [this, key](const Named& named, std::string_view name) {
        return named.key != key ? named.key < key : Name(named) < name;
    });
    for (; at != index.end() && Name(*at) == file; ++at) {
        if (last != std::string::npos) {
            // Its path ends in the wanted components, letter case aside.
            const std::string_view lower = Lower(*at);
            if (lower.size() < wanted.size() || lower.substr(lower.size() - wanted.size()) != wanted ||
                (lower.size() > wanted.size() && lower[lower.size() - wanted.size() - 1] != '/')) {
                continue;
            }
        }
        path.assign(root);
        if (!IsSeparator(root.back())) path += kSeparator;
        path.append(Relative(*at));
#ifdef _WIN32
        Normalize(path);
#endif
        if (Check(path)) return path;
    }
    return std::string();
}

#ifndef _WIN32
// The entries of the folder open at `folder` (its path below the root `relative`, '/'-ended, or
// empty for the root; `depth` folders down), as std::filesystem::recursive_directory_iterator with
// skip_permission_denied visits them (a folder's type from its entry, links to folders neither
// indexed nor entered), without a path object a name; closes `folder`. `entries` counts them: past
// indexLimit the index is cleared and the walk ends.
void Vault::Walk(int folder, std::string& relative, int depth, size_t& entries) {
    DIR* const directory = ::fdopendir(folder);
    if (directory == nullptr) {
        ::close(folder);
        return;
    }
    const size_t length = relative.size();
    while (const dirent* entry = ::readdir(directory)) {
        const char* const name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        if (++entries > indexLimit) break;
        unsigned char type = entry->d_type;
        struct stat status {};
        if (type == DT_UNKNOWN && ::fstatat(::dirfd(directory), name, &status, AT_SYMLINK_NOFOLLOW) == 0) {
            type = S_ISDIR(status.st_mode) ? DT_DIR : S_ISLNK(status.st_mode) ? DT_LNK : DT_REG;
        }
        // A link to a folder is a folder to the index (is_directory follows it), never entered.
        const bool isFolder = type == DT_DIR ||
            (type == DT_LNK && ::fstatat(::dirfd(directory), name, &status, 0) == 0 && S_ISDIR(status.st_mode));
        if (isFolder) {
            // Obsidian's settings, its trash, a repository's history: hidden folders hold no notes.
            if (type != DT_DIR || name[0] == '.' || depth + 1 >= kMaxIndexDepth) continue;
            const int inner = ::openat(::dirfd(directory), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (inner < 0) continue;
            relative.append(name);
            relative += '/';
            Walk(inner, relative, depth + 1, entries);
            relative.resize(length);
            if (entries > indexLimit) break;
            continue;
        }
        const size_t nameSize = std::strlen(name);
        if (name[0] == '.' || !IsMarkdownName(std::string_view(name, nameSize))) continue;
        relative.append(name, nameSize);
        AddNamed(relative, nameSize, (size_t)depth);
        relative.resize(length);
    }
    ::closedir(directory);
}
#endif

void Vault::AddNamed(std::string_view relative, size_t nameSize, size_t depth) {
    const size_t at = names.size();
    names.append(relative);
    names.append(relative);
    for (size_t lower = at + relative.size(); lower < names.size(); lower++) names[lower] = LowerAscii(names[lower]);
    index.push_back({ KeyOf(std::string_view(names).substr(names.size() - nameSize)), (uint32_t)at, (uint32_t)relative.size(),
        (uint32_t)nameSize, (uint32_t)depth });
}

void Vault::BuildIndex() {
    indexed = true;
    size_t entries = 0;
#ifndef _WIN32
    const int folder = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder >= 0) {
        std::string relative;
        Walk(folder, relative, 0, entries);
    }
#else
    std::error_code ec;
    fs::recursive_directory_iterator it(rootPath, fs::directory_options::skip_permission_denied, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        if (++entries > indexLimit) break;
        const std::string name = PathToUtf8(it->path().filename());
        std::error_code typeError;
        if (it->is_directory(typeError)) {
            // Obsidian's settings, its trash, a repository's history: hidden folders hold no notes.
            if ((!name.empty() && name[0] == '.') || it.depth() + 1 >= kMaxIndexDepth) it.disable_recursion_pending();
            continue;
        }
        if (name.empty() || name[0] == '.' || !IsMarkdownName(name)) continue;
        const std::string path = PathToUtf8(it->path());
        std::string relative = path.substr(BelowRoot(path, root));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        AddNamed(relative, name.size(), (size_t)it.depth());
    }
#endif
    if (entries > indexLimit) {
        index.clear();
        names.clear();
        return;
    }
    std::sort(index.begin(), index.end(), [this](const Named& a, const Named& b) {
        if (a.key != b.key) return a.key < b.key;
        const std::string_view x = Name(a);
        const std::string_view y = Name(b);
        if (x != y) return x < y;
        return a.depth != b.depth ? a.depth < b.depth : Relative(a) < Relative(b);
    });
    indexValid = true;
}

} // namespace TinyPdf::Internal
