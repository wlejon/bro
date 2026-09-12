#include "engine/engine.h"
#include "engine/capture_path.h"
#include "engine/overflow.h"
#include "engine/navmesh_subsystem.h"
#include "engine/scene_audio_sync.h"
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#if BRO_WITH_BRONZE
#include "bronze_host/eval.h"
#include "bronze_host/host_window_open.h"
#endif
#include "audio_inference/audio_inference.h"

#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "render/command_buffer.h"
#if BRO_WITH_NET
#include "net/net_service.h"
#endif
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "canvas/canvas_scene.h"
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#include "webgl/webgl2_context.h"

#include <broaudio/engine.h>
#include "broimage/encode.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>

#include <glad/gl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bro::engine {

void Engine::flush() {
    pumpVideoEvents();

    if (document_) {
        document_->setTransitionManager(&transitionManager_, engineNowMs_);
        animationManager_.setKeyframes(&document_->cascade().keyframes());
        document_->setAnimationManager(&animationManager_);
        document_->setWebAnimationManager(&webAnimationManager_);
        bool animActive = transitionManager_.tick(engineNowMs_) |
                          animationManager_.tick(engineNowMs_) |
                          webAnimationManager_.tick(engineNowMs_);
        if (animActive) {
            document_->markPaintDirty();
            uiDirty_ = true;
        }
    }

    if (document_ && (document_->isDirty() || !document_->layoutRoot())) {
        if (document_->isStructureDirty()) {
            ensureReplacedElements(document_->documentElement());
            iframeSyncNeeded_ = true;
        }

        dom::Element* previousHover = hoveredElement_.get();
        layout::ElementRefAdapter::setHoveredElement(previousHover);
        document_->resolveStyles();

        if (document_->isLayoutDirty() || document_->isStructureDirty() || !document_->layoutRoot()) {
            document_->performLayout(static_cast<float>(viewportWidth_),
                                     static_cast<float>(contentHeight()),
                                     *textMetrics_);
            if (document_->documentElement()) {
                auto& box = document_->documentElement()->layoutBox();
                documentHeight_ = box.marginBox().height;
            }
        }

        document_->clearDirty();
        document_->markPaintDirty();

        if (document_->documentElement()) {
            std::vector<dom::Element*> reclamped;
            if (clampScrollOffsets(document_->documentElement(), &reclamped)) {
                for (auto* elem : reclamped) dispatchScrollEvent(elem);
                markAppBaseDirty();
            }
        }
    }

    processPendingIframeReloads();
    processPendingWindowHosts();
#if BRO_WITH_BRONZE
    bro::bronze_host::drainHostWindowMessages();
#endif

    if (iframeSyncNeeded_) {
        syncIframes();
        iframeSyncNeeded_ = false;
    }

    syncAllIframeBoxes();
    deliverMediaQueryChangesAllRealms();

    for (auto& ev : transitionManager_.takePendingEvents()) {
        dom::TransitionEvent tevt(ev.type, true, false);
        tevt.setPropertyName(ev.name);
        tevt.setElapsedTime(ev.elapsedTime);
        tevt.setIsTrusted(true);
        dispatchEvent(ev.element, tevt);
    }
    for (auto& ev : animationManager_.takePendingEvents()) {
        dom::AnimationEvent aevt(ev.type, true, false);
        aevt.setAnimationName(ev.name);
        aevt.setElapsedTime(ev.elapsedTime);
        aevt.setIsTrusted(true);
        dispatchEvent(ev.element, aevt);
    }
    (void)webAnimationManager_.takeFinishedEvents();

    if (document_ && !document_->isStructureDirty()) {
        document_->drainPendingFrees();
    }

    recordIframeLayers();
    recordWindowHostLayers();

    if (isSystemVisible()) {
        layoutSystemPanels(*textMetrics_);
    }

#if BRO_WITH_3D
    if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
        for (auto& sg : sceneGraphs_) {
            if (sg.graph) sg.graph->materializeHtmlNodes(skia);
        }
    }
    const bool gpuTiming = gl_ && !sceneGraphs_.empty() &&
                           dynamic_cast<render::SkiaRenderer*>(renderer_.get());
    if (gpuTiming) {
        if (gpuTimerQuery_ == 0) glGenQueries(1, &gpuTimerQuery_);
        glBeginQuery(GL_TIME_ELAPSED, gpuTimerQuery_);
    }
    for (auto& sg : sceneGraphs_) {
        if (sg.element) {
            auto& box = sg.element->layoutBox();
            int ew = static_cast<int>(box.contentRect.width);
            int eh = static_cast<int>(box.contentRect.height);
            if (ew > 0 && eh > 0 &&
                (ew != sg.graph->canvasWidth() || eh != sg.graph->canvasHeight())) {
                sg.graph->setCanvasSize(ew, eh);
            }
        }
        if (sg.graph) sg.graph->render();
    }
    if (gpuTiming) { glEndQuery(GL_TIME_ELAPSED); gpuTimerPending_ = true; }

    pruneDetachedSceneGraphs();
