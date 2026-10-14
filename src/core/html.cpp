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

// Six bits a character, at most ten: 0 for a name no tag can have.
constexpr uint64_t NameKey(std::string_view name) {
    if (name.empty() || name.size() > 10) return 0;
    uint64_t key = 0;
    for (char c : name) {
        const int code = NameCode(c);
        if (code < 0) return 0;
        key = (key << 6) | static_cast<uint64_t>(code);
    }
    return key;
}

struct KeyedTag {
    uint64_t key;
    Tag tag;
};

constexpr std::array<KeyedTag, kTagCount - 1> MakeTagKeys() {
    std::array<KeyedTag, kTagCount - 1> keys{};
    for (size_t index = 1; index < kTagCount; index++) keys[index - 1] = { NameKey(kTagNames[index]), static_cast<Tag>(index) };
    for (size_t index = 1; index < keys.size(); index++) {
        const KeyedTag moving = keys[index];
        size_t at = index;
        for (; at > 0 && keys[at - 1].key > moving.key; at--) keys[at] = keys[at - 1];
        keys[at] = moving;
    }
    return keys;
}
constexpr std::array<KeyedTag, kTagCount - 1> kTagKeys = MakeTagKeys();

constexpr bool KeysAscend() {
    for (size_t index = 1; index < kTagKeys.size(); index++) {
        if (kTagKeys[index - 1].key >= kTagKeys[index].key) return false;
    }
    return kTagKeys[0].key != 0;
}
static_assert(KeysAscend(), "every tag name has a key of its own");

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

// Keeps the value of an attribute RayoMD uses; of a repeated one, the first, as browsers do.
void KeepAttribute(TagToken& token, std::string_view name, std::string_view value, uint16_t& seen) {
    static constexpr std::string_view kNames[] = { "align", "width", "height", "src", "alt", "href", "start", "id",
        "name", "open" };
    std::string_view TagToken::*const kFields[] = { &TagToken::align, &TagToken::width, &TagToken::height,
        &TagToken::src, &TagToken::alt, &TagToken::href, &TagToken::start, &TagToken::id, &TagToken::name,
        &TagToken::open };
    if (name.size() < 2 || name.size() > 6) return;
    for (size_t index = 0; index < std::size(kNames); index++) {
        if ((seen >> index) & 1 || !EqualsNoCase(name, kNames[index])) continue;
        seen = static_cast<uint16_t>(seen | (1u << index));
        token.*kFields[index] = value;
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
    if (key == 0) return Tag::Unknown;
    const auto found = std::lower_bound(kTagKeys.begin(), kTagKeys.end(), key,
        [](const KeyedTag& entry, uint64_t wanted) { return entry.key < wanted; });
    return found != kTagKeys.end() && found->key == key ? found->tag : Tag::Unknown;
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
    token = TagToken();
    if (at >= text.size() || text[at] != '<') return 0;
    const size_t limit = std::min(text.size(), at + kMaxTagBytes);
    size_t index = at + 1;
    if (index < limit && text[index] == '/') {
        token.closing = true;
        index++;
    }
    const size_t nameStart = index;
    if (index >= limit || !IsLetter(text[index])) return 0;
    for (index++; index < limit && (IsLetter(text[index]) || IsDigit(text[index]) || text[index] == '-'); index++) {}
    token.tag = LookupTag(text.substr(nameStart, index - nameStart));
    if (token.closing) {
        index = SkipSpace(text, index, limit);
        if (index >= limit || text[index] != '>') return 0;
        token.length = static_cast<uint32_t>(index + 1 - at);
        return token.length;
    }
    uint16_t seen = 0;
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
        KeepAttribute(token, name, value, seen);
    }
    token.length = static_cast<uint32_t>(index + 1 - at);
    return token.length;
}

int HtmlBlockStart(std::string_view line) {
    if (line.size() < 2 || line[0] != '<') return 0;
    const std::string_view rest = line.substr(1);
    for (std::string_view name : { std::string_view("pre"), std::string_view("script"), std::string_view("style"),
             std::string_view("textarea") }) {
        if (!StartsWithNoCase(rest, name)) continue;
        if (rest.size() == name.size()) return 1;
        const char after = rest[name.size()];
        if (after == ' ' || after == '\t' || after == '>') return 1;
    }
    if (line.compare(0, 4, "<!--") == 0) return 2;
    if (line[1] == '?') return 3;
    if (line[1] == '!' && line.size() > 2 && IsLetter(line[2])) return 4;
    if (line.compare(0, 9, "<![CDATA[") == 0) return 5;
    size_t index = line[1] == '/' ? 2 : 1;
    const size_t nameStart = index;
    while (index < line.size() && (IsLetter(line[index]) || IsDigit(line[index]))) index++;
    if (IsBlockName(LookupTag(line.substr(nameStart, index - nameStart))) &&
        (index == line.size() || line[index] == ' ' || line[index] == '\t' || line[index] == '>' ||
            (line[index] == '/' && index + 1 < line.size() && line[index + 1] == '>'))) {
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

bool SafeUrl(std::string_view url, bool image) {
    size_t index = 0;
    while (index < url.size() && static_cast<unsigned char>(url[index]) <= 0x20) index++;
    char scheme[11];
    size_t length = 0;
    for (; index < url.size() && length < sizeof(scheme); index++) {
        const char c = url[index];
        if (c == '\t' || c == '\n' || c == '\r') continue;
        scheme[length++] = Lower(c);
    }
    const std::string_view start(scheme, length);
    if (start.compare(0, 11, "javascript:") == 0 || start.compare(0, 9, "vbscript:") == 0) return false;
    if (start.compare(0, 5, "data:") != 0) return true;
    return image && start.compare(0, 11, "data:image/") == 0;
}

} // namespace TinyPdf::Internal::Html
