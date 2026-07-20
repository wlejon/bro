#include "scene/clipmap_terrain.h"

#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "util/log.h"

#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "clipmap_common.glsl.h" // kClipmapCommonSrc
#include "clipmap_detail.glsl.h"   // kClipmapDetailSrc
#include "clipmap_material.glsl.h" // kClipmapMaterialSrc
#include "clipmap.vert.glsl.h"   // kClipmapVertSrc
#include "clipmap.frag.glsl.h"   // kClipmapFragSrc

namespace bro::scene {

namespace {

// Uniform names, indexed by layer. Kept next to the shader's declarations.
const char* kLayerA[ClipmapTerrain::kMaxLayers] = {"u_l0a", "u_l1a", "u_l2a", "u_l3a"};
const char* kLayerB[ClipmapTerrain::kMaxLayers] = {"u_l0b", "u_l1b", "u_l2b", "u_l3b"};
const char* kLayerTex[ClipmapTerrain::kMaxLayers] = {"u_h0", "u_h1", "u_h2", "u_h3"};

// Must match CM_FADE in clipmap.vert.glsl / clipmap.frag.glsl.
constexpr float kFade = 0.08f;

float smoothstep01(float edge1, float x) {
    float t = std::clamp(x / edge1, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ClipmapTerrain::ClipmapTerrain(SceneGraph& graph, const ClipmapConfig& cfg)
    : graph_(graph), cfg_(cfg) {
    // The central hole is resolution/2 quads inset by resolution/4 on each
    // side, so the resolution must be a multiple of 4 for the rings to tile
    // exactly. Round rather than reject — a 126 that silently became a
    // half-cell-offset hole would be a far worse failure mode.
    cfg_.resolution = std::max(4, (cfg_.resolution / 4) * 4);
    cfg_.levels     = std::clamp(cfg_.levels, 1, 20);
    if (!(cfg_.cellSize > 0.0f)) cfg_.cellSize = 1.0f;

    node_ = graph_.createMesh("clipmapTerrain");
    if (!node_) return;
    graph_.root()->addChild(node_);

    buildGeometry();

    // Both stages get the same height source prepended. GLSL 330 has no
    // #include, and the two chunks are separate compilation units, so the
    // alternative is two copies that must stay byte-identical — a promise
    // rather than a guarantee. If the stages ever disagreed, the surface the
    // fragment shades would stop being the surface the vertex built.
    const std::string shared = std::string(kClipmapCommonSrc) + kClipmapDetailSrc;
    node_->setCustomShader(shared + kClipmapVertSrc,
                           shared + kClipmapMaterialSrc + kClipmapFragSrc);
    node_->setColor(0.40f, 0.44f, 0.36f, 1.0f);
    node_->setMetallic(0.0f);
    node_->setRoughness(0.95f);
    // A ring stack tens of kilometres across would swallow the whole shadow
    // atlas fit if it were a caster (the fit is padded by cullMargin, which is
    // itself huge here). It still RECEIVES shadows, which is what matters for
    // objects standing on it.
    node_->setCastsShadow(false);
    node_->setReceivesShadow(true);

    // Every sampler slot is bound from the start with a 1x1 zero placeholder.
    // An unbound sampler unit is undefined behaviour to read, and the shader
    // evaluates all four branches' texture fetches on some drivers regardless
    // of u_layerCount.
    const float zero = 0.0f;
    for (int i = 0; i < kMaxLayers; ++i)
        node_->setCustomShaderTexture(kLayerTex[i], 1, 1, &zero, true);

    pushStaticUniforms();
    pushLayerUniforms();
    update(0.0f, 0.0f, 0.0f);
}

ClipmapTerrain::~ClipmapTerrain() { destroy(); }

void ClipmapTerrain::destroy() {
    if (node_) {
        graph_.destroyNode(node_);
        node_ = nullptr;
    }
    for (auto& l : layers_) l = ClipmapLayer{};
    layerCount_ = 0;
}

// ---------------------------------------------------------------------------
// Geometry — built ONCE. Nothing below runs again for the node's lifetime.
// ---------------------------------------------------------------------------

void ClipmapTerrain::buildGeometry() {
    const int N = cfg_.resolution;
    const int L = cfg_.levels;
    const int row = N + 1;

    // Level 0 is a full N*N quad grid; levels 1..L-1 omit a central block the
    // next finer level covers.
    //
    // That block is NOT the full N/2 x N/2, and the shortfall is not slack —
    // it is what makes per-level snapping safe. Level l snaps its centre to a
    // 2*c_l grid and level l-1 to a 2*c_(l-1) = c_l grid. Those grids are
    // nested, so the two centres differ by 0 or exactly one c_l — and level
    // l-1's half-extent (N/2)*c_(l-1) is EXACTLY the half-extent of a full
    // N/2 x N/2 hole. Offset centres therefore leave a one-cell sliver of
    // nothing at the boundary (visible as background pixels straight through
    // the terrain). Insetting the hole by kHoleInset coarse cells per side
    // makes the rings overlap instead. The overlap costs a few percent more
    // quads and is invisible: both rings evaluate the same pure function of
    // world XZ at the same distance-derived mip, so the overlapping pixels are
    // identical in height, normal and shade — even z-fighting between them
    // resolves to the same colour.
    constexpr int kHoleInset = 2;   // 1 suffices geometrically; 2 is headroom
    const int holeQuads = N / 2 - 2 * kHoleInset;
    const bool hasHole = holeQuads > 0;

    bromesh::MeshData m;
    const size_t verts = static_cast<size_t>(row) * row * L;
    const size_t quads = static_cast<size_t>(N) * N
                       + static_cast<size_t>(L - 1)
                           * (static_cast<size_t>(N) * N
                              - (hasHole ? static_cast<size_t>(holeQuads) * holeQuads : 0));
    m.positions.reserve(verts * 3);
    m.normals.reserve(verts * 3);
    m.uvs.reserve(verts * 2);
    m.tangents.reserve(verts * 4);
    m.indices.reserve(quads * 6);

    const int holeLo = N / 4 + kHoleInset;
    const int holeHi = holeLo + holeQuads;   // quad indices [holeLo, holeHi) omitted

    for (int l = 0; l < L; ++l) {
        const float cell = cfg_.cellSize * std::exp2(static_cast<float>(l));
        const uint32_t base = static_cast<uint32_t>(m.positions.size() / 3);

        for (int j = 0; j <= N; ++j) {
            for (int i = 0; i <= N; ++i) {
                // Offset in metres from this level's centre. aPos.y = 0 — the
                // vertex shader supplies every height.
                m.positions.push_back((i - N / 2) * cell);
                m.positions.push_back(0.0f);
                m.positions.push_back((j - N / 2) * cell);
                m.normals.insert(m.normals.end(), {0.0f, 1.0f, 0.0f});
                // The level index rides in aUV.x; the vertex chunk reads it to
                // derive this level's cell size and snap grid.
                m.uvs.push_back(static_cast<float>(l));
                // aUV.y is the morph parity — which axes this vertex is OFF
                // the next coarser level's grid on. Level l snaps its centre
                // to a 2*c_l grid and its vertices sit at centre + (i - N/2)*
                // c_l with N/2 even, so the vertex lands on the coarser
                // level's grid exactly when the index is even. The coarsest
                // level has no coarser neighbour, so it never morphs.
                const int parity = (l + 1 < L)
                                 ? ((i & 1) | ((j & 1) << 1))
                                 : 0;
                m.uvs.push_back(static_cast<float>(parity));
                // Supplied rather than generated: MeshNode::setMesh runs
                // bromesh::generateTangents on any mesh with UVs+normals and
                // none, which on a quarter-million triangles is pure waste for
                // a shader that never samples a normal map.
                m.tangents.insert(m.tangents.end(), {1.0f, 0.0f, 0.0f, 1.0f});
            }
        }

        for (int j = 0; j < N; ++j) {
            for (int i = 0; i < N; ++i) {
                if (l > 0 && hasHole &&
                    i >= holeLo && i < holeHi && j >= holeLo && j < holeHi)
                    continue;
                const uint32_t v00 = base + static_cast<uint32_t>(j * row + i);
                const uint32_t v10 = v00 + 1;
                const uint32_t v01 = v00 + static_cast<uint32_t>(row);
                const uint32_t v11 = v01 + 1;
                // CCW seen from +Y (the surface faces up).
                m.indices.insert(m.indices.end(), {v00, v01, v10});
                m.indices.insert(m.indices.end(), {v10, v01, v11});
            }
        }
    }

    vertexCount_   = static_cast<int>(m.vertexCount());
    triangleCount_ = static_cast<int>(m.triangleCount());
    node_->setMesh(std::move(m));
}

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------

void ClipmapTerrain::pushStaticUniforms() {
    if (!node_) return;
    auto set = [&](const char* n, std::initializer_list<float> v) {
        node_->setCustomShaderUniform(n, static_cast<int>(v.size()), v.begin());
    };
    // Both are re-pushed every update once altitude is known; these are the
    // unzoomed values, so a terrain that is never updated still draws.
    set("u_cellSize", {cfg_.cellSize});
    set("u_cellScale", {1.0f});
    // K = N/4 — see the derivation in clipmap.vert.glsl.
    set("u_invK", {4.0f / static_cast<float>(cfg_.resolution)});
    set("u_heightScale", {cfg_.heightScale});
    set("u_seaLevel", {cfg_.seaLevel});
    set("u_snowLine", {cfg_.snowLine});
    set("u_detailWavelength", {cfg_.detailWavelength});
    set("u_detailRelief", {cfg_.detailRelief});
    set("u_detailGain", {cfg_.detailGain});
    set("u_detailOctaves", {static_cast<float>(cfg_.detailOctaves)});
    set("u_planetRadius", {cfg_.planetRadius});
}

void ClipmapTerrain::pushLayerUniforms() {
    if (!node_) return;
    float wrap[kMaxLayers] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < kMaxLayers; ++i) {
        const ClipmapLayer& l = layers_[i];
        const float a[3] = {l.originX, l.originZ,
                            l.metresPerCell > 0.0f ? l.metresPerCell : 1.0f};
        const float b[2] = {l.present ? static_cast<float>(l.width) : 0.0f,
                            l.present ? static_cast<float>(l.height) : 0.0f};
        node_->setCustomShaderUniform(kLayerA[i], 3, a);
        node_->setCustomShaderUniform(kLayerB[i], 2, b);
        wrap[i] = (l.present && l.wrapX) ? 1.0f : 0.0f;
    }
    node_->setCustomShaderUniform("u_lWrapX", 4, wrap);
    const float n = static_cast<float>(layerCount_);
    node_->setCustomShaderUniform("u_layerCount", 1, &n);

    const float lam = exemplarLambda();
    node_->setCustomShaderUniform("u_exLambda", 1, &lam);
}

// The exemplar's coarse repeat: twice the coarsest layer's cell, so the patch
// starts just under what the pyramid can represent. A property of the LAYER
// STACK, not of position — see cmExemplar for why it must not vary across the
// world. Falls back to the ring extent when no layer is installed yet.
float ClipmapTerrain::exemplarLambda() const {
    for (int i = kMaxLayers - 1; i >= 0; --i)
        if (layers_[i].present && layers_[i].metresPerCell > 0.0f)
            return 2.0f * layers_[i].metresPerCell;
    return 2.0f * cfg_.cellSize * std::exp2(static_cast<float>(cfg_.levels));
}

void ClipmapTerrain::recomputeHeightRange() {
    bool any = false;
    float lo = 0.0f, hi = 0.0f;
    for (const auto& l : layers_) {
        if (!l.present || l.data.empty()) continue;
        auto [mn, mx] = std::minmax_element(l.data.begin(), l.data.end());
        if (!any) { lo = *mn; hi = *mx; any = true; }
        else { lo = std::min(lo, *mn); hi = std::max(hi, *mx); }
    }
    if (!any) { lo = hi = 0.0f; }
    const float a = cfg_.seaLevel + cfg_.heightScale * lo;
    const float b = cfg_.seaLevel + cfg_.heightScale * hi;
    minHeight_ = std::min(a, b);
    maxHeight_ = std::max(a, b);
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

void ClipmapTerrain::setHeightLayer(int index, const float* data, int width,
                                    int height, float originX, float originZ,
                                    float metresPerCell, bool wrapX) {
    if (!node_ || index < 0 || index >= kMaxLayers) return;
    ClipmapLayer& l = layers_[index];

    if (!data || width <= 0 || height <= 0) {
        l = ClipmapLayer{};
        // Keep the sampler bound (see the constructor) but drop the pixels.
        const float zero = 0.0f;
        node_->setCustomShaderTexture(kLayerTex[index], 1, 1, &zero, true);
    } else {
        l.data.assign(data, data + static_cast<size_t>(width) * height);
        l.width = width;
        l.height = height;
        l.originX = originX;
        l.originZ = originZ;
        l.metresPerCell = metresPerCell > 0.0f ? metresPerCell : 1.0f;
        l.wrapX = wrapX;
        l.present = true;
        // mipmap: true is load-bearing, not an optimisation. The shader samples
        // at a FRACTIONAL textureLod level; without a chain GL clamps every lod
        // to 0 and the whole distance-continuous filtering story collapses.
        // repeat in S, clamp in T: longitude is periodic, latitude is not.
        node_->setCustomShaderTexture(kLayerTex[index], width, height,
                                      l.data.data(), true, wrapX, wrapX);
    }

    layerCount_ = 0;
    for (int i = 0; i < kMaxLayers; ++i)
        if (layers_[i].present) layerCount_ = i + 1;

    recomputeHeightRange();
    pushLayerUniforms();
}

// ---------------------------------------------------------------------------
// Detail exemplar
// ---------------------------------------------------------------------------

namespace {

// Separable box blur, run twice — close enough to a Gaussian for a high-pass
// whose only job is to strip landform-scale content off the patch.
void boxBlurWrap(std::vector<float>& v, int n, int radius) {
    if (radius < 1) return;
    std::vector<float> tmp(v.size());
    const float inv = 1.0f / static_cast<float>(2 * radius + 1);
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < n; ++y) {          // horizontal
            for (int x = 0; x < n; ++x) {
                float s = 0.0f;
                for (int k = -radius; k <= radius; ++k)
                    s += v[static_cast<size_t>(y) * n + ((x + k) % n + n) % n];
                tmp[static_cast<size_t>(y) * n + x] = s * inv;
            }
        }
        for (int x = 0; x < n; ++x) {          // vertical
            for (int y = 0; y < n; ++y) {
                float s = 0.0f;
                for (int k = -radius; k <= radius; ++k)
                    s += tmp[static_cast<size_t>(((y + k) % n + n) % n) * n + x];
                v[static_cast<size_t>(y) * n + x] = s * inv;
            }
        }
    }
}

} // namespace

void ClipmapTerrain::setDetailExemplar(const float* data, int width, int height,
                                       float metresPerCell) {
    if (!node_) return;
    if (!data || width <= 8 || height <= 8 || metresPerCell <= 0.0f) {
        exemplar_.clear();
        exemplarN_ = 0;
        const float zero = 0.0f;
        node_->setCustomShaderTexture("u_exemplar", 1, 1, &zero, true);
        const float present = 0.0f;
        node_->setCustomShaderUniform("u_exPresent", 1, &present);
        return;
    }

    // Square, power-of-two and bounded: the shader indexes it as one repeat, so
    // a non-square patch would stretch the structure along one axis, and the
    // mip chain wants a clean reduction. 1024 texels over a 61 km tile is 60 m
    // per texel, already finer than the 30 m data it came from can support once
    // the low frequencies are gone.
    const int src = std::min(width, height);
    int n = 1;
    while (n * 2 <= std::min(src, 1024)) n *= 2;

    // Crop a band before resampling: the wrap blend below consumes it.
    const int band = n / 8;
    const int keep = n - band;

    std::vector<float> e(static_cast<size_t>(n) * n);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const int sx = std::min(width - 1, x * src / n);
            const int sy = std::min(height - 1, y * src / n);
            e[static_cast<size_t>(y) * n + x] =
                data[static_cast<size_t>(sy) * width + sx];
        }
    }

