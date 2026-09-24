// native_scene_nodes.cpp — SceneNode properties, hierarchy, and transforms.

#include "bronze_host/native_scene_internal.h"
#include "natives/scene/native_scene_decl.h"

namespace bro::bronze_host {

namespace {

static thread_local std::vector<scene::SceneNode*> tl_childrenList;
static thread_local scene::SceneGraph* tl_childrenGraph = nullptr;
static thread_local double tl_vec3Buf[3];
static thread_local double tl_vec4Buf[4];
static thread_local double tl_mat4Buf[16];

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

int32_t bro_scene_SceneNode_id_get(void* self) {
    auto* n = nodeOf(self);
    return n ? static_cast<int32_t>(n->id()) : 0;
}

const char* bro_scene_SceneNode_name_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->name().c_str() : "";
}

void bro_scene_SceneNode_name_set(void* self, const char* v) {
    auto* n = nodeOf(self);
    if (n && v) n->setName(v);
}

bool bro_scene_SceneNode_visible_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->visible() : false;
}

void bro_scene_SceneNode_visible_set(void* self, bool v) {
    auto* n = nodeOf(self);
    if (n) n->setVisible(v);
}

double bro_scene_SceneNode_x_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->position().x : 0.0;
}

void bro_scene_SceneNode_x_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n) {
        auto p = n->position();
        p.x = static_cast<float>(v);
        n->setPosition(p);
    }
}

double bro_scene_SceneNode_y_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->position().y : 0.0;
}

void bro_scene_SceneNode_y_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n) {
        auto p = n->position();
        p.y = static_cast<float>(v);
        n->setPosition(p);
    }
}

double bro_scene_SceneNode_z_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->position().z : 0.0;
}

void bro_scene_SceneNode_z_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n) {
        auto p = n->position();
        p.z = static_cast<float>(v);
        n->setPosition(p);
    }
}

void bro_scene_SceneNode_position_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (!n) {
        copyBuffer<double>(nullptr, 0, out);
        return;
    }
    const auto& p = n->position();
    tl_vec3Buf[0] = p.x;
    tl_vec3Buf[1] = p.y;
    tl_vec3Buf[2] = p.z;
    copyBuffer(tl_vec3Buf, 3, out);
}

void bro_scene_SceneNode_position_set(void* self, const double* v, uint32_t v_len) {
    auto* n = nodeOf(self);
    if (n && v && v_len >= 3) {
        n->setPosition(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));
    }
}

void bro_scene_SceneNode_rotation_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (!n) {
        copyBuffer<double>(nullptr, 0, out);
        return;
    }
    const auto& r = n->rotation();
    tl_vec4Buf[0] = r.x;
    tl_vec4Buf[1] = r.y;
    tl_vec4Buf[2] = r.z;
    tl_vec4Buf[3] = r.w;
    copyBuffer(tl_vec4Buf, 4, out);
}

void bro_scene_SceneNode_rotation_set(void* self, const double* v, uint32_t v_len) {
    auto* n = nodeOf(self);
    if (n && v && v_len >= 4) {
        n->setRotation(bromath::Quat(static_cast<float>(v[0]), static_cast<float>(v[1]),
                                     static_cast<float>(v[2]), static_cast<float>(v[3])));
    }
}

double bro_scene_SceneNode_rotationX_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->rotationEuler().x : 0.0;
}

void bro_scene_SceneNode_rotationX_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n) {
        auto e = n->rotationEuler();
        n->setRotationEuler(static_cast<float>(v), e.y, e.z);
    }
}

double bro_scene_SceneNode_rotationY_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->rotationEuler().y : 0.0;
}

void bro_scene_SceneNode_rotationY_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n) {
        auto e = n->rotationEuler();
        n->setRotationEuler(e.x, static_cast<float>(v), e.z);
    }
}

double bro_scene_SceneNode_rotationZ_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->rotationEuler().z : 0.0;
}

void bro_scene_SceneNode_rotationZ_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n) {
        auto e = n->rotationEuler();
        n->setRotationEuler(e.x, e.y, static_cast<float>(v));
    }
}

