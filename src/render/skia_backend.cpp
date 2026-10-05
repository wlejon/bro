#include "render/skia_backend.h"
#include "render/filter_chain.h"
#include "render/system_font_mgr.h"
#include "svg/svg_renderer.h"
#include "util/log.h"

#include <cstring>
#include <sstream>
#include <cmath>

#include <include/core/SkBitmap.h>
#include <include/core/SkM44.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontMetrics.h>
#include <include/codec/SkCodec.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/utils/SkParsePath.h>
#include <include/core/SkImageFilter.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkBlurTypes.h>
#include <include/private/chromium/GrDeferredDisplayListRecorder.h>
#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#elif defined(__APPLE__)
#include <include/ports/SkFontMgr_mac_ct.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif

namespace bro::render {

using bromath::Color;

// ===========================================================================
// SkiaRenderer — Skia raster rendering
// ===========================================================================
//
// Drawing primitives, text, clips and layers live here. The frame and layer
// surfaces and pixel readback are in skia_backend_surfaces.cpp; gradient fills and
// SVG paint servers are in skia_backend_gradient.cpp.

SkiaRenderer::SkiaRenderer() = default;

SkiaRenderer::~SkiaRenderer() {
    // Shaped runs hold SkFonts derived from fonts_ — drop them first.
    shaper_.clear();
    fonts_.clear();
    // GPU objects go under the context lock.
    if (gpu_) {
        evictGpuImages(/*all=*/true);
        SkiaGpu::Lock lock = gpu_->lock();
        recorder_.reset();
        touched_.clear();
        afterSubmit_.clear();
    }
    surface_.reset();
}

SkColor SkiaRenderer::toSkColor(Color c) const {
    // Skia consumes sRGB-packed ARGB; convert from linear-float at the boundary.
    bromath::Color8 p = bromath::ctoColor8(c);
    return SkColorSetARGB(p.a, p.r, p.g, p.b);
}

void SkiaRenderer::clear(Color color) {
    if (canvas_) canvas_->clear(toSkColor(color));
}

void SkiaRenderer::drawRect(float x, float y, float w, float h, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

void SkiaRenderer::drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry), paint);
}

