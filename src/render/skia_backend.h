#pragma once

#include "render/renderer.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef BRO_NO_SKIA
#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#include <include/core/SkFont.h>
#include <include/core/SkTypeface.h>
#include <include/core/SkFontMgr.h>
#endif

struct SDL_Renderer;
struct SDL_Window;
struct SDL_Texture;

namespace bro::platform {
    class VulkanContext;
    class Window;
} // namespace bro::platform

namespace bro::render {

// ---------------------------------------------------------------------------
// SkiaRenderer -- Skia raster UI + SDL GPU scene compositing
//
// The UI (HTML/CSS) is rendered to a CPU-side Skia surface with transparency,
// uploaded to an SDL texture, and composited over GPU-rendered scene content.
// The scene layer draws directly to SDL_Renderer (GPU-accelerated).
// ---------------------------------------------------------------------------
#ifndef BRO_NO_SKIA

class SkiaRenderer final : public Renderer {
public:
    explicit SkiaRenderer(platform::Window& window);
    ~SkiaRenderer() override;

    void clear(Color color) override;

    void drawRect(float x, float y, float w, float h, Color color) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) override;
    void fillRect(float x, float y, float w, float h, Color color) override;

    void drawText(std::string_view text, float x, float y, uint64_t font_handle, Color color) override;
    TextMetrics measureText(std::string_view text, uint64_t font_handle) override;

    uint64_t createFont(std::string_view family, float size, int weight, bool italic) override;
    void deleteFont(uint64_t font_handle) override;

    void drawLine(float x1, float y1, float x2, float y2, Color color, float thickness) override;
    void drawImage(const void* data, size_t len, float x, float y, float w, float h) override;

    void setClip(float x, float y, float w, float h) override;
    void resetClip() override;

    // beginFrame/endFrame manage the Skia raster surface for UI rendering.
    // They do NOT call SDL_RenderPresent — the engine handles presentation.
    void beginFrame(int width, int height) override;
    void endFrame() override;

    /// Access the underlying SDL GPU renderer (for scene layers and compositing).
    SDL_Renderer* getSDLRenderer() const { return sdlRenderer_; }

    /// Access the UI overlay texture (alpha-blended Skia content).
    SDL_Texture* getUITexture() const { return uiTexture_; }

private:
    SkColor toSkColor(Color c) const;

    SDL_Renderer* sdlRenderer_ = nullptr;
    SDL_Texture* uiTexture_ = nullptr;
    int textureWidth_ = 0;
    int textureHeight_ = 0;

    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr;

    struct FontEntry {
        sk_sp<SkTypeface> typeface;
        std::unique_ptr<SkFont> font;
    };
    std::unordered_map<uint64_t, FontEntry> fonts_;
    uint64_t next_font_handle_ = 1;
};

#else // BRO_NO_SKIA

// ---------------------------------------------------------------------------
// SDLRenderer -- uses SDL3's 2D renderer for real on-screen output
// ---------------------------------------------------------------------------
class SDLRenderer final : public Renderer {
public:
    explicit SDLRenderer(platform::Window& window);
    ~SDLRenderer() override;

    void clear(Color color) override;

    void drawRect(float x, float y, float w, float h, Color color) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) override;
    void fillRect(float x, float y, float w, float h, Color color) override;

    void drawText(std::string_view text, float x, float y, uint64_t font_handle, Color color) override;
    TextMetrics measureText(std::string_view text, uint64_t font_handle) override;

    uint64_t createFont(std::string_view family, float size, int weight, bool italic) override;
    void deleteFont(uint64_t font_handle) override;

    void drawLine(float x1, float y1, float x2, float y2, Color color, float thickness) override;
    void drawImage(const void* data, size_t len, float x, float y, float w, float h) override;

    void setClip(float x, float y, float w, float h) override;
    void resetClip() override;

    void beginFrame(int width, int height) override;
    void endFrame() override;

    SDL_Renderer* getSDLRenderer() const { return sdlRenderer_; }

private:
    void renderTextToTexture(std::string_view text, uint64_t font_handle, Color color,
                             float x, float y);

    struct FontInfo {
        std::string family;
        float size = 0.0f;
        int weight = 400;
        bool italic = false;
        void* hfont = nullptr; // HFONT handle for Win32 GDI text
    };

    SDL_Renderer* sdlRenderer_ = nullptr;
    std::unordered_map<uint64_t, FontInfo> fonts_;
    uint64_t nextFontHandle_ = 1;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
};

#endif // BRO_NO_SKIA

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer(platform::VulkanContext* vk,
                                          platform::Window* window = nullptr);

} // namespace bro::render
