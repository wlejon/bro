#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/particles3d_node.h"
#include "util/log.h"

#include <cmath>

#include "particles3d.vert.h"
#include "particles3d.frag.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Mat4;

void SceneRenderer::ensureParticlePipeline() {
    if (particleProgram_) return;

    particleProgram_ = linkProgram(kParticles3DVertSrc, kParticles3DFragSrc,
                                   "Particle3D program");
    if (!particleProgram_) return;

    auto getU = [&](const char* n) { return glGetUniformLocation(particleProgram_, n); };
    pUVP_        = getU("uVP");
    pUModel_     = getU("uModel");
    pUCameraEye_ = getU("uCameraEye");
    pURight_     = getU("uRight");
    pUUp_        = getU("uUp");
    pUFlipGrid_  = getU("uFlipGrid");
    pUMode_      = getU("uMode");
    pUTex_       = getU("uTex");
    pUSceneDepth_   = getU("uSceneDepth");
    pUViewport_     = getU("uViewport");
    pUDepthRange_   = getU("uDepthRange");
    pUPerspective_  = getU("uPerspective");
    pUSoftDistance_ = getU("uSoftDistance");

    // Shared unit quad ([-1,1] both axes, two triangles). Every particle
    // system's per-node VAO binds this buffer at attribute 0.
    static const float quadVerts[12] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f,
    };
    glGenBuffers(1, &particleQuadVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, particleQuadVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// Depth-blit target for soft particles, sized to the mesh FBO. No color
// attachment, so both draw and read buffers are set to NONE for completeness.
void SceneRenderer::ensureSceneDepthCopy() {
    if (sceneDepthCopyFBO_ && sceneDepthCopyWidth_ == meshFBOWidth_ &&
        sceneDepthCopyHeight_ == meshFBOHeight_) return;

    destroySceneDepthCopy();

    sceneDepthCopyWidth_  = meshFBOWidth_;
    sceneDepthCopyHeight_ = meshFBOHeight_;

    // DEPTH24_STENCIL8 to match the mesh FBO's depth attachment —
    // glBlitFramebuffer requires identical depth formats on both ends.
    glGenTextures(1, &sceneDepthCopyTex_);
    glBindTexture(GL_TEXTURE_2D, sceneDepthCopyTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8,
                 sceneDepthCopyWidth_, sceneDepthCopyHeight_, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &sceneDepthCopyFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneDepthCopyFBO_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, sceneDepthCopyTex_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Scene depth copy FBO incomplete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroySceneDepthCopy();
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneRenderer::destroySceneDepthCopy() {
    if (sceneDepthCopyTex_) { glDeleteTextures(1, &sceneDepthCopyTex_); sceneDepthCopyTex_ = 0; }
    if (sceneDepthCopyFBO_) { glDeleteFramebuffers(1, &sceneDepthCopyFBO_); sceneDepthCopyFBO_ = 0; }
    sceneDepthCopyWidth_ = sceneDepthCopyHeight_ = 0;
}

// One instanced draw per particle system into the HDR mesh FBO. Runs after
// the opaque + splat passes: depth-tested against geometry (occluded behind
// walls) but not depth-writing (particles blend over each other — Normal
// systems are CPU-sorted back-to-front inside drawInstanced, Additive is
// order-independent). Rendering pre-tonemap means additive stacks push HDR
// luminance past the bloom threshold and glow for free.
//
// Soft particles: when a system requests softness > 0, its fragments fade
// over `softness` world units of depth gap to the opaque scene. The shader
// can't sample the depth texture attached to the current draw FBO (a
// framebuffer feedback loop in strict GL 3.3 even with glDepthMask(FALSE)),
// so the opaque depth is first snapshotted with a glBlitFramebuffer into
// sceneDepthCopyTex_. With MSAA on, render3D already resolved the
// multisampled depth into meshDepthTex_ before this pass, so meshFBO_ is
// always the single-sampled blit source here.
void SceneRenderer::renderParticles3DNodes() {
    ensureParticlePipeline();
    ensureFallbackTextures();
    if (!particleProgram_) return;

    bool wantSoft = false;
    for (auto& [id, node] : graph_.nodes_) {
        if (!node->renderVisible()) continue;
        if (node->type() != SceneNode::Type::Particles3D) continue;
        auto* p = static_cast<Particles3DNode*>(node.get());
        if (p->liveCount() > 0 && p->softness() > 0.0f) { wantSoft = true; break; }
    }

    bool depthReady = false;
    if (wantSoft && meshFBO_) {
        ensureSceneDepthCopy();
        if (sceneDepthCopyFBO_) {
            GLint prevFBO = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFBO);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, meshFBO_);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneDepthCopyFBO_);
            glBlitFramebuffer(0, 0, meshFBOWidth_, meshFBOHeight_,
                              0, 0, meshFBOWidth_, meshFBOHeight_,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFBO));
            depthReady = true;
        }
    }

    glUseProgram(particleProgram_);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    // Camera-relative VP, same as the mesh/instanced/billboard passes.
    Mat4 viewRot = graph_.viewMatrix_;
    viewRot.at(0, 3) = 0.0f;
    viewRot.at(1, 3) = 0.0f;
    viewRot.at(2, 3) = 0.0f;
    Mat4 vp = bromath::mmul(graph_.projectionMatrix_, viewRot);
    glUniformMatrix4fv(pUVP_, 1, GL_FALSE, vp.data);

    const Vec3& eye = graph_.cameraEye_;
    glUniform3f(pUCameraEye_, eye.x, eye.y, eye.z);

    const auto& V = graph_.viewMatrix_;
    const Vec3 camRight{V.at(0, 0), V.at(0, 1), V.at(0, 2)};
    const Vec3 camUp   {V.at(1, 0), V.at(1, 1), V.at(1, 2)};
    const Vec3 camFwd  {-V.at(2, 0), -V.at(2, 1), -V.at(2, 2)};
    glUniform3f(pURight_, camRight.x, camRight.y, camRight.z);
    glUniform3f(pUUp_, camUp.x, camUp.y, camUp.z);

    glActiveTexture(GL_TEXTURE0);
    glUniform1i(pUTex_, 0);

    // Scene depth on unit 1. Bound even when no system is soft (fallback2D_)
    // so the sampler stays valid on strict core-profile drivers; the shader
    // only reads it when uSoftDistance > 0.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, depthReady ? sceneDepthCopyTex_ : fallback2D_);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(pUSceneDepth_, 1);
    glUniform2f(pUViewport_, static_cast<float>(meshFBOWidth_),
                             static_cast<float>(meshFBOHeight_));
    glUniform2f(pUDepthRange_, graph_.cameraNearZ_, graph_.cameraFarZ_);
    glUniform1i(pUPerspective_, graph_.cameraIsPerspective_ ? 1 : 0);

    static const Mat4 kIdentity = bromath::midentity();

    for (auto& [id, node] : graph_.nodes_) {
        if (!node->renderVisible()) continue;
        if (node->type() != SceneNode::Type::Particles3D) continue;
        auto* p = static_cast<Particles3DNode*>(node.get());
        if (p->liveCount() <= 0) continue;
        if (cameraCulled(p)) {
            cullStats_.particlesCulled++;
            continue;
        }
        cullStats_.particlesDrawn++;

        if (p->blend() == Particles3DNode::Blend::Additive) {
            // Additive color; alpha still accumulates "over"-style so the
            // compositor sees coverage where particles glow over nothing.
            glBlendFuncSeparate(GL_ONE, GL_ONE,
                                GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }

        const Mat4& model = (p->space() == Particles3DNode::SimSpace::Local)
                          ? p->worldMatrix() : kIdentity;
        glUniformMatrix4fv(pUModel_, 1, GL_FALSE, model.data);
        glUniform2f(pUFlipGrid_, static_cast<float>(p->sheetCols()),
                                 static_cast<float>(p->sheetRows()));
        glUniform1f(pUSoftDistance_, depthReady ? p->softness() : 0.0f);

        GLuint tex = p->ensureTextureGL();
        glBindTexture(GL_TEXTURE_2D, tex ? tex : fallback2D_);
        glUniform1i(pUMode_, tex ? 1 : 0);

        p->drawInstanced(particleQuadVBO_, camFwd);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

}  // namespace bro::scene
