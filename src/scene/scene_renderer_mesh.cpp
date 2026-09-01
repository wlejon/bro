#include "scene/scene_renderer.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer_internal.h"
#include "scene/gl_available.h"
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
#include "mesh_instanced.vert.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

namespace {

// Upload a per-mesh uniform only when its value actually changed since the last
// draw on this program. See MeshDrawCache (scene_renderer.h) for why comparing
// against a per-program cache is sound. A location of -1 (uniform absent or
// optimised out) is skipped as before.
//
// The comparisons deliberately use ==/!= on floats: this is an "is it already
// exactly what we sent" test, not a numeric tolerance question, and the cached
// value is a bit-for-bit copy of what was uploaded. NaN sentinels never compare
// equal, which is what makes the first upload after a reset unconditional.
inline void uni1f(GLint loc, float& cached, float v) {
    if (loc >= 0 && cached != v) { glUniform1f(loc, v); cached = v; }
}
inline void uni1i(GLint loc, int& cached, int v) {
    if (loc >= 0 && cached != v) { glUniform1i(loc, v); cached = v; }
}
inline void uni3fv(GLint loc, float* cached, const float* v) {
    if (loc < 0) return;
    if (cached[0] == v[0] && cached[1] == v[1] && cached[2] == v[2]) return;
    glUniform3fv(loc, 1, v);
    cached[0] = v[0]; cached[1] = v[1]; cached[2] = v[2];
}
inline void uni4fv(GLint loc, float* cached, const float* v) {
    if (loc < 0) return;
    if (cached[0] == v[0] && cached[1] == v[1] &&
        cached[2] == v[2] && cached[3] == v[3]) return;
    glUniform4fv(loc, 1, v);
    cached[0] = v[0]; cached[1] = v[1]; cached[2] = v[2]; cached[3] = v[3];
}

}  // namespace

// Query every uniform location the mesh draw path uses. Shared by the regular
// and skinned mesh pipelines — both link mesh.frag against mesh.vert (the
// skinned one with the SKINNED define), so the uniform surface is identical.
void SceneRenderer::queryMeshUniformLocs(GLuint prog, MeshDrawLocs& d,
                                         MeshProgramLocs& l) {
    auto U = [prog](const char* name) {
        return glGetUniformLocation(prog, name);
    };
    // These locs are about to describe a (possibly different) program, whose
    // uniform state starts zeroed. Drop anything the value cache thinks it
    // knows — this is the one point where the "same program, same state"
    // assumption it relies on can change underneath it.
    d.cache = MeshDrawCache{};
    d.mvp            = U("uMVP");
    d.model          = U("uModel");
    d.normalMat      = U("uNormalMat");
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
    d.fogDensity     = U("uFogDensity");
    d.fogHeightFalloff = U("uFogHeightFalloff");
    d.fogStartDist   = U("uFogStartDist");
    d.fogCamY        = U("uFogCamY");
    resolveAtmLocs(prog, d.atm);
    d.ambient        = U("uAmbient");
    d.windDir        = U("uWindDir");
    d.windStrength   = U("uWindStrength");
    d.windTime       = U("uWindTime");
    d.windFreq       = U("uWindFreq");
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
    l.shadowTexelWorld     = U("uShadowTexelWorld");
    l.shadowDepthParams    = U("uShadowDepthParams");
    l.iblEnabled           = U("uIBLEnabled");
    l.iblIrradiance        = U("uIBLIrradiance");
    l.iblPrefilter         = U("uIBLPrefilter");
    l.iblBRDF              = U("uIBLBRDF");
    l.iblIntensity         = U("uIBLIntensity");
    l.iblRotation          = U("uIBLRotation");
    l.iblPrefilterMaxLOD   = U("uIBLPrefilterMaxLOD");

    queryProbeLocs(prog, d.probe);
}

