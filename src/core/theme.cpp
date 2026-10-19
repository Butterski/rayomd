// SPDX-License-Identifier: Apache-2.0

#include "theme.h"

#include "../common/text_utils.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace TinyPdf::Internal {
namespace {

constexpr double kBandTextSize = 9.0;

void AppendNumber(std::string& out, double value) {
    RayoMd::Text::AppendFixed2(out, value);
}

void AppendCount(std::string& out, size_t value) {
    char digits[24];
    out.append(digits, (size_t)(std::to_chars(digits, digits + sizeof(digits), value).ptr - digits));
}

// The most bytes of a header's or footer's text on a line `width` points wide: a theme of
// thousands of {title} under a long title made gigabytes for one line. A band shows one line,
// and this many bytes, cut before a character (a quarter as many characters of four bytes, less
// one), are wider than it unless its characters average under a tenth of an em: only text of
// mostly zero-width characters can show differently. About 2 KiB for A4, 64 KiB at most.
size_t FieldBytes(double width) {
    constexpr double kMinAdvance = kBandTextSize * 0.1;
    return (size_t)(std::max(width, 0.0) / kMinAdvance) * 4 + 64;
}

// The bytes of `piece` that fit in `room`: all, or those before the character that does not.
size_t FittingBytes(std::string_view piece, size_t room) {
    if (piece.size() <= room) return piece.size();
    while (room > 0 && ((unsigned char)piece[room] & 0xC0) == 0x80) room--;
    return room;
}

// Appends `piece` to `text` up to `limit` bytes in all, cut before a character; false when cut.
bool AppendCapped(std::string& text, std::string_view piece, size_t limit) {
    const size_t take = FittingBytes(piece, limit - std::min(text.size(), limit));
    text.append(piece.data(), take);
    return take == piece.size();
}

// A header or footer field with its placeholders replaced but {page} and {pages}, which each page
// fills in where `numbers` says, so that no page reads the field again; an unknown placeholder
// stays as it is, so a field that is just {logo} comes back unchanged. It ends where every page's
// text passes `limit` bytes (a number has a digit at least).
struct FieldTemplate {
    std::string text;
    std::vector<std::pair<size_t, bool>> numbers;   // offset in text; {pages} (true) or {page}
};

FieldTemplate ExpandField(std::string_view field, const ThemeDocument& document, size_t limit) {
    FieldTemplate out;
    const auto append = [&out, limit](std::string_view piece) {
        const size_t shown = out.text.size() + out.numbers.size();
        const size_t take = FittingBytes(piece, limit - std::min(shown, limit));
        out.text.append(piece.data(), take);
        return take == piece.size();
    };
    for (size_t at = 0; at < field.size();) {
        const size_t open = field.find('{', at);
        const size_t close = open == std::string_view::npos ? open : field.find('}', open);
        if (close == std::string_view::npos) {
            append(field.substr(at));
            break;
        }
        if (!append(field.substr(at, open - at))) break;
        const std::string_view name = field.substr(open + 1, close - open - 1);
        bool whole = true;
        if (name == "title") whole = append(document.title);
        else if (name == "author") whole = append(document.author);
        else if (name == "subject") whole = append(document.subject);
        else if (name == "date") whole = append(document.date);
        else if (name == "page" || name == "pages") out.numbers.push_back({ out.text.size(), name == "pages" });
        else whole = append(field.substr(open, close + 1 - open));
        if (!whole || out.text.size() + out.numbers.size() >= limit) break;
        at = close + 1;
    }
    return out;
}

// The text of a field on page `page` of `pages`, at most `limit` bytes.
std::string FieldText(const FieldTemplate& field, size_t page, size_t pages, size_t limit) {
    std::string text;
    size_t from = 0;
    for (const auto& [at, total] : field.numbers) {
        if (!AppendCapped(text, std::string_view(field.text).substr(from, at - from), limit)) return text;
        char digits[24];
        const size_t length = (size_t)(std::to_chars(digits, digits + sizeof(digits), total ? pages : page).ptr - digits);
        if (!AppendCapped(text, std::string_view(digits, length), limit)) return text;
        from = at;
    }
    AppendCapped(text, std::string_view(field.text).substr(from), limit);
    return text;
}

// How much of `text`, which is wider than `room`, is left when its last character is dropped
// until it is no wider (nothing when even no text is): text up to the end of a character, the
// longest that fits. Its width only grows with it, so a search finds that in a few measures of
// little more than it, where dropping a character at a time measured all of it for each.
size_t FittingLength(const ThemeTextFont& font, std::string_view text, double room) {
    // text[0, at] (at < size) cut before the character `at` is in: where dropping one steps.
    const auto boundary = [text](size_t at) {
        while (at > 0 && ((unsigned char)text[at] & 0xC0) == 0x80) at--;
        return at;
    };
    const auto fitsIn = [&](size_t at) { return font.Width(text.substr(0, boundary(at)), kBandTextSize, false) <= room; };
    size_t fits = 0;    // a length whose start fits, or 0
    size_t over = 64;   // one whose start does not, or the whole text
    while (over < text.size() && fitsIn(over)) {
        fits = over;
        over *= 2;
    }
    over = std::min(over, text.size());
    while (over - fits > 1) {
        const size_t middle = fits + (over - fits) / 2;
        if (fitsIn(middle)) fits = middle;
        else over = middle;
    }
    return boundary(fits);
}

// The image /Im(logo.index + 1) in a box of `width` by `height` from (x, y).
void AppendLogo(std::string& out, const ThemeLogoImage& logo, double x, double y, double width, double height) {
    out += "q ";
    AppendNumber(out, width);
    out += " 0 0 ";
    AppendNumber(out, height);
    out += " ";
    AppendNumber(out, x);
    out += " ";
    AppendNumber(out, y);
    out += " cm /Im";
    AppendCount(out, (size_t)logo.index + 1);
    out += " Do Q\n";
}

// One header or footer line around `middle`: left, centre and right, each text or the logo. A
// field shares the line with others in thirds, or has it to itself.
void AppendBand(std::string& out, const ThemeTextFont& font, const std::string (&fields)[3], const ThemePage& page,
    double middle, const ThemeLogoImage& logo) {
    const double content = page.width - page.margin * 2.0;
    const int used = !fields[0].empty() + !fields[1].empty() + !fields[2].empty();
    const double slot = used > 1 ? content / 3.0 : content;
    const auto left = [&](size_t index, double width) {
        return index == 0 ? page.margin : index == 1 ? (page.width - width) * 0.5 : page.width - page.margin - width;
    };
    for (size_t index = 0; index < 3; index++) {
        if (fields[index].empty()) continue;
        if (fields[index] == "{logo}") {
            if (logo.index < 0) continue;
            double height = std::min(page.margin * 0.5, 24.0);
            double width = height * logo.aspect;
            if (width > slot) {
                height *= slot / width;
                width = slot;
            }
            AppendLogo(out, logo, left(index, width), middle - height * 0.5, width, height);
            continue;
        }
        std::string text = fields[index];
        double width = font.Width(text, kBandTextSize, false);
        if (width > slot) {
            const std::string_view ellipsis = font.Ellipsis();
            const double room = slot - font.Width(ellipsis, kBandTextSize, false);
            text.resize(FittingLength(font, text, room));
            text += ellipsis;
            width = font.Width(text, kBandTextSize, false);
        }
        font.Emit(out, text, left(index, width), middle - 3.0, kBandTextSize, false);
    }
}

// The words of `text` in lines no wider than `width`; a longer word has a line of its own.
std::vector<std::string> WrapText(const ThemeTextFont& font, std::string_view text, double width, double size, bool bold) {
    std::vector<std::string> lines;
    std::string line;
    for (size_t at = 0; at < text.size();) {
        while (at < text.size() && text[at] == ' ') at++;
        if (at >= text.size()) break;
        const size_t end = std::min(text.find(' ', at), text.size());
        std::string wider = line;
        if (!wider.empty()) wider += ' ';
        wider.append(text.data() + at, end - at);
        if (!line.empty() && font.Width(wider, size, bold) > width) {
            lines.push_back(std::move(line));
            line.assign(text.data() + at, end - at);
        } else {
            line = std::move(wider);
        }
        at = end;
    }
    if (!line.empty()) lines.push_back(std::move(line));
    return lines;
}

} // namespace

