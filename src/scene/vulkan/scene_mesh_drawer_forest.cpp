// SceneMeshDrawer's forest path: a ForestNode becomes a handful of draws a
// frame, one per tier and shape (near leaves, leaf clusters or cards, bark of
// 10 and of 5 sides, blooms, clump lobes, crowns), each instancing this frame's draw
// entries for that shape (scene_forest.glsl). Every tree in view picks its
// tiers by its size on screen (ForestNode::treeLod); every shadow tile picks
// again for the trees in its light volume by its texel size. The node's
// specimens and trees live in one storage buffer uploaded once; only the
// entries are written each frame.
//
// Three phases: a node's PLAN (the trees the camera and each shadow tile
// see, at what size), the DETAIL factor that fits the budget (one node's, or
// several nodes' together, prepareForests, so a mixed forest of a node per
// leaf form thins alike), and its DRAWS at that detail.

#include "scene/vulkan/scene_mesh_drawer.h"

#include "scene/forest_node.h"
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
#include <functional>

namespace bro::scene::vk {

namespace {

// Geometry slots of the node's GpuMesh copies.
constexpr uint32_t kLeafSlot = 0x7ff0;
constexpr uint32_t kCardSlot = 0x7ff1;
constexpr uint32_t kClusterSlot = 0x7ff2;

constexpr uint32_t kSegmentsPerEntry = 16;   // bark: segments an instance
constexpr uint32_t kClumpsPerEntry = 16;     // sprites: clumps an instance
constexpr uint32_t kVertsPerClump = 18;      // three lobes of two triangles
constexpr uint32_t kBloomsPerEntry = 16;     // blooms: a card (6 vertices) each
constexpr uint32_t kVertsPerBloom = 6;
// A tree's blooms are drawn while the largest (ForestNode::bloomExtent of
// it) is at least this wide on screen at the crown's nearest point (px): the
// vertex stage culls each bloom under a quarter pixel by its own distance
// (mesh_forest_bloom.vert's FOREST_BLOOM_CULL_PX), so a tree whose largest
// bloom is under this even there has nothing left to draw.
constexpr float kBloomMinPixels = 0.2f;
constexpr int kNearSides = 10;
constexpr int kFarSides = 5;

// The shapes a forest draws, each one draw, in this order: a needle or
// scale node's lobe CORES first, so the leaves they hide fail the depth test;
// the blooms before the clumps and crowns, so the pixels they cover cost
// those nothing.
enum Group { Cores, LeafNear, LeafCard, Bark10, Bark5, Blooms, Clumps, Crowns, kGroups };

struct Entry {
    uint32_t instance, first, end, param;
    float widening, keepEnd, tail, fade;
};
static_assert(sizeof(Entry) == 32);

float floatBits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

struct Groups {
    std::vector<Entry> g[kGroups];
};

// A tree in view of the camera or of a shadow tile: its pixels per specimen
// unit there.
struct Seen {
    uint32_t instance;
    float px;           // at the crown (the leaves and sprites)
    int tile;           // index into the plan's tiles, -1 for the camera
    float barkPx = 0;   // at the whole tree (0: px)
};

// The detail factor that keeps `cost`'s vertices within `budget` millions: 1
// when they fit, else the least (to about 3%) that does.
float fitDetail(const std::function<double(float)>& cost, float budget) {
    if (budget <= 0.0f) return 1.0f;
    const double limit = static_cast<double>(budget) * 1e6;
    if (cost(1.0f) <= limit) return 1.0f;
    // At most kMaxDetail: past it the trees are sprites and trunks, and a
    // budget still unmet is the view's floor, not a reason to strip the
    // nearest tree bare.
    constexpr float kMaxDetail = 8.0f;
    if (cost(kMaxDetail) > limit) return kMaxDetail;
    float lo = 1.0f, hi = 2.0f;
    while (hi < kMaxDetail && cost(hi) > limit) { lo = hi; hi *= 2.0f; }
    hi = std::min(hi, kMaxDetail);
    for (int it = 0; it < 6; ++it) {
        const float mid = std::sqrt(lo * hi);
        if (cost(mid) > limit) lo = mid; else hi = mid;
    }
    return hi;
}

}  // namespace

struct SceneMeshDrawer::ForestPlan {
    ForestNode* node = nullptr;
    ForestNode::Stats stats;
    ForestNode::Lod lod, shadowLod;
    float leafLength = 0.0f;
    int leafVerts = 0, leafCopies = 1, cardVerts = ForestNode::kCardVertices, force = -1;
    bool cores = false;   // the leaves tier draws its lobes' cores (a spray node's)
    bool clusters = false;   // past the card widening a kept leaf is a cluster (Lod::clusters, a broad node's)
    uint32_t bloomBase = 0;  // the blooms' first record in the node's data (ForestNode::bloomRecordBase)
    std::vector<bromath::AABB3> bounds;   // every tree's, world, padded by the sway's reach
    std::vector<Seen> seen;               // the camera's trees
    std::vector<Seen> cast;               // each shadow tile's
    std::vector<int> tileSlots;           // per tile: its slot in the shadow plan
    std::vector<float> seenPx;            // per tree: the camera's px (Seen::px), 0 when unseen

