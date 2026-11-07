// Engine canvas/scene/font/GPU subsystem initialization (split from engine_init.cpp).
#include "engine/engine.h"
#include "engine/app_loader.h"
#include "engine/gizmo.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "layout/box.h"
#include "render/renderer.h"
#include "render/skia_backend.h"
#include "render/vulkan_context.h"
#include "canvas/canvas_scene.h"
#include "webgl/webgl2_context.h"
#include "webgl/client/webgl_thread.h"
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#include "util/log.h"
#include "util/subresource_access.h"
#include <include/core/SkData.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <utility>

namespace bro::engine {

webgl::WebGL2RenderingContext* Engine::createWebGL2Context(dom::Element* canvas,
                                                           const webgl::ContextAttributes& attributes) {
    if (!vulkanContext_) return nullptr;

    if (canvas && canvas->webglContext()) {
        return static_cast<webgl::WebGL2RenderingContext*>(canvas->webglContext());
    }

    // The drawing buffer is the canvas's width/height attributes when it has
    // them (what syncWebGLCanvasSizes keeps it at every frame after), else
    // its laid-out box, else the viewport. Starting from the box or viewport
    // when the attributes say otherwise left a `canvas.width = 64` canvas
    // with a 1920x1080 buffer until the next frame, so a draw + readPixels
    // in the same turn saw a 64-pixel corner of a stretched image.
    int cw = viewportWidth_, ch = viewportHeight_;
    if (canvas) {
        auto& box = canvas->layoutBox();
        if (box.contentRect.width > 0) cw = static_cast<int>(box.contentRect.width);
        if (box.contentRect.height > 0) ch = static_cast<int>(box.contentRect.height);
        const auto attrInt = [canvas](const char* name, int fallback) {
            const std::string& v = canvas->getAttribute(name);
            const int n = v.empty() ? 0 : std::atoi(v.c_str());
            return n > 0 ? n : fallback;
        };
        cw = attrInt("width", cw);
        ch = attrInt("height", ch);
    }
    if (!webglThread_) webglThread_ = std::make_shared<webgl::WebGLThread>();
    auto ctx2 = std::make_unique<webgl::WebGL2RenderingContext>(cw, ch, *vulkanContext_, attributes, webglThread_);
    auto* webglCtx = ctx2.get();
    if (canvas) canvas->setWebglContext(webglCtx);
    webglEntries_.push_back({std::move(ctx2), canvas, canvas ? canvas->document() : nullptr,
                             canvas ? canvas->nodeId() : 0u});
    return webglCtx;
}

render::KernelCanvas* Engine::createKernelCanvas(dom::Element* canvas) {
    if (!vulkanContext_ || !canvas) return nullptr;
    if (canvas->kernelCanvas()) return static_cast<render::KernelCanvas*>(canvas->kernelCanvas());
    std::shared_ptr<render::KernelCanvasBlit> blit = kernelCanvasBlit_.lock();
    if (!blit) {
        blit = std::make_shared<render::KernelCanvasBlit>(*vulkanContext_);
        if (!blit->valid()) LOG_ERROR("kernel canvas: %s", blit->error().c_str());
        kernelCanvasBlit_ = blit;
    }
    auto display = std::make_unique<render::KernelCanvas>(*vulkanContext_, std::move(blit));
    render::KernelCanvas* out = display.get();
    canvas->setKernelCanvas(out);
    kernelCanvases_.push_back({std::move(display), canvas, canvas->document(), canvas->nodeId()});
    return out;
}

canvas::CanvasScene* Engine::createCanvasContext(dom::Element* canvas) {
    if (!canvas) return nullptr;
    if (canvas->canvasScene()) {
        return static_cast<canvas::CanvasScene*>(canvas->canvasScene());
    }

    auto canvasScene = std::make_unique<canvas::CanvasScene>(renderer_.get());
    // Seed the bitmap size only from width/height attributes that were
    // assigned before getContext() ran. A canvas WITHOUT them stays
    // layout-driven (intrinsic 0 → CanvasScene::queryLayoutWidth falls
    // through to the content box), which is what ctx.canvasWidth and the
    // arcade apps' full-viewport canvases depend on. Pinning 300x150 here
    // would stretch every attribute-less canvas to its CSS box.
    const std::string wAttr = canvas->getAttribute("width");
    const std::string hAttr = canvas->getAttribute("height");
    if (!wAttr.empty()) canvasScene->setIntrinsicWidth(std::atoi(wAttr.c_str()));
    if (!hAttr.empty()) canvasScene->setIntrinsicHeight(std::atoi(hAttr.c_str()));
    canvasScene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
        auto* elem = static_cast<dom::Element*>(ud);
        if (!elem->parentNode()) {
            ox = oy = ow = oh = 0;
            return;
        }
        dom::AbsoluteRect r = dom::absoluteContentBox(elem);
        ox = r.x; oy = r.y; ow = r.width; oh = r.height;
    }, canvas);
    canvasScene->setDetachedCallback([](void* ud) -> bool {
        auto* n = static_cast<dom::Element*>(ud);
        while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
        return n->tagName() != "html" && n->tagName() != "HTML";
    }, canvas);
    canvasScene->setLiveCheck([](void* doc, void* node) -> bool {
        return static_cast<dom::Document*>(doc)->isNodeLive(
            static_cast<dom::Element*>(node));
    }, canvas->document());

    auto* csPtr = canvasScene.get();
    canvas->setCanvasScene(csPtr, &canvas::CanvasScene::onBackingElementDestroyed);

    // A canvas in a sub-document (secondary window, <iframe>, system panel)
    // is parked in THAT document's list, unbound: those documents composite
    // by blitting the scene inline from their own draw path, which hands the
    // scene the Skia context it should raster on at blit time (sub_document.cpp
    // replayBufferWithInlineCanvas, system_panels.cpp drawSystemPanelDoc).
    dom::Document* doc = canvas->document();
    if (doc) {
        auto parkInSubDoc = [&](std::vector<std::unique_ptr<canvas::CanvasScene>>& list) {
            canvasSceneRegistry_[canvasScene->sceneId()] = csPtr;
            list.push_back(std::move(canvasScene));
        };
        for (auto& h : windowHosts_) {
            if (h && h->document.get() == doc) {
                parkInSubDoc(h->canvasScenes);
                return csPtr;
            }
        }
        for (auto& d : iframeDocs_) {
            if (d && d->document.get() == doc) {
                parkInSubDoc(d->canvasScenes);
                return csPtr;
            }
        }
        for (auto& s : systemDocs_) {
            if (s.document.get() == doc) {
                parkInSubDoc(s.canvasScenes);
                return csPtr;
            }
        }
    }

    // The main document: the one path that registers a scene the compositor
    // rasterizes — the registry and the raster binding. Registering by hand
    // here left every 2D canvas whose getContext ran after boot unbound in
    // windowed mode, so it painted nothing; the scene-context factory below
    // goes through addCanvasScene too.
    addCanvasScene(std::move(canvasScene));
    return csPtr;
}

