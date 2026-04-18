// Headless tests for EdgePrimitive (orphan edges).
//   - Direct creation via registry.createEdgePrimitive
//   - Inference geo dedups vertices and edges
//   - Snap candidates surface in collectInferenceGeos
//   - Line tool's commitLine path persists orphan polylines
//   - Undo/redo restores deleted edge primitives
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_orphan_edges.js

advanceTime(100);
flush();

const E = window.__editor;
assert(E, 'app handle exists');
assert(typeof EdgePrimitive === 'function', 'EdgePrimitive is registered');

// Capture starting registry state — the default scene seeds one cube.
const initialPrims = E.registry.primitives.length;
const initialTopLevel = E.registry.topLevel().length;

// --- 1. Direct creation -----------------------------------------------------
{
    const positions = new Float32Array([
        0, 0, 0,    // 0
        1, 0, 0,    // 1
        1, 0, 1,    // 2
    ]);
    const edges = [{ a: 0, b: 1 }, { a: 1, b: 2 }];
    const p = E.registry.createEdgePrimitive(
        { name: 'TestEdges' },
        { positions, edges });

    assert(p.kind === 'edge-primitive', 'kind is edge-primitive');
    assert(p.edges.length === 2, '2 edges installed');
    assert(p.positions.length === 9, '3 verts × 3 floats');
    assert(p.edgesNode, 'edgesNode attached to scene');
    assert(p.inferenceGeo, 'inference geo built');
    assert(p.inferenceGeo.vertCount === 3, 'inference dedups: 3 unique verts');
    assert(p.inferenceGeo.edges.length === 2, 'inference: 2 edges');

    // collectInferenceGeos must surface this primitive's geo so snapping works.
    const geos = E.registry.collectInferenceGeos();
    const found = geos.find(g => g === p.getWorldInferenceGeo());
    assert(found, 'edge primitive contributes to inference set');

    // No raycast hit — pickAt skips edge primitives.
    const ray = E.screenToRay
        ? null  // can't easily build a ray that targets the edge from pure JS
        : null;
    assert(p.raycastWorld() === null, 'edge primitives are unpickable by ray');

    // Cleanup for next test.
    E.registry.remove(p.id);
}

// --- 2. Vertex deduplication ------------------------------------------------
{
    const positions = new Float32Array([
        0, 0, 0,
        1, 0, 0,
        1, 0, 0,    // duplicate of vert 1 (same position)
        2, 0, 0,
    ]);
    const edges = [
        { a: 0, b: 1 },
        { a: 2, b: 3 },     // shares a vert with edge (0,1) at the seam
    ];
    const p = E.registry.createEdgePrimitive(
        { name: 'DedupedEdges' }, { positions, edges });
    assert(p.inferenceGeo.vertCount === 3,
        `dedup yields 3 unique verts (got ${p.inferenceGeo.vertCount})`);
    assert(p.inferenceGeo.edges.length === 2, 'two edges survive dedup');
    E.registry.remove(p.id);
}

// --- 3. Line tool commit persists an orphan polyline ------------------------
{
    const beforeCount = E.registry.root.children.filter(
        c => c.kind === 'edge-primitive').length;

    const plane = {
        origin: [0, 0, 0],
        normal: [0, 1, 0],
        u:      [1, 0, 0],
        v:      [0, 0, 1],
    };
    E.beginLine([0, 0, 0], plane);
    E.addLinePoint([1, 0, 0]);
    E.addLinePoint([1, 0, 1]);
    // No closure — explicit commit must persist as an EdgePrimitive.
    E.commitLine();

    const after = E.registry.root.children.filter(
        c => c.kind === 'edge-primitive');
    assert(after.length === beforeCount + 1,
        `commitLine persisted one orphan polyline (was ${beforeCount}, now ${after.length})`);

    const orphan = after[after.length - 1];
    assert(orphan.edges.length === 2,
        `polyline has 2 segments (got ${orphan.edges.length})`);

    // --- 4. Undo / redo round-trip -------------------------------------------
    const idBefore = orphan.id;
    E.history.undo();
    let nowCount = E.registry.root.children.filter(
        c => c.kind === 'edge-primitive').length;
    assert(nowCount === beforeCount, 'undo removes the orphan polyline');

    E.history.redo();
    const restored = E.registry.getById(idBefore);
    assert(restored && restored.kind === 'edge-primitive',
        'redo restores the orphan polyline by id');
    assert(restored.edges.length === 2, 'restored polyline has 2 segments');

    // Cleanup.
    E.registry.remove(idBefore);
}

// --- 5. Cancel does NOT persist anything ------------------------------------
{
    const beforeCount = E.registry.root.children.filter(
        c => c.kind === 'edge-primitive').length;
    const plane = {
        origin: [0, 0, 0], normal: [0, 1, 0],
        u: [1, 0, 0], v: [0, 0, 1],
    };
    E.beginLine([0, 0, 0], plane);
    E.addLinePoint([1, 0, 0]);
    E.cancelLine();
    const afterCount = E.registry.root.children.filter(
        c => c.kind === 'edge-primitive').length;
    assert(afterCount === beforeCount, 'cancelLine drops the chain (no persist)');
}

// Restore registry to pre-test state (the default-scene cube).
assert(E.registry.primitives.length === initialPrims,
    'primitive count unchanged after orphan-edge tests');
assert(E.registry.topLevel().length === initialTopLevel,
    'top-level count unchanged after orphan-edge tests');

console.log('OK — EdgePrimitive: direct create, dedup, snap surface, ' +
            'line-tool persist, undo/redo, cancel-drops-chain');