void bro_scene_SceneNode_scale_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (!n) {
        copyBuffer<double>(nullptr, 0, out);
        return;
    }
    const auto& s = n->scale();
    tl_vec3Buf[0] = s.x;
    tl_vec3Buf[1] = s.y;
    tl_vec3Buf[2] = s.z;
    copyBuffer(tl_vec3Buf, 3, out);
}

void bro_scene_SceneNode_scale_set(void* self, const double* v, uint32_t v_len) {
    auto* n = nodeOf(self);
    if (n && v && v_len >= 3) {
        n->setScale(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));
    }
}

double bro_scene_SceneNode_scaleX_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->scale().x : 1.0;
}

void bro_scene_SceneNode_scaleX_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n) {
        auto s = n->scale();
        s.x = static_cast<float>(v);
        n->setScale(s);
    }
}

double bro_scene_SceneNode_scaleY_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->scale().y : 1.0;
}

void bro_scene_SceneNode_scaleY_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n) {
        auto s = n->scale();
        s.y = static_cast<float>(v);
        n->setScale(s);
    }
}

double bro_scene_SceneNode_scaleZ_get(void* self) {
    auto* n = nodeOf(self);
    return n ? n->scale().z : 1.0;
}

void bro_scene_SceneNode_scaleZ_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n) {
        auto s = n->scale();
        s.z = static_cast<float>(v);
        n->setScale(s);
    }
}

void bro_scene_SceneNode_quaternion_get(void* self, bronze_native_buffer* out) {
    bro_scene_SceneNode_rotation_get(self, out);
}

void bro_scene_SceneNode_quaternion_set(void* self, const double* v, uint32_t v_len) {
    bro_scene_SceneNode_rotation_set(self, v, v_len);
}


void bro_scene_SceneNode_worldPosition_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (!n) {
        copyBuffer<double>(nullptr, 0, out);
        return;
    }
    const auto p = n->localToWorld({0.0f, 0.0f, 0.0f});
    tl_vec3Buf[0] = p.x;
    tl_vec3Buf[1] = p.y;
    tl_vec3Buf[2] = p.z;
    copyBuffer(tl_vec3Buf, 3, out);
}

void bro_scene_SceneNode_worldMatrix_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (!n) {
        copyBuffer<double>(nullptr, 0, out);
        return;
    }
    const auto& m = n->worldMatrix();
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            tl_mat4Buf[col * 4 + row] = m.at(row, col);
        }
    }
    copyBuffer(tl_mat4Buf, 16, out);
}

void* bro_scene_SceneNode_parent_get(void* self) {
    auto* c = static_cast<HostSceneNodeCell*>(self);
    if (!c || c->tag != kHostSceneNodeTag) return nullptr;
    auto* n = c->node();
    return (n && n->parent()) ? wrapNode(n->parent(), c->graph()) : nullptr;
}

int32_t bro_scene_SceneNode_children_get(void* self) {
    tl_childrenList.clear();
    auto* c = nodeCellOf(self);
    tl_childrenGraph = c ? c->graph() : nullptr;
    auto* n = c ? c->node() : nullptr;
    if (n) {
        for (auto* ch : n->children()) {
            if (ch) tl_childrenList.push_back(ch);
        }
    }
    return static_cast<int32_t>(tl_childrenList.size());
}

void* bro_scene_SceneNode_children_get_at(int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= tl_childrenList.size()) return nullptr;
    auto* ch = tl_childrenList[index];
    return ch ? wrapNode(ch, tl_childrenGraph) : nullptr;
}

void* bro_scene_SceneNode_add(void* self, void* child) {
    auto* parent = nodeOf(self);
    auto* ch = nodeOf(child);
    if (parent && ch) parent->addChild(ch);
    return nullptr;
}

void bro_scene_SceneNode_remove(void* self, void* child) {
    auto* parent = nodeOf(self);
    auto* ch = nodeOf(child);
    if (parent && ch) parent->removeChild(ch);
}

