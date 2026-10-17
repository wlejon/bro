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
//            `flutterFade`; past `cardWidening` a leaf is a 6-vertex card.
//   IMPOSTORS  (the default far tier, `impostors` 1) past `clumpFrom` ..
//            `clumpTo` (a widening) the leaves hand over to one card a tree
//            showing the leaves tier's own picture of its specimen
//            (forest_impostor.h: colour, normal, depth and coverage baked
//            from `impostorViews` x `impostorViews` directions), lit at run
//            time as the leaves are, so the crown's colour, value, texture,
//            extent and gaps carry on from the leaves. The bark is in the
//            picture. The card is posed by the trunk and mean limb; its
//            shading does not move with it.
//   CLUMPS   (`impostors` 0) past `clumpFrom` .. `clumpTo` the leaves hand
//            over to the crown's clumps drawn as the three lobes their
//            leaves fill: leafy discs lit as spheres, posed like the leaves.
//   CROWNS   (`impostors` 0) once a clump is smaller than `crownFrom` ..
//            `crownTo` pixels, one disc a tree, posed by its trunk.
//   BARK     at the leaves, clumps and crowns tiers, the segments whose keep
//            radius (the thickest tip at or beyond them) is at least
//            `barkPixels` wide on screen: a connected prefix, so limbs drop
//            out tip first.
// A hand-over splits each pixel between the two tiers, and under MSAA each
// pixel's samples, so it covers once and blends in quarters rather than
// stippling.
// Every tier is posed by the same vegPose from the same records, so a
// cross-fade does not swim. Switch points are in pixels, so they follow the
// view's resolution and field of view, not metres.
//
// Shadows: each tile of the shadow plan gets the trees in its light volume
// at the tier its texel size asks for (texels are pixels there), leaves
// thinned to `shadowKeep` more; an impostor casts its picture from the
// light's side, at its baked depth. Grass casts into each light's first tile.
//
// A ForestNode is a MeshNode so the material setters apply to the leaves
// (colour, roughness, subsurface, shadows) and its mesh is the leaf (the
// leaf card the scatter would draw); `barkColor` / `barkRoughness` are the
// bark's. Its transform places the whole forest. It is left out of raycasts.

