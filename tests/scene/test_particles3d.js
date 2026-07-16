// Test scene.createParticles3D — the world-space 3D particle system.
// Exercises src/scene/particles3d_node.cpp, the particle pass in
// src/scene/scene_renderer_particles.cpp, and the Particles3D bindings in
// src/js/scene_bindings.cpp: emission + rendering into the 3D pass, the
// gravity arc against the analytic parabola, color-over-life in captured
// pixels, additive vs normal blend brightness, one-shot drain + onFinished
// (including deferred self-destroy), world-space trail vs local-space
// follow, immediate burst, the maxParticles cap, and seeded determinism.
// All timing goes through advanceTime() virtual time; captures overshoot
// durations (+50 ms) because float dt sums can land epsilon-under a target.

function near(a, b, tol, label) {
    assert(Math.abs(a - b) < tol, `${label}: ${a} vs ${b}`);
}

// Mean of R,G,B over a pixel rect (uncovered pixels are RGBA(0,0,0,0)).
function regionBrightness(img, x0, y0, x1, y1) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            const i = (y * img.width + x) * 4;
            sum += (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
            n++;
        }
    }
    return n ? sum / n : 0;
}

function avgBrightness(img) {
    return regionBrightness(img, 0, 0, img.width, img.height);
}

// Brightness-weighted row centroid — tracks where the particle cloud sits
// vertically on screen (row index grows downward).
function centroidY(img) {
    let sum = 0, wsum = 0;
    for (let y = 0; y < img.height; y++) {
        for (let x = 0; x < img.width; x++) {
            const i = (y * img.width + x) * 4;
            const b = (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
            sum += b;
            wsum += b * y;
        }
    }
    return sum > 0 ? wsum / sum : -1;
}

function channelSums(img) {
    let r = 0, g = 0, b = 0;
    for (let i = 0; i < img.data.length; i += 4) {
        r += img.data[i];
        g += img.data[i + 1];
        b += img.data[i + 2];
    }
    return { r, g, b };
}

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 60, near: 0.1, far: 100,
                       position: [0, 0, 4], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    }
    return { canvas: cv, scene: sc };
}