std::vector<std::string> ThemeBandStreams(const PdfTheme& theme, bool pageNumbers, const ThemeDocument& document,
    size_t pages, const ThemePage& page, const ThemeLogoImage& logo, const ThemeTextFont& font) {
    const std::string_view fields[6] = { theme.headerLeft, theme.headerCenter, theme.headerRight, theme.footerLeft,
        theme.footerCenter.empty() && pageNumbers ? std::string_view("{page} / {pages}") : theme.footerCenter,
        theme.footerRight };
    const size_t limit = FieldBytes(page.width - page.margin * 2.0);
    FieldTemplate templates[6];
    for (size_t index = 0; index < 6; index++) templates[index] = ExpandField(fields[index], document, limit);
    std::vector<std::string> streams(pages);
    for (size_t number = 1; number <= pages; number++) {
        std::string header[3];
        std::string footer[3];
        for (size_t index = 0; index < 3; index++) {
            header[index] = FieldText(templates[index], number, pages, limit);
            footer[index] = FieldText(templates[index + 3], number, pages, limit);
        }
        std::string& stream = streams[number - 1];
        stream = "q 0.45 0.45 0.45 rg\n";
        AppendBand(stream, font, header, page, page.height - page.margin * 0.5, logo);
        AppendBand(stream, font, footer, page, (page.bottoms != nullptr ? page.bottoms[number - 1] : 0.0) + page.margin * 0.5, logo);
        stream += "Q";
    }
    return streams;
}

