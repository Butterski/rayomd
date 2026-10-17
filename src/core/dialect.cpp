// SPDX-License-Identifier: Apache-2.0

#include "dialect.h"

#include "markdown_parser.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace TinyPdf::Internal {

namespace {

thread_local MarkdownDialect g_dialect = MarkdownDialect::GitHub;

constexpr size_t kNone = std::string_view::npos;

bool IsBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsBlank(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsBlank(text.back())) text.remove_suffix(1);
    return text;
}

// `text` in any case equals `lower`, which is in lower case.
bool EqualsLower(std::string_view text, std::string_view lower) {
    if (text.size() != lower.size()) return false;
    for (size_t index = 0; index < text.size(); index++) {
        if (Lower(text[index]) != lower[index]) return false;
    }
    return true;
}

// `text` in link text, shown as written: what would be syntax there escaped (the escapes the inline
// parser takes; not parentheses, as "\(" starts a formula).
void AppendEscaped(std::string& out, std::string_view text) {
    for (const char c : text) {
        if (c == '\\' || c == '`' || c == '*' || c == '_' || c == '[' || c == ']' || c == '$') out += '\\';
        out += c;
    }
}

// `text` as an image's alt text, which shows as written: without brackets, which would end it.
void AppendAlt(std::string& out, std::string_view text) {
    for (const char c : text) {
        if (c != '[' && c != ']') out += c;
    }
}

// `text` as a link destination in angle brackets.
void AppendDestination(std::string& out, std::string_view text) {
    for (const char c : text) {
        if (c == '<' || c == '>' || c == '\\') out += '\\';
        out += c;
    }
}

// `text` as an HTML attribute value in double quotes.
void AppendAttribute(std::string& out, std::string_view text) {
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c; break;
        }
    }
}

// The part of a path after its last '.' in its last component, empty without one.
std::string_view Extension(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.rfind('.');
    if (dot == kNone || (slash != kNone && dot < slash)) return {};
    return path.substr(dot + 1);
}

bool IsImageName(std::string_view path) {
    const std::string_view extension = Extension(path);
    for (const std::string_view image : { "png", "jpg", "jpeg", "svg", "gif", "webp", "bmp", "tif", "tiff", "avif" }) {
        if (EqualsLower(extension, image)) return true;
    }
    return false;
}

// The fence a line opens or closes, past its indentation and quote markers: three or more
// backticks (with no backtick after them) or tildes.
struct Fence {
    char marker = 0;
    size_t length = 0;
    bool bare = false;   // nothing but white space after it, so it can close a fence
};

Fence FenceOf(std::string_view line) {
    size_t at = 0;
    while (at < line.size() && (IsBlank(line[at]) || line[at] == '>')) at++;
    if (at == line.size() || (line[at] != '`' && line[at] != '~')) return {};
    const char marker = line[at];
    size_t end = at;
    while (end < line.size() && line[end] == marker) end++;
    if (end - at < 3) return {};
    const std::string_view rest = line.substr(end);
    if (marker == '`' && rest.find('`') != kNone) return {};
    return { marker, end - at, Trim(rest).empty() };
}

// Past its indentation and quote markers, a line that opens display math: "$$" and no "$$" after.
bool OpensDisplayMath(std::string_view line) {
    size_t at = 0;
    while (at < line.size() && (IsBlank(line[at]) || line[at] == '>')) at++;
    return line.substr(at, 2) == "$$" && line.find("$$", at + 2) == kNone;
}

// A line of '=' alone, a setext heading's underline.
bool IsEqualsLine(std::string_view line) {
    const std::string_view text = Trim(line);
    return !text.empty() && text.find_first_not_of('=') == kNone;
}

// The next `pattern` at or after a position in a line, for positions that only grow: however many
// openers ask, each byte of the line is searched once.
class NextOf {
public:
    explicit NextOf(std::string_view text) : pattern(text) {}
    void Reset(std::string_view text) {
        line = text;
        searched = false;
    }
    size_t From(size_t position) {
        if (!searched || position < from || (at != kNone && at < position)) {
            at = line.find(pattern, position);
            from = position;
            searched = true;
        }
        return at;
    }

private:
    std::string_view pattern;
    std::string_view line;
    size_t from = 0;
    size_t at = kNone;
    bool searched = false;
};

// One line after another outside fenced code and display math, Obsidian's syntax turned into
// RayoMD's (ObsidianToCommonMark).
class Converter {
public:
    explicit Converter(std::string& output) : out(output) {}
    void Convert(std::string_view line);
    bool inComment = false;   // in a %% comment that a line before opened

private:
    struct Run {
        size_t at;
        size_t length;
        size_t close;   // the next run of the same length, the code span's end; kNone for none
    };
    size_t CodeSpanEnd(size_t at);
    void Embed(std::string_view inner, bool alone);
    void WikiLink(std::string_view inner);

