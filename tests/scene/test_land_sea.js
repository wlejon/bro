// The land's sea (scene/nodes/land_node.h's Sea, draw/mesh.frag's SCENE_LAND
// build): water up to a level wherever the ground lies lower, decided a
// fragment at a time from the fragment's own ground height.
// Verifies, from 1 km straight down over a slope whose coast runs at 30° to
// the view's rows:
//   - the coast is drawn sharp and straight: its 20-80% rise narrower than
//     the same coast drawn by the surface's colour (h < 0 dark), a texel's
//     albedo read a fragment at a time
//   - deep water is the sea's scatter colour, shallow water the seabed through
//     it; the land's heights stay the seabed's
//   - `sea: null` dries it

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};

const S = 512;
const canvas = document.createElement('canvas');
canvas.setAttribute('width', String(S));
canvas.setAttribute('height', String(S));
document.body.appendChild(canvas);
flush();

// Each middle row's crossing of the coast (channel 1): the mean width of the
// 20-80% rise, and the crossings' RMS off their least-squares line.
function edge(img) {
    const rows = [];
    let width = 0;
    for (let y = S / 4; y < 3 * S / 4; y += 2) {
        const v = (x) => img.data[(y * img.width + x) * 4 + 1];
        let lo = 255, hi = 0;
        for (let x = 8; x < S - 8; x++) { lo = Math.min(lo, v(x)); hi = Math.max(hi, v(x)); }
        const at = (f) => {
            const t = lo + f * (hi - lo);
            for (let x = 8; x < S - 9; x++)
                if ((v(x) - t) * (v(x + 1) - t) <= 0 && v(x) !== v(x + 1)) return x + (t - v(x)) / (v(x + 1) - v(x));
            return NaN;
        };
        const a = at(0.2), b = at(0.8), m = at(0.5);
        if (Number.isNaN(a) || Number.isNaN(b) || Number.isNaN(m)) continue;
        width += Math.abs(b - a);
        rows.push([y, m]);
    }
    const n = rows.length;
    const my = rows.reduce((s, r) => s + r[0], 0) / n, mx = rows.reduce((s, r) => s + r[1], 0) / n;
    let syy = 0, sxy = 0;
    for (const [y, x] of rows) { syy += (y - my) * (y - my); sxy += (y - my) * (x - mx); }
    const k = sxy / syy;
    let r2 = 0;
    for (const [y, x] of rows) r2 += Math.pow(x - (mx + k * (y - my)), 2);
    return { rows: n, width: width / Math.max(n, 1), rms: Math.sqrt(r2 / Math.max(n, 1)) };
}

const px = (img, x, y) => {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
};

const scn = canvas.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    const frames = (n = 3) => { flush(); for (let i = 0; i < n; i++) advanceTime(16); };
    scn.setCamera({ fov: 50, near: 1, far: 5000, position: [0, 1000, 0], target: [0, 0, 0], up: [0, 0, -1] });
    // From the side, so the water's glint is not in the view.
    scn.createLight({ type: 'directional', direction: [-0.8, -0.5, -0.3], color: [1, 1, 1], intensity: 3 });
    const c = Math.cos(Math.PI / 6), s = Math.sin(Math.PI / 6);

    // The ground rises 0.2 m a metre across the line x cos 30° + z sin 30° = 0:
    // a sea at 0 is a third of a metre deep 1.5 m out (under a pixel here).
    const slope = N('Gradient', { 'Multiplier X': 0.2 * c, 'Multiplier Y': 0.2 * s });
    const sand = 'LandSurface landSurface(LandPoint q) { return landSurfaceOf(vec3(0.6, 0.55, 0.4)); }';

    // The negative control: the coast as the surface's colour.
    const land = scn.createLand({ height: slope, extent: 4000, surface: `
LandSurface landSurface(LandPoint q) { return landSurfaceOf(q.height < 0.0 ? vec3(0.004, 0.016, 0.03) : vec3(0.6, 0.55, 0.4)); }` });
    frames(20);
    const painted = edge(scn.captureFrame());

    land.setLand({ surface: sand, sea: { level: 0, absorb: [5, 5, 5] } });
    frames(20);
    const sea = edge(scn.captureFrame());
    assert(sea.rows > 100 && sea.width < 0.75 * painted.width && sea.rms < 0.5,
           `the coast is sharp and straight (${sea.rows} rows, ${sea.width.toFixed(2)} px wide, ${sea.rms.toFixed(2)} off; ` +
           `drawn by colour ${painted.width.toFixed(2)} px, ${painted.rms.toFixed(2)} off)`);

    // Clear water: the seabed shows through the shallows and fades into the
    // deep's scatter colour. (The view's x runs along the world's x, its
    // rows along z; the deep is where x cos 30° + z sin 30° < 0.)
    land.setLand({ sea: { absorb: [0.45, 0.09, 0.05], scatter: [0.004, 0.016, 0.03] } });
    frames(20);
    const metresPerPx = 2 * 1000 * Math.tan(25 * Math.PI / 180) / S;
    // d m along the slope's rise from the coast, through the view's middle.
    const at = (img, d) => px(img, Math.round(S / 2 + d * c / metresPerPx), Math.round(S / 2 + d * s / metresPerPx));
    const img = scn.captureFrame();
    const shore = at(img, -5), mid = at(img, -40), deep = at(img, -200);
    // (The dry slope faces away from the light, so is darker than level water.)
    assert(shore[1] > mid[1] && mid[1] > deep[1],
           `the water darkens with depth (1 m deep ${shore}, 8 m ${mid}, 40 m ${deep})`);
    assert(deep[2] > deep[0] && shore[0] > mid[0] + 10,
           `the deep is blue, the red goes first (deep ${deep}, 1 m ${shore}, 8 m ${mid})`);
    const h = land.landHeight(-200 * c, -200 * s);
    assert(Math.abs(h + 40) < 0.1, `the land's height is the seabed's (${h.toFixed(2)})`);

    land.setLand({ sea: null });
    frames(20);
    const drained = at(scn.captureFrame(), -200);
    assert(drained[1] > 3 * deep[1] + 10, `with no sea the seabed is bare (${drained} where the deep was ${deep})`);

    // A land all under its sea is water everywhere.
    land.setLand({ height: N('Constant', { Value: -30 }), sea: { scatter: [0.2, 0.3, 0.4] } });
    frames(20);
    const ocean = at(scn.captureFrame(), 0);
    assert(ocean[2] > 40 && ocean[2] > ocean[0], `a land under its sea is water (${ocean})`);

    console.log(`test_land_sea: OK (coast ${sea.width.toFixed(2)} px wide, ${sea.rms.toFixed(2)} off; ` +
                `by colour ${painted.width.toFixed(2)}, ${painted.rms.toFixed(2)}; deep ${deep}, shore ${shore})`);
}
