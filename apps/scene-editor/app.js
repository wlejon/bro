// =============================================================================
// Scene editor — entrypoint.
//
// Orchestrates orbit camera, tool modes, input, and the VCB. Per-object state
// (mesh, BVH, face groups, inference geo, render + edges nodes) lives on
// Primitive; the PrimitiveRegistry tracks every editable object and powers
// pickAt + multi-primitive inference. The app itself is mostly tool modes,
// event routing, and UI glue.
// =============================================================================

const canvas     = document.getElementById('canvas');
const scene      = canvas.getContext('scene');
const pickInfo   = document.getElementById('pick-info');
const toolName   = document.getElementById('tool-name');
const snapInfo   = document.getElementById('snap-info');
const snapMarker = document.getElementById('snap-marker');

// --- Registry of editable primitives ---------------------------------------
//
// The app works entirely through the registry — no hard-coded per-object
// bindings. The default scene seeds one box (same as the spike) so existing
// tests and the on-open experience stay identical.

const registry = new PrimitiveRegistry({ scene });

registry.create({
    type: 'box',
    name: 'Box',
    color: '#74b9ff',
    params: { sx: 1, sy: 1, sz: 1 },
});

// --- Highlight overlay ------------------------------------------------------
//
// Shared across primitives (only one drag / selection at a time). The
// highlight node lives outside any primitive — it's pure UI and rebuilt on
// every pick.

const HIGHLIGHT_EPS = 0.002;
let highlightNode      = null;
let highlightPrimitive = null;

function clearHighlight() {
    if (highlightNode) { highlightNode.destroy(); highlightNode = null; }
    highlightPrimitive = null;
}

// Build a highlight scene node from triangle indices into `primitive`. The
// overlay is nudged along the group normal to avoid z-fighting. During
// push/pull, the working (not-yet-committed) buffers can be passed as
// overrides so the highlight tracks the drag.
function setHighlightTriangles(primitive, triIndices, normal, positionsSrc, indicesSrc) {
    clearHighlight();
    if (!primitive || !triIndices || triIndices.length === 0) return;
    const posSrc = positionsSrc || primitive.positions;
    const idxSrc = indicesSrc   || primitive.indices;
    const nx = normal[0] * HIGHLIGHT_EPS;
    const ny = normal[1] * HIGHLIGHT_EPS;
    const nz = normal[2] * HIGHLIGHT_EPS;
    const n = triIndices.length;
    const positions = new Float32Array(n * 9);
    const normals   = new Float32Array(n * 9);
    const indices   = new Uint32Array(n * 3);
    for (let i = 0; i < n; i++) {
        const t = triIndices[i];
        for (let k = 0; k < 3; k++) {
            const src = idxSrc[t * 3 + k] * 3;
            const dst = (i * 3 + k) * 3;
            positions[dst + 0] = posSrc[src + 0] + nx;
            positions[dst + 1] = posSrc[src + 1] + ny;
            positions[dst + 2] = posSrc[src + 2] + nz;
            normals[dst + 0] = normal[0];
            normals[dst + 1] = normal[1];
            normals[dst + 2] = normal[2];
        }
        indices[i * 3 + 0] = i * 3 + 0;
        indices[i * 3 + 1] = i * 3 + 1;
        indices[i * 3 + 2] = i * 3 + 2;
    }
    highlightNode = scene.createMesh({
        positions, normals, indices,
        color: '#ffa502',
        emissive: 0.6,
        name: 'highlight',
    });
    highlightPrimitive = primitive;
}

function setHighlightTriangle(primitive, triIndex, normal) {
    setHighlightTriangles(primitive, [triIndex], normal);
}

function setHighlightFaceGroup(primitive, groupIdx) {
    if (!primitive) { clearHighlight(); return; }
    const g = primitive.faceGroups.groups[groupIdx];
    if (!g) { clearHighlight(); return; }
    setHighlightTriangles(primitive, g.tris, g.normal);
}

// --- Ground grid + XYZ axes -------------------------------------------------
//
// Static visual aid; not a primitive, just a single mesh sitting outside the
// registry so it never participates in picking or snapping.

