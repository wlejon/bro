// Test the skeletal animation player on SkinnedMeshNode — setSkeleton /
// addClip / play / crossfade / masked layer / pause / scrub / finished
// callback / getBoneWorldMatrix. Exercises src/scene/animation_player.cpp
// and the player bindings in src/js/scene_bindings.cpp.
//
// Rig: the same procedural 2-bone strip as test_skinned_mesh.js (flat
// vertical strip split rigidly at the y=1 hinge; bone 1's hinge at (0,1,0)).
// Two clips animate bone 1: "bend" rotates about Z, "twist" about Y, both
// identity → 90 deg over 1 s. Bone 1's model-space world matrix is
// analytically T(0,1,0) * R(q(t)), so getBoneWorldMatrix is checked against
// closed forms at the endpoints and against the SAME rigging pipeline
// (Animation.evaluate → Pose.computeWorldMatrices) at fractional times —
// verifying the C++ player's evaluate/blend/palette plumbing end to end.
// All time advance goes through advanceTime() virtual time.

function patchMaxAlpha(img, cx, cy, r) {
    let m = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            m = Math.max(m, img.data[(y * img.width + x) * 4 + 3]);
        }
    }
    return m;
}

function cmpMat(actual, expected, tol, label) {
    assert(actual !== null && actual !== undefined && actual.length === 16,
        `${label}: got a 16-float matrix`);
    for (let i = 0; i < 16; i++) {
        assert(Math.abs(actual[i] - expected[i]) < tol,
            `${label}: [${i}] ${actual[i]} vs ${expected[i]}`);
    }
}

