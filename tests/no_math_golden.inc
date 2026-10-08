// An ASCII document without any math syntax (no '$', no "\(", no "\[", no math fence).
// It goes through the standard-font renderer, whose output does not depend on the
// platform or on installed fonts, so its PDF bytes are a stable fingerprint.
static const char kNoMathGoldenDocument[] =
    "# Golden heading with *markers* and `code`\n\n"
    "Plain paragraph that is long enough to wrap onto a second line when it is laid out, because it "
    "keeps going for a while with ordinary words and a few numbers such as 1234 and 5678.\n\n"
    "Styled paragraph with **bold**, *italic*, ~~strike~~, `inline_code()`, a "
    "[link](https://example.com/golden), an <https://example.com/auto> autolink and escaped \\*stars\\*.\n\n"
    "Setext heading\n--------------\n\n"
    "- first bullet\n- second bullet with `code`\n  - nested bullet\n\n"
    "1. first numbered\n2. second numbered\n\n"
    "> A quoted paragraph with a [quoted link](https://example.com/quote) and enough words to wrap "
    "inside the quote strip so that more than one background rectangle is drawn.\n>\n"
    "> ## Quote heading\n\n"
    "| Name | Value | Notes |\n| :--- | ---: | :---: |\n| alpha | 1 | left |\n"
    "| a wider cell that wraps inside its column | 22 | centre |\n\n"
    "```\nfor (int i = 0; i < n; i++) total += values[i];\n```\n\n"
    "    indented code\n\n"
    "---\n\n"
    "\\pagebreak\n\n"
    "Last paragraph after the page break.  \nWith a hard line break.\n";
