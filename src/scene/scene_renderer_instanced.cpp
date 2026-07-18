#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "canvas/canvas_scene.h"
#include "util/log.h"

#include "broimage/decode.h"

#include <algorithm>
#include <cassert>
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
// runtime so the two shaders cannot drift apart accidentally. Declared in
// scene_renderer_internal.h — the custom-shader path (ensureCustomProgram,
// scene_renderer_mesh.cpp) splices user fragment chunks into this source.
std::string makeMeshInstancedFragSrc() {
    std::string s = kMeshFragSrc;
    // A miss on any anchor below means mesh.frag was edited without updating
    // this derivation — the injection would silently be skipped and instanced
    // tint/atlas would silently break, so make it loud (and fatal in Debug).
    auto anchorMissing = [](const std::string& anchor) {
        LOG_ERROR("makeMeshInstancedFragSrc: anchor \"%s\" not found in "
                  "mesh.frag — instanced injection skipped (mesh.frag edited "
                  "without updating this derivation?)", anchor.c_str());
        assert(!"makeMeshInstancedFragSrc: anchor missing in mesh.frag");
    };
    // Add the instance-only varying + uniform alongside the existing
    // varyings. uAlphaCutoff is already declared (and applied) by the base
    // kMeshFragSrc, so re-declaring it here would be a GLSL redeclaration
    // error — inject only what's unique to the instanced path.
    const std::string anchor1 = "in vec3 vBitangentW;";
    auto p = s.find(anchor1);
    if (p != std::string::npos) {
        s.insert(p + anchor1.size(),
                 "\nin vec4 vInstColor;\nuniform vec2 uAtlasGrid;");
    } else {
        anchorMissing(anchor1);
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
    } else {
        anchorMissing(anchor2);
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
    } else {
        anchorMissing(anchor3);
    }
    return s;
}

void SceneRenderer::queryInstancedUniformLocs(GLuint prog, InstancedDrawLocs& d,
                                              MeshProgramLocs& l) {
    auto U = [prog](const char* name) {
        return glGetUniformLocation(prog, name);
    };
    d.vp             = U("uVP");
    d.cameraEye      = U("uCameraEye");
    d.instModel      = U("uInstModel");
    d.color          = U("uColor");
    d.emissive       = U("uEmissive");
    d.emissiveColor  = U("uEmissiveColor");
    d.metallic       = U("uMetallic");
    d.roughness      = U("uRoughness");
    d.unlit          = U("uUnlit");
    d.useVertexColor = U("uUseVertexColor");
    d.nearClip       = U("uNearClip");
    d.useTexture     = U("uUseTexture");
    d.baseColorTex   = U("uBaseColorTex");
    d.normalMap      = U("uNormalMap");
    d.mrMap          = U("uMRMap");
    d.aoMap          = U("uAOMap");
    d.emissiveMap    = U("uEmissiveMap");
    d.hasTangent     = U("uHasTangent");
    d.hasNormalMap   = U("uHasNormalMap");
    d.hasMRMap       = U("uHasMRMap");
    d.hasAOMap       = U("uHasAOMap");
    d.hasEmissiveMap = U("uHasEmissiveMap");
    d.receivesShadow = U("uReceivesShadow");
    d.fogStart       = U("uFogStart");
    d.fogEnd         = U("uFogEnd");
    d.fogColor       = U("uFogColor");
    d.fogDensity     = U("uFogDensity");
    d.fogHeightFalloff = U("uFogHeightFalloff");
    d.fogStartDist   = U("uFogStartDist");
    d.fogCamY        = U("uFogCamY");
    d.ambient        = U("uAmbient");
    d.atlasGrid      = U("uAtlasGrid");
    d.alphaCutoff    = U("uAlphaCutoff");
    d.ssrMask        = U("uSSRMask");

    l.lightCount           = U("uLightCount");
    l.lightType            = U("uLightType");
    l.lightPos             = U("uLightPos");
    l.lightDir             = U("uLightDir");
    l.lightColor           = U("uLightColor");
    l.lightIntensity       = U("uLightIntensity");
    l.lightRange           = U("uLightRange");
    l.lightSpotCos         = U("uLightSpotCos");
    l.lightShadowSlot      = U("uLightShadowSlot");
    l.lightShadowSlotCount = U("uLightShadowSlotCount");
    l.lightCascadeSplit    = U("uLightCascadeSplit");
    l.shadowAtlas          = U("uShadowAtlas");
    l.shadowMatrix         = U("uShadowMatrix");
    l.shadowAtlasRect      = U("uShadowAtlasRect");
    l.shadowBias           = U("uShadowBias");
    l.shadowAtlasTexel     = U("uShadowAtlasTexel");
    l.shadowPCFTaps        = U("uShadowPCFTaps");
    l.iblEnabled           = U("uIBLEnabled");
    l.iblIrradiance        = U("uIBLIrradiance");
    l.iblPrefilter         = U("uIBLPrefilter");
    l.iblBRDF              = U("uIBLBRDF");
    l.iblIntensity         = U("uIBLIntensity");
    l.iblRotation          = U("uIBLRotation");
    l.iblPrefilterMaxLOD   = U("uIBLPrefilterMaxLOD");
}