    // A tree's caster in a shadow tile: at the tile's texel size, but never
    // a crown while the camera draws the tree's leaves. A crown caster's disc
    // stands past the crown's middle, shading only its far half: a crown
    // drawn as a crown shades its near half itself (the trace's own
    // leaves), but leaves take their shade from the casters alone, so the
    // near half of the crown went unshaded (a conifer's leaves 30 units
    // brighter than with leaf casters). Clump casters shade lobe by lobe.
    ForestNode::TreeLod casterLod(const ForestNode::Lod& L, const ForestNode::Specimen& sp, const Seen& s) const {
        ForestNode::TreeLod t = ForestNode::treeLod(L, sp, s.px, leafLength, force, s.barkPx);
        if (force >= 0 || t.crowns <= 0.0f || s.instance >= seenPx.size() || seenPx[s.instance] <= 0.0f) return t;
        const ForestNode::TreeLod c = ForestNode::treeLod(lod, sp, seenPx[s.instance], leafLength);
        if (c.leaves < std::max(c.clumps, c.crowns)) return t;
        // (A caster draws its heaviest tier, solid.)
        t.clumps = std::max(t.clumps, 1.0f - t.leaves);
        t.crowns = 0.0f;
        return t;
    }

    // What one tree at level of detail `t` adds to `G` (or, with no `G`, only
    // the vertices it would).
    void addTree(Groups* G, uint32_t inst, const ForestNode::Specimen& sp, const ForestNode::TreeLod& t,
                 bool shadow, uint64_t& vertices, float px = 0.0f) {
        const auto& keepRadius = node->keepRadius();
        float lo[ForestNode::kTiers], hi[ForestNode::kTiers];
        ForestNode::fadeRanges(t, lo, hi);
        if (shadow) {
            // A caster draws one tier, the heaviest, solid.
            const float w[ForestNode::kTiers] = {t.leaves, t.clumps, t.crowns};
            int tier = 0;
            for (int k = 1; k < ForestNode::kTiers; ++k)
                if (w[k] > w[tier]) tier = k;
            for (int k = 0; k < ForestNode::kTiers; ++k) { lo[k] = 0.0f; hi[k] = k == tier ? 1.0f : 0.0f; }
        }
        // Leaves: a prefix of the shuffled run, widened.
        if (hi[0] > lo[0] && sp.leafCount > 0) {
            float widening = t.widening;
            bool card = t.card;
            if (shadow) {
                widening /= std::sqrt(std::max(lod.shadowKeep, 0.01f));
                card = true;
            }
            const float keepF = static_cast<float>(sp.leafCount) / (widening * widening);
            const uint32_t n = std::min(sp.leafCount, static_cast<uint32_t>(std::ceil(keepF)));
            const float tail = std::max(1.0f, 0.25f * keepF);
            const float keepEnd = static_cast<float>(sp.leafStart) + keepF;
            const uint32_t K = static_cast<uint32_t>(card ? ForestNode::kCardCopies : leafCopies);
            // (A cluster, the card in colour with Lod::clusters, is a hexagon.)
            const bool cluster = card && !shadow && clusters;
            const uint32_t V = static_cast<uint32_t>(!card ? leafVerts
                                                     : cluster ? ForestNode::kCardVertices
                                                               : cardVerts);
            // An entry is K leaves' vertices whether all are kept or not.
            vertices += uint64_t{(n + K - 1) / K} * K * V;
            if (G) {
                const float fade = ForestNode::fadeCode(lo[0], hi[0]);
                auto& g = G->g[card ? LeafCard : LeafNear];
                for (uint32_t c = 0; c < n; c += K)
                    g.push_back({inst, sp.leafStart + c, sp.leafStart + std::min(c + K, n), V, widening, keepEnd,
                                 tail, fade});
                if (!shadow) stats.leaves += n;
            }
            // A spray node's leaves hide their lobes' depths: each lobe's
            // core drawn solid first (forestLobeCore), at the leaves' fade.
            if (G && !shadow && cores && sp.clumpCount > 0) {
                const float fade = ForestNode::fadeCode(lo[0], hi[0]);
                vertices += uint64_t{(sp.clumpCount + kClumpsPerEntry - 1) / kClumpsPerEntry} * kClumpsPerEntry *
                            kVertsPerClump;
                for (uint32_t c = 0; c < sp.clumpCount; c += kClumpsPerEntry)
                    G->g[Cores].push_back({inst, sp.clumpStart + c,
                                           sp.clumpStart + std::min(c + kClumpsPerEntry, sp.clumpCount), 0u, 1.0f,
                                           0.0f, 1.0f, fade});
            }
        }
        // Bark: the segments whose keep radius is wide enough, a connected
        // prefix (thickest first).
        if (sp.segCount > 0) {
            const float* k0 = keepRadius.data() + sp.segStart;
            const float* k1 = k0 + sp.segCount;
            const uint32_t n = static_cast<uint32_t>(
                std::upper_bound(k0, k1, t.barkRadius, [](float r, float v) { return r > v; }) - k0);
            const int sides = shadow ? kFarSides : t.barkSides;
            vertices += uint64_t{(n + kSegmentsPerEntry - 1) / kSegmentsPerEntry} * kSegmentsPerEntry *
                        static_cast<uint32_t>(sides) * 6u;
            if (G) {
                auto& g = G->g[sides == kNearSides ? Bark10 : Bark5];
                for (uint32_t c = 0; c < n; c += kSegmentsPerEntry)
                    g.push_back({inst, sp.segStart + c, sp.segStart + std::min(c + kSegmentsPerEntry, n),
                                 static_cast<uint32_t>(sides), 1.0f, 0.0f, 1.0f, 1.0f});
            }
        }
        // Sprites: counted when drawn, not against the budget, which buys
        // leaves and bark (a sprite tree's cost is a fixed few hundred
        // vertices and its pixels).
        if (hi[1] > lo[1] && sp.clumpCount > 0 && G) {
            vertices += uint64_t{(sp.clumpCount + kClumpsPerEntry - 1) / kClumpsPerEntry} * kClumpsPerEntry *
                        kVertsPerClump;
            const float fade = ForestNode::fadeCode(lo[1], hi[1]);
            for (uint32_t c = 0; c < sp.clumpCount; c += kClumpsPerEntry)
                G->g[Clumps].push_back({inst, sp.clumpStart + c,
                                        sp.clumpStart + std::min(c + kClumpsPerEntry, sp.clumpCount), 0u, 1.0f, 0.0f,
                                        1.0f, fade});
        }
        if (hi[2] > lo[2] && sp.clumpCount > 0 && G) {
            vertices += 6;
            G->g[Crowns].push_back({inst, 0u, 1u, 0u, 1.0f, 0.0f, 1.0f, ForestNode::fadeCode(lo[2], hi[2])});
        }
        // Blooms: whatever the tier, while the largest is wide enough to
        // leave a trace; solid (each dwindles into a dot by its own size on
        // screen, mesh_forest_bloom.vert, so there is nothing to cross-fade).
        // Counted when drawn, as the sprites are. They cast no shadow.
        if (G && !shadow && sp.bloomCount > 0 &&
            sp.bloomSize * ForestNode::bloomExtent(sp.bloomForm) * px >= kBloomMinPixels) {
            vertices += uint64_t{(sp.bloomCount + kBloomsPerEntry - 1) / kBloomsPerEntry} * kBloomsPerEntry *
                        kVertsPerBloom;
            for (uint32_t c = 0; c < sp.bloomCount; c += kBloomsPerEntry)
                G->g[Blooms].push_back({inst, sp.bloomStart + c,
                                        sp.bloomStart + std::min(c + kBloomsPerEntry, sp.bloomCount), bloomBase,
                                        1.0f, 0.0f, 1.0f, 1.0f});
            stats.blooms += sp.bloomCount;
        }
    }

