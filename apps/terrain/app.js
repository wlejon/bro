// =============================================================================
// Endless Terrain — chunked, streaming, deformable
// =============================================================================

var canvas = document.getElementById('c');
var scene = canvas.getContext('scene');
var info = document.getElementById('info');

// ---------------------------------------------------------------------------
// Constants (must match terrain-worker.js init message)
// ---------------------------------------------------------------------------

var SEED = 42;
var CHUNK_GRID = 32;
var CELL_SIZE = 1.0;
var CHUNK_WORLD_SIZE = CHUNK_GRID * CELL_SIZE;

var VIEW_DIST = 5;          // horizontal chunk radius around camera
var VIEW_DIST_Y = 3;        // vertical chunk radius
var UNLOAD_MARGIN = 2;      // extra distance before unloading
var WORKER_COUNT = 4;       // parallel worker threads
var MAX_INFLIGHT_PER_WORKER = 2;
var MAX_MESH_OPS_PER_FRAME = 2;

// ---------------------------------------------------------------------------
// 3D math helpers (camera only)
// ---------------------------------------------------------------------------

function v3add(a, b) { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function v3sub(a, b) { return [a[0]-b[0], a[1]-b[1], a[2]-b[2]]; }
function v3scale(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }
function v3len(a) { return Math.sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); }
function v3norm(a) { var l = v3len(a); return l > 0 ? v3scale(a, 1/l) : [0,0,0]; }

// ---------------------------------------------------------------------------
// Worker pool
// ---------------------------------------------------------------------------

var workers = [];
var workersReady = 0;

function initWorkers() {
    for (var i = 0; i < WORKER_COUNT; i++) {
        var w = new Worker('terrain-worker.js');
        var slot = { worker: w, busy: 0, ready: false };
        w.onmessage = (function(slotRef) {
            return function(e) { onWorkerMessage(slotRef, e.data); };
        })(slot);
        workers.push(slot);
        // Send init (worker will reply with 'ready' after building noise)
        w.postMessage({
            type: 'init',
            seed: SEED,
            chunkGrid: CHUNK_GRID,
            cellSize: CELL_SIZE
        });
    }
}

