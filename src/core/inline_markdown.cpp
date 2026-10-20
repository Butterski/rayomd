// SPDX-License-Identifier: Apache-2.0

#include "inline_markdown.h"
#include "html.h"
#include "../common/text_utils.h"
#include "../common/compiler.h"
#include "../common/thread_object.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>

// The stop scan in AVX2 where GCC or Clang can compile it for one function: the build stays
// baseline x86-64 and the CPU is asked once, as word_bounds.cpp does.
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <atomic>
#include <immintrin.h>
#include "../common/cpu_features.h"
#define RAYOMD_INLINE_AVX2 1
#endif

namespace TinyPdf::Internal {
namespace {

bool IsSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string_view TrimView(std::string_view value) {
    while (!value.empty() && IsSpace(value.front())) value.remove_prefix(1);
    while (!value.empty() && IsSpace(value.back())) value.remove_suffix(1);
    return value;
}

// Whether the inline parser on this thread writes character references as WinAnsi codes.
thread_local bool tWinAnsiReferences = false;

// The footnote labels the inline parser on this thread reads "[^label]" with, or null, and the
// most bytes a reference to one of them can take between "[^" and "]": a label of the table
// can grow or shrink when it is normalized, but not to less than a third of its bytes.
thread_local const std::unordered_map<std::string, int>* tFootnoteLabels = nullptr;
thread_local size_t tFootnoteLabelBytes = 0;

// The budget of the document ParseMarkdown is parsing on this thread, or null.
thread_local ParseBudget* tParseBudget = nullptr;

// The longest name is "thetasym": names in place, without a pointer to relocate each.
struct NamedReference {
    char name[9];
    uint16_t codePoint;
};

// Sorted by name, byte for byte.
constexpr NamedReference kNamedReferences[] = {
    { "AElig", 198 }, { "Aacute", 193 }, { "Acirc", 194 }, { "Agrave", 192 }, { "Alpha", 913 },
    { "Aring", 197 }, { "Atilde", 195 }, { "Auml", 196 }, { "Beta", 914 }, { "Ccedil", 199 },
    { "Chi", 935 }, { "Dagger", 8225 }, { "Delta", 916 }, { "ETH", 208 }, { "Eacute", 201 },
    { "Ecirc", 202 }, { "Egrave", 200 }, { "Epsilon", 917 }, { "Eta", 919 }, { "Euml", 203 },
    { "Gamma", 915 }, { "Iacute", 205 }, { "Icirc", 206 }, { "Igrave", 204 }, { "Iota", 921 },
    { "Iuml", 207 }, { "Kappa", 922 }, { "Lambda", 923 }, { "Mu", 924 }, { "Ntilde", 209 },
    { "Nu", 925 }, { "OElig", 338 }, { "Oacute", 211 }, { "Ocirc", 212 }, { "Ograve", 210 },
    { "Omega", 937 }, { "Omicron", 927 }, { "Oslash", 216 }, { "Otilde", 213 }, { "Ouml", 214 },
    { "Phi", 934 }, { "Pi", 928 }, { "Prime", 8243 }, { "Psi", 936 }, { "Rho", 929 },
    { "Scaron", 352 }, { "Sigma", 931 }, { "THORN", 222 }, { "Tau", 932 }, { "Theta", 920 },
    { "Uacute", 218 }, { "Ucirc", 219 }, { "Ugrave", 217 }, { "Upsilon", 933 }, { "Uuml", 220 },
    { "Xi", 926 }, { "Yacute", 221 }, { "Yuml", 376 }, { "Zeta", 918 }, { "aacute", 225 },
    { "acirc", 226 }, { "acute", 180 }, { "aelig", 230 }, { "agrave", 224 }, { "alefsym", 8501 },
    { "alpha", 945 }, { "amp", 38 }, { "and", 8743 }, { "ang", 8736 }, { "apos", 39 },
    { "aring", 229 }, { "asymp", 8776 }, { "atilde", 227 }, { "auml", 228 }, { "bdquo", 8222 },
    { "beta", 946 }, { "brvbar", 166 }, { "bull", 8226 }, { "cap", 8745 }, { "ccedil", 231 },
    { "cedil", 184 }, { "cent", 162 }, { "check", 10003 }, { "chi", 967 }, { "circ", 710 },
    { "clubs", 9827 }, { "cong", 8773 }, { "copy", 169 }, { "crarr", 8629 }, { "cross", 10007 },
    { "cup", 8746 }, { "curren", 164 }, { "dArr", 8659 }, { "dagger", 8224 }, { "darr", 8595 },
    { "deg", 176 }, { "delta", 948 }, { "diams", 9830 }, { "divide", 247 }, { "eacute", 233 },
    { "ecirc", 234 }, { "egrave", 232 }, { "empty", 8709 }, { "emsp", 8195 }, { "ensp", 8194 },
    { "epsilon", 949 }, { "equiv", 8801 }, { "eta", 951 }, { "eth", 240 }, { "euml", 235 },
    { "euro", 8364 }, { "exist", 8707 }, { "fnof", 402 }, { "forall", 8704 }, { "frac12", 189 },
    { "frac14", 188 }, { "frac34", 190 }, { "frasl", 8260 }, { "gamma", 947 }, { "ge", 8805 },
    { "gt", 62 }, { "hArr", 8660 }, { "harr", 8596 }, { "hearts", 9829 }, { "hellip", 8230 },
    { "iacute", 237 }, { "icirc", 238 }, { "iexcl", 161 }, { "igrave", 236 }, { "image", 8465 },
    { "infin", 8734 }, { "int", 8747 }, { "iota", 953 }, { "iquest", 191 }, { "isin", 8712 },
    { "iuml", 239 }, { "kappa", 954 }, { "lArr", 8656 }, { "lambda", 955 }, { "lang", 10216 },
    { "laquo", 171 }, { "larr", 8592 }, { "lceil", 8968 }, { "ldquo", 8220 }, { "le", 8804 },
    { "lfloor", 8970 }, { "lowast", 8727 }, { "loz", 9674 }, { "lrm", 8206 }, { "lsaquo", 8249 },
    { "lsquo", 8216 }, { "lt", 60 }, { "macr", 175 }, { "mdash", 8212 }, { "micro", 181 },
    { "middot", 183 }, { "minus", 8722 }, { "mu", 956 }, { "nabla", 8711 }, { "nbsp", 160 },
    { "ndash", 8211 }, { "ne", 8800 }, { "ni", 8715 }, { "not", 172 }, { "notin", 8713 },
    { "nsub", 8836 }, { "ntilde", 241 }, { "nu", 957 }, { "oacute", 243 }, { "ocirc", 244 },
    { "oelig", 339 }, { "ograve", 242 }, { "oline", 8254 }, { "omega", 969 }, { "omicron", 959 },
    { "oplus", 8853 }, { "or", 8744 }, { "ordf", 170 }, { "ordm", 186 }, { "oslash", 248 },
    { "otilde", 245 }, { "otimes", 8855 }, { "ouml", 246 }, { "para", 182 }, { "part", 8706 },
    { "permil", 8240 }, { "perp", 8869 }, { "phi", 966 }, { "pi", 960 }, { "piv", 982 },
    { "plusmn", 177 }, { "pound", 163 }, { "prime", 8242 }, { "prod", 8719 }, { "prop", 8733 },
    { "psi", 968 }, { "quot", 34 }, { "rArr", 8658 }, { "radic", 8730 }, { "rang", 10217 },
    { "raquo", 187 }, { "rarr", 8594 }, { "rceil", 8969 }, { "rdquo", 8221 }, { "real", 8476 },
    { "reg", 174 }, { "rfloor", 8971 }, { "rho", 961 }, { "rlm", 8207 }, { "rsaquo", 8250 },
    { "rsquo", 8217 }, { "sbquo", 8218 }, { "scaron", 353 }, { "sdot", 8901 }, { "sect", 167 },
    { "shy", 173 }, { "sigma", 963 }, { "sigmaf", 962 }, { "sim", 8764 }, { "spades", 9824 },
    { "sub", 8834 }, { "sube", 8838 }, { "sum", 8721 }, { "sup", 8835 }, { "sup1", 185 },
    { "sup2", 178 }, { "sup3", 179 }, { "supe", 8839 }, { "szlig", 223 }, { "tau", 964 },
    { "there4", 8756 }, { "theta", 952 }, { "thetasym", 977 }, { "thinsp", 8201 }, { "thorn", 254 },
    { "tilde", 732 }, { "times", 215 }, { "trade", 8482 }, { "uArr", 8657 }, { "uacute", 250 },
    { "uarr", 8593 }, { "ucirc", 251 }, { "ugrave", 249 }, { "uml", 168 }, { "upsih", 978 },
    { "upsilon", 965 }, { "uuml", 252 }, { "weierp", 8472 }, { "xi", 958 }, { "yacute", 253 },
    { "yen", 165 }, { "yuml", 255 }, { "zeta", 950 }, { "zwj", 8205 }, { "zwnj", 8204 },
};

void AppendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// The soft hyphen and the invisible joiners and direction marks: a reference to one is
// dropped, as the standard renderer's text drops the characters themselves.
bool IsDroppedReference(uint32_t codePoint) {
    return codePoint == 0xAD || (codePoint >= 0x200C && codePoint <= 0x200F);
}

// The en, em and thin spaces, which WinAnsi text writes as a plain space.
bool IsSpaceReference(uint32_t codePoint) {
    return codePoint == 0x2002 || codePoint == 0x2003 || codePoint == 0x2009;
}

// Appends the character a reference stands for in the encoding of the text being parsed.
// CharacterReferenceNeed sends a document with one WinAnsi lacks to the Unicode renderer, so
// the '?' only guards.
RAYOMD_NOINLINE void AppendReference(std::string& out, uint32_t codePoint) {
    if (IsDroppedReference(codePoint)) return;
    if (!tWinAnsiReferences) {
        AppendUtf8(out, codePoint);
        return;
    }
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
        return;
    }
    const int code = RayoMd::Text::WinAnsiCode(codePoint);
    out.push_back(code >= 0 ? static_cast<char>(code) : IsSpaceReference(codePoint) ? ' ' : '?');
}

// Appends `value` with its character references decoded: the text up to each '&' in one append.
void AppendDecoded(std::string& out, std::string_view value) {
    for (size_t i = 0; i < value.size();) {
        const void* found = memchr(value.data() + i, '&', value.size() - i);
        const size_t amp = found != nullptr ? static_cast<size_t>(static_cast<const char*>(found) - value.data()) : value.size();
        out.append(value.data() + i, amp - i);
        if (amp == value.size()) return;
        uint32_t codePoint = 0;
        const size_t length = MatchCharacterReference(value, amp, codePoint);
        if (length == 0) {
            out.push_back('&');
            i = amp + 1;
            continue;
        }
        AppendReference(out, codePoint);
        i = amp + length;
    }
}

// <br>, <br/> or <br /> in any case; `tag` is the text between the angle brackets.
bool IsBreakTag(std::string_view tag) {
    if (tag.size() < 2 || (tag[0] | 0x20) != 'b' || (tag[1] | 0x20) != 'r') return false;
    size_t at = 2;
    while (at < tag.size() && (tag[at] == ' ' || tag[at] == '\t')) at++;
    if (at < tag.size() && tag[at] == '/') at++;
    while (at < tag.size() && (tag[at] == ' ' || tag[at] == '\t')) at++;
    return at == tag.size();
}

bool IsAsciiAlphanumeric(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

// Whether the ':' at `colon`, which a '/' follows, starts the "://" of a bare URL: "http" or
// "https" in front of it, at the start of the text or after white space or one of * _ ~ (, and
// a letter or digit behind it. `start` is where the scheme starts. Constant time: most ':' and
// "://" in text start no link, and none may cost a scan.
bool MatchUrlScheme(std::string_view source, size_t colon, size_t& start) {
    if (colon + 3 >= source.size() || source[colon + 2] != '/' || !IsAsciiAlphanumeric(source[colon + 3])) return false;
    const auto schemeIs = [&](const char* scheme, size_t length) {
        if (colon < length) return false;
        for (size_t k = 0; k < length; k++) {
            if ((source[colon - length + k] | 0x20) != scheme[k]) return false;
        }
        return true;
    };
    const size_t schemeLength = schemeIs("https", 5) ? 5 : schemeIs("http", 4) ? 4 : 0;
    if (schemeLength == 0) return false;
    start = colon - schemeLength;
    if (start > 0) {
        const char before = source[start - 1];
        if (!IsSpace(before) && before != '*' && before != '_' && before != '~' && before != '(') return false;
    }
    return true;
}

// The end of the bare URL whose domain starts with the letter or digit at `domain`. The URL runs
// to white space or '<', less the punctuation that ends a sentence, a ')' that closes no '(' of
// its own and a character reference at its end, as on GitHub. Only a URL that becomes a link is
// scanned; what it leaves behind holds no '/', so no "://", and is never scanned again.
size_t UrlAutolinkEnd(std::string_view source, size_t domain) {
    size_t at = domain + 1;
    while (at < source.size() && !IsSpace(source[at]) && source[at] != '<' && (unsigned char)source[at] >= 0x20) at++;
    size_t opens = 0;
    size_t closes = 0;
    for (size_t k = domain; k < at; k++) {
        opens += source[k] == '(';
        closes += source[k] == ')';
    }
    size_t end = at;
    while (end > domain) {
        const char last = source[end - 1];
        if (last == '?' || last == '!' || last == '.' || last == ',' || last == ':' || last == '*' || last == '_' ||
            last == '~' || last == '\'' || last == '"') {
            end--;
        } else if (last == ')' && closes > opens) {
            closes--;
            end--;
        } else if (last == ';') {
            size_t name = end - 1;
            while (name > domain && IsAsciiAlphanumeric(source[name - 1])) name--;
            if (name == end - 1 || name <= domain || source[name - 1] != '&') break;
            end = name - 1;
        } else {
            break;
        }
    }
    return end;
}

// A bare email address around the '@' at `at`: letters, digits and . + - _ in front of it, and
// behind it a domain of two or more parts of letters, digits, - and _ that does not end in - or
// _. [start, end) is the address, without a '.' that ends a sentence.
bool MatchEmailAutolink(std::string_view source, size_t at, size_t& start, size_t& end) {
    start = at;
    while (start > 0) {
        const char ch = source[start - 1];
        if (!IsAsciiAlphanumeric(ch) && ch != '.' && ch != '+' && ch != '-' && ch != '_') break;
        start--;
    }
    if (start == at) return false;
    const auto domainByte = [](char ch) { return IsAsciiAlphanumeric(ch) || ch == '-' || ch == '_'; };
    size_t parts = 0;
    for (size_t i = at + 1; i < source.size();) {
        const size_t part = i;
        while (i < source.size() && domainByte(source[i])) i++;
        if (i == part) break;
        parts++;
        end = i;
        if (i + 1 >= source.size() || source[i] != '.' || !domainByte(source[i + 1])) break;
        i++;
    }
    return parts >= 2 && source[end - 1] != '-' && source[end - 1] != '_';
}

struct InlineLink {
    std::string_view label;
    std::string_view target;
    size_t end = std::string_view::npos;
};

size_t FindDestinationEnd(std::string_view source, size_t openParen) {
    if (openParen >= source.size() || source[openParen] != '(') return std::string_view::npos;
    int depth = 0;
    bool escaped = false;
    bool inAngleDestination = false;
    for (size_t i = openParen + 1; i < source.size(); i++) {
        char ch = source[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (inAngleDestination) {
            if (ch == '>') inAngleDestination = false;
            continue;
        }
        if (ch == '<') {
            inAngleDestination = true;
            continue;
        }
        if (ch == '(') {
            depth++;
            continue;
        }
        if (ch == ')') {
            if (depth == 0) return i;
            depth--;
        }
    }
    return std::string_view::npos;
}

// Appends the destination of a link target to `urls`, unescaped and with its character
// references decoded: the text between angle brackets, or else the text up to the first
// space. Appends nothing when there is none, or when Html::SafeUrl refuses it (javascript:,
// file:, ...): the link's text then links nowhere.
RAYOMD_NOINLINE void AppendDestination(std::string_view target, std::string& urls) {
    const size_t begin = urls.size();
    target = TrimView(target);
    if (target.empty()) return;
    std::string_view value;
    if (target.front() == '<') {
        size_t end = target.find('>');
        if (end == std::string_view::npos || end == 1) return;
        value = target.substr(1, end - 1);
    } else {
        size_t end = 0;
        while (end < target.size() && !IsSpace(target[end])) end++;
        value = target.substr(0, end);
    }
    value = TrimView(value);
    for (size_t i = 0; i < value.size();) {
        // The bytes before the next backslash or '&' in one append.
        size_t plain = i;
        while (plain < value.size() && value[plain] != '\\' && value[plain] != '&') plain++;
        urls.append(value.data() + i, plain - i);
        i = plain;
        if (i == value.size()) break;
        uint32_t codePoint = 0;
        size_t length = 0;
        if (value[i] == '\\' && i + 1 < value.size() && std::ispunct((unsigned char)value[i + 1])) {
            urls.push_back(value[i + 1]);
            i += 2;
        } else if (value[i] == '&' && (length = MatchCharacterReference(value, i, codePoint)) != 0) {
            AppendReference(urls, codePoint);
            i += length;
        } else {
            urls.push_back(value[i]);
            i++;
        }
    }
    if (!Html::SafeUrl(std::string_view(urls).substr(begin))) urls.resize(begin);
}

// What the HTML tags of a paragraph have opened so far: counters of the open elements by effect,
// and the link of an open <a href>, out.urls[urlBegin, urlEnd). Counters end with the paragraph.
struct HtmlInline {
    bool active = false;    // a tag was met: the runs from here on take this state into account
    uint16_t bold = 0;
    uint16_t italic = 0;
    uint16_t strike = 0;
    uint16_t code = 0;
    uint16_t underline = 0;
    uint16_t mark = 0;
    size_t urlBegin = 0;
    size_t urlEnd = 0;

    uint8_t Decor() const {
        return static_cast<uint8_t>((underline != 0 ? kDecorUnderline : 0) | (mark != 0 ? kDecorMark : 0));
    }
};

// The output side of the parser. Text is appended to the buffer of InlineRuns as it is
// read and a run is a range of that buffer, so text that continues in the same style
// joins the run before it by moving one end.
class RunWriter {
public:
    explicit RunWriter(InlineRuns& target) : out(target) {
        out.text.clear();
        out.urls.clear();
        out.runs.clear();
        out.scripts = false;
    }

    // Ends the plain text appended since the last run: one run in the given emphasis.
    void Flush(bool bold, bool italic, bool strike) {
        if (html.active) {
            FlushHtml(bold, italic, strike);
            return;
        }
        const size_t end = out.text.size();
        if (end == pending) return;
        if (!out.runs.empty()) {
            InlineRun& last = out.runs.back();
            if (last.bold == bold && last.italic == italic && last.strike == strike && last.urlBegin == last.urlEnd &&
                !last.code && !last.lineBreak && last.math == InlineMath::None) {
                last.end = end;
                pending = end;
                return;
            }
        }
        InlineRun& run = Add(end);
        run.bold = bold;
        run.italic = italic;
        run.strike = strike;
    }

    // Turns the text appended since the last run into a run of its own: a link label, an
    // autolink or a code span. Its link target is out.urls from `urlBegin` on. Empty text
    // adds nothing, and a run that repeats the style and target of the one before joins it.
    RAYOMD_HOT_INLINE void Close(bool bold, bool italic, bool strike, size_t urlBegin, bool code) {
        if (html.active) {
            CloseHtml(bold, italic, strike, urlBegin, code);
            return;
        }
        const size_t end = out.text.size();
        if (end != pending && !out.runs.empty()) {
            InlineRun& last = out.runs.back();
            if (last.bold == bold && last.italic == italic && last.strike == strike && last.code == code &&
                !last.lineBreak && last.math == InlineMath::None &&
                out.Url(last) == std::string_view(out.urls).substr(urlBegin)) {
                last.end = end;
                pending = end;
                out.urls.resize(urlBegin);
                return;
            }
        }
        if (end == pending) {
            out.urls.resize(urlBegin);
            return;
        }
        InlineRun& run = Add(end);
        run.urlBegin = urlBegin;
        run.urlEnd = out.urls.size();
        run.bold = bold;
        run.italic = italic;
        run.strike = strike;
        run.code = code;
    }

    // A formula is always its own run: it never joins neighbouring text or another
    // formula, and it carries no emphasis, link, or code state.
    void Math(bool display) {
        Add(out.text.size()).math = display ? InlineMath::Display : InlineMath::Inline;
    }

    // A footnote reference: its label as the text of a run of its own, like a formula.
    void Note() {
        Add(out.text.size()).math = InlineMath::Note;
    }

    // <sup> or <sub>: the text appended since the last run as a script of its own, like a note.
    void Script(bool sub) {
        Add(out.text.size()).math = sub ? InlineMath::Sub : InlineMath::Sup;
        out.scripts = true;
    }

    // A <br>: one space in a run of its own, at which the wrappers end the line and which
    // other readers take as the space it is. Text after it never joins it.
    void Break() {
        out.text.push_back(' ');
        Add(out.text.size()).lineBreak = true;
    }

    // Appends link text that was parsed on its own: each of its runs links to
    // out.urls[urlBegin, end) and adds its own emphasis to that around the link.
    void LinkText(const InlineRuns& label, size_t urlBegin, bool bold, bool italic, bool strike) {
        const size_t urlEnd = out.urls.size();
        if (label.runs.empty()) {
            out.urls.resize(urlBegin);
            return;
        }
        uint8_t decor = 0;
        bool code = false;
        if (html.active) {
            bold = bold || html.bold != 0;
            italic = italic || html.italic != 0;
            strike = strike || html.strike != 0;
            code = html.code != 0;
            decor = html.Decor();
        }
        for (const InlineRun& part : label.runs) {
            out.text.append(label.text, part.begin, part.end - part.begin);
            InlineRun& run = Add(out.text.size());
            run.urlBegin = urlBegin;
            run.urlEnd = urlEnd;
            run.bold = bold || part.bold;
            run.italic = italic || part.italic;
            run.strike = strike || part.strike;
            run.code = code || part.code;
            run.decor = static_cast<uint8_t>(decor | part.decor);
        }
    }

    HtmlInline html;

private:
    // Flush in the state of the paragraph's open HTML elements, which add to the emphasis and
    // may give the text code style, decoration and a link.
    RAYOMD_NOINLINE void FlushHtml(bool bold, bool italic, bool strike) {
        const size_t end = out.text.size();
        if (end == pending) return;
        StyledRun(end, bold || html.bold != 0, italic || html.italic != 0, strike || html.strike != 0, html.code != 0,
            html.Decor(), html.urlBegin, html.urlEnd);
    }

    // Close in that state. A code span inside an <a href> takes the element's link.
    RAYOMD_NOINLINE void CloseHtml(bool bold, bool italic, bool strike, size_t urlBegin, bool code) {
        const size_t end = out.text.size();
        if (!code) {
            bold = bold || html.bold != 0;
            italic = italic || html.italic != 0;
            strike = strike || html.strike != 0;
        }
        code = code || html.code != 0;
        const uint8_t decor = html.Decor();
        if (urlBegin == out.urls.size() && html.urlBegin != html.urlEnd) {
            if (end != pending) StyledRun(end, bold, italic, strike, code, decor, html.urlBegin, html.urlEnd);
            return;
        }
        if (end != pending && !out.runs.empty()) {
            InlineRun& last = out.runs.back();
            if (last.bold == bold && last.italic == italic && last.strike == strike && last.code == code &&
                last.decor == decor && !last.lineBreak && last.math == InlineMath::None &&
                out.Url(last) == std::string_view(out.urls).substr(urlBegin)) {
                last.end = end;
                pending = end;
                out.urls.resize(urlBegin);
                return;
            }
        }
        if (end == pending) {
            out.urls.resize(urlBegin);
            return;
        }
        InlineRun& run = Add(end);
        run.urlBegin = urlBegin;
        run.urlEnd = out.urls.size();
        run.bold = bold;
        run.italic = italic;
        run.strike = strike;
        run.code = code;
        run.decor = decor;
    }

    // The text [pending, end) as a run in this state, linking to out.urls[urlBegin, urlEnd); it
    // joins the run before when that has the same state, as Flush joins plain text.
    void StyledRun(size_t end, bool bold, bool italic, bool strike, bool code, uint8_t decor, size_t urlBegin,
        size_t urlEnd) {
        if (!out.runs.empty()) {
            InlineRun& last = out.runs.back();
            const bool sameUrl = urlBegin == urlEnd ? last.urlBegin == last.urlEnd
                : last.urlBegin == urlBegin && last.urlEnd == urlEnd;
            if (last.bold == bold && last.italic == italic && last.strike == strike && last.code == code &&
                last.decor == decor && sameUrl && !last.lineBreak && last.math == InlineMath::None) {
                last.end = end;
                pending = end;
                return;
            }
        }
        InlineRun& run = Add(end);
        run.bold = bold;
        run.italic = italic;
        run.strike = strike;
        run.code = code;
        run.decor = decor;
        run.urlBegin = urlBegin;
        run.urlEnd = urlEnd;
    }


private:
    InlineRun& Add(size_t end) {
        out.runs.emplace_back();
        InlineRun& run = out.runs.back();
        run.begin = pending;
        run.end = end;
        pending = end;
        return run;
    }

public:
    // Start of the text that belongs to no run yet.
    size_t Pending() const { return pending; }

private:
    InlineRuns& out;
    size_t pending = 0;     // start of the text that belongs to no run yet
};

// The bare URL or email address whose ':' or '@' is at source[at], as a link of its own: the
// index behind it, or 0 when none is there. The scheme, or the name in front of the '@', is the
// plain text written last and moves into the link. Out of the parse loop, which most ':' and
// '@' leave at its first test.
RAYOMD_NOINLINE size_t WriteAutolink(RunWriter& writer, InlineRuns& out, std::string_view source, size_t at,
    bool bold, bool italic, bool strike) {
    const bool email = source[at] == '@';
    size_t start = at;
    size_t end = 0;
    if (!(email ? MatchEmailAutolink(source, at, start, end) : MatchUrlScheme(source, at, start))) return 0;
    const size_t lead = at - start;
    std::string& text = out.text;
    if (text.size() < writer.Pending() + lead || text.compare(text.size() - lead, lead, source.data() + start, lead) != 0) {
        return 0;
    }
    if (!email) end = UrlAutolinkEnd(source, at + 3);
    text.resize(text.size() - lead);
    writer.Flush(bold, italic, strike);
    const size_t urlBegin = out.urls.size();
    if (email) out.urls += "mailto:";
    AppendDecoded(out.urls, source.substr(start, end - start));
    AppendDecoded(text, source.substr(start, end - start));
    writer.Close(bold, italic, strike, urlBegin, false);
    return end;
}

bool IsClassicEscapable(char ch) {
    constexpr std::string_view punctuation = R"(\`*{}[]()#+-.!_)";
    return punctuation.find(ch) != std::string_view::npos;
}

size_t DelimiterRun(std::string_view source, size_t start, char delimiter) {
    size_t end = start;
    while (end < source.size() && source[end] == delimiter) end++;
    return end - start;
}

size_t FindExactDelimiterRun(std::string_view source, size_t start, char delimiter, size_t length) {
    size_t position = start;
    while (position < source.size()) {
        position = source.find(delimiter, position);
        if (position == std::string_view::npos) return position;
        size_t run = DelimiterRun(source, position, delimiter);
        if (run == length) return position;
        position += run;
    }
    return std::string_view::npos;
}

// One pass that stops at the first character an address cannot hold: the text after a
// stray '<' is then not read again for every further '<' in front of the same '>'.
bool LooksLikeEmail(std::string_view value) {
    size_t at = std::string_view::npos;
    size_t atCount = 0;
    for (size_t i = 0; i < value.size(); i++) {
        const char ch = value[i];
        if (!(std::isalnum((unsigned char)ch) || ch == '@' || ch == '.' || ch == '-' || ch == '_' ||
            ch == '+' || ch == '%')) return false;
        if (ch == '@' && atCount++ == 0) at = i;
    }
    if (atCount != 1 || at == 0 || at + 1 >= value.size()) return false;
    size_t dot = value.find('.', at + 1);
    return dot != std::string_view::npos && dot != at + 1 && dot + 1 < value.size();
}

bool IntrawordUnderscore(std::string_view source, size_t start, size_t length) {
    bool leftWord = start > 0 && std::isalnum((unsigned char)source[start - 1]);
    bool rightWord = start + length < source.size() &&
        std::isalnum((unsigned char)source[start + length]);
    return leftWord && rightWord;
}

// Named MathOpener, not MathDelimiter: math_parser.h declares an unscoped
// enum MathDelimiter in this namespace.
enum class MathOpener { Dollar, DoubleDollar, Paren };

struct MathMatch {
    size_t contentBegin = 0;
    size_t contentEnd = 0;
    size_t end = 0;
    bool display = false;
    // The candidate is complete but longer than its size cap. The content range
    // then covers the delimiters too, and the caller shows those bytes as code.
    bool verbatim = false;
};

bool IsBlank(char ch) {
    return ch == ' ' || ch == '\t';
}

// The parser looks ahead from every opener: for the marker that closes an emphasis, the
// end of a code span, the end of a link destination. Ordinary text keeps each lookup
// short. A paragraph with thousands of unterminated openers repeats a long lookup for
// every one of them, which took seconds for 100 kB of text. InlineScanner counts the
// bytes these lookups read. Past a budget that ordinary text does not reach, it answers
// them from tables it builds once for the paragraph. Both ways return the same results.
class InlineScanner {
public:
    InlineScanner(std::string_view text, size_t lookaheadBudget) : source(text) {
        constexpr size_t kNever = static_cast<size_t>(-1);
        // The tables hold 32-bit positions.
        if (text.size() >= 0x7FFFFFF0u) budget = kNever;
        else if (lookaheadBudget != kDefaultLookaheadBudget) budget = lookaheadBudget;
        else budget = text.size() <= (kNever - 2048) / 8 ? text.size() * 8 + 2048 : kNever;
    }

    const std::string_view source;

    // Length of the run of source[i] that starts at i. A long run that is given up and
    // tried again a few characters later is measured once.
    size_t RunLength(size_t i) {
        if (i >= runBegin && i < runEnd) return runEnd - i;
        const char ch = source[i];
        size_t end = i + 1;
        while (end < source.size() && source[end] == ch) end++;
        runBegin = i;
        runEnd = end;
        return end - i;
    }

    // First ']' at or after `from`. The requests of one paragraph mostly move forward,
    // so one search serves every '[' in front of the same bracket.
    size_t NextCloseBracket(size_t from) {
        if (from < bracketFrom || (bracketAt != std::string_view::npos && from > bracketAt)) {
            bracketFrom = from;
            bracketAt = source.find(']', from);
        }
        return bracketAt;
    }

    // First '>' at or after `from`.
    size_t NextGreater(size_t from) {
        if (from < greaterFrom || (greaterAt != std::string_view::npos && from > greaterAt)) {
            greaterFrom = from;
            greaterAt = source.find('>', from);
        }
        return greaterAt;
    }

    // The length of the HTML tag at source[at] == '<' with `token` filled in, or 0. The scanner
    // keeps where the closing quotes of attribute values are, so unterminated quotes stay linear.
    uint32_t Tag(size_t at, Html::TagToken& token) {
        return Html::ScanTag(source, at, quotes, token);
    }

    // First "-->" at or after `from`: the end of an HTML comment.
    size_t NextCommentClose(size_t from) {
        if (from < commentFrom || (commentAt != std::string_view::npos && from > commentAt)) {
            commentFrom = from;
            commentAt = source.find("-->", from);
        }
        return commentAt;
    }

    // First run of exactly `length` backticks at or after `start`, the end of another run.
    RAYOMD_HOT_INLINE size_t BacktickClose(size_t start, size_t length) {
        if (!Indexed() || (start < source.size() && source[start] == '`')) {
            const size_t end = FindExactDelimiterRun(source, start, '`', length);
            Charge((end == std::string_view::npos ? source.size() : end) - start);
            return end;
        }
        Tables& index = Index();
        if (!index.backtickRunsBuilt) BuildBacktickRuns(index);
        const BacktickRun wanted = { static_cast<uint32_t>(std::min<size_t>(length, 0xFFFFFFFFu)), static_cast<uint32_t>(start) };
        auto found = std::lower_bound(index.backtickRuns.begin(), index.backtickRuns.end(), wanted, BacktickRunBefore);
        if (found == index.backtickRuns.end() || found->length != length) return std::string_view::npos;
        return found->position;
    }

    // FindDestinationEnd(source, openParen).
    size_t DestinationEnd(size_t openParen) {
        if (!Indexed()) {
            const size_t end = FindDestinationEnd(source, openParen);
            if (openParen < source.size()) Charge((end == std::string_view::npos ? source.size() : end) - openParen);
            return end;
        }
        if (openParen >= source.size() || source[openParen] != '(') return std::string_view::npos;
        Tables& index = Index();
        if (index.destinationEnds.empty()) BuildDestinationEnds(index);
        const uint32_t end = index.destinationEnds[openParen + 1];
        return end == kNoPosition ? std::string_view::npos : end;
    }

    // True when the emphasis opened by `length` markers in front of `start` has a closing
    // marker: one with text before it that is not escaped, inside a code span or inside
    // a formula.
    bool HasClosingDelimiter(size_t start, char marker, size_t length, bool recognizeMath) {
        if (Indexed()) return HasClosingDelimiterIndexed(start, marker, length, recognizeMath);
        size_t scanned = 0;
        const bool found = HasClosingDelimiterDirect(start, marker, length, recognizeMath, scanned);
        Charge(scanned);
        return found;
    }

private:
    static constexpr uint32_t kNoPosition = 0xFFFFFFFFu;

    struct BacktickRun {
        uint32_t length;
        uint32_t position;
    };
    static bool BacktickRunBefore(const BacktickRun& a, const BacktickRun& b) {
        return a.length != b.length ? a.length < b.length : a.position < b.position;
    }

    // What a walk from a position finds, for one marker and marker count.
    enum CloserMemo : unsigned char { kUnknown, kNoCloser, kCloserHere, kCloserAhead };

    struct Tables {
        std::vector<BacktickRun> backtickRuns;      // every backtick run, by length, then position
        bool backtickRunsBuilt = false;
        std::vector<uint32_t> destinationEnds;      // see BuildDestinationEnds
        std::vector<unsigned char> closers[6];      // CloserMemo per position: '*' x1..x3, '_' x1..x3
        std::vector<uint32_t> trail;                // positions the current walk passed
    };

    bool Indexed() const { return work >= budget; }
    void Charge(size_t bytes) { work += bytes; }
    Tables& Index() {
        if (!tables) tables = std::make_unique<Tables>();
        return *tables;
    }

    void BuildBacktickRuns(Tables& index) const;
    void BuildDestinationEnds(Tables& index) const;
    size_t MathEndCovering(size_t& cursor, size_t position);
    void AdvanceMath(size_t& cursor, size_t& formulaEnd, size_t position);
    bool HasClosingDelimiterDirect(size_t start, char marker, size_t length, bool recognizeMath, size_t& scanned);
    bool HasClosingDelimiterIndexed(size_t start, char marker, size_t length, bool recognizeMath);

    size_t budget = 0;
    size_t work = 0;
    size_t runBegin = 0;
    size_t runEnd = 0;
    size_t bracketFrom = std::string_view::npos;
    size_t bracketAt = std::string_view::npos;
    size_t greaterFrom = std::string_view::npos;
    size_t greaterAt = std::string_view::npos;
    size_t commentFrom = std::string_view::npos;
    size_t commentAt = std::string_view::npos;
    Html::QuoteCache quotes;
    std::unique_ptr<Tables> tables;
};

// Where the close tag "</name>" (any case; white space may come before its '>') of an element
// begins and ends at or after `from`; source.size() for both when the text has none. Callers
// go on after what it returns, so each byte is searched once.
struct CloseTag {
    size_t begin;
    size_t end;
};

CloseTag FindCloseTag(std::string_view source, size_t from, std::string_view name) {
    for (size_t at = source.find("</", from); at != std::string_view::npos; at = source.find("</", at + 2)) {
        size_t index = at + 2;
        if (source.size() - index < name.size()) break;
        bool same = true;
        for (size_t k = 0; k < name.size() && same; k++) {
            const char ch = source[index + k];
            same = (ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch) == name[k];
        }
        if (!same) continue;
        index += name.size();
        while (index < source.size() && (source[index] == ' ' || source[index] == '\t' || source[index] == '\n' ||
                                            source[index] == '\r')) {
            index++;
        }
        if (index < source.size() && source[index] == '>') return { at, index + 1 };
    }
    return { source.size(), source.size() };
}

// The text of a <sup> or <sub>, source[begin, end), as plain text: character references
// decoded, tags left out, line breaks as spaces.
void AppendScriptText(InlineScanner& scan, std::string& text, size_t begin, size_t end) {
    const std::string_view source = scan.source;
    Html::TagToken token;
    for (size_t at = begin; at < end;) {
        const char ch = source[at];
        if (ch == '<') {
            const uint32_t length = scan.Tag(at, token);
            if (length != 0 && at + length <= end) {
                at += length;
                continue;
            }
        } else if (ch == '&') {
            uint32_t codePoint = 0;
            const size_t length = MatchCharacterReference(source, at, codePoint);
            if (length != 0 && at + length <= end) {
                AppendReference(text, codePoint);
                at += length;
                continue;
            }
        }
        text.push_back(ch == '\n' || ch == '\r' ? ' ' : ch);
        at++;
    }
}

// The HTML tag at source[at] == '<', as GitHub shows it: emphasis, code, decoration, a link, a
// script, an image's alt text, quotation marks or a line break; an element whose content is not
// shown left out up to its close tag; any other tag removed and its text kept. Returns the index
// behind what it covers, or 0 when no tag starts there. Out of the parse loop: most text has none.
RAYOMD_NOINLINE size_t WriteHtmlTag(InlineScanner& scan, RunWriter& writer, InlineRuns& out, size_t at, bool bold,
    bool italic, bool strike) {
    using Html::Tag;
    // Kept from tag to tag: ScanTag clears only what the last tag set, where a new token would be
    // cleared whole (168 bytes) for every tag.
    RAYOMD_THREAD_OBJECT(Html::TagToken, token);
    const uint32_t length = scan.Tag(at, token);
    if (length == 0) return 0;
    const size_t end = at + length;
    // The text before the tag ends its run in the state before it.
    writer.Flush(bold, italic, strike);
    HtmlInline& html = writer.html;
    html.active = true;
    const auto count = [&](uint16_t& counter) {
        if (token.closing) {
            if (counter != 0) counter--;
        } else if (!token.selfClosing && counter != 0xFFFF) {
            counter++;
        }
    };
    switch (token.tag) {
    case Tag::B: case Tag::Strong: count(html.bold); break;
    case Tag::I: case Tag::Em: case Tag::Cite: case Tag::Dfn: case Tag::Var: count(html.italic); break;
    case Tag::S: case Tag::Strike: case Tag::Del: count(html.strike); break;
    case Tag::Code: case Tag::Tt: case Tag::Kbd: case Tag::Samp: count(html.code); break;
    case Tag::U: case Tag::Ins: count(html.underline); break;
    case Tag::Mark: count(html.mark); break;
    case Tag::Sup:
    case Tag::Sub: {
        if (token.closing || token.selfClosing) break;
        const CloseTag close = FindCloseTag(scan.source, end, Html::TagName(token.tag));
        AppendScriptText(scan, out.text, end, close.begin);
        writer.Script(token.tag == Tag::Sub);
        return close.end;
    }
    case Tag::A:
        if (token.closing) {
            html.urlBegin = html.urlEnd = 0;
        } else if (token.href.data() != nullptr) {
            // Decoded where it is kept, and taken back when it is not safe.
            const size_t begin = out.urls.size();
            if (token.href.find('&') == std::string_view::npos) out.urls.append(token.href.data(), token.href.size());
            else AppendDecoded(out.urls, token.href);
            if (Html::SafeUrl(std::string_view(out.urls).substr(begin))) {
                html.urlBegin = begin;
                html.urlEnd = out.urls.size();
            } else {
                out.urls.resize(begin);
            }
        }
        break;
    case Tag::Img:
        if (!token.closing) {
            out.text += "image: ";
            AppendDecoded(out.text, token.alt);
        }
        break;
    case Tag::Q: AppendReference(out.text, token.closing ? 0x201D : 0x201C); break;
    case Tag::Br:
        if (!token.closing) writer.Break();
        break;
    default:
        if (!token.closing && !token.selfClosing && Html::IsDroppedWithContent(token.tag)) {
            return FindCloseTag(scan.source, end, Html::TagName(token.tag)).end;
        }
        break;
    }
    return end;
}

bool ParseLinkAt(InlineScanner& scan, size_t start, size_t labelOffset, InlineLink& link) {
    const std::string_view source = scan.source;
    if (start + labelOffset >= source.size()) return false;
    size_t close = scan.NextCloseBracket(start + labelOffset);
    if (close == std::string_view::npos || close + 1 >= source.size() || source[close + 1] != '(') {
        return false;
    }
    size_t end = scan.DestinationEnd(close + 1);
    if (end == std::string_view::npos) return false;
    link.label = source.substr(start + labelOffset, close - (start + labelOffset));
    link.target = source.substr(close + 2, end - (close + 2));
    link.end = end + 1;
    return true;
}

// A linked image, "[![alt](src)](target)", whose link text is the whole image. ParseLinkAt
// would end that text at the image's own ']'. `start` is the position of the first '['.
RAYOMD_NOINLINE bool ParseLinkedImageAt(InlineScanner& scan, size_t start, InlineLink& link) {
    const std::string_view source = scan.source;
    InlineLink image;
    if (!ParseLinkAt(scan, start + 1, 2, image)) return false;
    const size_t close = image.end;
    if (close + 1 >= source.size() || source[close] != ']' || source[close + 1] != '(') return false;
    const size_t end = scan.DestinationEnd(close + 1);
    if (end == std::string_view::npos) return false;
    link.label = source.substr(start + 1, close - (start + 1));
    link.target = source.substr(close + 2, end - (close + 2));
    link.end = end + 1;
    return true;
}

// The end of the footnote reference at source[at], "[^label]" with a label of 1 to 999 bytes
// without white space or ']' (as cmark-gfm reads it), when tFootnoteLabels has the label;
// npos otherwise. Only as far as the longest label reaches: text full of "[^" costs no more
// than that much per opener.
RAYOMD_NOINLINE size_t FootnoteReferenceEnd(std::string_view source, size_t at) {
    const size_t begin = at + 2;
    const size_t limit = std::min(source.size(), begin + tFootnoteLabelBytes);
    size_t end = begin;
    for (; end < limit && source[end] != ']'; end++) {
        const char ch = source[end];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0') return std::string_view::npos;
    }
    if (end >= source.size() || source[end] != ']' || end == begin) return std::string_view::npos;
    const bool known = tFootnoteLabels->count(NormalizeReferenceLabel(source.substr(begin, end - begin))) != 0;
    return known ? end + 1 : std::string_view::npos;
}

// Returns the index of the first closing delimiter of a math body that starts at
// `from`, or npos. Shared body rules: a backslash and the byte after it are one
// unit (so \$ and \\ never close), a complete code span is opaque, and a line
// feed ends the search because the renderers split paragraphs at hard breaks
// before they parse inline text.
size_t FindMathClose(InlineScanner& scan, size_t from, MathOpener delimiter) {
    const std::string_view source = scan.source;
    for (size_t i = from; i < source.size();) {
        char ch = source[i];
        if (ch == '\n') return std::string_view::npos;
        if (ch == '\\') {
            if (delimiter == MathOpener::Paren && i + 1 < source.size()) {
                if (source[i + 1] == ')') return i;
                // A second "\(" ends the search, which keeps a run of unterminated
                // openers linear instead of quadratic.
                if (source[i + 1] == '(') return std::string_view::npos;
            }
            i += 2;
            continue;
        }
        if (ch == '`') {
            size_t run = scan.RunLength(i);
            size_t end = scan.BacktickClose(i + run, run);
            i = end == std::string_view::npos ? i + run : end + run;
            continue;
        }
        if (ch == '$') {
            if (delimiter == MathOpener::Dollar) return i;
            if (delimiter == MathOpener::DoubleDollar && i + 1 < source.size() && source[i + 1] == '$') return i;
        }
        i++;
    }
    return std::string_view::npos;
}

// Recognises math that opens at source[i], which must be '$' or the backslash
// of "\(". The first closing delimiter decides: when it is unusable the opener
// is literal text and no later delimiter is tried. For a '$' opener that is not
// math, match.end is the end of the bytes the caller must keep as literal text:
// one dollar, an unmatched "$$", or a whole empty "$$ $$" pair.
// A complete candidate whose body is over its size cap also matches, as a
// verbatim span from the opening to the closing delimiter: it is too long to be
// one unbreakable box, and reading it as Markdown instead would eat its '_',
// '*' and backslashes.
bool MatchMathAt(InlineScanner& scan, size_t i, MathMatch& match) {
    const std::string_view source = scan.source;
    if (source[i] == '$') {
        if (i + 1 < source.size() && source[i + 1] == '$') {
            match.end = i + 2;
            size_t close = FindMathClose(scan, i + 2, MathOpener::DoubleDollar);
            if (close == std::string_view::npos) return false;
            size_t begin = i + 2;
            size_t end = close;
            while (begin < end && IsSpace(source[begin])) begin++;
            while (end > begin && IsSpace(source[end - 1])) end--;
            if (begin == end) {
                match.end = close + 2;
                return false;
            }
            if (end - begin > kMaxDisplayMathBytes) match = { i, close + 2, close + 2, true, true };
            else match = { begin, end, close + 2, true };
            return true;
        }
        match.end = i + 1;
        if (i + 1 >= source.size() || IsSpace(source[i + 1])) return false;
        size_t close = FindMathClose(scan, i + 1, MathOpener::Dollar);
        if (close == std::string_view::npos || IsSpace(source[close - 1])) return false;
        if (close + 1 < source.size() && std::isdigit((unsigned char)source[close + 1])) return false;
        if (close - (i + 1) > kMaxInlineMathBytes) match = { i, close + 1, close + 1, false, true };
        else match = { i + 1, close, close + 1, false };
        return true;
    }
    // "\( x \)": the padded form only, because "\(" and "\)" are also classic escapes.
    if (i + 2 >= source.size() || source[i + 1] != '(' || !IsBlank(source[i + 2])) return false;
    size_t close = FindMathClose(scan, i + 2, MathOpener::Paren);
    if (close == std::string_view::npos || !IsBlank(source[close - 1])) return false;
    size_t begin = i + 2;
    size_t end = close;
    while (begin < end && IsSpace(source[begin])) begin++;
    while (end > begin && IsSpace(source[end - 1])) end--;
    if (begin == end) return false;
    if (end - begin > kMaxInlineMathBytes) match = { i, close + 2, close + 2, false, true };
    else match = { begin, end, close + 2, false };
    return true;
}

// Returns the end of the formula that covers `position`, or npos when that byte
// is ordinary text. Bytes before `cursor` are already known to lie outside math;
// the cursor only moves forward, so one emphasis lookup walks its text once.
// Kept out of line: inlined into HasClosingDelimiterDirect it slowed that function's
// scan for a closing marker, which text without any math runs too (measured
// 1.7x on a long run of unmatched markers).
RAYOMD_NOINLINE size_t InlineScanner::MathEndCovering(size_t& cursor, size_t position) {
    if (cursor >= position) return std::string_view::npos;
    std::string_view pending = source.substr(cursor, position - cursor);
    if (pending.find('$') == std::string_view::npos && pending.find("\\(") == std::string_view::npos) {
        cursor = position;
        return std::string_view::npos;
    }
    while (cursor < position) {
        char ch = source[cursor];
        if (ch == '$' || (ch == '\\' && cursor + 1 < source.size() && source[cursor + 1] == '(')) {
            MathMatch math;
            bool matched = MatchMathAt(*this, cursor, math);
            if (matched && math.end > position) {
                cursor = math.end;
                return math.end;
            }
            if (matched || ch == '$') {
                cursor = math.end;
                continue;
            }
        }
        if (ch == '\\' && cursor + 1 < source.size()) {
            cursor += 2;
            continue;
        }
        if (ch == '`') {
            size_t run = RunLength(cursor);
            size_t end = BacktickClose(cursor + run, run);
            cursor = end == std::string_view::npos ? cursor + run : end + run;
            continue;
        }
        cursor++;
    }
    return std::string_view::npos;
}

// The lookup as one scan from `start`. `scanned` receives the bytes it covered.
bool InlineScanner::HasClosingDelimiterDirect(size_t start, char marker, size_t length, bool recognizeMath,
    size_t& scanned) {
    size_t mathCursor = recognizeMath ? start : std::string_view::npos;
    size_t i = start;
    bool found = false;
    while (i + length <= source.size()) {
        if (source[i] == '\\' && i + 1 < source.size()) {
            i += 2;
            continue;
        }
        if (source[i] == '`') {
            size_t run = RunLength(i);
            size_t end = BacktickClose(i + run, run);
            if (end == std::string_view::npos) break;
            i = end + run;
            continue;
        }
        if (source[i] != marker || RunLength(i) < length) {
            i++;
            continue;
        }
        bool hasContent = i > start && !IsSpace(source[i - 1]);
        if (hasContent && !(marker == '_' && IntrawordUnderscore(source, i, length))) {
            // A marker inside a formula never closes emphasis. The check runs only
            // for a real candidate, so unmatched markers cost what they cost before.
            size_t mathEnd = mathCursor == std::string_view::npos ? std::string_view::npos :
                MathEndCovering(mathCursor, i);
            if (mathEnd == std::string_view::npos) {
                found = true;
                break;
            }
            i = mathEnd;
            continue;
        }
        i += length;
    }
    scanned = std::min(i, source.size()) - start;
    return found;
}

// Moves the math reading of an indexed walk up to `position`, over the same tokens as
// MathEndCovering. formulaEnd receives the end of the last formula it crossed: a
// position before that end lies inside the formula.
void InlineScanner::AdvanceMath(size_t& cursor, size_t& formulaEnd, size_t position) {
    while (cursor < position) {
        const char ch = source[cursor];
        if (ch == '$' || (ch == '\\' && cursor + 1 < source.size() && source[cursor + 1] == '(')) {
            MathMatch math;
            const bool matched = MatchMathAt(*this, cursor, math);
            if (matched) formulaEnd = math.end;
            if (matched || ch == '$') {
                cursor = math.end;
                continue;
            }
        }
        if (ch == '\\' && cursor + 1 < source.size()) {
            cursor += 2;
            continue;
        }
        if (ch == '`') {
            const size_t run = RunLength(cursor);
            const size_t end = BacktickClose(cursor + run, run);
            cursor = end == std::string_view::npos ? cursor + run : end + run;
            continue;
        }
        cursor++;
    }
}

// The lookup with a memo. A scan moves from one position to the next by rules that do
// not depend on where it started, so two scans that reach the same position end the
// same way. Every position a walk passes remembers that end, and a later walk stops at
// the first position it knows. Each position is therefore walked once per marker kind,
// however many openers ask.
//
// Two details keep this exact. A closer at the very first position does not count for
// the lookup that starts there (it has no text before it), so that one answer is taken
// from the position after it. And with formulas, what a walk finds also depends on how
// it reads the math in front of it: it only remembers, and only trusts, positions where
// that reading is not in the middle of a formula, a code span or an escape. From such a
// position every walk reads the math the same way.
bool InlineScanner::HasClosingDelimiterIndexed(size_t start, char marker, size_t length, bool recognizeMath) {
    Tables& index = Index();
    std::vector<unsigned char>& memo = index.closers[(marker == '_' ? 3 : 0) + (length - 1)];
    if (memo.empty()) memo.assign(source.size() + 1, kUnknown);
    std::vector<uint32_t>& trail = index.trail;
    trail.clear();
    size_t mathCursor = start;
    size_t formulaEnd = 0;
    unsigned char outcome = kNoCloser;
    for (size_t i = start; i + length <= source.size();) {
        bool settled = true;
        if (recognizeMath) {
            AdvanceMath(mathCursor, formulaEnd, i);
            settled = mathCursor == i;
        }
        if (settled) {
            const unsigned char known = memo[i];
            if (known != kUnknown && !(i == start && known == kCloserHere)) {
                outcome = known == kNoCloser ? kNoCloser : kCloserAhead;
                break;
            }
            if (known == kUnknown) trail.push_back(static_cast<uint32_t>(i));
        }
        const char ch = source[i];
        if (ch == '\\' && i + 1 < source.size()) {
            i += 2;
            continue;
        }
        if (ch == '`') {
            const size_t run = RunLength(i);
            const size_t end = BacktickClose(i + run, run);
            if (end == std::string_view::npos) break;
            i = end + run;
            continue;
        }
        if (ch != marker || RunLength(i) < length) {
            i++;
            continue;
        }
        if (!IsSpace(source[i - 1]) && !(marker == '_' && IntrawordUnderscore(source, i, length))) {
            // A closing marker for every walk that arrives here from before.
            const bool passed = !trail.empty() && trail.back() == i;
            if (i > start && formulaEnd > i) {
                i = formulaEnd;
                continue;
            }
            if (passed) {
                trail.pop_back();
                memo[i] = kCloserHere;
            }
            if (i > start) {
                outcome = kCloserAhead;
                break;
            }
        }
        i += length;
    }
    for (const uint32_t position : trail) memo[position] = outcome;
    return outcome != kNoCloser;
}

void InlineScanner::BuildBacktickRuns(Tables& index) const {
    for (size_t at = source.find('`'); at != std::string_view::npos;) {
        size_t end = at + 1;
        while (end < source.size() && source[end] == '`') end++;
        index.backtickRuns.push_back({ static_cast<uint32_t>(end - at), static_cast<uint32_t>(at) });
        at = source.find('`', end);
    }
    std::sort(index.backtickRuns.begin(), index.backtickRuns.end(), BacktickRunBefore);
    index.backtickRunsBuilt = true;
}

// destinationEnds[x] is what FindDestinationEnd finds for a scan that stands at position
// x outside nested parentheses and outside an angle destination: the first unescaped ')'
// that closes at that level, or kNoPosition. Read from right to left, every entry follows
// from entries after it, because whether a character is escaped depends only on the
// backslashes directly in front of it.
void InlineScanner::BuildDestinationEnds(Tables& index) const {
    const size_t size = source.size();
    std::vector<uint32_t>& ends = index.destinationEnds;
    ends.assign(size + 2, kNoPosition);
    size_t nextGreater = std::string_view::npos;    // first unescaped '>' after x
    for (size_t x = size; x-- > 0;) {
        const char ch = source[x];
        uint32_t end = ends[x + 1];
        if (ch == '(' || ch == ')' || ch == '<' || ch == '>') {
            size_t slashes = 0;
            while (x > slashes && source[x - slashes - 1] == '\\') slashes++;
            if ((slashes & 1) == 0) {
                if (ch == ')') {
                    end = static_cast<uint32_t>(x);
                } else if (ch == '(') {
                    // The scan resumes after the ')' that closes this parenthesis.
                    end = end == kNoPosition ? kNoPosition : ends[end + 1];
                } else if (ch == '<') {
                    // An angle destination runs to the next unescaped '>'.
                    end = nextGreater == std::string_view::npos ? kNoPosition : ends[nextGreater + 1];
                } else {
                    nextGreater = x;
                }
            }
        }
        ends[x] = end;
    }
}

// The bytes at which ParseInlineRuns stops copying text: inline syntax, and the ':' or '@' at
// which a bare URL or email address is found.
constexpr unsigned char kStops = RayoMd::Text::kByteInlineSyntax | RayoMd::Text::kByteAutolinkLead;

#if RAYOMD_INLINE_AVX2
// The stops as two tables of 16 bytes, looked up by a byte's low and high nibble (vpshufb): each
// high nibble of a stop has a bit of its own, which the low-nibble entry of each stop with that
// high nibble sets, so a byte is a stop when its two entries share a bit. Bytes from 0x80 look up
// 0 in the low table (vpshufb's sign rule) and in the high one. Twelve compares a step in SSE2
// (FindByteClass) are four instructions here.
struct StopNibbles {
    unsigned char low[16];
    unsigned char high[16];
};

constexpr StopNibbles MakeStopNibbles() {
    StopNibbles tables{};
    unsigned char bitOfHigh[8] = {};
    unsigned next = 0;
    for (unsigned value = 0; value < 0x80; value++) {
        if ((RayoMd::Text::Detail::kByteClasses[value] & kStops) == 0) continue;
        if (bitOfHigh[value >> 4] == 0) bitOfHigh[value >> 4] = static_cast<unsigned char>(1u << next++);
        tables.high[value >> 4] = bitOfHigh[value >> 4];
        tables.low[value & 15] |= bitOfHigh[value >> 4];
    }
    return tables;
}
constexpr StopNibbles kStopNibbles = MakeStopNibbles();

constexpr bool StopNibblesExact() {
    for (unsigned value = 0; value < 256; value++) {
        const bool stop = value < 0x80 && (kStopNibbles.low[value & 15] & kStopNibbles.high[value >> 4]) != 0;
        if (stop != ((RayoMd::Text::Detail::kByteClasses[value] & kStops) != 0)) return false;
    }
    return true;
}
static_assert(StopNibblesExact(), "the nibble tables find exactly the stops");

// The first stop in data[at, size) or size, for at least 32 bytes: 32 a step, the last step
// reading the text's last 32 bytes again.
__attribute__((target("avx2"))) RAYOMD_NOINLINE size_t FindStopsAvx2(const char* data, size_t at, size_t size) {
    const __m256i low = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(kStopNibbles.low)));
    const __m256i high = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(kStopNibbles.high)));
    const __m256i nibble = _mm256_set1_epi8(0x0F);
    const __m256i zero = _mm256_setzero_si256();
    for (; at + 32 <= size; at += 32) {
        const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + at));
        const __m256i hits = _mm256_and_si256(_mm256_shuffle_epi8(low, chunk),
            _mm256_shuffle_epi8(high, _mm256_and_si256(_mm256_srli_epi16(chunk, 4), nibble)));
        const uint32_t bits = ~static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(hits, zero)));
        if (bits != 0) return at + static_cast<size_t>(__builtin_ctz(bits));
    }
    if (at == size) return size;
    const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + size - 32));
    const __m256i hits = _mm256_and_si256(_mm256_shuffle_epi8(low, chunk),
        _mm256_shuffle_epi8(high, _mm256_and_si256(_mm256_srli_epi16(chunk, 4), nibble)));
    const uint32_t bits = ~static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(hits, zero))) >> (at + 32 - size);
    return bits != 0 ? at + static_cast<size_t>(__builtin_ctz(bits)) : size;
}

