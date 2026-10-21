#include "scene/world/plant_placement.h"

#include "scene/world/world_workers.h"
#include "util/json_in.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace bro::scene {

namespace {

constexpr int kPlantFloats = 8;   // PlantsNode::kPlantFloats
constexpr int kDraws = 7;         // a candidate's: jitter x, z, chance, species, scale, tint, yaw

// A candidate's draws: a hash of the layer's seed, its grid point and the
// draw (lowbias32, chained).
uint32_t mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
uint32_t draw(uint32_t seed, int64_t i, int64_t j, uint32_t k) {
    uint32_t h = mix(seed ^ 0x9e3779b9u);
    h = mix(h ^ static_cast<uint32_t>(i));
    h = mix(h ^ static_cast<uint32_t>(static_cast<uint64_t>(i) >> 32) ^ 0x85ebca6bu);
    h = mix(h ^ static_cast<uint32_t>(j));
    h = mix(h ^ static_cast<uint32_t>(static_cast<uint64_t>(j) >> 32) ^ 0xc2b2ae35u);
    return mix(h ^ k);
}
float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

void sampleOr(const FieldRef& f, const std::vector<float>& xs, const std::vector<float>& zs, float fallback,
              std::vector<float>& out) {
    out.resize(xs.size());
    if (f) f->sample(xs.data(), zs.data(), static_cast<int>(xs.size()), out.data());
    else std::fill(out.begin(), out.end(), fallback);
}

// The squared distance on the ground from (x, z) to leaf (lx, lz)'s square.
double leafDistance2(const CellGrid& grid, int32_t lx, int32_t lz, double x, double z) {
    const double s = grid.leaf;
    const double x0 = lx * s, z0 = lz * s;
    const double dx = std::max({x0 - x, 0.0, x - (x0 + s)});
    const double dz = std::max({z0 - z, 0.0, z - (z0 + s)});
    return dx * dx + dz * dz;
}

}  // namespace

bool parsePlacementRules(const std::string& json, PlacementRules& out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    util::JsonValue v;
    std::string why;
    if (!util::parseJson(json, v, &why)) return fail(why);
    if (!v.isObject()) return fail("placement rules: not an object");
    std::vector<FieldRef> fields;
    for (const util::JsonValue& f : v.get("fields").array) {
        FieldRef field = Field::make(f.get("tree").string, static_cast<int>(f.get("seed").number));
        if (!field) return fail("placement rules: field " + std::to_string(fields.size()) + " does not decode");
        fields.push_back(std::move(field));
    }
    bool bad = false;
    const auto fieldAt = [&](const util::JsonValue& o, const char* key) -> FieldRef {
        const util::JsonValue& i = o.get(key);
        if (!i.isNumber()) return nullptr;
        const auto at = static_cast<size_t>(i.number);
        if (i.number < 0.0 || at >= fields.size()) {
            bad = true;
            return nullptr;
        }
        return fields[at];
    };
    const auto number = [](const util::JsonValue& o, const char* key, float fallback) {
        const util::JsonValue& n = o.get(key);
        return n.isNumber() ? static_cast<float>(n.number) : fallback;
    };
    PlacementRules rules;
    rules.ground = fieldAt(v, "ground");
    rules.range = number(v, "range", rules.range);
    rules.hysteresis = number(v, "hysteresis", rules.hysteresis);
    for (const util::JsonValue& l : v.get("layers").array) {
        PlacementLayer layer;
        layer.spacing = number(l, "spacing", layer.spacing);
        layer.jitter = number(l, "jitter", layer.jitter);
        layer.chance = fieldAt(l, "chance");
        for (const util::JsonValue& s : l.get("species").array)
            layer.species.push_back({static_cast<int>(s.get("species").number), fieldAt(s, "weight")});
        layer.scale = fieldAt(l, "scale");
        layer.scaleVariety = number(l, "scaleVariety", layer.scaleVariety);
        layer.tint = fieldAt(l, "tint");
        layer.tintVariety = number(l, "tintVariety", layer.tintVariety);
        layer.sink = number(l, "sink", layer.sink);
        const util::JsonValue& seed = l.get("seed");
        layer.seed = seed.isNumber() ? static_cast<uint32_t>(static_cast<int64_t>(seed.number))
                                     : static_cast<uint32_t>(rules.layers.size() + 1);
        rules.layers.push_back(std::move(layer));
    }
    if (bad) return fail("placement rules: a field index past the table");
    if (!(rules.range > 0.0f) || rules.hysteresis < 0.0f) return fail("placement rules: range must be positive");
    out = std::move(rules);
    return true;
}

