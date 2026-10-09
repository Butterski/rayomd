#pragma once

// A theme's header, footer and cover page as PDF content operators. Kept apart from the engine
// (tiny_pdf.cpp), which sits at GCC's inlining limit: there, this cold code moved what the hot
// paths inline. Text is UTF-8; the renderer that owns the font measures and writes it.

#include "rayomd/tiny_pdf.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace TinyPdf::Internal {

// The font a theme's text is shown in, as the renderer that owns it measures and writes it.
class ThemeTextFont {
public:
    virtual ~ThemeTextFont() = default;
    virtual double Width(std::string_view utf8, double size, bool bold) const = 0;
    // A text object showing `utf8` from (x, baseline), ending with a line feed.
    virtual void Emit(std::string& out, std::string_view utf8, double x, double baseline, double size, bool bold) const = 0;
    // What ends text cut short, UTF-8.
    virtual std::string_view Ellipsis() const = 0;
};

// What a theme's text tells of the document: its title (the front matter's, else the first
// heading's), author, subject and date.
struct ThemeDocument {
    std::string_view title;
    std::string_view author;
    std::string_view subject;
    std::string_view date;
};

struct ThemePage {
    double width = 0.0;
    double height = 0.0;
    double margin = 0.0;
};

// The theme's logo among the document's images, /Im(index + 1); index -1 when there is none.
struct ThemeLogoImage {
    int index = -1;
    double aspect = 1.0;   // width over height
};

inline bool HasThemeBands(const PdfTheme& theme) {
    return !theme.headerLeft.empty() || !theme.headerCenter.empty() || !theme.headerRight.empty() ||
        !theme.footerLeft.empty() || !theme.footerCenter.empty() || !theme.footerRight.empty();
}

// The header and footer of every page, one content stream each: grey text in the middle of the
// top and bottom margin, at its left, centre and right, shortened with an ellipsis to fit its
// share of the line; {title}, {author}, {subject}, {date}, {page} and {pages} replaced, and a
// field that is just {logo} showing the logo. With `pageNumbers` an empty footer centre shows
// "{page} / {pages}".
std::vector<std::string> ThemeBandStreams(const PdfTheme& theme, bool pageNumbers, const ThemeDocument& document,
    size_t pages, const ThemePage& page, const ThemeLogoImage& logo, const ThemeTextFont& font);

// The cover page's content stream, centred: the logo, the title in `headingColor` over a short
// rule in `ruleColor` (PDF colour operands), the subject, and the author and the date near the
// foot. What the document does not give is left out.
std::string ThemeCoverStream(const ThemeDocument& document, const ThemePage& page, const ThemeLogoImage& logo,
    const ThemeTextFont& font, const char* headingColor, const char* ruleColor);

} // namespace TinyPdf::Internal