function pickWorker() {
    var best = null;
    for (var i = 0; i < workers.length; i++) {
        var s = workers[i];
        if (!s.ready) continue;
        if (s.busy >= MAX_INFLIGHT_PER_WORKER) continue;
        if (best === null || s.busy < best.busy) best = s;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Chunk registry
// ---------------------------------------------------------------------------

// state: 'pending' | 'loading' | 'loaded'
// node: SceneNode | null
// density: Float32Array | null
// edits: Float32Array (5 floats per edit) — accumulated deformations
var chunks = {};                    // id -> chunk record
var pendingQueue = [];              // ids waiting to dispatch
var meshOpQueue = [];               // worker results waiting to be applied to scene

function chunkId(cx, cy, cz) { return cx + ',' + cy + ',' + cz; }

function ensureChunk(cx, cy, cz) {
    var id = chunkId(cx, cy, cz);
    var c = chunks[id];
    if (!c) {
        c = {
            id: id, cx: cx, cy: cy, cz: cz,
            state: 'pending',
            node: null,
            density: null,
            edits: null,
            lod: 0
        };
        chunks[id] = c;
        pendingQueue.push(id);
    }
    return c;
}

function chunkDist(c, ccx, ccy, ccz) {
    var dx = c.cx - ccx, dy = c.cy - ccy, dz = c.cz - ccz;
    return Math.sqrt(dx*dx + dy*dy + dz*dz);
}

// ---------------------------------------------------------------------------
// Worker message handling
// ---------------------------------------------------------------------------

function onWorkerMessage(slot, msg) {
    if (msg.type === 'ready') {
        if (!slot.ready) {
            slot.ready = true;
            workersReady++;
        }
        return;
    }

    if (msg.type === 'chunk') {
        slot.busy = Math.max(0, slot.busy - 1);
        // Queue scene-side mesh op for the next frame budget
        meshOpQueue.push(msg);
        return;
    }
}

// ---------------------------------------------------------------------------
// Per-frame chunk management
// ---------------------------------------------------------------------------

function dispatchPending() {
    if (pendingQueue.length === 0) return;

    // Sort pending by distance to camera so closest chunks generate first
    var ccx = Math.floor(cam.pos[0] / CHUNK_WORLD_SIZE);
    var ccy = Math.floor(cam.pos[1] / CHUNK_WORLD_SIZE);
    var ccz = Math.floor(cam.pos[2] / CHUNK_WORLD_SIZE);

    pendingQueue.sort(function(a, b) {
        var ca = chunks[a], cb = chunks[b];
        if (!ca) return 1;
        if (!cb) return -1;
        return chunkDist(ca, ccx, ccy, ccz) - chunkDist(cb, ccx, ccy, ccz);
    });

    var stillPending = [];
    for (var i = 0; i < pendingQueue.length; i++) {
        var id = pendingQueue[i];
        var c = chunks[id];
        if (!c || c.state !== 'pending') continue;

        var slot = pickWorker();
        if (!slot) {
            // No worker available; remaining chunks stay pending
            for (var j = i; j < pendingQueue.length; j++) {
                stillPending.push(pendingQueue[j]);
            }
            break;
        }

        c.state = 'loading';
        slot.busy++;

        var edits = c.edits;
        var transfers = [];
        // Send a copy of edits — we keep the master list locally
        var editsCopy = edits ? new Float32Array(edits) : null;
        if (editsCopy) transfers.push(editsCopy.buffer);

        slot.worker.postMessage({
            type: 'generate',
            id: c.id,
            cx: c.cx, cy: c.cy, cz: c.cz,
            lod: c.lod,
            edits: editsCopy
        }, transfers);
    }
    pendingQueue = stillPending;
}

function applyMeshOps() {
    var applied = 0;
    while (meshOpQueue.length > 0 && applied < MAX_MESH_OPS_PER_FRAME) {
        var msg = meshOpQueue.shift();
        var c = chunks[msg.id];
        if (!c) continue;  // chunk was unloaded while in flight

        c.density = msg.density;

        if (msg.empty) {
            // No mesh to draw; if there was a node, destroy it
            if (c.node) {
                c.node.destroy();
                c.node = null;
            }
            c.state = 'loaded';
            applied++;
            continue;
        }

        if (c.node) {
            // Update existing node in place
            c.node.updateMesh({
                positions: msg.positions,
                normals: msg.normals,
                indices: msg.indices
            });
        } else {
            c.node = scene.createMesh({
                name: 'chunk_' + c.id,
                positions: msg.positions,
                normals: msg.normals,
                indices: msg.indices,
                x: c.cx * CHUNK_WORLD_SIZE,
                y: c.cy * CHUNK_WORLD_SIZE,
                z: c.cz * CHUNK_WORLD_SIZE,
                color: [0.45, 0.55, 0.35]
            });
        }
        c.state = 'loaded';
        applied++;
    }
}

function updateLoadedChunks() {
    var ccx = Math.floor(cam.pos[0] / CHUNK_WORLD_SIZE);
    var ccy = Math.floor(cam.pos[1] / CHUNK_WORLD_SIZE);
    var ccz = Math.floor(cam.pos[2] / CHUNK_WORLD_SIZE);

    // Request chunks within view distance
    for (var dz = -VIEW_DIST; dz <= VIEW_DIST; dz++) {
        for (var dy = -VIEW_DIST_Y; dy <= VIEW_DIST_Y; dy++) {
            for (var dx = -VIEW_DIST; dx <= VIEW_DIST; dx++) {
                // Sphere-ish culling
                if (dx*dx + dz*dz > VIEW_DIST * VIEW_DIST) continue;
                ensureChunk(ccx + dx, ccy + dy, ccz + dz);
            }
        }
    }

    // Unload chunks that drifted too far
    var unloadDist = VIEW_DIST + UNLOAD_MARGIN;
    var unloadDistSq = unloadDist * unloadDist;
    var toRemove = [];
    for (var id in chunks) {
        var c = chunks[id];
        var ddx = c.cx - ccx, ddy = c.cy - ccy, ddz = c.cz - ccz;
        if (ddx*ddx + ddz*ddz > unloadDistSq || Math.abs(ddy) > VIEW_DIST_Y + UNLOAD_MARGIN) {
            toRemove.push(id);
        }
    }
    for (var i = 0; i < toRemove.length; i++) {
        var c = chunks[toRemove[i]];
        if (c.node) c.node.destroy();
        delete chunks[toRemove[i]];
    }
}

// ---------------------------------------------------------------------------
// Camera (FPS-style fly cam)
// ---------------------------------------------------------------------------

var cam = {
    pos: [0, 60, 0],
    yaw: 0,
    pitch: -0.3,
    speed: 30,
    sensitivity: 0.003
};

function camForward() {
    return [
        Math.sin(cam.yaw) * Math.cos(cam.pitch),
        Math.sin(cam.pitch),
        -Math.cos(cam.yaw) * Math.cos(cam.pitch)
    ];
}
function camRight() {
    return [Math.cos(cam.yaw), 0, Math.sin(cam.yaw)];
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

var keys = {};
var mouseCaptured = false;
var mouseDown = { left: false, right: false };

document.addEventListener('keydown', function(e) {
    keys[e.key.toLowerCase()] = true;
});
document.addEventListener('keyup', function(e) {
    keys[e.key.toLowerCase()] = false;
    if (e.key === 'Escape') mouseCaptured = false;
});
canvas.addEventListener('click', function() { mouseCaptured = true; });
canvas.addEventListener('mousedown', function(e) {
    if (e.button === 0) mouseDown.left = true;
    if (e.button === 2) mouseDown.right = true;
});
canvas.addEventListener('mouseup', function(e) {
    if (e.button === 0) mouseDown.left = false;
    if (e.button === 2) mouseDown.right = false;
});
canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });
document.addEventListener('mousemove', function(e) {
    if (!mouseCaptured) return;
    cam.yaw += e.movementX * cam.sensitivity;
    cam.pitch -= e.movementY * cam.sensitivity;
    cam.pitch = Math.max(-1.4, Math.min(1.4, cam.pitch));
});

// ---------------------------------------------------------------------------
// Deformation — apply an edit at a world-space point
// ---------------------------------------------------------------------------

function deformAt(wx, wy, wz, radius, strength) {
    // Find all chunks intersecting the edit sphere
    var minCx = Math.floor((wx - radius) / CHUNK_WORLD_SIZE);
    var maxCx = Math.floor((wx + radius) / CHUNK_WORLD_SIZE);
    var minCy = Math.floor((wy - radius) / CHUNK_WORLD_SIZE);
    var maxCy = Math.floor((wy + radius) / CHUNK_WORLD_SIZE);
    var minCz = Math.floor((wz - radius) / CHUNK_WORLD_SIZE);
    var maxCz = Math.floor((wz + radius) / CHUNK_WORLD_SIZE);

    for (var cx = minCx; cx <= maxCx; cx++) {
        for (var cy = minCy; cy <= maxCy; cy++) {
            for (var cz = minCz; cz <= maxCz; cz++) {
                var id = chunkId(cx, cy, cz);
                var c = chunks[id];
                if (!c) continue;  // chunk not loaded; skip

                // Append edit (5 floats: x, y, z, radius, strength)
                var oldLen = c.edits ? c.edits.length : 0;
                var newEdits = new Float32Array(oldLen + 5);
                if (c.edits) newEdits.set(c.edits, 0);
                newEdits[oldLen + 0] = wx;
                newEdits[oldLen + 1] = wy;
                newEdits[oldLen + 2] = wz;
                newEdits[oldLen + 3] = radius;
                newEdits[oldLen + 4] = strength;
                c.edits = newEdits;

                // Re-queue for generation if not already
                if (c.state === 'loaded') {
                    c.state = 'pending';
                    pendingQueue.push(id);
                }
            }
        }
    }
}

// Simple ray-march against density field of loaded chunks for deformation targeting
function deformRayTarget() {
    // March a ray from camera forward; return first solid hit position
    var origin = cam.pos;
    var dir = camForward();
    var maxDist = 60;
    var stepSize = 0.5;

    for (var t = 0.5; t < maxDist; t += stepSize) {
        var px = origin[0] + dir[0] * t;
        var py = origin[1] + dir[1] * t;
        var pz = origin[2] + dir[2] * t;

        var cx = Math.floor(px / CHUNK_WORLD_SIZE);
        var cy = Math.floor(py / CHUNK_WORLD_SIZE);
        var cz = Math.floor(pz / CHUNK_WORLD_SIZE);
        var c = chunks[chunkId(cx, cy, cz)];
        if (!c || !c.density) continue;

        var G = CHUNK_GRID + 1;
        var step = CELL_SIZE * (1 << c.lod);
        // Sample nearest voxel
        var lx = (px - cx * CHUNK_WORLD_SIZE) / step;
        var ly = (py - cy * CHUNK_WORLD_SIZE) / step;
        var lz = (pz - cz * CHUNK_WORLD_SIZE) / step;
        var ix = Math.max(0, Math.min(G - 1, Math.floor(lx)));
        var iy = Math.max(0, Math.min(G - 1, Math.floor(ly)));
        var iz = Math.max(0, Math.min(G - 1, Math.floor(lz)));
        var d = c.density[(iz * G + iy) * G + ix];
        if (d < 0) {
            // Inside solid — return this point
            return [px, py, pz];
        }
    }
    return null;
}

// ---------------------------------------------------------------------------
// Render loop
// ---------------------------------------------------------------------------

var lastTime = Date.now();
var deformCooldown = 0;

function render() {
    var now = Date.now();
    var dt = Math.min((now - lastTime) / 1000, 0.05);
    lastTime = now;

    // --- Camera input ---
    var moveSpeed = cam.speed * dt;
    if (keys['shift']) moveSpeed *= 3;
    var fwd = camForward();
    var right = camRight();
    if (keys['w']) cam.pos = v3add(cam.pos, v3scale(fwd, moveSpeed));
    if (keys['s']) cam.pos = v3add(cam.pos, v3scale(fwd, -moveSpeed));
    if (keys['a']) cam.pos = v3add(cam.pos, v3scale(right, -moveSpeed));
    if (keys['d']) cam.pos = v3add(cam.pos, v3scale(right, moveSpeed));
    if (keys[' ']) cam.pos[1] += moveSpeed;
    if (keys['control']) cam.pos[1] -= moveSpeed;

    // --- Camera matrix ---
    var W = canvas.clientWidth, H = canvas.clientHeight;
    scene.setCamera({
        fov: 70,
        aspect: W / H,
        near: 0.5,
        far: 600,
        position: cam.pos,
        target: v3add(cam.pos, fwd)
    });

    // --- Deformation ---
    deformCooldown -= dt;
    if (deformCooldown <= 0 && (mouseDown.left || mouseDown.right)) {
        var hit = deformRayTarget();
        if (hit) {
            var strength = mouseDown.left ? 6.0 : -6.0;  // dig or place
            deformAt(hit[0], hit[1], hit[2], 4.0, strength);
            deformCooldown = 0.08;  // ~12 deforms per second
        }
    }

    // --- Chunk lifecycle ---
    if (workersReady > 0) {
        updateLoadedChunks();
        dispatchPending();
        applyMeshOps();
    }

    // --- HUD ---
    var loaded = 0, pending = 0, loading = 0;
    for (var id in chunks) {
        var s = chunks[id].state;
        if (s === 'loaded') loaded++;
        else if (s === 'pending') pending++;
        else if (s === 'loading') loading++;
    }
    info.textContent =
        'Pos: ' + cam.pos[0].toFixed(0) + ',' + cam.pos[1].toFixed(0) + ',' + cam.pos[2].toFixed(0) +
        '\nChunks: ' + loaded + ' loaded, ' + loading + ' loading, ' + pending + ' pending' +
        '\nWorkers: ' + workersReady + '/' + WORKER_COUNT;

    requestAnimationFrame(render);
}

initWorkers();
requestAnimationFrame(render);