    // MAKE IT PERIODIC FIRST. The order matters and it used to be the other way.
    //
    // The high-pass below blurs with a WRAPPING kernel, which is only meaningful
    // on a patch that already wraps. Run against the raw crop it averaged the
    // tile's left edge with its unrelated right edge, so the low-frequency
    // estimate went wrong exactly at the edges and subtracting it left a ridge
    // of ~7x the patch's own energy along both. The tap repeats that ridge every
    // lambda along a fixed rotation, which is what drew long streaks across
    // every continent. The periodic blend then folded those same corrupted
    // columns back into the patch, so the artifact was fed its own output.
    //
    // Cross-fade the last `band` columns/rows over the first and crop to `keep`:
    // out(0) is then literally the sample that follows out(keep-1) in the
    // source, so the wrap is continuous by construction rather than by hoping
    // the edges match. Plain smoothstep weights here, on RAW elevation — the two
    // sides sit at different heights and the ramp between them is low-frequency
    // content the high-pass is about to remove anyway.
    std::vector<float> p(static_cast<size_t>(keep) * keep);
    {
        auto at = [&](int x, int y) { return e[static_cast<size_t>(y) * n + x]; };
        auto ease = [](float t) { return t * t * (3.0f - 2.0f * t); };
        std::vector<float> q(static_cast<size_t>(keep) * n);   // X folded, all rows
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < keep; ++x)
                q[static_cast<size_t>(y) * keep + x] =
                    (x < band)
                        ? at(x + keep, y) * (1.0f - ease(float(x) / band))
                          + at(x, y) * ease(float(x) / band)
                        : at(x, y);
        for (int y = 0; y < keep; ++y)
            for (int x = 0; x < keep; ++x) {
                const float here = q[static_cast<size_t>(y) * keep + x];
                p[static_cast<size_t>(y) * keep + x] =
                    (y < band)
                        ? q[static_cast<size_t>(y + keep) * keep + x]
                              * (1.0f - ease(float(y) / band))
                          + here * ease(float(y) / band)
                        : here;
            }
    }

    // HIGH-PASS. The patch must supply detail, never landforms: its own valley
    // and range scales are already in the height pyramid, and adding them back
    // would fight the model's terrain rather than extend it.
    //
    // The radius sets the cutoff and has to agree with what the SHADER assumes
    // this patch contains. cmExRedundancy weighs each tap as though its content
    // sits at lambda/8; two box passes at radius r cut at ~4r texels, so
    // keep/32 puts the cutoff at keep/8 and makes that true.
    //
    // It was keep/8, cutting at lambda/2. With lambda = 2x the coarsest cell
    // that left the patch carrying structure at the DATA FLOOR itself — landform
    // scale, from a tile repeating every 15.36 km. The redundancy test could not
    // suppress it, because that test asks about lambda/8 while the visible
    // lattice is at lambda. From 28 km up it read as a regular grid of identical
    // ridges over every continent.
    {
        std::vector<float> lo = p;
        boxBlurWrap(lo, keep, std::max(1, keep / 32));
        for (size_t i = 0; i < p.size(); ++i) p[i] -= lo[i];
    }

    // MAKE IT STATIONARY.
    //
    // The patch is ONE 61 km tile of real terrain, and real terrain is not:
    // this one runs from a smooth plain at one end to broken mountains at the
    // other. The high-pass takes out low-frequency ELEVATION and leaves
    // low-frequency AMPLITUDE untouched, so tiling the result paints the
    // source's own roughness map across the world every lambda.
    //
    // Dividing by a smoothed local RMS keeps the structure while flattening the
    // envelope: ridge and drainage character lives in the SHAPE, not in how loud
    // the tile happens to be at that spot. Measured over the patch's column RMS
    // this takes the spread from 5.2x to 3.2x. It does not reach 1.0x and should
    // not — the remainder is variation at 1-3 km, which IS the structure, and
    // flattening that would leave uniform noise instead of terrain.
    //
    // The floor is a rail rather than the mechanism (it does not currently
    // bind): it stops a genuinely flat reach from being amplified to full
    // relief if a future exemplar contains one. Where ground should be smooth
    // versus broken is the DATA's business — it follows the coarse field's
    // slope, and it does not repeat.
    {
        std::vector<float> amp(p.size());
        for (size_t i = 0; i < p.size(); ++i) amp[i] = p[i] * p[i];
        boxBlurWrap(amp, keep, std::max(1, keep / 8));
        double acc = 0.0;
        for (size_t i = 0; i < amp.size(); ++i) {
            amp[i] = std::sqrt(std::max(0.0f, amp[i]));
            acc += amp[i];
        }
        const float mean = static_cast<float>(acc / std::max<size_t>(1, amp.size()));
        if (mean > 0.0f) {
            const float floorAmp = 0.35f * mean;
            for (size_t i = 0; i < p.size(); ++i)
                p[i] *= mean / std::max(amp[i], floorAmp);
        }
    }

    // NORMALISE BY FOOTPRINT. Relief per unit length is dimensionless, so the
    // patch applied at any wavelength carries the aspect ratio the model
    // produced at that scale — no tuned amplitude constant anywhere.
    {
        const float footprint = static_cast<float>(src) * metresPerCell;
        const float inv = footprint > 0.0f ? 1.0f / footprint : 0.0f;
        for (float& v : p) v *= inv;
    }

    exemplar_ = std::move(p);
    exemplarN_ = keep;
    node_->setCustomShaderTexture("u_exemplar", keep, keep, exemplar_.data(),
                                  true, /*repeat=*/true);
    const float nTexels = static_cast<float>(keep);
    node_->setCustomShaderUniform("u_exN", 1, &nTexels);
    const float present = 1.0f;
    node_->setCustomShaderUniform("u_exPresent", 1, &present);
    recomputeHeightRange();
}

