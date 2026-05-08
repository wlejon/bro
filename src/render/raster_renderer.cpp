#include "render/raster_renderer.h"
#include "render/filter_chain.h"
#include "svg/svg_renderer.h"

#include <include/core/SkBitmap.h>
#include <include/core/SkM44.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSurface.h>
#include <include/codec/SkCodec.h>
#include <include/effects/SkGradient.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkBlurTypes.h>
#include <stb_image_write.h>
#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#elif defined(__APPLE__)
#include <include/ports/SkFontMgr_mac_ct.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif
#include <include/utils/SkParsePath.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace bro::render {

// CSS gradient interpolation: premultiplied sRGB so opaque-color → transparent
// fades through the color's hue (browser default), not through gray. See
// skia_backend.cpp for details.
static const SkGradient::Interpolation kCSSGradInterp = {
    SkGradient::Interpolation::InPremul::kYes,
    SkGradient::Interpolation::ColorSpace::kDestination,
    SkGradient::Interpolation::HueMethod::kShorter,
};

// Helper: build SkGradient::Colors from our ColorStop span
static SkGradient::Colors buildGradColors(std::span<const ColorStop> stops) {
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

SkColor RasterRenderer::toSkColor(Color c) {
    return SkColorSetARGB(c.a, c.r, c.g, c.b);
}

void RasterRenderer::clear(Color c) {
    if (canvas_) canvas_->clear(toSkColor(c));
}

void RasterRenderer::drawRect(float x, float y, float w, float h, Color c) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(c));
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

void RasterRenderer::drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color c) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(c));
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry), paint);
}

void RasterRenderer::fillRect(float x, float y, float w, float h, Color c) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(c));
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

void RasterRenderer::fillRoundRect(float x, float y, float w, float h, float rx, float ry, Color c) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(c));
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry), paint);
}

static SkRRect makeRRectRaster(float x, float y, float w, float h, const Radii& r) {
    SkVector radii[4] = {
        {r.x[0], r.y[0]}, {r.x[1], r.y[1]},
        {r.x[2], r.y[2]}, {r.x[3], r.y[3]}
    };
    SkRRect rr;
    rr.setRectRadii(SkRect::MakeXYWH(x, y, w, h), radii);
    return rr;
}

void RasterRenderer::fillRoundRectRadii(float x, float y, float w, float h,
                                        const Radii& r, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kFill_Style);
    paint.setAntiAlias(true);
    canvas_->drawRRect(makeRRectRaster(x, y, w, h, r), paint);
}

void RasterRenderer::drawRoundRectRadii(float x, float y, float w, float h,
                                        const Radii& r, float strokeWidth, Color color) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(strokeWidth);
    paint.setAntiAlias(true);
    canvas_->drawRRect(makeRRectRaster(x, y, w, h, r), paint);
}

void RasterRenderer::setClipRRect(float x, float y, float w, float h, const Radii& r) {
    if (!canvas_) return;
    canvas_->clipRRect(makeRRectRaster(x, y, w, h, r), true);
}

void RasterRenderer::drawBoxShadowRadii(float x, float y, float w, float h,
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
        canvas_->clipRRect(makeRRectRaster(x, y, w, h, r), true);

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
        pb.addRRect(makeRRectRaster(ix, iy, iw, ih, ir));
        pb.setFillType(SkPathFillType::kEvenOdd);
        SkPath path = pb.detach();

        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(SkColorSetARGB(color.a, color.r, color.g, color.b));
        if (blur > 0)
            paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
        canvas_->drawPath(path, paint);
        canvas_->restore();
        return;
    }
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
    paint.setColor(SkColorSetARGB(color.a, color.r, color.g, color.b));
    if (blur > 0)
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    canvas_->drawRRect(makeRRectRaster(sx, sy, sw, sh, sr), paint);
}

