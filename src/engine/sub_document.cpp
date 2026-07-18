// Shared sub-document core — see sub_document.h for why this is a set of free
// functions over a SubDocRef view rather than a common base struct.

#include "engine/sub_document.h"

#include "engine/default_styles.h"
#include "engine/app_loader.h"
#include "engine/replaced_elements.h"
#include "engine/settings.h"
#include "util/log.h"
#include "layout/box.h"
#include "layout/element_ref_adapter.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "render/renderer.h"
#include "render/command_buffer.h"
#include "render/command_replayer.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/element_geometry.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/canvas_bindings.h"
#include "js/image_bindings.h"
#include "js/imagebitmap_bindings.h"
#include "js/window_bindings.h"
#include "js/window_host_bindings.h"
#include "js/settings_bindings.h"
#include "js/storage_bindings.h"
#include "canvas/canvas_scene.h"
#include "api/api.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>
#include <include/gpu/ganesh/GrDirectContext.h>

#include <glad/gl.h>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace bro::engine {

// The two bridges from an owning struct to the shared view. Kept here rather
// than inline in engine.h so SubDocRef stays an incomplete type there.
SubDocRef Engine::iframeSubDoc(IframeDoc& d) {
    return SubDocRef{d.jsCtx, d.timers, d.canvasScenes, d.document,
                     d.hoveredElement, d.boxW, d.boxH, d.cmdBuffer,
                     d.surface, d.surfW, d.surfH, d.fboTexture};
}

