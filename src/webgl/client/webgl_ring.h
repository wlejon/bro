#pragma once

// WebGLRing: the byte ring a WebGL context's commands travel in, one
// producer (the JS thread's WebGLClient) and one consumer (the replay).
//
// Records (webgl_cmd.h) never straddle the end: one that does not fit the
// lap's tail leaves a Wrap header there and starts the next lap. Positions
// are monotonic byte counts (a record's offset is its position modulo the
// power-of-two capacity), so empty is read == written and the free space is
// capacity - (written - read).
//
// The data plane is lock-free: the producer writes through a private cursor
// and makes its records visible with one release store of the write position
// (publish); the consumer makes the space it is done with reusable with one
// release store of the read position. Nothing is published per record.
//
// When the ring has no room the producer publishes and calls the owner's
// full hook, which must consume something before it returns: today the
// client replays everything on its own thread (inline replay); with a
// consumer thread it waits for the read position to move.

#include "webgl/client/webgl_cmd.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bro::webgl {

class WebGLRing {
public:
    using FullFn = void (*)(void* owner);

    static constexpr size_t kDefaultCapacity = size_t{1} << 20;

    /// `capacity` is rounded up to a power of two (at least 64 KiB).
    explicit WebGLRing(size_t capacity = kDefaultCapacity);
    ~WebGLRing();

    WebGLRing(const WebGLRing&) = delete;
    WebGLRing& operator=(const WebGLRing&) = delete;

    /// What the producer calls when the ring is full (see above).
    void setFullHook(FullFn fn, void* owner) {
        fullFn_ = fn;
        fullOwner_ = owner;
    }

    // --- Producer ---

    /// Room for one record of `bytes` (a multiple of 8): the caller writes it
    /// before anything else reserves or publishes.
    uint8_t* reserve(size_t bytes) {
        uint8_t* p = cur_;
        if (static_cast<size_t>(limit_ - p) < bytes) [[unlikely]]
            p = makeRoom(bytes);
        cur_ = p + bytes;
        return p;
    }

    /// Make every record reserved so far visible to the consumer.
    void publish() { published_.store(writePos(), std::memory_order_release); }

    /// Whether anything written is not yet consumed (the producer's view).
    bool pending() const { return writePos() != read_.load(std::memory_order_acquire); }

    size_t capacity() const { return cap_; }

    // --- Consumer ---

    /// Call f(op, record) for every published record, in order, then make
    /// their space reusable. Returns how many records were consumed.
    template <class F>
    size_t consume(F&& f);

private:
    uint64_t writePos() const { return lap_ + static_cast<uint64_t>(cur_ - base_); }
    uint8_t* makeRoom(size_t bytes);

    uint8_t* base_ = nullptr;
    size_t cap_ = 0;
    FullFn fullFn_ = nullptr;
    void* fullOwner_ = nullptr;

    // The producer's own: its cursor, the end of the room it last found, and
    // the position of base_ in the lap it is writing.
    uint8_t* cur_ = nullptr;
    uint8_t* limit_ = nullptr;
    uint64_t lap_ = 0;

    alignas(64) std::atomic<uint64_t> published_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

template <class F>
size_t WebGLRing::consume(F&& f) {
    uint64_t r = read_.load(std::memory_order_relaxed);
    const uint64_t w = published_.load(std::memory_order_acquire);
    const uint64_t mask = cap_ - 1;
    size_t n = 0;
    while (r < w) {
        const size_t off = static_cast<size_t>(r & mask);
        const uint8_t* rec = base_ + off;
        const CmdHeader h = cmd::load<CmdHeader>(rec);
        if (h.op == static_cast<uint16_t>(Op::Wrap)) {
            r += cap_ - off;
            continue;
        }
        f(static_cast<Op>(h.op), rec);
        r += static_cast<uint64_t>(h.words) * 8;
        ++n;
    }
    read_.store(r, std::memory_order_release);
    return n;
}

}  // namespace bro::webgl
