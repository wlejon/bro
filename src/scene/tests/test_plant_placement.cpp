// Placement (scene/world/plant_placement.h): fields from encoded FastNoise2
// trees, a cell's plants from the rules (as many as the grid and the chance
// say, the species by their weights, each the same however the ground is cut
// into cells), the rules' JSON, the leaves kept about a moving eye on the
// world's workers (nearest first, each once, dropped past the hysteresis);
// the cell store's runs (a run given back not reused while frames may read
// it, the changes the GPU copy takes); and the plants node over both.

#include "scene/nodes/plants_node.h"
#include "scene/world/cell_store.h"
#include "scene/world/fields.h"
#include "scene/world/plant_placement.h"
#include "scene/world/world_workers.h"

#include <FastNoise/FastNoise.h>
#include <FastNoise/Metadata.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace bro::scene;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    if (++g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

// A FastNoise2 node as data, its variables and sources set by name, encoded
// as the engine takes it.
struct Node {
    std::vector<std::unique_ptr<FastNoise::NodeData>> all;
    FastNoise::NodeData* root = nullptr;
};
const FastNoise::Metadata* metadata(const char* type) {
    for (const FastNoise::Metadata* m : FastNoise::Metadata::GetAll())
        if (m && std::strcmp(m->name, type) == 0) return m;
    return nullptr;
}
FastNoise::NodeData* make(Node& n, const char* type) {
    n.all.push_back(std::make_unique<FastNoise::NodeData>(metadata(type)));
    return n.all.back().get();
}
// (A per-dimension member: `dim` its dimension, -1 every one.)
void setVar(FastNoise::NodeData* d, const char* name, float v, int dim = -1) {
    for (size_t i = 0; i < d->metadata->memberVariables.size(); ++i) {
        const auto& m = d->metadata->memberVariables[i];
        if (std::strcmp(m.name, name) == 0 && (dim < 0 || m.dimensionIdx == dim)) d->variables[i] = v;
    }
}
void setHybrid(FastNoise::NodeData* d, const char* name, float v) {
    for (size_t i = 0; i < d->metadata->memberHybrids.size(); ++i)
        if (std::strcmp(d->metadata->memberHybrids[i].name, name) == 0) d->hybrids[i] = {nullptr, v};
}
std::string constant(float v) {
    Node n;
    n.root = make(n, "Constant");
    setVar(n.root, "Value", v);
    return FastNoise::Metadata::SerialiseNodeData(n.root);
}
std::string simplex(float featureScale) {
    Node n;
    n.root = make(n, "Simplex");
    setVar(n.root, "Feature Scale", featureScale);
    return FastNoise::Metadata::SerialiseNodeData(n.root);
}
// 1 or more west of x = edge - 0.5, 0 or less east of edge + 0.5 (x itself,
// remapped).
std::string westOf(float edge) {
    Node n;
    FastNoise::NodeData* x = make(n, "Gradient");
    setVar(x, "Multiplier", 1.0f, 0);
    n.root = make(n, "Remap");
    n.root->nodeLookups[0] = x;
    setHybrid(n.root, "From Min", edge - 0.5f);
    setHybrid(n.root, "From Max", edge + 0.5f);
    setHybrid(n.root, "To Min", 1.0f);
    setHybrid(n.root, "To Max", 0.0f);
    return FastNoise::Metadata::SerialiseNodeData(n.root);
}
FieldRef field(const std::string& encoded, int seed = 7) { return Field::make(encoded, seed); }

PlacementRules oneLayer(float spacing, FieldRef chance, float sink = 0.0f) {
    PlacementRules r;
    r.ground = field(constant(3.0f));
    PlacementLayer l;
    l.spacing = spacing;
    l.jitter = 0.9f;
    l.chance.field = std::move(chance);
    l.species.push_back({2, {}});
    l.sink = sink;
    r.layers.push_back(l);
    return r;
}

