// TileTree, CPU only: what a view wants, the order tiles are produced in,
// the draw set's coverage, and eviction.

#include "tilecache/tile_tree.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>

using namespace bro::tilecache;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
}

// The draw set covers each root once: areas (a tile is 4^-level of its root)
// sum to the number of roots drawn, and no tile is another's ancestor.
void checkCover(const TileTree& tree, uint32_t rootsExpected, const std::string& what) {
    const auto set = tree.drawSet();
    double area = 0;
    std::unordered_set<uint64_t> keys;
    for (const DrawTile& d : set) {
        area += std::ldexp(1.0, -2 * static_cast<int>(d.id.level));
        keys.insert(d.id.key());
    }
    bool overlap = false;
    for (const DrawTile& d : set) {
        for (TileId a = d.id; a.level > 0;) {
            a = a.parent();
            if (keys.count(a.key())) overlap = true;
        }
    }
    check(std::fabs(area - rootsExpected) < 1e-9, what + ": the draw set covers " + std::to_string(rootsExpected) +
                                                      " root(s) (area " + std::to_string(area) + ")");
    check(!overlap, what + ": no drawn tile is another's ancestor");
}

// Produce every wanted tile, checking each comes after its parent.
uint32_t produceAll(TileTree& tree, uint32_t perBatch, bool& parentFirst) {
    uint32_t n = 0;
    for (int guard = 0; guard < 10000; ++guard) {
        auto batch = tree.takeProductions(perBatch);
        if (batch.empty()) break;
        std::unordered_set<uint64_t> earlier;
        for (const Production& p : batch) {
            if (p.id.level > 0 && tree.slotOf(p.id.parent()) < 0 && !earlier.count(p.id.parent().key()))
                parentFirst = false;
            if (p.id.level > 0 && p.parentSlot < 0) parentFirst = false;
            earlier.insert(p.id.key());
        }
        for (const Production& p : batch) tree.markReady(p.id);
        n += static_cast<uint32_t>(batch.size());
    }
    return n;
}

View viewAt(double x, double y, double z) {
    View v;
    v.position = {x, y, z};
    v.projScale = 1000.0 / (2 * std::tan(30.0 * 3.14159265358979 / 180.0));   // 1000 px, 60 degrees
    return v;
}

Config planeConfig(uint32_t capacity) {
    Config c;
    c.domain.kind = Domain::Kind::Plane;
    c.domain.origin[0] = -512;
    c.domain.origin[1] = -512;
    c.domain.size = 1024;
    c.tileSize = 32;
    c.capacity = capacity;
    c.maxLevel = 12;
    c.pixelsPerCell = 2;
    return c;
}

void testPlaneSelection() {
    TileTree tree(planeConfig(512));
    tree.select(viewAt(0, 1e7, 0));
    check(tree.wanted() == 1, "far above the plane only the root is wanted");
    check(tree.drawSet().empty(), "nothing is drawn before anything is produced");

    tree.select(viewAt(13.7, 10, 21.3));   // off the corners every level shares
    check(tree.wanted() > 40, "near the plane the tree refines (" + std::to_string(tree.wanted()) + " wanted)");
    check(tree.wanted() <= 512, "the wanted tree fits the cache");
    check(tree.pending() == tree.wanted(), "every wanted tile is pending");

    // Part of the way: whatever is ready covers the plane without overlap.
    bool parentFirst = true;
    auto first = tree.takeProductions(7);
    for (const Production& p : first) tree.markReady(p.id);
    checkCover(tree, 1, "after 7 tiles");

    produceAll(tree, 16, parentFirst);
    check(parentFirst, "every tile is produced after its parent, with its parent's slot");
    check(tree.pending() == 0, "everything wanted is produced");
    checkCover(tree, 1, "fully produced");

    // The tile under the camera is the finest drawn.
    uint32_t deepest = 0, under = 0;
    for (const DrawTile& d : tree.drawSet()) {
        deepest = std::max(deepest, d.id.level);
        const double ext = tree.config().domain.tileExtent(d.id);
        const Vec3d o = tree.config().domain.point(d.id, 0, 0);
        if (o.x <= 13.7 && 13.7 < o.x + ext && o.z <= 21.3 && 21.3 < o.z + ext) under = d.id.level;
    }
    check(under == deepest && deepest >= 6, "the tile under the camera is the finest (level " + std::to_string(under) +
                                                " of " + std::to_string(deepest) + ")");
    for (const DrawTile& d : tree.drawSet())
        if (d.id.level > 0 && d.parentSlot < 0) check(false, "a drawn tile's parent is resident");
}

