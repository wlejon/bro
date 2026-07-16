#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/skinned_mesh_node.h"
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

SceneRenderer::SceneRenderer(SceneGraph& graph) : graph_(graph) {}

SceneRenderer::~SceneRenderer() {
    // Destroy GL resources
    if (fallback2D_) { glDeleteTextures(1, &fallback2D_); fallback2D_ = 0; }
    if (fallbackCube_) { glDeleteTextures(1, &fallbackCube_); fallbackCube_ = 0; }
    if (fallbackShadow_) { glDeleteTextures(1, &fallbackShadow_); fallbackShadow_ = 0; }
    destroyMeshFBO();
    destroyMSAAFBO();
    destroySceneDepthCopy();
    destroyTonemapFBO();
    if (meshProgram_) { glDeleteProgram(meshProgram_); meshProgram_ = 0; }
    if (meshSkinnedProgram_) { glDeleteProgram(meshSkinnedProgram_); meshSkinnedProgram_ = 0; }
    if (meshInstancedProgram_) { glDeleteProgram(meshInstancedProgram_); meshInstancedProgram_ = 0; }
    if (bbProgram_) { glDeleteProgram(bbProgram_); bbProgram_ = 0; }
    if (bbVBO_) { glDeleteBuffers(1, &bbVBO_); bbVBO_ = 0; }
    if (bbVAO_) { glDeleteVertexArrays(1, &bbVAO_); bbVAO_ = 0; }
    if (particleProgram_) { glDeleteProgram(particleProgram_); particleProgram_ = 0; }
    if (particleQuadVBO_) { glDeleteBuffers(1, &particleQuadVBO_); particleQuadVBO_ = 0; }
    if (tonemapProgram_) { glDeleteProgram(tonemapProgram_); tonemapProgram_ = 0; }
    if (tonemapVBO_) { glDeleteBuffers(1, &tonemapVBO_); tonemapVBO_ = 0; }
    if (tonemapVAO_) { glDeleteVertexArrays(1, &tonemapVAO_); tonemapVAO_ = 0; }
    destroyTiltShiftFBOs();
    if (blurProgram_) { glDeleteProgram(blurProgram_); blurProgram_ = 0; }
    if (tiltProgram_) { glDeleteProgram(tiltProgram_); tiltProgram_ = 0; }
    destroyBloomFBOs();
    if (bloomBrightProgram_) { glDeleteProgram(bloomBrightProgram_); bloomBrightProgram_ = 0; }
    destroyShadowAtlas();
    if (shadowProgram_) { glDeleteProgram(shadowProgram_); shadowProgram_ = 0; }
    if (shadowInstancedProgram_) { glDeleteProgram(shadowInstancedProgram_); shadowInstancedProgram_ = 0; }
    if (shadowSkinnedProgram_) { glDeleteProgram(shadowSkinnedProgram_); shadowSkinnedProgram_ = 0; }
    clearEnvironment();
    if (envConvertProgram_) { glDeleteProgram(envConvertProgram_); envConvertProgram_ = 0; }
    if (envConvertVBO_) { glDeleteBuffers(1, &envConvertVBO_); envConvertVBO_ = 0; }
    if (envConvertVAO_) { glDeleteVertexArrays(1, &envConvertVAO_); envConvertVAO_ = 0; }
    if (envConvertFBO_) { glDeleteFramebuffers(1, &envConvertFBO_); envConvertFBO_ = 0; }
    if (skyboxProgram_) { glDeleteProgram(skyboxProgram_); skyboxProgram_ = 0; }
    if (skyboxVBO_) { glDeleteBuffers(1, &skyboxVBO_); skyboxVBO_ = 0; }
    if (skyboxVAO_) { glDeleteVertexArrays(1, &skyboxVAO_); skyboxVAO_ = 0; }
    if (irrConvProgram_) { glDeleteProgram(irrConvProgram_); irrConvProgram_ = 0; }
    if (prefilterProgram_) { glDeleteProgram(prefilterProgram_); prefilterProgram_ = 0; }
    if (brdfLUTProgram_) { glDeleteProgram(brdfLUTProgram_); brdfLUTProgram_ = 0; }
    if (brdfLUT_) { glDeleteTextures(1, &brdfLUT_); brdfLUT_ = 0; }}

