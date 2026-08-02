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

// Surface (control-channel) layers. Layer 0 keeps the unnumbered names it has
// always had — `u_surface`, `u_surfA`, `u_surfB` — because shaders written
// against the single-layer API read them by name, and renaming for symmetry
// would break every one of them to no visual end. The stack extends upward from
// there.
const char* kSurfTex[ClipmapTerrain::kMaxLayers] =
    {"u_surface", "u_surface1", "u_surface2", "u_surface3"};
const char* kSurfA[ClipmapTerrain::kMaxLayers] =
    {"u_surfA", "u_surf1A", "u_surf2A", "u_surf3A"};
const char* kSurfB[ClipmapTerrain::kMaxLayers] =
    {"u_surfB", "u_surf1B", "u_surf2B", "u_surf3B"};

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

    const float zero3[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < kMaxLayers; ++i)
        node_->setCustomShaderTexture(kSurfTex[i], 1, 1, zero3, false, false, true, 3);

    pushStaticUniforms();
    pushLayerUniforms();
    pushSurfaceUniforms();
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

    // Material defaults
    set("u_albedoRock", {rockAlbedo_[0], rockAlbedo_[1], rockAlbedo_[2]});
    set("u_roughnessRock", {rockRoughness_});
    set("u_albedoSnow", {snowAlbedo_[0], snowAlbedo_[1], snowAlbedo_[2]});
    set("u_roughnessSnow", {snowRoughness_});
    set("u_albedoSand", {sandAlbedo_[0], sandAlbedo_[1], sandAlbedo_[2]});
    set("u_roughnessSand", {sandRoughness_});
    set("u_albedoGrass", {grassAlbedo_[0], grassAlbedo_[1], grassAlbedo_[2]});
    set("u_roughnessGrass", {grassRoughness_});
    set("u_albedoForest", {forestAlbedo_[0], forestAlbedo_[1], forestAlbedo_[2]});
    set("u_forestTint", {forestTint_});

    // Surface layer defaults
    set("u_surfA", {0.0f, 0.0f, 1.0f});
    set("u_surfB", {0.0f, 0.0f});
    set("u_surfPresent", {0.0f});
}

