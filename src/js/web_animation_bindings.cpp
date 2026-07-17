#include "js/web_animation_bindings.h"
#include "js/dom_bindings_internal.h"
#include "js/runtime.h"
#include "engine/engine.h"
#include "engine/css_transitions.h"
#include "engine/web_animations.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// ===========================================================================
// element.animate() — Web Animations API (commonly-used subset).
//
// Supported: array-of-keyframes and object-of-arrays keyframe forms, offsets
// (auto-distributed), per-keyframe easing; options as number (duration) or
// {duration, delay, endDelay, iterations (Infinity ok), direction, easing,
// fill, id}; Animation.play/pause/cancel/finish/reverse, currentTime,
// playbackRate, playState, finished promise (rejects with an AbortError-shaped
// DOMException on cancel), onfinish/oncancel; element.getAnimations() and
// document.getAnimations().
//
// Deliberate simplifications (documented in docs/web-animations-api.js):
//  - Property coverage = whatever the CSS transition interpolator supports
//    (numbers/lengths, colors, transform/filter function lists); other values
//    snap at 50%, and properties simply aren't clamped/validated.
//  - Multiple animations on one element compose in creation order (the
//    last-created wins per property) rather than full spec stacking.
//  - commitStyles()/persist() are not implemented; composite modes other than
//    "replace" are ignored.
//  - getAnimations() returns running/paused animations plus finished ones
//    still holding a forwards fill.
// ===========================================================================

namespace bro::js {

using bro::engine::WebAnimation;
using bro::engine::WebAnimationManager;
using bro::engine::WebAnimDirection;
using bro::engine::WebAnimFill;
using bro::engine::WebAnimKeyframe;
using bro::engine::WebAnimState;

namespace {

// ---------------------------------------------------------------------------
// Wrapper state
// ---------------------------------------------------------------------------

struct AnimationJS {
    JSContext* ctx = nullptr;
    WebAnimationManager* mgr = nullptr;
    uint64_t id = 0;
    std::string name; // options.id

    JSValue onfinish = JS_UNDEFINED;
    JSValue oncancel = JS_UNDEFINED;
    // The finished promise is created lazily on first access (like the
    // browsers do) so a cancel with no observer never produces an unhandled
    // rejection. promiseSettled tracks the CURRENT promise object only.
    JSValue finishedPromise = JS_UNDEFINED;
    JSValue finishedResolve = JS_UNDEFINED;
    JSValue finishedReject = JS_UNDEFINED;
    bool promiseSettled = false;
    // onfinish already fired for the current finished state — guards against a
    // synchronous finish() racing a tick-queued finish event (double fire).
    bool finishDelivered = false;

