#pragma once

#include "eye/eye_measure.h"
#include "eye/eye_types.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace bro::eye::draw {

inline const uint8_t* glyph5x7(char c) {
    static const uint8_t kSpace[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    struct Entry { char ch; uint8_t col[5]; };
    static const Entry kTable[] = {
        {'!', {0x00,0x00,0x5f,0x00,0x00}}, {'#', {0x14,0x7f,0x14,0x7f,0x14}},
        {'%', {0x23,0x13,0x08,0x64,0x62}}, {'&', {0x36,0x49,0x55,0x22,0x50}},
        {'\'', {0x00,0x05,0x03,0x00,0x00}}, {'(', {0x00,0x1c,0x22,0x41,0x00}},
        {')', {0x00,0x41,0x22,0x1c,0x00}}, {'*', {0x14,0x08,0x3e,0x08,0x14}},
        {'+', {0x08,0x08,0x3e,0x08,0x08}}, {',', {0x00,0x50,0x30,0x00,0x00}},
        {'-', {0x08,0x08,0x08,0x08,0x08}}, {'.', {0x00,0x60,0x60,0x00,0x00}},
        {'/', {0x20,0x10,0x08,0x04,0x02}}, {'0', {0x3e,0x51,0x49,0x45,0x3e}},
        {'1', {0x00,0x42,0x7f,0x40,0x00}}, {'2', {0x42,0x61,0x51,0x49,0x46}},
        {'3', {0x21,0x41,0x45,0x4b,0x31}}, {'4', {0x18,0x14,0x12,0x7f,0x10}},
        {'5', {0x27,0x45,0x45,0x45,0x39}}, {'6', {0x3c,0x4a,0x49,0x49,0x30}},
        {'7', {0x01,0x71,0x09,0x05,0x03}}, {'8', {0x36,0x49,0x49,0x49,0x36}},
        {'9', {0x06,0x49,0x49,0x29,0x1e}}, {':', {0x00,0x36,0x36,0x00,0x00}},
        {'<', {0x08,0x14,0x22,0x41,0x00}}, {'=', {0x14,0x14,0x14,0x14,0x14}},
        {'>', {0x00,0x41,0x22,0x14,0x08}}, {'?', {0x02,0x01,0x51,0x09,0x06}},
        {'_', {0x40,0x40,0x40,0x40,0x40}}, {'|', {0x00,0x00,0x7f,0x00,0x00}},
        {'A', {0x7e,0x11,0x11,0x11,0x7e}}, {'B', {0x7f,0x49,0x49,0x49,0x36}},
        {'C', {0x3e,0x41,0x41,0x41,0x22}}, {'D', {0x7f,0x41,0x41,0x22,0x1c}},
        {'E', {0x7f,0x49,0x49,0x49,0x41}}, {'F', {0x7f,0x09,0x09,0x09,0x01}},
        {'G', {0x3e,0x41,0x49,0x49,0x7a}}, {'H', {0x7f,0x08,0x08,0x08,0x7f}},
        {'I', {0x00,0x41,0x7f,0x41,0x00}}, {'J', {0x20,0x40,0x41,0x3f,0x01}},
        {'K', {0x7f,0x08,0x14,0x22,0x41}}, {'L', {0x7f,0x40,0x40,0x40,0x40}},
        {'M', {0x7f,0x02,0x0c,0x02,0x7f}}, {'N', {0x7f,0x04,0x08,0x10,0x7f}},
        {'O', {0x3e,0x41,0x41,0x41,0x3e}}, {'P', {0x7f,0x09,0x09,0x09,0x06}},
        {'Q', {0x3e,0x41,0x51,0x21,0x5e}}, {'R', {0x7f,0x09,0x19,0x29,0x46}},
        {'S', {0x46,0x49,0x49,0x49,0x31}}, {'T', {0x01,0x01,0x7f,0x01,0x01}},
        {'U', {0x3f,0x40,0x40,0x40,0x3f}}, {'V', {0x1f,0x20,0x40,0x20,0x1f}},
        {'W', {0x3f,0x40,0x38,0x40,0x3f}}, {'X', {0x63,0x14,0x08,0x14,0x63}},
        {'Y', {0x07,0x08,0x70,0x08,0x07}}, {'Z', {0x61,0x51,0x49,0x45,0x43}},
        {'a', {0x20,0x54,0x54,0x54,0x78}}, {'b', {0x7f,0x48,0x44,0x44,0x38}},
        {'c', {0x38,0x44,0x44,0x44,0x20}}, {'d', {0x38,0x44,0x44,0x48,0x7f}},
        {'e', {0x38,0x54,0x54,0x54,0x18}}, {'f', {0x08,0x7e,0x09,0x01,0x02}},
        {'g', {0x0c,0x52,0x52,0x52,0x3e}}, {'h', {0x7f,0x08,0x04,0x04,0x78}},
        {'i', {0x00,0x44,0x7d,0x40,0x00}}, {'j', {0x20,0x40,0x44,0x3d,0x00}},
        {'k', {0x7f,0x10,0x28,0x44,0x00}}, {'l', {0x00,0x41,0x7f,0x40,0x00}},
        {'m', {0x7c,0x04,0x18,0x04,0x78}}, {'n', {0x7c,0x08,0x04,0x04,0x78}},
        {'o', {0x38,0x44,0x44,0x44,0x38}}, {'p', {0x7c,0x14,0x14,0x14,0x08}},
        {'q', {0x08,0x14,0x14,0x18,0x7c}}, {'r', {0x7c,0x08,0x04,0x04,0x08}},
        {'s', {0x48,0x54,0x54,0x54,0x20}}, {'t', {0x04,0x3f,0x44,0x40,0x20}},
        {'u', {0x3c,0x40,0x40,0x20,0x7c}}, {'v', {0x1c,0x20,0x40,0x20,0x1c}},
        {'w', {0x3c,0x40,0x30,0x40,0x3c}}, {'x', {0x44,0x28,0x10,0x28,0x44}},
        {'y', {0x0c,0x50,0x50,0x50,0x3c}}, {'z', {0x44,0x64,0x54,0x4c,0x44}},
    };
    for (const auto& e : kTable) {
        if (e.ch == c) return e.col;
    }
    return kSpace;
}

inline void putPx(EyeImage& img, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
    uint8_t* p = img.rgba.data() + (static_cast<size_t>(y) * img.width + x) * 4;
    if (a == 255) {
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    } else if (a > 0) {
        const uint32_t ia = 255 - a;
        p[0] = static_cast<uint8_t>((p[0] * ia + r * a) / 255);
        p[1] = static_cast<uint8_t>((p[1] * ia + g * a) / 255);
        p[2] = static_cast<uint8_t>((p[2] * ia + b * a) / 255);
        p[3] = 255;
    }
}

inline void fillRect(EyeImage& img, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(img.width, x + w), y1 = std::min(img.height, y + h);
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) putPx(img, xx, yy, r, g, b, a);
    }
}

