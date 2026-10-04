#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/gl_available.h"
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
    // Destroy GL resources
    if (fallback2D_) { glDeleteTextures(1, &fallback2D_); fallback2D_ = 0; }
    if (fallbackCube_) { glDeleteTextures(1, &fallbackCube_); fallbackCube_ = 0; }
    if (fallbackShadow_) { glDeleteTextures(1, &fallbackShadow_); fallbackShadow_ = 0; }
    if (fallback3D_) { glDeleteTextures(1, &fallback3D_); fallback3D_ = 0; }
    clearColorLUT();
    destroyMeshFBO();
    destroyMSAAFBO();
    destroySceneDepthCopy();
    destroyTonemapFBO();
    if (meshProgram_) { glDeleteProgram(meshProgram_); meshProgram_ = 0; }
    for (auto& [key, entry] : customPrograms_) {
        if (entry.prog) glDeleteProgram(entry.prog);
    }
    customPrograms_.clear();
    for (auto& [key, entry] : customShadowPrograms_) {
        if (entry.prog) glDeleteProgram(entry.prog);
    }
    customShadowPrograms_.clear();
    if (meshSkinnedProgram_) { glDeleteProgram(meshSkinnedProgram_); meshSkinnedProgram_ = 0; }
    if (meshInstancedProgram_) { glDeleteProgram(meshInstancedProgram_); meshInstancedProgram_ = 0; }
    if (foliageScatterProgram_) { glDeleteProgram(foliageScatterProgram_); foliageScatterProgram_ = 0; }
    if (tubeProgram_) { glDeleteProgram(tubeProgram_); tubeProgram_ = 0; }
    if (tubeDepthProgram_) { glDeleteProgram(tubeDepthProgram_); tubeDepthProgram_ = 0; }
    if (bbProgram_) { glDeleteProgram(bbProgram_); bbProgram_ = 0; }
    if (bbVBO_) { glDeleteBuffers(1, &bbVBO_); bbVBO_ = 0; }
    if (bbVAO_) { glDeleteVertexArrays(1, &bbVAO_); bbVAO_ = 0; }
    if (particleProgram_) { glDeleteProgram(particleProgram_); particleProgram_ = 0; }
    if (particleQuadVBO_) { glDeleteBuffers(1, &particleQuadVBO_); particleQuadVBO_ = 0; }
    if (decalProgram_) { glDeleteProgram(decalProgram_); decalProgram_ = 0; }
    if (decalVBO_) { glDeleteBuffers(1, &decalVBO_); decalVBO_ = 0; }
    if (decalVAO_) { glDeleteVertexArrays(1, &decalVAO_); decalVAO_ = 0; }
    if (tonemapProgram_) { glDeleteProgram(tonemapProgram_); tonemapProgram_ = 0; }
    if (tonemapVBO_) { glDeleteBuffers(1, &tonemapVBO_); tonemapVBO_ = 0; }
    if (tonemapVAO_) { glDeleteVertexArrays(1, &tonemapVAO_); tonemapVAO_ = 0; }
    destroyTiltShiftFBOs();
    if (blurProgram_) { glDeleteProgram(blurProgram_); blurProgram_ = 0; }
    if (tiltProgram_) { glDeleteProgram(tiltProgram_); tiltProgram_ = 0; }
    destroyBloomFBOs();
    if (bloomBrightProgram_) { glDeleteProgram(bloomBrightProgram_); bloomBrightProgram_ = 0; }
    destroySSAOFBOs();
    if (ssaoProgram_) { glDeleteProgram(ssaoProgram_); ssaoProgram_ = 0; }
    if (ssaoNoiseTex_) { glDeleteTextures(1, &ssaoNoiseTex_); ssaoNoiseTex_ = 0; }
    destroySSRFBO();
    if (ssrProgram_) { glDeleteProgram(ssrProgram_); ssrProgram_ = 0; }
    destroyDoFFBOs();
    if (dofProgram_) { glDeleteProgram(dofProgram_); dofProgram_ = 0; }
    destroyFXAAFBO();
    if (fxaaProgram_) { glDeleteProgram(fxaaProgram_); fxaaProgram_ = 0; }
    if (probeCaptureFBO_) { glDeleteFramebuffers(1, &probeCaptureFBO_); probeCaptureFBO_ = 0; }
    if (probeDepthRBO_) { glDeleteRenderbuffers(1, &probeDepthRBO_); probeDepthRBO_ = 0; }
    destroyShadowAtlas();
    if (shadowProgram_) { glDeleteProgram(shadowProgram_); shadowProgram_ = 0; }
    if (shadowInstancedProgram_) { glDeleteProgram(shadowInstancedProgram_); shadowInstancedProgram_ = 0; }
    if (shadowSkinnedProgram_) { glDeleteProgram(shadowSkinnedProgram_); shadowSkinnedProgram_ = 0; }
    clearEnvironment();
    if (envConvertProgram_) { glDeleteProgram(envConvertProgram_); envConvertProgram_ = 0; }
    if (envConvertVBO_) { glDeleteBuffers(1, &envConvertVBO_); envConvertVBO_ = 0; }
    if (envConvertVAO_) { glDeleteVertexArrays(1, &envConvertVAO_); envConvertVAO_ = 0; }
    if (envConvertFBO_) { glDeleteFramebuffers(1, &envConvertFBO_); envConvertFBO_ = 0; }
    if (atmProgram_) { glDeleteProgram(atmProgram_); atmProgram_ = 0; }
    if (skyboxProgram_) { glDeleteProgram(skyboxProgram_); skyboxProgram_ = 0; }
    if (skyboxVBO_) { glDeleteBuffers(1, &skyboxVBO_); skyboxVBO_ = 0; }
    if (skyboxVAO_) { glDeleteVertexArrays(1, &skyboxVAO_); skyboxVAO_ = 0; }
    if (irrConvProgram_) { glDeleteProgram(irrConvProgram_); irrConvProgram_ = 0; }
    if (prefilterProgram_) { glDeleteProgram(prefilterProgram_); prefilterProgram_ = 0; }
    if (brdfLUTProgram_) { glDeleteProgram(brdfLUTProgram_); brdfLUTProgram_ = 0; }
    if (brdfLUT_) { glDeleteTextures(1, &brdfLUT_); brdfLUT_ = 0; }}

