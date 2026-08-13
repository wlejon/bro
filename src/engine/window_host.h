#pragma once

#include "engine/engine_config.h"
#include "engine/replaced_elements.h"
#include "dom/node_handle.h"
#include "js/message_queue.h"
#include "render/command_buffer.h"
#include "render/skia_backend.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct JSContext;

namespace bro::platform { class Window; }
namespace bro::dom { class Document; class Element; }
namespace bro::canvas { class CanvasScene; }
namespace bro::js { class Timers; }

namespace bro::engine {

/// Options for bro.window.open (geometry/flags feed the secondary
/// window; `display` is a display INDEX like bro.json's, -1 = OS default).
struct WindowHostOptions {
    std::string src;
    std::string title = "bro";
    int width = 800;
    int height = 600;
    int x = kWindowPosUnset;
    int y = kWindowPosUnset;
    int display = -1;
    bool resizable = true;
    bool borderless = false;
    bool alwaysOnTop = false;
    bool hidden = false;   // forced true in headless (deterministic tests)
    int minWidth = 0, minHeight = 0;   // 0 = unconstrained
    int maxWidth = 0, maxHeight = 0;

    /// Which of the above the CALLER passed explicitly.
    struct Provided {
        bool width = false, height = false, title = false;
        bool resizable = false, borderless = false, alwaysOnTop = false;
        bool minWidth = false, minHeight = false;
        bool maxWidth = false, maxHeight = false;
    } provided;
};

/// One secondary window host: a real OS window plus the isolated document
/// realm rendered into it.
struct WindowHost {
    uint64_t id = 0;        // bro-side handle id (stable across lifetime)
    uint32_t sdlId = 0;     // SDL windowID for event routing (0 until created)
    std::unique_ptr<platform::Window> window;
    WindowHostOptions opts; // requested state; geometry queried live once created
    bool pendingCreate = true;
    bool pendingClose = false;
    bool focused = false;
    bool minimized = false;
    bool occluded = false;
    int width = 0, height = 0;  // client size in window coords
    float clearColor[4] = {0.07f, 0.07f, 0.09f, 1.0f};

    // Document realm
    double displayScale = 1.0;
    bool loadFired = false;
    JSContext* jsCtx = nullptr;
    std::unique_ptr<js::Timers> timers;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes;
    std::unique_ptr<dom::Document> document;

    // Per-window input state
    MouseDispatchState mouseState;
    dom::Element* hoveredElement = nullptr;
    dom::Element* activeElement = nullptr;
    float lastMouseX = 0.0f, lastMouseY = 0.0f;
    int pressedButtons = 0;
    dom::ElementHandle controlDragElement;
    std::string resolvedCursor = "default";
    int boxW = 0, boxH = 0;

    // Render target
    render::CommandBuffer cmdBuffer;
    render::SkiaRenderer::GPUSurface surface;
    int surfW = 0, surfH = 0;
    unsigned int fboTexture = 0;

    std::vector<std::unique_ptr<js::Message>> inbox;
};

} // namespace bro::engine
