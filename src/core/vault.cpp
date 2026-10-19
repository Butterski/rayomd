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
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <system_error>
#include <unordered_map>

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

bool IsDotDot(const char* name, size_t length) {
    return length == 2 && name[0] == '.' && name[1] == '.';
}

} // namespace

// libstdc++'s lexically_normal step by step: a name is appended after a separator unless the text
// ends in one; "." and a trailing empty name add a separator after a name; ".." takes out the name
// before it (or the name before a trailing separator), is dropped at a root directory and kept
// after another "..". Output never outruns the input, so it is written over it.
void Normalize(std::string& path) {
    if (path.empty()) return;
    const size_t size = path.size();
    const size_t rootLength = RootLength(path);
    char* const text = path.data();
    for (size_t at = 0; at < rootLength; at++) {
        if (IsSeparator(text[at])) text[at] = kSeparator;
    }
    const bool rootDirectory = rootLength > 0 && text[rootLength - 1] == kSeparator;
    size_t out = rootLength;
    const auto nameStart = [&](size_t end) {
        while (end > rootLength && text[end - 1] != kSeparator) end--;
        return end;
    };
    const auto endsInName = [&]() { return out > rootLength && text[out - 1] != kSeparator; };
    const auto append = [&](size_t start, size_t length) {
        if (endsInName()) text[out++] = kSeparator;
        std::memmove(text + out, text + start, length);
        out += length;
    };
    for (size_t at = rootLength; at < size;) {
        const size_t start = at;
        while (at < size && !IsSeparator(text[at])) at++;
        const size_t length = at - start;
        const bool separated = at < size;
        while (at < size && IsSeparator(text[at])) at++;
        if (length == 1 && text[start] == '.') {
            if (endsInName()) text[out++] = kSeparator;
        } else if (IsDotDot(text + start, length)) {
            if (endsInName()) {
                const size_t name = nameStart(out);
                if (IsDotDot(text + name, out - name)) append(start, 2);
                else out = name;
            } else if (out == rootLength) {
                if (!rootDirectory) append(start, 2);
            } else {
                const size_t name = nameStart(out - 1);
                if (IsDotDot(text + name, out - 1 - name)) append(start, 2);
                else out = name;
            }
        } else if (length > 0) {
            append(start, length);
        }
        // A separator ending the path is an empty last name.
        if (separated && at == size && endsInName()) text[out++] = kSeparator;
    }
    if (out > rootLength && text[out - 1] == kSeparator) {
        const size_t name = nameStart(out - 1);
        if (IsDotDot(text + name, out - 1 - name)) out--;
    }
    if (out == 0) text[out++] = '.';
    path.resize(out);
}

namespace {

// A path's root ("C:\", "\\server\", "/"; root name and root directory) and its names after it, as
// std::filesystem::path's iterator gives them: each name, then an empty one when separators end the
// path after a name.
struct PathNames {
    std::string_view path;
    size_t rootLength;
    size_t at;
    bool afterName = false;

