#include "render/skia_backend.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef BRO_NO_SKIA
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkFontMgr.h>
#include <include/codec/SkCodec.h>
#include <include/ports/SkTypeface_win.h>
#endif

namespace bro::render {

// ===========================================================================
#ifdef BRO_NO_SKIA
// ===========================================================================
// SDLRenderer -- real on-screen rendering via SDL3's 2D renderer + Win32 GDI text
// ===========================================================================

SDLRenderer::SDLRenderer(platform::Window& window) {
    // Create SDL renderer on the existing window (picks best GPU backend)
    sdlRenderer_ = SDL_CreateRenderer(window.getSDLWindow(), nullptr);
    if (!sdlRenderer_) {
        LOG_ERROR("Failed to create SDL_Renderer: %s", SDL_GetError());
    } else {
        const char* name = SDL_GetRendererName(sdlRenderer_);
        LOG_INFO("SDLRenderer created (backend: %s)", name ? name : "unknown");
        // Disable vsync for uncapped framerate
        SDL_SetRenderVSync(sdlRenderer_, SDL_RENDERER_VSYNC_DISABLED);
        // Enable alpha blending so transparent UI elements show the scene behind
        SDL_SetRenderDrawBlendMode(sdlRenderer_, SDL_BLENDMODE_BLEND);
    }
}

SDLRenderer::~SDLRenderer() {
    // Clean up Win32 fonts
    for (auto& [handle, info] : fonts_) {
#ifdef _WIN32
        if (info.hfont) DeleteObject((HFONT)info.hfont);
#endif
    }
    if (sdlRenderer_) {
        SDL_DestroyRenderer(sdlRenderer_);
    }
}

void SDLRenderer::clear(Color color) {
    if (!sdlRenderer_) return;
    SDL_SetRenderDrawColor(sdlRenderer_, color.r, color.g, color.b, color.a);
    SDL_RenderClear(sdlRenderer_);
}

void SDLRenderer::drawRect(float x, float y, float w, float h, Color color) {
    if (!sdlRenderer_) return;
    SDL_SetRenderDrawColor(sdlRenderer_, color.r, color.g, color.b, color.a);
    SDL_FRect rect = {x, y, w, h};
    SDL_RenderRect(sdlRenderer_, &rect);
}

void SDLRenderer::drawRoundRect(float x, float y, float w, float h, float /*rx*/, float /*ry*/, Color color) {
    // SDL doesn't have native round rect -- draw a regular rect
    drawRect(x, y, w, h, color);
}

void SDLRenderer::fillRect(float x, float y, float w, float h, Color color) {
    if (!sdlRenderer_ || color.a == 0) return;
    SDL_SetRenderDrawColor(sdlRenderer_, color.r, color.g, color.b, color.a);
    SDL_FRect rect = {x, y, w, h};
    SDL_RenderFillRect(sdlRenderer_, &rect);
}

void SDLRenderer::drawText(std::string_view text, float x, float y, uint64_t font_handle, Color color) {
    if (!sdlRenderer_ || text.empty()) return;
#ifdef _WIN32
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) return;

    HFONT hfont = (HFONT)it->second.hfont;
    if (!hfont) return;
    float fontSize = it->second.size;

    // Render text to a small DIB via GDI, then upload as SDL texture
    std::string str(text);

    HDC hdc = CreateCompatibleDC(nullptr);
    SelectObject(hdc, hfont);

    SIZE textSize;
    GetTextExtentPoint32A(hdc, str.c_str(), (int)str.size(), &textSize);
    if (textSize.cx <= 0 || textSize.cy <= 0) {
        DeleteDC(hdc);
        return;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = textSize.cx;
    bmi.bmiHeader.biHeight = -textSize.cy; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        DeleteDC(hdc);
        return;
    }

    SelectObject(hdc, hbmp);
    SelectObject(hdc, hfont);

    // Draw white text on black background, we'll use it as alpha mask
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutA(hdc, 0, 0, str.c_str(), (int)str.size());
    GdiFlush();

    // Convert: set color channels to requested color, alpha from GDI luminance
    uint8_t* pixels = (uint8_t*)bits;
    for (int i = 0; i < textSize.cx * textSize.cy; i++) {
        uint8_t alpha = pixels[i * 4 + 0]; // blue channel (all channels are same for white)
        pixels[i * 4 + 0] = color.b;
        pixels[i * 4 + 1] = color.g;
        pixels[i * 4 + 2] = color.r;
        pixels[i * 4 + 3] = alpha;
    }

