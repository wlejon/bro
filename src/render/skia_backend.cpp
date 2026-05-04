#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "util/log.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <cstring>
#include <sstream>
#include <cmath>

#include <include/core/SkBitmap.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkColorSpace.h>
#include <include/codec/SkCodec.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/utils/SkParsePath.h>
#include <include/effects/SkGradient.h>
#include <include/core/SkImageFilter.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkBlurTypes.h>
#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#elif defined(__APPLE__)
#include <include/ports/SkFontMgr_mac_ct.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>

namespace bro::render {

// ===========================================================================
// SkiaRenderer — Skia raster rendering + OpenGL display
// ===========================================================================

sk_sp<GrDirectContext> SkiaRenderer::createGrContext() {
    auto glInterface = GrGLMakeNativeInterface();
    if (!glInterface) return nullptr;
    return GrDirectContexts::MakeGL(glInterface);
}

SkiaRenderer::SkiaRenderer(GLContext& gl) : gl_(&gl) {
    // Try to create Skia GPU (Ganesh GL) context
    grContext_ = createGrContext();
    if (grContext_) {
        gpuMode_ = true;
        LOG_INFO("SkiaRenderer created (GPU-accelerated Ganesh GL backend)");
    } else {
        LOG_INFO("SkiaRenderer created (CPU raster fallback)");
    }
}

SkiaRenderer::~SkiaRenderer() {
    for (auto& [k, e] : textTexCache_) {
        gl_->deleteTexture(e.tex);
    }
    textTexCache_.clear();
    fonts_.clear();
    surface_.reset();
    if (uiTexture_) gl_->deleteTexture(uiTexture_);
}

SkColor SkiaRenderer::toSkColor(Color c) const {
    return SkColorSetARGB(c.a, c.r, c.g, c.b);
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

void SkiaRenderer::drawText(std::string_view text, float x, float y, uint64_t font_handle, Color color) {
    if (!canvas_ || text.empty()) return;
    auto fontIt = fonts_.find(font_handle);
    if (fontIt == fonts_.end()) return;

    SkPaint paint;
    paint.setColor(toSkColor(color));

    // Per-glyph fallback: the primary font may lack coverage for some
    // codepoints (e.g. &#x2B1A; in plain Arial). Split the string into runs
    // where each run is fully covered by one typeface, drawing each run
    // with its own advance.
    const FontEntry& fe = fontIt->second;
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

void SkiaRenderer::drawTextEx(std::string_view text, float x, float y,
                              uint64_t font_handle, Color color,
                              float letterSpacing, float blur) {
    if (!canvas_ || text.empty()) return;
    auto fontIt = fonts_.find(font_handle);
    if (fontIt == fonts_.end()) return;

    SkPaint paint;
    paint.setColor(toSkColor(color));
    if (blur > 0) {
        // Skia sigma matches CSS blur radius / 2 (the same convention used
        // by drawBoxShadow). MakeBlur draws a Gaussian falloff over the
        // glyph mask, producing the text-shadow halo.
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    }

    const FontEntry& fe = fontIt->second;
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
            // Walk UTF-8 codepoints and add letter-spacing BETWEEN codepoints
            // (n - 1 times). The trailing slot is omitted to match the layout
            // box width and avoid drifting centered text leftward.
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

TextMetrics SkiaRenderer::measureText(std::string_view text, uint64_t font_handle) {
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) return {};
    const FontEntry& fe = it->second;
    const SkFont& primary = *fe.font;
    SkFontMetrics fm;
    primary.getMetrics(&fm);
    if (text.empty()) {
        return { 0.0f, 0.0f, -fm.fAscent, fm.fDescent };
    }
    auto runs = splitTextForFallback(text, primary, ensureFontMgr(),
                                      fe.style, fallbackCache_);
    float width = 0.0f;
    float maxH = 0.0f;
    for (const auto& run : runs) {
        const char* data = text.data() + run.start;
        SkRect bounds;
        width += run.font.measureText(data, run.length, SkTextEncoding::kUTF8, &bounds);
        if (bounds.height() > maxH) maxH = bounds.height();
    }
    return { width, maxH, -fm.fAscent, fm.fDescent };
}

SkFontMgr* SkiaRenderer::ensureFontMgr() {
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

uint64_t SkiaRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    SkFontStyle style(weight,
                      SkFontStyle::kNormal_Width,
                      italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);

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

    auto sk_font = std::make_unique<SkFont>(typeface, size);
    sk_font->setEdging(SkFont::Edging::kAntiAlias);

    uint64_t handle = next_font_handle_++;
    fonts_[handle] = FontEntry{std::move(typeface), std::move(sk_font), style};
    return handle;
}

void SkiaRenderer::deleteFont(uint64_t font_handle) {
    fonts_.erase(font_handle);
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
        paint.setColor(SkColorSetARGB(color.a, color.r, color.g, color.b));
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
    paint.setColor(SkColorSetARGB(color.a, color.r, color.g, color.b));
    if (blur > 0)
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    canvas_->drawRRect(makeRRect(sx, sy, sw, sh, sr), paint);
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
        paint.setColor(SkColorSetARGB(color.a, color.r, color.g, color.b));
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
    paint.setColor(SkColorSetARGB(color.a, color.r, color.g, color.b));
    if (blur > 0) {
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2.0f));
    }

    if (rx > 0 || ry > 0)
        canvas_->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(sx, sy, sw, sh), rx, ry), paint);
    else
        canvas_->drawRect(SkRect::MakeXYWH(sx, sy, sw, sh), paint);
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
        // Try with platform font manager
