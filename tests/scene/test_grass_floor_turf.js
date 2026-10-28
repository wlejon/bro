// The grass node's forest floor and turf (the land's cover and herbs,
// scene_land_layer.glsl; vulkan/shaders/ground/scene_ground.glsl). The land
// below (GROUND) grows what its data says: landData(0) the sward's density
// west and east of x = 0 and the canopy west and east, landData(1).x the
// herbs. Verifies:
//   - createGrass's `herbs` grow low plants among (and without) the grass,
//     as a herb module says (shaders/ground/scene_herb_layer.glsl; HERBS
//     below, a green leaf a cell): with no grass anywhere, a closed floor
//     still issues herb cells and draws green leaves; a herb density of 0
//     grows nothing; a module that does not compile draws no herbs
//   - `turf` / shadeGround shade the land as the grass's ground: the sward
//     where the grass grows (green, textured; SWARD below, a sward module of
//     turf alone), the litter where the floor is closed (brown, textured),
//     each refining the land's colour (a sward's green, litter's brown)
//     with no mean of its own, and shadeGround(land, false) gives the land
//     its own colour back, untextured, as light as its turf was

function patchStats(img, x0, y0, x1, y1) {
    let r = 0, g = 0, b = 0, l = 0, l2 = 0, n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) {
            const i = (y * img.width + x) * 4;
            const lum = 0.3 * img.data[i] + 0.59 * img.data[i + 1] + 0.11 * img.data[i + 2];
            r += img.data[i]; g += img.data[i + 1]; b += img.data[i + 2];
            l += lum; l2 += lum * lum; n++;
        }
    const m = l / n;
    return { r: r / n, g: g / n, b: b / n, lum: m, sd: Math.sqrt(Math.max(l2 / n - m * m, 0)) };
}

function diffCount(a, b) {
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4)
        if (Math.abs(a.data[i] - b.data[i]) + Math.abs(a.data[i + 1] - b.data[i + 1]) + Math.abs(a.data[i + 2] - b.data[i + 2]) > 6) n++;
    return n;
}

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};

const GROUND = `
LandSurface landSurface(LandPoint q) {
    vec4 d = landData(0);
    bool west = q.p.x < 0.0;
    LandSurface s = landSurfaceOf(mix(vec3(0.45), west ? vec3(0.08, 0.15, 0.035) : vec3(0.1, 0.07, 0.045), landData(1).y));
    s.cover = vec4(west ? d.x : d.y, 0.0, 1.0, west ? d.z : d.w);
    s.herbs = landData(1).x;
    return s;
}
`;
// The land's data: the sward's density west and east, the canopy west and
// east, the herbs, and whether its colour is the ground's (a sward's green
// west, litter's brown east) rather than grey.
const cover = (densW, densE, canW, canE, herbs, tinted = 0) => [densW, densE, canW, canE, herbs, tinted, 0, 0];

// A herb module: in a cell as the land's herb density says, one upright leaf a
// plant (a card of the ring's), green; from afar a green cover.
const HERBS = {
    height: 0.2,
    data: new Float32Array([0.2, 0.55, 0.12, 0]),
    source: `
int herbStand(ivec2 cell, vec2 xz, vec2 floorAt) { return grassHash(cell, 105) < floorAt.y ? 0 : -1; }
HerbMix herbFarMean(float canopy, float density, float tanView) {
    return HerbMix(0.6 * density, herbData(0).rgb, vec3(0.0), 0.0);
}
float herbFarLocal(vec2 xz, float canopy, float u, float fp, out vec3 bloom, out vec3 pick) {
    bloom = vec3(0.0);
    pick = vec3(0.0);
    return 0.0;
}
HerbFarLook herbFarLook(int variety, vec4 h) {
    return HerbFarLook(0.2, 0.15, 1.0, herbData(0).rgb, vec3(0.0), 0.0);
}
#ifdef HERB_BUILD
bool herbBuild(HerbPlant p, int card, int cards, int verts, vec3 v, inout HerbVertex o) {
    if (card != 0) return false;
    vec3 side = normalize(cross(vec3(0.0, 1.0, 0.0), -p.root) + vec3(1e-4, 0.0, 0.0));
    o.pos = p.root + side * (v.x * 0.12) + vec3(0.0, v.y * 0.2, 0.0);
    o.side = side;
    o.normal = normalize(cross(side, vec3(0.0, 1.0, 0.0)));
    o.uv = vec2(v.x * 2.0, v.y);
    o.seed = p.h.z;
    return true;
}
#endif
#ifdef HERB_SHADE
HerbShade herbShade(HerbFragment f) {
    vec3 n = normalize(f.normal);
    if (dot(n, -f.pos) < 0.0) n = -n;
    return HerbShade(1.0, herbData(0).rgb, n, 0.6, 1.0, 1.0, -1.0, vec4(0.0));
}
#endif
`,
};

