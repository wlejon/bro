// EditMesh surgery primitives — unit tests.
//
// Exercises findFaceGroupBoundary / duplicateBoundary / addBridge against
// known meshes (box, cylinder) so push/pull's commit path can compose them
// confidently. No scene/canvas needed.
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_editmesh_surgery.js

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

// Build an EditMesh tagged with the same face-group assignment the
// scene-editor's Primitive uses.
function tag(mesh) {
    const fg = Primitive.computeFaceGroups(mesh.positions, mesh.indices);
    return {
        em: EditMesh.fromMeshData(mesh.positions, mesh.indices, fg.triToGroup),
        fg,
    };
}

// -------------------------------------------------------------------------
// Boundary detection
// -------------------------------------------------------------------------

t('box top face has a single 4-vert boundary loop', () => {
    const box = Mesh.box(1, 1, 1);   // Mesh.box uses half-extent → top at y=1.
    const { em, fg } = tag(box);
    const topG = fg.groups.findIndex(g => g.normal[1] > 0.99);
    truthy(topG >= 0);
    const loops = EditMesh.findFaceGroupBoundary(em, topG);
    eq(loops.length, 1, 'one boundary loop');
    eq(loops[0].length, 4, '4 boundary half-edges');
    for (const he of loops[0]) {
        near(he.origin.y, 1.0, 1e-5, 'boundary vert on top plane y=1');
    }
});

t('cylinder side facet boundary is a 4-vert loop (top, top, bot, bot)', () => {
    const cyl = Mesh.cylinder(1, 1, 16);  // height=2 → caps at y=±1
    const { em, fg } = tag(cyl);
    const sideG = fg.groups.findIndex(g => Math.abs(g.normal[1]) < 0.1);
    truthy(sideG >= 0);
    const loops = EditMesh.findFaceGroupBoundary(em, sideG);
    eq(loops.length, 1, 'one boundary loop');
    eq(loops[0].length, 4, '4 boundary HEs around the quad');
    let top = 0, bot = 0;
    for (const he of loops[0]) {
        const y = he.origin.y;
        if (y > 0.5) top++;
        else if (y < -0.5) bot++;
    }
    eq(top, 2, '2 top verts');
    eq(bot, 2, '2 bot verts');
});

t('cylinder top cap boundary is a single N-loop (rim)', () => {
    const cyl = Mesh.cylinder(1, 1, 16);
    const { em, fg } = tag(cyl);
    const topG = fg.groups.findIndex(g => g.normal[1] > 0.99);
    truthy(topG >= 0);
    const loops = EditMesh.findFaceGroupBoundary(em, topG);
    eq(loops.length, 1, 'one boundary loop');
    eq(loops[0].length, 16, '16 boundary HEs (one per rim segment)');
});

// -------------------------------------------------------------------------
// duplicateBoundary
// -------------------------------------------------------------------------

t('duplicateBoundary on box top: 4 new verts at offset, top face moves', () => {
    const box = Mesh.box(1, 1, 1);
    const { em, fg } = tag(box);
    const topG = fg.groups.findIndex(g => g.normal[1] > 0.99);
    const vertCountBefore = em.vertices.length;
    const { dupMap, oldBoundary } = EditMesh.duplicateBoundary(em, topG, [0, 0.3, 0]);
    eq(em.vertices.length - vertCountBefore, 4,
       '4 new boundary verts appended');
    eq(dupMap.size, 4, 'dupMap has 4 entries');
    for (const [oldV, newV] of dupMap.entries()) {
        near(newV.x, oldV.x, 1e-5);
        near(newV.y, oldV.y + 0.3, 1e-5);
        near(newV.z, oldV.z, 1e-5);
    }
    // Top face's triangles now reference duplicates (origins moved up).
    for (const f of em.faces) {
        if (f.group !== topG) continue;
        for (const he of EditMesh.faceHalfEdges(f)) {
            near(he.origin.y, 1.3, 1e-5, 'top tri vert lifted to y=1.3');
        }
    }
    // Boundary HEs severed.
    for (const loop of oldBoundary) {
        for (const rec of loop) {
            eq(rec.he.twin, null, 'boundary HE twin severed');
        }
    }
});

// -------------------------------------------------------------------------
// addBridge
// -------------------------------------------------------------------------