void* bro_scene_SceneNode_addChild(void* self, void* child) {
    return bro_scene_SceneNode_add(self, child);
}

void bro_scene_SceneNode_removeChild(void* self, void* child) {
    bro_scene_SceneNode_remove(self, child);
}

void bro_scene_SceneNode_destroy(void* self) {
    auto* c = static_cast<HostSceneNodeCell*>(self);
    if (c && c->tag == kHostSceneNodeTag) {
        auto* g = c->graph();
        auto* n = c->node();
        if (g && n) g->destroyNode(n);
    }
}

void* bro_scene_SceneNode_setPosition(void* self, double x, double y, double z) {
    auto* n = nodeOf(self);
    if (n) n->setPosition(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    return nullptr;
}

void* bro_scene_SceneNode_setRotation(void* self, double x, double y, double z, bool w_given, double w) {
    auto* n = nodeOf(self);
    if (n) {
        if (w_given) {
            n->setRotation(bromath::Quat(static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(z), static_cast<float>(w)));
        } else {
            // Euler angles in degrees
            constexpr float kDeg2Rad = 0.017453292519943295f;
            n->setRotationEuler(static_cast<float>(x) * kDeg2Rad,
                                static_cast<float>(y) * kDeg2Rad,
                                static_cast<float>(z) * kDeg2Rad);
        }
    }
    return nullptr;
}

void* bro_scene_SceneNode_setScale(void* self, double x, bool y_given, double y, bool z_given, double z) {
    auto* n = nodeOf(self);
    if (n) {
        float fx = static_cast<float>(x);
        float fy = y_given ? static_cast<float>(y) : fx;
        float fz = z_given ? static_cast<float>(z) : fx;
        n->setScale(fx, fy, fz);
    }
    return nullptr;
}

void* bro_scene_SceneNode_lookAt(void* self, const double* target, uint32_t target_len, const double* up, uint32_t up_len) {
    auto* n = nodeOf(self);
    if (n && target && target_len >= 3) {
        bromath::Vec3 tgt{static_cast<float>(target[0]), static_cast<float>(target[1]), static_cast<float>(target[2])};
        bromath::Vec3 upVec{0.0f, 1.0f, 0.0f};
        if (up && up_len >= 3) {
            upVec = {static_cast<float>(up[0]), static_cast<float>(up[1]), static_cast<float>(up[2])};
        }
        n->lookAt(tgt, upVec);
    }
    return nullptr;
}

const char* bro_scene_SceneNode_type_get(void* self) {
    auto* n = nodeOf(self);
    if (!n) return "none";
    switch (n->type()) {
        case scene::SceneNode::Type::Mesh: {
            auto* mn = static_cast<scene::MeshNode*>(n);
            return mn->asSkinnedMesh() ? "skinnedMesh" : "mesh";
        }
        case scene::SceneNode::Type::InstancedMesh: return "instancedMesh";
        case scene::SceneNode::Type::Light: return "light";
        case scene::SceneNode::Type::Camera: return "camera";
        case scene::SceneNode::Type::Physics: return "physics";
        case scene::SceneNode::Type::Shape: return "shape";
        case scene::SceneNode::Type::Sprite: return "sprite";
        case scene::SceneNode::Type::Html: return "html";
        case scene::SceneNode::Type::Particles: return "particles";
        case scene::SceneNode::Type::Particles3D: return "particles3d";
        case scene::SceneNode::Type::GaussianSplat: return "gaussianSplat";
        case scene::SceneNode::Type::Decal: return "decal";
        case scene::SceneNode::Type::ReflectionProbe: return "reflectionProbe";
        default: return "node";
    }
}

const char* bro_scene_SceneNode_kind_get(void* self) {
    auto* n = nodeOf(self);
    if (!n) return "";
    if (n->type() == scene::SceneNode::Type::Light) {
        auto* l = static_cast<scene::LightNode*>(n);
        switch (l->kind()) {
            case scene::LightNode::Kind::Directional: return "directional";
            case scene::LightNode::Kind::Point: return "point";
            case scene::LightNode::Kind::Spot: return "spot";
        }
    }
    return "";
}

int32_t bro_scene_SceneNode_childCount_get(void* self) {
    auto* n = nodeOf(self);
    return n ? static_cast<int32_t>(n->children().size()) : 0;
}

bool bro_scene_SceneNode_castsShadow_get(void* self) {
    auto* n = nodeOf(self);
    if (!n) return false;
    if (n->type() == scene::SceneNode::Type::Light) {
        return static_cast<scene::LightNode*>(n)->castsShadow();
    }
    if (n->type() == scene::SceneNode::Type::Mesh) {
        return static_cast<scene::MeshNode*>(n)->castsShadow();
    }
    if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        return static_cast<scene::InstancedMeshNode*>(n)->castsShadow();
    }
    return false;
}

