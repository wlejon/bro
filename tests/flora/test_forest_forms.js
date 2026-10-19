// A mixed forest: conifers and shrubs from bro.flora's species presets,
// drawn through bro.scene's forest node a node per leaf form (docs/flora-api.js
// "Species presets", docs/scene-nodes-api.js createForest `leafForm`).
// Contract under test:
//   - world.plantSpecies grows the presets; emitForest({leafForm}) splits
//     the world by form and carries each specimen's look;
//   - createForest({forest, leafForm}) with no leaf draws the form's own
//     card: a needle node's leaves are sprays whose needles are cut out of
//     the card (fewer pixels than the same card drawn whole);
//   - each specimen's leaf colour tints its leaves against the node's;
//   - every tier of a needle and a scale-leaf node draws under validation;
//   - several forest nodes share one budget: they thin by one detail factor.

if (bro.flora && bro.flora.available === false) {
    skipTest('bro.flora not compiled in');
} else {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', '240');
    cv.setAttribute('height', '200');
    document.body.appendChild(cv);
    flush();
    const scene = cv.getContext('scene');
    if (!scene) missingGpuContext('scene');
    else run(scene);
}

function covered(img, bg) {
    let n = 0, lum = 0;
    for (let i = 0; i < img.data.length; i += 4) {
        const d = Math.abs(img.data[i] - bg.data[i]) + Math.abs(img.data[i + 1] - bg.data[i + 1]) +
                  Math.abs(img.data[i + 2] - bg.data[i + 2]);
        if (d > 24) { n++; lum += 0.2126 * img.data[i] + 0.7152 * img.data[i + 1] + 0.0722 * img.data[i + 2]; }
    }
    return { n, lum: n ? lum / n : 0 };
}

function run(scene) {
    const world = bro.flora.createWorld({
        rngSeed: 21,
        shadow: { origin: [-16, -2, -16], cellSize: 1, width: 32, height: 30, depth: 32, fill: 1 },
    });
    const names = bro.flora.speciesPresets();
    assert(['fir', 'cypress', 'boxwood'].every((n) => names.indexOf(n) >= 0), 'presets listed: ' + names.join());
    const fir = world.plantSpecies('fir', { origin: [-6, 0, 0] });
    const cypress = world.plantSpecies('cypress', { origin: [0, 0, 0] });
    const box = world.plantSpecies('boxwood', { origin: [6, 0, 0] });
    assert(fir === 0 && cypress === 1 && box === 2, 'planted in order');
    world.grow(900, 0.02);

    const needle = world.emitForest({ leafForm: 'needle', seed: 5 });
    const scale = world.emitForest({ leafForm: 'scale', seed: 5 });
    const broad = world.emitForest({ leafForm: 'broad', seed: 5 });
    assert(needle.specimenCount === 1 && scale.specimenCount === 1 && broad.specimenCount === 1,
           'one specimen a form: ' + [needle.specimenCount, scale.specimenCount, broad.specimenCount]);
    assert(needle.leafForm === 'needle' && needle.specimenLooks.length === 8 && needle.specimenLooks[3] === 1,
           'the emit names its form and each specimen its look');
    const firInfo = world.plantInfo(fir);
    assert(Math.abs(needle.specimenLooks[0] - firInfo.species.leafColor[0]) < 1e-6, 'the look is the species colour');
    const firHeight = needle.specimens[6];
    assert(firHeight > 4, 'the fir grew a tree: ' + firHeight.toFixed(2) + ' m');

    scene.setWind({ speed: 0, gust: 0 });
    scene.setCamera({ fov: 45, near: 0.1, far: 500, position: [0, 5, 22], target: [0, 4, 0] });
    const bg = scene.captureFrame();

    // ── A needle node, its own spray card ────────────────────────────────
    const color = [0.3, 0.5, 0.2];
    const nn = scene.createForest({ forest: needle, leafForm: 'needle', color, lod: { budget: 0, shadowBudget: 0, leavesRange: 0 } });
    assert(nn && nn.forestLod, 'a needle forest');
    for (const t of ['leaves', 'clumps', 'crowns']) {
        nn.setForestTier(t);
        const c = covered(scene.captureFrame(), bg);
        assert(c.n > 300, 'needle ' + t + ' draws: ' + c.n + ' px');
    }
    nn.setForestTier('leaves');
    const sprays = covered(scene.captureFrame(), bg);
    // The same card drawn whole (a broad form): the needles cover less.
    nn.setForest({ leafForm: 'broad' });
    const whole = covered(scene.captureFrame(), bg);
    assert(sprays.n < whole.n * 0.92, 'the needles are cut out of the card: ' + sprays.n + ' vs ' + whole.n);
    nn.setForest({ leafForm: 'needle' });

    // ── Each specimen's colour ───────────────────────────────────────────
    // The fir's leaves are darker than the node's colour: drawn with its
    // look they are darker than drawn with none.
    const own = covered(scene.captureFrame(), bg);
    nn.setForest({ specimenLooks: new Float32Array(8) });
    const plain = covered(scene.captureFrame(), bg);
    assert(own.lum < plain.lum - 4, 'the specimen colour tints the leaves: ' + own.lum.toFixed(1) + ' vs ' + plain.lum.toFixed(1));
    nn.setForest({ specimenLooks: needle.specimenLooks });
    nn.setForestTier('auto');

    // ── A scale-leaf node and a shrub node, every tier ───────────────────
    const sn = scene.createForest({ forest: scale, color, lod: { budget: 0, shadowBudget: 0, leavesRange: 0 } });
    const bn = scene.createForest({ forest: broad, color, lod: { budget: 0, shadowBudget: 0, leavesRange: 0 } });
    nn.visible = false;
    for (const [name, node] of [['scale', sn], ['shrub', bn]]) {
        const other = node === sn ? bn : sn;
        other.visible = false;
        for (const t of ['leaves', 'clumps', 'crowns']) {
            node.setForestTier(t);
            const c = covered(scene.captureFrame(), bg);
            // The boxwood is a metre and a quarter high here, a dozen pixels
            // a metre: its clumps are a few pixels each.
            assert(c.n > 40, name + ' ' + t + ' draws: ' + c.n + ' px');
        }
        node.setForestTier('auto');
        other.visible = true;
    }
    nn.visible = true;

    // ── One budget for them all ──────────────────────────────────────────
    scene.setCamera({ fov: 45, near: 0.1, far: 500, position: [0, 3, 9], target: [0, 3, 0] });
    scene.captureFrame();
    const free = [nn, sn, bn].map((n) => n.forestStats());
    const sum = free.reduce((a, s) => a + s.vertices, 0);
    for (const n of [nn, sn, bn]) n.setForestLod({ budget: sum * 0.3e-6 });
    scene.captureFrame();
    const tight = [nn, sn, bn].map((n) => n.forestStats());
    const tightSum = tight.reduce((a, s) => a + s.vertices, 0);
    assert(tight[0].detail > 1 && tight.every((s) => s.detail === tight[0].detail),
           'the nodes thin by one detail factor: ' + tight.map((s) => s.detail.toFixed(3)).join(' / '));
    assert(tightSum < sum * 0.45, 'and fit the budget together: ' + tightSum + ' of ' + sum);
    for (const n of [nn, sn, bn]) n.destroy();
}
