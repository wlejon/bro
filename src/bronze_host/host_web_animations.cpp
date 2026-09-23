#include "bronze_host/host_web_animations.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "dom/document.h"
#include "dom/element.h"
#include "engine/css_transitions.h"
#include "engine/engine.h"
#include "engine/web_animations.h"
#include "util/log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

static bool isJsArray(Value vIn) {
    if (!ev::isObject(vIn)) return false;
    const Rooted v(vIn);  // looking up Array.isArray allocates
    Value isArrFn = ev::getProperty(ev::globalValue("Array").value, "isArray");
    if (!ev::isFunction(isArrFn)) return false;
    const Value arg = v.get();
    Value res = ev::call(isArrFn, ev::undefined(), std::span<const Value>(&arg, 1)).value;
    return ev::toBool(res);
}

HostClass g_animationClass;

struct AnimationState {
    uint64_t id = 0;
    std::string name;
    ev::Persistent self;
    ev::Persistent onfinish;
    ev::Persistent oncancel;
    ev::Persistent finishedPromise;
    // `ready`: minted on first read and already resolved — this model has no
    // pending play/pause tasks (`pending` is always false), so an animation is
    // ready the moment it exists.
    ev::Persistent readyPromise;
    // `effect`: the KeyframeEffect, minted on first read and kept so
    // `anim.effect === anim.effect`.
    ev::Persistent effect;
    bool promiseSettled = false;
    bool finishDelivered = false;
};

std::unordered_map<uint64_t, AnimationState*>& liveStates() {
    static auto* m = new std::unordered_map<uint64_t, AnimationState*>();
    return *m;
}

std::unordered_map<uint64_t, ev::Persistent>& strongPins() {
    static auto* m = new std::unordered_map<uint64_t, ev::Persistent>();
    return *m;
}

void animationFinalizer(void* ptr) {
    auto* st = static_cast<AnimationState*>(ptr);
    if (!st) return;
    liveStates().erase(st->id);
    strongPins().erase(st->id);
    engine::Engine* eng = hostEngine();
    if (eng && engine::WebAnimationManager::isLive(&eng->webAnimationManager())) {
        eng->webAnimationManager().releaseFromWrapper(st->id);
    }
    delete st;
}

void resolveFinishedPromise(AnimationState* st, Value animObj) {
    if (ev::isUndefined(st->finishedPromise.get()) || st->promiseSettled) return;
    st->promiseSettled = true;
    ev::resolvePromise(st->finishedPromise.get(), animObj);
}

void rejectFinishedPromise(AnimationState* st) {
    if (ev::isUndefined(st->finishedPromise.get()) || st->promiseSettled) return;
    st->promiseSettled = true;
    Value err = hostMakeDomError("AbortError", "The animation was aborted");
    ev::rejectPromise(st->finishedPromise.get(), err);
}

void fireHandler(Value handler, Value animObj, const char* type, Value currentTime) {
    if (!ev::isFunction(handler)) return;
    // Rooted before the event object is built: its allocations would leave
    // the raw parameters pointing at the old semispace.
    ev::Persistent handlerP(handler);
    ev::Persistent animP(animObj);
    ev::Persistent timeP(currentTime);
    ObjectBuilder eb;
    eb.set("type", ev::fromUtf8(type));
    eb.set("currentTime", timeP.get());
    eb.set("target", animP.get());
    Value args[1] = { eb.get() };
    ev::call(handlerP.get(), animP.get(), std::span<const Value>(args, 1));
}

void settleFinish(AnimationState* st, Value animObjIn) {
    if (st->finishDelivered) return;
    st->finishDelivered = true;
    // Resolving the promise allocates; the handler call below needs the
    // object's post-resolve address.
    ev::Persistent animP(animObjIn);
    resolveFinishedPromise(st, animP.get());
    engine::Engine* eng = hostEngine();
    Value ct = ev::null();
    if (eng) {
        engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
        if (rec) {
            double now = eng->timeNowMs();
            auto cur = rec->currentTimeMs(now);
            if (cur) ct = ev::fromDouble(*cur);
        }
    }
    fireHandler(st->onfinish.get(), animP.get(), "finish", ct);
    strongPins().erase(st->id);
}