#endif  // BRO_WITH_3D

    webglEntries_.erase(
        std::remove_if(webglEntries_.begin(), webglEntries_.end(),
            [](auto& entry) {
                if (!entry.element) return false;
                auto* n = entry.element;
                while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                return n->tagName() != "html" && n->tagName() != "HTML";
            }),
        webglEntries_.end());

    for (auto& cs : canvasScenes_) {
        cs->rasterize(gl_.get());
        if (!cs->isDetached()) continue;
        canvasSceneRegistry_.erase(cs->sceneId());
        if (auto* el = static_cast<dom::Element*>(cs->backingElement()))
            el->setCanvasScene(nullptr);
    }
    canvasScenes_.erase(
        std::remove_if(canvasScenes_.begin(), canvasScenes_.end(),
            [](auto& cs) { return cs->isDetached(); }),
        canvasScenes_.end());
}

void Engine::advanceTime(double ms) {
    webgl::WebGL2RenderingContext* activeWebGL = nullptr;
    if (!webglEntries_.empty()) activeWebGL = webglEntries_[0].context.get();

    double remaining = ms;
    while (remaining > 0) {
        double step = std::min(remaining, 16.0);
        virtualTime_ += step;
        remaining -= step;

        double scaledStep = step * effectiveTimeScale();
        engineNowMs_ += scaledStep;

        drainWheelSmoothing(static_cast<float>(step) / 1000.0f);

        syncWebGLCanvasSizes();
        webgl::WebGL2RenderingContext::invalidateCurrent();
        if (activeWebGL) activeWebGL->bindCanvasFBO();

        if (!timePaused_) fireFrameCallbacks(scaledStep);

        if (audioInference_) audioInference_->stepInline();
        for (auto& pump : framePumps_) pump();

        tickSystemPanels(virtualTime_);

        if (!timePaused_) tickIframes(engineNowMs_);
        if (!timePaused_) tickWindowHosts(engineNowMs_);

        webgl::WebGL2RenderingContext::endAppGL();

#if BRO_WITH_PHYSICS
        if (physicsWorld_) {
            double stepMs = physicsWorld_->timeStep() * 1000.0;
            physicsAccumMs_ += scaledStep;
            int safety = 16;
            while (physicsAccumMs_ + 0.5 >= stepMs && safety-- > 0) {
                physicsAccumMs_ -= stepMs;
                physicsWorld_->stepInline();
            }
            physicsWorld_->setRenderAlpha(
                stepMs > 0.0 ? static_cast<float>(physicsAccumMs_ / stepMs) : 1.0f);
        }
#endif

#if BRO_WITH_3D
#if BRO_WITH_PHYSICS
        for (auto& sg : sceneGraphs_) sg.graph->syncPhysics();
#endif
#endif

        bro::engine::pumpNavMeshObstacles(static_cast<float>(scaledStep * 0.001));

#if BRO_WITH_3D
        {
            float aiDt = static_cast<float>(scaledStep * 0.001);
            for (auto& sg : sceneGraphs_) {
                sg.graph->syncAgents(aiDt);
                sg.graph->tickAnimations(aiDt);
            }
            SceneAudioSync::sync(aiDt);
        }

        auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
        if (skia) {
            for (auto& sg : sceneGraphs_) {
                if (sg.graph) {
                    sg.graph->materializeHtmlNodes(skia);
                }
            }
        }

        for (auto& sg : sceneGraphs_) {
            sg.graph->render();
        }
#endif

        flush();

        if (audioEngine_) {
            int audioFrames = static_cast<int>(step * audioEngine_->sampleRate() / 1000.0 + 0.5);
            if (audioFrames > 0)
                audioEngine_->renderBlock(audioFrames);
        }
    }
}

std::string Engine::eval(const std::string& code) {
#if BRO_WITH_BRONZE
    bool ok = bro::bronze_host::evalScript(*this, code);
    flush();
    if (!ok) {
        setTestFailure(true);
        return "error";
    }
    return "";
#else
    (void)code;
    flush();
    return "";
#endif
}