void testFields() {
    check(field("not a tree") == nullptr, "a string that is not a tree makes no field");
    const FieldRef c = field(constant(0.25f));
    check(c && std::abs(c->at(12.0f, -40.0f) - 0.25f) < 1e-6f, "a constant field is its value");
    const FieldRef s = field(simplex(0.05f));
    const float xs[5] = {0.0f, 10.0f, 20.5f, -33.0f, 7.0f}, zs[5] = {1.0f, -3.0f, 8.0f, 2.0f, 900.0f};
    float out[5];
    s->sample(xs, zs, 5, out);
    float worst = 0.0f;
    bool varies = false;
    // (A single value splits its point about a double origin, the batch's
    // floats do not: FastNoise2's simplex differs by ~1e-4 between them.)
    for (int i = 0; i < 5; ++i) {
        worst = std::max(worst, std::abs(out[i] - s->at(xs[i], zs[i])));
        varies = varies || std::abs(out[i] - out[0]) > 1e-3f;
    }
    check(worst < 5e-4f && varies, "a field sampled at points is its value at each (worst " + std::to_string(worst) + ")");
    const FieldRef other = field(simplex(0.05f), 8);
    check(std::abs(other->at(10.0f, -3.0f) - s->at(10.0f, -3.0f)) > 1e-4f, "another seed, another field");
    const FieldRef w = field(westOf(100.0f));
    check(w && w->at(50.0f, 0.0f) > 0.99f && w->at(150.0f, 0.0f) < 0.01f, "a remapped distance edges a region");
}

void testCell() {
    const CellGrid grid;   // 64 m leaves
    const PlacementRules every = oneLayer(4.0f, nullptr, 0.1f);
    const CellKey key{3, -2, 0, 0};
    const std::vector<float> a = placeCell(every, grid, key);
    check(a.size() == 16 * 16 * PlantsNode::kPlantFloats, "every candidate grows: 16 x 16 in a 64 m leaf at 4 m");
    check(a == placeCell(every, grid, key), "a cell is the same each time it is placed");
    bool near = true, foot = true, kinds = true;
    for (size_t i = 0; i < a.size(); i += PlantsNode::kPlantFloats) {
        const float x = a[i], z = a[i + 2];
        const float gx = std::round(x / 4.0f) * 4.0f, gz = std::round(z / 4.0f) * 4.0f;
        near = near && std::abs(x - gx) <= 1.8f + 1e-3f && std::abs(z - gz) <= 1.8f + 1e-3f && gx >= 192.0f &&
               gx < 256.0f && gz >= -128.0f && gz < -64.0f;
        foot = foot && std::abs(a[i + 1] - 2.9f) < 1e-5f;
        kinds = kinds && a[i + 5] == 2.0f && a[i + 4] == 1.0f;
    }
    check(near, "a plant stands within the jitter of its grid point, the point in its cell");
    check(foot && kinds, "on the ground less its sink, its species and size the rules'");

    // The same plants however the ground is cut: a level-1 cell is its four leaves'.
    const PlacementRules some = oneLayer(5.5f, field(simplex(0.03f)));
    std::multiset<std::tuple<float, float, float>> four, one;
    for (int dz = 0; dz < 2; ++dz)
        for (int dx = 0; dx < 2; ++dx) {
            const std::vector<float> p = placeCell(some, grid, CellKey{4 + dx, 6 + dz, 0, 0});
            for (size_t i = 0; i < p.size(); i += 8) four.insert({p[i], p[i + 2], p[i + 6]});
        }
    const std::vector<float> p = placeCell(some, grid, CellKey{2, 3, 1, 0});
    for (size_t i = 0; i < p.size(); i += 8) one.insert({p[i], p[i + 2], p[i + 6]});
    check(!four.empty() && four == one, "a parent cell's plants are its leaves'");

    // The chance: a constant quarter grows about a quarter.
    const PlacementRules quarter = oneLayer(2.0f, field(constant(0.25f)));
    size_t grown = 0;
    for (int x = 0; x < 4; ++x) grown += placeCell(quarter, grid, CellKey{x, 0, 0, 0}).size() / 8;
    check(std::abs(static_cast<double>(grown) / (4 * 32 * 32) - 0.25) < 0.03, "a constant chance grows its share");
    // A region's edge: none past it.
    const PlacementRules west = oneLayer(2.0f, field(westOf(100.0f)));
    const std::vector<float> edge = placeCell(west, grid, CellKey{1, 0, 0, 0});   // x 64..128
    bool inside = !edge.empty();
    for (size_t i = 0; i < edge.size(); i += 8) inside = inside && edge[i] < 101.0f;
    check(inside, "no plant where the chance is 0");

    // The species by their weights, 1 : 3.
    PlacementRules mix = oneLayer(2.0f, nullptr);
    mix.layers[0].species = {{0, {field(constant(1.0f))}}, {5, {field(constant(3.0f))}}};
    mix.layers[0].scaleVariety = 0.2f;
    mix.layers[0].scale.field = field(constant(2.0f));
    std::map<int, int> count;
    bool sized = true;
    for (int x = 0; x < 4; ++x) {
        const std::vector<float> q = placeCell(mix, grid, CellKey{x, 1, 0, 0});
        for (size_t i = 0; i < q.size(); i += 8) {
            ++count[static_cast<int>(q[i + 5])];
            sized = sized && q[i + 4] >= 1.6f - 1e-4f && q[i + 4] <= 2.4f + 1e-4f;
        }
    }
    const double share = static_cast<double>(count[5]) / (count[0] + count[5]);
    check(count.size() == 2 && std::abs(share - 0.75) < 0.03, "species drawn by their weights");
    check(sized, "a size within its variety of the field's");
}

