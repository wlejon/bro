// =============================================================================
// Mesh viewer + workbench. Heavy bromesh ops run in a worker; the main thread
// only handles rendering, skinning, and UI. Stats checks are on-demand to keep
// file loads instant.
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

// ---------------------------------------------------------------------------
// Lighting rig
// ---------------------------------------------------------------------------
//
// Three-point studio setup: a warm key light from the upper-right casts
// shadows, a cool fill from the opposite side softens the shaded side, and
// a top rim separates subject from background. ACES tonemap pins the
// highlights so specular on metallic materials doesn't clip.
//
// Shadows are on by default; the "Shadows" toggle flips the key light's
// castsShadow flag at runtime. A ground plane exists only to catch the
// drop shadow — it's placed below the camera target and receives but
// doesn't cast. All three lights plus the plane are built once and reused.

scene.setToneMap({ mode: 'aces', exposure: 1.0, gamma: 2.2 });
scene.setAmbient({ color: [0.08, 0.08, 0.10] });

const keyLight = scene.createLight({
    type: 'directional',
    direction: [-0.4, -1.0, -0.3],
    color:     [1.00, 0.96, 0.88],
    intensity: 3.5,
    castsShadow: true,
});
scene.createLight({
    type: 'directional',
    direction: [0.6, -0.4, 0.5],
    color:     [0.70, 0.82, 1.00],
    intensity: 1.2,
});
scene.createLight({
    type: 'directional',
    direction: [0.0, 0.8, -0.6],
    color:     [1.00, 1.00, 1.00],
    intensity: 0.8,
});

// Shadow-catcher plane. Placed at y = -1.2 (below typical bbox-normalized
// content) and sized to cover the far shadow cast of a reframed model.
const ground = scene.createMesh({
    mesh: 'plane', halfW: 8, halfD: 8,
    color: [0.25, 0.25, 0.27, 1.0],
    roughness: 0.95, metallic: 0.0,
    castsShadow: false,
});
ground.y = -1.2;

const opsPanel  = document.getElementById('ops-panel');
const dropOverlay = document.getElementById('drop-overlay');

const dirStatus = document.getElementById('dir-status');
const fileListEl = document.getElementById('file-list');

// stats
const $st = {
    meshes:   document.getElementById('st-meshes'),
    verts:    document.getElementById('st-verts'),
    tris:     document.getElementById('st-tris'),
    bbox:     document.getElementById('st-bbox'),
    uvs:      document.getElementById('st-uvs'),
    colors:   document.getElementById('st-colors'),
    manifold: document.getElementById('st-manifold'),
    rowMan:   document.getElementById('st-row-manifold'),
    volume:   document.getElementById('st-volume'),
    selfx:    document.getElementById('st-selfx'),
    rowSelfx: document.getElementById('st-row-selfx'),
    runBtn:   document.getElementById('stats-run'),
};

// view
const viewModeSel  = document.getElementById('view-mode');
const viewHullBtn  = document.getElementById('view-hull');
const viewSelfxBtn = document.getElementById('view-selfx');
const viewUVBtn    = document.getElementById('view-uv');
const viewBonesBtn = document.getElementById('view-bones');
const viewShadowsBtn = document.getElementById('view-shadows');
const viewTextureBtn = document.getElementById('view-texture');
const viewEmissiveRange = document.getElementById('view-emissive');
const viewEmissiveNum   = document.getElementById('view-emissive-num');

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

// uv inset
const uvInset    = document.getElementById('uv-inset');
const uvCanvas   = document.getElementById('uv-canvas');
const uvCtx      = uvCanvas.getContext('2d');

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let state = {
    dir: '',
    files: [],
    fileIndex: -1,
    loaded: null,
    paused: false,
    bindPoseOnly: false,
    panelHidden: false,

    view:    { color: 'original', hull: false, selfx: false, uv: false, bones: false, shadows: true, texture: true, emissive: 1.0 },
    modify:  { dirty: false },
    lod:     { ratio: 1.0, built: false, encoded: null, originalTris: 0 },
    rig:     { active: -1, blend: -1, blendW: 0.5 },
    boneNodes: [],

    busy: false,
};

// ---------------------------------------------------------------------------
// Mesh worker — promise-returning op dispatcher
// ---------------------------------------------------------------------------

const worker = new Worker('mesh-worker.js');
let nextJobId = 1;
const pending = new Map();    // id → { resolve, reject, label }

