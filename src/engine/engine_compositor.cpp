#include "engine/engine.h"
#include "engine/inspector_highlight.h"
#include "engine/overflow.h"

#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "layout/box.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "platform/sdl_window.h"
#include "render/command_buffer.h"
#include "render/command_replayer.h"
#include "render/gl_context.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>

#include <glad/gl.h>
#include <include/gpu/ganesh/GrDirectContext.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace bro::engine {

void Engine::addCanvasScene(std::unique_ptr<canvas::CanvasScene> scene) {
    if (scene) {
        scene->init(gl_.get());
        // Windowed GPU mode: bind to the shared canvas-raster worker. No GL
        // context is created here — the worker's context was created once at
        // run() start — so registering a canvas never races the raster thread.
        // (If the worker isn't up yet, e.g. a scene created during app load,
        // run()'s init binds it once the worker exists.)
        if (displayMode_ == DisplayMode::Windowed && window_) {
            if (canvasRasterThread_)
                scene->bindRasterThread(canvasRasterThread_.get());
        } else {
            // Headless / CPU fallback: use renderer's GrContext directly
            auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
            if (skia && skia->grContext()) {
                scene->setGrContext(skia->grContext());
            }
        }
    }
    if (scene) canvasSceneRegistry_[scene->sceneId()] = scene.get();
    canvasScenes_.push_back(std::move(scene));
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

void Engine::recordAppLayers(render::CommandBuffer& outBuffer,
                             int vpW, int vpH,
                             int insetTop, int insetRight, int insetBottom,
                             float scrollY) {
    if (!recordingRenderer_ || !drawTraversal_) return;

    int contentW = vpW - insetRight;
    int contentH = vpH - insetTop - insetBottom;

    outBuffer.clear();
    recordingRenderer_->setBuffer(&outBuffer);

    // Layer-break callback emits Cmd_LayerBreak. The replayer's handler does
    // the actual GPU surface management.
    drawTraversal_->setLayerBreakCallback(
        [this](canvas::CanvasScene* scene, unsigned int directTexture,
               float x, float y, float w, float h,
               float clipX, float clipY, float clipW, float clipH) {
            int kind = scene ? render::Cmd_LayerBreak::Canvas2D
                             : render::Cmd_LayerBreak::WebGL;
            recordingRenderer_->recordLayerBreak(
                kind, scene ? scene->sceneId() : 0, directTexture, x, y, w, h,
                clipX, clipY, clipW, clipH);
        });

    if (document_ && document_->documentElement()) {
        drawTraversal_->setBasePath(document_->basePath());
        drawTraversal_->draw(document_->documentElement(),
                             0, static_cast<float>(insetTop) - scrollY,
                             contentW, contentH, insetTop);

        drawSelectionHighlight(recordingRenderer_.get(),
                               static_cast<float>(insetTop) - scrollY);

        if (inspector_.visible) {
            dom::Element* highlight = inspector_.pickerMode && inspector_.pickerHover
                ? inspector_.pickerHover
                : inspector_.selected;
            if (highlight) {
                drawInspectorHighlight(recordingRenderer_.get(), highlight, scrollY,
                                       /*insetLeft=*/0, insetTop,
                                       contentW, contentH);
            }
        }
    }

    overlayMgr_.drawIfContext(OverlayContext::App, recordingRenderer_.get());

    if (document_) {
        float ct = static_cast<float>(insetTop);
        float vh = static_cast<float>(contentH);
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(contentW) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY);
        viewportScrollbar_.draw(recordingRenderer_.get(), m);

        drawElementScrollbars(recordingRenderer_.get(),
                              document_->documentElement(),
                              0.0f, static_cast<float>(insetTop) - scrollY);
    }

    drawTraversal_->setLayerBreakCallback(nullptr);
    recordingRenderer_->setBuffer(nullptr);
}

