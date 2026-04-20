// =============================================================================
// Mesh viewer + workbench — load .glb/.gltf/.obj/.ply/.stl, inspect, modify,
// generate LODs, bake AO/curvature/thickness into vertex colors, build collision
// hulls, export. CPU-skinned animation playback for rigged glTF.
//
// Modify ops replace each mesh's "work" copy. Animation pauses while modified
// (skinning needs the original topology). Reset restores the load-time state.
// =============================================================================

const fs   = require('fs');
const path = require('path');

const STORAGE_KEY = 'mesh-viewer:dir';
const LOAD_EXTS   = ['.glb', '.gltf', '.obj', '.ply', '.stl'];

const PALETTE = ['#e74c3c', '#3498db', '#2ecc71', '#f39c12', '#9b59b6',
                 '#16a085', '#d35400', '#c0392b', '#8e44ad'];

// ---------------------------------------------------------------------------
// DOM refs
// ---------------------------------------------------------------------------

const canvas    = document.getElementById('canvas');
const scene     = canvas.getContext('scene');
const statusEl  = document.getElementById('status');

const opsPanel  = document.getElementById('ops-panel');
const dropOverlay = document.getElementById('drop-overlay');
const helpEl    = document.getElementById('help');

const dirStatus     = document.getElementById('dir-status');
const fileListEl    = document.getElementById('file-list');
const openFolderBtn = document.getElementById('open-folder-btn');
const openFileBtn   = document.getElementById('open-file-btn');

// stats
const $st = {
    file:     document.getElementById('st-file'),
    meshes:   document.getElementById('st-meshes'),
    verts:    document.getElementById('st-verts'),
    tris:     document.getElementById('st-tris'),
    bbox:     document.getElementById('st-bbox'),
    manifold: document.getElementById('st-manifold'),
    rowMan:   document.getElementById('st-row-manifold'),
    volume:   document.getElementById('st-volume'),
    selfx:    document.getElementById('st-selfx'),
    rowSelfx: document.getElementById('st-row-selfx'),
    uvs:      document.getElementById('st-uvs'),
    colors:   document.getElementById('st-colors'),
};

// view
const viewModeSel = document.getElementById('view-mode');
const viewHullBtn  = document.getElementById('view-hull');
const viewSelfxBtn = document.getElementById('view-selfx');
const viewUVBtn    = document.getElementById('view-uv');
const viewBonesBtn = document.getElementById('view-bones');

// modify
const modSubLoopBtn   = document.getElementById('mod-sub-loop');
const modSubCCBtn     = document.getElementById('mod-sub-cc');
const modSubMidBtn    = document.getElementById('mod-sub-mid');
const modSmoothLapBtn = document.getElementById('mod-smooth-lap');
const modSmoothTauBtn = document.getElementById('mod-smooth-tau');
const modRemeshLenIn  = document.getElementById('mod-remesh-len');
const modRemeshBtn    = document.getElementById('mod-remesh');
const modSimplifyRng  = document.getElementById('mod-simplify-range');
const modSimplifyNum  = document.getElementById('mod-simplify-num');
const modUnwrapBtn    = document.getElementById('mod-unwrap');
const modResetBtn     = document.getElementById('mod-reset');

// LOD
const lodRange    = document.getElementById('lod-range');
const lodNum      = document.getElementById('lod-num');
const lodBuildBtn = document.getElementById('lod-build');
const lodClearBtn = document.getElementById('lod-clear');

// rig
const rigSection  = document.getElementById('rig-section');
const rigPauseBtn = document.getElementById('rig-pause');
const rigBindBtn  = document.getElementById('rig-bind');
const animListEl  = document.getElementById('anim-list');
const blendRow    = document.getElementById('blend-row');
const blendRange  = document.getElementById('blend-range');
const blendNum    = document.getElementById('blend-num');

// export
const expGlbBtn = document.getElementById('exp-glb');
const expObjBtn = document.getElementById('exp-obj');
const expPlyBtn = document.getElementById('exp-ply');
const expStlBtn = document.getElementById('exp-stl');

// uv inset
const uvInset    = document.getElementById('uv-inset');
const uvCanvas   = document.getElementById('uv-canvas');
const uvCtx      = uvCanvas.getContext('2d');

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let state = {
    mode: 'folder',
    dir: '',
    files: [],
    fileIndex: -1,
    loaded: null,           // { path, name, gltf, items[], skeleton, skin, animations, ... }
    paused: false,
    bindPoseOnly: false,
    panelHidden: false,

    view: {
        color:  'original',
        hull:   false,
        selfx:  false,
        uv:     false,
        bones:  false,
    },
    modify:  { dirty: false },
    lod:     { ratio: 1.0, built: false },
    rig:     { active: -1, blend: -1, blendW: 0.5 },
    boneNodes: [],
};

// loaded.items[i] : {
//     bind:      Mesh,         // original geometry, never mutated
//     basePositions, baseNormals, baseColors,  // snapshots from bind
//     work:      Mesh,         // currently displayed; mutated by bake/modify
//     node:      SceneNode,    // main render node
//     hullNode:  SceneNode?,   // convex-hull overlay (lazy)
//     selfxNode: SceneNode?,   // self-intersection highlight (lazy)
//     progressive: ProgressiveMesh?,  // built lazily for LOD
// }

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

const cam = Camera.createOrbit({ target: [0, 0, 0], dist: 6, fov: 45 });
let rightDown  = false;
let middleDown = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function setStatus(s, kind) {
    statusEl.textContent = s;
    statusEl.className = kind || '';
}