#ifdef _WIN32
        auto mgr = SkFontMgr_New_DirectWrite();
#elif defined(__APPLE__)
        auto mgr = SkFontMgr_New_CoreText(nullptr);
#else
        auto mgr = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
        typeface = mgr->makeFromData(skData);
    }
    if (!typeface) return false;
    customFonts_.push_back({family, weight, italic, typeface});
    return true;
}

void SkiaRenderer::saveLayerWithFilter(SkImageFilter* filter, float x, float y, float w, float h) {
    if (!canvas_) return;
    SkPaint paint;
    paint.setImageFilter(sk_ref_sp(filter));
    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    canvas_->saveLayer(SkCanvas::SaveLayerRec(&bounds, &paint));
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

void SkiaRenderer::drawImage(const void* data, size_t len, float x, float y, float w, float h) {
    if (!canvas_) return;

    // Try Skia's built-in codecs first (fast path when available).
    sk_sp<SkData> sk_data = SkData::MakeWithoutCopy(data, len);
    auto codec = SkCodec::MakeFromData(sk_data);
    if (codec) {
        auto [image, result] = codec->getImage();
        if (image) {
            canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
            return;
        }
    }

    // Fallback: decode via stb_image. Our Skia build may not link PNG/JPEG codecs.
    int iw = 0, ih = 0, comp = 0;
    unsigned char* pixels = stbi_load_from_memory(
        static_cast<const stbi_uc*>(data), static_cast<int>(len), &iw, &ih, &comp, 4);
    if (!pixels) { LOG_WARN("drawImage: decode failed (len=%zu)", len); return; }

    SkImageInfo info = SkImageInfo::Make(iw, ih, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    SkBitmap bmp;
    if (bmp.installPixels(info, pixels, iw * 4,
            [](void* px, void*) { stbi_image_free(px); }, nullptr)) {
        bmp.setImmutable();
        auto image = bmp.asImage();
        if (image) {
            canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
            return;
        }
    }
    stbi_image_free(pixels);
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
        colors[i] = SkColor4f{stops[i].color.r / 255.0f, stops[i].color.g / 255.0f,
                              stops[i].color.b / 255.0f, stops[i].color.a / 255.0f};
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
    // Skia sweep: 0° = 3 o'clock. CSS conic: 0° = 12 o'clock.
    // CSS angle is clockwise from top, so offset by (angleDeg - 90).
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

void SkiaRenderer::beginFrame(int width, int height) {
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_.reset();

        if (uiTexture_) gl_->deleteTexture(uiTexture_);
        uiTexture_ = gl_->createTexture2D(width, height, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);
        textureWidth_ = width;
        textureHeight_ = height;

        if (gpuMode_ && grContext_) {
            // Create FBO wrapping our texture for Skia GPU rendering
            if (gpuFBO_) glDeleteFramebuffers(1, &gpuFBO_);
            glGenFramebuffers(1, &gpuFBO_);
            glBindFramebuffer(GL_FRAMEBUFFER, gpuFBO_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, uiTexture_, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // Wrap the FBO as a Skia GPU render target
            GrGLFramebufferInfo fbInfo;
            fbInfo.fFBOID = gpuFBO_;
            fbInfo.fFormat = GL_RGBA8;
            fbInfo.fProtected = skgpu::Protected::kNo;
            auto backendRT = GrBackendRenderTargets::MakeGL(
                width, height, 0, 0, fbInfo);
            surface_ = SkSurfaces::WrapBackendRenderTarget(
                grContext_.get(), backendRT,
                kTopLeft_GrSurfaceOrigin,
                kRGBA_8888_SkColorType,
                SkColorSpace::MakeSRGB(), nullptr);
        }

        if (!surface_) {
            // Fallback to CPU raster
            gpuMode_ = false;
            surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        }
    }

    if (gpuMode_ && grContext_) {
        // Reset Skia's GL state tracking (we share the context with Three.js)
        grContext_->resetContext();
    }

    canvas_ = surface_->getCanvas();
    canvas_->clear(SK_ColorTRANSPARENT);
    canvas_->save();
}

void SkiaRenderer::endFrame() {
    if (canvas_) canvas_->restore();
    canvas_ = nullptr;

    if (gpuMode_ && grContext_) {
        // Flush all Skia GPU commands across all surfaces (including
        // HTML layer pool surfaces that were drawn to via switchSurface).
        grContext_->flushAndSubmit();
        pixelsPending_ = false;
    } else {
        pixelsPending_ = (surface_ && uiTexture_);
    }
}

void SkiaRenderer::uploadToGPU() {
    // GPU mode: already rendered to texture, nothing to upload
    if (!pixelsPending_ || !surface_ || !uiTexture_) return;
    pixelsPending_ = false;

    SkPixmap pixmap;
    if (!surface_->peekPixels(&pixmap)) return;

    gl_->uploadTexture2D(uiTexture_, pixmap.addr(),
                         static_cast<uint32_t>(pixmap.width()),
                         static_cast<uint32_t>(pixmap.height()),
                         GL_BGRA, GL_UNSIGNED_BYTE);
}

sk_sp<SkSurface> SkiaRenderer::switchSurface(sk_sp<SkSurface> newSurface) {
    // Restore the save() from beginFrame on the current surface
    if (canvas_) canvas_->restore();

    auto prev = surface_;
    surface_ = std::move(newSurface);
    canvas_ = surface_ ? surface_->getCanvas() : nullptr;

    if (canvas_) {
        canvas_->clear(SK_ColorTRANSPARENT);
        canvas_->save();
    }

    return prev;
}

GLuint SkiaRenderer::uploadSurfaceToTexture(SkSurface* surface, GLuint existingTex) {
    if (!surface || !gl_) return 0;

    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) return 0;

    int w = static_cast<int>(pixmap.width());
    int h = static_cast<int>(pixmap.height());

    GLuint tex = existingTex;
    if (!tex) {
        tex = gl_->createTexture2D(w, h, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);
    }
    gl_->uploadTexture2D(tex, pixmap.addr(), w, h, GL_BGRA, GL_UNSIGNED_BYTE);
    return tex;
}

SkiaRenderer::GPUSurface SkiaRenderer::createGPUSurface(int width, int height) {
    GPUSurface result;
    if (!gpuMode_ || !grContext_ || !gl_) return result;

    result.texture = gl_->createTexture2D(width, height, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);

    glGenFramebuffers(1, &result.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, result.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, result.texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GrGLFramebufferInfo fbInfo;
    fbInfo.fFBOID = result.fbo;
    fbInfo.fFormat = GL_RGBA8;
    fbInfo.fProtected = skgpu::Protected::kNo;
    auto backendRT = GrBackendRenderTargets::MakeGL(width, height, 0, 0, fbInfo);
    result.surface = SkSurfaces::WrapBackendRenderTarget(
        grContext_.get(), backendRT,
        kTopLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType,
        SkColorSpace::MakeSRGB(), nullptr);

    return result;
}

void SkiaRenderer::rewrapGPUSurface(GPUSurface& surf, int width, int height) {
    if (!gpuMode_ || !grContext_ || !surf.fbo) return;
    surf.surface.reset();
    GrGLFramebufferInfo fbInfo;
    fbInfo.fFBOID = surf.fbo;
    fbInfo.fFormat = GL_RGBA8;
    fbInfo.fProtected = skgpu::Protected::kNo;
    auto backendRT = GrBackendRenderTargets::MakeGL(width, height, 0, 0, fbInfo);
    surf.surface = SkSurfaces::WrapBackendRenderTarget(
        grContext_.get(), backendRT,
        kTopLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType,
        SkColorSpace::MakeSRGB(), nullptr);
}

void SkiaRenderer::destroyGPUSurface(GPUSurface& surf) {
    surf.surface.reset();
    if (surf.fbo) { glDeleteFramebuffers(1, &surf.fbo); surf.fbo = 0; }
    if (surf.texture && gl_) { gl_->deleteTexture(surf.texture); surf.texture = 0; }
}

GLuint SkiaRenderer::renderTextToTexture(std::string_view text,
                                          uint64_t font_handle,
                                          Color color,
                                          int& outW, int& outH) {
    if (text.empty()) return 0;

    // Cache key
    char key[256];
    std::snprintf(key, sizeof(key), "%.*s|%llu|%u%u%u%u",
                  (int)text.size(), text.data(), (unsigned long long)font_handle,
                  color.r, color.g, color.b, color.a);
    std::string cacheKey(key);

    auto it = textTexCache_.find(cacheKey);
    if (it != textTexCache_.end()) {
        outW = it->second.w;
        outH = it->second.h;
        return it->second.tex;
    }

    // Measure
    auto fit = fonts_.find(font_handle);
    if (fit == fonts_.end()) return 0;
    const FontEntry& fe = fit->second;
    const SkFont& font = *fe.font;

    // Split into fallback runs, then measure the union bounds so glyphs
    // from fallback typefaces (which may have different ascent/descent)
    // are not clipped.
    auto runs = splitTextForFallback(text, font, ensureFontMgr(),
                                      fe.style, fallbackCache_);
    if (runs.empty()) return 0;

    float width = 0.0f;
    SkRect unionBounds = SkRect::MakeEmpty();
    std::vector<float> runWidths;
    std::vector<SkRect> runBounds;
    runWidths.reserve(runs.size());
    runBounds.reserve(runs.size());
    float cursor = 0.0f;
    for (const auto& run : runs) {
        const char* data = text.data() + run.start;
        SkRect b;
        float w = run.font.measureText(data, run.length, SkTextEncoding::kUTF8, &b);
        runWidths.push_back(w);
        runBounds.push_back(b);
        // Bounds are relative to drawing origin; translate by cursor for union.
        SkRect placed = b; placed.offset(cursor, 0);
        if (unionBounds.isEmpty()) unionBounds = placed;
        else                       unionBounds.join(placed);
        cursor += w;
        width  += w;
    }
    int tw = (int)std::ceil(width) + 4;
    int th = (int)std::ceil(unionBounds.height()) + 4;
    if (tw <= 0 || th <= 0) return 0;

    // Render to a temporary Skia surface
    auto tmpSurface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(tw, th));
    if (!tmpSurface) return 0;

    auto* c = tmpSurface->getCanvas();
    c->clear(SK_ColorTRANSPARENT);

    SkPaint paint;
    paint.setColor(toSkColor(color));
    cursor = 0.0f;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        const auto& run = runs[r];
        const char* data = text.data() + run.start;
        c->drawSimpleText(data, run.length, SkTextEncoding::kUTF8,
                          -unionBounds.left() + 1 + cursor,
                          -unionBounds.top()  + 1,
                          run.font, paint);
        cursor += runWidths[r];
    }

    // Create GL texture and upload
    SkPixmap pixmap;
    if (!tmpSurface->peekPixels(&pixmap)) return 0;

    GLuint tex = gl_->createTexture2D(tw, th, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);
    if (!tex) return 0;

    gl_->uploadTexture2D(tex, pixmap.addr(), tw, th, GL_BGRA, GL_UNSIGNED_BYTE);

    textTexCache_[cacheKey] = {tex, tw, th};
    outW = tw;
    outH = th;
    return tex;
}

bool SkiaRenderer::saveScreenshot(const std::string& path) {
    if (!surface_) return false;

    // For GPU mode, read pixels back from the GPU surface
    SkPixmap pixmap;
    sk_sp<SkImage> image;
    SkBitmap bitmap;

    if (gpuMode_) {
        image = surface_->makeImageSnapshot();
        if (!image) return false;
        auto info = SkImageInfo::MakeN32Premul(image->width(), image->height());
        bitmap.allocPixels(info);
        if (!image->readPixels(bitmap.pixmap(), 0, 0)) return false;
        pixmap = bitmap.pixmap();
    } else {
        if (!surface_->peekPixels(&pixmap)) return false;
    }

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

std::vector<uint8_t> SkiaRenderer::capturePixels() {
    if (!surface_) return {};

    SkPixmap pixmap;
    sk_sp<SkImage> image;
    SkBitmap bitmap;

    if (gpuMode_) {
        image = surface_->makeImageSnapshot();
        if (!image) return {};
        auto info = SkImageInfo::MakeN32Premul(image->width(), image->height());
        bitmap.allocPixels(info);
        if (!image->readPixels(bitmap.pixmap(), 0, 0)) return {};
        pixmap = bitmap.pixmap();
    } else {
        if (!surface_->peekPixels(&pixmap)) return {};
    }

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

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer(GLContext* gl) {
    if (gl) {
        LOG_INFO("Creating SkiaRenderer (Skia raster + OpenGL display)");
        return std::make_unique<SkiaRenderer>(*gl);
    }
    LOG_ERROR("createRenderer: no GLContext provided");
    return nullptr;
}

} // namespace bro::render
