#pragma once

#include "render/command_buffer.h"
#include "render/renderer.h"

#include <string>
#include <vector>

namespace bro::render {

// Implements `render::Renderer` with two modes:
//   1. Recording mode (buffer != null): every mutating call appends a
//      `DrawCommand` to the bound `CommandBuffer`. Variable-length payloads
//      (text, gradient stops, polygons, images, filter chains) are copied
//      into the buffer's arena so it is self-contained.
//   2. Passthrough mode (buffer == null): every call forwards to
//      `measureRenderer` directly. Used by the headless CPU path which
//      shares one DrawTraversal but sometimes paints straight to a Skia
//      surface without going through record/replay.
//
// Query calls (measureText, registerCustomFont) always delegate to
// `measureRenderer` — DrawTraversal needs real font metrics to lay out text.
//
// Each Cmd_DrawText embeds the full font descriptor (family/size/weight/
// italic). The replayer re-resolves it against its own renderer's font cache,
// so font state never has to cross thread boundaries.
//
// Thread-safe to record on one thread and replay on another, provided the
// caller does not mutate the buffer during replay (typical pattern: ping-pong
// two buffers, swap pointers under an atomic publish).
class RecordingRenderer final : public Renderer {
public:
    // `buffer` receives recorded commands (variable-length payloads copied
    // into its arena). `measureRenderer` services synchronous queries and
    // owns the real font handles — must outlive this RecordingRenderer.
    RecordingRenderer(CommandBuffer* buffer, Renderer* measureRenderer);

    // Reset the binding to a different buffer (e.g. when the engine swaps
    // back/front buffers between frames).
    void setBuffer(CommandBuffer* buffer) { buffer_ = buffer; }
    CommandBuffer* buffer() const { return buffer_; }

    // ---- Renderer overrides ----
    void clear(bromath::Color color) override;

    void drawRect(float x, float y, float w, float h, bromath::Color color) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry, bromath::Color color) override;
    void fillRect(float x, float y, float w, float h, bromath::Color color) override;
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry, bromath::Color color) override;

    void fillRoundRectRadii(float x, float y, float w, float h,
                            const Radii& r, bromath::Color color) override;
    void drawRoundRectRadii(float x, float y, float w, float h,
                            const Radii& r, float strokeWidth, bromath::Color color) override;
    void setClipRRect(float x, float y, float w, float h, const Radii& r) override;
    void drawBoxShadowRadii(float x, float y, float w, float h,
                            const Radii& r,
                            float offsetX, float offsetY,
                            float blur, float spread,
                            bromath::Color color, bool inset) override;

    void drawText(std::string_view text, float x, float y,
                  FontRef font, bromath::Color color) override;
    TextMetrics measureText(std::string_view text, FontRef font) override;
    void drawTextEx(std::string_view text, float x, float y,
                    FontRef font, bromath::Color color,
                    float letterSpacing, float blur) override;

    bool registerCustomFont(const std::string& family,
                            const void* data, size_t len,
                            int weight, bool italic) override;

    void drawLine(float x1, float y1, float x2, float y2, bromath::Color color, float thickness) override;
    void drawImage(const void* data, size_t len, float x, float y, float w, float h,
                   uint64_t imageId = 0) override;
    void drawPixelsRGBA(const uint8_t* rgba,
                        int srcW, int srcH, int stride,
                        float x, float y, float w, float h) override;
    void drawSvgMarkup(const char* data, size_t len,
                       float x, float y, float w, float h) override;

    void drawCircle(float cx, float cy, float r,
                    bromath::Color fill, bromath::Color stroke, float strokeWidth) override;
    void drawEllipse(float cx, float cy, float rx, float ry,
                     bromath::Color fill, bromath::Color stroke, float strokeWidth) override;
    void drawPath(std::string_view svgPathData,
                  bromath::Color fill, bromath::Color stroke, float strokeWidth) override;
    void drawPolygon(std::span<const PointF> points,
                     bromath::Color fill, bromath::Color stroke, float strokeWidth) override;
    void drawPolyline(std::span<const PointF> points,
                      bromath::Color stroke, float strokeWidth) override;

    void drawBoxShadow(float x, float y, float w, float h,
                       float rx, float ry,
                       float offsetX, float offsetY,
                       float blur, float spread,
                       bromath::Color color, bool inset) override;

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
    void saveLayerWithBlend(BlendMode mode) override;
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
                          float x, float y, float w, float h,
                          float clipX = 0, float clipY = 0,
                          float clipW = -1, float clipH = -1);

    // Inline canvas blit — system panels composite their canvas scene onto
    // the current surface instead of breaking into a separate layer.
    void recordBlitCanvasInline(void* canvasScene, float x, float y, float w, float h);

private:
    CommandBuffer* buffer_;
    Renderer*      measureRenderer_;
};

} // namespace bro::render