function fileName(p) { return p.replace(/\\/g, '/').split('/').pop(); }

function fileExt(p) {
    const m = /\.([^.\\/]+)$/.exec(p);
    return m ? '.' + m[1].toLowerCase() : '';
}

function fmtNum(n) {
    if (n === undefined || n === null) return '—';
    if (typeof n !== 'number') return String(n);
    if (Math.abs(n) >= 1000) return n.toFixed(0).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
    return n.toFixed(Math.abs(n) < 10 ? 3 : 2);
}

function fmtVec3(v) {
    if (!v) return '—';
    return v.map(x => x.toFixed(2)).join(', ');
}

function clearNodes() {
    if (!state.loaded) return;
    for (const it of state.loaded.items) {
        if (it.node)      { it.node.destroy();      it.node = null; }
        if (it.hullNode)  { it.hullNode.destroy();  it.hullNode = null; }
        if (it.selfxNode) { it.selfxNode.destroy(); it.selfxNode = null; }
    }
    clearBoneNodes();
    state.loaded = null;
}

function clearBoneNodes() {
    for (const n of state.boneNodes) n.destroy();
    state.boneNodes = [];
}

// ---------------------------------------------------------------------------
// Loaders
// ---------------------------------------------------------------------------

function loadAnyMesh(filePath) {
    const ext = fileExt(filePath);
    if (ext === '.glb' || ext === '.gltf') return Mesh.loadGLTF(filePath);
    // OBJ/PLY/STL: synthesize a minimal "gltf-like" struct so the rest of the
    // pipeline doesn't care about the source format.
    let m;
    if      (ext === '.obj') m = Mesh.loadOBJ(filePath);
    else if (ext === '.ply') m = Mesh.loadPLY(filePath);
    else if (ext === '.stl') m = Mesh.loadSTL(filePath);
    else throw new Error('Unsupported extension: ' + ext);
    return {
        meshes: [m],
        skins: [], skeletons: [], animations: [],
        materials: [], images: [], meshMaterial: [],
    };
}

// ---------------------------------------------------------------------------
// Directory scan & file list
// ---------------------------------------------------------------------------

function pickInitialDir() {
    try {
        const saved = localStorage.getItem(STORAGE_KEY);
        if (saved && fs.existsSync(saved)) return saved;
    } catch (e) {}
    return null;
}

function scanDir(dir) {
    let entries;
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); }
    catch (e) { return { ok: false, error: e.message, files: [] }; }
    const files = [];
    for (const e of entries) {
        if (!e.isFile || !e.isFile()) continue;
        const lower = e.name.toLowerCase();
        if (LOAD_EXTS.some(ext => lower.endsWith(ext))) files.push(path.join(dir, e.name));
    }
    files.sort((a, b) => fileName(a).localeCompare(fileName(b)));
    return { ok: true, files };
}

