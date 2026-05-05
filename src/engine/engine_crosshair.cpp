#include "engine/engine.h"

#include "render/gl_context.h"
#include "render/skia_backend.h"

#include <glad/gl.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Crosshair spread system
// ---------------------------------------------------------------------------

void CrosshairConfig::tick(float dtSec) {
    if (dtSec <= 0.0f) return;

    // Compute target spread
    float target;
    if (manualSpread >= 0.0f) {
        // Manual override — use directly, skip interpolation
        currentSpread = manualSpread;
        // Still decay bloom so it's ready if we switch back to auto
        currentBloom = std::max(0.0f, currentBloom - bloomDecay * dtSec);
        return;
    }

    target = (aiming && adsSpread >= 0.0f) ? adsSpread : spread;
    if (moving) target += moveSpread;
    target += currentBloom;

    // Decay bloom
    currentBloom = std::max(0.0f, currentBloom - bloomDecay * dtSec);

    // Exponential lerp toward target
    float alpha = 1.0f - expf(-lerpSpeed * dtSec);
    currentSpread += (target - currentSpread) * alpha;
}

// ---------------------------------------------------------------------------
// Crosshair rendering
// ---------------------------------------------------------------------------

void Engine::drawCrosshairGL() {
    if (!crosshair_.visible || !gl_) return;

    float cx = viewportWidth_ * 0.5f;
    float cy = viewportHeight_ * 0.5f;
    float vw = static_cast<float>(viewportWidth_);
    float vh = static_cast<float>(viewportHeight_);

    // Build colored rectangles as ColorVertex triangles.
    // Each rect = 6 vertices (2 triangles).
    std::vector<render::ColorVertex> verts;
    verts.reserve(96); // outline + fill, up to ~16 rects

    auto pushRect = [&](float x, float y, float w, float h,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        float fr = r / 255.0f, fg = g / 255.0f, fb = b / 255.0f, fa = a / 255.0f;
        // Pre-multiply alpha for GL blending
        fr *= fa; fg *= fa; fb *= fa;
        float x2 = x + w, y2 = y + h;
        verts.push_back({x,  y,  fr, fg, fb, fa});
        verts.push_back({x2, y,  fr, fg, fb, fa});
        verts.push_back({x2, y2, fr, fg, fb, fa});
        verts.push_back({x,  y,  fr, fg, fb, fa});
        verts.push_back({x2, y2, fr, fg, fb, fa});
        verts.push_back({x,  y2, fr, fg, fb, fa});
    };

    auto pushCircle = [&](float ccx, float ccy, float radius, int segs,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        float fr = r / 255.0f, fg = g / 255.0f, fb = b / 255.0f, fa = a / 255.0f;
        fr *= fa; fg *= fa; fb *= fa;
        for (int i = 0; i < segs; i++) {
            float a0 = (float)i / segs * 6.2831853f;
            float a1 = (float)(i + 1) / segs * 6.2831853f;
            verts.push_back({ccx, ccy, fr, fg, fb, fa});
            verts.push_back({ccx + radius * cosf(a0), ccy + radius * sinf(a0), fr, fg, fb, fa});
            verts.push_back({ccx + radius * cosf(a1), ccy + radius * sinf(a1), fr, fg, fb, fa});
        }
    };

    auto pushRing = [&](float ccx, float ccy, float innerR, float outerR, int segs,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        float fr = r / 255.0f, fg = g / 255.0f, fb = b / 255.0f, fa = a / 255.0f;
        fr *= fa; fg *= fa; fb *= fa;
        for (int i = 0; i < segs; i++) {
            float a0 = (float)i / segs * 6.2831853f;
            float a1 = (float)(i + 1) / segs * 6.2831853f;
            float c0 = cosf(a0), s0 = sinf(a0);
            float c1 = cosf(a1), s1 = sinf(a1);
            // Two triangles per segment
            verts.push_back({ccx + innerR * c0, ccy + innerR * s0, fr, fg, fb, fa});
            verts.push_back({ccx + outerR * c0, ccy + outerR * s0, fr, fg, fb, fa});
            verts.push_back({ccx + outerR * c1, ccy + outerR * s1, fr, fg, fb, fa});
            verts.push_back({ccx + innerR * c0, ccy + innerR * s0, fr, fg, fb, fa});
            verts.push_back({ccx + outerR * c1, ccy + outerR * s1, fr, fg, fb, fa});
            verts.push_back({ccx + innerR * c1, ccy + innerR * s1, fr, fg, fb, fa});
        }
    };

    auto& ch = crosshair_;
    float ht = ch.thickness * 0.5f;
    float ot = ch.outline ? ch.outlineThickness : 0.0f;

    // Helper: emit cross arms (4 rectangles) with given color + optional outline expansion
    auto emitCrossArms = [&](float expand, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        float e = expand;
        // Right arm
        pushRect(cx + ch.currentSpread - e, cy - ht - e,
                 ch.size - ch.currentSpread + 2*e, ch.thickness + 2*e, r, g, b, a);
        // Left arm
        pushRect(cx - ch.size - e, cy - ht - e,
                 ch.size - ch.currentSpread + 2*e, ch.thickness + 2*e, r, g, b, a);
        // Bottom arm
        pushRect(cx - ht - e, cy + ch.currentSpread - e,
                 ch.thickness + 2*e, ch.size - ch.currentSpread + 2*e, r, g, b, a);
        // Top arm
        pushRect(cx - ht - e, cy - ch.size - e,
                 ch.thickness + 2*e, ch.size - ch.currentSpread + 2*e, r, g, b, a);
    };

    bool hasCross = (ch.style == CrosshairConfig::Cross || ch.style == CrosshairConfig::CrossDot);
    bool hasDot = (ch.style == CrosshairConfig::Dot || ch.style == CrosshairConfig::CrossDot);
    bool hasCircle = (ch.style == CrosshairConfig::Circle);
    int circleSegs = 32;

    // Outline pass
    if (ch.outline) {
        if (hasCross) {
            emitCrossArms(ot, ch.outR, ch.outG, ch.outB, ch.outA);
        }
        if (hasDot) {
            pushCircle(cx, cy, ch.dotSize + ot, circleSegs,
                       ch.outR, ch.outG, ch.outB, ch.outA);
        }
        if (hasCircle) {
            pushRing(cx, cy, ch.size - ch.thickness * 0.5f - ot,
                     ch.size + ch.thickness * 0.5f + ot, circleSegs,
                     ch.outR, ch.outG, ch.outB, ch.outA);
        }
    }

    // Fill pass
    if (hasCross) {
        emitCrossArms(0, ch.r, ch.g, ch.b, ch.a);
    }
    if (hasDot) {
        pushCircle(cx, cy, ch.dotSize, circleSegs, ch.r, ch.g, ch.b, ch.a);
    }
    if (hasCircle) {
        pushRing(cx, cy, ch.size - ch.thickness * 0.5f,
                 ch.size + ch.thickness * 0.5f, circleSegs,
                 ch.r, ch.g, ch.b, ch.a);
    }

    if (verts.empty()) return;

    // Draw using color pipeline
    glUseProgram(gl_->colorProgram());
    float viewport[2] = {vw, vh};
    glUniform2fv(gl_->colorViewportLoc(), 1, viewport);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(uiQuadVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(render::ColorVertex)),
                 verts.data(), GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          sizeof(render::ColorVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE,
                          sizeof(render::ColorVertex),
                          (void*)offsetof(render::ColorVertex, r));

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
}

