// Push/pull regression coverage. Guards against "edges opening up" bugs
// where vertex-substitution surgery drops twin pairings.
//
// The screenshot bug: Line-drawn triangle → first push makes a prism, then
// push on a side wall silently broke the manifold (~12 unpaired HEs).
//
// Each test: build a shape, do a sequence of push/pulls, check that the
// resulting mesh is still a closed manifold (boundaryHalfEdges == 0).

advanceTime(0);
flush();

const E = window.__editor;
const reg = E.registry;

let tests = 0, failed = 0;
function t(name, fn) {
    tests++;
    try { fn(); console.log(`  ok   ${name}`); }
    catch (e) { failed++; console.log(`  FAIL ${name}: ${e.message}`); }
}

function pushGroup(p, gi, dist) {
    const g = p.faceGroups.groups[gi];
    const tri = g.tris[0];
    const P = p.positions, I = p.indices;
    const i0 = I[tri*3], i1 = I[tri*3+1], i2 = I[tri*3+2];
    E.beginPushPull({
        triangleIndex: tri,
        position: [
            (P[i0*3]+P[i1*3]+P[i2*3])/3,
            (P[i0*3+1]+P[i1*3+1]+P[i2*3+1])/3,
            (P[i0*3+2]+P[i1*3+2]+P[i2*3+2])/3,
        ],
        normal: g.normal.slice(),
        distance: 0,
    });
    E.applyPushPull(dist);
    E.commitPushPull();
}
function assertManifold(p, label) {
    const em = EditMesh.fromMeshData(p.positions, p.indices);
    const v = EditMesh.validate(em);
    if (!v.isClosed) {
        throw new Error(`${label}: ${v.boundaryHalfEdges} unpaired HEs`);
    }
}
function makePolygon(points2D, name) {
    const polygon3D = points2D.map(p => [p[0], 0, p[1]]);
    const flat = Sketch.flatten3D(polygon3D);
    const mesh = Mesh.polygon3D(flat, [], [0, 1, 0]);
    const data = {
        positions: new Float32Array(mesh.positions),
        indices:   new Uint32Array(mesh.indices),
        normals:   new Float32Array(mesh.normals),
    };
    reg.createFromMesh({ name, color: '#888' }, data, reg.nextId());
    return reg.primitives[reg.primitives.length - 1];
}
function topGroup(p)  { return p.faceGroups.groups.findIndex(g => g.normal[1] >  0.9); }
function sideGroup(p) { return p.faceGroups.groups.findIndex(g => Math.abs(g.normal[1]) < 0.1); }

// ---------------------------------------------------------------------------

t('triangle sketch: extrude then side-push (the screenshot bug)', () => {
    reg.clear();
    const p = makePolygon([[0,0],[1,0],[0.5,1]], 'Tri');
    pushGroup(p, 0, 1.0);
    assertManifold(p, 'after extrude');
    pushGroup(p, sideGroup(p), 2.38);
    assertManifold(p, 'after side push');
    pushGroup(p, topGroup(p), 0.5);
    assertManifold(p, 'after third push');
});

t('L-shape sketch: extrude then push each side in turn', () => {
    reg.clear();
    const p = makePolygon(
        [[0,0],[2,0],[2,1],[1,1],[1,2],[0,2]], 'L');
    pushGroup(p, 0, 1.0);
    assertManifold(p, 'after extrude');
    // Push each side wall a little.
    for (let pass = 0; pass < 3; pass++) {
        const si = sideGroup(p);
        if (si < 0) break;
        pushGroup(p, si, 0.1);
        assertManifold(p, `after side push pass ${pass}`);
    }
});

t('pentagon sketch: extrude then top push', () => {
    reg.clear();
    const verts = [];
    for (let i = 0; i < 5; i++) {
        const a = -Math.PI/2 + i * 2*Math.PI/5;
        verts.push([Math.cos(a), Math.sin(a)]);
    }
    const p = makePolygon(verts, 'Pent');
    pushGroup(p, 0, 0.8);
    assertManifold(p, 'after extrude');
    pushGroup(p, topGroup(p), 0.5);
    assertManifold(p, 'after top push');
});

t('box: top push, side push, repeat', () => {
    reg.clear();
    reg.create({ type: 'box', name: 'b', params: { sx: 2, sy: 2, sz: 2 } });
    const b = reg.primitives[0];
    pushGroup(b, topGroup(b), 1.5);
    assertManifold(b, 'after top push');
    pushGroup(b, sideGroup(b), 0.7);
    assertManifold(b, 'after side push');
    pushGroup(b, topGroup(b), -0.3);
    assertManifold(b, 'after top inward push');
});

t('cylinder: cap push and facet push', () => {
    reg.clear();
    reg.create({ type: 'cylinder', name: 'c',
                 params: { r: 1, h: 1, seg: 16 } });
    const c = reg.primitives[0];
    pushGroup(c, topGroup(c), 0.8);
    assertManifold(c, 'after cap push');
    pushGroup(c, sideGroup(c), 0.5);
    assertManifold(c, 'after side push');
    pushGroup(c, sideGroup(c), -0.2);
    assertManifold(c, 'after side inward');
});

t('cylinder: push adjacent facets in succession', () => {
    reg.clear();
    reg.create({ type: 'cylinder', name: 'c',
                 params: { r: 1, h: 1, seg: 12 } });
    const c = reg.primitives[0];
    // Push 4 adjacent side facets — each adjacent neighbour reshapes.
    for (let i = 0; i < 4; i++) {
        const si = sideGroup(c);
        if (si < 0) break;
        pushGroup(c, si, 0.3 + i * 0.05);
        assertManifold(c, `facet push #${i}`);
    }
});

t('negative extrude through origin is still manifold', () => {
    reg.clear();
    const p = makePolygon([[0,0],[1,0],[0.5,1]], 'Tri');
    pushGroup(p, 0, -1.5);  // big negative — top goes through bottom
    assertManifold(p, 'after negative extrude');
});

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
