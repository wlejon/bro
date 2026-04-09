// =============================================================================
// Endless Terrain — multi-LOD clipmap world
// =============================================================================
//
// The world is conceptually one infinite density field. We sample it as
// concentric LOD shells centered on the camera:
//
//   LOD 0: small chunks at full detail, near the camera
//   LOD 1: larger chunks (2x cell size) at the next ring out
//   LOD 2: 4x cell size, even further out
//   ...
//
// Each LOD shell has its own chunk grid (LOD L chunks span 32 * 2^L world
// units). Every shell samples the SAME noise field, so the hills/mountains
// line up across LOD boundaries — moving forward gradually replaces a coarse
// silhouette of a far mountain with the high-detail version, no discrete
// "swap one chunk for a better chunk" snap.
//
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

// LOD shell layout. Each LOD level uses chunks that are 2x the world size of
// the previous level, so the shell at LOD L extends roughly 2x further than
// LOD L-1 with the same number of chunks.
var LOD_LEVELS = 9;          // 0..4 → 32m, 64m, 128m, 256m, 512m chunks
var LOD_RING_RADIUS = 4;     // chunks per side from camera at each LOD (~2km out at LOD 4)
var LOD_RING_RADIUS_Y = 1;   // vertical chunks per side at each LOD (3 layers)
var WORKER_COUNT = 2;
var MAX_INFLIGHT_PER_WORKER = 8;
var MAX_MESH_OPS_PER_FRAME = 32;

function lodCellSize(lod) { return CELL_SIZE * (1 << lod); }
function lodChunkSize(lod) { return CHUNK_GRID * lodCellSize(lod); }

// Per-LOD tint. Slightly different shades of green so the LOD shells are
// visible at a glance — set everything to the same color when you want a
// uniform look.
var LOD_COLORS = [
    [0.42, 0.58, 0.32],   // LOD 0 — fresh green (closest)
    [0.40, 0.55, 0.34],   // LOD 1
    [0.38, 0.52, 0.36],   // LOD 2
    [0.36, 0.49, 0.38],   // LOD 3
    [0.34, 0.46, 0.40]    // LOD 4 — gray-green (farthest)
];

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
// lod: LOD level (chunks at different LODs live in separate coord grids)
// cx,cy,cz: chunk coords *in this LOD's grid*
// node: SceneNode | null
var chunks = {};                    // id -> chunk record
var pendingQueue = [];              // ids waiting to dispatch
var meshOpQueue = [];               // worker results waiting to be applied to scene

// Player edits live in world space as a flat list (5 floats per edit:
// x, y, z, radius, strength). When meshing a chunk we filter to only the
// edits whose sphere overlaps the chunk bbox. This works across all LODs
// because each LOD chunk is just a different rasterization of the same
// world-space density field.
var allEdits = [];

function chunkId(lod, cx, cy, cz) {
    return lod + ':' + cx + ',' + cy + ',' + cz;
}

function ensureChunk(lod, cx, cy, cz) {
    var id = chunkId(lod, cx, cy, cz);
    var c = chunks[id];
    if (!c) {
        c = {
            id: id, lod: lod, cx: cx, cy: cy, cz: cz,
            state: 'pending',
            node: null
        };
        chunks[id] = c;
        pendingQueue.push(id);
    }
    return c;
}

// World-space distance from camera to a chunk's center, in horizontal plane
// (used for the LOD shell distance test and pending-queue ordering).
function chunkDistXZ(c) {
    var sz = lodChunkSize(c.lod);
    var wx = (c.cx + 0.5) * sz - cam.pos[0];
    var wz = (c.cz + 0.5) * sz - cam.pos[2];
    return Math.sqrt(wx*wx + wz*wz);
}