void SceneRenderer::ensureMeshPipeline() {
    if (meshProgram_) return;
    // The other way in is reflection-probe capture, which JS can request
    // without a render pass having run. Leaving meshProgram_ at 0 makes the
    // capture re-queue itself, which is exactly the "pipeline isn't up yet"
    // case it already handles.
    if (!glFunctionsLoaded()) return;

    const std::string meshFs = withAtmosphere(kMeshFragSrc);
    meshProgram_ = linkProgram(kMeshVertSrc, meshFs.c_str(), "Mesh program");
    if (!meshProgram_) return;

    queryMeshUniformLocs(meshProgram_, meshDraw_, meshLocs_);
}

void SceneRenderer::ensureSkinnedMeshPipeline() {
    if (meshSkinnedProgram_) return;

    std::string vsSrc = withSkinnedDefine(kMeshVertSrc);
    meshSkinnedProgram_ =
        linkProgram(vsSrc.c_str(), withAtmosphere(kMeshFragSrc).c_str(),
                    "Skinned mesh program");
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

// Cache-key prefix per program variant, so one chunk pair maps to up to
// three independent color programs (static / skinned / instanced).
static const char* customTargetTag(SceneRenderer::CustomShaderTarget t) {
    switch (t) {
        case SceneRenderer::CustomShaderTarget::Skinned:   return "S\x1f";
        case SceneRenderer::CustomShaderTarget::Instanced: return "I\x1f";
        default:                                           return "M\x1f";
    }
}

bool SceneRenderer::compileCustomShader(CustomShaderTarget target,
                                        const std::string& key,
                                        const std::string& vertexChunk,
                                        const std::string& fragmentChunk,
                                        std::string& errOut) {
    if (!glCreateShader) {
        // glad not loaded — no GL context (CPU raster path). The scene
        // canvas context is unavailable there too, so this is belt-and-
        // braces rather than a reachable path.
        errOut = "custom shaders require GPU rendering (no GL context)";
        return false;
    }
    if (!ensureCustomProgram(target, key, vertexChunk, fragmentChunk, &errOut))
        return false;
    // Eagerly build the matching shadow variant so a displaced mesh's first
    // shadow frame doesn't hitch on a compile — and so the fallback warning
    // (chunk references a mesh-pass-only symbol) surfaces at set time, not
    // mid-scene. Fragment-only shaders keep the shared default shadow
    // program; instanced shadows always do (undisplaced silhouette).
    if (!vertexChunk.empty() && target != CustomShaderTarget::Instanced)
        ensureCustomShadowProgram(target == CustomShaderTarget::Skinned,
                                  vertexChunk);
    return true;
}

SceneRenderer::CustomProgramEntry* SceneRenderer::ensureCustomProgram(
        CustomShaderTarget target, const std::string& key,
        const std::string& vertexChunk, const std::string& fragmentChunk,
        std::string* errOut) {
    std::string cacheKey = customTargetTag(target) + key;
    auto it = customPrograms_.find(cacheKey);
    if (it != customPrograms_.end()) return &it->second;

    std::string vsSrc, fsSrc;
    switch (target) {
        case CustomShaderTarget::Static:
            vsSrc = withUserChunk(kMeshVertSrc, vertexChunk, "CUSTOM_VERTEX");
            fsSrc = withAtmosphere(withUserChunk(kMeshFragSrc, fragmentChunk,
                                                 "CUSTOM_FRAGMENT").c_str());
            break;
        case CustomShaderTarget::Skinned: {
            std::string skinned = withSkinnedDefine(kMeshVertSrc);
            vsSrc = withUserChunk(skinned.c_str(), vertexChunk, "CUSTOM_VERTEX");
            fsSrc = withAtmosphere(withUserChunk(kMeshFragSrc, fragmentChunk,
                                                 "CUSTOM_FRAGMENT").c_str());
            break;
        }
        case CustomShaderTarget::Instanced: {
            std::string instFrag = makeMeshInstancedFragSrc();
            vsSrc = withUserChunk(kMeshInstancedVertSrc, vertexChunk,
                                  "CUSTOM_VERTEX");
            fsSrc = withUserChunk(instFrag.c_str(), fragmentChunk,
                                  "CUSTOM_FRAGMENT");
            break;
        }
    }
    GLuint prog = linkProgramCapture(vsSrc.c_str(), fsSrc.c_str(), errOut);
    if (!prog) return nullptr;

    CustomProgramEntry& e = customPrograms_[cacheKey];
    e.prog = prog;
    if (target == CustomShaderTarget::Instanced) {
        queryInstancedUniformLocs(prog, e.instDraw, e.locs);
    } else {
        queryMeshUniformLocs(prog, e.draw, e.locs);
        if (target == CustomShaderTarget::Skinned) {
            // Bind the palette block once, same as ensureSkinnedMeshPipeline
            // — per-node palettes rebind the buffer, not the block.
            GLuint bi = glGetUniformBlockIndex(prog, "BonePalette");
            if (bi != GL_INVALID_INDEX) {
                glUniformBlockBinding(prog, bi,
                                      SkinnedMeshNode::kPaletteBinding);
            }
        }
    }
    return &e;
}

void SceneRenderer::uploadUserUniforms(
        GLuint prog, std::unordered_map<std::string, GLint>& cache,
        const CustomShaderState* st) {
    if (!st) return;
    for (const auto& u : st->uniforms) {
        GLint loc;
        auto it = cache.find(u.name);
        if (it != cache.end()) {
            loc = it->second;
        } else {
            loc = glGetUniformLocation(prog, u.name.c_str());
            cache.emplace(u.name, loc);
        }
        if (loc < 0) continue;   // not declared / optimized out — silent, like GL
        switch (u.comps) {
            case 1: glUniform1fv(loc, 1, u.v); break;
            case 2: glUniform2fv(loc, 1, u.v); break;
            case 3: glUniform3fv(loc, 1, u.v); break;
            default: glUniform4fv(loc, 1, u.v); break;
        }
    }
}

void SceneRenderer::uploadUserTextures(
        GLuint prog, std::unordered_map<std::string, GLint>& cache,
        MeshNode* mesh) {
    const auto& texes = mesh->customShaderTextures();
    if (texes.empty()) return;
    // Consume staged uploads first — the setter runs on the JS thread and
    // only records bytes; this is the GL thread. renderMeshNode flushes
    // again right after, which is a no-op once the dirty flags are cleared.
    mesh->flushPendingTextures();
    int unit = MeshNode::kUserTextureUnitBase;
    for (const auto& t : mesh->customShaderTextures()) {
        if (unit >= MeshNode::userTextureUnitLimit()) break;  // budget checked at set time
        if (!t.tex) continue;
        GLint loc;
        auto it = cache.find(t.name);
        if (it != cache.end()) {
            loc = it->second;
        } else {
            loc = glGetUniformLocation(prog, t.name.c_str());
            cache.emplace(t.name, loc);
        }
        if (loc < 0) { ++unit; continue; }  // not declared — silent, like GL
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glUniform1i(loc, unit);
        ++unit;
    }
    glActiveTexture(GL_TEXTURE0);
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
    // camera-relative. That product is the same for every mesh the camera
    // sees, so it is built once per camera rather than once per draw — same
    // operations in the same order, so the result is bit-identical.
    Mat4 mvp = bromath::mmul(viewProjRot(), model);

    glUniformMatrix4fv(L.mvp, 1, GL_FALSE, mvp.data);
    glUniformMatrix4fv(L.model, 1, GL_FALSE, model.data);

    // Normal matrix: inverse-transpose of the model's upper 3x3, so normals
    // stay perpendicular under non-uniform scale. Cached per node — see
    // MeshNode::normalMatrix3 for why the camera-relative offset applied above
    // does not invalidate it.
    if (L.normalMat >= 0)
        glUniformMatrix3fv(L.normalMat, 1, GL_FALSE, mesh->normalMatrix3(model));
    // Material uniforms, sent only when they differ from what this program was
    // last given. A scene of many meshes sharing one material re-sends every
    // one of these on every draw otherwise, and the driver charges full price
    // for each. mvp/model/normalMat above stay unconditional — they are
    // per-mesh by definition and would never hit the cache.
    MeshDrawCache& C = L.cache;
    uni4fv(L.color, C.color, mesh->color());
    uni1f(L.emissive, C.emissive, mesh->emissive());
    uni3fv(L.emissiveColor, C.emissiveColor, mesh->emissiveColor());
    uni1f(L.metallic, C.metallic, mesh->metallic());
    uni1f(L.roughness, C.roughness, mesh->roughness());
    // A custom shader forces the lit path — see MeshNode::effectiveUnlit
    // (the mesh renders lit, pre-tonemap; routing in render3D agrees).
    uni1i(L.unlit, C.unlit, mesh->effectiveUnlit() ? 1 : 0);
    uni1i(L.twoSided, C.twoSided, mesh->twoSided() ? 1 : 0);
    uni1f(L.subsurface, C.subsurface, mesh->subsurface());
    uni1f(L.alphaCutoff, C.alphaCutoff, mesh->alphaCutoff());
    uni1i(L.useVertexColor, C.useVertexColor, mesh->vertexColorTintEnabled() ? 1 : 0);
    uni1f(L.nearClip, C.nearClip, mesh->nearClipDist());
    uni1f(L.windMask, C.windMask, mesh->windMask());

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
    // The texture BIND above stays unconditional: unit 0 is also written by
    // uploadUserTextures on the custom-shader path, so a cache here could go
    // stale behind our back. The sampler-unit and has-texture uniforms are
    // ours alone and cache safely.
    uni1i(L.baseColorTex, C.baseColorTex, 0);
    uni1i(L.useTexture, C.useTexture, bindTex ? 1 : 0);

    // PBR map bindings — units 5/6/7/8 avoid collision with baseColor (0),
    // shadow atlas (1), and IBL cubemaps/BRDF LUT (2/3/4).
    bool hasNM = mesh->hasNormalTexture();
    bool hasMR = mesh->hasMetallicRoughnessTexture();
    bool hasAO = mesh->hasOcclusionTexture();
    bool hasEM = mesh->hasEmissiveTexture();
    if (hasNM) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, mesh->normalTextureId());
        uni1i(L.normalMap, C.normalMap, 5);
    }
    if (hasMR) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, mesh->metallicRoughnessTextureId());
        uni1i(L.mrMap, C.mrMap, 6);
    }
    if (hasAO) {
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, mesh->occlusionTextureId());
        uni1i(L.aoMap, C.aoMap, 7);
    }
    if (hasEM) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, mesh->emissiveTextureId());
        uni1i(L.emissiveMap, C.emissiveMap, 8);
    }
    uni1i(L.hasTangent,     C.hasTangent,     mesh->mesh().hasTangents() ? 1 : 0);
    uni1i(L.hasNormalMap,   C.hasNormalMap,   hasNM ? 1 : 0);
    uni1i(L.hasMRMap,       C.hasMRMap,       hasMR ? 1 : 0);
    uni1i(L.hasAOMap,       C.hasAOMap,       hasAO ? 1 : 0);
    uni1i(L.hasEmissiveMap, C.hasEmissiveMap, hasEM ? 1 : 0);
    uni1i(L.receivesShadow, C.receivesShadow, mesh->receivesShadow() ? 1 : 0);

    // Local reflection probe: select the probe containing this mesh's bounds
    // center (or upload "none") — one probe per draw, sampler on unit 9.
    uploadProbeForDraw(mesh, L.probe);

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