void testJson() {
    const std::string tree = constant(0.5f);
    PlacementRules r;
    std::string err;
    const std::string ok = "{\"fields\":[{\"tree\":\"" + tree + "\",\"seed\":3}],\"ground\":0,\"range\":500,"
                           "\"layers\":[{\"spacing\":6,\"chance\":0,\"species\":[{\"species\":4,\"weight\":0},"
                           "{\"species\":1}],\"scaleVariety\":0.3,\"sink\":0.2,\"seed\":11}]}";
    check(parsePlacementRules(ok, r, &err), "rules parse: " + err);
    check(r.range == 500.0f && r.ground && r.ground->seed() == 3 && r.layers.size() == 1 &&
              r.layers[0].spacing == 6.0f && r.layers[0].chance.field == r.ground && r.layers[0].species.size() == 2 &&
              r.layers[0].species[1].weight.none() && r.layers[0].seed == 11u && r.layers[0].sink == 0.2f,
          "the rules' fields shared by index, their numbers read");
    check(!parsePlacementRules("{\"fields\":[],\"ground\":2}", r, &err), "a field index past the table fails");
    check(!parsePlacementRules("{\"fields\":[{\"tree\":\"zz\"}]}", r, &err), "a tree that does not decode fails");
    check(!parsePlacementRules("[1,2", r, &err), "bad JSON fails");
}

double leafDistance(const CellGrid& g, const CellKey& k, double x, double z) {
    const double x0 = k.x * g.leaf, z0 = k.z * g.leaf;
    const double dx = std::max({x0 - x, 0.0, x - x0 - g.leaf}), dz = std::max({z0 - z, 0.0, z - z0 - g.leaf});
    return std::hypot(dx, dz);
}

void testStream() {
    const CellGrid grid;
    auto rules = std::make_shared<PlacementRules>(oneLayer(8.0f, nullptr));
    rules->range = 300.0f;
    rules->hysteresis = 100.0f;
    WorldWorkers workers(3);
    PlantPlacement placement(rules, grid, workers);
    placement.queueMs = 0.0;
    std::set<uint64_t> resident;
    int placedTwice = 0;
    bool nearestFirst = true, pastRange = false;
    double last = -1.0;
    std::vector<PlantPlacement::Placed> placed;
    std::vector<CellKey> dropped;
    const auto run = [&](double x, double z) {
        last = -1.0;
        for (int i = 0; i < 10000; ++i) {
            const double eye[3] = {x, 0.0, z};
            placement.update(eye, placed, dropped);
            for (const CellKey& k : dropped) resident.erase(k.packed());
            for (const auto& p : placed) {
                placedTwice += resident.insert(p.key.packed()).second ? 0 : 1;
                const double d = leafDistance(grid, p.key, x, z);
                nearestFirst = nearestFirst && d + 1e-9 >= last;
                pastRange = pastRange || d > rules->range;
                last = d;
                check(p.records.size() == 64 * 8, "a leaf's plants come with it");
            }
            if (placement.pending() == 0) break;
            workers.drain();
        }
    };
    run(10.0, 20.0);
    size_t inRange = 0;
    for (int z = -8; z <= 8; ++z)
        for (int x = -8; x <= 8; ++x) inRange += leafDistance(grid, CellKey{x, z, 0, 0}, 10.0, 20.0) <= 300.0 ? 1 : 0;
    check(resident.size() == inRange && placement.resident() == inRange, "every leaf in range placed");
    check(nearestFirst && !pastRange && placedTwice == 0, "nearest first, none past range, each once");

    // A step: the new edge placed, nothing dropped inside the hysteresis.
    run(90.0, 20.0);
    bool kept = true;
    for (uint64_t k : resident) kept = kept && leafDistance(grid, CellKey::unpacked(k), 90.0, 20.0) <= 400.0;
    size_t now = 0;
    for (int z = -8; z <= 8; ++z)
        for (int x = -8; x <= 10; ++x) now += leafDistance(grid, CellKey{x, z, 0, 0}, 90.0, 20.0) <= 300.0 ? 1 : 0;
    size_t covered = 0;
    for (uint64_t k : resident) covered += leafDistance(grid, CellKey::unpacked(k), 90.0, 20.0) <= 300.0 ? 1 : 0;
    check(kept && covered == now && placedTwice == 0, "a step places its edge, keeps the rest");

    // A jump: the old leaves dropped, the new ones placed.
    run(5000.0, -3000.0);
    bool moved = !resident.empty();
    for (uint64_t k : resident) moved = moved && leafDistance(grid, CellKey::unpacked(k), 5000.0, -3000.0) <= 300.0;
    check(moved && resident.size() == placement.resident() && placedTwice == 0, "a jump drops the old, places the new");
}