    explicit PathNames(std::string_view text) : path(text), rootLength(RootLength(text)), at(rootLength) {}
    bool RootDirectory() const { return rootLength > 0 && IsSeparator(path[rootLength - 1]); }
    std::string_view RootName() const { return path.substr(0, rootLength - RootDirectory()); }
    bool Absolute() const {
#ifdef _WIN32
        return RootDirectory() && rootLength > 1;
#else
        return RootDirectory();
#endif
    }
    bool Next(std::string_view& name) {
        name = std::string_view();
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

} // namespace

std::string RelativePath(std::string_view path, std::string_view base) {
    PathNames from(path);
    PathNames to(base);
    std::string out;
    if (from.RootName() != to.RootName() || from.Absolute() != to.Absolute() || (!from.RootDirectory() && to.RootDirectory())) return out;
    // A root directory only `path` has (Windows: "\a" against "b") is a mismatch there, and
    // appending it takes back the ".." before it.
    const bool rooted = from.RootDirectory() && !to.RootDirectory();
    std::string_view a, b;
    bool hasA = from.Next(a);
    bool hasB = to.Next(b);
    while (!rooted && hasA && hasB && a == b) {
        hasA = from.Next(a);
        hasB = to.Next(b);
    }
    int up = 0;
    for (; hasB; hasB = to.Next(b)) up += b == ".." ? -1 : !b.empty() && b != ".";
    if (!rooted && up == 0 && a.empty()) return ".";
    if (up < 0) return out;
    if (rooted) out += kSeparator;
    for (; !rooted && up > 0; up--) out.append(out.empty() ? 0 : 1, kSeparator).append("..");
    for (; hasA; hasA = from.Next(a)) {
        if (!out.empty() && out.back() != kSeparator) out += kSeparator;
        out.append(a);
    }
    return out;
}

std::string_view FileNameOf(std::string_view path) {
    size_t start = path.size();
    while (start > 0 && !IsSeparator(path[start - 1])) start--;
    return path.substr(std::max(start, std::min(RootLength(path), path.size())));
}

std::string_view StemOf(std::string_view path) {
    const std::string_view name = FileNameOf(path);
    if (name == "." || name == "..") return name;
    const size_t dot = name.rfind('.');
    return dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot);
}

namespace {

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
    while (end > rootLength && IsSeparator(path[end - 1])) end--;
    return path.substr(0, end);
}

void AppendPath(std::string& base, std::string_view path) {
    const size_t rootLength = RootLength(path);
    const bool rootDirectory = rootLength > 0 && IsSeparator(path[rootLength - 1]);
    const std::string_view rootName = path.substr(0, rootLength - rootDirectory);
    const size_t baseRoot = RootLength(base);
    const size_t baseName = baseRoot - (baseRoot > 0 && IsSeparator(base[baseRoot - 1]));
#ifdef _WIN32
    const bool absolute = rootDirectory && !rootName.empty();
#else
    const bool absolute = rootDirectory;
#endif
    if (absolute || (!rootName.empty() && rootName != std::string_view(base).substr(0, baseName))) {
        base.assign(path);
        return;
    }
    if (rootDirectory) base.resize(baseName);
    else if (base.size() > baseRoot && !IsSeparator(base.back())) base += kSeparator;
    base.append(path.substr(rootName.size()));
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

// A note of the name index: its path below the root, '/'-separated, at names[at] as written and
// right after that in ASCII lower case; its name the last `nameSize` bytes of it, whose first
// eight are `key` (big-endian, zero-padded), which orders most names without reading them.
struct NameIndex {
    struct Named {
        uint64_t key;
        uint32_t at;
        uint32_t size;
        uint32_t nameSize;
        uint32_t depth;   // its folders below the root
    };

    std::string key;            // the root and the limit (KeptIndex)
    std::vector<Named> index;   // by name, then depth, then relative path
    std::string names;          // their paths
    // The folders the walk read, below the root ('/'-ended, empty for the root), each as it was
    // then; and whether the index may be kept: no link, no folder that could not be read, none
    // written within the settle time.
    std::vector<std::pair<std::string, RayoMd::File::FileIdentity>> folders;
    bool keep = true;

    std::string_view Relative(const Named& named) const { return std::string_view(names.data() + named.at, named.size); }
    std::string_view Lower(const Named& named) const { return std::string_view(names.data() + named.at + named.size, named.size); }
    std::string_view Name(const Named& named) const {
        return std::string_view(names.data() + named.at + 2 * named.size - named.nameSize, named.nameSize);
    }
    void Add(std::string_view relative, size_t nameSize, size_t depth) {
        const size_t at = names.size();
        names.append(relative);
        names.append(relative);
        for (size_t lower = at + relative.size(); lower < names.size(); lower++) names[lower] = LowerAscii(names[lower]);
        index.push_back({ KeyOf(std::string_view(names).substr(names.size() - nameSize)), (uint32_t)at, (uint32_t)relative.size(),
            (uint32_t)nameSize, (uint32_t)depth });
    }
};

namespace {

using RayoMd::File::FileIdentity;

// ---- The process's notes, resolved folders and name indexes (vault.h).

constexpr size_t kMaxKeptNoteBytes = size_t(16) << 20;   // a larger note is read every time
constexpr size_t kMaxKeptNotes = 4096;
constexpr size_t kMaxKeptRoots = 256;
constexpr size_t kMaxKeptIndexes = 4;

struct KeptNote {
    std::string key;    // the path Check takes
    std::string path;   // Windows: the path `key` resolves to
    std::shared_ptr<const std::string> text;   // null for a free slot
    FileIdentity identity;
    uint64_t used = 0;
};

struct KeptRoot {
    std::string resolved;
    uint64_t volume = 0;
    uint64_t index = 0;
    bool directory = false;
};

// The maps are the unordered_map<std::string, int> the parser's footnotes and the image registry
// use (no code of their own), each to a slot of a vector plus one (0: a key just added).
struct Kept {
    std::mutex mutex;
    std::unordered_map<std::string, int> notes;   // by KeptNote::key
    std::vector<KeptNote> noteSlots;
    std::unordered_map<std::string, int> roots;   // by the working folder (relative paths) and path
    std::vector<KeptRoot> rootSlots;
    std::shared_ptr<const NameIndex> indexes[kMaxKeptIndexes];   // the most recently used first
    size_t bytes = 0;
    uint64_t clock = 0;
};

// Never destroyed: an exit frees nothing, and a batch worker still running cannot meet a dead lock.
Kept& TheKept() {
    static Kept* const kept = new Kept;
    return *kept;
}

#ifdef _WIN32
std::atomic<uint64_t> g_settle{ 20000000 };   // 2 s in 100 ns
uint64_t Now() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return (uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime;
}
#else
std::atomic<uint64_t> g_settle{ 2000000000 };   // 2 s in ns
uint64_t Now() {
    struct timespec now {};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}
#endif
std::atomic<uint64_t> g_reads{ 0 };
std::atomic<uint64_t> g_walks{ 0 };

// Whether a file of `identity`, read from `start` on, was last written or changed a settle time
// before: a later write then shows as another time, even on a clock of coarse ticks.
bool Settled(const FileIdentity& identity, uint64_t start) {
    const uint64_t settle = g_settle.load(std::memory_order_relaxed);
    return identity.modified + settle < start && identity.changed + settle < start;
}

// What the file (or with `folder`, the folder) at `path` is now, in one call: 1, -1 for none or
// one of the other kind, 0 when one call cannot tell (Windows: a reparse point).
int StampOf(const std::string& path, FileIdentity& identity, bool folder) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(PathFromUtf8(path).c_str(), GetFileExInfoStandard, &data)) return -1;
    if (((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != folder) return -1;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return 0;
    identity.size = (uint64_t)data.nFileSizeHigh << 32 | data.nFileSizeLow;
    identity.modified = (uint64_t)data.ftLastWriteTime.dwHighDateTime << 32 | data.ftLastWriteTime.dwLowDateTime;
    identity.changed = (uint64_t)data.ftCreationTime.dwHighDateTime << 32 | data.ftCreationTime.dwLowDateTime;
#else
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0 || (folder ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode))) return -1;
    RayoMd::File::IdentityOf(status, identity);
#endif
    return 1;
}

// Whether a file is as it was (`kept`, when it was read or walked), by what StampOf says of it now.
bool Unchanged(const FileIdentity& kept, const FileIdentity& now) {
#ifndef _WIN32
    if (kept.volume != now.volume || kept.index != now.index) return false;
#endif
    return kept.size == now.size && kept.modified == now.modified && kept.changed == now.changed;
}

size_t CostOf(const KeptNote& note) {
    return 3 * note.key.size() + note.path.size() + note.text->size() + 160;
}

// `text`, read from the file of `identity` from `start` on, kept for `key` (Check's path) and
// `path` (Windows: what it resolves to): only whole, settled and not too large.
void Keep(const std::string& key, const std::string& path, const std::shared_ptr<const std::string>& text,
    const FileIdentity& identity, uint64_t start) {
    if (text->size() != identity.size || text->size() > kMaxKeptNoteBytes || !Settled(identity, start)) return;
    Kept& kept = TheKept();
    const std::lock_guard<std::mutex> lock(kept.mutex);
    int& slot = kept.notes[key];
    if (slot == 0) {
        // A new slot while none is free (every slot holds a note of the map, the new key aside),
        // else the first free one.
        if (kept.notes.size() <= kept.noteSlots.size()) {
            while (kept.noteSlots[slot].text != nullptr) slot++;
        } else {
            slot = (int)kept.noteSlots.size();
        }
        if ((size_t)slot == kept.noteSlots.size()) kept.noteSlots.emplace_back();
        slot++;
    }
    KeptNote& note = kept.noteSlots[slot - 1];
    if (note.text != nullptr) kept.bytes -= CostOf(note);
    note.key = key;
    note.path = path;
    note.text = text;
    note.identity = identity;
    note.used = ++kept.clock;
    kept.bytes += CostOf(note);
    while (kept.bytes > kMaxNoteCacheBytes || kept.notes.size() > kMaxKeptNotes) {
        KeptNote* oldest = nullptr;
        for (KeptNote& candidate : kept.noteSlots) {
            if (candidate.text != nullptr && (oldest == nullptr || candidate.used < oldest->used)) oldest = &candidate;
        }
        kept.bytes -= CostOf(*oldest);
        kept.notes.erase(kept.notes.find(oldest->key));
        oldest->text = nullptr;
    }
}

// `index` first among the kept name indexes (moved up, or added, the last one given up).
void KeepIndex(const std::shared_ptr<const NameIndex>& index) {
    Kept& kept = TheKept();
    const std::lock_guard<std::mutex> lock(kept.mutex);
    size_t at = 0;
    while (at + 1 < kMaxKeptIndexes && kept.indexes[at] != index) at++;
    for (; at > 0; at--) kept.indexes[at] = std::move(kept.indexes[at - 1]);
    kept.indexes[0] = index;
}

// The name index kept for `key` while every folder it read is as it was then (one stat each),
// then made the most recently used; null without one.
std::shared_ptr<const NameIndex> KeptIndex(const std::string& key, const std::string& root) {
    Kept& kept = TheKept();
    std::shared_ptr<const NameIndex> index;
    {
        const std::lock_guard<std::mutex> lock(kept.mutex);
        for (const auto& candidate : kept.indexes) {
            if (candidate != nullptr && candidate->key == key) index = candidate;
        }
    }
    if (index == nullptr) return nullptr;
    bool unchanged = true;
    std::string folder;
    for (size_t at = 0; unchanged && at < index->folders.size(); at++) {
        const std::string& relative = index->folders[at].first;
        const FileIdentity& then = index->folders[at].second;
        folder.assign(root);
        AppendPath(folder, relative);
        FileIdentity now;
        if (relative.empty() || relative.back() == '/') {
            unchanged = StampOf(folder, now, true) > 0 && Unchanged(then, now);
            continue;
        }
#ifndef _WIN32
        // A link: its target as it was, or still none.
        struct stat status {};
        now.opened = ::stat(folder.c_str(), &status) == 0;
        if (now.opened) RayoMd::File::IdentityOf(status, now);
        unchanged = now.opened == then.opened && (!now.opened || Unchanged(then, now));
#endif
    }
    if (!unchanged) {
        const std::lock_guard<std::mutex> lock(TheKept().mutex);
        for (auto& candidate : TheKept().indexes) {
            if (candidate == index) candidate = nullptr;
        }
        return nullptr;
    }
    KeepIndex(index);
    return index;
}

#ifndef _WIN32
// The regular file open at `fd`, of `identity` (its fstat), whole, as RayoMd::File::ReadAll reads
// one: its size, one read into the string kept; null when it has more than `maximum` bytes.
// Closes it.
std::shared_ptr<const std::string> ReadOpen(int fd, const FileIdentity& identity, size_t maximum) {
    std::shared_ptr<std::string> text;
    if (identity.size <= maximum) {
        text = std::make_shared<std::string>((size_t)identity.size, '\0');
        size_t got = 0;
        while (got < text->size()) {
            const ssize_t count = ::read(fd, &(*text)[got], text->size() - got);
            if (count <= 0) break;
            got += (size_t)count;
        }
        text->resize(got);
        g_reads.fetch_add(1, std::memory_order_relaxed);
    }
    ::close(fd);
    return text;
}

// What fstat says of the file open at `fd`; `opened` false when it is no regular file.
FileIdentity IdentityOf(int fd) {
    struct stat status {};
    FileIdentity identity;
    identity.opened = ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode);
    if (identity.opened) RayoMd::File::IdentityOf(status, identity);
    return identity;
}