void bro_scene_SceneNode_castsShadow_set(void* self, bool v) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setCastsShadow(v);
    } else if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setCastsShadow(v);
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        static_cast<scene::InstancedMeshNode*>(n)->setCastsShadow(v);
    }
}

bool bro_scene_SceneNode_receivesShadow_get(void* self) {
    auto* n = nodeOf(self);
    if (!n) return false;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        return static_cast<scene::MeshNode*>(n)->receivesShadow();
    }
    if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        return static_cast<scene::InstancedMeshNode*>(n)->receivesShadow();
    }
    return false;
}

void bro_scene_SceneNode_receivesShadow_set(void* self, bool v) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setReceivesShadow(v);
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        static_cast<scene::InstancedMeshNode*>(n)->setReceivesShadow(v);
    }
}

double bro_scene_SceneNode_metallic_get(void* self) {
    auto* n = nodeOf(self);
    if (n) {
        if (n->type() == scene::SceneNode::Type::Mesh) {
            return static_cast<double>(static_cast<scene::MeshNode*>(n)->metallic());
        }
        if (n->type() == scene::SceneNode::Type::InstancedMesh) {
            return static_cast<double>(static_cast<scene::InstancedMeshNode*>(n)->metallic());
        }
    }
    return 0.0;
}

void bro_scene_SceneNode_metallic_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setMetallic(static_cast<float>(v));
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        static_cast<scene::InstancedMeshNode*>(n)->setMetallic(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_roughness_get(void* self) {
    auto* n = nodeOf(self);
    if (n) {
        if (n->type() == scene::SceneNode::Type::Mesh) {
            return static_cast<double>(static_cast<scene::MeshNode*>(n)->roughness());
        }
        if (n->type() == scene::SceneNode::Type::InstancedMesh) {
            return static_cast<double>(static_cast<scene::InstancedMeshNode*>(n)->roughness());
        }
    }
    return 0.7;
}

void bro_scene_SceneNode_roughness_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setRoughness(static_cast<float>(v));
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        static_cast<scene::InstancedMeshNode*>(n)->setRoughness(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_emissive_get(void* self) {
    auto* n = nodeOf(self);
    if (n) {
        if (n->type() == scene::SceneNode::Type::Mesh) {
            return static_cast<double>(static_cast<scene::MeshNode*>(n)->emissive());
        }
        if (n->type() == scene::SceneNode::Type::InstancedMesh) {
            return static_cast<double>(static_cast<scene::InstancedMeshNode*>(n)->emissive());
        }
    }
    return 0.0;
}

void bro_scene_SceneNode_emissive_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setEmissive(static_cast<float>(v));
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        static_cast<scene::InstancedMeshNode*>(n)->setEmissive(static_cast<float>(v));
    }
}

void bro_scene_SceneNode_direction_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        const auto& d = static_cast<scene::LightNode*>(n)->direction();
        tl_vec3Buf[0] = d.x; tl_vec3Buf[1] = d.y; tl_vec3Buf[2] = d.z;
    } else {
        tl_vec3Buf[0] = 0; tl_vec3Buf[1] = -1; tl_vec3Buf[2] = 0;
    }
    copyBuffer(tl_vec3Buf, 3, out);
}

