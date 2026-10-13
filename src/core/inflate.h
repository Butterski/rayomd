#pragma once

// A zlib (RFC 1950) and DEFLATE (RFC 1951) decoder that hands out its output a piece at a time,
// for the PNG rows RayoMD re-encodes: the stream is in memory, the output goes through a window
// of the last 32 KB plus a chunk, so a whole image is never inflated at once. It accepts what
// zlib's uncompress() accepts for an output of known size: a header without a preset dictionary
// and with a window up to 32 KB, codes no longer than the format allows (an incomplete code only
// for a single code of one bit, or a distance code with no codes at all), no distance past the
// start of the output, deflate data that ends exactly at that size, and its Adler-32. No
// dependency.

#include <cstddef>
#include <cstdint>
#include <memory>

namespace RayoMd::Inflate {

class ZlibReader {
public:
    ZlibReader();
    ~ZlibReader();
    ZlibReader(const ZlibReader&) = delete;
    ZlibReader& operator=(const ZlibReader&) = delete;

    // Starts on the zlib stream data[0, size), which must inflate to exactly `outputSize` bytes,
    // handed out at most `largestTake` at a time; once per reader. False for a header zlib would
    // refuse or when the window cannot be allocated.
    bool Start(const uint8_t* data, size_t size, size_t outputSize, size_t largestTake);

    // The next `bytes` bytes of output (at most `largestTake`), valid until the next call; null
    // when the stream is malformed or ends short of them.
    const uint8_t* Take(size_t bytes);

    // After all the output was taken: true when the deflate data ends there and is followed by
    // its Adler-32.
    bool Finish();

    struct State;

private:
    std::unique_ptr<State> state_;
};

} // namespace RayoMd::Inflate