// The entries of the folder open at `folder` (its path below the root `relative`, '/'-ended, or
// empty for the root; `depth` folders down), as std::filesystem::recursive_directory_iterator with
// skip_permission_denied visits them (a folder's type from its entry, links to folders neither
// indexed nor entered), without a path object a name, into `index`, with what the folder is (one
// fstat); closes `folder`. `entries` counts them: past `limit` the walk ends. `start`: when it began.
void Walk(NameIndex& index, size_t limit, uint64_t start, int folder, std::string& relative, int depth, size_t& entries) {
    struct stat status {};
    FileIdentity identity;
    const bool known = ::fstat(folder, &status) == 0;
    if (known) RayoMd::File::IdentityOf(status, identity);
    index.keep = index.keep && known && Settled(identity, start);
    index.folders.emplace_back(relative, identity);
    DIR* const directory = ::fdopendir(folder);
    if (directory == nullptr) {
        ::close(folder);
        index.keep = false;
        return;
    }
    const size_t length = relative.size();
    while (const dirent* entry = ::readdir(directory)) {
        const char* const name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        if (++entries > limit) break;
        unsigned char type = entry->d_type;
        if (type == DT_UNKNOWN && ::fstatat(::dirfd(directory), name, &status, AT_SYMLINK_NOFOLLOW) == 0) {
            type = S_ISDIR(status.st_mode) ? DT_DIR : S_ISLNK(status.st_mode) ? DT_LNK : DT_REG;
        }
        bool isFolder = type == DT_DIR;
        if (type == DT_LNK) {
            // A link to a folder is a folder to the index (is_directory follows it), never entered.
            // What a link is depends on its target, which no folder's time tells: kept with what
            // the target is (none for a dangling link), its path without a '/' at the end.
            FileIdentity target;
            target.opened = ::fstatat(::dirfd(directory), name, &status, 0) == 0;
            if (target.opened) RayoMd::File::IdentityOf(status, target);
            isFolder = target.opened && S_ISDIR(status.st_mode);
            index.keep = index.keep && (!target.opened || Settled(target, start));
            index.folders.emplace_back(relative + name, target);
        }
        if (isFolder) {
            // Obsidian's settings, its trash, a repository's history: hidden folders hold no notes.
            if (type != DT_DIR || name[0] == '.' || depth + 1 >= Vault::kMaxIndexDepth) continue;
            const int inner = ::openat(::dirfd(directory), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (inner < 0) {
                index.keep = false;
                continue;
            }
            relative.append(name);
            relative += '/';
            Walk(index, limit, start, inner, relative, depth + 1, entries);
            relative.resize(length);
            if (entries > limit) break;
            continue;
        }
        const size_t nameSize = std::strlen(name);
        if (name[0] == '.' || !IsMarkdownName(std::string_view(name, nameSize))) continue;
        relative.append(name, nameSize);
        index.Add(relative, nameSize, (size_t)depth);
        relative.resize(length);
    }
    ::closedir(directory);
}
#endif

} // namespace

