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

#include "shadow.vert.h"
#include "shadow.frag.h"
#include "shadow_instanced.vert.h"

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

// ---------------------------------------------------------------------------
// Shadow pipeline
// ---------------------------------------------------------------------------

void SceneRenderer::ensureShadowPipeline() {
    if (shadowProgram_) return;
    shadowProgram_ = linkProgram(kShadowVertSrc, kShadowFragSrc, "Shadow program");
    if (shadowProgram_) {
        shadowUMVP_ = glGetUniformLocation(shadowProgram_, "uMVP");
    }
}

void SceneRenderer::ensureShadowInstancedPipeline() {
    if (shadowInstancedProgram_) return;
    shadowInstancedProgram_ = linkProgram(kShadowInstancedVertSrc, kShadowFragSrc, "Instanced shadow program");
    if (shadowInstancedProgram_) {
        shadowInstULightVP_ = glGetUniformLocation(shadowInstancedProgram_, "uLightVP");
        shadowInstUModel_   = glGetUniformLocation(shadowInstancedProgram_, "uModel");
    }
}

void SceneRenderer::ensureShadowSkinnedPipeline() {
    if (shadowSkinnedProgram_) return;
    std::string vsSrc = withSkinnedDefine(kShadowVertSrc);
    shadowSkinnedProgram_ =
        linkProgram(vsSrc.c_str(), kShadowFragSrc, "Skinned shadow program");
    if (shadowSkinnedProgram_) {
        shadowSkinnedUMVP_ = glGetUniformLocation(shadowSkinnedProgram_, "uMVP");
        GLuint bi = glGetUniformBlockIndex(shadowSkinnedProgram_, "BonePalette");
        if (bi != GL_INVALID_INDEX) {
            glUniformBlockBinding(shadowSkinnedProgram_, bi,
                                  SkinnedMeshNode::kPaletteBinding);
        }
    }
}

SceneRenderer::CustomShadowEntry* SceneRenderer::ensureCustomShadowProgram(
        bool skinned, const std::string& vertexChunk) {
    std::string cacheKey = (skinned ? "S\x1f" : "M\x1f") + vertexChunk;
    auto it = customShadowPrograms_.find(cacheKey);
    if (it != customShadowPrograms_.end())
        return it->second.prog ? &it->second : nullptr;

    std::string vsSrc = skinned ? withSkinnedDefine(kShadowVertSrc)
                                : std::string(kShadowVertSrc);
    vsSrc = withUserChunk(vsSrc.c_str(), vertexChunk, "CUSTOM_VERTEX");
    std::string err;
    GLuint prog = linkProgramCapture(vsSrc.c_str(), kShadowFragSrc, &err);

    // Cache failures too (prog stays 0): the chunk may reference symbols
    // that exist only in the mesh pass (e.g. a custom varying it writes) —
    // the caster then keeps the default shadow program (undisplaced
    // silhouette) instead of retrying the compile every frame.
    CustomShadowEntry& e = customShadowPrograms_[cacheKey];
    e.prog = prog;
    if (!prog) {
        LOG_WARN("Custom vertex chunk failed to compile against the "
                 "shadow shader — the mesh casts its undisplaced "
                 "silhouette: %s", err.c_str());
        return nullptr;
    }
    e.mvp          = glGetUniformLocation(prog, "uMVP");
    e.model        = glGetUniformLocation(prog, "uModel");
    e.windDir      = glGetUniformLocation(prog, "uWindDir");
    e.windStrength = glGetUniformLocation(prog, "uWindStrength");
    e.windTime     = glGetUniformLocation(prog, "uWindTime");
    e.windFreq     = glGetUniformLocation(prog, "uWindFreq");
    e.windMask     = glGetUniformLocation(prog, "uWindMask");
    if (skinned) {
        GLuint bi = glGetUniformBlockIndex(prog, "BonePalette");
        if (bi != GL_INVALID_INDEX) {
            glUniformBlockBinding(prog, bi, SkinnedMeshNode::kPaletteBinding);
        }
    }
    return &e;
}

