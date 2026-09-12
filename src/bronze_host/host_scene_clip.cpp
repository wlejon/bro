#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/clip_player.h"
#include "scene/scene_graph.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

HostClass g_clipPlayerClass;

namespace {

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
    if (ev::isString(v)) {
        float r = 1, g = 1, b = 1, a = 1;
        if (!parseColorValue(v, r, g, b, a)) return false;
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
    double duration = ev::isNumber(durVal) ? ev::toDouble(durVal) : -1.0;
    out.duration = (std::isfinite(duration) && duration >= 0.0)
        ? static_cast<float>(duration) : maxKeyTime;

    ev::CallResult jsonRes = stringifyJson(def);
    if (jsonRes.thrown || !ev::isString(jsonRes.value)) {
        err = "clipDef must be JSON-serializable (plain data only)";
        return false;
    }
    out.sourceJson = ev::toUtf8(jsonRes.value);
    return true;
}

static bool s_clipPlayerClassInstalled = false;

} // namespace

void ensureClipPlayerClassInstalled() {
    if (s_clipPlayerClassInstalled) return;
    s_clipPlayerClassInstalled = true;

    g_clipPlayerClass.install("AnimationPlayer", 0, nullptr, [](ObjectBuilder& b) {
        b.accessor("playing", [](Value self_, std::span<const Value>) {
            auto* p = clipPlayerOf(self_);
            return ev::fromBool(p && p->playing());
        }, nullptr);

        b.accessor("currentTime", [](Value self_, std::span<const Value>) {
            auto* p = clipPlayerOf(self_);
            return ev::fromDouble(p ? p->currentTime() : 0.0);
        }, nullptr);

        b.accessor("currentClip", [](Value self_, std::span<const Value>) {
            auto* p = clipPlayerOf(self_);
            return ev::fromUtf8(p ? p->currentClip() : "");
        }, nullptr);

        b.accessor("speed",
            [](Value self_, std::span<const Value>) {
                auto* p = clipPlayerOf(self_);
                return ev::fromDouble(p ? p->speed() : 1.0);
            },
            [](Value self_, std::span<const Value> a) {
                auto* p = clipPlayerOf(self_);
                if (p && !a.empty() && ev::isNumber(a[0])) {
                    p->setSpeed(static_cast<float>(ev::toDouble(a[0])));
                }
                return ev::undefined();
            });

        b.accessor("onFinished",
            [](Value, std::span<const Value>) { return ev::undefined(); },
            [](Value self_, std::span<const Value> a) {
                auto* p = clipPlayerOf(self_);
                if (!p) return ev::undefined();
                if (!a.empty() && ev::isFunction(a[0])) {
                    auto fnRef = std::make_shared<ev::Persistent>(a[0]);
                    p->setOnFinished([fnRef]() {
                        if (fnRef && ev::isFunction(fnRef->get())) {
                            ev::call(fnRef->get(), ev::undefined(), {});
                        }
                    });
                } else {
                    p->setOnFinished(nullptr);
                }
                return ev::undefined();
            });

        b.accessor("onEvent",
            [](Value, std::span<const Value>) { return ev::undefined(); },
            [](Value self_, std::span<const Value> a) {
                auto* p = clipPlayerOf(self_);
                if (!p) return ev::undefined();
                if (!a.empty() && ev::isFunction(a[0])) {
                    auto fnRef = std::make_shared<ev::Persistent>(a[0]);
                    p->setOnEvent([fnRef](const std::string& name, const std::string& argsJson) {
                        if (fnRef && ev::isFunction(fnRef->get())) {
                            Value args[2];
                            args[0] = ev::fromUtf8(name);
                            if (!argsJson.empty()) {
                                auto parsed = ev::parseJson(argsJson);
                                args[1] = parsed.thrown ? ev::undefined() : parsed.value;
                            } else {
                                args[1] = ev::undefined();
                            }
                            ev::call(fnRef->get(), ev::undefined(), std::span<const Value>(args, 2));
                        }
                    });
                } else {
                    p->setOnEvent(nullptr);
                }
                return ev::undefined();
            });

        b.def("addClip", 2, [](Value self_, std::span<const Value> a) {
            auto* p = clipPlayerOf(self_);
            if (!p) return ev::throwTypeError("addClip: player has been destroyed");
            if (a.size() < 2 || !ev::isString(a[0]))
                return ev::throwTypeError("addClip(name, clipDef)");
            auto clip = std::make_shared<scene::AnimationClip>();
            std::string err;
            if (!parseClipDef(a[1], *clip, err))
                return ev::throwTypeError("addClip: " + err);
            p->addClip(ev::toUtf8(a[0]), std::move(clip));
            return self_;
        });

        b.def("clipDef", 1, [](Value self_, std::span<const Value> a) {
            auto* p = clipPlayerOf(self_);
            if (!p) return ev::throwTypeError("clipDef: player has been destroyed");
            if (a.empty() || !ev::isString(a[0])) return ev::null();
            const scene::AnimationClip* clip = p->clip(ev::toUtf8(a[0]));
            if (!clip) return ev::null();
            auto parsed = ev::parseJson(clip->sourceJson);
            return parsed.thrown ? ev::null() : parsed.value;
        });

        b.def("play", 1, [](Value self_, std::span<const Value> a) {
            auto* c = clipPlayerCellOf(self_);
            auto* p = c ? c->player() : nullptr;
            if (!p) return ev::throwTypeError("play: player has been destroyed");
            if (a.empty() || !ev::isString(a[0]))
                return ev::throwTypeError("play(name, options?)");
            scene::ClipPlayer::PlayOptions opts;
            if (a.size() > 1 && ev::isObject(a[1])) {
                Value spdVal = ev::getProperty(a[1], "speed");
                if (ev::isNumber(spdVal)) opts.speed = static_cast<float>(ev::toDouble(spdVal));
                Value fromVal = ev::getProperty(a[1], "from");
                if (ev::isNumber(fromVal)) opts.from = static_cast<float>(ev::toDouble(fromVal));
                Value fadeVal = ev::getProperty(a[1], "fade");
                if (ev::isNumber(fadeVal)) opts.fade = static_cast<float>(ev::toDouble(fadeVal));
            }
            std::string name = ev::toUtf8(a[0]);
            std::string err;
            if (!p->play(name, opts, *c->graph(), err))
                return ev::throwTypeError("play: " + err);
            return self_;
        });

        b.def("pause", 0, [](Value self_, std::span<const Value>) {
            if (auto* p = clipPlayerOf(self_)) p->pause();
            return self_;
        });

        b.def("resume", 0, [](Value self_, std::span<const Value>) {
            if (auto* p = clipPlayerOf(self_)) p->resume();
            return self_;
        });

        b.def("stop", 0, [](Value self_, std::span<const Value>) {
            if (auto* p = clipPlayerOf(self_)) p->stop();
            return self_;
        });

        b.def("seek", 1, [](Value self_, std::span<const Value> a) {
            auto* c = clipPlayerCellOf(self_);
            auto* p = c ? c->player() : nullptr;
            if (!p) return ev::throwTypeError("seek: player has been destroyed");
            if (a.empty() || !ev::isNumber(a[0]))
                return ev::throwTypeError("seek(seconds)");
            p->seek(static_cast<float>(ev::toDouble(a[0])), *c->graph());
            return self_;
        });

        b.def("destroy", 0, [](Value self_, std::span<const Value>) {
            auto* c = clipPlayerCellOf(self_);
            if (c && c->graph()) c->graph()->destroyClipPlayer(c->id);
            return ev::undefined();
        });
    });
}

Value wrapClipPlayer(scene::ClipPlayer* player, scene::SceneGraph* graph) {
    if (!player || !graph) return ev::null();
    auto tok = graph->livenessToken();
    if (!tok) return ev::null();
    ensureClipPlayerClassInstalled();
    auto* cell = new HostClipPlayerCell{kHostClipPlayerTag, tok, player->id()};
    return g_clipPlayerClass.make(cell, [](void* p) {
        delete static_cast<HostClipPlayerCell*>(p);
    });
}

} // namespace bro::bronze_host

#endif // BRO_WITH_3D
