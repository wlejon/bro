// Engine headless/capture API methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
#include "render/cpu_raster_renderer.h"

#include "observer_check.js.h"

#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/event_dispatch.h"
#include "js/worker.h"
#include "js/net_bindings.h"
#include "net/net_service.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "canvas/canvas_scene.h"
#include "scene/scene_graph.h"
#include "webgl/webgl2_context.h"

#include <broaudio/engine.h>

#include <stb_image_write.h>

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>

#include <glad/gl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Headless API
// ---------------------------------------------------------------------------

void Engine::flush() {
    jsRuntime_->executePendingJobs();
    // Tick transitions and animations, mark dirty if any are active
    if (document_) {
        document_->setTransitionManager(&transitionManager_, virtualTime_);
        animationManager_.setKeyframes(&document_->cascade().keyframes());
        document_->setAnimationManager(&animationManager_);
        bool animActive = transitionManager_.tick(virtualTime_) | animationManager_.tick(virtualTime_);
        if (animActive) {
            document_->markDirty();
        }
    }
    // Re-layout any dirty system panel docs so subsequent hit-tests see the
    // current DOM state (click() fires events which can mutate system panel
    // DOM — e.g. bro.menu dropdowns — and we need layout fresh before the
    // next click tests against it).
    if (systemRenderer_) {
        for (auto& sdoc : systemDocs_) {
            if (!isSystemDocVisible(sdoc) || !sdoc.document) continue;
            if (!sdoc.document->isDirty()) continue;
            layout::SkiaTextMetrics sysMetrics(systemRenderer_.get(), sdoc.fontManager.get());
            sdoc.document->resolveStyles();
            sdoc.document->performLayout(static_cast<float>(viewportWidth_), sysMetrics);
            sdoc.document->clearDirty();
            systemDirty_ = true;
        }
    }

    if (document_ && document_->isDirty()) {
        if (document_->isStructureDirty()) {
            ensureReplacedElements(document_->documentElement());
        }
        layout::ElementRefAdapter::setHoveredElement(hoveredElement_);
        document_->resolveStyles();
        // performLayout() rebuilds the persistent layout tree when
        // structureDirty_ is set and clears the flag itself.
        document_->performLayout(static_cast<float>(viewportWidth_),
                                 static_cast<float>(contentHeight()), *textMetrics_);
        document_->clearDirty();

        // Notify ResizeObserver / IntersectionObserver after layout
        if (jsRuntime_) {
            JSValue r = JS_Eval(jsRuntime_->getContext(), js_observer_check,
                                strlen(js_observer_check), "<observer-check>", JS_EVAL_TYPE_GLOBAL);
            JS_FreeValue(jsRuntime_->getContext(), r);
        }
    }

    // Drain CSS transition/animation events and dispatch to JS.
    {
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
    }

    // Prune detached scene graphs (elements removed from DOM).
    // Must happen before canvas scene pruning so the scene graph releases
    // its canvasScene_ pointer before the CanvasScene is destroyed.
    sceneGraphs_.erase(
        std::remove_if(sceneGraphs_.begin(), sceneGraphs_.end(),
            [](auto& sg) {
                if (!sg.element) return false;
                auto* n = sg.element;
                while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                return n->tagName() != "html" && n->tagName() != "HTML";
            }),
        sceneGraphs_.end());

    // Prune detached canvas scenes (elements removed from DOM)
    for (auto& cs : canvasScenes_) {
        cs->rasterize(gl_.get());  // triggers detached check
    }
    canvasScenes_.erase(
        std::remove_if(canvasScenes_.begin(), canvasScenes_.end(),
            [](auto& cs) { return cs->isDetached(); }),
        canvasScenes_.end());
}

