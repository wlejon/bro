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
#include "util/log.h"

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
// The wood's index patterns (kTubeSlot + sides; + 0 the twigs', + 1 the
// fine twigs'; kTubeCurveSlot + sides the curved bark's).
constexpr uint32_t kTubeSlot = 0x7fe0;
constexpr uint32_t kTubeCurveSlot = 0x7fc0;

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
// Bark's sides (ForestNode::TreeLod::barkSides): 20 for a trunk many pixels
// wide, 10, or 5; each ring morphs between them by its own width.
constexpr int kNearestSides = 20;
constexpr int kNearSides = 10;
constexpr int kFarSides = 5;
// The rows of rings a CURVED segment of bark is drawn in, along the curve
// through its ends (scene_forest_tube.glsl, the entry's param / 256): a
// segment at least kBarkCurvePixels in radius on screen at its tree's
// nearest point, of a tree drawing 20 or 10 sides. Thinner wood (most of a
// near conifer's whorls), and every far tree's, is the one straight row
// between its ends: its bend inside a segment is a pixel or so, and rows
// on all of it more than doubled the trunks' view's vertices.
constexpr uint32_t barkRows(int sides) { return sides >= 20 ? 3u : sides >= 10 ? 2u : 1u; }
constexpr float kBarkCurvePixels = 6.0f;
// Twigs (a broad node's, scene_forest_shoot.glsl): pieces an instance, each
// a ribbon of 6 indices (4 corners, woodIndices; the budget counts the 6);
// a clump's pieces take as many entries as they
// need. A tree draws its lobe stems and sprays while their twigs are at
// least kTwigPixels wide at the crown's nearest point, and its shoots while
// theirs are (the vertex stage fades each piece out by its own width,
// mesh_forest_tube.vert's FOREST_TWIG_PX, so nothing pops there); and only
// while its leaves are leaves, not clusters: they fade out over
// kTwigWidening times the card widening.
constexpr uint32_t kTwigPiecesPerEntry = 64;
constexpr uint32_t kVertsPerTwig = 6;
// Bark down to this share of the round bark's cut (Lod::barkPixels, 0.7 px:
// 0.35 px) is drawn as ribbons covering their width. Down to 0.1 px took
// the flight's twigs 0.35 -> 2.5 ms a frame; at 0.35 px 0.6 ms, keeping
// most of the canopy's edges (40-80 m hard, noon 7.2 -> 6.9).
constexpr float kBarkRibbonCut = 0.5f;
// A clump at least kTwigFinePixels across (its radius) on screen at its
// nearest point draws its twigs FINE: each piece kTwigFineRows quads along
// the curve through its joints (forestHermite), with a collar where a twig
// leaves its wood; further off a piece is the one quad between its joints
// (its stems' pieces a few pixels long, the curve's bend inside one under
// a pixel). By the clump, not its leaf: a clump of small leaves has stems
// as long as any, a polyline of long straight runs at the leaf's measure.
constexpr uint32_t kTwigFineRows = 3;
// (A piece is about a quarter of its clump's radius; its chords turn about
// 13 degrees off its joints' ways, so the curve stands about 3% of the piece
// off its chord: a pixel at 120.)
constexpr float kTwigFinePixels = 120.0f;
constexpr float kTwigFineClumpLeaves = 40.0f;
constexpr float kTwigPixels = 0.15f;
constexpr float kTwigStemRadius = 0.04f;    // leaf lengths (forestPipe: a lobe stem's start)
constexpr float kTwigShootRadius = 0.011f;  // a shoot's at its base
constexpr float kTwigWidening[2] = {0.95f, 1.3f};

