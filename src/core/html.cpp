// SPDX-License-Identifier: Apache-2.0

#include "html.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>

namespace TinyPdf::Internal::Html {
namespace {

constexpr size_t kTagCount = static_cast<size_t>(Tag::Count);

// In the order of Tag.
constexpr std::string_view kTagNames[] = {
    "",
    "h1", "h2", "h3", "h4", "h5", "h6", "br", "b", "i", "strong", "em", "a", "pre", "code", "img", "tt", "div",
    "ins", "del", "sup", "sub", "p", "picture", "ol", "ul", "table", "thead", "tbody", "tfoot", "blockquote",
    "dl", "dt", "dd", "kbd", "q", "samp", "var", "hr", "ruby", "rt", "rp", "li", "tr", "td", "th", "s", "strike",
    "summary", "details", "caption", "figure", "figcaption", "abbr", "bdo", "cite", "dfn", "mark", "small",
    "source", "span", "time", "wbr",
    "u", "center",
    "address", "article", "aside", "base", "basefont", "body", "col", "colgroup", "dialog", "dir", "fieldset",
    "footer", "form", "frame", "frameset", "head", "header", "html", "iframe", "legend", "link", "main", "menu",
    "menuitem", "nav", "noframes", "optgroup", "option", "param", "search", "section", "title", "track",
    "script", "style", "template", "textarea", "noscript", "object", "embed", "svg", "math",
};
static_assert(std::size(kTagNames) == kTagCount, "a name for every tag");

// A letter's code is 1-26 in either case, a digit's 27-36; -1 for anything else.
constexpr int NameCode(char c) {
    return c >= 'a' && c <= 'z' ? c - 'a' + 1
        : c >= 'A' && c <= 'Z' ? c - 'A' + 1
        : c >= '0' && c <= '9' ? c - '0' + 27
        : -1;
}

constexpr std::array<int8_t, 256> MakeNameCodes() {
    std::array<int8_t, 256> codes{};
    for (size_t index = 0; index < codes.size(); index++) codes[index] = static_cast<int8_t>(NameCode(static_cast<char>(index)));
    return codes;
}
constexpr std::array<int8_t, 256> kNameCodes = MakeNameCodes();

// Six bits a character, at most ten: 0 for a name no tag can have.
constexpr uint64_t NameKey(std::string_view name) {
    if (name.empty() || name.size() > 10) return 0;
    uint64_t key = 0;
    for (char c : name) {
        const int code = kNameCodes[static_cast<unsigned char>(c)];
        if (code < 0) return 0;
        key = (key << 6) | static_cast<uint64_t>(code);
    }
    return key;
}

// The keys in an open-addressed table of 256 slots, under half full: a name is found, or known to
// be no tag's, in one or two probes.
constexpr size_t kTagSlots = 256;

constexpr size_t TagSlot(uint64_t key) {
    return static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 56);
}

struct TagTable {
    uint64_t keys[kTagSlots];
    Tag tags[kTagSlots];
};

constexpr TagTable MakeTagTable() {
    TagTable table{};
    for (size_t index = 1; index < kTagCount; index++) {
        const uint64_t key = NameKey(kTagNames[index]);
        size_t slot = TagSlot(key);
        while (table.keys[slot] != 0) slot = (slot + 1) % kTagSlots;
        table.keys[slot] = key;
        table.tags[slot] = static_cast<Tag>(index);
    }
    return table;
}
constexpr TagTable kTagTable = MakeTagTable();

constexpr bool KeysDistinct() {
    for (size_t index = 1; index < kTagCount; index++) {
        if (NameKey(kTagNames[index]) == 0) return false;
        for (size_t other = 1; other < index; other++) {
            if (NameKey(kTagNames[other]) == NameKey(kTagNames[index])) return false;
        }
    }
    return kTagCount < kTagSlots / 2;
}
static_assert(KeysDistinct(), "every tag name has a key of its own");

// The tag whose name has `key` (not 0), or Tag::Unknown.
inline Tag TagForKey(uint64_t key) {
    for (size_t slot = TagSlot(key);; slot = (slot + 1) % kTagSlots) {
        if (kTagTable.keys[slot] == key) return kTagTable.tags[slot];
        if (kTagTable.keys[slot] == 0) return Tag::Unknown;
    }
}

constexpr bool IsLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool IsDigit(char c) { return c >= '0' && c <= '9'; }
constexpr bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
constexpr bool IsAttributeNameStart(char c) { return IsLetter(c) || c == '_' || c == ':'; }
constexpr bool IsAttributeNameChar(char c) { return IsAttributeNameStart(c) || IsDigit(c) || c == '.' || c == '-'; }
constexpr bool IsUnquotedValueChar(char c) {
    return !IsSpace(c) && c != '"' && c != '\'' && c != '=' && c != '<' && c != '>' && c != '`';
}
constexpr char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsNoCase(std::string_view text, std::string_view lower) {
    if (text.size() != lower.size()) return false;
    for (size_t index = 0; index < text.size(); index++) {
        if (Lower(text[index]) != lower[index]) return false;
    }
    return true;
}

bool StartsWithNoCase(std::string_view text, std::string_view lower) {
    return text.size() >= lower.size() && EqualsNoCase(text.substr(0, lower.size()), lower);
}

size_t SkipSpace(std::string_view text, size_t at, size_t limit) {
    while (at < limit && IsSpace(text[at])) at++;
    return at;
}

// The values RayoMD keeps, in the order of TagToken::seen.
constexpr std::string_view kAttributeNames[] = { "align", "width", "height", "src", "alt", "href", "start", "id",
    "name", "open", "style", "class" };
constexpr std::string_view TagToken::*kAttributeFields[] = { &TagToken::align, &TagToken::width, &TagToken::height,
    &TagToken::src, &TagToken::alt, &TagToken::href, &TagToken::start, &TagToken::id, &TagToken::name,
    &TagToken::open, &TagToken::style, &TagToken::classes };
static_assert(std::size(kAttributeNames) == std::size(kAttributeFields), "a field for every name");

// Keeps the value of an attribute RayoMD uses; of a repeated one, the first, as browsers do.
void KeepAttribute(TagToken& token, std::string_view name, std::string_view value) {
    if (name.size() < 2 || name.size() > 6) return;
    for (size_t index = 0; index < std::size(kAttributeNames); index++) {
        if ((token.seen >> index) & 1 || !EqualsNoCase(name, kAttributeNames[index])) continue;
        token.seen = static_cast<uint16_t>(token.seen | (1u << index));
        token.*kAttributeFields[index] = value;
        if (index == 9) token.hasOpen = true;
        return;
    }
}

} // namespace