SubDocRef Engine::windowHostSubDoc(WindowHost& h) {
    return SubDocRef{h.jsCtx, h.timers, h.canvasScenes, h.document,
                     h.hoveredElement, h.boxW, h.boxH, h.cmdBuffer,
                     h.surface, h.surfW, h.surfH, h.fboTexture};
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

SubDocSource loadSubDocSource(const std::string& basePath, const std::string& srcAttr,
                              const util::AssetMounts* mounts, const char* what) {
    SubDocSource out;
    out.resolvedSrc = AppLoader::resolvePath(basePath, srcAttr, mounts);

    // A sub-document hosts a directory-based bro app (index.html + styles +
    // scripts), exactly like a top-level `bro <dir>`. If src points at a file,
    // use its dir.
    std::error_code ec;
    out.appDir = out.resolvedSrc;
    if (!fs::is_directory(out.resolvedSrc, ec)) {
        // The parent_path() fallback is ONLY for a src that names a file
        // ("child/index.html"). A src that doesn't exist must fail here:
        // falling back to its parent turns a typo like src="no-such-app" into
        // the host app's own directory, silently embedding the host document
        // inside itself instead of reporting the bad src.
        if (!fs::is_regular_file(out.resolvedSrc, ec)) {
            LOG_ERROR("%s: src '%s' does not exist (resolved to '%s')", what,
                      srcAttr.c_str(), out.resolvedSrc.c_str());
            return out;
        }
        out.appDir = fs::path(out.resolvedSrc).parent_path().string();
    }

    out.manifest = AppLoader::loadApp(out.appDir, mounts);
    out.html = AppLoader::loadFile(out.manifest.htmlPath);
    if (out.html.empty()) {
        LOG_ERROR("%s: no index.html at '%s' (src='%s')", what,
                  out.appDir.c_str(), srcAttr.c_str());
        return out;
    }

    for (auto& p : out.manifest.stylePaths) {
        std::string css = AppLoader::loadFile(p);
        if (!css.empty()) out.authorStyles += css + "\n";
    }
    out.ok = true;
    return out;
}

void buildSubDocDocument(SubDocRef d, const SubDocSource& src,
                         const std::string& colorScheme) {
    std::string html = src.html;
    std::vector<dom::Document::TemplateBlock> templates;
    html = dom::Document::extractTemplates(html, templates);
    d.document = std::make_unique<dom::Document>();
    d.document->setBasePath(src.manifest.basePath);
    // Sub-document media queries evaluate against the sub-doc's own box; the
    // color scheme follows the engine's (settings override or OS theme).
    d.document->setMediaViewport(static_cast<float>(d.boxW),
                                 static_cast<float>(d.boxH));
    d.document->setMediaColorScheme(colorScheme);
    d.document->parse(html, src.authorStyles, kDefaultStyles);
    if (!templates.empty()) d.document->injectTemplates(templates);
}

// ---------------------------------------------------------------------------
// Realm
// ---------------------------------------------------------------------------

void buildSubDocRealm(SubDocRef d, js::Runtime* runtime, Engine* engine,
                      const SubDocSource& src, render::Renderer* renderer,
                      const SubDocRealmOptions& o) {
    // Dedicated JS realm + standard app bindings, isolated from the host app.
    d.jsCtx = runtime->createContext();
    brokit::api::installConsole(d.jsCtx);
    d.timers = std::make_unique<js::Timers>();
    js::Timers::install(d.jsCtx, d.timers.get());
    // Prime the timer clock BEFORE any script runs — see nowMs in the options.
    d.timers->tick(o.nowMs);
    // Per-realm window state: innerWidth/innerHeight are this sub-doc's box,
    // and window.screen / navigator answer for `o.window` — the host's OWN
    // secondary window when there is one (window_bindings keys its window
    // pointer per JSContext exactly so this can differ per realm).
    js::installWindowBindings(d.jsCtx, d.boxW, d.boxH, o.displayScale,
                              o.window, o.headless);
    // Scoped bro.window.* for a secondary-window host: the same implementation
    // the app realm gets, parameterized on THIS host's window.
    if (o.installBroWindow)
        js::installBroWindowBindings(d.jsCtx, o.window, o.headless);
    // bro.window.open exists in child realms only to throw its deliberate
    // "main app realm only" error (the binding gates on the engine's primary
    // context at call time).
    js::installWindowHostBindings(d.jsCtx, engine);

    js::DomBindings::install(d.jsCtx, d.document.get());
    js::StorageBindings::install(d.jsCtx, src.manifest.basePath + "/.storage.json");
    js::StorageBindings::installSessionStorage(d.jsCtx);
    if (o.settings)
        js::SettingsBindings::install(d.jsCtx, o.settings, o.window, engine);
    js::CanvasBindings::install(d.jsCtx);
    js::ImageBindings::install(d.jsCtx, src.manifest.basePath);
    js::ImageBitmapBindings::install(d.jsCtx);

    // Canvas 2D getContext factory — parks each <canvas>'s scene in THIS
    // sub-doc. The captured references live in the owning struct, whose address
    // is stable (heap unique_ptr) for as long as this context exists.
    auto* scenes = &d.canvasScenes;
    bool warnWebGL = o.warnOnWebGL;
    const char* what = o.what;
    js::DomBindings::setGetContextFactory(d.jsCtx,
        [renderer, scenes, warnWebGL, what](JSContext* fctx, dom::Element* cel,
                                            const std::string& type) -> JSValue {
            if (type != "2d") {
                if (warnWebGL && (type == "webgl" || type == "webgl2" ||
                                  type == "experimental-webgl")) {
                    LOG_WARN("%s: canvas.getContext('%s') is not available in a "
                             "secondary window (v1) — returning null", what,
                             type.c_str());
                }
                return JS_NULL;
            }
            auto scene = std::make_unique<canvas::CanvasScene>(renderer);
            scene->init(nullptr);
            if (cel) {
                scene->setLayoutCallback(
                    [](void* ud, float& ox, float& oy, float& ow, float& oh) {
                        auto* e = static_cast<dom::Element*>(ud);
                        if (!e->parentNode()) { ox = oy = ow = oh = 0; return; }
                        dom::AbsoluteRect r = dom::absoluteContentBox(e);
                        ox = r.x; oy = r.y; ow = r.width; oh = r.height;
                    }, cel);
                scene->setDetachedCallback([](void* ud) -> bool {
                    auto* n = static_cast<dom::Element*>(ud);
                    while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                    return n->tagName() != "html" && n->tagName() != "HTML";
                }, cel);
                scene->setLiveCheck([](void* dd, void* node) -> bool {
                    return static_cast<dom::Document*>(dd)->isNodeLive(
                        static_cast<dom::Element*>(node));
                }, cel->document());
            }
            auto* ptr = scene.get();
            if (cel) cel->setCanvasScene(ptr, &canvas::CanvasScene::onBackingElementDestroyed);
            scenes->push_back(std::move(scene));
            return js::CanvasBindings::wrapContext2D(fctx, ptr);
        });
}

void runSubDocScripts(SubDocRef d, const SubDocSource& src, const char* what) {
    for (auto& s : src.manifest.scripts) {
        if (s.isModule) {
            LOG_WARN("%s '%s': <script type=module> not yet supported, skipping",
                     what, src.appDir.c_str());
            continue;
        }
        std::string code = s.isInline() ? s.code : AppLoader::loadFile(s.path);
        if (code.empty()) continue;
        std::string fname = s.isInline() ? (src.manifest.basePath + "/<inline>") : s.path;
        JSValue r = JS_Eval(d.jsCtx, code.c_str(), code.size(), fname.c_str(),
                            JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            JSValue ex = JS_GetException(d.jsCtx);
            const char* str = JS_ToCString(d.jsCtx, ex);
            if (str) {
                LOG_ERROR("%s '%s' JS error: %s", what, src.appDir.c_str(), str);
                JS_FreeCString(d.jsCtx, str);
            }
            JS_FreeValue(d.jsCtx, ex);
        }
        JS_FreeValue(d.jsCtx, r);
    }
}

void finishSubDocLoad(SubDocRef d, const SubDocSource& src,
                      render::Renderer* renderer, broaudio::Engine* audio,
                      layout::SkiaTextMetrics& metrics) {
    bro::engine::ensureReplacedElements(d.document->documentElement(), renderer,
                                        d.jsCtx, audio);
    d.document->resolveStyles();
    d.document->performLayout(static_cast<float>(d.boxW),
                              static_cast<float>(d.boxH), metrics);
    d.document->setBasePath(src.manifest.basePath);
}

static void collectNestedIframes(dom::Element* el, std::vector<dom::Element*>& out) {
    if (!el) return;
    std::string_view tag = el->tagName();
    if (tag == "iframe" || tag == "IFRAME") out.push_back(el);
    for (auto* child : el->childNodes()) {
        if (child->nodeType() == dom::NodeType::Element)
            collectNestedIframes(static_cast<dom::Element*>(child), out);
    }
}

void warnNestedIframes(SubDocRef d, const char* what) {
    if (!d.document) return;
    std::vector<dom::Element*> frames;
    collectNestedIframes(d.document->documentElement(), frames);
    for (auto* f : frames) {
        std::string src = f->getAttribute("src");
        LOG_WARN("%s: nested <iframe src=\"%s\"> is not supported (v1) — "
                 "the element renders as an empty box", what, src.c_str());
    }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

bool tickSubDoc(SubDocRef d, double nowMs) {
    if (d.timers) {
        d.timers->tick(nowMs);
        d.timers->fireAnimationFrames(nowMs);
    }
    // Never recorded / just reloaded (the buffer is cleared+refilled per record,
    // so 0 means no record has landed yet for this sub-doc).
    if (d.cmdBuffer.commandCount() == 0) return true;
    // DOM changed since the last resolveStyles.
    if (d.document && d.document->isDirty()) return true;
    // Animating: a rAF callback rescheduled itself for the next frame.
    if (d.timers && d.timers->hasPendingAnimationFrames()) return true;
    return false;
}

void recordSubDoc(SubDocRef d, render::RecordingRenderer* rec,
                  layout::DrawTraversal* traversal, layout::SkiaTextMetrics& metrics) {
    d.cmdBuffer.clear();
    if (!d.document || !d.document->documentElement()) return;
    // Style + lay the sub-document out at its box (mirrors layoutSystemPanels):
    // JS may have mutated its DOM since the last frame, and the text runs the
    // draw consumes are produced here.
    // Point the style adapter's hover/active state at THIS sub-document's
    // targets before resolving it (the thread-local was last set for the host
    // doc). :hover in the sub-doc resolves against its own hovered element;
    // restored to the host's on the next host resolveStyles.
    layout::ElementRefAdapter::setHoveredElement(d.hoveredElement);
    d.document->resolveStyles();
    d.document->performLayout(static_cast<float>(d.boxW),
                              static_cast<float>(d.boxH), metrics);
    // Hand this frame's <canvas> draw calls (recorded into each scene's command
    // list by JS on the main thread) to the scene's staged buffer, so the
    // raster-side inline blit (replaySubDoc) can replay them. Mirrors
    // stageSystemPanelCanvases for panels.
    for (auto& scene : d.canvasScenes) {
        if (scene) scene->stageCommandsForRaster();
    }
    rec->setBuffer(&d.cmdBuffer);
    traversal->setLayerBreakCallback(
        [rec](canvas::CanvasScene* scene, unsigned int, float x, float y,
              float w, float h, float, float, float, float) {
            if (scene) rec->recordBlitCanvasInline(scene, x, y, w, h);
        });
    traversal->setBasePath(d.document->basePath());
    traversal->draw(d.document->documentElement(), 0, 0,
                    static_cast<float>(d.boxW), static_cast<float>(d.boxH),
                    /*viewportTop=*/0);
    traversal->setLayerBreakCallback(nullptr);
    rec->setBuffer(nullptr);
}

// The inline-<canvas> blit handler every sub-document replay shares: the
// sub-doc's own canvases paint straight into the sub-doc surface (they are not
// app-level canvas layers).
static void replayBufferWithInlineCanvas(render::SkiaRenderer* renderer,
                                         GrDirectContext* grCtx,
                                         const render::CommandBuffer& buffer) {
    render::CommandReplayer replayer(renderer);
    replayer.setBlitCanvasInlineHandler(
        [&](void* scenePtr, float x, float y, float w, float h) {
            auto* scene = static_cast<canvas::CanvasScene*>(scenePtr);
            if (!scene || w <= 0 || h <= 0) return;
            if (grCtx) scene->setGrContext(grCtx);
            scene->flushStaged();
            auto* src = scene->surface();
            if (!src) return;
            auto img = src->makeImageSnapshot();
            if (!img) return;
            auto* c = renderer->getCanvas();
            if (!c) return;
            // canvas ensureSurface did raw GL; resync before sampling.
            if (grCtx) grCtx->resetContext();
            c->drawImageRect(img, SkRect::MakeXYWH(x, y, w, h),
                             SkSamplingOptions(SkFilterMode::kLinear));
            scene->clearDirty();
        });
    replayer.replay(buffer);
}

void replaySubDoc(SubDocRef d, render::SkiaRenderer* renderer) {
    auto* grCtx = renderer->grContext();
    if (d.cmdBuffer.commandCount() == 0) { d.fboTexture = 0; return; }
    int bw = std::max(1, d.boxW), bh = std::max(1, d.boxH);
    if (!d.surface.surface || d.surfW != bw || d.surfH != bh) {
        if (d.surface.surface) renderer->destroyGPUSurface(d.surface);
        d.surface = renderer->createGPUSurface(bw, bh);
        d.surfW = bw; d.surfH = bh;
    }
    renderer->rewrapGPUSurface(d.surface, bw, bh);
    auto prev = renderer->switchSurface(d.surface.surface);
    if (auto* c = renderer->getCanvas()) c->clear(SK_ColorTRANSPARENT);
    replayBufferWithInlineCanvas(renderer, grCtx, d.cmdBuffer);
    if (grCtx) grCtx->flush(d.surface.surface.get());
    d.fboTexture = d.surface.texture;
    renderer->switchSurface(prev);
}

std::vector<uint8_t> captureSubDoc(SubDocRef d, render::SkiaRenderer* skia,
                                   int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (d.cmdBuffer.commandCount() == 0) return {};
    int w = std::max(1, d.boxW), h = std::max(1, d.boxH);

    auto* grCtx = skia->grContext();
    grCtx->resetContext();
    render::SkiaRenderer::GPUSurface surf = skia->createGPUSurface(w, h);
    if (!surf.surface) { grCtx->resetContext(); return {}; }
    auto prev = skia->switchSurface(surf.surface);
    if (auto* c = skia->getCanvas()) c->clear(SK_ColorTRANSPARENT);

    replayBufferWithInlineCanvas(skia, grCtx, d.cmdBuffer);
    grCtx->flush(surf.surface.get());

    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, surf.texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        pixels.clear();
    else
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);

    skia->switchSurface(prev);
    skia->destroyGPUSurface(surf);
    grCtx->resetContext();

    if (pixels.empty()) return {};
    outW = w;
    outH = h;
    return pixels;
}

void teardownSubDoc(SubDocRef d) {
    if (d.timers && d.jsCtx) d.timers->clearAll(d.jsCtx);
    d.timers.reset();
    if (d.jsCtx) {
        js::DomBindings::cleanup(d.jsCtx);
        js::cleanupWindowHostBindings(d.jsCtx);
        js::cleanupWindowBindings(d.jsCtx);
    }
    d.document.reset();
    if (d.jsCtx) { JS_FreeContext(d.jsCtx); d.jsCtx = nullptr; }
}

} // namespace bro::engine
