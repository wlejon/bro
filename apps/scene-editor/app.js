// =============================================================================
// Scene editor — picking spike.
//
// Smallest possible end-to-end slice: orbit around a box, click to pick a
// triangle via bromesh's BVH. Everything downstream (EditMesh, face groups,
// tools, inference) builds on the screen→ray→hit pipeline proven here.
// =============================================================================

const canvas   = document.getElementById('canvas');
const scene    = canvas.getContext('scene');
const pickInfo = document.getElementById('pick-info');
const toolName = document.getElementById('tool-name');

// --- Geometry: one box. Picking/edit state is rebuilt whenever the mesh
// mutates (currently only push/pull). Buffers declared `let` so they can be
// replaced in place, but the scene node is mutated via node.updateMesh().

let boxMesh      = Mesh.box(1, 1, 1);
let boxPositions = boxMesh.positions;
let boxIndices   = boxMesh.indices;
let boxNormals   = boxMesh.normals;
let boxBVH       = new MeshBVH(boxMesh);

const boxNode = scene.createMesh({
    data: boxMesh,
    color: '#74b9ff',
    name: 'box',
});

// --- Highlight overlay ------------------------------------------------------
//
// The highlight is a 1-triangle scene node rebuilt on each pick, nudged along
// the face normal to stay above the underlying box (z-fighting would flicker
// the selection). Kept as its own node so we never mutate boxMesh.

const HIGHLIGHT_EPS = 0.002;
let highlightNode = null;

function clearHighlight() {
    if (highlightNode) {
        highlightNode.destroy();
        highlightNode = null;
    }
}

// Build a highlight scene node from a list of triangle indices into boxMesh.
// Offsets each vertex along the group's normal to stay above the base mesh.
function setHighlightTriangles(triIndices, normal) {
    clearHighlight();
    if (!triIndices || triIndices.length === 0) return;
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
            const src = boxIndices[t * 3 + k] * 3;
            const dst = (i * 3 + k) * 3;
            positions[dst + 0] = boxPositions[src + 0] + nx;
            positions[dst + 1] = boxPositions[src + 1] + ny;
            positions[dst + 2] = boxPositions[src + 2] + nz;
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
}

// Back-compat: highlight a single triangle.
function setHighlightTriangle(triIndex, normal) {
    setHighlightTriangles([triIndex], normal);
}

// --- Face groups ------------------------------------------------------------
//
// A "face" in SketchUp-speak is a maximal set of coplanar, edge-connected
// triangles. We detect them once: union-find over triangle pairs that share
// an edge (by vertex *position*, so hard-edge seams with duplicated indices
// still merge correctly) and whose normals are parallel within epsilon.

