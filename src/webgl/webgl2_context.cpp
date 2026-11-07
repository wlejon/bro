#include "webgl/webgl2_context.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_caps.h"
#include "render/vulkan_context.h"
#include "webgl/client/webgl_thread.h"

namespace bro::webgl {

namespace {

// What a context can grant of what it was asked: everything but a
// multisampled or unpremultiplied drawing buffer, desynchronized
// presentation and XR.
ContextAttributes granted(ContextAttributes a) {
    a.antialias = false;
    a.premultipliedAlpha = true;
    a.desynchronized = false;
    a.xrCompatible = false;
    return a;
}

}  // namespace

WebGL2RenderingContext::WebGL2RenderingContext(int width, int height, render::VulkanContext& device,
                                               const ContextAttributes& requested,
                                               std::shared_ptr<WebGLThread> thread)
    : width_(width), height_(height), device_(device), attributes_(granted(requested)),
      caps_(vk::buildWebGLCaps(device)),
      client_(thread ? std::move(thread) : std::make_shared<WebGLThread>(), device, caps_, attributes_) {
    client_.attach(width, height);
    // What a frame encoded is replayed and submitted by its end at the
    // latest (a canvas not composited included), so nothing the backend
    // holds outlives the frame it was for and a headless step sees it done.
    frameEndHook_ = device_.frames().addFrameEndHook([this] { client_.settle(); });
}

WebGL2RenderingContext::~WebGL2RenderingContext() {
    if (frameEndHook_ != 0) device_.frames().removeFrameEndHook(frameEndHook_);
    frameEndHook_ = 0;
    for (auto& cb : teardownCallbacks_)
        if (cb) cb(this);
    teardownCallbacks_.clear();
}

void WebGL2RenderingContext::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    // The presenter's submissions so far may sample the old images.
    if (!lost_) client_.resize(width, height, device_.queue().lastSubmittedTicket());
}

render::LayerImage WebGL2RenderingContext::frameEnd() {
    render::LayerImage out;
    if (lost_) return out;
    if (!client_.hasBackend()) return out;
    // Everything recorded is submitted, and the canvas image as that left it
    // answered: a round trip, so this submission precedes the presenter's.
    out = client_.flushThen([](vk::WebGLVkContext& gl) {
        const vk::WebGLVkCanvas& canvas = gl.canvas();
        render::LayerImage img;
        img.image = canvas.colorImage();
        img.view = canvas.presentView();
        img.layout = canvas.colorLayout();
        img.format = canvas.colorFormat();
        img.width = canvas.width();
        img.height = canvas.height();
        return img;
    });
    client_.presented();
    return out;
}

bool WebGL2RenderingContext::readCanvasPixels(std::vector<uint8_t>& out) {
    if (lost_) return false;
    if (!client_.hasBackend()) return false;
    return client_.sync([&out](vk::WebGLVkContext& gl) { return gl.readCanvasPixels(out); });
}

// WebGL 1.0 5.15.3 "lose the context": the backend and every object go now;
// the lost event is the engine's to fire, at the canvas, after this turn.
void WebGL2RenderingContext::loseContext() {
    if (lost_) return;  // its INVALID_OPERATION is unobservable: getError answers NO_ERROR while lost
    client_.detach();
    lost_ = true;
    lostErrorPending_ = true;
    restoreAllowed_ = true;
    pendingEvent_ = ContextEvent::Lost;
}

void WebGL2RenderingContext::restoreContext() {
    if (!lost_) {
        client_.syntheticError(GL_INVALID_OPERATION);
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
        client_.attach(width_, height_);
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
