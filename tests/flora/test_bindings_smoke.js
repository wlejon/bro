// Exercise the per-plant emit, plantInfo, setClimate, and sampleShadow
// bindings of bro.flora. Weights-free and fully deterministic (seeded RNG).
//
// Uses no scene state — a bare bro.json (or none) is fine.

// bro.flora is compile-gated (BRO_WITH_FLORA); the stub sets available:false.
if (bro.flora && bro.flora.available === false) {
    console.log('skip: bro.flora not compiled in (BRO_WITH_FLORA off)');
} else {
    runFloraSmoke();
}

function runFloraSmoke() {

const world = bro.flora.createWorld({
    rngSeed: 0xC0FFEE,
    climate: { annualTempBase: 15, annualPrecip: 1000 },
    shadow:  { origin: [-4, 0, -4], cellSize: 1, width: 8, height: 8, depth: 8, fill: 0.7 },
});

const protoY = world.addPrototype({
    name: 'Y',
    nodes: [
        { position: [ 0, 0, 0] },
        { position: [ 0.3, 1.0, 0], ageAtBirth: 0.2 },
        { position: [-0.3, 1.0, 0], ageAtBirth: 0.2 },
    ],
    edges: [[0, 1], [0, 2]],
    rootNode: 0,
    terminalNodes: [1, 2],
});
world.addVoronoiSite(protoY, 0.2, 0.85);

// Two plants with deliberately different species, so plantInfo can
// distinguish them.
const idxA = world.addPlant({
    origin: [0, 0, 0],
    species: { climateOptT: 8, leafDiameter: 0.05, floweringAge: 8 },
    prototypeIndex: protoY,
});
const idxB = world.addPlant({
    origin: [2, 0, 0],
    species: { climateOptT: 22, leafDiameter: 0.03, floweringAge: 12 },
    prototypeIndex: protoY,
});
assert(idxA === 0 && idxB === 1, 'plants indexed in order');

// Grow a bit so segments / foliage / blooms have content.
for (let i = 0; i < 150; i++) world.step(0.1);

// --- plantInfo ----------------------------------------------------------
const infoA = world.plantInfo(idxA);
const infoB = world.plantInfo(idxB);
console.log('A: age=' + infoA.age.toFixed(2) +
            ' modules=' + infoA.moduleCount +
            ' flowering=' + infoA.flowering +
            ' rootVigor=' + infoA.rootVigor.toFixed(3));
console.log('B: optT=' + infoB.species.climateOptT +
            ' leafDiameter=' + infoB.species.leafDiameter);
assert(infoA.species.climateOptT === 8,    'plantA climateOptT round-trip');
assert(infoB.species.climateOptT === 22,   'plantB climateOptT round-trip');
assert(infoA.moduleCount > 1, 'plantA grew modules');
assert(world.plantInfo(99) === null, 'plantInfo out-of-range → null');

// --- per-plant emit -----------------------------------------------------
const meshA = world.emitPlantMesh(idxA, 6);
assert(meshA && meshA.vertexCount > 0, 'emitPlantMesh produces geometry');
const segsA   = world.emitPlantSegments(idxA);
const segsB   = world.emitPlantSegments(idxB);
const segsAll = world.emitSegments();
// Once a seedling flowers, broflora disperses seeds → new plants. Sum over
// all plants must still match the world-level count.
let perPlantTotal = 0;
for (let i = 0; i < world.plantCount; i++) {
    perPlantTotal += world.emitPlantSegments(i).length;
}
console.log('segments: A=' + segsA.length + ' B=' + segsB.length +
            ' plants=' + world.plantCount +
            ' perPlantTotal=' + perPlantTotal +
            ' all=' + segsAll.length);
assert(perPlantTotal === segsAll.length,
    'per-plant segments sum to world segments across all plants');

const folA = world.emitPlantFoliage(idxA);
assert(folA.length === segsA.length, 'foliage in lockstep with segments per-plant');

const bloomsA = world.emitPlantBloomAnchors(idxA);
console.log('plant A blooms=' + bloomsA.length + ' flowering=' + infoA.flowering);
assert(bloomsA.length === 0 || infoA.flowering, 'blooms only when flowering');

assert(world.emitPlantMesh(-1, 6) === null, 'emitPlantMesh negative → null');
assert(world.emitPlantSegments(99) === null, 'emitPlantSegments OOR → null');

// --- sampleShadow -------------------------------------------------------
// Sample an unoccupied cell (far above the canopy) to read the fill
// value; sample a cell among the plants to confirm self-shadowing has
// happened. Q_G is the light passing through — lower under canopy.
// Must run while the plants are still healthy: the harsh-climate section
// below kills the foliage off, after which nothing casts shadow.
const qOpen   = world.sampleShadow([-3, 7, -3]);
const qCanopy = world.sampleShadow([0,  1,  0]);
const qOut    = world.sampleShadow([100, 100, 100]);
console.log('Q_G open=' + qOpen + ' under-canopy=' + qCanopy + ' out=' + qOut);
// broflora rebuilds the shadow grid each step — `fill` is only the initial
// state. After stepping, open cells return to Q_G ≈ 1 (full sun) while
// occupied cells get shaded by the plants.
assert(qOpen !== null && qOpen > 0.95,  'open cell reads ~full sun after step');
assert(qCanopy !== null && qCanopy < qOpen, 'canopy cell darker than open cell');
assert(qOut === null, 'sampleShadow OOB → null');

// --- setClimate ---------------------------------------------------------
const tBefore = infoA.rootVigor;
world.setClimate({ annualTempBase: -10, annualPrecip: 50 });   // brutal
for (let i = 0; i < 50; i++) world.step(0.1);
const infoA2 = world.plantInfo(idxA);
console.log('A vigor before=' + tBefore.toFixed(3) +
            ' after harsh climate=' + infoA2.rootVigor.toFixed(3));
assert(infoA2.rootVigor < tBefore, 'harsh climate reduces root vigor');

// --- removePlant (swap-and-pop) -----------------------------------------
const countBefore = world.plantCount;
const lastIdx = countBefore - 1;
const lastPlantInfo = world.plantInfo(lastIdx);
// Remove index 0. The plant at lastIdx should move to slot 0.
const ok = world.removePlant(0);
assert(ok === true, 'removePlant(0) ok');
assert(world.plantCount === countBefore - 1, 'plantCount decremented');
const newInfo0 = world.plantInfo(0);
assert(newInfo0.origin[0] === lastPlantInfo.origin[0], 'swap-and-pop: last plant moved to slot 0');
assert(world.plantInfo(lastIdx) === null, 'last slot now empty');
assert(world.removePlant(-1) === false, 'removePlant negative OOR fails');
assert(world.removePlant(999) === false, 'removePlant positive OOR fails');

console.log('flora_bindings_smoke ok');

}  // runFloraSmoke
