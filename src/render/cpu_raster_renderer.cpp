// CPU-raster Skia renderer — uploads pixels to a GL texture for compositing.
// Used by the Engine for system panel rendering (separate from the GPU raster thread).

#include "render/cpu_raster_renderer.h"
#include "render/gl_context.h"

#include <include/core/SkImageFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontMetrics.h>
#include <include/codec/SkCodec.h>
#include <include/effects/SkGradient.h>
#include <include/utils/SkParsePath.h>
#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif

#include <algorithm>
#include <cmath>
#include <sstream>

static SkGradient::Colors buildGradColors(std::span<const bro::render::ColorStop> stops) {
    thread_local std::vector<SkColor4f> colors;
    thread_local std::vector<float> pos;
    colors.resize(stops.size());
    pos.resize(stops.size());
    for (size_t i = 0; i < stops.size(); i++) {
        colors[i] = SkColor4f{stops[i].color.r / 255.0f, stops[i].color.g / 255.0f,
                              stops[i].color.b / 255.0f, stops[i].color.a / 255.0f};
        pos[i] = stops[i].offset;
    }
    return SkGradient::Colors(SkSpan(colors), SkSpan(pos), SkTileMode::kClamp);
}

namespace bro::render {

CPURasterRenderer::CPURasterRenderer(GLContext* gl) : gl_(gl) {}

CPURasterRenderer::~CPURasterRenderer() {
    fonts_.clear();
    surface_.reset();
    if (texture_ && gl_) gl_->deleteTexture(texture_);
}

SkColor CPURasterRenderer::toSkColor(Color c) const {
    return SkColorSetARGB(c.a, c.r, c.g, c.b);
}

void CPURasterRenderer::clear(Color color) {
    if (canvas_) canvas_->clear(toSkColor(color));
}

void CPURasterRenderer::drawRect(float x, float y, float w, float h, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

void CPURasterRenderer::drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry), paint);
}

void CPURasterRenderer::fillRect(float x, float y, float w, float h, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

void CPURasterRenderer::drawText(std::string_view text, float x, float y, uint64_t font_handle, Color color) {
    if (!canvas_ || text.empty()) return;
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    const FontEntry& fe = it->second;
    auto runs = splitTextForFallback(text, *fe.font, ensureFontMgr(),
                                      fe.style, fallbackCache_);
    if (runs.empty()) return;
    float cursor = x;
    for (const auto& run : runs) {
        const char* data = text.data() + run.start;
        canvas_->drawSimpleText(data, run.length, SkTextEncoding::kUTF8,
                                cursor, y, run.font, paint);
        cursor += run.font.measureText(data, run.length, SkTextEncoding::kUTF8);
    }
}

TextMetrics CPURasterRenderer::measureText(std::string_view text, uint64_t font_handle) {
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) return {};
    const FontEntry& fe = it->second;
    const SkFont& primary = *fe.font;
    SkFontMetrics fm;
    primary.getMetrics(&fm);
    if (text.empty()) return { 0.0f, 0.0f, -fm.fAscent, fm.fDescent };
    auto runs = splitTextForFallback(text, primary, ensureFontMgr(),
                                      fe.style, fallbackCache_);
    float width = 0.0f;
    float maxH = 0.0f;
    for (const auto& run : runs) {
        const char* data = text.data() + run.start;
        SkRect b;
        width += run.font.measureText(data, run.length, SkTextEncoding::kUTF8, &b);
        if (b.height() > maxH) maxH = b.height();
    }
    return { width, maxH, -fm.fAscent, fm.fDescent };
}

SkFontMgr* CPURasterRenderer::ensureFontMgr() {
    if (fontMgr_) return fontMgr_.get();
#ifdef _WIN32
    fontMgr_ = SkFontMgr_New_DirectWrite();
#else
    fontMgr_ = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
    return fontMgr_.get();
}

