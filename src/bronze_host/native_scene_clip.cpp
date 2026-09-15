// native_scene_clip.cpp — AnimationPlayer and Clip native implementations.

#if BRO_WITH_3D

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "scene/clip_player.h"
#include "scene/scene_graph.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

thread_local std::string tl_errorStr;
thread_local std::string tl_clipDefStr;


static ev::CallResult stringifyJson(Value v) {
    Value jsonVal = ev::globalValue("JSON").value;
    Value stringifyFn = ev::getProperty(jsonVal, "stringify");
    return ev::call(stringifyFn, jsonVal, std::span<const Value>(&v, 1));
}

struct PropInfo {
    const char* name;
    scene::ClipProp prop;
    int stride;
};

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
    for (const auto& p : kClipProps) {
        if (name == p.name) return &p;
    }
    return nullptr;
}

static bool readKeyVec3(Value v, bromath::Vec3& out) {
    if (ev::isNumber(v)) {
        float f = static_cast<float>(ev::toDouble(v));
        out = {f, f, f};
        return true;
    }
    if (!ev::isObject(v)) return false;
    double x = 0, y = 0, z = 0;
    Value e0 = ev::getElement(v, 0);
    Value e1 = ev::getElement(v, 1);
    Value e2 = ev::getElement(v, 2);
    if (!ev::isUndefined(e0)) x = ev::toDouble(e0);
    if (!ev::isUndefined(e1)) y = ev::toDouble(e1);
    if (!ev::isUndefined(e2)) z = ev::toDouble(e2);
    out = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    return true;
}

static bool readKeyQuat(Value v, bromath::Quat& out) {
    if (ev::isNumber(v)) {
        out = bromath::qfromEuler(0.0f, 0.0f, static_cast<float>(ev::toDouble(v)));
        return true;
    }
    if (ev::isObject(v)) {
        Value lenV = ev::getProperty(v, "length");
        if (ev::isNumber(lenV)) {
            double q[4] = {0, 0, 0, 1};
            for (uint32_t i = 0; i < 4; ++i) {
                Value elem = ev::getElement(v, i);
                if (!ev::isUndefined(elem)) q[i] = ev::toDouble(elem);
            }
            out = bromath::qnorm({static_cast<float>(q[0]), static_cast<float>(q[1]),
                                  static_cast<float>(q[2]), static_cast<float>(q[3])});
            return true;
        }
        Value axisVal = ev::getProperty(v, "axis");
        bromath::Vec3 axis;
        if (readKeyVec3(axisVal, axis)) {
            Value angVal = ev::getProperty(v, "angle");
            double angle = ev::isNumber(angVal) ? ev::toDouble(angVal) : 0.0;
            out = bromath::qaxisAngle(axis, static_cast<float>(angle));
            return true;
        }
    }
    return false;
}

static bool readKeyRotation(Value v, bromath::Quat& out) {
    if (ev::isObject(v)) {
        Value e = ev::getProperty(v, "euler");
        if (!ev::isUndefined(e)) {
            bromath::Vec3 r;
            bool ok = readKeyVec3(e, r);
            if (!ok) return false;
            out = bromath::qfromEuler(r);
            return true;
        }
    }
    return readKeyQuat(v, out);
}

static bool readKeyColor(Value v, bromath::Vec3& out) {
    float r = 1, g = 1, b = 1, a = 1;
    if (parseColorValue(v, r, g, b, a)) {
        out = {r, g, b};
        return true;
    }
    return readKeyVec3(v, out);
}

static bool readKeyValue(Value v, const PropInfo& info, float* dst, std::string& err) {
    switch (info.prop) {
        case scene::ClipProp::Rotation: {
            bromath::Quat q;
            if (!readKeyRotation(v, q)) {
                err = "rotation key value must be a quaternion [x,y,z,w], {axis, angle}, {euler: [rx,ry,rz]}, or a number (Z radians)";
                return false;
            }
            dst[0] = q.x; dst[1] = q.y; dst[2] = q.z; dst[3] = q.w;
            return true;
        }
        case scene::ClipProp::Color: {
            bromath::Vec3 c;
            if (!readKeyColor(v, c)) {
                err = "color key value must be [r,g,b] (0-1) or a CSS color string";
                return false;
            }
            dst[0] = c.x; dst[1] = c.y; dst[2] = c.z;
            return true;
        }
        case scene::ClipProp::Position:
        case scene::ClipProp::Scale: {
            bromath::Vec3 p;
            if (!readKeyVec3(v, p)) {
                err = "key value must be [x,y,z] (or a number for a uniform splat)";
                return false;
            }
            dst[0] = p.x; dst[1] = p.y; dst[2] = p.z;
            return true;
        }
        default: {
            if (!ev::isNumber(v)) {
                err = std::string("'") + info.name + "' key value must be a number";
                return false;
            }
            dst[0] = static_cast<float>(ev::toDouble(v));
            if (info.prop == scene::ClipProp::Fov)
                dst[0] *= 3.14159265f / 180.0f;
            return true;
        }
    }
}

