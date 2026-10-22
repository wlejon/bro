// A smooth surface under the atmosphere mirrors the sky itself
// (common/scene_lighting.glsl's sceneAmbient: the sky-view table along R
// low in the air, not the sky light's smooth SH).
// Verifies, from 10 m over a level mirror (white, metallic, roughness 0.04)
// looking at the horizon, the sun behind the eye: the mirror k rows under
// the horizon is the sky k rows over it, at several k (its colour and its
// fall toward the zenith alike).

function rowRGB(img, y) {
    const s = [0, 0, 0];
    for (let x = img.width / 4; x < (3 * img.width) / 4; x++) {
        const i = (y * img.width + x) * 4;
        s[0] += img.data[i]; s[1] += img.data[i + 1]; s[2] += img.data[i + 2];
    }
    return s.map((v) => v / (img.width / 2));
}
const fmt = (c) => '[' + c.map((v) => v.toFixed(1)).join(', ') + ']';

const S = 256;
const canvas = document.createElement('canvas');
canvas.setAttribute('width', String(S));
canvas.setAttribute('height', String(S));
document.body.appendChild(canvas);
flush();

const scn = canvas.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    const frames = (n = 3) => { flush(); for (let i = 0; i < n; i++) advanceTime(16); };
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.setAtmosphere({});
    scn.createLight({ type: 'directional', direction: [0, -0.7, -0.7], color: [1, 1, 1], intensity: 3 });
    scn.setCamera({ fov: 50, near: 0.5, far: 20000, position: [0, 10, 0], target: [0, 10, -100], up: [0, 1, 0] });
    scn.createMesh({ mesh: 'plane', halfW: 10000, halfD: 10000, color: [1, 1, 1], metallic: 1, roughness: 0.04 });
    frames(20);
    const img = scn.captureFrame();

    // The horizon's row (the level less its dip from 10 m, a few hundredths
    // of a degree: under a row).
    const h = S / 2;
    let worst = 0;
    const rows = [];
    for (const k of [12, 30, 60, 100]) {
        const sky = rowRGB(img, h - k), mirror = rowRGB(img, h + k);
        const off = Math.max(...[0, 1, 2].map((c) => Math.abs(mirror[c] - sky[c]) / Math.max(sky[c], 8)));
        worst = Math.max(worst, off);
        rows.push(`${k}: sky ${fmt(sky)} mirror ${fmt(mirror)}`);
    }
    assert(worst < 0.15, `the mirror is the sky (worst ${(100 * worst).toFixed(1)}% off; ${rows.join('; ')})`);
    console.log(`test_mirror_sky: OK (worst ${(100 * worst).toFixed(1)}% off; ${rows.join('; ')})`);
}
