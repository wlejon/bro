#include "render/recording_renderer.h"

#include <cassert>

namespace bro::render {

using bromath::Color;

RecordingRenderer::RecordingRenderer(CommandBuffer* buffer, Renderer* measureRenderer)
    : buffer_(buffer), measureRenderer_(measureRenderer) {}

void RecordingRenderer::clear(Color color) {
    if (!buffer_) { measureRenderer_->clear(color); return; }
    buffer_->append(Cmd_Clear{color});
}

void RecordingRenderer::drawRect(float x, float y, float w, float h, Color color) {
    if (!buffer_) { measureRenderer_->drawRect(x, y, w, h, color); return; }
    buffer_->append(Cmd_DrawRect{x, y, w, h, color});
}

void RecordingRenderer::drawRoundRect(float x, float y, float w, float h,
                                      float rx, float ry, Color color) {
    if (!buffer_) { measureRenderer_->drawRoundRect(x, y, w, h, rx, ry, color); return; }
    buffer_->append(Cmd_DrawRoundRect{x, y, w, h, rx, ry, color});
}

void RecordingRenderer::fillRect(float x, float y, float w, float h, Color color) {
    if (!buffer_) { measureRenderer_->fillRect(x, y, w, h, color); return; }
    buffer_->append(Cmd_FillRect{x, y, w, h, color});
}

void RecordingRenderer::fillRoundRect(float x, float y, float w, float h,
                                      float rx, float ry, Color color) {
    if (!buffer_) { measureRenderer_->fillRoundRect(x, y, w, h, rx, ry, color); return; }
    buffer_->append(Cmd_FillRoundRect{x, y, w, h, rx, ry, color});
}

void RecordingRenderer::fillRoundRectRadii(float x, float y, float w, float h,
                                           const Radii& r, Color color) {
    if (!buffer_) { measureRenderer_->fillRoundRectRadii(x, y, w, h, r, color); return; }
    buffer_->append(Cmd_FillRoundRectRadii{x, y, w, h, r, color});
}

void RecordingRenderer::drawRoundRectRadii(float x, float y, float w, float h,
                                           const Radii& r, float strokeWidth, Color color) {
    if (!buffer_) {
        measureRenderer_->drawRoundRectRadii(x, y, w, h, r, strokeWidth, color);
        return;
    }
    buffer_->append(Cmd_DrawRoundRectRadii{x, y, w, h, r, strokeWidth, color});
}

void RecordingRenderer::setClipRRect(float x, float y, float w, float h, const Radii& r) {
    if (!buffer_) { measureRenderer_->setClipRRect(x, y, w, h, r); return; }
    buffer_->append(Cmd_SetClipRRect{x, y, w, h, r});
}

void RecordingRenderer::drawBoxShadowRadii(float x, float y, float w, float h,
                                           const Radii& r,
                                           float offsetX, float offsetY,
                                           float blur, float spread,
                                           Color color, bool inset) {
    if (!buffer_) {
        measureRenderer_->drawBoxShadowRadii(x, y, w, h, r,
                                             offsetX, offsetY, blur, spread,
                                             color, inset);
        return;
    }
    buffer_->append(Cmd_DrawBoxShadowRadii{
        x, y, w, h, r, offsetX, offsetY, blur, spread, color, inset});
}

void RecordingRenderer::drawText(std::string_view text, float x, float y,
                                 FontRef font, Color color) {
    if (!buffer_) {
        measureRenderer_->drawText(text, x, y, font, color);
        return;
    }
    drawTextEx(text, x, y, font, color, 0.0f, 0.0f);
}

void RecordingRenderer::drawTextEx(std::string_view text, float x, float y,
                                   FontRef font, Color color,
                                   float letterSpacing, float blur) {
    if (!buffer_) {
        measureRenderer_->drawTextEx(text, x, y, font, color,
                                     letterSpacing, blur);
        return;
    }
    Cmd_DrawText cmd{};
    auto [tOff, tLen] = buffer_->pushString(text);
    cmd.textOffset = tOff;
    cmd.textLen = tLen;

    auto [fOff, fLen] = buffer_->pushString(font.family);
    cmd.familyOffset = fOff;
    cmd.familyLen = fLen;
    cmd.fontSize = font.size;
    cmd.fontWeight = font.weight;
    cmd.fontItalic = font.italic;

    cmd.x = x; cmd.y = y;
    cmd.color = color;
    cmd.letterSpacing = letterSpacing;
    cmd.blur = blur;
    buffer_->append(cmd);
}

TextMetrics RecordingRenderer::measureText(std::string_view text, FontRef font) {
    return measureRenderer_->measureText(text, font);
}

bool RecordingRenderer::registerCustomFont(const std::string& family,
                                           const void* data, size_t len,
                                           int weight, bool italic) {
    return measureRenderer_->registerCustomFont(family, data, len, weight, italic);
}

void RecordingRenderer::drawLine(float x1, float y1, float x2, float y2,
                                 Color color, float thickness) {
    if (!buffer_) { measureRenderer_->drawLine(x1, y1, x2, y2, color, thickness); return; }
    buffer_->append(Cmd_DrawLine{x1, y1, x2, y2, color, thickness});
}

void RecordingRenderer::drawImage(const void* data, size_t len,
                                  float x, float y, float w, float h) {
    if (!buffer_) { measureRenderer_->drawImage(data, len, x, y, w, h); return; }
    Cmd_DrawImage cmd{};
    cmd.dataOffset = buffer_->pushBytes(data, len);
    cmd.dataLen = static_cast<uint32_t>(len);
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    buffer_->append(cmd);
}

void RecordingRenderer::drawSvgMarkup(const char* data, size_t len,
                                      float x, float y, float w, float h) {
    if (!buffer_) { measureRenderer_->drawSvgMarkup(data, len, x, y, w, h); return; }
    Cmd_DrawSvgMarkup cmd{};
    cmd.dataOffset = buffer_->pushBytes(data, len);
    cmd.dataLen = static_cast<uint32_t>(len);
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    buffer_->append(cmd);
}

void RecordingRenderer::drawPixelsRGBA(const uint8_t* rgba,
                                       int srcW, int srcH, int stride,
                                       float x, float y, float w, float h) {
    if (!buffer_) {
        measureRenderer_->drawPixelsRGBA(rgba, srcW, srcH, stride, x, y, w, h);
        return;
    }
    Cmd_DrawPixelsRGBA cmd{};
    const size_t bytes = static_cast<size_t>(srcH) * static_cast<size_t>(stride);
    cmd.pixelsOffset = buffer_->pushBytes(rgba, bytes);
    cmd.srcW = srcW; cmd.srcH = srcH; cmd.stride = stride;
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    buffer_->append(cmd);
}

void RecordingRenderer::drawCircle(float cx, float cy, float r,
                                   Color fill, Color stroke, float strokeWidth) {
    if (!buffer_) {
        measureRenderer_->drawCircle(cx, cy, r, fill, stroke, strokeWidth);
        return;
    }
    buffer_->append(Cmd_DrawCircle{cx, cy, r, fill, stroke, strokeWidth});
}

void RecordingRenderer::drawEllipse(float cx, float cy, float rx, float ry,
                                    Color fill, Color stroke, float strokeWidth) {
    if (!buffer_) {
        measureRenderer_->drawEllipse(cx, cy, rx, ry, fill, stroke, strokeWidth);
        return;
    }
    buffer_->append(Cmd_DrawEllipse{cx, cy, rx, ry, fill, stroke, strokeWidth});
}

void RecordingRenderer::drawPath(std::string_view svgPathData,
                                 Color fill, Color stroke, float strokeWidth) {
    if (!buffer_) {
        measureRenderer_->drawPath(svgPathData, fill, stroke, strokeWidth);
        return;
    }
    Cmd_DrawPath cmd{};
    auto [off, len] = buffer_->pushString(svgPathData);
    cmd.pathOffset = off;
    cmd.pathLen = len;
    cmd.fill = fill; cmd.stroke = stroke; cmd.strokeWidth = strokeWidth;
    buffer_->append(cmd);
}

void RecordingRenderer::drawPolygon(std::span<const PointF> points,
                                    Color fill, Color stroke, float strokeWidth) {
    if (!buffer_) {
        measureRenderer_->drawPolygon(points, fill, stroke, strokeWidth);
        return;
    }
    Cmd_DrawPolygon cmd{};
    auto [off, count] = buffer_->pushSpan(points);
    cmd.pointsOffset = off;
    cmd.pointsLen = count;
    cmd.fill = fill; cmd.stroke = stroke; cmd.strokeWidth = strokeWidth;
    buffer_->append(cmd);
}

void RecordingRenderer::drawPolyline(std::span<const PointF> points,
                                     Color stroke, float strokeWidth) {
    if (!buffer_) {
        measureRenderer_->drawPolyline(points, stroke, strokeWidth);
        return;
    }
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
    if (!buffer_) {
        measureRenderer_->drawBoxShadow(x, y, w, h, rx, ry,
                                        offsetX, offsetY, blur, spread,
                                        color, inset);
        return;
    }
    buffer_->append(Cmd_DrawBoxShadow{
        x, y, w, h, rx, ry, offsetX, offsetY, blur, spread, color, inset});
}

void RecordingRenderer::save() {
    if (!buffer_) { measureRenderer_->save(); return; }
    buffer_->append(Cmd_Save{});
}
void RecordingRenderer::restore() {
    if (!buffer_) { measureRenderer_->restore(); return; }
    buffer_->append(Cmd_Restore{});
}
void RecordingRenderer::saveLayerAlpha(uint8_t alpha) {
    if (!buffer_) { measureRenderer_->saveLayerAlpha(alpha); return; }
    buffer_->append(Cmd_SaveLayerAlpha{alpha});
}
void RecordingRenderer::translate(float dx, float dy) {
    if (!buffer_) { measureRenderer_->translate(dx, dy); return; }
    buffer_->append(Cmd_Translate{dx, dy});
}
void RecordingRenderer::scale(float sx, float sy) {
    if (!buffer_) { measureRenderer_->scale(sx, sy); return; }
    buffer_->append(Cmd_Scale{sx, sy});
}
void RecordingRenderer::rotate(float degrees) {
    if (!buffer_) { measureRenderer_->rotate(degrees); return; }
    buffer_->append(Cmd_Rotate{degrees});
}
void RecordingRenderer::concat(float a, float b, float c, float d, float e, float f) {
    if (!buffer_) { measureRenderer_->concat(a, b, c, d, e, f); return; }
    buffer_->append(Cmd_Concat{a, b, c, d, e, f});
}
void RecordingRenderer::concat4x4(const float m[16]) {
    if (!buffer_) { measureRenderer_->concat4x4(m); return; }
    Cmd_Concat4x4 cmd{};
    for (int i = 0; i < 16; ++i) cmd.m[i] = m[i];
    buffer_->append(cmd);
}
void RecordingRenderer::saveLayerWithFilter(std::span<const CssFilterParams> filters,
                                            float x, float y, float w, float h) {
    if (!buffer_) { measureRenderer_->saveLayerWithFilter(filters, x, y, w, h); return; }
    Cmd_SaveLayerWithFilter cmd{};
    auto [off, count] = buffer_->pushSpan(filters);
    cmd.filtersOffset = off;
    cmd.filtersLen = count;
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    buffer_->append(cmd);
}
void RecordingRenderer::setClip(float x, float y, float w, float h) {
    if (!buffer_) { measureRenderer_->setClip(x, y, w, h); return; }
    buffer_->append(Cmd_SetClip{x, y, w, h});
}
void RecordingRenderer::resetClip() {
    if (!buffer_) { measureRenderer_->resetClip(); return; }
    buffer_->append(Cmd_ResetClip{});
}
void RecordingRenderer::setClipPolygon(std::span<const PointF> points) {
    if (!buffer_) { measureRenderer_->setClipPolygon(points); return; }
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
    if (!buffer_) {
        measureRenderer_->fillLinearGradient(x, y, w, h, startX, startY,
                                             endX, endY, stops);
        return;
    }
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
    if (!buffer_) {
        measureRenderer_->fillRadialGradient(x, y, w, h, cx, cy, rx, ry, stops);
        return;
    }
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
    if (!buffer_) {
        measureRenderer_->fillConicGradient(x, y, w, h, cx, cy, angleDeg, stops);
        return;
    }
    Cmd_FillConicGradient cmd{};
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    cmd.cx = cx; cmd.cy = cy; cmd.angleDeg = angleDeg;
    auto [off, count] = buffer_->pushSpan(stops);
    cmd.stopsOffset = off;
    cmd.stopsLen = count;
    buffer_->append(cmd);
}

void RecordingRenderer::beginFrame(int width, int height) {
    if (!buffer_) { measureRenderer_->beginFrame(width, height); return; }
    buffer_->append(Cmd_BeginFrame{width, height});
}
void RecordingRenderer::endFrame() {
    if (!buffer_) { measureRenderer_->endFrame(); return; }
    buffer_->append(Cmd_EndFrame{});
}

void RecordingRenderer::recordLayerBreak(int kind, void* canvasScene,
                                         unsigned int directTexture,
                                         float x, float y, float w, float h) {
    buffer_->append(Cmd_LayerBreak{kind, canvasScene, directTexture, x, y, w, h});
}

void RecordingRenderer::recordBlitCanvasInline(void* canvasScene,
                                               float x, float y, float w, float h) {
    buffer_->append(Cmd_BlitCanvasInline{canvasScene, x, y, w, h});
}

} // namespace bro::render
