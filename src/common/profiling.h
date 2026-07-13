#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace RayoMd::Profiling {

enum class Phase : size_t { Parse, Render, Font, Image, Assembly, Io, Count };

struct Snapshot {
    std::array<uint64_t, static_cast<size_t>(Phase::Count)> phaseNanoseconds{};
    uint64_t allocations = 0;
    uint64_t allocatedBytes = 0;
};

#ifdef RAYOMD_ENABLE_PROFILING
uint64_t NowNanoseconds();
void AddPhaseTime(Phase phase, uint64_t nanoseconds);
void RecordAllocation(size_t bytes);
Snapshot Capture();
void EmitDelta(const char* label, const Snapshot& before, const Snapshot& after);

class ScopedPhase {
public:
    explicit ScopedPhase(Phase phaseValue) : phase(phaseValue), start(NowNanoseconds()) {}
    ~ScopedPhase() { AddPhaseTime(phase, NowNanoseconds() - start); }
private:
    Phase phase;
    uint64_t start;
};
#else
inline Snapshot Capture() { return {}; }
inline void EmitDelta(const char*, const Snapshot&, const Snapshot&) {}
class ScopedPhase { public: explicit ScopedPhase(Phase) {} };
#endif

} // namespace RayoMd::Profiling