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

// With a frustum, tiles outside it are not refined and not drawn, and on a
// cube-sphere neither are tiles past the horizon.
void testFrustum() {
    TileTree all(planeConfig(4096)), seen(planeConfig(4096));
    View v = viewAt(13.7, 10, 21.3);
    all.select(v);
    v.frustum = true;
    v.forward = {1, 0, 0};       // looking along +x, level with the plane
    v.right = {0, 0, 1};
    v.up = {0, 1, 0};
    v.tanHalfX = v.tanHalfY = std::tan(30.0 * 3.14159265358979 / 180.0);
    seen.select(v);
    check(seen.wanted() < all.wanted() * 3 / 4, "a frustum refines fewer tiles (" + std::to_string(seen.wanted()) +
                                                    " of " + std::to_string(all.wanted()) + ")");
    bool parentFirst = true;
    produceAll(seen, 64, parentFirst);
    bool behind = false, under = false;
    for (const DrawTile& d : seen.drawSet()) {
        const double ext = seen.config().domain.tileExtent(d.id);
        const Vec3d o = seen.config().domain.point(d.id, 0, 0);
        if (o.x + ext < 13.7 - d.bounds.radius * 2) behind = true;
        // 30 m ahead the ground is 18 degrees down, inside the 30 degree half-angle.
        if (o.x <= 43.7 && 43.7 < o.x + ext && o.z <= 21.3 && 21.3 < o.z + ext && d.id.level >= 6) under = true;
    }
    check(!behind, "nothing wholly behind the camera is drawn");
    check(under, "the ground in view ahead still refines");

    Config c;
    c.domain.kind = Domain::Kind::CubeSphere;
    c.domain.radius = 6.371e6;
    c.tileSize = 64;
    c.capacity = 4096;
    c.maxLevel = 20;
    c.maxHeight = 9000;
    TileTree sphere(c);
    View s = viewAt(0, c.domain.radius + 2000, 0);
    s.frustum = true;
    s.forward = {1, 0, 0};       // toward the horizon, which is ~160 km away
    s.right = {0, 0, 1};
    s.up = {0, 1, 0};
    s.tanHalfX = s.tanHalfY = 1;
    sphere.select(s);
    produceAll(sphere, 256, parentFirst);
    bool farSide = false;
    for (const DrawTile& d : sphere.drawSet())
        if (d.bounds.center.y < 0 || d.bounds.center.x > 2e6) farSide = true;
    check(!farSide, "a cube-sphere draws nothing past the horizon");
    check(!sphere.drawSet().empty(), "and draws the ground in view");
}

// Height ranges: a tile without one takes its nearest ancestor's (else the
// config's), and measured ranges narrow selection. A camera 2 m over a
// plateau that never leaves [3000, 3040] m, looking straight down, wants far
// fewer tiles once the tiles say so than under the 9 km worst case, and the
// ground below it still refines.
void testRanges() {
    Config c;
    c.domain.kind = Domain::Kind::CubeSphere;
    c.domain.radius = 6.371e6;
    c.tileSize = 32;
    c.capacity = 4096;
    c.maxLevel = 22;
    c.maxHeight = 9000;
    TileTree tree(c);

    const TileId root{2, 0, 0, 0}, deep{2, 5, 10, 11};
    check(tree.range(deep).lo == 0 && tree.range(deep).hi == 9000, "no range set: the config's");
    tree.setRange(root, {-10, 40});
    check(tree.range(deep).lo == -10 && tree.range(deep).hi == 40, "a tile takes its nearest ancestor's range");
    tree.setRange(deep, {std::nan(""), 1});
    check(tree.range(deep).hi == 40, "a NaN range is refused");
    const double before = tree.bounds(deep).radius;
    tree.setRange(deep, {0, 20});
    check(tree.bounds(deep).radius < before, "a narrower range shrinks the bounds");
    tree.clear();
    check(tree.range(deep).hi == 9000, "clear() forgets ranges");

    // Over a plateau 3 km up: the range's bounds sit well above the base.
    View v = viewAt(0, c.domain.radius + 3002, 0);
    v.frustum = true;
    v.forward = {0, -1, 0};
    v.right = {1, 0, 0};
    v.up = {0, 0, -1};
    v.tanHalfX = v.tanHalfY = std::tan(30.0 * 3.14159265358979 / 180.0);
    tree.select(v);
    const uint32_t worst = tree.wanted();
    bool parentFirst = true;
    for (int round = 0; round < 6; ++round) {
        produceAll(tree, 4096, parentFirst);
        for (const DrawTile& d : tree.drawSet()) tree.setRange(d.id, {3000, 3040});
        for (uint32_t r = 0; r < 6; ++r) tree.setRange({r, 0, 0, 0}, {3000, 3040});
        tree.select(v);
    }
    check(tree.wanted() * 4 < worst, "measured ranges cut what a ground view wants (" +
                                         std::to_string(tree.wanted()) + " of " + std::to_string(worst) + ")");
    uint32_t deepest = 0;
    for (const DrawTile& d : tree.drawSet()) deepest = std::max(deepest, d.id.level);
    check(deepest >= 18, "and the ground below still refines (level " + std::to_string(deepest) + ")");
}

// A long flight: the tree keeps nodes only for what it still needs, so the
// tiles it knows stay within a few capacities however far the view goes, and
// the draw set covers the domain at every step.
void testLongFlight() {
    Config c = planeConfig(256);
    c.domain.size = 1 << 20;
    c.domain.origin[0] = c.domain.origin[1] = -(1 << 19);
    c.maxLevel = 18;
    TileTree tree(c);
    bool parentFirst = true, covered = true;
    uint32_t mostKnown = 0;
    for (int step = 0; step < 400; ++step) {
        const double x = -400000 + step * 2000.0;
        tree.select(viewAt(x, 30, 0.37 * x));
        produceAll(tree, 24, parentFirst);
        for (const DrawTile& d : tree.drawSet()) tree.setRange(d.id, {0, 1});
        mostKnown = std::max(mostKnown, tree.known());
        if (step % 50 == 0) {
            double area = 0;
            for (const DrawTile& d : tree.drawSet()) area += std::ldexp(1.0, -2 * static_cast<int>(d.id.level));
            covered = covered && std::fabs(area - 1) < 1e-9;
        }
    }
    check(parentFirst, "a long flight produces parents first");
    check(covered, "a long flight's draw set covers the plane at every check");
    check(mostKnown <= 6 * c.capacity, "the tree knows at most a few capacities of tiles over a long flight (" +
                                           std::to_string(mostKnown) + " for capacity " +
                                           std::to_string(c.capacity) + ")");
    check(tree.stats().evicted > 1000, "and evicted as it went (" + std::to_string(tree.stats().evicted) + ")");
}

} // namespace

int main() {
    testPlaneSelection();
    testEvictionAndRefill();
    testCubeSphere();
    testFrustum();
    testRanges();
    testLongFlight();
    std::printf("bro_tilecache_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
