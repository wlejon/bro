#include "webgl/webgl2_context.h"
#include "webgl/vulkan/webgl_vk_context.h"

namespace bro::webgl {

WebGL2RenderingContext::WebGL2RenderingContext(int width, int height, render::VulkanContext& device)
    : width_(width), height_(height), device_(device),
      backend_(std::make_unique<vk::WebGLVkContext>(width, height, device)) {}

WebGL2RenderingContext::~WebGL2RenderingContext() {
    for (auto& cb : teardownCallbacks_)
        if (cb) cb(this);
    teardownCallbacks_.clear();
}

void WebGL2RenderingContext::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    if (backend_) backend_->resize(width, height);
}

VkImage WebGL2RenderingContext::colorImage() const {
    return backend_ ? backend_->canvas().colorImage() : VK_NULL_HANDLE;
}

VkImageLayout WebGL2RenderingContext::colorLayout() const {
    return backend_ ? backend_->canvas().colorLayout() : VK_IMAGE_LAYOUT_UNDEFINED;
}

void WebGL2RenderingContext::flush() {
    if (backend_) backend_->flush();
}

bool WebGL2RenderingContext::readCanvasPixels(std::vector<uint8_t>& out) {
    return backend_ && backend_->readCanvasPixels(out);
}

// WebGL 1.0 5.15.3 "lose the context": the backend and every object go now;
// the lost event is the engine's to fire, at the canvas, after this turn.
void WebGL2RenderingContext::loseContext() {
    if (lost_) return;  // its INVALID_OPERATION is unobservable: getError answers NO_ERROR while lost
    nextObjectId_ = backend_->nextObjectId();
    backend_.reset();
    lost_ = true;
    lostErrorPending_ = true;
    restoreAllowed_ = true;
    pendingEvent_ = ContextEvent::Lost;
}

void WebGL2RenderingContext::restoreContext() {
    if (!lost_) {
        backend_->setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    // Before the lost event has fired, or after it was not cancelled, there
    // is nothing to restore yet / at all.
    if (restoreAllowed_ && pendingEvent_ == ContextEvent::None) pendingEvent_ = ContextEvent::Restored;
}

WebGL2RenderingContext::ContextEvent WebGL2RenderingContext::takeContextEvent() {
    const ContextEvent e = pendingEvent_;
    pendingEvent_ = ContextEvent::None;
    if (e == ContextEvent::Restored) {
        // WebGL 1.0 5.15.4 "restore the context": a fresh drawing buffer and
        // default state; names continue, so no lost object's names a new one.
        backend_ = std::make_unique<vk::WebGLVkContext>(width_, height_, device_, nextObjectId_);
        lost_ = false;
        lostErrorPending_ = false;
    }
    return e;
}

GLenum WebGL2RenderingContext::takeLostError() {
    const bool first = lostErrorPending_;
    lostErrorPending_ = false;
    return first ? 0x9242 /* CONTEXT_LOST_WEBGL */ : GL_NO_ERROR;
}

} // namespace bro::webgl