void SkiaRenderer::fillRect(float x, float y, float w, float h, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

const ShapedRun* SkiaRenderer::shapeText(std::string_view text, FontRef font,
                                   bool disableLigatures, TextDirection direction) {
    const FontEntry* fe = getOrCreateFont(font);
    if (!fe) return nullptr;
    return shaper_.shape(text, *fe->font, font.family, fe->style,
                         ensureFontMgr(), fallbackCache_,
                         direction, disableLigatures);
}

void SkiaRenderer::drawText(std::string_view text, float x, float y, FontRef font, Color color,
                            TextDirection direction) {
    drawTextEx(text, x, y, font, color, 0.0f, 0.0f, 0.0f, direction);
}

void SkiaRenderer::drawTextEx(std::string_view text, float x, float y,
                              FontRef font, Color color,
                              float letterSpacing, float blur,
                              float wordSpacing, TextDirection direction) {
    if (!canvas_ || text.empty()) return;
    const ShapedRun* run = shapeText(text, font, letterSpacing != 0.0f, direction);
    if (!run) return;
    sk_sp<SkTextBlob> blob = run->makeBlob(Spacing{letterSpacing, wordSpacing});
    if (!blob) return;

    SkPaint paint;
    paint.setColor(toSkColor(color));
    if (blur > 0) {
        // Skia sigma matches CSS blur radius / 2 (the same convention used
        // by drawBoxShadow). MakeBlur draws a Gaussian falloff over the
        // glyph mask, producing the text-shadow halo.
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    }
    canvas_->drawTextBlob(blob, x, y, paint);
}

bool SkiaRenderer::drawTextBlob(const SkTextBlob* blob, float x, float y,
                                Color color, float blur) {
    if (!canvas_ || !blob) return true;  // nothing to draw, but handled
    SkPaint paint;
    paint.setColor(toSkColor(color));
    if (blur > 0) {
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    }
    canvas_->drawTextBlob(blob, x, y, paint);
    return true;
}

TextMetrics SkiaRenderer::measureText(std::string_view text, FontRef font,
                                      TextDirection direction) {
    const FontEntry* fePtr = getOrCreateFont(font);
    if (!fePtr) return {};
    SkFontMetrics fm;
    fePtr->font->getMetrics(&fm);
    if (text.empty()) {
        return { 0.0f, 0.0f, -fm.fAscent, fm.fDescent, fm.fLeading, fm.fXHeight };
    }
    const ShapedRun* run = shapeText(text, font, false, direction);
    if (!run) return { 0.0f, 0.0f, -fm.fAscent, fm.fDescent, fm.fLeading, fm.fXHeight };
    return { run->width(), run->bounds().height(),
             -fm.fAscent, fm.fDescent, fm.fLeading, fm.fXHeight };
}

SkFontMgr* SkiaRenderer::ensureFontMgr() {
    if (fontMgr_) return fontMgr_.get();
    fontMgr_ = sk_ref_sp(systemFontMgr());
    return fontMgr_.get();
}

const SkiaRenderer::FontEntry* SkiaRenderer::getOrCreateFont(FontRef ref) {
    FontKey key{std::string(ref.family), ref.size, ref.weight, ref.italic};
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return &it->second;

    SkFontStyle style(ref.weight,
                      SkFontStyle::kNormal_Width,
                      ref.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);

    // Reuse the renderer-wide SkFontMgr so per-glyph fallback (matchFamily-
    // StyleCharacter in font_fallback.cpp) shares the same system manager.
    SkFontMgr* font_mgr = ensureFontMgr();

    // Map CSS generic family names to real font names
    auto resolveGeneric = [](const std::string& name) -> const char* {
#ifdef _WIN32
        if (name == "sans-serif")  return "Arial";
        if (name == "serif")       return "Times New Roman";
        if (name == "monospace")   return "Consolas";
        if (name == "cursive")     return "Comic Sans MS";
        if (name == "fantasy")     return "Impact";
        if (name == "system-ui")   return "Segoe UI";
#elif defined(__APPLE__)
        // macOS: prefer Arial/Times for sans-serif/serif because Skia's
        // CoreText backend reports OS/2 metrics that match Chromium's
        // line-height: normal exactly. Helvetica's hhea-derived metrics
        // come back tighter and break parity.
        if (name == "sans-serif")  return "Arial";
        if (name == "serif")       return "Times New Roman";
        if (name == "monospace")   return "Menlo";
        if (name == "cursive")     return "Apple Chancery";
        if (name == "fantasy")     return "Papyrus";
        if (name == "system-ui")   return "Helvetica Neue";
#else
        if (name == "sans-serif")  return "Liberation Sans";
        if (name == "serif")       return "Liberation Serif";
        if (name == "monospace")   return "Liberation Mono";
        if (name == "cursive")     return "DejaVu Sans";
        if (name == "fantasy")     return "DejaVu Sans";
        if (name == "system-ui")   return "Liberation Sans";
#endif
        return nullptr;
    };

    sk_sp<SkTypeface> typeface;
    std::istringstream stream{std::string(ref.family)};
    std::string name;
    while (std::getline(stream, name, ',')) {
        while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
        if (name.empty()) continue;
        // Check custom fonts first (@font-face registered)
        for (auto& cf : customFonts_) {
            if (cf.family == name) {
                typeface = cf.typeface;
                break;
            }
        }
        if (typeface) break;
        // Try CSS generic name
        const char* resolved = resolveGeneric(name);
        if (resolved) {
            typeface = font_mgr->matchFamilyStyle(resolved, style);
            if (typeface) break;
        }
        typeface = font_mgr->matchFamilyStyle(name.c_str(), style);
        if (typeface) break;
    }
    if (!typeface) {
        typeface = font_mgr->matchFamilyStyle(nullptr, SkFontStyle());
    }

    auto sk_font = std::make_unique<SkFont>(typeface, ref.size);
    sk_font->setEdging(SkFont::Edging::kAntiAlias);
    // Subpixel advances. HarfBuzz asks the SkFont for glyph widths and rounds
    // each one to a whole pixel unless the font says it wants subpixel
    // positioning (SkShaper_harfbuzz.cpp's skhb_glyph_h_advance), which
    // quantizes every advance and drifts a run's width away from what the
    // font actually specifies. Browsers position text subpixel; so do we.
    sk_font->setSubpixel(true);

    auto [ins, _] = fonts_.emplace(std::move(key),
        FontEntry{std::move(typeface), std::move(sk_font), style});
    return &ins->second;
}

void SkiaRenderer::drawLine(float x1, float y1, float x2, float y2, Color color, float thickness) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStrokeWidth(thickness);
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawLine(x1, y1, x2, y2, paint);
}

