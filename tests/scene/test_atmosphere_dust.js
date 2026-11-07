// setAtmosphere's mieScattering / mieAbsorption per channel: a dusty air
// whose aerosol absorbs the blue makes a redder sky than the same haze in
// grey, and one value for every channel is the grey haze (the same frame as
// `mie` / a number for mieAbsorption).

const S = 64;
const cv = document.createElement('canvas');
cv.setAttribute('width', String(S));
cv.setAttribute('height', String(S));
document.body.appendChild(cv);
flush();
const scene = cv.getContext('scene');

const px = (img, x, y) => {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
};

// The sky's mean colour over the frame's upper half, the eye on the ground
// looking at the horizon with the sun up behind it.
function sky(atm) {
    scene.setAtmosphere(atm);
    let img = null;
    for (let i = 0; i < 12; i++) img = scene.captureFrame();
    const sum = [0, 0, 0];
    for (let y = 0; y < S / 2; y++) {
        for (let x = 0; x < S; x++) {
            const p = px(img, x, y);
            for (let k = 0; k < 3; k++) sum[k] += p[k];
        }
    }
    return sum.map((s) => s / (S * S / 2));
}

if (!scene) {
    missingGpuContext('scene');
} else {
    scene.setCamera({ fov: 60, near: 0.5, far: 5000, position: [0, 2, 0], target: [0, 2, -100] });
    scene.createLight({ type: 'directional', direction: [0, -0.6, -0.8], color: [1, 1, 1], intensity: 128000 });
    scene.setExposure({ mode: 'auto', meter: 'incident' });

    const HAZE = 40e-6;
    const grey = sky({ mieScattering: HAZE, mieAbsorption: 4e-6 });
    const greyRgb = sky({ mieScattering: [HAZE, HAZE, HAZE], mieAbsorption: [4e-6, 4e-6, 4e-6] });
    for (let k = 0; k < 3; k++) {
        assert(Math.abs(grey[k] - greyRgb[k]) < 1.5, `one value is every channel's (${grey} vs ${greyRgb})`);
    }
    const dust = sky({ mieScattering: [HAZE, HAZE * 0.8, HAZE * 0.6], mieAbsorption: [4e-6, 40e-6, 120e-6] });
    const ratio = (c) => c[0] / Math.max(c[2], 1);
    assert(ratio(dust) > ratio(grey) * 1.3,
           `dust that absorbs the blue reddens the sky (r/b ${ratio(dust).toFixed(2)} vs grey ${ratio(grey).toFixed(2)}; ${dust} vs ${grey})`);
}
