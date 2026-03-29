#pragma once

struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;

namespace bro::render {

class GPUContext;

/// Abstract interface for rendering a scene behind the HTML/CSS UI.
///
/// The scene layer draws into an SDL_GPU render pass targeting the
/// swapchain. The HTML/CSS UI is composited on top as a transparent
/// texture, so CSS `background: transparent` lets the scene show through.
class SceneLayer {
public:
    virtual ~SceneLayer() = default;

    /// Called once after the GPU context is ready.
    virtual void onInit(GPUContext* gpu, int width, int height) = 0;

    /// Called when the window is resized.
    virtual void onResize(int width, int height) = 0;

    /// Called every frame. Draw into the render pass.
    virtual void onRender(GPUContext* gpu, SDL_GPUCommandBuffer* cmd,
                          SDL_GPURenderPass* pass,
                          int width, int height, double deltaTimeMs) = 0;

    /// Called before destruction.
    virtual void onCleanup() = 0;
};

} // namespace bro::render