    std::string& out;
    std::string_view line;
    NextOf closeBrackets{ "]]" };
    NextOf openBracket{ "[" };
    NextOf closeMark{ "==" };
    NextOf closeComment{ "%%" };
    NextOf dollar{ "$" };
    NextOf dollars{ "$$" };
    std::vector<Run> runs;   // the line's backtick runs, once one is met
    bool runsFound = false;
    size_t runCursor = 0;
    std::unordered_map<size_t, size_t> nextOfLength;
};

// The end of the code span whose backticks start at `at`: after its closing run of as many
// backticks, or after its own run when none closes it. The runs are found once a line, and each
// run's closing one in one pass back over them, so a line of unmatched runs stays linear.
size_t Converter::CodeSpanEnd(size_t at) {
    if (!runsFound) {
        runsFound = true;
        runs.clear();
        nextOfLength.clear();
        runCursor = 0;
        for (size_t index = at; index < line.size();) {
            if (line[index] != '`') {
                index++;
                continue;
            }
            size_t end = index;
            while (end < line.size() && line[end] == '`') end++;
            runs.push_back({ index, end - index, kNone });
            index = end;
        }
        for (size_t index = runs.size(); index-- > 0;) {
            const auto found = nextOfLength.find(runs[index].length);
            if (found != nextOfLength.end()) runs[index].close = found->second;
            nextOfLength[runs[index].length] = index;
        }
    }
    while (runCursor < runs.size() && runs[runCursor].at + runs[runCursor].length <= at) runCursor++;
    if (runCursor == runs.size()) return at + 1;
    const Run& run = runs[runCursor];
    if (run.at != at || run.close == kNone) return run.at + run.length;   // escaped into, or unclosed
    const Run& close = runs[run.close];
    runCursor = run.close + 1;
    return close.at + close.length;
}

void Converter::Convert(std::string_view text) {
    line = text;
    closeBrackets.Reset(text);
    openBracket.Reset(text);
    closeMark.Reset(text);
    closeComment.Reset(text);
    dollar.Reset(text);
    dollars.Reset(text);
    runsFound = false;
    size_t i = 0;
    if (inComment) {
        const size_t end = closeComment.From(0);
        if (end == kNone) return;
        inComment = false;
        i = end + 2;
    }
    if (IsEqualsLine(text.substr(i))) {
        out.append(text.substr(i));
        return;
    }
    size_t markClose = kNone;
    while (i < text.size()) {
        if (i == markClose) {
            out += "</mark>";
            i += 2;
            markClose = kNone;
            continue;
        }
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '\\' && next != '\0') {
            out.append(text.substr(i, 2));
            i += 2;
            continue;
        }
        if (c == '`') {
            const size_t end = CodeSpanEnd(i);
            out.append(text.substr(i, end - i));
            i = end;
            continue;
        }
        if (c == '$') {
            size_t end = i + 1;
            if (next == '$') {
                const size_t close = dollars.From(i + 2);
                end = close == kNone ? i + 2 : close + 2;
            } else if (next != '\0' && !IsBlank(next)) {
                const size_t close = dollar.From(i + 1);
                if (close != kNone && !IsBlank(text[close - 1]) &&
                    (close + 1 == text.size() || text[close + 1] < '0' || text[close + 1] > '9')) {
                    end = close + 1;
                }
            }
            out.append(text.substr(i, end - i));
            i = end;
            continue;
        }
        if (c == '%' && next == '%') {
            const size_t close = closeComment.From(i + 2);
            if (close == kNone) {
                inComment = true;
                break;
            }
            i = close + 2;
            continue;
        }
        const bool embed = c == '!' && next == '[' && i + 2 < text.size() && text[i + 2] == '[';
        if (embed || (c == '[' && next == '[')) {
            const size_t start = i + (embed ? 3 : 2);
            const size_t close = closeBrackets.From(start);
            const size_t bracket = openBracket.From(start);
            if (close != kNone && close > start && (bracket == kNone || bracket > close)) {
                const std::string_view inner = text.substr(start, close - start);
                if (embed) {
                    size_t before = 0;
                    while (before < i && (IsBlank(text[before]) || text[before] == '>')) before++;
                    Embed(inner, before == i && Trim(text.substr(close + 2)).empty());
                    i = close + 2;
                    continue;
                }
                if (!EqualsLower(inner, "_toc_")) {
                    WikiLink(inner);
                    i = close + 2;
                    continue;
                }
            }
        }
        if (c == '=' && next == '=' && markClose == kNone && (i == 0 || text[i - 1] != '=') && i + 2 < text.size() &&
            !IsBlank(text[i + 2]) && text[i + 2] != '=') {
            const size_t close = closeMark.From(i + 2);
            if (close != kNone && !IsBlank(text[close - 1]) && text[close - 1] != '\\' &&
                (close + 2 == text.size() || text[close + 2] != '=')) {
                out += "<mark>";
                markClose = close;
                i += 2;
                continue;
            }
        }
        out += c;
        i++;
    }
    if (markClose != kNone) out += "</mark>";   // its "==" went into code or a formula
}