t('addBridge produces 2 tris per boundary HE; rematch closes manifold', () => {
    const box = Mesh.box(1, 1, 1);
    const { em, fg } = tag(box);
    const topG = fg.groups.findIndex(g => g.normal[1] > 0.99);
    const triCountBefore = em.faces.length;
    const { oldBoundary } = EditMesh.duplicateBoundary(em, topG, [0, 0.5, 0]);
    // Each side wall bridge gets its own group so coplanar tris merge into
    // existing wall groups when rematchTwins runs (here we just use placeholder
    // ids per HE — the real push/pull commit picks groups based on coplanarity).
    let nextGroup = fg.groups.length;
    for (const loop of oldBoundary) {
        for (const rec of loop) {
            EditMesh.addBridge(em, rec.oldA, rec.oldB, rec.newA, rec.newB, nextGroup++);
        }
    }
    eq(em.faces.length - triCountBefore, 8, '8 bridge tris added');
    EditMesh.rematchTwins(em);
    const val = EditMesh.validate(em);
    truthy(val.ok, 'validate ok after bridge: ' + val.errors.join('; '));
    truthy(val.isClosed, 'box stays closed manifold ' +
           '(boundary=' + val.boundaryHalfEdges + ')');
});

// -------------------------------------------------------------------------
// End-to-end: cylinder side-facet pull via surgery (no warp)
// -------------------------------------------------------------------------

t('cylinder side-facet pull: bridges merge with caps (no warp), wall verts +2', () => {
    const cyl = Mesh.cylinder(1, 1, 16);
    const { em, fg } = tag(cyl);
    // +X-ish side facet.
    const sideG = (() => {
        let best = -1, bestX = -Infinity;
        for (let i = 0; i < fg.groups.length; i++) {
            const n = fg.groups[i].normal;
            if (Math.abs(n[1]) > 0.1) continue;
            if (n[0] > bestX) { bestX = n[0]; best = i; }
        }
        return best;
    })();
    truthy(sideG >= 0, 'found +X side facet');
    const sideNormal = fg.groups[sideG].normal.slice();
    const offset = [sideNormal[0] * 0.5, sideNormal[1] * 0.5, sideNormal[2] * 0.5];
    // Capture cap-rim count before surgery.
    const topG = fg.groups.findIndex(g => g.normal[1] > 0.99);
    const botG = fg.groups.findIndex(g => g.normal[1] < -0.99);
    const topRimBefore = EditMesh.findFaceGroupBoundary(em, topG)[0].length;
    const botRimBefore = EditMesh.findFaceGroupBoundary(em, botG)[0].length;

    // Surgery.
    const { oldBoundary } = EditMesh.duplicateBoundary(em, sideG, offset);
    // For each boundary edge, decide which group the bridge belongs to:
    //   - top/bot edge (horizontal) + horizontal offset → bridge is in the
    //     cap plane → merge with that cap
    //   - vertical edge (left/right wall side) → new face group
    let nextNewGroup = fg.groups.length;
    for (const loop of oldBoundary) {
        for (const rec of loop) {
            const a = rec.oldA, b = rec.oldB;
            const isTopEdge = Math.abs(a.y - 1) < 1e-4 && Math.abs(b.y - 1) < 1e-4;
            const isBotEdge = Math.abs(a.y + 1) < 1e-4 && Math.abs(b.y + 1) < 1e-4;
            let g;
            if (isTopEdge) g = topG;
            else if (isBotEdge) g = botG;
            else { g = nextNewGroup++; }
            EditMesh.addBridge(em, rec.oldA, rec.oldB, rec.newA, rec.newB, g);
        }
    }
    EditMesh.rematchTwins(em);
    const val = EditMesh.validate(em);
    truthy(val.ok, 'validate ok: ' + val.errors.join('; '));
    truthy(val.isClosed, 'cylinder stays closed ' +
           '(boundary=' + val.boundaryHalfEdges + ')');

    const topRimAfter = EditMesh.findFaceGroupBoundary(em, topG)[0].length;
    const botRimAfter = EditMesh.findFaceGroupBoundary(em, botG)[0].length;
    eq(topRimAfter, topRimBefore + 2, 'top rim grew by 2 verts');
    eq(botRimAfter, botRimBefore + 2, 'bottom rim grew by 2 verts');
});

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