std::vector<uint8_t> Engine::renderUnifiedToPixels() {
    if (!document_ || !gl_) return {};
    auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
    if (!skia) return {};

    int w = viewportWidth_, h = viewportHeight_;

    webgl::WebGL2RenderingContext* activeWebGL = nullptr;
    if (!webglEntries_.empty()) activeWebGL = webglEntries_[0].context.get();
    syncWebGLCanvasSizes();
    webgl::WebGL2RenderingContext::invalidateCurrent();
    if (activeWebGL) activeWebGL->bindCanvasFBO();

    webgl::WebGL2RenderingContext::endAppGL();

#if BRO_WITH_3D
    for (auto& sg : sceneGraphs_) {
        if (sg.graph) sg.graph->materializeHtmlNodes(skia);
    }
    for (auto& sg : sceneGraphs_) {
        if (sg.graph) sg.graph->render();
    }
#endif

    for (auto& cs : canvasScenes_) {
        cs->setViewportScroll(scrollY_);
        cs->rasterize(gl_.get());
    }
    canvasScenes_.erase(
        std::remove_if(canvasScenes_.begin(), canvasScenes_.end(),
            [](auto& cs) { return cs->isDetached(); }),
        canvasScenes_.end());

    if (isSystemVisible()) tickSystemPanels(virtualTime_);

    std::vector<UILayer> appLayers, systemLayers;
    render::CommandBuffer appCmds, sysCmds;
    int insetTop = contentTop();
    int cw = std::max(1, w - contentRight());
    int ch = std::max(1, h - insetTop - contentBottom());

    updateSelectionSnapshot();
    skia->beginFrame(w, h);
    recordAppLayers(appCmds, w, h,
                    insetTop, contentRight(), contentBottom(), scrollY_);
    recordSystemPanelLayers(sysCmds, w, h);
    recordIframeLayers();
    replayAppLayers(skia, appCmds,
                    screenshotHtmlPool_, screenshotHtmlPoolW_, screenshotHtmlPoolH_,
                    cw, ch, appLayers);
    replaySystemPanelLayers(skia, sysCmds,
                            screenshotSystemPool_, screenshotSystemPoolW_,
                            screenshotSystemPoolH_,
                            w, h, systemLayers);
    replayIframeLayers(skia);
    skia->endFrame();

    GLuint compositeFBO = 0, compositeTex = 0;
    glGenFramebuffers(1, &compositeFBO);
    compositeTex = gl_->createTexture2D(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, compositeTex, 0);

    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    compositeLayers(appLayers, compositeFBO, insetTop, cw, ch);
    compositeLayers(systemLayers, compositeFBO);

    std::vector<uint8_t> pixels(w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &compositeFBO);
    gl_->deleteTexture(compositeTex);

    if (activeWebGL) activeWebGL->restoreState();

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

bool Engine::screenshot(const std::string& path) {
    if (!document_) return false;
    if (!ensureParentDir(path)) return false;

    if (gl_ && dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
        auto pixels = renderUnifiedToPixels();
        if (pixels.empty()) return false;
        int w = viewportWidth_, h = viewportHeight_;
        return broimage::encode_png_file(path, pixels.data(), w, h, 4);
    }

    {
        webgl::WebGL2RenderingContext* activeWebGL = nullptr;
        if (!webglEntries_.empty()) activeWebGL = webglEntries_[0].context.get();
        syncWebGLCanvasSizes();
        webgl::WebGL2RenderingContext::invalidateCurrent();
        if (activeWebGL) activeWebGL->bindCanvasFBO();
        if (!timePaused_) fireFrameCallbacks(0.0);
        webgl::WebGL2RenderingContext::endAppGL();
    }

    renderer_->beginFrame(viewportWidth_, viewportHeight_);
    renderer_->clear({0, 0, 0, 255});

    renderer_->save();
    renderer_->translate(0.0f, static_cast<float>(contentTop()));

    for (auto& cs : canvasScenes_) {
        float cx, cy, cw, ch;
        cs->getScreenRect(cx, cy, cw, ch);
        if (cs->surface()) {
            auto* appCanvas = renderer_->getCanvas();
            if (appCanvas) {
                sk_sp<SkImage> img = cs->surface()->makeImageSnapshot();
                if (img) {
                    SkPaint paint;
                    paint.setBlendMode(SkBlendMode::kSrcOver);
                    appCanvas->drawImage(img, cx, cy, SkSamplingOptions(), &paint);
                }
            }
        }
    }

    drawTraversal_->draw(document_->documentElement(),
                         0, -scrollY_,
                         contentWidth(), contentHeight(), /*viewportTop=*/0);

    updateSelectionSnapshot();
    drawSelectionHighlight(renderer_.get(), -scrollY_);

    renderer_->restore();

    if (isSystemVisible()) {
        tickSystemPanels(virtualTime_);
        layoutSystemPanels(*textMetrics_);
        drawSystemPanels(renderer_.get(), *drawTraversal_);
    }

    renderer_->endFrame();

    return renderer_->saveScreenshot(path);
}

std::vector<uint8_t> Engine::capturePixels() {
    if (!document_) return {};

    if (gl_ && dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
        return renderUnifiedToPixels();
    }

    {
        webgl::WebGL2RenderingContext* activeWebGL = nullptr;
        if (!webglEntries_.empty()) activeWebGL = webglEntries_[0].context.get();
        syncWebGLCanvasSizes();
        webgl::WebGL2RenderingContext::invalidateCurrent();
        if (activeWebGL) activeWebGL->bindCanvasFBO();
        if (!timePaused_) fireFrameCallbacks(0.0);
        webgl::WebGL2RenderingContext::endAppGL();
    }

    renderer_->beginFrame(viewportWidth_, viewportHeight_);
    renderer_->clear({0, 0, 0, 255});

    renderer_->save();
    renderer_->translate(0.0f, static_cast<float>(contentTop()));

    for (auto& cs : canvasScenes_) {
        float cx, cy, cw, ch;
        cs->getScreenRect(cx, cy, cw, ch);
        if (cs->surface()) {
            auto* appCanvas = renderer_->getCanvas();
            if (appCanvas) {
                sk_sp<SkImage> img = cs->surface()->makeImageSnapshot();
                if (img) {
                    SkPaint paint;
                    paint.setBlendMode(SkBlendMode::kSrcOver);
                    appCanvas->drawImage(img, cx, cy, SkSamplingOptions(), &paint);
                }
            }
        }
    }

    drawTraversal_->draw(document_->documentElement(),
                         0, -scrollY_,
                         contentWidth(), contentHeight(), /*viewportTop=*/0);

    overlayMgr_.drawIfContext(OverlayContext::App, renderer_.get());

    renderer_->restore();

    if (isSystemVisible()) {
        tickSystemPanels(virtualTime_);
        layoutSystemPanels(*textMetrics_);
        drawSystemPanels(renderer_.get(), *drawTraversal_);
    }

    renderer_->endFrame();
    return renderer_->capturePixels();
}

bool Engine::screenshot(const std::string& path, int cx, int cy, int cw, int ch) {
    auto pixels = capturePixels();
    if (pixels.empty()) return false;

    int fw = viewportWidth_, fh = viewportHeight_;

    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > fw) cw = fw - cx;
    if (cy + ch > fh) ch = fh - cy;
    if (cw <= 0 || ch <= 0) return false;

    std::vector<uint8_t> cropped(cw * ch * 4);
    for (int y = 0; y < ch; ++y) {
        const uint8_t* src = pixels.data() + ((cy + y) * fw + cx) * 4;
        uint8_t* dst = cropped.data() + y * cw * 4;
        memcpy(dst, src, cw * 4);
    }

    if (!ensureParentDir(path)) return false;
    return broimage::encode_png_file(path, cropped.data(), cw, ch, 4);
}

