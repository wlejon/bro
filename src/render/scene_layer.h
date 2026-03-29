#pragma once

struct SDL_Renderer;

namespace bro::render {

/// Abstract interface for rendering a scene behind the HTML/CSS UI.
///
/// The scene layer draws directly to SDL_Renderer (GPU-accelerated via
/// D3D11/D3D12/Metal). The HTML/CSS UI is composited on top as a
/// transparent texture, so CSS `background: transparent` lets the scene
/// show through.
///
/// Use SDL_RenderFillRect, SDL_RenderLine, SDL_RenderGeometry, etc.
/// for GPU-accelerated drawing.
class SceneLayer {
public:
    virtual ~SceneLayer() = default;

    /// Called once after the SDL renderer is ready.
    virtual void onInit(SDL_Renderer* sdlRenderer, int width, int height) = 0;

    /// Called when the window is resized.
    virtual void onResize(int width, int height) = 0;

    /// Called every frame before the UI overlay.
    /// Draw directly to sdlRenderer for GPU-accelerated rendering.
    virtual void onRender(SDL_Renderer* sdlRenderer, int width, int height,
                          double deltaTimeMs) = 0;

    /// Called before destruction.
    virtual void onCleanup() = 0;
};

} // namespace bro::render
