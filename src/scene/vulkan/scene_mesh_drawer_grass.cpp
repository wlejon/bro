// SceneMeshDrawer's grass path: a GrassNode becomes up to four draws, one
// per ring around the camera, each instancing its ring mesh (a cell's
// blades) over only the cells of its ring the view can see. The shader (shaders/scene_grass.glsl)
// places, culls, bends and colours every blade; this file picks the cells,
// stamps the movers and binds the node's field and influence map.

#include "scene/vulkan/scene_mesh_drawer.h"

#include "scene/grass_node.h"
#include "scene/light_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

/// shaders/scene_grass.glsl's GrassUBO.
struct GrassUBO {
    float cells[4];
    float ring[4];
    float field[4];
    float fieldSize[4];
    float blade[4];
    float lush[4];
    float dry[4];
    float look[4];
    float push[4];
    float pushRate[4];
    float shape[4];
    float lod[4];
    float herbInfo[4];
    float herbRing[4];
    float herbShape[4];
    float layer[4];    // the turf's surface layer (scene_ground_layers.glsl): tile side, texels, border, levels (0: none)
    float layer2[4];   // its texel's most pixels, page shift, page entries a side, level-0 texel
    float herbs[GrassNode::kHerbMax * GrassNode::kHerbRecordFloats];
    float herbMean[GrassNode::kHerbMeanSteps * GrassNode::kHerbMeanFloats];
};
static_assert(sizeof(GrassUBO) == (17 + GrassNode::kHerbMax * GrassNode::kHerbRecordFloats / 4
                                   + GrassNode::kHerbMeanSteps * GrassNode::kHerbMeanFloats / 4) * 16);

constexpr float kPi = 3.14159265358979f;
constexpr float kWidthGrowth = 0.03f;            // a far blade is wider, so a ring keeps its cover
constexpr uint32_t kMaxRingInstances = 1u << 22;  // a guard, far above any view's need
constexpr int kTile = 16;                         // cells a side of the tiles the view culls

/// The x/z box around the eye holding everything of a disc of `radius` the
/// view can see (the view's cone flattened onto the ground), padded by
/// `margin`. The whole disc when the camera looks steeply down or is
/// orthographic.
void visibleBox(const SceneView& view, float radius, float margin, float (&box)[4]) {
    const float ex = view.eye.x, ez = view.eye.z;
    box[0] = ex - radius - margin;
    box[1] = ez - radius - margin;
    box[2] = ex + radius + margin;
    box[3] = ez + radius + margin;
    if (std::fabs(view.proj.at(3, 3)) > 0.5f) return;   // orthographic

    // The four corner rays of the frustum, as x/z angles.
    std::array<float, 4> ang{};
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int k = 0; k < 4; ++k) {
        const bromath::Mat4& ip = view.invProj;
        float p[4];
        for (int r = 0; r < 4; ++r)
            p[r] = ip.at(r, 0) * corners[k][0] + ip.at(r, 1) * corners[k][1] + ip.at(r, 2) * 0.5f + ip.at(r, 3);
        if (std::fabs(p[3]) < 1e-12f) return;
        const float vx = p[0] / p[3], vy = p[1] / p[3], vz = p[2] / p[3];
        const bromath::Mat4& iv = view.invRelView;
        const float dx = iv.at(0, 0) * vx + iv.at(0, 1) * vy + iv.at(0, 2) * vz;
        const float dz = iv.at(2, 0) * vx + iv.at(2, 1) * vy + iv.at(2, 2) * vz;
        if (dx * dx + dz * dz < 1e-8f) return;
        ang[k] = std::atan2(dz, dx);
    }
    // The smallest arc holding all four: the complement of the widest gap.
    std::sort(ang.begin(), ang.end());
    float gap = ang[0] + 2.0f * kPi - ang[3];
    float start = ang[0];
    for (int k = 0; k < 3; ++k) {
        if (ang[k + 1] - ang[k] > gap) {
            gap = ang[k + 1] - ang[k];
            start = ang[k + 1];
        }
    }
    const float arc = 2.0f * kPi - gap;
    if (arc >= kPi * 0.98f) return;

    float lo[2] = {ex, ez}, hi[2] = {ex, ez};
    const auto take = [&](float a) {
        const float x = ex + radius * std::cos(a), z = ez + radius * std::sin(a);
        lo[0] = std::min(lo[0], x);
        lo[1] = std::min(lo[1], z);
        hi[0] = std::max(hi[0], x);
        hi[1] = std::max(hi[1], z);
    };
    take(start);
    take(start + arc);
    for (int q = 0; q < 4; ++q) {
        const float a = static_cast<float>(q) * 0.5f * kPi;
        float rel = std::fmod(a - start, 2.0f * kPi);
        if (rel < 0.0f) rel += 2.0f * kPi;
        if (rel <= arc) take(a);
    }
    box[0] = lo[0] - margin;
    box[1] = lo[1] - margin;
    box[2] = hi[0] + margin;
    box[3] = hi[1] + margin;
}

