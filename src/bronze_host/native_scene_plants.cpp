// Plants natives: SceneGraph.createPlants and the PlantsNode members behind
// js/scene_plants.js (species, plants, placement rules, params, stats).

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_runtime.h"
#include "engine/engine.h"
#include "natives/scene/native_scene_decl.h"

#include "scene/nodes/land_node.h"
#include "scene/nodes/plants_node.h"
#include "scene/world/plant_canopy.h"
#include "scene/world/plant_placement.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace bro::bronze_host;

namespace {

scene::PlantsNode* plantsOf(void* self) {
    auto* n = nodeOf(self);
    if (!n || n->type() != scene::SceneNode::Type::Mesh) return nullptr;
    return static_cast<scene::MeshNode*>(n)->asPlants();
}

}  // namespace

void* bro_scene_SceneGraph_createPlants(void* self) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createPlants();
    g->root()->addChild(node);
    return wrapNode(node, g);
}

void bro_scene_SceneNode_setPlantModule(void* self, const char* source) {
    if (auto* pl = plantsOf(self)) pl->setModule(source ? std::string(source) : std::string());
}

void bro_scene_SceneNode_setPlantSpecies(void* self, const float* data, uint32_t data_len) {
    if (auto* pl = plantsOf(self))
        pl->setSpecies(data, static_cast<int>(data_len / scene::PlantsNode::kSpeciesFloats));
}

void bro_scene_SceneNode_setPlants(void* self, const float* data, uint32_t data_len) {
    auto* pl = plantsOf(self);
    if (!pl) return;
    const int count = static_cast<int>(data_len / scene::PlantsNode::kPlantFloats);
    // The script's feet are the place frame's; the plants are the body's.
    double o[3];
    placeOriginOfNode(self, o);
    if (o[0] == 0.0 && o[1] == 0.0 && o[2] == 0.0) {
        pl->setPlants(data, count);
        return;
    }
    std::vector<float> body(data, data + static_cast<size_t>(count) * scene::PlantsNode::kPlantFloats);
    for (int i = 0; i < count; ++i)
        for (int k = 0; k < 3; ++k) body[static_cast<size_t>(i) * scene::PlantsNode::kPlantFloats + k] += static_cast<float>(o[k]);
    pl->setPlants(body.data(), count);
}

const char* bro_scene_SceneNode_setPlantRules(void* self, const char* json) {
    static thread_local std::string tl_error;
    tl_error.clear();
    auto* pl = plantsOf(self);
    if (!pl) return tl_error.c_str();
    if (!json || !*json) {
        pl->setRules(nullptr);
        return tl_error.c_str();
    }
    auto rules = std::make_shared<scene::PlacementRules>();
    if (!scene::parsePlacementRules(json, *rules, &tl_error)) return tl_error.c_str();
    pl->setRules(std::move(rules));
    return tl_error.c_str();
}

const char* bro_scene_SceneNode_setPlantLand(void* self, void* landSelf, bool on) {
    static thread_local std::string tl_error;
    tl_error.clear();
    auto* pl = plantsOf(self);
    auto* n = nodeOf(landSelf);
    auto* land = n && n->type() == scene::SceneNode::Type::Mesh ? static_cast<scene::MeshNode*>(n)->asLand() : nullptr;
    if (!pl || !land) return tl_error.c_str();
    if (!on) {
        land->setCanopy({}, 0.0f, 0.0f, 0.0f);
        return tl_error.c_str();
    }
    std::string module;
    float top = 0.0f;
    if (const scene::PlacementRules* rules = pl->rules()) {
        if (!scene::plantCanopyModule(*rules, pl->species(0), pl->speciesCount(), scene::PlantsNode::kSpeciesFloats,
                                      module, tl_error))
            return tl_error.c_str();
        // (The land's bounds: twice the tallest species, for a size field
        // past 1.)
        for (int s = 0; s < pl->speciesCount(); ++s) top = std::max(top, 2.0f * pl->species(s)[0]);
    }
    const auto& p = pl->params();
    land->setCanopy(std::move(module), top, p.range, std::min(p.shadowRange, p.range));
    return tl_error.c_str();
}

void bro_scene_SceneNode_plantPlacement(void* self, bronze_native_buffer* out) {
    auto* pl = plantsOf(self);
    if (!pl) { transferBuffer(std::vector<double>{}, out); return; }
    transferBuffer(std::vector<double>{static_cast<double>(pl->cells().size()),
                                       static_cast<double>(pl->placementPending())},
                   out);
}

void bro_scene_SceneNode_setPlantParams(void* self, const double* params, uint32_t params_len) {
    auto* pl = plantsOf(self);
    if (!pl) return;
    scene::PlantsNode::Params p = pl->params();
    float* fields[scene::PlantsNode::kParamCount] = {&p.pixels, &p.minElements, &p.woodRange, &p.range,
                                                         &p.shadowTexels, &p.shadowRange};
    for (uint32_t i = 0; i < params_len && i < static_cast<uint32_t>(scene::PlantsNode::kParamCount); ++i)
        if (std::isfinite(params[i])) *fields[i] = static_cast<float>(params[i]);
    pl->setParams(p);
}

void bro_scene_SceneNode_plantStats(void* self, bronze_native_buffer* out) {
    // What the last frame drew: a frame a flush left pending is drawn first.
    if (auto* eng = hostEngine()) eng->renderPendingScenes(/*forRead=*/true);
    auto* pl = plantsOf(self);
    if (!pl) { transferBuffer(std::vector<double>{}, out); return; }
    const scene::PlantsNode::Stats& s = pl->stats();
    transferBuffer(std::vector<double>{static_cast<double>(pl->plantCount()), static_cast<double>(s.plants),
                                       static_cast<double>(s.elements), static_cast<double>(s.limbs),
                                       static_cast<double>(s.shadowElements)},
                   out);
}
