#include "engine/web_animations.h"
#include "engine/css_transitions.h"
#include "dom/document.h"
#include "dom/element.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace bro::engine {

using bromath::ccubicEase;

// ---------------------------------------------------------------------------
// WebAnimation timing model
// ---------------------------------------------------------------------------

double WebAnimation::activeDuration() const {
    if (duration <= 0 || iterations <= 0) return 0;
    return duration * iterations; // inf iterations → inf
}

double WebAnimation::endTimeMs() const {
    double end = delay + activeDuration() + endDelay;
    return end > 0 ? end : 0;
}

bool WebAnimation::currentTimeMs(double now, double* out) const {
    if (hasHoldTime) { *out = holdTime; return true; }
    if (hasStartTime) { *out = (now - startTime) * playbackRate; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Live-manager registry (wrapper-finalizer teardown safety)
// ---------------------------------------------------------------------------

static std::unordered_set<const WebAnimationManager*>& liveManagers() {
    static std::unordered_set<const WebAnimationManager*> s;
    return s;
}

WebAnimationManager::WebAnimationManager() { liveManagers().insert(this); }
WebAnimationManager::~WebAnimationManager() { liveManagers().erase(this); }

bool WebAnimationManager::isLive(const WebAnimationManager* m) {
    return m && liveManagers().count(m) > 0;
}

// ---------------------------------------------------------------------------
// Record management
// ---------------------------------------------------------------------------

WebAnimation& WebAnimationManager::create(dom::Element* elem, double now) {
    uint64_t id = nextId_++;
    WebAnimation& a = records_[id];
    a.id = id;
    a.elem = elem;
    a.nodeId = elem->nodeId();
    a.doc = elem->document();
    a.startTime = now;
    a.hasStartTime = true;
    a.state = WebAnimState::Running;
    byElem_.emplace(elem, id);
    return a;
}

WebAnimation* WebAnimationManager::find(uint64_t id) {
    auto it = records_.find(id);
    return it != records_.end() ? &it->second : nullptr;
}

void WebAnimationManager::eraseIndex(const WebAnimation& a) {
    auto range = byElem_.equal_range(a.elem);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == a.id) { byElem_.erase(it); break; }
    }
}

void WebAnimationManager::releaseFromWrapper(uint64_t id) {
    auto it = records_.find(id);
    if (it == records_.end()) return;
    WebAnimation& a = it->second;
    // A finished forwards-filling animation keeps applying its final value
    // with no JS reference (per spec); everything else is reclaimable now.
    bool holdsFill = a.state == WebAnimState::Finished && a.fillsForwards();
    bool stillTicking = a.state == WebAnimState::Running ||
                        a.state == WebAnimState::Paused;
    if (holdsFill || stillTicking) {
        // Running/paused records normally can't get here (the bindings pin the
        // wrapper while a finish can still be delivered), but a runtime
        // teardown drops those pins — keep the record inert-safe either way.
        a.orphaned = true;
        return;
    }
    eraseIndex(a);
    records_.erase(it);
}