void RasterRenderer::drawText(std::string_view text, float x, float y, FontRef font, Color c) {
    if (!canvas_ || text.empty()) return;
    const FontEntry* fePtr = getOrCreateFont(font);
    if (!fePtr) return;
    SkPaint paint;
    paint.setColor(toSkColor(c));
    const FontEntry& fe = *fePtr;
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

void RasterRenderer::drawTextEx(std::string_view text, float x, float y,
                                FontRef font, Color c,
                                float letterSpacing, float blur) {
    if (!canvas_ || text.empty()) return;
    const FontEntry* fePtr = getOrCreateFont(font);
    if (!fePtr) return;

    SkPaint paint;
    paint.setColor(toSkColor(c));
    if (blur > 0) {
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    }
    const FontEntry& fe = *fePtr;
    auto runs = splitTextForFallback(text, *fe.font, ensureFontMgr(),
                                      fe.style, fallbackCache_);
    if (runs.empty()) return;

    float cursor = x;
    for (size_t r = 0; r < runs.size(); ++r) {
        const auto& run = runs[r];
        const char* data = text.data() + run.start;
        bool isLastRun = (r + 1 == runs.size());
        if (letterSpacing == 0.0f) {
            canvas_->drawSimpleText(data, run.length, SkTextEncoding::kUTF8,
                                    cursor, y, run.font, paint);
            cursor += run.font.measureText(data, run.length, SkTextEncoding::kUTF8);
        } else {
            // Walk UTF-8 codepoints, applying letter-spacing BETWEEN them
            // (n - 1 times) so visible glyph extent matches the layout box
            // and centered text isn't drifted leftward by trailing dead space.
            size_t i = 0;
            while (i < run.length) {
                unsigned char b = static_cast<unsigned char>(data[i]);
                size_t n = 1;
                if      ((b & 0x80) == 0x00) n = 1;
                else if ((b & 0xE0) == 0xC0) n = 2;
                else if ((b & 0xF0) == 0xE0) n = 3;
                else if ((b & 0xF8) == 0xF0) n = 4;
                if (i + n > run.length) n = run.length - i;
                canvas_->drawSimpleText(data + i, n, SkTextEncoding::kUTF8,
                                        cursor, y, run.font, paint);
                cursor += run.font.measureText(data + i, n, SkTextEncoding::kUTF8);
                bool isLastCodepoint = (i + n >= run.length);
                if (!(isLastCodepoint && isLastRun)) cursor += letterSpacing;
                i += n;
            }
        }
    }
}

TextMetrics RasterRenderer::measureText(std::string_view text, FontRef font) {
    const FontEntry* fePtr = getOrCreateFont(font);
    if (!fePtr) return {};
    const FontEntry& fe = *fePtr;
    const SkFont& primary = *fe.font;
    SkFontMetrics fm;
    primary.getMetrics(&fm);
    if (text.empty()) return { 0.0f, 0.0f, -fm.fAscent, fm.fDescent, fm.fLeading };
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
    return { width, maxH, -fm.fAscent, fm.fDescent, fm.fLeading };
}

SkFontMgr* RasterRenderer::ensureFontMgr() {
    if (fontMgr_) return fontMgr_.get();
#ifdef _WIN32
    fontMgr_ = SkFontMgr_New_DirectWrite();
#elif defined(__APPLE__)
    fontMgr_ = SkFontMgr_New_CoreText(nullptr);
#else
    fontMgr_ = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
    return fontMgr_.get();
}

const RasterRenderer::FontEntry* RasterRenderer::getOrCreateFont(FontRef ref) {
    FontKey key{std::string(ref.family), ref.size, ref.weight, ref.italic};
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return &it->second;

    SkFontStyle style(ref.weight, SkFontStyle::kNormal_Width,
                      ref.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
    SkFontMgr* mgrRaw = ensureFontMgr();
    sk_sp<SkFontMgr> mgr(sk_ref_sp(mgrRaw));

    // Map CSS generic family names to real font names (must match SkiaRenderer)
    auto resolveGeneric = [](const std::string& name) -> const char* {
#ifdef _WIN32
        if (name == "sans-serif")  return "Arial";
        if (name == "serif")       return "Times New Roman";
        if (name == "monospace")   return "Consolas";
        if (name == "cursive")     return "Comic Sans MS";
        if (name == "fantasy")     return "Impact";
        if (name == "system-ui")   return "Segoe UI";
#elif defined(__APPLE__)
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

    // CSS font-family is comma-separated — try each name in order.
    sk_sp<SkTypeface> typeface;
    std::istringstream stream{std::string(ref.family)};
    std::string name;
    while (std::getline(stream, name, ',')) {
        while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
        if (name.empty()) continue;
        // Try CSS generic name first
        const char* resolved = resolveGeneric(name);
        if (resolved) {
            typeface = mgr->matchFamilyStyle(resolved, style);
            if (typeface) break;
        }
        typeface = mgr->matchFamilyStyle(name.c_str(), style);
        if (typeface) break;
    }
    if (!typeface) typeface = mgr->matchFamilyStyle(nullptr, SkFontStyle());
    auto sk_font = std::make_unique<SkFont>(typeface, ref.size);
    sk_font->setEdging(SkFont::Edging::kAntiAlias);
    auto [ins, _] = fonts_.emplace(std::move(key),
        FontEntry{ std::move(typeface), std::move(sk_font), style });
    return &ins->second;
}

void RasterRenderer::drawLine(float x1, float y1, float x2, float y2, Color c, float thickness) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setColor(toSkColor(c));
    paint.setStrokeWidth(thickness);
    paint.setStyle(SkPaint::kStroke_Style);
    canvas_->drawLine(x1, y1, x2, y2, paint);
}

void RasterRenderer::drawImage(const void* data, size_t len, float x, float y, float w, float h) {
    if (!canvas_) return;
    auto sk_data = SkData::MakeWithoutCopy(data, len);
    auto codec = SkCodec::MakeFromData(sk_data);
    if (!codec) return;
    auto [image, result] = codec->getImage();
    if (!image) return;
    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
}

void RasterRenderer::drawPixelsRGBA(const uint8_t* rgba, int srcW, int srcH, int stride,
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

void RasterRenderer::drawSvgMarkup(const char* data, size_t len,
                                   float x, float y, float w, float h) {
    if (!canvas_) return;
    bro::svg::renderSvgMarkupToCanvas(canvas_, data, len, x, y, w, h);
}

void RasterRenderer::drawCircle(float cx, float cy, float r,
                                Color fill, Color stroke, float strokeWidth) {
    if (!canvas_) return;
    if (fill.a > 0) {
        SkPaint paint; paint.setColor(toSkColor(fill));
        paint.setStyle(SkPaint::kFill_Style); paint.setAntiAlias(true);
        canvas_->drawCircle(cx, cy, r, paint);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint paint; paint.setColor(toSkColor(stroke));
        paint.setStyle(SkPaint::kStroke_Style); paint.setStrokeWidth(strokeWidth); paint.setAntiAlias(true);
        canvas_->drawCircle(cx, cy, r, paint);
    }
}

void RasterRenderer::drawEllipse(float cx, float cy, float rx, float ry,
                                  Color fill, Color stroke, float strokeWidth) {
    if (!canvas_) return;
    SkRect oval = SkRect::MakeXYWH(cx - rx, cy - ry, rx * 2, ry * 2);
    if (fill.a > 0) {
        SkPaint paint; paint.setColor(toSkColor(fill));
        paint.setStyle(SkPaint::kFill_Style); paint.setAntiAlias(true);
        canvas_->drawOval(oval, paint);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint paint; paint.setColor(toSkColor(stroke));
        paint.setStyle(SkPaint::kStroke_Style); paint.setStrokeWidth(strokeWidth); paint.setAntiAlias(true);
        canvas_->drawOval(oval, paint);
    }
}

void RasterRenderer::drawPath(std::string_view svgPathData,
                               Color fill, Color stroke, float strokeWidth) {
    if (!canvas_ || svgPathData.empty()) return;
    auto pathOpt = SkParsePath::FromSVGString(std::string(svgPathData).c_str());
    if (!pathOpt) return;
    const SkPath& path = *pathOpt;
    if (fill.a > 0) {
        SkPaint paint; paint.setColor(toSkColor(fill));
        paint.setStyle(SkPaint::kFill_Style); paint.setAntiAlias(true);
        canvas_->drawPath(path, paint);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint paint; paint.setColor(toSkColor(stroke));
        paint.setStyle(SkPaint::kStroke_Style); paint.setStrokeWidth(strokeWidth); paint.setAntiAlias(true);
        canvas_->drawPath(path, paint);
    }
}

void RasterRenderer::drawPolygon(std::span<const PointF> points,
                                  Color fill, Color stroke, float strokeWidth) {
    if (!canvas_ || points.size() < 2) return;
    SkPathBuilder builder;
    builder.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); i++) builder.lineTo(points[i].x, points[i].y);
    builder.close();
    SkPath path = builder.detach();
    if (fill.a > 0) {
        SkPaint paint; paint.setColor(toSkColor(fill));
        paint.setStyle(SkPaint::kFill_Style); paint.setAntiAlias(true);
        canvas_->drawPath(path, paint);
    }
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint paint; paint.setColor(toSkColor(stroke));
        paint.setStyle(SkPaint::kStroke_Style); paint.setStrokeWidth(strokeWidth); paint.setAntiAlias(true);
        canvas_->drawPath(path, paint);
    }
}

