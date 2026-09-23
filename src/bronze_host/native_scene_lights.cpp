// native_scene_lights.cpp — Skinned mesh animation, instanced mesh, HTML, particle, and FX methods on SceneNode.

#include "bronze_host/native_scene_internal.h"
#include "natives/scene/native_scene_decl.h"
#include "scene/animation_player.h"
#include "scene/instanced_mesh_node.h"
#include "scene/reflection_probe_node.h"
#include "scene/particle_node.h"
#include "scene/particles3d_node.h"
#include "scene/html_node.h"
#include <bromesh/animation/pose.h>
#include <bromesh/rigging/auto_rig.h>
#include <bromesh/io/splat_ply.h>
#include <bromesh/api.h>
#include "util/log.h"
#include <cstring>
#include <vector>

#include "json.hpp"
#include "embed/embed.h"

namespace bro::bronze_host {
using json = nlohmann::json;
}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;
namespace ev = bronze::embed;

void bro_scene_SceneNode_setSkeleton(void* self, uint64_t skelBits) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm) return;
    if (const auto* sk = bromesh::api::skeletonOf(ev::fromBits(skelBits))) {
        sm->ensurePlayer().setSkeleton(std::make_shared<bromesh::Skeleton>(*sk));
    }
}

void bro_scene_SceneNode_addClip(void* self, const char* name, uint64_t animBits) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !name) return;
    if (const auto* a = bromesh::api::animationOf(ev::fromBits(animBits))) {
        sm->ensurePlayer().addClip(name, std::make_shared<bromesh::Animation>(*a));
    }
}

void bro_scene_SceneNode_addBlendSpace1D(void* self, const char* name, const char* clipsJson) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !name) return;
    std::vector<scene::AnimationPlayer::BlendSpacePoint> pts;
    if (clipsJson && *clipsJson) {
        auto j = json::parse(clipsJson, nullptr, false);
        if (j.is_array()) {
            for (const auto& item : j) {
                scene::AnimationPlayer::BlendSpacePoint pt;
                pt.clip = item.value("clip", "");
                pt.pos[0] = item.value("pos", 0.0f);
                pt.timescale = item.value("timescale", 1.0f);
                pts.push_back(pt);
            }
        }
    }
    if (pts.empty()) {
        ev::throwTypeError("addBlendSpace1D: no points provided");
        return;
    }
    if (!sm->ensurePlayer().addBlendSpace1D(name, pts)) {
        ev::throwTypeError("addBlendSpace1D: failed (unknown clip or invalid points)");
        return;
    }
}

void bro_scene_SceneNode_addBlendSpace2D(void* self, const char* name, const char* clipsJson) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !name) return;
    std::vector<scene::AnimationPlayer::BlendSpacePoint> pts;
    if (clipsJson && *clipsJson) {
        auto j = json::parse(clipsJson, nullptr, false);
        if (j.is_array()) {
            for (const auto& item : j) {
                scene::AnimationPlayer::BlendSpacePoint pt;
                pt.clip = item.value("clip", "");
                if (item.contains("pos") && item["pos"].is_array() && item["pos"].size() >= 2) {
                    pt.pos[0] = item["pos"][0].get<float>();
                    pt.pos[1] = item["pos"][1].get<float>();
                }
                pt.timescale = item.value("timescale", 1.0f);
                pts.push_back(pt);
            }
        }
    }
    if (pts.empty()) {
        ev::throwTypeError("addBlendSpace2D: no points provided");
        return;
    }
    if (!sm->ensurePlayer().addBlendSpace2D(name, pts)) {
        ev::throwTypeError("addBlendSpace2D: failed (unknown clip or invalid points)");
        return;
    }
}

void bro_scene_SceneNode_setBlendPos(void* self, const char* name, double x, bool y_given, double y) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player() && name) {
        float fy = y_given ? static_cast<float>(y) : 0.0f;
        sm->player()->setBlendPos(name, static_cast<float>(x), fy);
    }
}

