#include "bronze_host/host_web_animations.h"
#include "bronze_host/host_web_animations_internal.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "dom/document.h"
#include "dom/element.h"
#include "engine/css_easing.h"
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
// CSSAnimation: the object a CSS @keyframes animation is seen through — an
// Animation with an animationName (CSS Animations 2).
HostClass g_cssAnimationClass;

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

// cancel(): reject `finished` (and replace it with a fresh pending promise on
// the next read), fire oncancel, release the pin.
void settleCancel(AnimationState* st, Value animObjIn) {
    ev::Persistent selfP(animObjIn);
    rejectFinishedPromise(st);
    st->finishedPromise.set(ev::undefined());
    st->promiseSettled = false;
    st->finishDelivered = false;
    fireHandler(st->oncancel.get(), selfP.get(), "cancel", ev::null());
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
    uint32_t len = 0;
    if (!ev::isNumber(lenVal) || !lengthWithin(ev::toDouble(lenVal), kMaxHostListLength, len)) return keys;
    keys.reserve(len);
    for (uint32_t k = 0; k < len; ++k) keys.push_back(ev::toUtf8(ev::getElement(arr.get(), k)));
    return keys;
}

// A keyframe value as its string: strings as they are, numbers through
// ToString, anything else empty. Converted at once, never held raw.
std::string keyframeValueString(Value v) {
    return (ev::isString(v) || ev::isNumber(v)) ? ev::toUtf8(v) : std::string();
}

// A keyframe's `easing`: a TypeError on anything that is not an
// <easing-function>, as the spec has it (it used to fall back to `ease`).
bool parseKeyframeEasing(const std::string& es, engine::WebAnimKeyframe& kf, std::string& error) {
    if (es.empty()) return true;
    if (!engine::tryParseEasing(es, kf.easing)) {
        error = "invalid easing '" + es + "'";
        return false;
    }
    kf.hasEasing = true;
    return true;
}

bool parseKeyframeArray(Value arrIn, std::vector<engine::WebAnimKeyframe>& frames,
                        std::string& error) {
    ev::Persistent arr(arrIn);
    Value lenVal = ev::getProperty(arr.get(), "length");
    uint32_t len = 0;
    if (ev::isNumber(lenVal) && !lengthWithin(ev::toDouble(lenVal), kMaxHostListLength, len)) return false;
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
                if (!parseKeyframeEasing(ev::toUtf8(pv), kf, error)) return false;
            } else if (pname != "composite") {
                kf.props.emplace_back(keyToCssProp(pname), keyframeValueString(pv));
            }
        }
        offsets.push_back(off);
        frames.push_back(std::move(kf));
    }
    return computeOffsets(offsets, frames);
}

bool parseKeyframeObject(Value obj, std::vector<engine::WebAnimKeyframe>& frames,
                         std::string& error) {
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
                int64_t arrLen = ev::isNumber(lv) ? satCast<int64_t>(ev::toDouble(lv)) : 0;
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
        if (!easings.empty() && !parseKeyframeEasing(easings[f % easings.size()], kf, error))
            return false;
        frames.push_back(std::move(kf));
    }
    return true;
}

// The native side of an Animation method call: its state, the engine, and
// the record (any may be null).
struct Call {
    AnimationState* st = nullptr;
    engine::Engine* eng = nullptr;
    engine::WebAnimation* rec = nullptr;
};

Call callOf(Value self) {
    Call c;
    c.st = static_cast<AnimationState*>(ev::handleData(self));
    c.eng = hostEngine();
    if (c.st && c.eng) c.rec = c.eng->webAnimationManager().find(c.st->id);
    return c;
}

void markTargetDirty(const Call& c) {
    if (!c.rec || !c.eng) return;
    if (auto* el = c.eng->webAnimationManager().resolveElement(*c.rec)) el->markDirty();
}

const char* replaceStateName(engine::WebAnimReplaceState s) {
    switch (s) {
        case engine::WebAnimReplaceState::Removed: return "removed";
        case engine::WebAnimReplaceState::Persisted: return "persisted";
        default: return "active";
    }
}

} // namespace