std::vector<float> placeCell(const PlacementRules& rules, const CellGrid& grid, const CellKey& key) {
    std::vector<float> out;
    const double side = static_cast<double>(grid.leaf) * static_cast<double>(int64_t{1} << key.level);
    const double x0 = key.x * side, z0 = key.z * side;
    std::vector<float> xs, zs, values, ground, total, pick;
    std::vector<int64_t> is, js;
    std::vector<uint32_t> hashes;
    for (const PlacementLayer& layer : rules.layers) {
        if (!(layer.spacing > 0.0f) || layer.species.empty()) continue;
        const double s = layer.spacing;
        // The grid points in the cell: [x0, x0 + side).
        const int64_t i0 = static_cast<int64_t>(std::ceil(x0 / s)), i1 = static_cast<int64_t>(std::ceil((x0 + side) / s));
        const int64_t j0 = static_cast<int64_t>(std::ceil(z0 / s)), j1 = static_cast<int64_t>(std::ceil((z0 + side) / s));
        xs.clear();
        zs.clear();
        is.clear();
        js.clear();
        hashes.clear();
        for (int64_t j = j0; j < j1; ++j)
            for (int64_t i = i0; i < i1; ++i) {
                uint32_t h[kDraws];
                for (uint32_t k = 0; k < kDraws; ++k) h[k] = draw(layer.seed, i, j, k);
                xs.push_back(static_cast<float>(i * s + layer.jitter * s * (unit(h[0]) - 0.5f)));
                zs.push_back(static_cast<float>(j * s + layer.jitter * s * (unit(h[1]) - 0.5f)));
                is.push_back(i);
                js.push_back(j);
                hashes.insert(hashes.end(), h, h + kDraws);
            }
        // Those that grow.
        sampleOr(layer.chance, xs, zs, 1.0f, values);
        size_t kept = 0;
        for (size_t c = 0; c < xs.size(); ++c) {
            if (!(unit(hashes[c * kDraws + 2]) < values[c])) continue;
            xs[kept] = xs[c];
            zs[kept] = zs[c];
            is[kept] = is[c];
            js[kept] = js[c];
            std::copy_n(hashes.begin() + static_cast<std::ptrdiff_t>(c * kDraws), kDraws,
                        hashes.begin() + static_cast<std::ptrdiff_t>(kept * kDraws));
            ++kept;
        }
        xs.resize(kept);
        zs.resize(kept);
        if (kept == 0) continue;
        // Their species: a draw over the weights' running total.
        std::vector<int> species(kept, -1);
        total.assign(kept, 0.0f);
        pick.resize(kept);
        for (size_t c = 0; c < kept; ++c) pick[c] = unit(hashes[c * kDraws + 3]);
        std::vector<std::vector<float>> weights(layer.species.size());
        for (size_t k = 0; k < layer.species.size(); ++k) {
            sampleOr(layer.species[k].weight, xs, zs, 1.0f, weights[k]);
            for (size_t c = 0; c < kept; ++c) total[c] += std::max(weights[k][c], 0.0f);
        }
        for (size_t c = 0; c < kept; ++c) {
            float r = pick[c] * total[c];
            for (size_t k = 0; k < layer.species.size() && total[c] > 0.0f; ++k) {
                const float w = std::max(weights[k][c], 0.0f);
                if (w <= 0.0f) continue;
                species[c] = layer.species[k].species;
                if (r < w) break;
                r -= w;
            }
        }
        std::vector<float> scale, tint;
        sampleOr(layer.scale, xs, zs, 1.0f, scale);
        sampleOr(layer.tint, xs, zs, 0.0f, tint);
        sampleOr(rules.ground, xs, zs, 0.0f, ground);
        for (size_t c = 0; c < kept; ++c) {
            const uint32_t* h = hashes.data() + c * kDraws;
            const float size = scale[c] * (1.0f + layer.scaleVariety * (2.0f * unit(h[4]) - 1.0f));
            if (species[c] < 0 || !(size > 0.0f)) continue;
            const float record[kPlantFloats] = {xs[c],
                                                ground[c] - layer.sink,
                                                zs[c],
                                                unit(h[6]) * 6.2831853f,
                                                size,
                                                static_cast<float>(species[c]),
                                                static_cast<float>(draw(layer.seed, is[c], js[c], kDraws) >> 8),
                                                tint[c] + layer.tintVariety * (unit(h[5]) - 0.5f)};
            out.insert(out.end(), record, record + kPlantFloats);
        }
    }
    return out;
}

