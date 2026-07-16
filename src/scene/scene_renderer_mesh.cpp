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

#include "mesh.vert.h"
#include "mesh.frag.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

// Query every uniform location the mesh draw path uses. Shared by the regular
// and skinned mesh pipelines — both link mesh.frag against mesh.vert (the
// skinned one with the SKINNED define), so the uniform surface is identical.
void SceneRenderer::queryMeshUniformLocs(GLuint prog, MeshDrawLocs& d,
                                         MeshProgramLocs& l) {
    auto U = [prog](const char* name) {
        return glGetUniformLocation(prog, name);
    };
    d.mvp            = U("uMVP");
    d.model          = U("uModel");
    d.color          = U("uColor");
    d.emissive       = U("uEmissive");
    d.emissiveColor  = U("uEmissiveColor");
    d.metallic       = U("uMetallic");
    d.roughness      = U("uRoughness");
    d.unlit          = U("uUnlit");
    d.twoSided       = U("uTwoSided");
    d.subsurface     = U("uSubsurface");
    d.alphaCutoff    = U("uAlphaCutoff");
    d.useVertexColor = U("uUseVertexColor");
    d.nearClip       = U("uNearClip");
    d.windMask       = U("uWindMask");
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
    d.ambient        = U("uAmbient");
    d.windDir        = U("uWindDir");
    d.windStrength   = U("uWindStrength");
    d.windTime       = U("uWindTime");
    d.windFreq       = U("uWindFreq");

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

void SceneRenderer::ensureMeshPipeline() {
    if (meshProgram_) return;

    meshProgram_ = linkProgram(kMeshVertSrc, kMeshFragSrc, "Mesh program");
    if (!meshProgram_) return;

    queryMeshUniformLocs(meshProgram_, meshDraw_, meshLocs_);
}

void SceneRenderer::ensureSkinnedMeshPipeline() {
    if (meshSkinnedProgram_) return;

    std::string vsSrc = withSkinnedDefine(kMeshVertSrc);
    meshSkinnedProgram_ =
        linkProgram(vsSrc.c_str(), kMeshFragSrc, "Skinned mesh program");
    if (!meshSkinnedProgram_) return;

    queryMeshUniformLocs(meshSkinnedProgram_, meshSkinnedDraw_, meshSkinnedLocs_);

    // Bind the palette block to SkinnedMeshNode::kPaletteBinding once —
    // per-node palettes rebind the buffer, not the block.
    GLuint bi = glGetUniformBlockIndex(meshSkinnedProgram_, "BonePalette");
    if (bi != GL_INVALID_INDEX) {
        glUniformBlockBinding(meshSkinnedProgram_, bi,
                              SkinnedMeshNode::kPaletteBinding);
    }
}

void SceneRenderer::renderMeshNode(MeshNode* mesh, const MeshDrawLocs& L) {
    // Apply any staged texture uploads/releases before reading material
    // state, so runtime texture swaps take effect this frame (the geometry
    // upload in drawRaw runs too late — after the bind decisions below).
    mesh->flushPendingTextures();

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

    glUniformMatrix4fv(L.mvp, 1, GL_FALSE, mvp.data);
    glUniformMatrix4fv(L.model, 1, GL_FALSE, model.data);
    glUniform4fv(L.color, 1, mesh->color());
    glUniform1f(L.emissive, mesh->emissive());
    glUniform3fv(L.emissiveColor, 1, mesh->emissiveColor());
    glUniform1f(L.metallic, mesh->metallic());
    glUniform1f(L.roughness, mesh->roughness());
    if (L.unlit >= 0) glUniform1i(L.unlit, mesh->unlit() ? 1 : 0);
    if (L.twoSided >= 0)   glUniform1i(L.twoSided, mesh->twoSided() ? 1 : 0);
    if (L.subsurface >= 0) glUniform1f(L.subsurface, mesh->subsurface());
    if (L.alphaCutoff >= 0) glUniform1f(L.alphaCutoff, mesh->alphaCutoff());
    glUniform1i(L.useVertexColor, mesh->vertexColorTintEnabled() ? 1 : 0);
    glUniform1f(L.nearClip, mesh->nearClipDist());
    if (L.windMask >= 0) glUniform1f(L.windMask, mesh->windMask());

    // Bind baseColor texture if present. Texture composes with the baseColor
    // factor and per-vertex tint — matches glTF "baseColorTexture *
    // baseColorFactor", with vertex color folded in for tile/terrain shading.
    // Resolution happens here, per draw: an external provider (scene-as-
    // texture) may return a different id every frame (source FBO recreated on
    // resize / renderScale change) or 0 (source destroyed / never rendered),
    // in which case the mesh falls back to its untextured base color.
    GLuint baseTex = mesh->resolvedBaseColorTextureId();
    if (baseTex && mesh->hasExternalBaseColorTexture() &&
        unlitOverlayActive_ && baseTex == tonemapColorTex_) {
        // Self-sampling guard: the post-tonemap unlit overlay draws INTO
        // tonemapFBO_, so an unlit mesh linked to its own scene's output
        // would sample the bound draw attachment — a GL feedback loop
        // (undefined behavior). Draw it untextured for this pass instead.
        // Lit self-sampling meshes are fine (they draw into meshFBO_) and
        // give the classic one-frame-delayed recursive image.
        baseTex = 0;
    }
    const bool bindTex = baseTex != 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bindTex ? baseTex : fallback2D_);
    if (bindTex && mesh->hasExternalBaseColorTexture()) {
        // External textures are scene LDR outputs with no mip chain, so the
        // owned-path default (LINEAR_MIPMAP_LINEAR, set at upload) would be
        // mipmap-incomplete here. Texture parameters live on the texture
        // object, shared with the source scene's own compositing — the values
        // below are exactly what the tonemap/post FBOs set at creation
        // (LINEAR + CLAMP_TO_EDGE), so re-asserting them never changes how
        // the source scene itself displays.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glUniform1i(L.baseColorTex, 0);
    glUniform1i(L.useTexture, bindTex ? 1 : 0);

    // PBR map bindings — units 5/6/7/8 avoid collision with baseColor (0),
    // shadow atlas (1), and IBL cubemaps/BRDF LUT (2/3/4).
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

    // Skinned nodes: flush palette/skin-VBO updates and bind the palette UBO
    // before the draw. Only reached with the skinned program bound — the
    // render walk routes skinReady() nodes here (see render3D).
    if (SkinnedMeshNode* sm = mesh->asSkinnedMesh()) {
        sm->prepareSkinnedDraw();
    }

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
