#pragma once

struct SDL_Renderer;

namespace bro::render {

class Renderer;

/// Abstract interface for rendering a 3D scene behind the HTML/CSS UI.
///
/// The scene layer draws first each frame. The HTML/CSS UI is composited
/// on top with alpha blending, so CSS `background: transparent` lets the
/// scene show through.
///
/// For GPU-accelerated 3D, use SDL_RenderGeometry() to submit triangles
/// through the hardware-accelerated SDL renderer pipeline.
class SceneLayer {
public:
    virtual ~SceneLayer() = default;

    /// Called once after the renderer is ready.
    virtual void onInit(Renderer& renderer, int width, int height) = 0;

    /// Called when the window is resized.
    virtual void onResize(int width, int height) = 0;

    /// Called every frame before the UI draw.
    /// Use the Renderer to draw, or SDL_Renderer directly for advanced ops.
    virtual void onRender(Renderer& renderer, int width, int height,
                          double deltaTimeMs) = 0;

    /// Called before destruction.
    virtual void onCleanup() = 0;
};

} // namespace bro::render
