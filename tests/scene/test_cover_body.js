// The cover on a round body (docs/scene-world-architecture.md §11, "The
// cover on the body"; scene/world/cover_regions.h, scene_grass.glsl's
// cover frame): a grass node's blades are laid out on the regions of the
// body's lattice and stood on the body. Verifies, on an Earth-radius body:
//   - the blades stand on the ground far from the place's anchor (9.5 km,
//     where the ground has fallen 7 m below the anchor's tangent plane): as
//     many seen from a standing eye there as at the anchor
//   - a re-anchor changes none of them: the frame after it is the frame
//     before it
//   - where two regions meet the blades are as thick as anywhere, on both
//     sides (no gap, no doubling)

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};

// Grey ground, the sward everywhere.
const GROUND = `
LandSurface landSurface(LandPoint q) {
    LandSurface s = landSurfaceOf(vec3(0.2));
    s.cover = vec4(1.0, 0.0, 1.0, 0.0);
    return s;
}
`;

// Red blades, a strip each, standing straight (no wind, no push).
const SWARD = {
    height: 0.35,
    source: `
void swardRoot(inout SwardCell s) {}
float swardDensity(inout SwardCell s) { return s.f.y; }
SwardFarLook swardFarLook(SwardCell s, vec4 h) { return SwardFarLook(grassSwardHeight(), 0.02, vec3(0.8, 0.05, 0.05)); }
vec3 swardBake(vec2 xz, float fp) { return vec3(0.5); }
SwardTurf swardTurf(SwardTurfIn t) { return SwardTurf(vec3(0.2), vec3(0.2), 0.0); }
vec4 swardLay(vec2 xz, vec3 gn) { return vec4(gn, 0.0); }
#ifdef SWARD_BUILD
bool swardBuild(SwardCell s, int j, SwardRing r, vec3 v, inout SwardVertex o) {
    vec2 xz = s.root + (vec2(grassHash(s.cell, j * 4), grassHash(s.cell, j * 4 + 2)) - 0.5) * r.spread;
    float len = grassSwardHeight() * (0.7 + 0.6 * grassHash(s.cell, j * 4 + 1));
    vec3 eye = coverEye();
    vec3 base = vec3(xz.x - eye.x, coverHeightAt(xz) - eye.y, xz.y - eye.z);
    vec3 side = normalize(cross(vec3(0.0, 1.0, 0.0), base) + vec3(1e-4, 0.0, 0.0));
    float w = max(r.width, length(base) * r.pixel * 0.65) * (1.0 - v.y);
    o.pos = base + side * (sign(v.x) * 0.5 * w) + vec3(0.0, v.y * len, 0.0);
    o.side = side;
    o.normal = normalize(cross(side, vec3(0.0, 1.0, 0.0)));
    o.up = s.ground;
    o.color = vec3(0.8, 0.05, 0.05);
    o.ao = 1.0;
    o.uv = vec2(sign(v.x) * 0.5, v.y);
    return true;
}
#endif
#ifdef SWARD_SHADE
SwardShade swardShade(SwardFragment f) {
    return SwardShade(1.0, 0.0, 1e4, f.color, normalize(f.normal), f.roughness, -1.0, vec4(0.0, 1.0, 0.0, 0.0));
}
#endif
`,
};

// The share of a capture's pixels in a rectangle that are a blade's red.
function redShare(img, x0, y0, x1, y1) {
    let n = 0, red = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) {
            const i = (y * img.width + x) * 4;
            const r = img.data[i], g = img.data[i + 1];
            if (r > 40 && r > 3 * g) red++;
            n++;
        }
    return n ? red / n : 0;
}

