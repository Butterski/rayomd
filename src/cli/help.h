// SPDX-License-Identifier: Apache-2.0

#pragma once

// The help of both command lines: `--help` and `-h` print it on standard output, a command line
// without arguments on standard error. One text, laid out as it is printed (LayOutHelp), so the
// binaries keep no padding: a tab moves on to the description column, onto the next line when
// what comes before it reaches the column, and a line that starts with a tab goes on with the
// description above it. A command line defines RAYOMD_HELP_TITLE, RAYOMD_HELP_PROGRAM and
// RAYOMD_HELP_IMAGE_CACHE before it includes this file; RAYOMD_SERVE_JSON keeps --serve --json.
// The options the help names are also what an unknown argument is matched against (NearestWord).

#include <cstddef>
#include <initializer_list>
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

// The help before LayOutHelp, a tab before each description.
constexpr const char kHelpSource[] = RAYOMD_HELP_TITLE " " RAYOMD_VERSION ", Markdown to PDF\n"
        "\n"
        "Usage: " RAYOMD_HELP_PROGRAM " <command> [style] [margin] [options]\n"
        "\n"
        "Commands:\n"
        "  --export <in.md> [out.pdf]\tExport one Markdown file; without out.pdf\n"
        "\tto in.pdf beside it, into a folder given, or\n"
        "\twith - to standard output\n"
        "  --stdin <out.pdf>\tExport Markdown read from standard input\n"
        "\t(- for standard output)\n"
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
        "  margin=0.75in, margin=54pt or margin=1.5cm (in, pt, cm, mm)\n"
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
        "  --page-numbers\t\"N\" at the foot of every page; \"N / M\":\n"
        "\ta theme's footer-center = {page} / {pages}\n"
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
        "  " RAYOMD_HELP_PROGRAM " --book SUMMARY.md book.pdf --toc\n";

inline std::string HelpText() {
    return LayOutHelp(kHelpSource);
}

constexpr char LowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool EqualsIgnoringCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    }
    return true;
}

// The commands of the help's Commands block. A command line's options follow its command and its
// paths; options given before the command are moved after them.
inline bool IsCommand(std::string_view argument) {
    for (const std::string_view command : { "--export", "--stdin", "--book", "--batch", "--stdin-batch", "--serve",
             "--bench", "--inspect-source", "--recover-source", "--doctor", "--version", "-v", "--help", "-h" }) {
        if (EqualsIgnoringCase(argument, command)) return true;
    }
    return false;
}

// The options that take a value, as `--name=VALUE` or as the next argument (`--theme FILE`).
inline bool TakesValue(std::string_view argument) {
    for (const std::string_view option : { "--toc-depth", "--page-break-before", "--dialect", "--footnotes", "--vault",
             "--embeds", "--theme", "--page-size", "--report", "--workers" }) {
        if (EqualsIgnoringCase(argument, option)) return true;
    }
    return false;
}

// The options whose level may be left out, --number-headings and --follow-links: a digit as the
// next argument is theirs (`--number-headings 2`), not a style's number.
inline bool TakesLevel(std::string_view argument, std::string_view next) {
    return (EqualsIgnoringCase(argument, "--number-headings") || EqualsIgnoringCase(argument, "--follow-links")) &&
        next.size() == 1 && next[0] >= '0' && next[0] <= '9';
}

// The style and margin words, which stand without dashes.
constexpr std::string_view kStyleAndMarginWords[] = { "elegant", "modern", "tech", "compact", "normal", "wide" };

// The next `--name` the help shows from `at` on, `at` moved past it; empty at the end. A name can
// come more than once.
inline std::string_view NextHelpFlag(size_t& at) {
    const std::string_view help(kHelpSource);
    for (; at + 2 < help.size(); at++) {
        if (help[at] != '-' || help[at + 1] != '-') continue;
        if (at > 0 && help[at - 1] != ' ' && help[at - 1] != '\t' && help[at - 1] != '(') continue;
        size_t end = at + 2;
        while (end < help.size() && ((help[end] >= 'a' && help[end] <= 'z') || (help[end] >= '0' && help[end] <= '9') ||
                                        help[end] == '-')) {
            end++;
        }
        if (end == at + 2) continue;
        const std::string_view flag = help.substr(at, end - at);
        at = end;
        return flag;
    }
    at = help.size();
    return std::string_view();
}

