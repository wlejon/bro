#pragma once

#include "canvas/canvas2d.h"
#include "render/scene_layer.h"
#include "render/renderer.h"
#include "render/gpu_context.h"

#include <unordered_map>
#include <string>
#include <vector>

struct SDL_GPUTexture;
struct SDL_GPUBuffer;

namespace bro::canvas {

class CanvasScene final : public render::SceneLayer {
public:
    explicit CanvasScene(render::Renderer* renderer) : renderer_(renderer) {}
    ~CanvasScene() override { onCleanup(); }

    void onInit(render::GPUContext* gpu, int w, int h) override {
        gpu_ = gpu;
        width_ = w;
        height_ = h;
    }
    void onResize(int w, int h) override { width_ = w; height_ = h; }
    void onCleanup() override;

    Canvas2D& canvas() { return canvas_; }
    render::Renderer* renderer() const { return renderer_; }
    int width() const { return width_; }
    int height() const { return height_; }

    /// Upload per-frame vertex data to GPU. Call before the render pass.
    void prepareFrame(render::GPUContext* gpu, SDL_GPUCommandBuffer* cmd, int w, int h);

    void onRender(render::GPUContext* gpu, SDL_GPUCommandBuffer* cmd,
                  SDL_GPURenderPass* pass,
                  int w, int h, double deltaTimeMs) override;

private:
    uint64_t getOrCreateFont(const std::string& fontStr);

    Canvas2D canvas_;
    render::Renderer* renderer_;
    render::GPUContext* gpu_ = nullptr;
    int width_ = 0, height_ = 0;

    std::unordered_map<std::string, uint64_t> fontCache_;

    // GPU vertex buffer (grown as needed)
    SDL_GPUBuffer* vertexBuf_ = nullptr;
    uint32_t vertexBufSize_ = 0;

    // Prepared frame data (filled by prepareFrame, drawn by onRender)
    uint32_t colorVertCount_ = 0;
    uint32_t colorBytes_ = 0;
    struct TextDraw {
        SDL_GPUTexture* tex;
        uint32_t firstVertex;
        uint32_t vertexCount;
    };
    std::vector<TextDraw> textDraws_;

    // Text texture cache
    struct CachedText {
        SDL_GPUTexture* texture;
        int w, h;
    };
    std::unordered_map<std::string, CachedText> textCache_;
};

} // namespace bro::canvas
