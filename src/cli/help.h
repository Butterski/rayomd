// SPDX-License-Identifier: Apache-2.0

#pragma once

// The help of both command lines: `--help` and `-h` print it on standard output, a command line
// without arguments on standard error. One text, laid out as it is printed (LayOutHelp), so the
// binaries keep no padding: a tab moves on to the description column, onto the next line when
// what comes before it reaches the column, and a line that starts with a tab goes on with the
// description above it. A command line defines RAYOMD_HELP_TITLE, RAYOMD_HELP_PROGRAM and
// RAYOMD_HELP_IMAGE_CACHE before it includes this file; RAYOMD_SERVE_JSON keeps --serve --json.

#include <cstddef>
#include <string>
#include <string_view>

#ifndef RAYOMD_VERSION
#define RAYOMD_VERSION "0.0.0"
#endif

namespace RayoMd::Cli {

// Where descriptions start: two spaces after the longest option, --allow-unsafe-local-images,
// which leaves 48 columns for them in 79.
constexpr size_t kHelpColumn = 31;

inline std::string LayOutHelp(std::string_view text) {
    std::string out;
    out.reserve(text.size() + text.size() / 2);
    size_t line = 0;   // where the line being written starts in `out`
    for (const char c : text) {
        if (c == '\t') {
            if (out.size() - line + 2 > kHelpColumn) {
                out += '\n';
                line = out.size();
            }
            out.append(kHelpColumn - (out.size() - line), ' ');
        } else {
            out += c;
            if (c == '\n') line = out.size();
        }
    }
    return out;
}

// The line an argument error ends with, instead of the whole help.
constexpr const char kHelpHint[] = "Run '" RAYOMD_HELP_PROGRAM " --help' for the commands and options.\n";

inline std::string HelpText() {
    return LayOutHelp(RAYOMD_HELP_TITLE " " RAYOMD_VERSION ", Markdown to PDF\n"
        "\n"
        "Usage: " RAYOMD_HELP_PROGRAM " <command> [style] [margin] [options]\n"
        "\n"
        "Commands:\n"
        "  --export <in.md> <out.pdf>\tExport one Markdown file\n"
        "  --stdin <out.pdf>\tExport Markdown read from standard input\n"
        "  --book <in...> <out.pdf>\tOne PDF of several files (Books, below)\n"
        "  --batch <in-dir> <out-dir>\tExport the Markdown files of a folder\n"
        "  --stdin-batch <out-dir>\tExport each Markdown path read from stdin\n"
        "  --serve <out-dir>\tStay running: export each path read from\n"
        "\tstdin, answering OK or ERR; quit ends it\n"
#if RAYOMD_SERVE_JSON
        "  --serve --json\tThe same, for JSON requests (Serve, below)\n"
#endif
        "  --bench <in.md> <out-dir> <n>\tTime n warm exports of one file\n"
        "  --inspect-source <in.pdf>\tCheck the Markdown source a PDF carries\n"
        "  --recover-source <in.pdf> <out.md>\tWrite that source out, byte for byte\n"
        "  --doctor\tCheck fonts, image support and an export\n"
        "  --version, -v\tPrint the version\n"
        "  --help, -h\tPrint this help\n"
        "\n"
        "Styles: elegant (default), modern, tech\n"
        "Margins: compact (34pt), normal (54pt, default), wide (72pt),\n"
        "  margin=0.75in or margin=54pt\n"
        "\n"
        "Output:\n"
        "  --compress\tSmaller files (FlateDecode), slower export\n"
        "  --pdfa\tPDF/A-3b, every font embedded (formulas' too)\n"
        "  --pdfua\tPDF/UA-1, tagged for screen readers; language\n"
        "\tfrom the front matter's lang, else en\n"
        "  --embed-source\tCarry the Markdown, recoverable byte for byte\n"
        "\t(anyone with the PDF can read all of it)\n"
        "  --black-text\tFor black-and-white printers: text black,\n"
        "\tfills and diagrams light grey\n"
        "  --theme=FILE\tFonts, colours, header, footer and cover\n"
        "\t(Themes, below)\n"
        "\n"
        "Images:\n"
        "  --allow-url-images\tFetch http and https images\n"
        "  --allow-unsafe-local-images\tLocal images outside the input's folder\n"
        "  --fast-images\tPNGs with transparency re-encoded faster,\n"
        "\ttheir streams larger\n"
        "  --no-image-cache\tKeep no re-encoded PNGs (Image cache, below)\n"
        "\n"
        "Content:\n"
        "  --toc\tA table of contents first, or at a [TOC] or\n"
        "\t[[_TOC_]] paragraph, which always gets one\n"
        "  --toc-depth=N\tHeading levels it lists, 1-6 (default 3)\n"
        "  --number-headings[=N]\t1, 1.1, 1.1.1 before the headings of N\n"
        "\tlevels, 1-6 (default 3)\n"
        "  --footnotes=page\tEach footnote at the foot of the page of its\n"
        "\tfirst reference (default: after the text)\n"
        "  --no-highlight\tCode in one colour\n"
        "  --no-diagrams\t```mermaid blocks as code, not drawn\n"
        "  --dialect=obsidian\tObsidian's Markdown, below (default github)\n"
        "\n"
        "Pages:\n"
        "  --page-numbers\t\"N / M\" at the foot of every page\n"
        "  --page-size=SIZE\ta4 (default), a3, a5, letter or legal, each\n"
        "\talso -landscape; WIDTHxHEIGHT in mm, cm, in\n"
        "\tor pt (210x297mm); WIDTHxauto (210mmxauto):\n"
        "\tone long page for screens\n"
        "  --page-break-before=hN\tA new page before headings of level 1 to N\n"
        "\n"
        "Batch:\n"
        "  --workers=N\t1-64 (default: automatic, at most 6)\n"
        "  --recursive\tSubfolders too, mirrored\n"
        "  --skip-unchanged\tKeep PDFs newer than their Markdown\n"
        "  --report=FILE\tOne JSON line a document (- for stdout)\n"
        "\n"
        "Obsidian (--dialect=obsidian): callouts, ==highlights==, %%comments%%,\n"
        "  [[wiki links]] and ![[images]]; ^block ids left out; ![[note]] alone on a\n"
        "  line prints that note, its #heading section or its #^id block.\n"
        "  --vault=DIR\tThe folder notes are found in by name and\n"
        "\tmust lie in, local images too (default: the\n"
        "\tinput's folder)\n"
        "  --embeds=link\tEmbedded notes stay links (default: content)\n"
        "  --hide-tags\t#tags left out\n"
        "\n"
        "Books (--book): a SUMMARY.md (mdBook, GitBook), a folder (its Markdown files\n"
        "  by name, README.md first) or the files given, each from a new page, the\n"
        "  links between them kept, one outline; with --toc one table of contents.\n"
        "  --follow-links[=N]\tWith --export: a book of the note and the notes\n"
        "\tit links to, N links deep (1-6, default 1)\n"
        "  --backlinks\tEnd each file with links to the files that\n"
        "\tlink to it\n"
        "\n"
        "Themes (--theme=FILE), key = value lines:\n"
        "  font, font-bold, font-italic, font-bold-italic: TrueType or OpenType files\n"
        "  heading-color, link-color, accent-color: colours such as #0B3D91\n"
        "  header-left/center/right, footer-left/center/right: text with {title}\n"
        "    {author} {subject} {date} {page} {pages}, or {logo} alone\n"
        "  logo: an image; cover = yes: a cover page first\n"
        "\n"
#if RAYOMD_SERVE_JSON
        "Serve (--serve --json): one JSON request a line, such as\n"
        "  {\"id\":\"1\",\"input\":\"a.md\",\"output\":\"a.pdf\",\"args\":[\"--toc\"]}\n"
        "  (\"markdown\":\"...\" and \"source_path\" in place of \"input\"), each answered\n"
        "  with its report line and id, after {\"rayomd\":\"VERSION\",\"protocol\":1};\n"
        "  quit ends it.\n"
        "\n"
#endif
        "Image cache: PNGs with transparency are re-encoded once and kept, at most\n"
        "  about 256 MB (the PDF is the same either way), in\n"
        "  " RAYOMD_HELP_IMAGE_CACHE "\n"
        "\n"
        "Examples:\n"
        "  " RAYOMD_HELP_PROGRAM " --export notes.md notes.pdf\n"
        "  " RAYOMD_HELP_PROGRAM " --export notes.md notes.pdf modern wide --toc --page-numbers\n"
        "  " RAYOMD_HELP_PROGRAM " --batch notes pdfs --recursive --workers=4\n"
        "  " RAYOMD_HELP_PROGRAM " --book SUMMARY.md book.pdf --toc\n");
}

} // namespace RayoMd::Cli
