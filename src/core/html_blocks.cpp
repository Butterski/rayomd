// SPDX-License-Identifier: Apache-2.0

// HTML blocks (html.h's MapBlock and MapParagraph): what an HTML block of a document stands for
// as blocks. Run once per HTML block and per paragraph that starts with '<', '!' or '[', built at
// -Os apart from the tag scanner in html.cpp, which reads every tag of a document.
#include "html.h"
#include "inline_markdown.h"
#include "markdown_parser.h"
#include "../common/compiler.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace TinyPdf::Internal::Html {
namespace {

constexpr bool IsDigit(char c) { return c >= '0' && c <= '9'; }
constexpr bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
constexpr char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsNoCase(std::string_view text, std::string_view lower) {
    if (text.size() != lower.size()) return false;
    for (size_t index = 0; index < text.size(); index++) {
        if (Lower(text[index]) != lower[index]) return false;
    }
    return true;
}

size_t SkipSpace(std::string_view text, size_t at, size_t limit) {
    while (at < limit && IsSpace(text[at])) at++;
    return at;
}

} // namespace

uint8_t HtmlState::Align() const {
    for (size_t index = depth; index-- > 0;) {
        if (open[index].align != 0) return open[index].align;
    }
    return 0;
}

namespace {

constexpr std::string_view::size_type kNone = std::string_view::npos;
// Quotes, lists and tables inside each other, each mapped on its own: deeper ones are read as
// elements left open, whose content is mapped where it stands.
constexpr int kMaxNesting = 16;

// `value` with its references to ASCII characters decoded ("&amp;", "&quot;", "&#47;"): an
// attribute's or a <pre>'s meaning in either text encoding of the renderers. References to other
// characters stay as written.
std::string DecodeAscii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (size_t at = 0; at < value.size(); at++) {
        uint32_t codePoint = 0;
        const size_t length = value[at] == '&' ? MatchCharacterReference(value, at, codePoint) : 0;
        if (length != 0 && codePoint != 0 && codePoint < 0x80) {
            out.push_back(static_cast<char>(codePoint));
            at += length - 1;
            continue;
        }
        out.push_back(value[at]);
    }
    return out;
}

// Block::align of an align attribute: left and justify 1, center 2, right 3; 0 for others.
uint8_t AlignOf(std::string_view value) {
    if (value.empty()) return 0;
    const std::string decoded = value.find('&') != std::string_view::npos ? DecodeAscii(value) : std::string();
    const std::string_view name = decoded.empty() ? value : std::string_view(decoded);
    if (EqualsNoCase(name, "left") || EqualsNoCase(name, "justify")) return 1;
    if (EqualsNoCase(name, "center") || EqualsNoCase(name, "middle")) return 2;
    if (EqualsNoCase(name, "right")) return 3;
    return 0;
}

// A width or height attribute in points: "170" or "170px" are pixels, 0.75 points each; "50%"
// is a fraction of the text width (`percent`). 0 for anything else.
float LengthOf(std::string_view value, bool& percent) {
    percent = false;
    size_t at = 0;
    double number = 0.0;
    for (; at < value.size() && IsDigit(value[at]) && at < 9; at++) number = number * 10.0 + (value[at] - '0');
    if (at == 0) return 0.0f;
    if (at < value.size() && value[at] == '.') {
        double scale = 0.1;
        for (at++; at < value.size() && IsDigit(value[at]) && scale > 1e-6; at++, scale *= 0.1) {
            number += (value[at] - '0') * scale;
        }
    }
    const std::string_view unit = value.substr(at);
    if (unit == "%") {
        percent = true;
        return static_cast<float>(std::min(number, 100.0) / 100.0);
    }
    if (!unit.empty() && !EqualsNoCase(unit, "px")) return 0.0f;
    return static_cast<float>(number * 0.75);
}

// Where the first close tag of `tag` at or after `from` begins and ends; kNone for both when the
// text has none. For <pre> and the elements dropped with their content, which do not nest: the
// caller goes on behind the close tag, or at the end when there is none, so each byte is read once.
struct Range {
    size_t begin;
    size_t end;
};

