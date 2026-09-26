// test_penetrations.js — Headless test for query-only Physics.penetrations, setTransform, setTransforms.

assert(typeof Physics === 'object', 'Physics namespace exists');
assert(typeof Physics.penetrations === 'function', 'Physics.penetrations exists');
assert(typeof Physics.setTransform === 'function', 'Physics.setTransform exists');
assert(typeof Physics.setTransforms === 'function', 'Physics.setTransforms exists');

Physics.destroyAll();

// -----------------------------------------------------------------------------
// Test 1: Two unit boxes offset by 0.8 should report depth 0.2
// -----------------------------------------------------------------------------
// Unit box has halfExtents {0.5, 0.5, 0.5} (size 1x1x1).
// Box 1 at (0, 0, 0): bounds [-0.5, 0.5] along X
// Box 2 at (0.8, 0, 0): bounds [0.3, 1.3] along X
// Overlap along X is [0.3, 0.5], length = 0.2.
const box1 = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
    position: { x: 0, y: 0, z: 0 },
    kinematic: true,
});

const box2 = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
    position: { x: 0.8, y: 0, z: 0 },
    kinematic: true,
});

assert(box1 > 0 && box2 > 0, 'both bodies created');

let res = Physics.penetrations({ bodies: [box1, box2] });
assert(res instanceof Float64Array, 'penetrations returns Float64Array');
assert(res.length === 8, `expected 1 hit (8 floats), got length ${res.length}`);

const tagA = res[0];
const subA = res[1];
const tagB = res[2];
const subB = res[3];
const depth = res[4];
const cx = res[5], cy = res[6], cz = res[7];

assert(tagA === Math.min(box1, box2), `tagA should be smaller body tag, got ${tagA}`);
assert(tagB === Math.max(box1, box2), `tagB should be larger body tag, got ${tagB}`);
assert(subA === 0, `subA should be 0, got ${subA}`);
assert(subB === 0, `subB should be 0, got ${subB}`);
assert(Math.abs(depth - 0.2) < 1e-3, `depth should be ~0.2, got ${depth}`);
assert(Math.abs(cx - 0.4) < 1e-2, `contact x should be ~0.4, got ${cx}`);
assert(Math.abs(cy) <= 0.5 + 1e-3, `contact y should be within [-0.5, 0.5], got ${cy}`);
assert(Math.abs(cz) <= 0.5 + 1e-3, `contact z should be within [-0.5, 0.5], got ${cz}`);
console.log(`[PASS] Two unit boxes offset by 0.8: depth=${depth.toFixed(4)}, contact=(${cx.toFixed(3)}, ${cy.toFixed(3)}, ${cz.toFixed(3)})`);

// -----------------------------------------------------------------------------
// Test 2: minDepth filtering
// -----------------------------------------------------------------------------
let resFiltered = Physics.penetrations({ bodies: [box1, box2], minDepth: 0.25 });
assert(resFiltered.length === 0, `expected 0 hits with minDepth 0.25, got ${resFiltered.length / 8}`);

resFiltered = Physics.penetrations({ bodies: [box1, box2], minDepth: 0.15 });
assert(resFiltered.length === 8, `expected 1 hit with minDepth 0.15, got ${resFiltered.length / 8}`);
console.log('[PASS] minDepth filtering');

// -----------------------------------------------------------------------------
// Test 3: ignorePairs filtering
// -----------------------------------------------------------------------------
let resIgnored = Physics.penetrations({ bodies: [box1, box2], ignorePairs: [[box1, box2]] });
assert(resIgnored.length === 0, `expected 0 hits with ignorePairs, got ${resIgnored.length / 8}`);
console.log('[PASS] ignorePairs filtering');

// -----------------------------------------------------------------------------
// Test 4: setTransform & setTransforms updates
// -----------------------------------------------------------------------------
// Move box2 to x=0.7: depth becomes 0.3
Physics.setTransform(box2, { x: 0.7, y: 0, z: 0 });
res = Physics.penetrations({ bodies: [box1, box2] });
assert(res.length === 8, 'expected 1 hit after setTransform');
assert(Math.abs(res[4] - 0.3) < 1e-3, `depth after setTransform should be ~0.3, got ${res[4]}`);
console.log(`[PASS] setTransform: depth=${res[4].toFixed(4)}`);

// Batch update via setTransforms: move box2 to x=0.6 -> depth 0.4
Physics.setTransforms([box2, 0.6, 0, 0, 0, 0, 0, 1]);
res = Physics.penetrations({ bodies: [box1, box2] });
assert(res.length === 8, 'expected 1 hit after setTransforms');
assert(Math.abs(res[4] - 0.4) < 1e-3, `depth after setTransforms should be ~0.4, got ${res[4]}`);
console.log(`[PASS] setTransforms: depth=${res[4].toFixed(4)}`);

