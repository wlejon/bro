// scene.createLand: a world's ground from a FastNoise height tree and a
// surface module (scene/nodes/land_node.h, vulkan/ground/scene_mesh_drawer_land.cpp,
// docs/land-api.js).
// Verifies:
//   - the CPU's heights are FastNoise2's on the tree: landHeight against
//     genSingle2D, sampleHeights against landHeight, NaN off the land
//   - a raycast down meets the land at its height, with an upward normal
//   - without a height it throws; a surface that does not compile draws
//     nothing (stats.compiled false) and the frame keeps its clear colour
//   - the land draws: from above, the frame is the surface's colour, its
//     tiles made (resident, levels) by the world service
//   - the GPU's heights are the tree's: a land at a constant 50 m is seen
//     from 52 m looking down and not from 48 m
//   - far from the origin (the rings follow the eye) it still draws

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
    const down = (x, y, z) => scn.setCamera({ fov: 50, near: 0.1, far: 5000, position: [x, y, z],
                                              target: [x, y - 10, z], up: [0, 0, -1] });

    // --- heights on the CPU ---------------------------------------------------
    const SEED = 1337;
    const hills = N('Multiply', { LHS: N('FractalFBm', { Source: N('Perlin', { 'Feature Scale': 300 }), Octaves: 4 }),
                                  RHS: 40 });
    let threw = false;
    try { scn.createLand({ extent: 100 }); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'createLand without a height throws');

    const land = scn.createLand({ height: hills, seed: SEED, extent: 1024, surface: RED });
    assert(land && typeof land.landHeight === 'function', 'createLand returns a land node');
    let worst = 0;
    for (const [x, z] of [[0, 0], [12.5, -40], [-300, 220], [700, 700], [-1000, 3]])
        worst = Math.max(worst, Math.abs(land.landHeight(x, z) - hills.genSingle2D(x, z, SEED)));
    assert(worst < 1e-4, `landHeight is FastNoise2's on the tree (worst ${worst})`);
    assert(Number.isNaN(land.landHeight(1100, 0)), 'off the land, NaN');
    const grid = land.sampleHeights(-20, -20, 10, 5, 5);
    let gridWorst = 0;
    for (let j = 0; j < 5; j++)
        for (let i = 0; i < 5; i++)
            gridWorst = Math.max(gridWorst, Math.abs(grid[j * 5 + i] - land.landHeight(-20 + i * 10, -20 + j * 10)));
    assert(grid.length === 25 && gridWorst < 1e-3, `sampleHeights are landHeight's (worst ${gridWorst})`);
    const p = land.landParams();
    assert(p.extent === 1024 && p.resolution === 256 && p.levels === 10, `default rings (${JSON.stringify(p)})`);

    // --- a raycast meets it -----------------------------------------------------
    const hit = scn.raycast([30, 200, -50], [0, -1, 0]);
    const h = land.landHeight(30, -50);
    assert(hit && hit.node === land, 'a ray down hits the land');
    assert(Math.abs(hit.point[1] - h) < 0.05, `at its height (${hit.point[1].toFixed(3)} vs ${h.toFixed(3)})`);
    assert(hit.normal[1] > 0.5, 'its normal points up');

    // --- it draws -------------------------------------------------------------
    down(0, h + 150, 0);
    frames(4);
    let st = land.landStats();
    assert(st.compiled && st.resident > 0 && st.levels > 0, `tiles made (${JSON.stringify(st)})`);
    let img = scn.captureFrame();
    const r = patchAvg(img, 16, 16, S - 16, S - 16, 0), g = patchAvg(img, 16, 16, S - 16, S - 16, 1);
    assert(r > 40 && r > 4 * g, `from above, the surface's red (r ${r.toFixed(1)} g ${g.toFixed(1)})`);

    // --- a surface that does not compile draws nothing ---------------------------
    land.setLand({ surface: 'this is not GLSL' });
    frames(4);
    st = land.landStats();
    assert(!st.compiled, 'a module that does not compile is not compiled');
    img = scn.captureFrame();
    const rBad = patchAvg(img, 16, 16, S - 16, S - 16, 0);
    assert(rBad < r / 4, `and nothing is drawn (r ${rBad.toFixed(1)})`);

    // --- the GPU's heights: a land at 50 m ---------------------------------------
    land.setLand({ height: N('Constant', { Value: 50 }), surface: RED });
    down(10, 52, 10);
    frames(4);
    img = scn.captureFrame();
    const above = patchAvg(img, 16, 16, S - 16, S - 16, 0);
    down(10, 48, 10);
    frames(4);
    img = scn.captureFrame();
    const below = patchAvg(img, 16, 16, S - 16, S - 16, 0);
    assert(above > 40 && below < above / 4, `seen from 52 m, not from 48 m (${above.toFixed(1)} / ${below.toFixed(1)})`);

    // --- far from the origin ------------------------------------------------------
    down(900, 120, -850);
    frames(6);
    img = scn.captureFrame();
    const far = patchAvg(img, 16, 16, S - 16, S - 16, 0);
    assert(far > 40, `far from the origin it still draws (r ${far.toFixed(1)})`);

    console.log('test_land: OK');
}
