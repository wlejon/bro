#include "scene/world/plant_placement.h"

#include "scene/world/world_workers.h"
#include "util/json_in.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
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

// The squared distance on the ground from (x, z) to leaf (lx, lz)'s square,
// through the ground's metric g (xx, xz, zz: square metres of ground a
// square metre of the grid's; the identity on the plane).
double leafDistance2(const CellGrid& grid, int32_t lx, int32_t lz, double x, double z,
                     const double* g = nullptr) {
    const double s = grid.leaf;
    const double x0 = lx * s, z0 = lz * s;
    const double dx = std::max({x0 - x, 0.0, x - (x0 + s)});
    const double dz = std::max({z0 - z, 0.0, z - (z0 + s)});
    if (!g) return dx * dx + dz * dz;
    // (The square's nearest corner in grid metres, the cross term's sign
    // taken from it: a little off the true nearest point under a sheared
    // metric, by under its shear over the range.)
    const double sx = x < x0 ? 1.0 : -1.0, sz = z < z0 ? 1.0 : -1.0;
    return g[0] * dx * dx + 2.0 * g[1] * (sx * dx) * (sz * dz) + g[2] * dz * dz;
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
    const auto termAt = [&](const util::JsonValue& o, const char* key) {
        PlacementTerm t;
        const util::JsonValue& v = o.get(key);
        if (v.isNumber()) {
            t.field = fieldAt(o, key);
        } else if (v.isObject()) {
            const util::JsonValue& own = v.get("own");
            if (!own.isNumber() || (own.number != 0.0 && own.number != 1.0)) bad = true;
            else t.own = static_cast<int>(own.number);
            t.scale = number(v, "scale", 1.0f);
            t.offset = number(v, "offset", 0.0f);
            t.field = fieldAt(v, "field");
        }
        return t;
    };
    PlacementRules rules;
    rules.ground = fieldAt(v, "ground");
    rules.range = number(v, "range", rules.range);
    rules.hysteresis = number(v, "hysteresis", rules.hysteresis);
    for (const util::JsonValue& l : v.get("layers").array) {
        PlacementLayer layer;
        layer.spacing = number(l, "spacing", layer.spacing);
        layer.jitter = number(l, "jitter", layer.jitter);
        layer.chance = termAt(l, "chance");
        for (const util::JsonValue& s : l.get("species").array)
            layer.species.push_back({static_cast<int>(s.get("species").number), termAt(s, "weight")});
        layer.scale = termAt(l, "scale");
        layer.scaleVariety = number(l, "scaleVariety", layer.scaleVariety);
        layer.tint = termAt(l, "tint");
        layer.tintVariety = number(l, "tintVariety", layer.tintVariety);
        layer.sink = number(l, "sink", layer.sink);
        const util::JsonValue& seed = l.get("seed");
        layer.seed = seed.isNumber() ? static_cast<uint32_t>(static_cast<int64_t>(seed.number))
                                     : static_cast<uint32_t>(rules.layers.size() + 1);
        rules.layers.push_back(std::move(layer));
    }
    if (bad) return fail("placement rules: a field index past the table, or an own value not 0 or 1");
    if (!(rules.range > 0.0f) || rules.hysteresis < 0.0f) return fail("placement rules: range must be positive");
    out = std::move(rules);
    return true;
}

void placementOrigin(const Body& body, const CellGrid& grid, const CellKey& key, double out[3]) {
    const double side = static_cast<double>(grid.leaf) * static_cast<double>(int64_t{1} << key.level);
    double d[3];
    dirOf(body, {key.root, (key.x + 0.5) * side, (key.z + 0.5) * side}, d);
    for (int i = 0; i < 3; ++i) out[i] = body.radius * d[i];
}

