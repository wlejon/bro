// Engine <iframe> sub-document hosting — embedded, isolated documents rendered
// into an <iframe> element's box. Structurally mirrors the system-panel
// sub-document mechanism (system_panels.cpp) but binds each sub-document to a
// DOM element in the app document and lays it out at that element's content box
// instead of an engine overlay slot. Each iframe is a full, isolated bro app:
// its own JS realm, DOM tree, timers, and canvas scenes.

#include "engine/engine.h"
#include "engine/default_styles.h"
#include "engine/app_loader.h"
#include "engine/replaced_elements.h"
#include "engine/settings.h"
#include "util/platform.h"
#include "util/log.h"
#include "layout/box.h"
#include "layout/skia_text_metrics.h"
#include "render/renderer.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/event.h"
#include "dom/element_geometry.h"
#include "js/runtime.h"
#include "js/event_dispatch.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/canvas_bindings.h"
#include "js/image_bindings.h"
#include "js/imagebitmap_bindings.h"
#include "js/window_bindings.h"
#include "js/settings_bindings.h"
#include "js/storage_bindings.h"
#include "canvas/canvas_scene.h"
#include "api/api.h"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

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

// Reconcile iframeDocs_ with the <iframe> elements currently in the app document.
void Engine::syncIframes() {
    if (!document_) return;

    std::vector<dom::Element*> frames;
    collectIframes(document_->documentElement(), frames);

    // Tear down sub-docs whose <iframe> element has left the tree.
    for (auto it = iframeDocs_.begin(); it != iframeDocs_.end();) {
        bool present = false;
        for (auto* f : frames) if (f == (*it)->element) { present = true; break; }
        if (!present) { teardownIframeDoc(it->get()); it = iframeDocs_.erase(it); }
        else ++it;
    }

    // Create sub-docs for new src'd iframes. (Reload-on-src-change is handled by
    // the JS handle in a later step.)
    for (auto* f : frames) {
        std::string src = f->getAttribute("src");
        if (src.empty()) continue;
        if (iframeDocForElement(f)) continue;
        createIframeDoc(f, src);
    }
}