    SDL_Surface* surf = SDL_CreateSurfaceFrom(textSize.cx, textSize.cy,
                                               SDL_PIXELFORMAT_ARGB8888,
                                               bits, textSize.cx * 4);
    if (surf) {
        SDL_Texture* tex = SDL_CreateTextureFromSurface(sdlRenderer_, surf);
        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_FRect dst = {x, y, (float)textSize.cx, (float)textSize.cy};
            SDL_RenderTexture(sdlRenderer_, tex, nullptr, &dst);
            SDL_DestroyTexture(tex);
        }
        SDL_DestroySurface(surf);
    }

    DeleteObject(hbmp);
    DeleteDC(hdc);
#else
    (void)text; (void)x; (void)y; (void)font_handle; (void)color;
#endif
}

TextMetrics SDLRenderer::measureText(std::string_view text, uint64_t font_handle) {
    TextMetrics m = {};
#ifdef _WIN32
    auto it = fonts_.find(font_handle);
    if (it == fonts_.end()) {
        m.width = static_cast<float>(text.size()) * 8.0f;
        m.height = 16.0f;
        return m;
    }

    HFONT hfont = (HFONT)it->second.hfont;
    if (!hfont) {
        m.width = static_cast<float>(text.size()) * it->second.size * 0.6f;
        m.height = it->second.size;
        return m;
    }

    HDC hdc = CreateCompatibleDC(nullptr);
    SelectObject(hdc, hfont);

    std::string str(text);
    SIZE textSize;
    GetTextExtentPoint32A(hdc, str.c_str(), (int)str.size(), &textSize);
    DeleteDC(hdc);

    m.width = (float)textSize.cx;
    m.height = (float)textSize.cy;
#else
    auto it = fonts_.find(font_handle);
    float sz = (it != fonts_.end()) ? it->second.size : 16.0f;
    m.width = static_cast<float>(text.size()) * sz * 0.6f;
    m.height = sz;
#endif
    return m;
}

uint64_t SDLRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    uint64_t handle = nextFontHandle_++;
    FontInfo info;
    info.family = std::string(family);
    info.size = size;
    info.weight = weight;
    info.italic = italic;

#ifdef _WIN32
    // Extract first font family name (before comma)
    std::string fam(family);
    auto comma = fam.find(',');
    if (comma != std::string::npos) fam = fam.substr(0, comma);
    // Trim whitespace
    while (!fam.empty() && fam.front() == ' ') fam.erase(fam.begin());
    while (!fam.empty() && fam.back() == ' ') fam.pop_back();

    int lfWeight = FW_NORMAL;
    if (weight >= 700) lfWeight = FW_BOLD;
    else if (weight >= 600) lfWeight = FW_SEMIBOLD;
    else if (weight >= 500) lfWeight = FW_MEDIUM;
    else if (weight <= 300) lfWeight = FW_LIGHT;

    HFONT hfont = CreateFontA(
        -(int)(size * 96.0f / 72.0f),  // height in pixels (negative = char height)
        0, 0, 0,
        lfWeight,
        italic ? TRUE : FALSE,
        FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        fam.c_str()
    );
    info.hfont = (void*)hfont;
#endif

    fonts_[handle] = std::move(info);
    return handle;
}

void SDLRenderer::deleteFont(uint64_t font_handle) {
    auto it = fonts_.find(font_handle);
    if (it != fonts_.end()) {
#ifdef _WIN32
        if (it->second.hfont) DeleteObject((HFONT)it->second.hfont);
#endif
        fonts_.erase(it);
    }
}

void SDLRenderer::drawLine(float x1, float y1, float x2, float y2, Color color, float /*thickness*/) {
    if (!sdlRenderer_) return;
    SDL_SetRenderDrawColor(sdlRenderer_, color.r, color.g, color.b, color.a);
    SDL_RenderLine(sdlRenderer_, x1, y1, x2, y2);
}

void SDLRenderer::drawImage(const void* /*data*/, size_t /*len*/, float /*x*/, float /*y*/, float /*w*/, float /*h*/) {
    // TODO: decode image and render
}

void SDLRenderer::setClip(float x, float y, float w, float h) {
    if (!sdlRenderer_) return;
    SDL_Rect r = {(int)x, (int)y, (int)w, (int)h};
    SDL_SetRenderClipRect(sdlRenderer_, &r);
}

void SDLRenderer::resetClip() {
    if (!sdlRenderer_) return;
    SDL_SetRenderClipRect(sdlRenderer_, nullptr);
}

void SDLRenderer::beginFrame(int width, int height) {
    frameWidth_ = width;
    frameHeight_ = height;
    // SDL renderer manages its own frame -- nothing needed here
}

void SDLRenderer::endFrame() {
    if (!sdlRenderer_) return;
    SDL_RenderPresent(sdlRenderer_);
}

// ---------------------------------------------------------------------------
// Factory (no Skia)
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer(platform::Window* window) {
    if (window) {
        LOG_INFO("Creating SDLRenderer (Skia not available)");
        return std::make_unique<SDLRenderer>(*window);
    }
    LOG_ERROR("createRenderer: no Window provided, cannot create SDLRenderer");
    return nullptr;
}

