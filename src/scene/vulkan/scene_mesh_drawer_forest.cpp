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
#include "scene/vulkan/scene_forest_cull.h"
#include "scene/vulkan/scene_forest_draw.h"
#include "scene/vulkan/scene_forest_gpu_plan.h"
#include "scene/vulkan/shaders/scene_forest_layout.h"
#include "scene/light_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"
#include "util/log.h"
#include "util/parallel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>

namespace bro::scene::vk {

namespace {

// (The groups, their entries' sizes and their draws: scene_forest_draw.h,
// shared with the GPU planner. Its rules here have their GLSL in
// shaders/scene_forest_plan.glsl, which must change with them.)
using namespace forest;

// A clumps-tier caster past kRunNear from the eye (m, the tree's middle)
// casts a disc a run of its clumps (FOREST_CLUMP_BLOCK of them, the
// ellipsoid they fill: FOREST_SPRITE_RUNS) in place of a disc a lobe,
// handed over dithered across kRunBand: the ground's shade near the eye
// keeps its lobes, and past it a broadleaf's thousand lobes, most of the
// shadow pass's fragments, are a hundred and fifty discs.
constexpr float kRunNear = 30.0f;
constexpr float kRunBand = 10.0f;
// A tree's blooms are drawn while the largest (ForestNode::bloomExtent of
// it) is at least this wide on screen at the crown's nearest point (px): the
// vertex stage culls each bloom under a quarter pixel by its own distance
// (mesh_forest_bloom.vert's FOREST_BLOOM_CULL_PX), so a tree whose largest
// bloom is under this even there has nothing left to draw.
constexpr float kBloomMinPixels = 0.2f;
// Bark's sides (ForestNode::TreeLod::barkSides): 20 for a trunk many pixels
// wide, 10, or 5; each ring morphs between them by its own width.
// The rows of rings a CURVED segment of bark is drawn in (barkRows), along the curve
// through its ends (scene_forest_tube.glsl, the entry's param >> FOREST_BARK_ROWS_SHIFT): a
// segment at least kBarkCurvePixels in radius on screen at its tree's
// nearest point, of a tree drawing 20 or 10 sides. Thinner wood (most of a
// near conifer's whorls), and every far tree's, is the one straight row
// between its ends: its bend inside a segment is a pixel or so, and rows
// on all of it more than doubled the trunks' view's vertices.
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
// Bark down to this share of the round bark's cut (Lod::barkPixels, 0.7 px:
// 0.35 px) is drawn as ribbons covering their width. Down to 0.1 px took
// the flight's twigs 0.35 -> 2.5 ms a frame; at 0.35 px 0.6 ms, keeping
// most of the canopy's edges (40-80 m hard, noon 7.2 -> 6.9).
constexpr float kBarkRibbonCut = 0.5f;
// How much of a needle node's undrawn fine wood shows through its needles
// (the share of its outline area the tint toward the bark stands for).
constexpr float kWoodShare = 0.6f;
// ForestNode::woodSeen of the forest demo's pines, where kWoodShare was
// fitted: a needle or scale-leaf node's share is scaled by its own over
// this. (At kWoodShare alone, a cypress, its scale leaves twice as deep,
// was tinted as much as a pine: its widened tier 8.9 levels lighter than
// its leaves at 25 m noon unshadowed, 1.4 with no tint.)
constexpr float kWoodSeenRef = 0.13f;
// How much further a broad node's lobe sprites reach, per unit of that
// share (their radius times 1 + this times it, at most 1.3).
constexpr float kLobeReach = 0.5f;
// A broad node's limbs among its lobes are in their shade: the tint toward
// its bark takes this of the bark's colour.
constexpr float kWoodShade = 0.5f;
// A clump at least kTwigFinePixels across (its radius) on screen at its
// nearest point draws its twigs FINE: each piece kTwigFineRows quads along
// the curve through its joints (forestHermite), with a collar where a twig
// leaves its wood; further off a piece is the one quad between its joints
// (its stems' pieces a few pixels long, the curve's bend inside one under
// a pixel). By the clump, not its leaf: a clump of small leaves has stems
// as long as any, a polyline of long straight runs at the leaf's measure.
// (kTwigFineRows of them.)
// (A piece is about a quarter of its clump's radius; its chords turn about
// 13 degrees off its joints' ways, so the curve stands about 3% of the piece
// off its chord: a pixel at 120.)
constexpr float kTwigFinePixels = 120.0f;
constexpr float kTwigFineClumpLeaves = 40.0f;
constexpr float kTwigPixels = 0.15f;
constexpr float kTwigStemRadius = 0.04f;    // leaf lengths (forestPipe: a lobe stem's start)
constexpr float kTwigShootRadius = float(FOREST_SHOOT_RADIUS_BASE);  // a shoot's at its base
constexpr float kTwigWidening[2] = {0.95f, 1.3f};

struct Entry {
    uint32_t instance, first, end, param;
    float widening, keepEnd, tail, fade;
    float wood[3];   // leaves' and sprites' tint toward the bark (scene_forest.glsl's e2; 0: none)
};
static_assert(sizeof(Entry) == 44);
// An entry's floats on the GPU (scene_forest_layout.h's FOREST_E_*, which
// scene_forest.glsl reads): its 8, then its pose slot and its wood.
constexpr size_t kEntryFloats = FOREST_ENTRY_RECORDS * 4;
// The most leaves posed a frame (64 bytes each: 256 MB). The flight's
// heaviest views keep about 1.5 million.
constexpr uint32_t kMaxPoses = 4u << 20;

float floatBits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// An allocator whose resize leaves new elements unset (default-init, not
// value-init): a group grown to be filled in place is not zeroed first.
template <class T> struct UninitAlloc : std::allocator<T> {
    template <class U> struct rebind { using other = UninitAlloc<U>; };
    UninitAlloc() = default;
    template <class U> UninitAlloc(const UninitAlloc<U>&) noexcept {}
    template <class U> void construct(U* p) noexcept { ::new (static_cast<void*>(p)) U; }
    template <class U, class... A> void construct(U* p, A&&... a) {
        ::new (static_cast<void*>(p)) U(std::forward<A>(a)...);
    }
};
using Entries = std::vector<Entry, UninitAlloc<Entry>>;

struct Groups {
    Entries g[kGroups];
};

// A tree in view of the camera or of a shadow tile: its pixels per specimen
// unit there.
struct Seen {
    uint32_t instance;
    float px;           // at the crown (the leaves and sprites)
    int tile;           // index into the plan's tiles, -1 for the camera
    float barkPx = 0;   // at the whole tree (0: px)
    bool forced = true; // within the forced tier's range (Lod::forceRange) and its ceiling (capForced)
    float dist = 0.0f;  // from the eye (m: the camera's to the crown, a caster's to the tree's middle)
    float far = -1.0f;  // the camera's: to the crown's far side (m; Lod::leavesRange)
    // The camera's bark by its segments' own widths (ForestPlan::classifyBark):
    // one past the last segment (thickest first) curved, one past the last at
    // least barkFineSides * barkSidesPixels wide, at least barkSidesPixels
    // wide. kAllSegments: measured at the tree's nearest point.
    static constexpr uint32_t kAllSegments = ~0u;
    uint32_t curveEnd = kAllSegments, fineEnd = kAllSegments, nearEnd = kAllSegments;
};

// The coarsest detail the budget asks for: past it the trees are sprites
// and trunks, and a budget still unmet is the view's floor, not a reason to
// strip the nearest tree bare.
constexpr float kMaxDetail = 8.0f;

// The detail factor that keeps `cost`'s vertices within `budget` millions: 1
// when they fit, else the least (to about 3%) that does.
float fitDetail(const std::function<double(float)>& cost, float budget) {
    if (budget <= 0.0f) return 1.0f;
    const double limit = static_cast<double>(budget) * 1e6;
    if (cost(1.0f) <= limit) return 1.0f;
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
    bool needles = false;    // a needle node's (its crowns shade through the whole crown)
    uint32_t bloomBase = 0;  // the blooms' first record in the node's data (ForestNode::bloomRecordBase)
    const std::vector<bromath::AABB3>* bounds = nullptr;   // every tree's, world, padded by the sway's reach (ForestCull's)
    std::vector<Seen> seen;               // the camera's trees
    std::vector<Seen> cast;               // each shadow tile's
    // Trees of the far cells (ForestCull::far): a crown alone at any
    // detail, no bark, no blooms: the crown's one entry and nothing more to
    // ask. The camera's, and each tile's (tree, tile).
    std::vector<uint32_t> crowned;
    std::vector<std::pair<uint32_t, int>> castCrowned;
    std::vector<int> tileSlots;           // per tile: its slot in the shadow plan
    float canopyTop = -1e30f;             // the highest tree top within 40 m of the eye (across the ground)
    // The view, for a clump's own size on screen (fine twigs).
    bromath::Vec3 eye{};
    bromath::Mat4 world, invWorld;
    float pxAtMetre = 0.0f, nodeScale = 1.0f;
    bool perspective = true;

    // The frame's working buffers (place()'s chunks, finishForest's groups
    // and sort), kept with the plan: a spent plan's go to the next frame's
    // (reuse), so a far view's tens of megabytes of entries are neither
    // allocated, faulted in nor freed each frame.
    std::vector<std::vector<Groups>> parts;
    Groups colour;
    std::vector<Groups> tiles;
    std::vector<uint32_t> order[kGroups];
    std::vector<uint64_t> keyed, keyedSwap;
    // planForest's chunks' lists, the camera's and then each tile's.
    struct ChunkLists {
        std::vector<Seen> seen;
        std::vector<uint32_t> crowned;
        std::vector<std::pair<uint32_t, int>> castCrowned;
        uint32_t culled = 0;
        float canopyTop = -1e30f;
    };
    std::vector<ChunkLists> chunkLists;
    // The first `n` of chunkLists, emptied.
    std::vector<ChunkLists>& clearChunks(size_t n) {
        if (chunkLists.size() < n) chunkLists.resize(n);
        for (size_t c = 0; c < n; ++c) {
            ChunkLists& l = chunkLists[c];
            l.seen.clear();
            l.crowned.clear();
            l.castCrowned.clear();
            l.culled = 0;
            l.canopyTop = -1e30f;
        }
        return chunkLists;
    }

    static void clearGroups(Groups& G) {
        for (auto& g : G.g) g.clear();
    }
    // `v` as `n` empty groups, keeping each one's room.
    static void clearGroups(std::vector<Groups>& v, size_t n) {
        v.resize(n);
        for (auto& G : v) clearGroups(G);
    }
    // Takes a spent plan's buffers, emptied.
    void reuse(ForestPlan& old) {
        const auto take = [](auto& mine, auto& theirs) {
            mine.swap(theirs);
            mine.clear();
        };
        take(seen, old.seen);
        take(cast, old.cast);
        take(crowned, old.crowned);
        take(castCrowned, old.castCrowned);
        for (int s = 0; s < 2; ++s) {
            take(split[s].fine, old.split[s].fine);
            take(split[s].varying, old.split[s].varying);
            split[s].groups.swap(old.split[s].groups);
            for (auto& G : split[s].groups) clearGroups(G);
        }
        parts.swap(old.parts);
        colour = std::move(old.colour);
        clearGroups(colour);
        tiles.swap(old.tiles);
        for (int k = 0; k < kGroups; ++k) take(order[k], old.order[k]);
        take(keyed, old.keyed);
        take(keyedSwap, old.keyedSwap);
        chunkLists.swap(old.chunkLists);
    }

    // The budget's detail factor addTree measures at (set by cost(),
    // forcedCost() and finishForest()): it scales what the eye misses
    // first, the twigs' fine rows and their shoots.
    float detail = 1.0f;
    // Clump `c` of tree `inst`: its radius (specimen units) and the pixels
    // a specimen unit spans at its nearest point (0 past the records).
    float clumpPx(uint32_t inst, uint32_t c, float& radius) const {
        const auto& rec = node->clumpRecords();
        radius = 0.0f;
        if (c * 8 + 3 >= rec.size()) return 0.0f;
        const ForestNode::Instance& in = node->instances()[inst];
        const float* q = &rec[c * 8];
        const bromath::Vec3 local{q[0] * in.cosYaw + q[2] * in.sinYaw, q[1], -q[0] * in.sinYaw + q[2] * in.cosYaw};
        const bromath::Vec3 w = bromath::mtransformPoint(world, in.position + local * in.scale);
        const float unit = in.scale * nodeScale;
        const float d = std::max(bromath::vlen(w - eye) - q[3] * unit, 0.25f);
        radius = q[3];
        return (perspective ? pxAtMetre / d : pxAtMetre) * unit;
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
        // Its leaves within the camera's Lod::leavesRange as the camera's
        // are (by the caster's own distance when the camera does not see it).
        const ForestNode::SeenTree* cam = node->seenNow(s.instance);
        const bool seenTree = cam != nullptr;
        const float camDist = seenTree ? cam->dist : s.dist;
        const float camFar = seenTree ? cam->far : s.dist;
        ForestNode::TreeLod t = ForestNode::treeLod(L, sp, s.px, leafLength, forceOf(s), s.barkPx, camDist, camFar);
        // Inside the sphere its leaf casters are at full detail whatever the
        // texel, thinned only by the shadow budget (L's detail): leaf casters widened to a texel shade more than
        // the leaves they stand for (a pine inside the sphere 13 levels
        // darker than its leaves forced), and at full detail unbounded a
        // trunks view sent 32 M caster vertices to 12.6 M by size.
        if (forceOf(s) < 0 && L.leavesRange > 0.0f && camDist >= 0.0f && camDist < L.leavesRange + 0.5f * L.leavesBand &&
            sp.clumpCount > 0) {
            const float w = std::max(1.0f, L.detail);
            t.widening = w;
            t.keep = 1.0f / (w * w);
        }
        // A crown across the near sphere: the camera draws both tiers whole,
        // each its pixels on its side, and so does its caster, each leaf and
        // clump kept by its own distance (forestCasterKept). (Both whole
        // shaded it twice; leaves to the crown's last pixel inside, then
        // clumps, stepped the crown 15 levels lighter as it left; leaves for
        // the share inside and clumps for the rest, each over the whole
        // crown, shaded a pine's near leaves 8 levels darker.)
        if (forceOf(s) < 0 && t.leaves >= 1.0f && t.clumps >= 1.0f) return t;
        if (forceOf(s) >= 0 || !seenTree) return t;
        const ForestNode::TreeLod c =
            ForestNode::treeLod(lod, sp, cam->px, leafLength, -1, 0.0f, camDist, camFar);
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
        // clumps: its crowns no more than the camera's share of them. Clumps
        // shaded by crown casters had only the far half's shade (a pine from
        // above at 80 m 7 levels brighter than its leaves at noon, 11 at
        // golden), and a crown shaded by clump casters takes their shade on
        // top of its own. (Handed over by a smoothstep across the camera's
        // share of the two from 0.35 to 0.65, the casters ran behind the
        // colour's hand-over and then ahead of it: a pine at 92 to 104 m
        // golden dipped 0.4 under both tiers' dY and then rose 1.4 over.)
        // A needle crown takes the whole crown's shade itself and reads the
        // map past it (scene_forest_surface.glsl), so no clump caster lands
        // on it: its caster's crowns follow the cube of the camera's share,
        // the clumps keeping their own casters through most of the hand-over
        // (at the camera's share, a pine at 120 m, half of each, was +0.24 /
        // +0.48 dY over its leaves at noon / golden; +0.11 / +0.15).
        const float cc = std::max(c.crowns, 0.0f);
        const float keep = std::min(t.crowns, needles ? cc * cc * cc : cc);
        t.clumps += t.crowns - keep;
        t.crowns = keep;
        return t;
    }

    // What addTree counts besides its vertices (a tally a thread, summed
    // after): the vertices counted when drawn but not against the budget
    // (the sprites, crowns, blooms, cores and bark ribbons: only with a
    // `G`), so a pass that groups can still give the budget's count, and the
    // leaves and blooms a colour group issues (Stats).
    struct Tally {
        uint64_t drawnOnly = 0, leaves = 0, blooms = 0;
        void drawn(uint64_t& vertices, uint64_t n) {
            vertices += n;
            drawnOnly += n;
        }
    };

    // What one tree at level of detail `t` adds to `G` (or, with no `G`, only
    // the vertices it would). Reads the plan only, so trees may be added on
    // several threads at once, each with its own `G` and `tally`.
    void addTree(Groups* G, uint32_t inst, const ForestNode::Specimen& sp, const ForestNode::TreeLod& t,
                 bool shadow, uint64_t& vertices, Tally& tally, float px = 0.0f, float barkPx = 0.0f,
                 const Seen* seen = nullptr, float castDist = 0.0f) const {
        const auto& keepRadius = node->keepRadius();
        // A needle node's fine wood (its twigs are no form of their own):
        // what the bark's cut leaves out (under kBarkRibbonCut of the cut)
        // tints its leaves and sprites toward the bark, by that wood's share
        // of the crown's outline. Undrawn, a pine's twigs and fine limbs
        // left its far tiers greener and lighter than its leaves tier,
        // where they stand among the needles (auto 100.8 against 107.1 at
        // 45 m noon unshadowed).
        float wood[3] = {0.0f, 0.0f, 0.0f};
        // A broad node's lobe sprites reach further by the same share: the
        // limbs the cut leaves out are cover between its lobes.
        float lobeReach = 1.0f;
        if (!shadow && sp.segCount > 0 && t.barkRadius > 0.0f && sp.crownRadius > 0.0f) {
            const float* k0 = keepRadius.data() + sp.segStart;
            const uint32_t cut = static_cast<uint32_t>(
                std::upper_bound(k0, k0 + sp.segCount, kBarkRibbonCut * t.barkRadius,
                                 [](float r, float v) { return r > v; }) - k0);
            const auto& A = node->woodArea();
            const float area = A[sp.segStart + sp.segCount] - A[sp.segStart + cut];
            const float crown = 3.14159265f * sp.crownRadius * std::max(sp.crownRadiusUp, sp.crownRadius * 0.25f);
            const float share = area / crown;
            if (twigs) lobeReach = 1.0f + std::clamp(kLobeReach * share, 0.0f, 0.3f);
            const size_t spk = static_cast<size_t>(&sp - node->specimens().data());
            // A needle or scale-leaf node's wood shows past the leaves in
            // front of it: as much as its leaves leave open (woodSeen), over
            // a pine's (kWoodSeenRef), which kWoodShare was fitted on.
            const float seen = twigs ? 1.0f : node->woodSeen(spk) / kWoodSeenRef;
            const float f = std::clamp(kWoodShare * share * seen, 0.0f, 0.5f);
            if (f > 0.0f) {
                float r[3];
                node->barkOverLeaf(spk, r);
                const float shade = twigs ? kWoodShade : 1.0f;
                for (int i = 0; i < 3; ++i) wood[i] = 1.0f + f * (shade * r[i] - 1.0f);
            }
        }
        // (A broad node's leaves hang from their twigs, drawn with them.)
        const float leafWood[3] = {twigs ? 0.0f : wood[0], twigs ? 0.0f : wood[1], twigs ? 0.0f : wood[2]};
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
                                     tail, fade, {leafWood[0], leafWood[1], leafWood[2]}});
                    if (!shadow) tally.leaves += to - from;
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
                    // Under the budget (detail over 1) the fine rows and the
                    // shoots go first, by the square of the detail: a shoot
                    // is a pixel wide where its leaves are 45, and a fine
                    // twig's bend under a pixel where its clump is 120 px
                    // across, both lost among the leaves before a leaf's
                    // thinning would be seen. (Among the forest's trunks
                    // 4.9 M vertices of twigs at detail 1, as many as their
                    // leaves; under the trees 6.4 M of fine rows alone.)
                    const float fineMin = kTwigFinePixels * detail * detail;
                    const bool mayFine = kTwigFineClumpLeaves * leafLength * px >= fineMin;
                    const auto& twigsOf = node->clumpTwigs();
                    const float fade = ForestNode::fadeCode(lo[0], hi[0]);
                    const float shootMin = kTwigPixels * detail * detail;
                    for (uint32_t c = sp.clumpStart; c < sp.clumpStart + sp.clumpCount && c < twigsOf.size(); ++c) {
                        // Each clump's shoots and fine rows by its own size
                        // on screen (the tree's crown's nearest point is a
                        // bound, the eye itself under the crown).
                        float radius = 0.0f;
                        const float cpx = shoots || mayFine ? clumpPx(inst, c, radius) : 0.0f;
                        const bool shootsC = shoots && 2.0f * kTwigShootRadius * leafLength * cpx >= shootMin;
                        const uint32_t n = shootsC ? twigsOf[c].pieces : twigsOf[c].stems;
                        const bool fine = mayFine && radius * cpx >= fineMin;
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
                tally.drawn(vertices, uint64_t{(sp.clumpCount + kClumpsPerEntry - 1) / kClumpsPerEntry} *
                                          kClumpsPerEntry * kVertsPerClump);
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
            // (By the segments' own widths where the plan measured them.)
            const bool own = seen && seen->curveEnd != Seen::kAllSegments;
            const uint32_t nc = rows > 1 && px > 0.0f
                ? own ? std::min(seen->curveEnd, n)
                      : static_cast<uint32_t>(std::upper_bound(k0, k0 + n, kBarkCurvePixels / px,
                                                               [](float r, float v) { return r > v; }) - k0)
                : 0u;
            const auto entries = [](uint32_t m) { return uint64_t{(m + kSegmentsPerEntry - 1) / kSegmentsPerEntry}; };
            // Each segment's sides by its own width: a ring under
            // barkSidesPixels draws a 5-sided outline whatever its sides, and
            // one under barkFineSides times it a 10-sided one
            // (forestTubeMorph), so a segment drawn with no more sides than
            // its widest ring morphs to has the outline the tree's would. Its
            // width is measured at the tree's nearest (the larger of its two
            // measures), a quarter over for the sway. A curved one keeps a
            // curved group's (10 sides and 2 rows at the least). (At the
            // trunk's sides, a near tree drew every limb as 20 in 3 rows: a
            // skim over the canopy's bark 21.9M vertices against a 12.5M
            // budget, so every near tree's leaves went to clumps.)
            const float wpx = 2.0f * 1.25f * std::max(px, barkPx);
            const auto under = [&](uint32_t from, uint32_t to, float w) {
                return wpx > 0.0f ? static_cast<uint32_t>(std::upper_bound(k0 + from, k0 + to, w / wpx,
                                                                           [](float v, float r) { return v >= r; }) - k0)
                                  : to;
            };
            const float fine = lod.barkFineSides * lod.barkSidesPixels;
            // [0, c20) curved at 20 sides, [c20, nc) at 10; [nc, n20) straight
            // at the tree's sides, [n20, n10) at 10, [n10, n) at 5.
            const auto ownEnd = [&](uint32_t end, uint32_t from, uint32_t to) { return std::clamp(end, from, to); };
            const uint32_t c20 = sides != kNearestSides ? 0u : own ? ownEnd(seen->fineEnd, 0, nc) : under(0, nc, fine);
            uint32_t n20 = n, n10 = n;
            if (sides > kFarSides) {
                n10 = own ? ownEnd(seen->nearEnd, nc, n) : under(nc, n, lod.barkSidesPixels);
                n20 = sides == kNearestSides
                    ? std::min(own ? ownEnd(seen->fineEnd, nc, n) : under(nc, n, fine), n10) : nc;
            }
            const uint64_t perEntry = uint64_t{kSegmentsPerEntry} * 6u;
            vertices += entries(c20) * barkRows(kNearestSides) * kNearestSides * perEntry;
            vertices += entries(nc - c20) * barkRows(kNearSides) * kNearSides * perEntry;
            vertices += entries(n20 - nc) * static_cast<uint32_t>(sides) * perEntry;
            vertices += entries(n10 - n20) * kNearSides * perEntry;
            vertices += entries(n - n10) * kFarSides * perEntry;
            if (G) {
                // (Its `widening` slot the radius it is cut at: scene_forest_tube.glsl
                // thins the bark just over it to nothing; its param its sides
                // and rows, sides | rows << FOREST_BARK_ROWS_SHIFT.)
                const auto add = [&](int k, int s, uint32_t r, uint32_t from, uint32_t to) {
                    for (uint32_t c = from; c < to; c += kSegmentsPerEntry)
                        G->g[k].push_back({inst, sp.segStart + c, sp.segStart + std::min(c + kSegmentsPerEntry, to),
                                           barkParam(s, r), t.barkRadius, 0.0f, 1.0f, 1.0f});
                };
                add(Bark20Curved, kNearestSides, barkRows(kNearestSides), 0, c20);
                add(Bark10Curved, kNearSides, barkRows(kNearSides), c20, nc);
                add(sides == kNearestSides ? Bark20 : sides == kNearSides ? Bark10 : Bark5, sides, 1, nc, n20);
                add(Bark10, kNearSides, 1, n20, n10);
                add(Bark5, kFarSides, 1, n10, n);
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
                    tally.drawn(vertices, uint64_t{(r1 - r0 + kTwigPiecesPerEntry - 1) / kTwigPiecesPerEntry} *
                                        kTwigPiecesPerEntry * kVertsPerTwig);
                    for (uint32_t c = r0; c < r1; c += kTwigPiecesPerEntry)
                            G->g[Twigs].push_back({inst, sp.segStart + c,
                                                   sp.segStart + std::min(c + kTwigPiecesPerEntry, r1), ~0u,
                                                   t.barkRadius, 1.0f, kBarkRibbonCut, 1.0f});
                }
            }
        }
        // Sprites: counted when drawn, not against the budget, which buys
        // leaves and bark (a sprite tree's cost is a fixed few hundred
        // vertices and its pixels).
        // A caster's clumps tier split between its lobes (near) and its runs
        // (far): [lo, mid) lobes, [mid, hi) runs. (A pinnate specimen's runs
        // hang beside each other across its curtain, so no ellipsoid stands
        // for one: its lobes cast at every distance.)
        float mid = hi[1];
        if (shadow && !node->pinnate(static_cast<size_t>(&sp - node->specimens().data()))) {
            const float w = std::clamp((castDist - kRunNear) / kRunBand, 0.0f, 1.0f);
            mid = hi[1] - (hi[1] - lo[1]) * w;
        }
        if (hi[1] > mid && sp.clumpCount > 0 && G) {
            const uint32_t n = (sp.clumpCount + FOREST_CLUMP_BLOCK - 1) / FOREST_CLUMP_BLOCK;
            tally.drawn(vertices, uint64_t{(n + kRunsPerEntry - 1) / kRunsPerEntry} * kRunsPerEntry *
                                      FOREST_CARD_VERTICES);
            const float fade = ForestNode::fadeCode(mid, hi[1]);
            for (uint32_t r = 0; r < n; r += kRunsPerEntry)
                G->g[Runs].push_back({inst, r, std::min(r + kRunsPerEntry, n), 0u, 1.0f, 0.0f, 1.0f, fade});
        }
        if (mid > lo[1] && sp.clumpCount > 0 && G) {
            tally.drawn(vertices, uint64_t{(sp.clumpCount + kClumpsPerEntry - 1) / kClumpsPerEntry} *
                                      kClumpsPerEntry * kVertsPerClump);
            const float fade = ForestNode::fadeCode(lo[1], mid);
            for (uint32_t c = 0; c < sp.clumpCount; c += kClumpsPerEntry)
                G->g[Clumps].push_back({inst, sp.clumpStart + c,
                                        sp.clumpStart + std::min(c + kClumpsPerEntry, sp.clumpCount), 0u, lobeReach,
                                        0.0f, 1.0f, fade, {wood[0], wood[1], wood[2]}});
        }
        if (hi[2] > lo[2] && sp.clumpCount > 0 && G) {
            tally.drawn(vertices, 6);
            G->g[Crowns].push_back({inst, 0u, 1u, 0u, 1.0f, 0.0f, 1.0f, ForestNode::fadeCode(lo[2], hi[2])});
        }
        // Blooms: whatever the tier, while the largest is wide enough to
        // leave a trace; solid (each dwindles into a dot by its own size on
        // screen, mesh_forest_bloom.vert, so there is nothing to cross-fade).
        // Counted when drawn, as the sprites are. They cast no shadow.
        if (G && !shadow && sp.bloomCount > 0 &&
            sp.bloomSize * ForestNode::bloomExtent(sp.bloomForm) * px >= kBloomMinPixels) {
            tally.drawn(vertices, uint64_t{(sp.bloomCount + kBloomsPerEntry - 1) / kBloomsPerEntry} * kBloomsPerEntry *
                                kVertsPerBloom);
            for (uint32_t c = 0; c < sp.bloomCount; c += kBloomsPerEntry)
                G->g[Blooms].push_back({inst, sp.bloomStart + c,
                                        sp.bloomStart + std::min(c + kBloomsPerEntry, sp.bloomCount), bloomBase,
                                        1.0f, 0.0f, 1.0f, 1.0f});
            tally.blooms += sp.bloomCount;
        }
    }

    // The vertices tree `s` sends at the plan's `detail` (`L`, the Lod scaled
    // by it), against the budget; adding its entries to `G` if given, the
    // vertices they draw to `*all` and its tiers to `tiers`.
    uint64_t treeCost(const Seen& s, const ForestNode::Lod& L, bool shadow, Tally& tally, Groups* G = nullptr,
                      uint64_t* all = nullptr, uint32_t* tiers = nullptr) const {
        const ForestNode::Specimen& sp = node->specimens()[node->instances()[s.instance].specimen];
        const ForestNode::TreeLod t =
            shadow ? casterLod(L, sp, s)
                   : ForestNode::treeLod(L, sp, s.px, leafLength, forceOf(s), s.barkPx, s.dist, s.far);
        if (tiers) {
            if (t.leaves > 0.0f) tiers[0]++;
            if (t.clumps > 0.0f) tiers[1]++;
            if (t.crowns > 0.0f) tiers[2]++;
        }
        uint64_t v = 0;
        const uint64_t only = tally.drawnOnly;
        addTree(G, s.instance, sp, t, shadow, v, tally, s.px, s.barkPx, shadow ? nullptr : &s, s.dist);
        if (all) *all += v;
        return v - (tally.drawnOnly - only);
    }

    // Trees [0, n) in chunks across the worker pool (scene_forest_cull.h).
    static uint32_t chunksOf(size_t n) { return forest::chunksOf(n); }
    template <class Fn> static void eachChunk(size_t n, Fn fn) { forest::eachChunk(n, fn); }
    // The camera's trees (or the shadow tiles') at detail `d`, chunk by
    // chunk across the pool, each chunk its own groups and counts, joined in
    // the trees' order: their entries appended to `out` (one Groups, or one
    // a tile; none: only counted), the vertices they draw to `drawn`, their
    // tiers to `tiers` (the camera's), the rest to `tally`, and each tree's
    // vertices against the budget to `each`.
    void place(bool shadow, float d, std::vector<Groups>* out, uint64_t& drawn,
               uint32_t (&tiers)[ForestNode::kTiers], Tally& tally, std::vector<uint64_t>* each = nullptr) {
        const std::vector<Seen>& trees = shadow ? cast : seen;
        this->detail = d;
        const ForestNode::Lod L = ForestNode::scaled(shadow ? shadowLod : lod, d);
        const uint32_t chunks = chunksOf(trees.size());
        if (out) {
            parts.resize(std::max<size_t>(parts.size(), chunks));
            for (uint32_t c = 0; c < chunks; ++c) clearGroups(parts[c], out->size());
        }
        std::vector<Tally> tallies(chunks);
        std::vector<uint64_t> drawnOf(chunks, 0);
        std::vector<std::array<uint32_t, ForestNode::kTiers>> tiersOf(chunks);
        if (each) each->resize(trees.size());
        eachChunk(trees.size(), [&](uint32_t c, uint32_t b, uint32_t e) {
            tiersOf[c] = {};
            for (uint32_t i = b; i < e; ++i) {
                Groups* G = out ? &parts[c][shadow ? static_cast<size_t>(trees[i].tile) : 0] : nullptr;
                const uint64_t v =
                    treeCost(trees[i], L, shadow, tallies[c], G, &drawnOf[c], shadow ? nullptr : tiersOf[c].data());
                if (each) (*each)[i] = v;
            }
        });
        for (uint32_t c = 0; c < chunks; ++c) {
            drawn += drawnOf[c];
            tally.drawnOnly += tallies[c].drawnOnly;
            tally.leaves += tallies[c].leaves;
            tally.blooms += tallies[c].blooms;
            for (int k = 0; k < ForestNode::kTiers; ++k) tiers[k] += tiersOf[c][k];
        }
        if (out) appendGroups(*out, chunks);
    }

    // The first `chunks` of parts' groups appended to `out`'s, in chunk
    // order: the order one thread would have added them in.
    // Each group is grown once, every chunk's place in it known, and the
    // chunks copied across the pool.
    void appendGroups(std::vector<Groups>& out, uint32_t chunks) {
        appendAt.resize(size_t{chunks} * out.size() * kGroups);
        for (size_t t = 0; t < out.size(); ++t)
            for (int k = 0; k < kGroups; ++k) {
                size_t n = out[t].g[k].size();
                for (uint32_t c = 0; c < chunks; ++c) {
                    appendAt[(size_t{c} * out.size() + t) * kGroups + k] = n;
                    n += parts[c][t].g[k].size();
                }
                out[t].g[k].resize(n);
            }
        bro::util::parallelChunks(chunks, [&](uint32_t c) {
            for (size_t t = 0; t < out.size(); ++t)
                for (int k = 0; k < kGroups; ++k) {
                    const Entries& from = parts[c][t].g[k];
                    if (!from.empty())
                        std::memcpy(out[t].g[k].data() + appendAt[(size_t{c} * out.size() + t) * kGroups + k],
                                    from.data(), from.size() * sizeof(Entry));
                }
        });
    }
    std::vector<size_t> appendAt;   // appendGroups': each chunk's place in each group

    // The budget's search asks cost() at a dozen details. A tree sends no
    // fewer vertices at a finer one, so one that sends as many at the
    // coarsest the search goes (kMaxDetail) as at 1 sends that at every
    // detail between: most of a large forest's, its far crowns. Their sum
    // is taken once, and each search step walks only the others.
    // (The search's first ask, at 1, is often its last: each tree's cost
    // there is kept, and the split made only if it asks again. The ask at 1
    // also groups the trees' entries, which the frame draws if it settles
    // on detail 1, as most do, rather than placing every tree again.)
    struct CostSplit {
        std::vector<uint64_t> fine;      // each tree's at detail 1 (seen / cast order)
        uint64_t fineSum = 0;
        bool split = false;
        uint64_t fixed = 0;
        std::vector<uint32_t> varying;   // indices into seen / cast
        // At detail 1: the entries (the camera's, or each tile's), the
        // vertices they draw, the trees per tier, the leaves and blooms.
        bool grouped = false;
        std::vector<Groups> groups;
        uint64_t drawn = 0, leaves = 0, blooms = 0;
        uint32_t tiers[ForestNode::kTiers] = {};
        // Whether the ask at 1 groups: not where the last frame settled
        // coarser (a binding budget's frames rarely draw detail 1).
        bool group = true;
    } split[2];

    // The vertices the camera's (or the shadow tiles') trees send at `detail`.
    double cost(float detail, bool shadow) {
        if (force >= 0) return 0.0;
        const std::vector<Seen>& trees = shadow ? cast : seen;
        CostSplit& cs = split[shadow ? 1 : 0];
        const ForestNode::Lod& base = shadow ? shadowLod : lod;
        const size_t groupsOf = shadow ? tileSlots.size() : 1;
        if (cs.fine.size() != trees.size()) {
            if (cs.group) clearGroups(cs.groups, groupsOf);
            Tally tally;
            place(shadow, 1.0f, cs.group ? &cs.groups : nullptr, cs.drawn, cs.tiers, tally, &cs.fine);
            cs.leaves = tally.leaves;
            cs.blooms = tally.blooms;
            for (uint64_t f : cs.fine) cs.fineSum += f;
            cs.grouped = cs.group;
        }
        if (detail == 1.0f) return static_cast<double>(cs.fineSum);
        if (!cs.split) {
            cs.split = true;
            this->detail = kMaxDetail;
            const ForestNode::Lod L = ForestNode::scaled(base, kMaxDetail);
            std::vector<uint8_t> varies(trees.size());
            eachChunk(trees.size(), [&](uint32_t, uint32_t b, uint32_t e) {
                Tally tally;
                for (uint32_t i = b; i < e; ++i) varies[i] = cs.fine[i] != treeCost(trees[i], L, shadow, tally);
            });
            for (uint32_t i = 0; i < trees.size(); ++i) {
                if (!varies[i]) cs.fixed += cs.fine[i];
                else cs.varying.push_back(i);
            }
        }
        this->detail = detail;
        const ForestNode::Lod L = ForestNode::scaled(base, detail);
        std::vector<uint64_t> sums(chunksOf(cs.varying.size()), 0);
        eachChunk(cs.varying.size(), [&](uint32_t c, uint32_t b, uint32_t e) {
            Tally tally;
            for (uint32_t k = b; k < e; ++k) sums[c] += treeCost(trees[cs.varying[k]], L, shadow, tally);
        });
        uint64_t v = cs.fixed;
        for (uint64_t s : sums) v += s;
        return static_cast<double>(v);
    }

    // The camera's tree `s`'s bark classes (Seen::curveEnd, ...) by each
    // segment's width at its own distance from the eye (at rest; the drawer
    // measures a quarter over for the sway, as at the tree's nearest point).
    // The tree's nearest point is a bound for all its wood: inside its box,
    // under its crown, it is the eye itself (0.25 m), and every limb of the
    // tree drew 20 sides in 3 rows, metres away or not (the forest's
    // understory view, 13 M vertices of curved bark against a 12.5 M
    // budget, so every near tree's leaves went to clumps). A class stays a
    // prefix of the thickest-first order (one past the last segment that
    // needs it): a thinner segment needing more only raises the ones
    // before it, never lowers one below its own width.
    void classifyBark(Seen& s) const {
        if (!perspective) return;
        const ForestNode::Instance& in = node->instances()[s.instance];
        const ForestNode::Specimen& sp = node->specimens()[in.specimen];
        if (sp.segCount == 0) return;
        const float* k0 = node->keepRadius().data() + sp.segStart;
        const std::vector<float>& rec = node->segmentRecords();
        if ((sp.segStart + sp.segCount) * 8u > rec.size()) return;
        // At most what the tree's nearest point asks for (its own distance
        // is never nearer).
        const float near = std::max(s.px, s.barkPx);
        const float fine = lod.barkFineSides * lod.barkSidesPixels;
        const float wMin = std::min(lod.barkSidesPixels, fine > 0.0f ? fine : lod.barkSidesPixels);
        const float rMin = std::min(kBarkCurvePixels / near, wMin / (2.0f * 1.25f * near));
        const uint32_t m = static_cast<uint32_t>(
            std::upper_bound(k0, k0 + sp.segCount, rMin, [](float r, float v) { return r > v; }) - k0);
        s.curveEnd = s.fineEnd = s.nearEnd = 0;
        if (m == 0) return;
        // The eye in the specimen's space.
        const bromath::Vec3 v = (bromath::mtransformPoint(invWorld, eye) - in.position) * (1.0f / in.scale);
        const bromath::Vec3 e{v.x * in.cosYaw - v.z * in.sinYaw, v.y, v.x * in.sinYaw + v.z * in.cosYaw};
        const float unit = in.scale * nodeScale;
        for (uint32_t i = 0; i < m; ++i) {
            const float* g = &rec[(sp.segStart + i) * 8u];
            const bromath::Vec3 a{g[0], g[1], g[2]}, b{g[4], g[5], g[6]};
            const bromath::Vec3 ab = b - a;
            const float l2 = bromath::vdot(ab, ab);
            const float f = l2 > 0.0f ? std::clamp(bromath::vdot(e - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            const float d = std::max((bromath::vlen(e - (a + ab * f)) - k0[i]) * unit, 0.25f);
            const float pxu = pxAtMetre * unit / d;   // px per specimen unit there
            if (k0[i] * pxu > kBarkCurvePixels) s.curveEnd = i + 1;
            const float w = 2.0f * 1.25f * k0[i] * pxu;
            if (fine > 0.0f && w >= fine) s.fineEnd = i + 1;
            if (w >= lod.barkSidesPixels) s.nearEnd = i + 1;
        }
    }

    // The vertices tree `s` sends at the forced tier, full detail.
    uint64_t forcedCost(const Seen& s, bool shadow) {
        detail = 1.0f;
        const ForestNode::Specimen& sp = node->specimens()[node->instances()[s.instance].specimen];
        uint64_t v = 0;
        Tally tally;
        addTree(nullptr, s.instance, sp,
                shadow ? casterLod(shadowLod, sp, s) : ForestNode::treeLod(lod, sp, s.px, leafLength, force, s.barkPx, s.dist, s.far),
                shadow, v, tally, s.px, s.barkPx, shadow ? nullptr : &s);
        return v;
    }
};

namespace {

// A forced tier holds for the nearest trees (the camera's, or the shadow
// tiles') of every node in `plans` until they send kForceCeiling budgets of
// vertices; the rest go by size, at full detail, as the trees past
// Lod::forceRange do. Forced Leaves over a whole forest was billions of
// vertices (the forest's glade, 2.2 G), a frame longer than the GPU's
// watchdog: the device was lost. A tiers subject (one tree) never meets it.
constexpr double kForceCeiling = 16.0;

template <class Plan> void capForced(const std::vector<Plan*>& plans, bool shadow) {
    std::vector<std::pair<Plan*, Seen*>> forced;
    double ceiling = 0.0;
    for (Plan* p : plans) {
        if (!p || p->force < 0) continue;
        const float budget = shadow ? p->lod.shadowBudget : p->lod.budget;
        ceiling = std::max(ceiling, kForceCeiling * 1e6 *
                                        (budget > 0.0f ? budget : shadow ? ForestNode::Lod{}.shadowBudget
                                                                         : ForestNode::Lod{}.budget));
        for (Seen& s : shadow ? p->cast : p->seen)
            if (s.forced) forced.push_back({p, &s});
    }
    std::stable_sort(forced.begin(), forced.end(),
                     [](const auto& a, const auto& b) { return a.second->dist < b.second->dist; });
    double v = 0.0;
    for (auto& [p, s] : forced) {
        if (v <= ceiling) v += static_cast<double>(p->forcedCost(*s, shadow));
        if (v <= ceiling) continue;
        s->forced = false;
        ++(shadow ? p->stats.shadowUnforced : p->stats.unforced);
    }
}

}  // namespace

SceneMeshDrawer::ForestCull& SceneMeshDrawer::forestCull(const ForestNode& node, const bromath::Mat4& world,
                                                         float reach) {
    const uint64_t use = ++forestCullUse_;
    auto& slot = forestCull_[&node];
    if (!slot) slot = std::make_shared<ForestCull>();
    ForestCull& c = *slot;
    if (c.generation != node.dataGeneration() || c.reach != reach ||
        std::memcmp(c.world.data, world.data, sizeof(world.data)) != 0 || c.bounds.size() != node.instances().size()) {
        c.build(node, world, reach);
        c.generation = node.dataGeneration();
    }
    c.used = use;
    // Nodes gone (or long undrawn) give their cells back.
    if ((use & 1023) == 0)
        for (auto it = forestCull_.begin(); it != forestCull_.end();)
            it = it->second->used + 4096 < use ? forestCull_.erase(it) : std::next(it);
    return c;
}

std::unique_ptr<SceneMeshDrawer::ForestPlan> SceneMeshDrawer::planForest(SceneFrame& frame, ForestNode& node) {
    if (node.instanceCount() == 0 || node.specimenCount() == 0 || node.mesh().empty() || node.effectiveUnlit()) {
        node.setStats({});
        return nullptr;
    }
    auto plan = std::make_unique<ForestPlan>();
    ForestPlan& P = *plan;
    if (!sparePlans_.empty()) {
        P.reuse(*sparePlans_.back());
        sparePlans_.pop_back();
    }
    P.node = &node;
    const SceneView& view = frame.view;
    const bromath::Mat4& world = node.worldMatrix();
    const float nodeScale = std::sqrt(world.data[4] * world.data[4] + world.data[5] * world.data[5] +
                                      world.data[6] * world.data[6]);
    // Pixels per metre at a metre's distance (perspective), or flat (ortho).
    const float pxAtMetre = std::fabs(view.proj.at(1, 1)) * static_cast<float>(view.height) * 0.5f;
    P.eye = view.eye;
    P.world = world;
    P.invWorld = bromath::minverse(world);
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
    // A forced tier is drawn whole, not cut to the near sphere (forced
    // leaves past it were their pixels inside it only).
    if (P.force >= 0) P.lod.leavesRange = P.shadowLod.leavesRange = 0.0f;
    P.cores = node.look().leafForm != 0;
    // A spray node's thinned leaves stay sprays, each widened (its needles
    // cut out as the near ones'), not clusters of broad leaves.
    P.clusters = P.lod.clusters > 0.5f && node.look().leafForm == 0;
    P.twigs = node.look().leafForm == 0 && !node.clumpTwigs().empty();
    P.needles = node.look().leafForm == 1;
    P.bloomBase = node.bloomRecordBase();
    const auto& specimens = node.specimens();
    const auto& instances = node.instances();

    float maxHeight = 0.0f;
    for (const auto& s : specimens) maxHeight = std::max(maxHeight, s.height);
    float maxScale = 0.0f;
    for (const auto& in : instances) maxScale = std::max(maxScale, in.scale);
    const float reach = frame.renderer.vegetationReach(maxHeight * maxScale, world);

    // Every tree's world bounds (kept from frame to frame), by cell.
    ForestCull& cull = forestCull(node, world, reach);
    P.bounds = &cull.bounds;

    // The far cells: every tree of one sure to draw as a crown alone, with
    // no bark (nor the ribbons the bark's cut leaves) and no blooms, at any
    // detail (the budget only coarsens), outside the near sphere and the
    // canopy's 40 m. Their trees take the crown's one entry and are asked
    // nothing more (P.crowned): most of a large forest's. Measured at the
    // cell's nearest point, which none of its trees is nearer than, with a
    // margin, so a tree on a test's edge goes with the trees that ask.
    const bromath::Vec3 eye = view.eye;
    const auto nearest = [&](const bromath::AABB3& b, float& across) {
        const float dx = std::max(std::max(b.min.x - eye.x, eye.x - b.max.x), 0.0f);
        const float dy = std::max(std::max(b.min.y - eye.y, eye.y - b.max.y), 0.0f);
        const float dz = std::max(std::max(b.min.z - eye.z, eye.z - b.max.z), 0.0f);
        across = std::sqrt(dx * dx + dz * dz);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    const bool farCells = view.perspective && P.force < 0 && P.lod.range <= 0.0f && P.lod.crownFrom > P.lod.crownTo;
    // treeLod's bark is cut at this many pixels across over 2, at least, for
    // trees of at most `unitPx` pixels a specimen unit: as wide as their
    // leaves widen (which they do at least as long as the largest's).
    const auto barkCut = [&](float unitPx) {
        const float w = P.leafLength > 0.0f ? std::max(1.0f, P.lod.leafPixels / (P.leafLength * unitPx)) : 1.0f;
        return 0.5f * (P.lod.barkKeepPixels > 0.0f ? std::min(P.lod.barkPixels * w, P.lod.barkKeepPixels)
                                                    : P.lod.barkPixels * w);
    };
    const float nearMin = P.lod.leavesRange > 0.0f ? P.lod.leavesRange + 0.5f * P.lod.leavesBand : 0.0f;
    std::vector<uint8_t> cameraFar(cull.cellBox.size(), 0);
    if (farCells)
        ForestPlan::eachChunk(cull.cellBox.size(), [&](uint32_t, uint32_t b, uint32_t e) {
            for (uint32_t c = b; c < e; ++c) {
                float across = 0.0f;
                const float d = nearest(cull.cellBox[c], across);
                if (across < 40.0f || d < 1.0f || d < nearMin) continue;
                const float px = pxAtMetre / d;   // a metre's pixels there
                cameraFar[c] = cull.cellClump[c] * px < 0.98f * P.lod.crownTo &&
                               cull.cellKeep[c] * px < 0.98f * kBarkRibbonCut * barkCut(cull.cellUnit[c] * px) &&
                               cull.cellBloom[c] * px < 0.98f * kBloomMinPixels;
            }
        });

    // The camera's trees: those of the cells in view, in the trees' order.
    const uint32_t stamp = node.beginSeen();
    cull.markCells([&](const bromath::AABB3& b) { return bromath::fintersects(view.frustum, b); },
                   [&](size_t c) { return cameraFar[c] != 0; });
    // (Chunk by chunk across the pool, each its own lists, joined in order.)
    const uint32_t chunks = ForestPlan::chunksOf(instances.size());
    auto& parts = P.clearChunks(chunks);
    ForestPlan::eachChunk(instances.size(), [&](uint32_t chunk, uint32_t begin, uint32_t end) {
        auto& out = parts[chunk];
        for (uint32_t i = begin; i < end; ++i) {
            const bromath::AABB3& b = cull.bounds[i];
            if (!cull.mark[i] || !bromath::fintersects(view.frustum, b)) {
                out.culled++;
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
            if (std::hypot(q.x - e.x, q.z - e.z) < 40.0f) out.canopyTop = std::max(out.canopyTop, b.max.y - reach);
            if (P.lod.range > 0.0f && d > P.lod.range) {
                out.culled++;
                continue;
            }
            // The leaves': to the crown, an upright cylinder round its
            // clumps, so a crown overhead is as near as its underside, not
            // the trunk beside the eye.
            float dl = d, far = d;
            if (sp.clumpCount > 0) {
                const bromath::Vec3 local{sp.crown.x * in.cosYaw + sp.crown.z * in.sinYaw, sp.crown.y,
                                          -sp.crown.x * in.sinYaw + sp.crown.z * in.cosYaw};
                const bromath::Vec3 c = bromath::mtransformPoint(world, in.position + local * in.scale);
                const float k = in.scale * nodeScale / 0.92f;
                const float dh = std::max(0.0f, std::hypot(e.x - c.x, e.z - c.z) - sp.crownRadius * k);
                const float dv = std::max(0.0f, std::fabs(e.y - c.y) - sp.crownRadiusUp * k);
                dl = std::max(std::hypot(dh, dv), d);
                far = bromath::vlen(c - e) + std::max(sp.crownRadius, sp.crownRadiusUp) * k;
            }
            const float unit = in.scale * nodeScale;
            const float px = (view.perspective ? pxAtMetre / dl : pxAtMetre) * unit;
            const float barkPx = (view.perspective ? pxAtMetre / d : pxAtMetre) * unit;
            node.seenTree(i) = {stamp, px, barkPx, dl, far, -1};
            if (cull.mark[i] == 2) {
                out.crowned.push_back(i);
                continue;
            }
            out.seen.push_back({i, px, -1, barkPx, P.lod.forceRange <= 0.0f || dl <= P.lod.forceRange, dl, far});
        }
        for (Seen& s : out.seen) P.classifyBark(s);
    });
    {
        size_t ns = 0, nc = 0;
        for (uint32_t c = 0; c < chunks; ++c) { ns += parts[c].seen.size(); nc += parts[c].crowned.size(); }
        P.seen.reserve(ns);
        P.crowned.reserve(nc);
        for (uint32_t c = 0; c < chunks; ++c) {
            const auto& p = parts[c];
            P.seen.insert(P.seen.end(), p.seen.begin(), p.seen.end());
            P.crowned.insert(P.crowned.end(), p.crowned.begin(), p.crowned.end());
            P.stats.culled += p.culled;
            P.canopyTop = std::max(P.canopyTop, p.canopyTop);
        }
    }

    // Each shadow tile's trees, at its texel size.
    const ShadowPlan& shadows = frame.renderer.shadowPlan();
    if (node.castsShadow() && frame.gpu.targets.shadowAtlas.width == static_cast<uint32_t>(shadows.atlasSize)) {
        for (int slot = 0; slot < shadows.tileCount && slot < 32; ++slot) {
            const ShadowTilePlan& tile = shadows.tiles[slot];
            if (!tile.render) continue;
            const int ti = static_cast<int>(P.tileSlots.size());
            P.tileSlots.push_back(slot);
            // A far cell's casters likewise, at the tile's texel (the
            // camera's far cells only: a caster follows the camera's tier).
            const auto casterFar = [&](size_t c) {
                if (!cameraFar[c]) return false;
                float across = 0.0f;
                const float d = nearest(cull.cellBox[c], across);
                const float px = 1.0f / std::max(tile.texelConst + (tile.ortho ? 0.0f : tile.texelPerMetre * d), 1e-5f);
                return cull.cellClump[c] * px < 0.98f * P.shadowLod.crownTo &&
                       cull.cellKeep[c] * px < 0.98f * barkCut(cull.cellUnit[c] * px);
            };
            cull.markCells([&](const bromath::AABB3& b) { return bromath::fintersects(tile.frustum, b); }, casterFar);
            auto& casts = P.clearChunks(chunks);
            ForestPlan::eachChunk(instances.size(), [&](uint32_t chunk, uint32_t begin, uint32_t end) {
                auto& out = casts[chunk];
                for (uint32_t i = begin; i < end; ++i) {
                    if (!cull.mark[i] || !bromath::fintersects(tile.frustum, cull.bounds[i])) continue;
                    if (cull.mark[i] == 2) {
                        out.castCrowned.push_back({i, ti});
                        continue;
                    }
                    float texel = tile.texelConst;
                    const float dc = bromath::vlen((cull.bounds[i].min + cull.bounds[i].max) * 0.5f - view.eye);
                    if (!tile.ortho) texel += tile.texelPerMetre * dc;
                    out.seen.push_back({i, instances[i].scale * nodeScale / std::max(texel, 1e-5f), ti, 0.0f,
                                        P.lod.forceRange <= 0.0f || dc <= P.lod.forceRange, dc});
                }
            });
            for (uint32_t c = 0; c < chunks; ++c) {
                P.cast.insert(P.cast.end(), casts[c].seen.begin(), casts[c].seen.end());
                P.castCrowned.insert(P.castCrowned.end(), casts[c].castCrowned.begin(), casts[c].castCrowned.end());
            }
        }
    }
    return plan;
}

const SceneVkBuffer* SceneMeshDrawer::ensurePoses(SceneFrame& frame, uint32_t slots) {
    constexpr VkDeviceSize kPoseBytes = FOREST_POSE_RECORDS * 16;   // uvec4s
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
    // The GPU's plans first (their poses after them).
    if (gpuPlanner_) gpuPlanner_->record(frame);
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
    if (forestDetail_.size() > 256) forestDetail_.clear();
    {
        // On the GPU when it plans this frame (scene_mesh_drawer_forest_gpu.cpp).
        std::vector<std::vector<MeshDraw>> gpu;
        if (prepareForestsGpu(frame, {&node}, gpu, forestDetail_[&node])) {
            out.swap(gpu[0]);
            return;
        }
    }
    auto plan = planForest(frame, node);
    if (!plan) return;
    ForestPlan& P = *plan;
    capForced(std::vector<ForestPlan*>{&P}, false);
    capForced(std::vector<ForestPlan*>{&P}, true);
    if (forestDetail_.size() > 256) forestDetail_.clear();
    {
        const ForestDetail& last = forestDetail_[&node];
        P.split[0].group = !(last.detail > 1.0f);
        P.split[1].group = !(last.shadow > 1.0f);
    }
    float detail = P.force >= 0 || P.seen.empty()
                       ? 1.0f : fitDetail([&](float d) { return P.cost(d, false); }, P.lod.budget);
    float shadowDetail = P.force >= 0 || P.cast.empty()
                             ? 1.0f : fitDetail([&](float d) { return P.cost(d, true); }, P.lod.shadowBudget);
    if (forestDetail_.size() > 256) forestDetail_.clear();
    if (P.force < 0)
        settleDetail(forestDetail_[&node], frame, P.lod.budget, P.lod.shadowBudget, detail, shadowDetail);
    finishForest(frame, P, detail, shadowDetail, out);
    if (sparePlans_.size() < 8) sparePlans_.push_back(std::move(plan));
}

SceneMeshDrawer::SettleRate SceneMeshDrawer::settleRate(ForestDetail& d, const SceneFrame& frame, float budget,
                                                        float shadowBudget) {
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
    const bool jump = moved > 25.0f || budget != d.budget || shadowBudget != d.shadowBudget;
    const bool snap = d.detail <= 0.0f || jump;
    const float now = frame.graph.windTime();
    float dt = std::clamp(now - d.time, 0.0f, 0.1f);
    if (dt <= 0.0f && moved > 1e-6f) dt = 1.0f / 60.0f;
    d.time = now;
    d.eye[0] = eye.x; d.eye[1] = eye.y; d.eye[2] = eye.z;
    d.budget = budget;
    d.shadowBudget = shadowBudget;
    return {snap, jump, std::pow(1.02f, dt * 30.0f)};
}

void SceneMeshDrawer::settleDetail(ForestDetail& d, const SceneFrame& frame, float budget, float shadowBudget,
                                   float& detail, float& shadowDetail) {
    // (The GPU planner settled it last: its details are on the GPU, so this
    // takes the fit as it is, as on a jump.)
    if (d.onGpu) d.detail = d.shadow = 0.0f;
    d.onGpu = false;
    const SettleRate rate = settleRate(d, frame, budget, shadowBudget);
    const bool snap = rate.snap;
    const float step = rate.step;
    const auto settle = [snap, step](float prev, float target) {
        if (snap || prev <= 0.0f) return target;
        if (target > prev) return std::min(target, prev * step);
        if (target < prev / 1.04f) return std::max(target, prev / step);
        return prev;
    };
    detail = d.detail = settle(d.detail, detail);
    // A frame holding a shadow tile (a sun's last cascade, drawn every
    // other frame) draws fewer casters: the shadow's fit there is not the
    // drawn frames', so its detail keeps theirs.
    const bool hold = frame.renderer.shadowPlan().heldTiles > 0 && !snap && d.shadow > 0.0f;
    shadowDetail = d.shadow = hold ? d.shadow : settle(d.shadow, shadowDetail);
}

void SceneMeshDrawer::prepareForests(SceneFrame& frame, const std::vector<ForestNode*>& nodes,
                                     std::vector<std::vector<MeshDraw>>& out) {
    out.assign(nodes.size(), {});
    poseJobs_.clear();
    poseUsed_ = 0;
    if (prepareForestsGpu(frame, nodes, out, forestsDetail_)) return;
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
    std::vector<ForestPlan*> raw;
    for (auto& p : plans) {
        raw.push_back(p.get());
        if (!p) continue;
        p->split[0].group = !(forestsDetail_.detail > 1.0f);
        p->split[1].group = !(forestsDetail_.shadow > 1.0f);
    }
    capForced(raw, false);
    capForced(raw, true);
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
    // (A few kept: as many as the frame's forests, up to 8.)
    sparePlans_.clear();
    for (auto& p : plans)
        if (p && sparePlans_.size() < 8) sparePlans_.push_back(std::move(p));
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
    Groups& colour = P.colour;
    uint64_t colourVerts = 0;
    stats.detail = detail;
    {
        const ForestNode::Lod L = ForestNode::scaled(lod, detail);
        P.detail = detail;
        node.setSeenLod(L);
        for (const Seen& s : P.seen) node.seenTree(s.instance).forceTier = P.forceOf(s);
        auto& fine = P.split[0];
        if (fine.grouped && detail == 1.0f) {
            for (int k = 0; k < kGroups; ++k) colour.g[k].swap(fine.groups[0].g[k]);
            colourVerts += fine.drawn;
            for (int k = 0; k < ForestNode::kTiers; ++k) stats.trees[k] += fine.tiers[k];
            stats.leaves += fine.leaves;
            stats.blooms += fine.blooms;
        } else {
            // (Placed into the ask at 1's groups, spare at this detail.)
            std::vector<Groups>& placed = fine.groups;
            ForestPlan::clearGroups(placed, 1);
            ForestPlan::Tally tally;
            P.place(false, detail, &placed, colourVerts, stats.trees, tally);
            for (int k = 0; k < kGroups; ++k) colour.g[k].swap(placed[0].g[k]);
            stats.leaves += tally.leaves;
            stats.blooms += tally.blooms;
        }
        // (addTree's entry for a crown alone.)
        stats.trees[2] += static_cast<uint32_t>(P.crowned.size());
        colourVerts += 6 * uint64_t{P.crowned.size()};
        Entries& crowns = colour.g[Crowns];
        const size_t base = crowns.size();
        crowns.resize(base + P.crowned.size());
        ForestPlan::eachChunk(P.crowned.size(), [&](uint32_t, uint32_t b, uint32_t e) {
            for (uint32_t j = b; j < e; ++j) crowns[base + j] = {P.crowned[j], 0u, 1u, 0u, 1.0f, 0.0f, 1.0f, 1.0f};
        });
    }
    stats.vertices = colourVerts;
    // A group nearest the eye first. Not for the early depth test: every
    // forest build can discard (the dithered fade, the near sphere), and a
    // stage that can discard gets none, on an RTX 4090 as on RADV. But the
    // same fragments shade faster in that order (on foot, the clumps 2.3 ms
    // nearest first, 3.3 in the trees' order; a vista's crowns 4.16 against
    // 4.31): neighbours on screen draw together.
    // (A stable sort by distance, as a radix sort of the distances' bits: a
    // distance is never negative, so its bits order as it does. `fromBit`
    // the lowest bit sorted on: 16, the exponent and 7 bits of the mantissa,
    // orders within 1% of the distance in 2 passes. The entries stay where
    // they are: the group's order is kept beside it, and the upload follows
    // it.)
    auto& order = P.order;
    std::vector<uint64_t>& keyed = P.keyed;
    std::vector<uint64_t>& swap = P.keyedSwap;
    const auto nearestFirst = [&](int k, int fromBit = 0) {
        const Entries& g = colour.g[k];
        const size_t n = g.size();
        order[k].clear();
        if (n < 2) return;
        keyed.resize(n);
        swap.resize(n);
        ForestPlan::eachChunk(n, [&](uint32_t, uint32_t b0, uint32_t b1) {
            for (uint32_t i = b0; i < b1; ++i) {
                const auto& b = (*P.bounds)[g[i].instance];
                const float d = bromath::vlen((b.min + b.max) * 0.5f - view.eye);
                uint32_t bits;
                std::memcpy(&bits, &d, sizeof(bits));
                keyed[i] = uint64_t{bits} << 32 | i;
            }
        });
        // (Its passes in turn: across the pool, the counts, places and
        // scatter ran slower, a far view's 0.5 ms.)
        for (int shift = 32 + fromBit; shift < 64; shift += 8) {
            size_t count[257] = {};
            for (uint64_t key : keyed) count[((key >> shift) & 255) + 1]++;
            if (*std::max_element(count, count + 257) == n) continue;   // one bucket: in order already
            for (int i = 0; i < 256; ++i) count[i + 1] += count[i];
            for (uint64_t key : keyed) swap[count[(key >> shift) & 255]++] = key;
            keyed.swap(swap);
        }
        order[k].resize(n);
        for (size_t i = 0; i < n; ++i) order[k][i] = static_cast<uint32_t>(keyed[i]);
    };
    // Tree by tree (a tree's run of entries stays in its order). The cores
    // and blooms gain nothing from it (measured: unchanged either way); a
    // far view's crowns (200k entries, the frame's longest serial step
    // sorted in full) to 1%.
    nearestFirst(LeafNear);
    nearestFirst(LeafCard);
    nearestFirst(Twigs);
    nearestFirst(TwigsFine);
    nearestFirst(Clumps);
    nearestFirst(Crowns, 16);

    // Each shadow tile's trees at the shadow detail.
    std::vector<Groups>& tiles = P.tiles;
    stats.shadowDetail = shadowDetail;
    {
        auto& fine = P.split[1];
        if (fine.grouped && shadowDetail == 1.0f) {
            tiles.swap(fine.groups);
            stats.shadowVertices += fine.drawn;
        } else {
            ForestPlan::clearGroups(tiles, P.tileSlots.size());
            ForestPlan::Tally tally;
            uint32_t unused[ForestNode::kTiers] = {};
            P.place(true, shadowDetail, &tiles, stats.shadowVertices, unused, tally);
        }
        stats.shadowVertices += 6 * uint64_t{P.castCrowned.size()};
        // (Each tile's grown once, then written at a cursor.)
        std::vector<size_t> cursor(tiles.size(), 0);
        for (const auto& c : P.castCrowned) cursor[static_cast<size_t>(c.second)]++;
        for (size_t t = 0; t < tiles.size(); ++t) {
            const size_t base = tiles[t].g[Crowns].size();
            tiles[t].g[Crowns].resize(base + cursor[t]);
            cursor[t] = base;
        }
        for (const auto& [i, tile] : P.castCrowned)
            tiles[static_cast<size_t>(tile)].g[Crowns][cursor[static_cast<size_t>(tile)]++] =
                {i, 0u, 1u, 0u, 1.0f, 0.0f, 1.0f, 1.0f};
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
    // a pose slot each in this frame's pose buffer; then each tile's leaf
    // casters, the same (the GPU planner's posed groups, in its order).
    // A group past the buffer's room (a forced Leaves tier over a whole
    // forest) poses its leaves vertex by vertex instead (kForestLeafInline).
    uint32_t poseLeaves = 0;
    const auto room = [&](const Entries& g) {
        uint32_t n = 0;
        for (const Entry& e : g) n += e.end - e.first;
        if (n == 0 || uint64_t{poseUsed_} + poseLeaves + n > kMaxPoses) return false;
        poseLeaves += n;
        return true;
    };
    bool posedGroup[kGroups] = {};
    for (int k : {LeafNear, LeafCard}) posedGroup[k] = room(colour.g[k]);
    std::vector<char> posedCasters(tiles.size());
    for (size_t t = 0; t < tiles.size(); ++t) posedCasters[t] = room(tiles[t].g[LeafCard]);
    const SceneVkBuffer* poses = ensurePoses(frame, poseUsed_ + poseLeaves);
    if (!poses) {
        node.setStats(stats);
        return;
    }
    // Entries go up as uint bits and floats, the layout scene_forest.glsl reads.
    auto* dst = static_cast<float*>(slice.mapped);
    size_t at = 0;
    // A group in its order (`ord`, or as it stands): a posed one in turn (its
    // slots run on from entry to entry), the rest across the pool.
    const auto put = [&](const Entries& g, bool posed, const std::vector<uint32_t>* ord) {
        const size_t first = at;
        const bool sorted = ord && ord->size() == g.size();
        const auto write = [&](size_t j, uint32_t slot) {
            const Entry& e = g[sorted ? (*ord)[j] : j];
            float* d = dst + (first + j) * kEntryFloats;
            d[FOREST_E_INSTANCE] = floatBits(e.instance);
            d[FOREST_E_FIRST] = floatBits(e.first);
            d[FOREST_E_END] = floatBits(e.end);
            d[FOREST_E_PARAM] = floatBits(e.param);
            d[FOREST_E_WIDENING] = e.widening;
            d[FOREST_E_KEEP_END] = e.keepEnd;
            d[FOREST_E_TAIL] = e.tail;
            d[FOREST_E_FADE] = e.fade;
            d[FOREST_E_SLOT] = floatBits(slot);
            for (int i = 0; i < 3; ++i) d[FOREST_E_WOOD + i] = e.wood[i];
            return e.end - e.first;
        };
        if (posed) {
            // Each chunk's leaves summed, then its first slot known: written
            // across the pool, the slots as in turn.
            const auto leaves = [&](size_t j) {
                const Entry& e = g[sorted ? (*ord)[j] : j];
                return e.end - e.first;
            };
            std::vector<uint32_t> slotAt(ForestPlan::chunksOf(g.size()) + 1, 0);
            ForestPlan::eachChunk(g.size(), [&](uint32_t c, uint32_t b, uint32_t e) {
                uint32_t n = 0;
                for (uint32_t j = b; j < e; ++j) n += leaves(j);
                slotAt[c + 1] = n;
            });
            slotAt[0] = poseUsed_;
            for (size_t c = 1; c < slotAt.size(); ++c) slotAt[c] += slotAt[c - 1];
            ForestPlan::eachChunk(g.size(), [&](uint32_t c, uint32_t b, uint32_t e) {
                uint32_t slot = slotAt[c];
                for (uint32_t j = b; j < e; ++j) slot += write(j, slot);
            });
            poseUsed_ = slotAt.back();
        } else {
            ForestPlan::eachChunk(g.size(), [&](uint32_t, uint32_t b, uint32_t e) {
                for (uint32_t j = b; j < e; ++j) write(j, ~0u);
            });
        }
        at += g.size();
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

    // The draws' shared state (scene_forest_draw.cpp).
    forest::DrawBase drawBase;
    if (!forest::drawBase(frame, *defaults_, device_, node, set, drawBase)) {
        node.setStats(stats);
        return;
    }
    drawBase.clusters = P.clusters;
    drawBase.lod = P.lod;
    drawBase.shadowLod = P.shadowLod;
    drawBase.farLast = P.canopyTop > -1e29f && view.eye.y < P.canopyTop;

    const auto emit = [&](const Groups& G, bool shadow, int slot, bool posedCaster) {
        for (int k = 0; k < kGroups; ++k) {
            const auto& g = G.g[k];
            if (g.empty()) continue;
            const bool posed = shadow ? posedCaster && k == LeafCard : posedGroup[k];
            const uint32_t first = put(g, posed, &G == &colour ? &order[k] : nullptr);
            const MeshDraw d = forest::groupDraw(frame, node, drawBase, k, shadow, slot, posed, first,
                                                 static_cast<uint32_t>(g.size()));
            if (posed) {
                // The pose pass runs these entries first, with the draw's own
                // push constants (its flags say cluster or cut), its first
                // entry in extra.z, extra.w 1 for a caster.
                ForestPoseJob job;
                job.set = set;
                job.push = d.push;
                job.push.extra[2] = static_cast<float>(first);
                job.push.extra[3] = shadow ? 1.0f : 0.0f;
                job.entries = static_cast<uint32_t>(g.size());
                poseJobs_.push_back(job);
            }
            out.push_back(d);
            stats.draws++;
        }
    };
    emit(colour, false, 0, false);
    for (size_t t = 0; t < tiles.size(); ++t) emit(tiles[t], true, P.tileSlots[t], posedCasters[t] != 0);
    node.setStats(stats);
}

}  // namespace bro::scene::vk