void bro_scene_SceneNode_playLayer(void* self, int32_t layer, const char* clipName, const char* jsonOpts) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) {
        ev::throwTypeError("playLayer: node is not a SkinnedMeshNode");
        return;
    }
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm) {
        ev::throwTypeError("playLayer: node is not a SkinnedMeshNode");
        return;
    }
    if (layer < 0 || layer >= scene::AnimationPlayer::kMaxLayers) {
        ev::throwTypeError("playLayer: layer slot out of range [0, 7]");
        return;
    }
    auto* player = sm->player();
    if (!player || !player->skeleton()) {
        ev::throwTypeError("playLayer: mesh has no skeleton assigned");
        return;
    }
    if (player->hasBlendSpace(clipName)) {
        ev::throwTypeError("playLayer: blend spaces are base-track only");
        return;
    }
    if (!player->hasClip(clipName)) {
        ev::throwTypeError(std::string("playLayer: unknown clip '") + clipName + "'");
        return;
    }
    scene::AnimationPlayer::PlayOptions opts;
    if (jsonOpts && *jsonOpts) {
        auto j = json::parse(jsonOpts, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            opts.loop = j.value("loop", true);
            opts.speed = j.value("speed", 1.0f);
            opts.fadeTime = j.value("fadeTime", j.value("fade", 0.0f));
            opts.weight = j.value("weight", 1.0f);
            if (j.contains("mask")) {
                if (j["mask"].is_array()) {
                    for (auto v : j["mask"]) opts.mask.push_back(v.get<uint8_t>());
                } else if (j["mask"].is_object()) {
                    size_t count = j["mask"].size();
                    opts.mask.resize(count);
                    for (size_t i = 0; i < count; ++i) {
                        std::string k = std::to_string(i);
                        if (j["mask"].contains(k)) opts.mask[i] = j["mask"][k].get<uint8_t>();
                    }
                }
            }
        }
    }
    if (!player->playLayer(layer, clipName, opts)) {
        ev::throwTypeError("playLayer: failed");
    }
}

void bro_scene_SceneNode_stopLayer(void* self, int32_t layer, bool fadeTime_given, double fadeTime) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player()) {
        float f = fadeTime_given ? static_cast<float>(fadeTime) : 0.0f;
        sm->player()->stopLayer(layer, f);
    }
}

void bro_scene_SceneNode_setLayerWeight(void* self, int32_t layer, double weight) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player()) {
        if (!sm->player()->setLayerWeight(layer, static_cast<float>(weight))) {
            ev::throwTypeError("setLayerWeight: failed (empty or invalid slot)");
        }
    }
}

void bro_scene_SceneNode_addStateMachine(void* self, const char* jsonDef) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) {
        ev::throwTypeError("addStateMachine: node is not a SkinnedMeshNode");
        return;
    }
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm) {
        ev::throwTypeError("addStateMachine: node is not a SkinnedMeshNode");
        return;
    }
    if (!jsonDef || !*jsonDef) {
        ev::throwTypeError("addStateMachine: definition required");
        return;
    }
    auto j = json::parse(jsonDef, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        ev::throwTypeError("addStateMachine: invalid JSON definition");
        return;
    }
    scene::AnimationPlayer::StateMachineDef def;
    def.initial = j.value("initial", "");
    if (j.contains("states") && j["states"].is_array()) {
        for (const auto& s : j["states"]) {
            scene::AnimationPlayer::StateDef sd;
            sd.name = s.value("name", "");
            sd.source = s.value("source", "");
            sd.speed = s.value("speed", 1.0f);
            sd.loop = s.value("loop", true);
            def.states.push_back(sd);
        }
    }
    if (j.contains("transitions") && j["transitions"].is_array()) {
        for (const auto& t : j["transitions"]) {
            scene::AnimationPlayer::TransitionDef td;
            td.from = t.value("from", "");
            td.to = t.value("to", "");
            td.fade = t.value("fade", 0.0f);
            td.autoAdvance = t.value("autoAdvance", false);
            td.syncPhase = t.value("syncPhase", false);
            def.transitions.push_back(td);
        }
    }
    std::string err;
    if (!sm->ensurePlayer().setStateMachine(def, &err)) {
        ev::throwTypeError("addStateMachine: " + err);
        return;
    }
}

