// Vegetation sway: bro.flora's emitters write the per-vertex / per-segment
// plant records (docs/flora-api.js "Vegetation records") and bro.scene bends
// them in its wind field (docs/vegetation-api.js "Vegetation records"), with no
// wind code in the app.
//
// Contract under test:
//   - every emitter carries records of the documented size: emitMesh /
//     emitFoliageMesh per vertex (16 floats), emitBranchTubes /
//     emitScatterSegments per segment (16), emitFoliageVegetation per leaf;
//   - wood carries no stem axis (word 12 is 0), leaves do; every record
//     carries its plant's trunk cantilever (word 15);
//   - scene.windAt is the field the shaders evaluate: zero in a calm, the
//     set speed and direction with gusts off, gust bands that travel
//     downwind at gustSpeed;
//   - each node kind (mesh, instanced, tubes, scatter) draws with its records
//     (under validation), stands still in a calm and moves in a wind;
//   - swaying casters keep their shadow tiles dynamic; a calm lets them cache.

function diffCount(a, b, tol) {
    if (a.width !== b.width || a.height !== b.height) return a.width * a.height;
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        if (Math.abs(a.data[i] - b.data[i]) > tol || Math.abs(a.data[i + 1] - b.data[i + 1]) > tol ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > tol) n++;
    }
    return n;
}

function growTree() {
    const world = bro.flora.createWorld({
        rngSeed: 7,
        shadow: { origin: [-8, 0, -8], cellSize: 1, width: 16, height: 16, depth: 16, fill: 1 },
    });
    const mono = world.addPrototype(bro.flora.prototypes.monopodial(3, 0.7));
    world.addVoronoiSite(mono, 0.5, 0.3);
    world.addPlant({ origin: [0, 0, 0], prototypeIndex: mono,
                     species: { growthScale: 2.0, maxAge: 400, apicalControl: 0.4, seedingRadius: 0 } });
    for (let i = 0; i < 600; i++) world.step(0.05);
    return world;
}

const FOLIAGE = { maxRadius: 0.05, minDepth: 1, perUnitLength: 60, seed: 3 };

function leaf() {
    const m = Mesh.leafCard('oval', { width: 0.12, length: 0.2 });
    m.colors = new Float32Array(0);
    return m;
}

if (bro.flora && bro.flora.available === false) {
    skipTest('bro.flora not compiled in');
} else {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', '160');
    cv.setAttribute('height', '160');
    document.body.appendChild(cv);
    flush();
    const scene = cv.getContext('scene');
    if (!scene) {
        missingGpuContext('scene');
    } else {
        run(scene);
    }
}

