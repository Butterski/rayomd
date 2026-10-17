// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../common/text_utils.h"
#include "shared_url.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace TinyPdf::Internal {

enum class InlineMath : unsigned char {
    None,
    Inline,   // $...$ or \( ... \)
    Display,  // $$...$$ inside paragraph text
    Note,     // a footnote reference, "[^label]", whose text is the label: one box, like a formula
    Sup,      // <sup>: its text, plain, raised and smaller in a box of its own, like a note
    Sub,      // <sub>: the same, lowered
};

// <sup> or <sub>: text in a box, which readers of formulas and notes take as text.
constexpr bool IsScript(InlineMath math) {
    return math == InlineMath::Sup || math == InlineMath::Sub;
}

// InlineRun::decor and InlineSpan::decor bits: HTML's <u> and <ins>, and <mark>.
constexpr uint8_t kDecorUnderline = 1;
constexpr uint8_t kDecorMark = 2;

// Source-size caps for math recognised inside inline text, so one stray '$' can
// never turn a whole paragraph into a single unbreakable box. A complete candidate
// with a longer body is not typeset: it becomes one code span that holds its
// source, delimiters included, byte for byte.
constexpr size_t kMaxInlineMathBytes = 1024;
constexpr size_t kMaxDisplayMathBytes = 8192;

struct InlineSpan {
    std::string text;   // math spans: the TeX source without its delimiters
    SharedUrl url;      // one string for all the spans of a link
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    bool lineBreak = false;
    uint8_t decor = 0;  // kDecor* bits
    InlineMath math = InlineMath::None;
};

// The spans of one piece of inline text in flat form: the text of every run lies in one
// buffer and the link targets in another. A caller that parses many paragraphs reuses
// one object, so parsing builds no string per span.
struct InlineRun {
    size_t begin = 0;       // InlineRuns::text[begin, end)
    size_t end = 0;
    size_t urlBegin = 0;    // InlineRuns::urls[urlBegin, urlEnd); empty when the run is no link
    size_t urlEnd = 0;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    bool lineBreak = false; // <br>: one space, at which the wrappers end the line
    uint8_t decor = 0;      // kDecor* bits
    InlineMath math = InlineMath::None;
};

struct InlineRuns {
    std::string text;
    std::string urls;
    std::vector<InlineRun> runs;
    bool scripts = false;   // a run is InlineMath::Sup or Sub, which the paragraph fast paths do not lay out

    std::string_view Text(const InlineRun& run) const {
        return std::string_view(text.data() + run.begin, run.end - run.begin);
    }
    std::string_view Url(const InlineRun& run) const {
        return std::string_view(urls.data() + run.urlBegin, run.urlEnd - run.urlBegin);
    }
};

struct ReferenceDefinition {
    std::string destination;
    std::string title;
};

using ReferenceDefinitions = std::unordered_map<std::string, ReferenceDefinition>;

// The longest destination a link reference definition may have; a line with a longer one is no
// definition but text. Every use of a reference copies its destination.
constexpr size_t kMaxReferenceDestinationBytes = 8192;

// What the parse of one document may add to its text, shared by all its levels and footnotes so
// that nesting cannot multiply it: the destination bytes resolved reference links copy in, at most
// twice the document's size and a mebibyte (a reference past that stays text), and the empty
// cells that complete pipe-table rows shorter than their header and HTML table rows shorter than
// their table's longest, at most 2^19 as in cmark-gfm (a pipe-table row past that ends its table,
// an HTML table past it is read as text). ParseMarkdown keeps one for its thread while it runs;
// text parsed outside one gets those budgets for itself.
class ParseBudget {
public:
    static constexpr size_t kReferenceSlackBytes = size_t(1) << 20;
    static constexpr size_t kPaddedCells = size_t(1) << 19;

    explicit ParseBudget(size_t sourceBytes);
    ~ParseBudget();
    ParseBudget(const ParseBudget&) = delete;
    ParseBudget& operator=(const ParseBudget&) = delete;
    // The budget of the document being parsed on this thread, or null.
    static ParseBudget* Current();

    size_t referenceBytes;      // destination bytes reference links may still copy in
    size_t paddedCells = kPaddedCells;  // empty cells short table rows may still get

private:
    ParseBudget* previous;
};

