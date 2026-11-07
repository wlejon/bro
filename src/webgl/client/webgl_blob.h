#pragma once

// WebGLBlob / WebGLBlobPool: the memory a payload larger than
// kInlinePayloadMax travels in, reused from upload to upload.
//
// The client (JS thread) takes a blob, copies the call's data into it and
// encodes a pointer to it; the replay (WebGL thread) hands its bytes to the
// backend and gives the blob back. A frame's large uploads therefore land in
// memory a previous frame already touched: no allocation, no page faults.
// A backend call that replaces a buffer's whole contents may keep the
// blob's vector as the buffer's CPU copy and return the old copy in it (a
// swap, vk::WebGLVkContext::bufferSubDataTake), so an upload the size of its
// buffer costs the WebGL thread one copy, into staging.
//
// The return path is lock-free: the WebGL thread pushes onto a stack the
// client takes whole (one exchange), so there is no ABA. The client keeps
// at most kKeepBytes of returned blobs; the rest are freed as they come back.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bro::webgl {

struct WebGLBlob {
    std::vector<uint8_t> bytes;
    WebGLBlob* next = nullptr;  // on the return stack
};

class WebGLBlobPool {
public:
    /// Returned blobs the client keeps for reuse at most, in capacity.
    static constexpr size_t kKeepBytes = size_t{32} << 20;

    WebGLBlobPool() = default;
    /// Frees every blob kept or returned (none may still be in a record).
    ~WebGLBlobPool();

    WebGLBlobPool(const WebGLBlobPool&) = delete;
    WebGLBlobPool& operator=(const WebGLBlobPool&) = delete;

    /// The client's: a blob of exactly `bytes` (contents unspecified), a
    /// returned one whose capacity fits when there is one.
    WebGLBlob* take(size_t bytes);

    /// The replay's: `blob` is done with (its size may have changed).
    void give(WebGLBlob* blob) {
        WebGLBlob* head = returned_.load(std::memory_order_relaxed);
        do blob->next = head;
        while (!returned_.compare_exchange_weak(head, blob, std::memory_order_release, std::memory_order_relaxed));
    }

private:
    void collect();

    alignas(64) std::atomic<WebGLBlob*> returned_{nullptr};
    // The client's own.
    alignas(64) std::vector<WebGLBlob*> kept_;
    size_t keptBytes_ = 0;
};

}  // namespace bro::webgl
