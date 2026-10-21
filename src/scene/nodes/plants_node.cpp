#include "scene/nodes/plants_node.h"

#include "scene/texture_source.h"
#include "scene/world/plant_placement.h"
#include "scene/world/world_workers.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene {

namespace {

// How much wider a cell's bounds are than its plants' boxes: the plan works
// each sphere out again on the GPU, a bit or two apart from the CPU's, and a
// plant a view sees must be in a cell it sees.
constexpr float kCellPad = 0.05f;
// The change log's length (a changed leaf an entry); further back is "all".
constexpr size_t kChangeLog = 8192;

}  // namespace

PlantsNode::PlantsNode(const std::string& name) : MeshNode(name) {
    setCastsShadow(false);
    writeHeader();
    generation_ = nextResourceGeneration();
    changesFloor_ = generation_;
}

PlantsNode::~PlantsNode() = default;

void PlantsNode::writeHeader() {
    float* h = store_.prefix();
    std::fill(h, h + kHeaderFloats, 0.0f);
    h[0] = static_cast<float>(store_.capacity());
    h[1] = static_cast<float>(speciesCount_);
    h[3] = 0.0f;
    h[4] = static_cast<float>(kMaxSpecies * kSpeciesFloats / 4);
    store_.prefixChanged();
}

void PlantsNode::setSpecies(const float* data, int count) {
    count = std::clamp(count, 0, kMaxSpecies);
    speciesCount_ = count;
    float* table = store_.prefix() + kHeaderFloats;
    std::fill(table, table + kMaxSpecies * kSpeciesFloats, 0.0f);
    std::memcpy(table, data, static_cast<size_t>(count) * kSpeciesFloats * sizeof(float));
    writeHeader();
    // Every leaf's bounds follow its species'.
    tallest_ = 0.0f;
    for (auto& [key, leaf] : leaves_) {
        const auto run = store_.runs().find(key.packed());
        if (run != store_.runs().end()) leaf.bounds = boundsOf(run->second.first, run->second.count);
    }
    changedAll();
}

void PlantsNode::setPlants(const float* data, int count) {
    for (auto it = leaves_.begin(); it != leaves_.end();) {
        it->second.fixed.clear();
        auto next = std::next(it);
        putLeaf(it, nullptr);
        it = next;
    }
    for (int i = 0; i < std::max(count, 0); ++i) {
        const float* p = data + static_cast<size_t>(i) * kPlantFloats;
        const CellKey key = grid_.leafAt(p[0], p[2]);
        auto it = leaves_.try_emplace(key).first;
        it->second.fixed.insert(it->second.fixed.end(), p, p + kPlantFloats);
    }
    for (auto it = leaves_.begin(); it != leaves_.end();) {
        auto next = std::next(it);
        putLeaf(it, nullptr);
        it = next;
    }
}

void PlantsNode::setRules(std::shared_ptr<const PlacementRules> rules) {
    // The placed plants go with the old rules.
    placement_.reset();
    const std::vector<float> none;
    for (auto it = leaves_.begin(); it != leaves_.end();) {
        auto next = std::next(it);
        if (it->second.placed > 0) putLeaf(it, &none);
        it = next;
    }
    if (rules) placement_ = std::make_unique<PlantPlacement>(std::move(rules), grid_, WorldWorkers::shared());
}

const PlacementRules* PlantsNode::rules() const { return placement_ ? &placement_->rules() : nullptr; }

size_t PlantsNode::placementPending() const { return placement_ ? placement_->pending() : 0; }

void PlantsNode::follow(double eyeX, double eyeZ) {
    store_.tick();
    if (!placement_) return;
    std::vector<PlantPlacement::Placed> placed;
    std::vector<CellKey> dropped;
    placement_->update(eyeX, eyeZ, placed, dropped);
    const std::vector<float> none;
    for (const CellKey& key : dropped) {
        const auto it = leaves_.find(key);
        if (it != leaves_.end()) putLeaf(it, &none);
    }
    for (const PlantPlacement::Placed& p : placed) putLeaf(leaves_.try_emplace(p.key).first, &p.records);
}

