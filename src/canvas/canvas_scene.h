#pragma once

#include "canvas/canvas2d.h"
#include "render/renderer.h"
#include "render/gl_context.h"

#include <unordered_map>
#include <string>
#include <vector>

#include <glad/gl.h>

namespace bro::canvas {

/// Per-canvas FBO-based renderer.  Each CanvasScene owns its own framebuffer
/// and rasterizes Canvas2D commands in canvas-local coordinates (0,0 = top-left
/// of the canvas element).  The engine composites the resulting texture at the
/// element's layout position — exactly like a browser.
class CanvasScene {
public:
    explicit CanvasScene(render::Renderer* renderer) : renderer_(renderer) {}
    ~CanvasScene() { cleanup(); }

    CanvasScene(const CanvasScene&) = delete;
    CanvasScene& operator=(const CanvasScene&) = delete;

    /// Callback invoked each frame to query the element's absolute position and size.
    using LayoutCallback = void(*)(void* userdata, float& outX, float& outY, float& outW, float& outH);
    void setLayoutCallback(LayoutCallback cb, void* ud) { layoutCb_ = cb; layoutUd_ = ud; }

    /// Callback to check if the backing element is still in the DOM.
    using DetachedCallback = bool(*)(void* userdata);
    void setDetachedCallback(DetachedCallback cb, void* ud) { detachedCb_ = cb; detachedUd_ = ud; }

    void init(render::GLContext* gl) { gl_ = gl; }
    void cleanup();

    Canvas2D& canvas() { return canvas_; }
    render::Renderer* renderer() const { return renderer_; }
    int width() const { return queryLayoutWidth(); }
    int height() const { return queryLayoutHeight(); }

    void setViewportScroll(float scrollY) { viewportScrollY_ = scrollY; }

    bool isDetached() const { return detached_; }

    /// Rasterize pending Canvas2D commands into this canvas's FBO.
    /// The FBO persists between frames (like a real canvas bitmap).
    void rasterize(render::GLContext* gl);

    /// Returns the FBO color texture (0 if nothing has been drawn yet).
    GLuint texture() const { return fboTexture_; }

    /// Get the screen-space rect where this canvas should be composited.
    void getScreenRect(float& x, float& y, float& w, float& h) const {
        x = screenX_;
        y = screenY_;
        w = static_cast<float>(fboWidth_);
        h = static_cast<float>(fboHeight_);
    }

private:
    int queryLayoutWidth() const {
        if (layoutCb_) {
            float ox, oy, ow = 0, oh = 0;
            layoutCb_(layoutUd_, ox, oy, ow, oh);
            if (ow > 0) return static_cast<int>(ow);
        }
        return fboWidth_ > 0 ? fboWidth_ : 300;  // HTML spec default
    }
    int queryLayoutHeight() const {
        if (layoutCb_) {
            float ox, oy, ow = 0, oh = 0;
            layoutCb_(layoutUd_, ox, oy, ow, oh);
            if (oh > 0) return static_cast<int>(oh);
        }
        return fboHeight_ > 0 ? fboHeight_ : 150;  // HTML spec default
    }

    uint64_t getOrCreateFont(const std::string& fontStr);
    void ensureFBO(int w, int h);

    Canvas2D canvas_;
    render::Renderer* renderer_;
    render::GLContext* gl_ = nullptr;
    LayoutCallback layoutCb_ = nullptr;
    void* layoutUd_ = nullptr;
    DetachedCallback detachedCb_ = nullptr;
    void* detachedUd_ = nullptr;
    float viewportScrollY_ = 0;
    bool detached_ = false;

    // Screen-space position for compositing (set each frame by rasterize)
    float screenX_ = 0, screenY_ = 0;

    // Per-canvas FBO
    GLuint fbo_ = 0;
    GLuint fboTexture_ = 0;
    int fboWidth_ = 0, fboHeight_ = 0;

    std::unordered_map<std::string, uint64_t> fontCache_;

    // GL vertex buffer (grown as needed)
    GLuint vertexBuf_ = 0;
    uint32_t vertexBufSize_ = 0;

    // VAOs for this scene
    GLuint colorVAO_ = 0;
    GLuint textureVAO_ = 0;

    // Per-frame draw data
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