void SetNoteSettleTime(uint64_t ticks) {
    g_settle.store(ticks, std::memory_order_relaxed);
}

VaultCounters CountedVaultWork() {
    return { g_reads.load(std::memory_order_relaxed), g_walks.load(std::memory_order_relaxed) };
}

void ForgetNotes() {
    Kept& kept = TheKept();
    const std::lock_guard<std::mutex> lock(kept.mutex);
    kept.notes.clear();
    kept.noteSlots.clear();
    kept.roots.clear();
    kept.rootSlots.clear();
    for (auto& index : kept.indexes) index = nullptr;
    kept.bytes = 0;
}

std::string CachedResolvedPath(std::string_view path, bool* directory) {
#ifdef _WIN32
    std::string resolved = ResolvedPath(path);
    std::error_code ec;
    if (directory != nullptr) *directory = !resolved.empty() && fs::is_directory(PathFromUtf8(resolved), ec);
    return resolved;
#else
    // A relative path is the working folder's.
    std::string key;
    if (path.empty() || path[0] != '/') {
        char folder[4096];
        if (::getcwd(folder, sizeof(folder)) == nullptr) return ResolvedPath(path);
        key.assign(folder);
        key += '\0';
    }
    key.append(path);
    const std::string file(path);
    Kept& kept = TheKept();
    KeptRoot known;
    bool found = false;
    {
        const std::lock_guard<std::mutex> lock(kept.mutex);
        const auto at = kept.roots.find(key);
        if (at != kept.roots.end()) {
            known = kept.rootSlots[at->second - 1];
            found = true;
        }
    }
    struct stat status {};
    const auto same = [&known, &status](const std::string& name) {
        return ::stat(name.c_str(), &status) == 0 && (uint64_t)status.st_dev == known.volume && (uint64_t)status.st_ino == known.index;
    };
    if (found && same(file) && (known.resolved == file || same(known.resolved))) {
        if (directory != nullptr) *directory = known.directory;
        return known.resolved;
    }
    known.resolved = ResolvedPath(path);
    if (known.resolved.empty() || ::stat(known.resolved.c_str(), &status) != 0) {
        if (directory != nullptr) *directory = false;
        return known.resolved;
    }
    known.volume = (uint64_t)status.st_dev;
    known.index = (uint64_t)status.st_ino;
    known.directory = S_ISDIR(status.st_mode);
    if (directory != nullptr) *directory = known.directory;
    const std::lock_guard<std::mutex> lock(kept.mutex);
    if (kept.roots.size() >= kMaxKeptRoots) {
        kept.roots.clear();
        kept.rootSlots.clear();
    }
    int& slot = kept.roots[key];
    if (slot == 0) {
        kept.rootSlots.emplace_back();
        slot = (int)kept.rootSlots.size();
    }
    return (kept.rootSlots[slot - 1] = known).resolved;
#endif
}