// A sward module (shaders/ground/scene_sward_layer.glsl) for the turf: green
// where the land grows grass, its tops mottled; it builds no blades.
const SWARD = {
    data: new Float32Array([0.12, 0.4, 0.05, 0]),
    source: `
void swardRoot(inout SwardCell s) {}
float swardDensity(inout SwardCell s) { return s.f.y; }
SwardFarLook swardFarLook(SwardCell s, vec4 h) { return SwardFarLook(0.0, 0.0, swardData(0).rgb); }
vec3 swardBake(vec2 xz, float fp) { return vec3(0.5 + 0.5 * groundFbm(xz, 2.0, fp, 2), 0.5, 0.5); }
SwardTurf swardTurf(SwardTurfIn t) {
    vec3 tops = swardData(0).rgb * (0.6 + 0.8 * t.baked.x);
    return SwardTurf(tops, tops * 0.5, clamp(t.density, 0.0, 1.0));
}
vec4 swardLay(vec2 xz, vec3 gn) { return vec4(gn, 0.0); }
#ifdef SWARD_BUILD
bool swardBuild(SwardCell s, int j, SwardRing r, vec3 v, inout SwardVertex o) { return false; }
#endif
#ifdef SWARD_SHADE
SwardShade swardShade(SwardFragment f) {
    return SwardShade(0.0, 0.0, 1e4, f.color, normalize(f.normal), f.roughness, 0.0, vec4(0.0));
}
#endif
`,
};

