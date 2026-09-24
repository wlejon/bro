#pragma once

#include "render/renderer.h"
#include "css/cascade.h"

namespace bro::engine {

/// Computed scrollbar geometry, reusable for drawing and hit testing.
struct ScrollbarMetrics {
    float trackX = 0, trackY = 0, trackW = 0, trackH = 0;
    float thumbY = 0, thumbH = 0;
    bool visible = false;  // false if content fits (no scrollbar needed)
};

/// A scrollbar UI component. Does NOT own scroll position — the caller
/// provides content/view dimensions and scroll offset, and the Scrollbar
/// handles rendering, hit testing, and drag interaction.
class Scrollbar {
public:
    /// Overlay colours (bromath::Color is 0..1 linear; alpha is coverage).
    struct Colors {
        bromath::Color track;
        bromath::Color thumb;
        bromath::Color thumbHover;
        bromath::Color thumbDrag;
    };

    struct Style {
        float width = 8.0f;
        float margin = 2.0f;
        float minThumbHeight = 24.0f;
    };

    /// The overlay palette for a used colour scheme: translucent white over a
    /// dark scheme, translucent black over a light one.
    static Colors schemeColors(bool dark);

    /// The palette for an element: CSS `scrollbar-color: <thumb> <track>`
    /// when set, else schemeColors() of the element's used colour scheme
    /// (its computed `color-scheme` weighed against `preferDark`, the
    /// prefers-color-scheme setting).
    static Colors colorsFor(const htmlayout::css::ComputedStyle& style, bool preferDark);

    Scrollbar();
    explicit Scrollbar(Style style);

    /// Compute scrollbar geometry from content/view dimensions.
    /// trackX, trackY: top-left of the scrollbar track in screen space.
    /// trackH: height of the track (usually the element's border-box height).
    /// contentH: total content height. viewH: visible height.
    /// scrollOffset: current scroll position (0 = top).
    ScrollbarMetrics layout(float trackX, float trackY, float trackH,
                            float contentH, float viewH,
                            float scrollOffset) const;

    /// Draw the scrollbar (track + thumb) using the given metrics.
    void draw(render::Renderer* renderer, const ScrollbarMetrics& m,
              const Colors& colors) const;

    /// Draw the scrollbar with explicit hover/drag state (for per-element tracking).
    void drawWithState(render::Renderer* renderer, const ScrollbarMetrics& m,
                       bool hovered, bool dragging, const Colors& colors) const;

    /// Returns true if (x, y) is within the scrollbar track.
    bool hitTest(float x, float y, const ScrollbarMetrics& m) const;

    /// Returns true if (x, y) is within the thumb specifically.
    bool thumbHitTest(float x, float y, const ScrollbarMetrics& m) const;

    /// Begin a thumb drag. Call when mousedown hits the thumb.
    void beginDrag(float mouseY, const ScrollbarMetrics& m);

    /// Update during drag — returns the new scroll offset.
    float updateDrag(float mouseY, float contentH, float viewH,
                     const ScrollbarMetrics& m) const;

    /// End a drag.
    void endDrag();

    bool isDragging() const { return dragging_; }

    /// Set/get hover state for visual feedback.
    void setHovered(bool h) { hovered_ = h; }
    bool isHovered() const { return hovered_; }

    /// Click on track (not on thumb): returns target scroll offset
    /// for a page-scroll toward the click position.
    float scrollToPosition(float mouseY, float contentH, float viewH,
                           const ScrollbarMetrics& m) const;

    const Style& style() const { return style_; }

private:
    Style style_;
    bool dragging_ = false;
    bool hovered_ = false;
    float dragStartMouseY_ = 0.0f;
    float dragStartThumbY_ = 0.0f;
};

} // namespace bro::engine
