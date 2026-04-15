// =============================================================================
// MeshyAI GLB viewer — loads rigged/animated glbs from D:/moba-game and
// renders them with CPU skinning via the new rigging bindings.
// =============================================================================

const FILES = [
    'D:/moba-game/Meshy_AI_Stone_Sentinel_biped_Character_output.glb',
    'D:/moba-game/Meshy_AI_Stone_Sentinel_biped_Animation_Walking_withSkin.glb',
    'D:/moba-game/Meshy_AI_Stone_Sentinel_biped_Animation_Running_withSkin.glb',
    'D:/moba-game/Meshy_AI_Crimson_Core_Knight_0414102836_texture.glb',
    'D:/moba-game/Meshy_AI_Crimson_Core_Knight_0414114102_generate.glb',
    'D:/moba-game/Meshy_AI_Gilded_Sentinel_0414102845_texture.glb',
    'D:/moba-game/Meshy_AI_Gilded_Sentinel_0414120138_generate.glb',
    'D:/moba-game/Meshy_AI_Golden_Core_Knight_0414102821_texture.glb',
    'D:/moba-game/Meshy_AI_Golden_Gem_Knight_0414102900_texture.glb',
];

const PALETTE = ['#e74c3c', '#3498db', '#2ecc71', '#f39c12', '#9b59b6',
                 '#16a085', '#d35400', '#c0392b', '#8e44ad'];

const canvas  = document.getElementById('canvas');
const scene   = canvas.getContext('scene');
const statusEl = document.getElementById('status');
const infoEl  = document.getElementById('info');

let state = {
    fileIndex: 0,
    loaded: null,       // { path, scene, bindMeshes[], node, skelIdx, skinIdx, animIdx, animations[], skeleton, skin, basePositions }
    nodes: [],          // scene nodes
    autoOrbit: true,
    paused: false,
    wire: false,
    bindPoseOnly: false,
    time: 0,
    cameraDist: 6,
    cameraHeight: 2,
    orbitAngle: 0,
    enableSkinning: true,
    showBones: false,
    boneNodes: [],    // scene nodes for bone markers
};

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

function bboxCenterRadius(mesh) {
    const bb = mesh.computeBBox();
    const cx = (bb.min[0] + bb.max[0]) * 0.5;
    const cy = (bb.min[1] + bb.max[1]) * 0.5;
    const cz = (bb.min[2] + bb.max[2]) * 0.5;
    const dx = bb.max[0] - bb.min[0];
    const dy = bb.max[1] - bb.min[1];
    const dz = bb.max[2] - bb.min[2];
    const r = Math.sqrt(dx*dx + dy*dy + dz*dz) * 0.5;
    return { cx, cy, cz, r, dx, dy, dz };
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

function loadFile(idx) {
    clearNodes();

    const path = FILES[idx];
    const name = path.split('/').pop();
    setStatus('Loading ' + name + ' …');

    let gltf;
    try {
        gltf = Mesh.loadGLTF(path);
    } catch (e) {
        setStatus('FAILED: ' + e.message);
        setInfo('Could not load ' + path);
        return;
    }

    if (!gltf || !gltf.meshes || gltf.meshes.length === 0) {
        setStatus('No meshes in ' + name);
        setInfo('meshes=0 skins=' + (gltf?.skins?.length || 0)
            + ' skeletons=' + (gltf?.skeletons?.length || 0)
            + ' animations=' + (gltf?.animations?.length || 0));
        return;
    }

    // Frame the first (or merged) mesh.
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
    state.cameraDist = Math.max(size * 2.2, 2);
    state.cameraHeight = cy + size * 0.3;
    state.target = [cx, cy, cz];

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

    // Create one scene mesh node per sub-mesh.
    for (let i = 0; i < meshes.length; i++) {
        const color = PALETTE[i % PALETTE.length];
        const node = scene.createMesh({
            data: meshes[i],
            color: color,
            name: 'mesh-' + i,
        });
        state.nodes.push(node);
    }

    state.loaded = {
        path, name,
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
    state.orbitAngle = 0;

    renderInfo();
    setStatus('[' + (idx+1) + '/' + FILES.length + '] ' + name);
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

// NOTE: CPU skinning is disabled by default — bromesh's glTF loader produces
// skinning matrices with ~100x scale on the diagonal for MeshyAI exports
// (root-node scale not composed into inverseBindMatrices). Set
// state.enableSkinning = true to experiment once that's fixed upstream.
function updateAnimation(dtMs) {
    const L = state.loaded;
    if (!L || !L.hasSkin || !L.hasSkel) return;
    if (!state.enableSkinning) return;

    // Advance anim clock
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

    // Apply to each bind mesh (we assume skin applies to mesh 0 primarily, but
    // try on all and silently skip mismatches).
    for (let i = 0; i < L.bindMeshes.length; i++) {
        const bm = L.bindMeshes[i];
        if (bm.mesh.vertexCount !== L.skin.vertexCount) continue;

        // Restore bind positions before skinning (fresh copy — applySkinning
        // mutates in place and would otherwise compound each frame).
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
    const size = state.cameraDist * 0.012;
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

function updateCamera() {
    if (state.autoOrbit) state.orbitAngle += 0.005;
    const target = state.target || [0, 0, 0];
    const dist = state.cameraDist;
    const h = state.cameraHeight;
    const cam = {
        fov: 45,
        position: [
            target[0] + Math.sin(state.orbitAngle) * dist,
            h,
            target[2] + Math.cos(state.orbitAngle) * dist,
        ],
        target: target,
        aspect: canvas.clientWidth / canvas.clientHeight,
    };
    scene.setCamera(cam);
}

let lastT = 0;
function frame(t) {
    if (!lastT) lastT = t;
    const dt = t - lastT;
    lastT = t;

    state.time += dt;
    updateAnimation(dt);
    if (state.showBones) updateBoneNodes();
    updateCamera();

    requestAnimationFrame(frame);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

document.addEventListener('keydown', (e) => {
    const k = e.key;
    if (k === ']' || k === 'ArrowRight') {
        state.fileIndex = (state.fileIndex + 1) % FILES.length;
        loadFile(state.fileIndex);
    } else if (k === '[' || k === 'ArrowLeft') {
        state.fileIndex = (state.fileIndex - 1 + FILES.length) % FILES.length;
        loadFile(state.fileIndex);
    } else if (k === ' ') {
        state.paused = !state.paused;
        renderInfo();
    } else if (k === 'a' || k === 'A') {
        const L = state.loaded;
        if (L && L.hasAnim) {
            L.animIdx = (L.animIdx + 1) % L.animations.length;
            L.animTime = 0;
            renderInfo();
        }
    } else if (k === 'b' || k === 'B') {
        state.bindPoseOnly = !state.bindPoseOnly;
        renderInfo();
    } else if (k === 'r' || k === 'R') {
        state.autoOrbit = !state.autoOrbit;
    } else if (k === 'w' || k === 'W') {
        state.showBones = !state.showBones;
        if (state.showBones) setupBoneNodes();
        else clearBoneNodes();
        renderInfo();
    }
});

// ---------------------------------------------------------------------------
// Go
// ---------------------------------------------------------------------------

loadFile(0);
requestAnimationFrame(frame);
