#include "engine/web_animations.h"
#include "engine/css_transitions.h"
#include "dom/document.h"
#include "dom/element.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace bro::engine {

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

std::optional<double> WebAnimation::currentTimeMs(double now) const {
    if (hasHoldTime) return holdTime;
    if (hasStartTime) return (now - startTime) * playbackRate;
    return std::nullopt;
}

WebAnimPhase WebAnimation::phaseAt(double now, double& iteration, double& localTime) const {
    if (state == WebAnimState::Idle) return WebAnimPhase::Idle;
    auto ct = currentTimeMs(now);
    if (!ct) return WebAnimPhase::Idle;
    localTime = *ct;
    // Web Animations §4.6.
    const double active = activeDuration();
    const double end = endTimeMs();
    const double beforeActive = std::max(std::min(delay, end), 0.0);
    const double activeAfter = std::max(std::min(delay + active, end), 0.0);
    const bool backwards = playbackRate < 0;
    if (localTime < beforeActive || (backwards && localTime == beforeActive))
        return WebAnimPhase::Before;
    if (localTime > activeAfter || (!backwards && localTime == activeAfter))
        return WebAnimPhase::After;
    iteration = duration > 0 ? std::floor((localTime - delay) / duration) : 0.0;
    if (std::isfinite(iterations) && iterations > 0)
        iteration = std::min(iteration, std::ceil(iterations) - 1.0);
    return WebAnimPhase::Active;
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
    a.nodeId = elem ? elem->nodeId() : 0;
    a.doc = elem ? elem->document() : nullptr;
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

const WebAnimation* WebAnimationManager::find(uint64_t id) const {
    auto it = records_.find(id);
    return it != records_.end() ? &it->second : nullptr;
}

void WebAnimationManager::eraseIndex(const WebAnimation& a) {
    auto range = byElem_.equal_range(a.elem);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == a.id) { byElem_.erase(it); break; }
    }
}

void WebAnimationManager::erase(uint64_t id) {
    auto it = records_.find(id);
    if (it == records_.end()) return;
    eraseIndex(it->second);
    records_.erase(it);
}

void WebAnimationManager::noteWrapped(uint64_t id) {
    if (WebAnimation* a = find(id)) a->wrapped = true;
}