std::string ResolvedFilePath(std::string_view path, std::string_view folder) {
#ifndef _WIN32
    // realpath(3) of a file that is no link: its folder's and its name.
    const std::string_view name = FileNameOf(path);
    struct stat status {};
    if (!folder.empty() && !name.empty() && name != "." && name != ".." && ::lstat(std::string(path).c_str(), &status) == 0 &&
        !S_ISLNK(status.st_mode)) {
        std::string resolved(folder);
        AppendPath(resolved, name);
        return resolved;
    }
#else
    (void)folder;
#endif
    return ResolvedPath(path);
}

Vault::Vault(const std::string& vaultRoot, const std::string& notePath, size_t maxIndexEntries)
    : indexLimit(maxIndexEntries) {
    std::string_view folder = vaultRoot;
    if (folder.empty()) folder = ParentOf(notePath);
    bool directory = false;
    root = CachedResolvedPath(folder.empty() ? std::string_view(".") : folder, &directory);
    if (!directory) root.clear();
#ifdef _WIN32
    if (!root.empty()) rootPath = PathFromUtf8(root);
#endif
}

Vault::~Vault() {
#ifndef _WIN32
    if (openFd >= 0) ::close(openFd);
    if (rootFd >= 0) ::close(rootFd);
#endif
}

