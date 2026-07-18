#include "js/audio_scene_sync.h"

#if BRO_WITH_3D  // modular-build feature gate

#include "js/scene_bindings_internal.h"

#include <broaudio/engine.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace bro::js {

namespace {

struct EmitterEntry {
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    uint32_t nodeId = 0;
    int handle = -1;       // broaudio playback instance id or voice id
    bool isVoice = false;
    bool hasPrev = false;  // prevPos valid (velocity needs two samples)
    bromath::Vec3 prevPos{};
};

struct ListenerEntry {
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    bool hasPrev = false;
    bromath::Vec3 prevEye{};
};

broaudio::Engine* s_engine = nullptr;
std::vector<EmitterEntry> s_emitters;
std::vector<ListenerEntry> s_listeners;

void pushPosition(const EmitterEntry& e, const bromath::Vec3& p) {
    if (e.isVoice) s_engine->setVoiceSpatialPosition(e.handle, p.x, p.y, p.z);
    else           s_engine->setPlaybackSpatialPosition(e.handle, p.x, p.y, p.z);
}

void pushVelocity(const EmitterEntry& e, const bromath::Vec3& v) {
    if (e.isVoice) s_engine->setVoiceSpatialVelocity(e.handle, v.x, v.y, v.z);
    else           s_engine->setPlaybackSpatialVelocity(e.handle, v.x, v.y, v.z);
}

void removeEmitterForNode(uint32_t nodeId) {
    for (size_t i = 0; i < s_emitters.size(); ++i) {
        if (s_emitters[i].nodeId == nodeId) {
            s_emitters.erase(s_emitters.begin() + i);
            return;
        }
    }
}

} // namespace

void installAudioSceneSync(broaudio::Engine* engine) {
    s_engine = engine;
    s_emitters.clear();
    s_listeners.clear();
}

void shutdownAudioSceneSync() {
    s_engine = nullptr;
    s_emitters.clear();
    s_listeners.clear();
}

void syncAudioSceneEmitters(float dtSec) {
    if (!s_engine) return;

    // Emitters: world position every frame; velocity by finite difference of
    // the last two synced positions over scaled dt (paused time -> dt 0 ->
    // velocity holds, which is moot since the audio clock is paused too).
    for (size_t i = 0; i < s_emitters.size();) {
        EmitterEntry& e = s_emitters[i];
        auto t = e.token.lock();
        scene::SceneGraph* g = t ? t->graph : nullptr;
        scene::SceneNode* n = g ? g->resolveNode(e.nodeId) : nullptr;
        if (!n) {
            // Node or graph destroyed — the binding dies with it. (A dead
            // AUDIO handle, by contrast, keeps the entry: broaudio setters
            // no-op on it, and the app may attach a fresh handle later.)
            s_emitters.erase(s_emitters.begin() + i);
            continue;
        }
        bromath::Vec3 wp = n->localToWorld({0.0f, 0.0f, 0.0f});
        pushPosition(e, wp);
        if (dtSec > 1e-6f) {
            if (e.hasPrev) {
                float inv = 1.0f / dtSec;
                pushVelocity(e, {(wp.x - e.prevPos.x) * inv,
                                 (wp.y - e.prevPos.y) * inv,
                                 (wp.z - e.prevPos.z) * inv});
            }
            e.prevPos = wp;
            e.hasPrev = true;
        }
        ++i;
    }

    // Camera-bound listeners. View matrix rows are the camera basis in world
    // space: row 1 = up, row 2 = -forward (same extraction as unprojectLocal).
    // Multiple bound graphs would fight over the single listener — last one
    // wins; binding is per-scene and apps bind one.
    for (size_t i = 0; i < s_listeners.size();) {
        ListenerEntry& l = s_listeners[i];
        auto t = l.token.lock();
        scene::SceneGraph* g = t ? t->graph : nullptr;
        if (!g) {
            s_listeners.erase(s_listeners.begin() + i);
            continue;
        }
        const bromath::Mat4& V = g->viewMatrix();
        bromath::Vec3 up { V.at(1, 0),  V.at(1, 1),  V.at(1, 2)};
        bromath::Vec3 fwd{-V.at(2, 0), -V.at(2, 1), -V.at(2, 2)};
        bromath::Vec3 eye = g->cameraEye();
        s_engine->setListenerPosition(eye.x, eye.y, eye.z);
        s_engine->setListenerOrientation(fwd.x, fwd.y, fwd.z, up.x, up.y, up.z);
        if (dtSec > 1e-6f) {
            if (l.hasPrev) {
                float inv = 1.0f / dtSec;
                s_engine->setListenerVelocity((eye.x - l.prevEye.x) * inv,
                                              (eye.y - l.prevEye.y) * inv,
                                              (eye.z - l.prevEye.z) * inv);
            }
            l.prevEye = eye;
            l.hasPrev = true;
        }
        ++i;
    }
}