const probe = freshScene(64);
if (!probe.scene) {
    console.log('scene context not available (no GPU) — skipping particles3d test');
} else {
    document.body.removeChild(probe.canvas);
    const SIZE = 128;

    // ------------------------------------------------------------------
    // Burst is immediate; maxParticles caps; basic getters
    // ------------------------------------------------------------------
    {
        const s = freshScene(SIZE);
        const em = s.scene.createParticles3D({
            name: 'caps', seed: 7, rate: 0, maxParticles: 50, lifetime: 5,
            velocity: { speed: 0 },
        });
        assert(em.type === 'particles3d', 'node type is particles3d');
        assert(em.particleCount === 0, 'starts empty');
        em.burst(30);
        assert(em.particleCount === 30, `burst(30) spawns immediately (${em.particleCount})`);
        em.burst(500);
        assert(em.particleCount === 50, `maxParticles caps the pool (${em.particleCount})`);
        assert(em.liveCount === 50, 'liveCount alias matches');
        assert(em.isPlaying === true, 'playing by default');
        em.clear();
        assert(em.particleCount === 0, 'clear() kills all live particles');
        document.body.removeChild(s.canvas);
    }

    // ------------------------------------------------------------------
    // Emitted particles appear in the expected screen region
    // ------------------------------------------------------------------
    {
        const s = freshScene(SIZE);
        s.scene.createParticles3D({
            seed: 11, rate: 0, burst: 100, maxParticles: 200,
            lifetime: 5, velocity: { speed: 0 },
            size: { start: 0.5, end: 0.5 },
            color: { start: '#ffffff', end: '#ffffff' },
        });
        advanceTime(66);
        const img = s.scene.captureFrame();
        assert(img !== null, 'captureFrame returns pixels');
        const t = Math.floor(SIZE / 3);
        const center = regionBrightness(img, t, t, 2 * t, 2 * t);
        const corner = regionBrightness(img, 0, 0, 16, 16);
        assert(center > 8, `particles light up the screen center (${center})`);
        assert(corner < 2, `corners stay empty (${corner})`);
        document.body.removeChild(s.canvas);
    }

    // ------------------------------------------------------------------
    // Gravity arc: cloud centroid falls following y(t) = -g t^2 / 2
    // ------------------------------------------------------------------
    {
        const s = freshScene(SIZE);
        s.scene.createParticles3D({
            seed: 3, rate: 0, burst: 150, maxParticles: 200,
            lifetime: 10, velocity: { speed: 0 },
            gravity: [0, -5, 0],
            size: { start: 0.3, end: 0.3 },
            color: { start: '#ffffff', end: '#ffffff' },
        });
        // Screen-space prediction: camera at z=4 looking at origin, fov 60
        // => half-height at the z=0 plane is tan(30) * 4 ~ 2.309 world units
        // mapping to SIZE/2 pixels.
        const worldToPx = (SIZE / 2) / (Math.tan(Math.PI / 6) * 4);
        const rowFor = (y) => SIZE / 2 - y * worldToPx;

        advanceTime(216);                        // ~0.2 s
        const imgA = s.scene.captureFrame();
        const cA = centroidY(imgA);
        advanceTime(600);                        // ~0.8 s total
        const imgB = s.scene.captureFrame();
        const cB = centroidY(imgB);

        assert(cA >= 0 && cB >= 0, 'both captures contain particles');
        assert(cB > cA + 10, `cloud fell between captures (${cA} -> ${cB})`);
        // Analytic check with a tolerant band (the exact virtual time is a
        // multiple of 16 ms, and the soft-point falloff blurs the centroid).
        const yA = -0.5 * 5 * 0.216 * 0.216;
        const yB = -0.5 * 5 * 0.816 * 0.816;
        near(cA, rowFor(yA), 12, 'early centroid near analytic parabola');
        near(cB, rowFor(yB), 15, 'late centroid near analytic parabola');
        document.body.removeChild(s.canvas);
    }

    // ------------------------------------------------------------------
    // Color-over-life shifts the captured hue red -> blue
    // ------------------------------------------------------------------
    {
        const s = freshScene(SIZE);
        s.scene.createParticles3D({
            seed: 5, rate: 0, burst: 100, maxParticles: 200,
            lifetime: 1.0, velocity: { speed: 0 },
            size: { start: 0.6, end: 0.6 },
            color: { start: '#ff0000', end: '#0000ff' },
        });
        // The lerp runs in linear space, so the "off" channel keeps a small
        // sRGB-visible tail near the ends — assert dominance, not purity.
        advanceTime(66);                          // u ~ 0.07: red end
        const early = channelSums(s.scene.captureFrame());
        assert(early.r > early.b * 2, `early frame is red-dominant (r=${early.r} b=${early.b})`);
        advanceTime(850);                         // u ~ 0.92: blue end
        const late = channelSums(s.scene.captureFrame());
        assert(late.b > late.r * 2, `late frame is blue-dominant (r=${late.r} b=${late.b})`);
        assert(late.r < early.r && late.b > early.b, 'hue crossfades over life');
        document.body.removeChild(s.canvas);
    }

    // ------------------------------------------------------------------
    // Additive blend stacks brighter than normal blend
    // ------------------------------------------------------------------
    {
        const opts = {
            seed: 9, rate: 0, burst: 120, maxParticles: 200,
            lifetime: 5, velocity: { speed: 0 },
            size: { start: 0.5, end: 0.5 },
            color: { start: '#606060', end: '#606060' },
        };
        const sN = freshScene(SIZE);
        sN.scene.createParticles3D(Object.assign({ blend: 'normal' }, opts));
        advanceTime(66);
        const normal = avgBrightness(sN.scene.captureFrame());
        document.body.removeChild(sN.canvas);

        const sA = freshScene(SIZE);
        sA.scene.createParticles3D(Object.assign({ blend: 'additive' }, opts));
        advanceTime(66);
        const additive = avgBrightness(sA.scene.captureFrame());
        document.body.removeChild(sA.canvas);

        assert(additive > normal * 1.5,
            `additive stacks brighter than normal (${additive} vs ${normal})`);
    }

    // ------------------------------------------------------------------
    // One-shot: emits for `duration`, drains, fires onFinished exactly once
    // ------------------------------------------------------------------
    {
        const s = freshScene(SIZE);
        let finishes = 0;
        const em = s.scene.createParticles3D({
            seed: 13, rate: 200, duration: 0.25, lifetime: 0.15,
            maxParticles: 200, velocity: { speed: 0.5, spread: 180 },
            onFinished: () => finishes++,
        });
        advanceTime(100);
        assert(em.particleCount > 0, 'one-shot emits during its window');
        assert(finishes === 0, 'onFinished not fired while alive');
        advanceTime(600);                         // window 250 + life 150 + slack
        assert(em.particleCount === 0, `one-shot drained (${em.particleCount})`);
        assert(finishes === 1, `onFinished fired exactly once (${finishes})`);
        assert(em.isPlaying === false, 'one-shot deactivates after finishing');
        advanceTime(300);
        assert(finishes === 1, 'onFinished does not re-fire');

        // play() restarts the window and re-arms onFinished.
        em.play();
        assert(em.isPlaying === true, 'play() restarts a finished one-shot');
        advanceTime(700);
        assert(finishes === 2, `restart fires onFinished again (${finishes})`);
        document.body.removeChild(s.canvas);
    }

    // ------------------------------------------------------------------
    // Deferred destroy: onFinished may destroy its own emitter node
    // ------------------------------------------------------------------
    {
        const s = freshScene(SIZE);
        let finishes = 0;
        const em = s.scene.createParticles3D({
            name: 'selfdestruct',
            seed: 17, rate: 100, duration: 0.1, lifetime: 0.1, maxParticles: 64,
        });
        em.onFinished = () => { finishes++; em.destroy(); };
        // A bystander system keeps ticking through the destroy.
        const other = s.scene.createParticles3D({ seed: 1, rate: 50, lifetime: 0.5 });
        advanceTime(500);
        assert(finishes === 1, `self-destroying onFinished ran once (${finishes})`);
        assert(s.scene.findByName('selfdestruct') === null, 'node destroyed from its own callback');
        assert(other.particleCount > 0, 'other systems keep simulating');
        advanceTime(200);                         // no crash after the destroy
        document.body.removeChild(s.canvas);
    }

    // ------------------------------------------------------------------
    // World space leaves a trail behind a moving emitter; local follows it
    // ------------------------------------------------------------------
    {
        const common = {
            rate: 400, maxParticles: 600, lifetime: 5,
            velocity: { speed: 0 }, size: { start: 0.3, end: 0.3 },
            color: { start: '#ffffff', end: '#ffffff' },
        };
        const mid = SIZE / 2;

        const sW = freshScene(SIZE);
        const emW = sW.scene.createParticles3D(
            Object.assign({ seed: 21, space: 'world', position: [-1.5, 0, 0] }, common));
        advanceTime(300);
        emW.position = [1.5, 0, 0];
        advanceTime(300);
        const imgW = sW.scene.captureFrame();
        const wLeft  = regionBrightness(imgW, 0, 0, mid, SIZE);
        const wRight = regionBrightness(imgW, mid, 0, SIZE, SIZE);
        // Absolute levels are small — a saturated ~8 px disc averaged over a
        // 64x128 half-region — so assert presence, not intensity.
        assert(wLeft > 0.5, `world space: trail persists at the old position (${wLeft})`);
        assert(wRight > 0.5, `world space: new particles at the new position (${wRight})`);
        document.body.removeChild(sW.canvas);

        const sL = freshScene(SIZE);
        const emL = sL.scene.createParticles3D(
            Object.assign({ seed: 21, space: 'local', position: [-1.5, 0, 0] }, common));
        advanceTime(300);
        emL.position = [1.5, 0, 0];
        advanceTime(300);
        const imgL = sL.scene.captureFrame();
        const lLeft  = regionBrightness(imgL, 0, 0, mid, SIZE);
        const lRight = regionBrightness(imgL, mid, 0, SIZE, SIZE);
        assert(lRight > 0.5, `local space: cloud renders at the node (${lRight})`);
        assert(lLeft < lRight * 0.05,
            `local space: whole cloud rides the node, no trail (${lLeft} vs ${lRight})`);
        document.body.removeChild(sL.canvas);
    }

    // ------------------------------------------------------------------
    // Determinism: same seed + same dt steps => identical pixels
    // ------------------------------------------------------------------
    {
        const runOnce = () => {
            const s = freshScene(SIZE);
            s.scene.createParticles3D({
                seed: 1234, shape: { type: 'sphere', radius: 0.6 },
                rate: 150, burst: 40, maxParticles: 400,
                lifetime: { min: 0.5, max: 2.0 },
                velocity: { speed: 1.2, speedSpread: 0.8, spread: 45 },
                gravity: [0, -2, 0], drag: 0.7,
                size: { start: 0.25, end: 0.05 },
                color: { start: '#ffcc88', end: '#3355ff' },
                rotation: { start: 0, spinSpeed: 90, spinSpread: 180 },
            });
            advanceTime(500);
            const img = s.scene.captureFrame();
            document.body.removeChild(s.canvas);
            return img;
        };
        const a = runOnce();
        const b = runOnce();
        assert(a.data.length === b.data.length, 'capture sizes match');
        let diff = 0;
        for (let i = 0; i < a.data.length; i++) {
            if (a.data[i] !== b.data[i]) diff++;
        }
        assert(avgBrightness(a) > 1, 'deterministic scene actually rendered particles');
        assert(diff === 0, `same seed + same steps reproduce exactly (${diff} bytes differ)`);
    }

    // ------------------------------------------------------------------
    // stop() halts emission but live particles finish; rate setter works
    // ------------------------------------------------------------------
    {
        const s = freshScene(SIZE);
        const em = s.scene.createParticles3D({
            seed: 2, rate: 100, lifetime: 0.3, maxParticles: 200,
        });
        advanceTime(200);
        assert(em.particleCount > 0, 'rate emission produced particles');
        assert(Math.abs(em.rate - 100) < 1e-6, 'rate getter round-trips');
        em.stop();
        assert(em.isPlaying === false, 'stop() halts emission');
        advanceTime(400);                         // life 0.3 + slack
        assert(em.particleCount === 0, 'existing particles drain after stop()');
        em.rate = 50;
        assert(Math.abs(em.rate - 50) < 1e-6, 'rate setter works');
        document.body.removeChild(s.canvas);
    }

    console.log('particles3d tests passed');
}