#ifndef _WIN32
// `relative` (below the root) opened beneath the root: a descriptor, -1 when it names no file
// there (or leaves the root, by "..", a symbolic link or a magic link), -2 when openat2 itself is
// refused (an old kernel, a seccomp filter) or the root cannot be opened, the root's descriptor
// then given up.
int Vault::OpenBeneath(const char* relative) {
#if defined(__linux__) && defined(SYS_openat2)
    if (rootFd == -1) rootFd = ::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (rootFd < 0) {
        rootFd = -2;
        return -2;
    }
    struct {
        uint64_t flags;
        uint64_t mode;
        uint64_t resolve;
    } how{ (uint64_t)(O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK), 0, 0x08 /* RESOLVE_BENEATH */ | 0x02 /* RESOLVE_NO_MAGICLINKS */ };
    const long fd = ::syscall(SYS_openat2, rootFd, relative, &how, sizeof(how));
    if (fd >= 0) return (int)fd;
    if (errno == ENOSYS || errno == EPERM || errno == EINVAL || errno == E2BIG) {
        ::close(rootFd);
        rootFd = -2;
        return -2;
    }
    return -1;
#else
    (void)relative;
    rootFd = -2;
    return -2;
#endif
}
#endif

#ifndef _WIN32
// `path`, `below` bytes into it below the root (0: not in it), opened beneath the root and left
// open for Read: 1, 0 when it is no regular file there, -1 when it must be checked by resolved path.
int Vault::Open(const std::string& path, size_t below) {
    if (below == 0 || rootFd == -2) return -1;
    const uint64_t start = Now();
    const int fd = OpenBeneath(path.c_str() + below);
    if (fd < 0) return fd == -1 ? 0 : -1;
    const FileIdentity opened = IdentityOf(fd);
    if (!opened.opened) {
        ::close(fd);
        return 0;
    }
    if (openFd >= 0) ::close(openFd);
    openFd = fd;
    openIdentity = opened;
    openPath.assign(path);
    openStart = start;
    return 1;
}
#endif

