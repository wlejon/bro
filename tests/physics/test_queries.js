// Test narrow-phase physics queries — castShape (all-hits + closest),
// overlapShape, overlapPoint, layer filtering, ignoreBody, and the
// heightfield collision shape (create, query against, settle a body on it).
// Exercises src/js/physics_bindings.cpp + src/physics/physics_world.cpp.

assert(typeof Physics === 'object', 'Physics namespace exists');
assert(typeof Physics.castShape === 'function', 'castShape exists');
assert(typeof Physics.castShapeClosest === 'function', 'castShapeClosest exists');
assert(typeof Physics.overlapShape === 'function', 'overlapShape exists');
assert(typeof Physics.overlapPoint === 'function', 'overlapPoint exists');

Physics.destroyAll();
Physics.setLayers({
    names: ['static', 'moving', 'ghost'],
    matrix: [
        false, true,  false,
        true,  true,  false,
        false, false, false,   // ghost collides with nothing (queries still see it)
    ],
});

// Ground slab: top face at y = 0.5.
const ground = Physics.createBody({
    shape: 'box', halfExtents: { x: 50, y: 0.5, z: 50 },
    position: { x: 0, y: 0, z: 0 }, static: true,
});

// Box in the cast path, on the ghost layer: top face at y = 5.5.
const midBox = Physics.createBody({
    shape: 'box', halfExtents: { x: 1, y: 0.5, z: 1 },
    position: { x: 0, y: 5, z: 0 }, static: true, layer: 'ghost',
});

// =========================================================================
// castShape — all hits, sorted by fraction
// =========================================================================
// Sphere r=0.5 swept down from y=10 over 20m: touches midBox when its center
// reaches y=6.0 (fraction 0.2), ground at center y=1.0 (fraction 0.45).
const hits = Physics.castShape({
    shape: 'sphere', radius: 0.5,
    position: { x: 0, y: 10, z: 0 },
    direction: { x: 0, y: -1, z: 0 },
    maxDistance: 20,
});
assert(Array.isArray(hits), 'castShape returns array');
assert(hits.length === 2, 'castShape hits both bodies, got ' + hits.length);
assert(hits[0].bodyId === midBox, 'first hit is midBox');
assert(Math.abs(hits[0].fraction - 0.2) < 0.01, 'midBox fraction ~0.2, got ' + hits[0].fraction);
assert(hits[1].bodyId === ground, 'second hit is ground');
assert(Math.abs(hits[1].fraction - 0.45) < 0.01, 'ground fraction ~0.45, got ' + hits[1].fraction);
assert(Math.abs(hits[0].position.y - 5.5) < 0.02, 'contact point on midBox top, got ' + hits[0].position.y);
assert(hits[0].normal.y > 0.99, 'contact normal points up, got ' + hits[0].normal.y);
assert(typeof hits[0].userData === 'bigint', 'cast hit carries userData');

// =========================================================================
// castShapeClosest — nearest hit only
// =========================================================================
const closest = Physics.castShapeClosest({
    shape: 'sphere', radius: 0.5,
    position: { x: 0, y: 10, z: 0 },
    direction: { x: 0, y: -1, z: 0 },
    maxDistance: 20,
});
assert(closest !== null, 'castShapeClosest hit');
assert(closest.bodyId === midBox, 'closest is midBox');
assert(Math.abs(closest.fraction - 0.2) < 0.01, 'closest fraction ~0.2');

// Capsule cast: bottom tip at halfHeight+radius = 0.8 below center; touches
// ground (top 0.5) at center y = 1.3 → fraction (10-1.3)/20 = 0.435.
const capHit = Physics.castShapeClosest({
    shape: 'capsule', radius: 0.3, halfHeight: 0.5,
    position: { x: 20, y: 10, z: 20 },
    direction: { x: 0, y: -1, z: 0 },
    maxDistance: 20,
});
assert(capHit !== null && capHit.bodyId === ground, 'capsule cast hits ground');
assert(Math.abs(capHit.fraction - 0.435) < 0.01, 'capsule fraction ~0.435, got ' + capHit.fraction);

// Miss: cast into empty space.
const miss = Physics.castShapeClosest({
    shape: 'sphere', radius: 0.5,
    position: { x: 0, y: 10, z: 0 },
    direction: { x: 0, y: 1, z: 0 },
    maxDistance: 20,
});
assert(miss === null, 'castShapeClosest returns null on miss');
assert(Physics.castShape({
    shape: 'sphere', radius: 0.5, position: { x: 0, y: 10, z: 0 },
    direction: { x: 0, y: 1, z: 0 }, maxDistance: 20,
}).length === 0, 'castShape returns [] on miss');

