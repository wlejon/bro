#include "webgl/webgl_scene.h"
#include "render/gl_context.h"

namespace bro::webgl {

WebGLScene::WebGLScene(WebGL2RenderingContext* ctx)
    : ctx_(ctx) {}

WebGLScene::~WebGLScene() {
    onCleanup();
}

void WebGLScene::onInit(render::GLContext* gl, int width, int height) {
    gl_ = gl;
    if (ctx_) {
        ctx_->resize(width, height);
    }
}

void WebGLScene::onResize(int width, int height) {
    if (ctx_) {
        ctx_->resize(width, height);
    }
}

void WebGLScene::onRender(render::GLContext* /*gl*/, int /*width*/, int /*height*/, double /*deltaTimeMs*/) {
    // With Vulkan as the sole graphics backend, presentation is handled
    // through VulkanPresenter rather than legacy GL fullscreen quads.
}

void WebGLScene::onCleanup() {
    quadVBO_ = 0;
    quadVAO_ = 0;
}

} // namespace bro::webgl
