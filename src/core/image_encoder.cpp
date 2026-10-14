// SPDX-License-Identifier: Apache-2.0

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

#ifdef __linux__
#include <sys/mman.h>
#endif

namespace TinyPdf::Internal {
namespace {

constexpr size_t kAbandoned = ~size_t(0);
// A segment's first block and what it reads past it (flate.cpp: kBlockStart, kMaxMatch, a hash).
constexpr size_t kFirstBlock = 65278 + 258 + 8;
constexpr size_t kHistory = RayoMd::Flate::kImageSegmentHistory;
// The largest segment (ImageSegmentSize) and the history before it.
constexpr size_t kBufferBytes = (size_t(1) << 20) + kHistory;

struct FreeBuffer {
    void operator()(uint8_t* buffer) const { std::free(buffer); }
};
using Buffer = std::unique_ptr<uint8_t[], FreeBuffer>;

struct Segment {
    size_t plane = 0;
    size_t start = 0;            // in the plane
    size_t size = 0;
    size_t history = 0;          // the plane's bytes before `start` that its buffer begins with
    size_t slot = 0;             // where its blocks are written in the plane's output
    uint8_t* input = nullptr;    // its buffer, from when the decoder reaches it until it is compressed
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
    size_t sizes[kMaxPlanes] = {};
    size_t count = 0;
    bool fast = false;   // DeflateImageSegment's fast mode (--fast-images)
    Buffer out[kMaxPlanes];
    std::atomic<size_t> ready[kMaxPlanes];
    // In the order their bytes arrive: the planes' rows are written side by side, so a segment
    // ending at a smaller share of its plane comes first.
    std::vector<Segment> segments;
    std::unique_ptr<std::atomic<bool>[]> claimed;
    std::atomic<size_t> claimedCount{ 0 };

    // The decoder's own: for each plane, its segments in order, the one being written, the bytes
    // written, and the last bytes of the segment before, which begin the next one's buffer.
    std::vector<size_t> planeSegments[kMaxPlanes];
    size_t writing[kMaxPlanes] = {};
    size_t position[kMaxPlanes] = {};
    uint8_t tail[kMaxPlanes][kHistory];
    size_t wakeStart[kMaxPlanes] = {};   // the start of the plane's next segment to wake helpers for
    bool failed = false;

    // Segment buffers: those allocated one by one or carved from `region`, and those free; at
    // most `bufferLimit`, one being written for each plane, one for each helper, and one more, so
    // that a full one is always there to free.
    std::vector<Buffer> buffers;
    Buffer region;
    size_t carved = 0;
    std::vector<uint8_t*> spare;
    size_t bufferLimit = 0;

    // A helper with no segment to take sleeps until the decoder writes the first block of one,
    // abandons a plane, or the last segment is taken; a decoder with no free buffer sleeps until
    // one is freed.
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<unsigned> sleepers{ 0 };
    std::atomic<unsigned> running{ 0 };   // helpers in Work

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

    void Release(Segment& segment) {
        if (!segment.input) return;
        std::lock_guard<std::mutex> lock(mutex);
        spare.push_back(segment.input);
        segment.input = nullptr;
        changed.notify_all();
    }