// The CPU's answer, asked once: -1 until then (as word_bounds.cpp keeps it).
std::atomic<signed char> avx2State{-1};

RAYOMD_NOINLINE bool AskAvx2() {
    const bool avx2 = RayoMd::Cpu::HasAvx2();
    avx2State.store(avx2 ? 1 : 0, std::memory_order_relaxed);
    return avx2;
}
#endif

// The first stop (kStops) in `source` from `from` on, or source.size(): AVX2 from 32 bytes where
// the CPU has it, else FindByteClass.
inline size_t FindStops(std::string_view source, size_t from) {
#if RAYOMD_INLINE_AVX2
    if (source.size() - from >= 32) {
        const signed char state = avx2State.load(std::memory_order_relaxed);
        if (state > 0 || (state < 0 && AskAvx2())) return FindStopsAvx2(source.data(), from, source.size());
    }
#endif
    return RayoMd::Text::FindByteClass<kStops>(source, from);
}

// The bytes ParagraphRunsTest looks for, a bit each: the line feed, '$' and 0xE2 answer for
// themselves; '\\' and '(', '[' and '^' are the halves of "\(" and "[^".
enum ParagraphByteBit : unsigned char {
    kBitLineFeed = 1, kBitDollar = 2, kBitSymbol = 4, kBitBackslash = 8, kBitParen = 16, kBitBracket = 32,
    kBitCaret = 64
};
constexpr unsigned char kParagraphByteValues[7] = { '\n', '$', 0xE2, '\\', '(', '[', '^' };

