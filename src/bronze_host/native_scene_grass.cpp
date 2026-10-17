// Grass natives: SceneGraph.createGrass and the GrassNode members behind
// js/scene_grass.js (field, params, movers, pushes, stats), plus the mesh
// node's sampleHeights / meshWorldBounds the wrapper bakes a field from.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/scene/native_scene_decl.h"

#include "scene/grass_node.h"

#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace bro::bronze_host;

namespace {

scene::GrassNode* grassOf(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return nullptr;
    return static_cast<scene::MeshNode*>(n)->asGrass();
}

scene::MeshNode* meshOf(void* self) {
    auto* n = nodeOf(self);
    return (n && n->type() == scene::SceneNode::Type::Mesh) ? static_cast<scene::MeshNode*>(n) : nullptr;
}

void worldPoint(const bromath::Mat4& m, const float* p, float* o) {
    for (int r = 0; r < 3; ++r) o[r] = m.at(r, 0) * p[0] + m.at(r, 1) * p[1] + m.at(r, 2) * p[2] + m.at(r, 3);
}

}  // namespace

void* bro_scene_SceneGraph_createGrass(void* self) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createGrass();
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void bro_scene_SceneNode_setGrassField(void* self, const float* data, uint32_t data_len, int32_t cols, int32_t rows,
                                       double x0, double z0, double cell) {
    auto* gr = grassOf(self);
    if (!gr || cols < 2 || rows < 2 || !(cell > 0.0)) return;
    if (static_cast<uint64_t>(data_len) < static_cast<uint64_t>(cols) * static_cast<uint64_t>(rows) * 4u) return;
    gr->setField(data, cols, rows, static_cast<float>(x0), static_cast<float>(z0), static_cast<float>(cell));
}

void bro_scene_SceneNode_setGrassFloor(void* self, const float* data, uint32_t data_len) {
    auto* gr = grassOf(self);
    if (!gr) return;
    const uint64_t need = static_cast<uint64_t>(gr->fieldCols()) * static_cast<uint64_t>(gr->fieldRows());
    if (!data || need == 0 || static_cast<uint64_t>(data_len) < need) gr->setFloor(nullptr, 0, 0);
    else gr->setFloor(data, gr->fieldCols(), gr->fieldRows(), static_cast<uint64_t>(data_len) >= need * 2 ? 2 : 1);
}

void bro_scene_SceneNode_setGrassHerbs(void* self, const float* data, uint32_t data_len, double range, double far) {
    auto* gr = grassOf(self);
    if (!gr) return;
    const int count = static_cast<int>(data_len / scene::GrassNode::kHerbRecordFloats);
    gr->setHerbs(data, count, std::isfinite(range) ? static_cast<float>(range) : 0.0f,
                 std::isfinite(far) ? static_cast<float>(far) : 0.0f);
}

void bro_scene_SceneNode_grassHerbPlants(void* self, double x, double z, double radius, bronze_native_buffer* out) {
    auto* gr = grassOf(self);
    std::vector<double> v;
    if (gr && std::isfinite(x) && std::isfinite(z) && radius > 0.0) {
        for (const scene::GrassNode::HerbPlant& p :
             gr->herbPlants(static_cast<float>(x), static_cast<float>(z), static_cast<float>(std::min(radius, 20.0)), 256)) {
            v.insert(v.end(), {p.x, p.y, p.z, static_cast<double>(p.species)});
        }
    }
    transferBuffer(std::move(v), out);
}

void bro_scene_SceneNode_setTurfGrass(void* self, void* grass, int32_t on) {
    auto* m = meshOf(self);
    if (!m) return;
    auto* gr = on ? grassOf(grass) : nullptr;
    m->setTurfGrass(gr ? gr->id() : 0u);
}

void bro_scene_SceneNode_setGrassParams(void* self, const double* params, uint32_t params_len) {
    if (auto* gr = grassOf(self)) gr->paramsFromArray(params, params_len);
}

void bro_scene_SceneNode_grassParams(void* self, bronze_native_buffer* out) {
    auto* gr = grassOf(self);
    if (!gr) { transferBuffer(std::vector<double>{}, out); return; }
    float v[scene::GrassNode::kParamCount];
    gr->paramsToArray(v);
    transferBuffer(std::vector<double>(std::begin(v), std::end(v)), out);
}

void bro_scene_SceneNode_setGrassMover(void* self, void* target, double radius, double strength, double trample) {
    auto* gr = grassOf(self);
    auto* t = nodeOf(target);
    if (!gr || !t) return;
    gr->setMover(t->id(), static_cast<float>(radius), static_cast<float>(strength), static_cast<float>(trample));
}

void bro_scene_SceneNode_grassPush(void* self, double x, double z, double radius, double vx, double vz,
                                   double strength, double trample) {
    if (auto* gr = grassOf(self))
        gr->push(static_cast<float>(x), static_cast<float>(z), static_cast<float>(radius), static_cast<float>(vx),
                 static_cast<float>(vz), static_cast<float>(strength), static_cast<float>(trample));
}

