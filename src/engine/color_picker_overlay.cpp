#include "engine/color_picker_overlay.h"

#include "layout/draw_traversal.h"
#include "util/platform.h"

#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace bro::engine {

using bromath::cfromColor8;

const char* const ColorPickerOverlay::kPresetHex[8] = {
    "#000000", "#ffffff", "#ff4136", "#ff851b",
    "#ffdc00", "#2ecc40", "#0074d9", "#b10dc9",
};

// ---------------------------------------------------------------------------
// Color conversions
// ---------------------------------------------------------------------------

void ColorPickerOverlay::hsvToRgb(float h, float s, float v,
                                  uint8_t& r, uint8_t& g, uint8_t& b) {
    float c = v * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float m = v - c;
    float rf = 0, gf = 0, bf = 0;
    if (hp < 1)      { rf = c; gf = x; bf = 0; }
    else if (hp < 2) { rf = x; gf = c; bf = 0; }
    else if (hp < 3) { rf = 0; gf = c; bf = x; }
    else if (hp < 4) { rf = 0; gf = x; bf = c; }
    else if (hp < 5) { rf = x; gf = 0; bf = c; }
    else             { rf = c; gf = 0; bf = x; }
    r = static_cast<uint8_t>(std::round((rf + m) * 255.0f));
    g = static_cast<uint8_t>(std::round((gf + m) * 255.0f));
    b = static_cast<uint8_t>(std::round((bf + m) * 255.0f));
}

void ColorPickerOverlay::rgbToHsv(uint8_t rr, uint8_t gg, uint8_t bb,
                                  float& h, float& s, float& v) {
    float r = rr / 255.0f, g = gg / 255.0f, b = bb / 255.0f;
    float mx = std::max({r, g, b});
    float mn = std::min({r, g, b});
    float d = mx - mn;
    v = mx;
    s = mx <= 0.0f ? 0.0f : d / mx;
    if (d <= 0.0f) { h = 0.0f; return; }
    if (mx == r)      h = 60.0f * (std::fmod((g - b) / d, 6.0f));
    else if (mx == g) h = 60.0f * (((b - r) / d) + 2.0f);
    else              h = 60.0f * (((r - g) / d) + 4.0f);
    if (h < 0.0f) h += 360.0f;
}

// ---------------------------------------------------------------------------
// Construction / layout
// ---------------------------------------------------------------------------

ColorPickerOverlay::ColorPickerOverlay(float anchorX, float anchorY,
                                       float anchorW, float anchorH,
                                       float viewportW, float viewportH,
                                       const std::string& initialHex,
                                       bool hasAlpha,
                                       ChangeCallback onChange,
                                       CommitCallback onCommit)
    : hasAlpha_(hasAlpha),
      onChange_(std::move(onChange)),
      onCommit_(std::move(onCommit)) {
    float ph, ps, pv, pa;
    if (parseColor(initialHex, ph, ps, pv, pa)) {
        h_ = ph; s_ = ps; v_ = pv; a_ = pa;
    }

    // Anchor below the swatch by default; clamp to viewport if provided.
    originX_ = anchorX;
    originY_ = anchorY + anchorH + 4.0f;
    if (viewportW > 0) {
        originX_ = std::min(originX_, viewportW - kPopupW - 4.0f);
        originX_ = std::max(originX_, 4.0f);
    }
    if (viewportH > 0 && originY_ + kPopupH > viewportH - 4.0f) {
        // Not enough room below — flip above the swatch.
        float above = anchorY - kPopupH - 4.0f;
        if (above >= 4.0f) originY_ = above;
        else originY_ = std::max(4.0f, viewportH - kPopupH - 4.0f);
    }
    initRects();

    // Preset colors are constant — resolve once now instead of per-frame.
    for (size_t i = 0; i < presetColors_.size(); ++i) {
        bromath::Color c = cfromColor8({0, 0, 0, 255});
        layout::DrawTraversal::tryParseColor(kPresetHex[i], c);
        presetColors_[i] = c;
    }

    syncHexText();
    lastEmitted_ = hexText_;
}