void RasterRenderer::drawPolyline(std::span<const PointF> points,
                                   Color stroke, float strokeWidth) {
    if (!canvas_ || points.size() < 2) return;
    SkPathBuilder builder;
    builder.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); i++) builder.lineTo(points[i].x, points[i].y);
    SkPath path = builder.detach();
    if (stroke.a > 0 && strokeWidth > 0) {
        SkPaint paint; paint.setColor(toSkColor(stroke));
        paint.setStyle(SkPaint::kStroke_Style); paint.setStrokeWidth(strokeWidth); paint.setAntiAlias(true);
        canvas_->drawPath(path, paint);
    }
}

void RasterRenderer::drawBoxShadow(float x, float y, float w, float h,
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
        paint.setColor(SkColorSetARGB(color.a, color.r, color.g, color.b));
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
    paint.setColor(SkColorSetARGB(color.a, color.r, color.g, color.b));
    if (blur > 0)
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    if (rx > 0 || ry > 0)
        canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(sx2, sy2, sw, sh), rx, ry), paint);
    else
        canvas_->drawRect(SkRect::MakeXYWH(sx2, sy2, sw, sh), paint);
}

void RasterRenderer::save() { if (canvas_) canvas_->save(); }
void RasterRenderer::restore() { if (canvas_) canvas_->restore(); }
void RasterRenderer::saveLayerAlpha(uint8_t alpha) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setAlphaf(alpha / 255.0f);
    canvas_->saveLayer(nullptr, &paint);
}
void RasterRenderer::translate(float dx, float dy) { if (canvas_) canvas_->translate(dx, dy); }
void RasterRenderer::scale(float sx, float sy) { if (canvas_) canvas_->scale(sx, sy); }
void RasterRenderer::rotate(float degrees) { if (canvas_) canvas_->rotate(degrees); }
void RasterRenderer::saveLayerWithFilter(std::span<const CssFilterParams> filters,
                                         float x, float y, float w, float h) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setImageFilter(BuildSkImageFilterChain(filters));
    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    canvas_->saveLayer(SkCanvas::SaveLayerRec(&bounds, &paint));
}
void RasterRenderer::concat(float a, float b, float c, float d, float e, float f) {
    if (!canvas_) return;
    SkMatrix m = SkMatrix::MakeAll(a, c, e, b, d, f, 0, 0, 1);
    canvas_->concat(m);
}

