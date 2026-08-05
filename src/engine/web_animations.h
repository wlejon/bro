#pragma once

// Web Animations API (element.animate) — the script-driven sibling of the CSS
// transition/animation managers in css_transitions.h. JS creates records via
// the bindings (src/js/web_animation_bindings.cpp); the manager plugs into the
// exact same seams the CSS managers use: applyOverrides() during style
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

#include <bromath/curves.h>
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
class TransitionManager;

// One keyframe: computed offset in [0,1], optional per-keyframe easing
// (applies to the segment from this keyframe to the next), and the declared
// properties (kebab-case name → CSS value string).
struct WebAnimKeyframe {
    float offset = 0.0f;
    bromath::CubicEase easing{0.0f, 0.0f, 1.0f, 1.0f}; // linear
    bool hasEasing = false;
    std::vector<std::pair<std::string, std::string>> props;
};

enum class WebAnimDirection { Normal, Reverse, Alternate, AlternateReverse };
enum class WebAnimFill { None, Forwards, Backwards, Both };

// Idle only after cancel(); a canceled record is kept so play() can restart
// it (per spec), but it applies nothing and never ticks.
enum class WebAnimState { Idle, Running, Paused, Finished };

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
    bromath::CubicEase easing{0.0f, 0.0f, 1.0f, 1.0f}; // whole-iteration; WAAPI default linear

    // Playback state (simplified spec model: exactly one of startTime /
    // holdTime resolves currentTime; holdTime wins).
    double playbackRate = 1.0;
    WebAnimState state = WebAnimState::Running;
    bool hasStartTime = false;
    double startTime = 0;    // engine ms at which currentTime was 0 (rate-adjusted)
    bool hasHoldTime = false;
    double holdTime = 0;     // frozen currentTime (paused / finished / rate 0)

    bool orphaned = false;       // JS wrapper finalized; GC record when it stops contributing
    bool finishNotified = false; // finish event already queued/delivered

    double activeDuration() const;               // duration * iterations (inf ok)
    double endTimeMs() const;                    // max(delay + activeDuration + endDelay, 0)
    std::optional<double> currentTimeMs(double now) const; // nullopt = unresolved (idle)
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

    // JS wrapper finalized: drop the record unless it is still holding a
    // forwards-fill (a finished forwards animation keeps applying its final
    // value even with no JS reference, per spec — such records are marked
    // orphaned and reclaimed when their element/document goes away).
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

    // Fresh play state including boundary crossings between ticks:
    // "idle" | "running" | "paused" | "finished".
    const char* playState(const WebAnimation& a, double now) const;

    // Live, still-document-owned element for this record, or null. Never
    // dereferences a dangling pointer (generation-checked via nodeId).
    dom::Element* resolveElement(const WebAnimation& a) const;

    // --- engine seams (mirror TransitionManager) -------------------------
    // Advance clocks, detect finishes (queued for main-thread delivery), and
    // collect this tick's active elements. Returns true if anything is
    // running or just completed (document should keep pumping frames).
    bool tick(double now);

    // Inject interpolated values into a computed style. Called after the CSS
    // transition/animation overrides — script animations sit above both in
    // composite order. Multiple animations on one element apply in creation
    // order, so the last-created wins per property.
    void applyOverrides(dom::Element* elem, htmlayout::css::ComputedStyle& style,
                        double now) const;

    // Element has ≥1 running (not paused/finished/idle) animation — drives the
    // per-frame re-resolve (animatingSelf) and compositor promotion.
    bool hasActive(dom::Element* elem) const;

    // Union of properties across running animations on `elem` is a non-empty
    // subset of `allowed` (compositor-promotion hint, cf. TransitionManager).
    bool activeAnimatesOnly(dom::Element* elem,
                            const std::set<std::string>& allowed) const;

    const std::vector<dom::Element*>& activeThisTick() const { return activeThisTick_; }

    // Finish events queued by tick(), drained on the main thread and delivered
    // to the JS wrappers (finished promise + onfinish).
    std::vector<uint64_t> takeFinishedEvents() { return std::move(pendingFinished_); }

    // Animation ids relevant to `elem` (running/paused, or finished while
    // holding a forwards fill), creation order. For getAnimations().
    std::vector<uint64_t> animationsFor(const dom::Element* elem, double now) const;
    std::vector<uint64_t> allAnimations(double now) const;

    // Whole-document teardown — Element*/Document* keys are about to dangle.
    void clearAll() {
        records_.clear();
        byElem_.clear();
        activeThisTick_.clear();
        pendingFinished_.clear();
    }

private:
    bool isRelevant(const WebAnimation& a, double now) const;
    void applyOne(const WebAnimation& a, htmlayout::css::ComputedStyle& style,
                  double now) const;
    void eraseIndex(const WebAnimation& a);

    std::unordered_map<uint64_t, WebAnimation> records_;
    // Element* used purely as a hash key (never dereferenced); entries carry a
    // nodeId generation check against address reuse.
    std::unordered_multimap<const dom::Element*, uint64_t> byElem_;
    std::vector<dom::Element*> activeThisTick_;
    std::vector<uint64_t> pendingFinished_;
    uint64_t nextId_ = 1;
};

// Extended compositor hint over all three animation sources — true iff the
// element has at least one active animation/transition and every active one
// (CSS animation, CSS transition, or script animation) is confined to
// transform/opacity. Supersedes the two-manager overload in css_transitions.h
// at the layout-thread promotion site.
bool isTransformOpacityOnly(dom::Element* elem,
                            const AnimationManager& anim,
                            const TransitionManager& trans,
                            const WebAnimationManager& web);

} // namespace bro::engine
