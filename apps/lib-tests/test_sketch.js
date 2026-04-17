// Tests for apps/lib/sketch.js.
//
// Run: bro-headless apps/lib-tests apps/lib-tests/test_sketch.js

'use strict';

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
function nearVec(a, b, eps, msg) {
    eps = eps || 1e-5;
    for (let i = 0; i < a.length; i++) {
        if (Math.abs(a[i] - b[i]) > eps) {
            throw new Error((msg || 'nearVec') + ' at [' + i + ']: ' + a + ' vs ' + b);
        }
    }
}
function truthy(v, msg) { if (!v) throw new Error(msg || 'expected truthy'); }
function falsy(v, msg)  { if (v)  throw new Error(msg || 'expected falsy'); }

// -------------------------------------------------------------------------
// Vector math
// -------------------------------------------------------------------------

t('v3 math basics', () => {
    eq(Sketch.v3add([1,2,3], [4,5,6]), [5,7,9]);
    eq(Sketch.v3sub([4,5,6], [1,2,3]), [3,3,3]);
    eq(Sketch.v3scale([1,2,3], 2),     [2,4,6]);
    eq(Sketch.v3dot([1,0,0], [1,0,0]), 1);
    eq(Sketch.v3dot([1,0,0], [0,1,0]), 0);
    eq(Sketch.v3cross([1,0,0], [0,1,0]), [0,0,1]);
    near(Sketch.v3len([3,4,0]), 5);
    nearVec(Sketch.v3norm([2,0,0]), [1,0,0]);
    near(Sketch.v3dist([0,0,0], [1,1,1]), Math.sqrt(3));
});

t('v3norm of zero returns zero, not NaN', () => {
    eq(Sketch.v3norm([0,0,0]), [0,0,0]);
});

// -------------------------------------------------------------------------
// Ray / plane
// -------------------------------------------------------------------------

t('rayToPlane: hits an axis-aligned plane', () => {
    const ray = { origin: [0,0,5], dir: [0,0,-1] };
    const p = Sketch.rayToPlane(ray, [0,0,0], [0,0,1]);
    nearVec(p, [0,0,0]);
});

t('rayToPlane: off-axis intersection', () => {
    const ray = { origin: [1,2,5], dir: [0,0,-1] };
    const p = Sketch.rayToPlane(ray, [0,0,0], [0,0,1]);
    nearVec(p, [1,2,0]);
});

t('rayToPlane: parallel ray returns null', () => {
    const ray = { origin: [0,1,0], dir: [1,0,0] };
    const p = Sketch.rayToPlane(ray, [0,0,0], [0,1,0]);
    eq(p, null);
});

t('rayToPlane: intersection behind origin returns null', () => {
    const ray = { origin: [0,0,-5], dir: [0,0,-1] };
    const p = Sketch.rayToPlane(ray, [0,0,0], [0,0,1]);
    eq(p, null);
});

// -------------------------------------------------------------------------
// Plane basis
// -------------------------------------------------------------------------

t('planeBasis: u, v orthonormal and both perpendicular to n', () => {
    const normals = [
        [0,0,1], [0,1,0], [1,0,0],
        [0.7071067811865476, 0.7071067811865476, 0],
        [0.57735, 0.57735, 0.57735],
    ];
    for (const n of normals) {
        const { u, v } = Sketch.planeBasis(n);
        near(Sketch.v3len(u), 1, 1e-5, `|u|=1 for n=${n}`);
        near(Sketch.v3len(v), 1, 1e-5, `|v|=1 for n=${n}`);
        near(Sketch.v3dot(u, v), 0, 1e-5, 'u ⟂ v');
        near(Sketch.v3dot(u, n), 0, 1e-5, 'u ⟂ n');
        near(Sketch.v3dot(v, n), 0, 1e-5, 'v ⟂ n');
    }
});

// -------------------------------------------------------------------------
// Project / unproject round-trip
// -------------------------------------------------------------------------

t('project → unproject round-trips on multiple planes', () => {
    // Drive the round-trip from 2D so the intermediate 3D is guaranteed
    // on-plane. An off-plane starting point's normal component is
    // (correctly) discarded by project2D and can't be reconstructed.
    const cases = [
        { normal: [0,1,0],                         origin: [0,5,0],   uv: [3, -2] },
        { normal: [1,0,0],                         origin: [7,0,0],   uv: [1, 4]  },
        { normal: [0.57735,0.57735,0.57735],       origin: [1,1,1],   uv: [2, -1] },
    ];
    for (const c of cases) {
        const { u, v } = Sketch.planeBasis(c.normal);
        const p3 = Sketch.unproject2Dto3D(c.uv, c.origin, u, v);
        const uv2 = Sketch.project3Dto2D(p3, c.origin, u, v);
        nearVec(uv2, c.uv, 1e-5, `round-trip n=${c.normal}`);
    }
});

// -------------------------------------------------------------------------
// axisLock / pickClosestAxis
// -------------------------------------------------------------------------