void SceneRenderer::ensureInstancedMeshPipeline() {
    if (meshInstancedProgram_) return;

    std::string fragSrc = makeMeshInstancedFragSrc();
    meshInstancedProgram_ = linkProgram(kMeshInstancedVertSrc, fragSrc.c_str(), "Instanced mesh program");

    if (!meshInstancedProgram_) return;

    queryInstancedUniformLocs(meshInstancedProgram_, meshInstDraw_, meshInstLocs_);
}

void SceneRenderer::renderInstancedMeshNode(InstancedMeshNode* mesh,
                                            const InstancedDrawLocs& L) {
    glUniformMatrix4fv(L.instModel, 1, GL_FALSE, mesh->worldMatrix().data);
    glUniform4fv(L.color, 1, mesh->color());
    glUniform1f(L.emissive, mesh->emissive());
    glUniform3fv(L.emissiveColor, 1, mesh->emissiveColor());
    glUniform1f(L.metallic, mesh->metallic());
    glUniform1f(L.roughness, mesh->roughness());
    // Same rule as renderMeshNode: a custom shader forces the lit path
    // (see InstancedMeshNode::effectiveUnlit).
    if (L.unlit >= 0) glUniform1i(L.unlit, mesh->effectiveUnlit() ? 1 : 0);
    glUniform1i(L.useVertexColor, mesh->vertexColorTintEnabled() ? 1 : 0);
    glUniform1f(L.nearClip, mesh->nearClipDist());

    bool bindTex = mesh->hasBaseColorTexture();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bindTex ? mesh->baseColorTextureId() : fallback2D_);
    glUniform1i(L.baseColorTex, 0);
    glUniform1i(L.useTexture, bindTex ? 1 : 0);

    bool hasNM = mesh->hasNormalTexture();
    bool hasMR = mesh->hasMetallicRoughnessTexture();
    bool hasAO = mesh->hasOcclusionTexture();
    bool hasEM = mesh->hasEmissiveTexture();
    if (hasNM) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, mesh->normalTextureId());
        if (L.normalMap >= 0) glUniform1i(L.normalMap, 5);
    }
    if (hasMR) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, mesh->metallicRoughnessTextureId());
        if (L.mrMap >= 0) glUniform1i(L.mrMap, 6);
    }
    if (hasAO) {
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, mesh->occlusionTextureId());
        if (L.aoMap >= 0) glUniform1i(L.aoMap, 7);
    }
    if (hasEM) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, mesh->emissiveTextureId());
        if (L.emissiveMap >= 0) glUniform1i(L.emissiveMap, 8);
    }
    if (L.hasTangent     >= 0) glUniform1i(L.hasTangent,     mesh->mesh().hasTangents() ? 1 : 0);
    if (L.hasNormalMap   >= 0) glUniform1i(L.hasNormalMap,   hasNM ? 1 : 0);
    if (L.hasMRMap       >= 0) glUniform1i(L.hasMRMap,       hasMR ? 1 : 0);
    if (L.hasAOMap       >= 0) glUniform1i(L.hasAOMap,       hasAO ? 1 : 0);
    if (L.hasEmissiveMap >= 0) glUniform1i(L.hasEmissiveMap, hasEM ? 1 : 0);
    if (L.receivesShadow >= 0) glUniform1i(L.receivesShadow, mesh->receivesShadow() ? 1 : 0);
    if (L.atlasGrid      >= 0) glUniform2f(L.atlasGrid, (float)mesh->atlasCols(), (float)mesh->atlasRows());
    if (L.alphaCutoff    >= 0) glUniform1f(L.alphaCutoff, mesh->alphaCutoff());

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
