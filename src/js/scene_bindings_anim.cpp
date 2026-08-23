#if BRO_WITH_3D

#include "js/scene_bindings.h"
#include "js/scene_bindings.h"
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

// Shared parser for play()/playLayer() option objects.
static void readPlayOptions(JSContext* ctx, JSValueConst obj,
                            scene::AnimationPlayer::PlayOptions& opts) {
    opts.loop     = qjsbind::get_prop_bool(ctx, obj, "loop", opts.loop);
    opts.speed    = (float)qjsbind::get_prop_number(ctx, obj, "speed", opts.speed);
    opts.fadeTime = (float)qjsbind::get_prop_number(ctx, obj, "fadeTime", opts.fadeTime);
    opts.weight   = (float)qjsbind::get_prop_number(ctx, obj, "weight", opts.weight);
    JSValue maskVal = JS_GetPropertyStr(ctx, obj, "mask");
    if (!JS_IsUndefined(maskVal) && !JS_IsNull(maskVal))
        readBoneMask(ctx, maskVal, opts.mask);
    JS_FreeValue(ctx, maskVal);
}

// addBlendSpace1D(name, [{clip, pos, timescale?}, ...]) /
// addBlendSpace2D(name, [{clip, pos: [x, y], timescale?}, ...]).
static JSValue addBlendSpaceImpl(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv, bool is2D) {
    const char* fn = is2D ? "addBlendSpace2D" : "addBlendSpace1D";
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "%s: node is not a skinned mesh", fn);
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsArray(argv[1]))
        return JS_ThrowTypeError(ctx, "%s(name, points[])", fn);

    std::vector<scene::AnimationPlayer::BlendSpacePoint> points;
    JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
    int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
    for (int32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        if (!JS_IsObject(e)) {
            JS_FreeValue(ctx, e);
            return JS_ThrowTypeError(ctx, "%s: points[%d] is not an object", fn, i);
        }
        scene::AnimationPlayer::BlendSpacePoint p;
        p.clip = qjsbind::get_prop_string(ctx, e, "clip", "");
        p.timescale = (float)qjsbind::get_prop_number(ctx, e, "timescale", 1.0);
        JSValue posVal = JS_GetPropertyStr(ctx, e, "pos");
        if (is2D) {
            bool ok = JS_IsArray(posVal);
            if (ok) {
                for (int k = 0; k < 2; k++) {
                    JSValue c = JS_GetPropertyUint32(ctx, posVal, (uint32_t)k);
                    double d = 0;
                    ok = ok && !JS_ToFloat64(ctx, &d, c);
                    JS_FreeValue(ctx, c);
                    p.pos[k] = (float)d;
                }
            }
            if (!ok) {
                JS_FreeValue(ctx, posVal); JS_FreeValue(ctx, e);
                return JS_ThrowTypeError(ctx,
                    "%s: points[%d].pos must be [x, y]", fn, i);
            }
        } else {
            double d = 0;
            if (JS_ToFloat64(ctx, &d, posVal)) {
                JS_FreeValue(ctx, posVal); JS_FreeValue(ctx, e);
                return JS_ThrowTypeError(ctx,
                    "%s: points[%d].pos must be a number", fn, i);
            }
            p.pos[0] = (float)d;
        }
        JS_FreeValue(ctx, posVal);
        JS_FreeValue(ctx, e);
        points.push_back(std::move(p));
    }

    std::string name = jsStr(ctx, argv[0]);
    auto& player = sm->ensurePlayer();
    bool ok = is2D ? player.addBlendSpace2D(name, std::move(points))
                   : player.addBlendSpace1D(name, std::move(points));
    if (!ok)
        return JS_ThrowTypeError(ctx,
            "%s: '%s' needs at least one point and every clip registered "
            "via addClip first", fn, name.c_str());
    return JS_DupValue(ctx, this_val);
}

JSValue js_node_addBlendSpace1D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return addBlendSpaceImpl(ctx, this_val, argc, argv, false);
}

