#include "engine/engine.h"
#include "engine/system_overlay.h"

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
#include "js/image_bindings.h"
#include "js/fetch_bindings.h"
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
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "engine/default_styles.h"
#include "util/log.h"
#include "util/time.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <glad/gl.h>
#include <cstdio>
#include <stdexcept>
#include <functional>
#include <string>
#include <unordered_map>

namespace {

// Access litehtml::element::m_renders (protected) via pointer-to-member.
// This is needed to purge expired weak_ptr<render_item> entries that
// otherwise retain deallocated render-item memory (make_shared + weak_ptr
// prevents the combined allocation from being freed).
struct LitehtmlElementAccess : litehtml::element {
    static auto rendersPtr() { return &LitehtmlElementAccess::m_renders; }
};
static const auto kRendersPtr = LitehtmlElementAccess::rendersPtr();

void purgeExpiredRenders(const litehtml::element::ptr& el) {
    if (!el) return;
    auto& renders = el.get()->*kRendersPtr;
    renders.remove_if([](const std::weak_ptr<litehtml::render_item>& w) {
        return w.expired();
    });
    for (auto& child : el->children()) {
        purgeExpiredRenders(child);
    }
}

} // anonymous namespace

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
// Form control helpers (used by both draw loop and event handlers)
// ---------------------------------------------------------------------------

static layout::ElInput* getElInput(dom::Element* el);
static layout::ElTextarea* getElTextarea(dom::Element* el);
static layout::ElSelect* getElSelect(dom::Element* el);

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

    // Load user stylesheets, prepended with browser-like defaults so apps
    // are visible and have sensible form control styling without an
    // explicit stylesheet.
    std::string userStyles = kDefaultStyles;
    userStyles += "\n";
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

    // 9. Set up window/navigator BEFORE DOM bindings (polyfills reference window)
    {
        JSContext* ctx = jsRuntime_->getContext();
        JSValue global = JS_GetGlobalObject(ctx);

        JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
        JS_SetPropertyStr(ctx, global, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));
        JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, viewportWidth_));
        JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, viewportHeight_));

        JSValue nav = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, nav, "userAgent", JS_NewString(ctx, "Bro/1.0"));
        JS_SetPropertyStr(ctx, nav, "platform", JS_NewString(ctx, "Win32"));
        JS_SetPropertyStr(ctx, nav, "language", JS_NewString(ctx, "en-US"));
        JS_SetPropertyStr(ctx, global, "navigator", nav);

        const char* windowEventPolyfill = R"JS(
