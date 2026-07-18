// Root motion extraction on SkinnedMeshNode — setRootMotion /
// consumeRootMotion: per-tick root deltas (translation + yaw) accumulated in
// model space and removed from the pose, continuous across loop wraps and
// crossfades. Exercises the extraction hook in
// src/scene/animation_player.cpp and the bindings in
// src/js/scene_bindings_anim.cpp. All time advance is advanceTime() virtual
// time; every expectation is compensated with the player's own clock
// (animationTime / blendState().phase) so clock granularity never matters.
//
// Rig: 2-bone hinge; the ROOT bone carries the motion channels.
//   walk: root T (0,0,0) -> (0,0.3,1) -> (0,0,2) over 1 s  (z = 2t, Y bob)
//   run:  root T (0,0,0) -> (0,0,4) over 1 s               (z = 4t)
//   turn: root R identity -> 90 deg about +Y over 1 s

function near(a, b, tol, label) {
    assert(Math.abs(a - b) < tol, `${label}: ${a} vs ${b}`);
}

// Yaw about +Y of quaternion [x,y,z,w] (same formula as the C++ extractor).
function yawOf(q) {
    return Math.atan2(2 * (q[0] * q[2] + q[3] * q[1]),
                      1 - 2 * (q[0] * q[0] + q[1] * q[1]));
}

