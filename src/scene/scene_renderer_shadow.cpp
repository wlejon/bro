#include "scene/scene_renderer.h"
#include "scene/depth_policy.h"
#include "scene/nodes/grass_node.h"
#include "scene/nodes/instanced_mesh_node.h"
#include "scene/nodes/plants_node.h"
#include "scene/nodes/skinned_mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/frame/scene_vk_bridge.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

using bromath::Mat4;
using bromath::Vec3;

// ---------------------------------------------------------------------------
// The shadow planner (see scene_renderer.h and world/shadow_pages.h).
// ---------------------------------------------------------------------------

namespace {

bromath::AABB3 padded(const bromath::AABB3& b, float pad) {
    return {b.min - Vec3{pad, pad, pad}, b.max + Vec3{pad, pad, pad}};
}

}  // namespace

ShadowMotion SceneRenderer::shadowMotion(uint32_t nodeId) const {
    const auto it = shadowMotion_.find(nodeId);
    return it == shadowMotion_.end() ? ShadowMotion::Still : it->second;
}

bromath::AABB3 SceneRenderer::seenBounds(float reach) const {
    // The view frustum's corners (out to the shadow's reach), in world space.
    const float zNear = graph_.cameraNearZ_;
    float zFar = graph_.cameraFarZ_;
    if (reach > 0.0f) zFar = std::min(zFar, std::max(reach, zNear));
    const Mat4 invView = bromath::minverse(graph_.viewMatrix_);
    bromath::AABB3 box = bromath::aempty3();
    for (int c = 0; c < 8; ++c) {
        const float z = (c & 4) ? zFar : zNear;
        float x, y;
        if (graph_.cameraIsPerspective_) {
            const float h = z * std::tan(graph_.cameraFovY_ * 0.5f);
            x = ((c & 1) ? h : -h) * graph_.cameraAspect_;
            y = (c & 2) ? h : -h;
        } else {
            x = (c & 1) ? graph_.cameraOrthoR_ : graph_.cameraOrthoL_;
            y = (c & 2) ? graph_.cameraOrthoT_ : graph_.cameraOrthoB_;
        }
        const Vec3 p{invView.at(0, 0) * x + invView.at(0, 1) * y - invView.at(0, 2) * z + invView.at(0, 3),
                     invView.at(1, 0) * x + invView.at(1, 1) * y - invView.at(1, 2) * z + invView.at(1, 3),
                     invView.at(2, 0) * x + invView.at(2, 1) * y - invView.at(2, 2) * z + invView.at(2, 3)};
        box = bromath::amerge(box, bromath::AABB3{p, p});
    }
    return box;
}