Value wrapAnimation(uint64_t id, const std::string& name = "") {
    auto it = liveStates().find(id);
    if (it != liveStates().end()) {
        return it->second->self.get();
    }
    engine::Engine* eng = hostEngine();
    engine::WebAnimation* rec = eng ? eng->webAnimationManager().find(id) : nullptr;
    auto* st = new AnimationState();
    st->id = id;
    st->name = name;
    HostClass& cls = rec && rec->isCssAnimation ? g_cssAnimationClass : g_animationClass;
    ObjectBuilder b(cls.make(st, animationFinalizer));
    Value obj = b.get();
    st->self = ev::Persistent(obj);
    liveStates()[id] = st;
    if (eng) {
        eng->webAnimationManager().noteWrapped(id);
        rec = eng->webAnimationManager().find(id);
        if (rec && (rec->state == engine::WebAnimState::Running ||
                    rec->state == engine::WebAnimState::Paused)) {
            strongPins()[id] = ev::Persistent(obj);
        }
    }
    return obj;
}

namespace {

static ev::Persistent s_skeletalAnimationCtor;

Value animationConstructor(Value self, std::span<const Value> a) {
    (void)self;
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

void decorateAnimationProto(ObjectBuilder& b) {
    b.def("play", 0, [](Value self_, std::span<const Value>) {
        Call c = callOf(self_);
        if (!c.rec) return ev::undefined();
        c.eng->webAnimationManager().play(*c.rec, c.eng->timeNowMs());
        c.rec->cssPlayOverride = true;  // animation-play-state no longer rules it
        if (c.st->promiseSettled) {
            c.st->finishedPromise.set(ev::undefined());
            c.st->promiseSettled = false;
        }
        c.st->finishDelivered = false;
        strongPins()[c.st->id] = ev::Persistent(self_);
        markTargetDirty(c);
        return ev::undefined();
    });

    b.def("pause", 0, [](Value self_, std::span<const Value>) {
        Call c = callOf(self_);
        if (!c.rec) return ev::undefined();
        c.eng->webAnimationManager().pause(*c.rec, c.eng->timeNowMs());
        c.rec->cssPlayOverride = true;
        markTargetDirty(c);
        return ev::undefined();
    });

    b.def("finish", 0, [](Value self_, std::span<const Value>) {
        Call c = callOf(self_);
        if (!c.rec) return ev::undefined();
        if (c.rec->playbackRate == 0 ||
            (c.rec->playbackRate > 0 && !std::isfinite(c.rec->endTimeMs()))) {
            return ev::throwValue(hostMakeDomError("InvalidStateError", "Animation cannot be finished if playbackRate is 0 or end time is infinite"));
        }
        c.eng->webAnimationManager().finishOp(*c.rec);
        markTargetDirty(c);
        settleFinish(c.st, self_);
        return ev::undefined();
    });

    b.def("cancel", 0, [](Value self_, std::span<const Value>) {
        Call c = callOf(self_);
        if (!c.rec || c.rec->state == engine::WebAnimState::Idle) return ev::undefined();
        c.eng->webAnimationManager().cancelOp(*c.rec);
        markTargetDirty(c);
        settleCancel(c.st, self_);
        return ev::undefined();
    });

    b.def("reverse", 0, [](Value self_, std::span<const Value>) {
        Call c = callOf(self_);
        if (!c.rec) return ev::undefined();
        c.eng->webAnimationManager().reverse(*c.rec, c.eng->timeNowMs());
        c.rec->cssPlayOverride = true;
        markTargetDirty(c);
        return ev::undefined();
    });

    // updatePlaybackRate(rate): the rate change without a jump. With no
    // pending tasks in this model it takes effect at once, preserving the
    // current time, as `playbackRate = rate` does on a running animation.
    b.def("updatePlaybackRate", 1, [](Value self_, std::span<const Value> a) {
        Call c = callOf(self_);
        if (!c.rec) return ev::undefined();
        double r = a.empty() ? 1.0 : ev::toDouble(a[0]);
        if (!std::isfinite(r)) return ev::throwTypeError("updatePlaybackRate: rate must be finite");
        c.eng->webAnimationManager().setRate(*c.rec, r, c.eng->timeNowMs());
        markTargetDirty(c);
        return ev::undefined();
    });

    // commitStyles(): write the effect's current values into the target's
    // inline style, so they outlast the animation (the idiom for keeping the
    // end state of a fill:'none' animation, or before cancel()).
    b.def("commitStyles", 0, [](Value self_, std::span<const Value>) {
        Call c = callOf(self_);
        if (!c.rec) return ev::undefined();
        engine::WebAnimationManager& mgr = c.eng->webAnimationManager();
        dom::Element* el = mgr.resolveElement(*c.rec);
        if (!el) {
            return ev::throwValue(hostMakeDomError(
                "InvalidStateError", "commitStyles: the animation has no target element"));
        }
        const uint64_t id = c.st->id;
        c.eng->flushLayoutForRead(el->document());
        engine::WebAnimation* rec = mgr.find(id);
        el = rec ? mgr.resolveElement(*rec) : nullptr;
        if (!rec || !el) return ev::undefined();
        std::vector<std::pair<std::string, std::string>> values;
        if (mgr.effectValues(*rec, el->computedStyle(), c.eng->timeNowMs(), values)) {
            for (const auto& [prop, value] : values) el->style().setProperty(prop, value);
        }
        return ev::undefined();
    });

    // persist(): exempt a finished fill-forwards animation from automatic
    // removal (or bring back one that was removed).
    b.def("persist", 0, [](Value self_, std::span<const Value>) {
        Call c = callOf(self_);
        if (!c.rec) return ev::undefined();
        c.rec->replaceState = engine::WebAnimReplaceState::Persisted;
        markTargetDirty(c);
        return ev::undefined();
    });

    b.accessor("replaceState", [](Value self_, std::span<const Value>) {
        Call c = callOf(self_);
        return ev::fromUtf8(c.rec ? replaceStateName(c.rec->replaceState) : "active");
    }, nullptr);

    b.accessor("playState", [](Value self_, std::span<const Value>) {
        Call c = callOf(self_);
        if (!c.rec) return ev::fromUtf8("idle");
        return ev::fromUtf8(c.eng->webAnimationManager().playState(*c.rec, c.eng->timeNowMs()));
    }, nullptr);

    b.accessor("playbackRate",
        [](Value self_, std::span<const Value>) {
            Call c = callOf(self_);
            return ev::fromDouble(c.rec ? c.rec->playbackRate : 1.0);
        },
        [](Value self_, std::span<const Value> a) {
            Call c = callOf(self_);
            if (c.rec) {
                double r = a.empty() ? 1.0 : ev::toDouble(a[0]);
                c.eng->webAnimationManager().setRate(*c.rec, r, c.eng->timeNowMs());
                markTargetDirty(c);
            }
            return ev::undefined();
        });

    b.accessor("currentTime",
        [](Value self_, std::span<const Value>) {
            Call c = callOf(self_);
            if (!c.rec) return ev::null();
            auto ct = c.rec->currentTimeMs(c.eng->timeNowMs());
            return ct ? ev::fromDouble(*ct) : ev::null();
        },
        [](Value self_, std::span<const Value> a) {
            Call c = callOf(self_);
            if (c.rec && !a.empty()) {
                double ct = ev::toDouble(a[0]);
                c.eng->webAnimationManager().setCurrentTime(*c.rec, ct, c.eng->timeNowMs());
                markTargetDirty(c);
            }
            return ev::undefined();
        });

    // startTime: the timeline time at which currentTime was 0 — null while
    // a hold time (pause, finish, idle) resolves currentTime instead.
    b.accessor("startTime",
        [](Value self_, std::span<const Value>) {
            Call c = callOf(self_);
            if (!c.rec || c.rec->hasHoldTime || !c.rec->hasStartTime ||
                c.rec->state == engine::WebAnimState::Idle) {
                return ev::null();
            }
            return ev::fromDouble(c.rec->startTime);
        },
        [](Value self_, std::span<const Value> a) {
            Call c = callOf(self_);
            if (!c.rec || a.empty() || !ev::isNumber(a[0])) return ev::undefined();
            c.eng->webAnimationManager().setStartTime(*c.rec, ev::toDouble(a[0]), c.eng->timeNowMs());
            markTargetDirty(c);
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
            Call c = callOf(self_);
            if (c.rec) {
                std::string ps = c.eng->webAnimationManager().playState(*c.rec, c.eng->timeNowMs());
                if (ps == "finished") resolveFinishedPromise(st, self_);
            }
        }
        return st->finishedPromise.get();
    }, nullptr);

    auto handlerAccessor = [&b](const char* name, ev::Persistent AnimationState::*slot) {
        b.accessor(name,
            [slot](Value self_, std::span<const Value>) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                return (st && ev::isFunction((st->*slot).get())) ? (st->*slot).get() : ev::null();
            },
            [slot](Value self_, std::span<const Value> a) {
                auto* st = static_cast<AnimationState*>(ev::handleData(self_));
                if (st) {
                    if (!a.empty() && ev::isFunction(a[0])) (st->*slot).set(a[0]);
                    else (st->*slot).set(ev::undefined());
                }
                return ev::undefined();
            });
    };
    handlerAccessor("onfinish", &AnimationState::onfinish);
    handlerAccessor("oncancel", &AnimationState::oncancel);
    handlerAccessor("onremove", &AnimationState::onremove);
}