void testStore() {
    CellStore store(4, 2);
    const float a[6] = {1, 2, 3, 4, 5, 6}, b[6] = {7, 8, 9, 10, 11, 12};
    store.put(CellKey{0, 0, 0, 0}, a, 3);
    const uint64_t layout = store.layout(), serial = store.serial();
    const CellStore::Run ra = store.runs().at(CellKey{0, 0, 0, 0}.packed());
    store.put(CellKey{1, 0, 0, 0}, b, 3);
    std::vector<std::pair<size_t, size_t>> changes;
    check(store.changesSince(serial, changes) && changes.size() == 1 && changes[0].second == 6 &&
              store.layout() == layout,
          "a put is one range changed, in place");
    store.drop(CellKey{0, 0, 0, 0});
    store.put(CellKey{2, 0, 0, 0}, b, 3);
    const CellStore::Run rc = store.runs().at(CellKey{2, 0, 0, 0}.packed());
    check(rc.first >= ra.first + ra.count || rc.first + rc.count <= ra.first, "a run given back is not reused at once");
    for (int t = 0; t < CellStore::kReuseTicks; ++t) store.tick();
    store.put(CellKey{3, 0, 0, 0}, a, 3);
    check(store.runs().at(CellKey{3, 0, 0, 0}.packed()).first == ra.first, "after a few frames it is");
    check(store.live() == 9 && *store.record(ra.first) == 1.0f, "the live records and their values");
    std::vector<float> big(2 * 10000, 1.0f);
    store.put(CellKey{4, 0, 0, 0}, big.data(), 10000);
    check(store.layout() != layout && !store.changesSince(serial, changes), "a pool grown is copied whole");
    for (int t = 0; t < CellStore::kLogTicks + 1; ++t) store.tick();
    const uint64_t s2 = store.serial();
    store.put(CellKey{5, 0, 0, 0}, a, 3);
    check(store.changesSince(s2, changes) && changes.size() == 1, "the log reaches back to a recent serial");
}