void SceneRenderer::gatherShadowCasters(bromath::AABB3& casterBounds) {
    ShadowPages& pages = shadowPages_;
    const ShadowPagesConfig& config = pages.config();
    const Vec3 eye = graph_.cameraEye_;
    // Vegetation sways whenever the wind field moves at all; a wind-masked
    // mesh bends under the wind.
    const bool swaying = !windFieldCalm(windField());
    const bool windy = windStrength_ > 0.0f;
    shadowCasterStates_.begin();
    casterBounds = bromath::aempty3();
    // How far from the eye the sway level, whose overlays carry what sways,
    // reaches (its window's half side).
    const float swayReach = std::ldexp(config.finestTexel, pages.swayLevel()) * SP_PAGE * (SP_WINDOW / 2);

    auto walk = [&](auto&& self, SceneNode* n) -> void {
        if (!n || !n->renderVisible()) return;
        if (n->type() == SceneNode::Type::Mesh) {
            auto* m = static_cast<MeshNode*>(n);
            // Unlit meshes never cast (a custom shader suppresses unlit).
            const bool casts = m->castsShadow() && !m->effectiveUnlit();
            if (casts && m->asGrass()) {
                // The near ring's blades, around the eye wherever its field
                // is: the sway level's overlays.
                const float r = 256.0f;
                pages.addDynamic({eye - Vec3{r, r, r}, eye + Vec3{r, r, r}}, true);
                shadowMotion_[m->id()] = ShadowMotion::Grass;
            } else if (casts && m->asPlants()) {
                // In the cached depth, but for the sway level's overlays
                // (which the finer levels read): those its cells near the
                // eye touch. Those in its shadow range price the pages they
                // cast into (addLoad).
                const PlantsNode* p = m->asPlants();
                const float castRange = std::min(p->params().shadowRange, p->params().range);
                const int castFrom = pages.swayLevel() + 1;
                float elements[SP_MAX_LEVELS] = {};
                bromath::AABB3 all = bromath::aempty3();
                for (const CellContent& c : p->cells()) {
                    if (bromath::aisEmpty(c.bounds)) continue;
                    const bromath::AABB3 b = padded(c.bounds, 1.0f);
                    all = bromath::amerge(all, b);
                    const float d = bromath::vlen(bromath::vmin(bromath::vmax(eye, b.min), b.max) - eye);
                    if (d < swayReach) pages.addDynamic(b, true);
                    if (d < castRange && castFrom < config.levels) {
                        p->castElements(c.key, config.finestTexel, castFrom, config.levels, elements);
                        pages.addLoad(b, elements);
                    }
                }
                if (!bromath::aisEmpty(all)) {
                    // (A cell past the plants' shadow range casts nothing:
                    // its coming and going changes no page.)
                    const float castReach = p->params().shadowRange;
                    shadowCasterStates_.still(m->id(), p->generation(), all, pages,
                                              [p, eye, castReach](uint64_t since, std::vector<bromath::AABB3>& boxes) {
                                                  if (!p->changesSince(since, boxes)) return false;
                                                  std::erase_if(boxes, [&](const bromath::AABB3& b) {
                                                      const Vec3 near = bromath::vmin(bromath::vmax(eye, b.min), b.max);
                                                      return bromath::vlen(near - eye) > castReach;
                                                  });
                                                  return true;
                                              });
                    casterBounds = bromath::amerge(casterBounds, all);
                }
                shadowMotion_[m->id()] = ShadowMotion::Swaying;
            } else if (casts && m->hasDrawableMesh() && !m->asGrass() && !m->asPlants()) {
                if (const auto wb = nodeWorldBounds(m)) {
                    auto* sm = m->asSkinnedMesh();
                    const bool skinned = sm && sm->skinReady();
                    // A custom vertex chunk displaces with no generation signal.
                    const bool chunk = m->hasCustomShader() && !m->customShader()->vertexChunk.empty();
                    const bool sways = (m->vegetationHeight() > 0.0f && swaying) ||
                                       (m->vegetationHeight() <= 0.0f && windy && m->windMask() > 0.0f);
                    const bool moving = shadowCasterStates_.caster(m->id(), m->changeGeneration(), *wb,
                                                                   skinned || chunk, pages, sways);
                    if (moving) shadowMotion_[m->id()] = ShadowMotion::Moving;
                    else if (sways) shadowMotion_[m->id()] = ShadowMotion::Swaying;
                    casterBounds = bromath::amerge(casterBounds, *wb);   // (its cullMargin in)
                }
            }
        } else if (n->type() == SceneNode::Type::InstancedMesh) {
            auto* m = static_cast<InstancedMeshNode*>(n);
            // Branch tubes, swaying scatter leaves, and instanced meshes.
            const bool procedural = (m->isTube() && m->tubeSegCount() > 0) ||
                                    (m->isScatter() && m->hasVegetation() && m->scatterSegCount() > 0);
            const bool casts = m->castsShadow() &&
                               (procedural || (!m->effectiveUnlit() && !m->mesh().empty() && m->instanceCount() > 0));
            if (casts) {
                if (const auto wb = nodeWorldBounds(m)) {
                    const CustomShaderState* cs = m->customShader();
                    const bool chunk = cs && !cs->vertexChunk.empty();
                    const bool sways = swaying && m->hasVegetation();
                    const bool moving =
                        shadowCasterStates_.caster(m->id(), m->changeGeneration(), *wb, chunk, pages, sways);
                    if (moving) shadowMotion_[m->id()] = ShadowMotion::Moving;
                    else if (sways) shadowMotion_[m->id()] = ShadowMotion::Swaying;
                    casterBounds = bromath::amerge(casterBounds, *wb);
                }
            }
        }
        for (auto* c : n->children()) self(self, c);
    };
    walk(walk, graph_.root_.get());
    shadowCasterStates_.end(pages);
}

