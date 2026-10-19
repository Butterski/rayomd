// SPDX-License-Identifier: Apache-2.0

// Needs <filesystem>, <iostream>, <set>, <string>, <vector> and src/common/path.h; on POSIX
// <sys/stat.h> and <unistd.h>. The file-system layer that stands in for std::filesystem in the
// engine and both command lines (src/common/path.h): its lexical functions against
// std::filesystem::path on a table of paths (UTF-8 strings and, on Windows, UTF-16), and its
// operations against std::filesystem's in a folder of files, folders and links: what a book's
// links and backlinks, a batch's outputs, the vault's index and the image policy read must not
// change. Included inside the anonymous namespace of tests/core_tests.cpp.

namespace path_tests {

namespace fs = std::filesystem;
namespace file = RayoMd::File;

fs::path FsPath(const std::string& utf8) {
    return fs::path(file::PathFromUtf8(utf8));
}

std::string Utf8(const fs::path& path) {
    return file::PathToUtf8(path.native());
}

bool CheckLexical() {
    bool ok = true;
    const auto expect = [&ok](const char* what, const std::string& input, const std::string& got, const std::string& want) {
        if (got == want) return;
        std::cerr << "path layer: " << what << "(" << input << ") = \"" << got << "\", std::filesystem: \"" << want << "\"" << std::endl;
        ok = false;
    };
    std::vector<std::string> paths = { "", ".", "..", "a", "a/", "a/.", "a/..", "a/../..", "./a", "../a", "a/./b", "a//b", "a/b/..",
        "a/b/../", "a/b/../..", "a/b/../../..", "/", "/..", "/../a", "/a/./b/../c", "a/../../b/../c", "../..", ".././..", "../a/../..",
        "a/b/c/../../d/", "x/y/.", "/a/b//", "///a/b", "a/.../b", "..a/b..", ".hidden/./x", "a/./", "./", "../", "a/../", "/a/..",
        "/a/../..", "x/./../y", "./.././a/", "a.md", ".md", "a.b.c", "a.", "/x/y.md", "x/", "dir.d/file", "c d.md", "x/.hidden", "..md",
        "/a//b.md", "a/b.tar.gz" };
#ifdef _WIN32
    for (const char* windows : { "C:\\a\\..\\b", "C:a\\..\\..", "C:\\..", "C:/x/./y", "\\a\\b", "C:", "C:\\", "c:a/b/", "a\\b/c", "C:\\x.y\\z",
             "C:x.md", "C:\\a\\b.md" }) {
        paths.push_back(windows);
    }
    // A share's server is a root name here, as for Windows; libstdc++ (MinGW) reads the two leading
    // separators as a root directory, and its lexically_normal makes them one, a path on the
    // current drive (base's books could not read a SUMMARY.md's chapters on a share).
    const std::pair<const char*, const char*> shares[] = { { "\\\\srv\\share\\x\\..", "\\\\srv\\share\\" }, { "\\\\srv", "\\\\srv" },
        { "//srv/s/./a", "\\\\srv\\s\\a" }, { "\\\\srv\\s\\..\\..", "\\\\srv\\" } };
    for (const auto& [path, want] : shares) {
        std::string got = path;
        file::Normalize(got);
        expect("Normalize", path, got, want);
        std::wstring wide = file::PathFromUtf8(path);
        file::Normalize(wide);
        expect("Normalize (UTF-16)", path, file::PathToUtf8(wide), want);
    }
    expect("ParentOf", "\\\\srv\\s\\x", std::string(file::ParentOf("\\\\srv\\s\\x")), "\\\\srv\\s");
    expect("IsAbsolute", "\\\\srv\\s", file::IsAbsolute("\\\\srv\\s") ? "yes" : "no", "yes");
#endif
    for (const std::string& path : paths) {
        const fs::path standard = FsPath(path);
        std::string got = path;
        file::Normalize(got);
        expect("Normalize", path, got, Utf8(standard.lexically_normal()));
        expect("StemOf", path, std::string(file::StemOf(path)), Utf8(standard.stem()));
        expect("ExtensionOf", path, std::string(file::ExtensionOf(path)), Utf8(standard.extension()));
        expect("FileNameOf", path, std::string(file::FileNameOf(path)), Utf8(standard.filename()));
        expect("ParentOf", path, std::string(file::ParentOf(path)), Utf8(standard.parent_path()));
        expect("RelativePart", path, std::string(file::RelativePart(path)), Utf8(standard.relative_path()));
        expect("IsAbsolute", path, file::IsAbsolute(path) ? "yes" : "no", standard.is_absolute() ? "yes" : "no");
        for (const char* extension : { ".pdf", "pdf", "", ".x.tmp" }) {
            std::string replaced = path;
            file::ReplaceExtension(replaced, extension);
            fs::path want = standard;
            want.replace_extension(extension);
            expect("ReplaceExtension", path + ", " + extension, replaced, Utf8(want));
        }
#ifdef _WIN32
        // The same on UTF-16, the Path every call opens.
        file::Path wide = file::PathFromUtf8(path);
        file::Normalize(wide);
        expect("Normalize (UTF-16)", path, file::PathToUtf8(wide), Utf8(standard.lexically_normal()));
        const file::Path native = standard.native();
        expect("StemOf (UTF-16)", path, file::PathToUtf8(file::Path(file::StemOf(file::PathView(native)))), Utf8(standard.stem()));
        expect("ParentOf (UTF-16)", path, file::PathToUtf8(file::Path(file::ParentOf(file::PathView(native)))), Utf8(standard.parent_path()));
        expect("FileNameOf (UTF-16)", path, file::PathToUtf8(file::Path(file::FileNameOf(file::PathView(native)))), Utf8(standard.filename()));
#endif
    }
    std::vector<std::pair<std::string, std::string>> relative = { { "/v/a.md", "/v" }, { "/v/a.md", "/v/sub" }, { "/v/sub/b.md", "/v" },
        { "/v/x/b.md", "/v/y/z" }, { "a.md", "" }, { "b/a.md", "" }, { "a.md", "b" }, { "a.md", ".." }, { "../a.md", ".." },
        { "../a.md", "" }, { "/a.md", "/" }, { "/v/a.md", "w" }, { "a.md", "/v" }, { "a/b", "a/b" }, { "a/b", "a/b/c" }, { "a", "a/b/.." },
        { "a/b/c", "a/./b" }, { "a/b", "a/../a" }, { "x/y.md", "x/y.md/z" }, { "a/", "a" }, { "a/b/", "a" }, { "", "" }, { "a", "" },
        { "", "a" }, { "../../a", "../b" }, { "/v/c d.md", "/v/sub/deep" }, { "in/sub/x.md", "in/" }, { "in//sub/x.md", "in" } };
#ifdef _WIN32
    relative.insert(relative.end(), { { "C:\\v\\a.md", "C:\\v\\s" }, { "C:\\v\\a.md", "D:\\v" }, { "\\a\\b", "c" }, { "C:\\a", "C:b" },
        { "\\a\\b", "" }, { "c:\\v\\a.md", "C:\\v" } });
#endif
    for (const auto& [path, base] : relative) {
        expect("RelativePath", path + ", " + base, file::RelativePath(path, base), Utf8(FsPath(path).lexically_relative(FsPath(base))));
#ifdef _WIN32
        expect("RelativePath (UTF-16)", path + ", " + base,
            file::PathToUtf8(file::RelativePath(file::PathView(file::PathFromUtf8(path)), file::PathView(file::PathFromUtf8(base)))),
            Utf8(FsPath(path).lexically_relative(FsPath(base))));
#endif
    }
    std::vector<std::pair<std::string, std::string>> joined = { { "a", "b" }, { "a/", "b" }, { "", "b" }, { "/", "b" }, { "a", "/b" },
        { "/x", "../y" }, { "a", "" }, { "a/", "" }, { ".", "b" }, { "a", "b/" }, { "a//", "b" } };
#ifdef _WIN32
    joined.insert(joined.end(), { { "C:\\a", "\\b" }, { "C:\\a", "D:b" }, { "C:\\a", "C:b" }, { "C:", "b" }, { "C:\\a", "D:\\b" },
        { "a", "C:\\x" } });
#endif
    for (const auto& [base, path] : joined) {
        std::string got = base;
        file::AppendPath(got, path);
        expect("AppendPath", base + ", " + path, got, Utf8(FsPath(base) / FsPath(path)));
#ifdef _WIN32
        file::Path wide = file::PathFromUtf8(base);
        file::AppendPath(wide, file::PathView(file::PathFromUtf8(path)));
        expect("AppendPath (UTF-16)", base + ", " + path, file::PathToUtf8(wide), Utf8(FsPath(base) / FsPath(path)));
#endif
    }
    return ok;
}

// The entries Walk visits, each "path depth" with the root's text cut off, entering every folder
// but those named in `skip`, as std::filesystem::recursive_directory_iterator with
// skip_permission_denied visits them; and ForEachEntry's names against directory_iterator's.
std::vector<std::string> Walked(const fs::path& root, const std::set<std::string>& skip) {
    std::vector<std::string> seen;
    const size_t prefix = root.native().size();
    const bool walked = file::Walk(root.native(), [&](const file::Entry& entry) {
        seen.push_back(Utf8(fs::path(entry.path->substr(prefix))) + " " + std::to_string(entry.depth));
        return skip.count(file::PathToUtf8(file::Path(entry.name))) != 0 ? file::WalkStep::Skip : file::WalkStep::Enter;
    });
    if (!walked) seen.push_back("failed");
    return seen;
}

std::vector<std::string> StandardWalk(const fs::path& root, const std::set<std::string>& skip) {
    std::vector<std::string> seen;
    const size_t prefix = root.native().size();
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        seen.push_back(Utf8(fs::path(it->path().native().substr(prefix))) + " " + std::to_string(it.depth()));
        if (skip.count(Utf8(it->path().filename())) != 0) it.disable_recursion_pending();
    }
    if (ec) seen.push_back("failed");
    return seen;
}