const sceneAxesData = SceneAxes.buildSceneAxes();
const sceneAxesNode = scene.createMesh({
    positions: sceneAxesData.positions,
    normals:   sceneAxesData.normals,
    colors:    sceneAxesData.colors,
    indices:   sceneAxesData.indices,
    emissive:  0.85,
    name: 'scene-axes',
});

// --- Camera -----------------------------------------------------------------

const cam = Camera.createOrbit({ target: [0, 0, 0], dist: 4, fov: 45 });

function applyCamera() {
    scene.setCamera(Camera.orbitViewOpts(cam, canvas));
}
applyCamera();

// --- Screen → world ray -----------------------------------------------------

function screenToRay(px, py) {
    const opts = Camera.orbitViewOpts(cam, canvas);
    const w = canvas.clientWidth || canvas.width;
    const h = canvas.clientHeight || canvas.height;
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
    dx /= dl; dy /= dl; dz /= dl;

    return {
        origin: [opts.position[0], opts.position[1], opts.position[2]],
        dir:    [dx, dy, dz],
    };
}

// pickAt returns { primitive, hit } or null.
function pickAt(px, py) {
    const ray = screenToRay(px, py);
    return registry.pickAt(ray.origin, ray.dir);
}

function formatHit(pick) {
    if (!pick) return 'no pick';
    const hit = pick.hit;
    const p = hit.position;
    const n = hit.normal;
    return `[${pick.primitive.name}] tri ${hit.triangleIndex}  dist ${hit.distance.toFixed(3)}\n` +
           `pos [${p[0].toFixed(2)}, ${p[1].toFixed(2)}, ${p[2].toFixed(2)}]\n` +
           `nrm [${n[0].toFixed(2)}, ${n[1].toFixed(2)}, ${n[2].toFixed(2)}]`;
}

// --- Tool modes -------------------------------------------------------------

let currentTool = 'select';

function setTool(t) {
    if (t === currentTool) return;
    if (pushpull.active) cancelPushPull();
    MeasureBox.clearLastOp(measureBoxState);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
    currentTool = t;
    if (toolName) toolName.textContent = t;
    clearHighlight();
    showSnapMarker(null);
    pickInfo.textContent = 'no pick';
}

// --- Push/Pull --------------------------------------------------------------

// Closest-point parameter along the axis-line passing through `pivot` for a
// cursor ray. Returns t such that (pivot + t*axis) is the closest point on
// the axis-line to the ray. Returns 0 if the ray is parallel to the axis
// (harmlessly freezes the drag until the cursor moves off-axis).
function rayVsAxisDistance(ray, pivot, axis) {
    const wx = ray.origin[0] - pivot[0];
    const wy = ray.origin[1] - pivot[1];
    const wz = ray.origin[2] - pivot[2];
    const b = ray.dir[0] * axis[0] + ray.dir[1] * axis[1] + ray.dir[2] * axis[2];
    const dDot = ray.dir[0] * wx + ray.dir[1] * wy + ray.dir[2] * wz;
    const eDot = axis[0] * wx + axis[1] * wy + axis[2] * wz;
    const denom = 1 - b * b;
    if (denom < 1e-6) return 0;
    return (eDot - b * dDot) / denom;
}

const pushpull = {
    active: false,
    primitive: null,        // Primitive the drag is bound to
    groupIdx: -1,
    faceTriangles: null,
    axis: [0, 0, 0],
    pivot: [0, 0, 0],
    vertexIndices: null,    // indices into primitive.positions
    vertexStart: null,      // xyz snapshot of the affected verts pre-drag
    workingPositions: null, // Float32Array scratch, rebuilt each move
    workingIndices: null,   // winding-flipped when inverted
    workingNormals: null,   // axis-component flipped when inverted
    distance: 0,
    inversionT: 0,
    inverted: false,
};

function flipNormalsAlongAxis(normals, axis) {
    const ax = axis[0], ay = axis[1], az = axis[2];
    const n = normals.length / 3;
    for (let i = 0; i < n; i++) {
        const ix = i * 3;
        const nx = normals[ix + 0], ny = normals[ix + 1], nz = normals[ix + 2];
        const d = nx * ax + ny * ay + nz * az;
        if (d === 0) continue;
        const k = 2 * d;
        normals[ix + 0] = nx - k * ax;
        normals[ix + 1] = ny - k * ay;
        normals[ix + 2] = nz - k * az;
    }
}