void SkiaRenderer::fillRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry), paint);
}

// Build an SkRRect with per-corner radii. Order in Radii: TL, TR, BR, BL —
// matches SkRRect::Corner enum.
static SkRRect makeRRect(float x, float y, float w, float h, const Radii& r) {
    SkVector radii[4] = {
        {r.x[0], r.y[0]}, {r.x[1], r.y[1]},
        {r.x[2], r.y[2]}, {r.x[3], r.y[3]}
    };
    SkRRect rr;
    rr.setRectRadii(SkRect::MakeXYWH(x, y, w, h), radii);
    return rr;
}

void SkiaRenderer::fillRoundRectRadii(float x, float y, float w, float h,
                                      const Radii& r, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kFill_Style);
    paint.setAntiAlias(true);
    canvas_->drawRRect(makeRRect(x, y, w, h, r), paint);
}

void SkiaRenderer::drawRoundRectRadii(float x, float y, float w, float h,
                                      const Radii& r, float strokeWidth, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(strokeWidth);
    paint.setAntiAlias(true);
    canvas_->drawRRect(makeRRect(x, y, w, h, r), paint);
}

void SkiaRenderer::setClipRRect(float x, float y, float w, float h, const Radii& r) {
    if (!canvas_) return;
    canvas_->clipRRect(makeRRect(x, y, w, h, r), true /*antialias*/);
}

void SkiaRenderer::drawBoxShadowRadii(float x, float y, float w, float h,
                                      const Radii& r,
                                      float offsetX, float offsetY,
                                      float blur, float spread,
                                      Color color, bool inset) {
    if (!canvas_) return;
    if (r.isZero()) {
        drawBoxShadow(x, y, w, h, 0, 0, offsetX, offsetY, blur, spread, color, inset);
        return;
    }
    if (inset) {
        canvas_->save();
        canvas_->clipRRect(makeRRect(x, y, w, h, r), true);

        float ix = x + offsetX + spread;
        float iy = y + offsetY + spread;
        float iw = w - spread * 2;
        float ih = h - spread * 2;
        Radii ir = r;
        for (int i = 0; i < 4; ++i) {
            ir.x[i] = std::max(0.0f, r.x[i] - spread);
            ir.y[i] = std::max(0.0f, r.y[i] - spread);
        }

        float pad = blur * 2 + 100;
        SkRect outerRect = SkRect::MakeXYWH(x - pad, y - pad, w + pad * 2, h + pad * 2);

        SkPathBuilder pb;
        pb.addRect(outerRect);
        pb.addRRect(makeRRect(ix, iy, iw, ih, ir));
        pb.setFillType(SkPathFillType::kEvenOdd);
        SkPath path = pb.detach();

        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(toSkColor(color));
        if (blur > 0)
            paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
        canvas_->drawPath(path, paint);
        canvas_->restore();
        return;
    }
    // Outer shadow: shift, expand by spread, expand corner radii similarly.
    float sx = x + offsetX - spread;
    float sy = y + offsetY - spread;
    float sw = w + spread * 2;
    float sh = h + spread * 2;
    Radii sr = r;
    for (int i = 0; i < 4; ++i) {
        sr.x[i] = std::max(0.0f, r.x[i] + spread);
        sr.y[i] = std::max(0.0f, r.y[i] + spread);
    }
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(toSkColor(color));
    if (blur > 0)
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    // Knock the border box out (see drawBoxShadow) so the shadow stays outside
    // the element and doesn't show through a transparent background.
    canvas_->save();
    canvas_->clipRRect(makeRRect(x, y, w, h, r), SkClipOp::kDifference, true);
    canvas_->drawRRect(makeRRect(sx, sy, sw, sh, sr), paint);
    canvas_->restore();
}