std::string NormalizeReferenceLabel(std::string_view label);
// "[text][label]", "[label][]" and "[label]" (also as images) with a defined label as the inline
// links "[text](<destination>)", within the ParseBudget.
std::string ResolveReferenceLinks(std::string_view input, const ReferenceDefinitions& definitions);
// The status symbols U+2705, U+26A0 (with or without the variation selector U+FE0F) and U+274C
// as "[OK]", "[!]" and "[X]", which the standard fonts can show; in one pass.
std::string NormalizeSymbols(std::string text);
// recognizeMath == false keeps every '$' and "\(" literal. It exists for text that has
// already been through the inline parser once and is only being re-read as plain text.
//
// lookaheadBudget is the number of bytes the parser may read while it looks ahead for
// closing delimiters before it answers those lookups from tables instead. The default
// derives it from the input size, so only text with very many unterminated openers gets
// there. The result does not depend on it; tests pass 0 to compare the two ways.
constexpr size_t kDefaultLookaheadBudget = static_cast<size_t>(-1);
std::vector<InlineSpan> ParseInlineSpans(std::string_view input, bool recognizeMath = true,
    size_t lookaheadBudget = kDefaultLookaheadBudget);
// The same parse into `out`, which is cleared first and must not hold `input`. The runs,
// read in order, cover out.text without a gap.
void ParseInlineRuns(std::string_view input, InlineRuns& out, bool recognizeMath = true,
    size_t lookaheadBudget = kDefaultLookaheadBudget);
// StripInlineMarkdown's text (markdown_parser.h) without a copy: a view of `input`, or of
// `scratch.text`.
std::string_view StripInlineMarkdownView(std::string_view input, InlineRuns& scratch, bool recognizeMath);

// Whether `text` needs the inline parser: inline syntax (or a byte of `classes`), or the "://"
// or '@' of a bare URL or email address. One pass, which stops at the first inline syntax; only
// text with a ':' or '@' and no syntax is searched again. The classes seen are summed before the
// test: fewer instructions than testing each block first, mostly in short table cells. When it
// returns false, `seenOut` (if given) gets the classes of all of `text`.
inline bool NeedsInlineParse(std::string_view text, unsigned char classes = 0, unsigned char* seenOut = nullptr) {
    const unsigned syntax = RayoMd::Text::kByteInlineSyntax | classes;
    const unsigned char* at = reinterpret_cast<const unsigned char*>(text.data());
    size_t left = text.size();
    const auto& byteClasses = RayoMd::Text::Detail::kByteClasses;
    unsigned char seen = 0;
    for (; left >= 8; at += 8, left -= 8) {
        seen |= byteClasses[at[0]] | byteClasses[at[1]] | byteClasses[at[2]] | byteClasses[at[3]] |
            byteClasses[at[4]] | byteClasses[at[5]] | byteClasses[at[6]] | byteClasses[at[7]];
        if (seen & syntax) return true;
    }
    for (; left > 0; at++, left--) seen |= byteClasses[*at];
    if (seen & syntax) return true;
    if (seenOut != nullptr) *seenOut = seen;
    return (seen & RayoMd::Text::kByteAutolinkLead) &&
        (text.find("://") != std::string_view::npos || text.find('@') != std::string_view::npos);
}

// The character reference ("&amp;", "&#169;", "&#xA9;") at text[at]: its length in bytes, with
// its code point, or 0 when none starts there. The named references are those of HTML 4 and
// "&apos;", "&check;" and "&cross;"; a number that names no character gives U+FFFD.
size_t MatchCharacterReference(std::string_view text, size_t at, uint32_t& codePoint);

// What the character references of a document need of the renderer: none or only ASCII, the
// standard fonts' WinAnsiEncoding, or Unicode text.
enum class ReferenceNeed : unsigned char { Ascii, WinAnsi, Unicode };
ReferenceNeed CharacterReferenceNeed(std::string_view text);

// The code points of the BMP from U+0080 on that `text` has, as UTF-8 or as character
// references (U+FFFD for a byte that is not UTF-8), ascending, each once.
using CidList = std::vector<uint16_t>;
void NonAsciiCodepoints(std::string_view text, CidList& out);

// While one lives, the inline parser on this thread writes character references as WinAnsi
// codes, the bytes of the standard renderer's text, instead of UTF-8.
class WinAnsiReferences {
public:
    WinAnsiReferences();
    ~WinAnsiReferences();
    WinAnsiReferences(const WinAnsiReferences&) = delete;
    WinAnsiReferences& operator=(const WinAnsiReferences&) = delete;

private:
    bool previous;
};

// While one lives, the inline parser on this thread reads "[^label]" as a footnote reference
// (InlineMath::Note) when `numbers` has the label, normalized as a link label is. "[^1](url)"
// stays a link.
class FootnoteLabels {
public:
    explicit FootnoteLabels(const std::unordered_map<std::string, int>& numbers);
    ~FootnoteLabels();
    FootnoteLabels(const FootnoteLabels&) = delete;
    FootnoteLabels& operator=(const FootnoteLabels&) = delete;

private:
    const std::unordered_map<std::string, int>* previous;
    size_t previousBytes;
};

} // namespace TinyPdf::Internal