function renderFileList() {
    fileListEl.innerHTML = '';
    if (state.files.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'empty';
        empty.textContent = 'No mesh files in this directory.';
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
    const sel = fileListEl.querySelector('.file-item.selected');
    if (sel && sel.scrollIntoView) sel.scrollIntoView({ block: 'nearest' });
}

function setDirectory(dir, opts) {
    opts = opts || {};
    const autoload = opts.autoload !== false;
    const selectedPath = opts.selectedPath || null;
    const normalized = path.normalize(dir).replace(/\\/g, '/');
    const res = scanDir(normalized);

    state.dir = normalized;
    dirStatus.textContent = normalized;

    if (!res.ok) {
        state.files = []; state.fileIndex = -1;
        dirStatus.textContent = 'Error: ' + res.error;
        dirStatus.style.color = '#ff6b6b';
        clearNodes(); renderStats(); renderFileList();
        setStatus('Invalid directory', 'error');
        return;
    }

    try { localStorage.setItem(STORAGE_KEY, normalized); } catch (e) {}

    state.files = res.files;
    dirStatus.textContent = res.files.length + ' file' + (res.files.length === 1 ? '' : 's') + ' · ' + normalized;
    dirStatus.style.color = '#888';

    if (state.files.length === 0) {
        state.fileIndex = -1;
        clearNodes(); renderStats(); renderFileList();
        setStatus('No mesh files in directory', 'warn');
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

function openFolderDialog() {
    if (typeof showOpenFolderDialog !== 'function') {
        setStatus('Native folder dialog unavailable', 'error'); return;
    }
    const picked = showOpenFolderDialog(state.dir || null);
    if (!picked || picked.length === 0) return;
    state.mode = 'folder';
    setDirectory(picked[0].replace(/\\/g, '/'));
}

function openFileDialog() {
    if (typeof showOpenFileDialog !== 'function') {
        setStatus('Native file dialog unavailable', 'error'); return;
    }
    const picked = showOpenFileDialog('Mesh|glb;gltf;obj;ply;stl');
    if (!picked || picked.length === 0) return;
    loadStandalonePath(picked[0].replace(/\\/g, '/'));
}

function loadStandalonePath(p) {
    state.mode = 'file';
    state.dir = '';
    state.files = [p];
    state.fileIndex = 0;
    dirStatus.textContent = 'single file · ' + path.dirname(p);
    dirStatus.style.color = '#888';
    renderFileList();
    loadFile(0);
}

// ---------------------------------------------------------------------------
// Loading + initial scene setup
// ---------------------------------------------------------------------------

function loadFile(idx) {
    clearNodes();
    resetUIState();

    if (idx < 0 || idx >= state.files.length) { setStatus('No file', 'warn'); return; }

    const filePath = state.files[idx];
    const name = fileName(filePath);
    setStatus('Loading ' + name + ' …');

    let gltf;
    try { gltf = loadAnyMesh(filePath); }
    catch (e) { setStatus('FAILED: ' + e.message, 'error'); return; }

    if (!gltf || !gltf.meshes || gltf.meshes.length === 0) {
        setStatus('No meshes in ' + name, 'warn');
        return;
    }

    const meshes  = gltf.meshes;
    const hasSkin = gltf.skins      && gltf.skins.length      > 0 && gltf.skins[0].boneCount > 0;
    const hasSkel = gltf.skeletons  && gltf.skeletons.length  > 0 && gltf.skeletons[0].boneCount > 0;
    const hasAnim = gltf.animations && gltf.animations.length > 0;

    // Frame camera on combined bbox.
    let lo = [ Infinity,  Infinity,  Infinity];
    let hi = [-Infinity, -Infinity, -Infinity];
    for (const m of meshes) {
        const bb = m.computeBBox();
        for (let i = 0; i < 3; i++) { if (bb.min[i] < lo[i]) lo[i] = bb.min[i]; if (bb.max[i] > hi[i]) hi[i] = bb.max[i]; }
    }
    const center = [(lo[0]+hi[0])*0.5, (lo[1]+hi[1])*0.5, (lo[2]+hi[2])*0.5];
    const size = Math.max(hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]) || 1;
    Camera.orbitReframe(cam, center, Math.max(size * 2.2, 2));

    // Per-mesh items.
    const materials = gltf.materials || [];
    const images    = gltf.images    || [];
    const meshMat   = gltf.meshMaterial || [];
    const items = [];
    for (let i = 0; i < meshes.length; i++) {
        const bind = meshes[i];
        if (!bind.hasNormals) bind.computeNormals();
        // bind is the pristine source — never mutated. work is a mutable
        // clone that ops/baking/skinning operate on.
        const work = bind.clone();

        const opts = { data: work, name: 'mesh-' + i };
        const matIdx = meshMat[i] ?? -1;
        const mat = (matIdx >= 0 && matIdx < materials.length) ? materials[matIdx] : null;
        const imgIdx = mat ? mat.baseColorTexture : -1;
        const img = (imgIdx >= 0 && imgIdx < images.length) ? images[imgIdx] : null;
        if (img && img.data && img.width > 0 && img.height > 0) {
            opts.texture = { width: img.width, height: img.height, data: img.data };
            opts.color   = mat.baseColorFactor || [1,1,1,1];
        } else {
            opts.color = PALETTE[i % PALETTE.length];
        }

        items.push({
            bind, work,
            basePositions: new Float32Array(bind.positions),
            baseNormals:   bind.hasNormals ? new Float32Array(bind.normals) : null,
            baseColors:    bind.hasColors ? new Float32Array(bind.colors) : null,
            node: scene.createMesh(opts),
            hullNode: null,
            selfxNode: null,
            progressive: null,
        });
    }

    state.loaded = {
        path: filePath, name, gltf,
        items,
        hasSkin, hasSkel, hasAnim,
        skeleton: hasSkel ? gltf.skeletons[0] : null,
        skin:     hasSkin ? gltf.skins[0]     : null,
        animations: gltf.animations || [],
    };

    state.modify.dirty = false;
    state.lod.built = false;
    state.lod.ratio = 1.0;

    if (hasAnim) state.rig.active = 0;
    state.rig.blend = -1;
    state.rig.blendW = 0.5;

    rigSection.style.display = (hasAnim || hasSkel) ? '' : 'none';
    renderRigUI();
    renderStats();
    syncControls();

    setStatus('[' + (idx+1) + '/' + state.files.length + '] ' + name);
}

function resetUIState() {
    state.view = { color: 'original', hull: false, selfx: false, uv: false, bones: false };
    state.lod = { ratio: 1.0, built: false };
    state.rig.active = -1;
    state.rig.blend = -1;
    viewModeSel.value = 'original';
    lodRange.value = 1.0;
    lodRange.disabled = true;
    lodNum.textContent = '—';
    modSimplifyRng.value = 1.0;
    modSimplifyNum.textContent = '100%';
    uvInset.classList.remove('show');
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

function renderStats() {
    const L = state.loaded;
    if (!L) {
        $st.file.textContent = '—';
        $st.meshes.textContent = '0';
        $st.verts.textContent = '0';
        $st.tris.textContent = '0';
        $st.bbox.textContent = '—';
        $st.manifold.textContent = '—';
        $st.volume.textContent = '—';
        $st.selfx.textContent = '—';
        $st.uvs.textContent = '—';
        $st.colors.textContent = '—';
        $st.rowMan.classList.remove('ok', 'bad');
        $st.rowSelfx.classList.remove('ok', 'bad');
        return;
    }

    $st.file.textContent = L.name;
    $st.meshes.textContent = L.items.length;

    let totalV = 0, totalT = 0;
    let lo = [ Infinity,  Infinity,  Infinity];
    let hi = [-Infinity, -Infinity, -Infinity];
    let hasUVs = false, hasColors = false;
    for (const it of L.items) {
        const w = it.work;
        totalV += w.vertexCount;
        totalT += w.triangleCount;
        if (w.hasUVs)    hasUVs = true;
        if (w.hasColors) hasColors = true;
        const bb = w.computeBBox();
        for (let i = 0; i < 3; i++) { if (bb.min[i] < lo[i]) lo[i] = bb.min[i]; if (bb.max[i] > hi[i]) hi[i] = bb.max[i]; }
    }
    $st.verts.textContent = fmtNum(totalV);
    $st.tris.textContent  = fmtNum(totalT);
    const ext = [hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]];
    $st.bbox.textContent = fmtVec3(ext);

    // Manifold / volume / self-int — only for single-mesh files, and skipped
    // automatically on heavy meshes (self-int is O(tris²) without acceleration
    // and would freeze the UI on >50k tri meshes). Click the overlay buttons
    // to compute on demand.
    const HEAVY_TRI = 50000;
    if (L.items.length === 1 && totalT <= HEAVY_TRI) {
        const w = L.items[0].work;
        let manifold = false;
        try { manifold = w.isManifold(); } catch (e) {}
        $st.manifold.textContent = manifold ? 'yes' : 'no';
        $st.rowMan.classList.toggle('ok',  manifold);
        $st.rowMan.classList.toggle('bad', !manifold);

        try { $st.volume.textContent = manifold ? fmtNum(w.computeVolume()) : 'n/a'; }
        catch (e) { $st.volume.textContent = '—'; }

        try {
            const sx = w.findSelfIntersections();
            const cnt = sx ? sx.length : 0;
            $st.selfx.textContent = cnt === 0 ? 'none' : (cnt + ' pair' + (cnt === 1 ? '' : 's'));
            $st.rowSelfx.classList.toggle('ok',  cnt === 0);
            $st.rowSelfx.classList.toggle('bad', cnt > 0);
        } catch (e) { $st.selfx.textContent = '—'; }
    } else if (L.items.length === 1) {
        $st.manifold.textContent = 'skipped';
        $st.volume.textContent   = 'skipped';
        $st.selfx.textContent    = 'skipped (>50k tris)';
        $st.rowMan.classList.remove('ok', 'bad');
        $st.rowSelfx.classList.remove('ok', 'bad');
    } else {
        $st.manifold.textContent = '(' + L.items.length + ' meshes)';
        $st.volume.textContent = '—';
        $st.selfx.textContent = '—';
        $st.rowMan.classList.remove('ok', 'bad');
        $st.rowSelfx.classList.remove('ok', 'bad');
    }

    $st.uvs.textContent    = hasUVs ? 'yes' : 'no';
    $st.colors.textContent = hasColors ? 'yes' : 'no';
}

// ---------------------------------------------------------------------------
// View — color modes (vertex colors)
// ---------------------------------------------------------------------------

// Replace the work mesh's colors based on the chosen mode and push to the
// scene node. View mode bakes are non-destructive to topology — only colors
// change — so they're safe even when "modify" hasn't been touched.
function applyColorMode(mode) {
    state.view.color = mode;
    const L = state.loaded;
    if (!L) return;

    for (const it of L.items) {
        const w = it.work;
        if (mode === 'original') {
            if (it.baseColors) w.colors = new Float32Array(it.baseColors);
            else { try { w.colors = new Float32Array(0); } catch (e) {} }
        } else {
            try {
                if      (mode === 'normals')   colorByNormals(w);
                else if (mode === 'curvature') w.bakeCurvature(1.0);
                else if (mode === 'ao')        w.bakeAmbientOcclusion(64, 0);
                else if (mode === 'thickness') w.bakeThickness(32, 0);
            } catch (e) {
                setStatus(mode + ' bake failed: ' + e.message, 'error');
                continue;
            }
        }
        it.node.updateMesh(w);
    }
    if (mode !== 'original') setStatus('Baked: ' + mode);
}

function colorByNormals(mesh) {
    if (!mesh.hasNormals) mesh.computeNormals();
    const n = mesh.normals;
    const nv = mesh.vertexCount;
    const c = new Float32Array(nv * 4);
    for (let i = 0; i < nv; i++) {
        c[i*4 + 0] = n[i*3 + 0] * 0.5 + 0.5;
        c[i*4 + 1] = n[i*3 + 1] * 0.5 + 0.5;
        c[i*4 + 2] = n[i*3 + 2] * 0.5 + 0.5;
        c[i*4 + 3] = 1.0;
    }
    mesh.colors = c;
}

// ---------------------------------------------------------------------------
// View — convex hull overlay
// ---------------------------------------------------------------------------

function setHullVisible(on) {
    state.view.hull = on;
    const L = state.loaded;
    if (!L) return;
    for (const it of L.items) {
        if (on) {
            if (!it.hullNode) {
                let hull;
                try { hull = it.work.convexHull(); }
                catch (e) { setStatus('Hull failed: ' + e.message, 'error'); state.view.hull = false; return; }
                it.hullNode = scene.createMesh({
                    data: hull,
                    color: [1.0, 0.85, 0.2, 1.0],
                    emissive: 0.6,
                    name: 'hull-' + it.node.name,
                });
                // Sit just outside the source so it's visible.
                it.hullNode.scaleX = it.hullNode.scaleY = it.hullNode.scaleZ = 1.01;
            } else {
                it.hullNode.visible = true;
            }
        } else if (it.hullNode) {
            it.hullNode.visible = false;
        }
    }
}

// ---------------------------------------------------------------------------
// View — self-intersection highlight (overlay sub-mesh of the bad triangles)
// ---------------------------------------------------------------------------

function setSelfxVisible(on) {
    state.view.selfx = on;
    const L = state.loaded;
    if (!L) return;
    for (const it of L.items) {
        if (it.selfxNode) { it.selfxNode.destroy(); it.selfxNode = null; }
        if (!on) continue;
        let pairs;
        try { pairs = it.work.findSelfIntersections(); }
        catch (e) { setStatus('Self-int failed: ' + e.message, 'error'); state.view.selfx = false; return; }
        if (!pairs || pairs.length === 0) continue;

        const srcPos = it.work.positions;
        const srcIdx = it.work.indices;
        const triSet = new Set();
        for (const p of pairs) { triSet.add(p.triA); triSet.add(p.triB); }

        const tris = [...triSet];
        const newIdx = new Uint32Array(tris.length * 3);
        for (let i = 0; i < tris.length; i++) {
            const t = tris[i];
            newIdx[i*3 + 0] = srcIdx[t*3 + 0];
            newIdx[i*3 + 1] = srcIdx[t*3 + 1];
            newIdx[i*3 + 2] = srcIdx[t*3 + 2];
        }
        it.selfxNode = scene.createMesh({
            positions: new Float32Array(srcPos),
            indices: newIdx,
            color: [1.0, 0.15, 0.15, 1.0],
            emissive: 1.0,
            depthBias: [-1, -1000],
            name: 'selfx-' + it.node.name,
        });
    }
}

// ---------------------------------------------------------------------------
// View — UV inset
// ---------------------------------------------------------------------------

function drawUVInset() {
    const W = uvCanvas.width, H = uvCanvas.height;
    uvCtx.fillStyle = '#050505';
    uvCtx.fillRect(0, 0, W, H);

    if (!state.view.uv || !state.loaded) return;

    // Frame around [0,1]x[0,1]
    uvCtx.strokeStyle = '#222';
    uvCtx.lineWidth = 1;
    uvCtx.strokeRect(0.5, 0.5, W - 1, H - 1);

    const colors = ['#74b9ff', '#7bed9f', '#ffa502', '#ff7675', '#a29bfe', '#fdcb6e'];
    let drawn = 0;
    for (let mi = 0; mi < state.loaded.items.length; mi++) {
        const m = state.loaded.items[mi].work;
        if (!m.hasUVs) continue;
        const uv  = m.uvs;
        const idx = m.indices;
        if (!uv || !idx) continue;
        const tris = idx.length / 3;
        uvCtx.strokeStyle = colors[mi % colors.length];
        uvCtx.lineWidth = 0.5;
        uvCtx.globalAlpha = 0.7;
        uvCtx.beginPath();
        for (let t = 0; t < tris; t++) {
            const a = idx[t*3], b = idx[t*3 + 1], c = idx[t*3 + 2];
            // V flipped (image coords)
            const ax = uv[a*2] * W,         ay = (1 - uv[a*2 + 1]) * H;
            const bx = uv[b*2] * W,         by = (1 - uv[b*2 + 1]) * H;
            const cx = uv[c*2] * W,         cy = (1 - uv[c*2 + 1]) * H;
            uvCtx.moveTo(ax, ay); uvCtx.lineTo(bx, by);
            uvCtx.lineTo(cx, cy); uvCtx.lineTo(ax, ay);
        }
        uvCtx.stroke();
        uvCtx.globalAlpha = 1.0;
        drawn++;
    }
    if (drawn === 0) {
        uvCtx.fillStyle = '#666';
        uvCtx.font = '11px monospace';
        uvCtx.textAlign = 'center';
        uvCtx.fillText('no UVs', W / 2, H / 2);
    }
}

function setUVVisible(on) {
    state.view.uv = on;
    uvInset.classList.toggle('show', on);
    if (on) drawUVInset();
}

// ---------------------------------------------------------------------------
// Modify ops — replace work mesh in place
// ---------------------------------------------------------------------------

// Apply `op(mesh)` to every mesh's work copy. `op` may mutate or return a new
// Mesh. After application, drops skinning/animation (modified topology no
// longer matches the skin) and refreshes downstream visuals.
function applyMeshOp(label, op) {
    const L = state.loaded;
    if (!L) return;
    setStatus(label + ' …');
    const t0 = performance.now();
    try {
        for (const it of L.items) {
            const result = op(it.work);
            if (result && result !== it.work) it.work = result;
            // Bake operations etc. left work mesh's colors set; keep view mode
            // selection stable by clearing colors when topology changed.
            if (state.view.color === 'original' && it.baseColors && it.work.vertexCount === it.bind.vertexCount) {
                // If verts still match bind, restore base colors.
                it.work.colors = new Float32Array(it.baseColors);
            } else if (state.view.color === 'original') {
                try { it.work.colors = new Float32Array(0); } catch (e) {}
            }
            it.node.updateMesh(it.work);
            // Invalidate per-mesh derived nodes / structures.
            if (it.hullNode)  { it.hullNode.destroy();  it.hullNode = null; }
            if (it.selfxNode) { it.selfxNode.destroy(); it.selfxNode = null; }
            it.progressive = null;
        }
    } catch (e) {
        setStatus(label + ' failed: ' + e.message, 'error');
        return;
    }
    state.modify.dirty = true;
    state.lod.built = false;
    state.lod.ratio = 1.0;
    lodRange.value = 1.0;
    lodRange.disabled = true;
    lodNum.textContent = '—';

    // Re-apply view modes that depend on the new geometry.
    if (state.view.color !== 'original') applyColorMode(state.view.color);
    if (state.view.hull)  setHullVisible(true);
    if (state.view.selfx) setSelfxVisible(true);
    if (state.view.uv)    drawUVInset();

    renderStats();
    syncControls();
    const dt = (performance.now() - t0).toFixed(0);
    setStatus(label + ' · ' + dt + ' ms');
}

function resetMods() {
    const L = state.loaded;
    if (!L) return;
    for (const it of L.items) {
        // Topology may have changed (subdivide/simplify/remesh), so positions
        // snapshots no longer match — rebuild work from the pristine bind.
        it.work = it.bind.clone();
        it.node.updateMesh(it.work);
        if (it.hullNode)  { it.hullNode.destroy();  it.hullNode = null; }
        if (it.selfxNode) { it.selfxNode.destroy(); it.selfxNode = null; }
        it.progressive = null;
    }
    state.modify.dirty = false;
    state.lod.built = false;
    state.lod.ratio = 1.0;
    lodRange.value = 1.0;
    lodRange.disabled = true;
    lodNum.textContent = '—';
    if (state.view.color !== 'original') applyColorMode(state.view.color);
    if (state.view.hull)  setHullVisible(true);
    if (state.view.selfx) setSelfxVisible(true);
    if (state.view.uv)    drawUVInset();
    renderStats();
    syncControls();
    setStatus('Reset to bind');
}

// ---------------------------------------------------------------------------
// LOD (ProgressiveMesh)
// ---------------------------------------------------------------------------

function buildLODChain() {
    const L = state.loaded;
    if (!L) return;
    setStatus('Building LOD chain …');
    const t0 = performance.now();
    try {
        for (const it of L.items) it.progressive = new ProgressiveMesh(it.work);
    } catch (e) {
        setStatus('LOD build failed: ' + e.message, 'error');
        return;
    }
    state.lod.built = true;
    state.lod.ratio = 1.0;
    lodRange.value = 1.0;
    lodRange.disabled = false;
    lodNum.textContent = '100%';
    setStatus('LOD chain built · ' + (performance.now() - t0).toFixed(0) + ' ms');
}

function applyLOD(ratio) {
    const L = state.loaded;
    if (!L || !state.lod.built) return;
    state.lod.ratio = ratio;
    lodNum.textContent = (ratio * 100).toFixed(0) + '%';
    let totalT = 0;
    for (const it of L.items) {
        if (!it.progressive) continue;
        const lod = it.progressive.atRatio(ratio);
        it.node.updateMesh(lod);
        totalT += lod.triangleCount;
    }
    // Don't replace it.work — LOD is preview-only. Reset on Reset/Modify.
    setStatus('LOD ' + (ratio*100).toFixed(0) + '% · ' + fmtNum(totalT) + ' tris');
}

function clearLOD() {
    const L = state.loaded;
    if (!L) return;
    for (const it of L.items) it.progressive = null;
    state.lod.built = false;
    state.lod.ratio = 1.0;
    lodRange.value = 1.0;
    lodRange.disabled = true;
    lodNum.textContent = '—';
    // Re-push the work mesh so the node returns to current state.
    for (const it of L.items) it.node.updateMesh(it.work);
}

// ---------------------------------------------------------------------------
// Bones
// ---------------------------------------------------------------------------

function setupBoneNodes() {
    clearBoneNodes();
    const L = state.loaded;
    if (!L || !L.hasSkel) return;
    const size = cam.dist * 0.012;
    for (let i = 0; i < L.skeleton.boneCount; i++) {
        state.boneNodes.push(scene.createMesh({
            data: Mesh.sphere(size, 8, 6),
            color: '#ffe66d',
            emissive: 1.0,
            depthBias: [-1, -1000],
            name: 'bone-' + i,
        }));
    }
}

function updateBoneNodes(pose) {
    const L = state.loaded;
    if (!L || !L.hasSkel || state.boneNodes.length === 0) return;
    const world = pose.computeWorldMatrices(L.skeleton);
    for (let i = 0; i < state.boneNodes.length; i++) {
        const b = i * 16;
        state.boneNodes[i].x = world[b + 12];
        state.boneNodes[i].y = world[b + 13];
        state.boneNodes[i].z = world[b + 14];
    }
}

// ---------------------------------------------------------------------------
// Animation update
// ---------------------------------------------------------------------------

let animTime = 0;

function currentPose(L) {
    if (state.bindPoseOnly || state.rig.active < 0 || !L.hasAnim) return L.skeleton.bindPose();
    const a  = L.animations[state.rig.active];
    const ta = a.duration > 0 ? animTime % a.duration : animTime;
    const pa = a.evaluate(L.skeleton, ta, { loop: true });

    if (state.rig.blend < 0) return pa;
    const b = L.animations[state.rig.blend];
    if (!b) return pa;
    const tb = b.duration > 0 ? animTime % b.duration : animTime;
    const pb = b.evaluate(L.skeleton, tb, { loop: true });
    try { return Pose.blend(pa, pb, state.rig.blendW); }
    catch (e) { return pa; }
}

function updateAnimation(dtMs) {
    const L = state.loaded;
    if (!L || !L.hasSkin || !L.hasSkel) return;
    if (state.modify.dirty || state.lod.built) return;     // topology mismatch — don't skin

    if (!state.paused && !state.bindPoseOnly) animTime += dtMs * 0.001;

    const pose = currentPose(L);
    const mats = pose.computeWorldMatrices(L.skeleton);

    for (let i = 0; i < L.items.length; i++) {
        const it = L.items[i];
        if (it.work.vertexCount !== L.skin.vertexCount) continue;

        // applySkinning mutates positions in place — restore the bind snapshot
        // first so frames don't compound.
        it.work.positions = new Float32Array(it.basePositions);
        if (it.baseNormals) it.work.normals = new Float32Array(it.baseNormals);

        try { it.work.applySkinning(L.skin, mats); }
        catch (e) {}
        it.work.computeNormals();
        it.node.updateMesh(it.work);
    }

    if (state.view.bones) updateBoneNodes(pose);
}

// ---------------------------------------------------------------------------
// Rig UI
// ---------------------------------------------------------------------------

function renderRigUI() {
    animListEl.innerHTML = '';
    const L = state.loaded;
    if (!L || !L.hasAnim) {
        const e = document.createElement('div');
        e.className = 'anim-item'; e.textContent = '(no animations)';
        animListEl.appendChild(e);
        blendRow.style.display = 'none';
        return;
    }
    for (let i = 0; i < L.animations.length; i++) {
        const a = L.animations[i];
        const el = document.createElement('div');
        let cls = 'anim-item';
        if (i === state.rig.active) cls += ' active';
        if (i === state.rig.blend)  cls += ' blend';
        el.className = cls;
        el.textContent = (a.name || ('anim ' + i)) + ' · ' + a.duration.toFixed(2) + 's';
        el.addEventListener('click', (ev) => {
            if (ev.shiftKey) {
                state.rig.blend = (state.rig.blend === i) ? -1 : i;
                if (state.rig.blend === state.rig.active) state.rig.blend = -1;
            } else {
                state.rig.active = i;
                if (state.rig.blend === i) state.rig.blend = -1;
            }
            renderRigUI();
        });
        animListEl.appendChild(el);
    }
    const showBlend = state.rig.blend >= 0;
    blendRow.style.display = showBlend ? '' : 'none';
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

// Save the currently displayed (work) mesh state to disk. For multi-mesh
// glTF, glTF re-export bundles all of them; OBJ/PLY/STL save only the first
// mesh (the formats don't natively cluster multiple meshes the same way).
function exportMesh(format) {
    const L = state.loaded;
    if (!L) return;
    if (typeof showSaveFileDialog !== 'function') {
        setStatus('Native save dialog unavailable', 'error'); return;
    }
    const ext = '.' + format;
    const baseName = (L.name || 'mesh').replace(/\.[^.]+$/, '') + ext;
    const filter = format.toUpperCase() + '|' + format;
    const target = showSaveFileDialog(filter, baseName);
    if (!target) return;
    const out = target.replace(/\\/g, '/');

    setStatus('Saving ' + fileName(out) + ' …');
    try {
        const m = L.items[0].work;
        let ok;
        if (format === 'glb' || format === 'gltf') {
            // For glTF, prefer skinned save when we still have the skeleton/skin
            // and topology is intact; otherwise fall back to plain mesh save.
            const canSkin = !state.modify.dirty && !state.lod.built && L.hasSkel && L.hasSkin;
            if (canSkin) {
                ok = m.saveGLTF(out, {
                    skin: L.skin,
                    skeleton: L.skeleton,
                    animations: L.animations,
                });
            } else {
                ok = m.saveGLTF(out);
            }
        } else if (format === 'obj') ok = m.saveOBJ(out);
        else if (format === 'ply') ok = m.savePLY(out);
        else if (format === 'stl') ok = m.saveSTL(out);
        if (ok) setStatus('Saved ' + fileName(out));
        else    setStatus('Save returned false', 'error');
    } catch (e) {
        setStatus('Save failed: ' + e.message, 'error');
    }
}

// ---------------------------------------------------------------------------
// Sync — keep all toggle buttons in lockstep with state
// ---------------------------------------------------------------------------

function syncControls() {
    rigPauseBtn.textContent = state.paused ? 'Play' : 'Pause';
    rigPauseBtn.classList.toggle('toggled', state.paused);
    rigBindBtn.classList.toggle('toggled', state.bindPoseOnly);

    viewHullBtn .classList.toggle('toggled', state.view.hull);
    viewSelfxBtn.classList.toggle('toggled', state.view.selfx);
    viewUVBtn   .classList.toggle('toggled', state.view.uv);
    viewBonesBtn.classList.toggle('toggled', state.view.bones);

    modResetBtn.disabled = !state.modify.dirty && !state.lod.built;
    lodBuildBtn.disabled = state.lod.built;
    lodClearBtn.disabled = !state.lod.built;

    const have = !!state.loaded;
    expGlbBtn.disabled = !have;
    expObjBtn.disabled = !have;
    expPlyBtn.disabled = !have;
    expStlBtn.disabled = !have;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

let lastT = 0;
function frame(t) {
    if (!lastT) lastT = t;
    const dt = t - lastT;
    lastT = t;
    updateAnimation(dt);
    scene.setCamera(Camera.orbitViewOpts(cam, canvas));
    requestAnimationFrame(frame);
}

// ---------------------------------------------------------------------------
// UI wiring
// ---------------------------------------------------------------------------

openFolderBtn.addEventListener('click', openFolderDialog);
openFileBtn  .addEventListener('click', openFileDialog);

// View
viewModeSel.addEventListener('change', () => applyColorMode(viewModeSel.value));
viewHullBtn .addEventListener('click', () => { setHullVisible(!state.view.hull);    syncControls(); });
viewSelfxBtn.addEventListener('click', () => { setSelfxVisible(!state.view.selfx);  syncControls(); });
viewUVBtn   .addEventListener('click', () => { setUVVisible(!state.view.uv);        syncControls(); });
viewBonesBtn.addEventListener('click', () => {
    state.view.bones = !state.view.bones;
    if (state.view.bones) setupBoneNodes(); else clearBoneNodes();
    syncControls();
});

// Modify
modSubLoopBtn.addEventListener('click', () => applyMeshOp('Subdivide Loop',         m => m.subdivideLoop(1)));
modSubCCBtn  .addEventListener('click', () => applyMeshOp('Subdivide Catmull-Clark', m => m.subdivideCatmullClark(1)));
modSubMidBtn .addEventListener('click', () => applyMeshOp('Subdivide Midpoint',     m => m.subdivideMidpoint(1)));
modSmoothLapBtn.addEventListener('click', () => applyMeshOp('Smooth Laplacian', m => m.smoothLaplacian(0.5, 5)));
modSmoothTauBtn.addEventListener('click', () => applyMeshOp('Smooth Taubin',    m => m.smoothTaubin(0.5, -0.53, 10)));
modRemeshBtn .addEventListener('click', () => {
    const len = parseFloat(modRemeshLenIn.value) || 0.05;
    applyMeshOp('Remesh @' + len, m => m.remeshIsotropic(len, 3));
});
modSimplifyRng.addEventListener('input', () => {
    const r = parseFloat(modSimplifyRng.value);
    modSimplifyNum.textContent = (r * 100).toFixed(0) + '%';
});
modSimplifyRng.addEventListener('change', () => {
    const r = parseFloat(modSimplifyRng.value);
    if (r >= 0.999) return;
    applyMeshOp('Simplify ' + (r*100).toFixed(0) + '%', m => m.simplify(r, 0.01));
});
modUnwrapBtn.addEventListener('click', () => applyMeshOp('UV unwrap', m => { m.unwrapUVs(); return m; }));
modResetBtn .addEventListener('click', resetMods);

// LOD
lodBuildBtn.addEventListener('click', buildLODChain);
lodClearBtn.addEventListener('click', clearLOD);
lodRange.addEventListener('input', () => {
    const r = parseFloat(lodRange.value);
    applyLOD(r);
});

// Rig
rigPauseBtn.addEventListener('click', () => { state.paused = !state.paused; syncControls(); });
rigBindBtn .addEventListener('click', () => { state.bindPoseOnly = !state.bindPoseOnly; syncControls(); });
blendRange .addEventListener('input', () => {
    state.rig.blendW = parseFloat(blendRange.value);
    blendNum.textContent = state.rig.blendW.toFixed(2);
});

// Export
expGlbBtn.addEventListener('click', () => exportMesh('glb'));
expObjBtn.addEventListener('click', () => exportMesh('obj'));
expPlyBtn.addEventListener('click', () => exportMesh('ply'));
expStlBtn.addEventListener('click', () => exportMesh('stl'));

// Hide panel
window.addEventListener('keydown', (e) => {
    // Ignore typing in inputs.
    const tag = e.target && e.target.tagName;
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
    if (e.key === 'h' || e.key === 'H') {
        state.panelHidden = !state.panelHidden;
        opsPanel.classList.toggle('hidden', state.panelHidden);
    } else if (e.key === 'o' || e.key === 'O') openFolderDialog();
    else if (e.key === 'f' || e.key === 'F') openFileDialog();
    else if (e.key === ' ') { state.paused = !state.paused; syncControls(); e.preventDefault(); }
});

// Drag-drop
canvas.addEventListener('dragenter', (e) => { e.preventDefault(); dropOverlay.classList.add('show'); });
canvas.addEventListener('dragover',  (e) => { e.preventDefault(); });
canvas.addEventListener('dragleave', (e) => { e.preventDefault(); dropOverlay.classList.remove('show'); });
canvas.addEventListener('drop', (e) => {
    e.preventDefault();
    dropOverlay.classList.remove('show');
    const files = e.dataTransfer && e.dataTransfer.files;
    if (!files || files.length === 0) return;
    const f = files[0];
    const p = (f.path || f.name || '').replace(/\\/g, '/');
    if (!p) { setStatus('Drop has no path', 'warn'); return; }
    if (!LOAD_EXTS.includes(fileExt(p))) { setStatus('Unsupported type: ' + p, 'warn'); return; }
    loadStandalonePath(p);
});

// ---------------------------------------------------------------------------
// Camera input — right=rotate, middle=pan, wheel=zoom
// ---------------------------------------------------------------------------

function updatePointerLock() {
    const want = rightDown || middleDown;
    const locked = document.pointerLockElement === canvas;
    if (want && !locked) canvas.requestPointerLock();
    else if (!want && locked) document.exitPointerLock();
}

canvas.addEventListener('mousedown', (e) => {
    if (e.button === 2)      { rightDown  = true; e.preventDefault(); updatePointerLock(); }
    else if (e.button === 1) { middleDown = true; e.preventDefault(); updatePointerLock(); }
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
canvas.addEventListener('auxclick', (e) => { if (e.button === 1) e.preventDefault(); });
canvas.addEventListener('wheel', (e) => {
    cam.dist = Math.max(0.1, cam.dist * Math.exp(e.deltaY * 0.001));
    e.preventDefault();
});

// ---------------------------------------------------------------------------
// Go
// ---------------------------------------------------------------------------

renderStats();
syncControls();
const initialDir = pickInitialDir();
if (initialDir) {
    state.mode = 'folder';
    // Show the folder contents but don't auto-load — saves startup time on
    // large meshes and lets the user pick which file to inspect.
    setDirectory(initialDir, { autoload: false });
    setStatus('Ready');
} else {
    dirStatus.textContent = 'Open Folder… (O) or Open File… (F).';
    setStatus('Ready');
}
requestAnimationFrame(frame);