void SkiaRenderer::drawCircle(float cx, float cy, float r,
                               Color fill, Color stroke, float strokeWidth) {
    if (!canvas_) return;
    if (fill.a > 0) {
        SkPaint paint;
        paint.setColor(toSkColor(fill));
        paint.setStyle(SkPaint::kFill_Style);
        paint.setAntiAlias(true);
        canvas_->drawCircle(cx, cy, r, paint);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint paint;
        paint.setColor(toSkColor(stroke));
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(strokeWidth);
        paint.setAntiAlias(true);
        canvas_->drawCircle(cx, cy, r, paint);
    }
}

void SkiaRenderer::drawEllipse(float cx, float cy, float rx, float ry,
                                Color fill, Color stroke, float strokeWidth) {
    if (!canvas_) return;
    SkRect oval = SkRect::MakeXYWH(cx - rx, cy - ry, rx * 2, ry * 2);
    if (fill.a > 0) {
        SkPaint paint;
        paint.setColor(toSkColor(fill));
        paint.setStyle(SkPaint::kFill_Style);
        paint.setAntiAlias(true);
        canvas_->drawOval(oval, paint);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint paint;
        paint.setColor(toSkColor(stroke));
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(strokeWidth);
        paint.setAntiAlias(true);
        canvas_->drawOval(oval, paint);
    }
}

void SkiaRenderer::drawPath(std::string_view svgPathData,
                             Color fill, Color stroke, float strokeWidth) {
    if (!canvas_ || svgPathData.empty()) return;
    auto pathOpt = SkParsePath::FromSVGString(std::string(svgPathData).c_str());
    if (!pathOpt) return;
    const SkPath& path = *pathOpt;
    if (fill.a > 0) {
        SkPaint paint;
        paint.setColor(toSkColor(fill));
        paint.setStyle(SkPaint::kFill_Style);
        paint.setAntiAlias(true);
        canvas_->drawPath(path, paint);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint paint;
        paint.setColor(toSkColor(stroke));
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(strokeWidth);
        paint.setAntiAlias(true);
        canvas_->drawPath(path, paint);
    }
}

void SkiaRenderer::drawPolygon(std::span<const PointF> points,
                                Color fill, Color stroke, float strokeWidth) {
    if (!canvas_ || points.size() < 2) return;
    SkPathBuilder builder;
    builder.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); i++)
        builder.lineTo(points[i].x, points[i].y);
    builder.close();
    SkPath path = builder.detach();
    if (fill.a > 0) {
        SkPaint paint;
        paint.setColor(toSkColor(fill));
        paint.setStyle(SkPaint::kFill_Style);
        paint.setAntiAlias(true);
        canvas_->drawPath(path, paint);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint paint;
        paint.setColor(toSkColor(stroke));
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(strokeWidth);
        paint.setAntiAlias(true);
        canvas_->drawPath(path, paint);
    }
}

void SkiaRenderer::drawPolyline(std::span<const PointF> points,
                                 Color stroke, float strokeWidth) {
    if (!canvas_ || points.size() < 2) return;
    SkPathBuilder builder;
    builder.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); i++)
        builder.lineTo(points[i].x, points[i].y);
    SkPath path = builder.detach();
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint paint;
        paint.setColor(toSkColor(stroke));
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(strokeWidth);
        paint.setAntiAlias(true);
        canvas_->drawPath(path, paint);
    }
}

