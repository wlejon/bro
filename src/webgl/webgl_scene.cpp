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
    // Create a quad VAO/VBO for compositing
    glGenVertexArrays(1, &quadVAO_);
    glGenBuffers(1, &quadVBO_);

    if (ctx_) {
        ctx_->resize(width, height);
    }
}

void WebGLScene::onResize(int width, int height) {
    if (ctx_) {
        ctx_->resize(width, height);
    }
}

void WebGLScene::onRender(render::GLContext* gl, int width, int height, double /*deltaTimeMs*/) {
    if (!ctx_ || !gl) return;

    // The WebGL context has already rendered to its FBO (driven by JS
    // requestAnimationFrame). We just need to draw the FBO's color
    // texture as a fullscreen quad.

    GLuint tex = ctx_->colorTexture();
    if (!tex) return;

    float w = (float)width, h = (float)height;
    render::TextureVertex quad[6] = {
        {0, 0, 0, 1}, {w, 0, 1, 1}, {w, h, 1, 0},
        {0, 0, 0, 1}, {w, h, 1, 0}, {0, h, 0, 0},
    };

    glBindBuffer(GL_ARRAY_BUFFER, quadVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);

    glBindVertexArray(quadVAO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex),
                          (void*)offsetof(render::TextureVertex, u));

    glUseProgram(gl->textureProgram());
    float viewport[2] = {w, h};
    glUniform2fv(gl->textureViewportLoc(), 1, viewport);
    glUniform1i(gl->textureSamplerLoc(), 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    // No blending — the WebGL scene is fully opaque in the scene pass
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void WebGLScene::onCleanup() {
    if (quadVBO_) { glDeleteBuffers(1, &quadVBO_); quadVBO_ = 0; }
    if (quadVAO_) { glDeleteVertexArrays(1, &quadVAO_); quadVAO_ = 0; }
}

} // namespace bro::webgl
