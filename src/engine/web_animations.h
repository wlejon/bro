#pragma once

// Web Animations: the one animation model behind element.animate(), CSS
// @keyframes animations and CSS transitions. A CSS animation is a record here
// like any script animation (a CSSAnimation, per CSS Animations 2):
// AnimationManager in css_transitions.h creates, updates and cancels it from
// the element's animation-* longhands and derives the animation* events from
// its phase, and script sees the very same record through getAnimations() and
// can pause, seek or re-time it. A running CSS transition is one too (a
// CSSTransition, CSS Transitions 2), owned by TransitionManager the same way.
// Records plug into the style pipeline here: applyOverrides() during style
// resolution injects interpolated values into computed style, tick() advances
// the clock on the engine's scaled (bro.time) timeline, and activeThisTick()
// feeds the compositor-promotion decision.
//
// Threading: identical discipline to TransitionManager — records are mutated
// by JS on the main thread (only while the layout thread is idle, the same
// handshake that makes DOM mutation safe) and read/ticked on the layout
// thread during a layout pass. No locks; the layout-pipeline phase handshake
// orders all accesses.
//
// Lifetime: a record never trusts its Element* — every dereference goes
// through Document::isLiveDocument + Document::resolveNode (generation-checked
// by nodeId, safe on dangling pointers), so destroyed elements and torn-down
// documents can never crash a tick. The JS Animation wrapper holds only the
// record id; when the wrapper is finalized after the manager itself died
// (engine teardown destroys members before the JS runtime), the static
// isLive() registry makes the release a no-op.

#include "engine/css_easing.h"

#include <css/cascade.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::dom { class Element; class Document; }

namespace bro::engine {

class AnimationManager;

// One keyframe: computed offset in [0,1], optional per-keyframe easing
// (applies to the segment from this keyframe to the next), and the declared
// properties (kebab-case name → CSS value string).
struct WebAnimKeyframe {
    float offset = 0.0f;
    TimingFunction easing;  // linear
    bool hasEasing = false;
    std::vector<std::pair<std::string, std::string>> props;
};

enum class WebAnimDirection { Normal, Reverse, Alternate, AlternateReverse };
enum class WebAnimFill { None, Forwards, Backwards, Both };

// Idle only after cancel(); a canceled record is kept so play() can restart
// it (per spec), but it applies nothing and never ticks.
enum class WebAnimState { Idle, Running, Paused, Finished };

// Web Animations §5.5: a finished, forwards-filling script animation whose
// every property a later one of the same kind overrides is removed.
enum class WebAnimReplaceState { Active, Removed, Persisted };

// The effect's phase at a time (Web Animations §4.6), with the current
// iteration while active.
enum class WebAnimPhase { Idle, Before, Active, After };

// effect.updateTiming() members set by script. A CSS animation stops taking
// those members from its animation-* longhands once script has set them.
enum WebAnimTimingField : uint32_t {
    kTimingDuration = 1u << 0,
    kTimingDelay = 1u << 1,
    kTimingEndDelay = 1u << 2,
    kTimingIterations = 1u << 3,
    kTimingDirection = 1u << 4,
    kTimingFill = 1u << 5,
    kTimingEasing = 1u << 6,
};

struct WebAnimation {
    uint64_t id = 0;

    // Target element — resolve via WebAnimationManager::resolveElement()
    // before ANY dereference. Raw pointers here may dangle.
    dom::Element* elem = nullptr;
    uint32_t nodeId = 0;
    dom::Document* doc = nullptr;

    // Effect (timing + keyframes).
    std::vector<WebAnimKeyframe> keyframes; // sorted by offset
    double duration = 0;     // ms per iteration
    double delay = 0;        // ms
    double endDelay = 0;     // ms
    double iterations = 1.0; // may be INFINITY
    WebAnimDirection direction = WebAnimDirection::Normal;
    WebAnimFill fill = WebAnimFill::None;
    TimingFunction easing;   // whole-iteration; WAAPI default linear
    // Easing of the 0% / 100% keyframes synthesized for a property no
    // explicit end keyframe names: linear for script animations, the
    // animation-timing-function for CSS ones.
    TimingFunction implicitKeyframeEasing;

    // Playback state (simplified spec model: exactly one of startTime /
    // holdTime resolves currentTime; holdTime wins).
    double playbackRate = 1.0;
    WebAnimState state = WebAnimState::Running;
    bool hasStartTime = false;
    double startTime = 0;    // engine ms at which currentTime was 0 (rate-adjusted)
    bool hasHoldTime = false;
    double holdTime = 0;     // frozen currentTime (paused / finished / rate 0)
    WebAnimReplaceState replaceState = WebAnimReplaceState::Active;

