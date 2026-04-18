// Headless tests for OffsetTool + Sketch.offsetPolygon2D + app wiring.
// Run: bro-headless apps/scene-editor apps/scene-editor/test_offset.js

advanceTime(100);
flush();

const E = window.__editor;
assert(E, 'app handle exists');
assert(typeof OffsetTool === 'object', 'OffsetTool registered');
assert(typeof Sketch.offsetPolygon2D === 'function', 'offsetPolygon2D exists');

// --- Sketch.offsetPolygon2D ------------------------------------------------

// Square inset: 2x2 square (CCW), inset by 0.5 → 1x1 centered square.
{
    const sq = [[0,0], [2,0], [2,2], [0,2]];
    const inset = Sketch.offsetPolygon2D(sq, -0.5);
    assert(inset && inset.length === 4, '4-vertex result');
    const expected = [[0.5,0.5], [1.5,0.5], [1.5,1.5], [0.5,1.5]];
    for (let i = 0; i < 4; i++) {
        const dx = inset[i][0] - expected[i][0];
        const dy = inset[i][1] - expected[i][1];
        assert(Math.abs(dx) < 1e-9 && Math.abs(dy) < 1e-9,
            `inset[${i}] ≈ ${expected[i]} (got ${inset[i]})`);
    }
}

// Square expand: 2x2 square expanded by 0.5 → 3x3 centered on (1,1).
{
    const sq = [[0,0], [2,0], [2,2], [0,2]];
    const ex = Sketch.offsetPolygon2D(sq, 0.5);
    assert(ex && ex.length === 4, 'expand: 4 verts');
    const expected = [[-0.5,-0.5], [2.5,-0.5], [2.5,2.5], [-0.5,2.5]];
    for (let i = 0; i < 4; i++) {
        const dx = ex[i][0] - expected[i][0];
        const dy = ex[i][1] - expected[i][1];
        assert(Math.abs(dx) < 1e-9 && Math.abs(dy) < 1e-9,
            `expand[${i}] ≈ ${expected[i]} (got ${ex[i]})`);
    }
}

// Inset that collapses: 2x2 square inset by 1.5 → null (medial-axis cross).
{
    const sq = [[0,0], [2,0], [2,2], [0,2]];
    const collapsed = Sketch.offsetPolygon2D(sq, -1.5);
    assert(collapsed === null, 'over-inset returns null');
}

// CW input still gets correct sign convention (positive = outward of input).
{
    const cw = [[0,0], [0,2], [2,2], [2,0]];   // same square, CW winding
    const ex = Sketch.offsetPolygon2D(cw, 0.5);
    assert(ex && ex.length === 4, 'CW expand: 4 verts');
    // CW expand should be same envelope as CCW expand: bounds [-0.5, 2.5].
    let minX = Infinity, maxX = -Infinity;
    for (const p of ex) {
        if (p[0] < minX) minX = p[0];
        if (p[0] > maxX) maxX = p[0];
    }
    assert(Math.abs(minX + 0.5) < 1e-9 && Math.abs(maxX - 2.5) < 1e-9,
        `CW expand bounds: [-0.5, 2.5] (got [${minX}, ${maxX}])`);
}

// Zero distance: identity.
{
    const sq = [[0,0], [2,0], [2,2], [0,2]];
    const same = Sketch.offsetPolygon2D(sq, 0);
    assert(same.length === 4 && same[0][0] === 0 && same[2][1] === 2,
        'zero distance is identity');
}

// --- App wiring: offset on a cube top face ---------------------------------

{
    // Default scene has a unit cube. Find its +Y face group.
    const prim = E.registry.primitives[0];
    assert(prim, 'has a primitive');
    const topGroup = prim.faceGroups.groups.findIndex(
        g => g.normal[1] > 0.999);
    assert(topGroup >= 0, 'cube has +Y face group');

    // Boundary loop in world space — 4 verts.
    const loop = E.faceGroupBoundaryWorld(prim, topGroup);
    assert(loop && loop.length === 4,
        `top boundary has 4 verts (got ${loop ? loop.length : 'null'})`);

    // Begin offset at the loop centroid.
    const cx = (loop[0][0]+loop[1][0]+loop[2][0]+loop[3][0]) / 4;
    const cy = (loop[0][1]+loop[1][1]+loop[2][1]+loop[3][1]) / 4;
    const cz = (loop[0][2]+loop[1][2]+loop[2][2]+loop[3][2]) / 4;
    // Use a clickPos slightly off-centre so the cursor-distance axis is
    // well-defined.
    const click = [cx + 0.4, cy, cz];
    const before = E.registry.primitives.length;
    const ok = E.beginOffset(prim, topGroup, click);
    assert(ok, 'beginOffset accepted');
    assert(OffsetTool.active(E.offsetToolState), 'offset active');

    // Apply -0.2 (inset).
    E.applyOffsetDistance(-0.2);
    const preview = E.offsetToolState.previewLoop;
    assert(preview && preview.length === 4,
        `inset preview has 4 verts (got ${preview ? preview.length : 'null'})`);

    // Commit.
    E.commitOffset();
    assert(!OffsetTool.active(E.offsetToolState), 'idle after commit');
    const after = E.registry.primitives.length;
    assert(after === before + 1,
        `commit added one primitive (was ${before}, now ${after})`);

    // The new primitive must lie at the same Y as the cube's top face
    // (0.5 for a unit cube centered at origin).
    const newPrim = E.registry.primitives[after - 1];
    let minY = Infinity, maxY = -Infinity;
    for (let i = 0; i < newPrim.positions.length; i += 3) {
        const w = newPrim.localToWorldPoint([
            newPrim.positions[i],
            newPrim.positions[i+1],
            newPrim.positions[i+2],
        ]);
        if (w[1] < minY) minY = w[1];
        if (w[1] > maxY) maxY = w[1];
    }
    assert(Math.abs(minY - maxY) < 1e-5,
        `offset face is planar (Y range ${(maxY-minY).toExponential(2)})`);
    assert(Math.abs(minY - loop[0][1]) < 1e-5,
        `offset face coplanar with source (got y=${minY}, expected ${loop[0][1]})`);

    // Undo / redo.
    const newId = newPrim.id;
    E.history.undo();
    assert(!E.registry.getById(newId), 'undo removes offset face');
    E.history.redo();
    assert(E.registry.getById(newId), 'redo restores offset face');

    E.registry.remove(newId);
}

// --- Cancel does not commit -------------------------------------------------

{
    const prim = E.registry.primitives[0];
    const topGroup = prim.faceGroups.groups.findIndex(
        g => g.normal[1] > 0.999);
    const loop = E.faceGroupBoundaryWorld(prim, topGroup);
    const click = [loop[0][0]+0.1, loop[0][1], loop[0][2]+0.1];
    const before = E.registry.primitives.length;
    E.beginOffset(prim, topGroup, click);
    E.applyOffsetDistance(-0.1);
    E.cancelOffset();
    assert(E.registry.primitives.length === before, 'cancel adds nothing');
    assert(!OffsetTool.active(E.offsetToolState), 'idle after cancel');
}

console.log('OK — Offset: offsetPolygon2D inset/expand/collapse/CW, app ' +
            'wiring, coplanar new primitive, undo/redo, cancel');