bool CheckOperations() {
    const fs::path root = fs::temp_directory_path() / "rayomd-test-path-layer";
    fs::remove_all(root);
    bool ok = true;
    const auto check = [&ok](const char* what, bool passed) {
        if (passed) return;
        std::cerr << "path layer: " << what << std::endl;
        ok = false;
    };
    // CreateDirectories as create_directories: made, already there, a file in the way, empty.
    bool created = false;
    int error = 0;
    check("CreateDirectories makes a tree", file::CreateDirectories((root / "a" / "b" / "c").native(), &error, &created) && created &&
        fs::is_directory(root / "a" / "b" / "c"));
    check("CreateDirectories of a folder there", file::CreateDirectories((root / "a" / "b").native(), &error, &created) && !created);
    check("CreateDirectories with a trailing separator", file::CreateDirectories((root / "t" / "u" / "").native(), &error, &created) &&
        created && fs::is_directory(root / "t" / "u"));
    std::ofstream(root / "a" / "file.md", std::ios::binary) << "text";
    check("CreateDirectories through a file", !file::CreateDirectories((root / "a" / "file.md" / "x").native(), &error) &&
        file::ErrorText(error) == std::error_code(ENOTDIR, std::generic_category()).message());
    check("CreateDirectories of a file", !file::CreateDirectories((root / "a" / "file.md").native(), &error));
    check("CreateDirectories of nothing", !file::CreateDirectories(file::Path(), &error));
    // StatusOf, IsDirectory, IsRegularFile.
    file::FileStatus status;
    check("StatusOf a file", file::StatusOf((root / "a" / "file.md").native(), status) && status.type == file::FileType::Regular &&
        status.size == 4 && file::IsRegularFile((root / "a" / "file.md").native()) && !file::IsDirectory((root / "a" / "file.md").native()));
    check("StatusOf a folder", file::StatusOf((root / "a").native(), status) && status.type == file::FileType::Directory &&
        file::IsDirectory((root / "a").native()));
    check("StatusOf nothing", !file::StatusOf((root / "none").native(), status, &error) && status.type == file::FileType::None &&
        file::ErrorText(error) == std::error_code(ENOENT, std::generic_category()).message());
    // SetWriteTimeNow, Rename over a file, Remove.
    fs::last_write_time(root / "a" / "file.md", fs::file_time_type::clock::now() - std::chrono::hours(48));
    file::StatusOf((root / "a" / "file.md").native(), status);
    const uint64_t before = status.modified;
    check("SetWriteTimeNow", file::SetWriteTimeNow((root / "a" / "file.md").native()) &&
        file::StatusOf((root / "a" / "file.md").native(), status) && status.modified > before &&
        fs::last_write_time(root / "a" / "file.md") > fs::file_time_type::clock::now() - std::chrono::hours(1));
    std::ofstream(root / "a" / "other.md", std::ios::binary) << "other text";
    check("Rename over a file", file::Rename((root / "a" / "other.md").native(), (root / "a" / "file.md").native()) &&
        !fs::exists(root / "a" / "other.md") && fs::file_size(root / "a" / "file.md") == 10);
    check("Remove", file::Remove((root / "a" / "file.md").native()) && !fs::exists(root / "a" / "file.md") &&
        !file::Remove((root / "a" / "file.md").native()) && file::Remove((root / "t" / "u").native()) && !fs::exists(root / "t" / "u"));
    // Absolute, Resolved, TempDirectory.
    file::Path absolute;
    std::error_code ec;
    check("Absolute of a relative path", file::Absolute(file::PathFromUtf8("x/y.md"), absolute) && fs::path(absolute) == fs::absolute("x/y.md", ec));
    check("Absolute of an absolute path", file::Absolute(root.native(), absolute) && absolute == root.native());
    check("Absolute of nothing", !file::Absolute(file::Path(), absolute));
    const file::Path resolved = file::Resolved((root / "a" / "b" / ".." / "b").native());
#ifdef _WIN32
    // GetFinalPathNameByHandleW: the names as the file system writes them, links resolved.
    check("Resolved", resolved.size() > 4 && resolved.compare(resolved.size() - 4, 4, L"\\a\\b") == 0 && fs::is_directory(resolved, ec));
#else
    check("Resolved", resolved == fs::canonical(root / "a" / "b", ec).native());
#endif
    check("Resolved of nothing", file::Resolved((root / "none").native()).empty());
    check("TempDirectory", file::TempDirectory() == fs::temp_directory_path(ec).native());
    // Folders: entries of each kind, a link to a folder (not entered), a hidden folder and one
    // skipped.
    for (const char* name : { "w/n1.md", "w/sub/n2.md", "w/sub/deeper/n3.md", "w/.hidden/n4.md", "w/skip/n5.md", "w/z.txt" }) {
        fs::create_directories((root / name).parent_path());
        std::ofstream(root / name, std::ios::binary) << name;
    }
#ifndef _WIN32
    std::error_code linked;
    fs::create_directory_symlink(root / "w" / "sub", root / "w" / "link", linked);
    fs::create_symlink(root / "w" / "n1.md", root / "w" / "file-link.md", linked);
    fs::create_symlink(root / "w" / "none", root / "w" / "dangling", linked);
    fs::create_directories(root / "w" / "closed" / "inner");
    ::chmod((root / "w" / "closed").c_str(), 0);
#endif
    const std::set<std::string> skip = { "skip" };
    check("Walk visits as recursive_directory_iterator", Walked(root / "w", skip) == StandardWalk(root / "w", skip));
    check("Walk of nothing", Walked(root / "none", skip) == StandardWalk(root / "none", skip));
    std::set<std::string> names;
    std::set<std::string> standard;
    bool typed = true;
    check("ForEachEntry", file::ForEachEntry((root / "w").native(), [&](const file::Entry& entry) {
        const fs::path path = root / "w" / fs::path(file::Path(entry.name));
        names.insert(Utf8(fs::path(file::Path(entry.name))));
        std::error_code entryError;
        typed = typed && (entry.type == file::FileType::Directory) == fs::is_directory(path, entryError) &&
            (entry.type == file::FileType::Regular) == fs::is_regular_file(path, entryError);
#ifndef _WIN32
        typed = typed && entry.link == fs::is_symlink(fs::symlink_status(path, entryError));
#endif
        return true;
    }));
    for (const fs::directory_entry& entry : fs::directory_iterator(root / "w")) standard.insert(Utf8(entry.path().filename()));
    check("ForEachEntry lists directory_iterator's names", names == standard && typed);
    check("ForEachEntry of nothing", !file::ForEachEntry((root / "none").native(), [](const file::Entry&) { return true; }, &error) &&
        file::ErrorText(error) == std::error_code(ENOENT, std::generic_category()).message());
#ifndef _WIN32
    ::chmod((root / "w" / "closed").c_str(), 0700);
#endif
    fs::remove_all(root);
    return ok;
}

bool CheckPathLayer() {
    return CheckLexical() && CheckOperations();
}

} // namespace path_tests
