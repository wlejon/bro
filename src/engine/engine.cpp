#include "engine/engine.h"

#include "platform/sdl_window.h"
#include "platform/vulkan_context.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/scene_layer.h"
#include "render/skia_backend.h"
#include "js/runtime.h"
#include "js/console.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/event_dispatch.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/event.h"
#include <litehtml/render_item.h>
#include "layout/container.h"
#include "util/log.h"
#include "util/time.h"

#include <SDL3/SDL.h>
#include <cstdio>
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

    // 2. Renderer (Skia raster + SDL display, no Vulkan needed)
    renderer_ = render::createRenderer(nullptr, window_.get());
    if (!renderer_) {
        throw std::runtime_error("Failed to create renderer");
    }

    // 4. JS runtime + bindings
    jsRuntime_ = std::make_unique<js::Runtime>();
    jsRuntime_->setModuleLoader();
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

    // 7. Parse HTML with litehtml (single parse, shared by layout + DOM)
    litehtmlDoc_ = litehtml::document::createFromString(html, container_.get(),
                                                         litehtml::master_css, userStyles);

    // 8. Build bro::dom tree from the same litehtml document
    document_ = std::make_unique<dom::Document>();
    document_->buildFrom(litehtmlDoc_);

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

    // 12. Stats overlay font
    statsFont_ = renderer_->createFont("Consolas", 13.0f, 400, false);
}

void Engine::setSceneLayer(std::unique_ptr<render::SceneLayer> layer) {
    sceneLayer_ = std::move(layer);
    if (sceneLayer_ && renderer_) {
        sceneLayer_->onInit(*renderer_, viewportWidth_, viewportHeight_);
    }
}

