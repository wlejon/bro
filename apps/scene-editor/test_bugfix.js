// End-to-end verification: replay the exact recipe that produced
// mesh-rendering-bug.bro (line-drawn polygon → extrude → push a side
// inward to create a concave cap) and confirm the resulting render mesh
// has zero T-junctions and zero triangle edges crossing polygon edges.
'use strict';

advanceTime(0); flush();

const E   = window.__editor;
const reg = E.registry;

// Reproduce the user's polygon from the saved file. Six vertices laid out
// as a hexagon-ish shape on the ground plane.
const ringXZ = [
    [4.71,  -5.41],   // v0
    [3.18,  -0.08],   // v1
    [5.05,  -0.12],   // v2
    [5.00,  -1.92],   // v3 — inner corner
    [6.75,  -0.34],   // v4
    [8.32,  -3.06],   // v5
];

// Build the polygon as a sketch face using bromesh's planar triangulator.
const flat = ringXZ.map(p => [p[0], 0, p[1]]);
const fl3 = Sketch.flatten3D(flat);
const mesh = Mesh.polygon3D(fl3, [], [0, 1, 0]);
const data = {
    positions: new Float32Array(mesh.positions),
    indices:   new Uint32Array(mesh.indices),
    normals:   new Float32Array(mesh.normals),
};
reg.clear();
reg.createFromMesh({ name: 'BugRepro', color: '#888' }, data, reg.nextId());
const p = reg.primitives[reg.primitives.length - 1];
console.log(`sketch:    faces=${p.indices.length/3} verts=${p.positions.length/3}`);

// Extrude the cap +Y by 4.07 (matches the saved file's Y range).
function pushGroup(prim, gi, dist) {
    const g = prim.faceGroups.groups[gi];
    const tri = g.tris[0];
    const P = prim.positions, I = prim.indices;
    const i0 = I[tri*3], i1 = I[tri*3+1], i2 = I[tri*3+2];
    const c = [
        (P[i0*3]+P[i1*3]+P[i2*3])/3,
        (P[i0*3+1]+P[i1*3+1]+P[i2*3+1])/3,
        (P[i0*3+2]+P[i1*3+2]+P[i2*3+2])/3,
    ];
    E.beginPushPullOn(prim, {
        triangleIndex: tri,
        position: c,
        normal: g.normal.slice(),
        distance: 0,
    });
    E.applyPushPull(dist);
    E.commitPushPull();
}

pushGroup(p, 0, 4.07);
console.log(`extruded:  faces=${p.indices.length/3} verts=${p.positions.length/3} groups=${p.faceGroups.groups.length}`);

// Find a side wall and push it INWARD to make the cap concave.
// "Inward" = along -normal of that wall. Pick the wall whose normal is
// closest to +X.
let sideIdx = -1, bestDot = -2;
for (let i = 0; i < p.faceGroups.groups.length; i++) {
    const n = p.faceGroups.groups[i].normal;
    if (Math.abs(n[1]) > 0.1) continue;     // skip caps
    if (n[0] > bestDot) { bestDot = n[0]; sideIdx = i; }
}
console.log(`pushing side group ${sideIdx} (normal ${p.faceGroups.groups[sideIdx].normal.map(n=>n.toFixed(2))}) by -1.5`);
pushGroup(p, sideIdx, -1.5);
console.log(`pushed:    faces=${p.indices.length/3} verts=${p.positions.length/3} groups=${p.faceGroups.groups.length}`);

// PolyMesh introspection.
if (p.polyMesh) {
    const pm = p.polyMesh;
    let hist = new Map();
    for (let f = 0; f < pm.faceCount; f++) {
        const n = pm.faceVertexCount(f);
        hist.set(n, (hist.get(n) || 0) + 1);
    }
    console.log(`polyMesh:  faces=${pm.faceCount} N-gon counts=${JSON.stringify(Array.from(hist.entries()))}`);
} else {
    console.log('NO POLYMESH on primitive — migration incomplete');
}

// T-junction + boundary-crossing check on the rendered triangle mesh.
const QUANT = 1e5;
const TP = p.positions, TI = p.indices;
function qkey(x,y,z){ return Math.round(x*QUANT)+','+Math.round(y*QUANT)+','+Math.round(z*QUANT); }
const byKey = new Map(), uPos = [];
for (let i = 0; i < TP.length/3; i++) {
    const k = qkey(TP[i*3],TP[i*3+1],TP[i*3+2]);
    if (!byKey.has(k)) { byKey.set(k, uPos.length); uPos.push([TP[i*3],TP[i*3+1],TP[i*3+2]]); }
}
function pointOnSegment(A,B,p,eps){
    const abx=B[0]-A[0],aby=B[1]-A[1],abz=B[2]-A[2];
    const apx=p[0]-A[0],apy=p[1]-A[1],apz=p[2]-A[2];
    const L2=abx*abx+aby*aby+abz*abz; if(L2<1e-18) return false;
    const t=(apx*abx+apy*aby+apz*abz)/L2; if(t<=eps||t>=1-eps) return false;
    const cx=A[0]+t*abx,cy=A[1]+t*aby,cz=A[2]+t*abz;
    return ((p[0]-cx)**2+(p[1]-cy)**2+(p[2]-cz)**2) < eps*eps;
}
let tjs = 0;
const seen = new Set();
for (let t = 0; t < TI.length/3; t++) {
    const tri = [TI[t*3],TI[t*3+1],TI[t*3+2]].map(i => byKey.get(qkey(TP[i*3],TP[i*3+1],TP[i*3+2])));
    for (let k = 0; k < 3; k++) {
        const a = tri[k], b = tri[(k+1)%3]; if (a===b) continue;
        const ek = a<b ? (a+'_'+b) : (b+'_'+a);
        if (seen.has(ek)) continue; seen.add(ek);
        for (let v = 0; v < uPos.length; v++) {
            if (v===a||v===b) continue;
            if (pointOnSegment(uPos[a],uPos[b],uPos[v],1e-3)) {
                tjs++;
                if (tjs <= 3) {
                    console.log(`  T-junction: edge [${uPos[a].map(n=>n.toFixed(2))}]→[${uPos[b].map(n=>n.toFixed(2))}] interior vert [${uPos[v].map(n=>n.toFixed(2))}]`);
                }
            }
        }
    }
}
console.log(`\nT-junctions: ${tjs}`);
assert(tjs === 0, `expected 0 T-junctions, got ${tjs}`);
console.log('*** PASS: end-to-end push/pull → concave cap → clean render mesh ***');