function flipAllWinding(indices) {
    const triCount = indices.length / 3;
    for (let t = 0; t < triCount; t++) {
        const b = indices[t * 3 + 1];
        indices[t * 3 + 1] = indices[t * 3 + 2];
        indices[t * 3 + 2] = b;
    }
}

// Drag distance at which the pushed face crosses the farthest non-affected
// vertex along the axis — the "push through" threshold.
function computeInversionT(positions, axis, pivot, affectedSet) {
    let m = Infinity;
    const vertCount = positions.length / 3;
    for (let vi = 0; vi < vertCount; vi++) {
        if (affectedSet.has(vi)) continue;
        const proj = positions[vi * 3 + 0] * axis[0] +
                     positions[vi * 3 + 1] * axis[1] +
                     positions[vi * 3 + 2] * axis[2];
        if (proj < m) m = proj;
    }
    const p0 = pivot[0] * axis[0] + pivot[1] * axis[1] + pivot[2] * axis[2];
    return m - p0;
}

// Start a push/pull on the given primitive + hit. The primitive reference is
// captured on the drag state, so a mid-drag active-primitive change (e.g.
// user clicks a different row in the outliner) doesn't redirect the commit.
function beginPushPull(primitive, hit) {
    if (!primitive) return;
    const gIdx = primitive.faceGroups.triToGroup[hit.triangleIndex];
    const g = primitive.faceGroups.groups[gIdx];
    const idxs = primitive.collectAffectedVertexIndices(gIdx);
    const snap = new Float32Array(idxs.length * 3);
    const affectedSet = new Set();
    for (let i = 0; i < idxs.length; i++) {
        const vi = idxs[i];
        affectedSet.add(vi);
        snap[i * 3 + 0] = primitive.positions[vi * 3 + 0];
        snap[i * 3 + 1] = primitive.positions[vi * 3 + 1];
        snap[i * 3 + 2] = primitive.positions[vi * 3 + 2];
    }
    pushpull.active = true;
    pushpull.primitive = primitive;
    pushpull.groupIdx = gIdx;
    pushpull.faceTriangles = g.tris.slice();
    pushpull.axis = g.normal.slice();
    pushpull.pivot = hit.position.slice();
    pushpull.vertexIndices = idxs;
    pushpull.vertexStart = snap;
    pushpull.workingPositions = new Float32Array(primitive.positions.length);
    pushpull.workingIndices = new Uint32Array(primitive.indices);
    pushpull.workingNormals = primitive.normals ? new Float32Array(primitive.normals) : null;
    pushpull.distance = 0;
    pushpull.inversionT = computeInversionT(
        primitive.positions, pushpull.axis, pushpull.pivot, affectedSet);
    pushpull.inverted = false;
    setHighlightTriangles(
        primitive, pushpull.faceTriangles, pushpull.axis,
        primitive.positions, pushpull.workingIndices);
    MeasureBox.clearLastOp(measureBoxState);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, true);
    renderMeasureBox();
}

function applyPushPull(t) {
    if (!pushpull.active) return;
    const prim = pushpull.primitive;
    pushpull.distance = t;
    const work = pushpull.workingPositions;
    work.set(prim.positions);
    const ax = pushpull.axis[0] * t;
    const ay = pushpull.axis[1] * t;
    const az = pushpull.axis[2] * t;
    const idxs = pushpull.vertexIndices;
    const snap = pushpull.vertexStart;
    for (let i = 0; i < idxs.length; i++) {
        const vi = idxs[i];
        work[vi * 3 + 0] = snap[i * 3 + 0] + ax;
        work[vi * 3 + 1] = snap[i * 3 + 1] + ay;
        work[vi * 3 + 2] = snap[i * 3 + 2] + az;
    }

    const newInverted = t < pushpull.inversionT;
    if (newInverted !== pushpull.inverted) {
        flipAllWinding(pushpull.workingIndices);
        if (pushpull.workingNormals) {
            flipNormalsAlongAxis(pushpull.workingNormals, pushpull.axis);
        }
        pushpull.inverted = newInverted;
    }

    prim.previewMesh(
        work, pushpull.workingIndices,
        pushpull.workingNormals || prim.normals);

    const hlNormal = pushpull.inverted
        ? [-pushpull.axis[0], -pushpull.axis[1], -pushpull.axis[2]]
        : pushpull.axis;
    setHighlightTriangles(
        prim, pushpull.faceTriangles, hlNormal, work, pushpull.workingIndices);

    pickInfo.textContent = `push/pull  ${t.toFixed(3)}` +
        (pushpull.inverted ? '  [inverted]' : '');
}

