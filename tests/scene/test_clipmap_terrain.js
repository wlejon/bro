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

    // =====================================================================
    // (6) The zoom is STABLE under an app that sizes its data from the
    //     terrain. farDistance changing is not cosmetic: an app answers it by
    //     re-cutting and re-uploading a height layer, megabytes plus mipmap
    //     generation, so a farDistance that steps every frame reads as a hard
    //     stutter. The loop closes because the horizon bound depends on
    //     maxHeight_, which is recomputed on every setHeightLayer — upload,
    //     bound moves, farDistance moves, upload again.
    //
    //     So: hold the altitude constant, jitter only the terrain height, and
    //     require the reach to settle. The jitter is small (peaks 2000..2600 m,
    //     a factor of 1.14 in horizon(peak)) — well inside the hysteresis band,
    //     and therefore something no amount of it may push across.
    // =====================================================================
    {
        // Peaks spanning 200..3000 m move horizon(peak) from 50 to 196 km, so
        // the bound swings by about 1.19x at these altitudes — comfortably
        // inside the 1.56x band, and therefore something that must never move
        // a settled stack.
        const PEAKS = [200, 1200, 2200, 3000];
        // SWEEP A FULL OCTAVE OF ALTITUDE. A threshold with no hysteresis only
        // misbehaves where the jitter straddles one of its steps, and the steps
        // are powers of two in reach — so a handful of arbitrary altitudes can
        // all land in flat regions and pass while the mechanism is broken (an
        // earlier four-altitude version of this test did exactly that). Walking
        // camY across a factor of two walks reach across a factor of two, which
        // cannot avoid a boundary.
        for (let s = 0; s < 16; s++) {
            const camY = 30000 * Math.pow(2, s / 16);
            const cmP = scene.createClipmapTerrain({
                levels: 10, resolution: 64, cellSize: 8,
                planetRadius: 6371000, seaLevel: 0, maxCellScale: 4096,
                detailWavelength: 64, detailRelief: 0.2, detailOctaves: 4,
            });

            const reaches = [];
            for (let frame = 0; frame < 40; frame++) {
                // What a well-behaved app does: size the request from the
                // terrain's own answer, then hand the result back.
                const cov = cmP.coverageDistance(camY);
                assert(isFinite(cov) && cov > 0,
                    `coverageDistance is a usable number (${cov})`);
                const peak = PEAKS[frame % PEAKS.length];
                const mpc = Math.max(1, (2 * cov) / 64);
                cmP.setHeightLayer(0, makeLayer(64, 64, -32 * mpc, -32 * mpc, mpc,
                    (x, z) => peak * (0.5 + 0.5 * Math.sin(x / 90000)
                                                * Math.cos(z / 70000))));
                cmP.update(0, camY, 0);
                reaches.push(cmP.farDistance);
            }

            // Warm-up is allowed: the stack starts at cellScale 1 and has to
            // climb to the altitude. What must not happen is perpetual motion.
            const settled = reaches.slice(24);
            const steps = settled.filter((r, i) => i > 0 && r !== settled[i - 1]).length;
            assert(steps === 0,
                `reach settles under a re-sizing app (camY ${camY.toFixed(0)}: ` +
                `${steps} steps in the last ${settled.length} frames, ` +
                `${[...new Set(settled)].join('/')})`);
            cmP.destroy();
        }
    }

    // =====================================================================
    // (7) bandLimited — where the data ends is DECLARED, not inferred.
    //
    // Procedural detail high-passes against the height data so the two never
    // describe the same wavelength twice. A grid of cell d carries nothing
    // shorter than 2d, so that is where the hand-over belongs — EXCEPT over a
    // smooth learned field, whose cell is metres but whose content stops at the
    // kilometre; there detail is deliberately allowed to overlap and roughen
    // from a fixed ceiling. Cell size alone cannot tell those two apart, so a
    // layer says which it is, per layer, and says nothing by default.
    //
    // Measured as displacement of the PICTURE against the same terrain with
    // detail switched off. That isolates what the synthetic band contributed
    // and is independent of what the underlying field happens to look like.
    // =====================================================================
    {
        // (4) parked the main terrain 60 km up and it is still in the graph; it
        // would draw over every frame below.
        cm.node.visible = false;

        const opts = {
            levels: 8, resolution: 64, cellSize: 1,
            detailWavelength: 48, detailRelief: 0.5, detailGain: 1.0,
            detailOctaves: 4,
        };
        const field = (x, z) => 200 * Math.sin(x * 0.0016) * Math.cos(z * 0.0014);

        function shot(layers, extra) {
            const t = scene.createClipmapTerrain(Object.assign({}, opts, extra || {}));
            for (const [i, desc] of layers) t.setHeightLayer(i, desc);
            scene.setCamera({
                fov: 60, near: 1, far: 100000, position: [0, 400, 0],
                target: [0.001, 0, 0], up: [0, 0, -1],
            });
            t.update(0, 400, 0);
            const img = scene.captureFrame();
            t.destroy();
            return img;
        }
        const meanAbsDiff = (a, b) => {
            let s = 0;
            for (let i = 0; i < a.data.length; i++) s += Math.abs(a.data[i] - b.data[i]);
            return s / a.data.length;
        };
        const maxAbsDiff = (a, b) => {
            let m = 0;
            for (let i = 0; i < a.data.length; i++)
                m = Math.max(m, Math.abs(a.data[i] - b.data[i]));
            return m;
        };

        // A 30 m layer: fine enough that the smooth-learned-window heuristic
        // fires, which is exactly the case the flag exists to override.
        const fine = () => makeLayer(256, 256, -3840, -3840, 30, field);
        const flagged = (desc) => Object.assign(desc, { bandLimited: true });

        const base  = shot([[0, fine()]], { detailRelief: 0 });
        const infer = shot([[0, fine()]]);
        const band  = shot([[0, flagged(fine())]]);

        const dInfer = meanAbsDiff(infer, base);
        const dBand  = meanAbsDiff(band, base);

        // The detail band is doing something in both cases — a flag that simply
        // switched detail off would pass the comparison below for the wrong
        // reason.
        assert(dBand > 0.3,
            `band-limited detail still textures the surface (${dBand.toFixed(2)})`);
        // ...but the inferred path adds the ~4 extra octaves between the layer's
        // own 60 m Nyquist and the 1 km roughening ceiling, and they dominate.
        assert(dInfer > 2.5 * dBand,
            `declaring the layer band-limited drops the synthesised band ` +
            `(inferred ${dInfer.toFixed(2)}, band-limited ${dBand.toFixed(2)})`);

        // BACKWARDS COMPATIBILITY. Saying nothing and saying false are the same
        // thing, and both are what the terrain drew before the flag existed.
        const explicitFalse = shot([[0, Object.assign(fine(), { bandLimited: false })]]);
        assert(maxAbsDiff(explicitFalse, infer) === 0,
            'omitting bandLimited renders identically to bandLimited: false');

        // The coarse-only world is untouched either way: the heuristic never
        // fires at kilometre cells, so the flag has nothing to override and must
        // not perturb the picture by so much as a bit.
        const coarse = () => makeLayer(64, 64, -245760, -245760, 7680, field);
        const coarseOff = shot([[0, coarse()]]);
        const coarseOn  = shot([[0, flagged(coarse())]]);
        assert(maxAbsDiff(coarseOn, coarseOff) === 0,
            'a kilometre-cell layer renders identically with and without the flag');

        // A MIXED STACK is the case the flag has to be per-layer for: a coarse
        // chart under a fine window, where only the fine layer declares itself.
        // The blend has to pick that declaration up where the fine layer covers.
        const stack = (bl) => [
            [1, makeLayer(64, 64, -245760, -245760, 7680, field)],
            [0, bl ? flagged(fine()) : fine()],
        ];
        const mixOff = shot(stack(false));
        const mixOn  = shot(stack(true));
        assert(meanAbsDiff(mixOn, mixOff) > 1.0,
            `the fine layer's declaration reaches the blend under a coarse ` +
            `chart (${meanAbsDiff(mixOn, mixOff).toFixed(2)})`);
    }

    // =====================================================================
    // Surface layers are a STACK, not a single chart.
    //
    // Before they were indexed, control channels were a property of the finest
    // layer alone: a terrain reaching hundreds of kilometres had one chart's
    // biome/moisture/temperature and nothing beyond its edge. The test that
    // matters is therefore about GROUND THE FINE LAYER DOES NOT COVER — an
    // assertion taken inside its footprint would pass on the old code too.
    // =====================================================================
    {
        const opts = { levels: 8, resolution: 64, cellSize: 1 };
        const flat = () => 0;
        const height = (w, mpc) => makeLayer(w, w, -(w * mpc) / 2, -(w * mpc) / 2, mpc, flat);

        // Three channels per texel. `v` is written to all three so a difference
        // shows whichever the material happens to read.
        function surf(w, mpc, v) {
            const data = new Float32Array(w * w * 3);
            data.fill(v);
            const span = w * mpc;
            return { data, width: w, height: w,
                     originX: -span / 2, originZ: -span / 2, metresPerCell: mpc };
        }

        // A fine 4 km window inside a coarse 128 km one. The camera looks at
        // ground ~20 km out, well outside the fine layer, so what is shaded
        // there can only have come from the coarse surface layer.
        function shot(surfaces) {
            const t = scene.createClipmapTerrain(opts);
            t.setHeightLayer(0, height(64, 64));      //   4.1 km span
            t.setHeightLayer(1, height(64, 2048));    // 131 km span
            for (const [i, desc] of surfaces) t.setSurfaceLayer(i, desc);
            scene.setCamera({
                fov: 60, near: 1, far: 200000, position: [0, 3000, 0],
                target: [0, 0, 20000], up: [0, 1, 0],
            });
            t.update(0, 3000, 0);
            const img = scene.captureFrame();
            t.destroy();
            return img;
        }
        const meanDiff = (a, b) => {
            let s = 0;
            for (let i = 0; i < a.data.length; i++) s += Math.abs(a.data[i] - b.data[i]);
            return s / a.data.length;
        };

        // Layer 0 alone, versus layer 0 plus a coarse layer that says something
        // different. If only the finest layer were consulted these would match.
        const fineOnly = shot([[0, surf(64, 64, 0.0)]]);
        const stacked  = shot([[0, surf(64, 64, 0.0)], [1, surf(64, 2048, 0.9)]]);
        assert(meanDiff(stacked, fineOnly) > 0.5,
            `a coarse surface layer shades ground the fine one does not cover ` +
            `(${meanDiff(stacked, fineOnly).toFixed(2)})`);

        // The single-argument form still addresses layer 0 — the whole reason
        // index 0 kept the unnumbered uniform names.
        function shotLegacy() {
            const t = scene.createClipmapTerrain(opts);
            t.setHeightLayer(0, height(64, 64));
            t.setHeightLayer(1, height(64, 2048));
            t.setSurfaceLayer(surf(64, 64, 0.0));     // no index
            scene.setCamera({
                fov: 60, near: 1, far: 200000, position: [0, 3000, 0],
                target: [0, 0, 20000], up: [0, 1, 0],
            });
            t.update(0, 3000, 0);
            const img = scene.captureFrame();
            t.destroy();
            return img;
        }
        assert(meanDiff(shotLegacy(), fineOnly) === 0,
            'setSurfaceLayer(desc) is exactly setSurfaceLayer(0, desc)');

        // Releasing a layer must actually release it, or a stale chart keeps
        // shading ground its owner thinks it gave back.
        const released = shot([[0, surf(64, 64, 0.0)], [1, surf(64, 2048, 0.9)],
                               [1, null]]);
        assert(meanDiff(released, fineOnly) === 0,
            'releasing a surface layer restores the shorter stack exactly');

        // Out-of-range indices throw rather than silently landing somewhere.
        let threw = false;
        const probe = scene.createClipmapTerrain(opts);
        try {
            probe.setSurfaceLayer(9, surf(8, 64, 0.5));
        } catch (e) {
            threw = true;
        } finally {
            // Created outside the try so the throw cannot leak it — a leaked
            // terrain survives to teardown and fails the run somewhere with no
            // relation to the assertion that caused it.
            probe.destroy();
        }
        assert(threw, 'setSurfaceLayer rejects an out-of-range index');

        // FOUR CHANNELS.
        //
        // The three-channel form has to stay bit-identical — every existing
        // caller passes width*height*3 and says nothing about components — and
        // the fourth channel has to actually arrive. The second half is the
        // one worth having: a widened buffer that the sampler still reads as
        // three would pass a "does the old form still work" test perfectly.
        function surf4(w, mpc, rgb, a) {
            const data = new Float32Array(w * w * 4);
            for (let i = 0; i < w * w; i++) {
                data[i * 4 + 0] = rgb; data[i * 4 + 1] = rgb;
                data[i * 4 + 2] = rgb; data[i * 4 + 3] = a;
            }
            const span = w * mpc;
            return { data, width: w, height: w, components: 4,
                     originX: -span / 2, originZ: -span / 2, metresPerCell: mpc };
        }

        // Same RGB as the three-channel case, so anything that differs came
        // through the widened path rather than through the values.
        const four = shot([[0, surf4(64, 64, 0.0, 0.0)],
                           [1, surf4(64, 2048, 0.9, 0.0)]]);
        assert(meanDiff(four, stacked) === 0,
            'a 4-component layer with w = 0 renders exactly as the 3-component one');

        // A short buffer must be refused rather than read past its end: the
        // size check multiplies by `components`, so declaring 4 and supplying
        // 3 is the way this API gets someone a heap overread.
        let shortThrew = false;
        const probe4 = scene.createClipmapTerrain(opts);
        try {
            const bad = surf(8, 64, 0.5);   // 8*8*3 floats
            bad.components = 4;             // ... declared as 8*8*4
            probe4.setSurfaceLayer(0, bad);
        } catch (e) {
            shortThrew = true;
        } finally {
            probe4.destroy();
        }
        assert(shortThrew, 'setSurfaceLayer rejects a buffer too short for its components');

        // 5 channels is not a thing, and failing loudly beats uploading garbage.
        let compsThrew = false;
        const probe5 = scene.createClipmapTerrain(opts);
        try {
            const bad = surf(8, 64, 0.5);
            bad.components = 5;
            probe5.setSurfaceLayer(0, bad);
        } catch (e) {
            compsThrew = true;
        } finally {
            probe5.destroy();
        }
        assert(compsThrew, 'setSurfaceLayer rejects a components count other than 3 or 4');
    }

    // =====================================================================
    // (7) The curvature chart centre.
    //
    // Default = the camera ground point, re-pushed every update; pinning it
    // via setChartCenter must (a) be a no-op when pinned exactly AT the
    // camera ground point — same floats, same frame — and (b) actually move
    // the picture when pinned elsewhere, or the uniform is a dead wire. The
    // camera parks at XZ = (0,0) so (a) is an exact-equality test.
    // =====================================================================
    {
        const opts = { levels: 8, resolution: 64, cellSize: 4,
                       planetRadius: 6371000, detailRelief: 0 };
        function shotChart(pin) {
            const t = scene.createClipmapTerrain(opts);
            const w = 64, mpc = 8192;
            t.setHeightLayer(0, makeLayer(w, w, -(w * mpc) / 2, -(w * mpc) / 2,
                                          mpc, (x, z) => 0.0001 * x + 200));
            scene.setCamera({
                fov: 60, near: 1, far: 800000, position: [0, 3000, 0],
                target: [0, 0, 40000], up: [0, 1, 0],
            });
            if (pin === 'clear') { t.setChartCenter(250000, 0); t.setChartCenter(null); }
            else if (pin) t.setChartCenter(pin[0], pin[1]);
            t.update(0, 3000, 0);
            const img = scene.captureFrame();
            t.destroy();
            return img;
        }
        const meanDiff = (a, b) => {
            let s = 0;
            for (let i = 0; i < a.data.length; i++) s += Math.abs(a.data[i] - b.data[i]);
            return s / a.data.length;
        };

        const def = shotChart(null);
        assert(meanDiff(shotChart([0, 0]), def) === 0,
            'a chart pinned at the camera ground point renders the default frame exactly');
        assert(meanDiff(shotChart([250000, 0]), def) > 0.5,
            `a chart pinned 250 km away moves the picture — the wire is live ` +
            `(${meanDiff(shotChart([250000, 0]), def).toFixed(2)})`);
        assert(meanDiff(shotChart('clear'), def) === 0,
            'setChartCenter(null) restores the camera-following default exactly');
    }

    // =====================================================================
    // (8) Chart-pinned ground truths — the LOD and the reach follow the
    //     RENDERED sheet, not the flat field.
    //
    // With a pinned centre, cmCurve drops the sheet at chord rho by the
    // sagitta ~rho^2/2R — 12.55 km at 400 km on Earth radius. Three claims:
    //   (a) renderedElevationAt is elevationAt EXACTLY (same bits) while no
    //       chart is pinned, and the analytic bent height once one is;
    //   (b) an eye standing ON the bent sheet 400 km out still renders its
    //       procedural detail. Broken, u_camGroundY carried the UNBENT field
    //       height, cmCellSize believed the eye was 12.6 km up, and its AA
    //       floor (~170 m at this viewport) killed every octave in the frame
    //       — the far-station picture airbrushed while the near one stayed
    //       crisp;
    //   (c) an eye flying high above the bent sheet keeps its horizon reach.
    //       Broken, its world Y was negative, horizonDistance clamped it to
    //       an eye at zero altitude, and the stack capped at horizon(peak)
    //       alone — the world ended tens of km out from a 20 km cruise.
    // =====================================================================
    {
        const R = 6371000;
        const w = 64, mpc = 16384;          // one layer covering +-524 km
        const rho = 400000;

        // (a) The query itself, against the closed form on a constant field.
        const tV = scene.createClipmapTerrain({
            levels: 8, resolution: 64, cellSize: 4,
            planetRadius: R, detailRelief: 0,
        });
        tV.setHeightLayer(0, makeLayer(w, w, -(w * mpc) / 2, -(w * mpc) / 2,
                                       mpc, () => 200));
        for (const [x, z] of [[0, 0], [123456, -98765], [400000, 250000]])
            assert(tV.renderedElevationAt(x, z) === tV.elevationAt(x, z),
                `renderedElevationAt IS elevationAt with no pinned chart (${x}, ${z})`);
        tV.setChartCenter(0, 0);
        assert(tV.renderedElevationAt(0, 0) === tV.elevationAt(0, 0),
            'renderedElevationAt at the pinned centre is still the flat answer');
        const th    = Math.asin(rho / (R + 200));
        const wantY = 200 * Math.cos(th) - 2 * R * Math.sin(th / 2) ** 2;
        const gotY  = tV.renderedElevationAt(rho, 0);
        assert(Math.abs(gotY - wantY) < 0.5,
            `renderedElevationAt bends by the sagitta at 400 km ` +
            `(${gotY.toFixed(1)} vs ${wantY.toFixed(1)} analytic)`);
        assert(tV.elevationAt(rho, 0) - gotY > 12000,
            'the 400 km drop is kilometres, not noise');
        tV.destroy();

        // (b) Eye-level detail at a 400 km station: a fine band-limited
        // window under the eye, detail on vs off, the frame must move.
        const t8 = scene.createClipmapTerrain({
            levels: 8, resolution: 64, cellSize: 4,
            planetRadius: R, maxCellScale: 4096,
            detailWavelength: 64, detailRelief: 0.6, detailOctaves: 6,
        });
        t8.setHeightLayer(1, Object.assign(
            makeLayer(w, w, -(w * mpc) / 2, -(w * mpc) / 2, mpc, () => 200),
            { bandLimited: true }));
        const fmpc = 32;                     // 2 km window centred on the station
        t8.setHeightLayer(0, Object.assign(
            makeLayer(w, w, rho - (w * fmpc) / 2, -(w * fmpc) / 2, fmpc,
                      () => 200),
            { bandLimited: true }));
        t8.setChartCenter(0, 0);

        const eyeY = t8.renderedElevationAt(rho, 0) + 2;
        assert(eyeY < -12000,
            `the station eye rides the bent sheet (${eyeY.toFixed(0)})`);
        scene.setCamera({ fov: 60, near: 0.5, far: 200000,
                          position: [rho, eyeY, 0],
                          target: [rho + 40, eyeY - 4, 0], up: [0, 1, 0] });
        for (let i = 0; i < 6; i++) t8.update(rho, eyeY, 0);
        assert(t8.cellScale === 1,
            `an eye 2 m over the rendered sheet keeps the finest rings ` +
            `(cellScale ${t8.cellScale})`);
        const withDetail = scene.captureFrame();
        t8.setDetail({ relief: 0 });
        t8.update(rho, eyeY, 0);
        const noDetail = scene.captureFrame();
        let diff = 0;
        for (let i = 0; i < withDetail.data.length; i++)
            diff += Math.abs(withDetail.data[i] - noDetail.data[i]);
        diff /= withDetail.data.length;
        assert(diff > 1,
            `detail octaves survive a 400 km chart offset — switching them ` +
            `off moves the frame (meanDiff ${diff.toFixed(2)})`);

        // (c) Reach for a 20 km eye 600 km off the pinned centre.
        const rho2 = 600000;
        const hiY  = t8.renderedElevationAt(rho2, 0) + 20000;
        assert(hiY < 0,
            `the cruise camera's world Y is negative under the bend ` +
            `(${hiY.toFixed(0)})`);
        for (let i = 0; i < 12; i++) t8.update(rho2, hiY, 0);
        assert(t8.cellScale >= 8,
            `a 20 km eye 600 km off the pinned centre zooms the stack out ` +
            `(cellScale ${t8.cellScale}, farDistance ` +
            `${(t8.farDistance / 1000).toFixed(0)} km)`);
        t8.destroy();
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
