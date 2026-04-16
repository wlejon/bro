// Headless test for the translate gizmo.
//
// Drives the gizmo through __editor hooks (no synthetic mouse events).
// Verifies:
//   - 3 axis nodes exist + visible while a primitive is active
//   - anchor follows the active primitive's bbox centroid
//   - hitTest distinguishes X / Y / Z arrows from world-space rays and
//     returns null when the ray misses
//   - hover state toggles per axis
//   - axis-locked drag translates only along the chosen axis
//   - drag commit refreshes the anchor to the new centroid
//   - cancel rolls back; non-target primitive untouched
//   - delete-active updates anchor to the new active primitive
//   - hide-active or empty registry hides the gizmo
//
// Run: bro-headless apps/scene-editor apps/scene-editor/test_gizmo.js

advanceTime(100);
flush();

const E = window.__editor;
const reg = E.registry;
const g = E.gizmo;

// --- Setup ----------------------------------------------------------------

assert(g, 'gizmo exposed on __editor');
assert(g.nodes.x && g.nodes.y && g.nodes.z, 'gizmo has 3 axis nodes');
assert(g.visible, 'gizmo visible (default box is active)');
assert(g.hovered === null, 'no axis hovered initially');

// Default box is centered at origin → centroid [0,0,0] → gizmo origin [0,0,0].
{
    const c = E.primCentroid(reg.active);
    assert(Math.abs(c[0]) < 1e-6 && Math.abs(c[1]) < 1e-6 && Math.abs(c[2]) < 1e-6,
        `default box centroid at origin (got [${c.join(',')}])`);
    assert(Math.abs(g.origin[0]) < 1e-6 && Math.abs(g.origin[1]) < 1e-6 && Math.abs(g.origin[2]) < 1e-6,
        `gizmo origin at default centroid (got [${g.origin.join(',')}])`);
}

// Scale > 0 (set by camera distance).
assert(g.scale > 0, `gizmo scale > 0 (got ${g.scale})`);

// --- hitTest: X axis from above ------------------------------------------

const armLen = g.arrow.length * g.scale;     // world-space arrow length
const midArm = armLen * 0.5;                  // a point well inside any arrow

{
    // Cursor over (midArm, 5, 0) looking straight down hits the +X arrow.
    const hit = Gizmo.hitTest(g, [midArm, 5, 0], [0, -1, 0]);
    assert(hit, 'ray over +X arrow returns a hit');
    assert(hit.axis === 'x', `+X arrow hit axis = "x" (got "${hit.axis}")`);
    assert(hit.axisDir[0] === 1 && hit.axisDir[1] === 0 && hit.axisDir[2] === 0,
        'X axisDir = [1,0,0]');
}
{
    // Cursor over (0, 5, midArm) hits +Z arrow.
    const hit = Gizmo.hitTest(g, [0, 5, midArm], [0, -1, 0]);
    assert(hit, 'ray over +Z arrow returns a hit');
    assert(hit.axis === 'z', `+Z arrow hit axis = "z" (got "${hit.axis}")`);
}
{
    // Y arrow: ray straight down through origin would overlap; use a ray
    // from +X side aimed toward the Y-axis at midArm height.
    const hit = Gizmo.hitTest(g, [5, midArm, 0], [-1, 0, 0]);
    assert(hit, 'ray toward +Y arrow returns a hit');
    assert(hit.axis === 'y', `+Y arrow hit axis = "y" (got "${hit.axis}")`);
}
{
    // Far away: ray well outside the gizmo's pick radius.
    const hit = Gizmo.hitTest(g, [10, 10, 10], [1, 0, 0]);
    assert(!hit, 'ray nowhere near the gizmo returns null');
}

// --- Hover state ----------------------------------------------------------

Gizmo.setHovered(g, 'x');
assert(g.hovered === 'x', 'hovered set to x');
assert(g.hoverNodes.x.visible, 'X hover node visible');
assert(!g.hoverNodes.y.visible && !g.hoverNodes.z.visible, 'other hover nodes hidden');
Gizmo.setHovered(g, null);
assert(g.hovered === null, 'hover cleared');
assert(!g.hoverNodes.x.visible && !g.hoverNodes.y.visible && !g.hoverNodes.z.visible,
    'all hover nodes hidden after clear');

// --- Anchor follows the active primitive ----------------------------------

const box2 = reg.create({
    type: 'box',
    name: 'Box 2',
    color: '#ffa502',
    position: [3, 1, 0],
    params: { sx: 1, sy: 1, sz: 1 },
});
// Adding doesn't change the active primitive — gizmo stays on default box.
assert(g.origin[0] === 0 && g.origin[1] === 0 && g.origin[2] === 0,
    'gizmo still at default box origin after adding box2');

// Activate box2 — gizmo should jump to its centroid.
reg.setActive(box2.id);
{
    const c = E.primCentroid(box2);
    assert(Math.abs(g.origin[0] - c[0]) < 1e-5 &&
           Math.abs(g.origin[1] - c[1]) < 1e-5 &&
           Math.abs(g.origin[2] - c[2]) < 1e-5,
        `gizmo origin follows active to box2 centroid (got [${g.origin.join(',')}])`);
}

// --- Axis-locked drag along X --------------------------------------------

const box1 = reg.primitives[0];
const box1Before = Array.from(box1.positions);
const box2Before = Array.from(box2.positions);