float ClipmapTerrain::exemplarAt(float ux, float uz) const {
    if (exemplarN_ <= 0) return 0.0f;
    const int   n = exemplarN_;
    const float fx = (ux - std::floor(ux)) * static_cast<float>(n);
    const float fz = (uz - std::floor(uz)) * static_cast<float>(n);
    const int   x0 = static_cast<int>(fx), z0 = static_cast<int>(fz);
    const float tx = fx - static_cast<float>(x0), tz = fz - static_cast<float>(z0);
    auto at = [&](int x, int z) {
        return exemplar_[static_cast<size_t>(((z % n) + n) % n) * n
                       + static_cast<size_t>(((x % n) + n) % n)];
    };
    return (at(x0, z0) * (1 - tx) + at(x0 + 1, z0) * tx) * (1 - tz)
         + (at(x0, z0 + 1) * (1 - tx) + at(x0 + 1, z0 + 1) * tx) * tz;
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------

void ClipmapTerrain::update(float camX, float camY, float camZ) {
    if (!node_) return;

    // Parking the node on the eye is what makes the renderer's camera-relative
    // subtraction leave the model matrix at identity, so the camera-relative
    // position the vertex chunk writes passes through untouched.
    node_->setPosition(camX, camY, camZ);

    const float camXZ[2] = {camX, camZ};
    node_->setCustomShaderUniform("u_camXZ", 2, camXZ);

    // Anchor the detail lattice near the camera. Snapping in double and
    // handing the shader both the anchor and the camera's small offset from it
    // is the whole trick: the shader never has to form a large noise
    // coordinate, and because the anchor step is an exact multiple of every
    // octave wavelength, an anchor jump cancels exactly against the offset and
    // the field does not shift.
    const double step = static_cast<double>(detailAnchorStep());
    const double ax = std::floor(static_cast<double>(camX) / step) * step;
    const double az = std::floor(static_cast<double>(camZ) / step) * step;
    const float anchor[2] = {static_cast<float>(ax), static_cast<float>(az)};
    const float offset[2] = {static_cast<float>(camX - ax),
                             static_cast<float>(camZ - az)};
    node_->setCustomShaderUniform("u_detailAnchor", 2, anchor);
    node_->setCustomShaderUniform("u_detailOffset", 2, offset);

    // Height above the ground, not above sea level — see cmCellSize. The base
    // stack is enough: detail is metres against an altitude term that only
    // matters at hundreds.
    const float groundY = baseElevationAt(camX, camZ);
    node_->setCustomShaderUniform("u_camGroundY", 1, &groundY);
    node_->setCustomShaderUniform("u_camY", 1, &camY);

    // World size of one pixel per unit of distance: 2*tan(fovY/2)/height. The
    // shader uses it to stop sampling the height field finer than the
    // framebuffer can resolve, which is a screen-space question and therefore
    // depends on FOV and viewport — neither of which is constant, so this is
    // pushed per frame rather than baked with the static uniforms.
    const int   vh    = graph_.canvasHeight();
    const float fovY  = graph_.cameraFovY();
    const float scale = (vh > 0 && fovY > 0.0f)
        ? 2.0f * std::tan(fovY * 0.5f) / static_cast<float>(vh)
        : 0.0f;   // 0 disables the term; the geometry limit still applies
    node_->setCustomShaderUniform("u_pixelScale", 1, &scale);

    // ZOOM THE STACK WITH ALTITUDE — why the world stops having an edge.
    //
    // The ring stack's reach is c0 * (N/2) * 2^(L-1), a CONSTANT. At 8 m cells
    // that is 524 km, which is a horizon from the ground and a plate of land
    // floating in the sky from 280 km up. Adding levels would fix the reach and
    // cost triangles forever; instead, re-read the same rings at a coarser c0.
    //
    // The choice is not a tuning knob — there is exactly one scale that costs
    // nothing. cmCellSize returns max(cGeo, cAA), and cGeo's floor is c0. So
    // raising c0 changes NOTHING anywhere cAA already exceeds it, and cAA is
    // smallest directly under the eye, where it is
    //     aa = |camY - groundY| * pixelScale * CM_PIXELS_PER_CELL.
    // Take the largest power of two with c0*2^k <= aa and the inequality holds
    // at every point in the stack (cAA only grows with distance). The rendered
    // surface is therefore IDENTICAL, and the extent is 2^k times larger: the
    // rings we drop were drawing sub-pixel triangles under the camera.
    //
    // From 280 km up, aa is ~540 m against an 8 m cell — six levels of the
    // stack were being spent on detail finer than the framebuffer could show.
    //
    // Power-of-two steps keep every level's snap grid nested (buildGeometry
    // relies on it) and keep the mip pyramid aligned.
    //
    // THE HYSTERESIS IS LOAD-BEARING, not polish. A step changes farDistance,
    // and an app that sizes its height data from farDistance answers by
    // re-cutting and re-uploading a layer — megabytes, plus mipmap generation.
    // So a step has to be RARE, and the thing being compared is noisy: aa is
    // driven by height above the TERRAIN, and terrain height under a camera
    // crossing the world at 13 km/s swings hundreds of metres per frame. With
    // the band at 10% that noise walked back and forth across a threshold and
    // re-uploaded the layer every frame, which reads as a hard lock.
    //
    // Stepping up at 2.5x and down at 0.8x leaves a 1.56x band between them,
    // far wider than the ground can jitter in a frame. The cost of the wider
    // band is only that the stack sometimes carries a finer c0 than it strictly
    // needs — that is, less reach than is free, never a visible change.
    // AND NEVER REACH PAST THE HORIZON. On a planet the zoom has a second upper
    // bound that has nothing to do with pixels: ground beyond sqrt(2Rh+h^2) is
    // behind the curve, so rings that extend past it draw geometry that bends
    // below the eye ray and is never seen — while still obliging the app to
    // generate height data across the whole footprint. That was the actual cost
    // driver: from the deck, reach was 524 km against a 5 km horizon, a factor
    // of a hundred in radius and ten thousand in area, all of it invisible.
    //
    // The bound is horizon(eye) + horizon(highest ground) — see
    // coverageDistance, which is the same rule and the number the app sizes its
    // data from. Height above SEA LEVEL: a camera on a summit sees much further
    // than one on a plain, and measuring from the ground underfoot cannot tell
    // them apart.
    //
    // It enters as a SECOND TARGET, not as a clamp after the fact. Both targets
    // are noisy — the pixel one through the ground underfoot, the horizon one
    // through maxHeight_, which is recomputed on every setHeightLayer — so a
    // clamp applied downstream of the band would be a threshold with no
    // hysteresis at all, and would close exactly the loop the band exists to
    // open: sizing a layer from farDistance moves maxHeight_, which moves the
    // cap, which moves farDistance, which re-uploads the layer. Taking the min
    // of the two targets and running the result through the one band means
    // every step, from either cause, has to clear 1.56x to fire.
    //
    // The cost is that cellScale_ may sit up to 2.5x past the horizon bound for
    // the frames before a step fires, i.e. some invisible geometry and a wider
    // data request. That is the same trade the band already makes in the other
    // direction, and it is bounded and transient where the re-upload loop was
    // neither.
    constexpr float kPixelsPerCell = 1.5f;   // == CM_PIXELS_PER_CELL
    if (scale > 0.0f) {
        const float aa = std::abs(camY - groundY) * scale * kPixelsPerCell;
        float want = aa / cfg_.cellSize;
        if (cfg_.planetRadius > 0.0f) {
            const float peak  = std::max(maxHeight_ - cfg_.seaLevel, 0.0f);
            const float reach = horizonDistance(camY - cfg_.seaLevel)
                              + horizonDistance(peak);
            const float unit  = cfg_.cellSize * static_cast<float>(cfg_.resolution / 2)
                              * std::exp2(static_cast<float>(cfg_.levels - 1));
            if (unit > 0.0f) want = std::min(want, reach / unit);
        }
        if (want >= cellScale_ * 2.5f)        cellScale_ *= 2.0f;
        else if (want < cellScale_ * 0.8f)    cellScale_ *= 0.5f;
        cellScale_ = std::clamp(cellScale_, 1.0f, std::max(1.0f, cfg_.maxCellScale));
    }
    const float c0 = cfg_.cellSize * cellScale_;
    node_->setCustomShaderUniform("u_cellSize", 1, &c0);
    node_->setCustomShaderUniform("u_cellScale", 1, &cellScale_);

    // Cull margin, not "disable culling". Frustum culling cannot see GLSL
    // displacement, and this node's baked AABB is a flat sheet at y = 0 in an
    // object space parked at the eye — everything the shader emits is outside
    // it. Two sources of slop, both bounded and both known here:
    //   * vertical: the geometry spans [minHeight, maxHeight] in WORLD metres,
    //     which the shader re-expresses relative to camY. So the margin has to
    //     track camera altitude — a fixed heightScale would cull the ground
    //     away the moment the camera climbed above it.
    //   * horizontal: each level snaps its centre to a 2*c_l grid, displacing
    //     it by up to 2*c_(L-1) = cellSize * 2^L beyond the baked extent.
    // Keeping culling ON matters: the node is one of the largest in any scene
    // that has it, and it should still drop out when the camera looks away.
    const float vertical = std::max(std::abs(maxHeight_ - camY),
                                    std::abs(minHeight_ - camY));
    const float snapSlop = c0 * std::exp2(static_cast<float>(cfg_.levels));
    // Detail rides on top of the layer range, so it widens the vertical span.
    // Octave i contributes detailRelief * lambda0 * (gain/2)^i at most, and the
    // slope modulator only scales that down, so the series bounds it.
    const float detail = detailBound();
    node_->setCullMargin(std::max(vertical + detail, snapSlop));
}

// ---------------------------------------------------------------------------
// CPU height query — must agree with the GPU or things fall through the floor
// ---------------------------------------------------------------------------

float ClipmapTerrain::baseElevationAt(float x, float z) const {
    // Mirrors cmHeight() in the shaders exactly, except that it always samples
    // mip level 0 (bilinear) — there is no CPU mip chain. Same layer order,
    // same coverage weights, same coarse-to-fine blend.
    auto sample = [&](const ClipmapLayer& l, float& w) -> float {
        if (!l.present || l.width < 1 || l.height < 1 || l.data.empty()) {
            w = 0.0f;
            return 0.0f;
        }
        const float tx = (x - l.originX) / l.metresPerCell;
        const float tz = (z - l.originZ) / l.metresPerCell;
        const float ux = (tx + 0.5f) / static_cast<float>(l.width);
        const float uz = (tz + 0.5f) / static_cast<float>(l.height);
        // cmEdge: a periodic layer has no east-west edge to be near.
        w = l.wrapX
            ? smoothstep01(kFade, std::min(uz, 1.0f - uz))
            : smoothstep01(kFade, std::min(std::min(ux, 1.0f - ux),
                                           std::min(uz, 1.0f - uz)));

        // GL_LINEAR at level 0, in texel space: GL_REPEAT in S when the layer
        // wraps, GL_CLAMP_TO_EDGE otherwise, and always clamped in T.
        const int x0 = static_cast<int>(std::floor(tx));
        const int z0 = static_cast<int>(std::floor(tz));
        const float fx = tx - static_cast<float>(x0);
        const float fz = tz - static_cast<float>(z0);
        auto at = [&](int ix, int iz) -> float {
            ix = l.wrapX ? ((ix % l.width) + l.width) % l.width
                         : std::clamp(ix, 0, l.width - 1);
            iz = std::clamp(iz, 0, l.height - 1);
            return l.data[static_cast<size_t>(iz) * l.width + ix];
        };
        const float h00 = at(x0, z0),     h10 = at(x0 + 1, z0);
        const float h01 = at(x0, z0 + 1), h11 = at(x0 + 1, z0 + 1);
        return (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fz)
             + (h01 * (1.0f - fx) + h11 * fx) * fz;
    };

    const int n = layerCount_;
    float w = 0.0f;
    float h = 0.0f;
    if      (n > 3) h = sample(layers_[3], w);
    else if (n > 2) h = sample(layers_[2], w);
    else if (n > 1) h = sample(layers_[1], w);
    else if (n > 0) h = sample(layers_[0], w);
    if (n > 3) { float s = sample(layers_[2], w); h = h + (s - h) * w; }
    if (n > 2) { float s = sample(layers_[1], w); h = h + (s - h) * w; }
    if (n > 1) { float s = sample(layers_[0], w); h = h + (s - h) * w; }

    return cfg_.seaLevel + cfg_.heightScale * h;
}

// Mirrors cmDataFloor() — the finest cell the DATA resolves here, blended in
// log2 with the same coverage weights baseElevationAt uses. It sets where the
// procedural band starts, so it has to agree with the shader or the surface a
// query reports stops being the surface the GPU draws.
float ClipmapTerrain::dataFloorAt(float x, float z) const {
    auto cover = [&](const ClipmapLayer& l) -> float {
        if (!l.present || l.width < 1 || l.height < 1 || l.data.empty()) return 0.0f;
        const float ux = ((x - l.originX) / l.metresPerCell + 0.5f)
                       / static_cast<float>(l.width);
        const float uz = ((z - l.originZ) / l.metresPerCell + 0.5f)
                       / static_cast<float>(l.height);
        if (l.wrapX) return smoothstep01(kFade, std::min(uz, 1.0f - uz));
        return smoothstep01(kFade, std::min(std::min(ux, 1.0f - ux),
                                            std::min(uz, 1.0f - uz)));
    };
    const int n = layerCount_;
    float f = 0.0f;
    if      (n > 3) f = std::log2(layers_[3].metresPerCell);
    else if (n > 2) f = std::log2(layers_[2].metresPerCell);
    else if (n > 1) f = std::log2(layers_[1].metresPerCell);
    else if (n > 0) f = std::log2(layers_[0].metresPerCell);
    else return cfg_.cellSize;
    if (n > 3) { float w = cover(layers_[2]); f += (std::log2(layers_[2].metresPerCell) - f) * w; }
    if (n > 2) { float w = cover(layers_[1]); f += (std::log2(layers_[1].metresPerCell) - f) * w; }
    if (n > 1) { float w = cover(layers_[0]); f += (std::log2(layers_[0].metresPerCell) - f) * w; }
    return std::exp2(f);
}

// ---------------------------------------------------------------------------
// CPU mirror of the procedural detail.
//
// Mirrors cmDetail() in clipmap_detail.glsl — same hash, same quintic fade,
// same octave amplitudes — with two deliberate differences:
//
//   * No band limit. The shader fades octaves out against the rendered cell
//     size, a screen-space quantity this query has no business knowing. A
//     collision query wants the surface as it exists, so every octave counts.
//     Near the camera the two agree anyway: that is where the rendered cell is
//     smallest and every octave is at full strength, and near the camera is the
//     only place anything collides.
//   * Doubles for the lattice coordinate, which makes the shader's anchoring
//     trick unnecessary.
// ---------------------------------------------------------------------------

namespace {

uint32_t detailHash(int32_t cx, int32_t cz) {
    uint32_t h = static_cast<uint32_t>(cx + 0x2000000) * 0x8da6b343u
               + static_cast<uint32_t>(cz + 0x2000000) * 0xd8163841u;
    h ^= h >> 15; h *= 0x2c1b3c6du;
    h ^= h >> 12; h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

void detailGradient(int32_t cx, int32_t cz, float& gx, float& gz) {
    const float a = static_cast<float>(detailHash(cx, cz) & 0xffffu)
                  * (6.2831853f / 65536.0f);
    gx = std::cos(a);
    gz = std::sin(a);
}

float detailNoise(double px, double pz) {
    const double flx = std::floor(px), flz = std::floor(pz);
    const auto ix = static_cast<int32_t>(flx);
    const auto iz = static_cast<int32_t>(flz);
    const float fx = static_cast<float>(px - flx);
    const float fz = static_cast<float>(pz - flz);

    const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float uz = fz * fz * fz * (fz * (fz * 6.0f - 15.0f) + 10.0f);

    auto corner = [&](int dx, int dz) {
        float gx, gz;
        detailGradient(ix + dx, iz + dz, gx, gz);
        return gx * (fx - static_cast<float>(dx))
             + gz * (fz - static_cast<float>(dz));
    };
    const float va = corner(0, 0), vb = corner(1, 0);
    const float vc = corner(0, 1), vd = corner(1, 1);

    return va + (vb - va) * ux + (vc - va) * uz
              + (va - vb - vc + vd) * ux * uz;
}

} // namespace

float ClipmapTerrain::elevationAt(float x, float z) const {
    const float h0 = baseElevationAt(x, z);

    const float e  = cfg_.cellSize;
    const float hx = baseElevationAt(x + e, z);
    const float hz = baseElevationAt(x, z + e);

    // cmSlopeFrom, verbatim: normalize(h0-hx, e, h0-hz).y, then 1 - that.
    const float dx = h0 - hx, dz = h0 - hz;
    const float len = std::sqrt(dx * dx + e * e + dz * dz);
    const float slope = std::clamp(1.0f - e / std::max(len, 1e-6f), 0.0f, 1.0f);
    const float weight = std::max(slope, 0.12f);   // cmDetailWeight

    // The band starts above cfg_.detailWavelength wherever the data is coarser
    // than it — see cmDetail. The high-pass against the data floor is mirrored;
    // the shader's low-pass against the rendered cell still is not, for the
    // reason above.
    //
    // THE UPWARD OCTAVES STAND DOWN WITH AN EXEMPLAR LOADED, exactly as
    // cmDetail does: they exist only to fill the gap between the data floor and
    // the base wavelength, and an exemplar fills that gap with real landforms.
    // Without this gate the two disagree wherever the floor is coarse — i.e.
    // outside the finest layer's window, where wDat passes every upward octave
    // on this side and none on the shader's — and elevationAt is what a caller
    // stands the camera and its collision on. It is the drawn surface or it is
    // nothing.
    const float floorM = dataFloorAt(x, z);
    const int   up     = (exemplarN_ > 0) ? 0 : kDetailUpOctaves;
    double lambda = static_cast<double>(cfg_.detailWavelength)
                  * std::exp2(static_cast<double>(up));
    float  sum    = 0.0f;
    const int n = up + std::min(cfg_.detailOctaves, 8);
    for (int i = 0; i < n; ++i) {
        const float wDat = 1.0f - smoothstep01(2.0f * floorM,
                                               static_cast<float>(lambda) - 2.0f * floorM);
        if (wDat > 0.0f) {
            const float gain = std::pow(cfg_.detailGain, std::max(0, i - up));
            const float amp = cfg_.detailRelief * static_cast<float>(lambda) * gain;
            sum += amp * wDat * detailNoise(x / lambda, z / lambda);
        }
        lambda *= 0.5;
    }

    // The exemplar, mirroring cmExemplar: same two taps, same rotations, same
    // ratio, at level 0 — the shader's lod only coarsens with distance and
    // anything that collides is near the camera, where it reads level 0 too.
    // Must match CM_EX_* in clipmap_detail.glsl.
    float ex = 0.0f;
    if (exemplarN_ > 0) {
        constexpr float kRatio = 13.7f, kRotA = 0.31f, kRotB = 2.24f, kFineW = 0.55f;
        const float la = exemplarLambda(), lb = la / kRatio;
        // cmExRedundancy, verbatim.
        auto redundancy = [&](float lambda) {
            return 1.0f - smoothstep01(6.0f * floorM,
                                       lambda * 0.125f - 2.0f * floorM);
        };
        auto tap = [&](float l, float rot) {
            const float cs = std::cos(rot), sn = std::sin(rot);
            return l * exemplarAt((cs * x - sn * z) / l, (sn * x + cs * z) / l);
        };
        const float wa = redundancy(la), wb = redundancy(lb) * kFineW;
        if (wa > 0.0f) ex += wa * tap(la, kRotA);
        if (wb > 0.0f) ex += wb * tap(lb, kRotB);
    }
    return h0 + weight * sum + ex;
}

float ClipmapTerrain::detailBound() const {
    // Worst case over the whole world, so it assumes the band has climbed as
    // far as it can: the upward octaves are only live where the data is coarse,
    // but a cull margin has to hold everywhere.
    float lambda = cfg_.detailWavelength * std::exp2(float(kDetailUpOctaves));
    float sum    = 0.0f;
    const int n = kDetailUpOctaves + std::min(cfg_.detailOctaves, 8);
    for (int i = 0; i < n; ++i) {
        sum += cfg_.detailRelief * lambda
             * std::pow(cfg_.detailGain, std::max(0, i - kDetailUpOctaves));
        lambda *= 0.5f;
    }
    return sum;
}

} // namespace bro::scene
