#include "engine/scrollbar.h"
#include "css/color.h"

#include <algorithm>
#include <cmath>

namespace bro::engine {

using bromath::cfromColor8;

Scrollbar::Scrollbar() : Scrollbar(Style{}) {}
Scrollbar::Scrollbar(Style style) : style_(style) {}

ScrollbarMetrics Scrollbar::layout(float trackX, float trackY, float trackH,
                                   float contentH, float viewH,
                                   float scrollOffset) const {
    ScrollbarMetrics m;
    if (contentH <= viewH || viewH <= 0) {
        m.visible = false;
        return m;
    }

    m.visible = true;
    m.trackX = trackX;
    m.trackY = trackY;
    m.trackW = style_.width;
    m.trackH = trackH;

    float thumbRatio = viewH / contentH;
    m.thumbH = std::max(thumbRatio * trackH, style_.minThumbHeight);

    float scrollRange = contentH - viewH;
    float thumbRange = trackH - m.thumbH;
    m.thumbY = trackY + (scrollRange > 0.0f
        ? (scrollOffset / scrollRange) * thumbRange
        : 0.0f);

    return m;
}

Scrollbar::Colors Scrollbar::schemeColors(bool dark) {
    // Alpha is coverage (0..1). The palette used to be written as 0..255
    // bytes into these float colours, which clamped to opaque white.
    const float v = dark ? 1.0f : 0.0f;
    return Colors{bromath::Color{v, v, v, 0.08f},
                  bromath::Color{v, v, v, dark ? 0.40f : 0.35f},
                  bromath::Color{v, v, v, dark ? 0.60f : 0.50f},
                  bromath::Color{v, v, v, dark ? 0.70f : 0.60f}};
}

Scrollbar::Colors Scrollbar::colorsFor(const htmlayout::css::ComputedStyle& style,
                                       bool preferDark) {
    auto csIt = style.find("color-scheme");
    const std::string_view cs = csIt != style.end() ? std::string_view(csIt->second)
                                                    : std::string_view("normal");
    const bool dark = htmlayout::css::usedColorScheme(
                          cs, preferDark ? htmlayout::css::ColorScheme::Dark
                                         : htmlayout::css::ColorScheme::Light) ==
                      htmlayout::css::ColorScheme::Dark;
    Colors c = schemeColors(dark);

    // scrollbar-color: <thumb-color> <track-color> (CSS Scrollbars 1).
    auto scIt = style.find("scrollbar-color");
    if (scIt != style.end() && scIt->second != "auto" && !scIt->second.empty()) {
        const std::string& v = scIt->second;
        // Split into two colour tokens at a space outside parentheses.
        int depth = 0;
        size_t split = std::string::npos;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '(') ++depth;
            else if (v[i] == ')') --depth;
            else if (v[i] == ' ' && depth == 0) { split = i; break; }
        }
        if (split != std::string::npos) {
            htmlayout::css::Color thumb, track;
            std::string a = v.substr(0, split), b = v.substr(split + 1);
            while (!b.empty() && b.front() == ' ') b.erase(b.begin());
            if (htmlayout::css::tryParseColor(a, thumb) &&
                htmlayout::css::tryParseColor(b, track)) {
                auto conv = [](const htmlayout::css::Color& k) {
                    return cfromColor8({k.r, k.g, k.b, k.a});
                };
                c.thumb = c.thumbHover = c.thumbDrag = conv(thumb);
                c.track = conv(track);
            }
        }
    }
    return c;
}

void Scrollbar::draw(render::Renderer* renderer, const ScrollbarMetrics& m,
                     const Colors& colors) const {
    drawWithState(renderer, m, hovered_, dragging_, colors);
}

void Scrollbar::drawWithState(render::Renderer* renderer, const ScrollbarMetrics& m,
                              bool hovered, bool dragging, const Colors& colors) const {
    if (!m.visible || !renderer) return;

    // Track background
    renderer->fillRect(m.trackX, m.trackY, m.trackW, m.trackH, colors.track);

    // Thumb — color depends on interaction state
    bromath::Color thumbColor = colors.thumb;
    if (dragging) {
        thumbColor = colors.thumbDrag;
    } else if (hovered) {
        thumbColor = colors.thumbHover;
    }
    renderer->fillRect(m.trackX, m.thumbY, m.trackW, m.thumbH, thumbColor);
}

bool Scrollbar::hitTest(float x, float y, const ScrollbarMetrics& m) const {
    if (!m.visible) return false;
    return x >= m.trackX && x < m.trackX + m.trackW &&
           y >= m.trackY && y < m.trackY + m.trackH;
}

bool Scrollbar::thumbHitTest(float x, float y, const ScrollbarMetrics& m) const {
    if (!m.visible) return false;
    return x >= m.trackX && x < m.trackX + m.trackW &&
           y >= m.thumbY && y < m.thumbY + m.thumbH;
}

void Scrollbar::beginDrag(float mouseY, const ScrollbarMetrics& m) {
    dragging_ = true;
    dragStartMouseY_ = mouseY;
    dragStartThumbY_ = m.thumbY - m.trackY;
}

float Scrollbar::updateDrag(float mouseY, float contentH, float viewH,
                            const ScrollbarMetrics& m) const {
    if (!dragging_ || contentH <= viewH) return 0.0f;

    float thumbRange = m.trackH - m.thumbH;
    if (thumbRange <= 0) return 0.0f;

    float deltaY = mouseY - dragStartMouseY_;
    float newThumbPos = std::clamp(dragStartThumbY_ + deltaY, 0.0f, thumbRange);

    float scrollRange = contentH - viewH;
    return (newThumbPos / thumbRange) * scrollRange;
}

void Scrollbar::endDrag() {
    dragging_ = false;
}

float Scrollbar::scrollToPosition(float mouseY, float contentH, float viewH,
                                  const ScrollbarMetrics& m) const {
    if (!m.visible || contentH <= viewH) return 0.0f;

    float scrollRange = contentH - viewH;

    // Click above thumb → page up, below → page down
    if (mouseY < m.thumbY) {
        // Page up: scroll back by one view height
        float thumbRange = m.trackH - m.thumbH;
        if (thumbRange <= 0) return 0.0f;
        float currentRatio = (m.thumbY - m.trackY) / thumbRange;
        float currentScroll = currentRatio * scrollRange;
        return std::max(0.0f, currentScroll - viewH);
    } else {
        // Page down: scroll forward by one view height
        float thumbRange = m.trackH - m.thumbH;
        if (thumbRange <= 0) return 0.0f;
        float currentRatio = (m.thumbY - m.trackY) / thumbRange;
        float currentScroll = currentRatio * scrollRange;
        return std::min(scrollRange, currentScroll + viewH);
    }
}

} // namespace bro::engine