// The wood draws indexed, so the vertex stage runs once a corner, not once a
// triangle's corner (scene_forest_tube.glsl's vertex numbering): bark of
// `sides` sides is barkRows + 1 rings of sides + 1 vertices a segment, each
// side two triangles between each two; a twig (sides 0) `rows` + 1 rows of its two sides
// a piece (four corners for the plain twigs' one row). Unindexed, a bark
// side's ring points were each worked out three times, a twig's ends (the
// shoot model, posed) three times over.
const bromesh::MeshData& woodIndices(int sides, uint32_t rows = 1) {
    static bromesh::MeshData twigs, fineTwigs, bark[3], curved[3];
    bromesh::MeshData& m = sides == 0 ? (rows > 1 ? fineTwigs : twigs)
                                      : (rows > 1 ? curved : bark)[sides == 5 ? 0 : sides == 10 ? 1 : 2];
    if (!m.indices.empty()) return m;
    if (sides == 0) {
        const uint32_t per = 2 * (rows + 1);
        for (uint32_t p = 0; p < kTwigPiecesPerEntry; ++p)
            for (uint32_t r = 0; r < rows; ++r)
                for (uint32_t c : {0u, 1u, 2u, 2u, 1u, 3u}) m.indices.push_back(per * p + 2 * r + c);
        m.positions.assign(3 * per * kTwigPiecesPerEntry, 0.0f);
        return m;
    }
    const uint32_t ring = static_cast<uint32_t>(sides) + 1, bRows = rows;
    for (uint32_t k = 0; k < kSegmentsPerEntry; ++k)
        for (uint32_t r = 0; r < bRows; ++r) {
            const uint32_t b = (k * (bRows + 1) + r) * ring, t = b + ring;
            for (uint32_t s = 0; s < static_cast<uint32_t>(sides); ++s)
                for (uint32_t v : {b + s, b + s + 1, t + s, t + s, b + s + 1, t + s + 1}) m.indices.push_back(v);
        }
    m.positions.assign(3 * (bRows + 1) * ring * kSegmentsPerEntry, 0.0f);
    return m;
}

// The shapes a forest draws, each one draw, in this order: a needle or
// scale node's lobe CORES first, so the leaves they hide fail the depth test;
// the blooms before the clumps and crowns, so the pixels they cover cost
// those nothing.
enum Group {
    Cores, LeafNear, LeafCard, Bark20Curved, Bark10Curved, Bark20, Bark10, Bark5, Twigs, TwigsFine, Blooms, Clumps,
    Crowns, kGroups
};