std::vector<float> placeCell(const PlacementRules& rules, const CellGrid& grid, const CellKey& key, const Body* body) {
    std::vector<float> out;
    const bool round = body && !body->flat();
    const double side = static_cast<double>(grid.leaf) * static_cast<double>(int64_t{1} << key.level);
    const double x0 = key.x * side, z0 = key.z * side;
    double origin[3] = {0.0, 0.0, 0.0};
    const double half = round ? faceHalf(*body) : 0.0;
    if (round) placementOrigin(*body, grid, key, origin);
    // A candidate's place: on the plane its x and z (ys unused); on a round
    // body its datum point relative to the origin and its up (us, 3 a one).
    std::vector<float> xs, ys, zs, us, thin, values, ground, total, pick;
    std::vector<int64_t> is, js;
    std::vector<uint32_t> hashes;
    const auto sampleOr = [&](const FieldRef& f, float fallback, std::vector<float>& o) {
        o.resize(xs.size());
        if (!f) std::fill(o.begin(), o.end(), fallback);
        else if (round) f->sample(xs.data(), ys.data(), zs.data(), static_cast<int>(xs.size()), origin, o.data());
        else f->sample(xs.data(), zs.data(), static_cast<int>(xs.size()), o.data());
    };
    // A term: its field (none: 1) times the land's own value there (none: 1).
    const auto sampleTerm = [&](const PlacementTerm& t, float fallback, std::vector<float>& o) {
        if (t.none()) {
            sampleOr(nullptr, fallback, o);
            return;
        }
        sampleOr(t.field, 1.0f, o);
        if (t.own < 0) return;
        for (size_t c = 0; c < xs.size(); ++c) {
            float v = 0.0f;
            if (rules.base) {
                if (round) {
                    const double d[3] = {us[c * 3], us[c * 3 + 1], us[c * 3 + 2]};
                    v = rules.base->ownDir(t.own, d);
                } else {
                    v = rules.base->ownAt(t.own, 0, xs[c], zs[c]);
                }
            }
            o[c] *= t.offset + t.scale * v;
        }
    };
    for (const PlacementLayer& layer : rules.layers) {
        if (!(layer.spacing > 0.0f) || layer.species.empty()) continue;
        const double s = layer.spacing;
        // (Each face's draws its own.)
        const uint32_t seed = round ? layer.seed ^ (0x632be5abu * (key.root + 1u)) : layer.seed;
        // The grid points in the cell: [x0, x0 + side).
        const int64_t i0 = static_cast<int64_t>(std::ceil(x0 / s)), i1 = static_cast<int64_t>(std::ceil((x0 + side) / s));
        const int64_t j0 = static_cast<int64_t>(std::ceil(z0 / s)), j1 = static_cast<int64_t>(std::ceil((z0 + side) / s));
        xs.clear();
        ys.clear();
        zs.clear();
        us.clear();
        thin.clear();
        is.clear();
        js.clear();
        hashes.clear();
        for (int64_t j = j0; j < j1; ++j)
            for (int64_t i = i0; i < i1; ++i) {
                uint32_t h[kDraws];
                for (uint32_t k = 0; k < kDraws; ++k) h[k] = draw(seed, i, j, k);
                const double x = i * s + layer.jitter * s * (unit(h[0]) - 0.5f);
                const double z = j * s + layer.jitter * s * (unit(h[1]) - 0.5f);
                if (round) {
                    // (A point past the face's edge is its neighbour's.)
                    if (!(x >= -half && x < half && z >= -half && z < half)) continue;
                    double d[3];
                    dirOf(*body, {key.root, x, z}, d);
                    // A chart square metre holds J of the ground's (1 at a
                    // face's middle, 1/sqrt 2 at its edge): a candidate
                    // grows by its chance times J, so the plants are as
                    // dense on the ground everywhere.
                    const double a = std::tan(x / body->radius), b = std::tan(z / body->radius);
                    thin.push_back(static_cast<float>((1.0 + a * a) * (1.0 + b * b) /
                                                      std::pow(1.0 + a * a + b * b, 1.5)));
                    xs.push_back(static_cast<float>(body->radius * d[0] - origin[0]));
                    ys.push_back(static_cast<float>(body->radius * d[1] - origin[1]));
                    zs.push_back(static_cast<float>(body->radius * d[2] - origin[2]));
                    us.insert(us.end(), {static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2])});
                } else {
                    xs.push_back(static_cast<float>(x));
                    zs.push_back(static_cast<float>(z));
                }
                is.push_back(i);
                js.push_back(j);
                hashes.insert(hashes.end(), h, h + kDraws);
            }
        // Those that grow.
        sampleTerm(layer.chance, 1.0f, values);
        size_t kept = 0;
        for (size_t c = 0; c < xs.size(); ++c) {
            if (!(unit(hashes[c * kDraws + 2]) < (round ? values[c] * thin[c] : values[c]))) continue;
            xs[kept] = xs[c];
            zs[kept] = zs[c];
            if (round) {
                ys[kept] = ys[c];
                std::copy_n(us.begin() + static_cast<std::ptrdiff_t>(c * 3), 3,
                            us.begin() + static_cast<std::ptrdiff_t>(kept * 3));
            }
            is[kept] = is[c];
            js[kept] = js[c];
            std::copy_n(hashes.begin() + static_cast<std::ptrdiff_t>(c * kDraws), kDraws,
                        hashes.begin() + static_cast<std::ptrdiff_t>(kept * kDraws));
            ++kept;
        }
        xs.resize(kept);
        zs.resize(kept);
        if (round) ys.resize(kept);
        if (kept == 0) continue;
        // Their species: a draw over the weights' running total.
        std::vector<int> species(kept, -1);
        total.assign(kept, 0.0f);
        pick.resize(kept);
        for (size_t c = 0; c < kept; ++c) pick[c] = unit(hashes[c * kDraws + 3]);
        std::vector<std::vector<float>> weights(layer.species.size());
        for (size_t k = 0; k < layer.species.size(); ++k) {
            sampleTerm(layer.species[k].weight, 1.0f, weights[k]);
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
        sampleTerm(layer.scale, 1.0f, scale);
        sampleTerm(layer.tint, 0.0f, tint);
        sampleOr(rules.ground, 0.0f, ground);
        if (rules.base)
            for (size_t c = 0; c < kept; ++c) {
                if (!round) {
                    ground[c] = rules.base->over(ground[c], xs[c], zs[c]);
                    continue;
                }
                const double d[3] = {us[c * 3], us[c * 3 + 1], us[c * 3 + 2]};
                ground[c] = rules.base->overDir(ground[c], d);
            }
        for (size_t c = 0; c < kept; ++c) {
            const uint32_t* h = hashes.data() + c * kDraws;
            const float size = scale[c] * (1.0f + layer.scaleVariety * (2.0f * unit(h[4]) - 1.0f));
            if (species[c] < 0 || !(size > 0.0f)) continue;
            // Its foot: the ground over the datum point, along its up on a
            // round body.
            const float g = ground[c] - layer.sink;
            const float* up = round ? us.data() + c * 3 : nullptr;
            const float record[kPlantFloats] = {round ? xs[c] + up[0] * g : xs[c],
                                                round ? ys[c] + up[1] * g : g,
                                                round ? zs[c] + up[2] * g : zs[c],
                                                unit(h[6]) * 6.2831853f,
                                                size,
                                                static_cast<float>(species[c]),
                                                static_cast<float>(draw(seed, is[c], js[c], kDraws) >> 8),
                                                tint[c] + layer.tintVariety * (unit(h[5]) - 0.5f)};
            out.insert(out.end(), record, record + kPlantFloats);
        }
    }
    return out;
}