function computeFaceGroups(positions, indices, cosTol = 0.9995) {
    const triCount = indices.length / 3;
    const normals = new Float32Array(triCount * 3);
    for (let t = 0; t < triCount; t++) {
        const i0 = indices[t * 3 + 0] * 3;
        const i1 = indices[t * 3 + 1] * 3;
        const i2 = indices[t * 3 + 2] * 3;
        const ax = positions[i1 + 0] - positions[i0 + 0];
        const ay = positions[i1 + 1] - positions[i0 + 1];
        const az = positions[i1 + 2] - positions[i0 + 2];
        const bx = positions[i2 + 0] - positions[i0 + 0];
        const by = positions[i2 + 1] - positions[i0 + 1];
        const bz = positions[i2 + 2] - positions[i0 + 2];
        let nx = ay * bz - az * by;
        let ny = az * bx - ax * bz;
        let nz = ax * by - ay * bx;
        const L = Math.hypot(nx, ny, nz) || 1;
        normals[t * 3 + 0] = nx / L;
        normals[t * 3 + 1] = ny / L;
        normals[t * 3 + 2] = nz / L;
    }

    const parent = new Int32Array(triCount);
    for (let i = 0; i < triCount; i++) parent[i] = i;
    function find(x) {
        while (parent[x] !== x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    }
    function union(a, b) {
        const ra = find(a), rb = find(b);
        if (ra !== rb) parent[rb] = ra;
    }

    // Quantize positions so vertex-index duplicates at hard-edge seams still
    // match across their shared edge.
    const Q = 1e5;
    function posKey(vi) {
        const p = vi * 3;
        return Math.round(positions[p] * Q) + ',' +
               Math.round(positions[p + 1] * Q) + ',' +
               Math.round(positions[p + 2] * Q);
    }
    function edgeKey(a, b) {
        const ka = posKey(a), kb = posKey(b);
        return ka < kb ? ka + '|' + kb : kb + '|' + ka;
    }

    const edgeTris = new Map();
    for (let t = 0; t < triCount; t++) {
        for (let e = 0; e < 3; e++) {
            const ia = indices[t * 3 + e];
            const ib = indices[t * 3 + ((e + 1) % 3)];
            const k = edgeKey(ia, ib);
            const arr = edgeTris.get(k);
            if (arr) arr.push(t); else edgeTris.set(k, [t]);
        }
    }

    for (const tris of edgeTris.values()) {
        for (let i = 0; i < tris.length; i++) {
            for (let j = i + 1; j < tris.length; j++) {
                const a = tris[i], b = tris[j];
                const dot = normals[a * 3 + 0] * normals[b * 3 + 0] +
                            normals[a * 3 + 1] * normals[b * 3 + 1] +
                            normals[a * 3 + 2] * normals[b * 3 + 2];
                if (dot > cosTol) union(a, b);
            }
        }
    }

    const rootToIdx = new Map();
    const triToGroup = new Int32Array(triCount);
    const groups = [];
    for (let t = 0; t < triCount; t++) {
        const r = find(t);
        let gi = rootToIdx.get(r);
        if (gi === undefined) {
            gi = groups.length;
            rootToIdx.set(r, gi);
            groups.push({
                tris: [],
                normal: [normals[r * 3 + 0], normals[r * 3 + 1], normals[r * 3 + 2]],
            });
        }
        triToGroup[t] = gi;
        groups[gi].tris.push(t);
    }
    return { groups, triToGroup };
}

let faceGroups = computeFaceGroups(boxPositions, boxIndices);

// Live edit-mesh backing the box. Not used for rendering yet — the scene node
// still renders boxMesh directly — but future editing tools will mutate this
// and then push a fresh MeshData back into the scene.
let editMesh = EditMesh.fromMeshData(boxPositions, boxIndices);

// Rebuild all mesh-derived state after positions have been mutated in place.
// Indices and normals are stable across push/pull so we don't touch them;
// callers must update boxPositions before invoking.
function rebuildMeshState() {
    boxMesh.positions = boxPositions;
    boxBVH     = new MeshBVH(boxMesh);
    faceGroups = computeFaceGroups(boxPositions, boxIndices);
    editMesh   = EditMesh.fromMeshData(boxPositions, boxIndices);
}

function setHighlightFaceGroup(groupIdx) {
    const g = faceGroups.groups[groupIdx];
    if (!g) { clearHighlight(); return; }
    setHighlightTriangles(g.tris, g.normal);
}

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

    // Camera basis from position/target/up.
    const fx = opts.target[0] - opts.position[0];
    const fy = opts.target[1] - opts.position[1];
    const fz = opts.target[2] - opts.position[2];
    const fl = Math.hypot(fx, fy, fz) || 1;
    const f  = [fx / fl, fy / fl, fz / fl];

    const up = opts.up;
    // right = normalize(cross(f, up))
    let rx = f[1] * up[2] - f[2] * up[1];
    let ry = f[2] * up[0] - f[0] * up[2];
    let rz = f[0] * up[1] - f[1] * up[0];
    const rl = Math.hypot(rx, ry, rz) || 1;
    rx /= rl; ry /= rl; rz /= rl;

    // trueUp = cross(r, f)
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

function pickAt(px, py) {
    const ray = screenToRay(px, py);
    return boxBVH.raycast(boxMesh, ray.origin, ray.dir, 0);
}

function formatHit(hit) {
    if (!hit) return 'no pick';
    const p = hit.position;
    const n = hit.normal;
    return `tri ${hit.triangleIndex}  dist ${hit.distance.toFixed(3)}\n` +
           `pos [${p[0].toFixed(2)}, ${p[1].toFixed(2)}, ${p[2].toFixed(2)}]\n` +
           `nrm [${n[0].toFixed(2)}, ${n[1].toFixed(2)}, ${n[2].toFixed(2)}]`;
}

// --- Tool modes -------------------------------------------------------------
//
// 'select'    — left-click picks a face group and highlights it (original UX).
// 'pushpull'  — left-drag on a face group extrudes along the face normal.
//               Drag distance is the closest-point parameter between the
//               cursor ray and the infinite line through the initial click
//               along the face normal; no snapping yet.

let currentTool = 'select';

function setTool(t) {
    if (t === currentTool) return;
    if (pushpull.active) cancelPushPull();
    currentTool = t;
    if (toolName) toolName.textContent = t;
    clearHighlight();
    pickInfo.textContent = 'no pick';
}

// --- Push/Pull --------------------------------------------------------------

const POS_QUANT = 1e5;
function quantizeKey(x, y, z) {
    return Math.round(x * POS_QUANT) + ',' +
           Math.round(y * POS_QUANT) + ',' +
           Math.round(z * POS_QUANT);
}

// Every MeshData vertex whose quantized position matches a face-group vertex
// must translate with the face — otherwise hard-edge seams (duplicated
// indices at the same position across adjacent face groups) would tear. For
// a cube-top push this picks up 12 indices (4 corners × 3 incident faces)
// so the 3 touching side faces stretch instead of ripping.
function collectAffectedVertexIndices(groupIdx) {
    const tris = faceGroups.groups[groupIdx].tris;
    const keys = new Set();
    for (const t of tris) {
        for (let k = 0; k < 3; k++) {
            const vi = boxIndices[t * 3 + k];
            keys.add(quantizeKey(
                boxPositions[vi * 3 + 0],
                boxPositions[vi * 3 + 1],
                boxPositions[vi * 3 + 2]));
        }
    }
    const vertCount = boxPositions.length / 3;
    const out = [];
    for (let vi = 0; vi < vertCount; vi++) {
        const k = quantizeKey(
            boxPositions[vi * 3 + 0],
            boxPositions[vi * 3 + 1],
            boxPositions[vi * 3 + 2]);
        if (keys.has(k)) out.push(vi);
    }
    return Uint32Array.from(out);
}

// Closest-point parameter along the axis-line passing through `pivot` for a
// cursor ray. Returns t such that (pivot + t*axis) is the closest point on
// the axis-line to the ray — this is the push/pull distance that makes the
// face follow the cursor. Returns 0 if the ray is degenerate (parallel to
// the axis), which harmlessly freezes the drag until the cursor moves off
// the axis.
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
    groupIdx: -1,
    axis: [0, 0, 0],
    pivot: [0, 0, 0],
    vertexIndices: null,     // Uint32Array
    vertexStart: null,       // Float32Array, xyz per index (pre-drag snapshot)
    workingPositions: null,  // Float32Array, scratch buffer reused each move
    distance: 0,
};

