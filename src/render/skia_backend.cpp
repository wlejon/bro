#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "util/log.h"

#include <cstring>
#include <sstream>
#include <cmath>

#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkColorSpace.h>
#include <include/codec/SkCodec.h>
#include <include/ports/SkTypeface_win.h>
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
    return { width, bounds.height() };
}

uint64_t SkiaRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    SkFontStyle style(weight,
                      SkFontStyle::kNormal_Width,
                      italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);

    sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_DirectWrite();

    // Map CSS generic family names to real font names
    auto resolveGeneric = [](const std::string& name) -> const char* {
        if (name == "sans-serif")  return "Arial";
        if (name == "serif")       return "Times New Roman";
        if (name == "monospace")   return "Consolas";
        if (name == "cursive")     return "Comic Sans MS";
        if (name == "fantasy")     return "Impact";
        if (name == "system-ui")   return "Segoe UI";
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
