#include "js/audio_scene_sync.h"

#if BRO_WITH_3D  // modular-build feature gate

#include "engine/scene_audio_sync.h"
#include "js/scene_bindings_internal.h"

#include <cstdint>

namespace bro::js {

void installAudioSceneSync(broaudio::Engine* engine) {
    bro::engine::SceneAudioSync::install(engine);
}

void shutdownAudioSceneSync() {
    bro::engine::SceneAudioSync::shutdown();
}

void syncAudioSceneEmitters(float dtSec) {
    bro::engine::SceneAudioSync::sync(dtSec);
}

JSValue js_node_attachAudioEmitter(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    scene::SceneNode* n = w ? w->node() : nullptr;
    if (!n || argc < 1) return JS_UNDEFINED;

    int handle = -1;
    JS_ToInt32(ctx, &handle, argv[0]);
    if (handle < 0) return JS_UNDEFINED;

    bool isVoice = false;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "voice");
        isVoice = JS_ToBool(ctx, v) > 0;
        JS_FreeValue(ctx, v);
    }

    bro::engine::SceneAudioSync::attachAudioEmitter(w->token, n, handle, isVoice);
    return JS_UNDEFINED;
}

JSValue js_node_detachAudioEmitter(JSContext* ctx, JSValueConst this_val,
                                   int /*argc*/, JSValueConst* /*argv*/) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (w) bro::engine::SceneAudioSync::detachAudioEmitter(w->id);
    return JS_UNDEFINED;
}

JSValue js_sg_bindAudioListenerToCamera(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<GraphWrapper>(ctx, this_val);
    scene::SceneGraph* g = w ? w->graph() : nullptr;
    if (!g) return JS_UNDEFINED;

    bool on = argc < 1 || JS_ToBool(ctx, argv[0]) > 0;  // default true
    bro::engine::SceneAudioSync::bindAudioListenerToCamera(g->livenessToken(), g, on);
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