function commitPushPull() {
    if (!pushpull.active) return;
    const prim = pushpull.primitive;
    // Stash the last-op (including the target primitive id) before pushpull
    // is cleared — redo needs a stable handle even if the active primitive
    // changes afterward.
    const lastOp = {
        primitiveId: prim.id,
        normal: pushpull.axis.slice(),
        centroid: prim.faceGroupCentroid(pushpull.groupIdx),
        distance: pushpull.distance,
    };
    lastOp.centroid[0] += pushpull.axis[0] * pushpull.distance;
    lastOp.centroid[1] += pushpull.axis[1] * pushpull.distance;
    lastOp.centroid[2] += pushpull.axis[2] * pushpull.distance;

    const newPositions = new Float32Array(pushpull.workingPositions);
    const newIndices   = new Uint32Array(pushpull.workingIndices);
    const newNormals   = pushpull.workingNormals
        ? new Float32Array(pushpull.workingNormals) : null;
    prim.updateGeometry(newPositions, newIndices, newNormals);

    pickInfo.textContent =
        `extruded ${pushpull.distance.toFixed(3)}` +
        (pushpull.inverted ? '  [inverted through]' : '');
    clearPushPull();
    clearHighlight();
    MeasureBox.clear(measureBoxState);
    MeasureBox.setLastOp(measureBoxState, lastOp);
    MeasureBox.setActive(measureBoxState, true);
    renderMeasureBox();
}

function cancelPushPull() {
    if (!pushpull.active) return;
    pushpull.primitive.revertMesh();
    pickInfo.textContent = 'push/pull cancelled';
    clearPushPull();
    clearHighlight();
    MeasureBox.clearLastOp(measureBoxState);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
}

function clearPushPull() {
    pushpull.active = false;
    pushpull.primitive = null;
    pushpull.faceTriangles = null;
    pushpull.vertexIndices = null;
    pushpull.vertexStart = null;
    pushpull.workingPositions = null;
    pushpull.workingIndices = null;
    pushpull.workingNormals = null;
    pushpull.inverted = false;
    activeSnap = null;
}

// --- Inference snap marker --------------------------------------------------

const SNAP_SHAPES = {
    'endpoint': '<circle cx="9" cy="9" r="5" fill="#2ecc71" stroke="#fff" stroke-width="1.5"/>',
    'midpoint': '<polygon points="9,2 16,9 9,16 2,9" fill="none" stroke="#1abc9c" stroke-width="2"/>',
    'on-edge':  '<rect x="3" y="3" width="12" height="12" fill="none" stroke="#e74c3c" stroke-width="2"/>',
    'on-face':  '<polygon points="9,3 15,9 9,15 3,9" fill="#3498db" fill-opacity="0.4" stroke="#3498db" stroke-width="1.5"/>',
};

const SNAP_SPHERE_RADIUS = 0.04;
const snapSphereMesh = Mesh.sphere(SNAP_SPHERE_RADIUS, 12, 8);
const snapSpheres = {};
for (const [type, color] of Object.entries(Inference._COLOR)) {
    const node = scene.createMesh({
        data: snapSphereMesh,
        color,
        emissive: 0.8,
        name: 'snap-sphere-' + type,
    });
    node.visible = false;
    snapSpheres[type] = node;
}

let activeSnap = null;

// --- Measurement Box (VCB) --------------------------------------------------