// "![[inner]]": an image, sized by "|300" or "|300x200" (else what follows "|" is its alt text),
// or, naming no image, a link to the note.
void Converter::Embed(std::string_view inner, bool alone) {
    const size_t bar = inner.find('|');
    std::string_view target = inner.substr(0, bar);
    if (!target.empty() && target.back() == '\\') target.remove_suffix(1);   // "\|" in a table
    target = Trim(target);
    if (!IsImageName(target)) {
        WikiLink(inner);
        return;
    }
    const std::string_view parameter = bar == kNone ? std::string_view() : Trim(inner.substr(bar + 1));
    const size_t digits = parameter.find_first_not_of("0123456789");
    std::string_view width;
    std::string_view height;
    std::string_view alt;
    if (!parameter.empty() && digits == kNone) {
        width = parameter;
    } else if (digits != kNone && digits > 0 && parameter[digits] == 'x' && digits + 1 < parameter.size() &&
               parameter.find_first_not_of("0123456789", digits + 1) == kNone) {
        width = parameter.substr(0, digits);
        height = parameter.substr(digits + 1);
    } else {
        alt = parameter;
    }
    if (alt.empty()) {
        const size_t slash = target.find_last_of("/\\");
        alt = slash == kNone ? target : target.substr(slash + 1);
    }
    if (alone && !width.empty()) {
        out += "<img src=\"";
        AppendAttribute(out, target);
        out += "\" alt=\"";
        AppendAttribute(out, alt);
        out += "\" width=\"";
        out += width;
        out += '"';
        if (!height.empty()) {
            out += " height=\"";
            out += height;
            out += '"';
        }
        out += '>';
        return;
    }
    out += "![";
    AppendAlt(out, alt);
    out += "](<";
    AppendDestination(out, target);
    out += ">)";
}

// "[[note#heading|text]]": a link to note.md (the note as named when it has an extension) at the
// heading, showing the text, else "note > heading". Without a note, a link to the heading in this
// document; one to a block ("#^id") there is only its text.
void Converter::WikiLink(std::string_view inner) {
    const size_t bar = inner.find('|');
    std::string_view target = inner.substr(0, bar);
    if (!target.empty() && target.back() == '\\') target.remove_suffix(1);   // "\|" in a table
    target = Trim(target);
    const std::string_view alias = bar == kNone ? std::string_view() : Trim(inner.substr(bar + 1));
    const size_t hash = target.find('#');
    const std::string_view note = Trim(target.substr(0, hash));
    const std::string_view headings = hash == kNone ? std::string_view() : target.substr(hash + 1);
    const size_t last = headings.rfind('#');
    const std::string_view anchor = Trim(last == kNone ? headings : headings.substr(last + 1));
    const bool block = !anchor.empty() && anchor[0] == '^';
    std::string shown(alias);
    if (shown.empty()) {
        shown = note;
        for (size_t start = 0; hash != kNone && start <= headings.size();) {
            size_t end = headings.find('#', start);
            if (end == kNone) end = headings.size();
            const std::string_view part = Trim(headings.substr(start, end - start));
            if (!part.empty()) {
                if (!shown.empty()) shown += " > ";
                shown += part;
            }
            start = end + 1;
        }
    }
    if (shown.empty() || (note.empty() && (anchor.empty() || block))) {
        AppendEscaped(out, shown.empty() ? inner : std::string_view(shown));
        return;
    }
    out += '[';
    AppendEscaped(out, shown);
    out += "](<";
    if (!note.empty()) {
        AppendDestination(out, note);
        if (Extension(note).empty()) out += ".md";
    }
    if (!anchor.empty() && !block) {
        out += '#';
        AppendDestination(out, anchor);
    }
    out += ">)";
}

