#include "engine/scene_audio_sync.h"

#if BRO_WITH_3D

#include <broaudio/engine.h>
#include "scene/mesh_node.h"
#include <bromath/mat.h>
#include <bromath/vec.h>

#include <algorithm>
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
    float currentOcclusion = 0.0f;
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

void pushOcclusion(const EmitterEntry& e, float occ) {
    if (e.isVoice) s_engine->setVoiceSpatialOcclusion(e.handle, occ);
    else           s_engine->setPlaybackSpatialOcclusion(e.handle, occ);
}

bool isDescendantOf(const scene::SceneNode* node, uint32_t ancestorId) {
    for (const scene::SceneNode* cur = node ? node->parent() : nullptr; cur != nullptr; cur = cur->parent()) {
        if (cur->id() == ancestorId) return true;
    }
    return false;
}

bool raycastOcclusionNode(scene::SceneNode* node, uint32_t emitterNodeId,
                         const bromath::Vec3& eye, const bromath::Vec3& dir, float dist) {
    if (!node) return false;
    if (node->id() == emitterNodeId) return false;

    if (node->type() == scene::SceneNode::Type::Mesh) {
        auto* meshNode = static_cast<scene::MeshNode*>(node);
        if (meshNode->id() != emitterNodeId && !isDescendantOf(meshNode, emitterNodeId)) {
            if (meshNode->drawMode() == scene::MeshNode::DrawMode::Triangles &&
                !meshNode->mesh().positions.empty()) {
                bromath::Mat4 inv = bromath::minverse(meshNode->worldMatrix());
                bromath::Vec3 localO = bromath::mtransformPoint(inv, eye);
                bromath::Vec3 localD = bromath::mtransformDir(inv, dir);
                float localDirLen = bromath::vlen(localD);
                if (localDirLen > 1e-4f) {
                    localD = localD * (1.0f / localDirLen);
                }
                float localMaxDist = dist * localDirLen;
                if (meshNode->bvh().raycastTest(meshNode->mesh(), &localO.x, &localD.x, localMaxDist)) {
                    return true;
                }
            }
        }
    }

    for (auto* child : node->children()) {
        if (raycastOcclusionNode(child, emitterNodeId, eye, dir, dist)) {
            return true;
        }
    }
    return false;
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

    scene::SceneGraph* activeGraph = nullptr;
    bromath::Vec3 activeEye{};
    bool hasActiveListener = false;

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
        if (!hasActiveListener) {
            activeGraph = g;
            activeEye = eye;
            hasActiveListener = true;
        }
        ++i;
    }

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

        float targetOcclusion = 0.0f;
        if (hasActiveListener && g) {
            const bromath::Vec3& eye = activeEye;
            float dist = bromath::vlen(wp - eye);
            if (dist > 0.01f) {
                bromath::Vec3 dir = {(wp.x - eye.x) / dist, (wp.y - eye.y) / dist, (wp.z - eye.z) / dist};
                if (g->root() && raycastOcclusionNode(g->root(), e.nodeId, eye, dir, dist)) {
                    targetOcclusion = 0.85f;
                }
            }
        }
        float rate = std::clamp(dtSec * 12.0f, 0.0f, 1.0f);
        e.currentOcclusion += (targetOcclusion - e.currentOcclusion) * rate;
        pushOcclusion(e, e.currentOcclusion);

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
    e.currentOcclusion = 0.0f;
    bromath::Vec3 wp = node->localToWorld({0.0f, 0.0f, 0.0f});
    pushPosition(e, wp);
    pushOcclusion(e, 0.0f);
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
