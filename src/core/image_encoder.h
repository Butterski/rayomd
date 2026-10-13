#pragma once

// The zlib streams of a re-encoded image's planes (the /Predictor 15 rows of its colour and
// alpha), compressed in segments (flate.h) while the decoder still writes later rows. The rows go
// straight into the buffer of the segment they belong to, and a segment's buffer is reused once it
// is compressed, so a plane is never held whole: the decoder waits, or compresses itself, while
// as many buffers as threads plus two are full. Up to three helper threads, kept for the whole
// process and shared by every encoder, take each segment once its first block is written and
// follow the decoder; the decoder compresses what is left in Finish. Each stream is
// RayoMd::Flate::ZlibCompressImage of the plane's bytes, whatever the number of helpers.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace TinyPdf::Internal {

class ImagePlanesEncoder {
public:
    static constexpr size_t kMaxPlanes = 2;
    // The helper threads of the process, shared by every encoder.
    static constexpr unsigned kMaxHelpers = 3;

    // Planes of `sizes[i]` bytes, written in order; up to `helpers` helper threads (at most
    // kMaxHelpers) are asked to join in.
    ImagePlanesEncoder(const size_t* sizes, size_t count, unsigned helpers);
    // Abandons what is not finished and waits for the helpers to leave.
    ~ImagePlanesEncoder();
    ImagePlanesEncoder(const ImagePlanesEncoder&) = delete;
    ImagePlanesEncoder& operator=(const ImagePlanesEncoder&) = delete;

    // Where to write the plane's next `bytes` bytes when they fall in one segment, then Commit
    // them; null when they do not (Append them instead) or a buffer could not be allocated.
    uint8_t* Reserve(size_t plane, size_t bytes);
    // The `bytes` bytes written where Reserve pointed are final.
    void Commit(size_t plane, size_t bytes);
    // Copies the plane's next `count` bytes in; false when a buffer could not be allocated.
    bool Append(size_t plane, const uint8_t* bytes, size_t count);
    // Plane `plane` is not wanted: its segments are skipped and it gets no stream.
    void Abandon(size_t plane);
    // Compresses the segments whose bytes are all written, on this thread: a decoder without
    // helpers that calls this as it goes finds them still in its cache.
    void EncodeReady();
    // Compresses beside the helpers until every segment is done, then gives the streams of the
    // planes that were written whole (an abandoned one stays empty). False when a buffer could not
    // be allocated. Every plane must be written whole or abandoned first.
    bool Finish(std::string* streams);

    struct State;   // opaque; the helpers hold it too

private:
    std::unique_ptr<State> state_;
};

} // namespace TinyPdf::Internal