Range FindClose(std::string_view html, size_t from, Tag tag) {
    QuoteCache quotes;
    TagToken token;
    for (size_t at = html.find('<', from); at != kNone; at = html.find('<', at + 1)) {
        const uint32_t length = ScanTag(html, at, quotes, token);
        if (length == 0) continue;
        if (token.tag == tag && token.closing) return { at, at + length };
        at += length - 1;
    }
    return { kNone, kNone };
}

// The close tag of each blockquote, ul, ol and table of an HTML block, found in one pass, made at
// the first question (most blocks hold none of these): a close tag ends the innermost open element
// of its kind. Mappers ask by the address of the open tag, so hundreds of such elements left open
// do not each search the rest of the block.
class Containers {
public:
    explicit Containers(std::string_view text) : html(text) {}

    // The close of the element whose open tag is at `tag`, when it ends at or before `limit`.
    bool Close(const char* tag, const char* limit, const char*& closeBegin, const char*& closeEnd) const {
        if (tag < html.data() || tag >= html.data() + html.size()) return false;
        if (!paired) MatchPairs();
        const size_t open = static_cast<size_t>(tag - html.data());
        const auto found = std::lower_bound(pairs.begin(), pairs.end(), open,
            [](const Pair& pair, size_t wanted) { return pair.open < wanted; });
        if (found == pairs.end() || found->open != open || html.data() + found->closeEnd > limit) return false;
        closeBegin = html.data() + found->closeBegin;
        closeEnd = html.data() + found->closeEnd;
        return true;
    }

private:
    struct Pair {
        size_t open;
        size_t closeBegin;
        size_t closeEnd;
    };

    static int Kind(Tag tag) {
        return tag == Tag::Blockquote ? 0 : tag == Tag::Ul ? 1 : tag == Tag::Ol ? 2 : tag == Tag::Table ? 3 : -1;
    }

    void MatchPairs() const {
        paired = true;
        QuoteCache quotes;
        TagToken token;
        std::vector<size_t> open[4];
        for (size_t at = html.find('<'); at != kNone; at = html.find('<', at + 1)) {
            const uint32_t length = ScanTag(html, at, quotes, token);
            if (length == 0) continue;
            const int kind = Kind(token.tag);
            if (kind >= 0 && !token.selfClosing) {
                if (!token.closing) {
                    open[kind].push_back(at);
                } else if (!open[kind].empty()) {
                    pairs.push_back({ open[kind].back(), at, at + length });
                    open[kind].pop_back();
                }
            }
            at += length - 1;
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.open < b.open; });
    }

    const std::string_view html;
    mutable bool paired = false;
    mutable std::vector<Pair> pairs;
};

// White space and <br> tags from `at` on: the index behind them.
size_t SkipGap(std::string_view text, size_t at, QuoteCache& quotes) {
    for (;;) {
        at = SkipSpace(text, at, text.size());
        TagToken token;
        const uint32_t length = at < text.size() && text[at] == '<' ? ScanTag(text, at, quotes, token) : 0;
        if (length == 0 || token.tag != Tag::Br || token.closing) return at;
        at += length;
    }
}

// One image at text[at], <img src> or Markdown's "![alt](src)" (not linked): the index behind
// it, or 0.
size_t OneImage(std::string_view text, size_t at, QuoteCache& quotes, ImageSource& image, std::string& alt) {
    if (at >= text.size()) return 0;
    if (text[at] == '!') return text.compare(at, 2, "![") == 0 ? ParseImageAt(text, at, alt, image) : 0;
    TagToken token;
    const uint32_t length = text[at] == '<' ? ScanTag(text, at, quotes, token) : 0;
    if (length == 0 || token.tag != Tag::Img || token.closing) return 0;
    image.src = DecodeAscii(token.src);
    if (image.src.empty() || !SafeUrl(image.src, true)) return 0;
    alt = StripInlineMarkdown(token.alt);
    bool percent = false;
    image.widthHint = LengthOf(token.width, percent);
    image.widthPercent = percent && image.widthHint > 0.0f;
    bool heightPercent = false;
    image.heightHint = LengthOf(token.height, heightPercent);
    if (heightPercent) image.heightHint = 0.0f;
    return at + length;
}

