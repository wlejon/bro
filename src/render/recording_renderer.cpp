#include "render/recording_renderer.h"

#include <cassert>

namespace bro::render {

RecordingRenderer::RecordingRenderer(CommandBuffer* buffer) : buffer_(buffer) {}

void RecordingRenderer::clear(Color color) {
    buffer_->append(Cmd_Clear{color});
}

void RecordingRenderer::drawRect(float x, float y, float w, float h, Color color) {
    buffer_->append(Cmd_DrawRect{x, y, w, h, color});
}

void RecordingRenderer::drawRoundRect(float x, float y, float w, float h,
                                      float rx, float ry, Color color) {
    buffer_->append(Cmd_DrawRoundRect{x, y, w, h, rx, ry, color});
}

void RecordingRenderer::fillRect(float x, float y, float w, float h, Color color) {
    buffer_->append(Cmd_FillRect{x, y, w, h, color});
}

void RecordingRenderer::fillRoundRect(float x, float y, float w, float h,
                                      float rx, float ry, Color color) {
    buffer_->append(Cmd_FillRoundRect{x, y, w, h, rx, ry, color});
}

void RecordingRenderer::fillRoundRectRadii(float x, float y, float w, float h,
                                           const Radii& r, Color color) {
    buffer_->append(Cmd_FillRoundRectRadii{x, y, w, h, r, color});
}

void RecordingRenderer::drawRoundRectRadii(float x, float y, float w, float h,
                                           const Radii& r, float strokeWidth, Color color) {
    buffer_->append(Cmd_DrawRoundRectRadii{x, y, w, h, r, strokeWidth, color});
}

void RecordingRenderer::setClipRRect(float x, float y, float w, float h, const Radii& r) {
    buffer_->append(Cmd_SetClipRRect{x, y, w, h, r});
}

void RecordingRenderer::drawBoxShadowRadii(float x, float y, float w, float h,
                                           const Radii& r,
                                           float offsetX, float offsetY,
                                           float blur, float spread,
                                           Color color, bool inset) {
    buffer_->append(Cmd_DrawBoxShadowRadii{
        x, y, w, h, r, offsetX, offsetY, blur, spread, color, inset});
}

void RecordingRenderer::drawText(std::string_view text, float x, float y,
                                 uint64_t font_handle, Color color) {
    drawTextEx(text, x, y, font_handle, color, 0.0f, 0.0f);
}

void RecordingRenderer::drawTextEx(std::string_view text, float x, float y,
                                   uint64_t font_handle, Color color,
                                   float letterSpacing, float blur) {
    Cmd_DrawText cmd{};
    auto [tOff, tLen] = buffer_->pushString(text);
    cmd.textOffset = tOff;
    cmd.textLen = tLen;

    // Resolve handle → descriptor. createFont assigns 1-based ids.
    if (font_handle >= 1 && font_handle <= fonts_.size()) {
        const auto& fd = fonts_[font_handle - 1];
        auto [fOff, fLen] = buffer_->pushString(fd.family);
        cmd.familyOffset = fOff;
        cmd.familyLen = fLen;
        cmd.fontSize = fd.size;
        cmd.fontWeight = fd.weight;
        cmd.fontItalic = fd.italic;
    } else {
        // Unknown handle — record an empty family; replayer falls back to
        // platform default. Still better than dropping the text.
        cmd.familyOffset = 0;
        cmd.familyLen = 0;
        cmd.fontSize = 14.0f;
        cmd.fontWeight = 400;
        cmd.fontItalic = false;
    }

    cmd.x = x; cmd.y = y;
    cmd.color = color;
    cmd.letterSpacing = letterSpacing;
    cmd.blur = blur;
    buffer_->append(cmd);
}

TextMetrics RecordingRenderer::measureText(std::string_view, uint64_t) {
    // DrawTraversal measures via FontManager metrics, not via renderer. If a
    // future caller routes through here, we have no surface to measure on —
    // return zeros and let the caller fall back. (Asserting would break the
    // headless harness during early bring-up.)
    return TextMetrics{};
}

uint64_t RecordingRenderer::createFont(std::string_view family, float size,
                                       int weight, bool italic) {
    // Cheap dedup: linear scan. Font count per app is small (< 50).
    for (size_t i = 0; i < fonts_.size(); ++i) {
        const auto& fd = fonts_[i];
        if (fd.size == size && fd.weight == weight && fd.italic == italic
            && fd.family == family) {
            return static_cast<uint64_t>(i + 1);
        }
    }
    fonts_.push_back(FontDesc{std::string(family), size, weight, italic});
    return nextFontHandle_++;
}

void RecordingRenderer::deleteFont(uint64_t /*font_handle*/) {
    // No-op. Records persist for the frame; the descriptor table is reset
    // out-of-band when the engine resets the recorder.
}

bool RecordingRenderer::registerCustomFont(const std::string& /*family*/,
                                           const void* /*data*/, size_t /*len*/,
                                           int /*weight*/, bool /*italic*/) {
    // Custom fonts are registered against the live (replay-side) renderer
    // through a separate path during app load — not via the recorder.
    return false;
}

void RecordingRenderer::drawLine(float x1, float y1, float x2, float y2,
                                 Color color, float thickness) {
    buffer_->append(Cmd_DrawLine{x1, y1, x2, y2, color, thickness});
}

void RecordingRenderer::drawImage(const void* data, size_t len,
                                  float x, float y, float w, float h) {
    Cmd_DrawImage cmd{};
    cmd.dataOffset = buffer_->pushBytes(data, len);
    cmd.dataLen = static_cast<uint32_t>(len);
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    buffer_->append(cmd);
}

void RecordingRenderer::drawPixelsRGBA(const uint8_t* rgba,
                                       int srcW, int srcH, int stride,
                                       float x, float y, float w, float h) {
    Cmd_DrawPixelsRGBA cmd{};
    const size_t bytes = static_cast<size_t>(srcH) * static_cast<size_t>(stride);
    cmd.pixelsOffset = buffer_->pushBytes(rgba, bytes);
    cmd.srcW = srcW; cmd.srcH = srcH; cmd.stride = stride;
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    buffer_->append(cmd);
}

void RecordingRenderer::drawCircle(float cx, float cy, float r,
                                   Color fill, Color stroke, float strokeWidth) {
    buffer_->append(Cmd_DrawCircle{cx, cy, r, fill, stroke, strokeWidth});
}

void RecordingRenderer::drawEllipse(float cx, float cy, float rx, float ry,
                                    Color fill, Color stroke, float strokeWidth) {
    buffer_->append(Cmd_DrawEllipse{cx, cy, rx, ry, fill, stroke, strokeWidth});
}

void RecordingRenderer::drawPath(std::string_view svgPathData,
                                 Color fill, Color stroke, float strokeWidth) {
    Cmd_DrawPath cmd{};
    auto [off, len] = buffer_->pushString(svgPathData);
    cmd.pathOffset = off;
    cmd.pathLen = len;
    cmd.fill = fill; cmd.stroke = stroke; cmd.strokeWidth = strokeWidth;
    buffer_->append(cmd);
}

void RecordingRenderer::drawPolygon(std::span<const PointF> points,
                                    Color fill, Color stroke, float strokeWidth) {
    Cmd_DrawPolygon cmd{};
    auto [off, count] = buffer_->pushSpan(points);
    cmd.pointsOffset = off;
    cmd.pointsLen = count;
    cmd.fill = fill; cmd.stroke = stroke; cmd.strokeWidth = strokeWidth;
    buffer_->append(cmd);
}

void RecordingRenderer::drawPolyline(std::span<const PointF> points,
                                     Color stroke, float strokeWidth) {
    Cmd_DrawPolyline cmd{};
    auto [off, count] = buffer_->pushSpan(points);
    cmd.pointsOffset = off;
    cmd.pointsLen = count;
    cmd.stroke = stroke; cmd.strokeWidth = strokeWidth;
    buffer_->append(cmd);
}

void RecordingRenderer::drawBoxShadow(float x, float y, float w, float h,
                                      float rx, float ry,
                                      float offsetX, float offsetY,
                                      float blur, float spread,
                                      Color color, bool inset) {
    buffer_->append(Cmd_DrawBoxShadow{
        x, y, w, h, rx, ry, offsetX, offsetY, blur, spread, color, inset});
}

void RecordingRenderer::save()      { buffer_->append(Cmd_Save{}); }
void RecordingRenderer::restore()   { buffer_->append(Cmd_Restore{}); }
void RecordingRenderer::saveLayerAlpha(uint8_t alpha) {
    buffer_->append(Cmd_SaveLayerAlpha{alpha});
}
void RecordingRenderer::translate(float dx, float dy) {
    buffer_->append(Cmd_Translate{dx, dy});
}
void RecordingRenderer::scale(float sx, float sy) {
    buffer_->append(Cmd_Scale{sx, sy});
}
void RecordingRenderer::rotate(float degrees) {
    buffer_->append(Cmd_Rotate{degrees});
}
void RecordingRenderer::concat(float a, float b, float c, float d, float e, float f) {
    buffer_->append(Cmd_Concat{a, b, c, d, e, f});
}
void RecordingRenderer::concat4x4(const float m[16]) {
    Cmd_Concat4x4 cmd{};
    for (int i = 0; i < 16; ++i) cmd.m[i] = m[i];
    buffer_->append(cmd);
}
void RecordingRenderer::saveLayerWithFilter(std::span<const CssFilterParams> filters,
                                            float x, float y, float w, float h) {
    Cmd_SaveLayerWithFilter cmd{};
    auto [off, count] = buffer_->pushSpan(filters);
    cmd.filtersOffset = off;
    cmd.filtersLen = count;
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    buffer_->append(cmd);
}
void RecordingRenderer::setClip(float x, float y, float w, float h) {
    buffer_->append(Cmd_SetClip{x, y, w, h});
}
void RecordingRenderer::resetClip() {
    buffer_->append(Cmd_ResetClip{});
}
void RecordingRenderer::setClipPolygon(std::span<const PointF> points) {
    Cmd_SetClipPolygon cmd{};
    auto [off, count] = buffer_->pushSpan(points);
    cmd.pointsOffset = off;
    cmd.pointsLen = count;
    buffer_->append(cmd);
}

void RecordingRenderer::fillLinearGradient(float x, float y, float w, float h,
                                           float startX, float startY,
                                           float endX, float endY,
                                           std::span<const ColorStop> stops) {
    Cmd_FillLinearGradient cmd{};
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    cmd.startX = startX; cmd.startY = startY; cmd.endX = endX; cmd.endY = endY;
    auto [off, count] = buffer_->pushSpan(stops);
    cmd.stopsOffset = off;
    cmd.stopsLen = count;
    buffer_->append(cmd);
}

void RecordingRenderer::fillRadialGradient(float x, float y, float w, float h,
                                           float cx, float cy, float rx, float ry,
                                           std::span<const ColorStop> stops) {
    Cmd_FillRadialGradient cmd{};
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    cmd.cx = cx; cmd.cy = cy; cmd.rx = rx; cmd.ry = ry;
    auto [off, count] = buffer_->pushSpan(stops);
    cmd.stopsOffset = off;
    cmd.stopsLen = count;
    buffer_->append(cmd);
}

void RecordingRenderer::fillConicGradient(float x, float y, float w, float h,
                                          float cx, float cy, float angleDeg,
                                          std::span<const ColorStop> stops) {
    Cmd_FillConicGradient cmd{};
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    cmd.cx = cx; cmd.cy = cy; cmd.angleDeg = angleDeg;
    auto [off, count] = buffer_->pushSpan(stops);
    cmd.stopsOffset = off;
    cmd.stopsLen = count;
    buffer_->append(cmd);
}

void RecordingRenderer::beginFrame(int width, int height) {
    buffer_->append(Cmd_BeginFrame{width, height});
}
void RecordingRenderer::endFrame() {
    buffer_->append(Cmd_EndFrame{});
}

void RecordingRenderer::recordLayerBreak(int kind, void* canvasScene,
                                         unsigned int directTexture,
                                         float x, float y, float w, float h) {
    buffer_->append(Cmd_LayerBreak{kind, canvasScene, directTexture, x, y, w, h});
}

} // namespace bro::render
