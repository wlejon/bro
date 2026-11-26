// NavGrid baking from a bro.dynamics world: createNavGrid({ fromDynamics })
// takes its obstacles from the bounds of the world's fixed bodies (projected
// to XZ), filtered by layer mask (dynamicsMask) and Y band (dynamicsMinY /
// dynamicsMaxY), leaving out a floor under the whole grid.
// (dynamics::World::fixedBounds, host_sibling_apis.cpp collectNavObstacles.)

const G = bro.ai.game;
assert(typeof G.createNavGrid === 'function', 'createNavGrid exists');
assert(bro.dynamics.available === true, 'bro.dynamics available');

const w = bro.dynamics.createWorld();
const DECO = 2;

// Fixed obstacle: 4x4 box centered at origin.
const box = w.addBox({ half: [2, 1, 2], position: [0, 0, 0], mass: 0 });
assert(w.hasBody(box), 'obstacle body');

// Floor spanning the whole grid: left out.
w.addBox({ half: [100, 0.5, 100], position: [0, -2, 0], mass: 0 });

// A moving body is not baked.
w.addBox({ half: [1, 1, 1], position: [5, 0, 5], mass: 10 });

// Tall fixed pillar high above the walkable slab (for the Y-band test).
w.addBox({ half: [1, 1, 1], position: [7, 10, -6], mass: 0 });

const nav = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromDynamics: w,
});
assert(nav !== null, 'baked nav grid');

assert(!nav.isWalkable(0, 0), 'fixed box center is blocked');
assert(!nav.isWalkable(1.5, 1.5), 'fixed box corner is blocked');
assert(nav.isWalkable(5, 5), 'moving body is not baked');
assert(nav.isWalkable(-8, 8), 'floor (full grid footprint) is not baked');
assert(!nav.isWalkable(7, -6), 'elevated pillar baked without a Y band');

// Routing goes around the baked box.
const path = nav.findPath(-8, 0, 8, 0);
assert(path.length >= 2, 'path around baked obstacle found, ' + path.length + ' waypoints');
for (const p of path)
    assert(nav.isWalkable(p.x, p.z), 'waypoint (' + p.x + ',' + p.z + ') is walkable');
// The straight line pierces the box, so some waypoint must detour laterally.
let maxAbsZ = 0;
for (const p of path) maxAbsZ = Math.max(maxAbsZ, Math.abs(p.z));
assert(maxAbsZ > 1.9, 'path detours around the box (max |z| = ' + maxAbsZ + ')');

// Y band: geometry outside the walkable slab is left out.
const navBand = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromDynamics: w,
    dynamicsMinY: -1, dynamicsMaxY: 5,   // pillar spans y 9..11
});
assert(!navBand.isWalkable(0, 0), 'ground-level box still baked with Y band');
assert(navBand.isWalkable(7, -6), 'pillar above the Y band is skipped');

// Layer mask.
w.addBox({ half: [1, 1, 1], position: [-7, 0, 0], mass: 0, layers: DECO });
const navAll = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromDynamics: w,
});
assert(!navAll.isWalkable(-7, 0), 'no mask: every layer is baked');
const navLayers = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromDynamics: w,
    dynamicsMask: 1,
});
assert(!navLayers.isWalkable(0, 0), 'mask keeps layer 1 bodies');
assert(navLayers.isWalkable(-7, 0), 'mask drops the other layer');

// Padding applies to baked obstacles too.
const navPad = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromDynamics: w, padding: 1.0,
});
assert(!navPad.isWalkable(2.8, 0), 'padding expands baked obstacle');

w.destroy();

console.log('test_navgrid_dynamics: OK');
