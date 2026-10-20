#include "scene/plants_node.h"

#include "scene/texture_source.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene {

namespace {

// How much wider a cell's bounds are than its plants' boxes: the plan works
// each sphere out again on the GPU, a bit or two apart from the CPU's, and a
// plant a view sees must be in a cell it sees.
constexpr float kCellPad = 0.05f;

}  // namespace

PlantsNode::PlantsNode(const std::string& name) : MeshNode(name) {
    setCastsShadow(false);
    rebuild();
}

PlantsNode::~PlantsNode() = default;

void PlantsNode::setSpecies(const float* data, int count) {
    count = std::clamp(count, 0, kMaxSpecies);
    std::vector<float> plants(data_.begin() + static_cast<std::ptrdiff_t>(plantsAt_), data_.end());
    speciesCount_ = count;
    plantsAt_ = kHeaderFloats + static_cast<size_t>(count) * kSpeciesFloats;
    data_.assign(plantsAt_, 0.0f);
    std::memcpy(data_.data() + kHeaderFloats, data, static_cast<size_t>(count) * kSpeciesFloats * sizeof(float));
    data_.insert(data_.end(), plants.begin(), plants.end());
    rebuild();
}

void PlantsNode::setPlants(const float* data, int count) {
    count = std::max(count, 0);
    // Into cell order: a leaf cell's plants a run, a top's leaves together.
    std::vector<uint32_t> order;
    cells_ = sortIntoCells(grid_, static_cast<size_t>(count),
                           [&](size_t i) {
                               const float* p = data + i * kPlantFloats;
                               return bromath::Vec2{p[0], p[2]};
                           },
                           order);
    plantCount_ = count;
    data_.resize(plantsAt_ + static_cast<size_t>(count) * kPlantFloats);
    for (size_t k = 0; k < order.size(); ++k)
        std::memcpy(data_.data() + plantsAt_ + k * kPlantFloats, data + size_t{order[k]} * kPlantFloats,
                    kPlantFloats * sizeof(float));
    rebuild();
}

void PlantsNode::rebuild() {
    // The header (scene_plants.glsl's).
    data_.resize(std::max(data_.size(), size_t{kHeaderFloats}));
    std::fill(data_.begin(), data_.begin() + kHeaderFloats, 0.0f);
    data_[0] = static_cast<float>(plantCount_);
    data_[1] = static_cast<float>(speciesCount_);
    data_[3] = 0.0f;
    data_[4] = static_cast<float>(speciesCount_ * kSpeciesFloats / 4);

    // Each plant's sphere: the crown's reach and the leaves past its skin,
    // and the sway.
    const int n = plantCount_;
    std::vector<float> spheres(static_cast<size_t>(n) * 4);
    for (int i = 0; i < n; ++i) {
        const float* p = plant(i);
        int s = static_cast<int>(p[5]);
        if (s < 0 || s >= speciesCount_) s = 0;
        float height = 10.0f, radius = 4.0f;
        if (speciesCount_ > 0) {
            height = species(s)[0];
            radius = species(s)[2];
        }
        const float scale = p[4];
        float* o = spheres.data() + static_cast<size_t>(i) * 4;
        o[0] = p[0];
        o[1] = p[1] + 0.5f * height * scale;
        o[2] = p[2];
        o[3] = (std::sqrt(0.25f * height * height + radius * radius) + 0.15f * radius) * scale + 0.3f;
    }
    // Each cell's bounds: its plants' boxes.
    for (CellContent& c : cells_) {
        c.bounds = bromath::aempty3();
        for (uint32_t k = c.first; k < c.first + c.count; ++k) {
            const float* s = spheres.data() + size_t{k} * 4;
            const float r = s[3] + kCellPad;
            c.bounds.min = bromath::vmin(c.bounds.min, bromath::Vec3{s[0] - r, s[1] - r, s[2] - r});
            c.bounds.max = bromath::vmax(c.bounds.max, bromath::Vec3{s[0] + r, s[1] + r, s[2] + r});
        }
    }
    // (Unique across nodes: the drawer keys its state by the node.)
    generation_ = nextResourceGeneration();
}

}  // namespace bro::scene
