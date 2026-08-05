#include "engine/scene_audio_sync.h"

#if BRO_WITH_3D

#include <broaudio/engine.h>

#include <cstdint>
#include <vector>

namespace bro::engine {

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

void SceneAudioSync::install(broaudio::Engine* engine) {
    s_engine = engine;
    s_emitters.clear();
    s_listeners.clear();
}

void SceneAudioSync::shutdown() {
    s_engine = nullptr;
    s_emitters.clear();
    s_listeners.clear();
}

void SceneAudioSync::sync(float dtSec) {
    if (!s_engine) return;

    for (size_t i = 0; i < s_emitters.size();) {
        EmitterEntry& e = s_emitters[i];
        auto t = e.token.lock();
        scene::SceneGraph* g = t ? t->graph : nullptr;
        scene::SceneNode* n = g ? g->resolveNode(e.nodeId) : nullptr;
        if (!n) {
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

void SceneAudioSync::attachAudioEmitter(std::weak_ptr<scene::SceneGraph::LivenessToken> token,
                                        scene::SceneNode* node, int handle, bool isVoice) {
    if (!node || !s_engine || handle < 0) return;

    removeEmitterForNode(node->id());

    if (isVoice) s_engine->setVoiceSpatialEnabled(handle, true);
    else         s_engine->setPlaybackSpatialEnabled(handle, true);

    EmitterEntry e;
    e.token = token;
    e.nodeId = node->id();
    e.handle = handle;
    e.isVoice = isVoice;
    bromath::Vec3 wp = node->localToWorld({0.0f, 0.0f, 0.0f});
    pushPosition(e, wp);
    e.prevPos = wp;
    e.hasPrev = true;
    s_emitters.push_back(e);
}

void SceneAudioSync::detachAudioEmitter(uint32_t nodeId) {
    removeEmitterForNode(nodeId);
}

void SceneAudioSync::bindAudioListenerToCamera(std::weak_ptr<scene::SceneGraph::LivenessToken> token,
                                              scene::SceneGraph* graph, bool enable) {
    if (!graph || !s_engine) return;

    for (size_t i = 0; i < s_listeners.size(); ++i) {
        auto t = s_listeners[i].token.lock();
        if (t && t->graph == graph) {
            if (!enable) s_listeners.erase(s_listeners.begin() + i);
            return;
        }
    }
    if (enable) {
        ListenerEntry l;
        l.token = token;
        s_listeners.push_back(l);
    }
}

} // namespace bro::engine

#else // !BRO_WITH_3D

namespace bro::engine {
void SceneAudioSync::install(broaudio::Engine* /*engine*/) {}
void SceneAudioSync::shutdown() {}
void SceneAudioSync::sync(float /*dtSec*/) {}
void SceneAudioSync::attachAudioEmitter(std::weak_ptr<scene::SceneGraph::LivenessToken>,
                                        scene::SceneNode*, int, bool) {}
void SceneAudioSync::detachAudioEmitter(uint32_t) {}
void SceneAudioSync::bindAudioListenerToCamera(std::weak_ptr<scene::SceneGraph::LivenessToken>,
                                              scene::SceneGraph*, bool) {}
} // namespace bro::engine

#endif
