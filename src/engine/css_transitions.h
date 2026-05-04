#pragma once

#include <css/cascade.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// Queued CSS event for thread-safe dispatch.
// Events are queued on the layout thread, then drained on the main thread.
struct PendingCSSEvent {
    dom::Element* element;
    std::string type;   // "transitionstart", "transitionend", "animationstart", etc.
    std::string name;   // property name (transitions) or animation name (animations)
    double elapsedTime; // seconds
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

    // Interpolate between two CSS values at progress t ∈ [0,1].
    static std::string interpolate(const std::string& from, const std::string& to,
                                   float t, const std::string& property);

    // Take all pending events (call from main thread after layout completes).
    std::vector<PendingCSSEvent> takePendingEvents() {
        return std::move(pendingEvents_);
    }

private:
    std::vector<PendingCSSEvent> pendingEvents_;
    std::unordered_map<dom::Element*, ElementTransitions> elements_;
};

// ---------------------------------------------------------------------------
// CSS Animations (@keyframes)
// ---------------------------------------------------------------------------

// A single in-flight animation instance on an element.
struct Animation {
    std::string name;           // @keyframes name
    double duration;            // ms
    double delay;               // ms
    CubicBezier easing;
    int iterationCount;         // -1 = infinite
    bool alternate;             // direction: alternate
    bool reverse;               // direction: reverse
    std::string fillMode;       // none, forwards, backwards, both
    double startTime;           // ms (engine time)
    int completedIterations = 0;
};

struct ElementAnimations {
    std::vector<Animation> active;
    // Last-seen animation-name from the cascade. Used to detect when
    // animation-name actually changes vs. when it's just being re-cascaded
    // with the same value — only the former should (re)start an animation.
    std::string previousName;
};

class AnimationManager {
public:
    // Called during style resolution to detect animation-name changes.
    void onStyleChange(dom::Element* elem,
                       const htmlayout::css::ComputedStyle& newStyle,
                       double currentTime);

    // Tick all animations. Returns true if any are active.
    bool tick(double currentTime);

    // Apply animation property overrides to computed style.
    void applyOverrides(dom::Element* elem,
                        htmlayout::css::ComputedStyle& style,
                        double currentTime) const;

    // Set the keyframe store (from htmlayout Cascade).
    void setKeyframes(const std::vector<htmlayout::css::KeyframeBlock>* kf) {
        keyframes_ = kf;
    }

    void removeElement(dom::Element* elem);
    bool hasActiveAnimations() const { return !elements_.empty(); }

    // Take all pending events (call from main thread after layout completes).
    std::vector<PendingCSSEvent> takePendingEvents() {
        return std::move(pendingEvents_);
    }

private:
    std::vector<PendingCSSEvent> pendingEvents_;
    const std::vector<htmlayout::css::KeyframeBlock>* keyframes_ = nullptr;
    std::unordered_map<dom::Element*, ElementAnimations> elements_;

    const htmlayout::css::KeyframeBlock* findKeyframes(const std::string& name) const;
};

} // namespace bro::engine