void Engine::advanceTime(double ms) {
    if (displayMode_ != DisplayMode::Headless) return;

    // Get active WebGL context for FBO binding
    webgl::WebGL2RenderingContext* activeWebGL = nullptr;
    if (!webglEntries_.empty()) activeWebGL = webglEntries_[0].context.get();

    double remaining = ms;
    while (remaining > 0) {
        double step = std::min(remaining, 16.0);
        virtualTime_ += step;
        remaining -= step;
        timers_->tick(virtualTime_);

        // Tick brokit fetch (pump pending HTTP requests)
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

        // Tick brokit WebSocket (pump pending connections/messages)
        {
            JSValue global = JS_GetGlobalObject(jsRuntime_->getContext());
            JSValue tickFn = JS_GetPropertyStr(jsRuntime_->getContext(), global, "__brokit_ws_tick");
            if (JS_IsFunction(jsRuntime_->getContext(), tickFn)) {
                JSValue ret = JS_Call(jsRuntime_->getContext(), tickFn, JS_UNDEFINED, 0, nullptr);
                JS_FreeValue(jsRuntime_->getContext(), ret);
            }
            JS_FreeValue(jsRuntime_->getContext(), tickFn);
            JS_FreeValue(jsRuntime_->getContext(), global);
        }

        if (activeWebGL) activeWebGL->bindCanvasFBO();
        timers_->fireAnimationFrames(virtualTime_);
        jsRuntime_->executePendingJobs();
        js::tickWorkers(jsRuntime_->getContext());
        jsRuntime_->executePendingJobs();

        // Poll network (drain subscriber's event queue, fire JS callbacks)
        if (netService_) {
            js::NetBindings::poll(jsRuntime_->getContext());
            jsRuntime_->executePendingJobs();
        }

        if (activeWebGL) activeWebGL->unbindCanvasFBO();

        // Step AI bindings once per advanceTime step (deterministic, uses the
        // headless virtual-time step as dt).
        {
            float aiDt = static_cast<float>(step * 0.001);
            for (auto& sg : sceneGraphs_) {
                sg.graph->syncAgents(aiDt);
            }
        }

        // Headless has no raster thread (the normal code path relies on the
        // raster loop to materialize HtmlNodes). Run it inline on the main
        // thread using the main renderer/font manager before scene render.
        auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
        if (skia) {
            for (auto& sg : sceneGraphs_) {
                if (sg.graph) {
                    sg.graph->materializeHtmlNodes(skia, &fontManager_);
                }
            }
        }

        // Auto-render scene graphs after JS execution
        for (auto& sg : sceneGraphs_) {
            sg.graph->render();
        }

        flush();

        // Pump headless audio engine: render frames matching this time step
        if (audioEngine_) {
            int audioFrames = static_cast<int>(step * audioEngine_->sampleRate() / 1000.0 + 0.5);
            if (audioFrames > 0)
                audioEngine_->renderBlock(audioFrames);
        }

        // Tick crosshair spread system
        crosshair_.tick(static_cast<float>(step * 0.001));

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
    webgl::WebGL2RenderingContext* activeWebGL = nullptr;
    if (!webglEntries_.empty()) activeWebGL = webglEntries_[0].context.get();
    if (activeWebGL) activeWebGL->bindCanvasFBO();

    // Fire any pending rAF callbacks so canvas commands are up to date
    timers_->fireAnimationFrames(virtualTime_);
    jsRuntime_->executePendingJobs();

    // Unbind WebGL canvas FBO
    if (activeWebGL) activeWebGL->unbindCanvasFBO();

    // GPU compositing path: replicate the windowed render pass to an offscreen FBO,
    // then read back pixels. This captures WebGL, Canvas2D, scene graph 3D + UI overlay.
    if (gl_ && (!webglEntries_.empty() || !canvasScenes_.empty() || !sceneGraphs_.empty())) {
        auto* skia = static_cast<render::SkiaRenderer*>(renderer_.get());
        int w = viewportWidth_, h = viewportHeight_;

        // 1. Rasterize HTML/CSS UI to Skia surface
        renderer_->beginFrame(w, h);
        if (document_->documentElement()) {
            drawTraversal_->draw(document_->documentElement(),
                                 0, static_cast<float>(contentTop()),
                                 w, contentHeight(), contentTop());
        }
        overlayMgr_.drawIfContext(OverlayContext::App, renderer_.get());
        renderer_->endFrame();
        skia->uploadToGPU();

        // 2. Rasterize canvas scenes into their per-canvas FBOs; prune detached
        for (auto& cs : canvasScenes_) {
            cs->setViewportScroll(scrollY_);
            cs->rasterize(gl_.get());
        }
        canvasScenes_.erase(
            std::remove_if(canvasScenes_.begin(), canvasScenes_.end(),
                [](auto& cs) { return cs->isDetached(); }),
            canvasScenes_.end());

        // 3. Create temporary compositing FBO
        GLuint compositeFBO = 0, compositeTex = 0;
        glGenFramebuffers(1, &compositeFBO);
        compositeTex = gl_->createTexture2D(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, compositeTex, 0);

        // 4. Clear and render scene layers (WebGL etc.)
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // 4. Composite UI overlay (premultiplied alpha)
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

        // 5. Composite canvas FBO textures ON TOP of the UI
        compositeCanvasScenes(gl_.get(), w, h, compositeFBO);

        // 5a. Composite scene graph mesh FBO textures at element positions
        for (auto& entry : sceneGraphs_) {
            if (!entry.element || !entry.graph) continue;
            GLuint tex = entry.element->sceneGraphFBOTexture();
            if (!tex) continue;
            auto& box = entry.element->layoutBox();
            float ex = box.contentRect.x, ey = box.contentRect.y;
            float ew = box.contentRect.width, eh = box.contentRect.height;
            for (auto* lp = entry.element->layoutParent(); lp; lp = lp->layoutParent()) {
                auto& pb = lp->layoutBox();
                ex += pb.contentRect.x;
                ey += pb.contentRect.y;
            }
            // Mesh FBO textures are bottom-up (OpenGL) — flip V
            float fw = (float)w, fh = (float)h;
            render::TextureVertex quad[6] = {
                {ex,    ey,    0, 1}, {ex+ew, ey,    1, 1}, {ex+ew, ey+eh, 1, 0},
                {ex,    ey,    0, 1}, {ex+ew, ey+eh, 1, 0}, {ex,    ey+eh, 0, 0},
            };

            GLuint quadVBO = 0, quadVAO = 0;
            glGenBuffers(1, &quadVBO);
            glGenVertexArrays(1, &quadVAO);
            glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
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
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            glDeleteBuffers(1, &quadVBO);
            glDeleteVertexArrays(1, &quadVAO);
        }

        // 5b. Composite WebGL textures at element positions
        for (auto& entry : webglEntries_) {
            GLuint tex = entry.context->colorTexture();
            if (!tex || !entry.element) continue;
            auto& box = entry.element->layoutBox();
            float ex = box.contentRect.x, ey = box.contentRect.y;
            float ew = box.contentRect.width, eh = box.contentRect.height;
            // Walk up layout parents to get absolute position
            for (auto* lp = entry.element->layoutParent(); lp; lp = lp->layoutParent()) {
                auto& pb = lp->layoutBox();
                ex += pb.contentRect.x;
                ey += pb.contentRect.y;
            }
            // WebGL textures are bottom-up — flip V
            float fw = (float)w, fh = (float)h;
            render::TextureVertex quad[6] = {
                {ex,    ey,    0, 1}, {ex+ew, ey,    1, 1}, {ex+ew, ey+eh, 1, 0},
                {ex,    ey,    0, 1}, {ex+ew, ey+eh, 1, 0}, {ex,    ey+eh, 0, 0},
            };

            GLuint quadVBO = 0, quadVAO = 0;
            glGenBuffers(1, &quadVBO);
            glGenVertexArrays(1, &quadVAO);
            glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
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
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            glDeleteBuffers(1, &quadVBO);
            glDeleteVertexArrays(1, &quadVAO);
        }

        // 5c. Draw crosshair overlay
        drawCrosshairGL();

        // 6. Read back pixels
        std::vector<uint8_t> pixels(w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        // 7. Cleanup compositing FBO + restore WebGL state
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &compositeFBO);
        gl_->deleteTexture(compositeTex);

        if (activeWebGL) activeWebGL->restoreState();

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

    // Composite canvas scenes (behind HTML) — Skia surface blit
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

    // Render HTML/CSS overlay on top
    drawTraversal_->draw(document_->documentElement(),
                         0, static_cast<float>(contentTop()),
                         viewportWidth_, contentHeight(), contentTop());

    // Render system panels on top of everything
    if (isSystemVisible()) {
        tickSystemPanels(virtualTime_);
        renderSystemPanels();

        if (systemRenderer_ && systemRenderer_->surface()) {
            auto* appCanvas = renderer_->getCanvas();
            if (appCanvas) {
                sk_sp<SkImage> sysImage = systemRenderer_->surface()->makeImageSnapshot();
                if (sysImage) {
                    SkPaint paint;
                    paint.setBlendMode(SkBlendMode::kSrcOver);
                    appCanvas->drawImage(sysImage, 0, 0, SkSamplingOptions(), &paint);
                }
            }
        }
    }

    // Draw crosshair on the Skia surface
    drawCrosshairSkia(renderer_->getCanvas());

    renderer_->endFrame();

    return renderer_->saveScreenshot(path);
}

std::vector<uint8_t> Engine::capturePixels() {
    if (!document_) return {};

    // Bind WebGL canvas FBO before firing rAF
    webgl::WebGL2RenderingContext* activeWebGL2 = nullptr;
    if (!webglEntries_.empty()) activeWebGL2 = webglEntries_[0].context.get();
    if (activeWebGL2) activeWebGL2->bindCanvasFBO();

    timers_->fireAnimationFrames(virtualTime_);
    jsRuntime_->executePendingJobs();

    if (activeWebGL2) activeWebGL2->unbindCanvasFBO();

    // GPU compositing path
    if (gl_ && (!webglEntries_.empty() || !canvasScenes_.empty() || !sceneGraphs_.empty())) {
        auto* skia = static_cast<render::SkiaRenderer*>(renderer_.get());
        int w = viewportWidth_, h = viewportHeight_;

        renderer_->beginFrame(w, h);
        if (document_->documentElement())
            drawTraversal_->draw(document_->documentElement(),
                                 0, static_cast<float>(contentTop()),
                                 w, contentHeight(), contentTop());
        renderer_->endFrame();
        skia->uploadToGPU();

        for (auto& cs : canvasScenes_) {
            cs->setViewportScroll(scrollY_);
            cs->rasterize(gl_.get());
        }
        canvasScenes_.erase(
            std::remove_if(canvasScenes_.begin(), canvasScenes_.end(),
                [](auto& cs) { return cs->isDetached(); }),
            canvasScenes_.end());

        // Create temporary compositing FBO
        GLuint compositeFBO = 0, compositeTex = 0;
        glGenFramebuffers(1, &compositeFBO);
        compositeTex = gl_->createTexture2D(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, compositeTex, 0);

        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

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

        // Composite canvas FBO textures ON TOP of the UI
        compositeCanvasScenes(gl_.get(), w, h, compositeFBO);

        // Composite scene graph mesh FBO textures at element positions
        for (auto& entry : sceneGraphs_) {
            if (!entry.element || !entry.graph) continue;
            GLuint tex = entry.element->sceneGraphFBOTexture();
            if (!tex) continue;
            auto& box = entry.element->layoutBox();
            float ex = box.contentRect.x, ey = box.contentRect.y;
            float ew = box.contentRect.width, eh = box.contentRect.height;
            for (auto* lp = entry.element->layoutParent(); lp; lp = lp->layoutParent()) {
                auto& pb = lp->layoutBox();
                ex += pb.contentRect.x;
                ey += pb.contentRect.y;
            }
            float fw = (float)w, fh = (float)h;
            render::TextureVertex quad[6] = {
                {ex,    ey,    0, 1}, {ex+ew, ey,    1, 1}, {ex+ew, ey+eh, 1, 0},
                {ex,    ey,    0, 1}, {ex+ew, ey+eh, 1, 0}, {ex,    ey+eh, 0, 0},
            };
            GLuint quadVBO = 0, quadVAO = 0;
            glGenBuffers(1, &quadVBO);
            glGenVertexArrays(1, &quadVAO);
            glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
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
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glDeleteBuffers(1, &quadVBO);
            glDeleteVertexArrays(1, &quadVAO);
        }

        // Composite WebGL textures at element positions
        for (auto& entry : webglEntries_) {
            GLuint tex = entry.context->colorTexture();
            if (!tex || !entry.element) continue;
            auto& box = entry.element->layoutBox();
            float ex = box.contentRect.x, ey = box.contentRect.y;
            float ew = box.contentRect.width, eh = box.contentRect.height;
            for (auto* lp = entry.element->layoutParent(); lp; lp = lp->layoutParent()) {
                auto& pb = lp->layoutBox();
                ex += pb.contentRect.x;
                ey += pb.contentRect.y;
            }
            float fw = (float)w, fh = (float)h;
            render::TextureVertex quad[6] = {
                {ex,    ey,    0, 1}, {ex+ew, ey,    1, 1}, {ex+ew, ey+eh, 1, 0},
                {ex,    ey,    0, 1}, {ex+ew, ey+eh, 1, 0}, {ex,    ey+eh, 0, 0},
            };
            GLuint quadVBO = 0, quadVAO = 0;
            glGenBuffers(1, &quadVBO);
            glGenVertexArrays(1, &quadVAO);
            glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
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
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glDeleteBuffers(1, &quadVBO);
            glDeleteVertexArrays(1, &quadVAO);
        }

        // Draw crosshair overlay
        drawCrosshairGL();

        // Read back pixels
        std::vector<uint8_t> pixels(w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &compositeFBO);
        gl_->deleteTexture(compositeTex);

        if (activeWebGL2) activeWebGL2->restoreState();

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

    // Composite canvas scenes — Skia surface blit
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
                         0, static_cast<float>(contentTop()),
                         viewportWidth_, contentHeight(), contentTop());

    overlayMgr_.drawIfContext(OverlayContext::App, renderer_.get());

    if (isSystemVisible()) {
        tickSystemPanels(virtualTime_);
        renderSystemPanels();
        if (systemRenderer_ && systemRenderer_->surface()) {
            auto* appCanvas = renderer_->getCanvas();
            if (appCanvas) {
                sk_sp<SkImage> sysImage = systemRenderer_->surface()->makeImageSnapshot();
                if (sysImage) {
                    SkPaint paint;
                    paint.setBlendMode(SkBlendMode::kSrcOver);
                    appCanvas->drawImage(sysImage, 0, 0, SkSamplingOptions(), &paint);
                }
            }
        }
    }

    // Draw crosshair on the Skia surface
    drawCrosshairSkia(renderer_->getCanvas());

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

// overlayQuerySelector() and overlayPanelNames() are in system_panels.cpp.

void Engine::dispatchClickOn(dom::Element* target) {
    if (!target || !jsRuntime_) return;
    if (document_) document_->setActiveElement(target);
    dom::MouseEvent event("click");
    js::dispatchDomEvent(jsRuntime_->getContext(), target, event);
}

} // namespace bro::engine
