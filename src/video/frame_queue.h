#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <vector>

namespace bro::video {

// Single-producer single-consumer bounded ring. One writer thread (e.g. the
// decoder) pushes; one reader thread (e.g. the render loop) pops the next
// item once its presentation time is due. No locks.
//
// Capacity is fixed at construction and should be a power of two for the
// wrap mask; the constructor rounds up. T must be move-constructible and
// move-assignable.
template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity) {
        size_t cap = 1;
        while (cap < capacity) cap <<= 1;
        capacity_ = cap;
        mask_ = cap - 1;
        slots_.resize(cap);
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    size_t capacity() const { return capacity_; }

    bool tryPush(T item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next = tail + 1;
        if (next - head_.load(std::memory_order_acquire) > capacity_) return false;
        slots_[tail & mask_] = std::move(item);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool tryPop(T& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        out = std::move(slots_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Inspect the next element without removing it. Safe only from the
    // consumer thread.
    const T* peek() const {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return nullptr;
        return &slots_[head & mask_];
    }

    size_t sizeApprox() const {
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    size_t capacity_ = 0;
    size_t mask_ = 0;
    std::vector<T> slots_;
};

} // namespace bro::video
