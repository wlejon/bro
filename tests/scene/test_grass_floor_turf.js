// The grass node's forest floor and turf (scene/nodes/grass_node.h setFloor,
// vulkan/shaders/ground/scene_ground.glsl). Verifies:
//   - createGrass's `herbs` grow low plants among (and without) the grass,
//     as a herb module says (shaders/ground/scene_herb_layer.glsl; HERBS
//     below, a green leaf a cell): with no grass anywhere, a closed floor
//     still issues herb cells and draws green leaves; a herb density of 0
//     grows nothing; a module that does not compile draws no herbs
//   - `turf` / shadeGround shade a ground mesh as the grass's ground: the
//     sward where the grass grows (green, textured; SWARD below, a sward
//     module of turf alone), the litter where the
//     floor is closed (brown, textured), and shadeGround(mesh, false) gives
//     the mesh its own colour back

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

// A herb module: in a cell as the floor's density says, one upright leaf a
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
// where the field grows grass, its tops mottled; it builds no blades.
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
    const floor = scn.createMesh({ mesh: 'plane', halfW: 30, halfD: 30, color: [0.45, 0.45, 0.45] });
    scn.setCamera({ fov: 55, near: 0.05, far: 200, position: [0, 1.6, 5], target: [0, 0, -1] });
    scn.setWind({ speed: 0, gust: 0 });
    const bare = scn.captureFrame();

    // ---- Low plants on the floor, no grass ----
    const grass = scn.createGrass({ ground: floor, cell: 0.5, density: 0, floor: 1,
                                    herbs: { ...HERBS, density: 1 } });
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
    grass.setGround({ ground: floor, cell: 0.5, density: 0, floor: 0, herbs: { ...HERBS, density: 0 } });
    const none = scn.captureFrame();
    assert(diffCount(none, bare) < 40, `no herb density and no grass grows nothing (${diffCount(none, bare)} px)`);
    // A module that does not compile: logged, and the node draws no herbs.
    grass.setGround({ ground: floor, cell: 0.5, density: 0, floor: 1,
                      herbs: { ...HERBS, source: HERBS.source + '\nint herbBroken() { return undefinedThing; }\n', density: 1 } });
    const broken = scn.captureFrame();
    const bst = grass.grassStats();
    assert((bst.herbInstances || []).every((c) => c === 0), `a module that does not compile draws no herbs (${(bst.herbInstances || []).join('/')})`);
    assert(diffCount(broken, bare) < 40, `nor any pixel of them (${diffCount(broken, bare)} px)`);

    // ---- Turf ----
    // Grass to the left, a closed canopy's floor to the right, shaded on the
    // ground; the camera high enough that the ground shows.
    grass.setSward(SWARD);
    grass.setGround({ ground: floor, cell: 0.5, density: (x) => (x < 0 ? 1 : 0), floor: (x) => (x < 0 ? 0 : 1), turf: true });
    scn.setCamera({ fov: 55, near: 0.05, far: 300, position: [0, 30, 20], target: [0, 0, -10] });
    const turf = scn.captureFrame();
    const sward = patchStats(turf, 10, 90, 80, 150), litter = patchStats(turf, 112, 90, 182, 150);
    assert(sward.g > sward.b + 8 && sward.g > sward.r * 0.9, `the turf under grass is green (${sward.r.toFixed(0)}, ${sward.g.toFixed(0)}, ${sward.b.toFixed(0)})`);
    // (Seen from above under a high sun the sward shows its blades' shaded
    // depths, sceneSwardLight; the litter lies level in the sun. In this dead
    // calm the sward stands unlaid, its darkest: about 1.6 x under the litter.)
    assert(litter.r > litter.b * 1.3 && litter.lum < sward.lum * 1.8, `the floor under a canopy is brown litter (${litter.r.toFixed(0)}, ${litter.g.toFixed(0)}, ${litter.b.toFixed(0)})`);
    assert(sward.sd > 0.08 * sward.lum && litter.sd > 0.08 * litter.lum,
        `both are textured (sd ${sward.sd.toFixed(2)} of ${sward.lum.toFixed(1)}, ${litter.sd.toFixed(2)} of ${litter.lum.toFixed(1)})`);
    grass.shadeGround(floor, false);
    const plain = scn.captureFrame();
    const back = patchStats(plain, 112, 90, 182, 150);
    assert(Math.abs(back.r - back.b) < 4 && back.sd < sward.sd, `shadeGround(mesh, false) gives the mesh its own colour (${back.r.toFixed(0)}, ${back.b.toFixed(0)}, sd ${back.sd.toFixed(2)})`);
    if (globalThis.__turfShot) screenshot(globalThis.__turfShot);
}