Engine::~Engine() {
    if (sceneLayer_) {
        sceneLayer_->onCleanup();
    }
    sceneLayer_.reset();
    if (statsFont_ && renderer_) {
        renderer_->deleteFont(statsFont_);
    }
    if (timers_ && jsRuntime_) {
        timers_->clearAll(jsRuntime_->getContext());
    }
    if (jsRuntime_) {
        js::DomBindings::cleanup(jsRuntime_->getContext());
    }
    // Destroy litehtml doc before container_ — litehtml::document::~document()
    // calls deleteFont which needs the FontManager inside container_ to be alive.
    litehtmlDoc_.reset();
    document_.reset();
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

    // Tell the layout container to skip full-viewport backgrounds when a scene
    // layer is active, so the scene shows through behind the HTML UI.
    container_->setSceneMode(sceneLayer_ != nullptr);

    // Initial layout
    if (litehtmlDoc_) {
        litehtmlDoc_->render(static_cast<litehtml::pixel_t>(viewportWidth_));
    }

    while (running_) {
        double frameStart = util::currentTimeMs();

        // 1. Poll platform events
        eventLoop_->pollEvents();
        if (eventLoop_->shouldQuit()) {
            running_ = false;
            break;
        }

        // 2. Tick timers
        double now = util::currentTimeMs();
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

        if (sceneLayer_) {
            // Scene layer fills the background
            sceneLayer_->onRender(*renderer_, viewportWidth_, viewportHeight_,
                                  totalFrameMs_);
        } else {
            renderer_->clear({255, 255, 255, 255});
        }

        // HTML/CSS UI composited on top (transparent backgrounds show scene)
        if (litehtmlDoc_) {
            litehtml::position clip(0, 0,
                                    static_cast<litehtml::pixel_t>(viewportWidth_),
                                    static_cast<litehtml::pixel_t>(viewportHeight_));
            litehtmlDoc_->draw(
                reinterpret_cast<litehtml::uint_ptr>(renderer_.get()), 0, 0, &clip);
        }

        double renderElapsed = util::currentTimeMs() - frameStart;
        drawStatsOverlay(renderElapsed);

        renderer_->endFrame();

        // 6. Frame time tracking
        totalFrameMs_ = util::currentTimeMs() - frameStart;
        double totalFrameMs = totalFrameMs_;
        statsAccumMs_ += totalFrameMs;
        statsFrameCount_++;
        if (totalFrameMs < statsMinFrameMs_) statsMinFrameMs_ = totalFrameMs;
        if (totalFrameMs > statsMaxFrameMs_) statsMaxFrameMs_ = totalFrameMs;
        if (statsAccumMs_ >= 500.0) {
            statsFps_ = statsFrameCount_ / (statsAccumMs_ / 1000.0);
            statsFrameTimeMs_ = statsAccumMs_ / statsFrameCount_;
            statsAccumMs_ = 0.0;
            statsFrameCount_ = 0;
            statsMinFrameMs_ = 999.0;
            statsMaxFrameMs_ = 0.0;
        }
    }
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void Engine::handleResize(int w, int h) {
    viewportWidth_ = w;
    viewportHeight_ = h;
    container_->setViewport(w, h);
    // Swapchain resize is handled by SkiaRenderer::beginFrame() when it detects
    // a size mismatch, so we don't call recreateSwapchain here.
    if (sceneLayer_) {
        sceneLayer_->onResize(w, h);
    }
    if (litehtmlDoc_) {
        litehtmlDoc_->render(static_cast<litehtml::pixel_t>(w));
    }
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void Engine::handleMouseDown(float x, float y, int button) {
    if (litehtmlDoc_) {
        litehtml::position::vector redraw;
        litehtmlDoc_->on_lbutton_down(static_cast<int>(x), static_cast<int>(y),
                                       static_cast<int>(x), static_cast<int>(y), redraw);
    }

    if (document_) {
        dom::MouseEvent evt("mousedown");
        evt.setClientX(static_cast<double>(x));
        evt.setClientY(static_cast<double>(y));
        evt.setButton(button);
        dom::Element* target = hitTest(x, y);
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
        dom::MouseEvent clickEvt("click");
        clickEvt.setClientX(static_cast<double>(x));
        clickEvt.setClientY(static_cast<double>(y));
        clickEvt.setButton(button);
        dom::Element* target = hitTest(x, y);
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
// Hit testing
// ---------------------------------------------------------------------------

dom::Element* Engine::hitTest(float x, float y) {
    if (!litehtmlDoc_ || !document_) return document_ ? document_->body() : nullptr;

    auto rootRender = litehtmlDoc_->root_render();
    if (!rootRender) return document_->body();

    auto lhElem = rootRender->get_element_by_point(
        static_cast<int>(x), static_cast<int>(y),
        static_cast<int>(x), static_cast<int>(y),
        [](const std::shared_ptr<litehtml::render_item>&) { return true; });

    if (!lhElem) return document_->body();

    dom::Element* found = document_->findElementByLitehtml(lhElem);
    if (!found) {
        // The hit element might be a text node or anonymous element.
        // Walk up litehtml's parent chain to find a mapped element.
        auto parent = lhElem->parent();
        while (parent && !found) {
            found = document_->findElementByLitehtml(parent);
            if (!found) parent = parent->parent();
        }
    }

    return found ? found : document_->body();
}

// ---------------------------------------------------------------------------
// Event dispatch to JS (delegates to shared implementation)
// ---------------------------------------------------------------------------

void Engine::dispatchEvent(dom::Element* target, dom::Event& event) {
    if (!target || !jsRuntime_) return;
    js::dispatchDomEvent(jsRuntime_->getContext(), target, event);
}

// ---------------------------------------------------------------------------
// Stats overlay
// ---------------------------------------------------------------------------

void Engine::drawStatsOverlay(double frameTimeMs) {
    if (!statsFont_) return;

    using render::Color;
    constexpr float pad = 6.0f;
    constexpr float lineH = 16.0f;
    constexpr int numLines = 5;
    const float boxW = 220.0f;
    const float boxH = pad * 2 + lineH * numLines;
    const float boxX = static_cast<float>(viewportWidth_) - boxW - 8.0f;
    const float boxY = 8.0f;

    // Semi-transparent background
    renderer_->fillRect(boxX, boxY, boxW, boxH, {0, 0, 0, 190});
    renderer_->drawRect(boxX, boxY, boxW, boxH, {80, 80, 80, 200});

    float y = boxY + pad;
    float x = boxX + pad;
    Color white{220, 220, 220, 255};
    Color label{140, 140, 140, 255};
    Color green{100, 220, 100, 255};
    Color yellow{220, 200, 80, 255};
    Color red{220, 80, 80, 255};

    // FPS color: green >= 55, yellow >= 30, red < 30
    Color fpsColor = statsFps_ >= 55.0 ? green : (statsFps_ >= 30.0 ? yellow : red);

    char buf[64];

    // Line 1: FPS
    std::snprintf(buf, sizeof(buf), "FPS: %.0f", statsFps_);
    renderer_->drawText(buf, x, y, statsFont_, fpsColor);
    y += lineH;

    // Line 2: Frame time (avg)
    std::snprintf(buf, sizeof(buf), "Frame: %.1f ms", statsFrameTimeMs_);
    renderer_->drawText(buf, x, y, statsFont_, white);
    y += lineH;

    // Line 3: Current frame (render only, no sleep)
    std::snprintf(buf, sizeof(buf), "Render: %.1f ms", frameTimeMs);
    renderer_->drawText(buf, x, y, statsFont_, white);
    y += lineH;

    // Line 4: Min/Max
    double dispMin = statsMinFrameMs_ < 999.0 ? statsMinFrameMs_ : 0.0;
    std::snprintf(buf, sizeof(buf), "Min/Max: %.1f / %.1f ms", dispMin, statsMaxFrameMs_);
    renderer_->drawText(buf, x, y, statsFont_, label);
    y += lineH;

    // Line 5: Viewport
    std::snprintf(buf, sizeof(buf), "Viewport: %d x %d", viewportWidth_, viewportHeight_);
    renderer_->drawText(buf, x, y, statsFont_, label);
}

} // namespace bro::engine
