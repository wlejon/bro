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

#include "mesh.vert.h"
#include "mesh.frag.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

void SceneRenderer::ensureMeshPipeline() {
    if (meshProgram_) return;

    meshProgram_ = linkProgram(kMeshVertSrc, kMeshFragSrc, "Mesh program");

    if (meshProgram_) {
        uMVP_ = glGetUniformLocation(meshProgram_, "uMVP");
        uModel_ = glGetUniformLocation(meshProgram_, "uModel");
        uColor_ = glGetUniformLocation(meshProgram_, "uColor");
        uEmissive_ = glGetUniformLocation(meshProgram_, "uEmissive");
        uEmissiveColor_ = glGetUniformLocation(meshProgram_, "uEmissiveColor");
        uMetallic_ = glGetUniformLocation(meshProgram_, "uMetallic");
        uRoughness_ = glGetUniformLocation(meshProgram_, "uRoughness");
        uUseVertexColor_ = glGetUniformLocation(meshProgram_, "uUseVertexColor");
        uUseTexture_     = glGetUniformLocation(meshProgram_, "uUseTexture");
        uBaseColorTex_   = glGetUniformLocation(meshProgram_, "uBaseColorTex");
        uHasTangent_     = glGetUniformLocation(meshProgram_, "uHasTangent");
        uHasNormalMap_   = glGetUniformLocation(meshProgram_, "uHasNormalMap");
        uHasMRMap_       = glGetUniformLocation(meshProgram_, "uHasMRMap");
        uHasAOMap_       = glGetUniformLocation(meshProgram_, "uHasAOMap");
        uHasEmissiveMap_ = glGetUniformLocation(meshProgram_, "uHasEmissiveMap");
        uNormalMap_      = glGetUniformLocation(meshProgram_, "uNormalMap");
        uMRMap_          = glGetUniformLocation(meshProgram_, "uMRMap");
        uAOMap_          = glGetUniformLocation(meshProgram_, "uAOMap");
        uEmissiveMap_    = glGetUniformLocation(meshProgram_, "uEmissiveMap");
        uReceivesShadow_ = glGetUniformLocation(meshProgram_, "uReceivesShadow");
        uFogStart_ = glGetUniformLocation(meshProgram_, "uFogStart");
        uFogEnd_ = glGetUniformLocation(meshProgram_, "uFogEnd");
        uFogColor_ = glGetUniformLocation(meshProgram_, "uFogColor");
        uAlphaCutoff_ = glGetUniformLocation(meshProgram_, "uAlphaCutoff");
        uNearClip_ = glGetUniformLocation(meshProgram_, "uNearClip");
        uAmbient_ = glGetUniformLocation(meshProgram_, "uAmbient");
        uUnlit_   = glGetUniformLocation(meshProgram_, "uUnlit");
        uTwoSided_   = glGetUniformLocation(meshProgram_, "uTwoSided");
        uSubsurface_ = glGetUniformLocation(meshProgram_, "uSubsurface");
        uWindDir_      = glGetUniformLocation(meshProgram_, "uWindDir");
        uWindStrength_ = glGetUniformLocation(meshProgram_, "uWindStrength");
        uWindTime_     = glGetUniformLocation(meshProgram_, "uWindTime");
        uWindFreq_     = glGetUniformLocation(meshProgram_, "uWindFreq");
        uWindMask_     = glGetUniformLocation(meshProgram_, "uWindMask");
        uLightCount_ = glGetUniformLocation(meshProgram_, "uLightCount");
        uLightType_ = glGetUniformLocation(meshProgram_, "uLightType");
        uLightPos_ = glGetUniformLocation(meshProgram_, "uLightPos");
        uLightDirArr_ = glGetUniformLocation(meshProgram_, "uLightDir");
        uLightColor_ = glGetUniformLocation(meshProgram_, "uLightColor");
        uLightIntensity_ = glGetUniformLocation(meshProgram_, "uLightIntensity");
        uLightRange_ = glGetUniformLocation(meshProgram_, "uLightRange");
        uLightSpotCos_ = glGetUniformLocation(meshProgram_, "uLightSpotCos");
        uLightShadowSlot_  = glGetUniformLocation(meshProgram_, "uLightShadowSlot");
        uLightShadowSlotCount_ = glGetUniformLocation(meshProgram_, "uLightShadowSlotCount");
        uLightCascadeSplit_    = glGetUniformLocation(meshProgram_, "uLightCascadeSplit");
        uShadowAtlas_      = glGetUniformLocation(meshProgram_, "uShadowAtlas");
        uShadowMatrix_     = glGetUniformLocation(meshProgram_, "uShadowMatrix");
        uShadowAtlasRect_  = glGetUniformLocation(meshProgram_, "uShadowAtlasRect");
        uShadowBiasArr_    = glGetUniformLocation(meshProgram_, "uShadowBias");
        uShadowAtlasTexel_ = glGetUniformLocation(meshProgram_, "uShadowAtlasTexel");
        uShadowPCFTaps_    = glGetUniformLocation(meshProgram_, "uShadowPCFTaps");

        uIBLEnabled_         = glGetUniformLocation(meshProgram_, "uIBLEnabled");
        uIBLIrradiance_      = glGetUniformLocation(meshProgram_, "uIBLIrradiance");
        uIBLPrefilter_       = glGetUniformLocation(meshProgram_, "uIBLPrefilter");
        uIBLBRDF_            = glGetUniformLocation(meshProgram_, "uIBLBRDF");
        uIBLIntensity_       = glGetUniformLocation(meshProgram_, "uIBLIntensity");
        uIBLRotation_        = glGetUniformLocation(meshProgram_, "uIBLRotation");
        uIBLPrefilterMaxLOD_ = glGetUniformLocation(meshProgram_, "uIBLPrefilterMaxLOD");

        // Legacy — no longer declared in the shader, fine if -1.
        uLightDir_ = -1;
        uCameraPos_ = -1;

        // Mirror into the shared MeshProgramLocs struct so uploadLights can
        // target either program from a single code path.
        meshLocs_.lightCount           = uLightCount_;
        meshLocs_.lightType            = uLightType_;
        meshLocs_.lightPos             = uLightPos_;
        meshLocs_.lightDir             = uLightDirArr_;
        meshLocs_.lightColor           = uLightColor_;
        meshLocs_.lightIntensity       = uLightIntensity_;
        meshLocs_.lightRange           = uLightRange_;
        meshLocs_.lightSpotCos         = uLightSpotCos_;
        meshLocs_.lightShadowSlot      = uLightShadowSlot_;
        meshLocs_.lightShadowSlotCount = uLightShadowSlotCount_;
        meshLocs_.lightCascadeSplit    = uLightCascadeSplit_;
        meshLocs_.shadowAtlas          = uShadowAtlas_;
        meshLocs_.shadowMatrix         = uShadowMatrix_;
        meshLocs_.shadowAtlasRect      = uShadowAtlasRect_;
        meshLocs_.shadowBias           = uShadowBiasArr_;
        meshLocs_.shadowAtlasTexel     = uShadowAtlasTexel_;
        meshLocs_.shadowPCFTaps        = uShadowPCFTaps_;
        meshLocs_.iblEnabled           = uIBLEnabled_;
        meshLocs_.iblIrradiance        = uIBLIrradiance_;
        meshLocs_.iblPrefilter         = uIBLPrefilter_;
        meshLocs_.iblBRDF              = uIBLBRDF_;
        meshLocs_.iblIntensity         = uIBLIntensity_;
        meshLocs_.iblRotation          = uIBLRotation_;
        meshLocs_.iblPrefilterMaxLOD   = uIBLPrefilterMaxLOD_;
    }
}

