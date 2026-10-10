#pragma once

// CaptureRenderer: a render::Renderer that draws nothing and records the
// calls the terminal painter makes (fills, strokes, text, lines, paths), with
// fixed font metrics so the oracle knows exactly where every cell is.

#include "render/renderer.h"

#include <string>
#include <vector>

namespace bro::terminal::test {

struct Op {
    enum Kind { Fill, Stroke, Text, Line, Path, Save, Restore, Clip, Image } kind = Fill;
    // Image: the destination is (x, y, w, h); the source rect and the pixels.
    float sx = 0, sy = 0, sw = 0, sh = 0;
    uint64_t pixelsId = 0;
    const uint8_t* pixels = nullptr;
    int pixelsW = 0, pixelsH = 0;
    float x = 0, y = 0, w = 0, h = 0;   // Fill/Stroke/Clip rect; Text origin (x, baseline y)
    float x2 = 0, y2 = 0;               // Line end
    float thickness = 0;                // Line / Path stroke width
    bromath::Color color{};
    std::string text;                   // Text: UTF-8; Path: SVG path data
    std::string family;
    float size = 0;
    int weight = 400;
    bool italic = false;
};

class CaptureRenderer final : public render::Renderer {
public:
    // measureText: `advance` per code point, these line metrics.
    float advance = 8.0f;
    float ascent = 12.0f;
    float descent = 4.0f;
    float xHeight = 7.0f;

    std::vector<Op> ops;

    void clear(bromath::Color) override {}
    void drawRect(float x, float y, float w, float h, bromath::Color c) override;
    void drawRoundRect(float x, float y, float w, float h, float, float, bromath::Color c) override;
    void fillRect(float x, float y, float w, float h, bromath::Color c) override;
    void fillRoundRect(float x, float y, float w, float h, float, float, bromath::Color c) override;
    void drawText(std::string_view text, float x, float y, render::FontRef font, bromath::Color color,
                  render::TextDirection direction = render::TextDirection::LTR) override;
    render::TextMetrics measureText(std::string_view text, render::FontRef font,
                                    render::TextDirection direction = render::TextDirection::LTR) override;
    void drawLine(float x1, float y1, float x2, float y2, bromath::Color color, float thickness) override;
    void drawImage(const void*, size_t, float, float, float, float, uint64_t = 0) override {}
    void drawSharedPixels(const render::SharedPixels& px, float sx, float sy, float sw, float sh, float x, float y,
                          float w, float h,
                          render::ImageSampling = render::ImageSampling::Smooth) override {
        Op op;
        op.kind = Op::Image;
        op.x = x, op.y = y, op.w = w, op.h = h;
        op.sx = sx, op.sy = sy, op.sw = sw, op.sh = sh;
        op.pixelsId = px.id;
        op.pixels = px.rgba;
        op.pixelsW = px.width;
        op.pixelsH = px.height;
        ops.push_back(std::move(op));
    }
    void drawCircle(float, float, float, bromath::Color, bromath::Color, float) override {}
    void drawEllipse(float, float, float, float, bromath::Color, bromath::Color, float) override {}
    void drawPath(std::string_view d, bromath::Color fill, bromath::Color stroke, float strokeWidth) override;
    void drawPolygon(std::span<const render::PointF>, bromath::Color, bromath::Color, float) override {}
    void drawPolyline(std::span<const render::PointF>, bromath::Color, float) override {}
    void drawBoxShadow(float, float, float, float, float, float, float, float, float, float, bromath::Color,
                       bool) override {}
    void save() override;
    void restore() override;
    void saveLayerAlpha(uint8_t) override { save(); }
    void translate(float, float) override {}
    void scale(float, float) override {}
    void rotate(float) override {}
    void concat(float, float, float, float, float, float) override {}
    void concat4x4(const float*) override {}
    void saveLayerWithFilter(std::span<const render::CssFilterParams>, float, float, float, float) override {
        save();
    }
    void setClip(float x, float y, float w, float h) override;
    void resetClip() override {}
    void fillLinearGradient(float, float, float, float, float, float, float, float,
                            std::span<const render::ColorStop>) override {}
    void fillRadialGradient(float, float, float, float, float, float, float, float,
                            std::span<const render::ColorStop>) override {}
    void fillConicGradient(float, float, float, float, float, float, float,
                           std::span<const render::ColorStop>) override {}
    void beginFrame(int, int) override {}
    void endFrame() override {}
};

} // namespace bro::terminal::test