(function() {
    var listeners = {};
    globalThis.__bro_win_listeners = listeners;
    globalThis.addEventListener = function(type, fn) {
        if (!listeners[type]) listeners[type] = [];
        listeners[type].push(fn);
    };
    globalThis.removeEventListener = function(type, fn) {
        var arr = listeners[type];
        if (!arr) return;
        var idx = arr.indexOf(fn);
        if (idx >= 0) arr.splice(idx, 1);
    };
    globalThis.__bro_dispatch_window_event = function(type, event) {
        var arr = listeners[type];
        if (!arr) return;
        for (var i = 0; i < arr.length; i++) {
            try { arr[i](event); } catch(e) { console.error('Event handler error:', e); }
        }
    };
})();
)JS";
        JSValue r = JS_Eval(ctx, windowEventPolyfill, strlen(windowEventPolyfill),
                            "<window-events>", JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(ctx, r);

        JS_FreeValue(ctx, global);
    }

    // 9a. Install DOM JS bindings (after window so polyfills work)
    js::DomBindings::install(jsRuntime_->getContext(), document_.get());

    // 9b. Install Canvas 2D and WebGL2 bindings + getContext factory
    js::CanvasBindings::install(jsRuntime_->getContext());
    js::WebGL2Bindings::install(jsRuntime_->getContext());
    js::ImageBindings::install(jsRuntime_->getContext(), manifest_.basePath);
    js::FetchBindings::install(jsRuntime_->getContext(), manifest_.basePath);
    js::DomBindings::setGetContextFactory(jsRuntime_->getContext(),
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

    // 12. System overlay (loads panels from system/ sibling directory)
    //     Shares the JS runtime — each panel gets its own JSContext.
    systemOverlay_ = std::make_unique<SystemOverlay>(jsRuntime_.get(), gl_.get(),
                                                      viewportWidth_, viewportHeight_);
    systemOverlay_->loadPanels("system");

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
    systemOverlay_.reset();
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
    // Clean up per-runtime DomBindings state before the runtime is freed.
    if (jsRuntime_) {
        js::DomBindings::cleanupRuntime(jsRuntime_->getRuntime());
    }
    // Destroy JS runtime BEFORE document — JS_FreeRuntime() runs GC finalizers
    // that dereference Element pointers, so elements must still be alive.
    jsRuntime_.reset();
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
    eventLoop_->onTextInput = [this](const std::string& text) {
        handleTextInput(text);
    };
    eventLoop_->onWheel = [this](float x, float y, float dx, float dy) {
        handleWheel(x, y, dx, dy);
    };

    // Tell the layout container to skip full-viewport backgrounds when a scene
    // layer is active, so the scene shows through behind the HTML UI.

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

        // 2. Tick timers + JS execution
        double now = util::currentTimeMs();
        double t0 = now;
        timers_->tick(now);

        // 2b. Tick system overlay timers
        if (systemOverlay_) {
            systemOverlay_->tick(now);
        }

        // 3. Bind WebGL FBO before JS callbacks (so gl.bindFramebuffer(null) targets canvas)
        auto* webglScene = dynamic_cast<webgl::WebGLScene*>(sceneLayer_.get());
        if (webglScene && webglScene->webglContext()) {
            webglScene->webglContext()->bindCanvasFBO();
        }

        // 3a. Fire requestAnimationFrame callbacks
        timers_->fireAnimationFrames(now);

        double tGlSave = util::currentTimeMs();

        // 3b. Run pending JS jobs (promises, etc.)
        jsRuntime_->executePendingJobs();

        // 3c. Unbind WebGL FBO
        if (webglScene && webglScene->webglContext()) {
            webglScene->webglContext()->unbindCanvasFBO();
        }

        double tJs = util::currentTimeMs();
        accumJsMs_ += tJs - t0;
        accumGlStateMs_ += tJs - tGlSave;  // GL save is inside JS phase

        // 3d. Periodic QuickJS cycle-collector GC + orphan sweep
        if (now - lastGCMs_ >= kGCIntervalMs) {
            js::DomBindings::sweepOrphanedWrappers(jsRuntime_->getContext());
            JS_RunGC(jsRuntime_->getRuntime());
            lastGCMs_ = now;
        }

        // 4. Re-layout + rasterize UI at most ~60fps.
        //    DOM mutations accumulate between UI frames; the cached Skia
        //    texture is composited every frame regardless (cheap).
        bool uiFrameDue = (now - lastUIRenderMs_ >= kUIFrameIntervalMs)
                          || !hasRenderedOnce_;

        // System overlay composites its own cached texture — no need to
        // force app UI re-rasterization every frame just because it's visible.

        double tLayout = tJs;
        if (document_ && document_->isDirty() && litehtmlDoc_ && uiFrameDue) {
            // Always rebuild the render tree when dirty.
            // DOM mutations (text changes, style updates) may alter the
            // litehtml element tree in ways that the render tree cache
            // doesn't detect.  Rebuilding is cheap relative to layout.
            purgeExpiredRenders(litehtmlDoc_->root());
            litehtmlDoc_->rebuild_render_tree();
            document_->clearStructureDirty();
            litehtmlDoc_->render(static_cast<litehtml::pixel_t>(viewportWidth_));
            document_->clearDirty();
            uiDirty_ = true;
            lastUIRenderMs_ = now;
            tLayout = util::currentTimeMs();
        }
        accumLayoutMs_ += tLayout - tJs;

        // === GPU FRAME (OpenGL) ===

        // 5a. Rasterize UI to Skia surface (CPU) if dirty
        double tRaster = tLayout;
        if (uiDirty_ || !hasRenderedOnce_) {
            renderer_->beginFrame(viewportWidth_, viewportHeight_);

            container_->drawStats.reset();
            double tDraw0 = util::currentTimeMs();
            if (litehtmlDoc_) {
                litehtml::position clip(0, 0,
                                        static_cast<litehtml::pixel_t>(viewportWidth_),
                                        static_cast<litehtml::pixel_t>(viewportHeight_));
                litehtmlDoc_->draw(
                    reinterpret_cast<litehtml::uint_ptr>(renderer_.get()), 0, 0, &clip);
            }
            // Draw overlays after all elements (z-order on top)
            if (document_) {
                auto* activeEl = document_->activeElement();
                auto* sel = getElSelect(activeEl);
                if (sel && sel->isOpen()) {
                    sel->drawDropdown();
                }
                auto* inp = getElInput(activeEl);
                if (inp && inp->isPickerOpen()) {
                    inp->drawColorPicker();
                }
            }
            accumDrawMs_ += util::currentTimeMs() - tDraw0;

            renderer_->endFrame();
            hasRenderedOnce_ = true;
            uiDirty_ = false;
            tRaster = util::currentTimeMs();
        }

        // 5b. Upload Skia pixels to GL texture
        double tUpload0 = util::currentTimeMs();
        skia->uploadToGPU();
        accumUploadMs_ += util::currentTimeMs() - tUpload0;
        accumRasterMs_ += util::currentTimeMs() - tLayout;

        double tGpu = util::currentTimeMs();

        // 5c. Scene layer prepares vertex data
        auto* canvasScene = dynamic_cast<canvas::CanvasScene*>(sceneLayer_.get());
        if (canvasScene) {
            canvasScene->prepareFrame(gl_.get(), viewportWidth_, viewportHeight_);
        }

        // 5d. Set viewport and clear
        glViewport(0, 0, viewportWidth_, viewportHeight_);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, uiTex);

            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // 5g. Render pass 3: composite system overlay (premultiplied alpha)
        if (systemOverlay_ && systemOverlay_->isVisible()) {
            systemOverlay_->render(viewportWidth_, viewportHeight_);

            // Ganesh may have changed GL state — restore what we need
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, viewportWidth_, viewportHeight_);

            GLuint sysTex = systemOverlay_->getTexture();
            if (sysTex) {
                float w = (float)viewportWidth_, h = (float)viewportHeight_;
                render::TextureVertex quad[6] = {
                    {0, 0, 0, 0}, {w, 0, 1, 0}, {w, h, 1, 1},
                    {0, 0, 0, 0}, {w, h, 1, 1}, {0, h, 0, 1},
                };

                glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);

                glBindVertexArray(uiQuadVAO_);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                                      sizeof(render::TextureVertex), (void*)0);
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                      sizeof(render::TextureVertex),
                                      (void*)offsetof(render::TextureVertex, u));

                glUseProgram(gl_->textureProgram());
                float viewport[2] = {w, h};
                glUniform2fv(gl_->textureViewportLoc(), 1, viewport);
                glUniform1i(gl_->textureSamplerLoc(), 0);

                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                glDisable(GL_SCISSOR_TEST);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, sysTex);

                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }

        // Reset GL to clean defaults so Three.js/WebGL re-binds everything
        // it needs on the next frame. This avoids expensive glGetIntegerv
        // queries which force GPU pipeline flushes.
        glUseProgram(0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);

        // 5g. Swap buffers
        gl_->swapBuffers();
        accumGpuMs_ += util::currentTimeMs() - tGpu;

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
            double n = statsFrameCount_;
            phaseJsMs_ = accumJsMs_ / n;
            phaseLayoutMs_ = accumLayoutMs_ / n;
            phaseRasterMs_ = accumRasterMs_ / n;
            phaseGpuMs_ = accumGpuMs_ / n;
            phaseGlStateMs_ = accumGlStateMs_ / n;
            phaseDrawMs_ = accumDrawMs_ / n;
            phaseUploadMs_ = accumUploadMs_ / n;
            accumJsMs_ = accumLayoutMs_ = accumRasterMs_ = accumGpuMs_ = accumGlStateMs_ = 0.0;
            accumDrawMs_ = accumUploadMs_ = 0.0;
            statsAccumMs_ = 0.0;
            statsFrameCount_ = 0;
            statsMinFrameMs_ = 999.0;
            statsMaxFrameMs_ = 0.0;
            uiDirty_ = true;  // refresh overlay

            // Push perf data to system overlay JS
            if (systemOverlay_) {
                systemOverlay_->updatePerf(statsFps_, statsFrameTimeMs_,
                                           phaseJsMs_, phaseLayoutMs_,
                                           phaseRasterMs_, phaseGpuMs_,
                                           phaseDrawMs_,
                                           viewportWidth_, viewportHeight_);
            }
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
    if (systemOverlay_) {
        systemOverlay_->onResize(w, h);
    }
    if (litehtmlDoc_) {
        litehtmlDoc_->render(static_cast<litehtml::pixel_t>(w));
    }

    // Update JS globals and dispatch resize event to window listeners
    if (jsRuntime_) {
        JSContext* ctx = jsRuntime_->getContext();
        JSValue global = JS_GetGlobalObject(ctx);

        // Update innerWidth / innerHeight
        JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, w));
        JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, h));

        // Update canvas element width/height attributes via JS
        const char* updateScript = R"JS(
            (function(w, h) {
                var c = document.querySelector('canvas');
                if (c) { c.width = w; c.height = h; }
            })
        )JS";
        JSValue fn = JS_Eval(ctx, updateScript, strlen(updateScript),
                             "<resize>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsFunction(ctx, fn)) {
            JSValue args[2] = { JS_NewInt32(ctx, w), JS_NewInt32(ctx, h) };
            JSValue ret = JS_Call(ctx, fn, global, 2, args);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
        }
        JS_FreeValue(ctx, fn);

        // Dispatch resize event to window listeners
        JSValue dispatch = JS_GetPropertyStr(ctx, global, "__bro_dispatch_window_event");
        if (JS_IsFunction(ctx, dispatch)) {
            JSValue evtType = JS_NewString(ctx, "resize");
            JSValue evt = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, evt, "type", JS_NewString(ctx, "resize"));
            JS_SetPropertyStr(ctx, evt, "target", JS_DupValue(ctx, global));
            JSValue dArgs[2] = { evtType, evt };
            JSValue ret = JS_Call(ctx, dispatch, global, 2, dArgs);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, evtType);
            JS_FreeValue(ctx, evt);
        }
        JS_FreeValue(ctx, dispatch);

        JS_FreeValue(ctx, global);
        jsRuntime_->executePendingJobs();
    }
}