void SkiaRenderer::drawBoxShadow(float x, float y, float w, float h,
                                 float rx, float ry,
                                 float offsetX, float offsetY,
                                 float blur, float spread,
                                 Color color, bool inset) {
    if (!canvas_) return;

    if (inset) {
        // Inset shadow: clip to element bounds, then draw a large rect with a
        // hole cut out (the hole is the element rect shrunk by spread and offset).
        canvas_->save();
        SkRect clipRect = SkRect::MakeXYWH(x, y, w, h);
        if (rx > 0 || ry > 0)
            canvas_->clipRRect(SkRRect::MakeRectXY(clipRect, rx, ry));
        else
            canvas_->clipRect(clipRect);

        // Inner hole: offset inward, shrunk by spread
        float ix = x + offsetX + spread;
        float iy = y + offsetY + spread;
        float iw = w - spread * 2;
        float ih = h - spread * 2;
        float ir = std::max(0.0f, rx - spread);

        // Outer rect (large enough to cover blur extent outside clip)
        float pad = blur * 2 + 100;
        SkRect outerRect = SkRect::MakeXYWH(x - pad, y - pad, w + pad * 2, h + pad * 2);

        SkPathBuilder pb;
        pb.addRect(outerRect);
        if (ir > 0)
            pb.addRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(ix, iy, iw, ih), ir, ir));
        else
            pb.addRect(SkRect::MakeXYWH(ix, iy, iw, ih));
        pb.setFillType(SkPathFillType::kEvenOdd);
        SkPath path = pb.detach();

        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(toSkColor(color));
        if (blur > 0)
            paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
        canvas_->drawPath(path, paint);
        canvas_->restore();
        return;
    }

    // Outer shadow
    float sx = x + offsetX - spread;
    float sy = y + offsetY - spread;
    float sw = w + spread * 2;
    float sh = h + spread * 2;

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(toSkColor(color));
    if (blur > 0) {
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    }

    // Knock the border box out of the shadow: CSS paints an outer box-shadow
    // only outside the border box, so it never shows through a transparent
    // element background.
    canvas_->save();
    SkRect borderBox = SkRect::MakeXYWH(x, y, w, h);
    if (rx > 0 || ry > 0)
        canvas_->clipRRect(SkRRect::MakeRectXY(borderBox, rx, ry), SkClipOp::kDifference, true);
    else
        canvas_->clipRect(borderBox, SkClipOp::kDifference, true);

    if (rx > 0 || ry > 0)
        canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(sx, sy, sw, sh), rx, ry), paint);
    else
        canvas_->drawRect(SkRect::MakeXYWH(sx, sy, sw, sh), paint);
    canvas_->restore();
}

void SkiaRenderer::save() {
    if (canvas_) canvas_->save();
}

void SkiaRenderer::restore() {
    if (canvas_) canvas_->restore();
}

void SkiaRenderer::saveLayerAlpha(uint8_t alpha) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setAlphaf(alpha / 255.0f);
    canvas_->saveLayer(nullptr, &paint);
}

void SkiaRenderer::translate(float dx, float dy) {
    if (canvas_) canvas_->translate(dx, dy);
}

void SkiaRenderer::scale(float sx, float sy) {
    if (canvas_) canvas_->scale(sx, sy);
}

void SkiaRenderer::rotate(float degrees) {
    if (canvas_) canvas_->rotate(degrees);
}

bool SkiaRenderer::registerCustomFont(const std::string& family,
                                       const void* data, size_t len,
                                       int weight, bool italic) {
    auto skData = SkData::MakeWithCopy(data, len);
    auto typeface = SkFontMgr::RefEmpty()->makeFromData(skData);
    if (!typeface) {
        typeface = ensureFontMgr()->makeFromData(skData);
    }
    if (!typeface) return false;
    customFonts_.push_back({family, weight, italic, typeface});
    // The same descriptor can now resolve to a different face, so every run
    // shaped against the old one is stale.
    shaper_.clear();
    noteFontRegistered();
    return true;
}