// A paragraph of nothing but images, each <img> or Markdown, each perhaps inside a link (<a
// href> or Markdown's "[![alt](src)](url)"), apart by white space or <br>: the first in `first`
// with the others in first.row, and its alt text in `alt`.
bool MatchImageRow(std::string_view text, ImageSource& first, std::string& alt) {
    QuoteCache quotes;
    std::vector<ImageSource> images;
    std::vector<std::string> alts;
    for (size_t at = SkipGap(text, 0, quotes); at < text.size(); at = SkipGap(text, at, quotes)) {
        ImageSource image;
        std::string imageAlt;
        size_t next = 0;
        TagToken token;
        if (text[at] == '[') {
            next = ParseImageAt(text, at, imageAlt, image);
        } else if (text[at] == '<' && ScanTag(text, at, quotes, token) != 0 && token.tag == Tag::A && !token.closing) {
            std::string link = DecodeAscii(token.href);
            if (!SafeUrl(link)) link.clear();
            next = OneImage(text, SkipSpace(text, at + token.length, text.size()), quotes, image, imageAlt);
            if (next == 0) return false;
            next = SkipSpace(text, next, text.size());
            TagToken close;
            if (next >= text.size() || ScanTag(text, next, quotes, close) == 0 || close.tag != Tag::A || !close.closing) {
                return false;
            }
            next += close.length;
            image.link = std::move(link);
        } else {
            next = OneImage(text, at, quotes, image, imageAlt);
        }
        if (next == 0) return false;
        images.push_back(std::move(image));
        alts.push_back(std::move(imageAlt));
        at = next;
    }
    if (images.empty()) return false;
    first = std::move(images[0]);
    alt = std::move(alts[0]);
    for (size_t index = 1; index < images.size(); index++) {
        images[index].alt = std::move(alts[index]);
        first.row.push_back(std::move(images[index]));
    }
    return true;
}

// A paragraph of nothing but <a id="..."></a> or <a name="...">, white space around: its id.
bool MatchAnchor(std::string_view text, std::string& id) {
    QuoteCache quotes;
    TagToken open;
    size_t at = SkipSpace(text, 0, text.size());
    if (at >= text.size() || ScanTag(text, at, quotes, open) == 0 || open.tag != Tag::A || open.closing) return false;
    const std::string_view name = !open.id.empty() ? open.id : open.name;
    if (name.empty() || open.href.data() != nullptr) return false;
    at = SkipSpace(text, at + open.length, text.size());
    if (!open.selfClosing) {
        TagToken close;
        if (at >= text.size() || ScanTag(text, at, quotes, close) == 0 || close.tag != Tag::A || !close.closing) return false;
        at = SkipSpace(text, at + close.length, text.size());
    }
    if (at != text.size()) return false;
    id = DecodeAscii(name);
    return true;
}

Block AnchorBlock(std::string id) {
    Block block;
    block.type = BlockType::Anchor;
    block.text = std::move(id);
    return block;
}

// What Text does with a byte: append it (kPlain), collapse it (kSpace), escape it with a backslash
// (kEscaped), write it as a character reference (kReference: '~' and '$'), or stop (kMarkup: '<').
enum : uint8_t { kPlain, kSpace, kEscaped, kReference, kMarkup };

constexpr std::array<uint8_t, 256> MakeTextClasses() {
    std::array<uint8_t, 256> classes{};
    for (const char ch : { ' ', '\t', '\n', '\r', '\f' }) classes[static_cast<unsigned char>(ch)] = kSpace;
    for (const char ch : { '\\', '`', '*', '_', '[', ']', '!' }) classes[static_cast<unsigned char>(ch)] = kEscaped;
    for (const char ch : { '~', '$' }) classes[static_cast<unsigned char>(ch)] = kReference;
    classes['<'] = kMarkup;
    return classes;
}
constexpr std::array<uint8_t, 256> kTextClasses = MakeTextClasses();

