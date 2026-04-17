// Headless tests for EdgeMesh + the edges scene node.
// Run: bro-headless apps/scene-editor apps/scene-editor/test_edges.js

advanceTime(100);
flush();

const E = window.__editor;
const EM = window.EdgeMesh;
assert(EM, 'EdgeMesh module is loaded');

// --- orthoBasis --------------------------------------------------------------
//
// The returned {u, v} must be unit, orthogonal to each other and to the input
// direction. No specific rotation is promised — the prism is rotationally
// symmetric around its length axis.

{
    const dirs = [
        [1, 0, 0], [0, 1, 0], [0, 0, 1],
        [1, 1, 1], [1, 2, 3], [-0.3, 0.7, -0.5],
    ];
    for (const d of dirs) {
        const L = Math.hypot(d[0], d[1], d[2]);
        const w = [d[0]/L, d[1]/L, d[2]/L];
        const [ux, uy, uz, vx, vy, vz] = EM.orthoBasis(w);
        const ul = Math.hypot(ux, uy, uz);
        const vl = Math.hypot(vx, vy, vz);
        assert(Math.abs(ul - 1) < 1e-6, `|u| = 1 (got ${ul})`);
        assert(Math.abs(vl - 1) < 1e-6, `|v| = 1 (got ${vl})`);
        const uw = ux*w[0] + uy*w[1] + uz*w[2];
        const vw = vx*w[0] + vy*w[1] + vz*w[2];
        const uv = ux*vx + uy*vy + uz*vz;
        assert(Math.abs(uw) < 1e-6, `u ⟂ w (got ${uw})`);
        assert(Math.abs(vw) < 1e-6, `v ⟂ w (got ${vw})`);
        assert(Math.abs(uv) < 1e-6, `u ⟂ v (got ${uv})`);
    }
}

// --- buildEdgeMesh geometry --------------------------------------------------

{
    // One axis-aligned edge from (0,0,0) to (1,0,0).
    const positions = new Float32Array([0,0,0, 1,0,0]);
    const edges = [{ a: 0, b: 1 }];
    const data = EM.buildEdgeMesh(positions, edges, { thickness: 0.1 });
    // 24 verts (4 per face × 6 faces) per edge.
    assert(data.positions.length === 24 * 3,
        `one edge → 24 positions (got ${data.positions.length / 3})`);
    assert(data.normals.length === 24 * 3, `one edge → 24 normals`);
    assert(data.colors.length === 24 * 4, `one edge → 24 RGBA colors`);
    assert(data.indices.length === 36,
        `one edge → 12 tris (got ${data.indices.length / 3})`);

    // All vertices inside [-ht, 1+ht] bbox, with ht=0.05 for a 0.1-thick
    // prism — the end caps push out past each endpoint by half-thickness.
    for (let i = 0; i < 24; i++) {
        const x = data.positions[i*3 + 0];
        const y = data.positions[i*3 + 1];
        const z = data.positions[i*3 + 2];
        assert(x >= -0.05 - 1e-6 && x <= 1.05 + 1e-6,
            `vert x in [-.05, 1.05] (got ${x})`);
        assert(y >= -0.05 - 1e-6 && y <= 0.05 + 1e-6, `vert y tight (got ${y})`);
        assert(z >= -0.05 - 1e-6 && z <= 0.05 + 1e-6, `vert z tight (got ${z})`);
    }

    // Normal length is unit everywhere.
    for (let i = 0; i < 24; i++) {
        const nx = data.normals[i*3 + 0];
        const ny = data.normals[i*3 + 1];
        const nz = data.normals[i*3 + 2];
        const nl = Math.hypot(nx, ny, nz);
        assert(Math.abs(nl - 1) < 1e-5, `normal[${i}] unit length (got ${nl})`);
    }

    // Winding check: for every triangle, (v1-v0) × (v2-v1) must dot the
    // stored normal positively (outward) — required for GL_BACK culling to
    // show the outer surface.
    for (let t = 0; t < data.indices.length / 3; t++) {
        const i0 = data.indices[t*3 + 0];
        const i1 = data.indices[t*3 + 1];
        const i2 = data.indices[t*3 + 2];
        const p0 = [data.positions[i0*3], data.positions[i0*3+1], data.positions[i0*3+2]];
        const p1 = [data.positions[i1*3], data.positions[i1*3+1], data.positions[i1*3+2]];
        const p2 = [data.positions[i2*3], data.positions[i2*3+1], data.positions[i2*3+2]];
        const ax = p1[0]-p0[0], ay = p1[1]-p0[1], az = p1[2]-p0[2];
        const bx = p2[0]-p1[0], by = p2[1]-p1[1], bz = p2[2]-p1[2];
        const cx = ay*bz - az*by;
        const cy = az*bx - ax*bz;
        const cz = ax*by - ay*bx;
        const n = [data.normals[i0*3], data.normals[i0*3+1], data.normals[i0*3+2]];
        const dot = cx*n[0] + cy*n[1] + cz*n[2];
        assert(dot > 0, `tri ${t} outward-wound (dot=${dot.toFixed(4)})`);
    }
}