    // --- CSS animation (CSSAnimation) ------------------------------------
    bool isCssAnimation = false;
    // Still tied to its animation-name layer: markup controls it and it
    // sorts with the CSS animations. Cleared when the layer goes away.
    bool cssOwned = false;
    std::string cssName;      // animationName
    int cssLayer = 0;         // position in the animation-name list
    // Script called play()/pause(): animation-play-state no longer applies.
    bool cssPlayOverride = false;
    uint32_t cssTimingOverride = 0; // WebAnimTimingField bits

    // --- CSS transition (CSSTransition) -----------------------------------
    // A running transition is a record too: two keyframes (the start and end
    // values), the transition-timing-function as the start keyframe's easing,
    // a backwards fill for the delay. cssOwned while the transition is running.
    bool isCssTransition = false;
    std::string cssProperty;      // transitionProperty
    // Transition generation (CSS Transitions 2 composite order): the style
    // change that started it. Ties sort by property name.
    uint64_t cssGeneration = 0;

    // Stopped contributing (finished without a forwards fill, or cancelled)
    // since its element last re-resolved: that re-resolve's drop from the
    // animated value to the base value is no style change to transition.
    // Cleared by applyOverrides at the re-resolve.
    mutable bool settling = false;

    bool wrapped = false;        // a JS Animation object exists for it
    bool orphaned = false;       // JS wrapper finalized; GC record when it stops contributing
    bool finishNotified = false; // finish event already queued/delivered

    double activeDuration() const;               // duration * iterations (inf ok)
    double endTimeMs() const;                    // max(delay + activeDuration + endDelay, 0)
    std::optional<double> currentTimeMs(double now) const; // nullopt = unresolved (idle)
    // Phase at `now`; `iteration` is the current iteration index when
    // Active, and `localTime` the current time (both untouched when Idle).
    WebAnimPhase phaseAt(double now, double& iteration, double& localTime) const;
    bool fillsForwards() const {
        return fill == WebAnimFill::Forwards || fill == WebAnimFill::Both;
    }
    bool fillsBackwards() const {
        return fill == WebAnimFill::Backwards || fill == WebAnimFill::Both;
    }
};

class WebAnimationManager {
public:
    WebAnimationManager();
    ~WebAnimationManager();
    WebAnimationManager(const WebAnimationManager&) = delete;
    WebAnimationManager& operator=(const WebAnimationManager&) = delete;

    // True iff `m` points at a live manager. Wrapper finalizers use this so a
    // finalizer running after engine teardown (members die before the JS
    // runtime) never touches a destroyed manager.
    static bool isLive(const WebAnimationManager* m);

    // Create a record targeting `elem`, auto-playing from `now` (element.animate
    // semantics). Caller fills effect fields on the returned record.
    WebAnimation& create(dom::Element* elem, double now);

    WebAnimation* find(uint64_t id);
    const WebAnimation* find(uint64_t id) const;

    // Drop a record outright (its owner is done with it and nothing wraps it).
    void erase(uint64_t id);

    // A JS wrapper now exists for the record.
    void noteWrapped(uint64_t id);
    // JS wrapper finalized: drop the record unless it is still holding a
    // forwards-fill (a finished forwards animation keeps applying its final
    // value even with no JS reference, per spec — such records are marked
    // orphaned and reclaimed when their element/document goes away) or is a
    // CSS animation its markup still owns.
    void releaseFromWrapper(uint64_t id);

    // --- playback control (main thread, layout idle) ---------------------
    void play(WebAnimation& a, double now);
    void pause(WebAnimation& a, double now);
    void cancelOp(WebAnimation& a);                 // → Idle, applies nothing
    void finishOp(WebAnimation& a);                 // seek to boundary, → Finished
    void reverse(WebAnimation& a, double now);      // flip rate + play
    void seek(WebAnimation& a, double t, double now);
    void setRate(WebAnimation& a, double rate, double now);
    void setStartTime(WebAnimation& a, double st, double now);
    void setCurrentTime(WebAnimation& a, double ct, double now);
    // The effect's timing changed (updateTiming, or a CSS animation's
    // longhands): a finished animation whose end moved past its current time
    // runs again.
    void timingChanged(WebAnimation& a, double now);

    // The markup that owned a CSS animation stopped naming it (or its element
    // went away): the animation is canceled and handed to script if a JS
    // object holds it, dropped otherwise. The cancel is queued for the
    // wrapper (finished rejection + oncancel).
    void cancelFromMarkup(uint64_t id);

    // A CSS transition that ran to completion: its markup lets go of it. The
    // record stays (finished) while a JS object holds it, else it is dropped.
    void disownFromMarkup(uint64_t id);