struct ParagraphTables {
    unsigned char byte[256];        // the bit of each byte
    unsigned char low[16];          // by low nibble: the bits of the bytes with that nibble
    unsigned char high[16];         // by high nibble, likewise; a byte's bit is in both of its entries
};

constexpr ParagraphTables MakeParagraphTables() {
    ParagraphTables tables{};
    for (unsigned bit = 0; bit < 7; bit++) {
        const unsigned char value = kParagraphByteValues[bit];
        tables.byte[value] = static_cast<unsigned char>(1u << bit);
        tables.low[value & 15] |= static_cast<unsigned char>(1u << bit);
        tables.high[value >> 4] |= static_cast<unsigned char>(1u << bit);
    }
    return tables;
}
constexpr ParagraphTables kParagraphTables = MakeParagraphTables();

constexpr bool ParagraphNibblesExact() {
    for (unsigned value = 0; value < 256; value++) {
        if ((kParagraphTables.low[value & 15] & kParagraphTables.high[value >> 4]) != kParagraphTables.byte[value]) return false;
    }
    return true;
}
static_assert(ParagraphNibblesExact(), "the nibble tables find exactly the paragraph bytes");

#if RAYOMD_INLINE_AVX2
// The bits of the 32 bytes at `at`.
__attribute__((target("avx2"), always_inline)) inline __m256i ParagraphStepAvx2(const char* at, __m256i low, __m256i high,
    __m256i nibble) {
    const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(at));
    return _mm256_and_si256(_mm256_shuffle_epi8(low, _mm256_and_si256(chunk, nibble)),
        _mm256_shuffle_epi8(high, _mm256_and_si256(_mm256_srli_epi16(chunk, 4), nibble)));
}

