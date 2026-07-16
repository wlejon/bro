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

    // =====================================================================
    // Node transform: position / rotation / uniform scale apply to the
    // cloud (splat centers are node-local; the pipeline applies the node's
    // world matrix). Analytic screen positions: camera at (0,0,5) looking
    // at the origin, fov 60, 128 px -> world (x,0,0) lands at
    // px = 64 + (x / 5) * (64 / tan(30 deg)) = 64 + 22.17 * x.
    // =====================================================================
    const diffCount = (a, b, tol) => {
        if (a.width !== b.width || a.height !== b.height) return a.width * a.height;
        let n = 0;
        for (let i = 0; i < a.data.length; i += 4) {
            if (Math.abs(a.data[i]     - b.data[i])     > tol ||
                Math.abs(a.data[i + 1] - b.data[i + 1]) > tol ||
                Math.abs(a.data[i + 2] - b.data[i + 2]) > tol ||
                Math.abs(a.data[i + 3] - b.data[i + 3]) > tol) n++;
        }
        return n;
    };
    const patchMaxAlpha = (img, cx, cy, r) => {
        let m = 0;
        for (let y = cy - r; y <= cy + r; y++) {
            for (let x = cx - r; x <= cx + r; x++) {
                if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
                m = Math.max(m, img.data[(y * img.width + x) * 4 + 3]);
            }
        }
        return m;
    };
    const oneSplat = (x, y, z) => {
        const positions = new Float32Array([x, y, z]);
        const scales = new Float32Array(3).fill(0.1);
        const rotations = new Float32Array([0, 0, 0, 1]);
        const opacities = new Float32Array([1]);
        const sh = new Float32Array(3).fill((1 - 0.5) / C0); // white
        return { positions, scales, rotations, opacities, sh, shDegree: 0 };
    };

    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

    // Golden: a world-space cloud at (2,0,0) with an identity node transform
    // (the pre-fix common case, e.g. PLY-loaded clouds).
    const baked = scene.createGaussianSplat({ cloud: oneSplat(2, 0, 0) });
    const imgBaked = scene.captureFrame();
    assert(patchMaxAlpha(imgBaked, 108, 64, 5) > 128,
        'baked world-space splat renders at the analytic position (108,64)');
    baked.destroy();

    // Same cloud at the local origin, moved by the NODE transform: must land
    // in the same place and vacate the origin.
    const moved = scene.createGaussianSplat({ cloud: oneSplat(0, 0, 0) });
    const imgAtOrigin = scene.captureFrame();
    assert(patchMaxAlpha(imgAtOrigin, 64, 64, 5) > 128, 'splat visible at origin before move');
    assert(patchMaxAlpha(imgAtOrigin, 108, 64, 5) === 0, 'target region empty before move');

    moved.position = [2, 0, 0];
    const imgMoved = scene.captureFrame();
    assert(patchMaxAlpha(imgMoved, 108, 64, 5) > 128,
        'node position moves the splat to the analytic position (108,64)');
    assert(patchMaxAlpha(imgMoved, 64, 64, 5) === 0, 'splat vacated the origin');
    const ndEquiv = diffCount(imgBaked, imgMoved, 2);
    assert(ndEquiv === 0,
        `node-transformed splat matches the world-space golden (diff pixels ${ndEquiv})`);
    moved.destroy();

    // Rotation: local (1.5,0,0) spun 180 deg about Z lands at world (-1.5,0,0)
    // -> screen x mirrors 97 -> 31 (both on the y=64 row, so no dependence on
    // capture row order).
    const rot = scene.createGaussianSplat({ cloud: oneSplat(1.5, 0, 0) });
    const imgPreRot = scene.captureFrame();
    assert(patchMaxAlpha(imgPreRot, 97, 64, 5) > 128, 'splat right of center before rotation');
    rot.rotation = Math.PI;
    const imgRot = scene.captureFrame();
    assert(patchMaxAlpha(imgRot, 31, 64, 5) > 128,
        'node rotation carries the splat to the mirrored position (31,64)');
    assert(patchMaxAlpha(imgRot, 97, 64, 5) === 0, 'splat vacated the pre-rotation position');
    rot.destroy();

    // Uniform scale: local (1,0,0) at node scale 2 lands at world (2,0,0).
    const scaled = scene.createGaussianSplat({ cloud: oneSplat(1, 0, 0), scale: 2 });
    const imgScaled = scene.captureFrame();
    assert(patchMaxAlpha(imgScaled, 108, 64, 5) > 128,
        'uniform node scale scales splat positions (1 * 2 -> 108,64)');
    assert(patchMaxAlpha(imgScaled, 86, 64, 3) === 0,
        'nothing left at the unscaled position (86,64)');
    scaled.destroy();
    flush();
}

document.body.removeChild(canvas);
