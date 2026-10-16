// Forest: bro.flora's emitForest / plantForest / forestCover and bro.scene's
// forest node (docs/flora-api.js "Forests", docs/scene-nodes-api.js
// createForest). Contract under test:
//   - emitForest's arrays fit together (record sizes, each specimen's runs,
//     keep radii thickest first, leaves as Uint32 pairs) and its own
//     instances stand every plant where it grew;
//   - plantForest stands trees over an area, deterministically, on the
//     ground function, thinned by density; forestCover is clear at a trunk
//     and full in the open;
//   - TIER SELECTION (forestTreeLod): as a tree shrinks on screen its leaves
//     widen and thin (keep = 1 / widening^2), turn to cards, hand over to
//     clumps and then to crowns, the weights summing to 1 and never turning
//     back; bark drops its thinnest segments;
//   - the choice follows the size on screen, not metres: twice the distance
//     through half the field of view draws the same;
//   - the vertex budget scales the detail to fit;
//   - POSE AGREEMENT: every tier stands in for the same tree posed by the
//     same sway, so in a wind each tier's silhouette moves the same way and
//     about as far; a calm is still at every tier;
//   - every tier draws under validation.

// Two broadleaf trees (the grove demo's recipe): a clear trunk with three
// scaffold limbs, the crown grown from monopodial modules on it.
function trunkPrototype() {
    const H = 2.0, n = 6, nodes = [], edges = [];
    for (let i = 0; i <= n; i++) {
        nodes.push({ position: [0.03 * Math.sin(i * 1.7), H * i / n, 0.03 * Math.cos(i * 2.3)],
                     ageAtBirth: 0.03 * i, lengthMax: 1, thickening: 1 });
        if (i) edges.push([i - 1, i]);
    }
    for (let k = 0; k < 3; k++) {
        const a = k * 2.094 + 0.4;
        nodes.push({ position: [Math.cos(a) * 0.25, H + 0.15, Math.sin(a) * 0.25], ageAtBirth: 0.2, lengthMax: 0.8, thickening: 1 });
        edges.push([n, nodes.length - 1]);
    }
    return { name: 'trunk', nodes, edges, rootNode: 0, terminalNodes: [n, n + 1, n + 2, n + 3] };
}

function growWorld() {
    const world = bro.flora.createWorld({
        rngSeed: 11,
        shadow: { origin: [-12, -2, -8], cellSize: 1, width: 24, height: 26, depth: 16, fill: 1 },
    });
    const mono = world.addPrototype(bro.flora.prototypes.monopodial(3, 0.7));
    const trunk = world.addPrototype(trunkPrototype());
    world.addVoronoiSite(mono, 0.5, 0.3);
    const species = {
        pipeExp: 2.0, leafDiameter: 0.028, shadeTolerance: 0.35, moduleMatureAge: 0.6, rootVigorMax: 4.0,
        apicalControl: 0.35, apicalControlMature: 0.3, individualVariation: 0.2, maxAge: 400, seedingRadius: 0,
        growthScale: 2.5, orthotropy: 0.55, tropismG2: 0.05,
    };
    world.addPlant({ origin: [-5, 0, 0], prototypeIndex: trunk, species });
    world.addPlant({ origin: [5, 0, 0], prototypeIndex: trunk, species });
    for (let i = 0; i < 1000; i++) world.step(0.04);
    return world;
}

const FOLIAGE = { seed: 3, perArea: 60 };

function leaf() {
    const m = Mesh.leafCard('oval', { width: 0.12, length: 0.2 });
    m.colors = new Float32Array(0);
    return m;
}

function diffCount(a, b, tol) {
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        if (Math.abs(a.data[i] - b.data[i]) > tol || Math.abs(a.data[i + 1] - b.data[i + 1]) > tol ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > tol) n++;
    }
    return n;
}

// The centroid (x, y in pixels) and count of the pixels that differ from `bg`.
function silhouette(img, bg) {
    let sx = 0, sy = 0, n = 0;
    for (let y = 0; y < img.height; y++) {
        for (let x = 0; x < img.width; x++) {
            const o = (y * img.width + x) * 4;
            if (Math.abs(img.data[o] - bg.data[o]) + Math.abs(img.data[o + 1] - bg.data[o + 1]) +
                Math.abs(img.data[o + 2] - bg.data[o + 2]) > 24) { sx += x; sy += y; n++; }
        }
    }
    return { x: n ? sx / n : 0, y: n ? sy / n : 0, n };
}

if (bro.flora && bro.flora.available === false) {
    skipTest('bro.flora not compiled in');
} else {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', '200');
    cv.setAttribute('height', '160');
    document.body.appendChild(cv);
    flush();
    const scene = cv.getContext('scene');
    if (!scene) missingGpuContext('scene');
    else run(scene);
}

