#pragma once

#include "engine/css_easing.h"

#include <css/cascade.h>

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bro::dom { class Element; class Document; }

namespace bro::engine {

class WebAnimationManager;

// The element or an ancestor computes to display:none.
bool inDisplayNoneSubtree(dom::Element* elem);

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
    TimingFunction easing;
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

    // Forget everything this manager holds about `elem`: its registered
    // transitions, its slot in the most recent tick's active list, and any
    // queued event naming it.
    //
    // MUST be called before the Element's storage goes away. The keys here are
    // raw Element* (unlike WebAnimationManager, whose records are generation-
    // checked through Document::resolveNode), and tick() dereferences them —
    // `elem->markDirty()` when a transition completes — while takePendingEvents
    // hands the pointer to dispatchEvent a few lines later. An element removed
    // mid-transition therefore crashed the process on the first tick after its
    // deferred free drained. Document::freeNode is the one call site: it fires
    // for every doomed node, deepest first, so a transition on any descendant
    // of a removed subtree is dropped with it. Removal cancels the transition
    // per CSS, so nothing is owed a transitionend.
    void forgetElement(dom::Element* elem);

    // The same, for every element belonging to `doc`. For the paths that
    // destroy a document's node storage wholesale without routing each node
    // through freeNode(): Document::parse() re-parsing over an existing tree,
    // and ~Document. Dereferences the keys, so call while they are still alive.
    void forgetDocument(const dom::Document* doc);

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

// A CSS animation IS a Web Animation (CSS Animations 2: a CSSAnimation): each
// layer of an element's animation-name list owns one record in the
// WebAnimationManager, which runs, interpolates and composites it exactly as
// it does element.animate(), and which script reaches through getAnimations().
// This manager is the markup side of that: it creates, re-times and cancels
// the records as the animation-* longhands change, keeps them on the current
// @keyframes, and derives the animationstart / iteration / end / cancel events
// from each record's phase, so a script seek or pause fires them as on the web.

// One entry of an element's animation-name list.
struct CssAnimationLayer {
    std::string name;
    uint64_t id = 0;            // WebAnimation record; 0 = no @keyframes of that name
    // Keyframes the record was built from, to rebuild when the rule changes.
    const htmlayout::css::KeyframeBlock* block = nullptr;
    size_t blockHash = 0;
    TimingFunction timing;      // the layer's animation-timing-function
    // Phase/iteration last reported through events.
    int lastPhase = 0;          // WebAnimPhase as int; 0 = Idle
    double lastIteration = 0;
    double lastElapsed = 0;     // s of active time, for a script cancel()
};

struct ElementAnimations {
    std::vector<CssAnimationLayer> layers;
    // The animation-* longhands last seen, joined. Unchanged longhands (the
    // cascade re-resolves an animating element every frame) are a no-op, and
    // an animation that ran to completion is not restarted by them.
    std::string signature;
};

class AnimationManager {
public:
    // The record store CSS animations live in. Set once by the engine.
    void setWebAnimations(WebAnimationManager* web) { web_ = web; }

    // Called during style resolution: reconcile the element's animation-*
    // longhands with the records it owns.
    void onStyleChange(dom::Element* elem,
                       const htmlayout::css::ComputedStyle& newStyle,
                       double currentTime);

    // Queue the animation* events due at `currentTime` and keep records on
    // their current @keyframes. Frame pumping is the WebAnimationManager's
    // tick; this returns whether anything was queued.
    bool tick(double currentTime);

    // Set the keyframe store (from htmlayout Cascade).
    void setKeyframes(const std::vector<htmlayout::css::KeyframeBlock>* kf) {
        keyframes_ = kf;
    }

    // Take all pending events (call from main thread after layout completes).
    std::vector<PendingCSSEvent> takePendingEvents() {
        return std::move(pendingEvents_);
    }

    // See TransitionManager::forgetElement / forgetDocument — the entries here
    // are keyed by raw Element* in exactly the same way. Forgetting an
    // element also cancels the records its layers own.
    void forgetElement(dom::Element* elem);
    void forgetDocument(const dom::Document* doc);

    // Whole-document teardown reset — see TransitionManager::clearAll().
    // Also drops the keyframe-store pointer: it aims into the old document's
    // cascade, which is freed with the document. (The records go with
    // WebAnimationManager::clearAll.)
    void clearAll() {
        elements_.clear();
        pendingEvents_.clear();
        keyframes_ = nullptr;
    }

private:
    std::vector<PendingCSSEvent> pendingEvents_;
    const std::vector<htmlayout::css::KeyframeBlock>* keyframes_ = nullptr;
    std::unordered_map<dom::Element*, ElementAnimations> elements_;
    WebAnimationManager* web_ = nullptr;

    const htmlayout::css::KeyframeBlock* findKeyframes(const std::string& name) const;
    void dropLayer(dom::Element* elem, CssAnimationLayer& layer, double now, bool fireCancel);
};

} // namespace bro::engine
