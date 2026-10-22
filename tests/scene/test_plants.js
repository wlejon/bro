// scene.createPlants: plants built on the GPU from a species table and their
// records (scene/nodes/plants_node.h, vulkan/shaders/foliage/scene_plants.glsl),
// planned on the GPU over the world cells in view
// (vulkan/foliage/scene_plants_gpu_plan.cpp).
// The plants are an inline module's (PLANT_MODULE).
// Verifies:
//   - without a module, or with one that does not compile, nothing is drawn
//   - the node draws: a tree in view turns its patch of the frame green, its
//     elements and limbs counted in plantStats
//   - the detail falls with distance (fewer elements far off), never under
//     minElements, and a plant out of view or past `range` draws nothing
//   - setPlants replaces the records; params set through setPlants change
//     the count
//   - casting: the shadow tiles' casters are counted, and the ground under a
//     tree is darker than beside it
//   - many plants: 200k over 4 km, the camera's count between the plants
//     surely in its horizontal field and those that may be
//
// The stats are the plan's, read back from the GPU a frame or two late:
// drawn() draws a few frames before it reads them.

function patchAvg(img, x0, y0, x1, y1, ch) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) { sum += img.data[(y * img.width + x) * 4 + ch]; n++; }
    return n ? sum / n : 0;
}

// A plant module (shaders/foliage/scene_plant_layer.glsl): lobes round the
// crown's envelope on a spiral, an element on a lobe's skin facing out, a
// limb from the trunk to each lobe. plantData(s, 0) its green, 1.xyz its
// bark, 1.w its lobes' size.
const PLANT_MODULE = `
vec4 tLobe(PlantSpecies s, PlantCrown c, int j) {
    float n = float(max(s.limbs, 1));
    float v = 1.0 - 2.0 * (float(j) + 0.5) / n;
    float a = float(j) * 2.39996323;
    float r = plantData(s.index, 1).w * c.radii.x;
    float w = plantCrownWidth(c, v) * c.radii.x - r;
    return vec4(c.center + vec3(cos(a) * w, v * (c.radii.y - r), sin(a) * w), r);
}
vec3 plantSway(Plant p, PlantSpecies s, float up, float lag) { return vec3(0.0); }
vec2 plantLeafPoint(int species, int c) {
    const vec2 pts[6] = vec2[6](vec2(0.0, 0.0), vec2(-0.48, 0.32), vec2(0.48, 0.32),
                                vec2(-0.36, 0.72), vec2(0.36, 0.72), vec2(0.0, 1.0));
    return pts[clamp(c, 0, 5)];
}
#ifdef PLANT_BUILD
bool plantElement(Plant p, PlantSpecies s, PlantCrown c, PlantElementIn k, inout PlantElement e) {
    vec4 lobe = tLobe(s, c, k.k % max(s.limbs, 1));
    uint ks = p.seed * 0x2c1b3c6du + uint(k.k) * 0x297a2d39u;
    vec3 d = plantSphere(plantRand(ks, 1u), plantRand(ks, 2u));
    vec3 n = plantDirToWorld(p, d);
    vec3 along = abs(n.y) < 0.9 ? normalize(cross(n, vec3(0.0, 1.0, 0.0))) : vec3(1.0, 0.0, 0.0);
    float size = s.leafLen * p.scale * k.grow * k.last;
    e.base = plantToWorld(p, lobe.xyz + d * lobe.w) - camera.eyeWorld.xyz - along * 0.5 * size;
    e.along = along * size;
    e.across = cross(n, along) * size * 0.6;
    e.leafNormal = n;
    e.crownNormal = n;
    e.albedo = plantData(s.index, 0).rgb;
    e.crownCenter = plantToWorld(p, c.center) - camera.eyeWorld.xyz;
    e.far = clamp((k.grow - 1.0) / 6.0, 0.0, 1.0);
    e.depth = 0.0;
    return true;
}
#endif
bool plantLimb(Plant p, PlantSpecies s, PlantCrown c, int limb, out PlantLimb l) {
    vec3 bark = plantData(s.index, 1).xyz;
    if (limb == 0) {
        l = PlantLimb(vec3(0.0), vec3(0.0, c.center.y, 0.0), 0.2 * p.scale, 0.08 * p.scale, 0.0, bark);
        return true;
    }
    vec4 lobe = tLobe(s, c, limb - 1);
    l = PlantLimb(vec3(0.0, s.base * s.height * p.scale, 0.0), lobe.xyz, 0.08 * p.scale, 0.03 * p.scale, 0.0, bark);
    return true;
}
#ifdef PLANT_SHADE
PlantSurface plantSurface(PlantFragment f) { return PlantSurface(f.albedo, normalize(f.normal), vec3(0.5)); }
#endif
`;