// One HTML block, or the content of an element of one, mapped to blocks (see MapBlock).
class BlockMapper {
public:
    BlockMapper(HtmlState& openState, std::vector<Block>& blocks, int depth, const Containers& closes)
        : state(openState), out(blocks), nesting(depth), containers(closes) {}

    void Map(std::string_view html) {
        paragraph.reserve(std::min<size_t>(html.size(), 512));
        for (size_t at = Text(html, 0); at < html.size(); at = Text(html, at)) at = Markup(html, at);
        Flush();
    }

private:
    // The text from html[at] to the next '<' (returned, or the end): white space collapsed to one
    // space, and the bytes the inline parser reads as Markdown escaped, as CommonMark shows the text
    // of HTML as it is. Bytes of no class, and single spaces between them, are appended a run at a time.
    RAYOMD_HOT_INLINE size_t Text(std::string_view html, size_t at) {
        while (at < html.size()) {
            size_t run = at;
            for (;;) {
                while (run < html.size() && kTextClasses[static_cast<unsigned char>(html[run])] == kPlain) run++;
                if (run == at || run + 1 >= html.size() || html[run] != ' ' ||
                    kTextClasses[static_cast<unsigned char>(html[run + 1])] != kPlain) {
                    break;
                }
                run++;
            }
            if (run != at) {
                Space();
                visible = true;
                paragraph.append(html.data() + at, run - at);
                if (run == html.size()) return run;
            }
            const char ch = html[run];
            const uint8_t kind = kTextClasses[static_cast<unsigned char>(ch)];
            if (kind == kMarkup) return run;
            at = run + 1;
            if (kind == kSpace) {
                space = true;
                continue;
            }
            Space();
            visible = true;
            if (kind == kEscaped) {
                const char escaped[2] = { '\\', ch };
                paragraph.append(escaped, 2);
            } else {
                paragraph += ch == '~' ? "&#126;" : "&#36;";
            }
        }
        return at;
    }

    // Markup that the inline parser reads (an inline tag, as written, on one line) or a literal.
    RAYOMD_HOT_INLINE void Inline(std::string_view markup) {
        Space();
        const size_t start = paragraph.size();
        paragraph.append(markup);
        for (size_t index = start; index < paragraph.size(); index++) {
            const char ch = paragraph[index];
            if (static_cast<unsigned char>(ch) <= '\r' && (ch == '\n' || ch == '\r' || ch == '\t')) paragraph[index] = ' ';
        }
    }

    // An <a> opened, with an href or without.
    void OpenLink(bool linked) {
        if (links < 64) linkBits = linked ? linkBits | (uint64_t{ 1 } << links) : linkBits & ~(uint64_t{ 1 } << links);
        links++;
    }

    RAYOMD_HOT_INLINE void Space() {
        if (space && paragraph.size() > contentStart) paragraph.push_back(' ');
        space = false;
    }

    // The paragraph or heading read so far as a block, after the anchors met in it.
    void Flush() {
        for (std::string& id : anchors) out.push_back(AnchorBlock(std::move(id)));
        anchors.clear();
        if (visible && paragraph.size() > contentStart) {
            const uint8_t align = paragraphAlign != 0 ? paragraphAlign : state.Align();
            const size_t first = out.size();
            if (heading != 0) {
                Block& block = Append(BlockType::Heading);
                block.level = heading;
                bool hasMath = false;
                block.text = StripInlineMarkdownKeepMath(paragraph, hasMath);
                block.hasMath = hasMath;
            } else if (!MapParagraph(paragraph, out)) {
                Append(BlockType::Paragraph).text = std::move(paragraph);
            }
            for (size_t index = first; index < out.size(); index++) {
                if (out[index].type != BlockType::Anchor) out[index].align = align;
            }
        }
        paragraph.clear();
        visible = false;
        contentStart = 0;
        space = false;
        paragraphAlign = 0;
    }

