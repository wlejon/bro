#include "engine/css_interpolation.h"
#include "dom/element.h"
#include "css/color.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace bro::engine {

using bromath::CubicEase;
using bromath::ccubicEase;

// ---------------------------------------------------------------------------
// CSS timing-function presets (control points for bromath::CubicEase)
// ---------------------------------------------------------------------------
// Endpoints (0,0) and (1,1) are implicit. `linear` is a degenerate case
// (CPs colinear with the endpoints) — ccubicEase returns the input.


CubicEase parseTimingFunction(const std::string& val) {
    if (val.empty() || val == "ease") return kEase;
    if (val == "linear") return kLinear;
    if (val == "ease-in") return kEaseIn;
    if (val == "ease-out") return kEaseOut;
    if (val == "ease-in-out") return kEaseInOut;

    // cubic-bezier(x1, y1, x2, y2)
    auto pos = val.find("cubic-bezier(");
    if (pos != std::string::npos) {
        const char* p = val.c_str() + pos + 13;
        char* end = nullptr;
        float x1 = std::strtof(p, &end); p = end; while (*p == ',' || *p == ' ') ++p;
        float y1 = std::strtof(p, &end); p = end; while (*p == ',' || *p == ' ') ++p;
        float x2 = std::strtof(p, &end); p = end; while (*p == ',' || *p == ' ') ++p;
        float y2 = std::strtof(p, &end);
        return {x1, y1, x2, y2};
    }

    return kEase;
}

// ---------------------------------------------------------------------------
// Value interpolation
// ---------------------------------------------------------------------------

// Try to parse a CSS color string into RGBA components.
// Any colour the cascade accepts: hex, named, rgb()/hsl() in both syntaxes,
// the CSS Color 4 functions and wide-gamut color() (gamut-mapped to sRGB).
// `currentcolor` is not a colour here (it interpolates discretely), and
// light-dark() reaches this point already resolved by the restyle pass.
static bool tryParseColorComponents(const std::string& s, float& r, float& g, float& b, float& a) {
    if (s.empty()) return false;
    if (s.size() == 12) {
        std::string lower = s;
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lower == "currentcolor") return false;
    }
    htmlayout::css::Color c;
    if (!htmlayout::css::tryParseColor(s, c)) return false;
    r = c.r / 255.0f;
    g = c.g / 255.0f;
    b = c.b / 255.0f;
    a = c.a / 255.0f;
    return true;
}

static std::string colorToRGBA(float r, float g, float b, float a) {
    int ri = std::clamp(static_cast<int>(r * 255 + 0.5f), 0, 255);
    int gi = std::clamp(static_cast<int>(g * 255 + 0.5f), 0, 255);
    int bi = std::clamp(static_cast<int>(b * 255 + 0.5f), 0, 255);
    if (a >= 1.0f)
        return "rgb(" + std::to_string(ri) + ", " + std::to_string(gi) + ", " + std::to_string(bi) + ")";
    std::ostringstream oss;
    oss << "rgba(" << ri << ", " << gi << ", " << bi << ", " << a << ")";
    return oss.str();
}

// Parse a CSS function call like "rotate(30deg)" into name + numeric args.
// Returns false if the string doesn't look like func(...).
struct CSSFunc {
    std::string name;
    std::vector<float> args;
    std::vector<std::string> argUnits; // unit suffix for each arg
};

static bool parseCSSFunctions(const std::string& val, std::vector<CSSFunc>& out) {
    out.clear();
    size_t pos = 0;
    while (pos < val.size()) {
        while (pos < val.size() && (val[pos] == ' ' || val[pos] == '\t'))
            ++pos;
        if (pos >= val.size()) break;

        size_t nameStart = pos;
        while (pos < val.size() && val[pos] != '(' && val[pos] != ' ')
            ++pos;
        if (pos >= val.size() || val[pos] != '(') return false;
        std::string func = val.substr(nameStart, pos - nameStart);
        ++pos; // skip '('

        CSSFunc cf;
        cf.name = func;

        // Parse args until ')'
        while (pos < val.size() && val[pos] != ')') {
            while (pos < val.size() && (val[pos] == ' ' || val[pos] == ',' || val[pos] == '\t'))
                ++pos;
            if (pos >= val.size() || val[pos] == ')') break;
            char* end = nullptr;
            float v = std::strtof(val.c_str() + pos, &end);
            if (end == val.c_str() + pos) return false; // not a number
            size_t uStart = static_cast<size_t>(end - val.c_str());
            std::string unit;
            while (uStart < val.size() && (std::isalpha(static_cast<unsigned char>(val[uStart])) || val[uStart] == '%'))
                unit += val[uStart++];
            cf.args.push_back(v);
            cf.argUnits.push_back(unit);
            pos = uStart;
        }
        if (pos < val.size()) ++pos; // skip ')'

        out.push_back(std::move(cf));
    }
    return !out.empty();
}