void bro_scene_SceneNode_travel(void* self, const char* targetState) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) {
        ev::throwTypeError("travel: node is not a SkinnedMeshNode");
        return;
    }
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !sm->player() || !sm->player()->hasStateMachine()) {
        ev::throwTypeError("travel before addStateMachine throws");
        return;
    }
    if (!targetState || !*targetState) {
        ev::throwTypeError("travel: target state required");
        return;
    }
    bool ok = sm->player()->travel(targetState);
    if (!ok) {
        ev::throwTypeError(std::string("travel: unknown state '") + targetState + "'");
        return;
    }
}

void bro_scene_SceneNode_setRootMotion(void* self, const char* jsonOpts) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) {
        ev::throwTypeError("setRootMotion: node is not a SkinnedMeshNode");
        return;
    }
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm) {
        ev::throwTypeError("setRootMotion: node is not a SkinnedMeshNode");
        return;
    }
    scene::AnimationPlayer::RootMotionOptions opts;
    if (jsonOpts && *jsonOpts) {
        auto j = json::parse(jsonOpts, nullptr, false);
        if (j.is_boolean()) {
            opts.enabled = j.get<bool>();
        } else if (j.is_object()) {
            opts.enabled = j.value("enabled", true);
            if (j.contains("bone")) {
                if (j["bone"].is_string()) {
                    opts.boneName = j["bone"].get<std::string>();
                } else if (j["bone"].is_number()) {
                    opts.bone = j["bone"].get<int>();
                }
            }
            opts.extractY = j.value("extractY", false);
        }
    }
    auto* player = sm->player();
    if (opts.enabled && (!player || !player->skeleton())) {
        ev::throwTypeError("setRootMotion before setSkeleton throws");
        return;
    }
    if (!sm->ensurePlayer().setRootMotion(opts)) {
        ev::throwTypeError("setRootMotion: failed (unknown or out-of-range bone)");
        return;
    }
}

const char* bro_scene_SceneNode_consumeRootMotion(void* self) {
    static thread_local std::string tl_rmJson;
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return "{\"translation\":[0,0,0],\"yaw\":0}";
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !sm->player()) return "{\"translation\":[0,0,0],\"yaw\":0}";
    auto rm = sm->player()->consumeRootMotion();
    json j;
    j["translation"] = {rm.translation[0], rm.translation[1], rm.translation[2]};
    j["yaw"] = rm.yaw;
    tl_rmJson = j.dump();
    return tl_rmJson.c_str();
}

void bro_scene_SceneNode_play(void* self, const char* clipName, const char* jsonOpts) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Sprite) {
        if (clipName && clipName[0]) {
            static_cast<scene::SpriteNode*>(n)->play(clipName);
        }
    } else if (n->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(n)->play();
    } else if (n->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(n)->play();
    } else if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (sm) {
            auto* player = sm->player();
            if (!player || !player->skeleton()) {
                ev::throwTypeError("play before setSkeleton/addClip throws");
                return;
            }
            if (clipName && clipName[0]) {
                if (!player->hasClip(clipName) && !player->hasBlendSpace(clipName)) {
                    ev::throwTypeError(std::string("play: unknown clip '") + clipName + "'");
                    return;
                }
                scene::AnimationPlayer::PlayOptions opts;
                if (jsonOpts && *jsonOpts) {
                    auto j = json::parse(jsonOpts, nullptr, false);
                    if (!j.is_discarded() && j.is_object()) {
                        opts.loop = j.value("loop", true);
                        opts.speed = j.value("speed", 1.0f);
                        opts.fadeTime = j.value("fadeTime", j.value("fade", 0.0f));
                        opts.weight = j.value("weight", 1.0f);
                        if (j.contains("mask")) {
                            if (j["mask"].is_array()) {
                                for (auto v : j["mask"]) opts.mask.push_back(v.get<uint8_t>());
                            } else if (j["mask"].is_object()) {
                                size_t count = j["mask"].size();
                                opts.mask.resize(count);
                                for (size_t i = 0; i < count; ++i) {
                                    std::string k = std::to_string(i);
                                    if (j["mask"].contains(k)) opts.mask[i] = j["mask"][k].get<uint8_t>();
                                }
                            }
                        }
                    }
                }
                player->play(clipName, opts);
            }
        }
    }
}