function beginPushPull(hit) {
    const gIdx = faceGroups.triToGroup[hit.triangleIndex];
    const g = faceGroups.groups[gIdx];
    const idxs = collectAffectedVertexIndices(gIdx);
    const snap = new Float32Array(idxs.length * 3);
    for (let i = 0; i < idxs.length; i++) {
        const vi = idxs[i];
        snap[i * 3 + 0] = boxPositions[vi * 3 + 0];
        snap[i * 3 + 1] = boxPositions[vi * 3 + 1];
        snap[i * 3 + 2] = boxPositions[vi * 3 + 2];
    }
    pushpull.active = true;
    pushpull.groupIdx = gIdx;
    pushpull.axis = g.normal.slice();
    pushpull.pivot = hit.position.slice();
    pushpull.vertexIndices = idxs;
    pushpull.vertexStart = snap;
    pushpull.workingPositions = new Float32Array(boxPositions.length);
    pushpull.distance = 0;
    clearHighlight();
}

// Apply the current drag distance to the scene node via updateMesh (does not
// touch boxPositions — that only happens on commit).
function applyPushPull(t) {
    pushpull.distance = t;
    const work = pushpull.workingPositions;
    work.set(boxPositions);
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
    boxNode.updateMesh({
        positions: work,
        indices: boxIndices,
        normals: boxNormals,
    });
    pickInfo.textContent = `push/pull  ${t.toFixed(3)}`;
}

