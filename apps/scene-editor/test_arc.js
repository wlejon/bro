// Headless tests for ArcTool + Sketch.arcPolyline + the app's arc wiring.
// Run: bro-headless apps/scene-editor apps/scene-editor/test_arc.js

advanceTime(100);
flush();

const E = window.__editor;
assert(E, 'app handle exists');
assert(typeof ArcTool === 'object', 'ArcTool is registered');
assert(typeof Sketch.arcPolyline === 'function', 'Sketch.arcPolyline exists');

// --- Sketch.arcPolyline geometry --------------------------------------------

// Symmetric arc on the XZ plane: chord (-1,0,0)→(+1,0,0), bulge (0,0,1).
// Expect 17 points (16 segments) all at distance r from a center on the
// chord-perpendicular line, and bulge height in (0, 1].
{
    const start = [-1, 0, 0];
    const end   = [ 1, 0, 0];
    const bulge = [ 0, 0, 1];   // chord length 2, bulge height 1 ⇒ semicircle
    const n     = [ 0, 1, 0];
    const arc = Sketch.arcPolyline(start, end, bulge, n, 16);
    assert(arc, 'arc returned');
    assert(arc.length === 17, `17 points (got ${arc.length})`);
    // Endpoints match the inputs.
    const dStart = Math.hypot(arc[0][0]-start[0], arc[0][1]-start[1], arc[0][2]-start[2]);
    const dEnd   = Math.hypot(arc[16][0]-end[0],  arc[16][1]-end[1],  arc[16][2]-end[2]);
    assert(dStart < 1e-5, `start matches (delta ${dStart})`);
    assert(dEnd   < 1e-5, `end matches (delta ${dEnd})`);
    // For a semicircle (h == L/2), r = h, center sits on the chord.
    // Every point is r=1 from the origin.
    for (let i = 0; i < arc.length; i++) {
        const r = Math.hypot(arc[i][0], arc[i][1], arc[i][2]);
        assert(Math.abs(r - 1) < 1e-5, `pt ${i} at r=1 (got ${r})`);
    }
    // Apex (mid index) is at +Z.
    const mid = arc[8];
    assert(Math.abs(mid[0]) < 1e-5, `apex on +Z axis (x=${mid[0]})`);
    assert(Math.abs(mid[2] - 1) < 1e-5, `apex z≈1 (z=${mid[2]})`);
}

// Shallow arc — bulge height much smaller than half-chord. Center should
// sit on the side OPPOSITE the bulge.
{
    const arc = Sketch.arcPolyline(
        [-1, 0, 0], [1, 0, 0], [0, 0, 0.1], [0, 1, 0], 8);
    assert(arc, 'shallow arc returned');
    // Apex z = 0.1.
    assert(Math.abs(arc[4][2] - 0.1) < 1e-5,
        `apex hits bulge height 0.1 (got ${arc[4][2]})`);
}

// Negative bulge — arc bows in -Z.
{
    const arc = Sketch.arcPolyline(
        [-1, 0, 0], [1, 0, 0], [0, 0, -0.5], [0, 1, 0], 8);
    assert(arc, 'negative arc returned');
    assert(arc[4][2] < 0, `apex on -Z side (z=${arc[4][2]})`);
}

// Degenerate: no chord ⇒ null.
{
    const arc = Sketch.arcPolyline([0,0,0], [0,0,0], [0,0,1], [0,1,0]);
    assert(arc === null, 'zero chord ⇒ null');
}

// Degenerate: zero bulge ⇒ straight chord (2 points).
{
    const arc = Sketch.arcPolyline([0,0,0], [1,0,0], [0.5,0,0], [0,1,0]);
    assert(arc && arc.length === 2, 'zero bulge ⇒ chord (2 pts)');
}

// --- ArcTool state machine --------------------------------------------------

{
    const st = ArcTool.createState();
    assert(!ArcTool.active(st), 'idle');
    const plane = { origin:[0,0,0], normal:[0,1,0], u:[1,0,0], v:[0,0,1] };
    ArcTool.begin(st, plane, [-1, 0, 0], 8);
    assert(st.stage === 'await-end', 'stage = await-end after begin');
    assert(ArcTool.active(st), 'active');

    // Preview during await-end is the straight chord.
    ArcTool.update(st, [1, 0, 0]);
    let prev = ArcTool.buildPolyline(st);
    assert(prev && prev.length === 2, 'await-end preview is chord');

    // Click 2 sets the chord. Too close ⇒ rejected.
    assert(!ArcTool.setEnd(st, [-1, 0, 0]),
        'setEnd at start position rejected');
    assert(ArcTool.setEnd(st, [1, 0, 0]), 'setEnd at distinct point accepted');
    assert(st.stage === 'await-bulge', 'stage = await-bulge');

    // Preview is now the arc using the live cursor as the bulge point.
    ArcTool.update(st, [0, 0, 0.5]);
    prev = ArcTool.buildPolyline(st);
    assert(prev && prev.length === 9, `arc preview has 9 pts (got ${prev.length})`);

    // Commit returns the polyline + clears state.
    ArcTool.update(st, [0, 0, 1]);
    const out = ArcTool.commit(st);
    assert(out && out.length === 9, 'commit returns 9-pt polyline');
    assert(!ArcTool.active(st), 'idle after commit');
    assert(st.start === null && st.end === null, 'state cleared on commit');
}

// --- App wiring: arc → EdgePrimitive ----------------------------------------

{
    const beforeCount = E.registry.root.children.filter(
        c => c.kind === 'edge-primitive').length;
    const plane = { origin:[0,0,0], normal:[0,1,0], u:[1,0,0], v:[0,0,1] };
    E.beginArc([-1, 0, 0], plane);
    assert(E.arcToolState.stage === 'await-end', 'app: await-end');
    E.setArcEnd([1, 0, 0]);
    assert(E.arcToolState.stage === 'await-bulge', 'app: await-bulge');
    E.updateArcAt([0, 0, 1]);
    E.commitArc();

    const after = E.registry.root.children.filter(
        c => c.kind === 'edge-primitive');
    assert(after.length === beforeCount + 1, 'one EdgePrimitive added');
    const arcPrim = after[after.length - 1];
    // Default 16 segments → 17 verts → 16 edges.
    assert(arcPrim.edges.length === 16,
        `arc primitive has 16 edges (got ${arcPrim.edges.length})`);

    // Undo removes it.
    const id = arcPrim.id;
    E.history.undo();
    assert(!E.registry.getById(id), 'undo removes the arc primitive');

    // Redo restores it.
    E.history.redo();
    assert(E.registry.getById(id), 'redo restores the arc primitive');

    // Cleanup.
    E.registry.remove(id);
}

// --- Cancel does NOT persist ------------------------------------------------

{
    const beforeCount = E.registry.root.children.filter(
        c => c.kind === 'edge-primitive').length;
    const plane = { origin:[0,0,0], normal:[0,1,0], u:[1,0,0], v:[0,0,1] };
    E.beginArc([0, 0, 0], plane);
    E.setArcEnd([1, 0, 0]);
    E.cancelArc();
    const afterCount = E.registry.root.children.filter(
        c => c.kind === 'edge-primitive').length;
    assert(afterCount === beforeCount, 'cancelArc drops the in-flight arc');
}

console.log('OK — Arc: arcPolyline geometry, state machine, app wiring, ' +
            'EdgePrimitive output, undo/redo, cancel');
