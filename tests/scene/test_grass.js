// scene.createGrass: procedural blades over the land they stand on
// (scene/nodes/grass_node.h, vulkan/shaders/ground/scene_grass.glsl), as a
// sward module (SWARD below) builds them where the land's surface module
// (GROUND below) grows them. Verifies:
//   - the node and its params round trip; its ground must be a land
//   - blades are drawn: ring instances in grassStats, a grassy land greener
//     than the bare one, dry where the land says; a density of 0 grows nothing
//   - grass is left out of raycasts
//   - a calm wind keeps the frame still; a wind moves it
//   - movers attach and detach; a push bends the blades it covers

function patchAvg(img, x0, y0, x1, y1, ch) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) { sum += img.data[(y * img.width + x) * 4 + ch]; n++; }
    return n ? sum / n : 0;
}

function diffCount(a, b) {
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4)
        if (Math.abs(a.data[i] - b.data[i]) + Math.abs(a.data[i + 1] - b.data[i + 1]) + Math.abs(a.data[i + 2] - b.data[i + 2]) > 6) n++;
    return n;
}

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    return { canvas: cv, scene: cv.getContext('scene') };
}

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};

// The land's surface module (shaders/ground/scene_land_layer.glsl): grey
// ground, the sward's density landData(0).x, lush west of x = 0 and dry
// east of it.
const GROUND = `
LandSurface landSurface(LandPoint q) {
    LandSurface s = landSurfaceOf(vec3(0.2));
    s.cover = vec4(landData(0).x, q.p.x > 0.0 ? 1.0 : 0.0, 1.0, 0.0);
    return s;
}
`;