// The bits of all bytes of data[0, size), size at least 32: 32 bytes a step, the last step reading
// the last 32 bytes again (a bit seen twice is the same bit). Both nibbles are masked, so 0xE2
// looks up its entries as any byte does.
__attribute__((target("avx2"))) RAYOMD_NOINLINE unsigned ParagraphBitsAvx2(const char* data, size_t size) {
    const __m256i low = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(kParagraphTables.low)));
    const __m256i high = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(kParagraphTables.high)));
    const __m256i nibble = _mm256_set1_epi8(0x0F);
    __m256i seen = ParagraphStepAvx2(data + size - 32, low, high, nibble);
    for (size_t at = 0; at + 32 < size; at += 32) seen = _mm256_or_si256(seen, ParagraphStepAvx2(data + at, low, high, nibble));
    __m128i half = _mm_or_si128(_mm256_castsi256_si128(seen), _mm256_extracti128_si256(seen, 1));
    half = _mm_or_si128(half, _mm_srli_si128(half, 8));
    half = _mm_or_si128(half, _mm_srli_si128(half, 4));
    uint32_t word = static_cast<uint32_t>(_mm_cvtsi128_si32(half));
    word |= word >> 16;
    word |= word >> 8;
    return word & 0xFF;
}
#endif

} // namespace