void testNode() {
    PlantsNode node;
    float species[PlantsNode::kSpeciesFloats * 3] = {};
    for (int s = 0; s < 3; ++s) {
        species[s * PlantsNode::kSpeciesFloats] = 10.0f;      // height
        species[s * PlantsNode::kSpeciesFloats + 2] = 3.0f;   // crown radius
    }
    node.setSpecies(species, 3);
    // Two plants by hand in one leaf, and rules over it.
    const float hand[16] = {10, 0, 10, 0, 1, 0, 1, 0, 12, 0, 11, 0, 2, 1, 2, 0};
    node.setPlants(hand, 2);
    check(node.plantCount() == 2 && node.cells().size() == 1, "plants by hand in their cell");
    auto rules = std::make_shared<PlacementRules>(oneLayer(16.0f, nullptr));
    rules->range = 100.0f;
    const uint64_t before = node.generation();
    node.setRules(rules);
    for (int i = 0; i < 1000 && (i == 0 || node.placementPending() > 0); ++i) {
        node.follow({0.0f, 0.0f, 0.0f});
        WorldWorkers::shared().drain();
    }
    node.follow({0.0f, 0.0f, 0.0f});
    size_t leaves = 0;
    for (int z = -3; z <= 3; ++z)
        for (int x = -3; x <= 3; ++x) leaves += leafDistance(node.cellGrid(), CellKey{x, z, 0, 0}, 0.0, 0.0) <= 100.0;
    check(node.cells().size() == leaves && node.plantCount() == static_cast<int>(leaves * 16 + 2),
          "the rules' leaves placed about the eye, the hand's kept with them");
    std::vector<bromath::AABB3> boxes;
    check(node.changesSince(before, boxes) && boxes.size() >= leaves, "each placed leaf a change");
    bool runs = true, bounded = true;
    for (const CellContent& c : node.cells()) {
        runs = runs && c.count > 0 && c.first + c.count <= node.store().capacity();
        for (uint32_t i = c.first; i < c.first + c.count; ++i) {
            const float* p = node.store().record(i);
            bounded = bounded && p[0] >= c.bounds.min.x && p[0] <= c.bounds.max.x && p[2] >= c.bounds.min.z &&
                      p[2] <= c.bounds.max.z && p[1] + 10.0f * p[4] <= c.bounds.max.y;
        }
    }
    check(runs && bounded, "each cell's run in the pool, its plants inside its bounds");
    check(std::abs(node.tallest() - 20.0f) < 1e-4f, "the tallest: species height by scale");

    // Far off (on a leaf corner again): the placed leaves go, the hand's stay.
    for (int i = 0; i < 1000 && (i == 0 || node.placementPending() > 0); ++i) {
        node.follow({10240.0f, 0.0f, 0.0f});
        WorldWorkers::shared().drain();
    }
    node.follow({10240.0f, 0.0f, 0.0f});
    bool handKept = false;
    for (const CellContent& c : node.cells()) handKept = handKept || (c.key.x == 0 && c.key.z == 0 && c.count == 2);
    check(handKept && node.cells().size() == leaves + 1, "the hand's plants stay as the eye leaves");
    node.setRules(nullptr);
    check(node.plantCount() == 2 && node.cells().size() == 1, "rules cleared: the placed plants go");
}

// Every plant of `node` where it stands on the body (through `place`).
std::vector<std::array<double, 3>> standing(const PlantsNode& node, const Place& place) {
    std::vector<std::array<double, 3>> out;
    for (const CellContent& c : node.cells())
        for (uint32_t i = c.first; i < c.first + c.count; ++i) {
            const float* p = node.store().record(i);
            std::array<double, 3> b;
            place.toBody(bromath::Vec3{p[0], p[1], p[2]}, b.data());
            out.push_back(b);
        }
    std::sort(out.begin(), out.end());
    return out;
}