void Engine::drawCrosshairSkia(SkCanvas* canvas) {
    if (!crosshair_.visible || !canvas) return;

    float cx = viewportWidth_ * 0.5f;
    float cy = viewportHeight_ * 0.5f;
    auto& ch = crosshair_;
    float ht = ch.thickness * 0.5f;
    float ot = ch.outline ? ch.outlineThickness : 0.0f;

    bool hasCross = (ch.style == CrosshairConfig::Cross || ch.style == CrosshairConfig::CrossDot);
    bool hasDot = (ch.style == CrosshairConfig::Dot || ch.style == CrosshairConfig::CrossDot);
    bool hasCircle = (ch.style == CrosshairConfig::Circle);

    auto drawCrossArms = [&](float expand, SkPaint& paint) {
        float e = expand;
        // Right
        canvas->drawRect(SkRect::MakeXYWH(cx + ch.currentSpread - e, cy - ht - e,
                         ch.size - ch.currentSpread + 2*e, ch.thickness + 2*e), paint);
        // Left
        canvas->drawRect(SkRect::MakeXYWH(cx - ch.size - e, cy - ht - e,
                         ch.size - ch.currentSpread + 2*e, ch.thickness + 2*e), paint);
        // Bottom
        canvas->drawRect(SkRect::MakeXYWH(cx - ht - e, cy + ch.currentSpread - e,
                         ch.thickness + 2*e, ch.size - ch.currentSpread + 2*e), paint);
        // Top
        canvas->drawRect(SkRect::MakeXYWH(cx - ht - e, cy - ch.size - e,
                         ch.thickness + 2*e, ch.size - ch.currentSpread + 2*e), paint);
    };

    // Outline
    if (ch.outline) {
        SkPaint outPaint;
        outPaint.setAntiAlias(true);
        outPaint.setColor(SkColorSetARGB(ch.outA, ch.outR, ch.outG, ch.outB));
        if (hasCross) drawCrossArms(ot, outPaint);
        if (hasDot) {
            outPaint.setStyle(SkPaint::kFill_Style);
            canvas->drawCircle(cx, cy, ch.dotSize + ot, outPaint);
        }
        if (hasCircle) {
            outPaint.setStyle(SkPaint::kStroke_Style);
            outPaint.setStrokeWidth(ch.thickness + 2 * ot);
            canvas->drawCircle(cx, cy, ch.size, outPaint);
        }
    }

    // Fill
    SkPaint fillPaint;
    fillPaint.setAntiAlias(true);
    fillPaint.setColor(SkColorSetARGB(ch.a, ch.r, ch.g, ch.b));
    if (hasCross) drawCrossArms(0, fillPaint);
    if (hasDot) {
        fillPaint.setStyle(SkPaint::kFill_Style);
        canvas->drawCircle(cx, cy, ch.dotSize, fillPaint);
    }
    if (hasCircle) {
        fillPaint.setStyle(SkPaint::kStroke_Style);
        fillPaint.setStrokeWidth(ch.thickness);
        canvas->drawCircle(cx, cy, ch.size, fillPaint);
    }
}

} // namespace bro::engine