unsigned ParagraphRunsTest(std::string_view text, bool notes) {
    unsigned bits = 0;
#if RAYOMD_INLINE_AVX2
    if (text.size() >= 32) {
        const signed char state = avx2State.load(std::memory_order_relaxed);
        if (state > 0 || (state < 0 && AskAvx2())) {
            bits = ParagraphBitsAvx2(text.data(), text.size());
        } else {
            bits = (text.find('\n') != std::string_view::npos ? kBitLineFeed : 0) |
                (text.find('$') != std::string_view::npos ? kBitDollar : 0) |
                (text.find('\xE2') != std::string_view::npos ? kBitSymbol : 0) |
                (text.find('\\') != std::string_view::npos ? kBitBackslash | kBitParen : 0) |
                (text.find('^') != std::string_view::npos ? kBitBracket | kBitCaret : 0);
        }
    } else
#endif
    {
        for (const char ch : text) bits |= kParagraphTables.byte[static_cast<unsigned char>(ch)];
    }
    if (bits & (kBitLineFeed | kBitDollar)) return kParagraphGeneral;
    // The pairs only where both of their bytes are, which is rare: a backslash, a caret.
    if ((bits & (kBitBackslash | kBitParen)) == (kBitBackslash | kBitParen) && text.find("\\(") != std::string_view::npos) {
        return kParagraphGeneral;
    }
    if (notes && (bits & (kBitBracket | kBitCaret)) == (kBitBracket | kBitCaret) && text.find("[^") != std::string_view::npos) {
        return kParagraphGeneral;
    }
    return (bits & kBitSymbol) ? kParagraphSymbol : 0;
}

