// document.timeline and the KeyframeEffect behind `anim.effect` — the timing
// as given (getTiming), as it stands now (getComputedTiming), the keyframes
// (getKeyframes), and re-timing (updateTiming). The effect is a view over the
// engine record, not a copy, so it always reads the animation's current state
// — including a CSS animation's, whose record the markup keeps up to date.

#include "bronze_host/host_web_animations_internal.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "dom/element.h"
#include "engine/css_easing.h"
#include "engine/engine.h"
#include "engine/web_animations.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// document.timeline — the DocumentTimeline every animation runs on. Its
// currentTime is the engine's scaled clock (bro.time), the same clock a
// record's startTime is expressed in, so
//     anim.currentTime == (document.timeline.currentTime - anim.startTime) * rate
// holds for a running animation.
// ---------------------------------------------------------------------------
HostClass g_animationTimelineClass;
HostClass g_documentTimelineClass;
ev::Persistent* g_documentTimeline = nullptr;

constexpr uint32_t kEffectTag = 0x4B464546u;  // 'KFEF'
struct EffectState {
    uint32_t tag = kEffectTag;
    uint64_t id = 0;
};
HostClass g_animationEffectClass;
HostClass g_keyframeEffectClass;

engine::WebAnimation* effectRecord(Value self) {
    if (!ev::isObject(self)) return nullptr;
    auto* es = static_cast<EffectState*>(ev::handleData(self));
    engine::Engine* eng = hostEngine();
    if (!es || es->tag != kEffectTag || !eng) return nullptr;
    return eng->webAnimationManager().find(es->id);
}

const char* fillName(engine::WebAnimFill f) {
    switch (f) {
        case engine::WebAnimFill::Forwards: return "forwards";
        case engine::WebAnimFill::Backwards: return "backwards";
        case engine::WebAnimFill::Both: return "both";
        default: return "none";
    }
}

const char* directionName(engine::WebAnimDirection d) {
    switch (d) {
        case engine::WebAnimDirection::Reverse: return "reverse";
        case engine::WebAnimDirection::Alternate: return "alternate";
        case engine::WebAnimDirection::AlternateReverse: return "alternate-reverse";
        default: return "normal";
    }
}