PlantPlacement::PlantPlacement(std::shared_ptr<const PlacementRules> rules, const CellGrid& grid,
                               WorldWorkers& workers, const Body* body)
    : rules_(std::move(rules)), grid_(grid), workers_(workers) {
    if (body && !body->flat()) {
        body_ = *body;
        round_ = true;
        half_ = faceHalf(body_);
    }
}

PlantPlacement::~PlantPlacement() = default;

size_t PlantPlacement::pending() const {
    // (Before the first update, the look about the eye is what is pending.)
    if (!looked_) return 1;
    return queue_.size() + posted_.size();
}

void PlantPlacement::eyes(const double eye[3], Eyes& out, CellKey& leaf) const {
    out = Eyes{};
    if (!round_) {
        out[0] = {true, eye[0], eye[2]};
        leaf = {static_cast<int32_t>(std::floor(eye[0] / grid_.leaf)),
                static_cast<int32_t>(std::floor(eye[2] / grid_.leaf)), 0, 0};
        return;
    }
    const double n = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    const double dir[3] = {eye[0] / n, eye[1] / n, eye[2] / n};
    const ChartPoint own = chartOf(body_, dir);
    leaf = {static_cast<int32_t>(std::floor(own.x / grid_.leaf)), static_cast<int32_t>(std::floor(own.z / grid_.leaf)),
            0, static_cast<uint8_t>(own.root)};
    // (The faces it is near: within 75 degrees of their middles, any whose
    // edge a range of plants could reach.)
    for (int f = 0; f < 6; ++f) {
        const double* fn = cubeFace(f).n;
        if (dir[0] * fn[0] + dir[1] * fn[1] + dir[2] * fn[2] < 0.25) continue;
        const ChartPoint c = chartOf(body_, dir, f);
        // The ground's metric on the chart there (a chart metre across a
        // face's edge is 1/sqrt 2 of the ground's): constant enough over a
        // range of plants, a few thousandths of the body round.
        const double a = std::tan(c.x / body_.radius), b = std::tan(c.z / body_.radius);
        const double a2 = 1.0 + a * a, b2 = 1.0 + b * b, rho4 = (a2 + b * b) * (a2 + b * b);
        EyeOn& on = out[static_cast<size_t>(f)];
        on = {true, c.x, c.z, {a2 * a2 * b2 / rho4, -a * b * a2 * b2 / rho4, b2 * b2 * a2 / rho4}};
    }
}

