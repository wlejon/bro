// scene.expect (docs/scene-api.js; scene/world/expected_eyes.h): where the
// eye is going, its land regions made ahead of it.
// Verifies:
//   - an expected eye far from the eye is not ready at first, then is, and
//     the regions in grow by its
//   - the eye arriving there finds its regions in at once; arriving where
//     none was expected it waits on them
//   - two at once, a third throws; a cancelled one is not ready and its
//     handle refuses to move

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
    scn.createLight({ type: 'directional', direction: [0.3, -1, 0.2], color: [1, 1, 1], intensity: 3 });
    const frames = (n = 3) => { flush(); for (let i = 0; i < n; i++) advanceTime(16); };
    const look = (x, y, z) =>
        scn.setCamera({ fov: 50, near: 0.1, far: 5000, position: [x, y, z], target: [x, y - 10, z], up: [0, 0, -1] });
    const settle = () => {
        for (let i = 0; i < 600 && (!land.landStats().regions || land.landStats().regionsPending > 0); i++) frames(1);
    };
    look(0, 40, 0);
    const land = scn.createLand({ height: N('Constant', { Value: 0 }), surface: RED, processes: {
        resolution: 64,
        regions: { init: 'rinit', cells: 64, apron: 8, cell: 4, ratio: 4, levels: 2, reach: 1, keep: 1 },
        source: `
void procInit(inout vec4 v[PROC_CHANNELS]) { v[0].x = 5.0; }
void rinit(inout vec4 v[PROC_CHANNELS]) {
    vec3 p = procPoint();
    v[0] = vec4(procCoarse(0).x + 2.0 * sin(p.x / 3.2) * cos(p.z / 2.9), 1.0, 0.0, 0.0);
}
` } });
    for (let i = 0; i < 60 && !land.landStats().ready; i++) frames(1);
    settle();
    const before = land.landStats().regions;
    assert(before > 0 && land.landStats().regionsPending === 0, `the eye's regions are in (${JSON.stringify(land.landStats())})`);

    // Expected 1.5 km off: made while the eye stays.
    const there = [1500, 0, 0];
    const h = scn.expect(scn.toBody(there), { within: 10 });
    assert(!h.ready, `not ready before the land has made it (pending ${h.pending})`);
    let waited = 0;
    for (; waited < 600 && !h.ready; waited++) frames(1);
    assert(h.ready && h.pending === 0, `ready once made (${waited} frames; pending ${h.pending})`);
    assert(land.landStats().regions > before,
           `its regions are in beside the eye's (${land.landStats().regions} over ${before})`);

    // Two at once; a third throws.
    const h2 = scn.expect(scn.toBody([-1500, 0, 0]));
    let threw = false;
    try { scn.expect(scn.toBody([0, 0, 1500])); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, 'a third expected eye throws a RangeError');
    h2.cancel();
    assert(!h2.ready && h2.pending === -1, 'a cancelled one is not ready');
    let refused = false;
    try { h2.move(scn.toBody([0, 0, 0])); } catch (e) { refused = true; }
    assert(refused, 'a cancelled one refuses to move');

    // Arriving where it was expected: nothing pending.
    look(there[0], 40, there[2]);
    frames(1);
    const arrived = land.landStats().regionsPending;
    assert(arrived === 0, `arriving where expected, its regions are in (${arrived} pending)`);
    h.cancel();

    // Arriving where it was not: it waits on them.
    look(-3000, 40, 0);
    frames(1);
    const cold = land.landStats().regionsPending;
    assert(cold > 0, `arriving where not expected, regions are pending (${cold})`);
}