dom::Element* WebAnimationManager::resolveElement(const WebAnimation& a) const {
    if (!dom::Document::isLiveDocument(a.doc)) return nullptr;
    dom::Node* n = a.doc->resolveNode(a.elem, a.nodeId);
    if (!n) return nullptr;
    // Nodes queued in pendingFrees_ still resolve (memory alive) but are on
    // their way out — treat as gone so we never dirty or route them.
    if (!a.doc->ownsNode(n)) return nullptr;
    return static_cast<dom::Element*>(n);
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

void WebAnimationManager::play(WebAnimation& a, double now) {
    double ct = 0;
    bool has = a.currentTimeMs(now, &ct);
    double end = a.endTimeMs();
    if (a.playbackRate >= 0) {
        // Auto-rewind: playing from past-the-end (or idle) restarts at 0.
        if (!has || ct < 0 || ct >= end) ct = 0;
    } else {
        if (!has || ct <= 0 || ct > end)
            ct = std::isfinite(end) ? end : 0; // can't seek to an infinite end
    }
    if (a.playbackRate != 0) {
        a.startTime = now - ct / a.playbackRate;
        a.hasStartTime = true;
        a.hasHoldTime = false;
    } else {
        a.holdTime = ct;
        a.hasHoldTime = true;
        a.hasStartTime = false;
    }
    a.state = WebAnimState::Running;
    a.finishNotified = false;
}

void WebAnimationManager::pause(WebAnimation& a, double now) {
    if (a.state == WebAnimState::Paused) return;
    double ct = 0;
    if (!a.currentTimeMs(now, &ct))
        ct = a.playbackRate < 0 ? a.endTimeMs() : 0;
    a.holdTime = ct;
    a.hasHoldTime = true;
    a.hasStartTime = false;
    a.state = WebAnimState::Paused;
}

void WebAnimationManager::cancelOp(WebAnimation& a) {
    a.hasStartTime = false;
    a.hasHoldTime = false;
    a.state = WebAnimState::Idle;
    a.finishNotified = false;
}

void WebAnimationManager::finishOp(WebAnimation& a) {
    double end = a.endTimeMs(); // caller ensured finite for forward rates
    a.holdTime = a.playbackRate < 0 ? 0 : end;
    a.hasHoldTime = true;
    a.hasStartTime = false;
    a.state = WebAnimState::Finished;
    a.finishNotified = true; // delivered synchronously by the binding
}

void WebAnimationManager::reverse(WebAnimation& a, double now) {
    a.playbackRate = -a.playbackRate;
    play(a, now);
}

void WebAnimationManager::seek(WebAnimation& a, double t, double now) {
    if (a.state == WebAnimState::Paused || !a.hasStartTime || a.playbackRate == 0) {
        a.holdTime = t;
        a.hasHoldTime = true;
    } else {
        a.startTime = now - t / a.playbackRate;
        a.hasStartTime = true;
        a.hasHoldTime = false;
    }
    if (a.state == WebAnimState::Idle) {
        // Seeking an idle (canceled) animation starts it paused at t — close
        // enough to spec's "set the hold time" behavior for our subset.
        a.state = WebAnimState::Paused;
    } else if (a.state == WebAnimState::Finished) {
        double end = a.endTimeMs();
        bool stillFinished = (a.playbackRate > 0 && t >= end) ||
                             (a.playbackRate < 0 && t <= 0);
        if (!stillFinished) {
            a.state = WebAnimState::Running;
            a.finishNotified = false;
            if (a.playbackRate != 0) {
                a.startTime = now - t / a.playbackRate;
                a.hasStartTime = true;
                a.hasHoldTime = false;
            }
        }
    }
}

void WebAnimationManager::setRate(WebAnimation& a, double rate, double now) {
    double ct = 0;
    bool has = a.currentTimeMs(now, &ct);
    a.playbackRate = rate;
    if (a.state == WebAnimState::Running && has) {
        if (rate != 0) {
            a.startTime = now - ct / rate;
            a.hasStartTime = true;
            a.hasHoldTime = false;
        } else {
            a.holdTime = ct;
            a.hasHoldTime = true;
            a.hasStartTime = false;
        }
    }
}

const char* WebAnimationManager::playState(const WebAnimation& a, double now) const {
    if (a.state == WebAnimState::Idle) return "idle";
    if (a.state == WebAnimState::Paused) return "paused";
    if (a.state == WebAnimState::Finished) return "finished";
    double ct = 0;
    if (!a.currentTimeMs(now, &ct)) return "idle";
    double end = a.endTimeMs();
    // Boundary crossed but tick() hasn't formalized it yet — report fresh.
    if ((a.playbackRate > 0 && std::isfinite(end) && ct >= end) ||
        (a.playbackRate < 0 && ct <= 0))
        return "finished";
    return "running";
}

// ---------------------------------------------------------------------------
// Engine seams
// ---------------------------------------------------------------------------

bool WebAnimationManager::tick(double now) {
    bool anyActive = false;
    bool anyCompleted = false;
    activeThisTick_.clear();

    for (auto it = records_.begin(); it != records_.end(); ) {
        WebAnimation& a = it->second;

        if (!dom::Document::isLiveDocument(a.doc)) {
            eraseIndex(a);
            it = records_.erase(it);
            continue;
        }

        dom::Element* elem = resolveElement(a);

        if (a.state == WebAnimState::Running) {
            double ct = 0;
            if (a.currentTimeMs(now, &ct)) {
                double end = a.endTimeMs();
                bool finished =
                    (a.playbackRate > 0 && std::isfinite(end) && ct >= end) ||
                    (a.playbackRate < 0 && ct <= 0);
                if (finished) {
                    a.holdTime = a.playbackRate < 0 ? 0 : end;
                    a.hasHoldTime = true;
                    a.hasStartTime = false;
                    a.state = WebAnimState::Finished;
                    if (!a.finishNotified) {
                        a.finishNotified = true;
                        pendingFinished_.push_back(a.id);
                    }
                    // One more re-resolve so the override either settles on the
                    // fill-forwards value or (fill:none) drops back to base —
                    // same settling markDirty the CSS managers issue.
                    if (elem) elem->markDirty();
                    anyCompleted = true;
                } else {
                    anyActive = true;
                    if (elem) activeThisTick_.push_back(elem);
                }
            }
        }

        // GC orphaned records that no longer contribute anything.
        if (a.orphaned && a.state != WebAnimState::Running &&
            a.state != WebAnimState::Paused &&
            !(a.state == WebAnimState::Finished && a.fillsForwards())) {
            eraseIndex(a);
            it = records_.erase(it);
            continue;
        }
        // Orphaned records whose element is gone hold nothing worth keeping.
        if (a.orphaned && !elem) {
            eraseIndex(a);
            it = records_.erase(it);
            continue;
        }

        ++it;
    }

    return anyActive || anyCompleted;
}

void WebAnimationManager::applyOne(const WebAnimation& a,
                                   htmlayout::css::ComputedStyle& style,
                                   double now) const {
    if (a.keyframes.empty()) return;

    double ct = 0;
    if (!a.currentTimeMs(now, &ct)) return; // idle

    double activeDur = a.activeDuration();
    double localT = ct - a.delay;
    bool before = localT < 0;
    bool after = std::isfinite(activeDur) && localT >= activeDur;

    // Fill phases: nothing applies before the delay without a backwards fill,
    // nor after the active interval without a forwards fill.
    if (before && !a.fillsBackwards()) return;
    if (after && !a.fillsForwards()) return;

    // Iteration progress + current iteration index.
    double localProgress;
    int curIter;
    if (a.duration <= 0 || a.iterations <= 0) {
        localProgress = before ? 0.0 : 1.0;
        curIter = 0;
        if (!before && a.iterations > 1.0 && std::isfinite(a.iterations))
            curIter = std::max(0, static_cast<int>(std::ceil(a.iterations)) - 1);
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
    float t = static_cast<float>(std::clamp(localProgress, 0.0, 1.0));
    t = ccubicEase(a.easing, t);
    t = std::clamp(t, 0.0f, 1.0f);

    // Per-property interpolation: each property collects its own stops from
    // the keyframes that declare it, with implicit endpoints synthesized from
    // the element's base (un-animated) value — the same rule the CSS
    // animation path uses for one-sided @keyframes.
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

        // Bracket t.
        const Stop* b = &stops.front();
        const Stop* e = &stops.back();
        if (t <= stops.front().offset) {
            b = e = &stops.front();
        } else if (t >= stops.back().offset) {
            b = e = &stops.back();
        } else {
            for (size_t i = 0; i + 1 < stops.size(); ++i) {
                if (t >= stops[i].offset && t <= stops[i + 1].offset) {
                    b = &stops[i];
                    e = &stops[i + 1];
                    break;
                }
            }
        }

        float range = e->offset - b->offset;
        float segT = range > 0 ? (t - b->offset) / range : 1.0f;
        if (b->kf && b->kf->hasEasing)
            segT = std::clamp(ccubicEase(b->kf->easing, segT), 0.0f, 1.0f);

        style[prop] = TransitionManager::interpolate(*b->value, *e->value, segT, prop);
    }
}

void WebAnimationManager::applyOverrides(dom::Element* elem,
                                         htmlayout::css::ComputedStyle& style,
                                         double now) const {
    auto range = byElem_.equal_range(elem);
    if (range.first == range.second) return;

    // Creation order: later-created animations apply last and win per property.
    std::vector<uint64_t> ids;
    for (auto it = range.first; it != range.second; ++it) ids.push_back(it->second);
    std::sort(ids.begin(), ids.end());

    for (uint64_t id : ids) {
        auto rIt = records_.find(id);
        if (rIt == records_.end()) continue;
        const WebAnimation& a = rIt->second;
        // Generation check: the map key is a raw pointer; a recycled address
        // must not inherit the old element's animations.
        if (a.elem != elem || a.nodeId != elem->nodeId()) continue;
        applyOne(a, style, now);
    }
}

bool WebAnimationManager::hasActive(dom::Element* elem) const {
    auto range = byElem_.equal_range(elem);
    for (auto it = range.first; it != range.second; ++it) {
        auto rIt = records_.find(it->second);
        if (rIt == records_.end()) continue;
        const WebAnimation& a = rIt->second;
        if (a.elem != elem || a.nodeId != elem->nodeId()) continue;
        if (a.state == WebAnimState::Running) return true;
    }
    return false;
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

bool WebAnimationManager::isRelevant(const WebAnimation& a, double now) const {
    (void)now;
    if (a.state == WebAnimState::Running || a.state == WebAnimState::Paused)
        return true;
    return a.state == WebAnimState::Finished && a.fillsForwards();
}

std::vector<uint64_t> WebAnimationManager::animationsFor(const dom::Element* elem,
                                                         double now) const {
    std::vector<uint64_t> out;
    auto range = byElem_.equal_range(elem);
    for (auto it = range.first; it != range.second; ++it) {
        auto rIt = records_.find(it->second);
        if (rIt == records_.end()) continue;
        const WebAnimation& a = rIt->second;
        if (a.elem != elem) continue;
        if (isRelevant(a, now)) out.push_back(a.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<uint64_t> WebAnimationManager::allAnimations(double now) const {
    std::vector<uint64_t> out;
    for (const auto& [id, a] : records_) {
        if (isRelevant(a, now)) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool isTransformOpacityOnly(dom::Element* elem,
                            const AnimationManager& anim,
                            const TransitionManager& trans,
                            const WebAnimationManager& web) {
    const std::set<std::string> allowed{"transform", "opacity"};
    bool A = anim.hasActive(elem);
    bool T = trans.hasActive(elem);
    bool W = web.hasActive(elem);
    if (!A && !T && !W) return false;
    if (A && !anim.activeAnimatesOnly(elem, allowed)) return false;
    if (T && !trans.activeAnimatesOnly(elem, allowed)) return false;
    if (W && !web.activeAnimatesOnly(elem, allowed)) return false;
    return true;
}

} // namespace bro::engine
