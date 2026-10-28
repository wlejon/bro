// The turf on a round body (docs/scene-world-architecture.md §11, "The
// cover on the body"; scene_ground.glsl's groundSurface, scene_grass.glsl's
// coverTurfCells): the turf's patterns are the lattice cell's holding each
// point, in its flat frame, and its tiles are the land's chart's. Verifies,
// on an Earth-radius body, with a sward of no blades whose turf is a wave
// in the cell's x and z:
//   - the turf drawn 9.5 km from the anchor, in the next cell
//   - a re-anchor changes none of it
//   - where two cells meet there is no line: the turf across the edge
//     changes no faster than across a cell's middle

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};

const GROUND = `
LandSurface landSurface(LandPoint q) {
    LandSurface s = landSurfaceOf(vec3(0.2));
    s.cover = vec4(1.0, 0.0, 1.0, 0.0);
    return s;
}
`;

// No blades; the turf a wave across the cover's frame, green to white
// (faded to its mean where a pixel spans its wavelength, as a module's
// detail is: the turf refines the land by it over that mean).
const SWARD = {
    height: 0.35,
    source: `
void swardRoot(inout SwardCell s) {}
float swardDensity(inout SwardCell s) { return 0.0; }
SwardFarLook swardFarLook(SwardCell s, vec4 h) { return SwardFarLook(grassSwardHeight(), 0.0, vec3(0.1)); }
vec3 swardBake(vec2 xz, float fp) { return vec3(0.5); }
SwardTurf swardTurf(SwardTurfIn t) {
    float w = 0.5 + 0.45 * sin(t.xz.x * 0.647 + t.xz.y * 0.31) * clamp((10.0 - t.fp) / 5.0, 0.0, 1.0);
    vec3 c = mix(vec3(0.05, 0.5, 0.05), vec3(0.9), w);
    return SwardTurf(c, c, 1.0);
}
vec4 swardLay(vec2 xz, vec3 gn) { return vec4(gn, 0.0); }
#ifdef SWARD_BUILD
bool swardBuild(SwardCell s, int j, SwardRing r, vec3 v, inout SwardVertex o) { return false; }
#endif
#ifdef SWARD_SHADE
SwardShade swardShade(SwardFragment f) {
    return SwardShade(1.0, 0.0, 1e4, f.color, normalize(f.normal), f.roughness, -1.0, vec4(0.0, 1.0, 0.0, 0.0));
}
#endif
`,
};

function compare(a, b) {
    let over = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        let d = 0;
        for (let k = 0; k < 3; k++) d = Math.max(d, Math.abs(a.data[i + k] - b.data[i + k]));
        if (d > 8) over++;
    }
    return over;
}

// The image's columns' mean brightness, and the most it changes between
// neighbouring columns.
function columnStep(img) {
    const means = [];
    for (let x = 0; x < img.width; x++) {
        let s = 0;
        for (let y = 0; y < img.height; y++) {
            const i = (y * img.width + x) * 4;
            s += img.data[i] + img.data[i + 1] + img.data[i + 2];
        }
        means.push(s / (3 * img.height));
    }
    let step = 0, lo = 255, hi = 0;
    for (let x = 1; x < means.length; x++) step = Math.max(step, Math.abs(means[x] - means[x - 1]));
    for (const m of means) { lo = Math.min(lo, m); hi = Math.max(hi, m); }
    return { step, range: hi - lo };
}

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
    scn.setAmbient({ color: [0.4, 0.4, 0.4] });
    scn.createLight({ type: 'directional', direction: [0.2, -1, 0.1], color: [1, 1, 1], intensity: 2 });
    scn.setWind({ speed: 0, gust: 0 });
    const frames = (n = 3) => { flush(); for (let i = 0; i < n; i++) advanceTime(16); };

    const R = 6371000;
    const unit = (v) => { const l = Math.hypot(v[0], v[1], v[2]); return [v[0] / l, v[1] / l, v[2] / l]; };
    const along = (x, z) => unit([Math.tan(x / R), 1, Math.tan(z / R)]);
    scn.setBody({ radius: R, at: [0, R, 0] });
    const land = scn.createLand({ height: N('Constant', { Value: 0 }), surface: GROUND });
    const grass = scn.createGrass({ ground: land, turf: true, sward: SWARD });
    assert(grass, 'createGrass on a round body');
    const serial = scn.place.serial;

    // Straight down on the ground over body direction d from `up` m.
    const down = (d, up) => {
        const p = scn.toPlace([d[0] * R, d[1] * R, d[2] * R]);
        const u = scn.upAt(p);
        scn.setCamera({ fov: 50, near: 0.05, far: 5000, position: [p[0] + u[0] * up, p[1] + u[1] * up, p[2] + u[2] * up],
                        target: p, up: [0, 0, -1] });
        frames(12);
        return scn.captureFrame();
    };

    // --- the next cell's turf, and a re-anchor ------------------------------------
    const dFar = along(9500, 0);
    const before = down(dFar, 30);
    assert(scn.place.serial === serial, 'no re-anchor yet');
    const far = columnStep(before);
    assert(far.range > 15,`the turf's wave seen 9.5 km from the anchor (columns over ${far.range.toFixed(1)})`);
    assert(scn.reanchorAt([dFar[0] * R, dFar[1] * R, dFar[2] * R]), 're-anchored on the eye\'s cell');
    frames(12);
    const moved = compare(before, scn.captureFrame());
    assert(moved < S * S * 0.01, `the turf after a re-anchor is the turf before it (${moved} px differ)`);

    // --- where two cells meet -----------------------------------------------------
    // The face's middle cell's edge, half a step out along x, against the
    // next cell's middle, from 30 m: the columns' mean steps no more.
    const step = 0.5 * Math.PI / scn.place.faceCells;
    const mid = columnStep(down(along(R * step, 0), 30));
    const edge = columnStep(down(unit([Math.tan(0.5 * step), 1, 0]), 30));
    assert(edge.step < 1.5 * mid.step + 2,
           `no line where two cells meet (a column's step ${edge.step.toFixed(2)} there, ${mid.step.toFixed(2)} mid-cell)`);

    console.log(`test_turf_body: OK (range ${far.range.toFixed(1)}, re-anchor ${moved} px, ` +
                `step ${edge.step.toFixed(2)} at the edge / ${mid.step.toFixed(2)} mid-cell)`);
}