function commitPushPull() {
    if (!pushpull.active) return;
    // Bake workingPositions into boxPositions; rebuild BVH/faceGroups/editMesh.
    boxPositions = new Float32Array(pushpull.workingPositions);
    rebuildMeshState();
    pushpull.active = false;
    pushpull.vertexIndices = null;
    pushpull.vertexStart = null;
    pushpull.workingPositions = null;
    pickInfo.textContent = `extruded ${pushpull.distance.toFixed(3)}`;
}

function cancelPushPull() {
    if (!pushpull.active) return;
    // Restore original geometry from the snapshot and push it back to the scene.
    const idxs = pushpull.vertexIndices;
    const snap = pushpull.vertexStart;
    for (let i = 0; i < idxs.length; i++) {
        const vi = idxs[i];
        boxPositions[vi * 3 + 0] = snap[i * 3 + 0];
        boxPositions[vi * 3 + 1] = snap[i * 3 + 1];
        boxPositions[vi * 3 + 2] = snap[i * 3 + 2];
    }
    boxNode.updateMesh({
        positions: boxPositions,
        indices: boxIndices,
        normals: boxNormals,
    });
    pushpull.active = false;
    pushpull.vertexIndices = null;
    pushpull.vertexStart = null;
    pushpull.workingPositions = null;
    pickInfo.textContent = 'push/pull cancelled';
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
    const hit = pickAt(e.clientX - r.left, e.clientY - r.top);
    pickInfo.textContent = formatHit(hit);
    window.__lastPick = hit;
    if (currentTool === 'select') {
        if (hit) setHighlightFaceGroup(faceGroups.triToGroup[hit.triangleIndex]);
        else clearHighlight();
    } else if (currentTool === 'pushpull') {
        if (hit) beginPushPull(hit);
        else clearHighlight();
    }
}

canvas.addEventListener('mousedown', (e) => {
    if (e.button === 0) {
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
    if (pushpull.active) {
        const r = canvas.getBoundingClientRect();
        const ray = screenToRay(e.clientX - r.left, e.clientY - r.top);
        applyPushPull(rayVsAxisDistance(ray, pushpull.pivot, pushpull.axis));
    }
});
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
canvas.addEventListener('auxclick',    (e) => { if (e.button === 1) e.preventDefault(); });
canvas.addEventListener('wheel', (e) => {
    const factor = Math.exp(e.deltaY * 0.001);
    cam.dist = Math.max(0.1, cam.dist * factor);
    applyCamera();
    e.preventDefault();
});
document.addEventListener('keydown', (e) => {
    // Ignore keybinds while typing in an input (none today, but future-proof).
    if (e.target && (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA')) return;
    const k = e.key.toLowerCase();
    if (k === 's') setTool('select');
    else if (k === 'p') setTool('pushpull');
    else if (k === 'escape') cancelPushPull();
});

// --- Test hook --------------------------------------------------------------
//
// Getters for mutable references so tests always see the current rebuilt
// state after push/pull commits, not the pre-edit binding.

window.__editor = {
    scene, cam, boxNode, pickAt, screenToRay,
    get boxMesh()      { return boxMesh; },
    get boxBVH()       { return boxBVH; },
    get boxPositions() { return boxPositions; },
    get boxIndices()   { return boxIndices; },
    get boxNormals()   { return boxNormals; },
    get faceGroups()   { return faceGroups; },
    get editMesh()     { return editMesh; },
    get highlightNode(){ return highlightNode; },
    get currentTool()  { return currentTool; },
    clearHighlight, setHighlightTriangle, setHighlightTriangles,
    computeFaceGroups, setHighlightFaceGroup,
    setTool,
    // Push/Pull programmatic hooks for headless testing.
    beginPushPull, applyPushPull, commitPushPull, cancelPushPull,
    collectAffectedVertexIndices, rayVsAxisDistance,
    get pushpull() { return pushpull; },
};