// T(0,1,0) * R(quat xyzw) — column-major, matching computeWorldMatrices.
function hingeWorld(q) {
    const [x, y, z, w] = q;
    // Rotation matrix columns from the quaternion.
    return [
        1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),     0,
        2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),     0,
        2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y), 0,
        0,                       1,                       0,                       1,
    ];
}

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU) — skipping skeletal animation test');
} else {
    const FOV = 40, EYE = [0, 1, 8];
    scene.setCamera({ fov: FOV, near: 0.1, far: 100, position: EYE, target: [0, 1, 0] });
    function project(x, y, z) {
        const d = EYE[2] - z;
        const k = 128 / (d * Math.tan((FOV / 2) * Math.PI / 180));
        return [Math.round(128 + x * k), Math.round(128 - (y - 1) * k)];
    }
    scene.createLight({ type: 'directional', direction: [0, 0, -1],
                        color: [1, 1, 1], intensity: 2.0 });

    // ------------------------------------------------------------------
    // 2-bone strip + skeleton + clips
    // ------------------------------------------------------------------
    const ROWS = 9;
    const positions = new Float32Array(ROWS * 2 * 3);
    const normals   = new Float32Array(ROWS * 2 * 3);
    const boneW = new Float32Array(ROWS * 2 * 4);
    const boneI = new Uint32Array(ROWS * 2 * 4);
    for (let r = 0; r < ROWS; r++) {
        const y = r * 0.25;
        for (let c = 0; c < 2; c++) {
            const v = r * 2 + c;
            positions[v * 3 + 0] = c === 0 ? -0.2 : 0.2;
            positions[v * 3 + 1] = y;
            normals[v * 3 + 2] = 1;
            boneW[v * 4 + 0] = 1;
            boneI[v * 4 + 0] = y > 1.0 ? 1 : 0;
        }
    }
    const indices = new Uint32Array((ROWS - 1) * 6);
    for (let r = 0; r < ROWS - 1; r++) {
        const bl = r * 2, br = r * 2 + 1, tl = (r + 1) * 2, tr = (r + 1) * 2 + 1;
        indices.set([bl, br, tr, bl, tr, tl], r * 6);
    }

    const skin = new SkinData({
        boneWeights: boneW,
        boneIndices: boneI,
        inverseBindMatrices: new Float32Array([
            1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
            1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
        ]),
        boneCount: 2,
    });
    const skel = new Skeleton({ bones: [
        { name: 'root',  parent: -1 },
        { name: 'upper', parent: 0, localT: [0, 1, 0],
          inverseBind: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-1,0,1] },
    ]});

    const s45 = Math.SQRT1_2;
    // bone-1 rotation ramps identity → 90 deg over 1 s (linear key interp)
    const bendClip = new Animation({
        name: 'bend', duration: 1,
        channels: [{ boneIndex: 1, path: 'rotation',
                     times: new Float32Array([0, 1]),
                     values: new Float32Array([0,0,0,1,  0,0,s45,s45]) }], // about Z
    });
    const twistClip = new Animation({
        name: 'twist', duration: 1,
        channels: [{ boneIndex: 1, path: 'rotation',
                     times: new Float32Array([0, 1]),
                     values: new Float32Array([0,0,0,1,  0,s45,0,s45]) }], // about Y
    });

    const node = scene.createSkinnedMesh({
        positions, normals, indices, color: 'red', roughness: 0.9, skin,
    });

    // ------------------------------------------------------------------
    // Error paths before setup
    // ------------------------------------------------------------------
    let threw = false;
    try { node.play('bend'); } catch (e) { threw = true; }
    assert(threw, 'play before setSkeleton/addClip throws');

    assert(node.setSkeleton(skel) === node, 'setSkeleton chains');
    assert(node.addClip('bend', bendClip) === node, 'addClip chains');
    node.addClip('twist', twistClip);

    threw = false;
    try { node.play('nope'); } catch (e) { threw = true; }
    assert(threw, 'play with unknown clip throws');

    // ------------------------------------------------------------------
    // Bind pose seam (skeleton set, nothing played yet)
    // ------------------------------------------------------------------
    cmpMat(node.getBoneWorldMatrix(1), hingeWorld([0, 0, 0, 1]), 1e-5,
        'bind pose bone 1');
    cmpMat(node.getBoneWorldMatrix('upper'), hingeWorld([0, 0, 0, 1]), 1e-5,
        'bone lookup by name');
    assert(node.getBoneWorldMatrix('nope') === null, 'unknown bone name -> null');
    assert(node.getBoneWorldMatrix(7) === null, 'out-of-range index -> null');
    assert(node.isPlaying === false, 'not playing initially');

    const imgBind = scene.captureFrame();

    // ------------------------------------------------------------------
    // One-shot play to the end: analytic 90-degree hinge + finished(once)
    // ------------------------------------------------------------------
    let finishes = [];
    node.onAnimationFinished = (name) => finishes.push(name);

    node.play('bend', { loop: false });
    assert(node.isPlaying === true, 'isPlaying true after play');
    assert(node.currentAnimation === 'bend', 'currentAnimation is bend');
    assert(Math.abs(node.animationDuration - 1) < 1e-6, 'animationDuration 1s');

    advanceTime(1050); // past the 1 s duration so the one-shot definitely ends
    assert(Math.abs(node.animationTime - 1) < 1e-6,
        `one-shot clamps at duration (${node.animationTime})`);
    cmpMat(node.getBoneWorldMatrix(1), hingeWorld([0, 0, s45, s45]), 1e-3,
        'bone 1 at t=1 (analytic Rz90 hinge)');
    assert(node.isPlaying === false, 'one-shot finished -> isPlaying false');
    assert(finishes.length === 1 && finishes[0] === 'bend',
        `finished fired exactly once with the clip name (${JSON.stringify(finishes)})`);
    advanceTime(500);
    assert(finishes.length === 1, 'finished does not re-fire while holding');

    // Pixels actually moved: strip top vacated, horizontal arm appeared.
    const imgBent = scene.captureFrame();
    const [txs, tys] = project(0, 1.75, 0);
    const [axs, ays] = project(-0.6, 1, 0);
    assert(patchMaxAlpha(imgBind, txs, tys, 3) > 128, 'bind: strip top visible');
    assert(patchMaxAlpha(imgBind, axs, ays, 3) === 0, 'bind: arm region empty');
    assert(patchMaxAlpha(imgBent, txs, tys, 3) === 0, 'bent: strip top vacated');
    assert(patchMaxAlpha(imgBent, axs, ays, 3) > 128, 'bent: arm visible');

    // ------------------------------------------------------------------
    // Mid-clip pose matches the rigging pipeline's own evaluate
    // ------------------------------------------------------------------
    node.play('bend', { loop: false });
    advanceTime(500);
    const tMid = node.animationTime;
    assert(Math.abs(tMid - 0.5) < 2e-3, `midpoint clock (${tMid})`);
    {
        const gt = bendClip.evaluate(skel, tMid, { loop: false })
                           .computeWorldMatrices(skel);
        cmpMat(node.getBoneWorldMatrix(1), Array.from(gt.slice(16, 32)), 1e-4,
            'bone 1 at t=0.5 matches Animation.evaluate ground truth');
    }

    // ------------------------------------------------------------------
    // Loop wrap
    // ------------------------------------------------------------------
    node.play('bend'); // loop defaults true
    advanceTime(1500);
    assert(Math.abs(node.animationTime - 0.5) < 2e-3,
        `looping clip wraps (${node.animationTime})`);
    assert(node.isPlaying === true, 'looping clip keeps playing');
    assert(finishes.length === 1, 'looping clip never fires finished');

    // ------------------------------------------------------------------
    // Crossfade at 50%: matches Pose.blend ground truth
    // ------------------------------------------------------------------
    node.play('bend', { loop: false });
    advanceTime(250);                          // bend at 0.25
    node.play('twist', { fadeTime: 0.5 });     // crossfade over 0.5 s
    assert(node.currentAnimation === 'twist', 'crossfade target is current');
    advanceTime(250);                          // bend 0.5, twist 0.25, alpha 0.5
    {
        const from = bendClip.evaluate(skel, 0.5, { loop: false });
        const to   = twistClip.evaluate(skel, 0.25, { loop: true });
        Pose.blend(from, to, 0.5);             // blends `to` into `from`
        const gt = from.computeWorldMatrices(skel);
        cmpMat(node.getBoneWorldMatrix(1), Array.from(gt.slice(16, 32)), 1e-3,
            'crossfade at 50% matches Pose.blend ground truth');
    }
    advanceTime(400);                          // fade completes (0.65 > 0.5)
    {
        const gt = twistClip.evaluate(skel, node.animationTime, { loop: true })
                            .computeWorldMatrices(skel);
        cmpMat(node.getBoneWorldMatrix(1), Array.from(gt.slice(16, 32)), 1e-3,
            'after the fade only the new clip drives the pose');
    }

    // ------------------------------------------------------------------
    // Masked layer: twist layered over a held bend, bone 1 only
    // ------------------------------------------------------------------
    node.play('bend', { loop: false });
    advanceTime(1050);                         // base finishes, holds Rz90
    node.play('twist', { mask: new Uint8Array([0, 1]) });
    advanceTime(250);                          // layer twist at 0.25, weight 1
    {
        const gt = twistClip.evaluate(skel, 0.25, { loop: true })
                            .computeWorldMatrices(skel);
        cmpMat(node.getBoneWorldMatrix(1), Array.from(gt.slice(16, 32)), 1e-3,
            'masked layer fully overrides bone 1');
        // Root is unmasked and unanimated: still identity.
        cmpMat(node.getBoneWorldMatrix(0),
            [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1], 1e-4,
            'unmasked root bone untouched by the layer');
    }
    assert(node.currentAnimation === 'bend',
        'layer play does not replace the base track');
    node.stop(); // clear base + looping layer before the next section

    // ------------------------------------------------------------------
    // Pause, scrub, resume, speed
    // ------------------------------------------------------------------
    node.play('bend', { loop: false });
    advanceTime(300);
    node.pause();
    const tPaused = node.animationTime;
    advanceTime(500);
    assert(node.animationTime === tPaused, 'paused clock frozen');
    assert(node.isPlaying === false, 'paused -> isPlaying false');

    node.animationTime = 0.75;                 // scrub while paused
    assert(Math.abs(node.animationTime - 0.75) < 1e-6, 'scrub sets the clock');
    {
        const gt = bendClip.evaluate(skel, 0.75, { loop: false })
                           .computeWorldMatrices(skel);
        cmpMat(node.getBoneWorldMatrix(1), Array.from(gt.slice(16, 32)), 1e-4,
            'scrub re-poses immediately while paused');
    }

    node.resume();
    assert(node.isPlaying === true, 'resume restarts the clock');
    node.animationSpeed = 0.5;
    assert(Math.abs(node.animationSpeed - 0.5) < 1e-6, 'animationSpeed get/set');
    advanceTime(200);                          // +0.1 s at half speed
    assert(Math.abs(node.animationTime - 0.85) < 2e-3,
        `speed scales the clock (${node.animationTime})`);

    // ------------------------------------------------------------------
    // stop(): back to bind pose, manual palette control returns
    // ------------------------------------------------------------------
    node.stop();
    assert(node.isPlaying === false, 'stopped');
    cmpMat(node.getBoneWorldMatrix(1), hingeWorld([0, 0, 0, 1]), 1e-4,
        'stop() returns to bind pose');
    // Manual palette drives again (player inactive): bend the arm by hand.
    const pose = bendClip.evaluate(skel, 1, { loop: false });
    node.setSkinningMatrices(pose.computeSkinningMatrices(skel));
    advanceTime(100);                          // ticks must not overwrite it
    const imgManual = scene.captureFrame();
    assert(patchMaxAlpha(imgManual, axs, ays, 3) > 128,
        'manual setSkinningMatrices wins while the player is inactive');

    // stop with fade: halfway through the fade the pose is blend(current, bind)
    node.play('bend', { loop: false });
    advanceTime(1050);                         // finish, hold Rz90
    node.stop({ fadeTime: 0.5 });
    advanceTime(250);                          // 50% back toward bind
    {
        const cur = bendClip.evaluate(skel, 1, { loop: false });
        Pose.blend(cur, skel.bindPose(), 0.5);
        const gt = cur.computeWorldMatrices(skel);
        cmpMat(node.getBoneWorldMatrix(1), Array.from(gt.slice(16, 32)), 1e-3,
            'stop fade blends toward bind pose');
    }
    advanceTime(300);                          // fade done -> deactivated
    assert(node.isPlaying === false, 'stop fade completes');
    cmpMat(node.getBoneWorldMatrix(1), hingeWorld([0, 0, 0, 1]), 1e-3,
        'stop fade lands exactly on bind pose');

    console.log('skeletal animation player tests passed');
}