void decorateCssAnimationProto(ObjectBuilder& b) {
    b.accessor("animationName", [](Value self_, std::span<const Value>) {
        Call c = callOf(self_);
        return ev::fromUtf8(c.rec ? c.rec->cssName : std::string());
    }, nullptr);
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

    g_animationClass.install("Animation", 0, animationConstructor, decorateAnimationProto);
    g_cssAnimationClass.install("CSSAnimation", 0, nullptr, decorateCssAnimationProto);
    g_cssAnimationClass.inherit(g_animationClass);

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
    ev::registerGlobal("CSSAnimation", g_cssAnimationClass.constructor());
    // Each global object is rooted across its defines (setProperty allocates).
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        const Rooted global(gt.value);
        ev::setProperty(global, "Animation", g_animationClass.constructor());
        ev::setProperty(global, "WebAnimation", g_animationClass.constructor());
        ev::setProperty(global, "CSSAnimation", g_cssAnimationClass.constructor());
    }
    ev::GlobalValue win = ev::globalValue("window");
    if (win.found && ev::isObject(win.value)) {
        const Rooted window(win.value);
        ev::setProperty(window, "Animation", g_animationClass.constructor());
        ev::setProperty(window, "WebAnimation", g_animationClass.constructor());
        ev::setProperty(window, "CSSAnimation", g_cssAnimationClass.constructor());
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
        std::string error;
        bool ok = isJsArray(a[0]) ? parseKeyframeArray(a[0], frames, error)
                                  : parseKeyframeObject(a[0], frames, error);
        if (!ok) {
            return ev::throwTypeError(
                ("Element.animate: " + (error.empty() ? std::string("invalid keyframes") : error)).c_str());
        }

        engine::WebAnimation scratch;
        std::string name;
        if (a.size() >= 2 && !ev::isUndefined(a[1]) && !ev::isNull(a[1])) {
            if (ev::isNumber(a[1])) {
                double d = ev::toDouble(a[1]);
                if (!(d >= 0)) return ev::throwTypeError("Element.animate: duration must be a non-negative number");
                scratch.duration = d;
            } else if (ev::isObject(a[1])) {
                uint32_t mask = 0;
                if (!parseEffectTiming(a[1], scratch, mask, error, &name))
                    return ev::throwTypeError(("Element.animate: " + error).c_str());
            } else {
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
        // Style first, as on the web: an animation a class change just
        // started is listed by the getAnimations() that follows it.
        eng->flushLayoutForRead(nst->el->document());
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
        if (eng->document()) eng->flushLayoutForRead(eng->document());
        auto ids = eng->webAnimationManager().allAnimations(eng->timeNowMs());
        return hostArrayOf(ids.size(), [&ids](size_t i) {
            return wrapAnimation(ids[i]);
        });
    });
}