    Block& Append(BlockType type) {
        out.emplace_back();
        out.back().type = type;
        return out.back();
    }

    void Anchor(std::string_view id) {
        if (!id.empty()) out.push_back(AnchorBlock(DecodeAscii(id)));
    }

    void Push(Tag tag, uint8_t align) {
        if (state.depth < HtmlState::kMaxOpen) state.open[state.depth++] = { tag, align };
    }

    void Pop(Tag tag) {
        for (size_t index = state.depth; index-- > 0;) {
            if (state.open[index].tag == tag) {
                state.depth = index;
                return;
            }
        }
    }

    // What starts at html[at] == '<': a comment, declaration or processing instruction (left
    // out), a tag, or a '<' that is text. Returns the index behind it.
    size_t Markup(std::string_view html, size_t at) {
        if (at + 1 < html.size() && (html[at + 1] == '!' || html[at + 1] == '?')) {
            if (at + 3 < html.size() && html[at + 1] == '!' && html[at + 2] == '-' && html[at + 3] == '-') {
                const size_t close = html.find("-->", at + 2);
                return close == kNone ? html.size() : close + 3;
            }
            const size_t close = html.find('>', at);
            return close == kNone ? html.size() : close + 1;
        }
        TagToken& token = scratch;
        const uint32_t length = ScanTag(html, at, quotes, token);
        if (length == 0) {
            Inline("&lt;");
            visible = true;
            return at + 1;
        }
        const size_t end = at + length;
        const Tag tag = token.tag;
        const bool opens = !token.closing && !token.selfClosing;
        if (opens && IsDroppedWithContent(tag)) {
            const Range close = FindClose(html, end, tag);
            return close.end == kNone ? html.size() : close.end;
        }
        switch (tag) {
        case Tag::P: case Tag::Div: case Tag::Center: case Tag::Section: case Tag::Article: case Tag::Main:
        case Tag::Header: case Tag::Footer: case Tag::Nav: case Tag::Aside: case Tag::Figure:
            Flush();
            if (token.closing) {
                Pop(tag);
            } else {
                Anchor(token.id);
                if (opens) Push(tag, tag == Tag::Center ? 2 : AlignOf(token.align));
            }
            return end;
        case Tag::H1: case Tag::H2: case Tag::H3: case Tag::H4: case Tag::H5: case Tag::H6:
            Flush();
            heading = 0;
            if (!token.closing) {
                Anchor(token.id);
                heading = static_cast<int>(tag) - static_cast<int>(Tag::H1) + 1;
                if (AlignOf(token.align) != 0) paragraphAlign = AlignOf(token.align);
            }
            return end;
        case Tag::Hr:
            Flush();
            if (!token.closing) Append(BlockType::Rule);
            return end;
        case Tag::Pre:
            Flush();
            return token.closing || token.selfClosing ? end : Pre(html, end);
        case Tag::Blockquote: case Tag::Ul: case Tag::Ol: case Tag::Table: {
            Flush();
            if (token.closing) {
                Pop(tag);
                return end;
            }
            Anchor(token.id);
            if (!opens) return end;
            const char* closeBegin = nullptr;
            const char* closeEnd = nullptr;
            if (nesting >= kMaxNesting ||
                !containers.Close(html.data() + at, html.data() + html.size(), closeBegin, closeEnd)) {
                Push(tag, AlignOf(token.align));   // left open: its content shows where it stands
                return end;
            }
            const std::string_view inner = html.substr(end, static_cast<size_t>(closeBegin - html.data()) - end);
            if (tag == Tag::Blockquote) Quote(inner);
            else if (tag == Tag::Table) Table(inner, AlignOf(token.align));
            else List(inner, tag == Tag::Ol, token.start);
            return static_cast<size_t>(closeEnd - html.data());
        }
        case Tag::Dt: case Tag::Summary:
            Flush();
            if (!token.closing) {
                Anchor(token.id);
                paragraph = "<b>";
                contentStart = paragraph.size();
            }
            return end;
        case Tag::Caption: case Tag::Figcaption:
            Flush();
            if (!token.closing) {
                paragraph = "<i>";
                contentStart = paragraph.size();
                paragraphAlign = 2;
            }
            return end;
        case Tag::Picture: case Tag::Source:
            return end;
        case Tag::A:
            // An <a> without href is only a target: it and its close tag stay out of the text, so
            // that it does not keep a paragraph of images from being a row.
            if (token.closing) {
                bool linked = false;
                if (links != 0) {
                    links--;
                    linked = links >= 64 || ((linkBits >> links) & 1) != 0;
                }
                if (linked) Inline(std::string_view(html.data() + at, length));
                return end;
            }
            if (token.href.data() == nullptr) {
                const std::string_view id = !token.id.empty() ? token.id : token.name;
                if (!id.empty()) anchors.push_back(DecodeAscii(id));
                if (!token.selfClosing) OpenLink(false);
                return end;
            }
            if (!token.selfClosing) OpenLink(true);
            Inline(std::string_view(html.data() + at, length));
            return end;
        default:
            if (IsBlockName(tag)) {
                Flush();
                if (!token.closing) Anchor(token.id);
                return end;
            }
            if (tag == Tag::Img && !token.closing) visible = true;
            Inline(std::string_view(html.data() + at, length));
            return end;
        }
    }