/// The box's cells (`cell` m) in tiles of kTile x kTile, as runs along each
/// row of tiles of the ones `keep(x0, z0, x1, z1)` accepts: the first cell
/// column, row, columns and rows of each.
using TileRuns = std::vector<std::array<int, 4>>;
template <class Keep>
bool tileRuns(const float (&box)[4], float cell, Keep&& keep, TileRuns& runs) {
    runs.clear();
    const int i0 = static_cast<int>(std::floor(box[0] / cell));
    const int j0 = static_cast<int>(std::floor(box[1] / cell));
    const int i1 = static_cast<int>(std::floor(box[2] / cell));
    const int j1 = static_cast<int>(std::floor(box[3] / cell));
    const uint64_t w = static_cast<uint64_t>(std::max(i1 - i0 + 1, 0));
    const uint64_t h = static_cast<uint64_t>(std::max(j1 - j0 + 1, 0));
    if (w == 0 || h == 0 || w * h > kMaxRingInstances) return false;
    for (int tj = j0; tj <= j1; tj += kTile) {
        const int cj1 = std::min(tj + kTile - 1, j1);
        bool inRun = false;   // (cells are signed: no sentinel column)
        int runStart = 0, runEnd = 0;
        const auto flush = [&]() {
            if (inRun) runs.push_back({runStart, tj, runEnd - runStart + 1, cj1 - tj + 1});
            inRun = false;
        };
        for (int ti = i0; ti <= i1; ti += kTile) {
            const int ci1 = std::min(ti + kTile - 1, i1);
            const float x0 = static_cast<float>(ti) * cell, x1 = static_cast<float>(ci1 + 1) * cell;
            const float z0 = static_cast<float>(tj) * cell, z1 = static_cast<float>(cj1 + 1) * cell;
            if (!keep(x0, z0, x1, z1)) {
                flush();
                continue;
            }
            if (!inRun) runStart = ti;
            inRun = true;
            runEnd = ci1;
        }
        flush();
    }
    return !runs.empty();
}

/// Whether a tile [x0, x1] x [z0, z1] reaches the ring [rIn, rOut] (padded
/// already) around the eye lifted `lift` over the ground.
bool tileInRing(float ex, float ez, float lift, float rIn, float rOut, float x0, float z0, float x1, float z1) {
    const float nx = std::clamp(ex, x0, x1) - ex, nz = std::clamp(ez, z0, z1) - ez;
    const float fx = std::max(std::fabs(x0 - ex), std::fabs(x1 - ex));
    const float fz = std::max(std::fabs(z0 - ez), std::fabs(z1 - ez));
    return nx * nx + nz * nz + lift * lift <= rOut * rOut && fx * fx + fz * fz + lift * lift >= rIn * rIn;
}