void bro_scene_SceneNode_stop(void* self, const char* jsonOpts) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Sprite) {
        static_cast<scene::SpriteNode*>(n)->stop();
    } else if (n->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(n)->stop();
    } else if (n->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(n)->stop();
    } else if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (sm && sm->player()) {
            float fadeTime = 0.0f;
            if (jsonOpts && *jsonOpts) {
                auto j = json::parse(jsonOpts, nullptr, false);
                if (!j.is_discarded() && j.is_object()) {
                    fadeTime = j.value("fadeTime", j.value("fade", 0.0f));
                }
            }
            sm->player()->stop(fadeTime);
        }
    }
}

void bro_scene_SceneNode_pause(void* self) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (sm && sm->player()) sm->player()->pause();
    }
}

void bro_scene_SceneNode_resume(void* self) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (sm && sm->player()) sm->player()->resume();
    }
}

int32_t bro_scene_SceneNode_setSkinningMatrices(void* self, uint64_t matsBits) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) {
        ev::throwTypeError("setSkinningMatrices: node is not a SkinnedMeshNode");
        return 0;
    }
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm) {
        ev::throwTypeError("setSkinningMatrices on plain mesh throws");
        return 0;
    }
    Value mv = ev::fromBits(matsBits);
    std::vector<float> mats;
    if (!readFloatVector(mv, mats) || mats.empty() || (mats.size() % 16 != 0)) {
        ev::throwTypeError("setSkinningMatrices: expected Float32Array of 4x4 matrices");
        return 0;
    }
    return sm->setSkinningMatrices(mats.data(), mats.size() / 16);
}

uint64_t bro_scene_SceneNode_getBoneWorldMatrix(void* self, uint64_t argBits) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return ev::toBits(ev::null());
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !sm->player()) return ev::toBits(ev::null());

    float mat[16];
    bool ok = false;
    Value arg = ev::fromBits(argBits);
    if (ev::isNumber(arg)) {
        int32_t idx = satCast<int32_t>(ev::toDouble(arg));
        ok = sm->player()->boneWorldMatrix(idx, mat);
    } else if (ev::isString(arg)) {
        std::string name = ev::toUtf8(arg);
        ok = sm->player()->boneWorldMatrix(name, mat);
    }
    if (!ok) return ev::toBits(ev::null());

    return ev::toBits(makeFloat32Array(mat, 16));
}

const char* bro_scene_SceneNode_blendState(void* self) {
    static thread_local std::string tl_bsJson;
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return "{}";
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !sm->player()) return "{}";
    auto bs = sm->player()->blendState();
    json j;
    j["phase"] = bs.phase;
    if (bs.state.empty()) {
        j["state"] = nullptr;
    } else {
        j["state"] = bs.state;
    }
    j["clips"] = json::array();
    for (const auto& cw : bs.clips) {
        j["clips"].push_back({{"name", cw.name}, {"weight", cw.weight}});
    }
    j["layers"] = json::array();
    for (const auto& ls : bs.layers) {
        j["layers"].push_back({{"slot", ls.slot}, {"name", ls.name}, {"weight", ls.weight}, {"phase", ls.phase}});
    }
    if (bs.hasPos) {
        if (bs.is2D) {
            j["pos"] = {bs.pos[0], bs.pos[1]};
        } else {
            j["pos"] = {bs.pos[0]};
        }
    } else {
        j["pos"] = json::array();
    }
    tl_bsJson = j.dump();
    return tl_bsJson.c_str();
}

