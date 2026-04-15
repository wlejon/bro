// =============================================================================
// mesh-forge - MeshyAI → game-ready rigged character pipeline
//
// Staged app: Load → Cleanup → UV → Export-untextured → Reimport-textured
//             → Rig → Animate → Export-final.
//
// Each stage is defined in stages/<name>.js and registered on window.Stages.
// Shared pipeline state lives in `ps`. Each stage owns its params panel and
// the scene nodes it creates; downstream stages are disabled until their
// prerequisites are met.
// =============================================================================

const canvas  = document.getElementById('view');
const scene   = canvas.getContext('scene');
const infoEl  = document.getElementById('info');
const statusEl = document.getElementById('status-bar');
const stagesEl = document.getElementById('stages');
const paramsHeader = document.getElementById('params-header');
const paramsBody   = document.getElementById('params-body');

// ---------------------------------------------------------------------------
// Pipeline state
// ---------------------------------------------------------------------------

const ps = {
    sourceMesh: null,      // raw Mesh from loadGLTF
    sourcePath: null,      // original file path
    sourceGltf: null,      // { meshes, skins, ... } - full loadGLTF return

    cleanMesh: null,       // post-cleanup Mesh
    cleanBase: null,       // snapshot of cleanMesh positions before skinning

    uvResult: null,        // { uvs, charts }

    untexturedExportPath: null,
    texturedMesh: null,    // post-texture reimport

    spec: null,
    landmarks: null,
    missing: [],
    skeleton: null,
    skin: null,

    animations: [],
    activeAnim: null,
    animTime: 0,
    animPlaying: false,
    ikEnabled: false,

    sceneNodes: {
        base: null,       // primary mesh node
        stage: {},        // per-stage scene-node registry keyed by stage id
    },
};

window.ps = ps; // expose for stage modules
window.scene = scene;

// ---------------------------------------------------------------------------
// Camera - orbit, auto-fit on mesh load
// ---------------------------------------------------------------------------

const cam = {
    target: [0, 1, 0],
    dist: 4,
    height: 1.5,
    angle: 0,
    auto: false,
};

function fitCameraToMesh(mesh) {
    const bb = mesh.computeBBox();
    const cx = (bb.min[0] + bb.max[0]) * 0.5;
    const cy = (bb.min[1] + bb.max[1]) * 0.5;
    const cz = (bb.min[2] + bb.max[2]) * 0.5;
    const size = Math.max(bb.max[0] - bb.min[0],
                          bb.max[1] - bb.min[1],
                          bb.max[2] - bb.min[2]);
    cam.target = [cx, cy, cz];
    cam.dist = Math.max(size * 2.2, 2);
    cam.height = cy + size * 0.3;
    cam.angle = 0;
}

function updateCamera() {
    if (cam.auto) cam.angle += 0.004;
    scene.setCamera({
        fov: 45,
        position: [
            cam.target[0] + Math.sin(cam.angle) * cam.dist,
            cam.height,
            cam.target[2] + Math.cos(cam.angle) * cam.dist,
        ],
        target: cam.target,
        aspect: canvas.clientWidth / canvas.clientHeight,
    });
}

window.fitCameraToMesh = fitCameraToMesh;

// ---------------------------------------------------------------------------
// Stage router
// ---------------------------------------------------------------------------

const Stages = window.Stages = window.Stages || [];
// Each stage module should push: { id, label, canEnter(ps), mount(ps, ctx), unmount(ps, ctx) }

let currentStage = null;

const ctx = {
    scene, canvas,
    ps,
    setStatus(msg, isError) {
        statusEl.textContent = msg || '';
        statusEl.classList.toggle('error', !!isError);
        if (isError) console.error(msg);
    },
    setInfo(msg) { infoEl.textContent = msg || ''; },
    paramsBody,
    paramsHeader,
    invalidateDownstream(fromId) { invalidateDownstream(fromId); },
    rebuildStageList() { rebuildStageList(); },
    selectStage(id) { selectStage(id); },
};
window.forgeCtx = ctx;

