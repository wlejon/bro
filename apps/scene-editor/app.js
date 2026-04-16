// =============================================================================
// Scene editor — picking spike.
//
// Smallest possible end-to-end slice: orbit around a box, click to pick a
// triangle via bromesh's BVH. Everything downstream (EditMesh, face groups,
// tools, inference) builds on the screen→ray→hit pipeline proven here.
// =============================================================================

const canvas     = document.getElementById('canvas');
const scene      = canvas.getContext('scene');
const pickInfo   = document.getElementById('pick-info');
const toolName   = document.getElementById('tool-name');
const snapInfo   = document.getElementById('snap-info');
const snapMarker = document.getElementById('snap-marker');

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

// Build a highlight scene node from a list of triangle indices into the
// source mesh. Offsets each vertex along the group's normal to stay above
// the base mesh. `positionsSrc`/`indicesSrc` let push/pull drag overlay the
// highlight on the working (not-yet-committed) geometry.
function setHighlightTriangles(triIndices, normal, positionsSrc, indicesSrc) {
    clearHighlight();
    if (!triIndices || triIndices.length === 0) return;
    const posSrc = positionsSrc || boxPositions;
    const idxSrc = indicesSrc   || boxIndices;
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

// Snap-feature index for the inference engine: deduped vertices + model edges.
// Re-derived alongside the BVH/face-groups whenever geometry mutates.
let inferenceGeo = Inference.buildInferenceGeo(boxPositions, boxIndices, faceGroups);

// --- Ground grid + XYZ axes -------------------------------------------------
//
// Static visual aid: faint XZ grid + bright RGB axes at y=-1 (where the box's
// base sits). Single mesh with per-vertex colors → one draw call. Not in
// boxBVH or inferenceGeo, so it never participates in picking or snapping.

const sceneAxesData = SceneAxes.buildSceneAxes();
const sceneAxesNode = scene.createMesh({
    positions: sceneAxesData.positions,
    normals:   sceneAxesData.normals,
    colors:    sceneAxesData.colors,
    indices:   sceneAxesData.indices,
    emissive:  0.85,    // self-lit so the grid stays bright at any orbit angle
    name: 'scene-axes',
});

// Rebuild all mesh-derived state after the box buffers have been mutated.
// Push/pull may flip winding and axis-parallel normals when the drag inverts
// the geometry, so all three of positions/indices/normals need to flow back
// into the Mesh object (BVH and face groups are re-derived from them).
function rebuildMeshState() {
    boxMesh.positions = boxPositions;
    boxMesh.indices   = boxIndices;
    if (boxNormals) boxMesh.normals = boxNormals;
    boxBVH       = new MeshBVH(boxMesh);
    faceGroups   = computeFaceGroups(boxPositions, boxIndices);
    editMesh     = EditMesh.fromMeshData(boxPositions, boxIndices);
    inferenceGeo = Inference.buildInferenceGeo(boxPositions, boxIndices, faceGroups);
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
    showSnapMarker(null);
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
    faceTriangles: null,     // face-group triangle indices (for drag-time highlight)
    axis: [0, 0, 0],
    pivot: [0, 0, 0],
    vertexIndices: null,     // Uint32Array (affected vertex indices)
    vertexStart: null,       // Float32Array, xyz per affected vertex (pre-drag snapshot)
    workingPositions: null,  // Float32Array, scratch — rebuilt each move
    workingIndices: null,    // Uint32Array — winding-flipped when inverted
    workingNormals: null,    // Float32Array — axis-component flipped when inverted
    distance: 0,
    // Inversion happens once the pushed face moves past the farthest non-affected
    // vertex along the push axis — i.e. the face has punched through to the
    // other side. `inversionT` is the drag distance at which that boundary
    // is crossed; `inverted` tracks whether we've applied the flip.
    inversionT: 0,
    inverted: false,
};

// Reflect a vertex normal across the plane perpendicular to `axis` in place.
// For axis-aligned normals this just flips the axis-parallel component; for
// off-axis normals (beveled meshes, smooth shading) it correctly preserves
// the perpendicular component and flips only the parallel one.
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

// Reverse winding of every triangle by swapping index 1 and 2. Involutive.
function flipAllWinding(indices) {
    const triCount = indices.length / 3;
    for (let t = 0; t < triCount; t++) {
        const b = indices[t * 3 + 1];
        indices[t * 3 + 1] = indices[t * 3 + 2];
        indices[t * 3 + 2] = b;
    }
}

// Drag distance at which the pushed face crosses the farthest non-affected
// vertex along the axis. Once t < inversionT, the face has punched through
// the opposite side of the mesh and rendering needs the flip treatment.
function computeInversionT(axis, pivot, affectedSet) {
    let m = Infinity;
    for (let vi = 0; vi < boxPositions.length / 3; vi++) {
        if (affectedSet.has(vi)) continue;
        const proj = boxPositions[vi * 3 + 0] * axis[0] +
                     boxPositions[vi * 3 + 1] * axis[1] +
                     boxPositions[vi * 3 + 2] * axis[2];
        if (proj < m) m = proj;
    }
    const p0 = pivot[0] * axis[0] + pivot[1] * axis[1] + pivot[2] * axis[2];
    return m - p0;
}

function beginPushPull(hit) {
    const gIdx = faceGroups.triToGroup[hit.triangleIndex];
    const g = faceGroups.groups[gIdx];
    const idxs = collectAffectedVertexIndices(gIdx);
    const snap = new Float32Array(idxs.length * 3);
    const affectedSet = new Set();
    for (let i = 0; i < idxs.length; i++) {
        const vi = idxs[i];
        affectedSet.add(vi);
        snap[i * 3 + 0] = boxPositions[vi * 3 + 0];
        snap[i * 3 + 1] = boxPositions[vi * 3 + 1];
        snap[i * 3 + 2] = boxPositions[vi * 3 + 2];
    }
    pushpull.active = true;
    pushpull.groupIdx = gIdx;
    pushpull.faceTriangles = g.tris.slice();
    pushpull.axis = g.normal.slice();
    pushpull.pivot = hit.position.slice();
    pushpull.vertexIndices = idxs;
    pushpull.vertexStart = snap;
    pushpull.workingPositions = new Float32Array(boxPositions.length);
    pushpull.workingIndices = new Uint32Array(boxIndices);
    pushpull.workingNormals = boxNormals ? new Float32Array(boxNormals) : null;
    pushpull.distance = 0;
    pushpull.inversionT = computeInversionT(pushpull.axis, pushpull.pivot, affectedSet);
    pushpull.inverted = false;
    // Seed the highlight on the face we're about to drag.
    setHighlightTriangles(
        pushpull.faceTriangles, pushpull.axis,
        boxPositions, pushpull.workingIndices);
}

// Apply the current drag distance to the scene node via updateMesh. Does not
// mutate boxPositions/boxIndices/boxNormals — only the working buffers —
// until commit bakes them in. Handles the inversion flip when `t` crosses
// the opposite-side threshold.
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

    // Toggle winding + axis-parallel normal sign on inversion boundary crossings.
    // Each flip is involutive, so togglable back and forth across the boundary
    // during a single drag without diverging from the source state.
    const newInverted = t < pushpull.inversionT;
    if (newInverted !== pushpull.inverted) {
        flipAllWinding(pushpull.workingIndices);
        if (pushpull.workingNormals) {
            flipNormalsAlongAxis(pushpull.workingNormals, pushpull.axis);
        }
        pushpull.inverted = newInverted;
    }

    boxNode.updateMesh({
        positions: work,
        indices:   pushpull.workingIndices,
        normals:   pushpull.workingNormals || boxNormals,
    });

    // Move the highlight with the face, flipping its outward direction when
    // the geometry has inverted so it stays on the visible side.
    const hlNormal = pushpull.inverted
        ? [-pushpull.axis[0], -pushpull.axis[1], -pushpull.axis[2]]
        : pushpull.axis;
    setHighlightTriangles(
        pushpull.faceTriangles, hlNormal, work, pushpull.workingIndices);

    pickInfo.textContent = `push/pull  ${t.toFixed(3)}` +
        (pushpull.inverted ? '  [inverted]' : '');
}

