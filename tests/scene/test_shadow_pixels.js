// Shadows on the GPU path, read back as pixels: each light type darkens the
// ground where a caster blocks it, and the per-mesh and per-light switches
// (receivesShadow, castsShadow, shadowBias) and the global quality
// (setShadowQuality) change what is drawn.
//
// The lit/shadowed probes are mirror images about the caster, so both see
// the same light and differ only by the occluder.

function patchBrightness(img, cx, cy, r) {
    let sum = 0, n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            const i = (y * img.width + x) * 4;
            sum += (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
            n++;
        }
    }
    return n ? sum / n : 0;
}

function diffCount(a, b, tol) {
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        if (Math.abs(a.data[i] - b.data[i]) > tol || Math.abs(a.data[i + 1] - b.data[i + 1]) > tol ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > tol) n++;
    }
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

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

// Brightness of the ground at world (x, 0, z).
function ground(scn, img, x, z) {
    const p = scn.projectLocal(x, 0, z);
    assert(p && !p.behind, `ground point (${x}, ${z}) is on screen`);
    return patchBrightness(img, Math.round(p.x), Math.round(p.y), 2);
}

function setup(size) {
    const s = freshScene(size);
    const scn = s.scene;
    scn.setCamera({ fov: 50, near: 0.1, far: 80, position: [0, 9, 9], target: [0, 0, 0] });
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scn.setAmbient([0.02, 0.02, 0.02]);
    const floor = scn.createMesh({ mesh: 'plane', halfW: 12, halfD: 12, color: [0.8, 0.8, 0.8],
                                   roughness: 1, castsShadow: false });
    return { s, scn, floor };
}