#if BRO_WITH_3D
static void severSceneGraphLink(dom::Element* el);
#endif

scene::SceneGraph* Engine::createSceneContext(dom::Element* canvas) {
#if !BRO_WITH_3D
    (void)canvas;
    return nullptr;
#else
    if (!vulkanContext_) return nullptr;

    // A CanvasScene for `canvas` — the 2D layer a graph's sprites and shapes
    // draw into — registered with the compositor and linked to the element.
    auto bindSceneCanvas = [this](dom::Element* canvas) -> canvas::CanvasScene* {
        auto canvasScene = std::make_unique<canvas::CanvasScene>(renderer_.get());
        canvasScene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
            auto* elem = static_cast<dom::Element*>(ud);
            if (!elem->parentNode()) {
                ox = oy = ow = oh = 0;
                return;
            }
            dom::AbsoluteRect r = dom::absoluteContentBox(elem);
            ox = r.x; oy = r.y; ow = r.width; oh = r.height;
        }, canvas);
        canvasScene->setDetachedCallback([](void* ud) -> bool {
            auto* n = static_cast<dom::Element*>(ud);
            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
            return n->tagName() != "html" && n->tagName() != "HTML";
        }, canvas);
        canvasScene->setLiveCheck([](void* doc, void* node) -> bool {
            return static_cast<dom::Document*>(doc)->isNodeLive(
                static_cast<dom::Element*>(node));
        }, canvas->document());
        auto* csPtr = canvasScene.get();
        canvas->setCanvasScene(csPtr, &canvas::CanvasScene::onBackingElementDestroyed);
        addCanvasScene(std::move(canvasScene));
        return csPtr;
    };

    if (canvas && canvas->sceneGraph()) {
        if (auto* existing = sceneGraphForElement(canvas)) return existing;
    }
    if (!canvas) return nullptr;

    auto graph = std::make_unique<scene::SceneGraph>();
    graph->setPhysicsWorld(physicsWorld_.get());
    graph->setCanvasSize(viewportWidth_, viewportHeight_);
    graph->setDeviceScale(deviceScale_.render);
    auto* graphPtr = graph.get();
    graphPtr->setGizmoProvider([this](scene::SceneGraph* g) {
        return gizmo_ ? gizmo_->meshesForRender(g)
                      : std::vector<scene::MeshNode*>{};
    });
    // The one path that binds a graph to a canvas — at creation below, and
    // for SceneGraph.attachTo / detach / keepAlive afterwards.
    graphPtr->setCanvasRebinder([this, graphPtr, bindSceneCanvas](void* targetPtr) -> bool {
        auto* target = static_cast<dom::Element*>(targetPtr);
        auto it = std::find_if(sceneGraphs_.begin(), sceneGraphs_.end(),
            [graphPtr](const SceneGraphEntry& e) { return e.graph.get() == graphPtr; });
        if (it == sceneGraphs_.end()) return false;
        dom::Element* old = liveElementOf(*it);
        if (target && target == old && !graphPtr->parked()) return true;
        if (target && target->sceneGraph()) return false;  // another graph's canvas

        // Unbind: the old canvas stops compositing the graph, and the old
        // CanvasScene is retired (the frame loop's detached sweep drops it).
        // Nothing the nodes hold on the GPU is touched.
        severSceneGraphLink(old);
        if (auto* oldCs = graphPtr->canvasScene()) {
            if (old && old->canvasScene() == oldCs) old->setCanvasScene(nullptr);
            oldCs->onElementFinalized();
        }
        graphPtr->setCanvasScene(nullptr);
        graphPtr->setLayerCallback({});
        it->element = nullptr;
        it->document = nullptr;
        it->elementId = 0;
        graphPtr->setParked(true);
        if (!target) return true;

        auto* csPtr = bindSceneCanvas(target);
        graphPtr->setCanvasScene(csPtr);
        int cw = graphPtr->canvasWidth(), ch = graphPtr->canvasHeight();
        const auto& box = target->layoutBox();
        if (box.contentRect.width > 0) cw = static_cast<int>(box.contentRect.width);
        if (box.contentRect.height > 0) ch = static_cast<int>(box.contentRect.height);
        graphPtr->setCanvasSize(cw, ch);
        target->setSceneGraph(graphPtr);
        graphPtr->setLayerCallback([target](const render::LayerImage& layer) {
            target->setSceneLayerReady(static_cast<bool>(layer));
        });
        it->element = target;
        it->document = target->document();
        it->elementId = target->nodeId();
        graphPtr->setParked(false);
        return true;
    });
    sceneGraphs_.push_back({std::move(graph), nullptr, nullptr, 0});
    graphPtr->setParked(true);
    graphPtr->rebindCanvas(canvas);
    return graphPtr;
