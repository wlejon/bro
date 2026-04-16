// Headless tests for the inference engine.
// Run: bro-headless apps/scene-editor apps/scene-editor/test_inference.js

advanceTime(100);
flush();

const E = window.__editor;
const I = window.Inference;
assert(I, 'Inference module is loaded');

// --- buildInferenceGeo on the cube -----------------------------------------
//
// The hard-edged cube has 24 vertex indices (4 per face × 6 faces) but only
// 8 unique world positions (the corners). And 12 unique edges (cube edges).

const geo = I.buildInferenceGeo(E.boxPositions, E.boxIndices, E.faceGroups);
assert(geo.vertCount === 8,
    `cube has 8 unique vertex positions (got ${geo.vertCount})`);
assert(geo.edges.length === 12,
    `cube has 12 model edges (got ${geo.edges.length})`);

// All cube corners should be at (±1, ±1, ±1).
const seenCorners = new Set();
for (let vi = 0; vi < geo.vertCount; vi++) {
    const x = geo.positions[vi * 3 + 0];
    const y = geo.positions[vi * 3 + 1];
    const z = geo.positions[vi * 3 + 2];
    assert(Math.abs(Math.abs(x) - 1) < 1e-5, `corner x is ±1 (got ${x})`);
    assert(Math.abs(Math.abs(y) - 1) < 1e-5, `corner y is ±1 (got ${y})`);
    assert(Math.abs(Math.abs(z) - 1) < 1e-5, `corner z is ±1 (got ${z})`);
    seenCorners.add(`${Math.sign(x)},${Math.sign(y)},${Math.sign(z)}`);
}
assert(seenCorners.size === 8, `all 8 distinct corner sign-triples (got ${seenCorners.size})`);

// --- worldToScreen ---------------------------------------------------------
//
// Hand-computed: camera at (0,0,5) looking at origin, fov=45, aspect=1, 800x800.
// Origin projects to screen center; (1,1,1) lands in upper-right quadrant
// (screen y is inverted: world +y → screen -y).

const camOpts = {
    position: [0, 0, 5],
    target:   [0, 0, 0],
    up:       [0, 1, 0],
    fov: 45, aspect: 1, near: 0.1, far: 100,
};
const W = 800, H = 800;

const center = I.worldToScreen([0, 0, 0], camOpts, W, H);
assert(!center.behind, 'origin is in front of camera');
assert(Math.abs(center.x - W / 2) < 1e-3, `origin → screen x center (got ${center.x})`);
assert(Math.abs(center.y - H / 2) < 1e-3, `origin → screen y center (got ${center.y})`);

const upRight = I.worldToScreen([1, 1, 1], camOpts, W, H);
assert(upRight.x > W / 2, 'positive world x → right of screen center');
assert(upRight.y < H / 2, 'positive world y → above screen center');

const behind = I.worldToScreen([0, 0, 10], camOpts, W, H);
assert(behind.behind, 'point past camera plane is flagged behind');

// --- closestPointOnSegmentToRay --------------------------------------------

// Ray straight down through (1, 100, 0), edge from (0,0,0)→(2,0,0). Closest
// point on the edge to that ray is (1, 0, 0); segment param t = 0.5.
{
    const cp = I.closestPointOnSegmentToRay(
        [0, 0, 0], [2, 0, 0],
        { origin: [1, 100, 0], dir: [0, -1, 0] });
    assert(Math.abs(cp.t - 0.5) < 1e-6, `t = 0.5 (got ${cp.t})`);
    assert(Math.abs(cp.point[0] - 1) < 1e-6, `closest x = 1 (got ${cp.point[0]})`);
    assert(Math.abs(cp.point[1])     < 1e-6, `closest y = 0 (got ${cp.point[1]})`);
}

// Ray that misses past the segment endpoint (t < 0): clamps to t=0.
{
    const cp = I.closestPointOnSegmentToRay(
        [0, 0, 0], [2, 0, 0],
        { origin: [-5, 100, 0], dir: [0, -1, 0] });
    assert(cp.t === 0, `clamped t = 0 (got ${cp.t})`);
}
// Ray past the other endpoint (t > 1): clamps to t=1.
{
    const cp = I.closestPointOnSegmentToRay(
        [0, 0, 0], [2, 0, 0],
        { origin: [9, 100, 0], dir: [0, -1, 0] });
    assert(cp.t === 1, `clamped t = 1 (got ${cp.t})`);
}