void SceneRenderer::renderMeshNode(MeshNode* mesh) {
    // Camera-relative rendering: offset model position by camera to avoid
    // float precision issues at large world coordinates (planet scale).
    Mat4 model = mesh->worldMatrix();
    model.at(0, 3) -= graph_.cameraEye_.x;
    model.at(1, 3) -= graph_.cameraEye_.y;
    model.at(2, 3) -= graph_.cameraEye_.z;

    // View matrix without translation (rotation only) since model is now
    // camera-relative
    Mat4 viewRot = graph_.viewMatrix_;
    viewRot.at(0, 3) = 0.0f;
    viewRot.at(1, 3) = 0.0f;
    viewRot.at(2, 3) = 0.0f;

    Mat4 mvp = bromath::mmul(bromath::mmul(graph_.projectionMatrix_, viewRot), model);

    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, mvp.data);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, model.data);
    glUniform4fv(uColor_, 1, mesh->color());
    glUniform1f(uEmissive_, mesh->emissive());
    glUniform3fv(uEmissiveColor_, 1, mesh->emissiveColor());
    glUniform1f(uMetallic_, mesh->metallic());
    glUniform1f(uRoughness_, mesh->roughness());
    if (uUnlit_ >= 0) glUniform1i(uUnlit_, mesh->unlit() ? 1 : 0);
    if (uTwoSided_ >= 0)   glUniform1i(uTwoSided_, mesh->twoSided() ? 1 : 0);
    if (uSubsurface_ >= 0) glUniform1f(uSubsurface_, mesh->subsurface());
    if (uAlphaCutoff_ >= 0) glUniform1f(uAlphaCutoff_, mesh->alphaCutoff());
    glUniform1i(uUseVertexColor_, mesh->vertexColorTintEnabled() ? 1 : 0);
    glUniform1f(uNearClip_, mesh->nearClipDist());
    if (uWindMask_ >= 0) glUniform1f(uWindMask_, mesh->windMask());

    // Bind baseColor texture if present. Texture composes with the baseColor
    // factor and per-vertex tint — matches glTF "baseColorTexture *
    // baseColorFactor", with vertex color folded in for tile/terrain shading.
    bool bindTex = mesh->hasBaseColorTexture();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bindTex ? mesh->baseColorTextureId() : fallback2D_);
    glUniform1i(uBaseColorTex_, 0);
    glUniform1i(uUseTexture_, bindTex ? 1 : 0);

    // PBR map bindings — units 5/6/7/8 avoid collision with baseColor (0),
    // shadow atlas (1), and IBL cubemaps/BRDF LUT (2/3/4).
    bool hasNM = mesh->hasNormalTexture();
    bool hasMR = mesh->hasMetallicRoughnessTexture();
    bool hasAO = mesh->hasOcclusionTexture();
    bool hasEM = mesh->hasEmissiveTexture();
    if (hasNM) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, mesh->normalTextureId());
        if (uNormalMap_ >= 0) glUniform1i(uNormalMap_, 5);
    }
    if (hasMR) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, mesh->metallicRoughnessTextureId());
        if (uMRMap_ >= 0) glUniform1i(uMRMap_, 6);
    }
    if (hasAO) {
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, mesh->occlusionTextureId());
        if (uAOMap_ >= 0) glUniform1i(uAOMap_, 7);
    }
    if (hasEM) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, mesh->emissiveTextureId());
        if (uEmissiveMap_ >= 0) glUniform1i(uEmissiveMap_, 8);
    }
    if (uHasTangent_     >= 0) glUniform1i(uHasTangent_,     mesh->mesh().hasTangents() ? 1 : 0);
    if (uHasNormalMap_   >= 0) glUniform1i(uHasNormalMap_,   hasNM ? 1 : 0);
    if (uHasMRMap_       >= 0) glUniform1i(uHasMRMap_,       hasMR ? 1 : 0);
    if (uHasAOMap_       >= 0) glUniform1i(uHasAOMap_,       hasAO ? 1 : 0);
    if (uHasEmissiveMap_ >= 0) glUniform1i(uHasEmissiveMap_, hasEM ? 1 : 0);
    if (uReceivesShadow_ >= 0) glUniform1i(uReceivesShadow_, mesh->receivesShadow() ? 1 : 0);

    // Per-mesh polygon offset (depth bias). Used by callers that need to
    // layer co-located meshes — e.g. terrain LOD shells that overlap and need
    // the high-detail mesh to consistently win the depth test.
    float pf = mesh->depthBiasFactor();
    float pu = mesh->depthBiasUnits();
    if (pf != 0.0f || pu != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pf, pu);
    }

    bool ts = mesh->twoSided();
    if (ts) glDisable(GL_CULL_FACE);

    // Alpha blending for translucent meshes (uniform color alpha < 1).
    // Depth writes are disabled so multiple translucent surfaces don't
    // occlude each other; opaque meshes still occlude translucent ones via
    // the unchanged depth test. Separate alpha function ensures the final
    // FBO stays composable over the 2D backdrop (standard "over" operator).
    bool translucent = mesh->color()[3] < 1.0f;
    if (translucent) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE,       GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
    }

    mesh->onRender(graph_);

    if (translucent) {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

    if (ts) glEnable(GL_CULL_FACE);

    if (pf != 0.0f || pu != 0.0f) {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 0.0f);
    }
}

}  // namespace bro::scene