// ---------------------------------------------------------------------------
// JS callbacks (registered on the SceneNode / SceneGraph prototypes by
// SceneBindings::install — declarations in scene_bindings_internal.h)
// ---------------------------------------------------------------------------

JSValue js_node_attachAudioEmitter(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    scene::SceneNode* n = w ? w->node() : nullptr;
    if (!n || !s_engine || argc < 1) return JS_UNDEFINED;

    int handle = -1;
    JS_ToInt32(ctx, &handle, argv[0]);
    if (handle < 0) return JS_UNDEFINED;

    bool isVoice = false;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "voice");
        isVoice = JS_ToBool(ctx, v) > 0;
        JS_FreeValue(ctx, v);
    }

    // One emitter per node — re-attach replaces.
    removeEmitterForNode(n->id());

    // Attaching implies spatialization; push the current position now so the
    // very first mixed block is already placed (velocity needs a second
    // frame).
    if (isVoice) s_engine->setVoiceSpatialEnabled(handle, true);
    else         s_engine->setPlaybackSpatialEnabled(handle, true);

    EmitterEntry e;
    e.token = w->token;
    e.nodeId = n->id();
    e.handle = handle;
    e.isVoice = isVoice;
    bromath::Vec3 wp = n->localToWorld({0.0f, 0.0f, 0.0f});
    pushPosition(e, wp);
    e.prevPos = wp;
    e.hasPrev = true;
    s_emitters.push_back(e);
    return JS_UNDEFINED;
}

JSValue js_node_detachAudioEmitter(JSContext* ctx, JSValueConst this_val,
                                   int /*argc*/, JSValueConst* /*argv*/) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (w) removeEmitterForNode(w->id);
    return JS_UNDEFINED;
}

JSValue js_sg_bindAudioListenerToCamera(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<GraphWrapper>(ctx, this_val);
    scene::SceneGraph* g = w ? w->graph() : nullptr;
    if (!g || !s_engine) return JS_UNDEFINED;

    bool on = argc < 1 || JS_ToBool(ctx, argv[0]) > 0;  // default true

    for (size_t i = 0; i < s_listeners.size(); ++i) {
        auto t = s_listeners[i].token.lock();
        if (t && t->graph == g) {
            if (!on) s_listeners.erase(s_listeners.begin() + i);
            return JS_UNDEFINED;  // already bound / just unbound
        }
    }
    if (on) {
        ListenerEntry l;
        l.token = g->livenessToken();
        s_listeners.push_back(l);
    }
    return JS_UNDEFINED;
}

} // namespace bro::js

#else  // !BRO_WITH_3D — no scene graph, nothing to sync

namespace bro::js {
void installAudioSceneSync(broaudio::Engine* /*engine*/) {}
void shutdownAudioSceneSync() {}
void syncAudioSceneEmitters(float /*dtSec*/) {}
} // namespace bro::js

#endif  // BRO_WITH_3D