static std::string interpolateCSSFunctions(const std::vector<CSSFunc>& a,
                                           const std::vector<CSSFunc>& b,
                                           float t) {
    std::ostringstream oss;
    for (size_t i = 0; i < a.size(); ++i) {
        if (i > 0) oss << " ";
        oss << a[i].name << "(";
        size_t nArgs = a[i].args.size();
        for (size_t j = 0; j < nArgs; ++j) {
            if (j > 0) oss << ", ";
            float v = a[i].args[j] + (b[i].args[j] - a[i].args[j]) * t;
            oss << v;
            if (!b[i].argUnits[j].empty()) oss << b[i].argUnits[j];
            else if (!a[i].argUnits[j].empty()) oss << a[i].argUnits[j];
        }
        oss << ")";
    }
    return oss.str();
}

std::string TransitionManager::interpolate(const std::string& from, const std::string& to,
                                           float t, const std::string& property) {
    // Try numeric interpolation first (handles px, em, %, unitless)
    {
        char* endA = nullptr;
        char* endB = nullptr;
        float a = std::strtof(from.c_str(), &endA);
        float b = std::strtof(to.c_str(), &endB);
        if (endA != from.c_str() && endB != to.c_str()) {
            float v = a + (b - a) * t;
            // Preserve unit from target
            std::string unit(endB);
            // Clean up float formatting
            std::ostringstream oss;
            oss << v;
            return oss.str() + unit;
        }
    }

    // Try color interpolation
    {
        float r1, g1, b1, a1, r2, g2, b2, a2;
        if (tryParseColorComponents(from, r1, g1, b1, a1) &&
            tryParseColorComponents(to, r2, g2, b2, a2)) {
            float r = r1 + (r2 - r1) * t;
            float g = g1 + (g2 - g1) * t;
            float b = b1 + (b2 - b1) * t;
            float a = a1 + (a2 - a1) * t;
            return colorToRGBA(r, g, b, a);
        }
    }

    // Try CSS function interpolation (transforms, filters, etc.)
    // e.g. "rotate(0deg)" → "rotate(360deg)", "scale(1)" → "scale(1.5)"
    {
        std::vector<CSSFunc> funcsA, funcsB;
        if (parseCSSFunctions(from, funcsA) && parseCSSFunctions(to, funcsB) &&
            funcsA.size() == funcsB.size()) {
            bool compatible = true;
            for (size_t i = 0; i < funcsA.size(); ++i) {
                if (funcsA[i].name != funcsB[i].name ||
                    funcsA[i].args.size() != funcsB[i].args.size()) {
                    compatible = false;
                    break;
                }
            }
            if (compatible) {
                return interpolateCSSFunctions(funcsA, funcsB, t);
            }
        }
    }

    // Non-interpolable: snap at 50%
    return t < 0.5f ? from : to;
}

// ---------------------------------------------------------------------------
// Parse transition shorthand properties
// ---------------------------------------------------------------------------

// Parse a duration string (e.g., "0.3s", "300ms") to milliseconds.
double parseDurationMs(const std::string& val) {
    if (val.empty()) return 0;
    char* end = nullptr;
    double v = std::strtod(val.c_str(), &end);
    std::string unit(end);
    if (unit.find("ms") != std::string::npos) return v;
    return v * 1000.0; // seconds → ms
}

