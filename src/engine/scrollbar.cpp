#include "engine/scrollbar.h"

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

void Scrollbar::draw(render::Renderer* renderer, const ScrollbarMetrics& m) const {
    drawWithState(renderer, m, hovered_, dragging_);
}

void Scrollbar::drawWithState(render::Renderer* renderer, const ScrollbarMetrics& m,
                              bool hovered, bool dragging) const {
    if (!m.visible || !renderer) return;

    // Track background
    renderer->fillRect(m.trackX, m.trackY, m.trackW, m.trackH, style_.trackColor);

    // Thumb — color depends on interaction state
    bromath::Color thumbColor = style_.thumbColor;
    if (dragging) {
        thumbColor = style_.thumbDragColor;
    } else if (hovered) {
        thumbColor = style_.thumbHoverColor;
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