struct Entry {
    uint32_t instance, first, end, param;
    float widening, keepEnd, tail, fade;
};
static_assert(sizeof(Entry) == 32);
// An entry's floats on the GPU: its 8, then its pose slot and padding
// (scene_forest.glsl's three records).
constexpr size_t kEntryFloats = 12;
// The most leaves posed a frame (64 bytes each: 256 MB). The flight's
// heaviest views keep about 1.5 million.
constexpr uint32_t kMaxPoses = 4u << 20;

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
    bool forced = true; // within the forced tier's range (Lod::forceRange)
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
    bool twigs = false;      // its leaves hang from twigs drawn as such (a broad node's)
    uint32_t bloomBase = 0;  // the blooms' first record in the node's data (ForestNode::bloomRecordBase)
    std::vector<bromath::AABB3> bounds;   // every tree's, world, padded by the sway's reach
    std::vector<Seen> seen;               // the camera's trees
    std::vector<Seen> cast;               // each shadow tile's
    std::vector<int> tileSlots;           // per tile: its slot in the shadow plan
    std::vector<float> seenPx;            // per tree: the camera's px (Seen::px), 0 when unseen
    float canopyTop = -1e30f;             // the highest tree top within 40 m of the eye (across the ground)
    // The view, for a clump's own size on screen (fine twigs).
    bromath::Vec3 eye{};
    bromath::Mat4 world;
    float pxAtMetre = 0.0f, nodeScale = 1.0f;
    bool perspective = true;

    // Whether clump `c` of tree `inst` draws its twigs fine (kTwigFinePixels).
    bool fineTwigs(uint32_t inst, uint32_t c) const {
        const auto& rec = node->clumpRecords();
        if (c * 8 + 3 >= rec.size()) return false;
        const ForestNode::Instance& in = node->instances()[inst];
        const float* q = &rec[c * 8];
        const bromath::Vec3 local{q[0] * in.cosYaw + q[2] * in.sinYaw, q[1], -q[0] * in.sinYaw + q[2] * in.cosYaw};
        const bromath::Vec3 w = bromath::mtransformPoint(world, in.position + local * in.scale);
        const float unit = in.scale * nodeScale;
        const float d = std::max(bromath::vlen(w - eye) - q[3] * unit, 0.25f);
        return q[3] * (perspective ? pxAtMetre / d : pxAtMetre) * unit >= kTwigFinePixels;
    }

    // A tree's caster in a shadow tile: at the tile's texel size, but never
    // a crown while the camera draws the tree's leaves. A crown caster's disc
    // stands past the crown's middle, shading only its far half: a crown
    // drawn as a crown shades its near half itself (the trace's own
    // leaves), but leaves take their shade from the casters alone, so the
    // near half of the crown went unshaded (a conifer's leaves 30 units
    // brighter than with leaf casters). Clump casters shade lobe by lobe.
    // The tier a tree is forced to, -1 for none (by size).
    int forceOf(const Seen& s) const { return s.forced ? force : -1; }

    ForestNode::TreeLod casterLod(const ForestNode::Lod& L, const ForestNode::Specimen& sp, const Seen& s) const {
        ForestNode::TreeLod t = ForestNode::treeLod(L, sp, s.px, leafLength, forceOf(s), s.barkPx);
        if (forceOf(s) >= 0 || s.instance >= seenPx.size() || seenPx[s.instance] <= 0.0f) return t;
        const ForestNode::TreeLod c = ForestNode::treeLod(lod, sp, seenPx[s.instance], leafLength);
        // Where the camera draws a tree's leaves, its caster keeps leaves
        // (at the tile's widening) for that share rather than clumps: a leaf
        // takes its shade from the casters alone, and a clump's disc stands
        // past its lobe's middle, so leaves shaded by clump casters had only
        // the far lobes' shade (a broadleaf at 45 / 75 m golden 6 / 11
        // levels brighter than its leaves).
        const float back = std::min(t.clumps, std::max(c.leaves - t.leaves, 0.0f));
        t.clumps -= back;
        t.leaves += back;
        if (t.crowns <= 0.0f) return t;
        // Where the camera sees leaves or clumps, the caster's crowns hand to
        // clumps, over the camera's share of the two from 0.35 to 0.65 (a
        // switch at a half stepped the shadow). Clumps shaded by crown
        // casters had only the far half's shade: a pine from above at 80 m
        // 7 levels brighter than its leaves at noon, 11 at golden.
        const float q = std::clamp((c.leaves + c.clumps - 0.35f) / 0.3f, 0.0f, 1.0f);
        const float m = q * q * (3.0f - 2.0f * q);
        t.clumps += m * t.crowns;
        t.crowns *= 1.0f - m;
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
            // A caster draws each tier at its share (lo 0, hi the share):
            // the leaves keep that share of their casters, a sprite stops
            // that share of the light it would (shadow_forest_sprite.vert).
            // One tier at a time, the heaviest, switched the shadow whole
            // where two tiers' shares crossed: a step in a tree's shade at
            // each handover.
            const float w[ForestNode::kTiers] = {t.leaves, t.clumps, t.crowns};
            for (int k = 0; k < ForestNode::kTiers; ++k) { lo[k] = 0.0f; hi[k] = std::clamp(w[k], 0.0f, 1.0f); }
        }
        // Leaves: a prefix of the shuffled run, widened.
        if (hi[0] > lo[0] && sp.leafCount > 0) {
            // Near leaves hand over to cards (clusters) through a narrow band
            // of widening (TreeLod::cardShare), the two splitting the kept
            // run (a shuffled prefix, so each part a random share of the
            // crown): switched whole, the crown stepped in brightness where
            // they met. Each leaf is drawn once, as one form or the other
            // (both forms over the whole run, dithered apart, doubled the
            // leaves' vertex work through the band: 8 ms flight frames
            // under the canopy). A caster is cards.
            const float cardShare = shadow ? 1.0f : std::clamp(t.cardShare, 0.0f, 1.0f);
            float widening = t.widening;
            if (shadow) widening /= std::sqrt(std::max(lod.shadowKeep, 0.01f));
            const float keepF = static_cast<float>(sp.leafCount) / (widening * widening) * (shadow ? hi[0] : 1.0f);
            const uint32_t n = std::min(sp.leafCount, static_cast<uint32_t>(std::ceil(keepF)));
            const float tail = std::max(1.0f, 0.25f * keepF);
            const float keepEnd = static_cast<float>(sp.leafStart) + keepF;
            const uint32_t nearN =
                std::min(n, static_cast<uint32_t>(std::lround(static_cast<float>(n) * (1.0f - cardShare))));
            for (int form = 0; form < 2; ++form) {
                const bool card = form == 1;
                const uint32_t from = card ? nearN : 0u, to = card ? n : nearN;
                if (to <= from) continue;
                const uint32_t K = static_cast<uint32_t>(card ? ForestNode::kCardCopies : leafCopies);
                // (A cluster, the card in colour with Lod::clusters, is a hexagon.)
                const bool cluster = card && !shadow && clusters;
                const uint32_t V = static_cast<uint32_t>(!card ? leafVerts
                                                         : cluster ? ForestNode::kCardVertices
                                                                   : cardVerts);
                // An entry is K leaves' vertices whether all are kept or not.
                vertices += uint64_t{(to - from + K - 1) / K} * K * V;
                if (G) {
                    const float fade = ForestNode::fadeCode(lo[0], hi[0]);
                    auto& g = G->g[card ? LeafCard : LeafNear];
                    for (uint32_t c = from; c < to; c += K)
                        g.push_back({inst, sp.leafStart + c, sp.leafStart + std::min(c + K, to), V, widening, keepEnd,
                                     tail, fade});
                    if (!shadow) stats.leaves += to - from;
                }
            }
            // A broad node's twigs, which its leaves hang from, at the
            // leaves' fade (colour only: a twig is under a shadow texel).
            if (!shadow && twigs && sp.clumpCount > 0) {
                const float stemPx = 2.0f * kTwigStemRadius * leafLength * px;
                const float shootPx = 2.0f * kTwigShootRadius * leafLength * px;
                const float w0 = lod.cardWidening * kTwigWidening[0], w1 = lod.cardWidening * kTwigWidening[1];
                // By the widening the tree's size asks for, forced tier or
                // not, so a forced Leaves tier draws the twigs By size does.
                const float wide = std::max(t.widening, lod.leafPixels / std::max(leafLength * px, 1e-6f));
                const float q = std::clamp((wide - w0) / std::max(w1 - w0, 1e-3f), 0.0f, 1.0f);
                const float keep = 1.0f - q * q * (3.0f - 2.0f * q);
                if (stemPx >= kTwigPixels && keep > 0.0f) {
                    const bool shoots = shootPx >= kTwigPixels;
                    // Only a tree whose crown's nearest point is that near
                    // has a clump near enough to draw fine.
                    // (A clump is under kTwigFineClumpLeaves leaves across.)
                    const bool mayFine = kTwigFineClumpLeaves * leafLength * px >= kTwigFinePixels;
                    const auto& twigsOf = node->clumpTwigs();
                    const float fade = ForestNode::fadeCode(lo[0], hi[0]);
                    for (uint32_t c = sp.clumpStart; c < sp.clumpStart + sp.clumpCount && c < twigsOf.size(); ++c) {
                        const uint32_t n = shoots ? twigsOf[c].pieces : twigsOf[c].stems;
                        const bool fine = mayFine && fineTwigs(inst, c);
                        vertices += uint64_t{(n + kTwigPiecesPerEntry - 1) / kTwigPiecesPerEntry} *
                                    kTwigPiecesPerEntry * kVertsPerTwig * (fine ? kTwigFineRows : 1u);
                        if (!G) continue;
                        for (uint32_t p = 0; p < n; p += kTwigPiecesPerEntry)
                            G->g[fine ? TwigsFine : Twigs].push_back(
                                {inst, p, std::min(p + kTwigPiecesPerEntry, n), c, t.widening, keep, 1.0f, fade});
                    }
                }
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
            // Its thickest segments, curved (barkRows), and the rest straight.
            const uint32_t rows = barkRows(sides);
            const uint32_t nc = rows > 1 && px > 0.0f
                ? static_cast<uint32_t>(std::upper_bound(k0, k0 + n, kBarkCurvePixels / px,
                                                         [](float r, float v) { return r > v; }) - k0)
                : 0u;
            const auto entries = [](uint32_t m) { return uint64_t{(m + kSegmentsPerEntry - 1) / kSegmentsPerEntry}; };
            vertices += (entries(nc) * rows + entries(n - nc)) * kSegmentsPerEntry * static_cast<uint32_t>(sides) * 6u;
            if (G) {
                auto& gc = G->g[sides == kNearestSides ? Bark20Curved : Bark10Curved];
                auto& g = G->g[sides == kNearestSides ? Bark20 : sides == kNearSides ? Bark10 : Bark5];
                // (Its `widening` slot the radius it is cut at: scene_forest_tube.glsl
                // thins the bark just over it to nothing; its param its sides
                // and rows, sides + 256 rows.)
                for (uint32_t c = 0; c < nc; c += kSegmentsPerEntry)
                    gc.push_back({inst, sp.segStart + c, sp.segStart + std::min(c + kSegmentsPerEntry, nc),
                                  static_cast<uint32_t>(sides) + 256u * rows, t.barkRadius, 0.0f, 1.0f, 1.0f});
                for (uint32_t c = nc; c < n; c += kSegmentsPerEntry)
                    g.push_back({inst, sp.segStart + c, sp.segStart + std::min(c + kSegmentsPerEntry, n),
                                 static_cast<uint32_t>(sides) + 256u, t.barkRadius, 0.0f, 1.0f, 1.0f});
            }
            // What the round bark leaves out or thins (keep radius under
            // twice the cut) down to kBarkRibbonCut of the cut, as ribbons
            // covering their width (mesh_forest_tube.vert's bark ribbon
            // build, in the twigs' draw; colour only). Counted when drawn,
            // not against the budget, as the sprites: four vertices a
            // segment, but counted as the round bark's they thinned the
            // canopy's leaves to pay for them (detail 1.07 -> 1.74).
            if (!shadow && t.barkRadius > 0.0f && G) {
                const auto upTo = [&](float r) {
                    return static_cast<uint32_t>(
                        std::upper_bound(k0, k1, r, [](float a, float v) { return a > v; }) - k0);
                };
                const uint32_t r0 = upTo(2.0f * t.barkRadius), r1 = upTo(kBarkRibbonCut * t.barkRadius);
                if (r1 > r0) {
                    vertices += uint64_t{(r1 - r0 + kTwigPiecesPerEntry - 1) / kTwigPiecesPerEntry} *
                                kTwigPiecesPerEntry * kVertsPerTwig;
                    for (uint32_t c = r0; c < r1; c += kTwigPiecesPerEntry)
                            G->g[Twigs].push_back({inst, sp.segStart + c,
                                                   sp.segStart + std::min(c + kTwigPiecesPerEntry, r1), ~0u,
                                                   t.barkRadius, 1.0f, 1.0f, 1.0f});
                }
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
                    shadow ? casterLod(L, sp, s) : ForestNode::treeLod(L, sp, s.px, leafLength, forceOf(s), s.barkPx),
                    shadow, v, s.px);
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
    P.eye = view.eye;
    P.world = world;
    P.pxAtMetre = pxAtMetre;
    P.nodeScale = nodeScale;
    P.perspective = view.perspective;
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
    P.twigs = node.look().leafForm == 0 && !node.clumpTwigs().empty();
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
        if (std::hypot(q.x - e.x, q.z - e.z) < 40.0f) P.canopyTop = std::max(P.canopyTop, b.max.y - reach);
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
                          (view.perspective ? pxAtMetre / d : pxAtMetre) * unit,
                          P.lod.forceRange <= 0.0f || dl <= P.lod.forceRange});
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
                const float dc = bromath::vlen((P.bounds[i].min + P.bounds[i].max) * 0.5f - view.eye);
                if (!tile.ortho) texel += tile.texelPerMetre * dc;
                P.cast.push_back({i, instances[i].scale * nodeScale / std::max(texel, 1e-5f), ti, 0.0f,
                                  P.lod.forceRange <= 0.0f || dc <= P.lod.forceRange});
            }
        }
    }
    return plan;
}