// A sward module (shaders/ground/scene_sward_layer.glsl): a blade a
// candidate as the land's density says, a strip bent by the wind and the
// movers' push, green where the land is lush and straw where it is dry.
const SWARD = {
    height: 0.35,
    data: new Float32Array([0.12, 0.4, 0.05, 0, 0.55, 0.42, 0.2, 0]),
    source: `
vec3 swColor(float dry) { return mix(swardData(0).rgb, swardData(1).rgb, clamp(dry, 0.0, 1.0)); }
void swardRoot(inout SwardCell s) {}
float swardDensity(inout SwardCell s) { return s.f.y; }
SwardFarLook swardFarLook(SwardCell s, vec4 h) { return SwardFarLook(grassSwardHeight(), 0.02, swColor(s.f.z)); }
vec3 swardBake(vec2 xz, float fp) { return vec3(0.5 + 0.5 * groundFbm(xz, 2.0, fp, 2), 0.5, 0.5); }
SwardTurf swardTurf(SwardTurfIn t) {
    vec3 tops = swColor(t.dry) * (0.6 + 0.8 * t.baked.x);
    return SwardTurf(tops, tops * 0.5, clamp(t.density, 0.0, 1.0));
}
vec4 swardLay(vec2 xz, vec3 gn) { return vec4(gn, 0.0); }
#ifdef SWARD_BUILD
bool swardBuild(SwardCell s, int j, SwardRing r, vec3 v, inout SwardVertex o) {
    vec2 xz = s.root + (vec2(grassHash(s.cell, j * 4), grassHash(s.cell, j * 4 + 2)) - 0.5) * r.spread;
    float len = grassSwardHeight() * s.f.w * (0.7 + 0.6 * grassHash(s.cell, j * 4 + 1));
    float trample;
    vec2 lie;
    vec2 bend = grassInfluence(xz, trample, lie) +
                camera.windField.xy * (0.02 * camera.windField.z + 0.5 * sceneWindGustAt(xz));
    vec3 eye = coverEye();
    float y0 = s.f.x;
    vec3 base = vec3(xz.x - eye.x, y0 - eye.y, xz.y - eye.z);
    vec3 side = normalize(cross(vec3(0.0, 1.0, 0.0), base) + vec3(1e-4, 0.0, 0.0));
    float w = max(r.width, length(base) * r.pixel * 0.65) * (1.0 - v.y);
    o.pos = base + side * (sign(v.x) * 0.5 * w) + vec3(bend.x * v.y * v.y, v.y, bend.y * v.y * v.y) * len;
    o.side = side;
    o.normal = normalize(cross(side, vec3(0.0, 1.0, 0.0)));
    o.up = s.ground;
    o.color = swColor(s.f.z) * mix(0.5, 1.0, v.y);
    o.ao = mix(0.5, 1.0, v.y);
    o.bulk = 0.0;
    o.far = 0.0;
    o.uv = vec2(sign(v.x) * 0.5, v.y);
    o.info = vec4(0.0);
    o.carries = false;
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

const S = 192;
const s = freshScene(S);
if (!s.scene) {
    missingGpuContext('scene');
} else {
    const scn = s.scene;
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.createLight({ type: 'directional', direction: [0.3, -1, 0.2], color: [1, 1, 1], intensity: 3 });
    const floor = scn.createLand({ height: N('Constant', { Value: 0.5 }), extent: 40, surface: GROUND,
                                   data: [1, 0, 0, 0] });
    scn.setCamera({ fov: 55, near: 0.05, far: 200, position: [0, 2.2, 6], target: [0, 0.5, -2] });
    scn.setWind({ speed: 0, gust: 0 });
    // The land's first tiles.
    const frames = (n = 3) => { flush(); for (let i = 0; i < n; i++) advanceTime(16); };
    frames(4);

    const bare = scn.captureFrame();

    // ---- The node ----
    let threw = false;
    const mesh = scn.createMesh({ mesh: 'plane', halfW: 1, halfD: 1 });
    mesh.visible = false;
    try { scn.createGrass({ ground: mesh, sward: SWARD }); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'createGrass on a ground that is not a land throws');

    const grass = scn.createGrass({ ground: floor, sward: SWARD });
    assert(grass && typeof grass.grassParams === 'function', 'createGrass returns a node');
    const p = grass.grassParams();
    assert(p.width > 0 && p.bladeDensity > 0 && p.range > 0, `params read back (${JSON.stringify(p)})`);
    grass.setGrass({ width: 0.005 });
    assert(Math.abs(grass.grassParams().width - 0.005) < 1e-5 && grass.grassParams().range === p.range,
        'setGrass changes what it names and keeps the rest');
    assert(floor.grassParams() === undefined, 'grass members answer undefined on a land');

    // ---- Drawn ----
    frames(2);
    const lush = scn.captureFrame();
    const st = grass.grassStats();
    assert(st && st.instances.length === 4 && st.instances[0] > 1000,
        `rings issue instances (${st && st.instances.join('/')})`);
    // Only cells the view can see are issued: looking up at the sky, next to
    // none (the box test is conservative at the frustum's corners).
    scn.setCamera({ position: [0, 2.2, 6], target: [0, 40, -2] });
    scn.captureFrame();
    const sky = grass.grassStats().instances;
    const sum = (a) => a.reduce((s, n) => s + n, 0);
    assert(sum(sky) < sum(st.instances) * 0.05, `looking at the sky issues next to no cells (${sky.join('/')} vs ${st.instances.join('/')})`);
    scn.setCamera({ fov: 55, near: 0.05, far: 200, position: [0, 2.2, 6], target: [0, 0.5, -2] });
    // Left half lush, right half sun-dried: more green on the left than the bare floor.
    const gBare = patchAvg(bare, 10, 120, 80, 180, 1) - patchAvg(bare, 10, 120, 80, 180, 2);
    const gLush = patchAvg(lush, 10, 120, 80, 180, 1) - patchAvg(lush, 10, 120, 80, 180, 2);
    assert(gLush > gBare + 6, `the lush half reads green (g-b ${gLush.toFixed(1)} vs bare ${gBare.toFixed(1)})`);
    const rDry = patchAvg(lush, 112, 120, 182, 180, 0) - patchAvg(lush, 112, 120, 182, 180, 2);
    const rLush = patchAvg(lush, 10, 120, 80, 180, 0) - patchAvg(lush, 10, 120, 80, 180, 2);
    assert(rDry > rLush, `the dry half is warmer (r-b ${rDry.toFixed(1)} vs ${rLush.toFixed(1)})`);

    // Not hit by rays.
    const hit = scn.raycast([0, 5, 2], [0, -1, 0]);
    assert(hit && hit.node === floor && Math.abs(hit.point[1] - 0.5) < 0.01,
        `raycasts pass through grass to the land (${hit && hit.point[1]})`);

    // ---- Still in a calm, moving in a wind ----
    advanceTime(200);
    const calm2 = scn.captureFrame();
    assert(diffCount(lush, calm2) === 0, `calm: nothing moves (${diffCount(lush, calm2)} px)`);
    scn.setWind({ direction: [1, 0, 0], speed: 5, gust: 1.5 });
    const w1 = scn.captureFrame();
    advanceTime(300);
    const w2 = scn.captureFrame();
    assert(diffCount(w1, w2) > 200, `wind: the blades move (${diffCount(w1, w2)} px)`);
    scn.setWind({ speed: 0, gust: 0 });

    // ---- Movers and pushes ----
    const ball = scn.createMesh({ mesh: 'sphere', radius: 0.3, color: [0.8, 0.2, 0.2] });
    ball.position = [0, 0.8, 0];
    grass.addMover(ball, { radius: 0.8, trample: 0.5 });
    assert(grass.grassStats().movers === 1, 'addMover tracks the node');
    grass.removeMover(ball);
    assert(grass.grassStats().movers === 0, 'removeMover stops');
    ball.visible = false;
    advanceTime(100);
    const before = scn.captureFrame();
    grass.push(0, 1, { radius: 1.5, vx: 3, strength: 1, trample: 1 });
    advanceTime(16);
    const after = scn.captureFrame();
    assert(diffCount(before, after) > 100, `a push bends the blades (${diffCount(before, after)} px)`);

    // ---- No density, no grass ----
    floor.setLand({ data: [0, 0, 0, 0] });
    frames(4);
    const none = scn.captureFrame();
    assert(diffCount(none, bare) < 40, `density 0 grows nothing (${diffCount(none, bare)} px differ from bare)`);

    if (globalThis.__grassShot) screenshot(globalThis.__grassShot);
}
