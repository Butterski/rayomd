// SPDX-License-Identifier: Apache-2.0

#include "diagram_cache.h"

#include "mermaid.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace TinyPdf::Internal {

namespace {

constexpr size_t kMaxDiagramBytes = size_t(16) << 20;
constexpr size_t kMaxOneDiagramBytes = size_t(4) << 20;

struct Cache {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const DrawnDiagram>> diagrams;
    size_t bytes = 0;
};

Cache& TheCache() {
    static Cache cache;
    return cache;
}

std::atomic<uint64_t> g_layouts{ 0 };

std::string KeyText(const DiagramKey& key) {
    const uint64_t fonts[2] = { key.textFont, key.boldFont };
    std::string text;
    text.reserve(1 + sizeof(fonts) + sizeof(Mermaid::Palette) + key.source.size());
    text += key.unicode ? 'U' : 'S';
    text.append(reinterpret_cast<const char*>(fonts), sizeof(fonts));
    text.append(reinterpret_cast<const char*>(key.palette), sizeof(Mermaid::Palette));
    text.append(key.source.data(), key.source.size());
    return text;
}

} // namespace

std::shared_ptr<const DrawnDiagram> FindDrawnDiagram(const DiagramKey& key) {
    const std::string text = KeyText(key);
    Cache& cache = TheCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    const auto found = cache.diagrams.find(text);
    return found == cache.diagrams.end() ? nullptr : found->second;
}

void KeepDrawnDiagram(const DiagramKey& key, std::shared_ptr<const DrawnDiagram> diagram, size_t formBytes) {
    std::string text = KeyText(key);
    size_t cost = text.size() + formBytes + 256;
    for (const SvgTextRun& run : diagram->runs) cost += run.text.size() * sizeof(wchar_t) + sizeof(SvgTextRun);
    if (cost > kMaxOneDiagramBytes) return;
    Cache& cache = TheCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.bytes + cost > kMaxDiagramBytes) {
        cache.diagrams.clear();
        cache.bytes = 0;
    }
    if (cache.diagrams.emplace(std::move(text), std::move(diagram)).second) cache.bytes += cost;
}

void CountDiagramLayout() {
    g_layouts.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<DrawnDiagram> NewDrawnDiagram() {
    return std::make_shared<DrawnDiagram>();
}

void NoteSvgTextRun(std::vector<SvgTextRun>& runs, bool bold, const std::wstring& text) {
    runs.push_back({ bold, text });
}

uint64_t Mermaid::Layouts() {
    return g_layouts.load(std::memory_order_relaxed);
}

void Mermaid::ForgetDiagrams() {
    Cache& cache = TheCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.diagrams.clear();
    cache.bytes = 0;
}

} // namespace TinyPdf::Internal