// Build a hit on the +X arrow at box2's location, then simulate cursor
// motion that puts the closest-point along the axis at a different X.
const armLen2 = g.arrow.length * g.scale;
const startCursor = [g.origin[0] + armLen2 * 0.5, g.origin[1] + 5, g.origin[2]];
const endCursor   = [g.origin[0] + armLen2 * 0.5 + 1.5, g.origin[1] + 5, g.origin[2]];
const startRay = { origin: startCursor, dir: [0, -1, 0] };
const endRay   = { origin: endCursor,   dir: [0, -1, 0] };

const startHit = Gizmo.hitTest(g, startRay.origin, startRay.dir);
assert(startHit && startHit.axis === 'x', 'start ray hits +X arrow');

E.beginGizmoDrag(box2, startHit, startRay);
assert(E.gizmoDrag.active, 'gizmo drag active');
assert(E.gizmoDrag.primitive === box2, 'gizmo drag bound to box2');

E.applyGizmoDrag(endRay);
// Mid-drag: canonical positions untouched.
for (let i = 0; i < box2Before.length; i++) {
    assert(Math.abs(box2.positions[i] - box2Before[i]) < 1e-5,
        `gizmo preview does not mutate canonical (idx ${i})`);
}

E.commitGizmoDrag();
assert(!E.gizmoDrag.active, 'gizmo drag inactive after commit');

// Y and Z must be unchanged; X must shift uniformly. The actual delta is
// the difference in axis-projected cursor position (1.5 in this setup
// because the cursor moved 1.5 units in X with a vertical ray).
let dxFirst = box2.positions[0] - box2Before[0];
assert(Math.abs(dxFirst - 1.5) < 1e-3,
    `box2 x shifted by ~1.5 after drag (got ${dxFirst})`);
for (let i = 0; i < box2Before.length; i += 3) {
    assert(Math.abs(box2.positions[i    ] - (box2Before[i    ] + dxFirst)) < 1e-3,
        `vert ${i/3} x shifted uniformly by drag delta`);
    assert(Math.abs(box2.positions[i + 1] -  box2Before[i + 1]) < 1e-5,
        `vert ${i/3} y unchanged (axis-locked)`);
    assert(Math.abs(box2.positions[i + 2] -  box2Before[i + 2]) < 1e-5,
        `vert ${i/3} z unchanged (axis-locked)`);
}

// box1 untouched.
for (let i = 0; i < box1Before.length; i++) {
    assert(Math.abs(box1.positions[i] - box1Before[i]) < 1e-5,
        `box1 (non-target) unchanged by gizmo drag (idx ${i})`);
}

// Anchor refreshed to the new centroid.
{
    const c = E.primCentroid(box2);
    assert(Math.abs(g.origin[0] - c[0]) < 1e-5,
        `gizmo origin re-anchored after drag commit (got x=${g.origin[0]}, expected ${c[0]})`);
}

// --- Cancel rolls back ----------------------------------------------------

const box2BeforeCancel = Array.from(box2.positions);
const startHit2 = Gizmo.hitTest(g, [g.origin[0] + armLen2 * 0.5, 5, g.origin[2]], [0, -1, 0]);
E.beginGizmoDrag(box2, startHit2, {
    origin: [g.origin[0] + armLen2 * 0.5, 5, g.origin[2]], dir: [0, -1, 0],
});
E.applyGizmoDrag({ origin: [g.origin[0] + armLen2 * 0.5 + 10, 5, g.origin[2]], dir: [0, -1, 0] });
E.cancelGizmoDrag();
assert(!E.gizmoDrag.active, 'gizmo drag inactive after cancel');
for (let i = 0; i < box2BeforeCancel.length; i++) {
    assert(Math.abs(box2.positions[i] - box2BeforeCancel[i]) < 1e-5,
        `cancel restored box2 positions (idx ${i})`);
}

// --- Delete active updates anchor to new active --------------------------

reg.remove(box2.id);
assert(reg.active === box1, 'active fell back to box1');
{
    const c = E.primCentroid(box1);
    assert(Math.abs(g.origin[0] - c[0]) < 1e-5,
        `gizmo origin moved to new active centroid (got ${g.origin[0]})`);
}
assert(g.visible, 'gizmo still visible (one primitive remains)');

// --- Hide active hides gizmo ---------------------------------------------

reg.setVisible(box1.id, false);
assert(!g.visible, 'gizmo hidden when active primitive is invisible');
reg.setVisible(box1.id, true);
assert(g.visible, 'gizmo visible again');

// --- Empty registry hides gizmo ------------------------------------------

reg.remove(box1.id);
assert(reg.primitives.length === 0, 'empty registry');
assert(reg.active === null, 'no active primitive');
assert(!g.visible, 'gizmo hidden in empty registry');

// --- Restore a fresh box for any follow-on tests / GUI ---------------------

reg.create({
    type: 'box',
    name: 'Box',
    color: '#74b9ff',
    params: { sx: 1, sy: 1, sz: 1 },
});
assert(g.visible, 'gizmo back after creating a primitive');

console.log(`OK — gizmo: 3 axis arrows visible while active prim, anchor follows centroid, ` +
            `hitTest distinguishes X/Y/Z, hover toggles, axis-locked drag translates only along ` +
            `that axis, commit re-anchors, cancel rolls back, non-target untouched, ` +
            `delete/hide/empty all update visibility correctly`);
