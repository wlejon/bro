#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "util/log.h"

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
#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
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

SkiaRenderer::SkiaRenderer(GLContext& gl) : gl_(&gl) {
    // Try to create Skia GPU (Ganesh GL) context
    auto glInterface = GrGLMakeNativeInterface();
    if (glInterface) {
        grContext_ = GrDirectContexts::MakeGL(glInterface);
    }
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
    canvas_->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8,
                            x, y, *fontIt->second.font, paint);
}

TextMetrics SkiaRenderer::measureText(std::string_view text, uint64_t font_handle) {
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) return {};
    const SkFont& font = *it->second.font;
    SkRect bounds;
    float width = font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &bounds);
    SkFontMetrics fm;
    font.getMetrics(&fm);
    return { width, bounds.height(), -fm.fAscent, fm.fDescent };
}

uint64_t SkiaRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    SkFontStyle style(weight,
                      SkFontStyle::kNormal_Width,
                      italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);

#ifdef _WIN32
    sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_DirectWrite();
#else
    sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif

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
        // Try CSS generic name first
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
    fonts_[handle] = FontEntry{std::move(typeface), std::move(sk_font)};
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

void SkiaRenderer::save() {
    if (canvas_) canvas_->save();
}

void SkiaRenderer::restore() {
    if (canvas_) canvas_->restore();
}

void SkiaRenderer::translate(float dx, float dy) {
    if (canvas_) canvas_->translate(dx, dy);
}

void SkiaRenderer::scale(float sx, float sy) {
    if (canvas_) canvas_->scale(sx, sy);
}

void SkiaRenderer::drawImage(const void* data, size_t len, float x, float y, float w, float h) {
    if (!canvas_) return;
    sk_sp<SkData> sk_data = SkData::MakeWithoutCopy(data, len);
    auto codec = SkCodec::MakeFromData(sk_data);
    if (!codec) return;
    auto [image, result] = codec->getImage();
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
    auto shader = SkShaders::LinearGradient(pts, SkGradient(buildGradColors(stops), {}));
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

void SkiaRenderer::fillConicGradient(float x, float y, float w, float h,
                                     float cx, float cy, float angleDeg,
                                     std::span<const ColorStop> stops) {
    if (!canvas_ || stops.empty()) return;
    // Skia sweep: 0° = 3 o'clock. CSS conic: 0° = 12 o'clock.
    // CSS angle is clockwise from top, so offset by (angleDeg - 90).
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
        // Flush Skia GPU commands — renders directly to uiTexture_ via FBO
        grContext_->flushAndSubmit(surface_.get());
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
    const SkFont& font = *fit->second.font;

    SkRect bounds;
    float width = font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &bounds);
    int tw = (int)std::ceil(width) + 4;
    int th = (int)std::ceil(bounds.height()) + 4;
    if (tw <= 0 || th <= 0) return 0;

    // Render to a temporary Skia surface
    auto tmpSurface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(tw, th));
    if (!tmpSurface) return 0;

    auto* c = tmpSurface->getCanvas();
    c->clear(SK_ColorTRANSPARENT);

    SkPaint paint;
    paint.setColor(toSkColor(color));
    c->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8,
                      -bounds.left() + 1, -bounds.top() + 1, font, paint);

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
