// native_scene_env.cpp — SceneGraph post-processing, environment, effects, raycasting, and mesh factory helpers.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/scene/native_scene_decl.h"
#include "engine/scene_audio_sync.h"
#include "util/asset_path.h"
#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/manipulation/normals.h>
#include <glad/gl.h>

namespace bro::bronze_host {

namespace {

struct RaycastSlot {
    scene::SceneNode* node = nullptr;
    scene::SceneGraph* graph = nullptr;
    bromath::Vec3 point{0, 0, 0};
    bromath::Vec3 normal{0, 1, 0};
    double distance = 0.0;
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
    float maxDist = opts_maxDistance_given ? static_cast<float>(opts_maxDistance) : 20.0f;
    int steps = opts_stepCount_given ? opts_stepCount : 32;
    float thick = opts_thickness_given ? static_cast<float>(opts_thickness) : 0.1f;
    float edgeFade = opts_roughnessCutoff_given ? static_cast<float>(opts_roughnessCutoff) : 0.1f;
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

void bro_scene_SceneGraph_setColorLUT(void* self, bool opts_texture_given, const char* opts_texture,
                                     bool opts_intensity_given, double opts_intensity) {
    auto* g = graphOf(self);
    if (!g) return;
    if (opts_texture_given && opts_texture && opts_texture[0] != '\0') {
        std::string tex = bro::util::resolveAssetPath(opts_texture);
        float inten = opts_intensity_given ? static_cast<float>(opts_intensity) : 1.0f;
        g->loadColorLUT(tex, 32, inten);
    } else {
        g->clearColorLUT();
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
    scene::MeshNode* closestNode = nullptr;
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

    g->root()->traverse([&](scene::SceneNode* node) {
        if (!node || node->type() != scene::SceneNode::Type::Mesh || !node->visible()) return;
        auto* mn = static_cast<scene::MeshNode*>(node);
        const bromesh::MeshData& md = mn->mesh();
        if (md.positions.empty() || md.indices.empty()) return;
        auto bvhOf = [&]() -> const bromesh::MeshBVH& { return mn->bvh(); };
        if (tryMesh(pickFrameOf(node->worldMatrix()), md, mn->localBounds(), bvhOf)) {
            closestNode = mn;
        }
    });

    if (closestNode) {
        tl_raycastSlot.node = closestNode;
        tl_raycastSlot.graph = g;
        tl_raycastSlot.point = closestWorldPoint;
        tl_raycastSlot.normal = closestWorldNormal;
        tl_raycastSlot.distance = closestDist;
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

void* bro_scene_SceneGraph_createMesh(void* self, const char* jsonOpts, uint64_t meshVal) {
    auto* g = graphOf(self);
    if (!g) return nullptr;

    auto* node = g->createMesh();
    if (!node) return nullptr;
    g->root()->addChild(node);

    bromesh::MeshData meshData;
    void* ptr = bronze::embed::handleData(bronze::Value{meshVal});
    if (ptr) {
        auto* srcMesh = static_cast<bromesh::MeshData*>(ptr);
        meshData = *srcMesh;
    }

    if (jsonOpts && jsonOpts[0] != '\0') {
        std::string s = jsonOpts;
        auto findNum = [&](const char* key, float def) -> float {
            auto pos = s.find(key);
            if (pos == std::string::npos) return def;
            pos += std::strlen(key);
            return std::strtof(s.c_str() + pos, nullptr);
        };
        float radius = findNum("\"radius\":", 0.5f);
        float hw = findNum("\"halfW\":", 0.5f);
        float hh = findNum("\"halfH\":", 0.5f);
        float hd = findNum("\"halfD\":", 0.5f);
        float height = findNum("\"height\":", 1.0f);

        if (meshData.positions.empty()) {
            if (s.find("\"mesh\":\"sphere\"") != std::string::npos) {
                meshData = bromesh::sphere(radius, 16, 12);
            } else if (s.find("\"mesh\":\"cylinder\"") != std::string::npos) {
                meshData = bromesh::cylinder(radius, height, 16);
            } else if (s.find("\"mesh\":\"capsule\"") != std::string::npos) {
                meshData = bromesh::capsule(radius, height, 16, 8);
            } else if (s.find("\"mesh\":\"plane\"") != std::string::npos) {
                meshData = bromesh::plane(hw, hd, 1, 1);
            } else if (s.find("\"mesh\":\"torus\"") != std::string::npos) {
                meshData = bromesh::torus(1.0f, 0.3f, 24, 12);
            } else {
                meshData = bromesh::box(hw, hh, hd);
            }
        }
        if (meshData.normals.empty() && !meshData.positions.empty()) {
            bromesh::computeNormals(meshData);
        }
        node->setMesh(std::move(meshData));

        // Quick color checks
        if (s.find("\"color\":\"red\"") != std::string::npos || s.find("\"color\":\"#ff0000\"") != std::string::npos) {
            node->setColor(1.0f, 0.0f, 0.0f, 1.0f);
        } else if (s.find("\"color\":\"green\"") != std::string::npos || s.find("\"color\":\"#00ff00\"") != std::string::npos) {
            node->setColor(0.0f, 1.0f, 0.0f, 1.0f);
        } else if (s.find("\"color\":\"blue\"") != std::string::npos || s.find("\"color\":\"#0000ff\"") != std::string::npos) {
            node->setColor(0.0f, 0.0f, 1.0f, 1.0f);
        } else if (s.find("\"color\":\"white\"") != std::string::npos || s.find("\"color\":\"#ffffff\"") != std::string::npos) {
            node->setColor(1.0f, 1.0f, 1.0f, 1.0f);
        }

        if (s.find("\"unlit\":true") != std::string::npos) node->setUnlit(true);
        if (s.find("\"castsShadow\":false") != std::string::npos) node->setCastsShadow(false);
        if (s.find("\"receivesShadow\":false") != std::string::npos) node->setReceivesShadow(false);

        // Metallic / Roughness
        float m = findNum("\"metallic\":", -1.0f);
        if (m >= 0.0f) node->setMetallic(m);
        float r = findNum("\"roughness\":", -1.0f);
        if (r >= 0.0f) node->setRoughness(r);

        float px = findNum("\"x\":", 0.0f);
        float py = findNum("\"y\":", 0.0f);
        float pz = findNum("\"z\":", 0.0f);
        if (px != 0.0f || py != 0.0f || pz != 0.0f) node->setPosition(px, py, pz);
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

void* bro_scene_SceneGraph_createSkinnedMesh(void* self, const char* jsonOpts, uint64_t meshVal,
                                            uint64_t /*skinDataVal*/, uint64_t /*skeletonVal*/) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createSkinnedMesh();
    g->root()->addChild(node);
    void* ptr = bronze::embed::handleData(bronze::Value{meshVal});
    if (ptr) {
        auto* srcMesh = static_cast<bromesh::MeshData*>(ptr);
        node->setMesh(*srcMesh);
    }
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
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void* bro_scene_SceneGraph_createSprite(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createSprite();
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
