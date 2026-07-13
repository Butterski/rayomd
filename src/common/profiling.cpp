#include "profiling.h"

#ifdef RAYOMD_ENABLE_PROFILING

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
std::atomic<uint64_t> g_allocations{0};
std::atomic<uint64_t> g_allocatedBytes{0};
thread_local std::array<uint64_t, static_cast<size_t>(RayoMd::Profiling::Phase::Count)> g_phaseNanoseconds{};

bool OutputEnabled() {
    const char* value = std::getenv("RAYOMD_PROFILE");
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}
} // namespace

namespace RayoMd::Profiling {

uint64_t NowNanoseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void AddPhaseTime(Phase phase, uint64_t nanoseconds) {
    g_phaseNanoseconds[static_cast<size_t>(phase)] += nanoseconds;
}

void RecordAllocation(size_t bytes) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    g_allocatedBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
}

Snapshot Capture() {
    Snapshot result;
    result.phaseNanoseconds = g_phaseNanoseconds;
    result.allocations = g_allocations.load(std::memory_order_relaxed);
    result.allocatedBytes = g_allocatedBytes.load(std::memory_order_relaxed);
    return result;
}

void EmitDelta(const char* label, const Snapshot& before, const Snapshot& after) {
    if (!OutputEnabled()) return;
    const auto us = [&](Phase phase) {
        size_t index = static_cast<size_t>(phase);
        return static_cast<double>(after.phaseNanoseconds[index] - before.phaseNanoseconds[index]) / 1000.0;
    };
    std::fprintf(stderr,
        "RAYOMD_PROFILE label=%s parse_us=%.3f render_us=%.3f font_us=%.3f image_us=%.3f "
        "assembly_us=%.3f io_us=%.3f allocations=%llu allocated_bytes=%llu\n",
        label ? label : "build", us(Phase::Parse), us(Phase::Render), us(Phase::Font),
        us(Phase::Image), us(Phase::Assembly), us(Phase::Io),
        static_cast<unsigned long long>(after.allocations - before.allocations),
        static_cast<unsigned long long>(after.allocatedBytes - before.allocatedBytes));
}

} // namespace RayoMd::Profiling

void* operator new(std::size_t size) {
    if (void* memory = std::malloc(size ? size : 1)) {
        RayoMd::Profiling::RecordAllocation(size);
        return memory;
    }
    std::abort();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (void* memory = std::malloc(size ? size : 1)) {
        RayoMd::Profiling::RecordAllocation(size);
        return memory;
    }
    return nullptr;
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

#endif