function invalidateDownstream(fromId) {
    const idx = Stages.findIndex(s => s.id === fromId);
    if (idx < 0) return;
    for (let i = idx + 1; i < Stages.length; i++) {
        const s = Stages[i];
        // Ask stage to clean up if it has tracked scene nodes
        const bag = ps.sceneNodes.stage[s.id];
        if (bag) {
            for (const n of bag) { try { n.destroy(); } catch(_) {} }
            ps.sceneNodes.stage[s.id] = [];
        }
    }
    // Reset downstream pipeline state slots, conservatively. Each stage
    // should re-derive from its input when re-entered.
    if (idx < Stages.findIndex(s => s.id === 'cleanup')) ps.cleanMesh = null;
    if (idx < Stages.findIndex(s => s.id === 'uv')) ps.uvResult = null;
    if (idx < Stages.findIndex(s => s.id === 'rig')) {
        ps.spec = ps.landmarks = ps.skeleton = ps.skin = null; ps.missing = [];
    }
    if (idx < Stages.findIndex(s => s.id === 'animate')) {
        ps.activeAnim = null; ps.animations = []; ps.animPlaying = false;
    }
}

function rebuildStageList() {
    stagesEl.innerHTML = '';
    for (const s of Stages) {
        const li = document.createElement('li');
        li.textContent = s.label;
        li.dataset.stageId = s.id;
        const enabled = !s.canEnter || s.canEnter(ps);
        if (!enabled) li.classList.add('disabled');
        if (currentStage && currentStage.id === s.id) li.classList.add('active');
        li.addEventListener('click', () => {
            if (!enabled) return;
            selectStage(s.id);
        });
        stagesEl.appendChild(li);
    }
}

function selectStage(id) {
    const next = Stages.find(s => s.id === id);
    if (!next) return;
    if (next.canEnter && !next.canEnter(ps)) return;
    if (currentStage) {
        try { currentStage.unmount && currentStage.unmount(ps, ctx); } catch(e) { console.error(e); }
    }
    paramsBody.innerHTML = '';
    paramsHeader.textContent = next.label;
    currentStage = next;
    ps.sceneNodes.stage[id] = ps.sceneNodes.stage[id] || [];
    try { next.mount && next.mount(ps, ctx); } catch(e) {
        console.error(e);
        ctx.setStatus('mount failed: ' + e.message, true);
    }
    rebuildStageList();
}

// Helper for stages to register scene nodes for automatic cleanup
ctx.trackNode = function(node, stageId) {
    const sid = stageId || (currentStage && currentStage.id);
    if (!sid) return node;
    ps.sceneNodes.stage[sid] = ps.sceneNodes.stage[sid] || [];
    ps.sceneNodes.stage[sid].push(node);
    return node;
};

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

document.addEventListener('keydown', (e) => {
    const tag = e.target && e.target.tagName;
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
});

canvas.addEventListener('wheel', (e) => {
    cam.dist = Math.max(0.5, cam.dist * (1 + e.deltaY * 0.001));
    e.preventDefault();
}, { passive: false });

let dragging = false, lastX = 0, lastY = 0;
canvas.addEventListener('mousedown', (e) => { dragging = true; lastX = e.clientX; lastY = e.clientY; cam.auto = false; });
window.addEventListener('mouseup', () => { dragging = false; });
window.addEventListener('mousemove', (e) => {
    if (!dragging) return;
    cam.angle -= (e.clientX - lastX) * 0.01;
    cam.height += (e.clientY - lastY) * 0.02;
    lastX = e.clientX; lastY = e.clientY;
});

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

let lastT = 0;
function frame(t) {
    if (!lastT) lastT = t;
    const dt = t - lastT;
    lastT = t;

    if (currentStage && typeof currentStage.tick === 'function') {
        try { currentStage.tick(ps, ctx, dt); } catch(e) {
            ctx.setStatus('tick: ' + e.message, true);
        }
    }
    updateCamera();
    requestAnimationFrame(frame);
}

// ---------------------------------------------------------------------------
// Bootstrap - populate stage list, land on Load
// ---------------------------------------------------------------------------

rebuildStageList();
selectStage('load');
requestAnimationFrame(frame);
ctx.setStatus('ready - choose a file via Load stage');
