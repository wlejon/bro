// JS-binding smoke test for bromesh's triangulatePolygon.
//
// Run: bro-headless apps/lib-tests apps/lib-tests/test_polygon.js

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
function truthy(v, msg) { if (!v) throw new Error(msg || 'expected truthy'); }

function totalArea2D(m) {
    let a = 0;
    const pos = m.positions;
    const idx = m.indices;
    for (let t = 0; t < idx.length; t += 3) {
        const ax = pos[idx[t+0]*3+0], ay = pos[idx[t+0]*3+1];
        const bx = pos[idx[t+1]*3+0], by = pos[idx[t+1]*3+1];
        const cx = pos[idx[t+2]*3+0], cy = pos[idx[t+2]*3+1];
        a += 0.5 * Math.abs((bx-ax)*(cy-ay) - (cx-ax)*(by-ay));
    }
    return a;
}
function totalArea3D(m) {
    let a = 0;
    const pos = m.positions;
    const idx = m.indices;
    for (let t = 0; t < idx.length; t += 3) {
        const ax=pos[idx[t+0]*3+0], ay=pos[idx[t+0]*3+1], az=pos[idx[t+0]*3+2];
        const bx=pos[idx[t+1]*3+0], by=pos[idx[t+1]*3+1], bz=pos[idx[t+1]*3+2];
        const cx=pos[idx[t+2]*3+0], cy=pos[idx[t+2]*3+1], cz=pos[idx[t+2]*3+2];
        const ex=bx-ax, ey=by-ay, ez=bz-az;
        const fx=cx-ax, fy=cy-ay, fz=cz-az;
        const crx=ey*fz-ez*fy, cry=ez*fx-ex*fz, crz=ex*fy-ey*fx;
        a += 0.5 * Math.sqrt(crx*crx + cry*cry + crz*crz);
    }
    return a;
}

// -------------------------------------------------------------------------
// 2D — plain number arrays
// -------------------------------------------------------------------------

t('polygon2D: unit square from plain array', () => {
    const m = Mesh.polygon2D([0,0, 1,0, 1,1, 0,1]);
    eq(m.vertexCount, 4);
    eq(m.triangleCount, 2);
    near(totalArea2D(m), 1.0);
});

t('polygon2D: Float32Array input works', () => {
    const m = Mesh.polygon2D(new Float32Array([0,0, 2,0, 2,1, 0,1]));
    eq(m.vertexCount, 4);
    eq(m.triangleCount, 2);
    near(totalArea2D(m), 2.0);
});

t('polygon2D: concave L-shape triangulates fully', () => {
    const m = Mesh.polygon2D([0,0, 2,0, 2,1, 1,1, 1,2, 0,2]);
    eq(m.vertexCount, 6);
    // n-2 for a simple polygon with n verts.
    eq(m.triangleCount, 4);
    near(totalArea2D(m), 3.0);
});

t('polygon2D: square with square hole — 8 tris, area 3', () => {
    const outer = [0,0, 2,0, 2,2, 0,2];
    const hole  = [0.5,0.5, 0.5,1.5, 1.5,1.5, 1.5,0.5];
    const m = Mesh.polygon2D(outer, [hole]);
    eq(m.vertexCount, 8);
    eq(m.triangleCount, 8);
    near(totalArea2D(m), 3.0);
});

t('polygon2D: z offset applied uniformly', () => {
    const m = Mesh.polygon2D([0,0, 1,0, 1,1, 0,1], [], 2.5);
    for (let i = 0; i < m.vertexCount; i++) {
        near(m.positions[i*3 + 2], 2.5);
    }
});

t('polygon2D: CCW winding gives +Z normals', () => {
    const m = Mesh.polygon2D([0,0, 1,0, 1,1, 0,1]);
    near(m.normals[2], 1.0);
});

t('polygon2D: CW winding gives -Z normals', () => {
    const m = Mesh.polygon2D([0,0, 0,1, 1,1, 1,0]);
    near(m.normals[2], -1.0);
});

t('polygon2D: degenerate (2 pts) returns empty', () => {
    const m = Mesh.polygon2D([0,0, 1,1]);
    eq(m.vertexCount, 0);
    eq(m.triangleCount, 0);
});

// -------------------------------------------------------------------------
// 3D
// -------------------------------------------------------------------------

t('polygon3D: square on XZ plane (normal +Y)', () => {
    const m = Mesh.polygon3D(
        [0,5,0, 1,5,0, 1,5,1, 0,5,1],
        [],
        [0, 1, 0]);
    eq(m.vertexCount, 4);
    eq(m.triangleCount, 2);
    near(totalArea3D(m), 1.0);
    // Positions verbatim.
    near(m.positions[0], 0); near(m.positions[1], 5); near(m.positions[2], 0);
    // Normals all +Y.
    near(m.normals[1], 1.0);
});

t('polygon3D: tilted plane preserves input positions, area 1', () => {
    const inv = 1 / Math.sqrt(3);
    const normal = [inv, inv, inv];
    // Build a unit square on the plane spanned by two orthonormal tangents.
    const u = [1/Math.sqrt(2), -1/Math.sqrt(2), 0];
    const v = [1/Math.sqrt(6),  1/Math.sqrt(6), -2/Math.sqrt(6)];
    const pt = (a, b) => [
        a*u[0] + b*v[0],
        a*u[1] + b*v[1],
        a*u[2] + b*v[2],
    ];
    const [p0, p1, p2, p3] = [pt(0,0), pt(1,0), pt(1,1), pt(0,1)];
    const outer = [...p0, ...p1, ...p2, ...p3];
    const m = Mesh.polygon3D(outer, [], normal);
    eq(m.triangleCount, 2);
    near(totalArea3D(m), 1.0);
    // Positions verbatim.
    for (let i = 0; i < 4; i++) {
        near(m.positions[i*3 + 0], outer[i*3 + 0]);
        near(m.positions[i*3 + 1], outer[i*3 + 1]);
        near(m.positions[i*3 + 2], outer[i*3 + 2]);
    }
    // Normals carry caller's vector.
    for (let i = 0; i < 4; i++) {
        near(m.normals[i*3 + 0], normal[0]);
        near(m.normals[i*3 + 1], normal[1]);
        near(m.normals[i*3 + 2], normal[2]);
    }
});

t('polygon3D: degenerate input returns empty', () => {
    const m = Mesh.polygon3D([0,0,0, 1,0,0], [], [0,1,0]);
    eq(m.vertexCount, 0);
});

// -------------------------------------------------------------------------
// End
// -------------------------------------------------------------------------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
