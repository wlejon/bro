#include "engine/engine.h"
#include "engine/key_mapping.h"
#include "engine/hit_testing.h"
#include "engine/overflow.h"
#include "engine/system_overlay.h"

#include "observer_check.js.h"
#include "canvas_resize.js.h"

#include "platform/sdl_window.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/scene_layer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/canvas_bindings.h"
#include "js/event_dispatch.h"
#include "js/audio_bindings.h"
#include "js/storage_bindings.h"
#include "js/dialog_bindings.h"
#include "js/window_bindings.h"
#include "js/custom_elements.h"
#include "js/webgl2_bindings.h"
#include "js/image_bindings.h"

#include "api/api.h"
#include "runtime/runtime.h"
#include <broaudio/engine.h>
#include "canvas/canvas_scene.h"
#include "webgl/webgl2_context.h"
#include "webgl/webgl_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/event.h"
#include "dom/shadow_root.h"

#include <cstring>
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

    // Wire brokit logging through bro's LOG_* macros
    brokit::Runtime::setLogCallback([](brokit::Runtime::LogLevel level, const std::string& msg) {
        switch (level) {
            case brokit::Runtime::LogLevel::Warn:  LOG_WARN("[console] %s", msg.c_str()); break;
            case brokit::Runtime::LogLevel::Error: LOG_ERROR("[console] %s", msg.c_str()); break;
            default: LOG_INFO("[console] %s", msg.c_str()); break;
        }
    });

    // Install all brokit APIs (console, timers, URL, crypto, encoding, fetch, etc.)
    brokit::api::installAll(jsRuntime_->getContext());

    timers_ = std::make_unique<js::Timers>();
    js::Timers::install(jsRuntime_->getContext(), timers_.get());

    // 4b. Audio engine + bindings
    audioEngine_ = std::make_unique<broaudio::Engine>();
    if (displayMode_ == DisplayMode::Windowed) {
        audioEngine_->init();
    } else {
        audioEngine_->initHeadless();
    }
    js::AudioBindings::install(jsRuntime_->getContext(), audioEngine_.get());

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

    // 8b. Set window title (windowed only)
    if (window_) {
        if (!config.title.empty()) {
            window_->setTitle(config.title);
        } else {
            std::string docTitle = document_->title();
            if (!docTitle.empty()) {
                window_->setTitle(docTitle);
            }
        }
    }

    // 9. Set up window/navigator/location/history BEFORE DOM bindings
    js::installWindowBindings(jsRuntime_->getContext(), viewportWidth_, viewportHeight_);

    // 9x. Native file dialogs (modal to our SDL window).
    //      Pass a tick callback so JS timers keep running while dialog is open.
    js::DialogBindings::install(jsRuntime_->getContext(),
                                window_ ? window_->getSDLWindow() : nullptr,
                                [this]() { tickTimersOnly(); });

    // 9a. Install DOM JS bindings (after window so polyfills work)
    js::DomBindings::install(jsRuntime_->getContext(), document_.get());

    // 9b. Install custom elements (after DOM bindings — needs element class ID)
    js::installCustomElements(jsRuntime_->getContext(),
                              js::DomBindings::elementClassId(), document_.get());

    // 9c. Install Canvas 2D bindings + getContext factory
    js::CanvasBindings::install(jsRuntime_->getContext());
    js::ImageBindings::install(jsRuntime_->getContext(), manifest_.basePath);

    // Register app directory as fetch base path (overlay: last added = checked first)
    brokit::api::addFetchBasePath(jsRuntime_->getContext(), manifest_.basePath);

    if (gl_) {
        // GPU path: WebGL2 + full canvas factory (windowed or GPU headless)
        js::WebGL2Bindings::install(jsRuntime_->getContext());
        js::DomBindings::setGetContextFactory(jsRuntime_->getContext(),
            [this](JSContext* ctx, dom::Element* el, const std::string& type) -> JSValue {
                if (type == "2d") {
                    auto scene = std::make_unique<canvas::CanvasScene>(renderer_.get());
                    if (el) {
                        scene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
                            auto* elem = static_cast<dom::Element*>(ud);
                            if (!elem->parentNode()) {
                                ox = oy = ow = oh = 0;
                                return;
                            }
                            auto& box = elem->layoutBox();
                            ox = box.contentRect.x;
                            oy = box.contentRect.y;
                            for (auto* lp = elem->layoutParent(); lp; lp = lp->layoutParent()) {
                                auto& pb = lp->layoutBox();
                                ox += pb.contentRect.x;
                                oy += pb.contentRect.y;
                                oy -= lp->scrollTopValue();
                            }
                            ow = box.contentRect.width;
                            oh = box.contentRect.height;
                        }, el);
                        scene->setDetachedCallback([](void* ud) -> bool {
                            // Walk up to check if connected to the document
                            auto* n = static_cast<dom::Element*>(ud);
                            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                            return n->tagName() != "html" && n->tagName() != "HTML";
                        }, el);
                    }
                    auto* ptr = scene.get();
                    if (el) el->setCanvasScene(ptr);
                    addCanvasScene(std::move(scene));
                    return js::CanvasBindings::wrapContext2D(ctx, ptr);
                }
                if (type == "webgl2" || type == "webgl") {
                    auto* webglCtx = new webgl::WebGL2RenderingContext(
                        viewportWidth_, viewportHeight_);
                    auto scene = std::make_unique<webgl::WebGLScene>(webglCtx);
                    addSceneLayer(std::move(scene));
                    return js::WebGL2Bindings::wrapContext(ctx, webglCtx);
                }
                return JS_NULL;
            });
    } else {
        // CPU path: 2D canvas only, no WebGL
        js::DomBindings::setGetContextFactory(jsRuntime_->getContext(),
            [this](JSContext* ctx, dom::Element* el, const std::string&) -> JSValue {
                auto scene = std::make_unique<canvas::CanvasScene>(renderer_.get());
                if (el) {
                    scene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
                        auto* elem = static_cast<dom::Element*>(ud);
                        if (!elem->parentNode()) {
                            ox = oy = ow = oh = 0;
                            return;
                        }
                        auto& box = elem->layoutBox();
                        ox = box.contentRect.x;
                        oy = box.contentRect.y;
                        for (auto* lp = elem->layoutParent(); lp; lp = lp->layoutParent()) {
                            auto& pb = lp->layoutBox();
                            ox += pb.contentRect.x;
                            oy += pb.contentRect.y;
                            oy -= lp->scrollTopValue();
                        }
                        ow = box.contentRect.width;
                        oh = box.contentRect.height;
                    }, el);
                    scene->setDetachedCallback([](void* ud) -> bool {
                        auto* n = static_cast<dom::Element*>(ud);
                        while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                        return n->tagName() != "html" && n->tagName() != "HTML";
                    }, el);
                }
                auto* ptr = scene.get();
                if (el) el->setCanvasScene(ptr);
                scene->init(nullptr);
                canvasScenes_.push_back(std::move(scene));
                return js::CanvasBindings::wrapContext2D(ctx, ptr);
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

    // 10a. Dispatch DOMContentLoaded on document
    {
        JSContext* ctx = jsRuntime_->getContext();
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue dispatch = JS_GetPropertyStr(ctx, global, "__bro_dispatch_window_event");
        if (JS_IsFunction(ctx, dispatch)) {
            // DOMContentLoaded
            JSValue dclType = JS_NewString(ctx, "DOMContentLoaded");
            JSValue dclEvt = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, dclEvt, "type", JS_NewString(ctx, "DOMContentLoaded"));
            JS_SetPropertyStr(ctx, dclEvt, "bubbles", JS_TRUE);
            JS_SetPropertyStr(ctx, dclEvt, "cancelable", JS_FALSE);
            JS_SetPropertyStr(ctx, dclEvt, "isTrusted", JS_TRUE);
            JS_SetPropertyStr(ctx, dclEvt, "target", JS_DupValue(ctx, global));
            JSValue dclArgs[2] = { dclType, dclEvt };
            JSValue dclRet = JS_Call(ctx, dispatch, global, 2, dclArgs);
            JS_FreeValue(ctx, dclRet);
            JS_FreeValue(ctx, dclType);
            JS_FreeValue(ctx, dclEvt);

            // load
            JSValue loadType = JS_NewString(ctx, "load");
            JSValue loadEvt = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, loadEvt, "type", JS_NewString(ctx, "load"));
            JS_SetPropertyStr(ctx, loadEvt, "bubbles", JS_FALSE);
            JS_SetPropertyStr(ctx, loadEvt, "cancelable", JS_FALSE);
            JS_SetPropertyStr(ctx, loadEvt, "isTrusted", JS_TRUE);
            JS_SetPropertyStr(ctx, loadEvt, "target", JS_DupValue(ctx, global));
            JSValue loadArgs[2] = { loadType, loadEvt };
            JSValue loadRet = JS_Call(ctx, dispatch, global, 2, loadArgs);
            JS_FreeValue(ctx, loadRet);
            JS_FreeValue(ctx, loadType);
            JS_FreeValue(ctx, loadEvt);
        }
        JS_FreeValue(ctx, dispatch);
        JS_FreeValue(ctx, global);
        jsRuntime_->executePendingJobs();
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