std::string_view TagName(Tag tag) {
    const size_t index = static_cast<size_t>(tag);
    return index < kTagCount ? kTagNames[index] : std::string_view();
}

Tag LookupTag(std::string_view name) {
    const uint64_t key = NameKey(name);
    return key != 0 ? TagForKey(key) : Tag::Unknown;
}

bool IsBlockName(Tag tag) {
    switch (tag) {
    case Tag::H1: case Tag::H2: case Tag::H3: case Tag::H4: case Tag::H5: case Tag::H6:
    case Tag::Blockquote: case Tag::Caption: case Tag::Center: case Tag::Dd: case Tag::Details: case Tag::Div:
    case Tag::Dl: case Tag::Dt: case Tag::Figcaption: case Tag::Figure: case Tag::Hr: case Tag::Li: case Tag::Ol:
    case Tag::P: case Tag::Summary: case Tag::Table: case Tag::Tbody: case Tag::Td: case Tag::Tfoot: case Tag::Th:
    case Tag::Thead: case Tag::Tr: case Tag::Ul:
        return true;
    default:
        return tag >= Tag::Address && tag <= Tag::Track;
    }
}

bool IsDroppedWithContent(Tag tag) {
    return tag >= Tag::Script || tag == Tag::Head || tag == Tag::Iframe || tag == Tag::Title;
}

size_t QuoteCache::Next(std::string_view text, size_t from, char quote) {
    if (text.data() != base || text.size() != size) {
        base = text.data();
        size = text.size();
        known[0] = known[1] = false;
    }
    if (from >= text.size()) return text.size();
    const int kind = quote == '"' ? 0 : 1;
    if (known[kind] && from >= searchedFrom[kind]) {
        if (from <= found[kind]) return found[kind];
    } else if (known[kind]) {
        // Before the last search: a quote between `from` and where that search began, else its answer.
        const void* hit = memchr(text.data() + from, quote, searchedFrom[kind] - from);
        return hit != nullptr ? static_cast<size_t>(static_cast<const char*>(hit) - text.data()) : found[kind];
    }
    const void* hit = memchr(text.data() + from, quote, text.size() - from);
    known[kind] = true;
    searchedFrom[kind] = from;
    found[kind] = hit != nullptr ? static_cast<size_t>(static_cast<const char*>(hit) - text.data()) : text.size();
    return found[kind];
}

