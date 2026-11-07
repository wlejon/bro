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
// (publish), at its sync points and every kPublishEvery bytes (the reserve's
// slow path, so the fast path stays one compare); the consumer makes the
// space it is done with reusable with one release store of the read
// position, every kPublishEvery bytes and at the end of a consume. Nothing
// is published per record. Each publish calls the owner's notify hook (the
// WebGL thread's wake, which costs a fence and a load while it is awake).
//
// When the ring has no room the producer publishes and calls the owner's
// full hook, which must not return before the consumer has moved the read
// position past what the hook was given (the client waits for the WebGL
// thread; a single-threaded test consumes itself).

#include "webgl/client/webgl_cmd.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bro::webgl {

class WebGLRing {
public:
    using FullFn = void (*)(void* owner, uint64_t readSeen);
    using NotifyFn = void (*)(void* owner);

    static constexpr size_t kDefaultCapacity = size_t{1} << 20;
    /// How many bytes either side moves before it publishes its position.
    static constexpr size_t kPublishEvery = 4096;

    /// `capacity` is rounded up to a power of two (at least 64 KiB).
    explicit WebGLRing(size_t capacity = kDefaultCapacity);
    ~WebGLRing();

    WebGLRing(const WebGLRing&) = delete;
    WebGLRing& operator=(const WebGLRing&) = delete;

    /// What the producer calls when the ring is full, and after each
    /// publish (see above).
    void setHooks(FullFn full, NotifyFn notify, void* owner) {
        fullFn_ = full;
        notifyFn_ = notify;
        owner_ = owner;
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

    /// Make every record reserved so far visible to the consumer, and call
    /// the notify hook.
    void publish() {
        const uint64_t w = writePos();
        lastPublished_ = w;
        published_.store(w, std::memory_order_release);
        if (notifyFn_) notifyFn_(owner_);
    }

    /// Whether anything written is not yet consumed (the producer's view).
    bool pending() const { return writePos() != read_.load(std::memory_order_acquire); }
    /// Bytes written so far, ever (the producer's view).
    uint64_t written() const { return writePos(); }
    /// Whether anything written is not yet published (the producer's view).
    bool unpublished() const { return writePos() != lastPublished_; }

    /// The consumer's position (either side).
    uint64_t readPosition() const { return read_.load(std::memory_order_acquire); }

    // --- Consumer ---

    /// Whether records are published that consume() has not taken.
    bool hasPublished() const {
        return published_.load(std::memory_order_acquire) != read_.load(std::memory_order_relaxed);
    }

    size_t capacity() const { return cap_; }

    /// Call f(op, record) for every published record, in order, then make
    /// their space reusable. Returns how many records were consumed.
    template <class F>
    size_t consume(F&& f);

private:
    uint64_t writePos() const { return lap_ + static_cast<uint64_t>(cur_ - base_); }
    uint8_t* makeRoom(size_t bytes);

    // Read by both sides, written only at construction (and the hooks, by
    // the producer's owner before use).
    uint8_t* base_ = nullptr;
    size_t cap_ = 0;
    FullFn fullFn_ = nullptr;
    NotifyFn notifyFn_ = nullptr;
    void* owner_ = nullptr;

    // The producer's own, on a line of their own (the consumer reading the
    // line the producer writes every record would cost the producer a
    // transfer per record): its cursor, the end of the room it last found (or
    // of the bytes it may write before it publishes again), the position of
    // base_ in the lap it is writing, and what it last published.
    alignas(64) uint8_t* cur_ = nullptr;
    uint8_t* limit_ = nullptr;
    uint64_t lap_ = 0;
    uint64_t lastPublished_ = 0;

    // One line each: the producer's position, and the consumer's (the
    // class's 64-byte alignment pads the last line, so nothing after the ring
    // shares it).
    alignas(64) std::atomic<uint64_t> published_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

template <class F>
size_t WebGLRing::consume(F&& f) {
    uint64_t r = read_.load(std::memory_order_relaxed);
    const uint64_t w = published_.load(std::memory_order_acquire);
    const uint64_t mask = cap_ - 1;
    uint64_t stored = r;
    size_t n = 0;
    while (r < w) {
        if (r - stored >= kPublishEvery) {
            read_.store(r, std::memory_order_release);
            stored = r;
        }
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