worker.onmessage = (e) => {
    const { id, ok, result, error } = e.data;
    const job = pending.get(id);
    if (!job) return;
    pending.delete(id);
    if (ok) job.resolve(result);
    else    job.reject(new Error(error || 'worker op failed'));
};

// Serialize a Mesh to plain typed arrays for the worker. Always copies — the
// main thread keeps its mesh intact.
function meshToData(m) {
    const out = {
        positions: new Float32Array(m.positions),
        indices:   new Uint32Array(m.indices),
    };
    if (m.hasNormals) out.normals = new Float32Array(m.normals);
    if (m.hasUVs)     out.uvs     = new Float32Array(m.uvs);
    if (m.hasColors)  out.colors  = new Float32Array(m.colors);
    return out;
}

// Reverse: rebuild a Mesh from worker reply data.
function meshFromData(d) {
    const opts = { positions: d.positions, indices: d.indices };
    if (d.normals) opts.normals = d.normals;
    if (d.uvs)     opts.uvs     = d.uvs;
    if (d.colors)  opts.colors  = d.colors;
    return new Mesh(opts);
}

function transferList(d) {
    const list = [d.positions.buffer, d.indices.buffer];
    if (d.normals) list.push(d.normals.buffer);
    if (d.uvs)     list.push(d.uvs.buffer);
    if (d.colors)  list.push(d.colors.buffer);
    return list;
}

function postOp(op, mesh, params) {
    const id = nextJobId++;
    const data = mesh ? meshToData(mesh) : null;
    return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        const transfers = data ? transferList(data) : [];
        worker.postMessage({ id, op, mesh: data, params: params || {} }, transfers);
    });
}

// Run an op against every item's work mesh in turn, applying `apply(item, result)`
// for each. Manages busy state, status message, and timing.
async function runForEach(label, op, perItemParams, apply) {
    if (!state.loaded) return;
    if (state.busy) { setStatus('busy — wait for current op', 'warn'); return; }
    setBusy(true, label);
    const t0 = performance.now();
    try {
        const items = state.loaded.items;
        for (let i = 0; i < items.length; i++) {
            if (items.length > 1) setStatus(label + ' (' + (i+1) + '/' + items.length + ') …', 'busy');
            const params = (typeof perItemParams === 'function') ? perItemParams(items[i], i) : perItemParams;
            const result = await postOp(op, items[i].work, params);
            apply(items[i], result);
        }
        setStatus(label + ' · ' + (performance.now() - t0).toFixed(0) + ' ms');
    } catch (e) {
        setStatus(label + ' failed: ' + e.message, 'error');
    } finally {
        setBusy(false);
    }
}

function setBusy(on, label) {
    state.busy = on;
    document.body.classList.toggle('busy', on);
    if (on && label) setStatus(label + ' …', 'busy');
    syncControls();
}

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

function fmtVec3(v) { return v ? v.map(x => x.toFixed(2)).join(', ') : '—'; }

