#include "scene/clipmap_terrain.h"

#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "util/log.h"

#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>

#include "clipmap_common.glsl.h" // kClipmapCommonSrc
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
    const std::string common(kClipmapCommonSrc);
    node_->setCustomShader(common + kClipmapVertSrc, common + kClipmapFragSrc);
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
                m.uvs.push_back(0.0f);
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
    set("u_cellSize", {cfg_.cellSize});
    // K = N/4 — see the derivation in clipmap.vert.glsl.
    set("u_invK", {4.0f / static_cast<float>(cfg_.resolution)});
    set("u_heightScale", {cfg_.heightScale});
    set("u_seaLevel", {cfg_.seaLevel});
}

void ClipmapTerrain::pushLayerUniforms() {
    if (!node_) return;
    for (int i = 0; i < kMaxLayers; ++i) {
        const ClipmapLayer& l = layers_[i];
        const float a[3] = {l.originX, l.originZ,
                            l.metresPerCell > 0.0f ? l.metresPerCell : 1.0f};
        const float b[2] = {l.present ? static_cast<float>(l.width) : 0.0f,
                            l.present ? static_cast<float>(l.height) : 0.0f};
        node_->setCustomShaderUniform(kLayerA[i], 3, a);
        node_->setCustomShaderUniform(kLayerB[i], 2, b);
    }
    const float n = static_cast<float>(layerCount_);
    node_->setCustomShaderUniform("u_layerCount", 1, &n);
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
                                    float metresPerCell) {
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
        l.present = true;
        // mipmap: true is load-bearing, not an optimisation. The shader samples
        // at a FRACTIONAL textureLod level; without a chain GL clamps every lod
        // to 0 and the whole distance-continuous filtering story collapses.
        node_->setCustomShaderTexture(kLayerTex[index], width, height,
                                      l.data.data(), true);
    }

    layerCount_ = 0;
    for (int i = 0; i < kMaxLayers; ++i)
        if (layers_[i].present) layerCount_ = i + 1;

    recomputeHeightRange();
    pushLayerUniforms();
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
    const float snapSlop = cfg_.cellSize * std::exp2(static_cast<float>(cfg_.levels));
    node_->setCullMargin(std::max(vertical, snapSlop));
}

// ---------------------------------------------------------------------------
// CPU height query — must agree with the GPU or things fall through the floor
// ---------------------------------------------------------------------------

float ClipmapTerrain::elevationAt(float x, float z) const {
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
        w = smoothstep01(kFade, std::min(std::min(ux, 1.0f - ux),
                                         std::min(uz, 1.0f - uz)));

        // GL_LINEAR + GL_CLAMP_TO_EDGE at level 0, in texel space.
        const int x0 = static_cast<int>(std::floor(tx));
        const int z0 = static_cast<int>(std::floor(tz));
        const float fx = tx - static_cast<float>(x0);
        const float fz = tz - static_cast<float>(z0);
        auto at = [&](int ix, int iz) -> float {
            ix = std::clamp(ix, 0, l.width - 1);
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

} // namespace bro::scene
