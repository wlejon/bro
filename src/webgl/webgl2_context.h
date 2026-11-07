#pragma once

#include "webgl/client/webgl_client.h"
#include "webgl/webgl_types.h"

#include "render/layer_image.h"
#include "render/vulkan_frames.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace bro::render { class VulkanContext; }
namespace bro::webgl::vk { class WebGLVkContext; struct WebGLCaps; }

namespace bro::webgl {

/// The WebGL2 context of one <canvas>. It owns the client (WebGLClient) the
/// JS binding calls, which encodes the calls for the backend that implements
/// the API and holds the drawing buffer (vk::WebGLVkContext), and carries
/// what outlives a backend: the drawing buffer's size, the context-loss
/// state (WEBGL_lose_context), the object names (one sequence, continued
/// across a loss), the device's capabilities snapshot (vk::WebGLCaps) and the
/// frame-end hook that submits the backend's open work. While the context is
/// lost there is no backend, and every call answers its lost value.
class WebGL2RenderingContext {
public:
    /// `requested` is what getContext was asked for; attributes() is what
    /// the context grants (ContextAttributes). The backend lives on `thread`
    /// (the engine's, shared by its contexts; null: one of its own).
    WebGL2RenderingContext(int width, int height, render::VulkanContext& device,
                           const ContextAttributes& requested = {},
                           std::shared_ptr<WebGLThread> thread = nullptr);
    ~WebGL2RenderingContext();

    /// What getContextAttributes answers.
    const ContextAttributes& attributes() const { return attributes_; }

    WebGL2RenderingContext(const WebGL2RenderingContext&) = delete;
    WebGL2RenderingContext& operator=(const WebGL2RenderingContext&) = delete;

    /// What the binding calls: null while the context is lost.
    WebGLClient* client() { return lost_ ? nullptr : &client_; }

    /// What the device can do: getParameter's constants, the extensions,
    /// precisions and sample counts. The same while lost.
    const vk::WebGLCaps& caps() const { return *caps_; }

    /// The name of an object about to be created (the backend's create*
    /// takes it). One sequence for every kind, continued across a context
    /// loss, so no name of a lost object ever names a new one.
    GLuint newObjectId() { return nextObjectId_++; }

    /// Run when the context is destroyed (the binding drops its wrappers).
    using TeardownCallback = std::function<void(WebGL2RenderingContext*)>;
    void addTeardownCallback(TeardownCallback cb) { teardownCallbacks_.push_back(std::move(cb)); }

    /// Resize the drawing buffer.
    void resize(int width, int height);
    int canvasWidth() const { return width_; }
    int canvasHeight() const { return height_; }

    // --- The drawing buffer, for the compositor and toDataURL ---
    /// The compositor is about to show the drawing buffer: submit the work
    /// recorded so far, mark it shown (vk::WebGLVkContext::presented), and
    /// answer the canvas image (its view, current layout and size) to sample
    /// in place, behind that submission in queue order. Empty while lost.
    render::LayerImage frameEnd();
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
    ContextAttributes attributes_;
    std::shared_ptr<const vk::WebGLCaps> caps_;
    WebGLClient client_;
    render::VulkanFrames::HookId frameEndHook_ = 0;
    std::vector<TeardownCallback> teardownCallbacks_;

    bool lost_ = false;
    bool restoreAllowed_ = false;
    bool lostErrorPending_ = false;
    ContextEvent pendingEvent_ = ContextEvent::None;
    GLuint nextObjectId_ = 1;
};

} // namespace bro::webgl
