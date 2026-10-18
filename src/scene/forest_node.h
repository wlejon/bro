#pragma once

// Forest: many instances of a few grown plants (SPECIMENS, bro.flora's
// emitForest, broflora/forest.h), each drawn at the level of detail its size
// on screen asks for, from standing among the trunks to the horizon. The
// GPU half is vulkan/shaders/scene_forest.glsl; the per-frame choice is
// vulkan/scene_mesh_drawer_forest.cpp.
//
// The tiers, a tree at a time (treeLod):
//   LEAVES   up close every leaf the plant grew, placed as the clump-mode
//            scatter places it, swaying with the wood and fluttering on its
//            own (vegLeaf). Once a leaf is smaller on screen than
//            `leafPixels` the crown THINS: a prefix of the tree's shuffled
//            leaves is kept, 1 / w^2 of them for a widening w, each w times
//            larger (and pulled into its lobe), so the crown's cover holds
//            while the work falls by w^2; the last of the kept run shrink
//            away, so the count never pops. Flutter fades out by
//            `flutterFade`. Past `cardWidening` a kept leaf is a CLUSTER
//            (`clusters` 1): a card facing the eye holding about w^2 leaves
//            of the leaf's own size round it, placed, turned and lit in the
//            fragment shader, so a thinned crown keeps leaf-sized detail
//            rather than turning into flakes (`clusters` 0: a 6-vertex card
//            of the widened leaf).
//   CLUMPS   past `clumpFrom` .. `clumpTo` (a widening) the leaves hand over
//            to the crown's clumps, each drawn as the three lobes its leaves
//            fill (procClumpLobe): a card a lobe, on which the fragment
//            shader places the lobe's leaves, at their own size and count,
//            on the near and far halves of its shell, turned out from its
//            middle as the scatter turns them, and lights them as the
//            leaves tier lights its leaves (scene_forest_mass.glsl). Nothing
//            is baked: a tree's clumps are its own records, so every tree
//            can be unique.
//   CROWNS   once a clump is smaller than `crownFrom` .. `crownTo` pixels,
//            one card a tree over the crown's whole extent, its clumps'
//            masses and gaps drawn the same way a size up.
//   BARK     at every tier, the segments whose keep radius (the thickest
//            tip at or beyond them) is at least `barkPixels` wide on screen:
//            a connected prefix, so limbs drop out tip first.
// A hand-over splits each pixel between the two tiers, and under MSAA each
// pixel's samples, so it covers once and blends in quarters rather than
// stippling.
//   BLOOMS   a flowering specimen's flowers, mophead heads or spikes
//            (setBlooms, emitForest's `blooms`), at its twigs' tips: a card
//            a bloom whose fragment shader shapes the form, posed by its
//            twig's sway. Drawn whatever the tree's tier while a bloom is
//            more than a fraction of a pixel across; under
//            FOREST_BLOOM_DOT_PX (mesh_forest_bloom.vert) a bloom is a dot
//            that size, its cover cut to the bloom's share of it, so the
//            blooms dwindle with distance rather than pop.
// Every tier is posed by the same vegPose from the same records, so a
// cross-fade does not swim. Switch points are in pixels, so they follow the
// view's resolution and field of view, not metres.
//
// Shadows: each tile of the shadow plan gets the trees in its light volume
// at the tier its texel size asks for (texels are pixels there), leaves
// thinned to `shadowKeep` more (as widened cards, not clusters); clumps and
// crowns cast their discs. Grass casts into each light's first tile.
//
// A ForestNode is a MeshNode so the material setters apply to the leaves
// (colour, roughness, subsurface, shadows) and its mesh is the leaf (the
// leaf card the scatter would draw); `barkColor` / `barkRoughness` are the
// bark's. Its transform places the whole forest. It is left out of raycasts.

#include "scene/mesh_node.h"

#include <bromath/aabb.h>
#include <bromath/vec.h>

#include <cstdint>
#include <vector>

namespace bro::scene {

class ForestNode : public MeshNode {
public:
    explicit ForestNode(const std::string& name = "");
    ~ForestNode() override;

    ForestNode* asForest() override { return this; }

    /// The leaf placement every clump is filled by (bromesh's
    /// LeafClumpOptions, as emitForest reports it).
    struct LeafParams {
        uint32_t seed = 0;
        float upBias = 0.0f, tiltJitter = 0.0f, shell = 0.0f;
        float baseScale = 1.0f, scaleJitter = 0.0f;
    };

