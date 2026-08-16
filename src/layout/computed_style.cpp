// Implementation notes live in computed_style.h. Every rule below was written
// against Chromium's observed getComputedStyle output — the comments name the
// divergences from the CSSOM spec where Chrome itself diverges, because
// matching the spec and failing broparity would be the wrong trade.

#include "layout/computed_style.h"

#include "css/color.h"
#include "css/properties.h"
#include "layout/formatting_context.h"
#include "layout/skia_text_metrics.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace bro::layout {
namespace {

// Is this a CSS property whose computed value should be resolved to rgb()?
bool isColorProperty(const std::string& prop) {
    return prop == "color" || prop == "background-color" ||
           prop == "border-top-color" || prop == "border-right-color" ||
           prop == "border-bottom-color" || prop == "border-left-color" ||
           prop == "outline-color";
}

// Resolve a CSS color value to rgb(r, g, b) or rgba(r, g, b, a) notation.
// Returns the original value if it's already in rgb()/rgba() form or can't be parsed.
std::string resolveColorToRgb(const std::string& value) {
    if (value.empty() || value == "transparent")
        return "rgba(0, 0, 0, 0)";
    if (value == "currentcolor" || value == "currentColor" || value == "inherit")
        return value;

    auto c = htmlayout::css::parseColor(value);
    // parseColor returns {0,0,0,0} for unrecognized — if alpha is 0 and input
    // wasn't "transparent", it likely failed to parse
    if (c.a == 0 && c.r == 0 && c.g == 0 && c.b == 0 && value != "transparent")
        return value;

    if (c.a == 255) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rgb(%d, %d, %d)", c.r, c.g, c.b);
        return buf;
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %.2g)",
                 c.r, c.g, c.b, c.a / 255.0);
        return buf;
    }
}

}  // namespace

