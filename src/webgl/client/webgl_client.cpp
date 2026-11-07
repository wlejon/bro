#include "webgl/client/webgl_client.h"

#include "webgl/client/webgl_thread.h"
#include "render/vulkan_context.h"
#include "webgl/vulkan/webgl_vk_context.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>

namespace bro::webgl {

WebGLClient::WebGLClient(std::shared_ptr<WebGLThread> thread, render::VulkanContext& device,
                         std::shared_ptr<const vk::WebGLCaps> caps, const ContextAttributes& attributes)
    : thread_(std::move(thread)), channel_(device, caps, attributes), capsRef_(std::move(caps)), caps_(*capsRef_),
      queue_(device.queue()) {
    channel_.ring.setHooks(&WebGLClient::onRingFull, &WebGLClient::onPublish, this);
    thread_->add(channel_);
}

WebGLClient::~WebGLClient() {
    // Everything before it replays (the backend's destructor waits for its
    // own submissions), then the WebGL thread lets the channel go.
    encodeSimple(channel_.ring, Op::Destroy, TypeList<>{});
    channel_.ring.publish();
    thread_->awaitDestroyed(channel_);
    channel_.ring.setHooks(nullptr, nullptr, nullptr);
}

void WebGLClient::attach(int width, int height) {
    forgetObjects();
    encodeSimple(channel_.ring, Op::Attach, rec::kAttach, width, height);
    channel_.ring.publish();
    attached_ = true;
}

void WebGLClient::detach() {
    forgetObjects();
    encodeSimple(channel_.ring, Op::Detach, TypeList<>{});
    channel_.ring.publish();
    attached_ = false;
}

void WebGLClient::forgetObjects() {
    programs_.clear();
    shaders_.clear();
    queries_.clear();
    fences_.clear();
    for (GLuint& q : activeQuery_) q = 0;
    extensions_.clear();
}

void WebGLClient::flush() {
    if (!attached_) return;
    encodeSimple(channel_.ring, Op::Flush, TypeList<>{});
    channel_.ring.publish();
}

void WebGLClient::settle() {
    if (!attached_ || channel_.ring.written() == settledAt_) return;
    flushThen([](vk::WebGLVkContext&) {});
}

void WebGLClient::finish() {
    if (!attached_) return;
    sync([](vk::WebGLVkContext& gl) { gl.finish(); });
}

void WebGLClient::runSync(WebGLSyncFn fn, void* ctx) {
    const uint64_t seq = ++syncs_;
    encodeSimple(channel_.ring, Op::SyncCall, rec::kSyncCall, fn, ctx, seq);
    channel_.ring.publish();
    channel_.progress.wait([&] { return channel_.syncsAnswered.load(std::memory_order_acquire) >= seq; },
                           kWaitSpin);
}

// The ring has no room: the WebGL thread frees some as it replays.
void WebGLClient::onRingFull(void* self, uint64_t readSeen) {
    WebGLChannel& ch = static_cast<WebGLClient*>(self)->channel_;
    ch.progress.wait([&] { return ch.ring.readPosition() != readSeen; }, kWaitSpin);
}

void WebGLClient::onPublish(void* self) { static_cast<WebGLClient*>(self)->thread_->wake(); }

void WebGLClient::reserveBlobBytes(uint64_t bytes) {
    // Over the cap, wait for the replay to free what it holds; a payload
    // larger than the cap alone is still taken once nothing else is held.
    std::atomic<size_t>& held = channel_.replay.blobBytes;
    waitFor([&] {
        const size_t now = held.load(std::memory_order_acquire);
        return now == 0 || now + bytes <= kBlobCap;
    });
    held.fetch_add(static_cast<size_t>(bytes), std::memory_order_relaxed);
}

void* WebGLClient::copyToBlob(const void* data, uint64_t bytes) {
    reserveBlobBytes(bytes);
    void* blob = std::malloc(static_cast<size_t>(bytes));
    if (!blob) throw std::bad_alloc();
    std::memcpy(blob, data, static_cast<size_t>(bytes));
    return blob;
}

// --- DOM sources and lists --------------------------------------------------

rec::PixelRef WebGLClient::copiedPixels(const uint8_t* rgba, uint32_t width, uint32_t height) {
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    reserveBlobBytes(bytes);
    std::shared_ptr<uint8_t> copy(new uint8_t[bytes], std::default_delete<uint8_t[]>());
    std::memcpy(copy.get(), rgba, bytes);
    rec::PixelRef ref;
    ref.rgba = std::move(copy);
    ref.width = width;
    ref.height = height;
    ref.counted = bytes;
    return ref;
}

rec::PixelRef WebGLClient::heldPixels(std::shared_ptr<const uint8_t> rgba, uint32_t width, uint32_t height) {
    rec::PixelRef ref;
    ref.rgba = std::move(rgba);
    ref.width = width;
    ref.height = height;
    return ref;
}

void WebGLClient::texImage2DSource(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                                   GLenum format, GLenum type, rec::PixelRef pixels) {
    void* ref = new rec::PixelRef(std::move(pixels));
    encodeSimple(channel_.ring, Op::TexImage2DSource, rec::kTexImage2DSource, target, level, internalformat, width, height,
                 format, type, ref);
}

void WebGLClient::texSubImage2DSource(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                      GLsizei height, GLenum format, GLenum type, rec::PixelRef pixels) {
    void* ref = new rec::PixelRef(std::move(pixels));
    encodeSimple(channel_.ring, Op::TexSubImage2DSource, rec::kTexSubImage2DSource, target, level, xoffset, yoffset, width,
                 height, format, type, ref);
}

void WebGLClient::invalidateFramebuffer(GLenum target, const GLenum* attachments, size_t n) {
    putPayload(Op::InvalidateFramebuffer, rec::kInvalidateFramebuffer, attachments, n * sizeof(GLenum), target);
}

void WebGLClient::invalidateSubFramebuffer(GLenum target, const GLenum* attachments, size_t n, GLint x, GLint y,
                                           GLsizei width, GLsizei height) {
    putPayload(Op::InvalidateSubFramebuffer, rec::kInvalidateSubFramebuffer, attachments, n * sizeof(GLenum), target,
               x, y, width, height);
}

void WebGLClient::transformFeedbackVaryings(GLuint program, const std::vector<std::string>& varyings,
                                            GLenum bufferMode) {
    std::string joined;
    for (const std::string& v : varyings) {
        joined += v;
        joined.push_back('\0');
    }
    putPayload(Op::TransformFeedbackVaryings, rec::kTransformFeedbackVaryings, joined.data(), joined.size(), program,
               bufferMode, static_cast<uint32_t>(varyings.size()));
}

}  // namespace bro::webgl