// On an Earth-radius body, the plants placed on its chart: on the datum
// (no ground), across a cube face's edge with neither a gap nor a double
// row, the same wherever the place frame is anchored, and kept where they
// stand on the body when it re-anchors.
void testRound() {
    const double R = 6371000.0;
    Body body;
    body.radius = R;
    const double s = std::sqrt(0.5);
    // On the edge between +X and +Y, a little way along it.
    double at[3] = {R * s, R * s, 0.0};
    const auto settle = [](PlantsNode& node, const bromath::Vec3& eye) {
        for (int i = 0; i < 1000 && (i == 0 || node.placementPending() > 0); ++i) {
            node.follow(eye);
            WorldWorkers::shared().drain();
        }
        node.follow(eye);
    };
    const auto rules = std::make_shared<PlacementRules>(oneLayer(16.0f, nullptr));
    float species[PlantsNode::kSpeciesFloats] = {10.0f, 0.0f, 3.0f};
    const auto made = [&](Place& place, const double eyeOnBody[3]) {
        auto node = std::make_unique<PlantsNode>();
        node->setSpecies(species, 1);
        node->setPlace(&place);
        auto r = std::make_shared<PlacementRules>(*rules);
        r->range = 400.0f;
        node->setRules(r);
        settle(*node, place.toPlace(eyeOnBody));
        return node;
    };

    Place place;
    place.setBody(body, at);
    auto node = made(place, at);
    const auto a = standing(*node, place);
    bool onDatum = !a.empty();
    std::set<int> roots;
    for (const CellContent& c : node->cells()) roots.insert(c.key.root);
    // (The rules' ground is 3 m everywhere.)
    for (const auto& b : a)
        onDatum = onDatum && std::abs(std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]) - (R + 3.0)) < 0.01;
    check(onDatum, "on a round body every plant stands on its ground");
    check(roots.size() == 2, "the plants about an edge are both faces'");
    // As many a square metre on the ground near the edge, and either side
    // of it, as the spacing says: the plants within 150 m of the eye's
    // ground and of two points 120 m off the edge, one a 16 m square (~276
    // in each; the chart's metres are 1.4 times as many there).
    const auto within = [&](const double c[3], double r) {
        size_t n = 0;
        for (const auto& b : a) n += std::hypot(b[0] - c[0], b[1] - c[1], b[2] - c[2]) < r ? 1 : 0;
        return n;
    };
    const double off = 120.0 / R;
    const double pX[3] = {R * std::cos(0.7853981634 - off), R * std::sin(0.7853981634 - off), 0.0};
    const double pY[3] = {R * std::cos(0.7853981634 + off), R * std::sin(0.7853981634 + off), 0.0};
    const size_t nEdge = within(at, 150.0), nX = within(pX, 150.0), nY = within(pY, 150.0);
    const double expect = 3.14159265 * 150.0 * 150.0 / 256.0;
    check(std::abs(double(nEdge) - expect) < 0.12 * expect && std::abs(double(nX) - expect) < 0.12 * expect &&
              std::abs(double(nY) - expect) < 0.12 * expect,
          "as many plants on the edge as either side of it (" + std::to_string(nEdge) + ", " + std::to_string(nX) +
              ", " + std::to_string(nY) + ")");

    // Placed as far as the range on the ground in every direction, across
    // the edge (where the chart's metre is 0.71 of the ground's) as along
    // it, and not much past it: the farthest plant in each of 16 sectors
    // about the eye.
    {
        // (The ground's tangents at the eye: along the edge, and across it.)
        const double e1[3] = {0.0, 0.0, 1.0}, e2[3] = {s, -s, 0.0};
        double far[16] = {};
        double farthest = 0.0;
        for (const auto& b : a) {
            const double d[3] = {b[0] - at[0], b[1] - at[1], b[2] - at[2]};
            const double x = d[0] * e1[0] + d[1] * e1[1] + d[2] * e1[2], y = d[0] * e2[0] + d[1] * e2[1] + d[2] * e2[2];
            const double r = std::hypot(x, y);
            const int sector = static_cast<int>(std::floor((std::atan2(y, x) + 3.14159265) / 6.2831853 * 16.0)) & 15;
            far[sector] = std::max(far[sector], r);
            farthest = std::max(farthest, r);
        }
        double nearest = 1e30;
        for (double f : far) nearest = std::min(nearest, f);
        check(nearest > 0.93 * 400.0 && farthest < 400.0 + 64.0 * 1.5 * 1.42,
              "placed to the range on the ground every way (" + std::to_string(nearest) + " .. " +
                  std::to_string(farthest) + " m)");
    }

    // The same plants with the place frame anchored 30 km away.
    Place far;
    const double fa = 30000.0 / R;
    const double farPoint[3] = {R * s * std::cos(fa), R * s * std::cos(fa), R * std::sin(fa)};
    far.setBody(body, farPoint);
    auto other = made(far, at);
    const auto b = standing(*other, far);
    bool same = a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); ++i)
        same = std::hypot(a[i][0] - b[i][0], a[i][1] - b[i][1], a[i][2] - b[i][2]) < 0.02;
    check(same, "the same plants wherever the place frame is anchored");

    // A re-anchor: each plant where it stood on the body.
    PlaceMove move;
    check(place.anchorAt(farPoint, move), "the place re-anchors");
    node->placeMoved(move);
    const auto c = standing(*node, place);
    bool kept = a.size() == c.size();
    for (size_t i = 0; kept && i < a.size(); ++i)
        kept = std::hypot(a[i][0] - c[i][0], a[i][1] - c[i][1], a[i][2] - c[i][2]) < 0.02;
    check(kept, "a re-anchor keeps every plant where it stood on the body");
}

}  // namespace

