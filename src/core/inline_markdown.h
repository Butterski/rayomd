#pragma once

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
};

// Source-size caps for math recognised inside inline text, so one stray '$' can
// never turn a whole paragraph into a single unbreakable box. A complete candidate
// with a longer body is not typeset: it becomes one code span that holds its
// source, delimiters included, byte for byte.
constexpr size_t kMaxInlineMathBytes = 1024;
constexpr size_t kMaxDisplayMathBytes = 8192;

struct InlineSpan {
    std::string text;   // math spans: the TeX source without its delimiters
    std::string url;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
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
    InlineMath math = InlineMath::None;
};

struct InlineRuns {
    std::string text;
    std::string urls;
    std::vector<InlineRun> runs;

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

std::string NormalizeReferenceLabel(std::string_view label);
std::string ResolveReferenceLinks(std::string_view input, const ReferenceDefinitions& definitions);
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

// The character reference ("&amp;", "&#169;", "&#xA9;") at text[at]: its length in bytes, with
// its code point, or 0 when none starts there. The named references are those of HTML 4 and
// "&apos;", "&check;" and "&cross;"; a number that names no character gives U+FFFD.
size_t MatchCharacterReference(std::string_view text, size_t at, uint32_t& codePoint);

// What the character references of a document need of the renderer: none or only ASCII, the
// standard fonts' WinAnsiEncoding, or Unicode text.
enum class ReferenceNeed : unsigned char { Ascii, WinAnsi, Unicode };
ReferenceNeed CharacterReferenceNeed(std::string_view text);

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

} // namespace TinyPdf::Internal
