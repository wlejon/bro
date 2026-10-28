#include "scene/nodes/plants_node.h"
#include "scene/nodes/land_node.h"

#include "scene/nodes/world_module.h"
#include "scene/texture_source.h"
#include "scene/world/plant_placement.h"
#include "scene/world/world_workers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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
        if (run != store_.runs().end()) measure(leaf, run->second.first, run->second.count);
    }
    changedAll();
}

void PlantsNode::setModule(std::string source) {
    moduleSource_ = std::move(source);
    moduleSourceHash_ = scene::moduleSourceHash(moduleSource_);
}

void PlantsNode::setPlants(const float* data, int count) {
    for (auto it = leaves_.begin(); it != leaves_.end();) {
        it->second.fixed.clear();
        auto next = std::next(it);
        putLeaf(it, nullptr);
        it = next;
    }
    // (On a round body the plants set by hand are kept apart from the
    // faces' leaves, by where they stand in the place frame.)
    const uint8_t root = radius_ > 0.0 ? kHandRoot : 0;
    for (int i = 0; i < std::max(count, 0); ++i) {
        const float* p = data + static_cast<size_t>(i) * kPlantFloats;
        const CellKey key = grid_.leafAt(p[0], p[2], root);
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
    rulesRef_ = rules;
    if (!rules || hold_) return;
    // Standing on a land with processes: their height under the rules' ground.
    if (base_) {
        auto withBase = std::make_shared<PlacementRules>(*rules);
        withBase->base = base_;
        rules = std::move(withBase);
    }
    placement_ = std::make_unique<PlantPlacement>(std::move(rules), grid_, WorldWorkers::shared(),
                                                  radius_ > 0.0 ? &place_->body() : nullptr);
}

void PlantsNode::setGround(std::shared_ptr<const LandBase> base, bool hold) {
    if (base == base_) {
        if (hold == hold_) return;
        // Held, the placement posts no new cells (it follows the eye, its
        // reach and what it drops, and what it placed stays: the eye
        // outrunning the regions under it on a descent dropped every plant
        // and placed them all again once they were in); let go, it goes on,
        // begun if it never was.
        hold_ = hold;
        if (!hold_ && !placement_) setRules(rulesRef_);
        return;
    }
    base_ = std::move(base);
    hold_ = hold;
    setRules(rulesRef_);
}

const PlacementRules* PlantsNode::rules() const { return rulesRef_.get(); }

size_t PlantsNode::placementPending() const {
    const size_t n = placement_ ? placement_->pending() : 0;
    return rulesRef_ && hold_ ? std::max<size_t>(n, 1) : n;
}

float PlantsNode::drawReach() const {
    // (Placed by fixed records alone, with no rules: all of it is there.)
    if (!rulesRef_) return real_;
    return placement_ ? std::min(real_, static_cast<float>(placement_->reach())) : 0.0f;
}

void PlantsNode::pendingWork(std::vector<std::string>& out, double within) const {
    const size_t n = placementPending();
    if (!n) return;
    if (const size_t again = placement_ ? placement_->replacing() : 0)
        out.push_back("plants: " + std::to_string(again) + " leaves placed again over finer land");
    // (Within a distance, placed out to it: past where they are placed the
    // land's canopy stands for them, drawReach.)
    const double reach = drawReach();
    if (std::isinf(within) || reach < std::min(within, static_cast<double>(real_)))
        out.push_back("plants: " + std::to_string(n) + " placements pending, placed out to " +
                      std::to_string(static_cast<int>(reach)) + " m");
}

void PlantsNode::setPlace(const Place* place) {
    place_ = place;
    const double radius = place && !place->body().flat() ? place->body().radius : 0.0;
    if (radius == radius_) return;
    radius_ = radius;
    setRules(rulesRef_);
}

void PlantsNode::follow(const bromath::Vec3& eye, const LandNode* land) {
    store_.tick();
    if (!placement_) return;
    double at[3] = {eye.x, eye.y, eye.z};
    if (radius_ > 0.0) place_->toBody(eye, at);
    // The eye's height over the land under it (its sea's surface where that
    // is higher).
    double lift = 0.0;
    if (land) {
        const double floor = land->sea().on ? land->sea().level : -std::numeric_limits<double>::infinity();
        if (radius_ > 0.0) {
            const double n = std::sqrt(at[0] * at[0] + at[1] * at[1] + at[2] * at[2]);
            const double dir[3] = {at[0] / n, at[1] / n, at[2] / n};
            lift = n - radius_ - std::max<double>(land->heightOver(dir), floor);
        } else {
            lift = at[1] - std::max<double>(land->heightAt(static_cast<float>(at[0]), static_cast<float>(at[2])), floor);
        }
        // (No height yet, while the land's processes have not run: as on
        // the ground until it has one.)
        if (!std::isfinite(lift)) lift = 0.0;
    }
    std::vector<PlantPlacement::Placed> placed;
    std::vector<CellKey> dropped;
    placement_->update(at, lift, placed, dropped, !hold_);
    const std::vector<float> none;
    for (const CellKey& key : dropped) {
        const auto it = leaves_.find(key);
        if (it != leaves_.end()) putLeaf(it, &none);
    }
    for (PlantPlacement::Placed& p : placed) {
        // (On a round body a leaf's plants come relative to its origin on
        // the body: where they stand in the place frame now.)
        if (radius_ > 0.0) {
            double origin[3];
            placementOrigin(place_->body(), grid_, p.key, origin);
            for (size_t r = 0; r + kPlantFloats <= p.records.size(); r += kPlantFloats) {
                float* rec = p.records.data() + r;
                const double b[3] = {origin[0] + rec[0], origin[1] + rec[1], origin[2] + rec[2]};
                double q[3];
                place_->toPlace(b, q);
                for (int i = 0; i < 3; ++i) rec[i] = static_cast<float>(q[i]);
            }
        }
        putLeaf(leaves_.try_emplace(p.key).first, &p.records);
    }
}

void PlantsNode::placeMoved(const PlaceMove& move) {
    if (radius_ <= 0.0) return;
    const auto moved = [&](float* rec) {
        const double p[3] = {rec[0], rec[1], rec[2]};
        double q[3];
        move.apply(p, q);
        for (int i = 0; i < 3; ++i) rec[i] = static_cast<float>(q[i]);
    };
    for (auto it = leaves_.begin(); it != leaves_.end();) {
        auto next = std::next(it);
        Leaf& leaf = it->second;
        for (size_t r = 0; r + kPlantFloats <= leaf.fixed.size(); r += kPlantFloats) moved(leaf.fixed.data() + r);
        std::vector<float> placed;
        if (leaf.placed > 0) {
            const CellStore::Run& run = store_.runs().at(it->first.packed());
            const float* from = store_.record(run.first + run.count - leaf.placed);
            placed.assign(from, from + size_t{leaf.placed} * kPlantFloats);
            for (size_t r = 0; r < placed.size(); r += kPlantFloats) moved(placed.data() + r);
        }
        putLeaf(it, &placed);
        it = next;
    }
    // (Every leaf moved: the views take it all as changed.)
    changedAll();
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
        measure(leaf, run.first, run.count);
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

void PlantsNode::measure(Leaf& leaf, uint32_t first, uint32_t count) {
    // Each plant's sphere: the crown's reach and the leaves past its skin,
    // and the sway.
    bromath::AABB3 b = bromath::aempty3();
    leaf.kinds.clear();
    for (uint32_t k = first; k < first + count; ++k) {
        const float* p = store_.record(k);
        int s = static_cast<int>(p[5]);
        if (s < 0 || s >= speciesCount_) s = 0;
        auto kind = std::find_if(leaf.kinds.begin(), leaf.kinds.end(), [s](const Kind& o) { return o.species == s; });
        if (kind == leaf.kinds.end()) kind = leaf.kinds.insert(leaf.kinds.end(), Kind{s, 0, 0.0f});
        ++kind->count;
        kind->scale += p[4];
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
    for (Kind& kind : leaf.kinds) kind.scale /= static_cast<float>(kind.count);
    leaf.bounds = b;
    leaf.cast.clear();
}

void PlantsNode::setParams(const Params& p) {
    params_ = p;
    for (const auto& [key, leaf] : leaves_) leaf.cast.clear();
}

void PlantsNode::castElements(const CellKey& key, float finestTexel, int from, int to, float* out) const {
    std::fill(out + from, out + to, 0.0f);
    const auto it = leaves_.find(key);
    if (it == leaves_.end() || speciesCount_ <= 0 || from >= to) return;
    const Leaf& leaf = it->second;
    const size_t levels = static_cast<size_t>(to - from);
    if (leaf.cast.size() == levels && leaf.castFinest == finestTexel && leaf.castFrom == from) {
        std::copy(leaf.cast.begin(), leaf.cast.end(), out + from);
        return;
    }
    // plants_plan.comp's caster pick: a crown under 1.5 elements of
    // shadowTexels across is a disc; else elementCount at 1/texel.
    const float kept = params_.shadowTexels;
    for (const Kind& kind : leaf.kinds) {
        const float* s = species(kind.species);
        const float radius = (std::sqrt(0.25f * s[0] * s[0] + s[2] * s[2]) + 0.15f * s[2]) * kind.scale + 0.3f;
        const float full = std::max(s[3], 1.0f);
        for (int level = from; level < to; ++level) {
            const float texel = std::ldexp(finestTexel, level);
            const float crown = radius / (texel * kept);
            float each = 1.0f;
            if (crown >= 1.5f) {
                const float length = s[4] * kind.scale / texel;
                const float least = std::clamp(3.14159f * crown * crown, 2.0f, params_.minElements);
                each = std::clamp(full * std::min(1.0f, length * length / (kept * kept)), std::min(least, full), full);
            }
            out[level] += each * static_cast<float>(kind.count);
        }
    }
    leaf.cast.assign(out + from, out + to);
    leaf.castFinest = finestTexel;
    leaf.castFrom = from;
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

float PlantsNode::nearest(const bromath::Vec3& eye) const {
    float d2 = std::numeric_limits<float>::max();
    for (const auto& [key, leaf] : leaves_) {
        const bromath::Vec3 c = bromath::vmax(leaf.bounds.min, bromath::vmin(eye, leaf.bounds.max));
        const float dx = c.x - eye.x, dy = c.y - eye.y, dz = c.z - eye.z;
        d2 = std::min(d2, dx * dx + dy * dy + dz * dz);
    }
    return std::sqrt(d2);
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