void RasterRenderer::concat4x4(const float m[16]) {
    if (!canvas_) return;
    // Input is column-major (m[0..3] = col 0, ...). SkM44 takes 16 scalars in
    // ROW-major order via its 16-arg constructor.
    SkM44 mat(m[0], m[4], m[ 8], m[12],
              m[1], m[5], m[ 9], m[13],
              m[2], m[6], m[10], m[14],
              m[3], m[7], m[11], m[15]);
    canvas_->concat(mat);
}

void RasterRenderer::setClip(float x, float y, float w, float h) {
    if (canvas_) canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void RasterRenderer::resetClip() {
    if (!canvas_) return;
    canvas_->restore();
    canvas_->save();
}

void RasterRenderer::setClipPolygon(std::span<const render::PointF> points) {
    if (!canvas_ || points.empty()) return;
    SkPathBuilder pb;
    pb.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); ++i) {
        pb.lineTo(points[i].x, points[i].y);
    }
    pb.close();
    canvas_->clipPath(pb.detach(), SkClipOp::kIntersect, /*doAntiAlias=*/true);
}

void RasterRenderer::fillLinearGradient(float x, float y, float w, float h,
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

void RasterRenderer::fillRadialGradient(float x, float y, float w, float h,
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

void RasterRenderer::fillConicGradient(float x, float y, float w, float h,
                                        float cx, float cy, float angleDeg,
                                        std::span<const ColorStop> stops) {
    if (!canvas_ || stops.empty()) return;
    float startAngle = angleDeg - 90.0f;
    auto shader = SkShaders::SweepGradient({cx, cy}, startAngle, startAngle + 360.0f,
        SkGradient(buildGradColors(stops), kCSSGradInterp));
    SkPaint paint;
    paint.setShader(shader);
    paint.setStyle(SkPaint::kFill_Style);
    canvas_->save();
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
    canvas_->restore();
}

void RasterRenderer::beginFrame(int width, int height) {
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    }
    canvas_ = surface_->getCanvas();
    canvas_->save();
}

void RasterRenderer::endFrame() {
    if (canvas_) canvas_->restore();
    canvas_ = nullptr;
}

bool RasterRenderer::saveScreenshot(const std::string& path) {
    if (!surface_) return false;
    sk_sp<SkImage> image = surface_->makeImageSnapshot();
    if (!image) return false;
    SkPixmap pixmap;
    if (!image->peekPixels(&pixmap)) return false;

    int w = pixmap.width(), h = pixmap.height();

    // Convert from N32 (BGRA premultiplied on Windows) to RGBA for PNG
    std::vector<uint8_t> rgba(w * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(pixmap.addr32(0, y));
        uint8_t* dst = rgba.data() + y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2]; // R <- B
            dst[x * 4 + 1] = src[x * 4 + 1]; // G
            dst[x * 4 + 2] = src[x * 4 + 0]; // B <- R
            dst[x * 4 + 3] = src[x * 4 + 3]; // A
        }
    }

    return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

std::vector<uint8_t> RasterRenderer::capturePixels() {
    if (!surface_) return {};
    sk_sp<SkImage> image = surface_->makeImageSnapshot();
    if (!image) return {};
    SkPixmap pixmap;
    if (!image->peekPixels(&pixmap)) return {};

    int w = pixmap.width(), h = pixmap.height();
    std::vector<uint8_t> rgba(w * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(pixmap.addr32(0, y));
        uint8_t* dst = rgba.data() + y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2]; // R <- B
            dst[x * 4 + 1] = src[x * 4 + 1]; // G
            dst[x * 4 + 2] = src[x * 4 + 0]; // B <- R
            dst[x * 4 + 3] = src[x * 4 + 3]; // A
        }
    }
    return rgba;
}

} // namespace bro::render