#include "scene/forest_impostor.h"
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
    };
    bool setSpecimens(SpecimenArrays&& a);

    /// The trees: 8 floats each, x, y, z, scale, yaw (radians; local (x, z)
    /// turns to (x cos + z sin, -x sin + z cos)), specimen, 0, 0, in the
    /// node's space. Rows naming a missing specimen are dropped.
    void setInstances(const float* rows, size_t count);
    size_t instanceCount() const { return instances_.size(); }
    size_t specimenCount() const { return specimens_.size(); }

    struct Lod {
        float leafPixels = 8.0f;      // a leaf shorter than this on screen thins the crown
        float flutterFade = 2.0f;     // widening by which a leaf's own motion is gone
        float cardWidening = 1.6f;    // widening past which a leaf is a 6-vertex card
        float clumpFrom = 3.0f;       // widening over which leaves hand over to clumps
        float clumpTo = 4.5f;
        float crownFrom = 5.0f;       // mean clump radius (px) over which clumps hand over to crowns
        float crownTo = 3.0f;
        float barkPixels = 0.7f;      // bark narrower than this on screen is left out
        float barkSidesPixels = 24.0f;// a trunk wider than this (px) has 10 sides, else 5
        float shadowKeep = 0.35f;     // the share of a tree's drawn leaves its shadow keeps
        float range = 0.0f;           // trees further than this (m) are not drawn; 0: no limit
        // Vertex budgets (millions), the leaves' and bark's: when the trees
        // in view would send more, `leafPixels` and `barkPixels` scale up
        // together (the DETAIL factor, Stats) until they fit, so every tree
        // thins alike, the furthest handing over to clumps first. The
        // shadow budget is every tile's together. 0: no budget.
        float budget = 18.0f;
        float shadowBudget = 6.0f;
        // The far tier: 1 impostors (one card a tree, its specimen's baked
        // picture), 0 the clumps and crowns sprites.
        float impostors = 1.0f;
        float impostorViews = 8.0f;    // directions a side of the hemi-octahedral grid
        float impostorTexels = 128.0f; // a direction's image a side
    };
    static constexpr int kLodCount = 16;
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

    /// Draw every tree at one tier (0 leaves, 1 clumps, 2 crowns, 3
    /// impostors, at full size and no thinning), or -1 to choose by size on
    /// screen. For tests and comparisons.
    void setForceTier(int t) { forceTier_ = t < -1 || t > 3 ? -1 : t; }
    int forceTier() const { return forceTier_; }

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
        bromath::AABB3 bounds;       // relative
    };
    const std::vector<Specimen>& specimens() const { return specimens_; }
    /// Segment keep radii, thickest first within each specimen.
    const std::vector<float>& keepRadius() const { return keepRadius_; }

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

    /// The leaf, length of: the leaf mesh's extent along its tip, times the
    /// placement's base scale (specimen space).
    float leafLength() const;
    /// `copies` copies of the leaf mesh end to end (an instance's worth), and
    /// the far card's; regenerated when the mesh changes.
    const bromesh::MeshData& leafBatch() const;
    const bromesh::MeshData& cardBatch() const;
    int leafCopies() const;
    int cardCopies() const { return kCardCopies; }
    static constexpr int kCardVertices = 6;
    static constexpr int kCardCopies = 160;

    // --- Level of detail ---

    struct TreeLod {
        float widening = 1.0f;     // leaves drawn this much larger
        float keep = 1.0f;         // share of the leaves drawn (1 / widening^2)
        bool card = false;         // leaves as cards
        float leaves = 1.0f;       // the leaf tier's weight (1 solid, 0 none)
        float clumps = 0.0f;       // the clump tier's
        float crowns = 0.0f;       // the crown tier's
        float impostors = 0.0f;    // the impostor tier's
        float barkRadius = 0.0f;   // segments whose keep radius is at least this (specimen units) are drawn
        int barkSides = 10;
        bool bark = true;          // bark segments drawn (an impostor carries its own)
    };
    static constexpr int kTiers = 4;   // leaves, clumps, crowns, impostors
    /// What a tree of `s` at `pxPerUnit` pixels per specimen unit (the view's
    /// pixels per metre at the tree times its scale) draws. `leafLength` in
    /// specimen units. `forceTier` as setForceTier. The drawer measures the
    /// leaves at the crown and the bark at the whole tree, so `barkPx` may
    /// differ (0: the same).
    static TreeLod treeLod(const Lod& lod, const Specimen& s, float pxPerUnit, float leafLength, int forceTier = -1,
                           float barkPx = 0.0f);

    /// The dithered fade code mesh.frag reads: the tier draws the pixels
    /// whose dither value h is in [lo, hi). The tiers of a tree split [0, 1)
    /// between them (crowns, clumps, impostors, then leaves), so a
    /// cross-fade covers every pixel exactly once. 1 is solid.
    static float fadeCode(float lo, float hi);
    /// A tree's tiers' [lo, hi) ranges, in TreeLod order (leaves, clumps,
    /// crowns, impostors).
    static void fadeRanges(const TreeLod& t, float (&lo)[kTiers], float (&hi)[kTiers]);

    // --- Impostors ---

    /// The baked pictures (forest_impostor.h), baked on first use and again
    /// when the specimens, the leaf mesh, the leaf or bark colour or the
    /// lod's view count or size change. Empty without specimens.
    const ForestImpostorAtlas& impostorAtlas() const;
    /// Its colour and data images, as textures the renderer uploads.
    const NodeTexture& impostorColor() const;
    const NodeTexture& impostorData() const;
    /// emitForest's arrays as set.
    const SpecimenArrays& arrays() const { return data_; }

    struct Stats {
        uint32_t trees[kTiers] = {};   // trees drawing each tier (a cross-fading tree counts in both)
        uint32_t culled = 0;       // trees outside the view
        uint64_t leaves = 0;       // leaves issued
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
    void ensureBatches() const;

    std::vector<Specimen> specimens_;
    SpecimenArrays data_;
    std::vector<float> keepRadius_;
    std::vector<Instance> instances_;
    bromath::AABB3 forestBounds_;
    Lod lod_;
    int forceTier_ = -1;
    float barkColor_[3] = {0.2f, 0.17f, 0.14f};
    float barkRoughness_ = 0.9f;
    Stats stats_;

    void refreshGpu() const;

    mutable std::vector<float> gpu_;
    mutable bool gpuDirty_ = true;
    mutable uint64_t dataGeneration_ = nextResourceGeneration();
    mutable uint64_t gpuMeshGeneration_ = 0;   // the leaf mesh and flutter fade gpu_ was built with
    mutable float gpuFlutter_ = 0.0f;
    mutable bool gpuImpostors_ = false;        // gpu_ carries the impostor spheres

    // The impostor bake and what it was baked from.
    mutable ForestImpostorAtlas impostor_;
    mutable NodeTexture impostorColor_, impostorData_;
    mutable uint64_t impostorSpecimens_ = 0, impostorMesh_ = 0;
    mutable float impostorKey_[8] = {};
    uint64_t specimensGeneration_ = nextResourceGeneration();

    mutable bromesh::MeshData leafBatch_, cardBatch_;
    mutable uint64_t batchGeneration_ = 0;
    mutable float leafLength_ = 0.0f;
    mutable int leafCopies_ = 1;
};

}  // namespace bro::scene
