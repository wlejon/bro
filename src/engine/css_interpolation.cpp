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

namespace {

// ---------------------------------------------------------------------------
// Component values: a value read as its top-level components, so every
// number and colour in it blends, not just the first.
// ---------------------------------------------------------------------------

struct Component {
    enum Kind { Number, Color, Function, Word, Comma } kind = Word;
    double number = 0;
    std::string unit;
    float rgba[4] = {0, 0, 0, 0};
    std::string name;   // function name
    std::string inner;  // function arguments
    std::string text;   // the component as written
};

bool startsNumber(const std::string& s, size_t i) {
    auto digitAt = [&](size_t k) {
        return k < s.size() && (std::isdigit(static_cast<unsigned char>(s[k])) || s[k] == '.');
    };
    if (i >= s.size()) return false;
    if (s[i] == '+' || s[i] == '-') return digitAt(i + 1) && (s[i + 1] != '.' || (i + 2 < s.size() &&
                                                    std::isdigit(static_cast<unsigned char>(s[i + 2]))));
    if (s[i] == '.') return i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]));
    return std::isdigit(static_cast<unsigned char>(s[i])) != 0;
}

bool tokenizeComponents(const std::string& v, std::vector<Component>& out) {
    out.clear();
    size_t i = 0;
    while (i < v.size()) {
        const char c = v[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        Component comp;
        if (c == ',') {
            comp.kind = Component::Comma;
            comp.text = ",";
            ++i;
        } else if (c == '"' || c == '\'') {
            const size_t end = v.find(c, i + 1);
            if (end == std::string::npos) return false;
            comp.text = v.substr(i, end + 1 - i);
            i = end + 1;
        } else if (startsNumber(v, i)) {
            char* end = nullptr;
            comp.kind = Component::Number;
            comp.number = std::strtod(v.c_str() + i, &end);
            size_t u = static_cast<size_t>(end - v.c_str());
            while (u < v.size() && (std::isalpha(static_cast<unsigned char>(v[u])) || v[u] == '%'))
                comp.unit += v[u++];
            comp.text = v.substr(i, u - i);
            i = u;
        } else {
            const size_t start = i;
            while (i < v.size() && !std::isspace(static_cast<unsigned char>(v[i])) && v[i] != ',' &&
                   v[i] != '(')
                ++i;
            if (i < v.size() && v[i] == '(') {
                comp.kind = Component::Function;
                comp.name = v.substr(start, i - start);
                int depth = 0;
                const size_t open = i;
                for (; i < v.size(); ++i) {
                    if (v[i] == '(') ++depth;
                    else if (v[i] == ')' && --depth == 0) break;
                }
                if (i >= v.size()) return false;
                comp.inner = v.substr(open + 1, i - open - 1);
                ++i;
            }
            comp.text = v.substr(start, i - start);
            if (comp.text.empty()) return false;
            float r, g, b, a;
            if (tryParseColorComponents(comp.text, r, g, b, a)) {
                comp.kind = Component::Color;
                comp.rgba[0] = r; comp.rgba[1] = g; comp.rgba[2] = b; comp.rgba[3] = a;
            }
        }
        out.push_back(std::move(comp));
    }
    return true;
}

std::string formatNumber(double v) {
    std::ostringstream oss;
    oss << static_cast<float>(v);
    return oss.str();
}

// A unit both ends can blend in: the same one, or a unitless zero's partner.
bool joinUnits(const Component& a, const Component& b, std::string& unit) {
    if (a.unit == b.unit) { unit = a.unit; return true; }
    if (a.unit.empty() && a.number == 0) { unit = b.unit; return true; }
    if (b.unit.empty() && b.number == 0) { unit = a.unit; return true; }
    return false;
}

// Colours blend premultiplied (CSS Color 4 §12.3), so a fade from
// `transparent` does not pass through its black.
std::string blendColors(const float* a, const float* b, float t) {
    const float alpha = std::clamp(a[3] + (b[3] - a[3]) * t, 0.0f, 1.0f);
    if (alpha <= 0.0f) return colorToRGBA(0, 0, 0, 0);
    float c[3];
    for (int i = 0; i < 3; ++i) {
        const float pa = a[i] * a[3], pb = b[i] * b[3];
        c[i] = (pa + (pb - pa) * t) / alpha;
    }
    return colorToRGBA(c[0], c[1], c[2], alpha);
}

// Blend two values of the same shape component by component: numbers of
// joinable units, colours, same-named functions (recursively), and equal
// keywords, commas and strings. False when the shapes differ.
bool interpolateComponents(const std::string& from, const std::string& to, float t,
                           std::string& out, int depth = 0) {
    if (depth > 8) return false;
    std::vector<Component> a, b;
    if (!tokenizeComponents(from, a) || !tokenizeComponents(to, b) || a.size() != b.size() ||
        a.empty())
        return false;
    std::string result;
    for (size_t i = 0; i < a.size(); ++i) {
        const Component& x = a[i];
        const Component& y = b[i];
        if (x.kind != y.kind) return false;
        std::string piece;
        switch (x.kind) {
            case Component::Comma:
                result += ",";
                continue;
            case Component::Number: {
                std::string unit;
                if (!joinUnits(x, y, unit)) return false;
                piece = formatNumber(x.number + (y.number - x.number) * t) + unit;
                break;
            }
            case Component::Color:
                piece = blendColors(x.rgba, y.rgba, t);
                break;
            case Component::Function:
                if (x.text == y.text) piece = x.text;
                else if (x.name != y.name) return false;
                else {
                    std::string inner;
                    if (!interpolateComponents(x.inner, y.inner, t, inner, depth + 1)) return false;
                    piece = x.name + "(" + inner + ")";
                }
                break;
            case Component::Word:
                if (x.text != y.text) return false;
                piece = x.text;
                break;
        }
        if (!result.empty() && result.back() != '(') result += ' ';
        result += piece;
    }
    out = std::move(result);
    return true;
}

// ---------------------------------------------------------------------------
// Shadow lists (box-shadow, text-shadow): each shadow blends its offsets,
// blur, spread and colour; the shorter list is padded with transparent
// shadows (CSS Backgrounds 3 §"Interpolation of shadows").
// ---------------------------------------------------------------------------

struct Shadow {
    bool inset = false;
    Component lengths[4];
    int lengthCount = 0;
    bool hasColor = false;
    Component color;  // a Color component, or a keyword (currentcolor)
};

bool parseShadow(const std::string& item, Shadow& s) {
    std::vector<Component> comps;
    if (!tokenizeComponents(item, comps)) return false;
    for (auto& c : comps) {
        if (c.kind == Component::Number) {
            if (s.lengthCount >= 4) return false;
            s.lengths[s.lengthCount++] = c;
        } else if (c.kind == Component::Word && c.text == "inset") {
            s.inset = true;
        } else if (c.kind == Component::Color || c.kind == Component::Word) {
            if (s.hasColor) return false;
            s.hasColor = true;
            s.color = c;
        } else {
            return false;
        }
    }
    if (s.lengthCount < 2) return false;
    for (int i = s.lengthCount; i < 4; ++i) {
        s.lengths[i].kind = Component::Number;
        s.lengths[i].number = 0;
    }
    return true;
}

Shadow transparentShadow(bool inset) {
    Shadow s;
    s.inset = inset;
    s.lengthCount = 4;
    for (auto& l : s.lengths) l.kind = Component::Number;
    s.hasColor = true;
    s.color.kind = Component::Color;
    s.color.text = "transparent";
    return s;
}

bool interpolateShadowLists(const std::string& from, const std::string& to, float t,
                            bool textShadow, std::string& out) {
    auto parseList = [](const std::string& v, std::vector<Shadow>& list) {
        list.clear();
        if (v.empty() || v == "none") return true;
        for (const std::string& item : splitCSS(v)) {
            Shadow s;
            if (!parseShadow(item, s)) return false;
            list.push_back(s);
        }
        return true;
    };
    std::vector<Shadow> a, b;
    if (!parseList(from, a) || !parseList(to, b)) return false;
    if (a.empty() && b.empty()) { out = "none"; return true; }
    while (a.size() < b.size()) a.push_back(transparentShadow(b[a.size()].inset));
    while (b.size() < a.size()) b.push_back(transparentShadow(a[b.size()].inset));

    std::string result;
    for (size_t i = 0; i < a.size(); ++i) {
        const Shadow& x = a[i];
        const Shadow& y = b[i];
        if (x.inset != y.inset) return false;
        std::string s = x.inset ? "inset " : "";
        const int n = textShadow ? 3 : 4;
        for (int k = 0; k < n; ++k) {
            std::string unit;
            if (!joinUnits(x.lengths[k], y.lengths[k], unit)) return false;
            if (unit.empty()) unit = "px";
            s += formatNumber(x.lengths[k].number + (y.lengths[k].number - x.lengths[k].number) * t) +
                 unit + " ";
        }
        // Colours: both real colours blend; a missing colour is currentcolor,
        // which this value-only interpolator cannot resolve, so that pair
        // takes the nearer end's colour.
        if (x.hasColor && y.hasColor && x.color.kind == Component::Color &&
            y.color.kind == Component::Color) {
            s += blendColors(x.color.rgba, y.color.rgba, t);
        } else {
            const Shadow& near = t < 0.5f ? x : y;
            s += near.hasColor ? near.color.text : "currentcolor";
        }
        if (!result.empty()) result += ", ";
        result += s;
    }
    out = std::move(result);
    return true;
}

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

// A single number each side in units that do not join (`10px` → `2em`):
// the old interpolator's blend of the numbers in the target's unit, kept
// until lengths of mixed units blend through calc().
bool mixedUnitNumbers(const std::string& from, const std::string& to, float t, std::string& out) {
    std::vector<Component> a, b;
    if (!tokenizeComponents(from, a) || !tokenizeComponents(to, b) || a.size() != 1 ||
        b.size() != 1 || a[0].kind != Component::Number || b[0].kind != Component::Number)
        return false;
    out = formatNumber(a[0].number + (b[0].number - a[0].number) * t) + b[0].unit;
    return true;
}

// The interpolated value when the pair blends at all (not the discrete
// fallback), for both isInterpolable and interpolate.
bool blendValues(const std::string& fromIn, const std::string& toIn, float t,
                 const std::string& property, std::string& out) {
    if (property == "transform") return interpolateTransformLists(fromIn, toIn, t, out);
    if (property == "box-shadow" || property == "text-shadow")
        return interpolateShadowLists(fromIn, toIn, t, property == "text-shadow", out);
    std::string from = fromIn, to = toIn;
    substituteNoneIdentity(from, to, property);
    if (interpolateComponents(from, to, t, out) || mixedUnitNumbers(from, to, t, out)) {
        // An overshooting easing extrapolates past the endpoints; opacity
        // is still a [0,1] value (its computed value is clamped).
        if (property == "opacity") {
            const float v = std::clamp(std::strtof(out.c_str(), nullptr), 0.0f, 1.0f);
            out = formatNumber(v);
        }
        return true;
    }
    return false;
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
    // matrices, shadow lists padded with transparent shadows); the few shapes
    // this interpolator still flips (percentages under a matrix blend, inset
    // against outset) are transitionable all the same, so they run without
    // allow-discrete.
    if (property == "transform" || property == "box-shadow" || property == "text-shadow")
        return true;
    std::string out;
    return blendValues(from, to, 0.5f, property, out);
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

    // Every number and colour in the value blends; a transform list pair of
    // different shapes blends through matrices; shadow lists pad.
    std::string out;
    if (blendValues(fromIn, toIn, t, property, out)) return out;

    // Non-interpolable: snap at 50%
    return t < 0.5f ? fromIn : toIn;
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