    ~AnimationJS();
};

// id → wrapper, raw mirror (NOT dup'd — maintained by the finalizer, so an
// entry is always a live object; the same pattern as Element::jsWrapper).
std::unordered_map<uint64_t, JSValue>& wrapperMirror() {
    static std::unordered_map<uint64_t, JSValue> m;
    return m;
}

// id → dup'd wrapper. Pins the wrapper while its animation can still deliver
// a finish event (running/paused), mirroring how a live animation stays
// reachable in a browser. Dropped on finish delivery, cancel, and context
// cleanup.
std::unordered_map<uint64_t, JSValue>& strongPins() {
    static std::unordered_map<uint64_t, JSValue> m;
    return m;
}

AnimationJS::~AnimationJS() {
    wrapperMirror().erase(id);
    if (ctx) {
        JSRuntime* rt = JS_GetRuntime(ctx);
        for (JSValue* v : {&onfinish, &oncancel, &finishedPromise,
                           &finishedResolve, &finishedReject}) {
            if (!JS_IsUndefined(*v)) JS_FreeValueRT(rt, *v);
        }
    }
    // Engine teardown destroys the manager before the JS runtime — isLive()
    // makes this a no-op then instead of a use-after-free.
    if (WebAnimationManager::isLive(mgr)) mgr->releaseFromWrapper(id);
}

void pinWrapper(JSContext* ctx, uint64_t id, JSValueConst obj) {
    auto& pins = strongPins();
    if (pins.find(id) == pins.end()) pins[id] = JS_DupValue(ctx, obj);
}

void unpinWrapper(JSContext* ctx, uint64_t id) {
    auto& pins = strongPins();
    auto it = pins.find(id);
    if (it != pins.end()) {
        JS_FreeValue(ctx, it->second);
        pins.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Context helpers
// ---------------------------------------------------------------------------

bro::engine::Engine* engineFor(JSContext* ctx) {
    auto it = s_ctx_engines.find(ctx);
    return it == s_ctx_engines.end() ? nullptr
                                     : static_cast<bro::engine::Engine*>(it->second);
}

double nowFor(JSContext* ctx) {
    auto* eng = engineFor(ctx);
    return eng ? eng->timeNowMs() : 0.0;
}

WebAnimation* recordFor(AnimationJS* a) {
    if (!a || !WebAnimationManager::isLive(a->mgr)) return nullptr;
    return a->mgr->find(a->id);
}

void markTargetDirty(AnimationJS* a, WebAnimation* rec) {
    if (!rec) return;
    if (auto* elem = a->mgr->resolveElement(*rec)) elem->markDirty();
}

// ---------------------------------------------------------------------------
// finished promise plumbing
// ---------------------------------------------------------------------------

JSValue makeAbortError(JSContext* ctx) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "AbortError"));
    JS_SetPropertyStr(ctx, err, "message",
                      JS_NewString(ctx, "The user aborted a request."));
    return err;
}

void dropFinishedPromise(JSContext* ctx, AnimationJS* a) {
    for (JSValue* v : {&a->finishedPromise, &a->finishedResolve, &a->finishedReject}) {
        if (!JS_IsUndefined(*v)) {
            JS_FreeValue(ctx, *v);
            *v = JS_UNDEFINED;
        }
    }
    a->promiseSettled = false;
}

void resolveFinishedPromise(JSContext* ctx, AnimationJS* a, JSValueConst animObj) {
    if (JS_IsUndefined(a->finishedPromise) || a->promiseSettled) return;
    a->promiseSettled = true;
    JSValue arg = JS_DupValue(ctx, animObj);
    JSValue r = JS_Call(ctx, a->finishedResolve, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
}

void rejectFinishedPromise(JSContext* ctx, AnimationJS* a) {
    if (JS_IsUndefined(a->finishedPromise) || a->promiseSettled) return;
    a->promiseSettled = true;
    JSValue err = makeAbortError(ctx);
    JSValue r = JS_Call(ctx, a->finishedReject, JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, err);
}

// Leaving the finished state replaces a settled promise with a fresh pending
// one (per spec); a pending promise is kept.
void freshenPromiseIfSettled(JSContext* ctx, AnimationJS* a) {
    if (a->promiseSettled) dropFinishedPromise(ctx, a);
}

// Fire an onfinish/oncancel handler with an AnimationPlaybackEvent-shaped
// plain object.
void fireHandler(JSContext* ctx, JSValueConst handler, JSValueConst animObj,
                 const char* type, JSValue currentTime) {
    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, currentTime);
        return;
    }
    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, ev, "currentTime", currentTime);
    JS_SetPropertyStr(ctx, ev, "target", JS_DupValue(ctx, animObj));
    JSValue ret = Runtime::callJs(ctx, handler, animObj, 1, &ev,
                                  ErrorOrigin::listener("animation event"));
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, ev);
}

// Settle everything for a finish: resolve the promise (if observed), fire
// onfinish, and drop the pin.
void settleFinish(JSContext* ctx, AnimationJS* a, JSValueConst animObj) {
    if (a->finishDelivered) return;
    a->finishDelivered = true;
    resolveFinishedPromise(ctx, a, animObj);
    WebAnimation* rec = recordFor(a);
    JSValue ct = JS_UNDEFINED;
    if (rec) {
        double v = 0;
        if (rec->currentTimeMs(nowFor(ctx), &v)) ct = JS_NewFloat64(ctx, v);
        else ct = JS_NULL;
    } else {
        ct = JS_NULL;
    }
    fireHandler(ctx, a->onfinish, animObj, "finish", ct);
    unpinWrapper(ctx, a->id);
}

