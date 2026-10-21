// scene.createPlants: plants built on the GPU from a species table and their
// records (scene/nodes/plants_node.h, vulkan/shaders/foliage/scene_plants.glsl),
// planned on the GPU over the world cells in view
// (vulkan/foliage/scene_plants_gpu_plan.cpp).
// Verifies:
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

    const species = [{ height: 8, base: 0.25, radius: 3, lobes: 12, lobeSize: 0.45, leafLength: 0.3, elements: 2000,
                       light: '#5a8a2a', dark: '#18300c' }];
    const plants = scn.createPlants({ species, plants: [{ x: 0, y: 0, z: 0, seed: 7 }] });
    assert(plants && typeof plants.plantStats === 'function', 'createPlants returns a plants node');
    // A few frames drawn, then the stats of the newest the GPU finished.
    const drawn = () => {
        flush();
        for (let i = 0; i < 3; i++) advanceTime(16);
        return plants.plantStats();
    };

    let st = drawn();
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
    st = drawn();
    assert(st.shadowElements > 0, `the shadow tiles' casters are counted (${st.shadowElements})`);
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
    st = drawn();
    assert(st.plants === 200000, `200k plants (${st.plants})`);
    assert(st.visible >= surely && st.visible <= maybe,
           `the camera draws the plants in its field (${surely} <= ${st.visible} <= ${maybe})`);
    assert(st.elements >= st.visible && st.elements < st.visible * 200,
           `each drawn with at least one element, the far ones few (${st.elements} for ${st.visible})`);
    assert(st.shadowElements > 0, `the many cast (${st.shadowElements})`);
}
