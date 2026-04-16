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

function setHighlightTriangle(triIndex, normal) {
    clearHighlight();
    const i0 = boxIndices[triIndex * 3 + 0];
    const i1 = boxIndices[triIndex * 3 + 1];
    const i2 = boxIndices[triIndex * 3 + 2];
    const nx = normal[0] * HIGHLIGHT_EPS;
    const ny = normal[1] * HIGHLIGHT_EPS;
    const nz = normal[2] * HIGHLIGHT_EPS;
    const positions = new Float32Array(9);
    for (let k = 0; k < 3; k++) {
        const src = [i0, i1, i2][k] * 3;
        positions[k * 3 + 0] = boxPositions[src + 0] + nx;
        positions[k * 3 + 1] = boxPositions[src + 1] + ny;
        positions[k * 3 + 2] = boxPositions[src + 2] + nz;
    }
    const normals = new Float32Array([
        normal[0], normal[1], normal[2],
        normal[0], normal[1], normal[2],
        normal[0], normal[1], normal[2],
    ]);
    const indices = new Uint32Array([0, 1, 2]);
    highlightNode = scene.createMesh({
        positions, normals, indices,
        color: '#ffa502',
        emissive: 0.6,
        name: 'highlight',
    });
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
        if (hit) setHighlightTriangle(hit.triangleIndex, hit.normal);
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
    clearHighlight, setHighlightTriangle,
};