void SceneRenderer::ensureFallbackTextures() {
    if (fallback2D_ && fallbackCube_ && fallbackShadow_) return;

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
    const int w = static_cast<int>(graph_.canvasWidth_ * renderScale_ + 0.5f);
    return w < 1 ? 1 : w;
}

int SceneRenderer::targetHeight() const {
    const int h = static_cast<int>(graph_.canvasHeight_ * renderScale_ + 0.5f);
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, meshFBOWidth_, meshFBOHeight_, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
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
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8,
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
    glUniform3f(L.ambient, ambientColor_[0], ambientColor_[1], ambientColor_[2]);
    if (L.windDir      >= 0) glUniform3fv(L.windDir, 1, windDir_);
    if (L.windStrength >= 0) glUniform1f(L.windStrength, windStrength_);
    if (L.windTime     >= 0) glUniform1f(L.windTime, windTime_);
    if (L.windFreq     >= 0) glUniform1f(L.windFreq, windFreq_);
}

// Conservative world-space bounds per cullable node type. The contract is
// strict — the box must contain everything the node can rasterize this frame,
// so culling can never pop visible content: skinned meshes use palette-posed
// bounds, wind sway pads by its max displacement, splats pad by the 3-sigma
// quad extent the splat shader emits.
bool SceneRenderer::nodeWorldBounds(SceneNode* n, bromath::AABB3& out) const {
    switch (n->type()) {
    case SceneNode::Type::Mesh: {
        auto* m = static_cast<MeshNode*>(n);
        if (m->mesh().empty()) return false;
        bromath::AABB3 local = m->localBounds();
        auto* sm = m->asSkinnedMesh();
        if (sm && sm->skinReady()) local = sm->posedLocalBounds();
        out = bromath::atransform(local, m->worldMatrix());
        // Wind sway displaces vertices in world space by at most
        // |windDir| * strength * windMask (per-vertex bend <= 1).
        float pad = m->windMask() * windStrength_ *
                    bromath::vlen(Vec3{windDir_[0], windDir_[1], windDir_[2]});
        if (pad > 0.0f) {
            out.min = out.min - Vec3{pad, pad, pad};
            out.max = out.max + Vec3{pad, pad, pad};
        }
        return true;
    }
    case SceneNode::Type::InstancedMesh: {
        auto* m = static_cast<InstancedMeshNode*>(n);
        float lo[3], hi[3];
        if (!m->computeWorldInstanceBounds(lo, hi)) return false;
        out.min = {lo[0], lo[1], lo[2]};
        out.max = {hi[0], hi[1], hi[2]};
        return true;
    }
    case SceneNode::Type::GaussianSplat: {
        auto* s = static_cast<GaussianSplatNode*>(n);
        if (s->splatCount() == 0) return false;
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
        return true;
    }
    case SceneNode::Type::Particles3D:
        return static_cast<Particles3DNode*>(n)->worldBounds(out);
    default:
        return false;
    }
}

bool SceneRenderer::cameraCulled(SceneNode* n) const {
    if (!cullingActive_) return false;
    bromath::AABB3 wb;
    if (!nodeWorldBounds(n, wb)) return false;
    return !bromath::fintersects(cameraFrustum_, wb);
}

