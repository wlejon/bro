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

SceneRenderer::WorldAABB SceneRenderer::computeShadowCasterBounds() const {
    WorldAABB out{};
    out.empty = true;
    out.min[0] = out.min[1] = out.min[2] =  1e30f;
    out.max[0] = out.max[1] = out.max[2] = -1e30f;

    auto expand = [&](const float lo[3], const float hi[3]) {
        for (int c = 0; c < 8; ++c) {
            float p[3] = {
                (c & 1) ? hi[0] : lo[0],
                (c & 2) ? hi[1] : lo[1],
                (c & 4) ? hi[2] : lo[2],
            };
            out.min[0] = std::min(out.min[0], p[0]);
            out.min[1] = std::min(out.min[1], p[1]);
            out.min[2] = std::min(out.min[2], p[2]);
            out.max[0] = std::max(out.max[0], p[0]);
            out.max[1] = std::max(out.max[1], p[1]);
            out.max[2] = std::max(out.max[2], p[2]);
            out.empty = false;
        }
    };
    auto walk = [&](auto&& self, SceneNode* n) -> void {
        if (!n || !n->visible()) return;
        if (n->type() == SceneNode::Type::Mesh) {
            auto* m = static_cast<MeshNode*>(n);
            // Same unlit rule as the caster gather (effectiveUnlit): a
            // custom shader suppresses unlit, so such meshes count here too.
            if (!m->effectiveUnlit() && !m->mesh().empty()) {
                // cullMargin pads the fit the same way it pads culling
                // bounds (world units) — a custom vertex shader can displace
                // geometry along the light direction past the bind-pose
                // AABB, and the directional depth range is fit from these
                // bounds.
                const auto& bb = m->localBounds();
                const float cm = m->cullMargin();
                const Mat4& M = m->worldMatrix();
                for (int c = 0; c < 8; ++c) {
                    Vec3 lp{
                        (c & 1) ? bb.max.x : bb.min.x,
                        (c & 2) ? bb.max.y : bb.min.y,
                        (c & 4) ? bb.max.z : bb.min.z,
                    };
                    Vec3 wp = bromath::mtransformPoint(M, lp);
                    out.min[0] = std::min(out.min[0], wp.x - cm);
                    out.min[1] = std::min(out.min[1], wp.y - cm);
                    out.min[2] = std::min(out.min[2], wp.z - cm);
                    out.max[0] = std::max(out.max[0], wp.x + cm);
                    out.max[1] = std::max(out.max[1], wp.y + cm);
                    out.max[2] = std::max(out.max[2], wp.z + cm);
                    out.empty = false;
                }
            }
        } else if (n->type() == SceneNode::Type::InstancedMesh) {
            auto* m = static_cast<InstancedMeshNode*>(n);
            float wlo[3], whi[3];
            if (m->computeWorldInstanceBounds(wlo, whi)) {
                const float cm = m->cullMargin();
                for (int i = 0; i < 3; ++i) { wlo[i] -= cm; whi[i] += cm; }
                expand(wlo, whi);
            }
        }
        for (auto* c : n->children()) self(self, c);
    };
    walk(walk, graph_.root_.get());
    return out;
}

