#include "engine/css_transitions.h"
#include "dom/element.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace bro::engine {

using bromath::CubicEase;
using bromath::ccubicEase;

// ---------------------------------------------------------------------------
// CSS timing-function presets (control points for bromath::CubicEase)
// ---------------------------------------------------------------------------
// Endpoints (0,0) and (1,1) are implicit. `linear` is a degenerate case
// (CPs colinear with the endpoints) — ccubicEase returns the input.

static constexpr CubicEase kLinear    {0.0f,  0.0f,  1.0f,  1.0f};
static constexpr CubicEase kEase      {0.25f, 0.1f,  0.25f, 1.0f};
static constexpr CubicEase kEaseIn    {0.42f, 0.0f,  1.0f,  1.0f};
static constexpr CubicEase kEaseOut   {0.0f,  0.0f,  0.58f, 1.0f};
static constexpr CubicEase kEaseInOut {0.42f, 0.0f,  0.58f, 1.0f};

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
static bool tryParseColorComponents(const std::string& s, float& r, float& g, float& b, float& a) {
    if (s.empty()) return false;

    // Try rgb(r,g,b) / rgba(r,g,b,a)
    if (s.substr(0, 4) == "rgba" || s.substr(0, 3) == "rgb") {
        auto paren = s.find('(');
        if (paren == std::string::npos) return false;
        const char* p = s.c_str() + paren + 1;
        char* end;
        r = std::strtof(p, &end) / 255.0f; p = end; while (*p == ',' || *p == ' ') ++p;
        g = std::strtof(p, &end) / 255.0f; p = end; while (*p == ',' || *p == ' ') ++p;
        b = std::strtof(p, &end) / 255.0f; p = end; while (*p == ',' || *p == ' ' || *p == '/') ++p;
        a = (*p && *p != ')') ? std::strtof(p, &end) : 1.0f;
        return true;
    }

    // Try #hex
    if (s[0] == '#') {
        unsigned int hex = 0;
        if (s.size() == 7) { // #RRGGBB
            hex = std::strtoul(s.c_str() + 1, nullptr, 16);
            r = ((hex >> 16) & 0xFF) / 255.0f;
            g = ((hex >> 8) & 0xFF) / 255.0f;
            b = (hex & 0xFF) / 255.0f;
            a = 1.0f;
            return true;
        }
        if (s.size() == 4) { // #RGB
            hex = std::strtoul(s.c_str() + 1, nullptr, 16);
            r = ((hex >> 8) & 0xF) / 15.0f;
            g = ((hex >> 4) & 0xF) / 15.0f;
            b = (hex & 0xF) / 15.0f;
            a = 1.0f;
            return true;
        }
    }

    return false;
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
static double parseDurationMs(const std::string& val) {
    if (val.empty()) return 0;
    char* end = nullptr;
    double v = std::strtod(val.c_str(), &end);
    std::string unit(end);
    if (unit.find("ms") != std::string::npos) return v;
    return v * 1000.0; // seconds → ms
}

// Split a comma-separated CSS value list, respecting parentheses.
static std::vector<std::string> splitCSS(const std::string& val) {
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
static std::string identityTransform(const std::string& target) {
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
static std::string initialValueForProperty(const std::string& prop,
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

// ---------------------------------------------------------------------------
// TransitionManager
// ---------------------------------------------------------------------------

void TransitionManager::onStyleChange(dom::Element* elem,
                                      const htmlayout::css::ComputedStyle& oldStyle,
                                      htmlayout::css::ComputedStyle& newStyle,
                                      double currentTime) {
    // Check for transition declarations.
    // Try longhand first, then fall back to parsing the transition shorthand.
    std::vector<std::string> properties, durations, timingFuncs, delays;

    auto tpIt = newStyle.find("transition-property");
    auto tdIt = newStyle.find("transition-duration");
    if (tpIt != newStyle.end() && tdIt != newStyle.end() &&
        tpIt->second != "none" && tdIt->second != "0s") {
        properties = splitCSS(tpIt->second);
        durations = splitCSS(tdIt->second);
        auto tfIt = newStyle.find("transition-timing-function");
        if (tfIt != newStyle.end()) timingFuncs = splitCSS(tfIt->second);
        auto delIt = newStyle.find("transition-delay");
        if (delIt != newStyle.end()) delays = splitCSS(delIt->second);
    } else {
        // Try the shorthand: transition: <property> <duration> [<timing>] [<delay>]
        auto trIt = newStyle.find("transition");
        if (trIt == newStyle.end() || trIt->second.empty() || trIt->second == "none")
            return;
        // Parse each comma-separated transition
        auto parts = splitCSS(trIt->second);
        for (auto& part : parts) {
            // Tokenize respecting parentheses so cubic-bezier(...) stays intact.
            std::vector<std::string> tokens;
            {
                size_t i = 0;
                while (i < part.size()) {
                    while (i < part.size() && (part[i] == ' ' || part[i] == '\t')) ++i;
                    if (i >= part.size()) break;
                    size_t start = i;
                    int depth = 0;
                    while (i < part.size() && (depth > 0 || (part[i] != ' ' && part[i] != '\t'))) {
                        if (part[i] == '(') ++depth;
                        else if (part[i] == ')') --depth;
                        ++i;
                    }
                    tokens.push_back(part.substr(start, i - start));
                }
            }
            std::string prop = "all";
            std::string dur = "0s";
            std::string timing = "ease";
            std::string delay = "0s";
            int numIdx = 0;
            for (auto& tok : tokens) {
                // Check if it's a duration/delay (contains 's' or 'ms')
                char* end = nullptr;
                std::strtof(tok.c_str(), &end);
                bool isTime = (end != tok.c_str() &&
                    (std::string(end) == "s" || std::string(end) == "ms"));
                if (isTime) {
                    if (numIdx == 0) { dur = tok; ++numIdx; }
                    else { delay = tok; ++numIdx; }
                } else if (tok == "ease" || tok == "linear" || tok == "ease-in" ||
                           tok == "ease-out" || tok == "ease-in-out" ||
                           tok.substr(0, 13) == "cubic-bezier(") {
                    timing = tok;
                } else {
                    prop = tok;
                }
            }
            properties.push_back(prop);
            durations.push_back(dur);
            timingFuncs.push_back(timing);
            delays.push_back(delay);
        }
    }

    if (properties.empty() || durations.empty()) return;

    auto& et = elements_[elem];

    // Check each property for changes
    bool isAll = (properties.size() == 1 && properties[0] == "all");

    // Check if a computed longhand property is covered by a transition
    // property, handling shorthand→longhand expansion.
    auto shorthandCovers = [](const std::string& shorthand, const std::string& longhand) -> bool {
        if (shorthand == longhand) return true;
        // Simple prefix: "margin" covers "margin-top", etc.
        if (longhand.size() > shorthand.size() &&
            longhand.substr(0, shorthand.size()) == shorthand &&
            longhand[shorthand.size()] == '-')
            return true;
        // border-radius expands to border-{top,bottom}-{left,right}-radius
        if (shorthand == "border-radius" &&
            longhand.find("border-") == 0 && longhand.find("-radius") != std::string::npos)
            return true;
        return false;
    };

    auto shouldTransition = [&](const std::string& prop) -> bool {
        if (isAll) return true;
        for (auto& p : properties) {
            if (p == "all" || shorthandCovers(p, prop)) return true;
        }
        return false;
    };

    auto getIndex = [&](const std::string& prop) -> size_t {
        if (isAll) return 0;
        for (size_t i = 0; i < properties.size(); ++i) {
            if (properties[i] == "all" || shorthandCovers(properties[i], prop))
                return i;
        }
        return 0;
    };

    // Compare old and new styles for changes
    for (auto& [prop, newVal] : newStyle) {
        // Skip transition-* properties themselves
        if (prop.substr(0, 11) == "transition-") continue;
        if (prop.substr(0, 10) == "animation-") continue;
        if (prop == "display" || prop == "transition" || prop == "animation") continue;

        if (!shouldTransition(prop)) continue;

        auto oldIt = oldStyle.find(prop);
        std::string oldVal = (oldIt != oldStyle.end()) ? oldIt->second : "";
        if (oldVal == newVal) continue;
        if (oldVal.empty()) {
            // Substitute CSS initial values so transitions from "nothing" work.
            oldVal = initialValueForProperty(prop, newVal);
            if (oldVal.empty()) continue;
        }

        size_t idx = getIndex(prop);
        double dur = parseDurationMs(durations[idx % durations.size()]);
        if (dur <= 0) continue;

        CubicEase easing = kEase;
        if (!timingFuncs.empty())
            easing = parseTimingFunction(timingFuncs[idx % timingFuncs.size()]);
        double delay = 0;
        if (!delays.empty())
            delay = parseDurationMs(delays[idx % delays.size()]);

        // Check if there's already an active transition for this property
        bool found = false;
        for (auto& tr : et.active) {
            if (tr.property == prop) {
                if (tr.endValue == newVal) {
                    // Already transitioning to this target — don't restart
                    found = true;
                } else {
                    // Target changed mid-transition — retarget from current value
                    double elapsed = currentTime - tr.startTime - tr.delay;
                    if (elapsed > 0 && elapsed < tr.duration) {
                        float progress = static_cast<float>(elapsed / tr.duration);
                        progress = ccubicEase(tr.easing, progress);
                        tr.startValue = interpolate(tr.startValue, tr.endValue, progress, prop);
                    }
                    tr.endValue = newVal;
                    tr.startTime = currentTime;
                    tr.duration = dur;
                    tr.delay = delay;
                    tr.easing = easing;
                    found = true;
                }
                break;
            }
        }
        if (!found) {
            et.active.push_back({prop, oldVal, newVal, currentTime, dur, delay, easing});
            pendingEvents_.push_back({elem, "transitionstart", prop, 0.0});
        }

        // Set the current value to the start value (transition hasn't progressed yet)
        newStyle[prop] = oldVal;
    }
}

bool TransitionManager::tick(double currentTime) {
    bool anyActive = false;
    bool anyCompleted = false;
    activeThisTick_.clear();

    for (auto it = elements_.begin(); it != elements_.end(); ) {
        auto& et = it->second;
        dom::Element* elem = it->first;

        // Queue transitionend for completed transitions before removing them
        for (auto& tr : et.active) {
            double elapsed = currentTime - tr.startTime - tr.delay;
            if (elapsed >= tr.duration) {
                pendingEvents_.push_back({elem, "transitionend", tr.property,
                                          tr.duration / 1000.0});
            }
        }

        size_t sizeBefore = et.active.size();
        et.active.erase(
            std::remove_if(et.active.begin(), et.active.end(),
                [currentTime](const Transition& tr) {
                    double elapsed = currentTime - tr.startTime - tr.delay;
                    return elapsed >= tr.duration;
                }),
            et.active.end());
        bool justCompleted = et.active.size() < sizeBefore;

        if (et.active.empty()) {
            if (justCompleted) {
                // Re-cascade once next frame so applyOverrides (now bailing)
                // stops writing the last interpolated value into computedStyle
                // — without this, the element keeps the final-frame value
                // (e.g. ~0.7px drift with cubic-bezier easing) until some
                // unrelated DOM mutation re-dirties it.
                elem->markDirty();
                anyCompleted = true;
            }
            it = elements_.erase(it);
        } else {
            // Don't markDirty here: the layout-thread coordinator decides
            // whether this element is a compositor-promotable layer (no base
            // re-record) or a base change. Still count as active so the layout
            // loop keeps ticking the animation forward.
            activeThisTick_.push_back(elem);
            anyActive = true;
            ++it;
        }
    }

    return anyActive || anyCompleted;
}

void TransitionManager::applyOverrides(dom::Element* elem,
                                       htmlayout::css::ComputedStyle& style,
                                       double currentTime) {
    auto it = elements_.find(elem);
    if (it == elements_.end()) return;

    for (auto& tr : it->second.active) {
        double elapsed = currentTime - tr.startTime - tr.delay;
        if (elapsed < 0) {
            // Still in delay period — keep start value
            style[tr.property] = tr.startValue;
        } else if (elapsed >= tr.duration) {
            // Complete — use end value
            style[tr.property] = tr.endValue;
        } else {
            float progress = static_cast<float>(elapsed / tr.duration);
            progress = ccubicEase(tr.easing, progress);
            style[tr.property] = interpolate(tr.startValue, tr.endValue, progress, tr.property);
        }
    }
}

void TransitionManager::removeElement(dom::Element* elem) {
    elements_.erase(elem);
}

bool TransitionManager::hasActive(dom::Element* elem) const {
    auto it = elements_.find(elem);
    return it != elements_.end() && !it->second.active.empty();
}

bool TransitionManager::activeAnimatesOnly(dom::Element* elem,
                                           const std::set<std::string>& allowed) const {
    auto it = elements_.find(elem);
    if (it == elements_.end() || it->second.active.empty()) return false;
    for (auto& tr : it->second.active) {
        if (allowed.find(tr.property) == allowed.end()) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// AnimationManager
// ---------------------------------------------------------------------------

const htmlayout::css::KeyframeBlock* AnimationManager::findKeyframes(const std::string& name) const {
    if (!keyframes_) return nullptr;
    for (auto& kf : *keyframes_) {
        if (kf.name == name) return &kf;
    }
    return nullptr;
}

// Per CSS Animations §4: setting display:none on an element (or any ancestor)
// terminates the animations running on it and its descendants. We don't tear
// the Animation down (it resumes if the element is shown again), but a hidden
// animation must stop reporting itself active — otherwise a single infinite
// animation on a hidden element (a load spinner in a display:none overlay, say)
// pins the whole document on the re-layout + re-raster path every frame.
static bool inDisplayNoneSubtree(dom::Element* elem) {
    for (dom::Element* e = elem; e; e = e->parentElement()) {
        const auto& cs = e->computedStyle();
        auto it = cs.find("display");
        if (it != cs.end() && it->second == "none") return true;
    }
    return false;
}

void AnimationManager::onStyleChange(dom::Element* elem,
                                     const htmlayout::css::ComputedStyle& newStyle,
                                     double currentTime) {
    // Check for animation declarations — try longhands first, then shorthand
    std::string animName;
    std::string durStr, timingStr, delayStr, iterStr, directionStr, fillModeStr;
    std::string playStateStr;

    auto anIt = newStyle.find("animation-name");
    if (anIt != newStyle.end() && anIt->second != "none" && !anIt->second.empty()) {
        animName = anIt->second;
        // Read longhand properties
        auto it = newStyle.find("animation-duration");
        if (it != newStyle.end()) durStr = it->second;
        it = newStyle.find("animation-timing-function");
        if (it != newStyle.end()) timingStr = it->second;
        it = newStyle.find("animation-delay");
        if (it != newStyle.end()) delayStr = it->second;
        it = newStyle.find("animation-iteration-count");
        if (it != newStyle.end()) iterStr = it->second;
        it = newStyle.find("animation-direction");
        if (it != newStyle.end()) directionStr = it->second;
        it = newStyle.find("animation-fill-mode");
        if (it != newStyle.end()) fillModeStr = it->second;
        it = newStyle.find("animation-play-state");
        if (it != newStyle.end()) playStateStr = it->second;
    } else {
        // Try shorthand: animation: name duration timing delay iteration direction fill
        auto aIt = newStyle.find("animation");
        if (aIt == newStyle.end() || aIt->second.empty() || aIt->second == "none") {
            // animation-name has been cleared. Reset the previousName memo so
            // that re-applying the same animation later (e.g. by re-adding a
            // class) triggers a fresh start, per CSS Animations §4.2.
            auto eit = elements_.find(elem);
            if (eit != elements_.end()) eit->second.previousName.clear();
            return;
        }
        // Parse shorthand
        std::istringstream iss(aIt->second);
        std::string tok;
        int numIdx = 0;
        while (iss >> tok) {
            char* end = nullptr;
            float numVal = std::strtof(tok.c_str(), &end);
            bool isNumber = (end != tok.c_str());
            std::string suffix = isNumber ? std::string(end) : "";
            bool isTime = isNumber && (suffix == "s" || suffix == "ms");
            bool isBareNumber = isNumber && suffix.empty();
            if (isTime) {
                if (numIdx == 0) { durStr = tok; ++numIdx; }
                else { delayStr = tok; ++numIdx; }
            } else if (isBareNumber && numVal > 0) {
                // Bare number = iteration count (e.g. "3")
                iterStr = tok;
            } else if (tok == "ease" || tok == "linear" || tok == "ease-in" ||
                       tok == "ease-out" || tok == "ease-in-out") {
                timingStr = tok;
            } else if (tok == "infinite") {
                iterStr = tok;
            } else if (tok == "alternate" || tok == "alternate-reverse" ||
                       tok == "reverse" || tok == "normal") {
                directionStr = tok;
            } else if (tok == "none" || tok == "forwards" || tok == "backwards" || tok == "both") {
                fillModeStr = tok;
            } else if (tok == "paused" || tok == "running") {
                playStateStr = tok;
            } else {
                animName = tok;
            }
        }
        if (animName.empty()) return;
    }

    auto& ea = elements_[elem];

    const bool wantPaused = (playStateStr == "paused");

    // Per CSS Animations §4.2: an animation only (re)starts when
    // animation-name *changes*. Once it has run to completion, the
    // animation does not re-trigger just because animation-name is
    // still the same value in the cascade. We track the previously-
    // seen name and bail if it's unchanged — without this guard, every
    // post-completion re-cascade of an unchanged animation-name would
    // register a fresh Animation, locking the element into an infinite
    // loop of restarting animations.
    if (ea.previousName == animName) {
        // animation-play-state CAN change without the name changing —
        // pause freezes the clock, resume shifts startTime by the pause span.
        for (auto& a : ea.active) {
            if (a.name != animName) continue;
            if (wantPaused && !a.paused) {
                a.paused = true;
                a.pausedAt = currentTime;
            } else if (!wantPaused && a.paused) {
                a.startTime += currentTime - a.pausedAt;
                a.paused = false;
            }
        }
        return;
    }
    ea.previousName = animName;

    // Belt-and-braces: if the same name is somehow already in the
    // active list (e.g. previousName was cleared mid-flight), don't
    // duplicate it.
    for (auto& a : ea.active) {
        if (a.name == animName) return;
    }

    if (!findKeyframes(animName)) return; // no keyframes defined

    // Parse collected values
    double dur = parseDurationMs(durStr);
    if (dur <= 0) return;

    CubicEase easing = kEase;
    if (!timingStr.empty()) easing = parseTimingFunction(timingStr);

    double delay = 0;
    if (!delayStr.empty()) delay = parseDurationMs(delayStr);

    int iterCount = 1;
    if (iterStr == "infinite") iterCount = -1;
    else if (!iterStr.empty()) {
        char* end = nullptr;
        int v = static_cast<int>(std::strtof(iterStr.c_str(), &end));
        if (end != iterStr.c_str() && v > 0) iterCount = v;
    }

    bool alternate = false, reverse = false;
    if (directionStr == "reverse") reverse = true;
    else if (directionStr == "alternate") alternate = true;
    else if (directionStr == "alternate-reverse") { alternate = true; reverse = true; }

    std::string fillMode = fillModeStr.empty() ? "none" : fillModeStr;

    Animation anim{animName, dur, delay, easing, iterCount,
                   alternate, reverse, fillMode, currentTime, 0};
    if (wantPaused) {
        // Born paused: the clock freezes at the start instant. A negative
        // delay still pins a deterministic mid-animation frame.
        anim.paused = true;
        anim.pausedAt = currentTime;
    }
    ea.active.push_back(std::move(anim));

    pendingEvents_.push_back({elem, "animationstart", animName, 0.0});
}

bool AnimationManager::tick(double currentTime) {
    bool anyActive = false;
    bool anyCompleted = false;
    activeThisTick_.clear();

    for (auto it = elements_.begin(); it != elements_.end(); ) {
        auto& ea = it->second;
        dom::Element* elem = it->first;

        // A hidden (display:none) element's animations don't run: keep the
        // entry so they resume if it's shown again, but don't tick, mark dirty,
        // or report active. This is what stops an infinite animation on a
        // hidden element from re-rasterizing the whole UI every frame.
        if (!ea.active.empty() && inDisplayNoneSubtree(elem)) { ++it; continue; }

        for (auto& a : ea.active) {
            double elapsed = a.effectiveTime(currentTime) - a.startTime - a.delay;
            if (elapsed < 0) continue;

            // Check for iteration events
            int currentIter = (a.duration > 0)
                ? static_cast<int>(elapsed / a.duration) : 0;
            if (currentIter > a.completedIterations && a.completedIterations > 0) {
                // Don't fire iteration event on the final completion
                bool isComplete = a.iterationCount >= 0 &&
                    elapsed >= a.duration * a.iterationCount;
                if (!isComplete) {
                    pendingEvents_.push_back({elem, "animationiteration", a.name,
                                              elapsed / 1000.0});
                }
            }
            a.completedIterations = currentIter;

            // Check for completion
            if (a.iterationCount >= 0 &&
                elapsed >= a.duration * a.iterationCount) {
                double totalDuration = a.duration * a.iterationCount;
                pendingEvents_.push_back({elem, "animationend", a.name,
                                          totalDuration / 1000.0});
            }
        }

        size_t sizeBefore = ea.active.size();
        ea.active.erase(
            std::remove_if(ea.active.begin(), ea.active.end(),
                [currentTime](const Animation& a) {
                    if (a.iterationCount < 0) return false; // infinite
                    double elapsed = a.effectiveTime(currentTime) - a.startTime - a.delay;
                    return elapsed >= a.duration * a.iterationCount;
                }),
            ea.active.end());
        bool justCompleted = ea.active.size() < sizeBefore;

        if (ea.active.empty()) {
            if (justCompleted) {
                // Re-cascade once next frame so applyOverrides (now bailing
                // because active is empty) lets computedStyle settle to the
                // post-animation cascade default — otherwise the element
                // keeps the final-frame interpolated value (e.g. tile-pop-in
                // leaves scale(~1.0025) instead of the true scale(1)).
                elem->markDirty();
                anyCompleted = true;
            }
            // Keep the element's entry so previousName persists — that memo
            // prevents the cascade from re-registering the same animation
            // while animation-name is still in computed style. Erasing here
            // would restart forever.
            ++it;
        } else {
            // See TransitionManager::tick: defer the promote-vs-base-dirty
            // decision to the layout-thread coordinator; just record activity.
            // Paused animations hold a static frame (applied during style
            // resolution) — they must not drive per-frame re-render.
            bool anyRunning = false;
            for (auto& a : ea.active) {
                if (!a.paused) { anyRunning = true; break; }
            }
            if (anyRunning) {
                activeThisTick_.push_back(elem);
                anyActive = true;
            }
            ++it;
        }
    }

    return anyActive || anyCompleted;
}

void AnimationManager::applyOverrides(dom::Element* elem,
                                      htmlayout::css::ComputedStyle& style,
                                      double currentTime) const {
    auto it = elements_.find(elem);
    if (it == elements_.end() || it->second.active.empty()) return;

    for (auto& anim : it->second.active) {
        auto* kf = findKeyframes(anim.name);
        if (!kf || kf->stops.empty()) continue;

        double elapsed = anim.effectiveTime(currentTime) - anim.startTime - anim.delay;
        if (elapsed < 0) {
            // In delay period — apply backwards fill if applicable
            if (anim.fillMode != "backwards" && anim.fillMode != "both") continue;
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
                continue;
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
}

void AnimationManager::removeElement(dom::Element* elem) {
    elements_.erase(elem);
}

bool AnimationManager::hasActive(dom::Element* elem) const {
    auto it = elements_.find(elem);
    return it != elements_.end() && !it->second.active.empty();
}

bool AnimationManager::activeAnimatesOnly(dom::Element* elem,
                                          const std::set<std::string>& allowed) const {
    auto it = elements_.find(elem);
    if (it == elements_.end() || it->second.active.empty()) return false;

    // Build the union of animated property longhands across all active
    // animations, resolved via the keyframe blocks — same as applyOverrides.
    std::unordered_set<std::string> animated;
    for (auto& anim : it->second.active) {
        auto* kf = findKeyframes(anim.name);
        if (!kf) continue;
        for (auto& stop : kf->stops) {
            for (auto& d : stop.declarations) animated.insert(d.property);
        }
    }
    if (animated.empty()) return false;
    for (auto& prop : animated) {
        if (allowed.find(prop) == allowed.end()) return false;
    }
    return true;
}

bool isTransformOpacityOnly(dom::Element* elem,
                            const AnimationManager& anim,
                            const TransitionManager& trans) {
    const std::set<std::string> allowed{"transform", "opacity"};
    bool A = anim.hasActive(elem);
    bool T = trans.hasActive(elem);
    if (!A && !T) return false;
    if (A && !anim.activeAnimatesOnly(elem, allowed)) return false;
    if (T && !trans.activeAnimatesOnly(elem, allowed)) return false;
    return true;
}

} // namespace bro::engine