// The far fans' ring ends where a blade of the mean height stands this many
// pixels tall, never past the node's range: further out the turf's tops
// (scene_ground.glsl) carry the sward, so the hand-over sits at the same
// size on screen at any resolution and field of view.
constexpr float kFanEdgePixels = 6.0f;

/// Ring `r` as this view draws it: the far ring pulled in to kFanEdgePixels
/// (never inside the ring before it, its band and a margin).
GrassNode::Ring ringInView(const GrassNode& node, const SceneView& view, int r) {
    GrassNode::Ring ring = node.ring(r);
    if (r != GrassNode::kRings - 1 || !view.perspective || view.height == 0) return ring;
    const float pxAtMetre = std::fabs(view.proj.at(1, 1)) * static_cast<float>(view.height) * 0.5f;
    const float edge = std::max(node.params().height * pxAtMetre / kFanEdgePixels, ring.inner + ring.bandIn + 1.5f);
    if (edge >= ring.outer) return ring;
    ring.thinFrom *= edge / ring.outer;
    ring.outer = edge;
    ring.bandOut = std::max(1.5f, edge * 0.12f);   // as GrassNode::ring's
    return ring;
}

MeshNode::UserTexture* slot(GrassNode& node, const char* name) {
    for (auto& t : node.customShaderTextures())
        if (t.name == name && t.w > 0 && t.h > 0) return &t;
    return nullptr;
}

/// The node's textures this frame: its field, influence map and floor (the
/// floor may be absent). False when the field or the map is not up yet.
struct GrassImages {
    const SceneVkImage* field = nullptr;
    const SceneVkImage* push = nullptr;
    const SceneVkImage* floor = nullptr;
};
bool grassImages(SceneGpuResources& res, GrassNode& node, GrassImages& out) {
    MeshNode::UserTexture* fieldTex = slot(node, GrassNode::kFieldTexture);
    MeshNode::UserTexture* pushTex = slot(node, GrassNode::kPushTexture);
    MeshNode::UserTexture* floorTex = slot(node, GrassNode::kFloorTexture);
    out.field = fieldTex ? res.userTexture(node.id(), *fieldTex) : nullptr;
    out.push = pushTex ? res.userTexture(node.id(), *pushTex) : nullptr;
    out.floor = floorTex ? res.userTexture(node.id(), *floorTex) : nullptr;
    return out.field && out.push;
}

/// The UBO's fields every draw of the node shares (the field, the colours,
/// the look, the influence map).
void fillShared(const GrassNode& node, GrassUBO& u) {
    const GrassNode::Params& p = node.params();
    u.cells[0] = p.tufts;
    u.cells[1] = p.flowering;
    u.cells[2] = p.dead;
    u.field[0] = node.fieldX0();
    u.field[1] = node.fieldZ0();
    u.field[2] = node.fieldCell();
    u.field[3] = p.width;   // over which a ring's width is how many blades its blade stands for
    u.fieldSize[0] = static_cast<float>(node.fieldCols());
    u.fieldSize[1] = static_cast<float>(node.fieldRows());
    u.fieldSize[2] = 1.0f / static_cast<float>(node.fieldCols());
    u.fieldSize[3] = 1.0f / static_cast<float>(node.fieldRows());
    std::copy(std::begin(p.lush), std::end(p.lush), u.lush);
    u.lush[3] = p.bulk;
    std::copy(std::begin(p.dry), std::end(p.dry), u.dry);
    u.dry[3] = p.clumpFreq;
    u.look[0] = p.rootShade;
    u.look[1] = p.sheen;
    u.look[2] = p.variety;
    u.look[3] = kWidthGrowth;
    u.push[0] = node.pushX0();
    u.push[1] = node.pushZ0();
    u.push[2] = GrassNode::kPushCell;
    u.push[3] = static_cast<float>(GrassNode::kPushCells);
    u.pushRate[0] = GrassNode::kBendSpringHz;
    u.pushRate[1] = GrassNode::kBendSpringDamping;
    u.pushRate[2] = node.hasFloor() ? 1.0f : 0.0f;
    u.pushRate[3] = GrassNode::kTrampleRelax;
    // The herbs, which every grass shader reads: the plants, and past them
    // the turf that stands in.
    if (node.hasHerbs()) {
        const std::vector<float>& rec = node.herbRecords();
        std::copy(rec.begin(), rec.end(), u.herbs);
        std::copy(node.herbMean(), node.herbMean() + GrassNode::kHerbMeanSteps * GrassNode::kHerbMeanFloats,
                  u.herbMean);
        u.herbInfo[0] = static_cast<float>(node.herbCount());
        u.herbInfo[1] = GrassNode::kHerbCell;
        const GrassNode::HerbRing last = node.herbRing(GrassNode::kHerbRings - 1);
        u.herbInfo[2] = last.outer;
        u.herbInfo[3] = last.bandOut;
        // (The whole plants' range: the herb draws set the rest of herbShape,
        // the turf reads only this.)
        u.herbShape[3] = node.herbRange();
    }
}

