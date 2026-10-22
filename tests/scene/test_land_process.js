// A land's processes (docs/scene-world-architecture.md §11, "Processes";
// scene/world/land_grid.h, vulkan/ground/scene_land_process.h): kernels the
// world writes, run over a grid of the land when it is made, whose first
// channel's x the land's height adds to its tree.
// Verifies:
//   - until they have run and been read back the land is not ready: its
//     heights are NaN, landStats().ready false; then it is
//   - the CPU's heights are the grid's: at a cell's centre exactly what
//     procInit wrote there (procPoint, procData), between centres bilinear
//   - the GPU's heights are the CPU's: the land is seen from 2 m over the
//     CPU's height and not from 2 m under, at cell centres and between
//   - a pass runs its iterations over procInit's values (a 3 x 3 smoothing
//     lowers the hills), and setLand with new processes runs them again
//   - the land's module reads every channel: channel 1 colours the surface
//   - on a round body the grid is the six cube faces: across a face's edge
//     the heights are procInit's smooth field to within its bilinear error,
//     with no step, and the GPU stands on them there too
//   - processes that do not compile leave the land unready and drawing
//     nothing

function patchAvg(img, x0, y0, x1, y1, ch) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) { sum += img.data[(y * img.width + x) * 4 + ch]; n++; }
    return n ? sum / n : 0;
}

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};
const RED = 'LandSurface landSurface(LandPoint q) { return landSurfaceOf(vec3(0.8, 0.05, 0.05)); }';