    // <pre>: its content up to </pre> as a code block, tags left out, ASCII references decoded,
    // the line break after <pre> and the one before </pre> dropped.
    size_t Pre(std::string_view html, size_t from) {
        const Range close = FindClose(html, from, Tag::Pre);
        const std::string_view content = html.substr(from, (close.begin == kNone ? html.size() : close.begin) - from);
        std::string text;
        text.reserve(content.size());
        QuoteCache local;
        TagToken token;
        for (size_t at = 0; at < content.size();) {
            const uint32_t length = content[at] == '<' ? ScanTag(content, at, local, token) : 0;
            if (length != 0) {
                at += length;
                continue;
            }
            text.push_back(content[at++]);
        }
        text = DecodeAscii(text);
        if (text.compare(0, 2, "\r\n") == 0) text.erase(0, 2);
        else if (!text.empty() && text[0] == '\n') text.erase(0, 1);
        if (!text.empty() && text.back() == '\n') text.pop_back();
        if (!text.empty() && text.back() == '\r') text.pop_back();
        Append(BlockType::Code).text = std::move(text);
        return close.end == kNone ? html.size() : close.end;
    }

    void Quote(std::string_view inner) {
        HtmlState local;
        Block quote;
        quote.type = BlockType::Quote;
        BlockMapper(local, quote.children, nesting + 1, containers).Map(inner);
        if (!quote.children.empty()) out.push_back(std::move(quote));
    }

