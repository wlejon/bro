// Repro test for the "errant top-cap triangle" bug.
//
// Scenario from the bug report:
//   1. Create a circle.
//   2. Push/pull UP into a cylinder (flat-extrude).
//   3. Pull a few side facets OUT individually.
//   4. Push one side facet IN.
//
// Expected: the top/bottom caps stay as single face groups; no spurious
// model edges render across the cap interior.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_pushpull_circle.js

'use strict';

const E   = window.__editor;
const reg = E.registry;
const h   = E.history;

let tests = 0, failed = 0;
function t(name, fn) {
    tests++;
    try { fn(); console.log('  ok   ' + name); }
    catch (e) {
        failed++;
        console.log('  FAIL ' + name + ': ' + (e && e.message ? e.message : e));
        if (e && e.stack) console.log(e.stack);
    }
}
function eq(a, b, msg) {
    const ja = JSON.stringify(a), jb = JSON.stringify(b);
    if (ja !== jb) throw new Error((msg || 'eq') + ': ' + ja + ' !== ' + jb);
}
function near(a, b, eps, msg) {
    if (Math.abs(a - b) > (eps || 1e-5)) {
        throw new Error((msg || 'near') + ': ' + a + ' vs ' + b);
    }
}
function truthy(v, msg) { if (!v) throw new Error(msg || 'expected truthy'); }

function triCentroid(prim, triIdx) {
    const P = prim.positions;
    const I = prim.indices;
    const c = [0, 0, 0];
    for (let k = 0; k < 3; k++) {
        const vi = I[triIdx * 3 + k];
        c[0] += P[vi * 3 + 0];
        c[1] += P[vi * 3 + 1];
        c[2] += P[vi * 3 + 2];
    }
    c[0] /= 3; c[1] /= 3; c[2] /= 3;
    return c;
}

function pushPullGroup(prim, groupIdx, distance) {
    const g = prim.faceGroups.groups[groupIdx];
    const tri0 = g.tris[0];
    E.setTool('pushpull');
    E.beginPushPullOn(prim, {
        triangleIndex: tri0,
        position: triCentroid(prim, tri0),
        normal: g.normal.slice(),
        distance: 0,
    });
    E.applyPushPull(distance);
    E.commitPushPull();
}

// Pick a side facet whose outward normal has the given x,z direction closest.
function findSideGroup(prim, dirX, dirZ) {
    const groups = prim.faceGroups.groups;
    let best = -1, bestDot = -Infinity;
    const L = Math.hypot(dirX, dirZ) || 1;
    const ux = dirX / L, uz = dirZ / L;
    for (let i = 0; i < groups.length; i++) {
        const n = groups[i].normal;
        if (Math.abs(n[1]) > 0.01) continue;   // skip caps
        const dot = n[0] * ux + n[2] * uz;
        if (dot > bestDot) { bestDot = dot; best = i; }
    }
    return best;
}

// -------------------------------------------------------------------------
// Setup
// -------------------------------------------------------------------------

// Build: circle → flat-extrude → pull 3 side facets out → push 1 in.
function buildScene(segments) {
    reg.clear();
    h.clear();

    // 1. Circle with known segment count.
    E.setTool('circle');
    E.beginCircle([0, 0, 0]);
    E.updateCircleAt([1, 0, 0]);
    // Force segment count via the tool state before commit.
    E.circleToolState.segments = segments;
    E.commitCircle();

    const circle = reg.primitives[reg.primitives.length - 1];
    reg.setActive(circle.id);

    eq(circle.positions.length / 3, segments,
       `circle has ${segments} rim verts`);
    eq(circle.faceGroups.groups.length, 1, 'circle is one face group');

    // 2. Flat-extrude into a cylinder.
    pushPullGroup(circle, 0, 1.0);

    // After flat-extrude we expect: 2 caps + `segments` side walls.
    eq(circle.faceGroups.groups.length, segments + 2,
       `cylinder has ${segments + 2} face groups`);

    return circle;
}

// -------------------------------------------------------------------------
// Diagnostics: look for unexpected model edges on the cap.
//
// A clean cylinder-cap interior has NO model edges — every interior diagonal
// separates two triangles in the same cap face group. The bug manifests as
// model edges crossing the cap interior.
// -------------------------------------------------------------------------

