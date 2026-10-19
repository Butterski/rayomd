// SPDX-License-Identifier: Apache-2.0

#pragma once

// The notes a document names (note embeds, embeds.h; linked notes, book.h): one resolver for both,
// confined to a root folder, the vault. Paths are UTF-8 strings with the platform's separators, as
// RayoMd::File::PathToUtf8 writes them; a std::filesystem::path is made only where a file is opened
// on Windows. Built at -Os: it reads files, once per note.

#include "../common/file_io.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

// The path (UTF-8) that `path` names with its links, junctions, "." and ".." resolved; empty when
// it names nothing. On Windows the file system's own answer (GetFinalPathNameByHandleW, without
// its "\\?\" prefix): the standard library there resolves no link.
std::string ResolvedPath(std::string_view path);

#ifdef _WIN32
// ResolvedPath for a path that may hold a name with no UTF-8 form (an unpaired surrogate, which
// NTFS allows): the resolved path as Windows gives it, empty when it names nothing.
std::filesystem::path ResolvedWidePath(const std::filesystem::path& path);
#endif

// The folder of the file at `path`: up to its last separator, or its root ("/", "C:\"); empty for
// a bare name. As std::filesystem::path::parent_path() gives it for a path that ends in a name.
std::string_view ParentOf(std::string_view path);

// `base` /= `path`, as std::filesystem::path::operator/= joins them: `path` alone when it is
// absolute (or names another drive), after `base`'s root when it begins at a root directory, else
// after a separator unless `base` is empty or ends in one.
void AppendPath(std::string& base, std::string_view path);

// `path` as std::filesystem::path::lexically_normal() writes it, in place: the platform's
// separators, one between components, no "." component, each ".." with the component before it
// taken out (one at the root dropped), a trailing separator kept, "." for an empty result. The
// text only moves forward, so one pass does.
void Normalize(std::string& path);

// path.lexically_relative(base) of std::filesystem, as a string with the platform's separators:
// empty when the two have different roots, or `base` leads above where `path` begins.
std::string RelativePath(std::string_view path, std::string_view base);

// path.filename() and path.stem() of std::filesystem: the last component (empty when `path` ends
// in a separator or is a root), and that without its extension (a name that only begins with a
// dot, "." and ".." have none).
std::string_view FileNameOf(std::string_view path);
std::string_view StemOf(std::string_view path);

// Whether the resolved path `file` lies in the resolved folder `root`: `root` and then at least one
// component more (so "/a/bc" is not in "/a/b"); on Windows with ASCII letters folded.
bool IsWithinRoot(std::string_view file, std::string_view root);

#ifdef _WIN32
// Whether a directory between `root` and the file `path` names, which lies in it by name, is a
// symbolic link, junction or mount point (name surrogates): they may lead off the root, to a
// share even, and are checked here, never followed. Other reparse points, such as cloud-file
// placeholders, are plain folders to this test. A missing directory counts as one.
bool DirectPathHasLink(const std::filesystem::path& root, const std::filesystem::path& path);
#endif

// The process's notes: what a vault read, kept between builds (the GUI's preview on every edit,
// --serve, batches) and given out again while the file is as it was then. A note is checked with
// one stat (one GetFileAttributesExW on Windows) a build: its size, last write and last status
// change to the nanosecond (POSIX; with its device and inode) or 100 ns (Windows); one written
// within kSettleTime of its read is never kept, as a write in the same tick of the file system's
// clock would not show. At most kMaxNoteCacheBytes, the least recently used given up first; the
// name indexes of the last few roots the same way, checked by a stat of each folder they read.
constexpr size_t kMaxNoteCacheBytes = size_t(64) << 20;
// For the tests: the settle time (POSIX nanoseconds or Windows 100 ns; 2 s by default), and what
// the vaults did: notes read from their files and name indexes walked.
void SetNoteSettleTime(uint64_t ticks);
struct VaultCounters {
    uint64_t reads = 0;
    uint64_t walks = 0;
};
VaultCounters CountedVaultWork();
void ForgetNotes();