// ---------------------------------------------------------------------------
// Input focus helpers
// ---------------------------------------------------------------------------

static layout::ElInput* getElInput(dom::Element* el) {
    if (!el) return nullptr;
    auto lh = el->litehtmlElement();
    if (!lh) return nullptr;
    return dynamic_cast<layout::ElInput*>(lh.get());
}

static layout::ElTextarea* getElTextarea(dom::Element* el) {
    if (!el) return nullptr;
    auto lh = el->litehtmlElement();
    if (!lh) return nullptr;
    return dynamic_cast<layout::ElTextarea*>(lh.get());
}

static layout::ElSelect* getElSelect(dom::Element* el) {
    if (!el) return nullptr;
    auto lh = el->litehtmlElement();
    if (!lh) return nullptr;
    return dynamic_cast<layout::ElSelect*>(lh.get());
}

// Returns true if the element is a focusable text-editing control (input or textarea)
static bool isTextEditable(dom::Element* el) {
    return getElInput(el) || getElTextarea(el);
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void Engine::handleMouseDown(float x, float y, int button) {
    if (litehtmlDoc_) {
        litehtml::position::vector redraw;
        litehtmlDoc_->on_lbutton_down(static_cast<int>(x), static_cast<int>(y),
                                       static_cast<int>(x), static_cast<int>(y), redraw);
        if (!redraw.empty()) uiDirty_ = true;
    }

    if (document_) {
        dom::MouseEvent evt("mousedown");
        evt.setClientX(static_cast<double>(x));
        evt.setClientY(static_cast<double>(y));
        evt.setButton(button);
        dom::Element* target = hitTest(x, y);
        if (target) {
            // Unfocus previous controls
            auto* prevActive = document_->activeElement();
            auto* prevInput = getElInput(prevActive);
            if (prevInput) {
                // Close color picker if clicking outside it
                if (prevInput->isPickerOpen()) {
                    auto dp = prevInput->lastDrawPos();
                    float px = dp.x, py = dp.y + dp.h + 2;
                    float pw = 200.0f, ph = 160.0f;
                    bool inPicker = (x >= px && x < px + pw && y >= py && y < py + ph);
                    bool inSwatch = (x >= dp.x && x < dp.x + dp.w && y >= dp.y && y < dp.y + dp.h);
                    if (!inPicker && !inSwatch) {
                        prevInput->setPickerOpen(false);
                    }
                }
                prevInput->setFocused(false);
                uiDirty_ = true;
            }
            auto* prevTextarea = getElTextarea(prevActive);
            if (prevTextarea) {
                prevTextarea->setFocused(false);
                uiDirty_ = true;
            }
            auto* prevSelect = getElSelect(prevActive);
            if (prevSelect && prevSelect->isOpen()) {
                // Check if click is inside the dropdown
                auto dp = prevSelect->lastDrawPos();
                auto opts = prevSelect->getOptions();
                auto font = prevSelect->css().get_font();
                float lineH = font ? static_cast<float>(prevSelect->css().get_font_metrics().height) : 20.0f;
                float dropY = dp.y + dp.h;
                float dropH = lineH * static_cast<float>(opts.size()) + 4.0f;
                bool inDropdown = (x >= dp.x && x < dp.x + dp.w &&
                                   y >= dropY && y < dropY + dropH);
                if (inDropdown) {
                    // Select the clicked option
                    int idx = static_cast<int>((y - dropY - 2.0f) / lineH);
                    idx = std::clamp(idx, 0, static_cast<int>(opts.size()) - 1);
                    prevSelect->setSelectedIndex(idx);
                    prevSelect->setOpen(false);
                    // Update value attribute
                    if (prevActive) {
                        prevActive->setAttribute("value", opts[idx].value);
                        dom::Event changeEvt("change");
                        dispatchEvent(prevActive, changeEvt);
                        dispatchInputEvent(prevActive);
                    }
                    uiDirty_ = true;
                    // Don't process further — we handled the dropdown click
                    dispatchEvent(target, evt);
                    return;
                }
                prevSelect->setOpen(false);
                uiDirty_ = true;
            }

            document_->setActiveElement(target);
            jsRuntime_->executePendingJobs();

            // Focus new input if clicking on one
            auto* newInput = getElInput(target);
            auto* newTextarea = getElTextarea(target);
            auto* newSelect = getElSelect(target);

            if (newInput) {
                newInput->setFocused(true);
                auto itype = newInput->inputType();

                if (itype == layout::ElInput::InputType::Checkbox) {
                    // Toggle checked state
                    const char* checked = newInput->get_attr("checked");
                    if (checked) {
                        target->removeAttribute("checked");
                    } else {
                        target->setAttribute("checked", "");
                    }
                    dom::Event changeEvt("change");
                    dispatchEvent(target, changeEvt);
                    dispatchInputEvent(target);
                    uiDirty_ = true;
                } else if (itype == layout::ElInput::InputType::Radio) {
                    // Uncheck other radios with same name
                    const char* name = newInput->get_attr("name");
                    if (name && *name && document_) {
                        auto* body = document_->body();
                        if (body) {
                            auto radios = body->querySelectorAll("input[type=\"radio\"]");
                            for (auto* el : radios) {
                                if (el == target) continue;
                                auto* otherInput = getElInput(el);
                                if (otherInput) {
                                    const char* otherName = otherInput->get_attr("name");
                                    if (otherName && strcmp(otherName, name) == 0) {
                                        el->removeAttribute("checked");
                                    }
                                }
                            }
                        }
                    }
                    target->setAttribute("checked", "");
                    dom::Event changeEvt("change");
                    dispatchEvent(target, changeEvt);
                    dispatchInputEvent(target);
                    uiDirty_ = true;
                } else if (itype == layout::ElInput::InputType::Range) {
                    // Click to set value at position
                    auto dp = newInput->lastDrawPos();
                    float thumbR = 7.0f;
                    float trackStart = dp.x + thumbR;
                    float trackEnd = dp.x + dp.w - thumbR;
                    float pct = (trackEnd > trackStart) ?
                        std::clamp((x - trackStart) / (trackEnd - trackStart), 0.0f, 1.0f) : 0.0f;
                    float mn = newInput->rangeMin(), mx = newInput->rangeMax();
                    float val = mn + pct * (mx - mn);
                    // Snap to step
                    float step = newInput->rangeStep();
                    if (step > 0) {
                        val = mn + std::round((val - mn) / step) * step;
                        val = std::clamp(val, mn, mx);
                    }
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%g", static_cast<double>(val));
                    target->setAttribute("value", buf);
                    newInput->setDragging(true);
                    dispatchInputEvent(target);
                    uiDirty_ = true;
                } else if (itype == layout::ElInput::InputType::Color) {
                    if (newInput->isPickerOpen()) {
                        // Click inside picker to select color
                        auto dp = newInput->lastDrawPos();
                        float px = dp.x, py = dp.y + dp.h + 2;
                        float pw = 200.0f, ph = 160.0f;
                        if (x >= px && x < px + pw && y >= py && y < py + ph) {
                            float cellW = (pw - 4) / 10.0f;
                            float cellH = (ph - 4) / 8.0f;
                            int col = static_cast<int>((x - px - 2) / cellW);
                            int row = static_cast<int>((y - py - 2) / cellH);
                            col = std::clamp(col, 0, 9);
                            row = std::clamp(row, 0, 7);

                            // Reproduce the same HSL->RGB conversion
                            float hue = col * 36.0f;
                            float sat, lit;
                            if (row == 0) {
                                sat = 0.0f; lit = col / 9.0f;
                            } else {
                                sat = 1.0f; lit = 0.15f + (row - 1) * 0.1f;
                            }

                            auto hue2rgb = [](float p, float q, float t) -> float {
                                if (t < 0) t += 1; if (t > 1) t -= 1;
                                if (t < 1.0f/6) return p + (q-p)*6*t;
                                if (t < 1.0f/2) return q;
                                if (t < 2.0f/3) return p + (q-p)*(2.0f/3-t)*6;
                                return p;
                            };
                            uint8_t cr, cg, cb;
                            if (sat == 0) {
                                cr = cg = cb = static_cast<uint8_t>(lit * 255);
                            } else {
                                float q = lit < 0.5f ? lit*(1+sat) : lit+sat-lit*sat;
                                float p = 2*lit-q;
                                float hn = hue/360.0f;
                                cr = static_cast<uint8_t>(hue2rgb(p, q, hn+1.0f/3)*255);
                                cg = static_cast<uint8_t>(hue2rgb(p, q, hn)*255);
                                cb = static_cast<uint8_t>(hue2rgb(p, q, hn-1.0f/3)*255);
                            }

                            char hex[8];
                            snprintf(hex, sizeof(hex), "#%02x%02x%02x", cr, cg, cb);
                            target->setAttribute("value", hex);
                            dom::Event changeEvt("change");
                            dispatchEvent(target, changeEvt);
                            dispatchInputEvent(target);
                        }
                        newInput->setPickerOpen(false);
                    } else {
                        newInput->setPickerOpen(true);
                    }
                    SDL_StopTextInput(window_->getSDLWindow());
                    uiDirty_ = true;
                } else if (newInput->isTextType()) {
                    // Check for number spin button click
                    if (newInput->inputType() == layout::ElInput::InputType::Number) {
                        auto dp = newInput->lastDrawPos();
                        float btnW = 16.0f;
                        float bx = dp.x + dp.w - btnW;
                        if (x >= bx && x <= dp.x + dp.w) {
                            // Click on spin buttons
                            std::string val = target->getAttribute("value");
                            float v = val.empty() ? 0 : static_cast<float>(atof(val.c_str()));
                            float step = newInput->rangeStep();
                            float midY = dp.y + dp.h / 2;
                            v += (y < midY) ? step : -step;
                            const char* minAttr = newInput->get_attr("min");
                            const char* maxAttr = newInput->get_attr("max");
                            if (minAttr) v = std::max(v, newInput->rangeMin());
                            if (maxAttr) v = std::min(v, newInput->rangeMax());
                            char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                            target->setAttribute("value", buf);
                            newInput->setCursorPos(static_cast<int>(strlen(buf)));
                            dispatchInputEvent(target);
                        }
                    }
                    const char* val = newInput->get_attr("value");
                    newInput->setCursorPos(val ? static_cast<int>(strlen(val)) : 0);
                    SDL_StartTextInput(window_->getSDLWindow());
                    uiDirty_ = true;
                } else {
                    // Button types — no text input
                    SDL_StopTextInput(window_->getSDLWindow());
                    uiDirty_ = true;
                }
            } else if (newTextarea) {
                newTextarea->setFocused(true);
                const char* val = newTextarea->get_attr("value");
                newTextarea->setCursorPos(val ? static_cast<int>(strlen(val)) : 0);
                SDL_StartTextInput(window_->getSDLWindow());
                uiDirty_ = true;
            } else if (newSelect) {
                // Toggle dropdown open/close
                newSelect->setOpen(!newSelect->isOpen());
                if (newSelect->isOpen()) {
                    newSelect->setHighlightedIndex(newSelect->selectedIndex());
                }
                SDL_StopTextInput(window_->getSDLWindow());
                uiDirty_ = true;
            } else {
                SDL_StopTextInput(window_->getSDLWindow());
            }

            dispatchEvent(target, evt);
        }
    }
}

void Engine::handleMouseUp(float x, float y, int button) {
    if (litehtmlDoc_) {
        litehtml::position::vector redraw;
        litehtmlDoc_->on_lbutton_up(static_cast<int>(x), static_cast<int>(y),
                                     static_cast<int>(x), static_cast<int>(y), redraw);
        if (!redraw.empty()) uiDirty_ = true;
    }

    // Stop range slider dragging
    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* input = getElInput(activeEl);
        if (input && input->isDragging()) {
            input->setDragging(false);
            dom::Event changeEvt("change");
            dispatchEvent(activeEl, changeEvt);
            uiDirty_ = true;
        }
    }

    if (document_) {
        dom::MouseEvent clickEvt("click");
        clickEvt.setClientX(static_cast<double>(x));
        clickEvt.setClientY(static_cast<double>(y));
        clickEvt.setButton(button);
        dom::Element* target = hitTest(x, y);
        if (target) {
            dispatchEvent(target, clickEvt);
            jsRuntime_->executePendingJobs();
        }
    }
}

