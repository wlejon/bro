#include "webgl/client/webgl_client.h"

#include "webgl/vulkan/webgl_vk_context.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>

namespace bro::webgl {

WebGLClient::WebGLClient(const vk::WebGLCaps& caps, render::VulkanQueue& queue) : caps_(caps), queue_(queue) {
    ring_.setFullHook(&WebGLClient::onRingFull, this);
}

WebGLClient::~WebGLClient() {
    // Nothing pending matters once the context goes: free what it owns.
    ring_.publish();
    ring_.consume([this](Op op, const uint8_t* rec) { replayWebGLCommand(nullptr, op, rec, replay_); });
    replay_.clear();
    backend_.reset();
}

void WebGLClient::attach(std::unique_ptr<vk::WebGLVkContext> backend) {
    drain();
    forgetObjects();
    backend_ = std::move(backend);
}

void WebGLClient::detach() {
    drain();
    replay_.clear();
    forgetObjects();
    backend_.reset();
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
    ring_.consume([this, gl](Op op, const uint8_t* rec) { replayWebGLCommand(gl, op, rec, replay_); });
    // A batch's end: what it changed is published (a consumer thread's loop
    // turn, later).
    if (gl) replay_.publish(*gl);
    replaying_ = false;
}

void WebGLClient::onRingFull(void* self) { static_cast<WebGLClient*>(self)->replayPending(); }

void WebGLClient::reserveBlobBytes(uint64_t bytes) {
    // Over the cap, the replays free what they hold first (inline replay
    // frees all of it); a payload larger than the cap alone is still taken.
    if (replay_.blobBytes.load(std::memory_order_acquire) + bytes > kBlobCap) drain();
    replay_.blobBytes.fetch_add(static_cast<size_t>(bytes), std::memory_order_relaxed);
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
    encodeSimple(ring_, Op::TexImage2DSource, rec::kTexImage2DSource, target, level, internalformat, width, height,
                 format, type, ref);
}

void WebGLClient::texSubImage2DSource(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                      GLsizei height, GLenum format, GLenum type, rec::PixelRef pixels) {
    void* ref = new rec::PixelRef(std::move(pixels));
    encodeSimple(ring_, Op::TexSubImage2DSource, rec::kTexSubImage2DSource, target, level, xoffset, yoffset, width,
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