function compare(a, b) {
    let over = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        let d = 0;
        for (let k = 0; k < 3; k++) d = Math.max(d, Math.abs(a.data[i + k] - b.data[i + k]));
        if (d > 8) over++;
    }
    return over;
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
    scn.setAmbient({ color: [0.3, 0.3, 0.3] });
    scn.createLight({ type: 'directional', direction: [0.3, -1, 0.2], color: [1, 1, 1], intensity: 2 });
    scn.setWind({ speed: 0, gust: 0 });
    const frames = (n = 3) => { flush(); for (let i = 0; i < n; i++) advanceTime(16); };

    // Anchored on the cell in the middle of face +Y (its axes the body's
    // x and z): the place's origin is (0, R, 0).
    const R = 6371000;
    const unit = (v) => { const l = Math.hypot(v[0], v[1], v[2]); return [v[0] / l, v[1] / l, v[2] / l]; };
    const along = (x, z) => unit([Math.tan(x / R), 1, Math.tan(z / R)]);   // the body over ground (x, z) m from it
    scn.setBody({ radius: R, at: [0, R, 0] });
    const land = scn.createLand({ height: N('Constant', { Value: 0 }), surface: GROUND });
    const grass = scn.createGrass({ ground: land, sward: SWARD });
    assert(grass, 'createGrass on a round body');
    const serial = scn.place.serial;

    // A look at the ground over body direction d from `up` m over it:
    // standing `back` m behind it (along the place's +z) or straight down.
    const look = (d, up, back) => {
        const p = scn.toPlace([d[0] * R, d[1] * R, d[2] * R]);
        const u = scn.upAt(p);
        const eye = [p[0] + u[0] * up, p[1] + u[1] * up, p[2] + u[2] * up + back];
        scn.setCamera({ fov: 50, near: 0.05, far: 500, position: eye, target: p, up: back ? u : [0, 0, -1] });
        frames(8);
        return scn.captureFrame();
    };

    // --- standing on the ground far from the anchor ------------------------------
    // From 1.2 m, the ground 3 m ahead: blades fill the lower view.
    const near = redShare(look(along(0, 0), 1.2, 3), 0, S / 2, S, S);
    assert(near > 0.25, `at the anchor the blades fill the view's lower half (${near.toFixed(3)})`);
    // 9.5 km along x: the next cell's region, the ground 7 m under the
    // anchor's tangent plane.
    const dFar = along(9500, 0);
    const before = look(dFar, 1.2, 3);
    const far = redShare(before, 0, S / 2, S, S);
    assert(scn.place.serial === serial, 'no re-anchor yet');
    assert(far > 0.7 * near && far < 1.4 * near,
           `9.5 km from the anchor they stand on the ground as at it (${far.toFixed(3)} / ${near.toFixed(3)})`);

    // --- a re-anchor changes none of it -------------------------------------------
    assert(scn.reanchorAt([dFar[0] * R, dFar[1] * R, dFar[2] * R]), 're-anchored on the eye\'s cell');
    frames(8);
    const moved = compare(before, scn.captureFrame());
    assert(moved < S * S * 0.01, `the blades after a re-anchor are the blades before it (${moved} px differ)`);

    // --- where two regions meet ---------------------------------------------------
    // The face's cells are equal angles, the middle one centred on the
    // face's: its edge is half a step out along x. Looking straight down on
    // it from 4 m, the blades are as thick either side as over a region's
    // middle.
    const faceCells = scn.place.faceCells;
    assert(faceCells % 2 === 1, `an odd count of cells a face (${faceCells})`);
    const step = 0.5 * Math.PI / faceCells;
    const mid = redShare(look(along(R * step, 0), 4, 0), 0, 0, S, S);
    const seam = look(unit([Math.tan(0.5 * step), 1, 0]), 4, 0);
    const left = redShare(seam, 0, 0, S / 2, S), right = redShare(seam, S / 2, 0, S, S);
    assert(mid > 0.2, `blades seen from 4 m (${mid.toFixed(3)})`);
    for (const [name, v] of [['one side', left], ['the other', right]])
        assert(v > 0.75 * mid && v < 1.33 * mid,
               `at a region's edge, ${name} as thick as a region's middle (${v.toFixed(3)} / ${mid.toFixed(3)})`);

    console.log(`test_cover_body: OK (standing ${near.toFixed(3)} / ${far.toFixed(3)}, re-anchor ${moved} px, ` +
                `edge ${left.toFixed(3)} | ${right.toFixed(3)} of ${mid.toFixed(3)})`);
}