uint32_t ScanTag(std::string_view text, size_t at, QuoteCache& quotes, TagToken& token) {
    // Only the values the token has are cleared (TagToken::seen): clearing the whole token took a
    // 168-byte memset per tag.
    for (uint16_t bits = token.seen, index = 0; bits != 0; bits = static_cast<uint16_t>(bits >> 1), index++) {
        if (bits & 1) token.*kAttributeFields[index] = std::string_view();
    }
    token.tag = Tag::Unknown;
    token.closing = token.selfClosing = token.hasOpen = false;
    token.seen = 0;
    token.length = 0;
    if (at >= text.size() || text[at] != '<') return 0;
    const size_t limit = std::min(text.size(), at + kMaxTagBytes);
    size_t index = at + 1;
    if (index < limit && text[index] == '/') {
        token.closing = true;
        index++;
    }
    // The name, keyed as it is read (NameKey): letters, digits and '-', which no tag of the table has.
    if (index >= limit || !IsLetter(text[index])) return 0;
    const size_t nameStart = index;
    uint64_t key = 0;
    bool keyed = true;
    for (; index < limit; index++) {
        const int code = kNameCodes[static_cast<unsigned char>(text[index])];
        if (code < 0) {
            if (text[index] != '-') break;
            keyed = false;
            continue;
        }
        key = (key << 6) | static_cast<uint64_t>(code);
    }
    token.tag = keyed && index - nameStart <= 10 ? TagForKey(key) : Tag::Unknown;
    if (token.closing) {
        index = SkipSpace(text, index, limit);
        if (index >= limit || text[index] != '>') return 0;
        token.length = static_cast<uint32_t>(index + 1 - at);
        return token.length;
    }
    size_t attributes = 0;
    for (;;) {
        const size_t gap = index;
        index = SkipSpace(text, index, limit);
        if (index >= limit) return 0;
        if (text[index] == '>') break;
        if (text[index] == '/') {
            if (index + 1 >= limit || text[index + 1] != '>') return 0;
            token.selfClosing = true;
            index++;
            break;
        }
        // An attribute: white space before it, a name, and perhaps "=" and a value.
        if (index == gap || !IsAttributeNameStart(text[index]) || ++attributes > kMaxTagAttributes) return 0;
        const size_t attributeStart = index;
        for (index++; index < limit && IsAttributeNameChar(text[index]); index++) {}
        const std::string_view name = text.substr(attributeStart, index - attributeStart);
        std::string_view value;
        size_t next = SkipSpace(text, index, limit);
        if (next < limit && text[next] == '=') {
            next = SkipSpace(text, next + 1, limit);
            if (next >= limit) return 0;
            const char first = text[next];
            if (first == '"' || first == '\'') {
                const size_t close = quotes.Next(text, next + 1, first);
                if (close >= limit) return 0;
                value = text.substr(next + 1, close - next - 1);
                index = close + 1;
            } else {
                const size_t valueStart = next;
                while (next < limit && IsUnquotedValueChar(text[next])) next++;
                if (next == valueStart) return 0;
                value = text.substr(valueStart, next - valueStart);
                index = next;
            }
        }
        KeepAttribute(token, name, value);
    }
    token.length = static_cast<uint32_t>(index + 1 - at);
    return token.length;
}

