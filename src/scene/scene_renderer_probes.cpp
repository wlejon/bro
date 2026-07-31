#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/reflection_probe_node.h"
#include "scene/skinned_mesh_node.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace bro::scene {

using bromath::Vec3;
using bromath::Mat4;

// ---------------------------------------------------------------------------
// Local reflection probes (Godot ReflectionProbe analog).
//
// Capture: 6 cube-face renders (90° FOV perspective) from the probe origin
// into the probe's HDR cubemap, then the shared GGX prefilter builds its
// specular roughness mip chain. The capture reuses the real scene draw path
// with a RESTRICTED feature set:
//   - skybox + OPAQUE mesh / skinned / custom / instanced draws only;
//   - excluded: translucent meshes, gaussian splats, 3D particles,
//     billboards, decals, SSR, probes themselves (no recursion), MSAA, and
//     the whole post stack (captures are raw HDR radiance — they tonemap
//     with the scene when reflected);
//   - shadows are ON: the capture runs right after renderShadowPass, so the
//     frame's atlas is fresh; the camera-relative shadow matrices are
//     rebaked for the probe eye. Spot/point shadows are exact; directional
//     CSM cascades stay fitted to the viewer camera, so geometry outside
//     their coverage simply captures unshadowed (out-of-frustum samples
//     read as lit).
//
// Application: per-draw CPU selection — a mesh binds the highest-priority
// probe whose box contains the mesh's bounds center (ties: smallest volume,
// i.e. the more local probe). One probe per draw (GL 3.3 forward — no
// clustered per-pixel probe lists); the mesh shader swaps the global IBL
// specular for the probe's chain, box-projected when enabled, fading back to
// global IBL over the `interior` margin near the box faces.
// ---------------------------------------------------------------------------