void Engine::handleMouseMove(float x, float y) {
    if (litehtmlDoc_) {
        litehtml::position::vector redraw;
        litehtmlDoc_->on_mouse_over(static_cast<int>(x), static_cast<int>(y),
                                     static_cast<int>(x), static_cast<int>(y), redraw);
        if (!redraw.empty()) {
            uiDirty_ = true;
        }
    }

    // Range slider dragging
    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* rangeInput = getElInput(activeEl);
        if (rangeInput && rangeInput->isDragging()) {
            auto dp = rangeInput->lastDrawPos();
            float thumbR = 7.0f;
            float trackStart = dp.x + thumbR;
            float trackEnd = dp.x + dp.w - thumbR;
            float pct = (trackEnd > trackStart) ?
                std::clamp((x - trackStart) / (trackEnd - trackStart), 0.0f, 1.0f) : 0.0f;
            float mn = rangeInput->rangeMin(), mx = rangeInput->rangeMax();
            float val = mn + pct * (mx - mn);
            float step = rangeInput->rangeStep();
            if (step > 0) {
                val = mn + std::round((val - mn) / step) * step;
                val = std::clamp(val, mn, mx);
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", static_cast<double>(val));
            activeEl->setAttribute("value", buf);
            dispatchInputEvent(activeEl);
            uiDirty_ = true;
        }
    }

    // Update dropdown highlight on hover
    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* select = getElSelect(activeEl);
        if (select && select->isOpen()) {
            auto dp = select->lastDrawPos();
            auto opts = select->getOptions();
            auto font = select->css().get_font();
            float lineH = font ? static_cast<float>(select->css().get_font_metrics().height) : 20.0f;
            float dropY = dp.y + dp.h;
            float dropH = lineH * static_cast<float>(opts.size()) + 4.0f;
            if (x >= dp.x && x < dp.x + dp.w && y >= dropY && y < dropY + dropH) {
                int idx = static_cast<int>((y - dropY - 2.0f) / lineH);
                idx = std::clamp(idx, 0, static_cast<int>(opts.size()) - 1);
                if (idx != select->highlightedIndex()) {
                    select->setHighlightedIndex(idx);
                    uiDirty_ = true;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Keyboard events
// ---------------------------------------------------------------------------

// Helper: update input value and dispatch "input" event for v-model
void Engine::dispatchInputEvent(dom::Element* el) {
    if (!el) return;
    dom::Event evt("input");
    dispatchEvent(el, evt);
    jsRuntime_->executePendingJobs();
    uiDirty_ = true;
}

void Engine::handleKeyDown(int keycode, int scancode, int mod, bool repeat) {
    // F8 toggles system overlay
    if (keycode == SDLK_F8 && !repeat) {
        if (systemOverlay_) {
            systemOverlay_->toggle();
            uiDirty_ = true;
        }
        return;
    }

    if (!document_) return;

    // Tab key: advance focus to next/previous focusable element
    if (keycode == SDLK_TAB) {
        advanceFocus((mod & SDL_KMOD_SHIFT) != 0);
        uiDirty_ = true;
        return;
    }

    // Check if a text input is focused — handle editing keys
    auto* activeEl = document_->activeElement();
    auto* input = getElInput(activeEl);

    // Handle checkbox/radio space toggle
    if (input && input->isFocused()) {
        auto itype = input->inputType();
        if ((itype == layout::ElInput::InputType::Checkbox || itype == layout::ElInput::InputType::Radio)
            && keycode == SDLK_SPACE) {
            if (itype == layout::ElInput::InputType::Checkbox) {
                const char* checked = input->get_attr("checked");
                if (checked)
                    activeEl->removeAttribute("checked");
                else
                    activeEl->setAttribute("checked", "");
            } else {
                activeEl->setAttribute("checked", "");
            }
            dom::Event changeEvt("change");
            dispatchEvent(activeEl, changeEvt);
            dispatchInputEvent(activeEl);

            dom::KeyboardEvent evt("keydown");
            evt.setKey(sdlKeycodeToWebKey(keycode, mod));
            evt.setCode(sdlScancodeToWebCode(scancode));
            evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
            evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
            evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
            evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
            evt.setRepeat(repeat);
            dispatchEvent(activeEl, evt);
            return;
        }

        // Handle range arrow keys
        if (itype == layout::ElInput::InputType::Range) {
            bool handled = false;
            if (keycode == SDLK_LEFT || keycode == SDLK_DOWN) {
                float v = input->rangeValue() - input->rangeStep();
                v = std::clamp(v, input->rangeMin(), input->rangeMax());
                char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                activeEl->setAttribute("value", buf);
                dispatchInputEvent(activeEl);
                handled = true;
            } else if (keycode == SDLK_RIGHT || keycode == SDLK_UP) {
                float v = input->rangeValue() + input->rangeStep();
                v = std::clamp(v, input->rangeMin(), input->rangeMax());
                char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                activeEl->setAttribute("value", buf);
                dispatchInputEvent(activeEl);
                handled = true;
            }
            if (handled) {
                dom::KeyboardEvent evt("keydown");
                evt.setKey(sdlKeycodeToWebKey(keycode, mod));
                evt.setCode(sdlScancodeToWebCode(scancode));
                evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
                evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
                evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
                evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
                evt.setRepeat(repeat);
                dispatchEvent(activeEl, evt);
                return;
            }
        }

        // Skip text editing for non-text types
        if (!input->isTextType()) {
            dom::KeyboardEvent nontextEvt("keydown");
            nontextEvt.setKey(sdlKeycodeToWebKey(keycode, mod));
            nontextEvt.setCode(sdlScancodeToWebCode(scancode));
            nontextEvt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
            nontextEvt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
            nontextEvt.setAltKey((mod & SDL_KMOD_ALT) != 0);
            nontextEvt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
            nontextEvt.setRepeat(repeat);
            dispatchEvent(activeEl, nontextEvt);
            return;
        }
    }

    if (input && input->isFocused() && input->isTextType()) {
        std::string val = activeEl->getAttribute("value");
        int pos = input->cursorPos();
        pos = std::clamp(pos, 0, static_cast<int>(val.size()));
        bool handled = false;

        if (keycode == SDLK_BACKSPACE) {
            if (pos > 0) {
                val.erase(pos - 1, 1);
                input->setCursorPos(pos - 1);
                activeEl->setAttribute("value", val);
                dispatchInputEvent(activeEl);
            }
            handled = true;
        } else if (keycode == SDLK_DELETE) {
            if (pos < static_cast<int>(val.size())) {
                val.erase(pos, 1);
                activeEl->setAttribute("value", val);
                dispatchInputEvent(activeEl);
            }
            handled = true;
        } else if (keycode == SDLK_LEFT) {
            if (pos > 0) {
                input->setCursorPos(pos - 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_RIGHT) {
            if (pos < static_cast<int>(val.size())) {
                input->setCursorPos(pos + 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_HOME) {
            input->setCursorPos(0);
            uiDirty_ = true;
            handled = true;
        } else if (keycode == SDLK_END) {
            input->setCursorPos(static_cast<int>(val.size()));
            uiDirty_ = true;
            handled = true;
        } else if (input->inputType() == layout::ElInput::InputType::Number &&
                   (keycode == SDLK_UP || keycode == SDLK_DOWN)) {
            // Increment/decrement number value
            float v = 0;
            if (!val.empty()) v = static_cast<float>(atof(val.c_str()));
            float step = input->rangeStep();
            v += (keycode == SDLK_UP) ? step : -step;
            // Clamp to min/max if specified
            float mn = input->rangeMin(), mx = input->rangeMax();
            const char* minAttr = input->get_attr("min");
            const char* maxAttr = input->get_attr("max");
            if (minAttr) v = std::max(v, mn);
            if (maxAttr) v = std::min(v, mx);
            char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
            activeEl->setAttribute("value", buf);
            input->setCursorPos(static_cast<int>(strlen(buf)));
            dispatchInputEvent(activeEl);
            handled = true;
        } else if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
            // Unfocus the input on Enter
            input->setFocused(false);
            SDL_StopTextInput(window_->getSDLWindow());
            uiDirty_ = true;
            handled = true;
        } else if (keycode == SDLK_ESCAPE) {
            // Unfocus on Escape
            input->setFocused(false);
            SDL_StopTextInput(window_->getSDLWindow());
            uiDirty_ = true;
            handled = true;
        } else if ((mod & SDL_KMOD_CTRL) && keycode == SDLK_A) {
            // Ctrl+A: select all (move cursor to end for now)
            input->setCursorPos(static_cast<int>(val.size()));
            uiDirty_ = true;
            handled = true;
        }

        if (handled) {
            // Still dispatch keydown event for JS listeners
            dom::KeyboardEvent evt("keydown");
            evt.setKey(sdlKeycodeToWebKey(keycode, mod));
            evt.setCode(sdlScancodeToWebCode(scancode));
            evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
            evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
            evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
            evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
            evt.setRepeat(repeat);
            dispatchEvent(activeEl, evt);
            return;
        }
    }

    // Check if a textarea is focused — handle multi-line editing keys
    auto* textarea = getElTextarea(activeEl);
    if (textarea && textarea->isFocused()) {
        std::string val = activeEl->getAttribute("value");
        int pos = textarea->cursorPos();
        pos = std::clamp(pos, 0, static_cast<int>(val.size()));
        bool handled = false;

        if (keycode == SDLK_BACKSPACE) {
            if (pos > 0) {
                val.erase(pos - 1, 1);
                textarea->setCursorPos(pos - 1);
                activeEl->setAttribute("value", val);
                dispatchInputEvent(activeEl);
            }
            handled = true;
        } else if (keycode == SDLK_DELETE) {
            if (pos < static_cast<int>(val.size())) {
                val.erase(pos, 1);
                activeEl->setAttribute("value", val);
                dispatchInputEvent(activeEl);
            }
            handled = true;
        } else if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
            // Insert newline in textarea
            val.insert(pos, 1, '\n');
            textarea->setCursorPos(pos + 1);
            activeEl->setAttribute("value", val);
            dispatchInputEvent(activeEl);
            handled = true;
        } else if (keycode == SDLK_LEFT) {
            if (pos > 0) {
                textarea->setCursorPos(pos - 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_RIGHT) {
            if (pos < static_cast<int>(val.size())) {
                textarea->setCursorPos(pos + 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_UP) {
            // Move cursor up one line
            int line = 0, col = 0;
            for (int i = 0; i < pos; ++i) {
                if (val[i] == '\n') { ++line; col = 0; } else { ++col; }
            }
            if (line > 0) {
                // Find start of previous line
                int prevLineStart = 0, prevLineLen = 0;
                int curLine = 0;
                for (int i = 0; i <= static_cast<int>(val.size()); ++i) {
                    if (curLine == line - 1) { prevLineStart = i; break; }
                    if (i < static_cast<int>(val.size()) && val[i] == '\n') ++curLine;
                }
                // Find length of previous line
                for (int i = prevLineStart; i < static_cast<int>(val.size()) && val[i] != '\n'; ++i)
                    ++prevLineLen;
                textarea->setCursorPos(prevLineStart + std::min(col, prevLineLen));
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_DOWN) {
            // Move cursor down one line
            int line = 0, col = 0;
            for (int i = 0; i < pos; ++i) {
                if (val[i] == '\n') { ++line; col = 0; } else { ++col; }
            }
            // Find start of next line
            int nextLineStart = -1;
            int curLine = 0;
            for (int i = 0; i < static_cast<int>(val.size()); ++i) {
                if (val[i] == '\n') {
                    if (curLine == line) { nextLineStart = i + 1; break; }
                    ++curLine;
                }
            }
            if (nextLineStart >= 0) {
                int nextLineLen = 0;
                for (int i = nextLineStart; i < static_cast<int>(val.size()) && val[i] != '\n'; ++i)
                    ++nextLineLen;
                textarea->setCursorPos(nextLineStart + std::min(col, nextLineLen));
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_HOME) {
            // Move to start of current line
            int lineStart = pos;
            while (lineStart > 0 && val[lineStart - 1] != '\n') --lineStart;
            textarea->setCursorPos(lineStart);
            uiDirty_ = true;
            handled = true;
        } else if (keycode == SDLK_END) {
            // Move to end of current line
            int lineEnd = pos;
            while (lineEnd < static_cast<int>(val.size()) && val[lineEnd] != '\n') ++lineEnd;
            textarea->setCursorPos(lineEnd);
            uiDirty_ = true;
            handled = true;
        } else if (keycode == SDLK_ESCAPE) {
            textarea->setFocused(false);
            SDL_StopTextInput(window_->getSDLWindow());
            uiDirty_ = true;
            handled = true;
        } else if ((mod & SDL_KMOD_CTRL) && keycode == SDLK_A) {
            textarea->setCursorPos(static_cast<int>(val.size()));
            uiDirty_ = true;
            handled = true;
        }

        if (handled) {
            dom::KeyboardEvent evt("keydown");
            evt.setKey(sdlKeycodeToWebKey(keycode, mod));
            evt.setCode(sdlScancodeToWebCode(scancode));
            evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
            evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
            evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
            evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
            evt.setRepeat(repeat);
            dispatchEvent(activeEl, evt);
            return;
        }
    }

    // Check if a select is open — handle arrow keys and enter
    auto* select = getElSelect(activeEl);
    if (select && select->isOpen()) {
        auto opts = select->getOptions();
        int hi = select->highlightedIndex();
        bool handled = false;

        if (keycode == SDLK_DOWN) {
            if (hi < static_cast<int>(opts.size()) - 1) {
                select->setHighlightedIndex(hi + 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_UP) {
            if (hi > 0) {
                select->setHighlightedIndex(hi - 1);
                uiDirty_ = true;
            }
            handled = true;
        } else if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
            if (hi >= 0 && hi < static_cast<int>(opts.size())) {
                select->setSelectedIndex(hi);
                activeEl->setAttribute("value", opts[hi].value);
                dom::Event changeEvt("change");
                dispatchEvent(activeEl, changeEvt);
                dispatchInputEvent(activeEl);
            }
            select->setOpen(false);
            uiDirty_ = true;
            handled = true;
        } else if (keycode == SDLK_ESCAPE) {
            select->setOpen(false);
            uiDirty_ = true;
            handled = true;
        }

        if (handled) {
            dom::KeyboardEvent evt("keydown");
            evt.setKey(sdlKeycodeToWebKey(keycode, mod));
            evt.setCode(sdlScancodeToWebCode(scancode));
            evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
            evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
            evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
            evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
            evt.setRepeat(repeat);
            dispatchEvent(activeEl, evt);
            return;
        }
    }

    // Default: dispatch keydown to body
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

void Engine::handleKeyUp(int keycode, int scancode, int mod, bool repeat) {
    if (!document_) return;

    // Dispatch keyup to the focused input if any, otherwise body
    dom::KeyboardEvent evt("keyup");
    evt.setKey(sdlKeycodeToWebKey(keycode, mod));
    evt.setCode(sdlScancodeToWebCode(scancode));
    evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
    evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
    evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
    evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
    evt.setRepeat(repeat);

    auto* activeEl = document_->activeElement();
    bool focusedControl = false;
    if (auto* input = getElInput(activeEl)) focusedControl = input->isFocused();
    if (auto* ta = getElTextarea(activeEl)) focusedControl = focusedControl || ta->isFocused();
    if (auto* sel = getElSelect(activeEl)) focusedControl = focusedControl || sel->isOpen();
    dom::Element* target = focusedControl ? activeEl : document_->body();
    if (target) {
        dispatchEvent(target, evt);
    }
}

// Filter out control characters (tab, etc.) that shouldn't be inserted as text
static bool isControlChar(const std::string& text) {
    if (text.empty()) return true;
    unsigned char c = static_cast<unsigned char>(text[0]);
    // Allow printable ASCII and multi-byte UTF-8 sequences
    if (text.size() == 1 && c < 0x20 && c != '\n') return true; // control chars except newline
    if (text.size() == 1 && c == 0x7f) return true; // DEL
    return false;
}

// Validate text for number input (digits, minus, decimal point, 'e'/'E')
static bool isValidNumberChar(const std::string& text) {
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || c == '-' || c == '.' || c == 'e' || c == 'E' || c == '+'))
            return false;
    }
    return !text.empty();
}

void Engine::handleTextInput(const std::string& text) {
    if (!document_) return;

    // Filter control characters for all inputs
    if (isControlChar(text)) return;

    auto* activeEl = document_->activeElement();

    // Try textarea first (also text-editable)
    auto* textarea = getElTextarea(activeEl);
    if (textarea && textarea->isFocused()) {
        std::string val = activeEl->getAttribute("value");
        int pos = std::clamp(textarea->cursorPos(), 0, static_cast<int>(val.size()));
        val.insert(pos, text);
        textarea->setCursorPos(pos + static_cast<int>(text.size()));
        activeEl->setAttribute("value", val);
        dispatchInputEvent(activeEl);
        return;
    }

    auto* input = getElInput(activeEl);
    if (!input || !input->isFocused() || !input->isTextType()) return;

    // Number type: only allow numeric characters
    if (input->inputType() == layout::ElInput::InputType::Number) {
        if (!isValidNumberChar(text)) return;
    }

    // Insert text at cursor position
    std::string val = activeEl->getAttribute("value");
    int pos = std::clamp(input->cursorPos(), 0, static_cast<int>(val.size()));
    val.insert(pos, text);
    input->setCursorPos(pos + static_cast<int>(text.size()));
    activeEl->setAttribute("value", val);
    dispatchInputEvent(activeEl);
}

// ---------------------------------------------------------------------------
// Tab focus navigation
// ---------------------------------------------------------------------------

void Engine::advanceFocus(bool reverse) {
    if (!document_) return;

    // Build list of focusable elements in DOM order
    std::vector<dom::Element*> focusable;
    auto* body = document_->body();
    if (!body) return;

    // Collect all elements via querySelectorAll for common focusable tags
    auto inputs = body->querySelectorAll("input");
    auto textareas = body->querySelectorAll("textarea");
    auto selects = body->querySelectorAll("select");
    auto buttons = body->querySelectorAll("button");

    // Merge into a single list — we need DOM order, so collect all elements
    // and filter. Use a simple recursive walk.
    std::function<void(dom::Node*)> walk = [&](dom::Node* node) {
        if (!node) return;
        if (node->nodeType() == dom::NodeType::Element) {
            auto* el = static_cast<dom::Element*>(node);
            std::string tag = el->tagName();
            for (auto& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool isFocusable = (tag == "input" || tag == "textarea" || tag == "select" || tag == "button");
            if (isFocusable) {
                // Skip hidden inputs
                auto* inp = getElInput(el);
                if (inp && inp->inputType() == layout::ElInput::InputType::Hidden)
                    isFocusable = false;
                // Skip disabled
                if (el->getAttribute("disabled") == "true" || el->attributes().count("disabled"))
                    isFocusable = false;
            }
            if (isFocusable) focusable.push_back(el);
        }
        for (auto* child : node->childNodes()) walk(child);
    };
    walk(body);

    if (focusable.empty()) return;

    // Find current active element
    auto* activeEl = document_->activeElement();
    int currentIdx = -1;
    for (int i = 0; i < static_cast<int>(focusable.size()); ++i) {
        if (focusable[i] == activeEl) { currentIdx = i; break; }
    }

    // Compute next index
    int nextIdx;
    if (reverse) {
        nextIdx = (currentIdx <= 0) ? static_cast<int>(focusable.size()) - 1 : currentIdx - 1;
    } else {
        nextIdx = (currentIdx < 0 || currentIdx >= static_cast<int>(focusable.size()) - 1) ? 0 : currentIdx + 1;
    }

    auto* nextEl = focusable[nextIdx];

    // Unfocus current
    if (activeEl) {
        auto* prevInput = getElInput(activeEl);
        if (prevInput) prevInput->setFocused(false);
        auto* prevTa = getElTextarea(activeEl);
        if (prevTa) prevTa->setFocused(false);
        auto* prevSel = getElSelect(activeEl);
        if (prevSel) prevSel->setOpen(false);
    }

    // Focus next
    document_->setActiveElement(nextEl);
    auto* newInput = getElInput(nextEl);
    auto* newTa = getElTextarea(nextEl);

    if (newInput) {
        newInput->setFocused(true);
        if (newInput->isTextType()) {
            const char* val = newInput->get_attr("value");
            newInput->setCursorPos(val ? static_cast<int>(strlen(val)) : 0);
            SDL_StartTextInput(window_->getSDLWindow());
        } else {
            SDL_StopTextInput(window_->getSDLWindow());
        }
    } else if (newTa) {
        newTa->setFocused(true);
        const char* val = newTa->get_attr("value");
        newTa->setCursorPos(val ? static_cast<int>(strlen(val)) : 0);
        SDL_StartTextInput(window_->getSDLWindow());
    } else {
        SDL_StopTextInput(window_->getSDLWindow());
    }

    uiDirty_ = true;
}

// ---------------------------------------------------------------------------
// Mouse wheel
// ---------------------------------------------------------------------------

void Engine::handleWheel(float x, float y, float /*dx*/, float dy) {
    if (!document_) return;

    // Check if mouse is over a focused textarea
    auto* activeEl = document_->activeElement();
    auto* textarea = getElTextarea(activeEl);
    if (textarea && textarea->isFocused()) {
        // Scroll by 3 lines per wheel tick
        auto fm = textarea->css().get_font_metrics();
        float lineH = (fm.height > 0) ? static_cast<float>(fm.height) : 16.0f;
        float scroll = textarea->scrollY() - dy * lineH * 3.0f;
        scroll = std::max(scroll, 0.0f);
        textarea->setScrollY(scroll);
        uiDirty_ = true;
        return;
    }

    // Also allow scrolling textarea under mouse cursor (not just active one)
    dom::Element* target = hitTest(x, y);
    auto* hoverTa = getElTextarea(target);
    if (hoverTa) {
        auto fm = hoverTa->css().get_font_metrics();
        float lineH = (fm.height > 0) ? static_cast<float>(fm.height) : 16.0f;
        float scroll = hoverTa->scrollY() - dy * lineH * 3.0f;
        scroll = std::max(scroll, 0.0f);
        hoverTa->setScrollY(scroll);
        uiDirty_ = true;
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


} // namespace bro::engine