function clearNodes() {
    if (!state.loaded) return;
    for (const it of state.loaded.items) {
        if (it.node)      { it.node.destroy();      it.node = null; }
        if (it.hullNode)  { it.hullNode.destroy();  it.hullNode = null; }
        if (it.selfxNode) { it.selfxNode.destroy(); it.selfxNode = null; }
    }
    clearBoneNodes();
    state.loaded = null;
    syncMenuExportEnabled();
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
    let m;
    if      (ext === '.obj') m = Mesh.loadOBJ(filePath);
    else if (ext === '.ply') m = Mesh.loadPLY(filePath);
    else if (ext === '.stl') m = Mesh.loadSTL(filePath);
    else throw new Error('Unsupported extension: ' + ext);
    return { meshes: [m], skins: [], skeletons: [], animations: [], materials: [], images: [], meshMaterial: [] };
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
        name.className = 'nm';
        name.textContent = fileName(state.files[i]);
        item.appendChild(num);
        item.appendChild(name);
        item.addEventListener('click', () => {
            if (state.busy) { setStatus('busy — wait for current op', 'warn'); return; }
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
    const autoload = opts.autoload === true;
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
    state.fileIndex = autoload ? targetIdx : -1;
    renderFileList();
    if (autoload) loadFile(targetIdx);
}

function openFolderDialog() {
    if (typeof showOpenFolderDialog !== 'function') {
        setStatus('Native folder dialog unavailable', 'error'); return;
    }
    const picked = showOpenFolderDialog(state.dir || null);
    if (!picked || picked.length === 0) return;
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

    let lo = [ Infinity,  Infinity,  Infinity];
    let hi = [-Infinity, -Infinity, -Infinity];
    for (const m of meshes) {
        const bb = m.computeBBox();
        for (let i = 0; i < 3; i++) { if (bb.min[i] < lo[i]) lo[i] = bb.min[i]; if (bb.max[i] > hi[i]) hi[i] = bb.max[i]; }
    }
    const center = [(lo[0]+hi[0])*0.5, (lo[1]+hi[1])*0.5, (lo[2]+hi[2])*0.5];
    const size = Math.max(hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]) || 1;
    Camera.orbitReframe(cam, center, Math.max(size * 2.2, 2));

    const materials = gltf.materials || [];
    const images    = gltf.images    || [];
    const meshMat   = gltf.meshMaterial || [];

    // glTF delivers textures as image indices (-1 = absent). Resolve to the
    // { width, height, data } shape createMesh() wants, or null. Shared
    // between baseColor / normal / metallicRoughness / occlusion.
    const resolveImg = (idx) => {
        if (idx == null || idx < 0 || idx >= images.length) return null;
        const im = images[idx];
        return (im && im.data && im.width > 0 && im.height > 0)
             ? { width: im.width, height: im.height, data: im.data }
             : null;
    };

    const items = [];
    for (let i = 0; i < meshes.length; i++) {
        const bind = meshes[i];
        if (!bind.hasNormals) bind.computeNormals();
        const work = bind.clone();

        const opts = { data: work, name: 'mesh-' + i, castsShadow: true, receivesShadow: true };
        const matIdx = meshMat[i] ?? -1;
        const mat = (matIdx >= 0 && matIdx < materials.length) ? materials[matIdx] : null;

        if (mat) {
            const baseTex = resolveImg(mat.baseColorTexture);
            if (baseTex) {
                opts.texture = baseTex;
                opts.color   = mat.baseColorFactor || [1, 1, 1, 1];
            } else {
                opts.color = mat.baseColorFactor || PALETTE[i % PALETTE.length];
            }
            // glTF spec: scalar factors multiply with sampled texture channels.
            // When a MR texture is present, factors default to 1.0 (pass-through);
            // when absent, the factors drive the PBR params directly. Our shader
            // does the same multiply, so just forward whatever the file carries.
            opts.metallic  = (mat.metallicFactor  != null) ? mat.metallicFactor  : 0.0;
            opts.roughness = (mat.roughnessFactor != null) ? mat.roughnessFactor : 1.0;

            const nTex  = resolveImg(mat.normalTexture);
            const mrTex = resolveImg(mat.metallicRoughnessTexture);
            const aoTex = resolveImg(mat.occlusionTexture);
            const emTex = resolveImg(mat.emissiveTexture);
            if (nTex)  opts.normalTexture            = nTex;
            if (mrTex) opts.metallicRoughnessTexture = mrTex;
            if (aoTex) opts.occlusionTexture         = aoTex;

            // Emissive: glTF spec is `emission = factor * sample(texture)`.
            // With a texture, forward scalar=1 + factor-as-tint so the map
            // gates where the mesh emits. Without a texture, fall back to
            // flat emission only when the factor is nonzero — otherwise
            // files that declare a zero factor stay dark, and files that
            // declare a `[1,1,1]` factor alongside a *missing* texture
            // don't glow across the entire mesh.
            const ef = mat.emissiveFactor || [0, 0, 0];
            if (emTex) {
                opts.emissiveTexture = emTex;
                opts.emissive        = 1.0;
                opts.emissiveColor   = ef;
            } else {
                const emStrength = Math.max(ef[0], ef[1], ef[2]);
                if (emStrength > 0.0) {
                    opts.emissive      = emStrength;
                    opts.emissiveColor = [ef[0] / emStrength, ef[1] / emStrength, ef[2] / emStrength];
                }
            }
        } else {
            opts.color = PALETTE[i % PALETTE.length];
        }

        // If the user has Texture toggled off, create without a baseColor map;
        // we still cache the image on the item so the toggle can re-apply it.
        const cachedTex = opts.texture || null;
        if (!state.view.texture) delete opts.texture;

        items.push({
            bind, work,
            basePositions: new Float32Array(bind.positions),
            baseNormals:   bind.hasNormals ? new Float32Array(bind.normals) : null,
            baseColors:    bind.hasColors  ? new Float32Array(bind.colors)  : null,
            node: scene.createMesh(opts),
            loadedEmissive: opts.emissive || 0,
            baseTexture: cachedTex,
            hullNode: null,
            selfxNode: null,
        });
    }

    // Seed the emissive slider with the file's own value (max across meshes)
    // so the UI shows what was loaded before any user tuning. Meshy AI ships
    // characters with factor [1,1,1] + an emissive tint map, which reads as
    // "full self-illumination" — drag toward 0 for non-emissive subjects.
    const fileEmissive = items.reduce((m, it) => Math.max(m, it.loadedEmissive), 0);
    viewEmissiveRange.value = fileEmissive.toFixed(2);
    viewEmissiveNum.textContent = fileEmissive.toFixed(2);
    state.view.emissive = fileEmissive;

    state.loaded = {
        path: filePath, name, gltf,
        items,
        hasSkin, hasSkel, hasAnim,
        skeleton: hasSkel ? gltf.skeletons[0] : null,
        skin:     hasSkin ? gltf.skins[0]     : null,
        animations: gltf.animations || [],
    };

    state.modify.dirty = false;
    state.lod = { ratio: 1.0, built: false, encoded: null, originalTris: 0 };

    if (hasAnim) state.rig.active = 0;
    state.rig.blend = -1;
    state.rig.blendW = 0.5;

    rigSection.style.display = (hasAnim || hasSkel) ? '' : 'none';
    renderRigUI();
    renderStats();
    syncControls();
    syncMenuExportEnabled();
    renderFileList();

    setStatus(name + ' · ' + items.length + ' mesh' + (items.length === 1 ? '' : 'es'));
}

function resetUIState() {
    state.view = { color: 'original', hull: false, selfx: false, uv: false, bones: false,
                   shadows: state.view.shadows, texture: true, emissive: state.view.emissive };
    state.lod = { ratio: 1.0, built: false, encoded: null, originalTris: 0 };
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
        $st.meshes.textContent = '0';
        $st.verts.textContent = '0';
        $st.tris.textContent = '0';
        $st.bbox.textContent = '—';
        $st.uvs.textContent = '—';
        $st.colors.textContent = '—';
        $st.manifold.textContent = '—';
        $st.volume.textContent = '—';
        $st.selfx.textContent = '—';
        $st.rowMan.classList.remove('ok', 'bad', 'muted');
        $st.rowSelfx.classList.remove('ok', 'bad', 'muted');
        $st.runBtn.disabled = true;
        return;
    }

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
    $st.bbox.textContent  = fmtVec3([hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]]);
    $st.uvs.textContent    = hasUVs   ? 'yes' : 'no';
    $st.colors.textContent = hasColors ? 'yes' : 'no';

    // Heavy checks aren't run on load (single big mesh would freeze the
    // worker). They reset to "—" and the user clicks "Run checks".
    $st.manifold.textContent = '—';
    $st.volume.textContent = '—';
    $st.selfx.textContent = '—';
    $st.rowMan.classList.remove('ok', 'bad', 'muted');
    $st.rowSelfx.classList.remove('ok', 'bad', 'muted');

    $st.runBtn.disabled = (L.items.length !== 1);
    if (L.items.length !== 1) {
        $st.manifold.textContent = '(' + L.items.length + ' meshes)';
        $st.volume.textContent = '—';
        $st.selfx.textContent = '—';
        $st.rowMan.classList.add('muted');
        $st.rowSelfx.classList.add('muted');
    }
}

