#include "webgl/client/webgl_client.h"

#include "webgl/vulkan/webgl_vk_context.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>

namespace bro::webgl {

WebGLClient::WebGLClient() { ring_.setFullHook(&WebGLClient::onRingFull, this); }

WebGLClient::~WebGLClient() {
    // Nothing pending matters once the context goes: free what it owns.
    ring_.publish();
    ring_.consume([this](Op op, const uint8_t* rec) { replayWebGLCommand(nullptr, op, rec, blobBytes_); });
    backend_.reset();
}

void WebGLClient::attach(std::unique_ptr<vk::WebGLVkContext> backend) {
    drain();
    backend_ = std::move(backend);
}

void WebGLClient::detach() {
    drain();
    backend_.reset();
}

void WebGLClient::flush() {
    if (!backend_) return;
    encodeSimple(ring_, Op::Flush, TypeList<>{});
    drain();
}

void WebGLClient::finish() {
    if (!backend_) return;
    sync([](vk::WebGLVkContext& gl) { gl.finish(); });
}

void WebGLClient::replayPending() {
    assert(!replaying_ && "a replay reached the client it replays for");
    replaying_ = true;
    ring_.publish();
    vk::WebGLVkContext* gl = backend_.get();
    ring_.consume([this, gl](Op op, const uint8_t* rec) { replayWebGLCommand(gl, op, rec, blobBytes_); });
    replaying_ = false;
}

void WebGLClient::onRingFull(void* self) { static_cast<WebGLClient*>(self)->replayPending(); }

void* WebGLClient::copyToBlob(const void* data, uint64_t bytes) {
    // Over the cap, the replays free what they hold first (inline replay
    // frees all of it); a payload larger than the cap alone is still taken.
    if (blobBytes_.load(std::memory_order_acquire) + bytes > kBlobCap) drain();
    void* blob = std::malloc(static_cast<size_t>(bytes));
    if (!blob) throw std::bad_alloc();
    std::memcpy(blob, data, static_cast<size_t>(bytes));
    blobBytes_.fetch_add(static_cast<size_t>(bytes), std::memory_order_relaxed);
    return blob;
}

}  // namespace bro::webgl
