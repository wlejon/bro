// Engine <iframe> sub-document hosting — embedded, isolated documents rendered
// into an <iframe> element's box. Structurally mirrors the system-panel
// sub-document mechanism (system_panels.cpp) but binds each sub-document to a
// DOM element in the app document and lays it out at that element's content box
// instead of an engine overlay slot. Each iframe is a full, isolated bro app:
// its own JS realm, DOM tree, timers, and canvas scenes.
//
// The build/teardown/record/replay machinery itself lives in sub_document.cpp,
// shared with secondary-window hosts (bro.window.open). What stays here is what
// is genuinely iframe-shaped: reconciliation against <iframe> elements, the
// element-box geometry, reload(), and the load event on the host element.

#include "engine/engine.h"
#include "engine/sub_document.h"
#include "engine/app_loader.h"
#include "util/platform.h"
#include "util/log.h"
#include "layout/box.h"
#include "layout/skia_text_metrics.h"
#include "render/renderer.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/event.h"
#include "js/runtime.h"
#include "js/event_dispatch.h"
#include "js/timers.h"
#include "canvas/canvas_scene.h"

#include <algorithm>

namespace bro::engine {

// Depth-first collect every <iframe> element in a document subtree.
static void collectIframes(dom::Element* el, std::vector<dom::Element*>& out) {
    if (!el) return;
    std::string_view tag = el->tagName();
    if (tag == "iframe" || tag == "IFRAME") out.push_back(el);
    for (auto* child : el->childNodes()) {
        if (child->nodeType() == dom::NodeType::Element)
            collectIframes(static_cast<dom::Element*>(child), out);
    }
}

Engine::IframeDoc* Engine::iframeDocForElement(const dom::Element* el) {
    for (auto& d : iframeDocs_)
        if (d->element == el) return d.get();
    return nullptr;
}

Engine::IframeDoc* Engine::iframeDocById(uint64_t id) {
    for (auto& d : iframeDocs_)
        if (d->id == id) return d.get();
    return nullptr;
}

// captureIframe() lives in engine_compositor.cpp, next to the record/replay
// helpers it reuses to render the sub-document synchronously on the main thread.

// Reconcile iframeDocs_ with the <iframe> elements currently in the app document.
void Engine::syncIframes() {
    if (!document_) return;

    std::vector<dom::Element*> frames;
    collectIframes(document_->documentElement(), frames);

    // Tear down sub-docs whose <iframe> element has left the tree.
    for (auto it = iframeDocs_.begin(); it != iframeDocs_.end();) {
        bool present = false;
        for (auto* f : frames) if (f == (*it)->element) { present = true; break; }
        if (!present) {
            // Hand the GPU surface to its owning context before the IframeDoc
            // (and with it the surface) is destroyed on this thread — see
            // queueIframeSurfaceFree. Erasing it here would leak the FBO.
            queueIframeSurfaceFree(std::move((*it)->surface));
            teardownIframeDoc(it->get());
            it = iframeDocs_.erase(it);
        }
        else ++it;
    }

    // Forget load failures for elements that are gone. Also what keeps stale
    // Element* keys from accumulating as the DOM churns (addresses get reused).
    for (auto it = iframeLoadFailed_.begin(); it != iframeLoadFailed_.end();) {
        bool present = false;
        for (auto* f : frames) if (f == it->first) { present = true; break; }
        if (!present) it = iframeLoadFailed_.erase(it);
        else ++it;
    }

    // Create sub-docs for new src'd iframes.
    for (auto* f : frames) {
        std::string src = f->getAttribute("src");
        if (src.empty()) continue;
        if (iframeDocForElement(f)) continue;
        // Don't re-attempt a src that already failed. syncIframes runs on every
        // DOM structure change, so without this a single bad src re-hits the
        // filesystem and re-logs the error on every mutation, forever. An
        // explicit reload() or src= clears the record and does retry.
        auto failed = iframeLoadFailed_.find(f);
        if (failed != iframeLoadFailed_.end() && failed->second == src) continue;
        createIframeDoc(f, src);
        if (iframeDocForElement(f)) iframeLoadFailed_.erase(f);
        else iframeLoadFailed_[f] = src;
    }
}

void Engine::createIframeDoc(dom::Element* el, const std::string& srcAttr) {
    SubDocSource source = loadSubDocSource(manifest_.basePath, srcAttr,
                                           &assetMounts_, "iframe");
    if (!source.ok) return;

    auto doc = std::make_unique<IframeDoc>();
    IframeDoc* dp = doc.get();
    dp->element = el;
    dp->id = nextIframeId_++;
    dp->src = source.resolvedSrc;

    // Content-box size from the laid-out <iframe> element (fallback 300x150).
    auto& lbox = el->layoutBox();
    dp->boxW = lbox.contentRect.width  > 0 ? static_cast<int>(lbox.contentRect.width)  : 300;
    dp->boxH = lbox.contentRect.height > 0 ? static_cast<int>(lbox.contentRect.height) : 150;

    SubDocRef ref = iframeSubDoc(*dp);
    buildSubDocDocument(ref, source, effectiveColorScheme());

    SubDocRealmOptions ropts;
    ropts.window = window_.get();
    ropts.displayScale = displayScale_;
    ropts.headless = displayMode_ == DisplayMode::Headless;
    ropts.settings = settings_.get();
    ropts.nowMs = engineNowMs_;
    ropts.what = "iframe";
    buildSubDocRealm(ref, jsRuntime_.get(), this, source, renderer_.get(), ropts);

    // location.reload() inside the sub-document reloads THIS iframe — the same
    // deferred teardown/rebuild as the host calling iframe.reload(). The hook
    // resolves the iframe by id at call time, so a call racing the sub-doc's
    // own destruction (already torn down, id gone) is a safe no-op.
    {
        JSValue global = JS_GetGlobalObject(dp->jsCtx);
        JSValue fdata[2] = {
            JS_NewInt64(dp->jsCtx,
                        static_cast<int64_t>(reinterpret_cast<intptr_t>(this))),
            JS_NewInt64(dp->jsCtx, static_cast<int64_t>(dp->id)),
        };
        JS_SetPropertyStr(dp->jsCtx, global, "__bro_location_reload",
            JS_NewCFunctionData(dp->jsCtx, [](JSContext* cx, JSValue, int,
                                              JSValue*, int, JSValue* fd) -> JSValue {
                int64_t p = 0, id = 0;
                JS_ToInt64(cx, &p, fd[0]);
                JS_ToInt64(cx, &id, fd[1]);
                auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
                if (self) {
                    if (auto* d = self->iframeDocById(static_cast<uint64_t>(id)))
                        self->reloadIframe(d->element);
                }
                return JS_UNDEFINED;
            }, 0, 0, 2, fdata));
        JS_FreeValue(dp->jsCtx, fdata[0]);
        JS_FreeValue(dp->jsCtx, fdata[1]);
        JS_FreeValue(dp->jsCtx, global);
    }

    // Register before running scripts so the getContext factory + element hook
    // resolve during script execution.
    iframeDocs_.push_back(std::move(doc));
    el->setIframeDoc(dp);

    runSubDocScripts(ref, source, "iframe");
    finishSubDocLoad(ref, source, renderer_.get(), audioEngine_.get(), *textMetrics_);

    LOG_INFO("iframe: loaded sub-document '%s' (%dx%d, id=%llu)",
             source.appDir.c_str(), dp->boxW, dp->boxH,
             static_cast<unsigned long long>(dp->id));

    // Fire a non-bubbling "load" event on the host-side <iframe> element once the
    // sub-document is parsed, scripted, and laid out — the signal host code waits
    // on before it looks at or drives the embedded app. Dispatched on the host
    // document's context (the element lives in the host realm).
    dom::Event loadEvent("load", /*bubbles=*/false, /*cancelable=*/false);
    js::dispatchDomEvent(jsRuntime_->getContext(), el, loadEvent);
}

// Request an <iframe>'s sub-document be reloaded from its current src. This is
// the create→look→refine hook — host code rewrites the embedded app's files,
// then reload() to re-render them. Called from JS (iframe.reload() / src=), so
// it only QUEUES: tearing the sub-document down here would free JS/DOM/canvas
// state the raster thread may be mid-replay on (replayIframeLayers), a
// use-after-free. processPendingIframeReloads() does the real work at the
// raster-idle point in the frame loop.
void Engine::reloadIframe(dom::Element* el) {
    if (!el) return;
    if (std::find(pendingIframeReloads_.begin(), pendingIframeReloads_.end(), el)
        == pendingIframeReloads_.end())
        pendingIframeReloads_.push_back(el);
    // Ensure a frame runs and reaches the isRasterIdle block that drains the
    // queue (the host document may otherwise be idle).
    uiDirty_ = true;
}

// Drain queued iframe reloads: tear each sub-document down and rebuild it from
// src. MUST run only at the raster-idle point in the frame loop (next to
// recordIframeLayers) — it mutates iframeDocs_ and frees sub-doc state.
void Engine::processPendingIframeReloads() {
    if (pendingIframeReloads_.empty()) return;
    std::vector<dom::Element*> reloads;
    reloads.swap(pendingIframeReloads_);
    for (dom::Element* el : reloads) {
        if (!el) continue;
        std::string src = el->getAttribute("src");
        // An explicit reload()/src= is a request to retry, so drop any record of
        // a previous failure for this element.
        iframeLoadFailed_.erase(el);
        // Salvage the existing GPU surface across the reload: it is owned by the
        // raster renderer's GL context, so recreating it would leak the old one
        // (and destroying it here would be the wrong context). Hand it to the
        // rebuilt sub-doc; replayIframeLayers resizes it if the box changed, and
        // keeping its fboTexture means the preview shows the old frame until the
        // new one paints instead of flashing blank.
        render::SkiaRenderer::GPUSurface salvaged;
        int salvagedW = 0, salvagedH = 0;
        unsigned int salvagedTex = 0;
        bool haveSalvage = false;
        for (auto it = iframeDocs_.begin(); it != iframeDocs_.end(); ++it) {
            if ((*it)->element == el) {
                salvaged = std::move((*it)->surface);
                salvagedW = (*it)->surfW;
                salvagedH = (*it)->surfH;
                salvagedTex = (*it)->fboTexture;
                haveSalvage = true;
                teardownIframeDoc(it->get());
                iframeDocs_.erase(it);
                break;
            }
        }
        if (!src.empty()) {
            createIframeDoc(el, src);
            // Record a failed rebuild so syncIframes doesn't immediately retry it
            // on the next DOM mutation.
            if (!iframeDocForElement(el)) iframeLoadFailed_[el] = src;
        }
        if (!haveSalvage) continue;
        // Hand the surface to the rebuilt sub-doc — or, if there is no rebuilt
        // sub-doc (empty src, or createIframeDoc bailed on a missing/broken
        // app), give it back to the raster thread to destroy. Dropping it here
        // would leak the FBO and release Ganesh off-thread.
        if (IframeDoc* nd = src.empty() ? nullptr : iframeDocForElement(el)) {
            nd->surface = std::move(salvaged);
            nd->surfW = salvagedW;
            nd->surfH = salvagedH;
            nd->fboTexture = salvagedTex;
        } else {
            queueIframeSurfaceFree(std::move(salvaged));
        }
    }
}

// Main thread, raster-idle only. See the header for why an iframe surface can
// only be destroyed on the raster thread. Secondary-window hosts route their
// surfaces through the same queue — it is a per-CONTEXT free list, not an
// iframe-specific one.
void Engine::queueIframeSurfaceFree(render::SkiaRenderer::GPUSurface&& surf) {
    if (!surf.surface && !surf.fbo && !surf.texture) return;
    iframeSurfaceFrees_.push_back(std::move(surf));
}

// Raster thread.
void Engine::drainIframeSurfaceFrees(render::SkiaRenderer* renderer) {
    if (iframeSurfaceFrees_.empty()) return;
    if (renderer) {
        for (auto& s : iframeSurfaceFrees_) renderer->destroyGPUSurface(s);
    }
    iframeSurfaceFrees_.clear();
}

// Advance each iframe sub-document's timers + requestAnimationFrame callbacks,
// and report whether any sub-document needs (re)recording this frame. Iframe
// activity has no other path to uiDirty_ (the host document may be idle while a
// preview animates or was just reloaded), so surface it here — otherwise the
// compositor never records the sub-doc and its fboTexture stays 0, giving a
// blank preview and a null iframe.capture().
bool Engine::tickIframes(double nowMs) {
    if (iframeDocs_.empty()) return false;
    bool active = false;
    for (auto& d : iframeDocs_) {
        if (tickSubDoc(iframeSubDoc(*d), nowMs)) active = true;
    }
    jsRuntime_->executePendingJobs();
    return active;
}

// Tear down one iframe sub-document. Destroy order mirrors destroySystemPanels:
// timers → DOM bindings → document (fires Element finalizers into the still-live
// canvasScenes) → JSContext. canvasScenes (a member, declared before `document`)
// destruct when the owning IframeDoc unique_ptr is finally erased.
//
// Deliberately does NOT touch doc->surface: it belongs to whichever GL context
// replayed the sub-doc, which is not necessarily this thread's (see
// queueIframeSurfaceFree). Both callers already account for it —
// processPendingIframeReloads moves the surface out first, and destroyAllIframes
// runs from ~Engine() after the surfaces have been released on their owning
// context (the raster thread's exit cleanup windowed; ~Engine() itself, via the
// main renderer, headless).
void Engine::teardownIframeDoc(IframeDoc* doc) {
    if (!doc) return;
    if (doc->element) doc->element->setIframeDoc(nullptr);
    teardownSubDoc(iframeSubDoc(*doc));
}

void Engine::destroyAllIframes() {
    for (auto& d : iframeDocs_) teardownIframeDoc(d.get());
    iframeDocs_.clear();
}

} // namespace bro::engine