// ---------------------------------------------------------------------------
// Wrapping
// ---------------------------------------------------------------------------

JSValue wrapAnimation(JSContext* ctx, WebAnimationManager* mgr, uint64_t id,
                      std::string name = {}) {
    auto& mirror = wrapperMirror();
    auto mIt = mirror.find(id);
    if (mIt != mirror.end()) return JS_DupValue(ctx, mIt->second);

    auto* a = new AnimationJS();
    a->ctx = ctx;
    a->mgr = mgr;
    a->id = id;
    a->name = std::move(name);
    JSValue obj = qjsbind::wrap<AnimationJS>(ctx, a);
    if (JS_IsException(obj)) return obj;
    mirror[id] = obj;

    // Pin while the animation can still deliver a finish.
    WebAnimation* rec = mgr->find(id);
    if (rec && (rec->state == WebAnimState::Running ||
                rec->state == WebAnimState::Paused)) {
        pinWrapper(ctx, id, obj);
    }
    return obj;
}

// ---------------------------------------------------------------------------
// Keyframe parsing
// ---------------------------------------------------------------------------

// Property key → kebab-case CSS property ("backgroundColor" → "background-color",
// "cssFloat" → "float").
std::string keyToCssProp(const std::string& key) {
    if (key == "cssFloat") return "float";
    if (key == "cssOffset") return "offset";
    return camelToKebab(key);
}

bool valueToCss(JSContext* ctx, JSValueConst v, std::string& out) {
    if (JS_IsNull(v) || JS_IsUndefined(v)) return false;
    out = jsToStdString(ctx, v);
    return true;
}

// Distribute unspecified offsets: singleton keyframe → 1 (animates from the
// base value); otherwise first → 0, last → 1, interior runs spaced evenly
// between their specified neighbors. `spec` holds -1 for unspecified.
// Returns false (and throws) on out-of-range / non-monotonic offsets.
bool computeOffsets(JSContext* ctx, std::vector<double>& spec,
                    std::vector<WebAnimKeyframe>& frames) {
    size_t n = frames.size();
    if (n == 0) return true;
    for (double o : spec) {
        if (o >= 0 && (o < 0 || o > 1 || std::isnan(o))) {
            JS_ThrowTypeError(ctx, "keyframe offset must be in [0, 1]");
            return false;
        }
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
            while (i < n && spec[i] < 0) ++i; // i now at next specified
            double lo = spec[runStart - 1];
            double hi = spec[i];
            size_t count = i - runStart + 1;
            for (size_t k = runStart; k < i; ++k)
                spec[k] = lo + (hi - lo) * static_cast<double>(k - runStart + 1) / count;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && spec[i] < spec[i - 1]) {
            JS_ThrowTypeError(ctx, "keyframe offsets must be monotonically increasing");
            return false;
        }
        frames[i].offset = static_cast<float>(spec[i]);
    }
    return true;
}

// Array-of-keyframes form.
bool parseKeyframeArray(JSContext* ctx, JSValueConst arr,
                        std::vector<WebAnimKeyframe>& frames) {
    int64_t len = 0;
    {
        JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
        JS_ToInt64(ctx, &len, lv);
        JS_FreeValue(ctx, lv);
    }
    std::vector<double> offsets;
    for (int64_t i = 0; i < len; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, arr, static_cast<uint32_t>(i));
        if (!JS_IsObject(item)) {
            JS_FreeValue(ctx, item);
            JS_ThrowTypeError(ctx, "keyframe %d is not an object", static_cast<int>(i));
            return false;
        }
        WebAnimKeyframe kf;
        double off = -1;

        JSPropertyEnum* props = nullptr;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &plen, item,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t p = 0; p < plen; ++p) {
                const char* cname = JS_AtomToCString(ctx, props[p].atom);
                if (!cname) continue;
                std::string pname(cname);
                JS_FreeCString(ctx, cname);
                JSValue pv = JS_GetProperty(ctx, item, props[p].atom);
                if (pname == "offset") {
                    if (!JS_IsNull(pv) && !JS_IsUndefined(pv)) JS_ToFloat64(ctx, &off, pv);
                } else if (pname == "easing") {
                    std::string es = jsToStdString(ctx, pv);
                    if (!es.empty()) {
                        kf.easing = bro::engine::parseTimingFunction(es);
                        kf.hasEasing = true;
                    }
                } else if (pname == "composite") {
                    // only "replace" is implemented — ignore
                } else {
                    std::string val;
                    if (valueToCss(ctx, pv, val))
                        kf.props.emplace_back(keyToCssProp(pname), std::move(val));
                }
                JS_FreeValue(ctx, pv);
            }
            JS_FreePropertyEnum(ctx, props, plen);
        }
        JS_FreeValue(ctx, item);
        frames.push_back(std::move(kf));
        offsets.push_back(off);
    }
    return computeOffsets(ctx, offsets, frames);
}