const measureBoxState = MeasureBox.createState();
const measureBoxEl    = document.getElementById('measure-box');

function renderMeasureBox() {
    if (!measureBoxEl) return;
    if (!measureBoxState.active) {
        measureBoxEl.style.display = 'none';
        return;
    }
    measureBoxEl.style.display = 'block';
    const buf = measureBoxState.buffer;
    const display = buf.length ? buf : '';
    const hint = measureBoxState.lastOp && !pushpull.active
        ? 'Type distance + Enter to re-extrude · Esc to dismiss'
        : 'Type exact distance + Enter · Esc cancel';
    measureBoxEl.innerHTML =
        '<div class="label">Distance</div>' +
        '<div><span class="value">' + (display || '&mdash;') + '</span>' +
        '<span class="caret"></span></div>' +
        '<div class="hint">' + hint + '</div>';
}
renderMeasureBox();

// Re-apply the last committed push/pull at a new distance on the same
// primitive it originally ran on. Normal-match + centroid-proximity relocate
// the pushed face on the post-commit geometry.
function redoLastPushPull(distance) {
    const op = measureBoxState.lastOp;
    if (!op) return false;
    const prim = registry.getById(op.primitiveId);
    if (!prim) return false;
    const gIdx = prim.findFaceGroupByNormal(op.normal, op.centroid);
    if (gIdx < 0) return false;
    const g = prim.faceGroups.groups[gIdx];
    const centroid = prim.faceGroupCentroid(gIdx);
    beginPushPull(prim, {
        triangleIndex: g.tris[0],
        position: centroid,
        normal: g.normal.slice(),
        distance: 0,
    });
    applyPushPull(distance);
    commitPushPull();
    return true;
}

function hideSnapSpheres() {
    for (const t in snapSpheres) snapSpheres[t].visible = false;
}

function showSnapMarker(snap) {
    activeSnap = snap;
    if (!snap) {
        snapMarker.style.display = 'none';
        if (snapInfo) snapInfo.textContent = '';
        hideSnapSpheres();
        return;
    }
    snapMarker.style.display = 'block';
    snapMarker.style.left = snap.screen.x + 'px';
    snapMarker.style.top  = snap.screen.y + 'px';
    snapMarker.innerHTML  = '<svg width="18" height="18">' +
        (SNAP_SHAPES[snap.type] || '') + '</svg>';
    if (snapInfo) {
        snapInfo.innerHTML = `snap: <b style="color:${snap.color}">${snap.label}</b>`;
    }
    hideSnapSpheres();
    const sph = snapSpheres[snap.type];
    if (sph) {
        sph.x = snap.position[0];
        sph.y = snap.position[1];
        sph.z = snap.position[2];
        sph.visible = true;
    }
}

const PUSHPULL_EXCLUDE = ['on-edge'];

// Resolve the best snap across every visible primitive. The on-face fallback
// uses registry.pickAt so it hits the nearest primitive under the ray.
function resolveSnap(cx, cy, ray, includeFaceFallback, excludeTypes) {
    const camOpts = Camera.orbitViewOpts(cam, canvas);
    const w = canvas.clientWidth || canvas.width;
    const h = canvas.clientHeight || canvas.height;
    let onFaceHit = null;
    if (includeFaceFallback) {
        const pick = registry.pickAt(ray.origin, ray.dir);
        if (pick) onFaceHit = pick.hit;
    }
    return Inference.findSnap({
        cursorX: cx, cursorY: cy, ray,
        camOpts, width: w, height: h,
        geos: registry.collectInferenceGeos(),
        onFaceHit,
        excludeTypes,
    });
}

function snapAxisDistance(snap, pivot, axis) {
    const dx = snap.position[0] - pivot[0];
    const dy = snap.position[1] - pivot[1];
    const dz = snap.position[2] - pivot[2];
    return dx * axis[0] + dy * axis[1] + dz * axis[2];
}

// --- Input: right=rotate, middle=pan, wheel=zoom, left=tool ----------------

let rightDown  = false;
let middleDown = false;

