#pragma once

#include "canvas/canvas2d.h"
#include "render/scene_layer.h"
#include "render/renderer.h"

#include <SDL3/SDL.h>
#include <unordered_map>
#include <string>
#include <cmath>

namespace bro::canvas {

class CanvasScene final : public render::SceneLayer {
public:
    explicit CanvasScene(render::Renderer* renderer) : renderer_(renderer) {}
    ~CanvasScene() override { onCleanup(); }

    void onInit(SDL_Renderer*, int w, int h) override { width_ = w; height_ = h; }
    void onResize(int w, int h) override { width_ = w; height_ = h; }
    void onCleanup() override {
        for (auto& [k, tex] : textCache_) SDL_DestroyTexture(tex.texture);
        textCache_.clear();
        for (auto& [k, h] : fontCache_) renderer_->deleteFont(h);
        fontCache_.clear();
    }

    Canvas2D& canvas() { return canvas_; }
    render::Renderer* renderer() const { return renderer_; }
    int width() const { return width_; }
    int height() const { return height_; }

    void onRender(SDL_Renderer* sdl, int w, int h, double) override;

private:
    uint64_t getOrCreateFont(const std::string& fontStr);

    struct CachedText {
        SDL_Texture* texture;
        int w, h;
    };

    Canvas2D canvas_;
    render::Renderer* renderer_;
    int width_ = 0, height_ = 0;

    std::unordered_map<std::string, uint64_t> fontCache_;
    std::unordered_map<std::string, CachedText> textCache_;
};

} // namespace bro::canvas
