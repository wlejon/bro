#include "engine/css_transitions.h"
#include "dom/element.h"
#include "engine/css_interpolation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace bro::engine {

using bromath::CubicEase;
using bromath::ccubicEase;

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

// ---------------------------------------------------------------------------
// Lifetime: forgetting elements whose storage is going away
// ---------------------------------------------------------------------------
//
// Both managers key on raw dom::Element* and both dereference the key (tick()
// calls markDirty() on completion; the queued events carry the pointer to
// dispatchEvent). Nothing else drops these entries, so an element removed while
// it still owns one leaves a dangling key that the next tick walks straight
// into. Document::freeNode() and the two wholesale-teardown paths call these.

// Erase `elem` from a manager's three raw-pointer holders. Shared by both
// managers because the members are shaped identically.
template <typename Map>
static void forgetElementIn(Map& elements,
                            std::vector<dom::Element*>& activeThisTick,
                            std::vector<PendingCSSEvent>& pendingEvents,
                            dom::Element* elem) {
    if (!elem) return;
    elements.erase(elem);
    activeThisTick.erase(
        std::remove(activeThisTick.begin(), activeThisTick.end(), elem),
        activeThisTick.end());
    pendingEvents.erase(
        std::remove_if(pendingEvents.begin(), pendingEvents.end(),
                       [elem](const PendingCSSEvent& ev) { return ev.element == elem; }),
        pendingEvents.end());
}

// Same, for every element owned by `doc`. Safe to dereference the keys: the
// callers run while the document's node storage is still intact.
template <typename Map>
static void forgetDocumentIn(Map& elements,
                             std::vector<dom::Element*>& activeThisTick,
                             std::vector<PendingCSSEvent>& pendingEvents,
                             const dom::Document* doc) {
    if (!doc) return;
    auto belongs = [doc](const dom::Element* e) {
        return e && e->document() == doc;
    };
    for (auto it = elements.begin(); it != elements.end(); ) {
        if (belongs(it->first)) it = elements.erase(it);
        else ++it;
    }
    activeThisTick.erase(
        std::remove_if(activeThisTick.begin(), activeThisTick.end(), belongs),
        activeThisTick.end());
    pendingEvents.erase(
        std::remove_if(pendingEvents.begin(), pendingEvents.end(),
                       [&](const PendingCSSEvent& ev) { return belongs(ev.element); }),
        pendingEvents.end());
}

void TransitionManager::forgetElement(dom::Element* elem) {
    forgetElementIn(elements_, activeThisTick_, pendingEvents_, elem);
}

void TransitionManager::forgetDocument(const dom::Document* doc) {
    forgetDocumentIn(elements_, activeThisTick_, pendingEvents_, doc);
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

    // The cascade expands the `animation` shorthand into its longhands
    // (htmlayout expandShorthand), so the longhands are the whole story. Each
    // is a comma list with one entry per animation layer; this manager runs
    // one animation per element, the first layer's.
    auto firstLayer = [](const std::string& s) {
        int depth = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '(') ++depth;
            else if (s[i] == ')') --depth;
            else if (s[i] == ',' && depth == 0) {
                std::string head = s.substr(0, i);
                while (!head.empty() && head.back() == ' ') head.pop_back();
                return head;
            }
        }
        return s;
    };
    auto longhand = [&](const char* prop) -> std::string {
        auto it = newStyle.find(prop);
        return it != newStyle.end() ? firstLayer(it->second) : std::string();
    };
    animName = longhand("animation-name");
    if (animName.empty() || animName == "none") {
        // animation-name has been cleared. Reset the previousName memo so
        // that re-applying the same animation later (e.g. by re-adding a
        // class) triggers a fresh start, per CSS Animations §4.2.
        auto eit = elements_.find(elem);
        if (eit != elements_.end()) eit->second.previousName.clear();
        return;
    }
    durStr = longhand("animation-duration");
    timingStr = longhand("animation-timing-function");
    delayStr = longhand("animation-delay");
    iterStr = longhand("animation-iteration-count");
    directionStr = longhand("animation-direction");
    fillModeStr = longhand("animation-fill-mode");
    playStateStr = longhand("animation-play-state");

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
        applyKeyframeInterpolation(kf, anim, currentTime, style);
    }
}

void AnimationManager::forgetElement(dom::Element* elem) {
    forgetElementIn(elements_, activeThisTick_, pendingEvents_, elem);
}

void AnimationManager::forgetDocument(const dom::Document* doc) {
    forgetDocumentIn(elements_, activeThisTick_, pendingEvents_, doc);
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
