// CSS transitions, the markup side. A running transition is a Web Animations
// record (web_animations.h), a CSSTransition to script; this file starts,
// retargets and cancels those records as an element's style changes and turns
// their phases into the transitionrun / transitionstart / transitionend /
// transitioncancel events.

#include "engine/css_transitions.h"
#include "dom/element.h"
#include "engine/css_interpolation.h"
#include "engine/web_animations.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bro::engine {

namespace {

// The element's transition-* lists, from the longhands or else the shorthand.
struct TransitionLists {
    std::vector<std::string> properties, durations, timings, delays;
};

TransitionLists parseTransitionLists(const htmlayout::css::ComputedStyle& style) {
    TransitionLists out;
    auto tpIt = style.find("transition-property");
    auto tdIt = style.find("transition-duration");
    if (tpIt != style.end() && tdIt != style.end() &&
        tpIt->second != "none" && tdIt->second != "0s") {
        out.properties = splitCSS(tpIt->second);
        out.durations = splitCSS(tdIt->second);
        auto tfIt = style.find("transition-timing-function");
        if (tfIt != style.end()) out.timings = splitCSS(tfIt->second);
        auto delIt = style.find("transition-delay");
        if (delIt != style.end()) out.delays = splitCSS(delIt->second);
        return out;
    }
    // The shorthand: transition: <property> <duration> [<timing>] [<delay>], ...
    auto trIt = style.find("transition");
    if (trIt == style.end() || trIt->second.empty() || trIt->second == "none") return out;
    for (auto& part : splitCSS(trIt->second)) {
        // Tokenize respecting parentheses so cubic-bezier(...) stays intact.
        std::vector<std::string> tokens;
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
        std::string prop = "all", dur = "0s", timing = "ease", delay = "0s";
        int numIdx = 0;
        for (auto& tok : tokens) {
            char* end = nullptr;
            std::strtof(tok.c_str(), &end);
            bool isTime = end != tok.c_str() &&
                          (std::string(end) == "s" || std::string(end) == "ms");
            TimingFunction tf;
            if (isTime) {
                (numIdx++ == 0 ? dur : delay) = tok;
            } else if (tryParseEasing(tok, tf)) {
                timing = tok;
            } else {
                prop = tok;
            }
        }
        out.properties.push_back(prop);
        out.durations.push_back(dur);
        out.timings.push_back(timing);
        out.delays.push_back(delay);
    }
    return out;
}

// Does transition-property entry `shorthand` name the computed longhand?
bool shorthandCovers(const std::string& shorthand, const std::string& longhand) {
    if (shorthand == "all" || shorthand == longhand) return true;
    // Simple prefix: "margin" covers "margin-top", etc.
    if (longhand.size() > shorthand.size() &&
        longhand.compare(0, shorthand.size(), shorthand) == 0 &&
        longhand[shorthand.size()] == '-')
        return true;
    // border-radius expands to border-{top,bottom}-{left,right}-radius
    return shorthand == "border-radius" && longhand.rfind("border-", 0) == 0 &&
           longhand.find("-radius") != std::string::npos;
}

// Index of the transition-property entry that covers `prop` (the last one
// naming it wins, as for any repeated list entry), or -1.
int matchIndex(const TransitionLists& l, const std::string& prop) {
    for (size_t i = l.properties.size(); i-- > 0;)
        if (shorthandCovers(l.properties[i], prop)) return static_cast<int>(i);
    return -1;
}

bool skipProperty(const std::string& prop) {
    return prop.rfind("transition", 0) == 0 || prop.rfind("animation", 0) == 0 ||
           prop == "display";
}

bool isDisplayNone(const htmlayout::css::ComputedStyle& style) {
    auto it = style.find("display");
    return it != style.end() && it->second == "none";
}

// Active time elapsed at `localTime`, clamped to the active interval.
double activeElapsed(const WebAnimation& rec, double localTime) {
    double t = std::max(localTime - rec.delay, 0.0);
    return std::min(t, rec.activeDuration());
}

// The transition's output progress at `now` (the timing function applied),
// for the reversing shortening factor.
double transformedProgress(const WebAnimation& rec, double now) {
    auto ct = rec.currentTimeMs(now);
    if (!ct || rec.duration <= 0 || rec.keyframes.empty()) return 1.0;
    double p = std::clamp((*ct - rec.delay) / rec.duration, 0.0, 1.0);
    return rec.keyframes.front().easing.apply(static_cast<float>(p));
}

template <class Vec, class Pred>
void eraseIf(Vec& v, Pred pred) {
    v.erase(std::remove_if(v.begin(), v.end(), pred), v.end());
}

}  // namespace

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

