// The atmosphere on a round body (scene/world/atmosphere.h,
// vulkan/lighting/scene_atmosphere.cpp): the air wraps the body set with
// scene.setBody, its radius the body's, about its centre.
// Verifies:
//   - from 2000 km up looking at the limb, the land's limb is the air's:
//     a band of blue glow just over the land's limb, black space above it,
//     the lit land below
//   - from 20,000 km a land of one albedo is smooth pixel to pixel (the
//     air read from its tables there, not the froxels)
//   - from 2 m on the ground the day sky over the horizon is blue
//   - round a 100 km moon the air is the moon's: from 300 km up the view
//     under the level is space (an Earth-sized air's lit disc fills it)

function boxRGB(img, x0, y0, x1, y1) {
    const s = [0, 0, 0];
    let n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) {
            const i = (y * img.width + x) * 4;
            s[0] += img.data[i]; s[1] += img.data[i + 1]; s[2] += img.data[i + 2]; n++;
        }
    return s.map((v) => v / n);
}
const fmt = (c) => '[' + c.map((v) => v.toFixed(1)).join(', ') + ']';

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};

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
    const R = 6371000;
    scn.setBody({ radius: R, at: [0, R, 0] });
    scn.setToneMap({ mode: 'aces' });
    // The limb seen from h up is a = acos(R / (R + h)) under the level, at
    // the body's point 90° - a round from the eye's: the sun at noon there.
    const h = 2000000, a = Math.acos(R / (R + h));
    const n = [0, Math.sin(a), -Math.cos(a)];
    scn.createLight({ type: 'directional', direction: [-n[0], -n[1], -n[2]], color: [1, 1, 1], intensity: 3 });
    scn.setAtmosphere({});
    const land = scn.createLand({ height: N('Constant', { Value: 0 }),
                                  surface: 'LandSurface landSurface(LandPoint q) { return landSurfaceOf(vec3(0.1, 0.25, 0.08)); }' });
    assert(land, 'createLand on the body');

    // --- from 2000 km, the limb (the view's middle row) ----------------------------
    const eye = [0, h, 0];
    scn.setCamera({ fov: 10, near: 100, far: 2e7, position: eye,
                    target: [0, h - 1e6 * Math.sin(a), -1e6 * Math.cos(a)], up: [0, 1, 0] });
    frames(30);
    const img = scn.captureFrame();
    const mid = S / 2, px = 10 / S;   // degrees a pixel
    const space = boxRGB(img, 64, 0, 192, 30);              // 5° and more over the limb
    const glow = boxRGB(img, 64, mid - Math.round(0.8 / px), 192, mid - Math.round(0.2 / px));   // the air over the limb
    const below = boxRGB(img, 64, mid + Math.round(1.5 / px), 192, S);   // the land
    console.log(`space ${fmt(space)} glow ${fmt(glow)} land ${fmt(below)}`);
    assert(Math.max(...space) < 8, `space over the air is black ${fmt(space)}`);
    assert(glow[2] > 40 && glow[2] > glow[0] + 10, `just over the land's limb the air glows blue ${fmt(glow)}`);
    assert(below[1] > 20, `under it the land, lit ${fmt(below)}`);

    // --- from 20,000 km, the disc ----------------------------------------------------
    // A land of one albedo seen whole: smooth pixel to pixel. (The froxels'
    // slices, squared in depth out to the air's horizon, are deeper there
    // than the air under the eye, and their dither was noise: 9.4 levels
    // pixel to pixel; read from the tables, 0.03.)
    scn.setDither(false);
    scn.setCamera({ fov: 40, near: 1000, far: 1e8, position: [0, 2e7, 0], target: [0, -R, 0], up: [0, 0, -1] });
    frames(10);
    {
        const disc = scn.captureFrame();
        let diff = 0, n = 0;
        for (let y = 112; y < 144; y++)
            for (let x = 112; x < 143; x++) {
                const i = (y * disc.width + x) * 4;
                for (let c = 0; c < 3; c++) diff += Math.abs(disc.data[i + 4 + c] - disc.data[i + c]);
                n += 3;
            }
        const lit = boxRGB(disc, 112, 112, 144, 144);
        console.log(`disc ${fmt(lit)}, ${(diff / n).toFixed(2)} levels pixel to pixel`);
        assert(lit[1] > 20 && diff / n < 2, `from 20,000 km the lit land is smooth (${fmt(lit)}, ${(diff / n).toFixed(2)} levels pixel to pixel)`);
    }
    scn.setDither(true);

    // --- from the ground -------------------------------------------------------------
    scn.setCamera({ fov: 60, near: 0.1, far: 1e5, position: [0, 2, 0], target: [0, 4, -5], up: [0, 1, 0] });
    frames(10);
    const sky = boxRGB(scn.captureFrame(), 80, 20, 176, 100);
    assert(sky[2] > sky[0] + 20 && sky[2] > 60, `from the ground the day sky is blue ${fmt(sky)}`);

    // --- a moon: the air's planet is the body's size -------------------------------
    // A body of 100 km seen from 300 km up looking level: its limb (and its
    // air's, 100 km thick) is 60° and more under the level, out of a 60°
    // view, which is black space to its foot; an air about an Earth-sized
    // planet would fill the view's lower part with its lit disc.
    const r = 100000;
    scn.setBody({ radius: r, at: [0, r, 0] });
    land.visible = false;
    scn.setCamera({ fov: 60, near: 100, far: 2e6, position: [0, 3 * r, 0], target: [0, 3 * r, -1e5], up: [0, 1, 0] });
    frames(10);
    const low = boxRGB(scn.captureFrame(), 0, 215, S, S);   // 20-30° under the level
    assert(Math.max(...low) < 8, `round a moon the air is the moon's: under the level, space ${fmt(low)}`);

    console.log('test_atmosphere_body: OK');
}
