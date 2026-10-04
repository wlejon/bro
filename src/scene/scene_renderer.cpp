#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "webgl/webgl_types.h"
#include "scene/skinned_mesh_node.h"
#include "scene/decal_node.h"
#include "scene/vulkan/scene_vk_bridge.h"
#include "canvas/canvas_scene.h"
#include "util/log.h"

#include "broimage/decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SceneRenderer::SceneRenderer(SceneGraph& graph) : graph_(graph) {
    // Fallback sun for scenes with no LightNode (see render3D). Configured
    // once; render3D only reads it.
    implicitSun_.setKind(LightNode::Kind::Directional);
    implicitSun_.setDirection(bromath::vnorm(Vec3(-0.3f, -1.0f, -0.5f)));
    implicitSun_.setColor(1.0f, 0.98f, 0.95f);
    implicitSun_.setIntensity(3.0f);
}

SceneRenderer::~SceneRenderer() {
    clearColorLUT();
    destroyMeshFBO();
    destroyMSAAFBO();
    destroySceneDepthCopy();
    destroyTonemapFBO();
    customPrograms_.clear();
    customShadowPrograms_.clear();
    destroyTiltShiftFBOs();
    destroyBloomFBOs();
    destroySSAOFBOs();
    destroySSRFBO();
    destroyDoFFBOs();
    destroyFXAAFBO();
    destroyShadowAtlas();
    clearEnvironment();
}

void SceneRenderer::ensureFallbackTextures() {
}

int SceneRenderer::targetWidth() const {
    const int w = static_cast<int>(graph_.canvasWidth_ * renderScale_ * deviceScale_ + 0.5f);
    return w < 1 ? 1 : w;
}

int SceneRenderer::targetHeight() const {
    const int h = static_cast<int>(graph_.canvasHeight_ * renderScale_ * deviceScale_ + 0.5f);
    return h < 1 ? 1 : h;
}

void SceneRenderer::ensureMeshFBO() {
}

void SceneRenderer::destroyMeshFBO() {
    meshDepthTex_ = 0;
    meshColorTex_ = 0;
    meshFBO_ = 0;
    meshFBOWidth_ = 0;
    meshFBOHeight_ = 0;
}

void SceneRenderer::ensureMSAAFBO() {
    msaaActive_ = false;
}

void SceneRenderer::destroyMSAAFBO() {
    msaaColorRBO_ = 0;
    msaaDepthRBO_ = 0;
    msaaFBO_ = 0;
    msaaWidth_ = msaaHeight_ = 0;
    msaaSamplesAllocated_ = 0;
    msaaActive_ = false;
}

void SceneRenderer::uploadMeshGlobals(const MeshDrawLocs& /*L*/) {
}

// Conservative world-space bounds per cullable node type. The contract is
// strict — the box must contain everything the node can rasterize this frame,
// so culling can never pop visible content: skinned meshes use palette-posed
// bounds, wind sway pads by its max displacement, splats pad by the 3-sigma
// quad extent the splat shader emits.
std::optional<bromath::AABB3> SceneRenderer::nodeWorldBounds(SceneNode* n) const {
    bromath::AABB3 out;
    switch (n->type()) {
    case SceneNode::Type::Mesh: {
        auto* m = static_cast<MeshNode*>(n);
        if (!m->hasDrawableMesh()) return std::nullopt;
        bromath::AABB3 local = m->localBounds();
        auto* sm = m->asSkinnedMesh();
        if (sm && sm->skinReady()) local = sm->posedLocalBounds();
        out = bromath::atransform(local, m->worldMatrix());
        // Wind sway displaces vertices in world space by at most
        // |windDir| * strength * windMask (per-vertex bend <= 1). cullMargin
        // is the user's promise about custom-vertex-shader displacement —
        // the engine can't infer it from GLSL (see setCullMargin).
        float pad = m->windMask() * windStrength_ *
                    bromath::vlen(Vec3{windDir_[0], windDir_[1], windDir_[2]}) +
                    m->cullMargin();
        if (pad > 0.0f) {
            out.min = out.min - Vec3{pad, pad, pad};
            out.max = out.max + Vec3{pad, pad, pad};
        }
        return out;
    }
    case SceneNode::Type::InstancedMesh: {
        auto* m = static_cast<InstancedMeshNode*>(n);
        float lo[3], hi[3];
        if (!m->computeWorldInstanceBounds(lo, hi)) return std::nullopt;
        const float pad = m->cullMargin();
        out.min = {lo[0] - pad, lo[1] - pad, lo[2] - pad};
        out.max = {hi[0] + pad, hi[1] + pad, hi[2] + pad};
        return out;
    }
    case SceneNode::Type::GaussianSplat: {
        auto* s = static_cast<GaussianSplatNode*>(n);
        if (s->splatCount() == 0) return std::nullopt;
        // Pad the local center bounds by the quad extent — kSigma = 3 in the
        // splat VS, plus half a sigma of headroom for the low-pass screen
        // dilation — then take the padded box through the node's world matrix
        // (the splat pipeline applies uModel). atransform scales the pad by
        // the node's uniform scale, matching the shader's sigma scaling.
        float pad = 3.5f * s->maxSigma();
        bromath::AABB3 local = s->localBounds();
        local.min = local.min - Vec3{pad, pad, pad};
        local.max = local.max + Vec3{pad, pad, pad};
        out = bromath::atransform(local, s->worldMatrix());
        return out;
    }
    case SceneNode::Type::Particles3D: {
        if (static_cast<Particles3DNode*>(n)->worldBounds(out)) return out;
        return std::nullopt;
    }
    case SceneNode::Type::Decal: {
        // The decal volume is exactly the unit box in local space (node
        // scale IS the size — see DecalNode), so the world AABB is that box
        // through the world matrix. Exact, no padding needed: the fragment
        // shader discards outside the volume.
        bromath::AABB3 local;
        local.min = Vec3{-0.5f, -0.5f, -0.5f};
        local.max = Vec3{ 0.5f,  0.5f,  0.5f};
        out = bromath::atransform(local, n->worldMatrix());
        return out;
    }
    default:
        return std::nullopt;
    }
}

