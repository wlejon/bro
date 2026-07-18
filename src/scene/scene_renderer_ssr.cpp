#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "util/log.h"

#include "post.vert.h"
#include "ssr.frag.h"

namespace bro::scene {

using bromath::Mat4;

// ---------------------------------------------------------------------------
// Screen-space reflections (opaque surfaces, right after the decal pass)
// ---------------------------------------------------------------------------
//
// See ssr.frag for the full algorithm. The C++ side owns the two snapshots
// the shader needs:
//   - ssrSourceTex_: the opaque+decal HDR color, whose alpha channel carries
//     the per-pixel reflectance mask the mesh shaders wrote during the SSR
//     mask phase (uSSRMask — see render3D / uploadMeshGlobals). With MSAA
//     active the blit from the multisampled FBO doubles as the resolve.
//   - sceneDepthCopyTex_: the resolved opaque depth (same snapshot the decal
//     and soft-particle passes use — the draw FBO's own depth attachment can
//     never be sampled in strict GL 3.3).
// The full-screen draw then rewrites the current HDR target in place with
// blending disabled: reflections mixed into rgb, alpha restored from mask to
// coverage. Running this pass is MANDATORY whenever the mask phase was
// active this frame — skipping it would leave reflectance values in the
// coverage channel the compositor reads.

void SceneRenderer::ensureSSRPipeline() {
    if (ssrProgram_ || ssrPipelineFailed_) return;

    ssrProgram_ = linkProgram(kPostVertSrc, kSSRFragSrc, "SSR program");
    if (!ssrProgram_) return;

    ssrUColor_       = glGetUniformLocation(ssrProgram_, "uColorTex");
    ssrUDepth_       = glGetUniformLocation(ssrProgram_, "uDepthTex");
    ssrUProj_        = glGetUniformLocation(ssrProgram_, "uProj");
    ssrUInvProj_     = glGetUniformLocation(ssrProgram_, "uInvProj");
    ssrUPerspective_ = glGetUniformLocation(ssrProgram_, "uPerspective");
    ssrUMaxDistance_ = glGetUniformLocation(ssrProgram_, "uMaxDistance");
    ssrUSteps_       = glGetUniformLocation(ssrProgram_, "uSteps");
    ssrUThickness_   = glGetUniformLocation(ssrProgram_, "uThickness");
    ssrUIntensity_   = glGetUniformLocation(ssrProgram_, "uIntensity");
    ssrUEdgeFade_    = glGetUniformLocation(ssrProgram_, "uEdgeFade");
}

void SceneRenderer::ensureSSRFBO() {
    // Full-res RGBA16F, matching the mesh FBO: the blit source is HDR and
    // a multisample resolve blit requires identical formats.
    if (meshFBOWidth_ <= 0 || meshFBOHeight_ <= 0) return;
    const int tw = meshFBOWidth_;
    const int th = meshFBOHeight_;
    if (ssrFBO_ && ssrWidth_ == tw && ssrHeight_ == th) return;
    destroySSRFBO();

    ssrWidth_  = tw;
    ssrHeight_ = th;
    glGenFramebuffers(1, &ssrFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, ssrFBO_);
    glGenTextures(1, &ssrSourceTex_);
    glBindTexture(GL_TEXTURE_2D, ssrSourceTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, tw, th, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ssrSourceTex_, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("SSR FBO incomplete: 0x%x", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneRenderer::destroySSRFBO() {
    if (ssrSourceTex_) { glDeleteTextures(1, &ssrSourceTex_); ssrSourceTex_ = 0; }
    if (ssrFBO_)       { glDeleteFramebuffers(1, &ssrFBO_); ssrFBO_ = 0; }
    ssrWidth_ = ssrHeight_ = 0;
}

void SceneRenderer::runSSRPass() {
    // The current draw FBO is the frame's HDR target (msaaFBO_ when MSAA is
    // active, else meshFBO_). Captured BEFORE the ensure* calls — the lazy
    // FBO-creation paths leave the binding at 0 (same trap as the decal
    // pass documents).
    GLint prevFBO = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFBO);

    ensureTonemapPipeline();   // shared full-screen quad VAO
    ensureSSRPipeline();
    ensureSSRFBO();
    ensureSceneDepthCopy();
    if (!ssrProgram_ || !ssrFBO_ || !sceneDepthCopyFBO_ || !tonemapVAO_) {
        // Init failed with the mask already written this frame — one frame
        // of off coverage. The sticky latch keeps the mask phase disabled
        // from the next frame on, so this can't recur.
        if (!ssrPipelineFailed_) {
            LOG_ERROR("SSR pass unavailable (pipeline/FBO init failed); "
                      "disabling SSR");
            ssrPipelineFailed_ = true;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFBO));
        return;
    }

    // Depth snapshot: meshFBO_ always holds the resolved single-sampled
    // depth here (render3D resolves MSAA depth before the decal/SSR slot).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, meshFBO_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneDepthCopyFBO_);
    glBlitFramebuffer(0, 0, meshFBOWidth_, meshFBOHeight_,
                      0, 0, meshFBOWidth_, meshFBOHeight_,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    // Color snapshot (rgb = lit opaque+decal HDR, a = reflectance mask).
    // Reading from the multisampled FBO makes this blit the resolve.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevFBO));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ssrFBO_);
    glBlitFramebuffer(0, 0, meshFBOWidth_, meshFBOHeight_,
                      0, 0, meshFBOWidth_, meshFBOHeight_,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFBO));

    // Full-screen composite, in place. Depth test off (no depth writes
    // happen with the test disabled, so the resolved depth stays valid);
    // blending off — the shader rewrites rgb AND alpha, discarding on sky
    // pixels so their color/coverage samples stay untouched.
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, meshFBOWidth_, meshFBOHeight_);

    glUseProgram(ssrProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ssrSourceTex_);
    glUniform1i(ssrUColor_, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneDepthCopyTex_);
    glUniform1i(ssrUDepth_, 1);
    glActiveTexture(GL_TEXTURE0);

    // The depth buffer was written camera-relative (proj * viewRot *
    // (model - eye) == proj * view * model), so proj/invProj alone move
    // between window depth and view space — exact for perspective AND ortho
    // (same contract as the SSAO and DoF passes).
    const Mat4& proj = graph_.projectionMatrix_;
    const Mat4 invProj = bromath::minverse(proj);
    glUniformMatrix4fv(ssrUProj_, 1, GL_FALSE, proj.data);
    glUniformMatrix4fv(ssrUInvProj_, 1, GL_FALSE, invProj.data);
    glUniform1i(ssrUPerspective_, graph_.cameraIsPerspective_ ? 1 : 0);
    glUniform1f(ssrUMaxDistance_, ssrMaxDistance_);
    glUniform1i(ssrUSteps_, ssrSteps_);
    glUniform1f(ssrUThickness_, ssrThickness_);
    glUniform1f(ssrUIntensity_, ssrIntensity_);
    glUniform1f(ssrUEdgeFade_, ssrEdgeFade_);

    glBindVertexArray(tonemapVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    // Restore what the surrounding passes expect (depth test on, LESS —
    // untouched above — depth writes on, no blend — untouched).
    glEnable(GL_DEPTH_TEST);
}

}  // namespace bro::scene
