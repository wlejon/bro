// Scene JS bindings — animation surface: the skinned-mesh animation player
// (setSkeleton / addClip / bone queries, unified play/stop/pause/resume that
// also drives sprites and particles) and the chainable Tween returned by
// scene.createTween(). Shared wrapper structs + helpers live in
// scene_bindings_internal.h.

#include "js/scene_bindings.h"
#if BRO_WITH_3D  // modular-build feature gate
#include "js/scene_bindings_internal.h"
#include "js/rigging_bindings.h"
#include "js/runtime.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/sprite_node.h"
#include "scene/particle_node.h"
#include "scene/particles3d_node.h"
#include "scene/mesh_node.h"
#include "scene/skinned_mesh_node.h"
#include "scene/animation_player.h"
#include "scene/tween.h"

#include <qjsbind/qjsbind.h>

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bro::js {

// ---------------------------------------------------------------------------
// Skinned-mesh animation player helpers
// ---------------------------------------------------------------------------

// Read a bone mask: Uint8Array or plain JS array of 0/1 (any nonzero = 1).
static bool readBoneMask(JSContext* ctx, JSValueConst v, std::vector<uint8_t>& out) {
    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &offset, &byteLen, &bpe);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        JS_FreeValue(ctx, abuf);
        if (raw && bpe == 1) {
            out.assign(raw + offset, raw + offset + byteLen);
            return true;
        }
        return false;
    }
    JS_FreeValue(ctx, abuf);
    if (!JS_IsArray(v)) return false;
    JSValue lenVal = JS_GetPropertyStr(ctx, v, "length");
    int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
    out.resize((size_t)len);
    for (int32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        double d = 0; JS_ToFloat64(ctx, &d, e);
        JS_FreeValue(ctx, e);
        out[(size_t)i] = d != 0.0 ? 1 : 0;
    }
    return true;
}

// setSkeleton(Skeleton) — copy the rigging Skeleton into the node's animation
// player (shared_ptr copy: the player never dangles when JS GC's the wrapper).
JSValue js_node_setSkeleton(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "setSkeleton: node is not a skinned mesh");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setSkeleton: missing Skeleton");
    bromesh::Skeleton* skel = RiggingBindings::getSkeleton(ctx, argv[0]);
    if (!skel)
        return JS_ThrowTypeError(ctx, "setSkeleton: argument must be a Skeleton");
    sm->ensurePlayer().setSkeleton(std::make_shared<bromesh::Skeleton>(*skel));
    return JS_DupValue(ctx, this_val);
}

// addClip(name, Animation) — register a clip (shared_ptr copy) for play().
JSValue js_node_addClip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "addClip: node is not a skinned mesh");
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "addClip(name, animation)");
    bromesh::Animation* anim = RiggingBindings::getAnimation(ctx, argv[1]);
    if (!anim)
        return JS_ThrowTypeError(ctx, "addClip: second argument must be an Animation");
    sm->ensurePlayer().addClip(jsStr(ctx, argv[0]),
                               std::make_shared<bromesh::Animation>(*anim));
    return JS_DupValue(ctx, this_val);
}

// getBoneWorldMatrix(nameOrIndex) — current posed bone matrix in MODEL space
// (before the node's own TRS), Float32Array(16) column-major, or null.
JSValue js_node_getBoneWorldMatrix(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "getBoneWorldMatrix: node is not a skinned mesh");
    auto* player = sm->player();
    if (!player || argc < 1) return JS_NULL;
    float m[16];
    bool ok = JS_IsString(argv[0])
        ? player->boneWorldMatrix(jsStr(ctx, argv[0]), m)
        : player->boneWorldMatrix((int)jsNum(ctx, argv[0]), m);
    if (!ok) return JS_NULL;
    return qjsbind::make_float32_array(ctx, m, 16);
}