// Object-of-arrays form: { opacity: [0, 1], transform: ['none', 'scale(2)'] }.
// Each property's m values land at offsets k/(m-1) (a single value at 1);
// keyframes carry only the properties declared at their offset, which the
// per-property interpolator handles directly. A scalar/array `easing` entry is
// applied to the merged keyframes in order (cyclically for arrays).
bool parseKeyframeObject(JSContext* ctx, JSValueConst obj,
                         std::vector<WebAnimKeyframe>& frames) {
    struct PropList {
        std::string prop;
        std::vector<std::string> values;
    };
    std::vector<PropList> lists;
    std::vector<std::string> easings;

    JSPropertyEnum* props = nullptr;
    uint32_t plen = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &plen, obj,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
        return false;
    for (uint32_t p = 0; p < plen; ++p) {
        const char* cname = JS_AtomToCString(ctx, props[p].atom);
        if (!cname) continue;
        std::string pname(cname);
        JS_FreeCString(ctx, cname);
        JSValue pv = JS_GetProperty(ctx, obj, props[p].atom);

        auto collect = [&](std::vector<std::string>& out) {
            if (JS_IsArray(pv)) {
                int64_t len = 0;
                JSValue lv = JS_GetPropertyStr(ctx, pv, "length");
                JS_ToInt64(ctx, &len, lv);
                JS_FreeValue(ctx, lv);
                for (int64_t i = 0; i < len; ++i) {
                    JSValue item = JS_GetPropertyUint32(ctx, pv, static_cast<uint32_t>(i));
                    std::string s;
                    if (valueToCss(ctx, item, s)) out.push_back(std::move(s));
                    JS_FreeValue(ctx, item);
                }
            } else {
                std::string s;
                if (valueToCss(ctx, pv, s)) out.push_back(std::move(s));
            }
        };

        if (pname == "offset" || pname == "composite") {
            // Explicit offset lists in the object form aren't supported —
            // values distribute evenly (documented simplification).
        } else if (pname == "easing") {
            collect(easings);
        } else {
            PropList pl;
            pl.prop = keyToCssProp(pname);
            collect(pl.values);
            if (!pl.values.empty()) lists.push_back(std::move(pl));
        }
        JS_FreeValue(ctx, pv);
    }
    JS_FreePropertyEnum(ctx, props, plen);

    // Merge distinct offsets across properties.
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
        WebAnimKeyframe kf;
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
                kf.easing = bro::engine::parseTimingFunction(es);
                kf.hasEasing = true;
            }
        }
        frames.push_back(std::move(kf));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Options parsing
// ---------------------------------------------------------------------------