void SceneRenderer::ensureShadowAtlas() {
    if (shadowAtlasTex_ && shadowAtlasAllocated_ == shadowAtlasSize_ && !shadowAtlasDirty_) return;
    destroyShadowAtlas();
    shadowAtlasAllocated_ = shadowAtlasSize_;
    shadowAtlasDirty_ = false;
    // Fresh texture = garbage texels; nothing cached survives, and the pass
    // must start from a full clear before any per-tile reuse.
    invalidateShadowCache();
    shadowAtlasNeedsClear_ = true;

    glGenTextures(1, &shadowAtlasTex_);
    glBindTexture(GL_TEXTURE_2D, shadowAtlasTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                 shadowAtlasSize_, shadowAtlasSize_, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Hardware PCF: sampler2DShadow returns a [0,1] comparison result and
    // bilinearly filters between neighbouring texels — much cheaper than
    // four manual texture() lookups, and visually identical for 2x2 PCF.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    // Atlas-edge sampling reads "infinitely far" depth, i.e. lit. Combined
    // with the in-tile clamp in sampleShadow() this avoids cross-tile bleed.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);

    glGenFramebuffers(1, &shadowAtlasFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowAtlasFBO_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, shadowAtlasTex_, 0);
    // No color buffer — depth-only.
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Shadow atlas FBO incomplete: 0x%x", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SceneRenderer::destroyShadowAtlas() {
    if (shadowAtlasFBO_) { glDeleteFramebuffers(1, &shadowAtlasFBO_); shadowAtlasFBO_ = 0; }
    if (shadowAtlasTex_) { glDeleteTextures(1, &shadowAtlasTex_); shadowAtlasTex_ = 0; }
    shadowAtlasAllocated_ = 0;
    invalidateShadowCache();
}

void SceneRenderer::computeShadowBounds(bromath::AABB3& casters,
                                        bromath::AABB3& receivers) const {
    casters = bromath::aempty3();
    receivers = bromath::aempty3();

    auto walk = [&](auto&& self, SceneNode* n) -> void {
        if (!n || !n->renderVisible()) return;
        if (n->type() == SceneNode::Type::Mesh) {
            auto* m = static_cast<MeshNode*>(n);
            // Same unlit rule as the caster gather (effectiveUnlit): a
            // custom shader suppresses unlit, so such meshes count here too.
            // hasDrawableMesh: LOD-chain-only nodes fit too (localBounds is
            // the chain union, so every level's silhouette is covered).
            if (!m->effectiveUnlit() && m->hasDrawableMesh()) {
                // cullMargin pads the fit the same way it pads culling
                // bounds (world units) — a custom vertex shader can displace
                // geometry along the light direction past the bind-pose
                // AABB, and the directional depth range is fit from these
                // bounds.
                const float cm = m->cullMargin();
                bromath::AABB3 wb = bromath::atransform(m->localBounds(), m->worldMatrix());
                wb.min -= Vec3{cm, cm, cm};
                wb.max += Vec3{cm, cm, cm};
                if (m->castsShadow()) casters = bromath::amerge(casters, wb);
                if (m->receivesShadow() || m->castsShadow())
                    receivers = bromath::amerge(receivers, wb);
            }
        } else if (n->type() == SceneNode::Type::InstancedMesh) {
            auto* m = static_cast<InstancedMeshNode*>(n);
            float wlo[3], whi[3];
            if (!m->effectiveUnlit() && m->computeWorldInstanceBounds(wlo, whi)) {
                const float cm = m->cullMargin();
                bromath::AABB3 wb{{wlo[0] - cm, wlo[1] - cm, wlo[2] - cm},
                                  {whi[0] + cm, whi[1] + cm, whi[2] + cm}};
                if (m->castsShadow()) casters = bromath::amerge(casters, wb);
                if (m->receivesShadow() || m->castsShadow())
                    receivers = bromath::amerge(receivers, wb);
            }
        }
        for (auto* c : n->children()) self(self, c);
    };
    walk(walk, graph_.root_.get());
}

namespace {

// A half-space: inside where dot(n, p) + d >= 0.
struct HalfSpace { Vec3 n; float d; };

// Clip the segment a->b against a set of half-spaces (Cyrus–Beck). Returns
// false when nothing of it survives; otherwise a/b are moved to the clipped
// endpoints.
bool clipSegment(Vec3& a, Vec3& b, const HalfSpace* hs, int count) {
    float t0 = 0.0f, t1 = 1.0f;
    const Vec3 ab = b - a;
    for (int i = 0; i < count; ++i) {
        const float da = bromath::vdot(hs[i].n, a) + hs[i].d;
        const float db = bromath::vdot(hs[i].n, b) + hs[i].d;
        if (da < 0.0f && db < 0.0f) return false;
        if (da >= 0.0f && db >= 0.0f) continue;
        const float t = da / (da - db);   // crossing parameter
        if (da < 0.0f) t0 = std::max(t0, t); else t1 = std::min(t1, t);
        if (t0 > t1) return false;
    }
    const Vec3 na = a + ab * t0;
    const Vec3 nb = a + ab * t1;
    a = na; b = nb;
    return true;
}

bool insideAll(const Vec3& p, const HalfSpace* hs, int count) {
    for (int i = 0; i < count; ++i)
        if (bromath::vdot(hs[i].n, p) + hs[i].d < 0.0f) return false;
    return true;
}

// Corner layout shared by the fit: index k*4 + j, k = 0 near / 1 far,
// j bit 0 = +x side, j bit 1 = +y side.
constexpr int kBoxEdges[12][2] = {
    {0, 1}, {1, 3}, {3, 2}, {2, 0},   // near ring
    {4, 5}, {5, 7}, {7, 6}, {6, 4},   // far ring
    {0, 4}, {1, 5}, {3, 7}, {2, 6},   // near -> far
};
constexpr int kBoxFaces[6][4] = {
    {0, 1, 3, 2}, {4, 5, 7, 6},       // near, far
    {0, 2, 6, 4}, {1, 3, 7, 5},       // -x, +x
    {0, 1, 5, 4}, {2, 3, 7, 6},       // -y, +y
};

// The vertex set of (convex frustum given by its 8 corners) ∩ (AABB): the
// corners of each inside the other, plus every edge of each clipped to the
// other. Every vertex of an intersection of two convex polytopes is one of
// those, so bounding this set bounds the intersection exactly. Returns the
// number of points written (0 = disjoint). `out` needs room for 64.
int clipFrustumToBox(const Vec3 corners[8], const bromath::AABB3& box, Vec3* out) {
    // Frustum half-spaces from its faces, oriented toward the centroid.
    Vec3 centroid{0, 0, 0};
    for (int i = 0; i < 8; ++i) centroid += corners[i];
    centroid *= 0.125f;
    HalfSpace fh[6]; int fhCount = 0;
    for (int f = 0; f < 6; ++f) {
        const Vec3& a = corners[kBoxFaces[f][0]];
        const Vec3& b = corners[kBoxFaces[f][1]];
        const Vec3& c = corners[kBoxFaces[f][2]];
        const Vec3& d = corners[kBoxFaces[f][3]];
        // Two triangles' normals summed: robust to one degenerate corner
        // pair (a perspective near face at a tiny near plane).
        Vec3 n = bromath::vcross(b - a, c - a) + bromath::vcross(c - a, d - a);
        const float L = bromath::vlen(n);
        if (L < 1e-12f) continue;               // degenerate face: no constraint
        n *= 1.0f / L;
        float dd = -bromath::vdot(n, a);
        if (bromath::vdot(n, centroid) + dd < 0.0f) { n = -n; dd = -dd; }
        fh[fhCount++] = {n, dd};
    }
    const HalfSpace bh[6] = {
        {{ 1, 0, 0}, -box.min.x}, {{-1, 0, 0},  box.max.x},
        {{ 0, 1, 0}, -box.min.y}, {{ 0,-1, 0},  box.max.y},
        {{ 0, 0, 1}, -box.min.z}, {{ 0, 0,-1},  box.max.z},
    };
    Vec3 bc[8];
    for (int c = 0; c < 8; ++c) {
        bc[c] = Vec3{(c & 1) ? box.max.x : box.min.x,
                     (c & 2) ? box.max.y : box.min.y,
                     (c & 4) ? box.max.z : box.min.z};
    }

    int n = 0;
    for (int i = 0; i < 8; ++i)
        if (insideAll(corners[i], bh, 6)) out[n++] = corners[i];
    for (int i = 0; i < 8; ++i)
        if (insideAll(bc[i], fh, fhCount)) out[n++] = bc[i];
    for (const auto& e : kBoxEdges) {
        Vec3 a = corners[e[0]], b = corners[e[1]];
        if (clipSegment(a, b, bh, 6)) { out[n++] = a; out[n++] = b; }
    }
    for (const auto& e : kBoxEdges) {
        Vec3 a = bc[e[0]], b = bc[e[1]];
        if (clipSegment(a, b, fh, fhCount)) { out[n++] = a; out[n++] = b; }
    }
    return n;
}

}  // namespace

void SceneRenderer::prepareShadows(const std::vector<LightNode*>& lights) {
    // Reset per-frame shadow state. Default every light to "no shadow".
    shadowTileCount_ = 0;
    shadowCasters_.clear();
    shadowSkinnedCasters_.clear();
    shadowCustomCasters_.clear();
    shadowSkinnedCustomCasters_.clear();
    shadowInstancedCasters_.clear();
    shadowTubeCasters_.clear();
    for (int i = 0; i < 32; ++i) {
        lightShadowSlot_[i] = -1;
        lightShadowSlotCount_[i] = 0;
        lightCascadeSplit_[i][0] = lightCascadeSplit_[i][1] =
        lightCascadeSplit_[i][2] = lightCascadeSplit_[i][3] = 1e30f;
    }

    // Quick skip: if no light wants shadows, don't bother fitting.
    bool anyCaster = false;
    for (auto* L : lights) {
        if (L && L->castsShadow()) { anyCaster = true; break; }
    }
    if (!anyCaster) return;

    // Gather shadow-casting meshes once. Unlit meshes never cast (a custom
    // shader suppresses unlit in the color pass, so custom-shader casters
    // route on castsShadow alone). Skinned meshes (ready skin) go in their
    // own list so the skinned depth shader deforms their silhouettes;
    // frustum fitting still uses their bind-pose bounds via
    // computeShadowCasterBounds (the directional depth range is padded by
    // the whole-scene extent, so palette motion stays covered). Casters
    // with a custom VERTEX chunk split further into the custom lists and
    // render with the spliced shadow variant — displaced silhouettes;
    // fragment-only shaders stay on the default depth program.
    auto hasVertexChunk = [](const MeshNode* m) {
        return m->hasCustomShader() && !m->customShader()->vertexChunk.empty();
    };
    auto gather = [&](auto&& self, SceneNode* n) -> void {
        if (!n || !n->renderVisible()) return;
        if (n->type() == SceneNode::Type::Mesh) {
            auto* m = static_cast<MeshNode*>(n);
            if (!m->effectiveUnlit() && m->castsShadow() && m->hasDrawableMesh()) {
                auto* sm = m->asSkinnedMesh();
                if (sm && sm->skinReady()) {
                    (hasVertexChunk(m) ? shadowSkinnedCustomCasters_
                                       : shadowSkinnedCasters_).push_back(m);
                } else {
                    (hasVertexChunk(m) ? shadowCustomCasters_
                                       : shadowCasters_).push_back(m);
                }
            }
        } else if (n->type() == SceneNode::Type::InstancedMesh) {
            auto* m = static_cast<InstancedMeshNode*>(n);
            if (m->castsShadow() && m->isTube() && m->tubeSegCount() > 0)
                shadowTubeCasters_.push_back(m);
            else if (!m->effectiveUnlit() && m->castsShadow() && !m->mesh().empty() && m->instanceCount() > 0)
                shadowInstancedCasters_.push_back(m);
        }
        for (auto* c : n->children()) self(self, c);
    };
    gather(gather, graph_.root_.get());
    // Group the custom casters by chunk source so the per-tile loop binds
    // each shadow variant once per group.
    auto byChunk = [](MeshNode* a, MeshNode* b) {
        return a->customShader()->vertexChunk < b->customShader()->vertexChunk;
    };
    std::stable_sort(shadowCustomCasters_.begin(), shadowCustomCasters_.end(), byChunk);
    std::stable_sort(shadowSkinnedCustomCasters_.begin(),
                     shadowSkinnedCustomCasters_.end(), byChunk);
    if (shadowCasters_.empty() && shadowSkinnedCasters_.empty() &&
        shadowCustomCasters_.empty() && shadowSkinnedCustomCasters_.empty() &&
        shadowInstancedCasters_.empty() && shadowTubeCasters_.empty()) return;

    // Scene bounds: casters stretch the directional depth range; receivers
    // clip the camera volume the directional fit covers.
    bromath::AABB3 casterBounds, receiverBounds;
    computeShadowBounds(casterBounds, receiverBounds);
    if (bromath::aisEmpty(casterBounds)) return;

    // Bias matrix maps NDC to UV [0,1]. XY always need the half-scale-and-
    // offset, but Z only does under the conventional [-1,1] mapping: with
    // clip control on, the shadow projections above already emit [0,1] depth,
    // so remapping z again would compress every comparison into [0.5,1] and
    // shadow everything.
    const float zs = gReversedZ ? 1.0f : 0.5f;
    const float zo = gReversedZ ? 0.0f : 0.5f;
    Mat4 bias = bromath::mmul(bromath::mtranslate({0.5f, 0.5f, zo}),
                              bromath::mscale({0.5f, 0.5f, zs}));

    // Atlas grid from the tile demand: one sun over an ortho camera (or a
    // single-cascade sun) takes the WHOLE atlas, up to four tiles take a
    // quarter each, anything more falls back to 16ths. A 512 px tile — the
    // old fixed 4x4 of a 2048 atlas — is what made every shadow a blob: at
    // 64 m across the view that is a 6 m texel. The demand is an upper
    // bound on what gets allocated (a point light that no longer fits is
    // skipped), so the grid always has room.
    int demand = 0;
    for (auto* L : lights) {
        if (!L || !L->castsShadow()) continue;
        switch (L->kind()) {
        case LightNode::Kind::Directional:
            demand += graph_.cameraIsPerspective_ ? L->cascadeCount() : 1; break;
        case LightNode::Kind::Spot:  demand += 1; break;
        case LightNode::Kind::Point: demand += 6; break;
        }
    }
    const int gridDim = demand <= 1 ? 1 : (demand <= 4 ? 2 : 4);
    if (gridDim != shadowGridDim_) {
        // Every tile moved: cached texels are at the wrong place.
        shadowGridDim_ = gridDim;
        invalidateShadowCache();
        shadowAtlasNeedsClear_ = true;
    }
    const float tileUV = 1.0f / (float)gridDim;
    const int   tilePx = shadowAtlasSize_ / gridDim;

    // texelConst / texelPerDist: world size of one shadow texel at the
    // receiver — a constant for an ortho tile, per metre of light distance
    // for a perspective one. zNear/zFar/ortho describe the tile's depth
    // mapping so the FS can express a world-unit bias in [0,1] depth.
    auto bakeTile = [&](int slot, const Mat4& lightProjView, LightNode* L,
                        float texelConst, float texelPerDist,
                        float zNear, float zFar, bool ortho) {
        // shadowMatrixCamRel = bias * proj * view * translate(cameraEye)
        // so the FS can multiply directly against vWorldPos (camera-relative).
        Mat4 t = bromath::mtranslate({graph_.cameraEye_.x, graph_.cameraEye_.y, graph_.cameraEye_.z});
        Mat4 cam = bromath::mmul(bromath::mmul(bias, lightProjView), t);
        std::memcpy(shadowMatrixCamRel_[slot], cam.data, sizeof(float) * 16);
        std::memcpy(shadowRenderMatrix_[slot], lightProjView.data, sizeof(float) * 16);

        int gx = slot % gridDim;
        int gy = slot / gridDim;
        shadowAtlasRect_[slot][0] = gx * tileUV;
        shadowAtlasRect_[slot][1] = gy * tileUV;
        shadowAtlasRect_[slot][2] = tileUV;
        shadowAtlasRect_[slot][3] = tileUV;

        shadowBias_[slot][0] = L->shadowBias();
        shadowBias_[slot][1] = L->shadowNormalBias();
        shadowTexelWorld_[slot][0] = texelConst;
        shadowTexelWorld_[slot][1] = texelPerDist;
        shadowDepthParams_[slot][0] = zNear;
        shadowDepthParams_[slot][1] = zFar;
        shadowDepthParams_[slot][2] = ortho ? 1.0f : 0.0f;

        shadowTileLight_[slot] = L;
    };

    // For each shadow-casting light, allocate slot(s) and build matrices.
    for (int i = 0; i < (int)lights.size() && i < 32; ++i) {
        LightNode* L = lights[i];
        if (!L || !L->castsShadow()) continue;
        if (shadowTileCount_ >= kMaxShadowTiles) break;

        if (L->kind() == LightNode::Kind::Directional) {
            Vec3 d = L->direction();
            float dlen = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
            if (dlen < 1e-6f) continue;
            d.x /= dlen; d.y /= dlen; d.z /= dlen;

            // Camera basis from view matrix (transposed columns: row 0 = right,
            // row 1 = up, row 2 = -forward). lookAt produces the same.
            Vec3 sBasis{graph_.viewMatrix_.at(0, 0), graph_.viewMatrix_.at(0, 1), graph_.viewMatrix_.at(0, 2)};
            Vec3 uBasis{graph_.viewMatrix_.at(1, 0), graph_.viewMatrix_.at(1, 1), graph_.viewMatrix_.at(1, 2)};
            Vec3 fBasis{-graph_.viewMatrix_.at(2, 0), -graph_.viewMatrix_.at(2, 1), -graph_.viewMatrix_.at(2, 2)};
            const bool persp = graph_.cameraIsPerspective_;

            // Number of cascades. Cap to remaining tile budget so we don't
            // blow past the atlas — better to drop late cascades than to
            // silently corrupt allocations. An orthographic camera has one
            // on-screen scale, so depth slices buy it nothing: one map.
            int N = L->cascadeCount();
            if (!persp) N = 1;
            int budgetLeft = kMaxShadowTiles - shadowTileCount_;
            if (N > budgetLeft) N = budgetLeft;
            if (N <= 0) continue;

            // Practical Split Scheme (Zhang et al., 2006): blend uniform and
            // log spacing. lambda ~0.5 works well outdoors; closer to 0
            // for tight indoor scenes with no far geometry.
            float zn = std::max(graph_.cameraNearZ_, 1e-3f);
            float zf = std::max(graph_.cameraFarZ_, zn + 1e-3f);
            float lambda = L->cascadeSplitLambda();
            // splitFar[c] = far view-distance of cascade c. cascade c covers
            // [splitFar[c-1], splitFar[c]], with splitFar[-1] = zn.
            float splitFar[5]; splitFar[0] = zn;
            for (int c = 1; c <= N; ++c) {
                float t = (float)c / (float)N;
                float uniform = zn + (zf - zn) * t;
                float logS    = zn * std::pow(zf / zn, t);
                splitFar[c] = lambda * logS + (1.0f - lambda) * uniform;
            }

            int firstSlot = shadowTileCount_;
            lightShadowSlot_[i] = firstSlot;
            lightShadowSlotCount_[i] = N;
            for (int c = 0; c < N - 1; ++c) {
                lightCascadeSplit_[i][c] = splitFar[c + 1];
            }
            // The last cascade absorbs anything farther — already 1e30f from reset.

            // A fixed light basis (eye at the world origin, looking along d)
            // so the projection window can be snapped to whole texels in a
            // frame that does not move with the camera. Looking AT the
            // cascade centre — the old way — puts the centre at light-space
            // (0,0) every frame, so snapping it snapped nothing and the map
            // shimmered under every pan.
            Vec3 up = (std::abs(d.y) > 0.99f) ? Vec3{0,0,1} : Vec3{0,1,0};
            Mat4 lightRot = bromath::mlookAt(Vec3{0, 0, 0}, d, up);

            // Caster extent along the light, in world units, for the depth
            // range: casters outside the fitted sphere (behind the camera,
            // above the view) must still write depth or they cast nothing
            // onto what IS in view.
            float castMinD =  1e30f, castMaxD = -1e30f;
            for (int c = 0; c < 8; ++c) {
                Vec3 p{(c & 1) ? casterBounds.max.x : casterBounds.min.x,
                       (c & 2) ? casterBounds.max.y : casterBounds.min.y,
                       (c & 4) ? casterBounds.max.z : casterBounds.min.z};
                float t = bromath::vdot(p, d);
                castMinD = std::min(castMinD, t);
                castMaxD = std::max(castMaxD, t);
            }

            // Per-cascade fit: the world-space corners of the camera
            // sub-volume [splitFar[c], splitFar[c+1]] — a frustum slice for
            // a perspective camera, a box for an orthographic one — CLIPPED
            // to the receiver bounds (the exact convex intersection), then
            // bounded with a sphere (rotation-stable) and fit as an ortho
            // frustum in light space. The clip is the whole story for an
            // ortho camera: its volume is near..far deep whatever is in it,
            // and an unclipped 1400 m slab over a 60 m scene is a 6 m texel.
            for (int c = 0; c < N; ++c) {
                float zNear = splitFar[c];
                float zFar  = splitFar[c + 1];
                float tanH  = std::tan(graph_.cameraFovY_ * 0.5f);

                Vec3 corners[8];
                for (int k = 0; k < 2; ++k) {
                    float z = (k == 0) ? zNear : zFar;
                    float hw, hh, cx = 0.0f, cy = 0.0f;
                    if (persp) {
                        hh = z * tanH;
                        hw = hh * graph_.cameraAspect_;
                    } else {
                        hw = 0.5f * (graph_.cameraOrthoR_ - graph_.cameraOrthoL_);
                        hh = 0.5f * (graph_.cameraOrthoT_ - graph_.cameraOrthoB_);
                        cx = 0.5f * (graph_.cameraOrthoR_ + graph_.cameraOrthoL_);
                        cy = 0.5f * (graph_.cameraOrthoT_ + graph_.cameraOrthoB_);
                    }
                    Vec3 cz = graph_.cameraEye_ + fBasis * z;
                    for (int j = 0; j < 4; ++j) {
                        float xs = (j & 1) ? 1.0f : -1.0f;
                        float ys = (j & 2) ? 1.0f : -1.0f;
                        corners[k*4 + j] = cz + sBasis * (cx + hw * xs)
                                              + uBasis * (cy + hh * ys);
                    }
                }

                Vec3 pts[64];
                int np = 0;
                if (!bromath::aisEmpty(receiverBounds))
                    np = clipFrustumToBox(corners, receiverBounds, pts);
                if (np == 0) {
                    // Nothing that receives shadow in this slice (or no
                    // bounds at all): fall back to the raw slice.
                    for (int k = 0; k < 8; ++k) pts[k] = corners[k];
                    np = 8;
                }

                bromath::AABB3 pb = bromath::aempty3();
                for (int k = 0; k < np; ++k) pb = bromath::aexpand(pb, pts[k]);
                Vec3 center = bromath::acenter(pb);
                float radius = 0.0f;
                for (int k = 0; k < np; ++k)
                    radius = std::max(radius, bromath::vlen(pts[k] - center));
                if (radius < 1e-3f) radius = 1.0f;
                // Quantise the radius to eighth-octave steps: a pan that
                // changes the clipped shape a little must not change the
                // texel size a little, or every shadow edge re-samples every
                // frame. It now steps (~9%) rarely instead.
                radius = std::exp2(std::ceil(std::log2(radius) * 8.0f) / 8.0f);

                // Snap the window to the texel grid in the fixed light basis.
                const float texelSize = (2.0f * radius) / (float)tilePx;
                Vec3 cLS = bromath::mtransformPoint(lightRot, center);
                cLS.x = std::floor(cLS.x / texelSize) * texelSize;
                cLS.y = std::floor(cLS.y / texelSize) * texelSize;

                // Depth along d (light view looks down -z, so depth = -z).
                // Cover the sphere, then stretch over every caster.
                const float centerD = bromath::vdot(center, d);
                float nearD = std::min(centerD - radius, castMinD);
                float farD  = std::max(centerD + radius, castMaxD);
                const float pad = std::max(0.5f, 0.01f * (farD - nearD));
                nearD -= pad; farD += pad;

                Mat4 proj = makeOrthoZeroToOne(cLS.x - radius, cLS.x + radius,
                                               cLS.y - radius, cLS.y + radius,
                                               nearD, farD);
                Mat4 projView = bromath::mmul(proj, lightRot);
                bakeTile(firstSlot + c, projView, L, texelSize, 0.0f,
                         nearD, farD, true);
                shadowTileCount_++;
            }
        }
        else if (L->kind() == LightNode::Kind::Spot) {
            // Spot light shadow = perspective projection from the light's
            // position along its direction. FOV = 2 * outerAngle so the
            // shadow frustum exactly covers the cone the FS computes
            // attenuation for. Range determines the far plane.
            Vec3 d = L->direction();
            float dlen = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
            if (dlen < 1e-6f) continue;
            d.x /= dlen; d.y /= dlen; d.z /= dlen;

            const Mat4& M = L->worldMatrix();
            Vec3 eye{M.at(0,3), M.at(1,3), M.at(2,3)};
            Vec3 target{eye.x + d.x, eye.y + d.y, eye.z + d.z};
            Vec3 up = (std::abs(d.y) > 0.99f) ? Vec3{0,0,1} : Vec3{0,1,0};

            float far  = std::max(L->range(), 0.5f);
            float near = std::max(0.1f, far * 0.005f);
            float fov  = 2.0f * std::max(L->outerAngle(), 0.05f);
            // Cap aperture below 180 deg so the perspective matrix stays sane.
            if (fov > 3.10f) fov = 3.10f;

            Mat4 view = bromath::mlookAt(eye, target, up);
            Mat4 proj = makePerspectiveZeroToOne(fov, 1.0f, near, far);
            Mat4 projView = bromath::mmul(proj, view);
            bakeTile(shadowTileCount_, projView, L,
                     0.0f, 2.0f * std::tan(fov * 0.5f) / (float)tilePx,
                     near, far, false);
            lightShadowSlot_[i] = shadowTileCount_;
            lightShadowSlotCount_[i] = 1;
            shadowTileCount_++;
        }
        else if (L->kind() == LightNode::Kind::Point) {
            // Point light = 6-face cube projection. Each face gets its own
            // atlas tile rendered with perspective(90deg, 1, near, far).
            // Needs 6 contiguous slots; skip if the budget can't fit them.
            if (shadowTileCount_ + 6 > kMaxShadowTiles) continue;

            const Mat4& M = L->worldMatrix();
            Vec3 eye{M.at(0,3), M.at(1,3), M.at(2,3)};
            float far  = std::max(L->range(), 0.5f);
            float near = std::max(0.05f, far * 0.005f);
            // PI/2 + small fudge so the 6 frusta have a smidge of overlap
            // at the seams; eliminates a single-texel sliver of "no shadow"
            // at face boundaries.
            Mat4 proj = makePerspectiveZeroToOne(1.5708f, 1.0f, near, far);

            // Cube-face conventions (matches D3D / OpenGL cube map order).
            // Each entry is { forward.xyz, up.xyz }.
            const Vec3 forward[6] = {
                { 1, 0, 0}, {-1, 0, 0},
                { 0, 1, 0}, { 0,-1, 0},
                { 0, 0, 1}, { 0, 0,-1},
            };
            const Vec3 upVec[6] = {
                {0,-1, 0}, {0,-1, 0},
                {0, 0, 1}, {0, 0,-1},
                {0,-1, 0}, {0,-1, 0},
            };

            int firstSlot = shadowTileCount_;
            lightShadowSlot_[i] = firstSlot;
            lightShadowSlotCount_[i] = 6;
            for (int f = 0; f < 6; ++f) {
                Vec3 target{eye.x + forward[f].x,
                            eye.y + forward[f].y,
                            eye.z + forward[f].z};
                Mat4 view = bromath::mlookAt(eye, target, upVec[f]);
                Mat4 projView = bromath::mmul(proj, view);
                // 90 deg face: a texel spans 2*tan(45deg)/tilePx per metre.
                bakeTile(firstSlot + f, projView, L,
                         0.0f, 2.0f / (float)tilePx, near, far, false);
                shadowTileCount_++;
            }
        }
    }
}

void SceneRenderer::renderShadowPass() {
    if (shadowTileCount_ == 0) return;
    ensureShadowPipeline();
    ensureShadowAtlas();
    if (!shadowProgram_ || !shadowAtlasFBO_) return;
    const bool hasInstancedCasters = !shadowInstancedCasters_.empty();
    const bool hasSkinnedCasters = !shadowSkinnedCasters_.empty();
    const bool hasCustomCasters = !shadowCustomCasters_.empty();
    const bool hasSkinnedCustomCasters = !shadowSkinnedCustomCasters_.empty();
    const bool hasTubeCasters = !shadowTubeCasters_.empty();

    const int tileSize = shadowAtlasSize_ / shadowGridDim_;  // grid chosen in prepareShadows

    // Per-tile frustum culling: casters are tested against each tile's light
    // volume (cascade ortho box, spot cone, point cube face — all encoded by
    // the tile's world-space lightVP, the exact matrix the rasterizer clips
    // against). NEVER the camera frustum: an off-screen caster must still
    // shadow on-screen geometry. Caster world bounds are cached once per
    // frame (posed for skinned casters); a caster without valid bounds draws
    // into every tile.
    struct CasterBounds { bromath::AABB3 box; bool valid; };
    std::vector<CasterBounds> staticBounds, skinnedBounds, instBounds,
                              customBounds, skinnedCustomBounds, tubeBounds;
    if (cullingActive_) {
        auto cache = [&](auto& casters, std::vector<CasterBounds>& out) {
            out.resize(casters.size());
            for (size_t i = 0; i < casters.size(); ++i) {
                if (auto wbOpt = nodeWorldBounds(casters[i])) {
                    out[i].valid = true;
                    out[i].box = *wbOpt;
                } else {
                    out[i].valid = false;
                }
            }
        };
        cache(shadowCasters_, staticBounds);
        cache(shadowSkinnedCasters_, skinnedBounds);
        cache(shadowCustomCasters_, customBounds);
        cache(shadowSkinnedCustomCasters_, skinnedCustomBounds);
        cache(shadowInstancedCasters_, instBounds);
        cache(shadowTubeCasters_, tubeBounds);
    }

    // Tile frustums, shared by the cache-signature build below and the
    // per-caster cull in the draw loop.
    Mat4 tileVP[kMaxShadowTiles];
    bromath::Frustum tileFrustum[kMaxShadowTiles];
    for (int slot = 0; slot < shadowTileCount_; ++slot) {
        std::memcpy(tileVP[slot].data, shadowRenderMatrix_[slot], sizeof(float) * 16);
        if (cullingActive_) tileFrustum[slot] = makeFrustum(tileVP[slot]);
    }

    // --- Static shadow-tile cache decision -------------------------------
    // A tile's depth content is a pure function of its lightVP matrix and
    // the (geometry, world transform) of every caster overlapping its
    // frustum — so it can be reused verbatim when neither changed. The
    // signature is the ordered (node id, change generation) list of
    // overlapping casters with per-list separators; conservative-correct:
    // any membership or generation difference re-renders, and tiles touched
    // by skinned or custom-vertex casters (pose/displacement changes with
    // no generation signal) are permanently dynamic. Directional cascades
    // fold camera motion into lightVP (the fit follows the camera), so they
    // only cache while the camera is still; spot/point tiles are camera-
    // independent and cache across any camera movement.
    cullStats_.shadowTilesTotal += shadowTileCount_;
    const bool fullClear = shadowAtlasNeedsClear_ || !shadowCacheEnabled_;
    bool renderSlot[kMaxShadowTiles] = {};
    bool anyRender = false;
    if (!shadowCacheEnabled_) {
        for (int slot = 0; slot < shadowTileCount_; ++slot) renderSlot[slot] = true;
        anyRender = true;
        cullStats_.shadowTilesRendered += shadowTileCount_;
    } else {
        std::vector<std::pair<uint32_t, uint64_t>> sig;
        for (int slot = 0; slot < shadowTileCount_; ++slot) {
            sig.clear();
            bool dynamic = false;
            auto addList = [&](const auto& casters,
                               const std::vector<CasterBounds>& bounds,
                               uint64_t listTag, bool listDynamic) {
                sig.emplace_back(0u, listTag);  // separator — node ids start at 1
                for (size_t i = 0; i < casters.size(); ++i) {
                    if (cullingActive_ && bounds[i].valid &&
                        !bromath::fintersects(tileFrustum[slot], bounds[i].box))
                        continue;
                    sig.emplace_back(casters[i]->id(),
                                     casters[i]->changeGeneration());
                    if (listDynamic) dynamic = true;
                }
            };
            addList(shadowCasters_,              staticBounds,        1, false);
            addList(shadowSkinnedCasters_,       skinnedBounds,       2, true);
            addList(shadowCustomCasters_,        customBounds,        3, false);
            addList(shadowSkinnedCustomCasters_, skinnedCustomBounds, 4, true);
            addList(shadowInstancedCasters_,     instBounds,          5, false);
            addList(shadowTubeCasters_,          tubeBounds,          6, false);

            ShadowTileCacheEntry& e = shadowTileCache_[slot];
            const uint32_t lightId = shadowTileLight_[slot]->id();
            if (!fullClear && !dynamic && e.valid && e.lightId == lightId &&
                std::memcmp(e.lightVP, shadowRenderMatrix_[slot],
                            sizeof(e.lightVP)) == 0 &&
                e.casters == sig) {
                // Atlas texels already hold exactly this content; the FS
                // sampling matrices were refreshed by prepareShadows.
                cullStats_.shadowTilesCached++;
                continue;
            }
            renderSlot[slot] = true;
            anyRender = true;
            cullStats_.shadowTilesRendered++;
            // Record what the tile is about to contain. Dynamic tiles never
            // validate — their casters mutate without a generation bump.
            e.valid = !dynamic;
            e.lightId = lightId;
            std::memcpy(e.lightVP, shadowRenderMatrix_[slot], sizeof(e.lightVP));
            e.casters = sig;
        }
    }
    if (!anyRender) return;  // every tile reused — no GL work at all

    if (hasInstancedCasters) ensureShadowInstancedPipeline();
    if (hasTubeCasters) ensureTubeDepthPipeline();
    if (hasSkinnedCasters) ensureShadowSkinnedPipeline();
    // Custom casters whose shadow variant failed to compile (cached failure
    // in ensureCustomShadowProgram) fall back to the default depth programs.
    if (hasSkinnedCustomCasters) ensureShadowSkinnedPipeline();

    glBindFramebuffer(GL_FRAMEBUFFER, shadowAtlasFBO_);
    glViewport(0, 0, shadowAtlasSize_, shadowAtlasSize_);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_TRUE);
    if (fullClear) {
        glClearDepth(1.0);   // shadow depth stays conventional: near 0, far 1
        glClear(GL_DEPTH_BUFFER_BIT);
        shadowAtlasNeedsClear_ = false;
    }
    // Cached path clears per tile (scissored) in the loop below, so
    // neighbouring reused tiles keep their depth.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // Front-face culling reduces self-shadow acne on closed convex meshes
    // because back-faces (relative to the light) carry the depth value used
    // for comparison. Opens up a peter-panning risk on thin geometry — the
    // normal-bias + constant bias compensate.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);

    // Slope-scaled depth bias shifts stored depth values away from the light
    // proportional to surface slope. This is the big hammer for self-shadow
    // acne — constant/normal bias alone can't cover the full dynamic range
    // of slopes a directional light sees across the scene.
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);

    glUseProgram(shadowProgram_);

    // Resolve each custom caster's shadow-variant entry once per frame, not
    // per tile — the cache key embeds the full chunk source, so a per-tile
    // lookup would re-hash the whole GLSL string N-tiles times. Entry
    // pointers stay valid (unordered_map nodes are stable); nullptr means
    // "variant failed to compile, use the default depth program". The lists
    // are sorted by chunk, so consecutive casters share resolutions.
    std::vector<CustomShadowEntry*> customEntries, skinnedCustomEntries;
    auto resolveEntries = [&](const std::vector<MeshNode*>& casters,
                              std::vector<CustomShadowEntry*>& out,
                              bool skinned) {
        out.resize(casters.size());
        const std::string* prevChunk = nullptr;
        CustomShadowEntry* prev = nullptr;
        for (size_t i = 0; i < casters.size(); ++i) {
            const std::string& chunk = casters[i]->customShader()->vertexChunk;
            if (!prevChunk || *prevChunk != chunk) {
                prev = ensureCustomShadowProgram(skinned, chunk);
                prevChunk = &chunk;
            }
            out[i] = prev;
        }
    };
    if (hasCustomCasters)
        resolveEntries(shadowCustomCasters_, customEntries, false);
    if (hasSkinnedCustomCasters)
        resolveEntries(shadowSkinnedCustomCasters_, skinnedCustomEntries, true);

    for (int slot = 0; slot < shadowTileCount_; ++slot) {
        if (!renderSlot[slot]) continue;  // tile reused from a previous frame
        int gx = slot % shadowGridDim_;
        int gy = slot / shadowGridDim_;
        glViewport(gx * tileSize, gy * tileSize, tileSize, tileSize);
        if (!fullClear) {
            // Scissored per-tile clear: only re-rendered tiles are wiped;
            // cached neighbours keep their depth. Same clear value as the
            // full clear, so the two paths are pixel-identical.
            glEnable(GL_SCISSOR_TEST);
            glScissor(gx * tileSize, gy * tileSize, tileSize, tileSize);
            glClearDepth(1.0);   // shadow depth stays conventional: near 0, far 1
            glClear(GL_DEPTH_BUFFER_BIT);
        }

        // shadowRenderMatrix_ holds lightProj*lightView in WORLD space.
        // Per-mesh: uMVP = renderMatrix * meshWorldModel.
        const Mat4& lightVP = tileVP[slot];

        auto tileCulled = [&](const std::vector<CasterBounds>& bounds, size_t i) {
            if (!cullingActive_ || !bounds[i].valid) return false;
            return !bromath::fintersects(tileFrustum[slot], bounds[i].box);
        };

        for (size_t i = 0; i < shadowCasters_.size(); ++i) {
            if (tileCulled(staticBounds, i)) { cullStats_.shadowCulled++; continue; }
            cullStats_.shadowDrawn++;
            MeshNode* mesh = shadowCasters_[i];
            Mat4 mvp = bromath::mmul(lightVP, mesh->worldMatrix());
            glUniformMatrix4fv(shadowUMVP_, 1, GL_FALSE, mvp.data);
            mesh->drawRaw();
        }

        // Skinned casters: SKINNED depth shader + per-node palette UBO, so
        // shadows deform with the mesh instead of staying in bind pose.
        if (hasSkinnedCasters && shadowSkinnedProgram_) {
            glUseProgram(shadowSkinnedProgram_);
            for (size_t i = 0; i < shadowSkinnedCasters_.size(); ++i) {
                if (tileCulled(skinnedBounds, i)) { cullStats_.shadowCulled++; continue; }
                cullStats_.shadowDrawn++;
                MeshNode* mesh = shadowSkinnedCasters_[i];
                Mat4 mvp = bromath::mmul(lightVP, mesh->worldMatrix());
                glUniformMatrix4fv(shadowSkinnedUMVP_, 1, GL_FALSE, mvp.data);
                mesh->asSkinnedMesh()->prepareSkinnedDraw();
                mesh->drawRaw();
            }
            glUseProgram(shadowProgram_);
        }

        // Custom-vertex casters: the spliced shadow variant runs the user's
        // userVertex hook so the DISPLACED silhouette lands in the atlas.
        // Casters are pre-sorted by chunk source (entries resolved once
        // before the tile loop), so each variant binds once per group; user
        // uniforms upload per caster (displacement may be uniform-driven,
        // and values are per-node). A variant whose compile failed (cached
        // in ensureCustomShadowProgram, entry == nullptr) falls back to the
        // default depth program — undisplaced, but still a shadow.
        auto drawCustomCasters = [&](const std::vector<MeshNode*>& casters,
                                     const std::vector<CasterBounds>& bounds,
                                     const std::vector<CustomShadowEntry*>& entries,
                                     bool skinned) {
            const GLuint fallback = skinned ? shadowSkinnedProgram_
                                            : shadowProgram_;
            const GLint fallbackMVP = skinned ? shadowSkinnedUMVP_
                                              : shadowUMVP_;
            bool anyBound = false;
            CustomShadowEntry* bound = nullptr;
            for (size_t i = 0; i < casters.size(); ++i) {
                if (tileCulled(bounds, i)) { cullStats_.shadowCulled++; continue; }
                cullStats_.shadowDrawn++;
                MeshNode* mesh = casters[i];
                CustomShadowEntry* entry = entries[i];
                if (!anyBound || entry != bound) {
                    anyBound = true;
                    bound = entry;
                    if (entry) {
                        glUseProgram(entry->prog);
                        // Wind globals: the custom variant applies the same
                        // pre-hook wind sway as mesh.vert so the hook input
                        // matches the color pass exactly.
                        if (entry->windDir >= 0)
                            glUniform3fv(entry->windDir, 1, windDir_);
                        if (entry->windStrength >= 0)
                            glUniform1f(entry->windStrength, windStrength_);
                        if (entry->windTime >= 0)
                            glUniform1f(entry->windTime, windTime_);
                        if (entry->windFreq >= 0)
                            glUniform1f(entry->windFreq, windFreq_);
                    } else if (fallback) {
                        glUseProgram(fallback);
                    }
                }
                if (!entry && !fallback) continue;
                Mat4 mvp = bromath::mmul(lightVP, mesh->worldMatrix());
                if (entry) {
                    glUniformMatrix4fv(entry->mvp, 1, GL_FALSE, mvp.data);
                    if (entry->model >= 0)
                        glUniformMatrix4fv(entry->model, 1, GL_FALSE,
                                           mesh->worldMatrix().data);
                    if (entry->windMask >= 0)
                        glUniform1f(entry->windMask, mesh->windMask());
                    uploadUserUniforms(entry->prog, entry->userLocs,
                                       mesh->customShader());
                    // Same sampler bindings as the color pass: a vertex chunk
                    // that displaces from a height texture must read the same
                    // texels here, or the caster's depth silhouette won't
                    // match the geometry the color pass actually draws.
                    uploadUserTextures(entry->prog, entry->userLocs, mesh);
                } else {
                    glUniformMatrix4fv(fallbackMVP, 1, GL_FALSE, mvp.data);
                }
                if (skinned) mesh->asSkinnedMesh()->prepareSkinnedDraw();
                mesh->drawRaw();
            }
            glUseProgram(shadowProgram_);
        };
        if (hasCustomCasters)
            drawCustomCasters(shadowCustomCasters_, customBounds,
                              customEntries, false);
        if (hasSkinnedCustomCasters)
            drawCustomCasters(shadowSkinnedCustomCasters_, skinnedCustomBounds,
                              skinnedCustomEntries, true);

        if (hasInstancedCasters && shadowInstancedProgram_) {
            glUseProgram(shadowInstancedProgram_);
            glUniformMatrix4fv(shadowInstULightVP_, 1, GL_FALSE, lightVP.data);
            for (size_t i = 0; i < shadowInstancedCasters_.size(); ++i) {
                if (tileCulled(instBounds, i)) { cullStats_.shadowCulled++; continue; }
                cullStats_.shadowDrawn++;
                InstancedMeshNode* m = shadowInstancedCasters_[i];
                glUniformMatrix4fv(shadowInstUModel_, 1, GL_FALSE, m->worldMatrix().data);
                m->drawRawInstancedDepth();
            }
            glUseProgram(shadowProgram_);
        }

        // Branch-tube casters: the SHADOW_PASS variant of the tube VS emits the
        // tube silhouette in light space. The segment TBO binds to unit 10 (the
        // same unit the forward tube pass uses).
        if (hasTubeCasters && tubeDepthProgram_) {
            glUseProgram(tubeDepthProgram_);
            glUniformMatrix4fv(tubeDepthULightVP_, 1, GL_FALSE, lightVP.data);
            for (size_t i = 0; i < shadowTubeCasters_.size(); ++i) {
                if (tileCulled(tubeBounds, i)) { cullStats_.shadowCulled++; continue; }
                cullStats_.shadowDrawn++;
                InstancedMeshNode* m = shadowTubeCasters_[i];
                glUniformMatrix4fv(tubeDepthUModel_, 1, GL_FALSE, m->worldMatrix().data);
                if (tubeDepthUSides_ >= 0) glUniform1i(tubeDepthUSides_, m->tubeSides());
                if (tubeDepthURadiusScale_ >= 0)
                    glUniform1f(tubeDepthURadiusScale_, m->tubeRadiusScale());
                glActiveTexture(GL_TEXTURE10);
                glBindTexture(GL_TEXTURE_BUFFER, m->tubeSegTexture());
                if (tubeDepthUSegments_ >= 0) glUniform1i(tubeDepthUSegments_, 10);
                glActiveTexture(GL_TEXTURE0);
                m->drawTubeDepth();
            }
            glUseProgram(shadowProgram_);
        }
    }

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.0f, 0.0f);
    glCullFace(GL_BACK);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
}

}  // namespace bro::scene
