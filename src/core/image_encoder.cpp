#include "image_encoder.h"

#include "flate.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace TinyPdf::Internal {
namespace {

constexpr size_t kAbandoned = ~size_t(0);
constexpr unsigned kMaxHelpers = 3;
// A segment's first block and what it reads past it (flate.cpp: kBlockStart, kMaxMatch, a hash).
constexpr size_t kFirstBlock = 65278 + 258 + 8;

struct FreeBuffer {
    void operator()(uint8_t* buffer) const { std::free(buffer); }
};

struct Segment {
    size_t plane = 0;
    size_t start = 0;   // in the plane
    size_t size = 0;
    size_t slot = 0;    // where its blocks are written in the plane's output
    size_t written = 0;
    uint32_t adler = 1;
};

// The encoder's tables, one set per thread and kept from segment to segment.
RayoMd::Flate::Scratch& ThreadScratch() {
    static thread_local RayoMd::Flate::Scratch scratch;
    return scratch;
}

} // namespace

struct ImagePlanesEncoder::State {
    const uint8_t* planes[kMaxPlanes] = {};
    size_t sizes[kMaxPlanes] = {};
    size_t count = 0;
    std::unique_ptr<uint8_t[], FreeBuffer> out[kMaxPlanes];
    std::atomic<size_t> ready[kMaxPlanes];
    // In the order their bytes arrive: the planes' rows are written side by side, so a segment
    // ending at a smaller share of its plane comes first.
    std::vector<Segment> segments;
    std::unique_ptr<std::atomic<bool>[]> claimed;
    std::atomic<size_t> claimedCount{ 0 };

    // A helper with no segment to take sleeps until the decoder publishes the first block of one,
    // abandons a plane, or the last segment is taken.
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<unsigned> sleepers{ 0 };
    std::atomic<unsigned> running{ 0 };   // helpers in Work
    size_t wakeStart[kMaxPlanes] = {};    // the decoder's own: the start of the plane's next segment to wake for

    // The bytes a segment needs before it is taken: all of them for the thread that writes them,
    // else its first block, as a helper compresses each block once its bytes are there.
    static size_t ClaimAt(size_t start, size_t size, bool streamed) {
        return start + (streamed ? std::min(size, kFirstBlock) : size);
    }

    bool Claimable(size_t index, bool streamed) const {
        const Segment& segment = segments[index];
        return !claimed[index].load() &&
            ready[segment.plane].load(std::memory_order_acquire) >= ClaimAt(segment.start, segment.size, streamed);
    }

    void Wake() {
        { std::lock_guard<std::mutex> lock(mutex); }
        changed.notify_all();
    }

    // The first segment not yet taken that can be, now taken by this thread; null when there is none.
    Segment* Claim(bool streamed) {
        for (size_t index = 0; index < segments.size(); index++) {
            if (!Claimable(index, streamed) || claimed[index].exchange(true)) continue;
            if (claimedCount.fetch_add(1) + 1 == segments.size()) Wake();
            return &segments[index];
        }
        return nullptr;
    }

    bool CanClaim() const {
        if (claimedCount.load() == segments.size()) return true;
        for (size_t index = 0; index < segments.size(); index++) {
            if (Claimable(index, true)) return true;
        }
        return false;
    }

    void Encode(Segment& segment, bool streamed) {
        if (ready[segment.plane].load(std::memory_order_acquire) == kAbandoned || !out[segment.plane]) return;
        const uint8_t* const data = planes[segment.plane] + segment.start;
        const size_t history = std::min(segment.start, RayoMd::Flate::kImageSegmentHistory);
        const bool last = segment.start + segment.size == sizes[segment.plane];
        RayoMd::Flate::InputProgress progress;
        progress.written = &ready[segment.plane];
        progress.offset = segment.start - history;
        segment.written = RayoMd::Flate::DeflateImageSegment(data - history, history, segment.size, last,
            out[segment.plane].get() + segment.slot, ThreadScratch(), streamed ? &progress : nullptr);
        // Abandoned while it was compressed: no stream is made of it.
        if (segment.written == 0) return;
        segment.adler = RayoMd::Flate::Adler32(data, segment.size);
    }

    // Takes segments as their first blocks arrive until all are taken.
    void Work() {
        while (claimedCount.load() < segments.size()) {
            if (Segment* const segment = Claim(true)) {
                Encode(*segment, true);
                continue;
            }
            std::unique_lock<std::mutex> lock(mutex);
            sleepers.fetch_add(1);
            changed.wait(lock, [&]() { return CanClaim(); });
            sleepers.fetch_sub(1);
        }
    }

    void LeaveHelpers();
};

namespace {

// The helper threads: started as encoders first ask for them, never stopped, and leaked so that
// no static destructor runs while they wait. An encoder is queued once for each helper it wants;
// a helper takes it from the queue and works on it until its segments are all taken.
class HelperPool {
public:
    static HelperPool& Get() {
        static HelperPool* const pool = new HelperPool();
        return *pool;
    }

