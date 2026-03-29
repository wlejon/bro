#pragma once

namespace bro::render {

class GLContext;

/// Abstract interface for rendering a scene behind the HTML/CSS UI.
///
/// The scene layer draws using OpenGL into the default framebuffer (or its
/// own FBO). The HTML/CSS UI is composited on top as a transparent texture,
/// so CSS `background: transparent` lets the scene show through.
class SceneLayer {
public:
    virtual ~SceneLayer() = default;

    /// Called once after the GL context is ready.
    virtual void onInit(GLContext* gl, int width, int height) = 0;

    /// Called when the window is resized.
    virtual void onResize(int width, int height) = 0;

    /// Called every frame. Draw using OpenGL.
    virtual void onRender(GLContext* gl, int width, int height, double deltaTimeMs) = 0;

    /// Called before destruction.
    virtual void onCleanup() = 0;
};

} // namespace bro::render