std::string camelToKebab(const std::string& str) {
    std::string out;
    for (char c : str) {
        if (std::isupper(static_cast<unsigned char>(c))) {
            out.push_back('-');
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string keyToCssProp(const std::string& key) {
    if (key == "cssFloat") return "float";
    if (key == "cssOffset") return "offset";
    return camelToKebab(key);
}

bool computeOffsets(std::vector<double>& spec, std::vector<engine::WebAnimKeyframe>& frames) {
    size_t n = frames.size();
    if (n == 0) return true;
    for (double o : spec) {
        if (o >= 0 && (o < 0 || o > 1 || std::isnan(o))) return false;
    }
    if (n == 1) {
        if (spec[0] < 0) spec[0] = 1.0;
    } else {
        if (spec[0] < 0) spec[0] = 0.0;
        if (spec[n - 1] < 0) spec[n - 1] = 1.0;
        size_t i = 0;
        while (i < n) {
            if (spec[i] >= 0) { ++i; continue; }
            size_t runStart = i;
            while (i < n && spec[i] < 0) ++i;
            double lo = spec[runStart - 1];
            double hi = spec[i];
            size_t count = i - runStart + 1;
            for (size_t k = runStart; k < i; ++k)
                spec[k] = lo + (hi - lo) * static_cast<double>(k - runStart + 1) / count;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && spec[i] < spec[i - 1]) return false;
        frames[i].offset = static_cast<float>(spec[i]);
    }
    return true;
}

// Object.keys(obj) as strings. Everything is rooted across the call and the
// element reads, which allocate.
std::vector<std::string> ownKeys(Value objIn) {
    std::vector<std::string> keys;
    ev::Persistent obj(objIn);
    ev::Persistent objCtor(ev::globalValue("Object").value);
    ev::Persistent keysFn(ev::getProperty(objCtor.get(), "keys"));
    Value arg = obj.get();
    ev::CallResult r = ev::call(keysFn.get(), objCtor.get(), std::span<const Value>(&arg, 1));
    if (r.thrown) return keys;
    ev::Persistent arr(r.value);
    Value lenVal = ev::getProperty(arr.get(), "length");
    uint32_t len = ev::isNumber(lenVal) ? static_cast<uint32_t>(ev::toDouble(lenVal)) : 0;
    keys.reserve(len);
    for (uint32_t k = 0; k < len; ++k) keys.push_back(ev::toUtf8(ev::getElement(arr.get(), k)));
    return keys;
}

// A keyframe value as its string: strings as they are, numbers through
// ToString, anything else empty. Converted at once, never held raw.
std::string keyframeValueString(Value v) {
    return (ev::isString(v) || ev::isNumber(v)) ? ev::toUtf8(v) : std::string();
}

bool parseKeyframeArray(Value arrIn, std::vector<engine::WebAnimKeyframe>& frames) {
    ev::Persistent arr(arrIn);
    Value lenVal = ev::getProperty(arr.get(), "length");
    int64_t len = ev::isNumber(lenVal) ? static_cast<int64_t>(ev::toDouble(lenVal)) : 0;
    std::vector<double> offsets;

    for (int64_t i = 0; i < len; ++i) {
        ev::Persistent item(ev::getElement(arr.get(), static_cast<uint32_t>(i)));
        if (!ev::isObject(item.get())) return false;
        engine::WebAnimKeyframe kf;
        double off = -1.0;

        for (const std::string& pname : ownKeys(item.get())) {
            Value pv = ev::getProperty(item.get(), pname.c_str());
            if (pname == "offset") {
                if (ev::isNumber(pv)) off = ev::toDouble(pv);
            } else if (pname == "easing") {
                std::string es = ev::toUtf8(pv);
                if (!es.empty()) {
                    kf.easing = engine::parseTimingFunction(es);
                    kf.hasEasing = true;
                }
            } else {
                kf.props.emplace_back(keyToCssProp(pname), keyframeValueString(pv));
            }
        }
        offsets.push_back(off);
        frames.push_back(std::move(kf));
    }
    return computeOffsets(offsets, frames);
}

bool parseKeyframeObject(Value obj, std::vector<engine::WebAnimKeyframe>& frames) {
    struct PropList {
        std::string prop;
        std::vector<std::string> values;
    };
    std::vector<PropList> lists;
    std::vector<std::string> easings;

    ev::Persistent objP(obj);
    for (const std::string& pname : ownKeys(objP.get())) {
        // Rooted: each element read and number conversion below allocates.
        ev::Persistent pv(ev::getProperty(objP.get(), pname.c_str()));

        auto collect = [&](std::vector<std::string>& out) {
            if (isJsArray(pv.get())) {
                Value lv = ev::getProperty(pv.get(), "length");
                int64_t arrLen = ev::isNumber(lv) ? static_cast<int64_t>(ev::toDouble(lv)) : 0;
                for (int64_t i = 0; i < arrLen; ++i) {
                    out.push_back(keyframeValueString(ev::getElement(pv.get(), static_cast<uint32_t>(i))));
                }
            } else {
                out.push_back(keyframeValueString(pv.get()));
            }
        };

        if (pname == "offset" || pname == "composite") {
            // Ignore explicit offset list in object form (distributes evenly)
        } else if (pname == "easing") {
            collect(easings);
        } else {
            PropList pl;
            pl.prop = keyToCssProp(pname);
            collect(pl.values);
            if (!pl.values.empty()) lists.push_back(std::move(pl));
        }
    }

    std::vector<float> offsets;
    auto offsetFor = [](size_t k, size_t m) -> float {
        return m <= 1 ? 1.0f : static_cast<float>(k) / static_cast<float>(m - 1);
    };
    for (const auto& pl : lists) {
        for (size_t k = 0; k < pl.values.size(); ++k) {
            float o = offsetFor(k, pl.values.size());
            if (std::find(offsets.begin(), offsets.end(), o) == offsets.end())
                offsets.push_back(o);
        }
    }
    std::sort(offsets.begin(), offsets.end());

    for (size_t f = 0; f < offsets.size(); ++f) {
        engine::WebAnimKeyframe kf;
        kf.offset = offsets[f];
        for (const auto& pl : lists) {
            for (size_t k = 0; k < pl.values.size(); ++k) {
                if (offsetFor(k, pl.values.size()) == offsets[f]) {
                    kf.props.emplace_back(pl.prop, pl.values[k]);
                    break;
                }
            }
        }
        if (!easings.empty()) {
            const std::string& es = easings[f % easings.size()];
            if (!es.empty()) {
                kf.easing = engine::parseTimingFunction(es);
                kf.hasEasing = true;
            }
        }
        frames.push_back(std::move(kf));
    }
    return true;
}

bool parseOptions(Value optIn, engine::WebAnimation& a, std::string& name) {
    if (ev::isUndefined(optIn) || ev::isNull(optIn)) return true;
    if (ev::isNumber(optIn)) {
        double d = ev::toDouble(optIn);
        if (!(d >= 0)) return false;
        a.duration = d;
        return true;
    }
    if (!ev::isObject(optIn)) return false;
    const Rooted opt(optIn);  // each property read allocates

    Value durVal = ev::getProperty(opt, "duration");
    if (ev::isNumber(durVal)) {
        double d = ev::toDouble(durVal);
        if (!(d >= 0)) return false;
        a.duration = d;
    }
    Value delVal = ev::getProperty(opt, "delay");
    if (ev::isNumber(delVal)) a.delay = ev::toDouble(delVal);

    Value endDelVal = ev::getProperty(opt, "endDelay");
    if (ev::isNumber(endDelVal)) a.endDelay = ev::toDouble(endDelVal);

    Value iterVal = ev::getProperty(opt, "iterations");
    if (ev::isNumber(iterVal)) {
        double d = ev::toDouble(iterVal);
        if (!std::isnan(d) && d >= 0) a.iterations = d;
    }

    Value dirVal = ev::getProperty(opt, "direction");
    if (ev::isString(dirVal)) {
        std::string dir = ev::toUtf8(dirVal);
        if (dir == "reverse") a.direction = engine::WebAnimDirection::Reverse;
        else if (dir == "alternate") a.direction = engine::WebAnimDirection::Alternate;
        else if (dir == "alternate-reverse") a.direction = engine::WebAnimDirection::AlternateReverse;
    }

    Value fillVal = ev::getProperty(opt, "fill");
    if (ev::isString(fillVal)) {
        std::string fill = ev::toUtf8(fillVal);
        if (fill == "forwards") a.fill = engine::WebAnimFill::Forwards;
        else if (fill == "backwards") a.fill = engine::WebAnimFill::Backwards;
        else if (fill == "both") a.fill = engine::WebAnimFill::Both;
    }

    Value easeVal = ev::getProperty(opt, "easing");
    if (ev::isString(easeVal)) {
        std::string easing = ev::toUtf8(easeVal);
        if (!easing.empty()) a.easing = engine::parseTimingFunction(easing);
    }

    Value idVal = ev::getProperty(opt, "id");
    if (ev::isString(idVal)) name = ev::toUtf8(idVal);

    return true;
}

} // namespace

Value wrapAnimation(uint64_t id, const std::string& name = "") {
    auto it = liveStates().find(id);
    if (it != liveStates().end()) {
        return it->second->self.get();
    }
    auto* st = new AnimationState();
    st->id = id;
    st->name = name;
    ObjectBuilder b(g_animationClass.make(st, animationFinalizer));
    Value obj = b.get();
    st->self = ev::Persistent(obj);
    liveStates()[id] = st;
    engine::Engine* eng = hostEngine();
    if (eng) {
        engine::WebAnimation* rec = eng->webAnimationManager().find(id);
        if (rec && (rec->state == engine::WebAnimState::Running ||
                    rec->state == engine::WebAnimState::Paused)) {
            strongPins()[id] = ev::Persistent(obj);
        }
    }
    return obj;
}

namespace {

// ---------------------------------------------------------------------------
// document.timeline — the DocumentTimeline every element.animate() runs on.
// Its currentTime is the engine's scaled clock (bro.time), the same clock a
// record's startTime is expressed in, so
//     anim.currentTime == (document.timeline.currentTime - anim.startTime) * rate
// holds for a running animation.
// ---------------------------------------------------------------------------
HostClass g_animationTimelineClass;
HostClass g_documentTimelineClass;
ev::Persistent* g_documentTimeline = nullptr;

Value documentTimelineValue() {
    if (!g_documentTimeline) {
        g_documentTimeline = new ev::Persistent(g_documentTimelineClass.make(nullptr, [](void*) {}));
    }
    return g_documentTimeline->get();
}

// ---------------------------------------------------------------------------
// KeyframeEffect — what `anim.effect` answers: the target, the timing as
// given (getTiming), the timing as it stands now (getComputedTiming), and the
// keyframes (getKeyframes). A view over the engine record, not a copy, so it
// always reads the animation's current state.
// ---------------------------------------------------------------------------
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

std::string easingName(const bromath::CubicEase& e) {
    auto is = [&e](float a, float b, float c, float d) {
        return e.p1x == a && e.p1y == b && e.p2x == c && e.p2y == d;
    };
    if (is(0.0f, 0.0f, 1.0f, 1.0f)) return "linear";
    if (is(0.25f, 0.1f, 0.25f, 1.0f)) return "ease";
    if (is(0.42f, 0.0f, 1.0f, 1.0f)) return "ease-in";
    if (is(0.0f, 0.0f, 0.58f, 1.0f)) return "ease-out";
    if (is(0.42f, 0.0f, 0.58f, 1.0f)) return "ease-in-out";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "cubic-bezier(%g, %g, %g, %g)", e.p1x, e.p1y, e.p2x, e.p2y);
    return buf;
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
    o.set("easing", ev::fromUtf8(easingName(rec.easing)));
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
    if (localTime < beforeEnd) {
        if (!rec.fillsBackwards()) return false;
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
    progress = bromath::ccubicEase(rec.easing, static_cast<float>(directed));
    if (directed <= 0.0) progress = 0.0;
    if (directed >= 1.0) progress = 1.0;
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
            o.set("easing", ev::fromUtf8(kf.hasEasing ? easingName(kf.easing) : "linear"));
            o.set("composite", ev::fromUtf8("auto"));
            for (const auto& [prop, value] : kf.props) {
                o.set(kebabToCamel(prop).c_str(), ev::fromUtf8(value));
            }
            return o.get();
        });
    });
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

