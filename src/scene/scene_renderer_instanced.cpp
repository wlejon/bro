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

#include "mesh.frag.h"
#include "mesh_instanced.vert.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

// Build the instanced fragment shader by mutating the regular kMeshFragSrc:
// add `in vec4 vInstColor;`, an optional atlas-grid UV remap on the base
// color sample, and multiply baseColor by the instance RGB tint. Done at
// runtime so the two shaders cannot drift apart accidentally.
static std::string makeMeshInstancedFragSrc() {
    std::string s = kMeshFragSrc;
    // Add the instance-only varying + uniform alongside the existing
    // varyings. uAlphaCutoff is already declared (and applied) by the base
    // kMeshFragSrc, so re-declaring it here would be a GLSL redeclaration
    // error — inject only what's unique to the instanced path.
    const std::string anchor1 = "in vec3 vBitangentW;";
    auto p = s.find(anchor1);
    if (p != std::string::npos) {
        s.insert(p + anchor1.size(),
                 "\nin vec4 vInstColor;\nuniform vec2 uAtlasGrid;");
    }
    // Replace the baseColor texture sample so it can pick a sub-rect of the
    // texture when uAtlasGrid > 1. Only the baseColor sampler uses atlas UV;
    // normal/MR/AO/emissive textures keep the raw vUV (leaf cards usually
    // have a baseColor only). The cell index is read from vInstColor.a as
    // packed by setInstancesFromPosQuatScale: cell = int(a * 256).
    const std::string anchor2 = "vec4 tex = texture(uBaseColorTex, vUV);";
    p = s.find(anchor2);
    if (p != std::string::npos) {
        s.replace(p, anchor2.size(),
                  "vec2 uvForBase = vUV;\n"
                  "        if (uAtlasGrid.x > 1.0 || uAtlasGrid.y > 1.0) {\n"
                  "            int cell = int(vInstColor.a * 256.0);\n"
                  "            int cols = int(uAtlasGrid.x); if (cols < 1) cols = 1;\n"
                  "            int rows = int(uAtlasGrid.y); if (rows < 1) rows = 1;\n"
                  "            int total = cols * rows;\n"
                  "            if (cell < 0) cell = 0;\n"
                  "            if (cell >= total) cell = total - 1;\n"
                  "            int cx = cell - (cell / cols) * cols;\n"
                  "            int cy = cell / cols;\n"
                  "            vec2 cellSize = vec2(1.0 / float(cols), 1.0 / float(rows));\n"
                  "            uvForBase = (vec2(float(cx), float(cy)) + fract(vUV)) * cellSize;\n"
                  "        }\n"
                  "        vec4 tex = texture(uBaseColorTex, uvForBase);");
    }
    // Multiply the resolved baseColor by the instance RGB tint right after
    // the base-color/alpha resolution block. Alpha is reserved for the atlas
    // index — never multiplied into baseAlpha. The alpha-cutoff discard is
    // inherited from the base shader, so it is not re-injected here.
    const std::string anchor3 = "        baseAlpha = uColor.a;\n    }\n";
    p = s.find(anchor3);
    if (p != std::string::npos) {
        s.insert(p + anchor3.size(),
                 "    baseColor *= vInstColor.rgb;\n");
    }
    return s;
}

void SceneRenderer::ensureInstancedMeshPipeline() {
    if (meshInstancedProgram_) return;

    std::string fragSrc = makeMeshInstancedFragSrc();
    meshInstancedProgram_ = linkProgram(kMeshInstancedVertSrc, fragSrc.c_str(), "Instanced mesh program");

    if (!meshInstancedProgram_) return;

    auto getU = [&](const char* n) { return glGetUniformLocation(meshInstancedProgram_, n); };
    uInstVP_              = getU("uVP");
    uInstCameraEye_       = getU("uCameraEye");
    uInstModel_           = getU("uInstModel");
    uInstColor_           = getU("uColor");
    uInstEmissive_        = getU("uEmissive");
    uInstEmissiveColor_   = getU("uEmissiveColor");
    uInstMetallic_        = getU("uMetallic");
    uInstRoughness_       = getU("uRoughness");
    uInstUseVertexColor_  = getU("uUseVertexColor");
    uInstUseTexture_      = getU("uUseTexture");
    uInstBaseColorTex_    = getU("uBaseColorTex");
    uInstHasTangent_      = getU("uHasTangent");
    uInstHasNormalMap_    = getU("uHasNormalMap");
    uInstHasMRMap_        = getU("uHasMRMap");
    uInstHasAOMap_        = getU("uHasAOMap");
    uInstHasEmissiveMap_  = getU("uHasEmissiveMap");
    uInstNormalMap_       = getU("uNormalMap");
    uInstMRMap_           = getU("uMRMap");
    uInstAOMap_           = getU("uAOMap");
    uInstEmissiveMap_     = getU("uEmissiveMap");
    uInstReceivesShadow_  = getU("uReceivesShadow");
    uInstFogStart_        = getU("uFogStart");
    uInstFogEnd_          = getU("uFogEnd");
    uInstFogColor_        = getU("uFogColor");
    uInstNearClip_        = getU("uNearClip");
    uInstAmbient_         = getU("uAmbient");
    uInstUnlit_           = getU("uUnlit");
    uInstAtlasGrid_       = getU("uAtlasGrid");
    uInstAlphaCutoff_     = getU("uAlphaCutoff");

    meshInstLocs_.lightCount           = getU("uLightCount");
    meshInstLocs_.lightType            = getU("uLightType");
    meshInstLocs_.lightPos             = getU("uLightPos");
    meshInstLocs_.lightDir             = getU("uLightDir");
    meshInstLocs_.lightColor           = getU("uLightColor");
    meshInstLocs_.lightIntensity       = getU("uLightIntensity");
    meshInstLocs_.lightRange           = getU("uLightRange");
    meshInstLocs_.lightSpotCos         = getU("uLightSpotCos");
    meshInstLocs_.lightShadowSlot      = getU("uLightShadowSlot");
    meshInstLocs_.lightShadowSlotCount = getU("uLightShadowSlotCount");
    meshInstLocs_.lightCascadeSplit    = getU("uLightCascadeSplit");
    meshInstLocs_.shadowAtlas          = getU("uShadowAtlas");
    meshInstLocs_.shadowMatrix         = getU("uShadowMatrix");
    meshInstLocs_.shadowAtlasRect      = getU("uShadowAtlasRect");
    meshInstLocs_.shadowBias           = getU("uShadowBias");
    meshInstLocs_.shadowAtlasTexel     = getU("uShadowAtlasTexel");
    meshInstLocs_.shadowPCFTaps        = getU("uShadowPCFTaps");
    meshInstLocs_.iblEnabled           = getU("uIBLEnabled");
    meshInstLocs_.iblIrradiance        = getU("uIBLIrradiance");
    meshInstLocs_.iblPrefilter         = getU("uIBLPrefilter");
    meshInstLocs_.iblBRDF              = getU("uIBLBRDF");
    meshInstLocs_.iblIntensity         = getU("uIBLIntensity");
    meshInstLocs_.iblRotation          = getU("uIBLRotation");
    meshInstLocs_.iblPrefilterMaxLOD   = getU("uIBLPrefilterMaxLOD");
}

