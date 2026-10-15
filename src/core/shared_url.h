// SPDX-License-Identifier: Apache-2.0

#pragma once

// A link's target held once and shared by every piece of its text, word, line and rectangle. Copied
// into each, a long URL over long link text took one copy per piece: gigabytes of memory, and of
// PDF, for a few megabytes of Markdown.

#include <memory>
#include <string>
#include <string_view>

namespace TinyPdf::Internal {

class SharedUrl {
public:
    SharedUrl() = default;
    explicit SharedUrl(std::string_view url) {
        if (!url.empty()) text = std::make_shared<const std::string>(url);
    }

    bool empty() const { return text == nullptr; }
    size_t size() const { return text == nullptr ? 0 : text->size(); }
    std::string_view View() const { return text == nullptr ? std::string_view() : std::string_view(*text); }
    operator std::string_view() const { return View(); }

    // The same target: the same string, or equal text.
    friend bool operator==(const SharedUrl& a, const SharedUrl& b) { return a.text == b.text || a.View() == b.View(); }
    friend bool operator!=(const SharedUrl& a, const SharedUrl& b) { return !(a == b); }
    friend bool operator==(const SharedUrl& a, std::string_view b) { return a.View() == b; }
    friend bool operator!=(const SharedUrl& a, std::string_view b) { return a.View() != b; }

private:
    std::shared_ptr<const std::string> text;
};

} // namespace TinyPdf::Internal
