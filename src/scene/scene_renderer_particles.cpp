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

// One instanced draw per particle system into the HDR mesh FBO. Runs after
// the opaque + splat passes: depth-tested against geometry (occluded behind
// walls) but not depth-writing (particles blend over each other — Normal
// systems are CPU-sorted back-to-front inside drawInstanced, Additive is
// order-independent). Rendering pre-tonemap means additive stacks push HDR
// luminance past the bloom threshold and glow for free.
//
// Soft particles (depth-fade at geometry intersections) are intentionally
// absent: the scene depth attachment is a renderbuffer (meshDepthRBO_, also
// shared by the tonemap FBO's overlay pass), so sampling scene depth would
// need an RBO->texture conversion plus a per-frame depth copy to dodge the
// framebuffer-feedback-loop rule. Revisit if the depth attachment becomes a
// texture for other reasons.
void SceneRenderer::renderParticles3DNodes() {
    ensureParticlePipeline();
    ensureFallbackTextures();
    if (!particleProgram_) return;

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

    static const Mat4 kIdentity = bromath::midentity();

    for (auto& [id, node] : graph_.nodes_) {
        if (!node->visible()) continue;
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

        GLuint tex = p->ensureTextureGL();
        glBindTexture(GL_TEXTURE_2D, tex ? tex : fallback2D_);
        glUniform1i(pUMode_, tex ? 1 : 0);

        p->drawInstanced(particleQuadVBO_, camFwd);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

}  // namespace bro::scene