void ColorPickerOverlay::initRects() {
    const float pad = 10.0f;

    // SV square (left) + Hue slider (right)
    const float svSize = 200.0f;
    const float hueW = 22.0f;
    const float gap = 8.0f;

    rSV_ = { pad, pad, svSize, svSize };
    rHue_ = { pad + svSize + gap, pad, hueW, svSize };

    // Alpha slider
    float ay = rSV_.y + rSV_.h + 10.0f;
    rAlpha_ = { pad, ay, kPopupW - 2 * pad, 14.0f };

    // Preview + hex field
    float fy = rAlpha_.y + rAlpha_.h + 10.0f;
    rPreview_ = { pad, fy, 44.0f, 26.0f };
    rHex_ = { pad + 50.0f, fy, kPopupW - 2 * pad - 50.0f, 26.0f };

    // RGB(A) readout row
    float ry = rHex_.y + rHex_.h + 8.0f;
    rRGBRow_ = { pad, ry, kPopupW - 2 * pad, 16.0f };

    // Presets row (8 swatches)
    float py = rRGBRow_.y + rRGBRow_.h + 10.0f;
    float swGap = 4.0f;
    float swW = (kPopupW - 2 * pad - 7 * swGap) / 8.0f;
    for (int i = 0; i < 8; ++i) {
        rPresets_[i] = { pad + i * (swW + swGap), py, swW, 18.0f };
    }
}

// ---------------------------------------------------------------------------
// Parsing / formatting
// ---------------------------------------------------------------------------

bool ColorPickerOverlay::parseColor(const std::string& s,
                                    float& outH, float& outS, float& outV, float& outA) {
    bromath::Color c = cfromColor8({0, 0, 0, 255});
    if (!layout::DrawTraversal::tryParseColor(s, c)) return false;
    // HSV operates in sRGB space; round-trip through Color8 at the boundary.
    bromath::Color8 c8 = bromath::ctoColor8(c);
    rgbToHsv(c8.r, c8.g, c8.b, outH, outS, outV);
    outA = c.a;
    return true;
}

std::string ColorPickerOverlay::formatHex() const {
    uint8_t r, g, b;
    hsvToRgb(h_, s_, v_, r, g, b);
    char buf[12];
    if (hasAlpha_) {
        int a8 = static_cast<int>(std::round(a_ * 255.0f));
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", r, g, b, a8);
    } else {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    }
    return buf;
}

void ColorPickerOverlay::syncHexText() {
    hexText_ = formatHex();
    hexCursor_ = static_cast<int>(hexText_.size());
}

void ColorPickerOverlay::applyHexText() {
    float h, s, v, a;
    if (!parseColor(hexText_, h, s, v, a)) return;
    h_ = h; s_ = s; v_ = v; a_ = a;
    emitChange();
}

void ColorPickerOverlay::emitChange() {
    if (!onChange_) return;
    std::string hex = formatHex();
    if (hex == lastEmitted_) return; // dedupe: skip JS event storm during drag
    lastEmitted_ = hex;
    onChange_(hex);
}

void ColorPickerOverlay::onDismiss() {
    if (onCommit_) onCommit_(formatHex());
}

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

void ColorPickerOverlay::getBounds(float& x, float& y, float& w, float& h) const {
    x = originX_;
    y = originY_;
    w = kPopupW;
    h = kPopupH;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void drawCheckerboard(render::Renderer* r, float x, float y, float w, float h,
                             float cell = 5.0f) {
    r->fillRect(x, y, w, h, cfromColor8({220, 220, 220, 255}));
    bromath::Color dark = cfromColor8({170, 170, 170, 255});
    int cols = static_cast<int>(std::ceil(w / cell));
    int rows = static_cast<int>(std::ceil(h / cell));
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            if (((row + col) & 1) == 0) continue;
            float cx = x + col * cell;
            float cy = y + row * cell;
            float cw = std::min(cell, x + w - cx);
            float ch = std::min(cell, y + h - cy);
            if (cw > 0 && ch > 0) r->fillRect(cx, cy, cw, ch, dark);
        }
    }
}

