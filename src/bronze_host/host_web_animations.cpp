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
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

static bool isJsArray(Value v) {
    if (!ev::isObject(v)) return false;
    Value isArrFn = ev::getProperty(ev::globalValue("Array").value, "isArray");
    if (!ev::isFunction(isArrFn)) return false;
    Value res = ev::call(isArrFn, ev::undefined(), std::span<const Value>(&v, 1)).value;
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
    ObjectBuilder eb;
    eb.set("type", ev::fromUtf8(type));
    eb.set("currentTime", currentTime);
    eb.set("target", animObj);
    Value evt = eb.get();
    Value args[1] = { evt };
    ev::call(handler, animObj, std::span<const Value>(args, 1));
}

void settleFinish(AnimationState* st, Value animObj) {
    if (st->finishDelivered) return;
    st->finishDelivered = true;
    resolveFinishedPromise(st, animObj);
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
    fireHandler(st->onfinish.get(), animObj, "finish", ct);
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

bool parseKeyframeArray(Value arr, std::vector<engine::WebAnimKeyframe>& frames) {
    Value lenVal = ev::getProperty(arr, "length");
    int64_t len = ev::isNumber(lenVal) ? static_cast<int64_t>(ev::toDouble(lenVal)) : 0;
    std::vector<double> offsets;
    Value objCtor = ev::globalValue("Object").value;
    Value keysFn = ev::getProperty(objCtor, "keys");

    for (int64_t i = 0; i < len; ++i) {
        Value item = ev::getElement(arr, static_cast<uint32_t>(i));
        if (!ev::isObject(item)) return false;
        engine::WebAnimKeyframe kf;
        double off = -1.0;

        Value keysArr = ev::call(keysFn, objCtor, std::span<const Value>(&item, 1)).value;
        Value klenVal = ev::getProperty(keysArr, "length");
        uint32_t klen = ev::isNumber(klenVal) ? static_cast<uint32_t>(ev::toDouble(klenVal)) : 0;
        for (uint32_t k = 0; k < klen; ++k) {
            std::string pname = ev::toUtf8(ev::getElement(keysArr, k));
            Value pv = ev::getProperty(item, pname.c_str());
            if (pname == "offset") {
                if (ev::isNumber(pv)) off = ev::toDouble(pv);
            } else if (pname == "easing") {
                std::string es = ev::toUtf8(pv);
                if (!es.empty()) {
                    kf.easing = engine::parseTimingFunction(es);
                    kf.hasEasing = true;
                }
            } else {
                std::string valStr = ev::isString(pv) ? ev::toUtf8(pv) :
                                     (ev::isNumber(pv) ? std::to_string(ev::toDouble(pv)) : "");
                kf.props.emplace_back(keyToCssProp(pname), valStr);
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

    Value objCtor = ev::globalValue("Object").value;
    Value keysFn = ev::getProperty(objCtor, "keys");
    Value keysArr = ev::call(keysFn, objCtor, std::span<const Value>(&obj, 1)).value;
    Value klenVal = ev::getProperty(keysArr, "length");
    uint32_t klen = ev::isNumber(klenVal) ? static_cast<uint32_t>(ev::toDouble(klenVal)) : 0;

    for (uint32_t p = 0; p < klen; ++p) {
        std::string pname = ev::toUtf8(ev::getElement(keysArr, p));
        Value pv = ev::getProperty(obj, pname.c_str());

        auto collect = [&](std::vector<std::string>& out) {
            if (isJsArray(pv)) {
                Value lv = ev::getProperty(pv, "length");
                int64_t arrLen = ev::isNumber(lv) ? static_cast<int64_t>(ev::toDouble(lv)) : 0;
                for (int64_t i = 0; i < arrLen; ++i) {
                    Value el = ev::getElement(pv, static_cast<uint32_t>(i));
                    std::string s = ev::isString(el) ? ev::toUtf8(el) :
                                    (ev::isNumber(el) ? std::to_string(ev::toDouble(el)) : "");
                    out.push_back(std::move(s));
                }
            } else {
                std::string s = ev::isString(pv) ? ev::toUtf8(pv) :
                                (ev::isNumber(pv) ? std::to_string(ev::toDouble(pv)) : "");
                out.push_back(std::move(s));
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

bool parseOptions(Value opt, engine::WebAnimation& a, std::string& name) {
    if (ev::isUndefined(opt) || ev::isNull(opt)) return true;
    if (ev::isNumber(opt)) {
        double d = ev::toDouble(opt);
        if (!(d >= 0)) return false;
        a.duration = d;
        return true;
    }
    if (!ev::isObject(opt)) return false;

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

void installWebAnimationGlobals() {
    g_animationClass.init("Animation", [](ObjectBuilder& b) {
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
            rejectFinishedPromise(st);
            st->finishedPromise.set(ev::undefined());
            st->promiseSettled = false;
            st->finishDelivered = false;
            fireHandler(st->oncancel.get(), self_, "cancel", ev::null());
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

    ev::registerGlobal("Animation", g_animationClass.constructor());
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