// A land's processes' height under the rules' ground (rules.base): each
// plant's foot is the ground plus the base where it stands, on the plane and
// on a round body.
void testBase() {
    const auto linear = [](LandGrid g, auto f) {
        LandBase b{std::move(g), {}};
        b.values.assign(b.grid.texels(), 0.0f);
        const int s = b.grid.side();
        for (int r = 0; r < b.grid.roots(); ++r)
            for (int j = 0; j < s; ++j)
                for (int i = 0; i < s; ++i)
                    b.values[(static_cast<size_t>(r) * s + j) * s + i] = static_cast<float>(
                        f(r, -b.grid.half() + (i - 0.5) * b.grid.cell(), -b.grid.half() + (j - 0.5) * b.grid.cell()));
        return std::make_shared<const LandBase>(std::move(b));
    };
    const CellGrid grid;
    PlacementRules flat = oneLayer(4.0f, nullptr, 0.1f);
    LandGrid g;
    g.configure(Body{}, 1024.0, 64);
    flat.base = linear(g, [](int, double x, double z) { return 0.5 * x - 0.25 * z + 20.0; });
    const std::vector<float> a = placeCell(flat, grid, CellKey{3, -2, 0, 0});
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); i += PlantsNode::kPlantFloats) {
        const float want = 3.0f - 0.1f + 0.5f * a[i] - 0.25f * a[i + 2] + 20.0f;
        worst = std::max(worst, std::abs(a[i + 1] - want));
    }
    check(!a.empty() && worst < 1e-2f, "flat: a plant stands on the ground plus the base (worst " + std::to_string(worst) + ")");

    // The base's scale on the ground (the tree's): half of it, the ground's
    // 3 m stands 1.5 m over the base.
    {
        LandBase half = *flat.base;
        half.scales.assign(half.grid.texels(), 0.5f);
        flat.base = std::make_shared<const LandBase>(std::move(half));
    }
    const std::vector<float> s = placeCell(flat, grid, CellKey{3, -2, 0, 0});
    worst = 0.0f;
    for (size_t i = 0; i < s.size(); i += PlantsNode::kPlantFloats) {
        const float want = 1.5f - 0.1f + 0.5f * s[i] - 0.25f * s[i + 2] + 20.0f;
        worst = std::max(worst, std::abs(s[i + 1] - want));
    }
    check(!s.empty() && worst < 1e-2f, "flat: the base scales the ground under it (worst " + std::to_string(worst) + ")");

    const double R = 6371000.0;
    Body body;
    body.radius = R;
    PlacementRules round = oneLayer(16.0f, nullptr);
    LandGrid rg;
    rg.configure(body, 0.0, 32);
    round.base = linear(rg, [&](int r, double x, double z) {
        double d[3];
        dirOf(body, {r, x, z}, d);
        return 200.0 * d[0];
    });
    const CellKey key{40, -12, 0, 0};
    const std::vector<float> b = placeCell(round, grid, key, &body);
    double origin[3];
    placementOrigin(body, grid, key, origin);
    double worstR = 0.0;
    for (size_t i = 0; i < b.size(); i += PlantsNode::kPlantFloats) {
        double p[3] = {origin[0] + b[i], origin[1] + b[i + 1], origin[2] + b[i + 2]};
        const double r = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        for (double& v : p) v /= r;
        worstR = std::max(worstR, std::abs(r - R - 3.0 - round.base->overDir(0.0f, p)));
    }
    check(!b.empty() && worstR < 0.05, "round: a plant stands on the ground plus the base (worst " + std::to_string(worstR) + ")");
}