    /// emitForest's arrays (broflora/forest.h; specimens as its binding
    /// packs them, 16 floats each). False, changing nothing, when they do
    /// not fit together.
    struct SpecimenArrays {
        std::vector<float> specimens, segments, tangents, segmentVegetation, keepRadius, clumps, clumpVegetation;
        std::vector<uint32_t> leaves;
        LeafParams leaf;
        std::vector<float> blooms;   // setBlooms's (setSpecimens leaves them empty)
    };
    bool setSpecimens(SpecimenArrays&& a);

    /// The specimens' blooms (emitForest's `blooms`, 8 floats each: position
    /// xyz relative, size, direction xyz, segment within its specimen's;
    /// and `specimenBlooms`, 8 floats a specimen: bloomStart, bloomCount,
    /// form (broflora's BloomForm: 0 none, 1 flower, 2 head, 3 spike), size,
    /// colour rgb, 0). Both empty: no blooms. False, changing nothing, when
    /// they do not fit the specimens. setSpecimens clears them.
    bool setBlooms(std::vector<float> blooms, std::vector<float> specimenBlooms);
    static constexpr int kBloomForms = 4;
    /// How far a bloom of `size` reaches past its twig's tip (a spike's stalk
    /// and spike, a head set past the tip; mesh_forest_bloom.vert), and the
    /// side of a square of its area (a flower's or a head's across; a
    /// spike's, the root of its length times its width), both in multiples
    /// of its size: the vertex stage culls a bloom by the latter.
    static float bloomReach(int form) { return form == 3 ? 1.9f : 1.0f; }
    static float bloomExtent(int form) { return form == 3 ? 0.42f : 1.0f; }

    /// The trees: 8 floats each, x, y, z, scale, yaw (radians; local (x, z)
    /// turns to (x cos + z sin, -x sin + z cos)), specimen, 0, 0, in the
    /// node's space. Rows naming a missing specimen are dropped.
    void setInstances(const float* rows, size_t count);
    size_t instanceCount() const { return instances_.size(); }
    size_t specimenCount() const { return specimens_.size(); }

    struct Lod {
        float leafPixels = 8.0f;      // a leaf shorter than this on screen thins the crown
        float flutterFade = 2.0f;     // widening by which a leaf's own motion is gone
        float cardWidening = 1.6f;    // widening past which a leaf is a cluster (or a 6-vertex card)
        float clumpFrom = 3.0f;       // widening over which leaves hand over to clumps
        float clumpTo = 4.5f;
        float crownFrom = 7.0f;       // mean clump radius (px) over which clumps hand over to crowns
        float crownTo = 4.5f;
        float barkPixels = 0.7f;      // bark narrower than this on screen is left out
        // A trunk wider than this (px) has 10 sides, wider than
        // `barkFineSides` times it 20, else 5; each ring morphs between them
        // by its own width (scene_forest_tube.glsl's forestTubeMorph: 5 to
        // 10 over 1 to 2 times this, 10 to 20 over 2.5 to 5 times), so
        // neither the tree's switch nor a branch's narrowing shows.
        float barkSidesPixels = 24.0f;
        float shadowKeep = 0.35f;     // the share of a tree's drawn leaves its shadow keeps
        float range = 0.0f;           // trees further than this (m) are not drawn; 0: no limit
        // Vertex budgets (millions), the leaves' and bark's: when the trees
        // in view would send more, `leafPixels` and `barkPixels` scale up
        // together (the DETAIL factor, Stats) until they fit, so every tree
        // thins alike, the furthest handing over to clumps first. The
        // shadow budget is every tile's together. 0: no budget. (A cluster
        // fills its widened outline with leaves, a sprite places its own:
        // both cost fragments the vertex count does not see.)
        // (12.5: the leaf cards' vertices since a broad leaf took 9, a spray
        // 4, hold more leaves than 14 did at 12 and 6.)
        float budget = 12.5f;
        float shadowBudget = 6.0f;
        // Past `cardWidening` a kept leaf is drawn as a cluster: its widened
        // card holds the widening^2 leaves it stands for, each of its own
        // size (1); or as the one widened leaf (0).
        float clusters = 1.0f;
        // Bark at least this wide on screen (px) is drawn whatever the
        // widening and the budget ask, so a mid tree keeps its trunk and
        // scaffold limbs under its crown.
        float barkKeepPixels = 2.0f;
        // Bark of 20 sides on a trunk wider than this times barkSidesPixels
        // (0: never). Over 2.5 a ring past the shader's 10-to-20 morph pops
        // as its tree switches.
        float barkFineSides = 2.5f;
        // A forced tier (setForceTier) holds for the trees within this
        // distance of the eye (m: to the crown, as the leaves measure it;
        // to the tree's middle for a shadow caster); further trees go by
        // size, at full detail. 0: every tree. Forcing the leaves on a whole
        // forest to the horizon is billions of vertices, a frame longer than
        // a GPU's watchdog allows.
        float forceRange = 0.0f;
    };
    static constexpr int kLodCount = 17;
    /// `lod` with its pixel thresholds scaled by a detail factor (>= 1).
    static Lod scaled(const Lod& lod, float detail) {
        Lod l = lod;
        l.leafPixels *= detail;
        l.barkPixels *= detail;
        return l;
    }
    void setLod(const Lod& l) { lod_ = l; }
    const Lod& lod() const { return lod_; }
    void lodToArray(float (&out)[kLodCount]) const;
    /// From the flat list (lodToArray's order); a NaN keeps the current value.
    void lodFromArray(const double* v, size_t n);

