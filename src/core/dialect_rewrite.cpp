// SPDX-License-Identifier: Apache-2.0

// Obsidian's syntax turned into RayoMD's before the parse (ObsidianToCommonMark, RewriteObsidian)
// and its callouts (TakeCallout): dialect.h. Built at -O2, as it reads every byte of a note on
// every build of a preview; the per-build dialect flag is dialect.cpp, at -Os.

#include "dialect.h"

#include "markdown_parser.h"
#include "../common/text_utils.h"

#include <algorithm>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOMD_DIALECT_SSE2 1
#endif

namespace TinyPdf::Internal {

namespace {

constexpr size_t kNone = std::string_view::npos;

bool IsBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// A byte that may start what Converter::Convert reads: an escape, a code span, a formula, a
// comment, an embed or wiki link, a highlight. Any other is copied as it is.
bool IsSyntaxByte(char c) {
    switch (c) {
    case '\\': case '`': case '$': case '%': case '!': case '[': case '=': return true;
    default: return false;
    }
}

// The first byte of text[from, stop) that IsSyntaxByte, else `stop`: 16 bytes a step in SSE2, one
// compare for each of the seven bytes. Most of a note is plain text, which this skips.
size_t NextSyntaxByte(const char* text, size_t from, size_t stop) {
    size_t at = from;
#if RAYOMD_DIALECT_SSE2
    const __m128i backslash = _mm_set1_epi8('\\'), tick = _mm_set1_epi8('`'), dollar = _mm_set1_epi8('$'),
                  percent = _mm_set1_epi8('%'), bang = _mm_set1_epi8('!'), bracket = _mm_set1_epi8('['),
                  equals = _mm_set1_epi8('=');
    for (; at + 16 <= stop; at += 16) {
        const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text + at));
        const __m128i hit = _mm_or_si128(
            _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(chunk, backslash), _mm_cmpeq_epi8(chunk, tick)),
                         _mm_or_si128(_mm_cmpeq_epi8(chunk, dollar), _mm_cmpeq_epi8(chunk, percent))),
            _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(chunk, bang), _mm_cmpeq_epi8(chunk, bracket)),
                         _mm_cmpeq_epi8(chunk, equals)));
        const unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(hit));
        if (bits != 0) return at + RayoMd::Text::Detail::LowestBit(bits);
    }
#endif
    while (at < stop && !IsSyntaxByte(text[at])) at++;
    return at;
}

// The first "[[", "==" or "%%" at or after `from`, or a '^' after white space or at a line's start
// (a block id, " ^id" or "^id" ending a line), the only syntax Converter::Convert and
// ObsidianToCommonMark change, else kNone: a line without any is copied as it is. In SSE2 16
// positions a step, each byte compared with the next; a pair never spans a line end.
size_t FindSyntaxPair(std::string_view text, size_t from) {
    const char* const data = text.data();
    const size_t size = text.size();
    size_t at = from;
    if (at < size && data[at] == '^' && (at == 0 || data[at - 1] == '\n' || data[at - 1] == ' ' || data[at - 1] == '\t')) {
        return at;
    }
#if RAYOMD_DIALECT_SSE2
    const __m128i bracket = _mm_set1_epi8('['), equals = _mm_set1_epi8('='), percent = _mm_set1_epi8('%'),
                  caret = _mm_set1_epi8('^'), space = _mm_set1_epi8(' '), tab = _mm_set1_epi8('\t'),
                  newline = _mm_set1_epi8('\n');
    for (; at + 17 <= size; at += 16) {
        const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + at));
        const __m128i next = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + at + 1));
        const __m128i opener = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(chunk, bracket), _mm_cmpeq_epi8(chunk, equals)),
                                            _mm_cmpeq_epi8(chunk, percent));
        const __m128i blank = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(chunk, space), _mm_cmpeq_epi8(chunk, tab)),
                                           _mm_cmpeq_epi8(chunk, newline));
        const unsigned pairs = static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(opener, _mm_cmpeq_epi8(chunk, next))));
        // A block id's caret is the byte after the blank: one position later.
        const unsigned carets = static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(blank, _mm_cmpeq_epi8(next, caret))));
        const unsigned bits = pairs | (carets << 1);
        if (bits != 0) return at + RayoMd::Text::Detail::LowestBit(bits);
    }
#endif
    for (; at + 1 < size; at++) {
        const char c = data[at];
        if ((c == '[' || c == '=' || c == '%') && data[at + 1] == c) return at;
        if ((c == ' ' || c == '\t' || c == '\n') && data[at + 1] == '^') return at + 1;
    }
    return kNone;
}

bool IsBlockIdByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

