#include "engine/engine.h"

#include "platform/sdl_window.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/scene_layer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "js/runtime.h"
#include "js/console.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/canvas_bindings.h"
#include "js/event_dispatch.h"
#include "js/audio_bindings.h"
#include "js/storage_bindings.h"
#include "js/webgl2_bindings.h"
#include "audio/audio_engine.h"
#include "canvas/canvas_scene.h"
#include "webgl/webgl2_context.h"
#include "webgl/webgl_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/event.h"
#include <litehtml/render_item.h>
#include "layout/container.h"
#include "util/log.h"
#include "util/time.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <glad/gl.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace bro::engine {

// ---------------------------------------------------------------------------
// SDL keycode → standard web KeyboardEvent.key mapping
// ---------------------------------------------------------------------------

static std::string sdlKeycodeToWebKey(int32_t keycode, int mod)
{
    // Special keys (with SDLK_SCANCODE_MASK = 0x40000000)
    switch (keycode) {
        case SDLK_RETURN:    return "Enter";
        case SDLK_ESCAPE:    return "Escape";
        case SDLK_BACKSPACE: return "Backspace";
        case SDLK_TAB:       return "Tab";
        case SDLK_SPACE:     return " ";
        case SDLK_DELETE:    return "Delete";
        case SDLK_INSERT:    return "Insert";
        case SDLK_HOME:      return "Home";
        case SDLK_END:       return "End";
        case SDLK_PAGEUP:    return "PageUp";
        case SDLK_PAGEDOWN:  return "PageDown";
        case SDLK_RIGHT:     return "ArrowRight";
        case SDLK_LEFT:      return "ArrowLeft";
        case SDLK_DOWN:      return "ArrowDown";
        case SDLK_UP:        return "ArrowUp";
        case SDLK_F1:  return "F1";  case SDLK_F2:  return "F2";
        case SDLK_F3:  return "F3";  case SDLK_F4:  return "F4";
        case SDLK_F5:  return "F5";  case SDLK_F6:  return "F6";
        case SDLK_F7:  return "F7";  case SDLK_F8:  return "F8";
        case SDLK_F9:  return "F9";  case SDLK_F10: return "F10";
        case SDLK_F11: return "F11"; case SDLK_F12: return "F12";
        case SDLK_LSHIFT: case SDLK_RSHIFT: return "Shift";
        case SDLK_LCTRL:  case SDLK_RCTRL:  return "Control";
        case SDLK_LALT:   case SDLK_RALT:   return "Alt";
        case SDLK_LGUI:   case SDLK_RGUI:   return "Meta";
        case SDLK_CAPSLOCK:   return "CapsLock";
        case SDLK_NUMLOCKCLEAR: return "NumLock";
        case SDLK_SCROLLLOCK: return "ScrollLock";
        case SDLK_PAUSE:     return "Pause";
        case SDLK_PRINTSCREEN: return "PrintScreen";
        case SDLK_MENU:      return "ContextMenu";
        default: break;
    }

    // Printable ASCII characters
    if (keycode >= 'a' && keycode <= 'z') {
        bool shift = (mod & SDL_KMOD_SHIFT) != 0;
        char c = shift ? (char)(keycode - 32) : (char)keycode;
        return std::string(1, c);
    }
    if (keycode >= '0' && keycode <= '9') {
        // Handle shift+digit for symbols
        if (mod & SDL_KMOD_SHIFT) {
            const char* symbols = ")!@#$%^&*(";
            return std::string(1, symbols[keycode - '0']);
        }
        return std::string(1, (char)keycode);
    }

    // Punctuation
    switch (keycode) {
        case SDLK_MINUS:         return (mod & SDL_KMOD_SHIFT) ? "_" : "-";
        case SDLK_EQUALS:        return (mod & SDL_KMOD_SHIFT) ? "+" : "=";
        case SDLK_LEFTBRACKET:   return (mod & SDL_KMOD_SHIFT) ? "{" : "[";
        case SDLK_RIGHTBRACKET:  return (mod & SDL_KMOD_SHIFT) ? "}" : "]";
        case SDLK_BACKSLASH:     return (mod & SDL_KMOD_SHIFT) ? "|" : "\\";
        case SDLK_SEMICOLON:     return (mod & SDL_KMOD_SHIFT) ? ":" : ";";
        case SDLK_APOSTROPHE:    return (mod & SDL_KMOD_SHIFT) ? "\"" : "'";
        case SDLK_GRAVE:         return (mod & SDL_KMOD_SHIFT) ? "~" : "`";
        case SDLK_COMMA:         return (mod & SDL_KMOD_SHIFT) ? "<" : ",";
        case SDLK_PERIOD:        return (mod & SDL_KMOD_SHIFT) ? ">" : ".";
        case SDLK_SLASH:         return (mod & SDL_KMOD_SHIFT) ? "?" : "/";
        default: break;
    }

    // Fallback: return the numeric keycode as string
    return std::to_string(keycode);
}

