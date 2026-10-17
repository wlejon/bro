// Forest natives: SceneGraph.createForest and the ForestNode members behind
// js/scene_forest.js (specimens, trees, level of detail, bark, stats).

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
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
                                            uint32_t leaf_len) {
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

void bro_scene_SceneNode_setForestTier(void* self, int32_t tier) {
    if (auto* f = forestOf(self)) f->setForceTier(tier);
}

void bro_scene_SceneNode_forestStats(void* self, bronze_native_buffer* out) {
    auto* f = forestOf(self);
    if (!f) { transferBuffer(std::vector<double>{}, out); return; }
    const auto& s = f->stats();
    transferBuffer(std::vector<double>{static_cast<double>(s.trees[0]), static_cast<double>(s.trees[1]),
                                       static_cast<double>(s.trees[2]), static_cast<double>(s.culled),
                                       static_cast<double>(s.leaves), static_cast<double>(s.vertices),
                                       static_cast<double>(s.shadowVertices), static_cast<double>(s.entries),
                                       static_cast<double>(s.draws), static_cast<double>(f->instanceCount()),
                                       static_cast<double>(f->specimenCount()), static_cast<double>(s.detail),
                                       static_cast<double>(s.shadowDetail), static_cast<double>(s.trees[3])},
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
                                       t.barkRadius, static_cast<double>(t.barkSides), t.impostors,
                                       t.bark ? 1.0 : 0.0},
                   out);
}