void TransitionManager::start(dom::Element* elem, ElementTransitions& et,
                              const std::string& prop, const std::string& from,
                              const std::string& to, double duration, double delay,
                              const TimingFunction& timing, double now,
                              const std::string& reversingAdjustedStart,
                              double shorteningFactor) {
    WebAnimation& rec = web_->create(elem, now);
    rec.isCssTransition = true;
    rec.cssOwned = true;
    rec.cssProperty = prop;
    rec.cssGeneration = generation_;
    rec.duration = duration;
    rec.delay = delay;
    // The start value holds through the delay (CSS Transitions 2: a
    // transition's effect fills backwards).
    rec.fill = WebAnimFill::Backwards;
    // The transition-timing-function eases the one keyframe interval; the
    // effect's own easing stays linear.
    WebAnimKeyframe k0, k1;
    k0.offset = 0.0f;
    k0.easing = timing;
    k0.hasEasing = true;
    k0.props.emplace_back(prop, from);
    k1.offset = 1.0f;
    k1.props.emplace_back(prop, to);
    rec.keyframes = {std::move(k0), std::move(k1)};
    rec.implicitKeyframeEasing = timing;

    RunningTransition run;
    run.property = prop;
    run.id = rec.id;
    run.endValue = to;
    run.reversingAdjustedStart = reversingAdjustedStart;
    run.shorteningFactor = shorteningFactor;
    et.running.push_back(std::move(run));
}

void TransitionManager::cancel(dom::Element* elem, RunningTransition& run, double now) {
    if (!run.id || !web_) return;
    if (WebAnimation* rec = web_->find(run.id)) {
        if (rec->cssOwned) {
            double iteration = 0, local = 0;
            WebAnimPhase ph = rec->phaseAt(now, iteration, local);
            // transitionrun was dispatched for it (or is still queued) and it
            // has not ended: it ends in a transitioncancel.
            if (run.lastPhase >= 0 && (ph == WebAnimPhase::Before || ph == WebAnimPhase::Active))
                pendingEvents_.push_back({elem, "transitioncancel", run.property,
                                          activeElapsed(*rec, local) / 1000.0});
        }
        web_->cancelFromMarkup(run.id);
    }
    run.id = 0;
}