static std::string sdlScancodeToWebCode(int32_t scancode)
{
    // Letters (SDL_SCANCODE_A=4 through SDL_SCANCODE_Z=29)
    if (scancode >= 4 && scancode <= 29) {
        char c = 'A' + (char)(scancode - 4);
        return std::string("Key") + c;
    }
    // Digits (SDL_SCANCODE_1=30 through SDL_SCANCODE_0=39)
    if (scancode >= 30 && scancode <= 39) {
        char c = (scancode == 39) ? '0' : (char)('1' + (scancode - 30));
        return std::string("Digit") + c;
    }

    switch (scancode) {
        case 40: return "Enter";
        case 41: return "Escape";
        case 42: return "Backspace";
        case 43: return "Tab";
        case 44: return "Space";
        case 45: return "Minus";
        case 46: return "Equal";
        case 47: return "BracketLeft";
        case 48: return "BracketRight";
        case 49: return "Backslash";
        case 51: return "Semicolon";
        case 52: return "Quote";
        case 53: return "Backquote";
        case 54: return "Comma";
        case 55: return "Period";
        case 56: return "Slash";
        case 57: return "CapsLock";
        case 58: return "F1";  case 59: return "F2";
        case 60: return "F3";  case 61: return "F4";
        case 62: return "F5";  case 63: return "F6";
        case 64: return "F7";  case 65: return "F8";
        case 66: return "F9";  case 67: return "F10";
        case 68: return "F11"; case 69: return "F12";
        case 70: return "PrintScreen";
        case 71: return "ScrollLock";
        case 72: return "Pause";
        case 73: return "Insert";
        case 74: return "Home";
        case 75: return "PageUp";
        case 76: return "Delete";
        case 77: return "End";
        case 78: return "PageDown";
        case 79: return "ArrowRight";
        case 80: return "ArrowLeft";
        case 81: return "ArrowDown";
        case 82: return "ArrowUp";
        case 224: return "ShiftLeft";
        case 225: return "ShiftRight";
        case 226: return "ControlLeft";
        case 228: return "AltLeft";
        case 230: return "AltRight";
        default: break;
    }
    return "Unknown" + std::to_string(scancode);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Engine::Engine(const std::string& appDir, int width, int height)
    : viewportWidth_(width)
    , viewportHeight_(height) {

    // 1. Window (now creates OpenGL context)
    window_ = std::make_unique<platform::Window>("Bro", static_cast<uint32_t>(width),
                                                  static_cast<uint32_t>(height));

    // 2. GL context (shader programs + helpers)
    gl_ = std::make_unique<render::GLContext>(*window_);

    // 3. Renderer (Skia raster + OpenGL display)
    renderer_ = render::createRenderer(gl_.get());
    if (!renderer_) {
        throw std::runtime_error("Failed to create renderer");
    }

    // 4. JS runtime + bindings
    jsRuntime_ = std::make_unique<js::Runtime>();
    jsRuntime_->setModuleLoader();
    js::Console::install(jsRuntime_->getContext());
    timers_ = std::make_unique<js::Timers>();
    js::Timers::install(jsRuntime_->getContext(), timers_.get());

    // 4b. Audio engine + bindings
    audioEngine_ = std::make_unique<audio::AudioEngine>();
    audioEngine_->init();
    js::AudioBindings::install(jsRuntime_->getContext(), audioEngine_.get());

    // 4c. localStorage (persisted in app directory)
    std::string storagePath = manifest_.basePath + "/.storage.json";
    js::StorageBindings::install(jsRuntime_->getContext(), storagePath);

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

    // 9b. Install Canvas 2D and WebGL2 bindings + getContext factory
    js::CanvasBindings::install(jsRuntime_->getContext());
    js::WebGL2Bindings::install(jsRuntime_->getContext());
    js::DomBindings::setGetContextFactory(
        [this](JSContext* ctx, dom::Element*, const std::string& type) -> JSValue {
            if (type == "2d") {
                auto scene = std::make_unique<canvas::CanvasScene>(renderer_.get());
                auto* ptr = scene.get();
                setSceneLayer(std::move(scene));
                return js::CanvasBindings::wrapContext2D(ctx, ptr);
            }
            if (type == "webgl2" || type == "webgl") {
                auto* webglCtx = new webgl::WebGL2RenderingContext(
                    viewportWidth_, viewportHeight_);
                auto scene = std::make_unique<webgl::WebGLScene>(webglCtx);
                setSceneLayer(std::move(scene));
                return js::WebGL2Bindings::wrapContext(ctx, webglCtx);
            }
            return JS_NULL;
        });

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

    // 13. Create UI overlay quad VAO/VBO
    glGenVertexArrays(1, &uiQuadVAO_);
    glGenBuffers(1, &uiQuadVBO_);
}

void Engine::setSceneLayer(std::unique_ptr<render::SceneLayer> layer) {
    sceneLayer_ = std::move(layer);
    if (sceneLayer_) {
        sceneLayer_->onInit(gl_.get(), viewportWidth_, viewportHeight_);
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
        js::AudioBindings::cleanup(jsRuntime_->getContext());
        js::StorageBindings::cleanup(jsRuntime_->getContext());
        js::WebGL2Bindings::cleanup(jsRuntime_->getContext());
        js::DomBindings::cleanup(jsRuntime_->getContext());
    }
    if (uiQuadVBO_) { glDeleteBuffers(1, &uiQuadVBO_); uiQuadVBO_ = 0; }
    if (uiQuadVAO_) { glDeleteVertexArrays(1, &uiQuadVAO_); uiQuadVAO_ = 0; }
    audioEngine_.reset();
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
    eventLoop_->onKeyDown = [this](int32_t keycode, int32_t scancode, uint16_t mod, bool repeat) {
        handleKeyDown(keycode, scancode, static_cast<int>(mod), repeat);
    };
    eventLoop_->onKeyUp = [this](int32_t keycode, int32_t scancode, uint16_t mod, bool repeat) {
        handleKeyUp(keycode, scancode, static_cast<int>(mod), repeat);
    };

    // Tell the layout container to skip full-viewport backgrounds when a scene
    // layer is active, so the scene shows through behind the HTML UI.
    container_->setSceneMode(sceneLayer_ != nullptr);

    // Initial layout
    if (litehtmlDoc_) {
        litehtmlDoc_->render(static_cast<litehtml::pixel_t>(viewportWidth_));
    }

    auto* skia = static_cast<render::SkiaRenderer*>(renderer_.get());

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

        // 3. Bind WebGL FBO before JS callbacks (so gl.bindFramebuffer(null) targets canvas)
        auto* webglScene = dynamic_cast<webgl::WebGLScene*>(sceneLayer_.get());
        if (webglScene && webglScene->webglContext()) {
            webglScene->webglContext()->bindCanvasFBO();
        }

        // 3a. Fire requestAnimationFrame callbacks
        timers_->fireAnimationFrames(now);

        // 3b. Run pending JS jobs (promises, etc.)
        jsRuntime_->executePendingJobs();

        // 3c. Unbind WebGL FBO
        if (webglScene && webglScene->webglContext()) {
            webglScene->webglContext()->unbindCanvasFBO();
        }

        // 4. Re-layout if DOM is dirty
        if (document_ && document_->isDirty() && litehtmlDoc_) {
            litehtmlDoc_->render(static_cast<litehtml::pixel_t>(viewportWidth_));
            document_->clearDirty();
            uiDirty_ = true;
        }

        // === GPU FRAME (OpenGL) ===

        // 5a. Rasterize UI to Skia surface (CPU) if dirty
        if (uiDirty_ || !hasRenderedOnce_) {
            renderer_->beginFrame(viewportWidth_, viewportHeight_);

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
            hasRenderedOnce_ = true;
            uiDirty_ = false;
        }

        // 5b. Upload Skia pixels to GL texture
        skia->uploadToGPU();

        // 5c. Scene layer prepares vertex data
        auto* canvasScene = dynamic_cast<canvas::CanvasScene*>(sceneLayer_.get());
        if (canvasScene) {
            canvasScene->prepareFrame(gl_.get(), viewportWidth_, viewportHeight_);
        }

        // 5d. Set viewport and clear
        glViewport(0, 0, viewportWidth_, viewportHeight_);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // 5e. Render pass 1: scene draws (directly to default framebuffer)
        if (sceneLayer_) {
            sceneLayer_->onRender(gl_.get(), viewportWidth_, viewportHeight_, totalFrameMs_);
        }

        // 5f. Render pass 2: composite UI overlay (premultiplied alpha)
        GLuint uiTex = skia->getUITexture();
        if (uiTex) {
            float w = (float)viewportWidth_, h = (float)viewportHeight_;
            render::TextureVertex quad[6] = {
                {0, 0, 0, 0}, {w, 0, 1, 0}, {w, h, 1, 1},
                {0, 0, 0, 0}, {w, h, 1, 1}, {0, h, 0, 1},
            };

            glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);

            glBindVertexArray(uiQuadVAO_);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex),
                                  (void*)offsetof(render::TextureVertex, u));

            glUseProgram(gl_->textureProgram());
            float viewport[2] = {w, h};
            glUniform2fv(gl_->textureViewportLoc(), 1, viewport);
            glUniform1i(gl_->textureSamplerLoc(), 0);

            // Premultiplied alpha blend (Skia output is premultiplied)
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, uiTex);

            glDrawArrays(GL_TRIANGLES, 0, 6);

            glBindVertexArray(0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDisable(GL_BLEND);
        }

        // 5g. Swap buffers
        gl_->swapBuffers();

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
            uiDirty_ = true;  // refresh stats overlay text
        }
    }
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void Engine::handleResize(int w, int h) {
    viewportWidth_ = w;
    viewportHeight_ = h;
    uiDirty_ = true;
    hasRenderedOnce_ = false;
    container_->setViewport(w, h);
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

void Engine::handleKeyDown(int keycode, int scancode, int mod, bool repeat) {
    if (document_) {
        dom::KeyboardEvent evt("keydown");
        evt.setKey(sdlKeycodeToWebKey(keycode, mod));
        evt.setCode(sdlScancodeToWebCode(scancode));
        evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
        evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
        evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
        evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
        evt.setRepeat(repeat);
        dom::Element* target = document_->body();
        if (target) {
            dispatchEvent(target, evt);
        }
    }
}

void Engine::handleKeyUp(int keycode, int scancode, int mod, bool repeat) {
    if (document_) {
        dom::KeyboardEvent evt("keyup");
        evt.setKey(sdlKeycodeToWebKey(keycode, mod));
        evt.setCode(sdlScancodeToWebCode(scancode));
        evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
        evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
        evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
        evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
        evt.setRepeat(repeat);
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
    constexpr int numLines = 2;
    const float boxW = 320.0f;
    const float boxH = pad * 2 + lineH * numLines;
    const float boxX = static_cast<float>(viewportWidth_) - boxW - 8.0f;
    const float boxY = 8.0f;

    // Opaque background — covers previous frame's overlay without re-compositing
    renderer_->fillRect(boxX, boxY, boxW, boxH, {0, 0, 0, 255});

    float y = boxY + pad;
    float x = boxX + pad;
    Color green{100, 220, 100, 255};
    Color yellow{220, 200, 80, 255};
    Color red{220, 80, 80, 255};
    Color label{140, 140, 140, 255};

    Color fpsColor = statsFps_ >= 55.0 ? green : (statsFps_ >= 30.0 ? yellow : red);

    char buf[128];

    // Line 1: FPS + frame time + render time
    std::snprintf(buf, sizeof(buf), "FPS: %.0f  Frame: %.1fms  Render: %.1fms",
                  statsFps_, statsFrameTimeMs_, frameTimeMs);
    renderer_->drawText(buf, x, y, statsFont_, fpsColor);
    y += lineH;

    // Line 2: Min/Max + viewport
    double dispMin = statsMinFrameMs_ < 999.0 ? statsMinFrameMs_ : 0.0;
    std::snprintf(buf, sizeof(buf), "Min/Max: %.1f/%.1fms  Viewport: %dx%d",
                  dispMin, statsMaxFrameMs_, viewportWidth_, viewportHeight_);
    renderer_->drawText(buf, x, y, statsFont_, label);
}

} // namespace bro::engine