RAYOMD_NOINLINE size_t MatchCharacterReference(std::string_view text, size_t at, uint32_t& codePoint) {
    if (at + 3 > text.size() || text[at] != '&') return 0;
    size_t i = at + 1;
    const auto digit = [](char ch, bool hex) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (hex && ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (hex && ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    if (text[i] == '#') {
        // "&#" and one to seven digits, or "&#x" and one to six hexadecimal digits, then ';'.
        const bool hex = ++i < text.size() && (text[i] == 'x' || text[i] == 'X');
        if (hex) i++;
        const size_t digitsBegin = i;
        const size_t digitsEnd = std::min(text.size(), digitsBegin + (hex ? 6 : 7));
        uint32_t value = 0;
        for (int d; i < digitsEnd && (d = digit(text[i], hex)) >= 0; i++) value = value * (hex ? 16 : 10) + (uint32_t)d;
        if (i == digitsBegin || i >= text.size() || text[i] != ';') return 0;
        codePoint = value == 0 || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF) ? 0xFFFD : value;
        return i + 1 - at;
    }
    // '&', a letter and letters and digits, then ';': a name the table holds, eight at most.
    const auto alphanumeric = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    };
    const size_t nameBegin = i;
    while (i < text.size() && i - nameBegin < 9 && alphanumeric(text[i])) i++;
    if (i == nameBegin || i >= text.size() || text[i] != ';' || digit(text[nameBegin], false) >= 0) return 0;
    const std::string_view name = text.substr(nameBegin, i - nameBegin);
    const auto found = std::lower_bound(std::begin(kNamedReferences), std::end(kNamedReferences), name,
        [](const NamedReference& entry, std::string_view wanted) { return std::string_view(entry.name) < wanted; });
    if (found == std::end(kNamedReferences) || std::string_view(found->name) != name) return 0;
    codePoint = found->codePoint;
    return i + 1 - at;
}

ReferenceNeed CharacterReferenceNeed(std::string_view text) {
    ReferenceNeed need = ReferenceNeed::Ascii;
    for (size_t at = text.find('&'); at != std::string_view::npos; at = text.find('&', at + 1)) {
        uint32_t codePoint = 0;
        if (MatchCharacterReference(text, at, codePoint) == 0 || codePoint < 0x80 || IsDroppedReference(codePoint) ||
            IsSpaceReference(codePoint)) {
            continue;
        }
        if (RayoMd::Text::WinAnsiCode(codePoint) < 0) return ReferenceNeed::Unicode;
        need = ReferenceNeed::WinAnsi;
    }
    return need;
}

#ifdef RAYOMD_BUILD_SESSION
// A BuildSession counts the references of its text as CharacterReferenceNeed reads them, a change at
// a time (build_session.cpp's SessionText).
RAYOMD_COLD ReferenceNeed CharacterReferenceNeedAt(std::string_view text, size_t at) {
    uint32_t codePoint = 0;
    if (MatchCharacterReference(text, at, codePoint) == 0 || codePoint < 0x80 || IsDroppedReference(codePoint) ||
        IsSpaceReference(codePoint)) {
        return ReferenceNeed::Ascii;
    }
    return RayoMd::Text::WinAnsiCode(codePoint) < 0 ? ReferenceNeed::Unicode : ReferenceNeed::WinAnsi;
}
#endif