// =========================================================================
// Layer filtering + ignoreBody
// =========================================================================
// Only 'static' layer visible → skips the ghost-layer midBox.
const staticOnly = Physics.castShapeClosest({
    shape: 'sphere', radius: 0.5,
    position: { x: 0, y: 10, z: 0 },
    direction: { x: 0, y: -1, z: 0 },
    maxDistance: 20,
    layers: ['static'],
});
assert(staticOnly !== null && staticOnly.bodyId === ground, 'layer filter skips ghost body');

// Only 'ghost' visible → only midBox.
const ghostOnly = Physics.castShape({
    shape: 'sphere', radius: 0.5,
    position: { x: 0, y: 10, z: 0 },
    direction: { x: 0, y: -1, z: 0 },
    maxDistance: 20,
    layers: ['ghost'],
});
assert(ghostOnly.length === 1 && ghostOnly[0].bodyId === midBox, 'ghost-only filter');

// ignoreBody excludes the midBox → ground is nearest.
const ignored = Physics.castShapeClosest({
    shape: 'sphere', radius: 0.5,
    position: { x: 0, y: 10, z: 0 },
    direction: { x: 0, y: -1, z: 0 },
    maxDistance: 20,
    ignoreBody: midBox,
});
assert(ignored !== null && ignored.bodyId === ground, 'ignoreBody skips midBox');

// Non-convex query shape and missing direction must throw.
let threw = false;
try { Physics.castShape({ shape: 'compound', parts: [{ shape: 'box' }], direction: { x: 0, y: -1, z: 0 } }); }
catch (e) { threw = true; }
assert(threw, 'non-convex query shape throws');
threw = false;
try { Physics.castShape({ shape: 'sphere', radius: 0.5 }); }
catch (e) { threw = true; }
assert(threw, 'zero direction throws');

// =========================================================================
// overlapShape — bodies overlapping a shape at a transform
// =========================================================================
// Two dynamic spheres above the ground (world not stepped, they stay put).
const sphA = Physics.createBody({ shape: 'sphere', radius: 0.5, position: { x: 20, y: 3, z: 0 } });
const sphB = Physics.createBody({ shape: 'sphere', radius: 0.5, position: { x: 21, y: 3, z: 0 } });
const sphC = Physics.createBody({ shape: 'sphere', radius: 0.5, position: { x: 40, y: 3, z: 0 } });
Physics.setUserData(sphA, 777);

const ov = Physics.overlapShape({
    shape: 'box', halfExtents: { x: 1.5, y: 1, z: 1 },
    position: { x: 20.5, y: 3, z: 0 },
});
const ovIds = ov.map(h => h.bodyId).sort((a, b) => a - b);
assert(ovIds.length === 2 && ovIds[0] === Math.min(sphA, sphB) && ovIds[1] === Math.max(sphA, sphB),
       'overlapShape finds exactly {A, B}, got [' + ovIds + ']');
for (const h of ov) {
    assert(h.depth > 0, 'overlap depth > 0');
    assert(typeof h.position === 'object' && typeof h.normal === 'object', 'overlap has contact info');
}
const hitA = ov.find(h => h.bodyId === sphA);
assert(Number(hitA.userData) === 777, 'overlap hit carries userData');

// Layer filter: ghost-only overlap at midBox finds just midBox.
const ovGhost = Physics.overlapShape({
    shape: 'box', halfExtents: { x: 2, y: 2, z: 2 },
    position: { x: 0, y: 5, z: 0 },
    layers: ['ghost'],
});
assert(ovGhost.length === 1 && ovGhost[0].bodyId === midBox, 'overlapShape ghost-layer filter');

// ignoreBody on overlap.
const ovIgn = Physics.overlapShape({
    shape: 'box', halfExtents: { x: 1.5, y: 1, z: 1 },
    position: { x: 20.5, y: 3, z: 0 },
    ignoreBody: sphA,
});
assert(ovIgn.length === 1 && ovIgn[0].bodyId === sphB, 'overlapShape ignoreBody');

// =========================================================================
// overlapPoint — bodies containing a point
// =========================================================================
const atA = Physics.overlapPoint(20, 3, 0);
assert(atA.length === 1 && atA[0].bodyId === sphA, 'overlapPoint inside sphere A');
assert(Number(atA[0].userData) === 777, 'overlapPoint carries userData');

const inGround = Physics.overlapPoint(0, 0.2, 30);
assert(inGround.length === 1 && inGround[0].bodyId === ground, 'overlapPoint inside ground');

assert(Physics.overlapPoint(25, 3, 0).length === 0, 'overlapPoint in empty space');
assert(Physics.overlapPoint(20, 3, 0, { ignoreBody: sphA }).length === 0, 'overlapPoint ignoreBody');
assert(Physics.overlapPoint(0, 0.2, 30, { layers: ['moving'] }).length === 0,
       'overlapPoint layer filter excludes ground');