void WebAnimationManager::releaseFromWrapper(uint64_t id) {
    auto it = records_.find(id);
    if (it == records_.end()) return;
    WebAnimation& a = it->second;
    a.wrapped = false;
    // Its markup still owns a CSS animation: the record lives on, and a
    // later getAnimations() mints a fresh wrapper for it.
    if (a.cssOwned) return;
    // A finished forwards-filling animation keeps applying its final value
    // with no JS reference (per spec); everything else is reclaimable now.
    bool holdsFill = a.state == WebAnimState::Finished && a.fillsForwards() &&
                     a.replaceState != WebAnimReplaceState::Removed;
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

void WebAnimationManager::cancelFromMarkup(uint64_t id) {
    WebAnimation* a = find(id);
    if (!a) return;
    const bool wasActive = a->state != WebAnimState::Idle;
    cancelOp(*a);
    a->cssOwned = false;
    if (!a->wrapped) {
        erase(id);
        return;
    }
    if (wasActive) pendingCanceled_.push_back(id);
}

dom::Element* WebAnimationManager::resolveElement(const WebAnimation& a) const {
    if (!a.elem || !dom::Document::isLiveDocument(a.doc)) return nullptr;
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
    auto ctOpt = a.currentTimeMs(now);
    double ct = ctOpt.value_or(0);
    bool has = ctOpt.has_value();
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
    auto ctOpt = a.currentTimeMs(now);
    double ct = ctOpt ? *ctOpt : (a.playbackRate < 0 ? a.endTimeMs() : 0);
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

void WebAnimationManager::timingChanged(WebAnimation& a, double now) {
    if (a.state != WebAnimState::Finished || a.playbackRate == 0) return;
    auto ctOpt = a.currentTimeMs(now);
    if (!ctOpt) return;
    const double ct = *ctOpt;
    const double end = a.endTimeMs();
    const bool stillFinished = (a.playbackRate > 0 && ct >= end) ||
                               (a.playbackRate < 0 && ct <= 0);
    if (stillFinished) return;
    // The end moved past where it stopped: it runs on from there.
    a.startTime = now - ct / a.playbackRate;
    a.hasStartTime = true;
    a.hasHoldTime = false;
    a.state = WebAnimState::Running;
    a.finishNotified = false;
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

void WebAnimationManager::setStartTime(WebAnimation& a, double st, double now) {
    (void)now;
    a.startTime = st;
    a.hasStartTime = true;
    a.hasHoldTime = false;
    if (a.state == WebAnimState::Paused) a.state = WebAnimState::Running;
}

void WebAnimationManager::setCurrentTime(WebAnimation& a, double ct, double now) {
    // "Silently set the current time": a paused (or idle) animation keeps a
    // HOLD time — seeking it must not start the clock again. An idle one is
    // paused at the new time, as a seek on the web leaves it.
    if (a.state == WebAnimState::Paused || a.state == WebAnimState::Idle) {
        a.holdTime = ct;
        a.hasHoldTime = true;
        a.hasStartTime = false;
        a.state = WebAnimState::Paused;
        a.finishNotified = false;
        return;
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
    if (a.state == WebAnimState::Finished) {
        // Seeking a finished animation back inside its interval runs it again.
        const bool stillFinished = (a.playbackRate > 0 && ct >= a.endTimeMs()) ||
                                   (a.playbackRate < 0 && ct <= 0);
        if (stillFinished) {
            a.holdTime = ct;
            a.hasHoldTime = true;
            a.hasStartTime = false;
        } else {
            a.state = WebAnimState::Running;
            a.finishNotified = false;
        }
    }
}

void WebAnimationManager::setRate(WebAnimation& a, double rate, double now) {
    auto ctOpt = a.currentTimeMs(now);
    a.playbackRate = rate;
    if (a.state == WebAnimState::Running && ctOpt) {
        double ct = *ctOpt;
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
    auto ctOpt = a.currentTimeMs(now);
    if (!ctOpt) return "idle";
    double ct = *ctOpt;
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

        if (!a.elem) { ++it; continue; }  // new Animation(): no target yet
        if (!dom::Document::isLiveDocument(a.doc)) {
            eraseIndex(a);
            it = records_.erase(it);
            continue;
        }

        dom::Element* elem = resolveElement(a);

        if (a.state == WebAnimState::Running) {
            if (auto ctOpt = a.currentTimeMs(now)) {
                double ct = *ctOpt;
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
                } else if (a.cssOwned && elem && inDisplayNoneSubtree(elem)) {
                    // A CSS animation under display:none does not drive frames:
                    // an infinite spinner in a hidden overlay must not pin the
                    // document on the re-layout path. Its clock runs on, so it
                    // is where it should be when shown again.
                } else {
                    anyActive = true;
                    if (elem) activeThisTick_.push_back(elem);
                }
            }
        }

        // Records their markup owns live as long as the markup names them.
        if (a.cssOwned) { ++it; continue; }

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

    removeReplaced();
    return anyActive || anyCompleted;
}

// Web Animations §5.5 "Replacing animations": a finished, forwards-filling
// script animation whose every property is also animated by a later such
// animation on the same element contributes nothing any more. It is removed
// (a `remove` event for script, replaceState "removed") unless persist() was
// called — without this, every fire-and-forget `fill: 'forwards'` animation
// stayed in the stack for the life of the element.
void WebAnimationManager::removeReplaced() {
    auto replaceable = [](const WebAnimation& a) {
        return !a.cssOwned && a.state == WebAnimState::Finished && a.fillsForwards() &&
               a.replaceState != WebAnimReplaceState::Removed && a.elem;
    };
    std::unordered_set<const dom::Element*> elems;
    for (const auto& [id, a] : records_)
        if (replaceable(a) && a.replaceState == WebAnimReplaceState::Active) elems.insert(a.elem);
    if (elems.empty()) return;

    std::vector<uint64_t> doomed;
    for (const dom::Element* elem : elems) {
        std::vector<const WebAnimation*> stack;
        auto range = byElem_.equal_range(elem);
        for (auto it = range.first; it != range.second; ++it) {
            const WebAnimation* a = find(it->second);
            if (a && replaceable(*a)) stack.push_back(a);
        }
        if (stack.size() < 2) continue;
        std::sort(stack.begin(), stack.end(),
                  [](const WebAnimation* x, const WebAnimation* y) { return compositeOrderLess(*x, *y); });
        // Walk from the top of the stack down, collecting what is covered.
        std::unordered_set<std::string> covered;
        for (size_t i = stack.size(); i-- > 0;) {
            const WebAnimation& a = *stack[i];
            bool allCovered = !covered.empty();
            for (const auto& kf : a.keyframes)
                for (const auto& [p, v] : kf.props)
                    if (!covered.count(p)) allCovered = false;
            if (allCovered && a.replaceState == WebAnimReplaceState::Active) {
                doomed.push_back(a.id);
                continue;
            }
            for (const auto& kf : a.keyframes)
                for (const auto& [p, v] : kf.props) covered.insert(p);
        }
    }
    for (uint64_t id : doomed) {
        WebAnimation* a = find(id);
        if (!a) continue;
        a->replaceState = WebAnimReplaceState::Removed;
        if (a->wrapped) pendingRemoved_.push_back(id);
        else erase(id);
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

bool WebAnimationManager::isRelevant(const WebAnimation& a, double now) const {
    (void)now;
    if (a.replaceState == WebAnimReplaceState::Removed) return false;
    if (a.state == WebAnimState::Running || a.state == WebAnimState::Paused)
        return true;
    return a.state == WebAnimState::Finished && a.fillsForwards();
}

std::vector<uint64_t> WebAnimationManager::animationsFor(const dom::Element* elem,
                                                         double now) const {
    std::vector<uint64_t> out;
    for (const WebAnimation* a : stackFor(elem, false))
        if (isRelevant(*a, now)) out.push_back(a->id);
    return out;
}

std::vector<uint64_t> WebAnimationManager::allAnimations(double now) const {
    // CSS animations first, then script animations, each in creation order —
    // the document-wide composite order without a tree walk.
    std::vector<const WebAnimation*> list;
    for (const auto& [id, a] : records_) {
        if (a.elem && isRelevant(a, now)) list.push_back(&a);
    }
    std::sort(list.begin(), list.end(), [](const WebAnimation* x, const WebAnimation* y) {
        if (x->cssOwned != y->cssOwned) return x->cssOwned;
        return x->id < y->id;
    });
    std::vector<uint64_t> out;
    out.reserve(list.size());
    for (const WebAnimation* a : list) out.push_back(a->id);
    return out;
}

} // namespace bro::engine
