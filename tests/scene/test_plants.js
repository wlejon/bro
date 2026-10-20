// scene.createPlants: plants built on the GPU from a species table and their
// records (scene/plants_node.h, vulkan/shaders/scene_plants.glsl). Verifies:
//   - the node draws: a tree in view turns its patch of the frame green, its
//     elements and limbs counted in plantStats
//   - the detail falls with distance (fewer elements far off), never under
//     minElements, and a plant out of view or past `range` draws nothing
//   - setPlants replaces the records; params set through setPlants change
//     the count
//   - casting: the shadow tiles' casters are counted, and the ground under a
//     tree is darker than beside it

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
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scn.createLight({ type: 'directional', direction: [1, -1, 0], color: [1, 1, 1], intensity: 3, castShadow: true });
    const floor = scn.createMesh({ mesh: 'plane', halfW: 60, halfD: 60, color: [0.6, 0.6, 0.6] });
    floor.receivesShadow = true;
    scn.setWind({ speed: 0, gust: 0 });
    scn.setCamera({ fov: 50, near: 0.1, far: 500, position: [0, 4, 16], target: [0, 4, 0] });

    const species = [{ height: 8, base: 0.25, radius: 3, lobes: 12, lobeSize: 0.45, leafLength: 0.3, elements: 2000,
                       light: '#5a8a2a', dark: '#18300c' }];
    const plants = scn.createPlants({ species, plants: [{ x: 0, y: 0, z: 0, seed: 7 }] });
    assert(plants && typeof plants.plantStats === 'function', 'createPlants returns a plants node');

    flush();
    let st = plants.plantStats();
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
    flush();
    st = plants.plantStats();
    assert(st.shadowElements > 0, `the shadow tiles' casters are counted (${st.shadowElements})`);
    const above = scn.captureFrame();

    // Far off: fewer elements, falling with distance; a crown a few
    // elements across on screen may go under minElements, never to none.
    const near = st.elements;
    scn.setCamera({ fov: 50, near: 0.1, far: 2000, position: [0, 4, 40], target: [0, 4, 0] });
    flush();
    const mid = plants.plantStats().elements;
    scn.setCamera({ fov: 50, near: 0.1, far: 2000, position: [0, 4, 400], target: [0, 4, 0] });
    flush();
    st = plants.plantStats();
    assert(near > mid && mid > st.elements && st.elements >= 1,
           `elements fall with distance, never to none (${near}, ${mid}, ${st.elements})`);
    assert(st.limbs <= 1, `far off, the trunk alone (${st.limbs})`);

    // Past `range`, or out of view: nothing.
    plants.setPlants({ range: 100 });
    flush();
    assert(plants.plantStats().visible === 0, 'past range, not drawn');
    plants.setPlants({ range: 6000 });
    scn.setCamera({ fov: 50, near: 0.1, far: 2000, position: [0, 4, 400], target: [0, 4, 800] });
    flush();
    assert(plants.plantStats().visible === 0, 'behind the eye, not drawn');

    // Coarser elements, fewer of them.
    scn.setCamera({ fov: 50, near: 0.1, far: 500, position: [0, 4, 40], target: [0, 4, 0] });
    flush();
    const fine = plants.plantStats().elements;
    plants.setPlants({ pixels: 40 });
    flush();
    const coarse = plants.plantStats().elements;
    assert(coarse < fine, `a larger pixels draws fewer elements (${fine} -> ${coarse})`);

    // New records replace the old.
    plants.setPlants([{ x: -4, z: 0, seed: 1 }, { x: 4, z: 0, seed: 2 }, { x: 0, z: -6, seed: 3 }]);
    flush();
    assert(plants.plantStats().plants === 3, 'setPlants replaces the plants');

    // (About 5 px a metre on the ground from 40 m up: the shadow's middle
    // about 6 m +x of the trunk, the open ground as far -x.)
    const ppm = (S / 2) / (40 * Math.tan(25 * Math.PI / 180));
    const at = (m) => Math.round(S / 2 + m * ppm);
    const under = patchAvg(above, at(5.5), S / 2 - 4, at(7.5), S / 2 + 4, 1);
    const open = patchAvg(above, at(-7.5), S / 2 - 4, at(-5.5), S / 2 + 4, 1);
    assert(under < open * 0.85, `the crown's shadow darkens the ground (${under.toFixed(1)} vs ${open.toFixed(1)})`);
}