function capRimVertIndices(prim, y) {
    // Unique positions at the given Y (within 1e-4).
    const Q = 1e5;
    const keys = new Map();
    for (let vi = 0; vi < prim.positions.length / 3; vi++) {
        const py = prim.positions[vi * 3 + 1];
        if (Math.abs(py - y) > 1e-3) continue;
        const k = Math.round(prim.positions[vi * 3 + 0] * Q) + ',' +
                  Math.round(prim.positions[vi * 3 + 1] * Q) + ',' +
                  Math.round(prim.positions[vi * 3 + 2] * Q);
        if (!keys.has(k)) keys.set(k, vi);
    }
    return keys;
}

// Count inferenceGeo edges that lie on the cap plane (both endpoints at
// matching Y). In a clean cylinder, these edges trace the rim only — the
// rim has `segments` edges. Anything higher = interior diagonals leaked out.
function countCapPlaneEdges(prim, y, epsY) {
    epsY = epsY != null ? epsY : 1e-3;
    const geo = prim.inferenceGeo;
    let onRim = 0, onInterior = 0;
    const Q = 1e5;
    const rimPosKeys = new Set();
    // Rim positions = cap vertex positions at Y.
    for (let vi = 0; vi < prim.positions.length / 3; vi++) {
        if (Math.abs(prim.positions[vi * 3 + 1] - y) > epsY) continue;
        const k = Math.round(prim.positions[vi * 3 + 0] * Q) + ',' +
                  Math.round(prim.positions[vi * 3 + 1] * Q) + ',' +
                  Math.round(prim.positions[vi * 3 + 2] * Q);
        rimPosKeys.add(k);
    }
    for (const e of geo.edges) {
        const ay = geo.positions[e.a * 3 + 1];
        const by = geo.positions[e.b * 3 + 1];
        if (Math.abs(ay - y) > epsY) continue;
        if (Math.abs(by - y) > epsY) continue;
        // Rim edge = connects two adjacent rim positions (both in rimPosKeys).
        const ka = Math.round(geo.positions[e.a * 3 + 0] * Q) + ',' +
                   Math.round(geo.positions[e.a * 3 + 1] * Q) + ',' +
                   Math.round(geo.positions[e.a * 3 + 2] * Q);
        const kb = Math.round(geo.positions[e.b * 3 + 0] * Q) + ',' +
                   Math.round(geo.positions[e.b * 3 + 1] * Q) + ',' +
                   Math.round(geo.positions[e.b * 3 + 2] * Q);
        if (rimPosKeys.has(ka) && rimPosKeys.has(kb)) onRim++;
        else onInterior++;
    }
    return { onRim, onInterior };
}

// -------------------------------------------------------------------------
// Baseline: cylinder straight from flat-extrude has a clean cap (no
// interior edges).
// -------------------------------------------------------------------------

t('baseline: flat-extruded cylinder cap has no interior model edges', () => {
    const cyl = buildScene(16);
    const top = countCapPlaneEdges(cyl, 1.0);
    const bot = countCapPlaneEdges(cyl, 0.0);
    // Rim should have exactly `segments` edges; interior count must be 0.
    eq(top.onRim, 16, `top rim has 16 edges (got ${top.onRim})`);
    eq(top.onInterior, 0,
       `top cap has no interior edges (got ${top.onInterior})`);
    eq(bot.onRim, 16, `bottom rim has 16 edges (got ${bot.onRim})`);
    eq(bot.onInterior, 0,
       `bottom cap has no interior edges (got ${bot.onInterior})`);
});

// -------------------------------------------------------------------------
// Repro: pull a side facet out, then push another in. Inspect cap edges.
// -------------------------------------------------------------------------

