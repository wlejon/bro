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
#include "render/gl_context.h"
#include "render/skia_backend.h"

#include <glad/gl.h>
#include <include/gpu/ganesh/GrDirectContext.h>

#include <cstddef>
#include <utility>

namespace bro::engine {

void Engine::addCanvasScene(std::unique_ptr<canvas::CanvasScene> scene) {
    if (scene) {
        scene->init(gl_.get());
        // In windowed GPU mode, each canvas gets its own thread with a shared
        // GL context + GrDirectContext for parallel rasterization.
        if (displayMode_ == DisplayMode::Windowed && window_) {
            // Context creation on the main thread (macOS/AppKit requirement);
            // startThread() blocks until the worker has MakeCurrent'd it, so
            // the next createSharedContext call cannot overlap with a worker's
            // wgl*Context call (Windows/NVIDIA requirement).
            auto ctx = window_->createSharedContext();
            if (ctx) {
                scene->startThread(ctx, window_->getSDLWindow());
            }
        } else {
            // Headless / CPU fallback: use renderer's GrContext directly
            auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
            if (skia && skia->grContext()) {
                scene->setGrContext(skia->grContext());
            }
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

void Engine::buildAppLayers(render::SkiaRenderer* renderer,
                            layout::DrawTraversal& traversal,
                            std::vector<render::SkiaRenderer::GPUSurface>& pool,
                            int& poolW, int& poolH,
                            int vpW, int vpH,
                            int insetTop, int insetRight, int insetBottom,
                            float scrollY,
                            std::vector<UILayer>& outLayers) {
    if (!renderer || !renderer->grContext()) return;

    int contentW = vpW - insetRight;
    int contentH = vpH - insetTop - insetBottom;

    // Invalidate pool on viewport resize.
    if (poolW != vpW || poolH != vpH) {
        for (auto& ps : pool) renderer->destroyGPUSurface(ps);
        pool.clear();
        poolW = vpW;
        poolH = vpH;
    }

    int htmlLayerIdx = 0;

    // Layer-break callback fires on canvas/WebGL/scene-graph elements
    // encountered during the draw traversal. Each break: emit the current
    // HTML layer (texture from pool[prev]), emit the canvas/WebGL/scene
    // layer, switch the renderer to a fresh pool surface for subsequent
    // HTML content.
    traversal.setLayerBreakCallback(
        [&](canvas::CanvasScene* scene, unsigned int directTexture,
            float x, float y, float w, float h) {
            int prevIdx = htmlLayerIdx;
            htmlLayerIdx++;
            while (htmlLayerIdx >= static_cast<int>(pool.size())) {
                pool.push_back(renderer->createGPUSurface(vpW, vpH));
            }
            renderer->switchSurface(pool[htmlLayerIdx].surface);

            UILayer htmlLayer;
            htmlLayer.type = UILayer::HTML;
            htmlLayer.texture = pool[prevIdx].texture;
            outLayers.push_back(std::move(htmlLayer));

            UILayer canvasLayer;
            canvasLayer.type = UILayer::Canvas;
            canvasLayer.canvasScene = scene;
            canvasLayer.texture = directTexture;  // non-zero for WebGL/scene-graph
            canvasLayer.cx = x; canvasLayer.cy = y;
            canvasLayer.cw = w; canvasLayer.ch = h;
            outLayers.push_back(std::move(canvasLayer));
        });

    // Ensure pool has a GPU surface for HTML layer 0
    if (pool.empty()) {
        pool.push_back(renderer->createGPUSurface(vpW, vpH));
    }
    // Rewrap existing pool surfaces with fresh Skia wrappers
    for (auto& ps : pool) {
        renderer->rewrapGPUSurface(ps, vpW, vpH);
    }
    // Switch to pool surface for HTML layer 0
    auto origSurface = renderer->switchSurface(pool[0].surface);

    // Draw traversal — reads layout boxes and computed styles (read-only).
    // App content is translated down by insetTop so the top strip is
    // reserved for the engine-owned menu bar.
    if (document_ && document_->documentElement()) {
        traversal.setBasePath(document_->basePath());
        traversal.draw(document_->documentElement(),
                       0, static_cast<float>(insetTop) - scrollY,
                       contentW, contentH, insetTop);

        // Selection highlight overlay sits above text. Reads
        // selectionSnapshot_ (built on main thread before we run).
        drawSelectionHighlight(renderer,
                               static_cast<float>(insetTop) - scrollY);

        // Inspector box-model overlay. The picker hover wins over the static
        // selection while picker mode is active so the user can preview boxes
        // before clicking.
        if (inspector_.visible) {
            dom::Element* highlight = inspector_.pickerMode && inspector_.pickerHover
                ? inspector_.pickerHover
                : inspector_.selected;
            if (highlight) {
                drawInspectorHighlight(renderer, highlight, scrollY,
                                       /*insetLeft=*/0, insetTop,
                                       contentW, contentH);
            }
        }
    }

    // App-context overlay (dropdown / color picker / etc.)
    overlayMgr_.drawIfContext(OverlayContext::App, renderer);

    // Viewport scrollbar at the right edge of the app content area (which is
    // contentW, not vpW, when the inspector is docked on the right).
    if (document_) {
        float ct = static_cast<float>(insetTop);
        float vh = static_cast<float>(contentH);
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(contentW) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY);
        viewportScrollbar_.draw(renderer, m);

        drawElementScrollbars(renderer,
                              document_->documentElement(),
                              0.0f, static_cast<float>(insetTop) - scrollY);
    }

    // Capture the last HTML layer
    renderer->switchSurface(origSurface);
    UILayer lastHtml;
    lastHtml.type = UILayer::HTML;
    lastHtml.texture = pool[htmlLayerIdx].texture;
    outLayers.push_back(std::move(lastHtml));

    // Flush each pool surface's deferred Ganesh ops
    for (int i = 0; i <= htmlLayerIdx; ++i) {
        if (pool[i].surface && renderer->grContext()) {
            renderer->grContext()->flush(pool[i].surface.get());
        }
    }
    traversal.setLayerBreakCallback(nullptr);
}

void Engine::buildSystemPanelLayers(render::SkiaRenderer* renderer,
                                    layout::DrawTraversal& traversal,
                                    layout::FontManager* fontManager,
                                    std::vector<render::SkiaRenderer::GPUSurface>& pool,
                                    int& poolW, int& poolH,
                                    int vpW, int vpH,
                                    std::vector<UILayer>& outLayers) {
    if (!renderer || !renderer->grContext() || !isSystemVisible()) return;

    if (poolW != vpW || poolH != vpH) {
        for (auto& ps : pool) renderer->destroyGPUSurface(ps);
        pool.clear();
        poolW = vpW;
        poolH = vpH;
    }

    layout::SkiaTextMetrics sysMetrics(renderer, fontManager);
    layoutSystemPanels(sysMetrics);

    size_t panelIdx = 0;
    for (auto& sdoc : systemDocs_) {
        if (!isSystemDocVisible(sdoc) || !sdoc.document) continue;

        while (panelIdx >= pool.size()) {
            pool.push_back(renderer->createGPUSurface(vpW, vpH));
        }
        renderer->rewrapGPUSurface(pool[panelIdx], vpW, vpH);
        auto prev = renderer->switchSurface(pool[panelIdx].surface);

        drawSystemPanelDoc(renderer, traversal, sdoc, vpW, vpH);

        UILayer panelLayer;
        panelLayer.type = UILayer::HTML;
        panelLayer.texture = pool[panelIdx].texture;
        outLayers.push_back(std::move(panelLayer));

        if (renderer->grContext()) {
            renderer->grContext()->flush(pool[panelIdx].surface.get());
        }
        renderer->switchSurface(prev);
        panelIdx++;
    }
    systemDirty_ = false;
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
            // Canvas/WebGL layer — get texture from canvas scene or direct texture
            GLuint tex = 0;
            if (layer.canvasScene) {
                tex = layer.canvasScene->texture();
            } else {
                tex = layer.texture;  // WebGL direct texture
            }
            if (tex) {
                float cx = layer.cx, cy = layer.cy;
                float cw = layer.cw, ch = layer.ch;

                // WebGL textures are bottom-up (origin at lower-left) so flip V coords
                float v0 = layer.canvasScene ? 0.0f : 1.0f;
                float v1 = layer.canvasScene ? 1.0f : 0.0f;

                render::TextureVertex quad[6] = {
                    {cx,    cy,    0, v0}, {cx+cw, cy,    1, v0}, {cx+cw, cy+ch, 1, v1},
                    {cx,    cy,    0, v0}, {cx+cw, cy+ch, 1, v1}, {cx,    cy+ch, 0, v1},
                };
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
                glBindTexture(GL_TEXTURE_2D, tex);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }
    }
}

} // namespace bro::engine
