#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
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
// Light collection + upload
// ---------------------------------------------------------------------------

void SceneRenderer::collectLights(std::vector<LightNode*>& out) const {
    out.clear();
    for (auto& [id, node] : graph_.nodes_) {
        // renderVisible: a range-gated light stops contributing, same as
        // toggling `visible` off.
        if (!node->renderVisible()) continue;
        if (node->type() != SceneNode::Type::Light) continue;
        // Include only nodes actually attached to the tree (parent chain ends
        // at root_). Detached lights created but never added shouldn't light.
        SceneNode* p = node.get();
        while (p && p->parent()) p = p->parent();
        if (p != graph_.root_.get()) continue;
        out.push_back(static_cast<LightNode*>(node.get()));
        if (out.size() >= 32) break;
    }
}

void SceneRenderer::uploadLights(const std::vector<LightNode*>& lights,
                              const MeshProgramLocs& locs) {
    const int count = std::min((int)lights.size(), 32);
    glUniform1i(locs.lightCount, count);

    // Always upload shadow uniforms (even when no lights / no shadows): the
    // shader unconditionally indexes them per-iteration. Texel + tap config
    // is global so set them once per draw regardless of light count.
    if (locs.shadowAtlasTexel >= 0) {
        float texel = (shadowAtlasSize_ > 0) ? (1.0f / (float)shadowAtlasSize_) : 0.0f;
        glUniform1f(locs.shadowAtlasTexel, texel);
    }
    if (locs.shadowPCFTaps >= 0) glUniform1i(locs.shadowPCFTaps, shadowPCFTaps_);

    // Ensure valid texture objects exist for every sampler unit this program
    // references. macOS GL 4.1 core profile rejects draws (GL_INVALID_OPERATION)
    // when a sampler uniform points at an unbound texture, or when two samplers
    // of different types resolve to the same unit.
    ensureFallbackTextures();

    // Bind the shadow atlas to a fixed texture unit (1; unit 0 is baseColor).
    // sampler2DShadow performs the depth comparison via the texture's
    // GL_TEXTURE_COMPARE_MODE state set in ensureShadowAtlas().
    if (locs.shadowAtlas >= 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, shadowAtlasTex_ ? shadowAtlasTex_ : fallbackShadow_);
        glUniform1i(locs.shadowAtlas, 1);
        glActiveTexture(GL_TEXTURE0);
    }

    // IBL bindings: irradiance cube on unit 2, prefilter cube on 3, BRDF
    // LUT on 4. The mesh shader only reads them when uIBLEnabled == 1, so
    // it's safe to leave them unbound if no environment is loaded — but we
    // still bind the cube samplers (the GL spec lets a samplerCube uniform
    // point at "no texture" but some drivers warn). Sampler unit assignments
    // must match the bindIBLTextures calls in renderMeshNode for textured
    // meshes, which re-bind unit 0 only.
    bool iblOn = (envIrradianceCube_ != 0) && (envPrefilterCube_ != 0)
              && (brdfLUT_ != 0);
    if (locs.iblEnabled >= 0) glUniform1i(locs.iblEnabled, iblOn ? 1 : 0);
    if (locs.iblIntensity >= 0)       glUniform1f(locs.iblIntensity, iblOn ? envIntensity_ : 0.0f);
    if (locs.iblRotation >= 0)        glUniform1f(locs.iblRotation, envRotation_);
    if (locs.iblPrefilterMaxLOD >= 0) glUniform1f(locs.iblPrefilterMaxLOD,
                                                  iblOn ? (float)(envPrefilterMips_ - 1) : 0.0f);

    // Bind IBL textures unconditionally — use fallbacks when IBL is off so the
    // sampler units always have a valid texture of the matching type.
    if (locs.iblIrradiance >= 0) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_CUBE_MAP, iblOn ? envIrradianceCube_ : fallbackCube_);
        glUniform1i(locs.iblIrradiance, 2);
    }
    if (locs.iblPrefilter >= 0) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_CUBE_MAP, iblOn ? envPrefilterCube_ : fallbackCube_);
        glUniform1i(locs.iblPrefilter, 3);
    }
    if (locs.iblBRDF >= 0) {
        glActiveTexture(GL_TEXTURE4);
        // The BRDF LUT is env-independent and also feeds the local-probe
        // specular path, so bind it whenever it exists — not only when the
        // full IBL trio is loaded (probe captures bake it on first need).
        glBindTexture(GL_TEXTURE_2D, brdfLUT_ ? brdfLUT_ : fallback2D_);
        glUniform1i(locs.iblBRDF, 4);
    }
    glActiveTexture(GL_TEXTURE0);

    if (locs.shadowMatrix >= 0 && shadowTileCount_ > 0) {
        glUniformMatrix4fv(locs.shadowMatrix, shadowTileCount_, GL_FALSE,
                           &shadowMatrixCamRel_[0][0]);
    }
    if (locs.shadowAtlasRect >= 0 && shadowTileCount_ > 0) {
        glUniform4fv(locs.shadowAtlasRect, shadowTileCount_, &shadowAtlasRect_[0][0]);
    }
    if (locs.shadowBias >= 0 && shadowTileCount_ > 0) {
        glUniform2fv(locs.shadowBias, shadowTileCount_, &shadowBias_[0][0]);
    }
    if (locs.shadowTexelWorld >= 0 && shadowTileCount_ > 0) {
        glUniform2fv(locs.shadowTexelWorld, shadowTileCount_, &shadowTexelWorld_[0][0]);
    }
    if (locs.shadowDepthParams >= 0 && shadowTileCount_ > 0) {
        glUniform3fv(locs.shadowDepthParams, shadowTileCount_, &shadowDepthParams_[0][0]);
    }
    if (locs.lightShadowSlot >= 0) {
        // Always send 32 slots so any light index is safe to read; -1 default.
        glUniform1iv(locs.lightShadowSlot, 32, lightShadowSlot_);
    }
    if (locs.lightShadowSlotCount >= 0) {
        glUniform1iv(locs.lightShadowSlotCount, 32, lightShadowSlotCount_);
    }
    if (locs.lightCascadeSplit >= 0) {
        glUniform4fv(locs.lightCascadeSplit, 32, &lightCascadeSplit_[0][0]);
    }

    if (count == 0) return;

    int   type[32];
    float pos[32 * 3];
    float dir[32 * 3];
    float col[32 * 3];
    float intensity[32];
    float range[32];
    float spotCos[32 * 2];

    for (int i = 0; i < count; ++i) {
        LightNode* L = lights[i];
        type[i] = static_cast<int>(L->kind());

        // Light world position (column-major translation column), then
        // made camera-relative to match vWorldPos in the fragment shader.
        const Mat4& M = L->worldMatrix();
        Vec3 rel { M.at(0, 3) - graph_.cameraEye_.x,
                   M.at(1, 3) - graph_.cameraEye_.y,
                   M.at(2, 3) - graph_.cameraEye_.z };

        Vec3 d = L->direction();
        float dlen = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
        if (dlen > 1e-6f) { d.x /= dlen; d.y /= dlen; d.z /= dlen; }

        pos[i*3+0] = rel.x; pos[i*3+1] = rel.y; pos[i*3+2] = rel.z;
        dir[i*3+0] = d.x;   dir[i*3+1] = d.y;   dir[i*3+2] = d.z;

        const Vec3& c = L->color();
        col[i*3+0] = c.x; col[i*3+1] = c.y; col[i*3+2] = c.z;

        intensity[i] = L->intensity();
        range[i]     = L->range();

        // Pre-compute spot cos-angles (shader compares cos directly).
        spotCos[i*2+0] = std::cos(L->innerAngle());
        spotCos[i*2+1] = std::cos(L->outerAngle());
    }

    if (locs.lightType >= 0)      glUniform1iv(locs.lightType, count, type);
    if (locs.lightPos >= 0)       glUniform3fv(locs.lightPos, count, pos);
    if (locs.lightDir >= 0)       glUniform3fv(locs.lightDir, count, dir);
    if (locs.lightColor >= 0)     glUniform3fv(locs.lightColor, count, col);
    if (locs.lightIntensity >= 0) glUniform1fv(locs.lightIntensity, count, intensity);
    if (locs.lightRange >= 0)     glUniform1fv(locs.lightRange, count, range);
    if (locs.lightSpotCos >= 0)   glUniform2fv(locs.lightSpotCos, count, spotCos);
}

}  // namespace bro::scene