function updatePointerLock() {
    const want = rightDown || middleDown;
    const locked = document.pointerLockElement === canvas;
    if (want && !locked) canvas.requestPointerLock();
    else if (!want && locked) document.exitPointerLock();
}

function handleLeftDown(e) {
    const r = canvas.getBoundingClientRect();
    const pick = pickAt(e.clientX - r.left, e.clientY - r.top);
    pickInfo.textContent = formatHit(pick);
    // Legacy test hook: keep `__lastPick` in the hit shape (not {primitive,hit}).
    window.__lastPick = pick && pick.hit;
    if (currentTool === 'select') {
        if (pick) {
            registry.setActive(pick.primitive.id);
            setHighlightFaceGroup(
                pick.primitive,
                pick.primitive.faceGroups.triToGroup[pick.hit.triangleIndex]);
        } else {
            clearHighlight();
        }
    } else if (currentTool === 'pushpull') {
        if (pick) {
            registry.setActive(pick.primitive.id);
            beginPushPull(pick.primitive, pick.hit);
        } else {
            clearHighlight();
        }
    }
}

canvas.addEventListener('mousedown', (e) => {
    if (e.button === 0) {
        if (!pushpull.active && measureBoxState.lastOp) {
            MeasureBox.clearLastOp(measureBoxState);
            MeasureBox.clear(measureBoxState);
            MeasureBox.setActive(measureBoxState, false);
            renderMeasureBox();
        }
        handleLeftDown(e);
    } else if (e.button === 2) {
        rightDown = true;
        e.preventDefault();
        updatePointerLock();
    } else if (e.button === 1) {
        middleDown = true;
        e.preventDefault();
        updatePointerLock();
    }
});
document.addEventListener('mouseup', (e) => {
    if (e.button === 0 && pushpull.active) commitPushPull();
    if (e.button === 2) rightDown  = false;
    if (e.button === 1) middleDown = false;
    updatePointerLock();
});
document.addEventListener('mousemove', (e) => {
    if (rightDown)  { Camera.orbitLook(cam, e.movementX, e.movementY); applyCamera(); return; }
    if (middleDown) { Camera.orbitPan (cam, e.movementX, e.movementY); applyCamera(); return; }
    const r = canvas.getBoundingClientRect();
    const cx = e.clientX - r.left;
    const cy = e.clientY - r.top;
    const ray = screenToRay(cx, cy);
    if (pushpull.active) {
        const vcbVal = MeasureBox.parseValue(measureBoxState.buffer);
        if (vcbVal !== null) {
            showSnapMarker(resolveSnap(cx, cy, ray, false, PUSHPULL_EXCLUDE));
            return;
        }
        const snap = resolveSnap(cx, cy, ray, false, PUSHPULL_EXCLUDE);
        let dist;
        if (snap) {
            dist = snapAxisDistance(snap, pushpull.pivot, pushpull.axis);
            showSnapMarker(snap);
        } else {
            dist = rayVsAxisDistance(ray, pushpull.pivot, pushpull.axis);
            showSnapMarker(null);
        }
        applyPushPull(dist);
        return;
    }
    showSnapMarker(resolveSnap(cx, cy, ray, false));
});
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
canvas.addEventListener('auxclick',    (e) => { if (e.button === 1) e.preventDefault(); });
canvas.addEventListener('wheel', (e) => {
    const factor = Math.exp(e.deltaY * 0.001);
    cam.dist = Math.max(0.1, cam.dist * factor);
    applyCamera();
    e.preventDefault();
});

function handleMeasureBoxKey(key) {
    if (!measureBoxState.active) return false;
    const action = MeasureBox.feedKey(measureBoxState, key);
    if (action === 'ignored') return false;
    if (action === 'append') {
        renderMeasureBox();
        if (pushpull.active) {
            const v = MeasureBox.parseValue(measureBoxState.buffer);
            if (v !== null) applyPushPull(v);
        }
        return true;
    }
    if (action === 'commit') {
        const v = MeasureBox.parseValue(measureBoxState.buffer);
        if (pushpull.active) {
            applyPushPull(v);
            commitPushPull();
        } else if (measureBoxState.lastOp) {
            MeasureBox.clear(measureBoxState);
            renderMeasureBox();
            redoLastPushPull(v);
        }
        return true;
    }
    if (action === 'cancel') {
        renderMeasureBox();
        if (pushpull.active) {
            cancelPushPull();
        } else {
            MeasureBox.clearLastOp(measureBoxState);
            MeasureBox.setActive(measureBoxState, false);
            renderMeasureBox();
        }
        return true;
    }
    return false;
}