static ev::Persistent s_skeletalAnimationCtor;

Value animationConstructor(Value self, std::span<const Value> a) {
    if (ev::isFunction(s_skeletalAnimationCtor.get()) && !a.empty() && ev::isObject(a[0])) {
        Value chan = ev::getProperty(a[0], "channels");
        Value dur = ev::getProperty(a[0], "duration");
        if (!ev::isUndefined(chan) || !ev::isUndefined(dur)) {
            ev::CallResult r = ev::construct(s_skeletalAnimationCtor.get(), a);
            return r.thrown ? ev::undefined() : r.value;
        }
    }
    engine::Engine* eng = hostEngine();
    uint64_t id = 0;
    if (eng) {
        engine::WebAnimation& rec = eng->webAnimationManager().create(nullptr, eng->timeNowMs());
        id = rec.id;
    }
    return wrapAnimation(id);
}

} // namespace

void installWebAnimationGlobals() {
    ev::GlobalValue existing = ev::globalValue("Animation");
    if (existing.found && ev::isFunction(existing.value)) {
        s_skeletalAnimationCtor.set(existing.value);
    } else {
        s_skeletalAnimationCtor.set(ev::undefined());
    }

    installTimelineAndEffectClasses();

    g_animationClass.install("Animation", 0, animationConstructor, [](ObjectBuilder& b) {
        b.def("play", 0, [](Value self_, std::span<const Value>) {
            auto* st = static_cast<AnimationState*>(ev::handleData(self_));
            if (!st) return ev::undefined();
            engine::Engine* eng = hostEngine();
            if (!eng) return ev::undefined();
            engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
            if (!rec) return ev::undefined();
            eng->webAnimationManager().play(*rec, eng->timeNowMs());
            if (st->promiseSettled) {
                st->finishedPromise.set(ev::undefined());
                st->promiseSettled = false;
            }
            st->finishDelivered = false;
            strongPins()[st->id] = ev::Persistent(self_);
            if (auto* el = eng->webAnimationManager().resolveElement(*rec)) el->markDirty();
            return ev::undefined();
        });

        b.def("pause", 0, [](Value self_, std::span<const Value>) {
            auto* st = static_cast<AnimationState*>(ev::handleData(self_));
            if (!st) return ev::undefined();
            engine::Engine* eng = hostEngine();
            if (!eng) return ev::undefined();
            engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
            if (!rec) return ev::undefined();
            eng->webAnimationManager().pause(*rec, eng->timeNowMs());
            if (auto* el = eng->webAnimationManager().resolveElement(*rec)) el->markDirty();
            return ev::undefined();
        });

        b.def("finish", 0, [](Value self_, std::span<const Value>) {
            auto* st = static_cast<AnimationState*>(ev::handleData(self_));
            if (!st) return ev::undefined();
            engine::Engine* eng = hostEngine();
            if (!eng) return ev::undefined();
            engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
            if (!rec) return ev::undefined();
            if (rec->playbackRate == 0 || (rec->playbackRate > 0 && !std::isfinite(rec->endTimeMs()))) {
                return ev::throwValue(hostMakeDomError("InvalidStateError", "Animation cannot be finished if playbackRate is 0 or end time is infinite"));
            }
            eng->webAnimationManager().finishOp(*rec);
            if (auto* el = eng->webAnimationManager().resolveElement(*rec)) el->markDirty();
            settleFinish(st, self_);
            return ev::undefined();
        });

        b.def("cancel", 0, [](Value self_, std::span<const Value>) {
            auto* st = static_cast<AnimationState*>(ev::handleData(self_));
            if (!st) return ev::undefined();
            engine::Engine* eng = hostEngine();
            if (!eng) return ev::undefined();
            engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
            if (!rec || rec->state == engine::WebAnimState::Idle) return ev::undefined();
            eng->webAnimationManager().cancelOp(*rec);
            if (auto* el = eng->webAnimationManager().resolveElement(*rec)) el->markDirty();
            // `self_` is a plain copy; rejecting the promise allocates.
            ev::Persistent selfP(self_);
            rejectFinishedPromise(st);
            st->finishedPromise.set(ev::undefined());
            st->promiseSettled = false;
            st->finishDelivered = false;
            fireHandler(st->oncancel.get(), selfP.get(), "cancel", ev::null());
            strongPins().erase(st->id);
            return ev::undefined();
        });

        b.def("reverse", 0, [](Value self_, std::span<const Value>) {
            auto* st = static_cast<AnimationState*>(ev::handleData(self_));
            if (!st) return ev::undefined();
            engine::Engine* eng = hostEngine();
            if (!eng) return ev::undefined();
            engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
            if (!rec) return ev::undefined();
            eng->webAnimationManager().reverse(*rec, eng->timeNowMs());
            if (auto* el = eng->webAnimationManager().resolveElement(*rec)) el->markDirty();
            return ev::undefined();
        });

        b.accessor("playState", [](Value self_, std::span<const Value>) {
            auto* st = static_cast<AnimationState*>(ev::handleData(self_));
            if (!st) return ev::fromUtf8("idle");
            engine::Engine* eng = hostEngine();
            if (!eng) return ev::fromUtf8("idle");
            engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
            if (!rec) return ev::fromUtf8("idle");
            return ev::fromUtf8(eng->webAnimationManager().playState(*rec, eng->timeNowMs()));
        }, nullptr);

        b.accessor("playbackRate",
            [](Value self_, std::span<const Value>) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                if (!st) return ev::fromDouble(1.0);
                engine::Engine* eng = hostEngine();
                if (!eng) return ev::fromDouble(1.0);
                engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
                return ev::fromDouble(rec ? rec->playbackRate : 1.0);
            },
            [](Value self_, std::span<const Value> a) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                if (!st) return ev::undefined();
                engine::Engine* eng = hostEngine();
                if (!eng) return ev::undefined();
                engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
                if (rec) {
                    double r = a.empty() ? 1.0 : ev::toDouble(a[0]);
                    eng->webAnimationManager().setRate(*rec, r, eng->timeNowMs());
                    if (auto* el = eng->webAnimationManager().resolveElement(*rec)) el->markDirty();
                }
                return ev::undefined();
            });

        b.accessor("currentTime",
            [](Value self_, std::span<const Value>) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                if (!st) return ev::null();
                engine::Engine* eng = hostEngine();
                if (!eng) return ev::null();
                engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
                if (!rec) return ev::null();
                auto ct = rec->currentTimeMs(eng->timeNowMs());
                return ct ? ev::fromDouble(*ct) : ev::null();
            },
            [](Value self_, std::span<const Value> a) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                if (!st) return ev::undefined();
                engine::Engine* eng = hostEngine();
                if (!eng) return ev::undefined();
                engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
                if (rec && !a.empty()) {
                    double ct = ev::toDouble(a[0]);
                    eng->webAnimationManager().setCurrentTime(*rec, ct, eng->timeNowMs());
                    if (auto* el = eng->webAnimationManager().resolveElement(*rec)) el->markDirty();
                }
                return ev::undefined();
            });

        // startTime: the timeline time at which currentTime was 0 — null while
        // a hold time (pause, finish, idle) resolves currentTime instead.
        b.accessor("startTime",
            [](Value self_, std::span<const Value>) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                engine::Engine* eng = hostEngine();
                if (!st || !eng) return ev::null();
                engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
                if (!rec || rec->hasHoldTime || !rec->hasStartTime ||
                    rec->state == engine::WebAnimState::Idle) {
                    return ev::null();
                }
                return ev::fromDouble(rec->startTime);
            },
            [](Value self_, std::span<const Value> a) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                engine::Engine* eng = hostEngine();
                if (!st || !eng || a.empty() || !ev::isNumber(a[0])) return ev::undefined();
                engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
                if (!rec) return ev::undefined();
                eng->webAnimationManager().setStartTime(*rec, ev::toDouble(a[0]), eng->timeNowMs());
                if (auto* el = eng->webAnimationManager().resolveElement(*rec)) el->markDirty();
                return ev::undefined();
            });

        b.accessor("timeline", [](Value, std::span<const Value>) {
            return documentTimelineValue();
        }, nullptr);

        b.accessor("effect", [](Value self_, std::span<const Value>) {
            auto* st = static_cast<AnimationState*>(ev::handleData(self_));
            return st ? effectValueFor(st) : ev::null();
        }, nullptr);

        b.accessor("ready", [](Value self_, std::span<const Value>) {
            auto* st = static_cast<AnimationState*>(ev::handleData(self_));
            if (!st) return ev::null();
            if (ev::isUndefined(st->readyPromise.get())) {
                ev::Persistent self(self_);
                st->readyPromise.set(ev::createPromise());
                ev::resolvePromise(st->readyPromise.get(), self.get());
            }
            return st->readyPromise.get();
        }, nullptr);

        b.accessor("pending", [](Value, std::span<const Value>) {
            return ev::fromBool(false);
        }, nullptr);

        b.accessor("id",
            [](Value self_, std::span<const Value>) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                return st ? ev::fromUtf8(st->name) : ev::fromUtf8("");
            },
            [](Value self_, std::span<const Value> a) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                if (st && !a.empty()) st->name = ev::toUtf8(a[0]);
                return ev::undefined();
            });

        b.accessor("finished", [](Value self_, std::span<const Value>) {
            auto* st = static_cast<AnimationState*>(ev::handleData(self_));
            if (!st) return ev::null();
            if (ev::isUndefined(st->finishedPromise.get())) {
                st->finishedPromise.set(ev::createPromise());
                st->promiseSettled = false;
                engine::Engine* eng = hostEngine();
                if (eng) {
                    engine::WebAnimation* rec = eng->webAnimationManager().find(st->id);
                    if (rec) {
                        std::string ps = eng->webAnimationManager().playState(*rec, eng->timeNowMs());
                        if (ps == "finished") {
                            resolveFinishedPromise(st, self_);
                        }
                    }
                }
            }
            return st->finishedPromise.get();
        }, nullptr);

        b.accessor("onfinish",
            [](Value self_, std::span<const Value>) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                return (st && ev::isFunction(st->onfinish.get())) ? st->onfinish.get() : ev::null();
            },
            [](Value self_, std::span<const Value> a) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                if (st) {
                    if (!a.empty() && ev::isFunction(a[0])) st->onfinish.set(a[0]);
                    else st->onfinish.set(ev::undefined());
                }
                return ev::undefined();
            });

        b.accessor("oncancel",
            [](Value self_, std::span<const Value>) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                return (st && ev::isFunction(st->oncancel.get())) ? st->oncancel.get() : ev::null();
            },
            [](Value self_, std::span<const Value> a) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                if (st) {
                    if (!a.empty() && ev::isFunction(a[0])) st->oncancel.set(a[0]);
                    else st->oncancel.set(ev::undefined());
                }
                return ev::undefined();
            });
    });

    if (ev::isFunction(s_skeletalAnimationCtor.get())) {
        Value skelProto = ev::getProperty(s_skeletalAnimationCtor.get(), "prototype");
        if (ev::isObject(skelProto)) {
            ev::setPrototype(skelProto, g_animationClass.prototype());
        }
        Value retarget = ev::getProperty(s_skeletalAnimationCtor.get(), "retarget");
        if (!ev::isUndefined(retarget)) {
            ev::setProperty(g_animationClass.constructor(), "retarget", retarget);
        }
    }

    ev::registerGlobal("Animation", g_animationClass.constructor());
    ev::registerGlobal("WebAnimation", g_animationClass.constructor());
    // Each global object is rooted across its two defines (setProperty allocates).
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        const Rooted global(gt.value);
        ev::setProperty(global, "Animation", g_animationClass.constructor());
        ev::setProperty(global, "WebAnimation", g_animationClass.constructor());
    }
    ev::GlobalValue win = ev::globalValue("window");
    if (win.found && ev::isObject(win.value)) {
        const Rooted window(win.value);
        ev::setProperty(window, "Animation", g_animationClass.constructor());
        ev::setProperty(window, "WebAnimation", g_animationClass.constructor());
    }
}

