// Engine headless/capture API methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
#include "engine/system_overlay.h"

#include "observer_check.js.h"

#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/scene_layer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/event_dispatch.h"
#include "js/worker.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "canvas/canvas_scene.h"
#include "webgl/webgl2_context.h"
#include "webgl/webgl_scene.h"

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
    if (document_ && document_->isDirty()) {
        if (document_->isStructureDirty()) {
            ensureReplacedElements(document_->documentElement());
        }
        document_->resolveStyles();
        document_->clearStructureDirty();
        document_->performLayout(static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), *textMetrics_);
        document_->clearDirty();

        // Notify ResizeObserver / IntersectionObserver after layout
        if (jsRuntime_) {
            JSValue r = JS_Eval(jsRuntime_->getContext(), js_observer_check,
                                strlen(js_observer_check), "<observer-check>", JS_EVAL_TYPE_GLOBAL);
            JS_FreeValue(jsRuntime_->getContext(), r);
        }
    }

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

    // Bind WebGL canvas FBO so rAF draw commands target it correctly
    webgl::WebGLScene* webglScene = nullptr;
    for (auto& sl : sceneLayers_) { webglScene = dynamic_cast<webgl::WebGLScene*>(sl.get()); if (webglScene) break; }

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

        if (webglScene && webglScene->webglContext())
            webglScene->webglContext()->bindCanvasFBO();
        timers_->fireAnimationFrames(virtualTime_);
        jsRuntime_->executePendingJobs();
        js::tickWorkers(jsRuntime_->getContext());
        jsRuntime_->executePendingJobs();
        if (webglScene && webglScene->webglContext())
            webglScene->webglContext()->unbindCanvasFBO();

        flush();

        // Pump headless audio engine: render frames matching this time step
        if (audioEngine_) {
            int audioFrames = static_cast<int>(step * audioEngine_->sampleRate() / 1000.0 + 0.5);
            if (audioFrames > 0)
                audioEngine_->renderBlock(audioFrames);
        }

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
    webgl::WebGLScene* webglScene = nullptr;
    for (auto& sl : sceneLayers_) { webglScene = dynamic_cast<webgl::WebGLScene*>(sl.get()); if (webglScene) break; }
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
    if (gl_ && (!sceneLayers_.empty() || !canvasScenes_.empty())) {
        auto* skia = static_cast<render::SkiaRenderer*>(renderer_.get());
        int w = viewportWidth_, h = viewportHeight_;

        // 1. Rasterize HTML/CSS UI to Skia surface
        renderer_->beginFrame(w, h);
        if (document_->documentElement()) {
            drawTraversal_->draw(document_->documentElement(), 0, 0, w, h);
        }
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
        for (auto& sl : sceneLayers_) { if (sl) sl->onRender(gl_.get(), w, h, 0.0); }

        // 5. Composite UI overlay (premultiplied alpha)
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

        // 6. Composite canvas FBO textures ON TOP of the UI
        compositeCanvasScenes(gl_.get(), w, h, compositeFBO);

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
    webgl::WebGLScene* webglScene = nullptr;
    for (auto& sl : sceneLayers_) { webglScene = dynamic_cast<webgl::WebGLScene*>(sl.get()); if (webglScene) break; }
    if (webglScene && webglScene->webglContext())
        webglScene->webglContext()->bindCanvasFBO();

    timers_->fireAnimationFrames(virtualTime_);
    jsRuntime_->executePendingJobs();

    if (webglScene && webglScene->webglContext())
        webglScene->webglContext()->unbindCanvasFBO();

    // GPU compositing path
    if (gl_ && (!sceneLayers_.empty() || !canvasScenes_.empty())) {
        auto* skia = static_cast<render::SkiaRenderer*>(renderer_.get());
        int w = viewportWidth_, h = viewportHeight_;

        renderer_->beginFrame(w, h);
        if (document_->documentElement())
            drawTraversal_->draw(document_->documentElement(), 0, 0, w, h);
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
        for (auto& sl : sceneLayers_) { if (sl) sl->onRender(gl_.get(), w, h, 0.0); }

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
