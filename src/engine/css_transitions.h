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

// `ancestor` is a proper ancestor of `elem` (parent-element chain).
bool isElementAncestor(const dom::Element* ancestor, dom::Element* elem);

// CSS initial value for a property, shaped to match `refValue` where structure
// matters (transform identities: "scale(1.4)" → "scale(1)"). Empty string when
// no useful initial exists. Shared by the transition, CSS-animation, and Web
// Animations (element.animate) interpolators for implicit endpoints.
std::string cssInitialValueForProperty(const std::string& prop,
                                       const std::string& refValue);

// ---------------------------------------------------------------------------
// CSS Transitions
// ---------------------------------------------------------------------------

// A running CSS transition IS a Web Animation (CSS Transitions 2: a
// CSSTransition): a two-keyframe record in the WebAnimationManager, which
// runs, interpolates and composites it like any animation and which script
// reaches through getAnimations(). This manager is the markup side: it starts,
// retargets (with the reversing shortening of CSS Transitions §3.1) and
// cancels those records as style changes, and derives transitionrun / start /
// end / cancel from each record's phase, so a script seek or cancel fires
// them as on the web.

// One running transition of an element.
struct RunningTransition {
    std::string property;
    uint64_t id = 0;                    // WebAnimation record
    std::string endValue;
    std::string reversingAdjustedStart; // CSS Transitions §3.1
    double shorteningFactor = 1.0;
    int lastPhase = -1;                 // WebAnimPhase as int; -1 = not reported yet
    double lastElapsed = 0;             // s of active time, for transitioncancel
};

// Per-element transition state.
struct ElementTransitions {
    std::vector<RunningTransition> running;
    // Transitions that just completed (or script cancelled): property → end
    // value. The next style change of the element must not start a new
    // transition from the value the finished one last applied to the value it
    // ended on; it consumes these.
    std::vector<std::pair<std::string, std::string>> completed;
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
    // The record store transitions live in. Set once by the engine.
    void setWebAnimations(WebAnimationManager* web) { web_ = web; }

    // Called during style resolution with the element's previous computed
    // style and the cascade's new one: starts, retargets and cancels the
    // element's transitions. A property with a transition in flight keeps its
    // previous value in newStyle (the record's interpolated value replaces it
    // when the Web Animations overrides apply), so the style diff does not
    // count a running transition as a style change of its own.
    void onStyleChange(dom::Element* elem,
                       const htmlayout::css::ComputedStyle& oldStyle,
                       htmlayout::css::ComputedStyle& newStyle,
                       double currentTime);

    // Queue the transition* events due at `currentTime`, retire completed
    // transitions and cancel those display:none took out of the rendering.
    // Frame pumping is the WebAnimationManager's tick; this returns whether
    // anything was queued or completed.
    bool tick(double currentTime);

    // `ancestor`'s display flipped to or from none during style resolution:
    // mark the elements under it that have transitions dirty, so they
    // re-resolve in the same pass and are cancelled.
    void displayToggled(dom::Element* ancestor);

    // Interpolate between two CSS values at progress t ∈ [0,1].
    static std::string interpolate(const std::string& from, const std::string& to,
                                   float t, const std::string& property);

    // Take all pending events (call from main thread after layout completes).
    std::vector<PendingCSSEvent> takePendingEvents() {
        return std::move(pendingEvents_);
    }

    // Forget everything this manager holds about `elem`: its transitions
    // (their records are cancelled) and any queued event naming it.
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
    // a full tree walk. (The records go with WebAnimationManager::clearAll.)
    void clearAll() {
        elements_.clear();
        pendingEvents_.clear();
    }

private:
    std::vector<PendingCSSEvent> pendingEvents_;
    std::unordered_map<dom::Element*, ElementTransitions> elements_;
    WebAnimationManager* web_ = nullptr;
    uint64_t generation_ = 0;

    // Derive the events for `run`'s phase change since it was last seen.
    // True when the transition is over (completed, or cancelled by script);
    // it is then recorded in et.completed and its record let go of.
    bool advance(dom::Element* elem, ElementTransitions& et, RunningTransition& run,
                 double now);
    // Cancel a running transition from markup (transitioncancel if it had
    // not ended).
    void cancel(dom::Element* elem, RunningTransition& run, double now);
    void start(dom::Element* elem, ElementTransitions& et, const std::string& prop,
               const std::string& from, const std::string& to, double duration,
               double delay, const TimingFunction& timing, double now,
               const std::string& reversingAdjustedStart, double shorteningFactor);
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
    // The element (or an ancestor) is display:none: its animations are
    // cancelled, and start afresh when it is displayed again.
    bool hidden = false;
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

    // `ancestor`'s display flipped to or from none during style resolution:
    // mark the elements under it that have CSS animations dirty, so they
    // re-resolve in the same pass — their animations are cancelled, or start
    // afresh — and getAnimations() right after the change agrees.
    void displayToggled(dom::Element* ancestor);

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
