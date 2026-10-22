// SurfaceLayers (scene/world/surface_layers.h), the CPU half of the surface layers
// every provider shares: which tiles a view needs, the jobs it offers the
// world service and what they are worth, and what the page table says, held
// against a page table rebuilt from the writes alone as the GPU's is. A
// view's tiles must be a quadtree's leaves and all their ancestors, finer
// toward the eye; every filled tile must be found through its entry (and
// decode to its slot); the coarsest level is filled whatever the budget and
// a tile never ahead of its parent; no slot holds two tiles; a stale
// generation refills while the old tiles keep their entries.

#include "scene/world/surface_layers.h"
#include "scene/world/world_service.h"

#include <bromath/frustum.h>
#include <bromath/mat.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bro::scene;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    if (++g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

/// A view from `eye` looking along `dir` (60 degrees vertically, 2560 x 1364).
SurfaceLayers::View makeView(bromath::Vec3 eye, bromath::Vec3 dir) {
    const bromath::Vec3 up = std::fabs(dir.y) > 0.99f ? bromath::Vec3{0, 0, 1} : bromath::Vec3{0, 1, 0};
    const bromath::Mat4 view = bromath::mlookAt(eye, eye + dir, up);
    const float aspect = 2560.0f / 1364.0f;
    const bromath::Mat4 proj = bromath::mperspective(1.0471976f, aspect, 0.1f, 50000.0f);
    SurfaceLayers::View v;
    v.eye = eye;
    v.frustum = bromath::ffromViewProj(bromath::mmul(proj, view), false);
    v.focalPixels = 0.5f * 1364.0f / std::tan(0.5f * 1.0471976f);
    return v;
}

/// The page table as the GPU holds it: every write applied in order.
struct Pages {
    std::map<std::tuple<int, int, int>, uint32_t> entries;
    void apply(const std::vector<SurfacePageWrite>& writes) {
        for (const SurfacePageWrite& w : writes) entries[{w.layer, w.ex, w.ez}] = w.value;
    }
    /// The slot holding tile `k` per the table, -1 when none.
    int lookup(const CellKey& k, int pageSize, int shift) const {
        const auto it = entries.find({k.level, k.x & (pageSize - 1), k.z & (pageSize - 1)});
        if (it == entries.end() || it->second == 0) return -1;
        const uint32_t e = it->second;
        if (((e >> 12) & 1023u) != (static_cast<uint32_t>(k.x >> shift) & 1023u) ||
            (e >> 22) != (static_cast<uint32_t>(k.z >> shift) & 1023u))
            return -1;
        return static_cast<int>(e & 4095u) - 1;
    }
};

bromath::AABB3 world() { return {{-5000.0f, -10.0f, -5000.0f}, {5000.0f, 10.0f, 5000.0f}}; }

/// A frame with every job granted: the selection alone.
void fillAll(SurfaceLayers& L, const SurfaceLayers::View& v, uint64_t frame) {
    L.update(v, frame);
    std::vector<uint64_t> keys;
    for (const SurfaceJob& j : L.jobs()) keys.push_back(j.key.packed());
    L.commit(keys);
}

/// A frame whose jobs go through the world service, as the turf's do.
void fillServed(SurfaceLayers& L, WorldService& service, int kind, const SurfaceLayers::View& v, uint64_t frame) {
    L.update(v, frame);
    service.beginFrame();
    for (const SurfaceJob& j : L.jobs()) service.offer({j.key.packed(), 0, kind, j.error, j.side, j.required});
    service.schedule();
    std::vector<uint64_t> keys;
    for (const WorldJob& j : service.granted(kind)) keys.push_back(j.key);
    L.commit(keys);
}

void testSelection() {
    SurfaceLayerConfig c;
    c.slots = 2048;
    SurfaceLayers L;
    L.configure(c, world());
    check(L.levels() == 15, "levels: 1 m tiles to a 10 km extent take 15 (got " + std::to_string(L.levels()) + ")");

    Pages pages;
    const SurfaceLayers::View v = makeView({0.0f, 2.0f, 0.0f}, {0.0f, -0.2f, 1.0f});
    fillAll(L, v, 1);
    pages.apply(L.pageWrites());
    const SurfaceLayerStats s = L.stats();
    check(s.needed > 0 && s.missing == 0, "a walk's view: every needed tile filled in one frame with no budget");
    check(s.filled == s.needed, "every needed tile filled");
    check(s.needed <= static_cast<uint32_t>(c.slots), "the selection fits the atlas");

    std::set<uint64_t> keys;
    std::set<int> slots;
    int level0 = 0;
    for (const SurfaceFill& f : L.fills()) {
        keys.insert(f.key.packed());
        check(slots.insert(f.slot).second, "no two fills share a slot");
        check(pages.lookup(f.key, c.pageSize, L.pageShift()) == f.slot, "a filled tile is found at its slot");
        if (f.key.level == 0) ++level0;
        check(std::fabs(f.side - c.tileMetres * std::pow(2.0, f.key.level)) < 1e-6, "a fill's side is its level's");
    }
    check(level0 > 0, "standing on the ground, the tiles reach level 0 near the feet");
    // Every tile's parent is there too, up to the coarsest.
    for (const SurfaceFill& f : L.fills()) {
        if (f.key.level + 1 >= L.levels()) continue;
        check(keys.count(f.key.parent().packed()) == 1, "a tile's parent is needed with it");
    }
    // The tile under the eye is level 0; the ones a kilometre off are coarse.
    const CellKey under{0, 0, 0, 0};
    check(keys.count(under.packed()) == 1, "the tile under the eye is level 0");
    bool farFine = false;
    for (const SurfaceFill& f : L.fills())
        if (f.key.level <= 2 && std::hypot(f.x0 + 0.5 * f.side, f.z0 + 0.5 * f.side) > 1000.0) farFine = true;
    check(!farFine, "no fine tiles a kilometre off");

    // A second frame of the same view has no jobs, fills nothing and writes nothing.
    fillAll(L, v, 2);
    check(L.jobs().empty() && L.fills().empty() && L.pageWrites().empty(), "a still view fills nothing more");
}