JSValue js_node_addBlendSpace2D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return addBlendSpaceImpl(ctx, this_val, argc, argv, true);
}

// setBlendPos(name, x[, y]) or setBlendPos(name, [x, y]).
JSValue js_node_setBlendPos(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "setBlendPos: node is not a skinned mesh");
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "setBlendPos(name, x[, y])");
    double x = 0, y = 0;
    if (JS_IsArray(argv[1])) {
        JSValue e0 = JS_GetPropertyUint32(ctx, argv[1], 0);
        JSValue e1 = JS_GetPropertyUint32(ctx, argv[1], 1);
        JS_ToFloat64(ctx, &x, e0);
        if (!JS_IsUndefined(e1)) JS_ToFloat64(ctx, &y, e1);
        JS_FreeValue(ctx, e0); JS_FreeValue(ctx, e1);
    } else {
        JS_ToFloat64(ctx, &x, argv[1]);
        if (argc > 2) JS_ToFloat64(ctx, &y, argv[2]);
    }
    std::string name = jsStr(ctx, argv[0]);
    auto* player = sm->player();
    if (!player || !player->setBlendPos(name, (float)x, (float)y))
        return JS_ThrowTypeError(ctx, "setBlendPos: unknown blend space '%s'",
                                 name.c_str());
    return JS_DupValue(ctx, this_val);
}

// addStateMachine({states, transitions, initial?}) — install a state machine
// on the animation player and enter its initial state (defaults to the first
// state). States: {name, source, speed?, loop?} where source names a
// registered clip or blend space. Transitions: {from, to, fade?,
// autoAdvance?, syncPhase?}; from may be '*' (wildcard).
JSValue js_node_addStateMachine(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "addStateMachine: node is not a skinned mesh");
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "addStateMachine({states, transitions, initial?})");

    scene::AnimationPlayer::StateMachineDef def;
    def.initial = qjsbind::get_prop_string(ctx, argv[0], "initial", "");

    JSValue statesVal = JS_GetPropertyStr(ctx, argv[0], "states");
    if (JS_IsArray(statesVal)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, statesVal, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, statesVal, (uint32_t)i);
            scene::AnimationPlayer::StateDef st;
            if (JS_IsObject(e)) {
                st.name   = qjsbind::get_prop_string(ctx, e, "name", "");
                st.source = qjsbind::get_prop_string(ctx, e, "source", "");
                st.speed  = (float)qjsbind::get_prop_number(ctx, e, "speed", 1.0);
                st.loop   = qjsbind::get_prop_bool(ctx, e, "loop", true);
            }
            JS_FreeValue(ctx, e);
            def.states.push_back(std::move(st));
        }
    }
    JS_FreeValue(ctx, statesVal);

    JSValue transVal = JS_GetPropertyStr(ctx, argv[0], "transitions");
    if (JS_IsArray(transVal)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, transVal, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, transVal, (uint32_t)i);
            scene::AnimationPlayer::TransitionDef tr;
            if (JS_IsObject(e)) {
                tr.from = qjsbind::get_prop_string(ctx, e, "from", "");
                tr.to   = qjsbind::get_prop_string(ctx, e, "to", "");
                tr.fade = (float)qjsbind::get_prop_number(ctx, e, "fade", 0.0);
                tr.autoAdvance = qjsbind::get_prop_bool(ctx, e, "autoAdvance", false);
                tr.syncPhase   = qjsbind::get_prop_bool(ctx, e, "syncPhase", false);
            }
            JS_FreeValue(ctx, e);
            def.transitions.push_back(std::move(tr));
        }
    }
    JS_FreeValue(ctx, transVal);

    std::string err;
    if (!sm->ensurePlayer().setStateMachine(std::move(def), &err))
        return JS_ThrowTypeError(ctx, "addStateMachine: %s", err.c_str());
    return JS_DupValue(ctx, this_val);
}