/// The eye's lift: its height over the ground past a standing eye's. The
/// rings scale with it (a blade's ring distance is its distance across the
/// ground and the lift together), so seen from above the canopy the near
/// rings, all sub-pixel there, are not issued at all; a standing or
/// crouching eye has none.
float eyeLift(const GrassNode& node, const SceneView& view) {
    constexpr float kStandingEye = 2.0f;
    return std::max(0.0f, view.eye.y - node.fieldHeight(view.eye.x, view.eye.z, view.eye.y) - kStandingEye);
}

/// The ground set's surface layer (scene_ground_layers.glsl's bindings 4-8):
/// its four layers and its page table, or the empty fallbacks.
struct GroundLayerBinding {
    VkImageView layers[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSampler layerSampler = VK_NULL_HANDLE;
    VkImageView page = VK_NULL_HANDLE;
    VkSampler pageSampler = VK_NULL_HANDLE;
};

VkDescriptorSet writeSet(SceneFrame& frame, VkDevice device, const SceneDefaults& defaults, const GrassUBO& u,
                         const GrassImages& img, const GroundLayerBinding* ground = nullptr) {
    const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(&u, sizeof(u));
    VkDescriptorSet set = frame.gpu.device.frameSet(defaults.customLayout);
    if (set == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    writer.writeImage(1, img.field->view, img.field->sampler);
    writer.writeImage(2, img.push->view, img.push->sampler);
    // No floor: black (an open sky everywhere).
    writer.writeImage(3, img.floor ? img.floor->view : defaults.black.view,
                      img.floor ? img.floor->sampler : defaults.sampler);
    uint32_t b = 4;
    if (ground) {
        for (int i = 0; i < 4; ++i, ++b)
            writer.writeImage(b, ground->layers[i] ? ground->layers[i] : defaults.zeroArray.view,
                              ground->layers[i] ? ground->layerSampler : defaults.sampler);
        writer.writeImage(b++, ground->page ? ground->page : defaults.zeroUintArray.view,
                          ground->page ? ground->pageSampler : defaults.nearestSampler);
    }
    for (; b <= static_cast<uint32_t>(MeshNode::kMaxUserTextures); ++b)
        writer.writeImage(b, defaults.white.view, defaults.sampler);
    writer.updateSet(device, set);
    return set;
}

}  // namespace

VkDescriptorSet SceneMeshDrawer::groundSet(SceneFrame& frame, GrassNode& node) {
    if (!node.hasField()) return VK_NULL_HANDLE;
    GrassImages img;
    if (!grassImages(frame.gpu.resources, node, img)) return VK_NULL_HANDLE;
    GrassUBO u{};
    fillShared(node, u);
    // The ring the turf continues: the far ring as this view draws it.
    const GrassNode::Ring outer = ringInView(node, frame.view, GrassNode::kRings - 1);
    u.ring[0] = 0.0f;
    u.ring[1] = outer.outer;
    u.ring[3] = outer.bandOut;
    u.blade[0] = node.params().height;
    u.blade[2] = node.params().minNormalY;
    u.blade[3] = node.params().wind;   // the sward's lay in the wind (grassSwardLay), as the blades'
    u.lod[3] = eyeLift(node, frame.view);
    // The turf's surface layer, once its tiles are configured (its first
    // frame works everything out where it draws).
    GroundLayerBinding layers;
    if (GroundLayers* gl = groundLayers(frame, node); gl && gl->tiles.configured()) {
        const SurfaceLayerConfig& c = gl->tiles.config();
        for (int i = 0; i < gl->atlas.layerCount() && i < 4; ++i) layers.layers[i] = gl->atlas.layerView(i);
        layers.layerSampler = gl->atlas.layerSampler();
        layers.page = gl->atlas.pageView();
        layers.pageSampler = gl->atlas.pageSampler();
        u.layer[0] = c.tileMetres;
        u.layer[1] = static_cast<float>(c.tileTexels);
        u.layer[2] = static_cast<float>(c.border);
        u.layer[3] = static_cast<float>(gl->tiles.levels());
        u.layer2[0] = c.texelPixels;
        u.layer2[1] = static_cast<float>(gl->tiles.pageShift());
        u.layer2[2] = static_cast<float>(c.pageSize);
        u.layer2[3] = gl->tiles.texelMetres(0);
    }
    return writeSet(frame, device_, *defaults_, u, img, &layers);
}

uint64_t SceneMeshDrawer::groundGeneration(GrassNode& node) {
    // FNV-1a over everything every tile's fill reads: the field and floor,
    // the herbs and the parameters the patterns take.
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (const char* name : {GrassNode::kFieldTexture, GrassNode::kFloorTexture}) {
        const MeshNode::UserTexture* t = slot(node, name);
        const uint64_t g = t ? t->generation : 0;
        mix(&g, sizeof(g));
    }
    const std::vector<float>& herbs = node.herbRecords();
    if (!herbs.empty()) mix(herbs.data(), herbs.size() * sizeof(float));
    const int count = node.hasHerbs() ? node.herbCount() : 0;
    mix(&count, sizeof(count));
    const GrassNode::Params& p = node.params();
    const float params[] = {p.clumpFreq, p.height, p.flowering, p.dead, node.fieldX0(), node.fieldZ0(),
                            node.fieldCell()};
    mix(params, sizeof(params));
    return h;
}

void SceneMeshDrawer::prepareGrass(SceneFrame& frame, GrassNode& node, std::vector<MeshDraw>& out) {
    out.clear();
    const SceneView& view = frame.view;
    node.stamp(frame.graph, frame.renderer.windTime(), view.eye.x, view.eye.z);
    for (int i = 0; i < GrassNode::kRings; ++i) node.setDrawnInstances(i, 0);
    if (!node.hasField() || node.effectiveUnlit()) return;

    SceneGpuResources& res = frame.gpu.resources;
    GrassImages img;
    const bool ready = grassImages(res, node, img);
    res.pruneUserTextures(node.id(), node.customShaderTextures());
    if (!ready) return;

    const GpuMesh* ringMeshes[GrassNode::kRings];
    for (int r = 0; r < GrassNode::kRings; ++r) {
        ringMeshes[r] = res.mesh(node.id(), static_cast<uint32_t>(r), node.geometryGeneration(), node.ringMesh(r));
        if (!ringMeshes[r]) return;
    }

    const GrassNode::Params& p = node.params();
    GrassUBO u{};
    fillShared(node, u);

    uint32_t flags = 0;
    if (frame.ssr) flags |= mesh_flags::kReflectance;
    if (node.receivesShadow()) flags |= mesh_flags::kReceivesShadow;
    flags |= mesh_flags::kTwoSided;
    // A culm's head is cut per pixel: under 4x MSAA it keeps its share of a
    // pixel's samples rather than all or none.
    const uint32_t msaa = frame.gpu.targets.samples() == VK_SAMPLE_COUNT_4_BIT ? mesh_flags::kMsaa : 0u;

    // The shadow tiles the near ring casts into: a directional light's first
    // cascade, every tile of any other light.
    uint32_t shadowMask = 0;
    {
        const ShadowPlan& plan = frame.renderer.shadowPlan();
        for (int i = 0; i < ShadowPlan::kMaxLights; ++i) {
            const int slot = plan.lightSlot[i];
            if (slot < 0 || slot >= plan.tileCount) continue;
            const LightNode* light = plan.tiles[slot].light;
            const int count = light && light->kind() == LightNode::Kind::Directional ? 1 : plan.lightSlotCount[i];
            for (int k = 0; k < count && slot + k < 32; ++k) shadowMask |= 1u << (slot + k);
        }
    }

    const float lift = eyeLift(node, view);
    TileRuns runs;

    for (int r = 0; r < GrassNode::kRings; ++r) {
        const GrassNode::Ring ring = ringInView(node, view, r);
        // Blades lean and stand up to ~2x the mean height: pad the box by
        // that, a cell and a margin.
        float box[4];
        visibleBox(view, ring.outer, ring.cell * 2.0f + p.height * 3.0f + 1.0f, box);

        // The box in tiles of kTile x kTile cells, each kept only when it can
        // hold a drawn blade: it reaches the ring (its dithered bands
        // included), something grows on it, and its box (ground to the
        // tallest blade, padded by a blade's lean and its spread) is in the
        // view. Every vertex of an instance runs whether its blades are drawn
        // or not, so cells the shader would only cull are not issued at all.
        // A flowering culm stands at most about 4.8 x the mean height times
        // the field's multiplier (clump 1.4, tuft 1.15, tall patch 1.7, culm
        // 1.75), a leaf 3.6; a cell's roots gather to its tussock up to
        // kGrassTussockReach off (scene_grass.glsl's grassTussock).
        constexpr float kGrassTussockReach = 0.35f;
        const float reach = ring.cell + ring.spread + p.height * 1.5f + kGrassTussockReach;
        const float top = p.height * 4.8f;
        const float ex = view.eye.x, ez = view.eye.z;
        const float rIn = std::max(ring.inner - ring.bandIn - reach, 0.0f), rOut = ring.outer + reach;
        const auto keep = [&](float x0, float z0, float x1, float z1) {
            float lo = 0.0f, hi = 0.0f, tall = 0.0f;
            return tileInRing(ex, ez, lift, rIn, rOut, x0, z0, x1, z1) &&
                   node.fieldRange(x0 - reach, z0 - reach, x1 + reach, z1 + reach, lo, hi, tall) &&
                   bromath::fintersects(view.frustum, bromath::AABB3{{x0 - reach, lo, z0 - reach},
                                                                     {x1 + reach, hi + top * tall, z1 + reach}});
        };
        if (!tileRuns(box, ring.cell, keep, runs)) continue;

        u.cells[3] = ring.cell;
        u.ring[0] = ring.inner;
        u.ring[1] = ring.outer;
        u.ring[2] = ring.bandIn;
        u.ring[3] = ring.bandOut;
        u.blade[0] = p.height;
        u.blade[1] = ring.width;
        u.blade[2] = p.minNormalY;
        u.blade[3] = p.wind;
        u.shape[0] = static_cast<float>(ring.blades);
        u.shape[1] = static_cast<float>(GrassNode::bladeVertices(ring.segments));
        u.shape[2] = ring.spread;
        u.shape[3] = ring.fan;
        u.lod[0] = ring.thinFrom;
        u.lod[1] = ring.minKeep;
        u.lod[2] = GrassNode::kShadowKeep;
        u.lod[3] = lift;

        VkDescriptorSet set = writeSet(frame, device_, *defaults_, u, img);
        if (set == VK_NULL_HANDLE) return;

        const GpuMesh* blade = ringMeshes[r];
        MeshDraw draw;
        draw.kind = MeshKind::Grass;
        draw.nodeId = node.id();
        draw.vertices = blade->vertices.buffer;
        draw.indices = blade->indices.buffer;
        draw.indexCount = blade->indexCount;
        draw.customSet = set;
        std::memcpy(draw.push.baseColor, node.color(), sizeof(draw.push.baseColor));
        draw.push.baseColor[3] = 1.0f;
        draw.push.pbrParams[0] = node.metallic();
        draw.push.pbrParams[1] = node.roughness();
        draw.push.pbrParams[3] = static_cast<float>(flags | msaa);
        draw.push.extra[1] = node.subsurface();
        draw.push.extra[3] = 1.0f + 256.0f;   // no atlas
        draw.twoSided = true;
        // (Timed by ring: perf.gpuPasses().)
        static constexpr const char* kRingLabels[GrassNode::kRings] = {"grass:ring0", "grass:ring1", "grass:ring2",
                                                                       "grass:ring3"};
        draw.timerLabel = kRingLabels[r];
        // Only the near ring casts: its blades are the ones whose shadows read.
        // And only into a sun's nearest cascade: a blade is narrower than a
        // far cascade's texel, and the near ring lies inside the first.
        draw.castsShadow = r == 0 && node.castsShadow();
        draw.shadowTileMask = shadowMask;
        // A caster keeps at most kShadowKeep of an instance's blades, the
        // first ones (scene_grass.glsl's thinning: blade j while j + a hash
        // < kept + 0.5), so its draw stops past them: the rest ran every
        // vertex only to be culled.
        if (draw.castsShadow) {
            const uint32_t blades = static_cast<uint32_t>(ring.blades);
            const uint32_t kept = std::min(blades, static_cast<uint32_t>(std::ceil(blades * GrassNode::kShadowKeep)) + 1u);
            const uint32_t perBlade = 3u * (2u * static_cast<uint32_t>(ring.segments) - 1u);   // grass_node.cpp's bladeMesh
            draw.shadowIndexCount = std::min(blade->indexCount, perBlade * kept);
        }
        // A draw a run of tiles: its cell rectangle rides in the model
        // matrix's first row (column, row, columns), which grass has no use
        // for (its blades stand on the field, the node's transform ignored);
        // the shadow pass passes that row on as modelRows[0].
        uint32_t drawn = 0;
        for (const auto& run : runs) {
            MeshDraw d = draw;
            std::memset(d.push.model, 0, sizeof(d.push.model));
            d.push.model[0] = static_cast<float>(run[0]);
            d.push.model[4] = static_cast<float>(run[1]);
            d.push.model[8] = static_cast<float>(run[2]);
            d.push.model[15] = 1.0f;
            d.instanceCount = static_cast<uint32_t>(run[2]) * static_cast<uint32_t>(run[3]);
            drawn += d.instanceCount;
            out.push_back(d);
        }
        node.setDrawnInstances(r, drawn);
    }

    // The herbs: a draw a run of tiles of the herbs' cells in each of their
    // rings, mesh_herb.vert building each cell's plant (if any) from its
    // card strips, mesh.frag's SCENE_HERB build cutting them. They cast no
    // shadow (yet): they lie in the grass's and the trees'.
    for (int r = 0; r < GrassNode::kHerbRings; ++r) node.setDrawnHerbs(r, 0);
    if (!node.hasHerbs()) return;
    const float herbTop = node.herbHeight() * 1.4f + 0.2f;
    for (int r = 0; r < GrassNode::kHerbRings; ++r) {
        const GrassNode::HerbRing ring = node.herbRing(r);
        // Seen from this high, all of the far ring is left to the turf
        // (scene_herb_mix.glsl's herbFarSteep): not issued at all.
        if (r == GrassNode::kHerbRings - 1 && lift > 0.5f * ring.outer) continue;
        const GpuMesh* cards = res.mesh(node.id(), static_cast<uint32_t>(GrassNode::kRings + r),
                                        node.geometryGeneration(), node.herbRingMesh(r));
        if (!cards) return;
        const float cell = GrassNode::kHerbCell;
        // A plant reaches out about its spread and leans with the wind (a far
        // plant widened up to twice).
        const float reach = cell + (0.8f + node.herbHeight() * 0.5f) * (r == GrassNode::kHerbRings - 1 ? 2.0f : 1.0f);
        float box[4];
        visibleBox(view, ring.outer, reach + 1.0f, box);
        const float ex = view.eye.x, ez = view.eye.z;
        const float rIn = std::max(ring.inner - ring.bandIn - reach, 0.0f), rOut = ring.outer + reach;
        const auto keep = [&](float x0, float z0, float x1, float z1) {
            float lo = 0.0f, hi = 0.0f;
            return tileInRing(ex, ez, lift, rIn, rOut, x0, z0, x1, z1) &&
                   node.herbRangeAt(x0 - reach, z0 - reach, x1 + reach, z1 + reach, lo, hi) &&
                   bromath::fintersects(view.frustum, bromath::AABB3{{x0 - reach, lo, z0 - reach},
                                                                     {x1 + reach, hi + herbTop, z1 + reach}});
        };
        if (!tileRuns(box, cell, keep, runs)) continue;

        u.cells[3] = cell;
        u.herbRing[0] = ring.inner;
        u.herbRing[1] = ring.outer;
        u.herbRing[2] = ring.bandIn;
        u.herbRing[3] = ring.bandOut;
        u.herbShape[0] = static_cast<float>(ring.cards);
        u.herbShape[1] = static_cast<float>(GrassNode::cardVertices(ring.segments));
        // Which ring (2: the far one, thinned past the whole plants' range).
        u.herbShape[2] = static_cast<float>(r);
        u.herbShape[3] = node.herbRange();
        u.blade[2] = p.minNormalY;
        u.blade[3] = p.wind;
        u.lod[3] = lift;
        VkDescriptorSet set = writeSet(frame, device_, *defaults_, u, img);
        if (set == VK_NULL_HANDLE) return;

        MeshDraw draw;
        draw.kind = MeshKind::Grass;
        draw.nodeId = node.id();
        draw.vertices = cards->vertices.buffer;
        draw.indices = cards->indices.buffer;
        draw.indexCount = cards->indexCount;
        draw.customSet = set;
        std::memcpy(draw.push.baseColor, node.color(), sizeof(draw.push.baseColor));
        draw.push.baseColor[3] = 1.0f;
        draw.push.pbrParams[0] = 0.0f;
        draw.push.pbrParams[1] = 0.62f;
        draw.push.pbrParams[3] = static_cast<float>(flags | mesh_flags::kHerb | msaa);
        draw.push.extra[1] = node.subsurface();
        draw.push.extra[3] = 1.0f + 256.0f;   // no atlas
        draw.twoSided = true;
        // (Timed by ring: perf.gpuPasses().)
        static constexpr const char* kHerbLabels[GrassNode::kHerbRings] = {"herb:ring0", "herb:ring1", "herb:ring2"};
        draw.timerLabel = kHerbLabels[r];
        draw.castsShadow = false;
        uint32_t drawn = 0;
        for (const auto& run : runs) {
            MeshDraw d = draw;
            std::memset(d.push.model, 0, sizeof(d.push.model));
            d.push.model[0] = static_cast<float>(run[0]);
            d.push.model[4] = static_cast<float>(run[1]);
            d.push.model[8] = static_cast<float>(run[2]);
            d.push.model[15] = 1.0f;
            d.instanceCount = static_cast<uint32_t>(run[2]) * static_cast<uint32_t>(run[3]);
            drawn += d.instanceCount;
            out.push_back(d);
        }
        node.setDrawnHerbs(r, drawn);
    }
}

}  // namespace bro::scene::vk
