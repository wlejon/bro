// Forest natives: SceneGraph.createForest and the ForestNode members behind
// js/scene_forest.js (specimens, blooms, trees, level of detail, bark, stats).

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_runtime.h"
#include "engine/engine.h"
#include "natives/scene/native_scene_decl.h"

#include "scene/forest_node.h"

#include <cmath>
#include <vector>

using namespace bro::bronze_host;

namespace {

scene::ForestNode* forestOf(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return nullptr;
    return static_cast<scene::MeshNode*>(n)->asForest();
}

std::vector<float> copyOf(const float* p, uint32_t n) { return p ? std::vector<float>(p, p + n) : std::vector<float>{}; }

}  // namespace

bool bro_scene_SceneNode_setForestSpecimens(void* self, const float* specimens, uint32_t specimens_len,
                                            const float* segments, uint32_t segments_len, const float* tangents,
                                            uint32_t tangents_len, const float* segmentVegetation,
                                            uint32_t segmentVegetation_len, const float* keepRadius,
                                            uint32_t keepRadius_len, const float* clumps, uint32_t clumps_len,
                                            const float* clumpVegetation, uint32_t clumpVegetation_len,
                                            const int32_t* leaves, uint32_t leaves_len, const double* leaf,
                                            uint32_t leaf_len, const float* segmentClumps, uint32_t segmentClumps_len,
                                            const float* clumpGroups, uint32_t clumpGroups_len) {
    auto* f = forestOf(self);
    if (!f) return false;
    scene::ForestNode::SpecimenArrays a;
    a.specimens = copyOf(specimens, specimens_len);
    a.segments = copyOf(segments, segments_len);
    a.tangents = copyOf(tangents, tangents_len);
    a.segmentVegetation = copyOf(segmentVegetation, segmentVegetation_len);
    a.keepRadius = copyOf(keepRadius, keepRadius_len);
    a.clumps = copyOf(clumps, clumps_len);
    a.clumpVegetation = copyOf(clumpVegetation, clumpVegetation_len);
    a.segmentClumps = copyOf(segmentClumps, segmentClumps_len);
    a.clumpGroups = copyOf(clumpGroups, clumpGroups_len);
    if (leaves) a.leaves.assign(reinterpret_cast<const uint32_t*>(leaves),
                                reinterpret_cast<const uint32_t*>(leaves) + leaves_len);
    const auto at = [&](uint32_t i, double d) { return i < leaf_len && std::isfinite(leaf[i]) ? leaf[i] : d; };
    a.leaf.seed = static_cast<uint32_t>(static_cast<uint64_t>(at(0, 0.0)) & 0xffffffffu);
    a.leaf.upBias = static_cast<float>(at(1, 0.0));
    a.leaf.tiltJitter = static_cast<float>(at(2, 0.0));
    a.leaf.shell = static_cast<float>(at(3, 0.0));
    a.leaf.baseScale = static_cast<float>(at(4, 1.0));
    a.leaf.scaleJitter = static_cast<float>(at(5, 0.0));
    return f->setSpecimens(std::move(a));
}

bool bro_scene_SceneNode_setForestBlooms(void* self, const float* blooms, uint32_t blooms_len,
                                         const float* specimenBlooms, uint32_t specimenBlooms_len) {
    auto* f = forestOf(self);
    if (!f) return false;
    return f->setBlooms(copyOf(blooms, blooms_len), copyOf(specimenBlooms, specimenBlooms_len));
}

void bro_scene_SceneNode_setForestInstances(void* self, const float* rows, uint32_t rows_len) {
    if (auto* f = forestOf(self)) f->setInstances(rows, rows ? rows_len / 8 : 0);
}

void bro_scene_SceneNode_setForestLod(void* self, const double* lod, uint32_t lod_len) {
    if (auto* f = forestOf(self)) f->lodFromArray(lod, lod_len);
}

void bro_scene_SceneNode_forestLod(void* self, bronze_native_buffer* out) {
    auto* f = forestOf(self);
    if (!f) { transferBuffer(std::vector<double>{}, out); return; }
    float v[scene::ForestNode::kLodCount];
    f->lodToArray(v);
    transferBuffer(std::vector<double>(std::begin(v), std::end(v)), out);
}