// Pass transforms directly inside penetrations options: move box2 to x=0.9 -> depth 0.1
res = Physics.penetrations({
    bodies: [box1, box2],
    transforms: new Float64Array([box2, 0.9, 0, 0, 0, 0, 0, 1]),
});
assert(res.length === 8, 'expected 1 hit after inline transforms');
assert(Math.abs(res[4] - 0.1) < 1e-3, `depth after inline transforms should be ~0.1, got ${res[4]}`);
console.log(`[PASS] inline transforms in penetrations: depth=${res[4].toFixed(4)}`);

Physics.destroyBody(box1);
Physics.destroyBody(box2);

// -----------------------------------------------------------------------------
// Test 5: Compound shape with two hulls reporting per-sub-shape pairs
// -----------------------------------------------------------------------------
// Compound body with two box sub-shapes:
// sub-shape 0 at local (-1, 0, 0)
// sub-shape 1 at local (+1, 0, 0)
const compoundBody = Physics.createBody({
    shape: 'compound',
    parts: [
        {
            shape: 'box',
            halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
            localPosition: { x: -1, y: 0, z: 0 },
        },
        {
            shape: 'box',
            halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
            localPosition: { x: 1, y: 0, z: 0 },
        },
    ],
    position: { x: 0, y: 0, z: 0 },
    kinematic: true,
});

// Target box placed at (1.8, 0, 0).
// Sub-shape 1 of compound (at world x=1.0, bounds [0.5, 1.5]) will overlap
// targetBox (at world x=1.8, bounds [1.3, 2.3]) by 0.2 along X.
// Sub-shape 0 of compound (at world x=-1.0) will NOT overlap.
const targetBox = Physics.createBody({
    shape: 'box',
    halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
    position: { x: 1.8, y: 0, z: 0 },
    kinematic: true,
});

res = Physics.penetrations({ bodies: [compoundBody, targetBox] });
assert(res.length === 8, `expected 1 hit for compound vs target, got ${res.length / 8}`);

const cTagA = res[0];
const cSubA = res[1];
const cTagB = res[2];
const cSubB = res[3];
const cDepth = res[4];

assert(Math.abs(cDepth - 0.2) < 1e-3, `compound depth should be ~0.2, got ${cDepth}`);

if (cTagA === compoundBody) {
    assert(cSubA === 1, `sub-shape index for compound body should be 1, got ${cSubA}`);
    assert(cSubB === 0, `sub-shape index for target box should be 0, got ${cSubB}`);
} else {
    assert(cSubB === 1, `sub-shape index for compound body should be 1, got ${cSubB}`);
    assert(cSubA === 0, `sub-shape index for target box should be 0, got ${cSubA}`);
}

console.log(`[PASS] Compound with two hulls reports per-sub-shape pair: sub=${cTagA === compoundBody ? cSubA : cSubB}, depth=${cDepth.toFixed(4)}`);

// Move target to overlap sub-shape 0 of compound: place at (-1.8, 0, 0)
Physics.setTransform(targetBox, { x: -1.8, y: 0, z: 0 });
res = Physics.penetrations({ bodies: [compoundBody, targetBox] });
assert(res.length === 8, `expected 1 hit for sub-shape 0, got ${res.length / 8}`);
const subReported = (res[0] === compoundBody) ? res[1] : res[3];
assert(subReported === 0, `sub-shape index should now be 0, got ${subReported}`);
assert(Math.abs(res[4] - 0.2) < 1e-3, `depth should be ~0.2, got ${res[4]}`);
console.log(`[PASS] Compound sub-shape 0 overlap: sub=${subReported}, depth=${res[4].toFixed(4)}`);

Physics.destroyBody(compoundBody);
Physics.destroyBody(targetBox);

// -----------------------------------------------------------------------------
// Test 6: matrix scale reaches the query (off-origin hulls, so the COM moves)
// -----------------------------------------------------------------------------
const cube = (cx, cy, cz, h) => {
    const p = [];
    for (let q = 0; q < 8; q++) p.push(cx + (q & 1 ? h : -h), cy + (q & 2 ? h : -h), cz + (q & 4 ? h : -h));
    return new Float64Array(p);
};
const colMajor = (sx, sy, sz, tx, ty, tz) => [sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, sz, 0, tx, ty, tz, 1];

const pair = Physics.createBody({
    shape: 'compound',
    parts: [
        { shape: 'convexHull', points: cube(-1, 0, 0, 0.5), convexRadius: 0 },
        { shape: 'convexHull', points: cube(1, 0, 0, 0.5), convexRadius: 0 },
    ],
    kinematic: true,
});
const probe = Physics.createBody({ shape: 'convexHull', points: cube(0, 0, 0, 0.5), convexRadius: 0, kinematic: true });
assert(pair > 0 && probe > 0, 'hull bodies created');