    void Encode(Segment& segment, bool streamed) {
        if (ready[segment.plane].load(std::memory_order_acquire) != kAbandoned && out[segment.plane]) {
            const bool last = segment.start + segment.size == sizes[segment.plane];
            RayoMd::Flate::InputProgress progress;
            progress.written = &ready[segment.plane];
            progress.offset = segment.start - segment.history;
            segment.written = RayoMd::Flate::DeflateImageSegment(segment.input, segment.history, segment.size, last,
                out[segment.plane].get() + segment.slot, ThreadScratch(), streamed ? &progress : nullptr, fast);
            // Abandoned while it was compressed: no stream is made of it.
            if (segment.written != 0) segment.adler = RayoMd::Flate::Adler32(segment.input + segment.history, segment.size);
        }
        Release(segment);
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

    // A new segment buffer. The first encoders of a process carve theirs from one region on 2 MB
    // pages where Linux offers them (transparent huge pages in "madvise" or "always" mode): a
    // fresh process faults the mascot's buffers in twice instead of about 640 times. Later ones
    // take the allocator's memory, which a long process has already mapped.
    uint8_t* NewBuffer() {
#ifdef __linux__
        constexpr size_t kHugePage = size_t(2) << 20;
        static std::atomic<int> hugeEncodersLeft{ 4 };
        if (carved == 0 && buffers.empty() && hugeEncodersLeft.fetch_sub(1, std::memory_order_relaxed) > 0) {
            const size_t bytes = (bufferLimit * kBufferBytes + kHugePage - 1) & ~(kHugePage - 1);
            region.reset(static_cast<uint8_t*>(std::aligned_alloc(kHugePage, bytes)));
            if (region) madvise(region.get(), bytes, MADV_HUGEPAGE);
        }
        if (region) return region.get() + kBufferBytes * carved++;
#endif
        buffers.emplace_back(static_cast<uint8_t*>(std::malloc(kBufferBytes)));
        return buffers.back().get();
    }

    // A free segment buffer: a spare one, a new one while fewer than the limit exist, else one
    // freed by compressing a written segment here or by waiting for a helper. Null when it cannot
    // be allocated.
    uint8_t* AcquireBuffer() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!spare.empty()) {
                    uint8_t* const buffer = spare.back();
                    spare.pop_back();
                    return buffer;
                }
                if (buffers.size() + carved < bufferLimit) return NewBuffer();
            }
            if (Segment* const segment = Claim(false)) {
                Encode(*segment, false);
                continue;
            }
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&]() { return !spare.empty(); });
        }
    }

    // The segment that the plane's next byte goes to, with its buffer; null on a failed allocation.
    Segment* Writing(size_t plane) {
        Segment& segment = segments[planeSegments[plane][writing[plane]]];
        if (!segment.input) {
            uint8_t* const buffer = failed ? nullptr : AcquireBuffer();
            if (!buffer) {
                failed = true;
                return nullptr;
            }
            std::memcpy(buffer, tail[plane], segment.history);
            segment.input = buffer;   // the helpers read it after the bytes it holds are published
        }
        return &segment;
    }

    // The plane's next `bytes` bytes are in its segment's buffer: they are published, and the end
    // of a segment is kept for the next one.
    void Advance(size_t plane, size_t bytes) {
        Segment& segment = segments[planeSegments[plane][writing[plane]]];
        size_t& at = position[plane];
        at += bytes;
        if (at == segment.start + segment.size) {
            if (writing[plane] + 1 < planeSegments[plane].size()) {
                const Segment& next = segments[planeSegments[plane][writing[plane] + 1]];
                std::memcpy(tail[plane], segment.input + segment.history + segment.size - next.history, next.history);
            }
            writing[plane]++;
        }
        ready[plane].store(at);   // sequentially consistent with the sleepers' count
        size_t& start = wakeStart[plane];
        bool crossed = false;
        while (start < sizes[plane]) {
            const size_t size = RayoMd::Flate::ImageSegmentSize(sizes[plane] - start);
            if (at < ClaimAt(start, size, true)) break;
            start += size;
            crossed = true;
        }
        if (crossed && sleepers.load() > 0) Wake();
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

ImagePlanesEncoder::ImagePlanesEncoder(const size_t* sizes, size_t count, unsigned helpers, bool fast)
    : state_(new State) {
    State& state = *state_;
    state.count = count;
    state.fast = fast;
    for (size_t plane = 0; plane < count; plane++) {
        state.sizes[plane] = sizes[plane];
        state.ready[plane].store(0, std::memory_order_relaxed);
        size_t start = 0;
        size_t slot = 2;   // behind the zlib header
        do {
            Segment segment;
            segment.plane = plane;
            segment.start = start;
            segment.size = RayoMd::Flate::ImageSegmentSize(sizes[plane] - start);
            segment.history = std::min(start, kHistory);
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
    for (size_t index = 0; index < state.segments.size(); index++) {
        state.planeSegments[state.segments[index].plane].push_back(index);   // in plane order, as sorting kept it
    }
    state.claimed.reset(new std::atomic<bool>[state.segments.size()]);
    for (size_t index = 0; index < state.segments.size(); index++) state.claimed[index].store(false, std::memory_order_relaxed);
    helpers = (unsigned)std::min<size_t>(std::min(helpers, kMaxHelpers), state.segments.size());
    state.bufferLimit = count + helpers + 1;
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

uint8_t* ImagePlanesEncoder::Reserve(size_t plane, size_t bytes) {
    State& state = *state_;
    const size_t at = state.position[plane];
    if (bytes == 0 || at + bytes > state.sizes[plane]) return nullptr;
    Segment* const segment = state.Writing(plane);
    if (!segment || at + bytes > segment->start + segment->size) return nullptr;
    return segment->input + segment->history + (at - segment->start);
}

void ImagePlanesEncoder::Commit(size_t plane, size_t bytes) { state_->Advance(plane, bytes); }

bool ImagePlanesEncoder::Append(size_t plane, const uint8_t* bytes, size_t count) {
    State& state = *state_;
    if (state.position[plane] + count > state.sizes[plane]) return false;
    while (count > 0) {
        Segment* const segment = state.Writing(plane);
        if (!segment) return false;
        const size_t at = state.position[plane];
        const size_t piece = std::min(count, segment->start + segment->size - at);
        std::memcpy(segment->input + segment->history + (at - segment->start), bytes, piece);
        state.Advance(plane, piece);
        bytes += piece;
        count -= piece;
    }
    return true;
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
    if (state.failed) return false;
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
        for (const size_t index : state.planeSegments[plane]) {
            const Segment& segment = state.segments[index];
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
