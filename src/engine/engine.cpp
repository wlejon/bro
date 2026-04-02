#include "engine/engine.h"
#include "engine/system_overlay.h"

#include "platform/sdl_window.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/raster_renderer.h"
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
#include "js/window_bindings.h"
#include "js/custom_elements.h"
#include "js/webgl2_bindings.h"
#include "js/image_bindings.h"
#include "js/fetch_bindings.h"
#include "audio/audio_engine.h"
#include "canvas/canvas_scene.h"
#include "canvas/canvas2d.h"
#include "webgl/webgl2_context.h"
#include "webgl/webgl_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/event.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "layout/element_ref_adapter.h"
#include "layout/layout_node_adapter.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/el_svg.h"
#include "engine/default_styles.h"
#include "util/log.h"
#include "util/time.h"

#include <stb_image_write.h>

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <glad/gl.h>
#include <cstdio>
#include <stdexcept>
#include <functional>
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
// Form control helpers (used by both draw loop and event handlers)
// ---------------------------------------------------------------------------

static layout::ElInput* getElInput(dom::Element* el);
static layout::ElTextarea* getElTextarea(dom::Element* el);
static layout::ElSelect* getElSelect(dom::Element* el);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Engine::Engine(const EngineConfig& config)
    : displayMode_(config.displayMode)
    , viewportWidth_(config.width)
    , viewportHeight_(config.height) {

    // === Mode-specific initialization ===

    // hasGL: true when we have a GPU context (windowed, or headless with GPU)
    const bool hasGL = (displayMode_ == DisplayMode::Windowed) || config.useGPU;

    if (hasGL) {
        // Create window (hidden for headless, visible for windowed)
        bool hidden = (displayMode_ == DisplayMode::Headless);
        window_ = std::make_unique<platform::Window>("Bro", static_cast<uint32_t>(config.width),
                                                      static_cast<uint32_t>(config.height), hidden);

        // GL context (shader programs + helpers)
        gl_ = std::make_unique<render::GLContext>(*window_);

        // Renderer (Skia raster + OpenGL display)
        renderer_ = render::createRenderer(gl_.get());
        if (!renderer_) {
            throw std::runtime_error("Failed to create renderer");
        }
    } else {
        // No GPU: CPU-only Skia renderer, no window/GL
        renderer_ = std::make_unique<render::RasterRenderer>();
    }

    if (displayMode_ == DisplayMode::Headless) {
        virtualTime_ = util::currentTimeMs();
    }

    // === Common initialization ===

    // 4. JS runtime + bindings
    jsRuntime_ = std::make_unique<js::Runtime>();
    jsRuntime_->setModuleLoader();
    js::Console::install(jsRuntime_->getContext());
    timers_ = std::make_unique<js::Timers>();
    js::Timers::install(jsRuntime_->getContext(), timers_.get());

    // 4b. Audio engine + bindings
    if (displayMode_ == DisplayMode::Windowed) {
        audioEngine_ = std::make_unique<audio::AudioEngine>();
        audioEngine_->init();
        js::AudioBindings::install(jsRuntime_->getContext(), audioEngine_.get());
    } else {
        js::AudioBindings::install(jsRuntime_->getContext(), nullptr);
    }

    // 5. Layout helpers
    drawTraversal_ = std::make_unique<layout::DrawTraversal>(renderer_.get(), &fontManager_);
    textMetrics_ = std::make_unique<layout::SkiaTextMetrics>(renderer_.get(), &fontManager_);

    // 6. Load the application
    manifest_ = AppLoader::loadApp(config.appDir);
    std::string html = AppLoader::loadFile(manifest_.htmlPath);
    if (html.empty()) {
        throw std::runtime_error("Failed to load index.html from " + config.appDir);
    }

    // 4c. localStorage (persisted) + sessionStorage (in-memory)
    std::string storagePath = manifest_.basePath + "/.storage.json";
    js::StorageBindings::install(jsRuntime_->getContext(), storagePath);
    js::StorageBindings::installSessionStorage(jsRuntime_->getContext());

    // Set the base path so relative paths work.
    drawTraversal_->setBasePath(manifest_.basePath);
    drawTraversal_->setViewport(viewportWidth_, viewportHeight_);

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

    // 7. Extract <template> blocks before parsing (gumbo discards them)
    std::vector<dom::Document::TemplateBlock> templateBlocks;
    html = dom::Document::extractTemplates(html, templateBlocks);

    // 8. Parse HTML and build bro::dom tree
    document_ = std::make_unique<dom::Document>();
    document_->setBasePath(manifest_.basePath);
    document_->parse(html, userStyles);

    // 8a. Inject extracted templates back into the DOM tree
    if (!templateBlocks.empty())
        document_->injectTemplates(templateBlocks);

    // 8b. Set window title from <title> element (windowed only)
    if (window_) {
        std::string docTitle = document_->title();
        if (!docTitle.empty()) {
            window_->setTitle(docTitle);
        }
    }

    // 9. Set up window/navigator/location/history BEFORE DOM bindings
    js::installWindowBindings(jsRuntime_->getContext(), viewportWidth_, viewportHeight_);

    // 9a. Install DOM JS bindings (after window so polyfills work)
    js::DomBindings::install(jsRuntime_->getContext(), document_.get());

    // 9b. Install custom elements (after DOM bindings — needs element class ID)
    js::installCustomElements(jsRuntime_->getContext(),
                              js::DomBindings::elementClassId(), document_.get());

    // 9c. Install Canvas 2D bindings + getContext factory
    js::CanvasBindings::install(jsRuntime_->getContext());
    js::ImageBindings::install(jsRuntime_->getContext(), manifest_.basePath);
    js::FetchBindings::install(jsRuntime_->getContext(), manifest_.basePath);

    if (gl_) {
        // GPU path: WebGL2 + full canvas factory (windowed or GPU headless)
        js::WebGL2Bindings::install(jsRuntime_->getContext());
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
    } else {
        // CPU path: 2D canvas only, no WebGL
        js::DomBindings::setGetContextFactory(jsRuntime_->getContext(),
            [this](JSContext* ctx, dom::Element*, const std::string&) -> JSValue {
                headlessCanvasScene_ = std::make_unique<canvas::CanvasScene>(renderer_.get());
                headlessCanvasScenePtr_ = headlessCanvasScene_.get();
                headlessCanvasScene_->onInit(nullptr, viewportWidth_, viewportHeight_);
                return js::CanvasBindings::wrapContext2D(ctx, headlessCanvasScenePtr_);
            });
    }

    // 10. Load and execute scripts
    for (auto& scriptPath : manifest_.scriptPaths) {
        std::string code = AppLoader::loadFile(scriptPath);
        if (!code.empty()) {
            if (!jsRuntime_->eval(code, scriptPath)) {
                LOG_ERROR("Failed to execute script: %s", scriptPath.c_str());
            }
        }
    }

    // === Mode-specific post-init ===

    if (displayMode_ == DisplayMode::Windowed) {
        // 11. Event loop
        eventLoop_ = std::make_unique<platform::EventLoop>();

        // 13. Create UI overlay quad VAO/VBO
        glGenVertexArrays(1, &uiQuadVAO_);
        glGenBuffers(1, &uiQuadVBO_);
    }

    // 12. System overlay (loads panels from system/ sibling directory)
    //     Shares the JS runtime — each panel gets its own JSContext.
    systemOverlay_ = std::make_unique<SystemOverlay>(jsRuntime_.get(), gl_.get(),
                                                      viewportWidth_, viewportHeight_);
    systemOverlay_->loadPanels("system");

    // Headless: do initial layout + flush
    if (displayMode_ == DisplayMode::Headless) {
        ensureReplacedElements(document_->documentElement());
        document_->resolveStyles();
        document_->performLayout(static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), *textMetrics_);
        flush();
    }
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
    headlessCanvasScene_.reset();

    // 1. Clear timers (they hold JS callbacks)
    if (timers_ && jsRuntime_) {
        timers_->clearAll(jsRuntime_->getContext());
    }

    // 2. Clear JS bindings
    if (jsRuntime_) {
        JSContext* ctx = jsRuntime_->getContext();
        js::AudioBindings::cleanup(ctx);
        js::StorageBindings::cleanup(ctx);
        if (gl_) {
            js::WebGL2Bindings::cleanup(ctx);
        }
        js::cleanupCustomElements(ctx);

        // Clean up global properties (prevents leaked references)
        JSValue global = JS_GetGlobalObject(ctx);
        JS_DeleteProperty(ctx, global, JS_NewAtom(ctx, "__bro_elem_map"), 0);
        JS_DeleteProperty(ctx, global, JS_NewAtom(ctx, "document"), 0);
        JS_DeleteProperty(ctx, global, JS_NewAtom(ctx, "console"), 0);
        JS_FreeValue(ctx, global);

        js::DomBindings::cleanup(ctx);
        jsRuntime_->executePendingJobs();
        JS_RunGC(jsRuntime_->getRuntime());
    }

    // 3. GL cleanup (windowed only)
    if (uiQuadVBO_) { glDeleteBuffers(1, &uiQuadVBO_); uiQuadVBO_ = 0; }
    if (uiQuadVAO_) { glDeleteVertexArrays(1, &uiQuadVAO_); uiQuadVAO_ = 0; }
    audioEngine_.reset();

    // 4. Release layout resources before document
    drawTraversal_.reset();

    // Clean up per-runtime DomBindings state before the runtime is freed.
    if (jsRuntime_) {
        js::DomBindings::cleanupRuntime(jsRuntime_->getRuntime());
    }
    // Destroy JS runtime BEFORE document — JS_FreeRuntime() runs GC finalizers
    // that dereference Element pointers, so elements must still be alive.
    jsRuntime_.reset();
    document_.reset();
    timers_.reset();
    renderer_.reset();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void Engine::run() {
    // Headless mode: initial layout is done in constructor, return immediately.
    // The HeadlessController drives subsequent frames via advanceTime/flush.
    if (displayMode_ == DisplayMode::Headless) {
        return;
    }

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

    // Initial layout
    if (document_) {
        ensureReplacedElements(document_->documentElement());
        document_->resolveStyles();
        document_->performLayout(static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), *textMetrics_);
        if (document_->documentElement()) {
            auto& box = document_->documentElement()->layoutBox();
            documentHeight_ = box.marginBox().height;
        }
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
        if (document_ && (document_->isDirty() || !hasRenderedOnce_) && uiFrameDue) {
            ensureReplacedElements(document_->documentElement());
            document_->resolveStyles();
            document_->clearStructureDirty();
            document_->performLayout(static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), *textMetrics_);
            document_->clearDirty();
            // Update document height for scroll clamping
            if (document_->documentElement()) {
                auto& box = document_->documentElement()->layoutBox();
                documentHeight_ = box.marginBox().height;
            }

            // Process auto-scroll-to-bottom for overflow elements.
            if (document_) {
                std::function<void(dom::Node*)> processScrollToBottom;
                processScrollToBottom = [&](dom::Node* node) {
                    if (!node || node->nodeType() != dom::NodeType::Element) return;
                    auto* elem = static_cast<dom::Element*>(node);
                    if (elem->needsScrollToBottom()) {
                        elem->setScrollToBottom(false);
                        auto& style = elem->computedStyle();
                        auto ovIt = style.find("overflow");
                        if (ovIt != style.end() && ovIt->second != "visible") {
                            // Scroll to bottom by setting a large scroll offset
                            float currentScroll = elem->scrollTopValue();
                            elem->setScrollTopValue(currentScroll + 100000.0f);
                        }
                    }
                    for (auto* child : elem->childNodes())
                        processScrollToBottom(child);
                };
                processScrollToBottom(document_->documentElement());
            }

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

            double tDraw0 = util::currentTimeMs();
            if (document_ && document_->documentElement()) {
                drawTraversal_->draw(document_->documentElement(), 0, -scrollY_,
                                     viewportWidth_, viewportHeight_);
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

            // Draw scrollbar if content overflows viewport
            if (documentHeight_ > static_cast<float>(viewportHeight_)) {
                float vh = static_cast<float>(viewportHeight_);
                float trackX = static_cast<float>(viewportWidth_ - kScrollbarWidth - kScrollbarMargin);
                float trackH = vh;
                float thumbRatio = vh / documentHeight_;
                float thumbH = std::max(thumbRatio * trackH, 24.0f);
                float scrollRange = documentHeight_ - vh;
                float thumbY = (scrollRange > 0.0f) ? (scrollY_ / scrollRange) * (trackH - thumbH) : 0.0f;

                // Track background
                render::Color trackColor{255, 255, 255, 32};
                renderer_->fillRect(trackX, 0.0f,
                                    static_cast<float>(kScrollbarWidth), trackH,
                                    trackColor);
                // Thumb
                render::Color thumbColor{255, 255, 255, 128};
                renderer_->fillRect(trackX, thumbY,
                                    static_cast<float>(kScrollbarWidth), thumbH,
                                    thumbColor);
            }

            // Draw scrollbars for overflow elements
            if (document_) {
                int scrollYi = static_cast<int>(scrollY_);
                std::function<void(dom::Node*)> drawElemScrollbars;
                drawElemScrollbars = [&](dom::Node* node) {
                    if (!node || node->nodeType() != dom::NodeType::Element) return;
                    auto* elem = static_cast<dom::Element*>(node);
                    auto& style = elem->computedStyle();
                    auto ovIt = style.find("overflow");
                    if (ovIt != style.end() && ovIt->second != "visible") {
                        float scrollTop = elem->scrollTopValue();
                        if (scrollTop != 0) {
                            auto& lbox = elem->layoutBox();
                            auto mb = lbox.marginBox();
                            float ex = mb.x;
                            float ey = mb.y - scrollYi;
                            float ew = mb.width;
                            float eh = mb.height;
                            float contentH = lbox.contentRect.height;
                            float viewH = eh;
                            if (contentH > viewH) {
                                float sbW = 5.0f;
                                float trackX = ex + ew - sbW - 1.0f;
                                float scrollRange = contentH - viewH;
                                float thumbRatio = viewH / contentH;
                                float thumbH = std::max(thumbRatio * eh, 16.0f);
                                float thumbY = ey + (scrollRange > 0 ?
                                    (scrollTop / scrollRange) * (eh - thumbH) : 0);

                                render::Color tc{255, 255, 255, 20};
                                renderer_->fillRect(trackX, ey, sbW, eh, tc);
                                render::Color th{255, 255, 255, 100};
                                renderer_->fillRect(trackX, thumbY, sbW, thumbH, th);
                            }
                        }
                    }
                    for (auto* child : elem->childNodes())
                        drawElemScrollbars(child);
                };
                drawElemScrollbars(document_->documentElement());
            }

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

        // Restore WebGL shadow state so apps with internal caches (three.js)
        // see the same GL state they left on the previous frame.
        // Uses shadow-tracked values — no expensive glGet* queries.
        {
            auto* wgl = dynamic_cast<webgl::WebGLScene*>(sceneLayer_.get());
            if (wgl && wgl->webglContext()) {
                wgl->webglContext()->restoreState();
            }
        }

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
    drawTraversal_->setViewport(w, h);
    if (sceneLayer_) {
        sceneLayer_->onResize(w, h);
    }
    if (systemOverlay_) {
        systemOverlay_->onResize(w, h);
    }
    if (document_) {
        document_->resolveStyles();
        document_->performLayout(static_cast<float>(w), static_cast<float>(h), *textMetrics_);
        if (document_->documentElement()) {
            auto& box = document_->documentElement()->layoutBox();
            documentHeight_ = box.marginBox().height;
        }
        // Clamp scroll after resize
        float maxScroll = std::max(0.0f, documentHeight_ - static_cast<float>(h));
        scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll);
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
    return el ? el->inputControl() : nullptr;
}
static layout::ElTextarea* getElTextarea(dom::Element* el) {
    return el ? el->textareaControl() : nullptr;
}
static layout::ElSelect* getElSelect(dom::Element* el) {
    return el ? el->selectControl() : nullptr;
}

// Returns true if the element is a focusable text-editing control (input or textarea)
static bool isTextEditable(dom::Element* el) {
    return getElInput(el) || getElTextarea(el);
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void Engine::handleMouseDown(float x, float y, int button) {
    // x, y = raw mouse position (screen space).
    // docX, docY = document space (for hit testing into the scrolled document).
    // IMPORTANT: overlay positions (lastDrawPos, color picker, select dropdown)
    // are in screen space — always use x/y when comparing, never docX/docY.
    float docX = x, docY = y + scrollY_;
    uiDirty_ = true;

    if (document_) {
        dom::MouseEvent evt("mousedown");
        evt.setClientX(static_cast<double>(x));
        evt.setClientY(static_cast<double>(y));
        evt.setButton(button);
        dom::Element* target = hitTest(docX, docY);
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
                    if (inPicker) {
                        // Click inside picker — select the color
                        float cellW = (pw - 4) / 10.0f;
                        float cellH = (ph - 4) / 8.0f;
                        int col = static_cast<int>((x - px - 2) / cellW);
                        int row = static_cast<int>((y - py - 2) / cellH);
                        col = std::clamp(col, 0, 9);
                        row = std::clamp(row, 0, 7);

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
                        prevActive->setAttribute("value", hex);
                        dom::Event changeEvt("change");
                        dispatchEvent(prevActive, changeEvt);
                        dispatchInputEvent(prevActive);
                        prevInput->setPickerOpen(false);
                        uiDirty_ = true;
                        return; // consumed the click
                    } else if (inSwatch) {
                        prevInput->setPickerOpen(false);
                        uiDirty_ = true;
                        return; // consumed — don't fall through
                    } else {
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
                float lineH = prevSelect->dropdownLineHeight();
                float dropY = dp.y + dp.h;
                float dropH = lineH * static_cast<float>(opts.size()) + 2.0f;
                bool inDropdown = (x >= dp.x && x < dp.x + dp.w &&
                                   y >= dropY && y < dropY + dropH);
                if (inDropdown) {
                    // Select the clicked option
                    int idx = static_cast<int>((y - dropY - 1.0f) / lineH);
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
                auto itype = newInput->inputType(target);

                if (itype == layout::ElInput::InputType::Checkbox) {
                    // Toggle checked state
                    if (target->hasAttribute("checked")) {
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
                    std::string nameStr = target->getAttribute("name");
                    const char* name = nameStr.empty() ? nullptr : nameStr.c_str();
                    if (name && *name && document_) {
                        auto* body = document_->body();
                        if (body) {
                            auto radios = body->querySelectorAll("input[type=\"radio\"]");
                            for (auto* el : radios) {
                                if (el == target) continue;
                                auto* otherInput = getElInput(el);
                                if (otherInput) {
                                    std::string otherNameStr = el->getAttribute("name");
                                    if (!otherNameStr.empty() && otherNameStr == nameStr) {
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
                } else if (newInput->isTextType(target)) {
                    // Check for number spin button click
                    if (newInput->inputType(target) == layout::ElInput::InputType::Number) {
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
                            std::string minAttr = target->getAttribute("min");
                            std::string maxAttr = target->getAttribute("max");
                            if (!minAttr.empty()) v = std::max(v, newInput->rangeMin());
                            if (!maxAttr.empty()) v = std::min(v, newInput->rangeMax());
                            char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                            target->setAttribute("value", buf);
                            newInput->setCursorPos(static_cast<int>(strlen(buf)));
                            dispatchInputEvent(target);
                        }
                    }
                    std::string valStr = target->getAttribute("value");
                    newInput->setCursorPos(static_cast<int>(valStr.size()));
                    SDL_StartTextInput(window_->getSDLWindow());
                    uiDirty_ = true;
                } else {
                    // Button types — no text input
                    SDL_StopTextInput(window_->getSDLWindow());
                    uiDirty_ = true;
                }
            } else if (newTextarea) {
                newTextarea->setFocused(true);
                std::string taValStr = target->getAttribute("value");
                newTextarea->setCursorPos(static_cast<int>(taValStr.size()));
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
    // x, y = screen space. docX, docY = document space (see handleMouseDown).
    float docX = x, docY = y + scrollY_;
    uiDirty_ = true;

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
        dom::Element* target = hitTest(docX, docY);
        if (target) {
            dispatchEvent(target, clickEvt);
            jsRuntime_->executePendingJobs();
        }
    }
}

void Engine::handleMouseMove(float x, float y) {
    // x, y = screen space. docX, docY = document space (see handleMouseDown).
    float docX = x, docY = y + scrollY_;

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
            float lineH = select->dropdownLineHeight();
            float dropY = dp.y + dp.h;
            float dropH = lineH * static_cast<float>(opts.size()) + 2.0f;
            if (x >= dp.x && x < dp.x + dp.w && y >= dropY && y < dropY + dropH) {
                int idx = static_cast<int>((y - dropY - 1.0f) / lineH);
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
        auto itype = input->inputType(activeEl);
        if ((itype == layout::ElInput::InputType::Checkbox || itype == layout::ElInput::InputType::Radio)
            && keycode == SDLK_SPACE) {
            if (itype == layout::ElInput::InputType::Checkbox) {
                if (activeEl->hasAttribute("checked"))
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
        if (!input->isTextType(activeEl)) {
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

    if (input && input->isFocused() && input->isTextType(activeEl)) {
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
        } else if (input->inputType(activeEl) == layout::ElInput::InputType::Number &&
                   (keycode == SDLK_UP || keycode == SDLK_DOWN)) {
            // Increment/decrement number value
            float v = 0;
            if (!val.empty()) v = static_cast<float>(atof(val.c_str()));
            float step = input->rangeStep();
            v += (keycode == SDLK_UP) ? step : -step;
            // Clamp to min/max if specified
            float mn = input->rangeMin(), mx = input->rangeMax();
            std::string minAttrStr = activeEl->getAttribute("min");
            std::string maxAttrStr = activeEl->getAttribute("max");
            if (!minAttrStr.empty()) v = std::max(v, mn);
            if (!maxAttrStr.empty()) v = std::min(v, mx);
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
    if (!input || !input->isFocused() || !input->isTextType(activeEl)) return;

    // Number type: only allow numeric characters
    if (input->inputType(activeEl) == layout::ElInput::InputType::Number) {
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
                if (inp && inp->inputType(el) == layout::ElInput::InputType::Hidden)
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
        if (newInput->isTextType(nextEl)) {
            std::string v = nextEl->getAttribute("value");
            newInput->setCursorPos(static_cast<int>(v.size()));
            SDL_StartTextInput(window_->getSDLWindow());
        } else {
            SDL_StopTextInput(window_->getSDLWindow());
        }
    } else if (newTa) {
        newTa->setFocused(true);
        std::string v = nextEl->getAttribute("value");
        newTa->setCursorPos(static_cast<int>(v.size()));
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
        float lineH = 16.0f;
        float scroll = textarea->scrollY() - dy * lineH * 3.0f;
        scroll = std::max(scroll, 0.0f);
        textarea->setScrollY(scroll);
        uiDirty_ = true;
        return;
    }

    // Also allow scrolling textarea under mouse cursor (not just active one)
    float docX = x, docY = y + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    auto* hoverTa = getElTextarea(target);
    if (hoverTa) {
        float lineH = 16.0f;
        float scroll = hoverTa->scrollY() - dy * lineH * 3.0f;
        scroll = std::max(scroll, 0.0f);
        hoverTa->setScrollY(scroll);
        uiDirty_ = true;
        return;
    }

    // Check if target or an ancestor is a scrollable overflow element
    {
        auto* el = target;
        while (el) {
            auto& style = el->computedStyle();
            auto ovIt = style.find("overflow");
            if (ovIt != style.end() && ovIt->second != "visible") {
                float scrollPx = -dy * kScrollSpeed;
                float currentScroll = el->scrollTopValue();
                el->setScrollTopValue(currentScroll + scrollPx);
                uiDirty_ = true;
                document_->markDirty();
                return;
            }
            el = el->parentElement();
        }
    }

    // Viewport scrolling
    float maxScroll = std::max(0.0f, documentHeight_ - static_cast<float>(viewportHeight_));
    scrollY_ = std::clamp(scrollY_ - dy * kScrollSpeed, 0.0f, maxScroll);
    uiDirty_ = true;
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

dom::Element* Engine::hitTest(float x, float y) {
    // x, y are already in document space (scroll-adjusted by callers)
    if (!document_ || !document_->documentElement())
        return document_ ? document_->body() : nullptr;

    auto layoutTree = layout::LayoutNodeAdapter::buildTree(document_->documentElement());
    auto* hit = htmlayout::layout::hitTest(layoutTree.get(), x, y);
    if (!hit) return document_->body();

    auto* adapter = static_cast<layout::LayoutNodeAdapter*>(hit);
    if (adapter->element()) return adapter->element();
    return document_->body();
}

// ---------------------------------------------------------------------------
// Event dispatch to JS (delegates to shared implementation)
// ---------------------------------------------------------------------------

void Engine::dispatchEvent(dom::Element* target, dom::Event& event) {
    if (!target || !jsRuntime_) return;
    js::dispatchDomEvent(jsRuntime_->getContext(), target, event);
}


// ---------------------------------------------------------------------------
// Replaced element control initialization
// ---------------------------------------------------------------------------

void Engine::ensureReplacedElements(dom::Element* elem) {
    if (!elem) return;

    const auto& tag = elem->tagName();

    if (tag == "INPUT" && !elem->inputControl()) {
        auto ctrl = std::make_unique<layout::ElInput>(renderer_.get());
        ctrl->setElement(elem);
        elem->setInputControl(std::move(ctrl));
    } else if (tag == "TEXTAREA" && !elem->textareaControl()) {
        auto ctrl = std::make_unique<layout::ElTextarea>(renderer_.get());
        ctrl->setElement(elem);
        elem->setTextareaControl(std::move(ctrl));
    } else if (tag == "SELECT" && !elem->selectControl()) {
        auto ctrl = std::make_unique<layout::ElSelect>(renderer_.get());
        ctrl->setElement(elem);
        elem->setSelectControl(std::move(ctrl));
    } else if ((tag == "SVG" || tag == "svg") && !elem->svgControl()) {
        auto ctrl = std::make_unique<layout::ElSvg>(renderer_.get());
        ctrl->setElement(elem);
        ctrl->parseAttributes();
        elem->setSvgControl(std::move(ctrl));
    }

    // Recurse into children
    for (auto* child : elem->childNodes()) {
        if (child->nodeType() == dom::NodeType::Element) {
            ensureReplacedElements(static_cast<dom::Element*>(child));
        }
    }

    // Recurse into shadow DOM
    if (elem->hasShadow()) {
        auto* sr = elem->shadowRoot();
        for (auto* child : sr->childNodes()) {
            if (child->nodeType() == dom::NodeType::Element) {
                ensureReplacedElements(static_cast<dom::Element*>(child));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Headless API
// ---------------------------------------------------------------------------

void Engine::flush() {
    jsRuntime_->executePendingJobs();
    if (document_ && document_->isDirty()) {
        ensureReplacedElements(document_->documentElement());
        document_->resolveStyles();
        document_->clearStructureDirty();
        document_->performLayout(static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), *textMetrics_);
        document_->clearDirty();
    }
}

void Engine::advanceTime(double ms) {
    if (displayMode_ != DisplayMode::Headless) return;

    // Bind WebGL canvas FBO so rAF draw commands target it correctly
    auto* webglScene = dynamic_cast<webgl::WebGLScene*>(sceneLayer_.get());

    double remaining = ms;
    while (remaining > 0) {
        double step = std::min(remaining, 16.0);
        virtualTime_ += step;
        remaining -= step;
        timers_->tick(virtualTime_);

        if (webglScene && webglScene->webglContext())
            webglScene->webglContext()->bindCanvasFBO();
        timers_->fireAnimationFrames(virtualTime_);
        jsRuntime_->executePendingJobs();
        if (webglScene && webglScene->webglContext())
            webglScene->webglContext()->unbindCanvasFBO();

        flush();

        // Periodic GC + orphan sweep (every ~1s of virtual time)
        if (virtualTime_ - lastGCMs_ >= kGCIntervalMs) {
            js::DomBindings::sweepOrphanedWrappers(jsRuntime_->getContext());
            JS_RunGC(jsRuntime_->getRuntime());
            lastGCMs_ = virtualTime_;
        }
    }
}

std::string Engine::eval(const std::string& code) {
    JSContext* ctx = jsRuntime_->getContext();
    JSValue result = JS_Eval(ctx, code.c_str(), code.size(), "<eval>",
                              JS_EVAL_TYPE_GLOBAL);
    std::string output;
    if (JS_IsException(result)) {
        js::Runtime::checkException(ctx, result);
        output = "[exception]";
    } else {
        const char* str = JS_ToCString(ctx, result);
        if (str) {
            output = str;
            JS_FreeCString(ctx, str);
        } else {
            output = "[null]";
        }
    }
    JS_FreeValue(ctx, result);
    flush();
    return output;
}

bool Engine::screenshot(const std::string& path) {
    if (!document_) return false;

    // Bind WebGL canvas FBO before firing rAF (so GL draw commands target the canvas)
    auto* webglScene = dynamic_cast<webgl::WebGLScene*>(sceneLayer_.get());
    if (webglScene && webglScene->webglContext()) {
        webglScene->webglContext()->bindCanvasFBO();
    }

    // Fire any pending rAF callbacks so canvas commands are up to date
    timers_->fireAnimationFrames(virtualTime_);
    jsRuntime_->executePendingJobs();

    // Unbind WebGL canvas FBO
    if (webglScene && webglScene->webglContext()) {
        webglScene->webglContext()->unbindCanvasFBO();
    }

    // GPU compositing path: replicate the windowed render pass to an offscreen FBO,
    // then read back pixels. This captures scene layers (WebGL, Canvas2D) + UI overlay.
    if (gl_ && sceneLayer_) {
        auto* skia = static_cast<render::SkiaRenderer*>(renderer_.get());
        int w = viewportWidth_, h = viewportHeight_;

        // 1. Rasterize HTML/CSS UI to Skia surface
        renderer_->beginFrame(w, h);
        if (document_->documentElement()) {
            drawTraversal_->draw(document_->documentElement(), 0, 0, w, h);
        }
        renderer_->endFrame();
        skia->uploadToGPU();

        // 2. Prepare Canvas2D scene if applicable
        auto* canvasScene = dynamic_cast<canvas::CanvasScene*>(sceneLayer_.get());
        if (canvasScene) {
            canvasScene->prepareFrame(gl_.get(), w, h);
        }

        // 3. Bind WebGL canvas FBO before rAF (same as windowed loop)
        auto* webglScene = dynamic_cast<webgl::WebGLScene*>(sceneLayer_.get());
        if (webglScene && webglScene->webglContext()) {
            // WebGL content was already rendered during rAF above — no need to re-render
        }

        // 4. Create temporary compositing FBO
        GLuint compositeFBO = 0, compositeTex = 0;
        glGenFramebuffers(1, &compositeFBO);
        compositeTex = gl_->createTexture2D(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, compositeTex, 0);

        // 5. Clear and render scene layer
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        sceneLayer_->onRender(gl_.get(), w, h, 0.0);

        // 6. Composite UI overlay (premultiplied alpha)
        GLuint uiTex = skia->getUITexture();
        if (uiTex) {
            float fw = (float)w, fh = (float)h;
            render::TextureVertex quad[6] = {
                {0, 0, 0, 0}, {fw, 0, 1, 0}, {fw, fh, 1, 1},
                {0, 0, 0, 0}, {fw, fh, 1, 1}, {0, fh, 0, 1},
            };

            GLuint quadVBO = 0;
            glGenBuffers(1, &quadVBO);
            glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

            GLuint quadVAO = 0;
            glGenVertexArrays(1, &quadVAO);
            glBindVertexArray(quadVAO);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex),
                                  (void*)offsetof(render::TextureVertex, u));

            glUseProgram(gl_->textureProgram());
            float viewport[2] = {fw, fh};
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

            glDeleteBuffers(1, &quadVBO);
            glDeleteVertexArrays(1, &quadVAO);
        }

        // 7. Read back pixels
        std::vector<uint8_t> pixels(w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        // 8. Cleanup compositing FBO + restore WebGL state
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &compositeFBO);
        gl_->deleteTexture(compositeTex);

        if (webglScene && webglScene->webglContext()) {
            webglScene->webglContext()->restoreState();
        }

        // 9. Flip vertically (OpenGL reads bottom-to-top, PNG is top-to-bottom)
        int rowBytes = w * 4;
        std::vector<uint8_t> row(rowBytes);
        for (int y = 0; y < h / 2; ++y) {
            uint8_t* top = pixels.data() + y * rowBytes;
            uint8_t* bot = pixels.data() + (h - 1 - y) * rowBytes;
            memcpy(row.data(), top, rowBytes);
            memcpy(top, bot, rowBytes);
            memcpy(bot, row.data(), rowBytes);
        }

        // 10. Save as PNG
        return stbi_write_png(path.c_str(), w, h, 4, pixels.data(), rowBytes) != 0;
    }

    // CPU path: render to Skia raster surface and save
    renderer_->beginFrame(viewportWidth_, viewportHeight_);
    renderer_->clear({0, 0, 0, 255});

    // Render canvas scene first (behind HTML) — CPU software replay
    if (headlessCanvasScenePtr_) {
        auto& cmds = headlessCanvasScenePtr_->canvas().commands();
        uint8_t fillR = 0, fillG = 0, fillB = 0, fillA = 255;
        float globalAlpha = 1.0f;

        for (auto& cmd : cmds) {
            using CT = canvas::CmdType;
            switch (cmd.type) {
            case CT::SetFillStyle:
                fillR = cmd.r; fillG = cmd.g; fillB = cmd.b; fillA = cmd.a;
                break;
            case CT::SetGlobalAlpha:
                globalAlpha = cmd.f;
                break;
            case CT::FillRect: {
                uint8_t a = static_cast<uint8_t>(fillA * globalAlpha);
                renderer_->fillRect(cmd.x, cmd.y, cmd.w, cmd.h,
                                    {fillR, fillG, fillB, a});
                break;
            }
            case CT::ClearRect:
                renderer_->fillRect(cmd.x, cmd.y, cmd.w, cmd.h, {0, 0, 0, 255});
                break;
            default: break;
            }
        }
    }

    // Render HTML/CSS overlay on top
    drawTraversal_->draw(document_->documentElement(), 0, 0, viewportWidth_, viewportHeight_);

    // Render system overlay on top of everything
    if (systemOverlay_ && systemOverlay_->isVisible()) {
        systemOverlay_->tick(virtualTime_);
        systemOverlay_->render(viewportWidth_, viewportHeight_);

        auto* sysRenderer = systemOverlay_->getRenderer();
        if (sysRenderer && sysRenderer->surface()) {
            auto* appCanvas = renderer_->getCanvas();
            if (appCanvas) {
                sk_sp<SkImage> sysImage = sysRenderer->surface()->makeImageSnapshot();
                if (sysImage) {
                    SkPaint paint;
                    paint.setBlendMode(SkBlendMode::kSrcOver);
                    appCanvas->drawImage(sysImage, 0, 0, SkSamplingOptions(), &paint);
                }
            }
        }
    }

    renderer_->endFrame();

    return renderer_->saveScreenshot(path);
}

std::vector<uint8_t> Engine::capturePixels() {
    if (!document_) return {};

    // Bind WebGL canvas FBO before firing rAF
    auto* webglScene = dynamic_cast<webgl::WebGLScene*>(sceneLayer_.get());
    if (webglScene && webglScene->webglContext())
        webglScene->webglContext()->bindCanvasFBO();

    timers_->fireAnimationFrames(virtualTime_);
    jsRuntime_->executePendingJobs();

    if (webglScene && webglScene->webglContext())
        webglScene->webglContext()->unbindCanvasFBO();

    // GPU compositing path
    if (gl_ && sceneLayer_) {
        auto* skia = static_cast<render::SkiaRenderer*>(renderer_.get());
        int w = viewportWidth_, h = viewportHeight_;

        renderer_->beginFrame(w, h);
        if (document_->documentElement())
            drawTraversal_->draw(document_->documentElement(), 0, 0, w, h);
        renderer_->endFrame();
        skia->uploadToGPU();

        auto* canvasScene = dynamic_cast<canvas::CanvasScene*>(sceneLayer_.get());
        if (canvasScene) canvasScene->prepareFrame(gl_.get(), w, h);

        // Create temporary compositing FBO
        GLuint compositeFBO = 0, compositeTex = 0;
        glGenFramebuffers(1, &compositeFBO);
        compositeTex = gl_->createTexture2D(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, compositeTex, 0);

        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        sceneLayer_->onRender(gl_.get(), w, h, 0.0);

        // Composite UI overlay
        GLuint uiTex = skia->getUITexture();
        if (uiTex) {
            float fw = (float)w, fh = (float)h;
            render::TextureVertex quad[6] = {
                {0, 0, 0, 0}, {fw, 0, 1, 0}, {fw, fh, 1, 1},
                {0, 0, 0, 0}, {fw, fh, 1, 1}, {0, fh, 0, 1},
            };

            GLuint quadVBO = 0;
            glGenBuffers(1, &quadVBO);
            glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

            GLuint quadVAO = 0;
            glGenVertexArrays(1, &quadVAO);
            glBindVertexArray(quadVAO);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex),
                                  (void*)offsetof(render::TextureVertex, u));

            glUseProgram(gl_->textureProgram());
            float viewport[2] = {fw, fh};
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

            glDeleteBuffers(1, &quadVBO);
            glDeleteVertexArrays(1, &quadVAO);
        }

        // Read back pixels
        std::vector<uint8_t> pixels(w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &compositeFBO);
        gl_->deleteTexture(compositeTex);

        if (webglScene && webglScene->webglContext())
            webglScene->webglContext()->restoreState();

        // Flip vertically (OpenGL reads bottom-to-top)
        int rowBytes = w * 4;
        std::vector<uint8_t> row(rowBytes);
        for (int y = 0; y < h / 2; ++y) {
            uint8_t* top = pixels.data() + y * rowBytes;
            uint8_t* bot = pixels.data() + (h - 1 - y) * rowBytes;
            memcpy(row.data(), top, rowBytes);
            memcpy(top, bot, rowBytes);
            memcpy(bot, row.data(), rowBytes);
        }

        return pixels;
    }

    // CPU path
    renderer_->beginFrame(viewportWidth_, viewportHeight_);
    renderer_->clear({0, 0, 0, 255});

    if (headlessCanvasScenePtr_) {
        auto& cmds = headlessCanvasScenePtr_->canvas().commands();
        uint8_t fillR = 0, fillG = 0, fillB = 0, fillA = 255;
        float globalAlpha = 1.0f;
        for (auto& cmd : cmds) {
            using CT = canvas::CmdType;
            switch (cmd.type) {
            case CT::SetFillStyle:
                fillR = cmd.r; fillG = cmd.g; fillB = cmd.b; fillA = cmd.a; break;
            case CT::SetGlobalAlpha:
                globalAlpha = cmd.f; break;
            case CT::FillRect: {
                uint8_t a = static_cast<uint8_t>(fillA * globalAlpha);
                renderer_->fillRect(cmd.x, cmd.y, cmd.w, cmd.h, {fillR, fillG, fillB, a});
                break;
            }
            case CT::ClearRect:
                renderer_->fillRect(cmd.x, cmd.y, cmd.w, cmd.h, {0, 0, 0, 255}); break;
            default: break;
            }
        }
    }

    drawTraversal_->draw(document_->documentElement(), 0, 0, viewportWidth_, viewportHeight_);

    if (systemOverlay_ && systemOverlay_->isVisible()) {
        systemOverlay_->tick(virtualTime_);
        systemOverlay_->render(viewportWidth_, viewportHeight_);
        auto* sysRenderer = systemOverlay_->getRenderer();
        if (sysRenderer && sysRenderer->surface()) {
            auto* appCanvas = renderer_->getCanvas();
            if (appCanvas) {
                sk_sp<SkImage> sysImage = sysRenderer->surface()->makeImageSnapshot();
                if (sysImage) {
                    SkPaint paint;
                    paint.setBlendMode(SkBlendMode::kSrcOver);
                    appCanvas->drawImage(sysImage, 0, 0, SkSamplingOptions(), &paint);
                }
            }
        }
    }

    renderer_->endFrame();
    return renderer_->capturePixels();
}

bool Engine::screenshot(const std::string& path, int cx, int cy, int cw, int ch) {
    auto pixels = capturePixels();
    if (pixels.empty()) return false;

    int fw = viewportWidth_, fh = viewportHeight_;

    // Clamp crop rect to viewport bounds
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > fw) cw = fw - cx;
    if (cy + ch > fh) ch = fh - cy;
    if (cw <= 0 || ch <= 0) return false;

    // Extract cropped region
    std::vector<uint8_t> cropped(cw * ch * 4);
    for (int y = 0; y < ch; ++y) {
        const uint8_t* src = pixels.data() + ((cy + y) * fw + cx) * 4;
        uint8_t* dst = cropped.data() + y * cw * 4;
        memcpy(dst, src, cw * 4);
    }

    return stbi_write_png(path.c_str(), cw, ch, 4, cropped.data(), cw * 4) != 0;
}

dom::Element* Engine::querySelector(const std::string& selector) const {
    if (!document_) return nullptr;

    // Handle #id shorthand
    if (!selector.empty() && selector[0] == '#') {
        return document_->getElementById(selector.substr(1));
    }

    return document_->querySelector(selector);
}

void Engine::dispatchClickOn(dom::Element* target) {
    if (!target || !jsRuntime_) return;
    if (document_) document_->setActiveElement(target);
    dom::MouseEvent event("click");
    js::dispatchDomEvent(jsRuntime_->getContext(), target, event);
}

} // namespace bro::engine