    /// Draw every tree at one tier (0 leaves, 1 clumps, 2 crowns, at full
    /// size and no thinning), or -1 to choose by size on screen. For tests
    /// and comparisons.
    void setForceTier(int t) { forceTier_ = t < -1 || t > 2 ? -1 : t; }
    int forceTier() const { return forceTier_; }

    /// What the node's leaves are and how its plants move (scene_forest.glsl's
    /// header[3].zw):
    ///   leafForm  0 broad leaves (the leaf mesh drawn as it is), 1 needle
    ///             sprays, 2 scale-leaf sprays: the leaf mesh is the spray's
    ///             card and mesh.frag cuts the needles or scale-leaf
    ///             branchlets out of it (forestSprayCover), the far tiers
    ///             taking its leaves as covering kSprayFill of the card
    ///   flutter   the leaves' own motion, times (needles barely flutter)
    ///   limbBob   how much a limb rises and falls as it rings (0.6 a
    ///             broadleaf's; a conifer's long boughs bounce, ~1.6)
    ///   twigBob   the same for its twigs (0.8; a shrub's springy sprays more)
    /// Each specimen's leaves and bark take its own colour (setSpecimenLooks)
    /// against the node's: a tint, the colour over the node's mean.
    struct Look {
        int leafForm = 0;
        float flutter = 1.0f;
        float limbBob = 0.6f;
        float twigBob = 0.8f;
        // The leaf is a broad form's own card (scene_forest.js's), whose
        // outline mesh.frag cuts per pixel near (MESH_FOREST_LEAF_CUT); a
        // leaf mesh an app gives is drawn as it is.
        bool leafCut = false;
    };
    static constexpr int kLeafForms = 3;
    /// A crown traces its clumps in runs of this many that lie together, each
    /// run bounded by a sphere its ray can skip (gpuData's block records: the
    /// sphere, then the run's clumps as 16-bit specimen indices).
    static constexpr uint32_t kClumpBlock = 8;
    static constexpr uint32_t kBlockRecords = 2;
    /// The share of a spray's card its needles or scales cover (mesh.frag's
    /// forestSprayCover averages to it), by leaf form.
    static float sprayFill(int leafForm) { return leafForm == 1 ? 0.45f : leafForm == 2 ? 0.47f : 1.0f; }
    void setLook(const Look& l);
    const Look& look() const { return look_; }
    /// Per specimen, 8 floats (emitForest's `specimenLooks`): leaf colour,
    /// leaf form, bark colour, leaf size (read by createForest for its
    /// default card, not here); a colour of 0, 0, 0 takes the node's.
    /// Empty: every specimen the node's colours. False, changing nothing, when
    /// the count does not match the specimens. Twice as long: after the
    /// looks, 8 floats a specimen of its broad leaf (emitForest's
    /// `specimenLeaves`): width over length (0 the card's), tip (0 pointed,
    /// 1 rounded), teeth a side, tooth depth (of the half-width; deep teeth
    /// are spines), gloss (0..1) — mesh.frag cuts the outline and shades
    /// the leaf from them — then a needle or scale-leaf spray's brush (0
    /// lying round its clump, 1 radiating from the shoot, swept up), 0, 0.
    bool setSpecimenLooks(std::vector<float> looks);

    /// Bark material (the leaves take the node's).
    void setBarkColor(float r, float g, float b) { barkColor_[0] = r; barkColor_[1] = g; barkColor_[2] = b; }
    const float* barkColor() const { return barkColor_; }
    void setBarkRoughness(float r) { barkRoughness_ = r; }
    float barkRoughness() const { return barkRoughness_; }