void NonAsciiCodepoints(std::string_view text, CidList& out) {
    out.clear();
    // A bit per code point in words of 64; `touched` has a bit per word written, the only words
    // that are cleared and read back.
    uint64_t bits[1024];
    uint64_t touched[16] = {};
    const auto mark = [&](uint32_t codePoint) {
        if (codePoint < 0x80 || codePoint > 0xFFFF) return;
        const uint32_t word = codePoint >> 6;
        const uint64_t wordBit = uint64_t(1) << (word & 63);
        if ((touched[word >> 6] & wordBit) == 0) {
            touched[word >> 6] |= wordBit;
            bits[word] = 0;
        }
        bits[word] |= uint64_t(1) << (codePoint & 63);
    };
    constexpr uint64_t kOnes = 0x0101010101010101ull;
    constexpr uint64_t kHigh = 0x8080808080808080ull;
    const char* const data = text.data();
    const size_t size = text.size();
    size_t at = 0;
    while (at < size) {
        const unsigned char byte = static_cast<unsigned char>(data[at]);
        uint32_t codePoint = 0;
        size_t length = 0;
        if (byte >= 0x80) {
            if (RayoMd::Text::DecodeUtf8(text, at, codePoint, length)) {
                mark(codePoint);
                at += length;
            } else {
                mark(0xFFFD);
                at++;
            }
            continue;
        }
        if (byte == '&') length = MatchCharacterReference(text, at, codePoint);
        if (length != 0) {
            mark(codePoint);
            at += length;
            continue;
        }
        // Then eight bytes at a time past ASCII without '&'; on little-endian targets on to the
        // first byte that is not (the lowest flagged byte is exact: only those above it can be false).
        at++;
        while (size - at >= 8) {
            uint64_t chunk;
            memcpy(&chunk, data + at, 8);
            const uint64_t amp = chunk ^ (kOnes * '&');
            const uint64_t flagged = (chunk | ((amp - kOnes) & ~amp)) & kHigh;
            if (flagged == 0) {
                at += 8;
                continue;
            }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            const uint32_t low = static_cast<uint32_t>(flagged);
            at += (low != 0 ? RayoMd::Text::Detail::LowestBit(low)
                            : 32 + RayoMd::Text::Detail::LowestBit(static_cast<uint32_t>(flagged >> 32))) / 8;
#endif
            break;
        }
    }
    for (uint32_t group = 0; group < 16; group++) {
        for (uint64_t words = touched[group]; words != 0; words &= words - 1) {
            const uint32_t low = static_cast<uint32_t>(words);
            const uint32_t word = group * 64 + (low != 0 ? RayoMd::Text::Detail::LowestBit(low)
                                                         : 32 + RayoMd::Text::Detail::LowestBit(static_cast<uint32_t>(words >> 32)));
            for (uint64_t set = bits[word]; set != 0; set &= set - 1) {
                const uint32_t setLow = static_cast<uint32_t>(set);
                const uint32_t bit = setLow != 0 ? RayoMd::Text::Detail::LowestBit(setLow)
                                                 : 32 + RayoMd::Text::Detail::LowestBit(static_cast<uint32_t>(set >> 32));
                out.push_back(static_cast<uint16_t>(word * 64 + bit));
            }
        }
    }
}

WinAnsiReferences::WinAnsiReferences() : previous(tWinAnsiReferences) {
    tWinAnsiReferences = true;
}

WinAnsiReferences::~WinAnsiReferences() {
    tWinAnsiReferences = previous;
}

bool WinAnsiReferences::Active() {
    return tWinAnsiReferences;
}

FootnoteLabels::FootnoteLabels(const std::unordered_map<std::string, int>& numbers)
    : previous(tFootnoteLabels), previousBytes(tFootnoteLabelBytes) {
    size_t longest = 0;
    for (const auto& entry : numbers) longest = std::max(longest, entry.first.size());
    tFootnoteLabels = &numbers;
    tFootnoteLabelBytes = std::min<size_t>(999, longest * 3 + 4);
}

FootnoteLabels::~FootnoteLabels() {
    tFootnoteLabels = previous;
    tFootnoteLabelBytes = previousBytes;
}

ParseBudget::ParseBudget(size_t sourceBytes)
    : referenceBytes(2 * sourceBytes + kReferenceSlackBytes), previous(tParseBudget) {
    tParseBudget = this;
}

ParseBudget::~ParseBudget() {
    tParseBudget = previous;
}

ParseBudget* ParseBudget::Current() {
    return tParseBudget;
}

std::string NormalizeReferenceLabel(std::string_view label) {
    label = TrimView(label);
    std::string normalized;
    normalized.reserve(label.size());
    bool pendingSpace = false;
    for (char ch : label) {
        if (IsSpace(ch)) {
            pendingSpace = !normalized.empty();
            continue;
        }
        if (pendingSpace) normalized.push_back(' ');
        pendingSpace = false;
        normalized.push_back((char)std::tolower((unsigned char)ch));
    }
    return normalized;
}

std::string ResolveReferenceLinks(std::string_view input, const ReferenceDefinitions& definitions) {
    if (definitions.empty() || input.find('[') == std::string_view::npos) return std::string(input);
    std::string output;
    output.reserve(input.size());
    // The destination bytes this text may still copy in: its document's budget, or outside a
    // parse one of its own. A reference past it stays text.
    ParseBudget* const budget = ParseBudget::Current();
    size_t ownBytes = 2 * input.size() + ParseBudget::kReferenceSlackBytes;
    size_t& bytesLeft = budget != nullptr ? budget->referenceBytes : ownBytes;
    const auto affordable = [&](const ReferenceDefinition& definition) {
        if (definition.destination.size() > bytesLeft) return false;
        bytesLeft -= definition.destination.size();
        return true;
    };
    // First ']' at or after `from`: one search serves every '[' in front of the same bracket. One
    // such cache for the label's ']' and one for the reference's ("[a][b]"), whose searches would
    // otherwise evict each other: "[[[…][" then text without ']' searched the text once a '['.
    struct CloseBracket {
        size_t from = std::string_view::npos;
        size_t at = std::string_view::npos;
    };
    CloseBracket labelBracket;
    CloseBracket referenceBracket;
    auto nextCloseBracket = [&input](CloseBracket& cache, size_t from) {
        if (from < cache.from || (cache.at != std::string_view::npos && from > cache.at)) {
            cache.from = from;
            cache.at = input.find(']', from);
        }
        return cache.at;
    };
    // "[text](<destination>)", the inline link a resolved reference becomes.
    const auto appendLink = [&](bool image, std::string_view text, const ReferenceDefinition& definition) {
        if (image) output.push_back('!');
        output.push_back('[');
        output.append(text);
        output += "](<";
        output.append(definition.destination);
        output += ">)";
    };
    for (size_t i = 0; i < input.size();) {
        // An escaped character, "\[" among them, starts no reference.
        if (input[i] == '\\' && i + 1 < input.size()) {
            output.append(input.substr(i, 2));
            i += 2;
            continue;
        }
        if (input[i] == '`') {
            size_t run = DelimiterRun(input, i, '`');
            size_t end = FindExactDelimiterRun(input, i + run, '`', run);
            if (end == std::string_view::npos) {
                output.append(input.substr(i));
                break;
            }
            output.append(input.substr(i, end + run - i));
            i = end + run;
            continue;
        }
        size_t labelStart = i;
        bool image = input[i] == '!' && i + 1 < input.size() && input[i + 1] == '[';
        if (image) labelStart++;
        if (input[labelStart] != '[') {
            // This byte starts nothing, nor do those up to the next that may be syntax ('\\',
            // '`', "![" and '[' among them): copied as one run, not a push_back each.
            const size_t next = RayoMd::Text::FindByteClass<RayoMd::Text::kByteInlineSyntax>(input, i + 1);
            output.append(input.data() + i, next - i);
            i = next;
            continue;
        }
        size_t close = nextCloseBracket(labelBracket, labelStart + 1);
        if (close == std::string_view::npos || (close + 1 < input.size() && input[close + 1] == '(')) {
            output.push_back(input[i++]);
            continue;
        }
        size_t cursor = close + 1;
        while (cursor < input.size() && (input[cursor] == ' ' || input[cursor] == '\t')) cursor++;
        if (cursor >= input.size() || input[cursor] != '[') {
            // A shortcut reference, "[label]" alone, when the label is defined. A label holds no
            // '[', so the labels tried never overlap: "[[[[a]" looks up "a" alone, once.
            const std::string_view label = input.substr(labelStart + 1, close - labelStart - 1);
            const auto found = label.empty() || input.find('[', labelStart + 1) < close
                ? definitions.end()
                : definitions.find(NormalizeReferenceLabel(label));
            if (found == definitions.end() || !affordable(found->second)) {
                output.push_back(input[i++]);
                continue;
            }
            appendLink(image, label, found->second);
            i = close + 1;
            continue;
        }
        size_t referenceClose = nextCloseBracket(referenceBracket, cursor + 1);
        if (referenceClose == std::string_view::npos) {
            output.push_back(input[i++]);
            continue;
        }
        std::string_view visible = input.substr(labelStart + 1, close - labelStart - 1);
        std::string_view referenceLabel = input.substr(cursor + 1, referenceClose - cursor - 1);
        if (referenceLabel.empty()) referenceLabel = visible;
        auto found = definitions.find(NormalizeReferenceLabel(referenceLabel));
        if (found == definitions.end() || !affordable(found->second)) {
            size_t unresolvedEnd = referenceClose + 1;
            output.append(input.substr(i, unresolvedEnd - i));
            i = unresolvedEnd;
            continue;
        }
        appendLink(image, visible, found->second);
        i = referenceClose + 1;
    }
    return output;
}

// Link text with inline syntax, parsed on its own and appended in runs that link to
// out.urls from `urlBegin` on. The text ends at the first ']', so it holds no other link;
// a linked image's text is its image. Out of the parse loop: most link text is plain.
static RAYOMD_NOINLINE void AppendLinkText(std::string_view label, RunWriter& writer, size_t urlBegin, bool bold,
    bool italic, bool strike) {
    InlineRuns runs;
    ParseInlineRuns(label, runs, false);
    writer.LinkText(runs, urlBegin, bold, italic, strike);
}

std::string NormalizeSymbols(std::string text) {
    size_t at = text.find('\xE2');
    if (at == std::string::npos) return text;
    // One pass into a new string: replacing in place moved the rest of the text for every symbol,
    // quadratic in a paragraph of them. Each symbol starts with the only 0xE2 it holds and
    // becomes ASCII, so one pass finds what replacing symbol by symbol found.
    std::string out;
    size_t copied = 0;
    for (; at != std::string::npos; at = text.find('\xE2', at)) {
        const std::string_view rest(text.data() + at, text.size() - at);
        std::string_view replacement;
        size_t length = 3;
        if (rest.compare(0, 3, "\xE2\x9C\x85") == 0) {
            replacement = "[OK]";
        } else if (rest.compare(0, 3, "\xE2\x9A\xA0") == 0) {
            replacement = "[!]";
            if (rest.compare(3, 3, "\xEF\xB8\x8F") == 0) length = 6;
        } else if (rest.compare(0, 3, "\xE2\x9D\x8C") == 0) {
            replacement = "[X]";
        } else {
            at++;
            continue;
        }
        if (copied == 0) out.reserve(text.size() + text.size() / 3);
        out.append(text, copied, at - copied);
        out.append(replacement);
        at += length;
        copied = at;
    }
    if (copied == 0) return text;
    out.append(text, copied, std::string::npos);
    return out;
}

