#pragma once

// Helpers shared by the DrawTraversal translation units (draw_traversal*.cpp).
// Internal to the painter: nothing outside src/layout/draw_traversal*.cpp
// includes this. The small ones are inline here; the rest are defined once in
// draw_traversal_css.cpp.

#include "layout/draw_traversal.h"
#include "layout/css_shadow.h"
#include "css/transform.h"
#include "dom/element.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace bro::layout {

using bromath::cfromColor8;

// True when the style's `opacity` makes its whole subtree invisible: the
// alpha it would composite at is 0. Such a subtree records no paint at all, so
// a closed, faded-out overlay leaves its layer empty instead of compositing a
// transparent surface over what is under it.
template <class Style>
inline bool opacityHidesAll(const Style& style) {
    auto it = style.find("opacity");
    if (it == style.end() || it->second.empty()) return false;
    const float opacity = std::strtof(it->second.c_str(), nullptr);
    return static_cast<int>(opacity * 255) <= 0;
}

// ---------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------

// Return true if a CSS `transform` value uses any 3D function (rotateX/Y/3d,
// translateZ/3d, scaleZ/3d, perspective(), matrix3d). Cheap substring scan —
// false positives only mean we take the 4x4 path unnecessarily.
inline bool transformHas3D(const std::string& v) {
    if (v.empty() || v == "none") return false;
    // Quickly look for any of the 3D function tokens.
    static constexpr const char* kKeys[] = {
        "rotateX", "rotateY", "rotate3d", "rotateZ",
        "translateZ", "translate3d", "scaleZ", "scale3d",
        "matrix3d", "perspective("
    };
    for (auto* k : kKeys) {
        if (v.find(k) != std::string::npos) return true;
    }
    return false;
}

// Walk to the element's layout parent and read its `perspective` value (length
// or 0 for "none"). Returns 0 if no ancestor sets perspective or the element
// has no parent.
inline float parentPerspective(bro::dom::Element* elem) {
    if (!elem) return 0.0f;
    auto* p = elem->layoutParent();
    if (!p) return 0.0f;
    auto& cs = p->computedStyle();
    auto it = cs.find("perspective");
    if (it == cs.end()) return 0.0f;
    return htmlayout::css::parsePerspective(it->second);
}

// The element's full 4x4 transform including ancestor perspective (see the
// definition in draw_traversal_css.cpp for the argument conventions).
htmlayout::css::Matrix3D buildElementTransform4x4(
        const htmlayout::css::ComputedStyle& style,
        float bx, float by, float bw, float bh,
        float persp,
        float pbx, float pby, float pbw, float pbh,
        const htmlayout::css::ComputedStyle* perspStyle,
        bool& has3D);

// ---------------------------------------------------------------------------
// Colour, shadows, filters, blending, clip-path
// ---------------------------------------------------------------------------

// The element's `color`: what `currentcolor` (and a shadow that names no
// colour) paints with.
inline bromath::Color styleCurrentColor(const htmlayout::css::ComputedStyle& style) {
    bromath::Color c = cfromColor8({0, 0, 0, 255});
    auto it = style.find("color");
    if (it != style.end()) DrawTraversal::tryParseColor(it->second, c);
    return c;
}

// CSS image-rendering: pixelated / crisp-edges sample nearest, the rest
// (auto, smooth, high-quality) smooth. Inherited, so the computed value is
// the element's own.
inline render::ImageSampling styleImageSampling(const htmlayout::css::ComputedStyle& style) {
    auto it = style.find("image-rendering");
    if (it != style.end() &&
        (it->second == "pixelated" || it->second == "crisp-edges" || it->second == "-webkit-optimize-contrast"))
        return render::ImageSampling::Pixelated;
    return render::ImageSampling::Smooth;
}

// CSS image-orientation: `none` draws a photo as stored; anything else
// (from-image, the initial value) turns it upright by its EXIF orientation.
inline bool styleImageOriented(const htmlayout::css::ComputedStyle& style) {
    auto it = style.find("image-orientation");
    return it == style.end() || it->second != "none";
}