// Where a line's Obsidian block id begins ("text ^id" or "^id" alone, white space after it), with
// the white space before it: what the PDF leaves out. kNone when the line ends in none.
size_t BlockIdStart(std::string_view line) {
    size_t end = line.size();
    while (end > 0 && IsBlank(line[end - 1])) end--;
    size_t caret = end;
    while (caret > 0 && IsBlockIdByte(line[caret - 1])) caret--;
    if (caret == end || caret == 0 || line[caret - 1] != '^') return kNone;
    caret--;
    if (caret > 0 && line[caret - 1] != ' ' && line[caret - 1] != '\t') return kNone;
    while (caret > 0 && IsBlank(line[caret - 1])) caret--;
    return caret;
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
// Always inlined: TagHider calls it too, and as a call it cost ObsidianToCommonMark 0.2 % of
// README.md's build.
RAYOMD_HOT_INLINE bool OpensDisplayMath(std::string_view line) {
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

// The code spans of one line: the end of the span whose backticks start at a position, after its
// closing run of as many backticks, or after its own run when none closes it. The runs are found
// once a line, and each run's closing one in one pass back over them, so a line of unmatched runs
// stays linear. Positions asked must grow within a line.
class CodeSpans {
public:
    void Reset() { found = false; }
    size_t End(std::string_view line, size_t at);

private:
    struct Run {
        size_t at;
        size_t length;
        size_t close;   // the next run of the same length, the code span's end; kNone for none
    };
    std::vector<Run> runs;   // the line's backtick runs, once one is met
    bool found = false;
    size_t cursor = 0;
    std::vector<size_t> nextOfLength;   // by run length, the run after the current one; reused
};

RAYOMD_HOT_INLINE size_t CodeSpans::End(std::string_view line, size_t at) {
    if (!found) {
        found = true;
        runs.clear();
        cursor = 0;
        size_t longest = 0;
        for (size_t index = at; index < line.size();) {
            if (line[index] != '`') {
                index++;
                continue;
            }
            size_t end = index;
            while (end < line.size() && line[end] == '`') end++;
            runs.push_back({ index, end - index, kNone });
            longest = std::max(longest, end - index);
            index = end;
        }
        nextOfLength.assign(longest + 1, kNone);   // at most the line's length
        for (size_t index = runs.size(); index-- > 0;) {
            runs[index].close = nextOfLength[runs[index].length];
            nextOfLength[runs[index].length] = index;
        }
    }
    while (cursor < runs.size() && runs[cursor].at + runs[cursor].length <= at) cursor++;
    if (cursor == runs.size()) return at + 1;
    const Run& run = runs[cursor];
    if (run.at != at || run.close == kNone) return run.at + run.length;   // escaped into, or unclosed
    const Run& close = runs[run.close];
    cursor = run.close + 1;
    return close.at + close.length;
}

// One line after another outside fenced code and display math, Obsidian's syntax turned into
// RayoMD's (ObsidianToCommonMark).
class Converter {
public:
    explicit Converter(std::string& output) : out(output) {}
    void Convert(std::string_view line);
    bool inComment = false;   // in a %% comment that a line before opened

private:
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
    CodeSpans spans;
};

void Converter::Convert(std::string_view text) {
    line = text;
    closeBrackets.Reset(text);
    openBracket.Reset(text);
    closeMark.Reset(text);
    closeComment.Reset(text);
    dollar.Reset(text);
    dollars.Reset(text);
    spans.Reset();
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
            const size_t end = spans.End(text, i);
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
        // This byte, and the bytes after it that start none of the syntax above, up to the close of
        // an open highlight, in one append.
        const size_t stop = markClose == kNone ? text.size() : markClose;
        const size_t end = NextSyntaxByte(text.data(), i + 1, stop);
        out.append(text.data() + i, end - i);
        i = end;
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

// The end of the front matter, the line feed after the line "---" or "..." that closes a first
// line "---" (the text's size when that line ends the text), else 0.
size_t FrontMatterEnd(std::string_view markdown) {
    if (Trim(markdown.substr(0, markdown.find('\n'))) != "---") return 0;
    for (size_t start = markdown.find('\n'); start != kNone;) {
        const size_t end = markdown.find('\n', start + 1);
        const std::string_view line = Trim(markdown.substr(start + 1, end == kNone ? kNone : end - start - 1));
        if (line == "---" || line == "...") return end == kNone ? markdown.size() : end;
        start = end;
    }
    return 0;
}

// The first byte of [at, end) that is one of `Bytes`, else `end`: 16 bytes a step in SSE2.
template <char... Bytes>
size_t FindAny(const char* text, size_t at, size_t end) {
#if RAYOMD_DIALECT_SSE2
    for (; at + 16 <= end; at += 16) {
        const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text + at));
        __m128i hit = _mm_setzero_si128();
        ((hit = _mm_or_si128(hit, _mm_cmpeq_epi8(chunk, _mm_set1_epi8(Bytes)))), ...);
        const unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(hit));
        if (bits != 0) return at + RayoMd::Text::Detail::LowestBit(bits);
    }
#endif
    while (at < end && ((text[at] != Bytes) && ...)) at++;
    return at;
}