void ParseInlineRuns(std::string_view input, InlineRuns& out, bool recognizeMath, size_t lookaheadBudget, bool maySymbols) {
    // Only text with a status symbol needs a normalised copy; all other text is read in place.
    std::string normalized;
    std::string_view source = input;
    if (maySymbols && input.find('\xE2') != std::string_view::npos) {
        normalized = NormalizeSymbols(std::string(input));
        source = normalized;
    }
    // Text without a dollar sign or "\(" cannot contain a formula: skip every math
    // check below, including the lookup HasClosingDelimiter makes for each emphasis.
    if (recognizeMath && source.find('$') == std::string_view::npos && source.find("\\(") == std::string_view::npos) {
        recognizeMath = false;
    }
    InlineScanner scan(source, lookaheadBudget);
    RunWriter writer(out);
    std::string& text = out.text;
    if (text.capacity() < source.size()) text.reserve(source.size() + 16);
    bool bold = false;
    bool italic = false;
    bool strike = false;

    auto flush = [&]() { writer.Flush(bold, italic, strike); };
    // What MatchMathAt recognised: a formula, or, for a candidate over its size cap, the
    // delimited source byte for byte as a code span.
    auto pushMath = [&](const MathMatch& match) {
        flush();
        text.append(source.data() + match.contentBegin, match.contentEnd - match.contentBegin);
        if (match.verbatim) writer.Close(false, false, false, out.urls.size(), true);
        else writer.Math(match.display);
    };
    const auto& classes = RayoMd::Text::Detail::kByteClasses;
    // A bare URL or email address is found at its ':' or '@' (kStops). Both are rare in text, and
    // the stop costs less than looking for "://" and '@' in every text first.
    auto ordinary = [&](size_t index) {
        return (classes[(unsigned char)source[index]] & kStops) == 0;
    };

    for (size_t i = 0; i < source.size();) {
        if (ordinary(i)) {
            // A run of bytes none of which can start inline syntax: taken in one append.
            const size_t end = FindStops(source, i + 1);
            text.append(source.data() + i, end - i);
            i = end;
            continue;
        }
        if (source[i] == '&') {
            uint32_t codePoint = 0;
            const size_t length = MatchCharacterReference(source, i, codePoint);
            if (length != 0) {
                AppendReference(text, codePoint);
                i += length;
            } else {
                text.push_back('&');
                i++;
            }
            continue;
        }
        if (source[i] == '!' && i + 1 < source.size() && source[i + 1] == '[') {
            InlineLink image;
            if (ParseLinkAt(scan, i, 2, image)) {
                text += "image: ";
                AppendDecoded(text, image.label);
                i = image.end;
                continue;
            }
        }
        if (source[i] == '[') {
            // "[![" either starts a linked image or is a '[' before an image.
            const bool imageText = i + 2 < source.size() && source[i + 1] == '!' && source[i + 2] == '[';
            InlineLink link;
            if (imageText ? ParseLinkedImageAt(scan, i, link) : ParseLinkAt(scan, i, 1, link)) {
                flush();
                const size_t urlBegin = out.urls.size();
                AppendDestination(link.target, out.urls);
                if (RayoMd::Text::ContainsByteClass(link.label, RayoMd::Text::kByteInlineSyntax)) {
                    AppendLinkText(link.label, writer, urlBegin, bold, italic, strike);
                } else {
                    text.append(link.label.data(), link.label.size());
                    writer.Close(bold, italic, strike, urlBegin, false);
                }
                i = link.end;
                continue;
            }
            if (tFootnoteLabels != nullptr && i + 1 < source.size() && source[i + 1] == '^') {
                const size_t end = FootnoteReferenceEnd(source, i);
                if (end != std::string_view::npos) {
                    flush();
                    text.append(source.data() + i + 2, end - i - 3);
                    writer.Note();
                    i = end;
                    continue;
                }
            }
        }
        if (source[i] == '`') {
            size_t run = scan.RunLength(i);
            size_t end = scan.BacktickClose(i + run, run);
            if (end != std::string::npos) {
                flush();
                const size_t begin = text.size();
                text.append(source.data() + i + run, end - i - run);
                for (size_t at = begin; at < text.size(); at++) {
                    if (text[at] == '\n' || text[at] == '\r') text[at] = ' ';
                }
                if (text.size() - begin >= 2 && text[begin] == ' ' && text.back() == ' ' &&
                    text.find_first_not_of(' ', begin) != std::string::npos) {
                    text.erase(begin, 1);
                    text.pop_back();
                }
                writer.Close(false, false, false, out.urls.size(), true);
                i = end + run;
                continue;
            }
        }
        if (source[i] == '<') {
            // An HTML comment is left out, with the spaces after it when a space comes before.
            // "<!-->" and "<!--->" are comments too.
            if (i + 3 < source.size() && source[i + 1] == '!' && source[i + 2] == '-' && source[i + 3] == '-') {
                const size_t close = scan.NextCommentClose(i + 2);
                if (close != std::string_view::npos) {
                    i = close + 3;
                    if (text.empty() || text.back() == ' ') {
                        while (i < source.size() && source[i] == ' ') i++;
                    }
                    continue;
                }
            }
            size_t end = scan.NextGreater(i + 1);
            if (end != std::string::npos) {
                std::string_view target(source.data() + i + 1, end - i - 1);
                if (IsBreakTag(target)) {
                    flush();
                    writer.Break();
                    i = end + 1;
                    continue;
                }
                const bool web = target.size() > 7 && (target.substr(0, 7) == "http://" ||
                    (target.size() > 8 && target.substr(0, 8) == "https://"));
                if (web || LooksLikeEmail(target)) {
                    flush();
                    const size_t urlBegin = out.urls.size();
                    if (!web) out.urls += "mailto:";
                    out.urls.append(target.data(), target.size());
                    text.append(target.data(), target.size());
                    writer.Close(bold, italic, strike, urlBegin, false);
                    i = end + 1;
                    continue;
                }
                const size_t tagEnd = WriteHtmlTag(scan, writer, out, i, bold, italic, strike);
                if (tagEnd != 0) {
                    i = tagEnd;
                    continue;
                }
            }
        }
        if (source[i] == '$' && recognizeMath) {
            MathMatch math;
            if (MatchMathAt(scan, i, math)) {
                pushMath(math);
                i = math.end;
                continue;
            }
            // Not math: keep the dollar(s) as text. An unmatched "$$" and an empty
            // "$$ $$" are skipped as a whole so their dollars are never retried
            // as openers.
            text.append(source.data() + i, math.end - i);
            i = math.end;
            continue;
        }
        if (source[i] == '\\' && i + 1 < source.size()) {
            char next = source[i + 1];
            if (recognizeMath && next == '$') {
                text.push_back('$');
                i += 2;
                continue;
            }
            if (recognizeMath && next == '(') {
                MathMatch math;
                if (MatchMathAt(scan, i, math)) {
                    pushMath(math);
                    i = math.end;
                    continue;
                }
            }
            if (IsClassicEscapable(next)) {
                text.push_back(next);
                i += 2;
                continue;
            }
        }
        if (source[i] == '*' || source[i] == '_') {
            char marker = source[i];
            size_t run = scan.RunLength(i);
            size_t length = std::min<size_t>(3, run);
            if (marker == '_' && IntrawordUnderscore(source, i, length)) {
                text.append(run, marker);
                i += run;
                continue;
            }
            bool active = length == 3 ? bold && italic : (length == 2 ? bold : italic);
            bool canClose = i > 0 && !IsSpace(source[i - 1]);
            bool canOpen = i + length < source.size() && !IsSpace(source[i + length]);
            if ((active && canClose) || (canOpen && scan.HasClosingDelimiter(i + length, marker, length, recognizeMath))) {
                flush();
                if (length == 3) {
                    bold = !bold;
                    italic = !italic;
                } else if (length == 2) {
                    bold = !bold;
                } else {
                    italic = !italic;
                }
                i += length;
                continue;
            }
            text.append(length, marker);
            i += length;
            continue;
        }
        if (source[i] == ':' || source[i] == '@') {
            // A bare URL or email address, found at its ':' or '@'. Most ':' are followed by no
            // '/'; a ':' or '@' that starts no address is text.
            if (source[i] == '@' || (i + 1 < source.size() && source[i + 1] == '/')) {
                const size_t end = WriteAutolink(writer, out, source, i, bold, italic, strike);
                if (end != 0) {
                    i = end;
                    continue;
                }
            }
            text.push_back(source[i++]);
            continue;
        }
        if (i + 1 < source.size() && source.compare(i, 2, "~~") == 0) {
            bool canOpen = i + 2 < source.size() && !IsSpace(source[i + 2]);
            bool canClose = i > 0 && !IsSpace(source[i - 1]);
            if ((strike && canClose) || (canOpen && source.find("~~", i + 2) != std::string::npos)) {
                flush();
                strike = !strike;
                i += 2;
                continue;
            }
        }
        text.push_back(source[i++]);
    }

    flush();
}

std::string_view StripInlineMarkdownView(std::string_view input, InlineRuns& scratch, bool recognizeMath) {
    if (!RayoMd::Text::ContainsByteClass(input, RayoMd::Text::kByteInlineSyntax | RayoMd::Text::kByteSymbolLead)) {
        return TrimView(input);
    }
    ParseInlineRuns(input, scratch, recognizeMath);
    return TrimView(scratch.text);
}

namespace {
struct ScratchSlot {
    InlineRuns runs;
    bool busy = false;
};
ScratchSlot& ThreadScratch() {
    RAYOMD_THREAD_OBJECT(ScratchSlot, slot);   // never a plain thread_local: MinGW (thread_object.h)
    return slot;
}
RAYOMD_COLD void ReleaseRuns(InlineRuns& runs) {
    runs = InlineRuns();
}
} // namespace

ScratchRuns::ScratchRuns() : runs(&own) {
    ScratchSlot& slot = ThreadScratch();
    if (slot.busy) return;
    slot.busy = true;
    runs = &slot.runs;
}

ScratchRuns::~ScratchRuns() {
    if (runs == &own) return;
    constexpr size_t kKeptBytes = 1u << 20;
    if (runs->text.capacity() + runs->urls.capacity() + runs->runs.capacity() * sizeof(InlineRun) > kKeptBytes) {
        ReleaseRuns(*runs);
    }
    ThreadScratch().busy = false;
}

std::vector<InlineSpan> ParseInlineSpans(std::string_view input, bool recognizeMath, size_t lookaheadBudget) {
    InlineRuns runs;
    ParseInlineRuns(input, runs, recognizeMath, lookaheadBudget);
    std::vector<InlineSpan> spans(runs.runs.size());
    for (size_t i = 0; i < spans.size(); i++) {
        const InlineRun& run = runs.runs[i];
        const std::string_view text = runs.Text(run);
        InlineSpan& span = spans[i];
        span.text.assign(text.data(), text.size());
        // The runs of one link share its target in runs.urls: its spans share one string, where
        // a copy each made a long target over text of alternating styles gigabytes.
        if (i > 0 && run.urlBegin == runs.runs[i - 1].urlBegin && run.urlEnd == runs.runs[i - 1].urlEnd) {
            span.url = spans[i - 1].url;
        } else {
            span.url = SharedUrl(runs.Url(run));
        }
        span.bold = run.bold;
        span.italic = run.italic;
        span.strike = run.strike;
        span.code = run.code;
        span.lineBreak = run.lineBreak;
        span.decor = run.decor;
        span.math = run.math;
    }
    return spans;
}

} // namespace TinyPdf::Internal