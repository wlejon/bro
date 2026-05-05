#pragma once

#include "render/command_buffer.h"
#include "render/renderer.h"

#include <string>
#include <vector>

namespace bro::render {

// Implements `render::Renderer` by appending a `DrawCommand` for each call
// into a target `CommandBuffer`. Variable-length payloads (text, gradient
// stops, polygons, images, filter chains) are copied into the buffer's arena
// so the buffer is self-contained — the caller can drop the original data
// after the call returns.
//
// Thread-safe to record on one thread and replay on another, provided the
// caller does not mutate the buffer during replay (typical pattern: ping-pong
// two buffers, swap pointers under an atomic publish).
//
// Implements text and font as descriptors (family/size/weight/italic) rather
// than handles, because font handles are per-FontManager and FontManager is
// per-thread. The replayer resolves descriptors against its renderer's
// FontManager.
//
// Returns dummy values from synchronous query methods (measureText, getCanvas,
// surface, capturePixels) — DrawTraversal does not call those during the paint
// walk. createFont/deleteFont also no-op, since text commands carry the full
// font descriptor.
class RecordingRenderer final : public Renderer {
public:
    explicit RecordingRenderer(CommandBuffer* buffer);

    // Reset the binding to a different buffer (e.g. when the engine swaps
    // back/front buffers between frames).
    void setBuffer(CommandBuffer* buffer) { buffer_ = buffer; }
    CommandBuffer* buffer() const { return buffer_; }

    // ---- Renderer overrides ----
    void clear(Color color) override;

    void drawRect(float x, float y, float w, float h, Color color) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) override;
    void fillRect(float x, float y, float w, float h, Color color) override;
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) override;

    void fillRoundRectRadii(float x, float y, float w, float h,
                            const Radii& r, Color color) override;
    void drawRoundRectRadii(float x, float y, float w, float h,
                            const Radii& r, float strokeWidth, Color color) override;
    void setClipRRect(float x, float y, float w, float h, const Radii& r) override;
    void drawBoxShadowRadii(float x, float y, float w, float h,
                            const Radii& r,
                            float offsetX, float offsetY,
                            float blur, float spread,
                            Color color, bool inset) override;

    void drawText(std::string_view text, float x, float y,
                  uint64_t font_handle, Color color) override;
    TextMetrics measureText(std::string_view text, uint64_t font_handle) override;
    void drawTextEx(std::string_view text, float x, float y,
                    uint64_t font_handle, Color color,
                    float letterSpacing, float blur) override;

    uint64_t createFont(std::string_view family, float size, int weight, bool italic) override;
    void deleteFont(uint64_t font_handle) override;
    bool registerCustomFont(const std::string& family,
                            const void* data, size_t len,
                            int weight, bool italic) override;

    void drawLine(float x1, float y1, float x2, float y2, Color color, float thickness) override;
    void drawImage(const void* data, size_t len, float x, float y, float w, float h) override;
    void drawPixelsRGBA(const uint8_t* rgba,
                        int srcW, int srcH, int stride,
                        float x, float y, float w, float h) override;

    void drawCircle(float cx, float cy, float r,
                    Color fill, Color stroke, float strokeWidth) override;
    void drawEllipse(float cx, float cy, float rx, float ry,
                     Color fill, Color stroke, float strokeWidth) override;
    void drawPath(std::string_view svgPathData,
                  Color fill, Color stroke, float strokeWidth) override;
    void drawPolygon(std::span<const PointF> points,
                     Color fill, Color stroke, float strokeWidth) override;
    void drawPolyline(std::span<const PointF> points,
                      Color stroke, float strokeWidth) override;

    void drawBoxShadow(float x, float y, float w, float h,
                       float rx, float ry,
                       float offsetX, float offsetY,
                       float blur, float spread,
                       Color color, bool inset) override;

    void save() override;
    void restore() override;
    void saveLayerAlpha(uint8_t alpha) override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void rotate(float degrees) override;
    void concat(float a, float b, float c, float d, float e, float f) override;
    void concat4x4(const float m[16]) override;
    void saveLayerWithFilter(std::span<const CssFilterParams> filters,
                             float x, float y, float w, float h) override;
    void setClip(float x, float y, float w, float h) override;
    void resetClip() override;
    void setClipPolygon(std::span<const PointF> points) override;

    void fillLinearGradient(float x, float y, float w, float h,
                            float startX, float startY, float endX, float endY,
                            std::span<const ColorStop> stops) override;
    void fillRadialGradient(float x, float y, float w, float h,
                            float cx, float cy, float rx, float ry,
                            std::span<const ColorStop> stops) override;
    void fillConicGradient(float x, float y, float w, float h,
                           float cx, float cy, float angleDeg,
                           std::span<const ColorStop> stops) override;

    void beginFrame(int width, int height) override;
    void endFrame() override;

    // Layer break — emitted by DrawTraversal's layer-break callback when it
    // crosses a canvas/WebGL boundary. Not on the Renderer interface; the
    // engine wires the callback to call this directly.
    void recordLayerBreak(int kind, void* canvasScene, unsigned int directTexture,
                          float x, float y, float w, float h);

private:
    // FontManager calls createFont() to register fonts and gets back synthetic
    // handles (1, 2, 3, ...). Later, drawText() receives those handles and
    // must record the full descriptor in the command so the replay-side
    // renderer can resolve it through its own FontManager.
    struct FontDesc {
        std::string family;
        float size;
        int   weight;
        bool  italic;
    };

    CommandBuffer* buffer_;
    std::vector<FontDesc> fonts_;  // index = handle - 1
    uint64_t nextFontHandle_ = 1;
};

} // namespace bro::render