// Collect edits whose sphere intersects a chunk's world bbox.
function editsForChunk(lod, cx, cy, cz) {
    var sz = lodChunkSize(lod);
    var minX = cx * sz, maxX = minX + sz;
    var minY = cy * sz, maxY = minY + sz;
    var minZ = cz * sz, maxZ = minZ + sz;
    var out = null;
    var n = allEdits.length;
    for (var i = 0; i < n; i += 5) {
        var ex = allEdits[i],     ey = allEdits[i + 1];
        var ez = allEdits[i + 2], er = allEdits[i + 3];
        if (ex + er < minX || ex - er > maxX) continue;
        if (ey + er < minY || ey - er > maxY) continue;
        if (ez + er < minZ || ez - er > maxZ) continue;
        if (!out) out = [];
        out.push(ex, ey, ez, er, allEdits[i + 4]);
    }
    return out ? new Float32Array(out) : null;
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

    // Order: lower LOD (= finer detail, closer to camera) first, then by
    // world-space distance. This makes the high-detail near rings appear
    // before the slow background fills in.
    pendingQueue.sort(function(a, b) {
        var ca = chunks[a], cb = chunks[b];
        if (!ca) return 1;
        if (!cb) return -1;
        if (ca.lod !== cb.lod) return ca.lod - cb.lod;
        return chunkDistXZ(ca) - chunkDistXZ(cb);
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

        var edits = editsForChunk(c.lod, c.cx, c.cy, c.cz);
        var transfers = [];
        if (edits) transfers.push(edits.buffer);

        slot.worker.postMessage({
            type: 'generate',
            id: c.id,
            cx: c.cx, cy: c.cy, cz: c.cz,
            lod: c.lod,
            edits: edits
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

        if (c.dirty) {
            // Edit landed while this chunk was generating — discard the stale
            // result and re-queue. The next dispatch will pick up the new edits.
            c.dirty = false;
            c.state = 'pending';
            pendingQueue.push(c.id);
            continue;
        }

        if (msg.empty) {
            if (c.node) {
                c.node.destroy();
                c.node = null;
            }
            c.state = 'loaded';
            applied++;
            continue;
        }

        var sz = lodChunkSize(c.lod);
        if (c.node) {
            // msg.mesh is a transferred Mesh handle — the underlying MeshData
            // lives in C++ and was moved across threads by pointer.
            c.node.updateMesh(msg.mesh);
        } else {
            // Polygon offset bias by LOD: finer LOD (lower index) gets a more
            // negative units value so it consistently wins the depth test
            // against any coarser LOD chunks underneath it. The factor term
            // scales with depth slope, so distant near-edge-on geometry also
            // resolves cleanly.
            var biasUnits = -2.0 * (LOD_LEVELS - c.lod);
            var biasFactor = -1.0 * (LOD_LEVELS - c.lod);
            // Subtle tint per LOD: each level gets a slightly different shade
            // so the layering is visible without screaming "debug colors".
            // Disable by setting all the same.
            var col = LOD_COLORS[c.lod] || [0.45, 0.55, 0.35];
            c.node = scene.createMesh({
                name: 'chunk_' + c.id,
                mesh: msg.mesh,
                x: c.cx * sz,
                y: c.cy * sz,
                z: c.cz * sz,
                color: col,
                depthBias: [biasFactor, biasUnits]
            });
        }
        c.state = 'loaded';
        applied++;
    }
}

function updateLoadedChunks() {
    // Each LOD level fully covers a disc out to LOD_RING_RADIUS chunks. The
    // discs OVERLAP — LOD 1 also covers everything LOD 0 covers, LOD 2 covers
    // everything LOD 0 and 1 cover, and so on. There are no rings or holes.
    //
    // Stacking is what eliminates the LOD-snap pop: when the camera moves, a
    // newly-revealed area is already populated by the coarse outer shells, so
    // the higher-detail shell quietly fills in on top instead of replacing
    // anything visually. Per-mesh polygon offset (set in applyMeshOps) makes
    // the high-LOD chunks always win the depth test against the coarse ones
    // they sit on.
    var wantedIds = {};

    for (var lod = 0; lod < LOD_LEVELS; lod++) {
        var sz = lodChunkSize(lod);
        var ccx = Math.floor(cam.pos[0] / sz);
        var ccy = Math.floor(cam.pos[1] / sz);
        var ccz = Math.floor(cam.pos[2] / sz);

        var outerDist = LOD_RING_RADIUS * sz;
        var rChunks = LOD_RING_RADIUS + 1;
        var rChunksY = LOD_RING_RADIUS_Y;

        for (var dz = -rChunks; dz <= rChunks; dz++) {
            for (var dy = -rChunksY; dy <= rChunksY; dy++) {
                for (var dx = -rChunks; dx <= rChunks; dx++) {
                    var cx = ccx + dx, cy = ccy + dy, cz = ccz + dz;

                    var wcx = (cx + 0.5) * sz - cam.pos[0];
                    var wcz = (cz + 0.5) * sz - cam.pos[2];
                    var dist = Math.sqrt(wcx*wcx + wcz*wcz);
                    if (dist > outerDist) continue;

                    var id = chunkId(lod, cx, cy, cz);
                    wantedIds[id] = true;
                    ensureChunk(lod, cx, cy, cz);
                }
            }
        }
    }

    // Unload anything that fell outside every shell.
    var toRemove = [];
    for (var id in chunks) {
        if (!wantedIds[id]) toRemove.push(id);
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
    pos: [0, 80, 0],
    yaw: 0,
    pitch: -0.2,
    speed: 60,
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
    // Append the edit to the world-space master list. Future chunk generations
    // (at any LOD) will pick it up via editsForChunk.
    allEdits.push(wx, wy, wz, radius, strength);

    // Re-mesh every currently-loaded chunk (across all LODs) whose bbox the
    // edit sphere touches. We don't filter by LOD: the user expects close-up
    // detail to update, and the coarse rings will quietly catch up.
    for (var id in chunks) {
        var c = chunks[id];
        var sz = lodChunkSize(c.lod);
        var minX = c.cx * sz, maxX = minX + sz;
        var minY = c.cy * sz, maxY = minY + sz;
        var minZ = c.cz * sz, maxZ = minZ + sz;

        if (wx + radius < minX || wx - radius > maxX) continue;
        if (wy + radius < minY || wy - radius > maxY) continue;
        if (wz + radius < minZ || wz - radius > maxZ) continue;

        if (c.state === 'loaded') {
            c.state = 'pending';
            pendingQueue.push(id);
        } else if (c.state === 'loading') {
            // applyMeshOps will discard the stale result and re-queue.
            c.dirty = true;
        }
    }
}

// Pick a point to deform: cast a ray against the scene's mesh nodes. The
// scene graph owns the real triangles in C++, so we delegate to a native
// raycast rather than keeping a parallel density grid on the JS side. This
// is what retired the per-chunk density field from the worker message and
// the main-thread chunk record.
function deformRayTarget() {
    var hit = scene.raycast(cam.pos, camForward(), 80);
    return hit ? hit.point : null;
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
    // Far plane has to comfortably reach the outermost LOD shell so distant
    // mountains aren't clipped. LOD_RING_RADIUS * lodChunkSize(LOD_LEVELS - 1)
    // is the world distance to the last shell's outer edge.
    var W = canvas.clientWidth, H = canvas.clientHeight;
    var maxViewDist = LOD_RING_RADIUS * lodChunkSize(LOD_LEVELS - 1);
    scene.setCamera({
        fov: 70,
        aspect: W / H,
        near: 0.5,
        far: maxViewDist * 1.5,
        position: cam.pos,
        target: v3add(cam.pos, fwd)
    });

    // --- Deformation ---
    deformCooldown -= dt;
    if (deformCooldown <= 0 && (mouseDown.left || mouseDown.right)) {
        var hit = deformRayTarget();
        if (hit) {
            // Left = dig (push toward air, density positive)
            // Right = place (push toward solid, density negative)
            var strength = mouseDown.left ? 18.0 : -18.0;
            deformAt(hit[0], hit[1], hit[2], 6.0, strength);
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
    var perLod = new Array(LOD_LEVELS);
    for (var L = 0; L < LOD_LEVELS; L++) perLod[L] = 0;
    for (var id in chunks) {
        var ch = chunks[id];
        var s = ch.state;
        if (s === 'loaded') { loaded++; perLod[ch.lod]++; }
        else if (s === 'pending') pending++;
        else if (s === 'loading') loading++;
    }
    info.textContent =
        'Pos: ' + cam.pos[0].toFixed(0) + ',' + cam.pos[1].toFixed(0) + ',' + cam.pos[2].toFixed(0) +
        '\nChunks: ' + loaded + ' loaded, ' + loading + ' loading, ' + pending + ' pending' +
        '\nLOD: ' + perLod.join('/') +
        '\nView: ' + Math.round(maxViewDist) + 'm  Workers: ' + workersReady + '/' + WORKER_COUNT;

    requestAnimationFrame(render);
}

initWorkers();
requestAnimationFrame(render);
