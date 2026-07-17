// 3D color-grading LUT (scene.setColorLUT) — strip image -> 3D texture,
// applied after tonemapping in the tonemap pass. A neutral LUT must be an
// identity (within trilinear + 8-bit quantization tolerance); a red-boosting
// LUT must shift a gray patch measurably red while leaving green/blue alone.
// LUT strips are generated on the fly with bro.image.encodePngFile.

const fs = require('fs');
const tmpDir = 'D:/projects/bro/tests/scene/tmp_lut';
fs.mkdirSync(tmpDir, { recursive: true });

const SIZE = 16;

// Build a strip: SIZE tiles of SIZE x SIZE. Tile index = blue, tile x = red,
// tile y = green (top-down). transform(r,g,b) -> [r,g,b] in [0,1].
function writeLutStrip(path, transform) {
    const w = SIZE * SIZE, h = SIZE;
    const px = new Uint8Array(w * h * 4);
    for (let b = 0; b < SIZE; b++) {
        for (let g = 0; g < SIZE; g++) {
            for (let r = 0; r < SIZE; r++) {
                const [rr, gg, bb] = transform(
                    r / (SIZE - 1), g / (SIZE - 1), b / (SIZE - 1));
                const i = (g * w + b * SIZE + r) * 4;
                px[i]     = Math.round(Math.min(1, Math.max(0, rr)) * 255);
                px[i + 1] = Math.round(Math.min(1, Math.max(0, gg)) * 255);
                px[i + 2] = Math.round(Math.min(1, Math.max(0, bb)) * 255);
                px[i + 3] = 255;
            }
        }
    }
    assert(bro.image.encodePngFile(path, px, w, h, 4), 'wrote ' + path);
}

const neutralPath = tmpDir + '/neutral.png';
const redPath = tmpDir + '/redboost.png';
writeLutStrip(neutralPath, (r, g, b) => [r, g, b]);
writeLutStrip(redPath, (r, g, b) => [r + 0.3, g, b]);

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '96');
canvas.setAttribute('height', '96');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping color LUT test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setCamera({
        fov: 60, near: 0.1, far: 100,
        position: [0, 0, 5], target: [0, 0, 0], up: [0, 1, 0],
    });
    // Mid-gray emissive patch; zero-intensity light suppresses the implicit
    // sun so the pixel value is deterministic (~0.5 -> 128).
    scene.createLight({ type: 'directional', intensity: 0 });
    scene.createMesh({
        mesh: Mesh.box(2, 2, 0.1), color: [0.5, 0.5, 0.5, 1], emissive: 1,
    });

    const center = () => {
        const img = scene.captureFrame();
        const i = (48 * img.width + 48) * 4;
        return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2] };
    };

    const base = center();
    assert(base.r > 100 && base.r < 160, `gray patch visible (r=${base.r})`);
    assert(base.r === base.g && base.g === base.b,
        `patch is neutral gray (${base.r},${base.g},${base.b})`);

    // --- Neutral LUT = identity within quantization tolerance ---------------
    assert(scene.setColorLUT({ path: neutralPath, size: SIZE }),
        'neutral LUT loads');
    const neutral = center();
    assert(Math.abs(neutral.r - base.r) <= 2 &&
           Math.abs(neutral.g - base.g) <= 2 &&
           Math.abs(neutral.b - base.b) <= 2,
        `neutral LUT is identity (${base.r},${base.g},${base.b}) -> ` +
        `(${neutral.r},${neutral.g},${neutral.b})`);

    // --- Red-boost LUT shifts the gray patch red ----------------------------
    assert(scene.setColorLUT({ path: redPath }), 'red LUT loads (size inferred)');
    const red = center();
    assert(red.r - base.r > 50,
        `red channel boosted by LUT (${base.r} -> ${red.r})`);
    assert(Math.abs(red.g - base.g) <= 2 && Math.abs(red.b - base.b) <= 2,
        `green/blue untouched (${red.g},${red.b} vs ${base.g},${base.b})`);

    // --- amount blends toward the graded result -----------------------------
    assert(scene.setColorLUT({ path: redPath, amount: 0.5 }), 'half-amount LUT');
    const half = center();
    const fullBoost = red.r - base.r;
    const halfBoost = half.r - base.r;
    assert(halfBoost > fullBoost * 0.3 && halfBoost < fullBoost * 0.7,
        `amount 0.5 blends halfway (full=${fullBoost}, half=${halfBoost})`);

    // --- clear restores the exact baseline ----------------------------------
    scene.setColorLUT(null);
    const cleared = center();
    assert(cleared.r === base.r && cleared.g === base.g && cleared.b === base.b,
        `cleared LUT restores baseline exactly ` +
        `(${cleared.r},${cleared.g},${cleared.b})`);

    // --- a malformed image is rejected --------------------------------------
    const badPath = tmpDir + '/bad.png';
    assert(bro.image.encodePngFile(badPath, new Uint8Array(10 * 10 * 4), 10, 10, 4),
        'wrote bad.png');
    assert(scene.setColorLUT({ path: badPath }) === false,
        'non-strip image rejected');

    flush();
}

document.body.removeChild(canvas);
try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (e) {}