void PlantsNode::putLeaf(Leaves::iterator it, const std::vector<float>* placed) {
    Leaf& leaf = it->second;
    const CellKey key = it->first;
    std::vector<float> records = leaf.fixed;
    if (placed) {
        records.insert(records.end(), placed->begin(), placed->end());
        leaf.placed = static_cast<uint32_t>(placed->size() / kPlantFloats);
    } else if (leaf.placed > 0) {
        // The placed ones it has follow its fixed ones in its run.
        const CellStore::Run& run = store_.runs().at(key.packed());
        const float* from = store_.record(run.first + run.count - leaf.placed);
        records.insert(records.end(), from, from + size_t{leaf.placed} * kPlantFloats);
    }
    const bromath::AABB3 old = leaf.bounds;
    const uint32_t count = static_cast<uint32_t>(records.size() / kPlantFloats);
    const uint32_t capacity = store_.capacity();
    store_.put(key, records.data(), count);
    if (store_.capacity() != capacity) writeHeader();
    if (count > 0) {
        const CellStore::Run& run = store_.runs().at(key.packed());
        leaf.bounds = boundsOf(run.first, run.count);
    } else {
        leaves_.erase(it);
    }
    // The change: the leaf's old bounds and its new.
    generation_ = nextResourceGeneration();
    bromath::AABB3 box = old;
    if (count > 0) box = bromath::aisEmpty(old) ? leaf.bounds : bromath::amerge(old, leaf.bounds);
    if (!bromath::aisEmpty(box)) changes_.push_back({generation_, box});
    if (changes_.size() > kChangeLog) {
        const size_t cut = changes_.size() - kChangeLog / 2;
        changesFloor_ = changes_[cut - 1].generation;
        changes_.erase(changes_.begin(), changes_.begin() + static_cast<std::ptrdiff_t>(cut));
    }
    cellsDirty_ = true;
}

bromath::AABB3 PlantsNode::boundsOf(uint32_t first, uint32_t count) {
    // Each plant's sphere: the crown's reach and the leaves past its skin,
    // and the sway.
    bromath::AABB3 b = bromath::aempty3();
    for (uint32_t k = first; k < first + count; ++k) {
        const float* p = store_.record(k);
        int s = static_cast<int>(p[5]);
        if (s < 0 || s >= speciesCount_) s = 0;
        float height = 10.0f, radius = 4.0f;
        if (speciesCount_ > 0) {
            height = species(s)[0];
            radius = species(s)[2];
        }
        const float scale = p[4];
        tallest_ = std::max(tallest_, height * scale);
        const float cy = p[1] + 0.5f * height * scale;
        const float r = (std::sqrt(0.25f * height * height + radius * radius) + 0.15f * radius) * scale + 0.3f +
                        kCellPad;
        b.min = bromath::vmin(b.min, bromath::Vec3{p[0] - r, cy - r, p[2] - r});
        b.max = bromath::vmax(b.max, bromath::Vec3{p[0] + r, cy + r, p[2] + r});
    }
    return b;
}

void PlantsNode::changedAll() {
    generation_ = nextResourceGeneration();
    changes_.clear();
    changesFloor_ = generation_;
    cellsDirty_ = true;
}

bool PlantsNode::changesSince(uint64_t generation, std::vector<bromath::AABB3>& out) const {
    out.clear();
    if (generation < changesFloor_) return false;
    const auto from = std::upper_bound(changes_.begin(), changes_.end(), generation,
                                       [](uint64_t g, const Change& c) { return g < c.generation; });
    for (auto it = from; it != changes_.end(); ++it) out.push_back(it->bounds);
    return true;
}

const std::vector<CellContent>& PlantsNode::cells() const {
    if (!cellsDirty_) return cells_;
    cellsDirty_ = false;
    cells_.clear();
    cells_.reserve(leaves_.size());
    for (const auto& [key, leaf] : leaves_) {
        const auto run = store_.runs().find(key.packed());
        if (run == store_.runs().end()) continue;
        CellContent c;
        c.key = key;
        c.first = run->second.first;
        c.count = run->second.count;
        c.bounds = leaf.bounds;
        cells_.push_back(c);
    }
    return cells_;
}

}  // namespace bro::scene