bool TransitionManager::advance(dom::Element* elem, ElementTransitions& et,
                                RunningTransition& run, double now) {
    WebAnimation* rec = web_->find(run.id);
    if (!rec || !rec->cssOwned) return true;

    double iteration = 0, local = 0;
    const WebAnimPhase ph = rec->phaseAt(now, iteration, local);
    const double active = rec->activeDuration();
    const double intervalStart = std::max(std::min(-rec->delay, active), 0.0) / 1000.0;
    const double intervalEnd =
        std::max(std::min(rec->endTimeMs() - rec->delay, active), 0.0) / 1000.0;
    auto queue = [&](const char* type, double elapsed) {
        pendingEvents_.push_back({elem, type, run.property, elapsed});
    };

    // CSS Transitions 2 §6.1: the events follow the phase, so a script seek,
    // finish() or cancel() drives them as well as the clock.
    WebAnimPhase prev = static_cast<WebAnimPhase>(run.lastPhase);
    if (run.lastPhase < 0) {
        if (ph != WebAnimPhase::Idle) queue("transitionrun", intervalStart);
        prev = WebAnimPhase::Before;
    }
    switch (ph) {
        case WebAnimPhase::Active:
            if (prev == WebAnimPhase::Before) queue("transitionstart", intervalStart);
            else if (prev == WebAnimPhase::After) queue("transitionstart", intervalEnd);
            break;
        case WebAnimPhase::After:
            if (prev == WebAnimPhase::Before) queue("transitionstart", intervalStart);
            if (prev != WebAnimPhase::After) queue("transitionend", intervalEnd);
            break;
        case WebAnimPhase::Before:
            if (prev == WebAnimPhase::Active) {
                queue("transitionend", intervalStart);
            } else if (prev == WebAnimPhase::After) {
                queue("transitionstart", intervalEnd);
                queue("transitionend", intervalStart);
            }
            break;
        case WebAnimPhase::Idle:
            if (run.lastPhase >= 0 &&
                (prev == WebAnimPhase::Before || prev == WebAnimPhase::Active))
                queue("transitioncancel", run.lastElapsed);
            break;
    }
    run.lastPhase = static_cast<int>(ph);
    if (ph != WebAnimPhase::Idle) run.lastElapsed = activeElapsed(*rec, local) / 1000.0;

    // Done: it ran to its end, or script cancelled it. Either way the element
    // now shows the end value, which no new transition must start from.
    if (ph != WebAnimPhase::After && ph != WebAnimPhase::Idle) return false;
    et.completed.emplace_back(run.property, run.endValue);
    web_->disownFromMarkup(run.id);
    run.id = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Style changes
// ---------------------------------------------------------------------------

void TransitionManager::onStyleChange(dom::Element* elem,
                                      const htmlayout::css::ComputedStyle& oldStyle,
                                      htmlayout::css::ComputedStyle& newStyle,
                                      double currentTime) {
    if (!web_) return;
    const TransitionLists lists = parseTransitionLists(newStyle);
    const bool declared = !lists.properties.empty() && !lists.durations.empty();
    auto eit = elements_.find(elem);
    if (!declared && eit == elements_.end()) return;

    ElementTransitions& et = elements_[elem];
    ++generation_;

    // Settle what script or the clock finished since the last look.
    eraseIf(et.running, [&](RunningTransition& r) { return advance(elem, et, r, currentTime); });

    // Not rendered, before or after this change: nothing transitions, and
    // what was running is cancelled (CSS Transitions §3).
    const bool hidden = isDisplayNone(newStyle) || isDisplayNone(oldStyle) ||
                        inDisplayNoneSubtree(elem->parentElement());
    if (hidden || !declared) {
        for (RunningTransition& r : et.running) cancel(elem, r, currentTime);
        elements_.erase(elem);
        return;
    }

    std::vector<std::pair<std::string, std::string>> completed = std::move(et.completed);
    et.completed.clear();

    for (auto& [prop, newVal] : newStyle) {
        if (skipProperty(prop)) continue;
        const int idx = matchIndex(lists, prop);
        auto runIt = std::find_if(et.running.begin(), et.running.end(),
                                  [&](const RunningTransition& r) { return r.property == prop; });
        if (idx < 0) {
            // transition-property no longer names it.
            if (runIt != et.running.end()) {
                cancel(elem, *runIt, currentTime);
                et.running.erase(runIt);
            }
            continue;
        }

        auto oldIt = oldStyle.find(prop);
        std::string oldVal = oldIt != oldStyle.end() ? oldIt->second : std::string();
        const size_t i = static_cast<size_t>(idx);
        const double duration = std::max(0.0, parseDurationMs(lists.durations[i % lists.durations.size()]));
        const double delay =
            lists.delays.empty() ? 0.0 : parseDurationMs(lists.delays[i % lists.delays.size()]);
        const TimingFunction timing = lists.timings.empty()
                                          ? TimingFunction::ease()
                                          : parseTimingFunction(lists.timings[i % lists.timings.size()]);
        const bool canRun = duration + delay > 0 && duration > 0;

        if (runIt != et.running.end()) {
            RunningTransition& run = *runIt;
            if (run.endValue == newVal) {
                // Already heading there: the record keeps interpolating.
                newStyle[prop] = oldVal;
                continue;
            }
            // The target moved mid-transition: the running transition is
            // cancelled and a new one starts from where it had got to.
            WebAnimation* rec = web_->find(run.id);
            std::string current = oldVal;
            std::vector<std::pair<std::string, std::string>> vals;
            if (rec && web_->effectValues(*rec, newStyle, currentTime, vals) && !vals.empty())
                current = vals.front().second;
            const bool reversing = run.reversingAdjustedStart == newVal;
            double factor = 1.0;
            if (reversing && rec) {
                // Reversing shortening (CSS Transitions §3.1): going back to
                // where it came from takes as long as the way there took.
                const double p = transformedProgress(*rec, currentTime);
                factor = std::clamp(std::abs(p * run.shorteningFactor + 1.0 - run.shorteningFactor),
                                    0.0, 1.0);
            }
            const std::string adjustedStart = reversing ? run.endValue : current;
            cancel(elem, run, currentTime);
            et.running.erase(runIt);
            if (current == newVal || !canRun) continue;
            start(elem, et, prop, current, newVal, duration * factor,
                  delay < 0 ? delay * factor : delay, timing, currentTime, adjustedStart, factor);
            newStyle[prop] = oldVal;
            continue;
        }

        // No transition running. One that just completed onto this value is
        // not a change to transition from its last frame.
        auto doneIt = std::find_if(completed.begin(), completed.end(),
                                   [&](const auto& c) { return c.first == prop; });
        if (doneIt != completed.end() && doneIt->second == newVal) continue;

        if (oldVal == newVal || !canRun) continue;
        // A value an animation drives is not a style change.
        if (web_->animatesProperty(elem, prop)) continue;
        if (oldVal.empty()) {
            // Substitute CSS initial values so transitions from "nothing" work.
            oldVal = initialValueForProperty(prop, newVal);
            if (oldVal.empty() || oldVal == newVal) continue;
        }
        start(elem, et, prop, oldVal, newVal, duration, delay, timing, currentTime, oldVal, 1.0);
        // It shows its start value until the record's interpolation applies.
        newStyle[prop] = oldVal;
    }

    // A running transition whose property left the computed style entirely.
    eraseIf(et.running, [&](RunningTransition& r) {
        if (newStyle.find(r.property) != newStyle.end()) return false;
        cancel(elem, r, currentTime);
        return true;
    });

    if (et.running.empty()) elements_.erase(elem);
}

void TransitionManager::displayToggled(dom::Element* ancestor) {
    for (auto& [elem, et] : elements_)
        if (!et.running.empty() && isElementAncestor(ancestor, elem)) elem->markDirty();
}

bool TransitionManager::tick(double currentTime) {
    if (!web_) return false;
    const size_t before = pendingEvents_.size();
    bool anyCompleted = false;

    for (auto it = elements_.begin(); it != elements_.end();) {
        dom::Element* elem = it->first;
        ElementTransitions& et = it->second;
        // display:none on an ancestor takes the element out of the
        // rendering without re-resolving it: its transitions are cancelled.
        // Re-resolved (hidden), it drops the value the transition last applied
        // for its own, so being shown again is no style change.
        if (inDisplayNoneSubtree(elem)) {
            for (RunningTransition& r : et.running) cancel(elem, r, currentTime);
            if (!et.running.empty()) elem->markDirty();
            it = elements_.erase(it);
            continue;
        }
        const size_t n = et.running.size();
        eraseIf(et.running, [&](RunningTransition& r) { return advance(elem, et, r, currentTime); });
        if (et.running.size() != n) {
            // One more re-resolve so the finished transition's last
            // interpolated value gives way to the value it ended on.
            elem->markDirty();
            anyCompleted = true;
        }
        if (et.running.empty() && et.completed.empty()) it = elements_.erase(it);
        else ++it;
    }
    return anyCompleted || pendingEvents_.size() != before;
}

// ---------------------------------------------------------------------------
// Lifetime: forgetting elements whose storage is going away
// ---------------------------------------------------------------------------
//
// The manager keys on raw dom::Element* and dereferences the key (tick()
// calls markDirty() on completion; the queued events carry the pointer to
// dispatchEvent). Document::freeNode() and the two wholesale-teardown paths
// call these. Removal cancels a transition per CSS; nothing is owed an event.

void TransitionManager::forgetElement(dom::Element* elem) {
    if (!elem) return;
    auto it = elements_.find(elem);
    if (it != elements_.end()) {
        for (RunningTransition& r : it->second.running)
            if (r.id && web_) web_->cancelFromMarkup(r.id);
        elements_.erase(it);
    }
    eraseIf(pendingEvents_, [elem](const PendingCSSEvent& ev) { return ev.element == elem; });
}

// Safe to dereference the keys: the callers run while the document's node
// storage is still intact.
void TransitionManager::forgetDocument(const dom::Document* doc) {
    if (!doc) return;
    auto belongs = [doc](const dom::Element* e) { return e && e->document() == doc; };
    for (auto it = elements_.begin(); it != elements_.end();) {
        if (!belongs(it->first)) { ++it; continue; }
        for (RunningTransition& r : it->second.running)
            if (r.id && web_) web_->cancelFromMarkup(r.id);
        it = elements_.erase(it);
    }
    eraseIf(pendingEvents_, [&](const PendingCSSEvent& ev) { return belongs(ev.element); });
}

} // namespace bro::engine
