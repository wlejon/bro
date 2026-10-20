#pragma once
// A forest node's trees' world bounds and the cells of ground they stand in
// (SceneMeshDrawer::ForestCull), kept while its trees, transform and sway
// reach hold: the CPU planner tests a cell before its trees
// (scene_mesh_drawer_forest.cpp), the GPU planner takes the bounds as its
// static data (scene_forest_gpu_plan.cpp). And the pool's chunking both use.

#include "scene/forest_node.h"
#include "scene/vulkan/scene_mesh_drawer.h"
#include "util/parallel.h"

#include <bromath/aabb.h>
#include <bromath/frustum.h>
#include <bromath/mat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bro::scene::vk {

namespace forest {

// Trees [0, n) in chunks across the worker pool: `fn(chunk, begin, end)`.
// About 8 chunks a thread, of 256 to 2048 trees: a view's costly trees
// (the near ones, leaves and bark) stand together in the list, and
// in a few large chunks the frame waited on the one that held them (on
// foot, 24k trees seen in 12 chunks, the main thread waited 6 ms).
inline uint32_t chunkSize(size_t n) {
    const size_t want = n / (size_t{bro::util::parallelWidth()} * 8);
    return static_cast<uint32_t>(std::clamp<size_t>(want, 256, 2048));
}
inline uint32_t chunksOf(size_t n) {
    const uint32_t s = chunkSize(n);
    return static_cast<uint32_t>((n + s - 1) / s);
}
template <class Fn> void eachChunk(size_t n, Fn fn) {
    const uint32_t s = chunkSize(n);
    const auto run = [&](uint32_t c) {
        const uint32_t b = c * s;
        fn(c, b, static_cast<uint32_t>(std::min<size_t>(n, size_t{b} + s)));
    };
    bro::util::parallelChunks(chunksOf(n), run);
}

/// Per cell, the largest of its trees' (in metres): mean clump radius
/// (infinite for a tree with no clumps), thickest bark's keep radius,
/// largest bloom's extent, and their scale (m a specimen unit). What a far
/// cell is measured by (crownsAlone).
struct CellExtent {
    float clump = 0.0f, keep = 0.0f, bloom = 0.0f, unit = 0.0f;
};
/// `e` grown by tree `in` of `node` (`nodeScale` the node's world scale).
inline void growExtent(CellExtent& e, const ForestNode& node, const ForestNode::Instance& in, float nodeScale) {
    const ForestNode::Specimen& sp = node.specimens()[in.specimen];
    const float unit = in.scale * nodeScale;
    e.unit = std::max(e.unit, unit);
    e.clump = std::max(e.clump, sp.clumpCount > 0 ? sp.meanClumpRadius * unit : 1e30f);
    if (sp.segCount > 0) e.keep = std::max(e.keep, node.keepRadius()[sp.segStart] * unit);
    if (sp.bloomCount > 0) e.bloom = std::max(e.bloom, sp.bloomSize * ForestNode::bloomExtent(sp.bloomForm) * unit);
}
/// Whether a view of `lod` (with no forced tier) can have far cells at all:
/// a perspective one, unlimited in range, with a crown hand-over.
inline bool farCellsApply(const ForestNode::Lod& lod, int force, bool perspective) {
    return perspective && force < 0 && lod.range <= 0.0f && lod.crownFrom > lod.crownTo;
}
/// A far cell: every tree of a cell of extent `e` draws as a crown alone,
/// with no bark (nor the ribbons the bark's cut leaves) and no blooms, at
/// any detail (the budget only coarsens), outside the near sphere and the
/// canopy's 40 m, the cell's nearest point `d` from the eye (`across` of it
/// along the ground), `pxAtMetre` the view's pixels a metre at a metre.
/// Measured at the nearest point, which none of its trees is nearer than,
/// with a margin, so a tree on a test's edge goes with the trees that ask.
/// (scene_mesh_drawer_forest.cpp: planForest's far cells, and the far
/// cards' cells.)
bool crownsAlone(const ForestNode::Lod& lod, float leafLength, float pxAtMetre, const CellExtent& e, float d,
                 float across);

}  // namespace forest

struct SceneMeshDrawer::ForestCull {
    static constexpr float kCell = 32.0f;   // metres a side: a couple of dozen trees at a forest's spacing
    uint64_t generation = 0;
    bromath::Mat4 world;
    float reach = -1.0f;
    uint64_t used = 0;
    std::vector<bromath::AABB3> bounds;    // every tree's, world, padded by the reach
    std::vector<uint32_t> start, items;    // each cell's trees (ascending), cells end to end
    std::vector<bromath::AABB3> cellBox;   // the box round each cell's trees
    std::vector<uint8_t> mark;             // per tree: in a cell a test let through (2: a far cell's)
    // Per cell, its forest::CellExtent's fields: what far() measures a cell by.
    std::vector<float> cellClump, cellKeep, cellBloom;
    std::vector<float> cellUnit;

