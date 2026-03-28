#include "engine/engine.h"

#include "platform/sdl_window.h"
#include "platform/vulkan_context.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/skia_backend.h"
#include "js/runtime.h"
#include "js/console.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/container.h"
#include "util/log.h"

#include <windows.h>
#include <stdexcept>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Engine::Engine(const std::string& appDir, int width, int height)
    : viewportWidth_(width)
    , viewportHeight_(height) {

    // 1. Window
    window_ = std::make_unique<platform::Window>("Bro", static_cast<uint32_t>(width),
                                                  static_cast<uint32_t>(height));

    // 2. Renderer
    // When Skia is available, init Vulkan and use SkiaRenderer.
    // Otherwise, use SDL's built-in 2D renderer (handles its own GPU context).
#ifndef BRO_NO_SKIA
    try {
        vulkan_ = std::make_unique<platform::VulkanContext>();
        vulkan_->init(*window_);
    } catch (const std::exception& e) {
        LOG_WARN("Vulkan init failed (%s) -- falling back to SDL renderer", e.what());
        vulkan_.reset();
    }
#endif
    renderer_ = render::createRenderer(vulkan_.get(), window_.get());
    if (!renderer_) {
        throw std::runtime_error("Failed to create renderer");
    }

    // 4. JS runtime + bindings
    jsRuntime_ = std::make_unique<js::Runtime>();
    js::Console::install(jsRuntime_->getContext());
    timers_ = std::make_unique<js::Timers>();
    js::Timers::install(jsRuntime_->getContext(), timers_.get());

    // 5. Layout container
    container_ = std::make_unique<layout::BroContainer>(renderer_.get(), viewportWidth_, viewportHeight_);

    // 6. Load the application
    manifest_ = AppLoader::loadApp(appDir);
    std::string html = AppLoader::loadFile(manifest_.htmlPath);
    if (html.empty()) {
        throw std::runtime_error("Failed to load index.html from " + appDir);
    }

    // Set the base URL so CSS @import / relative paths work.
    container_->set_base_url(manifest_.basePath.c_str());

    // Load user stylesheets into a single string that we prepend.
    std::string userStyles;
    for (auto& cssPath : manifest_.stylePaths) {
        std::string css = AppLoader::loadFile(cssPath);
        if (!css.empty()) {
            userStyles += css + "\n";
        }
    }

    // 7. Parse HTML with litehtml
    litehtmlDoc_ = litehtml::document::createFromString(html, container_.get(),
                                                         litehtml::master_css, userStyles);

    // 8. Build bro::dom tree
    document_ = std::make_unique<dom::Document>();
    document_->parse(html, container_.get());

    // 9. Install DOM JS bindings
    js::DomBindings::install(jsRuntime_->getContext(), document_.get());

    // 10. Load and execute scripts
    for (auto& scriptPath : manifest_.scriptPaths) {
        std::string code = AppLoader::loadFile(scriptPath);
        if (!code.empty()) {
            if (!jsRuntime_->eval(code, scriptPath)) {
                LOG_ERROR("Failed to execute script: %s", scriptPath.c_str());
            }
        }
    }

    // 11. Event loop
    eventLoop_ = std::make_unique<platform::EventLoop>();
}

