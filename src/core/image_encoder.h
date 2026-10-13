#pragma once

// The zlib streams of a re-encoded image's planes (the /Predictor 15 rows of its colour and
// alpha), compressed in segments (flate.h) while the decoder still writes later rows. Up to three
// helper threads, kept for the whole process and shared by every encoder, take each segment once
// its bytes are published; the caller compresses what is left in Finish. Each stream is
// RayoMd::Flate::ZlibCompressImage of the plane's bytes, whatever the number of helpers.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace TinyPdf::Internal {

class ImagePlanesEncoder {
public:
    static constexpr size_t kMaxPlanes = 2;

    // Plane i's `sizes[i]` bytes at `planes[i]`, written in order; up to `helpers` helper threads
    // (at most three) are asked to join in.
    ImagePlanesEncoder(const uint8_t* const* planes, const size_t* sizes, size_t count, unsigned helpers);
    // Abandons what is not finished and waits for the helpers to leave.
    ~ImagePlanesEncoder();
    ImagePlanesEncoder(const ImagePlanesEncoder&) = delete;
    ImagePlanesEncoder& operator=(const ImagePlanesEncoder&) = delete;

    // Plane `plane`'s first `bytes` bytes are final (stored with release order after them).
    void Publish(size_t plane, size_t bytes);
    // Plane `plane` is not wanted: its segments are skipped and it gets no stream.
    void Abandon(size_t plane);
    // Compresses the next segments on this thread while their bytes are all published: a decoder
    // without helpers that calls this as it goes finds the bytes still in its cache.
    void EncodeReady();
    // Compresses beside the helpers until every segment is done, then gives the streams of the
    // planes that were published whole (an abandoned one stays empty). False when an output
    // buffer could not be allocated. Every plane must be published whole or abandoned first.
    bool Finish(std::string* streams);

    struct State;   // opaque; the helpers hold it too

private:
    std::unique_ptr<State> state_;
};

} // namespace TinyPdf::Internal