double Engine::gpuFrameMs() {
    if (gpuTimerQuery_ == 0 || !gpuTimerPending_) return lastGpuFrameMs_;
    GLuint64 elapsedNs = 0;
    glGetQueryObjectui64v(gpuTimerQuery_, GL_QUERY_RESULT, &elapsedNs);
    gpuTimerPending_ = false;
    lastGpuFrameMs_ = static_cast<double>(elapsedNs) / 1.0e6;
    return lastGpuFrameMs_;
}

dom::Element* Engine::querySelector(const std::string& selector) const {
    if (!document_) return nullptr;

    if (!selector.empty() && selector[0] == '#') {
        return document_->getElementById(selector.substr(1));
    }

    return document_->querySelector(selector);
}

void Engine::dispatchClickOn(dom::Element* target) {
    if (!target) return;
    if (document_) document_->setActiveElement(target);
    dom::MouseEvent event("click");
    event.setIsTrusted(true);
    dispatchEvent(target, event);
}

#if BRO_WITH_3D
scene::CullStats Engine::sceneCullStats() const {
    scene::CullStats sum;
    for (const auto& sg : sceneGraphs_) {
        if (!sg.graph) continue;
        const scene::CullStats& s = sg.graph->cullStats();
        sum.meshDrawn        += s.meshDrawn;
        sum.meshCulled       += s.meshCulled;
        sum.instancedDrawn   += s.instancedDrawn;
        sum.instancedCulled  += s.instancedCulled;
        sum.splatDrawn       += s.splatDrawn;
        sum.splatCulled      += s.splatCulled;
        sum.particlesDrawn   += s.particlesDrawn;
        sum.particlesCulled  += s.particlesCulled;
        sum.billboardsDrawn  += s.billboardsDrawn;
        sum.billboardsCulled += s.billboardsCulled;
        sum.shadowDrawn         += s.shadowDrawn;
        sum.shadowCulled        += s.shadowCulled;
        sum.shadowTilesTotal    += s.shadowTilesTotal;
        sum.shadowTilesRendered += s.shadowTilesRendered;
        sum.shadowTilesCached   += s.shadowTilesCached;
    }
    return sum;
}
#endif

} // namespace bro::engine
