// Test NavGrid baking from a physics world — createNavGrid({ fromPhysics })
// derives obstacles from static, non-sensor body AABBs (projected to XZ),
// with layer / Y-band filters, the whole-grid-footprint floor skip, and
// support for both sandbox world handles and the default Physics world.
// Exercises PhysicsWorld::collectStaticBodies + js_createNavGrid
// (src/physics/physics_world.cpp + src/js/ai_bindings.cpp).

const G = bro.ai.game;
assert(typeof G.createNavGrid === 'function', 'createNavGrid exists');
assert(typeof Physics === 'object', 'Physics available');

// =========================================================================
// Sandbox world bake
// =========================================================================
const w = Physics.createWorldHandle({ maxBodies: 256 });

// Static obstacle: 4x4 box centered at origin.
const box = w.createBody({
    shape: 'box', halfExtents: { x: 2, y: 1, z: 2 },
    position: { x: 0, y: 0, z: 0 }, static: true,
});
assert(box > 0, 'obstacle body');

// Floor spanning the whole grid — must be skipped (full-footprint heuristic).
w.createBody({
    shape: 'box', halfExtents: { x: 100, y: 0.5, z: 100 },
    position: { x: 0, y: -2, z: 0 }, static: true,
});

// Dynamic body — not static, not baked.
w.createBody({
    shape: 'box', halfExtents: { x: 1, y: 1, z: 1 },
    position: { x: 5, y: 0, z: 5 },
});

// Static sensor — overlap trigger, doesn't block movement, not baked.
w.createBody({
    shape: 'box', halfExtents: { x: 1, y: 1, z: 1 },
    position: { x: -5, y: 0, z: -5 }, static: true, sensor: true,
});

// Tall static pillar high above the walkable slab (for the Y-band test).
w.createBody({
    shape: 'box', halfExtents: { x: 1, y: 1, z: 1 },
    position: { x: 7, y: 10, z: -6 }, static: true,
});

const nav = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromPhysics: w,
});
assert(nav !== null, 'baked nav grid');

assert(!nav.isWalkable(0, 0), 'static box center is blocked');
assert(!nav.isWalkable(1.5, 1.5), 'static box corner is blocked');
assert(nav.isWalkable(5, 5), 'dynamic body is not baked');
assert(nav.isWalkable(-5, -5), 'sensor is not baked');
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

// =========================================================================
// Y band filter — exclude geometry outside the walkable slab
// =========================================================================
const navBand = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromPhysics: w,
    physicsMinY: -1, physicsMaxY: 5,   // pillar spans y 9..11 → excluded
});
assert(!navBand.isWalkable(0, 0), 'ground-level box still baked with Y band');
assert(navBand.isWalkable(7, -6), 'pillar above the Y band is skipped');

// =========================================================================
// Layer filter
// =========================================================================
w.setLayers({
    names: ['static', 'moving', 'deco'],
    matrix: [
        false, true, false,
        true,  true, true,
        false, true, false,
    ],
});
w.createBody({
    shape: 'box', halfExtents: { x: 1, y: 1, z: 1 },
    position: { x: -7, y: 0, z: 0 }, static: true, layer: 'deco',
});
const navLayers = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromPhysics: w,
    physicsLayers: ['static'],
});
assert(!navLayers.isWalkable(0, 0), "layer filter keeps 'static' bodies");
assert(navLayers.isWalkable(-7, 0), "layer filter drops 'deco' bodies");

// Padding applies to baked obstacles too.
const navPad = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromPhysics: w, padding: 1.0,
});
assert(!navPad.isWalkable(2.8, 0), 'padding expands baked obstacle');

w.destroy();

// =========================================================================
// Default world bake — fromPhysics: Physics (or true)
// =========================================================================
Physics.destroyAll();
const defBox = Physics.createBody({
    shape: 'box', halfExtents: { x: 2, y: 1, z: 2 },
    position: { x: 3, y: 0, z: 3 }, static: true,
});
assert(defBox > 0, 'default-world obstacle');

const navDef = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromPhysics: Physics,
});
assert(!navDef.isWalkable(3, 3), 'default world (Physics) baked');
assert(navDef.isWalkable(-3, -3), 'open space walkable');

const navTrue = G.createNavGrid({
    minX: -10, minZ: -10, maxX: 10, maxZ: 10, cellSize: 0.5,
    fromPhysics: true,
});
assert(!navTrue.isWalkable(3, 3), 'fromPhysics: true uses the default world');

Physics.destroyAll();

console.log('test_navgrid_physics: OK');
