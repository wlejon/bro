// Land natives: SceneGraph.createLand and the LandNode members behind
// js/scene_land.js (its module, extent and rings, heights, stats).

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/scene/native_scene_decl.h"

#include "scene/nodes/land_node.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace bro::bronze_host;

namespace {

scene::LandNode* landOf(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return nullptr;
    return static_cast<scene::MeshNode*>(n)->asLand();
}

}  // namespace

void* bro_scene_SceneGraph_createLand(void* self) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createLand();
    g->root()->addChild(node);
    return wrapNode(node, g);
}

const char* bro_scene_SceneNode_setLandModule(void* self, const char* source, const float* data, uint32_t data_len,
                                              const char* heightTree, int32_t seed) {
    static thread_local std::string tl_error;
    tl_error.clear();
    if (auto* land = landOf(self))
        land->setModule(source ? std::string(source) : std::string(), data, data_len,
                        heightTree ? std::string(heightTree) : std::string(), seed, tl_error);
    return tl_error.c_str();
}

void bro_scene_SceneNode_setLandParams(void* self, const double* params, uint32_t params_len) {
    auto* land = landOf(self);
    if (!land) return;
    const auto given = [&](uint32_t i) { return i < params_len && !std::isnan(params[i]); };
    if (given(0)) land->setExtent(static_cast<float>(params[0]));
    scene::LandNode::Rings r = land->rings();
    if (given(1)) r.cellSize = static_cast<float>(params[1]);
    if (given(2)) r.resolution = static_cast<int>(params[2]);
    if (given(3)) r.levels = static_cast<int>(params[3]);
    if (given(1) || given(2) || given(3)) land->setRings(r);
}

void bro_scene_SceneNode_landParams(void* self, bronze_native_buffer* out) {
    auto* land = landOf(self);
    if (!land) { transferBuffer(std::vector<double>{}, out); return; }
    const scene::LandNode::Rings& r = land->rings();
    transferBuffer(std::vector<double>{land->extent(), r.cellSize, static_cast<double>(r.resolution),
                                       static_cast<double>(r.levels)},
                   out);
}

double bro_scene_SceneNode_landHeight(void* self, double x, double z) {
    auto* land = landOf(self);
    if (!land || std::fabs(x) > land->extent() || std::fabs(z) > land->extent())
        return std::numeric_limits<double>::quiet_NaN();
    return land->heightAt(static_cast<float>(x), static_cast<float>(z));
}

void bro_scene_SceneNode_landStats(void* self, bronze_native_buffer* out) {
    auto* land = landOf(self);
    if (!land) { transferBuffer(std::vector<double>{}, out); return; }
    const scene::LandNode::Stats& s = land->stats();
    transferBuffer(std::vector<double>{static_cast<double>(s.needed), static_cast<double>(s.resident),
                                       static_cast<double>(s.missing), static_cast<double>(s.filled),
                                       static_cast<double>(s.levels), s.shortfall, s.compiled ? 1.0 : 0.0},
                   out);
}