void SceneRenderer::prepareShadows(const std::vector<LightNode*>& lights) {
    // Reset per-frame shadow state. Default every light to "no shadow".
    shadowTileCount_ = 0;
    shadowCasters_.clear();
    shadowSkinnedCasters_.clear();
    shadowCustomCasters_.clear();
    shadowSkinnedCustomCasters_.clear();
    shadowInstancedCasters_.clear();
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
        if (!n || !n->visible()) return;
        if (n->type() == SceneNode::Type::Mesh) {
            auto* m = static_cast<MeshNode*>(n);
            if (!m->effectiveUnlit() && m->castsShadow() && !m->mesh().empty()) {
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
            if (!m->effectiveUnlit() && m->castsShadow() && !m->mesh().empty() && m->instanceCount() > 0)
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
        shadowInstancedCasters_.empty()) return;

    // Scene bounds for fitting directional frustums. CSM uses view-frustum
    // slices instead — added in a follow-up commit.
    WorldAABB bounds = computeShadowCasterBounds();
    if (bounds.empty) return;

    // Bias matrix maps NDC [-1,1] to UV [0,1] in all three dims.
    Mat4 bias = bromath::mmul(bromath::mtranslate({0.5f, 0.5f, 0.5f}), bromath::mscale({0.5f, 0.5f, 0.5f}));

    // Allocate atlas tiles in a square grid: ceil(sqrt(MAX)) x ceil(sqrt(MAX)).
    // For MAX=16 this gives a clean 4x4. Each tile gets equal area.
    const int gridDim = 4;                     // 4x4 = 16 tiles
    const float tileUV = 1.0f / (float)gridDim; // 0.25 per tile

    auto bakeTile = [&](int slot, const Mat4& lightProjView, LightNode* L) {
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

        shadowTileLight_[slot] = L;
    };

    // For each shadow-casting light, allocate slot(s) and build matrices.
    // Spot/Point are deferred to follow-up commits — only Directional fits
    // the scene-bounds-ortho path here.
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

            // Number of cascades. Cap to remaining tile budget so we don't
            // blow past the atlas — better to drop late cascades than to
            // silently corrupt allocations.
            int N = L->cascadeCount();
            if (graph_.cameraIsPerspective_ == false) N = 1;  // ortho cam = no need for splits
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

            // Per-cascade fit: find the world-space corners of the camera
            // sub-frustum [splitFar[c], splitFar[c+1]], then bound them
            // with a sphere (rotation-stable; eliminates shimmer when the
            // camera turns) and fit an ortho frustum in the light's view.
            for (int c = 0; c < N; ++c) {
                float zNear = splitFar[c];
                float zFar  = splitFar[c + 1];
                float tanH  = std::tan(graph_.cameraFovY_ * 0.5f);

                Vec3 corners[8];
                for (int k = 0; k < 2; ++k) {
                    float z  = (k == 0) ? zNear : zFar;
                    float hh = z * tanH;
                    float hw = hh * graph_.cameraAspect_;
                    Vec3 cz{graph_.cameraEye_.x + fBasis.x * z,
                            graph_.cameraEye_.y + fBasis.y * z,
                            graph_.cameraEye_.z + fBasis.z * z};
                    for (int j = 0; j < 4; ++j) {
                        float xs = (j & 1) ? 1.0f : -1.0f;
                        float ys = (j & 2) ? 1.0f : -1.0f;
                        corners[k*4 + j] = Vec3{
                            cz.x + sBasis.x * (hw * xs) + uBasis.x * (hh * ys),
                            cz.y + sBasis.y * (hw * xs) + uBasis.y * (hh * ys),
                            cz.z + sBasis.z * (hw * xs) + uBasis.z * (hh * ys)};
                    }
                }

                Vec3 center{0,0,0};
                for (int k = 0; k < 8; ++k) {
                    center.x += corners[k].x;
                    center.y += corners[k].y;
                    center.z += corners[k].z;
                }
                center.x *= 0.125f; center.y *= 0.125f; center.z *= 0.125f;

                float radius = 0.0f;
                for (int k = 0; k < 8; ++k) {
                    float dx = corners[k].x - center.x;
                    float dy = corners[k].y - center.y;
                    float dz = corners[k].z - center.z;
                    radius = std::max(radius, std::sqrt(dx*dx + dy*dy + dz*dz));
                }
                if (radius < 1e-3f) radius = 1.0f;
                // Snap radius to 16ths of a unit so it doesn't change every
                // micro-frame; combined with sphere fit this is the second
                // half of the texel-snap shimmer fix.
                radius = std::ceil(radius * 16.0f) / 16.0f;

                // Light-space view: looking from above the bounding sphere
                // along the light direction, looking AT the sphere center.
                Vec3 eye{ center.x - d.x * radius * 2.0f,
                          center.y - d.y * radius * 2.0f,
                          center.z - d.z * radius * 2.0f };
                Vec3 up = (std::abs(d.y) > 0.99f) ? Vec3{0,0,1} : Vec3{0,1,0};
                Mat4 view = bromath::mlookAt(eye, center, up);

                // Texel-snap the cascade origin in light-space xy. Without
                // this the shadow edges shimmer as the camera moves because
                // the same world fragment maps to slightly different texels
                // each frame. Snap the world center, not the projection.
                int tilePx = shadowAtlasSize_ / 4;
                float texelSize = (2.0f * radius) / (float)tilePx;
                Vec3 centerLS = bromath::mtransformPoint(view, center);
                float snapX = std::floor(centerLS.x / texelSize) * texelSize;
                float snapY = std::floor(centerLS.y / texelSize) * texelSize;
                float dxLS = centerLS.x - snapX;
                float dyLS = centerLS.y - snapY;
                // Build ortho extents around the snapped origin.
                Mat4 proj = bromath::mortho(
                    -radius - dxLS, radius - dxLS,
                    -radius - dyLS, radius - dyLS,
                    -radius * 2.0f - radius, -(-radius * 2.0f) + radius);
                // Expand the depth range: scene casters outside the sphere
                // should still write their depths (otherwise close objects
                // behind the cascade get omitted from the shadow). Use the
                // scene AABB extent along the light direction as an extra
                // pad on the near side.
                Vec3 boundsCenter{
                    0.5f * (bounds.min[0] + bounds.max[0]),
                    0.5f * (bounds.min[1] + bounds.max[1]),
                    0.5f * (bounds.min[2] + bounds.max[2])};
                Vec3 boundsExt{
                    0.5f * (bounds.max[0] - bounds.min[0]),
                    0.5f * (bounds.max[1] - bounds.min[1]),
                    0.5f * (bounds.max[2] - bounds.min[2])};
                float sceneRadius = std::sqrt(boundsExt.x*boundsExt.x +
                                              boundsExt.y*boundsExt.y +
                                              boundsExt.z*boundsExt.z);
                float depthExt = std::max(sceneRadius * 2.0f, radius * 4.0f);
                proj = bromath::mortho(
                    -radius - dxLS, radius - dxLS,
                    -radius - dyLS, radius - dyLS,
                    0.0f, depthExt);

                Mat4 projView = bromath::mmul(proj, view);
                bakeTile(firstSlot + c, projView, L);
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
            Mat4 proj = bromath::mperspective(fov, 1.0f, near, far);
            Mat4 projView = bromath::mmul(proj, view);
            bakeTile(shadowTileCount_, projView, L);
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
            Mat4 proj = bromath::mperspective(1.5708f, 1.0f, near, far);

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
                bakeTile(firstSlot + f, projView, L);
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

    const int tileSize = shadowAtlasSize_ / 4;  // matches gridDim in prepareShadows

    // Per-tile frustum culling: casters are tested against each tile's light
    // volume (cascade ortho box, spot cone, point cube face — all encoded by
    // the tile's world-space lightVP, the exact matrix the rasterizer clips
    // against). NEVER the camera frustum: an off-screen caster must still
    // shadow on-screen geometry. Caster world bounds are cached once per
    // frame (posed for skinned casters); a caster without valid bounds draws
    // into every tile.
    struct CasterBounds { bromath::AABB3 box; bool valid; };
    std::vector<CasterBounds> staticBounds, skinnedBounds, instBounds,
                              customBounds, skinnedCustomBounds;
    if (cullingActive_) {
        auto cache = [&](auto& casters, std::vector<CasterBounds>& out) {
            out.resize(casters.size());
            for (size_t i = 0; i < casters.size(); ++i)
                out[i].valid = nodeWorldBounds(casters[i], out[i].box);
        };
        cache(shadowCasters_, staticBounds);
        cache(shadowSkinnedCasters_, skinnedBounds);
        cache(shadowCustomCasters_, customBounds);
        cache(shadowSkinnedCustomCasters_, skinnedCustomBounds);
        cache(shadowInstancedCasters_, instBounds);
    }

    // Tile frustums, shared by the cache-signature build below and the
    // per-caster cull in the draw loop.
    Mat4 tileVP[kMaxShadowTiles];
    bromath::Frustum tileFrustum[kMaxShadowTiles];
    for (int slot = 0; slot < shadowTileCount_; ++slot) {
        std::memcpy(tileVP[slot].data, shadowRenderMatrix_[slot], sizeof(float) * 16);
        if (cullingActive_) tileFrustum[slot] = bromath::ffromViewProj(tileVP[slot]);
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
            addList(shadowCustomCasters_,        customBounds,        3, true);
            addList(shadowSkinnedCustomCasters_, skinnedCustomBounds, 4, true);
            addList(shadowInstancedCasters_,     instBounds,          5, false);

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
        int gx = slot % 4;
        int gy = slot / 4;
        glViewport(gx * tileSize, gy * tileSize, tileSize, tileSize);
        if (!fullClear) {
            // Scissored per-tile clear: only re-rendered tiles are wiped;
            // cached neighbours keep their depth. Same clear value as the
            // full clear, so the two paths are pixel-identical.
            glEnable(GL_SCISSOR_TEST);
            glScissor(gx * tileSize, gy * tileSize, tileSize, tileSize);
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