void testEvictionAndRefill() {
    TileTree tree(planeConfig(64));
    bool parentFirst = true;
    tree.select(viewAt(-400, 5, -400));
    check(tree.wanted() <= 64, "a small cache bounds the wanted tree");
    produceAll(tree, 64, parentFirst);
    const uint32_t resident = tree.resident();
    check(tree.pending() == 0 && resident == tree.wanted(), "all wanted tiles resident");

    // Across the plane: the old corner's fine tiles give up their slots.
    tree.select(viewAt(400, 5, 400));
    check(tree.pending() > 0, "a new corner is pending");
    checkCover(tree, 1, "after moving, before refilling (the coarse tiles still cover)");
    produceAll(tree, 64, parentFirst);
    check(tree.pending() == 0, "the new corner is produced");
    check(tree.stats().evicted > 0, "tiles were evicted to make room (" + std::to_string(tree.stats().evicted) + ")");
    check(parentFirst, "refilling keeps parents first");
    checkCover(tree, 1, "after refilling");

    // Abandoning a production hands its slot back.
    tree.select(viewAt(-400, 5, 400));
    const uint32_t before = tree.resident();
    auto batch = tree.takeProductions(1);
    check(batch.size() == 1, "one tile to produce");
    tree.abandon(batch[0].id);
    check(tree.resident() <= before, "an abandoned tile frees its slot");
    check(tree.slotOf(batch[0].id) < 0, "an abandoned tile is not ready");

    tree.clear();
    check(tree.resident() == 0 && tree.drawSet().empty(), "clear() frees every slot");
}

void testCubeSphere() {
    Config c;
    c.domain.kind = Domain::Kind::CubeSphere;
    c.domain.radius = 6.371e6;
    c.tileSize = 64;
    c.capacity = 1024;
    c.maxLevel = 20;
    c.maxHeight = 9000;
    TileTree tree(c);

    // Every root's base surface lies on the sphere.
    bool onSphere = true;
    for (uint32_t r = 0; r < 6; ++r) {
        const Vec3d p = c.domain.point({r, 3, 5, 2}, 0.3, 0.7);
        if (std::fabs(std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z) - c.domain.radius) > 1e-3) onSphere = false;
    }
    check(onSphere, "cube-sphere points lie on the sphere");

    tree.select(viewAt(0, c.domain.radius + 200, 0));   // 200 m above the +Y face's centre
    bool parentFirst = true;
    produceAll(tree, 64, parentFirst);
    checkCover(tree, 6, "cube-sphere");
    uint32_t deepestTop = 0, deepestOther = 0;
    for (const DrawTile& d : tree.drawSet()) {
        if (d.id.root == 2) deepestTop = std::max(deepestTop, d.id.level);
        else deepestOther = std::max(deepestOther, d.id.level);
    }
    check(deepestTop >= 12 && deepestTop > deepestOther + 4,
          "refinement concentrates under the camera (+Y face level " + std::to_string(deepestTop) +
              ", others " + std::to_string(deepestOther) + ")");
}

} // namespace

int main() {
    testPlaneSelection();
    testEvictionAndRefill();
    testCubeSphere();
    std::printf("bro_tilecache_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
