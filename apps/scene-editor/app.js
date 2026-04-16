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

// --- Geometry: one box, built once, BVH built against it --------------------

const boxMesh = Mesh.box(1, 1, 1);
const boxBVH  = new MeshBVH(boxMesh);

// Cache flat attribute buffers once — the Mesh getters copy on each call.
const boxPositions = boxMesh.positions;
const boxIndices   = boxMesh.indices;

// The scene node sits at identity so world == mesh-local. Picking ignores
// transforms until we need them — one less thing to get wrong in the spike.
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

const faceGroups = computeFaceGroups(boxPositions, boxIndices);

// Live edit-mesh backing the box. Not used for rendering yet — the scene node
// still renders boxMesh directly — but future editing tools will mutate this
// and then push a fresh MeshData back into the scene.
const editMesh = EditMesh.fromMeshData(boxPositions, boxIndices);

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

// --- Input: right=rotate, middle=pan, wheel=zoom, left=pick ----------------

let rightDown  = false;
let middleDown = false;

function updatePointerLock() {
    const want = rightDown || middleDown;
    const locked = document.pointerLockElement === canvas;
    if (want && !locked) canvas.requestPointerLock();
    else if (!want && locked) document.exitPointerLock();
}

canvas.addEventListener('mousedown', (e) => {
    if (e.button === 0) {
        const r = canvas.getBoundingClientRect();
        const hit = pickAt(e.clientX - r.left, e.clientY - r.top);
        pickInfo.textContent = formatHit(hit);
        window.__lastPick = hit;
        if (hit) setHighlightFaceGroup(faceGroups.triToGroup[hit.triangleIndex]);
        else clearHighlight();
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
    if (e.button === 2) rightDown  = false;
    if (e.button === 1) middleDown = false;
    updatePointerLock();
});
document.addEventListener('mousemove', (e) => {
    if (rightDown)  { Camera.orbitLook(cam, e.movementX, e.movementY); applyCamera(); }
    if (middleDown) { Camera.orbitPan (cam, e.movementX, e.movementY); applyCamera(); }
});
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
canvas.addEventListener('auxclick',    (e) => { if (e.button === 1) e.preventDefault(); });
canvas.addEventListener('wheel', (e) => {
    const factor = Math.exp(e.deltaY * 0.001);
    cam.dist = Math.max(0.1, cam.dist * factor);
    applyCamera();
    e.preventDefault();
});

// --- Test hook --------------------------------------------------------------

window.__editor = {
    scene, cam, boxMesh, boxBVH, boxNode, pickAt, screenToRay,
    get highlightNode() { return highlightNode; },
    clearHighlight, setHighlightTriangle, setHighlightTriangles,
    computeFaceGroups, faceGroups, setHighlightFaceGroup,
    editMesh,
};