t('repro: pull 3 facets out, push 1 in — cap edges do not leak', () => {
    const cyl = buildScene(16);
    const startGroups = cyl.faceGroups.groups.length;
    eq(startGroups, 18, '16 side walls + 2 caps before any pulls');

    // Pull facet nearest +X outward by 0.5.
    pushPullGroup(cyl, findSideGroup(cyl, 1, 0), 0.5);
    // Pull facet nearest +Z outward by 0.4.
    pushPullGroup(cyl, findSideGroup(cyl, 0, 1), 0.4);
    // Pull facet nearest -X outward by 0.3.
    pushPullGroup(cyl, findSideGroup(cyl, -1, 0), 0.3);
    // Push facet nearest -Z inward by -0.2.
    pushPullGroup(cyl, findSideGroup(cyl, 0, -1), -0.2);

    // Vertex-substitution surgery adds NO new groups — neighbouring facets
    // tilt to new planes in place.
    const after = cyl.faceGroups.groups.length;
    const added = after - startGroups;
    eq(added, 0, `no new groups (got ${added} new)`);

    // Crucial check: cap interior still has no model edges.
    const top = countCapPlaneEdges(cyl, 1.0);
    const bot = countCapPlaneEdges(cyl, 0.0);
    console.log('  top edges:', JSON.stringify(top));
    console.log('  bot edges:', JSON.stringify(bot));
    eq(top.onInterior, 0,
       `top cap has no interior edges (got ${top.onInterior})`);
    eq(bot.onInterior, 0,
       `bottom cap has no interior edges (got ${bot.onInterior})`);
});

// -------------------------------------------------------------------------
// Snapshot: print inference-geo edges grouped by where they lie so we can
// eyeball whether anything suspicious leaks out.
// -------------------------------------------------------------------------

t('snapshot: dump all rendered edges on the bug scenario', () => {
    const cyl = buildScene(16);
    pushPullGroup(cyl, findSideGroup(cyl, 1, 0), 0.5);
    pushPullGroup(cyl, findSideGroup(cyl, 0, 1), 0.4);
    pushPullGroup(cyl, findSideGroup(cyl, -1, 0), 0.3);
    pushPullGroup(cyl, findSideGroup(cyl, 0, -1), -0.2);

    const geo = cyl.inferenceGeo;
    console.log('  total rendered edges:', geo.edges.length);
    let topCnt = 0, botCnt = 0, vertCnt = 0, mixedCnt = 0;
    for (const e of geo.edges) {
        const ay = geo.positions[e.a * 3 + 1];
        const by = geo.positions[e.b * 3 + 1];
        const onTop = Math.abs(ay - 1) < 1e-3 && Math.abs(by - 1) < 1e-3;
        const onBot = Math.abs(ay) < 1e-3 && Math.abs(by) < 1e-3;
        const vert  = Math.abs(ay - by) > 0.5;
        if (onTop) topCnt++;
        else if (onBot) botCnt++;
        else if (vert) vertCnt++;
        else mixedCnt++;
    }
    console.log(`  top-plane: ${topCnt}  bot-plane: ${botCnt}  vertical: ${vertCnt}  mixed: ${mixedCnt}`);
});

// -------------------------------------------------------------------------
// Render a screenshot of the bug scenario for visual inspection.
// -------------------------------------------------------------------------

t('baseline cap triangulation winding (no push/pulls)', () => {
    reg.clear();
    h.clear();
    E.setTool('circle');
    E.beginCircle([0, 0, 0]);
    E.updateCircleAt([1, 0, 0]);
    E.circleToolState.segments = 32;
    E.commitCircle();

    const circle = reg.primitives[reg.primitives.length - 1];
    reg.setActive(circle.id);
    pushPullGroup(circle, 0, 1.0);   // flat extrude to make it a cylinder

    const I = circle.indices;
    const P = circle.positions;
    const capTris = circle.faceGroups.groups.find(g => g.normal[1] > 0.99).tris;
    let flipped = 0;
    for (const t of capTris) {
        const i0 = I[t*3], i1 = I[t*3+1], i2 = I[t*3+2];
        const ax = P[i0*3], az = P[i0*3+2];
        const bx = P[i1*3], bz = P[i1*3+2];
        const cx = P[i2*3], cz = P[i2*3+2];
        // 3D cross product Y component: (E1 × E2).y = E1.z*E2.x - E1.x*E2.z
        const ex1 = bx - ax, ez1 = bz - az;
        const ex2 = cx - ax, ez2 = cz - az;
        const cy = ez1 * ex2 - ex1 * ez2;
        if (cy < 0) {
            flipped++;
            if (flipped < 4) {
                console.log(`  baseline flipped cap tri ${t}: ` +
                  `[${ax.toFixed(3)},${az.toFixed(3)}] ` +
                  `[${bx.toFixed(3)},${bz.toFixed(3)}] ` +
                  `[${cx.toFixed(3)},${cz.toFixed(3)}] cy=${cy.toFixed(4)}`);
            }
        }
    }
    console.log(`  baseline cap: ${flipped} of ${capTris.length} tris are CW (should be 0)`);
});

