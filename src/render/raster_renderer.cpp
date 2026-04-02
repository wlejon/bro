#include "render/raster_renderer.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSurface.h>
#include <include/codec/SkCodec.h>
#include <include/effects/SkGradient.h>
#include <include/ports/SkTypeface_win.h>
#include <include/utils/SkParsePath.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace bro::render {

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

void RasterRenderer::drawText(std::string_view text, float x, float y, uint64_t font_handle, Color c) {
    if (!canvas_) return;
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) return;
    SkPaint paint;
    paint.setColor(toSkColor(c));
    canvas_->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8,
                            x, y, *it->second.font, paint);
}

TextMetrics RasterRenderer::measureText(std::string_view text, uint64_t font_handle) {
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) return {};
    const SkFont& font = *it->second.font;
    SkRect bounds;
    float width = font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &bounds);
    SkFontMetrics fm;
    font.getMetrics(&fm);
    return { width, bounds.height(), -fm.fAscent, fm.fDescent };
}

uint64_t RasterRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    SkFontStyle style(weight, SkFontStyle::kNormal_Width,
                      italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
    sk_sp<SkFontMgr> mgr = SkFontMgr_New_DirectWrite();
    // CSS font-family is comma-separated — try each name in order.
    sk_sp<SkTypeface> typeface;
    std::string families(family);
    std::istringstream stream(families);
    std::string name;
    while (std::getline(stream, name, ',')) {
        while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
        if (name.empty()) continue;
        typeface = mgr->matchFamilyStyle(name.c_str(), style);
        if (typeface) break;
    }
    if (!typeface) typeface = mgr->matchFamilyStyle(nullptr, SkFontStyle());
    auto sk_font = std::make_unique<SkFont>(typeface, size);
    sk_font->setEdging(SkFont::Edging::kSubpixelAntiAlias);
    sk_font->setSubpixel(true);
    uint64_t handle = nextHandle_++;
    fonts_[handle] = { std::move(typeface), std::move(sk_font) };
    return handle;
}

void RasterRenderer::deleteFont(uint64_t h) { fonts_.erase(h); }

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

void RasterRenderer::save() { if (canvas_) canvas_->save(); }
void RasterRenderer::restore() { if (canvas_) canvas_->restore(); }
void RasterRenderer::translate(float dx, float dy) { if (canvas_) canvas_->translate(dx, dy); }
void RasterRenderer::scale(float sx, float sy) { if (canvas_) canvas_->scale(sx, sy); }

void RasterRenderer::setClip(float x, float y, float w, float h) {
    if (canvas_) canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void RasterRenderer::resetClip() {
    if (!canvas_) return;
    canvas_->restore();
    canvas_->save();
}

void RasterRenderer::fillLinearGradient(float x, float y, float w, float h,
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

void RasterRenderer::fillConicGradient(float x, float y, float w, float h,
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
    int rowBytes = ((w * 3 + 3) / 4) * 4; // BMP rows are 4-byte aligned
    int dataSize = rowBytes * h;

    // BMP file header (14 bytes) + BITMAPINFOHEADER (40 bytes)
    uint8_t header[54] = {};
    auto put16 = [&](int off, uint16_t v) { memcpy(header + off, &v, 2); };
    auto put32 = [&](int off, uint32_t v) { memcpy(header + off, &v, 4); };
    header[0] = 'B'; header[1] = 'M';
    put32(2, 54 + dataSize);   // file size
    put32(10, 54);             // pixel data offset
    put32(14, 40);             // info header size
    put32(18, w);
    put32(22, h);
    put16(26, 1);              // planes
    put16(28, 24);             // bits per pixel
    put32(34, dataSize);

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<char*>(header), 54);

    // BMP is bottom-to-top, BGR
    std::vector<uint8_t> row(rowBytes, 0);
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(pixmap.addr32(0, y));
        for (int x = 0; x < w; ++x) {
            // N32 on Windows = BGRA premultiplied
            row[x * 3 + 0] = src[x * 4 + 0]; // B
            row[x * 3 + 1] = src[x * 4 + 1]; // G
            row[x * 3 + 2] = src[x * 4 + 2]; // R
        }
        out.write(reinterpret_cast<char*>(row.data()), rowBytes);
    }
    return out.good();
}

} // namespace bro::render