// ResolvedPath of a folder or file that builds resolve again and again (a vault's root, the
// document's folder), from the process's cache while `path` and the answer still name the same
// object (device and inode: one stat each, one when they are equal); `directory` tells whether it
// is a folder. On Windows ResolvedPath.
std::string CachedResolvedPath(std::string_view path, bool* directory = nullptr);
// ResolvedPath of the file at `path`, whose folder (ParentOf, "." for none) resolves to `folder`:
// on POSIX that and the file's name when it is no symbolic link (one lstat), else ResolvedPath.
std::string ResolvedFilePath(std::string_view path, std::string_view folder);

struct NameIndex;

// The vault: a root folder, and the notes in it that links and embeds name. A note is a regular
// file inside the root once links and ".." are resolved (on Windows reached through no symbolic
// link, junction or mount point below the root, DirectPathHasLink); anything else is no note.
class Vault {
public:
    // The name index's limits: past this many entries (files and folders) every lookup by name
    // fails; folders deeper than this many levels below the root are not looked into.
    static constexpr size_t kMaxIndexEntries = 20000;
    static constexpr int kMaxIndexDepth = 32;

    // The root: `vaultRoot` (UTF-8) when it is set, else the folder of the note at `notePath`.
    // `maxIndexEntries`: kMaxIndexEntries, lower in tests (20,000 files took a minute to make on
    // Windows).
    Vault(const std::string& vaultRoot, const std::string& notePath, size_t maxIndexEntries = kMaxIndexEntries);
    ~Vault();
    Vault(const Vault&) = delete;
    Vault& operator=(const Vault&) = delete;

    bool Valid() const { return !root.empty(); }
    // The root, resolved.
    const std::string& Root() const { return root; }

    // The note that `target` (a note's name or path in UTF-8, without "#fragment"; ".md" added
    // when it has no extension) names, written in a note in the resolved folder `from`: the path
    // relative to `from`, then relative to the root, then, with `byName`, the file of that name
    // (its last components, ASCII case folded) anywhere under the root, the fewest folders deep
    // first, then by path byte order. Empty when none is a note of the vault; an absolute path
    // never is.
    std::string Find(std::string_view from, std::string_view target, bool byName);

    // The note at `path` (from Find, or ResolvedPath of the first note of a book) whole, null when
    // it cannot be read or has more than `maximum` bytes, and which file it is. A note inside the
    // root comes from the process's cache while its file is as it was, else is read as Find opened
    // it (on Linux through the root's descriptor, RESOLVE_BENEATH) and kept; another path is read
    // as RayoMd::File::ReadAll reads it.
    std::shared_ptr<const std::string> Read(const std::string& path, size_t maximum, RayoMd::File::FileIdentity& identity);

private:
    bool Check(std::string& path);
    int Cached(const std::string& key);
    void BuildIndex();

    std::string root;
    std::shared_ptr<const NameIndex> index;   // null until a lookup by name needs it, or past its limit
    size_t indexLimit;
    bool indexed = false;
    // The note Cached found as it was: Check's path (on Windows the path it resolves to), its text
    // and identity, for the Read that follows.
    std::string hitPath;
    std::shared_ptr<const std::string> hitText;
    RayoMd::File::FileIdentity hitIdentity;
#ifdef _WIN32
    std::filesystem::path rootPath;   // `root`, for the calls that take one
    std::string checkedKey;           // the path Check accepted last, before it was resolved
    std::string checkedPath;          // and resolved
    uint64_t checkedStart = 0;        // when Check began
#else
    // Linux 5.6+: the root open (O_PATH) on the first note not in the cache, every note opened
    // beneath it with openat2 (RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS): no escape by "..", a
    // symbolic link or a magic link, one call a candidate instead of a realpath walk and a stat.
    // -2 where it cannot be opened or the kernel or a seccomp filter refuses openat2: the
    // resolved-path checks then. The file Find opened last stays open for the Read that follows,
    // with what its fstat said and when Check began.
    int OpenBeneath(const char* relative);
    int rootFd = -1;
    int openFd = -1;
    RayoMd::File::FileIdentity openIdentity;
    std::string openPath;
    uint64_t openStart = 0;
#endif
};

} // namespace TinyPdf::Internal