// =========================================================================
// Heightfield shape — bowl h(x,z) = 0.02*((x-32)^2 + (z-32)^2)
// =========================================================================
const n = 64;
const heights = new Float32Array(n * n);
for (let z = 0; z < n; z++)
    for (let x = 0; x < n; x++)
        heights[z * n + x] = 0.02 * ((x - 32) * (x - 32) + (z - 32) * (z - 32));

// Placed away from the ground slab (which spans ±50 around the origin).
const hf = Physics.createBody({
    shape: 'heightfield',
    heights, sampleCount: n,
    scale: { x: 1, y: 1, z: 1 },
    position: { x: 100, y: 0, z: 100 },
});
assert(hf > 0, 'heightfield body created');

// Raycast against it (broadphase — hit presence + body identity).
const rayHits = Physics.raycast(132, 100, 132, 0, -1, 0, 200);
assert(rayHits.some(h => h.bodyId === hf), 'raycast hits heightfield');

// Shape-cast down onto the analytic surface. At world (116, z=132) the local
// sample is (16, 32) → h = 0.02*256 = 5.12.
const hfHit = Physics.castShapeClosest({
    shape: 'sphere', radius: 0.1,
    position: { x: 116, y: 60, z: 132 },
    direction: { x: 0, y: -1, z: 0 },
    maxDistance: 100,
});
assert(hfHit !== null && hfHit.bodyId === hf, 'shape cast hits heightfield');
assert(Math.abs(hfHit.position.y - 5.12) < 0.2,
       'heightfield contact near analytic height 5.12, got ' + hfHit.position.y);

// Bowl bottom (locally flat): sphere r=0.5 from y=20, surface y=0 →
// center stops at 0.5, fraction (20-0.5)/40 = 0.4875.
const bowlHit = Physics.castShapeClosest({
    shape: 'sphere', radius: 0.5,
    position: { x: 132, y: 20, z: 132 },
    direction: { x: 0, y: -1, z: 0 },
    maxDistance: 40,
});
assert(bowlHit !== null && bowlHit.bodyId === hf, 'bowl-bottom cast hits heightfield');
assert(Math.abs(bowlHit.position.y) < 0.1, 'bowl bottom at y~0, got ' + bowlHit.position.y);
assert(Math.abs(bowlHit.fraction - 0.4875) < 0.01, 'bowl fraction ~0.4875, got ' + bowlHit.fraction);

// =========================================================================
// Sandbox world: heightfield settle + queries (caller-driven stepping)
// =========================================================================
const w = Physics.createWorldHandle({ maxBodies: 64 });
assert(typeof w.castShape === 'function', 'sandbox castShape exists');
assert(typeof w.castShapeClosest === 'function', 'sandbox castShapeClosest exists');
assert(typeof w.overlapShape === 'function', 'sandbox overlapShape exists');
assert(typeof w.overlapPoint === 'function', 'sandbox overlapPoint exists');

const wHf = w.createBody({
    shape: 'heightfield', heights, sampleCount: n,
    position: { x: 0, y: 0, z: 0 },
});
assert(wHf > 0, 'sandbox heightfield created');

// Drop a sphere into the bowl; it must come to rest with its center ~0.5
// above the bowl bottom (surface y=0 at local (32,32)).
const ball = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 32, y: 8, z: 32 } });
for (let i = 0; i < 300; i++) w.step(1 / 60);
const rest = w.getTransform(ball).position;
assert(Math.abs(rest.y - 0.5) < 0.15, 'ball rests on heightfield at y~0.5, got ' + rest.y);
assert(Math.abs(rest.x - 32) < 1.0 && Math.abs(rest.z - 32) < 1.0, 'ball stays near bowl bottom');
const restVel = w.getVelocity(ball).linear;
assert(Math.abs(restVel.x) + Math.abs(restVel.y) + Math.abs(restVel.z) < 0.5, 'ball has settled');

// Sandbox queries see both bodies.
const wCast = w.castShapeClosest({
    shape: 'sphere', radius: 0.5,
    position: { x: 32, y: 10, z: 32 },
    direction: { x: 0, y: -1, z: 0 },
    maxDistance: 20,
    ignoreBody: ball,
});
assert(wCast !== null && wCast.bodyId === wHf, 'sandbox cast hits heightfield');
const wAtBall = w.overlapPoint(rest.x, rest.y, rest.z);
assert(wAtBall.length === 1 && wAtBall[0].bodyId === ball, 'sandbox overlapPoint finds ball');
const wOv = w.overlapShape({
    shape: 'box', halfExtents: { x: 1, y: 1, z: 1 },
    position: { x: 32, y: 0.5, z: 32 },
});
const wOvIds = wOv.map(h => h.bodyId).sort((a, b) => a - b);
assert(wOvIds.length === 2, 'sandbox overlapShape finds ball + heightfield, got [' + wOvIds + ']');

w.destroy();

// =========================================================================
// Cleanup
// =========================================================================
Physics.destroyAll();
