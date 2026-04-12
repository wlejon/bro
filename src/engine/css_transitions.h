#pragma once

#include <css/cascade.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::dom { class Element; }

namespace bro::engine {

// CSS cubic-bezier timing function.
struct CubicBezier {
    float x1, y1, x2, y2;

    // Evaluate the timing function at progress t ∈ [0,1].
    float evaluate(float t) const;

    // Preset timing functions
    static CubicBezier linear()    { return {0, 0, 1, 1}; }
    static CubicBezier ease()      { return {0.25f, 0.1f, 0.25f, 1.0f}; }
    static CubicBezier easeIn()    { return {0.42f, 0, 1, 1}; }
    static CubicBezier easeOut()   { return {0, 0, 0.58f, 1}; }
    static CubicBezier easeInOut() { return {0.42f, 0, 0.58f, 1}; }
};

// Parse a CSS timing-function string → CubicBezier.
CubicBezier parseTimingFunction(const std::string& val);

// A single in-flight property transition.
struct Transition {
    std::string property;
    std::string startValue;
    std::string endValue;
    double startTime;   // ms (engine time)
    double duration;    // ms
    double delay;       // ms
    CubicBezier easing;
};

// Per-element transition state.
struct ElementTransitions {
    std::vector<Transition> active;
    // The "resting" computed style — what cascade produced last time,
    // before transition overrides were applied.
    htmlayout::css::ComputedStyle targetStyle;
};

// Manages CSS transitions for all elements.
class TransitionManager {
public:
    // Called during style resolution. Compares oldStyle to newStyle and starts
    // transitions for properties that have transition-* declarations.
    // Modifies newStyle in-place: for transitioning properties, the value is
    // set to the interpolated value (not the target).
    void onStyleChange(dom::Element* elem,
                       const htmlayout::css::ComputedStyle& oldStyle,
                       htmlayout::css::ComputedStyle& newStyle,
                       double currentTime);

    // Tick all active transitions. Returns true if any transitions are active
    // (meaning the document should be marked dirty for re-render).
    bool tick(double currentTime);

    // Apply transition overrides to an element's computed style.
    // Called after resolveStyles to re-inject interpolated values.
    void applyOverrides(dom::Element* elem, htmlayout::css::ComputedStyle& style,
                        double currentTime);

    // Remove all transitions for a removed element.
    void removeElement(dom::Element* elem);

    // Check if any transitions are running.
    bool hasActiveTransitions() const { return !elements_.empty(); }

private:
    // Interpolate between two CSS values at progress t ∈ [0,1].
    static std::string interpolate(const std::string& from, const std::string& to,
                                   float t, const std::string& property);

    std::unordered_map<dom::Element*, ElementTransitions> elements_;
};

} // namespace bro::engine
