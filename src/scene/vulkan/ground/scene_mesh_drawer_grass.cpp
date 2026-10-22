// SceneMeshDrawer's grass path: a GrassNode becomes a draw a ring around the
// camera (four of grass, three of herbs). This file picks each ring's cells
// the view can see, stamps the movers and binds the node's field and
// influence map; the cover's plan (scene_cover_plan.h) lists the blades and
// plants in those cells the ring draws, and the shaders
// (shaders/ground/scene_grass.glsl, scene_herb.glsl) place, bend and colour
// each one, an instance a blade or a plant.

#include "scene/vulkan/draw/scene_mesh_drawer.h"

#include "scene/nodes/grass_node.h"
#include "scene/nodes/light_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/ground/scene_cover_plan.h"
#include "util/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

/// shaders/ground/scene_grass.glsl's GrassUBO.
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
    float herbData[GrassNode::kHerbDataVec4s * 4];   // the herb module's
};
static_assert(sizeof(GrassUBO) == (17 + GrassNode::kHerbDataVec4s) * 16);

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

// The cover's far tier (shaders/ground/scene_cover_far.glsl, whose constants
// these are): its levels, the ground's image an element stands for (pixels
// a side), the least sine of the eye's line onto the ground, and how far it
// reaches (its last stretch handing over to the turf alone by a dither).
constexpr int kCoverLevels = 10;           // COVER_FAR_LEVELS
constexpr float kCoverFarPixels = 5.0f;    // COVER_FAR_PIXELS
constexpr float kCoverFarMinSin = 0.01f;   // COVER_FAR_MIN_SIN
constexpr float kCoverFarRange = 3000.0f;
constexpr float kCoverFarBand = 600.0f;

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
// `herbs`: the node's herb module is drawn (it compiled).
void fillShared(const GrassNode& node, bool herbs, GrassUBO& u) {
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
    // The herbs' grid, which the far tier's elements stand on with or
    // without herbs.
    u.herbInfo[1] = GrassNode::kHerbCell;
    // The herbs, which every grass shader reads: the plants, and past them
    // the turf that stands in.
    if (herbs) {
        const std::vector<float>& data = node.herbData();
        std::copy(data.begin(), data.end(), u.herbData);
        u.herbInfo[0] = 1.0f;
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
    fillShared(node, coverShaders(node).index != 0, u);
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
    const uint64_t module = node.hasHerbs() ? node.herbSourceHash() : 0;
    mix(&module, sizeof(module));
    const std::vector<float>& herbs = node.herbData();
    if (module && !herbs.empty()) mix(herbs.data(), herbs.size() * sizeof(float));
    const GrassNode::Params& p = node.params();
    const float params[] = {p.clumpFreq, p.height, p.flowering, p.dead, node.fieldX0(), node.fieldZ0(),
                            node.fieldCell()};
    mix(params, sizeof(params));
    return h;
}

const CoverShaders& SceneMeshDrawer::coverShaders(const GrassNode& node) {
    if (!node.hasHerbs()) return *covers_[0];
    auto it = coverByModule_.find(node.herbSourceHash());
    if (it == coverByModule_.end()) {
        // Compiled here, at the first frame that draws it (its shaders in
        // parallel, each from the SPIR-V cache when it has been compiled
        // before). A module that does not compile is logged once and drawn
        // as none.
        auto set = std::make_unique<CoverShaders>();
        std::string err;
        uint32_t index = 0;
        if (set->create(device_, node.herbSource(), err)) {
            index = static_cast<uint32_t>(covers_.size());
            set->index = index;
            covers_.push_back(std::move(set));
        } else {
            LOG_ERROR("Grass node %u: its herb module did not compile, so it draws no herbs:\n%s", node.id(),
                      err.c_str());
        }
        it = coverByModule_.emplace(node.herbSourceHash(), index).first;
    }
    return *covers_[it->second];
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
    // Its shaders: the built-in ones, or its world's herb module's (no herbs
    // when that did not compile).
    const CoverShaders& cover = coverShaders(node);
    const bool herbs = cover.index != 0;
    GrassUBO u{};
    fillShared(node, herbs, u);

    uint32_t flags = 0;
    if (frame.ssr) flags |= mesh_flags::kReflectance;
    if (node.receivesShadow()) flags |= mesh_flags::kReceivesShadow;
    flags |= mesh_flags::kTwoSided;
    // A culm's head is cut per pixel: under 4x MSAA it keeps its share of a
    // pixel's samples rather than all or none.
    const uint32_t msaa = frame.gpu.targets.samples() == VK_SAMPLE_COUNT_4_BIT ? mesh_flags::kMsaa : 0u;

    const float lift = eyeLift(node, view);
    TileRuns runs;
    // Each ring's camera draw and its cells, for the cover's plan.
    struct Planned {
        MeshDraw draw;
        TileRuns runs;
        uint32_t perCell = 1;
        CoverPlan::Ring::Kind kind = CoverPlan::Ring::Kind::Grass;
    };
    std::vector<Planned> planned;
    // The rings planned: their camera draws take the plan's commands.
    const auto emitPlanned = [&]() {
        std::vector<CoverPlan::Ring> rings;
        rings.reserve(planned.size());
        for (const Planned& p : planned)
            rings.push_back({&p.runs, p.perCell, p.kind, p.draw.customSet, p.draw.indexCount});
        std::vector<CoverPlan::Draw> draws;
        if (!coverPlan_->add(frame, node.id(), cover, rings, draws)) return;
        for (size_t i = 0; i < planned.size(); ++i) {
            MeshDraw d = planned[i].draw;
            d.cover = cover.index;
            d.indirect = draws[i].commands;
            d.indirectOffset = draws[i].offset;
            d.vertexSet = draws[i].listSet;
            // After the plants, before the ground: the ground under the
            // blades then fails the early depth test rather than shading.
            d.order = MeshDraw::kOrderMid - 1;
            out.push_back(d);
        }
    };

    // The cover's far tier (scene_cover_far.glsl): a draw a level of its
    // nested cells, each over the tiles whose elements' lambda can fall in
    // that level. A tile's lambda is at least the ground's at its nearest
    // seen as face on as it can be (the eye's elevation over its lowest
    // ground, tipped by a slope as steep as its height range, and more), and
    // at most its farthest's seen edge on.
    const auto planCoverFar = [&]() {
        if (!view.perspective || view.height == 0) return;
        const GpuMesh* quad = res.mesh(node.id(), static_cast<uint32_t>(GrassNode::kRings + GrassNode::kHerbRings - 1),
                                       node.geometryGeneration(), node.herbRingMesh(GrassNode::kHerbRings - 1));
        if (!quad) return;
        const float pxAtMetre = std::fabs(view.proj.at(1, 1)) * static_cast<float>(view.height) * 0.5f;
        const float cell0 = GrassNode::kHerbCell;
        const auto lambda = [&](float d, float sinE) {
            const float cellPx = cell0 * pxAtMetre / std::max(d, 1e-3f);
            const float keep = cellPx * cellPx * std::max(sinE, kCoverFarMinSin) / (kCoverFarPixels * kCoverFarPixels);
            return std::max(-0.5f * std::log2(std::max(keep, 1e-12f)), 0.0f);
        };
        // Where the rings hand over: the fans' outer band and the far herb
        // ring's.
        const GrassNode::Ring fan = ringInView(node, view, GrassNode::kRings - 1);
        float start = fan.outer - fan.bandOut;
        if (herbs) {
            const GrassNode::HerbRing hr = node.herbRing(GrassNode::kHerbRings - 1);
            start = std::min(start, hr.outer - hr.bandOut);
        }
        const float tallest = std::max(node.params().height * 4.8f, herbs ? node.herbHeight() * 1.4f : 0.0f);
        const float ex = view.eye.x, ez = view.eye.z;
        u.ring[0] = kCoverFarRange;
        u.ring[1] = fan.outer;
        u.ring[2] = kCoverFarBand;
        u.ring[3] = fan.bandOut;
        u.blade[0] = node.params().height;
        u.blade[2] = node.params().minNormalY;
        u.blade[3] = node.params().wind;
        u.lod[3] = lift;
        u.herbShape[2] = 3.0f;   // the far tier
        for (int k = 0; k < kCoverLevels; ++k) {
            const bool last = k == kCoverLevels - 1;
            const float cell = cell0 * std::exp2(static_cast<float>(k));
            // The level's lambda is under k + 1 only nearer than the ground
            // seen face on gives it there.
            const float reachK = last ? kCoverFarRange : std::min(kCoverFarRange, cell0 * pxAtMetre * std::exp2(static_cast<float>(k + 1)) / kCoverFarPixels);
            // An element stands within its cell, widened up to
            // COVER_FAR_WIDEN times its plant's spread.
            const float reach = cell + 8.0f * 1.5f;
            float box[4];
            visibleBox(view, reachK, reach, box);
            const auto keep = [&](float x0, float z0, float x1, float z1) {
                const float nx = std::clamp(ex, x0, x1) - ex, nz = std::clamp(ez, z0, z1) - ez;
                const float fx = std::max(std::fabs(x0 - ex), std::fabs(x1 - ex));
                const float fz = std::max(std::fabs(z0 - ez), std::fabs(z1 - ez));
                const float nearH = std::sqrt(nx * nx + nz * nz);
                const float dNear = std::sqrt(nearH * nearH + lift * lift);
                const float dFar = std::sqrt(fx * fx + fz * fz + lift * lift);
                if (dFar < start || dNear > kCoverFarRange) return false;
                float lo = 0.0f, hi = 0.0f, tall = 0.0f;
                const bool grows = node.fieldRange(x0 - reach, z0 - reach, x1 + reach, z1 + reach, lo, hi, tall) ||
                                   (herbs &&
                                    node.herbRangeAt(x0 - reach, z0 - reach, x1 + reach, z1 + reach, lo, hi));
                if (!grows) return false;
                // The eye's elevation over the tile's lowest ground, tipped
                // by a slope as steep as twice its rise across the tile and
                // a quarter more (the ground between its coarse blocks).
                const float rise = std::max(view.eye.y - lo, 0.0f);
                const float elev = std::atan2(rise, std::max(nearH, 1e-3f));
                const float tilt = std::atan(2.0f * (hi - lo) / std::max(x1 - x0, 1e-3f) + 0.25f);
                const float sinMax = std::sin(std::min(elev + tilt, 0.5f * kPi));
                const float lamLo = lambda(dNear, sinMax), lamHi = lambda(dFar, kCoverFarMinSin);
                if (lamHi < static_cast<float>(k) || (!last && lamLo >= static_cast<float>(k + 1))) return false;
                return bromath::fintersects(view.frustum, bromath::AABB3{{x0 - reach, lo, z0 - reach},
                                                                         {x1 + reach, hi + tallest, z1 + reach}});
            };
            TileRuns levelRuns;
            if (!tileRuns(box, cell, keep, levelRuns)) continue;

            u.cells[3] = cell;
            u.herbShape[0] = static_cast<float>(k);
            VkDescriptorSet set = writeSet(frame, device_, *defaults_, u, img);
            if (set == VK_NULL_HANDLE) return;

            MeshDraw draw;
            draw.kind = MeshKind::Grass;
            draw.nodeId = node.id();
            draw.vertices = quad->vertices.buffer;
            draw.indices = quad->indices.buffer;
            draw.indexCount = 6;   // the first card's quad
            draw.customSet = set;
            std::memcpy(draw.push.baseColor, node.color(), sizeof(draw.push.baseColor));
            draw.push.baseColor[3] = 1.0f;
            draw.push.pbrParams[1] = 0.85f;
            draw.push.pbrParams[3] = static_cast<float>(flags | mesh_flags::kCoverFar | msaa);
            draw.push.extra[1] = node.subsurface();
            draw.push.extra[3] = 1.0f + 256.0f;   // no atlas
            draw.twoSided = true;
            draw.castsShadow = false;
            static constexpr const char* kLevelLabels[kCoverLevels] = {
                "cover:far0", "cover:far1", "cover:far2", "cover:far3", "cover:far4",
                "cover:far5", "cover:far6", "cover:far7", "cover:far8", "cover:far9"};
            draw.timerLabel = kLevelLabels[k];
            planned.push_back({draw, std::move(levelRuns), 1u, CoverPlan::Ring::Kind::Far});
        }
    };

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
        // view. The plan tests every blade of every cell kept.
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
        if (set == VK_NULL_HANDLE) break;

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
        const uint32_t perBlade = 3u * (2u * static_cast<uint32_t>(ring.segments) - 1u);   // grass_node.cpp's bladeMesh
        uint32_t drawn = 0;
        for (const auto& run : runs) drawn += static_cast<uint32_t>(run[2]) * static_cast<uint32_t>(run[3]);
        node.setDrawnInstances(r, drawn);
        // Only the near ring casts: its blades are the ones whose shadows read.
        // And only into the overlays of the sun's finest levels and of
        // local lights (ShadowViewPlan::grass): a blade is narrower than a
        // coarser level's texel, and it moves with the wind every frame.
        // A caster is drawn over the ring's cells, an instance a cell
        // running every blade's test at each vertex (it keeps at most
        // kShadowKeep of a cell's blades, the first ones, scene_grass.glsl's
        // thinning: blade j while j + a hash < kept + 0.5, so its draw stops
        // past them): a draw a run of tiles, its cell rectangle in the model
        // matrix's first row (column, row, columns), which grass has no use
        // for (its blades stand on the field, the node's transform ignored);
        // the shadow pass passes that row on as modelRows[0].
        if (r == 0 && node.castsShadow()) {
            MeshDraw caster = draw;
            caster.shadowOnly = true;
            caster.castsShadow = true;
            caster.shadowMotion = ShadowMotion::Grass;
            const uint32_t blades = static_cast<uint32_t>(ring.blades);
            const uint32_t kept = std::min(blades, static_cast<uint32_t>(std::ceil(blades * GrassNode::kShadowKeep)) + 1u);
            caster.shadowIndexCount = std::min(blade->indexCount, perBlade * kept);
            for (const auto& run : runs) {
                MeshDraw d = caster;
                std::memset(d.push.model, 0, sizeof(d.push.model));
                d.push.model[0] = static_cast<float>(run[0]);
                d.push.model[4] = static_cast<float>(run[1]);
                d.push.model[8] = static_cast<float>(run[2]);
                d.push.model[15] = 1.0f;
                d.instanceCount = static_cast<uint32_t>(run[2]) * static_cast<uint32_t>(run[3]);
                out.push_back(d);
            }
        }
        // The camera's draw: an instance a blade the plan lists, the ring
        // mesh's first blade its vertices.
        draw.indexCount = std::min(blade->indexCount, perBlade);
        planned.push_back({draw, runs, static_cast<uint32_t>(ring.blades), CoverPlan::Ring::Kind::Grass});
    }

    // The herbs: a draw in each of their rings, an instance a plant the plan
    // lists from the herbs' cells in view, mesh_herb.vert building it from
    // its card strips, mesh.frag's SCENE_HERB build cutting them. They cast
    // no shadow (yet): they lie in the grass's and the trees'.
    for (int r = 0; r < GrassNode::kHerbRings; ++r) node.setDrawnHerbs(r, 0);
    if (!herbs) {
        planCoverFar();
        emitPlanned();
        return;
    }
    const float herbTop = node.herbHeight() * 1.4f + 0.2f;
    for (int r = 0; r < GrassNode::kHerbRings; ++r) {
        const GrassNode::HerbRing ring = node.herbRing(r);
        // Seen from this high, all of the far ring is left to the turf
        // (scene_herb_layer.glsl's herbFarSteep): not issued at all.
        if (r == GrassNode::kHerbRings - 1 && lift > 0.5f * ring.outer) continue;
        const GpuMesh* cards = res.mesh(node.id(), static_cast<uint32_t>(GrassNode::kRings + r),
                                        node.geometryGeneration(), node.herbRingMesh(r));
        if (!cards) break;
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
        if (set == VK_NULL_HANDLE) break;

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
        draw.push.pbrParams[3] = static_cast<float>(flags | mesh_flags::kHerb | msaa);
        draw.push.extra[1] = node.subsurface();
        draw.push.extra[3] = 1.0f + 256.0f;   // no atlas
        draw.twoSided = true;
        // (Timed by ring: perf.gpuPasses().)
        static constexpr const char* kHerbLabels[GrassNode::kHerbRings] = {"herb:ring0", "herb:ring1", "herb:ring2"};
        draw.timerLabel = kHerbLabels[r];
        draw.castsShadow = false;
        uint32_t drawn = 0;
        for (const auto& run : runs) drawn += static_cast<uint32_t>(run[2]) * static_cast<uint32_t>(run[3]);
        node.setDrawnHerbs(r, drawn);
        planned.push_back({draw, runs, 1u, CoverPlan::Ring::Kind::Herbs});
    }
    planCoverFar();
    emitPlanned();
}

}  // namespace bro::scene::vk