void bro_scene_SceneNode_onAnimationFinished_set(void* self, uint64_t cbBits) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm) return;
    Value cb = ev::fromBits(cbBits);
    if (ev::isFunction(cb)) {
        std::shared_ptr<ev::Persistent> fnRef = std::make_shared<ev::Persistent>(cb);
        sm->ensurePlayer().setOnFinished([fnRef](const std::string& name) {
            if (fnRef && ev::isFunction(fnRef->get())) {
                ev::Persistent nameVal(ev::fromUtf8(name));
                Value args[] = { nameVal.get() };
                ev::call(fnRef->get(), ev::undefined(), args);
            }
        });
    } else {
        if (sm->player()) sm->player()->setOnFinished(nullptr);
    }
}

void bro_scene_SceneNode_onStateChanged_set(void* self, uint64_t cbBits) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm) return;
    Value cb = ev::fromBits(cbBits);
    if (ev::isFunction(cb)) {
        std::shared_ptr<ev::Persistent> fnRef = std::make_shared<ev::Persistent>(cb);
        sm->ensurePlayer().setOnStateChanged([fnRef](const std::string& from, const std::string& to) {
            if (fnRef && ev::isFunction(fnRef->get())) {
                ev::Persistent fromVal(from.empty() ? ev::null() : ev::fromUtf8(from));
                ev::Persistent toVal(ev::fromUtf8(to));
                Value args[] = { fromVal.get(), toVal.get() };
                ev::call(fnRef->get(), ev::undefined(), args);
            }
        });
    } else {
        if (sm->player()) sm->player()->setOnStateChanged(nullptr);
    }
}

int32_t bro_scene_SceneNode_boneCount_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return 0;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    return sm ? sm->boneCount() : 0;
}

bool bro_scene_SceneNode_skinReady_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return false;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    return sm ? sm->skinReady() : false;
}

bool bro_scene_SceneNode_isPlaying_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return false;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    return (sm && sm->player()) ? sm->player()->isPlaying() : false;
}

const char* bro_scene_SceneNode_currentAnimation_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return "";
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    return (sm && sm->player()) ? sm->player()->currentClip().c_str() : "";
}

double bro_scene_SceneNode_animationDuration_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return 0.0;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    return (sm && sm->player()) ? static_cast<double>(sm->player()->duration()) : 0.0;
}

double bro_scene_SceneNode_animationTime_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return 0.0;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    return (sm && sm->player()) ? static_cast<double>(sm->player()->time()) : 0.0;
}

void bro_scene_SceneNode_animationTime_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player()) sm->player()->setTime(static_cast<float>(v));
}

double bro_scene_SceneNode_animationSpeed_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return 1.0;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    return (sm && sm->player()) ? static_cast<double>(sm->player()->speed()) : 1.0;
}

void bro_scene_SceneNode_animationSpeed_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player()) sm->player()->setSpeed(static_cast<float>(v));
}

const char* bro_scene_SceneNode_state_get(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return "";
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !sm->player() || !sm->player()->hasStateMachine()) return "";
    const auto& s = sm->player()->currentState();
    return s.c_str();
}

void bro_scene_SceneNode_setInstanceTransform(void* self, int32_t index, const double* matrix, uint32_t matrix_len) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::InstancedMesh && matrix && matrix_len >= 16 && index >= 0) {
        auto* im = static_cast<scene::InstancedMeshNode*>(n);
        size_t idx = static_cast<size_t>(index);
        if (idx < im->instanceCount()) {
            // Only the transform changes: the record's tint (setInstanceColor,
            // or a variant index packed into alpha) is read back and kept.
            float rec[16];
            if (!im->instanceRecord(idx, rec)) {
                std::memset(rec, 0, sizeof(rec));
                rec[12] = 1.0f; rec[13] = 1.0f; rec[14] = 1.0f; rec[15] = 1.0f;
            }
            rec[0] = static_cast<float>(matrix[0]);  rec[1] = static_cast<float>(matrix[4]);  rec[2] = static_cast<float>(matrix[8]);   rec[3] = static_cast<float>(matrix[12]);
            rec[4] = static_cast<float>(matrix[1]);  rec[5] = static_cast<float>(matrix[5]);  rec[6] = static_cast<float>(matrix[9]);   rec[7] = static_cast<float>(matrix[13]);
            rec[8] = static_cast<float>(matrix[2]);  rec[9] = static_cast<float>(matrix[6]);  rec[10] = static_cast<float>(matrix[10]); rec[11] = static_cast<float>(matrix[14]);
            im->updateInstance(idx, rec);
        }
    }
}