document.addEventListener('keydown', (e) => {
    if (e.target && (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA')) return;
    if (handleMeasureBoxKey(e.key)) { e.preventDefault(); return; }
    const k = e.key.toLowerCase();
    if (k === 's') setTool('select');
    else if (k === 'p') setTool('pushpull');
    else if (k === 'escape') cancelPushPull();
});

// --- Outliner panel --------------------------------------------------------
//
// Left-bottom DOM panel listing every primitive with add-dropdown + per-row
// visibility / delete / rename. Re-renders whenever the registry changes.

const outlinerListEl = document.getElementById('outliner-list');
const outlinerAddEl  = document.getElementById('outliner-add');

// Palette for auto-assigned colors on new primitives (wrap-around). The
// default box uses index 0 — subsequent adds walk the palette.
const OUTLINER_COLORS = [
    '#74b9ff', '#ffa502', '#2ecc71', '#e74c3c',
    '#9b59b6', '#f1c40f', '#1abc9c', '#e67e22',
];

// New primitives spawn along +X offset from origin so they don't stack on
// the default box. Monotonically increasing — deletions don't reclaim slots
// so the UX is "every add goes to fresh space".
let _outlinerNextAddX = 3;

function outlinerStartRename(prim, spanEl) {
    spanEl.setAttribute('contenteditable', 'true');
    spanEl.focus();
    const range = document.createRange();
    range.selectNodeContents(spanEl);
    const sel = window.getSelection();
    sel.removeAllRanges();
    sel.addRange(range);
    const commit = (save) => {
        spanEl.removeAttribute('contenteditable');
        spanEl.onkeydown = null;
        spanEl.onblur = null;
        if (!save) { outlinerRender(); return; }
        const name = (spanEl.textContent || '').trim();
        if (name.length) registry.setName(prim.id, name);
        else outlinerRender();
    };
    spanEl.onblur = () => commit(true);
    spanEl.onkeydown = (e) => {
        if (e.key === 'Enter') { e.preventDefault(); commit(true); }
        else if (e.key === 'Escape') { e.preventDefault(); commit(false); }
    };
}

function outlinerRender() {
    if (!outlinerListEl) return;
    outlinerListEl.innerHTML = '';
    for (const p of registry.primitives) {
        const row = document.createElement('div');
        let cls = 'outliner-row';
        if (p === registry.active) cls += ' active';
        if (!p.visible) cls += ' hidden';
        row.className = cls;
        row.dataset.id = String(p.id);
        row.onclick = () => registry.setActive(p.id);

        const vis = document.createElement('button');
        vis.className = 'outliner-vis';
        // Filled circle = visible; hollow circle = hidden. Glyph-only keeps
        // the panel width tight; title gives accessibility text.
        vis.textContent = p.visible ? '\u25CF' : '\u25CB';
        vis.title = p.visible ? 'Hide' : 'Show';
        vis.onclick = (e) => {
            e.stopPropagation();
            registry.setVisible(p.id, !p.visible);
        };
        row.appendChild(vis);

        const name = document.createElement('span');
        name.className = 'outliner-name';
        name.textContent = p.name;
        name.title = 'Double-click to rename';
        name.ondblclick = (e) => {
            e.stopPropagation();
            outlinerStartRename(p, name);
        };
        row.appendChild(name);

        const del = document.createElement('button');
        del.className = 'outliner-del';
        del.textContent = '\u00D7';
        del.title = 'Delete';
        del.onclick = (e) => {
            e.stopPropagation();
            // If the drag was on this primitive, cancel first — otherwise
            // commit would rebuild a destroyed scene node.
            if (pushpull.active && pushpull.primitive && pushpull.primitive.id === p.id) {
                cancelPushPull();
            }
            if (highlightPrimitive && highlightPrimitive.id === p.id) clearHighlight();
            registry.remove(p.id);
        };
        row.appendChild(del);

        outlinerListEl.appendChild(row);
    }
}

if (outlinerAddEl) {
    outlinerAddEl.onchange = () => {
        const type = outlinerAddEl.value;
        outlinerAddEl.value = '';
        if (!type) return;
        const idx = registry.primitives.length;
        const spec = {
            type,
            name: type[0].toUpperCase() + type.slice(1) + ' ' + (idx + 1),
            color: OUTLINER_COLORS[idx % OUTLINER_COLORS.length],
            position: [_outlinerNextAddX, 0, 0],
        };
        if (type === 'box')      spec.params = { sx: 1, sy: 1, sz: 1 };
        if (type === 'sphere')   spec.params = { r: 1, seg: 24, rings: 16 };
        if (type === 'cylinder') spec.params = { r: 0.8, h: 2, seg: 24 };
        if (type === 'plane')    spec.params = { w: 2, d: 2, sx: 1, sz: 1 };
        _outlinerNextAddX += 2.5;
        registry.create(spec);
    };
}

registry.onChange = outlinerRender;
outlinerRender();

// --- Test hook --------------------------------------------------------------
//
// Getters resolve lazily against `registry.active` so tests always see the
// current rebuilt state after push/pull commits, not a stale pre-edit
// binding. Legacy positional APIs (beginPushPull(hit), setHighlight...)
// assume the active primitive to stay compatible with the pre-refactor tests.

window.__editor = {
    scene, cam, sceneAxesNode,
    registry,
    pickAt, screenToRay,

    // Active-primitive shortcuts (compat with the single-primitive spike).
    get boxNode()      { return registry.active && registry.active.meshNode; },
    get boxMesh()      { return registry.active && registry.active.mesh; },
    get boxBVH()       { return registry.active && registry.active.bvh; },
    get boxPositions() { return registry.active && registry.active.positions; },
    get boxIndices()   { return registry.active && registry.active.indices; },
    get boxNormals()   { return registry.active && registry.active.normals; },
    get faceGroups()   { return registry.active && registry.active.faceGroups; },
    get editMesh()     { return registry.active && registry.active.editMesh; },
    get inferenceGeo() { return registry.active && registry.active.inferenceGeo; },
    get edgesNode()    { return registry.active && registry.active.edgesNode; },
    get highlightNode(){ return highlightNode; },
    get currentTool()  { return currentTool; },
    get activeSnap()   { return activeSnap; },

    clearHighlight,
    setHighlightTriangle:  (triIdx, normal) =>
        setHighlightTriangle(registry.active, triIdx, normal),
    setHighlightTriangles: (triIdxs, normal, positionsSrc, indicesSrc) =>
        setHighlightTriangles(registry.active, triIdxs, normal, positionsSrc, indicesSrc),
    setHighlightFaceGroup: (groupIdx) =>
        setHighlightFaceGroup(registry.active, groupIdx),
    computeFaceGroups: Primitive.computeFaceGroups,
    setTool,

    // Tools — primitive-aware API plus a legacy single-arg shim that assumes
    // the active primitive.
    beginPushPull:       (hit) => beginPushPull(registry.active, hit),
    beginPushPullOn:     (primitive, hit) => beginPushPull(primitive, hit),
    applyPushPull, commitPushPull, cancelPushPull,
    collectAffectedVertexIndices: (groupIdx) =>
        registry.active.collectAffectedVertexIndices(groupIdx),
    rayVsAxisDistance,
    get pushpull() { return pushpull; },

    // Inference hooks.
    resolveSnap, snapAxisDistance, showSnapMarker,

    // VCB.
    measureBoxState, renderMeasureBox, handleMeasureBoxKey,
    redoLastPushPull,
    findFaceGroupByNormal: (n, ref) => registry.active.findFaceGroupByNormal(n, ref),
    faceGroupCentroid:     (gIdx)   => registry.active.faceGroupCentroid(gIdx),

    // Outliner hooks (UI is in DOM; tests can trigger the internal helpers
    // without synthesizing events).
    outlinerRender,
};