    // --- What the drawer reads ---

    struct Specimen {
        uint32_t segStart = 0, segCount = 0;
        uint32_t clumpStart = 0, clumpCount = 0;
        uint32_t leafStart = 0, leafCount = 0;
        float height = 0.0f;
        bromath::Vec3 crown{};       // the crown's centre (of its clumps' extent), relative
        float crownRadius = 0.0f;    // across
        float crownRadiusUp = 0.0f;  // up
        float meanClumpRadius = 0.0f;
        // The crown sprite's pose: the clumps' mean branch weight, on the
        // limb of the clump nearest the crown's middle.
        float meanClumpWeight = 0.0f;
        uint32_t crownClump = 0;
        float trunkRadius = 0.0f;    // the thickest bark
        float clumpFlat = 1.0f;      // its clumps' height over their width (emitForest's specimens[15])
        // Its blooms (setBlooms): their run, form, colour and the largest's size.
        uint32_t bloomStart = 0, bloomCount = 0;
        int bloomForm = 0;
        float bloomColor[3] = {0.0f, 0.0f, 0.0f};
        float bloomSize = 0.0f;
        bromath::AABB3 bounds;       // relative: its bark, clumps and blooms
        bromath::AABB3 bareBounds;   // its bark and clumps
    };
    const std::vector<Specimen>& specimens() const { return specimens_; }
    /// Segment keep radii, thickest first within each specimen.
    const std::vector<float>& keepRadius() const { return keepRadius_; }
    /// A broad node's twigs (scene_forest_shoot.glsl): each clump's pieces,
    /// its lobe stems and sprays first (`stems`), then its shoots
    /// (`pieces`); 0 for a clump with no leaves.
    struct ClumpTwigs { uint32_t stems = 0, pieces = 0; };
    const std::vector<ClumpTwigs>& clumpTwigs() const { return clumpTwigs_; }
    /// The clumps as emitForest gave them, 8 floats each: centre xyz
    /// (specimen units, relative), radius, twig direction xyz, leaves.
    const std::vector<float>& clumpRecords() const { return data_.clumps; }
    /// The pieces a clump of `leaves` leaves draws (forestTwigPieces).
    static ClumpTwigs twigPieces(uint32_t leaves);

    struct Instance {
        bromath::Vec3 position{};
        float scale = 1.0f;
        float cosYaw = 1.0f, sinYaw = 0.0f;
        uint32_t specimen = 0;
        float seed = 0.0f;
        bromath::AABB3 bounds;       // node space, padded by the sway's reach
    };
    const std::vector<Instance>& instances() const { return instances_; }
    /// Node space, every tree (empty without trees).
    const bromath::AABB3& forestBounds() const { return forestBounds_; }

    /// The node's GPU data (scene_forest.glsl's header and records), moving
    /// to a new generation when the specimens or instances change.
    /// It reads the leaf mesh's length and the flutter fade too, so a new
    /// leaf or level of detail moves it as well.
    const std::vector<float>& gpuData() const;
    uint64_t dataGeneration() const;
    /// The blooms' first record in gpuData (the bloom draw entries' param).
    uint32_t bloomRecordBase() const {
        gpuData();
        return gpuBloomBase_;
    }

    /// The leaf, length of: the leaf mesh's extent along its tip, times the
    /// placement's base scale (specimen space).
    float leafLength() const;
    /// The leaf's width over its length (its mesh's extent across and along).
    float leafAspect() const;
    /// `copies` copies of the leaf mesh end to end (an instance's worth), and
    /// the far card's; regenerated when the mesh changes.
    const bromesh::MeshData& leafBatch() const;
    const bromesh::MeshData& cardBatch() const;
    /// kCardCopies leaf clusters (Lod::clusters): a hexagon each, every
    /// vertex at the leaf's middle in its frame, its uv the corner's
    /// direction (mesh_forest_leaf.vert faces and sizes it).
    const bromesh::MeshData& clusterBatch() const;
    int leafCopies() const;
    int cardCopies() const { return kCardCopies; }
    /// The far card's vertices: kCardVertices round a broad leaf's outline,
    /// 4 for a spray's (a quad: its needles are cut out per pixel, so the
    /// outline costs nothing, and a spray node's cards are its heaviest
    /// draw). A cluster is always a kCardVertices hexagon.
    int cardVertices() const;
    /// Moves when the card batch is rebuilt (its mesh or the leaf form).
    uint64_t cardGeneration() const;
    static constexpr int kCardVertices = 6;
    static constexpr int kCardCopies = 160;

