// Blooms: a flowering shrub's flowers, heads or spikes, from bro.flora's
// emitForest through bro.scene's forest node (docs/flora-api.js emitForest
// `blooms`, docs/scene-nodes-api.js "Forest" BLOOMS).
// Contract under test:
//   - grown past their flowering age, a hydrangea, a hibiscus and a lavender
//     carry blooms in the emit (a head, a flower, a spike), each on its own
//     specimen's segments; a fir carries none;
//   - the forest node draws them up close (forestStats().blooms), none for
//     the fir's node, and none once the shrubs are far enough for every
//     bloom to be under a fraction of a pixel;
//   - a bloom's pixels take its species' hue: blue-violet heads, red
//     flowers, purple spikes, where the same view without the blooms has
//     none of it;
//   - every form draws under validation.

// Pixels of `img` within `r` of (x, y) that pass `hue`.
function hued(img, x, y, r, hue) {
    let n = 0;
    for (let j = Math.max(0, Math.round(y) - r); j <= Math.min(img.height - 1, Math.round(y) + r); j++) {
        for (let i = Math.max(0, Math.round(x) - r); i <= Math.min(img.width - 1, Math.round(x) + r); i++) {
            const k = (j * img.width + i) * 4;
            if (hue(img.data[k], img.data[k + 1], img.data[k + 2])) n++;
        }
    }
    return n;
}

const HUES = {
    // A hydrangea's heads: blue-violet.
    2: (r, g, b) => b > 30 && b > 1.15 * g && b > 1.05 * r,
    // A hibiscus's flowers: red.
    1: (r, g, b) => r > 30 && r > 1.8 * g && r > 1.6 * b,
    // Lavender: purple, blue and red over green.
    3: (r, g, b) => b > 25 && b > 1.15 * g && r > 1.02 * g,
};

