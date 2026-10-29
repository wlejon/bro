// A land's processes (scene/world/land_grid.h, vulkan/ground/scene_land_process.h): kernels the
// world writes, run over a grid of the land when it is made, whose first
// channel is the land's height over its tree (x added, y the tree's scale).
// Verifies:
//   - until they have run and been read back the land is not ready: its
//     heights are NaN, landStats().ready false; then it is
//   - the CPU's heights are the grid's: the cubic B-spline over what
//     procInit wrote at the cells' centres (procPoint, procData)
//   - the GPU's heights are the CPU's: the land is seen from 2 m over the
//     CPU's height and not from 2 m under, at cell centres and between
//   - a pass runs its iterations over procInit's values (a 3 x 3 smoothing
//     lowers the hills), and setLand with new processes runs them again
//   - the land's module reads every channel: channel 1 colours the surface
//   - channel 0's y scales the tree, on the CPU and the GPU, and is 1 where
//     procInit leaves it
//   - on a round body the grid is the six cube faces: across a face's edge
//     the heights are procInit's smooth field to within its B-spline error,
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
    // The grid read through the cubic B-spline over procInit's cells (the
    // halo and past it the edge's cells), at centres and between.
    const bspline = (t) => [(1 - t) ** 3 / 6, (3 * t ** 3 - 6 * t * t + 4) / 6, (-3 * t ** 3 + 3 * t * t + 3 * t + 1) / 6, t ** 3 / 6];
    const cellF = (i, j) => f(centre(Math.min(Math.max(i, 0), CELLS - 1)), centre(Math.min(Math.max(j, 0), CELLS - 1)));
    const spline = (x, z) => {
        const u = (x + E) / CELL - 0.5, v = (z + E) / CELL - 0.5;
        const i = Math.floor(u), j = Math.floor(v), wx = bspline(u - i), wz = bspline(v - j);
        let s = 0;
        for (let b = 0; b < 4; b++)
            for (let a = 0; a < 4; a++) s += wx[a] * wz[b] * cellF(i - 1 + a, j - 1 + b);
        return s;
    };
    let worst = 0;
    for (const [i, j] of [[12, 33], [0, 0], [49, 7], [25, 25], [40, 49]]) {
        const x = centre(i), z = centre(j);
        worst = Math.max(worst, Math.abs(land.landHeight(x, z) - spline(x, z)));
    }
    assert(worst < 2e-3, `at a cell's centre the height is the B-spline of procInit's cells (worst ${worst.toExponential(2)} m)`);
    {
        const x = centre(20) + 0.3 * CELL, z = centre(30) + 0.7 * CELL;
        const got = land.landHeight(x, z), want = spline(x, z);
        assert(Math.abs(got - want) < 2e-3, `between centres, the B-spline (${got.toFixed(4)} / ${want.toFixed(4)})`);
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
    scn.setBands({ real: 150 });
    const plants = scn.createPlants({
        species: [{ height: 8, radius: 1, albedo: [0.02, 0.4, 0.02] }],
        rules: { ground: N('Constant', { Value: 0 }), range: 150, layers: [{ spacing: 6, chance: 1, species: [0] }] },
        land,
    });
    // (From 100 m up: the eye's height over the land counts in the range,
    // so from 200 m none of a 150 m range is placed.)
    look(0, 100, 0);
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

    // Channel 0's z and w are the world's own, read by the plants' rules: a
    // wood of dense crowns where z is 1 (x < 0) and none where it is 0, the
    // land red under it; seen from 200 m straight down the west half is
    // the crowns' green, the east the land's red. (Their canopy past the
    // plants' range reads the same term: the land's module still compiles.)
    land.setLand({ surface: RED, processes: { resolution: CELLS, source: `
void procInit(inout vec4 v[PROC_CHANNELS]) {
    v[0].x = 0.0;
    v[0].z = procPoint().x < 0.0 ? 1.0 : 0.0;
}` } });
    plants.setPlants({
        species: [{ height: 8, radius: 3, albedo: [0.02, 0.4, 0.02] }],
        rules: { ground: N('Constant', { Value: 0 }), range: 150, layers: [{ spacing: 5, chance: { own: 'z' }, species: [0] }] },
    });
    assert(untilReady(land), 'own values: ready');
    for (let i = 0; i < 120 && plants.plantPlacement().pending > 0; i++) frames(1);
    look(0, 200, 0);
    frames(10);
    {
        const img = scn.captureFrame();
        const westRed = patchAvg(img, 8, S / 4, S / 2 - 16, (3 * S) / 4, 0);
        const eastRed = patchAvg(img, S / 2 + 16, S / 4, S - 8, (3 * S) / 4, 0);
        assert(land.landStats().compiled, `the land compiles with the plants' own-value canopy (${JSON.stringify(land.landStats())})`);
        assert(eastRed > 40 && westRed < eastRed / 2,
               `the woods grow where the land's own z is 1, not where 0 (red west ${westRed.toFixed(1)}, east ${eastRed.toFixed(1)})`);
    }
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

    // --- the tree's scale ----------------------------------------------------------
    // Channel 0's y scales the tree: a tree of 10 m under y = 0.5 + x / 2000
    // (linear, so exact between centres) and x = 2 is 7 + x / 200 m, on the
    // CPU and the GPU; a cell left alone keeps the tree whole (y starts at 1).
    land.setLand({
        height: N('Constant', { Value: 10 }), surface: RED,
        processes: { resolution: CELLS, source: `
void procInit(inout vec4 v[PROC_CHANNELS]) {
    v[0].x = 2.0;
    v[0].y = 0.5 + procPoint().x / 2000.0;
}` },
    });
    assert(untilReady(land), 'the tree\'s scale: ready');
    let worstScale = 0;
    for (const [x, z] of [[-700, 120], [0, 0], [333, -450], [810, 600]])
        worstScale = Math.max(worstScale, Math.abs(land.landHeight(x, z) - (7 + x / 200)));
    assert(worstScale < 2e-3, `channel 0's y scales the tree on the CPU (worst ${worstScale.toExponential(2)} m)`);
    standsOn(land, -600, 200, 'the tree scaled by 0.2');
    standsOn(land, 700, -300, 'the tree scaled by 0.85');
    land.setLand({ processes: { resolution: CELLS, source: 'void procInit(inout vec4 v[PROC_CHANNELS]) { v[0].x = 1.0; }' } });
    assert(untilReady(land) && Math.abs(land.landHeight(250, -90) - 11) < 1e-4,
           `a cell left alone keeps the tree whole (${land.landHeight(250, -90)})`);
    land.setLand({ height: N('Constant', { Value: 0 }) });

    // --- processes that do not compile -----------------------------------------
    land.setLand({ surface: RED, processes: { source: 'void procInit(inout vec4 v[PROC_CHANNELS]) { nonsense; }' } });
    frames(10);
    assert(!land.landStats().ready && Number.isNaN(land.landHeight(0, 0)),
           'processes that do not compile: never ready');
    land.setLand({ processes: null });
    assert(land.landStats().ready && land.landHeight(0, 0) === 0, 'no processes: ready at once, the tree alone');

    // --- regional grids ------------------------------------------------------------
    // The global grid flat at 5 m; two levels of regions about the eye (4 m
    // and 16 m cells), each the coarser field plus its own band, a pass
    // setting each region's edge cells to the coarser field's (its
    // boundary taken from it). Conserving nothing: the bands are far wider
    // than the coarser cells, which a conserving region would take back out
    // (below).
    land.setLand({ height: N('Constant', { Value: 0 }), surface: RED, processes: {
        resolution: CELLS,
        regions: { init: 'rinit', cells: 64, apron: 8, cell: 4, ratio: 4, levels: 2, reach: 1, keep: 1, conserve: [],
                   passes: [{ fn: 'redge', iterations: 1 }] },
        source: `
void procInit(inout vec4 v[PROC_CHANNELS]) { v[0].x = 5.0; }
float band(int level, vec3 p) {
    return level == 0 ? 6.0 * sin(p.x / 37.0) * cos(p.z / 23.0) : 3.0 * sin(p.x / 150.0 + p.z / 210.0);
}
void rinit(inout vec4 v[PROC_CHANNELS]) {
    v[0] = vec4(procCoarse(0).x + band(procLevel(), procPoint()), 1.0, 0.0, 0.0);
}
void redge(inout vec4 v[PROC_CHANNELS]) { if (procBorder() == 0) v[0].x = procCoarse(0).x; }
` } });
    assert(untilReady(land), `regions: ready (${JSON.stringify(land.landStats())})`);
    look(0, 40, 0);
    for (let i = 0; i < 400 && (!land.landStats().regions || land.landStats().regionsPending > 0); i++) frames(1);
    frames(10);
    const rs = land.landStats();
    assert(rs.regions >= 9 && rs.regionsPending === 0, `regions: the ones about the eye are in (${JSON.stringify(rs)})`);
    // The CPU's heights are the bands over the global grid's, under the eye.
    const bands = (x, z) => 5 + 6 * Math.sin(x / 37) * Math.cos(z / 23) + 3 * Math.sin(x / 150 + z / 210);
    let rworst = 0;
    for (const [x, z] of [[0, 0], [31, -17], [-60, 44], [45, 50]])
        rworst = Math.max(rworst, Math.abs(land.landHeight(x, z) - bands(x, z)));
    assert(rworst < 0.25, `regions: the CPU's heights are the levels' bands, to their B-spline error (worst ${rworst.toFixed(3)} m)`);
    // No step anywhere out from the eye, through the regions' aprons and
    // their edges into the global grid's field.
    let rstep = 0, prevH = null;
    for (let x = 0; x <= 1000; x += 0.5) {
        const h = land.landHeight(x, 0.37 * x);
        if (prevH !== null) rstep = Math.max(rstep, Math.abs(h - prevH));
        prevH = h;
    }
    assert(rstep < 0.3, `regions: no step out of them, half a metre at a time (largest ${rstep.toFixed(3)} m)`);
    // The GPU stands on the CPU's heights over them (within 2 m).
    standsOn(land, 0, 0, 'regions: under the eye');
    standsOn(land, 31, -17, 'regions: a level 0 band');
    standsOn(land, -120, -35, 'regions: further out');

    // The levels' relief (processes.regions.relief): each level's band a
    // tree, laid down by its regions (landRelief) and, wherever they are
    // not in, by the land's height itself (times channel 0's y), on the GPU
    // and the CPU alike: the land past the regions has the bands it has
    // under the eye, and nothing steps between the two.
    const RSEED = 9;
    const relief = [N('Multiply', { LHS: N('Simplex', { 'Feature Scale': 120 }), RHS: 6 }),
                    { noise: N('Multiply', { LHS: N('Simplex', { 'Feature Scale': 400 }), RHS: 3 }), seed: 4 }];
    land.setLand({ height: N('Constant', { Value: 0 }), seed: RSEED, surface: RED, processes: {
        resolution: CELLS,
        regions: { init: 'rinit', cells: 64, apron: 8, cell: 4, ratio: 4, levels: 2, reach: 1, keep: 1, conserve: [],
                   relief },
        source: `
void procInit(inout vec4 v[PROC_CHANNELS]) { v[0] = vec4(5.0, 1.0, 0.0, 0.0); }
void rinit(inout vec4 v[PROC_CHANNELS]) {
    vec4 c = procCoarse(0);
    int L = procLevel();
    vec3 p = procPoint();
    for (int k = L + 1; k < LAND_RELIEF_LEVELS; ++k) c.x += c.y * landReliefRest(k) * landRelief(k, p.xz);
    v[0] = vec4(c.x + c.y * landRelief(L, p.xz), c.y, 0.0, 0.0);
}
` } });
    assert(untilReady(land), `relief: ready (${JSON.stringify(land.landStats())})`);
    look(0, 40, 0);
    for (let i = 0; i < 400 && (!land.landStats().regions || land.landStats().regionsPending > 0); i++) frames(1);
    frames(10);
    const reliefAt = (x, z) => 5 + relief[0].genSingle2D(x, z, RSEED) + relief[1].noise.genSingle2D(x, z, 4);
    let fworst = 0, fat = '';
    for (const [x, z] of [[0, 0], [31, -17], [-60, 44], [900, 0], [-700, 800], [400, -950]]) {
        const e = Math.abs(land.landHeight(x, z) - reliefAt(x, z));
        if (e > fworst) { fworst = e; fat = `${x}, ${z}`; }
    }
    assert(fworst < 0.25, `relief: the CPU's heights are the bands under the eye and past the regions (worst ${fworst.toFixed(3)} m at ${fat})`);
    let fstep = 0, prevF = null;
    for (let x = 0; x <= 1000; x += 0.5) {
        const h = land.landHeight(x, 0.37 * x);
        if (prevF !== null) fstep = Math.max(fstep, Math.abs(h - prevF));
        prevF = h;
    }
    assert(fstep < 0.6, `relief: no step out of the regions, half a metre at a time (largest ${fstep.toFixed(3)} m)`);
    standsOn(land, 0, 0, 'relief: under the eye');
    standsOn(land, 900, 0, 'relief: past the regions');
    standsOn(land, -700, 800, 'relief: past the regions, elsewhere');

    // Conserving the height (the default): each level drifts from the
    // coarser field (+20 m on level 0, -10 m on level 1, as an erosion
    // lowers or a lift raises a whole region) and adds detail finer than the
    // coarser cell. The drift is taken out, the detail kept: the heights'
    // mean is the global grid's 5 m, and they still stand about it.
    land.setLand({ height: N('Constant', { Value: 0 }), surface: RED, processes: {
        resolution: CELLS,
        regions: { init: 'rinit', cells: 64, apron: 8, cell: 4, ratio: 4, levels: 2, reach: 1, keep: 1 },
        source: `
void procInit(inout vec4 v[PROC_CHANNELS]) { v[0].x = 5.0; }
void rinit(inout vec4 v[PROC_CHANNELS]) {
    vec3 p = procPoint();
    float detail = procLevel() == 0 ? 2.0 * sin(p.x / 3.2) * cos(p.z / 2.9) : 0.0;
    v[0] = vec4(procCoarse(0).x + (procLevel() == 0 ? 20.0 : -10.0) + detail, 1.0, 0.0, 0.0);
}
` } });
    assert(untilReady(land), `conserving regions: ready (${JSON.stringify(land.landStats())})`);
    look(0, 40, 0);
    for (let i = 0; i < 400 && (!land.landStats().regions || land.landStats().regionsPending > 0); i++) frames(1);
    frames(10);
    let cmean = 0, crms = 0, cn = 0;
    for (let z = -32; z <= 32; z += 0.5)
        for (let x = -32; x <= 32; x += 0.5) {
            const d = land.landHeight(x, z) - 5;
            cmean += d; crms += d * d; cn++;
        }
    cmean /= cn;
    crms = Math.sqrt(crms / cn);
    assert(Math.abs(cmean) < 0.5, `conserving regions: their mean is the coarser field's, the drift out (mean ${cmean.toFixed(3)} m over 5)`);
    assert(crms > 0.5, `conserving regions: their detail is kept (rms ${crms.toFixed(3)} m about 5)`);
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
    // (A cell is ~310 km of a field changing 300 m a radian: its B-spline
    // error is centimetres, its halo's nearest-cell copy at the edge a metre.)
    assert(err < 3, `across a face's edge the heights are procInit's field (worst ${err.toFixed(3)} m)`);
    assert(step < 1, `and step nowhere between points 2 km apart (largest ${step.toFixed(3)} m)`);
    for (const d of [d1, d2]) {
        const p = scn.toPlace([d[0] * R, d[1] * R, d[2] * R]);
        standsOn(round, p[0], p[2], `on the ${d === d1 ? '+X' : '+Y'} face by the edge`);
    }

    console.log('test_land_process: OK');
}