    // The vertices the camera's (or the shadow tiles') trees send at `detail`.
    double cost(float detail, bool shadow) {
        if (force >= 0) return 0.0;
        const ForestNode::Lod L = ForestNode::scaled(shadow ? shadowLod : lod, detail);
        const auto& specimens = node->specimens();
        const auto& instances = node->instances();
        uint64_t v = 0;
        for (const Seen& s : shadow ? cast : seen) {
            const ForestNode::Specimen& sp = specimens[instances[s.instance].specimen];
            addTree(nullptr, s.instance, sp,
                    shadow ? casterLod(L, sp, s) : ForestNode::treeLod(L, sp, s.px, leafLength, force, s.barkPx),
                    shadow, v);
        }
        return static_cast<double>(v);
    }
};

std::unique_ptr<SceneMeshDrawer::ForestPlan> SceneMeshDrawer::planForest(SceneFrame& frame, ForestNode& node) {
    if (node.instanceCount() == 0 || node.specimenCount() == 0 || node.mesh().empty() || node.effectiveUnlit()) {
        node.setStats({});
        return nullptr;
    }
    auto plan = std::make_unique<ForestPlan>();
    ForestPlan& P = *plan;
    P.node = &node;
    const SceneView& view = frame.view;
    const bromath::Mat4& world = node.worldMatrix();
    const float nodeScale = std::sqrt(world.data[4] * world.data[4] + world.data[5] * world.data[5] +
                                      world.data[6] * world.data[6]);
    // Pixels per metre at a metre's distance (perspective), or flat (ortho).
    const float pxAtMetre = std::fabs(view.proj.at(1, 1)) * static_cast<float>(view.height) * 0.5f;
    P.lod = node.lod();
    // A shadow is a silhouette: its crowns take over from the clumps at
    // twice the size (in texels) the camera's do.
    P.shadowLod = P.lod;
    P.shadowLod.crownFrom *= 2.0f;
    P.shadowLod.crownTo *= 2.0f;
    P.leafLength = node.leafLength();
    P.leafVerts = static_cast<int>(node.mesh().vertexCount());
    P.leafCopies = node.leafCopies();
    P.cardVerts = node.cardVertices();
    P.force = node.forceTier();
    P.cores = node.look().leafForm != 0;
    // A spray node's thinned leaves stay sprays, each widened (its needles
    // cut out as the near ones'), not clusters of broad leaves.
    P.clusters = P.lod.clusters > 0.5f && node.look().leafForm == 0;
    const auto& specimens = node.specimens();
    const auto& instances = node.instances();

    float maxHeight = 0.0f;
    for (const auto& s : specimens) maxHeight = std::max(maxHeight, s.height);
    float maxScale = 0.0f;
    for (const auto& in : instances) maxScale = std::max(maxScale, in.scale);
    const float reach = frame.renderer.vegetationReach(maxHeight * maxScale, world);

    // Every tree's world bounds once.
    P.bounds.resize(instances.size());
    for (size_t i = 0; i < instances.size(); ++i) {
        P.bounds[i] = bromath::atransform(instances[i].bounds, world);
        P.bounds[i].min -= bromath::Vec3{reach, reach, reach};
        P.bounds[i].max += bromath::Vec3{reach, reach, reach};
    }

    // The camera's trees.
    P.seen.reserve(instances.size());
    for (uint32_t i = 0; i < instances.size(); ++i) {
        const bromath::AABB3& b = P.bounds[i];
        if (!bromath::fintersects(view.frustum, b)) {
            P.stats.culled++;
            continue;
        }
        const ForestNode::Instance& in = instances[i];
        const ForestNode::Specimen& sp = specimens[in.specimen];
        // The bark's distance: to the nearest point of the tree's box at
        // rest (the wind's strength does not move the choice).
        const bromath::Vec3 e = view.eye;
        const bromath::Vec3 q{std::clamp(e.x, b.min.x + reach, b.max.x - reach),
                              std::clamp(e.y, b.min.y + reach, b.max.y - reach),
                              std::clamp(e.z, b.min.z + reach, b.max.z - reach)};
        const float d = std::max(bromath::vlen(q - e), 0.25f);
        if (P.lod.range > 0.0f && d > P.lod.range) {
            P.stats.culled++;
            continue;
        }
        // The leaves': to the crown, an upright cylinder round its clumps,
        // so a crown overhead is as near as its underside, not the trunk
        // beside the eye.
        float dl = d;
        if (sp.clumpCount > 0) {
            const bromath::Vec3 local{sp.crown.x * in.cosYaw + sp.crown.z * in.sinYaw, sp.crown.y,
                                      -sp.crown.x * in.sinYaw + sp.crown.z * in.cosYaw};
            const bromath::Vec3 c = bromath::mtransformPoint(world, in.position + local * in.scale);
            const float k = in.scale * nodeScale / 0.92f;
            const float dh = std::max(0.0f, std::hypot(e.x - c.x, e.z - c.z) - sp.crownRadius * k);
            const float dv = std::max(0.0f, std::fabs(e.y - c.y) - sp.crownRadiusUp * k);
            dl = std::max(std::hypot(dh, dv), d);
        }
        const float unit = in.scale * nodeScale;
        P.seen.push_back({i, (view.perspective ? pxAtMetre / dl : pxAtMetre) * unit, -1,
                          (view.perspective ? pxAtMetre / d : pxAtMetre) * unit});
    }

    P.seenPx.assign(instances.size(), 0.0f);
    for (const Seen& s : P.seen) P.seenPx[s.instance] = s.px;

    // Each shadow tile's trees, at its texel size.
    const ShadowPlan& shadows = frame.renderer.shadowPlan();
    if (node.castsShadow() && frame.gpu.targets.shadowAtlas.width == static_cast<uint32_t>(shadows.atlasSize)) {
        for (int slot = 0; slot < shadows.tileCount && slot < 32; ++slot) {
            const ShadowTilePlan& tile = shadows.tiles[slot];
            if (!tile.render) continue;
            const int ti = static_cast<int>(P.tileSlots.size());
            P.tileSlots.push_back(slot);
            for (uint32_t i = 0; i < instances.size(); ++i) {
                if (!bromath::fintersects(tile.frustum, P.bounds[i])) continue;
                float texel = tile.texelConst;
                if (!tile.ortho) {
                    const bromath::Vec3 c = (P.bounds[i].min + P.bounds[i].max) * 0.5f;
                    texel += tile.texelPerMetre * bromath::vlen(c - view.eye);
                }
                P.cast.push_back({i, instances[i].scale * nodeScale / std::max(texel, 1e-5f), ti});
            }
        }
    }
    return plan;
}

void SceneMeshDrawer::prepareForest(SceneFrame& frame, ForestNode& node, std::vector<MeshDraw>& out) {
    out.clear();
    auto plan = planForest(frame, node);
    if (!plan) return;
    ForestPlan& P = *plan;
    const float detail = P.force >= 0 || P.seen.empty()
                             ? 1.0f : fitDetail([&](float d) { return P.cost(d, false); }, P.lod.budget);
    const float shadowDetail = P.force >= 0 || P.cast.empty()
                                   ? 1.0f : fitDetail([&](float d) { return P.cost(d, true); }, P.lod.shadowBudget);
    finishForest(frame, P, detail, shadowDetail, out);
}

void SceneMeshDrawer::prepareForests(SceneFrame& frame, const std::vector<ForestNode*>& nodes,
                                     std::vector<std::vector<MeshDraw>>& out) {
    out.assign(nodes.size(), {});
    std::vector<std::unique_ptr<ForestPlan>> plans(nodes.size());
    float budget = 0.0f, shadowBudget = 0.0f;
    bool forced = false, anySeen = false, anyCast = false;
    for (size_t i = 0; i < nodes.size(); ++i) {
        plans[i] = planForest(frame, *nodes[i]);
        if (!plans[i]) continue;
        const ForestPlan& P = *plans[i];
        // One budget for them all: the largest any asks for (0, none, if
        // any asks for none).
        budget = P.lod.budget <= 0.0f || budget < 0.0f ? -1.0f : std::max(budget, P.lod.budget);
        shadowBudget = P.lod.shadowBudget <= 0.0f || shadowBudget < 0.0f ? -1.0f
                                                                          : std::max(shadowBudget, P.lod.shadowBudget);
        forced = forced || P.force >= 0;
        anySeen = anySeen || !P.seen.empty();
        anyCast = anyCast || !P.cast.empty();
    }
    const auto total = [&](float d, bool shadow) {
        double v = 0.0;
        for (auto& p : plans)
            if (p) v += p->cost(d, shadow);
        return v;
    };
    const float detail = forced || !anySeen || budget <= 0.0f
                             ? 1.0f : fitDetail([&](float d) { return total(d, false); }, budget);
    const float shadowDetail = forced || !anyCast || shadowBudget <= 0.0f
                                   ? 1.0f : fitDetail([&](float d) { return total(d, true); }, shadowBudget);
    for (size_t i = 0; i < nodes.size(); ++i)
        if (plans[i]) finishForest(frame, *plans[i], detail, shadowDetail, out[i]);
}

void SceneMeshDrawer::finishForest(SceneFrame& frame, ForestPlan& P, float detail, float shadowDetail,
                                   std::vector<MeshDraw>& out) {
    out.clear();
    ForestNode& node = *P.node;
    ForestNode::Stats& stats = P.stats;
    const SceneView& view = frame.view;
    const bromath::Mat4& world = node.worldMatrix();
    const ForestNode::Lod& lod = P.lod;
    const auto& specimens = node.specimens();
    const auto& instances = node.instances();
    SceneGpuResources& res = frame.gpu.resources;
    const std::vector<float>& data = node.gpuData();
    const SceneVkBuffer* store = res.storage(node.id(), node.dataGeneration(), data.data(), data.size() * sizeof(float));
    const GpuMesh* leafMesh = res.mesh(node.id(), kLeafSlot, node.geometryGeneration(), node.leafBatch());
    const uint64_t cardGen = node.cardGeneration();
    const GpuMesh* cardMesh = res.mesh(node.id(), kCardSlot, cardGen, node.cardBatch());
    const GpuMesh* clusterMesh = res.mesh(node.id(), kClusterSlot, node.geometryGeneration(), node.clusterBatch());
    if (!store || !leafMesh || !cardMesh || !clusterMesh) {
        node.setStats({});
        return;
    }

    // The camera's trees at the detail.
    Groups colour;
    uint64_t colourVerts = 0;
    stats.detail = detail;
    P.bloomBase = node.bloomRecordBase();
    {
        const ForestNode::Lod L = ForestNode::scaled(lod, detail);
        for (const Seen& s : P.seen) {
            const ForestNode::Specimen& sp = specimens[instances[s.instance].specimen];
            const ForestNode::TreeLod t = ForestNode::treeLod(L, sp, s.px, P.leafLength, P.force, s.barkPx);
            if (t.leaves > 0.0f) stats.trees[0]++;
            if (t.clumps > 0.0f) stats.trees[1]++;
            if (t.crowns > 0.0f) stats.trees[2]++;
            P.addTree(&colour, s.instance, sp, t, false, colourVerts, s.px);
        }
    }
    stats.vertices = colourVerts;
    // Clumps and crowns nearest the eye first: each writes the depth of the
    // leaves it shows, so one behind a crown already drawn fails the early
    // depth test rather than placing its leaves for nothing.
    const auto nearestFirst = [&](std::vector<Entry>& g) {
        std::vector<std::pair<float, Entry>> keyed;
        keyed.reserve(g.size());
        for (const Entry& e : g)
            keyed.push_back({bromath::vlen((P.bounds[e.instance].min + P.bounds[e.instance].max) * 0.5f - view.eye),
                             e});
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < g.size(); ++i) g[i] = keyed[i].second;
    };
    nearestFirst(colour.g[Cores]);
    // So do the leaves, tree by tree (a tree's run of entries stays in its
    // order): the sprays of a crown behind a nearer one fail the depth test
    // rather than being shaded first and covered.
    nearestFirst(colour.g[LeafNear]);
    nearestFirst(colour.g[LeafCard]);
    nearestFirst(colour.g[Blooms]);
    nearestFirst(colour.g[Clumps]);
    nearestFirst(colour.g[Crowns]);

