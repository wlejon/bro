// The land's detail a fragment at a time (scene/nodes/land_node.h,
// draw/mesh.frag's SCENE_LAND build): its fragments read its albedo and
// normal from its tiles at their own footprint, not the rings' vertices.
// Verifies, from 1 km straight down:
//   - a straight edge of its surface (dark to light, at 30° to the view's
//     rows) is drawn narrow and straight: its 20-80% rise a few pixels
//     wide, and the rows' crossings off the line through them by little
//   - so is a straight step of its height lit from the side (its normal)

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

// Each middle row's crossing of the edge between its plateaus (channel 1):
// the mean width of the 20-80% rise, and the crossings' RMS off their
// least-squares line.
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

const scn = canvas.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    const frames = (n = 3) => { flush(); for (let i = 0; i < n; i++) advanceTime(16); };
    scn.setCamera({ fov: 50, near: 1, far: 5000, position: [0, 1000, 0], target: [0, 0, 0], up: [0, 0, -1] });
    const light = scn.createLight({ type: 'directional', direction: [0, -1, 0], color: [1, 1, 1], intensity: 3 });
    // The line x cos 30° + z sin 30° = 0 (a row's crossing moves 0.58 px a row).
    const c = Math.cos(Math.PI / 6).toFixed(5), s = Math.sin(Math.PI / 6).toFixed(5);

    const land = scn.createLand({ height: N('Constant', { Value: 0 }), extent: 4000, surface: `
LandSurface landSurface(LandPoint q) {
    return landSurfaceOf(vec3(q.p.x * ${c} + q.p.y * ${s} > 0.0 ? 0.6 : 0.05));
}` });
    frames(20);
    const a = edge(scn.captureFrame());
    // (Read at its vertices: 3.5 px wide, 1.0 px off. A fragment at a time: 1.8, 0.5.)
    assert(a.rows > 100 && a.width < 2.6 && a.rms < 0.75,
           `the surface's edge is narrow and straight (${a.rows} rows, ${a.width.toFixed(2)} px wide, ${a.rms.toFixed(2)} px off its line)`);

    // A ramp 60 m wide (x cos 30° + z sin 30° from -30 to 30 m) 12 m up,
    // lit from the side: its two creases are edges of its light.
    land.setLand({
        height: N('Multiply', { LHS: N('Min', { LHS: N('Max', { LHS: N('Gradient', { 'Multiplier X': +c, 'Multiplier Y': +s }), RHS: -30 }), RHS: 30 }), RHS: 0.2 }),
        surface: 'LandSurface landSurface(LandPoint q) { return landSurfaceOf(vec3(0.4)); }',
    });
    light.direction = [-0.8, -0.5, -0.3];
    frames(20);
    const n = edge(scn.captureFrame());
    // (At its vertices: 6.5 px wide. A fragment at a time: 3.3.)
    assert(n.rows > 100 && n.width < 4.8 && n.rms < 0.5,
           `the light's edge on its height is narrow and straight (${n.rows} rows, ${n.width.toFixed(2)} px wide, ${n.rms.toFixed(2)} px off its line)`);

    console.log(`test_land_fragment: OK (surface ${a.width.toFixed(2)} px wide, ${a.rms.toFixed(2)} off; ` +
                `light ${n.width.toFixed(2)} px wide, ${n.rms.toFixed(2)} off)`);
}