// A byte of a tag's name: a letter (any non-ASCII byte), a digit, '_', '-' or '/'.
bool IsTagByte(unsigned char c) {
    return c >= 0x80 || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
        c == '/';
}

// The columns of a line's indentation, counted up to 4.
size_t Indentation(std::string_view line) {
    size_t columns = 0;
    for (size_t at = 0; at < line.size() && columns < 4; at++) {
        if (line[at] == ' ') columns++;
        else if (line[at] == '\t') columns = 4;
        else break;
    }
    return columns;
}

// --hide-tags (HideTags): the lines up to the last '#' read one by one for the fences, display
// formulas, indented code and comments they open and close; only a line with a '#' or "%%" is
// scanned, from one byte that can start syntax to the next (FindAny). The text between the tags
// is copied in runs.
class TagHider {
public:
    TagHider(std::string_view text, std::string& output) : markdown(text), out(output) {}
    bool Run();

private:
    void Scan(size_t lineStart, size_t lineEnd, size_t from);

    std::string_view markdown;
    std::string& out;
    size_t copied = 0;
    bool changed = false;
    bool inComment = false;
    NextOf closeBrackets{ "]]" };
    NextOf closeComment{ "%%" };
    NextOf dollar{ "$" };
    NextOf dollars{ "$$" };
    CodeSpans spans;
};

bool TagHider::Run() {
    size_t hash = markdown.find('#');
    if (hash == kNone) return false;
    const size_t frontMatterEnd = FrontMatterEnd(markdown);
    if (frontMatterEnd == markdown.size()) return false;
    size_t start = frontMatterEnd > 0 ? frontMatterEnd + 1 : 0;
    if (hash < start) hash = markdown.find('#', start);
    size_t percent = markdown.find("%%", start);
    Fence fence;
    bool displayMath = false;
    bool blank = true;      // the line before is blank
    bool indented = false;  // or indented code
    while (hash != kNone) {
        size_t end = markdown.find('\n', start);
        if (end == kNone) end = markdown.size();
        const std::string_view line = markdown.substr(start, end - start);
        // The line's first byte past its white space, and past its quote markers: only a line
        // whose lead is '`', '~' or '$' can open or close a fence or a display formula.
        size_t text = 0;
        while (text < line.size() && IsBlank(line[text])) text++;
        size_t first = text;
        while (first < line.size() && (IsBlank(line[first]) || line[first] == '>')) first++;
        const char lead = first < line.size() ? line[first] : '\0';
        const bool lineBlank = text == line.size();
        bool code = false;
        if (inComment) {
            if (percent < end) {
                inComment = false;
                Scan(start, end, percent - start + 2);
            }
        } else if (fence.marker != 0) {
            if (lead == fence.marker) {
                const Fence closing = FenceOf(line);
                if (closing.marker == fence.marker && closing.length >= fence.length && closing.bare) fence = {};
            }
        } else if (displayMath) {
            if (lineBlank || line.find("$$") != kNone) displayMath = false;
        } else if (const Fence opening = lead == '`' || lead == '~' ? FenceOf(line) : Fence(); opening.marker != 0) {
            fence = opening;
        } else if (lead == '$' && OpensDisplayMath(line)) {
            displayMath = true;
        } else {
            code = !lineBlank && (blank || indented) && Indentation(line) >= 4;
            if (!code && (hash < end || percent < end)) Scan(start, end, 0);
        }
        blank = lineBlank;
        indented = code;
        if (end == markdown.size()) break;
        start = end + 1;
        if (hash < start) hash = markdown.find('#', start);
        if (percent < start) percent = markdown.find("%%", start);
    }
    if (!changed) return false;
    out.append(markdown.substr(copied));
    return true;
}

