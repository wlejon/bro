// CSS @keyframes animations, the markup side. The animations themselves are
// Web Animations records (web_animations.h); this file maps an element's
// animation-* longhands onto them and turns their phases into the
// animationstart / animationiteration / animationend / animationcancel events.

#include "engine/css_transitions.h"
#include "engine/css_interpolation.h"
#include "engine/web_animations.h"
#include "dom/element.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string_view>

namespace bro::engine {

// Per CSS Animations §4: setting display:none on an element (or any ancestor)
// takes it out of the rendering. Its animations keep their records (they
// resume, at the time their clock reached, if it is shown again) but must not
// report themselves active — otherwise a single infinite animation on a
// hidden element (a load spinner in a display:none overlay, say) pins the
// whole document on the re-layout + re-raster path every frame.
bool inDisplayNoneSubtree(dom::Element* elem) {
    for (dom::Element* e = elem; e; e = e->parentElement()) {
        const auto& cs = e->computedStyle();
        auto it = cs.find("display");
        if (it != cs.end() && it->second == "none") return true;
    }
    return false;
}

namespace {

constexpr const char* kLonghands[] = {
    "animation-name",           "animation-duration",  "animation-timing-function",
    "animation-delay",          "animation-iteration-count", "animation-direction",
    "animation-fill-mode",      "animation-play-state",
};

constexpr const char* kTimingProp = "animation-timing-function";

size_t hashCombine(size_t h, size_t v) {
    return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

size_t hashBlock(const htmlayout::css::KeyframeBlock& kb) {
    std::hash<std::string_view> hs;
    std::hash<float> hf;
    size_t h = kb.stops.size();
    for (const auto& stop : kb.stops) {
        h = hashCombine(h, hf(stop.offset));
        for (const auto& d : stop.declarations) {
            h = hashCombine(h, hs(d.property));
            h = hashCombine(h, hs(d.value));
        }
    }
    return h;
}

// The @keyframes rule as Web Animations keyframes. Every keyframe carries its
// interval's easing: its own animation-timing-function when it names one,
// else the layer's — a CSS animation eases each keyframe interval, never the
// whole iteration (the effect easing stays linear).
std::vector<WebAnimKeyframe> buildKeyframes(const htmlayout::css::KeyframeBlock& kb,
                                            const TimingFunction& timing) {
    std::vector<WebAnimKeyframe> out;
    out.reserve(kb.stops.size());
    for (const auto& stop : kb.stops) {
        WebAnimKeyframe kf;
        kf.offset = stop.offset;
        kf.easing = timing;
        kf.hasEasing = true;
        for (const auto& d : stop.declarations) {
            if (d.property == kTimingProp) kf.easing = parseTimingFunction(d.value);
            else kf.props.emplace_back(d.property, d.value);
        }
        out.push_back(std::move(kf));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const WebAnimKeyframe& a, const WebAnimKeyframe& b) { return a.offset < b.offset; });
    return out;
}

// One layer's values from the comma lists; a shorter list repeats.
struct LayerSpec {
    double duration = 0;
    TimingFunction timing = TimingFunction::ease();
    double delay = 0;
    double iterations = 1;
    WebAnimDirection direction = WebAnimDirection::Normal;
    WebAnimFill fill = WebAnimFill::None;
    bool paused = false;
};

const std::string& pick(const std::vector<std::string>& list, size_t i) {
    static const std::string empty;
    return list.empty() ? empty : list[i % list.size()];
}

LayerSpec layerSpec(const std::vector<std::vector<std::string>>& lists, size_t i) {
    LayerSpec s;
    s.duration = std::max(0.0, parseDurationMs(pick(lists[1], i)));
    const std::string& tf = pick(lists[2], i);
    if (!tf.empty()) s.timing = parseTimingFunction(tf);
    s.delay = parseDurationMs(pick(lists[3], i));
    const std::string& it = pick(lists[4], i);
    if (it == "infinite") {
        s.iterations = INFINITY;
    } else if (!it.empty()) {
        char* end = nullptr;
        double v = std::strtod(it.c_str(), &end);
        if (end != it.c_str() && v >= 0) s.iterations = v;
    }
    const std::string& dir = pick(lists[5], i);
    if (dir == "reverse") s.direction = WebAnimDirection::Reverse;
    else if (dir == "alternate") s.direction = WebAnimDirection::Alternate;
    else if (dir == "alternate-reverse") s.direction = WebAnimDirection::AlternateReverse;
    const std::string& fill = pick(lists[6], i);
    if (fill == "forwards") s.fill = WebAnimFill::Forwards;
    else if (fill == "backwards") s.fill = WebAnimFill::Backwards;
    else if (fill == "both") s.fill = WebAnimFill::Both;
    s.paused = pick(lists[7], i) == "paused";
    return s;
}

// Apply a layer's timing to its record, member by member, leaving alone what
// script set through effect.updateTiming(). True if anything changed.
bool applyTiming(WebAnimation& rec, const LayerSpec& s) {
    const uint32_t ov = rec.cssTimingOverride;
    bool changed = false;
    auto set = [&](uint32_t bit, auto& field, const auto& value) {
        if ((ov & bit) || field == value) return;
        field = value;
        changed = true;
    };
    set(kTimingDuration, rec.duration, s.duration);
    set(kTimingDelay, rec.delay, s.delay);
    set(kTimingIterations, rec.iterations, s.iterations);
    set(kTimingDirection, rec.direction, s.direction);
    set(kTimingFill, rec.fill, s.fill);
    return changed;
}

// Active time elapsed at `localTime`, clamped to the active interval — an
// animation event's elapsedTime is measured in it.
double activeElapsed(const WebAnimation& rec, double localTime) {
    double t = std::max(localTime - rec.delay, 0.0);
    double active = rec.activeDuration();
    return std::isfinite(active) ? std::min(t, active) : t;
}

}  // namespace

const htmlayout::css::KeyframeBlock* AnimationManager::findKeyframes(const std::string& name) const {
    if (!keyframes_) return nullptr;
    // The last @keyframes of a name wins, as for any at-rule redefinition.
    const htmlayout::css::KeyframeBlock* found = nullptr;
    for (auto& kf : *keyframes_) {
        if (kf.name == name) found = &kf;
    }
    return found;
}

void AnimationManager::dropLayer(dom::Element* elem, CssAnimationLayer& layer, double now,
                                 bool fireCancel) {
    if (!layer.id || !web_) return;
    if (WebAnimation* rec = web_->find(layer.id)) {
        if (fireCancel && rec->cssOwned) {
            // An animation that had not ended stops without an animationend:
            // animationcancel (CSS Animations 2, event dispatch).
            double iteration = 0, local = 0;
            WebAnimPhase ph = rec->phaseAt(now, iteration, local);
            if (ph == WebAnimPhase::Before || ph == WebAnimPhase::Active) {
                pendingEvents_.push_back({elem, "animationcancel", layer.name,
                                          activeElapsed(*rec, local) / 1000.0});
            }
        }
        web_->cancelFromMarkup(layer.id);
    }
    layer.id = 0;
}

void AnimationManager::onStyleChange(dom::Element* elem,
                                     const htmlayout::css::ComputedStyle& newStyle,
                                     double currentTime) {
    if (!web_) return;

    // The cascade expands the `animation` shorthand into its longhands
    // (htmlayout expandShorthand), so the longhands are the whole story. Each
    // is a comma list with one entry per animation layer.
    std::string signature;
    std::vector<std::vector<std::string>> lists;
    lists.reserve(std::size(kLonghands));
    for (const char* prop : kLonghands) {
        auto it = newStyle.find(prop);
        const std::string& v = it != newStyle.end() ? it->second : std::string();
        signature += v;
        signature += '\x1f';
        lists.push_back(v.empty() ? std::vector<std::string>{} : splitCSS(v));
    }
    const std::vector<std::string>& names = lists[0];
    const bool none = std::all_of(names.begin(), names.end(),
                                  [](const std::string& n) { return n.empty() || n == "none"; });

    auto eit = elements_.find(elem);
    if (eit == elements_.end() && none) return;
    ElementAnimations& ea = elements_[elem];
    // Unchanged longhands: the per-frame re-resolve of an animating element,
    // or a finished animation whose name is still set (which must not
    // restart it — an animation starts only when animation-name changes).
    if (ea.signature == signature) return;
    ea.signature = signature;

    // Match the new name list against the layers the element has: a layer
    // whose name stays in the list keeps running (re-timed if its longhands
    // changed); duplicates match from the end of the list. A layer whose name
    // leaves the list is cancelled — removing a class that set the animation
    // stops it, and taking it off and putting it back restarts it.
    std::vector<CssAnimationLayer> old = std::move(ea.layers);
    std::vector<bool> used(old.size(), false);
    std::vector<CssAnimationLayer> next(names.size());
    for (size_t i = names.size(); i-- > 0;) {
        next[i].name = names[i];
        if (names[i].empty() || names[i] == "none") continue;
        for (size_t j = old.size(); j-- > 0;) {
            if (!used[j] && old[j].name == names[i]) {
                used[j] = true;
                next[i] = std::move(old[j]);
                break;
            }
        }
    }
    for (size_t j = 0; j < old.size(); ++j)
        if (!used[j]) dropLayer(elem, old[j], currentTime, /*fireCancel=*/true);

    for (size_t i = 0; i < next.size(); ++i) {
        CssAnimationLayer& layer = next[i];
        if (layer.name.empty() || layer.name == "none") continue;
        const LayerSpec spec = layerSpec(lists, i);
        WebAnimation* rec = layer.id ? web_->find(layer.id) : nullptr;

        if (!rec) {
            layer.id = 0;
            const htmlayout::css::KeyframeBlock* kb = findKeyframes(layer.name);
            if (!kb) continue;  // no @keyframes of that name: nothing runs
            WebAnimation& created = web_->create(elem, currentTime);
            created.isCssAnimation = true;
            created.cssOwned = true;
            created.cssName = layer.name;
            created.cssLayer = static_cast<int>(i);
            applyTiming(created, spec);
            created.implicitKeyframeEasing = spec.timing;
            created.keyframes = buildKeyframes(*kb, spec.timing);
            layer.id = created.id;
            layer.block = kb;
            layer.blockHash = hashBlock(*kb);
            layer.timing = spec.timing;
            layer.lastPhase = static_cast<int>(WebAnimPhase::Idle);
            // Born paused: the clock holds at the start. A negative delay
            // still pins a deterministic mid-animation frame.
            if (spec.paused) web_->pause(created, currentTime);
            continue;
        }

        rec->cssLayer = static_cast<int>(i);
        bool changed = applyTiming(*rec, spec);
        // animation-timing-function eases the keyframe intervals, which
        // effect.updateTiming({easing}) does not touch (that is the effect's
        // own, whole-iteration easing), so it always follows the markup.
        if (layer.timing != spec.timing) {
            layer.timing = spec.timing;
            rec->implicitKeyframeEasing = spec.timing;
            if (layer.block) rec->keyframes = buildKeyframes(*layer.block, spec.timing);
        }
        if (changed) web_->timingChanged(*rec, currentTime);

        // animation-play-state, unless script took playback over with
        // play()/pause(). A cancelled animation stays cancelled.
        if (!rec->cssPlayOverride) {
            if (spec.paused && rec->state == WebAnimState::Running) {
                web_->pause(*rec, currentTime);
            } else if (!spec.paused && rec->state == WebAnimState::Paused) {
                web_->play(*rec, currentTime);
            }
        }
    }

    ea.layers = std::move(next);
    if (none) elements_.erase(elem);
}

bool AnimationManager::tick(double currentTime) {
    if (!web_) return false;
    const size_t before = pendingEvents_.size();

    for (auto& [elem, ea] : elements_) {
        // Hidden: no events until it is shown; the phase catches up then.
        if (inDisplayNoneSubtree(elem)) continue;

        for (CssAnimationLayer& layer : ea.layers) {
            if (!layer.id) {
                // A name with no @keyframes yet: once a stylesheet defines
                // it, the next resolve reconciles the element and starts it.
                if (!layer.name.empty() && layer.name != "none" && findKeyframes(layer.name)) {
                    ea.signature.clear();
                    elem->markDirty();
                }
                continue;
            }
            WebAnimation* rec = web_->find(layer.id);
            if (!rec || !rec->cssOwned) { layer.id = 0; continue; }

            // The @keyframes rule may have been replaced or edited since the
            // record was built (a stylesheet swap): follow it.
            if (const htmlayout::css::KeyframeBlock* kb = findKeyframes(layer.name)) {
                size_t h = hashBlock(*kb);
                if (kb != layer.block || h != layer.blockHash) {
                    layer.block = kb;
                    layer.blockHash = h;
                    rec->keyframes = buildKeyframes(*kb, layer.timing);
                    elem->markDirty();
                }
            }

            // Events from the phase change since the last tick (CSS
            // Animations 2 §4.2). A script seek or pause drives them too.
            double iteration = 0, local = 0;
            const WebAnimPhase ph = rec->phaseAt(currentTime, iteration, local);
            const WebAnimPhase prev = static_cast<WebAnimPhase>(layer.lastPhase);
            const double active = rec->activeDuration();
            const double intervalStart =
                std::max(std::min(-rec->delay, active), 0.0) / 1000.0;
            const double intervalEnd =
                std::max(std::min(rec->endTimeMs() - rec->delay, active), 0.0) / 1000.0;
            auto queue = [&](const char* type, double elapsed) {
                pendingEvents_.push_back({elem, type, layer.name, elapsed});
            };
            const bool wasBeforeOrIdle = prev == WebAnimPhase::Idle || prev == WebAnimPhase::Before;
            switch (ph) {
                case WebAnimPhase::Active:
                    if (wasBeforeOrIdle) queue("animationstart", intervalStart);
                    else if (prev == WebAnimPhase::After) queue("animationstart", intervalEnd);
                    else if (iteration != layer.lastIteration)
                        queue("animationiteration", iteration * rec->duration / 1000.0);
                    break;
                case WebAnimPhase::After:
                    if (wasBeforeOrIdle) {
                        queue("animationstart", intervalStart);
                        queue("animationend", intervalEnd);
                    } else if (prev == WebAnimPhase::Active) {
                        queue("animationend", intervalEnd);
                    }
                    break;
                case WebAnimPhase::Before:
                    if (prev == WebAnimPhase::Active) {
                        queue("animationend", intervalStart);
                    } else if (prev == WebAnimPhase::After) {
                        queue("animationstart", intervalEnd);
                        queue("animationend", intervalStart);
                    }
                    break;
                case WebAnimPhase::Idle:
                    // Cancelled from script (cancel()): the time it had run
                    // when last seen.
                    if (prev == WebAnimPhase::Before || prev == WebAnimPhase::Active)
                        queue("animationcancel", layer.lastElapsed);
                    break;
            }
            layer.lastPhase = static_cast<int>(ph);
            if (ph != WebAnimPhase::Idle) layer.lastElapsed = activeElapsed(*rec, local) / 1000.0;
            if (ph == WebAnimPhase::Active) layer.lastIteration = iteration;
        }
    }
    return pendingEvents_.size() != before;
}

void AnimationManager::forgetElement(dom::Element* elem) {
    if (!elem) return;
    auto it = elements_.find(elem);
    if (it != elements_.end()) {
        for (CssAnimationLayer& layer : it->second.layers)
            if (layer.id && web_) web_->cancelFromMarkup(layer.id);
        elements_.erase(it);
    }
    pendingEvents_.erase(
        std::remove_if(pendingEvents_.begin(), pendingEvents_.end(),
                       [elem](const PendingCSSEvent& ev) { return ev.element == elem; }),
        pendingEvents_.end());
}

// Safe to dereference the keys: the callers run while the document's node
// storage is still intact.
void AnimationManager::forgetDocument(const dom::Document* doc) {
    if (!doc) return;
    auto belongs = [doc](const dom::Element* e) { return e && e->document() == doc; };
    for (auto it = elements_.begin(); it != elements_.end();) {
        if (!belongs(it->first)) { ++it; continue; }
        for (CssAnimationLayer& layer : it->second.layers)
            if (layer.id && web_) web_->cancelFromMarkup(layer.id);
        it = elements_.erase(it);
    }
    pendingEvents_.erase(
        std::remove_if(pendingEvents_.begin(), pendingEvents_.end(),
                       [&](const PendingCSSEvent& ev) { return belongs(ev.element); }),
        pendingEvents_.end());
}

} // namespace bro::engine