uint64_t CPURasterRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    SkFontStyle style(weight,
                      SkFontStyle::kNormal_Width,
                      italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
    SkFontMgr* font_mgrRaw = ensureFontMgr();
    sk_sp<SkFontMgr> font_mgr(sk_ref_sp(font_mgrRaw));

    auto resolveGeneric = [](const std::string& name) -> const char* {
#ifdef _WIN32
        if (name == "sans-serif")  return "Arial";
        if (name == "serif")       return "Times New Roman";
        if (name == "monospace")   return "Consolas";
        if (name == "cursive")     return "Comic Sans MS";
        if (name == "fantasy")     return "Impact";
        if (name == "system-ui")   return "Segoe UI";
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
    std::string families(family);
    std::istringstream stream(families);
    std::string name;
    while (std::getline(stream, name, ',')) {
        while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
        if (name.empty()) continue;
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

    auto sk_font = std::make_unique<SkFont>(typeface, size);
    sk_font->setEdging(SkFont::Edging::kAntiAlias);

    uint64_t handle = nextFontHandle_++;
    fonts_[handle] = FontEntry{std::move(typeface), std::move(sk_font), style};
    return handle;
}

void CPURasterRenderer::deleteFont(uint64_t font_handle) {
    fonts_.erase(font_handle);
}

void CPURasterRenderer::drawLine(float x1, float y1, float x2, float y2, Color color, float thickness) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStrokeWidth(thickness);
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawLine(x1, y1, x2, y2, paint);
}

void CPURasterRenderer::drawImage(const void* data, size_t len, float x, float y, float w, float h) {
    if (!canvas_) return;
    sk_sp<SkData> sk_data = SkData::MakeWithoutCopy(data, len);
    auto codec = SkCodec::MakeFromData(sk_data);
    if (!codec) return;
    auto [image, result] = codec->getImage();
    if (!image) return;
    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
}

void CPURasterRenderer::fillRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry), paint);
}

void CPURasterRenderer::drawCircle(float cx, float cy, float r,
                                   Color fill, Color stroke, float strokeWidth) {
    if (!canvas_) return;
    if (fill.a > 0) {
        SkPaint p; p.setColor(toSkColor(fill)); p.setStyle(SkPaint::kFill_Style); p.setAntiAlias(true);
        canvas_->drawCircle(cx, cy, r, p);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint p; p.setColor(toSkColor(stroke)); p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(strokeWidth); p.setAntiAlias(true);
        canvas_->drawCircle(cx, cy, r, p);
    }
}

void CPURasterRenderer::drawEllipse(float cx, float cy, float rx, float ry,
                                    Color fill, Color stroke, float strokeWidth) {
    if (!canvas_) return;
    SkRect oval = SkRect::MakeXYWH(cx - rx, cy - ry, rx * 2, ry * 2);
    if (fill.a > 0) {
        SkPaint p; p.setColor(toSkColor(fill)); p.setStyle(SkPaint::kFill_Style); p.setAntiAlias(true);
        canvas_->drawOval(oval, p);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint p; p.setColor(toSkColor(stroke)); p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(strokeWidth); p.setAntiAlias(true);
        canvas_->drawOval(oval, p);
    }
}

void CPURasterRenderer::drawPath(std::string_view svgPathData,
                                 Color fill, Color stroke, float strokeWidth) {
    if (!canvas_ || svgPathData.empty()) return;
    auto pathOpt = SkParsePath::FromSVGString(std::string(svgPathData).c_str());
    if (!pathOpt) return;
    const SkPath& path = *pathOpt;
    if (fill.a > 0) {
        SkPaint p; p.setColor(toSkColor(fill)); p.setStyle(SkPaint::kFill_Style); p.setAntiAlias(true);
        canvas_->drawPath(path, p);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint p; p.setColor(toSkColor(stroke)); p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(strokeWidth); p.setAntiAlias(true);
        canvas_->drawPath(path, p);
    }
}

void CPURasterRenderer::drawPolygon(std::span<const PointF> points,
                                    Color fill, Color stroke, float strokeWidth) {
    if (!canvas_ || points.size() < 2) return;
    SkPathBuilder builder;
    builder.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); i++) builder.lineTo(points[i].x, points[i].y);
    builder.close();
    SkPath path = builder.detach();
    if (fill.a > 0) {
        SkPaint p; p.setColor(toSkColor(fill)); p.setStyle(SkPaint::kFill_Style); p.setAntiAlias(true);
        canvas_->drawPath(path, p);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint p; p.setColor(toSkColor(stroke)); p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(strokeWidth); p.setAntiAlias(true);
        canvas_->drawPath(path, p);
    }
}

void CPURasterRenderer::drawPolyline(std::span<const PointF> points,
                                     Color stroke, float strokeWidth) {
    if (!canvas_ || points.size() < 2) return;
    SkPathBuilder builder;
    builder.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); i++) builder.lineTo(points[i].x, points[i].y);
    SkPath path = builder.detach();
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint p; p.setColor(toSkColor(stroke)); p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(strokeWidth); p.setAntiAlias(true);
        canvas_->drawPath(path, p);
    }
}

