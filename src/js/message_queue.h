#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bro::js {

/// A heavyweight object transferred by pointer across a postMessage boundary.
/// Used for types whose data lives in C++ (e.g. Mesh) so they can cross thread
/// boundaries with no JS-heap round-trip. The Message owns the pointer until
/// deserialize extracts it and transfers ownership to a destination wrapper.
struct TransferredObject {
    enum Type : uint8_t {
        kNone        = 0,
        kMesh        = 1,  // bromesh::MeshData*
        kImageBitmap = 2,  // SkImage* (one owned ref)
    };

    uint8_t type = kNone;
    void* ptr = nullptr;
    void (*deleter)(void*) = nullptr;

    TransferredObject() = default;
    TransferredObject(uint8_t t, void* p, void (*d)(void*))
        : type(t), ptr(p), deleter(d) {}

    TransferredObject(const TransferredObject&) = delete;
    TransferredObject& operator=(const TransferredObject&) = delete;

    TransferredObject(TransferredObject&& o) noexcept
        : type(o.type), ptr(o.ptr), deleter(o.deleter) {
        o.type = kNone; o.ptr = nullptr; o.deleter = nullptr;
    }
    TransferredObject& operator=(TransferredObject&& o) noexcept {
        if (this != &o) {
            if (ptr && deleter) deleter(ptr);
            type = o.type; ptr = o.ptr; deleter = o.deleter;
            o.type = kNone; o.ptr = nullptr; o.deleter = nullptr;
        }
        return *this;
    }
    ~TransferredObject() {
        if (ptr && deleter) deleter(ptr);
    }

    /// Relinquish ownership of the payload without deleting it. The caller
    /// becomes responsible for deleting via the returned (ptr, deleter) pair.
    void* release() noexcept {
        void* p = ptr;
        ptr = nullptr;
        deleter = nullptr;
        type = kNone;
        return p;
    }
};

/// A serialized message for cross-thread postMessage.
struct Message {
    std::vector<uint8_t> data;                             // Serialized payload
    std::vector<std::vector<uint8_t>> transferredBuffers;  // Transferred ArrayBuffers (byte copy today)
    std::vector<TransferredObject> transferredObjects;     // Transferred opaque C++ objects (true zero-copy)
};

/// Single-producer, single-consumer lock-free message queue.
/// Uses atomics only — no mutexes. Power-of-two capacity for fast modulo.
class MessageQueue {
public:
    static constexpr size_t kCapacity = 256;

    /// Push a heap-allocated Message. Returns false if full.
    /// The queue takes ownership of the pointer on success.
    bool push(Message* msg) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) & (kCapacity - 1);
        if (next == head_.load(std::memory_order_acquire))
            return false;  // full
        slots_[tail] = msg;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    /// Pop a Message. Returns nullptr if empty.
    /// Caller takes ownership and must delete the returned pointer.
    Message* pop() {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire))
            return nullptr;  // empty
        Message* msg = slots_[head];
        slots_[head] = nullptr;
        head_.store((head + 1) & (kCapacity - 1), std::memory_order_release);
        return msg;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    /// Drain and delete all remaining messages (for cleanup).
    void clear() {
        while (Message* msg = pop())
            delete msg;
    }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    Message* slots_[kCapacity]{};
};

}  // namespace bro::js