void Engine::addSceneLayer(std::unique_ptr<render::SceneLayer> layer) {
    if (layer) {
        layer->onInit(gl_.get(), viewportWidth_, viewportHeight_);
    }
    sceneLayers_.push_back(std::move(layer));
}

void Engine::addCanvasScene(std::unique_ptr<canvas::CanvasScene> scene) {
    if (scene) {
        scene->init(gl_.get());
        // Share GPU Skia context for hardware-accelerated canvas rendering
        auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
        if (skia && skia->grContext()) {
            scene->setGrContext(skia->grContext());
        }
    }
    canvasScenes_.push_back(std::move(scene));
}

void Engine::compositeCanvasScenes(int w, int h) {
    compositeCanvasScenes(gl_.get(), w, h, 0);
}

void Engine::compositeCanvasScenes(render::GLContext* gl, int w, int h, GLuint targetFBO) {
    if (!gl || canvasScenes_.empty()) return;

    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
    glViewport(0, 0, w, h);

    glUseProgram(gl->textureProgram());
    float viewport[2] = {(float)w, (float)h};
    glUniform2fv(gl->textureViewportLoc(), 1, viewport);
    glUniform1i(gl->textureSamplerLoc(), 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (auto& cs : canvasScenes_) {
        GLuint tex = cs->texture();
        if (!tex) continue;

        float cx, cy, cw, ch;
        cs->getScreenRect(cx, cy, cw, ch);

        // Raster surface is top-down: V=0 at top, V=1 at bottom.
        render::TextureVertex quad[6] = {
            {cx,      cy,      0, 0}, {cx+cw, cy,      1, 0}, {cx+cw, cy+ch, 1, 1},
            {cx,      cy,      0, 0}, {cx+cw, cy+ch, 1, 1}, {cx,      cy+ch, 0, 1},
        };

        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex),
                              (void*)offsetof(render::TextureVertex, u));

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &vbo);
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Engine::drawTexturedQuad(GLuint tex, float x, float y, float w, float h) {
    if (!tex || !gl_) return;

    render::TextureVertex quad[6] = {
        {x,   y,   0, 0}, {x+w, y,   1, 0}, {x+w, y+h, 1, 1},
        {x,   y,   0, 0}, {x+w, y+h, 1, 1}, {x,   y+h, 0, 1},
    };

    glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);

    glBindVertexArray(uiQuadVAO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex),
                          (void*)offsetof(render::TextureVertex, u));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Engine::compositeLayers() {
    if (!gl_) return;

    auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
    if (!skia) return;

    float vw = static_cast<float>(viewportWidth_);
    float vh = static_cast<float>(viewportHeight_);

    glUseProgram(gl_->textureProgram());
    float viewport[2] = {vw, vh};
    glUniform2fv(gl_->textureViewportLoc(), 1, viewport);
    glUniform1i(gl_->textureSamplerLoc(), 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Bind VAO and set up vertex attribs once for all quads
    glBindVertexArray(uiQuadVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          sizeof(render::TextureVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          sizeof(render::TextureVertex),
                          (void*)offsetof(render::TextureVertex, u));
    glActiveTexture(GL_TEXTURE0);

    for (auto& layer : uiLayers_) {
        if (layer.type == UILayer::HTML) {
            if (layer.texture) {
                render::TextureVertex quad[6] = {
                    {0,  0,  0, 0}, {vw, 0,  1, 0}, {vw, vh, 1, 1},
                    {0,  0,  0, 0}, {vw, vh, 1, 1}, {0,  vh, 0, 1},
                };
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
                glBindTexture(GL_TEXTURE_2D, layer.texture);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        } else {
            // Canvas layer — texture already uploaded in raster phase
            if (layer.canvasScene) {
                GLuint tex = layer.canvasScene->texture();
                if (tex) {
                    float cx = layer.cx, cy = layer.cy;
                    float cw = layer.cw, ch = layer.ch;
                    render::TextureVertex quad[6] = {
                        {cx,    cy,    0, 0}, {cx+cw, cy,    1, 0}, {cx+cw, cy+ch, 1, 1},
                        {cx,    cy,    0, 0}, {cx+cw, cy+ch, 1, 1}, {cx,    cy+ch, 0, 1},
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
                    // Canvas textures use premultiplied alpha (same as HTML layers)
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                }
            }
        }
    }
}

Engine::~Engine() {
    for (auto& sl : sceneLayers_) {
        if (sl) sl->onCleanup();
    }
    sceneLayers_.clear();
    canvasScenes_.clear();
    systemOverlay_.reset();

    // 1. Clear timers (they hold JS callbacks)
    if (timers_ && jsRuntime_) {
        timers_->clearAll(jsRuntime_->getContext());
    }

    // 2. Clear JS bindings
    if (jsRuntime_) {
        JSContext* ctx = jsRuntime_->getContext();
        js::setElementFinalizerShutdown(true);
        js::AudioBindings::cleanup(ctx);
        js::StorageBindings::cleanup(ctx);
        if (gl_) {
            js::WebGL2Bindings::cleanup(ctx);
        }
        js::cleanupCustomElements(ctx);

        // Clean up global properties (prevents leaked references).
        // Delete document BEFORE elem_map — the map holds JS refs to elements
        // whose finalizers call freeNode(); deleting the map first can free
        // elements that are still referenced by the document tree, causing
        // use-after-free when the document wrapper is subsequently collected.
        JSValue global = JS_GetGlobalObject(ctx);
        JSAtom a1 = JS_NewAtom(ctx, "document");
        JSAtom a2 = JS_NewAtom(ctx, "__bro_elem_map");
        JSAtom a3 = JS_NewAtom(ctx, "console");
        JS_DeleteProperty(ctx, global, a1, 0);
        JS_DeleteProperty(ctx, global, a2, 0);
        JS_DeleteProperty(ctx, global, a3, 0);
        JS_FreeAtom(ctx, a1);
        JS_FreeAtom(ctx, a2);
        JS_FreeAtom(ctx, a3);
        JS_FreeValue(ctx, global);

        js::DomBindings::cleanup(ctx);
        jsRuntime_->executePendingJobs();
        JS_RunGC(jsRuntime_->getRuntime());
    }

    // 3. GL cleanup (windowed only)
    {
        auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
        if (skia) {
            for (auto& ps : htmlSurfacePool_) skia->destroyGPUSurface(ps);
        }
        htmlSurfacePool_.clear();
    }
    if (uiQuadVBO_) { glDeleteBuffers(1, &uiQuadVBO_); uiQuadVBO_ = 0; }
    if (uiQuadVAO_) { glDeleteVertexArrays(1, &uiQuadVAO_); uiQuadVAO_ = 0; }

    // 4. Release layout resources before document
    drawTraversal_.reset();

    // Clean up per-runtime DomBindings state before the runtime is freed.
    if (jsRuntime_) {
        js::DomBindings::cleanupRuntime(jsRuntime_->getRuntime());
    }
    // Destroy JS runtime BEFORE document — JS_FreeRuntime() runs GC finalizers
    // that dereference Element pointers, so elements must still be alive.
    // Audio engine must also outlive JS runtime because VoiceAllocator/MidiInput
    // destructors reference it (removeVoice, close).
    jsRuntime_.reset();
    audioEngine_.reset();
    document_.reset();
    timers_.reset();
    renderer_.reset();
}

// ---------------------------------------------------------------------------
// Lightweight tick for use during modal blocking (move/resize, dialogs)
// ---------------------------------------------------------------------------

void Engine::tickTimersOnly()
{
    double now = util::currentTimeMs();
    timers_->tick(now);
    jsRuntime_->executePendingJobs();
}

/// SDL event watcher — fires on the main thread during Windows' modal
/// move/resize loop, keeping JS timers (audio sequencer etc.) alive.
static bool modalEventWatcher(void* userdata, SDL_Event* event)
{
    if (event->type >= SDL_EVENT_WINDOW_FIRST &&
        event->type <= SDL_EVENT_WINDOW_LAST) {
        static_cast<Engine*>(userdata)->tickTimersOnly();
    }
    return true; // keep the event in the queue
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

    // Event watcher keeps JS timers alive during Windows' modal move/resize loop.
    SDL_AddEventWatch(modalEventWatcher, this);

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

        // 2a. Tick brokit fetch (pump pending HTTP requests)
        {
            JSValue global = JS_GetGlobalObject(jsRuntime_->getContext());
            JSValue tickFn = JS_GetPropertyStr(jsRuntime_->getContext(), global, "__brokit_fetch_tick");
            if (JS_IsFunction(jsRuntime_->getContext(), tickFn)) {
                JSValue ret = JS_Call(jsRuntime_->getContext(), tickFn, JS_UNDEFINED, 0, nullptr);
                JS_FreeValue(jsRuntime_->getContext(), ret);
            }
            JS_FreeValue(jsRuntime_->getContext(), tickFn);
            JS_FreeValue(jsRuntime_->getContext(), global);
        }

        // 2b. Tick system overlay timers
        if (systemOverlay_) {
            systemOverlay_->tick(now);
        }

        // 3. Bind WebGL FBO before JS callbacks (so gl.bindFramebuffer(null) targets canvas)
        webgl::WebGLScene* webglScene = nullptr;
        for (auto& sl : sceneLayers_) {
            webglScene = dynamic_cast<webgl::WebGLScene*>(sl.get());
            if (webglScene) break;
        }
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

        // 4. Re-layout when DOM is dirty.
        //    The cached Skia texture is composited every GPU frame (cheap).
        //    Layout must always run before rasterization so computed styles
        //    are fresh — never rasterize with stale styles.

        double tLayout = tJs;
        if (document_ && (document_->isDirty() || !hasRenderedOnce_)) {
            if (document_->isStructureDirty()) {
                ensureReplacedElements(document_->documentElement());
            }
            document_->resolveStyles();
            document_->clearStructureDirty();
            document_->performLayout(static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), *textMetrics_);
            document_->clearDirty();
            // Update document height for scroll clamping
            if (document_->documentElement()) {
                auto& box = document_->documentElement()->layoutBox();
                documentHeight_ = box.marginBox().height;
            }

            // Process auto-scroll-to-bottom for tracked overflow elements.
            if (document_ && !document_->scrollToBottomElements().empty()) {
                // Copy the set since setScrollToBottom(false) mutates it.
                auto pending = document_->scrollToBottomElements();
                for (auto* elem : pending) {
                    std::string ov = getOverflowY(elem->computedStyle());
                    if (overflowClips(ov)) {
                        elem->setScrollTopValue(maxScrollTop(elem));
                    }
                    elem->setScrollToBottom(false);
                }
            }

            // Notify ResizeObserver / IntersectionObserver after layout
            if (jsRuntime_) {
                auto* ctx = jsRuntime_->getContext();
                // Compile observer check once, reuse thereafter
                if (JS_IsUndefined(observerCheckFn_)) {
                    observerCheckFn_ = JS_Eval(ctx, js_observer_check,
                                               strlen(js_observer_check),
                                               "<observer-check>",
                                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
                }
                if (!JS_IsUndefined(observerCheckFn_) && !JS_IsException(observerCheckFn_)) {
                    JSValue r = JS_EvalFunction(ctx, JS_DupValue(ctx, observerCheckFn_));
                    JS_FreeValue(ctx, r);
                }
            }

            uiDirty_ = true;
            lastUIRenderMs_ = now;
            tLayout = util::currentTimeMs();
        }
        accumLayoutMs_ += tLayout - tJs;

        // === GPU FRAME (OpenGL) ===

        // 5a. Rasterize HTML layers to Skia surfaces if dirty
        double tRaster = tLayout;
        if (uiDirty_ || !hasRenderedOnce_) {
            // Invalidate surface pool on viewport resize
            if (htmlSurfacePoolW_ != viewportWidth_ || htmlSurfacePoolH_ != viewportHeight_) {
                for (auto& ps : htmlSurfacePool_) {
                    skia->destroyGPUSurface(ps);
                }
                htmlSurfacePool_.clear();
                uiLayers_.clear();
                htmlSurfacePoolW_ = viewportWidth_;
                htmlSurfacePoolH_ = viewportHeight_;
            }

            // Reset layer list for this frame
            uiLayers_.clear();
            int htmlLayerIdx = 0;

            // Set up layer break callback — when the draw traversal hits
            // a <canvas>, we finalize the current HTML layer and start a new one.
            drawTraversal_->setLayerBreakCallback(
                [this, skia, &htmlLayerIdx](canvas::CanvasScene* scene,
                                            float x, float y, float w, float h) {
                    // Start next HTML layer with a pooled GPU surface
                    int prevIdx = htmlLayerIdx;
                    htmlLayerIdx++;
                    while (htmlLayerIdx >= (int)htmlSurfacePool_.size()) {
                        htmlSurfacePool_.push_back(
                            skia->createGPUSurface(viewportWidth_, viewportHeight_));
                    }
                    // Switch to new surface; the previous surface becomes an HTML layer
                    skia->switchSurface(htmlSurfacePool_[htmlLayerIdx].surface);
                    UILayer htmlLayer;
                    htmlLayer.type = UILayer::HTML;
                    htmlLayer.texture = htmlSurfacePool_[prevIdx].texture;
                    uiLayers_.push_back(std::move(htmlLayer));

                    // Insert canvas layer
                    UILayer canvasLayer;
                    canvasLayer.type = UILayer::Canvas;
                    canvasLayer.canvasScene = scene;
                    canvasLayer.cx = x; canvasLayer.cy = y;
                    canvasLayer.cw = w; canvasLayer.ch = h;
                    uiLayers_.push_back(std::move(canvasLayer));
                });

            double tDraw0 = util::currentTimeMs();

            renderer_->beginFrame(viewportWidth_, viewportHeight_);

            // Ensure pool has a GPU surface for HTML layer 0
            if (htmlSurfacePool_.empty()) {
                htmlSurfacePool_.push_back(
                    skia->createGPUSurface(viewportWidth_, viewportHeight_));
            }
            // Switch to pool surface for HTML layer 0; renderer's
            // own surface is saved and restored at endFrame.
            auto origSurface = skia->switchSurface(htmlSurfacePool_[0].surface);

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

            // Draw viewport scrollbar
            {
                float vh = static_cast<float>(viewportHeight_);
                auto& vs = viewportScrollbar_.style();
                auto m = viewportScrollbar_.layout(
                    static_cast<float>(viewportWidth_) - vs.width - vs.margin,
                    0.0f, vh, documentHeight_, vh, scrollY_);
                viewportScrollbar_.draw(renderer_.get(), m);
            }

            // Draw scrollbars for overflow elements
            if (document_) {
                std::function<void(dom::Element*, float, float)> drawElemScrollbars;
                drawElemScrollbars = [&](dom::Element* elem, float offsetX, float offsetY) {
                    if (!elem) return;
                    auto& style = elem->computedStyle();
                    {
                        auto it = style.find("display");
                        if (it != style.end() && it->second == "none") return;
                    }

                    auto& lbox = elem->layoutBox();
                    float absX = lbox.contentRect.x + offsetX;
                    float absY = lbox.contentRect.y + offsetY;

                    std::string ov = getOverflowY(style);
                    if (overflowScrollable(ov)) {
                        float maxST = maxScrollTop(elem);
                        if (maxST > 0) {
                            float viewH = lbox.contentRect.height;
                            float contentH = viewH + maxST;
                            float bx = absX - lbox.padding.left - lbox.border.left;
                            float by = absY - lbox.padding.top - lbox.border.top;
                            float bw = lbox.fullWidth();
                            float bh = lbox.fullHeight();

                            auto& es = elementScrollbar_.style();
                            auto m = elementScrollbar_.layout(
                                bx + bw - es.width - es.margin,
                                by, bh, contentH, viewH,
                                elem->scrollTopValue());
                            bool isHovered = (scrollbarHoveredElement_ == elem);
                            bool isDragging = (scrollbarDragTarget_ == elem);
                            elementScrollbar_.drawWithState(renderer_.get(), m,
                                                            isHovered, isDragging);
                        }
                    }

                    float childOffsetX = absX;
                    float childOffsetY = absY - elem->scrollTopValue();
                    elem->forEachComposedChild([&](dom::Element* child) {
                        drawElemScrollbars(child, childOffsetX, childOffsetY);
                    });
                };
                drawElemScrollbars(document_->documentElement(),
                                   0.0f, -scrollY_);
            }

            // Capture the current (last) HTML layer
            skia->switchSurface(origSurface);
            UILayer lastHtml;
            lastHtml.type = UILayer::HTML;
            lastHtml.texture = htmlSurfacePool_[htmlLayerIdx].texture;
            uiLayers_.push_back(std::move(lastHtml));

            // Flush all Ganesh GPU commands — HTML layer textures are now ready.
            // No CPU→GPU upload needed since pool surfaces render directly to GPU.
            renderer_->endFrame();

            drawTraversal_->setLayerBreakCallback(nullptr);
            hasRenderedOnce_ = true;
            uiDirty_ = false;
        }

        // Rasterize canvas scenes (flush deferred Skia commands + upload texture).
        // Done every frame since canvases animate independently of HTML layout.
        {
            bool anyCanvasRasterized = false;
            for (auto& layer : uiLayers_) {
                if (layer.type == UILayer::Canvas && layer.canvasScene) {
                    layer.canvasScene->rasterize(gl_.get());
                    anyCanvasRasterized = true;
                }
            }
            // After Ganesh canvas rendering, reset GL state so the engine's
            // raw GL compositing pass and next frame's SkiaRenderer see clean state.
            if (anyCanvasRasterized) {
                auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
                if (skia && skia->grContext()) {
                    skia->grContext()->resetContext();
                }
            }
        }
        tRaster = util::currentTimeMs();

        accumRasterMs_ += tRaster - tLayout;

        double tGpu = util::currentTimeMs();

        // 5b. Update canvas scene scroll + clean up detached
        for (auto& cs : canvasScenes_) {
            cs->setViewportScroll(scrollY_);
            cs->checkDetached();
        }
        canvasScenes_.erase(
            std::remove_if(canvasScenes_.begin(), canvasScenes_.end(),
                [](auto& cs) { return cs->isDetached(); }),
            canvasScenes_.end());

        // 5c. Set viewport and clear
        glViewport(0, 0, viewportWidth_, viewportHeight_);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // 5d. Render pass 1: scene layers (WebGL etc.) draw to default framebuffer
        for (auto& sl : sceneLayers_) {
            if (sl) sl->onRender(gl_.get(), viewportWidth_, viewportHeight_, totalFrameMs_);
        }

        // 5e. Render pass 2: composite UI layers in DOM order
        //     HTML layers (cached textures) interleaved with canvas layers
        //     (freshly uploaded textures). Correct document-order stacking.
        compositeLayers();

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
            webgl::WebGLScene* wgl = nullptr;
            for (auto& sl : sceneLayers_) {
                wgl = dynamic_cast<webgl::WebGLScene*>(sl.get());
                if (wgl) break;
            }
            if (wgl && wgl->webglContext()) {
                wgl->webglContext()->restoreState();
            }
        }

        // Measure GPU work before swap (swap includes vsync wait)
        accumGpuMs_ += util::currentTimeMs() - tGpu;

        // Swap buffers (may block on vsync — not counted as GPU work)
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

    SDL_RemoveEventWatch(modalEventWatcher, this);
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
    for (auto& sl : sceneLayers_) {
        if (sl) sl->onResize(w, h);
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
        JSValue fn = JS_Eval(ctx, js_canvas_resize, strlen(js_canvas_resize),
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

// Input focus helpers (also defined in input_handling.cpp for use there)
static layout::ElInput* getElInput(dom::Element* el) {
    return el ? el->inputControl() : nullptr;
}
static layout::ElTextarea* getElTextarea(dom::Element* el) {
    return el ? el->textareaControl() : nullptr;
}
static layout::ElSelect* getElSelect(dom::Element* el) {
    return el ? el->selectControl() : nullptr;
}

// Input handling methods (handleMouse*, handleKey*, handleTextInput,
// handleWheel, advanceFocus, dispatchInputEvent) are in input_handling.cpp.


dom::Element* Engine::hitTest(float x, float y) {
    // x, y are already in document space (scroll-adjusted by callers)
    if (!document_ || !document_->documentElement())
        return document_ ? document_->body() : nullptr;

    auto* hit = hitTestElement(document_->documentElement(), x, y, 0.0f, 0.0f);
    return hit ? hit : document_->body();
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
        ctrl->initSelectedIndex();
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

// Headless/capture API (flush, advanceTime, eval, screenshot, capturePixels,
// querySelector, dispatchClickOn) is in headless_api.cpp.
} // namespace bro::engine