void bro_scene_SceneNode_setInstanceColor(void* self, int32_t index, const double* color, uint32_t color_len) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::InstancedMesh && color && color_len >= 3 && index >= 0) {
        auto* im = static_cast<scene::InstancedMeshNode*>(n);
        size_t idx = static_cast<size_t>(index);
        if (idx < im->instanceCount()) {
            float rec[16];
            float rows[12];
            if (im->instanceRows(idx, rows)) {
                for (int k = 0; k < 12; ++k) rec[k] = rows[k];
            } else {
                std::memset(rec, 0, sizeof(rec));
                rec[0] = rec[5] = rec[10] = 1.0f;
            }
            rec[12] = static_cast<float>(color[0]);
            rec[13] = static_cast<float>(color[1]);
            rec[14] = static_cast<float>(color[2]);
            rec[15] = (color_len >= 4) ? static_cast<float>(color[3]) : 1.0f;
            im->updateInstance(idx, rec);
        }
    }
}

void bro_scene_SceneNode_setInstanceCount(void* self, int32_t count) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::InstancedMesh && count >= 0) {
        auto* im = static_cast<scene::InstancedMeshNode*>(n);
        size_t newCount = static_cast<size_t>(count);
        std::vector<float> data(newCount * 16, 0.0f);
        for (size_t i = 0; i < newCount; ++i) {
            float rows[12];
            if (im->instanceRows(i, rows)) {
                for (int k = 0; k < 12; ++k) data[i * 16 + k] = rows[k];
                data[i * 16 + 12] = 1.0f; data[i * 16 + 13] = 1.0f; data[i * 16 + 14] = 1.0f; data[i * 16 + 15] = 1.0f;
            } else {
                data[i * 16 + 0] = data[i * 16 + 5] = data[i * 16 + 10] = 1.0f;
                data[i * 16 + 12] = data[i * 16 + 13] = data[i * 16 + 14] = data[i * 16 + 15] = 1.0f;
            }
        }
        im->setInstances(data.data(), newCount);
    }
}

void bro_scene_SceneNode_setInstances(void* self, const float* data, uint32_t count) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::InstancedMesh && data && count >= 16) {
        static_cast<scene::InstancedMeshNode*>(n)->setInstances(data, count / 16);
    }
}

double bro_scene_SceneNode_instanceCount_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::InstancedMesh) {
        return static_cast<double>(static_cast<scene::InstancedMeshNode*>(n)->instanceCount());
    }
    return 0.0;
}

void* bro_scene_SceneNode_setHtml(void* self, const char* html) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Html && html) {
        static_cast<scene::HtmlNode*>(n)->setHtml(html);
    }
    // Declared to return a SceneNode, so bronze mints a new OWNING handle
    // over the pointer; answering `self` would have two handles delete one
    // cell (see chained() in native_scene_anim.cpp). A fresh cell resolves
    // to the same node.
    auto* c = nodeCellOf(self);
    return c ? new HostSceneNodeCell(*c) : nullptr;
}

void bro_scene_SceneNode_markHtmlDirty(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Html) {
        static_cast<scene::HtmlNode*>(n)->markHtmlDirty();
    }
}

void bro_scene_SceneNode_burst(void* self, int32_t count) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(n)->burst(count);
    } else if (n->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(n)->burst(count);
    }
}

