#include "render/skia_backend.h"
#include "util/log.h"

#include <cstring>
#include <sstream>

#ifndef BRO_NO_SKIA
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkFontMgr.h>
#include <include/gpu/GrBackendSurface.h>
#endif

namespace bro::render {

// ===========================================================================
#ifdef BRO_NO_SKIA
// ===========================================================================
// SoftwareRenderer implementation (stub / debug)
// ===========================================================================

SoftwareRenderer::SoftwareRenderer() {
    LOG_INFO("SoftwareRenderer created (Skia not available)");
}

SoftwareRenderer::~SoftwareRenderer() = default;

void SoftwareRenderer::clear(Color color) {
    LOG_INFO("SoftwareRenderer::clear(%u, %u, %u, %u)", color.r, color.g, color.b, color.a);
    std::ostringstream os;
    os << "clear(" << (int)color.r << "," << (int)color.g << "," << (int)color.b << "," << (int)color.a << ")";
    commands_.push_back(os.str());
}

void SoftwareRenderer::drawRect(float x, float y, float w, float h, Color color) {
    LOG_INFO("SoftwareRenderer::drawRect(%.1f, %.1f, %.1f, %.1f, rgba(%u,%u,%u,%u))",
             x, y, w, h, color.r, color.g, color.b, color.a);
    std::ostringstream os;
    os << "drawRect(" << x << "," << y << "," << w << "," << h << ")";
    commands_.push_back(os.str());
}

void SoftwareRenderer::drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) {
    LOG_INFO("SoftwareRenderer::drawRoundRect(%.1f, %.1f, %.1f, %.1f, rx=%.1f, ry=%.1f, rgba(%u,%u,%u,%u))",
             x, y, w, h, rx, ry, color.r, color.g, color.b, color.a);
    std::ostringstream os;
    os << "drawRoundRect(" << x << "," << y << "," << w << "," << h << "," << rx << "," << ry << ")";
    commands_.push_back(os.str());
}

void SoftwareRenderer::fillRect(float x, float y, float w, float h, Color color) {
    LOG_INFO("SoftwareRenderer::fillRect(%.1f, %.1f, %.1f, %.1f, rgba(%u,%u,%u,%u))",
             x, y, w, h, color.r, color.g, color.b, color.a);
    std::ostringstream os;
    os << "fillRect(" << x << "," << y << "," << w << "," << h << ")";
    commands_.push_back(os.str());
}

void SoftwareRenderer::drawText(std::string_view text, float x, float y, uint64_t font_handle, Color color) {
    LOG_INFO("SoftwareRenderer::drawText(\"%.*s\", %.1f, %.1f, font=%llu, rgba(%u,%u,%u,%u))",
             static_cast<int>(text.size()), text.data(), x, y,
             static_cast<unsigned long long>(font_handle),
             color.r, color.g, color.b, color.a);
    std::ostringstream os;
    os << "drawText(\"" << text << "\"," << x << "," << y << ",font=" << font_handle << ")";
    commands_.push_back(os.str());
}

TextMetrics SoftwareRenderer::measureText(std::string_view text, uint64_t font_handle) {
    float font_size = 16.0f; // default
    auto it = fonts_.find(font_handle);
    if (it != fonts_.end()) {
        font_size = it->second.size;
    }
    TextMetrics m;
    m.width  = static_cast<float>(text.size()) * font_size * 0.6f;
    m.height = font_size;
    return m;
}

uint64_t SoftwareRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    uint64_t handle = next_font_handle_++;
    fonts_[handle] = FontInfo{std::string(family), size, weight, italic};
    LOG_INFO("SoftwareRenderer::createFont(\"%.*s\", size=%.1f, weight=%d, italic=%d) -> handle %llu",
             static_cast<int>(family.size()), family.data(), size, weight, italic ? 1 : 0,
             static_cast<unsigned long long>(handle));
    return handle;
}

void SoftwareRenderer::deleteFont(uint64_t font_handle) {
    fonts_.erase(font_handle);
    LOG_INFO("SoftwareRenderer::deleteFont(%llu)", static_cast<unsigned long long>(font_handle));
}

void SoftwareRenderer::drawLine(float x1, float y1, float x2, float y2, Color color, float thickness) {
    LOG_INFO("SoftwareRenderer::drawLine(%.1f, %.1f -> %.1f, %.1f, thickness=%.1f, rgba(%u,%u,%u,%u))",
             x1, y1, x2, y2, thickness, color.r, color.g, color.b, color.a);
    std::ostringstream os;
    os << "drawLine(" << x1 << "," << y1 << "," << x2 << "," << y2 << ",t=" << thickness << ")";
    commands_.push_back(os.str());
}

void SoftwareRenderer::drawImage(const void* /*data*/, size_t len, float x, float y, float w, float h) {
    LOG_INFO("SoftwareRenderer::drawImage(%zu bytes, %.1f, %.1f, %.1f, %.1f)", len, x, y, w, h);
    std::ostringstream os;
    os << "drawImage(" << len << " bytes," << x << "," << y << "," << w << "," << h << ")";
    commands_.push_back(os.str());
}

