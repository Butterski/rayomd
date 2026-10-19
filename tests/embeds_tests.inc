// SPDX-License-Identifier: Apache-2.0

// Needs <filesystem>, <fstream>, <string>, <vector>; uses math_markdown_tests::Repeat,
// parse_limits_tests::Bounded and CountOccurrences of tests/core_tests.cpp, so it is included after
// parse_limits_tests.inc. Obsidian's note embeds (embeds.h) and the vault they are read from
// (vault.h): resolution, sections and blocks, every limit, footnotes, the source map, contents,
// images and fonts; and the books of linked notes (book.h's ReadLinkedBook) with their backlinks.
// Included inside the anonymous namespace of tests/core_tests.cpp.

namespace embeds_tests {

namespace fs = std::filesystem;

void WriteNote(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

TinyPdf::PdfOptions ObsidianOptions(const fs::path& document) {
    TinyPdf::PdfOptions options;
    options.dialect = TinyPdf::MarkdownDialect::Obsidian;
    options.sourcePath = RayoMd::File::PathToUtf8(document);
    return options;
}

// The notes the embeds of `markdown`, the document at `document`, bring in: each entry's text, the
// document's own (entry 0) empty; none when ResolveEmbeds reads no note.
std::vector<std::string> Embedded(const std::string& markdown, const TinyPdf::PdfOptions& options) {
    TinyPdf::Internal::EmbedSet set;
    std::vector<std::string> texts;
    if (!TinyPdf::Internal::ResolveEmbeds(markdown, options, set)) return texts;
    for (const TinyPdf::Internal::EmbedEntry& entry : set.entries) texts.push_back(entry.text);
    return texts;
}

bool Fail(const char* what) {
    std::cerr << "note embeds: " << what << std::endl;
    return false;
}

// Resolution: relative to the embedding note, then to the vault, then by name (fewest folders
// first, then byte order, letter case aside); ".md" added; nothing outside the vault, no absolute
// path, no link out of it; a section up to the next heading of its level, a block by its id.
bool CheckResolution(const fs::path& root) {
    const fs::path vault = root / "vault";
    WriteNote(vault / "dup.md", "root dup\n");
    WriteNote(vault / "sub" / "dup.md", "sub dup\n");
    WriteNote(vault / "sub" / "r.md", "![[dup]]\n\n![[only-root]]\n\n![[../a]]\n");
    WriteNote(vault / "only-root.md", "only root\n");
    WriteNote(vault / "a.md", "# A\n\nAlpha text.\n");
    WriteNote(vault / "deep" / "x" / "Named.md", "deep named\n");
    WriteNote(vault / "b" / "n.md", "b n\n");
    WriteNote(vault / "a" / "n.md", "a n\n");
    WriteNote(vault / "a" / "z" / "n.md", "a z n\n");
    WriteNote(vault / ".obsidian" / "hidden.md", "hidden\n");
    WriteNote(root / "outside.md", "outside\n");
    const TinyPdf::PdfOptions options = ObsidianOptions(vault / "main.md");
    const std::vector<std::string> order = Embedded("![[dup]]\n\n![[sub/r]]\n\n![[named]]\n\n![[N]]\n\n![[hidden]]\n", options);
    if (order != std::vector<std::string>{ "", "root dup\n\n", "![[dup]]\n\n![[only-root]]\n\n![[../a]]\n\n", "sub dup\n\n",
            "only root\n\n", "# A\n\nAlpha text.\n\n", "deep named\n\n", "a n\n\n" }) {
        return Fail("resolution order");
    }
    std::string absolute = RayoMd::File::PathToUtf8(vault / "a");
    if (!Embedded("![[../outside]]\n\n![[" + absolute + "]]\n\n![[a.png]]\n\n![[a.pdf]]\n", options).empty()) {
        return Fail("a note outside the vault, an absolute path or a media file was embedded");
    }
#ifndef _WIN32
    std::error_code error;
    fs::create_symlink(root / "outside.md", vault / "link.md", error);
    if (!error && !Embedded("![[link]]\n", options).empty()) return Fail("a link out of the vault was followed");
#endif
    // With a vault folder, a note of the document's folder may embed one anywhere in it.
    TinyPdf::PdfOptions inVault = ObsidianOptions(vault / "sub" / "main.md");
    if (!Embedded("![[only-root]]\n", inVault).empty()) return Fail("the vault root defaults to the document's folder");
    inVault.vaultRoot = RayoMd::File::PathToUtf8(vault);
    if (Embedded("![[only-root]]\n", inVault).size() != 2) return Fail("--vault");

    WriteNote(vault / "sections.md", "---\ntitle: t\n---\n# Top\n\n## One\n\nOne text.\n\n```\n## Not\n```\n\n### Inner\n\nIn.\n\n"
        "## Two\n\nTwo text.\n\n# Next\n\nNext text.\n");
    WriteNote(vault / "blocks.md", "Lead.\nPara one ^p1\n\n- item one\n- item two ^li\n\n| t | u |\n|---|---|\n| 1 | 2 |\n\n^tbl\n\n"
        "After.\n");
    const std::vector<std::string> parts = Embedded("![[sections#One]]\n\n![[sections#two]]\n\n![[sections#Top#Two]]\n\n"
        "![[sections#Missing]]\n\n![[blocks#^p1]]\n\n![[blocks#^li]]\n\n![[blocks#^tbl]]\n\n![[blocks#^none]]\n\n![[sections]]\n", options);
    const std::vector<std::string> expected = { "", "## One\n\nOne text.\n\n```\n## Not\n```\n\n### Inner\n\nIn.\n\n",
        "## Two\n\nTwo text.\n\n", "## Two\n\nTwo text.\n\n", "Lead.\nPara one\n", "- item two\n", "| t | u |\n|---|---|\n| 1 | 2 |\n",
        "# Top\n\n## One\n\nOne text.\n\n```\n## Not\n```\n\n### Inner\n\nIn.\n\n## Two\n\nTwo text.\n\n# Next\n\nNext text.\n\n" };
    if (parts != expected) return Fail("sections and blocks");
    return true;
}

// Every limit leaves the embed a link: a cycle, depth 9, the 1,001st embed, 32 MiB of embedded
// text; and a megabyte of openers or of embeds stays linear.
bool CheckLimits(const fs::path& root) {
    using math_markdown_tests::Repeat;
    const fs::path vault = root / "limits";
    WriteNote(vault / "cyc1.md", "one\n\n![[cyc2]]\n");
    WriteNote(vault / "cyc2.md", "two\n\n![[cyc1]]\n\n![[main]]\n");
    const TinyPdf::PdfOptions options = ObsidianOptions(vault / "main.md");
    WriteNote(vault / "main.md", "![[cyc1]]\n");
    if (Embedded("![[cyc1]]\n", options).size() != 3) return Fail("a cycle");
    for (int level = 1; level <= 10; level++) {
        WriteNote(vault / ("d" + std::to_string(level) + ".md"), "level " + std::to_string(level) + "\n\n![[d" + std::to_string(level + 1) + "]]\n");
    }
    if (Embedded("![[d1]]\n", options).size() != 1 + 8) return Fail("embeds nest 8 deep");
    WriteNote(vault / "small.md", "small\n");
    if (Embedded(Repeat("![[small]]\n\n", 1001), options).size() != 1 + 1000) return Fail("at most 1,000 embeds");
    WriteNote(vault / "big1.md", std::string(20u << 20, 'x') + "\n");
    WriteNote(vault / "big2.md", std::string(20u << 20, 'y') + "\n");
    if (Embedded("![[big1]]\n\n![[big2]]\n\n![[small]]\n", options).size() != 3) return Fail("at most 32 MiB of embedded text");
    fs::remove(vault / "big1.md");
    fs::remove(vault / "big2.md");
    TinyPdf::PdfOptions links = options;
    links.embedsAsLinks = true;
    std::string pdf;
    if (!TinyPdf::BuildPdf("![[small]]\n", links, pdf).Ok() || CountOccurrences(pdf, "/URI (small.md)") != 1) return Fail("--embeds=link");
    TinyPdf::PdfOptions stdinOptions;
    stdinOptions.dialect = TinyPdf::MarkdownDialect::Obsidian;
    if (!TinyPdf::BuildPdf("![[small]]\n", stdinOptions, pdf).Ok() || CountOccurrences(pdf, "/URI (small.md)") != 1) {
        return Fail("a document without a path keeps its embeds links");
    }
    if (!TinyPdf::BuildPdf("![[small]]\n", options, pdf).Ok() || CountOccurrences(pdf, "/URI") != 0 || pdf.find("(small) Tj") == std::string::npos) {
        return Fail("an embedded note");
    }
    std::string missing;
    for (size_t index = 0; index < 20000; index++) missing += "![[m" + std::to_string(index) + "]]\n";
    return parse_limits_tests::Bounded("a megabyte of embed openers", 5.0, 256, [&]() {
        std::string out;
        return TinyPdf::BuildPdf(Repeat("![[", 350000), options, out).Ok() &&
            TinyPdf::BuildPdf(Repeat("![[small]]\n", size_t(1) << 14), options, out).Ok() &&
            TinyPdf::BuildPdf(missing, options, out).Ok();
    });
}

// The embedded blocks in the document: footnotes of their own, the embed's line in the source map,
// headings in the table of contents and numbered, images from their note's folder, and the fonts
// chosen for all of the text.
bool CheckBuilds(const fs::path& root) {
    const fs::path vault = root / "builds";
    WriteNote(vault / "a.md", "# A\n\nAlpha body[^1].\n\n[^1]: Alpha note.\n");
    const TinyPdf::PdfOptions plain = ObsidianOptions(vault / "main.md");
    const std::vector<std::string> notes = Embedded("Main[^1].\n\n![[a]]\n\n[^1]: Main note.\n", plain);
    if (notes.size() != 2 || notes[1].find("body[^1~e1]") == std::string::npos || notes[1].find("[^1~e1]: Alpha") == std::string::npos) {
        return Fail("footnote labels of an embedded note");
    }
    std::string pdf;
    if (!TinyPdf::BuildPdf("Main[^1].\n\n![[a]]\n\n[^1]: Main note.\n", plain, pdf).Ok() || pdf.find("( Main note.) Tj") == std::string::npos ||
        pdf.find("( Alpha note.) Tj") == std::string::npos || pdf.find("(2.) Tj") == std::string::npos) {
        return Fail("footnotes of an embedded note");
    }
    std::vector<TinyPdf::SourceBlock> map;
    TinyPdf::PdfOptions mapped = plain;
    mapped.sourceMap = &map;
    if (!TinyPdf::BuildPdf("# T\n\n![[a]]\nEnd.\n", mapped, pdf).Ok() || map.size() != 4 || map[1].firstLine != 2 || map[1].endLine != 3 ||
        map[2].firstLine != 2 || map[3].firstLine != 3) {
        return Fail("source map of an embedded note");
    }
    WriteNote(vault / "part.md", "## Embedded\n\nX.\n");
    TinyPdf::PdfOptions numbered = plain;
    numbered.toc = true;
    numbered.numberHeadings = 3;
    if (!TinyPdf::BuildPdf("# Title\n\n## Intro\n\n![[part]]\n", numbered, pdf).Ok() || pdf.find("/Title (2 Embedded)") == std::string::npos ||
        pdf.find("(2 Embedded) Tj") == std::string::npos) {
        return Fail("contents and numbers of embedded headings");
    }
    std::error_code error;
    fs::create_directories(vault / "sub" / "img");
    const fs::path png = fs::path(__FILE__).parent_path().parent_path() / "docs" / "assets" / "branding" / "rayomd.png";
    fs::copy_file(png, vault / "sub" / "img" / "pic.png", fs::copy_options::overwrite_existing, error);
    WriteNote(vault / "sub" / "pictured.md", "![pic](img/pic.png)\n");
    const TinyPdf::BuildResult pictured = TinyPdf::BuildPdf("![[pictured]]\n\n![pic](img/pic.png)\n", plain, pdf);
    std::string reference;
    const TinyPdf::BuildResult direct = TinyPdf::BuildPdf("![pic](sub/img/pic.png)\n", plain, reference);
    if (error || !pictured.Ok() || pictured.failedImages != 1 || !direct.Ok() || direct.failedImages != 0 ||
        CountOccurrences(pdf, "/Subtype /Image") == 0 || CountOccurrences(pdf, "/Subtype /Image") != CountOccurrences(reference, "/Subtype /Image")) {
        std::cerr << "images: " << pictured.failedImages << " failed, " << CountOccurrences(pdf, "/Subtype /Image") << " against "
                  << CountOccurrences(reference, "/Subtype /Image") << std::endl;
        return Fail("an image of an embedded note resolves from its folder");
    }
    WriteNote(vault / "cjk.md", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\n");
    const TinyPdf::BuildResult cjk = TinyPdf::BuildPdf("Caf\xC3\xA9\n\n![[cjk]]\n", plain, pdf);
    if (!cjk.Ok() || (pdf.find("/Subtype /Type0") == std::string::npos && cjk.missingCharacters != 3)) {
        return Fail("a WinAnsi note embedding a CJK note takes a Unicode font");
    }
    if (TinyPdf::Internal::ObsidianToCommonMark("Text ^abc-1\n^id\nx ^2 y\n[[#^id]]\n$a ^b$") != "Text\n\nx ^2 y\n^id\n$a ^b$") {
        return Fail("block ids");
    }
    return true;
}

// The name index past its entries finds no note by name, while paths still resolve; with a cap of
// 64 entries, as 20,000 files took a minute to make on Windows.
bool CheckIndexCap(const fs::path& root) {
    constexpr size_t kCap = 64;
    const fs::path vault = root / "crowded";
    WriteNote(vault / "deep" / "target.md", "target\n");
    WriteNote(vault / "near.md", "near\n");
    fs::create_directories(vault / "many");
    for (size_t index = 0; index < kCap; index++) {
        std::ofstream(vault / "many" / ("f" + std::to_string(index)), std::ios::binary);
    }
    TinyPdf::Internal::Vault crowded(RayoMd::File::PathToUtf8(vault), std::string(), kCap);
    const std::string base = TinyPdf::Internal::ResolvedPath(RayoMd::File::PathToUtf8(vault));
    if (!crowded.Find(base, "target", true).empty() || crowded.Find(base, "near", true).empty() ||
        crowded.Find(base, "deep/target", false).empty()) {
        return Fail("the name index's cap");
    }
    fs::remove_all(vault / "many");
    TinyPdf::Internal::Vault spacious(RayoMd::File::PathToUtf8(vault), std::string(), kCap);
    return !spacious.Find(base, "TARGET", true).empty() || Fail("a note by name, letter case aside");
}

// The process's notes (vault.h): a second build reads no note and walks no folder; a note
// rewritten, added, removed or swapped for a link out of the vault is seen on the next build; a
// note written within the settle time is read every time; builds on several threads agree.
bool CheckNoteCache(const fs::path& root) {
    using namespace TinyPdf::Internal;
#ifdef _WIN32
    constexpr uint64_t kShort = 500000, kDefault = 20000000;   // 50 ms, 2 s in 100 ns
#else
    constexpr uint64_t kShort = 50000000, kDefault = 2000000000;   // 50 ms, 2 s in ns
#endif
    const auto settle = []() { std::this_thread::sleep_for(std::chrono::milliseconds(120)); };
    const fs::path vault = root / "cache";
    WriteNote(vault / "a.md", "Alpha one.\n");
    WriteNote(vault / "sub" / "deep.md", "Deep one.\n");
    SetNoteSettleTime(kShort);
    ForgetNotes();
    settle();
    const TinyPdf::PdfOptions options = ObsidianOptions(vault / "main.md");
    std::string pdf;
    const auto build = [&options, &pdf](const std::string& markdown, VaultCounters& work) {
        const VaultCounters before = CountedVaultWork();
        const bool ok = TinyPdf::BuildPdf(markdown, options, pdf).Ok();
        const VaultCounters after = CountedVaultWork();
        work = { after.reads - before.reads, after.walks - before.walks };
        return ok;
    };
    const std::string doc = "![[a]]\n\n![[deep]]\n";
    VaultCounters work;
    if (!build(doc, work) || work.reads != 2 || work.walks != 1) {
        std::cerr << "first build: " << work.reads << " reads, " << work.walks << " walks" << std::endl;
        return Fail("a first build");
    }
    const std::string first = pdf;
    if (!build(doc, work) || work.reads != 0 || work.walks != 0 || pdf != first) {
        std::cerr << "second build: " << work.reads << " reads, " << work.walks << " walks" << std::endl;
        return Fail("a second build reads its notes again");
    }
    WriteNote(vault / "a.md", "Alpha two.\n");
    settle();
    if (!build(doc, work) || work.reads != 1 || work.walks != 0 || pdf.find("(Alpha two.) Tj") == std::string::npos) {
        return Fail("a note rewritten is read again");
    }
    WriteNote(vault / "sub" / "new.md", "New one.\n");
    settle();
    if (!build(doc + "\n![[new]]\n", work) || work.walks != 1 || pdf.find("(New one.) Tj") == std::string::npos) {
        return Fail("a note added to a folder is found by name");
    }
    fs::remove(vault / "a.md");
    settle();
    if (!build(doc, work) || pdf.find("Alpha") != std::string::npos) return Fail("a note removed is still embedded");
#ifndef _WIN32
    WriteNote(root / "outside-cache.md", "Outside text.\n");
    fs::remove(vault / "sub" / "deep.md");
    std::error_code error;
    fs::create_symlink(root / "outside-cache.md", vault / "sub" / "deep.md", error);
    settle();
    if (error || !build(doc, work) || pdf.find("Outside") != std::string::npos) return Fail("a note swapped for a link out of the vault");
#endif
    SetNoteSettleTime(kDefault);
    WriteNote(vault / "fresh.md", "Fresh one.\n");
    if (!build("![[fresh]]\n", work) || work.reads != 1 || !build("![[fresh]]\n", work) || work.reads != 1) {
        return Fail("a note written within the settle time is kept");
    }
    SetNoteSettleTime(kShort);
    WriteNote(vault / "b.md", "Bravo one.\n");
    settle();
    std::vector<std::string> outputs(4);
    std::vector<std::thread> threads;
    for (std::string& output : outputs) {
        threads.emplace_back([&output, &options]() {
            for (int round = 0; round < 20; round++) {
                std::string built;
                if (!TinyPdf::BuildPdf("![[b]]\n\n![[new]]\n", options, built).Ok() || (!output.empty() && built != output)) {
                    output = "differs";
                    return;
                }
                output = built;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    SetNoteSettleTime(kDefault);
    ForgetNotes();
    for (const std::string& output : outputs) {
        if (output != outputs[0] || output.find("(Bravo one.) Tj") == std::string::npos) return Fail("builds on several threads");
    }
    return true;
}

bool CheckEmbeds() {
    const fs::path root = fs::temp_directory_path() / "rayomd-test-embeds";
    fs::remove_all(root);
    const bool ok = CheckResolution(root) && CheckLimits(root) && CheckBuilds(root) && CheckIndexCap(root) && CheckNoteCache(root);
    fs::remove_all(root);
    return ok;
}

// The string path helpers (vault.h) against std::filesystem, which book.cpp used before: the
// links between a book's files and the backlinks' targets must not change.
bool CheckPathHelpers() {
    using RayoMd::File::PathFromUtf8;
    using RayoMd::File::PathToUtf8;
    bool ok = true;
    const auto expect = [&ok](const char* what, const std::string& input, const std::string& got, const std::string& want) {
        if (got == want) return;
        std::cerr << "path helpers: " << what << "(" << input << ") = \"" << got << "\", std::filesystem: \"" << want << "\"" << std::endl;
        ok = false;
    };
    std::vector<std::string> normal = { "", ".", "..", "a", "a/", "a/.", "a/..", "a/../..", "./a", "../a", "a/./b", "a//b", "a/b/..",
        "a/b/../", "a/b/../..", "a/b/../../..", "/", "/..", "/../a", "/a/./b/../c", "a/../../b/../c", "../..", ".././..", "../a/../..",
        "a/b/c/../../d/", "x/y/.", "/a/b//", "///a/b", "a/.../b", "..a/b..", ".hidden/./x", "a/./", "./", "../", "a/../", "/a/..",
        "/a/../..", "x/./../y", "./.././a/" };
#ifdef _WIN32
    for (const char* windows : { "C:\\a\\..\\b", "C:a\\..\\..", "C:\\..", "\\\\srv\\share\\x\\..", "C:/x/./y", "\\a\\b", "C:", "C:\\",
             "c:a/b/", "\\\\srv", "\\\\srv\\", "a\\b/c" }) {
        normal.push_back(windows);
    }
#endif
    for (const std::string& path : normal) {
        std::string got = path;
        TinyPdf::Internal::Normalize(got);
        expect("Normalize", path, got, PathToUtf8(PathFromUtf8(path).lexically_normal()));
    }
    std::vector<std::pair<std::string, std::string>> relative = { { "/v/a.md", "/v" }, { "/v/a.md", "/v/sub" }, { "/v/sub/b.md", "/v" },
        { "/v/x/b.md", "/v/y/z" }, { "a.md", "" }, { "b/a.md", "" }, { "a.md", "b" }, { "a.md", ".." }, { "../a.md", ".." },
        { "../a.md", "" }, { "/a.md", "/" }, { "/v/a.md", "w" }, { "a.md", "/v" }, { "a/b", "a/b" }, { "a/b", "a/b/c" }, { "a", "a/b/.." },
        { "a/b/c", "a/./b" }, { "a/b", "a/../a" }, { "x/y.md", "x/y.md/z" }, { "a/", "a" }, { "a/b/", "a" }, { "", "" }, { "a", "" },
        { "", "a" }, { "../../a", "../b" }, { "/v/c d.md", "/v/sub/deep" } };
#ifdef _WIN32
    relative.insert(relative.end(), { { "C:\\v\\a.md", "C:\\v\\s" }, { "C:\\v\\a.md", "D:\\v" }, { "\\a\\b", "c" }, { "C:\\a", "C:b" },
        { "\\a\\b", "" }, { "c:\\v\\a.md", "C:\\v" } });
#endif
    for (const auto& [path, base] : relative) {
        expect("RelativePath", path + ", " + base, TinyPdf::Internal::RelativePath(path, base),
            PathToUtf8(PathFromUtf8(path).lexically_relative(PathFromUtf8(base))));
    }
    for (const char* path : { "a.md", "a", ".md", "a.b.c", "a.", "/x/y.md", "x/", "/", ".", "..", "x/..", "x/.hidden", "dir.d/file",
             "c d.md", "", "x/.", "..md" }) {
        expect("StemOf", path, std::string(TinyPdf::Internal::StemOf(path)), PathToUtf8(PathFromUtf8(path).stem()));
    }
    for (const char* path : { "a/b.md", "/b.md", "b.md", "/a/b/c.md", "a/b/c", "a//b.md", "/a//b//c.md" }) {
        expect("ParentOf", path, std::string(TinyPdf::Internal::ParentOf(path)), PathToUtf8(PathFromUtf8(path).parent_path()));
    }
    std::vector<std::pair<std::string, std::string>> joined = { { "a", "b" }, { "a/", "b" }, { "", "b" }, { "/", "b" }, { "a", "/b" },
        { "/x", "../y" }, { "a", "" }, { "a/", "" }, { ".", "b" }, { "a", "b/" } };
#ifdef _WIN32
    joined.insert(joined.end(), { { "C:\\a", "\\b" }, { "C:\\a", "D:b" }, { "C:\\a", "C:b" }, { "C:", "b" }, { "C:\\a", "D:\\b" },
        { "a", "\\\\srv\\x" }, { "\\\\srv\\s", "\\x" } });
#endif
    for (const auto& [base, path] : joined) {
        std::string got = base;
        TinyPdf::Internal::AppendPath(got, path);
        expect("AppendPath", base + ", " + path, got, PathToUtf8(PathFromUtf8(base) / PathFromUtf8(path)));
    }
    return ok;
}

// The files of a book that links name (BookFileOfLink): relative paths, folders by their README.md
// or index.md, escapes, and in Obsidian's dialect names and path ends in any case; and the
// backlinks' paragraphs (AddBacklinks): each linking file once in the book's order, its title or
// file name escaped, its path relative to the linked file's folder and percent-encoded.
bool CheckBookLinks() {
    std::vector<TinyPdf::BookChapter> chapters(8);
    const char* const files[] = { "/v/README.md", "/v/guide/intro.md", "/v/guide/index.md", "/v/guide/sub/Deep Note.md", "/v/v1.2 notes.md",
        "", "rel/x.md", "/v/guide/./odd/../other.md" };
    std::vector<std::string> paths(chapters.size());
    for (size_t index = 0; index < chapters.size(); index++) {
        chapters[index].path = files[index];
        chapters[index].part = chapters[index].path.empty();
        if (!chapters[index].part) {
            paths[index] = RayoMd::File::PathToUtf8(RayoMd::File::PathFromUtf8(files[index]).lexically_normal());
        }
    }
    constexpr size_t kNone = std::string::npos;
    struct Case {
        size_t from;
        const char* link;
        size_t github;
        size_t obsidian;
    };
    const Case cases[] = { { 1, "../README.md", 0, 0 }, { 1, "..", 0, 0 }, { 1, "../", 0, 0 }, { 0, "guide", 2, 2 }, { 0, "guide/", 2, 2 },
        { 0, "guide/intro.md", 1, 1 }, { 0, "./guide/./intro.md", 1, 1 }, { 0, "guide/sub/Deep%20Note.md", 3, 3 }, { 3, "../../README.md", 0, 0 },
        { 3, "../intro.md", 1, 1 }, { 0, "/v/README.md", kNone, kNone }, { 0, "http://x/y.md", kNone, kNone }, { 0, "mailto:a@b", kNone, kNone },
        { 6, "x.md", 6, 6 }, { 6, "../README.md", kNone, kNone }, { 0, "guide/other.md", 7, 7 }, { 1, "other.md", 7, 7 }, { 0, "Deep Note", kNone, 3 },
        { 0, "deep note", kNone, 3 }, { 0, "sub/deep%20note", kNone, 3 }, { 0, "ub/deep note", kNone, kNone }, { 0, "v1.2 notes", kNone, 4 },
        { 0, "intro.md", kNone, 1 }, { 0, "picture.png", kNone, kNone }, { 0, "INTRO", kNone, 1 }, { 0, "guide/../README", kNone, 0 },
        { 0, "a%2Fb", kNone, kNone }, { 0, "", kNone, kNone }, { 1, "./", 2, 2 }, { 1, ".", 2, 2 }, { 0, "guide/odd/..", 2, 2 },
        { 0, "%67uide/intro.md", 1, 1 }, { 2, "sub/../../guide/sub/..", 2, 2 }, { 1, "%2Fv%2FREADME.md", 0, 0 },
        { 1, "%2Fv%2Fguide", 2, 2 } };
    bool ok = true;
    for (const TinyPdf::MarkdownDialect dialect : { TinyPdf::MarkdownDialect::GitHub, TinyPdf::MarkdownDialect::Obsidian }) {
        const TinyPdf::Internal::DialectScope scope(dialect);
        for (const Case& test : cases) {
            const size_t want = dialect == TinyPdf::MarkdownDialect::GitHub ? test.github : test.obsidian;
            const size_t got = TinyPdf::Internal::BookFileOfLink(chapters, paths, test.from, test.link);
            if (got == want) continue;
            std::cerr << "book links: \"" << test.link << "\" from " << test.from << " found " << (long long)got << ", not " << (long long)want << std::endl;
            ok = false;
        }
    }
    TinyPdf::Book book;
    const char* const linked[][3] = { { "/v/a.md", "", "[b](sub/b.md) and [c](sub/deep/c%20d.md) and [e](other/e.md) and [b again](sub/b.md)" },
        { "/v/sub/b.md", "Bee (2)", "[a](../a.md), [c](deep/c%20d.md)" },
        { "/v/sub/deep/c d.md", "", "[b](../b.md) [e](../../other/e.md) [self](c%20d.md)" }, { "", "Part", "" },
        { "/v/other/e.md", "", "[a](../a.md)" }, { "rel.md", "", "[a](/v/a.md) [x](x/y.md)" }, { "x/y.md", "", "[rel](../rel.md)" },
        { "/w/./z/../t.md", "", "[e](../v/other/e.md) [a](/v/a.md)" } };
    std::vector<std::string> texts;
    for (const auto& file : linked) {
        TinyPdf::BookChapter chapter;
        chapter.path = file[0];
        chapter.title = file[1];
        chapter.part = chapter.path.empty();
        book.chapters.push_back(chapter);
        texts.push_back(file[2]);
    }
    TinyPdf::Internal::AddBacklinks(book, texts);
    const std::vector<std::string> expected = {
        "[b](sub/b.md) and [c](sub/deep/c%20d.md) and [e](other/e.md) and [b again](sub/b.md)\n\nLinked from: [Bee \\(2\\)](<sub/b.md>), [e](<other/e.md>)\n",
        "[a](../a.md), [c](deep/c%20d.md)\n\nLinked from: [a](<../a.md>), [c d](<deep/c%20d.md>)\n",
        "[b](../b.md) [e](../../other/e.md) [self](c%20d.md)\n\nLinked from: [a](<../../a.md>), [Bee \\(2\\)](<../b.md>)\n", "",
        "[a](../a.md)\n\nLinked from: [a](<../a.md>), [c d](<../sub/deep/c%20d.md>), [t](<../../w/t.md>)\n",
        "[a](/v/a.md) [x](x/y.md)\n\nLinked from: [y](<x/y.md>)\n", "[rel](../rel.md)\n\nLinked from: [rel](<../rel.md>)\n",
        "[e](../v/other/e.md) [a](/v/a.md)" };
    for (size_t index = 0; index < texts.size(); index++) {
        if (texts[index] == expected[index]) continue;
        std::cerr << "backlinks of " << index << ": \"" << texts[index] << "\"" << std::endl;
        ok = false;
    }
    return ok;
}

// Books of linked notes: breadth first in link order to the depth, each note once (also under
// two names), nothing missing or outside the vault, a note's name as its title, the backlinks'
// links within the book, and the limit of notes.
bool CheckLinkedNotes() {
    if (!CheckPathHelpers() || !CheckBookLinks()) return false;
    const fs::path root = fs::temp_directory_path() / "rayomd-test-linked";
    fs::remove_all(root);
    const fs::path vault = root / "vault";
    WriteNote(vault / "root.md", "# Root\n\nSee [[one]], [two](two.md), [[missing]], [out](../outside.md), [web](https://example.com), "
        "[[one]] again and [also](hard.md).\n");
    WriteNote(vault / "one.md", "Body one, then [[three]].\n");
    WriteNote(vault / "two.md", "# Two\n\nBack to [[root]] and [[v1.2 notes]].\n");
    WriteNote(vault / "v1.2 notes.md", "# Versions\n");
    WriteNote(vault / "three.md", "# Three\n");
    WriteNote(root / "outside.md", "# Outside\n");
    std::error_code error;
    fs::create_hard_link(vault / "one.md", vault / "hard.md", error);
    TinyPdf::PdfOptions options;
    options.dialect = TinyPdf::MarkdownDialect::Obsidian;
    const auto stems = [&](int depth, size_t& leftOut) {
        TinyPdf::Book book;
        std::string message;
        std::string out;
        if (!RayoMd::Book::ReadLinkedBook(vault / "root.md", options, depth, book, leftOut, message)) return out;
        for (const TinyPdf::BookChapter& chapter : book.chapters) {
            out += RayoMd::File::PathToUtf8(RayoMd::File::PathFromUtf8(chapter.path).stem()) + "=" + chapter.title + ";";
        }
        return out;
    };
    size_t leftOut = 0;
    if (stems(0, leftOut) != "root=;" || stems(1, leftOut) != "root=;one=one;two=two;" ||
        stems(2, leftOut) != "root=;one=one;two=two;three=three;v1.2 notes=v1.2 notes;" || leftOut != 0) {
        std::cerr << "linked notes: " << stems(2, leftOut) << std::endl;
        return false;
    }
    TinyPdf::Book book;
    std::string message;
    if (!RayoMd::Book::ReadLinkedBook(vault / "root.md", options, 2, book, leftOut, message)) return Fail("a book of linked notes");
    std::string plain;
    std::string backlinked;
    TinyPdf::PdfOptions withBacklinks = options;
    withBacklinks.backlinks = true;
    if (!TinyPdf::BuildBookPdf(book, options, plain).Ok() || !TinyPdf::BuildBookPdf(book, withBacklinks, backlinked).Ok() ||
        CountOccurrences(backlinked, "(Linked from:) Tj") != 5 || CountOccurrences(backlinked, "/Dest") != CountOccurrences(plain, "/Dest") + 5 ||
        CountOccurrences(backlinked, "/URI") != CountOccurrences(plain, "/URI")) {
        return Fail("backlinks");
    }
    // The note of a thousand links: a book of at most 1,000 notes, the others counted.
    std::string hub = "# Hub\n\n";
    for (size_t index = 0; index < RayoMd::Book::kMaxLinkedNotes + 1; index++) {
        const std::string name = "n" + std::to_string(index);
        WriteNote(vault / "many" / (name + ".md"), name + "\n");
        hub += "[" + name + "](many/" + name + ".md)\n";
    }
    WriteNote(vault / "hub.md", hub);
    TinyPdf::Book many;
    const bool read = RayoMd::Book::ReadLinkedBook(vault / "hub.md", options, 1, many, leftOut, message);
    fs::remove_all(root);
    if (!read || many.chapters.size() != RayoMd::Book::kMaxLinkedNotes || leftOut != 2) return Fail("at most 1,000 linked notes");
    return true;
}

} // namespace embeds_tests