// Unified play() for SpriteNode, ParticleNode, and SkinnedMeshNode.
// Sprite: play(name?) — resume without a name. Particles: play().
// Skinned mesh: play(clipName, {loop, speed, fadeTime, weight, mask}).
JSValue js_node_play(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node()) return JS_UNDEFINED;
    if (w->node()->type() == scene::SceneNode::Type::Sprite) {
        auto* s = static_cast<scene::SpriteNode*>(w->node());
        if (argc > 0 && JS_IsString(argv[0])) s->play(jsStr(ctx, argv[0]));
        else                                  s->resume();
    } else if (w->node()->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(w->node())->play();
    } else if (w->node()->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(w->node())->play();
    } else if (auto* sm = asSkinnedMesh(w)) {
        if (argc > 0 && JS_IsString(argv[0])) {
            auto& player = sm->ensurePlayer();
            scene::AnimationPlayer::PlayOptions opts;
            if (argc > 1 && JS_IsObject(argv[1])) {
                opts.loop     = qjsbind::get_prop_bool(ctx, argv[1], "loop", true);
                opts.speed    = (float)qjsbind::get_prop_number(ctx, argv[1], "speed", 1.0);
                opts.fadeTime = (float)qjsbind::get_prop_number(ctx, argv[1], "fadeTime", 0.0);
                opts.weight   = (float)qjsbind::get_prop_number(ctx, argv[1], "weight", 1.0);
                JSValue maskVal = JS_GetPropertyStr(ctx, argv[1], "mask");
                if (!JS_IsUndefined(maskVal) && !JS_IsNull(maskVal))
                    readBoneMask(ctx, maskVal, opts.mask);
                JS_FreeValue(ctx, maskVal);
            }
            std::string name = jsStr(ctx, argv[0]);
            if (!player.play(name, opts))
                return JS_ThrowTypeError(ctx,
                    "play: unknown clip '%s' (addClip first) or no skeleton "
                    "(setSkeleton first)", name.c_str());
        } else if (sm->player()) {
            sm->player()->resume();
        }
    }
    return JS_DupValue(ctx, this_val);
}

// stop() — sprite/particles; skinned mesh: stop({fadeTime}) fades to bind
// pose and deactivates the player (manual setSkinningMatrices works again).
JSValue js_node_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node()) return JS_UNDEFINED;
    if (w->node()->type() == scene::SceneNode::Type::Sprite) {
        static_cast<scene::SpriteNode*>(w->node())->stop();
    } else if (w->node()->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(w->node())->stop();
    } else if (w->node()->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(w->node())->stop();
    } else if (auto* sm = asSkinnedMesh(w)) {
        if (auto* player = sm->player()) {
            float fade = 0.0f;
            if (argc > 0 && JS_IsObject(argv[0]))
                fade = (float)qjsbind::get_prop_number(ctx, argv[0], "fadeTime", 0.0);
            player->stop(fade);
        }
    }
    return JS_DupValue(ctx, this_val);
}

// pause()/resume() — freeze / unfreeze playback in place (sprite freezes the
// frame index; the skinned-mesh player holds the current pose).
JSValue js_node_pause(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node()) return JS_UNDEFINED;
    if (w->node()->type() == scene::SceneNode::Type::Sprite) {
        static_cast<scene::SpriteNode*>(w->node())->stop();
    } else if (auto* sm = asSkinnedMesh(w)) {
        if (auto* player = sm->player()) player->pause();
    }
    return JS_DupValue(ctx, this_val);
}

JSValue js_node_resume(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node()) return JS_UNDEFINED;
    if (w->node()->type() == scene::SceneNode::Type::Sprite) {
        static_cast<scene::SpriteNode*>(w->node())->resume();
    } else if (auto* sm = asSkinnedMesh(w)) {
        if (auto* player = sm->player()) player->resume();
    }
    return JS_DupValue(ctx, this_val);
}

// ---------------------------------------------------------------------------
// Tween bindings — scene.createTween() returns a chainable Tween. The C++
// Tween is owned by the SceneGraph and referenced by id, so a destroyed
// tween (or torn-down graph) can never dangle through a live JS wrapper.
// ---------------------------------------------------------------------------

static scene::Tween* getTween(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<TweenWrapper>(ctx, val);
    return (w && w->graph()) ? w->graph()->findTween(w->id) : nullptr;
}

// Wrap a JS function into a void() callback (tween.call / onFinished).
std::function<void()> makeVoidCallback(JSContext* ctx, JSValueConst fnVal) {
    auto ref = std::make_shared<JSFnRef>(ctx, JS_DupValue(ctx, fnVal));
    return [ref]() {
        JSValue fn = JS_DupValue(ref->ctx, ref->fn);
        JSValue r = JS_Call(ref->ctx, fn, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(r)) Runtime::checkException(ref->ctx, r);
        JS_FreeValue(ref->ctx, r);
        JS_FreeValue(ref->ctx, fn);
    };
}

// number → {f,f,f}, [x,y,z] → Vec3. Returns false for anything else.
static bool readTweenVec3(JSContext* ctx, JSValueConst v, bromath::Vec3& out) {
    if (JS_IsNumber(v)) {
        float f = (float)jsNum(ctx, v);
        out = {f, f, f};
        return true;
    }
    if (!JS_IsArray(v)) return false;
    double x = 0, y = 0, z = 0;
    JSValue e0 = JS_GetPropertyUint32(ctx, v, 0);
    JSValue e1 = JS_GetPropertyUint32(ctx, v, 1);
    JSValue e2 = JS_GetPropertyUint32(ctx, v, 2);
    if (!JS_IsUndefined(e0)) JS_ToFloat64(ctx, &x, e0);
    if (!JS_IsUndefined(e1)) JS_ToFloat64(ctx, &y, e1);
    if (!JS_IsUndefined(e2)) JS_ToFloat64(ctx, &z, e2);
    JS_FreeValue(ctx, e0); JS_FreeValue(ctx, e1); JS_FreeValue(ctx, e2);
    out = {(float)x, (float)y, (float)z};
    return true;
}

