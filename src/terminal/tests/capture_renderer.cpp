#include "capture_renderer.h"

namespace bro::terminal::test {

namespace {

size_t codepoints(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

} // namespace

void CaptureRenderer::drawRect(float x, float y, float w, float h, bromath::Color c) {
    Op op;
    op.kind = Op::Stroke;
    op.x = x, op.y = y, op.w = w, op.h = h, op.color = c;
    ops.push_back(std::move(op));
}

void CaptureRenderer::drawRoundRect(float x, float y, float w, float h, float, float, bromath::Color c) {
    drawRect(x, y, w, h, c);
}

void CaptureRenderer::fillRect(float x, float y, float w, float h, bromath::Color c) {
    Op op;
    op.kind = Op::Fill;
    op.x = x, op.y = y, op.w = w, op.h = h, op.color = c;
    ops.push_back(std::move(op));
}

void CaptureRenderer::fillRoundRect(float x, float y, float w, float h, float, float, bromath::Color c) {
    fillRect(x, y, w, h, c);
}

void CaptureRenderer::drawText(std::string_view text, float x, float y, render::FontRef font,
                               bromath::Color color, render::TextDirection) {
    Op op;
    op.kind = Op::Text;
    op.x = x, op.y = y, op.color = color;
    op.text = std::string(text);
    op.family = std::string(font.family);
    op.size = font.size;
    op.weight = font.weight;
    op.italic = font.italic;
    ops.push_back(std::move(op));
}

render::TextMetrics CaptureRenderer::measureText(std::string_view text, render::FontRef, render::TextDirection) {
    render::TextMetrics m;
    m.width = advance * float(codepoints(text));
    m.ascent = ascent;
    m.descent = descent;
    m.height = ascent + descent;
    m.xHeight = xHeight;
    return m;
}

void CaptureRenderer::drawLine(float x1, float y1, float x2, float y2, bromath::Color color, float thickness) {
    Op op;
    op.kind = Op::Line;
    op.x = x1, op.y = y1, op.x2 = x2, op.y2 = y2, op.color = color, op.thickness = thickness;
    ops.push_back(std::move(op));
}

void CaptureRenderer::drawPath(std::string_view d, bromath::Color, bromath::Color stroke, float strokeWidth) {
    Op op;
    op.kind = Op::Path;
    op.text = std::string(d);
    op.color = stroke;
    op.thickness = strokeWidth;
    ops.push_back(std::move(op));
}

void CaptureRenderer::save() {
    Op op;
    op.kind = Op::Save;
    ops.push_back(std::move(op));
}

void CaptureRenderer::restore() {
    Op op;
    op.kind = Op::Restore;
    ops.push_back(std::move(op));
}

void CaptureRenderer::setClip(float x, float y, float w, float h) {
    Op op;
    op.kind = Op::Clip;
    op.x = x, op.y = y, op.w = w, op.h = h;
    ops.push_back(std::move(op));
}

} // namespace bro::terminal::test
