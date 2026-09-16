// native_scene_env.cpp — SceneGraph post-processing, environment, effects, raycasting, and mesh factory helpers.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/scene/native_scene_decl.h"
#include "scene/animation_player.h"
#include "engine/scene_audio_sync.h"
#include "util/asset_path.h"
#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/manipulation/normals.h>
#include <glad/gl.h>
#include <json.hpp>

namespace bro::bronze_host {

namespace {

struct RaycastSlot {
    scene::SceneNode* node = nullptr;
    scene::SceneGraph* graph = nullptr;
    bromath::Vec3 point{0, 0, 0};
    bromath::Vec3 normal{0, 1, 0};
    double distance = 0.0;
    int32_t instance = -1;
};
static thread_local RaycastSlot tl_raycastSlot;

struct CullStatsSlot {
    int32_t totalNodes = 0;
    int32_t renderedNodes = 0;
    int32_t culledNodes = 0;
};
static thread_local CullStatsSlot tl_cullStatsSlot;

static thread_local double tl_vec3Buf[3];
static thread_local double tl_unprojectBuf[6];
static thread_local std::vector<uint8_t> tl_tonemapPixels;

struct PickFrame {
    bromath::Vec3 bx, by, bz, bt;
    bromath::Vec3 r0, r1, r2;
    bool ok = false;