// number → Rz(angle); [x,y,z,w] → quat; {axis:[..], angle} → axis-angle.
static bool readTweenQuat(JSContext* ctx, JSValueConst v, bromath::Quat& out) {
    if (JS_IsNumber(v)) {
        out = bromath::qfromEuler(0.0f, 0.0f, (float)jsNum(ctx, v));
        return true;
    }
    if (JS_IsArray(v)) {
        double q[4] = {0, 0, 0, 1};
        for (uint32_t i = 0; i < 4; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            if (!JS_IsUndefined(e)) JS_ToFloat64(ctx, &q[i], e);
            JS_FreeValue(ctx, e);
        }
        out = bromath::qnorm({(float)q[0], (float)q[1], (float)q[2], (float)q[3]});
        return true;
    }
    if (JS_IsObject(v)) {
        JSValue axisVal = JS_GetPropertyStr(ctx, v, "axis");
        bromath::Vec3 axis;
        bool hasAxis = readTweenVec3(ctx, axisVal, axis);
        JS_FreeValue(ctx, axisVal);
        if (!hasAxis) return false;
        double angle = qjsbind::get_prop_number(ctx, v, "angle", 0.0);
        out = bromath::qaxisAngle(axis, (float)angle);
        return true;
    }
    return false;
}

// [r,g,b] in 0..1 or a CSS color string.
static bool readTweenColor(JSContext* ctx, JSValueConst v, bromath::Vec3& out) {
    if (JS_IsString(v)) {
        uint8_t r, g, b, a;
        if (!parseColor(jsStr(ctx, v), r, g, b, a)) return false;
        out = {r / 255.0f, g / 255.0f, b / 255.0f};
        return true;
    }
    return readTweenVec3(ctx, v, out);
}

// Parse the props object of to() into anims sharing duration/delay/ease.
static void parseTweenProps(JSContext* ctx, JSValueConst props, uint32_t nodeId,
                            float duration, float delay, scene::Tween::Ease ease,
                            std::vector<scene::Tween::Anim>& out) {
    using Anim = scene::Tween::Anim;
    using Prop = scene::Tween::Prop;
    auto base = [&](Prop p) {
        Anim a;
        a.nodeId = nodeId;
        a.prop = p;
        a.duration = duration;
        a.delay = delay;
        a.ease = ease;
        return a;
    };

    JSValue v = JS_GetPropertyStr(ctx, props, "position");
    if (!JS_IsUndefined(v)) {
        Anim a = base(Prop::Position);
        if (readTweenVec3(ctx, v, a.v3To)) out.push_back(std::move(a));
    }
    JS_FreeValue(ctx, v);

    // Two spellings, both landing on the quaternion slot: `rotation` (number
    // = Z radians / quat array / axis-angle) matches node.rotation's 2D
    // convenience, `quaternion` matches the node.quaternion prop.
    for (const char* key : {"rotation", "quaternion"}) {
        v = JS_GetPropertyStr(ctx, props, key);
        if (!JS_IsUndefined(v)) {
            Anim a = base(Prop::Quaternion);
            if (readTweenQuat(ctx, v, a.qTo)) out.push_back(std::move(a));
        }
        JS_FreeValue(ctx, v);
    }

    v = JS_GetPropertyStr(ctx, props, "scale");
    if (!JS_IsUndefined(v)) {
        Anim a = base(Prop::Scale);
        if (readTweenVec3(ctx, v, a.v3To)) out.push_back(std::move(a));
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, props, "opacity");
    if (JS_IsNumber(v)) {
        Anim a = base(Prop::Opacity);
        a.fTo = (float)jsNum(ctx, v);
        out.push_back(std::move(a));
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, props, "color");
    if (!JS_IsUndefined(v)) {
        Anim a = base(Prop::Color);
        if (readTweenColor(ctx, v, a.v3To)) out.push_back(std::move(a));
    }
    JS_FreeValue(ctx, v);
}