// The optimal string alignment distance of `a` and `b` (insertions, deletions, substitutions and
// swaps of neighbours, without letter case); words of at most 63 bytes.
inline size_t EditDistance(std::string_view a, std::string_view b) {
    size_t rows[3][64];
    size_t* before = rows[0];
    size_t* previous = rows[1];
    size_t* current = rows[2];
    for (size_t j = 0; j <= b.size(); j++) previous[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        current[0] = i;
        for (size_t j = 1; j <= b.size(); j++) {
            size_t best = previous[j - 1] + (LowerAscii(a[i - 1]) == LowerAscii(b[j - 1]) ? 0 : 1);
            if (previous[j] + 1 < best) best = previous[j] + 1;
            if (current[j - 1] + 1 < best) best = current[j - 1] + 1;
            if (i > 1 && j > 1 && LowerAscii(a[i - 1]) == LowerAscii(b[j - 2]) &&
                LowerAscii(a[i - 2]) == LowerAscii(b[j - 1]) && before[j - 2] + 1 < best) {
                best = before[j - 2] + 1;
            }
            current[j] = best;
        }
        size_t* const reused = before;
        before = previous;
        previous = current;
        current = reused;
    }
    return previous[b.size()];
}

// The option of the help, or the style or margin word, that `typed` most likely meant (its
// `=VALUE` left out): one edit away for up to 6 bytes, two for more, else the only option it
// begins, from 5 bytes. Empty when none is that near, or `typed` is one of them.
inline std::string NearestWord(std::string_view typed) {
    typed = typed.substr(0, typed.find('='));
    if (typed.empty() || typed.size() > 63) return std::string();
    const size_t allowed = typed.size() <= 6 ? 1 : 2;
    std::string_view best;
    size_t bestDistance = allowed + 1;
    std::string_view begun;
    size_t begunCount = 0;
    bool exact = false;
    const auto consider = [&](std::string_view word) {
        if (word.size() > 63) return;
        const size_t distance = EditDistance(typed, word);
        exact = exact || distance == 0;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = word;
        }
        if (typed.size() >= 5 && word.size() > typed.size() && EqualsIgnoringCase(word.substr(0, typed.size()), typed) &&
            word != begun) {
            begun = word;
            begunCount++;
        }
    };
    size_t at = 0;
    for (std::string_view flag = NextHelpFlag(at); !flag.empty(); flag = NextHelpFlag(at)) consider(flag);
    for (const std::string_view word : kStyleAndMarginWords) consider(word);
    if (exact) return std::string();
    if (bestDistance <= allowed) return std::string(best);
    return begunCount == 1 ? std::string(begun) : std::string();
}

// `name=FORM` as the help first shows the option `name` (--theme=FILE), else `name=VALUE`.
inline std::string ValueForm(std::string_view name) {
    const std::string_view help(kHelpSource);
    size_t at = 0;
    for (std::string_view flag = NextHelpFlag(at); !flag.empty(); flag = NextHelpFlag(at)) {
        if (flag != name || help[at] != '=') continue;
        size_t end = at;
        while (end < help.size() && help[end] != '\t' && help[end] != ' ' && help[end] != '\n' && help[end] != ')') end++;
        return std::string(help.substr(at - flag.size(), flag.size() + end - at));
    }
    return std::string(name) + "=VALUE";
}

// What to say after an export option that was not understood: a sentence with a leading space, or
// nothing.
inline std::string UnknownOptionHint(std::string_view typed) {
    const std::string_view name = typed.substr(0, typed.find('='));
    if (EqualsIgnoringCase(name, "-o") || EqualsIgnoringCase(name, "--output") || EqualsIgnoringCase(name, "--out")) {
        return " The output is the path after the input: --export in.md out.pdf.";
    }
    if (TakesValue(name) && name.size() == typed.size()) {
        std::string lowered(name);
        for (char& c : lowered) c = LowerAscii(c);
        return " It takes a value: " + ValueForm(lowered) + ".";
    }
    const std::string nearest = NearestWord(typed);
    return nearest.empty() ? std::string() : " Did you mean " + nearest + "?";
}

// What to say after an unknown command: a sentence with a leading space, or nothing.
inline std::string UnknownCommandHint(std::string_view typed) {
    const std::string_view name = typed.substr(0, typed.find('='));
    bool option = false;
    size_t at = 0;
    for (std::string_view flag = NextHelpFlag(at); !flag.empty() && !option; flag = NextHelpFlag(at)) {
        option = EqualsIgnoringCase(flag, name);
    }
    for (const std::string_view word : kStyleAndMarginWords) option = option || EqualsIgnoringCase(word, typed);
    if (option) return " It is an option, which follows a command: --export in.md " + std::string(typed) + ".";
    if (!typed.empty() && typed[0] != '-') {
        return " To export a file: " RAYOMD_HELP_PROGRAM " --export " + std::string(typed) + ".";
    }
    const std::string nearest = NearestWord(typed);
    return nearest.empty() ? std::string() : " Did you mean " + nearest + "?";
}

} // namespace RayoMd::Cli
