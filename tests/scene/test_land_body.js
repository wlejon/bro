// The land on a round body (docs/scene-world-architecture.md §11, "The land
// on the body"; scene/nodes/land_node.h, world/body_surface_map.h): an
// Earth-sized sphere whose land is keyed on the cube faces of its chart and
// drawn in rings on its map about the eye.
// Verifies:
//   - the CPU's heights are FastNoise2's 3D tree at the datum's point under
//     a place point: landHeight's ground, taken back to the body, stands
//     that high over the datum
//   - the GPU's heights are the tree's there too: a land at a constant 50 m
//     is seen from 52 m looking down and not from 48 m, 6371 km out
//   - it draws from the grass to geostationary orbit: from 10 km, 400 km
//     and 36,000 km up the frame's middle is the land, and from orbit the
//     body is a disc with space round it
//   - over a cube's corner, where three faces' tiles meet, no hole shows
//   - the body's curve: from 2 m up looking level, the ground's horizon is
//     below the eye's level line, as far below as the body's dip has it

function patchAvg(img, x0, y0, x1, y1, ch) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) { sum += img.data[(y * img.width + x) * 4 + ch]; n++; }
    return n ? sum / n : 0;
}
function patchMin(img, x0, y0, x1, y1, ch) {
    let m = 255;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) m = Math.min(m, img.data[(y * img.width + x) * 4 + ch]);
    return m;
}

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};
const RED = 'LandSurface landSurface(LandPoint q) { return landSurfaceOf(vec3(0.8, 0.05, 0.05)); }';

const S = 128;
const canvas = document.createElement('canvas');
canvas.setAttribute('width', String(S));
canvas.setAttribute('height', String(S));
document.body.appendChild(canvas);
flush();

const scn = canvas.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.createLight({ type: 'directional', direction: [0.3, -1, 0.2], color: [1, 1, 1], intensity: 3 });
    const frames = (n = 3) => { flush(); for (let i = 0; i < n; i++) advanceTime(16); };
    const R = 6371000;
    const unit = (v) => { const l = Math.hypot(v[0], v[1], v[2]); return [v[0] / l, v[1] / l, v[2] / l]; };
    // Stand the place on the body at direction `d`.
    const standAt = (d) => { const u = unit(d); scn.setBody({ radius: R, at: [u[0] * R, u[1] * R, u[2] * R] }); };
    const look = (pos, target, up = [0, 0, -1], far = 1e5, near = 0.1) =>
        scn.setCamera({ fov: 50, near, far, position: pos, target, up });

    standAt([0.31, 0.83, -0.46]);
    const SEED = 4242;
    const hills = N('Multiply', { LHS: N('FractalFBm', { Source: N('Perlin', { 'Feature Scale': 2000 }), Octaves: 5 }),
                                  RHS: 300 });
    const land = scn.createLand({ height: hills, seed: SEED, surface: RED });
    assert(land, 'createLand on a round body');
    // (Its rings reach round the body by themselves: no `levels` asked.)

    // --- heights on the CPU ---------------------------------------------------
    let worst = 0;
    for (const [x, z] of [[0, 0], [12.5, -40], [-3000, 2200], [7000, 7000], [-9000, 30]]) {
        const y = land.landHeight(x, z);
        const b = scn.toBody([x, y, z]);
        const r = Math.hypot(b[0], b[1], b[2]);
        const want = hills.genSingle3D(b[0] / r * R, b[1] / r * R, b[2] / r * R, SEED);
        worst = Math.max(worst, Math.abs(r - R - want));
    }
    assert(worst < 0.01, `landHeight stands the tree's height over the datum (worst ${worst.toFixed(4)} m)`);

    // --- the GPU's heights: a land at 50 m --------------------------------------
    land.setLand({ height: N('Constant', { Value: 50 }), surface: RED });
    const flat = land.landHeight(10, 10);
    look([10, flat + 2, 10], [10, flat - 8, 10]);
    frames(6);
    let img = scn.captureFrame();
    const above = patchAvg(img, 16, 16, S - 16, S - 16, 0);
    look([10, flat - 2, 10], [10, flat - 12, 10]);
    frames(6);
    img = scn.captureFrame();
    const below = patchAvg(img, 16, 16, S - 16, S - 16, 0);
    assert(above > 40 && below < above / 4,
           `seen from 2 m over it, not from 2 m under (${above.toFixed(1)} / ${below.toFixed(1)})`);

    // --- from the grass to orbit -----------------------------------------------
    land.setLand({ height: hills, surface: RED });
    for (const alt of [10e3, 400e3, 36000e3]) {
        const y = land.landHeight(0, 0) + alt;
        look([0, y, 0], [0, y - 10, 0], [0, 0, -1], alt + 2 * R, Math.max(0.1, alt * 1e-4));
        frames(10);
        img = scn.captureFrame();
        const mid = patchAvg(img, 56, 56, 72, 72, 0);
        assert(mid > 40, `${alt / 1000} km up, the land below (r ${mid.toFixed(1)}; ${JSON.stringify(land.landStats())})`);
        if (alt > 1e7) {
            const corner = patchAvg(img, 0, 0, 8, 8, 0);
            assert(corner < 10, `from orbit, space round the body (r ${corner.toFixed(1)})`);
        }
    }

    // --- a cube's corner --------------------------------------------------------
    // Level land (so a hole is the clear colour, not a slope turned from the
    // sun), looked down on with the corner, where three faces' tiles meet,
    // in the middle.
    standAt([1, 1, 1]);
    land.setLand({ height: N('Constant', { Value: 0 }), surface: RED });
    const s3 = 1 / Math.sqrt(3);
    const corner = scn.toPlace([s3 * R, s3 * R, s3 * R]);
    for (const alt of [300, 20e3]) {
        const y = land.landHeight(corner[0], corner[2]) + alt;
        look([corner[0], y, corner[2]], [corner[0], y - 10, corner[2]], [0, 0, -1], alt * 10 + 1e4, 0.1);
        frames(10);
        img = scn.captureFrame();
        const mean = patchAvg(img, 8, 8, S - 8, S - 8, 0), hole = patchMin(img, 8, 8, S - 8, S - 8, 0);
        assert(mean > 40 && hole > 0.6 * mean,
               `over a cube's corner from ${alt} m, no hole (darkest red ${hole}, mean ${mean.toFixed(1)})`);
    }

    // --- the body's curve: the horizon dips ------------------------------------
    // From 20 km up (over a land at the datum) the horizon is acos(R / (R +
    // h)) below level: 4.5 degrees, 11 pixels of a 50-degree view 128 high.
    // From the same height over a flat body it would be at the level line.
    land.setLand({ height: N('Constant', { Value: 0 }), surface: RED });
    const H = 20000;
    look([0, H, 0], [0, H, -1000], [0, 1, 0], 1e6, 1);
    frames(10);
    img = scn.captureFrame();
    // The first row from the top that is the land's red.
    let row = -1;
    for (let y = 0; y < S && row < 0; y++) if (patchAvg(img, 32, y, 96, y + 1, 0) > 40) row = y;
    const dip = Math.acos(R / (R + H));
    const want = S / 2 + Math.tan(dip) / Math.tan(25 * Math.PI / 180) * S / 2;
    assert(row >= 0 && Math.abs(row - want) < 2.5, `the horizon dips with the body (row ${row}, want ${want.toFixed(1)})`);

    console.log('test_land_body: OK');
}