t('cap triangulation has no long/near-collinear triangles (32-gon)', () => {
    const cyl = buildScene(32);
    const I = cyl.indices;
    const P = cyl.positions;
    const capTris = cyl.faceGroups.groups.find(g => g.normal[1] > 0.99).tris;
    // Maximum angular span (on the rim circle) of any single cap triangle.
    // For a Delaunay-like triangulation of a regular 32-gon, the maximum
    // span should be small — two adjacent rim verts + one near-neighbor.
    // A span ≥ 90° signals a pathological diagonal.
    let maxSpan = 0;
    for (const t of capTris) {
        const angles = [];
        for (let k = 0; k < 3; k++) {
            const vi = I[t * 3 + k];
            const x = P[vi * 3 + 0];
            const z = P[vi * 3 + 2];
            angles.push(Math.atan2(z, x));
        }
        angles.sort((a, b) => a - b);
        const g1 = angles[1] - angles[0];
        const g2 = angles[2] - angles[1];
        const g3 = 2 * Math.PI - (angles[2] - angles[0]);
        const span = Math.max(g1, g2, g3);
        if (span > maxSpan) maxSpan = span;
        if (span > Math.PI * 0.5) {
            const vs = [];
            for (let k = 0; k < 3; k++) {
                const vi = I[t * 3 + k];
                vs.push([P[vi*3+0].toFixed(3), P[vi*3+1].toFixed(3), P[vi*3+2].toFixed(3)]);
            }
            console.log('  LONG-SPAN cap tri', t, `span=${(span*180/Math.PI).toFixed(1)}°:`, JSON.stringify(vs));
        }
    }
    console.log(`  max cap-tri angular span: ${(maxSpan*180/Math.PI).toFixed(1)}°`);
});