    void build(const ForestNode& node, const bromath::Mat4& w, float r) {
        const auto& instances = node.instances();
        const float nodeScale = std::sqrt(w.data[4] * w.data[4] + w.data[5] * w.data[5] + w.data[6] * w.data[6]);
        const size_t n = instances.size();
        bounds.resize(n);
        float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
        for (size_t i = 0; i < n; ++i) {
            bromath::AABB3& b = bounds[i];
            b = bromath::atransform(instances[i].bounds, w);
            b.min -= bromath::Vec3{r, r, r};
            b.max += bromath::Vec3{r, r, r};
            const float cx = (b.min.x + b.max.x) * 0.5f, cz = (b.min.z + b.max.z) * 0.5f;
            x0 = std::min(x0, cx); x1 = std::max(x1, cx);
            z0 = std::min(z0, cz); z1 = std::max(z1, cz);
        }
        const int gw = n ? static_cast<int>((x1 - x0) / kCell) + 1 : 0;
        const int gh = n ? static_cast<int>((z1 - z0) / kCell) + 1 : 0;
        const auto cellOf = [&](size_t i) {
            const bromath::AABB3& b = bounds[i];
            const int cx = std::clamp(static_cast<int>(((b.min.x + b.max.x) * 0.5f - x0) / kCell), 0, gw - 1);
            const int cz = std::clamp(static_cast<int>(((b.min.z + b.max.z) * 0.5f - z0) / kCell), 0, gh - 1);
            return static_cast<size_t>(cz) * gw + cx;
        };
        // A counting sort: each cell keeps its trees in the order given.
        start.assign(static_cast<size_t>(gw) * gh + 1, 0);
        for (size_t i = 0; i < n; ++i) start[cellOf(i) + 1]++;
        for (size_t c = 1; c < start.size(); ++c) start[c] += start[c - 1];
        items.resize(n);
        std::vector<uint32_t> fill(start.begin(), start.end() - 1);
        cellBox.assign(start.size() - 1, bromath::AABB3{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}});
        cellClump.assign(start.size() - 1, 0.0f);
        cellKeep.assign(start.size() - 1, 0.0f);
        cellBloom.assign(start.size() - 1, 0.0f);
        cellUnit.assign(start.size() - 1, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            const size_t c = cellOf(i);
            items[fill[c]++] = static_cast<uint32_t>(i);
            cellBox[c].min = bromath::vmin(cellBox[c].min, bounds[i].min);
            cellBox[c].max = bromath::vmax(cellBox[c].max, bounds[i].max);
            forest::CellExtent e{cellClump[c], cellKeep[c], cellBloom[c], cellUnit[c]};
            forest::growExtent(e, node, instances[i], nodeScale);
            cellClump[c] = e.clump;
            cellKeep[c] = e.keep;
            cellBloom[c] = e.bloom;
            cellUnit[c] = e.unit;
        }
        mark.assign(n, 0);
        world = w;
        reach = r;
    }
    // Mark the trees of every cell `hit` lets through (and no others): 2
    // those of the cells `far` names, else 1.
    // (Across the pool: a cell marks only its own trees.)
    template <class Hit, class Far> void markCells(Hit hit, Far far) {
        forest::eachChunk(mark.size(), [&](uint32_t, uint32_t b, uint32_t e) {
            std::fill(mark.begin() + b, mark.begin() + e, uint8_t{0});
        });
        const size_t cells = start.empty() ? 0 : start.size() - 1;
        forest::eachChunk(cells, [&](uint32_t, uint32_t b, uint32_t e) {
            for (uint32_t c = b; c < e; ++c) {
                if (start[c] == start[c + 1] || !hit(cellBox[c])) continue;
                const uint8_t m = far(c) ? 2 : 1;
                for (uint32_t k = start[c]; k < start[c + 1]; ++k) mark[items[k]] = m;
            }
        });
    }
    // The highest tree top in view within 40 m of `eye` across the ground
    // (-1e30 for none), as planForest's camera loop finds it: only the
    // cells that near are walked.
    float canopyTop(const bromath::Frustum& frustum, const bromath::Vec3& eye) const {
        float top = -1e30f;
        const size_t cells = start.empty() ? 0 : start.size() - 1;
        for (size_t c = 0; c < cells; ++c) {
            if (start[c] == start[c + 1]) continue;
            const bromath::AABB3& cb = cellBox[c];
            const float dx = std::max(std::max(cb.min.x - eye.x, eye.x - cb.max.x), 0.0f);
            const float dz = std::max(std::max(cb.min.z - eye.z, eye.z - cb.max.z), 0.0f);
            if (dx * dx + dz * dz >= 41.0f * 41.0f || !bromath::fintersects(frustum, cb)) continue;
            for (uint32_t k = start[c]; k < start[c + 1]; ++k) {
                const bromath::AABB3& b = bounds[items[k]];
                if (!bromath::fintersects(frustum, b)) continue;
                const bromath::Vec3 q{std::clamp(eye.x, b.min.x + reach, b.max.x - reach),
                                      std::clamp(eye.y, b.min.y + reach, b.max.y - reach),
                                      std::clamp(eye.z, b.min.z + reach, b.max.z - reach)};
                if (std::hypot(q.x - eye.x, q.z - eye.z) < 40.0f) top = std::max(top, b.max.y - reach);
            }
        }
        return top;
    }
};

}  // namespace bro::scene::vk