// to(node|null, props, duration[, opts]) — append a step (or join the
// previous one after parallel()).
JSValue js_tween_to(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* tw = getTween(ctx, this_val);
    if (!tw) return JS_ThrowTypeError(ctx, "to: tween has been destroyed");
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "to(node, props, duration[, opts]) requires 3 arguments");

    uint32_t nodeId = 0;
    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        auto* nw = qjsbind::unwrap<NodeWrapper>(ctx, argv[0]);
        if (!nw || !nw->node())
            return JS_ThrowTypeError(ctx, "to: first argument must be a SceneNode or null");
        nodeId = nw->node()->id();
    }
    if (!JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "to: props must be an object");
    float duration = (float)jsNum(ctx, argv[2]);
    if (!(duration >= 0.0f)) duration = 0.0f;   // NaN/negative → instant

    float delay = 0.0f;
    auto ease = scene::Tween::Ease::Linear;
    JSValue onUpdateVal = JS_UNDEFINED;
    if (argc > 3 && JS_IsObject(argv[3])) {
        std::string easing = qjsbind::get_prop_string(ctx, argv[3], "easing", "linear");
        if (!scene::Tween::easeFromString(easing, ease))
            return JS_ThrowTypeError(ctx, "to: unknown easing '%s'", easing.c_str());
        delay = (float)qjsbind::get_prop_number(ctx, argv[3], "delay", 0.0);
        onUpdateVal = JS_GetPropertyStr(ctx, argv[3], "onUpdate");
    }

    std::vector<scene::Tween::Anim> anims;
    parseTweenProps(ctx, argv[1], nodeId, duration, delay, ease, anims);

    if (JS_IsFunction(ctx, onUpdateVal)) {
        // Callback anim: onUpdate(easedT) every tick — tweens anything the
        // property set doesn't cover (camera, material params, ...).
        scene::Tween::Anim a;
        a.prop = scene::Tween::Prop::Custom;
        a.duration = duration;
        a.delay = delay;
        a.ease = ease;
        auto ref = std::make_shared<JSFnRef>(ctx, JS_DupValue(ctx, onUpdateVal));
        a.onUpdate = [ref](float t) {
            JSValue fn = JS_DupValue(ref->ctx, ref->fn);
            JSValue arg = JS_NewFloat64(ref->ctx, t);
            JSValue r = JS_Call(ref->ctx, fn, JS_UNDEFINED, 1, &arg);
            if (JS_IsException(r)) Runtime::checkException(ref->ctx, r);
            JS_FreeValue(ref->ctx, r);
            JS_FreeValue(ref->ctx, arg);
            JS_FreeValue(ref->ctx, fn);
        };
        anims.push_back(std::move(a));
    } else if (anims.empty()) {
        // No props, no onUpdate: a pure wait step of `duration` seconds.
        scene::Tween::Anim a;
        a.prop = scene::Tween::Prop::Custom;
        a.duration = duration;
        a.delay = delay;
        a.ease = ease;
        anims.push_back(std::move(a));
    }
    JS_FreeValue(ctx, onUpdateVal);

    tw->addAnims(std::move(anims));
    return JS_DupValue(ctx, this_val);
}

JSValue js_tween_parallel(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (auto* tw = getTween(ctx, this_val)) tw->parallel();
    return JS_DupValue(ctx, this_val);
}

JSValue js_tween_call(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* tw = getTween(ctx, this_val);
    if (!tw) return JS_ThrowTypeError(ctx, "call: tween has been destroyed");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "call(fn) requires a function");
    tw->addCall(makeVoidCallback(ctx, argv[0]));
    return JS_DupValue(ctx, this_val);
}

// loop(n?) — run the whole sequence n times total; loop() / loop(Infinity)
// = forever.
JSValue js_tween_loop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* tw = getTween(ctx, this_val);
    if (!tw) return JS_ThrowTypeError(ctx, "loop: tween has been destroyed");
    int n = -1;
    if (argc > 0 && JS_IsNumber(argv[0])) {
        double d = jsNum(ctx, argv[0]);
        if (std::isfinite(d)) n = (int)d;
    }
    tw->setLoops(n);
    return JS_DupValue(ctx, this_val);
}

JSValue js_tween_start(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* tw = getTween(ctx, this_val);
    if (!tw) return JS_ThrowTypeError(ctx, "start: tween has been destroyed");
    tw->start();
    return JS_DupValue(ctx, this_val);
}

JSValue js_tween_stop(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (auto* tw = getTween(ctx, this_val)) tw->stop();
    return JS_DupValue(ctx, this_val);
}

JSValue js_tween_pause(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (auto* tw = getTween(ctx, this_val)) tw->pause();
    return JS_DupValue(ctx, this_val);
}

JSValue js_tween_resume(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (auto* tw = getTween(ctx, this_val)) tw->resume();
    return JS_DupValue(ctx, this_val);
}

JSValue js_tween_destroy(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<TweenWrapper>(ctx, this_val);
    if (w && w->graph()) w->graph()->destroyTween(w->id);
    return JS_UNDEFINED;
}

// createTween() — SceneGraph method.
JSValue js_sg_createTween(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    auto* t = g->createTween();
    return qjsbind::wrap<TweenWrapper>(ctx,
        new TweenWrapper{g->livenessToken(), t->id()});
}

} // namespace bro::js

#endif  // BRO_WITH_3D
