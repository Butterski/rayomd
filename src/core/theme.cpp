#include "theme.h"

#include "../common/text_utils.h"

#include <algorithm>
#include <charconv>

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

// A header or footer field with its placeholders replaced; an unknown one stays as it is, so a
// field that is just {logo} comes back unchanged.
std::string FieldText(std::string_view field, const ThemeDocument& document, size_t page, size_t pages) {
    std::string text;
    for (size_t at = 0; at < field.size();) {
        const size_t open = field.find('{', at);
        const size_t close = open == std::string_view::npos ? open : field.find('}', open);
        if (close == std::string_view::npos) {
            text.append(field.data() + at, field.size() - at);
            break;
        }
        text.append(field.data() + at, open - at);
        const std::string_view name = field.substr(open + 1, close - open - 1);
        if (name == "title") text += document.title;
        else if (name == "author") text += document.author;
        else if (name == "subject") text += document.subject;
        else if (name == "date") text += document.date;
        else if (name == "page") AppendCount(text, page);
        else if (name == "pages") AppendCount(text, pages);
        else text.append(field.data() + open, close + 1 - open);
        at = close + 1;
    }
    return text;
}

// Drops the last character of UTF-8 text.
void DropLastCharacter(std::string& text) {
    while (!text.empty() && ((unsigned char)text.back() & 0xC0) == 0x80) text.pop_back();
    if (!text.empty()) text.pop_back();
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
            while (!text.empty() && font.Width(text, kBandTextSize, false) > room) DropLastCharacter(text);
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
    std::vector<std::string> streams(pages);
    for (size_t number = 1; number <= pages; number++) {
        std::string header[3];
        std::string footer[3];
        for (size_t index = 0; index < 3; index++) {
            header[index] = FieldText(fields[index], document, number, pages);
            footer[index] = FieldText(fields[index + 3], document, number, pages);
        }
        std::string& stream = streams[number - 1];
        stream = "q 0.45 0.45 0.45 rg\n";
        AppendBand(stream, font, header, page, page.height - page.margin * 0.5, logo);
        AppendBand(stream, font, footer, page, page.margin * 0.5, logo);
        stream += "Q";
    }
    return streams;
}

std::string ThemeCoverStream(const ThemeDocument& document, const ThemePage& page, const ThemeLogoImage& logo,
    const ThemeTextFont& font, const char* headingColor, const char* ruleColor) {
    const double content = page.width - page.margin * 2.0;
    std::string out;
    // Centred lines of `text` from the baseline `y` down, which moves past them.
    const auto lines = [&](std::string_view text, double& y, double size, double leading, bool bold, const char* color) {
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