double PlantPlacement::distance2(const Eyes& e, const CellKey& k) const {
    const EyeOn& on = e[k.root % 6];
    if (!on.on) return std::numeric_limits<double>::infinity();
    return leafDistance2(grid_, k.x, k.z, on.x, on.z, round_ ? on.g : nullptr);
}

bool PlantPlacement::owns(int32_t x, int32_t z) const {
    if (!round_) return true;
    const double s = grid_.leaf;
    return x * s < half_ && (x + 1) * s > -half_ && z * s < half_ && (z + 1) * s > -half_;
}

void PlantPlacement::want(const Eyes& e) {
    const double range = rules_->range;
    std::unordered_set<uint64_t, KeyHash> queued;
    // (Each with its distance, worked out once: sorting on distance2 itself
    // was most of a jump's first look, 58 ms of a frame.)
    std::vector<std::pair<double, CellKey>> next;
    for (const CellKey& k : queue_) {
        const double d2 = distance2(e, k);
        if (d2 <= range * range && queued.insert(k.packed()).second) next.push_back({d2, k});
    }
    // (A round body's chart has up to 1.5 of its metres to the ground's.)
    const int32_t reach = static_cast<int32_t>(std::ceil((round_ ? 1.5 : 1.0) * range / grid_.leaf)) + 1;
    for (int r = 0; r < 6; ++r) {
        const EyeOn& on = e[static_cast<size_t>(r)];
        if (!on.on) continue;
        const double* g = round_ ? on.g : nullptr;
        // The queue: those still in range, and every leaf in range not
        // placed, posted or queued; the nearest last. A leaf this far
        // inside the range was in it at the last look, so is one of those
        // already: only the band outside it is looked up (all of it on the
        // first look on the root or a jump).
        const EyeOn& was = wanted_[static_cast<size_t>(r)];
        const double mx = on.x - was.x, mz = on.z - was.z;
        const double moved = !was.on ? 1e30
                             : g    ? std::sqrt(std::max(g[0] * mx * mx + 2.0 * g[1] * mx * mz + g[2] * mz * mz, 0.0))
                                    : std::hypot(mx, mz);
        const double inner = range - moved - 1.5 * grid_.leaf;
        const double inner2 = inner > 0.0 ? inner * inner : -1.0;
        const auto cx = static_cast<int32_t>(std::floor(on.x / grid_.leaf));
        const auto cz = static_cast<int32_t>(std::floor(on.z / grid_.leaf));
        for (int32_t z = cz - reach; z <= cz + reach; ++z)
            for (int32_t x = cx - reach; x <= cx + reach; ++x) {
                const double d2 = leafDistance2(grid_, x, z, on.x, on.z, g);
                if (d2 > range * range || d2 <= inner2 || !owns(x, z)) continue;
                const CellKey k{x, z, 0, static_cast<uint8_t>(r)};
                const uint64_t p = k.packed();
                if (resident_.count(p) || posted_.count(p) || queued.count(p)) continue;
                next.push_back({d2, k});
            }
    }
    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    queue_.clear();
    queue_.reserve(next.size());
    for (const auto& [d2, k] : next) queue_.push_back(k);
    wanted_ = e;
}