function run(scene) {
    // ── The records ──────────────────────────────────────────────────────
    const world = growTree();
    const bark = world.emitMesh(6);
    assert(bark.vertexCount > 0, 'the tree grew bark');
    assert(bark.vegetation.length === bark.vertexCount * 16,
           'emitMesh writes 16 floats a vertex: ' + bark.vegetation.length + ' for ' + bark.vertexCount);
    const leaves = world.emitFoliageMesh(leaf(), FOLIAGE);
    assert(leaves.vertexCount > 0, 'the tree has leaves');
    assert(leaves.vegetation.length === leaves.vertexCount * 16, 'emitFoliageMesh writes 16 floats a vertex');
    const tubes = world.emitBranchTubes();
    assert(tubes.vegetation.length === tubes.segCount * 16, 'emitBranchTubes writes 16 floats a segment');
    const scatter = world.emitScatterSegments(FOLIAGE);
    assert(scatter.vegetation.length === scatter.segCount * 16, 'emitScatterSegments writes 16 floats a segment');
    const perLeaf = world.emitFoliageVegetation(FOLIAGE);
    const xforms = world.emitFoliageTransforms(FOLIAGE);
    assert(perLeaf.length === xforms.length, 'emitFoliageVegetation: one 16-float record per leaf transform');

    // Bark is wood (no stem axis word), leaves are not; every record carries
    // its trunk; the branch weight runs 0..1.
    // (Each read of a mesh's `vegetation` copies it out: read it once.)
    let wood = 0, leafy = 0, trunks = 0, wMin = 1, wMax = 0;
    const bv = bark.vegetation, lv = leaves.vegetation;
    for (let i = 0; i < bark.vertexCount; i++) {
        if (bv[i * 16 + 12] === 0) wood++;
        if (bv[i * 16 + 15] > 0) trunks++;
        wMin = Math.min(wMin, bv[i * 16 + 11]);
        wMax = Math.max(wMax, bv[i * 16 + 11]);
    }
    for (let i = 0; i < leaves.vertexCount; i++) {
        if (lv[i * 16 + 12] > 0 && Number.isInteger(lv[i * 16 + 12])) leafy++;
    }
    assert(wood === bark.vertexCount, 'every bark vertex is wood');
    assert(trunks === bark.vertexCount, 'every bark vertex carries its trunk');
    assert(leafy === leaves.vertexCount, 'every leaf vertex has a stem axis');
    assert(wMin === 0 && wMax > 0.9 && wMax <= 1, 'branch weight spans trunk to tip: ' + wMin + '..' + wMax);
    assert(bark.vegetation[3] > 1, 'the plant height is recorded: ' + bark.vegetation[3]);

    // ── The field ────────────────────────────────────────────────────────
    scene.setWind({ direction: [1, 0, 0], speed: 0, gust: 0 });
    let w = scene.windAt(3, 0, 4);
    assert(w.speed === 0 && w.gust === 0, 'a calm field is still');
    scene.setWind({ speed: 3, gust: 0, drift: 0 });
    w = scene.windAt([3, 0, 4]);
    assert(w.gust === 0, 'no gust with gust 0');
    assert(w.velocity[0] > 0 && Math.abs(w.velocity[1]) < 1e-6, 'blows along +x: ' + w.velocity);
    assert(w.speed > 3 * 0.7 && w.speed < 3 * 1.3 * 1.1, 'about the steady speed: ' + w.speed);
    const state = scene.wind();
    assert(state.speed === 3 && state.gust === 0 && state.drift === 0, 'wind() reports the field set');
    assert(state.strength === 0, 'the legacy strength is left alone');

    // Gust bands travel downwind: the gust at x now is the gust at x + speed * dt later.
    scene.setWind({ speed: 2, gust: 1, gustSpeed: 10, gustScale: 20, drift: 0 });
    let matched = 0, nonzero = 0;
    const before = [];
    const t0 = scene.wind().time;
    for (let i = 0; i < 40; i++) before.push(scene.windAt(i * 3, 0, 5).gust);
    advanceTime(500);
    const t1 = scene.wind().time;
    assert(t1 > t0, 'the wind clock runs on virtual time');
    const shift = 10 * (t1 - t0);
    for (let i = 0; i < 40; i++) {
        const g = scene.windAt(i * 3 + shift, 0, 5).gust;
        if (before[i] > 0) nonzero++;
        if (Math.abs(g - before[i]) < 0.02) matched++;
    }
    assert(nonzero > 0, 'some gust bands over the line');
    assert(matched >= 36, 'gust bands advect downwind at gustSpeed: ' + matched + '/40 matched');

    // ── The draw ─────────────────────────────────────────────────────────
    scene.setCamera({ fov: 45, near: 0.1, far: 100, position: [0, 4, 14], target: [0, 3, 0] });
    scene.createMesh({ mesh: 'plane', halfW: 10, halfD: 10, color: '#556633', castsShadow: false });
    const sun = scene.createLight({ type: 'directional', direction: [-0.4, -1, -0.3], intensity: 3 });
    sun.castsShadow = true;

    const kinds = {
        mesh: () => [scene.createMesh({ data: bark, color: '#5a4636', castsShadow: true }),
                     scene.createMesh({ data: leaves, color: '#4a7a2a', twoSided: true, subsurface: 0.4,
                                        castsShadow: true })],
        gpu: () => [scene.createInstancedMesh({ tube: tubes, color: '#5a4636', castsShadow: true }),
                    scene.createInstancedMesh({ mesh: leaf(), scatter: Object.assign({}, FOLIAGE, scatter),
                                                doubleSided: true, color: '#4a7a2a', castsShadow: true })],
        // emitFoliageTransforms rows are already the instance layout.
        instanced: () => [scene.createInstancedMesh({ mesh: leaf(), instances: xforms, vegetation: perLeaf,
                                                      doubleSided: true, color: '#4a7a2a', castsShadow: true })],
    };
    for (const [name, make] of Object.entries(kinds)) {
        const nodes = make();
        scene.setWind({ speed: 0, gust: 0 });
        scene.captureFrame();
        const c0 = scene.captureFrame();
        advanceTime(700);
        const c1 = scene.captureFrame();
        const st = scene.cullStats();
        assert(diffCount(c0, c1, 0) === 0, name + ': still in a calm');
        assert(st.shadowTilesCached > 0, name + ': a calm lets the shadow tiles cache');

        scene.setWind({ speed: 5, gust: 1.5, gustScale: 15 });
        const m0 = scene.captureFrame();
        advanceTime(700);
        const m1 = scene.captureFrame();
        const moving = diffCount(m0, m1, 2);
        assert(moving > 10, name + ': sways in a wind (' + moving + ' px changed)');
        assert(scene.cullStats().shadowTilesCached === 0, name + ': swaying casters keep the shadow tiles dynamic');
        // The sun's last cascade (of three or more) is drawn every other
        // frame, so its far shadows lag a frame's sway on the frame between
        // (shadowTilesHeld); every nearer cascade is drawn every frame.
        let wasHeld = false, heldFrames = 0;
        for (let i = 0; i < 6; i++) {
            scene.captureFrame();
            const st = scene.cullStats();
            assert(st.shadowTilesCached === 0, name + ': a swaying frame caches no tile');
            assert(st.shadowTilesHeld <= (st.shadowTilesTotal >= 3 ? 1 : 0) &&
                   st.shadowTilesRendered + st.shadowTilesHeld === st.shadowTilesTotal,
                name + `: only the last cascade is held (${st.shadowTilesRendered} drawn, ${st.shadowTilesHeld} held)`);
            assert(!st.shadowTilesHeld || st.shadowTilesHeldMask === 1 << (st.shadowTilesTotal - 1),
                name + `: the held tile is the last cascade (mask ${st.shadowTilesHeldMask})`);
            assert(!(wasHeld && st.shadowTilesHeld), name + ': the last cascade is drawn at least every other frame');
            wasHeld = st.shadowTilesHeld > 0;
            if (wasHeld) heldFrames++;
        }
        assert(heldFrames === 3 || scene.cullStats().shadowTilesTotal < 3, name + `: held every other frame (${heldFrames} of 6)`);
        for (const n of nodes) n.destroy();
    }
}
