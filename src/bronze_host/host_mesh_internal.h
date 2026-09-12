#pragma once

#if BRO_WITH_3D

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bromath/vec.h"
#include "bromath/quat.h"
#include "bromath/mat.h"
#include "bromath/aabb.h"
#include "bromesh/mesh_data.h"
#include "bromesh/analysis/bvh.h"
#include "bromesh/analysis/bbox.h"
#include "bromesh/analysis/raycast.h"
#include "bromesh/analysis/bake_texture.h"
#include "bromesh/procedural/obstacle_field.h"
#include "bromesh/optimization/progressive.h"
#include "bromesh/manipulation/poly_mesh.h"
#include "bromesh/procedural/lsystem.h"
#include "bromesh/procedural/branches.h"
#include "bromesh/procedural/leaf_scatter.h"
#include "scene/scene_graph.h"
#include "scene/mesh_node.h"
#include "scene/skinned_mesh_node.h"

#include <memory>
#include <string>
#include <vector>
#include <span>
#include <filesystem>

namespace bro::bronze_host {

inline constexpr uint32_t kHostMeshTag            = 0x4D455348u;  // 'MESH'
inline constexpr uint32_t kHostMeshBVHTag         = 0x4D425648u;  // 'MBVH'
inline constexpr uint32_t kHostCapsuleFieldTag    = 0x43415053u;  // 'CAPS'
inline constexpr uint32_t kHostProgressiveMeshTag = 0x50524F47u;  // 'PROG'
inline constexpr uint32_t kHostPolyMeshTag        = 0x504F4C59u;  // 'POLY'
inline constexpr uint32_t kHostLSystemTag         = 0x4C535953u;  // 'LSYS'

struct HostMesh {
    uint32_t tag = kHostMeshTag;
    std::unique_ptr<bromesh::MeshData> mesh;
};

struct HostMeshBVH {
    uint32_t tag = kHostMeshBVHTag;
    std::unique_ptr<bromesh::MeshBVH> bvh;
};

struct HostCapsuleField {
    uint32_t tag = kHostCapsuleFieldTag;
    std::unique_ptr<bromesh::CapsuleField> field;
};

struct HostProgressiveMesh {
    uint32_t tag = kHostProgressiveMeshTag;
    std::unique_ptr<bromesh::ProgressiveMesh> pm;
};

struct HostPolyMesh {
    uint32_t tag = kHostPolyMeshTag;
    std::unique_ptr<bromesh::PolyMesh> pm;
};

struct HostLSystem {
    uint32_t tag = kHostLSystemTag;
    std::unique_ptr<bromesh::LSystem> ls;
    std::vector<bromesh::Module> axiom;
    HostLSystem() : ls(std::make_unique<bromesh::LSystem>()) {}
};

// HostClass instances
extern HostClass g_meshClass;
extern HostClass g_meshBVHClass;
extern HostClass g_capsuleFieldClass;
extern HostClass g_progressiveMeshClass;
extern HostClass g_polyMeshClass;
extern HostClass g_lSystemClass;

// Unwrappers
inline HostMesh* hostMeshOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostMesh*>(ev::handleData(v));
    return (p && p->tag == kHostMeshTag) ? p : nullptr;
}

inline bromesh::MeshData* hostMeshDataOf(Value v) {
    auto* hm = hostMeshOf(v);
    return (hm && hm->mesh) ? hm->mesh.get() : nullptr;
}

inline std::unique_ptr<bromesh::MeshData> takeMeshDataOf(Value v) {
    auto* hm = hostMeshOf(v);
    if (!hm || !hm->mesh) return nullptr;
    return std::move(hm->mesh);
}

inline HostMeshBVH* hostMeshBVHOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostMeshBVH*>(ev::handleData(v));
    return (p && p->tag == kHostMeshBVHTag) ? p : nullptr;
}

inline HostCapsuleField* hostCapsuleFieldOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostCapsuleField*>(ev::handleData(v));
    return (p && p->tag == kHostCapsuleFieldTag) ? p : nullptr;
}

inline HostProgressiveMesh* hostProgressiveMeshOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostProgressiveMesh*>(ev::handleData(v));
    return (p && p->tag == kHostProgressiveMeshTag) ? p : nullptr;
}

inline HostPolyMesh* hostPolyMeshOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostPolyMesh*>(ev::handleData(v));
    return (p && p->tag == kHostPolyMeshTag) ? p : nullptr;
}

inline HostLSystem* hostLSystemOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostLSystem*>(ev::handleData(v));
    return (p && p->tag == kHostLSystemTag) ? p : nullptr;
}

