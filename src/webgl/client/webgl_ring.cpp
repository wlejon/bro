#include "webgl/client/webgl_ring.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <new>

namespace bro::webgl {

WebGLRing::WebGLRing(size_t capacity) {
    size_t cap = size_t{64} * 1024;
    while (cap < capacity) cap <<= 1;
    cap_ = cap;
    base_ = static_cast<uint8_t*>(std::aligned_alloc(64, cap_));
    if (!base_) throw std::bad_alloc();
    cur_ = base_;
    limit_ = base_ + cap_;
}

WebGLRing::~WebGLRing() { std::free(base_); }

// The slow half of reserve: wrap to the next lap when the record does not fit
// the tail, and ask the owner to consume while there is not enough free space.
uint8_t* WebGLRing::makeRoom(size_t bytes) {
    assert(bytes % 8 == 0 && bytes <= cap_);
    for (;;) {
        const uint64_t w = writePos();
        const uint64_t r = read_.load(std::memory_order_acquire);
        const size_t free = cap_ - static_cast<size_t>(w - r);
        const size_t tail = cap_ - static_cast<size_t>(cur_ - base_);
        if (tail < bytes) {
            if (free >= tail) {
                // The tail is the consumer's to skip: a Wrap header (every
                // record is a multiple of 8, so a non-empty tail holds one).
                if (tail > 0) cmd::storeHeader(cur_, Op::Wrap, 0);
                lap_ += cap_;
                cur_ = base_;
                continue;
            }
        } else if (free >= bytes) {
            limit_ = cur_ + std::min(free, tail);
            return cur_;
        }
        // Full: what is written goes to the consumer, which must make room.
        publish();
        if (fullFn_) fullFn_(fullOwner_);
        if (read_.load(std::memory_order_acquire) == r) {
            assert(!"WebGLRing: the full hook consumed nothing");
            std::abort();
        }
    }
}

}  // namespace bro::webgl