// 1 when the note kept for `key` is as its file is now (one stat), its text then the hit; -1 when
// one is kept and the file is gone or no regular file; 0 when none is kept or it changed.
int Vault::Cached(const std::string& key) {
    Kept& kept = TheKept();
    KeptNote note;
    {
        const std::lock_guard<std::mutex> lock(kept.mutex);
        const auto found = kept.notes.find(key);
        if (found == kept.notes.end()) return 0;
        KeptNote& slot = kept.noteSlots[found->second - 1];
        slot.used = ++kept.clock;
        note.path = slot.path;
        note.text = slot.text;
        note.identity = slot.identity;
    }
    FileIdentity now;
    const int stamp = StampOf(key, now, false);
    if (stamp <= 0) return stamp;
    if (!Unchanged(note.identity, now)) return 0;
    hitPath = note.path.empty() ? key : note.path;
    hitText = std::move(note.text);
    hitIdentity = note.identity;
    return 1;
}

std::shared_ptr<const std::string> Vault::Read(const std::string& path, size_t maximum, FileIdentity& identity) {
    // Not the note Check found kept: maybe another one is (a book reads its notes after finding them).
    if (hitText == nullptr || hitPath != path) {
        hitText.reset();
#ifndef _WIN32
        const bool kept = rootFd != -2 && !(openFd >= 0 && openPath == path) && BelowRoot(path, root) != 0;
#else
        const bool kept = BelowRoot(path, root) != 0;
#endif
        if (kept && Cached(path) < 0) {
            identity = FileIdentity();
            return nullptr;
        }
    }
    if (hitText != nullptr && hitPath == path) {
        identity = hitIdentity;
        identity.opened = true;
        return identity.size <= maximum ? hitText : nullptr;
    }
#ifndef _WIN32
    // The note Check left open, else opened now (ReadLinkedBook's first note).
    const int opened = openFd >= 0 && openPath == path ? 1 : Open(path, BelowRoot(path, root));
    if (opened == 0) {
        identity = FileIdentity();
        return nullptr;
    }
    if (opened > 0) {
        const int fd = openFd;
        openFd = -1;
        identity = openIdentity;
        std::shared_ptr<const std::string> text = ReadOpen(fd, identity, maximum);
        if (text != nullptr) Keep(path, std::string(), text, identity, openStart);
        return text;
    }
#endif
    std::shared_ptr<std::string> text = std::make_shared<std::string>();
    if (!RayoMd::File::ReadAll(PathFromUtf8(path), maximum, *text, identity)) return nullptr;
    g_reads.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    // Kept only as Check accepted it: in the root, through no link.
    if (path == checkedPath) Keep(checkedKey, path, text, identity, checkedStart);
#endif
    return text;
}

// Whether `path` (normalized) is a regular file inside the root: as kept, if its file is as it was;
// through the root's descriptor (Linux) as written, the file left open for Read; else resolved, and
// `path` then the resolved path.
bool Vault::Check(std::string& path) {
#ifndef _WIN32
    if (rootFd != -2) {
        const size_t below = BelowRoot(path, root);
        if (below == 0) return false;
        int found = Cached(path);
        if (found == 0) found = Open(path, below);
        if (found >= 0) return found > 0;
    }
#else
    // In the root by name before anything is opened, and through no link below it (TryOpenDirect's
    // rule for images); the file itself no link either.
    if (BelowRoot(path, root) == 0 || path.find_first_of("*?") != std::string::npos) return false;
    const uint64_t start = Now();
    const int cached = Cached(path);
    if (cached != 0) {
        if (cached > 0) path = hitPath;
        return cached > 0;
    }
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
#ifdef _WIN32
    checkedKey = path;
    checkedPath = resolved;
    checkedStart = start;
#endif
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
    if (index == nullptr) return std::string();
    // The wanted path's last components, as the index keeps them.
    std::string wanted(target);
    if (addExtension) wanted += ".md";
    Normalize(wanted);
    for (char& c : wanted) c = IsSeparator(c) ? '/' : LowerAscii(c);
    if (wanted.empty() || wanted.compare(0, 3, "../") == 0) return std::string();
    const size_t last = wanted.rfind('/');
    const std::string_view file = last == std::string::npos ? std::string_view(wanted) : std::string_view(wanted).substr(last + 1);
    const uint64_t key = KeyOf(file);
    const NameIndex& names = *index;
    auto at = std::lower_bound(names.index.begin(), names.index.end(), file, [&names, key](const NameIndex::Named& named, std::string_view name) {
        return named.key != key ? named.key < key : names.Name(named) < name;
    });
    for (; at != names.index.end() && names.Name(*at) == file; ++at) {
        if (last != std::string::npos) {
            // Its path ends in the wanted components, letter case aside.
            const std::string_view lower = names.Lower(*at);
            if (lower.size() < wanted.size() || lower.substr(lower.size() - wanted.size()) != wanted ||
                (lower.size() > wanted.size() && lower[lower.size() - wanted.size() - 1] != '/')) {
                continue;
            }
        }
        path.assign(root);
        if (!IsSeparator(root.back())) path += kSeparator;
        path.append(names.Relative(*at));
#ifdef _WIN32
        Normalize(path);
#endif
        if (Check(path)) return path;
    }
    return std::string();
}

