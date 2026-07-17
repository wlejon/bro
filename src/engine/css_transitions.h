#pragma once

#include <bromath/curves.h>
#include <css/cascade.h>

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bro::dom { class Element; }

namespace bro::engine {

// Parse a CSS timing-function string → bromath::CubicEase.
// Named easings map to standard control points; cubic-bezier(x1,y1,x2,y2) is
// parsed directly. Unknown / empty → "ease".
bromath::CubicEase parseTimingFunction(const std::string& val);

// CSS initial value for a property, shaped to match `refValue` where structure
// matters (transform identities: "scale(1.4)" → "scale(1)"). Empty string when
// no useful initial exists. Shared by the transition, CSS-animation, and Web
// Animations (element.animate) interpolators for implicit endpoints.
std::string cssInitialValueForProperty(const std::string& prop,
                                       const std::string& refValue);

// A single in-flight property transition.
struct Transition {
    std::string property;
    std::string startValue;
    std::string endValue;
    double startTime;   // ms (engine time)
    double duration;    // ms
    double delay;       // ms
    bromath::CubicEase easing;
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

    // Check if any transitions are running.
    bool hasActiveTransitions() const { return !elements_.empty(); }

    // Read-only compositor-hint accessors.
    // hasActive: element is present with at least one active transition.
    bool hasActive(dom::Element* elem) const;
    // activeAnimatesOnly: element has at least one active transition AND every
    // active Transition::property is in `allowed`. False if none are active.
    bool activeAnimatesOnly(dom::Element* elem,
                            const std::set<std::string>& allowed) const;

    // Elements with ≥1 active transition after the most recent tick(). tick()
    // collects these instead of marking them dirty itself, so the layout-thread
    // coordinator can decide per element whether it's a compositor-promotable
    // (transform/opacity-only) layer or a base change that must re-record.
    const std::vector<dom::Element*>& activeThisTick() const { return activeThisTick_; }

    // Interpolate between two CSS values at progress t ∈ [0,1].
    static std::string interpolate(const std::string& from, const std::string& to,
                                   float t, const std::string& property);

    // Take all pending events (call from main thread after layout completes).
    std::vector<PendingCSSEvent> takePendingEvents() {
        return std::move(pendingEvents_);
    }

    // Drop every registered transition and queued event. Used when the app
    // document is torn down as a whole (top-level location.reload()) — the
    // Element* keys are about to dangle and per-element removal would need
    // a full tree walk.
    void clearAll() {
        elements_.clear();
        activeThisTick_.clear();
        pendingEvents_.clear();
    }

private:
    std::vector<PendingCSSEvent> pendingEvents_;
    std::unordered_map<dom::Element*, ElementTransitions> elements_;
    std::vector<dom::Element*> activeThisTick_;
};

// ---------------------------------------------------------------------------
// CSS Animations (@keyframes)
// ---------------------------------------------------------------------------

// A single in-flight animation instance on an element.
struct Animation {
    std::string name;           // @keyframes name
    double duration;            // ms
    double delay;               // ms
    bromath::CubicEase easing;
    int iterationCount;         // -1 = infinite
    bool alternate;             // direction: alternate
    bool reverse;               // direction: reverse
    std::string fillMode;       // none, forwards, backwards, both
    double startTime;           // ms (engine time)
    int completedIterations = 0;
    // animation-play-state: while paused, the clock freezes at pausedAt;
    // resuming shifts startTime forward by the paused span.
    bool paused = false;
    double pausedAt = 0;        // ms (engine time), valid while paused

    // The clock used for progress: frozen at pausedAt while paused.
    double effectiveTime(double currentTime) const {
        return paused ? pausedAt : currentTime;
    }
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

    bool hasActiveAnimations() const { return !elements_.empty(); }

    // Read-only compositor-hint accessors.
    // hasActive: element is present with at least one active animation.
    bool hasActive(dom::Element* elem) const;
    // activeAnimatesOnly: element has at least one active animation AND the
    // union of all animated property longhands across its active animations
    // (resolved via the keyframe blocks, same as applyOverrides) is a non-empty
    // subset of `allowed`. False if it has no active animations.
    bool activeAnimatesOnly(dom::Element* elem,
                            const std::set<std::string>& allowed) const;

    // Elements with ≥1 active animation after the most recent tick(). See the
    // matching TransitionManager::activeThisTick() note.
    const std::vector<dom::Element*>& activeThisTick() const { return activeThisTick_; }

    // Take all pending events (call from main thread after layout completes).
    std::vector<PendingCSSEvent> takePendingEvents() {
        return std::move(pendingEvents_);
    }

    // Whole-document teardown reset — see TransitionManager::clearAll().
    // Also drops the keyframe-store pointer: it aims into the old document's
    // cascade, which is freed with the document.
    void clearAll() {
        elements_.clear();
        activeThisTick_.clear();
        pendingEvents_.clear();
        keyframes_ = nullptr;
    }

private:
    std::vector<PendingCSSEvent> pendingEvents_;
    const std::vector<htmlayout::css::KeyframeBlock>* keyframes_ = nullptr;
    std::unordered_map<dom::Element*, ElementAnimations> elements_;
    std::vector<dom::Element*> activeThisTick_;

    const htmlayout::css::KeyframeBlock* findKeyframes(const std::string& name) const;
};

// True iff the element has active CSS animations and/or transitions and they are
// ALL confined to transform/opacity — the properties a compositor layer can
// animate without re-rasterizing surrounding content. Requires at least one of
// the two managers to report an active animation/transition on the element.
bool isTransformOpacityOnly(dom::Element* elem,
                            const AnimationManager& anim,
                            const TransitionManager& trans);

} // namespace bro::engine