async function runStatsChecks() {
    const L = state.loaded;
    if (!L || L.items.length !== 1 || state.busy) return;
    setBusy(true, 'Running checks');
    try {
        const w = L.items[0].work;
        // Manifold + volume in one trip.
        $st.manifold.textContent = '…';
        $st.volume.textContent = '…';
        const r1 = await postOp('isManifold', w, {});
        $st.manifold.textContent = r1.manifold ? 'yes' : 'no';
        $st.rowMan.classList.toggle('ok',  r1.manifold);
        $st.rowMan.classList.toggle('bad', !r1.manifold);
        $st.volume.textContent = (r1.volume !== null) ? fmtNum(r1.volume) : 'n/a';

        // Self-int check — can be very slow on dense meshes.
        $st.selfx.textContent = 'computing …';
        const r2 = await postOp('selfInt', w, {});
        const cnt = r2.pairs ? r2.pairs.length : 0;
        $st.selfx.textContent = cnt === 0 ? 'none' : (cnt + ' pair' + (cnt === 1 ? '' : 's'));
        $st.rowSelfx.classList.toggle('ok',  cnt === 0);
        $st.rowSelfx.classList.toggle('bad', cnt > 0);
        setStatus('Checks done');
    } catch (e) {
        setStatus('Checks failed: ' + e.message, 'error');
    } finally {
        setBusy(false);
    }
}