void Vault::BuildIndex() {
    indexed = true;
    std::string key = root;
    key += '\0';
    key.append(reinterpret_cast<const char*>(&indexLimit), sizeof(indexLimit));
    index = KeptIndex(key, root);
    if (index != nullptr) return;
    const std::shared_ptr<NameIndex> built = std::make_shared<NameIndex>();
    built->key = std::move(key);
    g_walks.fetch_add(1, std::memory_order_relaxed);
    const uint64_t start = Now();
    size_t entries = 0;
#ifndef _WIN32
    const int folder = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder >= 0) {
        std::string relative;
        Walk(*built, indexLimit, start, folder, relative, 0, entries);
    }
#else
    // Each folder entered as it was (one attribute query a folder).
    const auto stamp = [&built, start](const std::string& path, std::string relative) {
        FileIdentity identity;
        built->keep = built->keep && StampOf(path, identity, true) > 0 && Settled(identity, start);
        built->folders.emplace_back(std::move(relative), identity);
    };
    stamp(root, std::string());
    std::error_code ec;
    fs::recursive_directory_iterator it(rootPath, fs::directory_options::skip_permission_denied, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        if (++entries > indexLimit) break;
        const std::string name = PathToUtf8(it->path().filename());
        std::error_code typeError;
        const std::string path = PathToUtf8(it->path());
        std::string relative = path.substr(BelowRoot(path, root));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        if (it->is_directory(typeError)) {
            // Obsidian's settings, its trash, a repository's history: hidden folders hold no notes.
            if ((!name.empty() && name[0] == '.') || it.depth() + 1 >= kMaxIndexDepth) it.disable_recursion_pending();
            else stamp(path, relative + '/');
            continue;
        }
        if (name.empty() || name[0] == '.' || !IsMarkdownName(name)) continue;
        built->Add(relative, name.size(), (size_t)it.depth());
    }
    if (ec) built->keep = false;
#endif
    if (entries > indexLimit) return;
    NameIndex& names = *built;
    const auto before = [&names](const NameIndex::Named& a, const NameIndex::Named& b) {
        if (a.key != b.key) return a.key < b.key;
        const std::string_view x = names.Name(a);
        const std::string_view y = names.Name(b);
        if (x != y) return x < y;
        return a.depth != b.depth ? a.depth < b.depth : names.Relative(a) < names.Relative(b);
    };
    // Shell sort (Gonnet's gaps): a total order (no two paths are equal), so the result is
    // std::sort's, in a tenth of its code; at most 20,000 entries, once per change of the vault.
    std::vector<NameIndex::Named>& sorted = names.index;
    for (size_t gap = sorted.size() / 2; gap > 0; gap = gap == 2 ? 1 : gap * 5 / 11) {
        for (size_t at = gap; at < sorted.size(); at++) {
            const NameIndex::Named entry = sorted[at];
            size_t to = at;
            for (; to >= gap && before(entry, sorted[to - gap]); to -= gap) sorted[to] = sorted[to - gap];
            sorted[to] = entry;
        }
    }
    if (built->keep) KeepIndex(built);
    index = built;
}

} // namespace TinyPdf::Internal