bool parseOptions(JSContext* ctx, JSValueConst opt, WebAnimation& a,
                  std::string& name) {
    if (JS_IsUndefined(opt) || JS_IsNull(opt)) return true;
    if (JS_IsNumber(opt)) {
        double d = 0;
        JS_ToFloat64(ctx, &d, opt);
        if (!(d >= 0)) {
            JS_ThrowTypeError(ctx, "animate(): duration must be a non-negative number");
            return false;
        }
        a.duration = d;
        return true;
    }
    if (!JS_IsObject(opt)) {
        JS_ThrowTypeError(ctx, "animate(): options must be a number or an object");
        return false;
    }

    auto num = [&](const char* key, double* out) -> bool { // true if present
        JSValue v = JS_GetPropertyStr(ctx, opt, key);
        bool present = !JS_IsUndefined(v) && !JS_IsNull(v);
        if (present) JS_ToFloat64(ctx, out, v);
        JS_FreeValue(ctx, v);
        return present;
    };
    auto str = [&](const char* key) -> std::string {
        JSValue v = JS_GetPropertyStr(ctx, opt, key);
        std::string s;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) s = jsToStdString(ctx, v);
        JS_FreeValue(ctx, v);
        return s;
    };

    double d = 0;
    if (num("duration", &d)) {
        if (!(d >= 0)) { // rejects NaN and negatives ("auto" parses as NaN → 0 below)
            JS_ThrowTypeError(ctx, "animate(): duration must be a non-negative number");
            return false;
        }
        a.duration = d;
    }
    if (num("delay", &d)) a.delay = d;
    if (num("endDelay", &d)) a.endDelay = d;
    if (num("iterations", &d)) {
        if (std::isnan(d) || d < 0) {
            JS_ThrowTypeError(ctx, "animate(): iterations must be a non-negative number");
            return false;
        }
        a.iterations = d;
    }

    std::string dir = str("direction");
    if (dir == "reverse") a.direction = WebAnimDirection::Reverse;
    else if (dir == "alternate") a.direction = WebAnimDirection::Alternate;
    else if (dir == "alternate-reverse") a.direction = WebAnimDirection::AlternateReverse;

    std::string fill = str("fill");
    if (fill == "forwards") a.fill = WebAnimFill::Forwards;
    else if (fill == "backwards") a.fill = WebAnimFill::Backwards;
    else if (fill == "both") a.fill = WebAnimFill::Both;

    std::string easing = str("easing");
    if (!easing.empty()) a.easing = bro::engine::parseTimingFunction(easing);
    // (default stays linear — the WAAPI default, unlike CSS's "ease")

    name = str("id");
    return true;
}

// ---------------------------------------------------------------------------
// Animation prototype
// ---------------------------------------------------------------------------

AnimationJS* self(JSValueConst v) {
    return static_cast<AnimationJS*>(JS_GetOpaque(v, qjsbind::class_id<AnimationJS>()));
}

JSValue js_anim_play(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* a = self(this_val);
    WebAnimation* rec = recordFor(a);
    if (!rec) return JS_UNDEFINED; // torn down — inert
    a->mgr->play(*rec, nowFor(ctx));
    freshenPromiseIfSettled(ctx, a);
    a->finishDelivered = false;
    pinWrapper(ctx, a->id, this_val);
    markTargetDirty(a, rec);
    return JS_UNDEFINED;
}

JSValue js_anim_pause(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* a = self(this_val);
    WebAnimation* rec = recordFor(a);
    if (!rec) return JS_UNDEFINED;
    a->mgr->pause(*rec, nowFor(ctx));
    markTargetDirty(a, rec);
    return JS_UNDEFINED;
}

JSValue js_anim_cancel(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* a = self(this_val);
    WebAnimation* rec = recordFor(a);
    if (!rec || rec->state == WebAnimState::Idle) return JS_UNDEFINED;
    a->mgr->cancelOp(*rec);
    markTargetDirty(a, rec);
    // Reject the current finished promise with an AbortError, then replace it
    // (lazily) with a fresh pending one, per spec.
    rejectFinishedPromise(ctx, a);
    dropFinishedPromise(ctx, a);
    a->finishDelivered = false;
    fireHandler(ctx, a->oncancel, this_val, "cancel", JS_NULL);
    unpinWrapper(ctx, a->id);
    return JS_UNDEFINED;
}