const hitOn = (r, body) => (r[0] === body ? r[1] : r[3]);

Physics.setTransforms([{ body: pair, matrix: colMajor(2, 1, 1, 0, 0, 0) }, { body: probe, matrix: colMajor(1, 1, 1, 3.3, 0, 0) }]);
res = Physics.penetrations({ bodies: [pair, probe] });
assert(res.length === 8, `scaled compound: expected 1 hit, got ${res.length / 8}`);
assert(hitOn(res, pair) === 1, `scaled compound: expected sub 1, got ${hitOn(res, pair)}`);
assert(Math.abs(res[4] - 0.2) < 1e-3, `scaled compound: depth should be ~0.2, got ${res[4]}`);
console.log(`[PASS] scaled compound: sub 1 spans x [1, 3], depth=${res[4].toFixed(4)}`);

Physics.setTransforms([{ body: pair, matrix: colMajor(1, 1, 1, 0, 0, 0) }]);
res = Physics.penetrations({ bodies: [pair, probe] });
assert(res.length === 0, `unit scale again: expected 0 hits, got ${res.length / 8}`);
console.log('[PASS] unit scale restores the unscaled shape');

Physics.setTransforms([{ body: pair, matrix: colMajor(1, 2, 1, 0, 0, 0) }, { body: probe, matrix: colMajor(1, 1, 1, 1, 1.3, 0) }]);
res = Physics.penetrations({ bodies: [pair, probe] });
assert(res.length === 8 && Math.abs(res[4] - 0.2) < 1e-3, `y-scaled: depth should be ~0.2, got ${res.length ? res[4] : 'no hit'}`);
console.log(`[PASS] non-uniform scale along y: depth=${res[4].toFixed(4)}`);

Physics.setTransforms([{ body: pair, matrix: colMajor(-1, 1, 1, 0, 0, 0) }, { body: probe, matrix: colMajor(1, 1, 1, -1.8, 0, 0) }]);
res = Physics.penetrations({ bodies: [pair, probe] });
assert(res.length === 8, `mirrored: expected 1 hit, got ${res.length / 8}`);
assert(hitOn(res, pair) === 1, `mirrored: sub 1 now sits at x -1, got sub ${hitOn(res, pair)}`);
assert(Math.abs(res[4] - 0.2) < 1e-3, `mirrored: depth should be ~0.2, got ${res[4]}`);
console.log(`[PASS] mirrored matrix: sub 1 at x -1, depth=${res[4].toFixed(4)}`);

// -----------------------------------------------------------------------------
// Test 7: touching or apart pairs are not penetrations unless maxSeparation asks
// -----------------------------------------------------------------------------
Physics.setTransforms([{ body: pair, matrix: colMajor(1, 1, 1, 0, 0, 0) }, { body: probe, matrix: colMajor(1, 1, 1, 2.2, 0, 0) }]);
res = Physics.penetrations({ bodies: [pair, probe] });
assert(res.length === 0, `apart by 0.2: expected 0 hits, got ${res.length / 8}`);
res = Physics.penetrations({ bodies: [pair, probe], maxSeparation: 0.5 });
assert(res.length === 8 && res[4] < 0, `maxSeparation 0.5: expected one negative-depth hit, got ${res.length ? res[4] : 'none'}`);
console.log(`[PASS] separated pairs: none by default, depth ${res[4].toFixed(4)} with maxSeparation`);

// -----------------------------------------------------------------------------
// Test 8: a small sharp hull keeps its exact depth
// -----------------------------------------------------------------------------
const bolt = Physics.createBody({ shape: 'convexHull', points: cube(0, 5, 0, 0.01), convexRadius: 0, kinematic: true });
const bolt2 = Physics.createBody({ shape: 'convexHull', points: cube(0, 5, 0, 0.01), convexRadius: 0, kinematic: true });
Physics.setTransforms([{ body: bolt2, matrix: colMajor(1, 1, 1, 0.015, 0, 0) }]);
res = Physics.penetrations({ bodies: [bolt, bolt2] });
assert(res.length === 8 && Math.abs(res[4] - 0.005) < 2e-4, `small hulls: depth should be ~0.005, got ${res.length ? res[4] : 'no hit'}`);
console.log(`[PASS] small sharp hulls: depth=${res[4].toFixed(5)}`);

// -----------------------------------------------------------------------------
// Test 9: an ambiguous flat length needs its stride
// -----------------------------------------------------------------------------
let threw = false;
try { Physics.setTransforms(new Float64Array(136)); } catch (e) { threw = true; }
assert(threw, 'a length of 136 (8 x 17) without a stride throws');
console.log('[PASS] ambiguous stride throws');

Physics.destroyBody(pair);
Physics.destroyBody(probe);
Physics.destroyBody(bolt);
Physics.destroyBody(bolt2);

console.log('ALL PENETRATION TESTS PASSED');