t('axisLock: snaps to axis direction', () => {
    const locked = Sketch.axisLock([0,0,0], [3, 0.5, -0.2], [1,0,0]);
    nearVec(locked, [3, 0, 0]);
});

t('axisLock: reverse direction works', () => {
    const locked = Sketch.axisLock([0,0,0], [-2, 1, 0], [1,0,0]);
    nearVec(locked, [-2, 0, 0]);
});

t('axisLock: handles non-unit axis', () => {
    const locked = Sketch.axisLock([0,0,0], [3, 0.5, -0.2], [2,0,0]);
    nearVec(locked, [3, 0, 0]);
});

t('pickClosestAxis: drag nearly along +X picks X', () => {
    const r = Sketch.pickClosestAxis([0,0,0], [3, 0.3, 0.1],
        [[1,0,0], [0,1,0], [0,0,1]]);
    eq(r.index, 0);
    truthy(r.alignment > 0.99);
});

t('pickClosestAxis: null on zero-length drag', () => {
    const r = Sketch.pickClosestAxis([1,2,3], [1,2,3],
        [[1,0,0], [0,1,0], [0,0,1]]);
    eq(r, null);
});

// -------------------------------------------------------------------------
// Rectangle from corners
// -------------------------------------------------------------------------

t('rectFromCorners: XY-plane square', () => {
    const u = [1,0,0], v = [0,1,0];
    const rect = Sketch.rectFromCorners([0,0,0], [2,3,0], u, v);
    eq(rect.length, 4);
    nearVec(rect[0], [0,0,0]);
    nearVec(rect[1], [2,0,0]);
    nearVec(rect[2], [2,3,0]);
    nearVec(rect[3], [0,3,0]);
});

t('rectFromCorners: handles negative-area (corner order flipped)', () => {
    // p0 = "top-right", p2 = "bottom-left" in uv → flipped winding.
    const u = [1,0,0], v = [0,1,0];
    const rect = Sketch.rectFromCorners([2,3,0], [0,0,0], u, v);
    eq(rect.length, 4);
    // Orientation swap keeps the quad planar and CCW-ish — the sequence
    // should still walk the four corners of a 2x3 rect with matching area.
    const signed = Sketch.polygonArea2D(rect.map(p => [
        Sketch.v3dot(Sketch.v3sub(p, [2,3,0]), u),
        Sketch.v3dot(Sketch.v3sub(p, [2,3,0]), v),
    ]));
    truthy(signed > 0, 'returned corners are CCW in (u,v)');
    near(Math.abs(signed), 6, 1e-5);
});

t('rectFromCorners: tilted plane produces 4 coplanar points', () => {
    const normal = Sketch.v3norm([1,1,1]);
    const { u, v } = Sketch.planeBasis(normal);
    const origin = [1,1,1];
    const p0 = origin;
    const p2 = Sketch.unproject2Dto3D([2, 3], origin, u, v);
    const rect = Sketch.rectFromCorners(p0, p2, u, v);
    for (const p of rect) {
        const d = Sketch.v3dot(Sketch.v3sub(p, origin), normal);
        near(d, 0, 1e-5, 'coplanar with the (u,v) plane');
    }
});

// -------------------------------------------------------------------------
// Circle polyline
// -------------------------------------------------------------------------

t('circlePolyline: emits N points at correct radius', () => {
    const pts = Sketch.circlePolyline([0,0,0], 2, [0,0,1], 16);
    eq(pts.length, 16);
    for (const p of pts) {
        near(Sketch.v3len(p), 2, 1e-5, '|p| = radius');
        near(p[2], 0, 1e-5, 'z = 0 on XY plane');
    }
});

t('circlePolyline: segments are equidistant', () => {
    const pts = Sketch.circlePolyline([0,0,0], 1, [0,0,1], 8);
    const d0 = Sketch.v3dist(pts[0], pts[1]);
    for (let i = 1; i < 8; i++) {
        const di = Sketch.v3dist(pts[i], pts[(i+1) % 8]);
        near(di, d0, 1e-5, 'segment lengths equal');
    }
});

t('circlePolyline: CCW as seen from +normal', () => {
    const pts = Sketch.circlePolyline([0,0,0], 1, [0,0,1], 4);
    const area = Sketch.polygonArea2D(pts.map(p => [p[0], p[1]]));
    truthy(area > 0, 'positive signed area = CCW');
});

t('circlePolyline: tilted plane stays planar', () => {
    const normal = Sketch.v3norm([1,2,3]);
    const pts = Sketch.circlePolyline([5,5,5], 1.5, normal, 24);
    for (const p of pts) {
        const d = Sketch.v3dot(Sketch.v3sub(p, [5,5,5]), normal);
        near(d, 0, 1e-5, 'each point on plane');
        near(Sketch.v3dist(p, [5,5,5]), 1.5, 1e-5, 'radius preserved');
    }
});