JSValue js_anim_finish(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* a = self(this_val);
    WebAnimation* rec = recordFor(a);
    if (!rec) return JS_UNDEFINED;
    if (rec->playbackRate > 0 && !std::isfinite(rec->endTimeMs())) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "InvalidStateError"));
        JS_SetPropertyStr(ctx, err, "message",
                          JS_NewString(ctx, "Cannot finish an infinite animation"));
        return JS_Throw(ctx, err);
    }
    a->mgr->finishOp(*rec);
    markTargetDirty(a, rec);
    settleFinish(ctx, a, this_val);
    return JS_UNDEFINED;
}

JSValue js_anim_reverse(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* a = self(this_val);
    WebAnimation* rec = recordFor(a);
    if (!rec) return JS_UNDEFINED;
    a->mgr->reverse(*rec, nowFor(ctx));
    freshenPromiseIfSettled(ctx, a);
    a->finishDelivered = false;
    pinWrapper(ctx, a->id, this_val);
    markTargetDirty(a, rec);
    return JS_UNDEFINED;
}

JSValue js_anim_get_currentTime(JSContext* ctx, JSValueConst this_val) {
    auto* a = self(this_val);
    WebAnimation* rec = recordFor(a);
    if (!rec) return JS_NULL;
    double ct = 0;
    if (!rec->currentTimeMs(nowFor(ctx), &ct)) return JS_NULL;
    return JS_NewFloat64(ctx, ct);
}

JSValue js_anim_set_currentTime(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* a = self(this_val);
    WebAnimation* rec = recordFor(a);
    if (!rec) return JS_UNDEFINED;
    if (JS_IsNull(val) || JS_IsUndefined(val))
        return JS_ThrowTypeError(ctx, "currentTime may not be set to null");
    double t = 0;
    JS_ToFloat64(ctx, &t, val);
    bool wasFinished = rec->state == WebAnimState::Finished;
    a->mgr->seek(*rec, t, nowFor(ctx));
    if (wasFinished && rec->state != WebAnimState::Finished) {
        freshenPromiseIfSettled(ctx, a);
        a->finishDelivered = false;
        pinWrapper(ctx, a->id, this_val);
    }
    markTargetDirty(a, rec);
    return JS_UNDEFINED;
}

JSValue js_anim_get_playbackRate(JSContext* ctx, JSValueConst this_val) {
    auto* a = self(this_val);
    WebAnimation* rec = recordFor(a);
    return JS_NewFloat64(ctx, rec ? rec->playbackRate : 1.0);
}

JSValue js_anim_set_playbackRate(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* a = self(this_val);
    WebAnimation* rec = recordFor(a);
    if (!rec) return JS_UNDEFINED;
    double r = 0;
    JS_ToFloat64(ctx, &r, val);
    a->mgr->setRate(*rec, r, nowFor(ctx));
    markTargetDirty(a, rec);
    return JS_UNDEFINED;
}

JSValue js_anim_get_playState(JSContext* ctx, JSValueConst this_val) {
    auto* a = self(this_val);
    WebAnimation* rec = recordFor(a);
    if (!rec) return JS_NewString(ctx, "idle");
    return JS_NewString(ctx, a->mgr->playState(*rec, nowFor(ctx)));
}

JSValue js_anim_get_pending(JSContext* ctx, JSValueConst this_val) {
    (void)this_val;
    (void)ctx;
    return JS_FALSE; // play/pause apply immediately in this engine
}

JSValue js_anim_get_id(JSContext* ctx, JSValueConst this_val) {
    auto* a = self(this_val);
    return JS_NewString(ctx, a ? a->name.c_str() : "");
}

JSValue js_anim_set_id(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* a = self(this_val);
    if (a) a->name = jsToStdString(ctx, val);
    return JS_UNDEFINED;
}

JSValue js_anim_get_finished(JSContext* ctx, JSValueConst this_val) {
    auto* a = self(this_val);
    if (!a) return JS_NULL;
    if (JS_IsUndefined(a->finishedPromise)) {
        JSValue funcs[2];
        a->finishedPromise = JS_NewPromiseCapability(ctx, funcs);
        a->finishedResolve = funcs[0];
        a->finishedReject = funcs[1];
        a->promiseSettled = false;
        // Already finished? Settle the freshly-minted promise immediately.
        WebAnimation* rec = recordFor(a);
        if (rec) {
            const char* st = a->mgr->playState(*rec, nowFor(ctx));
            if (std::string(st) == "finished")
                resolveFinishedPromise(ctx, a, this_val);
        }
    }
    return JS_DupValue(ctx, a->finishedPromise);
}