// Wrappers
Value wrapMesh(std::unique_ptr<bromesh::MeshData> data);
Value wrapMesh(bromesh::MeshData&& data);
Value wrapMeshBVH(bromesh::MeshBVH&& bvh);
Value wrapCapsuleField(std::unique_ptr<bromesh::CapsuleField> field);
Value wrapProgressiveMesh(bromesh::ProgressiveMesh&& pm);
Value wrapPolyMesh(std::unique_ptr<bromesh::PolyMesh> pm);

// Helpers
inline bool readVec3(Value v, float out[3]) {
    if (ev::isUndefined(v) || ev::isNull(v)) return false;
    if (auto info = ev::typedArrayInfo(v)) {
        if (info.data && info.elementCount >= 3) {
            const float* fp = reinterpret_cast<const float*>(info.data);
            out[0] = fp[0]; out[1] = fp[1]; out[2] = fp[2];
            return true;
        }
    }
    if (ev::isObject(v)) {
        Value xVal = ev::getProperty(v, "x");
        if (ev::isNumber(xVal)) {
            out[0] = static_cast<float>(ev::toDouble(xVal));
            out[1] = static_cast<float>(ev::toDouble(ev::getProperty(v, "y")));
            out[2] = static_cast<float>(ev::toDouble(ev::getProperty(v, "z")));
            return true;
        }
        Value e0 = ev::getElement(v, 0);
        if (!ev::isUndefined(e0)) {
            out[0] = static_cast<float>(ev::toDouble(e0));
            out[1] = static_cast<float>(ev::toDouble(ev::getElement(v, 1)));
            out[2] = static_cast<float>(ev::toDouble(ev::getElement(v, 2)));
            return true;
        }
    }
    return false;
}

inline bromath::Vec3 readBmVec3(Value v) {
    float f[3] = {0.0f, 0.0f, 0.0f};
    readVec3(v, f);
    return {f[0], f[1], f[2]};
}

inline bool readFloatLike(Value v, std::vector<float>& out) {
    if (readFloatVector(v, out)) return true;
    return false;
}

inline bool readVec2List(Value v, std::vector<bromath::Vec2>& out) {
    out.clear();
    if (!ev::isObject(v)) return false;
    if (auto info = ev::typedArrayInfo(v)) {
        if (info.data && info.byteLength % 8 == 0) {
            const float* f = reinterpret_cast<const float*>(info.data);
            size_t n = info.byteLength / 8;
            out.resize(n);
            for (size_t i = 0; i < n; i++) out[i] = { f[2*i+0], f[2*i+1] };
            return true;
        }
    }
    Value lv = ev::getProperty(v, "length");
    if (!ev::isNumber(lv)) return false;
    uint32_t n = static_cast<uint32_t>(ev::toDouble(lv));
    out.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        Value elem = ev::getElement(v, i);
        float x = 0, y = 0;
        if (ev::isObject(elem)) {
            Value xVal = ev::getProperty(elem, "x");
            if (ev::isNumber(xVal)) {
                x = static_cast<float>(ev::toDouble(xVal));
                y = static_cast<float>(ev::toDouble(ev::getProperty(elem, "y")));
            } else {
                x = static_cast<float>(ev::toDouble(ev::getElement(elem, 0)));
                y = static_cast<float>(ev::toDouble(ev::getElement(elem, 1)));
            }
        }
        out[i] = {x, y};
    }
    return true;
}

inline bool readVec3List(Value v, std::vector<bromath::Vec3>& out) {
    out.clear();
    if (!ev::isObject(v)) return false;
    if (auto info = ev::typedArrayInfo(v)) {
        if (info.data && info.byteLength % 12 == 0) {
            const float* f = reinterpret_cast<const float*>(info.data);
            size_t n = info.byteLength / 12;
            out.resize(n);
            for (size_t i = 0; i < n; i++) out[i] = { f[3*i+0], f[3*i+1], f[3*i+2] };
            return true;
        }
    }
    Value lv = ev::getProperty(v, "length");
    if (!ev::isNumber(lv)) return false;
    uint32_t n = static_cast<uint32_t>(ev::toDouble(lv));
    out.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        Value elem = ev::getElement(v, i);
        float p[3] = {0,0,0};
        readVec3(elem, p);
        out[i] = {p[0], p[1], p[2]};
    }
    return true;
}

inline const bromesh::CapsuleField* readAvoidField(Value obj, const char* prop) {
    if (!ev::isObject(obj)) return nullptr;
    Value v = ev::getProperty(obj, prop);
    auto* h = hostCapsuleFieldOf(v);
    return (h && h->field) ? h->field.get() : nullptr;
}

