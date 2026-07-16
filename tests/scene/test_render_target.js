// Test render-target plumbing in the 3D scene renderer — the depth-texture
// FBO attachment (post-tonemap unlit-overlay occlusion), soft particles
// (scene-depth alpha fade in particles3d.frag, gated per Particles3DNode by
// `softness`), setRenderScale (internal FBO chain at canvas * scale), and
// setMSAA (multisampled HDR passes resolved before tonemap). Exercises
// src/scene/scene_renderer.cpp (ensureMeshFBO / ensureMSAAFBO / resolve
// ordering), scene_renderer_particles.cpp (scene-depth snapshot blit),
// scene_renderer_postfx.cpp (shared depth attachment, scaled chains), and
// the setRenderScale / setMSAA / softness bindings in scene_bindings.cpp.

function near(a, b, tol, label) {
    assert(Math.abs(a - b) < tol, `${label}: ${a} vs ${b}`);
}

// Mean of one channel (0=r 1=g 2=b 3=a) over a pixel rect.
function regionChannel(img, ch, x0, y0, x1, y1) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            sum += img.data[(y * img.width + x) * 4 + ch];
            n++;
        }
    }
    return n ? sum / n : 0;
}

function centerChannel(img, ch, r) {
    const cx = Math.floor(img.width / 2), cy = Math.floor(img.height / 2);
    return regionChannel(img, ch, cx - r, cy - r, cx + r, cy + r);
}