// ---------------------------------------------------------------------------
// View — color modes (vertex colors)
// ---------------------------------------------------------------------------

async function applyColorMode(mode) {
    state.view.color = mode;
    const L = state.loaded;
    if (!L) return;

    if (mode === 'original') {
        for (const it of L.items) {
            if (it.baseColors) it.work.colors = new Float32Array(it.baseColors);
            else { try { it.work.colors = new Float32Array(0); } catch (e) {} }
            it.node.updateMesh(it.work);
        }
        return;
    }

    if (mode === 'normals') {
        // JS-only and cheap — no worker needed.
        for (const it of L.items) {
            const w = it.work;
            if (!w.hasNormals) w.computeNormals();
            const n = w.normals;
            const nv = w.vertexCount;
            const c = new Float32Array(nv * 4);
            for (let i = 0; i < nv; i++) {
                c[i*4 + 0] = n[i*3 + 0] * 0.5 + 0.5;
                c[i*4 + 1] = n[i*3 + 1] * 0.5 + 0.5;
                c[i*4 + 2] = n[i*3 + 2] * 0.5 + 0.5;
                c[i*4 + 3] = 1.0;
            }
            w.colors = c;
            it.node.updateMesh(w);
        }
        return;
    }

    // AO / curvature / thickness — worker.
    const opMap = { ao: 'bakeAO', curvature: 'bakeCurv', thickness: 'bakeThick' };
    const labelMap = { ao: 'Baking AO', curvature: 'Baking curvature', thickness: 'Baking thickness' };
    await runForEach(labelMap[mode], opMap[mode], {}, (it, result) => {
        // Worker returned a Mesh; we want its colors+normals applied to our work mesh.
        if (result.mesh.colors)  it.work.colors  = result.mesh.colors;
        if (result.mesh.normals) it.work.normals = result.mesh.normals;
        it.node.updateMesh(it.work);
    });
}

// ---------------------------------------------------------------------------
// View — convex hull overlay
// ---------------------------------------------------------------------------

async function setHullVisible(on) {
    state.view.hull = on;
    const L = state.loaded;
    if (!L) { syncControls(); return; }
    if (!on) {
        for (const it of L.items) if (it.hullNode) it.hullNode.visible = false;
        syncControls();
        return;
    }
    // Build hulls (lazy).
    const needBuild = L.items.some(it => !it.hullNode);
    if (needBuild) {
        await runForEach('Convex hull', 'convexHull', {}, (it, result) => {
            const hull = meshFromData(result.mesh);
            if (it.hullNode) it.hullNode.destroy();
            it.hullNode = scene.createMesh({
                data: hull,
                color: [1.0, 0.85, 0.2, 1.0],
                unlit: true,
                castsShadow: false,
                name: 'hull-' + it.node.name,
            });
            it.hullNode.scaleX = it.hullNode.scaleY = it.hullNode.scaleZ = 1.01;
        });
    } else {
        for (const it of L.items) if (it.hullNode) it.hullNode.visible = true;
    }
    syncControls();
}

// ---------------------------------------------------------------------------
// View — self-intersection highlight
// ---------------------------------------------------------------------------

async function setSelfxVisible(on) {
    state.view.selfx = on;
    const L = state.loaded;
    if (!L) { syncControls(); return; }
    for (const it of L.items) {
        if (it.selfxNode) { it.selfxNode.destroy(); it.selfxNode = null; }
    }
    if (!on) { syncControls(); return; }

    await runForEach('Self-intersect', 'selfInt', {}, (it, result) => {
        const pairs = result.pairs || [];
        if (pairs.length === 0) return;
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
            unlit: true,
            castsShadow: false,
            depthBias: [-1, -1000],
            name: 'selfx-' + it.node.name,
        });
    });
    syncControls();
}