const walkZ = (t) => 2 * t;
const walkY = (t) => 0.6 * Math.min(t, 1 - t);

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU) — skipping root motion test');
} else {
    const positions = new Float32Array([-0.2, 0, 0,  0.2, 0, 0,
                                        -0.2, 2, 0,  0.2, 2, 0]);
    const normals = new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]);
    const indices = new Uint32Array([0, 1, 3, 0, 3, 2]);
    const skin = new SkinData({
        boneWeights: new Float32Array([1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0]),
        boneIndices: new Uint32Array([0,0,0,0, 0,0,0,0, 1,0,0,0, 1,0,0,0]),
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
    const walkClip = new Animation({
        name: 'walk', duration: 1,
        channels: [{ boneIndex: 0, path: 'translation',
                     times: new Float32Array([0, 0.5, 1]),
                     values: new Float32Array([0,0,0, 0,0.3,1, 0,0,2]) }],
    });
    const runClip = new Animation({
        name: 'run', duration: 1,
        channels: [{ boneIndex: 0, path: 'translation',
                     times: new Float32Array([0, 1]),
                     values: new Float32Array([0,0,0, 0,0,4]) }],
    });
    const turnClip = new Animation({
        name: 'turn', duration: 1,
        channels: [{ boneIndex: 0, path: 'rotation',
                     times: new Float32Array([0, 1]),
                     values: new Float32Array([0,0,0,1, 0,s45,0,s45]) }],
    });

    const node = scene.createSkinnedMesh({ positions, normals, indices,
                                           color: 'red', skin });

    // ------------------------------------------------------------------
    // Errors: no skeleton / unknown bone
    // ------------------------------------------------------------------
    let threw = false;
    try { node.setRootMotion({ enabled: true }); } catch (e) { threw = true; }
    assert(threw, 'setRootMotion before setSkeleton throws');

    node.setSkeleton(skel);
    node.addClip('walk', walkClip).addClip('run', runClip)
        .addClip('turn', turnClip);

    threw = false;
    try { node.setRootMotion({ enabled: true, bone: 'ghost' }); }
    catch (e) { threw = true; }
    assert(threw, 'unknown bone name throws');
    threw = false;
    try { node.setRootMotion({ enabled: true, bone: 99 }); }
    catch (e) { threw = true; }
    assert(threw, 'out-of-range bone index throws');

    // Named bone works; auto-detect (used below) resolves the same root.
    assert(node.setRootMotion({ enabled: true, bone: 'root' }) === node,
        'setRootMotion chains');
    node.setRootMotion({ enabled: false });

    // ------------------------------------------------------------------
    // Enabling mid-clip never spikes (baseline rebases to the live pose)
    // ------------------------------------------------------------------
    node.play('walk');                       // root motion still disabled
    advanceTime(700);                        // root z ~1.4 by now
    node.setRootMotion({ enabled: true });   // auto-detect: parentless bone 0
    {
        const d = node.consumeRootMotion();
        assert(d.translation[0] === 0 && d.translation[1] === 0 &&
               d.translation[2] === 0 && d.yaw === 0,
            'freshly enabled: accumulator is zero');
    }
    const tEnable = node.animationTime;
    advanceTime(50);
    {
        const d = node.consumeRootMotion();
        near(d.translation[2], walkZ(node.animationTime) - walkZ(tEnable), 1e-3,
            'first tick after mid-clip enable measures one tick, not a spike');
        assert(Math.abs(d.translation[2]) < 0.3, 'no enable spike');
    }

    // ------------------------------------------------------------------
    // Steady walk: delta matches the authored track; pose root is pinned
    // ------------------------------------------------------------------
    node.play('walk');                       // restart at t=0
    node.setRootMotion({ enabled: true });   // re-pin at the t=0 root (origin)
    {
        const d = node.consumeRootMotion();
        assert(d.translation[2] === 0, 'play + enable: zero before any tick');
    }
    advanceTime(400);
    const t1 = node.animationTime;
    {
        const d = node.consumeRootMotion();
        near(d.translation[2], walkZ(t1), 5e-3, 'z delta matches authored track');
        assert(d.translation[0] === 0, 'x never moves');
        assert(d.translation[1] === 0, 'Y not extracted by default');
        near(d.yaw, 0, 1e-6, 'no yaw in a translation clip');

        const m = node.getBoneWorldMatrix(0);
        near(m[12], 0, 1e-4, 'pose root X pinned at origin');
        near(m[14], 0, 1e-4, 'pose root Z pinned at origin');
        near(m[13], walkY(t1), 1e-3, 'pose root Y stays authored (bob renders)');
    }

    // ------------------------------------------------------------------
    // Loop wrap: the wrap tick contributes the clip's net displacement
    // ------------------------------------------------------------------
    advanceTime(700);                        // crosses the 1.0 s wrap
    {
        const t2 = node.animationTime;
        assert(t2 < t1, 'clock wrapped');
        const d = node.consumeRootMotion();
        // z = 2t is linear, so across one wrap the exact sum is
        // [z(1) - z(t1)] + [z(t2) - z(0)] = 2 + 2*t2 - 2*t1.
        near(d.translation[2], 2 + walkZ(t2) - walkZ(t1), 5e-3,
            'wrap tick sums end-segment + net + start-segment');
        near(node.getBoneWorldMatrix(0)[14], 0, 1e-4,
            'pose root still pinned after the wrap');
    }

    // Consume resets on read.
    {
        const d = node.consumeRootMotion();
        assert(d.translation[0] === 0 && d.translation[1] === 0 &&
               d.translation[2] === 0 && d.yaw === 0, 'consume resets');
    }

    // ------------------------------------------------------------------
    // Pause yields zero
    // ------------------------------------------------------------------
    node.pause();
    advanceTime(300);
    {
        const d = node.consumeRootMotion();
        assert(d.translation[2] === 0 && d.yaw === 0, 'paused: zero delta');
    }
    node.resume();

    // ------------------------------------------------------------------
    // extractY: Y is accumulated and pinned instead of staying authored
    // ------------------------------------------------------------------
    const tY0 = node.animationTime;
    node.setRootMotion({ enabled: true, extractY: true });  // re-pin here
    node.consumeRootMotion();
    advanceTime(200);
    {
        const tY1 = node.animationTime;
        const d = node.consumeRootMotion();
        near(d.translation[1], walkY(tY1) - walkY(tY0), 1e-3,
            'extractY accumulates the authored Y delta');
        near(node.getBoneWorldMatrix(0)[13], walkY(tY0), 1e-3,
            'extractY pins pose Y at the enable-time value');
    }

    // ------------------------------------------------------------------
    // Yaw: a turning clip accumulates yaw; the pose rotation is unwound
    // ------------------------------------------------------------------
    node.play('turn');
    node.setRootMotion({ enabled: true });
    node.consumeRootMotion();
    const turnYawAt = (t) => yawOf(Array.from(
        turnClip.evaluate(skel, t, { loop: true }).data.slice(3, 7)));
    advanceTime(500);
    const tTurn1 = node.animationTime;
    {
        // Expected from the SAME rigging pipeline (root quat at time t).
        const d = node.consumeRootMotion();
        near(d.yaw, turnYawAt(tTurn1), 1e-3, 'yaw delta matches the authored rotation');
        near(d.translation[2], 0, 1e-6, 'pure turn: no translation');
        const m = node.getBoneWorldMatrix(0);
        near(m[0], 1, 1e-3, 'pose root yaw unwound (m00)');
        near(m[10], 1, 1e-3, 'pose root yaw unwound (m22)');
    }
    advanceTime(700);                        // across the turn clip's wrap
    {
        const tTurn2 = node.animationTime;
        assert(tTurn2 < tTurn1, 'turn clip wrapped');
        const d = node.consumeRootMotion();
        // Net loop yaw is pi/2: [pi/2 - yaw(t1)] + yaw(t2).
        near(d.yaw, Math.PI / 2 - turnYawAt(tTurn1) + turnYawAt(tTurn2), 2e-3,
            'wrap tick sums the net loop yaw');
    }

    // ------------------------------------------------------------------
    // Crossfade mid-walk: deltas stay finite and continuous through both
    // the fade and the walk clip's wrap inside it (no wrap-spike)
    // ------------------------------------------------------------------
    node.play('walk');
    node.setRootMotion({ enabled: true });
    advanceTime(800);
    node.consumeRootMotion();
    node.play('run', { fadeTime: 0.4 });     // walk wraps ~200 ms into the fade
    // Deltas are extracted from the BLENDED pose: while run (root near z=0)
    // fades over walk (root near z=1.6) an alignment component appears, so
    // deltas may dip slightly negative — the guarantee is continuity and
    // boundedness. An uncorrected walk wrap inside the fade would show as a
    // single ~1.8-unit outlier; assert none appears.
    let prevDz = null;
    for (let i = 0; i < 12; i++) {
        advanceTime(50);
        const d = node.consumeRootMotion();
        assert(Number.isFinite(d.translation[2]) && Number.isFinite(d.yaw),
            `fade step ${i}: finite`);
        assert(Math.abs(d.translation[2]) < 0.5,
            `fade step ${i}: no wrap-spike (${d.translation[2]})`);
        if (prevDz !== null)
            assert(Math.abs(d.translation[2] - prevDz) < 0.3,
                `fade step ${i}: continuous (${prevDz} -> ${d.translation[2]})`);
        prevDz = d.translation[2];
    }

    // ------------------------------------------------------------------
    // Blend space drives playback: weighted rate + weighted net on wrap
    // ------------------------------------------------------------------
    node.addBlendSpace1D('rmSpace', [
        { clip: 'walk', pos: 0 },
        { clip: 'run',  pos: 1 },
    ]);
    node.play('rmSpace');
    node.setBlendPos('rmSpace', 0.5);        // z rate = (2+4)/2 = 3 u/s
    node.setRootMotion({ enabled: true });
    node.consumeRootMotion();
    advanceTime(400);
    const p1 = node.blendState().phase;
    {
        const d = node.consumeRootMotion();
        near(d.translation[2], 3 * p1, 5e-3, 'blend-space delta is the weighted rate');
    }
    advanceTime(700);                        // crosses the shared-phase wrap
    {
        const p2 = node.blendState().phase;
        assert(p2 < p1, 'shared phase wrapped');
        const d = node.consumeRootMotion();
        near(d.translation[2], 3 * (1 + p2 - p1), 5e-3,
            'space wrap adds the weight-blended net displacement');
    }

    // ------------------------------------------------------------------
    // State machine drives playback
    // ------------------------------------------------------------------
    node.addStateMachine({
        states: [{ name: 'go', source: 'walk' }],
        transitions: [],
    });
    node.setRootMotion({ enabled: true });
    node.consumeRootMotion();
    advanceTime(300);
    {
        const d = node.consumeRootMotion();
        near(d.translation[2], walkZ(node.animationTime), 5e-3,
            'machine-driven playback extracts root motion too');
        assert(node.state === 'go', 'machine still active');
    }

    // ------------------------------------------------------------------
    // Disable: nothing accumulates, pose root moves again
    // ------------------------------------------------------------------
    node.setRootMotion({ enabled: false });
    advanceTime(300);
    {
        const d = node.consumeRootMotion();
        assert(d.translation[2] === 0, 'disabled: no accumulation');
        near(node.getBoneWorldMatrix(0)[14], walkZ(node.animationTime), 2e-3,
            'disabled: pose root no longer pinned');
    }

    console.log('root motion tests passed');
}