void ClipmapTerrain::pushLayerUniforms() {
    if (!node_) return;
    float wrap[kMaxLayers] = {0.0f, 0.0f, 0.0f, 0.0f};
    float band[kMaxLayers] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < kMaxLayers; ++i) {
        const ClipmapLayer& l = layers_[i];
        const float a[3] = {l.originX, l.originZ,
                            l.metresPerCell > 0.0f ? l.metresPerCell : 1.0f};
        const float b[2] = {l.present ? static_cast<float>(l.width) : 0.0f,
                            l.present ? static_cast<float>(l.height) : 0.0f};
        node_->setCustomShaderUniform(kLayerA[i], 3, a);
        node_->setCustomShaderUniform(kLayerB[i], 2, b);
        wrap[i] = (l.present && l.wrapX) ? 1.0f : 0.0f;
        // An absent slot declares nothing: cmDataFloor's blend starts from the
        // coarsest PRESENT layer, and a released slot must not leave a claim
        // about band limits behind for the next one to inherit.
        band[i] = (l.present && l.bandLimited) ? 1.0f : 0.0f;
    }
    node_->setCustomShaderUniform("u_lWrapX", 4, wrap);
    node_->setCustomShaderUniform("u_lBandLimited", 4, band);
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
                                    float metresPerCell, bool wrapX,
                                    bool bandLimited) {
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
        l.bandLimited = bandLimited;
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
    lastCamX_ = camX;
    lastCamZ_ = camZ;

    // The curvature chart centre: the camera ground point unless the app
    // pinned it (setChartCenter). Pushing the SAME floats as u_camXZ in the
    // default case is what keeps the shader's delta an exact 0.0 and the
    // default path bit-for-bit what it was.
    const float chartXZ[2] = {chartPinned_ ? chartX_ : camX,
                              chartPinned_ ? chartZ_ : camZ};
    node_->setCustomShaderUniform("u_chartXZ", 2, chartXZ);

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
    //
    // "Ground" means the RENDERED sheet, and under a pinned chart centre that
    // is not the flat field height. cmCurve maps the flat point at arc
    // distance d = R*th from the centre to chord rho = (R+h)*sin th and
    // height y = h*cos th - 2R*sin^2(th/2), so the sheet under a camera at
    // chord rho sits ~rho^2/2R BELOW the field — 12.6 km at rho = 400 km on
    // Earth radius. The shader's cmCellSize/cmCellSizeAA read
    //     dy = |u_camY - u_camGroundY|
    // as the eye-to-surface distance and floor the sampled cell at
    // dy * u_pixelScale * CM_PIXELS_PER_CELL; feeding the flat height makes
    // dy ~ rho^2/2R for an eye standing ON the sheet, an ~18 m minimum cell
    // (1080 px, 55 deg fov) that airbrushes the whole frame. So: invert the
    // chord mapping to the flat point actually drawn beneath the eye, and
    // bend its height exactly as cmCurve will. chartPointUnder is the
    // identity — same call, same bits — whenever no chart is pinned.
    float groundY;
    {
        float gx, gz, gth;
        if (chartPointUnder(camX, camZ, gx, gz, gth)) {
            const double h = baseElevationAt(gx, gz);
            const double s = std::sin(0.5 * static_cast<double>(gth));
            groundY = static_cast<float>(
                h * std::cos(static_cast<double>(gth))
                - 2.0 * static_cast<double>(cfg_.planetRadius) * s * s);
        } else {
            groundY = baseElevationAt(camX, camZ);
        }
    }
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
            // Eye height above SEA LEVEL for the horizon reach. Raw camY is
            // that height only while the datum is the y = 0 plane; under a
            // pinned chart the datum is cmCurve's sphere — radius R centred
            // at C = (chartX, -R, chartZ), since |cmCurve(rel, h) - C| =
            // R + h identically — and a camera hugging the bent sheet at
            // chord rho has camY ~ -rho^2/2R. horizonDistance's max(h, 0)
            // reads that as an eye at the planet's surface with zero height:
            // reach collapses to horizon(peak) alone and the stack is capped
            // tens of km out however high the camera actually flies. The real
            // altitude, whatever the chart, is |cam - C| - R. Double, not
            // float: R eats seven significant digits and the float
            // subtraction would quantise the altitude to half-metres.
            float eyeASL = camY - cfg_.seaLevel;
            if (chartPinned_) {
                const double R  = cfg_.planetRadius;
                const double dx = static_cast<double>(camX) - chartX_;
                const double dz = static_cast<double>(camZ) - chartZ_;
                const double dy = static_cast<double>(camY) + R;
                eyeASL = static_cast<float>(
                             std::sqrt(dx * dx + dz * dz + dy * dy) - R)
                       - cfg_.seaLevel;
            }
            const float peak  = std::max(maxHeight_ - cfg_.seaLevel, 0.0f);
            const float reach = horizonDistance(eyeASL)
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
    float vertical = std::max(std::abs(maxHeight_ - camY),
                              std::abs(minHeight_ - camY));
    // On a planet the sheet also SAGS below the flat heights: the datum at arc
    // distance d from the chart centre sits 2R*sin^2(d/2R) below y = 0 — 86 km
    // at a 1049 km reach. The farthest ground from the chart centre is the
    // stack's rim on the far side of the camera, so the bound is reach plus
    // the camera's own offset from the centre (0 unless the chart is pinned).
    if (cfg_.planetRadius > 0.0f) {
        const float dx = camX - (chartPinned_ ? chartX_ : camX);
        const float dz = camZ - (chartPinned_ ? chartZ_ : camZ);
        const float d  = farDistance() + std::sqrt(dx * dx + dz * dz);
        const float s  = std::sin(0.5f * d / cfg_.planetRadius);
        vertical += 2.0f * cfg_.planetRadius * s * s;
    }
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
    // reason above. This is the surface a caller stands the camera and its
    // collision on — it must be the drawn surface exactly.
    const float floorM = dataFloorAt(x, z);
    const int   up     = kDetailUpOctaves;
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

    return h0 + weight * sum;
}

// ---------------------------------------------------------------------------
// Chart-aware ground — where the RENDERED sheet is, not where the field is.
//
// cmCurve (clipmap_common.glsl) maps the flat-chart point at arc distance
// d = R*th from the chart centre to
//     chord  rho = (R + h) * sin th                (horizontal, from centre)
//     height y   = h * cos th - 2R * sin^2(th/2)
// With the default camera-following centre the bend under any point the
// engine asks about is re-zeroed every update; with a PINNED centre
// (setChartCenter) a world position at chord rho sits over the flat point at
// arc d = R * asin(rho / (R + h)), and the sheet there has dropped by the
// sagitta 2R*sin^2(th/2) ~ rho^2/2R. Both R and the mapping are the very
// ones the shader uses (cfg_.planetRadius is pushed as u_planetRadius), so
// the two cannot drift.
//
// The inversion needs h before it has found the point that carries h, so it
// runs twice: h/R < ~1.3e-3 on any Earth-like world, so the h = 0 pass lands
// within rho * h/R (~500 m at 400 km) and the refined pass within
// centimetres of the true foot point.
// ---------------------------------------------------------------------------

