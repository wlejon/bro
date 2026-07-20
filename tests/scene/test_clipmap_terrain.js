// Clipmap terrain (scene.createClipmapTerrain) — camera-centred rings whose
// vertices are displaced on the GPU from a streamed height pyramid.
//
// The properties worth testing are the ones the design claims:
//   (1) elevationAt() is the SAME height function the GPU renders — a visible
//       mesh and a collision query that disagree drop the player through the
//       floor. Checked against an analytic field, through heightScale/seaLevel,
//       and across a two-layer blend.
//   (2) No holes. Displacement is a pure function of world XZ evaluated at a
//       mip level that depends only on distance, so two rings meeting at a
//       boundary must land on the same height. A crack shows as background
//       (alpha 0) in the interior of a downward-looking frame; the terrain
//       itself writes alpha 255.
//   (3) Travelling a long way keeps that true, and the rings follow the camera.
//   (4) So does climbing — the fly-to-orbit case, where the visible ring count
//       goes from one or two to all of them.

const SIZE = 128;

const canvas = document.createElement('canvas');
canvas.setAttribute('width', String(SIZE));
canvas.setAttribute('height', String(SIZE));
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');

// Count pixels the renderer never touched. The scene clears to a transparent
// background, so alpha is an exact coverage mask — no colour heuristics.
function holeCount(img, inset) {
    let holes = 0;
    for (let y = inset; y < img.height - inset; y++) {
        for (let x = inset; x < img.width - inset; x++) {
            if (img.data[(y * img.width + x) * 4 + 3] < 250) holes++;
        }
    }
    return holes;
}