const bromath::Mat4& SceneRenderer::viewProjRot() const {
    if (vpValid_ &&
        std::memcmp(&vpSrcProj_, &graph_.projectionMatrix_,
                    sizeof(bromath::Mat4)) == 0 &&
        std::memcmp(&vpSrcView_, &graph_.viewMatrix_,
                    sizeof(bromath::Mat4)) == 0) {
        return vpCached_;
    }
    vpSrcProj_ = graph_.projectionMatrix_;
    vpSrcView_ = graph_.viewMatrix_;
    bromath::Mat4 viewRot = graph_.viewMatrix_;
    viewRot.at(0, 3) = 0.0f;
    viewRot.at(1, 3) = 0.0f;
    viewRot.at(2, 3) = 0.0f;
    vpCached_ = bromath::mmul(graph_.projectionMatrix_, viewRot);
    vpValid_ = true;
    return vpCached_;
}

bool SceneRenderer::cameraCulled(SceneNode* n) const {
    if (!cullingActive_) return false;
    auto wbOpt = nodeWorldBounds(n);
    if (!wbOpt) return false;
    return !bromath::fintersects(cameraFrustum_, *wbOpt);
}

void SceneRenderer::render3D() {
    if (defaultVulkanContext_) {
        if (!vkBridge_) {
            vkBridge_ = std::make_unique<vk::SceneVkBridge>(*defaultVulkanContext_);
            if (!vkBridge_->init()) {
                vkBridge_.reset();
            }
        }
        if (vkBridge_) {
            initDepthPolicy();
            graph_.syncProjectionToDepthPolicy();
            cullingActive_ = frustumCullingEnabled_;
            if (cullingActive_) {
                cameraFrustum_ = makeFrustum(
                    bromath::mmul(graph_.projectionMatrix_, graph_.viewMatrix_));
            }
            cullStats_ = CullStats{};

            std::vector<LightNode*> lights;
            collectLights(lights);
            std::vector<LightNode*> fallback;
            if (lights.empty()) { fallback.push_back(&implicitSun_); }
            const auto& activeLights = lights.empty() ? fallback : lights;

            updateSunIrradiance(activeLights);
            prepareShadows(activeLights);
            renderShadowPass();

            vkBridge_->render3D(graph_, *this);
            hasMeshContent_ = vkBridge_->hasMeshContent();
            return;
        }
    }

    static bool warned = false;
    if (!warned) {
        warned = true;
        LOG_WARN("scene: no Vulkan context — the 3D pass is disabled for this "
                 "process (CPU raster path). 2D canvas content still draws.");
    }
}

VkImage SceneRenderer::vkOutputImage() const {
    return (vkBridge_ && hasMeshContent_) ? vkBridge_->ldrImage() : VK_NULL_HANDLE;
}

VkImageLayout SceneRenderer::vkOutputLayout() const {
    return vkBridge_ ? vkBridge_->currentLayout() : VK_IMAGE_LAYOUT_UNDEFINED;
}

uint32_t SceneRenderer::vkOutputWidth() const {
    return vkBridge_ ? vkBridge_->width() : 0;
}

uint32_t SceneRenderer::vkOutputHeight() const {
    return vkBridge_ ? vkBridge_->height() : 0;
}

}  // namespace bro::scene