    // The items of a list, each <li> at this level up to its </li>, the next <li> or the end.
    void List(std::string_view inner, bool ordered, std::string_view startAttribute) {
        int number = 1;
        if (ordered && !startAttribute.empty()) {
            const std::string start = DecodeAscii(startAttribute);
            size_t at = start[0] == '-' ? 1 : 0;
            long value = 0;
            for (; at < start.size() && IsDigit(start[at]) && value < 100000000; at++) value = value * 10 + (start[at] - '0');
            if (at == start.size() && at > (start[0] == '-' ? 1u : 0u)) number = static_cast<int>(start[0] == '-' ? -value : value);
        }
        QuoteCache local;
        TagToken token;
        int lists = 0;
        size_t itemBegin = kNone;
        const auto item = [&](std::string_view content) {
            std::vector<Block> blocks;
            HtmlState itemState;
            BlockMapper(itemState, blocks, nesting + 1, containers).Map(content);
            Block& added = Append(ordered ? BlockType::Numbered : BlockType::Bullet);
            added.number = number++;
            if (!blocks.empty() && blocks.front().type == BlockType::Paragraph) {
                added.text = std::move(blocks.front().text);
                blocks.erase(blocks.begin());
            }
            added.children = std::move(blocks);
        };
        for (size_t at = inner.find('<'); at != kNone; at = inner.find('<', at + 1)) {
            const uint32_t length = ScanTag(inner, at, local, token);
            if (length == 0) continue;
            if (token.tag == Tag::Ul || token.tag == Tag::Ol) {
                if (!token.closing && !token.selfClosing) lists++;
                else if (token.closing && lists > 0) lists--;
            } else if (token.tag == Tag::Li && lists == 0) {
                if (itemBegin != kNone) item(inner.substr(itemBegin, at - itemBegin));
                itemBegin = token.closing ? kNone : at + length;
            }
            at += length - 1;
        }
        if (itemBegin != kNone) item(inner.substr(itemBegin));
    }

    // A table of rows of cells of inline content becomes a Table block, its first row the header;
    // any other, its cells' content in reading order.
    void Table(std::string_view inner, uint8_t tableAlign) {
        constexpr size_t kMaxRows = 1000;
        constexpr size_t kMaxColumns = 64;
        std::vector<std::vector<std::string>> rows;
        std::vector<int> aligns;
        std::string_view caption;
        bool simple = true;
        QuoteCache local;
        TagToken token;
        size_t cellBegin = kNone;
        size_t captionBegin = kNone;
        int cellAlign = -1;
        const auto endCell = [&](size_t at) {
            if (cellBegin == kNone) return;
            std::vector<Block> blocks;
            HtmlState cellState;
            BlockMapper(cellState, blocks, nesting + 1, containers).Map(inner.substr(cellBegin, at - cellBegin));
            if (blocks.size() > 1 || (blocks.size() == 1 && blocks[0].type != BlockType::Paragraph)) simple = false;
            if (rows.empty()) rows.emplace_back();
            if (rows.size() == 1) aligns.push_back(cellAlign);
            rows.back().push_back(blocks.empty() ? std::string() : std::move(blocks[0].text));
            if (rows.back().size() > kMaxColumns) simple = false;
            cellBegin = kNone;
        };
        for (size_t at = inner.find('<'); at != kNone && simple; at = inner.find('<', at + 1)) {
            const uint32_t length = ScanTag(inner, at, local, token);
            if (length == 0) continue;
            switch (token.tag) {
            case Tag::Table: simple = false; break;
            case Tag::Tr:
                endCell(at);
                if (!token.closing) {
                    if (rows.size() >= kMaxRows) simple = false;
                    rows.emplace_back();
                }
                break;
            case Tag::Td: case Tag::Th:
                endCell(at);
                if (!token.closing) {
                    cellBegin = at + length;
                    const uint8_t align = AlignOf(token.align);
                    cellAlign = align == 2 ? 0 : align == 3 ? 1 : -1;
                }
                break;
            case Tag::Caption:
                endCell(at);
                if (!token.closing) captionBegin = at + length;
                else if (captionBegin != kNone) caption = inner.substr(captionBegin, at - captionBegin);
                break;
            case Tag::Thead: case Tag::Tbody: case Tag::Tfoot:
                endCell(at);
                break;
            default:
                break;
            }
            at += length - 1;
        }
        if (simple) endCell(inner.size());
        // Rows that hold no cell (a <tr> before the first cell, or closed twice) are left out.
        rows.erase(std::remove_if(rows.begin(), rows.end(), [](const std::vector<std::string>& row) { return row.empty(); }),
            rows.end());
        if (!simple || rows.empty()) {
            BlockMapper(state, out, nesting + 1, containers).Map(inner);
            return;
        }
        if (!caption.empty()) {
            Flush();
            paragraph = "<i>";
            contentStart = paragraph.size();
            paragraphAlign = 2;
            Map(caption);
        }
        size_t columns = 0;
        for (const auto& row : rows) columns = std::max(columns, row.size());
        for (auto& row : rows) row.resize(columns);
        aligns.resize(columns, -1);
        Block& table = Append(BlockType::Table);
        table.rows = std::move(rows);
        table.aligns = std::move(aligns);
        table.align = tableAlign != 0 ? tableAlign : state.Align();
    }