void SceneRenderer::renderInstancedMeshNode(InstancedMeshNode* mesh) {
    glUniformMatrix4fv(uInstModel_, 1, GL_FALSE, mesh->worldMatrix().data);
    glUniform4fv(uInstColor_, 1, mesh->color());
    glUniform1f(uInstEmissive_, mesh->emissive());
    glUniform3fv(uInstEmissiveColor_, 1, mesh->emissiveColor());
    glUniform1f(uInstMetallic_, mesh->metallic());
    glUniform1f(uInstRoughness_, mesh->roughness());
    if (uInstUnlit_ >= 0) glUniform1i(uInstUnlit_, mesh->unlit() ? 1 : 0);
    glUniform1i(uInstUseVertexColor_, mesh->vertexColorTintEnabled() ? 1 : 0);
    glUniform1f(uInstNearClip_, mesh->nearClipDist());

    bool bindTex = mesh->hasBaseColorTexture();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bindTex ? mesh->baseColorTextureId() : fallback2D_);
    glUniform1i(uInstBaseColorTex_, 0);
    glUniform1i(uInstUseTexture_, bindTex ? 1 : 0);

    bool hasNM = mesh->hasNormalTexture();
    bool hasMR = mesh->hasMetallicRoughnessTexture();
    bool hasAO = mesh->hasOcclusionTexture();
    bool hasEM = mesh->hasEmissiveTexture();
    if (hasNM) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, mesh->normalTextureId());
        if (uInstNormalMap_ >= 0) glUniform1i(uInstNormalMap_, 5);
    }
    if (hasMR) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, mesh->metallicRoughnessTextureId());
        if (uInstMRMap_ >= 0) glUniform1i(uInstMRMap_, 6);
    }
    if (hasAO) {
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, mesh->occlusionTextureId());
        if (uInstAOMap_ >= 0) glUniform1i(uInstAOMap_, 7);
    }
    if (hasEM) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, mesh->emissiveTextureId());
        if (uInstEmissiveMap_ >= 0) glUniform1i(uInstEmissiveMap_, 8);
    }
    if (uInstHasTangent_     >= 0) glUniform1i(uInstHasTangent_,     mesh->mesh().hasTangents() ? 1 : 0);
    if (uInstHasNormalMap_   >= 0) glUniform1i(uInstHasNormalMap_,   hasNM ? 1 : 0);
    if (uInstHasMRMap_       >= 0) glUniform1i(uInstHasMRMap_,       hasMR ? 1 : 0);
    if (uInstHasAOMap_       >= 0) glUniform1i(uInstHasAOMap_,       hasAO ? 1 : 0);
    if (uInstHasEmissiveMap_ >= 0) glUniform1i(uInstHasEmissiveMap_, hasEM ? 1 : 0);
    if (uInstReceivesShadow_ >= 0) glUniform1i(uInstReceivesShadow_, mesh->receivesShadow() ? 1 : 0);
    if (uInstAtlasGrid_      >= 0) glUniform2f(uInstAtlasGrid_, (float)mesh->atlasCols(), (float)mesh->atlasRows());
    if (uInstAlphaCutoff_    >= 0) glUniform1f(uInstAlphaCutoff_, mesh->alphaCutoff());

    bool ds = mesh->doubleSided();
    if (ds) glDisable(GL_CULL_FACE);

    float pf = mesh->depthBiasFactor();
    float pu = mesh->depthBiasUnits();
    if (pf != 0.0f || pu != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pf, pu);
    }

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

    if (pf != 0.0f || pu != 0.0f) {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 0.0f);
    }

    if (ds) glEnable(GL_CULL_FACE);
}

}  // namespace bro::scene
