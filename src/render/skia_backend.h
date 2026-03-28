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
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/vk/GrVkDirectContext.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/vk/VulkanBackendContext.h>
#include <include/gpu/vk/VulkanExtensions.h>
#endif

struct SDL_Renderer;
struct SDL_Window;
struct SDL_Texture;

namespace bro::platform {
    class VulkanContext;
    class Window;
} // namespace bro::platform

namespace bro::render {

#ifdef BRO_NO_SKIA

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

#else // Skia is available

// ---------------------------------------------------------------------------
// SkiaRenderer -- real GPU-accelerated renderer via Skia + Vulkan
// ---------------------------------------------------------------------------
class SkiaRenderer final : public Renderer {
public:
    explicit SkiaRenderer(platform::VulkanContext& vk);
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

    void beginFrame(int width, int height) override;
    void endFrame() override;

private:
    SkColor toSkColor(Color c) const;

    platform::VulkanContext& vk_;
    skgpu::VulkanExtensions vk_extensions_;
    VkPhysicalDeviceFeatures device_features_{};
    sk_sp<GrDirectContext> gr_context_;
    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr; // owned by surface_

    struct FontEntry {
        sk_sp<SkTypeface> typeface;
        std::unique_ptr<SkFont> font;
    };
    std::unordered_map<uint64_t, FontEntry> fonts_;
    uint64_t next_font_handle_ = 1;
};

#endif // BRO_NO_SKIA

// ---------------------------------------------------------------------------
// Factory -- returns the appropriate renderer for the current build.
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer(platform::VulkanContext* vk,
                                          platform::Window* window = nullptr);

} // namespace bro::render
