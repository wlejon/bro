// Scene JS bindings — data-driven animation clips: the ClipPlayer returned by
// scene.createAnimationPlayer() and the plain-JSON clipDef parser feeding it.
// The full data model and playback semantics live in docs/animation-api.js
// and src/scene/clip_player.h; this unit only translates JSON ↔ AnimationClip
// and forwards control calls through the id + weak-token liveness scheme.

#include "js/scene_bindings.h"
#if BRO_WITH_3D  // modular-build feature gate
#include "js/scene_bindings_internal.h"
#include "js/runtime.h"
#include "scene/clip_player.h"
#include "scene/scene_graph.h"

#include <qjsbind/qjsbind.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace bro::js {

static scene::ClipPlayer* getPlayer(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<ClipPlayerWrapper>(ctx, val);
    return w ? w->player() : nullptr;
}

// ---------------------------------------------------------------------------
// clipDef parsing
// ---------------------------------------------------------------------------

namespace {

struct PropInfo {
    const char* name;
    scene::ClipProp prop;
    int stride;
};

// The supported property set. "quaternion" is an alias for "rotation",
// matching the tween props object.
const PropInfo kClipProps[] = {
    {"position",   scene::ClipProp::Position,  3},
    {"rotation",   scene::ClipProp::Rotation,  4},
    {"quaternion", scene::ClipProp::Rotation,  4},
    {"scale",      scene::ClipProp::Scale,     3},
    {"opacity",    scene::ClipProp::Opacity,   1},
    {"color",      scene::ClipProp::Color,     3},
    {"fov",        scene::ClipProp::Fov,       1},
    {"intensity",  scene::ClipProp::Intensity, 1},
    {"range",      scene::ClipProp::Range,     1},
    {"emissive",   scene::ClipProp::Emissive,  1},
    {"metallic",   scene::ClipProp::Metallic,  1},
    {"roughness",  scene::ClipProp::Roughness, 1},
};

const char* kClipPropList =
    "position, rotation, quaternion, scale, opacity, color, fov, intensity, "
    "range, emissive, metallic, roughness";

const PropInfo* findClipProp(const std::string& name) {
    for (const auto& p : kClipProps)
        if (name == p.name) return &p;
    return nullptr;
}

// Rotation key values additionally accept {euler: [rx, ry, rz]} (radians).
// Plain arrays are ALWAYS quaternions [x,y,z,w] — use the euler form for
// angles (documented in docs/animation-api.js).
bool readKeyRotation(JSContext* ctx, JSValueConst v, bromath::Quat& out) {
    if (JS_IsObject(v) && !JS_IsArray(v)) {
        JSValue e = JS_GetPropertyStr(ctx, v, "euler");
        if (!JS_IsUndefined(e)) {
            bromath::Vec3 r;
            bool ok = readTweenVec3(ctx, e, r);
            JS_FreeValue(ctx, e);
            if (!ok) return false;
            out = bromath::qfromEuler(r);
            return true;
        }
        JS_FreeValue(ctx, e);
    }
    return readTweenQuat(ctx, v, out);
}

// Read one key's value for `info` into `dst` (stride floats). Returns false
// (with `err` set) on a malformed value.
bool readKeyValue(JSContext* ctx, JSValueConst v, const PropInfo& info,
                  float* dst, std::string& err) {
    switch (info.prop) {
        case scene::ClipProp::Rotation: {
            bromath::Quat q;
            if (!readKeyRotation(ctx, v, q)) {
                err = "rotation key value must be a quaternion [x,y,z,w], "
                      "{axis, angle}, {euler: [rx,ry,rz]}, or a number "
                      "(Z radians)";
                return false;
            }
            dst[0] = q.x; dst[1] = q.y; dst[2] = q.z; dst[3] = q.w;
            return true;
        }
        case scene::ClipProp::Color: {
            bromath::Vec3 c;
            if (!readTweenColor(ctx, v, c)) {
                err = "color key value must be [r,g,b] (0-1) or a CSS color "
                      "string";
                return false;
            }
            dst[0] = c.x; dst[1] = c.y; dst[2] = c.z;
            return true;
        }
        case scene::ClipProp::Position:
        case scene::ClipProp::Scale: {
            bromath::Vec3 p;
            if (!readTweenVec3(ctx, v, p)) {
                err = "key value must be [x,y,z] (or a number for a uniform "
                      "splat)";
                return false;
            }
            dst[0] = p.x; dst[1] = p.y; dst[2] = p.z;
            return true;
        }
        default: {
            if (!JS_IsNumber(v)) {
                err = std::string("'") + info.name +
                      "' key value must be a number";
                return false;
            }
            dst[0] = (float)jsNum(ctx, v);
            // JS camera fov is degrees (matching camera.fov / setCamera);
            // the engine setter takes radians.
            if (info.prop == scene::ClipProp::Fov)
                dst[0] *= 3.14159265f / 180.0f;
            return true;
        }
    }
}

int32_t jsArrayLength(JSContext* ctx, JSValueConst v) {
    JSValue lenVal = JS_GetPropertyStr(ctx, v, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    return len;
}

// Parse one property track {target, property, keys: [...]}. Keys are sorted
// by time (stable) so authors may list them in any order.
bool parsePropTrack(JSContext* ctx, JSValueConst trackVal,
                    scene::AnimationClip::PropTrack& out, std::string& err) {
    out.target = qjsbind::get_prop_string(ctx, trackVal, "target", "");
    if (out.target.empty()) {
        err = "property track needs a non-empty 'target' node name";
        return false;
    }
    std::string propName =
        qjsbind::get_prop_string(ctx, trackVal, "property", "");
    const PropInfo* info = findClipProp(propName);
    if (!info) {
        err = "unknown property '" + propName + "' (supported: " +
              kClipPropList + ")";
        return false;
    }
    out.prop = info->prop;
    out.stride = info->stride;

    JSValue keysVal = JS_GetPropertyStr(ctx, trackVal, "keys");
    int32_t nKeys = JS_IsArray(keysVal) ? jsArrayLength(ctx, keysVal) : 0;
    if (nKeys < 1) {
        JS_FreeValue(ctx, keysVal);
        err = "track '" + out.target + "." + propName +
              "' needs a non-empty 'keys' array";
        return false;
    }

    struct Parsed {
        float time;
        float value[4];
        scene::ClipInterp interp;
        scene::Tween::Ease ease;
    };
    std::vector<Parsed> keys((size_t)nKeys);

    for (int32_t i = 0; i < nKeys; i++) {
        JSValue keyVal = JS_GetPropertyUint32(ctx, keysVal, (uint32_t)i);
        Parsed& k = keys[(size_t)i];

        double time = qjsbind::get_prop_number(ctx, keyVal, "time", NAN);
        if (!std::isfinite(time) || time < 0.0) {
            JS_FreeValue(ctx, keyVal);
            JS_FreeValue(ctx, keysVal);
            err = "key " + std::to_string(i) + " of '" + out.target + "." +
                  propName + "' needs a finite time >= 0";
            return false;
        }
        k.time = (float)time;

        JSValue v = JS_GetPropertyStr(ctx, keyVal, "value");
        bool ok = readKeyValue(ctx, v, *info, k.value, err);
        JS_FreeValue(ctx, v);
        if (!ok) {
            JS_FreeValue(ctx, keyVal);
            JS_FreeValue(ctx, keysVal);
            err = "key " + std::to_string(i) + " of '" + out.target + "." +
                  propName + "': " + err;
            return false;
        }

        std::string interp =
            qjsbind::get_prop_string(ctx, keyVal, "interp", "linear");
        if (interp == "linear")     k.interp = scene::ClipInterp::Linear;
        else if (interp == "step")  k.interp = scene::ClipInterp::Step;
        else if (interp == "cubic") k.interp = scene::ClipInterp::Cubic;
        else {
            JS_FreeValue(ctx, keyVal);
            JS_FreeValue(ctx, keysVal);
            err = "unknown interp '" + interp +
                  "' (use 'linear', 'step', or 'cubic')";
            return false;
        }

        std::string ease =
            qjsbind::get_prop_string(ctx, keyVal, "ease", "linear");
        k.ease = scene::Tween::Ease::Linear;
        if (!scene::Tween::easeFromString(ease, k.ease)) {
            JS_FreeValue(ctx, keyVal);
            JS_FreeValue(ctx, keysVal);
            err = "unknown ease '" + ease + "'";
            return false;
        }
        JS_FreeValue(ctx, keyVal);
    }
    JS_FreeValue(ctx, keysVal);

    std::stable_sort(keys.begin(), keys.end(),
                     [](const Parsed& a, const Parsed& b) {
                         return a.time < b.time;
                     });

    out.times.reserve(keys.size());
    out.values.reserve(keys.size() * (size_t)out.stride);
    out.interps.reserve(keys.size());
    out.eases.reserve(keys.size());
    for (const Parsed& k : keys) {
        out.times.push_back(k.time);
        for (int c = 0; c < out.stride; c++) out.values.push_back(k.value[c]);
        out.interps.push_back(k.interp);
        out.eases.push_back(k.ease);
    }
    return true;
}

// Parse one event track {type: 'event', keys: [{time, name, args?}]}.
// `args` is JSON-stringified here and parsed again when the event fires, so
// the engine stays JS-free and the payload round-trips by value.
bool parseEventTrack(JSContext* ctx, JSValueConst trackVal,
                     scene::AnimationClip::EventTrack& out, std::string& err) {
    JSValue keysVal = JS_GetPropertyStr(ctx, trackVal, "keys");
    int32_t nKeys = JS_IsArray(keysVal) ? jsArrayLength(ctx, keysVal) : 0;
    if (nKeys < 1) {
        JS_FreeValue(ctx, keysVal);
        err = "event track needs a non-empty 'keys' array";
        return false;
    }
    out.keys.resize((size_t)nKeys);
    for (int32_t i = 0; i < nKeys; i++) {
        JSValue keyVal = JS_GetPropertyUint32(ctx, keysVal, (uint32_t)i);
        auto& k = out.keys[(size_t)i];

        double time = qjsbind::get_prop_number(ctx, keyVal, "time", NAN);
        k.name = qjsbind::get_prop_string(ctx, keyVal, "name", "");
        if (!std::isfinite(time) || time < 0.0 || k.name.empty()) {
            JS_FreeValue(ctx, keyVal);
            JS_FreeValue(ctx, keysVal);
            err = "event key " + std::to_string(i) +
                  " needs a finite time >= 0 and a non-empty 'name'";
            return false;
        }
        k.time = (float)time;

        JSValue args = JS_GetPropertyStr(ctx, keyVal, "args");
        if (!JS_IsUndefined(args) && !JS_IsNull(args)) {
            JSValue json =
                JS_JSONStringify(ctx, args, JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsException(json) || JS_IsUndefined(json)) {
                JS_FreeValue(ctx, json);
                JS_FreeValue(ctx, args);
                JS_FreeValue(ctx, keyVal);
                JS_FreeValue(ctx, keysVal);
                err = "event key " + std::to_string(i) +
                      ": args must be JSON-serializable";
                return false;
            }
            k.argsJson = jsStr(ctx, json);
            JS_FreeValue(ctx, json);
        }
        JS_FreeValue(ctx, args);
        JS_FreeValue(ctx, keyVal);
    }
    JS_FreeValue(ctx, keysVal);

    std::stable_sort(out.keys.begin(), out.keys.end(),
                     [](const scene::AnimationClip::EventKey& a,
                        const scene::AnimationClip::EventKey& b) {
                         return a.time < b.time;
                     });
    return true;
}

// Parse a whole clipDef. On success `out` also carries the verbatim JSON of
// the def (the clipDef() round-trip source).
bool parseClipDef(JSContext* ctx, JSValueConst def,
                  scene::AnimationClip& out, std::string& err) {
    if (!JS_IsObject(def)) {
        err = "clipDef must be an object";
        return false;
    }

    std::string loop = qjsbind::get_prop_string(ctx, def, "loop", "none");
    if (loop == "none")          out.loop = scene::AnimationClip::Loop::None;
    else if (loop == "loop")     out.loop = scene::AnimationClip::Loop::Loop;
    else if (loop == "pingpong") out.loop = scene::AnimationClip::Loop::PingPong;
    else {
        err = "unknown loop mode '" + loop +
              "' (use 'none', 'loop', or 'pingpong')";
        return false;
    }

    JSValue tracksVal = JS_GetPropertyStr(ctx, def, "tracks");
    int32_t nTracks = JS_IsArray(tracksVal) ? jsArrayLength(ctx, tracksVal) : 0;
    if (nTracks < 1) {
        JS_FreeValue(ctx, tracksVal);
        err = "clipDef needs a non-empty 'tracks' array";
        return false;
    }
    float maxKeyTime = 0.0f;
    for (int32_t i = 0; i < nTracks; i++) {
        JSValue trackVal = JS_GetPropertyUint32(ctx, tracksVal, (uint32_t)i);
        std::string type =
            qjsbind::get_prop_string(ctx, trackVal, "type", "property");
        bool ok;
        if (type == "event") {
            scene::AnimationClip::EventTrack et;
            ok = parseEventTrack(ctx, trackVal, et, err);
            if (ok) {
                maxKeyTime = std::max(maxKeyTime, et.keys.back().time);
                out.events.push_back(std::move(et));
            }
        } else if (type == "property") {
            scene::AnimationClip::PropTrack pt;
            ok = parsePropTrack(ctx, trackVal, pt, err);
            if (ok) {
                maxKeyTime = std::max(maxKeyTime, pt.times.back());
                out.props.push_back(std::move(pt));
            }
        } else {
            ok = false;
            err = "unknown track type '" + type +
                  "' (use 'property' or 'event')";
        }
        JS_FreeValue(ctx, trackVal);
        if (!ok) {
            JS_FreeValue(ctx, tracksVal);
            err = "track " + std::to_string(i) + ": " + err;
            return false;
        }
    }
    JS_FreeValue(ctx, tracksVal);

    // duration defaults to the last key; an explicit shorter duration is
    // allowed (trailing keys are simply unreachable).
    double duration = qjsbind::get_prop_number(ctx, def, "duration", -1.0);
    out.duration = (std::isfinite(duration) && duration >= 0.0)
        ? (float)duration : maxKeyTime;

    JSValue json = JS_JSONStringify(ctx, def, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json) || JS_IsUndefined(json)) {
        JS_FreeValue(ctx, json);
        err = "clipDef must be JSON-serializable (plain data only)";
        return false;
    }
    out.sourceJson = jsStr(ctx, json);
    JS_FreeValue(ctx, json);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Player methods
// ---------------------------------------------------------------------------

// addClip(name, clipDef) — parse + register (replaces an existing name).
JSValue js_clip_addClip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* p = getPlayer(ctx, this_val);
    if (!p) return JS_ThrowTypeError(ctx, "addClip: player has been destroyed");
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "addClip(name, clipDef)");
    auto clip = std::make_shared<scene::AnimationClip>();
    std::string err;
    if (!parseClipDef(ctx, argv[1], *clip, err))
        return JS_ThrowTypeError(ctx, "addClip: %s", err.c_str());
    p->addClip(jsStr(ctx, argv[0]), std::move(clip));
    return JS_DupValue(ctx, this_val);
}