std::string computedProperty(dom::Element* el, const std::string& prop,
                             SkiaTextMetrics* metrics) {
    if (!el) return "";
    auto& style = el->computedStyle();
    std::string value;
    auto it = style.find(prop);
    if (it != style.end()) {
        value = it->second;
    } else {
        // Fall back to CSS initial value for known properties
        value = htmlayout::css::initialValue(prop);
    }

    // Resolve gap shorthand from longhands if not directly present
    if (prop == "gap" && (value == "normal" || value.empty())) {
        auto rg = style.find("row-gap");
        auto cg = style.find("column-gap");
        if (rg != style.end() && cg != style.end()) {
            if (rg->second == cg->second)
                return rg->second;
            return rg->second + " " + cg->second;
        } else if (rg != style.end()) {
            return rg->second;
        } else if (cg != style.end()) {
            return cg->second;
        }
    }

    // Resolve width/height to used values from layout box (matches Chrome
    // getComputedStyle). Always pull from the layout box rather than the
    // specified value so:
    //   - `box-sizing: border-box` reports the border-box dimension
    //     (Chrome behavior; CSSOM spec is content-box but Chrome diverges).
    //   - `auto` resolves to the used pixel size.
    //   - non-replaced inline elements still report "auto" since they have no
    //     CSS-meaningful width/height.
    if (prop == "width" || prop == "height") {
        // SVG geometry presentation attributes: SVG2 makes width/height on
        // shapes (rect, image, use, foreignObject…) real CSS geometry
        // properties, and Chromium's getComputedStyle reports them as px
        // lengths sourced from the attribute. bro renders SVG subtrees via
        // SkSVGDOM — the children have no layout boxes — so resolve straight
        // from the attribute for any element inside an <svg>.
        {
            bool insideSvg = false;
            for (auto* p = el->parentElement(); p; p = p->parentElement()) {
                const std::string& t = p->tagName();
                if (t == "svg" || t == "SVG") { insideSvg = true; break; }
            }
            if (insideSvg) {
                // Author CSS beats the presentation attribute in the
                // cascade; only fall back to the attribute when the
                // cascade left the property at its initial value.
                if (!value.empty() && value != "auto") return value;
                // width/height are geometry properties only on the shapes
                // SVG2 lists (rect, image, use, foreignObject, nested svg).
                // On pattern/mask/filter/gradient/… the attribute is plain
                // data, and Chromium reports the CSS initial value.
                std::string lt = el->tagName();
                for (auto& c : lt) c = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(c)));
                if (lt != "rect" && lt != "image" && lt != "use" &&
                    lt != "foreignobject" && lt != "svg") {
                    return value.empty() ? std::string("auto") : value;
                }
                const std::string& attr = el->getAttribute(prop);
                if (!attr.empty()) {
                    char* end = nullptr;
                    double n = std::strtod(attr.c_str(), &end);
                    if (end != attr.c_str() &&
                        (*end == '\0' || std::string_view(end) == "px")) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.6gpx", n);
                        return buf;
                    }
                    // Non-px units (%, em…): report the attribute as
                    // specified — no SVG viewport resolution here.
                    return attr;
                }
                // No geometry attribute: the property's initial value.
                return value.empty() ? std::string("auto") : value;
            }
        }
        auto dIt = style.find("display");
        std::string disp = (dIt != style.end()) ? dIt->second : "inline";
        const std::string& tag = el->tagName();
        bool isReplaced =
            tag == "img" || tag == "IMG" ||
            tag == "video" || tag == "VIDEO" ||
            tag == "canvas" || tag == "CANVAS" ||
            tag == "iframe" || tag == "IFRAME" ||
            tag == "input" || tag == "INPUT" ||
            tag == "embed" || tag == "EMBED" ||
            tag == "object" || tag == "OBJECT" ||
            tag == "svg" || tag == "SVG";
        if (disp == "inline" && !isReplaced) {
            return "auto";
        }
        // display:none / display:contents — no box is generated, so Chrome
        // returns the specified value, not a laid-out (zero) used value.
        if (disp == "none" || disp == "contents") {
            return value.empty() ? std::string("auto") : value;
        }
        auto& box = el->layoutBox();
        float v = (prop == "width")
            ? box.contentRect.width
            : box.contentRect.height;
        // For border-box, include padding + border on the relevant axis so
        // getComputedStyle reflects the box-sizing-aware dimension Chrome
        // exposes (and that broparity expects).
        auto bsIt = style.find("box-sizing");
        if (bsIt != style.end() && bsIt->second == "border-box") {
            if (prop == "width")
                v += box.padding.left + box.padding.right
                   + box.border.left + box.border.right;
            else
                v += box.padding.top + box.padding.bottom
                   + box.border.top + box.border.bottom;
        }
        if (v >= 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6gpx", v);
            return buf;
        }
    }

    // Resolve padding-* and margin-* to pixel values when the specified value
    // is `auto` or a percentage. Chrome reports these as resolved px while
    // length values stay as specified (so margin-collapse adjustments don't
    // leak into getComputedStyle output).
    if (prop == "padding-top" || prop == "padding-right" ||
        prop == "padding-bottom" || prop == "padding-left" ||
        prop == "margin-top" || prop == "margin-right" ||
        prop == "margin-bottom" || prop == "margin-left") {
        bool isPct = !value.empty() && value.back() == '%';
        bool isAuto = value == "auto";
        // calc() computes to a resolved px length too (Chromium reports
        // e.g. `margin-left: calc(10px - 30px)` as "-20px").
        bool isCalc = value.rfind("calc(", 0) == 0;
        if (isPct || isAuto || isCalc) {
            auto& box = el->layoutBox();
            float v = 0;
            if (prop == "padding-top")    v = box.padding.top;
            else if (prop == "padding-right")  v = box.padding.right;
            else if (prop == "padding-bottom") v = box.padding.bottom;
            else if (prop == "padding-left")   v = box.padding.left;
            else if (prop == "margin-top")     v = box.margin.top;
            else if (prop == "margin-right")   v = box.margin.right;
            else if (prop == "margin-bottom")  v = box.margin.bottom;
            else if (prop == "margin-left")    v = box.margin.left;
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6gpx", v);
            return buf;
        }
        // Absolutize font-relative and absolute-unit lengths (em, rem, pt…)
        // to px: the computed value of a margin/padding <length> is always
        // absolute (Chromium reports e.g. `margin: 1em` as "16px").
        bool endsPx = value.size() > 2 &&
            value.compare(value.size() - 2, 2, "px") == 0;
        bool hasAlphaUnit = !value.empty() &&
            std::isalpha(static_cast<unsigned char>(value.back()));
        if (hasAlphaUnit && !endsPx) {
            std::string fs;
            auto fsIt = style.find("font-size");
            if (fsIt != style.end()) fs = fsIt->second;
            float fontSize = htmlayout::layout::resolveLength(fs, 16.0f, 16.0f);
            if (fontSize <= 0) fontSize = 16.0f;
            float v = htmlayout::layout::resolveLength(value, 0.0f, fontSize);
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6gpx", v);
            return buf;
        }
    }

    // Resolve line-height to used px value (matches browser getComputedStyle behavior).
    // Per CSSOM, the resolved value of line-height is the used value in px,
    // except for "normal" which serializes as "normal".
    if (prop == "line-height" && !value.empty() && value != "normal") {
        // Get font-size from the same computed style (may itself need resolution)
        std::string fs;
        auto fsIt = style.find("font-size");
        if (fsIt != style.end()) fs = fsIt->second;
        float fontSize = htmlayout::layout::resolveLength(fs, 16.0f, 16.0f);
        if (fontSize <= 0) fontSize = 16.0f;

        SkiaTextMetrics* tm = metrics;

        std::string ff, fw;
        auto ffIt = style.find("font-family");
        if (ffIt != style.end()) ff = ffIt->second;
        auto fwIt = style.find("font-weight");
        if (fwIt != style.end()) fw = fwIt->second;

        float lh = tm
            ? htmlayout::layout::resolveLineHeight(value, fontSize, ff, fw, *tm)
            : htmlayout::layout::resolveLineHeight(value, fontSize);
        if (lh > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4gpx", lh);
            return buf;
        }
    }

    // Resolve colors to rgb() notation (matches browser getComputedStyle behavior)
    if (isColorProperty(prop))
        value = resolveColorToRgb(value);

    // Resolve font-weight keywords to numeric (matches Chrome)
    if (prop == "font-weight") {
        if (value == "normal") return "400";
        if (value == "bold") return "700";
    }

    // Resolve border-width to 0px when border-style is none
    if ((prop == "border-top-width" || prop == "border-right-width" ||
         prop == "border-bottom-width" || prop == "border-left-width") &&
        (value == "medium" || value == "thin" || value == "thick")) {
        // Check if the corresponding border-style is none
        std::string sideProp = prop.substr(0, prop.rfind("-width")) + "-style";
        auto sit = style.find(sideProp);
        std::string borderStyle = (sit != style.end()) ? sit->second : "none";
        if (borderStyle == "none" || borderStyle == "hidden")
            return "0px";
    }

    return value;
}
}  // namespace bro::layout