std::string kebabToCamel(const std::string& s) {
    std::string out;
    bool up = false;
    for (char c : s) {
        if (c == '-') { up = true; continue; }
        out.push_back(up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        up = false;
    }
    return out;
}

// EffectTiming members, shared by getTiming and getComputedTiming.
void setTimingMembers(ObjectBuilder& o, const engine::WebAnimation& rec) {
    o.set("delay", ev::fromDouble(rec.delay));
    o.set("endDelay", ev::fromDouble(rec.endDelay));
    o.set("fill", ev::fromUtf8(fillName(rec.fill)));
    o.set("iterationStart", ev::fromDouble(0));
    o.set("iterations", ev::fromDouble(rec.iterations));
    o.set("duration", ev::fromDouble(rec.duration));
    o.set("direction", ev::fromUtf8(directionName(rec.direction)));
    o.set("easing", ev::fromUtf8(rec.easing.toString()));
}

// Web Animations §4.8-4.10: phase, overall / simple iteration progress, the
// current iteration, direction, then the effect easing. False when the
// effect is not in effect (outside its active interval with no fill there).
bool computedProgress(const engine::WebAnimation& rec, double localTime,
                      double& progress, double& iteration) {
    const double active = rec.activeDuration();
    const double beforeEnd = std::max(std::min(rec.delay, rec.endTimeMs()), 0.0);
    const double activeEnd = std::max(std::min(rec.delay + active, rec.endTimeMs()), 0.0);
    double activeTime;
    bool after = false;
    bool before = false;
    if (localTime < beforeEnd) {
        if (!rec.fillsBackwards()) return false;
        before = true;
        activeTime = std::max(localTime - rec.delay, 0.0);
    } else if (localTime >= activeEnd && std::isfinite(active)) {
        if (!rec.fillsForwards()) return false;
        after = true;
        activeTime = std::max(std::min(localTime - rec.delay, active), 0.0);
    } else {
        activeTime = localTime - rec.delay;
    }

    double overall = rec.duration == 0 ? (after ? rec.iterations : 0.0)
                                       : activeTime / rec.duration;
    double simple = std::isfinite(overall) ? std::fmod(overall, 1.0) : std::fmod(rec.iterations, 1.0);
    if (simple == 0 && (after || localTime >= beforeEnd) && activeTime == active &&
        rec.iterations != 0) {
        simple = 1.0;
    }
    if (after && !std::isfinite(rec.iterations)) {
        iteration = INFINITY;
    } else if (simple == 1.0) {
        iteration = std::floor(overall) - 1.0;
        if (iteration < 0) iteration = 0;
    } else {
        iteration = std::floor(overall);
    }

    bool reversed = false;
    switch (rec.direction) {
        case engine::WebAnimDirection::Reverse: reversed = true; break;
        case engine::WebAnimDirection::Alternate:
            reversed = std::isfinite(iteration) && std::fmod(iteration, 2.0) == 1.0;
            break;
        case engine::WebAnimDirection::AlternateReverse:
            reversed = !(std::isfinite(iteration) && std::fmod(iteration, 2.0) == 1.0);
            break;
        default: break;
    }
    const double directed = reversed ? 1.0 - simple : simple;
    progress = rec.easing.apply(static_cast<float>(directed), before);
    if (rec.easing.kind == engine::TimingFunction::Kind::Cubic) {
        // A cubic curve starts at 0 and ends at 1 exactly; steps() and
        // linear() report what they compute at the ends.
        if (directed <= 0.0) progress = 0.0;
        if (directed >= 1.0) progress = 1.0;
    }
    return true;
}

void decorateTimelineProto(ObjectBuilder& b) {
    b.accessor("currentTime",
               [](Value, std::span<const Value>) -> Value {
                   engine::Engine* eng = hostEngine();
                   return eng ? ev::fromDouble(eng->timeNowMs()) : ev::null();
               },
               nullptr);
}

void decorateKeyframeEffectProto(ObjectBuilder& b) {
    b.accessor("target",
               [](Value self, std::span<const Value>) -> Value {
                   engine::WebAnimation* rec = effectRecord(self);
                   engine::Engine* eng = hostEngine();
                   if (!rec || !eng) return ev::null();
                   dom::Element* el = eng->webAnimationManager().resolveElement(*rec);
                   return el ? hostElementValue(el) : ev::null();
               },
               nullptr);
    b.accessor("pseudoElement", [](Value, std::span<const Value>) { return ev::null(); },
               nullptr);
    b.accessor("composite", [](Value, std::span<const Value>) { return ev::fromUtf8("replace"); },
               nullptr);

    b.def("getTiming", 0, [](Value self, std::span<const Value>) -> Value {
        engine::WebAnimation* rec = effectRecord(self);
        ObjectBuilder o;
        if (rec) setTimingMembers(o, *rec);
        return o.get();
    });

    b.def("getComputedTiming", 0, [](Value self, std::span<const Value>) -> Value {
        engine::WebAnimation* rec = effectRecord(self);
        engine::Engine* eng = hostEngine();
        ObjectBuilder o;
        if (!rec || !eng) return o.get();
        setTimingMembers(o, *rec);
        o.set("activeDuration", ev::fromDouble(rec->activeDuration()));
        o.set("endTime", ev::fromDouble(rec->endTimeMs()));
        std::optional<double> local = rec->state == engine::WebAnimState::Idle
                                          ? std::nullopt
                                          : rec->currentTimeMs(eng->timeNowMs());
        double progress = 0, iteration = 0;
        if (local && computedProgress(*rec, *local, progress, iteration)) {
            o.set("localTime", ev::fromDouble(*local));
            o.set("progress", ev::fromDouble(progress));
            o.set("currentIteration", ev::fromDouble(iteration));
        } else {
            o.set("localTime", local ? ev::fromDouble(*local) : ev::null());
            o.set("progress", ev::null());
            o.set("currentIteration", ev::null());
        }
        return o.get();
    });

    b.def("getKeyframes", 0, [](Value self, std::span<const Value>) -> Value {
        engine::WebAnimation* rec = effectRecord(self);
        if (!rec) return makeEmptyArray();
        // A copy: building the objects allocates, and nothing may hold a
        // pointer into the record across that.
        const std::vector<engine::WebAnimKeyframe> frames = rec->keyframes;
        return hostArrayOf(frames.size(), [&frames](size_t i) {
            const engine::WebAnimKeyframe& kf = frames[i];
            ObjectBuilder o;
            o.set("offset", ev::fromDouble(kf.offset));
            o.set("computedOffset", ev::fromDouble(kf.offset));
            o.set("easing", ev::fromUtf8(kf.hasEasing ? kf.easing.toString() : "linear"));
            o.set("composite", ev::fromUtf8("auto"));
            for (const auto& [prop, value] : kf.props) {
                o.set(kebabToCamel(prop).c_str(), ev::fromUtf8(value));
            }
            return o.get();
        });
    });

    // updateTiming(partial EffectTiming): re-time the running effect. On a
    // CSS animation the members set here stop following its animation-*
    // longhands (CSS Animations 2 §4.3).
    b.def("updateTiming", 1, [](Value self, std::span<const Value> a) -> Value {
        engine::WebAnimation* rec = effectRecord(self);
        engine::Engine* eng = hostEngine();
        if (!rec || !eng) return ev::undefined();
        Value arg = a.empty() ? ev::undefined() : a[0];
        if (ev::isUndefined(arg) || ev::isNull(arg)) return ev::undefined();
        if (!ev::isObject(arg)) return ev::throwTypeError("updateTiming: timing must be an object");
        const uint64_t id = rec->id;
        engine::WebAnimation scratch;
        scratch.duration = rec->duration;
        scratch.delay = rec->delay;
        scratch.endDelay = rec->endDelay;
        scratch.iterations = rec->iterations;
        scratch.direction = rec->direction;
        scratch.fill = rec->fill;
        scratch.easing = rec->easing;
        uint32_t mask = 0;
        std::string error;
        if (!parseEffectTiming(arg, scratch, mask, error, nullptr))
            return ev::throwTypeError(("updateTiming: " + error).c_str());
        // Reading the members ran script getters: find the record again.
        rec = eng->webAnimationManager().find(id);
        if (!rec) return ev::undefined();
        rec->duration = scratch.duration;
        rec->delay = scratch.delay;
        rec->endDelay = scratch.endDelay;
        rec->iterations = scratch.iterations;
        rec->direction = scratch.direction;
        rec->fill = scratch.fill;
        rec->easing = scratch.easing;
        if (rec->isCssAnimation) rec->cssTimingOverride |= mask;
        eng->webAnimationManager().timingChanged(*rec, eng->timeNowMs());
        if (auto* el = eng->webAnimationManager().resolveElement(*rec)) el->markDirty();
        return ev::undefined();
    });
}

}  // namespace

