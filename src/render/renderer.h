#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace bro::render {

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

struct TextMetrics {
    float width = 0.0f;
    float height = 0.0f;
};

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual void clear(Color color) = 0;

    virtual void drawRect(float x, float y, float w, float h, Color color) = 0;
    virtual void drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) = 0;
    virtual void fillRect(float x, float y, float w, float h, Color color) = 0;

    virtual void drawText(std::string_view text, float x, float y, uint64_t font_handle, Color color) = 0;
    virtual TextMetrics measureText(std::string_view text, uint64_t font_handle) = 0;

    virtual uint64_t createFont(std::string_view family, float size, int weight, bool italic) = 0;
    virtual void deleteFont(uint64_t font_handle) = 0;

    virtual void drawLine(float x1, float y1, float x2, float y2, Color color, float thickness) = 0;
    virtual void drawImage(const void* data, size_t len, float x, float y, float w, float h) = 0;

    virtual void setClip(float x, float y, float w, float h) = 0;
    virtual void resetClip() = 0;

    virtual void beginFrame(int width, int height) = 0;
    virtual void endFrame() = 0;
};

} // namespace bro::render
