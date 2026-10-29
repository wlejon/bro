// Land natives: SceneGraph.createLand and the LandNode members behind
// js/scene_land.js (its module, extent and rings, heights, stats).

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/scene/native_scene_decl.h"

#include "scene/nodes/land_node.h"

#include <algorithm>
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

const char* bro_scene_SceneNode_setLandRelief(void* self, const char* trees, const double* seeds, uint32_t seeds_len) {
    static thread_local std::string tl_error;
    tl_error.clear();
    auto* land = landOf(self);
    if (!land) return tl_error.c_str();
    // (One tree a line, a level each.)
    std::vector<std::string> list;
    const std::string all = trees ? std::string(trees) : std::string();
    for (size_t at = 0; !all.empty() && at <= all.size();) {
        const size_t nl = std::min(all.find('\n', at), all.size());
        list.push_back(all.substr(at, nl - at));
        at = nl + 1;
    }
    std::vector<int> s;
    for (uint32_t k = 0; k < seeds_len; ++k) s.push_back(static_cast<int>(seeds[k]));
    land->setRelief(list, s, tl_error);
    return tl_error.c_str();
}

const char* bro_scene_SceneNode_setLandProcess(void* self, const char* source, const char* passes,
                                               const double* params, uint32_t params_len, const float* data,
                                               uint32_t data_len) {
    static thread_local std::string tl_error;
    tl_error.clear();
    auto* land = landOf(self);
    if (!land) return tl_error.c_str();
    scene::LandProcess p;
    p.source = source ? std::string(source) : std::string();
    if (params_len > 0) p.resolution = static_cast<int>(params[0]);
    if (params_len > 1) p.channels = static_cast<int>(params[1]);
    // The passes' functions, comma-separated, their iterations after the
    // resolution and channels; then (after a '|') the regions' init and
    // (after another) their passes, the regions' cells, apron, cell, ratio,
    // levels, reach, keep and conserve after the global passes' iterations, their
    // passes' iterations after those.
    const std::string all = passes ? std::string(passes) : std::string();
    const size_t bar1 = all.find('|');
    const size_t bar2 = bar1 == std::string::npos ? std::string::npos : all.find('|', bar1 + 1);
    const auto split = [](const std::string& names, std::vector<std::string>& out) {
        for (size_t at = 0; !names.empty() && at <= names.size();) {
            const size_t comma = std::min(names.find(',', at), names.size());
            out.push_back(names.substr(at, comma - at));
            at = comma + 1;
        }
    };
    std::vector<std::string> global, region;
    split(all.substr(0, bar1), global);
    uint32_t k = 2;
    const auto next = [&](double fallback) { return k < params_len ? params[k++] : (++k, fallback); };
    for (const std::string& n : global) p.passes.push_back({n, static_cast<int>(next(1.0))});
    if (bar2 != std::string::npos) {
        scene::LandRegionConfig& r = p.regions;
        r.init = all.substr(bar1 + 1, bar2 - bar1 - 1);
        split(all.substr(bar2 + 1), region);
        r.cells = static_cast<int>(next(r.cells));
        r.apron = static_cast<int>(next(r.apron));
        r.cell = next(r.cell);
        r.ratio = static_cast<int>(next(r.ratio));
        r.levels = static_cast<int>(next(r.levels));
        r.reach = static_cast<int>(next(r.reach));
        r.keep = static_cast<int>(next(r.keep));
        r.conserve = static_cast<uint32_t>(next(r.conserve));
        for (const std::string& n : region) r.passes.push_back({n, static_cast<int>(next(1.0))});
    }
    if (data && data_len) p.data.assign(data, data + data_len);
    land->setProcess(std::move(p), tl_error);
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
    // The sea (scene_land.js): on, level, absorb rgb, scatter rgb, roughness.
    if (given(4)) {
        scene::LandNode::Sea s = land->sea();
        s.on = params[4] > 0.0;
        if (given(5)) s.level = static_cast<float>(params[5]);
        for (int k = 0; k < 3; ++k) {
            if (given(6 + k)) s.absorb[k] = static_cast<float>(params[6 + k]);
            if (given(9 + k)) s.scatter[k] = static_cast<float>(params[9 + k]);
        }
        if (given(12)) s.roughness = static_cast<float>(params[12]);
        land->setSea(s);
    }
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
    double o[3];
    placeOriginOfNode(self, o);   // (x, z) is the place frame's; the land the body's
    x += o[0];
    z += o[2];
    if (!land || (!land->round() && (std::fabs(x) > land->extent() || std::fabs(z) > land->extent())))
        return std::numeric_limits<double>::quiet_NaN();
    return land->heightAt(static_cast<float>(x), static_cast<float>(z));
}

void bro_scene_SceneNode_landStats(void* self, bronze_native_buffer* out) {
    auto* land = landOf(self);
    if (!land) { transferBuffer(std::vector<double>{}, out); return; }
    const scene::LandNode::Stats& s = land->stats();
    transferBuffer(std::vector<double>{static_cast<double>(s.needed), static_cast<double>(s.resident),
                                       static_cast<double>(s.missing), static_cast<double>(s.filled),
                                       static_cast<double>(s.levels), s.shortfall, s.compiled ? 1.0 : 0.0,
                                       land->ready() ? 1.0 : 0.0, static_cast<double>(s.regions),
                                       static_cast<double>(s.regionsPending), land->groundReady() ? 1.0 : 0.0},
                   out);
}
