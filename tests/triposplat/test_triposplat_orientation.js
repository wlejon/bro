// Weights-gated: bro.triposplat.generate() returns the cloud Y-up and facing
// the default camera (+Z). The sampler works Z-up facing +X and the binding
// turns it into scene space; it used to turn it the wrong way, so a figure
// came back head-down and side-on. Structure only, no image grading: a
// subject red on top, green bottom-left and blue bottom-right must come back
// with red above the other two and green to the left (-x) of blue, as the
// image shows it to a camera on +Z. Skips (passes with a message) without a
// GPU or the checkpoints (TRIPOSPLAT_WEIGHTS_ROOT, else the ../brovisionml
// and ../brodiffusion sibling weights).
//
// Wall time on an RTX 4090: ~20 s (load + one small generate).

const fs = require('node:fs');

const ROOT = process.env.TRIPOSPLAT_WEIGHTS_ROOT || '..';
const W = {
    dinov3: ROOT + '/brovisionml/weights/triposplat/clip_vision/dino_v3_vit_h.safetensors',
    vae: ROOT + '/brodiffusion/weights/triposplat/vae/flux2-vae.safetensors',
    flow: ROOT + '/brodiffusion/weights/triposplat/diffusion_models/triposplat_fp16.safetensors',
    decoder: ROOT + '/brodiffusion/weights/triposplat/vae/triposplat_vae_decoder_fp16.safetensors',
};

if (!bro.triposplat || bro.triposplat.available === false) {
    console.log('bro.triposplat is the stub; skipping');
} else if (!bro.gpu || !bro.gpu.available) {
    console.log('no GPU backend; skipping');
} else if (!Object.values(W).every((p) => fs.existsSync(p))) {
    console.log('triposplat weights not found under ' + ROOT + '; skipping');
} else {
    // A 512x512 block on a transparent background.
    const S = 512, data = new Uint8Array(S * S * 4);
    for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) {
        if (!(x > 128 && x < 384 && y > 96 && y < 416)) continue;
        const i = (y * S + x) * 4, top = y < 200, left = x < 256;
        data[i] = top ? 220 : 30;
        data[i + 1] = !top && left ? 200 : 40;
        data[i + 2] = !top && !left ? 200 : 40;
        data[i + 3] = 255;
    }

    const ts = bro.triposplat.load(W);
    const c = ts.generate({ width: S, height: S, data }, { steps: 8, numGaussians: 65536, seed: 7 });
    assert(!c.cancelled && c.count > 0, 'generated a cloud');

    // Mean position of each colour mass, from the DC term of the splats' SH.
    const stride = c.sh.length / c.count;
    const mass = { r: [0, 0, 0, 0], g: [0, 0, 0, 0], b: [0, 0, 0, 0] };
    for (let i = 0; i < c.count; i++) {
        if (c.opacities[i] < 0.3) continue;
        const r = c.sh[i * stride], g = c.sh[i * stride + 1], b = c.sh[i * stride + 2];
        const k = r > g + 0.3 && r > b + 0.3 ? 'r' : g > r + 0.3 && g > b + 0.3 ? 'g'
                : b > r + 0.3 && b > g + 0.3 ? 'b' : null;
        if (!k) continue;
        for (let a = 0; a < 3; a++) mass[k][a] += c.positions[i * 3 + a];
        mass[k][3]++;
    }
    const at = {};
    for (const k in mass) {
        assert(mass[k][3] > 50, k + ' mass reconstructed (' + mass[k][3] + ' splats)');
        at[k] = mass[k].slice(0, 3).map((v) => v / mass[k][3]);
        console.log(k + ': ' + mass[k][3] + ' splats at ' + at[k].map((v) => v.toFixed(3)).join(', '));
    }
    assert(at.r[1] > at.g[1] && at.r[1] > at.b[1], 'the red top sits above the bottom (Y-up, not head-down)');
    assert(at.g[0] < at.b[0], 'image-left (green) is at -x of image-right (blue): facing +Z, not mirrored');
    assert(Math.abs(at.g[0] - at.b[0]) > Math.abs(at.g[2] - at.b[2]),
           'left-right runs along x, not z (not side-on to the camera)');
    if (ts.dispose) ts.dispose();
}
console.log('PASS');