t('render: screenshot of the bug scenario (32 segments, adjacent pulls)', () => {
    const cyl = buildScene(32);

    // Track which cap-rim vertex positions move across each pull, so we can
    // correlate the flipped triangle back to a specific pull.
    function snapshotCapPositions(c) {
        const out = [];
        for (let vi = 0; vi < c.positions.length / 3; vi++) {
            if (Math.abs(c.positions[vi*3+1] - 1) < 1e-3) {
                out.push([+c.positions[vi*3+0].toFixed(3),
                          +c.positions[vi*3+2].toFixed(3)]);
            }
        }
        return out;
    }
    const beforeAll = snapshotCapPositions(cyl);
    console.log('  before any pulls, cap verts (first 5):',
                JSON.stringify(beforeAll.slice(0, 5)));

    pushPullGroup(cyl, findSideGroup(cyl, Math.cos(-Math.PI/8), Math.sin(-Math.PI/8)), 0.6);
    pushPullGroup(cyl, findSideGroup(cyl, 1, 0), 0.8);
    pushPullGroup(cyl, findSideGroup(cyl, Math.cos(Math.PI/8), Math.sin(Math.PI/8)), 0.6);
    pushPullGroup(cyl, findSideGroup(cyl, -1, 0), -0.3);

    // Find which cap-rim verts moved farthest.
    const afterAll = snapshotCapPositions(cyl);
    const moved = [];
    for (let i = 0; i < beforeAll.length && i < afterAll.length; i++) {
        const dx = afterAll[i][0] - beforeAll[i][0];
        const dz = afterAll[i][1] - beforeAll[i][1];
        const d = Math.hypot(dx, dz);
        if (d > 0.1) moved.push({ i, before: beforeAll[i], after: afterAll[i], d: +d.toFixed(3) });
    }
    console.log('  cap verts that moved (>0.1):', JSON.stringify(moved.slice(0, 8)));

    const geo = cyl.inferenceGeo;
    console.log('  total rendered edges:', geo.edges.length);
    let topCnt = 0, botCnt = 0, vertCnt = 0, mixedCnt = 0;
    for (const e of geo.edges) {
        const ay = geo.positions[e.a * 3 + 1];
        const by = geo.positions[e.b * 3 + 1];
        const onTop = Math.abs(ay - 1) < 1e-3 && Math.abs(by - 1) < 1e-3;
        const onBot = Math.abs(ay) < 1e-3 && Math.abs(by) < 1e-3;
        const vert  = Math.abs(ay - by) > 0.5;
        if (onTop) topCnt++;
        else if (onBot) botCnt++;
        else if (vert) vertCnt++;
        else mixedCnt++;
    }
    console.log(`  top-plane: ${topCnt}  bot-plane: ${botCnt}  vertical: ${vertCnt}  mixed: ${mixedCnt}`);

    // Pitch the camera well down — we want to see the top cap head-on to
    // catch any errant cap edges.
    const cam = E.scene && null; // no direct cam handle via __editor
    // Drive camera via global `E.registry` → scene.setCamera. Simpler: rebuild
    // view by spinning the orbit cam.
    for (let i = 0; i < 150; i++) {
        // Walking it down 1 pixel at a time keeps each rotation in the safe
        // pitch range (quaternion-based orbit clamps per-step).
    }
    // Directly set the scene camera above +Y, looking straight down.
    E.scene.setCamera({
        position: [0.1, 5, 0.1],
        target:   [0, 0.5, 0],
        up:       [0, 0, -1],
        fov: 45, near: 0.1, far: 1000,
        aspect: 1920 / 1080,
    });
    advanceTime(100);
    flush();
    screenshot('apps/scene-editor/_pushpull_circle_top.png');

    // Angled view — matches the user's screenshot perspective.
    E.scene.setCamera({
        position: [2.5, 2.2, 2.5],
        target:   [0, 0.5, 0],
        up:       [0, 1, 0],
        fov: 45, near: 0.1, far: 1000,
        aspect: 1920 / 1080,
    });
    advanceTime(100);
    flush();
    screenshot('apps/scene-editor/_pushpull_circle_iso.png');

    // Dump the cap triangulation indices so we can see what manifold chose.
    const I = cyl.indices;
    const P = cyl.positions;
    const capTris = cyl.faceGroups.groups.find(g => g.normal[1] > 0.99).tris;
    console.log('  top cap tri count:', capTris.length);

    // Check for degenerate or flipped triangles on the cap. A triangle whose
    // signed 2D area (in the XZ plane, since cap normal is +Y) is near zero
    // or opposite-sign from its neighbors indicates a topological problem.
    let degen = 0, flipped = 0;
    const flippedDetails = [];
    for (const t of capTris) {
        const i0 = I[t * 3 + 0], i1 = I[t * 3 + 1], i2 = I[t * 3 + 2];
        const ax = P[i0*3], ay = P[i0*3+1], az = P[i0*3+2];
        const bx = P[i1*3], by = P[i1*3+1], bz = P[i1*3+2];
        const cx = P[i2*3], cy = P[i2*3+1], cz = P[i2*3+2];
        // Cross product for area / winding.
        const ex1 = bx - ax, ey1 = by - ay, ez1 = bz - az;
        const ex2 = cx - ax, ey2 = cy - ay, ez2 = cz - az;
        const nx = ey1 * ez2 - ez1 * ey2;
        const ny = ez1 * ex2 - ex1 * ez2;
        const nz = ex1 * ey2 - ey1 * ex2;
        const L = Math.hypot(nx, ny, nz);
        if (L < 1e-6) degen++;
        // For a top cap (normal +Y), winding-normal Y component should be > 0.
        else if (ny < -0.1) {
            flipped++;
            flippedDetails.push({
                t, v: [
                    [+ax.toFixed(3), +ay.toFixed(3), +az.toFixed(3)],
                    [+bx.toFixed(3), +by.toFixed(3), +bz.toFixed(3)],
                    [+cx.toFixed(3), +cy.toFixed(3), +cz.toFixed(3)],
                ],
                n: [+nx.toFixed(3), +ny.toFixed(3), +nz.toFixed(3)],
            });
        }
    }
    console.log(`  cap degen tris: ${degen}, flipped: ${flipped}`);
    for (const fd of flippedDetails) {
        console.log('  flipped tri', fd.t, 'verts:', JSON.stringify(fd.v), 'n:', JSON.stringify(fd.n));
    }
});

// -------------------------------------------------------------------------
// Wrap-up
// -------------------------------------------------------------------------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
