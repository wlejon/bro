#include "render/skia_backend.h"
#include "render/gpu_context.h"
#include "util/log.h"

#include <SDL3/SDL_gpu.h>
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
#include <include/codec/SkCodec.h>
#include <include/ports/SkTypeface_win.h>

namespace bro::render {

// ===========================================================================
// SkiaRenderer — Skia raster rendering + SDL_GPU display
// ===========================================================================

SkiaRenderer::SkiaRenderer(GPUContext& gpu) : gpu_(&gpu) {
    LOG_INFO("SkiaRenderer created (SDL_GPU backend)");
}

SkiaRenderer::~SkiaRenderer() {
    for (auto& [k, e] : textTexCache_) {
        gpu_->releaseTexture(e.tex);
    }
    textTexCache_.clear();
    fonts_.clear();
    surface_.reset();
    if (uiTexture_) gpu_->releaseTexture(uiTexture_);
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
    if (!canvas_) return;
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) return;
    SkPaint paint;
    paint.setColor(toSkColor(color));
    canvas_->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, x, y, *it->second.font, paint);
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

    sk_sp<SkTypeface> typeface;
    std::string families(family);
    std::istringstream stream(families);
    std::string name;
    while (std::getline(stream, name, ',')) {
        while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
        if (name.empty()) continue;
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
    // (Re)create raster surface and GPU texture if size changed
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        if (uiTexture_) gpu_->releaseTexture(uiTexture_);
        uiTexture_ = gpu_->createTexture2D(
            width, height,
            SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
            SDL_GPU_TEXTUREUSAGE_SAMPLER);
        textureWidth_ = width;
        textureHeight_ = height;
    }

    canvas_ = surface_->getCanvas();
    canvas_->clear(SK_ColorTRANSPARENT);
    canvas_->save();
}

void SkiaRenderer::endFrame() {
    if (canvas_) canvas_->restore();
    canvas_ = nullptr;
    pixelsPending_ = (surface_ && uiTexture_);
}

void SkiaRenderer::uploadToGPU(SDL_GPUCommandBuffer* cmd) {
    if (!pixelsPending_ || !surface_ || !uiTexture_) return;
    pixelsPending_ = false;

    SkPixmap pixmap;
    if (!surface_->peekPixels(&pixmap)) return;

    gpu_->uploadToTexture(cmd, uiTexture_,
                          pixmap.addr(),
                          static_cast<uint32_t>(pixmap.width()),
                          static_cast<uint32_t>(pixmap.height()),
                          static_cast<uint32_t>(pixmap.rowBytes()));
}

SDL_GPUTexture* SkiaRenderer::renderTextToTexture(SDL_GPUCommandBuffer* cmd,
                                                    std::string_view text,
                                                    uint64_t font_handle,
                                                    Color color,
                                                    int& outW, int& outH) {
    if (text.empty()) return nullptr;

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
    if (fit == fonts_.end()) return nullptr;
    const SkFont& font = *fit->second.font;

    SkRect bounds;
    float width = font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &bounds);
    int tw = (int)std::ceil(width) + 4;
    int th = (int)std::ceil(bounds.height()) + 4;
    if (tw <= 0 || th <= 0) return nullptr;

    // Render to a temporary Skia surface
    auto tmpSurface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(tw, th));
    if (!tmpSurface) return nullptr;

    auto* c = tmpSurface->getCanvas();
    c->clear(SK_ColorTRANSPARENT);

    SkPaint paint;
    paint.setColor(toSkColor(color));
    c->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8,
                      -bounds.left() + 1, -bounds.top() + 1, font, paint);

    // Create GPU texture and upload
    SkPixmap pixmap;
    if (!tmpSurface->peekPixels(&pixmap)) return nullptr;

    SDL_GPUTexture* tex = gpu_->createTexture2D(
        tw, th, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!tex) return nullptr;

    gpu_->uploadToTexture(cmd, tex, pixmap.addr(), tw, th, (uint32_t)pixmap.rowBytes());

    textTexCache_[cacheKey] = {tex, tw, th};
    outW = tw;
    outH = th;
    return tex;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer(GPUContext* gpu) {
    if (gpu) {
        LOG_INFO("Creating SkiaRenderer (Skia raster + SDL_GPU display)");
        return std::make_unique<SkiaRenderer>(*gpu);
    }
    LOG_ERROR("createRenderer: no GPUContext provided");
    return nullptr;
}

} // namespace bro::render