// ---------------------------------------------------------------------------
// View — UV inset
// ---------------------------------------------------------------------------

function drawUVInset() {
    const W = uvCanvas.width, H = uvCanvas.height;
    uvCtx.fillStyle = '#050505';
    uvCtx.fillRect(0, 0, W, H);
    if (!state.view.uv || !state.loaded) return;
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
            const ax = uv[a*2] * W, ay = (1 - uv[a*2 + 1]) * H;
            const bx = uv[b*2] * W, by = (1 - uv[b*2 + 1]) * H;
            const cx = uv[c*2] * W, cy = (1 - uv[c*2 + 1]) * H;
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
    syncControls();
}

// ---------------------------------------------------------------------------
// Modify ops
// ---------------------------------------------------------------------------

async function runModify(label, op, params) {
    const L = state.loaded;
    if (!L) return;
    await runForEach(label, op, params, (it, result) => {
        it.work = meshFromData(result.mesh);
        it.node.updateMesh(it.work);
        if (it.hullNode)  { it.hullNode.destroy();  it.hullNode = null; }
        if (it.selfxNode) { it.selfxNode.destroy(); it.selfxNode = null; }
    });
    state.modify.dirty = true;
    state.lod = { ratio: 1.0, built: false, encoded: null, originalTris: 0 };
    lodRange.value = 1.0;
    lodRange.disabled = true;
    lodNum.textContent = '—';

    // Re-apply view layers that depend on the new geometry.
    if (state.view.color !== 'original' && state.view.color !== 'normals') {
        await applyColorMode(state.view.color);
    } else {
        await applyColorMode(state.view.color);
    }
    if (state.view.hull)  await setHullVisible(true);
    if (state.view.selfx) await setSelfxVisible(true);
    if (state.view.uv)    drawUVInset();

    renderStats();
    syncControls();
}