void testBudgetAndCoarsest() {
    SurfaceLayerConfig c;
    c.slots = 2048;
    SurfaceLayers L;
    L.configure(c, world());
    // Eight fills a frame: a floor of 8 ms at a guessed 1 ms a fill, and no
    // frame ever read.
    WorldService service;
    WorldServiceConfig sc;
    sc.floorMs = 8.0f;
    service.configure(sc);
    const int kind = service.addKind({"turf", WorldSchedule::Raster, 1.0f});
    Pages pages;
    const SurfaceLayers::View v = makeView({0.0f, 150.0f, 0.0f}, {0.0f, -0.6f, 1.0f});
    L.update(v, 1);
    const int top = L.levels() - 1;
    // The jobs' errors: required for the coarsest level, every other one
    // missing with nothing resident above it; no tile's above its parent's.
    std::map<uint64_t, float> errors;
    for (const SurfaceJob& j : L.jobs()) {
        check(j.required == (j.key.level == top), "the coarsest level's missing tiles are required, no others");
        check(j.error > 0.0f, "a missing tile's error is positive");
        errors[j.key.packed()] = j.error;
    }
    for (const SurfaceJob& j : L.jobs()) {
        if (j.key.level == top) continue;
        const auto p = errors.find(j.key.parent().packed());
        check(p != errors.end() && j.error <= p->second, "a tile's error is never above its parent's");
    }
    L.commit({});
    fillServed(L, service, kind, v, 2);
    pages.apply(L.pageWrites());
    int topFilled = 0, others = 0;
    for (const SurfaceFill& f : L.fills()) (f.key.level == top ? topFilled : others)++;
    check(topFilled >= 1, "the coarsest tiles are filled on the first frame");
    check(others <= 8, "the rest keep to the budget");
    check(L.stats().missing > 0, "a flight's view is not filled in one budgeted frame");
    // Every fill's parent is resident, or filled with it: the world fills
    // in from the top down.
    std::set<uint64_t> filled;
    for (const SurfaceFill& f : L.fills()) filled.insert(f.key.packed());
    for (const SurfaceFill& f : L.fills())
        if (f.key.level < top) check(filled.count(f.key.parent().packed()) == 1, "a fill's parent goes first");
    // Frames on, it converges.
    uint64_t frame = 2;
    while (L.stats().missing > 0 && frame < 400) {
        fillServed(L, service, kind, v, ++frame);
        pages.apply(L.pageWrites());
    }
    check(L.stats().missing == 0, "a still view converges under the budget (frames " + std::to_string(frame) + ")");
    check(service.stats().queued == 0 && service.stats().worstError == 0.0f, "settled, nothing is queued");
    // A new generation: every needed tile is a job again, none required
    // (each still draws, stale, until its refill lands).
    L.setGeneration(3);
    ++frame;
    L.update(v, frame);
    bool stale = !L.jobs().empty();
    for (const SurfaceJob& j : L.jobs()) stale = stale && !j.required;
    check(stale, "a new generation makes every needed tile a job, none of them required");
    L.commit({});
}

