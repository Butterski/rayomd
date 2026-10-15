// SPDX-License-Identifier: Apache-2.0

#pragma once

// A link's target held once and shared by every piece of its text, word, line and rectangle. Copied
// into each, a long URL over long link text took one copy per piece: gigabytes of memory, and of
// PDF, for a few megabytes of Markdown.

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

namespace TinyPdf::Internal {

// The text and its count of holders in one allocation (a shared_ptr to a string took two, which
// a document of many short links paid for in instructions); the text ends in a NUL.
class SharedUrl {
public:
    SharedUrl() = default;
    explicit SharedUrl(std::string_view url) {
        if (url.empty()) return;
        text = new (::operator new(sizeof(Text) + url.size() + 1)) Text(url.size());
        std::memcpy(text->Chars(), url.data(), url.size());
        text->Chars()[url.size()] = '\0';
    }
    SharedUrl(const SharedUrl& other) noexcept : text(other.text) {
        if (text != nullptr) text->holders.fetch_add(1, std::memory_order_relaxed);
    }
    SharedUrl(SharedUrl&& other) noexcept : text(other.text) { other.text = nullptr; }
    SharedUrl& operator=(SharedUrl other) noexcept {
        std::swap(text, other.text);
        return *this;
    }
    ~SharedUrl() {
        if (text != nullptr && text->holders.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            text->~Text();
            ::operator delete(text);
        }
    }

    bool empty() const { return text == nullptr; }
    size_t size() const { return text == nullptr ? 0 : text->size; }
    std::string_view View() const { return text == nullptr ? std::string_view() : std::string_view(text->Chars(), text->size); }
    operator std::string_view() const { return View(); }

    // The same target: the same text object, or equal text.
    friend bool operator==(const SharedUrl& a, const SharedUrl& b) { return a.text == b.text || a.View() == b.View(); }
    friend bool operator!=(const SharedUrl& a, const SharedUrl& b) { return !(a == b); }
    friend bool operator==(const SharedUrl& a, std::string_view b) { return a.View() == b; }
    friend bool operator!=(const SharedUrl& a, std::string_view b) { return a.View() != b; }

private:
    struct Text {
        explicit Text(size_t bytes) : size(bytes) {}
        char* Chars() { return reinterpret_cast<char*>(this + 1); }
        std::atomic<size_t> holders{ 1 };
        size_t size;
    };
    Text* text = nullptr;
};

} // namespace TinyPdf::Internal
