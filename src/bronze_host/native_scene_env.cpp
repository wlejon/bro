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

#include <cmath>

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

// The graph's own view/projection answer both directions, whichever way the
// camera was set: scene.setCamera / setCameraQuat / setCameraOrtho write them
// directly, and an active CameraNode writes them the same way.
void bro_scene_SceneGraph_unprojectLocal(void* self, double x, double y, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    if (!out) return;
    bromath::Vec3 origin, dir;
    if (g && std::isfinite(x) && std::isfinite(y) &&
        g->unprojectLocal(static_cast<float>(x), static_cast<float>(y), origin, dir)) {
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

void bro_scene_SceneGraph_projectLocal(void* self, double x, double y, double z, bronze_native_buffer* out) {
    auto* g = graphOf(self);
    if (!out) return;
    float px = 0.0f, py = 0.0f, depth = 0.0f;
    if (g && std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
        g->projectLocal(bromath::Vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)),
                        px, py, depth)) {
        tl_unprojectBuf[0] = px;
        tl_unprojectBuf[1] = py;
        tl_unprojectBuf[2] = depth;
        copyBuffer(tl_unprojectBuf, 3, out);
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

}  // extern "C"