    bromath::Vec3 pointToWorld(const bromath::Vec3& v) const {
        return bx * v.x + by * v.y + bz * v.z + bt;
    }
    bromath::Vec3 dirToLocal(const bromath::Vec3& v) const {
        return {bromath::vdot(v, r0), bromath::vdot(v, r1), bromath::vdot(v, r2)};
    }
    bromath::Vec3 pointToLocal(const bromath::Vec3& v) const {
        bromath::Vec3 d = v - bt;
        return {bromath::vdot(d, r0), bromath::vdot(d, r1), bromath::vdot(d, r2)};
    }
    bromath::Vec3 normalToWorld(const bromath::Vec3& n) const {
        return r0 * n.x + r1 * n.y + r2 * n.z;
    }
};

PickFrame pickFrameOf(const bromath::Mat4& m) {
    PickFrame f;
    const float* a = m.data;
    f.bx = {a[0], a[1], a[2]};
    f.by = {a[4], a[5], a[6]};
    f.bz = {a[8], a[9], a[10]};
    f.bt = {a[12], a[13], a[14]};

    float det = bromath::vdot(f.bx, bromath::vcross(f.by, f.bz));
    if (std::abs(det) < 1e-12f) {
        f.ok = false;
        return f;
    }
    float inv = 1.0f / det;
    f.r0 = bromath::vcross(f.by, f.bz) * inv;
    f.r1 = bromath::vcross(f.bz, f.bx) * inv;
    f.r2 = bromath::vcross(f.bx, f.by) * inv;
    f.ok = true;
    return f;
}

bool slabHit(const bromath::AABB3& b, const bromath::Vec3& ro,
             const bromath::Vec3& rd, float& tNearOut) {
    float tmin = -1e30f;
    float tmax = 1e30f;
    const float bmin[3] = {b.min.x, b.min.y, b.min.z};
    const float bmax[3] = {b.max.x, b.max.y, b.max.z};
    const float roA[3] = {ro.x, ro.y, ro.z};
    const float rdA[3] = {rd.x, rd.y, rd.z};

    for (int i = 0; i < 3; ++i) {
        if (std::abs(rdA[i]) < 1e-12f) {
            if (roA[i] < bmin[i] || roA[i] > bmax[i]) return false;
        } else {
            float inv = 1.0f / rdA[i];
            float t1 = (bmin[i] - roA[i]) * inv;
            float t2 = (bmax[i] - roA[i]) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    if (tmax < 0.0f) return false;
    tNearOut = tmin > 0.0f ? tmin : 0.0f;
    return true;
}

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

void bro_scene_SceneGraph_setTiltShift(void* self, bool opts_blur_given, double opts_blur,
                                      bool opts_focus_given, double opts_focus,
                                      bool opts_range_given, double opts_range) {
    auto* g = graphOf(self);
    if (!g) return;
    bool enabled = opts_blur_given || opts_focus_given || opts_range_given;
    float focus = opts_focus_given ? static_cast<float>(opts_focus) : 0.5f;
    float range = opts_range_given ? static_cast<float>(opts_range) : 0.2f;
    float blur = opts_blur_given ? static_cast<float>(opts_blur) : 1.0f;
    g->setTiltShift(enabled, focus, range, 0.1f, blur, 1.0f, 1.0f);
}

void bro_scene_SceneGraph_setBloom(void* self, bool opts_threshold_given, double opts_threshold,
                                  bool opts_intensity_given, double opts_intensity,
                                  bool opts_radius_given, double opts_radius,
                                  bool /*opts_iterations_given*/, int32_t /*opts_iterations*/) {
    auto* g = graphOf(self);
    if (!g) return;
    bool enabled = opts_threshold_given || opts_intensity_given || opts_radius_given;
    float thresh = opts_threshold_given ? static_cast<float>(opts_threshold) : 1.0f;
    float inten = opts_intensity_given ? static_cast<float>(opts_intensity) : 1.0f;
    float radius = opts_radius_given ? static_cast<float>(opts_radius) : 1.0f;
    g->setBloom(enabled, thresh, inten, radius);
}

void bro_scene_SceneGraph_setSSAO(void* self, bool opts_radius_given, double opts_radius,
                                 bool opts_bias_given, double opts_bias,
                                 bool opts_intensity_given, double opts_intensity,
                                 bool /*opts_sampleCount_given*/, int32_t /*opts_sampleCount*/) {
    auto* g = graphOf(self);
    if (!g) return;
    bool enabled = opts_radius_given || opts_bias_given || opts_intensity_given;
    float radius = opts_radius_given ? static_cast<float>(opts_radius) : 0.5f;
    float inten = opts_intensity_given ? static_cast<float>(opts_intensity) : 1.0f;
    float bias = opts_bias_given ? static_cast<float>(opts_bias) : 0.025f;
    g->setSSAO(enabled, radius, inten, bias);
}

void bro_scene_SceneGraph_setSSR(void* self, bool opts_maxDistance_given, double opts_maxDistance,
                                bool opts_thickness_given, double opts_thickness,
                                bool opts_stepCount_given, int32_t opts_stepCount,
                                bool opts_roughnessCutoff_given, double opts_roughnessCutoff) {
    auto* g = graphOf(self);
    if (!g) return;
    bool enabled = opts_maxDistance_given || opts_thickness_given || opts_stepCount_given || opts_roughnessCutoff_given;
    float maxDist = opts_maxDistance_given ? static_cast<float>(opts_maxDistance) : 30.0f;
    int steps = opts_stepCount_given ? opts_stepCount : 48;
    float thick = opts_thickness_given ? static_cast<float>(opts_thickness) : 0.3f;
    float edgeFade = opts_roughnessCutoff_given ? static_cast<float>(opts_roughnessCutoff) : 0.05f;
    g->setSSR(enabled, maxDist, steps, thick, 1.0f, edgeFade);
}

void bro_scene_SceneGraph_setDepthOfField(void* self, bool opts_focusDistance_given, double opts_focusDistance,
                                        bool opts_focalLength_given, double opts_focalLength,
                                        bool /*opts_fStop_given*/, double /*opts_fStop*/,
                                        bool opts_maxBlur_given, double opts_maxBlur) {
    auto* g = graphOf(self);
    if (!g) return;
    bool enabled = opts_focusDistance_given || opts_focalLength_given || opts_maxBlur_given;
    float focus = opts_focusDistance_given ? static_cast<float>(opts_focusDistance) : 10.0f;
    float range = opts_focalLength_given ? static_cast<float>(opts_focalLength) : 5.0f;
    float maxBlur = opts_maxBlur_given ? static_cast<float>(opts_maxBlur) : 1.0f;
    g->setDepthOfField(enabled, focus, range, maxBlur);
}

bool bro_scene_SceneGraph_setColorLUT(void* self, const char* path, int32_t size, double amount) {
    auto* g = graphOf(self);
    if (!g) return false;
    if (path && path[0] != '\0') {
        std::string tex = bro::util::resolveAssetPath(path);
        return g->loadColorLUT(tex, size, static_cast<float>(amount));
    } else {
        g->clearColorLUT();
        return true;
    }
}

void bro_scene_SceneGraph_setFXAA(void* self, bool enabled) {
    auto* g = graphOf(self);
    if (g) g->setFXAA(enabled);
}

void bro_scene_SceneGraph_setRenderScale(void* self, double scale) {
    auto* g = graphOf(self);
    if (g) g->setRenderScale(static_cast<float>(scale));
}

void bro_scene_SceneGraph_setMSAA(void* self, int32_t samples) {
    auto* g = graphOf(self);
    if (g) g->setMSAA(samples);
}

void bro_scene_SceneGraph_setEnvironment(void* self, bool opts_panorama_given, const char* opts_panorama,
                                        bool /*opts_cubeMap_given*/, const char* /*opts_cubeMap*/,
                                        const double* /*opts_color*/, uint32_t /*opts_color_len*/,
                                        bool opts_intensity_given, double opts_intensity,
                                        bool opts_rotation_given, double opts_rotation,
                                        bool /*opts_background_given*/, bool /*opts_background*/) {
    auto* g = graphOf(self);
    if (!g) return;
    if (opts_panorama_given) {
        if (opts_panorama && opts_panorama[0] != '\0') {
            g->loadEnvironment(bro::util::resolveAssetPath(opts_panorama));
        } else {
            g->clearEnvironment();
        }
    }
    if (opts_intensity_given) g->setEnvironmentIntensity(static_cast<float>(opts_intensity));
    if (opts_rotation_given) g->setEnvironmentRotation(static_cast<float>(opts_rotation));
}

void bro_scene_SceneGraph_setFrustumCulling(void* self, bool enabled) {
    auto* g = graphOf(self);
    if (g) g->setFrustumCulling(enabled);
}

void bro_scene_SceneGraph_cullStats(void* self) {
    auto* g = graphOf(self);
    if (g) {
        const auto& s = g->cullStats();
        int rendered = s.meshDrawn + s.instancedDrawn + s.splatDrawn + s.particlesDrawn + s.billboardsDrawn + s.decalsDrawn;
        int culled = s.meshCulled + s.instancedCulled + s.splatCulled + s.particlesCulled + s.billboardsCulled + s.decalsCulled;
        tl_cullStatsSlot.renderedNodes = rendered;
        tl_cullStatsSlot.culledNodes = culled;
        tl_cullStatsSlot.totalNodes = rendered + culled;
    } else {
        tl_cullStatsSlot.renderedNodes = 0;
        tl_cullStatsSlot.culledNodes = 0;
        tl_cullStatsSlot.totalNodes = 0;
    }
}

int32_t bro_scene_SceneGraph_cullStats_totalNodes(void) {
    return tl_cullStatsSlot.totalNodes;
}

int32_t bro_scene_SceneGraph_cullStats_renderedNodes(void) {
    return tl_cullStatsSlot.renderedNodes;
}

int32_t bro_scene_SceneGraph_cullStats_culledNodes(void) {
    return tl_cullStatsSlot.culledNodes;
}

const char* bro_scene_SceneGraph_cullStatsJson(void* self) {
    auto* g = graphOf(self);
    static thread_local std::string tl_cullJson;
    if (!g) return "{}";
    const auto& s = g->cullStats();
    int rendered = s.meshDrawn + s.instancedDrawn + s.splatDrawn + s.particlesDrawn + s.billboardsDrawn + s.decalsDrawn;
    int culled = s.meshCulled + s.instancedCulled + s.splatCulled + s.particlesCulled + s.billboardsCulled + s.decalsCulled;
    nlohmann::json j;
    j["totalNodes"] = rendered + culled;
    j["renderedNodes"] = rendered;
    j["culledNodes"] = culled;
    j["meshDrawn"] = s.meshDrawn;
    j["meshCulled"] = s.meshCulled;
    j["instancedDrawn"] = s.instancedDrawn;
    j["instancedCulled"] = s.instancedCulled;
    j["splatDrawn"] = s.splatDrawn;
    j["splatCulled"] = s.splatCulled;
    j["particlesDrawn"] = s.particlesDrawn;
    j["particlesCulled"] = s.particlesCulled;
    j["billboardsDrawn"] = s.billboardsDrawn;
    j["billboardsCulled"] = s.billboardsCulled;
    j["decalsDrawn"] = s.decalsDrawn;
    j["decalsCulled"] = s.decalsCulled;
    j["shadowDrawn"] = s.shadowDrawn;
    j["shadowCulled"] = s.shadowCulled;
    j["shadowTilesTotal"] = s.shadowTilesTotal;
    j["shadowTilesRendered"] = s.shadowTilesRendered;
    j["shadowTilesCached"] = s.shadowTilesCached;
    tl_cullJson = j.dump();
    return tl_cullJson.c_str();
}

void bro_scene_SceneGraph_clear(void* self) {
    auto* g = graphOf(self);
    if (!g) return;
    auto* root = g->root();
    if (!root) return;
    while (!root->children().empty()) {
        g->destroyNode(root->children().back());
    }
}

void bro_scene_SceneGraph_syncPhysics(void* self) {
    auto* g = graphOf(self);
    if (g) g->syncPhysics();
}

bool bro_scene_SceneGraph_raycast(void* self, const double* origin, uint32_t origin_len,
                                  const double* direction, uint32_t direction_len) {
    auto* g = graphOf(self);
    if (!g || !origin || origin_len < 3 || !direction || direction_len < 3) return false;

    bromath::Vec3 o{static_cast<float>(origin[0]), static_cast<float>(origin[1]), static_cast<float>(origin[2])};
    bromath::Vec3 d{static_cast<float>(direction[0]), static_cast<float>(direction[1]), static_cast<float>(direction[2])};
    d = bromath::vnorm(d);
    if (bromath::vlen2(d) < 1e-12f) return false;

    float closestDist = 1e30f;
    scene::SceneNode* closestNode = nullptr;
    bromath::Vec3 closestWorldPoint;
    bromath::Vec3 closestWorldNormal;

    auto tryMesh = [&](const PickFrame& f, const bromesh::MeshData& md,
                       const bromath::AABB3& lb, auto&& bvhOf) -> bool {
        if (!f.ok) return false;
        bromath::Vec3 lo = f.pointToLocal(o);
        bromath::Vec3 ld = f.dirToLocal(d);
        float ldLen = bromath::vlen(ld);
        if (ldLen < 1e-20f) return false;
        bromath::Vec3 ldN = ld * (1.0f / ldLen);
        float localMax = closestDist * ldLen;

        float tNear = 0.0f;
        if (!slabHit(lb, lo, ldN, tNear) || tNear > localMax) return false;

        const float op[3] = {lo.x, lo.y, lo.z};
        const float dp[3] = {ldN.x, ldN.y, ldN.z};
        bromesh::RayHit hit = bvhOf().raycast(md, op, dp, localMax);
        if (!hit.hit) return false;

        bromath::Vec3 worldHit = f.pointToWorld({hit.position[0], hit.position[1], hit.position[2]});
        float worldDist = bromath::vlen(worldHit - o);
        if (worldDist >= closestDist) return false;

        closestDist = worldDist;
        closestWorldPoint = worldHit;
        closestWorldNormal = bromath::vnorm(f.normalToWorld({hit.normal[0], hit.normal[1], hit.normal[2]}));
        return true;
    };

    int32_t closestInstance = -1;
    g->root()->traverse([&](scene::SceneNode* node) {
        if (!node || !node->visible()) return;
        if (node->type() == scene::SceneNode::Type::Mesh) {
            auto* mn = static_cast<scene::MeshNode*>(node);
            const bromesh::MeshData& md = mn->mesh();
            if (md.positions.empty() || md.indices.empty()) return;
            auto bvhOf = [&]() -> const bromesh::MeshBVH& { return mn->bvh(); };
            if (tryMesh(pickFrameOf(node->worldMatrix()), md, mn->localBounds(), bvhOf)) {
                closestNode = mn;
                closestInstance = -1;
            }
        } else if (node->type() == scene::SceneNode::Type::InstancedMesh) {
            auto* im = static_cast<scene::InstancedMeshNode*>(node);
            const bromesh::MeshData& md = im->mesh();
            if (md.positions.empty() || md.indices.empty() || im->instanceCount() == 0) return;
            auto bvhOf = [&]() -> const bromesh::MeshBVH& { return im->bvh(); };
            for (size_t i = 0; i < im->instanceCount(); ++i) {
                float rows[12];
                if (!im->instanceRows(i, rows)) continue;
                bromath::Mat4 instLocal = bromath::midentity();
                instLocal.at(0, 0) = rows[0]; instLocal.at(0, 1) = rows[1]; instLocal.at(0, 2) = rows[2]; instLocal.at(0, 3) = rows[3];
                instLocal.at(1, 0) = rows[4]; instLocal.at(1, 1) = rows[5]; instLocal.at(1, 2) = rows[6]; instLocal.at(1, 3) = rows[7];
                instLocal.at(2, 0) = rows[8]; instLocal.at(2, 1) = rows[9]; instLocal.at(2, 2) = rows[10]; instLocal.at(2, 3) = rows[11];
                bromath::Mat4 instWorld = bromath::mmul(node->worldMatrix(), instLocal);
                if (tryMesh(pickFrameOf(instWorld), md, im->localBounds(), bvhOf)) {
                    closestNode = im;
                    closestInstance = static_cast<int32_t>(i);
                }
            }
        }
    });


    if (closestNode) {
        tl_raycastSlot.node = closestNode;
        tl_raycastSlot.graph = g;
        tl_raycastSlot.point = closestWorldPoint;
        tl_raycastSlot.normal = closestWorldNormal;
        tl_raycastSlot.distance = closestDist;
        tl_raycastSlot.instance = closestInstance;
        return true;
    }
    return false;
}

void* bro_scene_SceneGraph_raycast_node(void) {
    return tl_raycastSlot.node ? wrapNode(tl_raycastSlot.node, tl_raycastSlot.graph) : nullptr;
}

void bro_scene_SceneGraph_raycast_point(bronze_native_buffer* out) {
    tl_vec3Buf[0] = tl_raycastSlot.point.x;
    tl_vec3Buf[1] = tl_raycastSlot.point.y;
    tl_vec3Buf[2] = tl_raycastSlot.point.z;
    copyBuffer(tl_vec3Buf, 3, out);
}

void bro_scene_SceneGraph_raycast_normal(bronze_native_buffer* out) {
    tl_vec3Buf[0] = tl_raycastSlot.normal.x;
    tl_vec3Buf[1] = tl_raycastSlot.normal.y;
    tl_vec3Buf[2] = tl_raycastSlot.normal.z;
    copyBuffer(tl_vec3Buf, 3, out);
}

double bro_scene_SceneGraph_raycast_distance(void) {
    return tl_raycastSlot.distance;
}

int32_t bro_scene_SceneGraph_raycast_instance(void) {
    return tl_raycastSlot.instance;
}

void bro_scene_SceneGraph_unprojectLocal(void* self, void* node, const double* screenPoint, uint32_t screenPoint_len, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    if (!g || !out || !screenPoint || screenPoint_len < 2) {
        copyBuffer<double>(nullptr, 0, out);
        return;
    }
    auto cam = g->activeCamera();
    if (!cam) {
        copyBuffer<double>(nullptr, 0, out);
        return;
    }

    bromath::Vec3 origin, dir;
    if (g->unprojectLocal(static_cast<float>(screenPoint[0]), static_cast<float>(screenPoint[1]), origin, dir)) {
        if (node) {
            auto* n = nodeOf(node);
            if (n) {
                auto inv = bromath::minverse(n->worldMatrix());
                origin = bromath::mtransformPoint(inv, origin);
                dir = bromath::vnorm(bromath::mtransformDir(inv, dir));
            }
        }
        tl_unprojectBuf[0] = origin.x;
        tl_unprojectBuf[1] = origin.y;
        tl_unprojectBuf[2] = origin.z;
        tl_unprojectBuf[3] = dir.x;
        tl_unprojectBuf[4] = dir.y;
        tl_unprojectBuf[5] = dir.z;
        copyBuffer(tl_unprojectBuf, 6, out);
    } else {
        copyBuffer<double>(nullptr, 0, out);
    }
}

void bro_scene_SceneGraph_bindAudioListenerToCamera(void* self, bool bind) {
    auto* g = graphOf(self);
    if (g) engine::SceneAudioSync::bindAudioListenerToCamera(g->livenessToken(), g, bind);
}

void bro_scene_SceneGraph_detachAIWorld(void* self) {
    auto* g = graphOf(self);
    if (g) g->detachAIWorld();
}

// --- Manual helpers for captureFrame, toImageData, createMesh, etc. ----------

void bro_scene_SceneGraph_render(void* self) {
    auto* g = graphOf(self);
    if (g) g->render();
}

double bro_scene_SceneGraph_canvasWidth(void* self) {
    auto* g = graphOf(self);
    return g ? g->canvasWidth() : 0.0;
}

double bro_scene_SceneGraph_canvasHeight(void* self) {
    auto* g = graphOf(self);
    return g ? g->canvasHeight() : 0.0;
}

void bro_scene_SceneGraph_setCanvasSize(void* self, double w, double h) {
    auto* g = graphOf(self);
    if (g) g->setCanvasSize(static_cast<float>(w), static_cast<float>(h));
}

void bro_scene_SceneGraph_readTonemapPixels(void* self, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    if (!g) { copyBuffer<uint8_t>(nullptr, 0, out); return; }
    int w = 0, h = 0;
    tl_tonemapPixels = g->readTonemapPixelsRGBA(w, h);
    copyBuffer(tl_tonemapPixels.data(), static_cast<uint32_t>(tl_tonemapPixels.size()), out);
}

void* bro_scene_SceneGraph_createMesh(void* self, uint64_t optsBits, uint64_t meshVal) {
    auto* g = graphOf(self);
    if (!g) return nullptr;

    auto* node = g->createMesh();
    if (!node) return nullptr;
    g->root()->addChild(node);

    bromesh::MeshData meshData;
    void* ptr = bronze::embed::handleData(bronze::Value{meshVal});
    if (ptr) {
        meshData = *static_cast<bromesh::MeshData*>(ptr);
    }

    Value opts = ev::fromBits(optsBits);
    if (ev::isObject(opts)) {
        // 1. Raw positions/indices
        Value posVal = ev::getProperty(opts, "positions");
        Value idxVal = ev::getProperty(opts, "indices");
        bool hasRaw = false;
        if (!ev::isUndefined(posVal) && !ev::isUndefined(idxVal)) {
            if (readFloatVector(posVal, meshData.positions) && readU32Vector(idxVal, meshData.indices)) {
                readFloatVector(ev::getProperty(opts, "normals"), meshData.normals);
                readFloatVector(ev::getProperty(opts, "colors"), meshData.colors);
                readFloatVector(ev::getProperty(opts, "uvs"), meshData.uvs);
                readFloatVector(ev::getProperty(opts, "tangents"), meshData.tangents);
                hasRaw = true;
            }
        }

        // 2. Mesh object or primitive name
        Value meshProp = ev::getProperty(opts, "mesh");
        if (!hasRaw && meshData.positions.empty()) {
            if (ev::isObject(meshProp)) {
                void* mptr = bronze::embed::handleData(meshProp);
                if (mptr) {
                    meshData = *static_cast<bromesh::MeshData*>(mptr);
                    hasRaw = true;
                }
            }
        }

        if (!hasRaw && meshData.positions.empty()) {
            std::string meshType = "box";
            if (ev::isString(meshProp)) meshType = ev::toUtf8(meshProp);
            auto getNum = [&](const char* k, float def) -> float {
                Value v = ev::getProperty(opts, k);
                return ev::isNumber(v) ? static_cast<float>(ev::toDouble(v)) : def;
            };
            if (meshType == "sphere") {
                float r = getNum("radius", 0.5f);
                int seg = static_cast<int>(getNum("segments", 16));
                int rings = static_cast<int>(getNum("rings", 12));
                meshData = bromesh::sphere(r, seg, rings);
            } else if (meshType == "cylinder") {
                float r = getNum("radius", 0.5f);
                float h = getNum("height", 1.0f);
                int seg = static_cast<int>(getNum("segments", 16));
                meshData = bromesh::cylinder(r, h, seg);
            } else if (meshType == "capsule") {
                float r = getNum("radius", 0.5f);
                float h = getNum("height", 1.0f);
                int seg = static_cast<int>(getNum("segments", 16));
                int rings = static_cast<int>(getNum("rings", 8));
                meshData = bromesh::capsule(r, h, seg, rings);
            } else if (meshType == "plane") {
                float hw = getNum("halfW", 5.0f);
                float hd = getNum("halfD", 5.0f);
                int sx = static_cast<int>(getNum("subdivX", 1));
                int sz = static_cast<int>(getNum("subdivZ", 1));
                meshData = bromesh::plane(hw, hd, sx, sz);
            } else if (meshType == "torus") {
                float maj = getNum("majorRadius", 1.0f);
                float min = getNum("minorRadius", 0.3f);
                int majSeg = static_cast<int>(getNum("majorSegments", 24));
                int minSeg = static_cast<int>(getNum("minorSegments", 12));
                meshData = bromesh::torus(maj, min, majSeg, minSeg);
            } else {
                float hw = getNum("halfW", 0.5f);
                float hh = getNum("halfH", 0.5f);
                float hd = getNum("halfD", 0.5f);
                meshData = bromesh::box(hw, hh, hd);
            }
        }

        if (meshData.normals.empty() && !meshData.positions.empty()) {
            bromesh::computeNormals(meshData);
        }
        node->setMesh(std::move(meshData));

        // Color
        Value colorVal = ev::getProperty(opts, "color");
        float cr = 1, cg = 1, cb = 1, ca = 1;
        if (parseColorValue(colorVal, cr, cg, cb, ca)) {
            node->setColor(cr, cg, cb, ca);
        }

        // Metallic / Roughness / Emissive
        Value metVal = ev::getProperty(opts, "metallic");
        if (ev::isNumber(metVal)) node->setMetallic(static_cast<float>(ev::toDouble(metVal)));
        Value roughVal = ev::getProperty(opts, "roughness");
        if (ev::isNumber(roughVal)) node->setRoughness(static_cast<float>(ev::toDouble(roughVal)));
        Value emissVal = ev::getProperty(opts, "emissive");
        if (ev::isNumber(emissVal)) node->setEmissive(static_cast<float>(ev::toDouble(emissVal)));

        Value unlitVal = ev::getProperty(opts, "unlit");
        if (!ev::isUndefined(unlitVal)) node->setUnlit(ev::toBool(unlitVal));
        Value csVal = ev::getProperty(opts, "castsShadow");
        if (!ev::isUndefined(csVal)) node->setCastsShadow(ev::toBool(csVal));
        Value rsVal = ev::getProperty(opts, "receivesShadow");
        if (!ev::isUndefined(rsVal)) node->setReceivesShadow(ev::toBool(rsVal));
    } else {
        if (meshData.positions.empty()) {
            meshData = bromesh::box(0.5f, 0.5f, 0.5f);
        }
        if (meshData.normals.empty()) {
            bromesh::computeNormals(meshData);
        }
        node->setMesh(std::move(meshData));
    }

    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createSkinnedMesh(void* self, uint64_t optsBits, uint64_t meshHandle) {
    auto* g = graphOf(self);
    if (!g) return nullptr;

    Value opts = ev::fromBits(optsBits);
    if (!ev::isObject(opts)) {
        ev::throwTypeError("createSkinnedMesh: options object with 'skin' is required");
        return nullptr;
    }
    Value skinVal = ev::getProperty(opts, "skin");
    void* sptr = ev::isObject(skinVal) ? bronze::embed::handleData(skinVal) : nullptr;
    if (!sptr) {
        ev::throwTypeError("createSkinnedMesh: 'skin' option (SkinData) is required");
        return nullptr;
    }
    auto* sd = static_cast<bromesh::SkinData*>(sptr);

    bromesh::MeshData meshData;
    void* mptr = bronze::embed::handleData(bronze::Value{meshHandle});
    if (mptr) {
        meshData = *static_cast<bromesh::MeshData*>(mptr);
    }

    // 1. Raw positions/indices
    Value posVal = ev::getProperty(opts, "positions");
    Value idxVal = ev::getProperty(opts, "indices");
    bool hasRaw = false;
    if (!ev::isUndefined(posVal) && !ev::isUndefined(idxVal)) {
        if (readFloatVector(posVal, meshData.positions) && readU32Vector(idxVal, meshData.indices)) {
            readFloatVector(ev::getProperty(opts, "normals"), meshData.normals);
            readFloatVector(ev::getProperty(opts, "colors"), meshData.colors);
            readFloatVector(ev::getProperty(opts, "uvs"), meshData.uvs);
            readFloatVector(ev::getProperty(opts, "tangents"), meshData.tangents);
            hasRaw = true;
        }
    }

    // 2. Mesh object or primitive name
    Value meshProp = ev::getProperty(opts, "mesh");
    if (!hasRaw && meshData.positions.empty()) {
        if (ev::isObject(meshProp)) {
            void* p = bronze::embed::handleData(meshProp);
            if (p) {
                meshData = *static_cast<bromesh::MeshData*>(p);
                hasRaw = true;
            }
        }
    }

    if (!hasRaw && meshData.positions.empty()) {
        std::string meshType = "box";
        if (ev::isString(meshProp)) meshType = ev::toUtf8(meshProp);
        auto getNum = [&](const char* k, float def) -> float {
            Value v = ev::getProperty(opts, k);
            return ev::isNumber(v) ? static_cast<float>(ev::toDouble(v)) : def;
        };
        if (meshType == "sphere") {
            float r = getNum("radius", 0.5f);
            int seg = static_cast<int>(getNum("segments", 16));
            int rings = static_cast<int>(getNum("rings", 12));
            meshData = bromesh::sphere(r, seg, rings);
        } else if (meshType == "cylinder") {
            float r = getNum("radius", 0.5f);
            float h = getNum("height", 1.0f);
            int seg = static_cast<int>(getNum("segments", 16));
            meshData = bromesh::cylinder(r, h, seg);
        } else if (meshType == "capsule") {
            float r = getNum("radius", 0.5f);
            float h = getNum("height", 1.0f);
            int seg = static_cast<int>(getNum("segments", 16));
            int rings = static_cast<int>(getNum("rings", 8));
            meshData = bromesh::capsule(r, h, seg, rings);
        } else if (meshType == "plane") {
            float hw = getNum("halfW", 5.0f);
            float hd = getNum("halfD", 5.0f);
            int sx = static_cast<int>(getNum("subdivX", 1));
            int sz = static_cast<int>(getNum("subdivZ", 1));
            meshData = bromesh::plane(hw, hd, sx, sz);
        } else if (meshType == "torus") {
            float maj = getNum("majorRadius", 1.0f);
            float min = getNum("minorRadius", 0.3f);
            int majSeg = static_cast<int>(getNum("majorSegments", 24));
            int minSeg = static_cast<int>(getNum("minorSegments", 12));
            meshData = bromesh::torus(maj, min, majSeg, minSeg);
        } else {
            float hw = getNum("halfW", 0.5f);
            float hh = getNum("halfH", 0.5f);
            float hd = getNum("halfD", 0.5f);
            meshData = bromesh::box(hw, hh, hd);
        }
    }

    size_t vertCount = meshData.positions.size() / 3;
    size_t skinVertCount = sd->boneWeights.size() / 4;
    if (skinVertCount != vertCount) {
        ev::throwTypeError("createSkinnedMesh: skin vertex count does not match mesh vertex count");
        return nullptr;
    }

    auto* node = g->createSkinnedMesh();
    g->root()->addChild(node);

    if (meshData.normals.empty() && !meshData.positions.empty()) {
        bromesh::computeNormals(meshData);
    }
    node->setMesh(std::move(meshData));
    node->setSkin(*sd);

    Value skelProp = ev::getProperty(opts, "skeleton");
    if (ev::isObject(skelProp)) {
        void* skelPtr = bronze::embed::handleData(skelProp);
        if (skelPtr) {
            auto* skel = static_cast<bromesh::Skeleton*>(skelPtr);
            node->ensurePlayer().setSkeleton(std::make_shared<bromesh::Skeleton>(*skel));
        }
    }

    // Material properties
    Value colorVal = ev::getProperty(opts, "color");
    float cr = 1, cg = 1, cb = 1, ca = 1;
    if (parseColorValue(colorVal, cr, cg, cb, ca)) {
        node->setColor(cr, cg, cb, ca);
    }
    Value metVal = ev::getProperty(opts, "metallic");
    if (ev::isNumber(metVal)) node->setMetallic(static_cast<float>(ev::toDouble(metVal)));
    Value roughVal = ev::getProperty(opts, "roughness");
    if (ev::isNumber(roughVal)) node->setRoughness(static_cast<float>(ev::toDouble(roughVal)));
    Value emissVal = ev::getProperty(opts, "emissive");
    if (ev::isNumber(emissVal)) node->setEmissive(static_cast<float>(ev::toDouble(emissVal)));
    Value unlitVal = ev::getProperty(opts, "unlit");
    if (!ev::isUndefined(unlitVal)) node->setUnlit(ev::toBool(unlitVal));
    Value csVal = ev::getProperty(opts, "castsShadow");
    if (!ev::isUndefined(csVal)) node->setCastsShadow(ev::toBool(csVal));
    Value rsVal = ev::getProperty(opts, "receivesShadow");
    if (!ev::isUndefined(rsVal)) node->setReceivesShadow(ev::toBool(rsVal));
    Value nameVal = ev::getProperty(opts, "name");
    if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createInstancedMesh(void* self, const char* jsonOpts, uint64_t meshVal) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createInstancedMesh();
    g->root()->addChild(node);
    void* ptr = bronze::embed::handleData(bronze::Value{meshVal});
    if (ptr) {
        auto* srcMesh = static_cast<bromesh::MeshData*>(ptr);
        node->setMesh(*srcMesh);
    }
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createShape(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createShape();
    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = nlohmann::json::parse(jsonOpts);
            if (j.contains("name") && j["name"].is_string()) node->setName(j["name"].get<std::string>());
            if (j.contains("width") && j.contains("height")) {
                node->setSize(j["width"].get<float>(), j["height"].get<float>());
            }
            if (j.contains("fill") && j["fill"].is_string()) {
                float r = 1, g = 1, b = 1, a = 1;
                if (parseHexOrCssColor(j["fill"].get<std::string>(), r, g, b, a)) {
                    node->setFillColor(bromath::Color{r, g, b, a});
                }
            }
            if (j.contains("worldAnchor")) {
                const auto& wa = j["worldAnchor"];
                if (wa.is_array() && wa.size() >= 3) {
                    node->setWorldAnchor(bromath::Vec3{wa[0].get<float>(), wa[1].get<float>(), wa[2].get<float>()});
                }
            }
            if (j.contains("billboard") && j["billboard"].is_string()) {
                std::string m = j["billboard"].get<std::string>();
                if (m == "ylock" || m == "yLock") node->setBillboardMode(scene::SceneNode::BillboardMode::YLock);
                else node->setBillboardMode(scene::SceneNode::BillboardMode::Full);
            }
        } catch (...) {}
    }
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createSprite(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createSprite();
    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = nlohmann::json::parse(jsonOpts);
            if (j.contains("sheet") && j["sheet"].is_object()) {
                const auto& s = j["sheet"];
                node->setSheetGrid(s.value("frameWidth", 0), s.value("frameHeight", 0), s.value("columns", 1), s.value("rows", 1));
            }
            if (j.contains("animations") && j["animations"].is_object()) {
                for (auto it = j["animations"].begin(); it != j["animations"].end(); ++it) {
                    const auto& specVal = it.value();
                    if (specVal.is_object()) {
                        scene::SpriteNode::AnimationSpec spec;
                        spec.fps = specVal.value("fps", 12.0f);
                        spec.loop = specVal.value("loop", true);
                        spec.next = specVal.value("next", "");
                        if (specVal.contains("frames") && specVal["frames"].is_array()) {
                            for (const auto& f : specVal["frames"]) if (f.is_number()) spec.frames.push_back(f.template get<int>());
                        }
                        node->addAnimation(it.key(), std::move(spec));
                    }
                }
            }
            if (j.contains("width") && j.contains("height")) node->setSize(j.value("width", 0.0f), j.value("height", 0.0f));
            if (j.contains("opacity")) node->setOpacity(j.value("opacity", 1.0f));
        } catch (...) {}
    }
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createPhysicsNode(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createPhysicsNode();
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createParticles3D(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createParticles3D();
    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = nlohmann::json::parse(jsonOpts);
            if (j.contains("name") && j["name"].is_string()) node->setName(j["name"].get<std::string>());
            if (j.contains("seed") && j["seed"].is_number()) node->setSeed(j["seed"].get<uint64_t>());
            if (j.contains("rate") && j["rate"].is_number()) node->setRate(j["rate"].get<float>());
            if (j.contains("lifetime")) {
                if (j["lifetime"].is_number()) {
                    float lt = j["lifetime"].get<float>();
                    node->setLifetime(lt, lt);
                } else if (j["lifetime"].is_object()) {
                    float lo = j["lifetime"].value("min", 0.5f);
                    float hi = j["lifetime"].value("max", 1.0f);
                    node->setLifetime(lo, hi);
                }
            }
            if (j.contains("size")) {
                if (j["size"].is_number()) {
                    float s = j["size"].get<float>();
                    node->setSize(s, s);
                } else if (j["size"].is_object()) {
                    float st = j["size"].value("start", 0.2f);
                    float en = j["size"].value("end", 0.2f);
                    node->setSize(st, en);
                }
            }
            if (j.contains("position")) {
                const auto& p = j["position"];
                if (p.is_array() && p.size() >= 3) {
                    node->setPosition(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
                }
            }
            if (j.contains("maxParticles") && j["maxParticles"].is_number()) node->setMaxParticles(j["maxParticles"].get<int>());
            if (j.contains("softness") && j["softness"].is_number()) node->setSoftness(j["softness"].get<float>());
            if (j.contains("duration") && j["duration"].is_number()) node->setDuration(j["duration"].get<float>(), j.value("loop", false));
            if (j.contains("blend") && j["blend"].is_string()) {
                node->setBlend(j["blend"] == "additive" ? scene::Particles3DNode::Blend::Additive : scene::Particles3DNode::Blend::Normal);
            }
            if (j.contains("space") && j["space"].is_string()) {
                node->setSpace(j["space"] == "local" ? scene::Particles3DNode::SimSpace::Local : scene::Particles3DNode::SimSpace::World);
            }
            if (j.contains("gravity") && j["gravity"].is_array() && j["gravity"].size() >= 3) {
                node->setGravity({j["gravity"][0].get<float>(), j["gravity"][1].get<float>(), j["gravity"][2].get<float>()});
            }
            if (j.contains("drag") && j["drag"].is_number()) node->setDrag(j["drag"].get<float>());
            if (j.contains("velocity") && j["velocity"].is_object()) {
                const auto& vel = j["velocity"];
                float sp = vel.value("speed", 1.0f);
                float spSpr = vel.value("speedSpread", 0.0f);
                node->setSpeed(sp, spSpr);
                float spr = vel.value("spread", 0.0f);
                bromath::Vec3 dir{0, 1, 0};
                if (vel.contains("direction") && vel["direction"].is_array() && vel["direction"].size() >= 3) {
                    dir = {vel["direction"][0].get<float>(), vel["direction"][1].get<float>(), vel["direction"][2].get<float>()};
                }
                node->setDirection(dir, spr);
            }
            if (j.contains("rotation") && j["rotation"].is_object()) {
                const auto& r = j["rotation"];
                node->setRotation(r.value("start", 0.0f), r.value("spinSpeed", 0.0f), r.value("spinSpread", 0.0f));
            }
            if (j.contains("shape") && j["shape"].is_object()) {
                const auto& sh = j["shape"];
                std::string type = sh.value("type", "point");
                if (type == "sphere") node->setShape(scene::Particles3DNode::EmitterShape::Sphere);
                else if (type == "hemisphere") node->setShape(scene::Particles3DNode::EmitterShape::Hemisphere);
                else if (type == "box") node->setShape(scene::Particles3DNode::EmitterShape::Box);
                else if (type == "cone") node->setShape(scene::Particles3DNode::EmitterShape::Cone);
                else node->setShape(scene::Particles3DNode::EmitterShape::Point);
                if (sh.contains("radius") && sh["radius"].is_number()) node->setShapeRadius(sh["radius"].get<float>());
            }
            if (j.contains("color") && j["color"].is_object()) {
                auto parseC = [](const nlohmann::json& v) -> bromath::Color {
                    if (v.is_array() && v.size() >= 3) {
                        float a = v.size() >= 4 ? v[3].get<float>() : 1.0f;
                        return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>(), a};
                    }
                    if (v.is_string()) {
                        std::string s = v.get<std::string>();
                        if (!s.empty() && s[0] == '#' && s.size() == 7) {
                            int r = std::stoi(s.substr(1, 2), nullptr, 16);
                            int g = std::stoi(s.substr(3, 2), nullptr, 16);
                            int b = std::stoi(s.substr(5, 2), nullptr, 16);
                            return {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
                        }
                    }
                    return {1.0f, 1.0f, 1.0f, 1.0f};
                };
                const auto& c = j["color"];
                bromath::Color st = c.contains("start") ? parseC(c["start"]) : bromath::Color{1,1,1,1};
                bromath::Color en = c.contains("end") ? parseC(c["end"]) : st;
                node->setColors(st, en);
            }
            if (j.contains("burst") && j["burst"].is_number()) {
                node->burst(j["burst"].get<int>());
            }
            if (j.value("autoplay", true)) node->play();
        } catch (...) {}
    }
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createGaussianSplat(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createGaussianSplat();
    g->root()->addChild(node);
    return wrapNode(node, g);
}

}  // extern "C"