void Engine::replayAppLayers(render::SkiaRenderer* renderer,
                             const render::CommandBuffer& buffer,
                             std::vector<render::SkiaRenderer::GPUSurface>& pool,
                             int& poolW, int& poolH,
                             int vpW, int vpH,
                             std::vector<UILayer>& outLayers) {
    if (!renderer || !renderer->grContext()) return;

    if (poolW != vpW || poolH != vpH) {
        for (auto& ps : pool) renderer->destroyGPUSurface(ps);
        pool.clear();
        poolW = vpW;
        poolH = vpH;
    }
    if (pool.empty()) {
        pool.push_back(renderer->createGPUSurface(vpW, vpH));
    }
    for (auto& ps : pool) {
        renderer->rewrapGPUSurface(ps, vpW, vpH);
    }

    int htmlLayerIdx = 0;
    auto origSurface = renderer->switchSurface(pool[0].surface);

    render::CommandReplayer replayer(renderer);
    replayer.setLayerBreakHandler(
        [&](int kind, uint64_t sceneId, unsigned int directTexture,
            float x, float y, float w, float h,
            float clipX, float clipY, float clipW, float clipH) {
            int prevIdx = htmlLayerIdx;
            htmlLayerIdx++;
            while (htmlLayerIdx >= static_cast<int>(pool.size())) {
                pool.push_back(renderer->createGPUSurface(vpW, vpH));
                renderer->rewrapGPUSurface(pool.back(), vpW, vpH);
            }
            renderer->switchSurface(pool[htmlLayerIdx].surface);

            UILayer htmlLayer;
            htmlLayer.type = UILayer::HTML;
            htmlLayer.texture = pool[prevIdx].texture;
            outLayers.push_back(std::move(htmlLayer));

            UILayer canvasLayer;
            canvasLayer.type = UILayer::Canvas;
            canvasLayer.canvasSceneId = sceneId;
            canvasLayer.texture = directTexture;
            canvasLayer.cx = x; canvasLayer.cy = y;
            canvasLayer.cw = w; canvasLayer.ch = h;
            canvasLayer.clipX = clipX; canvasLayer.clipY = clipY;
            canvasLayer.clipW = clipW; canvasLayer.clipH = clipH;
            outLayers.push_back(std::move(canvasLayer));
            (void)kind;
        });

    replayer.replay(buffer);

    // Capture the trailing HTML layer.
    renderer->switchSurface(origSurface);
    UILayer lastHtml;
    lastHtml.type = UILayer::HTML;
    lastHtml.texture = pool[htmlLayerIdx].texture;
    outLayers.push_back(std::move(lastHtml));

    // Flush each pool surface's deferred Ganesh ops.
    for (int i = 0; i <= htmlLayerIdx; ++i) {
        if (pool[i].surface && renderer->grContext()) {
            renderer->grContext()->flush(pool[i].surface.get());
        }
    }
}

void Engine::recordSystemPanelLayers(render::CommandBuffer& outBuffer,
                                     int vpW, int vpH) {
    outBuffer.clear();
    if (!recordingRenderer_ || !drawTraversal_ || !isSystemVisible()) return;

    // Layout still runs on main thread using textMetrics_ (paired with
    // renderer_, which the recording renderer also delegates to).
    layoutSystemPanels(*textMetrics_);

    recordingRenderer_->setBuffer(&outBuffer);

    bool first = true;
    for (auto& sdoc : systemDocs_) {
        if (!isSystemDocVisible(sdoc) || !sdoc.document) continue;

        // Between panels, emit an HtmlSurface boundary so the replayer
        // captures the current panel into a UILayer and starts a new surface.
        if (!first) {
            recordingRenderer_->recordLayerBreak(
                render::Cmd_LayerBreak::HtmlSurface, 0, 0, 0, 0, 0, 0);
        }
        first = false;

        drawSystemPanelDoc(recordingRenderer_.get(), *drawTraversal_, sdoc, vpW, vpH);
    }

    recordingRenderer_->setBuffer(nullptr);
    systemDirty_ = false;
}

