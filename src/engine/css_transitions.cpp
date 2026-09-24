#include "engine/css_transitions.h"
#include "dom/element.h"
#include "engine/css_interpolation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace bro::engine {

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
                TimingFunction tf;
                if (isTime) {
                    if (numIdx == 0) { dur = tok; ++numIdx; }
                    else { delay = tok; ++numIdx; }
                } else if (tryParseEasing(tok, tf)) {
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

        TimingFunction easing = TimingFunction::ease();
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
                        progress = tr.easing.apply(progress);
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
            progress = tr.easing.apply(progress);
            style[tr.property] = interpolate(tr.startValue, tr.endValue, progress, tr.property);
        }
    }
}

// ---------------------------------------------------------------------------
// Lifetime: forgetting elements whose storage is going away
// ---------------------------------------------------------------------------
//
// The manager keys on raw dom::Element* and dereferences the key (tick()
// calls markDirty() on completion; the queued events carry the pointer to
// dispatchEvent). Nothing else drops these entries, so an element removed while
// it still owns one leaves a dangling key that the next tick walks straight
// into. Document::freeNode() and the two wholesale-teardown paths call these.

void TransitionManager::forgetElement(dom::Element* elem) {
    if (!elem) return;
    elements_.erase(elem);
    activeThisTick_.erase(
        std::remove(activeThisTick_.begin(), activeThisTick_.end(), elem),
        activeThisTick_.end());
    pendingEvents_.erase(
        std::remove_if(pendingEvents_.begin(), pendingEvents_.end(),
                       [elem](const PendingCSSEvent& ev) { return ev.element == elem; }),
        pendingEvents_.end());
}

// Safe to dereference the keys: the callers run while the document's node
// storage is still intact.
void TransitionManager::forgetDocument(const dom::Document* doc) {
    if (!doc) return;
    auto belongs = [doc](const dom::Element* e) {
        return e && e->document() == doc;
    };
    for (auto it = elements_.begin(); it != elements_.end(); ) {
        if (belongs(it->first)) it = elements_.erase(it);
        else ++it;
    }
    activeThisTick_.erase(
        std::remove_if(activeThisTick_.begin(), activeThisTick_.end(), belongs),
        activeThisTick_.end());
    pendingEvents_.erase(
        std::remove_if(pendingEvents_.begin(), pendingEvents_.end(),
                       [&](const PendingCSSEvent& ev) { return belongs(ev.element); }),
        pendingEvents_.end());
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

} // namespace bro::engine