    // Some animation on `elem` other than a CSS transition animates `prop`
    // and is in effect (running, paused, or holding a forwards fill), or just
    // stopped (settling). A value an animation drives, or drops on ending, is
    // not a style change a transition answers.
    bool animatesProperty(const dom::Element* elem, const std::string& prop) const;

    // Fresh play state including boundary crossings between ticks:
    // "idle" | "running" | "paused" | "finished".
    const char* playState(const WebAnimation& a, double now) const;

    // Live, still-document-owned element for this record, or null. Never
    // dereferences a dangling pointer (generation-checked via nodeId).
    dom::Element* resolveElement(const WebAnimation& a) const;

    // --- engine seams (mirror TransitionManager) -------------------------
    // Advance clocks, detect finishes (queued for main-thread delivery),
    // remove replaced animations, and collect this tick's active elements.
    // Returns true if anything is running or just completed (document should
    // keep pumping frames).
    bool tick(double now);

    // Inject interpolated values into a computed style, after the CSS
    // transition overrides. Composite order: CSS animations first, in
    // animation-name order, then script animations in creation order, so a
    // later one wins per property.
    void applyOverrides(dom::Element* elem, htmlayout::css::ComputedStyle& style,
                        double now) const;

    // What `a` alone contributes on top of `base` right now (commitStyles):
    // false when its effect is not in effect.
    bool effectValues(const WebAnimation& a, const htmlayout::css::ComputedStyle& base,
                      double now,
                      std::vector<std::pair<std::string, std::string>>& out) const;

    // Element has ≥1 running (not paused/finished/idle) animation — drives the
    // per-frame re-resolve (animatingSelf) and compositor promotion.
    bool hasActive(dom::Element* elem) const;

    // Union of properties across running animations on `elem` is a non-empty
    // subset of `allowed` (compositor-promotion hint, cf. TransitionManager).
    bool activeAnimatesOnly(dom::Element* elem,
                            const std::set<std::string>& allowed) const;

    const std::vector<dom::Element*>& activeThisTick() const { return activeThisTick_; }

    // Events queued by tick() / cancelFromMarkup, drained on the main thread
    // and delivered to the JS wrappers: finish (finished promise + onfinish),
    // cancel (rejection + oncancel), remove (onremove).
    std::vector<uint64_t> takeFinishedEvents() { return std::move(pendingFinished_); }
    std::vector<uint64_t> takeCanceledEvents() { return std::move(pendingCanceled_); }
    std::vector<uint64_t> takeRemovedEvents() { return std::move(pendingRemoved_); }

    // Animation ids relevant to `elem` (running/paused, or finished while
    // holding a forwards fill), composite order. For getAnimations().
    std::vector<uint64_t> animationsFor(const dom::Element* elem, double now) const;
    // Every relevant animation, in document composite order: CSS transitions,
    // then CSS animations, each by the tree order of their elements, then
    // script animations by creation.
    std::vector<uint64_t> allAnimations(double now) const;

    // Whole-document teardown — Element*/Document* keys are about to dangle.
    void clearAll() {
        records_.clear();
        byElem_.clear();
        activeThisTick_.clear();
        pendingFinished_.clear();
        pendingCanceled_.clear();
        pendingRemoved_.clear();
    }

private:
    bool isRelevant(const WebAnimation& a, double now) const;
    bool applyOne(const WebAnimation& a, htmlayout::css::ComputedStyle& style,
                  double now) const;
    void eraseIndex(const WebAnimation& a);
    // Records on `elem`, composite order.
    std::vector<const WebAnimation*> stackFor(const dom::Element* elem, bool checkNode) const;
    void removeReplaced();

    std::unordered_map<uint64_t, WebAnimation> records_;
    // Element* used purely as a hash key (never dereferenced); entries carry a
    // nodeId generation check against address reuse.
    std::unordered_multimap<const dom::Element*, uint64_t> byElem_;
    std::vector<dom::Element*> activeThisTick_;
    std::vector<uint64_t> pendingFinished_;
    std::vector<uint64_t> pendingCanceled_;
    std::vector<uint64_t> pendingRemoved_;
    uint64_t nextId_ = 1;
};

// Composite order of two records on one element: CSS transitions the markup
// owns first (by generation, then property name), then CSS animations it owns
// in animation-name order, then everything else by creation.
bool compositeOrderLess(const WebAnimation& a, const WebAnimation& b);

// Compositor hint — true iff the element has at least one running animation
// (CSS transition, CSS animation or script) and every running one is confined
// to transform/opacity.
bool isTransformOpacityOnly(dom::Element* elem, const WebAnimationManager& web);

} // namespace bro::engine
