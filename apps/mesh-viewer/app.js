// =============================================================================
// GLB / GLTF viewer — browse a directory, load rigged/animated meshes, and
// render with CPU skinning via the rigging bindings.
// =============================================================================

const fs   = require('fs');
const path = require('path');

const STORAGE_KEY    = 'mesh-viewer:dir';
const EXTS           = ['.glb', '.gltf'];

const PALETTE = ['#e74c3c', '#3498db', '#2ecc71', '#f39c12', '#9b59b6',
                 '#16a085', '#d35400', '#c0392b', '#8e44ad'];

const canvas    = document.getElementById('canvas');
const scene     = canvas.getContext('scene');
const statusEl  = document.getElementById('status');
const infoEl    = document.getElementById('info');
const openFolderBtn = document.getElementById('open-folder-btn');
const openFileBtn   = document.getElementById('open-file-btn');
const dirStatus = document.getElementById('dir-status');
const fileListEl = document.getElementById('file-list');

let state = {
    mode: 'folder',     // 'folder' (scanned list) or 'file' (single-file pin, no list)
    dir: '',            // currently-scanned directory (folder mode only)
    files: [],          // absolute paths of .glb/.gltf found in dir (folder mode) or just [loadedPath] (file mode)
    fileIndex: -1,
    loaded: null,       // loaded gltf state (see loadFile)
    nodes: [],          // scene nodes
    paused: false,
    bindPoseOnly: false,
    time: 0,
    enableSkinning: true,
    showBones: false,
    boneNodes: [],
};

// Orbit camera — right-drag rotates, wheel zooms. Target + dist get reset
// on each file load based on the mesh's bbox.
const cam = Camera.createOrbit({ target: [0, 0, 0], dist: 6, fov: 45 });
let rightDown  = false;
let middleDown = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function setStatus(s) { statusEl.textContent = s; }
function setInfo(s) { infoEl.textContent = s; }

function clearNodes() {
    for (const n of state.nodes) n.destroy();
    state.nodes = [];
    clearBoneNodes();
}

function clearBoneNodes() {
    for (const n of state.boneNodes) n.destroy();
    state.boneNodes = [];
}

function fileName(p) {
    return p.replace(/\\/g, '/').split('/').pop();
}

// ---------------------------------------------------------------------------
// Directory scanning + file list UI
// ---------------------------------------------------------------------------

// Returns the last-used directory (from localStorage) if it still exists,
// or null if the user has never picked a file. No hardcoded defaults — an
// empty viewer on first launch invites the user to use "Open File…".
function pickInitialDir() {
    try {
        const saved = localStorage.getItem(STORAGE_KEY);
        if (saved && fs.existsSync(saved)) return saved;
    } catch (e) {}
    return null;
}