void SceneRenderer::prepareShadows(const std::vector<LightNode*>& lights) {
    ShadowPages& pages = shadowPages_;
    ShadowPlan& plan = shadowPlan_;
    plan.pcfTaps = shadowPCFTaps_;
    // A pixel's footprint on the target the frame renders at.
    const float rows = static_cast<float>(std::max(targetHeight(), 1));
    plan.pixelAngle = graph_.cameraIsPerspective_ ? 2.0f * std::tan(graph_.cameraFovY_ * 0.5f) / rows : 0.0f;
    plan.orthoPixel = graph_.cameraIsPerspective_ ? 0.0f : (graph_.cameraOrthoT_ - graph_.cameraOrthoB_) / rows;
    shadowMotion_.clear();
    const Vec3 eye = graph_.cameraEye_;
    pages.beginFrame(eye);

    bool anyLight = false;
    for (auto* L : lights) anyLight = anyLight || (L && L->castsShadow());
    bromath::AABB3 casterBounds = bromath::aempty3();
    if (anyLight) {
        gatherShadowCasters(casterBounds);
    } else {
        // Nothing casts: the pages go with their lights (endLights), and the
        // casters are met anew when a light casts again.
        shadowCasterStates_.clear();
    }

    // The lights: the first shadowed directional light is the sun (its
    // clipmap); spots and point lights get their maps while there is room.
    // Positions are relative to the eye in relViewProj (what the GPU draws
    // and samples with), absolute in viewProj (culling).
    bool haveSun = false;
    float sunReach = 0.0f;
    for (int i = 0; i < static_cast<int>(lights.size()) && i < ShadowPlan::kMaxLights && anyLight; ++i) {
        LightNode* L = lights[i];
        if (!L || !L->castsShadow()) continue;
        Vec3 d = L->direction();
        const float dlen = bromath::vlen(d);
        if (L->kind() != LightNode::Kind::Point && dlen < 1e-6f) continue;
        if (dlen > 0.0f) d = d * (1.0f / dlen);

        if (L->kind() == LightNode::Kind::Directional) {
            if (haveSun) continue;
            haveSun = true;
            ShadowSun sun;
            sun.lightId = L->id();
            sun.lightIndex = i;
            sun.direction = d;
            sun.casters = casterBounds;
            sun.bias = L->shadowBias();
            sun.normalBias = L->shadowNormalBias();
            sun.reach = L->shadowDistance();
            sun.angularRadius = atmosphere_.sunAngularRadius;
            sunReach = sun.reach;
            pages.setSun(sun);
            continue;
        }
        const Mat4& M = L->worldMatrix();
        const Vec3 pos{M.at(0, 3), M.at(1, 3), M.at(2, 3)};
        const Vec3 rel = pos - eye;
        const float far = std::max(L->range(), 0.5f);
        ShadowLocal local;
        local.lightId = L->id();
        local.lightIndex = i;
        local.bias = L->shadowBias();
        local.normalBias = L->shadowNormalBias();
        local.zFar = far;
        // The finest mip is SP_LOCAL pages of SP_PAGE texels across.
        constexpr float kSide = static_cast<float>(SP_LOCAL * SP_PAGE);
        if (L->kind() == LightNode::Kind::Spot) {
            // The cone: FOV twice the outer angle (capped below 180 degrees).
            const Vec3 up = std::abs(d.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
            const float fov = std::min(2.0f * std::max(L->outerAngle(), 0.05f), 3.10f);
            local.zNear = std::max(0.1f, far * 0.005f);
            const Mat4 proj = makePerspectiveZeroToOne(fov, 1.0f, local.zNear, far);
            local.faces = 1;
            local.viewProj[0] = bromath::mmul(proj, bromath::mlookAt(pos, pos + d, up));
            local.relViewProj[0] = bromath::mmul(proj, bromath::mlookAt(rel, rel + d, up));
            local.texelPerMetre = 2.0f * std::tan(fov * 0.5f) / kSide;
        } else {
            // Six cube faces (the D3D / GL order), a little over 90 degrees
            // so the faces overlap at their seams.
            local.zNear = std::max(0.05f, far * 0.005f);
            const Mat4 proj = makePerspectiveZeroToOne(1.5708f, 1.0f, local.zNear, far);
            static const Vec3 kForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            static const Vec3 kUp[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
            local.faces = 6;
            for (int f = 0; f < 6; ++f) {
                local.viewProj[f] = bromath::mmul(proj, bromath::mlookAt(pos, pos + kForward[f], kUp[f]));
                local.relViewProj[f] = bromath::mmul(proj, bromath::mlookAt(rel, rel + kForward[f], kUp[f]));
            }
            local.texelPerMetre = 2.0f / kSide;
        }
        pages.addLocal(local);
    }
    pages.endLights();
    pages.setSeen(anyLight ? seenBounds(sunReach) : bromath::aempty3());

    // What the marking passes read back since the last frame, then the plan.
    if (vkBridge_) vkBridge_->shadowRequests(pages);
    pages.plan(plan);

    const ShadowPagesStats& s = pages.stats();
    cullStats_.shadowPagesNeeded = static_cast<int>(s.needed);
    cullStats_.shadowPagesResident = static_cast<int>(s.resident);
    cullStats_.shadowPagesDrawn = static_cast<int>(s.drawn);
    cullStats_.shadowPagesDynamic = static_cast<int>(s.dynamic);
    cullStats_.shadowPagesWanted = static_cast<int>(s.wanted);
    cullStats_.shadowPagesEvicted = static_cast<int>(s.evicted);
    cullStats_.shadowPagesBuilding = static_cast<int>(s.building);
    cullStats_.shadowViews = static_cast<int>(s.views);
}

}  // namespace bro::scene