// travel(stateName) — follow the defined transition (wildcard fallback) from
// the current state; no defined transition warns and hard-switches (fade 0).
JSValue js_node_travel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "travel: node is not a skinned mesh");
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "travel(stateName)");
    std::string name = jsStr(ctx, argv[0]);
    auto* player = sm->player();
    if (!player || !player->travel(name))
        return JS_ThrowTypeError(ctx,
            "travel: unknown state '%s' (addStateMachine first)", name.c_str());
    return JS_DupValue(ctx, this_val);
}

// setRootMotion({enabled, bone?, extractY?}) — bone is a name or index;
// omitted = auto-detect (first parentless bone, else bone 0).
JSValue js_node_setRootMotion(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "setRootMotion: node is not a skinned mesh");
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "setRootMotion({enabled, bone?, extractY?})");

    scene::AnimationPlayer::RootMotionOptions opts;
    opts.enabled  = qjsbind::get_prop_bool(ctx, argv[0], "enabled", opts.enabled);
    opts.extractY = qjsbind::get_prop_bool(ctx, argv[0], "extractY", opts.extractY);
    JSValue boneVal = JS_GetPropertyStr(ctx, argv[0], "bone");
    if (JS_IsString(boneVal))       opts.boneName = jsStr(ctx, boneVal);
    else if (JS_IsNumber(boneVal))  opts.bone = (int)jsNum(ctx, boneVal);
    JS_FreeValue(ctx, boneVal);

    if (!sm->ensurePlayer().setRootMotion(opts))
        return JS_ThrowTypeError(ctx,
            "setRootMotion: no skeleton (setSkeleton first) or unknown bone");
    return JS_DupValue(ctx, this_val);
}

// consumeRootMotion() — {translation: [x, y, z], yaw} accumulated in MODEL
// space since the last call; resets on read.
JSValue js_node_consumeRootMotion(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "consumeRootMotion: node is not a skinned mesh");
    scene::AnimationPlayer::RootMotionDelta d;
    if (auto* player = sm->player()) d = player->consumeRootMotion();
    JSValue obj = JS_NewObject(ctx);
    JSValue t = JS_NewArray(ctx);
    for (uint32_t i = 0; i < 3; i++)
        JS_SetPropertyUint32(ctx, t, i, JS_NewFloat64(ctx, d.translation[i]));
    JS_SetPropertyStr(ctx, obj, "translation", t);
    JS_SetPropertyStr(ctx, obj, "yaw", JS_NewFloat64(ctx, d.yaw));
    return obj;
}

// blendState() — { state, clips: [{name, weight}], phase, pos?, layers: [...] }.
JSValue js_node_blendState(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "blendState: node is not a skinned mesh");
    auto* player = sm->player();
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "state", JS_NULL);
    JSValue clips = JS_NewArray(ctx);
    JSValue layers = JS_NewArray(ctx);
    double phase = 0.0;
    if (player) {
        auto s = player->blendState();
        phase = s.phase;
        if (!s.state.empty())
            JS_SetPropertyStr(ctx, obj, "state", JS_NewString(ctx, s.state.c_str()));
        for (uint32_t i = 0; i < s.clips.size(); i++) {
            JSValue c = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, c, "name", JS_NewString(ctx, s.clips[i].name.c_str()));
            JS_SetPropertyStr(ctx, c, "weight", JS_NewFloat64(ctx, s.clips[i].weight));
            JS_SetPropertyUint32(ctx, clips, i, c);
        }
        if (s.hasPos) {
            JSValue pos = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, pos, 0, JS_NewFloat64(ctx, s.pos[0]));
            if (s.is2D)
                JS_SetPropertyUint32(ctx, pos, 1, JS_NewFloat64(ctx, s.pos[1]));
            JS_SetPropertyStr(ctx, obj, "pos", pos);
        }
        for (uint32_t i = 0; i < s.layers.size(); i++) {
            const auto& L = s.layers[i];
            JSValue l = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, l, "slot", JS_NewInt32(ctx, L.slot));
            JS_SetPropertyStr(ctx, l, "name", JS_NewString(ctx, L.name.c_str()));
            JS_SetPropertyStr(ctx, l, "weight", JS_NewFloat64(ctx, L.weight));
            JS_SetPropertyStr(ctx, l, "phase", JS_NewFloat64(ctx, L.phase));
            JS_SetPropertyUint32(ctx, layers, i, l);
        }
    }
    JS_SetPropertyStr(ctx, obj, "clips", clips);
    JS_SetPropertyStr(ctx, obj, "phase", JS_NewFloat64(ctx, phase));
    JS_SetPropertyStr(ctx, obj, "layers", layers);
    return obj;
}

