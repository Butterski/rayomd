// SPDX-License-Identifier: Apache-2.0

#include "diagram_cache.h"

#include "mermaid.h"
#include "svg.h"

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
std::atomic<uint64_t> g_svgFiles{ 0 };

constexpr size_t kMaxSvgBytes = size_t(32) << 20;
constexpr size_t kMaxOneSvgBytes = size_t(8) << 20;
constexpr size_t kMaxSvgFiles = 4096;

struct SvgCache {
    struct Entry {
        std::shared_ptr<const ConvertedSvg> svg;
        size_t cost = 0;
    };
    std::mutex mutex;
    std::unordered_map<std::string, Entry> files;
    size_t bytes = 0;
};

SvgCache& TheSvgCache() {
    static SvgCache cache;
    return cache;
}

std::string KeyText(char kind, bool unicode, uint64_t textFont, uint64_t boldFont, std::string_view extra,
    std::string_view text) {
    const uint64_t fonts[2] = { textFont, boldFont };
    std::string key;
    key.reserve(2 + sizeof(fonts) + extra.size() + text.size());
    key += kind;
    key += unicode ? 'U' : 'S';
    key.append(reinterpret_cast<const char*>(fonts), sizeof(fonts));
    key.append(extra.data(), extra.size());
    key.append(text.data(), text.size());
    return key;
}

std::string KeyText(const DiagramKey& key) {
    return KeyText('D', key.unicode, key.textFont, key.boldFont,
        std::string_view(reinterpret_cast<const char*>(key.palette), sizeof(Mermaid::Palette)), key.source);
}

std::string KeyText(const SvgFileKey& key) {
    return KeyText('F', key.unicode, key.textFont, key.boldFont, {}, key.file);
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

std::shared_ptr<const ConvertedSvg> FindConvertedSvg(const SvgFileKey& key, const RayoMd::File::Stamp& stamp, size_t base) {
    const std::string text = KeyText(key);
    SvgCache& cache = TheSvgCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    const auto found = cache.files.find(text);
    if (found == cache.files.end() || found->second.svg->stamp != stamp || found->second.svg->base != base) return nullptr;
    return found->second.svg;
}

void KeepConvertedSvg(const SvgFileKey& key, std::shared_ptr<const ConvertedSvg> svg, size_t bytes) {
    std::string text = KeyText(key);
    size_t cost = text.size() + bytes + 256 + svg->images.size() * sizeof(svg->images[0]);
    for (const SvgTextRun& run : svg->runs) cost += run.text.size() * sizeof(wchar_t) + sizeof(SvgTextRun);
    if (cost > kMaxOneSvgBytes) return;
    SvgCache& cache = TheSvgCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    const auto found = cache.files.find(text);
    if (found != cache.files.end()) {
        cache.bytes -= found->second.cost;
        cache.files.erase(found);
    }
    if (cache.bytes + cost > kMaxSvgBytes || cache.files.size() >= kMaxSvgFiles) {
        cache.files.clear();
        cache.bytes = 0;
    }
    cache.files.emplace(std::move(text), SvgCache::Entry{ std::move(svg), cost });
    cache.bytes += cost;
}

std::shared_ptr<ConvertedSvg> NewConvertedSvg() {
    return std::make_shared<ConvertedSvg>();
}

void CountSvgFileConversion() {
    g_svgFiles.fetch_add(1, std::memory_order_relaxed);
}

uint64_t SvgFileConversions() {
    return g_svgFiles.load(std::memory_order_relaxed);
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