function run(scene) {
    const world = bro.flora.createWorld({
        rngSeed: 33,
        shadow: { origin: [-16, -2, -16], cellSize: 1, width: 32, height: 30, depth: 32, fill: 1 },
    });
    const kinds = ['hydrangea', 'hibiscus', 'lavender'];
    const at = { hydrangea: [-4, 0, 0], hibiscus: [0, 0, 0], lavender: [4, 0, 0] };
    for (const k of kinds) world.plantSpecies(k, { origin: at[k] });
    world.plantSpecies('fir', { origin: [0, 0, -10] });
    // Past the species' flowering age (10).
    world.grow(650, 0.02);

    const shrubs = world.emitForest({ leafForm: 'broad', seed: 3 });
    const firs = world.emitForest({ leafForm: 'needle', seed: 3 });
    assert(shrubs.specimenCount === 3 && firs.specimenCount === 1, 'three shrubs and a fir');
    assert(shrubs.specimenBlooms && shrubs.specimenBlooms.length === 24, 'each shrub its bloom record');
    assert(firs.bloomCount === 0 && firs.blooms.length === 0, 'the fir bears no blooms');
    const forms = {};
    for (let s = 0; s < 3; s++) {
        const sb = shrubs.specimenBlooms.subarray(s * 8, s * 8 + 8);
        const plant = shrubs.specimens[s * 16 + 11];
        const name = kinds[plant];
        forms[name] = { form: sb[2], start: sb[0], count: sb[1], specimen: s };
        assert(sb[1] > 0, name + ' bears blooms: ' + sb[1]);
        // Each on its own specimen's segments.
        const segCount = shrubs.specimens[s * 16 + 1];
        let own = true;
        for (let b = sb[0]; b < sb[0] + sb[1]; b++) own = own && shrubs.blooms[b * 8 + 7] < segCount;
        assert(own, name + "'s blooms sway with its own twigs");
    }
    assert(forms.hydrangea.form === 2 && forms.hibiscus.form === 1 && forms.lavender.form === 3,
           'a head, a flower and a spike: ' + kinds.map((k) => forms[k].form).join());
    assert(shrubs.bloomCount * 8 === shrubs.blooms.length, 'the bloom count');

    scene.setWind({ speed: 0, gust: 0 });
    const color = [0.25, 0.4, 0.18];
    const node = scene.createForest({ forest: shrubs, color, lod: { budget: 0, shadowBudget: 0 } });
    const firNode = scene.createForest({ forest: firs, color, lod: { budget: 0, shadowBudget: 0 } });
    // The same shrubs with no blooms, for the hue's comparison.
    const bare = Object.assign({}, shrubs, { blooms: new Float32Array(0), specimenBlooms: new Float32Array(0) });

    // ── Each form up close ───────────────────────────────────────────────
    for (const name of kinds) {
        const o = at[name];
        scene.setCamera({ fov: 45, near: 0.05, far: 500, position: [o[0], 1.9, o[2] + 2.6], target: [o[0], 0.7, o[2]] });
        node.setForest({ forest: shrubs });
        const img = scene.captureFrame();
        const stats = node.forestStats();
        assert(stats.blooms > 0, name + ': blooms drawn up close: ' + stats.blooms);
        assert(firNode.forestStats().blooms === 0, name + ': the fir draws none');
        node.setForest({ forest: bare });
        const without = scene.captureFrame();
        assert(node.forestStats().blooms === 0, name + ': none without the emit\'s blooms');

        // The blooms facing the camera most: their pixels take the hue.
        const f = forms[name];
        const hue = HUES[f.form];
        let best = 0, bestWithout = 0, inView = 0;
        for (let b = f.start; b < f.start + f.count; b++) {
            const p = shrubs.blooms.subarray(b * 8, b * 8 + 8);
            // Where its form stands: a head's ball and a flower just past
            // the tip, a spike above its stalk.
            const up = f.form === 3 ? 1.35 * p[3] : f.form === 2 ? 0.3 * p[3] : 0.05 * p[3];
            const s = scene.projectLocal(o[0] + p[0], o[1] + p[1] + up, o[2] + p[2]);
            if (!s || s.behind || s.x < 0 || s.y < 0 || s.x >= 320 || s.y >= 240) continue;
            inView++;
            const n = hued(img, s.x, s.y, 3, hue);
            if (n > best) { best = n; bestWithout = hued(without, s.x, s.y, 3, hue); }
        }
        assert(inView > 0, name + ': blooms in view');
        assert(best >= 6 && bestWithout <= best / 3,
               name + ': a bloom takes its hue: ' + best + ' px (' + bestWithout + ' without the blooms)');
    }
    node.setForest({ forest: shrubs });

    // ── Far off, none ───────────────────────────────────────────────────
    scene.setCamera({ fov: 45, near: 0.5, far: 5000, position: [0, 30, 1500], target: [0, 0, 0] });
    scene.captureFrame();
    assert(node.forestStats().blooms === 0, 'none once every bloom is under a fraction of a pixel: ' +
           node.forestStats().blooms);

    // ── Every tier still draws its blooms nearby ─────────────────────────
    scene.setCamera({ fov: 45, near: 0.1, far: 500, position: [0, 3, 12], target: [0, 0.8, 0] });
    for (const t of ['leaves', 'clumps', 'crowns']) {
        node.setForestTier(t);
        scene.captureFrame();
        assert(node.forestStats().blooms > 0, 'blooms at the ' + t + ' tier: ' + node.forestStats().blooms);
    }
    node.setForestTier('auto');
    node.destroy();
    firNode.destroy();
}

if (bro.flora && bro.flora.available === false) {
    skipTest('bro.flora not compiled in');
} else {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', '320');
    cv.setAttribute('height', '240');
    document.body.appendChild(cv);
    flush();
    const scene = cv.getContext('scene');
    if (!scene) missingGpuContext('scene');
    else run(scene);
}
