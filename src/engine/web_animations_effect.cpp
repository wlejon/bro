// The effect half of the Web Animations model: composite order, and turning a
// record's keyframes + timing into computed-style values. Shared verbatim by
// script animations and CSS @keyframes animations, which is what keeps the two
// from drifting apart (they used to be two interpolators).

#include "engine/web_animations.h"
#include "engine/css_transitions.h"
#include "dom/element.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_set>

namespace bro::engine {

namespace {
// Animation class (Web Animations §5.4.2): owned CSS transitions, then owned
// CSS animations, then everything else. A record its markup let go of sorts
// with the script animations.
int animationClass(const WebAnimation& a) {
    if (!a.cssOwned) return 2;
    return a.isCssTransition ? 0 : 1;
}
}  // namespace

bool compositeOrderLess(const WebAnimation& a, const WebAnimation& b) {
    const int ca = animationClass(a), cb = animationClass(b);
    if (ca != cb) return ca < cb;
    if (ca == 0) {
        if (a.cssGeneration != b.cssGeneration) return a.cssGeneration < b.cssGeneration;
        if (a.cssProperty != b.cssProperty) return a.cssProperty < b.cssProperty;
    } else if (ca == 1 && a.cssLayer != b.cssLayer) {
        return a.cssLayer < b.cssLayer;
    }
    return a.id < b.id;
}

std::vector<const WebAnimation*> WebAnimationManager::stackFor(const dom::Element* elem,
                                                               bool checkNode) const {
    std::vector<const WebAnimation*> out;
    auto range = byElem_.equal_range(elem);
    for (auto it = range.first; it != range.second; ++it) {
        auto rIt = records_.find(it->second);
        if (rIt == records_.end()) continue;
        const WebAnimation& a = rIt->second;
        // Generation check: the map key is a raw pointer; a recycled address
        // must not inherit the old element's animations.
        if (a.elem != elem) continue;
        if (checkNode && a.nodeId != elem->nodeId()) continue;
        if (a.replaceState == WebAnimReplaceState::Removed) continue;
        out.push_back(&a);
    }
    std::sort(out.begin(), out.end(),
              [](const WebAnimation* x, const WebAnimation* y) { return compositeOrderLess(*x, *y); });
    return out;
}

bool WebAnimationManager::applyOne(const WebAnimation& a,
                                   htmlayout::css::ComputedStyle& style,
                                   double now) const {
    if (a.keyframes.empty()) return false;

    auto ctOpt = a.currentTimeMs(now);
    if (!ctOpt) return false; // idle
    double ct = *ctOpt;

    double activeDur = a.activeDuration();
    double localT = ct - a.delay;
    bool before = localT < 0;
    bool after = std::isfinite(activeDur) && localT >= activeDur;

    // Fill phases: nothing applies before the delay without a backwards fill,
    // nor after the active interval without a forwards fill.
    if (before && !a.fillsBackwards()) return false;
    if (after && !a.fillsForwards()) return false;

    // Iteration progress + current iteration index.
    double localProgress;
    int curIter;
    if (a.duration <= 0 || a.iterations <= 0) {
        localProgress = before ? 0.0 : 1.0;
        curIter = 0;
        if (!before && a.iterations > 1.0 && std::isfinite(a.iterations))
            curIter = std::max(0, static_cast<int>(std::ceil(a.iterations)) - 1);
        if (!before && a.iterations <= 0) localProgress = 0.0;
    } else if (before) {
        localProgress = 0.0;
        curIter = 0;
    } else {
        double overall = std::min(localT, std::isfinite(activeDur) ? activeDur : localT);
        double ip = overall / a.duration;
        curIter = static_cast<int>(ip);
        localProgress = ip - curIter;
        if (after) {
            // Land exactly on the final iteration's end progress.
            double frac = std::isfinite(a.iterations)
                              ? a.iterations - std::floor(a.iterations)
                              : 0.0;
            curIter = std::isfinite(a.iterations)
                          ? std::max(0, static_cast<int>(std::ceil(a.iterations)) - 1)
                          : curIter;
            localProgress = frac > 0 ? frac : 1.0;
        }
    }

    // Direction.
    bool rev = false;
    switch (a.direction) {
        case WebAnimDirection::Normal:           rev = false; break;
        case WebAnimDirection::Reverse:          rev = true; break;
        case WebAnimDirection::Alternate:        rev = (curIter % 2) != 0; break;
        case WebAnimDirection::AlternateReverse: rev = (curIter % 2) == 0; break;
    }
    if (rev) localProgress = 1.0 - localProgress;

    // Whole-iteration easing (options.easing), then per-keyframe easing below.
    // An overshooting curve may leave [0,1]; the end intervals extrapolate.
    float t = static_cast<float>(std::clamp(localProgress, 0.0, 1.0));
    t = a.easing.apply(t, before);

    // Per-property interpolation: each property collects its own stops from
    // the keyframes that declare it, with implicit endpoints synthesized from
    // the element's base (un-animated) value — the same rule for one-sided
    // @keyframes as for a one-keyframe element.animate().
    struct Stop {
        float offset;
        const std::string* value;
        const WebAnimKeyframe* kf; // null for implicit endpoints
    };

    // Union of animated properties, in first-seen order.
    std::vector<const std::string*> props;
    {
        std::unordered_set<std::string_view> seen;
        for (const auto& kf : a.keyframes)
            for (const auto& [p, v] : kf.props)
                if (seen.insert(p).second) props.push_back(&p);
    }

    auto baseValueFor = [&](const std::string& prop,
                            const std::string& ref) -> std::string {
        auto sIt = style.find(prop);
        if (sIt != style.end() && !sIt->second.empty() && sIt->second != "none")
            return sIt->second;
        std::string iv = cssInitialValueForProperty(prop, ref);
        return iv.empty() ? ref : iv;
    };

    std::vector<Stop> stops;
    for (const std::string* propPtr : props) {
        const std::string& prop = *propPtr;
        stops.clear();
        for (const auto& kf : a.keyframes) {
            for (const auto& [p, v] : kf.props) {
                if (p == prop) { stops.push_back({kf.offset, &v, &kf}); break; }
            }
        }
        if (stops.empty()) continue;

        // Implicit endpoints from the base value.
        std::string implicitStart, implicitEnd;
        bool hasImplicitStart = stops.front().offset > 0.0001f;
        bool hasImplicitEnd = stops.back().offset < 0.9999f;
        if (hasImplicitStart) implicitStart = baseValueFor(prop, *stops.front().value);
        if (hasImplicitEnd) implicitEnd = baseValueFor(prop, *stops.back().value);
        if (hasImplicitStart) stops.insert(stops.begin(), {0.0f, &implicitStart, nullptr});
        if (hasImplicitEnd) stops.push_back({1.0f, &implicitEnd, nullptr});

        if (stops.size() == 1) {
            style[prop] = *stops.front().value;
            continue;
        }

        // The interval holding t; outside [0,1] the end intervals extrapolate.
        size_t i = 0;
        if (t > 1.0f) {
            i = stops.size() - 2;
        } else if (t >= 0.0f) {
            while (i + 2 < stops.size() && stops[i + 1].offset <= t) ++i;
        }
        const Stop* b = &stops[i];
        const Stop* e = &stops[i + 1];

        float range = e->offset - b->offset;
        float segT = range > 0 ? (t - b->offset) / range : 1.0f;
        if (segT >= 0.0f && segT <= 1.0f) {
            // The interval's easing: its start keyframe's, or for a
            // synthesized start keyframe the record's implicit easing.
            if (b->kf) {
                if (b->kf->hasEasing) segT = b->kf->easing.apply(segT, before);
            } else {
                segT = a.implicitKeyframeEasing.apply(segT, before);
            }
        }

        style[prop] = TransitionManager::interpolate(*b->value, *e->value, segT, prop);
    }
    return true;
}

void WebAnimationManager::applyOverrides(dom::Element* elem,
                                         htmlayout::css::ComputedStyle& style,
                                         double now) const {
    auto range = byElem_.equal_range(elem);
    if (range.first == range.second) return;
    for (auto it = range.first; it != range.second; ++it) {
        auto rIt = records_.find(it->second);
        if (rIt != records_.end()) rIt->second.settling = false;
    }
    for (const WebAnimation* a : stackFor(elem, true)) applyOne(*a, style, now);
}

bool WebAnimationManager::effectValues(
    const WebAnimation& a, const htmlayout::css::ComputedStyle& base, double now,
    std::vector<std::pair<std::string, std::string>>& out) const {
    htmlayout::css::ComputedStyle style = base;
    if (!applyOne(a, style, now)) return false;
    std::unordered_set<std::string_view> seen;
    for (const auto& kf : a.keyframes) {
        for (const auto& [p, v] : kf.props) {
            if (!seen.insert(p).second) continue;
            auto it = style.find(p);
            if (it != style.end()) out.emplace_back(p, it->second);
        }
    }
    return true;
}

bool WebAnimationManager::activeAnimatesOnly(
    dom::Element* elem, const std::set<std::string>& allowed) const {
    auto range = byElem_.equal_range(elem);
    bool any = false;
    for (auto it = range.first; it != range.second; ++it) {
        auto rIt = records_.find(it->second);
        if (rIt == records_.end()) continue;
        const WebAnimation& a = rIt->second;
        if (a.elem != elem || a.nodeId != elem->nodeId()) continue;
        if (a.state != WebAnimState::Running) continue;
        any = true;
        for (const auto& kf : a.keyframes)
            for (const auto& [p, v] : kf.props)
                if (allowed.find(p) == allowed.end()) return false;
    }
    return any;
}

bool WebAnimationManager::animatesProperty(const dom::Element* elem,
                                           const std::string& prop) const {
    auto range = byElem_.equal_range(elem);
    for (auto it = range.first; it != range.second; ++it) {
        auto rIt = records_.find(it->second);
        if (rIt == records_.end()) continue;
        const WebAnimation& a = rIt->second;
        if (a.elem != elem || a.isCssTransition || !(a.settling || isRelevant(a, 0))) continue;
        for (const auto& kf : a.keyframes)
            for (const auto& [p, v] : kf.props)
                if (p == prop) return true;
    }
    return false;
}

bool isTransformOpacityOnly(dom::Element* elem, const WebAnimationManager& web) {
    static const std::set<std::string> allowed{"transform", "opacity"};
    return web.activeAnimatesOnly(elem, allowed);
}

} // namespace bro::engine