void PlantPlacement::update(const double eye[3], std::vector<Placed>& placed, std::vector<CellKey>& dropped) {
    placed.clear();
    dropped.clear();
    const double keep = static_cast<double>(rules_->range) + rules_->hysteresis;
    Eyes e;
    CellKey leaf;
    eyes(eye, e, leaf);
    if (!looked_ || leaf != eyeLeaf_) {
        looked_ = true;
        eyeLeaf_ = leaf;
        for (auto it = resident_.begin(); it != resident_.end();) {
            const CellKey k = CellKey::unpacked(*it);
            if (distance2(e, k) > keep * keep) {
                dropped.push_back(k);
                it = resident_.erase(it);
            } else {
                ++it;
            }
        }
        want(e);
    }
    // What the workers finished, if still wanted.
    std::vector<Placed> done;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        done.swap(inbox_->done);
    }
    for (Placed& p : done) {
        posted_.erase(p.key.packed());
        if (distance2(e, p.key) > keep * keep) continue;
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
    const auto now = std::chrono::steady_clock::now();
    const double since = lastUpdate_.time_since_epoch().count() != 0
                             ? std::chrono::duration<double, std::milli>(now - lastUpdate_).count()
                             : 0.0;
    lastUpdate_ = now;
    const double span = queueMs > 0.0 ? std::clamp(1.25 * since, queueMs, std::max(queueMs, kMaxQueueMs)) : 0.0;
    const double threads = workers_.threads();
    const size_t limit = static_cast<size_t>(
        std::clamp(cost > 0.0 ? threads * span / cost : (span > 0.0 ? threads : 1.0), 1.0, 4096.0));
    while (!queue_.empty() && posted_.size() < limit) {
        const CellKey k = queue_.back();
        queue_.pop_back();
        posted_.insert(k.packed());
        workers_.post([rules = rules_, grid = grid_, k, inbox = inbox_, body = body_, round = round_] {
            const auto t0 = std::chrono::steady_clock::now();
            Placed p{k, placeCell(*rules, grid, k, round ? &body : nullptr)};
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->done.push_back(std::move(p));
            inbox->costMs = inbox->costMs > 0.0 ? inbox->costMs + 0.1 * (ms - inbox->costMs) : ms;
        });
    }
}

}  // namespace bro::scene