function resetMods() {
    const L = state.loaded;
    if (!L) return;
    for (const it of L.items) {
        it.work = it.bind.clone();
        it.node.updateMesh(it.work);
        if (it.hullNode)  { it.hullNode.destroy();  it.hullNode = null; }
        if (it.selfxNode) { it.selfxNode.destroy(); it.selfxNode = null; }
    }
    state.modify.dirty = false;
    state.lod = { ratio: 1.0, built: false, encoded: null, originalTris: 0 };
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
// LOD
// ---------------------------------------------------------------------------

async function buildLODChain() {
    const L = state.loaded;
    if (!L) return;
    // Only single-mesh files get the LOD slider — multi-mesh would need
    // synchronized chains and the UI surface isn't worth it yet.
    if (L.items.length !== 1) {
        setStatus('LOD chain: single-mesh files only', 'warn');
        return;
    }
    setBusy(true, 'Building LOD chain');
    try {
        const it = L.items[0];
        const result = await postOp('lodBuild', it.work, {});
        state.lod.encoded = result.encoded;
        state.lod.built = true;
        state.lod.ratio = 1.0;
        state.lod.originalTris = it.work.triangleCount;
        lodRange.value = 1.0;
        lodRange.disabled = false;
        lodNum.textContent = '100%';
        setStatus('LOD chain built');
    } catch (e) {
        setStatus('LOD build failed: ' + e.message, 'error');
    } finally {
        setBusy(false);
    }
}

let lodPending = null;
async function applyLOD(ratio) {
    const L = state.loaded;
    if (!L || !state.lod.built || !state.lod.encoded) return;
    state.lod.ratio = ratio;
    lodNum.textContent = (ratio * 100).toFixed(0) + '%';
    // Coalesce slider drag — only run latest request.
    if (lodPending) { lodPending = ratio; return; }
    lodPending = ratio;
    while (lodPending !== null) {
        const r = lodPending;
        try {
            const result = await postOp('lodAt', null, { encoded: state.lod.encoded, ratio: r });
            const lod = meshFromData(result.mesh);
            L.items[0].node.updateMesh(lod);
            setStatus('LOD ' + (r*100).toFixed(0) + '% · ' + fmtNum(lod.triangleCount) + ' tris');
        } catch (e) {
            setStatus('LOD failed: ' + e.message, 'error');
            break;
        }
        // If another value was set during the await, loop and apply it too.
        if (lodPending === r) lodPending = null;
    }
}

function clearLOD() {
    const L = state.loaded;
    if (!L) return;
    state.lod = { ratio: 1.0, built: false, encoded: null, originalTris: 0 };
    lodRange.value = 1.0;
    lodRange.disabled = true;
    lodNum.textContent = '—';
    for (const it of L.items) it.node.updateMesh(it.work);
    syncControls();
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
            unlit: true,
            castsShadow: false,
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
    if (state.modify.dirty || state.lod.built) return;

    if (!state.paused && !state.bindPoseOnly) animTime += dtMs * 0.001;

    const pose = currentPose(L);
    const mats = pose.computeWorldMatrices(L.skeleton);

    for (let i = 0; i < L.items.length; i++) {
        const it = L.items[i];
        if (it.work.vertexCount !== L.skin.vertexCount) continue;
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
    blendRow.style.display = state.rig.blend >= 0 ? '' : 'none';
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

function exportMesh(format) {
    const L = state.loaded;
    if (!L) { setStatus('Nothing loaded', 'warn'); return; }
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
            const canSkin = !state.modify.dirty && !state.lod.built && L.hasSkel && L.hasSkin;
            if (canSkin) {
                ok = m.saveGLTF(out, { skin: L.skin, skeleton: L.skeleton, animations: L.animations });
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
    viewShadowsBtn.classList.toggle('toggled', state.view.shadows);
    viewTextureBtn.classList.toggle('toggled', state.view.texture);

    modResetBtn.disabled = state.busy || (!state.modify.dirty && !state.lod.built);
    lodBuildBtn.disabled = state.busy || state.lod.built || !state.loaded || (state.loaded && state.loaded.items.length !== 1);
    lodClearBtn.disabled = !state.lod.built;
    $st.runBtn.disabled = state.busy || !state.loaded || (state.loaded && state.loaded.items.length !== 1);
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

function setupMenu() {
    if (typeof bro === 'undefined' || !bro.menu) return;
    bro.menu.set([
        { id: 'file', label: 'File', items: [
            { id: 'file.openFolder', label: 'Open Folder...', accel: 'Ctrl+O' },
            { id: 'file.openFile',   label: 'Open File...',   accel: 'Ctrl+F' },
            { separator: true },
            { id: 'file.exportGlb', label: 'Save As GLB...', enabled: false },
            { id: 'file.exportObj', label: 'Save As OBJ...', enabled: false },
            { id: 'file.exportPly', label: 'Save As PLY...', enabled: false },
            { id: 'file.exportStl', label: 'Save As STL...', enabled: false },
            { separator: true },
            { id: '__system.quit', label: 'Quit', accel: 'Ctrl+Q' },
        ]},
        { id: 'view', label: 'View', items: [
            { id: 'view.togglePanel', label: 'Hide Ops Panel', accel: 'H' },
            { separator: true },
            { id: '__system.preferences', label: 'Preferences...' },
        ]},
    ]);
    bro.menu.on('file.openFolder', openFolderDialog);
    bro.menu.on('file.openFile',   openFileDialog);
    bro.menu.on('file.exportGlb',  () => exportMesh('glb'));
    bro.menu.on('file.exportObj',  () => exportMesh('obj'));
    bro.menu.on('file.exportPly',  () => exportMesh('ply'));
    bro.menu.on('file.exportStl',  () => exportMesh('stl'));
    bro.menu.on('view.togglePanel', () => togglePanel());
}

function togglePanel() {
    state.panelHidden = !state.panelHidden;
    opsPanel.classList.toggle('hidden', state.panelHidden);
    if (bro && bro.menu && bro.menu.updateItem) {
        bro.menu.updateItem('view.togglePanel', { label: state.panelHidden ? 'Show Ops Panel' : 'Hide Ops Panel' });
    }
}

function syncMenuExportEnabled() {
    if (typeof bro === 'undefined' || !bro.menu || !bro.menu.updateItem) return;
    const have = !!state.loaded;
    bro.menu.updateItem('file.exportGlb', { enabled: have });
    bro.menu.updateItem('file.exportObj', { enabled: have });
    bro.menu.updateItem('file.exportPly', { enabled: have });
    bro.menu.updateItem('file.exportStl', { enabled: have });
}

// ---------------------------------------------------------------------------
// Camera + main loop
// ---------------------------------------------------------------------------

const cam = Camera.createOrbit({ target: [0, 0, 0], dist: 6, fov: 45 });
let rightDown  = false;
let middleDown = false;

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
// UI wiring — buttons and inputs
// ---------------------------------------------------------------------------

// View
viewModeSel.addEventListener('change', () => applyColorMode(viewModeSel.value));
viewHullBtn .addEventListener('click', () => setHullVisible(!state.view.hull));
viewSelfxBtn.addEventListener('click', () => setSelfxVisible(!state.view.selfx));
viewUVBtn   .addEventListener('click', () => setUVVisible(!state.view.uv));
viewBonesBtn.addEventListener('click', () => {
    state.view.bones = !state.view.bones;
    if (state.view.bones) setupBoneNodes(); else clearBoneNodes();
    syncControls();
});
viewShadowsBtn.addEventListener('click', () => {
    state.view.shadows = !state.view.shadows;
    keyLight.castsShadow = state.view.shadows;
    syncControls();
});
viewTextureBtn.addEventListener('click', () => {
    state.view.texture = !state.view.texture;
    if (state.loaded) {
        for (const it of state.loaded.items) {
            it.node.setBaseColorTexture(state.view.texture ? it.baseTexture : null);
        }
    }
    syncControls();
});
viewEmissiveRange.addEventListener('input', () => {
    const v = parseFloat(viewEmissiveRange.value);
    state.view.emissive = v;
    viewEmissiveNum.textContent = v.toFixed(2);
    if (state.loaded) {
        for (const it of state.loaded.items) it.node.emissive = v;
    }
});

// Stats
$st.runBtn.addEventListener('click', runStatsChecks);

// Modify
modSubLoopBtn.addEventListener('click', () => runModify('Subdivide Loop',         'subdivideLoop', { iters: 1 }));
modSubCCBtn  .addEventListener('click', () => runModify('Subdivide Catmull-Clark', 'subdivideCC',  { iters: 1 }));
modSubMidBtn .addEventListener('click', () => runModify('Subdivide Midpoint',     'subdivideMid',  { iters: 1 }));
modSmoothLapBtn.addEventListener('click', () => runModify('Smooth Laplacian', 'smoothLap', { lambda: 0.5, iters: 5 }));
modSmoothTauBtn.addEventListener('click', () => runModify('Smooth Taubin',    'smoothTau', { lambda: 0.5, mu: -0.53, iters: 10 }));
modRemeshBtn .addEventListener('click', () => {
    const len = parseFloat(modRemeshLenIn.value) || 0.05;
    runModify('Remesh @' + len, 'remesh', { edgeLen: len, iters: 3 });
});
modSimplifyRng.addEventListener('input', () => {
    const r = parseFloat(modSimplifyRng.value);
    modSimplifyNum.textContent = (r * 100).toFixed(0) + '%';
});
modSimplifyRng.addEventListener('change', () => {
    const r = parseFloat(modSimplifyRng.value);
    if (r >= 0.999) return;
    runModify('Simplify ' + (r*100).toFixed(0) + '%', 'simplify', { ratio: r, error: 0.01 });
});
modUnwrapBtn.addEventListener('click', () => runModify('UV unwrap', 'unwrap', {}));
modResetBtn .addEventListener('click', resetMods);

// LOD
lodBuildBtn.addEventListener('click', buildLODChain);
lodClearBtn.addEventListener('click', clearLOD);
lodRange.addEventListener('input', () => applyLOD(parseFloat(lodRange.value)));

// Rig
rigPauseBtn.addEventListener('click', () => { state.paused = !state.paused; syncControls(); });
rigBindBtn .addEventListener('click', () => { state.bindPoseOnly = !state.bindPoseOnly; syncControls(); });
blendRange .addEventListener('input', () => {
    state.rig.blendW = parseFloat(blendRange.value);
    blendNum.textContent = state.rig.blendW.toFixed(2);
});

// Keyboard
window.addEventListener('keydown', (e) => {
    const tag = e.target && e.target.tagName;
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
    if (e.key === 'h' || e.key === 'H') togglePanel();
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

// Camera input
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

setupMenu();
renderStats();
syncControls();

const initialDir = pickInitialDir();
if (initialDir) {
    setDirectory(initialDir, { autoload: false });
    setStatus('Ready');
} else {
    dirStatus.textContent = 'File → Open Folder... or Open File...';
    setStatus('Ready');
}

requestAnimationFrame(frame);