const S = 192;
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
    scn.createLight({ type: 'directional', direction: [1, -1, 0], color: [1, 1, 1], intensity: 3, castShadow: true });
    const floor = scn.createMesh({ mesh: 'plane', halfW: 60, halfD: 60, color: [0.6, 0.6, 0.6] });
    floor.receivesShadow = true;
    scn.setWind({ speed: 0, gust: 0 });
    scn.setCamera({ fov: 50, near: 0.1, far: 500, position: [0, 4, 16], target: [0, 4, 0] });

    // The species' engine fields; its data the module's: a green, a stalk
    // brown, the lobes' size.
    const species = [{ height: 8, base: 0.25, radius: 3, limbs: 12, leafLength: 0.3, elements: 2000,
                       albedo: [0.1, 0.25, 0.04], data: [0.1, 0.3, 0.05, 0.45, 0.15, 0.1, 0.07, 0.2] }];
    const plants = scn.createPlants({ species, plants: [{ x: 0, y: 0, z: 0, seed: 7 }] });
    assert(plants && typeof plants.plantStats === 'function', 'createPlants returns a plants node');
    // A few frames drawn, then the stats of the newest the GPU finished.
    const drawn = () => {
        flush();
        for (let i = 0; i < 3; i++) advanceTime(16);
        return plants.plantStats();
    };

    // No module: nothing is drawn. One that does not compile: the same.
    let st = drawn();
    assert(st.visible === 0 && st.elements === 0, `no module, no plants drawn (${JSON.stringify(st)})`);
    plants.setPlants({ module: 'this is not GLSL' });
    st = drawn();
    assert(st.visible === 0 && st.elements === 0, `a module that does not compile draws none (${JSON.stringify(st)})`);
    plants.setPlants({ module: PLANT_MODULE });

    st = drawn();
    assert(st.plants === 1 && st.visible === 1, `one plant, in view (${JSON.stringify(st)})`);
    assert(st.elements > 100 && st.elements <= 2000, `near, it draws many elements (${st.elements})`);
    assert(st.limbs > 1, `near, its limbs are drawn as well as its trunk (${st.limbs})`);

    // Green where the crown is: the frame's middle, above the trunk.
    const img = scn.captureFrame();
    const g = patchAvg(img, S / 2 - 12, S / 2 - 30, S / 2 + 12, S / 2 - 6, 1);
    const r = patchAvg(img, S / 2 - 12, S / 2 - 30, S / 2 + 12, S / 2 - 6, 0);
    assert(g > r, `the crown is green (g ${g.toFixed(1)} r ${r.toFixed(1)})`);

    // The shadow: casters counted, and from above, the ground the sun's slant
    // throws the crown's shadow on (+x of it) darker than the open ground on
    // its other side.
    plants.castsShadow = true;
    scn.setCamera({ fov: 50, near: 0.1, far: 500, position: [0, 40, 0], target: [0, 0, 0], up: [0, 0, -1] });
    // (Pages are cached and the world service builds them within its
    // budget, so the casters are drawn on the frames that build one: under
    // a loaded machine not necessarily the last of three.)
    let casters = drawn().shadowElements;
    for (let i = 0; i < 30 && !(casters > 0); i++) { advanceTime(16); casters = plants.plantStats().shadowElements; }
    assert(casters > 0, `the shadow tiles' casters are counted (${casters})`);
    st = plants.plantStats();
    const above = scn.captureFrame();

    // Far off: fewer elements, falling with distance; a crown a few
    // elements across on screen may go under minElements, never to none.
    const near = st.elements;
    scn.setCamera({ fov: 50, near: 0.1, far: 2000, position: [0, 4, 40], target: [0, 4, 0] });
    const mid = drawn().elements;
    scn.setCamera({ fov: 50, near: 0.1, far: 2000, position: [0, 4, 400], target: [0, 4, 0] });
    st = drawn();
    assert(near > mid && mid > st.elements && st.elements >= 1,
           `elements fall with distance, never to none (${near}, ${mid}, ${st.elements})`);
    assert(st.limbs <= 1, `far off, the trunk alone (${st.limbs})`);

    // Past `range`, or out of view: nothing.
    plants.setPlants({ range: 100 });
    assert(drawn().visible === 0, 'past range, not drawn');
    plants.setPlants({ range: 6000 });
    scn.setCamera({ fov: 50, near: 0.1, far: 2000, position: [0, 4, 400], target: [0, 4, 800] });
    assert(drawn().visible === 0, 'behind the eye, not drawn');

    // Coarser elements, fewer of them.
    scn.setCamera({ fov: 50, near: 0.1, far: 500, position: [0, 4, 40], target: [0, 4, 0] });
    const fine = drawn().elements;
    plants.setPlants({ pixels: 40 });
    const coarse = drawn().elements;
    assert(coarse < fine, `a larger pixels draws fewer elements (${fine} -> ${coarse})`);
    plants.setPlants({ pixels: 14 });

    // New records replace the old.
    plants.setPlants([{ x: -4, z: 0, seed: 1 }, { x: 4, z: 0, seed: 2 }, { x: 0, z: -6, seed: 3 }]);
    assert(drawn().plants === 3, 'setPlants replaces the plants');

    // (About 5 px a metre on the ground from 40 m up: the shadow's middle
    // about 6 m +x of the trunk, the open ground as far -x.)
    const ppm = (S / 2) / (40 * Math.tan(25 * Math.PI / 180));
    const at = (m) => Math.round(S / 2 + m * ppm);
    const under = patchAvg(above, at(5.5), S / 2 - 4, at(7.5), S / 2 + 4, 1);
    const open = patchAvg(above, at(-7.5), S / 2 - 4, at(-5.5), S / 2 + 4, 1);
    assert(under < open * 0.85, `the crown's shadow darkens the ground (${under.toFixed(1)} vs ${open.toFixed(1)})`);

    // Many plants: 200k over 4 km square, the eye at the middle at the
    // crowns' height looking along -z (the side planes upright, 25 degrees
    // either side). A plant whose centre is well inside that wedge is drawn;
    // one further out than its sphere reaches is not.
    let seed = 12345;
    const rand = () => (seed = (seed * 1664525 + 1013904223) >>> 0) / 4294967296;
    const many = [];
    for (let i = 0; i < 200000; i++) many.push({ x: (rand() - 0.5) * 4000, z: (rand() - 0.5) * 4000, seed: i });
    plants.setPlants(many);
    scn.setCamera({ fov: 50, near: 0.1, far: 5000, position: [0, 4, 0], target: [0, 4, -100] });
    const reach = Math.sqrt(16 + 9) + 0.45 + 0.3;   // the sphere's radius (PlantsNode)
    const half = 25 * Math.PI / 180;
    let surely = 0, maybe = 0;
    for (const p of many) {
        const d = Math.hypot(p.x, p.z);
        const off = Math.abs(Math.atan2(p.x, -p.z));
        if (d > 12 && off < half - 0.01) surely++;
        if (off < half + Math.asin(Math.min(1, (reach + 0.5) / Math.max(d, 1e-3))) + 0.01) maybe++;
    }
    // The shadow pages that hold them are drawn in the first frames and then
    // kept, so their casters are counted in those frames.
    let cast = 0;
    flush();
    for (let i = 0; i < 3; i++) {
        advanceTime(16);
        cast = Math.max(cast, plants.plantStats().shadowElements);
    }
    st = drawn();
    assert(st.plants === 200000, `200k plants (${st.plants})`);
    assert(st.visible >= surely && st.visible <= maybe,
           `the camera draws the plants in its field (${surely} <= ${st.visible} <= ${maybe})`);
    assert(st.elements >= st.visible && st.elements < st.visible * 200,
           `each drawn with at least one element, the far ones few (${st.elements} for ${st.visible})`);
    assert(cast > 0, `the many cast (${cast})`);

    // Rules: the plants placed per cell on the engine's threads, from
    // fields (a number is a constant field), around the eye out to `range`.
    // Flown 20 km, about as many cells are kept wherever the eye is (as
    // many as the leaves in range there, plus those kept in the hysteresis
    // band behind it), and back where it began the plants are there again.
    const settle = () => {
        let p;
        for (let i = 0; i < 600; i++) {
            st = drawn();
            p = plants.plantPlacement();
            if (p.pending === 0) break;
        }
        return p;
    };
    const kinked = FastNoise.create('Simplex');
    kinked.set('Feature Scale', 80);
    const size = FastNoise.create('Add');   // 0.5 .. 1.5
    size.set('LHS', kinked);
    size.set('RHS', 1);
    plants.setPlants({ plants: [], rules: { range: 400, hysteresis: 64,
                                            layers: [{ spacing: 12, chance: 0.5, species: [0], scale: size }] } });
    scn.setCamera({ fov: 50, near: 0.1, far: 2000, position: [0, 4, 0], target: [0, 4, -100] });
    let p = settle();
    const home = { cells: p.cells, plants: st.plants };
    const disc = Math.PI * 400 * 400 / (12 * 12) * 0.5;
    assert(p.pending === 0 && home.plants > disc * 0.8 && home.plants < disc * 3,
           `placed about half the candidates in range (${home.plants}, a disc's ${Math.round(disc)}; ${p.cells} cells)`);
    assert(st.visible > 0, `the placed plants draw (${st.visible})`);
    let most = 0;
    for (let z = 0; z >= -20000; z -= 40) {
        scn.setCamera({ fov: 50, near: 0.1, far: 2000, position: [0, 4, z], target: [0, 4, z - 100] });
        flush();
        advanceTime(16);
        most = Math.max(most, plants.plantPlacement().cells);
    }
    p = settle();
    const about = (a, b) => Math.abs(a - b) <= b * 0.3;
    assert(about(p.cells, home.cells), `20 km off, about as many cells (${p.cells} vs ${home.cells})`);
    assert(most <= home.cells * 2, `on the way, never more than twice them (${most})`);
    assert(about(st.plants, home.plants), `and about as many plants (${st.plants} vs ${home.plants})`);
    assert(st.visible > 0, `the plants there draw (${st.visible})`);
    scn.setCamera({ fov: 50, near: 0.1, far: 2000, position: [0, 4, 0], target: [0, 4, -100] });
    p = settle();
    assert(about(p.cells, home.cells) && about(st.plants, home.plants), `back home, the plants again (${st.plants})`);
    plants.setPlants({ rules: null });
    assert(drawn().plants === 0, 'rules cleared, no plants');
}