void bro_scene_SceneNode_direction_set(void* self, const double* v, uint32_t len) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light && v && len >= 3) {
        static_cast<scene::LightNode*>(n)->setDirection({static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])});
    }
}

void bro_scene_SceneNode_color_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        const auto& c = static_cast<scene::LightNode*>(n)->color();
        tl_vec3Buf[0] = c.x; tl_vec3Buf[1] = c.y; tl_vec3Buf[2] = c.z;
    } else if (n && n->type() == scene::SceneNode::Type::Mesh) {
        const auto* c = static_cast<scene::MeshNode*>(n)->color();
        tl_vec3Buf[0] = c[0]; tl_vec3Buf[1] = c[1]; tl_vec3Buf[2] = c[2];
    } else if (n && n->type() == scene::SceneNode::Type::InstancedMesh) {
        const auto* c = static_cast<scene::InstancedMeshNode*>(n)->color();
        tl_vec3Buf[0] = c[0]; tl_vec3Buf[1] = c[1]; tl_vec3Buf[2] = c[2];
    } else {
        tl_vec3Buf[0] = 1; tl_vec3Buf[1] = 1; tl_vec3Buf[2] = 1;
    }
    copyBuffer(tl_vec3Buf, 3, out);
}

void bro_scene_SceneNode_color_set(void* self, const double* v, uint32_t len) {
    auto* n = nodeOf(self);
    if (!n || !v || len < 3) return;
    if (n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setColor(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));
    } else if (n->type() == scene::SceneNode::Type::Mesh) {
        if (len >= 4) {
            static_cast<scene::MeshNode*>(n)->setColor(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3]));
        } else {
            static_cast<scene::MeshNode*>(n)->setColor(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), 1.0f);
        }
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        if (len >= 4) {
            static_cast<scene::InstancedMeshNode*>(n)->setColor(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3]));
        } else {
            static_cast<scene::InstancedMeshNode*>(n)->setColor(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), 1.0f);
        }
    } else if (n->type() == scene::SceneNode::Type::Shape) {
        float a = len >= 4 ? static_cast<float>(v[3]) : 1.0f;
        static_cast<scene::ShapeNode*>(n)->setFillColor(bromath::Color{
            static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), a
        });
    }
}

double bro_scene_SceneNode_cullMargin_get(void* self) {
    auto* n = nodeOf(self);
    if (!n) return 0.0;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        return static_cast<double>(static_cast<scene::MeshNode*>(n)->cullMargin());
    }
    if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        return static_cast<double>(static_cast<scene::InstancedMeshNode*>(n)->cullMargin());
    }
    return 0.0;
}

void bro_scene_SceneNode_cullMargin_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (!n) return;
    float m = static_cast<float>(v);
    if (n->type() == scene::SceneNode::Type::Mesh) {
        static_cast<scene::MeshNode*>(n)->setCullMargin(m);
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        static_cast<scene::InstancedMeshNode*>(n)->setCullMargin(m);
    }
}

double bro_scene_SceneNode_intensity_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        return static_cast<double>(static_cast<scene::LightNode*>(n)->intensity());
    }
    if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
        return static_cast<double>(static_cast<scene::ReflectionProbeNode*>(n)->intensity());
    }
    return 1.0;
}

void bro_scene_SceneNode_intensity_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setIntensity(static_cast<float>(v));
    } else if (n && n->type() == scene::SceneNode::Type::ReflectionProbe) {
        static_cast<scene::ReflectionProbeNode*>(n)->setIntensity(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_range_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        return static_cast<double>(static_cast<scene::LightNode*>(n)->range());
    }
    return 10.0;
}

void bro_scene_SceneNode_range_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setRange(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_innerAngle_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        return static_cast<double>(static_cast<scene::LightNode*>(n)->innerAngle());
    }
    return 0.35;
}

void bro_scene_SceneNode_innerAngle_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setInnerAngle(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_outerAngle_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        return static_cast<double>(static_cast<scene::LightNode*>(n)->outerAngle());
    }
    return 0.52;
}

