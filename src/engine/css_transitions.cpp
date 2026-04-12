#include "engine/css_transitions.h"
#include "dom/element.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace bro::engine {

// ---------------------------------------------------------------------------
// CubicBezier evaluation (Newton-Raphson root finding on the X curve)
// ---------------------------------------------------------------------------

float CubicBezier::evaluate(float t) const {
    if (x1 == 0 && y1 == 0 && x2 == 1 && y2 == 1)
        return t; // linear shortcut

    // Solve for the bezier parameter u where bezier_x(u) == t,
    // then return bezier_y(u).
    auto sampleX = [&](float u) {
        return 3.0f * (1-u)*(1-u)*u*x1 + 3.0f*(1-u)*u*u*x2 + u*u*u;
    };
    auto sampleY = [&](float u) {
        return 3.0f * (1-u)*(1-u)*u*y1 + 3.0f*(1-u)*u*u*y2 + u*u*u;
    };
    auto sampleDX = [&](float u) {
        return 3.0f*(1-u)*(1-u)*x1 + 6.0f*(1-u)*u*(x2-x1) + 3.0f*u*u*(1-x2);
    };

    // Newton-Raphson
    float u = t;
    for (int i = 0; i < 8; ++i) {
        float dx = sampleX(u) - t;
        if (std::abs(dx) < 1e-6f) break;
        float deriv = sampleDX(u);
        if (std::abs(deriv) < 1e-6f) break;
        u -= dx / deriv;
    }
    u = std::clamp(u, 0.0f, 1.0f);
    return sampleY(u);
}

CubicBezier parseTimingFunction(const std::string& val) {
    if (val.empty() || val == "ease") return CubicBezier::ease();
    if (val == "linear") return CubicBezier::linear();
    if (val == "ease-in") return CubicBezier::easeIn();
    if (val == "ease-out") return CubicBezier::easeOut();
    if (val == "ease-in-out") return CubicBezier::easeInOut();

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

    return CubicBezier::ease();
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
            std::istringstream iss(part);
            std::string tok;
            std::string prop = "all";
            std::string dur = "0s";
            std::string timing = "ease";
            std::string delay = "0s";
            int numIdx = 0;
            while (iss >> tok) {
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

    auto shouldTransition = [&](const std::string& prop) -> bool {
        if (isAll) return true;
        for (auto& p : properties) {
            if (p == prop || p == "all") return true;
        }
        return false;
    };

    auto getIndex = [&](const std::string& prop) -> size_t {
        if (isAll) return 0;
        for (size_t i = 0; i < properties.size(); ++i) {
            if (properties[i] == prop || properties[i] == "all") return i;
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
        if (oldVal.empty()) continue; // Don't transition from nothing

        size_t idx = getIndex(prop);
        double dur = parseDurationMs(durations[idx % durations.size()]);
        if (dur <= 0) continue;

        CubicBezier easing = CubicBezier::ease();
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
                        progress = tr.easing.evaluate(progress);
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
        }

        // Set the current value to the start value (transition hasn't progressed yet)
        newStyle[prop] = oldVal;
    }
}

bool TransitionManager::tick(double currentTime) {
    bool anyActive = false;

    for (auto it = elements_.begin(); it != elements_.end(); ) {
        auto& et = it->second;
        dom::Element* elem = it->first;

        // Remove completed transitions
        et.active.erase(
            std::remove_if(et.active.begin(), et.active.end(),
                [currentTime](const Transition& tr) {
                    double elapsed = currentTime - tr.startTime - tr.delay;
                    return elapsed >= tr.duration;
                }),
            et.active.end());

        if (et.active.empty()) {
            it = elements_.erase(it);
        } else {
            // Mark element dirty so it gets re-resolved with updated overrides
            elem->markDirty();
            anyActive = true;
            ++it;
        }
    }

    return anyActive;
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
            progress = tr.easing.evaluate(progress);
            style[tr.property] = interpolate(tr.startValue, tr.endValue, progress, tr.property);
        }
    }
}

void TransitionManager::removeElement(dom::Element* elem) {
    elements_.erase(elem);
}

} // namespace bro::engine