void CPURasterRenderer::drawBoxShadow(float x, float y, float w, float h,
                                      float rx, float ry,
                                      float offsetX, float offsetY,
                                      float blur, float spread,
                                      Color color, bool inset) {
    if (!canvas_) return;

    if (inset) {
        canvas_->save();
        SkRect clipRect = SkRect::MakeXYWH(x, y, w, h);
        if (rx > 0 || ry > 0)
            canvas_->clipRRect(SkRRect::MakeRectXY(clipRect, rx, ry));
        else
            canvas_->clipRect(clipRect);
        float ix = x + offsetX + spread;
        float iy = y + offsetY + spread;
        float iw = w - spread * 2;
        float ih = h - spread * 2;
        float ir = std::max(0.0f, rx - spread);
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

    float sx2 = x + offsetX - spread;
    float sy2 = y + offsetY - spread;
    float sw = w + spread * 2;
    float sh = h + spread * 2;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(toSkColor(color));
    if (blur > 0)
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    if (rx > 0 || ry > 0)
        canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(sx2, sy2, sw, sh), rx, ry), paint);
    else
        canvas_->drawRect(SkRect::MakeXYWH(sx2, sy2, sw, sh), paint);
}

void CPURasterRenderer::save() { if (canvas_) canvas_->save(); }
void CPURasterRenderer::restore() { if (canvas_) canvas_->restore(); }
void CPURasterRenderer::saveLayerAlpha(uint8_t alpha) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setAlphaf(alpha / 255.0f);
    canvas_->saveLayer(nullptr, &paint);
}
void CPURasterRenderer::translate(float dx, float dy) { if (canvas_) canvas_->translate(dx, dy); }
void CPURasterRenderer::scale(float sx, float sy) { if (canvas_) canvas_->scale(sx, sy); }
void CPURasterRenderer::rotate(float degrees) { if (canvas_) canvas_->rotate(degrees); }
void CPURasterRenderer::saveLayerWithFilter(SkImageFilter* filter, float x, float y, float w, float h) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setImageFilter(sk_ref_sp(filter));
    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    canvas_->saveLayer(SkCanvas::SaveLayerRec(&bounds, &paint));
}
void CPURasterRenderer::concat(float a, float b, float c, float d, float e, float f) {
    if (!canvas_) return;
    SkMatrix m = SkMatrix::MakeAll(a, c, e, b, d, f, 0, 0, 1);
    canvas_->concat(m);
}

void CPURasterRenderer::setClip(float x, float y, float w, float h) {
    if (canvas_) canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void CPURasterRenderer::resetClip() {
    if (!canvas_) return;
    canvas_->restore();
    canvas_->save();
}

void CPURasterRenderer::fillLinearGradient(float x, float y, float w, float h,
                                           float startX, float startY, float endX, float endY,
                                           std::span<const ColorStop> stops) {
    if (!canvas_ || stops.empty()) return;
    SkPoint pts[2] = { {startX, startY}, {endX, endY} };
    auto shader = SkShaders::LinearGradient(pts, SkGradient(buildGradColors(stops), {}));
    SkPaint paint;
    paint.setShader(shader);
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->save();
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    canvas_->restore();
}

void CPURasterRenderer::fillRadialGradient(float x, float y, float w, float h,
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
        canvas_->translate(cx, cy);
        canvas_->scale(1.0f, ry / rx);
        canvas_->translate(-cx, -cy);
        paint.setShader(SkShaders::RadialGradient({cx, cy}, rx,
            SkGradient(buildGradColors(stops), {})));
    } else {
        paint.setShader(SkShaders::RadialGradient({cx, cy}, r,
            SkGradient(buildGradColors(stops), {})));
    }
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    canvas_->restore();
}

void CPURasterRenderer::fillConicGradient(float x, float y, float w, float h,
                                          float cx, float cy, float angleDeg,
                                          std::span<const ColorStop> stops) {
    if (!canvas_ || stops.empty()) return;
    float startAngle = angleDeg - 90.0f;
    auto shader = SkShaders::SweepGradient({cx, cy}, startAngle, startAngle + 360.0f,
        SkGradient(buildGradColors(stops), {}));
    SkPaint paint;
    paint.setShader(shader);
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->save();
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    canvas_->restore();
}

void CPURasterRenderer::beginFrame(int width, int height) {
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        if (gl_) {
            if (texture_) gl_->deleteTexture(texture_);
            texture_ = gl_->createTexture2D(width, height, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);
        }
        texWidth_ = width;
        texHeight_ = height;
    }
    canvas_ = surface_->getCanvas();
    canvas_->restoreToCount(0);
    canvas_->clear(SK_ColorTRANSPARENT);
    canvas_->save();
}

void CPURasterRenderer::endFrame() {
    if (canvas_) canvas_->restore();
    canvas_ = nullptr;
}

void CPURasterRenderer::uploadToGPU() {
    if (!gl_ || !surface_ || !texture_) return;
    SkPixmap pixmap;
    if (!surface_->peekPixels(&pixmap)) return;

    gl_->uploadTexture2D(texture_, pixmap.addr(),
                         static_cast<uint32_t>(pixmap.width()),
                         static_cast<uint32_t>(pixmap.height()),
                         GL_BGRA, GL_UNSIGNED_BYTE);
}

} // namespace bro::render
