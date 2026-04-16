// EditMesh unit tests. No scene/canvas needed — exercises the half-edge DS
// directly. Run: bro-headless apps/scene-editor apps/scene-editor/test_editmesh.js

// --- Box round-trip ---------------------------------------------------------

// Use local names that don't shadow app.js globals (index.html loads app.js
// before this, so `boxMesh` is already declared at top level).
const testBox = Mesh.box(1, 1, 1);
const positions0 = testBox.positions;
const indices0   = testBox.indices;
const triCount0  = testBox.triangleCount;
const vertCount0 = testBox.vertexCount;

console.log(`box: ${vertCount0} verts, ${triCount0} tris`);

const em = EditMesh.fromMeshData(positions0, indices0);

assert(em.vertices.length === vertCount0,
    `edit mesh vertex count matches (${em.vertices.length} vs ${vertCount0})`);
assert(em.faces.length === triCount0,
    `edit mesh face count matches (${em.faces.length} vs ${triCount0})`);
assert(em.halfEdges.length === triCount0 * 3,
    `edit mesh half-edge count == 3 * faces (${em.halfEdges.length})`);

// Every vertex should have an outgoing half-edge.
for (let i = 0; i < em.vertices.length; i++) {
    assert(em.vertices[i].halfEdge, `vertex ${i} has an outgoing half-edge`);
}

const val = EditMesh.validate(em);
assert(val.ok, `validate reports ok (errors: ${val.errors.join('; ')})`);
assert(val.isClosed,
    `box is closed manifold (${val.boundaryHalfEdges} unmatched boundary half-edges)`);

// --- Round-trip equivalence -------------------------------------------------
//
// Serialize back; the resulting triangles must describe the same set of
// position-triples (as sets, since vertex and triangle ordering need not be
// preserved by the DS).

const rt = EditMesh.toMeshData(em);
assert(rt.positions.length === positions0.length,
    `round-trip position count matches`);
assert(rt.indices.length === indices0.length,
    `round-trip index count matches`);

function triangleKeySet(positions, indices) {
    const keys = new Set();
    function key(vi) {
        return Math.round(positions[vi * 3 + 0] * 1e5) + ',' +
               Math.round(positions[vi * 3 + 1] * 1e5) + ',' +
               Math.round(positions[vi * 3 + 2] * 1e5);
    }
    const triCount = indices.length / 3;
    for (let t = 0; t < triCount; t++) {
        const k = [key(indices[t*3]), key(indices[t*3+1]), key(indices[t*3+2])];
        // Sort the three endpoint-keys so winding flips don't matter for
        // set-equality (this test is about topology, not orientation).
        k.sort();
        keys.add(k.join('|'));
    }
    return keys;
}

const origTris = triangleKeySet(positions0, indices0);
const rtTris   = triangleKeySet(rt.positions, rt.indices);
assert(origTris.size === rtTris.size,
    `same number of distinct triangle-as-position-set`);
for (const k of origTris) {
    assert(rtTris.has(k), `round-trip preserves triangle ${k}`);
}

// --- Icosahedron: simplest closed mesh --------------------------------------
//
// 20 tris, 12 verts, 30 edges, 60 half-edges. Euler: V - E + F = 2.

const icoMesh = Mesh.icosahedron();
const emIco   = EditMesh.fromMeshData(icoMesh.positions, icoMesh.indices);
const valIco  = EditMesh.validate(emIco);
assert(valIco.ok, `icosahedron validate ok: ${valIco.errors.join('; ')}`);
assert(valIco.isClosed,
    `icosahedron is closed (boundary=${valIco.boundaryHalfEdges})`);
assert(emIco.halfEdges.length === 60,
    `icosahedron has 60 half-edges (got ${emIco.halfEdges.length})`);

// --- UV sphere: closed with seam duplicates ---------------------------------
//
// Historically broken (bromesh's sphere emitted degenerate pole triangles and
// (segments+1) duplicate pole verts). Now a single-vertex pole with a proper
// fan of triangles, so the mesh is a clean closed manifold. Seam duplicates
// remain (s=0 and s=segments at same position, distinct UVs) and close up in
// the position-based twin pass.

const sphMesh = Mesh.sphere(1, 16, 12);
const emSph   = EditMesh.fromMeshData(sphMesh.positions, sphMesh.indices);
const valSph  = EditMesh.validate(emSph);
assert(valSph.ok, `sphere validate ok: ${valSph.errors.join('; ')}`);
assert(valSph.isClosed,
    `sphere is closed (boundary=${valSph.boundaryHalfEdges})`);

// --- Plane smoke test: open boundary ----------------------------------------
//
// A flat plane isn't closed; it should report exactly the perimeter as
// boundary half-edges. For a plane with subdivX×subdivZ = 1×1 it's 4 boundary
// half-edges (the four outer edges).

const planeMesh = Mesh.plane(1, 1, 1, 1);
const emPlane   = EditMesh.fromMeshData(planeMesh.positions, planeMesh.indices);
const valPlane  = EditMesh.validate(emPlane);
assert(valPlane.ok, `plane validate ok: ${valPlane.errors.join('; ')}`);
assert(!valPlane.isClosed, `plane is open`);
assert(valPlane.boundaryHalfEdges === 4,
    `plane has 4 boundary half-edges (got ${valPlane.boundaryHalfEdges})`);

console.log(`OK — EditMesh round-trips box (${em.vertices.length}v/${em.faces.length}f), ` +
            `validates icosahedron (${emIco.vertices.length}v/${emIco.faces.length}f) and ` +
            `sphere (${emSph.vertices.length}v/${emSph.faces.length}f) as closed, ` +
            `plane (${emPlane.vertices.length}v/${emPlane.faces.length}f) has 4 boundary edges`);