void SceneRenderer::render3D() {
    hasMeshContent_ = false;
    cullStats_ = CullStats{};

    // Check for 3D content: mesh nodes OR world-anchored Shape/Sprite/Html.
    // Both render into the mesh FBO (depth-tested against each other) so
    // they share the same setup path.
    bool hasMeshNodes = false;
    bool hasInstancedMeshNodes = false;
    bool hasSplatNodes = false;
    bool hasParticle3DNodes = false;
    bool hasBillboardNodes = false;
    bool hasLightIcons = false;
    for (auto& [id, node] : graph_.nodes_) {
        if (!node->visible()) continue;
        if (node->type() == SceneNode::Type::Mesh) hasMeshNodes = true;
        else if (node->type() == SceneNode::Type::InstancedMesh) hasInstancedMeshNodes = true;
        else if (node->type() == SceneNode::Type::GaussianSplat) hasSplatNodes = true;
        else if (node->type() == SceneNode::Type::Particles3D) hasParticle3DNodes = true;
        else if (node->hasWorldAnchor())           hasBillboardNodes = true;
        else if (showLightIcons_ && node->type() == SceneNode::Type::Light) hasLightIcons = true;
    }

    // Resolve the gizmo overlay up-front so it can force the 3D pass even
    // when the canvas has no other 3D content. Cached and replayed below.
    std::vector<MeshNode*> gizmoMeshes;
    if (graph_.gizmoProvider_) gizmoMeshes = graph_.gizmoProvider_(&graph_);
    const bool hasGizmo = !gizmoMeshes.empty();

    const bool has3D = (hasMeshNodes || hasInstancedMeshNodes || hasSplatNodes || hasParticle3DNodes || hasBillboardNodes || hasGizmo || hasLightIcons)
                       && graph_.canvasWidth_ > 0 && graph_.canvasHeight_ > 0;

    // Per-frame culling state. World-space camera frustum: the passes render
    // camera-relative (proj * viewRot * (model - eye)), which is algebraically
    // proj * view * model, so testing world AABBs against proj*view matches
    // what the rasterizer clips exactly. Gribb-Hartmann extraction is
    // projection-agnostic, so ortho cameras work unchanged.
    cullingActive_ = frustumCullingEnabled_ && has3D;
    if (cullingActive_) {
        cameraFrustum_ = bromath::ffromViewProj(
            bromath::mmul(graph_.projectionMatrix_, graph_.viewMatrix_));
    }

    if (has3D) {
        ensureMeshPipeline();
        if (hasBillboardNodes || hasLightIcons) ensureBillboardPipeline();
        ensureMeshFBO();

        // Collect lights once per frame — reused for shadow + mesh + gizmo
        // passes. Done before any FBO bind so the shadow pass can manage
        // its own FBO state cleanly.
        std::vector<LightNode*> lights;
        collectLights(lights);
        static LightNode implicitSun;
        implicitSun.setKind(LightNode::Kind::Directional);
        implicitSun.setDirection(bromath::vnorm(Vec3(-0.3f, -1.0f, -0.5f)));
        implicitSun.setColor(1.0f, 0.98f, 0.95f);
        implicitSun.setIntensity(3.0f);
        std::vector<LightNode*> fallback;
        if (lights.empty()) { fallback.push_back(&implicitSun); }
        const auto& activeLights = lights.empty() ? fallback : lights;

        // Shadow caster pass renders into the shadow atlas (its own FBO).
        // Returns with FBO unbound; the mesh pass below rebinds meshFBO_.
        prepareShadows(activeLights);
        renderShadowPass();

        if (meshFBO_) {
            // MSAA: when active, the HDR passes below render into the
            // multisampled FBO and resolve into meshFBO_'s single-sampled
            // textures — depth mid-frame (before the depth-sampling blended
            // passes; see the depth-resolve comment below), color once at
            // the end, right before the tonemap pass.
            ensureMSAAFBO();
            const GLuint hdrFBO = msaaActive_ ? msaaFBO_ : meshFBO_;
            glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);
            glViewport(0, 0, meshFBOWidth_, meshFBOHeight_);
            // Reset state that Ganesh may have changed.
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_BLEND);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);

            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);

            hasMeshContent_ = true;

            // Skybox first — paints the IBL cubemap into the cleared FBO so
            // subsequent geometry naturally composits over it. No-op when no
            // environment is loaded; depth state is left as the geometry
            // pass expects (test on, write on, LESS).
            renderSkyboxPass();

            // --- Mesh pass --------------------------------------------------
            // Lit meshes render to the HDR FBO (pass through tonemap). Unlit
            // meshes are deferred to a post-tonemap overlay pass so their
            // authored colors aren't desaturated by ACES. Skinned meshes
            // (SkinnedMeshNode with a ready skin) are collected during the
            // same walk and drawn right after with the SKINNED program
            // variant; a skinned node whose skin doesn't match its mesh
            // degrades to the static path.
            std::vector<MeshNode*> unlitMeshes;
            std::vector<MeshNode*> skinnedMeshes;
            std::vector<MeshNode*> unlitSkinnedMeshes;
            if (hasMeshNodes && meshProgram_) {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glUseProgram(meshProgram_);

                uploadMeshGlobals(meshDraw_);
                uploadLights(activeLights, meshLocs_);

                std::function<void(SceneNode*)> walkMesh = [&](SceneNode* n) {
                    if (!n->visible()) return;
                    if (n->type() == SceneNode::Type::Mesh) {
                        auto* m = static_cast<MeshNode*>(n);
                        if (cameraCulled(m)) {
                            cullStats_.meshCulled++;
                        } else {
                            cullStats_.meshDrawn++;
                            auto* sm = m->asSkinnedMesh();
                            if (sm && sm->skinReady()) {
                                (m->unlit() ? unlitSkinnedMeshes : skinnedMeshes)
                                    .push_back(m);
                            } else if (m->unlit()) {
                                unlitMeshes.push_back(m);
                            } else {
                                renderMeshNode(m, meshDraw_);
                            }
                        }
                    }
                    for (auto* c : n->children()) walkMesh(c);
                };
                walkMesh(graph_.root_.get());

                // Skinned sub-pass: identical state, skinned program.
                if (!skinnedMeshes.empty()) {
                    ensureSkinnedMeshPipeline();
                    if (meshSkinnedProgram_) {
                        glUseProgram(meshSkinnedProgram_);
                        uploadMeshGlobals(meshSkinnedDraw_);
                        uploadLights(activeLights, meshSkinnedLocs_);
                        for (MeshNode* m : skinnedMeshes)
                            renderMeshNode(m, meshSkinnedDraw_);
                        glUseProgram(meshProgram_);
                    }
                }

                glDisable(GL_CULL_FACE);
            }

            // --- Instanced mesh pass ----------------------------------------
            // Same camera/lighting state as the regular pass. Per-instance
            // attributes carry the node-local transform; renderInstancedMeshNode
            // additionally uploads the node's own worldMatrix() (uInstModel) so
            // instances parented under a transformed node (e.g. TileWorld's
            // origin) land in the right place, same as uModel does for
            // renderMeshNode. Walks the same node tree filtered by InstancedMesh
            // type.
            if (hasInstancedMeshNodes) {
                ensureInstancedMeshPipeline();
                if (meshInstancedProgram_) {
                    glEnable(GL_CULL_FACE);
                    glCullFace(GL_BACK);
                    glUseProgram(meshInstancedProgram_);

                    Mat4 viewRot = graph_.viewMatrix_;
                    viewRot.at(0, 3) = 0.0f;
                    viewRot.at(1, 3) = 0.0f;
                    viewRot.at(2, 3) = 0.0f;
                    Mat4 vp = bromath::mmul(graph_.projectionMatrix_, viewRot);
                    glUniformMatrix4fv(uInstVP_, 1, GL_FALSE, vp.data);
                    glUniform3f(uInstCameraEye_, graph_.cameraEye_.x, graph_.cameraEye_.y, graph_.cameraEye_.z);

                    glUniform1f(uInstFogStart_, fogStart_);
                    glUniform1f(uInstFogEnd_, fogEnd_);
                    glUniform3f(uInstFogColor_, fogColor_[0], fogColor_[1], fogColor_[2]);
                    glUniform3f(uInstAmbient_, ambientColor_[0], ambientColor_[1], ambientColor_[2]);
                    uploadLights(activeLights, meshInstLocs_);

                    std::function<void(SceneNode*)> walkInst = [&](SceneNode* n) {
                        if (!n->visible()) return;
                        if (n->type() == SceneNode::Type::InstancedMesh) {
                            // Whole-node test against the world bounds of all
                            // instances — no per-instance culling.
                            if (cameraCulled(n)) {
                                cullStats_.instancedCulled++;
                            } else {
                                cullStats_.instancedDrawn++;
                                renderInstancedMeshNode(static_cast<InstancedMeshNode*>(n));
                            }
                        }
                        for (auto* c : n->children()) walkInst(c);
                    };
                    walkInst(graph_.root_.get());

                    glDisable(GL_CULL_FACE);
                }
            }

            // --- MSAA depth resolve ----------------------------------------
            // The opaque passes above are the only depth writers, so the
            // multisampled depth is final here. Resolving now (a) fills
            // meshDepthTex_, which the soft-particle pass snapshots for its
            // depth fade (a multisampled attachment can't be sampled in GL
            // 3.3) and the post-tonemap unlit overlay depth-tests against,
            // and (b) keeps the ordering safe: the blended passes below
            // (splats, particles, billboards) never write depth, so this
            // resolved copy stays valid for the rest of the frame. Color
            // resolves separately, after all HDR passes, before tonemap.
            if (msaaActive_) {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, msaaFBO_);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, meshFBO_);
                glBlitFramebuffer(0, 0, meshFBOWidth_, meshFBOHeight_,
                                  0, 0, meshFBOWidth_, meshFBOHeight_,
                                  GL_DEPTH_BUFFER_BIT, GL_NEAREST);
                glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);
            }

            // --- Gaussian splat pass ---------------------------------------
            // After opaque geometry so splats depth-test against it; sorted +
            // blended internally (see renderGaussianSplatNodes).
            if (hasSplatNodes) {
                renderGaussianSplatNodes();
            }

            // --- 3D particle pass ------------------------------------------
            // Depth-tested against geometry, no depth writes, blended into
            // the HDR target pre-tonemap so additive systems bloom. One
            // instanced draw per system.
            if (hasParticle3DNodes) {
                renderParticles3DNodes();
            }

            // --- Billboard pass --------------------------------------------
            // Depth test on (occluded behind geometry), depth write off (so
            // multiple billboards don't occlude each other).
            if ((hasBillboardNodes || hasLightIcons) && bbProgram_) {
                glUseProgram(bbProgram_);
                glDepthMask(GL_FALSE);
                glEnable(GL_BLEND);
                // Premultiplied "over" — matches both our SDF/rect fills (we
                // premultiply in the fragment shader) and Skia textures (which
                // produce premultiplied output).
                glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

                // View-rotation-only matrix: in camera-relative space the eye
                // is at origin, so only orientation remains.
                Mat4 viewRot = graph_.viewMatrix_;
                viewRot.at(0, 3) = 0.0f;
                viewRot.at(1, 3) = 0.0f;
                viewRot.at(2, 3) = 0.0f;
                Mat4 vp = bromath::mmul(graph_.projectionMatrix_, viewRot);
                glUniformMatrix4fv(bbUVP_, 1, GL_FALSE, vp.data);
                glBindVertexArray(bbVAO_);

                std::function<void(SceneNode*)> walkBB = [&](SceneNode* n) {
                    if (!n->visible()) return;
                    if (n->hasWorldAnchor()) {
                        renderBillboardNode(n);
                    }
                    for (auto* c : n->children()) walkBB(c);
                };
                walkBB(graph_.root_.get());

                // Light marker icons (editor affordance). Drawn in the same
                // pass so they occlude correctly against geometry.
                if (hasLightIcons) {
                    for (auto& [id, node] : graph_.nodes_) {
                        if (!node->visible()) continue;
                        if (node->type() != SceneNode::Type::Light) continue;
                        renderLightIcon(static_cast<LightNode*>(node.get()));
                    }
                }

                glBindVertexArray(0);
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            }

            glUseProgram(0);
            glDisable(GL_DEPTH_TEST);

            // --- MSAA color resolve ----------------------------------------
            // Multisampled HDR -> meshColorTex_, which bloom and tonemap
            // read. Depth was already resolved before the blended passes.
            if (msaaActive_) {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, msaaFBO_);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, meshFBO_);
                glBlitFramebuffer(0, 0, meshFBOWidth_, meshFBOHeight_,
                                  0, 0, meshFBOWidth_, meshFBOHeight_,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // --- Tonemap pass ----------------------------------------------
            // HDR mesh FBO -> LDR output texture. The compositor reads the
            // LDR texture; the HDR texture is internal.
            runTonemapPass();

            // --- Post-tonemap unlit overlay --------------------------------
            // Unlit meshes (scene-editor axes, engine gizmo) render directly
            // into the LDR tonemap target so their authored colors aren't
            // desaturated by ACES. Shares the mesh FBO's depth buffer so they
            // still occlude against scene geometry. Gizmo handles disable
            // depth test to stay always-on-top.
            const bool hasOverlay = !unlitMeshes.empty() ||
                                    !unlitSkinnedMeshes.empty() || hasGizmo;
            if (hasOverlay && tonemapFBO_ && meshProgram_) {
                glBindFramebuffer(GL_FRAMEBUFFER, tonemapFBO_);
                glViewport(0, 0, tonemapFBOWidth_, tonemapFBOHeight_);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_FALSE);                  // scene depth stays intact
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glEnable(GL_BLEND);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glUseProgram(meshProgram_);
                glUniform1f(meshDraw_.fogStart, 0.0f);
                glUniform1f(meshDraw_.fogEnd, 0.0f);
                glUniform3f(meshDraw_.fogColor, 0.0f, 0.0f, 0.0f);
                glUniform3f(meshDraw_.ambient, 0.0f, 0.0f, 0.0f);
                // uUnlit is set per-mesh by renderMeshNode; still need light
                // uniforms uploaded (shader accesses count even if unused).
                uploadLights(activeLights, meshLocs_);

                for (MeshNode* mn : unlitMeshes) {
                    renderMeshNode(mn, meshDraw_);
                }

                // Unlit skinned meshes need the skinned program for the
                // palette deform; same zeroed fog/ambient contract.
                if (!unlitSkinnedMeshes.empty()) {
                    ensureSkinnedMeshPipeline();
                    if (meshSkinnedProgram_) {
                        glUseProgram(meshSkinnedProgram_);
                        glUniform1f(meshSkinnedDraw_.fogStart, 0.0f);
                        glUniform1f(meshSkinnedDraw_.fogEnd, 0.0f);
                        glUniform3f(meshSkinnedDraw_.fogColor, 0.0f, 0.0f, 0.0f);
                        glUniform3f(meshSkinnedDraw_.ambient, 0.0f, 0.0f, 0.0f);
                        uploadLights(activeLights, meshSkinnedLocs_);
                        for (MeshNode* mn : unlitSkinnedMeshes)
                            renderMeshNode(mn, meshSkinnedDraw_);
                        glUseProgram(meshProgram_);
                    }
                }

                if (hasGizmo) {
                    glDisable(GL_DEPTH_TEST);
                    for (MeshNode* mn : gizmoMeshes) {
                        if (!mn) continue;
                        renderMeshNode(mn, meshDraw_);
                    }
                    glEnable(GL_DEPTH_TEST);
                    hasMeshContent_ = true;
                }

                glDisable(GL_BLEND);
                glDisable(GL_CULL_FACE);
                glDepthMask(GL_TRUE);
                glDisable(GL_DEPTH_TEST);
                glUseProgram(0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }

            // --- Tilt-shift DOF post pass ------------------------------------
            // Runs on the finished LDR frame (tonemap + any overlay) and, when
            // enabled, produces postColorTex_ for the compositor via
            // finalColorTex(). No-op (clears tiltActive_) when disabled.
            runTiltShiftPass();
        }
    }
}

}  // namespace bro::scene