// --- buildEdgeMesh handles a diagonal direction ------------------------------

{
    const positions = new Float32Array([0,0,0, 1,1,1]);
    const data = EM.buildEdgeMesh(positions, [{ a: 0, b: 1 }], { thickness: 0.02 });
    assert(data.positions.length === 24 * 3, 'diagonal edge: 24 positions');
    // Diagonal edge length is √3, so some vertex must sit well past the
    // axis-aligned bbox a naive emitter would have produced.
    let maxAxisProj = 0;
    for (let i = 0; i < 24; i++) {
        const dx = data.positions[i*3 + 0];
        const dy = data.positions[i*3 + 1];
        const dz = data.positions[i*3 + 2];
        // Project onto edge direction (1,1,1)/√3.
        const p = (dx + dy + dz) / Math.sqrt(3);
        if (p > maxAxisProj) maxAxisProj = p;
    }
    const edgeLen = Math.sqrt(3);
    assert(maxAxisProj > edgeLen - 1e-3,
        `diagonal prism extends to edge length (got ${maxAxisProj} vs ${edgeLen})`);
}

// --- Empty input returns an empty mesh ---------------------------------------

{
    const data = EM.buildEdgeMesh(new Float32Array([]), [], { thickness: 0.01 });
    assert(data.positions.length === 0, 'no edges → no positions');
    assert(data.indices.length === 0, 'no edges → no indices');
}

// --- Edges scene node wired into app.js --------------------------------------

assert(E.edgesNode, 'app exposes edgesNode after initial build');
// Cube has 12 edges → 12 × 24 = 288 verts. Node's mesh isn't exposed as a
// JS object, but we can at least confirm a node exists and survived the
// rebuild path.

// Push/pull commit triggers rebuildMeshState() → rebuildEdgesNode(). The node
// identity may change across rebuilds (destroy + recreate), but .edgesNode
// must still resolve to a live node afterward.
const before = E.edgesNode;
assert(before, 'edges node exists before push/pull');

const topGroup = E.faceGroups.groups.find(g => g.normal[1] > 0.999);
assert(topGroup, 'cube has a +Y face group');
const topTri = topGroup.tris[0];

function triCentroid(triIdx) {
    const i0 = E.boxIndices[triIdx * 3 + 0];
    const i1 = E.boxIndices[triIdx * 3 + 1];
    const i2 = E.boxIndices[triIdx * 3 + 2];
    const P = E.boxPositions;
    return [
        (P[i0*3+0] + P[i1*3+0] + P[i2*3+0]) / 3,
        (P[i0*3+1] + P[i1*3+1] + P[i2*3+1]) / 3,
        (P[i0*3+2] + P[i1*3+2] + P[i2*3+2]) / 3,
    ];
}
E.beginPushPull({
    triangleIndex: topTri,
    position: triCentroid(topTri),
    normal: topGroup.normal.slice(),
    distance: 0,
});
E.applyPushPull(0.5);
E.commitPushPull();

assert(E.edgesNode, 'edges node re-created after push/pull commit');
assert(E.edgesNode !== before, 'edges node identity replaced on rebuild');

// After a +Y extrusion via SketchUp-style surgery the cube collapses
// cleanly into a taller cube: 4 new top + 4 bottom + 4 vertical = 12
// model edges. Side faces retriangulate with their top corners snapped
// to the moved cap, so there is no y=1 waist seam.
assert(E.inferenceGeo.edges.length === 12,
    `post-commit pulled-cube has 12 edges (got ${E.inferenceGeo.edges.length})`);

screenshot('apps/scene-editor/_edges_after.png');
console.log(`OK — EdgeMesh: orthoBasis ⟂, prism winding outward, diagonal edges supported; ` +
            `edges scene node survives rebuild`);