    // Each shadow tile's trees at the shadow detail.
    std::vector<Groups> tiles(P.tileSlots.size());
    stats.shadowDetail = shadowDetail;
    {
        const ForestNode::Lod L = ForestNode::scaled(P.shadowLod, shadowDetail);
        for (const Seen& s : P.cast) {
            const ForestNode::Specimen& sp = specimens[instances[s.instance].specimen];
            P.addTree(&tiles[static_cast<size_t>(s.tile)], s.instance, sp, P.casterLod(L, sp, s), true,
                      stats.shadowVertices);
        }
    }

    // One buffer of entries: the camera's groups, then each tile's.
    size_t total = 0;
    for (const auto& g : colour.g) total += g.size();
    for (const auto& t : tiles)
        for (const auto& g : t.g) total += g.size();
    stats.entries = static_cast<uint32_t>(total);
    if (total == 0) {
        node.setStats(stats);
        return;
    }
    const size_t bytes = total * sizeof(Entry);
    const render::UploadSlice slice = frame.gpu.device.frameUpload(bytes);
    if (!slice) {
        node.setStats(stats);
        return;
    }
    // Entries go up as uint bits and floats, the layout scene_forest.glsl reads.
    auto* dst = static_cast<float*>(slice.mapped);
    size_t at = 0;
    const auto put = [&](const std::vector<Entry>& g) {
        const size_t first = at;
        for (const Entry& e : g) {
            float* d = dst + at * 8;
            d[0] = floatBits(e.instance);
            d[1] = floatBits(e.first);
            d[2] = floatBits(e.end);
            d[3] = floatBits(e.param);
            d[4] = e.widening;
            d[5] = e.keepEnd;
            d[6] = e.tail;
            d[7] = e.fade;
            ++at;
        }
        return static_cast<uint32_t>(first);
    };