inline bool readSpheres(Value v, std::vector<bromesh::Sphere>& out) {
    out.clear();
    if (!ev::isObject(v)) return false;
    Value lv = ev::getProperty(v, "length");
    if (!ev::isNumber(lv)) return false;
    uint32_t n = static_cast<uint32_t>(ev::toDouble(lv));
    out.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        Value o = ev::getElement(v, i);
        bromesh::Sphere s{};
        s.center = readBmVec3(ev::getProperty(o, "center"));
        Value rV = ev::getProperty(o, "radius");
        if (ev::isNumber(rV)) s.radius = static_cast<float>(ev::toDouble(rV));
        Value tV = ev::getProperty(o, "tag");
        if (ev::isNumber(tV)) s.tag = static_cast<int>(ev::toDouble(tV));
        out[i] = s;
    }
    return true;
}

inline bool readCapsules(Value v, std::vector<bromesh::Capsule>& out) {
    out.clear();
    if (!ev::isObject(v)) return false;
    Value lv = ev::getProperty(v, "length");
    if (!ev::isNumber(lv)) return false;
    uint32_t n = static_cast<uint32_t>(ev::toDouble(lv));
    out.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        Value o = ev::getElement(v, i);
        bromesh::Capsule c{};
        c.a = readBmVec3(ev::getProperty(o, "a"));
        c.b = readBmVec3(ev::getProperty(o, "b"));
        Value rV = ev::getProperty(o, "radius");
        if (ev::isNumber(rV)) c.radius = static_cast<float>(ev::toDouble(rV));
        Value tV = ev::getProperty(o, "tag");
        if (ev::isNumber(tV)) c.tag = static_cast<int>(ev::toDouble(tV));
        out[i] = c;
    }
    return true;
}

inline bool readBranchSegments(Value v, std::vector<bromesh::BranchSegment>& out) {
    out.clear();
    if (!ev::isObject(v)) return false;
    Value lv = ev::getProperty(v, "length");
    if (!ev::isNumber(lv)) return false;
    uint32_t n = static_cast<uint32_t>(ev::toDouble(lv));
    out.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        Value o = ev::getElement(v, i);
        bromesh::BranchSegment s{};
        Value pV = ev::getProperty(o, "parent");
        s.parent = ev::isNumber(pV) ? static_cast<int>(ev::toDouble(pV)) : -1;
        Value dV = ev::getProperty(o, "depth");
        s.depth = ev::isNumber(dV) ? static_cast<int>(ev::toDouble(dV)) : 0;
        Value rV = ev::getProperty(o, "radius");
        s.radius = ev::isNumber(rV) ? static_cast<float>(ev::toDouble(rV)) : 0.0f;
        s.from = readBmVec3(ev::getProperty(o, "from"));
        s.to   = readBmVec3(ev::getProperty(o, "to"));
        out[i] = s;
    }
    return true;
}

inline Value makeBranchSegments(const std::vector<bromesh::BranchSegment>& segs) {
    return hostArrayOf(segs.size(), [&segs](size_t i) {
        const auto& s = segs[i];
        ObjectBuilder o;
        o.set("parent", ev::fromDouble(s.parent));
        std::vector<float> fv = {s.from.x, s.from.y, s.from.z};
        std::vector<float> tv = {s.to.x, s.to.y, s.to.z};
        o.set("from", hostArrayOf(3, [&fv](size_t j) { return ev::fromDouble(fv[j]); }));
        o.set("to", hostArrayOf(3, [&tv](size_t j) { return ev::fromDouble(tv[j]); }));
        o.set("radius", ev::fromDouble(s.radius));
        o.set("depth", ev::fromDouble(s.depth));
        return o.get();
    });
}

Value makeBBox(const bromath::AABB3& bb);
Value makeRayHit(const bromesh::RayHit& h);
Value makeTextureBuffer(const bromesh::TextureBuffer& tb);

std::string resolveMeshWritePath(const std::string& path);

// Subsystem decorators
void decorateMeshCore(ObjectBuilder& b);
void decorateMeshPrimitives(HostClass& cls);
void decorateMeshManipulate(ObjectBuilder& b, HostClass& cls);
void decorateMeshAnalysis(ObjectBuilder& b, HostClass& cls);
void decorateMeshOptimize(ObjectBuilder& b, HostClass& cls);

void installMeshBVH();
void installCapsuleField();
void installProgressiveMesh();
void installPolyMesh();
void installLSystem();

void installMeshGlobals();

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