// Split a comma-separated CSS value list, respecting parentheses.
std::vector<std::string> splitCSS(const std::string& val) {
    std::vector<std::string> result;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= val.size(); ++i) {
        if (i < val.size() && val[i] == '(') ++depth;
        else if (i < val.size() && val[i] == ')') --depth;
        else if ((i == val.size() || val[i] == ',') && depth <= 0) {
            std::string s = val.substr(start, i - start);
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t");
            if (a != std::string::npos)
                result.push_back(s.substr(a, b - a + 1));
            else
                result.push_back("");
            start = i + 1;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// CSS initial value defaults for transitioning from "nothing"
// ---------------------------------------------------------------------------

// Build an identity transform string matching the structure of the target
// value, e.g. "scale(1.4)" → "scale(1)", "rotate(180deg)" → "rotate(0deg)".
std::string identityTransform(const std::string& target) {
    std::vector<CSSFunc> funcs;
    if (!parseCSSFunctions(target, funcs)) return "";
    std::ostringstream oss;
    for (size_t i = 0; i < funcs.size(); ++i) {
        if (i > 0) oss << " ";
        auto& fn = funcs[i];
        oss << fn.name << "(";
        // Determine identity value per function name
        float identity = 0.0f; // translateX, translateY, rotate, skew → 0
        if (fn.name == "scale" || fn.name == "scaleX" || fn.name == "scaleY" ||
            fn.name == "scaleZ" || fn.name == "scale3d")
            identity = 1.0f;
        for (size_t j = 0; j < fn.args.size(); ++j) {
            if (j > 0) oss << ", ";
            oss << identity;
            if (!fn.argUnits[j].empty()) oss << fn.argUnits[j];
        }
        oss << ")";
    }
    return oss.str();
}

// Return the CSS initial value for a property so transitions from an
// absent/empty value can interpolate.  `newVal` is used to match the
// structure of transform functions.
std::string initialValueForProperty(const std::string& prop,
                                    const std::string& newVal) {
    if (prop == "transform") return identityTransform(newVal);
    if (prop == "opacity") return "1";
    if (prop == "border-radius" || prop == "border-top-left-radius" ||
        prop == "border-top-right-radius" || prop == "border-bottom-left-radius" ||
        prop == "border-bottom-right-radius")
        return "0px";
    if (prop == "box-shadow") return "0 0 0 rgba(0, 0, 0, 0)";
    if (prop == "filter") return "none";
    if (prop == "margin" || prop == "margin-top" || prop == "margin-right" ||
        prop == "margin-bottom" || prop == "margin-left" ||
        prop == "padding" || prop == "padding-top" || prop == "padding-right" ||
        prop == "padding-bottom" || prop == "padding-left" ||
        prop == "top" || prop == "right" || prop == "bottom" || prop == "left")
        return "0px";
    return "";
}

std::string cssInitialValueForProperty(const std::string& prop,
                                       const std::string& refValue) {
    return initialValueForProperty(prop, refValue);
}

void applyKeyframeInterpolation(const htmlayout::css::KeyframeBlock* kf,
                                const Animation& anim,
                                double currentTime,
                                htmlayout::css::ComputedStyle& style) {
    if (!kf || kf->stops.empty()) return;

    double elapsed = anim.effectiveTime(currentTime) - anim.startTime - anim.delay;
    if (elapsed < 0) {
        // In delay period — apply backwards fill if applicable
        if (anim.fillMode != "backwards" && anim.fillMode != "both") return;
        elapsed = 0;
    }

    // Compute iteration and progress
    double iterProgress = elapsed / anim.duration;
    int currentIter = static_cast<int>(iterProgress);
    float localProgress = static_cast<float>(iterProgress - currentIter);

    // Clamp to iteration count
    if (anim.iterationCount >= 0 && currentIter >= anim.iterationCount) {
        if (anim.fillMode == "forwards" || anim.fillMode == "both") {
            currentIter = anim.iterationCount - 1;
            localProgress = 1.0f;
        } else {
            return;
        }
    }

    // Handle direction
    bool thisIterReverse = anim.reverse;
    if (anim.alternate && (currentIter % 2 != 0))
        thisIterReverse = !thisIterReverse;
    if (thisIterReverse)
        localProgress = 1.0f - localProgress;

    // Apply easing
    localProgress = ccubicEase(anim.easing, localProgress);

    // Find bracketing keyframe stops. When the @keyframes omits a 0% or
    // 100% stop, CSS synthesizes an implicit endpoint from the element's
    // *base* (un-animated) value — that is what makes a one-sided rule like
    //   @keyframes spin { to { transform: rotate(360deg); } }
    // actually interpolate rotate(0deg)→rotate(360deg) and spin. Without it,
    // front()==back() collapses the segment and the value stays constant.
    float t = std::clamp(localProgress, 0.0f, 1.0f);
    const auto& stops = kf->stops;

    // Union of properties this animation touches (for implicit endpoints).
    std::unordered_set<std::string> animProps;
    for (auto& stop : stops)
        for (auto& d : stop.declarations) animProps.insert(d.property);

    // Base (un-animated) value for a property, shaped to the opposite
    // endpoint so transform identities match (rotate→rotate(0deg), etc.).
    auto baseValueFor = [&](const std::string& prop,
                            const std::string& ref) -> std::string {
        auto sIt = style.find(prop);
        if (sIt != style.end() && !sIt->second.empty() && sIt->second != "none")
            return sIt->second;
        std::string iv = initialValueForProperty(prop, ref);
        return iv.empty() ? ref : iv;
    };

    const htmlayout::css::KeyframeStop* beforeStop = nullptr;
    const htmlayout::css::KeyframeStop* afterStop = nullptr;
    float beforeOffset = 0.0f, afterOffset = 1.0f;
    bool beforeImplicit = false, afterImplicit = false;

    if (t <= stops.front().offset) {
        if (stops.front().offset <= 0.0001f) {
            beforeStop = afterStop = &stops.front();
            beforeOffset = afterOffset = stops.front().offset;
        } else {
            // No 0% stop: implicit-from (base) → first real stop.
            beforeImplicit = true;
            afterStop = &stops.front();
            afterOffset = stops.front().offset;
        }
    } else if (t >= stops.back().offset) {
        if (stops.back().offset >= 0.9999f) {
            beforeStop = afterStop = &stops.back();
            beforeOffset = afterOffset = stops.back().offset;
        } else {
            // No 100% stop: last real stop → implicit-to (base).
            beforeStop = &stops.back();
            beforeOffset = stops.back().offset;
            afterImplicit = true;
        }
    } else {
        for (size_t i = 0; i + 1 < stops.size(); ++i) {
            if (t >= stops[i].offset && t <= stops[i + 1].offset) {
                beforeStop = &stops[i];     beforeOffset = stops[i].offset;
                afterStop  = &stops[i + 1]; afterOffset  = stops[i + 1].offset;
                break;
            }
        }
    }

    float segmentRange = afterOffset - beforeOffset;
    float segmentT = segmentRange > 0 ? (t - beforeOffset) / segmentRange : 0.0f;

    // Build property maps for the two endpoints, filling implicit endpoints
    // from the element's base value.
    std::unordered_map<std::string, std::string> beforeProps, afterProps;
    if (beforeStop)
        for (auto& d : beforeStop->declarations) beforeProps[d.property] = d.value;
    if (afterStop)
        for (auto& d : afterStop->declarations) afterProps[d.property] = d.value;
    if (beforeImplicit)
        for (auto& p : animProps) {
            auto aIt = afterProps.find(p);
            beforeProps[p] = baseValueFor(p, aIt != afterProps.end()
                                                 ? aIt->second : std::string());
        }
    if (afterImplicit)
        for (auto& p : animProps) {
            auto bIt = beforeProps.find(p);
            afterProps[p] = baseValueFor(p, bIt != beforeProps.end()
                                                ? bIt->second : std::string());
        }

    // Interpolate each property present in either stop
    std::unordered_set<std::string> allProps;
    for (auto& [k, v] : beforeProps) allProps.insert(k);
    for (auto& [k, v] : afterProps) allProps.insert(k);

    for (auto& prop : allProps) {
        auto bIt = beforeProps.find(prop);
        auto aIt = afterProps.find(prop);
        if (bIt != beforeProps.end() && aIt != afterProps.end()) {
            style[prop] = TransitionManager::interpolate(bIt->second, aIt->second,
                                                         segmentT, prop);
        } else if (aIt != afterProps.end()) {
            // Only in after — snap at start of segment
            style[prop] = aIt->second;
        } else {
            style[prop] = bIt->second;
        }
    }
}

} // namespace bro::engine