void bro_scene_SceneNode_grassStats(void* self, bronze_native_buffer* out) {
    auto* gr = grassOf(self);
    if (!gr) { transferBuffer(std::vector<double>{}, out); return; }
    std::vector<double> v;
    v.push_back(scene::GrassNode::kRings);
    for (int i = 0; i < scene::GrassNode::kRings; ++i) v.push_back(gr->drawnInstances(i));
    v.push_back(static_cast<double>(gr->moverCount()));
    v.push_back(gr->fieldCols());
    v.push_back(gr->fieldRows());
    for (int i = 0; i < scene::GrassNode::kHerbRings; ++i) v.push_back(gr->drawnHerbs(i));
    transferBuffer(std::move(v), out);
}

void bro_scene_SceneNode_sampleHeights(void* self, double x0d, double z0d, double celld, int32_t cols, int32_t rows,
                                       bronze_native_buffer* out) {
    auto* m = meshOf(self);
    if (!m || m->asGrass() || cols <= 0 || rows <= 0 || !(celld > 0.0) ||
        static_cast<uint64_t>(cols) * static_cast<uint64_t>(rows) > (1u << 26)) {
        transferBuffer(std::vector<float>{}, out);
        return;
    }
    const float x0 = static_cast<float>(x0d), z0 = static_cast<float>(z0d), cell = static_cast<float>(celld);
    std::vector<float> h(static_cast<size_t>(cols) * rows, std::numeric_limits<float>::quiet_NaN());
    const bromesh::MeshData& md = m->mesh();
    const bromath::Mat4& wm = m->worldMatrix();
    const size_t nv = md.positions.size() / 3;
    std::vector<float> wp(nv * 3);
    for (size_t i = 0; i < nv; ++i) worldPoint(wm, &md.positions[i * 3], &wp[i * 3]);

    const auto tri = [&](uint32_t ia, uint32_t ib, uint32_t ic) {
        if (ia >= nv || ib >= nv || ic >= nv) return;
        const float* a = &wp[ia * 3];
        const float* b = &wp[ib * 3];
        const float* c = &wp[ic * 3];
        const float det = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
        if (std::fabs(det) < 1e-12f) return;   // edge-on from above
        const float minX = std::min({a[0], b[0], c[0]}), maxX = std::max({a[0], b[0], c[0]});
        const float minZ = std::min({a[2], b[2], c[2]}), maxZ = std::max({a[2], b[2], c[2]});
        const int i0 = std::max(0, static_cast<int>(std::ceil((minX - x0) / cell - 1e-4f)));
        const int i1 = std::min(cols - 1, static_cast<int>(std::floor((maxX - x0) / cell + 1e-4f)));
        const int j0 = std::max(0, static_cast<int>(std::ceil((minZ - z0) / cell - 1e-4f)));
        const int j1 = std::min(rows - 1, static_cast<int>(std::floor((maxZ - z0) / cell + 1e-4f)));
        const float inv = 1.0f / det;
        constexpr float eps = -1e-5f;
        for (int j = j0; j <= j1; ++j) {
            const float z = z0 + j * cell;
            for (int i = i0; i <= i1; ++i) {
                const float x = x0 + i * cell;
                const float u = ((x - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (z - a[2])) * inv;
                const float v = ((b[0] - a[0]) * (z - a[2]) - (x - a[0]) * (b[2] - a[2])) * inv;
                if (u < eps || v < eps || u + v > 1.0f - eps) continue;
                const float y = a[1] + u * (b[1] - a[1]) + v * (c[1] - a[1]);
                float& dst = h[static_cast<size_t>(j) * cols + i];
                if (std::isnan(dst) || y > dst) dst = y;   // the top surface
            }
        }
    };
    if (!md.indices.empty()) {
        for (size_t t = 0; t + 2 < md.indices.size(); t += 3) tri(md.indices[t], md.indices[t + 1], md.indices[t + 2]);
    } else {
        for (uint32_t t = 0; t + 2 < nv; t += 3) tri(t, t + 1, t + 2);
    }
    transferBuffer(std::move(h), out);
}

void bro_scene_SceneNode_meshWorldBounds(void* self, bronze_native_buffer* out) {
    auto* m = meshOf(self);
    if (!m || m->asGrass() || m->mesh().positions.size() < 3) { transferBuffer(std::vector<double>{}, out); return; }
    const auto& pos = m->mesh().positions;
    const bromath::Mat4& wm = m->worldMatrix();
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (size_t i = 0; i + 2 < pos.size(); i += 3) {
        float w[3];
        worldPoint(wm, &pos[i], w);
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], static_cast<double>(w[k]));
            hi[k] = std::max(hi[k], static_cast<double>(w[k]));
        }
    }
    transferBuffer(std::vector<double>{lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]}, out);
}