const SceneVkBuffer* SceneMeshDrawer::ensurePoses(SceneFrame& frame, uint32_t slots) {
    constexpr VkDeviceSize kPoseBytes = 64;   // scene_forest.glsl's FOREST_POSE_RECORDS uvec4s
    const VkDeviceSize need = std::max<VkDeviceSize>(slots, 1) * kPoseBytes;
    if (poses_.isValid() && poses_.size >= need) return &poses_;
    // Grown with room to spare; the old buffer stays until the frames that
    // read it are done (this one's earlier nodes among them).
    // (destroyBuffer defers the destruction past the frames in flight.)
    if (poses_.isValid()) frame.gpu.allocator.destroyBuffer(poses_);
    const VkDeviceSize size = std::max(
        need, std::min<VkDeviceSize>(std::max<VkDeviceSize>(need + need / 2, VkDeviceSize{1} << 20),
                                     VkDeviceSize{kMaxPoses} * kPoseBytes));
    if (!frame.gpu.allocator.createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, poses_)) {
        LOG_ERROR("SceneMeshDrawer: Failed creating the forest pose buffer (%llu bytes)",
                  static_cast<unsigned long long>(size));
        poses_ = {};
        return nullptr;
    }
    return &poses_;
}

void SceneMeshDrawer::recordForestPoses(SceneFrame& frame) {
    if (poseJobs_.empty()) return;
    VkCommandBuffer cmd = frame.cmd;
    // The last frame's draws read the buffer this one overwrites.
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before,
                         0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, posePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, poseLayout_, 0, 1, &frame.cameraSet, 0, nullptr);
    for (const ForestPoseJob& job : poseJobs_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, poseLayout_, 3, 1, &job.set, 0, nullptr);
        vkCmdPushConstants(cmd, poseLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MeshPushConstants), &job.push);
        vkCmdDispatch(cmd, job.entries, 1, 1);
    }
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &after,
                         0, nullptr, 0, nullptr);
    poseJobs_.clear();
}

