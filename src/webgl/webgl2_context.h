#pragma once

#include "webgl/webgl_types.h"

#include "render/layer_image.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace bro::render { class VulkanContext; }
namespace bro::webgl::vk { class WebGLVkContext; }

namespace bro::webgl {

/// The WebGL2 context of one <canvas>. It owns the backend that implements
/// the API and holds the drawing buffer (vk::WebGLVkContext), and carries
/// what outlives a backend: the drawing buffer's size and the context-loss
/// state (WEBGL_lose_context). The JS binding calls the backend directly;
/// while the context is lost there is none, and every call answers its lost
/// value.
class WebGL2RenderingContext {
public:
    WebGL2RenderingContext(int width, int height, render::VulkanContext& device);
    ~WebGL2RenderingContext();

    WebGL2RenderingContext(const WebGL2RenderingContext&) = delete;
    WebGL2RenderingContext& operator=(const WebGL2RenderingContext&) = delete;

    /// The backend WebGL calls run against: null while the context is lost.
    vk::WebGLVkContext* backend() const { return backend_.get(); }

    /// Run when the context is destroyed (the binding drops its wrappers).
    using TeardownCallback = std::function<void(WebGL2RenderingContext*)>;
    void addTeardownCallback(TeardownCallback cb) { teardownCallbacks_.push_back(std::move(cb)); }

    /// Resize the drawing buffer.
    void resize(int width, int height);
    int canvasWidth() const { return width_; }
    int canvasHeight() const { return height_; }

    // --- The drawing buffer, for the compositor and toDataURL ---
    /// The canvas image (its view, current layout and size), for the
    /// compositor to sample in place; empty while lost.
    render::LayerImage drawingBuffer() const;
    /// Submit the recorded work: the engine is about to sample the canvas.
    void flush();
    /// The canvas as tightly packed, top-down RGBA (canvasWidth() x
    /// canvasHeight()). False while lost.
    bool readCanvasPixels(std::vector<uint8_t>& out);

    // --- Context loss (WEBGL_lose_context) ---
    // Losing the context destroys the backend and every object with it; the
    // engine fires the canvas's webglcontextlost / webglcontextrestored.
    enum class ContextEvent : uint8_t { None, Lost, Restored };
    bool isContextLost() const { return lost_; }
    void loseContext();
    void restoreContext();
    /// The context event the engine owes the canvas, taken once. Taking
    /// Restored is what restores: a fresh backend, every object gone.
    ContextEvent takeContextEvent();
    /// The lost event was not cancelled: restoreContext may not restore.
    void forbidRestore() { restoreAllowed_ = false; }
    /// getError while lost: CONTEXT_LOST_WEBGL once, then NO_ERROR.
    GLenum takeLostError();

private:
    int width_;
    int height_;
    render::VulkanContext& device_;
    std::unique_ptr<vk::WebGLVkContext> backend_;
    std::vector<TeardownCallback> teardownCallbacks_;

    bool lost_ = false;
    bool restoreAllowed_ = false;
    bool lostErrorPending_ = false;
    ContextEvent pendingEvent_ = ContextEvent::None;
    GLuint nextObjectId_ = 1;       // the lost backend's, for the restored one
};

} // namespace bro::webgl