// playLayer(slot, name, {mask?, weight?, speed?, loop?, fadeTime?}).
JSValue js_node_playLayer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "playLayer: node is not a skinned mesh");
    if (argc < 2 || !JS_IsNumber(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "playLayer(slot, clipName[, opts])");
    int slot = (int)jsNum(ctx, argv[0]);
    scene::AnimationPlayer::PlayOptions opts;
    if (argc > 2 && JS_IsObject(argv[2])) readPlayOptions(ctx, argv[2], opts);
    std::string name = jsStr(ctx, argv[1]);
    if (!sm->ensurePlayer().playLayer(slot, name, opts))
        return JS_ThrowTypeError(ctx,
            "playLayer: bad slot %d (0..%d), unknown clip '%s', or no "
            "skeleton (blend spaces are base-track only)",
            slot, scene::AnimationPlayer::kMaxLayers - 1, name.c_str());
    return JS_DupValue(ctx, this_val);
}

// stopLayer(slot[, {fadeTime}]).
JSValue js_node_stopLayer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "stopLayer: node is not a skinned mesh");
    if (argc < 1 || !JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "stopLayer(slot[, {fadeTime}])");
    float fade = 0.0f;
    if (argc > 1 && JS_IsObject(argv[1]))
        fade = (float)qjsbind::get_prop_number(ctx, argv[1], "fadeTime", 0.0);
    if (auto* player = sm->player())
        player->stopLayer((int)jsNum(ctx, argv[0]), fade);
    return JS_DupValue(ctx, this_val);
}

// setLayerWeight(slot, weight).
JSValue js_node_setLayerWeight(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    auto* sm = asSkinnedMesh(w);
    if (!sm)
        return JS_ThrowTypeError(ctx, "setLayerWeight: node is not a skinned mesh");
    if (argc < 2 || !JS_IsNumber(argv[0]) || !JS_IsNumber(argv[1]))
        return JS_ThrowTypeError(ctx, "setLayerWeight(slot, weight)");
    auto* player = sm->player();
    if (!player || !player->setLayerWeight((int)jsNum(ctx, argv[0]),
                                           (float)jsNum(ctx, argv[1])))
        return JS_ThrowTypeError(ctx, "setLayerWeight: no active layer in slot %d",
                                 (int)jsNum(ctx, argv[0]));
    return JS_DupValue(ctx, this_val);
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
            if (argc > 1 && JS_IsObject(argv[1])) readPlayOptions(ctx, argv[1], opts);
            std::string name = jsStr(ctx, argv[0]);
            if (!player.play(name, opts))
                return JS_ThrowTypeError(ctx,
                    "play: unknown clip or blend space '%s' (addClip / "
                    "addBlendSpace first) or no skeleton (setSkeleton first)",
                    name.c_str());
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
bool readTweenVec3(JSContext* ctx, JSValueConst v, bromath::Vec3& out) {
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
bool readTweenQuat(JSContext* ctx, JSValueConst v, bromath::Quat& out) {
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
bool readTweenColor(JSContext* ctx, JSValueConst v, bromath::Vec3& out) {
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

static inline void _unused_scene_anim_install(JSContext* ctx)
{
    // No install needed
    (void)ctx;
}

} // namespace bro::js

#endif // BRO_WITH_3D