void decorateElementWebAnimations(ObjectBuilder& b) {
    b.def("animate", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* nst = hostNodeStateOfValue(self_);
        if (!nst || !nst->el) return ev::throwTypeError("Element.animate: invalid receiver");
        engine::Engine* eng = hostEngine();
        if (!eng) return ev::throwError("Element.animate: no engine");
        if (a.empty() || (!ev::isObject(a[0]) && !isJsArray(a[0]))) {
            return ev::throwTypeError("Element.animate: keyframes must be an object or array");
        }

        std::vector<engine::WebAnimKeyframe> frames;
        bool ok = isJsArray(a[0]) ? parseKeyframeArray(a[0], frames)
                                  : parseKeyframeObject(a[0], frames);
        if (!ok) return ev::throwTypeError("Element.animate: invalid keyframes");

        engine::WebAnimation scratch;
        std::string name;
        if (a.size() >= 2) {
            if (!parseOptions(a[1], scratch, name)) {
                return ev::throwTypeError("Element.animate: invalid options");
            }
        }

        double now = eng->timeNowMs();
        engine::WebAnimation& rec = eng->webAnimationManager().create(nst->el, now);
        rec.keyframes = std::move(frames);
        rec.duration = scratch.duration;
        rec.delay = scratch.delay;
        rec.endDelay = scratch.endDelay;
        rec.iterations = scratch.iterations;
        rec.direction = scratch.direction;
        rec.fill = scratch.fill;
        rec.easing = scratch.easing;

        nst->el->markDirty();
        return wrapAnimation(rec.id, name);
    });

    b.def("getAnimations", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* nst = hostNodeStateOfValue(self_);
        if (!nst || !nst->el) return makeEmptyArray();
        engine::Engine* eng = hostEngine();
        if (!eng) return makeEmptyArray();
        auto ids = eng->webAnimationManager().animationsFor(nst->el, eng->timeNowMs());
        return hostArrayOf(ids.size(), [&ids](size_t i) {
            return wrapAnimation(ids[i]);
        });
    });
}

void decorateDocumentWebAnimations(ObjectBuilder& b) {
    b.accessor("timeline", [](Value, std::span<const Value>) {
        return documentTimelineValue();
    }, nullptr);

    b.def("getAnimations", 0, [](Value, std::span<const Value>) {
        engine::Engine* eng = hostEngine();
        if (!eng) return makeEmptyArray();
        auto ids = eng->webAnimationManager().allAnimations(eng->timeNowMs());
        return hostArrayOf(ids.size(), [&ids](size_t i) {
            return wrapAnimation(ids[i]);
        });
    });
}

void deliverWebAnimationFinishEvents() {
    engine::Engine* eng = hostEngine();
    if (!eng) return;
    std::vector<uint64_t> ids = eng->webAnimationManager().takeFinishedEvents();
    for (uint64_t id : ids) {
        auto it = liveStates().find(id);
        if (it != liveStates().end()) {
            AnimationState* st = it->second;
            settleFinish(st, st->self.get());
        }
    }
}

} // namespace bro::bronze_host