const probe = freshScene(64);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);

    // ---- Directional: shadow, receivesShadow, castsShadow, shadowBias ------
    {
        const { s, scn, floor } = setup(192);
        // A 2 m column; the sun falls at 45 degrees toward +x, so the shadow
        // runs from x = 0.3 to x = 2.3 along z = 0.
        const box = scn.createMesh({ mesh: 'box', halfW: 0.3, halfH: 1, halfD: 0.3, color: 'white', y: 1 });
        const sun = scn.createLight({ type: 'directional', direction: [1, -1, 0], intensity: 3 });
        sun.castsShadow = true;

        let f = scn.captureFrame();
        const lit = ground(scn, f, -1.4, 0);
        const shaded = ground(scn, f, 1.4, 0);
        assert(lit > 60, `the sunlit ground is bright (${lit.toFixed(1)})`);
        assert(shaded < lit * 0.35, `the column shadows the ground (${shaded.toFixed(1)} vs ${lit.toFixed(1)})`);

        floor.receivesShadow = false;
        f = scn.captureFrame();
        assert(ground(scn, f, 1.4, 0) > lit * 0.9, 'receivesShadow=false ignores the shadow');
        floor.receivesShadow = true;

        box.castsShadow = false;
        f = scn.captureFrame();
        assert(ground(scn, f, 1.4, 0) > lit * 0.9, 'castsShadow=false casts nothing');
        box.castsShadow = true;

        // A constant bias past the whole depth range compares every
        // receiver in front of its caster.
        sun.shadowBias = 1.0;
        f = scn.captureFrame();
        assert(ground(scn, f, 1.4, 0) > lit * 0.9, 'a huge shadowBias removes the shadow');
        sun.shadowBias = 0;

        sun.castsShadow = false;
        f = scn.captureFrame();
        assert(ground(scn, f, 1.4, 0) > lit * 0.9, 'an unshadowed light lights everything');
        dropScene(s);
    }

    // ---- Spot: the cone, and a spot shadow ---------------------------------
    {
        const { s, scn } = setup(192);
        const spot = scn.createLight({ type: 'spot', position: [0, 6, 0], direction: [0, -1, 0],
                                       range: 20, intensity: 60, innerAngle: 0.45, outerAngle: 0.55 });
        let f = scn.captureFrame();
        const inside = ground(scn, f, 1.0, 0);
        const outside = ground(scn, f, 5.0, 0);
        assert(inside > 50, `inside the cone is lit (${inside.toFixed(1)})`);
        assert(outside < inside * 0.1, `outside the cone is dark (${outside.toFixed(1)} vs ${inside.toFixed(1)})`);

        // A block at x = 1, 2 m below the light, throws its shadow to x = 1.33
        // on the ground; x = -1.33 is the same distance into the cone.
        scn.createMesh({ mesh: 'box', halfW: 0.25, halfH: 0.25, halfD: 0.25, color: 'white', x: 1, y: 1.5 });
        spot.castsShadow = true;
        f = scn.captureFrame();
        const litSide = ground(scn, f, -1.33, 0);
        const shadowSide = ground(scn, f, 1.33, 0);
        assert(shadowSide < litSide * 0.35,
            `the spot shadow darkens the ground (${shadowSide.toFixed(1)} vs ${litSide.toFixed(1)})`);
        dropScene(s);
    }

    // ---- Point: a cube-face shadow on each side ----------------------------
    {
        const { s, scn } = setup(192);
        const lamp = scn.createLight({ type: 'point', position: [0, 4, 0], range: 30, intensity: 40 });
        lamp.castsShadow = true;
        // Two blockers on different cube faces (+x and -z); shadows at 2.4 m.
        scn.createMesh({ mesh: 'box', halfW: 0.25, halfH: 0.25, halfD: 0.25, color: 'white', x: 1.5, y: 1.5 });
        scn.createMesh({ mesh: 'box', halfW: 0.25, halfH: 0.25, halfD: 0.25, color: 'white', z: -1.5, y: 1.5 });
        const f = scn.captureFrame();
        const st = scn.cullStats();
        assert(st.shadowTilesTotal === 6, `a point light takes six tiles (${st.shadowTilesTotal})`);
        const litX = ground(scn, f, -2.4, 0), shadowX = ground(scn, f, 2.4, 0);
        const shadowZ = ground(scn, f, 0, -2.4);
        assert(litX > 40, `the lamp lights the ground (${litX.toFixed(1)})`);
        assert(shadowX < litX * 0.35, `+x face shadow (${shadowX.toFixed(1)} vs ${litX.toFixed(1)})`);
        assert(shadowZ < litX * 0.35, `-z face shadow (${shadowZ.toFixed(1)} vs ${litX.toFixed(1)})`);
        dropScene(s);
    }

    // ---- Cascades: a near and a far caster each shadow their own spot ------
    {
        const s = freshScene(192);
        const scn = s.scene;
        scn.setCamera({ fov: 50, near: 0.1, far: 120, position: [0, 14, 10], target: [0, 0, -16] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn.setAmbient([0.02, 0.02, 0.02]);
        scn.createMesh({ mesh: 'plane', halfW: 60, halfD: 60, color: [0.8, 0.8, 0.8], roughness: 1,
                         castsShadow: false });
        scn.createMesh({ mesh: 'box', halfW: 0.4, halfH: 1, halfD: 0.4, color: 'white', y: 1, z: -4 });
        scn.createMesh({ mesh: 'box', halfW: 1, halfH: 2.5, halfD: 3, color: 'white', y: 2.5, z: -36 });
        const sun = scn.createLight({ type: 'directional', direction: [1, -1, 0], intensity: 3 });
        sun.castsShadow = true;
        sun.cascadeCount = 4;
        const f = scn.captureFrame();
        assert(scn.cullStats().shadowTilesTotal === 4, 'four cascades take four tiles');
        const nearLit = ground(scn, f, -1.5, -4), nearShadow = ground(scn, f, 1.5, -4);
        const farLit = ground(scn, f, -3.5, -36), farShadow = ground(scn, f, 3.5, -36);
        assert(nearShadow < nearLit * 0.35, `near cascade shadow (${nearShadow.toFixed(1)} vs ${nearLit.toFixed(1)})`);
        assert(farShadow < farLit * 0.35, `far cascade shadow (${farShadow.toFixed(1)} vs ${farLit.toFixed(1)})`);
        dropScene(s);
    }

    // ---- Quality: the PCF grid and the atlas size change the edges ---------
    {
        const { s, scn } = setup(192);
        scn.createMesh({ mesh: 'sphere', radius: 0.8, color: 'white', y: 1.2 });
        const sun = scn.createLight({ type: 'directional', direction: [0.4, -1, 0.2], intensity: 3 });
        sun.castsShadow = true;
        sun.cascadeCount = 1;
        scn.setShadowQuality({ atlasSize: 4096, pcfTaps: 1 });
        const hard = scn.captureFrame();
        scn.setShadowQuality({ atlasSize: 4096, pcfTaps: 5 });
        const soft = scn.captureFrame();
        assert(diffCount(hard, soft, 2) > 20, `PCF 5x5 softens the edge (${diffCount(hard, soft, 2)} px)`);
        scn.setShadowQuality({ atlasSize: 256, pcfTaps: 1 });
        const coarse = scn.captureFrame();
        assert(diffCount(hard, coarse, 2) > 20, `a 256 atlas changes the edge (${diffCount(hard, coarse, 2)} px)`);
        // Still a shadow at either size.
        const under = scn.projectLocal(0.48, 0, 0.24);
        assert(patchBrightness(coarse, Math.round(under.x), Math.round(under.y), 1) <
               patchBrightness(coarse, Math.round(scn.projectLocal(-3, 0, 0).x),
                               Math.round(scn.projectLocal(-3, 0, 0).y), 1) * 0.5,
            'the coarse atlas still shadows the core');
        dropScene(s);
    }
}
