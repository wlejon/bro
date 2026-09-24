#include "engine/css_interpolation.h"
#include "dom/element.h"
#include "css/color.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace bro::engine {

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

namespace {

bool isFunctionListProperty(const std::string& property) {
    return property == "transform" || property == "filter" || property == "backdrop-filter";
}

// `none` in a transform or filter list interpolates as the identity list
// shaped like the other side ("none" → "rotate(90deg)" runs from rotate(0deg)).
void substituteNoneIdentity(std::string& from, std::string& to, const std::string& property) {
    if (!isFunctionListProperty(property)) return;
    if (from == "none" && to != "none") {
        std::string id = identityTransform(to);
        if (!id.empty()) from = std::move(id);
    } else if (to == "none" && from != "none") {
        std::string id = identityTransform(from);
        if (!id.empty()) to = std::move(id);
    }
}

bool bothNumeric(const std::string& from, const std::string& to) {
    char* endA = nullptr;
    char* endB = nullptr;
    std::strtof(from.c_str(), &endA);
    std::strtof(to.c_str(), &endB);
    return endA != from.c_str() && endB != to.c_str();
}

bool compatibleFunctionLists(const std::string& from, const std::string& to) {
    std::vector<CSSFunc> a, b;
    if (!parseCSSFunctions(from, a) || !parseCSSFunctions(to, b) || a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].args.size() != b[i].args.size()) return false;
    return true;
}

}  // namespace

bool TransitionManager::isInterpolable(const std::string& from, const std::string& to,
                                       const std::string& property) {
    // Discrete by animation type (CSS Display 4: display animates discretely,
    // none-aware).
    if (property == "display") return false;
    // visibility interpolates when one end is visible (it stays visible
    // throughout); hidden ↔ collapse is a plain discrete flip.
    if (property == "visibility") return from == "visible" || to == "visible";
    // Lists the spec interpolates in every case (a transform list pair through
    // matrices, shadow lists padded with transparent shadows); this
    // interpolator approximates the shapes it cannot blend with a flip, but
    // they are transitionable, so they run without allow-discrete.
    if (property == "transform" || property == "box-shadow" || property == "text-shadow")
        return true;
    std::string a = from, b = to;
    substituteNoneIdentity(a, b, property);
    if (bothNumeric(a, b)) return true;
    float r, g, bl, al;
    if (tryParseColorComponents(a, r, g, bl, al) && tryParseColorComponents(b, r, g, bl, al))
        return true;
    return compatibleFunctionLists(a, b);
}

std::string TransitionManager::interpolate(const std::string& fromIn, const std::string& toIn,
                                           float t, const std::string& property) {
    // The discrete types with a none-aware rule: between the endpoints (0 < p
    // < 1) the value is the visible one; at or past an endpoint, that
    // endpoint (CSS Display 4 for display, CSS Transitions for visibility).
    if (property == "display" || property == "visibility") {
        const char* shown = property == "display" ? nullptr : "visible";
        const bool fromSpecial = shown ? fromIn == shown : fromIn == "none";
        const bool toSpecial = shown ? toIn == shown : toIn == "none";
        if (fromSpecial != toSpecial) {
            if (t <= 0.0f) return fromIn;
            if (t >= 1.0f) return toIn;
            // display: the value that is not none; visibility: visible.
            if (shown) return shown;
            return fromSpecial ? toIn : fromIn;
        }
        return t < 0.5f ? fromIn : toIn;
    }

    std::string from = fromIn, to = toIn;
    substituteNoneIdentity(from, to, property);

    // Try numeric interpolation first (handles px, em, %, unitless)
    {
        char* endA = nullptr;
        char* endB = nullptr;
        float a = std::strtof(from.c_str(), &endA);
        float b = std::strtof(to.c_str(), &endB);
        if (endA != from.c_str() && endB != to.c_str()) {
            float v = a + (b - a) * t;
            // An overshooting easing extrapolates past the endpoints; opacity
            // is still a [0,1] value (its computed value is clamped).
            if (property == "opacity") v = std::clamp(v, 0.0f, 1.0f);
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
            float a = std::clamp(a1 + (a2 - a1) * t, 0.0f, 1.0f);
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
        // translate*, rotate*, skew*, and the filters blur / grayscale /
        // sepia / invert / hue-rotate → 0; scales and the multiplicative
        // filters → 1.
        float identity = 0.0f;
        if (fn.name == "scale" || fn.name == "scaleX" || fn.name == "scaleY" ||
            fn.name == "scaleZ" || fn.name == "scale3d" || fn.name == "brightness" ||
            fn.name == "contrast" || fn.name == "saturate" || fn.name == "opacity")
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

} // namespace bro::engine