void bro_scene_SceneNode_setForestBark(void* self, double r, double g, double b, double roughness) {
    if (auto* f = forestOf(self)) {
        f->setBarkColor(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b));
        f->setBarkRoughness(static_cast<float>(roughness));
    }
}

bool bro_scene_SceneNode_setForestLook(void* self, const float* looks, uint32_t looks_len, const double* look,
                                       uint32_t look_len) {
    auto* f = forestOf(self);
    if (!f) return false;
    scene::ForestNode::Look l = f->look();
    const auto at = [&](uint32_t i, float d) {
        return i < look_len && std::isfinite(look[i]) ? static_cast<float>(look[i]) : d;
    };
    l.leafForm = static_cast<int>(std::lround(at(0, static_cast<float>(l.leafForm))));
    l.flutter = at(1, l.flutter);
    l.limbBob = at(2, l.limbBob);
    l.twigBob = at(3, l.twigBob);
    l.leafCut = at(5, l.leafCut ? 1.0f : 0.0f) != 0.0f;
    f->setLook(l);
    // look[4]: whether `looks` is given (else the specimens keep theirs).
    if (at(4, 0.0f) == 0.0f) return true;
    return f->setSpecimenLooks(copyOf(looks, looks_len));
}

void bro_scene_SceneNode_setForestTier(void* self, int32_t tier) {
    if (auto* f = forestOf(self)) f->setForceTier(tier);
}

void bro_scene_SceneNode_forestStats(void* self, bronze_native_buffer* out) {
    // What the last frame drew: a frame a flush left pending is drawn first.
    if (auto* eng = hostEngine()) eng->renderPendingScenes(/*forRead=*/true);
    auto* f = forestOf(self);
    if (!f) { transferBuffer(std::vector<double>{}, out); return; }
    const auto& s = f->stats();
    transferBuffer(std::vector<double>{static_cast<double>(s.trees[0]), static_cast<double>(s.trees[1]),
                                       static_cast<double>(s.trees[2]), static_cast<double>(s.culled),
                                       static_cast<double>(s.leaves), static_cast<double>(s.vertices),
                                       static_cast<double>(s.shadowVertices), static_cast<double>(s.entries),
                                       static_cast<double>(s.draws), static_cast<double>(f->instanceCount()),
                                       static_cast<double>(f->specimenCount()), static_cast<double>(s.detail),
                                       static_cast<double>(s.shadowDetail), static_cast<double>(s.blooms),
                                       static_cast<double>(s.unforced), static_cast<double>(s.shadowUnforced)},
                   out);
}

void bro_scene_SceneNode_forestTreeLod(void* self, int32_t specimen, double pxPerUnit, bronze_native_buffer* out) {
    auto* f = forestOf(self);
    if (!f || specimen < 0 || static_cast<size_t>(specimen) >= f->specimenCount()) {
        transferBuffer(std::vector<double>{}, out);
        return;
    }
    const auto t = scene::ForestNode::treeLod(f->lod(), f->specimens()[static_cast<size_t>(specimen)],
                                              static_cast<float>(pxPerUnit), f->leafLength(), f->forceTier());
    transferBuffer(std::vector<double>{t.widening, t.keep, t.card ? 1.0 : 0.0, t.leaves, t.clumps, t.crowns,
                                       t.barkRadius, static_cast<double>(t.barkSides)},
                   out);
}

void bro_scene_SceneNode_forestSeenLod(void* self, int32_t instance, bronze_native_buffer* out) {
    auto* f = forestOf(self);
    const auto* lods = f ? &f->seenLods() : nullptr;
    if (!lods || instance < 0 || static_cast<size_t>(instance) >= lods->size() ||
        (*lods)[static_cast<size_t>(instance)].widening <= 0.0f) {
        transferBuffer(std::vector<double>{}, out);
        return;
    }
    const auto& t = (*lods)[static_cast<size_t>(instance)];
    transferBuffer(std::vector<double>{t.widening, t.keep, t.card ? 1.0 : 0.0, t.leaves, t.clumps, t.crowns,
                                       t.barkRadius, static_cast<double>(t.barkSides)},
                   out);
}