void bro_scene_SceneNode_clear(void* self) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(n)->clear();
    } else if (n->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(n)->clear();
    }
}

void bro_scene_SceneNode_probeCapture(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
        static_cast<scene::ReflectionProbeNode*>(n)->requestCapture();
    }
}

void bro_scene_SceneNode_setInstancesFromTransforms(void* self, const float* data, uint32_t count) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::InstancedMesh && data && count >= 9) {
        static_cast<scene::InstancedMeshNode*>(n)->setInstancesFromPosQuatScale(data, count / 9);
    }
}

const char* bro_scene_SceneNode_updateMode_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
        return static_cast<scene::ReflectionProbeNode*>(n)->updateMode() ==
               scene::ReflectionProbeNode::UpdateMode::Once ? "once" : "manual";
    }
    return "";
}

void bro_scene_SceneNode_updateMode_set(void* self, const char* mode) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::ReflectionProbe && mode) {
        auto* rp = static_cast<scene::ReflectionProbeNode*>(n);
        if (std::strcmp(mode, "manual") == 0) {
            rp->setUpdateMode(scene::ReflectionProbeNode::UpdateMode::Manual);
        } else {
            rp->setUpdateMode(scene::ReflectionProbeNode::UpdateMode::Once);
        }
    }
}

int32_t bro_scene_SceneNode_resolution_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
        return static_cast<scene::ReflectionProbeNode*>(n)->resolution();
    }
    return 0;
}

void bro_scene_SceneNode_resolution_set(void* self, int32_t res) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
        static_cast<scene::ReflectionProbeNode*>(n)->setResolution(res);
    }
}

bool bro_scene_SceneNode_boxProjection_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
        return static_cast<scene::ReflectionProbeNode*>(n)->boxProjection();
    }
    return true;
}

void bro_scene_SceneNode_boxProjection_set(void* self, bool bp) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
        static_cast<scene::ReflectionProbeNode*>(n)->setBoxProjection(bp);
    }
}

bool bro_scene_SceneNode_savePly(void* self, const char* path) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::GaussianSplat && path) {
        auto* sn = static_cast<scene::GaussianSplatNode*>(n);
        if (sn->splatCount() == 0) return false;
        return bromesh::saveSplatPLY(sn->cloud(), path);
    }
    return false;
}

int32_t bro_scene_SceneNode_splatCount_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::GaussianSplat) {
        return static_cast<int32_t>(static_cast<scene::GaussianSplatNode*>(n)->splatCount());
    }
    return 0;
}

void bro_scene_SceneNode_setCloud(void* self,
                                  const float* pos, uint32_t posCount,
                                  const float* scales, uint32_t scaleCount,
                                  const float* rots, uint32_t rotCount,
                                  const float* opacities, uint32_t opCount,
                                  const float* sh, uint32_t shCount,
                                  int32_t shDegree) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::GaussianSplat) return;
    auto* sn = static_cast<scene::GaussianSplatNode*>(n);
    bromesh::GaussianSplatCloud cloud;
    if (pos && posCount > 0) cloud.positions.assign(pos, pos + posCount);
    if (scales && scaleCount > 0) cloud.scales.assign(scales, scales + scaleCount);
    if (rots && rotCount > 0) cloud.rotations.assign(rots, rots + rotCount);
    if (opacities && opCount > 0) cloud.opacities.assign(opacities, opacities + opCount);
    if (sh && shCount > 0) cloud.sh.assign(sh, sh + shCount);
    cloud.shDegree = shDegree;
    sn->setCloud(std::move(cloud));
}

bool bro_scene_SceneNode_loadSplatPly(void* self, const char* path) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::GaussianSplat || !path) return false;
    auto* sn = static_cast<scene::GaussianSplatNode*>(n);
    auto cloud = bromesh::loadSplatPLY(path);
    if (cloud.empty()) return false;
    sn->setCloud(std::move(cloud));
    return true;
}

}  // extern "C"