void deliverWebAnimationFinishEvents() {
    engine::Engine* eng = hostEngine();
    if (!eng) return;
    auto& mgr = eng->webAnimationManager();
    for (uint64_t id : mgr.takeFinishedEvents()) {
        auto it = liveStates().find(id);
        if (it != liveStates().end()) {
            AnimationState* st = it->second;
            settleFinish(st, st->self.get());
        }
    }
    // A CSS animation its markup stopped naming, while script held it.
    for (uint64_t id : mgr.takeCanceledEvents()) {
        auto it = liveStates().find(id);
        if (it != liveStates().end()) {
            AnimationState* st = it->second;
            settleCancel(st, st->self.get());
        }
    }
    // Replaced by later animations (Web Animations §5.5).
    for (uint64_t id : mgr.takeRemovedEvents()) {
        auto it = liveStates().find(id);
        if (it != liveStates().end()) {
            AnimationState* st = it->second;
            ev::Persistent selfP(st->self.get());
            Value ct = ev::null();
            if (engine::WebAnimation* rec = mgr.find(id)) {
                if (auto cur = rec->currentTimeMs(eng->timeNowMs())) ct = ev::fromDouble(*cur);
            }
            fireHandler(st->onremove.get(), selfP.get(), "remove", ct);
        }
    }
}

} // namespace bro::bronze_host