static int32_t arrayLength(Value v) {
    Value lenVal = ev::getProperty(v, "length");
    return ev::isNumber(lenVal) ? static_cast<int32_t>(ev::toDouble(lenVal)) : 0;
}

static bool parsePropTrack(Value trackVal, scene::AnimationClip::PropTrack& out, std::string& err) {
    Value targetVal = ev::getProperty(trackVal, "target");
    out.target = ev::isString(targetVal) ? ev::toUtf8(targetVal) : "";
    if (out.target.empty()) {
        err = "property track needs a non-empty 'target' node name";
        return false;
    }
    Value propVal = ev::getProperty(trackVal, "property");
    std::string propName = ev::isString(propVal) ? ev::toUtf8(propVal) : "";
    const PropInfo* info = findClipProp(propName);
    if (!info) {
        err = "unknown property '" + propName + "' (supported: " + kClipPropList + ")";
        return false;
    }
    out.prop = info->prop;
    out.stride = info->stride;

    Value keysVal = ev::getProperty(trackVal, "keys");
    int32_t nKeys = ev::isObject(keysVal) ? arrayLength(keysVal) : 0;
    if (nKeys < 1) {
        err = "track '" + out.target + "." + propName + "' needs a non-empty 'keys' array";
        return false;
    }

    struct Parsed {
        float time;
        float value[4];
        scene::ClipInterp interp;
        scene::Tween::Ease ease;
    };
    std::vector<Parsed> keys(static_cast<size_t>(nKeys));

    for (int32_t i = 0; i < nKeys; ++i) {
        Value keyVal = ev::getElement(keysVal, static_cast<uint32_t>(i));
        Parsed& k = keys[static_cast<size_t>(i)];

        Value timeVal = ev::getProperty(keyVal, "time");
        double time = ev::isNumber(timeVal) ? ev::toDouble(timeVal) : NAN;
        if (!std::isfinite(time) || time < 0.0) {
            err = "key " + std::to_string(i) + " of '" + out.target + "." + propName + "' needs a finite time >= 0";
            return false;
        }
        k.time = static_cast<float>(time);

        Value v = ev::getProperty(keyVal, "value");
        bool ok = readKeyValue(v, *info, k.value, err);
        if (!ok) {
            err = "key " + std::to_string(i) + " of '" + out.target + "." + propName + "': " + err;
            return false;
        }

        Value interpVal = ev::getProperty(keyVal, "interp");
        std::string interp = ev::isString(interpVal) ? ev::toUtf8(interpVal) : "linear";
        if (interp == "linear")      k.interp = scene::ClipInterp::Linear;
        else if (interp == "step")   k.interp = scene::ClipInterp::Step;
        else if (interp == "cubic")  k.interp = scene::ClipInterp::Cubic;
        else {
            err = "unknown interp '" + interp + "' (use 'linear', 'step', or 'cubic')";
            return false;
        }

        Value easeVal = ev::getProperty(keyVal, "ease");
        std::string ease = ev::isString(easeVal) ? ev::toUtf8(easeVal) : "linear";
        k.ease = scene::Tween::Ease::Linear;
        if (!scene::Tween::easeFromString(ease, k.ease)) {
            err = "unknown ease '" + ease + "'";
            return false;
        }
    }

    std::stable_sort(keys.begin(), keys.end(), [](const Parsed& a, const Parsed& b) {
        return a.time < b.time;
    });

    out.times.reserve(keys.size());
    out.values.reserve(keys.size() * static_cast<size_t>(out.stride));
    out.interps.reserve(keys.size());
    out.eases.reserve(keys.size());
    for (const Parsed& k : keys) {
        out.times.push_back(k.time);
        for (int c = 0; c < out.stride; ++c) out.values.push_back(k.value[c]);
        out.interps.push_back(k.interp);
        out.eases.push_back(k.ease);
    }
    return true;
}