    VkDescriptorSet set = frame.gpu.device.frameSet(defaults_->boneLayout);
    if (set == VK_NULL_HANDLE) {
        node.setStats(stats);
        return;
    }
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(1, store->buffer, store->size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.writeBuffer(2, slice.buffer, bytes, slice.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(device_, set);

    // The draws' shared state: the leaves' material, the forest flag.
    MeshDraw base;
    base.nodeId = node.id();
    base.vertexSet = set;
    std::memcpy(base.push.model, view.relative(world).data, sizeof(base.push.model));
    std::memcpy(base.push.baseColor, node.color(), sizeof(base.push.baseColor));
    base.push.baseColor[3] = 1.0f;
    base.push.pbrParams[0] = node.metallic();
    base.push.pbrParams[1] = node.roughness();
    base.push.pbrParams[2] = node.alphaCutoff();
    base.push.extra[0] = node.nearClipDist();
    base.push.extra[1] = node.subsurface();
    base.push.extra[2] = 0.0f;
    base.push.extra[3] = 1.0f + 256.0f;   // no atlas
    uint32_t flags = mesh_flags::kForest | mesh_flags::vertexColor(node.vertexColorMode());
    // In a 4x target a hand-over splits each pixel's samples, a blend in
    // quarters rather than a stipple of whole pixels.
    const uint32_t msaa = frame.gpu.targets.samples() == VK_SAMPLE_COUNT_4_BIT ? mesh_flags::kForestMsaa : 0u;
    flags |= msaa;
    if (frame.ssr) flags |= mesh_flags::kReflectance;
    if (node.receivesShadow()) flags |= mesh_flags::kReceivesShadow;
    uint32_t leafFlags = flags | mesh_flags::kTwoSided;
    if (node.mesh().hasTangents()) leafFlags |= mesh_flags::kHasTangents;
    const uint32_t barkFlags =
        mesh_flags::kForest | msaa | (flags & (mesh_flags::kReflectance | mesh_flags::kReceivesShadow));
    const uint32_t spriteFlags = barkFlags | mesh_flags::kForestSprite;
    // The leaves' maps (an albedo with alpha is the usual leaf); sprites and
    // bark take the plain colour.
    VkDescriptorSet leafMaterial = VK_NULL_HANDLE;
    {
        struct Images { const SceneVkImage* albedo; const SceneVkImage* normal; } im{
            res.texture(node.id(), TextureSlot::BaseColor, node.baseColorTexture()),
            res.texture(node.id(), TextureSlot::Normal, node.normalTexture())};
        if (im.albedo || im.normal) {
            leafMaterial = frame.gpu.device.frameSet(defaults_->materialLayout);
            SceneVkDescriptorWriter w;
            w.writeImage(0, im.albedo ? im.albedo->view : defaults_->white.view,
                         im.albedo ? im.albedo->sampler : defaults_->sampler);
            w.writeImage(1, im.normal ? im.normal->view : defaults_->flatNormal.view,
                         im.normal ? im.normal->sampler : defaults_->sampler);
            w.writeImage(2, defaults_->white.view, defaults_->sampler);
            w.writeImage(3, defaults_->black.view, defaults_->sampler);
            w.writeImage(4, defaults_->white.view, defaults_->sampler);
            w.updateSet(device_, leafMaterial);
            if (im.albedo) leafFlags |= mesh_flags::kAlbedoMap;
            if (im.normal) leafFlags |= mesh_flags::kNormalMap;
        }
    }

    const auto emit = [&](const Groups& G, bool shadow, int slot) {
        for (int k = 0; k < kGroups; ++k) {
            const auto& g = G.g[k];
            if (g.empty()) continue;
            const uint32_t first = put(g);
            MeshDraw d = base;
            d.shadowOnly = shadow;
            d.castsShadow = shadow;
            if (shadow) d.shadowTileMask = 1u << slot;
            switch (k) {
            case LeafNear:
            case LeafCard: {
                // In colour, past the card widening, each kept leaf is a
                // cluster of leaves (Lod::clusters); a caster stays a card.
                const bool cluster = k == LeafCard && !shadow && P.clusters;
                const GpuMesh* m = k == LeafNear ? leafMesh : cluster ? clusterMesh : cardMesh;
                d.kind = MeshKind::ForestLeaf;
                d.vertices = m->vertices.buffer;
                d.indices = m->indices.buffer;
                d.indexCount = m->indexCount;
                d.instanceCount = static_cast<uint32_t>(g.size());
                d.firstInstance = first;
                d.twoSided = true;
                d.materialSet = leafMaterial;
                // A needle or scale-leaf node's leaf is its spray's card, its
                // needles cut out by mesh.frag's SCENE_FOREST_SPRAY build.
                const bool spray = !cluster && node.look().leafForm != 0;
                // A broad form's own card near: its outline cut per pixel,
                // rounded and pointed (Look::leafCut; untextured: a
                // texture's alpha is its outline).
                const bool cut = k == LeafNear && node.look().leafForm == 0 && node.look().leafCut &&
                                 (leafFlags & mesh_flags::kAlbedoMap) == 0u;
                d.push.pbrParams[3] = static_cast<float>(leafFlags | (cluster ? mesh_flags::kForestCluster : 0u) |
                                                         (spray ? mesh_flags::kForestSpray : 0u) |
                                                         (cut ? mesh_flags::kForestLeafCut : 0u));
                break;
            }
            case Bark10:
            case Bark5: {
                const int sides = k == Bark10 ? kNearSides : kFarSides;
                d.kind = MeshKind::ForestTube;
                d.indexCount = kSegmentsPerEntry * static_cast<uint32_t>(sides) * 6u;
                d.instanceCount = static_cast<uint32_t>(g.size());
                d.firstInstance = first;
                const float* bc = node.barkColor();
                d.push.baseColor[0] = bc[0];
                d.push.baseColor[1] = bc[1];
                d.push.baseColor[2] = bc[2];
                d.push.baseColor[3] = 1.0f;
                d.push.pbrParams[1] = node.barkRoughness();
                d.push.extra[1] = 0.0f;
                d.push.pbrParams[3] = static_cast<float>(barkFlags);
                break;
            }
            case Cores:
            case Clumps:
                d.kind = MeshKind::ForestSprite;
                d.push.extra[2] = k == Cores ? 2.0f : 0.0f;
                d.indexCount = kClumpsPerEntry * kVertsPerClump;
                d.instanceCount = static_cast<uint32_t>(g.size());
                d.firstInstance = first;
                d.twoSided = true;   // no culling; lit from its front
                d.push.pbrParams[3] = static_cast<float>(spriteFlags);
                break;
            case Blooms:
                // A card a bloom (mesh_forest_bloom.vert), shaped and lit by
                // mesh.frag's SCENE_FOREST_BLOOM build: white times the
                // instance colour, which is the bloom's own; petals matte.
                d.kind = MeshKind::ForestSprite;
                d.indexCount = kBloomsPerEntry * kVertsPerBloom;
                d.instanceCount = static_cast<uint32_t>(g.size());
                d.firstInstance = first;
                d.twoSided = true;
                d.push.baseColor[0] = d.push.baseColor[1] = d.push.baseColor[2] = d.push.baseColor[3] = 1.0f;
                d.push.pbrParams[0] = 0.0f;
                d.push.pbrParams[1] = 0.8f;
                d.push.pbrParams[2] = 0.0f;
                d.push.extra[1] = 0.0f;
                d.push.pbrParams[3] =
                    static_cast<float>(barkFlags | mesh_flags::kTwoSided | mesh_flags::kForestBloom);
                break;
            case Crowns:
                d.kind = MeshKind::ForestSprite;
                d.push.extra[2] = 1.0f;
                d.indexCount = static_cast<uint32_t>(g.size()) * 6u;
                d.instanceCount = 1;
                d.firstVertex = first * 6u;
                d.twoSided = true;
                d.push.pbrParams[3] = static_cast<float>(spriteFlags);
                break;
            default: break;
            }
            out.push_back(d);
            stats.draws++;
        }
    };
    emit(colour, false, 0);
    for (size_t t = 0; t < tiles.size(); ++t) emit(tiles[t], true, P.tileSlots[t]);
    node.setStats(stats);
}

}  // namespace bro::scene::vk
