#pragma once

namespace bro::render {

/// Abstract interface for rendering a scene behind the HTML/CSS UI.
class SceneLayer {
public:
    virtual ~SceneLayer() = default;

    /// Called once after the graphics context is ready.
    virtual void onInit(int width, int height) = 0;

    /// Called when the window is resized.
    virtual void onResize(int width, int height) = 0;

    /// Called every frame.
    virtual void onRender(int width, int height, double deltaTimeMs) = 0;

    /// Called before destruction.
    virtual void onCleanup() = 0;
};

} // namespace bro::render