#endif  // BRO_WITH_3D
}

size_t Engine::sceneContextCount() const {
#if BRO_WITH_3D
    return sceneGraphs_.size();
#else
    return 0;
#endif
}

#if BRO_WITH_3D
dom::Element* Engine::liveElementOf(const SceneGraphEntry& entry) const {
    if (!entry.element || !entry.document) return nullptr;
    if (!dom::Document::isLiveDocument(entry.document)) return nullptr;
    auto* node = entry.document->resolveNode(entry.element, entry.elementId);
    return node ? static_cast<dom::Element*>(node) : nullptr;
}

static void severSceneGraphLink(dom::Element* el) {
    if (!el) return;
    el->setSceneGraph(nullptr);
    el->setSceneLayerReady(false);
}

void Engine::pruneDetachedSceneGraphs() {
    // A keepAlive graph whose canvas left the DOM (or died) is parked, not
    // destroyed: its GPU state waits for SceneGraph.attachTo on a new canvas.
    // Parked entries have no element, which the sweep below skips.
    for (auto& sg : sceneGraphs_) {
        if (!sg.element || !sg.graph || !sg.graph->keepAlive()) continue;
        dom::Element* el = liveElementOf(sg);
        bool detached = !el;
        if (el) {
            auto* n = el;
            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
            detached = n->tagName() != "html" && n->tagName() != "HTML";
        }
        if (detached) sg.graph->rebindCanvas(nullptr);
    }
    sceneGraphs_.erase(
        std::remove_if(sceneGraphs_.begin(), sceneGraphs_.end(),
            [this](SceneGraphEntry& sg) {
                if (!sg.element || !sg.document) return false;
                dom::Element* el = liveElementOf(sg);
                if (!el) return true;

                auto* n = el;
                while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                const bool detached =
                    n->tagName() != "html" && n->tagName() != "HTML";
                if (detached) severSceneGraphLink(el);
                return detached;
            }),
        sceneGraphs_.end());
}

void Engine::clearSceneGraphs() {
    for (auto& sg : sceneGraphs_) severSceneGraphLink(liveElementOf(sg));
    sceneGraphs_.clear();
}
#endif  // BRO_WITH_3D

