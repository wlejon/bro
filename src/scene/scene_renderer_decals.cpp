#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/decal_node.h"
#include "util/log.h"

#include <algorithm>
#include <functional>

#include "decal.vert.h"
#include "decal.frag.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Mat4;

void SceneRenderer::ensureDecalPipeline() {
    if (decalProgram_) return;

    decalProgram_ = linkProgram(kDecalVertSrc, kDecalFragSrc, "Decal program");
    if (!decalProgram_) return;

    auto getU = [&](const char* n) {
        return glGetUniformLocation(decalProgram_, n);
    };
    dcUMVP_              = getU("uMVP");
    dcUInvViewProj_      = getU("uInvViewProj");
    dcUInvModel_         = getU("uInvModel");
    dcUDecalUp_          = getU("uDecalUp");
    dcUViewport_         = getU("uViewport");
    dcUSceneDepth_       = getU("uSceneDepth");
    dcUAlbedoTex_        = getU("uAlbedoTex");
    dcUEmissionTex_      = getU("uEmissionTex");
    dcUHasAlbedo_        = getU("uHasAlbedo");
    dcUHasEmission_      = getU("uHasEmission");
    dcUModulate_         = getU("uModulate");
    dcUEmissionStrength_ = getU("uEmissionStrength");
    dcUUpperFade_        = getU("uUpperFade");
    dcULowerFade_        = getU("uLowerFade");
    dcUNormalFade_       = getU("uNormalFade");
    dcUAmbient_          = getU("uAmbient");
    dcUSunDir_           = getU("uSunDir");
    dcUSunColor_         = getU("uSunColor");

    // Unit cube [-0.5, 0.5]^3, 12 triangles, CCW winding facing OUTWARD —
    // the pass culls FRONT faces and rasterizes the back faces so a camera
    // inside the volume still covers the right screen area.
    static const float c = 0.5f;
    static const float verts[36 * 3] = {
        // -X face
        -c,-c,-c,  -c,-c, c,  -c, c, c,   -c,-c,-c,  -c, c, c,  -c, c,-c,
        // +X face
         c,-c,-c,   c, c,-c,   c, c, c,    c,-c,-c,   c, c, c,   c,-c, c,
        // -Y face
        -c,-c,-c,   c,-c,-c,   c,-c, c,   -c,-c,-c,   c,-c, c,  -c,-c, c,
        // +Y face
        -c, c,-c,  -c, c, c,   c, c, c,   -c, c,-c,   c, c, c,   c, c,-c,
        // -Z face
        -c,-c,-c,  -c, c,-c,   c, c,-c,   -c,-c,-c,   c, c,-c,   c,-c,-c,
        // +Z face
        -c,-c, c,   c,-c, c,   c, c, c,   -c,-c, c,   c, c, c,  -c, c, c,
    };

    glGenBuffers(1, &decalVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, decalVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glGenVertexArrays(1, &decalVAO_);
    glBindVertexArray(decalVAO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          (void*)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// Draw every visible decal volume onto the opaque HDR result. Runs right
// after the opaque passes (and the MSAA depth resolve, so meshDepthTex_ is
// final and single-sampled) and before the translucent/splat/particle/
// billboard passes — decals receive opaque depth correctly AND stay under
// particles.
//
// The fragment shader reconstructs the opaque surface position from a depth
// snapshot: like the soft-particle pass, the draw FBO's own depth attachment
// can't be sampled (GL 3.3 feedback loop), so the resolved depth is first
// blitted into sceneDepthCopyTex_. Boxes render back-faces with depth-test
// GEQUAL and no depth writes: camera-inside-the-volume still rasterizes, and
// pixels where the scene is entirely behind the volume are skipped by the
// test. All GL state touched here is restored to what the surrounding
// passes in render3D expect (LESS depth func, no blend, no cull, depth
// writes on).
void SceneRenderer::renderDecalPass(const std::vector<LightNode*>& lights) {
    // Gather visible decals through the tree walk (visibility prunes whole
    // subtrees, matching every other pass), frustum-culled on the box's
    // world AABB via the shared machinery.
    std::vector<DecalNode*> decals;
    std::function<void(SceneNode*)> walk = [&](SceneNode* n) {
        if (!n->renderVisible()) return;
        if (n->type() == SceneNode::Type::Decal) {
            auto* d = static_cast<DecalNode*>(n);
            if (cameraCulled(d)) {
                cullStats_.decalsCulled++;
            } else {
                cullStats_.decalsDrawn++;
                decals.push_back(d);
            }
        }
        for (auto* c : n->children()) walk(c);
    };
    walk(graph_.root_.get());
    if (decals.empty()) return;

    // Draw order: ascending renderPriority (higher priority blends later,
    // i.e. on top); stable so equal priorities keep tree order.
    std::stable_sort(decals.begin(), decals.end(),
                     [](DecalNode* a, DecalNode* b) {
                         return a->renderPriority() < b->renderPriority();
                     });

    ensureDecalPipeline();
    ensureFallbackTextures();
    if (!decalProgram_ || !meshFBO_) return;

    // Apply staged texture uploads for every decal BEFORE any sampler
    // bindings below — the upload path binds GL_TEXTURE_2D on whatever unit
    // is active, so flushing mid-loop would clobber the depth snapshot
    // binding on unit 1.
    for (DecalNode* d : decals) d->flushPendingTextures();

    // Opaque-depth snapshot (same contract as soft particles: with MSAA on,
    // render3D already resolved depth into meshFBO_, so meshFBO_ is always
    // the single-sampled blit source). The current draw FBO is captured
    // BEFORE ensureSceneDepthCopy — the lazy FBO creation path leaves the
    // binding at 0, so reading it afterwards would misroute the whole pass
    // to the default framebuffer on the first frame.
    GLint prevFBO = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFBO);
    ensureSceneDepthCopy();
    if (!sceneDepthCopyFBO_) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, meshFBO_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneDepthCopyFBO_);
    glBlitFramebuffer(0, 0, meshFBOWidth_, meshFBOHeight_,
                      0, 0, meshFBOWidth_, meshFBOHeight_,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFBO));

    glUseProgram(decalProgram_);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glEnable(GL_BLEND);
    // Premultiplied color over the lit HDR; dest alpha untouched — decals
    // only appear on already-covered pixels and must not alter the coverage
    // the compositor reads.
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    // Camera-relative VP, same convention as every other pass; its inverse
    // takes clip coords straight back to camera-relative world (valid for
    // perspective AND ortho projections — no linearization shortcuts).
    Mat4 viewRot = graph_.viewMatrix_;
    viewRot.at(0, 3) = 0.0f;
    viewRot.at(1, 3) = 0.0f;
    viewRot.at(2, 3) = 0.0f;
    const Mat4 vp = bromath::mmul(graph_.projectionMatrix_, viewRot);
    const Mat4 invVP = bromath::minverse(vp);
    glUniformMatrix4fv(dcUInvViewProj_, 1, GL_FALSE, invVP.data);
    glUniform2f(dcUViewport_, static_cast<float>(meshFBOWidth_),
                              static_cast<float>(meshFBOHeight_));

    // Lighting approximation inputs: flat ambient + the first directional
    // light (the implicit sun when the scene declares no lights). Scenes lit
    // only by points/spots leave the sun term black — the decal then gets
    // ambient only (documented limitation).
    glUniform3f(dcUAmbient_, ambientColor_[0], ambientColor_[1],
                ambientColor_[2]);
    Vec3 sunDir{0.0f, -1.0f, 0.0f};
    Vec3 sunColor{0.0f, 0.0f, 0.0f};
    for (LightNode* L : lights) {
        if (L->kind() == LightNode::Kind::Directional) {
            sunDir = bromath::vnorm(L->direction());
            sunColor = L->color() * L->intensity();
            break;
        }
    }
    glUniform3f(dcUSunDir_, sunDir.x, sunDir.y, sunDir.z);
    glUniform3f(dcUSunColor_, sunColor.x, sunColor.y, sunColor.z);

    // Texture units: 0 = albedo, 1 = scene depth snapshot, 2 = emission.
    glUniform1i(dcUAlbedoTex_, 0);
    glUniform1i(dcUSceneDepth_, 1);
    glUniform1i(dcUEmissionTex_, 2);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneDepthCopyTex_);

    glBindVertexArray(decalVAO_);

    for (DecalNode* d : decals) {
        // Camera-relative model (same precision discipline as the mesh
        // pass); its inverse takes reconstructed camera-relative positions
        // straight into decal-local space.
        Mat4 model = d->worldMatrix();
        model.at(0, 3) -= graph_.cameraEye_.x;
        model.at(1, 3) -= graph_.cameraEye_.y;
        model.at(2, 3) -= graph_.cameraEye_.z;
        const Mat4 mvp = bromath::mmul(vp, model);
        const Mat4 invModel = bromath::minverse(model);
        glUniformMatrix4fv(dcUMVP_, 1, GL_FALSE, mvp.data);
        glUniformMatrix4fv(dcUInvModel_, 1, GL_FALSE, invModel.data);

        // Decal +Y axis in world space (projection runs along -Y).
        const Mat4& w = d->worldMatrix();
        Vec3 up = bromath::vnorm(Vec3{w.at(0, 1), w.at(1, 1), w.at(2, 1)});
        glUniform3f(dcUDecalUp_, up.x, up.y, up.z);

        glUniform4fv(dcUModulate_, 1, d->modulate());
        glUniform1f(dcUEmissionStrength_, d->emissionStrength());
        glUniform1f(dcUUpperFade_, d->upperFade());
        glUniform1f(dcULowerFade_, d->lowerFade());
        glUniform1f(dcUNormalFade_, d->normalFade());

        const GLuint albedo = d->albedoTextureId();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, albedo ? albedo : fallback2D_);
        glUniform1i(dcUHasAlbedo_, albedo ? 1 : 0);

        const GLuint emission = d->emissionTextureId();
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, emission ? emission : fallback2D_);
        glUniform1i(dcUHasEmission_, emission ? 1 : 0);

        glDrawArrays(GL_TRIANGLES, 0, 36);
    }

    // Restore the state the surrounding passes expect.
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glCullFace(GL_BACK);
    glDisable(GL_CULL_FACE);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
}

}  // namespace bro::scene
