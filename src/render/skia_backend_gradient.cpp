// SkiaRenderer's paint servers: CSS linear / radial / conic gradient fills,
// and SVG paths filled and stroked with solid or gradient paint.

#include "render/skia_backend.h"

#include <include/core/SkPath.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/effects/SkGradient.h>
#include <include/utils/SkParsePath.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace bro::render {

// CSS spec: gradient color interpolation is in premultiplied sRGB by
// default. Without premul, fading from an opaque color to `transparent`
// (rgba(0,0,0,0)) interpolates each RGB channel toward 0 — so a blue→
// transparent gradient passes through dark gray on its way out, which
// looks dim and the soft "glow" extent appears much smaller than in
// browsers. With premul, the color stays the same hue while alpha drops,
// matching Chromium/Firefox.
static const SkGradient::Interpolation kCSSGradInterp = {
    SkGradient::Interpolation::InPremul::kYes,
    SkGradient::Interpolation::ColorSpace::kDestination,
    SkGradient::Interpolation::HueMethod::kShorter,
};

// Helper: build SkGradient::Colors from our ColorStop span
static SkGradient::Colors buildGradColors(std::span<const ColorStop> stops) {
    // Store in thread-local vectors to keep spans valid for the shader call
    thread_local std::vector<SkColor4f> colors;
    thread_local std::vector<float> pos;
    colors.resize(stops.size());
    pos.resize(stops.size());
    for (size_t i = 0; i < stops.size(); i++) {
        const auto& c = stops[i].color;
        colors[i] = SkColor4f{bromath::clinearToSrgb(c.r),
                              bromath::clinearToSrgb(c.g),
                              bromath::clinearToSrgb(c.b),
                              c.a};
        pos[i] = stops[i].offset;
    }
    return SkGradient::Colors(SkSpan(colors), SkSpan(pos), SkTileMode::kClamp);
}

void SkiaRenderer::fillLinearGradient(float x, float y, float w, float h,
                                      float startX, float startY, float endX, float endY,
                                      std::span<const ColorStop> stops) {
    if (!canvas_ || stops.empty()) return;
    SkPoint pts[2] = { {startX, startY}, {endX, endY} };
    auto shader = SkShaders::LinearGradient(pts, SkGradient(buildGradColors(stops), kCSSGradInterp));
    SkPaint paint;
    paint.setShader(shader);
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->save();
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    canvas_->restore();
}

void SkiaRenderer::fillRadialGradient(float x, float y, float w, float h,
                                      float cx, float cy, float rx, float ry,
                                      std::span<const ColorStop> stops) {
    if (!canvas_ || stops.empty()) return;
    float r = std::max(rx, ry);
    if (r < 0.001f) r = 0.001f;
    SkPaint paint;
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->save();
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
    if (std::abs(rx - ry) > 0.001f && rx > 0 && ry > 0) {
        // Ellipse gradient: scale the canvas vertically around the center so a
        // circle of radius rx draws as an ellipse with vertical radius ry. The
        // drawRect runs AFTER the scale, so its Y range must be expanded by
        // the inverse scale factor to still cover the entire (clipped) box.
        // Without this, the rect ends up vertically shrunk and corners are
        // not filled by the gradient.
        canvas_->translate(cx, cy);
        canvas_->scale(1.0f, ry / rx);
        canvas_->translate(-cx, -cy);
        paint.setShader(SkShaders::RadialGradient({cx, cy}, rx,
            SkGradient(buildGradColors(stops), kCSSGradInterp)));
        float invS = rx / ry;
        float pyTop    = cy + (y - cy) * invS;
        float pyBottom = cy + ((y + h) - cy) * invS;
        canvas_->drawRect(SkRect::MakeLTRB(x, pyTop, x + w, pyBottom), paint);
    } else {
        paint.setShader(SkShaders::RadialGradient({cx, cy}, r,
            SkGradient(buildGradColors(stops), kCSSGradInterp)));
        canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    }
    canvas_->restore();
}

void SkiaRenderer::fillConicGradient(float x, float y, float w, float h,
                                     float cx, float cy, float angleDeg,
                                     std::span<const ColorStop> stops) {
    if (!canvas_ || stops.empty()) return;
    // Skia sweep: 0° = 3 o'clock, pos 0..1 maps across [startAngle, endAngle],
    // and pixel angles are measured in [0, 360). CSS conic: 0° = 12 o'clock,
    // clockwise. Rotating the sweep by moving startAngle negative would push
    // the [270, 360) arc outside the range (clamped to the last color), so
    // instead keep the full [0, 360] range and rotate the pattern with a local
    // matrix: -90° aligns pos 0 with 12 o'clock, plus the CSS "from" angle.
    SkMatrix lm = SkMatrix::RotateDeg(angleDeg - 90.0f, {cx, cy});
    auto shader = SkShaders::SweepGradient({cx, cy}, 0.0f, 360.0f,
        SkGradient(buildGradColors(stops), kCSSGradInterp), &lm);
    SkPaint paint;
    paint.setShader(shader);
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->save();
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    canvas_->restore();
}

