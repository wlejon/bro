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

    const sun = scene.createLight({
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
    // Six data slots. The cap was four because the first world needed four;
    // a planet-scale stack wants a fine window, regional, continental and
    // global charts with room over. The wiring proof has to be a PICTURE:
    // an unrolled blend chain that stops at four would leave slots 4 and 5
    // silently dead while every CPU-side accessor still reported them.
    //   - a SET 6th layer moves the frame (the wire is live),
    //   - an UNSET one does not (releasing it restores the frame exactly),
    //   - the CPU mirror agrees layer by layer out to the 6th.
    // =====================================================================
    {
        const opts = { levels: 10, resolution: 64, cellSize: 1, detailRelief: 0 };
        // Nested footprints, finest first, each with its own constant height
        // so whichever layer wins at a radius is legible in the query.
        const heights = () => [
            makeLayer(64, 64, -512, -512, 16, () => 0),          //   1 km span
            makeLayer(64, 64, -1024, -1024, 32, () => 5),        //   2 km
            makeLayer(64, 64, -2048, -2048, 64, () => 10),       //   4 km
            makeLayer(64, 64, -4096, -4096, 128, () => 15),      //   8 km
            makeLayer(64, 64, -8192, -8192, 256, () => 20),      //  16 km
            makeLayer(64, 64, -262144, -262144, 8192, () => 2000) // 524 km
        ];

        const t6 = scene.createClipmapTerrain(opts);
        heights().forEach((h, i) => t6.setHeightLayer(i, h));
        assert(t6.layerCount === 6, `six height layers install (${t6.layerCount})`);

        // Each finer layer wins at its own radius; the 6th is the base.
        for (const [x, want] of [[0, 0], [700, 5], [1500, 10], [3000, 15],
                                 [6000, 20], [100000, 2000]]) {
            const got = t6.elevationAt(x, 0);
            assert(Math.abs(got - want) < 0.5,
                `layer stack of six blends correctly at x=${x} ` +
                `(${got.toFixed(2)} vs ${want})`);
        }

        const maxAbsDiff = (a, b) => {
            let m = 0;
            for (let i = 0; i < a.data.length; i++)
                m = Math.max(m, Math.abs(a.data[i] - b.data[i]));
            return m;
        };
        const meanAbsDiff = (a, b) => {
            let s = 0;
            for (let i = 0; i < a.data.length; i++) s += Math.abs(a.data[i] - b.data[i]);
            return s / a.data.length;
        };
        function frame6() {
            // Ground ~60 km out is covered by layer 5 alone (2000 m) — with
            // it released the stack clamps to layer 4's edge texel (20 m), so
            // the far field's whole silhouette moves.
            scene.setCamera({
                fov: 60, near: 1, far: 400000, position: [0, 4000, 0],
                target: [0, 0, 60000], up: [0, 1, 0],
            });
            // Update until the zoom hysteresis settles: a single update per
            // capture would leave cellScale mid-climb, and two captures at
            // different zooms cannot be compared bit for bit.
            for (let i = 0; i < 8; i++) t6.update(0, 4000, 0);
            return scene.captureFrame();
        }

        const withSix = frame6();
        t6.setHeightLayer(5, null);
        const withFive = frame6();
        assert(meanAbsDiff(withSix, withFive) > 0.5,
            `the 6th height slot is a live wire — releasing it moves the frame ` +
            `(${meanAbsDiff(withSix, withFive).toFixed(2)})`);
        t6.setHeightLayer(5, heights()[5]);
        assert(maxAbsDiff(frame6(), withSix) === 0,
            're-installing the 6th layer restores the frame bit for bit');

        // The 6th SURFACE slot reaches the blend too: a coarse control layer
        // at index 5 must shade ground the finer ones do not cover.
        function surf6(w, mpc, v) {
            const data = new Float32Array(w * w * 3);
            data.fill(v);
            const span = w * mpc;
            return { data, width: w, height: w,
                     originX: -span / 2, originZ: -span / 2, metresPerCell: mpc };
        }
        for (let i = 0; i < 5; i++) t6.setSurfaceLayer(i, surf6(64, 8 << i, 0.1));
        const surfFive = frame6();
        t6.setSurfaceLayer(5, surf6(64, 8192, 0.9));
        const surfSix = frame6();
        assert(meanAbsDiff(surfSix, surfFive) > 0.5,
            `the 6th surface slot is a live wire ` +
            `(${meanAbsDiff(surfSix, surfFive).toFixed(2)})`);
        t6.setSurfaceLayer(5, null);
        assert(maxAbsDiff(frame6(), surfFive) === 0,
            'releasing the 6th surface layer restores the frame bit for bit');
        t6.destroy();

        // Indices 0..5 are in range; 6 is not.
        const probe6 = scene.createClipmapTerrain(opts);
        let threw6 = false;
        try { probe6.setHeightLayer(6, heights()[0]); } catch (e) { threw6 = true; }
        probe6.setHeightLayer(5, heights()[0]);
        assert(probe6.layerCount === 6, 'index 5 is a valid height slot');
        probe6.destroy();
        assert(threw6, 'setHeightLayer rejects index 6');
    }

    // =====================================================================
    // Per-layer LOD fade (layerFade, opt-in).
    //
    // With the flag on, a finer layer's blend weight in ALL THREE chains
    // (height, data floor, surface channels) is scaled by
    // 1 - smoothstep(1.2T, 3.2T, c): full weight while the rendered cell is
    // still finer than the layer's texel, gone by the time the cell is a
    // little over a mip level and a half wider. The band was [2T, 8T] until
    // c8184413 narrowed it; the probe altitudes below are chosen against the
    // band that is actually in cmLayerFade, so they move when it does.
    // What must hold, measured as the frame difference between the same scene
    // WITH and WITHOUT a 30 m fine window (height + surface + the detail floor
    // it moves):
    //   * low camera (c ~ 20 m under the eye, under 1.2T = 36 m): the window
    //     is live at full weight;
    //   * high camera (c ~ 338 m >= 3.2T = 96 m): the window's contribution
    //     is gone TO THE BIT — this is also the coherence proof, because a
    //     chain that kept fading independently (say surface but not floor)
    //     would leave a nonzero residue here;
    //   * in between: a SWEPT range of altitudes, of which some sample shows a
    //     partial contribution and none of which rises — the fade is a ramp,
    //     not a switch. Swept rather than probed at one altitude on purpose:
    //     see the note on the sweep for why a fixed altitude is not portable;
    //   * with the fade OFF at the same high camera the window still stamps
    //     the frame (the different-toned rectangle this flag exists to
    //     remove), so the zero above is the fade's doing, not invisibility;
    //   * omitted and explicit-false render bit-identically.
    // =====================================================================
    {
        const optsBase = {
            levels: 10, resolution: 64, cellSize: 2,
            detailWavelength: 48, detailRelief: 0.3, detailOctaves: 5,
        };
        // Both fields ride 200 m up so the moisture-driven grass band — the
        // material channel that makes the tone rectangle visible — is what
        // covers the frame, not the surface-channel-blind sand band at sea
        // level.
        const rough = (x, z) => 200 + 40 * Math.sin(x * 0.05) * Math.cos(z * 0.045);
        const gentle = (x, z) => 200 + 8 * Math.sin(x * 0.0004) * Math.cos(z * 0.00035);
        function surfL(w, mpc, v) {
            const data = new Float32Array(w * w * 3);
            data.fill(v);
            const span = w * mpc;
            return { data, width: w, height: w,
                     originX: -span / 2, originZ: -span / 2, metresPerCell: mpc };
        }
        function shot(alt, withFine, extra) {
            const t = scene.createClipmapTerrain(
                Object.assign({}, optsBase, extra || {}));
            t.setHeightLayer(1, makeLayer(128, 128, -15360, -15360, 240, gentle));
            if (withFine) {
                // Height, surface and (through the floor) detail all at
                // T = 30 m, so every chain's fade is exercised and they must
                // all be gone together at c >= 3.2T = 96 m.
                t.setHeightLayer(0, makeLayer(256, 256, -3840, -3840, 30, rough));
                t.setSurfaceLayer(0, surfL(128, 30, 0.9));
                t.setSurfaceLayer(1, surfL(128, 240, 0.1));
            } else {
                // Surface layers must stay contiguous from 0, so the coarse
                // chart sits at index 0 here — same data, same arithmetic,
                // just a shorter stack.
                t.setSurfaceLayer(0, surfL(128, 240, 0.1));
            }
            scene.setCamera({ fov: 60, near: 1, far: 300000,
                              position: [0, alt, 0], target: [0.001, 0, 0],
                              up: [0, 0, -1] });
            for (let i = 0; i < 10; i++) t.update(0, alt, 0);
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
        const dAt = (alt, extra) =>
            meanAbsDiff(shot(alt, true, extra), shot(alt, false, extra));

        const ON = { layerFade: true };
        const dLow  = dAt(1500, ON);
        const dHigh = maxAbsDiff(shot(25000, true, ON), shot(25000, false, ON));
        assert(dLow > 2,
            `faded stack still shows the fine window at eye scale (${dLow.toFixed(2)})`);
        assert(dHigh === 0,
            `at c >= 3.2T every chain's contribution from the window is gone ` +
            `to the bit (maxAbsDiff ${dHigh})`);

        // THE RAMP IS SWEPT, NOT PROBED AT ONE ALTITUDE, and that is the whole
        // point of the shape below.
        //
        // A single "mid-ramp" altitude encodes an assumption about what c is at
        // that altitude, and c is not a property of this test. It comes out of
        // cmCellSize — max(cGeo, cAA), where cAA rides on u_pixelScale, i.e. on
        // the projection and the render target the frame is actually drawn into.
        // The fade band is only 1.2T..3.2T, a factor of 2.7 in c, so an altitude
        // that measures a partial weight in one configuration can be past the
        // end of the ramp in another. That is not hypothetical: alt 8000 sat
        // mid-band until c8184413 narrowed the band, and alt 4000 measured a
        // partial 3.36 on both a hardware driver and llvmpipe here while
        // reading an exact 0 on CI. Chasing the number is the wrong move; the
        // section's claim does not depend on it.
        //
        // So sweep geometrically and assert the CLAIM: that between full weight
        // and gone the contribution passes through partial values rather than
        // switching. The step ratio is ~1.35, comfortably inside the band's own
        // 2.7, so wherever the band falls within the swept range at least one
        // sample lands inside it — for any u_pixelScale, on any driver.
        const ALTS = [1500, 2000, 2700, 3600, 4900, 6600, 8900, 12000, 16000, 22000];
        const ramp = ALTS.map((alt) => dAt(alt, ON));
        const shown = ramp.map((d) => d.toFixed(2)).join(' ');

        const partial = ramp.filter((d) => d > 0.05 && d < dLow);
        assert(partial.length > 0,
            `the fade is a ramp, not a switch — some altitude between full ` +
            `weight and gone shows a PARTIAL contribution ` +
            `(dLow ${dLow.toFixed(2)}, sweep ${shown})`);

        // ...and it only ever descends. Equal steps are expected (both ends of
        // the band are flat), so this is non-increasing rather than strictly
        // decreasing; the tolerance is 1% of dLow, since the two scenes differ
        // in more than the fade weight alone and the frame is 8-bit. A step
        // back UP beyond that would mean this is not a fade at all.
        const tol = 0.01 * dLow;
        for (let i = 1; i < ramp.length; i++) {
            assert(ramp[i] <= ramp[i - 1] + tol,
                `the fade is monotone in altitude — step ${i} ` +
                `(alt ${ALTS[i - 1]} -> ${ALTS[i]}) rose from ` +
                `${ramp[i - 1].toFixed(2)} to ${ramp[i].toFixed(2)} ` +
                `(sweep ${shown})`);
        }

        // The zero above is the FADE's doing: with the flag off the same
        // window still stamps the same high frame — the different-toned
        // rectangle the consumer measured at -20/-11/-4 RGB. Measured on the
        // window's own pixels (the centre patch), where it is ~17 LSB here.
        const patchLum = (img) => {
            let s = 0, n = 0;
            const w = img.width, lo = Math.floor(w * 0.44), hi = w - lo;
            for (let y = lo; y < hi; y++)
                for (let x = lo; x < hi; x++) {
                    const i = (y * w + x) * 4;
                    s += (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
                    n++;
                }
            return s / n;
        };
        const offHighWith    = patchLum(shot(25000, true, {}));
        const offHighWithout = patchLum(shot(25000, false, {}));
        assert(Math.abs(offHighWith - offHighWithout) > 8,
            `without the fade the sub-pixel window still stamps its rectangle ` +
            `(${offHighWith.toFixed(1)} vs ${offHighWithout.toFixed(1)})`);

        // Default-off bit-identity: omitting the flag IS the flag at false.
        assert(maxAbsDiff(shot(1500, true, {}),
                          shot(1500, true, { layerFade: false })) === 0,
            'omitting layerFade renders identically to layerFade: false');
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

    // =====================================================================
    // (9) The shading normal knows the chart's own metric.
    //
    // The AE chart is length-true radially and compresses ACROSS-track by
    // sinc = sin(th)/th — cmCurve builds the geometry that way — so the true
    // across-slope of the drawn surface is flat_gradient / sinc. The shading
    // normal used to be built from the flat gradient alone, understating
    // across-slope by 0.32% at th = 0.138 (881 km) and 24% at th = 1.256
    // (8,000 km): the shaded surface was not the drawn surface.
    //
    // The assertion is an EQUIVALENCE, because the whole claim of the pinned
    // chart is that a station at any radius is the same place as the centre:
    // a flat across-slope g at flat radius rho is, on the sphere, the slope
    // g/sinc — so a frame of it, taken along the LOCAL vertical with the
    // light along the LOCAL up, must match a frame of an explicit g/sinc
    // slope at the chart centre, where the chart is exact by construction.
    // Materials are forced uniform and the camera sits high enough that the
    // mottle band-limits to zero, so luminance is pure shading response.
    //
    // WHERE THE STATION CAN BE. u_camXZ does double duty — the camera's
    // world position and the flat coordinate the rings sample from — so the
    // sheet drawn near a camera at world chord x comes from flat
    // F = R asin(x / R) and lands (by cmCurve) at world x = F sinc exactly:
    // hovering over a flat point means standing at ITS WORLD CHORD, and the
    // ring stack then has to reach the F - x = F (1 - sinc) flat metres from
    // its own centre to the window. That bounds the test radius: at
    // th = 0.44 the offset is ~91 km, inside a settled 131 km reach, and the
    // window is sized (262 km at 4096 m/cell) so the sampled mip at that
    // flat distance still resolves it. The field is a LINEAR across-slope,
    // which every mip level and every differencing spacing reproduces
    // exactly, so the analytic slope is the rendered slope.
    //
    //   analytic margins, g = 1.4, th = 0.4395, sinc = 0.9681, and the
    //   pipeline's measured response of ~46.6 LSB per unit N.L per unit
    //   intensity at this albedo — so ~373 per N.L at intensity 8:
    //     N.L true  = 1/sqrt(1 + (g/sinc)^2) = 0.5683  -> ~212
    //     N.L flat  = 1/sqrt(1 + g^2)        = 0.5812  -> ~217
    //   a ~4.8-LSB mismatch pre-fix against the 2.5-LSB gate below
    //   (measured: station 215.5 vs centre 211.1 on the pre-fix binary).
    //   At 881 km — today's rim, th = 0.1383, sinc error 0.32% — the same
    //   construction moves by ~0.5 LSB, gated at 1.5: the fix leaves
    //   today's frames within an LSB.
    // =====================================================================
    {
        cm.node.visible = false;   // (4) parked it 60 km up, still in the graph
        const R = 6371000;
        const g = 1.4;
        const sunWas = { direction: [0.3, -0.9, 0.3], intensity: 2.5 };
        sun.intensity = 4.0;
        scene.setAmbient([0, 0, 0]);

        // One frame: an across-track slope `s` painted into a 262 km window
        // at flat radius rho, camera 3000 m up the LOCAL vertical over the
        // window centre's own world chord, light down the LOCAL up, screen
        // up = the across axis (+z) in both frames.
        function slopeShot(rho, s) {
            const t = scene.createClipmapTerrain({
                levels: 8, resolution: 64, cellSize: 4,
                planetRadius: R, detailRelief: 0, snowLine: 1e6,
            });
            const m = { albedo: [0.5, 0.5, 0.5], roughness: 1.0 };
            t.setMaterials({ rock: m, snow: m, sand: m, grass: m });
            const w = 64, mpc = 4096;
            t.setHeightLayer(0, makeLayer(w, w, rho - (w * mpc) / 2,
                                          -(w * mpc) / 2, mpc,
                                          (x, z) => s * z));
            t.setChartCenter(0, 0);
            const th = rho / R;
            const sinc = th > 0 ? Math.sin(th) / th : 1;
            // The window centre's world position and local up, exactly as
            // cmCurve places them (h = 0 on the window's centreline).
            const P  = [rho * sinc, -2 * R * Math.sin(th / 2) ** 2, 0];
            const E2 = [Math.sin(th), Math.cos(th), 0];
            const cam = [P[0] + 3000 * E2[0], P[1] + 3000 * E2[1], P[2]];
            sun.direction = [-E2[0], -E2[1], -E2[2]];
            scene.setCamera({ fov: 60, near: 10, far: 400000,
                              position: cam, target: P, up: [0, 0, 1] });
            for (let i = 0; i < 8; i++) t.update(cam[0], cam[1], cam[2]);
            const img = scene.captureFrame();
            t.destroy();
            return img;
        }
        function patchMean(img) {
            const inset = Math.floor(img.width * 0.34);
            let sum = 0, n = 0;
            for (let y = inset; y < img.height - inset; y++)
                for (let x = inset; x < img.width - inset; x++) {
                    const i = (y * img.width + x) * 4;
                    sum += (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
                    n++;
                }
            return sum / n;
        }

        // The far station — the analytic point that fails on the
        // uncorrected normal.
        {
            const rho = 2800000;                  // th = 0.4395
            const th = rho / R, sinc = Math.sin(th) / th;
            const station = patchMean(slopeShot(rho, g));
            const centre  = patchMean(slopeShot(0, g / sinc));
            assert(centre > 100,
                `metric reference frame is lit (${centre.toFixed(1)})`);
            assert(Math.abs(station - centre) <= 2.5,
                `across-slope at th=${th.toFixed(3)} shades as its true ` +
                `sphere slope g/sinc (station ${station.toFixed(2)} vs ` +
                `centre ${centre.toFixed(2)})`);
        }

        // Today's rim — quantifies that the fix leaves the near field
        // within an LSB.
        {
            const rho = 881000;                   // th = 0.1383
            const th = rho / R, sinc = Math.sin(th) / th;
            const station = patchMean(slopeShot(rho, g));
            const centre  = patchMean(slopeShot(0, g / sinc));
            assert(Math.abs(station - centre) <= 1.5,
                `the 881 km rim moves by at most an LSB ` +
                `(station ${station.toFixed(2)} vs centre ${centre.toFixed(2)})`);
        }

        sun.direction = sunWas.direction;
        sun.intensity = sunWas.intensity;
        cm.node.visible = true;
    }

    // =====================================================================
    // (10) Coverage floor (coverageFloor, opt-in).
    //
    // The horizon enters the zoom policy only as a CEILING, and the 2.5x
    // step-up gate lets the settled cellScale sit up to 2.5x below it — so
    // at a 1,111 km camera the stack reaches 2,097 km against a 3,923 km
    // tangent to the limb, and the sheet ends mid-disc. The floor asks for
    // reach >= the limb, clamped to the data's own edge (maxCellScale).
    //   * default OFF: today's numbers exactly (cellScale 16, reach
    //     2,097 km < limb) — the lag is real and unchanged;
    //   * ON at the same camera: reach >= min(limb, data reach);
    //   * ON at eye level: cellScale identical to default — the floor is
    //     fractions of one scale step there;
    //   * ON under the §6 app loop (re-sizing its layer from the terrain's
    //     own answer every frame): reach settles — the floor depends only
    //     on eye altitude and static config, not on maxHeight_, so it
    //     cannot reopen the re-upload loop, and its 1x up-gate plus the
    //     1.25x down-veto cannot fight the 0.8x step-down (derivation in
    //     ClipmapTerrain::update).
    // =====================================================================
    {
        const R = 6371000;
        const opts = { levels: 10, resolution: 64, cellSize: 8,
                       planetRadius: R, maxCellScale: 4096, detailRelief: 0 };
        const unit = 8 * 32 * Math.pow(2, 9);        // 131072 m at cellScale 1
        const field = (x, z) =>
            1500 * (0.5 + 0.5 * Math.sin(x / 90000) * Math.cos(z / 70000));
        function mk(extra) {
            const t = scene.createClipmapTerrain(Object.assign({}, opts, extra || {}));
            const w = 64, mpc = 131072;              // global chart, +-4,194 km
            t.setHeightLayer(0, makeLayer(w, w, -(w * mpc) / 2, -(w * mpc) / 2,
                                          mpc, field));
            t.setChartCenter(0, 0);
            return t;
        }
        const alt = 1111000;

        // Default off — pin the lag itself so this section notices if the
        // baseline behaviour ever drifts.
        const off = mk({});
        for (let i = 0; i < 20; i++) off.update(0, alt, 0);
        const limb = off.horizonDistance(alt);
        assert(limb > 3.9e6 && limb < 4.0e6,
            `tangent to the limb at 1,111 km is ~3,923 km (${(limb / 1000).toFixed(0)} km)`);
        assert(off.cellScale === 16 && off.farDistance < limb,
            `without the floor the reach lags the limb (cellScale ` +
            `${off.cellScale}, ${(off.farDistance / 1000).toFixed(0)} km vs ` +
            `${(limb / 1000).toFixed(0)} km)`);
        off.destroy();

        // On — reach clears min(limb need, data reach).
        const on = mk({ coverageFloor: true });
        for (let i = 0; i < 20; i++) on.update(0, alt, 0);
        const dataReach = 4096 * unit;
        assert(on.farDistance >= Math.min(limb, dataReach),
            `the floor carries the reach to the limb ` +
            `(cellScale ${on.cellScale}, ${(on.farDistance / 1000).toFixed(0)} km)`);

        // ...and stays settled under the app loop from (6): re-cut the layer
        // from coverageDistance every frame, jitter the peaks, demand zero
        // steps once warm.
        const reaches = [];
        const PEAKS = [2000, 2300, 2600, 2150];
        for (let f = 0; f < 40; f++) {
            const cov = on.coverageDistance(alt);
            const mpc = Math.max(1, (2 * cov) / 64);
            const peak = PEAKS[f % PEAKS.length];
            on.setHeightLayer(0, makeLayer(64, 64, -32 * mpc, -32 * mpc, mpc,
                (x, z) => peak * (0.5 + 0.5 * Math.sin(x / 90000)
                                            * Math.cos(z / 70000))));
            on.update(0, alt, 0);
            reaches.push(on.farDistance);
        }
        const settled = reaches.slice(24);
        const steps = settled.filter((r, i) => i > 0 && r !== settled[i - 1]).length;
        assert(steps === 0,
            `floored reach settles under a re-sizing app (${steps} steps, ` +
            `${[...new Set(settled)].join('/')})`);
        on.destroy();

        // Eye level: the floor must not move the zoom the pixels chose.
        function eyeScale(extra) {
            const t = mk(extra);
            const y = t.elevationAt(0, 0) + 2;
            for (let i = 0; i < 10; i++) t.update(0, y, 0);
            const cs = t.cellScale;
            t.destroy();
            return cs;
        }
        const eyeOff = eyeScale({});
        const eyeOn  = eyeScale({ coverageFloor: true });
        assert(eyeOn === eyeOff,
            `at eye level the floor is inactive (cellScale ${eyeOn} vs ${eyeOff})`);
    }

    cm.destroy();
    assert(cm.node === null, 'destroy() releases the node');
    // Destroy is idempotent and every accessor stays safe afterwards.
    cm.destroy();
    assert(cm.elevationAt(0, 0) === 0, 'elevationAt on a destroyed terrain is inert');

    // =====================================================================
    // (11) CUBIC RECONSTRUCTION OF THE CONTROL CHANNELS (cubicSurface).
    //
    // Bilinear is C0: its derivative jumps at every texel edge, so a threshold
    // placed on a control channel draws its contour as a chain of straight
    // segments hinged on the texel lattice — an organic feature bounded by the
    // data grid, worst at the coarse rungs where a texel is kilometres. The
    // opt-in mode reconstructs each surface layer with a cubic B-spline (C2)
    // instead. Weights, the sixteen-taps-to-four reduction, the edge clamp and
    // why not Catmull-Rom: clipmap_cubic.glsl.
    //
    // What this section proves:
    //   * OFF changes nothing — not the vertex source, not one byte of the
    //     fragment source outside a single contiguous insertion, not one bit
    //     of a frame with no control layer to read.
    //   * ON is measurably smoother, measured as the thing the artefact IS:
    //     the second difference of the rendered channel across the texel
    //     lattice, with a gate the bilinear path cannot pass.
    //   * The geometry is untouched in both modes.
    //   * layerFade's exact zero survives the new chain.
    //   * The four-tap footprint stays inside the layer.
    // =====================================================================
    {
        const bytesEqual = (a, b) => {
            if (a.data.length !== b.data.length) return false;
            for (let i = 0; i < a.data.length; i++)
                if (a.data[i] !== b.data[i]) return false;
            return true;
        };

        // --- OFF is not merely equivalent, it is the same source ------------
        {
            const off = scene.createClipmapTerrain({ levels: 4, resolution: 16 });
            const on  = scene.createClipmapTerrain({ levels: 4, resolution: 16,
                                                     cubicSurface: true });
            const fOff = off.shaderSource('fragment');
            const fOn  = on.shaderSource('fragment');

            assert(fOff.length > 0 && fOn.length > fOff.length,
                'shaderSource returns the composed fragment chunk');
            assert(fOff.indexOf('cmCubicTap') < 0,
                'the default fragment source carries no cubic code at all');
            assert(fOn.indexOf('cmCubicTap') > 0,
                'the cubic fragment source carries the filter');

            // The strong statement, and the one that survives future edits to
            // any of the four chunks: turning the mode ON inserts ONE
            // contiguous run of bytes and changes nothing else. Turning it off
            // therefore cannot shift a single instruction — the off path is
            // not a dead branch, it is source that is not there.
            let p = 0;
            while (p < fOff.length && fOff.charCodeAt(p) === fOn.charCodeAt(p)) p++;
            let s = 0;
            while (s < fOff.length - p &&
                   fOff.charCodeAt(fOff.length - 1 - s) ===
                   fOn.charCodeAt(fOn.length - 1 - s)) s++;
            assert(p + s === fOff.length,
                `the cubic source is the default source plus one contiguous ` +
                `insertion (matched ${p} + ${s} of ${fOff.length} bytes)`);

            // The vertex stage never receives the chunk. It must not: nothing
            // there reads a control channel, and clipmap.vert.glsl declares a
            // cmSurface of its own (the sheet height) that the chunk's rename
            // would otherwise capture.
            assert(off.shaderSource('vertex') === on.shaderSource('vertex'),
                'the vertex source is identical in both modes');
            assert(on.shaderSource('vertex').indexOf('cmCubicTap') < 0,
                'the cubic chunk never reaches the vertex stage');
            assert(off.shaderSource('geometry') === '',
                'an unknown stage name returns empty');

            off.destroy();
            on.destroy();
        }

        // --- the measurement scene -----------------------------------------
        //
        // Flat ground under a high, narrow-angle camera, so the frame is a
        // pure picture of the reconstructed channel and nothing else:
        //   * a constant height layer  -> every fragment has the same normal
        //     and the same altitude, so slope, snow and sand are identically 0
        //     and grass owns the whole frame;
        //   * detailRelief 0 and an altitude far above the mottle's fade band
        //     -> no procedural term contributes;
        //   * biome 6 (grassland) and temperature 0.5 -> no biome branch, no
        //     forest tint, past the cold clamp, so the grass colour is a
        //     monotone function of the MOISTURE channel alone.
        // One control texel is 2048 m — the coarse rung the artefact was
        // measured at — and the camera sits where that texel spans about ten
        // pixels.
        const SURF = 64, TEXEL = 2048, ALT = 74200, FOV = 20;
        const HALF = SURF * TEXEL / 2;

        function control(fn) {
            const data = new Float32Array(SURF * SURF * 3);
            for (let j = 0; j < SURF; j++)
                for (let i = 0; i < SURF; i++) {
                    const k = (j * SURF + i) * 3;
                    data[k]     = 6.0;
                    data[k + 1] = fn(i, j);
                    data[k + 2] = 0.5;
                }
            return { data, width: SURF, height: SURF,
                     originX: -HALF, originZ: -HALF,
                     metresPerCell: TEXEL, components: 3 };
        }

        function flat(cubic, extra) {
            const opts = { levels: 12, resolution: 64, cellSize: 8,
                           detailRelief: 0, snowLine: 1e9, cubicSurface: cubic };
            for (const k in (extra || {})) opts[k] = extra[k];
            const t = scene.createClipmapTerrain(opts);
            t.setHeightLayer(0, makeLayer(64, 64, -HALF * 2, -HALF * 2,
                                          HALF * 4 / 64, () => 100));
            t.setMaterials({ grass: { albedo: [0.05, 0.95, 0.05] } });
            return t;
        }

        function shotDown(t) {
            scene.setCamera({
                fov: FOV, near: 1, far: 4000000,
                position: [0, ALT, 0], target: [0.001, 0, 0], up: [0, 0, -1],
            });
            t.update(0, ALT, 0);
            return scene.captureFrame();
        }

        // Mean over scanlines of each scanline's LARGEST second difference in
        // green. The second difference is exactly the quantity that is
        // discontinuous: a bilinear reconstruction is piecewise linear along a
        // row, so its second difference is ~0 inside a texel and spikes at
        // every texel edge, and it is those spikes a threshold turns into the
        // straight segments of a jagged contour. A C2 reconstruction has no
        // spike to find. Taking the per-row maximum rather than the mean keeps
        // the statistic on the kinks instead of drowning them in the 8-bit
        // quantisation floor that both paths share.
        function kinkEnergy(img) {
            let sum = 0, rows = 0;
            for (let y = 8; y < img.height - 8; y++) {
                let mx = 0;
                for (let x = 9; x < img.width - 9; x++) {
                    const i = (y * img.width + x) * 4 + 1;
                    const d2 = Math.abs(img.data[i - 4] - 2 * img.data[i] +
                                        img.data[i + 4]);
                    if (d2 > mx) mx = d2;
                }
                sum += mx; rows++;
            }
            return sum / rows;
        }

        // --- the gate -------------------------------------------------------
        //
        // Moisture runs through a cosine of two, three and four texels across
        // X — real content near the top of the band a chart can carry, which
        // is what a coarse rung holds and what bilinear renders as a fan of
        // straight segments. Three periods rather than one so the gate does
        // not rest on a single number.
        //
        // Strictly one terrain in the scene at a time: two overlapping sheets
        // at the same altitude decide themselves by depth, and the frame would
        // be measuring the tie-break rather than the filter.
        const probes = [[0, 0], [4321, -8765], [60000, 60000]];
        function run(cubic, field) {
            const t = flat(cubic);
            t.setSurfaceLayer(0, field);
            const img = shotDown(t);
            const mirror = probes.map(([x, z]) =>
                [t.elevationAt(x, z), t.renderedElevationAt(x, z)]);
            t.destroy();
            return { img, mirror };
        }
        {
            let off = null, on = null;
            for (const period of [2, 3, 4]) {
                const field = control((i) =>
                    0.333 + 0.333 * Math.cos(2 * Math.PI * i / period));
                off = run(false, field);
                on  = run(true, field);
                const kOff = kinkEnergy(off.img), kOn = kinkEnergy(on.img);
                console.log(`  cubic: ${period}-texel period — kink energy ` +
                            `bilinear ${kOff.toFixed(2)} -> cubic ` +
                            `${kOn.toFixed(2)} (${(kOff / kOn).toFixed(2)}x)`);

                // The bilinear path cannot pass this at any of the three. Its
                // kink is a property of the FILTER, not of the data or the
                // resolution: it is the field's curvature over one texel,
                // collected into one pixel at the texel edge, so it grows as
                // the content gets shorter — 5, 8, 11.7, 16 levels at 6, 4, 3,
                // 2 texels — while the cubic path sits at 2, which is the
                // 8-bit quantisation floor of the measurement itself. There is
                // nothing left in the frame for a smoother filter to remove.
                assert(kOff > 2.5 * kOn,
                    `cubic reconstruction removes the texel-lattice kinks at a ` +
                    `${period}-texel period (bilinear ${kOff.toFixed(2)}, ` +
                    `cubic ${kOn.toFixed(2)})`);
            }
            const imgOff = off.img, imgOn = on.img;

            // ...and it is a smoothing, not a flattening: the channel still
            // drives the frame. A filter that had simply washed the field out
            // would pass the gate above and be useless.
            const spread = (img) => {
                let lo = 255, hi = 0;
                for (let i = 1; i < img.data.length; i += 4) {
                    if (img.data[i] < lo) lo = img.data[i];
                    if (img.data[i] > hi) hi = img.data[i];
                }
                return hi - lo;
            };
            const sOn = spread(imgOn), sOff = spread(imgOff);
            assert(sOn > 0.5 * sOff,
                `the smoothed channel still carries its signal ` +
                `(green spread ${sOn} vs ${sOff})`);

            // GEOMETRY IS UNTOUCHED. The mode changes what a fragment reads,
            // never where the surface is: the coverage mask must be identical
            // to the bit, and the CPU mirrors with it.
            let alphaDiff = 0;
            for (let i = 3; i < imgOff.data.length; i += 4)
                if (imgOff.data[i] !== imgOn.data[i]) alphaDiff++;
            assert(alphaDiff === 0,
                `the drawn surface does not move (${alphaDiff} alpha pixels differ)`);
            probes.forEach(([x, z], i) => {
                assert(on.mirror[i][0] === off.mirror[i][0] &&
                       on.mirror[i][1] === off.mirror[i][1],
                    `the CPU elevation mirrors are unchanged at (${x}, ${z})`);
            });
        }

        // --- with no control layer there is nothing to reconstruct ----------
        // The chain returns before its first fetch, so the frame must be
        // identical to the bit. This is the frame-level half of the source
        // argument above: the chunk cannot leak into a stack that never calls
        // it.
        {
            function bare(cubic) {
                const t = flat(cubic);
                const img = shotDown(t);
                t.destroy();
                return img;
            }
            assert(bytesEqual(bare(false), bare(true)),
                'with no surface layer installed both modes render identically');
        }

        // --- a constant field is reproduced exactly, edges included ---------
        // The B-spline's weights are non-negative and sum to 1, so a constant
        // field must come back as that constant EVERYWHERE — including the
        // border rows, where the four taps clamp into the band of texel
        // centres. If the clamp were wrong the border would read something
        // else, and this frame would differ from the bilinear one. It is the
        // edge behaviour asserted rather than described.
        {
            const k = control(() => 0.42);
            // A camera aimed across the layer's own corner, so the assertion
            // covers the border rows and not just the interior.
            function shotEdge(cubic) {
                const t = flat(cubic);
                t.setSurfaceLayer(0, k);
                scene.setCamera({
                    fov: 70, near: 1, far: 4000000,
                    position: [HALF, ALT, HALF], target: [0, 0, 0], up: [0, 1, 0],
                });
                t.update(HALF, ALT, HALF);
                const img = scene.captureFrame();
                t.destroy();
                return img;
            }
            assert(bytesEqual(shotEdge(false), shotEdge(true)),
                'a constant control field reconstructs identically under both ' +
                'filters, across the layer edge included');
        }

        // --- layerFade's exact zero survives --------------------------------
        // A layer whose data has gone deeply sub-pixel is mixed in with an
        // EXACT 0.0, and mix(s, f, 0.0) is s to the bit. The test does not
        // trust the arithmetic: it replaces the faded layer's contents with a
        // completely different field and demands the frame not move by one
        // bit. That also proves the cubic fetches stay finite — a NaN arriving
        // from off the texture would survive the zero and poison the mix.
        {
            // 64 m texels seen from 200 km: the rendered cell there is ~830 m,
            // past the 3.2T = 205 m at which cmLayerFade reaches an exact zero.
            const FINE = 64, N = 128, ALT2 = 200000;
            const coarse = control((i, j) =>
                0.333 + 0.333 * Math.cos(Math.PI * (i + j) / 3));
            function fine(fn) {
                const data = new Float32Array(N * N * 3);
                for (let j = 0; j < N; j++)
                    for (let i = 0; i < N; i++) {
                        const k = (j * N + i) * 3;
                        data[k] = 6.0; data[k + 1] = fn(i, j); data[k + 2] = 0.5;
                    }
                return { data, width: N, height: N,
                         originX: -N * FINE / 2, originZ: -N * FINE / 2,
                         metresPerCell: FINE, components: 3 };
            }
            const quietFine = fine(() => 0.05);
            const loudFine  = fine((i, j) => ((i ^ j) & 1) ? 0.0 : 0.66);
            function shotHigh(t) {
                scene.setCamera({
                    fov: FOV, near: 1, far: 4000000,
                    position: [0, ALT2, 0], target: [0.001, 0, 0], up: [0, 0, -1],
                });
                // Let the cellScale hysteresis settle before measuring, or
                // the two captures would differ by the zoom policy's own
                // step rather than by the layer under test.
                for (let i = 0; i < 12; i++) t.update(0, ALT2, 0);
                return scene.captureFrame();
            }
            function swap(cubic, fade) {
                const t = flat(cubic, { layerFade: fade });
                t.setSurfaceLayer(1, coarse);
                t.setSurfaceLayer(0, quietFine);
                const a = shotHigh(t);
                t.setSurfaceLayer(0, loudFine);
                const b = shotHigh(t);
                t.destroy();
                return bytesEqual(a, b);
            }
            for (const cubic of [false, true]) {
                assert(swap(cubic, true),
                    `a faded-out layer contributes nothing to the bit ` +
                    `(cubicSurface ${cubic})`);
                // ...and the assertion above is not passing because nothing was
                // looking: the identical swap with the fade OFF moves the frame.
                assert(!swap(cubic, false),
                    `the faded-layer test has teeth — unfaded, the same swap ` +
                    `changes the frame (cubicSurface ${cubic})`);
            }
        }
    }

    // =====================================================================
    // (12) MIP-AWARE CUBIC RECONSTRUCTION OF THE HEIGHT FIELD (cubicHeight).
    //
    // The same C0 problem as (11), one field over and with two differences
    // that are the whole of the work. Height is read by the VERTEX stage (the
    // displaced geometry) and five more times per FRAGMENT (the shading
    // normal's taps), so the filter has to go into both stages at once or the
    // shaded surface stops being the drawn one. And cmLayer samples a
    // FRACTIONAL MIP, so correct tap spacing is the SAMPLED level's texel size
    // — a level-0-spaced filter puts all four fetches inside one sampled texel
    // and returns plain trilinear at exactly the coarse rungs it was added for.
    // Derivation, the fractional-mip choice and the gate:
    // clipmap_cubic_height.glsl.
    //
    // What this section proves:
    //   * OFF changes nothing — not one byte of either stage's source outside
    //     a single contiguous insertion.
    //   * ON is measurably smoother at TWO coarse rungs, one of them at
    //     lod > 1, where a level-0-spaced filter would be trilinear and score
    //     no better than bilinear. That second rung is the mip-awareness
    //     proof: there is nothing else in the frame that could move it.
    //   * The near field — where the CPU mirrors are exact and where things
    //     stand — is untouched to the bit, in the frame and in the queries.
    //     That is the coherence proof AND the cost bound: those fragments
    //     take no extra fetch at all.
    //   * The four-tap footprint stays inside a clamped layer and keeps
    //     wrapping on a periodic one.
    //   * layerFade's exact zero survives the new chain, in both modes.
    // =====================================================================
    {
        const bytesEqual = (a, b) => {
            if (a.data.length !== b.data.length) return false;
            for (let i = 0; i < a.data.length; i++)
                if (a.data[i] !== b.data[i]) return false;
            return true;
        };
        const maxDiff = (a, b) => {
            let m = 0;
            for (let i = 0; i < a.data.length; i++) {
                const d = Math.abs(a.data[i] - b.data[i]);
                if (d > m) m = d;
            }
            return m;
        };

        // --- OFF is not merely equivalent, it is the same source, twice -----
        {
            const off = scene.createClipmapTerrain({ levels: 4, resolution: 16 });
            const on  = scene.createClipmapTerrain({ levels: 4, resolution: 16,
                                                     cubicHeight: true });
            const both = scene.createClipmapTerrain({ levels: 4, resolution: 16,
                                                      cubicHeight: true,
                                                      cubicSurface: true });

            // The strong statement, for BOTH stages this time: turning the mode
            // on inserts ONE contiguous run of bytes and changes nothing else,
            // so turning it off cannot shift a single instruction.
            function oneInsertion(base, grown) {
                let p = 0;
                while (p < base.length && base.charCodeAt(p) === grown.charCodeAt(p)) p++;
                let s = 0;
                while (s < base.length - p &&
                       base.charCodeAt(base.length - 1 - s) ===
                       grown.charCodeAt(grown.length - 1 - s)) s++;
                return p + s === base.length;
            }
            for (const stage of ['vertex', 'fragment']) {
                const a = off.shaderSource(stage), b = on.shaderSource(stage);
                assert(a.indexOf('cmHeightCubic') < 0,
                    `the default ${stage} source carries no cubic height code`);
                assert(b.indexOf('cmHeightCubic') > 0,
                    `the cubic ${stage} source carries the filter`);
                assert(b.length > a.length && oneInsertion(a, b),
                    `the cubic ${stage} source is the default plus exactly one ` +
                    `contiguous insertion`);
            }
            // Unlike cubicSurface this one MUST reach the vertex stage: the
            // displaced geometry is one of the two things it exists to keep in
            // step with the other.
            assert(on.shaderSource('vertex').indexOf('cmCubicTapLevel') > 0,
                'the height chunk reaches the vertex stage — the geometry is ' +
                'reconstructed by the same filter the fragment shades from');
            // The two flags are independent, and together they are still one
            // run of bytes: the chunks are adjacent by construction.
            assert(oneInsertion(off.shaderSource('fragment'),
                                both.shaderSource('fragment')),
                'both cubic flags together are still one contiguous insertion');
            assert(both.shaderSource('fragment').indexOf('cmSurfaceCubic') > 0 &&
                   both.shaderSource('fragment').indexOf('cmHeightCubic') > 0,
                'both flags compose');

            off.destroy(); on.destroy(); both.destroy();
        }

        // --- the measurement rig --------------------------------------------
        //
        // Nadir over a field that varies only in X, lit from the side, with
        // every material albedo set to the same grey. Nothing in the frame is
        // then a function of anything but the SHADING NORMAL — which is the
        // derivative of the reconstruction, and therefore exactly where a C0
        // height field's discontinuity lives. Mottling is faded out at this
        // cell size and detailRelief is 0, so no procedural term contributes.
        //
        // The cell size is pinned: cellSize 1024 with maxCellScale 1 makes
        // cDesired an exact 1024 m across the whole frame (the ring-cell limit
        // beats the pixel limit everywhere in it), so the sampled mip level is
        // one number and the rung under test is the rung that was chosen.
        const CH_ALT = 80000, CH_FOV = 20;
        const chSunWas = { direction: sun.direction, intensity: sun.intensity };
        sun.direction = [-0.6, -0.8, 0.0];
        sun.intensity = 4.0;

        function chTerrain(cubic, extra) {
            const opts = { levels: 6, resolution: 64, cellSize: 1024,
                           maxCellScale: 1, detailRelief: 0, snowLine: 1e9,
                           cubicHeight: cubic };
            for (const k in (extra || {})) opts[k] = extra[k];
            const t = scene.createClipmapTerrain(opts);
            const m = { albedo: [0.5, 0.5, 0.5], roughness: 1.0 };
            t.setMaterials({ rock: m, snow: m, sand: m, grass: m });
            return t;
        }
        function chShot(t, x, alt, fov) {
            scene.setCamera({
                fov: fov, near: 1, far: 4000000,
                position: [x, alt, 0], target: [x + 0.001, 0, 0], up: [0, 0, -1],
            });
            t.update(x, alt, 0);
            return scene.captureFrame();
        }
        // Mean over scanlines of each scanline's largest second difference in
        // green — 57d75804's statistic, on the channel this field drives. A
        // bilinear height is piecewise linear along a row, so its NORMAL is
        // piecewise constant and jumps once per texel; the second difference
        // of the shaded result spikes at every one of those jumps, and it is
        // those spikes a threshold on altitude turns into straight segments.
        // A C1 reconstruction has no spike to find. Per-row MAX rather than
        // mean, so the statistic stays on the kinks instead of drowning in the
        // 8-bit floor both paths share.
        function kinkEnergy(img) {
            let sum = 0, rows = 0;
            for (let y = 8; y < img.height - 8; y++) {
                let mx = 0;
                for (let x = 9; x < img.width - 9; x++) {
                    const i = (y * img.width + x) * 4 + 1;
                    const d2 = Math.abs(img.data[i - 4] - 2 * img.data[i] +
                                        img.data[i + 4]);
                    if (d2 > mx) mx = d2;
                }
                sum += mx; rows++;
            }
            return sum / rows;
        }

        // --- the gate, at two rungs -----------------------------------------
        //
        // cDesired is 1024 m in both. The rungs differ in the LAYER's texel
        // size, and therefore in the mip level the fragment reads:
        //
        //   texel 2048 m -> lod = 0      -> sampled texel 2048 m
        //   texel  448 m -> lod = 1.193  -> sampled texel 1024 m
        //
        // The second is the one that matters. There, a filter spaced at LEVEL
        // ZERO's 448 m would put all four fetches inside a single 1024 m
        // sampled texel, where the hardware is already exactly linear, and
        // would return trilinear to the bit — the same number the bilinear
        // path scores. Passing at that rung cannot happen by accident. Its lod
        // is deliberately FRACTIONAL as well, so the two-level blend is under
        // test and not just the level-1 spacing.
        //
        // The camera altitude is set per rung so that a sampled texel is ~20
        // pixels in both — inside the gate's fully-on region and far enough
        // above the 8-bit floor for the two numbers to be comparable. Content
        // sits at three sampled texels per period, near the top of the band a
        // chart at that rung can carry, which is what a coarse rung holds.
        {
            const rungs = [
                { texel: 2048, w: 64,  lod: '0',     alt: 37000 },
                { texel: 448,  w: 192, lod: '1.193', alt: 18500 },
            ];
            for (const r of rungs) {
                const sampled = Math.max(r.texel, 1024);
                const period  = 3 * sampled;
                const half    = r.w * r.texel / 2;
                const field   = makeLayer(r.w, r.w, -half, -half, r.texel,
                    (x) => 3000 + 1200 * Math.cos(2 * Math.PI * x / period));
                function shot(cubic) {
                    const t = chTerrain(cubic);
                    t.setHeightLayer(0, field);
                    const img = chShot(t, 0, r.alt, CH_FOV);
                    t.destroy();
                    return img;
                }
                const kOff = kinkEnergy(shot(false));
                const kOn  = kinkEnergy(shot(true));
                console.log(`  cubicHeight: ${r.texel} m texel at lod ${r.lod} — ` +
                            `kink energy bilinear ${kOff.toFixed(2)} -> cubic ` +
                            `${kOn.toFixed(2)} (${(kOff / kOn).toFixed(2)}x)`);
                assert(kOff > 2.5 * kOn,
                    `the mip-aware cubic removes the texel-lattice kinks in the ` +
                    `shading normal at a ${r.texel} m texel, lod ${r.lod} ` +
                    `(bilinear ${kOff.toFixed(2)}, cubic ${kOn.toFixed(2)})`);
            }
        }

        // --- the near field is untouched, to the bit ------------------------
        //
        // Two things at once, and they are the same fact. THE COST BOUND: the
        // gate is an exact 0 while a sampled texel is more than 128 pixels
        // wide, so a fragment looking at ground underfoot takes the single
        // trilinear fetch it always took — not a cheaper cubic, no cubic. And
        // THE COHERENCE: elevationAt / renderedElevationAt are camera-free and
        // therefore unchanged by this mode, which is only honest because the
        // surface they mirror has not moved either. Here that is asserted as
        // frame equality rather than argued: 150 m up over a 128 m texel, one
        // texel is 310 pixels wide and both modes draw the same bits.
        {
            const near = makeLayer(128, 128, -8192, -8192, 128,
                (x, z) => 60 * Math.sin(x / 700) * Math.cos(z / 540)
                        + 18 * Math.sin(x / 130 + z / 90));
            const probes = [[0, 0], [211, -389], [1500, 900]];
            function nearRun(cubic) {
                const t = chTerrain(cubic, { cellSize: 4, levels: 8,
                                             snowLine: 1e9 });
                t.setHeightLayer(0, near);
                const img = chShot(t, 0, 150, CH_FOV);
                const mirror = probes.map(([x, z]) =>
                    [t.elevationAt(x, z), t.renderedElevationAt(x, z)]);
                t.destroy();
                return { img, mirror };
            }
            const a = nearRun(false), b = nearRun(true);
            assert(bytesEqual(a.img, b.img),
                'the near field is bit-identical in both modes — the gate is ' +
                'off where a texel spans hundreds of pixels, so those ' +
                'fragments pay nothing and the CPU mirrors stay exact');
            probes.forEach(([x, z], i) => {
                assert(b.mirror[i][0] === a.mirror[i][0] &&
                       b.mirror[i][1] === a.mirror[i][1],
                    `the CPU elevation mirrors do not move at (${x}, ${z})`);
            });

            // ...and the frame equality above is not passing because nothing
            // was looking. The SAME terrain seen from orbit, where a sampled
            // texel is a handful of pixels, does move.
            function farRun(cubic) {
                const t = chTerrain(cubic, { cellSize: 4, levels: 8,
                                             snowLine: 1e9 });
                t.setHeightLayer(0, near);
                const img = chShot(t, 0, 60000, CH_FOV);
                t.destroy();
                return img;
            }
            assert(!bytesEqual(farRun(false), farRun(true)),
                'the near-field test has teeth — the same stack seen from a ' +
                'coarse rung does move');
        }

        // --- a constant field, edges included --------------------------------
        // The B-spline's weights are non-negative and sum to 1, so a constant
        // field must come back as that constant EVERYWHERE — including the
        // border texels, where the four fetches clamp into the band of texel
        // centres AT THE SAMPLED LEVEL. If that clamp were wrong the border
        // would read something else and this frame would move. It is the edge
        // behaviour asserted rather than described.
        {
            const k = makeLayer(64, 64, -65536, -65536, 2048, () => 3000);
            function edgeShot(cubic) {
                const t = chTerrain(cubic);
                t.setHeightLayer(0, k);
                // Aimed at the layer's own corner, so the assertion covers the
                // border texels and not just the interior.
                const img = chShot(t, 62000, CH_ALT, 60);
                t.destroy();
                return img;
            }
            assert(bytesEqual(edgeShot(false), edgeShot(true)),
                'a constant height field reconstructs identically under both ' +
                'filters, across the layer edge included');
        }

        // --- a periodic layer keeps wrapping ---------------------------------
        // A wrapX layer has no east-west edge, and cmLayer deliberately leaves
        // uv.x outside [0,1] for GL_REPEAT to resolve ACROSS MIP LEVELS. The
        // cubic tap must leave X alone on such a layer for the same reason; had
        // it clamped X the way it clamps a bounded layer, the reconstruction
        // would flatten into a band at the seam. The field below is periodic in
        // texel index with a period that divides the width, so the ground at
        // the seam is the SAME ground as half a chart away — two frames of the
        // same data, one straddling the seam and one not. They must agree, and
        // agree no worse than the bilinear path's own two frames do (which is
        // the honest tolerance: both carry the fp noise of a world X that
        // differs by 65 km).
        {
            const W = 64, MPC = 2048, PERIOD = 8;      // 8 texels | 64
            const half = W * MPC / 2;
            const data = new Float32Array(W * W);
            for (let j = 0; j < W; j++)
                for (let i = 0; i < W; i++)
                    data[j * W + i] = 3000 + 400 * Math.cos(2 * Math.PI * i / PERIOD);
            const wrapLayer = { data, width: W, height: W, originX: -half,
                                originZ: -half, metresPerCell: MPC, wrapX: true };
            // uv = 0 sits half a texel before texel 0's centre.
            const seamX = -half - MPC / 2;
            const midX  = seamX + 4 * PERIOD * MPC;    // the same data, 65 km east
            function wrapPair(cubic) {
                const t = chTerrain(cubic);
                t.setHeightLayer(0, wrapLayer);
                const a = chShot(t, seamX, CH_ALT, CH_FOV);
                const b = chShot(t, midX, CH_ALT, CH_FOV);
                t.destroy();
                return maxDiff(a, b);
            }
            const dOff = wrapPair(false), dOn = wrapPair(true);
            console.log(`  cubicHeight: periodic layer, seam vs mid — ` +
                        `bilinear ${dOff}, cubic ${dOn}`);
            assert(dOn <= dOff + 2,
                `the cubic tap keeps a periodic layer periodic across the seam ` +
                `(seam-vs-mid max difference ${dOn}, bilinear's own ${dOff})`);
            // ...and the comparison can see a seam when there is one: the same
            // data NOT declared periodic has a real east-west edge there, the
            // coverage ramp and the clamp both fire, and the two frames stop
            // being the same picture. (It does not isolate the tap clamp on its
            // own — the ramp is enough by itself — but it is what proves the
            // measurement above is not blind.)
            {
                const bounded = { data, width: W, height: W, originX: -half,
                                  originZ: -half, metresPerCell: MPC };
                const t = chTerrain(true);
                t.setHeightLayer(0, bounded);
                const a = chShot(t, seamX, CH_ALT, CH_FOV);
                const b = chShot(t, midX, CH_ALT, CH_FOV);
                t.destroy();
                assert(maxDiff(a, b) > 8,
                    `the periodic-seam comparison has teeth — the same data ` +
                    `without wrapX differs across the seam (${maxDiff(a, b)})`);
            }
        }

        // --- layerFade's exact zero survives ---------------------------------
        // A layer whose data has gone deeply sub-pixel is mixed in with an
        // EXACT 0.0, and mix(h, s, 0.0) is h to the bit — which is what lets
        // the new chain SKIP the reconstruction it would discard. The test does
        // not trust that: it replaces the faded layer's contents with a
        // completely different field and demands the frame not move by one bit.
        // That also proves the cubic fetches stay finite — a NaN arriving from
        // off the texture would survive the zero and poison the mix.
        {
            const coarse = makeLayer(64, 64, -65536, -65536, 2048,
                (x) => 3000 + 400 * Math.cos(2 * Math.PI * x / 6144));
            // 64 m texels against a 1024 m rendered cell: past the
            // 3.2T = 205 m at which cmLayerFade reaches an exact zero.
            function fine(fn) {
                return makeLayer(128, 128, -4096, -4096, 64, fn);
            }
            const quietFine = fine(() => 3000);
            const loudFine  = fine((x, z) => 3000 + 900 * Math.sin(x / 190)
                                                        * Math.cos(z / 150));
            function swap(cubic, fade) {
                const t = chTerrain(cubic, { layerFade: fade });
                t.setHeightLayer(1, coarse);
                t.setHeightLayer(0, quietFine);
                const a = chShot(t, 0, CH_ALT, CH_FOV);
                t.setHeightLayer(0, loudFine);
                const b = chShot(t, 0, CH_ALT, CH_FOV);
                t.destroy();
                return bytesEqual(a, b);
            }
            for (const cubic of [false, true]) {
                assert(swap(cubic, true),
                    `a faded-out height layer contributes nothing to the bit ` +
                    `(cubicHeight ${cubic})`);
                assert(!swap(cubic, false),
                    `the faded-layer test has teeth — unfaded, the same swap ` +
                    `changes the frame (cubicHeight ${cubic})`);
            }
        }

        // --- cost, at a ground camera and an orbital one ---------------------
        // Printed rather than gated: frame time is not a stable assertion in a
        // suite that shares a GPU with whatever else is running. The ground
        // camera's cost is asserted STRUCTURALLY instead, above — a frame that
        // is identical to the bit took the same fetches. The orbital number is
        // here so a regression in the gate has somewhere to show up.
        {
            const fld = makeLayer(64, 64, -65536, -65536, 2048,
                (x, z) => 3000 + 400 * Math.cos(2 * Math.PI * x / 6144)
                               + 300 * Math.sin(2 * Math.PI * z / 5000));
            function timeAt(cubic, alt, fov) {
                const t = chTerrain(cubic);
                t.setHeightLayer(0, fld);
                chShot(t, 0, alt, fov);                 // warm
                let best = Infinity;
                for (let run = 0; run < 5; run++) {
                    const t0 = Date.now();
                    for (let i = 0; i < 200; i++) chShot(t, 0, alt, fov);
                    const dt = (Date.now() - t0) / 200;
                    if (dt < best) best = dt;
                }
                t.destroy();
                return best;
            }
            for (const [name, alt, fov] of [['ground', 150, CH_FOV],
                                            ['orbit', CH_ALT, CH_FOV]]) {
                const a = timeAt(false, alt, fov), b = timeAt(true, alt, fov);
                console.log(`  cubicHeight: ${name} camera — ` +
                            `${a.toFixed(3)} ms -> ${b.toFixed(3)} ms ` +
                            `(${(b / a).toFixed(2)}x)`);
            }
        }

        sun.direction = chSunWas.direction;
        sun.intensity = chSunWas.intensity;
    }

    console.log('clipmap terrain test passed');
}

document.body.removeChild(canvas);
flush();
