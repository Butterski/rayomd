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
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

// The path (UTF-8) that `path` names with its links, junctions, "." and ".." resolved; empty when
// it names nothing. On Windows the file system's own answer (GetFinalPathNameByHandleW, without
// its "\\?\" prefix): the standard library there resolves no link.
std::string ResolvedPath(std::string_view path);

// The folder of the file at `path`: up to its last separator, or its root ("/", "C:\"); empty for
// a bare name. As std::filesystem::path::parent_path() gives it for a path that ends in a name.
std::string_view ParentOf(std::string_view path);

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

    // The note at `path` (from Find, or ResolvedPath of the first note of a book) whole, at most
    // `maximum` bytes, and which file it is. A note inside the root is read as Find opened it (on
    // Linux through the root's descriptor, RESOLVE_BENEATH); another path as
    // RayoMd::File::ReadAll reads it.
    bool Read(const std::string& path, size_t maximum, std::string& text, RayoMd::File::FileIdentity& identity);

private:
    // A note of the name index: its path below the root, '/'-separated, at names[at] as written and
    // right after that in ASCII lower case; its name the last `nameSize` bytes of it, whose first
    // eight are `key` (big-endian, zero-padded), which orders most names without reading them.
    struct Named {
        uint64_t key;
        uint32_t at;
        uint32_t size;
        uint32_t nameSize;
        uint32_t depth;   // its folders below the root
    };

    std::string_view Relative(const Named& named) const { return std::string_view(names.data() + named.at, named.size); }
    std::string_view Lower(const Named& named) const { return std::string_view(names.data() + named.at + named.size, named.size); }
    std::string_view Name(const Named& named) const {
        return std::string_view(names.data() + named.at + 2 * named.size - named.nameSize, named.nameSize);
    }
    void AddNamed(std::string_view relative, size_t nameSize, size_t depth);
    bool Check(std::string& path);
    void BuildIndex();

    std::string root;
    std::vector<Named> index;   // by name, then depth, then relative path
    std::string names;          // their paths
    size_t indexLimit;
    bool indexed = false;
    bool indexValid = false;
#ifdef _WIN32
    std::filesystem::path rootPath;   // `root`, for the calls that take one
#else
    // Linux 5.6+: the root open (O_PATH) once, every note opened beneath it with openat2
    // (RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS): no escape by "..", a symbolic link or a magic link,
    // one call a candidate instead of a realpath walk and a stat. -1 where the kernel or a seccomp
    // filter refuses openat2: the resolved-path checks then. The file Find opened last stays open
    // for the Read that follows, with what its fstat said.
    void Walk(int folder, std::string& relative, int depth, size_t& entries);
    int OpenBeneath(const char* relative);
    int rootFd = -1;
    int openFd = -1;
    RayoMd::File::FileIdentity openIdentity;
    std::string openPath;
#endif
};

} // namespace TinyPdf::Internal
