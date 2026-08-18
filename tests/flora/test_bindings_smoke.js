// Exercise the prototype factories, world simulation, per-plant emit,
// plantInfo, setClimate, and sampleShadow bindings of bro.flora.
// Weights-free and fully deterministic (seeded RNG).
//
// Uses no scene state — a bare bro.json (or none) is fine.

// bro.flora is compile-gated (BRO_WITH_FLORA); the stub sets available:false.
if (bro.flora && bro.flora.available === false) {
    console.log('skip: bro.flora not compiled in (BRO_WITH_FLORA off)');
} else {
    runFloraSmoke();
}

function runFloraSmoke() {

// ── 1. Test Prototype Factories ─────────────────────────────────────────
console.log('Testing prototype factories...');

function verifyProtoSpec(spec, expectedName, expectedMinNodes, expectedTerms) {
    assert(spec && typeof spec === 'object', expectedName + ' spec is object');
    assert(spec.name === expectedName, expectedName + ' name matches: ' + spec.name);
    assert(Array.isArray(spec.nodes) && spec.nodes.length >= expectedMinNodes,
           expectedName + ' has valid nodes (' + spec.nodes.length + ')');
    assert(Array.isArray(spec.edges) && spec.edges.length >= expectedMinNodes - 1,
           expectedName + ' has valid edges (' + spec.edges.length + ')');
    assert(typeof spec.rootNode === 'number', expectedName + ' has rootNode');
    assert(Array.isArray(spec.terminalNodes) && spec.terminalNodes.length === expectedTerms,
           expectedName + ' terminalNodes count=' + spec.terminalNodes.length + ' expected=' + expectedTerms);
    for (let i = 0; i < spec.nodes.length; ++i) {
        const nd = spec.nodes[i];
        assert(Array.isArray(nd.position) && nd.position.length === 3, 'node ' + i + ' has valid position');
        assert(typeof nd.ageAtBirth === 'number', 'node ' + i + ' has ageAtBirth');
        assert(typeof nd.lengthMax === 'number', 'node ' + i + ' has lengthMax');
        assert(typeof nd.thickening === 'number', 'node ' + i + ' has thickening');
    }
}

const specStraight = bro.flora.prototypes.straight();
verifyProtoSpec(specStraight, 'straight', 2, 1);

const specFork = bro.flora.prototypes.fork();
verifyProtoSpec(specFork, 'fork', 3, 2);

const specWhorl3 = bro.flora.prototypes.whorl();
verifyProtoSpec(specWhorl3, 'whorl', 4, 3);

const specWhorl4 = bro.flora.prototypes.whorl(4, 0.7);
verifyProtoSpec(specWhorl4, 'whorl', 5, 4);

// Monopodial leader prototype (apical tip + lateral branches)
const specMonoDefault = bro.flora.prototypes.monopodial();
verifyProtoSpec(specMonoDefault, 'monopodial', 5, 3); // 1 apical tip + 2 laterals = 3 terminals

const specMono3 = bro.flora.prototypes.monopodial(3, 0.65);
verifyProtoSpec(specMono3, 'monopodial', 6, 4); // 1 apical tip + 3 laterals = 4 terminals

// Sympodial fork prototype (dominant primary + secondary)
const specSymDefault = bro.flora.prototypes.sympodial();
verifyProtoSpec(specSymDefault, 'sympodial', 4, 2);

const specSymCustom = bro.flora.prototypes.sympodial(0.25, 0.75);
verifyProtoSpec(specSymCustom, 'sympodial', 4, 2);

// Horizontal tier prototype (shelf arms)
const specTierDefault = bro.flora.prototypes.horizontalTier();
verifyProtoSpec(specTierDefault, 'tier', 5, 3);

const specTierAlias = bro.flora.prototypes.tier(4, 0.85);
verifyProtoSpec(specTierAlias, 'tier', 6, 4);

// Weeping prototype (pendulous droop shoot)
const specWeepDefault = bro.flora.prototypes.weeping();
verifyProtoSpec(specWeepDefault, 'weeping', 4, 2);

const specWeepCustom = bro.flora.prototypes.weeping(0.7, 0.5);
verifyProtoSpec(specWeepCustom, 'weeping', 4, 2);

console.log('All prototype factories verified successfully');

// ── 2. World Creation & Prototype Registration ───────────────────────────
const world = bro.flora.createWorld({
    rngSeed: 0xC0FFEE,
    climate: { annualTempBase: 15, annualPrecip: 1000 },
    shadow:  { origin: [-6, 0, -6], cellSize: 1, width: 12, height: 12, depth: 12, fill: 0.8 },
});

assert(world.prototypeCount === 0, 'initial prototype count is 0');
assert(world.plantCount === 0, 'initial plant count is 0');

const protoStraightIdx = world.addPrototype(specStraight);
const protoForkIdx     = world.addPrototype(specFork);
const protoWhorlIdx    = world.addPrototype(specWhorl4);
const protoMonoIdx     = world.addPrototype(specMono3);
const protoSymIdx      = world.addPrototype(specSymCustom);
const protoTierIdx     = world.addPrototype(specTierAlias);
const protoWeepIdx     = world.addPrototype(specWeepCustom);

assert(world.prototypeCount === 7, 'all 7 prototypes registered, count=' + world.prototypeCount);

// Voronoi sites for developmental crown morphospace (determinacy D, apicalControl λ)
world.addVoronoiSite(protoMonoIdx, 0.1, 0.90); // excurrent / conifer
world.addVoronoiSite(protoSymIdx,  0.7, 0.40); // decurrent / spreading
world.addVoronoiSite(protoTierIdx, 0.4, 0.70); // tiered
world.addVoronoiSite(protoWeepIdx, 0.9, 0.20); // weeping

// ── 3. Planting Seedlings with New Prototypes ─────────────────────────────
// Plant seedlings representing different architectural growth models
const idxConifer = world.addPlant({
    origin: [-2, 0, -2],
    species: { apicalControl: 0.85, determinacy: 0.2, shadeTolerance: 0.6, climateOptT: 10, floweringAge: 8 },
    prototypeIndex: protoMonoIdx,
});

const idxOak = world.addPlant({
    origin: [2, 0, -2],
    species: { apicalControl: 0.35, determinacy: 0.7, shadeTolerance: 0.4, climateOptT: 18, floweringAge: 10 },
    prototypeIndex: protoSymIdx,
});

const idxPine = world.addPlant({
    origin: [-2, 0, 2],
    species: { apicalControl: 0.75, determinacy: 0.4, shadeTolerance: 0.5, climateOptT: 14, floweringAge: 9 },
    prototypeIndex: protoTierIdx,
});

const idxWillow = world.addPlant({
    origin: [2, 0, 2],
    species: { apicalControl: 0.25, determinacy: 0.85, shadeTolerance: 0.7, climateOptT: 16, floweringAge: 7 },
    prototypeIndex: protoWeepIdx,
});

assert(world.plantCount === 4, '4 seedlings planted');
assert(world.validate() === null, 'world validation passes after planting');

// Grow seedlings through development steps
console.log('Stepping simulation...');
for (let i = 0; i < 150; i++) world.step(0.1);

assert(world.validate() === null, 'world validation passes after growth');
assert(world.simTime > 14.9, 'simTime advanced correctly: ' + world.simTime.toFixed(2));
assert(world.moduleCount >= 4, 'modules grew across plants: ' + world.moduleCount);

// ── 4. Inspect Plants & Emit Geometry ────────────────────────────────────
for (let i = 0; i < world.plantCount; i++) {
    const info = world.plantInfo(i);
    assert(info !== null, 'plantInfo(' + i + ') valid');
    assert(info.moduleCount > 0, 'plant ' + i + ' has modules');
    console.log('Plant ' + i + ': age=' + info.age.toFixed(1) +
                ' modules=' + info.moduleCount +
                ' flowering=' + info.flowering +
                ' rootVigor=' + info.rootVigor.toFixed(3));

    const pMesh = world.emitPlantMesh(i, 6);
    assert(pMesh && pMesh.vertexCount > 0, 'emitPlantMesh(' + i + ') produces vertices');

    const pSegs = world.emitPlantSegments(i);
    assert(pSegs && pSegs.length > 0, 'emitPlantSegments(' + i + ') produces segments');

    const pFol = world.emitPlantFoliage(i);
    assert(pFol && pFol.length === pSegs.length, 'emitPlantFoliage(' + i + ') matches segments length');
}

// World-level emissions
const worldMesh = world.emitMesh(6);
assert(worldMesh && worldMesh.vertexCount > 0, 'world emitMesh produces mesh');

const worldSegs = world.emitSegments();
const worldFol  = world.emitFoliage();
assert(worldSegs.length === worldFol.length, 'world segments and foliage match length');
assert(worldSegs.length > 0, 'world segments non-empty');

// Fast native transform buffers
const leafTransforms = world.emitFoliageTransforms({ minDepth: 0 });
assert(leafTransforms instanceof Float32Array, 'emitFoliageTransforms returns Float32Array');

const segTransforms = world.emitSegmentTransforms();
assert(segTransforms instanceof Float32Array, 'emitSegmentTransforms returns Float32Array');
assert(segTransforms.length === worldSegs.length * 16, 'segTransforms has 16 floats per segment');

const scatterSegs = world.emitScatterSegments({ minDepth: 0 });
assert(scatterSegs && scatterSegs.segments instanceof Float32Array, 'emitScatterSegments returns segments buffer');
assert(scatterSegs.segCount >= 0, 'scatterSegs has valid segCount');

const branchTubes = world.emitBranchTubes();
assert(branchTubes && branchTubes.segments instanceof Float32Array, 'emitBranchTubes returns tube buffer');
assert(branchTubes.segCount > 0 && branchTubes.segCount <= worldSegs.length, 'branchTubes segCount valid');

// ── 5. Shadow Grid & Climate Succession ─────────────────────────────────
const qOpen   = world.sampleShadow([-5, 11, -5]); // High up in open corner
const qCanopy = world.sampleShadow([-2, 0, -2]);  // Ground level under the conifer canopy
const qOut    = world.sampleShadow([100, 100, 100]); // Out of bounds

console.log('Q_G open=' + qOpen + ' under-canopy=' + qCanopy + ' out=' + qOut);
assert(qOpen !== null && qOpen > 0.95, 'open cell reads ~full sun after step');
assert(qCanopy !== null && qCanopy < qOpen, 'under-canopy cell darker than open sky');
assert(qOut === null, 'sampleShadow OOB returns null');

// Climate test
const info0Before = world.plantInfo(0);
const vigorBefore = info0Before.rootVigor;
world.setClimate({ annualTempBase: -15, annualPrecip: 30 }); // Extreme harsh cold & drought
for (let i = 0; i < 50; i++) world.step(0.1);
const info0After = world.plantInfo(0);
const vigorAfter = (info0After && typeof info0After.rootVigor === 'number') ? info0After.rootVigor : 0;
console.log('Plant 0 vigor before=' + vigorBefore.toFixed(3) + ' after harsh climate=' + vigorAfter.toFixed(3));
assert(vigorAfter < vigorBefore, 'harsh climate reduces root vigor');

// ── 6. Remove Plant (Swap-and-Pop) ──────────────────────────────────────
const countBefore = world.plantCount;
const lastIdx = countBefore - 1;
const lastPlantInfo = world.plantInfo(lastIdx);
const ok = world.removePlant(0);
assert(ok === true, 'removePlant(0) succeeded');
assert(world.plantCount === countBefore - 1, 'plantCount decremented');
const newInfo0 = world.plantInfo(0);
assert(newInfo0.origin[0] === lastPlantInfo.origin[0], 'swap-and-pop: last plant moved to slot 0');
assert(world.plantInfo(lastIdx) === null, 'vacated slot is null');
assert(world.removePlant(-1) === false, 'removePlant negative OOR fails');
assert(world.removePlant(999) === false, 'removePlant positive OOR fails');

console.log('flora_bindings_smoke ok');

} // runFloraSmoke