void SoftwareRenderer::setClip(float x, float y, float w, float h) {
    LOG_INFO("SoftwareRenderer::setClip(%.1f, %.1f, %.1f, %.1f)", x, y, w, h);
    std::ostringstream os;
    os << "setClip(" << x << "," << y << "," << w << "," << h << ")";
    commands_.push_back(os.str());
}

void SoftwareRenderer::resetClip() {
    LOG_INFO("SoftwareRenderer::resetClip()");
    commands_.push_back("resetClip()");
}

void SoftwareRenderer::beginFrame(int width, int height) {
    LOG_INFO("SoftwareRenderer::beginFrame(%d, %d)", width, height);
    commands_.clear();
}

void SoftwareRenderer::endFrame() {
    LOG_INFO("SoftwareRenderer::endFrame() -- %zu commands recorded", commands_.size());
}

// ---------------------------------------------------------------------------
// Factory (no Skia)
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer(platform::VulkanContext* /*vk*/) {
    LOG_INFO("Creating SoftwareRenderer (BRO_NO_SKIA defined)");
    return std::make_unique<SoftwareRenderer>();
}

// ===========================================================================
#else // Skia IS available
// ===========================================================================
// SkiaRenderer implementation
// ===========================================================================

SkiaRenderer::SkiaRenderer(platform::VulkanContext& vk)
    : vk_(vk)
{
    GrVkBackendContext backend_ctx{};
    // TODO: populate backend_ctx from vk_ (instance, device, queue, etc.)

    gr_context_ = GrDirectContext::MakeVulkan(backend_ctx);
    if (!gr_context_) {
        LOG_ERROR("Failed to create GrDirectContext from Vulkan backend");
    }
}

SkiaRenderer::~SkiaRenderer() {
    fonts_.clear();
    surface_.reset();
    if (gr_context_) {
        gr_context_->abandonContext();
    }
}

SkColor SkiaRenderer::toSkColor(Color c) const {
    return SkColorSetARGB(c.a, c.r, c.g, c.b);
}

void SkiaRenderer::clear(Color color) {
    if (canvas_) {
        canvas_->clear(toSkColor(color));
    }
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
    if (it == fonts_.end()) {
        LOG_WARN("drawText: unknown font handle %llu", static_cast<unsigned long long>(font_handle));
        return;
    }
    SkPaint paint;
    paint.setColor(toSkColor(color));
    canvas_->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, x, y, *it->second.font, paint);
}

TextMetrics SkiaRenderer::measureText(std::string_view text, uint64_t font_handle) {
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) {
        LOG_WARN("measureText: unknown font handle %llu", static_cast<unsigned long long>(font_handle));
        return {};
    }
    const SkFont& font = *it->second.font;
    SkRect bounds;
    float width = font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &bounds);
    TextMetrics m;
    m.width  = width;
    m.height = bounds.height();
    return m;
}

uint64_t SkiaRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    SkFontStyle style(weight,
                      SkFontStyle::kNormal_Width,
                      italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);

    sk_sp<SkFontMgr> font_mgr = SkFontMgr::RefDefault();
    sk_sp<SkTypeface> typeface = font_mgr->matchFamilyStyle(std::string(family).c_str(), style);
    if (!typeface) {
        typeface = SkTypeface::MakeDefault();
    }

    auto sk_font = std::make_unique<SkFont>(typeface, size);
    sk_font->setEdging(SkFont::Edging::kSubpixelAntiAlias);
    sk_font->setSubpixel(true);

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
    sk_sp<SkImage> image = SkImage::MakeFromEncoded(sk_data);
    if (!image) {
        LOG_WARN("drawImage: failed to decode image (%zu bytes)", len);
        return;
    }
    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
}

void SkiaRenderer::setClip(float x, float y, float w, float h) {
    if (!canvas_) return;
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void SkiaRenderer::resetClip() {
    if (!canvas_) return;
    canvas_->restore();
    canvas_->save();
}

void SkiaRenderer::beginFrame(int width, int height) {
    // TODO: acquire swapchain image from vk_, wrap as SkSurface
    // For now, create an offscreen surface if dimensions changed or surface is null.
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
        surface_ = SkSurface::MakeRenderTarget(gr_context_.get(), SkBudgeted::kNo, info);
        if (!surface_) {
            LOG_ERROR("SkiaRenderer::beginFrame: failed to create SkSurface (%dx%d)", width, height);
            canvas_ = nullptr;
            return;
        }
    }
    canvas_ = surface_->getCanvas();
    canvas_->save();
}

void SkiaRenderer::endFrame() {
    if (canvas_) {
        canvas_->restore();
    }
    if (surface_) {
        surface_->flushAndSubmit();
    }
    // TODO: present swapchain image via vk_
    canvas_ = nullptr;
}

// ---------------------------------------------------------------------------
// Factory (Skia available)
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer(platform::VulkanContext* vk) {
    if (vk) {
        LOG_INFO("Creating SkiaRenderer with Vulkan backend");
        return std::make_unique<SkiaRenderer>(*vk);
    }
    LOG_ERROR("createRenderer: VulkanContext is null, cannot create SkiaRenderer");
    return nullptr;
}

#endif // BRO_NO_SKIA

} // namespace bro::render