// Obsidian's callout types and their colours (TakeCallout). Arrays, not pointers: no relocations.
struct CalloutKind {
    char name[10];
    uint8_t colour;
};
constexpr CalloutKind kCalloutKinds[] = {
    { "note", 1 }, { "info", 1 }, { "todo", 1 },
    { "success", 2 }, { "check", 2 }, { "done", 2 },
    { "example", 3 },
    { "question", 4 }, { "help", 4 }, { "faq", 4 }, { "warning", 4 }, { "caution", 4 }, { "attention", 4 },
    { "failure", 5 }, { "fail", 5 }, { "missing", 5 }, { "danger", 5 }, { "error", 5 }, { "bug", 5 },
    { "abstract", 6 }, { "summary", 6 }, { "tldr", 6 }, { "tip", 6 }, { "hint", 6 }, { "important", 6 },
    { "quote", 7 }, { "cite", 7 },
};

} // namespace

DialectScope::DialectScope(MarkdownDialect dialect) : previous(g_dialect) {
    g_dialect = dialect;
}

DialectScope::~DialectScope() {
    g_dialect = previous;
}

bool ObsidianDialect() {
    return g_dialect == MarkdownDialect::Obsidian;
}

std::string ObsidianToCommonMark(std::string_view markdown) {
    std::string out;
    out.reserve(markdown.size() + markdown.size() / 8);
    Converter converter(out);
    // The front matter, when a line "---" or "..." closes the first line "---".
    size_t frontMatterEnd = 0;
    if (Trim(markdown.substr(0, markdown.find('\n'))) == "---") {
        for (size_t start = markdown.find('\n'); start != kNone && frontMatterEnd == 0;) {
            const size_t end = markdown.find('\n', start + 1);
            const std::string_view line = Trim(markdown.substr(start + 1, end == kNone ? kNone : end - start - 1));
            if (line == "---" || line == "...") frontMatterEnd = end == kNone ? markdown.size() : end;
            start = end;
        }
    }
    out.append(markdown.substr(0, frontMatterEnd));
    Fence fence;
    bool displayMath = false;
    size_t start = frontMatterEnd;
    if (frontMatterEnd > 0) {
        if (frontMatterEnd == markdown.size()) return out;
        out += '\n';
        start++;
    }
    for (;;) {
        size_t end = markdown.find('\n', start);
        if (end == kNone) end = markdown.size();
        const std::string_view line = markdown.substr(start, end - start);
        if (converter.inComment) {
            converter.Convert(line);
        } else if (fence.marker != 0) {
            out.append(line);
            const Fence closing = FenceOf(line);
            if (closing.marker == fence.marker && closing.length >= fence.length && closing.bare) fence = {};
        } else if (displayMath) {
            out.append(line);
            if (line.find("$$") != kNone || Trim(line).empty()) displayMath = false;
        } else if (const Fence opening = FenceOf(line); opening.marker != 0) {
            out.append(line);
            fence = opening;
        } else if (OpensDisplayMath(line)) {
            out.append(line);
            displayMath = true;
        } else {
            converter.Convert(line);
        }
        if (end == markdown.size()) break;
        out += '\n';
        start = end + 1;
    }
    return out;
}

uint8_t TakeCallout(std::string& quote, std::string& title) {
    const std::string_view text = quote;
    size_t start = 0;
    while (start < text.size() && IsBlank(text[start])) start++;
    if (text.size() - start < 4 || text[start] != '[' || text[start + 1] != '!') return 0;
    const size_t lineEnd = std::min(text.find('\n'), text.size());
    const std::string_view line = text.substr(start, lineEnd - start);
    const size_t close = line.find(']');
    if (close == kNone || close < 3) return 0;
    const std::string_view type = line.substr(2, close - 2);
    for (const char c : type) {
        if (IsBlank(c)) return 0;
    }
    size_t rest = close + 1;
    if (rest < line.size() && (line[rest] == '+' || line[rest] == '-')) rest++;
    if (rest < line.size() && !IsBlank(line[rest])) return 0;
    uint8_t colour = 1;
    for (const CalloutKind& kind : kCalloutKinds) {
        if (EqualsLower(type, kind.name)) {
            colour = kind.colour;
            break;
        }
    }
    const std::string_view written = Trim(line.substr(rest));
    title = written.empty() ? std::string() : StripInlineMarkdown(written);
    if (Trim(title).empty()) {
        title.assign(type);
        for (char& c : title) c = Lower(c);
        if (title[0] >= 'a' && title[0] <= 'z') title[0] = (char)(title[0] - 32);
    }
    quote.erase(0, lineEnd == text.size() ? text.size() : lineEnd + 1);
    return colour;
}

} // namespace TinyPdf::Internal
