// Test scene.createGaussianSplat — exercises src/scene/gaussian_splat_node.cpp
// and the createGaussianSplat/savePly/splatCount bindings in
// src/js/scene_bindings.cpp. Builds an in-memory SoA cloud (the same shape
// bro.triposplat.generate returns) rather than depending on a real .ply asset,
// then round-trips it through savePly/path-load.

const os = require('os');
const path = require('path');
const fs = require('fs');

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping gaussian splat test');
} else if (typeof scene.createGaussianSplat !== 'function') {
    console.log('no createGaussianSplat; skipping');
} else {
    scene.setCamera({
        fov: 60, near: 0.1, far: 100,
        position: [0, 0, 5], target: [0, 0, 0], up: [0, 1, 0],
    });

    // =====================================================================
    // Build a small synthetic SoA cloud: 5 splats, degree-0 SH (flat color).
    // =====================================================================
    const N = 5;
    const positions = new Float32Array([
        0, 0, 0,
        1, 0, 0,
        -1, 0, 0,
        0, 1, 0,
        0, -1, 0,
    ]);
    const scales = new Float32Array(N * 3).fill(0.1);
    const rotations = new Float32Array(N * 4);
    for (let i = 0; i < N; i++) rotations[i * 4 + 3] = 1; // identity quat xyzw
    const opacities = new Float32Array([1, 0.8, 0.6, 0.4, 0.2]);

    // Degree-0 SH: stride 3 (RGB DC term only). rgb = clamp(0, C0*dc + 0.5).
    // Pick dc so each splat comes out close to a distinct primary color.
    const C0 = 0.28209479177387814;
    const toDc = (v) => (v - 0.5) / C0;
    const sh = new Float32Array([
        toDc(1), toDc(1), toDc(1),   // white
        toDc(1), toDc(0), toDc(0),   // red
        toDc(0), toDc(1), toDc(0),   // green
        toDc(0), toDc(0), toDc(1),   // blue
        toDc(0.5), toDc(0.5), toDc(0.5), // gray
    ]);

    const cloud = { positions, scales, rotations, opacities, sh, shDegree: 0 };

    const node = scene.createGaussianSplat({
        cloud, name: 'splats', x: 1, y: 2, z: 3, scale: 2,
    });

    assert(node !== null && node !== undefined, 'createGaussianSplat returns node');
    assert(node.type === 'gaussianSplat', 'node type is gaussianSplat');
    assert(node.splatCount === N, 'splatCount reflects cloud size, got ' + node.splatCount);
    assert(node.name === 'splats', 'name reflects opts.name');

    const pos = node.position;
    assert(Array.isArray(pos), 'position is array');
    assert(Math.abs(pos[0] - 1) < 0.01, 'position.x = 1');
    assert(Math.abs(pos[1] - 2) < 0.01, 'position.y = 2');
    assert(Math.abs(pos[2] - 3) < 0.01, 'position.z = 3');

    // Move it back to the origin so it's actually in the camera frustum, then
    // force a real frame — this exercises depth sort + SH evaluation + the
    // EWA splat shader end to end, not just bookkeeping.
    node.position = [0, 0, 0];
    flush();
    flush();

    // Non-splat nodes report splatCount 0 rather than throwing.
    const box = scene.createMesh({ mesh: 'box' });
    assert(box.splatCount === 0, 'non-splat node splatCount is 0');

    // =====================================================================
    // savePly write-back + path reload round-trip
    // =====================================================================
    assert(typeof node.savePly === 'function', 'savePly is a function');
    const plyPath = path.join(os.tmpdir(), 'bro_test_splat_' + Date.now() + '.ply');
    const saved = node.savePly(plyPath);
    assert(saved === true, 'savePly returns true');
    assert(fs.existsSync(plyPath), 'savePly writes a file');
    const stat = fs.statSync(plyPath);
    assert(stat.size > 0, 'saved ply has content');

    const reloaded = scene.createGaussianSplat({ path: plyPath, name: 'reloaded' });
    assert(reloaded !== null, 'createGaussianSplat({path}) returns node');
    assert(reloaded.type === 'gaussianSplat', 'reloaded node type is gaussianSplat');
    assert(reloaded.splatCount === N, 'reloaded splatCount matches original, got ' + reloaded.splatCount);
    flush();

    // savePly on a non-splat node throws.
    let threw = false;
    try { box.savePly(plyPath); } catch (e) { threw = true; }
    assert(threw, 'savePly throws on a non-splat node');

    // savePly on an empty splat cloud throws.
    const empty = scene.createGaussianSplat({ name: 'empty' });
    assert(empty.splatCount === 0, 'splat node with no cloud has splatCount 0');
    let emptyThrew = false;
    try { empty.savePly(path.join(os.tmpdir(), 'bro_test_splat_empty.ply')); } catch (e) { emptyThrew = true; }
    assert(emptyThrew, 'savePly throws on an empty splat cloud');

    // =====================================================================
    // Lifecycle
    // =====================================================================
    node.destroy();
    assert(node.splatCount === 0, 'splatCount is 0 after destroy');
    assert(node.type === undefined, 'type is undefined after destroy');

    reloaded.destroy();
    box.destroy();
    empty.destroy();

    fs.unlinkSync(plyPath);
}

document.body.removeChild(canvas);