function scanDir(dir) {
    let entries;
    try {
        entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch (e) {
        return { ok: false, error: e.message, files: [] };
    }
    const files = [];
    for (const e of entries) {
        if (!e.isFile || !e.isFile()) continue;
        const lower = e.name.toLowerCase();
        if (EXTS.some(ext => lower.endsWith(ext))) {
            files.push(path.join(dir, e.name));
        }
    }
    files.sort((a, b) => fileName(a).localeCompare(fileName(b)));
    return { ok: true, files };
}

function renderFileList() {
    fileListEl.innerHTML = '';
    if (state.files.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'empty';
        empty.textContent = 'No .glb / .gltf files in this directory.';
        fileListEl.appendChild(empty);
        return;
    }
    for (let i = 0; i < state.files.length; i++) {
        const item = document.createElement('div');
        item.className = 'file-item' + (i === state.fileIndex ? ' selected' : '');
        const num = document.createElement('span');
        num.className = 'dim';
        num.textContent = (i + 1).toString().padStart(2, ' ');
        const name = document.createElement('span');
        name.textContent = ' ' + fileName(state.files[i]);
        item.appendChild(num);
        item.appendChild(name);
        item.addEventListener('click', () => {
            state.fileIndex = i;
            loadFile(i);
            renderFileList();
        });
        fileListEl.appendChild(item);
    }
    // Scroll the selected item into view.
    const sel = fileListEl.querySelector('.file-item.selected');
    if (sel && sel.scrollIntoView) sel.scrollIntoView({ block: 'nearest' });
}

// Scan `dir`, populate the file list, and optionally select a specific file
// (by absolute path) — loading it and scrolling it into view. If no
// selectedPath is given, the first file is loaded (matching the previous
// auto-load behavior).
function setDirectory(dir, opts) {
    opts = opts || {};
    const autoload = opts.autoload !== false;
    const selectedPath = opts.selectedPath || null;
    const normalized = path.normalize(dir).replace(/\\/g, '/');
    const res = scanDir(normalized);

    state.dir = normalized;
    dirStatus.textContent = normalized;

    if (!res.ok) {
        state.files = [];
        state.fileIndex = -1;
        dirStatus.textContent = 'Error: ' + res.error;
        dirStatus.style.color = '#ff6b6b';
        clearNodes();
        setStatus('Invalid directory');
        setInfo('');
        renderFileList();
        return;
    }

    try { localStorage.setItem(STORAGE_KEY, normalized); } catch (e) {}

    state.files = res.files;
    const countMsg = res.files.length + ' file' + (res.files.length === 1 ? '' : 's') + ' · ' + normalized;
    dirStatus.textContent = countMsg;
    dirStatus.style.color = '#888';

    if (state.files.length === 0) {
        state.fileIndex = -1;
        clearNodes();
        setStatus('No .glb/.gltf in directory');
        setInfo('');
        renderFileList();
        return;
    }

    let targetIdx = 0;
    if (selectedPath) {
        const normSel = path.normalize(selectedPath).replace(/\\/g, '/');
        const found = state.files.indexOf(normSel);
        if (found >= 0) targetIdx = found;
    }
    state.fileIndex = targetIdx;
    renderFileList();
    if (autoload) loadFile(targetIdx);
}

// Native folder picker — scans the picked folder and shows its contents
// in the file list. [/] walks through siblings.
function openFolderDialog() {
    if (typeof showOpenFolderDialog !== 'function') {
        setStatus('Native folder dialog unavailable');
        return;
    }
    const picked = showOpenFolderDialog(state.dir || null);
    if (!picked || picked.length === 0) return;
    const folder = picked[0].replace(/\\/g, '/');
    state.mode = 'folder';
    setDirectory(folder);
}

// Native file picker — loads exactly one file and does NOT scan its
// containing folder. The file list shows just the loaded entry; [/] is a
// no-op until the user opens a folder.
function openFileDialog() {
    if (typeof showOpenFileDialog !== 'function') {
        setStatus('Native file dialog unavailable');
        return;
    }
    const picked = showOpenFileDialog('GLB / GLTF|glb;gltf');
    if (!picked || picked.length === 0) return;
    const file = picked[0].replace(/\\/g, '/');
    state.mode = 'file';
    state.dir = '';
    state.files = [file];
    state.fileIndex = 0;
    dirStatus.textContent = 'single file · ' + path.dirname(file);
    dirStatus.style.color = '#888';
    renderFileList();
    loadFile(0);
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

function loadFile(idx) {
    clearNodes();

    if (idx < 0 || idx >= state.files.length) {
        setStatus('No file');
        return;
    }

    const filePath = state.files[idx];
    const name = fileName(filePath);
    setStatus('Loading ' + name + ' …');

    let gltf;
    try {
        gltf = Mesh.loadGLTF(filePath);
    } catch (e) {
        setStatus('FAILED: ' + e.message);
        setInfo('Could not load ' + filePath);
        return;
    }

    if (!gltf || !gltf.meshes || gltf.meshes.length === 0) {
        setStatus('No meshes in ' + name);
        setInfo('meshes=0 skins=' + (gltf?.skins?.length || 0)
            + ' skeletons=' + (gltf?.skeletons?.length || 0)
            + ' animations=' + (gltf?.animations?.length || 0));
        return;
    }

    const meshes = gltf.meshes;
    const hasSkin = gltf.skins && gltf.skins.length > 0 && gltf.skins[0].boneCount > 0;
    const hasSkel = gltf.skeletons && gltf.skeletons.length > 0 && gltf.skeletons[0].boneCount > 0;
    const hasAnim = gltf.animations && gltf.animations.length > 0;

    // Compute overall bbox across all meshes for camera framing.
    let minX=Infinity, minY=Infinity, minZ=Infinity;
    let maxX=-Infinity, maxY=-Infinity, maxZ=-Infinity;
    for (const m of meshes) {
        const bb = m.computeBBox();
        if (bb.min[0] < minX) minX = bb.min[0];
        if (bb.min[1] < minY) minY = bb.min[1];
        if (bb.min[2] < minZ) minZ = bb.min[2];
        if (bb.max[0] > maxX) maxX = bb.max[0];
        if (bb.max[1] > maxY) maxY = bb.max[1];
        if (bb.max[2] > maxZ) maxZ = bb.max[2];
    }
    const cx = (minX + maxX) * 0.5;
    const cy = (minY + maxY) * 0.5;
    const cz = (minZ + maxZ) * 0.5;
    const size = Math.max(maxX - minX, maxY - minY, maxZ - minZ);
    cam.target = [cx, cy, cz];
    cam.dist = Math.max(size * 2.2, 2);
    state.modelSize = size;

    // Record basePositions snapshot for skinning restore.
    const bindMeshes = [];
    for (let i = 0; i < meshes.length; i++) {
        const m = meshes[i];
        if (!m.hasNormals) m.computeNormals();
        bindMeshes.push({
            mesh: m,
            basePositions: new Float32Array(m.positions),
            baseNormals:   m.hasNormals ? new Float32Array(m.normals) : null,
        });
    }

    const materials = gltf.materials || [];
    const images    = gltf.images    || [];
    const meshMat   = gltf.meshMaterial || [];

    for (let i = 0; i < meshes.length; i++) {
        const opts = { data: meshes[i], name: 'mesh-' + i };

        const matIdx = meshMat[i] ?? -1;
        const mat = (matIdx >= 0 && matIdx < materials.length) ? materials[matIdx] : null;
        const imgIdx = mat ? mat.baseColorTexture : -1;
        const img = (imgIdx >= 0 && imgIdx < images.length) ? images[imgIdx] : null;

        if (img && img.data && img.width > 0 && img.height > 0) {
            opts.texture = { width: img.width, height: img.height, data: img.data };
            opts.color = mat.baseColorFactor || [1,1,1,1];
        } else {
            opts.color = PALETTE[i % PALETTE.length];
        }

        state.nodes.push(scene.createMesh(opts));
    }

    state.loaded = {
        path: filePath, name,
        gltf,
        bindMeshes,
        hasSkin, hasSkel, hasAnim,
        skeleton: hasSkel ? gltf.skeletons[0] : null,
        skin:     hasSkin ? gltf.skins[0]     : null,
        animations: gltf.animations || [],
        animIdx: hasAnim ? 0 : -1,
        animTime: 0,
    };

    state.time = 0;

    renderInfo();
    setStatus('[' + (idx+1) + '/' + state.files.length + '] ' + name);
}

function renderInfo() {
    const L = state.loaded;
    if (!L) { setInfo(''); return; }
    const lines = [];
    lines.push('file: ' + L.name);
    let totalV = 0, totalT = 0;
    for (const bm of L.bindMeshes) { totalV += bm.mesh.vertexCount; totalT += bm.mesh.triangleCount; }
    lines.push('meshes=' + L.bindMeshes.length + ' verts=' + totalV + ' tris=' + totalT);
    if (L.hasSkel) {
        lines.push('skeleton: bones=' + L.skeleton.boneCount + ' sockets=' + L.skeleton.socketCount);
    } else {
        lines.push('skeleton: none');
    }
    if (L.hasSkin) {
        lines.push('skin: verts=' + L.skin.vertexCount + ' bones=' + L.skin.boneCount);
    } else {
        lines.push('skin: none');
    }
    if (L.hasAnim) {
        const parts = L.animations.map((a, i) => (i === L.animIdx ? '[' : ' ')
            + (a.name || 'anim' + i) + ' (' + a.duration.toFixed(2) + 's)'
            + (i === L.animIdx ? ']' : ' '));
        lines.push('animations (' + L.animations.length + '): ' + parts.join(' '));
    } else {
        lines.push('animations: none');
    }
    lines.push(state.bindPoseOnly ? 'mode: BIND POSE' : (state.paused ? 'mode: PAUSED' : 'mode: playing'));
    setInfo(lines.join('\n'));
}

// ---------------------------------------------------------------------------
// Animation update (CPU skinning)
// ---------------------------------------------------------------------------

function updateAnimation(dtMs) {
    const L = state.loaded;
    if (!L || !L.hasSkin || !L.hasSkel) return;
    if (!state.enableSkinning) return;

    let pose;
    if (state.bindPoseOnly || !L.hasAnim || L.animIdx < 0) {
        pose = L.skeleton.bindPose();
    } else {
        if (!state.paused) L.animTime += dtMs * 0.001;
        const anim = L.animations[L.animIdx];
        let t = L.animTime;
        if (anim.duration > 0) t = t % anim.duration;
        pose = anim.evaluate(L.skeleton, t, { loop: true });
    }

    // applySkinning expects WORLD matrices (it multiplies by inverseBind
    // internally). Don't pass computeSkinningMatrices output or you get a
    // double-inverse-bind multiply.
    const mats = pose.computeWorldMatrices(L.skeleton);

    for (let i = 0; i < L.bindMeshes.length; i++) {
        const bm = L.bindMeshes[i];
        if (bm.mesh.vertexCount !== L.skin.vertexCount) continue;

        // Restore bind positions before skinning (applySkinning mutates in
        // place and would otherwise compound each frame).
        bm.mesh.positions = new Float32Array(bm.basePositions);
        if (bm.baseNormals) bm.mesh.normals = new Float32Array(bm.baseNormals);

        try {
            bm.mesh.applySkinning(L.skin, mats);
        } catch (e) {
            // swallow, keep bind pose
        }
        bm.mesh.computeNormals();
        state.nodes[i].updateMesh(bm.mesh);
    }
}

// ---------------------------------------------------------------------------
// Bone visualization
// ---------------------------------------------------------------------------

function setupBoneNodes() {
    clearBoneNodes();
    const L = state.loaded;
    if (!L || !L.hasSkel) return;
    const size = cam.dist * 0.012;
    for (let i = 0; i < L.skeleton.boneCount; i++) {
        const s = scene.createMesh({
            data: Mesh.sphere(size, 8, 6),
            color: '#ffe66d',
            emissive: 1.0,
            depthBias: [-1, -1000],
            name: 'bone-' + i,
        });
        state.boneNodes.push(s);
    }
}

function updateBoneNodes() {
    const L = state.loaded;
    if (!L || !L.hasSkel || state.boneNodes.length === 0) return;
    let pose;
    if (state.bindPoseOnly || !L.hasAnim || L.animIdx < 0) {
        pose = L.skeleton.bindPose();
    } else {
        const anim = L.animations[L.animIdx];
        let t = L.animTime;
        if (anim.duration > 0) t = t % anim.duration;
        pose = anim.evaluate(L.skeleton, t, { loop: true });
    }
    const world = pose.computeWorldMatrices(L.skeleton);
    const n = state.boneNodes.length;
    for (let i = 0; i < n; i++) {
        const b = i * 16;
        state.boneNodes[i].x = world[b + 12];
        state.boneNodes[i].y = world[b + 13];
        state.boneNodes[i].z = world[b + 14];
    }
}

// ---------------------------------------------------------------------------
// Camera + loop
// ---------------------------------------------------------------------------

let lastT = 0;
function frame(t) {
    if (!lastT) lastT = t;
    const dt = t - lastT;
    lastT = t;

    state.time += dt;
    updateAnimation(dt);
    if (state.showBones) updateBoneNodes();
    scene.setCamera(Camera.orbitViewOpts(cam, canvas));

    requestAnimationFrame(frame);
}

// ---------------------------------------------------------------------------
// UI wiring — playback / view toggles
// ---------------------------------------------------------------------------

const pausePlayBtn = document.getElementById('btn-pauseplay');
const nextAnimBtn  = document.getElementById('btn-next-anim');
const bindPoseBtn  = document.getElementById('btn-bindpose');
const bonesBtn     = document.getElementById('btn-bones');

function syncToggleButtons() {
    pausePlayBtn.textContent = state.paused ? 'Play' : 'Pause';
    pausePlayBtn.classList.toggle('toggled', state.paused);
    bindPoseBtn.classList.toggle('toggled', state.bindPoseOnly);
    bonesBtn.classList.toggle('toggled', state.showBones);
}

pausePlayBtn.addEventListener('click', () => {
    state.paused = !state.paused;
    syncToggleButtons();
    renderInfo();
});

nextAnimBtn.addEventListener('click', () => {
    const L = state.loaded;
    if (!L || !L.hasAnim) return;
    L.animIdx = (L.animIdx + 1) % L.animations.length;
    L.animTime = 0;
    renderInfo();
});

bindPoseBtn.addEventListener('click', () => {
    state.bindPoseOnly = !state.bindPoseOnly;
    syncToggleButtons();
    renderInfo();
});

bonesBtn.addEventListener('click', () => {
    state.showBones = !state.showBones;
    if (state.showBones) setupBoneNodes();
    else clearBoneNodes();
    syncToggleButtons();
    renderInfo();
});

openFolderBtn.addEventListener('click', openFolderDialog);
openFileBtn.addEventListener('click', openFileDialog);

syncToggleButtons();

// --- Orbit camera input: right=rotate, middle=pan, wheel=zoom ----------------

// Pointer lock hides the cursor and delivers unbounded movementX/Y deltas —
// that's what lets a drag continue past the window edge. Lock whenever any
// drag button is held and release when both are up.
function updatePointerLock() {
    const want = rightDown || middleDown;
    const locked = document.pointerLockElement === canvas;
    if (want && !locked) canvas.requestPointerLock();
    else if (!want && locked) document.exitPointerLock();
}

canvas.addEventListener('mousedown', (e) => {
    if (e.button === 2) {
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
    if (rightDown)  Camera.orbitLook(cam, e.movementX, e.movementY);
    if (middleDown) Camera.orbitPan (cam, e.movementX, e.movementY);
});
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
// Browsers auto-scroll on middle-click if we don't swallow this.
canvas.addEventListener('auxclick', (e) => { if (e.button === 1) e.preventDefault(); });
canvas.addEventListener('wheel', (e) => {
    // Multiplicative zoom — feels consistent across model sizes. Clamp so
    // the camera never flips inside the target.
    const factor = Math.exp(e.deltaY * 0.001);
    cam.dist = Math.max(0.1, cam.dist * factor);
    e.preventDefault();
});

// ---------------------------------------------------------------------------
// Go
// ---------------------------------------------------------------------------

const initialDir = pickInitialDir();
if (initialDir) {
    state.mode = 'folder';
    setDirectory(initialDir);
} else {
    dirStatus.textContent = 'Open Folder… (O) to browse, or Open File… (F) for one mesh.';
    setStatus('Ready');
}
requestAnimationFrame(frame);