static bool parseEventTrack(Value trackVal, scene::AnimationClip::EventTrack& out, std::string& err) {
    Value keysVal = ev::getProperty(trackVal, "keys");
    int32_t nKeys = ev::isObject(keysVal) ? arrayLength(keysVal) : 0;
    if (nKeys < 1) {
        err = "event track needs a non-empty 'keys' array";
        return false;
    }
    out.keys.resize(static_cast<size_t>(nKeys));
    for (int32_t i = 0; i < nKeys; ++i) {
        Value keyVal = ev::getElement(keysVal, static_cast<uint32_t>(i));
        auto& k = out.keys[static_cast<size_t>(i)];

        Value timeVal = ev::getProperty(keyVal, "time");
        double time = ev::isNumber(timeVal) ? ev::toDouble(timeVal) : NAN;
        Value nameVal = ev::getProperty(keyVal, "name");
        k.name = ev::isString(nameVal) ? ev::toUtf8(nameVal) : "";
        if (!std::isfinite(time) || time < 0.0 || k.name.empty()) {
            err = "event key " + std::to_string(i) + " needs a finite time >= 0 and a non-empty 'name'";
            return false;
        }
        k.time = static_cast<float>(time);

        Value args = ev::getProperty(keyVal, "args");
        if (!ev::isUndefined(args) && !ev::isNull(args)) {
            ev::CallResult jsonRes = stringifyJson(args);
            if (jsonRes.thrown || !ev::isString(jsonRes.value)) {
                err = "event key " + std::to_string(i) + ": args must be JSON-serializable";
                return false;
            }
            k.argsJson = ev::toUtf8(jsonRes.value);
        }
    }

    std::stable_sort(out.keys.begin(), out.keys.end(), [](const scene::AnimationClip::EventKey& a,
                                                         const scene::AnimationClip::EventKey& b) {
        return a.time < b.time;
    });
    return true;
}

static bool parseClipDef(Value def, scene::AnimationClip& out, std::string& err) {
    if (!ev::isObject(def)) {
        err = "clipDef must be an object";
        return false;
    }

    Value loopVal = ev::getProperty(def, "loop");
    std::string loop = ev::isString(loopVal) ? ev::toUtf8(loopVal) : "none";
    if (loop == "none")          out.loop = scene::AnimationClip::Loop::None;
    else if (loop == "loop")     out.loop = scene::AnimationClip::Loop::Loop;
    else if (loop == "pingpong") out.loop = scene::AnimationClip::Loop::PingPong;
    else {
        err = "unknown loop mode '" + loop + "' (use 'none', 'loop', or 'pingpong')";
        return false;
    }

    Value tracksVal = ev::getProperty(def, "tracks");
    int32_t nTracks = ev::isObject(tracksVal) ? arrayLength(tracksVal) : 0;
    if (nTracks < 1) {
        err = "clipDef needs a non-empty 'tracks' array";
        return false;
    }
    float maxKeyTime = 0.0f;
    for (int32_t i = 0; i < nTracks; ++i) {
        Value trackVal = ev::getElement(tracksVal, static_cast<uint32_t>(i));
        Value typeVal = ev::getProperty(trackVal, "type");
        std::string type = ev::isString(typeVal) ? ev::toUtf8(typeVal) : "property";
        bool ok = false;
        if (type == "event") {
            scene::AnimationClip::EventTrack et;
            ok = parseEventTrack(trackVal, et, err);
            if (ok) {
                if (!et.keys.empty()) maxKeyTime = std::max(maxKeyTime, et.keys.back().time);
                out.events.push_back(std::move(et));
            }
        } else if (type == "property") {
            scene::AnimationClip::PropTrack pt;
            ok = parsePropTrack(trackVal, pt, err);
            if (ok) {
                if (!pt.times.empty()) maxKeyTime = std::max(maxKeyTime, pt.times.back());
                out.props.push_back(std::move(pt));
            }
        } else {
            err = "unknown track type '" + type + "' (use 'property' or 'event')";
        }
        if (!ok) {
            err = "track " + std::to_string(i) + ": " + err;
            return false;
        }
    }

    Value durVal = ev::getProperty(def, "duration");
    if (ev::isNumber(durVal)) {
        double d = ev::toDouble(durVal);
        if (!std::isfinite(d) || d < 0.0) {
            err = "duration must be a finite number >= 0";
            return false;
        }
        out.duration = static_cast<float>(d);
    } else {
        out.duration = maxKeyTime;
    }
    return true;
}

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

