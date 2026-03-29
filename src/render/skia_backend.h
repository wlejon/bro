#pragma once

#include "render/renderer.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#include <include/core/SkFont.h>
#include <include/core/SkTypeface.h>
#include <include/core/SkFontMgr.h>

struct SDL_GPUTexture;
struct SDL_GPUCommandBuffer;

namespace bro::render {

class GPUContext;

// ---------------------------------------------------------------------------
// SkiaRenderer -- Skia raster UI + SDL_GPU display
//
// The UI (HTML/CSS) is rendered to a CPU-side Skia surface with transparency,
// uploaded to an SDL_GPU texture, and composited over GPU-rendered scene
// content via the texture pipeline.
// ---------------------------------------------------------------------------

class SkiaRenderer final : public Renderer {
public:
    explicit SkiaRenderer(GPUContext& gpu);
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
    void beginFrame(int width, int height) override;
    void endFrame() override;

    /// Upload Skia pixels to the GPU texture. Call after endFrame(),
    /// within an active command buffer (before any render pass).
    void uploadToGPU(SDL_GPUCommandBuffer* cmd);

    /// Access the UI overlay GPU texture (BGRA8, premultiplied alpha).
    SDL_GPUTexture* getUITexture() const { return uiTexture_; }

    /// Render text to a GPU texture (for scene-layer text).
    /// Caller does NOT own the texture — it is cached internally.
    SDL_GPUTexture* renderTextToTexture(SDL_GPUCommandBuffer* cmd,
                                         std::string_view text, uint64_t font_handle,
                                         Color color, int& outW, int& outH);

    GPUContext* gpu() const { return gpu_; }

private:
    SkColor toSkColor(Color c) const;

    GPUContext* gpu_ = nullptr;
    SDL_GPUTexture* uiTexture_ = nullptr;
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

    // Cached text textures for scene-layer rendering
    struct TextCacheEntry { SDL_GPUTexture* tex; int w; int h; };
    std::unordered_map<std::string, TextCacheEntry> textTexCache_;

    // Pending pixel data for upload
    bool pixelsPending_ = false;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer(GPUContext* gpu);

} // namespace bro::render