// The land's own values (LandBase::own, channel 0's z and w) as the rules'
// terms: on the plane the woods only where z says (1 west of x = 0, 0 east),
// their species by w (1 north of z = 0, 0 south) and by 1 - w; a term
// times a field; with no base the own value is 0, so nothing grows; and the
// JSON's own terms, a bad one refused.
void testOwn() {
    const CellGrid grid;
    LandGrid g;
    g.configure(Body{}, 1024.0, 64);   // 32 m texels
    LandBase b{g, {}, {}, {}};
    const int s = b.grid.side();
    b.values.assign(b.grid.texels(), 0.0f);
    b.own[0].assign(b.grid.texels(), 0.0f);
    b.own[1].assign(b.grid.texels(), 0.0f);
    for (int j = 0; j < s; ++j)
        for (int i = 0; i < s; ++i) {
            const double x = -b.grid.half() + (i - 0.5) * b.grid.cell(), z = -b.grid.half() + (j - 0.5) * b.grid.cell();
            b.own[0][static_cast<size_t>(j) * s + i] = x < 0.0 ? 1.0f : 0.0f;
            b.own[1][static_cast<size_t>(j) * s + i] = z < 0.0 ? 1.0f : 0.0f;
        }
    PlacementRules r = oneLayer(4.0f, nullptr);
    r.base = std::make_shared<const LandBase>(std::move(b));
    r.layers[0].chance.own = 0;
    r.layers[0].species = {{0, {}}, {5, {}}};
    r.layers[0].species[0].weight.own = 1;
    r.layers[0].species[1].weight.own = 1;
    r.layers[0].species[1].weight.scale = -1.0f;
    r.layers[0].species[1].weight.offset = 1.0f;
    int west = 0, east = 0, wrong = 0, total = 0;
    for (int cz = -3; cz < 3; ++cz)
        for (int cx = -3; cx < 3; ++cx) {
            const std::vector<float> p = placeCell(r, grid, CellKey{cx, cz, 0, 0});
            for (size_t i = 0; i < p.size(); i += PlantsNode::kPlantFloats) {
                const float x = p[i], z = p[i + 2];
                const int sp = static_cast<int>(p[i + 5]);
                ++total;
                if (x < -40.0f) ++west;
                if (x > 40.0f) ++east;
                if ((z < -40.0f && sp != 0) || (z > 40.0f && sp != 5)) ++wrong;
            }
        }
    check(west > 300 && east == 0, "own z: the woods grow where it is 1, not where 0 (" + std::to_string(west) +
                                       " west, " + std::to_string(east) + " east)");
    check(wrong == 0, "own w: the species by w and by 1 - w (" + std::to_string(wrong) + " of " +
                          std::to_string(total) + " the wrong one)");

    // Times a field: a constant 0.5 halves the woods.
    PlacementRules half = r;
    half.layers[0].chance.field = field(constant(0.5f));
    int halfWest = 0;
    for (int cz = -3; cz < 3; ++cz)
        for (int cx = -3; cx < 0; ++cx) {
            const std::vector<float> p = placeCell(half, grid, CellKey{cx, cz, 0, 0});
            for (size_t i = 0; i < p.size(); i += PlantsNode::kPlantFloats) halfWest += p[i] < -40.0f ? 1 : 0;
        }
    check(std::abs(halfWest - 0.5 * west) < 0.12 * west,
          "own z times a field of 0.5: half the woods (" + std::to_string(halfWest) + " of " + std::to_string(west) + ")");

    PlacementRules bare = r;
    bare.base = nullptr;
    size_t none = 0;
    for (int cx = -3; cx < 3; ++cx) none += placeCell(bare, grid, CellKey{cx, 0, 0, 0}).size();
    check(none == 0, "with no land the own value is 0: nothing grows");

    PlacementRules parsed;
    std::string err;
    const std::string ok = "{\"fields\":[{\"tree\":\"" + constant(0.5f) + "\",\"seed\":3}],\"layers\":[{\"spacing\":4,"
                           "\"chance\":{\"own\":0,\"scale\":2,\"offset\":-0.5,\"field\":0},"
                           "\"species\":[{\"species\":1,\"weight\":{\"own\":1}}]}]}";
    check(parsePlacementRules(ok, parsed, &err), "own terms parse: " + err);
    const PlacementTerm& c = parsed.layers.empty() ? PlacementTerm{} : parsed.layers[0].chance;
    check(c.own == 0 && c.scale == 2.0f && c.offset == -0.5f && c.field && !parsed.layers.empty() &&
              parsed.layers[0].species[0].weight.own == 1 && !parsed.layers[0].species[0].weight.field,
          "an own term's value, scale, offset and field read");
    check(!parsePlacementRules("{\"fields\":[],\"layers\":[{\"chance\":{\"own\":2},\"species\":[0]}]}", parsed, &err),
          "an own value not z or w is refused");
}

int main() {
    testFields();
    testCell();
    testJson();
    testStream();
    testStore();
    testNode();
    testRound();
    testBase();
    testOwn();
    std::printf("plant placement: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