// Build an SkGradient::Colors from ColorStops with an explicit tile mode (SVG
// spreadMethod). Mirrors buildGradColors but lets the caller pick the mode.
static SkGradient::Colors buildGradColorsTiled(std::span<const ColorStop> stops, SkTileMode tm) {
    thread_local std::vector<SkColor4f> colors;
    thread_local std::vector<float> pos;
    colors.resize(stops.size());
    pos.resize(stops.size());
    for (size_t i = 0; i < stops.size(); i++) {
        const auto& c = stops[i].color;
        colors[i] = SkColor4f{bromath::clinearToSrgb(c.r), bromath::clinearToSrgb(c.g),
                              bromath::clinearToSrgb(c.b), c.a};
        pos[i] = stops[i].offset;
    }
    return SkGradient::Colors(SkSpan(colors), SkSpan(pos), tm);
}

static SkTileMode toTileMode(GradientPaint::Spread s) {
    switch (s) {
        case GradientPaint::Spread::Reflect: return SkTileMode::kMirror;
        case GradientPaint::Spread::Repeat:  return SkTileMode::kRepeat;
        default:                             return SkTileMode::kClamp;
    }
}

// Build a gradient SkShader for an SVG paint server (geometry already in user
// space; `transform` is the SVG gradientTransform). Returns null for solid/none.
static sk_sp<SkShader> makeSvgShader(const GradientPaint& gp, std::span<const ColorStop> stops) {
    if (stops.empty()) return nullptr;
    SkMatrix lm = SkMatrix::MakeAll(gp.transform[0], gp.transform[2], gp.transform[4],
                                    gp.transform[1], gp.transform[3], gp.transform[5],
                                    0, 0, 1);
    SkTileMode tm = toTileMode(gp.spread);
    if (gp.kind == GradientPaint::Kind::Linear) {
        SkPoint pts[2] = {{gp.p0[0], gp.p0[1]}, {gp.p1[0], gp.p1[1]}};
        return SkShaders::LinearGradient(pts,
            SkGradient(buildGradColorsTiled(stops, tm), kCSSGradInterp), &lm);
    }
    if (gp.kind == GradientPaint::Kind::Radial) {
        float r = gp.radius > 0.0001f ? gp.radius : 0.0001f;
        SkPoint center{gp.center[0], gp.center[1]};
        if (gp.hasFocal) {
            SkPoint focal{gp.focal[0], gp.focal[1]};
            return SkShaders::TwoPointConicalGradient(focal, 0.0f, center, r,
                SkGradient(buildGradColorsTiled(stops, tm), kCSSGradInterp), &lm);
        }
        return SkShaders::RadialGradient(center, r,
            SkGradient(buildGradColorsTiled(stops, tm), kCSSGradInterp), &lm);
    }
    return nullptr;
}

void SkiaRenderer::drawSvgPath(std::string_view d, PathFillRule rule,
                               const GradientPaint& fill, std::span<const ColorStop> fillStops,
                               const GradientPaint& stroke, std::span<const ColorStop> strokeStops,
                               const StrokeStyle& ss, std::span<const float> dash) {
    if (!canvas_ || d.empty()) return;
    auto pathOpt = SkParsePath::FromSVGString(std::string(d).c_str());
    if (!pathOpt) return;
    SkPath path = *pathOpt;
    path.setFillType(rule == PathFillRule::EvenOdd ? SkPathFillType::kEvenOdd
                                                   : SkPathFillType::kWinding);
    // Fill
    if (fill.kind != GradientPaint::Kind::None) {
        SkPaint paint;
        paint.setStyle(SkPaint::kFill_Style);
        paint.setAntiAlias(true);
        if (fill.kind == GradientPaint::Kind::Solid) {
            if (fill.color.a > 0) { paint.setColor(toSkColor(fill.color)); canvas_->drawPath(path, paint); }
        } else if (auto sh = makeSvgShader(fill, fillStops)) {
            paint.setShader(sh); canvas_->drawPath(path, paint);
        }
    }
    // Stroke
    if (stroke.kind != GradientPaint::Kind::None && ss.width > 0) {
        SkPaint paint;
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setAntiAlias(true);
        paint.setStrokeWidth(ss.width);
        paint.setStrokeCap(ss.cap == StrokeStyle::Cap::Round  ? SkPaint::kRound_Cap
                         : ss.cap == StrokeStyle::Cap::Square ? SkPaint::kSquare_Cap
                                                              : SkPaint::kButt_Cap);
        paint.setStrokeJoin(ss.join == StrokeStyle::Join::Round ? SkPaint::kRound_Join
                          : ss.join == StrokeStyle::Join::Bevel ? SkPaint::kBevel_Join
                                                                : SkPaint::kMiter_Join);
        paint.setStrokeMiter(ss.miterLimit);
        if (dash.size() >= 2) {
            paint.setPathEffect(SkDashPathEffect::Make(SkSpan(dash.data(), dash.size()), ss.dashOffset));
        }
        if (stroke.kind == GradientPaint::Kind::Solid) {
            if (stroke.color.a > 0) { paint.setColor(toSkColor(stroke.color)); canvas_->drawPath(path, paint); }
        } else if (auto sh = makeSvgShader(stroke, strokeStops)) {
            paint.setShader(sh); canvas_->drawPath(path, paint);
        }
    }
}

void SkiaRenderer::clipSvgPath(std::string_view d, PathFillRule rule) {
    if (!canvas_ || d.empty()) return;
    auto pathOpt = SkParsePath::FromSVGString(std::string(d).c_str());
    if (!pathOpt) return;
    SkPath path = *pathOpt;
    path.setFillType(rule == PathFillRule::EvenOdd ? SkPathFillType::kEvenOdd
                                                   : SkPathFillType::kWinding);
    canvas_->clipPath(path, SkClipOp::kIntersect, /*doAntiAlias=*/true);
}

} // namespace bro::render