    HtmlState& state;
    std::vector<Block>& out;
    const int nesting;
    const Containers& containers;
    std::string paragraph;
    size_t contentStart = 0;        // the paragraph's text before this is a wrapper (<b>, <i>)
    bool space = false;             // white space seen since the last text
    bool visible = false;           // the paragraph has text or an image, not only tags
    int heading = 0;                // the level of an h1-h6 being read
    uint8_t paragraphAlign = 0;     // the alignment of the element the paragraph is, over state's
    std::vector<std::string> anchors;   // ids of <a> met in the paragraph, for blocks before it
    uint64_t linkBits = 0;              // of the <a> elements open, the outermost 64: whether each has an href
    size_t links = 0;                   // how many are open
    TagToken scratch;                   // Markup's tag: a member, not cleared for every tag (ScanTag sets it all)
    QuoteCache quotes;
};

} // namespace

void MapBlock(std::string_view html, HtmlState& state, std::vector<Block>& out) {
    const Containers containers(html);
    BlockMapper(state, out, 0, containers).Map(html);
}

size_t MapBlockLines(const std::vector<std::string_view>& lines, size_t start, int condition, HtmlState& state,
    std::vector<Block>& out, std::vector<Alignment>& alignments) {
    size_t end = start;
    if (condition <= 5) {
        while (end + 1 < lines.size() && !HtmlBlockEnds(lines[end], condition)) end++;
    } else {
        while (end + 1 < lines.size() && !HtmlBlockEnds(lines[end + 1], condition)) end++;
    }
    // The block's text: a view of the source while its lines are one piece of it, else the lines joined.
    std::string_view html = lines[start];
    std::string joined;
    bool joining = false;
    for (size_t index = start + 1; index <= end; index++) {
        const std::string_view line = lines[index];
        if (!joining && line.data() == html.data() + html.size() + 1 && html.data()[html.size()] == '\n') {
            html = std::string_view(html.data(), html.size() + 1 + line.size());
            continue;
        }
        if (!joining) joined.assign(html.data(), html.size());
        joining = true;
        joined.push_back('\n');
        joined.append(line.data(), line.size());
    }
    const size_t mapStart = out.size();
    MapBlock(joining ? std::string_view(joined) : html, state, out);
    alignments.push_back({ mapStart, out.size(), state.Align() });
    return end + 1;
}

void ApplyAlignments(std::vector<Block>& blocks, const std::vector<Alignment>& alignments) {
    for (size_t index = 0; index < alignments.size(); index++) {
        const uint8_t align = alignments[index].align;
        const size_t end = index + 1 < alignments.size() ? alignments[index + 1].mapStart : blocks.size();
        for (size_t block = alignments[index].mapEnd; align != 0 && block < end; block++) {
            if (blocks[block].align == 0) blocks[block].align = align;
        }
    }
}

bool MapParagraph(std::string_view paragraph, std::vector<Block>& out) {
    if (paragraph.empty() || (paragraph[0] != '<' && paragraph[0] != '!' && paragraph[0] != '[')) return false;
    std::string id;
    if (paragraph[0] == '<' && MatchAnchor(paragraph, id)) {
        out.push_back(AnchorBlock(std::move(id)));
        return true;
    }
    ImageSource first;
    std::string alt;
    if (!MatchImageRow(paragraph, first, alt)) return false;
    Block block;
    block.type = BlockType::Image;
    block.text = std::move(alt);
    block.image = std::make_unique<ImageSource>(std::move(first));
    out.push_back(std::move(block));
    return true;
}

} // namespace TinyPdf::Internal::Html