function luminanceStdDev(img, inset) {
    let n = 0, sum = 0, sum2 = 0;
    for (let y = inset; y < img.height - inset; y++) {
        for (let x = inset; x < img.width - inset; x++) {
            const i = (y * img.width + x) * 4;
            const l = (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
            sum += l; sum2 += l * l; n++;
        }
    }
    const mean = sum / n;
    return Math.sqrt(Math.max(0, sum2 / n - mean * mean));
}

// Build a layer's samples from an analytic function of world position. Texel
// (i, j) sits at world (originX + i*mpc, originZ + j*mpc).
function makeLayer(w, h, originX, originZ, mpc, fn) {
    const data = new Float32Array(w * h);
    for (let j = 0; j < h; j++)
        for (let i = 0; i < w; i++)
            data[j * w + i] = fn(originX + i * mpc, originZ + j * mpc);
    return { data, width: w, height: h, originX, originZ, metresPerCell: mpc };
}

if (!scene) {
    console.log('no scene context (no GPU) — skipping clipmap terrain test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

    // =====================================================================
    // (1) elevationAt matches the analytic field it was fed.
    //
    // A LINEAR field is reproduced EXACTLY by the bilinear reconstruction
    // both the CPU query and the GPU's level-0 fetch perform, so this is an
    // equality test (to float epsilon), not a tolerance test.
    // =====================================================================
    {
        // detailAmplitude 0: this section is about the layer stack, and
        // procedural detail deliberately displaces the surface away from it.
        const cm = scene.createClipmapTerrain({
            levels: 6, resolution: 32, cellSize: 1, detailRelief: 0,
        });
        const f = (x, z) => 0.01 * x + 0.02 * z;
        cm.setHeightLayer(0, makeLayer(64, 64, -128, -128, 4, f));

        assert(cm.layerCount === 1, `one layer installed (${cm.layerCount})`);
        assert(cm.levels === 6 && cm.resolution === 32,
            `config round-trips (levels=${cm.levels}, res=${cm.resolution})`);
        assert(cm.triangleCount > 0 && cm.vertexCount > 0,
            'ring geometry was built');

        for (const [x, z] of [[0, 0], [10, 20], [-37.5, 61.25], [100, -100]]) {
            const got = cm.elevationAt(x, z);
            const want = f(x, z);
            assert(Math.abs(got - want) < 1e-3,
                `elevationAt(${x},${z}) = ${got.toFixed(4)}, expected ${want.toFixed(4)}`);
        }

        // Beyond the layer's footprint the sampler clamps to the edge texel,
        // which is what keeps the horizon from folding to zero.
        const edge = cm.elevationAt(9999, 0);
        assert(Math.abs(edge - f(-128 + 63 * 4, 0)) < 1e-2,
            `outside the footprint clamps to the edge sample (${edge.toFixed(3)})`);

        cm.destroy();
    }

    // heightScale / seaLevel are applied on top of the sample.
    {
        const cm = scene.createClipmapTerrain({
            levels: 4, resolution: 16, cellSize: 2,
            heightScale: 3, seaLevel: 25, detailRelief: 0,
        });
        cm.setHeightLayer(0, makeLayer(32, 32, -256, -256, 16,
                                       (x, z) => 0.001 * x - 0.002 * z));
        for (const [x, z] of [[0, 0], [64, -32]]) {
            const want = 25 + 3 * (0.001 * x - 0.002 * z);
            const got = cm.elevationAt(x, z);
            assert(Math.abs(got - want) < 1e-3,
                `heightScale/seaLevel applied at (${x},${z}): ` +
                `${got.toFixed(4)} vs ${want.toFixed(4)}`);
        }
        cm.destroy();
    }

    // Two layers: the fine one wins well inside its footprint, the coarse one
    // takes over outside it, and the handover is a ramp rather than a step.
    {
        const cm = scene.createClipmapTerrain({
            levels: 5, resolution: 16, cellSize: 1, detailRelief: 0 });
        // Coarse layer 1: constant 100, covering +-4096 m.
        cm.setHeightLayer(1, makeLayer(64, 64, -4096, -4096, 128, () => 100));
        // Fine layer 0: constant 0, covering +-512 m.
        cm.setHeightLayer(0, makeLayer(64, 64, -512, -512, 16, () => 0));
        assert(cm.layerCount === 2, `two layers (${cm.layerCount})`);

        const middle = cm.elevationAt(0, 0);
        assert(Math.abs(middle) < 0.5,
            `fine layer wins at its centre (${middle.toFixed(2)})`);

        const outside = cm.elevationAt(3000, 0);
        assert(Math.abs(outside - 100) < 0.5,
            `coarse layer takes over outside the fine footprint (${outside.toFixed(2)})`);

        // The fade band is the outer 8% of the layer's texel extent per axis:
        // the fine layer spans ~1024 m, so the outer ~82 m on each side ramps
        // (x ~ 422..504). Sample across it and require a monotone climb from 0
        // toward 100 — a hard switch would show two values and nothing between.
        const ramp = [380, 430, 460, 490, 520].map(x => cm.elevationAt(x, 0));
        for (let i = 1; i < ramp.length; i++) {
            assert(ramp[i] >= ramp[i - 1] - 0.01,
                `blend is monotone across the fade band (${ramp.join(', ')})`);
        }
        assert(ramp[0] < 20 && ramp[ramp.length - 1] > 80,
            `blend spans both layers across the band (${ramp.map(v => v.toFixed(1)).join(', ')})`);
        const mid = ramp.filter(v => v > 15 && v < 85);
        assert(mid.length >= 1,
            `handover is a ramp, not a step (${ramp.map(v => v.toFixed(1)).join(', ')})`);

        // Releasing a layer drops it from the stack.
        cm.setHeightLayer(0, null);
        assert(cm.layerCount === 2,
            'layerCount tracks the highest present index, not the count');
        assert(Math.abs(cm.elevationAt(0, 0) - 100) < 0.5,
            'released fine layer stops contributing');
        cm.destroy();
    }

    // =====================================================================
    // (2) No holes — the crack test.
    //
    // A high-amplitude bumpy field seen from straight above spans many rings.
    // If two rings disagreed about the height at a shared boundary the seam
    // would open and the background (alpha 0) would show through. Anything
    // but zero uncovered interior pixels is a crack.
    // =====================================================================
    const bumpy = (x, z) =>
        60 * Math.sin(x * 0.004) * Math.cos(z * 0.0035) +
        18 * Math.sin(x * 0.02 + z * 0.017);

    // Coarse world layer (~7.7 km/side of coverage at 240 m/cell) with a
    // finer 30 m field over the origin, the shape the target app uses.
    const coarse = makeLayer(128, 128, -15360, -15360, 240, bumpy);
    const fine   = makeLayer(256, 256, -3840, -3840, 30, bumpy);

    scene.createLight({
        type: 'directional', direction: [0.3, -0.9, 0.3],
        color: [1, 1, 1], intensity: 2.5,
    });

    // A flat field must cover the frame — this catches a crack that the bumpy
    // field's own variation could otherwise hide. Run before the main terrain
    // exists so nothing else can be filling the gaps.
    {
        const cmFlat = scene.createClipmapTerrain({
            levels: 8, resolution: 64, cellSize: 1, detailRelief: 0 });
        cmFlat.setHeightLayer(0, makeLayer(64, 64, -2048, -2048, 64, () => 0));
        scene.setCamera({
            fov: 60, near: 1, far: 100000, position: [0, 120, 0],
            target: [0.001, 0, 0], up: [0, 0, -1],
        });
        cmFlat.update(0, 120, 0);
        const holes = holeCount(scene.captureFrame(), 4);
        assert(holes === 0, `flat field covers the frame too (${holes})`);
        cmFlat.destroy();
    }

    const cm = scene.createClipmapTerrain({
        levels: 12, resolution: 64, cellSize: 2,
    });
    cm.setHeightLayer(1, coarse);
    cm.setHeightLayer(0, fine);

    function frameAt(x, y, z, target) {
        scene.setCamera({
            fov: 60, near: 1, far: 200000,
            position: [x, y, z],
            target: target || [x + 0.001, y - 1, z],
            up: [0, 0, -1],
        });
        cm.update(x, y, z);
        return scene.captureFrame();
    }

    {
        const img = frameAt(0, 300, 0);
        const holes = holeCount(img, 4);
        assert(holes === 0,
            `looking down: no uncovered pixels in the frame interior (${holes})`);

        // ...and the surface is actually displaced, not a flat sheet: a
        // grazing key light turns slope into luminance variation.
        const sd = luminanceStdDev(img, 4);
        assert(sd > 3,
            `displaced terrain shades with visible relief (stddev ${sd.toFixed(2)})`);
    }

    // =====================================================================
    // (3) Travelling a long way: the rings follow, and nothing opens up.
    // =====================================================================
    {
        const positions = [[0, 300], [500, -400], [5000, 5000],
                           [-20000, 12000], [120000, -80000]];
        for (const [x, z] of positions) {
            const img = frameAt(x, 300, z);
            const holes = holeCount(img, 4);
            assert(holes === 0,
                `no holes after moving to (${x}, ${z}) — ${holes} uncovered pixels`);

            // The node is parked on the eye every update; that is what leaves
            // the model matrix at identity for the camera-relative shader.
            const p = cm.node.position;
            assert(Math.abs(p[0] - x) < 0.5 && Math.abs(p[2] - z) < 0.5 &&
                   Math.abs(p[1] - 300) < 0.5,
                `node follows the camera to (${x}, 300, ${z}) — got ` +
                `(${p[0]}, ${p[1]}, ${p[2]})`);
        }

        // Displacement is a function of world position, so returning to a
        // place must reproduce that place — the terrain may not drift.
        const a = frameAt(500, 300, -400);
        const b = frameAt(500, 300, -400);
        let maxDelta = 0;
        for (let i = 0; i < a.data.length; i += 41)
            maxDelta = Math.max(maxDelta, Math.abs(a.data[i] - b.data[i]));
        assert(maxDelta === 0,
            `revisiting a position renders identically (maxDelta=${maxDelta})`);
    }

    // =====================================================================
    // (4) Rising to orbit: the surface stays continuous as more and more
    // rings come into view and every ring's mip level climbs.
    // =====================================================================
    {
        // Altitudes are measured above the ground under the camera — below it
        // the camera is inside the terrain and sees backfaces, which is
        // correct behaviour rather than a hole.
        // The ring stack is finite: 12 levels of 64 quads at 2 m reach
        // 32*2*2^11 = 131 km, which comfortably contains the ~50 km ground
        // footprint of the highest camera below. Sweep past that and the
        // frame edges would legitimately show background, not a crack.
        assert(cm.farDistance > 100000,
            `ring stack reaches past the sweep (${cm.farDistance} m)`);

        const ground = cm.elevationAt(1234, -987);
        let prevMean = null;
        for (const alt of [3, 12, 60, 400, 2500, 12000, 60000]) {
            const y = ground + alt;
            const img = frameAt(1234, y, -987);
            const holes = holeCount(img, 4);
            assert(holes === 0,
                `no holes at altitude ${alt} m (${holes} uncovered pixels)`);

            // Everything visible stays lit terrain rather than degenerating
            // into black slivers as the mip level climbs.
            let sum = 0, n = 0;
            for (let i = 0; i < img.data.length; i += 4) {
                sum += (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
                n++;
            }
            const mean = sum / n;
            assert(mean > 8, `altitude ${alt} m still renders lit terrain (mean ${mean.toFixed(1)})`);
            prevMean = mean;
        }
        assert(prevMean !== null, 'altitude sweep ran');
    }

    // =====================================================================
    // (5) Procedural detail. It synthesises the decades below the finest layer,
    //     so the collision query has to carry it too — a surface you can see
    //     and a surface you can stand on that disagree by metres is the "fall
    //     through the floor" bug. And it must not cost the crack-free
    //     guarantee: the octaves are band-limited by the same cell size that
    //     picks the height mip, so rings still agree at their boundaries.
    // =====================================================================
    {
        const opts = {
            levels: 8, resolution: 64, cellSize: 1,
            detailWavelength: 32, detailRelief: 0.25, detailGain: 1.0,
            detailOctaves: 6,
        };
        const flat = (w) => makeLayer(64, 64, -2048, -2048, 64, () => 0);

        const cmD = scene.createClipmapTerrain(opts);
        cmD.setHeightLayer(0, flat());

        // Detail moves a flat field, deterministically, within its bound.
        // Octave i contributes relief * lambda0 / 2^i, so the series caps at
        // 0.25 * 32 * 2 = 16 m; the slope modulator only scales that down, and
        // on a flat field it sits at its 0.05 floor.
        let moved = 0, maxAbs = 0;
        for (let i = 0; i < 64; i++) {
            const x = i * 7.3 - 200, z = i * -3.1 + 150;
            const h = cmD.elevationAt(x, z);
            if (Math.abs(h) > 1e-4) moved++;
            maxAbs = Math.max(maxAbs, Math.abs(h));
            assert(cmD.elevationAt(x, z) === h, `elevationAt is deterministic at ${x},${z}`);
        }
        assert(moved > 50, `detail displaces a flat field (${moved}/64 samples)`);
        assert(maxAbs < 16.01, `detail stays inside its amplitude bound (${maxAbs.toFixed(2)})`);

        // Continuous, not hashed-per-sample: neighbouring queries a tenth of
        // the finest octave apart must not jump. Finest octave here is 1 m.
        let maxJump = 0;
        for (let i = 0; i < 200; i++) {
            const x = 40 + i * 0.1;
            maxJump = Math.max(maxJump,
                Math.abs(cmD.elevationAt(x + 0.1, 12) - cmD.elevationAt(x, 12)));
        }
        assert(maxJump < 1.5, `detail is continuous (largest 0.1 m step ${maxJump.toFixed(3)})`);

        // Turning it off returns the layer stack exactly.
        const cmOff = scene.createClipmapTerrain(
            Object.assign({}, opts, { detailRelief: 0 }));
        cmOff.setHeightLayer(0, flat());
        assert(Math.abs(cmOff.elevationAt(17.5, -22.5)) < 1e-4,
            'detailRelief 0 leaves the layer stack untouched');
        cmOff.destroy();

        // Still no cracks, and the surface now has texture: a detailed flat
        // field must vary in luminance where the undetailed one was uniform.
        scene.setCamera({
            fov: 60, near: 1, far: 100000, position: [0, 60, 0],
            target: [0.001, 0, 0], up: [0, 0, -1],
        });
        cmD.update(0, 60, 0);
        const img = scene.captureFrame();
        assert(holeCount(img, 4) === 0, 'detail keeps the surface crack-free');
        const sd = luminanceStdDev(img, 4);
        assert(sd > 1.0, `detail gives a flat field visible relief (sd ${sd.toFixed(2)})`);

        cmD.destroy();
    }

    cm.destroy();
    assert(cm.node === null, 'destroy() releases the node');
    // Destroy is idempotent and every accessor stays safe afterwards.
    cm.destroy();
    assert(cm.elevationAt(0, 0) === 0, 'elevationAt on a destroyed terrain is inert');

    console.log('clipmap terrain test passed');
}

document.body.removeChild(canvas);
flush();