// --- findSnap: endpoint -----------------------------------------------------
//
// A ray straight at the corner (1,1,1), with the cursor positioned exactly on
// the projected screen pixel. Should snap to that endpoint.

{
    const corner = [1, 1, 1];
    const sp = I.worldToScreen(corner, camOpts, W, H);
    const ray = makeRay(camOpts, W, H, sp.x, sp.y);
    const snap = I.findSnap({
        cursorX: sp.x, cursorY: sp.y, ray, camOpts, width: W, height: H, geo,
    });
    assert(snap, 'corner-aimed cursor produces a snap');
    assert(snap.type === 'endpoint',
        `snap is endpoint at corner (got ${snap.type})`);
    assert(Math.abs(snap.position[0] - 1) < 1e-5, `snap x = 1 (got ${snap.position[0]})`);
    assert(Math.abs(snap.position[1] - 1) < 1e-5, `snap y = 1 (got ${snap.position[1]})`);
    assert(Math.abs(snap.position[2] - 1) < 1e-5, `snap z = 1 (got ${snap.position[2]})`);
}

// --- findSnap: midpoint ----------------------------------------------------
//
// Aim cursor exactly at the midpoint of one cube edge. With endpoints out of
// tolerance (the corner is far away in screen pixels), midpoint wins.

{
    // Pick the +X +Y top-front edge: (1,1,-1) → (1,1,1). Midpoint = (1,1,0).
    const mid = [1, 1, 0];
    const sp = I.worldToScreen(mid, camOpts, W, H);
    const ray = makeRay(camOpts, W, H, sp.x, sp.y);
    const snap = I.findSnap({
        cursorX: sp.x, cursorY: sp.y, ray, camOpts, width: W, height: H, geo,
    });
    assert(snap, 'midpoint-aimed cursor produces a snap');
    assert(snap.type === 'midpoint',
        `snap is midpoint at edge centre (got ${snap.type})`);
    // Confirm we got the (1,1,0) midpoint (not some other edge's midpoint
    // happening to project nearby).
    assert(Math.abs(snap.position[0] - 1) < 1e-5, `mid x = 1 (got ${snap.position[0]})`);
    assert(Math.abs(snap.position[1] - 1) < 1e-5, `mid y = 1 (got ${snap.position[1]})`);
    assert(Math.abs(snap.position[2])     < 1e-5, `mid z = 0 (got ${snap.position[2]})`);
}

// --- findSnap: on-edge ------------------------------------------------------
//
// Aim cursor at a non-midpoint, non-endpoint point on a known edge. Should
// snap on-edge.

{
    // Edge (1,1,-1) → (1,1,1) again. Pick t = 0.25 → world (1, 1, -0.5).
    const along = [1, 1, -0.5];
    const sp = I.worldToScreen(along, camOpts, W, H);
    const ray = makeRay(camOpts, W, H, sp.x, sp.y);
    const snap = I.findSnap({
        cursorX: sp.x, cursorY: sp.y, ray, camOpts, width: W, height: H, geo,
    });
    assert(snap, 'on-edge cursor produces a snap');
    assert(snap.type === 'on-edge',
        `snap is on-edge for non-vertex cursor (got ${snap.type})`);
}

// --- findSnap: priority — endpoint beats midpoint --------------------------
//
// Aim cursor at the corner (1,1,1). Both the corner endpoint and any nearby
// midpoint within screen tolerance must lose to the endpoint.

{
    const corner = [1, 1, 1];
    const sp = I.worldToScreen(corner, camOpts, W, H);
    const ray = makeRay(camOpts, W, H, sp.x, sp.y);
    const snap = I.findSnap({
        cursorX: sp.x, cursorY: sp.y, ray, camOpts, width: W, height: H, geo,
        tol: 200,   // huge tolerance to ensure many candidates
    });
    assert(snap.type === 'endpoint',
        `endpoint priority over other snaps within tol (got ${snap.type})`);
}

// --- findSnap: no candidates returns null ----------------------------------