// ===========================================================================
#else // Skia IS available
// ===========================================================================
// SkiaRenderer — Skia raster rendering + SDL display
//
// Renders to a CPU-side Skia surface for full-quality text and graphics,
// then uploads the pixels to an SDL texture for GPU-accelerated display.
// SDL handles GPU backend selection (D3D11/12, Metal, etc.).
// ===========================================================================

SkiaRenderer::SkiaRenderer(platform::Window& window) {
    sdlRenderer_ = SDL_CreateRenderer(window.getSDLWindow(), nullptr);
    if (!sdlRenderer_) {
        LOG_ERROR("Failed to create SDL_Renderer: %s", SDL_GetError());
        return;
    }
    const char* name = SDL_GetRendererName(sdlRenderer_);
    LOG_INFO("SkiaRenderer created (SDL backend: %s)", name ? name : "unknown");
    SDL_SetRenderVSync(sdlRenderer_, SDL_RENDERER_VSYNC_DISABLED);
    SDL_SetRenderDrawBlendMode(sdlRenderer_, SDL_BLENDMODE_BLEND);
}

SkiaRenderer::~SkiaRenderer() {
    for (auto& [k, e] : textTexCache_) SDL_DestroyTexture(e.tex);
    textTexCache_.clear();
    fonts_.clear();
    surface_.reset();
    if (uiTexture_) SDL_DestroyTexture(uiTexture_);
    if (sdlRenderer_) SDL_DestroyRenderer(sdlRenderer_);
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
    TextMetrics m;
    m.width  = width;
    m.height = bounds.height();
    return m;
}

uint64_t SkiaRenderer::createFont(std::string_view family, float size, int weight, bool italic) {
    SkFontStyle style(weight,
                      SkFontStyle::kNormal_Width,
                      italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);

    sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_DirectWrite();

    // CSS font-family is comma-separated (e.g. "Arial,sans-serif").
    // Try each family name in order until one matches.
    sk_sp<SkTypeface> typeface;
    std::string families(family);
    std::istringstream stream(families);
    std::string name;
    while (std::getline(stream, name, ',')) {
        // Trim whitespace and quotes
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
    if (!canvas_) return;
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void SkiaRenderer::resetClip() {
    if (!canvas_) return;
    canvas_->restore();
    canvas_->save();
}

void SkiaRenderer::beginFrame(int width, int height) {
    if (!sdlRenderer_) { canvas_ = nullptr; return; }

    // (Re)create raster surface and UI texture if size changed
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        if (uiTexture_) SDL_DestroyTexture(uiTexture_);
        uiTexture_ = SDL_CreateTexture(sdlRenderer_, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, width, height);
        SDL_SetTextureBlendMode(uiTexture_, SDL_BLENDMODE_BLEND);
        textureWidth_ = width;
        textureHeight_ = height;
    }

    canvas_ = surface_->getCanvas();
    // Clear to fully transparent so scene shows through
    canvas_->clear(SK_ColorTRANSPARENT);
    canvas_->save();
}

void SkiaRenderer::endFrame() {
    if (canvas_) canvas_->restore();
    canvas_ = nullptr;

    if (!sdlRenderer_ || !surface_ || !uiTexture_) return;

    // Upload Skia raster pixels to the UI overlay texture
    SkPixmap pixmap;
    if (!surface_->peekPixels(&pixmap)) return;
    SDL_UpdateTexture(uiTexture_, nullptr, pixmap.addr(), (int)pixmap.rowBytes());
    // NOTE: Does NOT present. The engine handles compositing and presentation.
}

SDL_Texture* SkiaRenderer::renderTextToTexture(std::string_view text, uint64_t font_handle,
                                                Color color, int& outW, int& outH) {
    if (!sdlRenderer_ || text.empty()) return nullptr;

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

    // Extract pixels and upload to SDL texture
    SkPixmap pixmap;
    if (!tmpSurface->peekPixels(&pixmap)) return nullptr;

    SDL_Texture* tex = SDL_CreateTexture(sdlRenderer_, SDL_PIXELFORMAT_ARGB8888,
                                          SDL_TEXTUREACCESS_STATIC, tw, th);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(tex, nullptr, pixmap.addr(), (int)pixmap.rowBytes());

    textTexCache_[cacheKey] = {tex, tw, th};
    outW = tw;
    outH = th;
    return tex;
}

// ---------------------------------------------------------------------------
// Factory (Skia available)
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer(platform::Window* window) {
    if (window) {
        LOG_INFO("Creating SkiaRenderer (Skia raster + SDL display)");
        return std::make_unique<SkiaRenderer>(*window);
    }
    LOG_ERROR("createRenderer: no Window provided");
    return nullptr;
}

#endif // BRO_NO_SKIA

} // namespace bro::render
