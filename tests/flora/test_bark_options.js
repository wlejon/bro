// A species' bark settings reach the bark build of mesh.frag through the
// vegetation records' bark word (broflora packBarkWord): barkFurrow cuts the
// plates deeper (more contrast on the trunk), barkMoss greens it. Drawn as
// GPU tubes (emitBranchTubes, MeshKind::Tube; flora/test_forest.js covers a
// forest's), compared on the trunk's pixels against the view without it.

function growTree(bark) {
    const world = bro.flora.createWorld({
        rngSeed: 7,
        shadow: { origin: [-8, 0, -8], cellSize: 1, width: 16, height: 16, depth: 16, fill: 1 },
    });
    const mono = world.addPrototype(bro.flora.prototypes.monopodial(3, 0.7));
    world.addVoronoiSite(mono, 0.5, 0.3);
    world.addPlant({ origin: [0, 0, 0], prototypeIndex: mono,
                     species: Object.assign({ growthScale: 2.0, maxAge: 400, apicalControl: 0.4, seedingRadius: 0 },
                                            bark) });
    for (let i = 0; i < 600; i++) world.step(0.05);
    return world;
}

// The trunk's pixels (where the frame differs from the empty view): their
// count, their local contrast (the mean luminance step to the next trunk
// pixel across the stem, over the mean: the furrows, not the lit and
// shaded sides), and their mean green over red.
function trunkStats(frame, empty) {
    const a = frame.data, b = empty.data, w = frame.width;
    const lum = (i) => 0.2126 * a[i] + 0.7152 * a[i + 1] + 0.0722 * a[i + 2];
    const on = (i) => Math.abs(a[i] - b[i]) + Math.abs(a[i + 1] - b[i + 1]) + Math.abs(a[i + 2] - b[i + 2]) >= 24;
    let n = 0, sum = 0, r = 0, g = 0, steps = 0, step = 0;
    for (let i = 0; i < a.length; i += 4) {
        if (!on(i)) continue;
        n++; sum += lum(i); r += a[i]; g += a[i + 1];
        if ((i / 4) % w < w - 1 && on(i + 4)) { steps++; step += Math.abs(lum(i + 4) - lum(i)); }
    }
    const mean = sum / Math.max(n, 1);
    return { n, contrast: step / Math.max(steps, 1) / Math.max(mean, 1e-3), green: g / Math.max(r, 1) };
}

if (bro.flora && bro.flora.available === false) {
    skipTest('bro.flora not compiled in');
} else {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', '256');
    cv.setAttribute('height', '256');
    document.body.appendChild(cv);
    flush();
    const scene = cv.getContext('scene');
    if (!scene) {
        missingGpuContext('scene');
    } else {
        run(scene);
    }
}

function run(scene) {
    scene.setWind({ speed: 0, gust: 0 });
    scene.setCamera({ fov: 30, near: 0.05, far: 50, position: [0, 0.5, 1.1], target: [0, 0.45, 0] });
    scene.createLight({ type: 'directional', direction: [-0.4, -1, -0.5], intensity: 3 });
    scene.captureFrame();
    const empty = scene.captureFrame();

    const settings = {
        smooth: { barkFurrow: 0, barkMoss: 0 },
        furrowed: { barkFurrow: 1, barkMoss: 0 },
        mossy: { barkFurrow: 0, barkMoss: 1 },
    };
    const kinds = {
        tube: (w) => scene.createInstancedMesh({ tube: w.emitBranchTubes(), color: '#6a5646' }),
    };
    for (const [kind, make] of Object.entries(kinds)) {
        const stats = {};
        for (const [name, bark] of Object.entries(settings)) {
            const node = make(growTree(bark));
            scene.captureFrame();
            stats[name] = trunkStats(scene.captureFrame(), empty);
            node.destroy();
        }
        const s = stats.smooth, f = stats.furrowed, m = stats.mossy;
        console.log(kind + ' bark: px ' + s.n + '/' + f.n + '/' + m.n + ', contrast ' + s.contrast.toFixed(3) + ' / ' +
                    f.contrast.toFixed(3) + ', green ' + s.green.toFixed(3) + ' / ' + m.green.toFixed(3));
        assert(s.n > 500 && f.n > 500 && m.n > 500, kind + ': the trunk fills part of the view (' + s.n + ' px)');
        assert(f.contrast > s.contrast * 1.3,
               kind + ': barkFurrow cuts the plates deeper: contrast ' + s.contrast.toFixed(3) + ' -> ' +
               f.contrast.toFixed(3));
        assert(m.green > s.green * 1.06,
               kind + ': barkMoss greens the bark: green/red ' + s.green.toFixed(3) + ' -> ' + m.green.toFixed(3));
    }
}
