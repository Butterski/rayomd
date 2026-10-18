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
#endif

#include <cstdlib>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif
#include <algorithm>
#include <system_error>
#include <tuple>

namespace TinyPdf::Internal {

namespace fs = std::filesystem;
using RayoMd::File::PathFromUtf8;
using RayoMd::File::PathToUtf8;

namespace {

char LowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

std::string LowerAscii(std::string_view text) {
    std::string lower(text);
    for (char& c : lower) c = LowerAscii(c);
    return lower;
}

// A path below the root as '/'-separated UTF-8.
std::string Generic(const fs::path& path) {
    std::string text = PathToUtf8(path);
#ifdef _WIN32
    std::replace(text.begin(), text.end(), '\\', '/');
#endif
    return text;
}

// Whether a file name ends in ".md" or ".markdown", in any case.
bool IsMarkdownName(std::string_view name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    const std::string extension = LowerAscii(name.substr(dot + 1));
    return extension == "md" || extension == "markdown";
}

} // namespace

fs::path ResolvedPath(const fs::path& path) {
#ifdef _WIN32
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
    return fs::path(name);
#else
    // realpath(3): one call; std::filesystem::canonical splits and rebuilds the path a component at
    // a time (most of an embed's cost).
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (resolved == nullptr) return fs::path();
    fs::path canonical(resolved);
    std::free(resolved);
    return canonical;
#endif
}

bool IsWithinRoot(const fs::path& file, const fs::path& root) {
    auto at = file.begin();
    for (const fs::path& part : root) {
        if (at == file.end()) return false;
#ifdef _WIN32
        const std::wstring& a = at->native();
        const std::wstring& b = part.native();
        const auto fold = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? (wchar_t)(c - L'A' + L'a') : c; };
        if (a.size() != b.size() || !std::equal(a.begin(), a.end(), b.begin(), [&](wchar_t x, wchar_t y) { return fold(x) == fold(y); })) {
            return false;
        }
#else
        if (at->native() != part.native()) return false;
#endif
        ++at;
    }
    return at != file.end();
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

Vault::Vault(const std::string& vaultRoot, const std::string& notePath) {
    fs::path folder;
    if (!vaultRoot.empty()) {
        folder = PathFromUtf8(vaultRoot);
    } else {
        folder = PathFromUtf8(notePath).parent_path();
        if (folder.empty()) folder = ".";
    }
    root = ResolvedPath(folder);
    std::error_code ec;
    if (!root.empty() && !fs::is_directory(root, ec)) root.clear();
#if defined(__linux__) && defined(SYS_openat2)
    if (!root.empty()) rootFd = ::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
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

// The file at `fd` whole, at most `maximum` bytes, as RayoMd::File::ReadAll reads one; closes it.
bool ReadOpen(int fd, size_t maximum, std::string& text, RayoMd::File::FileIdentity& identity) {
    struct stat status {};
    const bool regular = ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode);
    identity.opened = regular;
    identity.volume = (uint64_t)status.st_dev;
    identity.index = (uint64_t)status.st_ino;
    identity.size = (uint64_t)status.st_size;
    bool whole = regular && identity.size <= maximum;
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

} // namespace

// `relative` (below the root) opened beneath the root: a descriptor, -1 when it names no file
// there (or leaves the root, by "..", a symbolic link or a magic link), -2 when openat2 itself is
// refused (an old kernel, a seccomp filter), the root's descriptor then given up.
int Vault::OpenBeneath(const fs::path& relative) {
#if defined(__linux__) && defined(SYS_openat2)
    struct {
        uint64_t flags;
        uint64_t mode;
        uint64_t resolve;
    } how{ (uint64_t)(O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK), 0, 0x08 /* RESOLVE_BENEATH */ | 0x02 /* RESOLVE_NO_MAGICLINKS */ };
    const long fd = ::syscall(SYS_openat2, rootFd, relative.c_str(), &how, sizeof(how));
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

bool Vault::Read(const fs::path& path, size_t maximum, std::string& text, RayoMd::File::FileIdentity& identity) {
#ifndef _WIN32
    if (openFd >= 0 && openPath == path) {
        const int fd = openFd;
        openFd = -1;
        return ReadOpen(fd, maximum, text, identity);
    }
    if (rootFd >= 0) {
        const fs::path relative = path.lexically_normal().lexically_relative(root);
        if (!relative.empty() && *relative.begin() != ".." && relative != ".") {
            const int fd = OpenBeneath(relative);
            if (fd >= 0) return ReadOpen(fd, maximum, text, identity);
            if (fd == -1) {
                identity = RayoMd::File::FileIdentity();
                return false;
            }
        }
    }
#endif
    return RayoMd::File::ReadAll(path, maximum, text, identity);
}

// `candidate` once checked, when it is a regular file inside the root; else empty. Through the
// root's descriptor (Linux), the path below the root as written, the file left open for Read;
// else resolved.
fs::path Vault::Check(const fs::path& candidate) {
    const fs::path lexical = candidate.lexically_normal();
#ifndef _WIN32
    if (rootFd >= 0) {
        const fs::path relative = lexical.lexically_relative(root);
        if (relative.empty() || *relative.begin() == ".." || relative == ".") return fs::path();
        const int fd = OpenBeneath(relative);
        if (fd >= 0) {
            struct stat status {};
            if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
                ::close(fd);
                return fs::path();
            }
            if (openFd >= 0) ::close(openFd);
            openFd = fd;
            openPath = lexical;
            return lexical;
        }
        if (fd == -1) return fs::path();
    }
#endif
#ifdef _WIN32
    // In the root by name before anything is opened, and through no link below it (TryOpenDirect's
    // rule for images); the file itself no link either. Wildcards are looked for below the root
    // only: the root, from GetFinalPathNameByHandleW, begins with "\\?\".
    if (!IsWithinRoot(lexical, root) || lexical.lexically_relative(root).native().find_first_of(L"*?") != std::wstring::npos ||
        DirectPathHasLink(root, lexical)) {
        return fs::path();
    }
    const DWORD attributes = GetFileAttributesW(lexical.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return fs::path();
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileExW(lexical.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) return fs::path();
        FindClose(find);
        if (IsReparseTagNameSurrogate(entry.dwReserved0)) return fs::path();
    }
#endif
    fs::path resolved = ResolvedPath(lexical);
    std::error_code ec;
    if (resolved.empty() || !IsWithinRoot(resolved, root) || !fs::is_regular_file(resolved, ec)) return fs::path();
    return resolved;
}

fs::path Vault::Find(const fs::path& from, std::string_view target, bool byName) {
    while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) target.remove_prefix(1);
    while (!target.empty() && (target.back() == ' ' || target.back() == '\t' || target.back() == '\r')) target.remove_suffix(1);
    if (root.empty() || target.empty() || target.find('\0') != std::string_view::npos) return fs::path();
    std::string name(target);
    const size_t slash = name.find_last_of("/\\");
    if (!IsMarkdownName(slash == std::string::npos ? std::string_view(name) : std::string_view(name).substr(slash + 1))) {
        name += ".md";
    }
    const fs::path relative = PathFromUtf8(name);
    if (relative.has_root_path() || name[0] == '/' || name[0] == '\\') return fs::path();
    for (const fs::path* base : { &from, static_cast<const fs::path*>(&root) }) {
        if (base->empty()) continue;
        fs::path found = Check(*base / relative);
        if (!found.empty()) return found;
    }
    if (!byName) return fs::path();
    if (!indexed) BuildIndex();
    if (!indexValid) return fs::path();
    // The wanted path's last components, as the index keeps them.
    std::string wanted = LowerAscii(Generic(relative.lexically_normal()));
    while (wanted.compare(0, 2, "./") == 0) wanted.erase(0, 2);
    if (wanted.empty() || wanted.compare(0, 3, "../") == 0) return fs::path();
    const size_t last = wanted.rfind('/');
    const std::string file = last == std::string::npos ? wanted : wanted.substr(last + 1);
    auto at = std::lower_bound(index.begin(), index.end(), file, [](const Named& named, const std::string& key) { return named.name < key; });
    for (; at != index.end() && at->name == file; ++at) {
        if (last != std::string::npos) {
            const std::string lower = LowerAscii(at->relative);
            if (lower.size() < wanted.size() || lower.compare(lower.size() - wanted.size(), wanted.size(), wanted) != 0 ||
                (lower.size() > wanted.size() && lower[lower.size() - wanted.size() - 1] != '/')) {
                continue;
            }
        }
        fs::path found = Check(root / PathFromUtf8(at->relative));
        if (!found.empty()) return found;
    }
    return fs::path();
}

void Vault::BuildIndex() {
    indexed = true;
    std::error_code ec;
    size_t entries = 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        if (++entries > kMaxIndexEntries) {
            index.clear();
            return;
        }
        const std::string name = PathToUtf8(it->path().filename());
        std::error_code typeError;
        if (it->is_directory(typeError)) {
            // Obsidian's settings, its trash, a repository's history: hidden folders hold no notes.
            if ((!name.empty() && name[0] == '.') || it.depth() + 1 >= kMaxIndexDepth) it.disable_recursion_pending();
            continue;
        }
        if (name.empty() || name[0] == '.' || !IsMarkdownName(name)) continue;
        index.push_back({ LowerAscii(name), Generic(it->path().lexically_relative(root)), (size_t)it.depth() });
    }
    std::sort(index.begin(), index.end(), [](const Named& a, const Named& b) {
        return std::tie(a.name, a.depth, a.relative) < std::tie(b.name, b.depth, b.relative);
    });
    indexValid = true;
}

} // namespace TinyPdf::Internal