// Count pixels whose red channel is neither background-dark nor interior-
// bright — the fractional-coverage pixels MSAA produces along an edge.
function intermediateCount(img) {
    let n = 0;
    for (let i = 0; i < img.data.length; i += 4) {
        const r = img.data[i];
        if (r > 30 && r < 225) n++;
    }
    return n;
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

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

const probe = freshScene(64);
if (!probe.scene) {
    console.log('scene context not available (no GPU) — skipping render target test');
} else {
    dropScene(probe);
    const SIZE = 128;

    // =====================================================================
    // Depth-texture refactor: the post-tonemap unlit overlay still depth-
    // tests against the scene (it shares the mesh FBO's depth attachment,
    // now a texture). An unlit green box hidden behind an opaque red box
    // must stay hidden — with and without MSAA (resolved depth path).
    // =====================================================================
    {
        for (const samples of [0, 4]) {
            const s = freshScene(SIZE);
            if (samples) s.scene.setMSAA(samples);
            s.scene.createMesh({ mesh: 'box', color: '#ff0000', z: 0 });
            s.scene.createMesh({ mesh: 'box', color: '#00ff00', z: -2, unlit: true });
            const img = s.scene.captureFrame();
            const red = centerChannel(img, 0, 8);
            const green = centerChannel(img, 1, 8);
            assert(red > 60, `[msaa=${samples}] front box renders red (${red})`);
            assert(green < 20,
                `[msaa=${samples}] unlit overlay is occluded by scene depth (${green})`);
            dropScene(s);
        }
        // And an unoccluded unlit mesh still draws (overlay pass alive).
        const s = freshScene(SIZE);
        s.scene.createMesh({ mesh: 'box', color: '#00ff00', z: 0, unlit: true });
        const g = centerChannel(s.scene.captureFrame(), 1, 8);
        assert(g > 200, `unoccluded unlit mesh draws at authored color (${g})`);
        dropScene(s);
    }

    // =====================================================================
    // Soft particles: a red particle hovering 0.25 world units in front of
    // a wall. softness 2.0 => alpha *= gap/softness = 0.125, so the patch
    // dims by ~8x vs softness 0. With no wall behind (depth = far) the
    // fade factor saturates at 1 and brightness returns.
    // =====================================================================
    {
        const emitterOpts = (softness) => ({
            seed: 7, rate: 0, burst: 1, maxParticles: 4,
            lifetime: 10, velocity: { speed: 0 },
            position: [0, 0, -1.7],
            size: { start: 0.8, end: 0.8 },
            color: { start: '#ff0000', end: '#ff0000' },
            softness,
        });
        const wallOpts = { mesh: 'box', halfW: 2, halfH: 2, halfD: 0.05,
                           z: -2, color: '#000020' };
        const capture = (withWall, softness, msaa) => {
            const s = freshScene(SIZE);
            if (msaa) s.scene.setMSAA(msaa);
            if (withWall) s.scene.createMesh(wallOpts);
            const em = s.scene.createParticles3D(emitterOpts(softness));
            assert(Math.abs(em.softness - softness) < 1e-6,
                `softness opt round-trips through the getter (${em.softness})`);
            advanceTime(66);
            const r = centerChannel(s.scene.captureFrame(), 0, 5);
            dropScene(s);
            return r;
        };

        const hard = capture(true, 0, 0);
        const soft = capture(true, 2.0, 0);
        const noWall = capture(false, 2.0, 0);
        assert(hard > 40, `hard particle is clearly visible over the wall (${hard})`);
        assert(soft < hard * 0.3,
            `softness fades alpha near intersecting geometry (${soft} vs ${hard})`);
        assert(soft > 3, `soft particle fades, not vanishes (${soft})`);
        assert(noWall > hard * 0.8,
            `no geometry behind -> no fade (${noWall} vs ${hard})`);

        // Ordering under MSAA: the depth snapshot must source the resolved
        // depth, so the fade behaves identically with multisampling on.
        const softMsaa = capture(true, 2.0, 4);
        assert(softMsaa < hard * 0.3,
            `soft fade works under MSAA (${softMsaa} vs ${hard})`);

        // Live setter: flipping softness on an existing emitter takes
        // effect next frame.
        const s = freshScene(SIZE);
        s.scene.createMesh(wallOpts);
        const em = s.scene.createParticles3D(emitterOpts(0));
        advanceTime(66);
        const before = centerChannel(s.scene.captureFrame(), 0, 5);
        em.softness = 2.0;
        const after = centerChannel(s.scene.captureFrame(), 0, 5);
        assert(after < before * 0.3,
            `softness setter applies to a live system (${after} vs ${before})`);
        dropScene(s);
    }

    // =====================================================================
    // Render scale: internal targets shrink to canvas * scale (captureFrame
    // reads the internal target, so dimensions expose it) while the scene
    // still renders. Clamped to [0.25, 2.0].
    // =====================================================================
    {
        const s = freshScene(SIZE);
        s.scene.createMesh({ mesh: 'box', color: '#ff0000' });

        assert(s.scene.renderScale === 1, 'renderScale defaults to 1');
        const full = s.scene.captureFrame();
        assert(full.width === SIZE && full.height === SIZE,
            `scale 1 renders at canvas size (${full.width}x${full.height})`);
        const fullRed = centerChannel(full, 0, 8);
        assert(fullRed > 60, `box renders at scale 1 (${fullRed})`);

        s.scene.setRenderScale(0.5);
        near(s.scene.renderScale, 0.5, 1e-6, 'setRenderScale getter round-trips');
        const half = s.scene.captureFrame();
        assert(half.width === SIZE / 2 && half.height === SIZE / 2,
            `scale 0.5 halves the internal target (${half.width}x${half.height})`);
        const halfRed = centerChannel(half, 0, 4);
        assert(halfRed > 60, `scene still renders at scale 0.5 (${halfRed})`);
        const halfCorner = regionChannel(half, 0, 0, 0, 8, 8);
        assert(halfCorner < 10, `corners stay empty at scale 0.5 (${halfCorner})`);

        // Property-setter path + supersampling.
        s.scene.renderScale = 2;
        const dbl = s.scene.captureFrame();
        assert(dbl.width === SIZE * 2, `scale 2 supersamples (${dbl.width})`);
        assert(centerChannel(dbl, 0, 16) > 60, 'scene renders at scale 2');

        // Clamping.
        s.scene.setRenderScale(0.05);
        near(s.scene.renderScale, 0.25, 1e-6, 'scale clamps up to 0.25');
        s.scene.setRenderScale(9);
        near(s.scene.renderScale, 2.0, 1e-6, 'scale clamps down to 2.0');

        // Restore + everything back to normal.
        s.scene.setRenderScale(1);
        const back = s.scene.captureFrame();
        assert(back.width === SIZE, 'scale 1 restores canvas-sized target');
        dropScene(s);
    }

    // =====================================================================
    // MSAA: a rotated white box on black background. Binary rasterization
    // leaves edge pixels either full or empty; MSAA resolve produces
    // fractional-coverage intermediates along the slanted edges.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        s.scene.setAmbient([1, 1, 1]);
        s.scene.createMesh({ mesh: 'box', color: '#ffffff', rz: 30 });

        assert(s.scene.msaa === 0, 'msaa defaults to 0 (off)');
        const off = s.scene.captureFrame();
        const offMid = intermediateCount(off);

        s.scene.setMSAA(4);
        assert(s.scene.msaa === 4, 'setMSAA getter round-trips');
        const on = s.scene.captureFrame();
        const onMid = intermediateCount(on);

        assert(centerChannel(off, 0, 8) > 200 && centerChannel(on, 0, 8) > 200,
            'box interior stays solid white with and without MSAA');
        assert(onMid >= offMid + 20,
            `MSAA smooths the slanted edges (intermediates ${offMid} -> ${onMid})`);

        // Toggle back off: edge crispness returns.
        s.scene.setMSAA(0);
        const offAgain = intermediateCount(s.scene.captureFrame());
        assert(offAgain < onMid,
            `setMSAA(0) turns multisampling back off (${offAgain} vs ${onMid})`);

        // 1 sample is off; property-setter path works.
        s.scene.setMSAA(1);
        assert(s.scene.msaa === 0, 'setMSAA(1) means off');
        s.scene.msaa = 8;
        assert(s.scene.msaa === 8, 'msaa property setter works');
        dropScene(s);
    }

    // =====================================================================
    // Combined: MSAA + render scale + soft particles in one frame — the
    // full plumbing coexists and produces a sane frame.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        s.scene.setMSAA(4);
        s.scene.setRenderScale(0.5);
        s.scene.createMesh({ mesh: 'box', halfW: 2, halfH: 2, halfD: 0.05,
                             z: -2, color: '#000020' });
        s.scene.createParticles3D({
            seed: 7, rate: 0, burst: 1, maxParticles: 4,
            lifetime: 10, velocity: { speed: 0 },
            position: [0, 0, -1.7],
            size: { start: 0.8, end: 0.8 },
            color: { start: '#ff0000', end: '#ff0000' },
            softness: 2.0,
        });
        advanceTime(66);
        const img = s.scene.captureFrame();
        assert(img.width === SIZE / 2, `combined frame renders at scale (${img.width})`);
        const red = centerChannel(img, 0, 4);
        assert(red > 1 && red < 60,
            `soft particle still fades with MSAA + scale (${red})`);
        dropScene(s);
    }

    console.log('render target tests passed');
}