inline void drawRectBox(EyeImage& img, int x, int y, int w, int h, int thickness, uint8_t r, uint8_t g, uint8_t b) {
    if (w <= 0 || h <= 0) return;
    fillRect(img, x, y, w, thickness, r, g, b);
    fillRect(img, x, y + h - thickness, w, thickness, r, g, b);
    fillRect(img, x, y, thickness, h, r, g, b);
    fillRect(img, x + w - thickness, y, thickness, h, r, g, b);
}

inline void drawLine(EyeImage& img, int x0, int y0, int x1, int y1, int thickness,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    const int steps = std::max(1, std::max(dx, dy));
    const int half = std::max(0, thickness / 2);
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const int x = static_cast<int>(std::lround(x0 + (x1 - x0) * t));
        const int y = static_cast<int>(std::lround(y0 + (y1 - y0) * t));
        if (half == 0) {
            putPx(img, x, y, r, g, b, a);
        } else {
            fillRect(img, x - half, y - half, thickness, thickness, r, g, b, a);
        }
    }
}

inline void drawCircle(EyeImage& img, int cx, int cy, int rad, int thickness,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    const int steps = std::max(24, rad * 6);
    for (int i = 0; i < steps; ++i) {
        const double ang = (2.0 * 3.14159265358979323846 * i) / steps;
        const int x = static_cast<int>(std::lround(cx + rad * std::cos(ang)));
        const int y = static_cast<int>(std::lround(cy + rad * std::sin(ang)));
        fillRect(img, x - thickness / 2, y - thickness / 2, std::max(1, thickness), std::max(1, thickness), r, g, b, a);
    }
}