// =============================================================================
// AnimationPlayer lifecycle & standard methods
// =============================================================================

void bro_animation_AnimationPlayer_dtor(void* self) {
    delete clipPlayerCellOf(self);
}

void* bro_animation_AnimationPlayer_ctor(void) {
    return new HostClipPlayerCell();
}

void bro_animation_AnimationPlayer_pause(void* self) {
    auto* c = clipPlayerCellOf(self);
    if (c && c->player()) c->player()->pause();
}

void bro_animation_AnimationPlayer_resume(void* self) {
    auto* c = clipPlayerCellOf(self);
    if (c && c->player()) c->player()->resume();
}

void bro_animation_AnimationPlayer_stop(void* self) {
    auto* c = clipPlayerCellOf(self);
    if (c && c->player()) c->player()->stop();
}

void bro_animation_AnimationPlayer_seek(void* self, double time) {
    auto* c = clipPlayerCellOf(self);
    if (c && c->player() && c->graph()) c->player()->seek(static_cast<float>(time), *c->graph());
}

void bro_animation_AnimationPlayer_destroy(void* self) {
    auto* c = clipPlayerCellOf(self);
    if (c && c->graph()) {
        c->graph()->destroyClipPlayer(c->id);
    }
}

// =============================================================================
// AnimationPlayer extended methods
// =============================================================================

const char* bro_animation_AnimationPlayer_addClip(void* self, const char* name, uint64_t clipBits) {
    auto* c = clipPlayerCellOf(self);
    if (!c || !c->player()) return "player destroyed";
    if (!name || name[0] == '\0') return "name required";
    Value clipVal = ev::fromBits(clipBits);
    scene::AnimationClip clip;
    std::string err;
    if (!parseClipDef(clipVal, clip, err)) {
        tl_errorStr = err;
        return tl_errorStr.c_str();
    }
    auto jsonRes = stringifyJson(clipVal);
    if (!jsonRes.thrown && ev::isString(jsonRes.value)) {
        clip.sourceJson = ev::toUtf8(jsonRes.value);
    }
    c->player()->addClip(name, std::make_shared<scene::AnimationClip>(std::move(clip)));
    return nullptr;
}

const char* bro_animation_AnimationPlayer_clipDef(void* self, const char* name) {
    auto* c = clipPlayerCellOf(self);
    if (!c || !c->player() || !name) return nullptr;
    const auto* cl = c->player()->clip(name);
    return cl ? cl->sourceJson.c_str() : nullptr;
}

const char* bro_animation_AnimationPlayer_play(void* self, const char* clipName, uint64_t optsBits) {
    auto* c = clipPlayerCellOf(self);
    if (!c || !c->player() || !c->graph()) return "player destroyed";
    if (!clipName || clipName[0] == '\0') return "clipName required";
    scene::ClipPlayer::PlayOptions opts;
    Value optsVal = ev::fromBits(optsBits);
    if (ev::isObject(optsVal)) {
        Value spd = ev::getProperty(optsVal, "speed");
        if (ev::isNumber(spd)) opts.speed = static_cast<float>(ev::toDouble(spd));
        Value from = ev::getProperty(optsVal, "from");
        if (ev::isNumber(from)) opts.from = static_cast<float>(ev::toDouble(from));
        Value fade = ev::getProperty(optsVal, "fade");
        if (ev::isNumber(fade)) opts.fade = static_cast<float>(ev::toDouble(fade));
    }
    std::string err;
    if (!c->player()->play(clipName, opts, *c->graph(), err)) {
        tl_errorStr = err;
        return tl_errorStr.c_str();
    }
    return nullptr;
}

bool bro_animation_AnimationPlayer_playing(void* self) {
    auto* c = clipPlayerCellOf(self);
    return c && c->player() && c->player()->playing();
}

const char* bro_animation_AnimationPlayer_currentClip(void* self) {
    auto* c = clipPlayerCellOf(self);
    return (c && c->player()) ? c->player()->currentClip().c_str() : "";
}

double bro_animation_AnimationPlayer_currentTime(void* self) {
    auto* c = clipPlayerCellOf(self);
    return (c && c->player()) ? static_cast<double>(c->player()->currentTime()) : 0.0;
}

double bro_animation_AnimationPlayer_speed_get(void* self) {
    auto* c = clipPlayerCellOf(self);
    return (c && c->player()) ? static_cast<double>(c->player()->speed()) : 1.0;
}