// clipDef(name) — the verbatim clipDef back out (JSON round-trip), or null.
JSValue js_clip_clipDef(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* p = getPlayer(ctx, this_val);
    if (!p) return JS_ThrowTypeError(ctx, "clipDef: player has been destroyed");
    if (argc < 1) return JS_NULL;
    const scene::AnimationClip* clip = p->clip(jsStr(ctx, argv[0]));
    if (!clip) return JS_NULL;
    return JS_ParseJSON(ctx, clip->sourceJson.c_str(),
                        clip->sourceJson.size(), "<clipDef>");
}

// play(name, {speed?, from?, fade?}) — resolve targets + start (see
// clip_player.h for semantics). Failures throw with the engine's reason.
JSValue js_clip_play(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<ClipPlayerWrapper>(ctx, this_val);
    auto* p = w ? w->player() : nullptr;
    if (!p) return JS_ThrowTypeError(ctx, "play: player has been destroyed");
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "play(name, options?)");
    scene::ClipPlayer::PlayOptions opts;
    if (argc > 1 && JS_IsObject(argv[1])) {
        opts.speed = (float)qjsbind::get_prop_number(ctx, argv[1], "speed", opts.speed);
        opts.from  = (float)qjsbind::get_prop_number(ctx, argv[1], "from", opts.from);
        opts.fade  = (float)qjsbind::get_prop_number(ctx, argv[1], "fade", opts.fade);
    }
    std::string name = jsStr(ctx, argv[0]);
    std::string err;
    if (!p->play(name, opts, *w->graph(), err))
        return JS_ThrowTypeError(ctx, "play: %s", err.c_str());
    return JS_DupValue(ctx, this_val);
}