std::string ThemeCoverStream(const ThemeDocument& document, const ThemePage& page, const ThemeLogoImage& logo,
    const ThemeTextFont& font, std::string_view headingColor, std::string_view ruleColor) {
    const double content = page.width - page.margin * 2.0;
    std::string out;
    // Centred lines of `text` from the baseline `y` down, which moves past them.
    const auto lines = [&](std::string_view text, double& y, double size, double leading, bool bold, std::string_view color) {
        if (text.empty()) return;
        out += "q ";
        out += color;
        out += " rg\n";
        for (const std::string& line : WrapText(font, text, content, size, bold)) {
            font.Emit(out, line, (page.width - font.Width(line, size, bold)) * 0.5, y, size, bold);
            y -= leading;
        }
        out += "Q\n";
    };
    if (logo.index >= 0) {
        double height = std::min(72.0, page.height * 0.12);
        double width = height * logo.aspect;
        if (width > content * 0.6) {
            height *= content * 0.6 / width;
            width = content * 0.6;
        }
        AppendLogo(out, logo, (page.width - width) * 0.5, page.height * 0.8 - height, width, height);
    }
    double y = page.height * 0.62;
    lines(document.title, y, 28.0, 35.0, true, headingColor);
    if (!document.title.empty()) {
        const double ruleY = y + 19.0;   // 16 points under the last line
        const double ruleWidth = std::min(120.0, content * 0.3);
        out += "q ";
        out += ruleColor;
        out += " RG 1.5 w ";
        AppendNumber(out, (page.width - ruleWidth) * 0.5);
        out += " ";
        AppendNumber(out, ruleY);
        out += " m ";
        AppendNumber(out, (page.width + ruleWidth) * 0.5);
        out += " ";
        AppendNumber(out, ruleY);
        out += " l S Q\n";
        y = ruleY - 32.0;
    }
    lines(document.subject, y, 15.0, 20.0, false, "0.30 0.30 0.30");
    double foot = page.margin + 36.0;
    if (!document.date.empty()) {
        double dateY = foot;
        lines(document.date, dateY, 11.0, 15.0, false, "0.40 0.40 0.40");
        foot += 18.0;
    }
    if (!document.author.empty()) {
        // From the top of its lines, so that the last one ends at the foot.
        const size_t count = WrapText(font, document.author, content, 12.0, false).size();
        double authorY = foot + (double)(count > 0 ? count - 1 : 0) * 16.0;
        lines(document.author, authorY, 12.0, 16.0, false, "0.15 0.15 0.15");
    }
    return out;
}

} // namespace TinyPdf::Internal