void testEvictionAndGenerations() {
    SurfaceLayerConfig c;
    c.slots = 600;
    SurfaceLayers L;
    L.configure(c, world());
    Pages pages;
    uint64_t frame = 0;
    // Walk east 2 km: tiles behind are given up for those ahead, the table
    // stays true to the slots throughout.
    std::map<int, uint64_t> slotOwner;
    for (int step = 0; step < 40; ++step) {
        const SurfaceLayers::View v = makeView({step * 50.0f, 2.0f, 0.0f}, {1.0f, -0.2f, 0.0f});
        fillAll(L, v, ++frame);
        pages.apply(L.pageWrites());
        check(L.stats().resident <= static_cast<uint32_t>(c.slots), "never more tiles than slots");
        for (const SurfaceFill& f : L.fills()) slotOwner[f.slot] = f.key.packed();
        // Every slot's current tile is still found where its entry says, and
        // no entry points at a slot another tile now holds.
        for (const auto& [key, value] : pages.entries) {
            if (value == 0) continue;
            const int slot = static_cast<int>(value & 4095u) - 1;
            const auto it = slotOwner.find(slot);
            check(it != slotOwner.end(), "an entry points at a filled slot");
            if (it == slotOwner.end()) continue;
            const CellKey k = CellKey::unpacked(it->second);
            check(static_cast<int>(k.level) == std::get<0>(key) && (k.x & (c.pageSize - 1)) == std::get<1>(key) &&
                      (k.z & (c.pageSize - 1)) == std::get<2>(key),
                  "an entry's slot holds that entry's tile");
        }
    }
    // A new generation refills the view's tiles, and keeps their entries
    // until then.
    const SurfaceLayers::View v = makeView({0.0f, 2.0f, 0.0f}, {0.0f, -0.2f, 1.0f});
    fillAll(L, v, ++frame);
    pages.apply(L.pageWrites());
    const uint32_t needed = L.stats().needed;
    L.setGeneration(7);
    fillAll(L, v, ++frame);
    check(L.stats().filled == needed, "a new generation refills every needed tile");
    for (const SurfaceFill& f : L.fills())
        check(pages.lookup(f.key, c.pageSize, L.pageShift()) == f.slot, "a refilled tile keeps its slot");
}

void testAliases() {
    SurfaceLayerConfig c;
    c.pageSize = 4;   // so tiles 4 apart share an entry
    c.slots = 4095;
    SurfaceLayers L;
    L.configure(c, world());
    Pages pages;
    const SurfaceLayers::View v = makeView({0.0f, 2.0f, 0.0f}, {0.0f, -0.2f, 1.0f});
    fillAll(L, v, 1);
    pages.apply(L.pageWrites());
    std::set<std::tuple<int, int, int>> used;
    for (const SurfaceFill& f : L.fills())
        check(used.insert({f.key.level, f.key.x & 3, f.key.z & 3}).second, "no two tiles paged into one entry");
    for (const SurfaceFill& f : L.fills())
        check(pages.lookup(f.key, c.pageSize, L.pageShift()) == f.slot, "each paged tile found through its entry");
}

void testItems() {
    SurfaceLayerConfig c;
    c.slots = 4095;
    SurfaceLayers L;
    L.configure(c, world());
    // Ground only east of x = 0, in 100 m items.
    std::vector<SurfaceItem> items;
    for (int z = -50; z < 50; ++z)
        for (int x = 0; x < 50; ++x)
            items.push_back({{{x * 100.0f, -1.0f, z * 100.0f}, {(x + 1) * 100.0f, 1.0f, (z + 1) * 100.0f}},
                             static_cast<uint64_t>((z + 50) * 50 + x + 1)});
    L.setItems(items);
    const SurfaceLayers::View v = makeView({0.0f, 2.0f, 0.0f}, {1.0f, -0.2f, 0.0f});
    fillAll(L, v, 1);
    bool west = false;
    for (const SurfaceFill& f : L.fills())
        if (f.x0 + f.side <= -1.0) west = true;
    check(!west, "no tile is needed where no item lies");
    check(L.stats().missing == 0 && L.stats().filled > 0, "the items' tiles fill");

    // One item changes: only the tiles over it (and their ancestors) refill.
    fillAll(L, v, 2);
    check(L.fills().empty(), "unchanged items fill nothing");
    const size_t changed = 50 * 50 + 1;   // the item at x 100..200, z 0..100
    items[changed].generation = 999999;
    L.setItems(items);
    fillAll(L, v, 3);
    const bromath::AABB3& b = items[changed].bounds;
    int over = 0;
    for (const SurfaceFill& f : L.fills()) {
        // (With the tile's border, its texels past the edge reading the
        // item, and as coarse as the cells the items are gathered into: a
        // 10 km world's are 32 m, a coarser tile's its own size.)
        const double pad = f.side / c.tileTexels * c.border + std::max(f.side, 32.0) + 32.0;
        const bool overlaps = f.x0 - pad < b.max.x && f.x0 + f.side + pad > b.min.x && f.z0 - pad < b.max.z &&
                              f.z0 + f.side + pad > b.min.z;
        check(overlaps, "a refilled tile lies over the changed item (level " + std::to_string(f.key.level) + " at " +
                            std::to_string(f.x0) + ", " + std::to_string(f.z0) + ")");
        ++over;
    }
    check(over > 0, "the tiles over the changed item refill");
}

}  // namespace

int main() {
    testSelection();
    testBudgetAndCoarsest();
    testEvictionAndGenerations();
    testAliases();
    testItems();
    std::printf("surface layers: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