// -------------------------------------------------------------------------
// polygonArea2D
// -------------------------------------------------------------------------

t('polygonArea2D: CCW square = +1', () => {
    near(Sketch.polygonArea2D([[0,0],[1,0],[1,1],[0,1]]), 1);
});
t('polygonArea2D: CW square = -1', () => {
    near(Sketch.polygonArea2D([[0,0],[0,1],[1,1],[1,0]]), -1);
});
t('polygonArea2D: accepts flat arrays', () => {
    near(Sketch.polygonArea2D([0,0, 1,0, 1,1, 0,1]), 1);
});
t('polygonArea2D: L-shape (area 3)', () => {
    near(Sketch.polygonArea2D([[0,0],[2,0],[2,1],[1,1],[1,2],[0,2]]), 3);
});
t('polygonArea2D: degenerate (2 pts) → 0', () => {
    near(Sketch.polygonArea2D([[0,0],[1,1]]), 0);
});

// -------------------------------------------------------------------------
// polylineLength3D + flatten3D
// -------------------------------------------------------------------------

t('polylineLength3D: open and closed', () => {
    const pts = [[0,0,0], [3,0,0], [3,4,0]];
    near(Sketch.polylineLength3D(pts, false), 7);
    near(Sketch.polylineLength3D(pts, true), 7 + 5);   // back to origin
});

t('flatten3D: correct layout', () => {
    const pts = [[1,2,3],[4,5,6]];
    const flat = Sketch.flatten3D(pts);
    eq(flat.length, 6);
    near(flat[0], 1); near(flat[1], 2); near(flat[2], 3);
    near(flat[3], 4); near(flat[4], 5); near(flat[5], 6);
});

// -------------------------------------------------------------------------
// Integration with Mesh.polygon3D
// -------------------------------------------------------------------------

t('integration: circlePolyline → Mesh.polygon3D → area ≈ π r²', () => {
    const radius = 1.5;
    const pts = Sketch.circlePolyline([0,0,0], radius, [0,0,1], 64);
    const flat = Sketch.flatten3D(pts);
    const mesh = Mesh.polygon3D(flat, [], [0,0,1]);
    eq(mesh.vertexCount, 64);
    eq(mesh.triangleCount, 62);   // n-2 for a simple polygon
    let area = 0;
    for (let t = 0; t < mesh.indices.length; t += 3) {
        const a = mesh.indices[t+0], b = mesh.indices[t+1], c = mesh.indices[t+2];
        const ax = mesh.positions[a*3], ay = mesh.positions[a*3+1];
        const bx = mesh.positions[b*3], by = mesh.positions[b*3+1];
        const cx = mesh.positions[c*3], cy = mesh.positions[c*3+1];
        area += 0.5 * Math.abs((bx-ax)*(cy-ay) - (cx-ax)*(by-ay));
    }
    // 64-segment approximation of π·r² = π·2.25 ≈ 7.0686. Allow 1% slack.
    near(area, Math.PI * radius * radius, 0.08, 'circle area ≈ πr²');
});

// -------------------------------------------------------------------------
// splitSelfIntersectingPolygon / hasSelfIntersections
// -------------------------------------------------------------------------

t('hasSelfIntersections: simple polygon = false', () => {
    if (Sketch.hasSelfIntersections([[0,0],[1,0],[1,1],[0,1]])) {
        throw new Error('unit square should not self-intersect');
    }
});

t('hasSelfIntersections: figure-8 = true', () => {
    if (!Sketch.hasSelfIntersections([[0,0],[2,2],[2,0],[0,2]])) {
        throw new Error('figure-8 should self-intersect');
    }
});

t('splitSelfIntersectingPolygon: simple polygon → one loop', () => {
    const loops = Sketch.splitSelfIntersectingPolygon(
        [[0,0],[1,0],[0.5,1]]);
    eq(loops.length, 1);
    eq(loops[0].length, 3);
});

t('splitSelfIntersectingPolygon: reverses CW input to CCW', () => {
    const loops = Sketch.splitSelfIntersectingPolygon(
        [[0,0],[0.5,1],[1,0]]);
    eq(loops.length, 1);
    if (Sketch.polygonArea2D(loops[0]) <= 0) {
        throw new Error('output must be CCW');
    }
});

t('splitSelfIntersectingPolygon: figure-8 → two triangles', () => {
    const loops = Sketch.splitSelfIntersectingPolygon(
        [[0,0],[2,2],[2,0],[0,2]]);
    eq(loops.length, 2);
    for (const l of loops) {
        eq(l.length, 3);
        if (Sketch.polygonArea2D(l) <= 0) throw new Error('each loop CCW');
    }
});

t('splitSelfIntersectingPolygon: bowtie → two loops', () => {
    const loops = Sketch.splitSelfIntersectingPolygon(
        [[0,0],[1,0],[0,1],[1,1]]);
    eq(loops.length, 2);
});

// -------------------------------------------------------------------------
// End
// -------------------------------------------------------------------------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
