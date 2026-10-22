// The body and its place frame (scene/world/place.h, SceneGraph::setBody):
// a scene on a flat body re-anchors to the 16 km lattice cell under the eye
// and nothing on screen steps.
// Verifies:
//   - setBody gives a place with no move: the origin is the anchor cell's
//     corner, toBody / toPlace round-trip
//   - reanchorAt moves the scene's nodes and camera into the new frame (the
//     `reanchor` event carries the move), the land's heights stay the
//     body's, and the frame after renders as the frame before; the shadow
//     pages kept on the body are not redrawn
//   - the eye walking past the cell's margin re-anchors on its own

function compare(a, b) {
    let maxDiff = 0, over = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        let d = 0;
        for (let k = 0; k < 3; k++) d = Math.max(d, Math.abs(a.data[i + k] - b.data[i + k]));
        maxDiff = Math.max(maxDiff, d);
        if (d > 3) over++;
    }
    return { maxDiff, over };
}

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};
// Stripes keyed on the body: a land drawn in the wrong frame shows them shifted.
const STRIPES = `LandSurface landSurface(LandPoint q) {
    float s = step(0.5, fract(q.p.x / 6.0)) * 0.6 + step(0.5, fract(q.p.y / 9.0)) * 0.3;
    return landSurfaceOf(vec3(0.15 + s, 0.4, 0.2 + 0.5 * s));
}`;

const S = 160;
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
    scn.setAmbient({ color: [0.15, 0.15, 0.15] });
    scn.createLight({ type: 'directional', direction: [0.3, -1, 0.2], color: [1, 1, 1], intensity: 3 });
    const SEED = 7;
    const hills = N('Multiply', { LHS: N('FractalFBm', { Source: N('Perlin', { 'Feature Scale': 200 }), Octaves: 3 }),
                                  RHS: 12 });
    const land = scn.createLand({ height: hills, seed: SEED, extent: 60000, surface: STRIPES });

    // --- the body ---------------------------------------------------------------
    const none = scn.place;
    assert(none && !none.hasBody, 'without a body, no place');
    scn.setBody({ at: [0, 0, 0] });
    const p0 = scn.place;
    assert(p0.hasBody && p0.body.radius === Infinity && p0.lattice === 16384, 'a flat body');
    assert(p0.origin[0] === 0 && p0.origin[2] === 0, `anchored at the origin's cell (${p0.origin})`);
    const rt = scn.toPlace(scn.toBody([123.25, 4, -77.5]));
    assert(rt[0] === 123.25 && rt[1] === 4 && rt[2] === -77.5, `toBody / toPlace round-trip (${rt})`);

    // --- a scene near the cell's margin ---------------------------------------
    const X = 9000, Z = 9030;
    const ground = land.landHeight(X, Z);
    const box = scn.createMesh({ mesh: Mesh.box(1, 1, 1), color: [0.9, 0.2, 0.1, 1], roughness: 0.5 });
    box.x = X + 2; box.y = ground + 1; box.z = Z;
    const view = (x, z) => scn.setCamera({ fov: 55, near: 0.1, far: 4000, position: [x - 6, ground + 9, z + 14],
                                          target: [x, ground, z], up: [0, 1, 0] });
    const settle = () => {
        for (let i = 0; i < 12; i++) {
            advanceTime(16);
            scn.captureFrame();
            const st = scn.cullStats();
            if (i >= 3 && st.shadowPagesWanted === 0 && st.shadowPagesDrawn === 0) break;
        }
        return scn.captureFrame();
    };
    view(X, Z);
    flush();
    settle();
    const before = settle();

    const moves = [];
    scn.addEventListener('reanchor', (e) => moves.push(e));
    const eyeBody = scn.toBody([X - 6, ground + 9, Z + 14]);
    assert(scn.reanchorAt(eyeBody) === true, 'reanchorAt takes the eye\'s cell');
    const p1 = scn.place;
    assert(p1.origin[0] === 16384 && p1.origin[2] === 16384 && p1.serial > p0.serial,
           `the place moved a cell (${p1.origin}, serial ${p1.serial})`);
    assert(moves.length === 1 && !moves[0].tilts &&
           moves[0].translation[0] === -16384 && moves[0].translation[2] === -16384,
           `the reanchor event carries the move (${moves.length ? moves[0].translation : 'none'})`);
    assert(box.x === X + 2 - 16384 && box.z === Z - 16384, `the nodes moved with it (${box.x}, ${box.z})`);
    assert(land.landHeight(X - 16384, Z - 16384) === ground, 'the land\'s heights are the body\'s');

    advanceTime(16);
    scn.captureFrame();
    const st = scn.cullStats();
    assert(st.shadowPagesDrawn === 0, `the shadow pages stay (${st.shadowPagesDrawn} drawn)`);
    const after = settle();
    const c = compare(before, after);
    console.log(`place: re-anchor: max diff ${c.maxDiff}, ${c.over} px over 3`);
    assert(c.over <= 2 && c.maxDiff <= 8, `the frame does not step (${c.over} px differ by more than 3, max ${c.maxDiff})`);

    // --- walking on: past the margin it re-anchors itself -----------------------
    // (The eye at place x 10300 is past 8192 + 2048 of the cell's centre.)
    view(10300 + 6, 0);
    advanceTime(16);
    const p2 = scn.place;
    assert(moves.length === 2 && p2.origin[0] === 32768 && p2.origin[2] === 16384,
           `the eye past the margin re-anchors (${p2.origin}, ${moves.length} moves)`);

    document.body.removeChild(canvas);
    flush();
}