void SkiaRenderer::saveLayerWithFilter(std::span<const CssFilterParams> filters,
                                       float x, float y, float w, float h) {
    if (!canvas_) return;
    SkPaint paint;
    auto imgFilter = BuildSkImageFilterChain(filters);
    paint.setImageFilter(imgFilter);
    // Expand the layer bounds to the filter's output extent so effects that
    // paint outside the element box — a drop-shadow offset, a blur's spread —
    // aren't clipped away. computeFastBounds accounts for offset + blur.
    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    if (imgFilter) bounds = imgFilter->computeFastBounds(bounds);
    canvas_->saveLayer(SkCanvas::SaveLayerRec(&bounds, &paint));
}

void SkiaRenderer::saveLayerWithBlend(BlendMode mode) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setBlendMode(toSkBlendMode(mode));
    canvas_->saveLayer(nullptr, &paint);
}

void SkiaRenderer::concat4x4(const float m[16]) {
    if (!canvas_) return;
    SkM44 mat(m[0], m[4], m[ 8], m[12],
              m[1], m[5], m[ 9], m[13],
              m[2], m[6], m[10], m[14],
              m[3], m[7], m[11], m[15]);
    canvas_->concat(mat);
}

void SkiaRenderer::concat(float a, float b, float c, float d, float e, float f) {
    if (!canvas_) return;
    // CSS matrix(a,b,c,d,e,f) maps to SkMatrix:
    //   [a c e]     SkMatrix uses row-major: [scaleX skewX transX]
    //   [b d f]                               [skewY scaleY transY]
    //   [0 0 1]                               [persp0 persp1 persp2]
    SkMatrix m = SkMatrix::MakeAll(a, c, e,
                                   b, d, f,
                                   0, 0, 1);
    canvas_->concat(m);
}

void SkiaRenderer::drawImage(const void* data, size_t len, float x, float y, float w, float h,
                             uint64_t imageId) {
    if (!canvas_) return;
    // Decoding happens once per image id; subsequent frames reuse the SkImage.
    sk_sp<SkImage> image = imageCache_.resolve(imageId, data, len);
    if (!image) return;
    if (recorder_ && imageId != 0) image = gpuImage(imageId, image);
    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
}

void SkiaRenderer::drawPixelsRGBA(const uint8_t* rgba, int srcW, int srcH, int stride,
                                  float x, float y, float w, float h) {
    if (!canvas_ || !rgba || srcW <= 0 || srcH <= 0) return;
    if (stride <= 0) stride = srcW * 4;

    SkImageInfo info = SkImageInfo::Make(srcW, srcH, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    SkBitmap bmp;
    if (!bmp.installPixels(info, const_cast<uint8_t*>(rgba), static_cast<size_t>(stride))) return;
    auto image = bmp.asImage();
    if (!image) return;
    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
}

void SkiaRenderer::drawSvgMarkup(const char* data, size_t len,
                                 float x, float y, float w, float h) {
    if (!canvas_) return;
    bro::svg::renderSvgMarkupToCanvas(canvas_, data, len, x, y, w, h);
}

void SkiaRenderer::setClip(float x, float y, float w, float h) {
    if (canvas_) canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void SkiaRenderer::resetClip() {
    if (!canvas_) return;
    canvas_->restore();
    canvas_->save();
}

void SkiaRenderer::setClipPolygon(std::span<const render::PointF> points) {
    if (!canvas_ || points.empty()) return;
    SkPathBuilder pb;
    pb.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); ++i) {
        pb.lineTo(points[i].x, points[i].y);
    }
    pb.close();
    canvas_->clipPath(pb.detach(), SkClipOp::kIntersect, /*doAntiAlias=*/true);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer() {
    LOG_INFO("Creating SkiaRenderer (Skia raster)");
    return std::make_unique<SkiaRenderer>();
}

} // namespace bro::render