void SceneMeshDrawer::prepareForest(SceneFrame& frame, ForestNode& node, std::vector<MeshDraw>& out) {
    out.clear();
    poseJobs_.clear();
    poseUsed_ = 0;
    auto plan = planForest(frame, node);
    if (!plan) return;
    ForestPlan& P = *plan;
    float detail = P.force >= 0 || P.seen.empty()
                       ? 1.0f : fitDetail([&](float d) { return P.cost(d, false); }, P.lod.budget);
    float shadowDetail = P.force >= 0 || P.cast.empty()
                             ? 1.0f : fitDetail([&](float d) { return P.cost(d, true); }, P.lod.shadowBudget);
    if (forestDetail_.size() > 256) forestDetail_.clear();
    if (P.force < 0)
        settleDetail(forestDetail_[&node], frame, P.lod.budget, P.lod.shadowBudget, detail, shadowDetail);
    finishForest(frame, P, detail, shadowDetail, out);
}

void SceneMeshDrawer::settleDetail(ForestDetail& d, const SceneFrame& frame, float budget, float shadowBudget,
                                   float& detail, float& shadowDetail) {
    // A jump of the eye (a cut, a teleport, the first frame) or a new
    // budget takes the fit as it is. Otherwise the detail moves toward the
    // fit 2% a 30th of a second, a finer fit only once it is 4% finer than
    // what is drawn: each frame's fit, quantised by its search and moving
    // with the camera, re-thinned the crowns a little every frame. A step is
    // a share of the trees near a hand-over (leaves to cards, ±6% of
    // widening) switching at once, so it stays small: at 6% a 30th (or a
    // quarter of the gap a 30th) a dolly out through the card distances
    // switched half a nearby crown to cards in one frame. The rates go by
    // the scene's clock, not by renders: a frame drawn several times moved
    // it as many steps (20% at once). With the clock still (paused) a moving
    // eye settles at 60 Hz.
    const bromath::Vec3& eye = frame.view.eye;
    const float dx = eye.x - d.eye[0], dy = eye.y - d.eye[1], dz = eye.z - d.eye[2];
    const float moved = dx * dx + dy * dy + dz * dz;
    const bool snap = d.detail <= 0.0f || moved > 25.0f || budget != d.budget || shadowBudget != d.shadowBudget;
    const float now = frame.graph.windTime();
    float dt = std::clamp(now - d.time, 0.0f, 0.1f);
    if (dt <= 0.0f && moved > 1e-6f) dt = 1.0f / 60.0f;
    d.time = now;
    d.eye[0] = eye.x; d.eye[1] = eye.y; d.eye[2] = eye.z;
    d.budget = budget;
    d.shadowBudget = shadowBudget;
    const float step = std::pow(1.02f, dt * 30.0f);
    const auto settle = [snap, step](float prev, float target) {
        if (snap || prev <= 0.0f) return target;
        if (target > prev) return std::min(target, prev * step);
        if (target < prev / 1.04f) return std::max(target, prev / step);
        return prev;
    };
    detail = d.detail = settle(d.detail, detail);
    shadowDetail = d.shadow = settle(d.shadow, shadowDetail);
}

