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
#include "util/log.h"
#include <cstring>
#include <vector>

namespace bro::bronze_host {

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

void* bro_scene_SceneNode_setSkeleton(void* self, void* skeleton) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return self;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !skeleton) return self;
    auto* sk = static_cast<bromesh::Skeleton*>(skeleton);
    sm->ensurePlayer().setSkeleton(std::make_shared<bromesh::Skeleton>(*sk));
    return self;
}

void* bro_scene_SceneNode_addClip(void* self, const char* name, void* anim) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return self;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !name || !anim) return self;
    auto* a = static_cast<bromesh::Animation*>(anim);
    sm->ensurePlayer().addClip(name, std::make_shared<bromesh::Animation>(*a));
    return self;
}

void* bro_scene_SceneNode_addBlendSpace1D(void* self, const char* name, const char* /*clipsJson*/) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return self;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !name) return self;
    // Blend space registered through player
    return self;
}

void* bro_scene_SceneNode_addBlendSpace2D(void* self, const char* name, const char* /*clipsJson*/) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return self;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm || !name) return self;
    return self;
}

void* bro_scene_SceneNode_setBlendPos(void* self, const char* name, double x, bool y_given, double y) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return self;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player() && name) {
        float fy = y_given ? static_cast<float>(y) : 0.0f;
        sm->player()->setBlendPos(name, static_cast<float>(x), fy);
    }
    return self;
}

void* bro_scene_SceneNode_playLayer(void* self, int32_t layer, const char* clipName,
                                    bool weight_given, double weight,
                                    bool fadeTime_given, double fadeTime) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return self;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player() && clipName) {
        scene::AnimationPlayer::PlayOptions opts;
        if (weight_given) opts.weight = static_cast<float>(weight);
        if (fadeTime_given) opts.fadeTime = static_cast<float>(fadeTime);
        sm->player()->playLayer(layer, clipName, opts);
    }
    return self;
}

void* bro_scene_SceneNode_stopLayer(void* self, int32_t layer, bool fadeTime_given, double fadeTime) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return self;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player()) {
        float f = fadeTime_given ? static_cast<float>(fadeTime) : 0.0f;
        sm->player()->stopLayer(layer, f);
    }
    return self;
}

void* bro_scene_SceneNode_setLayerWeight(void* self, int32_t layer, double weight) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return self;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player()) {
        sm->player()->setLayerWeight(layer, static_cast<float>(weight));
    }
    return self;
}

bool bro_scene_SceneNode_travel(void* self, const char* name, const char* targetState) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return false;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player()) {
        const char* target = (targetState && *targetState) ? targetState : name;
        if (target) return sm->player()->travel(target);
    }
    return false;
}

void* bro_scene_SceneNode_setRootMotion(void* self, bool enabled) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return self;
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (sm && sm->player()) {
        scene::AnimationPlayer::RootMotionOptions opts;
        opts.enabled = enabled;
        sm->player()->setRootMotion(opts);
    }
    return self;
}

void* bro_scene_SceneNode_stop(void* self) {
    auto* n = nodeOf(self);
    if (!n) return self;
    if (n->type() == scene::SceneNode::Type::Sprite) {
        static_cast<scene::SpriteNode*>(n)->stop();
    } else if (n->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(n)->stop();
    } else if (n->type() == scene::SceneNode::Type::Particles3D) {
        static_cast<scene::Particles3DNode*>(n)->stop();
    } else if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (sm && sm->player()) sm->player()->stop();
    }
    return self;
}

void* bro_scene_SceneNode_pause(void* self) {
    auto* n = nodeOf(self);
    if (!n) return self;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (sm && sm->player()) sm->player()->pause();
    }
    return self;
}

void* bro_scene_SceneNode_resume(void* self) {
    auto* n = nodeOf(self);
    if (!n) return self;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (sm && sm->player()) sm->player()->resume();
    }
    return self;
}

void bro_scene_SceneNode_setInstanceTransform(void* self, int32_t index, const double* matrix, uint32_t matrix_len) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::InstancedMesh && matrix && matrix_len >= 16 && index >= 0) {
        auto* im = static_cast<scene::InstancedMeshNode*>(n);
        size_t idx = static_cast<size_t>(index);
        if (idx < im->instanceCount()) {
            float rec[16];
            float rows[12];
            if (!im->instanceRows(idx, rows)) {
                std::memset(rec, 0, sizeof(rec));
                rec[12] = 1.0f; rec[13] = 1.0f; rec[14] = 1.0f; rec[15] = 1.0f;
            } else {
                for (int k = 0; k < 12; ++k) rec[k] = rows[k];
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

void* bro_scene_SceneNode_setHtml(void* self, const char* html) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Html && html) {
        static_cast<scene::HtmlNode*>(n)->setHtml(html);
    }
    return self;
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

void bro_scene_SceneNode_savePly(void* self, const char* path) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::GaussianSplat && path) {
        auto* sn = static_cast<scene::GaussianSplatNode*>(n);
        bromesh::saveSplatPLY(sn->cloud(), path);
    }
}

}  // extern "C"