JSValue js_clip_pause(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (auto* p = getPlayer(ctx, this_val)) p->pause();
    return JS_DupValue(ctx, this_val);
}

JSValue js_clip_resume(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (auto* p = getPlayer(ctx, this_val)) p->resume();
    return JS_DupValue(ctx, this_val);
}

JSValue js_clip_stop(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (auto* p = getPlayer(ctx, this_val)) p->stop();
    return JS_DupValue(ctx, this_val);
}

// seek(t) — scrub (writes immediately, works while paused, fires no events).
JSValue js_clip_seek(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<ClipPlayerWrapper>(ctx, this_val);
    auto* p = w ? w->player() : nullptr;
    if (!p) return JS_ThrowTypeError(ctx, "seek: player has been destroyed");
    if (argc < 1 || !JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "seek(seconds)");
    p->seek((float)jsNum(ctx, argv[0]), *w->graph());
    return JS_DupValue(ctx, this_val);
}

JSValue js_clip_destroy(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<ClipPlayerWrapper>(ctx, this_val);
    if (w && w->graph()) w->graph()->destroyClipPlayer(w->id);
    return JS_UNDEFINED;
}

// Wrap a JS function into the engine's (name, argsJson) event callback.
scene::ClipPlayer::EventCallback makeClipEventCallback(JSContext* ctx, JSValueConst fnVal) {
    auto ref = std::make_shared<JSFnRef>(ctx, JS_DupValue(ctx, fnVal));
    return [ref](const std::string& name, const std::string& argsJson) {
        JSContext* c = ref->ctx;
        JSValue fn = JS_DupValue(c, ref->fn);
        JSValue args[2];
        args[0] = JS_NewString(c, name.c_str());
        args[1] = argsJson.empty()
            ? JS_UNDEFINED
            : JS_ParseJSON(c, argsJson.c_str(), argsJson.size(), "<event-args>");
        if (JS_IsException(args[1])) args[1] = JS_UNDEFINED;
        JSValue r = JS_Call(c, fn, JS_UNDEFINED, 2, args);
        if (JS_IsException(r)) Runtime::checkException(c, r);
        JS_FreeValue(c, r);
        JS_FreeValue(c, args[0]);
        JS_FreeValue(c, args[1]);
        JS_FreeValue(c, fn);
    };
}

// createAnimationPlayer() — SceneGraph method.
JSValue js_sg_createAnimationPlayer(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    auto* p = g->createClipPlayer();
    return qjsbind::wrap<ClipPlayerWrapper>(ctx,
        new ClipPlayerWrapper{g->livenessToken(), p->id()});
}

} // namespace bro::js

#endif  // BRO_WITH_3D