void SceneRenderer::ensureFallbackTextures() {
    if (fallback2D_ && fallbackCube_ && fallbackShadow_ && fallback3D_) return;

    if (!fallback3D_) {
        glGenTextures(1, &fallback3D_);
        glBindTexture(GL_TEXTURE_3D, fallback3D_);
        uint8_t white[4] = {255, 255, 255, 255};
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_3D, 0);
    }

    if (!fallback2D_) {
        glGenTextures(1, &fallback2D_);
        glBindTexture(GL_TEXTURE_2D, fallback2D_);
        uint8_t white[4] = {255, 255, 255, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    if (!fallbackCube_) {
        glGenTextures(1, &fallbackCube_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, fallbackCube_);
        uint8_t white[4] = {255, 255, 255, 255};
        for (int f = 0; f < 6; ++f) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, white);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }

    if (!fallbackShadow_) {
        glGenTextures(1, &fallbackShadow_);
        glBindTexture(GL_TEXTURE_2D, fallbackShadow_);
        float one = 1.0f; // depth = far, comparison always passes (ref <= 1)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 1, 1, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, &one);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
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
    if (graph_.canvasWidth_ <= 0 || graph_.canvasHeight_ <= 0) return;
    const int tw = targetWidth();
    const int th = targetHeight();
    if (meshFBO_ && meshFBOWidth_ == tw && meshFBOHeight_ == th) return;

    destroyMeshFBO();

    meshFBOWidth_ = tw;
    meshFBOHeight_ = th;

    glGenFramebuffers(1, &meshFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, meshFBO_);

    // HDR color attachment — RGBA16F so lighting can exceed 1.0 before
    // tonemap. The LDR output texture consumed by the compositor is a
    // separate RGBA8 texture owned by the tonemap FBO.
    glGenTextures(1, &meshColorTex_);
    glBindTexture(GL_TEXTURE_2D, meshColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, meshFBOWidth_, meshFBOHeight_, 0,
                 GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, meshColorTex_, 0);

    // Depth-stencil texture (not an RBO): the soft-particle pass samples it
    // (via the sceneDepthCopy blit) and the tonemap FBO re-attaches it for
    // the post-tonemap unlit overlay's depth test.
    glGenTextures(1, &meshDepthTex_);
    glBindTexture(GL_TEXTURE_2D, meshDepthTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, depthStencilInternalFormat(),
                 meshFBOWidth_, meshFBOHeight_, 0,
                 GL_DEPTH_STENCIL, depthStencilType(), nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, meshDepthTex_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Mesh FBO incomplete: 0x%x", status);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneRenderer::destroyMeshFBO() {
    if (meshDepthTex_) { glDeleteTextures(1, &meshDepthTex_); meshDepthTex_ = 0; }
    if (meshColorTex_) { glDeleteTextures(1, &meshColorTex_); meshColorTex_ = 0; }
    if (meshFBO_) { glDeleteFramebuffers(1, &meshFBO_); meshFBO_ = 0; }
    meshFBOWidth_ = 0;
    meshFBOHeight_ = 0;
}

// Multisampled HDR target (color RGBA16F + depth-stencil renderbuffers) at
// the mesh FBO size. Recreated when the size or sample count changes; torn
// down when MSAA is turned off. Sets msaaActive_ for the frame — false on
// any allocation failure so rendering falls back to the single-sampled path.
void SceneRenderer::ensureMSAAFBO() {
    msaaActive_ = false;
    if (msaaSamples_ < 2 || !meshFBO_) {
        destroyMSAAFBO();
        return;
    }

    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    const int samples = msaaSamples_ > maxSamples ? static_cast<int>(maxSamples)
                                                  : msaaSamples_;
    if (samples < 2) {
        destroyMSAAFBO();
        return;
    }

    if (!msaaFBO_ || msaaWidth_ != meshFBOWidth_ || msaaHeight_ != meshFBOHeight_ ||
        msaaSamplesAllocated_ != samples) {
        destroyMSAAFBO();

        glGenRenderbuffers(1, &msaaColorRBO_);
        glBindRenderbuffer(GL_RENDERBUFFER, msaaColorRBO_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA16F,
                                         meshFBOWidth_, meshFBOHeight_);
        glGenRenderbuffers(1, &msaaDepthRBO_);
        glBindRenderbuffer(GL_RENDERBUFFER, msaaDepthRBO_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, depthStencilInternalFormat(),
                                         meshFBOWidth_, meshFBOHeight_);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        glGenFramebuffers(1, &msaaFBO_);
        glBindFramebuffer(GL_FRAMEBUFFER, msaaFBO_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, msaaColorRBO_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, msaaDepthRBO_);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("MSAA FBO incomplete (%d samples): 0x%x", samples, status);
            destroyMSAAFBO();
            return;
        }
        msaaWidth_ = meshFBOWidth_;
        msaaHeight_ = meshFBOHeight_;
        msaaSamplesAllocated_ = samples;
    }

    msaaActive_ = true;
}

void SceneRenderer::destroyMSAAFBO() {
    if (msaaColorRBO_) { glDeleteRenderbuffers(1, &msaaColorRBO_); msaaColorRBO_ = 0; }
    if (msaaDepthRBO_) { glDeleteRenderbuffers(1, &msaaDepthRBO_); msaaDepthRBO_ = 0; }
    if (msaaFBO_) { glDeleteFramebuffers(1, &msaaFBO_); msaaFBO_ = 0; }
    msaaWidth_ = msaaHeight_ = 0;
    msaaSamplesAllocated_ = 0;
    msaaActive_ = false;
}

void SceneRenderer::uploadMeshGlobals(const MeshDrawLocs& L) {
    glUniform1f(L.fogStart, fogStart_);
    glUniform1f(L.fogEnd, fogEnd_);
    glUniform3f(L.fogColor, fogColor_[0], fogColor_[1], fogColor_[2]);
    glUniform1f(L.fogDensity, fogDensity_);
    glUniform1f(L.fogHeightFalloff, fogHeightFalloff_);
    glUniform1f(L.fogStartDist, fogStartDist_);
    glUniform1f(L.fogCamY, graph_.cameraEye_.y);
    uploadAtmLocs(L.atm);
    glUniform3f(L.ambient, effectiveAmbient()[0], effectiveAmbient()[1], effectiveAmbient()[2]);
    if (L.windDir      >= 0) glUniform3fv(L.windDir, 1, windDir_);
    if (L.windStrength >= 0) glUniform1f(L.windStrength, windStrength_);
    if (L.windTime     >= 0) glUniform1f(L.windTime, windTime_);
    if (L.windFreq     >= 0) glUniform1f(L.windFreq, windFreq_);
    // SSR mask phase: opaque draws write the reflectance mask into alpha
    // (see render3D — cleared before any pass that blends against alpha).
    if (L.ssrMask      >= 0) glUniform1i(L.ssrMask, ssrMaskActive_ ? 1 : 0);
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