const S = 192;
const cv = document.createElement('canvas');
cv.setAttribute('width', String(S));
cv.setAttribute('height', String(S));
document.body.appendChild(cv);
flush();
const scn = cv.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.createLight({ type: 'directional', direction: [0.3, -1, 0.2], color: [1, 1, 1], intensity: 8 });
    const floor = scn.createLand({ height: N('Constant', { Value: 0 }), extent: 60, surface: GROUND,
                                   data: cover(0, 0, 1, 1, 0) });
    scn.setCamera({ fov: 55, near: 0.05, far: 200, position: [0, 1.6, 5], target: [0, 0, -1] });
    scn.setWind({ speed: 0, gust: 0 });
    const frames = (n = 4) => { flush(); for (let i = 0; i < n; i++) advanceTime(16); };
    frames();
    const bare = scn.captureFrame();

    // ---- Low plants on the floor, no grass ----
    floor.setLand({ data: cover(0, 0, 1, 1, 1) });
    const grass = scn.createGrass({ ground: floor, herbs: HERBS });
    frames();
    const plants = scn.captureFrame();
    const st = grass.grassStats();
    const herbCells = (st.herbInstances || []).reduce((a, b) => a + b, 0);
    assert(herbCells > 100, `a closed floor's herbs issue cells with no grass (${(st.herbInstances || []).join('/')})`);
    // (They grow in patches: judge the pixels they cover.)
    let gb = 0, gbBare = 0, covered = 0;
    for (let i = 0; i < plants.data.length; i += 4) {
        const d = Math.abs(plants.data[i] - bare.data[i]) + Math.abs(plants.data[i + 1] - bare.data[i + 1]) +
                  Math.abs(plants.data[i + 2] - bare.data[i + 2]);
        if (d <= 6) continue;
        gb += plants.data[i + 1] - plants.data[i + 2];
        gbBare += bare.data[i + 1] - bare.data[i + 2];
        covered++;
    }
    assert(covered > 200, `low plants are drawn (${covered} px)`);
    assert(gb / covered > gbBare / covered + 3, `they are green (g-b ${(gb / covered).toFixed(1)} vs ${(gbBare / covered).toFixed(1)})`);
    floor.setLand({ data: cover(0, 0, 1, 1, 0) });
    frames();
    const none = scn.captureFrame();
    assert(diffCount(none, bare) < 40, `no herb density and no grass grows nothing (${diffCount(none, bare)} px)`);
    // A module that does not compile: logged, and the node draws no herbs.
    floor.setLand({ data: cover(0, 0, 1, 1, 1) });
    grass.setHerbs({ ...HERBS, source: HERBS.source + '\nint herbBroken() { return undefinedThing; }\n' });
    frames();
    const broken = scn.captureFrame();
    const bst = grass.grassStats();
    assert((bst.herbInstances || []).every((c) => c === 0), `a module that does not compile draws no herbs (${(bst.herbInstances || []).join('/')})`);
    assert(diffCount(broken, bare) < 40, `nor any pixel of them (${diffCount(broken, bare)} px)`);

    // ---- Turf ----
    // Grass to the left, a closed canopy's floor to the right, shaded on the
    // ground; the camera high enough that the ground shows.
    grass.setHerbs({ source: '' });
    grass.setSward(SWARD);
    floor.setLand({ data: cover(1, 0, 0, 1, 0, 1) });
    grass.shadeGround(floor);
    scn.setCamera({ fov: 55, near: 0.05, far: 300, position: [0, 30, 20], target: [0, 0, -10] });
    frames(8);
    const turf = scn.captureFrame();
    const sward = patchStats(turf, 10, 90, 80, 150), litter = patchStats(turf, 112, 90, 182, 150);
    assert(sward.g > sward.b + 8 && sward.g > sward.r * 0.9, `the turf under grass is green (${sward.r.toFixed(0)}, ${sward.g.toFixed(0)}, ${sward.b.toFixed(0)})`);
    // (Seen from above under a high sun the sward shows its blades' shaded
    // depths, sceneSwardLight; the litter lies level in the sun. In this dead
    // calm the sward stands unlaid, its darkest: about 1.6 x under the litter.)
    assert(litter.r > litter.b * 1.3 && litter.lum < sward.lum * 1.8, `the floor under a canopy is brown litter (${litter.r.toFixed(0)}, ${litter.g.toFixed(0)}, ${litter.b.toFixed(0)})`);
    // (Its tiles band-limit the patterns to their texel: at this canvas's
    // pixel the sward's mottling is about 6% of its light, the litter's 20%.)
    assert(sward.sd > 0.045 * sward.lum && litter.sd > 0.12 * litter.lum,
        `both are textured (sd ${sward.sd.toFixed(2)} of ${sward.lum.toFixed(1)}, ${litter.sd.toFixed(2)} of ${litter.lum.toFixed(1)})`);
    grass.shadeGround(floor, false);
    frames();
    const plain = scn.captureFrame();
    const back = patchStats(plain, 112, 90, 182, 150);
    // (The litter's leaves near the eye lie in each other's shade, which its
    // far look does not hold: 42 against the land's 53.5, a third of a stop.)
    assert(back.sd < 0.5 * litter.sd && Math.abs(back.lum - litter.lum) < 0.35 * litter.lum,
        `shadeGround(mesh, false) gives the mesh its own colour, untextured, as light as its turf (lum ${back.lum.toFixed(1)} vs ${litter.lum.toFixed(1)}, sd ${back.sd.toFixed(2)} vs ${litter.sd.toFixed(2)})`);
    if (globalThis.__turfShot) screenshot(globalThis.__turfShot);
}