PlantPlacement::PlantPlacement(std::shared_ptr<const PlacementRules> rules, const CellGrid& grid,
                               WorldWorkers& workers)
    : rules_(std::move(rules)), grid_(grid), workers_(workers) {}

PlantPlacement::~PlantPlacement() = default;

size_t PlantPlacement::pending() const {
    // (Before the first update, the look about the eye is what is pending.)
    if (eyeLeafX_ == INT32_MIN) return 1;
    return queue_.size() + posted_.size();
}

void PlantPlacement::want(double eyeX, double eyeZ) {
    const double range = rules_->range;
    // The queue: those still in range, and every leaf in range not placed,
    // posted or queued; the nearest last. A leaf this far inside the range
    // was in it at the last look, so is one of those already: only the
    // band outside it is looked up (all of it on the first look or a jump).
    const double moved = std::hypot(eyeX - wantX_, eyeZ - wantZ_);
    const double inner = range - moved - 1.5 * grid_.leaf;
    const double inner2 = inner > 0.0 ? inner * inner : -1.0;
    wantX_ = eyeX;
    wantZ_ = eyeZ;
    std::unordered_set<uint64_t, KeyHash> queued;
    std::vector<CellKey> next;
    for (const CellKey& k : queue_)
        if (leafDistance2(grid_, k.x, k.z, eyeX, eyeZ) <= range * range && queued.insert(k.packed()).second)
            next.push_back(k);
    const int32_t reach = static_cast<int32_t>(std::ceil(range / grid_.leaf)) + 1;
    for (int32_t z = eyeLeafZ_ - reach; z <= eyeLeafZ_ + reach; ++z)
        for (int32_t x = eyeLeafX_ - reach; x <= eyeLeafX_ + reach; ++x) {
            const double d2 = leafDistance2(grid_, x, z, eyeX, eyeZ);
            if (d2 > range * range || d2 <= inner2) continue;
            const CellKey k{x, z, 0, 0};
            const uint64_t p = k.packed();
            if (resident_.count(p) || posted_.count(p) || queued.count(p)) continue;
            next.push_back(k);
        }
    std::sort(next.begin(), next.end(), [&](const CellKey& a, const CellKey& b) {
        return leafDistance2(grid_, a.x, a.z, eyeX, eyeZ) > leafDistance2(grid_, b.x, b.z, eyeX, eyeZ);
    });
    queue_ = std::move(next);
}

void PlantPlacement::update(double eyeX, double eyeZ, std::vector<Placed>& placed, std::vector<CellKey>& dropped) {
    placed.clear();
    dropped.clear();
    const double keep = static_cast<double>(rules_->range) + rules_->hysteresis;
    const CellKey eye = grid_.leafAt(static_cast<float>(eyeX), static_cast<float>(eyeZ));
    if (eye.x != eyeLeafX_ || eye.z != eyeLeafZ_) {
        eyeLeafX_ = eye.x;
        eyeLeafZ_ = eye.z;
        for (auto it = resident_.begin(); it != resident_.end();) {
            const CellKey k = CellKey::unpacked(*it);
            if (leafDistance2(grid_, k.x, k.z, eyeX, eyeZ) > keep * keep) {
                dropped.push_back(k);
                it = resident_.erase(it);
            } else {
                ++it;
            }
        }
        want(eyeX, eyeZ);
    }
    // What the workers finished, if still wanted.
    std::vector<Placed> done;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        done.swap(inbox_->done);
    }
    for (Placed& p : done) {
        posted_.erase(p.key.packed());
        if (leafDistance2(grid_, p.key.x, p.key.z, eyeX, eyeZ) > keep * keep) continue;
        resident_.insert(p.key.packed());
        placed.push_back(std::move(p));
    }
    // The next jobs, the nearest first: before a job's cost is known, one a
    // thread.
    double cost;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        cost = inbox_->costMs;
    }
    const double threads = workers_.threads();
    const size_t limit = static_cast<size_t>(
        std::clamp(cost > 0.0 ? threads * queueMs / cost : (queueMs > 0.0 ? threads : 1.0), 1.0, 4096.0));
    while (!queue_.empty() && posted_.size() < limit) {
        const CellKey k = queue_.back();
        queue_.pop_back();
        posted_.insert(k.packed());
        workers_.post([rules = rules_, grid = grid_, k, inbox = inbox_] {
            const auto t0 = std::chrono::steady_clock::now();
            Placed p{k, placeCell(*rules, grid, k)};
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->done.push_back(std::move(p));
            inbox->costMs = inbox->costMs > 0.0 ? inbox->costMs + 0.1 * (ms - inbox->costMs) : ms;
        });
    }
}

}  // namespace bro::scene