dom::ListenerHandle Engine::addWindowEventListener(const std::string& type,
                                                   dom::EventCallback cb,
                                                   dom::ListenerOptions opts) {
    if (!document_) return dom::ListenerHandle{};
    return document_->windowListeners().add(type, std::move(cb), opts);
}

bool Engine::removeWindowEventListener(dom::ListenerHandle handle) {
    if (!document_) return false;
    return document_->windowListeners().remove(handle);
}

void Engine::resetMenuBarDefaults() {
    menuBar_.roots.clear();

    MenuBar::Item file;
    file.id = "file"; file.label = "File";
    MenuBar::Item quit;
    quit.id = "__system.quit"; quit.label = "Quit"; quit.accel = "Ctrl+Q";
    file.children.push_back(std::move(quit));

    MenuBar::Item edit;
    edit.id = "edit"; edit.label = "Edit";
    MenuBar::Item prefs;
    prefs.id = "__system.preferences"; prefs.label = "Preferences...";
    edit.children.push_back(std::move(prefs));

    MenuBar::Item view;
    view.id = "view"; view.label = "View";
    MenuBar::Item insp;
    insp.id = "__system.inspector"; insp.label = "Inspector";
    view.children.push_back(std::move(insp));

    menuBar_.roots.push_back(std::move(file));
    menuBar_.roots.push_back(std::move(edit));
    menuBar_.roots.push_back(std::move(view));
    menuBar_.dirty = true;
}

void Engine::loadCustomFonts() {
    if (!document_ || !renderer_) return;
    auto& fontFaces = document_->cascade().fontFaces();
    std::string basePath = document_->basePath();

    for (auto& ff : fontFaces) {
        bool alreadyLoaded = std::any_of(
            loadedFonts_.begin(), loadedFonts_.end(),
            [&](const LoadedFont& lf) {
                return lf.family == ff.family && lf.weight == ff.weight &&
                       lf.italic == ff.italic;
            });
        if (alreadyLoaded) continue;

        // src is relative to the sheet that declared the rule (its directory,
        // through @import too); a sheet with no base is the document's.
        std::string path = AppLoader::resolvePath(
            ff.baseUrl.empty() ? basePath : ff.baseUrl, ff.src, &assetMounts_);
        // The sheet named the file or URL: the host's access gate decides
        // whether it may be read or fetched (util/subresource_access.h).
        if (const std::string refused = util::subresourceRefusal(path); !refused.empty()) {
            LOG_WARN("@font-face '%s': %s", ff.family.c_str(), refused.c_str());
            continue;
        }

        // One copy of the bytes, shared by the UI, layout and raster
        // renderers. Mapped read-only where a mapping does not lock the file
        // (it would block saving the font during hot reload on Windows), so
        // its pages are page cache, shared with every process using the font.
#ifdef _WIN32
        sk_sp<SkData> bytes;
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (file.is_open()) {
                const auto size = static_cast<size_t>(file.tellg());
                file.seekg(0, std::ios::beg);
                auto owned = SkData::MakeUninitialized(size);
                if (file.read(static_cast<char*>(owned->writable_data()), static_cast<std::streamsize>(size)))
                    bytes = std::move(owned);
            }
        }
#else
        sk_sp<SkData> bytes = SkData::MakeFromFileName(path.c_str());
#endif
        if (!bytes || bytes->size() == 0) {
            LOG_WARN("Failed to load @font-face '%s' from '%s'", ff.family.c_str(), path.c_str());
            continue;
        }
        const void* data = bytes->data();
        const size_t size = bytes->size();
        std::shared_ptr<const void> keep(data, [bytes](const void*) mutable { bytes.reset(); });

        if (renderer_->registerCustomFont(ff.family, {keep, data, size}, ff.weight, ff.italic)) {
            LOG_INFO("Loaded @font-face '%s' from '%s'", ff.family.c_str(), path.c_str());
            loadedFonts_.push_back({ff.family, std::move(keep), data, size, ff.weight, ff.italic});
        }
    }
}
} // namespace bro::engine

namespace bro::engine {

// Skia's GPU context on the engine's device. Null when Skia cannot run on the
// device, which the caller makes fatal: every layer is drawn on the GPU.
render::SkiaGpu* Engine::createSkiaGpu() {
    if (!vulkanContext_) return nullptr;
    auto gpu = std::make_unique<render::SkiaGpu>(*vulkanContext_);
    if (!gpu->init()) return nullptr;
    skiaGpu_ = std::move(gpu);
    return skiaGpu_.get();
}

} // namespace bro::engine
