// Test bro.flora wind, density, and batch/placement APIs.

if (bro.flora && bro.flora.available === false) {
    console.log('skip: bro.flora not compiled in');
} else {
    runFloraWindDensityBatchTest();
}

function runFloraWindDensityBatchTest() {
    console.log('Testing bro.flora wind, density, and batch APIs...');

    // ── 1. Wind API ──────────────────────────────────────────────────────────
    console.log('Testing wind API...');
    const initialWind = bro.flora.wind();
    assert(initialWind && typeof initialWind === 'object', 'wind() returns object');
    assert(typeof initialWind.strength === 'number', 'wind.strength is number');
    assert(typeof initialWind.dirX === 'number', 'wind.dirX is number');
    assert(typeof initialWind.dirY === 'number', 'wind.dirY is number');

    bro.flora.setWind(2.5, 0.8, 0.6);
    const w1 = bro.flora.wind();
    assert(Math.abs(w1.strength - 2.5) < 1e-4, 'setWind sets strength: ' + w1.strength);
    assert(Math.abs(w1.dirX - 0.8) < 1e-4, 'setWind sets dirX: ' + w1.dirX);
    assert(Math.abs(w1.dirY - 0.6) < 1e-4, 'setWind sets dirY: ' + w1.dirY);

    // Test wind([strength, dirX, dirY]) setter behavior
    bro.flora.wind(4.0, -1.0, 0.0);
    const w2 = bro.flora.wind();
    assert(Math.abs(w2.strength - 4.0) < 1e-4, 'wind(args) sets strength');
    assert(Math.abs(w2.dirX - (-1.0)) < 1e-4, 'wind(args) sets dirX');
    assert(Math.abs(w2.dirY - 0.0) < 1e-4, 'wind(args) sets dirY');

    // ── 2. Density API ───────────────────────────────────────────────────────
    console.log('Testing density API...');
    bro.flora.setDensity(0.65);
    const d1 = bro.flora.density();
    assert(Math.abs(d1 - 0.65) < 1e-4, 'setDensity sets density: ' + d1);

    // Test density(val) setter behavior
    bro.flora.density(1.4);
    const d2 = bro.flora.density();
    assert(Math.abs(d2 - 1.4) < 1e-4, 'density(val) sets density: ' + d2);

    // ── 3. Batches & Placement API ───────────────────────────────────────────
    console.log('Testing batches and placement API...');
    // Clear any previous state
    bro.flora.clear();

    const b0 = bro.flora.batches();
    const gb0 = bro.flora.getBatches();
    assert(Array.isArray(b0), 'batches() returns array');
    assert(Array.isArray(gb0), 'getBatches() returns array');
    assert(b0.length === 0, 'initial batches length is 0');
    assert(gb0.length === 0, 'initial getBatches length is 0');

    // Add placement with count
    const batch1 = bro.flora.addPlacement({
        count: 5,
        radius: 10,
    });
    assert(batch1 !== null && typeof batch1 === 'object', 'addPlacement returns batch');
    assert(batch1.instanceCount === 5, 'batch instanceCount is 5');
    assert(batch1.transforms instanceof Float32Array, 'batch transforms is Float32Array');
    assert(batch1.transforms.length === 5 * 16, 'batch transforms length is 5 * 16');

    let currentBatches = bro.flora.batches();
    assert(currentBatches.length === 1, 'batches length is 1 after addPlacement');
    assert(bro.flora.getBatches().length === 1, 'getBatches length is 1 after addPlacement');
    assert(currentBatches[0] === batch1, 'batch in list matches returned batch');

    // Add placement with position triples
    const batchPos = bro.flora.addPlacement({
        transforms: [[0, 0, 0], [10, 2, 5], [20, 0, -10]],
    });
    assert(batchPos.instanceCount === 3, 'position triples instanceCount is 3');
    assert(batchPos.transforms.length === 3 * 16, 'position triples transforms length is 48');
    assert(Math.abs(batchPos.transforms[16 + 3] - 10) < 1e-4, 'triple 1 posX mapped');
    assert(Math.abs(batchPos.transforms[16 + 7] - 2) < 1e-4, 'triple 1 posY mapped');
    assert(Math.abs(batchPos.transforms[16 + 11] - 5) < 1e-4, 'triple 1 posZ mapped');

    // Add placement with explicit transforms
    // Row-major 3x4 affine (translation in floats 3/7/11) plus an RGBA tint
    // in floats 12-15, the layout docs/flora-api.js gives for placement
    // batches.
    const customTransforms = new Float32Array([
        1, 0, 0, 10,
        0, 1, 0, 0,
        0, 0, 1, 5,
        1, 1, 1, 1,

        1, 0, 0, 20,
        0, 1, 0, 0,
        0, 0, 1, 15,
        1, 1, 1, 1,
    ]);
    const batch2 = bro.flora.placement({
        transforms: customTransforms,
        castShadow: true,
        receiveShadow: true,
    });
    assert(batch2.instanceCount === 2, 'batch2 instanceCount is 2');
    assert(batch2.castShadow === true, 'batch2 castShadow is true');
    assert(batch2.receiveShadow === true, 'batch2 receiveShadow is true');

    currentBatches = bro.flora.batches();
    assert(currentBatches.length === 3, 'batches length is 3');

    // ── 4. Wind Update & Animation ───────────────────────────────────────────
    console.log('Testing wind simulation update...');
    bro.flora.setWind(3.0, 1.0, 0.0);

    // Step simulation
    bro.flora.update(0.2);

    // After wind update, instance transforms should be updated with tilt/sway
    const currentBatchesAfterUpdate = bro.flora.batches();
    assert(currentBatchesAfterUpdate.length === 3, 'batches count preserved after update');
    
    // Check that transforms updated
    const updatedBatch2 = currentBatchesAfterUpdate[2];
    assert(updatedBatch2.transforms instanceof Float32Array, 'transforms still Float32Array');
    let hasChanged = false;
    for (let i = 0; i < updatedBatch2.transforms.length; ++i) {
        if (Math.abs(updatedBatch2.transforms[i] - customTransforms[i]) > 1e-4) {
            hasChanged = true;
            break;
        }
    }
    assert(hasChanged, 'wind update modifies instance transforms with sway');

    // Further step
    bro.flora.update(0.3);

    // ── 5. Integration with World Simulation & Mesh Emit ─────────────────────
    console.log('Testing wind & density integration with world mesh emit...');
    const world = bro.flora.createWorld({
        rngSeed: 12345,
        climate: { annualTempBase: 20, annualPrecip: 1200 },
        shadow:  { origin: [-5, 0, -5], cellSize: 1, width: 10, height: 10, depth: 10, fill: 1.0 },
    });
    const protoStraight = world.addPrototype(bro.flora.prototypes.monopodial());
    world.addPlant({
        origin: [0, 0, 0],
        species: { apicalControl: 0.85, determinacy: 0.2, shadeTolerance: 0.6, climateOptT: 20, floweringAge: 8 },
        prototypeIndex: protoStraight,
    });
    for (let i = 0; i < 150; i++) world.step(0.1);

    // Emit mesh with wind = 0
    bro.flora.setWind(0.0, 1.0, 0.0);
    const calmMesh = world.emitMesh(6);
    assert(calmMesh && calmMesh.vertexCount > 0, 'calmMesh emitted with vertices: ' + (calmMesh ? calmMesh.vertexCount : 0));

    // Emit mesh with strong wind
    bro.flora.setWind(8.0, 1.0, 0.0);
    const windyMesh = world.emitMesh(6);
    assert(windyMesh && windyMesh.vertexCount === calmMesh.vertexCount, 'windyMesh emitted with matching vertexCount');

    // Foliage transforms with wind
    bro.flora.setWind(0.0, 1.0, 0.0);
    const calmFoliage = world.emitFoliageTransforms({ minDepth: 0, perUnitLength: 2.0 });
    assert(calmFoliage instanceof Float32Array && calmFoliage.length > 0, 'calm foliage transforms emitted');

    bro.flora.setWind(6.0, 1.0, 0.0);
    const windyFoliage = world.emitFoliageTransforms({ minDepth: 0, perUnitLength: 2.0 });
    assert(windyFoliage instanceof Float32Array && windyFoliage.length === calmFoliage.length, 'windy foliage transforms length matches');

    let foliageDisplaced = false;
    for (let i = 0; i < calmFoliage.length; ++i) {
        if (Math.abs(windyFoliage[i] - calmFoliage[i]) > 1e-4) {
            foliageDisplaced = true;
            break;
        }
    }
    assert(foliageDisplaced, 'wind displaces emitted foliage transforms');

    // Test density scaling foliage emit
    bro.flora.setWind(0.0, 1.0, 0.0);
    bro.flora.setDensity(1.0);
    const tfNormal = world.emitFoliageTransforms({ minDepth: 0, perUnitLength: 2.0 });
    bro.flora.setDensity(2.5);
    const tfDense = world.emitFoliageTransforms({ minDepth: 0, perUnitLength: 2.0 });
    assert(tfNormal && tfDense, 'foliage transforms emitted');
    assert(tfDense.length >= tfNormal.length, 'density=2.5 emits >= foliage transforms than density=1.0 (' + tfDense.length + ' vs ' + tfNormal.length + ')');

    // ── 6. Clear API ─────────────────────────────────────────────────────────
    console.log('Testing clear API...');
    bro.flora.clear();
    assert(bro.flora.batches().length === 0, 'batches cleared after clear()');
    assert(bro.flora.getBatches().length === 0, 'getBatches cleared after clear()');
    const resetWind = bro.flora.wind();
    assert(resetWind.strength === 0, 'wind strength reset to 0 after clear()');
    assert(bro.flora.density() === 1.0, 'density reset to 1.0 after clear()');

    console.log('flora_wind_density_batches ok');
}