// The line [lineStart, lineEnd) from `from` on: escapes, code spans, formulas, comments and wiki
// links skipped as Converter::Convert reads them, the tags in the text dropped.
void TagHider::Scan(size_t lineStart, size_t lineEnd, size_t from) {
    const std::string_view text = markdown.substr(lineStart, lineEnd - lineStart);
    closeBrackets.Reset(text);
    closeComment.Reset(text);
    dollar.Reset(text);
    dollars.Reset(text);
    spans.Reset();
    const size_t size = text.size();
    for (size_t i = FindAny<'#', '\\', '`', '$', '%', '['>(text.data(), from, size); i < size;
         i = FindAny<'#', '\\', '`', '$', '%', '['>(text.data(), i, size)) {
        const char c = text[i];
        const char next = i + 1 < size ? text[i + 1] : '\0';
        if (c == '#') {
            size_t end = i + 1;
            bool word = false;
            if (i == 0 || IsBlank(text[i - 1])) {
                for (; end < size && IsTagByte(static_cast<unsigned char>(text[end])); end++) {
                    word |= text[end] < '0' || text[end] > '9';
                }
            }
            if (word) {
                // The tag and the space after it, else the space before it.
                size_t dropFrom = lineStart + i;
                size_t dropTo = lineStart + end;
                if (end < size && text[end] == ' ') dropTo++;
                else if (i > 0 && text[i - 1] == ' ' && dropFrom - 1 >= copied) dropFrom--;
                if (!changed) {
                    changed = true;
                    out.clear();
                    out.reserve(markdown.size());
                }
                out.append(markdown.data() + copied, dropFrom - copied);
                copied = dropTo;
            }
            i = end;
        } else if (c == '\\') {
            i += next != '\0' ? 2 : 1;
        } else if (c == '`') {
            i = spans.End(text, i);
        } else if (c == '$') {
            size_t end = i + 1;
            if (next == '$') {
                const size_t close = dollars.From(i + 2);
                end = close == kNone ? i + 2 : close + 2;
            } else if (next != '\0' && !IsBlank(next)) {
                const size_t close = dollar.From(i + 1);
                if (close != kNone && !IsBlank(text[close - 1]) &&
                    (close + 1 == size || text[close + 1] < '0' || text[close + 1] > '9')) {
                    end = close + 1;
                }
            }
            i = end;
        } else if (c == '%' && next == '%') {
            const size_t close = closeComment.From(i + 2);
            if (close == kNone) {
                inComment = true;
                return;
            }
            i = close + 2;
        } else if (c == '[' && next == '[') {
            const size_t close = closeBrackets.From(i + 2);
            i = close == kNone ? i + 2 : close + 2;
        } else {
            i++;
        }
    }
}

} // namespace

bool HideTags(std::string_view markdown, std::string& out) {
    return TagHider(markdown, out).Run();
}

// RewriteObsidian with HiddenTags(): the tags first, then the pairs of what is left.
static RAYOMD_NOINLINE bool RewriteWithoutTags(std::string_view markdown, std::string& out) {
    std::string untagged;
    const bool hidden = HideTags(markdown, untagged);
    const std::string_view text = hidden ? std::string_view(untagged) : markdown;
    if (FindSyntaxPair(text, 0) == kNone) {
        if (hidden) out = std::move(untagged);
        return hidden;
    }
    out = ObsidianToCommonMark(text);
    return true;
}

bool RewriteObsidian(std::string_view markdown, std::string& out) {
    if (HiddenTags()) return RewriteWithoutTags(markdown, out);
    if (FindSyntaxPair(markdown, 0) == kNone) return false;
    out = ObsidianToCommonMark(markdown);
    return true;
}

std::string ObsidianToCommonMark(std::string_view markdown) {
    std::string out;
    out.reserve(markdown.size() + markdown.size() / 8);
    Converter converter(out);
    const size_t frontMatterEnd = FrontMatterEnd(markdown);
    if (frontMatterEnd == markdown.size()) return std::string(markdown);
    // Unchanged text is copied in runs from `copied` on; only a line with a pair outside code and
    // formulas, or in a comment, is converted. Lines are still read one by one up to the last pair,
    // for the fences and display formulas they open and close.
    size_t copied = 0;
    size_t start = frontMatterEnd > 0 ? frontMatterEnd + 1 : 0;
    size_t pair = FindSyntaxPair(markdown, start);
    Fence fence;
    bool displayMath = false;
    while (pair != kNone || converter.inComment) {
        size_t end = markdown.find('\n', start);
        if (end == kNone) end = markdown.size();
        const std::string_view line = markdown.substr(start, end - start);
        bool convert = false;
        if (converter.inComment) {
            convert = true;
        } else if (fence.marker != 0) {
            const Fence closing = FenceOf(line);
            if (closing.marker == fence.marker && closing.length >= fence.length && closing.bare) fence = {};
        } else if (displayMath) {
            if (line.find("$$") != kNone || Trim(line).empty()) displayMath = false;
        } else if (const Fence opening = FenceOf(line); opening.marker != 0) {
            fence = opening;
        } else if (OpensDisplayMath(line)) {
            displayMath = true;
        } else {
            convert = pair < end;
        }
        if (convert) {
            out.append(markdown.substr(copied, start - copied));
            // A block id is no text: Obsidian shows none.
            const size_t id = BlockIdStart(line);
            converter.Convert(id == kNone ? line : line.substr(0, id));
            copied = end;
        }
        if (end == markdown.size()) break;
        start = end + 1;
        if (pair != kNone && pair < start) pair = FindSyntaxPair(markdown, start);
    }
    out.append(markdown.substr(copied));
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