bool parseEffectTiming(Value objIn, engine::WebAnimation& a, uint32_t& setMask,
                       std::string& error, std::string* id) {
    const Rooted opt(objIn);  // each property read allocates

    Value durVal = ev::getProperty(opt, "duration");
    if (ev::isNumber(durVal)) {
        double d = ev::toDouble(durVal);
        if (!(d >= 0)) { error = "duration must be a non-negative number"; return false; }
        a.duration = d;
        setMask |= engine::kTimingDuration;
    } else if (ev::isString(durVal)) {
        if (ev::toUtf8(durVal) != "auto") { error = "duration must be a number or 'auto'"; return false; }
        a.duration = 0;
        setMask |= engine::kTimingDuration;
    }

    Value delVal = ev::getProperty(opt, "delay");
    if (ev::isNumber(delVal)) {
        double d = ev::toDouble(delVal);
        if (!std::isfinite(d)) { error = "delay must be finite"; return false; }
        a.delay = d;
        setMask |= engine::kTimingDelay;
    }

    Value endDelVal = ev::getProperty(opt, "endDelay");
    if (ev::isNumber(endDelVal)) {
        double d = ev::toDouble(endDelVal);
        if (!std::isfinite(d)) { error = "endDelay must be finite"; return false; }
        a.endDelay = d;
        setMask |= engine::kTimingEndDelay;
    }

    Value iterVal = ev::getProperty(opt, "iterations");
    if (ev::isNumber(iterVal)) {
        double d = ev::toDouble(iterVal);
        if (std::isnan(d) || d < 0) { error = "iterations must be a non-negative number"; return false; }
        a.iterations = d;
        setMask |= engine::kTimingIterations;
    }

    Value dirVal = ev::getProperty(opt, "direction");
    if (ev::isString(dirVal)) {
        std::string dir = ev::toUtf8(dirVal);
        if (dir == "normal") a.direction = engine::WebAnimDirection::Normal;
        else if (dir == "reverse") a.direction = engine::WebAnimDirection::Reverse;
        else if (dir == "alternate") a.direction = engine::WebAnimDirection::Alternate;
        else if (dir == "alternate-reverse") a.direction = engine::WebAnimDirection::AlternateReverse;
        else { error = "invalid direction '" + dir + "'"; return false; }
        setMask |= engine::kTimingDirection;
    }

    Value fillVal = ev::getProperty(opt, "fill");
    if (ev::isString(fillVal)) {
        std::string fill = ev::toUtf8(fillVal);
        if (fill == "none" || fill == "auto") a.fill = engine::WebAnimFill::None;
        else if (fill == "forwards") a.fill = engine::WebAnimFill::Forwards;
        else if (fill == "backwards") a.fill = engine::WebAnimFill::Backwards;
        else if (fill == "both") a.fill = engine::WebAnimFill::Both;
        else { error = "invalid fill '" + fill + "'"; return false; }
        setMask |= engine::kTimingFill;
    }

    Value easeVal = ev::getProperty(opt, "easing");
    if (ev::isString(easeVal)) {
        std::string easing = ev::toUtf8(easeVal);
        if (!engine::tryParseEasing(easing, a.easing)) {
            error = "invalid easing '" + easing + "'";
            return false;
        }
        setMask |= engine::kTimingEasing;
    }

    if (id) {
        Value idVal = ev::getProperty(opt, "id");
        if (ev::isString(idVal)) *id = ev::toUtf8(idVal);
    }
    return true;
}

Value documentTimelineValue() {
    if (!g_documentTimeline) {
        g_documentTimeline = new ev::Persistent(g_documentTimelineClass.make(nullptr, [](void*) {}));
    }
    return g_documentTimeline->get();
}

void installTimelineAndEffectClasses() {
    g_animationTimelineClass.install("AnimationTimeline", 0, nullptr, decorateTimelineProto);
    g_documentTimelineClass.install("DocumentTimeline", 0, nullptr, nullptr);
    g_documentTimelineClass.inherit(g_animationTimelineClass);
    g_animationEffectClass.install("AnimationEffect", 0, nullptr, nullptr);
    g_keyframeEffectClass.install("KeyframeEffect", 0, nullptr, decorateKeyframeEffectProto);
    g_keyframeEffectClass.inherit(g_animationEffectClass);
}

Value effectValueFor(AnimationState* st) {
    if (ev::isUndefined(st->effect.get())) {
        auto* es = new EffectState();
        es->id = st->id;
        st->effect.set(g_keyframeEffectClass.make(es, [](void* p) {
            delete static_cast<EffectState*>(p);
        }));
    }
    return st->effect.get();
}

} // namespace bro::bronze_host