// What a shadow's em/rem/vw lengths resolve against for `elem`.
CssLengthContext shadowLengthContext(dom::Element* elem,
                                     const htmlayout::css::ComputedStyle& style,
                                     int viewportW, int viewportH);

// CSS `filter:` value → list of CssFilterParams descriptors.
std::vector<render::CssFilterParams> parseCSSFilter(const std::string& val,
                                                    const bromath::Color& currentColor,
                                                    const CssLengthContext& lengths);

// CSS `mix-blend-mode` / `background-blend-mode` keyword → render::BlendMode.
render::BlendMode parseBlendMode(const std::string& v);

// CSS `clip-path: polygon(...)` → border-box-relative vertices (empty = no clip).
std::vector<render::PointF> parseClipPathPolygon(
    const std::string& val, float refW, float refH, float fontSize);

// ---------------------------------------------------------------------------
// Overflow
// ---------------------------------------------------------------------------

/// Get the effective vertical overflow value, checking overflow-y then overflow.
inline std::string getOverflowY(const htmlayout::css::ComputedStyle& style) {
    auto oyIt = style.find("overflow-y");
    if (oyIt != style.end()) return oyIt->second;
    auto oIt = style.find("overflow");
    if (oIt != style.end()) return oIt->second;
    return "visible";
}

/// Get the effective horizontal overflow value, checking overflow-x then overflow.
inline std::string getOverflowX(const htmlayout::css::ComputedStyle& style) {
    auto oxIt = style.find("overflow-x");
    if (oxIt != style.end()) return oxIt->second;
    auto oIt = style.find("overflow");
    if (oIt != style.end()) return oIt->second;
    return "visible";
}

/// Does a value on either overflow axis clip the box?
///
/// Per CSS, when one axis is non-visible and the other is visible the visible
/// one is treated as `auto` — so if either axis clips, both effectively do.
inline bool overflowAxisClips(const std::string& v) {
    return v == "hidden" || v == "scroll" || v == "auto" || v == "clip";
}

// Whether `elem`'s overflow is propagated to the viewport (CSS 2.1 §11.1.1),
// so its own box does not clip.
bool overflowBelongsToViewport(dom::Element* elem);

// ---------------------------------------------------------------------------
// Lengths, radii, collapsed tables
// ---------------------------------------------------------------------------

// Parse a CSS length value (px, em, %) into pixels. Percentage is relative to ref.
inline float parseLengthPx(const std::string& val, float ref = 0) {
    if (val.empty()) return 0;
    char* end = nullptr;
    float v = std::strtof(val.c_str(), &end);
    if (end == val.c_str()) return 0;
    if (end && *end == '%') return v * ref / 100.0f;
    return v; // px or unitless
}

// Per-corner border radii for a box of (boxW, boxH), overlap-scaled.
render::Radii getRadii(const htmlayout::css::ComputedStyle& style,
                       float boxW, float boxH);

// Return true if `elem` is a table with `border-collapse: collapse`. The
// table's own borders are painted centered on its border-box outer edge to
// "win" against adjacent cell borders when the table border is thicker.
inline bool isCollapsedTable(dom::Element* elem) {
    if (!elem) return false;
    auto& cs = elem->computedStyle();
    auto dIt = cs.find("display");
    if (dIt == cs.end()) return false;
    const std::string& d = dIt->second;
    if (d != "table" && d != "inline-table") return false;
    auto bcIt = cs.find("border-collapse");
    return (bcIt != cs.end() && bcIt->second == "collapse");
}

// Read a length from the style map, treating empty/none as 0.
inline float styleLengthPx(const htmlayout::css::ComputedStyle& cs, const char* prop) {
    auto it = cs.find(prop);
    if (it == cs.end() || it->second.empty() || it->second == "none") return 0.0f;
    // Simple px parse — collapse-mode borders are always concrete lengths.
    char* end = nullptr;
    float v = std::strtof(it->second.c_str(), &end);
    return v;
}

} // namespace bro::layout
