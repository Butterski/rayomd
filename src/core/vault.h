// SPDX-License-Identifier: Apache-2.0

#pragma once

// The notes a document names (note embeds, embeds.h; linked notes, book.h): one resolver for both,
// confined to a root folder, the vault. Built at -Os: it reads files, once per note.

#include "../common/file_io.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

// The path that `path` names with its links, junctions, "." and ".." resolved; empty when it
// names nothing. On Windows the file system's own answer (GetFinalPathNameByHandleW): the
// standard library there resolves no link.
std::filesystem::path ResolvedPath(const std::filesystem::path& path);

// Whether the resolved path `file` lies in the resolved directory `root`, component by component
// (so "/a/bc" is not in "/a/b"); on Windows with ASCII letters folded.
bool IsWithinRoot(const std::filesystem::path& file, const std::filesystem::path& root);

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
    const std::filesystem::path& Root() const { return root; }

    // The note that `target` (a note's name or path in UTF-8, without "#fragment"; ".md" added
    // when it has no extension) names, written in a note in the resolved folder `from`: the path
    // relative to `from`, then relative to the root, then, with `byName`, the file of that name
    // (its last components, ASCII case folded) anywhere under the root, the fewest folders deep
    // first, then by path byte order. Empty when none is a note of the vault; an absolute path
    // never is.
    std::filesystem::path Find(const std::filesystem::path& from, std::string_view target, bool byName);

    // The note at `path` (from Find, or the first note of a book) whole, at most `maximum` bytes,
    // and which file it is. A note inside the root is read as Find opened it (on Linux through
    // the root's descriptor, RESOLVE_BENEATH); another path as RayoMd::File::ReadAll reads it.
    bool Read(const std::filesystem::path& path, size_t maximum, std::string& text,
        RayoMd::File::FileIdentity& identity);

private:
    struct Named {
        std::string name;       // the file's name, ASCII lower case
        std::string relative;   // its path below the root, '/'-separated, as written
        size_t depth;           // its folders below the root
    };

    std::filesystem::path Check(const std::filesystem::path& candidate);
    void BuildIndex();

    std::filesystem::path root;
    std::vector<Named> index;   // by name, then depth, then relative path
    size_t indexLimit;
    bool indexed = false;
    bool indexValid = false;
#ifndef _WIN32
    // Linux 5.6+: the root open (O_PATH) once, every note opened beneath it with openat2
    // (RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS): no escape by "..", a symbolic link or a magic link,
    // one call a candidate instead of a realpath walk and a stat. -1 where the kernel or a seccomp
    // filter refuses openat2: the resolved-path checks then. The file Find opened last stays open
    // for the Read that follows.
    int OpenBeneath(const std::filesystem::path& relative);
    int rootFd = -1;
    int openFd = -1;
    std::filesystem::path openPath;
#endif
};

} // namespace TinyPdf::Internal
