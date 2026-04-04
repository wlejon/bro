#pragma once

#include "canvas/canvas2d.h"
#include "render/scene_layer.h"
#include "render/renderer.h"
#include "render/gl_context.h"

#include <unordered_map>
#include <string>
#include <vector>

#include <glad/gl.h>

namespace bro::canvas {

class CanvasScene final : public render::SceneLayer {
public:
    explicit CanvasScene(render::Renderer* renderer) : renderer_(renderer) {}

    /// Callback invoked each frame before rendering to update offset and size.
    using LayoutCallback = void(*)(void* userdata, float& outX, float& outY, float& outW, float& outH);
    void setLayoutCallback(LayoutCallback cb, void* ud) { layoutCb_ = cb; layoutUd_ = ud; }
    ~CanvasScene() override { onCleanup(); }

    void onInit(render::GLContext* gl, int w, int h) override {
        gl_ = gl;
        width_ = w;
        height_ = h;
    }
    void onResize(int w, int h) override { width_ = w; height_ = h; }
    void onCleanup() override;

    Canvas2D& canvas() { return canvas_; }
    render::Renderer* renderer() const { return renderer_; }
    int width() const { return width_; }
    int height() const { return height_; }

    /// Build per-frame vertex data and upload to GPU. Call before onRender.
    void prepareFrame(render::GLContext* gl, int w, int h);

    void onRender(render::GLContext* gl, int w, int h, double deltaTimeMs) override;

private:
    uint64_t getOrCreateFont(const std::string& fontStr);

    Canvas2D canvas_;
    render::Renderer* renderer_;
    render::GLContext* gl_ = nullptr;
    int width_ = 0, height_ = 0;
    float offsetX_ = 0, offsetY_ = 0;
    LayoutCallback layoutCb_ = nullptr;
    void* layoutUd_ = nullptr;

    std::unordered_map<std::string, uint64_t> fontCache_;

    // GL vertex buffer (grown as needed)
    GLuint vertexBuf_ = 0;
    uint32_t vertexBufSize_ = 0;

    // VAOs for this scene
    GLuint colorVAO_ = 0;
    GLuint textureVAO_ = 0;

    // Prepared frame data (filled by prepareFrame, drawn by onRender)
    uint32_t colorVertCount_ = 0;
    uint32_t colorBytes_ = 0;
    struct TextDraw {
        GLuint tex;
        uint32_t firstVertex;
        uint32_t vertexCount;
    };
    std::vector<TextDraw> textDraws_;

    // Text texture cache
    struct CachedText {
        GLuint texture;
        int w, h;
    };
    std::unordered_map<std::string, CachedText> textCache_;
};

} // namespace bro::canvas