function commitPushPull() {
    if (!pushpull.active) return;
    // Bake working buffers into the canonical box buffers; rebuild downstream.
    boxPositions = new Float32Array(pushpull.workingPositions);
    boxIndices   = new Uint32Array(pushpull.workingIndices);
    if (pushpull.workingNormals) {
        boxNormals = new Float32Array(pushpull.workingNormals);
    }
    rebuildMeshState();
    pickInfo.textContent =
        `extruded ${pushpull.distance.toFixed(3)}` +
        (pushpull.inverted ? '  [inverted through]' : '');
    clearPushPull();
    clearHighlight();
}

function cancelPushPull() {
    if (!pushpull.active) return;
    // Push the pristine boxMesh state back to the scene. workingIndices and
    // workingNormals may have been flipped by crossing inversion — we don't
    // need to unflip them here because they're about to be thrown away.
    boxNode.updateMesh({
        positions: boxPositions,
        indices:   boxIndices,
        normals:   boxNormals,
    });
    pickInfo.textContent = 'push/pull cancelled';
    clearPushPull();
    clearHighlight();
}

function clearPushPull() {
    pushpull.active = false;
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
//
// Two visual layers per snap:
//   1. A 2D SVG glyph in screen space (always at the projected pixel).
//   2. A small persistent 3D scene node at the snap's world position, so the
//      user reads "you'll land HERE in 3D" — not just "your cursor is near
//      this thing on screen".
// Push/pull projects the snap onto the drag axis to lock depth. Hover only
// surfaces feature snaps (vertex/midpoint/edge); the on-face fallback is
// suppressed since it would keep the marker visible everywhere on the model
// and read as flicker rather than a meaningful snap.

const SNAP_SHAPES = {
    'endpoint': '<circle cx="9" cy="9" r="5" fill="#2ecc71" stroke="#fff" stroke-width="1.5"/>',
    'midpoint': '<polygon points="9,2 16,9 9,16 2,9" fill="none" stroke="#1abc9c" stroke-width="2"/>',
    'on-edge':  '<rect x="3" y="3" width="12" height="12" fill="none" stroke="#e74c3c" stroke-width="2"/>',
    'on-face':  '<polygon points="9,3 15,9 9,15 3,9" fill="#3498db" fill-opacity="0.4" stroke="#3498db" stroke-width="1.5"/>',
};

// 3D snap indicator: one sphere per snap type, pre-created with the type's
// color (color isn't a settable prop on a SceneNode — only set at createMesh
// time — so we keep one node per color and toggle visibility instead of
// recreating). All hidden until the first snap.
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
    // Reveal only the matching color sphere at the snap world position.
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

// Look up the best snap for a canvas-relative cursor pixel + ray.
//   - `includeFaceFallback`: include on-face snap when no feature is in tol.
//     False by default — only feature snaps are surfaced (avoids the marker
//     trailing the cursor across the whole model, which reads as jitter).
//   - `excludeTypes`: hide specific snap types from this lookup. The push/pull
//     drag passes ['on-edge'] because edge-projection-along-axis flickers
//     wildly when the cursor moves perpendicular to a long edge.
function resolveSnap(cx, cy, ray, includeFaceFallback, excludeTypes) {
    const camOpts = Camera.orbitViewOpts(cam, canvas);
    const w = canvas.clientWidth || canvas.width;
    const h = canvas.clientHeight || canvas.height;
    const onFaceHit = includeFaceFallback
        ? boxBVH.raycast(boxMesh, ray.origin, ray.dir, 0)
        : null;
    return Inference.findSnap({
        cursorX: cx, cursorY: cy, ray,
        camOpts, width: w, height: h,
        geo: inferenceGeo,
        onFaceHit,
        excludeTypes,
    });
}

// Project a snap point onto the push axis line through `pivot`. Returns the
// scalar drag distance so applyPushPull(t) lands the pushed face coplanar
// with the snapped feature along the axis.
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
    const r = canvas.getBoundingClientRect();
    const cx = e.clientX - r.left;
    const cy = e.clientY - r.top;
    const ray = screenToRay(cx, cy);
    if (pushpull.active) {
        // Inference takes priority over raw cursor projection when a vertex
        // or midpoint snaps. on-edge is excluded here: it's noisy under an
        // axis-constrained drag (the projected closest-point slides along the
        // edge as the cursor moves perpendicular to the axis, jerking depth).
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
    // Hover snap: only feature snaps surface. No on-face fallback — that
    // would keep the marker visible everywhere on the model and read as
    // jitter rather than a meaningful snap target.
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
    scene, cam, boxNode, sceneAxesNode, pickAt, screenToRay,
    get boxMesh()      { return boxMesh; },
    get boxBVH()       { return boxBVH; },
    get boxPositions() { return boxPositions; },
    get boxIndices()   { return boxIndices; },
    get boxNormals()   { return boxNormals; },
    get faceGroups()   { return faceGroups; },
    get editMesh()     { return editMesh; },
    get inferenceGeo() { return inferenceGeo; },
    get highlightNode(){ return highlightNode; },
    get currentTool()  { return currentTool; },
    get activeSnap()   { return activeSnap; },
    clearHighlight, setHighlightTriangle, setHighlightTriangles,
    computeFaceGroups, setHighlightFaceGroup,
    setTool,
    // Push/Pull programmatic hooks for headless testing.
    beginPushPull, applyPushPull, commitPushPull, cancelPushPull,
    collectAffectedVertexIndices, rayVsAxisDistance,
    get pushpull() { return pushpull; },
    // Inference hooks for headless testing.
    resolveSnap, snapAxisDistance, showSnapMarker,
};