JSValue js_anim_get_onfinish(JSContext* ctx, JSValueConst this_val) {
    auto* a = self(this_val);
    return a ? JS_DupValue(ctx, a->onfinish) : JS_NULL;
}

JSValue js_anim_set_onfinish(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* a = self(this_val);
    if (!a) return JS_UNDEFINED;
    if (!JS_IsUndefined(a->onfinish)) JS_FreeValue(ctx, a->onfinish);
    a->onfinish = JS_IsFunction(ctx, val) ? JS_DupValue(ctx, val) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

JSValue js_anim_get_oncancel(JSContext* ctx, JSValueConst this_val) {
    auto* a = self(this_val);
    return a ? JS_DupValue(ctx, a->oncancel) : JS_NULL;
}

JSValue js_anim_set_oncancel(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* a = self(this_val);
    if (!a) return JS_UNDEFINED;
    if (!JS_IsUndefined(a->oncancel)) JS_FreeValue(ctx, a->oncancel);
    a->oncancel = JS_IsFunction(ctx, val) ? JS_DupValue(ctx, val) : JS_UNDEFINED;
    return JS_UNDEFINED;
}

const JSCFunctionListEntry js_animation_proto_funcs[] = {
    JS_CFUNC_DEF("play",    0, js_anim_play),
    JS_CFUNC_DEF("pause",   0, js_anim_pause),
    JS_CFUNC_DEF("cancel",  0, js_anim_cancel),
    JS_CFUNC_DEF("finish",  0, js_anim_finish),
    JS_CFUNC_DEF("reverse", 0, js_anim_reverse),
    JS_CGETSET_DEF("currentTime",  js_anim_get_currentTime,  js_anim_set_currentTime),
    JS_CGETSET_DEF("playbackRate", js_anim_get_playbackRate, js_anim_set_playbackRate),
    JS_CGETSET_DEF("playState",    js_anim_get_playState,    nullptr),
    JS_CGETSET_DEF("pending",      js_anim_get_pending,      nullptr),
    JS_CGETSET_DEF("id",           js_anim_get_id,           js_anim_set_id),
    JS_CGETSET_DEF("finished",     js_anim_get_finished,     nullptr),
    JS_CGETSET_DEF("onfinish",     js_anim_get_onfinish,     js_anim_set_onfinish),
    JS_CGETSET_DEF("oncancel",     js_anim_get_oncancel,     js_anim_set_oncancel),
};

// ---------------------------------------------------------------------------
// element.animate / getAnimations, document.getAnimations
// ---------------------------------------------------------------------------

JSValue js_element_animate(JSContext* ctx, JSValueConst this_val,
                           int argc, JSValueConst* argv) {
    auto* el = getElement(this_val);
    if (!el) return JS_ThrowTypeError(ctx, "animate(): invalid element");
    auto* eng = engineFor(ctx);
    if (!eng)
        return JS_ThrowTypeError(ctx, "animate(): not available in this context");
    if (argc < 1 || (!JS_IsObject(argv[0]) && !JS_IsNull(argv[0])))
        return JS_ThrowTypeError(ctx, "animate(): keyframes must be an object or array");

    std::vector<WebAnimKeyframe> frames;
    if (!JS_IsNull(argv[0])) {
        bool ok = JS_IsArray(argv[0]) ? parseKeyframeArray(ctx, argv[0], frames)
                                      : parseKeyframeObject(ctx, argv[0], frames);
        if (!ok) return JS_EXCEPTION;
    }

    auto& mgr = eng->webAnimationManager();
    double now = eng->timeNowMs();

    // Parse options into a scratch record first so a throw leaves no record.
    WebAnimation scratch;
    std::string name;
    if (argc >= 2 && !parseOptions(ctx, argv[1], scratch, name))
        return JS_EXCEPTION;

    WebAnimation& rec = mgr.create(el, now);
    rec.keyframes = std::move(frames);
    rec.duration = scratch.duration;
    rec.delay = scratch.delay;
    rec.endDelay = scratch.endDelay;
    rec.iterations = scratch.iterations;
    rec.direction = scratch.direction;
    rec.fill = scratch.fill;
    rec.easing = scratch.easing;

    el->markDirty();
    return wrapAnimation(ctx, &mgr, rec.id, std::move(name));
}

JSValue js_element_getAnimations(JSContext* ctx, JSValueConst this_val,
                                 int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    auto* el = getElement(this_val);
    auto* eng = engineFor(ctx);
    if (!el || !eng) return arr;
    auto& mgr = eng->webAnimationManager();
    uint32_t idx = 0;
    for (uint64_t id : mgr.animationsFor(el, eng->timeNowMs()))
        JS_SetPropertyUint32(ctx, arr, idx++, wrapAnimation(ctx, &mgr, id));
    return arr;
}

JSValue js_document_getAnimations(JSContext* ctx, JSValueConst /*this_val*/,
                                  int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    auto* eng = engineFor(ctx);
    if (!eng) return arr;
    auto& mgr = eng->webAnimationManager();
    uint32_t idx = 0;
    for (uint64_t id : mgr.allAnimations(eng->timeNowMs()))
        JS_SetPropertyUint32(ctx, arr, idx++, wrapAnimation(ctx, &mgr, id));
    return arr;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void installWebAnimationBindings(JSContext* ctx) {
    qjsbind::Class<AnimationJS>(ctx, "Animation", qjsbind::NoGlobal)
        .gc_mark([](AnimationJS* a, JSRuntime* rt, JS_MarkFunc* mark) {
            JS_MarkValue(rt, a->onfinish, mark);
            JS_MarkValue(rt, a->oncancel, mark);
            JS_MarkValue(rt, a->finishedPromise, mark);
            JS_MarkValue(rt, a->finishedResolve, mark);
            JS_MarkValue(rt, a->finishedReject, mark);
        })
        .function_list(js_animation_proto_funcs,
                       sizeof(js_animation_proto_funcs) /
                           sizeof(js_animation_proto_funcs[0]));

    JSValue eproto = JS_GetClassProto(ctx, js_element_class_id);
    if (JS_IsObject(eproto)) {
        JS_SetPropertyStr(ctx, eproto, "animate",
            JS_NewCFunction(ctx, js_element_animate, "animate", 2));
        JS_SetPropertyStr(ctx, eproto, "getAnimations",
            JS_NewCFunction(ctx, js_element_getAnimations, "getAnimations", 0));
    }
    JS_FreeValue(ctx, eproto);

    JSValue dproto = JS_GetClassProto(ctx, js_document_class_id);
    if (JS_IsObject(dproto)) {
        JS_SetPropertyStr(ctx, dproto, "getAnimations",
            JS_NewCFunction(ctx, js_document_getAnimations, "getAnimations", 0));
    }
    JS_FreeValue(ctx, dproto);
}

void cleanupWebAnimationBindings(JSContext* ctx) {
    auto& pins = strongPins();
    for (auto it = pins.begin(); it != pins.end(); ) {
        auto* a = static_cast<AnimationJS*>(
            JS_GetOpaque(it->second, qjsbind::class_id<AnimationJS>()));
        if (a && a->ctx == ctx) {
            JS_FreeValue(ctx, it->second);
            it = pins.erase(it);
        } else {
            ++it;
        }
    }
}

void deliverWebAnimationFinishEvents(JSContext* ctx, std::vector<uint64_t> ids) {
    for (uint64_t id : ids) {
        auto mIt = wrapperMirror().find(id);
        if (mIt == wrapperMirror().end()) continue; // wrapper already gone
        JSValue obj = JS_DupValue(ctx, mIt->second);
        auto* a = static_cast<AnimationJS*>(
            JS_GetOpaque(obj, qjsbind::class_id<AnimationJS>()));
        if (a && a->ctx == ctx) settleFinish(ctx, a, obj);
        JS_FreeValue(ctx, obj);
    }
}

} // namespace bro::js
