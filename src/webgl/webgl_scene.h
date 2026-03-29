#pragma once

#include "render/scene_layer.h"
#include "webgl/webgl2_context.h"

#include <glad/gl.h>

namespace bro::webgl {

/// SceneLayer that composites the WebGL2 canvas FBO into the window.
///
/// The WebGL2RenderingContext renders into its own FBO. This scene layer
/// draws that FBO's color texture as a fullscreen quad so it appears
/// behind the HTML/CSS UI overlay.
class WebGLScene final : public render::SceneLayer {
public:
    explicit WebGLScene(WebGL2RenderingContext* ctx);
    ~WebGLScene() override;

    void onInit(render::GLContext* gl, int width, int height) override;
    void onResize(int width, int height) override;
    void onRender(render::GLContext* gl, int width, int height, double deltaTimeMs) override;
    void onCleanup() override;

    WebGL2RenderingContext* webglContext() const { return ctx_; }

private:
    WebGL2RenderingContext* ctx_;
    render::GLContext* gl_ = nullptr;
    GLuint quadVAO_ = 0;
    GLuint quadVBO_ = 0;
};

} // namespace bro::webgl