void Engine::replaySystemPanelLayers(render::SkiaRenderer* renderer,
                                     const render::CommandBuffer& buffer,
                                     std::vector<render::SkiaRenderer::GPUSurface>& pool,
                                     int& poolW, int& poolH,
                                     int vpW, int vpH,
                                     std::vector<UILayer>& outLayers) {
    if (!renderer || !renderer->grContext()) return;
    if (buffer.commandCount() == 0) return;

    if (poolW != vpW || poolH != vpH) {
        for (auto& ps : pool) renderer->destroyGPUSurface(ps);
        pool.clear();
        poolW = vpW;
        poolH = vpH;
    }

    auto ensurePoolAt = [&](size_t idx) {
        while (idx >= pool.size()) {
            pool.push_back(renderer->createGPUSurface(vpW, vpH));
            renderer->rewrapGPUSurface(pool.back(), vpW, vpH);
        }
    };

    size_t panelIdx = 0;
    ensurePoolAt(panelIdx);
    renderer->rewrapGPUSurface(pool[panelIdx], vpW, vpH);
    auto origSurface = renderer->switchSurface(pool[panelIdx].surface);

    auto* grCtx = renderer->grContext();

    render::CommandReplayer replayer(renderer);
    replayer.setLayerBreakHandler(
        [&](int kind, uint64_t /*sceneId*/, unsigned int /*tex*/,
            float, float, float, float, float, float, float, float) {
            if (kind != render::Cmd_LayerBreak::HtmlSurface) return;
            // Capture current panel into a UILayer, advance to next surface.
            if (grCtx) grCtx->flush(pool[panelIdx].surface.get());
            UILayer panelLayer;
            panelLayer.type = UILayer::HTML;
            panelLayer.texture = pool[panelIdx].texture;
            outLayers.push_back(std::move(panelLayer));

            panelIdx++;
            ensurePoolAt(panelIdx);
            renderer->rewrapGPUSurface(pool[panelIdx], vpW, vpH);
            renderer->switchSurface(pool[panelIdx].surface);
        });
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
            SkRect dst = SkRect::MakeXYWH(x, y, w, h);
            c->drawImageRect(img, dst, SkSamplingOptions(SkFilterMode::kLinear));
            scene->clearDirty();
        });

    replayer.replay(buffer);

    // Capture the final panel.
    if (grCtx) grCtx->flush(pool[panelIdx].surface.get());
    UILayer panelLayer;
    panelLayer.type = UILayer::HTML;
    panelLayer.texture = pool[panelIdx].texture;
    outLayers.push_back(std::move(panelLayer));

    renderer->switchSurface(origSurface);
}

void Engine::compositeLayers(const std::vector<UILayer>& layers, GLuint targetFBO) {
    if (!gl_) return;
    if (layers.empty()) return;

    float vw = static_cast<float>(viewportWidth_);
    float vh = static_cast<float>(viewportHeight_);

    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
    glViewport(0, 0, viewportWidth_, viewportHeight_);

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

    for (auto& layer : layers) {
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
            // Canvas/WebGL layer — get texture from canvas scene or direct
            // texture. A canvasSceneId that no longer resolves (scene detached
            // since this layer was recorded) draws nothing this frame.
            GLuint tex = 0;
            bool isCanvas = layer.canvasSceneId != 0;
            if (isCanvas) {
                if (auto* cs = canvasSceneById(layer.canvasSceneId))
                    tex = cs->texture();
            } else {
                tex = layer.texture;  // WebGL direct texture
            }
            if (tex) {
                float cx = layer.cx, cy = layer.cy;
                float cw = layer.cw, ch = layer.ch;

                // WebGL textures are bottom-up (origin at lower-left) so flip V coords
                float v0 = isCanvas ? 0.0f : 1.0f;
                float v1 = isCanvas ? 1.0f : 0.0f;

                // Canvas/WebGL layers composite outside the Skia clip stack, so
                // re-apply any ancestor overflow/scroll clip as a GL scissor.
                // Clip space is top-left pixels; scissor is bottom-left window
                // coords, so flip Y. clipW < 0 ⇒ unclipped.
                bool scissored = false;
                if (layer.clipW >= 0.0f && layer.clipH >= 0.0f) {
                    int sx = static_cast<int>(std::floor(layer.clipX));
                    int sw = static_cast<int>(std::ceil(layer.clipX + layer.clipW)) - sx;
                    int syTop = static_cast<int>(std::floor(layer.clipY));
                    int sh = static_cast<int>(std::ceil(layer.clipY + layer.clipH)) - syTop;
                    int sy = viewportHeight_ - (syTop + sh);
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(sx, sy, std::max(0, sw), std::max(0, sh));
                    scissored = true;
                }

                render::TextureVertex quad[6] = {
                    {cx,    cy,    0, v0}, {cx+cw, cy,    1, v0}, {cx+cw, cy+ch, 1, v1},
                    {cx,    cy,    0, v0}, {cx+cw, cy+ch, 1, v1}, {cx,    cy+ch, 0, v1},
                };
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
                glBindTexture(GL_TEXTURE_2D, tex);
                glDrawArrays(GL_TRIANGLES, 0, 6);

                if (scissored) glDisable(GL_SCISSOR_TEST);
            }
        }
    }
}

} // namespace bro::engine