void Engine::createIframeDoc(dom::Element* el, const std::string& srcAttr) {
    // Resolve src against the host app's base path.
    std::string resolved = AppLoader::resolvePath(manifest_.basePath, srcAttr, &assetMounts_);

    // An iframe hosts a directory-based bro app (index.html + styles + scripts),
    // exactly like a top-level `bro <dir>`. If src points at a file, use its dir.
    std::error_code ec;
    std::string appDir = resolved;
    if (!fs::is_directory(resolved, ec))
        appDir = fs::path(resolved).parent_path().string();

    AppManifest manifest = AppLoader::loadApp(appDir, &assetMounts_);
    std::string html = AppLoader::loadFile(manifest.htmlPath);
    if (html.empty()) {
        LOG_ERROR("iframe: no index.html at '%s' (src='%s')", appDir.c_str(), srcAttr.c_str());
        return;
    }

    auto doc = std::make_unique<IframeDoc>();
    IframeDoc* dp = doc.get();
    dp->element = el;
    dp->id = nextIframeId_++;
    dp->src = resolved;

    // Content-box size from the laid-out <iframe> element (fallback 300x150).
    auto& lbox = el->layoutBox();
    dp->boxW = lbox.contentRect.width  > 0 ? static_cast<int>(lbox.contentRect.width)  : 300;
    dp->boxH = lbox.contentRect.height > 0 ? static_cast<int>(lbox.contentRect.height) : 150;

    // Author styles.
    std::string authorStyles;
    for (auto& p : manifest.stylePaths) {
        std::string css = AppLoader::loadFile(p);
        if (!css.empty()) authorStyles += css + "\n";
    }

    // Parse HTML (extract <template> blocks first, like the top-level app path).
    std::vector<dom::Document::TemplateBlock> templates;
    html = dom::Document::extractTemplates(html, templates);
    dp->document = std::make_unique<dom::Document>();
    dp->document->setBasePath(manifest.basePath);
    dp->document->parse(html, authorStyles, kDefaultStyles);
    if (!templates.empty()) dp->document->injectTemplates(templates);

    // Dedicated JS realm + standard app bindings, isolated from the host app.
    dp->jsCtx = jsRuntime_->createContext();
    brokit::api::installConsole(dp->jsCtx);
    dp->timers = std::make_unique<js::Timers>();
    js::Timers::install(dp->jsCtx, dp->timers.get());
    js::installWindowBindings(dp->jsCtx, dp->boxW, dp->boxH);
    js::DomBindings::install(dp->jsCtx, dp->document.get());
    js::StorageBindings::install(dp->jsCtx, manifest.basePath + "/.storage.json");
    js::StorageBindings::installSessionStorage(dp->jsCtx);
    if (settings_)
        js::SettingsBindings::install(dp->jsCtx, settings_.get(),
                                      window_ ? window_.get() : nullptr);
    js::CanvasBindings::install(dp->jsCtx);
    js::ImageBindings::install(dp->jsCtx, manifest.basePath);
    js::ImageBitmapBindings::install(dp->jsCtx);

    // Canvas 2D getContext factory — parks each <canvas>'s scene in THIS sub-doc.
    // dp is stable (heap unique_ptr), so it's captured directly.
    js::DomBindings::setGetContextFactory(dp->jsCtx,
        [this, dp](JSContext* fctx, dom::Element* cel, const std::string& type) -> JSValue {
            if (type != "2d") return JS_NULL;
            auto scene = std::make_unique<canvas::CanvasScene>(renderer_.get());
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
                scene->setLiveCheck([](void* d, void* node) -> bool {
                    return static_cast<dom::Document*>(d)->isNodeLive(
                        static_cast<dom::Element*>(node));
                }, cel->document());
            }
            auto* ptr = scene.get();
            if (cel) cel->setCanvasScene(ptr, &canvas::CanvasScene::onBackingElementDestroyed);
            dp->canvasScenes.push_back(std::move(scene));
            return js::CanvasBindings::wrapContext2D(fctx, ptr);
        });

    // Register before running scripts so the getContext factory + element hook
    // resolve during script execution.
    iframeDocs_.push_back(std::move(doc));
    el->setIframeDoc(dp);

    // Run scripts on the sub-doc's own context. Each iframe has its own JSContext
    // (not the runtime's shared module context), so only classic scripts are
    // supported for now — type="module" is skipped with a warning.
    for (auto& s : manifest.scripts) {
        if (s.isModule) {
            LOG_WARN("iframe '%s': <script type=module> not yet supported, skipping",
                     srcAttr.c_str());
            continue;
        }
        std::string code = s.isInline() ? s.code : AppLoader::loadFile(s.path);
        if (code.empty()) continue;
        std::string fname = s.isInline() ? (manifest.basePath + "/<inline>") : s.path;
        JSValue r = JS_Eval(dp->jsCtx, code.c_str(), code.size(), fname.c_str(),
                            JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            JSValue ex = JS_GetException(dp->jsCtx);
            const char* str = JS_ToCString(dp->jsCtx, ex);
            if (str) {
                LOG_ERROR("iframe '%s' JS error: %s", srcAttr.c_str(), str);
                JS_FreeCString(dp->jsCtx, str);
            }
            JS_FreeValue(dp->jsCtx, ex);
        }
        JS_FreeValue(dp->jsCtx, r);
    }

    // Replaced elements + style/layout at the box size. Namespace-qualified to
    // reach the free function, not Engine's 1-arg member of the same name.
    bro::engine::ensureReplacedElements(dp->document->documentElement(), renderer_.get(),
                                        dp->jsCtx, audioEngine_.get());
    dp->document->resolveStyles();
    dp->document->performLayout(static_cast<float>(dp->boxW),
                                static_cast<float>(dp->boxH), *textMetrics_);
    dp->document->setBasePath(manifest.basePath);

    LOG_INFO("iframe: loaded sub-document '%s' (%dx%d, id=%llu)",
             appDir.c_str(), dp->boxW, dp->boxH,
             static_cast<unsigned long long>(dp->id));

    // Fire a non-bubbling "load" event on the host-side <iframe> element once the
    // sub-document is parsed, scripted, and laid out — the signal host code waits
    // on before it looks at or drives the embedded app. Dispatched on the host
    // document's context (the element lives in the host realm).
    dom::Event loadEvent("load", /*bubbles=*/false, /*cancelable=*/false);
    js::dispatchDomEvent(jsRuntime_->getContext(), el, loadEvent);
}

// Reload an <iframe>'s sub-document from its current src attribute: tear the old
// sub-document down and build a fresh one. This is the create→look→refine hook —
// host code rewrites the embedded app's files, then reload() to re-render them.
void Engine::reloadIframe(dom::Element* el) {
    if (!el) return;
    for (auto it = iframeDocs_.begin(); it != iframeDocs_.end(); ++it) {
        if ((*it)->element == el) {
            teardownIframeDoc(it->get());
            iframeDocs_.erase(it);
            break;
        }
    }
    std::string src = el->getAttribute("src");
    if (!src.empty()) createIframeDoc(el, src);
}

// Advance each iframe sub-document's timers + requestAnimationFrame callbacks.
void Engine::tickIframes(double nowMs) {
    if (iframeDocs_.empty()) return;
    for (auto& d : iframeDocs_) {
        if (!d->timers) continue;
        d->timers->tick(nowMs);
        d->timers->fireAnimationFrames(nowMs);
    }
    jsRuntime_->executePendingJobs();
}

// Tear down one iframe sub-document. Destroy order mirrors destroySystemPanels:
// timers → DOM bindings → document (fires Element finalizers into the still-live
// canvasScenes) → JSContext. canvasScenes (a member, declared before `document`)
// destruct when the owning IframeDoc unique_ptr is finally erased.
void Engine::teardownIframeDoc(IframeDoc* doc) {
    if (!doc) return;
    if (doc->element) doc->element->setIframeDoc(nullptr);
    if (doc->timers && doc->jsCtx) doc->timers->clearAll(doc->jsCtx);
    doc->timers.reset();
    if (doc->jsCtx) js::DomBindings::cleanup(doc->jsCtx);
    doc->document.reset();
    if (doc->jsCtx) { JS_FreeContext(doc->jsCtx); doc->jsCtx = nullptr; }
}

void Engine::destroyAllIframes() {
    for (auto& d : iframeDocs_) teardownIframeDoc(d.get());
    iframeDocs_.clear();
}

} // namespace bro::engine