Engine::~Engine() {
    if (timers_ && jsRuntime_) {
        timers_->clearAll(jsRuntime_->getContext());
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void Engine::run() {
    running_ = true;

    // Wire up event-loop callbacks.
    eventLoop_->onQuit = [this]() { running_ = false; };
    eventLoop_->onResize = [this](uint32_t w, uint32_t h) {
        handleResize(static_cast<int>(w), static_cast<int>(h));
    };
    eventLoop_->onMouseDown = [this](float x, float y, uint8_t btn) {
        handleMouseDown(x, y, static_cast<int>(btn));
    };
    eventLoop_->onMouseUp = [this](float x, float y, uint8_t btn) {
        handleMouseUp(x, y, static_cast<int>(btn));
    };
    eventLoop_->onMouseMove = [this](float x, float y) {
        handleMouseMove(x, y);
    };
    eventLoop_->onKeyDown = [this](int32_t keycode, int32_t scancode, uint16_t mod) {
        handleKeyDown(keycode, scancode, static_cast<int>(mod));
    };
    eventLoop_->onKeyUp = [this](int32_t keycode, int32_t scancode, uint16_t mod) {
        handleKeyUp(keycode, scancode, static_cast<int>(mod));
    };

    auto getTimeMs = []() -> double {
        LARGE_INTEGER freq, counter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart) * 1000.0
             / static_cast<double>(freq.QuadPart);
    };

    // Initial layout
    if (litehtmlDoc_) {
        litehtmlDoc_->render(static_cast<litehtml::pixel_t>(viewportWidth_));
    }

    while (running_) {
        // 1. Poll platform events
        eventLoop_->pollEvents();
        if (eventLoop_->shouldQuit()) {
            running_ = false;
            break;
        }

        // 2. Tick timers
        double now = getTimeMs();
        timers_->tick(now);

        // 3. Run pending JS jobs (promises, etc.)
        jsRuntime_->executePendingJobs();

        // 4. Re-layout if DOM is dirty
        if (document_ && document_->isDirty() && litehtmlDoc_) {
            litehtmlDoc_->render(static_cast<litehtml::pixel_t>(viewportWidth_));
            document_->clearDirty();
        }

        // 5. Render
        renderer_->beginFrame(viewportWidth_, viewportHeight_);
        renderer_->clear({255, 255, 255, 255});

        if (litehtmlDoc_) {
            litehtml::position clip(0, 0,
                                    static_cast<litehtml::pixel_t>(viewportWidth_),
                                    static_cast<litehtml::pixel_t>(viewportHeight_));
            litehtmlDoc_->draw(
                reinterpret_cast<litehtml::uint_ptr>(renderer_.get()), 0, 0, &clip);
        }

        renderer_->endFrame();
    }
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void Engine::handleResize(int w, int h) {
    viewportWidth_ = w;
    viewportHeight_ = h;
    window_->setSize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    container_->setViewport(w, h);
    if (litehtmlDoc_) {
        litehtmlDoc_->render(static_cast<litehtml::pixel_t>(w));
    }
#ifndef BRO_NO_SKIA
    if (vulkan_) {
        vulkan_->recreateSwapchain(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    }
#endif
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void Engine::handleMouseDown(float x, float y, int button) {
    // Forward to litehtml for hit testing.
    if (litehtmlDoc_) {
        litehtml::position::vector redraw;
        litehtmlDoc_->on_lbutton_down(static_cast<int>(x), static_cast<int>(y),
                                       static_cast<int>(x), static_cast<int>(y), redraw);
    }

    // Dispatch to JS listeners on the bro::dom side.
    if (document_) {
        dom::MouseEvent evt("mousedown");
        evt.setClientX(static_cast<double>(x));
        evt.setClientY(static_cast<double>(y));
        evt.setButton(button);
        // Simple hit test: find the deepest element at (x,y).
        // For now, dispatch to body.
        dom::Element* target = document_->body();
        if (target) {
            dispatchEvent(target, evt);
        }
    }
}

void Engine::handleMouseUp(float x, float y, int button) {
    if (litehtmlDoc_) {
        litehtml::position::vector redraw;
        litehtmlDoc_->on_lbutton_up(static_cast<int>(x), static_cast<int>(y),
                                     static_cast<int>(x), static_cast<int>(y), redraw);
    }

    if (document_) {
        // Dispatch click event.
        dom::MouseEvent clickEvt("click");
        clickEvt.setClientX(static_cast<double>(x));
        clickEvt.setClientY(static_cast<double>(y));
        clickEvt.setButton(button);
        dom::Element* target = document_->body();
        if (target) {
            dispatchEvent(target, clickEvt);
        }
    }
}

void Engine::handleMouseMove(float x, float y) {
    if (litehtmlDoc_) {
        litehtml::position::vector redraw;
        litehtmlDoc_->on_mouse_over(static_cast<int>(x), static_cast<int>(y),
                                     static_cast<int>(x), static_cast<int>(y), redraw);
    }
}

// ---------------------------------------------------------------------------
// Keyboard events
// ---------------------------------------------------------------------------

void Engine::handleKeyDown(int keycode, int /*scancode*/, int mod) {
    if (document_) {
        dom::KeyboardEvent evt("keydown");
        evt.setKey(std::to_string(keycode));
        evt.setCode(std::to_string(keycode));
        evt.setCtrlKey((mod & 0x0040) != 0);  // KMOD_CTRL
        evt.setShiftKey((mod & 0x0001) != 0);  // KMOD_SHIFT
        evt.setAltKey((mod & 0x0100) != 0);    // KMOD_ALT
        dom::Element* target = document_->body();
        if (target) {
            dispatchEvent(target, evt);
        }
    }
}

void Engine::handleKeyUp(int keycode, int /*scancode*/, int mod) {
    if (document_) {
        dom::KeyboardEvent evt("keyup");
        evt.setKey(std::to_string(keycode));
        evt.setCode(std::to_string(keycode));
        evt.setCtrlKey((mod & 0x0040) != 0);
        evt.setShiftKey((mod & 0x0001) != 0);
        evt.setAltKey((mod & 0x0100) != 0);
        dom::Element* target = document_->body();
        if (target) {
            dispatchEvent(target, evt);
        }
    }
}

// ---------------------------------------------------------------------------
// Event dispatch to JS
// ---------------------------------------------------------------------------

void Engine::dispatchEvent(dom::Element* target, dom::Event& event) {
    if (!target || !jsRuntime_) return;

    event.setTarget(target);
    event.setCurrentTarget(target);

    auto& listeners = target->listeners();
    auto it = listeners.find(event.type());
    if (it == listeners.end()) return;

    JSContext* ctx = jsRuntime_->getContext();
    JSValue global = jsRuntime_->getGlobalObject();

    for (uint64_t listenerId : it->second) {
        // Listener IDs are indices into a global listener registry managed by
        // DomBindings. The JS callback is stored as a property on the global
        // object under the key "__bro_listener_<id>".
        std::string key = "__bro_listener_" + std::to_string(listenerId);
        JSValue fn = JS_GetPropertyStr(ctx, global, key.c_str());
        if (JS_IsFunction(ctx, fn)) {
            // Create a minimal event object to pass to JS.
            JSValue jsEvent = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, jsEvent, "type",
                              JS_NewString(ctx, event.type().c_str()));
            JS_SetPropertyStr(ctx, jsEvent, "timeStamp",
                              JS_NewFloat64(ctx, event.timeStamp()));

            JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, &jsEvent);
            if (JS_IsException(result)) {
                js::Runtime::checkException(ctx, result);
            }
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, jsEvent);
        }
        JS_FreeValue(ctx, fn);

        if (event.propagationStopped()) break;
    }

    JS_FreeValue(ctx, global);
}

} // namespace bro::engine