function run(scene) {
    // ── emitForest ───────────────────────────────────────────────────────
    const world = growWorld();
    const f = world.emitForest(FOLIAGE);
    assert(f && f.specimenCount === 2, 'a specimen a plant: ' + (f && f.specimenCount));
    assert(f.specimens.length === 32 && f.instances.length === 16, 'specimens 16 floats, instances 8');
    const segs = f.segments.length / 8, clumps = f.clumps.length / 8;
    assert(Number.isInteger(segs) && segs > 0 && f.tangents.length === segs * 6 &&
           f.segmentVegetation.length === segs * 16 && f.keepRadius.length === segs,
           'segment arrays fit: ' + segs);
    assert(Number.isInteger(clumps) && clumps > 0 && f.clumpVegetation.length === clumps * 16,
           'clump arrays fit: ' + clumps);
    assert(f.leaves instanceof Uint32Array && f.leaves.length === f.leafCount * 2, 'leaves are uint32 pairs');
    let segSum = 0, clumpSum = 0, leafSum = 0, sorted = true;
    for (let s = 0; s < 2; s++) {
        const r = f.specimens.subarray(s * 16, s * 16 + 16);
        assert(r[0] === segSum && r[2] === clumpSum && r[4] === leafSum, 'specimen ' + s + ' runs follow on');
        segSum += r[1]; clumpSum += r[3]; leafSum += r[5];
        for (let i = r[0] + 1; i < r[0] + r[1]; i++) if (f.keepRadius[i] > f.keepRadius[i - 1]) sorted = false;
        assert(r[6] > 1 && r[7] > 0.3, 'specimen ' + s + ' has a height and crown: ' + r[6] + ', ' + r[7]);
        assert(Math.abs(f.instances[s * 8] - r[12]) < 1e-5 && f.instances[s * 8 + 5] === s,
               'its own instance stands it where it grew');
    }
    assert(segSum === segs && clumpSum === clumps && leafSum === f.leafCount, 'the runs cover the arrays');
    assert(sorted, 'keep radii run thickest first within a specimen');

    // ── plantForest / forestCover ────────────────────────────────────────
    const ground = (x, z) => 0.05 * x + 1;
    const opts = { area: [-40, -40, 40, 40], spacing: 8, ground, seed: 5, scale: [1, 2] };
    const trees = bro.flora.plantForest(f, opts);
    const n = trees.length / 8;
    assert(n > 60 && n <= 121, 'about one tree a cell: ' + n);
    let inside = true, onGround = true, scaled = true, specimens = true;
    for (let i = 0; i < n; i++) {
        const [x, y, z, s, , sp] = trees.subarray(i * 8, i * 8 + 8);
        if (x < -40 || x > 40 || z < -40 || z > 40) inside = false;
        if (Math.abs(y - (ground(x, z) - 0.1)) > 1e-4) onGround = false;
        if (s < 1 || s > 2) scaled = false;
        if (sp !== 0 && sp !== 1) specimens = false;
    }
    assert(inside && onGround && scaled && specimens, 'inside the area, on the ground, scaled, a specimen each');
    assert(bro.flora.plantForest(f, opts).every((v, i) => v === trees[i]), 'the same seed plants the same forest');
    assert(bro.flora.plantForest(f, Object.assign({}, opts, { density: 0 })).length === 0, 'density 0 plants nothing');
    const half = bro.flora.plantForest(f, Object.assign({}, opts, { density: (x) => (x < 0 ? 1 : 0) })).length / 8;
    assert(half > n * 0.3 && half < n * 0.7, 'a density function thins where it is low: ' + half + ' of ' + n);
    const cover = bro.flora.forestCover(f, { instances: trees });
    assert(cover(trees[0], trees[2]) < 0.05, 'nothing grows at a trunk: ' + cover(trees[0], trees[2]));
    assert(cover(500, 500) > 0.99, 'full cover in the open: ' + cover(500, 500));

    // ── The node ─────────────────────────────────────────────────────────
    const node = scene.createForest({ forest: f, leaf: leaf(), color: '#4a7a2a', castsShadow: true });
    assert(node && node.forestLod, 'createForest returns the node');
    node.setForestLod({ leafPixels: 9, budget: 0, shadowBudget: 0 });
    const lod = node.forestLod();
    assert(lod.leafPixels === 9 && lod.clumpFrom === 3 && lod.budget === 0, 'lod round trips: ' + JSON.stringify(lod));

    // ── Tier selection ───────────────────────────────────────────────────
    let prev = null, monotone = true, sums = true, keeps = true, cards = true;
    let sawThin = false, sawCard = false, sawClumps = false, sawCrowns = false, sawBlend = false;
    for (let px = 4000; px > 0.3; px *= 0.93) {
        const t = node.forestTreeLod(0, px);
        if (Math.abs(t.leaves + t.clumps + t.crowns - 1) > 1e-5) sums = false;
        if (Math.abs(t.keep - 1 / (t.widening * t.widening)) > 1e-5) keeps = false;
        if (t.card !== t.widening > lod.cardWidening) cards = false;
        if (prev && (t.widening < prev.widening - 1e-6 || t.leaves > prev.leaves + 1e-6 ||
                     t.crowns < prev.crowns - 1e-6 || t.barkRadius < prev.barkRadius - 1e-9)) monotone = false;
        if (t.widening > 1.01) sawThin = true;
        if (t.card) sawCard = true;
        if (t.clumps > 0.99) sawClumps = true;
        if (t.crowns > 0.99) sawCrowns = true;
        if (t.leaves > 0.05 && t.clumps > 0.05) sawBlend = true;
        prev = t;
    }
    const near = node.forestTreeLod(0, 4000), far = node.forestTreeLod(0, 0.3);
    assert(near.leaves === 1 && near.widening === 1 && near.barkSides === 10, 'close up: every leaf, full bark');
    assert(far.crowns === 1 && far.barkSides === 5, 'far off: a crown');
    assert(sums, 'the tier weights sum to 1');
    assert(keeps, 'keep = 1 / widening^2');
    assert(cards, 'leaves turn to cards past cardWidening');
    assert(monotone, 'shrinking on screen never brings detail back');
    assert(sawThin && sawCard && sawBlend && sawClumps && sawCrowns, 'every stage is passed through');

    // ── Screen size, not metres ──────────────────────────────────────────
    // The same tree from the same spot in a view twice as tall is twice the
    // pixels, so it draws more leaves; widen the field of view to halve the
    // pixels again and it draws what the small view did.
    scene.setWind({ speed: 0, gust: 0 });
    const cv2 = document.createElement('canvas');
    cv2.setAttribute('width', '400');
    cv2.setAttribute('height', '320');
    document.body.appendChild(cv2);
    flush();
    const big = cv2.getContext('scene');
    const node2 = big.createForest({ forest: f, leaf: leaf(), color: '#4a7a2a', lod: node.forestLod() });
    const fovFor = (k) => 2 * Math.atan(k * Math.tan(15 * Math.PI / 180)) * 180 / Math.PI;
    const look = (scn, nd, fov) => {
        scn.setCamera({ fov, near: 0.1, far: 500, position: [5, 3, 18], target: [5, 3, 0] });
        scn.captureFrame();
        return nd.forestStats();
    };
    const small = look(scene, node, 30), sharp = look(big, node2, 30), wide = look(big, node2, fovFor(2));    assert(small.leaves > 0 && small.leaves < f.leafCount, 'the small view thins the crowns: ' + small.leaves);
    assert(sharp.leaves > small.leaves * 2, 'twice the pixels, more leaves: ' + sharp.leaves + ' vs ' + small.leaves);
    assert(Math.abs(wide.leaves - small.leaves) <= Math.max(2, 0.01 * small.leaves),
           'the same pixels, the same leaves: ' + wide.leaves + ' vs ' + small.leaves);
    node2.destroy();
    cv2.remove();

    // ── The budget ───────────────────────────────────────────────────────
    scene.setCamera({ fov: 50, near: 0.1, far: 500, position: [0, 3, 18], target: [0, 3, 0] });
    scene.captureFrame();
    const free = node.forestStats();
    assert(free.detail === 1, 'no budget, full detail');
    node.setForestLod({ budget: free.vertices * 0.25e-6 });
    scene.captureFrame();
    const tight = node.forestStats();
    assert(tight.detail > 1 && tight.vertices < free.vertices * 0.4,
           'a quarter budget scales the detail down: ' + tight.detail.toFixed(2) + ', ' + tight.vertices + ' of ' + free.vertices);
    node.setForestLod({ budget: 0 });

    // ── Pose agreement ───────────────────────────────────────────────────
    // A steady wind along +x (no gusts, so the bend settles): each tier's
    // silhouette moves right by about as much.
    scene.setCamera({ fov: 50, near: 0.1, far: 500, position: [0, 4, 26], target: [0, 4, 0] });
    node.visible = false;
    const bg = scene.captureFrame();
    node.visible = true;
    const TIERS = ['leaves', 'clumps', 'crowns'];
    const rest = {}, bent = {};
    for (const t of TIERS) {
        node.setForestTier(t);
        const c0 = scene.captureFrame();
        rest[t] = silhouette(c0, bg);
        assert(rest[t].n > 500, t + ': the tier draws (' + rest[t].n + ' px)');
        advanceTime(500);
        assert(diffCount(c0, scene.captureFrame(), 0) === 0, t + ': still in a calm');
    }
    scene.setWind({ direction: [1, 0, 0], speed: 9, gust: 0, drift: 0 });
    advanceTime(6000);
    for (const t of TIERS) {
        node.setForestTier(t);
        bent[t] = silhouette(scene.captureFrame(), bg);
    }
    const dx = TIERS.map((t) => bent[t].x - rest[t].x);
    console.log('pose: each tier\'s silhouette moved ' + dx.map((v) => v.toFixed(2)).join(' / ') + ' px (leaves / clumps / crowns)');
    const lo = Math.min(...dx), hi = Math.max(...dx);
    assert(lo > 0.5, 'every tier bends downwind: ' + dx.map((v) => v.toFixed(2)).join(' / ') + ' px');
    assert(hi < 2.2 * lo, 'and about as far: ' + dx.map((v) => v.toFixed(2)).join(' / ') + ' px');
    node.setForestTier('auto');
    node.destroy();
}