void SceneMeshDrawer::prepareForests(SceneFrame& frame, const std::vector<ForestNode*>& nodes,
                                     std::vector<std::vector<MeshDraw>>& out) {
    out.assign(nodes.size(), {});
    poseJobs_.clear();
    poseUsed_ = 0;
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
    float detail = forced || !anySeen || budget <= 0.0f
                       ? 1.0f : fitDetail([&](float d) { return total(d, false); }, budget);
    float shadowDetail = forced || !anyCast || shadowBudget <= 0.0f
                             ? 1.0f : fitDetail([&](float d) { return total(d, true); }, shadowBudget);
    if (!forced) settleDetail(forestsDetail_, frame, budget, shadowBudget, detail, shadowDetail);
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
            const ForestNode::TreeLod t = ForestNode::treeLod(L, sp, s.px, P.leafLength, P.forceOf(s), s.barkPx);
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
    nearestFirst(colour.g[Twigs]);
    nearestFirst(colour.g[TwigsFine]);
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
    const size_t bytes = total * kEntryFloats * sizeof(float);
    const render::UploadSlice slice = frame.gpu.device.frameUpload(bytes);
    if (!slice) {
        node.setStats(stats);
        return;
    }
    // The colour leaves are posed a leaf at a time (forest_leaf_pose.comp):
    // a pose slot each in this frame's pose buffer.
    // A group past the buffer's room (a forced Leaves tier over a whole
    // forest) poses its leaves vertex by vertex instead (kForestLeafInline).
    uint32_t poseLeaves = 0;
    bool posedGroup[kGroups] = {};
    for (int k : {LeafNear, LeafCard}) {
        uint32_t n = 0;
        for (const Entry& e : colour.g[k]) n += e.end - e.first;
        if (n == 0 || uint64_t{poseUsed_} + poseLeaves + n > kMaxPoses) continue;
        posedGroup[k] = true;
        poseLeaves += n;
    }
    const SceneVkBuffer* poses = ensurePoses(frame, poseUsed_ + poseLeaves);
    if (!poses) {
        node.setStats(stats);
        return;
    }
    // Entries go up as uint bits and floats, the layout scene_forest.glsl reads.
    auto* dst = static_cast<float*>(slice.mapped);
    size_t at = 0;
    const auto put = [&](const std::vector<Entry>& g, bool posed) {
        const size_t first = at;
        for (const Entry& e : g) {
            float* d = dst + at * kEntryFloats;
            d[0] = floatBits(e.instance);
            d[1] = floatBits(e.first);
            d[2] = floatBits(e.end);
            d[3] = floatBits(e.param);
            d[4] = e.widening;
            d[5] = e.keepEnd;
            d[6] = e.tail;
            d[7] = e.fade;
            d[8] = floatBits(posed ? poseUsed_ : ~0u);
            d[9] = d[10] = d[11] = 0.0f;
            if (posed) poseUsed_ += e.end - e.first;
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
    writer.writeBuffer(3, poses->buffer, poses->size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
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
            const bool posed = !shadow && posedGroup[k];
            const uint32_t first = put(g, posed);
            MeshDraw d = base;
            d.shadowOnly = shadow;
            d.castsShadow = shadow;
            // Across every forest node: cores, leaves and bark (near, solid)
            // first, then the rest of the scene, then the far tiers, so the
            // clumps and crowns behind near trees and ground are rejected
            // before they shade. Seen from over the canopy the crowns are
            // what hides the rest (the floor, the leaves under them), so
            // there they keep their place before it.
            const bool farLast = P.canopyTop > -1e29f && view.eye.y < P.canopyTop;
            // (Ranked as before the curved bark and fine twigs had groups of
            // their own, each with its plain one: the clumps and crowns
            // under kOrderMid, so near them the static meshes and grass
            // behind fail the depth test, not shade.)
            const int rank = k == Bark20Curved ? Bark20 - 2 : k == Bark10Curved ? Bark10 - 2
                           : k <= LeafCard ? k : k <= Twigs ? k - 2 : k - 3;
            d.order = farLast && (k == Clumps || k == Crowns) ? static_cast<uint8_t>(MeshDraw::kOrderFar + rank)
                                                              : static_cast<uint8_t>(rank);
            if (shadow) d.shadowTileMask = 1u << slot;
            // The timed passes' name for it: the group and the node's leaf form.
            static constexpr const char* kLabel[kGroups][3] = {
                {"forest:cores:broad", "forest:cores:needle", "forest:cores:scale"},
                {"forest:leafNear:broad", "forest:leafNear:needle", "forest:leafNear:scale"},
                {"forest:leafCard:broad", "forest:leafCard:needle", "forest:leafCard:scale"},
                {"forest:bark20c:broad", "forest:bark20c:needle", "forest:bark20c:scale"},
                {"forest:bark10c:broad", "forest:bark10c:needle", "forest:bark10c:scale"},
                {"forest:bark20:broad", "forest:bark20:needle", "forest:bark20:scale"},
                {"forest:bark10:broad", "forest:bark10:needle", "forest:bark10:scale"},
                {"forest:bark5:broad", "forest:bark5:needle", "forest:bark5:scale"},
                {"forest:twigs:broad", "forest:twigs:needle", "forest:twigs:scale"},
                {"forest:twigsFine:broad", "forest:twigsFine:needle", "forest:twigsFine:scale"},
                {"forest:blooms:broad", "forest:blooms:needle", "forest:blooms:scale"},
                {"forest:clumps:broad", "forest:clumps:needle", "forest:clumps:scale"},
                {"forest:crowns:broad", "forest:crowns:needle", "forest:crowns:scale"}};
            static constexpr const char* kShadowLabel[kGroups] = {
                "shadow:forest:cores", "shadow:forest:leafNear", "shadow:forest:leafCard", "shadow:forest:bark20c",
                "shadow:forest:bark10c", "shadow:forest:bark20", "shadow:forest:bark10",
                "shadow:forest:bark5", "shadow:forest:twigs", "shadow:forest:twigsFine", "shadow:forest:blooms", "shadow:forest:clumps",
                "shadow:forest:crowns"};
            d.timerLabel = kLabel[k][std::clamp(static_cast<int>(node.look().leafForm), 0, 2)];
            if (shadow) d.shadowTimerLabel = kShadowLabel[k];
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
                                                         (cut ? mesh_flags::kForestLeafCut : 0u) |
                                                         (!shadow && !posed ? mesh_flags::kForestLeafInline : 0u));
                break;
            }
            case Bark20Curved:
            case Bark10Curved:
            case Bark20:
            case Bark10:
            case Bark5: {
                const int sides = k == Bark20 || k == Bark20Curved ? kNearestSides
                                  : k == Bark10 || k == Bark10Curved ? kNearSides : kFarSides;
                const bool curved = k == Bark20Curved || k == Bark10Curved;
                const uint32_t rows = curved ? barkRows(sides) : 1u;
                d.kind = MeshKind::ForestTube;
                d.indexCount = kSegmentsPerEntry * static_cast<uint32_t>(sides) * 6u * rows;
                if (const GpuMesh* ix = res.mesh(node.id(), (curved ? kTubeCurveSlot : kTubeSlot) + sides, 1,
                                                 woodIndices(sides, rows)))
                    d.indices = ix->indices.buffer;
                d.instanceCount = static_cast<uint32_t>(g.size());
                d.firstInstance = first;
                const float* bc = node.barkColor();
                d.push.baseColor[0] = bc[0];
                d.push.baseColor[1] = bc[1];
                d.push.baseColor[2] = bc[2];
                d.push.baseColor[3] = 1.0f;
                d.push.pbrParams[1] = node.barkRoughness();
                d.push.extra[1] = 0.0f;
                // The width (px) the rings' sides morph by (forestTubeMorph),
                // the tree's choice of sides by its trunk's (treeLod):
                // negative, never the twigs' 3.
                d.push.extra[2] = -(shadow ? P.shadowLod : P.lod).barkSidesPixels;
                d.push.pbrParams[3] = static_cast<float>(barkFlags);
                break;
            }
            case Twigs:
            case TwigsFine: {
                // Ribbons facing the eye (mesh_forest_tube.vert's twig
                // build, push.extra.z 2 + its rows a piece) in the bark's
                // colour.
                const uint32_t rows = k == TwigsFine ? kTwigFineRows : 1u;
                d.kind = MeshKind::ForestTube;
                d.indexCount = kTwigPiecesPerEntry * kVertsPerTwig * rows;
                // (Slot kTubeSlot + 1, which no bark's sides take, for the fine pattern.)
                if (const GpuMesh* ix = res.mesh(node.id(), kTubeSlot + (rows > 1 ? 1 : 0), 1, woodIndices(0, rows)))
                    d.indices = ix->indices.buffer;
                d.instanceCount = static_cast<uint32_t>(g.size());
                d.firstInstance = first;
                d.twoSided = true;
                const float* bc = node.barkColor();
                d.push.baseColor[0] = bc[0];
                d.push.baseColor[1] = bc[1];
                d.push.baseColor[2] = bc[2];
                d.push.baseColor[3] = 1.0f;
                d.push.pbrParams[1] = node.barkRoughness();
                d.push.extra[1] = 0.0f;
                d.push.extra[2] = 2.0f + static_cast<float>(rows);
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
            if (posed) {
                // The pose pass runs these entries first, with the draw's own
                // push constants (its flags say cluster or cut), its first
                // entry in extra.z.
                ForestPoseJob job;
                job.set = set;
                job.push = d.push;
                job.push.extra[2] = static_cast<float>(first);
                job.entries = static_cast<uint32_t>(g.size());
                poseJobs_.push_back(job);
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