namespace {

inline Vec3 xformPoint(const Mat4& m, const Vec3& p) {
    return Vec3{
        m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
        m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
        m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

// True when the node's parent chain ends at the graph root (same "actually
// attached" rule collectLights applies).
inline bool attachedToRoot(SceneNode* n, SceneNode* root) {
    SceneNode* p = n;
    while (p && p->parent()) p = p->parent();
    return p == root;
}

} // namespace

void SceneRenderer::queryProbeLocs(GLuint prog, ProbeLocs& p) {
    auto U = [prog](const char* name) {
        return glGetUniformLocation(prog, name);
    };
    p.enabled       = U("uProbeEnabled");
    p.specular      = U("uProbeSpecular");
    p.worldToLocal  = U("uProbeWorldToLocal");
    p.localToWorld  = U("uProbeLocalToWorld");
    p.pos           = U("uProbePos");
    p.boxSize       = U("uProbeBoxSize");
    p.boxProjection = U("uProbeBoxProjection");
    p.intensity     = U("uProbeIntensity");
    p.blendDist     = U("uProbeBlendDist");
    p.maxLOD        = U("uProbeMaxLOD");
}

void SceneRenderer::collectFrameProbes() {
    frameProbes_.clear();
    for (auto& [id, node] : graph_.nodes_) {
        if (node->type() != SceneNode::Type::ReflectionProbe) continue;
        if (!node->renderVisible()) continue;
        auto* p = static_cast<ReflectionProbeNode*>(node.get());
        if (!p->hasData() || !p->prefilterCubemap()) continue;
        if (!attachedToRoot(p, graph_.root_.get())) continue;

        FrameProbe fp;
        fp.probe = p;
        fp.world = p->worldMatrix();
        fp.invWorld = bromath::minverse(fp.world);
        fp.pos = Vec3{fp.world.at(0, 3), fp.world.at(1, 3), fp.world.at(2, 3)};
        for (int c = 0; c < 3; ++c) {
            fp.boxSize[c] = std::sqrt(
                fp.world.at(0, c) * fp.world.at(0, c) +
                fp.world.at(1, c) * fp.world.at(1, c) +
                fp.world.at(2, c) * fp.world.at(2, c));
        }
        fp.volume = fp.boxSize[0] * fp.boxSize[1] * fp.boxSize[2];
        fp.priority = p->priority();
        frameProbes_.push_back(fp);
    }
    // Highest priority first; ties go to the smallest volume so the more
    // local probe wins. Selection below takes the first containing box.
    std::stable_sort(frameProbes_.begin(), frameProbes_.end(),
        [](const FrameProbe& a, const FrameProbe& b) {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.volume < b.volume;
        });

    // The push_backs above just invalidated every FrameProbe address, and the
    // capture/prefilter passes that ran before this bind their own cubemaps on
    // the same texture unit uploadProbeForDraw uses. Retire the per-program
    // probe caches again here so the mesh pass starts from a real upload
    // rather than trusting unit 9 to be where it left it.
    ++probeEpoch_;
}

void SceneRenderer::uploadProbeForDraw(SceneNode* node, const ProbeLocs& P) {
    if (P.enabled < 0) return;   // program without the probe uniform surface

    const FrameProbe* sel = nullptr;
    if (!probeCaptureActive_ && !frameProbes_.empty()) {
        bromath::AABB3 wb;
        Vec3 c;
        if (nodeWorldBounds(node, wb)) {
            c = (wb.min + wb.max) * 0.5f;
        } else {
            const Mat4& w = node->worldMatrix();
            c = Vec3{w.at(0, 3), w.at(1, 3), w.at(2, 3)};
        }
        for (const FrameProbe& fp : frameProbes_) {
            Vec3 lp = xformPoint(fp.invWorld, c);
            if (std::fabs(lp.x) <= 0.5f && std::fabs(lp.y) <= 0.5f &&
                std::fabs(lp.z) <= 0.5f) {
                sel = &fp;
                break;
            }
        }
    }

    // Everything below is a function of `sel` alone — the mesh only chose it.
    // So a run of meshes resolving to the same probe, and the common case of
    // resolving to none (still 4 GL calls a draw), can skip the whole upload.
    // Epoch first: a FrameProbe* from a previous frame points into a vector
    // that has since been cleared, so it must never even be compared.
    if (P.cacheEpoch == probeEpoch_ && P.cacheSel == sel) return;
    P.cacheEpoch = probeEpoch_;
    P.cacheSel = sel;

    if (!sel) {
        glUniform1i(P.enabled, 0);
        if (P.specular >= 0) {
            // Keep unit 9 pointing at a valid cube (strict core-profile
            // drivers reject draws with unbound sampler uniforms).
            glActiveTexture(GL_TEXTURE9);
            glBindTexture(GL_TEXTURE_CUBE_MAP, fallbackCube_);
            glUniform1i(P.specular, 9);
            glActiveTexture(GL_TEXTURE0);
        }
        return;
    }

    // Fold the camera-relative offset into the matrices so the shader can
    // feed vWorldPos (camera-relative) straight through:
    //   worldToLocal * translate(eye)  maps cam-rel -> probe unit-box space
    //   translate(-eye) * world        maps probe space -> cam-rel world
    const Vec3 eye = graph_.cameraEye_;
    Mat4 w2l = bromath::mmul(sel->invWorld, bromath::mtranslate(eye));
    Mat4 l2w = bromath::mmul(
        bromath::mtranslate(Vec3{-eye.x, -eye.y, -eye.z}), sel->world);

    glUniform1i(P.enabled, 1);
    if (P.worldToLocal >= 0)
        glUniformMatrix4fv(P.worldToLocal, 1, GL_FALSE, w2l.data);
    if (P.localToWorld >= 0)
        glUniformMatrix4fv(P.localToWorld, 1, GL_FALSE, l2w.data);
    if (P.pos >= 0)
        glUniform3f(P.pos, sel->pos.x - eye.x, sel->pos.y - eye.y,
                    sel->pos.z - eye.z);
    if (P.boxSize >= 0) glUniform3fv(P.boxSize, 1, sel->boxSize);
    if (P.boxProjection >= 0)
        glUniform1i(P.boxProjection, sel->probe->boxProjection() ? 1 : 0);
    if (P.intensity >= 0) glUniform1f(P.intensity, sel->probe->intensity());
    if (P.blendDist >= 0) glUniform1f(P.blendDist, sel->probe->interior());
    if (P.maxLOD >= 0)
        glUniform1f(P.maxLOD, (float)(sel->probe->prefilterMips() - 1));
    if (P.specular >= 0) {
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_CUBE_MAP, sel->probe->prefilterCubemap());
        glUniform1i(P.specular, 9);
        glActiveTexture(GL_TEXTURE0);
    }
}

void SceneRenderer::updateReflectionProbes(const std::vector<LightNode*>& lights) {
    // Gather first — a capture rewrites camera + GL state, so don't capture
    // mid-iteration over the node table.
    std::vector<ReflectionProbeNode*> pending;
    for (auto& [id, node] : graph_.nodes_) {
        if (node->type() != SceneNode::Type::ReflectionProbe) continue;
        if (!node->renderVisible()) continue;
        auto* p = static_cast<ReflectionProbeNode*>(node.get());
        if (!p->captureRequested()) continue;
        if (!attachedToRoot(p, graph_.root_.get())) continue;
        pending.push_back(p);
    }
    for (auto* p : pending) captureReflectionProbe(p, lights);
}

bool SceneRenderer::captureReflectionProbe(ReflectionProbeNode* probe,
                                           const std::vector<LightNode*>& lights) {
    ensureMeshPipeline();
    if (!meshProgram_) {
        probe->requestCapture();  // keep pending; pipeline may come up later
        return false;
    }
    ensureFallbackTextures();
    // Probes need the (env-independent) BRDF LUT even when no global
    // environment is loaded — the split-sum specular term uses it.
    ensureBRDFLUT();

    if (!probe->ensureTextures()) return false;
    const int res = probe->allocatedResolution();

    // Shared capture FBO + depth-stencil renderbuffer at the probe size.
    if (!probeCaptureFBO_) glGenFramebuffers(1, &probeCaptureFBO_);
    if (!probeDepthRBO_ || probeDepthSize_ != res) {
        if (probeDepthRBO_) glDeleteRenderbuffers(1, &probeDepthRBO_);
        glGenRenderbuffers(1, &probeDepthRBO_);
        glBindRenderbuffer(GL_RENDERBUFFER, probeDepthRBO_);
        glRenderbufferStorage(GL_RENDERBUFFER, depthStencilInternalFormat(), res, res);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        probeDepthSize_ = res;
    }

    // Save everything the capture overrides: caller FBO/viewport, the graph
    // camera (the capture drives the normal draw path through it), the
    // per-frame culling state, and the camera-relative shadow matrices.
    GLint prevFBO = 0, prevViewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    const Mat4 savedView = graph_.viewMatrix_;
    const Mat4 savedProj = graph_.projectionMatrix_;
    const Vec3 savedEye = graph_.cameraEye_;
    const float savedNear = graph_.cameraNearZ_;
    const float savedFar = graph_.cameraFarZ_;
    const float savedFov = graph_.cameraFovY_;
    const float savedAspect = graph_.cameraAspect_;
    const bool savedPersp = graph_.cameraIsPerspective_;
    const bool savedCullingActive = cullingActive_;
    const bromath::Frustum savedFrustum = cameraFrustum_;

    const Mat4& probeWorld = probe->worldMatrix();
    const Vec3 pos{probeWorld.at(0, 3), probeWorld.at(1, 3), probeWorld.at(2, 3)};

    // Rebake the camera-relative shadow matrices for the probe eye so the
    // frame's shadow atlas applies to the capture draws (the mesh shader
    // multiplies them against camera-relative positions).
    float savedShadowCamRel[kMaxShadowTiles][16];
    std::memcpy(savedShadowCamRel, shadowMatrixCamRel_, sizeof(savedShadowCamRel));
    {
        const Mat4 bias = bromath::mmul(
            bromath::mtranslate(Vec3{0.5f, 0.5f, 0.5f}),
            bromath::mscale(Vec3{0.5f, 0.5f, 0.5f}));
        const Mat4 t = bromath::mtranslate(pos);
        for (int s = 0; s < shadowTileCount_; ++s) {
            Mat4 lightVP;
            std::memcpy(lightVP.data, shadowRenderMatrix_[s], sizeof(lightVP.data));
            Mat4 cam = bromath::mmul(bromath::mmul(bias, lightVP), t);
            std::memcpy(shadowMatrixCamRel_[s], cam.data, sizeof(cam.data));
        }
    }

    probeCaptureActive_ = true;

    const float nearZ = 0.05f;
    const float farZ = savedFar > 1000.0f ? savedFar : 1000.0f;
    const float fovY = 1.57079632679f;   // 90°
    const Mat4 proj = makePerspective(fovY, 1.0f, nearZ, farZ);

    // Standard GL cube-face capture bases (+X -X +Y -Y +Z -Z with the
    // V-flip folded into the up vectors) so the cubemap samples correctly
    // by direction.
    static const Vec3 kDirs[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const Vec3 kUps[6] = {
        {0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};

    glBindFramebuffer(GL_FRAMEBUFFER, probeCaptureFBO_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, probeDepthRBO_);

    // Reset state Ganesh (or a prior pass) may have changed — mirrors the
    // main HDR FBO setup in render3D.
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glViewport(0, 0, res, res);

    bool ok = true;
    for (int face = 0; face < 6 && ok; ++face) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                               probe->captureCubemap(), 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("Reflection probe FBO incomplete on face %d: 0x%x",
                      face, status);
            ok = false;
            break;
        }

        graph_.viewMatrix_ = bromath::mlookAt(pos, pos + kDirs[face], kUps[face]);
        graph_.projectionMatrix_ = proj;
        graph_.cameraEye_ = pos;
        graph_.cameraNearZ_ = nearZ;
        graph_.cameraFarZ_ = farZ;
        graph_.cameraFovY_ = fovY;
        graph_.cameraAspect_ = 1.0f;
        graph_.cameraIsPerspective_ = true;
        cullingActive_ = frustumCullingEnabled_;
        if (cullingActive_) {
            cameraFrustum_ = makeFrustum(
                bromath::mmul(proj, graph_.viewMatrix_));
        }

        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClearDepth(depthClearFar());
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(depthFuncCloser());

        renderSkyboxPass();
        renderProbeSceneOpaque(lights);
    }

    if (ok) {
        // Mip the raw capture (the prefilter's Krivanek bias samples LODs),
        // then build the roughness chain with the shared GGX prefilter.
        glBindTexture(GL_TEXTURE_CUBE_MAP, probe->captureCubemap());
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        ok = runPrefilterInto(probe->captureCubemap(), res,
                              probe->prefilterCubemap(), res,
                              probe->prefilterMips());
    }

    if (ok) {
        probe->markCaptured();
    } else {
        // Drop the request so a broken capture doesn't retry (and spam the
        // log) every frame; the probe simply stays inactive.
        probe->clearCaptureRequest();
        LOG_ERROR("Reflection probe capture failed (node '%s')",
                  probe->name().c_str());
    }

    // Restore everything.
    probeCaptureActive_ = false;
    graph_.viewMatrix_ = savedView;
    graph_.projectionMatrix_ = savedProj;
    graph_.cameraEye_ = savedEye;
    graph_.cameraNearZ_ = savedNear;
    graph_.cameraFarZ_ = savedFar;
    graph_.cameraFovY_ = savedFov;
    graph_.cameraAspect_ = savedAspect;
    graph_.cameraIsPerspective_ = savedPersp;
    cullingActive_ = savedCullingActive;
    cameraFrustum_ = savedFrustum;
    std::memcpy(shadowMatrixCamRel_, savedShadowCamRel, sizeof(savedShadowCamRel));

    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return ok;
}

void SceneRenderer::renderProbeSceneOpaque(const std::vector<LightNode*>& lights) {
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    GLuint boundProg = 0;
    auto bindMeshProg = [&](GLuint prog, const MeshDrawLocs& d,
                            const MeshProgramLocs& locs) {
        if (prog == boundProg) return;
        boundProg = prog;
        glUseProgram(prog);
        uploadMeshGlobals(d);
        uploadLights(lights, locs);
    };

    Mat4 viewRot = graph_.viewMatrix_;
    viewRot.at(0, 3) = 0.0f;
    viewRot.at(1, 3) = 0.0f;
    viewRot.at(2, 3) = 0.0f;
    const Mat4 vp = bromath::mmul(graph_.projectionMatrix_, viewRot);

    auto bindInstProg = [&](GLuint prog, const InstancedDrawLocs& d,
                            const MeshProgramLocs& locs) {
        if (prog == boundProg) return;
        boundProg = prog;
        glUseProgram(prog);
        glUniformMatrix4fv(d.vp, 1, GL_FALSE, vp.data);
        glUniform3f(d.cameraEye, graph_.cameraEye_.x, graph_.cameraEye_.y,
                    graph_.cameraEye_.z);
        glUniform1f(d.fogStart, fogStart_);
        glUniform1f(d.fogEnd, fogEnd_);
        glUniform3f(d.fogColor, fogColor_[0], fogColor_[1], fogColor_[2]);
        glUniform1f(d.fogDensity, fogDensity_);
        glUniform1f(d.fogHeightFalloff, fogHeightFalloff_);
        glUniform1f(d.fogStartDist, fogStartDist_);
        glUniform1f(d.fogCamY, graph_.cameraEye_.y);
        glUniform3f(d.ambient, effectiveAmbient()[0], effectiveAmbient()[1],
                    effectiveAmbient()[2]);
        if (d.ssrMask >= 0) glUniform1i(d.ssrMask, 0);
        uploadLights(lights, locs);
    };

    std::function<void(SceneNode*)> walk = [&](SceneNode* n) {
        if (!n->renderVisible()) return;
        if (n->type() == SceneNode::Type::Mesh) {
            auto* m = static_cast<MeshNode*>(n);
            // Opaque only — the capture has no sorted translucent pass.
            if (!cameraCulled(m) && m->color()[3] >= 1.0f) {
                auto* sm = m->asSkinnedMesh();
                const bool skinnedReady = sm && sm->skinReady();
                CustomProgramEntry* entry = nullptr;
                if (m->hasCustomShader()) {
                    const auto* st = m->customShader();
                    std::string err;
                    entry = ensureCustomProgram(
                        skinnedReady ? CustomShaderTarget::Skinned
                                     : CustomShaderTarget::Static,
                        st->key, st->vertexChunk, st->fragmentChunk, &err);
                    if (!entry) {
                        LOG_ERROR("Custom shader failed in probe capture "
                                  "(rendering default): %s", err.c_str());
                    }
                }
                if (entry) {
                    bindMeshProg(entry->prog, entry->draw, entry->locs);
                    uploadUserUniforms(entry->prog, entry->userLocs,
                                       m->customShader());
                    uploadUserTextures(entry->prog, entry->userLocs, m);
                    renderMeshNode(m, entry->draw);
                } else if (skinnedReady) {
                    ensureSkinnedMeshPipeline();
                    if (meshSkinnedProgram_) {
                        bindMeshProg(meshSkinnedProgram_, meshSkinnedDraw_,
                                     meshSkinnedLocs_);
                        renderMeshNode(m, meshSkinnedDraw_);
                    }
                } else if (meshProgram_) {
                    bindMeshProg(meshProgram_, meshDraw_, meshLocs_);
                    renderMeshNode(m, meshDraw_);
                }
            }
        } else if (n->type() == SceneNode::Type::InstancedMesh) {
            auto* m = static_cast<InstancedMeshNode*>(n);
            ensureInstancedMeshPipeline();
            if (meshInstancedProgram_ && !cameraCulled(m) &&
                m->color()[3] >= 1.0f) {
                CustomProgramEntry* entry = nullptr;
                if (m->hasCustomShader()) {
                    const auto* st = m->customShader();
                    std::string err;
                    entry = ensureCustomProgram(CustomShaderTarget::Instanced,
                                                st->key, st->vertexChunk,
                                                st->fragmentChunk, &err);
                    if (!entry) {
                        LOG_ERROR("Custom shader failed in probe capture "
                                  "(rendering default): %s", err.c_str());
                    }
                }
                if (entry) {
                    bindInstProg(entry->prog, entry->instDraw, entry->locs);
                    uploadUserUniforms(entry->prog, entry->userLocs,
                                       m->customShader());
                    renderInstancedMeshNode(m, entry->instDraw);
                } else {
                    bindInstProg(meshInstancedProgram_, meshInstDraw_,
                                 meshInstLocs_);
                    renderInstancedMeshNode(m, meshInstDraw_);
                }
            }
        }
        for (auto* c : n->children()) walk(c);
    };
    walk(graph_.root_.get());

    glDisable(GL_CULL_FACE);
    glUseProgram(0);
}

}  // namespace bro::scene