bool ClipmapTerrain::chartPointUnder(float x, float z, float& fx, float& fz,
                                     float& th) const {
    fx = x;
    fz = z;
    th = 0.0f;
    if (!chartPinned_ || cfg_.planetRadius <= 0.0f) return false;
    const double R  = cfg_.planetRadius;
    const double dx = static_cast<double>(x) - chartX_;
    const double dz = static_cast<double>(z) - chartZ_;
    const double rho = std::sqrt(dx * dx + dz * dz);
    if (!(rho > 0.0)) return false;
    const double ux = dx / rho, uz = dz / rho;
    // asin's argument is clamped: a chord past R + h has bent beyond the
    // sphere's equator and has no point beneath it — answer with the rim.
    double t = std::asin(std::min(rho / R, 1.0));
    const double h0 = baseElevationAt(static_cast<float>(chartX_ + ux * R * t),
                                      static_cast<float>(chartZ_ + uz * R * t));
    t = std::asin(std::min(rho / std::max(R + h0, 1.0), 1.0));
    fx = static_cast<float>(chartX_ + ux * R * t);
    fz = static_cast<float>(chartZ_ + uz * R * t);
    th = static_cast<float>(t);
    return true;
}

float ClipmapTerrain::renderedElevationAt(float x, float z) const {
    float fx, fz, th;
    if (!chartPointUnder(x, z, fx, fz, th)) return elevationAt(x, z);
    const double h = elevationAt(fx, fz);
    const double s = std::sin(0.5 * static_cast<double>(th));
    return static_cast<float>(h * std::cos(static_cast<double>(th))
                              - 2.0 * static_cast<double>(cfg_.planetRadius)
                                    * s * s);
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

void ClipmapTerrain::setChartCenter(float x, float z) {
    chartPinned_ = true;
    chartX_ = x;
    chartZ_ = z;
    if (node_) {
        const float chartXZ[2] = {x, z};
        node_->setCustomShaderUniform("u_chartXZ", 2, chartXZ);
    }
}

void ClipmapTerrain::clearChartCenter() {
    chartPinned_ = false;
    if (node_) {
        const float chartXZ[2] = {lastCamX_, lastCamZ_};
        node_->setCustomShaderUniform("u_chartXZ", 2, chartXZ);
    }
}

void ClipmapTerrain::setSnowLine(float snowLine) {
    cfg_.snowLine = snowLine;
    if (node_) {
        float sl = snowLine;
        node_->setCustomShaderUniform("u_snowLine", 1, &sl);
    }
}

void ClipmapTerrain::setForest(const float* albedo, float strength) {
    std::copy(albedo, albedo + 3, forestAlbedo_);
    forestTint_ = strength;
    if (node_) {
        node_->setCustomShaderUniform("u_albedoForest", 3, forestAlbedo_);
        node_->setCustomShaderUniform("u_forestTint", 1, &forestTint_);
    }
}

void ClipmapTerrain::setDetail(float wavelength, float relief, float gain, int octaves) {
    cfg_.detailWavelength = wavelength;
    cfg_.detailRelief = relief;
    cfg_.detailGain = gain;
    cfg_.detailOctaves = octaves;
    if (node_) {
        node_->setCustomShaderUniform("u_detailWavelength", 1, &wavelength);
        node_->setCustomShaderUniform("u_detailRelief", 1, &relief);
        node_->setCustomShaderUniform("u_detailGain", 1, &gain);
        float oct = (float)octaves;
        node_->setCustomShaderUniform("u_detailOctaves", 1, &oct);
    }
}

void ClipmapTerrain::setMaterials(const float* rockAlbedo, float rockRoughness,
                                  const float* snowAlbedo, float snowRoughness,
                                  const float* sandAlbedo, float sandRoughness,
                                  const float* grassAlbedo, float grassRoughness) {
    std::copy(rockAlbedo, rockAlbedo + 3, rockAlbedo_);
    rockRoughness_ = rockRoughness;
    std::copy(snowAlbedo, snowAlbedo + 3, snowAlbedo_);
    snowRoughness_ = snowRoughness;
    std::copy(sandAlbedo, sandAlbedo + 3, sandAlbedo_);
    sandRoughness_ = sandRoughness;
    std::copy(grassAlbedo, grassAlbedo + 3, grassAlbedo_);
    grassRoughness_ = grassRoughness;

    if (node_) {
        node_->setCustomShaderUniform("u_albedoRock", 3, rockAlbedo_);
        node_->setCustomShaderUniform("u_roughnessRock", 1, &rockRoughness_);
        node_->setCustomShaderUniform("u_albedoSnow", 3, snowAlbedo_);
        node_->setCustomShaderUniform("u_roughnessSnow", 1, &snowRoughness_);
        node_->setCustomShaderUniform("u_albedoSand", 3, sandAlbedo_);
        node_->setCustomShaderUniform("u_roughnessSand", 1, &sandRoughness_);
        node_->setCustomShaderUniform("u_albedoGrass", 3, grassAlbedo_);
        node_->setCustomShaderUniform("u_roughnessGrass", 1, &grassRoughness_);
    }
}

void ClipmapTerrain::setSurfaceLayer(const float* data, int width, int height,
                                     float originX, float originZ, float metresPerCell,
                                     int components) {
    setSurfaceLayer(0, data, width, height, originX, originZ, metresPerCell, components);
}

void ClipmapTerrain::setSurfaceLayer(int index, const float* data, int width, int height,
                                     float originX, float originZ, float metresPerCell,
                                     int components) {
    if (!node_) return;
    if (index < 0 || index >= kMaxLayers) return;
    if (components != 3 && components != 4) return;

    SurfaceLayer& s = surf_[index];
    if (!data || width <= 0 || height <= 0) {
        s.data.clear();
        s.width = s.height = 0;
        s.present = false;
        const float zero4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        node_->setCustomShaderTexture(kSurfTex[index], 1, 1, zero4, false, false, true, 4);
    } else {
        // Stored RGBA whatever the caller supplied, so the sampler's swizzle is
        // one thing and not two. A three-channel caller gets w = 0: GL would
        // fill an RGB texture's alpha with 1.0, and a channel nobody supplied
        // reading as saturated is a bug waiting for the first shader that uses
        // it.
        const size_t texels = (size_t)width * (size_t)height;
        s.data.assign(texels * 4, 0.0f);
        for (size_t i = 0; i < texels; ++i) {
            s.data[i * 4 + 0] = data[i * components + 0];
            s.data[i * 4 + 1] = data[i * components + 1];
            s.data[i * 4 + 2] = data[i * components + 2];
            if (components == 4) s.data[i * 4 + 3] = data[i * 4 + 3];
        }
        s.width = width;
        s.height = height;
        s.originX = originX;
        s.originZ = originZ;
        s.metresPerCell = metresPerCell;
        s.present = true;
        node_->setCustomShaderTexture(kSurfTex[index], width, height, s.data.data(),
                                      false, false, true, 4);
    }

    // Contiguous run from 0, exactly like the height stack: the shader's blend
    // starts from the coarsest PRESENT layer and a hole in the middle would make
    // "coarsest present" mean something different per fragment.
    surfaceLayerCount_ = 0;
    for (int i = 0; i < kMaxLayers && surf_[i].present; ++i) surfaceLayerCount_ = i + 1;

    pushSurfaceUniforms();
}

void ClipmapTerrain::pushSurfaceUniforms() {
    if (!node_) return;
    for (int i = 0; i < kMaxLayers; ++i) {
        const SurfaceLayer& s = surf_[i];
        const float a[3] = {s.originX, s.originZ,
                            s.metresPerCell > 0.0f ? s.metresPerCell : 1.0f};
        const float b[2] = {s.present ? static_cast<float>(s.width) : 0.0f,
                            s.present ? static_cast<float>(s.height) : 0.0f};
        node_->setCustomShaderUniform(kSurfA[i], 3, a);
        node_->setCustomShaderUniform(kSurfB[i], 2, b);
    }
    const float n = static_cast<float>(surfaceLayerCount_);
    node_->setCustomShaderUniform("u_surfaceCount", 1, &n);

    // u_surfPresent is layer 0's own flag and is kept because shaders written
    // against the single-layer API branch on it. It means exactly what it always
    // meant — "is there a finest control layer" — and stays truthful under the
    // stack; `u_surfaceCount` is what a multi-layer shader reads.
    const float present = surf_[0].present ? 1.0f : 0.0f;
    node_->setCustomShaderUniform("u_surfPresent", 1, &present);
}

} // namespace bro::scene
