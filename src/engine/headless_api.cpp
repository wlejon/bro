// Engine headless/capture API methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
#include "engine/system_overlay.h"

#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/scene_layer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/event_dispatch.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "canvas/canvas_scene.h"
#include "canvas/canvas2d.h"
#include "webgl/webgl2_context.h"
#include "webgl/webgl_scene.h"

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
        auto* webglScene2 = dynamic_cast<webgl::WebGLScene*>(sceneLayer_.get());
        if (webglScene2 && webglScene2->webglContext()) {
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