void bro_scene_SceneNode_outerAngle_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setOuterAngle(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_shadowBias_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        return static_cast<double>(static_cast<scene::LightNode*>(n)->shadowBias());
    }
    return 0.0;
}

void bro_scene_SceneNode_shadowBias_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setShadowBias(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_shadowNormalBias_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        return static_cast<double>(static_cast<scene::LightNode*>(n)->shadowNormalBias());
    }
    return 0.03;
}

void bro_scene_SceneNode_shadowNormalBias_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setShadowNormalBias(static_cast<float>(v));
    }
}

int32_t bro_scene_SceneNode_cascadeCount_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        return static_cast<scene::LightNode*>(n)->cascadeCount();
    }
    return 4;
}

void bro_scene_SceneNode_cascadeCount_set(void* self, int32_t v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setCascadeCount(v);
    }
}

double bro_scene_SceneNode_cascadeSplitLambda_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        return static_cast<double>(static_cast<scene::LightNode*>(n)->cascadeSplitLambda());
    }
    return 0.5;
}

void bro_scene_SceneNode_cascadeSplitLambda_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Light) {
        static_cast<scene::LightNode*>(n)->setCascadeSplitLambda(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_fov_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        return static_cast<scene::CameraNode*>(n)->fovDegrees();
    }
    return 60.0;
}

void bro_scene_SceneNode_fov_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        static_cast<scene::CameraNode*>(n)->setFovDegrees(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_near_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        return static_cast<scene::CameraNode*>(n)->nearZ();
    }
    return 0.1;
}

void bro_scene_SceneNode_near_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        static_cast<scene::CameraNode*>(n)->setNearZ(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_far_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        return static_cast<scene::CameraNode*>(n)->farZ();
    }
    return 1000.0;
}

void bro_scene_SceneNode_far_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        static_cast<scene::CameraNode*>(n)->setFarZ(static_cast<float>(v));
    }
}

const char* bro_scene_SceneNode_projection_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        return static_cast<scene::CameraNode*>(n)->perspective() ? "perspective" : "orthographic";
    }
    return "perspective";
}

void bro_scene_SceneNode_projection_set(void* self, const char* v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        static_cast<scene::CameraNode*>(n)->setPerspective(!v || std::string_view(v) == "perspective");
    }
}

double bro_scene_SceneNode_aspect_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        return static_cast<scene::CameraNode*>(n)->aspect();
    }
    return 0.0;
}

void bro_scene_SceneNode_aspect_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        static_cast<scene::CameraNode*>(n)->setAspect(static_cast<float>(v));
    }
}

double bro_scene_SceneNode_orthoHeight_get(void* self) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        return static_cast<scene::CameraNode*>(n)->orthoHeight();
    }
    return 10.0;
}

void bro_scene_SceneNode_orthoHeight_set(void* self, double v) {
    auto* n = nodeOf(self);
    if (n && n->type() == scene::SceneNode::Type::Camera) {
        static_cast<scene::CameraNode*>(n)->setOrthoHeight(static_cast<float>(v));
    }
}

void bro_scene_SceneNode_worldAnchor_get(void* self, bronze_native_buffer* out) {
    auto* n = nodeOf(self);
    if (n && n->hasWorldAnchor()) {
        const auto& a = n->worldAnchor();
        tl_vec3Buf[0] = a.x; tl_vec3Buf[1] = a.y; tl_vec3Buf[2] = a.z;
    } else {
        tl_vec3Buf[0] = 0; tl_vec3Buf[1] = 0; tl_vec3Buf[2] = 0;
    }
    copyBuffer(tl_vec3Buf, 3, out);
}

void bro_scene_SceneNode_worldAnchor_set(void* self, const double* v, uint32_t len) {
    auto* n = nodeOf(self);
    if (!n) return;
    if (!v || len < 3) {
        n->clearWorldAnchor();
    } else {
        n->setWorldAnchor(bromath::Vec3{static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])});
    }
}

}  // extern "C"