const S = 96;
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
    const untilReady = (land) => {
        for (let i = 0; i < 40 && !land.landStats()?.ready; i++) frames(1);
        frames(6);
        return !!land.landStats()?.ready;
    };
    const look = (x, y, z, far = 5000) =>
        scn.setCamera({ fov: 50, near: 0.1, far, position: [x, y, z], target: [x, y - 10, z], up: [0, 0, -1] });
    // How red the middle of the view looking down from `dy` over the CPU's height at (x, z).
    const redFrom = (land, x, z, dy) => {
        look(x, land.landHeight(x, z) + dy, z);
        frames(6);
        return patchAvg(scn.captureFrame(), 24, 24, S - 24, S - 24, 0);
    };
    const standsOn = (land, x, z, what) => {
        const above = redFrom(land, x, z, 2), below = redFrom(land, x, z, -2);
        assert(above > 40 && below < above / 4,
               `${what}: seen from 2 m over the CPU's height, not from 2 m under (${above.toFixed(1)} / ${below.toFixed(1)}; ` +
               `${JSON.stringify(land.landStats())})`);
    };

    // --- a flat land: procInit alone ------------------------------------------
    // A 1 km land in 50 cells of 40 m; procInit a smooth field plus procData(0).x.
    const E = 1000, CELLS = 50, CELL = 2 * E / CELLS;
    const f = (x, z) => 60 * Math.sin(x / 150) * Math.cos(z / 210) + 5;
    const INIT = `
void procInit(inout vec4 v[PROC_CHANNELS]) {
    vec3 p = procPoint();
    v[0].x = 60.0 * sin(p.x / 150.0) * cos(p.z / 210.0) + procData(0).x;
}
void blur(inout vec4 v[PROC_CHANNELS]) {
    vec4 s = vec4(0.0);
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) s += procRead(0, ivec2(i, j));
    v[0] = s / 9.0;
}
`;
    const land = scn.createLand({ height: N('Constant', { Value: 0 }), extent: E, surface: RED,
                                  processes: { source: INIT, resolution: CELLS, data: [5, 0, 0, 0] } });
    assert(land, 'createLand with processes');
    assert(Number.isNaN(land.landHeight(0, 0)) && land.landStats()?.ready === false,
           `not ready before they have run (${land.landHeight(0, 0)}, ${JSON.stringify(land.landStats())})`);
    assert(untilReady(land), `ready once they have run (${JSON.stringify(land.landStats())})`);

    // Cell (i, j)'s centre is at -E + (i + 0.5) CELL (i from 0).
    const centre = (i) => -E + (i + 0.5) * CELL;
    let worst = 0;
    for (const [i, j] of [[12, 33], [0, 0], [49, 7], [25, 25], [40, 49]]) {
        const x = centre(i), z = centre(j);
        worst = Math.max(worst, Math.abs(land.landHeight(x, z) - f(x, z)));
    }
    assert(worst < 2e-3, `at a cell's centre the height is procInit's (worst ${worst.toExponential(2)} m)`);
    // Between centres, bilinear.
    {
        const x0 = centre(20), z0 = centre(30), fx = 0.3, fz = 0.7;
        const h = (x, z) => land.landHeight(x, z);
        const want = (1 - fz) * ((1 - fx) * h(x0, z0) + fx * h(x0 + CELL, z0)) +
                     fz * ((1 - fx) * h(x0, z0 + CELL) + fx * h(x0 + CELL, z0 + CELL));
        const got = h(x0 + fx * CELL, z0 + fz * CELL);
        assert(Math.abs(got - want) < 2e-3, `between centres, bilinear (${got.toFixed(4)} / ${want.toFixed(4)})`);
    }
    standsOn(land, centre(30), centre(20), 'at a cell centre');
    standsOn(land, centre(10) + 13.7, centre(41) - 8.2, 'between centres');

    // --- a pass ---------------------------------------------------------------
    const amplitude = () => {
        let m = 0;
        for (let i = 0; i < CELLS; i += 3) for (let j = 0; j < CELLS; j += 3)
            m = Math.max(m, Math.abs(land.landHeight(centre(i), centre(j)) - 5));
        return m;
    };
    const before = amplitude();
    land.setLand({ processes: { source: INIT, resolution: CELLS, data: [5, 0, 0, 0],
                                passes: [{ fn: 'blur', iterations: 30 }] } });
    assert(Number.isNaN(land.landHeight(0, 0)), 'new processes: not ready until they have run again');
    assert(untilReady(land), 'ready again');
    const after = amplitude();
    assert(after < 0.8 * before && after > 0.1 * before,
           `30 iterations of a 3 x 3 smoothing lower the hills (${before.toFixed(2)} -> ${after.toFixed(2)} m)`);
    standsOn(land, centre(17) + 5, centre(28) + 11, 'after a pass');

    // --- plants standing on it -------------------------------------------------
    // They stand on the land's heights (its processes' with their rules'
    // ground: bro_plant_placement_test), so none is placed while it is not
    // ready, and they are once it is.
    const plants = scn.createPlants({
        species: [{ height: 8, radius: 1, albedo: [0.02, 0.4, 0.02] }],
        rules: { ground: N('Constant', { Value: 0 }), range: 150, layers: [{ spacing: 6, chance: 1, species: [0] }] },
        range: 150, land,
    });
    look(0, 200, 0);
    frames(20);
    const placedBefore = plants.plantPlacement();
    land.setLand({ processes: { source: INIT, resolution: CELLS, data: [9, 0, 0, 0] } });
    frames(1);
    const held = plants.plantPlacement();
    assert(placedBefore.cells > 0 && held.cells === 0 && held.pending > 0,
           `plants on it: none placed while it is not ready (${JSON.stringify(placedBefore)} -> ${JSON.stringify(held)})`);
    assert(untilReady(land), 'ready with plants on it');
    for (let i = 0; i < 60 && plants.plantPlacement().pending > 0; i++) frames(1);
    assert(plants.plantPlacement().cells > 0, `placed once it is (${JSON.stringify(plants.plantPlacement())})`);
    plants.visible = false;

    // --- a second channel the surface reads -------------------------------------
    land.setLand({
        surface: 'LandSurface landSurface(LandPoint q) { return landSurfaceOf(vec3(landGrid(1, q.p).x, 0.05, 0.05)); }',
        processes: { resolution: CELLS, channels: 2, source: `
void procInit(inout vec4 v[PROC_CHANNELS]) {
    v[0].x = 3.0;
    v[1].x = procPoint().x > 0.0 ? 0.8 : 0.02;
}` },
    });
    assert(untilReady(land), 'two channels: ready');
    assert(Math.abs(land.landHeight(100, 100) - 3) < 1e-4, `channel 0 is the height (${land.landHeight(100, 100)})`);
    const east = redFrom(land, 300, 0, 20), west = redFrom(land, -300, 0, 20);
    assert(east > 40 && west < east / 4, `channel 1 colours the surface (east ${east.toFixed(1)}, west ${west.toFixed(1)})`);

    // --- processes that do not compile -----------------------------------------
    land.setLand({ surface: RED, processes: { source: 'void procInit(inout vec4 v[PROC_CHANNELS]) { nonsense; }' } });
    frames(10);
    assert(!land.landStats().ready && Number.isNaN(land.landHeight(0, 0)),
           'processes that do not compile: never ready');
    land.setLand({ processes: null });
    assert(land.landStats().ready && land.landHeight(0, 0) === 0, 'no processes: ready at once, the tree alone');
    land.visible = false;

    // --- a round body: the six faces ----------------------------------------------
    const R = 6371000;
    const unit = (v) => { const l = Math.hypot(v[0], v[1], v[2]); return [v[0] / l, v[1] / l, v[2] / l]; };
    // On the edge between the +X and +Y faces.
    const at = unit([1, 1, 0.3]);
    scn.setBody({ radius: R, at: [at[0] * R, at[1] * R, at[2] * R] });
    const g = (d) => 300 * d[0] + 120 * d[2];
    const round = scn.createLand({
        height: N('Constant', { Value: 0 }), surface: RED,
        processes: { resolution: 64, source: `
void procInit(inout vec4 v[PROC_CHANNELS]) {
    vec3 d = procUp();
    v[0].x = 300.0 * d.x + 120.0 * d.z;
}` },
    });
    assert(untilReady(round), `on a round body: ready (${JSON.stringify(round.landStats())})`);
    // Along a great circle across the edge, 40 km either side: each place
    // point's ground over the datum against the field at its direction.
    const d1 = unit([1, 0.99, 0.3]), d2 = unit([0.99, 1, 0.3]);
    let err = 0, step = 0, prev = null;
    for (let k = 0; k <= 40; k++) {
        const t = k / 40;
        const d = unit([d1[0] + (d2[0] - d1[0]) * t, d1[1] + (d2[1] - d1[1]) * t, d1[2] + (d2[2] - d1[2]) * t]);
        const p = scn.toPlace([d[0] * R, d[1] * R, d[2] * R]);
        const y = round.landHeight(p[0], p[2]);
        const b = scn.toBody([p[0], y, p[2]]);
        const r = Math.hypot(b[0], b[1], b[2]);
        const h = r - R;
        err = Math.max(err, Math.abs(h - g([b[0] / r, b[1] / r, b[2] / r])));
        if (prev !== null) step = Math.max(step, Math.abs(h - prev));
        prev = h;
    }
    // (A cell is ~310 km of a field changing 300 m a radian: its bilinear
    // error is centimetres, its halo's nearest-cell copy at the edge a metre.)
    assert(err < 3, `across a face's edge the heights are procInit's field (worst ${err.toFixed(3)} m)`);
    assert(step < 1, `and step nowhere between points 2 km apart (largest ${step.toFixed(3)} m)`);
    for (const d of [d1, d2]) {
        const p = scn.toPlace([d[0] * R, d[1] * R, d[2] * R]);
        standsOn(round, p[0], p[2], `on the ${d === d1 ? '+X' : '+Y'} face by the edge`);
    }

    console.log('test_land_process: OK');
}