    // --- Level of detail ---

    struct TreeLod {
        float widening = 1.0f;     // leaves drawn this much larger
        float keep = 1.0f;         // share of the leaves drawn (1 / widening^2)
        bool card = false;         // leaves as cards (cardShare past a half)
        float cardShare = 0.0f;    // the leaves' share drawn as cards, rising through cardWidening (+-6%)
        float leaves = 1.0f;       // the leaf tier's weight (1 solid, 0 none)
        float clumps = 0.0f;       // the clump tier's
        float crowns = 0.0f;       // the crown tier's
        float barkRadius = 0.0f;   // segments whose keep radius is at least this (specimen units) are drawn
        int barkSides = 10;
    };
    static constexpr int kTiers = 3;   // leaves, clumps, crowns
    /// What a tree of `s` at `pxPerUnit` pixels per specimen unit (the view's
    /// pixels per metre at the tree times its scale) draws. `leafLength` in
    /// specimen units. `forceTier` as setForceTier. The drawer measures the
    /// leaves at the crown and the bark at the whole tree, so `barkPx` may
    /// differ (0: the same).
    static TreeLod treeLod(const Lod& lod, const Specimen& s, float pxPerUnit, float leafLength, int forceTier = -1,
                           float barkPx = 0.0f);

    /// The dithered fade code mesh.frag reads: the tier draws the pixels
    /// whose dither value h is in [lo, hi). The tiers of a tree split [0, 1)
    /// between them (crowns, clumps, then leaves), so a cross-fade covers
    /// every pixel exactly once. 1 is solid.
    static float fadeCode(float lo, float hi);
    /// A tree's tiers' [lo, hi) ranges, in TreeLod order (leaves, clumps,
    /// crowns).
    static void fadeRanges(const TreeLod& t, float (&lo)[kTiers], float (&hi)[kTiers]);

    struct Stats {
        uint32_t trees[kTiers] = {};   // trees drawing each tier (a cross-fading tree counts in both)
        uint32_t culled = 0;       // trees outside the view
        uint64_t leaves = 0;       // leaves issued
        uint64_t blooms = 0;       // blooms issued
        uint64_t vertices = 0;     // vertices issued (all tiers, bark included)
        uint64_t shadowVertices = 0;
        uint32_t entries = 0;      // draw entries written
        uint32_t draws = 0;
        float detail = 1.0f;       // the budget's scale on the pixel thresholds (1: none)
        float shadowDetail = 1.0f;
    };
    void setStats(const Stats& s) { stats_ = s; }
    const Stats& stats() const { return stats_; }

private:
    void rebuildBounds();
    // clumpTwigs_ from the clumps' leaves and the specimens' phyllotaxis.
    void countClumpTwigs();
    void ensureBatches() const;

    std::vector<Specimen> specimens_;
    SpecimenArrays data_;
    std::vector<float> keepRadius_;
    std::vector<ClumpTwigs> clumpTwigs_;
    std::vector<Instance> instances_;
    bromath::AABB3 forestBounds_;
    Lod lod_;
    int forceTier_ = -1;
    Look look_;
    std::vector<float> looks_;
    std::vector<float> leafLooks_;   // 8 a specimen (setSpecimenLooks), or empty
    mutable float gpuColors_[6] = {};   // the node's leaf and bark colour gpu_'s tints were taken against
    float barkColor_[3] = {0.2f, 0.17f, 0.14f};
    float barkRoughness_ = 0.9f;
    Stats stats_;

    void refreshGpu() const;

    mutable std::vector<float> gpu_;
    mutable bool gpuDirty_ = true;
    mutable uint64_t dataGeneration_ = nextResourceGeneration();
    mutable uint64_t gpuMeshGeneration_ = 0;   // the leaf mesh and flutter fade gpu_ was built with
    mutable float gpuFlutter_ = 0.0f;
    mutable uint32_t gpuBloomBase_ = 0;

    mutable bromesh::MeshData leafBatch_, cardBatch_, clusterBatch_;
    mutable uint64_t batchGeneration_ = 0;
    mutable int batchForm_ = -1;                 // the leaf form the batches were built for
    mutable int cardVertices_ = kCardVertices;
    mutable uint64_t cardGeneration_ = 0;
    mutable float leafLength_ = 0.0f;
    mutable float leafAspect_ = 0.5f;
    mutable int leafCopies_ = 1;
};

}  // namespace bro::scene