{
    const ray = makeRay(camOpts, W, H, 0, 0);
    const snap = I.findSnap({
        cursorX: 0, cursorY: 0, ray, camOpts, width: W, height: H, geo,
        tol: 5,
    });
    assert(snap === null, `top-left corner (no features) → null (got ${snap && snap.type})`);
}

// --- findSnap: excludeTypes -------------------------------------------------
//
// On-edge cursor with on-edge excluded should fall through to no snap (the
// ray's closest-point on the edge isn't an endpoint or midpoint), proving the
// filter actually skips the edge-projection path. Used by push/pull drag to
// suppress the noisy axis-projection of on-edge snaps.

{
    const along = [1, 1, -0.5];
    const sp = I.worldToScreen(along, camOpts, W, H);
    const ray = makeRay(camOpts, W, H, sp.x, sp.y);
    const snap = I.findSnap({
        cursorX: sp.x, cursorY: sp.y, ray, camOpts, width: W, height: H, geo,
        excludeTypes: ['on-edge'],
    });
    assert(snap === null,
        `excluding on-edge drops the on-edge snap (got ${snap && snap.type})`);
}

// Excluding endpoints lets midpoint take over when both are within tol.
{
    const corner = [1, 1, 1];
    const sp = I.worldToScreen(corner, camOpts, W, H);
    const ray = makeRay(camOpts, W, H, sp.x, sp.y);
    const snap = I.findSnap({
        cursorX: sp.x, cursorY: sp.y, ray, camOpts, width: W, height: H, geo,
        tol: 200, excludeTypes: ['endpoint'],
    });
    assert(snap, 'still some snap candidate within tol=200');
    assert(snap.type !== 'endpoint',
        `excludeTypes drops endpoint (got ${snap.type})`);
}

// --- findSnap: on-face fallback --------------------------------------------
//
// No vertex/edge within tol; supplying onFaceHit yields an on-face snap.

{
    const ray = makeRay(camOpts, W, H, W / 2, H / 2);
    const fakeHit = {
        triangleIndex: 0,
        position: [0, 0, 1],
        normal:   [0, 0, 1],
        distance: 4,
    };
    const snap = I.findSnap({
        cursorX: W / 2, cursorY: H / 2, ray, camOpts, width: W, height: H, geo,
        tol: 5, onFaceHit: fakeHit,
    });
    assert(snap, 'on-face fallback returns a snap');
    assert(snap.type === 'on-face', `snap is on-face (got ${snap.type})`);
}

// --- Helper: build a ray from a camOpts + screen pixel ---------------------
//
// Mirrors app.js#screenToRay, kept local so the test doesn't depend on the
// app's camera state.

function makeRay(opts, w, h, px, py) {
    const nx = (2 * px / w) - 1;
    const ny = 1 - (2 * py / h);
    const tanHalf = Math.tan(opts.fov * Math.PI / 180 * 0.5);
    const aspect  = opts.aspect;

    const fx = opts.target[0] - opts.position[0];
    const fy = opts.target[1] - opts.position[1];
    const fz = opts.target[2] - opts.position[2];
    const fl = Math.hypot(fx, fy, fz) || 1;
    const f  = [fx / fl, fy / fl, fz / fl];
    const up = opts.up;
    let rx = f[1] * up[2] - f[2] * up[1];
    let ry = f[2] * up[0] - f[0] * up[2];
    let rz = f[0] * up[1] - f[1] * up[0];
    const rl = Math.hypot(rx, ry, rz) || 1;
    rx /= rl; ry /= rl; rz /= rl;
    const ux = ry * f[2] - rz * f[1];
    const uy = rz * f[0] - rx * f[2];
    const uz = rx * f[1] - ry * f[0];
    const sx = nx * aspect * tanHalf;
    const sy = ny * tanHalf;
    let dx = f[0] + sx * rx + sy * ux;
    let dy = f[1] + sx * ry + sy * uy;
    let dz = f[2] + sx * rz + sy * uz;
    const dl = Math.hypot(dx, dy, dz) || 1;
    return {
        origin: opts.position.slice(),
        dir: [dx / dl, dy / dl, dz / dl],
    };
}

console.log(`OK — inference: ${geo.vertCount} verts, ${geo.edges.length} edges; ` +
            `endpoint/midpoint/on-edge/on-face snaps + priority validated`);