void ColorPickerOverlay::draw(render::Renderer* r) {
    if (!r) return;

    render::FontRef font{std::string_view{"Arial"}, 12.0f, 400, false};
    render::FontRef fontSmall{std::string_view{"Arial"}, 10.0f, 400, false};

    // Lazy line-metrics resolve.
    if (!fontMetricsResolved_) {
        fontAscent_      = render::LineMetrics::from(r->measureText("M", font)).ascent;
        fontSmallAscent_ = render::LineMetrics::from(r->measureText("M", fontSmall)).ascent;
        fontMetricsResolved_ = true;
    }

    // Background
    r->fillRect(originX_, originY_, kPopupW, kPopupH, cfromColor8({40, 40, 44, 245}));
    r->drawRect(originX_, originY_, kPopupW, kPopupH, cfromColor8({120, 120, 130, 255}));

    // --- SV square ---
    {
        float x = originX_ + rSV_.x;
        float y = originY_ + rSV_.y;
        float w = rSV_.w;
        float h = rSV_.h;

        // Base: pure hue color at full S=1, V=1
        uint8_t rr, gg, bb;
        hsvToRgb(h_, 1.0f, 1.0f, rr, gg, bb);
        r->fillRect(x, y, w, h, cfromColor8({rr, gg, bb, 255}));

        // White→transparent horizontal (saturates left→right)
        {
            std::array<render::ColorStop, 2> stops = {{
                {0.0f, cfromColor8({255, 255, 255, 255})},
                {1.0f, cfromColor8({255, 255, 255, 0})},
            }};
            r->fillLinearGradient(x, y, w, h, x, y, x + w, y, stops);
        }
        // Black→transparent vertical (top=1, bottom=0)
        {
            std::array<render::ColorStop, 2> stops = {{
                {0.0f, cfromColor8({0, 0, 0, 0})},
                {1.0f, cfromColor8({0, 0, 0, 255})},
            }};
            r->fillLinearGradient(x, y, w, h, x, y, x, y + h, stops);
        }

        // Border
        r->drawRect(x, y, w, h, cfromColor8({80, 80, 90, 255}));

        // Selection ring
        float cx = x + s_ * w;
        float cy = y + (1.0f - v_) * h;
        r->drawCircle(cx, cy, 5.0f, cfromColor8({0, 0, 0, 0}), cfromColor8({255, 255, 255, 255}), 2.0f);
        r->drawCircle(cx, cy, 6.5f, cfromColor8({0, 0, 0, 0}), cfromColor8({0, 0, 0, 220}), 1.0f);
    }

    // --- Hue slider ---
    {
        float x = originX_ + rHue_.x;
        float y = originY_ + rHue_.y;
        float w = rHue_.w;
        float h = rHue_.h;

        std::array<render::ColorStop, 7> stops = {{
            {0.0f      , cfromColor8({255, 0, 0, 255})},
            {1.0f / 6.0f, cfromColor8({255, 255, 0, 255})},
            {2.0f / 6.0f, cfromColor8({0, 255, 0, 255})},
            {3.0f / 6.0f, cfromColor8({0, 255, 255, 255})},
            {4.0f / 6.0f, cfromColor8({0, 0, 255, 255})},
            {5.0f / 6.0f, cfromColor8({255, 0, 255, 255})},
            {1.0f      , cfromColor8({255, 0, 0, 255})},
        }};
        r->fillLinearGradient(x, y, w, h, x, y, x, y + h, stops);
        r->drawRect(x, y, w, h, cfromColor8({80, 80, 90, 255}));

        float hy = y + (h_ / 360.0f) * h;
        r->drawLine(x - 2, hy, x + w + 2, hy, cfromColor8({255, 255, 255, 255}), 2.0f);
        r->drawLine(x - 2, hy, x + w + 2, hy, cfromColor8({0, 0, 0, 220}), 1.0f);
    }

    // --- Alpha slider ---
    {
        float x = originX_ + rAlpha_.x;
        float y = originY_ + rAlpha_.y;
        float w = rAlpha_.w;
        float h = rAlpha_.h;

        drawCheckerboard(r, x, y, w, h);

        uint8_t rr, gg, bb;
        hsvToRgb(h_, s_, v_, rr, gg, bb);
        std::array<render::ColorStop, 2> stops = {{
            {0.0f, cfromColor8({rr, gg, bb, 0})},
            {1.0f, cfromColor8({rr, gg, bb, 255})},
        }};
        r->fillLinearGradient(x, y, w, h, x, y, x + w, y, stops);
        r->drawRect(x, y, w, h, cfromColor8({80, 80, 90, 255}));

        if (hasAlpha_) {
            float ax = x + a_ * w;
            r->drawLine(ax, y - 2, ax, y + h + 2, cfromColor8({255, 255, 255, 255}), 2.0f);
            r->drawLine(ax, y - 2, ax, y + h + 2, cfromColor8({0, 0, 0, 220}), 1.0f);
        }
    }

    // --- Preview + hex field ---
    {
        float px = originX_ + rPreview_.x;
        float py = originY_ + rPreview_.y;
        float pw = rPreview_.w;
        float ph = rPreview_.h;

        drawCheckerboard(r, px, py, pw, ph);
        uint8_t rr, gg, bb;
        hsvToRgb(h_, s_, v_, rr, gg, bb);
        uint8_t a8 = static_cast<uint8_t>(std::round(a_ * 255.0f));
        r->fillRect(px, py, pw, ph, cfromColor8({rr, gg, bb, a8}));
        r->drawRect(px, py, pw, ph, cfromColor8({80, 80, 90, 255}));

        // Hex text field
        float hx = originX_ + rHex_.x;
        float hy = originY_ + rHex_.y;
        float hw = rHex_.w;
        float hh = rHex_.h;

        r->fillRect(hx, hy, hw, hh, cfromColor8({255, 255, 255, 255}));
        r->drawRect(hx, hy, hw, hh,
                    hexFocused_ ? cfromColor8({0, 120, 215, 255})
                                : cfromColor8({80, 80, 90, 255}));

        {
            float textX = hx + 6.0f;
            float textY = hy + (hh - (fontAscent_ * 1.25f)) / 2.0f + fontAscent_;
            r->drawText(hexText_, textX, textY, font, cfromColor8({30, 30, 30, 255}));

            if (hexFocused_) {
                std::string pre = hexText_.substr(0,
                    std::clamp(hexCursor_, 0, static_cast<int>(hexText_.size())));
                float cx = textX + r->measureText(pre, font).width;
                r->drawLine(cx, hy + 4.0f, cx, hy + hh - 4.0f,
                            cfromColor8({30, 30, 30, 255}), 1.0f);
            }
        }
    }

    // --- RGB(A) row ---
    {
        uint8_t rr, gg, bb;
        hsvToRgb(h_, s_, v_, rr, gg, bb);
        char buf[64];
        if (hasAlpha_) {
            int a8 = static_cast<int>(std::round(a_ * 255.0f));
            std::snprintf(buf, sizeof(buf), "R %d   G %d   B %d   A %d",
                         rr, gg, bb, a8);
        } else {
            std::snprintf(buf, sizeof(buf), "R %d   G %d   B %d", rr, gg, bb);
        }
        float tx = originX_ + rRGBRow_.x;
        float ty = originY_ + rRGBRow_.y + fontSmallAscent_;
        r->drawText(buf, tx, ty, fontSmall, cfromColor8({200, 200, 205, 255}));
    }

    // --- Presets ---
    for (size_t i = 0; i < rPresets_.size(); ++i) {
        float x = originX_ + rPresets_[i].x;
        float y = originY_ + rPresets_[i].y;
        float w = rPresets_[i].w;
        float h = rPresets_[i].h;
        r->fillRect(x, y, w, h, presetColors_[i]);
        r->drawRect(x, y, w, h, cfromColor8({80, 80, 90, 255}));
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static bool inRect(float x, float y, float rx, float ry, float rw, float rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

bool ColorPickerOverlay::onMouseDown(float x, float y, int /*button*/) {
    float svX = originX_ + rSV_.x, svY = originY_ + rSV_.y;
    float hueX = originX_ + rHue_.x, hueY = originY_ + rHue_.y;
    float aX = originX_ + rAlpha_.x, aY = originY_ + rAlpha_.y;

    if (inRect(x, y, svX, svY, rSV_.w, rSV_.h)) {
        drag_ = Drag::SV;
        s_ = std::clamp((x - svX) / rSV_.w, 0.0f, 1.0f);
        v_ = 1.0f - std::clamp((y - svY) / rSV_.h, 0.0f, 1.0f);
        hexFocused_ = false;
        syncHexText();
        emitChange();
        return true;
    }

    if (inRect(x, y, hueX, hueY, rHue_.w, rHue_.h)) {
        drag_ = Drag::Hue;
        h_ = std::clamp((y - hueY) / rHue_.h, 0.0f, 1.0f) * 360.0f;
        if (h_ >= 360.0f) h_ = 0.0f;
        hexFocused_ = false;
        syncHexText();
        emitChange();
        return true;
    }

    if (hasAlpha_ && inRect(x, y, aX, aY, rAlpha_.w, rAlpha_.h)) {
        drag_ = Drag::Alpha;
        a_ = std::clamp((x - aX) / rAlpha_.w, 0.0f, 1.0f);
        hexFocused_ = false;
        syncHexText();
        emitChange();
        return true;
    }

    // Hex field click — focus it
    float hx = originX_ + rHex_.x, hy = originY_ + rHex_.y;
    if (inRect(x, y, hx, hy, rHex_.w, rHex_.h)) {
        hexFocused_ = true;
        hexCursor_ = static_cast<int>(hexText_.size());
        return true;
    }

    // Presets
    for (size_t i = 0; i < rPresets_.size(); ++i) {
        float px = originX_ + rPresets_[i].x;
        float py = originY_ + rPresets_[i].y;
        if (inRect(x, y, px, py, rPresets_[i].w, rPresets_[i].h)) {
            float ph, ps, pv, pa;
            if (parseColor(kPresetHex[i], ph, ps, pv, pa)) {
                h_ = ph; s_ = ps; v_ = pv;
                if (hasAlpha_) a_ = pa;
            }
            hexFocused_ = false;
            syncHexText();
            emitChange();
            return true;
        }
    }

    // Click inside popup but not on any control — clear hex focus.
    hexFocused_ = false;
    return true;
}

bool ColorPickerOverlay::onMouseMove(float x, float y) {
    if (drag_ == Drag::None) return false;

    switch (drag_) {
        case Drag::SV: {
            float svX = originX_ + rSV_.x, svY = originY_ + rSV_.y;
            s_ = std::clamp((x - svX) / rSV_.w, 0.0f, 1.0f);
            v_ = 1.0f - std::clamp((y - svY) / rSV_.h, 0.0f, 1.0f);
            break;
        }
        case Drag::Hue: {
            float hueY = originY_ + rHue_.y;
            h_ = std::clamp((y - hueY) / rHue_.h, 0.0f, 1.0f) * 360.0f;
            if (h_ >= 360.0f) h_ = 0.0f;
            break;
        }
        case Drag::Alpha: {
            float aX = originX_ + rAlpha_.x;
            a_ = std::clamp((x - aX) / rAlpha_.w, 0.0f, 1.0f);
            break;
        }
        case Drag::None: break;
    }
    syncHexText();
    emitChange();
    return true;
}

bool ColorPickerOverlay::onMouseUp(float /*x*/, float /*y*/, int /*button*/) {
    drag_ = Drag::None;
    return false;
}

bool ColorPickerOverlay::onKeyDown(int keycode, int mod) {
    if (!hexFocused_) {
        if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
            requestDismiss();
            return true;
        }
        return false;
    }

    int pos = std::clamp(hexCursor_, 0, static_cast<int>(hexText_.size()));

    if (keycode == SDLK_BACKSPACE) {
        if (pos > 0) {
            hexText_.erase(pos - 1, 1);
            hexCursor_ = pos - 1;
            applyHexText();
        }
        return true;
    }
    if (keycode == SDLK_DELETE) {
        if (pos < static_cast<int>(hexText_.size())) {
            hexText_.erase(pos, 1);
            applyHexText();
        }
        return true;
    }
    if (keycode == SDLK_LEFT)  { if (pos > 0) hexCursor_ = pos - 1; return true; }
    if (keycode == SDLK_RIGHT) {
        if (pos < static_cast<int>(hexText_.size())) hexCursor_ = pos + 1;
        return true;
    }
    if (keycode == SDLK_HOME) { hexCursor_ = 0; return true; }
    if (keycode == SDLK_END)  { hexCursor_ = static_cast<int>(hexText_.size()); return true; }
    if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
        applyHexText();
        syncHexText();
        hexFocused_ = false;
        requestDismiss();
        return true;
    }
    if (util::hasPrimaryMod(mod) && keycode == SDLK_A) {
        hexCursor_ = static_cast<int>(hexText_.size());
        return true;
    }

    return false;
}

bool ColorPickerOverlay::onTextInput(const std::string& text) {
    if (!hexFocused_) return false;

    // Filter: only allow hex chars + '#'. Cap length at 9 ("#rrggbbaa").
    std::string filtered;
    filtered.reserve(text.size());
    for (char c : text) {
        if (c == '#' || (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            filtered.push_back(c);
        }
    }
    if (filtered.empty()) return false;

    constexpr size_t maxLen = 9; // "#rrggbbaa"
    if (hexText_.size() >= maxLen) return true;
    if (hexText_.size() + filtered.size() > maxLen) {
        filtered.resize(maxLen - hexText_.size());
    }

    int pos = std::clamp(hexCursor_, 0, static_cast<int>(hexText_.size()));
    hexText_.insert(pos, filtered);
    hexCursor_ = pos + static_cast<int>(filtered.size());
    applyHexText();
    return true;
}

} // namespace bro::engine