    void Request(ImagePlanesEncoder::State* state, unsigned helpers) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (; threads_ < helpers; threads_++) std::thread([this]() { Run(); }).detach();
            for (unsigned helper = 0; helper < helpers; helper++) queue_.push_back(state);
        }
        queued_.notify_all();
    }

    // No helper takes `state` from now on; those that took it are counted in state->running.
    void Cancel(ImagePlanesEncoder::State* state) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.erase(std::remove(queue_.begin(), queue_.end(), state), queue_.end());
    }

private:
    void Run() {
        for (;;) {
            ImagePlanesEncoder::State* state = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                queued_.wait(lock, [&]() { return !queue_.empty(); });
                state = queue_.front();
                queue_.pop_front();
                state->running.fetch_add(1);
            }
            state->Work();
            // Under the lock: once this helper has left, the encoder may destroy the state.
            std::lock_guard<std::mutex> lock(state->mutex);
            state->running.fetch_sub(1);
            state->changed.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable queued_;
    std::deque<ImagePlanesEncoder::State*> queue_;
    unsigned threads_ = 0;
};

} // namespace

void ImagePlanesEncoder::State::LeaveHelpers() {
    HelperPool::Get().Cancel(this);
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&]() { return running.load() == 0; });
}

ImagePlanesEncoder::ImagePlanesEncoder(const uint8_t* const* planes, const size_t* sizes, size_t count,
    unsigned helpers)
    : state_(new State) {
    State& state = *state_;
    state.count = count;
    for (size_t plane = 0; plane < count; plane++) {
        state.planes[plane] = planes[plane];
        state.sizes[plane] = sizes[plane];
        state.ready[plane].store(0, std::memory_order_relaxed);
        size_t start = 0;
        size_t slot = 2;   // behind the zlib header
        do {
            Segment segment;
            segment.plane = plane;
            segment.start = start;
            segment.size = RayoMd::Flate::ImageSegmentSize(sizes[plane] - start);
            segment.slot = slot;
            start += segment.size;
            slot += RayoMd::Flate::ImageSegmentBound(segment.size);   // its slack too, as threads write side by side
            state.segments.push_back(segment);
        } while (start < sizes[plane]);
        state.out[plane].reset(static_cast<uint8_t*>(std::malloc(slot + 4)));
    }
    std::stable_sort(state.segments.begin(), state.segments.end(), [&](const Segment& a, const Segment& b) {
        // Below 2^31 each, so the products fit.
        return (uint64_t)(a.start + a.size) * state.sizes[b.plane] < (uint64_t)(b.start + b.size) * state.sizes[a.plane];
    });
    state.claimed.reset(new std::atomic<bool>[state.segments.size()]);
    for (size_t index = 0; index < state.segments.size(); index++) state.claimed[index].store(false, std::memory_order_relaxed);
    helpers = (unsigned)std::min<size_t>(std::min(helpers, kMaxHelpers), state.segments.size());
    if (helpers > 0) HelperPool::Get().Request(&state, helpers);
}

ImagePlanesEncoder::~ImagePlanesEncoder() {
    State& state = *state_;
    for (size_t plane = 0; plane < state.count; plane++) {
        if (state.ready[plane].load(std::memory_order_relaxed) != state.sizes[plane]) state.ready[plane].store(kAbandoned);
    }
    state.Wake();
    state.LeaveHelpers();
}

void ImagePlanesEncoder::Publish(size_t plane, size_t bytes) {
    State& state = *state_;
    state.ready[plane].store(bytes);   // sequentially consistent with the sleepers' count
    size_t& start = state.wakeStart[plane];
    const size_t size = state.sizes[plane];
    bool crossed = false;
    while (start < size) {
        const size_t segment = RayoMd::Flate::ImageSegmentSize(size - start);
        if (bytes < State::ClaimAt(start, segment, true)) break;
        start += segment;
        crossed = true;
    }
    if (crossed && state.sleepers.load() > 0) state.Wake();
}

void ImagePlanesEncoder::Abandon(size_t plane) {
    state_->ready[plane].store(kAbandoned);
    state_->Wake();
}

void ImagePlanesEncoder::EncodeReady() {
    while (Segment* const segment = state_->Claim(false)) state_->Encode(*segment, false);
}

bool ImagePlanesEncoder::Finish(std::string* streams) {
    State& state = *state_;
    state.Work();
    state.LeaveHelpers();
    for (size_t plane = 0; plane < state.count; plane++) {
        streams[plane].clear();
        if (state.ready[plane].load(std::memory_order_acquire) == kAbandoned) continue;
        uint8_t* const out = state.out[plane].get();
        if (!out) return false;
        // The segments' blocks moved together behind the header, then the Adler-32 of them all.
        out[0] = 0x78;
        out[1] = 0x5E;
        size_t at = 2;
        uint32_t adler = 1;
        for (const Segment& segment : state.segments) {
            if (segment.plane != plane) continue;
            std::memmove(out + at, out + segment.slot, segment.written);
            at += segment.written;
            adler = segment.start == 0 ? segment.adler : RayoMd::Flate::Adler32Combine(adler, segment.adler, segment.size);
        }
        out[at++] = (uint8_t)(adler >> 24);
        out[at++] = (uint8_t)(adler >> 16);
        out[at++] = (uint8_t)(adler >> 8);
        out[at++] = (uint8_t)adler;
        streams[plane].assign(reinterpret_cast<const char*>(out), at);
    }
    return true;
}

} // namespace TinyPdf::Internal