inline void drawText(EyeImage& img, int x, int y, const std::string& text, int scale,
                     uint8_t r, uint8_t g, uint8_t b, bool pillBg = true) {
    if (text.empty()) return;
    scale = std::clamp(scale, 1, 4);
    const int charW = 6 * scale;
    const int charH = 7 * scale;
    if (pillBg) {
        fillRect(img, x - 3, y - 2, static_cast<int>(text.size()) * charW + 6, charH + 4, 16, 18, 22, 220);
    }
    for (size_t i = 0; i < text.size(); ++i) {
        const uint8_t* cols = glyph5x7(text[i]);
        const int cx = x + static_cast<int>(i) * charW;
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if (cols[col] & (1u << row)) {
                    fillRect(img, cx + col * scale, y + row * scale, scale, scale, r, g, b);
                }
            }
        }
    }
}

inline EyeImage fitPanel(EyeImageView src, int maxDim) {
    if (!src.valid()) return {};
    if (maxDim <= 0 || (src.width <= maxDim && src.height <= maxDim)) {
        EyeImage out{src.width, src.height, {}};
        out.rgba.resize(static_cast<size_t>(src.width) * src.height * 4);
        const int stride = src.effectiveStride();
        for (int y = 0; y < src.height; ++y) {
            std::memcpy(out.rgba.data() + static_cast<size_t>(y) * src.width * 4,
                        src.rgba + static_cast<size_t>(y) * stride,
                        static_cast<size_t>(src.width) * 4);
        }
        return out;
    }
    const double scale = static_cast<double>(maxDim) / static_cast<double>(std::max(src.width, src.height));
    const int nw = std::max(1, static_cast<int>(std::lround(src.width * scale)));
    const int nh = std::max(1, static_cast<int>(std::lround(src.height * scale)));
    return resampleRgba(src, nw, nh);
}

inline void blitPanel(EyeImage& dst, int dx, int dy, const EyeImage& src) {
    for (int y = 0; y < src.height; ++y) {
        if (dy + y < 0 || dy + y >= dst.height) continue;
        for (int x = 0; x < src.width; ++x) {
            if (dx + x < 0 || dx + x >= dst.width) continue;
            const uint8_t* sp = src.rgba.data() + (static_cast<size_t>(y) * src.width + x) * 4;
            uint8_t* dp = dst.rgba.data() + (static_cast<size_t>(dy + y) * dst.width + (dx + x)) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
        }
    }
}

inline EyeVisualResult assembleSheet(const std::vector<EyeImage>& panels,
                                     const std::vector<std::string>& labels,
                                     const std::string& layout,
                                     int fontScale) {
    EyeVisualResult res;
    if (panels.empty()) return res;

    const int pad = 12;
    const int headerH = 9 * std::clamp(fontScale, 1, 4) + 8;
    const size_t count = panels.size();
    const int cols = (layout == "stack") ? 1
                   : (layout == "grid" ? static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))))
                                       : static_cast<int>(count));
    const int rows = static_cast<int>((count + cols - 1) / cols);

    int cellW = 1, cellH = 1;
    for (const auto& p : panels) {
        cellW = std::max(cellW, p.width);
        cellH = std::max(cellH, p.height);
    }

    const int totalW = pad + cols * (cellW + pad);
    const int totalH = pad + rows * (headerH + cellH + pad);

    EyeImage canvas{totalW, totalH, std::vector<uint8_t>(static_cast<size_t>(totalW) * totalH * 4, 24)};
    for (size_t i = 3; i < canvas.rgba.size(); i += 4) canvas.rgba[i] = 255;

    for (size_t i = 0; i < count; ++i) {
        const int c = static_cast<int>(i % cols);
        const int r = static_cast<int>(i / cols);
        const int x = pad + c * (cellW + pad);
        const int y = pad + r * (headerH + cellH + pad);
        std::string lbl = i < labels.size() ? labels[i] : ("panel " + std::to_string(i));
        int lblScale = fontScale;
        while (lblScale > 1 && static_cast<int>(lbl.size()) * 6 * lblScale > cellW) --lblScale;
        const int maxChars = std::max(4, cellW / (6 * lblScale));
        if (static_cast<int>(lbl.size()) > maxChars) lbl = lbl.substr(0, static_cast<size_t>(maxChars - 2)) + "..";
        drawText(canvas, x, y + 2, lbl, lblScale, 235, 240, 245, false);
        blitPanel(canvas, x, y + headerH, panels[i]);
        drawRectBox(canvas, x, y + headerH, panels[i].width, panels[i].height, 1, 70, 74, 82);
        res.panels.push_back({lbl, x, y + headerH, panels[i].width, panels[i].height});
    }

    res.width = canvas.width;
    res.height = canvas.height;
    res.rgba = std::move(canvas.rgba);
    return res;
}

}  // namespace bro::eye::draw