void bro_animation_AnimationPlayer_speed_set(void* self, double v) {
    auto* c = clipPlayerCellOf(self);
    if (c && c->player()) c->player()->setSpeed(static_cast<float>(v));
}

void bro_animation_AnimationPlayer_setOnFinished(void* self, uint64_t cbBits) {
    auto* c = clipPlayerCellOf(self);
    if (!c || !c->player()) return;
    Value cb = ev::fromBits(cbBits);
    if (ev::isFunction(cb)) {
        auto fnRef = std::make_shared<ev::Persistent>(cb);
        c->player()->setOnFinished([fnRef]() {
            if (fnRef && ev::isFunction(fnRef->get())) {
                ev::call(fnRef->get(), ev::undefined(), {});
            }
        });
    } else {
        c->player()->setOnFinished(nullptr);
    }
}

void bro_animation_AnimationPlayer_setOnEvent(void* self, uint64_t cbBits) {
    auto* c = clipPlayerCellOf(self);
    if (!c || !c->player()) return;
    Value cb = ev::fromBits(cbBits);
    if (ev::isFunction(cb)) {
        auto fnRef = std::make_shared<ev::Persistent>(cb);
        c->player()->setOnEvent([fnRef](const std::string& name, const std::string& argsJson) {
            if (fnRef && ev::isFunction(fnRef->get())) {
                Value nameVal = ev::fromUtf8(name);
                Value argsVal = ev::undefined();
                if (!argsJson.empty()) {
                    Value jsonVal = ev::globalValue("JSON").value;
                    Value parseFn = ev::getProperty(jsonVal, "parse");
                    Value sVal = ev::fromUtf8(argsJson);
                    auto r = ev::call(parseFn, jsonVal, std::span<const Value>(&sVal, 1));
                    if (!r.thrown) argsVal = r.value;
                }
                const Value argv[2] = {nameVal, argsVal};
                ev::call(fnRef->get(), ev::undefined(), std::span<const Value>(argv, 2));
            }
        });
    } else {
        c->player()->setOnEvent(nullptr);
    }
}

}  // extern "C"

namespace bro::bronze_host {

bool registerClipPlayerNatives(std::string* error) {
    using namespace natives;
    return fn("__bro_native.animation.AnimationPlayer_addClip", (void*)&bro_animation_AnimationPlayer_addClip, "str", {"__bro_native.animation.AnimationPlayer", "str", "dynamic"}, error) &&
           fn("__bro_native.animation.AnimationPlayer_clipDef", (void*)&bro_animation_AnimationPlayer_clipDef, "str", {"__bro_native.animation.AnimationPlayer", "str"}, error) &&
           fn("__bro_native.animation.AnimationPlayer_play", (void*)&bro_animation_AnimationPlayer_play, "str", {"__bro_native.animation.AnimationPlayer", "str", "dynamic"}, error) &&
           fn("__bro_native.animation.AnimationPlayer_playing", (void*)&bro_animation_AnimationPlayer_playing, "bool", {"__bro_native.animation.AnimationPlayer"}, error) &&
           fn("__bro_native.animation.AnimationPlayer_currentClip", (void*)&bro_animation_AnimationPlayer_currentClip, "str", {"__bro_native.animation.AnimationPlayer"}, error) &&
           fn("__bro_native.animation.AnimationPlayer_currentTime", (void*)&bro_animation_AnimationPlayer_currentTime, "f64", {"__bro_native.animation.AnimationPlayer"}, error) &&
           fn("__bro_native.animation.AnimationPlayer_speed_get", (void*)&bro_animation_AnimationPlayer_speed_get, "f64", {"__bro_native.animation.AnimationPlayer"}, error) &&
           fn("__bro_native.animation.AnimationPlayer_speed_set", (void*)&bro_animation_AnimationPlayer_speed_set, "void", {"__bro_native.animation.AnimationPlayer", "f64"}, error) &&
           fn("__bro_native.animation.AnimationPlayer_setOnFinished", (void*)&bro_animation_AnimationPlayer_setOnFinished, "void", {"__bro_native.animation.AnimationPlayer", "dynamic"}, error) &&
           fn("__bro_native.animation.AnimationPlayer_setOnEvent", (void*)&bro_animation_AnimationPlayer_setOnEvent, "void", {"__bro_native.animation.AnimationPlayer", "dynamic"}, error);
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
