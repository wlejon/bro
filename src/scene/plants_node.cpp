#include "scene/plants_node.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

PlantsNode::PlantsNode(const std::string& name) : MeshNode(name) {
    setCastsShadow(false);
}

PlantsNode::~PlantsNode() = default;

void PlantsNode::setSpecies(const float* data, int count) {
    count = std::clamp(count, 0, kMaxSpecies);
    species_.assign(data, data + static_cast<size_t>(count) * kSpeciesFloats);
    updateSpheres();
    ++generation_;
}

void PlantsNode::setPlants(const float* data, int count) {
    count = std::max(count, 0);
    plants_.assign(data, data + static_cast<size_t>(count) * kPlantFloats);
    updateSpheres();
    ++generation_;
}

void PlantsNode::updateSpheres() {
    const int n = plantCount();
    const int ns = speciesCount();
    spheres_.resize(static_cast<size_t>(n) * 4);
    for (int i = 0; i < n; ++i) {
        const float* p = plants_.data() + static_cast<size_t>(i) * kPlantFloats;
        int s = static_cast<int>(p[5]);
        if (s < 0 || s >= ns) s = 0;
        float height = 10.0f, radius = 4.0f;
        if (ns > 0) {
            const float* sp = species_.data() + static_cast<size_t>(s) * kSpeciesFloats;
            height = sp[0];
            radius = sp[2];
        }
        const float scale = p[4];
        float* o = spheres_.data() + static_cast<size_t>(i) * 4;
        o[0] = p[0];
        o[1] = p[1] + 0.5f * height * scale;
        o[2] = p[2];
        // The crown's reach and the leaves past its skin, and the sway.
        o[3] = (std::sqrt(0.25f * height * height + radius * radius) + 0.15f * radius) * scale + 0.3f;
    }
}

}  // namespace bro::scene