int HtmlBlockStart(std::string_view line) {
    if (line.size() < 2 || line[0] != '<') return 0;
    const char second = line[1];
    if (second == '!') {
        if (line.substr(0, 4) == std::string_view("<!--", 4)) return 2;
        if (line.size() > 2 && IsLetter(line[2])) return 4;
        return line.substr(0, 9) == std::string_view("<![CDATA[", 9) ? 5 : 0;
    }
    if (second == '?') return 3;
    size_t index = second == '/' ? 2 : 1;
    const size_t nameStart = index;
    while (index < line.size() && (IsLetter(line[index]) || IsDigit(line[index]))) index++;
    const Tag tag = LookupTag(line.substr(nameStart, index - nameStart));
    const bool nameEnds = index == line.size() || line[index] == ' ' || line[index] == '\t' || line[index] == '>';
    if (second != '/' && nameEnds &&
        (tag == Tag::Pre || tag == Tag::Script || tag == Tag::Style || tag == Tag::Textarea)) {
        return 1;
    }
    if (IsBlockName(tag) && (nameEnds || (line[index] == '/' && index + 1 < line.size() && line[index + 1] == '>'))) {
        return 6;
    }
    QuoteCache quotes;
    TagToken token;
    const size_t length = ScanTag(line, 0, quotes, token);
    if (length == 0) return 0;
    if (!token.closing && (token.tag == Tag::Pre || token.tag == Tag::Script || token.tag == Tag::Style ||
                              token.tag == Tag::Textarea)) {
        return 0;
    }
    for (size_t after = length; after < line.size(); after++) {
        if (line[after] != ' ' && line[after] != '\t') return 0;
    }
    return 7;
}

bool HtmlBlockEnds(std::string_view line, int condition) {
    switch (condition) {
    case 1:
        for (size_t at = line.find("</"); at != std::string_view::npos; at = line.find("</", at + 2)) {
            const std::string_view tail = line.substr(at + 2);
            for (std::string_view name : { std::string_view("pre>"), std::string_view("script>"),
                     std::string_view("style>"), std::string_view("textarea>") }) {
                if (StartsWithNoCase(tail, name)) return true;
            }
        }
        return false;
    case 2: return line.find("-->") != std::string_view::npos;
    case 3: return line.find("?>") != std::string_view::npos;
    case 4: return line.find('>') != std::string_view::npos;
    case 5: return line.find("]]>") != std::string_view::npos;
    default:
        return std::all_of(line.begin(), line.end(), [](char c) { return c == ' ' || c == '\t'; });
    }
}

// The bytes a /URI leaves out (AppendUriLiteral in tiny_pdf.cpp): the C0 controls and DEL. The
// scheme is read without them, so the target checked is the one a viewer gets.
static bool DroppedFromUri(char c) {
    return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
}

bool SafeUrl(std::string_view url, bool image) {
    size_t index = 0;
    while (index < url.size() && (url[index] == ' ' || DroppedFromUri(url[index]))) index++;
    // Most targets ("https:", "mailto:", "#top", "page.md") are answered from two bytes: a first
    // byte that starts none of the refused forms below, then one that is no drive letter's ':'
    // and none of the bytes the scheme reading skips.
    if (index + 1 < url.size()) {
        const char first = Lower(url[index]);
        const char second = url[index + 1];
        if (first != 'j' && first != 'v' && first != 'd' && first != 'f' && first != '\\' && first != '/' &&
            second != ':' && !DroppedFromUri(second)) {
            return true;
        }
    }
    char scheme[11];
    size_t length = 0;
    for (; index < url.size() && length < sizeof(scheme); index++) {
        const char c = url[index];
        if (DroppedFromUri(c)) continue;
        scheme[length++] = Lower(c);
    }
    const std::string_view start(scheme, length);
    if (start.compare(0, 11, "javascript:") == 0 || start.compare(0, 9, "vbscript:") == 0) return false;
    if (!image) {
        // A link opens no local file: not file:, not a drive path ("C:\", "c:/"), and not a path
        // of two slashes with a backslash among them, which Windows takes for a network share
        // ("\\host\share"). An image's source has the local image policy (relative paths only).
        if (start.compare(0, 5, "file:") == 0) return false;
        if (length >= 3 && start[0] >= 'a' && start[0] <= 'z' && start[1] == ':' && (start[2] == '\\' || start[2] == '/')) {
            return false;
        }
        if (length >= 2 && (start[0] == '\\' || start[0] == '/') && (start[1] == '\\' || start[1] == '/') &&
            (start[0] == '\\' || start[1] == '\\')) {
            return false;
        }
    }
    if (start.compare(0, 5, "data:") != 0) return true;
    return image && start.compare(0, 11, "data:image/") == 0;
}

} // namespace TinyPdf::Internal::Html
